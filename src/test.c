// test.c — dev-only tests (--test). Headless: buffers and files only, no window, no GPU.
// Results go to build\teal.log; test_run returns the number of failures.

#if TEAL_DEV

#define TEST_FUZZ_OPS 100000
#define TEST_FUZZ_CHECK_EVERY 50
#define TEST_REF_CAP MB(4)

typedef struct Test {
    Arena arena; // test data; reset between tests
    u64 rng;
    i32 failures;
    String8 tmp_dir; // build\tmp: round-trip inputs stay there for inspection
} Test;

static u64 test_rand(Test *t) { // splitmix64
    u64 z = (t->rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static i64 test_below(Test *t, i64 n) { // uniform in [0, n), 0 when n <= 0
    return n > 0 ? (i64)(test_rand(t) % (u64)n) : 0;
}

// NUL-terminated formatting into a fixed buffer, for the labels passed to the checks.
static void test_cstr(char *out, i64 cap, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    i64 n = fmt_v((u8 *)out, cap - 1, fmt, args);
    va_end(args);
    out[MIN(n, cap - 1)] = 0;
}

#define TEST_CHECK(t, cond, ...) \
    do { if (!(cond)) { LOG("test: FAIL: " __VA_ARGS__); (t)->failures++; return 0; } } while (0)

// ---------------------------------------------------------------------------
// The naive reference: a flat array.

typedef struct TestRef {
    u8 *data;
    i64 len;
    i64 cap;
    i64 newlines; // maintained by counting, never by the buffer's logic
} TestRef;

static b32 test_equal(u8 *a, u8 *b, i64 n) {
    for (i64 i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static i64 test_count_newlines(u8 *s, i64 n) {
    i64 count = 0;
    for (i64 i = 0; i < n; i++) count += s[i] == '\n';
    return count;
}

static void test_ref_replace(TestRef *ref, i64 start, i64 end, String8 text) {
    ref->newlines -= test_count_newlines(ref->data + start, end - start);
    ref->newlines += test_count_newlines(text.data, text.len);
    memmove(ref->data + start + text.len, ref->data + end, (size_t)(ref->len - end));
    memcpy(ref->data + start, text.data, (size_t)text.len);
    ref->len += text.len - (end - start);
}

// Character boundaries by forward decoding from the start of the line (a line start always is one).
static i64 test_ref_line_start(TestRef *ref, i64 pos) {
    while (pos > 0 && ref->data[pos - 1] != '\n') pos--;
    return pos;
}

static i64 test_ref_snap(TestRef *ref, i64 pos) { // the boundary at or before pos
    i64 b = test_ref_line_start(ref, pos);
    for (;;) {
        i64 advance;
        utf8_decode(ref->data + b, ref->len - b, &advance);
        if (b + advance > pos) return b;
        b += advance;
    }
}

static i64 test_ref_next(TestRef *ref, i64 pos) {
    if (pos >= ref->len) return ref->len;
    i64 advance;
    utf8_decode(ref->data + pos, ref->len - pos, &advance);
    return pos + advance;
}

static i64 test_ref_prev(TestRef *ref, i64 pos) {
    if (pos <= 0) return 0;
    return test_ref_snap(ref, pos - 1);
}

// ---------------------------------------------------------------------------
// Comparisons

static b32 test_compare_full(Test *t, Buffer *buf, TestRef *ref, const char *when) {
    u64 mark = arena_pos(&t->arena);
    i64 size = buffer_size(buf);
    TEST_CHECK(t, size == ref->len, "%s: size %D, expected %D", when, size, ref->len);

    u8 *copy = PUSH_ARRAY(&t->arena, u8, size);
    buffer_copy(buf, 0, size, copy);
    for (i64 i = 0; i < size; i++) {
        TEST_CHECK(t, copy[i] == ref->data[i], "%s: byte %D is 0x%x, expected 0x%x", when, i, (u32)copy[i], (u32)ref->data[i]);
    }

    String8 before, after;
    buffer_segments(buf, &before, &after);
    TEST_CHECK(t, before.len + after.len == size, "%s: segments %D + %D != size %D", when, before.len, after.len, size);
    TEST_CHECK(t, test_equal(before.data, ref->data, before.len), "%s: segment before the gap differs", when);
    TEST_CHECK(t, test_equal(after.data, ref->data + before.len, after.len), "%s: segment after the gap differs", when);

    // Line starts by full scan.
    i64 *starts = PUSH_ARRAY(&t->arena, i64, ref->len + 2);
    i64 lines = 0;
    starts[lines++] = 0;
    for (i64 i = 0; i < ref->len; i++) if (ref->data[i] == '\n') starts[lines++] = i + 1;
    TEST_CHECK(t, buffer_line_count(buf) == lines, "%s: line count %D, expected %D", when, buffer_line_count(buf), lines);
    for (i64 l = 0; l < lines; l++) {
        i64 s = buffer_line_start(buf, l), e = buffer_line_end(buf, l);
        i64 want_e = l + 1 < lines ? starts[l + 1] - 1 : ref->len;
        TEST_CHECK(t, s == starts[l], "%s: line %D starts at %D, expected %D", when, l, s, starts[l]);
        TEST_CHECK(t, e == want_e, "%s: line %D ends at %D, expected %D", when, l, e, want_e);
    }
    // line_of at both ends of every line would dominate the run time in the /Od build: sample them.
    // Plus the lines around the gap, where the index changes.
    i64 gap_line = 0;
    while (gap_line + 1 < lines && starts[gap_line + 1] <= buf->gap_start) gap_line++;
    for (i32 i = 0; i < 505; i++) {
        i64 l = i < 500 ? test_below(t, lines) : CLAMP(gap_line + i - 502, 0, lines - 1);
        i64 s = starts[l], want_e = l + 1 < lines ? starts[l + 1] - 1 : ref->len;
        TEST_CHECK(t, buffer_line_of(buf, s) == l, "%s: line_of(%D) = %D, expected %D", when, s, buffer_line_of(buf, s), l);
        TEST_CHECK(t, buffer_line_of(buf, want_e) == l, "%s: line_of(%D) = %D, expected %D", when, want_e, buffer_line_of(buf, want_e), l);
        if (s > 0) TEST_CHECK(t, buffer_line_of(buf, s - 1) == l - 1, "%s: line_of(%D) = %D, expected %D", when, s - 1, buffer_line_of(buf, s - 1), l - 1);
    }
    for (i32 i = 0; i < 100; i++) {
        i64 off = test_below(t, ref->len + 1);
        i64 lo = 0, hi = lines; // last start <= off
        while (hi - lo > 1) {
            i64 mid = (lo + hi) / 2;
            if (starts[mid] <= off) lo = mid;
            else hi = mid;
        }
        TEST_CHECK(t, buffer_line_of(buf, off) == lo, "%s: line_of(%D) = %D, expected %D", when, off, buffer_line_of(buf, off), lo);
    }

    // Lines as strings: random ones, plus the line holding the gap (the one most likely to straddle it).
    for (i32 i = 0; i < 21; i++) {
        i64 l = i < 20 ? test_below(t, lines) : buffer_line_of(buf, buf->gap_start);
        String8 s = buffer_line(buf, &t->arena, l);
        i64 want_e = l + 1 < lines ? starts[l + 1] - 1 : ref->len;
        TEST_CHECK(t, s.len == want_e - starts[l], "%s: line %D has length %D, expected %D", when, l, s.len, want_e - starts[l]);
        TEST_CHECK(t, test_equal(s.data, ref->data + starts[l], s.len), "%s: line %D text differs", when, l);
        b32 straddles = starts[l] < buf->gap_start && want_e > buf->gap_start;
        b32 inside = s.data >= buf->text && s.data < buf->text + buf->text_cap;
        TEST_CHECK(t, s.len == 0 || inside == !straddles, "%s: line %D: points into the buffer = %d, straddles the gap = %d",
                   when, l, inside, straddles);
    }

    // Character stepping at random boundaries.
    for (i32 i = 0; i < 100; i++) {
        i64 b = test_ref_snap(ref, test_below(t, ref->len + 1));
        TEST_CHECK(t, buffer_next_char(buf, b) == test_ref_next(ref, b), "%s: next_char(%D) = %D, expected %D", when, b,
                   buffer_next_char(buf, b), test_ref_next(ref, b));
        TEST_CHECK(t, buffer_prev_char(buf, b) == test_ref_prev(ref, b), "%s: prev_char(%D) = %D, expected %D", when, b,
                   buffer_prev_char(buf, b), test_ref_prev(ref, b));
    }
    arena_pop_to(&t->arena, mark);
    return 1;
}

// Cheap checks after every operation.
static b32 test_compare_quick(Test *t, Buffer *buf, TestRef *ref, const char *when) {
    TEST_CHECK(t, buffer_size(buf) == ref->len, "%s: size %D, expected %D", when, buffer_size(buf), ref->len);
    TEST_CHECK(t, buffer_line_count(buf) == ref->newlines + 1, "%s: line count %D, expected %D", when,
               buffer_line_count(buf), ref->newlines + 1);
    if (ref->len > 0) {
        i64 off = test_below(t, ref->len);
        TEST_CHECK(t, buffer_byte(buf, off) == ref->data[off], "%s: byte %D differs", when, off);
        i64 l = buffer_line_of(buf, off);
        TEST_CHECK(t, buffer_line_start(buf, l) <= off && off <= buffer_line_end(buf, l), "%s: line_of(%D) = %D spans %D..%D",
                   when, off, l, buffer_line_start(buf, l), buffer_line_end(buf, l));
    }
    return 1;
}

// ---------------------------------------------------------------------------
// Differential fuzz

static String8 test_random_text(Test *t, u8 *out, i64 len) {
    static const char *pieces[] = {
        "a", "b", "x", "Z", "0", " ", " ", "\t", "\n", "\n", "\r", "\r\n", "\0",
        "\xC4\x9F",         // ğ
        "\xE2\x82\xAC",     // €
        "\xF0\x9F\x98\x80", // 😀
        "\x80", "\xFF", "\xC3", "\xE2\x82", // invalid: stray continuation, never valid, truncated sequences
    };
    static const u8 piece_len[] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 2, 3, 4, 1, 1, 1, 2 };
    i64 n = 0;
    while (n < len) {
        i64 k = test_below(t, ARRAY_COUNT(pieces));
        i64 m = MIN((i64)piece_len[k], len - n);
        memcpy(out + n, pieces[k], (size_t)m);
        n += m;
    }
    return str8(out, n);
}

static b32 test_fuzz(Test *t, u64 seed) {
    t->rng = seed;
    Buffer *buf = buffer_create(STR8_LIT("fuzz"));
    TEST_CHECK(t, buf, "fuzz: buffer_create failed");
    TestRef ref = { PUSH_ARRAY(&t->arena, u8, TEST_REF_CAP), 0, TEST_REF_CAP, 0 };
    u8 *text = PUSH_ARRAY(&t->arena, u8, KB(256));
    i64 cursor = 0; // edits cluster around it, like typing
    b32 ok = 1;
    i64 gap_moves = 0, grows = 0;

    for (i32 op = 0; op < TEST_FUZZ_OPS && ok; op++) {
        i64 len = ref.len;
        i64 r = test_below(t, 100);
        i64 start, end;
        String8 s = str8(text, 0);
        // Positions: half near the last edit, half anywhere.
        i64 near = cursor + test_below(t, 65) - 32; // CLAMP evaluates its argument more than once
        i64 at = test_below(t, 2) ? CLAMP(near, 0, len) : test_below(t, len + 1);
        if (r < 30) {                  // small insert
            start = end = at;
            s = test_random_text(t, text, 1 + test_below(t, 16));
        } else if (r < 50) {           // small replace
            start = at;
            end = at + test_below(t, 33);
            s = test_random_text(t, text, test_below(t, 17));
        } else if (r < 65) {           // small delete
            start = at;
            end = at + 1 + test_below(t, 64);
        } else if (r < 70) {           // at the boundaries
            b32 front = (b32)test_below(t, 2);
            start = front ? 0 : len;
            end = front ? test_below(t, 8) : len;
            s = test_random_text(t, text, test_below(t, 8));
        } else if (r < 75) {           // a run of newlines
            start = end = at;
            i64 n = 1 + test_below(t, 20);
            memset(text, '\n', (size_t)n);
            s = str8(text, n);
        } else if (r < 78) {           // no-op
            start = end = at;
        } else if (r < 80) {           // large insert, unless the text is already large
            start = end = at;
            if (len < (i64)KB(256)) s = test_random_text(t, text, KB(16) + test_below(t, KB(240)));
        } else if (r < 82 || len > (i64)KB(384)) { // large delete
            start = at;
            end = at + test_below(t, len / 2 + 1);
        } else {                       // replace with a run of mixed text and newlines
            start = at;
            end = at + test_below(t, 256);
            s = test_random_text(t, text, test_below(t, 512));
        }
        end = MIN(end, len); // clamped here: MIN would evaluate a test_below argument twice
        start = test_ref_snap(&ref, start);
        end = test_ref_snap(&ref, MAX(start, end));
        ASSERT(ref.len - (end - start) + s.len <= ref.cap);

        i64 gap_before = buf->gap_start;
        i64 cap_before = buf->text_cap;
        u64 edits_before = buf->edit_count;
        b32 noop = start == end && s.len == 0;
        if (!buffer_replace(buf, start, end, s)) {
            LOG("test: FAIL: fuzz op %d (seed 0x%X): buffer_replace(%D, %D, %D bytes) refused", op, seed, start, end, s.len);
            t->failures++;
            ok = 0;
            break;
        }
        test_ref_replace(&ref, start, end, s);
        gap_moves += !noop && (gap_before < start || gap_before > end);
        grows += buf->text_cap != cap_before;
        cursor = start + s.len;

        if (buf->edit_count != edits_before + (noop ? 0 : 1)) {
            LOG("test: FAIL: fuzz op %d (seed 0x%X): edit_count %U after %U (no-op %d)", op, seed, buf->edit_count, edits_before, noop);
            t->failures++;
            ok = 0;
            break;
        }
        char when[96];
        test_cstr(when, sizeof(when), "fuzz op %d (seed 0x%X)", op, seed);
        ok = test_compare_quick(t, buf, &ref, when);
        if (ok && (op % TEST_FUZZ_CHECK_EVERY == 0 || op == TEST_FUZZ_OPS - 1)) ok = test_compare_full(t, buf, &ref, when);
        if (!ok) LOG("test: the failing op was replace(%D, %D, %D bytes); size now %D", start, end, s.len, ref.len);
    }
    if (ok) {
        LOG("test: ok: fuzz, %d ops, seed 0x%X, final size %D, %D lines, %D gap moves, %D gap growths, modified %d",
            TEST_FUZZ_OPS, seed, ref.len, ref.newlines + 1, gap_moves, grows, buf->modified);
    }
    buffer_destroy(buf);
    return ok;
}

// ---------------------------------------------------------------------------
// Markers: differential fuzz against naively adjusted positions

#define TEST_MARKER_OPS 20000
#define TEST_MARKER_COUNT 300

typedef struct TestMarker {
    BufferMarker handle; // 0 = destroyed
    i64 pos;
    b32 advance;
} TestMarker;

// The rules of buffer.h, applied to one position. Snapping is done separately, on every marker.
static i64 test_ref_adjust(i64 pos, b32 advance, i64 start, i64 end, i64 len) {
    if (pos < start) return pos;
    if (pos > end) return pos + len - (end - start);
    if (pos == end && end > start) return start + len;
    return advance ? start + len : start;
}

static i64 test_random_live_marker(Test *t, TestMarker *m, i64 count) {
    for (i32 tries = 0; tries < 16; tries++) {
        i64 k = test_below(t, count);
        if (m[k].handle) return k;
    }
    return -1;
}

static b32 test_markers(Test *t, u64 seed) {
    t->rng = seed ^ 0x6d61726b6572ull;
    Buffer *buf = buffer_create(STR8_LIT("markers"));
    TEST_CHECK(t, buf, "markers: buffer_create failed");
    TestRef ref = { PUSH_ARRAY(&t->arena, u8, MB(1)), 0, MB(1), 0 };
    u8 *text = PUSH_ARRAY(&t->arena, u8, KB(64));
    String8 init = test_random_text(t, text, KB(4));
    buffer_replace(buf, 0, 0, init);
    test_ref_replace(&ref, 0, 0, init);

    TestMarker *m = PUSH_ARRAY(&t->arena, TestMarker, TEST_MARKER_COUNT);
    for (i64 k = 0; k < TEST_MARKER_COUNT; k++) {
        m[k].advance = (b32)test_below(t, 2);
        m[k].pos = test_ref_snap(&ref, test_below(t, ref.len + 1));
        m[k].handle = buffer_marker_create(buf, m[k].pos, m[k].advance);
    }
    i64 live = TEST_MARKER_COUNT, snaps = 0, at_marker = 0;
    b32 ok = 1;
    for (i32 op = 0; op < TEST_MARKER_OPS && ok; op++) {
        i64 r = test_below(t, 100);
        i64 start = 0, end = 0;
        String8 s = str8(text, 0);
        if (r < 4) { // destroy one, or bring a destroyed one back (slot reuse)
            i64 k = test_below(t, TEST_MARKER_COUNT);
            if (m[k].handle) {
                buffer_marker_destroy(buf, m[k].handle);
                m[k].handle = 0;
                live--;
            } else {
                m[k].advance = (b32)test_below(t, 2);
                i64 want = test_below(t, ref.len + 1); // not necessarily a boundary: create snaps
                m[k].pos = test_ref_snap(&ref, want);
                m[k].handle = buffer_marker_create(buf, want, m[k].advance);
                live++;
            }
        } else if (r < 8) { // set
            i64 k = test_random_live_marker(t, m, TEST_MARKER_COUNT);
            if (k >= 0) {
                i64 want = test_below(t, ref.len + 1);
                buffer_marker_set(buf, m[k].handle, want);
                m[k].pos = test_ref_snap(&ref, want);
            }
        } else { // edit, often exactly at markers
            i64 k = test_random_live_marker(t, m, TEST_MARKER_COUNT);
            b32 from_marker = k >= 0 && test_below(t, 2);
            start = from_marker ? m[k].pos : test_below(t, ref.len + 1);
            at_marker += from_marker;
            i64 kind = test_below(t, 3); // insert, delete, replace
            if (ref.len > (i64)KB(32)) kind = 1;
            if (kind != 0) {
                i64 k2 = test_random_live_marker(t, m, TEST_MARKER_COUNT);
                end = (k2 >= 0 && test_below(t, 2) && m[k2].pos >= start) ? m[k2].pos : start + 1 + test_below(t, 24);
            } else {
                end = start;
            }
            end = MIN(end, ref.len);
            start = test_ref_snap(&ref, start);
            end = test_ref_snap(&ref, MAX(start, end));
            if (kind != 1) s = test_random_text(t, text, 1 + test_below(t, 8));
            if (kind != 1 && test_below(t, 3) == 0) { // fragments of multi-byte characters: joins and splits
                static const char *frags[] = { "â", "", "¬", "â", "¬", "Ä", "", "ð", "" };
                String8 f = str8_cstr(frags[test_below(t, ARRAY_COUNT(frags))]);
                memcpy(text, f.data, (size_t)f.len);
                s = str8(text, f.len);
            }
            if (!buffer_replace(buf, start, end, s)) {
                LOG("test: FAIL: markers op %d (seed 0x%X): replace(%D, %D, %D bytes) refused", op, seed, start, end, s.len);
                t->failures++;
                ok = 0;
                break;
            }
            test_ref_replace(&ref, start, end, s);
            for (i64 j = 0; j < TEST_MARKER_COUNT; j++) {
                if (!m[j].handle) continue;
                i64 adjusted = test_ref_adjust(m[j].pos, m[j].advance, start, end, s.len);
                m[j].pos = test_ref_snap(&ref, adjusted); // every marker, not only the ones near the edit
                snaps += m[j].pos != adjusted;
            }
        }
        if (buf->marker_live != live) {
            LOG("test: FAIL: markers op %d (seed 0x%X): %D live markers, expected %D", op, seed, buf->marker_live, live);
            t->failures++;
            ok = 0;
            break;
        }
        for (i64 j = 0; j < TEST_MARKER_COUNT && ok; j++) {
            if (!m[j].handle) continue;
            i64 got = buffer_marker_get(buf, m[j].handle);
            if (got != m[j].pos) {
                LOG("test: FAIL: markers op %d (seed 0x%X): marker %D (%s) at %D, expected %D; last edit replace(%D, %D, %D bytes)",
                    op, seed, j, m[j].advance ? "advance" : "stay", got, m[j].pos, start, end, s.len);
                t->failures++;
                ok = 0;
            }
        }
    }
    if (ok) {
        for (i64 j = 0; j < TEST_MARKER_COUNT; j++) if (m[j].handle) buffer_marker_destroy(buf, m[j].handle);
        TEST_CHECK(t, buf->marker_live == 0, "markers: %D live after destroying all", buf->marker_live);
        LOG("test: ok: markers, %d ops, %d markers, seed 0x%X, %D edits at a marker, %D snaps to a boundary, %D slots used",
            TEST_MARKER_OPS, TEST_MARKER_COUNT, seed, at_marker, snaps, buf->marker_count);
    }
    buffer_destroy(buf);
    return ok;
}

// ---------------------------------------------------------------------------
// Capacity and read-only

static b32 test_fill(Buffer *buf, TestRef *ref, i64 n, u8 *scratch) {
    for (i64 i = 0; i < n; i++) scratch[i] = (u8)(i % 61 == 60 ? '\n' : 'a' + i % 26);
    String8 s = str8(scratch, n);
    if (!buffer_replace(buf, buffer_size(buf), buffer_size(buf), s)) return 0;
    test_ref_replace(ref, ref->len, ref->len, s);
    return 1;
}

static b32 test_capacity(Test *t) {
    i64 cap = KB(256);
    Buffer *buf = buffer_create_reserve(STR8_LIT("small"), cap);
    TEST_CHECK(t, buf, "capacity: buffer_create_reserve failed");
    TestRef ref = { PUSH_ARRAY(&t->arena, u8, cap + 16), 0, cap + 16, 0 };
    u8 *scratch = PUSH_ARRAY(&t->arena, u8, cap + 16);
    b32 ok = test_fill(buf, &ref, KB(200), scratch);
    TEST_CHECK(t, ok, "capacity: 200 KB into a 256 KB buffer refused");
    u64 edits = buf->edit_count;

    // Too large by one byte: refused, nothing changes.
    i64 too_much = cap - ref.len + 1;
    for (i64 i = 0; i < too_much; i++) scratch[i] = 'x';
    TEST_CHECK(t, !buffer_replace(buf, 100, 100, str8(scratch, too_much)), "capacity: an insert past capacity was accepted");
    TEST_CHECK(t, buf->edit_count == edits, "capacity: a refused edit changed edit_count");
    if (!test_compare_full(t, buf, &ref, "capacity after refusal")) return 0;

    // Exactly to capacity, through a replace in the middle: accepted.
    String8 exact = str8(scratch, too_much - 1 + 10);
    TEST_CHECK(t, buffer_replace(buf, 1000, 1010, exact), "capacity: filling exactly to capacity was refused");
    test_ref_replace(&ref, 1000, 1010, exact);
    TEST_CHECK(t, buffer_size(buf) == cap, "capacity: size %D, expected %D", buffer_size(buf), cap);
    if (!test_compare_full(t, buf, &ref, "capacity full")) return 0;
    TEST_CHECK(t, !buffer_replace(buf, 0, 0, STR8_LIT("y")), "capacity: one byte past a full buffer was accepted");
    TEST_CHECK(t, buffer_replace(buf, 0, 1, STR8_LIT("y")), "capacity: a same-size replace in a full buffer was refused");
    test_ref_replace(&ref, 0, 1, STR8_LIT("y"));
    if (!test_compare_full(t, buf, &ref, "capacity same-size")) return 0;
    TEST_CHECK(t, buffer_destroy(buf), "capacity: buffer memory not released");
    LOG("test: ok: capacity (refused past %D bytes, buffer unchanged)", cap);
    return 1;
}

static b32 test_read_only(Test *t) {
    Buffer *buf = buffer_create(STR8_LIT("ro"));
    TEST_CHECK(t, buf, "read-only: buffer_create failed");
    TestRef ref = { PUSH_ARRAY(&t->arena, u8, KB(64)), 0, KB(64), 0 };
    u8 *scratch = PUSH_ARRAY(&t->arena, u8, KB(16));
    TEST_CHECK(t, test_fill(buf, &ref, KB(10), scratch), "read-only: fill refused");
    buf->modified = 0;
    buf->read_only = 1;
    u64 edits = buf->edit_count;

    TEST_CHECK(t, !buffer_replace(buf, 5, 9, STR8_LIT("x\ny")), "read-only: an edit was accepted");
    TEST_CHECK(t, !buffer_replace(buf, 0, 0, STR8_LIT("\n")), "read-only: an insert was accepted");
    TEST_CHECK(t, !buffer_replace(buf, 0, 100, STR8_LIT("")), "read-only: a delete was accepted");
    TEST_CHECK(t, !buf->modified && buf->edit_count == edits, "read-only: a refused edit changed modified / edit_count");
    if (!test_compare_full(t, buf, &ref, "read-only refused")) return 0;

    buf->inhibit_read_only = 1;
    TEST_CHECK(t, buffer_replace(buf, 5, 9, STR8_LIT("x\ny")), "read-only: an edit with inhibit_read_only was refused");
    test_ref_replace(&ref, 5, 9, STR8_LIT("x\ny"));
    TEST_CHECK(t, buf->modified && buf->edit_count == edits + 1, "read-only: an inhibited edit did not update modified / edit_count");
    if (!test_compare_full(t, buf, &ref, "read-only inhibited")) return 0;

    buf->inhibit_read_only = 0;
    TEST_CHECK(t, !buffer_replace(buf, 0, 0, STR8_LIT("z")), "read-only: an edit after clearing inhibit_read_only was accepted");
    if (!test_compare_full(t, buf, &ref, "read-only refused again")) return 0;
    TEST_CHECK(t, buffer_destroy(buf), "read-only: buffer memory not released");
    LOG("test: ok: read-only and inhibit_read_only");
    return 1;
}

// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Files

static String8 test_path(Test *t, const char *name, const char *suffix) {
    return str8_fmt(&t->arena, "%S\\rt_%s%s", t->tmp_dir, name, suffix);
}

static b32 test_read_file(Test *t, String8 path, String8 *out) {
    OsFile file;
    OsFileInfo info;
    if (os_file_open_read(path, &file, &info) != OS_FILE_OK) return 0;
    u8 *data = PUSH_ARRAY(&t->arena, u8, info.size);
    OsFileStatus status = os_file_read(file, data, info.size);
    os_file_close(file);
    *out = str8(data, info.size);
    return status == OS_FILE_OK;
}

static b32 test_file_absent(String8 path) {
    OsFileInfo info;
    return os_file_info(path, &info) == OS_FILE_NOT_FOUND;
}

typedef struct TestExpect {
    BufferEncoding encoding;
    BufferEol eol;
    String8 text;
} TestExpect;

// What loading `bytes` must produce, computed the naive way: whole-array conversion, then
// classification and CR stripping on the converted text.
static TestExpect test_expect_load(Test *t, String8 b) {
    TestExpect e = { BUFFER_UTF8, BUFFER_EOL_LF, { 0 } };
    u8 *out = PUSH_ARRAY(&t->arena, u8, b.len * 2 + 4);
    i64 n = 0;
    b32 even = (b.len & 1) == 0;
    if (b.len >= 3 && b.data[0] == 0xEF && b.data[1] == 0xBB && b.data[2] == 0xBF) e.encoding = BUFFER_UTF8_BOM;
    else if (b.len >= 2 && even && b.data[0] == 0xFF && b.data[1] == 0xFE) e.encoding = BUFFER_UTF16LE;
    else if (b.len >= 2 && even && b.data[0] == 0xFE && b.data[1] == 0xFF) e.encoding = BUFFER_UTF16BE;
    if (e.encoding == BUFFER_UTF16LE || e.encoding == BUFFER_UTF16BE) {
        b32 big = e.encoding == BUFFER_UTF16BE;
        i64 units = b.len / 2 - 1;
        u8 *u8s = b.data + 2;
        for (i64 i = 0; i < units; i++) {
            u32 u = big ? ((u32)u8s[2 * i] << 8 | u8s[2 * i + 1]) : ((u32)u8s[2 * i + 1] << 8 | u8s[2 * i]);
            u32 next = 0;
            if (i + 1 < units) next = big ? ((u32)u8s[2 * i + 2] << 8 | u8s[2 * i + 3]) : ((u32)u8s[2 * i + 3] << 8 | u8s[2 * i + 2]);
            if (u >= 0xD800 && u <= 0xDBFF && next >= 0xDC00 && next <= 0xDFFF) {
                n += utf8_encode(0x10000 + ((u - 0xD800) << 10) + (next - 0xDC00), out + n);
                i++;
            } else if (u >= 0xD800 && u <= 0xDFFF) { // WTF-8
                out[n++] = (u8)(0xE0 | (u >> 12));
                out[n++] = (u8)(0x80 | ((u >> 6) & 0x3F));
                out[n++] = (u8)(0x80 | (u & 0x3F));
            } else {
                n += utf8_encode(u, out + n);
            }
        }
    } else {
        i64 skip = e.encoding == BUFFER_UTF8_BOM ? 3 : 0;
        memcpy(out, b.data + skip, (size_t)(b.len - skip));
        n = b.len - skip;
    }
    i64 crlf = 0, bare = 0;
    for (i64 i = 0; i < n; i++) {
        if (out[i] == '\n') {
            if (i > 0 && out[i - 1] == '\r') crlf++;
            else bare++;
        }
    }
    e.eol = crlf && bare ? BUFFER_EOL_MIXED : crlf ? BUFFER_EOL_CRLF : BUFFER_EOL_LF;
    if (e.eol == BUFFER_EOL_CRLF) {
        i64 w = 0;
        for (i64 i = 0; i < n; i++) {
            if (out[i] == '\r' && i + 1 < n && out[i + 1] == '\n') continue;
            out[w++] = out[i];
        }
        n = w;
    }
    e.text = str8(out, n);
    return e;
}

static const char *test_encoding_name(BufferEncoding e) {
    return e == BUFFER_UTF8 ? "UTF-8" : e == BUFFER_UTF8_BOM ? "UTF-8 BOM" : e == BUFFER_UTF16LE ? "UTF-16LE" : "UTF-16BE";
}

static const char *test_eol_name(BufferEol e) {
    return e == BUFFER_EOL_LF ? "LF" : e == BUFFER_EOL_CRLF ? "CRLF" : "mixed";
}

// Writes `bytes`, loads them, checks encoding, line endings, text and line index against the
// expectation, then saves (as a new file, and in place over the original) and requires the
// written bytes to equal the input. `want_*` < 0: take the reference's word for it.
static b32 test_round_trip(Test *t, const char *name, String8 bytes, i32 want_encoding, i32 want_eol, b32 flush) {
    u64 mark = arena_pos(&t->arena);
    String8 path = test_path(t, name, "");
    String8 out_path = test_path(t, name, ".out");
    TEST_CHECK(t, os_write_file(path, bytes), "%s: could not write the input file", name);
    TestExpect e = test_expect_load(t, bytes);
    if (want_encoding >= 0) TEST_CHECK(t, e.encoding == (BufferEncoding)want_encoding, "%s: reference says %s", name, test_encoding_name(e.encoding));
    if (want_eol >= 0) TEST_CHECK(t, e.eol == (BufferEol)want_eol, "%s: reference says %s", name, test_eol_name(e.eol));

    Buffer *buf = buffer_create(STR8_LIT("rt"));
    TEST_CHECK(t, buf, "%s: buffer_create failed", name);
    OsFileStatus status = buffer_load_file(buf, path);
    b32 ok = status == OS_FILE_OK;
    if (!ok) LOG("test: FAIL: %s: load: %s", name, buffer_status_text(status));
    if (ok && (buf->encoding != e.encoding || buf->eol != e.eol)) {
        LOG("test: FAIL: %s: loaded as %s %s, expected %s %s", name, test_encoding_name(buf->encoding), test_eol_name(buf->eol),
            test_encoding_name(e.encoding), test_eol_name(e.eol));
        ok = 0;
    }
    if (ok && (buf->modified || buf->edit_count != 0 || buf->file_size != bytes.len)) {
        LOG("test: FAIL: %s: after load modified %d, edit_count %U, file_size %D", name, buf->modified, buf->edit_count, buf->file_size);
        ok = 0;
    }
    if (ok) {
        TestRef ref = { e.text.data, e.text.len, e.text.len, test_count_newlines(e.text.data, e.text.len) };
        char when[96];
        test_cstr(when, sizeof(when), "%s: loaded text", name);
        ok = test_compare_full(t, buf, &ref, when);
        if (!ok) t->failures--; // counted again below
    }
    if (ok) {
        status = buffer_save_as_opt(buf, out_path, flush);
        String8 written;
        if (status != OS_FILE_OK) {
            LOG("test: FAIL: %s: save-as: %s", name, buffer_status_text(status));
            ok = 0;
        } else if (!test_read_file(t, out_path, &written) || written.len != bytes.len || !test_equal(written.data, bytes.data, bytes.len)) {
            LOG("test: FAIL: %s: save-as wrote %D bytes that differ from the %D input bytes", name, written.len, bytes.len);
            ok = 0;
        } else if (!str8_equal(buffer_file_name(buf->path), buffer_file_name(out_path)) || !str8_equal(buf->name, buffer_file_name(out_path))) {
            LOG("test: FAIL: %s: save-as did not visit the new path (%S)", name, buf->path);
            ok = 0;
        }
        os_file_delete(out_path);
    }
    if (ok && flush) {
        // In place over the original: the ReplaceFileW path.
        buffer_save_as_opt(buf, path, 1);
        String8 written;
        if (!test_read_file(t, path, &written) || written.len != bytes.len || !test_equal(written.data, bytes.data, bytes.len)) {
            LOG("test: FAIL: %s: saving over the original changed its bytes", name);
            ok = 0;
        } else if (!test_file_absent(str8_fmt(&t->arena, "%S.teal~1", path))) {
            LOG("test: FAIL: %s: a temp file was left behind", name);
            ok = 0;
        }
    }
    TEST_CHECK(t, buffer_destroy(buf), "%s: buffer memory not released", name);
    if (ok && !flush) os_file_delete(path); // the boundary sweeps: hundreds of files, kept only on failure
    arena_pop_to(&t->arena, mark);
    if (!ok) t->failures++;
    return ok;
}

// Bytes from a C literal, embedded NULs included.
#define TEST_BYTES(s) str8((u8 *)(s), (i64)sizeof(s) - 1)

static String8 test_utf16(Test *t, const u16 *units, i64 count, b32 big) {
    u8 *b = PUSH_ARRAY(&t->arena, u8, count * 2 + 2);
    u16 bom = 0xFEFF;
    for (i64 i = -1; i < count; i++) {
        u16 u = i < 0 ? bom : units[i];
        b[2 * (i + 1)] = (u8)(big ? u >> 8 : u);
        b[2 * (i + 1) + 1] = (u8)(big ? u : u >> 8);
    }
    return str8(b, count * 2 + 2);
}

static void test_files(Test *t) {
    i32 before = t->failures;
    enum { ANY = -1 };
    const i32 U8 = BUFFER_UTF8, BOM = BUFFER_UTF8_BOM, LE = BUFFER_UTF16LE, BE = BUFFER_UTF16BE;
    const i32 LF = BUFFER_EOL_LF, CRLF = BUFFER_EOL_CRLF, MIXED = BUFFER_EOL_MIXED;

    test_round_trip(t, "lf.c", TEST_BYTES("int main(void) {\n\treturn 0;\n}\n"), U8, LF, 1);
    test_round_trip(t, "crlf.c", TEST_BYTES("// CRLF file\r\nint main(void) {\r\n\treturn 0;\r\n}\r\n"), U8, CRLF, 1);
    test_round_trip(t, "mixed.txt", TEST_BYTES("crlf\r\nlf\ncrlf again\r\nlf\n"), U8, MIXED, 1);
    test_round_trip(t, "lone_cr.txt", TEST_BYTES("a\rb\r\nc\r\n"), U8, CRLF, 1);
    test_round_trip(t, "cr_cr_lf.txt", TEST_BYTES("x\r\r\ny\r\n"), U8, CRLF, 1);
    test_round_trip(t, "bom.txt", TEST_BYTES("\xEF\xBB\xBFh\xC3\xA9llo, BOM\n"), BOM, LF, 1);
    test_round_trip(t, "bom_crlf.txt", TEST_BYTES("\xEF\xBB\xBF" "a\r\nb\r\n"), BOM, CRLF, 1);
    test_round_trip(t, "bom_only.txt", TEST_BYTES("\xEF\xBB\xBF"), BOM, LF, 1);
    test_round_trip(t, "invalid.txt", TEST_BYTES("valid: \xC4\x9F\xE2\x82\xAC\n"
                                                 "stray continuation: \x80 never valid: \xFF \xFE\n"
                                                 "truncated: \xE2\x82 overlong: \xC0\xAF surrogate: \xED\xA0\x80 end\n"), U8, LF, 1);
    test_round_trip(t, "nul.txt", TEST_BYTES("a\0b\0\0c\n\0\n"), U8, LF, 1);
    test_round_trip(t, "empty.txt", TEST_BYTES(""), U8, LF, 1);
    test_round_trip(t, "no_final_newline.txt", TEST_BYTES("line 1\nline 2"), U8, LF, 1);
    test_round_trip(t, "only_lf.txt", TEST_BYTES("\n\n\n\n\n"), U8, LF, 1);
    test_round_trip(t, "only_crlf.txt", TEST_BYTES("\r\n\r\n\r\n"), U8, CRLF, 1);
    test_round_trip(t, "odd_ff_fe.txt", TEST_BYTES("\xFF\xFE\x41"), U8, LF, 1);
    test_round_trip(t, "last_cr.txt", TEST_BYTES("\r"), U8, LF, 1);
    test_round_trip(t, "last_cr_lf.txt", TEST_BYTES("a\nb\r"), U8, LF, 1);
    test_round_trip(t, "last_cr_crlf.txt", TEST_BYTES("a\r\nb\r"), U8, CRLF, 1);
    test_round_trip(t, "last_cr_cr_crlf.txt", TEST_BYTES("a\r\nb\r\r"), U8, CRLF, 1);

    {   // One 10 MB line.
        i64 n = MB(10);
        u8 *b = PUSH_ARRAY(&t->arena, u8, n);
        for (i64 i = 0; i < n; i++) b[i] = (u8)('a' + i % 26);
        test_round_trip(t, "long_line.txt", str8(b, n), U8, LF, 1);
    }
    {   // ~4 MB of CRLF lines whose very last newline is a bare LF: the whole file is restored.
        i64 lines = 100000, n = 0;
        u8 *b = PUSH_ARRAY(&t->arena, u8, lines * 48 + 3);
        for (i32 bom = 0; bom < 2; bom++) {
            memcpy(b, "\xEF\xBB\xBF", 3);
            n = bom ? 3 : 0;
            for (i64 l = 0; l < lines; l++) {
                i64 len = 10 + l % 30;
                for (i64 i = 0; i < len; i++) b[n++] = (u8)('a' + (l + i) % 26);
                if (l + 1 < lines) b[n++] = '\r';
                b[n++] = '\n';
            }
            test_round_trip(t, bom ? "rollback_bom.txt" : "rollback.txt", str8(b, n), bom ? BOM : U8, MIXED, 1);
        }
    }
    {   // CR and LF on either side of every 16-byte SSE block boundary, while stripping and while
        // rolling back, without and with a BOM (which shifts the file offsets by 3).
        u8 b[128];
        char name[64];
        for (i32 k = 0; k <= 48; k++) {
            for (i32 variant = 0; variant < 4; variant++) {
                i64 n = 0;
                if (variant == 3) { memcpy(b, "\xEF\xBB\xBF", 3); n = 3; }
                for (i32 i = 0; i < k; i++) b[n++] = 'a';
                b[n++] = '\r';
                b[n++] = '\n';
                if (variant == 1) { // a bare LF after the pair: rollback
                    for (i32 i = 0; i < k % 7; i++) b[n++] = 'b';
                    b[n++] = '\n';
                } else if (variant == 2) { // a CRLF pair first, then the bare LF at offset k
                    n = 0;
                    memcpy(b, "x\r\n", 3);
                    n = 3;
                    for (i32 i = 3; i < k; i++) b[n++] = 'c';
                    b[n++] = '\n';
                } else {
                    memcpy(b + n, "end\r\n", 5);
                    n += 5;
                }
                test_cstr(name, sizeof(name), "sse_%d_%d.txt", k, variant);
                test_round_trip(t, name, str8(b, n), ANY, ANY, 0);
            }
        }
    }

    {   // UTF-16, both byte orders.
        static const u16 text[] = { 'H', 'i', '\r', '\n', 0x011F, 0xD83D, 0xDE00, '\r', '\n' }; // ğ 😀
        static const u16 lone[] = { 'a', 0xD800, 'b', 0xDC00, 0xD83D, 0xDE00, '\n', 0xD83D };  // unpaired surrogates
        static const u16 mixed[] = { 'a', '\r', '\n', 'b', '\n' };
        for (i32 big = 0; big < 2; big++) {
            test_round_trip(t, big ? "utf16be.txt" : "utf16le.txt", test_utf16(t, text, ARRAY_COUNT(text), big), big ? BE : LE, CRLF, 1);
            test_round_trip(t, big ? "utf16be_lone.txt" : "utf16le_lone.txt", test_utf16(t, lone, ARRAY_COUNT(lone), big), big ? BE : LE, LF, 1);
            test_round_trip(t, big ? "utf16be_mixed.txt" : "utf16le_mixed.txt", test_utf16(t, mixed, ARRAY_COUNT(mixed), big), big ? BE : LE, MIXED, 1);
            test_round_trip(t, big ? "utf16be_empty.txt" : "utf16le_empty.txt", test_utf16(t, NULL, 0, big), big ? BE : LE, LF, 1);
        }
    }
    {   // UTF-16 64 KB chunk boundaries (32768 units), all alignments within +-2 units of the
        // first two boundaries: a pair split across, CR | LF, an unpaired high surrogate, a
        // final CR.
        i64 cap = 65536 + 8;
        u16 *u = PUSH_ARRAY(&t->arena, u16, cap);
        char name[64];
        for (i32 boundary = 1; boundary <= 2; boundary++) {
            for (i32 d = -2; d <= 2; d++) {
                i64 p = 32768 * boundary + d; // index of the unit after the boundary
                for (i32 kind = 0; kind < 4; kind++) {
                    i64 count = p + 4;
                    for (i64 i = 0; i < count; i++) u[i] = (u16)(i % 50 == 49 ? '\n' : 'a' + i % 26);
                    for (i64 i = 0; i < count; i++) if (u[i] == '\n') u[i - 1] = '\r'; // all CRLF
                    if (kind == 0) { u[p - 1] = 0xD83D; u[p] = 0xDE00; }
                    if (kind == 1) { u[p - 1] = '\r'; u[p] = '\n'; }
                    if (kind == 2) { u[p - 1] = 0xD800; u[p] = 'x'; }
                    if (kind == 3) { count = p; u[p - 1] = '\r'; }
                    if (u[p - 2] == '\r' && kind != 1) u[p - 2] = 'y'; // keep every CR paired with its LF
                    for (i32 big = 0; big < 2; big++) {
                        test_cstr(name, sizeof(name), "utf16_chunk_%d_%d_%d_%s.txt", boundary, d, kind, big ? "be" : "le");
                        test_round_trip(t, name, test_utf16(t, u, count, big), big ? BE : LE, ANY, 0);
                    }
                }
            }
        }
    }
    if (t->failures == before) LOG("test: ok: file round trips");
}

// Load, edit, save, compare with the expected bytes, reload, compare the text.
static b32 test_edit_save(Test *t, const char *name, String8 bytes, i64 at, String8 insert, String8 want) {
    u64 mark = arena_pos(&t->arena);
    String8 path = test_path(t, name, "");
    TEST_CHECK(t, os_write_file(path, bytes), "%s: could not write the input file", name);
    Buffer *buf = buffer_create(STR8_LIT("edit"));
    TEST_CHECK(t, buffer_load_file(buf, path) == OS_FILE_OK, "%s: load failed", name);
    TEST_CHECK(t, buffer_replace(buf, at, at, insert), "%s: edit refused", name);
    TEST_CHECK(t, buf->modified, "%s: not modified after an edit", name);
    TEST_CHECK(t, buffer_save(buf) == OS_FILE_OK, "%s: save failed", name);
    TEST_CHECK(t, !buf->modified, "%s: still modified after saving", name);
    String8 written;
    TEST_CHECK(t, test_read_file(t, path, &written) && written.len == want.len && test_equal(written.data, want.data, want.len),
               "%s: saved %D bytes, not the expected %D", name, written.len, want.len);
    i64 size = buffer_size(buf);
    u8 *text = PUSH_ARRAY(&t->arena, u8, size);
    buffer_copy(buf, 0, size, text);
    BufferEncoding encoding = buf->encoding;
    BufferEol eol = buf->eol;
    buffer_destroy(buf);

    buf = buffer_create(STR8_LIT("reload"));
    TEST_CHECK(t, buffer_load_file(buf, path) == OS_FILE_OK, "%s: reload failed", name);
    TestRef ref = { text, size, size, test_count_newlines(text, size) };
    TEST_CHECK(t, buf->encoding == encoding && buf->eol == eol, "%s: reloaded as %s %s", name,
               test_encoding_name(buf->encoding), test_eol_name(buf->eol));
    if (!test_compare_full(t, buf, &ref, name)) return 0;
    buffer_destroy(buf);
    arena_pop_to(&t->arena, mark);
    return 1;
}

static void test_edits(Test *t) {
    i32 before = t->failures;
    test_edit_save(t, "edit_crlf.txt", TEST_BYTES("one\r\ntwo\r\n"), 4, TEST_BYTES("new\nline "),
                   TEST_BYTES("one\r\nnew\r\nline two\r\n"));
    test_edit_save(t, "edit_mixed.txt", TEST_BYTES("a\r\nb\n"), 3, TEST_BYTES("x\n"), TEST_BYTES("a\r\nx\nb\n"));
    test_edit_save(t, "edit_bom.txt", TEST_BYTES("\xEF\xBB\xBF" "ab"), 1, TEST_BYTES("\xC4\x9F\n"),
                   TEST_BYTES("\xEF\xBB\xBF" "a\xC4\x9F\nb"));
    static const u16 before16[] = { 'a', 'b', '\r', '\n' };
    static const u16 after16[] = { 0x011F, '\r', '\n', 0xD83D, 0xDE00, 'a', 'b', '\r', '\n' };
    for (i32 big = 0; big < 2; big++) {
        test_edit_save(t, big ? "edit_utf16be.txt" : "edit_utf16le.txt", test_utf16(t, before16, 4, big), 0,
                       TEST_BYTES("\xC4\x9F\n\xF0\x9F\x98\x80"), test_utf16(t, after16, ARRAY_COUNT(after16), big));
    }
    if (t->failures == before) LOG("test: ok: load, edit, save, reload");
}

static b32 test_failures(Test *t) {
    String8 path = test_path(t, "readonly.txt", "");
    String8 original = TEST_BYTES("original\r\n");
    os_dev_set_read_only(path, 0);
    TEST_CHECK(t, os_write_file(path, original), "save failure: could not write the input file");
    Buffer *buf = buffer_create(STR8_LIT("ro"));
    TEST_CHECK(t, buffer_load_file(buf, path) == OS_FILE_OK, "save failure: load failed");
    TEST_CHECK(t, buffer_replace(buf, 0, 0, STR8_LIT("changed ")), "save failure: edit refused");
    TEST_CHECK(t, os_dev_set_read_only(path, 1), "save failure: could not make the file read-only");
    OsFileStatus status = buffer_save(buf);
    os_dev_set_read_only(path, 0);
    String8 now;
    TEST_CHECK(t, status == OS_FILE_READ_ONLY, "save failure: saving over a read-only file returned '%s'", buffer_status_text(status));
    TEST_CHECK(t, test_read_file(t, path, &now) && now.len == original.len && test_equal(now.data, original.data, now.len),
               "save failure: the read-only original changed");
    TEST_CHECK(t, buf->modified, "save failure: the buffer is no longer modified");
    TEST_CHECK(t, test_file_absent(str8_fmt(&t->arena, "%S.teal~1", path)), "save failure: a temp file was left behind");

    String8 old_path = buf->path;
    status = buffer_save_as(buf, str8_fmt(&t->arena, "%S\\no_such_dir\\x.txt", t->tmp_dir));
    TEST_CHECK(t, status == OS_FILE_NOT_FOUND, "save failure: save-as into a missing directory returned '%s'", buffer_status_text(status));
    TEST_CHECK(t, str8_equal(buf->path, old_path), "save failure: a failed save-as changed the path");
    buffer_destroy(buf);

    buf = buffer_create(STR8_LIT("missing"));
    status = buffer_load_file(buf, test_path(t, "does_not_exist.txt", ""));
    TEST_CHECK(t, status == OS_FILE_NOT_FOUND && buffer_size(buf) == 0, "load failure: a missing file gave '%s'", buffer_status_text(status));
    status = buffer_load_file(buf, t->tmp_dir);
    TEST_CHECK(t, status == OS_FILE_IS_DIRECTORY && buffer_size(buf) == 0, "load failure: a directory gave '%s'", buffer_status_text(status));
    buffer_destroy(buf);

    // A file with two hard links: the save must go in place so both names see the new text.
    String8 a = test_path(t, "link_a.txt", ""), b = test_path(t, "link_b.txt", "");
    os_file_delete(b);
    TEST_CHECK(t, os_write_file(a, TEST_BYTES("linked\n")) && os_dev_hard_link(a, b), "hard link: setup failed");
    buf = buffer_create(STR8_LIT("link"));
    TEST_CHECK(t, buffer_load_file(buf, a) == OS_FILE_OK, "hard link: load failed");
    buffer_replace(buf, 0, 0, STR8_LIT("still "));
    TEST_CHECK(t, buffer_save(buf) == OS_FILE_OK, "hard link: save failed");
    TEST_CHECK(t, test_read_file(t, b, &now) && str8_equal(now, STR8_LIT("still linked\n")),
               "hard link: the other name does not see the saved text (link broken by a swap?)");
    buffer_destroy(buf);
    os_file_delete(b);
    LOG("test: ok: save to a read-only file refused, original untouched; load / save-as failures; hard links kept");
    return 1;
}

// The language from the extension, through buffer_set_path.
static b32 test_language(Test *t) {
    static const struct { const char *path; BufferLanguage language; } cases[] = {
        { "C:\\x\\a.jai", BUFFER_LANG_JAI }, { "a.c", BUFFER_LANG_C }, { "A.H", BUFFER_LANG_C },
        { "a.cpp", BUFFER_LANG_CPP }, { "a.hpp", BUFFER_LANG_CPP }, { "a.cc", BUFFER_LANG_CPP },
        { "a.cxx", BUFFER_LANG_CPP }, { "a.hh", BUFFER_LANG_CPP }, { "a.cs", BUFFER_LANG_CSHARP },
        { "a.js", BUFFER_LANG_JAVASCRIPT }, { "a.mjs", BUFFER_LANG_JAVASCRIPT }, { "a.cjs", BUFFER_LANG_JAVASCRIPT },
        { "a.jsx", BUFFER_LANG_JAVASCRIPT }, { "a.ts", BUFFER_LANG_TYPESCRIPT }, { "a.Tsx", BUFFER_LANG_TYPESCRIPT },
        { "a.txt", BUFFER_LANG_FUNDAMENTAL }, { "Makefile", BUFFER_LANG_FUNDAMENTAL }, { "a.c.bak", BUFFER_LANG_FUNDAMENTAL },
        { "dir.c\\file", BUFFER_LANG_FUNDAMENTAL }, { "a.", BUFFER_LANG_FUNDAMENTAL },
    };
    Buffer *buf = buffer_create(STR8_LIT("lang"));
    TEST_CHECK(t, buf, "language: buffer_create failed");
    for (i64 i = 0; i < ARRAY_COUNT(cases); i++) {
        buffer_set_path(buf, str8_cstr(cases[i].path));
        TEST_CHECK(t, buf->language == cases[i].language, "language: %s gave %s, expected %s", cases[i].path,
                   buffer_language_name(buf->language), buffer_language_name(cases[i].language));
    }
    buffer_destroy(buf);
    LOG("test: ok: language from the file extension");
    return 1;
}

// ---------------------------------------------------------------------------
// --bench-buffer (the frame part runs in the platform layer, through the real app path)

#define TEST_BENCH_SIZE MB(100)

// build\tmp\bench_100mb.txt: ~100 MB of code-like lines (~2 M), generated once.
String8 test_bench_buffer_file(Arena *arena, String8 tmp_dir) {
    String8 path = str8_fmt(arena, "%S\\bench_100mb.txt", tmp_dir);
    OsFileInfo info;
    if (os_file_info(path, &info) == OS_FILE_OK && info.size >= (i64)TEST_BENCH_SIZE) return path;

    static const char *words[] = {
        "if", "(", ")", "{", "}", "return", "buffer", "->", "gap_start", "=", "+", "i64", "u8", "*",
        "line", "count", "0", ";", "//", "the", "text", "for", "while", "x", "y", "size", "<", "1",
    };
    Test t = { 0 };
    t.rng = 0xbe7c4;
    i64 cap = MB(1), used = 0, total = 0, lines = 0;
    u8 *chunk = PUSH_ARRAY(arena, u8, cap + 256);
    OsFile file;
    if (os_file_open_overwrite(path, &file) != OS_FILE_OK) {
        LOG("bench-buffer: cannot create %S", path);
        return str8(NULL, 0);
    }
    u64 t0 = os_time_us();
    while (total + used < (i64)TEST_BENCH_SIZE) {
        // One line: indentation, then words up to a random length (0..~100 bytes, ~50 on average).
        i64 indent = test_below(&t, 4) * 4, target = test_below(&t, 90);
        for (i64 i = 0; i < indent; i++) chunk[used++] = ' ';
        while (target > 0) {
            String8 w = str8_cstr(words[test_below(&t, ARRAY_COUNT(words))]);
            memcpy(chunk + used, w.data, (size_t)w.len);
            used += w.len;
            chunk[used++] = ' ';
            target -= w.len + 1;
        }
        chunk[used++] = '\n';
        lines++;
        if (used >= cap) {
            os_file_write(file, chunk, used);
            total += used;
            used = 0;
        }
    }
    os_file_write(file, chunk, used);
    total += used;
    os_file_close(file);
    LOG("bench-buffer: generated %S: %D bytes, %D lines in %U ms", path, total, lines, (os_time_us() - t0) / 1000);
    return path;
}

void test_bench_buffer(String8 path, String8 tmp_dir) {
    Test t = { 0 };
    t.rng = 0x5eed;
    Buffer *buf = buffer_create(STR8_LIT("bench"));
    u64 t0 = os_time_us();
    OsFileStatus status = buffer_load_file(buf, path);
    u64 load_us = os_time_us() - t0;
    if (status != OS_FILE_OK) {
        LOG("bench-buffer: load failed: %s", buffer_status_text(status));
        buffer_destroy(buf);
        return;
    }
    i64 size = buffer_size(buf), lines = buffer_line_count(buf);
    LOG("bench-buffer: load (warm file cache): %U ms for %D bytes, %D lines (%U MB/s)", load_us / 1000, size, lines,
        load_us ? (u64)size / load_us : 0);

    // Typing: 10,000 single characters at one spot in the middle. The first one moves the gap there.
    i64 at = buffer_line_start(buf, lines / 2);
    u64 first_us = 0, max_us = 0;
    t0 = os_time_us();
    for (i32 i = 0; i < 10000; i++) {
        u64 a = os_time_us();
        buffer_replace(buf, at + i, at + i, STR8_LIT("x"));
        u64 d = os_time_us() - a;
        if (i == 0) first_us = d;
        else max_us = MAX(max_us, d);
    }
    u64 typing_us = os_time_us() - t0;
    LOG("bench-buffer: 10,000 inserts at one spot: %U us total, first (gap move to the middle) %U us, then avg %U ns, worst %U us",
        typing_us, first_us, (typing_us - first_us) * 1000 / 9999, max_us);

    // 1,000 inserts at random positions: every one moves the gap.
    max_us = 0;
    t0 = os_time_us();
    for (i32 i = 0; i < 1000; i++) {
        i64 line = test_below(&t, buffer_line_count(buf));
        i64 pos = buffer_line_start(buf, line);
        u64 a = os_time_us();
        buffer_replace(buf, pos, pos, STR8_LIT("y"));
        max_us = MAX(max_us, os_time_us() - a);
    }
    u64 random_us = os_time_us() - t0;
    LOG("bench-buffer: 1,000 inserts at random positions: %U ms total, avg %U us, worst %U us", random_us / 1000,
        random_us / 1000, max_us);

    // 1,000,000 offset-to-line lookups.
    size = buffer_size(buf);
    i64 checksum = 0;
    t0 = os_time_us();
    for (i32 i = 0; i < 1000000; i++) checksum += buffer_line_of(buf, test_below(&t, size + 1));
    u64 lookup_us = os_time_us() - t0;
    LOG("bench-buffer: 1,000,000 line_of: %U ms total, avg %U ns (checksum %D)", lookup_us / 1000, lookup_us / 1000, checksum);

    // Saving, with and without the flush to disk.
    Arena scratch = arena_create(MB(1));
    String8 out = str8_fmt(&scratch, "%S\\bench_out.txt", tmp_dir);
    for (i32 flush = 1; flush >= 0; flush--) {
        os_file_delete(out);
        t0 = os_time_us();
        status = buffer_save_as_opt(buf, out, flush);
        u64 save_us = os_time_us() - t0;
        LOG("bench-buffer: save-as %s flush: %U ms for %D bytes (%s)", flush ? "with" : "without", save_us / 1000,
            buffer_size(buf), buffer_status_text(status));
    }
    os_file_delete(out);
    os_release(scratch.base);
    buffer_destroy(buf);
}

i32 test_run(u64 seed, String8 tmp_dir) {
    Test t = { 0 };
    t.arena = arena_create(GB(4));
    t.tmp_dir = tmp_dir;
    LOG("test: seed 0x%X (override with --seed), files in %S", seed, tmp_dir);
    u64 t0 = os_time_us();

    test_fuzz(&t, seed);
    arena_reset(&t.arena);
    test_markers(&t, seed);
    arena_reset(&t.arena);
    test_capacity(&t);
    arena_reset(&t.arena);
    test_read_only(&t);
    arena_reset(&t.arena);
    test_files(&t);
    arena_reset(&t.arena);
    test_edits(&t);
    arena_reset(&t.arena);
    test_failures(&t);
    arena_reset(&t.arena);
    test_language(&t);
    arena_reset(&t.arena);

    LOG("test: %s, %d failure(s), %U ms", t.failures ? "FAIL" : "PASS", t.failures, (os_time_us() - t0) / 1000);
    os_release(t.arena.base);
    return t.failures;
}

#endif // TEAL_DEV
