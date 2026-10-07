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
// View: column mapping

// Reference columns of one line, decoded the naive way from a flat copy: the start offset and
// start column of every character, plus the end of the line as a final entry.
typedef struct TestColumns {
    i64 count; // characters; entry [count] is the end of the line
    i64 *offset;
    i64 *col;
} TestColumns;

static TestColumns test_line_columns(Test *t, u8 *s, i64 len, i64 base) {
    TestColumns c = { 0, PUSH_ARRAY(&t->arena, i64, len + 1), PUSH_ARRAY(&t->arena, i64, len + 1) };
    i64 col = 0;
    for (i64 i = 0; i < len;) {
        c.offset[c.count] = base + i;
        c.col[c.count] = col;
        c.count++;
        u8 b = s[i];
        i64 advance = 1;
        if (b >= 0x80) utf8_decode(s + i, len - i, &advance);
        if (b == '\t') col = (col / 4 + 1) * 4;
        else if ((b < 0x20 && b != '\n') || b == 0x7F) col += 2;
        else col += 1;
        i += advance;
    }
    c.offset[c.count] = base + len;
    c.col[c.count] = col;
    return c;
}

static b32 test_columns(Test *t, u64 seed) {
    t->rng = seed ^ 0x636f6cull;
    static const char *pieces[] = {
        "a", "b", " ", "\t", "\t", "\xC4\x9F", "\xE2\x82\xAC", "\xF0\x9F\x98\x80", "\x01", "\r", "\x7F", "\x00",
        "\x80", "\xFF", "\xE2\x82", "\xC3", // invalid
    };
    static const u8 piece_len[] = { 1, 1, 1, 1, 1, 2, 3, 4, 1, 1, 1, 1, 1, 1, 2, 1 };
    Buffer *buf = buffer_create(STR8_LIT("columns"));
    TEST_CHECK(t, buf, "columns: buffer_create failed");
    u8 *line = PUSH_ARRAY(&t->arena, u8, 1024);
    i64 checks = 0;
    for (i32 iter = 0; iter < 3000; iter++) {
        u64 mark = arena_pos(&t->arena);
        // One line, between two others, with the gap somewhere inside it most of the time.
        i64 n = 0, want = test_below(t, 80);
        while (n < want) {
            i64 k = test_below(t, ARRAY_COUNT(pieces));
            memcpy(line + n, pieces[k], piece_len[k]);
            n += piece_len[k];
        }
        buffer_replace(buf, 0, buffer_size(buf), STR8_LIT("first\n"));
        buffer_replace(buf, 6, 6, str8(line, n));
        buffer_replace(buf, 6 + n, 6 + n, STR8_LIT("\nlast"));
        i64 gap_at = 6 + test_below(t, n + 1);
        buffer_replace(buf, gap_at, gap_at, STR8_LIT("z")); // moves the gap there
        buffer_replace(buf, gap_at, gap_at + 1, STR8_LIT(""));
        TestColumns c = test_line_columns(t, line, n, 6);
        i64 width = c.col[c.count];

        for (i64 k = 0; k <= c.count; k++) {
            i64 got = view_column_of(buf, c.offset[k]);
            TEST_CHECK(t, got == c.col[k], "columns iter %d (seed 0x%X): column_of(%D) = %D, expected %D", iter, seed,
                       c.offset[k], got, c.col[k]);
            i64 back = view_offset_at_column(buf, 1, c.col[k]);
            TEST_CHECK(t, back == c.offset[k], "columns iter %d (seed 0x%X): offset_at_column(%D) = %D, expected %D", iter,
                       seed, c.col[k], back, c.offset[k]);
            checks += 2;
        }
        for (i64 col = 0; col <= width + 3; col++) {
            i64 k = 0; // the character covering col, or the end
            while (k < c.count && c.col[k + 1] <= col) k++;
            i64 expect = k == c.count ? c.offset[k] : 2 * (col - c.col[k]) <= c.col[k + 1] - c.col[k] ? c.offset[k] : c.offset[k + 1];
            i64 got = view_offset_at_column(buf, 1, col);
            TEST_CHECK(t, got == expect, "columns iter %d (seed 0x%X): offset_at_column(col %D) = %D, expected %D", iter,
                       seed, col, got, expect);
            // The drawing walk: the first character that ends past col.
            i64 wc = 0;
            i64 walked = view_walk(buf, 6, 6 + n, &wc, col);
            TEST_CHECK(t, walked == c.offset[k] && wc == c.col[k], "columns iter %d (seed 0x%X): walk to %D stopped at %D col %D, expected %D col %D",
                       iter, seed, col, walked, wc, c.offset[k], c.col[k]);
            checks += 2;
        }
        arena_pop_to(&t->arena, mark);
    }
    buffer_destroy(buf);
    LOG("test: ok: column mapping, 3000 fuzzed lines, %D checks", checks);
    return 1;
}

// ---------------------------------------------------------------------------
// View: commands, scrolling, fuzz

// The built-in defaults that matter to view commands (word bytes, fsync on save).
static const Settings test_settings = { .font_size = 12, .line_height = 100, .tab_width = 4, .fsync_on_save = 1 };

typedef struct TestView {
    Buffer *buf;
    View *view;
    Echo echo;
    CommandContext ctx;
} TestView;

// `marked` is the text with '|' at each cursor (in order; the first is the primary).
static b32 test_view_open(Test *t, TestView *tv, const char *marked, i32 rows, i32 cols) {
    String8 m = str8_cstr(marked);
    u8 *text = PUSH_ARRAY(&t->arena, u8, m.len);
    i64 n = 0, cursors[16], count = 0;
    for (i64 i = 0; i < m.len; i++) {
        if (m.data[i] == '|' && count < ARRAY_COUNT(cursors)) cursors[count++] = n;
        else text[n++] = m.data[i];
    }
    memset(tv, 0, sizeof(*tv));
    tv->buf = buffer_create(STR8_LIT("test.c"));
    TEST_CHECK(t, tv->buf, "view: buffer_create failed");
    buffer_replace(tv->buf, 0, 0, str8(text, n));
    tv->buf->modified = 0;
    tv->view = view_create(&t->arena, tv->buf);
    tv->view->rows = rows;
    tv->view->cols = cols;
    view_set_point(tv->view, &tv->view->cursors[0], count ? cursors[0] : 0);
    for (i64 k = 1; k < count; k++) view_add_cursor(tv->view, cursors[k]);
    tv->ctx.view = tv->view;
    tv->ctx.echo = &tv->echo;
    tv->ctx.settings = &test_settings;
    view_ensure_visible(tv->view);
    return 1;
}

static b32 test_view_close(Test *t, TestView *tv) {
    view_destroy(tv->view);
    i64 live = tv->buf->marker_live;
    buffer_destroy(tv->buf);
    TEST_CHECK(t, live == 0, "view: %D markers still live after view_destroy", live);
    return 1;
}

// As the app does for a key: the echo area is cleared, then the command runs.
static void test_view_run(TestView *tv, const Command *cmd) {
    echo_clear(&tv->echo);
    view_run_command(&tv->ctx, cmd);
}

// The text with '|' at every cursor, NUL-terminated, in the test arena.
static char *test_view_marked(Test *t, TestView *tv) {
    i64 size = buffer_size(tv->buf);
    View *v = tv->view;
    char *out = PUSH_ARRAY(&t->arena, char, size + v->cursor_count + 1);
    i64 n = 0;
    for (i64 i = 0; i <= size; i++) {
        for (i32 k = 0; k < v->cursor_count; k++) if (view_point(v, &v->cursors[k]) == i) out[n++] = '|';
        if (i < size) out[n++] = (char)buffer_byte(tv->buf, i);
    }
    out[n] = 0;
    return out;
}

static b32 test_cstr_equal(const char *a, const char *b) {
    return str8_equal(str8_cstr(a), str8_cstr(b));
}

typedef struct TestMotion {
    const char *before;      // '|' marks point
    const Command *cmds[6];  // run in order, NULL-terminated
    const char *after;
    const char *message;     // the echo area after the last command; NULL = empty
} TestMotion;

static b32 test_motions(Test *t) {
#define F &CMD_FORWARD_CHAR
#define B &CMD_BACKWARD_CHAR
#define N &CMD_NEXT_LINE
#define P &CMD_PREVIOUS_LINE
#define A &CMD_MOVE_BEGINNING_OF_LINE
#define E &CMD_MOVE_END_OF_LINE
#define WF &CMD_FORWARD_WORD
#define WB &CMD_BACKWARD_WORD
#define PF &CMD_FORWARD_PARAGRAPH
#define PB &CMD_BACKWARD_PARAGRAPH
#define BOB &CMD_BEGINNING_OF_BUFFER
#define EOB &CMD_END_OF_BUFFER
    static const TestMotion cases[] = {
        // Characters, multi-byte, limits.
        { "|abc", { F }, "a|bc", NULL },
        { "abc|", { F }, "abc|", "End of buffer" },
        { "|abc", { B }, "|abc", "Beginning of buffer" },
        { "a|\xC4\x9F" "b", { F }, "a\xC4\x9F|b", NULL },
        { "a\xC4\x9F|b", { B }, "a|\xC4\x9F" "b", NULL },
        { "a|\xF0\x9F\x98\x80" "b", { F, F }, "a\xF0\x9F\x98\x80" "b|", NULL },
        { "a|\x80\xFF" "b", { F, F }, "a\x80\xFF|b", NULL },
        { "ab\n|cd", { B }, "ab|\ncd", NULL },
        // Lines: goal column, shorter lines, tabs, control characters, limits.
        { "ab|c\nde", { N }, "abc\nde|", NULL },
        { "abcdef|gh\nab\nabcdefgh", { N, N }, "abcdefgh\nab\nabcdef|gh", NULL },
        { "abcdef|gh\nab\nx", { N, P }, "abcdef|gh\nab\nx", NULL },
        { "abcdefgh\nab\nabcdef|gh", { P, P }, "abcdef|gh\nab\nabcdefgh", NULL },
        { "abcdef|gh\nab\nabcdefgh", { N, B, N }, "abcdefgh\nab\na|bcdefgh", NULL }, // the goal is retaken after C-b
        { "abcde|f\n\tx", { N }, "abcdef\n\tx|", NULL },
        { "ab|cdef\n\tx", { N }, "abcdef\n|\tx", NULL },     // column 2 is in the first half of the tab
        { "abc|def\n\tx", { N }, "abcdef\n\t|x", NULL },     // column 3 is in its second half
        { "x\t|y\nabcdefgh", { N }, "x\ty\nabcd|efgh", NULL },
        { "a\rb|c\nabcdefg", { N }, "a\rbc\nabcd|efg", NULL }, // ^M is two columns
        { "abcd|e\na\rbc", { N }, "abcde\na\rb|c", NULL },
        { "abc|\na\rbc", { N }, "abc\na\r|bc", NULL },         // column 3 is the second half of ^M
        { "ab|\na\rbc", { N }, "ab\na|\rbc", NULL },           // column 2 is its first half
        { "ab\nc|d", { N }, "ab\ncd|", "End of buffer" },
        { "ab\ncd\n|", { N }, "ab\ncd\n|", "End of buffer" },
        { "a|b\ncd", { P }, "|ab\ncd", "Beginning of buffer" },
        { "ab|c\n\nxyz", { N }, "abc\n|\nxyz", NULL },
        { "ab|c\n\nxyz", { N, N }, "abc\n\nxy|z", NULL },
        { "a\xC4\x9F\xC4\x9F|x\nabcdef", { N }, "a\xC4\x9F\xC4\x9F" "x\nabc|def", NULL },
        { "abc|def\na\xC4\x9F\xC4\x9F\xC4\x9F" "b", { N }, "abcdef\na\xC4\x9F\xC4\x9F|\xC4\x9F" "b", NULL },
        // Line ends.
        { "x\nab|c\nd", { A }, "x\n|abc\nd", NULL },
        { "x\nab|c\nd", { E }, "x\nabc|\nd", NULL },
        { "x\nabc|", { E }, "x\nabc|", NULL },
        // Words: underscore is not a word character, digits are, bytes >= 0x80 are letters.
        { "|foo_bar baz", { WF }, "foo|_bar baz", NULL },
        { "|foo_bar baz", { WF, WF }, "foo_bar| baz", NULL },
        { "foo_bar baz|", { WB }, "foo_bar |baz", NULL },
        { "foo_bar baz|", { WB, WB }, "foo_|bar baz", NULL },
        { "|  x1y2 z", { WF }, "  x1y2| z", NULL },
        { "|\xC4\x9F\xC3\xBC\xC5\x9F abc", { WF }, "\xC4\x9F\xC3\xBC\xC5\x9F| abc", NULL },
        { "abc \xC4\x9F\xC3\xBC|", { WB }, "abc |\xC4\x9F\xC3\xBC", NULL },
        { "abc|", { WF }, "abc|", NULL },
        { "|abc", { WB }, "|abc", NULL },
        { "a|b; (c)", { WF, WF }, "ab; (c|)", NULL },
        // Paragraphs: separators are lines of spaces and tabs.
        { "|a\nb\n\nc\nd", { PF }, "a\nb\n|\nc\nd", NULL },
        { "|a\nb\n\nc\nd", { PF, PF }, "a\nb\n\nc\nd|", NULL },
        { "|a\n  \t\nb", { PF }, "a\n|  \t\nb", NULL },
        { "a\nb\n\nc\nd|", { PB }, "a\nb\n|\nc\nd", NULL },
        { "a\nb\n\nc\nd|", { PB, PB }, "|a\nb\n\nc\nd", NULL },
        { "a\n\n|\n\nb", { PF }, "a\n\n\n\nb|", NULL },
        { "a\n\n\n|b\nc", { PB }, "a\n\n|\nb\nc", NULL },      // Emacs: right after an empty line, stop there
        { "a\n  \n|b", { PB }, "|a\n  \nb", NULL },             // ... but not after a line of spaces (Emacs too)
        { "a\n  \nb|c", { PB }, "a\n|  \nbc", NULL },
        { "a\n\n\nb\nc|", { PF }, "a\n\n\nb\nc|", NULL },
        // Buffer ends.
        { "ab\nc|d", { BOB }, "|ab\ncd", NULL },
        { "a|b\ncd", { EOB }, "ab\ncd|", NULL },
    };
#undef F
#undef B
#undef N
#undef P
#undef A
#undef E
#undef WF
#undef WB
#undef PF
#undef PB
#undef BOB
#undef EOB
    for (i64 i = 0; i < ARRAY_COUNT(cases); i++) {
        u64 mark = arena_pos(&t->arena);
        const TestMotion *c = &cases[i];
        TestView tv;
        if (!test_view_open(t, &tv, c->before, 10, 40)) return 0;
        for (i32 k = 0; k < 6 && c->cmds[k]; k++) test_view_run(&tv, c->cmds[k]);
        char *got = test_view_marked(t, &tv);
        String8 msg = str8(tv.echo.text, tv.echo.len);
        b32 ok = test_cstr_equal(got, c->after);
        b32 msg_ok = c->message ? str8_equal(msg, str8_cstr(c->message)) : msg.len == 0;
        if (!test_view_close(t, &tv)) return 0;
        TEST_CHECK(t, ok, "motion case %D (%s...): got \"%s\", expected \"%s\"", i, c->cmds[0]->name, got, c->after);
        TEST_CHECK(t, msg_ok, "motion case %D (%s...): message \"%S\", expected \"%s\"", i, c->cmds[0]->name, msg,
                   c->message ? c->message : "");
        arena_pop_to(&t->arena, mark);
    }
    LOG("test: ok: motion table, %D cases", ARRAY_COUNT(cases));
    return 1;
}

// Scrolling, recentering and horizontal scrolling on a 100-line buffer, 10 rows x 40 columns.
static b32 test_scrolling(Test *t) {
    u8 *text = PUSH_ARRAY(&t->arena, u8, 1024);
    i64 n = 0;
    text[n++] = '|';
    for (i32 l = 0; l < 100; l++) {
        String8 num = str8_fmt(&t->arena, "%d", l);
        memcpy(text + n, num.data, (size_t)num.len);
        n += num.len;
        if (l < 99) text[n++] = '\n';
    }
    text[n] = 0;
    TestView tv;
    if (!test_view_open(t, &tv, (char *)text, 10, 40)) return 0;
    View *v = tv.view;
    Buffer *buf = tv.buf;
#define LINE() buffer_line_of(buf, view_point(v, &v->cursors[0]))
#define EXPECT(cmd, top, line, msg)                                                                              \
    do {                                                                                                         \
        test_view_run(&tv, cmd);                                                                                 \
        String8 m = str8(tv.echo.text, tv.echo.len);                                                             \
        TEST_CHECK(t, view_top_line(v) == (top) && LINE() == (line) && str8_equal(m, STR8_LIT(msg)),            \
                   "scrolling: %s: top %D, line %D, message \"%S\"; expected top %D, line %D, \"%s\"", (cmd)->name, \
                   view_top_line(v), LINE(), m, (i64)(top), (i64)(line), msg);                                  \
    } while (0)
    EXPECT(&CMD_SCROLL_UP_COMMAND, 8, 8, "");    // point dragged to the start of the top line
    EXPECT(&CMD_SCROLL_UP_COMMAND, 16, 16, "");
    EXPECT(&CMD_SCROLL_DOWN_COMMAND, 8, 16, ""); // still visible: stays
    EXPECT(&CMD_SCROLL_DOWN_COMMAND, 0, 9, "");  // dragged to the bottom line
    EXPECT(&CMD_SCROLL_DOWN_COMMAND, 0, 9, "Beginning of buffer");
    TEST_CHECK(t, view_point(v, &v->cursors[0]) == buffer_line_start(buf, 9), "scrolling: dragged point is not at a line start");
    view_goto_line_column(v, 50, 0);
    view_ensure_visible(v);
    tv.ctx.last_command = NULL;
    TEST_CHECK(t, view_top_line(v) == 45, "scrolling: goto line 50 gave top %D, expected 45 (centered)", view_top_line(v));
    EXPECT(&CMD_RECENTER_TOP_BOTTOM, 45, 50, "");
    EXPECT(&CMD_RECENTER_TOP_BOTTOM, 50, 50, "");
    EXPECT(&CMD_RECENTER_TOP_BOTTOM, 41, 50, "");
    EXPECT(&CMD_RECENTER_TOP_BOTTOM, 45, 50, "");
    EXPECT(&CMD_NEXT_LINE, 45, 51, "");
    EXPECT(&CMD_NEXT_LINE, 45, 52, "");
    EXPECT(&CMD_NEXT_LINE, 45, 53, "");
    EXPECT(&CMD_NEXT_LINE, 45, 54, "");
    EXPECT(&CMD_NEXT_LINE, 50, 55, "");          // off screen: recentered
    EXPECT(&CMD_RECENTER_TOP_BOTTOM, 50, 55, ""); // a new cycle starts at the center
    EXPECT(&CMD_END_OF_BUFFER, 92, 99, "");      // (recenter -3)
    EXPECT(&CMD_SCROLL_UP_COMMAND, 99, 99, "");  // the top may reach the last line
    EXPECT(&CMD_SCROLL_UP_COMMAND, 99, 99, "End of buffer");
    EXPECT(&CMD_BEGINNING_OF_BUFFER, 0, 0, "");
    EXPECT(&CMD_END_OF_BUFFER, 92, 99, "");
    EXPECT(&CMD_PREVIOUS_LINE, 92, 98, "");
    EXPECT(&CMD_END_OF_BUFFER, 92, 99, "");      // visible: no recenter
#undef EXPECT
#undef LINE
    if (!test_view_close(t, &tv)) return 0;

    // Horizontal: a 200-column line in 40 columns.
    u8 *line = PUSH_ARRAY(&t->arena, u8, 256);
    line[0] = '|';
    for (i32 i = 1; i <= 200; i++) line[i] = (u8)('a' + i % 26);
    memcpy(line + 201, "\nshort", 7);
    if (!test_view_open(t, &tv, (char *)line, 10, 40)) return 0;
    v = tv.view;
    test_view_run(&tv, &CMD_MOVE_END_OF_LINE);
    TEST_CHECK(t, v->left_col == 180, "horizontal: end of a 200-column line gave left %D, expected 180", v->left_col);
    test_view_run(&tv, &CMD_NEXT_LINE);
    TEST_CHECK(t, v->left_col == 0, "horizontal: a short line gave left %D, expected 0", v->left_col);
    view_goto_line_column(v, 0, 39);
    view_ensure_visible(v);
    TEST_CHECK(t, v->left_col == 0, "horizontal: column 39 of 40 gave left %D, expected 0", v->left_col);
    view_goto_line_column(v, 0, 40);
    view_ensure_visible(v);
    TEST_CHECK(t, v->left_col == 20, "horizontal: column 40 of 40 gave left %D, expected 20", v->left_col);
    view_goto_line_column(v, 0, 50);
    view_ensure_visible(v);
    TEST_CHECK(t, v->left_col == 20, "horizontal: column 50 (visible) moved left to %D", v->left_col);
    if (!test_view_close(t, &tv)) return 0;
    LOG("test: ok: scrolling, recentering, horizontal scrolling");
    return 1;
}

// Every cursor on a boundary inside the buffer, the primary one visible, a valid scroll position.
static b32 test_view_valid(Test *t, TestView *tv, const char *when) {
    View *v = tv->view;
    Buffer *buf = tv->buf;
    i64 size = buffer_size(buf);
    for (i32 k = 0; k < v->cursor_count; k++) {
        i64 p = view_point(v, &v->cursors[k]);
        TEST_CHECK(t, p >= 0 && p <= size && buffer_snap_char(buf, p) == p, "%s: cursor %d at %D (size %D) is not a boundary",
                   when, k, p, size);
    }
    i64 top_pos = buffer_marker_get(buf, v->top);
    i64 top = buffer_line_of(buf, top_pos);
    TEST_CHECK(t, top_pos == buffer_line_start(buf, top), "%s: top at %D is not a line start", when, top_pos);
    TEST_CHECK(t, top <= buffer_line_count(buf) - 1, "%s: top line %D past the last line", when, top);
    i64 p = view_point(v, &v->cursors[0]);
    i64 line = buffer_line_of(buf, p);
    TEST_CHECK(t, line >= top && line < top + v->rows, "%s: point line %D outside [%D, %D)", when, line, top, top + v->rows);
    i64 col = view_column_of(buf, p);
    i64 w = p < size && view_is_control(buffer_byte(buf, p)) ? 2 : 1;
    TEST_CHECK(t, v->left_col >= 0 && col >= v->left_col && col + w <= v->left_col + v->cols,
               "%s: point column %D (width %D) outside [%D, %D)", when, col, w, v->left_col, v->left_col + v->cols);
    return 1;
}

// Driver probes: each bumps the goal column of the cursor it is given, so the counts show
// which cursors a command ran for.
static void test_bump_cursor(CommandContext *ctx) { ctx->cursor->goal_col++; }
static const Command TEST_CMD_EACH = { "test-each", test_bump_cursor, 0 };
static const Command TEST_CMD_ONCE = { "test-once", test_bump_cursor, COMMAND_ONCE };

// Three cursors, every command through view_run_command.
static b32 test_multi_cursor(Test *t) {
    TestView tv;
    if (!test_view_open(t, &tv, "a|b\nc|d\ne|f", 10, 40)) return 0;
    View *v = tv.view;
    TEST_CHECK(t, v->cursor_count == 3, "cursors: %d cursors, expected 3", v->cursor_count);
    struct { const Command *cmd; u32 codepoint; const char *after; } steps[] = {
        { &CMD_SELF_INSERT, 'X', "aX|b\ncX|d\neX|f" },
        { &CMD_DELETE_BACKWARD_CHAR, 0, "a|b\nc|d\ne|f" },
        { &CMD_DELETE_CHAR, 0, "a|\nc|\ne|" },
        { &CMD_SELF_INSERT, 0x11F, "a\xC4\x9F|\nc\xC4\x9F|\ne\xC4\x9F|" },
        { &CMD_BACKWARD_CHAR, 0, "a|\xC4\x9F\nc|\xC4\x9F\ne|\xC4\x9F" },
        { &CMD_NEWLINE, 0, "a\n|\xC4\x9F\nc\n|\xC4\x9F\ne\n|\xC4\x9F" },
        { &CMD_DELETE_BACKWARD_CHAR, 0, "a|\xC4\x9F\nc|\xC4\x9F\ne|\xC4\x9F" },
        { &CMD_FORWARD_CHAR, 0, "a\xC4\x9F|\nc\xC4\x9F|\ne\xC4\x9F|" },
        { &CMD_DELETE_CHAR, 0, "a\xC4\x9F|c\xC4\x9F|e\xC4\x9F|" },
    };
    for (i64 i = 0; i < ARRAY_COUNT(steps); i++) {
        tv.ctx.codepoint = steps[i].codepoint;
        test_view_run(&tv, steps[i].cmd);
        char *got = test_view_marked(t, &tv);
        TEST_CHECK(t, test_cstr_equal(got, steps[i].after), "cursors: step %D (%s): got \"%s\", expected \"%s\"", i,
                   steps[i].cmd->name, got, steps[i].after);
    }
    // The driver: a per-cursor command runs once for every cursor, a COMMAND_ONCE command once
    // in all, with the primary cursor.
    for (i32 k = 0; k < 3; k++) v->cursors[k].goal_col = 0;
    test_view_run(&tv, &TEST_CMD_EACH);
    test_view_run(&tv, &TEST_CMD_ONCE);
    TEST_CHECK(t, v->cursors[0].goal_col == 2 && v->cursors[1].goal_col == 1 && v->cursors[2].goal_col == 1,
               "cursors: driver ran per-cursor / once commands %D, %D, %D times (expected 2, 1, 1)", v->cursors[0].goal_col,
               v->cursors[1].goal_col, v->cursors[2].goal_col);
    TEST_CHECK(t, tv.ctx.last_command == &TEST_CMD_ONCE && tv.ctx.cursor == NULL, "cursors: driver did not update the context");
    if (!test_view_close(t, &tv)) return 0;
    LOG("test: ok: three cursors (self-insert, deletes, newline, motion) and the command driver");
    return 1;
}

// Read-only buffers refuse every edit with a message; save-buffer messages.
static b32 test_view_edit_limits(Test *t) {
    TestView tv;
    if (!test_view_open(t, &tv, "ab|c\nd", 10, 40)) return 0;
    tv.buf->read_only = 1;
    const Command *edits[] = { &CMD_SELF_INSERT, &CMD_NEWLINE, &CMD_DELETE_BACKWARD_CHAR, &CMD_DELETE_CHAR };
    tv.ctx.codepoint = 'x';
    for (i64 i = 0; i < ARRAY_COUNT(edits); i++) {
        test_view_run(&tv, edits[i]);
        char *got = test_view_marked(t, &tv);
        String8 msg = str8(tv.echo.text, tv.echo.len);
        TEST_CHECK(t, test_cstr_equal(got, "ab|c\nd") && !tv.buf->modified && tv.buf->edit_count == 1,
                   "read-only: %s changed the buffer: \"%s\"", edits[i]->name, got);
        TEST_CHECK(t, str8_equal(msg, STR8_LIT("Buffer is read-only: test.c")), "read-only: %s said \"%S\"", edits[i]->name, msg);
    }
    tv.buf->read_only = 0;

    // Limits of the deletes.
    view_set_point(tv.view, &tv.view->cursors[0], 0);
    test_view_run(&tv, &CMD_DELETE_BACKWARD_CHAR);
    TEST_CHECK(t, str8_equal(str8(tv.echo.text, tv.echo.len), STR8_LIT("Beginning of buffer")), "delete-backward-char at 0: no message");
    view_set_point(tv.view, &tv.view->cursors[0], buffer_size(tv.buf));
    test_view_run(&tv, &CMD_DELETE_CHAR);
    TEST_CHECK(t, str8_equal(str8(tv.echo.text, tv.echo.len), STR8_LIT("End of buffer")), "delete-char at the end: no message");
    TEST_CHECK(t, !tv.buf->modified, "the refused deletes modified the buffer");

    // save-buffer: nothing to save; no file; a real save.
    test_view_run(&tv, &CMD_SAVE_BUFFER);
    TEST_CHECK(t, str8_equal(str8(tv.echo.text, tv.echo.len), STR8_LIT("(No changes need to be saved)")), "save-buffer unmodified: \"%S\"",
               str8(tv.echo.text, tv.echo.len));
    tv.ctx.codepoint = 'y';
    test_view_run(&tv, &CMD_SELF_INSERT);
    test_view_run(&tv, &CMD_SAVE_BUFFER);
    TEST_CHECK(t, str8_equal(str8(tv.echo.text, tv.echo.len), STR8_LIT("Cannot save test.c: buffer is not visiting a file")),
               "save-buffer without a file: \"%S\"", str8(tv.echo.text, tv.echo.len));
    String8 path = test_path(t, "view_save.txt", "");
    os_file_delete(path);
    buffer_set_path(tv.buf, os_full_path(&t->arena, path));
    test_view_run(&tv, &CMD_SAVE_BUFFER);
    String8 want = str8_fmt(&t->arena, "Wrote %S", tv.buf->path), written;
    TEST_CHECK(t, str8_equal(str8(tv.echo.text, tv.echo.len), want), "save-buffer: \"%S\"", str8(tv.echo.text, tv.echo.len));
    TEST_CHECK(t, !tv.buf->modified && test_read_file(t, path, &written) && str8_equal(written, STR8_LIT("abc\ndy")),
               "save-buffer: the file does not hold the text");
    os_file_delete(path);
    if (!test_view_close(t, &tv)) return 0;
    LOG("test: ok: read-only buffer refuses edits with a message; delete limits; save-buffer messages");
    return 1;
}

#define TEST_VIEW_FUZZ_OPS 20000

static b32 test_view_fuzz(Test *t, u64 seed) {
    t->rng = seed ^ 0x76696577ull;
    static const char *pieces[] = {
        "a", "b", "x", "_", "1", " ", " ", " ", "\t", "\n", "\n", "\n\n", "\n  \n", "\xC4\x9F", "\xE2\x82\xAC",
        "\xF0\x9F\x98\x80", "\r", "\x01", "\x7F", "\x80", "\xFF", "\xE2\x82", "word", "longer_identifier",
    };
    static const Command *commands[] = {
        &CMD_FORWARD_CHAR, &CMD_BACKWARD_CHAR, &CMD_NEXT_LINE, &CMD_PREVIOUS_LINE, &CMD_NEXT_LINE, &CMD_PREVIOUS_LINE,
        &CMD_MOVE_BEGINNING_OF_LINE, &CMD_MOVE_END_OF_LINE, &CMD_FORWARD_WORD, &CMD_BACKWARD_WORD,
        &CMD_FORWARD_PARAGRAPH, &CMD_BACKWARD_PARAGRAPH, &CMD_BEGINNING_OF_BUFFER, &CMD_END_OF_BUFFER,
        &CMD_SCROLL_UP_COMMAND, &CMD_SCROLL_DOWN_COMMAND, &CMD_RECENTER_TOP_BOTTOM,
        &CMD_SELF_INSERT, &CMD_SELF_INSERT, &CMD_NEWLINE, &CMD_DELETE_BACKWARD_CHAR, &CMD_DELETE_CHAR,
    };
    static const u32 typed[] = { 'a', 'Z', ' ', '\t', '_', 0x11F, 0x20AC, 0x1F600 };
    u8 *text = PUSH_ARRAY(&t->arena, u8, KB(64));
    i64 n = 0;
    while (n < (i64)KB(16)) {
        String8 piece = str8_cstr(pieces[test_below(t, ARRAY_COUNT(pieces))]);
        if (test_below(t, 200) == 0) for (i32 i = 0; i < 300; i++) text[n++] = 'L'; // a long line
        memcpy(text + n, piece.data, (size_t)piece.len);
        n += piece.len;
    }
    text[n] = 0;
    TestView tv;
    if (!test_view_open(t, &tv, "", 20, 60)) return 0;
    buffer_replace(tv.buf, 0, 0, str8(text, n));
    View *v = tv.view;
    i64 commands_run = 0, edits = 0;
    for (i32 op = 0; op < TEST_VIEW_FUZZ_OPS; op++) {
        i64 r = test_below(t, 100);
        i64 size = buffer_size(tv.buf);
        const char *what;
        if (r < 60) {
            const Command *cmd = commands[test_below(t, ARRAY_COUNT(commands))];
            tv.ctx.codepoint = typed[test_below(t, ARRAY_COUNT(typed))];
            test_view_run(&tv, cmd);
            what = cmd->name;
            commands_run++;
        } else {
            tv.ctx.last_command = NULL; // as the app does for anything that is not a command
            if (r < 72) { // an edit elsewhere (another view, a program): markers keep up
                i64 start = buffer_snap_char(tv.buf, test_below(t, size + 1));
                i64 end = buffer_snap_char(tv.buf, MIN(start + test_below(t, size > (i64)KB(32) ? 400 : 40), size));
                end = MAX(start, end);
                i64 len = 0;
                if (size < (i64)KB(32)) {
                    for (i64 k = test_below(t, 4); k > 0; k--) {
                        String8 piece = str8_cstr(pieces[test_below(t, ARRAY_COUNT(pieces))]);
                        memcpy(text + len, piece.data, (size_t)piece.len);
                        len += piece.len;
                    }
                }
                buffer_replace(tv.buf, start, end, str8(text, len));
                what = "edit";
                edits++;
            } else if (r < 80) {
                view_scroll_lines(v, test_below(t, 61) - 30);
                what = "wheel";
            } else if (r < 88) {
                view_set_point_at(v, test_below(t, v->rows + 2), v->left_col + test_below(t, v->cols + 4));
                what = "click";
            } else if (r < 93) {
                v->rows = (i32)(1 + test_below(t, 40));
                v->cols = (i32)(4 + test_below(t, 100));
                what = "resize";
            } else {
                view_goto_line_column(v, test_below(t, buffer_line_count(tv.buf) + 5), test_below(t, 120));
                what = "goto";
            }
            view_ensure_visible(v); // every frame does this after the layout
        }
        char when[96];
        test_cstr(when, sizeof(when), "view fuzz op %d (seed 0x%X, %s)", op, seed, what);
        if (!test_view_valid(t, &tv, when)) {
            test_view_close(t, &tv);
            return 0;
        }
    }
    if (!test_view_close(t, &tv)) return 0;
    LOG("test: ok: view fuzz, %d ops (%D commands, %D edits), seed 0x%X", TEST_VIEW_FUZZ_OPS, commands_run, edits, seed);
    return 1;
}

// ---------------------------------------------------------------------------
// Commands

// Every command is found by its own name, names are unique, and unknown names are refused.
static b32 test_commands(Test *t) {
    i32 n = command_count();
    for (i32 i = 0; i < n; i++) {
        const Command *c = command_at(i);
        String8 name = str8_cstr(c->name);
        TEST_CHECK(t, name.len > 0 && c->fn, "commands: entry %d has no name or function", i);
        TEST_CHECK(t, command_find(name) == c, "commands: '%s' does not find itself (duplicate name?)", c->name);
    }
    TEST_CHECK(t, !command_find(STR8_LIT("no-such-command")) && !command_find(STR8_LIT("")) &&
                  !command_find(STR8_LIT("forward-cha")), "commands: unknown names must not be found");
    LOG("test: ok: commands, %d in the table", n);
    return 1;
}

// ---------------------------------------------------------------------------
// Keys: kbd notation and chords

static b32 test_chord_is(KeyChord c, const char *expected) {
    u8 text[KEY_SEQ_TEXT_CAP];
    i64 n = key_chord_print(c, text, sizeof(text));
    return str8_equal(str8(text, n), str8_cstr(expected));
}

static b32 test_kbd(Test *t, u64 seed) {
    // Parse, then print the canonical form.
    static const char *canonical[][2] = {
        { "C-x C-s", "C-x C-s" }, { "M-<", "M-<" }, { "M->", "M->" }, { "C-<home>", "C-<home>" },
        { "S-C-a", "C-S-a" }, { "C-A", "C-S-a" }, { "M-SPC", "M-SPC" }, { "SPC", "SPC" }, { "C--", "C--" },
        { "-", "-" }, { "<pageup>", "<prior>" }, { "<pagedown> RET", "<next> RET" }, { "C-x 4 f", "C-x 4 f" },
        { "M-{", "M-{" }, { "C-\xc5\x9f", "C-\xc5\x9f" }, { "C-\xc5\x9e", "C-S-\xc5\x9f" }, { "TAB DEL ESC", "TAB DEL ESC" },
        { "<f24>", "<f24>" }, { "  C-x   o ", "C-x o" }, { "C-M-S-<delete>", "C-M-S-<delete>" }, { "<return>", "RET" },
        { "M-S-<up>", "M-S-<up>" }, { "C-x C-=", "C-x C-=" }, { "C-c ,", "C-c ," }, { "A", "A" }, { "<", "<" },
        { "C-<", "C-<" }, { "C-h k", "C-h k" }, { "\xce\xa9", "\xce\xa9" }, { "M-\xd0\x96", "M-S-\xd0\xb6" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(canonical); i++) {
        KeySeq seq;
        const char *error = NULL;
        TEST_CHECK(t, key_seq_parse(str8_cstr(canonical[i][0]), &seq, &error), "kbd: '%s' rejected: %s", canonical[i][0], error);
        u8 text[KEY_SEQ_TEXT_CAP];
        String8 printed = str8(text, key_seq_print(&seq, text, sizeof(text)));
        TEST_CHECK(t, str8_equal(printed, str8_cstr(canonical[i][1])), "kbd: '%s' printed as '%S', expected '%s'",
                   canonical[i][0], printed, canonical[i][1]);
    }
    static const char *bad[] = {
        "", "   ", "C-", "M-", "C-xy", "<foo>", "C-C-x", "a b c d e", "C-S-/", "S-a", "S-1", "C-\x01", "C-\xff",
        "<>", "M-S-1", "x-", "C-x C-", "\x7f", "C-\xc2\x85",
    };
    for (i32 i = 0; i < ARRAY_COUNT(bad); i++) {
        KeySeq seq;
        const char *error = NULL;
        TEST_CHECK(t, !key_seq_parse(str8_cstr(bad[i]), &seq, &error) && error, "kbd: '%s' must be rejected", bad[i]);
    }

    // Round trip: random sequences print and parse back to the same chords.
    static const u32 chars[] = { 'a', 'z', 'A', 'Q', '0', '9', '/', '-', '<', '>', '{', ';', ',', '=', '+', '@', ' ',
                                 0xE7, 0xC7, 0x15F, 0x15E, 0x131, 0x130, 0x3B1, 0x416, 0x6F22, 0x1F600 };
    t->rng = seed ^ 0x6b62;
    for (i32 iter = 0; iter < 20000; iter++) {
        KeySeq seq = { .len = 1 + (i32)test_below(t, KEY_SEQ_MAX) };
        for (i32 k = 0; k < seq.len; k++) {
            u32 cm = (test_below(t, 2) ? CHORD_CTRL : 0) | (test_below(t, 2) ? CHORD_META : 0);
            KeyChord c;
            if (test_below(t, 3) == 0) {
                Key key;
                do key = (Key)test_below(t, KEY_COUNT); while (!key_is_named(key));
                c = CHORD_NAMED | (u32)key | cm | (test_below(t, 2) ? CHORD_SHIFT : 0);
            } else {
                u32 cp = chars[test_below(t, ARRAY_COUNT(chars))];
                u32 lower = key_letter_lower(cp);
                if (cm && lower) c = lower | cm | (test_below(t, 2) ? CHORD_SHIFT : 0);
                else c = cp | cm;
            }
            seq.chords[k] = c;
        }
        u8 text[KEY_SEQ_TEXT_CAP];
        String8 printed = str8(text, key_seq_print(&seq, text, sizeof(text)));
        KeySeq back;
        const char *error = NULL;
        TEST_CHECK(t, key_seq_parse(printed, &back, &error), "kbd: round trip: '%S' rejected: %s", printed, error);
        b32 same = back.len == seq.len;
        for (i32 k = 0; same && k < seq.len; k++) same = back.chords[k] == seq.chords[k];
        TEST_CHECK(t, same, "kbd: round trip: '%S' parsed to different chords", printed);
    }
    LOG("test: ok: kbd notation, %d canonical forms, %d rejections, 20000 round trips", (i32)ARRAY_COUNT(canonical),
        (i32)ARRAY_COUNT(bad));
    return 1;
}

// Chords from (named key, character, modifiers), as the platform delivers KEY_DOWN.
static b32 test_chords(Test *t) {
    static const struct { Key key; u32 cp; u32 mods; const char *chord; } cases[] = {
        { KEY_A, 'A', MOD_CTRL | MOD_SHIFT, "C-S-a" },   // letters keep Shift
        { KEY_A, 'a', MOD_CTRL, "C-a" },
        { KEY_A, 'A', MOD_CTRL, "C-a" },                 // Caps Lock: no Shift held
        { KEY_A, 'a', MOD_CTRL | MOD_SHIFT, "C-S-a" },   // Caps Lock and Shift
        { KEY_SLASH, '?', MOD_CTRL | MOD_SHIFT, "C-?" }, // symbols absorb Shift
        { KEY_7, '/', MOD_CTRL | MOD_SHIFT, "C-/" },     // "/" is Shift+7 on Turkish Q
        { KEY_2, '@', MOD_CTRL | MOD_SHIFT, "C-@" },
        { KEY_7, '{', MOD_ALT, "M-{" },                  // AltGr+7 with Left Alt (Turkish Q)
        { KEY_Q, '@', MOD_CTRL | MOD_ALT, "C-M-@" },     // AltGr+q with Right Ctrl and Left Alt
        { KEY_SPACE, ' ', MOD_CTRL, "C-SPC" },
        { KEY_LEFT, 0, MOD_SHIFT, "S-<left>" },          // named keys keep every modifier
        { KEY_F5, 0, 0, "<f5>" },
        { KEY_ENTER, 0, MOD_CTRL, "C-RET" },
        { KEY_TAB, 0, MOD_SHIFT, "S-TAB" },
        { KEY_BACKSPACE, 0, MOD_ALT, "M-DEL" },
        { KEY_HOME, 0, MOD_CTRL | MOD_ALT | MOD_SHIFT, "C-M-S-<home>" },
        { KEY_SEMICOLON, 0x15E, MOD_CTRL | MOD_SHIFT, "C-S-\xc5\x9f" }, // Ş on Turkish Q
        { KEY_I, 0x130, MOD_CTRL | MOD_SHIFT, "C-S-i" },                // İ: default case mapping
        { KEY_NONE, 'x', MOD_ALT, "M-x" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        KeyChord c = 0;
        TEST_CHECK(t, key_chord_from_event(cases[i].key, cases[i].cp, cases[i].mods, &c) && test_chord_is(c, cases[i].chord),
                   "chords: case %d (expected %s)", i, cases[i].chord);
    }
    // Not chords: plain or shifted characters (their text event follows), keys without a character.
    static const struct { Key key; u32 cp; u32 mods; } none[] = {
        { KEY_A, 'a', 0 }, { KEY_A, 'A', MOD_SHIFT }, { KEY_SPACE, ' ', 0 }, { KEY_7, '{', 0 },
        { KEY_OEM_102, 0, MOD_CTRL }, { KEY_NONE, 0, MOD_ALT },
    };
    for (i32 i = 0; i < ARRAY_COUNT(none); i++) {
        KeyChord c;
        TEST_CHECK(t, !key_chord_from_event(none[i].key, none[i].cp, none[i].mods, &c), "chords: case %d must not be a chord", i);
    }
    TEST_CHECK(t, test_chord_is(key_chord_from_text('o'), "o") && test_chord_is(key_chord_from_text(' '), "SPC") &&
                  test_chord_is(key_chord_from_text('O'), "O"), "chords: text events");
    LOG("test: ok: chords from key events, %d cases", (i32)(ARRAY_COUNT(cases) + ARRAY_COUNT(none)));
    return 1;
}

// ---------------------------------------------------------------------------
// Keys: keymaps and the sequence state machine

static i32 test_bind(Keymap *map, const char *keys, const Command *command) {
    KeySeq seq;
    const char *error;
    if (!key_seq_parse(str8_cstr(keys), &seq, &error)) return -100;
    return keymap_bind(map, &seq, command);
}

typedef struct TestKeys {
    KeyInput in;
    Keymap *stack[2];
    i32 count;
    KeyResult r;
} TestKeys;

static KeyResultKind test_key(TestKeys *k, Key key, u32 cp, u32 mods) {
    Event e = { .kind = EVENT_KEY_DOWN, .key = key, .codepoint = cp, .mods = mods };
    key_input_feed(&k->in, k->stack, k->count, &e, &k->r);
    return k->r.kind;
}

static KeyResultKind test_text(TestKeys *k, u32 cp) {
    Event e = { .kind = EVENT_TEXT, .codepoint = cp };
    key_input_feed(&k->in, k->stack, k->count, &e, &k->r);
    return k->r.kind;
}

// A plain character typed: its KEY_DOWN (ignored by the keymap), then its text.
static KeyResultKind test_type(TestKeys *k, u32 cp) {
    if (test_key(k, KEY_NONE, cp, 0) != KEY_RESULT_IGNORED) return k->r.kind;
    return test_text(k, cp);
}

static b32 test_seq_is(KeySeq *seq, const char *expected) {
    u8 text[KEY_SEQ_TEXT_CAP];
    return str8_equal(str8(text, key_seq_print(seq, text, sizeof(text))), str8_cstr(expected));
}

static b32 test_key_input(Test *t) {
    Keymap *global = PUSH_STRUCT(&t->arena, Keymap);
    Keymap *context = PUSH_STRUCT(&t->arena, Keymap);
    global->name = "global";
    context->name = "context";
    i32 binds = test_bind(global, "C-f", &CMD_FORWARD_CHAR) + test_bind(global, "<left>", &CMD_BACKWARD_CHAR) +
                test_bind(global, "C-x C-s", &CMD_SAVE_BUFFER) + test_bind(global, "C-x o", &CMD_NEXT_LINE) +
                test_bind(global, "C-g", &CMD_KEYBOARD_QUIT) + test_bind(global, "ESC", &CMD_KEYBOARD_QUIT) +
                test_bind(global, "TAB", &CMD_SELF_INSERT) + test_bind(global, "C-S-a", &CMD_BEGINNING_OF_BUFFER) +
                test_bind(global, "C-a", &CMD_MOVE_BEGINNING_OF_LINE);
    TEST_CHECK(t, binds == 0 && global->count == 9, "keys: binding the test map (%d, %d)", binds, global->count);
    TestKeys k = { .stack = { global }, .count = 1 };

    // Prefix, then completion; the pending prefix is the sequence so far.
    TEST_CHECK(t, test_key(&k, KEY_X, 'x', MOD_CTRL) == KEY_RESULT_PREFIX && test_seq_is(&k.r.seq, "C-x"), "keys: C-x is a prefix");
    TEST_CHECK(t, test_key(&k, KEY_S, 's', MOD_CTRL) == KEY_RESULT_COMMAND && k.r.command == &CMD_SAVE_BUFFER &&
                  k.in.pending.len == 0, "keys: C-x C-s runs save-buffer");
    // Undefined resets the state.
    TEST_CHECK(t, test_key(&k, KEY_X, 'x', MOD_CTRL) == KEY_RESULT_PREFIX, "keys: C-x again");
    TEST_CHECK(t, test_key(&k, KEY_Q, 'q', MOD_CTRL) == KEY_RESULT_UNDEFINED && test_seq_is(&k.r.seq, "C-x C-q") &&
                  k.in.pending.len == 0, "keys: C-x C-q is undefined");
    TEST_CHECK(t, test_key(&k, KEY_F, 'f', MOD_CTRL) == KEY_RESULT_COMMAND && k.r.command == &CMD_FORWARD_CHAR,
               "keys: C-f after an undefined sequence");
    // keyboard-quit cancels a prefix; alone it is a command.
    test_key(&k, KEY_X, 'x', MOD_CTRL);
    TEST_CHECK(t, test_key(&k, KEY_G, 'g', MOD_CTRL) == KEY_RESULT_QUIT && k.in.pending.len == 0, "keys: C-x C-g quits");
    test_key(&k, KEY_X, 'x', MOD_CTRL);
    TEST_CHECK(t, test_key(&k, KEY_ESCAPE, 0, 0) == KEY_RESULT_QUIT, "keys: C-x ESC quits");
    TEST_CHECK(t, test_key(&k, KEY_G, 'g', MOD_CTRL) == KEY_RESULT_COMMAND && k.r.command == &CMD_KEYBOARD_QUIT,
               "keys: C-g alone runs keyboard-quit");
    // Shift-translation: an unbound chord with Shift is looked up without it.
    TEST_CHECK(t, test_key(&k, KEY_LEFT, 0, MOD_SHIFT) == KEY_RESULT_COMMAND && k.r.command == &CMD_BACKWARD_CHAR &&
                  k.r.shift_translated, "keys: S-<left> is shift-translated to backward-char");
    TEST_CHECK(t, test_key(&k, KEY_F, 'F', MOD_CTRL | MOD_SHIFT) == KEY_RESULT_COMMAND && k.r.command == &CMD_FORWARD_CHAR &&
                  k.r.shift_translated, "keys: C-S-f is shift-translated to forward-char");
    TEST_CHECK(t, test_key(&k, KEY_LEFT, 0, 0) == KEY_RESULT_COMMAND && !k.r.shift_translated, "keys: <left> is not translated");
    TEST_CHECK(t, test_key(&k, KEY_A, 'A', MOD_CTRL | MOD_SHIFT) == KEY_RESULT_COMMAND && k.r.command == &CMD_BEGINNING_OF_BUFFER &&
                  !k.r.shift_translated, "keys: a bound C-S-a is not translated");
    TEST_CHECK(t, test_key(&k, KEY_X, 'X', MOD_CTRL | MOD_SHIFT) == KEY_RESULT_PREFIX && k.r.shift_translated &&
                  test_key(&k, KEY_S, 's', MOD_CTRL) == KEY_RESULT_COMMAND && k.r.command == &CMD_SAVE_BUFFER,
               "keys: C-S-x is translated to the C-x prefix");
    TEST_CHECK(t, test_key(&k, KEY_Q, 'Q', MOD_CTRL | MOD_SHIFT) == KEY_RESULT_UNDEFINED && test_seq_is(&k.r.seq, "C-S-q"),
               "keys: C-S-q is undefined, reported as typed");
    // Text from a consumed KEY_DOWN is dropped up to the next KEY_DOWN.
    TEST_CHECK(t, test_key(&k, KEY_F, 'f', MOD_CTRL) == KEY_RESULT_COMMAND && test_text(&k, 'f') == KEY_RESULT_DROPPED &&
                  test_text(&k, 'g') == KEY_RESULT_DROPPED, "keys: text after a consumed KEY_DOWN is dropped");
    TEST_CHECK(t, test_key(&k, KEY_A, 'a', 0) == KEY_RESULT_IGNORED && test_text(&k, 'a') == KEY_RESULT_SELF_INSERT &&
                  k.r.command == &CMD_SELF_INSERT && k.r.codepoint == 'a', "keys: a plain character self-inserts");
    TEST_CHECK(t, test_text(&k, 0x15F) == KEY_RESULT_SELF_INSERT && k.r.codepoint == 0x15F,
               "keys: composed text without a KEY_DOWN (dead keys, IME) self-inserts");
    // A plain character as the second key.
    TEST_CHECK(t, test_key(&k, KEY_X, 'x', MOD_CTRL) == KEY_RESULT_PREFIX && test_type(&k, 'o') == KEY_RESULT_COMMAND &&
                  k.r.command == &CMD_NEXT_LINE, "keys: C-x o through a text event");
    TEST_CHECK(t, test_key(&k, KEY_X, 'x', MOD_CTRL) == KEY_RESULT_PREFIX && test_type(&k, 'z') == KEY_RESULT_UNDEFINED &&
                  test_seq_is(&k.r.seq, "C-x z"), "keys: C-x z is undefined");
    // TAB bound to self-insert-command carries a tab.
    TEST_CHECK(t, test_key(&k, KEY_TAB, 0, 0) == KEY_RESULT_COMMAND && k.r.command == &CMD_SELF_INSERT && k.r.codepoint == '\t',
               "keys: TAB inserts a tab");
    // describe-key: the next complete sequence is described, not run.
    k.in.describe = 1;
    TEST_CHECK(t, test_key(&k, KEY_X, 'x', MOD_CTRL) == KEY_RESULT_PREFIX && test_key(&k, KEY_S, 's', MOD_CTRL) == KEY_RESULT_DESCRIBE &&
                  k.r.command == &CMD_SAVE_BUFFER && !k.in.describe, "keys: describe C-x C-s");
    k.in.describe = 1;
    TEST_CHECK(t, test_key(&k, KEY_Q, 'q', MOD_CTRL) == KEY_RESULT_DESCRIBE && !k.r.command, "keys: describe an undefined C-q");
    k.in.describe = 1;
    TEST_CHECK(t, test_type(&k, 'a') == KEY_RESULT_DESCRIBE && k.r.command == &CMD_SELF_INSERT, "keys: describe a");
    TEST_CHECK(t, test_key(&k, KEY_F, 'f', MOD_CTRL) == KEY_RESULT_COMMAND, "keys: describe ends after one sequence");

    // Two maps sharing a prefix: prefixes merge across maps, the first exact match wins.
    TEST_CHECK(t, test_bind(context, "C-x k", &CMD_END_OF_BUFFER) == 0 && test_bind(context, "C-f", &CMD_NEXT_LINE) == 0 &&
                  test_bind(context, "C-c x", &CMD_PREVIOUS_LINE) == 0, "keys: binding the context map");
    TestKeys k2 = { .stack = { context, global }, .count = 2 };
    TEST_CHECK(t, test_key(&k2, KEY_X, 'x', MOD_CTRL) == KEY_RESULT_PREFIX && test_key(&k2, KEY_K, 'k', 0) == KEY_RESULT_IGNORED &&
                  test_text(&k2, 'k') == KEY_RESULT_COMMAND && k2.r.command == &CMD_END_OF_BUFFER, "keys: C-x k from the context map");
    TEST_CHECK(t, test_key(&k2, KEY_X, 'x', MOD_CTRL) == KEY_RESULT_PREFIX && test_key(&k2, KEY_S, 's', MOD_CTRL) == KEY_RESULT_COMMAND &&
                  k2.r.command == &CMD_SAVE_BUFFER, "keys: C-x C-s from the global map is not hidden by the context's C-x k");
    TEST_CHECK(t, test_key(&k2, KEY_X, 'x', MOD_CTRL) == KEY_RESULT_PREFIX && test_type(&k2, 'o') == KEY_RESULT_COMMAND &&
                  k2.r.command == &CMD_NEXT_LINE, "keys: C-x o from the global map");
    TEST_CHECK(t, test_key(&k2, KEY_X, 'x', MOD_CTRL) == KEY_RESULT_PREFIX && test_type(&k2, 'q') == KEY_RESULT_UNDEFINED,
               "keys: C-x q is undefined in both maps");
    TEST_CHECK(t, test_key(&k2, KEY_F, 'f', MOD_CTRL) == KEY_RESULT_COMMAND && k2.r.command == &CMD_NEXT_LINE,
               "keys: C-f bound in both maps: the context map wins");
    TEST_CHECK(t, test_key(&k2, KEY_C, 'c', MOD_CTRL) == KEY_RESULT_PREFIX && test_type(&k2, 'x') == KEY_RESULT_COMMAND &&
                  k2.r.command == &CMD_PREVIOUS_LINE, "keys: a prefix that exists only in the context map");

    // Binding conflicts: a binding removes the bindings it is a prefix of, or that are its prefix.
    TEST_CHECK(t, test_bind(global, "C-x", &CMD_END_OF_BUFFER) == 2 && global->count == 8, "keys: C-x replaces C-x C-s and C-x o");
    TEST_CHECK(t, test_bind(global, "C-x C-s", &CMD_SAVE_BUFFER) == 1 && global->count == 8, "keys: C-x C-s replaces C-x");
    TEST_CHECK(t, test_bind(global, "C-x C-s", &CMD_FORWARD_CHAR) == 0 && global->count == 8, "keys: rebinding replaces in place");
    TEST_CHECK(t, test_bind(global, "C-x C-s", NULL) == 0 && global->count == 7, "keys: none removes");
    TEST_CHECK(t, test_bind(global, "C-x C-s", NULL) == 0 && global->count == 7, "keys: removing an unbound sequence");
    LOG("test: ok: keymaps and the key sequence state machine");
    return 1;
}

// ---------------------------------------------------------------------------
// Hot reload: a real directory watch in build\tmp\watch, config_poll with the time passed in

static b32 test_write(Test *t, String8 path, const char *text) {
    TEST_CHECK(t, os_write_file(path, str8_cstr(text)), "reload: cannot write %S", path);
    return 1;
}

static b32 test_hot_reload(Test *t) {
    String8 dir = str8_fmt(&t->arena, "%S\\watch", t->tmp_dir);
    String8 path = str8_fmt(&t->arena, "%S\\teal.conf", dir);
    String8 temp = str8_fmt(&t->arena, "%S\\teal.conf.tmp", dir);
    String8 other = str8_fmt(&t->arena, "%S\\other.txt", dir);
    TEST_CHECK(t, os_make_dir(dir), "reload: cannot create %S", dir);
    if (!test_write(t, path, "[settings]\ntab_width = 5\n")) return 0;
    Config *c = PUSH_STRUCT(&t->arena, Config);
    ConfigSource src = { .path = path };
    u64 now = 1000000;
    TEST_CHECK(t, config_poll(&src, c, &t->arena, 1, now) == CONFIG_POLL_LOADED && c->settings.tab_width == 5 &&
                  config_wait_ms(&src, now) == CONFIG_WAIT_INFINITE, "reload: first load");
    TEST_CHECK(t, config_poll(&src, c, &t->arena, 0, now) == CONFIG_POLL_UNCHANGED, "reload: an unchanged file is not read again");

    OsWatch w = os_watch_dir(dir);
    TEST_CHECK(t, w, "reload: cannot watch %S", dir);
    while (os_dev_watch_wait(w, 50)) {} // nothing pending
    // Written in place.
    if (!test_write(t, path, "[settings]\ntab_width = 12\n")) return 0;
    TEST_CHECK(t, os_dev_watch_wait(w, 2000), "reload: no notification after a write");
    TEST_CHECK(t, config_poll(&src, c, &t->arena, 0, now) == CONFIG_POLL_LOADED && c->settings.tab_width == 12,
               "reload: the new value after a write (tab_width %d)", c->settings.tab_width);
    // Another file in the directory: a notification, but nothing to load.
    while (os_dev_watch_wait(w, 50)) {}
    if (!test_write(t, other, "x")) return 0;
    TEST_CHECK(t, os_dev_watch_wait(w, 2000) && config_poll(&src, c, &t->arena, 0, now) == CONFIG_POLL_UNCHANGED,
               "reload: another file changing does not reload");
    // Saved through a temporary file and a rename; while the file is gone, nothing changes.
    while (os_dev_watch_wait(w, 50)) {}
    if (!test_write(t, temp, "[settings]\ntab_width = 7\n# saved by rename\n")) return 0;
    os_file_delete(path);
    TEST_CHECK(t, os_dev_watch_wait(w, 2000) && config_poll(&src, c, &t->arena, 0, now) == CONFIG_POLL_UNCHANGED,
               "reload: a missing file (mid-rename) counts as unchanged");
    TEST_CHECK(t, os_file_replace(path, temp) == OS_FILE_OK, "reload: rename failed");
    TEST_CHECK(t, os_dev_watch_wait(w, 2000), "reload: no notification after the rename");
    TEST_CHECK(t, config_poll(&src, c, &t->arena, 0, now) == CONFIG_POLL_LOADED && c->settings.tab_width == 7,
               "reload: the new value after a save by rename (tab_width %d)", c->settings.tab_width);

    // A sharing violation is retried every 100 ms; it loads once the file is released.
    while (os_dev_watch_wait(w, 50)) {}
    if (!test_write(t, path, "[settings]\ntab_width = 9\n")) return 0;
    OsFile lock;
    TEST_CHECK(t, os_dev_lock_file(path, &lock) == OS_FILE_OK, "reload: cannot lock %S", path);
    TEST_CHECK(t, os_dev_watch_wait(w, 2000), "reload: no notification before the locked read");
    TEST_CHECK(t, config_poll(&src, c, &t->arena, 0, now) == CONFIG_POLL_RETRY && src.status == OS_FILE_SHARING_VIOLATION &&
                  config_wait_ms(&src, now) == CONFIG_RETRY_MS, "reload: locked: a retry in 100 ms");
    TEST_CHECK(t, config_poll(&src, c, &t->arena, 0, now + 50000) == CONFIG_POLL_UNCHANGED && config_wait_ms(&src, now + 50000) == 50,
               "reload: the retry is not due after 50 ms");
    TEST_CHECK(t, config_poll(&src, c, &t->arena, 0, now + 100000) == CONFIG_POLL_RETRY && src.attempts == 2,
               "reload: still locked at the second attempt");
    os_file_close(lock);
    TEST_CHECK(t, config_poll(&src, c, &t->arena, 0, now + 200000) == CONFIG_POLL_LOADED && c->settings.tab_width == 9 &&
                  config_wait_ms(&src, now + 200000) == CONFIG_WAIT_INFINITE, "reload: loaded once released, no more waiting");
    // Locked for good: 5 attempts in all, then a failure, and the wait is infinite again.
    if (!test_write(t, path, "[settings]\ntab_width = 3\n# a different size\n")) return 0;
    TEST_CHECK(t, os_dev_lock_file(path, &lock) == OS_FILE_OK, "reload: cannot lock %S", path);
    ConfigPoll r = CONFIG_POLL_UNCHANGED;
    i32 reads = 0;
    for (i32 i = 0; i < 10 && r != CONFIG_POLL_FAILED; i++) {
        r = config_poll(&src, c, &t->arena, 0, now + (u64)i * CONFIG_RETRY_MS * 1000);
        reads += r != CONFIG_POLL_UNCHANGED;
    }
    TEST_CHECK(t, r == CONFIG_POLL_FAILED && reads == CONFIG_RETRY_ATTEMPTS && src.status == OS_FILE_SHARING_VIOLATION &&
                  config_wait_ms(&src, now) == CONFIG_WAIT_INFINITE, "reload: gave up after %d reads (expected %d)", reads,
               (i32)CONFIG_RETRY_ATTEMPTS);
    os_file_close(lock);
    TEST_CHECK(t, config_poll(&src, c, &t->arena, 0, now) == CONFIG_POLL_LOADED && c->settings.tab_width == 3,
               "reload: the next poll after the failure loads the file");
    os_unwatch(w);
    TEST_CHECK(t, os_dev_watch_count() == 0, "reload: the watch was not released");
    LOG("test: ok: hot reload: write, other file, save by rename, sharing violation retried and given up");
    return 1;
}

// ---------------------------------------------------------------------------
// The buffer list and *Messages*

static b32 test_buffer_list(Test *t) {
    BufferList list;
    buffer_list_init(&list);
    Buffer *a = buffer_create(STR8_LIT("a.c")), *b = buffer_create(STR8_LIT("b.c"));
    TEST_CHECK(t, a && b, "buffer list: buffer_create failed");
    buffer_set_path(a, STR8_LIT("C:\\Work\\a.c"));
    buffer_set_path(b, STR8_LIT("C:\\Work\\b.c"));
    // 200 numbered lines in each, loaded before they are listed (as files are).
    for (i32 i = 0; i < 200; i++) {
        u8 line[16];
        i64 n = fmt_buf(line, sizeof(line), "line %d\n", i);
        buffer_replace(a, buffer_size(a), buffer_size(a), str8(line, n));
        buffer_replace(b, buffer_size(b), buffer_size(b), str8(line, n));
    }
    buffer_list_add(&list, a);
    buffer_list_add(&list, b);
    TEST_CHECK(t, buffer_list_find_path(&list, STR8_LIT("c:\\work\\B.C")) == b && !buffer_list_find_path(&list, STR8_LIT("C:\\Work\\c.c")) &&
                  buffer_list_index(&list, b) == 1, "buffer list: lookup by path, case-insensitive");

    Arena arena = arena_create(MB(1));
    View *v = view_create(&arena, a);
    v->rows = 20;
    v->cols = 40;
    view_goto_line_column(v, 150, 3);
    view_ensure_visible(v);
    i64 point = view_point(v, &v->cursors[0]), top = view_top_line(v);
    view_add_cursor(v, 5); // a second cursor collapses on a switch
    view_switch_buffer(v, &list, b);
    TEST_CHECK(t, v->buffer == b && view_point(v, &v->cursors[0]) == 0 && view_top_line(v) == 0 && v->cursor_count == 1,
               "buffer list: a buffer never shown starts at the top");
    view_goto_line_column(v, 40, 0);
    view_ensure_visible(v);
    i64 point_b = view_point(v, &v->cursors[0]);
    // An edit in a, while it is not shown, moves its saved point like any marker.
    buffer_replace(a, 0, 0, STR8_LIT("xyz"));
    view_switch_buffer(v, &list, a);
    TEST_CHECK(t, view_point(v, &v->cursors[0]) == point + 3 && view_top_line(v) == top && v->cursor_count == 1,
               "buffer list: switching back restores point and scroll (point %D, expected %D)", view_point(v, &v->cursors[0]), point + 3);
    view_switch_buffer(v, &list, b);
    TEST_CHECK(t, view_point(v, &v->cursors[0]) == point_b, "buffer list: and again for b");
    view_destroy(v);
    os_release(arena.base);
    TEST_CHECK(t, buffer_list_destroy(&list) == 0, "buffer list: markers or buffers leaked");

    // *Messages*: read-only, appended through inhibit_read_only, capped at ECHO_LOG_LINES.
    Buffer *log = buffer_create(STR8_LIT("*Messages*"));
    TEST_CHECK(t, log, "messages: buffer_create failed");
    log->read_only = 1;
    Echo echo = { .log = log };
    for (i32 i = 0; i < ECHO_LOG_LINES + 500; i++) echo_message(&echo, "message %d", i);
    echo_set(&echo, STR8_LIT("C-x-"));
    String8 first = buffer_line(log, &t->arena, 0), last = buffer_line(log, &t->arena, ECHO_LOG_LINES - 1);
    TEST_CHECK(t, buffer_line_count(log) == ECHO_LOG_LINES + 1 && str8_equal(first, STR8_LIT("message 500")) &&
                  str8_equal(last, STR8_LIT("message 1499")) && buffer_line_end(log, ECHO_LOG_LINES) == buffer_size(log),
               "messages: %D lines, first '%S', last '%S'", buffer_line_count(log), first, last);
    TEST_CHECK(t, log->read_only && !log->inhibit_read_only && !log->modified && !buffer_replace(log, 0, 0, STR8_LIT("x")),
               "messages: stays read-only");
    TEST_CHECK(t, buffer_destroy(log), "messages: buffer_destroy failed");
    LOG("test: ok: buffer list (restore point and scroll, lookup by path), *Messages* capped at %d lines", (i32)ECHO_LOG_LINES);
    return 1;
}

// ---------------------------------------------------------------------------
// Config

static const Command *test_binding(Config *c, const char *keys) {
    KeySeq seq;
    const char *error;
    if (!key_seq_parse(str8_cstr(keys), &seq, &error)) return NULL;
    return keymap_get(&c->global, &seq);
}

static b32 test_config(Test *t, u64 seed) {
    Config *c = PUSH_STRUCT(&t->arena, Config);

    // The built-in defaults: clean, and the CLAUDE.md theme.
    config_init(c);
    config_parse(c, &t->arena, config_default_text(), STR8_LIT("<built-in>"));
    Settings *s = &c->settings;
    Theme *th = &c->theme;
    TEST_CHECK(t, c->errors == 0 && c->warnings == 0, "config: defaults: %d errors, %d warnings", c->errors, c->warnings);
    TEST_CHECK(t, str8_equal(str8(s->font, s->font_len), STR8_LIT("Consolas")) && s->font_size == 12.0f &&
                  s->line_height == 100 && s->render_mode == FB_RENDER_NATURAL_SYMMETRIC && s->tab_width == 4 &&
                  !s->underscore_is_word && s->fsync_on_save, "config: default settings");
    TEST_CHECK(t, th->background == 0x072626 && th->text == 0xd3b58d && th->cursor == 0x90ee90 && th->selection == 0x0000ff &&
                  th->comment == 0x3fdf1f && th->string == 0x0fdfaf && th->keyword == 0xffffff && th->number == 0x7ad0c6 &&
                  th->type == 0x8cde94 && th->variable == 0xc1d1e3, "config: default colors (the CLAUDE.md theme)");
    TEST_CHECK(t, test_binding(c, "C-x C-s") == &CMD_SAVE_BUFFER && test_binding(c, "M-<") == &CMD_BEGINNING_OF_BUFFER &&
                  test_binding(c, "C-m") == &CMD_NEWLINE && test_binding(c, "C-j") == &CMD_NEWLINE &&
                  test_binding(c, "ESC") == &CMD_KEYBOARD_QUIT && test_binding(c, "TAB") == &CMD_SELF_INSERT &&
                  test_binding(c, "<next>") == &CMD_SCROLL_UP_COMMAND && test_binding(c, "C-x C-c") == &CMD_SAVE_BUFFERS_KILL_TERMINAL &&
                  test_binding(c, "C-x <right>") == &CMD_NEXT_BUFFER && test_binding(c, "C-x <left>") == &CMD_PREVIOUS_BUFFER &&
                  test_binding(c, "C-x C-+") == &CMD_TEXT_SCALE_INCREASE && test_binding(c, "C-x C-=") == &CMD_TEXT_SCALE_INCREASE &&
                  test_binding(c, "C-x C--") == &CMD_TEXT_SCALE_DECREASE && test_binding(c, "C-x C-0") == &CMD_TEXT_SCALE_RESET &&
                  test_binding(c, "C-h k") == &CMD_DESCRIBE_KEY && test_binding(c, "C-c ,") == &CMD_OPEN_CONFIG &&
                  test_binding(c, "C-c r") == &CMD_RELOAD_CONFIG, "config: default bindings");

    // Parse time of the built-in config (about as large as a full user file), for the log.
    u64 t0 = os_time_us();
    for (i32 i = 0; i < 1000; i++) {
        u64 mark = arena_pos(&t->arena);
        Config *tmp = PUSH_STRUCT(&t->arena, Config);
        config_init(tmp);
        config_parse(tmp, &t->arena, config_default_text(), STR8_LIT("<built-in>"));
        arena_pop_to(&t->arena, mark);
    }
    LOG("test: config: parsing the built-in config (%D bytes, %d bindings) takes %U ns", config_default_text().len,
        c->global.count, (os_time_us() - t0));

    // A valid user file on top: only what it names changes.
    config_parse(c, &t->arena, STR8_LIT("# mine\n\n[settings]\nfont_size = 10.5\ntab_width=8\n  underscore_is_word = true  \r\n"
                                        "font = Courier New\n[colors]\nbackground = #102030\n[keys]\n"
                                        "C-x C-s   forward-char\n=  newline\nC-f none\nC-q C-q C-q C-q backward-char\n"),
                 STR8_LIT("user.conf"));
    TEST_CHECK(t, c->errors == 0 && c->warnings == 0, "config: valid user file: %d errors, %d warnings", c->errors, c->warnings);
    TEST_CHECK(t, s->font_size == 10.5f && s->tab_width == 8 && s->underscore_is_word && s->fsync_on_save &&
                  s->line_height == 100 && str8_equal(str8(s->font, s->font_len), STR8_LIT("Courier New")),
               "config: user settings over the defaults");
    TEST_CHECK(t, th->background == 0x102030 && th->text == 0xd3b58d, "config: user color over the defaults");
    TEST_CHECK(t, test_binding(c, "C-x C-s") == &CMD_FORWARD_CHAR && test_binding(c, "=") == &CMD_NEWLINE &&
                  !test_binding(c, "C-f") && test_binding(c, "C-b") == &CMD_BACKWARD_CHAR &&
                  test_binding(c, "C-q C-q C-q C-q") == &CMD_BACKWARD_CHAR, "config: user bindings, none, the defaults kept");

    // Every kind of error and warning, with its line number.
    config_init(c);
    String8 bad = STR8_LIT(
        "stray line\n"                        // 1
        "[settings]\n"                        // 2
        "font_size = big\n"                   // 3
        "nosuch = 1\n"                        // 4
        "tab_width\n"                         // 5
        "[colors]\n"                          // 6
        "background = 123456\n"               // 7
        "purple = #ffffff\n"                  // 8
        "[keys]\n"                            // 9
        "C-xy forward-char\n"                 // 10
        "C-x C-q no-such-command\n"           // 11
        "C-x\n"                               // 12
        "[bogus]\n"                           // 13
        "skipped silently\n"                  // 14
        "[keys\n"                             // 15
        "[settings]\n"                        // 16
        "line_height = 50\n"                  // 17
        "font_size = 200\n"                   // 18
        "render_mode = fancy\n"               // 19
        "tab_width = 0\n"                     // 20
        "fsync_on_save = yes\n"               // 21
        "[keys]\n"                            // 22
        "C-c q forward-char\n"                // 23
        "C-c q x backward-char\n"             // 24
        "font = x\n");                        // 25: a key sequence "font =", command "x"
    config_parse(c, &t->arena, bad, STR8_LIT("t.conf"));
    static const char *expected[] = {
        "t.conf:1: line outside a section ([settings], [colors] or [keys])",
        "t.conf:3: font_size: 'big' is not a number",
        "t.conf:4: unknown setting 'nosuch'",
        "t.conf:5: expected name = value",
        "t.conf:7: background: '123456' is not a color (#rrggbb)",
        "t.conf:8: unknown color 'purple'",
        "t.conf:10: bad key sequence 'C-xy': more than one character (separate chords with spaces)",
        "t.conf:11: unknown command 'no-such-command'",
        "t.conf:12: expected a key sequence and a command",
        "t.conf:13: unknown section [bogus]",
        "t.conf:15: bad section header (expected [settings], [colors] or [keys])",
        "t.conf:17: warning: line_height 50 is out of range (80 to 300), using 80",
        "t.conf:18: warning: font_size 200 is out of range (4 to 96), using 96",
        "t.conf:19: render_mode: 'fancy' is not symmetric, natural or classic",
        "t.conf:20: warning: tab_width 0 is out of range (1 to 16), using 1",
        "t.conf:21: fsync_on_save: 'yes' is not true or false",
        "t.conf:24: warning: C-c q x removed 1 binding it conflicts with (one sequence is a prefix of the other)",
        "t.conf:25: bad key sequence 'font =': more than one character (separate chords with spaces)",
    };
    i32 k = 0;
    for (ConfigDiag *d = c->first_diag; d; d = d->next, k++) {
        TEST_CHECK(t, k < ARRAY_COUNT(expected) && str8_equal(d->text, str8_cstr(expected[k])),
                   "config: diagnostic %d is '%S', expected '%s'", k, d->text, k < ARRAY_COUNT(expected) ? expected[k] : "(none)");
    }
    TEST_CHECK(t, k == ARRAY_COUNT(expected) && c->errors == 14 && c->warnings == 4,
               "config: %d diagnostics (%d errors, %d warnings), expected %d", k, c->errors, c->warnings, (i32)ARRAY_COUNT(expected));
    TEST_CHECK(t, c->settings.line_height == 80 && c->settings.font_size == 96.0f && c->settings.tab_width == 1,
               "config: clamped values are applied");
    TEST_CHECK(t, !test_binding(c, "C-c q") && test_binding(c, "C-c q x") == &CMD_BACKWARD_CHAR, "config: the later binding wins");

    // Files: the user file layered over the defaults; a missing file leaves the defaults.
    String8 path = str8_fmt(&t->arena, "%S\\cfg_user.conf", t->tmp_dir);
    String8 user = STR8_LIT("[keys]\nC-x C-s none\n[colors]\ncursor = #ff0000\n");
    TEST_CHECK(t, os_write_file(path, user), "config: writing %S", path);
    OsFileInfo info;
    TEST_CHECK(t, config_load(c, &t->arena, path, &info) == OS_FILE_OK && info.size == user.len && c->errors == 0 &&
                  !test_binding(c, "C-x C-s") && test_binding(c, "C-f") == &CMD_FORWARD_CHAR && c->theme.cursor == 0xff0000 &&
                  c->theme.background == 0x072626 && c->settings.font_size == 12.0f, "config: load layers the user file over the defaults");
    String8 missing = str8_fmt(&t->arena, "%S\\cfg_missing.conf", t->tmp_dir);
    os_file_delete(missing);
    TEST_CHECK(t, config_load(c, &t->arena, missing, &info) == OS_FILE_NOT_FOUND && test_binding(c, "C-x C-s") == &CMD_SAVE_BUFFER &&
                  config_load(c, &t->arena, str8(NULL, 0), &info) == OS_FILE_NOT_FOUND && c->errors == 0,
               "config: no user file gives the defaults");
    TEST_CHECK(t, str8_equal(config_file_name(STR8_LIT("C:\\a\\b\\teal.conf")), STR8_LIT("teal.conf")), "config: file name");

    // Random bytes never crash the parser; diagnostics stay bounded.
    static const u8 alphabet[] = "[]=#\n\n\n \t\r-<>abcxyzCMS0123456789settingscolorskeysfont_size#rrggbb";
    t->rng = seed ^ 0xc0f1;
    u8 *junk = PUSH_ARRAY(&t->arena, u8, 4096);
    i64 diags = 0;
    for (i32 iter = 0; iter < 10000; iter++) {
        i64 len = test_below(t, 400);
        for (i64 i = 0; i < len; i++) {
            junk[i] = test_below(t, 4) ? alphabet[test_below(t, (i64)sizeof(alphabet) - 1)] : (u8)test_below(t, 256);
        }
        if (iter == 9999) { // and one long line of section headers
            len = 4096;
            for (i64 i = 0; i < len; i++) junk[i] = "[keys]\n"[i % 7];
        }
        u64 mark = arena_pos(&t->arena);
        config_init(c);
        config_parse(c, &t->arena, str8(junk, len), STR8_LIT("junk.conf"));
        i64 stored = 0;
        for (ConfigDiag *d = c->first_diag; d; d = d->next) stored++;
        TEST_CHECK(t, stored == MIN((i64)(c->errors + c->warnings), (i64)CONFIG_DIAG_CAP) && c->global.count <= KEYMAP_CAP,
                   "config: fuzz iteration %d: %D diagnostics stored of %d", iter, stored, c->errors + c->warnings);
        diags += c->errors + c->warnings;
        arena_pop_to(&t->arena, mark);
    }

    // The settings reach the view: '_' in words, the tab width.
    Settings u = test_settings;
    u.underscore_is_word = 1;
    TestView tv;
    if (!test_view_open(t, &tv, "|foo_bar baz", 10, 40)) return 0;
    test_view_run(&tv, &CMD_FORWARD_WORD);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "foo|_bar baz"), "config: '_' separates words by default");
    tv.ctx.settings = &u;
    test_view_run(&tv, &CMD_BACKWARD_WORD);
    test_view_run(&tv, &CMD_FORWARD_WORD);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "foo_bar| baz"), "config: underscore_is_word = true");
    tv.buf->tab_width = 8;
    buffer_replace(tv.buf, 0, 0, STR8_LIT("\t"));
    TEST_CHECK(t, view_column_of(tv.buf, 1) == 8 && view_column_of(tv.buf, 2) == 9, "config: tab_width 8");
    if (!test_view_close(t, &tv)) return 0;
    LOG("test: ok: config: defaults, layering, %d diagnostics, files, 10000 random inputs (%D diagnostics), settings in the view",
        (i32)ARRAY_COUNT(expected), diags);
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
    test_columns(&t, seed);
    arena_reset(&t.arena);
    test_motions(&t);
    arena_reset(&t.arena);
    test_scrolling(&t);
    arena_reset(&t.arena);
    test_multi_cursor(&t);
    arena_reset(&t.arena);
    test_view_edit_limits(&t);
    arena_reset(&t.arena);
    test_view_fuzz(&t, seed);
    arena_reset(&t.arena);
    test_commands(&t);
    arena_reset(&t.arena);
    test_kbd(&t, seed);
    arena_reset(&t.arena);
    test_chords(&t);
    arena_reset(&t.arena);
    test_key_input(&t);
    arena_reset(&t.arena);
    test_config(&t, seed);
    arena_reset(&t.arena);
    test_buffer_list(&t);
    arena_reset(&t.arena);
    test_hot_reload(&t);
    arena_reset(&t.arena);

    LOG("test: %s, %d failure(s), %U ms", t.failures ? "FAIL" : "PASS", t.failures, (os_time_us() - t0) / 1000);
    os_release(t.arena.base);
    return t.failures;
}

#endif // TEAL_DEV
