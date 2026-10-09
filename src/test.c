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
        i64 near_pos = cursor + test_below(t, 65) - 32; // CLAMP evaluates its argument more than once
        i64 at = test_below(t, 2) ? CLAMP(near_pos, 0, len) : test_below(t, len + 1);
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

// ---------------------------------------------------------------------------
// Lexer states in the buffer: they follow their lines through every edit and gap move; an edit
// lowers state_valid, raises state_dirty and gives new lines the state of the edited line.

#define TEST_STATE_OPS 3000
#define TEST_STATE_LINES_CAP 20000

typedef struct TestStates {
    u32 *tag;   // per line (tag[0] = 0)
    i64 lines;
    i64 valid, dirty, known;
} TestStates;

static b32 test_states_equal(Test *t, Buffer *buf, TestStates *ref, i32 op) {
    TEST_CHECK(t, buffer_line_count(buf) == ref->lines, "states: op %d: %D lines, expected %D", op, buffer_line_count(buf), ref->lines);
    for (i64 l = 0; l < ref->lines; l++) {
        TEST_CHECK(t, buffer_line_state(buf, l) == ref->tag[l], "states: op %d: line %D has %u, expected %u", op, l,
                   buffer_line_state(buf, l), ref->tag[l]);
    }
    TEST_CHECK(t, buf->state_valid == ref->valid && buf->state_dirty == ref->dirty && buf->state_known == ref->known,
               "states: op %d: valid %D dirty %D known %D, expected %D %D %D", op, buf->state_valid, buf->state_dirty,
               buf->state_known, ref->valid, ref->dirty, ref->known);
    return 1;
}

static b32 test_line_states(Test *t, u64 seed) {
    t->rng = seed ^ 0x7374617465ull;
    Buffer *buf = buffer_create(STR8_LIT("states"));
    TEST_CHECK(t, buf, "states: buffer_create failed");
    TestRef ref = { PUSH_ARRAY(&t->arena, u8, MB(1)), 0, MB(1), 0 };
    u8 *text = PUSH_ARRAY(&t->arena, u8, KB(64));
    String8 init = test_random_text(t, text, KB(8));
    buffer_replace(buf, 0, 0, init);
    test_ref_replace(&ref, 0, 0, init);
    TEST_CHECK(t, buffer_states_memory(buf) == 0 && !buf->line_state, "states: nothing reserved before they are enabled");
    TEST_CHECK(t, buffer_states_enable(buf, 1) && buffer_states_memory(buf) > 0, "states: enable");
    TestStates s = { PUSH_ARRAY(&t->arena, u32, TEST_STATE_LINES_CAP), buffer_line_count(buf), 0, -1, 1 };
    u32 *next = PUSH_ARRAY(&t->arena, u32, TEST_STATE_LINES_CAP);
    u32 tag = 1;
    for (i64 l = 1; l < s.lines; l++) buffer_set_line_state(buf, l, s.tag[l] = tag++);
    s.known = s.lines;
    buf->state_known = s.known;
    i64 big = 0;
    for (i32 op = 0; op < TEST_STATE_OPS; op++) {
        if (test_below(t, 10) == 0) {
            // As the lexer would: some states rewritten, the frontiers moved.
            for (i32 k = 0; k < 20 && s.lines > 1; k++) {
                i64 l = 1 + test_below(t, s.lines - 1);
                buffer_set_line_state(buf, l, s.tag[l] = tag++);
            }
            buf->state_known = s.known = 1 + test_below(t, s.lines);
            buf->state_valid = s.valid = test_below(t, s.known);
            buf->state_dirty = s.dirty = test_below(t, s.lines + 1) - 1;
        }
        // A replace anywhere (it moves the gap there); now and then a large insert that grows the index.
        i64 a = test_ref_snap(&ref, test_below(t, ref.len + 1));
        i64 span = test_below(t, 64);
        i64 b = test_ref_snap(&ref, MIN(a + span, ref.len));
        i64 len = test_below(t, 48);
        if (test_below(t, 200) == 0 && ref.len < KB(400)) {
            len = KB(16) + test_below(t, KB(16));
            big++;
        }
        String8 ins = test_random_text(t, text, len);
        i64 first = test_count_newlines(ref.data, a);
        i64 removed = test_count_newlines(ref.data + a, b - a), inserted = test_count_newlines(ins.data, ins.len);
        if (s.lines - removed + inserted > TEST_STATE_LINES_CAP) continue;
        TEST_CHECK(t, buffer_replace(buf, a, b, ins), "states: op %d: replace failed", op);
        test_ref_replace(&ref, a, b, ins);
        if (a != b || ins.len) {
            // The flat model: lines first+1 .. first+removed go, `inserted` lines with line first's state come.
            i64 n = 0;
            for (i64 l = 0; l <= first; l++) next[n++] = s.tag[l];
            for (i64 l = 0; l < inserted; l++) next[n++] = s.tag[first];
            for (i64 l = first + removed + 1; l < s.lines; l++) next[n++] = s.tag[l];
            memcpy(s.tag, next, (size_t)n * sizeof(u32));
            s.lines = n;
            i64 delta = inserted - removed;
            s.valid = MIN(s.valid, first);
            s.dirty = MAX(s.dirty > first + removed ? s.dirty + delta : s.dirty, first + inserted);
            if (s.known > first + removed) s.known += delta;
            else if (s.known > first) s.known = first + inserted + 1;
        }
        if (!test_states_equal(t, buf, &s, op)) return 0;
    }
    u64 committed = buffer_states_memory(buf);
    TEST_CHECK(t, committed == (u64)buf->nl_cap * sizeof(u32), "states: committed with the index (%U, %D entries)", committed, buf->nl_cap);
    TEST_CHECK(t, buffer_states_enable(buf, 0) && buffer_states_memory(buf) == 0 && buffer_line_state(buf, 1) == 0, "states: disable");
    TEST_CHECK(t, buffer_replace(buf, 0, 0, STR8_LIT("\n\n")), "states: an edit while disabled");
    TEST_CHECK(t, buffer_states_enable(buf, 1) && buf->state_valid == 0 && buf->state_known == 1 && buf->state_dirty == -1,
               "states: enabled again, all untrusted");
    TEST_CHECK(t, buffer_destroy(buf), "states: destroy");
    LOG("test: ok: line states (%d edits, %D large, follow their lines; valid / dirty / known; enable, disable, release)",
        TEST_STATE_OPS, big);
    return 1;
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
static const Settings test_settings = { .font_size = 12, .line_height = 100, .tab_width = 4, .fsync_on_save = 1,
                                        .undo_limit_mb = 64, .transient_mark_mode = 1, .kill_ring_max = 60,
                                        .indent_width = 4 };

typedef struct TestView {
    Buffer *buf;
    View *view;
    Echo echo;
    CommandContext ctx;
    KillRing *kills;
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
    buffer_undo_enable(tv->buf, 0); // the starting text is not an edit: as if loaded
    buffer_undo_enable(tv->buf, 1);
    buffer_mark_saved(tv->buf);
    tv->view = view_create(&t->arena, tv->buf);
    tv->view->rows = rows;
    tv->view->cols = cols;
    view_set_point(tv->view, &tv->view->cursors[0], count ? cursors[0] : 0);
    for (i64 k = 1; k < count; k++) view_add_cursor(tv->view, cursors[k]);
    tv->ctx.view = tv->view;
    tv->ctx.echo = &tv->echo;
    tv->ctx.settings = &test_settings;
    tv->ctx.scratch = &t->arena;
    tv->kills = PUSH_STRUCT(&t->arena, KillRing);
    kill_init(tv->kills, 60);
    tv->ctx.kills = tv->kills;
    view_ensure_visible(tv->view);
    return 1;
}

static b32 test_view_close(Test *t, TestView *tv) {
    kill_destroy(tv->kills);
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
    for (i64 i = 0; i < want.len; i++) if (want.data[i] == '\\') want.data[i] = '/'; // paths are shown with forward slashes
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
// Undo (buffer level)

static b32 test_text_is(Test *t, Buffer *buf, String8 expected) {
    String8 got = buffer_text(buf, &t->arena, 0, buffer_size(buf));
    return str8_equal(got, expected);
}

static b32 test_undo_buffer(Test *t, u64 seed) {
    enum { K = 40 };
    Buffer *buf = buffer_create(STR8_LIT("undo"));
    TEST_CHECK(t, buf, "undo: buffer_create failed");
    String8 original = STR8_LIT("The quick brown fox\njumps over\nthe lazy dog.\n");
    buffer_replace(buf, 0, 0, original);
    buffer_mark_saved(buf); // as if loaded
    buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_NONE, 0, 0);
    // Not this first insert: start the history from the "loaded" text.
    buffer_undo_enable(buf, 0);
    buffer_undo_enable(buf, 1);
    String8 texts[K + 1];
    texts[0] = original;
    i64 points[K + 1];
    t->rng = seed ^ 0x0d0;
    for (i32 k = 1; k <= K; k++) {
        i64 size = buffer_size(buf);
        i64 a = test_below(t, size + 1), b = MIN(size, a + test_below(t, 6));
        a = buffer_snap_char(buf, a);
        b = buffer_snap_char(buf, b);
        points[k] = a;
        buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_NONE, 0, a);
        static const char *inserts[] = { "x", "\n", "yz", "\xc5\x9f\xc4\x9f", "longer text\n" };
        b32 ok = buffer_replace(buf, a, b, str8_cstr(inserts[test_below(t, ARRAY_COUNT(inserts))]));
        if (k % 3 == 0) ok &= buffer_replace(buf, 0, 0, STR8_LIT("#")); // two records in one group
        TEST_CHECK(t, ok, "undo: edit %d refused", k);
        texts[k] = buffer_text(buf, &t->arena, 0, buffer_size(buf));
        texts[k] = str8_copy(&t->arena, texts[k]);
    }
    TEST_CHECK(t, buf->modified, "undo: not modified after edits");
    // N undos in a chain give the text after command K - N; point goes where it was.
    i64 point = 0;
    for (i32 n = 1; n <= K; n++) {
        TEST_CHECK(t, buffer_undo(buf, n > 1, point, &point) == BUFFER_UNDO_DONE, "undo: undo %d failed", n);
        TEST_CHECK(t, test_text_is(t, buf, texts[K - n]) && point == points[K - n + 1],
                   "undo: after %d undos the text is not the text after command %d (point %D, expected %D)", n, K - n, point,
                   points[K - n + 1]);
    }
    TEST_CHECK(t, !buf->modified, "undo: undoing everything left the buffer modified");
    TEST_CHECK(t, buffer_undo(buf, 1, point, &point) == BUFFER_UNDO_NOTHING, "undo: no further undo information expected");
    // Redo walks forward again; the saved state (here the original) clears modified.
    for (i32 n = 1; n <= K; n++) {
        TEST_CHECK(t, buffer_redo(buf, point, &point) == BUFFER_UNDO_DONE && test_text_is(t, buf, texts[n]),
                   "undo: redo %d does not give the text after command %d", n, n);
        TEST_CHECK(t, buf->modified, "undo: modified after redo %d", n);
    }
    TEST_CHECK(t, buffer_redo(buf, point, &point) == BUFFER_UNDO_NOTHING, "undo: no further redo information expected");
    // Saved in the middle: undoing back to it clears modified, past it sets it again.
    buffer_mark_saved(buf);
    buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_NONE, 0, 0);
    buffer_replace(buf, 0, 0, STR8_LIT("after save"));
    TEST_CHECK(t, buf->modified && buffer_undo(buf, 0, 0, &point) == BUFFER_UNDO_DONE && !buf->modified &&
                  buffer_undo(buf, 1, 0, &point) == BUFFER_UNDO_DONE && buf->modified &&
                  buffer_redo(buf, 0, &point) == BUFFER_UNDO_DONE && !buf->modified,
               "undo: modified follows the saved state");
    // Merging: 25 consecutive merging commands are a group of 20 and one of 5.
    String8 before = str8_copy(&t->arena, buffer_text(buf, &t->arena, 0, buffer_size(buf)));
    for (i32 i = 0; i < 25; i++) {
        buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_INSERT, i > 0, buffer_size(buf));
        buffer_replace(buf, buffer_size(buf), buffer_size(buf), STR8_LIT("a"));
    }
    buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_NONE, 0, 0);
    TEST_CHECK(t, buffer_undo(buf, 0, 0, &point) == BUFFER_UNDO_DONE && buffer_size(buf) == before.len + 20,
               "undo: the 5 inserts after the first 20 are one group");
    TEST_CHECK(t, buffer_undo(buf, 1, 0, &point) == BUFFER_UNDO_DONE && test_text_is(t, buf, before),
               "undo: the first 20 inserts are one group");
    // Read-only: nothing is undone.
    buf->read_only = 1;
    TEST_CHECK(t, buffer_undo(buf, 0, 0, &point) == BUFFER_UNDO_FAILED && test_text_is(t, buf, before), "undo: read-only");
    buf->read_only = 0;
    // The limit: old groups are dropped, the last one stays even when it is larger than the limit.
    buffer_undo_set_limit(buf, 4096);
    for (i32 i = 0; i < 100; i++) {
        buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_NONE, 0, 0);
        buffer_replace(buf, 0, 0, STR8_LIT("0123456789012345678901234567890123456789012345678901234567890123456789"));
        buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_NONE, 0, 0);
        buffer_replace(buf, 0, 70, STR8_LIT(""));
    }
    TEST_CHECK(t, buf->undo.log_used <= 4096 && buf->undo.first_id > 0, "undo: the limit did not drop old groups (%U bytes)",
               buf->undo.log_used);
    i32 undone = 0;
    for (b32 chain = 0; buffer_undo(buf, chain, 0, &point) == BUFFER_UNDO_DONE; chain = 1) undone++;
    TEST_CHECK(t, undone > 0 && undone < 200, "undo: %d groups undoable under the limit", undone);
    u8 *big = PUSH_ARRAY(&t->arena, u8, MB(1));
    memset(big, 'b', MB(1));
    buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_NONE, 0, 0);
    buffer_replace(buf, 0, 0, str8(big, MB(1)));
    String8 with_big = str8_copy(&t->arena, buffer_text(buf, &t->arena, 0, buffer_size(buf)));
    buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_NONE, 0, 0);
    buffer_replace(buf, 0, MB(1), STR8_LIT("")); // a record larger than the limit
    TEST_CHECK(t, buf->undo.group_count == 1 && buffer_undo(buf, 0, 0, &point) == BUFFER_UNDO_DONE && test_text_is(t, buf, with_big),
               "undo: the last command stays undoable over the limit");
    // The log's commit follows what it keeps: 8 MB deleted, then a small edit drops that group.
    u8 *eight = PUSH_ARRAY(&t->arena, u8, MB(8));
    memset(eight, 'e', MB(8));
    buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_NONE, 0, 0);
    buffer_replace(buf, 0, 0, str8(eight, MB(8)));
    buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_NONE, 0, 0);
    buffer_replace(buf, 0, MB(8), STR8_LIT(""));
    u64 high = buf->undo.log_committed;
    buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_NONE, 0, 0);
    buffer_replace(buf, 0, 0, STR8_LIT("small"));
    TEST_CHECK(t, high >= MB(8) && buf->undo.log_committed <= buf->undo.log_used + MB(5),
               "undo: the commit shrinks when old groups are dropped (%U KB committed for %U KB)", buf->undo.log_committed / 1024,
               buf->undo.log_used / 1024);
    // Disabled: no log, modified on every edit.
    buffer_undo_enable(buf, 0);
    buffer_mark_saved(buf);
    buffer_replace(buf, 0, 0, STR8_LIT("x"));
    TEST_CHECK(t, buf->modified && buffer_undo(buf, 0, 0, &point) == BUFFER_UNDO_NOTHING, "undo: disabled");
    u64 memory = buffer_undo_memory(buf);
    TEST_CHECK(t, buffer_destroy(buf), "undo: memory not released");
    LOG("test: ok: undo log: %d edits undone and redone, saved state, merging at 20, read-only, limit, disabled (%U bytes committed)",
        (i32)K, memory);
    return 1;
}

// ---------------------------------------------------------------------------
// Undo through the command driver

static void test_type_char(TestView *tv, u32 c) {
    tv->ctx.codepoint = c;
    test_view_run(tv, &CMD_SELF_INSERT);
}

static b32 test_echo_is(TestView *tv, const char *expected) {
    return str8_equal(str8(tv->echo.text, tv->echo.len), str8_cstr(expected));
}

static b32 test_undo_commands(Test *t, u64 seed) {
    TestView tv;
    // The Emacs chain: A, B, undo, undo, another command, undo, undo ends at the text after B.
    if (!test_view_open(t, &tv, "|hello", 10, 40)) return 0;
    test_view_run(&tv, &CMD_NEWLINE);  // A
    test_type_char(&tv, 'Z');          // B
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "\nZ|hello"), "undo: setup");
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "\n|hello") && test_echo_is(&tv, "Undo"), "undo: first undo");
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "|hello") && !tv.buf->modified, "undo: second undo");
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, test_echo_is(&tv, "No further undo information"), "undo: nothing further in the chain");
    test_view_run(&tv, &CMD_FORWARD_CHAR); // another command ends the run
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "\n|hello"), "undo: after another command undo undoes the undos (A back)");
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "\nZ|hello") && tv.buf->modified, "undo: and then B back: the text after B");
    // undo-redo goes forward along the undos.
    test_view_run(&tv, &CMD_FORWARD_CHAR);
    test_view_run(&tv, &CMD_UNDO);
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("hello")), "undo: back to the start");
    test_view_run(&tv, &CMD_UNDO_REDO);
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("\nhello")) && test_echo_is(&tv, "Redo"), "undo-redo: A again");
    test_view_run(&tv, &CMD_UNDO_REDO);
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("\nZhello")), "undo-redo: B again (%s)", test_view_marked(t, &tv));
    test_view_run(&tv, &CMD_UNDO_REDO);
    TEST_CHECK(t, test_echo_is(&tv, "No further redo information"), "undo-redo: nothing further");
    if (!test_view_close(t, &tv)) return 0;

    // K commands then N undos gives the text after command K - N.
    enum { K = 30 };
    if (!test_view_open(t, &tv, "one two\nthree |four\nfive", 10, 40)) return 0;
    char *texts[K + 1];
    texts[0] = test_view_marked(t, &tv);
    static const Command *motions[] = { &CMD_FORWARD_CHAR, &CMD_BACKWARD_WORD, &CMD_NEXT_LINE, &CMD_MOVE_END_OF_LINE };
    t->rng = seed ^ 0x1d0;
    for (i32 k = 1; k <= K; k++) {
        test_view_run(&tv, motions[test_below(t, ARRAY_COUNT(motions))]);
        i64 size = buffer_size(tv.buf);
        switch (test_below(t, 3)) {
        case 0: test_view_run(&tv, &CMD_NEWLINE); break;
        case 1: test_type_char(&tv, "abc\xc5\x9f"[test_below(t, 3)]); break;
        case 2: test_view_run(&tv, size > 4 ? &CMD_DELETE_BACKWARD_CHAR : &CMD_NEWLINE); break;
        }
        if (buffer_size(tv.buf) == size && test_cstr_equal(test_view_marked(t, &tv), texts[k - 1])) { // a refused delete
            test_view_run(&tv, &CMD_NEWLINE);
        }
        texts[k] = test_view_marked(t, &tv);
    }
    for (i32 n = 1; n <= K; n++) {
        test_view_run(&tv, &CMD_UNDO);
        String8 text = buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf));
        String8 expected = str8_cstr(texts[K - n]);
        // Compare the text only (point is where that command started, checked below).
        u8 *plain = PUSH_ARRAY(&t->arena, u8, expected.len);
        i64 m = 0;
        for (i64 i = 0; i < expected.len; i++) if (expected.data[i] != '|') plain[m++] = expected.data[i];
        TEST_CHECK(t, str8_equal(text, str8(plain, m)), "undo: after %d undos the text is not the text after command %d", n, K - n);
    }
    TEST_CHECK(t, !tv.buf->modified, "undo: undoing everything leaves the buffer unmodified");
    if (!test_view_close(t, &tv)) return 0;

    // Merging: groups of exactly 20 self-inserts, and of 20 single deletes.
    if (!test_view_open(t, &tv, "|", 10, 40)) return 0;
    for (i32 i = 0; i < 41; i++) test_type_char(&tv, 'a' + i % 26);
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, buffer_size(tv.buf) == 40, "undo: the 41st insert is a group of its own (size %D)", buffer_size(tv.buf));
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, buffer_size(tv.buf) == 20, "undo: inserts 21-40 are one group of 20 (size %D)", buffer_size(tv.buf));
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, buffer_size(tv.buf) == 0, "undo: inserts 1-20 are one group of 20");
    test_view_run(&tv, &CMD_FORWARD_CHAR);
    test_view_run(&tv, &CMD_UNDO); // the undos undone: 20 back
    test_view_run(&tv, &CMD_UNDO);
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, buffer_size(tv.buf) == 41, "undo: the inserts are back (size %D)", buffer_size(tv.buf));
    test_view_run(&tv, &CMD_MOVE_END_OF_LINE);
    for (i32 i = 0; i < 25; i++) test_view_run(&tv, &CMD_DELETE_BACKWARD_CHAR);
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, buffer_size(tv.buf) == 16 + 5, "undo: deletes 21-25 are one group (size %D)", buffer_size(tv.buf));
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, buffer_size(tv.buf) == 41 && test_cstr_equal(test_view_marked(t, &tv), "abcdefghijklmnopqrstuvwxyzabcdefghijklmno|"),
               "undo: deletes 1-20 are one group, point back at the end");
    if (!test_view_close(t, &tv)) return 0;

    // Point goes back to where the undone command started.
    if (!test_view_open(t, &tv, "|hello", 10, 40)) return 0;
    test_type_char(&tv, 'a');
    test_type_char(&tv, 'b');
    test_view_run(&tv, &CMD_MOVE_END_OF_LINE);
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "|hello"), "undo: point restored to where typing started");
    if (!test_view_close(t, &tv)) return 0;

    // A three-cursor command is one group.
    if (!test_view_open(t, &tv, "a|b|c|", 10, 40)) return 0;
    test_view_run(&tv, &CMD_NEWLINE);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "a\n|b\n|c\n|"), "undo: three cursors, newline");
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "a|b|c|"), "undo: one undo reverts all three cursors' edits");
    if (!test_view_close(t, &tv)) return 0;
    LOG("test: ok: undo commands: the Emacs chain, undo-redo, %d commands undone, merging at 20, point, three cursors", (i32)K);
    return 1;
}

// ---------------------------------------------------------------------------
// Mark and region: shift-select and the region rules of the driver

static void test_run_shift(TestView *tv, const Command *cmd) {
    tv->ctx.shift_translated = 1;
    test_view_run(tv, cmd);
    tv->ctx.shift_translated = 0;
}

// "abc" with the region marked: '[' at the mark when active (']' when only set), '|' at point.
static b32 test_region_is(Test *t, TestView *tv, const char *expected) {
    Cursor *c = &tv->view->cursors[0];
    i64 size = buffer_size(tv->buf), p = view_point(tv->view, c), m = buffer_marker_get(tv->buf, c->mark);
    char *out = PUSH_ARRAY(&t->arena, char, size + 3);
    i64 n = 0;
    for (i64 i = 0; i <= size; i++) {
        if (c->mark_set && i == m) out[n++] = c->mark_active ? '[' : ']';
        if (i == p) out[n++] = '|';
        if (i < size) out[n++] = (char)buffer_byte(tv->buf, i);
    }
    out[n] = 0;
    if (test_cstr_equal(out, expected)) return 1;
    LOG("test: region is '%s', expected '%s'", out, expected);
    return 0;
}

static b32 test_region(Test *t) {
    TestView tv;
    if (!test_view_open(t, &tv, "|abcdef", 10, 40)) return 0;
    // Shift + motion activates the mark at point, unshifted motion ends it.
    test_run_shift(&tv, &CMD_FORWARD_CHAR);
    test_run_shift(&tv, &CMD_FORWARD_CHAR);
    TEST_CHECK(t, test_region_is(t, &tv, "[ab|cdef") && tv.view->cursors[0].mark_shift, "region: S-<right> twice");
    test_run_shift(&tv, &CMD_BACKWARD_CHAR);
    TEST_CHECK(t, test_region_is(t, &tv, "[a|bcdef"), "region: S-<left> keeps the shift mark");
    test_view_run(&tv, &CMD_FORWARD_CHAR);
    TEST_CHECK(t, test_region_is(t, &tv, "]ab|cdef"), "region: an unshifted motion deactivates a shift region");
    // C-SPC then shifted motions extend; an unshifted motion keeps a C-SPC region.
    test_view_run(&tv, &CMD_SET_MARK_COMMAND);
    TEST_CHECK(t, test_echo_is(&tv, "Mark set"), "region: Mark set");
    test_run_shift(&tv, &CMD_FORWARD_CHAR);
    test_view_run(&tv, &CMD_FORWARD_CHAR);
    TEST_CHECK(t, test_region_is(t, &tv, "ab[cd|ef") && !tv.view->cursors[0].mark_shift, "region: C-SPC region extends and stays");
    test_view_run(&tv, &CMD_KEYBOARD_QUIT);
    TEST_CHECK(t, test_region_is(t, &tv, "ab]cd|ef"), "region: C-g deactivates");
    // C-SPC C-SPC: set, then deactivated.
    test_view_run(&tv, &CMD_SET_MARK_COMMAND);
    test_view_run(&tv, &CMD_SET_MARK_COMMAND);
    TEST_CHECK(t, test_region_is(t, &tv, "abcd]|ef") && test_echo_is(&tv, "Mark deactivated"), "region: C-SPC C-SPC");
    // exchange-point-and-mark activates; mark-whole-buffer.
    test_view_run(&tv, &CMD_BACKWARD_CHAR);
    test_view_run(&tv, &CMD_BACKWARD_CHAR);
    test_view_run(&tv, &CMD_EXCHANGE_POINT_AND_MARK);
    TEST_CHECK(t, test_region_is(t, &tv, "ab[cd|ef"), "region: exchange-point-and-mark");
    test_view_run(&tv, &CMD_MARK_WHOLE_BUFFER);
    TEST_CHECK(t, test_region_is(t, &tv, "|abcdef["), "region: mark-whole-buffer");
    // Typing with an active region inserts at point and deactivates (delete_selection_mode off).
    test_view_run(&tv, &CMD_FORWARD_CHAR); // a C-x h region stays active over a motion
    test_type_char(&tv, 'X');
    TEST_CHECK(t, test_region_is(t, &tv, "aX|bcdef]"), "region: typing inserts and deactivates");
    // DEL with an active region deletes the region (delete-active-region); undo restores it.
    test_view_run(&tv, &CMD_SET_MARK_COMMAND);
    test_run_shift(&tv, &CMD_FORWARD_CHAR);
    test_run_shift(&tv, &CMD_FORWARD_CHAR);
    test_view_run(&tv, &CMD_DELETE_BACKWARD_CHAR);
    TEST_CHECK(t, test_region_is(t, &tv, "aX]|def"), "region: DEL deletes the active region");
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("aXbcdef")) && !tv.view->cursors[0].mark_active, "region: undo restores, mark inactive");
    // delete_selection_mode: typing replaces the region.
    Settings dsm = test_settings;
    dsm.delete_selection_mode = 1;
    tv.ctx.settings = &dsm;
    test_view_run(&tv, &CMD_BEGINNING_OF_BUFFER);
    test_run_shift(&tv, &CMD_FORWARD_WORD);
    test_type_char(&tv, 'Y');
    TEST_CHECK(t, test_region_is(t, &tv, "]Y|"), "region: delete_selection_mode replaces the region");
    // Without transient mark mode, DEL deletes one character even with the mark active.
    Settings no_tmm = test_settings;
    no_tmm.transient_mark_mode = 0;
    tv.ctx.settings = &no_tmm;
    buffer_replace(tv.buf, 0, buffer_size(tv.buf), STR8_LIT("abc"));
    test_view_run(&tv, &CMD_BEGINNING_OF_BUFFER);
    test_view_run(&tv, &CMD_SET_MARK_COMMAND);
    test_view_run(&tv, &CMD_END_OF_BUFFER);
    test_view_run(&tv, &CMD_DELETE_BACKWARD_CHAR);
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("ab")), "region: without transient mark mode DEL deletes one character");
    tv.ctx.settings = &test_settings;
    // No mark yet: exchange-point-and-mark says so.
    if (!test_view_close(t, &tv)) return 0;
    if (!test_view_open(t, &tv, "a|b", 10, 40)) return 0;
    test_view_run(&tv, &CMD_EXCHANGE_POINT_AND_MARK);
    TEST_CHECK(t, test_echo_is(&tv, "No mark set in this buffer"), "region: exchange without a mark");
    if (!test_view_close(t, &tv)) return 0;
    // What a double click selects.
    Buffer *wb = buffer_create(STR8_LIT("words"));
    buffer_replace(wb, 0, 0, STR8_LIT("foo_bar  baz,\xc5\x9fu\n"));
    static const struct { i64 pos; b32 underscore; i64 start, end; } words[] = {
        { 1, 0, 0, 3 }, { 1, 1, 0, 7 }, { 3, 0, 3, 4 }, { 8, 0, 7, 9 }, { 12, 0, 12, 13 }, { 13, 0, 13, 16 }, { 16, 0, 16, 17 }, { 17, 0, 17, 17 },
    };
    for (i32 i = 0; i < ARRAY_COUNT(words); i++) {
        i64 s0, e0;
        view_word_bounds(wb, words[i].pos, words[i].underscore, &s0, &e0);
        TEST_CHECK(t, s0 == words[i].start && e0 == words[i].end, "region: word bounds case %d: [%D, %D)", i, s0, e0);
    }
    buffer_destroy(wb);
    LOG("test: ok: mark and region: shift-select, C-SPC, C-g, C-x C-x, C-x h, typing, delete-active-region, delete_selection_mode, word bounds");
    return 1;
}

// ---------------------------------------------------------------------------
// Clipboard: conversions and the fake (the real clipboard is never touched in --test)

static b32 test_clipboard(Test *t) {
    // UTF-8 with LF -> UTF-16 with CRLF.
    String8 text = STR8_LIT("a\nb\xc5\x9f\n\xf0\x9f\x98\x80z");
    u16 expected[] = { 'a', '\r', '\n', 'b', 0x15F, '\r', '\n', 0xD83D, 0xDE00, 'z' };
    u16 out[32];
    i64 n = clip_utf16_len(text);
    TEST_CHECK(t, n == ARRAY_COUNT(expected) && clip_utf16_write(text, out) == n &&
                  test_equal((u8 *)out, (u8 *)expected, n * 2), "clipboard: LF -> CRLF, UTF-16 with a surrogate pair");
    // CRLF and lone CR -> LF; an unpaired surrogate -> U+FFFD.
    u16 in[] = { 'x', '\r', '\n', 'y', '\r', 'z', '\n', 0xD800, '!', '\r' };
    String8 back = clip_utf8_from_utf16(&t->arena, in, ARRAY_COUNT(in));
    TEST_CHECK(t, str8_equal(back, STR8_LIT("x\ny\nz\n\xef\xbf\xbd!\n")), "clipboard: CRLF and CR -> LF ('%S')", back);
    TEST_CHECK(t, str8_equal(clip_utf8_from_utf16(&t->arena, out, n), text), "clipboard: round trip");
    // The fake: set, get, sequence numbers, a copy by another program.
    u32 seq = os_clipboard_seq();
    String8 got;
    TEST_CHECK(t, os_clipboard_set(text) && os_clipboard_seq() != seq && os_clipboard_get(&t->arena, &got) && str8_equal(got, text),
               "clipboard: the fake round trip");
    seq = os_clipboard_seq();
    os_dev_clipboard_external(STR8_LIT("from elsewhere\n"));
    TEST_CHECK(t, os_clipboard_seq() != seq && os_clipboard_get(&t->arena, &got) && str8_equal(got, STR8_LIT("from elsewhere\n")),
               "clipboard: an external copy");
    LOG("test: ok: clipboard: CRLF conversions both ways, surrogates, the fake clipboard");
    return 1;
}

// ---------------------------------------------------------------------------
// Kill ring

static b32 test_kill_is(TestView *tv, i32 back, const char *expected) {
    return str8_equal(kill_entry(tv->kills, back), str8_cstr(expected));
}

static b32 test_kill(Test *t) {
    TestView tv;
    // kill-line: the rest of the line; through the newline when only blanks are left; at the end
    // of a line the newline; at the end of the buffer a message. C-k C-k appends.
    if (!test_view_open(t, &tv, "ab|cd\nef   \ngh\nlast", 10, 40)) return 0;
    test_view_run(&tv, &CMD_KILL_LINE);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "ab|\nef   \ngh\nlast") && test_kill_is(&tv, 0, "cd"), "kill-line: rest of the line");
    test_view_run(&tv, &CMD_KILL_LINE);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "ab|ef   \ngh\nlast") && test_kill_is(&tv, 0, "cd\n") && tv.kills->count == 1,
               "kill-line: at the end of a line the newline, appended");
    test_view_run(&tv, &CMD_FORWARD_CHAR);
    test_view_run(&tv, &CMD_FORWARD_CHAR);
    test_view_run(&tv, &CMD_KILL_LINE);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "abef|gh\nlast") && test_kill_is(&tv, 0, "   \n") && tv.kills->count == 2,
               "kill-line: only blanks left: through the newline (a new entry after a motion)");
    test_view_run(&tv, &CMD_NEXT_LINE);
    test_view_run(&tv, &CMD_BACKWARD_CHAR);
    test_view_run(&tv, &CMD_BACKWARD_CHAR);
    test_view_run(&tv, &CMD_KILL_LINE);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "abefgh\nla|") && test_kill_is(&tv, 0, "st"), "kill-line: the last line");
    test_view_run(&tv, &CMD_KILL_LINE);
    TEST_CHECK(t, test_echo_is(&tv, "End of buffer") && test_kill_is(&tv, 0, "st"), "kill-line: at the end of the buffer");
    if (!test_view_close(t, &tv)) return 0;
    if (!test_view_open(t, &tv, "\xc5\x9f|\xc4\x9f\xc3\xbc \n\xc4\xb1", 10, 40)) return 0;
    test_view_run(&tv, &CMD_KILL_LINE);
    TEST_CHECK(t, test_kill_is(&tv, 0, "\xc4\x9f\xc3\xbc "), "kill-line: multi-byte text");
    if (!test_view_close(t, &tv)) return 0;

    // M-d M-d appends; M-DEL M-DEL prepends; kill-whole-line; kill-region; kill-ring-save.
    if (!test_view_open(t, &tv, "one two| three four\nfive six\nseven", 10, 40)) return 0;
    test_view_run(&tv, &CMD_KILL_WORD);
    test_view_run(&tv, &CMD_KILL_WORD);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "one two|\nfive six\nseven") && test_kill_is(&tv, 0, " three four"),
               "kill-word twice: appended");
    test_view_run(&tv, &CMD_MOVE_END_OF_LINE); // not a kill: the next kill starts a new entry
    test_view_run(&tv, &CMD_BACKWARD_KILL_WORD);
    test_view_run(&tv, &CMD_BACKWARD_KILL_WORD);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "|\nfive six\nseven") && test_kill_is(&tv, 0, "one two") && tv.kills->count == 2,
               "backward-kill-word twice: prepended");
    test_view_run(&tv, &CMD_NEXT_LINE);
    test_view_run(&tv, &CMD_KILL_WHOLE_LINE);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "\n|seven") && test_kill_is(&tv, 0, "five six\n"), "kill-whole-line");
    test_view_run(&tv, &CMD_KILL_WHOLE_LINE);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "|") && test_kill_is(&tv, 0, "five six\n\nseven"),
               "kill-whole-line on the last line takes the newline before it, appended");
    buffer_replace(tv.buf, 0, 0, STR8_LIT("alpha beta"));
    test_view_run(&tv, &CMD_BEGINNING_OF_BUFFER);
    test_view_run(&tv, &CMD_KILL_REGION);
    TEST_CHECK(t, test_echo_is(&tv, "The mark is not set now, so there is no region"), "kill-region without a mark");
    test_view_run(&tv, &CMD_SET_MARK_COMMAND);
    test_view_run(&tv, &CMD_FORWARD_WORD);
    test_view_run(&tv, &CMD_KILL_RING_SAVE);
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("alpha beta")) && test_kill_is(&tv, 0, "alpha") && !tv.view->cursors[0].mark_active,
               "kill-ring-save copies and deactivates");
    test_view_run(&tv, &CMD_KILL_REGION); // M-w then C-w: appends
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT(" beta")) && test_kill_is(&tv, 0, "alphaalpha"), "kill-region after kill-ring-save appends");
    // yank, yank-pop rotation.
    test_view_run(&tv, &CMD_END_OF_BUFFER);
    test_view_run(&tv, &CMD_YANK);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), " betaalphaalpha|"), "yank the newest kill");
    test_view_run(&tv, &CMD_YANK_POP);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), " betafive six\n\nseven|"), "yank-pop: the next older");
    test_view_run(&tv, &CMD_YANK_POP);
    test_view_run(&tv, &CMD_YANK_POP);
    test_view_run(&tv, &CMD_YANK_POP);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), " betaalphaalpha|"), "yank-pop: the ring wraps around");
    test_view_run(&tv, &CMD_BACKWARD_CHAR);
    test_view_run(&tv, &CMD_YANK_POP);
    TEST_CHECK(t, test_echo_is(&tv, "Previous command was not a yank"), "yank-pop not after a yank");
    // Undo of a yank.
    test_view_run(&tv, &CMD_YANK);
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT(" betaalphaalpha")), "undo of a yank");

    // The clipboard link: kills go to the (fake) clipboard; a copy by another program is yanked
    // and becomes the newest kill, once.
    String8 clip;
    TEST_CHECK(t, os_clipboard_get(&t->arena, &clip) && str8_equal(clip, STR8_LIT("alphaalpha")), "clipboard: holds the last kill ('%S')", clip);
    os_dev_clipboard_external(STR8_LIT("from\nelsewhere")); // stored with CRLF, as another program would
    i32 count = tv.kills->count;
    test_view_run(&tv, &CMD_END_OF_BUFFER);
    test_view_run(&tv, &CMD_YANK);
    TEST_CHECK(t, test_kill_is(&tv, 0, "from\nelsewhere") && tv.kills->count == count + 1, "clipboard: the external copy is the newest kill");
    test_view_run(&tv, &CMD_YANK);
    TEST_CHECK(t, tv.kills->count == count + 1, "clipboard: not pushed twice");
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT(" betaalphaalphafrom\nelsewherefrom\nelsewhere")), "clipboard: yanked with LF");
    // Read-only: the text is copied, the buffer stays.
    tv.buf->read_only = 1;
    test_view_run(&tv, &CMD_BEGINNING_OF_BUFFER);
    test_view_run(&tv, &CMD_KILL_LINE);
    TEST_CHECK(t, test_kill_is(&tv, 0, " betaalphaalphafrom") && test_echo_is(&tv, "Buffer is read-only: test.c") &&
                  test_text_is(t, tv.buf, STR8_LIT(" betaalphaalphafrom\nelsewherefrom\nelsewhere")), "kill in a read-only buffer copies");
    tv.buf->read_only = 0;
    if (!test_view_close(t, &tv)) return 0;

    // Storage: small entries share one arena, compacted as old ones are dropped; an entry over
    // 1 MB has its own reservation and appending to it does not move it.
    KillRing *k = PUSH_STRUCT(&t->arena, KillRing);
    kill_init(k, 5);
    u8 *big = PUSH_ARRAY(&t->arena, u8, MB(3));
    for (i32 i = 0; i < 40; i++) {
        i64 len = 1000 + i * 3000;
        u8 *dst = kill_push(k, len);
        TEST_CHECK(t, dst != NULL, "kill storage: push %d", i);
        memset(dst, 'a' + i % 26, (size_t)len);
    }
    for (i32 b = 0; b < 5; b++) {
        String8 e = kill_entry(k, b);
        i32 i = 39 - b;
        TEST_CHECK(t, e.len == 1000 + i * 3000 && e.data[0] == 'a' + i % 26 && e.data[e.len - 1] == 'a' + i % 26 &&
                      e.data >= k->small_base && e.data < k->small_base + k->small_used, "kill storage: small entry %d after compaction", b);
    }
    TEST_CHECK(t, k->count == 5 && k->small_used < (u64)(5 * 120000 * 2), "kill storage: dead bytes compacted (%U used)", k->small_used);
    u8 *dst = kill_extend(k, MB(1), 0); // the newest grows past 1 MB: its own reservation, once
    memset(dst, 'Z', MB(1));
    String8 grown = kill_entry(k, 0);
    TEST_CHECK(t, grown.len == 1000 + 39 * 3000 + MB(1) && grown.data[0] == 'a' + 39 % 26 && grown.data[grown.len - 1] == 'Z' &&
                  (grown.data < k->small_base || grown.data >= k->small_base + KILL_SMALL_RESERVE), "kill storage: moved to its own reservation");
    for (i32 i = 0; i < 3; i++) {
        memset(big, '0' + i, MB(3));
        u8 *d = kill_extend(k, MB(3), i == 1);
        memcpy(d, big, MB(3));
        TEST_CHECK(t, kill_entry(k, 0).data == grown.data, "kill storage: appending to a large entry does not move it");
    }
    String8 e = kill_entry(k, 0);
    TEST_CHECK(t, e.len == grown.len + (i64)MB(9) && e.data[0] == '1' && e.data[MB(3)] == 'a' + 39 % 26 && e.data[e.len - 1] == '2',
               "kill storage: appended and prepended in place");
    kill_set_max(k, 2);
    TEST_CHECK(t, k->count == 2 && kill_entry(k, 0).len == e.len, "kill storage: kill_ring_max drops the oldest");
    kill_destroy(k);
    LOG("test: ok: kill ring: kill-line cases, append and prepend, whole line, region, yank and yank-pop, clipboard link, read-only, storage");
    return 1;
}

// ---------------------------------------------------------------------------
// Indentation

// ---------------------------------------------------------------------------
// Syntax

// Golden token tests: code lines, each followed by its kinds, one letter per byte (' ' = any):
// t text, c comment, s string, n number, k keyword, y type, o constant, d directive, f function,
// v variable, p punctuation, i invalid. Lines are lexed in order, each from the state the one
// before ended in.
typedef struct TestGolden {
    BufferLanguage language;
    const char *what;
    const char *lines[40]; // code, kinds, code, kinds, ... NULL
} TestGolden;

static const TestGolden test_goldens[] = {
    { BUFFER_LANG_C, "C: keywords, types, a number, a comment", {
        "static const u32 x = 0x1F; // hi",
        "kkkkkk kkkkk yyy t p nnnnp ccccc" } },
    { BUFFER_LANG_C, "C: every number form", {
        "n = 0x1F + 0XaB + 0b101 + 0755 + 1.5e-3f + .5 + 1'000'000ull + 0x1.8p3 + 10u + 1E+9L;",
        "t p nnnn p nnnn p nnnnn p nnnn p nnnnnnn p nn p nnnnnnnnnnnn p nnnnnnn p nnn p nnnnnp" } },
    { BUFFER_LANG_C, "C: strings, chars, prefixes, escapes", {
        "s = \"a\\\"b\"; c = '\\''; w = L\"wide\"; u = u8'x';",
        "t p ssssssp t p ssssp t p sssssssp t p sssssp" } },
    { BUFFER_LANG_C, "C: an unterminated string ends at the end of the line", {
        "s = \"abc",
        "t p ssss",
        "int y;",
        "yyy tp" } },
    { BUFFER_LANG_C, "C: a string continued with a backslash", {
        "s = \"abc\\",
        "t p sssss",
        "def\"; int",
        "ssssp yyy" } },
    { BUFFER_LANG_C, "C: a block comment across lines", {
        "x = 1; /* a",
        "t p np cccc",
        "b */ int y;",
        "cccc yyy tp" } },
    { BUFFER_LANG_C, "C: a line comment continued with a backslash", {
        "// a \\",
        "cccccc",
        "int still;",
        "cccccccccc",
        "int z;",
        "yyy tp" } },
    { BUFFER_LANG_CPP, "C++: raw strings, on one line and across lines", {
        "auto r = R\"xy(a)\" )xy\" + 1;",
        "kkkk t p sssssssssssss p np",
        "auto m = u8R\"--(start",
        "kkkk t p ssssssssssss",
        "still )- \" )--x\" in",
        "sssssssssssssssssss",
        "end)--\"; int z;",
        "sssssssp yyy tp" } },
    { BUFFER_LANG_CPP, "C++: a raw string without a delimiter, quotes inside (manual G8)", {
        "x = R\"(raw \"text\")\";",
        "t p sssssssssssssssp" } },
    { BUFFER_LANG_C, "C: #define with a number and a comment (manual G5)", {
        "#define MAX 10 // note",
        "ddddddd fff nn ccccccc" } },
    { BUFFER_LANG_C, "C: preprocessor lines: the directive word, the rest lexed, continuation", {
        "#include <stdio.h> // c",
        "dddddddd sssssssss cccc",
        "#  define MAX(a, b) ((a) > (b) ? (a) : (b))",
        "ddddddddd fffptp tp pptp p ptp p ptp p ptpp",
        "#define S \"str\" \\",
        "ddddddd f sssss p",
        "    + 12 // x",
        "    p nn cccc",
        "int after;",
        "yyy tttttp",
        "# pragma once",
        "dddddddd tttt",
        "#if defined(X) && X > 1",
        "ddd tttttttptp pp t p n" } },
    { BUFFER_LANG_C, "C: function names in definitions at column 0", {
        "int main(void)",
        "yyy ffffpyyyyp",
        "static void foo(int x)",
        "kkkkkk yyyy fffpyyy tp",
        "static int",
        "kkkkkk yyy",
        "foo(int x)",
        "fffpyyy tp",
        "LRESULT CALLBACK WndProc(HWND h, UINT m)",
        "ttttttt tttttttt fffffffptttt tp tttt tp",
        "foo(x);",
        "tttptpp",
        "int foo(int);",
        "yyy fffpyyypp",
        "typedef void (*Fn)(int);",
        "kkkkkkk yyyy ppttppyyypp",
        "    bar(x) {",
        "    tttptp p",
        "if(x)",
        "kkptp",
        "void Foo::bar(int)",
        "yyyy tttppfffpyyyp",
        "Foo::~Foo()",
        "tttpppfffpp" } },
    { BUFFER_LANG_C, "C: non-ASCII identifiers, control characters, invalid bytes", {
        "int \xC3\xA9t\xC3\xA9 = 1; \x01 @ \xFF\xFEx;",
        "yyy ttttt p np i i tttp" } },
    { BUFFER_LANG_JAI, "Jai: nested comments on one line and across lines", {
        "/* a /* b */ still */ x := 1;",
        "ccccccccccccccccccccc v pp np",
        "/* outer",
        "cccccccc",
        "/* inner */",
        "ccccccccccc",
        "still comment */ y: int;",
        "cccccccccccccccc vp yyyp" } },
    { BUFFER_LANG_JAI, "Jai: a here-string", {
        "s := #string END",
        "v pp ddddddd sss",
        "text with END inside and \"quotes\" /* not a comment",
        "ssssssssssssssssssssssssssssssssssssssssssssssssss",
        "  END;",
        "  sssp",
        "x := 1;",
        "v pp np" } },
    { BUFFER_LANG_JAI, "Jai: declarations", {
        "main :: () {",
        "ffff pp pp p",
        "    v: Vector2;",
        "    vp tttttttp",
        "Vector2 :: struct { x: float; }",
        "fffffff pp kkkkkk p vp yyyyyp p",
        "MAX :: 128;",
        "fff pp nnnp" } },
    { BUFFER_LANG_JAI, "Jai: directives, notes, ---", {
        "#import \"Basic\"; x: int = ---; @Note proc :: () #expand {}",
        "ddddddd sssssssp vp yyy p ooop ddddd ffff pp pp ddddddd pp" } },
    { BUFFER_LANG_JAI, "Jai: every number form, a range", {
        "n := 0x1F + 0b1010 + 0h3F80_0000 + 1_000_000 + 1.5e3 + .5; r := 0..10;",
        "v pp nnnn p nnnnnn p nnnnnnnnnnn p nnnnnnnnn p nnnnn p nnp v pp nppnnp" } },
    { BUFFER_LANG_JAI, "Jai: strings, an unterminated string", {
        "s := \"a\\\"b\\n\" ; t := \"open",
        "v pp ssssssss p v pp sssss",
        "x := 1;",
        "v pp np" } },
    { BUFFER_LANG_JAI, "Jai: keywords and constants", {
        "for it, i: items { if it == null continue; }",
        "kkk kkp vp ttttt p kk kk pp oooo kkkkkkkkp p" } },
    { BUFFER_LANG_CSHARP, "C#: a verbatim string across lines", {
        "s = @\"C:\\path \"\"quoted\"\"",
        "t p sssssssssssssssssssss",
        "still\"; int x;",
        "ssssssp yyy tp" } },
    { BUFFER_LANG_CSHARP, "C#: an interpolated string with holes, escaped braces, a string in a hole", {
        "var s = $\"a {x + 1} b {{c}} {f(\"q\")}\";",
        "kkk t p sssspt p npsssssssssptpsssppsp" } },
    { BUFFER_LANG_CSHARP, "C#: a raw string across lines", {
        "var r = \"\"\"",
        "kkk t p sss",
        "  \"quoted\" and \"\" two",
        "sssssssssssssssssssss",
        "  \"\"\";",
        "  sssp",
        "int y;",
        "yyy tp" } },
    { BUFFER_LANG_CSHARP, "C#: an interpolated raw string with two dollars", {
        "x = $$\"\"\"{{a}} {b}\"\"\";",
        "t p ssssspptppsssssssp" } },
    { BUFFER_LANG_CSHARP, "C#: preprocessor lines, chars, numbers, a verbatim identifier", {
        "#region Helpers",
        "ddddddd ttttttt",
        "char c = '\\''; decimal d = 1_000.5m; var h = 0xFF_FFu; var @class = 1;",
        "yyyy t p ssssp yyyyyyy t p nnnnnnnnp kkk t p nnnnnnnnp kkk tttttt p np",
        "#if DEBUG && !RELEASE // c",
        "ddd ttttt pp pttttttt cccc" } },
    { BUFFER_LANG_CSHARP, "C#: an unterminated regular string ends at the end of the line", {
        "s = \"a\\\"b",
        "t p sssss",
        "int y;",
        "yyy tp" } },
    { BUFFER_LANG_CSHARP, "C#: an interpolated verbatim string with holes across lines", {
        "s = $@\"line {x}",
        "t p ssssssssptp",
        "more {y} end\";",
        "sssssptpsssssp" } },
    { BUFFER_LANG_CSHARP, "C#: contextual keywords", {
        "public async Task F() => await G();",
        "kkkkkk kkkkk tttt tpp pp kkkkk tppp" } },
    { BUFFER_LANG_JAVASCRIPT, "JS: regex versus division in 18 contexts (the rule carried across lines)", {
        "x = a / b / c;",
        "t p t p t p tp",
        "x = /ab+c/gi;",
        "t p ssssssssp",
        "f(/re/);",
        "tpsssspp",
        "a = [/a/, /b/];",
        "t p psssp ssspp",
        "return /x/.test(s);",
        "kkkkkk ssspttttptpp",
        "x = y.length / 2;",
        "t p tptttttt p np",
        "x = (a + b) / 2;",
        "t p pt p tp p np",
        "x = arr[0] / 2;",
        "t p tttpnp p np",
        "if (!/^\\d+$/.test(s)) {}",
        "kk ppssssssspttttptpp pp",
        "x = c ? /a/ : /b/;",
        "t p t p sss p sssp",
        "a = b++ / 2;",
        "t p tpp p np",
        "t = typeof /re/;",
        "t p kkkkkk ssssp",
        "x = /[/]/.source;",
        "t p ssssspttttttp",
        "}",
        "p",
        "/foo/.test(s);",
        "ssssspttttptpp",
        "x = a",
        "t p t",
        "/ b;",
        "p tp",
        "y = this / 2;",
        "t p kkkk p np" } },
    { BUFFER_LANG_JAVASCRIPT, "JS: template literals nested two deep, across lines", {
        "s = `x ${a + `y ${b}",
        "t p sssppt p ssspptp",
        "z` + 1} w`;",
        "ss p npsssp",
        "x = 1;",
        "t p np",
        "t = `a\\`b",
        "t p sssss",
        "c ${x}`;",
        "sspptpsp" } },
    { BUFFER_LANG_JAVASCRIPT, "JS: strings, every number form, a continued string", {
        "s = 'abc",
        "t p ssss",
        "n = 0x1F + 0b101 + 0o17 + 1_000n + 1.5e-3 + .5;",
        "t p nnnn p nnnnn p nnnn p nnnnnn p nnnnnn p nnp",
        "s = \"abc\\",
        "t p sssss",
        "def\"; x",
        "ssssp t" } },
    { BUFFER_LANG_JAVASCRIPT, "JS: TypeScript words are plain in JavaScript; non-ASCII, control characters", {
        "let string = interface;",
        "kkk tttttt p tttttttttp",
        "const \xC4\x9F = \"\xC3\xA9\"; \x01",
        "kkkkk tt p ssssp i" } },
    { BUFFER_LANG_TYPESCRIPT, "TS: keywords and built-in types", {
        "interface P { name: string; age?: number; }",
        "kkkkkkkkk t p ttttp yyyyyyp tttpp yyyyyyp p",
        "let v: unknown = undefined satisfies any;",
        "kkk tp yyyyyyy p ooooooooo kkkkkkkkk yyyp" } },
    { BUFFER_LANG_JAI, "Jai: non-ASCII identifiers, control characters", {
        "\xC3\xA7" "a\xC4\x9Fr\xC4\xB1 := \"\xC3\xBC\"; \x01",
        "vvvvvvvv pp ssssp i" } },
};

// The kind at byte b of a lexed line.
static SyntaxKind test_kind_at(SyntaxTokens *out, i64 b) {
    SyntaxKind kind = SYN_TEXT;
    for (i32 k = 0; k < out->count && out->tokens[k].start <= (u32)b; k++) kind = (SyntaxKind)out->tokens[k].kind;
    return kind;
}

static b32 test_golden(Test *t, const TestGolden *g) {
    static const char letters[SYN_KIND_COUNT + 1] = "tcsnkyodfvpi";
    SyntaxToken tokens[512];
    SyntaxTokens out = { tokens, 0, ARRAY_COUNT(tokens) };
    u32 state = 0;
    for (i32 k = 0; g->lines[k]; k += 2) {
        String8 code = str8_cstr(g->lines[k]);
        const char *kinds = g->lines[k + 1];
        state = syntax_lex(g->language, state, code, &out);
        u8 got[256];
        i64 n = MIN(code.len, (i64)sizeof(got) - 1);
        for (i64 b = 0; b < n; b++) got[b] = (u8)letters[test_kind_at(&out, b)];
        b32 ok = 1;
        for (i64 b = 0; kinds[b] && b < n; b++) ok &= kinds[b] == ' ' || (u8)kinds[b] == got[b];
        if (!ok) LOG("test: golden '%s', line %d:\n  %S\n  %s (expected)\n  %S (got)", g->what, k / 2, code, kinds, str8(got, n));
        TEST_CHECK(t, ok, "syntax: golden '%s', line %d", g->what, k / 2);
    }
    return 1;
}

// Incremental equals full: generated source-like text, random edits, catch-up with random needs
// and budgets in between. After every catch-up the states up to state_valid must equal a lex from
// scratch; at the end every state must, and sampled lines' tokens too.
typedef struct TestCorpus {
    BufferLanguage language;
    const char **lines;
    i32 line_count;
    const char **pieces; // edits that change states: comment and string delimiters ...
    i32 piece_count;
} TestCorpus;

static const char *test_corpus_c[] = {
    "#include <stdio.h>", "#define MAX(a, b) ((a) > (b) ? (a) : (b))", "#define LONG \\", "    1 + 2 \\", "    + 3",
    "/* a block comment", " * that spans", " */", "int main(void) {", "    const char *s = \"str\\\"ing\"; // c",
    "    auto r = R\"x(raw", "    still raw )x\" + 1;", "    char c = '\\'';", "    x = 1'000 + 0x1F;", "}",
    "// line comment \\", "continued", "static int", "foo(int x)", "    return x * 2; /* inline */ y;",
    "    s = \"unterminated", "    s = \"continued \\", "string\";", "", "typedef struct { u8 *data; i64 len; } String8;",
    "LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l)", "#if defined(X) /* c", "*/ && Y", "#endif",
};
static const char *test_pieces_c[] = { "/*", "*/", "\"", "'", "R\"x(", ")x\"", "\\", "\n", "#", "//", "{", "}", "\\\n", "x" };

static const char *test_corpus_jai[] = {
    "#import \"Basic\";", "/* outer", "/* inner */", "still comment */", "main :: () {", "    s := #string DONE",
    "text \"inside\" /* the string", "DONE;", "    x: int = ---;", "    for it, i: items { if it == null continue; }",
    "    n := 0x1F + 0h3F80_0000 + 1_000;", "}", "Vector2 :: struct { x, y: float; }", "// comment", "@Note proc :: () #expand {}",
    "    t := \"open", "",
};
static const char *test_pieces_jai[] = { "/*", "*/", "\"", "#string X\n", "X\n", "\n", "//", ":", "::", "#string DONE", "DONE" };

static const char *test_corpus_cs[] = {
    "#region Helpers", "#if DEBUG", "#endif", "/* a block", " comment */", "class C {", "    string s = @\"C:\\path \"\"q\"\"",
    "still verbatim\";", "    var r = \"\"\"", "      raw \"\" text", "      \"\"\";", "    var i = $\"a {x + 1} b {{c}} {f(\"q\")}\";",
    "    var v = $@\"line {", "        x + 1", "    } end\";", "    var w = $$\"\"\"{{a}} {b}\"\"\";", "    char c = '\\'';",
    "    decimal d = 1_000.5m; // c", "}", "    s = \"open", "",
};
static const char *test_pieces_cs[] = { "/*", "*/", "\"", "@\"", "$\"", "\"\"\"", "{", "}", "\n", "//", "$$\"\"\"", "$@\"" };

static const char *test_corpus_js[] = {
    "const re = /ab+c/gi;", "x = a / b / c;", "s = `x ${a + `y ${b}", "z` + 1} w`;", "/* block", "comment */",
    "function f(a, b) {", "  return a / 2;", "}", "let t = `multi", "line ${x}`;", "s = 'str\\", "continued';",
    "n = 0x1F + 1_000n;", "if (!/^\\d+$/.test(s)) {}", "// comment", "", "x = a", "/ b;", "y = this / 2;",
};
static const char *test_corpus_ts[] = {
    "interface P { name: string; age?: number; }", "let v: unknown = undefined satisfies any;", "type T = keyof typeof x;",
    "const re = /ab+c/gi;", "s = `x ${a + `y ${b}", "z` + 1} w`;", "/* block", "comment */", "function f(a: number): number {",
    "  return a / 2;", "}", "let t = `multi", "line ${x}`;", "",
};
static const char *test_pieces_js[] = { "/*", "*/", "`", "${", "}", "\"", "'", "/", "\n", "//", "\\\n", "{", "x" };

static const TestCorpus test_corpora[] = {
    { BUFFER_LANG_JAVASCRIPT, test_corpus_js, ARRAY_COUNT(test_corpus_js), test_pieces_js, ARRAY_COUNT(test_pieces_js) },
    { BUFFER_LANG_TYPESCRIPT, test_corpus_ts, ARRAY_COUNT(test_corpus_ts), test_pieces_js, ARRAY_COUNT(test_pieces_js) },
    { BUFFER_LANG_CSHARP, test_corpus_cs, ARRAY_COUNT(test_corpus_cs), test_pieces_cs, ARRAY_COUNT(test_pieces_cs) },
    { BUFFER_LANG_C, test_corpus_c, ARRAY_COUNT(test_corpus_c), test_pieces_c, ARRAY_COUNT(test_pieces_c) },
    { BUFFER_LANG_JAI, test_corpus_jai, ARRAY_COUNT(test_corpus_jai), test_pieces_jai, ARRAY_COUNT(test_pieces_jai) },
};

// States of every line lexed from scratch, into `states`.
static void test_full_states(Buffer *buf, Arena *scratch, u32 *states) {
    u32 state = 0;
    i64 count = buffer_line_count(buf);
    for (i64 l = 0; l < count; l++) {
        states[l] = state;
        u64 mark = arena_pos(scratch);
        state = syntax_lex(buf->language, state, buffer_line(buf, scratch, l), NULL);
        arena_pop_to(scratch, mark);
    }
}

static b32 test_incremental(Test *t, const TestCorpus *c, u64 seed) {
    t->rng = seed ^ (0x696e6372ull + (u64)c->language);
    Buffer *buf = buffer_create(STR8_LIT("incremental"));
    TEST_CHECK(t, buf, "syntax: buffer_create failed");
    buf->language = c->language;
    Arena scratch = arena_create(MB(64));
    u8 *text = PUSH_ARRAY(&t->arena, u8, MB(1));
    i64 n = 0;
    for (i32 l = 0; l < 3000; l++) {
        String8 line = str8_cstr(c->lines[test_below(t, c->line_count)]);
        memcpy(text + n, line.data, (size_t)line.len);
        n += line.len;
        text[n++] = '\n';
    }
    buffer_replace(buf, 0, 0, str8(text, n));
    i64 cap = 20000;
    u32 *full = PUSH_ARRAY(&t->arena, u32, cap);
    syntax_dev_fake_clock(1);
    i64 checks = 0, converged = 0;
    for (i32 op = 0; op < 600; op++) {
        i64 size = buffer_size(buf), a = test_below(t, size + 1), b = a;
        String8 ins = str8(NULL, 0);
        switch (test_below(t, 4)) {
        case 0: ins = str8_cstr(c->pieces[test_below(t, c->piece_count)]); break;
        case 1: b = MIN(a + test_below(t, 200), size); break;
        case 2: ins = str8_cstr(c->lines[test_below(t, c->line_count)]); break;
        default: b = MIN(a + test_below(t, 20), size); ins = str8_cstr(c->pieces[test_below(t, c->piece_count)]); break;
        }
        a = buffer_snap_char(buf, a);
        b = buffer_snap_char(buf, b);
        if (buffer_line_count(buf) + 2 >= cap) b = size; // keep it bounded: a big delete
        buffer_replace(buf, a, b, ins);
        if (test_below(t, 3) == 0) {
            // Half of them complete (so later edits can converge), half partial.
            b32 complete = test_below(t, 2) == 0;
            i64 need = complete ? buffer_line_count(buf) - 1 : test_below(t, buffer_line_count(buf));
            u64 before = syntax_dev_converged();
            syntax_catch_up(buf, need, complete ? (u64)I64_MAX : (u64)test_below(t, KB(64)), &scratch);
            converged += syntax_dev_converged() > before;
            test_full_states(buf, &scratch, full);
            for (i64 l = 0; l <= buf->state_valid; l++) {
                TEST_CHECK(t, buffer_line_state(buf, l) == full[l], "syntax: %s incremental: op %d, line %D (valid to %D): 0x%x, full 0x%x",
                           buffer_language_name(c->language), op, l, buf->state_valid, buffer_line_state(buf, l), full[l]);
            }
            checks++;
        }
    }
    syntax_dev_fake_clock(0);
    i64 last = buffer_line_count(buf) - 1;
    TEST_CHECK(t, syntax_catch_up(buf, last, (u64)I64_MAX, &scratch) && buf->state_valid == last, "syntax: final catch-up");
    test_full_states(buf, &scratch, full);
    for (i64 l = 0; l <= last; l++) {
        TEST_CHECK(t, buffer_line_state(buf, l) == full[l], "syntax: %s incremental: final, line %D: 0x%x, full 0x%x",
                   buffer_language_name(c->language), l, buffer_line_state(buf, l), full[l]);
    }
    // Sampled lines: the tokens for drawing (from stored states) equal a fresh lex.
    SyntaxToken t1[1024], t2[1024];
    for (i32 k = 0; k < 200; k++) {
        i64 l = test_below(t, last + 1);
        u64 mark = arena_pos(&scratch);
        String8 line = buffer_line(buf, &scratch, l);
        SyntaxTokens a = { t1, 0, ARRAY_COUNT(t1) }, b = { t2, 0, ARRAY_COUNT(t2) };
        TEST_CHECK(t, syntax_line_tokens(buf, l, line, &a), "syntax: line %D has tokens", l);
        syntax_lex(c->language, full[l], line, &b);
        b32 same = a.count == b.count;
        for (i32 i = 0; same && i < a.count; i++) same = a.tokens[i].start == b.tokens[i].start && a.tokens[i].kind == b.tokens[i].kind;
        TEST_CHECK(t, same, "syntax: %s incremental: the tokens of line %D differ", buffer_language_name(c->language), l);
        arena_pop_to(&scratch, mark);
    }
    LOG("test: ok: %s incremental equals full (600 edits, %D partial catch-ups checked, %D of them converged, %D lines)",
        buffer_language_name(c->language), checks, converged, last + 1);
    os_release(scratch.base);
    TEST_CHECK(t, buffer_destroy(buf), "syntax: destroy");
    return 1;
}

// The budget: with the clock counting bytes lexed, one catch-up goes past its budget by less than
// one clock check's worth of work: SYNTAX_CHECK_LINES lines and SYNTAX_CHECK_BYTES bytes plus the
// line that crossed them.
static b32 test_syntax_budget(Test *t, u64 seed) {
    t->rng = seed ^ 0x627564676574ull;
    Buffer *buf = buffer_create(STR8_LIT("budget.c"));
    TEST_CHECK(t, buf, "syntax: buffer_create failed");
    buf->language = BUFFER_LANG_C;
    Arena scratch = arena_create(MB(64));
    u8 *text = PUSH_ARRAY(&t->arena, u8, MB(8));
    i64 n = 0, longest = 0;
    for (i32 l = 0; l < 40000; l++) {
        i64 len = test_below(t, 100) == 0 ? test_below(t, KB(40)) : test_below(t, 60);
        if (l % 10000 == 5000) len = KB(50);
        for (i64 i = 0; i < len; i++) text[n + i] = (u8)('a' + (i % 26));
        n += len;
        text[n++] = '\n';
        longest = MAX(longest, len + 1);
    }
    buffer_replace(buf, 0, 0, str8(text, n));
    syntax_dev_fake_clock(1);
    i64 calls = 0, worst = 0;
    for (i32 round = 0; round < 40; round++) {
        // A comment opened and closed again at the top: every state changes.
        if (round & 1) buffer_replace(buf, 0, 2, STR8_LIT(""));
        else buffer_replace(buf, 0, 0, STR8_LIT("/*"));
        i64 last = buffer_line_count(buf) - 1;
        for (b32 done = 0; !done; calls++) {
            u64 budget = (u64)(1 + test_below(t, round < 20 ? KB(200) : 2000));
            u64 before = syntax_dev_lexed();
            done = syntax_catch_up(buf, last, budget, &scratch);
            u64 used = syntax_dev_lexed() - before;
            TEST_CHECK(t, done || used >= budget, "syntax: budget: stopped early (%U of %U)", used, budget);
            i64 over = (i64)used - (i64)budget;
            worst = MAX(worst, over);
            TEST_CHECK(t, over < (i64)SYNTAX_CHECK_BYTES + longest && over < (i64)SYNTAX_CHECK_LINES * longest,
                       "syntax: budget %U, lexed %U: over by %D", budget, used, over);
        }
    }
    syntax_dev_fake_clock(0);
    os_release(scratch.base);
    TEST_CHECK(t, buffer_destroy(buf), "syntax: destroy");
    LOG("test: ok: syntax budget (%D catch-ups, worst overshoot %D bytes, bound %D + the longest line %D)", calls, worst,
        (i64)SYNTAX_CHECK_BYTES, longest);
    return 1;
}

static b32 test_syntax(Test *t, u64 seed) {
    for (i32 i = 0; i < ARRAY_COUNT(test_goldens); i++) if (!test_golden(t, &test_goldens[i])) return 0;
    LOG("test: ok: syntax goldens (%d cases)", (i32)ARRAY_COUNT(test_goldens));
    for (i32 i = 0; i < ARRAY_COUNT(test_corpora); i++) if (!test_incremental(t, &test_corpora[i], seed)) return 0;
    return test_syntax_budget(t, seed);
}

// Bracket matching on tokens. In `marked`, \x01 is before the bracket asked about and \x02 before
// its expected match (none: no match). A match found must also lead back.
static b32 test_match_case(Test *t, BufferLanguage language, const char *what, String8 marked) {
    u8 *text = PUSH_ARRAY(&t->arena, u8, marked.len);
    i64 n = 0, at = -1, want = -1;
    for (i64 i = 0; i < marked.len; i++) {
        if (marked.data[i] == 1) at = n;
        else if (marked.data[i] == 2) want = n;
        else text[n++] = marked.data[i];
    }
    Buffer *buf = buffer_create(STR8_LIT("match"));
    TEST_CHECK(t, buf, "match: buffer_create failed");
    buf->language = language;
    buffer_replace(buf, 0, 0, str8(text, n));
    syntax_catch_up(buf, buffer_line_count(buf) - 1, (u64)I64_MAX, &t->arena);
    i64 got = syntax_match_bracket(buf, at, &t->arena);
    i64 back = got >= 0 ? syntax_match_bracket(buf, got, &t->arena) : -1;
    buffer_destroy(buf);
    TEST_CHECK(t, got == want && (want < 0 || back == at), "match: %s: from %D: %D (back %D), expected %D", what, at, got, back, want);
    return 1;
}

static b32 test_match(Test *t) {
    static const struct { BufferLanguage language; const char *what, *marked; } cases[] = {
        { BUFFER_LANG_C, "nested", "f\x01(a(b[1]), {c}\x02)" },
        { BUFFER_LANG_C, "across lines", "int f() \x01{\n  if (x) {\n    y();\n  }\n\x02}\n" },
        { BUFFER_LANG_C, "a bracket in a string has no match", "s = \"\x01(\"; )" },
        { BUFFER_LANG_C, "brackets in strings, chars and comments are skipped", "f\x01(\")\", ')', /* ) */ // )\nx\x02)" },
        { BUFFER_LANG_C, "none found", "f\x01(a, b\n" },
        { BUFFER_LANG_C, "a mismatched pair on the way", "f\x01(a]" },
        { BUFFER_LANG_C, "a preprocessor line's brackets do not count", "\x01{\n#define X }\n#if (A\n\x02}" },
        { BUFFER_LANG_FUNDAMENTAL, "Fundamental: every bracket counts", "f\x01(\"\x02)\")" },
        { BUFFER_LANG_JAVASCRIPT, "a brace in a template hole", "s = `${\x01{a: `}`\x02}}`;" },
        { BUFFER_LANG_JAI, "Jai: a nested comment between", "f :: () \x01{ /* } /* } */ } */ \x02}" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        if (!test_match_case(t, cases[i].language, cases[i].what, str8_cstr(cases[i].marked))) return 0;
    }
    // Too far: the match lies beyond SYNTAX_MATCH_MAX.
    i64 n = SYNTAX_MATCH_MAX + 100;
    u8 *far_text = PUSH_ARRAY(&t->arena, u8, n + 3);
    far_text[0] = 1;
    far_text[1] = '{';
    for (i64 i = 2; i < n; i += 2) far_text[i] = 'x', far_text[i + 1] = '\n';
    far_text[n] = '}';
    if (!test_match_case(t, BUFFER_LANG_C, "too far", str8(far_text, n + 1))) return 0;
    // The innermost opener that encloses a position.
    Buffer *buf = buffer_create(STR8_LIT("enclosing"));
    buf->language = BUFFER_LANG_C;
    String8 text = STR8_LIT("switch (x) {\ncase 1: f(a, \"{\");\n  case 2:");
    buffer_replace(buf, 0, 0, text);
    syntax_catch_up(buf, 2, (u64)I64_MAX, &t->arena);
    i64 open = syntax_enclosing_open(buf, buffer_line_start(buf, 2) + 2, &t->arena);
    i64 none = syntax_enclosing_open(buf, 3, &t->arena);
    buffer_destroy(buf);
    TEST_CHECK(t, open == 11 && none == -1, "match: enclosing opener %D (expected 11), none %D", open, none);
    LOG("test: ok: bracket matching (%d cases, too far, the enclosing opener)", (i32)ARRAY_COUNT(cases));
    return 1;
}

// Every line of `input` reindented by the rule, top to bottom (as TAB over the whole buffer: blank
// lines emptied, lines inside multi-line comments and strings left alone).
static b32 test_indent_case(Test *t, BufferLanguage language, const char *what, const char *input, const char *expected, b32 tabs,
                            i32 tab_width) {
    Buffer *buf = buffer_create(STR8_LIT("indent"));
    buf->language = language;
    buf->indent_tabs = tabs;
    buf->tab_width = tab_width;
    buffer_replace(buf, 0, 0, str8_cstr(input));
    for (i64 line = 0; line < buffer_line_count(buf); line++) {
        i64 s = buffer_line_start(buf, line), e = buffer_line_end(buf, line), p = s;
        while (p < e && (buffer_byte(buf, p) == ' ' || buffer_byte(buf, p) == '\t')) p++;
        if (p == e) buffer_replace(buf, s, e, STR8_LIT(""));
        else if (!edit_line_fixed(buf, line, &t->arena)) edit_set_indent(buf, line, edit_compute_indent(buf, line, 4, &t->arena));
    }
    String8 got = buffer_text(buf, &t->arena, 0, buffer_size(buf));
    b32 ok = str8_equal(got, str8_cstr(expected));
    if (!ok) LOG("test: indent '%s':\n%S\n--- expected:\n%s", what, got, expected);
    buffer_destroy(buf);
    TEST_CHECK(t, ok, "indent: %s", what);
    return 1;
}

static b32 test_indent(Test *t) {
    static const struct { BufferLanguage language; const char *what, *input, *expected; } cases[] = {
        { BUFFER_LANG_C, "C: nested blocks, else, a call continued over two lines",
          "int f(int a) {\nif (a) {\nreturn g(a,\nb,\nc);\n} else {\nreturn 0;\n}\n}\n",
          "int f(int a) {\n    if (a) {\n        return g(a,\n            b,\n            c);\n    } else {\n        return 0;\n    }\n}\n" },
        // Phase 8: one level after a line that leaves brackets open, however many (was two here).
        { BUFFER_LANG_C, "C: a line that is only \"));\"",
          "x = f(g(\na\n));\ny;\n",
          "x = f(g(\n    a\n));\ny;\n" },
        { BUFFER_LANG_C, "C: two openers on one line, blank lines, wrong indentation fixed",
          "    foo({\n\n   a,\n\n})\n        bar();\n",
          "foo({\n\n    a,\n\n})\nbar();\n" },
        { BUFFER_LANG_C, "C: a negative balance stops at column 0",
          "a)));\n}\nb;\n",
          "a)));\n}\nb;\n" },
        { BUFFER_LANG_JAI, "Jai: procedure and loop",
          "main :: () {\nfor 0..10 {\nprint(\"%\\n\", it);\n}\n}\n",
          "main :: () {\n    for 0..10 {\n        print(\"%\\n\", it);\n    }\n}\n" },
        { BUFFER_LANG_JAVASCRIPT, "JavaScript: array literal with an object, a function",
          "const a = [\n1,\n{ b: 2 },\n];\nfunction f() {\nreturn a;\n}\n",
          "const a = [\n    1,\n    { b: 2 },\n];\nfunction f() {\n    return a;\n}\n" },
        { BUFFER_LANG_C, "brackets closed on the same line do not count",
          "if (a) { b(); }\nc;\n[x] = y[0];\nz;\n",
          "if (a) { b(); }\nc;\n[x] = y[0];\nz;\n" },
        // Phase 8: on tokens.
        { BUFFER_LANG_C, "C: brackets in strings, chars and comments do not count",
          "f(\"(\", '{');\nx;\n/* ( { */\ny;\n// ) }\nz = \"}\";\nw;\n",
          "f(\"(\", '{');\nx;\n/* ( { */\ny;\n// ) }\nz = \"}\";\nw;\n" },
        { BUFFER_LANG_JAVASCRIPT, "JavaScript: a callback: one level, and the closing line returns",
          "foo(function () {\nbar();\n});\nnext();\nf((x) => {\nreturn x;\n});\ny;\n",
          "foo(function () {\n    bar();\n});\nnext();\nf((x) => {\n    return x;\n});\ny;\n" },
        { BUFFER_LANG_JAVASCRIPT, "JavaScript: nested callbacks",
          "a(() => {\nb(function () {\nc([\n1,\n]);\n});\n});\nd;\n",
          "a(() => {\n    b(function () {\n        c([\n            1,\n        ]);\n    });\n});\nd;\n" },
        { BUFFER_LANG_C, "C: continued call arguments return to the call's line",
          "    x = foo(a,\nb,\nc);\nd;\n",
          "x = foo(a,\n    b,\n    c);\nd;\n" },
        { BUFFER_LANG_C, "C: } else {",
          "if (a) {\nb;\n} else {\nc;\n}\nd;\n",
          "if (a) {\n    b;\n} else {\n    c;\n}\nd;\n" },
        { BUFFER_LANG_C, "C: lines inside a block comment are left alone; comment-only lines are skipped",
          "int x;\n/*\n   * keep\n  */\ny;\n",
          "int x;\n/*\n   * keep\n  */\ny;\n" },
        { BUFFER_LANG_CPP, "C++: lines inside a raw string are left alone; the next statement returns",
          "  auto s = R\"(\n  a {\n)\";\nz;\n",
          "auto s = R\"(\n  a {\n)\";\nz;\n" },
        { BUFFER_LANG_JAVASCRIPT, "JavaScript: lines inside a template literal are left alone",
          "s = `\n  ${x} {\n`;\ny;\n",
          "s = `\n  ${x} {\n`;\ny;\n" },
        // Brace-less bodies, labels, preprocessor lines.
        { BUFFER_LANG_C, "C: a brace-less if followed by else",
          "if (a)\nx();\nelse\ny();\nz();\n",
          "if (a)\n    x();\nelse\n    y();\nz();\n" },
        { BUFFER_LANG_C, "C: else if",
          "if (a)\nx();\nelse if (b)\ny();\nelse\nw();\nz();\n",
          "if (a)\n    x();\nelse if (b)\n    y();\nelse\n    w();\nz();\n" },
        { BUFFER_LANG_C, "C: Allman braces after a header",
          "if (a)\n{\nb();\n}\nc();\n",
          "if (a)\n{\n    b();\n}\nc();\n" },
        { BUFFER_LANG_C, "C: nested brace-less for and if, a dangling else",
          "for (i = 0; i < n; i++)\nif (x)\na();\nelse\nb();\nc();\n",
          "for (i = 0; i < n; i++)\n    if (x)\n        a();\n    else\n        b();\nc();\n" },
        { BUFFER_LANG_C, "C: an else after a nested if-else binds to the outer if",
          "if (x)\nif (y)\na();\nelse\nb();\nelse\nc();\nd();\n",
          "if (x)\n    if (y)\n        a();\n    else\n        b();\nelse\n    c();\nd();\n" },
        { BUFFER_LANG_C, "C: a condition over two lines",
          "if (a &&\nb)\nc();\nd();\n",
          "if (a &&\n    b)\n    c();\nd();\n" },
        { BUFFER_LANG_JAVASCRIPT, "JavaScript: a brace-less body that is a call with a callback",
          "if (a)\nfoo(function () {\nbar();\n});\nnext();\n",
          "if (a)\n    foo(function () {\n        bar();\n    });\nnext();\n" },
        { BUFFER_LANG_C, "C: a comment between a header and its body",
          "while (x)\n// step\nx = f(x);\ny();\n",
          "while (x)\n    // step\n    x = f(x);\ny();\n" },
        { BUFFER_LANG_C, "C: a switch with fall-through labels and a braced case body",
          "switch (x) {\ncase 1:\ncase 2:\na();\nbreak;\ncase 3: {\nint y = 1;\n}\ndefault:\nb();\n}\n",
          "switch (x) {\n    case 1:\n    case 2:\n        a();\n        break;\n    case 3: {\n        int y = 1;\n    }\n    default:\n        b();\n}\n" },
        { BUFFER_LANG_JAI, "Jai: if x == { case ...; }",
          "if x == {\ncase 1;\na();\ncase;\nb();\n}\n",
          "if x == {\n    case 1;\n        a();\n    case;\n        b();\n}\n" },
        { BUFFER_LANG_JAI, "Jai: a brace-less for",
          "for it: items\nprint(it);\ndone();\n",
          "for it: items\n    print(it);\ndone();\n" },
        { BUFFER_LANG_CSHARP, "C#: foreach and using without braces",
          "foreach (var x in xs)\nusing (var f = Open(x))\nf.Read();\nDone();\n",
          "foreach (var x in xs)\n    using (var f = Open(x))\n        f.Read();\nDone();\n" },
        { BUFFER_LANG_C, "C: an #if block in the middle of a function, a multi-line #define",
          "void f(void) {\na();\n#if DEBUG\nlog();\n#else\n  #define X(a) \\\n     ((a) + 1)\n#endif\nb();\n}\n",
          "void f(void) {\n    a();\n#if DEBUG\n    log();\n#else\n  #define X(a) \\\n     ((a) + 1)\n#endif\n    b();\n}\n" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        if (!test_indent_case(t, cases[i].language, cases[i].what, cases[i].input, cases[i].expected, 0, 4)) return 0;
    }
    // Tabs: whole tabs, then spaces for the rest (tab width 8, indent width 4).
    if (!test_indent_case(t, BUFFER_LANG_C, "tabs", "a {\nb {\nc {\nd;\n}\n}\n}\n",
                          "a {\n    b {\n\tc {\n\t    d;\n\t}\n    }\n}\n", 1, 8)) return 0;
    // Detection from the first indented lines.
    Buffer *buf = buffer_create(STR8_LIT("d.c"));
    buffer_replace(buf, 0, 0, STR8_LIT("a\n\tb\n\tc\n  d\n * comment\n"));
    TEST_CHECK(t, edit_detect_tabs(buf) == 1, "indent: detect tabs");
    buffer_replace(buf, 0, buffer_size(buf), STR8_LIT("a\n    b\n    c\n\td\n"));
    TEST_CHECK(t, edit_detect_tabs(buf) == 0, "indent: detect spaces");
    buffer_replace(buf, 0, buffer_size(buf), STR8_LIT("a\nb\n * x\n"));
    TEST_CHECK(t, edit_detect_tabs(buf) == -1, "indent: nothing to detect");
    buffer_destroy(buf);

    // The commands.
    TestView tv;
    if (!test_view_open(t, &tv, "int f() {|", 10, 40)) return 0;
    test_view_run(&tv, &CMD_NEWLINE);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "int f() {\n    |"), "newline indents the new line");
    test_type_char(&tv, 'x');
    test_view_run(&tv, &CMD_NEWLINE);
    test_view_run(&tv, &CMD_NEWLINE);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "int f() {\n    x\n\n    |"), "newline empties the blank line it leaves");
    test_type_char(&tv, '}');
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "int f() {\n    x\n\n}|"), "a closing brace typed first reindents the line");
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "int f() {\n    x\n\n    |"), "the brace and its reindent are one undo");
    // TAB: from inside the indentation to the text; in the text point stays with it.
    buffer_replace(tv.buf, 0, buffer_size(tv.buf), STR8_LIT("if (a) {\nfoo(b);\n}"));
    test_view_run(&tv, &CMD_BEGINNING_OF_BUFFER);
    test_view_run(&tv, &CMD_NEXT_LINE);
    test_view_run(&tv, &CMD_INDENT_FOR_TAB_COMMAND);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "if (a) {\n    |foo(b);\n}"), "TAB from the line start");
    test_view_run(&tv, &CMD_FORWARD_WORD);
    test_view_run(&tv, &CMD_UNINDENT);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "if (a) {\nfoo|(b);\n}"), "backtab: one level less, point with the text");
    test_view_run(&tv, &CMD_INDENT_FOR_TAB_COMMAND);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "if (a) {\n    foo|(b);\n}"), "TAB in the text keeps point with it");
    // TAB over a region reindents every line.
    buffer_replace(tv.buf, 0, buffer_size(tv.buf), STR8_LIT("  a {\n        b;\n   \n c;\n}\n"));
    test_view_run(&tv, &CMD_MARK_WHOLE_BUFFER);
    test_view_run(&tv, &CMD_INDENT_FOR_TAB_COMMAND);
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("a {\n    b;\n\n    c;\n}\n")) && !tv.view->cursors[0].mark_active,
               "TAB over a region");
    // M-i to the next stop; C-q handled by the keymap (quoted state) inserts literally.
    buffer_replace(tv.buf, 0, buffer_size(tv.buf), STR8_LIT("ab"));
    test_view_run(&tv, &CMD_END_OF_BUFFER);
    test_view_run(&tv, &CMD_TAB_TO_TAB_STOP);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "ab  |"), "M-i to the next stop");
    if (!test_view_close(t, &tv)) return 0;

    // On tokens, typed: RET into a block comment keeps the previous line's indentation; a switch typed
    // with RET and closers only; TAB leaves a preprocessor line where it is.
    if (!test_view_open(t, &tv, "    /* a|", 10, 40)) return 0;
    tv.buf->language = BUFFER_LANG_C;
    test_view_run(&tv, &CMD_NEWLINE);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "    /* a\n    |"), "RET into a block comment");
    buffer_replace(tv.buf, 0, buffer_size(tv.buf), STR8_LIT(""));
    // A label typed after a statement is reindented by the ':' that completes it (electric labels).
    const char *typed = "switch (x) {\ncase 1:\na();\nbreak;\ndefault: {\nb();\n}\n}\n";
    for (const char *c = typed; *c; c++) {
        if (*c == '\n') test_view_run(&tv, &CMD_NEWLINE);
        else if (*c == '\t') test_view_run(&tv, &CMD_INDENT_FOR_TAB_COMMAND);
        else test_type_char(&tv, (u8)*c);
    }
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("switch (x) {\n    case 1:\n        a();\n        break;\n    default: {\n"
                                                    "        b();\n    }\n}\n")), "a switch typed with RET and closers");
    buffer_replace(tv.buf, 0, buffer_size(tv.buf), STR8_LIT("{\n#if X\n}"));
    test_view_run(&tv, &CMD_MARK_WHOLE_BUFFER);
    test_view_run(&tv, &CMD_INDENT_FOR_TAB_COMMAND);
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("{\n#if X\n}")), "TAB leaves a preprocessor line");
    if (!test_view_close(t, &tv)) return 0;
    LOG("test: ok: indentation: %d rule cases, tabs, detection, newline, closing brace, TAB, backtab, region, M-i, "
        "RET into a comment, a typed switch, preprocessor lines", (i32)ARRAY_COUNT(cases));
    return 1;
}

// Electric labels: the character that completes a case or default label (':', Jai ';') reindents its
// line when only blanks follow it, in the same undo group as the character. `before` has point where
// the character is typed; the previous command was a motion, so the character starts its own group.
static b32 test_electric_labels(Test *t) {
    static const struct {
        BufferLanguage language;
        const char *what, *before;
        u32 c;
        const char *after;
    } cases[] = {
        { BUFFER_LANG_C, "C case", "switch (x) {\n    case 0:\n        a();\n        case 1|\n}", ':',
          "switch (x) {\n    case 0:\n        a();\n    case 1:|\n}" },
        { BUFFER_LANG_C, "C default", "switch (x) {\n    case 0:\n        a();\n        break;\n        default|\n}", ':',
          "switch (x) {\n    case 0:\n        a();\n        break;\n    default:|\n}" },
        { BUFFER_LANG_C, "C label at the right place already", "switch (x) {\n    case 1|\n}", ':', "switch (x) {\n    case 1:|\n}" },
        { BUFFER_LANG_CPP, "C++ case with blanks after point", "switch (x) {\ncase A::B|  \n}", ':', "switch (x) {\n    case A::B:|  \n}" },
        { BUFFER_LANG_CSHARP, "C# case", "switch (x) {\n    case 0:\n        a();\n        break;\n        case \"b\"|\n}", ':',
          "switch (x) {\n    case 0:\n        a();\n        break;\n    case \"b\":|\n}" },
        { BUFFER_LANG_JAVASCRIPT, "JavaScript default", "switch (x) {\n    case 0:\n        a();\n        default|\n}", ':',
          "switch (x) {\n    case 0:\n        a();\n    default:|\n}" },
        { BUFFER_LANG_JAI, "Jai case", "if x == {\n    case 0;\n        a();\n        case 1|\n}", ';',
          "if x == {\n    case 0;\n        a();\n    case 1;|\n}" },
        { BUFFER_LANG_JAI, "Jai: ':' completes nothing", "if x == {\n    case 0;\n        a();\n        case 1|\n}", ':',
          "if x == {\n    case 0;\n        a();\n        case 1:|\n}" },
        { BUFFER_LANG_C, "C: ';' completes nothing", "switch (x) {\n        case 1|\n}", ';', "switch (x) {\n        case 1;|\n}" },
        { BUFFER_LANG_C, "not a label (a conditional)", "int f() {\n        x = a ? b |\n}", ':', "int f() {\n        x = a ? b :|\n}" },
        { BUFFER_LANG_C, "text after point", "switch (x) {\n        case 1| a();\n}", ':', "switch (x) {\n        case 1:| a();\n}" },
        { BUFFER_LANG_C, "in a string", "switch (x) {\n        f(\"case 1|\");\n}", ':', "switch (x) {\n        f(\"case 1:|\");\n}" },
        { BUFFER_LANG_FUNDAMENTAL, "Fundamental", "switch (x) {\n        case 1|\n}", ':', "switch (x) {\n        case 1:|\n}" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        TestView tv;
        if (!test_view_open(t, &tv, cases[i].before, 10, 60)) return 0;
        tv.buf->language = cases[i].language;
        String8 before = str8_copy(&t->arena, buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)));
        i64 point = view_point(tv.view, &tv.view->cursors[0]);
        test_view_run(&tv, &CMD_FORWARD_CHAR); // a motion: the character starts a new undo group
        test_view_run(&tv, &CMD_BACKWARD_CHAR);
        test_type_char(&tv, cases[i].c);
        char *got = test_view_marked(t, &tv);
        TEST_CHECK(t, test_cstr_equal(got, cases[i].after), "electric label: %s: got '%s'", cases[i].what, got);
        test_view_run(&tv, &CMD_UNDO);
        TEST_CHECK(t, test_text_is(t, tv.buf, before) && view_point(tv.view, &tv.view->cursors[0]) == point,
                   "electric label: %s: one undo restores the line and point", cases[i].what);
        if (!test_view_close(t, &tv)) return 0;
    }
    LOG("test: ok: electric labels, %d cases with their undo", (i32)ARRAY_COUNT(cases));
    return 1;
}

// ---------------------------------------------------------------------------
// The search engine against a naive reference

// The naive search: every start in the range, on a character boundary when the needle starts with a
// continuation byte, compared after folding the candidate's bytes with the matcher's match_fold.
static i64 test_search_ref(Test *t, TestRef *ref, String8 needle, b32 fold, b32 forward, i64 from, i64 lo, i64 hi) {
    if (!needle.len) return -1;
    lo = CLAMP(lo, 0, ref->len);
    hi = CLAMP(hi, lo, ref->len);
    from = CLAMP(from, lo, hi);
    u64 mark = arena_pos(&t->arena);
    String8 want = fold ? match_fold(&t->arena, needle) : needle;
    b32 boundary = (needle.data[0] & 0xC0) == 0x80;
    i64 found = -1;
    i64 first = forward ? from : from - needle.len, step = forward ? 1 : -1;
    for (i64 c = first; found < 0 && c >= lo && c + needle.len <= hi; c += step) {
        if (boundary && test_ref_snap(ref, c) != c) continue;
        String8 cand = str8(ref->data + c, needle.len);
        if (fold) cand = match_fold(&t->arena, cand);
        if (str8_equal(cand, want)) found = c;
    }
    arena_pop_to(&t->arena, mark);
    return found;
}

// Runs a search to its end, in slices of `slice` positions (0: one uncut run). The match start or -1.
static i64 test_search_run(Search *s, Buffer *buf, i64 slice, i32 *slices) {
    *slices = 0;
    SearchStatus st;
    do {
        st = search_run(s, buf, slice ? slice : I64_MAX / 4);
        (*slices)++;
    } while (st == SEARCH_RUNNING);
    return st == SEARCH_FOUND ? s->match_start : -1;
}

static b32 test_search(Test *t, u64 seed) {
    t->rng = seed ^ 0x5EA7C4;
    // Pieces with case pairs of every kind the tables have, lead bytes that differ between the cases
    // (ÿ / Ÿ, р / Р), the Turkish i's, sigma, invalid bytes, newlines.
    static const char *pieces[] = { "a", "A", "b", "B", "ab", "Ab", "i", "I", "\xc4\xb1", "\xc4\xb0", "\xc3\xa9", "\xc3\x89",
                                    "\xc3\xbf", "\xc5\xb8", "\xcf\x83", "\xce\xa3", "\xcf\x82", "\xd0\xb4", "\xd0\x94",
                                    "\xd1\x80", "\xd0\xa0", "\x80", "\xc3", "\n", " ", "aa", "x" };
    Search *s = PUSH_STRUCT(&t->arena, Search);
    TestRef ref = { 0 };
    ref.cap = KB(4);
    ref.data = PUSH_ARRAY(&t->arena, u8, ref.cap);
    i64 searches = 0, found = 0, straddled = 0, sliced_runs = 0;
    for (i32 round = 0; round < 1500; round++) {
        // A haystack of up to ~300 bytes.
        ref.len = 0;
        i32 count = (i32)test_below(t, 120);
        for (i32 k = 0; k < count; k++) {
            String8 p = str8_cstr(pieces[test_below(t, ARRAY_COUNT(pieces))]);
            memcpy(ref.data + ref.len, p.data, (size_t)p.len);
            ref.len += p.len;
        }
        Buffer *buf = buffer_create(STR8_LIT("search"));
        TEST_CHECK(t, buf, "search: buffer_create failed");
        buffer_replace(buf, 0, 0, str8(ref.data, ref.len));
        for (i32 q = 0; q < 12; q++) {
            // A needle: pieces, or bytes cut from the haystack (possibly starting inside a character),
            // the latter sometimes with its ASCII case flipped.
            u8 nbuf[64];
            String8 needle;
            if (ref.len && test_below(t, 2)) {
                i64 at = test_below(t, ref.len), n = 1 + test_below(t, MIN(8, ref.len - at));
                memcpy(nbuf, ref.data + at, (size_t)n);
                if (test_below(t, 2)) for (i64 k = 0; k < n; k++) if ((nbuf[k] | 0x20) >= 'a' && (nbuf[k] | 0x20) <= 'z') nbuf[k] ^= 0x20;
                needle = str8(nbuf, n);
            } else {
                i64 n = 0;
                for (i32 k = 0, pieces_n = 1 + (i32)test_below(t, 3); k < pieces_n; k++) {
                    String8 p = str8_cstr(pieces[test_below(t, ARRAY_COUNT(pieces))]);
                    memcpy(nbuf + n, p.data, (size_t)p.len);
                    n += p.len;
                }
                needle = str8(nbuf, n);
            }
            b32 fold = (b32)test_below(t, 2), forward = (b32)test_below(t, 2);
            i64 lo = test_below(t, 4) ? 0 : test_below(t, ref.len + 1);
            i64 hi = test_below(t, 4) ? ref.len : lo + test_below(t, ref.len - lo + 1);
            i64 from = test_below(t, 3) ? test_below(t, ref.len + 1) : forward ? lo : hi;
            i64 want = test_search_ref(t, &ref, needle, fold, forward, from, lo, hi);
            // The gap: random, or inside the expected match.
            i64 gap = want >= 0 && needle.len > 1 && test_below(t, 2) ? want + 1 + test_below(t, needle.len - 1) : test_below(t, ref.len + 1);
            buffer_replace(buf, gap, gap, STR8_LIT("#"));
            buffer_replace(buf, gap, gap + 1, STR8_LIT(""));
            straddled += want >= 0 && gap > want && gap < want + needle.len;
            search_begin(s, buf, needle, fold, forward, from, lo, hi);
            i32 slices;
            i64 got = test_search_run(s, buf, 0, &slices);
            TEST_CHECK(t, got == want && (got < 0 || s->match_end == got + needle.len),
                       "search: round %d query %d (seed 0x%X): '%S' fold %d forward %d from %D in [%D, %D), gap %D: got %D, want %D",
                       round, q, seed, needle, fold, forward, from, lo, hi, gap, got, want);
            i64 slice = 1 + test_below(t, 64);
            search_restart(s, buf, forward, from, lo, hi);
            i64 cut = test_search_run(s, buf, slice, &slices);
            TEST_CHECK(t, cut == want, "search: round %d query %d (seed 0x%X): in slices of %D: got %D, want %D", round, q, seed,
                       slice, cut, want);
            sliced_runs += slices > 1;
            searches += 2;
            found += want >= 0;
        }
        buffer_destroy(buf);
    }

    // Fixed cases: matches at both ends, overlapping candidates, the range, an edit restarting a
    // search that is under way.
    Buffer *buf = buffer_create(STR8_LIT("search"));
    buffer_replace(buf, 0, 0, STR8_LIT("abcab aaaaa"));
    struct { const char *needle; b32 forward; i64 from, lo, hi, want; } fixed[] = {
        { "ab", 1, 0, 0, 11, 0 }, { "ab", 0, 11, 0, 11, 3 }, { "ab", 1, 1, 0, 11, 3 }, { "ab", 0, 4, 0, 11, 0 },
        { "aaa", 1, 7, 0, 11, 7 }, { "aaa", 1, 8, 0, 11, 8 }, { "aaa", 1, 9, 0, 11, -1 }, { "aaa", 0, 11, 0, 11, 8 },
        { "aaa", 0, 10, 0, 11, 7 }, { "a", 0, 11, 0, 11, 10 }, { "ab", 1, 0, 1, 11, 3 }, { "ab", 0, 11, 0, 4, 0 },
        { "ab", 1, 3, 0, 4, -1 },
    };
    for (i32 i = 0; i < ARRAY_COUNT(fixed); i++) {
        search_begin(s, buf, str8_cstr(fixed[i].needle), 0, fixed[i].forward, fixed[i].from, fixed[i].lo, fixed[i].hi);
        i32 slices;
        i64 got = test_search_run(s, buf, 0, &slices);
        TEST_CHECK(t, got == fixed[i].want, "search: fixed case %d ('%s'): got %D, want %D", i, fixed[i].needle, got, fixed[i].want);
    }
    buffer_replace(buf, 0, buffer_size(buf), STR8_LIT("xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxneedle"));
    search_begin(s, buf, STR8_LIT("needle"), 0, 1, 0, 0, buffer_size(buf));
    TEST_CHECK(t, search_run(s, buf, 10) == SEARCH_RUNNING && search_progress(s) > 0 && search_progress(s) < 100,
               "search: a slice leaves it running, with progress");
    buffer_replace(buf, 0, 0, STR8_LIT("needle")); // an edit while it runs: it starts over from `from`
    i32 slices;
    TEST_CHECK(t, test_search_run(s, buf, 10, &slices) == 0, "search: restarted after an edit");
    // A needle too long, an empty one.
    u8 *big = PUSH_ARRAY(&t->arena, u8, SEARCH_NEEDLE_MAX + 1);
    memset(big, 'x', SEARCH_NEEDLE_MAX + 1);
    TEST_CHECK(t, !search_begin(s, buf, str8(big, SEARCH_NEEDLE_MAX + 1), 0, 1, 0, 0, buffer_size(buf)) && s->status == SEARCH_NOT_FOUND,
               "search: a needle longer than SEARCH_NEEDLE_MAX is refused");
    TEST_CHECK(t, search_begin(s, buf, str8(big, SEARCH_NEEDLE_MAX), 0, 1, 0, 0, buffer_size(buf)), "search: SEARCH_NEEDLE_MAX bytes");
    TEST_CHECK(t, !search_begin(s, buf, STR8_LIT(""), 0, 1, 0, 0, buffer_size(buf)), "search: an empty needle is refused");
    // The known limit: Turkish dotted and dotless i do not pair.
    buffer_replace(buf, 0, buffer_size(buf), STR8_LIT("\xc4\xb0 I"));
    search_begin(s, buf, STR8_LIT("i"), 1, 1, 0, 0, buffer_size(buf));
    TEST_CHECK(t, test_search_run(s, buf, 0, &slices) == 3, "search: 'i' folded finds 'I', not the dotted capital I");
    search_begin(s, buf, STR8_LIT("\xc4\xb1"), 1, 1, 0, 0, buffer_size(buf));
    TEST_CHECK(t, test_search_run(s, buf, 0, &slices) == -1, "search: a dotless i folded does not find 'I' (known limit)");
    buffer_destroy(buf);

    // Smart case.
    static const struct { const char *s; b32 upper; } cases[] = {
        { "abc", 0 }, { "aBc", 1 }, { "\xc5\x9f", 0 }, { "\xc5\x9e", 1 }, { "\xc4\xb1", 0 }, { "\xc4\xb0", 1 }, { "12_+", 0 },
        { "\x80\xc3", 0 }, { "\xd0\x94", 1 }, { "\xcf\x82", 0 },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        TEST_CHECK(t, search_has_upper(str8_cstr(cases[i].s)) == cases[i].upper, "search: search_has_upper case %d", i);
    }
    // The fast path's assumption: every character folding to f starts with one of f's first bytes.
    for (u32 c = 0x80; c < 0x3000; c++) {
        if (c >= 0xD800 && c <= 0xDFFF) continue;
        u32 f = search_fold_char(c);
        u8 enc[4], first[2];
        utf8_encode(c, enc);
        i32 n = search_first_bytes(f, first);
        TEST_CHECK(t, enc[0] == first[0] || (n == 2 && enc[0] == first[1]), "search: U+%x folds to U+%x, whose first bytes miss it", c, f);
    }
    LOG("test: ok: search: %D searches against the reference (%D found, %D across the gap, %D runs in several slices), "
        "%d fixed cases, restart after an edit, needle limits, Turkish i (known limit), smart case, first bytes",
        searches, found, straddled, sliced_runs, (i32)ARRAY_COUNT(fixed));
    return 1;
}

// ---------------------------------------------------------------------------
// Other editing commands, on tricky input

typedef struct TestEditCase {
    const Command *command;
    const char *before, *after; // '|' marks point
    const char *message;        // expected echo, or NULL
} TestEditCase;

static b32 test_edit_commands(Test *t) {
    static const TestEditCase cases[] = {
        { &CMD_OPEN_LINE, "ab|cd", "ab|\ncd", NULL },
        { &CMD_OPEN_LINE, "abcd|", "abcd|\n", NULL },
        { &CMD_DELETE_INDENTATION, "foo  \n   |bar", "foo| bar", NULL },
        { &CMD_DELETE_INDENTATION, "foo(\n  ba|r", "foo(|bar", NULL },
        { &CMD_DELETE_INDENTATION, "foo\n  )|;", "foo|);", NULL },
        { &CMD_DELETE_INDENTATION, "\n  |bar", "|bar", NULL },
        { &CMD_DELETE_INDENTATION, "fo|o", "fo|o", NULL },
        { &CMD_DELETE_INDENTATION, "\xc5\x9f\n\xc4\x9f|", "\xc5\x9f| \xc4\x9f", NULL },
        { &CMD_DELETE_HORIZONTAL_SPACE, "a \t | \tb", "a|b", NULL },
        { &CMD_DELETE_HORIZONTAL_SPACE, "a|b", "a|b", NULL },
        { &CMD_JUST_ONE_SPACE, "a \t| \tb", "a |b", NULL },
        { &CMD_JUST_ONE_SPACE, "a|b", "a |b", NULL },
        { &CMD_JUST_ONE_SPACE, "a |b", "a |b", NULL },
        { &CMD_TRANSPOSE_CHARS, "ab|cd", "acb|d", NULL },
        { &CMD_TRANSPOSE_CHARS, "abc|\nd", "acb|\nd", NULL },
        { &CMD_TRANSPOSE_CHARS, "abc|", "acb|", NULL },
        { &CMD_TRANSPOSE_CHARS, "|abc", "|abc", "Beginning of buffer" },
        { &CMD_TRANSPOSE_CHARS, "a|", "a|", "Beginning of buffer" },
        { &CMD_TRANSPOSE_CHARS, "a\n|b", "ab\n|", NULL },
        { &CMD_TRANSPOSE_CHARS, "x\xc5\x9f|\xc4\x9f", "x\xc4\x9f\xc5\x9f|", NULL },
        { &CMD_UPCASE_WORD, "|hello world", "HELLO| world", NULL },
        { &CMD_UPCASE_WORD, "hel|lo world", "helLO| world", NULL },
        { &CMD_UPCASE_WORD, "| \xc4\xb1\xc5\x9f\xc3\x9f.", " I\xc5\x9e\xc3\x9f|.", NULL },
        { &CMD_DOWNCASE_WORD, "|\xc5\x9e\xc4\x9e\xc3\x9c \xc4\xb0X", "\xc5\x9f\xc4\x9f\xc3\xbc| \xc4\xb0X", NULL },
        { &CMD_DOWNCASE_WORD, "|", "|", NULL },
        { &CMD_CAPITALIZE_WORD, "|hELLO wORLD", "Hello| wORLD", NULL },
        { &CMD_CAPITALIZE_WORD, "|  \xd0\xbf\xd0\xa0\xd0\x98", "  \xd0\x9f\xd1\x80\xd0\xb8|", NULL },
        { &CMD_COMMENT_LINE, "  fo|o();\nbar", "  // foo();\n|bar", NULL },
        { &CMD_COMMENT_LINE, "  // fo|o();\nbar", "  foo();\n|bar", NULL },
        { &CMD_COMMENT_LINE, "  //fo|o();", "  foo();|", NULL },
        { &CMD_COMMENT_LINE, "   |", "   |", NULL },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        TestView tv;
        if (!test_view_open(t, &tv, cases[i].before, 10, 40)) return 0;
        test_view_run(&tv, cases[i].command);
        char *got = test_view_marked(t, &tv);
        b32 message_ok = !cases[i].message || test_echo_is(&tv, cases[i].message);
        TEST_CHECK(t, test_cstr_equal(got, cases[i].after) && message_ok, "edit: %s case %d: got '%s', expected '%s'",
                   cases[i].command->name, i, got, cases[i].after);
        if (!test_view_close(t, &tv)) return 0;
    }
    // comment-line over a region: the smallest indentation, blank lines left alone; again: uncommented.
    TestView tv;
    if (!test_view_open(t, &tv, "|if (a) {\n    b();\n\n    c();\n}\nnext", 10, 40)) return 0;
    test_view_run(&tv, &CMD_SET_MARK_COMMAND);
    for (i32 i = 0; i < 5; i++) test_view_run(&tv, &CMD_NEXT_LINE);
    test_view_run(&tv, &CMD_COMMENT_LINE);
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("// if (a) {\n//     b();\n\n//     c();\n// }\nnext")), "edit: comment a region");
    test_view_run(&tv, &CMD_BEGINNING_OF_BUFFER);
    test_view_run(&tv, &CMD_SET_MARK_COMMAND);
    for (i32 i = 0; i < 5; i++) test_view_run(&tv, &CMD_NEXT_LINE);
    test_view_run(&tv, &CMD_COMMENT_LINE);
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("if (a) {\n    b();\n\n    c();\n}\nnext")), "edit: uncomment the region");
    test_view_run(&tv, &CMD_UNDO);
    test_view_run(&tv, &CMD_UNDO);
    TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("if (a) {\n    b();\n\n    c();\n}\nnext")) && !tv.buf->modified,
               "edit: two undos give the original");
    if (!test_view_close(t, &tv)) return 0;
    LOG("test: ok: editing commands, %d cases (open-line, delete-indentation, whitespace, transpose, case, comment-line)",
        (i32)ARRAY_COUNT(cases) + 3);
    return 1;
}

// ---------------------------------------------------------------------------
// Undo fuzz: random edits, kills, yanks, indentation, undo and undo-redo through the driver.
// Invariants: every state id stands for one text (a state seen again has the same text); undo
// and redo only lead to states seen before; modified exactly when the state is not the saved
// one; point is a valid character boundary.

#define TEST_UNDO_FUZZ_OPS 4000
#define TEST_UNDO_STATES (1 << 18)

static b32 test_undo_fuzz(Test *t, u64 seed) {
    TestView tv;
    if (!test_view_open(t, &tv, "|int main() {\n    return 0;\n}\n\nfoo bar \xc5\x9f\xc4\x9f\xc3\xbc\n", 20, 60)) return 0;
    String8 *texts = PUSH_ARRAY(&t->arena, String8, TEST_UNDO_STATES);
    texts[tv.buf->undo.state] = str8_copy(&t->arena, buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)));
    static const Command *edits[] = {
        &CMD_NEWLINE, &CMD_DELETE_BACKWARD_CHAR, &CMD_DELETE_CHAR, &CMD_KILL_LINE, &CMD_KILL_WORD, &CMD_BACKWARD_KILL_WORD,
        &CMD_KILL_WHOLE_LINE, &CMD_YANK, &CMD_YANK_POP, &CMD_KILL_REGION, &CMD_KILL_RING_SAVE, &CMD_INDENT_FOR_TAB_COMMAND,
        &CMD_UNINDENT, &CMD_TAB_TO_TAB_STOP, &CMD_OPEN_LINE, &CMD_DELETE_INDENTATION, &CMD_JUST_ONE_SPACE,
        &CMD_DELETE_HORIZONTAL_SPACE, &CMD_TRANSPOSE_CHARS, &CMD_UPCASE_WORD, &CMD_DOWNCASE_WORD, &CMD_CAPITALIZE_WORD,
        &CMD_COMMENT_LINE, &CMD_MARK_WHOLE_BUFFER,
    };
    static const Command *motions[] = {
        &CMD_FORWARD_CHAR, &CMD_BACKWARD_CHAR, &CMD_NEXT_LINE, &CMD_PREVIOUS_LINE, &CMD_FORWARD_WORD, &CMD_BACKWARD_WORD,
        &CMD_MOVE_BEGINNING_OF_LINE, &CMD_MOVE_END_OF_LINE, &CMD_BEGINNING_OF_BUFFER, &CMD_END_OF_BUFFER, &CMD_SET_MARK_COMMAND,
        &CMD_KEYBOARD_QUIT, &CMD_FORWARD_PARAGRAPH,
    };
    static const u32 chars[] = { 'a', 'Z', ' ', '{', '}', '(', ')', ';', 0x15F, 0x131, '\t' };
    t->rng = seed ^ 0x0f0;
    u64 t0 = os_time_us();
    i64 undos = 0, redos = 0, known = 0;
    for (i32 op = 0; op < TEST_UNDO_FUZZ_OPS; op++) {
        u64 r = test_below(t, 100);
        const Command *cmd;
        b32 shift = 0;
        if (r < 30) {
            cmd = &CMD_SELF_INSERT;
            tv.ctx.codepoint = chars[test_below(t, ARRAY_COUNT(chars))];
        } else if (r < 55) {
            cmd = edits[test_below(t, ARRAY_COUNT(edits))];
        } else if (r < 75) {
            cmd = motions[test_below(t, ARRAY_COUNT(motions))];
            shift = test_below(t, 4) == 0;
        } else if (r < 92) {
            cmd = &CMD_UNDO;
        } else {
            cmd = &CMD_UNDO_REDO;
        }
        u64 before = tv.buf->undo.state;
        tv.ctx.shift_translated = shift;
        test_view_run(&tv, cmd);
        tv.ctx.shift_translated = 0;
        u64 state = tv.buf->undo.state;
        String8 text = buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf));
        TEST_CHECK(t, state < TEST_UNDO_STATES, "undo fuzz op %d (seed 0x%X): state %U past the test's table", op, seed, state);
        if (texts[state].data) {
            TEST_CHECK(t, str8_equal(texts[state], text), "undo fuzz op %d (seed 0x%X, %s): state %U came back with another text",
                       op, seed, cmd->name, state);
            known += state != before;
        } else {
            TEST_CHECK(t, (cmd != &CMD_UNDO && cmd != &CMD_UNDO_REDO) || state == before,
                       "undo fuzz op %d (seed 0x%X, %s): led to a state never seen", op, seed, cmd->name);
            texts[state] = str8_copy(&t->arena, text);
        }
        undos += cmd == &CMD_UNDO && state != before;
        redos += cmd == &CMD_UNDO_REDO && state != before;
        TEST_CHECK(t, tv.buf->modified == (state != tv.buf->undo.saved_state), "undo fuzz op %d (seed 0x%X): modified flag", op, seed);
        i64 p = view_point(tv.view, &tv.view->cursors[0]);
        TEST_CHECK(t, p >= 0 && p <= buffer_size(tv.buf) && buffer_snap_char(tv.buf, p) == p, "undo fuzz op %d: point %D", op, p);
        if (buffer_size(tv.buf) > 4000) { // keep the text small
            test_view_run(&tv, &CMD_MARK_WHOLE_BUFFER);
            test_view_run(&tv, &CMD_KILL_REGION);
            texts[tv.buf->undo.state] = str8_copy(&t->arena, buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)));
        }
    }
    u64 t1 = os_time_us();
    // Undo all the way back: the original text, unmodified.
    test_view_run(&tv, &CMD_FORWARD_CHAR);
    for (i32 i = 0; i < 100000 && tv.buf->undo.state != tv.buf->undo.saved_state; i++) {
        test_view_run(&tv, &CMD_UNDO);
        if (test_echo_is(&tv, "No further undo information")) break;
    }
    TEST_CHECK(t, !tv.buf->modified && str8_equal(buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)), texts[tv.buf->undo.saved_state]),
               "undo fuzz (seed 0x%X): undoing everything does not give the original text", seed);
    if (!test_view_close(t, &tv)) return 0;
    LOG("test: ok: undo fuzz, %d commands (seed 0x%X): %D undos and %D redos changed the text, %D returns to known states; "
        "%U ms, undoing everything %U ms", (i32)TEST_UNDO_FUZZ_OPS, seed, undos, redos, known, (t1 - t0) / 1000,
        (os_time_us() - t1) / 1000);
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
        { KEY_A, 'A', KEYMOD_CTRL | KEYMOD_SHIFT, "C-S-a" },   // letters keep Shift
        { KEY_A, 'a', KEYMOD_CTRL, "C-a" },
        { KEY_A, 'A', KEYMOD_CTRL, "C-a" },                 // Caps Lock: no Shift held
        { KEY_A, 'a', KEYMOD_CTRL | KEYMOD_SHIFT, "C-S-a" },   // Caps Lock and Shift
        { KEY_SLASH, '?', KEYMOD_CTRL | KEYMOD_SHIFT, "C-?" }, // symbols absorb Shift
        { KEY_7, '/', KEYMOD_CTRL | KEYMOD_SHIFT, "C-/" },     // "/" is Shift+7 on Turkish Q
        { KEY_2, '@', KEYMOD_CTRL | KEYMOD_SHIFT, "C-@" },
        { KEY_7, '{', KEYMOD_ALT, "M-{" },                  // AltGr+7 with Left Alt (Turkish Q)
        { KEY_Q, '@', KEYMOD_CTRL | KEYMOD_ALT, "C-M-@" },     // AltGr+q with Right Ctrl and Left Alt
        { KEY_SPACE, ' ', KEYMOD_CTRL, "C-SPC" },
        { KEY_LEFT, 0, KEYMOD_SHIFT, "S-<left>" },          // named keys keep every modifier
        { KEY_F5, 0, 0, "<f5>" },
        { KEY_ENTER, 0, KEYMOD_CTRL, "C-RET" },
        { KEY_TAB, 0, KEYMOD_SHIFT, "<backtab>" },
        { KEY_BACKSPACE, 0, KEYMOD_ALT, "M-DEL" },
        { KEY_HOME, 0, KEYMOD_CTRL | KEYMOD_ALT | KEYMOD_SHIFT, "C-M-S-<home>" },
        { KEY_SEMICOLON, 0x15E, KEYMOD_CTRL | KEYMOD_SHIFT, "C-S-\xc5\x9f" }, // Ş on Turkish Q
        { KEY_I, 0x130, KEYMOD_CTRL | KEYMOD_SHIFT, "C-S-i" },                // İ: default case mapping
        { KEY_NONE, 'x', KEYMOD_ALT, "M-x" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        KeyChord c = 0;
        TEST_CHECK(t, key_chord_from_event(cases[i].key, cases[i].cp, cases[i].mods, &c) && test_chord_is(c, cases[i].chord),
                   "chords: case %d (expected %s)", i, cases[i].chord);
    }
    // Not chords: plain or shifted characters (their text event follows), keys without a character.
    static const struct { Key key; u32 cp; u32 mods; } none[] = {
        { KEY_A, 'a', 0 }, { KEY_A, 'A', KEYMOD_SHIFT }, { KEY_SPACE, ' ', 0 }, { KEY_7, '{', 0 },
        { KEY_OEM_102, 0, KEYMOD_CTRL }, { KEY_NONE, 0, KEYMOD_ALT },
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
    TEST_CHECK(t, test_key(&k, KEY_X, 'x', KEYMOD_CTRL) == KEY_RESULT_PREFIX && test_seq_is(&k.r.seq, "C-x"), "keys: C-x is a prefix");
    TEST_CHECK(t, test_key(&k, KEY_S, 's', KEYMOD_CTRL) == KEY_RESULT_COMMAND && k.r.command == &CMD_SAVE_BUFFER &&
                  k.in.pending.len == 0, "keys: C-x C-s runs save-buffer");
    // Undefined resets the state.
    TEST_CHECK(t, test_key(&k, KEY_X, 'x', KEYMOD_CTRL) == KEY_RESULT_PREFIX, "keys: C-x again");
    TEST_CHECK(t, test_key(&k, KEY_Q, 'q', KEYMOD_CTRL) == KEY_RESULT_UNDEFINED && test_seq_is(&k.r.seq, "C-x C-q") &&
                  k.in.pending.len == 0, "keys: C-x C-q is undefined");
    TEST_CHECK(t, test_key(&k, KEY_F, 'f', KEYMOD_CTRL) == KEY_RESULT_COMMAND && k.r.command == &CMD_FORWARD_CHAR,
               "keys: C-f after an undefined sequence");
    // keyboard-quit cancels a prefix; alone it is a command.
    test_key(&k, KEY_X, 'x', KEYMOD_CTRL);
    TEST_CHECK(t, test_key(&k, KEY_G, 'g', KEYMOD_CTRL) == KEY_RESULT_QUIT && k.in.pending.len == 0, "keys: C-x C-g quits");
    test_key(&k, KEY_X, 'x', KEYMOD_CTRL);
    TEST_CHECK(t, test_key(&k, KEY_ESCAPE, 0, 0) == KEY_RESULT_QUIT, "keys: C-x ESC quits");
    TEST_CHECK(t, test_key(&k, KEY_G, 'g', KEYMOD_CTRL) == KEY_RESULT_COMMAND && k.r.command == &CMD_KEYBOARD_QUIT,
               "keys: C-g alone runs keyboard-quit");
    // Shift-translation: an unbound chord with Shift is looked up without it.
    TEST_CHECK(t, test_key(&k, KEY_LEFT, 0, KEYMOD_SHIFT) == KEY_RESULT_COMMAND && k.r.command == &CMD_BACKWARD_CHAR &&
                  k.r.shift_translated, "keys: S-<left> is shift-translated to backward-char");
    TEST_CHECK(t, test_key(&k, KEY_F, 'F', KEYMOD_CTRL | KEYMOD_SHIFT) == KEY_RESULT_COMMAND && k.r.command == &CMD_FORWARD_CHAR &&
                  k.r.shift_translated, "keys: C-S-f is shift-translated to forward-char");
    TEST_CHECK(t, test_key(&k, KEY_LEFT, 0, 0) == KEY_RESULT_COMMAND && !k.r.shift_translated, "keys: <left> is not translated");
    TEST_CHECK(t, test_key(&k, KEY_A, 'A', KEYMOD_CTRL | KEYMOD_SHIFT) == KEY_RESULT_COMMAND && k.r.command == &CMD_BEGINNING_OF_BUFFER &&
                  !k.r.shift_translated, "keys: a bound C-S-a is not translated");
    TEST_CHECK(t, test_key(&k, KEY_X, 'X', KEYMOD_CTRL | KEYMOD_SHIFT) == KEY_RESULT_PREFIX && k.r.shift_translated &&
                  test_key(&k, KEY_S, 's', KEYMOD_CTRL) == KEY_RESULT_COMMAND && k.r.command == &CMD_SAVE_BUFFER,
               "keys: C-S-x is translated to the C-x prefix");
    TEST_CHECK(t, test_key(&k, KEY_Q, 'Q', KEYMOD_CTRL | KEYMOD_SHIFT) == KEY_RESULT_UNDEFINED && test_seq_is(&k.r.seq, "C-S-q"),
               "keys: C-S-q is undefined, reported as typed");
    // Text from a consumed KEY_DOWN is dropped up to the next KEY_DOWN.
    TEST_CHECK(t, test_key(&k, KEY_F, 'f', KEYMOD_CTRL) == KEY_RESULT_COMMAND && test_text(&k, 'f') == KEY_RESULT_DROPPED &&
                  test_text(&k, 'g') == KEY_RESULT_DROPPED, "keys: text after a consumed KEY_DOWN is dropped");
    TEST_CHECK(t, test_key(&k, KEY_A, 'a', 0) == KEY_RESULT_IGNORED && test_text(&k, 'a') == KEY_RESULT_SELF_INSERT &&
                  k.r.command == &CMD_SELF_INSERT && k.r.codepoint == 'a', "keys: a plain character self-inserts");
    TEST_CHECK(t, test_text(&k, 0x15F) == KEY_RESULT_SELF_INSERT && k.r.codepoint == 0x15F,
               "keys: composed text without a KEY_DOWN (dead keys, IME) self-inserts");
    // A plain character as the second key.
    TEST_CHECK(t, test_key(&k, KEY_X, 'x', KEYMOD_CTRL) == KEY_RESULT_PREFIX && test_type(&k, 'o') == KEY_RESULT_COMMAND &&
                  k.r.command == &CMD_NEXT_LINE, "keys: C-x o through a text event");
    TEST_CHECK(t, test_key(&k, KEY_X, 'x', KEYMOD_CTRL) == KEY_RESULT_PREFIX && test_type(&k, 'z') == KEY_RESULT_UNDEFINED &&
                  test_seq_is(&k.r.seq, "C-x z"), "keys: C-x z is undefined");
    // TAB bound to self-insert-command carries a tab.
    TEST_CHECK(t, test_key(&k, KEY_TAB, 0, 0) == KEY_RESULT_COMMAND && k.r.command == &CMD_SELF_INSERT && k.r.codepoint == '\t',
               "keys: TAB inserts a tab");
    // describe-key: the next complete sequence is described, not run.
    k.in.describe = 1;
    TEST_CHECK(t, test_key(&k, KEY_X, 'x', KEYMOD_CTRL) == KEY_RESULT_PREFIX && test_key(&k, KEY_S, 's', KEYMOD_CTRL) == KEY_RESULT_DESCRIBE &&
                  k.r.command == &CMD_SAVE_BUFFER && !k.in.describe, "keys: describe C-x C-s");
    k.in.describe = 1;
    TEST_CHECK(t, test_key(&k, KEY_Q, 'q', KEYMOD_CTRL) == KEY_RESULT_DESCRIBE && !k.r.command, "keys: describe an undefined C-q");
    k.in.describe = 1;
    TEST_CHECK(t, test_type(&k, 'a') == KEY_RESULT_DESCRIBE && k.r.command == &CMD_SELF_INSERT, "keys: describe a");
    TEST_CHECK(t, test_key(&k, KEY_F, 'f', KEYMOD_CTRL) == KEY_RESULT_COMMAND, "keys: describe ends after one sequence");

    // Two maps sharing a prefix: prefixes merge across maps, the first exact match wins.
    TEST_CHECK(t, test_bind(context, "C-x k", &CMD_END_OF_BUFFER) == 0 && test_bind(context, "C-f", &CMD_NEXT_LINE) == 0 &&
                  test_bind(context, "C-c x", &CMD_PREVIOUS_LINE) == 0, "keys: binding the context map");
    TestKeys k2 = { .stack = { context, global }, .count = 2 };
    TEST_CHECK(t, test_key(&k2, KEY_X, 'x', KEYMOD_CTRL) == KEY_RESULT_PREFIX && test_key(&k2, KEY_K, 'k', 0) == KEY_RESULT_IGNORED &&
                  test_text(&k2, 'k') == KEY_RESULT_COMMAND && k2.r.command == &CMD_END_OF_BUFFER, "keys: C-x k from the context map");
    TEST_CHECK(t, test_key(&k2, KEY_X, 'x', KEYMOD_CTRL) == KEY_RESULT_PREFIX && test_key(&k2, KEY_S, 's', KEYMOD_CTRL) == KEY_RESULT_COMMAND &&
                  k2.r.command == &CMD_SAVE_BUFFER, "keys: C-x C-s from the global map is not hidden by the context's C-x k");
    TEST_CHECK(t, test_key(&k2, KEY_X, 'x', KEYMOD_CTRL) == KEY_RESULT_PREFIX && test_type(&k2, 'o') == KEY_RESULT_COMMAND &&
                  k2.r.command == &CMD_NEXT_LINE, "keys: C-x o from the global map");
    TEST_CHECK(t, test_key(&k2, KEY_X, 'x', KEYMOD_CTRL) == KEY_RESULT_PREFIX && test_type(&k2, 'q') == KEY_RESULT_UNDEFINED,
               "keys: C-x q is undefined in both maps");
    TEST_CHECK(t, test_key(&k2, KEY_F, 'f', KEYMOD_CTRL) == KEY_RESULT_COMMAND && k2.r.command == &CMD_NEXT_LINE,
               "keys: C-f bound in both maps: the context map wins");
    TEST_CHECK(t, test_key(&k2, KEY_C, 'c', KEYMOD_CTRL) == KEY_RESULT_PREFIX && test_type(&k2, 'x') == KEY_RESULT_COMMAND &&
                  k2.r.command == &CMD_PREVIOUS_LINE, "keys: a prefix that exists only in the context map");

    // Binding conflicts: a binding removes the bindings it is a prefix of, or that are its prefix.
    TEST_CHECK(t, test_bind(global, "C-x", &CMD_END_OF_BUFFER) == 2 && global->count == 8, "keys: C-x replaces C-x C-s and C-x o");
    TEST_CHECK(t, test_bind(global, "C-x C-s", &CMD_SAVE_BUFFER) == 1 && global->count == 8, "keys: C-x C-s replaces C-x");
    TEST_CHECK(t, test_bind(global, "C-x C-s", &CMD_FORWARD_CHAR) == 0 && global->count == 8, "keys: rebinding replaces in place");
    TEST_CHECK(t, test_bind(global, "C-x C-s", NULL) == 0 && global->count == 7, "keys: none removes");
    TEST_CHECK(t, test_bind(global, "C-x C-s", NULL) == 0 && global->count == 7, "keys: removing an unbound sequence");
    // quoted-insert (C-q): the next key is inserted literally.
    Keymap *map = PUSH_STRUCT(&t->arena, Keymap);
    TestKeys kq = { .stack = { map }, .count = 1 };
    kq.in.quoted = 1;
    TEST_CHECK(t, test_key(&kq, KEY_TAB, 0, 0) == KEY_RESULT_QUOTED && kq.r.codepoint == '\t', "C-q TAB");
    kq.in.quoted = 1;
    TEST_CHECK(t, test_key(&kq, KEY_J, 'j', KEYMOD_CTRL) == KEY_RESULT_QUOTED && kq.r.codepoint == 10, "C-q C-j");
    kq.in.quoted = 1;
    TEST_CHECK(t, test_key(&kq, KEY_A, 'a', 0) == KEY_RESULT_IGNORED && test_text(&kq, 'a') == KEY_RESULT_QUOTED && kq.r.codepoint == 'a',
               "C-q a");
    kq.in.quoted = 1;
    TEST_CHECK(t, test_key(&kq, KEY_LEFT, 0, 0) == KEY_RESULT_QUOTED && kq.r.codepoint == 0 && !kq.in.quoted, "C-q <left>: nothing");
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
    // Settle: a notification is acted on 50 ms after the last one, so a file truncated and then
    // written is read once, full, never empty.
    if (!test_write(t, path, "")) return 0;
    config_notify(&src, now);
    TEST_CHECK(t, config_pending(&src) && config_poll(&src, c, &t->arena, 0, now + 10000) == CONFIG_POLL_UNCHANGED &&
                  config_wait_ms(&src, now + 10000) == CONFIG_SETTLE_MS - 10, "reload: settling: no read before 50 ms");
    if (!test_write(t, path, "[settings]\ntab_width = 11\n# written after the truncation\n")) return 0;
    config_notify(&src, now + 30000); // the second notification moves the read to now + 80 ms
    TEST_CHECK(t, config_poll(&src, c, &t->arena, 0, now + 60000) == CONFIG_POLL_UNCHANGED, "reload: settling restarts on a notification");
    TEST_CHECK(t, config_poll(&src, c, &t->arena, 0, now + 80000) == CONFIG_POLL_LOADED && c->settings.tab_width == 11 &&
                  !config_pending(&src) && config_wait_ms(&src, now + 80000) == CONFIG_WAIT_INFINITE,
               "reload: settled: the full file is read (tab_width %d)", c->settings.tab_width);
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
    view_switch_buffer(v, &list, b, NULL);
    TEST_CHECK(t, v->buffer == b && view_point(v, &v->cursors[0]) == 0 && view_top_line(v) == 0 && v->cursor_count == 1,
               "buffer list: a buffer never shown starts at the top");
    view_goto_line_column(v, 40, 0);
    view_ensure_visible(v);
    i64 point_b = view_point(v, &v->cursors[0]);
    // An edit in a, while it is not shown, moves its saved point like any marker.
    buffer_replace(a, 0, 0, STR8_LIT("xyz"));
    view_switch_buffer(v, &list, a, NULL);
    TEST_CHECK(t, view_point(v, &v->cursors[0]) == point + 3 && view_top_line(v) == top && v->cursor_count == 1,
               "buffer list: switching back restores point and scroll (point %D, expected %D)", view_point(v, &v->cursors[0]), point + 3);
    view_switch_buffer(v, &list, b, NULL);
    TEST_CHECK(t, view_point(v, &v->cursors[0]) == point_b, "buffer list: and again for b");

    // Two views: each remembers where it showed a buffer; a view that never showed it takes the position of
    // a view showing it now, else where it was last shown anywhere.
    View *v2 = view_create(&arena, a);
    v2->rows = 20;
    v2->cols = 40;
    view_goto_line_column(v2, 10, 0);
    view_ensure_visible(v2);
    i64 point2 = view_point(v2, &v2->cursors[0]);
    view_switch_buffer(v2, &list, b, v); // v shows b at line 40
    TEST_CHECK(t, view_point(v2, &v2->cursors[0]) == point_b, "view memory: a view new to b takes the point of the view showing it");
    view_goto_line_column(v2, 90, 0);
    view_switch_buffer(v, &list, a, NULL);  // v leaves b at line 40; the list's entry for b now says line 40
    view_switch_buffer(v2, &list, a, NULL); // v2 back to a: its own place (line 10), not v's (line 150)
    TEST_CHECK(t, view_point(v2, &v2->cursors[0]) == point2 && view_point(v, &v->cursors[0]) == point + 3,
               "view memory: each view returns to its own place in a (%D, %D)", view_point(v2, &v2->cursors[0]), point2);
    view_switch_buffer(v2, &list, b, NULL);
    TEST_CHECK(t, buffer_line_of(b, view_point(v2, &v2->cursors[0])) == 90, "view memory: and in b (line 90, not the list's 40)");
    view_switch_buffer(v, &list, b, NULL);
    TEST_CHECK(t, view_point(v, &v->cursors[0]) == point_b, "view memory: v's own place in b");
    view_forget_buffer(v, a);
    view_forget_buffer(v2, a);
    TEST_CHECK(t, v->memory_count == 0 && v2->memory_count == 0, "view memory: forgotten");
    view_destroy(v2);
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
    LOG("test: ok: buffer list (restore point and scroll, lookup by path, each view remembers its own place, forgetting), *Messages* capped at %d lines", (i32)ECHO_LOG_LINES);
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

// The minibuffer's part of the config: [keys minibuffer] binds in its own map, [keys] and
// [keys global] in the global one; the new settings and colors.
static b32 test_config_minibuffer(Test *t) {
    Config *c = PUSH_STRUCT(&t->arena, Config);
    config_init(c);
    config_parse(c, &t->arena, config_default_text(), STR8_LIT("<built-in>"));
    TEST_CHECK(t, c->settings.completion_lines == 8 && c->settings.auto_revert && c->theme.prompt == 0x0fdfaf &&
                  c->theme.completion_selection == 0x0000ff && c->theme.completion_match == 0xffffff,
               "config minibuffer: defaults");
    String8 user = STR8_LIT("[keys minibuffer]\nC-c a forward-char\n[keys global]\nC-c b backward-char\n[keys]\nC-c c next-line\n"
                            "[keys bogus]\nC-c d previous-line\n[keysminibuffer]\n[settings]\ncompletion_lines = 99\nauto_revert = false\n"
                            "[colors]\nprompt = #123456\n");
    config_parse(c, &t->arena, user, STR8_LIT("t.conf"));
    KeySeq a, b, d;
    const char *error;
    key_seq_parse(STR8_LIT("C-c a"), &a, &error);
    key_seq_parse(STR8_LIT("C-c b"), &b, &error);
    key_seq_parse(STR8_LIT("C-c d"), &d, &error);
    TEST_CHECK(t, keymap_get(&c->minibuffer, &a) == &CMD_FORWARD_CHAR && !keymap_get(&c->global, &a) &&
                  keymap_get(&c->global, &b) == &CMD_BACKWARD_CHAR && test_binding(c, "C-c c") == &CMD_NEXT_LINE &&
                  !keymap_get(&c->global, &d) && !keymap_get(&c->minibuffer, &d),
               "config minibuffer: sections bind in the right map");
    TEST_CHECK(t, c->errors == 2 && c->warnings == 1 && c->settings.completion_lines == 40 && !c->settings.auto_revert &&
                  c->theme.prompt == 0x123456, "config minibuffer: %d errors, %d warnings, completion_lines %d", c->errors,
               c->warnings, c->settings.completion_lines);
    TEST_CHECK(t, str8_equal(c->first_diag->text, STR8_LIT("t.conf:7: unknown section [keys bogus]")) &&
                  str8_equal(c->first_diag->next->text, STR8_LIT("t.conf:9: unknown section [keysminibuffer]")),
               "config minibuffer: diagnostics '%S'", c->first_diag->text);
    LOG("test: ok: config: [keys minibuffer], completion_lines, auto_revert, minibuffer colors");
    return 1;
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
                  test_binding(c, "ESC") == &CMD_KEYBOARD_QUIT && test_binding(c, "TAB") == &CMD_INDENT_FOR_TAB_COMMAND && test_binding(c, "<backtab>") == &CMD_UNINDENT &&
                  test_binding(c, "<next>") == &CMD_SCROLL_UP_COMMAND && test_binding(c, "C-x C-c") == &CMD_SAVE_BUFFERS_KILL_TERMINAL &&
                  test_binding(c, "C-x <right>") == &CMD_NEXT_BUFFER && test_binding(c, "C-x <left>") == &CMD_PREVIOUS_BUFFER &&
                  test_binding(c, "C-x C-+") == &CMD_TEXT_SCALE_INCREASE && test_binding(c, "C-x C-=") == &CMD_TEXT_SCALE_INCREASE &&
                  test_binding(c, "C-x C--") == &CMD_TEXT_SCALE_DECREASE && test_binding(c, "C-x C-0") == &CMD_TEXT_SCALE_RESET &&
                  test_binding(c, "C-h k") == &CMD_DESCRIBE_KEY && test_binding(c, "C-c ,") == &CMD_OPEN_CONFIG &&
                  test_binding(c, "C-c r") == &CMD_RELOAD_CONFIG, "config: default bindings");
    TEST_CHECK(t, s->split_width_threshold == 160 && s->startup_windows == 1 && th->window_divider == 0x126367 &&
                  th->mode_line_inactive_background == th->background && th->mode_line_inactive_text == th->text,
                  "config: window settings and colors");
    // The inactive mode line follows background and text until a file sets it.
    {
        u64 mark = arena_pos(&t->arena);
        Config *w = PUSH_STRUCT(&t->arena, Config);
        config_init(w);
        config_parse(w, &t->arena, config_default_text(), STR8_LIT("<built-in>"));
        config_parse(w, &t->arena, STR8_LIT("[colors]\nbackground = #101010\ntext = #202020\n[settings]\nstartup_windows = 3\n"), STR8_LIT("a.conf"));
        b32 follows = w->theme.mode_line_inactive_background == 0x101010 && w->theme.mode_line_inactive_text == 0x202020;
        b32 clamped = w->settings.startup_windows == 2 && w->warnings == 1;
        config_parse(w, &t->arena, STR8_LIT("[colors]\nmode_line_inactive_background = #303030\n"), STR8_LIT("b.conf"));
        config_parse(w, &t->arena, STR8_LIT("[colors]\nbackground = #404040\n"), STR8_LIT("c.conf"));
        b32 kept = w->theme.mode_line_inactive_background == 0x303030 && w->theme.mode_line_inactive_text == 0x202020;
        arena_pop_to(&t->arena, mark);
        TEST_CHECK(t, follows && clamped && kept, "config: derived inactive mode line colors (%d %d %d), startup_windows clamped",
                   follows, clamped, kept);
    }

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
                                        "C-x C-s   forward-char\n=  newline\nC-f none\n<f9> <f9> <f9> <f9> backward-char\n"),
                 STR8_LIT("user.conf"));
    TEST_CHECK(t, c->errors == 0 && c->warnings == 0, "config: valid user file: %d errors, %d warnings", c->errors, c->warnings);
    TEST_CHECK(t, s->font_size == 10.5f && s->tab_width == 8 && s->underscore_is_word && s->fsync_on_save &&
                  s->line_height == 100 && str8_equal(str8(s->font, s->font_len), STR8_LIT("Courier New")),
               "config: user settings over the defaults");
    TEST_CHECK(t, th->background == 0x102030 && th->text == 0xd3b58d, "config: user color over the defaults");
    TEST_CHECK(t, test_binding(c, "C-x C-s") == &CMD_FORWARD_CHAR && test_binding(c, "=") == &CMD_NEWLINE &&
                  !test_binding(c, "C-f") && test_binding(c, "C-b") == &CMD_BACKWARD_CHAR &&
                  test_binding(c, "<f9> <f9> <f9> <f9>") == &CMD_BACKWARD_CHAR, "config: user bindings, none, the defaults kept");

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
        "t.conf:1: line outside a section ([settings], [colors], [keys], [keys minibuffer] or [keys isearch])",
        "t.conf:3: font_size: 'big' is not a number",
        "t.conf:4: unknown setting 'nosuch'",
        "t.conf:5: expected name = value",
        "t.conf:7: background: '123456' is not a color (#rrggbb)",
        "t.conf:8: unknown color 'purple'",
        "t.conf:10: bad key sequence 'C-xy': more than one character (separate chords with spaces)",
        "t.conf:11: unknown command 'no-such-command'",
        "t.conf:12: expected a key sequence and a command",
        "t.conf:13: unknown section [bogus]",
        "t.conf:15: bad section header (expected [settings], [colors], [keys], [keys minibuffer] or [keys isearch])",
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
// --bench-syntax: source-like text per language (a block of realistic code repeated, '~' replaced by
// a running number), lexed for throughput.
static const char *test_bench_syntax_blocks[] = {
    /* Fundamental */ "",
    /* Jai */
    "// item ~: a procedure and its data\n"
    "Vector~ :: struct { x, y: float; }\n"
    "proc_~ :: (v: Vector~, count: int) -> float {\n"
    "    /* nested /* comment */ still comment */\n"
    "    s := \"string ~ with \\\"quotes\\\"\\n\";\n"
    "    for i: 0..count-1 { if i == 3 continue; }\n"
    "    return v.x * 0x1F + 1_000 + 0h3F80_0000;\n"
    "}\n"
    "TEXT_~ :: #string DONE\n"
    "here-string text /* not a comment */\n"
    "DONE\n",
    /* C */
    "/* Block ~: a comment that spans\n"
    " * two lines. */\n"
    "static int count_~(const char *s, int n) {\n"
    "    int total = 0; // running total\n"
    "    for (int i = 0; i < n; i++) {\n"
    "        if (s[i] == '\\n' || s[i] == '\"') total += 0x1F;\n"
    "    }\n"
    "    return total > 100 ? total : -1;\n"
    "}\n"
    "#define LIMIT_~ (1024 * 64) \\\n"
    "    /* continued */\n",
    /* C++ */
    "// item ~\n"
    "template <typename T> struct Box~ { T value; };\n"
    "auto text_~ = R\"x(raw \"string\" ~)x\";\n"
    "constexpr uint64_t mask_~ = 0xFFFF'0000ull;\n"
    "void use_~() {\n"
    "    auto b = Box~<int>{42};\n"
    "    if (b.value > 1'000) return; /* done */\n"
    "}\n",
    /* C# */
    "/// <summary>Item ~</summary>\n"
    "public class Item~ {\n"
    "    private string name = @\"C:\\path\\~ \"\"quoted\"\"\";\n"
    "    public int Count { get; set; } = 0x1F;\n"
    "    public string Describe() => $\"Item {Count} of {name.Length + 1}\";\n"
    "    /* block\n"
    "       comment */\n"
    "}\n",
    /* JavaScript */
    "// item ~\n"
    "const re~ = /ab+c\\d/gi;\n"
    "function f~(a, b) {\n"
    "    const s = `value ${a + b} and ${`inner ${a}`}`;\n"
    "    return a / b > 2 ? s : 'none';\n"
    "}\n"
    "/* block ~\n"
    "   comment */\n",
    /* TypeScript */
    "// item ~\n"
    "interface Shape~ { name: string; area(): number; }\n"
    "export function area~(s: Shape~, scale: number = 1.5): number {\n"
    "    const label = `shape ${s.name} at ${scale}`;\n"
    "    return s.area() * scale / 2;\n"
    "}\n"
    "/* block ~\n"
    "   comment */\n",
};

// `size` bytes (whole blocks) of source-like text in a language.
static String8 test_bench_syntax_text(Arena *arena, BufferLanguage language, i64 size) {
    u8 *out = PUSH_ARRAY(arena, u8, size + KB(4));
    String8 block = str8_cstr(test_bench_syntax_blocks[language]);
    i64 n = 0;
    for (i64 k = 0; n < size; k++) {
        u8 num[24];
        i64 len = fmt_buf(num, sizeof(num), "%D", k);
        for (i64 i = 0; i < block.len; i++) {
            if (block.data[i] == '~') {
                memcpy(out + n, num, (size_t)len);
                n += len;
            } else {
                out[n++] = block.data[i];
            }
        }
    }
    return str8(out, n);
}

// Lexing throughput per language on ~100 MB: the lexers alone (state only, as catch-up; with tokens,
// as drawing) and catch-up over a buffer (line text, clock checks, state stores).
void test_bench_syntax(void) {
    syntax_init();
    Arena arena = arena_create(GB(1));
    for (i32 lang = BUFFER_LANG_JAI; lang <= BUFFER_LANG_TYPESCRIPT; lang++) {
        u64 mark = arena_pos(&arena);
        String8 text = test_bench_syntax_text(&arena, (BufferLanguage)lang, MB(100));
        SyntaxToken *tokens = PUSH_ARRAY(&arena, SyntaxToken, KB(64));
        u64 best_state = ~(u64)0, best_tokens = ~(u64)0;
        i64 lines = 0;
        for (i32 pass = 0; pass < 3; pass++) {
            for (i32 with_tokens = 0; with_tokens < 2; with_tokens++) {
                SyntaxTokens out = { tokens, 0, KB(64) };
                u64 t0 = os_time_us();
                u32 state = 0;
                lines = 0;
                for (i64 i = 0; i < text.len;) {
                    i64 e = i;
                    while (e < text.len && text.data[e] != '\n') e++;
                    state = syntax_lex((BufferLanguage)lang, state, str8(text.data + i, e - i), with_tokens ? &out : NULL);
                    i = e + 1;
                    lines++;
                }
                u64 us = MAX(os_time_us() - t0, (u64)1);
                if (with_tokens) best_tokens = MIN(best_tokens, us);
                else best_state = MIN(best_state, us);
                if (state == 0xFFFFFFFF) LOG("bench-syntax: impossible state"); // keeps the loop from being optimized away
            }
        }
        Buffer *buf = buffer_create(STR8_LIT("bench"));
        buffer_undo_enable(buf, 0);
        buffer_replace(buf, 0, 0, text);
        buf->language = (BufferLanguage)lang;
        Arena scratch = arena_create(MB(64));
        u64 t0 = os_time_us();
        syntax_catch_up(buf, buffer_line_count(buf) - 1, (u64)I64_MAX, &scratch);
        u64 catch_us = MAX(os_time_us() - t0, (u64)1);
        u64 states = buffer_states_memory(buf);
        os_release(scratch.base);
        buffer_destroy(buf);
        f64 mb = (f64)text.len / (1024.0 * 1024.0);
        LOG("bench-syntax: %s: %D MB, %D lines: lexer %D MB/s (state only), %D MB/s (with tokens); catch-up %D MB/s (%U ms); "
            "states %U KB committed", buffer_language_name((BufferLanguage)lang), (i64)mb, lines, (i64)(mb * 1e6 / (f64)best_state),
            (i64)(mb * 1e6 / (f64)best_tokens), (i64)(mb * 1e6 / (f64)catch_us), catch_us / 1000, states / 1024);
        arena_pop_to(&arena, mark);
    }
    os_release(arena.base);
}

// build\tmp\bench_syntax_100mb.c: ~100 MB of C, generated once (the frames of --bench-syntax).
String8 test_bench_syntax_file(Arena *arena, String8 tmp_dir) {
    String8 path = str8_fmt(arena, "%S\\bench_syntax_100mb.c", tmp_dir);
    OsFileInfo info;
    if (os_file_info(path, &info) == OS_FILE_OK && info.size >= (i64)TEST_BENCH_SIZE) return path;
    Arena scratch = arena_create(GB(1));
    String8 text = test_bench_syntax_text(&scratch, BUFFER_LANG_C, (i64)TEST_BENCH_SIZE);
    if (!os_write_file(path, text)) LOG("bench-syntax: cannot write %S", path);
    os_release(scratch.base);
    return path;
}

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

    // Reverting after a one-line change outside: another buffer edits a line in the middle of the
    // file the first one now visits and saves it; the first one reads it back (one replace of the
    // changed middle).
    Buffer *other = buffer_create(STR8_LIT("bench-other"));
    if (other && buffer_load_file(other, out) == OS_FILE_OK) {
        i64 mid = buffer_line_start(other, buffer_line_count(other) / 2);
        buffer_replace(other, mid, mid + 4, STR8_LIT("EDIT"));
        buffer_save_opt(other, 0);
        u64 undo_before = buffer_undo_memory(buf);
        t0 = os_time_us();
        status = buffer_revert(buf, 0);
        u64 revert_us = os_time_us() - t0;
        LOG("bench-buffer: revert after a one-line change outside: %U ms for %D bytes (%s); undo log %U KB -> %U KB committed",
            revert_us / 1000, buffer_size(buf), buffer_status_text(status), undo_before / 1024, buffer_undo_memory(buf) / 1024);
    }
    if (other) buffer_destroy(other);
    os_file_delete(out);
    os_release(scratch.base);
    buffer_destroy(buf);
}

// ---------------------------------------------------------------------------
// The headless app: the real key path (keymap, driver, app commands, prompts) without a window
// or a font. Keys in --keys notation.

static App *test_app_create(Test *t) {
    AppArgs args = { .dpi_scale = 1.0f, .headless = 1 };
    App *app = app_create(&t->arena, &args);
    if (app) app_dev_feed_events(app, NULL, 0, &t->arena); // the first frame: layout
    return app;
}

static b32 test_app_destroy(Test *t, App *app, const char *what) {
    i32 leaks = app_shutdown(app);
    TEST_CHECK(t, leaks == 0, "%s: %d leak(s) at shutdown", what, leaks);
    return 1;
}

// show_paren_mode in a headless app: the opener at point, the closer before point (which wins), a
// bracket in a string, the setting off.
static b32 test_show_paren(Test *t) {
    String8 path = str8_fmt(&t->arena, "%S\\p8paren.c", t->tmp_dir);
    String8 text = STR8_LIT("int f(int a) {\n    return g()(a + \"(\");\n}\n");
    TEST_CHECK(t, os_write_file(path, text), "paren: cannot write the file");
    App *app = test_app_create(t);
    TEST_CHECK(t, app && app_dev_visit(app, path), "paren: app or visit failed");
    View *v = app_selected_view(app);
    app_dev_feed_events(app, NULL, 0, &t->arena); // a frame: the states
    i64 a, b;
    view_set_point(v, &v->cursors[0], 13); // on '{'
    TEST_CHECK(t, app_dev_paren(app, &t->arena, &a, &b) && a == 13 && b == text.len - 2, "paren: opener at point (%D %D)", a, b);
    view_set_point(v, &v->cursors[0], 29); // after "g()": on '(' too, the closer before point wins
    TEST_CHECK(t, app_dev_paren(app, &t->arena, &a, &b) && a == 28 && b == 27, "paren: the closer before point wins (%D %D)", a, b);
    view_set_point(v, &v->cursors[0], 35); // on the '(' in the string
    TEST_CHECK(t, !app_dev_paren(app, &t->arena, &a, &b), "paren: a bracket in a string");
    view_set_point(v, &v->cursors[0], 3); // no bracket
    TEST_CHECK(t, !app_dev_paren(app, &t->arena, &a, &b), "paren: none at point");
    view_set_point(v, &v->cursors[0], 13);
    app->config->settings.show_paren_mode = 0;
    TEST_CHECK(t, !app_dev_paren(app, &t->arena, &a, &b), "paren: show_paren_mode off");
    app->config->settings.show_paren_mode = 1;
    if (!test_app_destroy(t, app, "paren")) return 0;
    os_file_delete(path);
    LOG("test: ok: show-paren (an opener at point, the closer before point wins, in a string, none, off)");
    return 1;
}

static String8 test_app_text(Test *t, App *app) {
    Buffer *buf = app_selected_view(app)->buffer;
    return buffer_text(buf, &t->arena, 0, buffer_size(buf));
}

static b32 test_headless_app(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "headless app: app_create failed");
    app_dev_feed(app, "h i RET C-a x C-/", &t->arena);
    String8 text = test_app_text(t, app);
    TEST_CHECK(t, str8_equal(text, STR8_LIT("hi\n")) && str8_equal(app_selected_view(app)->buffer->name, STR8_LIT("*scratch*")),
               "headless app: typed into *scratch*: '%S'", text);
    if (!test_app_destroy(t, app, "headless app")) return 0;
    LOG("test: ok: headless app (keys through the keymap and the driver)");
    return 1;
}

// ---------------------------------------------------------------------------
// isearch through the real key path

// A headless app showing *scratch* with `text`, point at `point`.
static App *test_search_app(Test *t, String8 text, i64 point) {
    App *app = test_app_create(t);
    if (!app) return NULL;
    View *v = app_selected_view(app);
    buffer_replace(v->buffer, 0, buffer_size(v->buffer), text);
    view_set_point(v, &v->cursors[0], point);
    view_ensure_visible(v);
    return app;
}

static i64 test_app_point(App *app) {
    View *v = app_selected_view(app);
    return view_point(v, &v->cursors[0]);
}

static b32 test_app_echo_is(App *app, const char *expected) {
    return str8_equal(str8(app->echo.text, app->echo.len), str8_cstr(expected));
}

// The wheel over the first view: positive notches scroll towards the top.
static void test_app_wheel(Test *t, App *app, i32 notches) {
    Event e = { .kind = EVENT_MOUSE_WHEEL, .x = 100, .y = 100, .wheel = notches * 120 };
    app_dev_feed_events(app, &e, 1, &t->arena);
}

// Every event of `keys` in a single frame (typed ahead, or --keys).
static void test_app_feed_batch(Test *t, App *app, const char *keys) {
    Event events[256];
    i32 n = app_dev_key_events(app, str8_cstr(keys), events, ARRAY_COUNT(events));
    app_dev_feed_events(app, events, n, &t->arena);
}

// Frames without events until no search is pending; returns how many.
static i32 test_app_pump(Test *t, App *app) {
    i32 frames = 0;
    while (app_wants_frame(app) && frames < 100000) {
        app_dev_feed_events(app, NULL, 0, &t->arena);
        frames++;
    }
    return frames;
}

static b32 test_isearch_state(Test *t, App *app, const char *what, b32 active, i64 point, const char *prompt, const char *string, i64 fail) {
    Isearch *is = &app->isearch;
    TEST_CHECK(t, is->active == active, "isearch: %s: active %d, want %d", what, is->active, active);
    TEST_CHECK(t, test_app_point(app) == point, "isearch: %s: point %D, want %D", what, test_app_point(app), point);
    if (!active) return 1;
    String8 p = isearch_prompt(is, &t->arena), s = isearch_top(is)->string;
    TEST_CHECK(t, str8_equal(p, str8_cstr(prompt)), "isearch: %s: prompt '%S', want '%s'", what, p, prompt);
    TEST_CHECK(t, str8_equal(s, str8_cstr(string)), "isearch: %s: string '%S', want '%s'", what, s, string);
    i64 want_fail = fail < 0 ? s.len : fail;
    TEST_CHECK(t, isearch_fail_pos(is) == want_fail, "isearch: %s: failing from %D, want %D", what, isearch_fail_pos(is), want_fail);
    return 1;
}

static b32 test_isearch(Test *t) {
    // "foo bar\nfoo baz\nqux Foo\n": foo at 0 and 8, Foo at 20; the search starts at 5.
    String8 text = STR8_LIT("foo bar\nfoo baz\nqux Foo\n");
    App *app = test_search_app(t, text, 5);
    TEST_CHECK(t, app, "isearch: app_create failed");
    static const struct {
        const char *keys;
        b32 active;
        i64 point;
        const char *prompt, *string;
        i64 fail; // -1: nothing fails
    } steps[] = {
        { "C-s", 1, 5, "I-search: ", "", -1 },
        { "f", 1, 9, "I-search: ", "f", -1 },
        { "o o", 1, 11, "I-search: ", "foo", -1 },
        { "C-s", 1, 23, "I-search: ", "foo", -1 },               // Foo: no capital in the string, folded
        { "C-s", 1, 23, "Failing I-search: ", "foo", -1 },       // no more: point stays, nothing of the string fails
        { "C-s", 1, 3, "Wrapped I-search: ", "foo", -1 },        // wraps to the top; before the start: wrapped
        { "C-s", 1, 11, "Overwrapped I-search: ", "foo", -1 },   // past the start again
        { "C-r", 1, 8, "Overwrapped I-search backward: ", "foo", -1 }, // reversed: the same match, point to its start
        { "C-r", 1, 0, "Overwrapped I-search backward: ", "foo", -1 },
        { "DEL", 1, 8, "Overwrapped I-search backward: ", "foo", -1 }, // DEL unwinds every step
        { "DEL", 1, 11, "Overwrapped I-search: ", "foo", -1 },
        { "DEL", 1, 3, "Wrapped I-search: ", "foo", -1 },
        { "DEL", 1, 23, "Failing I-search: ", "foo", -1 },
        { "DEL", 1, 23, "I-search: ", "foo", -1 },
        { "DEL", 1, 11, "I-search: ", "foo", -1 },
        { "DEL", 1, 10, "I-search: ", "fo", -1 },
        { "DEL", 1, 9, "I-search: ", "f", -1 },
        { "DEL", 1, 5, "I-search: ", "", -1 },
        { "DEL", 1, 5, "I-search: ", "", -1 },                    // nothing left to undo
        { "f o o x", 1, 11, "Failing I-search: ", "foox", 3 },  // the x fails
        { "y", 1, 11, "Failing I-search: ", "fooxy", 3 },
        { "C-g", 1, 11, "I-search: ", "foo", -1 },                // C-g removes what fails
        { "C-g", 0, 5, NULL, NULL, -1 },                          // then quits to the start
    };
    for (i32 i = 0; i < ARRAY_COUNT(steps); i++) {
        app_dev_feed(app, steps[i].keys, &t->arena);
        char what[64];
        test_cstr(what, sizeof(what), "step %d ('%s')", i, steps[i].keys);
        if (!test_isearch_state(t, app, what, steps[i].active, steps[i].point, steps[i].prompt, steps[i].string, steps[i].fail)) return 0;
    }
    TEST_CHECK(t, test_app_echo_is(app, "Quit") && !app_selected_view(app)->cursors[0].mark_set, "isearch: C-g quits without a mark");
    MiniHistory *h = &app->mini.histories[MINI_HISTORY_SEARCH];
    TEST_CHECK(t, h->count == 0, "isearch: a quit search is not in the history");

    // RET and ESC: end at the match, the mark at the start, the string in the history.
    View *v = app_selected_view(app);
    Cursor *c = &v->cursors[0];
    app_dev_feed(app, "C-s f o o RET", &t->arena);
    TEST_CHECK(t, !app->isearch.active && test_app_point(app) == 11 && c->mark_set && !c->mark_active &&
                  buffer_marker_get(v->buffer, c->mark) == 5 && test_app_echo_is(app, "Mark saved where search started"),
               "isearch: RET ends at the match and sets the mark at the start");
    TEST_CHECK(t, h->count == 1 && str8_equal(h->items[0], STR8_LIT("foo")), "isearch: RET puts the string in the history");
    view_set_point(v, c, 0);
    app_dev_feed(app, "C-s b a z ESC", &t->arena);
    TEST_CHECK(t, !app->isearch.active && test_app_point(app) == 15 && buffer_marker_get(v->buffer, c->mark) == 0,
               "isearch: ESC ends at the match and sets the mark");
    // Typed ahead (all the keys in one frame, as --keys delivers them): RET still ends at the match.
    view_set_point(v, c, 0);
    test_app_feed_batch(t, app, "C-s b a z RET");
    TEST_CHECK(t, !app->isearch.active && test_app_point(app) == 15, "isearch: typed ahead, RET ends at the match (%D)", test_app_point(app));
    // C-s C-s: the last string; M-p / M-n walk the history around.
    view_set_point(v, c, 0);
    app_dev_feed(app, "C-s C-s", &t->arena);
    if (!test_isearch_state(t, app, "C-s C-s", 1, 15, "I-search: ", "baz", -1)) return 0;
    app_dev_feed(app, "M-p", &t->arena);
    if (!test_isearch_state(t, app, "M-p", 1, 15, "I-search: ", "baz", -1)) return 0;
    app_dev_feed(app, "M-p", &t->arena);
    if (!test_isearch_state(t, app, "M-p M-p", 1, 3, "I-search: ", "foo", -1)) return 0;
    app_dev_feed(app, "M-n", &t->arena);
    if (!test_isearch_state(t, app, "M-n", 1, 15, "I-search: ", "baz", -1)) return 0;
    app_dev_feed(app, "M-n", &t->arena);
    if (!test_isearch_state(t, app, "M-n around", 1, 3, "I-search: ", "foo", -1)) return 0;
    app_dev_feed(app, "DEL", &t->arena);
    if (!test_isearch_state(t, app, "DEL after M-n", 1, 15, "I-search: ", "baz", -1)) return 0;
    app_dev_feed(app, "C-g", &t->arena);
    // A key bound to another command ends the search at the match, then runs.
    view_set_point(v, c, 0);
    app_dev_feed(app, "C-s b a C-f", &t->arena);
    TEST_CHECK(t, !app->isearch.active && test_app_point(app) == 7 && buffer_marker_get(v->buffer, c->mark) == 0 &&
                  test_app_echo_is(app, "Mark saved where search started"), "isearch: C-f ends the search at the match, then runs");
    // A global prefix ends it at once, and shows.
    view_set_point(v, c, 0);
    app_dev_feed(app, "C-s f C-x", &t->arena);
    TEST_CHECK(t, !app->isearch.active && test_app_point(app) == 1 && test_app_echo_is(app, "C-x-"), "isearch: C-x ends the search");
    app_dev_feed(app, "C-g", &t->arena);
    // C-w: the rest of the word after the match, lowercased while folding.
    view_set_point(v, c, 16);
    app_dev_feed(app, "C-s x C-w", &t->arena);
    if (!test_isearch_state(t, app, "C-w", 1, 23, "I-search: ", "x foo", -1)) return 0;
    app_dev_feed(app, "C-g C-g", &t->arena);
    // C-y: the latest kill, lowercased while folding.
    memcpy(kill_push(&app->kills, 3), "BAZ", 3);
    kill_to_clipboard(&app->kills);
    view_set_point(v, c, 0);
    app_dev_feed(app, "C-s C-y", &t->arena);
    if (!test_isearch_state(t, app, "C-y", 1, 15, "I-search: ", "baz", -1)) return 0;
    app_dev_feed(app, "C-g C-g", &t->arena);
    // Smart case: a capital letter makes the search exact; M-c switches either way.
    view_set_point(v, c, 0);
    app_dev_feed(app, "C-s F o o", &t->arena);
    if (!test_isearch_state(t, app, "smart case: exact", 1, 23, "I-search: ", "Foo", -1)) return 0;
    app_dev_feed(app, "C-g C-g", &t->arena);
    view_set_point(v, c, 17);
    app_dev_feed(app, "C-s f o o", &t->arena);
    if (!test_isearch_state(t, app, "smart case: folded", 1, 23, "I-search: ", "foo", -1)) return 0;
    app_dev_feed(app, "M-c", &t->arena);
    if (!test_isearch_state(t, app, "M-c: exact", 1, 23, "Failing I-search: ", "foo", -1)) return 0;
    TEST_CHECK(t, test_app_echo_is(app, "case sensitive"), "isearch: M-c says so");
    app_dev_feed(app, "M-c", &t->arena);
    if (!test_isearch_state(t, app, "M-c again: folded", 1, 23, "I-search: ", "foo", -1)) return 0;
    app_dev_feed(app, "C-g C-g", &t->arena);
    // Not in the minibuffer; the isearch commands outside a search.
    app_dev_feed(app, "M-x C-s", &t->arena);
    TEST_CHECK(t, !app->isearch.active && app->mini.active && test_app_echo_is(app, "isearch is not available in the minibuffer"),
               "isearch: refused in the minibuffer");
    app_dev_feed(app, "C-g", &t->arena);
    app_dev_feed(app, "M-x i s e a r c h - e x i t RET", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "Not in an isearch"), "isearch: isearch-exit outside a search");
    if (!test_app_destroy(t, app, "isearch")) return 0;

    // Sliced: a fixed number of positions per frame instead of the clock.
    i64 big = 2000000;
    u8 *data = PUSH_ARRAY(&t->arena, u8, big + 8);
    memset(data, 'x', (size_t)big);
    memcpy(data + big, "needle\n", 7);
    app = test_search_app(t, str8(data, big + 7), 0);
    app->dev_work_budget = KB(64);
    app_dev_feed(app, "C-s n e e d l e", &t->arena);
    TEST_CHECK(t, isearch_pending(&app->isearch) && app_wants_frame(app) && search_progress(&app->isearch.search) > 0 &&
                  search_progress(&app->isearch.search) < 100 && test_app_point(app) == 0, "isearch: sliced: still searching after the keys");
    i32 frames = test_app_pump(t, app);
    if (!test_isearch_state(t, app, "sliced", 1, big + 6, "I-search: ", "needle", -1)) return 0;
    // The 12 key events after the "n" already searched one slice each: about 30 frames of 64 KB in all.
    TEST_CHECK(t, frames + 12 >= (i32)(big / KB(64)) && frames + 12 <= (i32)(big / KB(64)) + 2 && !app_wants_frame(app),
               "isearch: sliced: %d frames after the keys for %D bytes at 64 KB a frame", frames, big);
    app_dev_feed(app, "C-g C-g", &t->arena);
    view_set_point(app_selected_view(app), &app_selected_view(app)->cursors[0], 0);
    app_dev_feed(app, "C-s n e", &t->arena); // "n" being searched, "ne" waiting
    app_dev_feed(app, "DEL", &t->arena);       // drops "ne"; "n" still searched
    TEST_CHECK(t, isearch_pending(&app->isearch) && app->isearch.count == 2, "isearch: sliced: DEL drops a waiting step");
    app_dev_feed(app, "DEL", &t->arena);       // drops the step being searched
    if (!test_isearch_state(t, app, "sliced: DEL during the search", 1, 0, "I-search: ", "", -1)) return 0;
    TEST_CHECK(t, !app_wants_frame(app), "isearch: sliced: nothing pending after DEL");
    app_dev_feed(app, "C-g", &t->arena);
    app->dev_work_budget = 0;
    if (!test_app_destroy(t, app, "isearch sliced")) return 0;

    // The wheel: it drags point, but the next key acts on the session's own match.
    u8 *lines = PUSH_ARRAY(&t->arena, u8, 200 * 13);
    for (i32 i = 0; i < 200; i++) fmt_buf(lines + i * 13, 14, "foo line %03d\n", i); // foo at 13 * i
    app = test_search_app(t, str8(lines, 200 * 13), 0);
    v = app_selected_view(app);
    c = &v->cursors[0];
    app_dev_feed(app, "C-s f o o", &t->arena);
    test_app_wheel(t, app, -10);
    TEST_CHECK(t, test_app_point(app) > 13 * 20, "isearch: the wheel dragged point (%D)", test_app_point(app));
    app_dev_feed(app, "C-s", &t->arena);
    TEST_CHECK(t, test_app_point(app) == 16 && view_top_line(v) <= 1, "isearch: wheel then C-s: the next match after the session's (%D, top %D)",
               test_app_point(app), view_top_line(v));
    test_app_wheel(t, app, -10);
    app_dev_feed(app, "DEL", &t->arena);
    TEST_CHECK(t, test_app_point(app) == 3, "isearch: wheel then DEL: back to the step before (%D)", test_app_point(app));
    test_app_wheel(t, app, -10);
    app_dev_feed(app, "C-w", &t->arena);
    if (!test_isearch_state(t, app, "wheel then C-w", 1, 8, "I-search: ", "foo line", -1)) return 0;
    test_app_wheel(t, app, -10);
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, test_app_point(app) == 8 && buffer_marker_get(v->buffer, c->mark) == 0 && view_top_line(v) == 0,
               "isearch: wheel then RET: at the match, not where the wheel left point");
    app_dev_feed(app, "C-s f o o C-s", &t->arena);
    test_app_wheel(t, app, -10);
    app_dev_feed(app, "C-g", &t->arena);
    TEST_CHECK(t, !app->isearch.active && test_app_point(app) == 8, "isearch: wheel then C-g: back to the start (%D)", test_app_point(app));
    app_dev_feed(app, "C-s f o o", &t->arena);
    test_app_wheel(t, app, -10);
    app_dev_feed(app, "C-f", &t->arena);
    TEST_CHECK(t, !app->isearch.active && test_app_point(app) == 17, "isearch: wheel then C-f: ends at the match, then runs (%D)",
               test_app_point(app));
    if (!test_app_destroy(t, app, "isearch wheel")) return 0;
    LOG("test: ok: isearch: %d steps of the state table (extend, repeat, fail, wrap, overwrap, reverse, DEL unwinding, C-g twice), "
        "RET, ESC, C-s C-s, M-p / M-n, another command, a global prefix, C-w, C-y, smart case, M-c, the minibuffer, sliced "
        "(%d frames), the wheel", (i32)ARRAY_COUNT(steps), frames);
    return 1;
}

// ---------------------------------------------------------------------------
// query-replace and replace-string

static b32 test_replace_text(Test *t, App *app, const char *what, const char *want) {
    String8 got = test_app_text(t, app);
    TEST_CHECK(t, str8_equal(got, str8_cstr(want)), "replace: %s: '%S', want '%s'", what, got, want);
    return 1;
}

static b32 test_replace(Test *t) {
    // Case conversion (Emacs' replace-match).
    static const struct { const char *to, *match, *want; } cases[] = {
        { "bar", "foo", "bar" }, { "bar", "Foo", "Bar" }, { "bar", "FOO", "BAR" }, { "bar baz", "Foo Bar", "Bar Baz" },
        { "bar", "fOO", "bar" }, { "bar", "Foo-bar", "bar" }, { "bar", "X", "BAR" }, { "xy", "A B", "XY" },
        { "bar", "F1", "BAR" }, { "bar", "1foo", "bar" }, { "\xc4\xb1\xc5\x9f\xc4\xb1k", "Foo", "I\xc5\x9f\xc4\xb1k" },
        { "\xc3\xa7" "ay", "FOO", "\xc3\x87" "AY" }, { "a-b", "Foo", "A-B" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        String8 got = replace_case(&t->arena, str8_cstr(cases[i].to), str8_cstr(cases[i].match));
        TEST_CHECK(t, str8_equal(got, str8_cstr(cases[i].want)), "replace: case conversion %d: '%s' for '%s' gave '%S', want '%s'", i,
                   cases[i].to, cases[i].match, got, cases[i].want);
    }

    // Every answer.
    App *app = test_search_app(t, STR8_LIT("a foo b foo c foo d foo e foo f foo\n"), 0);
    TEST_CHECK(t, app, "replace: app_create failed");
    Replace *rp = &app->replace;
    View *v = app_selected_view(app);
    app_dev_feed(app, "M-%", &t->arena);
    TEST_CHECK(t, str8_equal(app_dev_prompt(app), STR8_LIT("Query replace: ")), "replace: the first prompt '%S'", app_dev_prompt(app));
    app_dev_feed(app, "f o o RET", &t->arena);
    TEST_CHECK(t, str8_equal(app_dev_prompt(app), STR8_LIT("Query replace foo with: ")), "replace: the second prompt '%S'", app_dev_prompt(app));
    app_dev_feed(app, "b a r RET", &t->arena);
    String8 q = replace_prompt(rp, &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_ASKING && test_app_point(app) == 5 && str8_equal(q, STR8_LIT("Query replacing foo with bar: (y, n, !, q, .)")),
               "replace: asking at the first match: '%S', point %D", q, test_app_point(app));
    app_dev_feed(app, "y", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_ASKING && test_app_point(app) == 11, "replace: y replaces and asks at the next (%D)", test_app_point(app));
    app_dev_feed(app, "SPC n DEL", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_ASKING && test_app_point(app) == 29, "replace: SPC replaces, n and DEL skip (%D)", test_app_point(app));
    app_dev_feed(app, ".", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_OFF && test_app_point(app) == 29 && test_app_echo_is(app, "Replaced 3 occurrences"),
               "replace: . replaces this one and stops");
    if (!test_replace_text(t, app, "y SPC n DEL .", "a bar b bar c foo d foo e bar f foo\n")) return 0;
    view_set_point(v, &v->cursors[0], 0);
    app_dev_feed(app, "M-%", &t->arena);
    TEST_CHECK(t, str8_equal(app_dev_prompt(app), STR8_LIT("Query replace (default foo -> bar): ")), "replace: the default '%S'", app_dev_prompt(app));
    app_dev_feed(app, "RET q", &t->arena); // an empty answer repeats the last pair
    TEST_CHECK(t, rp->state == REPLACE_OFF && test_app_point(app) == 17 && test_app_echo_is(app, "Replaced 0 occurrences"),
               "replace: the last pair again, q stops at the match (%D)", test_app_point(app));
    app_dev_feed(app, "M-% RET RET", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_OFF && test_app_point(app) == 23, "replace: RET stops (%D)", test_app_point(app));
    view_set_point(v, &v->cursors[0], 0);
    app_dev_feed(app, "M-% RET !", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_OFF && test_app_point(app) == 35 && test_app_echo_is(app, "Replaced 3 occurrences"), "replace: ! replaces the rest");
    if (!test_replace_text(t, app, "!", "a bar b bar c bar d bar e bar f bar\n")) return 0;
    buffer_replace(v->buffer, 0, buffer_size(v->buffer), STR8_LIT("x1 x2 x3\n"));
    view_set_point(v, &v->cursors[0], 0);
    app_dev_feed(app, "M-% x RET y RET y C-g", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_OFF && test_app_point(app) == 4 && test_app_echo_is(app, "Quit"), "replace: C-g stops, keeping what was done");
    if (!test_replace_text(t, app, "C-g", "y1 x2 x3\n")) return 0;
    view_set_point(v, &v->cursors[0], 0);
    app_dev_feed(app, "M-% RET C-f", &t->arena); // another key ends the session, then runs
    TEST_CHECK(t, rp->state == REPLACE_OFF && test_app_point(app) == 5 && test_app_echo_is(app, "Replaced 0 occurrences"),
               "replace: C-f ends the session at the match, then runs (%D)", test_app_point(app));
    // Typed ahead: the answers in the same frame as the prompt's RET are not lost.
    buffer_replace(v->buffer, 0, buffer_size(v->buffer), STR8_LIT("foo foo foo\n"));
    view_set_point(v, &v->cursors[0], 0);
    test_app_feed_batch(t, app, "M-% f o o RET b a r RET y n y");
    TEST_CHECK(t, rp->state == REPLACE_OFF, "replace: typed ahead, the session ran to its end");
    if (!test_replace_text(t, app, "typed ahead", "bar foo bar\n")) return 0;
    // A change from outside while asking (a revert): the answer does not replace a stale range; the match
    // is searched again and asked about.
    buffer_replace(v->buffer, 0, buffer_size(v->buffer), STR8_LIT("ab foo\n"));
    view_set_point(v, &v->cursors[0], 0);
    app_dev_feed(app, "M-% f o o RET b a r RET", &t->arena);
    buffer_replace(v->buffer, 0, 0, STR8_LIT("xyz")); // as a revert would
    app_dev_feed(app, "y", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_ASKING && rp->match_start == 6 && test_app_point(app) == 9, "replace: a stale match is searched again");
    app_dev_feed(app, "y", &t->arena);
    if (!test_replace_text(t, app, "after an outside change", "xyzab bar\n")) return 0;
    // One undo takes back the whole session (point where it started); undo-redo applies it again.
    buffer_replace(v->buffer, 0, buffer_size(v->buffer), STR8_LIT("foo foo foo foo\n"));
    view_set_point(v, &v->cursors[0], 2);
    app_dev_feed(app, "C-f", &t->arena); // a command: the edit above is its own group
    app_dev_feed(app, "M-% f o o RET b a r RET y n y", &t->arena); // the last y finds no more: the session ends
    if (!test_replace_text(t, app, "before undo", "foo bar foo bar\n")) return 0;
    app_dev_feed(app, "C-/", &t->arena);
    if (!test_replace_text(t, app, "one undo", "foo foo foo foo\n")) return 0;
    TEST_CHECK(t, test_app_point(app) == 3, "replace: undo puts point where the session started (%D)", test_app_point(app));
    app_dev_feed(app, "C-?", &t->arena);
    if (!test_replace_text(t, app, "undo-redo", "foo bar foo bar\n")) return 0;
    // The region only; case conversion through the app; a capital in the search string is exact.
    buffer_replace(v->buffer, 0, buffer_size(v->buffer), STR8_LIT("foo foo foo foo\n"));
    view_set_mark(v, &v->cursors[0], 4, 1);
    view_set_point(v, &v->cursors[0], 11);
    app_dev_feed(app, "M-%", &t->arena);
    TEST_CHECK(t, str8_equal(app_dev_prompt(app), STR8_LIT("Query replace in region (default foo -> bar): ")), "replace: the region prompt");
    app_dev_feed(app, "x RET RET", &t->arena); // "x": nothing in the region
    TEST_CHECK(t, test_app_echo_is(app, "Replaced 0 occurrences"), "replace: x in the region");
    view_set_mark(v, &v->cursors[0], 4, 1);
    view_set_point(v, &v->cursors[0], 11);
    app_dev_feed(app, "M-% f o o RET x RET !", &t->arena);
    if (!test_replace_text(t, app, "region", "foo x x foo\n")) return 0;
    TEST_CHECK(t, !v->cursors[0].mark_active, "replace: the region is deactivated");
    buffer_replace(v->buffer, 0, buffer_size(v->buffer), STR8_LIT("foo Foo FOO\n"));
    view_set_point(v, &v->cursors[0], 0);
    app_dev_feed(app, "M-% f o o RET b a r RET !", &t->arena);
    if (!test_replace_text(t, app, "case conversion", "bar Bar BAR\n")) return 0;
    view_set_point(v, &v->cursors[0], 0);
    app_dev_feed(app, "M-% b a r RET B a z RET !", &t->arena); // a capital in the replacement: no conversion
    if (!test_replace_text(t, app, "replacement with a capital", "Baz Baz Baz\n")) return 0;
    buffer_replace(v->buffer, 0, buffer_size(v->buffer), STR8_LIT("foo Foo FOO\n"));
    view_set_point(v, &v->cursors[0], 0);
    app_dev_feed(app, "M-% F o o RET b a r RET !", &t->arena); // a capital in the search string: exact
    if (!test_replace_text(t, app, "exact search", "foo bar FOO\n")) return 0;
    // replace-string; no matches; a replacement containing the search string; a read-only buffer.
    buffer_replace(v->buffer, 0, buffer_size(v->buffer), STR8_LIT("a a a\n"));
    view_set_point(v, &v->cursors[0], 0);
    app_dev_feed(app, "M-x r e p l a c e - s t r i n g RET a RET a a RET", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_OFF && test_app_echo_is(app, "Replaced 3 occurrences"), "replace: replace-string, a -> aa");
    if (!test_replace_text(t, app, "replace-string a -> aa", "aa aa aa\n")) return 0;
    app_dev_feed(app, "M-% z z z RET y RET", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_OFF && test_app_echo_is(app, "Replaced 0 occurrences"), "replace: no matches");
    v->buffer->read_only = 1;
    app_dev_feed(app, "M-%", &t->arena);
    TEST_CHECK(t, !app->mini.active && test_app_echo_is(app, "Buffer is read-only: *scratch*"), "replace: a read-only buffer");
    v->buffer->read_only = 0;
    if (!test_app_destroy(t, app, "replace")) return 0;
    app = test_search_app(t, STR8_LIT("foo\n"), 0);
    app_dev_feed(app, "M-% RET", &t->arena);
    TEST_CHECK(t, !app->mini.active && app->replace.state == REPLACE_OFF && test_app_echo_is(app, "Empty search string"),
               "replace: an empty search string without a last pair");
    if (!test_app_destroy(t, app, "replace empty")) return 0;

    // The wheel while asking: the answer acts on the session's own match.
    u8 *lines = PUSH_ARRAY(&t->arena, u8, 200 * 13);
    for (i32 i = 0; i < 200; i++) fmt_buf(lines + i * 13, 14, "foo line %03d\n", i);
    app = test_search_app(t, str8(lines, 200 * 13), 0);
    rp = &app->replace;
    v = app_selected_view(app);
    app_dev_feed(app, "M-% f o o RET b a r RET", &t->arena);
    test_app_wheel(t, app, -10);
    TEST_CHECK(t, test_app_point(app) > 13 * 20, "replace: the wheel dragged point (%D)", test_app_point(app));
    app_dev_feed(app, "y", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_ASKING && test_app_point(app) == 16 && view_top_line(v) <= 1,
               "replace: wheel then y: the next question at line 1 (%D)", test_app_point(app));
    test_app_wheel(t, app, -10);
    app_dev_feed(app, "n", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_ASKING && test_app_point(app) == 29, "replace: wheel then n: line 1 skipped (%D)", test_app_point(app));
    test_app_wheel(t, app, -10);
    app_dev_feed(app, "q", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_OFF && test_app_point(app) == 29, "replace: wheel then q: point at the match (%D)", test_app_point(app));
    String8 text = test_app_text(t, app);
    TEST_CHECK(t, mem_equal(text.data, "bar line 000\nfoo line 001\nfoo line 002\n", 39), "replace: the wheel: the right lines replaced");
    view_set_point(v, &v->cursors[0], 0);
    app_dev_feed(app, "M-% RET", &t->arena);
    test_app_wheel(t, app, -10);
    app_dev_feed(app, ".", &t->arena);
    text = test_app_text(t, app);
    TEST_CHECK(t, rp->state == REPLACE_OFF && test_app_point(app) == 16 && mem_equal(text.data, "bar line 000\nbar line 001\nfoo line 002\n", 39),
               "replace: wheel then .: replaced, point after it (%D)", test_app_point(app));
    if (!test_app_destroy(t, app, "replace wheel")) return 0;

    // Replace-all across frames: a fixed number of positions per frame; keys wait; C-g stops it midway.
    i64 n = 20000;
    u8 *foos = PUSH_ARRAY(&t->arena, u8, n * 4);
    for (i64 i = 0; i < n; i++) memcpy(foos + i * 4, "foo\n", 4);
    String8 original = str8(foos, n * 4);
    app = test_search_app(t, original, 0);
    rp = &app->replace;
    v = app_selected_view(app);
    app->dev_work_budget = 4096;
    app_dev_feed(app, "M-% f o o RET b a r RET !", &t->arena);
    q = replace_prompt(rp, &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_ALL && app_wants_frame(app) && str8_starts_with(q, STR8_LIT("Replacing... ")) && rp->count > 0 &&
                  rp->count < n, "replace-all: running after the first frame ('%S', %D)", q, rp->count);
    i64 point = test_app_point(app);
    app_dev_feed(app, "x C-f RET y M-x", &t->arena); // ignored meanwhile
    text = test_app_text(t, app);
    b32 clean = text.len == n * 4 + 0 && rp->state == REPLACE_ALL && !app->mini.active;
    for (i64 i = 0; clean && i < text.len; i++) clean = text.data[i] != 'x' && text.data[i] != 'y';
    TEST_CHECK(t, clean && test_app_point(app) == point && buffer_line_count(v->buffer) == n + 1, "replace-all: keys meanwhile change nothing");
    for (i32 k = 0; k < 40; k++) app_dev_feed_events(app, NULL, 0, &t->arena);
    i64 partial = rp->count;
    TEST_CHECK(t, rp->state == REPLACE_ALL && partial > 0 && partial < n, "replace-all: midway (%D of %D)", partial, n);
    app_dev_feed(app, "C-g", &t->arena);
    text = test_app_text(t, app);
    i64 bars = 0, rest_ok = 1;
    for (i64 i = 0; i < n; i++) {
        if (mem_equal(text.data + i * 4, "bar\n", 4)) bars += i == bars;
        else rest_ok &= mem_equal(text.data + i * 4, "foo\n", 4) && i >= bars;
    }
    char want[64];
    test_cstr(want, sizeof(want), "Replaced %D occurrences (stopped)", partial);
    TEST_CHECK(t, rp->state == REPLACE_OFF && bars == partial && rest_ok && test_app_echo_is(app, want) && !app_wants_frame(app),
               "replace-all: C-g stops after %D, the rest untouched", partial);
    String8 stopped = str8_copy(&t->arena, text);
    app_dev_feed(app, "C-/", &t->arena);
    TEST_CHECK(t, str8_equal(test_app_text(t, app), original) && test_app_point(app) == 0, "replace-all: one undo restores the original text");
    app_dev_feed(app, "C-?", &t->arena);
    TEST_CHECK(t, str8_equal(test_app_text(t, app), stopped), "replace-all: undo-redo applies exactly the partial replacement again");
    view_set_point(v, &v->cursors[0], 0);
    app_dev_feed(app, "M-% RET", &t->arena);
    test_app_pump(t, app); // the first match is behind the replaced part: searched in slices
    TEST_CHECK(t, rp->state == REPLACE_ASKING && rp->match_start == partial * 4, "replace-all: asking after the replaced part");
    app_dev_feed(app, "!", &t->arena);
    i32 frames = test_app_pump(t, app);
    test_cstr(want, sizeof(want), "Replaced %D occurrences", n - partial);
    TEST_CHECK(t, rp->state == REPLACE_OFF && test_app_echo_is(app, want) && frames > 10, "replace-all: to the end (%d frames)", frames);
    view_set_point(v, &v->cursors[0], 0);
    app_dev_feed(app, "M-x r e p l a c e - s t r i n g RET b a r RET b a z RET", &t->arena);
    TEST_CHECK(t, rp->state == REPLACE_ALL && str8_starts_with(replace_prompt(rp, &t->arena), STR8_LIT("Replacing... ")),
               "replace-all: replace-string runs across frames too");
    test_app_pump(t, app);
    text = test_app_text(t, app);
    b32 all = text.len == n * 4;
    for (i64 i = 0; all && i < n; i++) all = mem_equal(text.data + i * 4, "baz\n", 4);
    test_cstr(want, sizeof(want), "Replaced %D occurrences", n);
    TEST_CHECK(t, all && test_app_echo_is(app, want), "replace-all: replace-string to the end");
    app->dev_work_budget = 0;
    if (!test_app_destroy(t, app, "replace-all")) return 0;
    LOG("test: ok: replace: %d case conversions, every answer, the last pair, C-g, another key, one undo and undo-redo, the region, "
        "case conversion, exact search, replace-string, a -> aa, no matches, read-only, an empty search string, the wheel, "
        "replace-all stopped at %D of %D and undone in one step", (i32)ARRAY_COUNT(cases), partial, n);
    return 1;
}

// ---------------------------------------------------------------------------
// The minibuffer: prompts opened directly, keys through the headless app.

typedef struct TestPromptLog {
    i32 calls;
    MiniResult r;
    u8 text[256];
    i64 len;
    View *view;   // the View the continuation ran on
    b32 chain;    // test_prompt_chain: open a second prompt from the continuation
} TestPromptLog;

static TestPromptLog test_prompt_log;

static void test_prompt_done(CommandContext *ctx, MiniResult *r) {
    TestPromptLog *log = &test_prompt_log;
    log->calls++;
    log->r = *r;
    log->len = MIN(r->text.len, (i64)sizeof(log->text));
    memcpy(log->text, r->text.data, (size_t)log->len);
    log->view = ctx->view;
}

static void test_prompt_chain(CommandContext *ctx, MiniResult *r) {
    test_prompt_done(ctx, r);
    MiniRequest next = { .kind = MINI_TEXT, .prompt = STR8_LIT("Second: "), .done = test_prompt_done };
    minibuffer_read(ctx, &next);
}

static b32 test_prompt_open(App *app, MiniRequest *req) {
    app->ctx.view = app_selected_view(app);
    if (!req->done) req->done = test_prompt_done;
    return minibuffer_read(&app->ctx, req);
}

static b32 test_prompt_text(App *app, const char *prompt, const char *initial, MiniKind kind) {
    MiniRequest req = { .kind = kind, .prompt = str8_cstr(prompt), .initial = str8_cstr(initial), .history = MINI_HISTORY_TEXT };
    return test_prompt_open(app, &req);
}

static b32 test_input_is(Test *t, App *app, const char *expected) {
    return str8_equal(minibuffer_input(&app->mini, &t->arena), str8_cstr(expected));
}

static b32 test_echo_has(App *app, const char *expected) {
    return str8_equal(str8(app->echo.text, app->echo.len), str8_cstr(expected));
}

static b32 test_logged(i32 calls, const char *text) {
    TestPromptLog *log = &test_prompt_log;
    return log->calls == calls && str8_equal(str8(log->text, log->len), str8_cstr(text));
}

static b32 test_minibuffer(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "minibuffer: app_create failed");
    Minibuffer *mb = &app->mini;
    View *v = app_selected_view(app);
    TestPromptLog *log = &test_prompt_log;
    *log = (TestPromptLog){ 0 };

    // RET accepts; the continuation runs on the calling view; typing never reaches its buffer.
    TEST_CHECK(t, test_prompt_text(app, "Text: ", "ab", MINI_TEXT) && mb->active && test_input_is(t, app, "ab"), "minibuffer: open");
    app_dev_feed(app, "c RET", &t->arena);
    TEST_CHECK(t, !mb->active && test_logged(1, "abc") && log->view == v && buffer_size(v->buffer) == 0,
               "minibuffer: RET accepts 'abc' on the calling view (calls %d)", log->calls);
    // C-g and ESC abort: no continuation, "Quit".
    test_prompt_text(app, "Text: ", "", MINI_TEXT);
    app_dev_feed(app, "x C-g", &t->arena);
    TEST_CHECK(t, !mb->active && log->calls == 1 && test_echo_has(app, "Quit"), "minibuffer: C-g aborts");
    test_prompt_text(app, "Text: ", "", MINI_TEXT);
    app_dev_feed(app, "x ESC", &t->arena);
    TEST_CHECK(t, !mb->active && log->calls == 1 && test_echo_has(app, "Quit"), "minibuffer: ESC aborts");
    // A pending prefix: C-x C-g cancels only the prefix; the next C-g aborts.
    test_prompt_text(app, "Text: ", "", MINI_TEXT);
    app_dev_feed(app, "C-x C-g", &t->arena);
    TEST_CHECK(t, mb->active, "minibuffer: C-x C-g cancels only the prefix");
    app_dev_feed(app, "C-g", &t->arena);
    TEST_CHECK(t, !mb->active && log->calls == 1, "minibuffer: then C-g aborts");

    // Editing commands, the kill ring, undo and the region work in it; kills reach the normal buffer.
    test_prompt_text(app, "Text: ", "hello world", MINI_TEXT);
    app_dev_feed(app, "C-a M-d C-y C-y", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "hellohello world"), "minibuffer: kill-word and yank");
    app_dev_feed(app, "C-/", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "hello world"), "minibuffer: undo");
    app_dev_feed(app, "C-e C-SPC M-b C-w RET", &t->arena);
    TEST_CHECK(t, test_logged(2, "hello "), "minibuffer: a region killed");
    app_dev_feed(app, "C-y", &t->arena);
    String8 text = test_app_text(t, app);
    TEST_CHECK(t, str8_equal(text, STR8_LIT("world")), "minibuffer: the kill yanked in the buffer: '%S'", text);
    app_dev_feed(app, "C-/", &t->arena);
    // Undo history does not survive from one prompt to the next; the initial input is not undoable.
    test_prompt_text(app, "Text: ", "abc", MINI_TEXT);
    app_dev_feed(app, "C-/", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "abc"), "minibuffer: nothing to undo in a new prompt");
    app_dev_feed(app, "C-g", &t->arena);

    // The mouse: a click in the minibuffer line moves point there; clicks elsewhere are ignored.
    test_prompt_text(app, "Text: ", "abcdef", MINI_TEXT);
    View *m = mb->view;
    Event click = { .kind = EVENT_MOUSE_DOWN, .button = MOUSE_LEFT, .clicks = 1, .x = m->x + 4 + 2 * 8 + 3, .y = m->y + 4 };
    Event up = { .kind = EVENT_MOUSE_UP, .button = MOUSE_LEFT };
    app_dev_feed_events(app, &click, 1, &t->arena);
    app_dev_feed_events(app, &up, 1, &t->arena);
    TEST_CHECK(t, view_point(m, &m->cursors[0]) == 2, "minibuffer: click at column 2 gave point %D", view_point(m, &m->cursors[0]));
    click.x = 30;
    click.y = 30;
    app_dev_feed_events(app, &click, 1, &t->arena);
    app_dev_feed_events(app, &up, 1, &t->arena);
    TEST_CHECK(t, view_point(m, &m->cursors[0]) == 2 && mb->active, "minibuffer: a click in a view is ignored");
    app_dev_feed(app, "C-g", &t->arena);

    // Numbers: N or N:M; anything else is refused with a note and the prompt stays.
    test_prompt_text(app, "Line: ", "", MINI_NUMBER);
    app_dev_feed(app, "1 2 x RET", &t->arena);
    TEST_CHECK(t, mb->active && test_echo_has(app, "Please enter a number") && log->calls == 2, "minibuffer: not a number");
    app_dev_feed(app, "DEL : 5 RET", &t->arena);
    TEST_CHECK(t, !mb->active && log->calls == 3 && log->r.numbers == 2 && log->r.number[0] == 12 && log->r.number[1] == 5,
               "minibuffer: 12:5");
    // yes or no, typed in full.
    test_prompt_text(app, "Sure? (yes or no) ", "", MINI_YES_NO);
    app_dev_feed(app, "y e RET", &t->arena);
    TEST_CHECK(t, mb->active && test_echo_has(app, "Please answer yes or no") && test_input_is(t, app, ""), "minibuffer: 'ye'");
    app_dev_feed(app, "n o RET", &t->arena);
    TEST_CHECK(t, !mb->active && log->calls == 4 && !log->r.yes, "minibuffer: no");
    test_prompt_text(app, "Sure? (yes or no) ", "", MINI_YES_NO);
    app_dev_feed(app, "Y E S RET", &t->arena);
    TEST_CHECK(t, !mb->active && log->calls == 5 && log->r.yes, "minibuffer: YES");

    // A single key: other keys are refused with a note; the quit keys abort, even one of the answers.
    MiniRequest key = { .kind = MINI_KEY, .prompt = STR8_LIT("Save? (y or n) "), .answers = "yn" };
    test_prompt_open(app, &key);
    app_dev_feed(app, "x", &t->arena);
    TEST_CHECK(t, mb->active && test_echo_has(app, "Please answer y or n"), "minibuffer: key 'x' refused");
    app_dev_feed(app, "C-x", &t->arena);
    TEST_CHECK(t, mb->active && test_echo_has(app, "Please answer y or n") && app->keys.pending.len == 0, "minibuffer: a prefix key refused");
    app_dev_feed(app, "Y", &t->arena);
    TEST_CHECK(t, !mb->active && log->calls == 6 && log->r.key == 'y', "minibuffer: key Y");
    const char *quits[] = { "C-g", "ESC", "<f5>", "q" };
    KeySeq seq;
    const char *error;
    key_seq_parse(STR8_LIT("<f5>"), &seq, &error);
    keymap_bind(&app->config->minibuffer, &seq, &CMD_ABORT_MINIBUFFERS);
    key_seq_parse(STR8_LIT("q"), &seq, &error);
    keymap_bind(&app->config->global, &seq, &CMD_KEYBOARD_QUIT);
    for (i32 i = 0; i < ARRAY_COUNT(quits); i++) {
        MiniRequest q = { .kind = MINI_KEY, .prompt = STR8_LIT("Save? (y, n, ! or q) "), .answers = "yn!q" };
        test_prompt_open(app, &q);
        app_dev_feed(app, quits[i], &t->arena);
        TEST_CHECK(t, !mb->active && log->calls == 6 && test_echo_has(app, "Quit"), "minibuffer: %s aborts a key prompt", quits[i]);
    }
    keymap_bind(&app->config->global, &seq, NULL);
    MiniRequest q = { .kind = MINI_KEY, .prompt = STR8_LIT("Save? (y, n, ! or q) "), .answers = "yn!q" };
    test_prompt_open(app, &q);
    app_dev_feed(app, "z", &t->arena);
    TEST_CHECK(t, test_echo_has(app, "Please answer y, n, ! or q"), "minibuffer: the answers listed");
    app_dev_feed(app, "!", &t->arena);
    TEST_CHECK(t, !mb->active && log->calls == 7 && log->r.key == '!', "minibuffer: key !");

    // History: per category, no consecutive duplicates; M-n past the newest gives back the input.
    const char *entries[] = { "one", "two", "two", "three" };
    for (i32 i = 0; i < ARRAY_COUNT(entries); i++) {
        MiniRequest h = { .kind = MINI_TEXT, .prompt = STR8_LIT("M-x "), .initial = str8_cstr(entries[i]), .history = MINI_HISTORY_COMMAND };
        test_prompt_open(app, &h);
        app_dev_feed(app, "RET", &t->arena);
    }
    MiniRequest h = { .kind = MINI_TEXT, .prompt = STR8_LIT("M-x "), .history = MINI_HISTORY_COMMAND };
    test_prompt_open(app, &h);
    app_dev_feed(app, "t y p e d M-p", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "three"), "minibuffer: M-p");
    app_dev_feed(app, "M-p", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "two"), "minibuffer: M-p twice (no duplicate)");
    app_dev_feed(app, "M-p", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "one"), "minibuffer: M-p thrice");
    app_dev_feed(app, "M-p", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "one") && test_echo_has(app, "Beginning of history; no preceding item"), "minibuffer: oldest");
    app_dev_feed(app, "M-n M-n M-n", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "typed"), "minibuffer: M-n back to the typed input");
    app_dev_feed(app, "M-n", &t->arena);
    TEST_CHECK(t, test_echo_has(app, "End of history; no default available"), "minibuffer: newest");
    app_dev_feed(app, "C-g", &t->arena);
    MiniRequest other = { .kind = MINI_TEXT, .prompt = STR8_LIT("File: "), .history = MINI_HISTORY_FILE };
    test_prompt_open(app, &other);
    app_dev_feed(app, "M-p", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "") && test_echo_has(app, "Beginning of history; no preceding item"), "minibuffer: another category");
    // No recursion.
    TEST_CHECK(t, !test_prompt_text(app, "Again: ", "", MINI_TEXT) &&
                  test_echo_has(app, "Command attempted to use minibuffer while in minibuffer"), "minibuffer: recursion refused");
    app_dev_feed(app, "C-g", &t->arena);

    // A chain: the continuation opens the next prompt; an abort there drops the rest of the chain.
    i32 calls = log->calls;
    MiniRequest chain = { .kind = MINI_TEXT, .prompt = STR8_LIT("First: "), .done = test_prompt_chain };
    test_prompt_open(app, &chain);
    app_dev_feed(app, "a RET", &t->arena);
    TEST_CHECK(t, mb->active && log->calls == calls + 1 && str8_equal(mb->prompt, STR8_LIT("Second: ")), "minibuffer: chained prompt");
    app_dev_feed(app, "b RET", &t->arena);
    TEST_CHECK(t, !mb->active && test_logged(calls + 2, "b"), "minibuffer: second link accepted");
    test_prompt_open(app, &chain);
    app_dev_feed(app, "a RET C-g", &t->arena);
    TEST_CHECK(t, !mb->active && log->calls == calls + 3 && !mb->done, "minibuffer: abort in the second link");

    // DEL in a file name prompt removes the last component after a slash.
    MiniRequest file = { .kind = MINI_TEXT, .prompt = STR8_LIT("Find file: "), .initial = STR8_LIT("c:/a/src/"), .file = 1 };
    test_prompt_open(app, &file);
    app_dev_feed(app, "DEL", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "c:/a/"), "minibuffer: DEL after a slash");
    app_dev_feed(app, "x DEL DEL", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "c:/"), "minibuffer: DEL of a character, then of a component");
    app_dev_feed(app, "DEL", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "c:"), "minibuffer: DEL without an earlier slash");
    app_dev_feed(app, "C-g", &t->arena);

    // Opening and closing never moves the calling view.
    for (i32 i = 0; i < 300; i++) app_dev_feed(app, "x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x x RET", &t->arena);
    app_dev_feed(app, "M-< C-v C-v C-e", &t->arena);
    i64 top = buffer_marker_get(v->buffer, v->top), left = v->left_col;
    TEST_CHECK(t, top > 0, "minibuffer: the view is scrolled");
    test_prompt_text(app, "Text: ", "", MINI_TEXT);
    app_dev_feed(app, "a b C-g", &t->arena);
    TEST_CHECK(t, buffer_marker_get(v->buffer, v->top) == top && v->left_col == left, "minibuffer: the scroll position moved");
    if (!test_app_destroy(t, app, "minibuffer")) return 0;
    LOG("test: ok: minibuffer (accept, abort, prefix, editing, mouse, number, yes-or-no, keys, history, recursion, chain, updir, scroll)");
    return 1;
}

// The candidate list, completion commands and M-x, through the keys.
static b32 test_completion(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "completion: app_create failed");
    Minibuffer *mb = &app->mini;
    View *v = app_selected_view(app);
    app_dev_feed(app, "a b c RET d e f C-a", &t->arena);
    i64 top = buffer_marker_get(v->buffer, v->top);

    // M-x runs the chosen command on the calling view, and it becomes last_command.
    app_dev_feed(app, "M-x f o r w a r d - c h a r", &t->arena);
    TEST_CHECK(t, mb->active && mb->match_count >= 1 && str8_equal(mb->cands[mb->matches[0]].text, STR8_LIT("forward-char")) &&
                  mb->cands[mb->matches[0]].score == MATCH_EXACT, "completion: M-x forward-char ranks first");
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, !mb->active && view_point(v, &v->cursors[0]) == 5 && app->ctx.last_command == &CMD_FORWARD_CHAR,
               "completion: M-x forward-char ran (point %D)", view_point(v, &v->cursors[0]));
    // Annotations: the key binding.
    app_dev_feed(app, "M-x f o r w a r d - c h a r", &t->arena);
    TEST_CHECK(t, str8_equal(mb->cands[mb->matches[0]].annotation, STR8_LIT("C-f")), "completion: the binding annotation '%S'",
               mb->cands[mb->matches[0]].annotation);
    app_dev_feed(app, "C-g", &t->arena);
    // No match: RET refuses, the prompt stays; C-j with a partial name too.
    app_dev_feed(app, "M-x x y z z y RET", &t->arena);
    TEST_CHECK(t, mb->active && mb->match_count == 0 && test_echo_has(app, "No match"), "completion: RET with no match");
    app_dev_feed(app, "C-a C-k e x i t C-j", &t->arena);
    TEST_CHECK(t, mb->active && test_echo_has(app, "No match"), "completion: C-j with a partial name");
    // TAB: the longest common prefix first, then "Complete, but not unique" on the exact one.
    app_dev_feed(app, "TAB", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "exit-minibuffer"), "completion: TAB to the common prefix");
    app_dev_feed(app, "TAB", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "exit-minibuffer") && test_echo_has(app, "Complete, but not unique"), "completion: not unique");
    app_dev_feed(app, "- i TAB", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "exit-minibuffer-input"), "completion: TAB to the only match");
    app_dev_feed(app, "TAB", &t->arena);
    TEST_CHECK(t, test_echo_has(app, "Sole completion"), "completion: sole completion");
    // TAB on a substring match completes to the selection; recursion is refused inside.
    app_dev_feed(app, "C-a C-k - b u f f e r TAB", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "beginning-of-buffer"), "completion: TAB to the selection");
    app_dev_feed(app, "M-x", &t->arena);
    TEST_CHECK(t, mb->active && test_echo_has(app, "Command attempted to use minibuffer while in minibuffer"), "completion: no recursion");
    // C-n / C-p / pages move the selection; RET takes it.
    app_dev_feed(app, "C-a C-k o f - b u f f e r", &t->arena);
    TEST_CHECK(t, mb->match_count == 2 && mb->selected == 0, "completion: two matches for 'of-buffer' (%D)", mb->match_count);
    app_dev_feed(app, "C-n C-n C-n", &t->arena);
    TEST_CHECK(t, mb->selected == 1, "completion: C-n stops at the last match");
    app_dev_feed(app, "C-p C-p <down>", &t->arena);
    TEST_CHECK(t, mb->selected == 1, "completion: C-p and <down>");
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, !mb->active && view_point(v, &v->cursors[0]) == buffer_size(v->buffer), "completion: end-of-buffer ran");
    app_dev_feed(app, "M-x", &t->arena);
    i64 all = mb->match_count;
    app_dev_feed(app, "<next> <next>", &t->arena);
    TEST_CHECK(t, mb->selected == 16 && mb->list_top == 9, "completion: two pages down (selected %D, top %D of %D)", mb->selected,
               mb->list_top, all);
    app_dev_feed(app, "<prior>", &t->arena);
    TEST_CHECK(t, mb->selected == 8 && mb->list_top == 8, "completion: a page up (selected %D, top %D)", mb->selected, mb->list_top);
    // Typing filters again and selects the first match.
    app_dev_feed(app, "u n d o", &t->arena);
    TEST_CHECK(t, mb->selected == 0 && mb->list_top == 0 && mb->match_count == 2, "completion: refiltered (%D)", mb->match_count);
    app_dev_feed(app, "C-g", &t->arena);
    TEST_CHECK(t, buffer_marker_get(v->buffer, v->top) == top && v->left_col == 0, "completion: the scroll position moved");
    // History of commands: M-p gives the last one run.
    app_dev_feed(app, "M-x M-p", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, "end-of-buffer"), "completion: command history");
    app_dev_feed(app, "C-g", &t->arena);
    if (!test_app_destroy(t, app, "completion")) return 0;
    LOG("test: ok: completion (M-x, annotations, no match, TAB, selection, pages, refilter, history)");
    return 1;
}

static Buffer *test_current(App *app) {
    return app_selected_view(app)->buffer;
}

static b32 test_current_is(App *app, const char *name) {
    return str8_equal(test_current(app)->name, str8_cstr(name));
}

// set-language through M-x: the language, its states, the mode line; Fundamental gives the states back.
static b32 test_set_language(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "set-language: app_create failed");
    Buffer *b = test_current(app);
    buffer_replace(b, 0, 0, STR8_LIT("let x = `a\nb`;\n"));
    app_dev_feed(app, "M-x s e t - l a n g u a g e RET J a v a S c r i p t RET", &t->arena);
    TEST_CHECK(t, b->language == BUFFER_LANG_JAVASCRIPT && b->states_on && test_echo_has(app, "Language: JavaScript"),
               "set-language: JavaScript (%d, states %d)", b->language, b->states_on);
    TEST_CHECK(t, syntax_line_ready(b, 1) && (buffer_line_state(b, 1) & SYNTAX_STATE_LITERAL), "set-language: states computed (line 1 in the template)");
    app_dev_feed(app, "M-x s e t - l a n g u a g e RET c o b o l RET", &t->arena);
    TEST_CHECK(t, app->mini.active && b->language == BUFFER_LANG_JAVASCRIPT, "set-language: no such language is refused");
    app_dev_feed(app, "C-g M-x s e t - l a n g u a g e RET F u n d RET", &t->arena);
    TEST_CHECK(t, !app->mini.active && b->language == BUFFER_LANG_FUNDAMENTAL && !b->states_on && buffer_states_memory(b) == 0,
               "set-language: Fundamental gives the states back");
    if (!test_app_destroy(t, app, "set-language")) return 0;
    LOG("test: ok: set-language (a language, its states, a refused name, Fundamental)");
    return 1;
}

// Unique buffer names, switch-to-buffer, kill-buffer, through the keys.
static b32 test_buffers(Test *t) {
    String8 root = str8_fmt(&t->arena, "%S\\p7buf", t->tmp_dir);
    os_make_dir(root);
    os_make_dir(str8_fmt(&t->arena, "%S\\a", root));
    os_make_dir(str8_fmt(&t->arena, "%S\\b", root));
    String8 xa = str8_fmt(&t->arena, "%S\\a\\x.c", root), xb = str8_fmt(&t->arena, "%S\\b\\x.c", root);
    String8 one = str8_fmt(&t->arena, "%S\\one.txt", root);
    TEST_CHECK(t, os_write_file(xa, STR8_LIT("a\n")) && os_write_file(xb, STR8_LIT("b\n")) && os_write_file(one, STR8_LIT("1\n")),
               "buffers: cannot write the files");
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "buffers: app_create failed");
    Minibuffer *mb = &app->mini;
    BufferList *list = &app->buffers;
    app_dev_visit(app, xa);
    app_dev_visit(app, xb);
    TEST_CHECK(t, test_current_is(app, "x.c<b>") && buffer_list_find_name(list, STR8_LIT("x.c")), "buffers: unique names");
    app_dev_visit(app, one);

    // switch-to-buffer: the default (most recently shown other buffer) on empty input.
    app_dev_feed(app, "C-x b", &t->arena);
    TEST_CHECK(t, str8_equal(mb->prompt, STR8_LIT("Switch to buffer (default x.c<b>): ")) &&
                  str8_equal(mb->cands[mb->matches[0]].text, STR8_LIT("x.c<b>")), "buffers: switch prompt and default '%S'", mb->prompt);
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, test_current_is(app, "x.c<b>"), "buffers: switched to the default");
    app_dev_feed(app, "C-x b RET", &t->arena);
    TEST_CHECK(t, test_current_is(app, "one.txt"), "buffers: and back");
    app_dev_feed(app, "C-x b x . c RET", &t->arena);
    TEST_CHECK(t, test_current_is(app, "x.c"), "buffers: the exact name first");
    // An unknown name creates an empty buffer: RET with no match, or C-j with the name as typed.
    i32 count = list->count;
    app_dev_feed(app, "C-x b n e w b u f RET", &t->arena);
    TEST_CHECK(t, test_current_is(app, "newbuf") && list->count == count + 1 && !test_current(app)->path.len, "buffers: created by RET");
    app_dev_feed(app, "C-x b o n e C-j", &t->arena);
    TEST_CHECK(t, test_current_is(app, "one") && list->count == count + 2, "buffers: created by C-j");
    app_dev_feed(app, "C-x b C-g", &t->arena);
    TEST_CHECK(t, test_current_is(app, "one") && list->count == count + 2, "buffers: abort switches nothing, creates nothing");

    // kill-buffer: the default is the current buffer; the view shows the most recently shown other one.
    app_dev_feed(app, "C-x k", &t->arena);
    TEST_CHECK(t, str8_equal(mb->prompt, STR8_LIT("Kill buffer (default one): ")), "buffers: kill prompt '%S'", mb->prompt);
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, test_current_is(app, "newbuf") && list->count == count + 1 && !buffer_list_find_name(list, STR8_LIT("one")),
               "buffers: killed the current buffer");
    app_dev_feed(app, "C-x k q q q q RET", &t->arena);
    TEST_CHECK(t, mb->active && test_echo_has(app, "No match"), "buffers: kill an unknown name");
    app_dev_feed(app, "C-g", &t->arena);
    // A modified file buffer asks; no, C-g and yes.
    app_dev_feed(app, "C-x b x . c RET z", &t->arena);
    Buffer *x = test_current(app);
    TEST_CHECK(t, str8_equal(x->name, STR8_LIT("x.c")) && x->modified, "buffers: x.c modified");
    app_dev_feed(app, "C-x k RET", &t->arena);
    TEST_CHECK(t, mb->active && str8_equal(mb->prompt, STR8_LIT("Buffer x.c modified; kill anyway? (yes or no) ")), "buffers: asks");
    app_dev_feed(app, "n o RET", &t->arena);
    TEST_CHECK(t, !mb->active && test_current(app) == x && buffer_list_index(list, x) >= 0, "buffers: 'no' keeps it");
    app_dev_feed(app, "C-x k RET C-g", &t->arena);
    TEST_CHECK(t, !mb->active && test_current(app) == x && buffer_list_index(list, x) >= 0 && test_echo_has(app, "Quit"),
               "buffers: an abort at the question keeps it");
    app_dev_feed(app, "C-x k RET y e s RET", &t->arena);
    TEST_CHECK(t, !mb->active && test_current_is(app, "newbuf") && !buffer_list_find_name(list, STR8_LIT("x.c")), "buffers: 'yes' kills");
    // *Messages* cannot be killed; *scratch* comes back empty.
    app_dev_feed(app, "C-x k * M e s s a g e s * RET", &t->arena);
    TEST_CHECK(t, buffer_list_find_name(list, STR8_LIT("*Messages*")) && test_echo_has(app, "*Messages* cannot be killed"),
               "buffers: *Messages*");
    app_dev_feed(app, "C-x b * s c r a t c h * RET h i", &t->arena);
    Buffer *old = test_current(app);
    count = list->count;
    app_dev_feed(app, "C-x k RET", &t->arena);
    Buffer *fresh = buffer_list_find_name(list, STR8_LIT("*scratch*"));
    TEST_CHECK(t, fresh && fresh != old && buffer_size(fresh) == 0 && list->count == count && test_current_is(app, "newbuf"),
               "buffers: *scratch* recreated");
    if (!test_app_destroy(t, app, "buffers")) return 0;
    LOG("test: ok: buffers (unique names, switch-to-buffer default and creation, kill-buffer, *Messages*, *scratch*)");
    return 1;
}

// A path with forward slashes, as prompts show it.
static String8 test_slashes(Test *t, String8 path) {
    String8 s = str8_copy(&t->arena, path);
    for (i64 i = 0; i < s.len; i++) if (s.data[i] == '\\') s.data[i] = '/';
    return s;
}

static b32 test_file_is(Test *t, String8 path, const char *expected) {
    String8 got;
    return test_read_file(t, path, &got) && str8_equal(got, str8_cstr(expected));
}

// find-file, write-file and save-buffer without a file, against a small tree.
static b32 test_find_write(Test *t) {
    String8 root = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p7files", t->tmp_dir));
    String8 src = str8_fmt(&t->arena, "%S\\src", root);
    os_make_dir(root);
    os_make_dir(src);
    os_make_dir(str8_fmt(&t->arena, "%S\\sub", src));
    String8 app_c = str8_fmt(&t->arena, "%S\\app.c", src);
    String8 written[] = { str8_fmt(&t->arena, "%S\\w.txt", src), str8_fmt(&t->arena, "%S\\nofile.txt", src),
                          str8_fmt(&t->arena, "%S\\new.c", src), str8_fmt(&t->arena, "%S\\*scratch*", src) };
    for (i32 i = 0; i < ARRAY_COUNT(written); i++) os_file_delete(written[i]);
    TEST_CHECK(t, os_write_file(str8_fmt(&t->arena, "%S\\top.txt", root), STR8_LIT("top\n")) && os_write_file(app_c, STR8_LIT("app\n")) &&
                  os_write_file(str8_fmt(&t->arena, "%S\\apple.txt", src), STR8_LIT("apple\n")) &&
                  os_write_file(str8_fmt(&t->arena, "%S\\sub\\deep.c", src), STR8_LIT("deep\n")), "find/write: cannot write the tree");
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "find/write: app_create failed");
    Minibuffer *mb = &app->mini;
    app_dev_visit(app, str8_fmt(&t->arena, "%S\\top.txt", root));

    // The initial input: the buffer's directory, forward slashes, a trailing slash.
    app_dev_feed(app, "C-x C-f", &t->arena);
    String8 want = str8_fmt(&t->arena, "%S/", test_slashes(t, root));
    TEST_CHECK(t, mb->active && str8_equal(minibuffer_input(mb, &t->arena), want) && mb->match_count == 2 &&
                  str8_equal(mb->cands[mb->matches[0]].text, STR8_LIT("src")) && (mb->cands[mb->matches[0]].flags & CANDIDATE_DIR),
               "find/write: initial input '%S', directories first", minibuffer_input(mb, &t->arena));
    // A partial name: prefix matches; RET opens the selected one.
    app_dev_feed(app, "s r c / a p", &t->arena);
    TEST_CHECK(t, mb->match_count == 2 && str8_equal(mb->cands[mb->matches[0]].text, STR8_LIT("app.c")), "find/write: 'ap' in src");
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, !mb->active && test_current_is(app, "app.c"), "find/write: app.c opened");
    // RET on a directory descends; DEL after a slash goes up a component.
    app_dev_feed(app, "C-x C-f s u RET", &t->arena);
    TEST_CHECK(t, mb->active && str8_equal(minibuffer_input(mb, &t->arena), str8_fmt(&t->arena, "%S/src/sub/", test_slashes(t, root))),
               "find/write: descended into sub");
    app_dev_feed(app, "d RET", &t->arena);
    TEST_CHECK(t, test_current_is(app, "deep.c"), "find/write: deep.c opened");
    app_dev_feed(app, "C-x C-f DEL DEL", &t->arena);
    TEST_CHECK(t, str8_equal(minibuffer_input(mb, &t->arena), want), "find/write: DEL DEL up to the root of the tree");
    // A typed directory is not opened; C-j takes the input as typed.
    app_dev_feed(app, "s r c C-j", &t->arena);
    TEST_CHECK(t, !mb->active && test_current_is(app, "deep.c") &&
                  test_echo_has(app, (const char *)str8_fmt(&t->arena, "%S/src is a directory%c", test_slashes(t, root), 0).data),
               "find/write: a directory is not visited ('%S')", str8(app->echo.text, app->echo.len));
    // "~/" after a directory starts over at the profile directory; the shadowed prefix is ignored.
    app_dev_feed(app, "C-x C-f ~ /", &t->arena);
    String8 home = os_full_path(&t->arena, os_get_env(&t->arena, STR8_LIT("USERPROFILE")));
    TEST_CHECK(t, str8_equal(mb->cand_key, home) || str8_equal(mb->cand_key, str8_fmt(&t->arena, "%S\\", home)),
               "find/write: ~/ lists '%S', expected '%S'", mb->cand_key, home);
    app_dev_feed(app, "C-g", &t->arena);
    // A name that does not exist: a new-file buffer visiting it.
    app_dev_feed(app, "C-x C-f DEL n e w . c RET", &t->arena);
    Buffer *nb = test_current(app);
    TEST_CHECK(t, str8_equal(nb->name, STR8_LIT("new.c")) && nb->path.len && test_echo_has(app, "(New file)") &&
                  test_file_absent(written[2]), "find/write: a new file");

    // write-file: a new name writes and visits it.
    app_dev_feed(app, "h e l l o C-x C-w", &t->arena);
    TEST_CHECK(t, str8_equal(mb->prompt, STR8_LIT("Write file: ")) && mb->kind == MINI_CHOICE, "find/write: write prompt");
    app_dev_feed(app, "w . t x t RET", &t->arena);
    TEST_CHECK(t, !mb->active && test_current_is(app, "w.txt") && !nb->modified && test_file_is(t, written[0], "hello"),
               "find/write: written to w.txt ('%S')", str8(app->echo.text, app->echo.len));
    // An existing file: n and C-g leave it, y overwrites.
    app_dev_feed(app, "C-x C-w a p p . c RET", &t->arena);
    TEST_CHECK(t, mb->active && mb->kind == MINI_KEY && str8_equal(mb->prompt, STR8_LIT("File exists; overwrite? (y or n) ")),
               "find/write: overwrite question");
    app_dev_feed(app, "n", &t->arena);
    TEST_CHECK(t, !mb->active && test_file_is(t, app_c, "app\n") && test_current_is(app, "w.txt"), "find/write: n keeps app.c");
    app_dev_feed(app, "C-x C-w a p p . c RET C-g", &t->arena);
    TEST_CHECK(t, !mb->active && test_file_is(t, app_c, "app\n") && test_current_is(app, "w.txt") && test_echo_has(app, "Quit"),
               "find/write: C-g keeps app.c");
    app_dev_feed(app, "C-x C-w a p p . c RET y", &t->arena);
    TEST_CHECK(t, !mb->active && test_file_is(t, app_c, "hello") && str8_equal(test_current(app)->path, app_c),
               "find/write: y overwrites app.c");
    // save-buffer without a file runs the write-file prompt; a directory means the buffer's name in it.
    app_dev_feed(app, "C-x b n o f i l e . t x t RET z z C-x C-s", &t->arena);
    TEST_CHECK(t, mb->active && str8_equal(mb->prompt, STR8_LIT("Write file: ")), "find/write: save-buffer asks for a file");
    app_dev_feed(app, "C-a C-k", &t->arena);
    for (i64 i = 0; i < src.len; i++) {
        Event e[2];
        u8 c = src.data[i];
        const char *err;
        u8 tok[2] = { c, 0 };
        i32 n = key_dev_events(str8(tok, 1), e, &err);
        app_dev_feed_events(app, e, n, &t->arena);
    }
    app_dev_feed(app, "\\ C-j", &t->arena);
    TEST_CHECK(t, !mb->active && test_file_is(t, written[1], "zz") && test_current_is(app, "nofile.txt"),
               "find/write: saved into the directory under the buffer's name ('%S')", str8(app->echo.text, app->echo.len));
    if (!test_app_destroy(t, app, "find/write")) return 0;
    for (i32 i = 0; i < ARRAY_COUNT(written); i++) os_file_delete(written[i]);
    LOG("test: ok: find-file (initial directory, partial names, descend, updir, directories, ~/, new files), write-file, "
        "save-buffer without a file");
    return 1;
}

// goto-line: N, N:M, clamped, refused junk, abort.
static b32 test_goto_line(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "goto-line: app_create failed");
    View *v = app_selected_view(app);
    Buffer *buf = v->buffer;
    for (i32 i = 1; i <= 100; i++) {
        u8 line[32];
        i64 n = fmt_buf(line, sizeof(line), "\tline %d\n", i);
        buffer_replace(buf, buffer_size(buf), buffer_size(buf), str8(line, n));
    }
    app_dev_feed(app, "M-<", &t->arena);
    static const struct { const char *keys; i64 line, col; } cases[] = {
        { "M-g g 5 0 RET", 49, 0 },
        { "M-g M-g 1 0 : 6 RET", 9, 5 },  // column 6 is the visual column after the tab (4 wide), 'i'
        { "M-g g 9 9 9 9 RET", 100, 0 },  // clamped to the last line (empty, after the last newline)
        { "M-g g 0 RET", 0, 0 },
        { "M-g g SPC 3 : 9 9 9 SPC RET", 2, 10 }, // the column clamped to the line's end
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        app_dev_feed(app, cases[i].keys, &t->arena);
        i64 p = view_point(v, &v->cursors[0]);
        i64 line = buffer_line_of(buf, p), col = view_column_of(buf, p);
        TEST_CHECK(t, !app->mini.active && line == cases[i].line && col == cases[i].col && line >= view_top_line(v) &&
                      line < view_top_line(v) + v->rows, "goto-line: '%s' gave line %D column %D", cases[i].keys, line, col);
    }
    i64 before = view_point(v, &v->cursors[0]);
    app_dev_feed(app, "M-g g a b c RET", &t->arena);
    TEST_CHECK(t, app->mini.active && test_echo_has(app, "Please enter a number"), "goto-line: junk refused");
    app_dev_feed(app, "C-g", &t->arena);
    app_dev_feed(app, "M-g g 4 0 C-g", &t->arena);
    TEST_CHECK(t, !app->mini.active && view_point(v, &v->cursors[0]) == before, "goto-line: abort leaves point");
    app_dev_feed(app, "M-g g M-p", &t->arena);
    TEST_CHECK(t, test_input_is(t, app, " 3:999 "), "goto-line: history");
    app_dev_feed(app, "C-g", &t->arena);
    if (!test_app_destroy(t, app, "goto-line")) return 0;
    LOG("test: ok: goto-line (N, N:M, clamping, junk, abort, history)");
    return 1;
}

// save-some-buffers (y, n, !, q, none, abort) and quitting (both answers, aborts, the close button).
static b32 test_save_some(Test *t) {
    String8 root = str8_fmt(&t->arena, "%S\\p7save", t->tmp_dir);
    os_make_dir(root);
    String8 f[3];
    for (i32 i = 0; i < 3; i++) {
        f[i] = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\f%d.txt", root, i + 1));
        TEST_CHECK(t, os_write_file(f[i], STR8_LIT("0\n")), "save-some: cannot write %S", f[i]);
    }
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "save-some: app_create failed");
    Minibuffer *mb = &app->mini;
    Buffer *b[3];
    for (i32 i = 0; i < 3; i++) {
        app_dev_visit(app, f[i]);
        b[i] = test_current(app);
    }
#define TEST_MODIFY(i) do { app_switch_buffer(app, app_selected_view(app), b[i]); app_dev_feed(app, "M-< x", &t->arena); } while (0)
    for (i32 i = 0; i < 3; i++) TEST_MODIFY(i);
    app_dev_feed(app, "C-x s", &t->arena);
    String8 q1 = str8_fmt(&t->arena, "Save file %S? (y, n, !, q) ", test_slashes(t, f[0]));
    TEST_CHECK(t, mb->active && mb->kind == MINI_KEY && str8_equal(mb->prompt, q1), "save-some: first question '%S'", mb->prompt);
    app_dev_feed(app, "y n y", &t->arena);
    TEST_CHECK(t, !mb->active && test_file_is(t, f[0], "x0\n") && test_file_is(t, f[1], "0\n") && b[1]->modified &&
                  test_file_is(t, f[2], "x0\n") && !b[0]->modified && !b[2]->modified, "save-some: y n y");
    TEST_MODIFY(0);
    TEST_MODIFY(2);
    app_dev_feed(app, "C-x s !", &t->arena);
    TEST_CHECK(t, !mb->active && !b[0]->modified && !b[1]->modified && !b[2]->modified && test_file_is(t, f[0], "xx0\n") &&
                  test_file_is(t, f[1], "x0\n"), "save-some: ! saves the rest");
    app_dev_feed(app, "C-x s", &t->arena);
    TEST_CHECK(t, !mb->active && test_echo_has(app, "(No files need saving)"), "save-some: none");
    for (i32 i = 0; i < 3; i++) TEST_MODIFY(i);
    app_dev_feed(app, "C-x s q", &t->arena);
    TEST_CHECK(t, !mb->active && b[0]->modified && b[1]->modified && b[2]->modified, "save-some: q stops");
    // An abort halfway: what was saved stays saved, the rest stays modified, no more questions.
    app_dev_feed(app, "C-x s y C-g", &t->arena);
    TEST_CHECK(t, !mb->active && !b[0]->modified && b[1]->modified && b[2]->modified && test_echo_has(app, "Quit") &&
                  test_file_is(t, f[0], "xxx0\n"), "save-some: abort at the second question");
    app_dev_feed(app, "C-x s ESC", &t->arena);
    TEST_CHECK(t, !mb->active && b[1]->modified && b[2]->modified, "save-some: ESC at the first question");

    // Quitting: every save question first, then "exit anyway?" while modified file buffers remain.
    app_dev_feed(app, "C-x C-c n n", &t->arena);
    TEST_CHECK(t, mb->active && mb->kind == MINI_YES_NO && str8_equal(mb->prompt, STR8_LIT("Modified buffers exist; exit anyway? (yes or no) ")),
               "quit: the exit question '%S'", mb->prompt);
    app_dev_feed(app, "n o RET", &t->arena);
    TEST_CHECK(t, !mb->active && !app->quit, "quit: 'no' stays");
    app_dev_feed(app, "C-x C-c y n C-g", &t->arena);
    TEST_CHECK(t, !mb->active && !app->quit && !b[1]->modified && b[2]->modified, "quit: abort at the exit question keeps the save");
    app_dev_feed(app, "C-x C-c C-g", &t->arena);
    TEST_CHECK(t, !mb->active && !app->quit && b[2]->modified, "quit: abort at a save question");
    // The close button aborts an open prompt and starts the same chain.
    app_dev_feed(app, "C-x b", &t->arena);
    Event close = { .kind = EVENT_CLOSE };
    app_dev_feed_events(app, &close, 1, &t->arena);
    String8 q3 = str8_fmt(&t->arena, "Save file %S? (y, n, !, q) ", test_slashes(t, f[2]));
    TEST_CHECK(t, mb->active && str8_equal(mb->prompt, q3) && !app->quit, "quit: the close button asks '%S'", mb->prompt);
    app_dev_feed(app, "n y e s RET", &t->arena);
    TEST_CHECK(t, app->quit && b[2]->modified, "quit: 'yes' exits without saving");
    if (!test_app_destroy(t, app, "save-some")) return 0;

    // Saving everything quits without the exit question.
    app = test_app_create(t);
    TEST_CHECK(t, app, "save-some: app_create failed");
    app_dev_visit(app, f[0]);
    app_dev_feed(app, "z", &t->arena);
    TEST_CHECK(t, app_dev_feed(app, "C-x C-c", &t->arena) && app->mini.active, "quit: asks about f1");
    TEST_CHECK(t, !app_dev_feed(app, "!", &t->arena) && app->quit && test_file_is(t, f[0], "zxxx0\n"), "quit: ! saves and exits");
    if (!test_app_destroy(t, app, "save-some")) return 0;
    // Nothing modified: C-x C-c exits at once.
    app = test_app_create(t);
    TEST_CHECK(t, app && !app_dev_feed(app, "a C-x C-c", &t->arena) && app->quit, "quit: a modified *scratch* does not ask");
    if (!test_app_destroy(t, app, "save-some")) return 0;
#undef TEST_MODIFY
    LOG("test: ok: save-some-buffers (y, n, !, q, none, aborts) and quitting (both answers, aborts, close button)");
    return 1;
}

// buffer_revert: one replace of the changed middle; markers, undo, line endings, no-ops, read-only;
// then revert-buffer through the keys.
static b32 test_revert(Test *t) {
    String8 path = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p7revert.txt", t->tmp_dir));
    os_dev_set_read_only(path, 0);
    TEST_CHECK(t, os_write_file(path, STR8_LIT("aaaa\nbbbb\ncccc\n")), "revert: cannot write the file");
    Buffer *buf = buffer_create(STR8_LIT(""));
    TEST_CHECK(t, buf && buffer_load_file(buf, path) == OS_FILE_OK, "revert: load failed");
    BufferMarker before = buffer_marker_create(buf, 2, 0), after = buffer_marker_create(buf, 12, 1);
    BufferMarker inside = buffer_marker_create(buf, 6, 0), inside_adv = buffer_marker_create(buf, 7, 1);
    // Typing just before: a merged group of self-inserts that the revert must not join.
    buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_INSERT, 0, 0);
    buffer_replace(buf, 0, 0, STR8_LIT("T"));
    buffer_undo_boundary(buf, BUFFER_UNDO_MERGE_INSERT, 1, 1);
    buffer_replace(buf, 1, 1, STR8_LIT("U"));
    TEST_CHECK(t, os_write_file(path, STR8_LIT("aaaa\nXYZ\ncccc\n")), "revert: cannot change the file");
    i64 groups = buf->undo.group_count;
    TEST_CHECK(t, buffer_revert(buf, 3) == OS_FILE_OK && test_text_is(t, buf, STR8_LIT("aaaa\nXYZ\ncccc\n")) && !buf->modified &&
                  buf->undo.group_count == groups + 1, "revert: text, unmodified, one undo group");
    // The typing was undone by the revert too ("TU" is not in the file): prefix "" .. the whole middle.
    // `before` (at 4 after the typing) was inside it: back to line 0, column 4.
    TEST_CHECK(t, buffer_marker_get(buf, before) == 4 &&buffer_marker_get(buf, after) == 11, "revert: markers before (%D) and after (%D)",
               buffer_marker_get(buf, before), buffer_marker_get(buf, after));
    buffer_marker_destroy(buf, before);
    buffer_marker_destroy(buf, after);
    buffer_marker_destroy(buf, inside);
    buffer_marker_destroy(buf, inside_adv);
    // Undo restores the text before the revert (modified); undo-redo returns to the file (unmodified).
    i64 p;
    TEST_CHECK(t, buffer_undo(buf, 0, 0, &p) == BUFFER_UNDO_DONE && test_text_is(t, buf, STR8_LIT("TUaaaa\nbbbb\ncccc\n")) && buf->modified,
               "revert: undo restores the old text");
    TEST_CHECK(t, buffer_undo(buf, 1, 0, &p) == BUFFER_UNDO_DONE && test_text_is(t, buf, STR8_LIT("aaaa\nbbbb\ncccc\n")),
               "revert: the typing is a group of its own");
    TEST_CHECK(t, buffer_redo(buf, 0, &p) == BUFFER_UNDO_DONE && buffer_redo(buf, 0, &p) == BUFFER_UNDO_DONE &&
                  test_text_is(t, buf, STR8_LIT("aaaa\nXYZ\ncccc\n")) && !buf->modified, "revert: undo-redo back to the file, unmodified");
    TEST_CHECK(t, buffer_destroy(buf), "revert: destroy");

    // Exact marker positions on a clean buffer: before, after (same distance from the end), inside
    // (by line and column, whatever their insertion type).
    TEST_CHECK(t, os_write_file(path, STR8_LIT("aaaa\nbbbb\ncccc\n")), "revert: cannot write the file");
    buf = buffer_create(STR8_LIT(""));
    TEST_CHECK(t, buf && buffer_load_file(buf, path) == OS_FILE_OK, "revert: load failed");
    before = buffer_marker_create(buf, 2, 0);
    after = buffer_marker_create(buf, 12, 1);
    inside = buffer_marker_create(buf, 6, 0);
    inside_adv = buffer_marker_create(buf, 7, 1);
    TEST_CHECK(t, os_write_file(path, STR8_LIT("aaaa\nXYZ\ncccc\n")), "revert: cannot change the file");
    TEST_CHECK(t, buffer_revert(buf, 0) == OS_FILE_OK, "revert: failed");
    TEST_CHECK(t, buffer_marker_get(buf, before) == 2 && buffer_marker_get(buf, after) == 11 && buffer_marker_get(buf, inside) == 6 &&
                  buffer_marker_get(buf, inside_adv) == 7,"revert: markers %D %D %D %D", buffer_marker_get(buf, before),
               buffer_marker_get(buf, after), buffer_marker_get(buf, inside), buffer_marker_get(buf, inside_adv));
    buffer_marker_destroy(buf, before);
    buffer_marker_destroy(buf, after);
    buffer_marker_destroy(buf, inside);
    buffer_marker_destroy(buf, inside_adv);
    // An insertion: a marker exactly at the start of the (empty) range stays before the new text,
    // whatever its insertion type; one after it shifts.
    TEST_CHECK(t, os_write_file(path, STR8_LIT("aaaa\nXQYZ\ncccc\n")), "revert: cannot change the file");
    inside = buffer_marker_create(buf, 6, 1);
    after = buffer_marker_create(buf, 7, 0);
    TEST_CHECK(t, buffer_revert(buf, 0) == OS_FILE_OK && buffer_marker_get(buf, inside) == 6 && buffer_marker_get(buf, after) == 8,
               "revert: insertion, markers %D %D", buffer_marker_get(buf, inside), buffer_marker_get(buf, after));
    buffer_marker_destroy(buf, inside);
    buffer_marker_destroy(buf, after);
    TEST_CHECK(t, os_write_file(path, STR8_LIT("aaaa\nXYZ\ncccc\n")) && buffer_revert(buf, 0) == OS_FILE_OK, "revert: back");
    // Line endings only: the same text, the mode taken over, no edit, no undo group.
    TEST_CHECK(t, os_write_file(path, STR8_LIT("aaaa\r\nXYZ\r\ncccc\r\n")), "revert: cannot change the file");
    u64 edits = buf->edit_count;
    groups = buf->undo.group_count;
    TEST_CHECK(t, buffer_revert(buf, 0) == OS_FILE_OK && buf->eol == BUFFER_EOL_CRLF && buf->edit_count == edits &&
                  buf->undo.group_count == groups && !buf->modified && test_text_is(t, buf, STR8_LIT("aaaa\nXYZ\ncccc\n")),
               "revert: line endings only");
    // An unchanged file: no edit, no group.
    TEST_CHECK(t, buffer_revert(buf, 0) == OS_FILE_OK && buf->edit_count == edits && buf->undo.group_count == groups, "revert: unchanged");
    // A multi-byte character changed in its last byte: the replaced range covers whole characters.
    TEST_CHECK(t, os_write_file(path, STR8_LIT("a\xC3\xA9z\n")), "revert: cannot change the file");
    TEST_CHECK(t, buffer_revert(buf, 0) == OS_FILE_OK && os_write_file(path, STR8_LIT("a\xC3\xA8z\n")) && buffer_revert(buf, 0) == OS_FILE_OK &&
                  test_text_is(t, buf, STR8_LIT("a\xC3\xA8z\n")) && buf->undo.groups[buf->undo.group_count - 1].size ==
                  sizeof(BufferUndoRecord) + 8, "revert: whole characters");
    // A read-only file reverts and the buffer stays read-only.
    TEST_CHECK(t, os_write_file(path, STR8_LIT("ro\n")) && os_dev_set_read_only(path, 1), "revert: cannot make the file read-only");
    TEST_CHECK(t, buffer_revert(buf, 0) == OS_FILE_OK && buf->read_only && test_text_is(t, buf, STR8_LIT("ro\n")), "revert: read-only");
    os_dev_set_read_only(path, 0);
    // A missing file: refused, the buffer untouched.
    os_file_delete(path);
    edits = buf->edit_count;
    TEST_CHECK(t, buffer_revert(buf, 0) == OS_FILE_NOT_FOUND && buf->edit_count == edits && test_text_is(t, buf, STR8_LIT("ro\n")),
               "revert: a missing file");
    TEST_CHECK(t, buffer_destroy(buf), "revert: destroy");

    // A whole-file rewrite (every line changed, as an outside formatter does): markers keep their line
    // and character column, clamped to the line's end and to the last line.
    {
        u8 *text = PUSH_ARRAY(&t->arena, u8, 4096);
        i64 n = 0;
        for (i32 i = 0; i < 100; i++) n += fmt_buf(text + n, 4096 - n, "line %d\n", i);
        TEST_CHECK(t, os_write_file(path, str8(text, n)), "revert: cannot write the file");
        buf = buffer_create(STR8_LIT(""));
        TEST_CHECK(t, buf && buffer_load_file(buf, path) == OS_FILE_OK, "revert: load failed");
        BufferMarker m50 = buffer_marker_create(buf, buffer_line_start(buf, 50) + 3, 1);
        BufferMarker m70 = buffer_marker_create(buf, buffer_line_start(buf, 70) + 7, 0);
        BufferMarker top = buffer_marker_create(buf, buffer_line_start(buf, 40), 0);
        BufferMarker end = buffer_marker_create(buf, buffer_size(buf), 1);
        // Every line rewritten; line 70 becomes shorter than column 7 ("\xC3\xA9" is one character).
        n = 0;
        for (i32 i = 0; i < 100; i++) n += fmt_buf(text + n, 4096 - n, i == 70 ? "\xC3\xA9\n" : "\xC3\xA9LINE %d;\n", i);
        TEST_CHECK(t, os_write_file(path, str8(text, n)) && buffer_revert(buf, 0) == OS_FILE_OK, "revert: whole-file rewrite");
        TEST_CHECK(t, buffer_marker_get(buf, m50) == buffer_line_start(buf, 50) + 4 && buffer_marker_get(buf, m70) == buffer_line_end(buf, 70) &&
                      buffer_marker_get(buf, top) == buffer_line_start(buf, 40) && buffer_marker_get(buf, end) == buffer_size(buf),
                   "revert: a whole-file rewrite keeps line and column (%D %D %D %D)", buffer_marker_get(buf, m50),
                   buffer_marker_get(buf, m70), buffer_marker_get(buf, top), buffer_marker_get(buf, end));
        // A much shorter file: clamped to the last line and its end.
        TEST_CHECK(t, os_write_file(path, STR8_LIT("x\nyy")) && buffer_revert(buf, 0) == OS_FILE_OK, "revert: a shorter file");
        TEST_CHECK(t, buffer_marker_get(buf, m50) == 4 && buffer_marker_get(buf, top) == 2 && buffer_marker_get(buf, end) == 4,
                   "revert: clamped to the last line (%D %D %D)", buffer_marker_get(buf, m50), buffer_marker_get(buf, top),
                   buffer_marker_get(buf, end));
        buffer_marker_destroy(buf, m50);
        buffer_marker_destroy(buf, m70);
        buffer_marker_destroy(buf, top);
        buffer_marker_destroy(buf, end);
        TEST_CHECK(t, buffer_destroy(buf), "revert: destroy");
    }

    // revert-buffer: unmodified reverts at once; modified asks (no, C-g, yes).
    TEST_CHECK(t, os_write_file(path, STR8_LIT("one\n")), "revert: cannot write the file");
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "revert: app_create failed");
    app_dev_visit(app, path);
    Buffer *b = test_current(app);
    os_write_file(path, STR8_LIT("two\n"));
    app_dev_feed(app, "M-x r e v e r t - b u f f e r RET", &t->arena);
    TEST_CHECK(t, !app->mini.active && test_text_is(t, b, STR8_LIT("two\n")) && test_echo_has(app, "Reverted p7revert.txt"),
               "revert: revert-buffer unmodified");
    // Point was at 0, the start of the replaced "one": it stays at line 0, column 0.
    app_dev_feed(app, "x M-x r e v e r t - b u f f e r RET", &t->arena);
    String8 q = str8_fmt(&t->arena, "Discard edits and reread from %S? (yes or no) ", test_slashes(t, path));
    TEST_CHECK(t, app->mini.active && str8_equal(app->mini.prompt, q), "revert: asks '%S'", app->mini.prompt);
    app_dev_feed(app, "n o RET", &t->arena);
    TEST_CHECK(t, test_text_is(t, b, STR8_LIT("xtwo\n")) && b->modified, "revert: 'no' keeps the edits");
    app_dev_feed(app, "M-x r e v e r t - b u f f e r RET C-g", &t->arena);
    TEST_CHECK(t, test_text_is(t, b, STR8_LIT("xtwo\n")) && b->modified && !app->mini.active, "revert: C-g keeps the edits");
    app_dev_feed(app, "M-x r e v e r t - b u f f e r RET y e s RET", &t->arena);
    TEST_CHECK(t, test_text_is(t, b, STR8_LIT("two\n")) && !b->modified, "revert: 'yes' rereads");
    app_dev_feed(app, "C-/", &t->arena);
    TEST_CHECK(t, test_text_is(t, b, STR8_LIT("xtwo\n")) && b->modified, "revert: undone through the keys");
    if (!test_app_destroy(t, app, "revert")) return 0;
    os_file_delete(path);
    LOG("test: ok: revert (one replace, markers by line and column, a whole-file rewrite, undo and undo-redo, line endings, no-op, whole characters, read-only, "
        "missing file, revert-buffer answers)");
    return 1;
}

static void test_focus(App *app, b32 focused, Arena *scratch) {
    Event e = { .kind = EVENT_FOCUS, .focused = focused };
    app_dev_feed_events(app, &e, 1, scratch);
}

static b32 test_mode_line_has(Test *t, App *app, const char *what) {
    String8 mode = app_mode_line_text(app_selected_view(app), &t->arena), w = str8_cstr(what);
    for (i64 i = 0; i + w.len <= mode.len; i++) if (mem_equal(mode.data + i, w.data, w.len)) return 1;
    return 0;
}

// Files changed outside the editor: activation checks, watches, auto-revert, the save guard.
static b32 test_disk(Test *t) {
    String8 root = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p7disk", t->tmp_dir));
    os_make_dir(root);
    String8 path = str8_fmt(&t->arena, "%S\\d.txt", root), other = str8_fmt(&t->arena, "%S\\e.txt", root);
    TEST_CHECK(t, os_write_file(path, STR8_LIT("line1\nline2\nline3\n")) && os_write_file(other, STR8_LIT("e\n")), "disk: cannot write");
    i32 watches_before = os_dev_watch_count();
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "disk: app_create failed");
    View *v = app_selected_view(app);
    test_focus(app, 1, &t->arena);
    app_dev_visit(app, other);
    app_dev_visit(app, path);
    app_dev_feed_events(app, NULL, 0, &t->arena);
    Buffer *b = test_current(app), *e = buffer_list_find_name(&app->buffers, STR8_LIT("e.txt"));
    TEST_CHECK(t, app->watch_count == 1 && app->watches[0].watch && str8_equal(app->watches[0].dir, root), "disk: the directory is watched");

    // Unmodified: reloaded on activation, point kept by the markers. Changes are seen by size and write
    // time, so each change here also changes the size: a rewrite within the same file-time tick at the
    // same size cannot be told apart (the optimized build is that fast).
    app_dev_feed(app, "C-n C-n", &t->arena);
    TEST_CHECK(t, os_write_file(path, STR8_LIT("LINE ONE\nline2\nline3\n")) && os_write_file(other, STR8_LIT("EE\n")), "disk: change");
    test_focus(app, 0, &t->arena);
    test_focus(app, 1, &t->arena);
    TEST_CHECK(t, test_text_is(t, b, STR8_LIT("LINE ONE\nline2\nline3\n")) && view_point(v, &v->cursors[0]) == 15 && !b->modified &&
                  b->disk_state == BUFFER_DISK_OK && test_text_is(t, e, STR8_LIT("EE\n")), "disk: reloaded, point %D",
               view_point(v, &v->cursors[0]));
    // Our own save is not a change.
    app_dev_feed(app, "x C-x C-s", &t->arena);
    TEST_CHECK(t, !app->mini.active && test_file_is(t, path, "LINE ONE\nline2\nxline3\n"), "disk: saved");
    echo_clear(&app->echo);
    test_focus(app, 0, &t->arena);
    test_focus(app, 1, &t->arena);
    TEST_CHECK(t, app->echo.len == 0 && b->disk_state == BUFFER_DISK_OK, "disk: our own save reported '%S'", str8(app->echo.text, app->echo.len));

    // A watch notification while focused: the displayed buffer reloads after the settle delay.
    TEST_CHECK(t, os_write_file(path, STR8_LIT("watched\n")), "disk: change");
    Event dir = { .kind = EVENT_DIR_CHANGED, .watch = app->watches[0].watch }, wake = { .kind = EVENT_WAKEUP };
    app_dev_feed_events(app, &dir, 1, &t->arena);
    TEST_CHECK(t, app->disk_pending && test_text_is(t, b, STR8_LIT("LINE ONE\nline2\nxline3\n")) && app_wait_ms(app) <= CONFIG_SETTLE_MS,
               "disk: not before the settle delay");
    app->disk_due_us = 0;
    app_dev_feed_events(app, &wake, 1, &t->arena);
    TEST_CHECK(t, !app->disk_pending && test_text_is(t, b, STR8_LIT("watched\n")) && test_echo_has(app, "Reverted d.txt"),
               "disk: reloaded by the watch");
    // Undo brings back the text before the reload, as a modified buffer; undo-redo the file's.
    app_dev_feed(app, "C-/", &t->arena);
    TEST_CHECK(t, test_text_is(t, b, STR8_LIT("LINE ONE\nline2\nxline3\n")) && b->modified, "disk: undo of a reload");
    app_dev_feed(app, "C-?", &t->arena);
    TEST_CHECK(t, test_text_is(t, b, STR8_LIT("watched\n")) && !b->modified, "disk: undo-redo of a reload");
    // Without focus a notification is left to the activation check.
    test_focus(app, 0, &t->arena);
    TEST_CHECK(t, os_write_file(path, STR8_LIT("unfocused\n")), "disk: change");
    app_dev_feed_events(app, &dir, 1, &t->arena);
    TEST_CHECK(t, !app->disk_pending && test_text_is(t, b, STR8_LIT("watched\n")), "disk: ignored without focus");
    test_focus(app, 1, &t->arena);
    TEST_CHECK(t, test_text_is(t, b, STR8_LIT("unfocused\n")), "disk: then reloaded on activation");

    // Modified: never reloaded; reported once; flagged in the mode line; saving asks.
    app_dev_feed(app, "M-> m", &t->arena);
    TEST_CHECK(t, os_write_file(path, STR8_LIT("outside\n")), "disk: change");
    test_focus(app, 0, &t->arena);
    test_focus(app, 1, &t->arena);
    TEST_CHECK(t, test_text_is(t, b, STR8_LIT("unfocused\nm")) && b->disk_state == BUFFER_DISK_CHANGED &&
                  test_echo_has(app, "d.txt changed on disk") && test_mode_line_has(t, app, "d.txt  [changed on disk]"),
               "disk: a modified buffer is flagged, not reloaded");
    echo_clear(&app->echo);
    test_focus(app, 0, &t->arena);
    test_focus(app, 1, &t->arena);
    TEST_CHECK(t, app->echo.len == 0 && b->disk_state == BUFFER_DISK_CHANGED, "disk: reported once");
    app_dev_feed(app, "C-x C-s", &t->arena);
    TEST_CHECK(t, app->mini.active && str8_equal(app->mini.prompt, STR8_LIT("d.txt has changed since visited or saved. Save anyway? (yes or no) ")),
               "disk: the save guard '%S'", app->mini.prompt);
    app_dev_feed(app, "n o RET", &t->arena);
    TEST_CHECK(t, test_file_is(t, path, "outside\n") && b->modified, "disk: 'no' does not save");
    app_dev_feed(app, "C-x C-s C-g", &t->arena);
    TEST_CHECK(t, test_file_is(t, path, "outside\n") && b->modified && b->disk_state == BUFFER_DISK_CHANGED, "disk: C-g does not save");
    // In a save-some-buffers walk: an abort at the guard stops the walk.
    app_dev_feed(app, "C-x b e . t x t RET z C-x b RET", &t->arena);
    app_dev_feed(app, "C-x s y y C-g", &t->arena); // e.txt first (list order), then d.txt and its guard
    TEST_CHECK(t, !app->mini.active && b->modified && !e->modified && test_file_is(t, path, "outside\n"),
               "disk: abort at the guard in a walk (e.txt, saved before it, stays saved)");
    app_dev_feed(app, "C-x s y y e s RET", &t->arena);
    TEST_CHECK(t, !app->mini.active && !b->modified && test_file_is(t, path, "unfocused\nm") &&
                  b->disk_state == BUFFER_DISK_OK, "disk: 'yes' at the guard saves and the walk goes on");

    // Deleted: reported once, the buffer stays, saving writes it again without asking.
    os_file_delete(path);
    test_focus(app, 0, &t->arena);
    test_focus(app, 1, &t->arena);
    TEST_CHECK(t, b->disk_state == BUFFER_DISK_DELETED && test_echo_has(app, "d.txt deleted on disk") && test_text_is(t, b, STR8_LIT("unfocused\nm")) &&
                  test_mode_line_has(t, app, "[deleted on disk]"), "disk: deleted");
    app_dev_feed(app, "C-e ! C-x C-s", &t->arena);
    TEST_CHECK(t, !app->mini.active && test_file_is(t, path, "unfocused\nm!") && b->disk_state == BUFFER_DISK_OK, "disk: saved again");

    // auto_revert off: an unmodified buffer is flagged like a modified one.
    app->config->settings.auto_revert = 0;
    TEST_CHECK(t, os_write_file(path, STR8_LIT("no revert\n")), "disk: change");
    test_focus(app, 0, &t->arena);
    test_focus(app, 1, &t->arena);
    TEST_CHECK(t, test_text_is(t, b, STR8_LIT("unfocused\nm!")) && b->disk_state == BUFFER_DISK_CHANGED, "disk: auto_revert off");
    app->config->settings.auto_revert = 1;

    // Watches follow the displayed buffers and are all released.
    app_dev_feed(app, "C-x b * s c r a t c h * RET", &t->arena);
    TEST_CHECK(t, app->watch_count == 0 && os_dev_watch_count() == watches_before, "disk: no watch without a displayed file");
    if (!test_app_destroy(t, app, "disk")) return 0;
    TEST_CHECK(t, os_dev_watch_count() == watches_before, "disk: watches left open");
    LOG("test: ok: changed on disk (activation, watch and settle, focus, undo of a reload, modified, once, save guard, walk, deleted, "
        "auto_revert off, watches)");
    return 1;
}

// The end of the Windows session: the unsaved flag the platform answers from, and the app's chain.
// Highlighting after an auto-revert: the outside change opens a comment at the top; once the states
// caught up, every one equals a lex from scratch.
static b32 test_revert_highlight(Test *t) {
    String8 path = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p8revert.c", t->tmp_dir));
    TEST_CHECK(t, os_write_file(path, STR8_LIT("int a;\n/* x */\nint b;\nchar *s = \"*/\";\n")), "revert highlight: cannot write");
    App *app = test_app_create(t);
    TEST_CHECK(t, app && app_dev_visit(app, path), "revert highlight: app or visit failed");
    Buffer *b = test_current(app);
    test_focus(app, 1, &t->arena);
    TEST_CHECK(t, b->states_on && syntax_line_ready(b, 3) && !(buffer_line_state(b, 2) & SYNTAX_STATE_LITERAL),
               "revert highlight: states before");
    TEST_CHECK(t, os_write_file(path, STR8_LIT("/* open\nint a;\n/* x */\nint b;\nchar *s = \"*/\";\n")), "revert highlight: cannot change");
    test_focus(app, 0, &t->arena);
    test_focus(app, 1, &t->arena); // activation: reverted at once (unmodified)
    TEST_CHECK(t, test_text_is(t, b, STR8_LIT("/* open\nint a;\n/* x */\nint b;\nchar *s = \"*/\";\n")), "revert highlight: reverted");
    app_dev_feed_events(app, NULL, 0, &t->arena); // a frame: catch-up
    i64 last = buffer_line_count(b) - 1;
    TEST_CHECK(t, syntax_line_ready(b, last), "revert highlight: caught up (valid to %D of %D)", b->state_valid, last);
    u32 full[16];
    test_full_states(b, &t->arena, full);
    for (i64 l = 0; l <= last; l++) {
        TEST_CHECK(t, buffer_line_state(b, l) == full[l], "revert highlight: line %D: 0x%x, full 0x%x", l, buffer_line_state(b, l), full[l]);
    }
    TEST_CHECK(t, (buffer_line_state(b, 1) & SYNTAX_STATE_LITERAL) && (buffer_line_state(b, 2) & SYNTAX_STATE_LITERAL) &&
                  !(buffer_line_state(b, 3) & SYNTAX_STATE_LITERAL), "revert highlight: lines 1-2 start in the comment, 3 after it");
    if (!test_app_destroy(t, app, "revert highlight")) return 0;
    os_file_delete(path);
    LOG("test: ok: highlighting after an auto-revert (states equal a lex from scratch)");
    return 1;
}

static b32 test_end_session(Test *t) {
    String8 path = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p7end.txt", t->tmp_dir));
    TEST_CHECK(t, os_write_file(path, STR8_LIT("end\n")), "end session: cannot write");
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "end session: app_create failed");
    app_dev_feed(app, "a b c", &t->arena);
    TEST_CHECK(t, !os_dev_unsaved_files(), "end session: *scratch* does not count");
    app_dev_visit(app, path);
    app_dev_feed(app, "x", &t->arena);
    TEST_CHECK(t, os_dev_unsaved_files(), "end session: a modified file buffer counts");
    app_dev_feed(app, "C-/", &t->arena);
    TEST_CHECK(t, !os_dev_unsaved_files(), "end session: undo back to the saved state");
    app_dev_feed(app, "x", &t->arena);
    Event end = { .kind = EVENT_END_SESSION };
    app_dev_feed_events(app, &end, 1, &t->arena);
    String8 q = str8_fmt(&t->arena, "Save file %S? (y, n, !, q) ", test_slashes(t, path));
    TEST_CHECK(t, app->mini.active && str8_equal(app->mini.prompt, q), "end session: asks to save '%S'", app->mini.prompt);
    app_dev_feed(app, "n", &t->arena);
    app_dev_feed_events(app, &end, 1, &t->arena); // Windows asks again: the chain is not restarted
    TEST_CHECK(t, app->mini.active && app->mini.kind == MINI_YES_NO, "end session: a repeated query restarted the chain");
    app_dev_feed(app, "C-g", &t->arena);
    TEST_CHECK(t, !app->mini.active && !app->quit && os_dev_unsaved_files(), "end session: abort keeps the flag, no exit");
    // A query while another prompt is open: that prompt is aborted, the chain starts.
    app_dev_feed(app, "C-x b", &t->arena);
    app_dev_feed_events(app, &end, 1, &t->arena);
    TEST_CHECK(t, app->mini.active && str8_equal(app->mini.prompt, q), "end session: replaces an open prompt");
    TEST_CHECK(t, !app_dev_feed(app, "y", &t->arena) && app->quit && test_file_is(t, path, "xend\n"), "end session: saved, then exit");
    if (!test_app_destroy(t, app, "end session")) return 0;
    app = test_app_create(t);
    TEST_CHECK(t, app, "end session: app_create failed");
    app_dev_visit(app, path);
    app_dev_feed(app, "z", &t->arena);
    app_dev_feed_events(app, &end, 1, &t->arena);
    TEST_CHECK(t, !app_dev_feed(app, "n y e s RET", &t->arena) && app->quit && test_file_is(t, path, "xend\n"),
               "end session: exit anyway without saving");
    if (!test_app_destroy(t, app, "end session")) return 0;
    os_set_unsaved_files(0);
    LOG("test: ok: end of session (the unsaved flag, the chain, a repeated query, abort, an open prompt, both exits)");
    return 1;
}

// os_list_dir on a small tree: every entry once, directories flagged, "." and ".." left out.
static b32 test_list_dir(Test *t) {
    String8 dir = str8_fmt(&t->arena, "%S\\list", t->tmp_dir);
    os_make_dir(dir);
    os_make_dir(str8_fmt(&t->arena, "%S\\sub", dir));
    TEST_CHECK(t, os_write_file(str8_fmt(&t->arena, "%S\\a.txt", dir), STR8_LIT("a")) &&
                  os_write_file(str8_fmt(&t->arena, "%S\\B.c", dir), STR8_LIT("b")), "list dir: cannot write the files");
    OsDirEntry *e;
    i64 n;
    TEST_CHECK(t, os_list_dir(&t->arena, dir, &e, &n) == OS_FILE_OK && n == 3, "list dir: %D entries, expected 3", n);
    i32 seen = 0;
    for (i64 i = 0; i < n; i++) {
        if (str8_equal(e[i].name, STR8_LIT("a.txt")) && !e[i].is_dir) seen |= 1;
        if (str8_equal(e[i].name, STR8_LIT("B.c")) && !e[i].is_dir) seen |= 2;
        if (str8_equal(e[i].name, STR8_LIT("sub")) && e[i].is_dir) seen |= 4;
    }
    TEST_CHECK(t, seen == 7, "list dir: entries or flags wrong (%d)", seen);
    TEST_CHECK(t, os_list_dir(&t->arena, str8_fmt(&t->arena, "%S/", dir), &e, &n) == OS_FILE_OK && n == 3,
               "list dir: a trailing separator");
    TEST_CHECK(t, os_list_dir(&t->arena, str8_fmt(&t->arena, "%S\\sub", dir), &e, &n) == OS_FILE_OK && n == 0, "list dir: empty");
    TEST_CHECK(t, os_list_dir(&t->arena, str8_fmt(&t->arena, "%S\\nope", dir), &e, &n) == OS_FILE_NOT_FOUND && n == 0,
               "list dir: a missing directory");
    LOG("test: ok: directory listing");
    return 1;
}

// ---------------------------------------------------------------------------
// Matching

static void test_candidates(Arena *arena, const char **texts, i64 n, Candidate *out) {
    for (i64 i = 0; i < n; i++) {
        out[i] = (Candidate){ .text = str8_cstr(texts[i]) };
        out[i].folded = match_fold(arena, out[i].text);
    }
}

// The ranked candidates as "a,b,c".
static String8 test_ranked(Test *t, const char **texts, i64 n, const char *input) {
    Candidate *c = PUSH_ARRAY(&t->arena, Candidate, n);
    test_candidates(&t->arena, texts, n, c);
    i32 *order = PUSH_ARRAY(&t->arena, i32, n);
    MatchQuery q = match_query(&t->arena, str8_cstr(input));
    i64 found = match_rank(&q, c, n, order, 0);
    String8 s = str8_fmt(&t->arena, "");
    for (i64 i = 0; i < found; i++) s = str8_fmt(&t->arena, i ? "%S,%S" : "%S%S", s, c[order[i]].text);
    return s;
}

static b32 test_matcher(Test *t) {
    static const char *cmds[] = { "save-buffer", "find-file", "write-file", "file-find", "files", "FIND-FILE-X", "kill-buffer" };
    static const struct { const char *input, *expected; } cases[] = {
        { "", "save-buffer,find-file,write-file,file-find,files,FIND-FILE-X,kill-buffer" }, // everything, own order
        { "file", "file-find,files,find-file,write-file,FIND-FILE-X" },  // prefix first, then substring, stable
        { "find-file", "find-file,FIND-FILE-X" },                        // exact first, then prefix
        { "FIND-file", "find-file,FIND-FILE-X" },                        // case-insensitive
        { "buf  save", "save-buffer" },                                  // terms in any order, extra spaces
        { "file find", "file-find,find-file,FIND-FILE-X" },              // all terms; prefix of the first term first
        { "fi le", "find-file,file-find,files,FIND-FILE-X,write-file" }, // "fi" is a prefix of four
        { "xyz", "" },                                                   // no match
        { "buffer kill x", "" },                                         // one term missing
        { "  ", "save-buffer,find-file,write-file,file-find,files,FIND-FILE-X,kill-buffer" }, // blanks only: no terms
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        String8 got = test_ranked(t, cmds, ARRAY_COUNT(cmds), cases[i].input);
        TEST_CHECK(t, str8_equal(got, str8_cstr(cases[i].expected)), "matcher: '%s' gave '%S', expected '%s'", cases[i].input, got,
                   cases[i].expected);
    }
    // Non-ASCII folding keeps byte offsets; spans point into the shown text.
    static const char *words[] = { "\xC3\x9C" "berstra\xC3\x9F" "e", "\xC4\xB0stanbul" }; // Überstraße, İstanbul
    String8 got = test_ranked(t, words, 2, "\xC3\xBC" "ber");                                // über
    TEST_CHECK(t, str8_equal(got, str8_cstr(words[0])), "matcher: non-ASCII case folding ('%S')", got);
    Candidate c[2];
    test_candidates(&t->arena, words, 2, c);
    TEST_CHECK(t, c[1].folded.len == c[1].text.len, "matcher: folding changed a length");
    MatchQuery q = match_query(&t->arena, STR8_LIT("STRA e"));
    MatchSpan spans[4];
    i32 n = match_spans(&q, &c[0], spans, 4);
    TEST_CHECK(t, n == 2 && spans[0].start == 5 && spans[0].end == 9 && spans[1].start == 3 && spans[1].end == 4,
               "matcher: spans (%d: %D-%D, %D-%D)", n, spans[0].start, spans[0].end, spans[1].start, spans[1].end);
    // Narrowing: while the input only grows, ranking just the previous matches gives the full result.
    Test rng = { .rng = 0x5EED };
    Candidate *pool = PUSH_ARRAY(&t->arena, Candidate, 500), *full = PUSH_ARRAY(&t->arena, Candidate, 500);
    for (i32 i = 0; i < 500; i++) {
        u8 *w = PUSH_ARRAY(&t->arena, u8, 12);
        i64 len = 1 + test_below(&rng, 11);
        for (i64 k = 0; k < len; k++) w[k] = "abcAB -"[test_below(&rng, 7)];
        pool[i] = (Candidate){ .text = str8(w, len) };
        pool[i].folded = match_fold(&t->arena, pool[i].text);
        full[i] = pool[i];
    }
    i32 *a = PUSH_ARRAY(&t->arena, i32, 500), *b = PUSH_ARRAY(&t->arena, i32, 500);
    for (i32 round = 0; round < 200; round++) {
        u8 typed[8];
        i64 len = 0;
        MatchQuery q0 = match_query(&t->arena, str8(typed, 0));
        match_rank(&q0, pool, 500, a, 0);
        while (len < (i64)sizeof(typed)) {
            typed[len++] = (u8)"abcAB -"[test_below(&rng, 7)];
            MatchQuery qn = match_query(&t->arena, str8(typed, len));
            i64 na = match_rank(&qn, pool, 500, a, 1), nb = match_rank(&qn, full, 500, b, 0);
            TEST_CHECK(t, na == nb && test_equal((u8 *)a, (u8 *)b, na * (i64)sizeof(i32)), "matcher: narrowing differs for '%S' (%D vs %D)",
                       str8(typed, len), na, nb);
        }
    }
    LOG("test: ok: matcher (%d ranking cases, folding, spans, narrowing on 200 typed inputs)", (i32)ARRAY_COUNT(cases));
    return 1;
}

// ---------------------------------------------------------------------------
// Steps of docs/MANUAL_TESTS.md checked through the headless app; their ids are in the log lines.

static App *test_manual_app(Test *t, String8 file, i64 line, i64 col, String8 config) {
    AppFileArg *f = PUSH_STRUCT(&t->arena, AppFileArg);
    *f = (AppFileArg){ file, line, col };
    AppArgs args = { .dpi_scale = 1.0f, .headless = 1, .files = f, .file_count = file.len || line ? 1 : 0, .config_path = config };
    App *app = app_create(&t->arena, &args);
    if (app) app_dev_feed_events(app, NULL, 0, &t->arena); // the first frame: layout, the +LINE:COL jump
    return app;
}

static String8 test_manual_dir(Test *t) {
    String8 dir = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\manual", t->tmp_dir));
    os_make_dir(dir);
    return dir;
}

static b32 test_contains(String8 s, const char *what) {
    String8 w = str8_cstr(what);
    for (i64 i = 0; i + w.len <= s.len; i++) if (mem_equal(s.data + i, w.data, w.len)) return 1;
    return 0;
}

static String8 test_echo(App *app) {
    return str8(app->echo.text, app->echo.len);
}

// C1-C8: the mode line, +LINE:COL, a new file, a directory, the modified flag and saving, bytes that
// are not UTF-8, a read-only file.
static b32 test_manual_files(Test *t) {
    String8 dir = test_manual_dir(t), none = { 0 };
    u8 *lines = PUSH_ARRAY(&t->arena, u8, 300 * 16);
    i64 n = 0;
    for (i32 i = 0; i < 300; i++) n += fmt_buf(lines + n, 16, "int x%03d;\r\n", i);
    String8 c_path = str8_fmt(&t->arena, "%S\\m.c", dir);
    TEST_CHECK(t, os_write_file(c_path, str8(lines, n)), "manual C1: cannot write");
    App *app = test_manual_app(t, c_path, 0, 0, none);
    TEST_CHECK(t, app, "manual C1: app_create failed");
    String8 mode = app_mode_line_text(app_selected_view(app), &t->arena);
    TEST_CHECK(t, test_contains(mode, " -:---  m.c ") && test_contains(mode, "Top   L1 C0    (C)    UTF-8 CRLF"),
               "manual C1: mode line '%S'", mode);
    app_dev_feed(app, "M-g g 1 5 0 RET", &t->arena);
    mode = app_mode_line_text(app_selected_view(app), &t->arena);
    TEST_CHECK(t, test_contains(mode, "%   L150 C0"), "manual C1: a percent in the middle: '%S'", mode);
    if (!test_app_destroy(t, app, "manual C1")) return 0;

    // teal +120:8 file: line 120 centered, column 8 (1-based) is C7.
    app = test_manual_app(t, c_path, 120, 8, none);
    TEST_CHECK(t, app, "manual C2: app_create failed");
    View *v = app_selected_view(app);
    mode = app_mode_line_text(v, &t->arena);
    i64 top = view_top_line(v), want_top = 119 - v->rows / 2;
    TEST_CHECK(t, test_contains(mode, "L120 C7") && top >= want_top - 1 && top <= want_top + 1, "manual C2: '%S', top line %D, expected %D",
               mode, top, want_top);
    if (!test_app_destroy(t, app, "manual C2")) return 0;

    // A path that does not exist: an empty buffer visiting it, "(New file)".
    String8 absent = str8_fmt(&t->arena, "%S\\absent.txt", dir);
    os_file_delete(absent);
    app = test_manual_app(t, absent, 0, 0, none);
    TEST_CHECK(t, app && test_current_is(app, "absent.txt") && buffer_size(test_current(app)) == 0 && test_echo_has(app, "(New file)"),
               "manual C3: a new file: echo '%S'", app ? test_echo(app) : none);
    if (!test_app_destroy(t, app, "manual C3")) return 0;

    // A directory: an error message, *scratch* stays.
    app = test_manual_app(t, dir, 0, 0, none);
    TEST_CHECK(t, app && test_current_is(app, "*scratch*") && str8_starts_with(test_echo(app), STR8_LIT("Cannot open ")) &&
                  test_contains(test_echo(app), "is a directory"), "manual C4: a directory: echo '%S'", app ? test_echo(app) : none);
    if (!test_app_destroy(t, app, "manual C4")) return 0;

    // Editing shows "**", saving says "Wrote ..." and the flag goes.
    String8 e_path = str8_fmt(&t->arena, "%S\\e.txt", dir);
    TEST_CHECK(t, os_write_file(e_path, STR8_LIT("abc\n")), "manual C5: cannot write");
    app = test_manual_app(t, e_path, 0, 0, none);
    TEST_CHECK(t, app, "manual C5: app_create failed");
    app_dev_feed(app, "x", &t->arena);
    TEST_CHECK(t, test_mode_line_has(t, app, " -:**-  e.txt"), "manual C5: '**' after an edit");
    app_dev_feed(app, "C-x C-s", &t->arena);
    TEST_CHECK(t, str8_starts_with(test_echo(app), STR8_LIT("Wrote ")) && test_mode_line_has(t, app, " -:---  e.txt") &&
                  test_file_is(t, e_path, "xabc\n"), "manual C5: saved: echo '%S'", test_echo(app));
    if (!test_app_destroy(t, app, "manual C5")) return 0;

    // Windows-1254 bytes (s, dotless i, g with their Turkish marks) are not UTF-8: kept byte for byte.
    String8 w_path = str8_fmt(&t->arena, "%S\\w1254.txt", dir);
    TEST_CHECK(t, os_write_file(w_path, STR8_LIT("abc \xFE\xFD\xF0 def\r\n\xDE\xDD\xD0\r\n")), "manual C7: cannot write");
    app = test_manual_app(t, w_path, 0, 0, none);
    TEST_CHECK(t, app, "manual C7: app_create failed");
    app_dev_feed(app, "X C-x C-s", &t->arena);
    TEST_CHECK(t, test_file_is(t, w_path, "Xabc \xFE\xFD\xF0 def\r\n\xDE\xDD\xD0\r\n"), "manual C7: the other bytes changed");
    if (!test_app_destroy(t, app, "manual C7")) return 0;

    // A read-only file: "%%", typing refused with a message.
    String8 r_path = str8_fmt(&t->arena, "%S\\ro.txt", dir);
    os_dev_set_read_only(r_path, 0);
    TEST_CHECK(t, os_write_file(r_path, STR8_LIT("read only\n")) && os_dev_set_read_only(r_path, 1), "manual C8: cannot write");
    app = test_manual_app(t, r_path, 0, 0, none);
    TEST_CHECK(t, app, "manual C8: app_create failed");
    b32 flag = test_mode_line_has(t, app, " -:%%-  ro.txt");
    app_dev_feed(app, "a", &t->arena);
    b32 refused = test_echo_has(app, "Buffer is read-only: ro.txt") && test_text_is(t, test_current(app), STR8_LIT("read only\n"));
    os_dev_set_read_only(r_path, 0);
    TEST_CHECK(t, flag && refused, "manual C8: '%%%%' %d, refused %d (echo '%S')", flag, refused, test_echo(app));
    if (!test_app_destroy(t, app, "manual C8")) return 0;
    LOG("test: ok: manual C1 C2 C3 C4 C5 C7 C8 (mode line Top / percent, +LINE:COL centered, new file, directory, ** and Wrote, "
        "Windows-1254 bytes kept, read-only %%%%)");
    return 1;
}

// B7 B9 D9 D10 D11 E9 E11 F10 F11: quit and undefined keys, yanking an emoji and CJK, horizontal
// scrolling back, a tab, the wheel and clicks, three kills as one, RET with tabs, C-q TAB.
static b32 test_manual_editing(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "manual editing: app_create failed");
    View *v = app_selected_view(app);
    Cursor *c = &v->cursors[0];
    app_dev_feed(app, "C-x C-g", &t->arena);
    TEST_CHECK(t, test_echo_has(app, "Quit"), "manual B7: C-x C-g gave '%S'", test_echo(app));
    app_dev_feed(app, "C-x q", &t->arena);
    TEST_CHECK(t, test_echo_has(app, "C-x q is undefined"), "manual B7: C-x q gave '%S'", test_echo(app));

    app_dev_show_scratch(app, STR8_LIT(""));
    os_dev_clipboard_external(STR8_LIT("emoji \xF0\x9F\x98\x80 CJK \xE6\xBC\xA2\xE5\xAD\x97"));
    app_dev_feed(app, "C-y", &t->arena);
    TEST_CHECK(t, test_text_is(t, v->buffer, STR8_LIT("emoji \xF0\x9F\x98\x80 CJK \xE6\xBC\xA2\xE5\xAD\x97")),
               "manual B9: yanked an emoji and CJK");

    u8 long_line[201];
    memset(long_line, 'a', 200);
    long_line[200] = '\n';
    app_dev_show_scratch(app, str8(long_line, 201));
    app_dev_feed(app, "C-e", &t->arena);
    i64 left = v->left_col;
    app_dev_feed(app, "C-a", &t->arena);
    TEST_CHECK(t, left > 0 && v->left_col == 0 && view_point(v, c) == 0, "manual D9: C-e scrolled to %D, C-a back to %D", left, v->left_col);

    app_dev_show_scratch(app, STR8_LIT("\tx\n"));
    app_dev_feed(app, "C-f", &t->arena);
    b32 over = view_point(v, c) == 1 && test_mode_line_has(t, app, "L1 C4");
    app_dev_feed(app, "C-b", &t->arena);
    TEST_CHECK(t, over && view_point(v, c) == 0 && test_mode_line_has(t, app, "L1 C0"), "manual D10: across a tab in one step");

    // The wheel: 5 notches down scroll 15 lines and point comes along; a click sets point.
    u8 *many = PUSH_ARRAY(&t->arena, u8, 200 * 5);
    for (i32 i = 0; i < 200; i++) memcpy(many + i * 5, "line\n", 5);
    app_dev_show_scratch(app, str8(many, 200 * 5));
    test_app_wheel(t, app, -5);
    TEST_CHECK(t, view_top_line(v) == 15 && view_point(v, c) >= buffer_line_start(v->buffer, 15), "manual D11: wheel: top %D, point %D",
               view_top_line(v), view_point(v, c));
    Event click[2] = { { .kind = EVENT_MOUSE_DOWN, .button = MOUSE_LEFT, .clicks = 1, .x = 4 + 3 * 8 + 2, .y = 2 * 16 + 2 },
                       { .kind = EVENT_MOUSE_UP, .button = MOUSE_LEFT, .x = 4 + 3 * 8 + 2, .y = 2 * 16 + 2 } };
    app_dev_feed_events(app, click, 2, &t->arena);
    TEST_CHECK(t, view_point(v, c) == buffer_line_start(v->buffer, 17) + 3, "manual D11: click: point %D", view_point(v, c));

    // E9: a double click selects a word, a triple click a line, a drag from where it went down.
    app_dev_show_scratch(app, STR8_LIT("foo bar baz\nline two\n"));
    Event dbl = { .kind = EVENT_MOUSE_DOWN, .button = MOUSE_LEFT, .clicks = 2, .x = 4 + 5 * 8 + 2, .y = 2 };
    Event up = { .kind = EVENT_MOUSE_UP, .button = MOUSE_LEFT, .x = 4 + 5 * 8 + 2, .y = 2 };
    app_dev_feed_events(app, &dbl, 1, &t->arena);
    app_dev_feed_events(app, &up, 1, &t->arena);
    TEST_CHECK(t, c->mark_active && buffer_marker_get(v->buffer, c->mark) == 4 && view_point(v, c) == 7, "manual E9: double click");
    Event tri = { .kind = EVENT_MOUSE_DOWN, .button = MOUSE_LEFT, .clicks = 3, .x = 4 + 2 * 8 + 2, .y = 16 + 2 };
    app_dev_feed_events(app, &tri, 1, &t->arena);
    app_dev_feed_events(app, &up, 1, &t->arena);
    TEST_CHECK(t, c->mark_active && buffer_marker_get(v->buffer, c->mark) == 12 && view_point(v, c) == 21, "manual E9: triple click");
    Event drag[3] = { { .kind = EVENT_MOUSE_DOWN, .button = MOUSE_LEFT, .clicks = 1, .x = 4 + 2, .y = 2 },
                      { .kind = EVENT_MOUSE_MOVE, .x = 4 + 3 * 8 + 2, .y = 16 + 2 },
                      { .kind = EVENT_MOUSE_UP, .button = MOUSE_LEFT, .x = 4 + 3 * 8 + 2, .y = 16 + 2 } };
    app_dev_feed_events(app, drag, 3, &t->arena);
    TEST_CHECK(t, c->mark_active && buffer_marker_get(v->buffer, c->mark) == 0 && view_point(v, c) == 15, "manual E9: drag: mark %D, point %D",
               buffer_marker_get(v->buffer, c->mark), view_point(v, c));

    // E11: three C-k in a row are one piece for C-y.
    app_dev_show_scratch(app, STR8_LIT("a\nb\nc\nd\n"));
    app_dev_feed(app, "C-k C-k C-k M-> C-y", &t->arena);
    TEST_CHECK(t, test_text_is(t, v->buffer, STR8_LIT("\nc\nd\na\nb")), "manual E11: three kills yanked as one");

    // F11: C-q TAB inserts a tab.
    app_dev_show_scratch(app, STR8_LIT(""));
    app_dev_feed(app, "C-q TAB", &t->arena);
    TEST_CHECK(t, test_text_is(t, v->buffer, STR8_LIT("\t")), "manual F11: C-q TAB");

    // F10: a file indented with tabs: RET indents with tabs.
    String8 tabs = str8_fmt(&t->arena, "%S\\tabs.c", test_manual_dir(t));
    TEST_CHECK(t, os_write_file(tabs, STR8_LIT("int f() {\n\tif (x) {\n\t\ty();\n\t}\n}\n")) && app_dev_visit(app, tabs), "manual F10: visit");
    app_dev_feed(app, "C-n C-n C-e RET", &t->arena);
    TEST_CHECK(t, test_text_is(t, test_current(app), STR8_LIT("int f() {\n\tif (x) {\n\t\ty();\n\t\t\n\t}\n}\n")), "manual F10: RET with tabs");
    if (!test_app_destroy(t, app, "manual editing")) return 0;
    LOG("test: ok: manual B7 B9 D9 D10 D11 E9 E11 F10 F11 (C-x C-g, C-x q, emoji and CJK yanked, C-a after horizontal scrolling, "
        "a tab in one step, wheel and click, double / triple click and drag, three C-k as one, RET with tabs, C-q TAB)");
    return 1;
}

// G1 G4 G10: the colors of the token kinds; "/*" typed on line 1 makes the rest a comment and "*/" ends it;
// set-language Jai.
static b32 test_manual_syntax(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "manual syntax: app_create failed");
    String8 path = str8_fmt(&t->arena, "%S\\g4.c", test_manual_dir(t));
    TEST_CHECK(t, os_write_file(path, STR8_LIT("int a;\nint b; // x\nint c;\n")) && app_dev_visit(app, path), "manual G4: visit");
    Buffer *buf = test_current(app);
    SyntaxToken tokens[64];
    SyntaxKind kinds[2];
    for (i32 step = 0; step < 2; step++) {
        app_dev_feed(app, step == 0 ? "M-< / *" : "* /", &t->arena);
        test_app_pump(t, app);
        SyntaxTokens out = { tokens, 0, ARRAY_COUNT(tokens) };
        String8 line = buffer_text(buf, &t->arena, buffer_line_start(buf, 2), buffer_line_end(buf, 2));
        TEST_CHECK(t, syntax_line_tokens(buf, 2, line, &out), "manual G4: line 3 not ready");
        kinds[step] = test_kind_at(&out, 0);
    }
    TEST_CHECK(t, kinds[0] == SYN_COMMENT && kinds[1] == SYN_TYPE, "manual G4: line 3 after '/*' %d, after '*/' %d", kinds[0], kinds[1]);
    app_dev_feed(app, "M-x s e t - l a n g u a g e RET J a i RET", &t->arena);
    TEST_CHECK(t, test_echo_has(app, "Language: Jai") && buf->language == BUFFER_LANG_JAI, "manual G10: '%S'", test_echo(app));
    // G1: each token kind is drawn in its theme role, with the default theme's colors.
    static const struct { SyntaxKind kind; u32 rgb; } roles[] = {
        { SYN_TEXT, 0xd3b58d }, { SYN_COMMENT, 0x3fdf1f }, { SYN_STRING, 0x0fdfaf }, { SYN_NUMBER, 0x7ad0c6 }, { SYN_KEYWORD, 0xffffff },
        { SYN_TYPE, 0x8cde94 }, { SYN_CONSTANT, 0x7ad0c6 }, { SYN_DIRECTIVE, 0x8cde94 }, { SYN_FUNCTION, 0xffffff },
        { SYN_VARIABLE, 0xc1d1e3 }, { SYN_PUNCTUATION, 0xd3b58d },
    };
    for (i32 i = 0; i < ARRAY_COUNT(roles); i++) {
        u32 got = app_kind_color(&app->config->theme, roles[i].kind);
        TEST_CHECK(t, got == roles[i].rgb, "manual G1: kind %d drawn in %06x, expected %06x", (i32)roles[i].kind, got, roles[i].rgb);
    }
    if (!test_app_destroy(t, app, "manual syntax")) return 0;
    LOG("test: ok: manual G1 G4 G10 (every token kind in its theme color; '/*' typed comments out the rest and '*/' restores it; "
        "set-language Jai)");
    return 1;
}

// H1 H6 H7 H9 H13 H14 H15: M-x with two words, c:/, a new file saved, buffer cycling, the close
// button's "no", C-g in prompts, *Messages*.
static b32 test_manual_minibuffer(Test *t) {
    String8 dir = test_manual_dir(t);
    String8 h1 = str8_fmt(&t->arena, "%S\\h1.txt", dir), h7 = str8_fmt(&t->arena, "%S\\h7.txt", dir);
    os_file_delete(h7);
    TEST_CHECK(t, os_write_file(h1, STR8_LIT("one\n")), "manual H1: cannot write");
    App *app = test_app_create(t);
    TEST_CHECK(t, app && app_dev_visit(app, h1), "manual H1: visit");
    Minibuffer *mb = &app->mini;
    app_dev_feed(app, "x M-x s a v SPC b u f", &t->arena);
    TEST_CHECK(t, mb->active && mb->match_count >= 1 && str8_equal(mb->cands[mb->matches[0]].text, STR8_LIT("save-buffer")) &&
                  str8_equal(mb->cands[mb->matches[0]].annotation, STR8_LIT("C-x C-s")), "manual H1: 'sav buf' gave '%S' '%S'",
               mb->match_count ? mb->cands[mb->matches[0]].text : STR8_LIT(""), mb->match_count ? mb->cands[mb->matches[0]].annotation : STR8_LIT(""));
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, !mb->active && test_file_is(t, h1, "xone\n"), "manual H1: RET ran save-buffer");

    app_dev_feed(app, "C-x C-f c : /", &t->arena);
    TEST_CHECK(t, mb->active && mb->cand_key.len == 3 && (mb->cand_key.data[0] | 0x20) == 'c' && mb->cand_key.data[1] == ':',
               "manual H6: c:/ lists '%S'", mb->cand_key);
    app_dev_feed(app, "C-g", &t->arena);
    TEST_CHECK(t, !mb->active && test_current_is(app, "h1.txt"), "manual H14: C-g in find-file");
    app_dev_feed(app, "M-x s e t - l a n g u a g e RET C-g", &t->arena);
    TEST_CHECK(t, !mb->active && test_current(app)->language == BUFFER_LANG_FUNDAMENTAL, "manual H14: C-g in set-language");

    app_dev_feed(app, "C-x C-f h 7 . t x t RET", &t->arena);
    TEST_CHECK(t, test_current_is(app, "h7.txt") && test_echo_has(app, "(New file)") && test_file_absent(h7), "manual H7: (New file)");
    app_dev_feed(app, "h i C-x C-s", &t->arena);
    TEST_CHECK(t, test_file_is(t, h7, "hi"), "manual H7: C-x C-s created the file");

    View *v = app_selected_view(app);
    i64 point = view_point(v, &v->cursors[0]);
    app_dev_feed(app, "C-x <left>", &t->arena);
    b32 left = !test_current_is(app, "h7.txt");
    app_dev_feed(app, "C-x <right>", &t->arena);
    TEST_CHECK(t, left && test_current_is(app, "h7.txt") && view_point(v, &v->cursors[0]) == point, "manual H9: C-x <left> / <right>");

    app_dev_feed(app, "C-x b * M e s s a g e s * RET", &t->arena);
    TEST_CHECK(t, test_current_is(app, "*Messages*"), "manual H15: C-x b *Messages*");

    // The close button with a modified file: the save question, then "exit anyway?"; "no" stays.
    app_dev_feed(app, "C-x b h 7 . t x t RET z", &t->arena);
    Event close = { .kind = EVENT_CLOSE };
    app_dev_feed_events(app, &close, 1, &t->arena);
    b32 asked = mb->active && str8_starts_with(mb->prompt, STR8_LIT("Save file "));
    app_dev_feed(app, "n", &t->arena);
    b32 anyway = mb->active && str8_equal(mb->prompt, STR8_LIT("Modified buffers exist; exit anyway? (yes or no) "));
    String8 prompt = str8_copy(&t->arena, mb->prompt);
    app_dev_feed(app, "n o RET", &t->arena);
    TEST_CHECK(t, asked && anyway && !app->quit && !mb->active, "manual H13: asked %d, exit anyway %d ('%S'), quit %d", asked, anyway, prompt,
               app->quit);
    app_dev_feed(app, "C-/", &t->arena);
    if (!test_app_destroy(t, app, "manual minibuffer")) return 0;
    LOG("test: ok: manual H1 H6 H7 H9 H13 H14 H15 (M-x sav buf with C-x C-s and RET, c:/, a new file created by C-x C-s, "
        "C-x <left> / <right>, the close button's no, C-g in find-file and set-language, *Messages*)");
    return 1;
}

// I1 I2 I5 I6 I7: open-config creates teal.conf; a saved change reloads it ("Reloaded"); tab_width,
// a new binding, an unknown command.
static b32 test_manual_reload(Test *t, App *app, String8 path, String8 text) {
    if (!os_write_file(path, text)) return 0;
    Event dir = { .kind = EVENT_DIR_CHANGED, .watch = app->config_watch }, wake = { .kind = EVENT_WAKEUP };
    app_dev_feed_events(app, &dir, 1, &t->arena);
    app->config_source.settle_at_us = 0; // the settle delay has passed
    app_dev_feed_events(app, &wake, 1, &t->arena);
    return 1;
}

static b32 test_manual_config(Test *t) {
    String8 dir = str8_fmt(&t->arena, "%S\\cfg", test_manual_dir(t)), conf = str8_fmt(&t->arena, "%S\\teal.conf", dir);
    os_file_delete(conf);
    App *app = test_manual_app(t, str8(NULL, 0), 0, 0, conf);
    TEST_CHECK(t, app, "manual I1: app_create failed");
    app_dev_feed(app, "C-c ,", &t->arena);
    String8 created = str8_fmt(&t->arena, "Created %S from the built-in defaults", conf);
    TEST_CHECK(t, str8_equal(test_echo(app), created) && test_current_is(app, "teal.conf") && test_file_is(t, conf, (const char *)config_default_text().data) &&
                  app->config_watch, "manual I1: open-config: '%S'", test_echo(app));

    TEST_CHECK(t, test_manual_reload(t, app, conf, STR8_LIT("[colors]\nbackground = #202020\n")) && test_echo_has(app, "Reloaded teal.conf") &&
                  app->config->theme.background == 0x202020, "manual I2: echo '%S', background %06x", test_echo(app), app->config->theme.background);

    View *v = app_selected_view(app);
    TEST_CHECK(t, test_manual_reload(t, app, conf, STR8_LIT("[settings]\ntab_width = 8\n")), "manual I5: write");
    app_dev_show_scratch(app, STR8_LIT("\tx"));
    app_dev_feed(app, "C-f", &t->arena);
    TEST_CHECK(t, view_column_of(v->buffer, view_point(v, &v->cursors[0])) == 8, "manual I5: tab_width 8 in the buffer");

    TEST_CHECK(t, test_manual_reload(t, app, conf, STR8_LIT("[keys]\nC-z undo\n")), "manual I6: write");
    app_dev_show_scratch(app, STR8_LIT(""));
    app_dev_feed(app, "a b", &t->arena);
    app_dev_feed(app, "C-z", &t->arena);
    TEST_CHECK(t, test_text_is(t, v->buffer, STR8_LIT("")), "manual I6: C-z undo");

    TEST_CHECK(t, test_manual_reload(t, app, conf, STR8_LIT("[keys]\nC-z no-such-command\n")) &&
                  test_echo_has(app, "teal.conf:2: unknown command 'no-such-command'"), "manual I7: echo '%S'", test_echo(app));
    app_dev_feed(app, "o k", &t->arena);
    TEST_CHECK(t, test_text_is(t, v->buffer, STR8_LIT("ok")), "manual I7: the editor keeps working");
    if (!test_app_destroy(t, app, "manual config")) return 0;
    LOG("test: ok: manual I1 I2 I5 I6 I7 (open-config creates and visits teal.conf; a saved change reloads with 'Reloaded teal.conf'; "
        "tab_width; C-z undo; an unknown command reported, editing goes on)");
    return 1;
}

// J1 K4 K13: "Reverted" when coming back; C-x C-x after a search; replacing foo / Foo / FOO.
static b32 test_manual_disk_search(Test *t) {
    String8 path = str8_fmt(&t->arena, "%S\\j1.txt", test_manual_dir(t));
    TEST_CHECK(t, os_write_file(path, STR8_LIT("one\ntwo\n")), "manual J1: cannot write");
    App *app = test_app_create(t);
    TEST_CHECK(t, app && app_dev_visit(app, path), "manual J1: visit");
    test_focus(app, 1, &t->arena);
    app_dev_feed(app, "C-n", &t->arena);
    TEST_CHECK(t, os_write_file(path, STR8_LIT("ONE\ntwo\nthree\n")), "manual J1: change");
    test_focus(app, 0, &t->arena);
    test_focus(app, 1, &t->arena);
    View *v = app_selected_view(app);
    TEST_CHECK(t, test_echo_has(app, "Reverted j1.txt") && view_point(v, &v->cursors[0]) == 4, "manual J1: echo '%S', point %D", test_echo(app),
               view_point(v, &v->cursors[0]));

    app_dev_show_scratch(app, STR8_LIT("a foo b foo\n"));
    app_dev_feed(app, "C-s f o o RET", &t->arena);
    i64 at = view_point(v, &v->cursors[0]);
    app_dev_feed(app, "C-x C-x", &t->arena);
    TEST_CHECK(t, at == 5 && view_point(v, &v->cursors[0]) == 0, "manual K4: C-x C-x after the search: %D then %D", at, view_point(v, &v->cursors[0]));

    app_dev_show_scratch(app, STR8_LIT("foo Foo FOO\n"));
    app_dev_feed(app, "C-g M-% f o o RET b a r RET !", &t->arena); // C-g: C-x C-x left the region active
    test_app_pump(t, app); // replace-all works over frames
    TEST_CHECK(t, test_text_is(t, v->buffer, STR8_LIT("bar Bar BAR\n")) && test_echo_has(app, "Replaced 3 occurrences"),
               "manual K13: echo '%S', text '%S', point %D", test_echo(app), test_app_text(t, app), test_app_point(app));
    if (!test_app_destroy(t, app, "manual disk and search")) return 0;
    LOG("test: ok: manual J1 K4 K13 ('Reverted' on coming back, point kept; C-x C-x back to the search's start; foo Foo FOO -> bar Bar BAR, "
        "'Replaced 3 occurrences')");
    return 1;
}

// ---------------------------------------------------------------------------
// The window tree (window.c), headless: views are stand-in pointers the tree never looks into.

#define TEST_VIEW(k) ((View *)(uintptr_t)(0x1000 + (k)))

static const WindowMetrics test_window_m = { .cell_w = 8, .line_h = 16, .pad = 4, .divider = 1 };

// The leaves tile the frame exactly (inside it, no overlap, the areas add up), no size is negative, the
// tree's links agree, and with a frame that fits every minimum, every window has its minimum.
static b32 test_window_tiles(Test *t, WindowTree *w, const char *what) {
    i32 leaves[WINDOW_MAX];
    i32 n = window_leaves(w, leaves);
    TEST_CHECK(t, n >= 1 && n <= WINDOW_MAX, "windows: %s: %d windows", what, n);
    i64 area = 0;
    for (i32 i = 0; i < n; i++) {
        WindowNode *a = &w->nodes[leaves[i]];
        TEST_CHECK(t, a->w >= 0 && a->h >= 0, "windows: %s: window %d has %dx%d", what, leaves[i], a->w, a->h);
        TEST_CHECK(t, a->x >= w->x && a->y >= w->y && a->x + a->w <= w->x + w->w && a->y + a->h <= w->y + w->h,
                   "windows: %s: window %d at %d,%d %dx%d leaves the frame %dx%d", what, leaves[i], a->x, a->y, a->w, a->h, w->w, w->h);
        area += (i64)a->w * a->h;
        for (i32 k = 0; k < i; k++) {
            WindowNode *b = &w->nodes[leaves[k]];
            b32 overlap = a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
            TEST_CHECK(t, !overlap, "windows: %s: windows %d and %d overlap", what, leaves[i], leaves[k]);
        }
    }
    TEST_CHECK(t, area == (i64)w->w * w->h, "windows: %s: the windows cover %D of %D pixels", what, area, (i64)w->w * w->h);
    for (i32 i = 0; i < WINDOW_NODE_MAX; i++) {
        WindowNode *node = &w->nodes[i];
        if (!node->used) continue;
        TEST_CHECK(t, (node->parent < 0) == (i == w->root), "windows: %s: node %d's parent %d (root %d)", what, i, node->parent, w->root);
        if (node->split == WINDOW_LEAF) continue;
        for (i32 c = 0; c < 2; c++) TEST_CHECK(t, w->nodes[node->child[c]].parent == i, "windows: %s: node %d's child %d", what, i, c);
    }
    i32 min_w = window_min(w, w->root, WINDOW_SIDE_BY_SIDE), min_h = window_min(w, w->root, WINDOW_STACKED);
    if (min_w <= w->w && min_h <= w->h) {
        for (i32 i = 0; i < n; i++) {
            WindowNode *a = &w->nodes[leaves[i]];
            TEST_CHECK(t, a->w >= window_min(w, leaves[i], WINDOW_SIDE_BY_SIDE) && a->h >= window_min(w, leaves[i], WINDOW_STACKED),
                       "windows: %s: window %d is %dx%d, below the minimum in a %dx%d frame", what, leaves[i], a->w, a->h, w->w, w->h);
        }
    }
    // Every split puts its line at its ratio, to the cell, unless a minimum size or the frame's edge moved it.
    for (i32 i = 0; i < WINDOW_NODE_MAX; i++) {
        WindowNode *node = &w->nodes[i];
        if (!node->used || node->split == WINDOW_LEAF) continue;
        i32 total = window_size(w, i, node->split), first = window_size(w, node->child[0], node->split);
        i32 unit = window_unit(w, node->split);
        i32 min0 = window_min(w, node->child[0], node->split), min1 = window_min(w, node->child[1], node->split);
        f32 at = node->ratio * (f32)total;
        b32 near_ratio = first >= at - (f32)unit && first <= at + (f32)unit;
        b32 clamped = first == min0 || first == total - min1 || first == 0 || first == total;
        TEST_CHECK(t, near_ratio || clamped, "windows: %s: split %d puts %d of %d first, ratio %d/1000", what, i, first, total,
                   (i32)(node->ratio * 1000));
    }
    return 1;
}

static b32 test_window_same_rects(Test *t, WindowTree *w, i32 *rects, b32 save, const char *what) {
    for (i32 i = 0; i < WINDOW_NODE_MAX; i++) {
        WindowNode *n = &w->nodes[i];
        i32 r[4] = { n->used ? n->x : -1, n->y, n->w, n->h };
        for (i32 k = 0; k < 4; k++) {
            if (save) rects[i * 4 + k] = r[k];
            else TEST_CHECK(t, rects[i * 4 + k] == r[k], "windows: %s: node %d's rect changed", what, i);
        }
    }
    return 1;
}

static b32 test_window_tree(Test *t, u64 seed) {
    WindowTree tree, *w = &tree;
    WindowMetrics m = test_window_m;
    i32 leaf, b, c;

    // C-x 3, then C-x 2 in the left window: cyclic order left-top, left-bottom, right.
    window_init(w, TEST_VIEW(0));
    window_layout(w, 0, 0, 1280, 784, m);
    i32 a = w->root;
    TEST_CHECK(t, window_split(w, a, WINDOW_SIDE_BY_SIDE, TEST_VIEW(1), &b) == WINDOW_OK, "windows: C-x 3");
    TEST_CHECK(t, window_split(w, a, WINDOW_STACKED, TEST_VIEW(2), &c) == WINDOW_OK, "windows: C-x 2");
    if (!test_window_tiles(t, w, "three windows")) return 0;
    i32 order[WINDOW_MAX];
    TEST_CHECK(t, window_leaves(w, order) == 3 && order[0] == a && order[1] == c && order[2] == b, "windows: cyclic order");
    TEST_CHECK(t, window_next(w, a, 1) == c && window_next(w, c, 1) == b && window_next(w, b, 1) == a && window_next(w, a, -1) == b,
               "windows: next and previous in cyclic order");
    TEST_CHECK(t, w->selected == a && w->nodes[a].w == 640 && w->nodes[b].x == 640 && w->nodes[c].y == 400 && w->nodes[a].h == 400,
               "windows: halves (%d %d %d %d)", w->nodes[a].w, w->nodes[b].x, w->nodes[c].y, w->nodes[a].h);
    TEST_CHECK(t, window_has_divider(w, a) && window_has_divider(w, c) && !window_has_divider(w, b), "windows: dividers");
    TEST_CHECK(t, window_at(w, 10, 10) == a && window_at(w, 10, 400) == c && window_at(w, 700, 10) == b && window_at(w, 2000, 10) == -1,
               "windows: window_at");
    WindowSplit split;
    TEST_CHECK(t, window_edge_at(w, 639, 600, 3, &split) == w->root && split == WINDOW_SIDE_BY_SIDE, "windows: the divider");
    TEST_CHECK(t, window_edge_at(w, 100, 400 - 8, 3, &split) == w->nodes[a].parent && split == WINDOW_STACKED, "windows: the upper mode line");
    TEST_CHECK(t, window_edge_at(w, 100, 784 - 8, 3, &split) == -1 && window_edge_at(w, 100, 100, 3, &split) == -1,
               "windows: no edge on the bottom mode line or in the text");
    // The LRU and MRU choices.
    window_select(w, b);
    window_select(w, a);
    TEST_CHECK(t, window_lru(w, a) == c, "windows: the least recently used window");
    View *gone = window_delete(w, a);
    TEST_CHECK(t, gone == TEST_VIEW(0) && w->selected == b && window_count(w) == 2, "windows: deleting the selected one selects the MRU one");
    if (!test_window_tiles(t, w, "after a delete")) return 0;
    TEST_CHECK(t, w->nodes[c].x == 0 && w->nodes[c].h == 784 && w->nodes[c].w == 640, "windows: the sibling takes the space");

    // Thirds: C-x 3 twice, then balance. Deleting keeps the far window's size; so does a drag.
    window_init(w, TEST_VIEW(0));
    window_layout(w, 0, 0, 1200, 784, m);
    a = w->root;
    window_split(w, a, WINDOW_SIDE_BY_SIDE, TEST_VIEW(1), &b);
    window_split(w, a, WINDOW_SIDE_BY_SIDE, TEST_VIEW(2), &c);
    TEST_CHECK(t, w->nodes[a].w == 304 && w->nodes[c].w == 296 && w->nodes[b].w == 600, "windows: quarters, half (%d %d %d)",
               w->nodes[a].w, w->nodes[c].w, w->nodes[b].w);
    window_balance(w);
    TEST_CHECK(t, w->nodes[a].w == 400 && w->nodes[c].w == 400 && w->nodes[b].w == 400, "windows: balanced thirds (%d %d %d)",
               w->nodes[a].w, w->nodes[c].w, w->nodes[b].w);
    i32 moved = window_move_split(w, w->nodes[a].parent, 480);
    TEST_CHECK(t, moved == 80 && w->nodes[a].w == 480 && w->nodes[c].w == 320 && w->nodes[b].w == 400,
               "windows: a drag changes only the windows beside the line (%d: %d %d %d)", moved, w->nodes[a].w, w->nodes[c].w, w->nodes[b].w);
    moved = window_move_split(w, w->nodes[a].parent, 0);
    TEST_CHECK(t, w->nodes[a].w == window_min(w, a, WINDOW_SIDE_BY_SIDE) || w->nodes[a].w - window_min(w, a, WINDOW_SIDE_BY_SIDE) < 8,
               "windows: a drag stops at the minimum (%d)", w->nodes[a].w);
    window_balance(w);
    TEST_CHECK(t, window_resize(w, c, WINDOW_SIDE_BY_SIDE, 1) && w->nodes[c].w == 408 && w->nodes[b].w == 392 && w->nodes[a].w == 400,
               "windows: enlarge takes a column from the neighbor (%d %d %d)", w->nodes[a].w, w->nodes[c].w, w->nodes[b].w);
    TEST_CHECK(t, window_resize(w, b, WINDOW_SIDE_BY_SIDE, -1) && w->nodes[b].w == 384 && w->nodes[c].w == 416,
               "windows: shrink gives a column to the neighbor");
    TEST_CHECK(t, !window_resize(w, b, WINDOW_STACKED, 1), "windows: no vertical neighbor: cannot enlarge");
    window_delete(w, a);
    TEST_CHECK(t, w->nodes[b].w == 384 && w->nodes[c].w == 816, "windows: deleting gives the space to the adjacent window only (%d %d)",
               w->nodes[c].w, w->nodes[b].w);

    // Refusals: too small, too many.
    window_init(w, TEST_VIEW(0));
    window_layout(w, 0, 0, 1280, 784, m);
    i32 count = 1;
    for (i32 k = 1; k < 20; k++) {
        WindowStatus s = window_split(w, w->selected, WINDOW_STACKED, TEST_VIEW(k), &leaf);
        if (s != WINDOW_OK) {
            TEST_CHECK(t, s == WINDOW_TOO_SMALL && w->nodes[w->selected].h < 2 * WINDOW_MIN_LINES * 16, "windows: refused while big enough");
            break;
        }
        count++;
    }
    TEST_CHECK(t, count == 4 && test_window_tiles(t, w, "stacked until too small"), "windows: %d stacked", count);
    for (i32 k = count; k < 20; k++) {
        i32 leaves[WINDOW_MAX];
        i32 n = window_leaves(w, leaves), biggest = leaves[0];
        for (i32 i = 1; i < n; i++) if (w->nodes[leaves[i]].h > w->nodes[biggest].h) biggest = leaves[i];
        WindowStatus s = window_split(w, biggest, w->nodes[biggest].w >= w->nodes[biggest].h ? WINDOW_SIDE_BY_SIDE : WINDOW_STACKED,
                                      TEST_VIEW(k), &leaf);
        if (s != WINDOW_OK) {
            TEST_CHECK(t, s == WINDOW_TOO_MANY && n == WINDOW_MAX, "windows: refused with %d windows: %d", n, (i32)s);
            break;
        }
    }
    TEST_CHECK(t, window_count(w) == WINDOW_MAX, "windows: %d windows at most", WINDOW_MAX);

    // The fuzz: random operations on random frames, tiny ones included, from several starting shapes.
    t->rng = seed ^ 0x77696e646f77ull;
    i32 rects[WINDOW_NODE_MAX * 4];
    i32 ops = 0, tiny = 0;
    for (i32 round = 0; round < 400; round++) {
        window_init(w, TEST_VIEW(0));
        i32 fw = 200 + (i32)test_below(t, 1800), fh = 100 + (i32)test_below(t, 1100);
        window_layout(w, 0, 0, fw, fh, m);
        for (i32 step = 0; step < 40; step++, ops++) {
            i32 leaves[WINDOW_MAX];
            i32 n = window_leaves(w, leaves);
            i32 pick = leaves[test_below(t, n)];
            u64 r = test_below(t, 100);
            const char *what;
            if (r < 30) {
                what = "split";
                window_split(w, pick, test_below(t, 2) ? WINDOW_SIDE_BY_SIDE : WINDOW_STACKED, TEST_VIEW(step + 1), &leaf);
            } else if (r < 45) {
                what = "delete";
                if (n > 1) window_delete(w, pick);
            } else if (r < 60) {
                what = "resize";
                window_resize(w, pick, test_below(t, 2) ? WINDOW_SIDE_BY_SIDE : WINDOW_STACKED, (i32)test_below(t, 21) - 10);
            } else if (r < 72) {
                what = "drag";
                i32 node = (i32)test_below(t, WINDOW_NODE_MAX);
                if (w->nodes[node].used && w->nodes[node].split != WINDOW_LEAF) {
                    window_move_split(w, node, (i32)test_below(t, window_size(w, node, w->nodes[node].split) + 40) - 20);
                }
            } else if (r < 77) {
                what = "balance";
                window_balance(w);
            } else if (r < 85) {
                what = "select";
                window_select(w, pick);
            } else {
                // A frame resize, there and back: the same rects. Sometimes tiny (down to nothing), sometimes
                // just below what the minimum sizes need.
                what = "frame resize";
                test_window_same_rects(t, w, rects, 1, what);
                i32 ow = w->w, oh = w->h, nw, nh;
                u64 kind = test_below(t, 4);
                if (kind == 0) {
                    nw = (i32)test_below(t, 3);
                    nh = (i32)test_below(t, 3);
                } else if (kind == 1) {
                    nw = (i32)test_below(t, 40);
                    nh = (i32)test_below(t, 40);
                } else if (kind == 2) {
                    nw = MAX(window_min(w, w->root, WINDOW_SIDE_BY_SIDE) - 1 - (i32)test_below(t, 30), 0);
                    nh = MAX(window_min(w, w->root, WINDOW_STACKED) - 1 - (i32)test_below(t, 30), 0);
                } else {
                    nw = 200 + (i32)test_below(t, 1800);
                    nh = 100 + (i32)test_below(t, 1100);
                }
                tiny += nw < 40 || nh < 40;
                window_layout(w, 0, 0, nw, nh, m);
                if (!test_window_tiles(t, w, "a resized frame")) return 0;
                window_layout(w, 0, 0, ow, oh, m);
                if (!test_window_same_rects(t, w, rects, 0, "a frame resized and back")) return 0;
            }
            if (!test_window_tiles(t, w, what)) return 0;
        }
    }
    LOG("test: ok: window tree (cyclic order, halves, dividers, edges, LRU / MRU, deletes, thirds and balance, drags and resizes "
        "that change only the windows beside the line, refusals, a fuzz of %d operations on 400 frames: exact tiling, minimum "
        "sizes, splits at their ratios, frame resizes there and back (%d tiny or empty frames))", ops, tiny);
    return 1;
}

// ---------------------------------------------------------------------------
// Windows through the headless app: a 1280 x 800 frame (the tree gets 1280 x 784 above the echo line),
// 8 x 16 cells, a 4 px pad, a 1 px divider.

static View *test_win(App *app, i32 i) {
    View *views[WINDOW_MAX];
    i32 n = app_views(app, views);
    return i >= 0 && i < n ? views[i] : NULL;
}

static i64 test_win_point(App *app, i32 i) {
    View *v = test_win(app, i);
    return view_point(v, &v->cursors[0]);
}

// *scratch* in the selected window with `lines` numbered lines ("line 0\n" ...), point at the start.
static void test_win_lines(Test *t, App *app, i32 lines) {
    u8 *text = PUSH_ARRAY(&t->arena, u8, (i64)lines * 16);
    i64 n = 0;
    for (i32 i = 0; i < lines; i++) n += fmt_buf(text + n, 16, "line %d\n", i);
    app_dev_show_scratch(app, str8(text, n));
}

static void test_win_mouse(Test *t, App *app, EventKind kind, i32 x, i32 y) {
    Event e = { .kind = kind, .button = MOUSE_LEFT, .x = x, .y = y, .clicks = 1 };
    app_dev_feed_events(app, &e, 1, &t->arena);
}

// " " separated characters of `s` for app_dev_feed.
static const char *test_win_keys(Test *t, String8 s) {
    u8 *out = PUSH_ARRAY(&t->arena, u8, s.len * 2 + 1);
    i64 n = 0;
    for (i64 i = 0; i < s.len; i++) {
        out[n++] = s.data[i];
        out[n++] = ' ';
    }
    out[n] = 0;
    return (const char *)out;
}

// Each command of the phase, the order other-window visits windows in, the refusals.
static b32 test_window_commands(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "window commands: app_create failed");
    test_win_lines(t, app, 300);
    app_dev_feed(app, "M-g g 1 0 0 RET", &t->arena);
    i64 p = test_app_point(app);
    app_dev_feed(app, "C-x 3", &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 2 && app_dev_window_selected(app) == 0 && test_win(app, 1)->buffer == test_win(app, 0)->buffer &&
                  test_win_point(app, 1) == p && test_win(app, 0)->w == 640 && test_win(app, 1)->x == 640 &&
                  app_dev_window_top(app, 1) == app_dev_window_top(app, 0),
               "window commands: C-x 3 (a new window on the same buffer at the same place, the left one selected)");
    app_dev_feed(app, "C-x 2", &t->arena); // the left one: top-left 0, bottom-left 1, right 2
    TEST_CHECK(t, app_dev_window_count(app) == 3 && app_dev_window_selected(app) == 0 && test_win(app, 1)->x == 0 &&
                  test_win(app, 1)->y == 400 && test_win(app, 0)->h == 400 && test_win(app, 2)->h == 784,
               "window commands: C-x 2 (%d %d %d)", test_win(app, 1)->y, test_win(app, 0)->h, test_win(app, 2)->h);
    i32 seen[4];
    for (i32 k = 0; k < 4; k++) {
        app_dev_feed(app, "C-x o", &t->arena);
        seen[k] = app_dev_window_selected(app);
    }
    TEST_CHECK(t, seen[0] == 1 && seen[1] == 2 && seen[2] == 0 && seen[3] == 1, "window commands: C-x o in cyclic order (%d %d %d %d)",
               seen[0], seen[1], seen[2], seen[3]);
    app_dev_feed(app, "C-x o C-x o", &t->arena); // the top-left one selected, the right one most recently before it
    app_dev_feed(app, "C-x 0", &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 2 && app_dev_window_selected(app) == 1 && test_win(app, 0)->h == 784,
               "window commands: C-x 0 deletes it and selects the most recently used one (selected %d)", app_dev_window_selected(app));
    app_dev_feed(app, "C-x 1", &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 1 && test_win(app, 0)->w == 1280, "window commands: C-x 1");
    app_dev_feed(app, "C-x 0", &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 1 && test_app_echo_is(app, "Attempt to delete minibuffer or sole ordinary window"),
               "window commands: C-x 0 on the only window");

    // Refusals: too small (each C-x 2 halves the selected, top window), too many.
    i32 count = 1;
    for (i32 k = 0; k < 10 && !test_app_echo_is(app, "Window too small for splitting"); k++) {
        app_dev_feed(app, "C-x 2", &t->arena);
        count = app_dev_window_count(app);
    }
    TEST_CHECK(t, test_app_echo_is(app, "Window too small for splitting") && count == 4 && test_win(app, 0)->h >= 2 * WINDOW_MIN_LINES * 16 / 2,
               "window commands: 'Window too small for splitting' with %d windows", count);
    app_dev_feed(app, "C-x 1", &t->arena);
    for (i32 k = 0; k < 10 && !test_app_echo_is(app, "Too many windows"); k++) {
        app_dev_feed(app, "C-x 3", &t->arena);
        if (!test_app_echo_is(app, "Too many windows")) app_dev_feed(app, "C-x +", &t->arena);
    }
    TEST_CHECK(t, app_dev_window_count(app) == WINDOW_MAX && test_app_echo_is(app, "Too many windows"), "window commands: 'Too many windows' at %d",
               app_dev_window_count(app));
    for (i32 i = 0; i < WINDOW_MAX; i++) TEST_CHECK(t, test_win(app, i)->w == 160, "window commands: C-x + makes 8 windows 160 px wide");

    // Sizes.
    app_dev_feed(app, "C-x 1 C-x ^", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "Cannot enlarge selected window"), "window commands: C-x ^ with one window");
    app_dev_feed(app, "C-x 2 C-x ^", &t->arena);
    TEST_CHECK(t, test_win(app, 0)->h == 416 && test_win(app, 1)->h == 368, "window commands: C-x ^ (%d %d)", test_win(app, 0)->h, test_win(app, 1)->h);
    app_dev_feed(app, "C-x }", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "Cannot enlarge selected window"), "window commands: C-x } without a window beside it");
    app_dev_feed(app, "C-x 1 C-x 3 C-x }", &t->arena);
    TEST_CHECK(t, test_win(app, 0)->w == 648 && test_win(app, 1)->w == 632, "window commands: C-x }");
    app_dev_feed(app, "C-x { C-x {", &t->arena);
    TEST_CHECK(t, test_win(app, 0)->w == 632, "window commands: C-x {");
    app_dev_feed(app, "C-x o C-x {", &t->arena); // the right window gives a column to the left one (no window after it)
    TEST_CHECK(t, test_win(app, 1)->w == 640, "window commands: C-x { in the right window (%d)", test_win(app, 1)->w);

    // scroll-other-window.
    app_dev_feed(app, "C-x 1 C-M-v", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "There is no other window"), "window commands: C-M-v with one window");
    app_dev_feed(app, "C-x 3", &t->arena);
    i64 top0 = app_dev_window_top(app, 0), top1 = app_dev_window_top(app, 1);
    app_dev_feed(app, "C-M-v", &t->arena);
    TEST_CHECK(t, app_dev_window_top(app, 1) == top1 + 46 && app_dev_window_top(app, 0) == top0 && app_dev_window_selected(app) == 0,
               "window commands: C-M-v scrolls the other window a screen (%D -> %D)", top1, app_dev_window_top(app, 1));
    app_dev_feed(app, "C-M-S-v", &t->arena);
    TEST_CHECK(t, app_dev_window_top(app, 1) == top1, "window commands: C-M-S-v scrolls it back");

    // other-window ends an isearch like any other command.
    app_dev_feed(app, "C-s l i n e", &t->arena);
    TEST_CHECK(t, app->isearch.active, "window commands: isearch started");
    app_dev_feed(app, "C-x o", &t->arena);
    TEST_CHECK(t, !app->isearch.active && app_dev_window_selected(app) == 1, "window commands: C-x o ends the isearch");

    // In the minibuffer.
    i64 caller_top = app_dev_window_top(app, 1);
    app_dev_feed(app, "M-x C-x 3", &t->arena);
    TEST_CHECK(t, app->mini.active && test_app_echo_is(app, "Attempt to split minibuffer window") && app_dev_window_count(app) == 2,
               "window commands: C-x 3 in the minibuffer");
    app_dev_feed(app, "C-x 0", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "Attempt to delete minibuffer or sole ordinary window") && app_dev_window_count(app) == 2,
               "window commands: C-x 0 in the minibuffer");
    app_dev_feed(app, "C-x 1", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "Can't expand minibuffer to full frame") && app_dev_window_count(app) == 2,
               "window commands: C-x 1 in the minibuffer");
    app_dev_feed(app, "C-x o", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "Cannot select another window from the minibuffer") && app_dev_window_selected(app) == 1,
               "window commands: C-x o in the minibuffer");
    app_dev_feed(app, "C-x 4 0", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "Attempt to delete minibuffer or sole ordinary window") && app_dev_window_count(app) == 2,
               "window commands: C-x 4 0 in the minibuffer");
    app_dev_feed(app, "C-M-v", &t->arena);
    TEST_CHECK(t, app_dev_window_top(app, 1) == caller_top + 46 && app->mini.active,
               "window commands: C-M-v in the minibuffer scrolls the window it was called from");
    app_dev_feed(app, "C-x ^", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "Cannot enlarge selected window"), "window commands: C-x ^ in the minibuffer acts on the caller");
    app_dev_feed(app, "C-g", &t->arena);
    TEST_CHECK(t, !app->mini.active && app_dev_window_selected(app) == 1, "window commands: the minibuffer closed");
    if (!test_app_destroy(t, app, "window commands")) return 0;
    LOG("test: ok: window commands (C-x 3, C-x 2, C-x o in cyclic order, C-x 0 and the most recently used window, C-x 1, the "
        "refusals, C-x +, C-x ^, C-x }, C-x {, C-M-v and C-M-S-v, C-x o ending an isearch, every window command in the minibuffer)");
    return 1;
}

// The pop-up rule in each case, with C-x 4 b, C-x 4 f, C-h e; C-x 4 0.
static b32 test_window_popup(Test *t) {
    String8 dir = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p10windows", t->tmp_dir));
    os_make_dir(dir);
    String8 file = str8_fmt(&t->arena, "%S\\other.txt", dir), anchor = str8_fmt(&t->arena, "%S\\anchor.txt", dir);
    String8 conf = str8_fmt(&t->arena, "%S\\narrow.conf", dir);
    TEST_CHECK(t, os_write_file(file, STR8_LIT("other\n")) && os_write_file(anchor, STR8_LIT("anchor\n")) &&
                  os_write_file(conf, STR8_LIT("[settings]\nsplit_width_threshold = 200\n")), "pop-up: cannot write the files");
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "pop-up: app_create failed");

    // One window, a frame of 160 columns (the threshold): split side by side; the new window is selected.
    app_dev_feed(app, "C-x 4 b * M e s s a g e s * RET", &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 2 && app_dev_window_selected(app) == 1 && test_win(app, 1)->buffer == app->messages &&
                  test_win(app, 1)->x == 640 && test_win(app, 0)->buffer != app->messages,
               "pop-up: one window: split side by side, the buffer in the new window");
    // A window already showing it is reused.
    app_dev_feed(app, "C-x o C-x 4 b * M e s s a g e s * RET", &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 2 && app_dev_window_selected(app) == 1, "pop-up: the window showing it is reused");
    // Otherwise the least recently used other window: top-left (selected), bottom-left, right; the right one was
    // selected after the bottom-left one.
    app_dev_feed(app, "C-x 1 C-x 3 C-x 2 C-x o C-x o C-x o", &t->arena);
    TEST_CHECK(t, app_dev_window_selected(app) == 0, "pop-up: back in the first window");
    app_dev_feed(app, "C-x 4 b p o p RET", &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 3 && app_dev_window_selected(app) == 1 && str8_equal(test_win(app, 1)->buffer->name, STR8_LIT("pop")),
               "pop-up: the least recently used other window (selected %d)", app_dev_window_selected(app));
    // One window in a frame narrower than split_width_threshold: one above the other.
    app_dev_use_config(app, conf);
    app_dev_feed(app, "C-x 1 C-x 4 b * s c r a t c h * RET", &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 2 && test_win(app, 1)->x == 0 && test_win(app, 1)->y > 0 && app_dev_window_selected(app) == 1,
               "pop-up: below 200 columns, one above the other");
    // C-h e: *Messages* in another window, not selected, point at its end.
    app_dev_feed(app, "C-x 1 C-h e", &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 2 && app_dev_window_selected(app) == 0 && test_win(app, 1)->buffer == app->messages &&
                  test_win_point(app, 1) == buffer_size(app->messages), "pop-up: C-h e");
    // C-x 4 f: the file in the other window, selected.
    app_dev_visit(app, anchor);
    app_dev_feed(app, "C-x 4 f", &t->arena);
    TEST_CHECK(t, app->mini.active && str8_equal(app->mini.prompt, STR8_LIT("Find file in other window: ")), "pop-up: C-x 4 f prompt");
    app_dev_feed(app, test_win_keys(t, STR8_LIT("other.txt")), &t->arena);
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 2 && app_dev_window_selected(app) == 1 && str8_equal(test_win(app, 1)->buffer->name, STR8_LIT("other.txt")),
               "pop-up: C-x 4 f opens the file in the other window");
    // C-x 4 0: the buffer and its window go.
    app_dev_feed(app, "C-x 4 0", &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 1 && !buffer_list_find_name(&app->buffers, STR8_LIT("other.txt")),
               "pop-up: C-x 4 0 kills the buffer and deletes its window");
    app_dev_feed(app, "C-x 4 0", &t->arena); // anchor.txt in the only window: killed, the window stays
    TEST_CHECK(t, app_dev_window_count(app) == 1 && !buffer_list_find_name(&app->buffers, STR8_LIT("anchor.txt")) &&
                  test_app_echo_is(app, "Attempt to delete minibuffer or sole ordinary window"), "pop-up: C-x 4 0 in the only window");
    app_dev_feed(app, "C-x 4 b", &t->arena);
    TEST_CHECK(t, app->mini.active && str8_starts_with(app->mini.prompt, STR8_LIT("Switch to buffer in other window (default ")), "pop-up: C-x 4 b prompt");
    app_dev_feed(app, "C-g", &t->arena);
    if (!test_app_destroy(t, app, "pop-up")) return 0;
    LOG("test: ok: pop-up rule (one window: side by side from split_width_threshold columns, else stacked; a window already showing "
        "the buffer; the least recently used other window; C-x 4 b, C-x 4 f, C-h e), C-x 4 0 (also in the only window)");
    return 1;
}

// Two windows on one buffer: independent point and scroll, an edit in one moves the other's markers,
// kill-buffer replaces the buffer in both, each window's remembered position.
static b32 test_window_same_buffer(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "same buffer: app_create failed");
    test_win_lines(t, app, 300);
    app_dev_feed(app, "M-g g 1 0 0 RET C-x 3 C-x o M->", &t->arena);
    i64 p0 = test_win_point(app, 0), top0 = app_dev_window_top(app, 0);
    TEST_CHECK(t, test_win_point(app, 1) == buffer_size(test_win(app, 1)->buffer) && app_dev_window_top(app, 1) > top0 &&
                  buffer_line_of(test_win(app, 0)->buffer, p0) == 99, "same buffer: each window has its own point and scroll");
    app_dev_feed(app, "M-< a b c RET", &t->arena); // in the right window, before the left window's point and top
    TEST_CHECK(t, test_win_point(app, 0) == p0 + 4 && app_dev_window_top(app, 0) == top0 + 1 && test_win_point(app, 1) == 4,
               "same buffer: an edit in one window moves the other's point (%D, want %D) and top", test_win_point(app, 0), p0 + 4);
    // Each window returns to its own place after C-x b back and forth; the other is not disturbed.
    app_dev_feed(app, "C-x o C-x b * M e s s a g e s * RET C-x b * s c r a t c h * RET", &t->arena);
    TEST_CHECK(t, app_dev_window_selected(app) == 0 && test_win_point(app, 0) == p0 + 4 && app_dev_window_top(app, 0) == top0 + 1 &&
                  test_win_point(app, 1) == 4, "same buffer: C-x b back and forth keeps each window's place (%D, want %D)",
               test_win_point(app, 0), p0 + 4);
    // kill-buffer replaces it in both windows.
    app_dev_feed(app, "C-x b t w o RET C-x o C-x b t w o RET", &t->arena);
    TEST_CHECK(t, str8_equal(test_win(app, 0)->buffer->name, STR8_LIT("two")) && test_win(app, 1)->buffer == test_win(app, 0)->buffer,
               "same buffer: both windows on 'two'");
    app_dev_feed(app, "C-x k RET", &t->arena);
    TEST_CHECK(t, !buffer_list_find_name(&app->buffers, STR8_LIT("two")) && test_win(app, 0)->buffer != NULL &&
                  !str8_equal(test_win(app, 0)->buffer->name, STR8_LIT("two")) && !str8_equal(test_win(app, 1)->buffer->name, STR8_LIT("two")),
               "same buffer: kill-buffer replaced it in both windows");
    if (!test_app_destroy(t, app, "same buffer")) return 0;
    LOG("test: ok: two windows on one buffer (own point and scroll, an edit moving the other's markers, C-x b back and forth, "
        "kill-buffer in both)");
    return 1;
}

// Clicks, the wheel, divider and mode-line drags, drag selections, the mouse cursor.
static b32 test_window_mouse(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "mouse: app_create failed");
    test_win_lines(t, app, 300);
    app_dev_feed(app, "C-x 3", &t->arena);
    // A click in the right window selects it and puts point there: row 6, column 7.
    test_win_mouse(t, app, EVENT_MOUSE_DOWN, 640 + 4 + 7 * 8 + 2, 6 * 16 + 5);
    test_win_mouse(t, app, EVENT_MOUSE_UP, 640 + 4 + 7 * 8 + 2, 6 * 16 + 5);
    Buffer *buf = test_win(app, 1)->buffer;
    TEST_CHECK(t, app_dev_window_selected(app) == 1 && test_win_point(app, 1) == buffer_line_start(buf, 6) + 6, // "line 6" has 6 columns
               "mouse: a click selects the window and sets point (%D)", test_win_point(app, 1));
    // The wheel over the left window scrolls it, not the selected one.
    Event wheel = { .kind = EVENT_MOUSE_WHEEL, .x = 100, .y = 100, .wheel = -120 };
    app_dev_feed_events(app, &wheel, 1, &t->arena);
    TEST_CHECK(t, app_dev_window_top(app, 0) == 3 && app_dev_window_top(app, 1) == 0 && app_dev_window_selected(app) == 1,
               "mouse: the wheel over a window that is not selected scrolls it");
    // The cursor: the divider (the left window's last pixel, 639), text, the bottom mode line.
    TEST_CHECK(t, app_mouse_cursor(app, 639, 300) == MOUSE_CURSOR_RESIZE_WE && app_mouse_cursor(app, 641, 300) == MOUSE_CURSOR_RESIZE_WE &&
                  app_mouse_cursor(app, 300, 300) == MOUSE_CURSOR_ARROW && app_mouse_cursor(app, 300, 775) == MOUSE_CURSOR_ARROW,
               "mouse: the cursor over the divider, text and the bottom mode line");
    // Dragging the divider: only the two windows change; snapped to cells.
    test_win_mouse(t, app, EVENT_MOUSE_DOWN, 639, 300);
    test_win_mouse(t, app, EVENT_MOUSE_MOVE, 719, 300);
    test_win_mouse(t, app, EVENT_MOUSE_UP, 719, 300);
    TEST_CHECK(t, test_win(app, 0)->w == 720 && test_win(app, 1)->x == 720 && test_win_point(app, 1) == buffer_line_start(buf, 6) + 6,
               "mouse: dragging the divider (%d)", test_win(app, 0)->w);
    // A mode line with a window below it: the vertical resize cursor; dragging it moves that line only.
    app_dev_feed(app, "C-x o C-x 2", &t->arena); // the left window: top-left 0, bottom-left 1, right 2
    TEST_CHECK(t, app_mouse_cursor(app, 100, 390) == MOUSE_CURSOR_RESIZE_NS && app_mouse_cursor(app, 100, 775) == MOUSE_CURSOR_ARROW,
               "mouse: the cursor over a mode line with a window below it, and over the bottom one");
    app_dev_feed(app, "C-x o", &t->arena);
    test_win_mouse(t, app, EVENT_MOUSE_DOWN, 100, 390);
    test_win_mouse(t, app, EVENT_MOUSE_MOVE, 100, 330);
    test_win_mouse(t, app, EVENT_MOUSE_UP, 100, 330);
    TEST_CHECK(t, test_win(app, 0)->h == 336 && test_win(app, 1)->y == 336 && test_win(app, 2)->h == 784 && app_dev_window_selected(app) == 0,
               "mouse: dragging a mode line (%d), its window selected", test_win(app, 0)->h);
    // A drag selection stays in the window it started in: over the right window, the column is the last visible one.
    i64 right_point = test_win_point(app, 2);
    test_win_mouse(t, app, EVENT_MOUSE_DOWN, 4 + 2, 5);
    test_win_mouse(t, app, EVENT_MOUSE_MOVE, 900, 5);
    test_win_mouse(t, app, EVENT_MOUSE_UP, 900, 5);
    View *left = test_win(app, 0);
    TEST_CHECK(t, app_dev_window_selected(app) == 0 && left->cursors[0].mark_active && test_win_point(app, 2) == right_point &&
                  test_win_point(app, 0) == buffer_line_end(left->buffer, view_top_line(left)),
               "mouse: a drag selection stays in its window");
    // While the minibuffer reads, no edge is draggable and presses outside its line do nothing.
    app_dev_feed(app, "M-x", &t->arena);
    TEST_CHECK(t, app_mouse_cursor(app, 719, 600) == MOUSE_CURSOR_ARROW, "mouse: no resize cursor while the minibuffer reads");
    test_win_mouse(t, app, EVENT_MOUSE_DOWN, 719, 600);
    test_win_mouse(t, app, EVENT_MOUSE_MOVE, 500, 600);
    test_win_mouse(t, app, EVENT_MOUSE_UP, 500, 600);
    TEST_CHECK(t, test_win(app, 0)->w == 720 && app->mini.active, "mouse: the divider stays while the minibuffer reads");
    app_dev_feed(app, "C-g", &t->arena);
    if (!test_app_destroy(t, app, "mouse")) return 0;
    LOG("test: ok: mouse in windows (a click selects and sets point, the wheel over another window, divider and mode-line drags "
        "and their cursors, a drag selection kept in its window, nothing to drag while the minibuffer reads)");
    return 1;
}

// The minibuffer and isearch from the second of three windows: that window stays selected.
static b32 test_window_prompts(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "prompts: app_create failed");
    test_win_lines(t, app, 300);
    app_dev_feed(app, "C-x 3 C-x 2 C-x o", &t->arena);
    TEST_CHECK(t, app_dev_window_selected(app) == 1, "prompts: the second window");
    app_dev_feed(app, "M-x g o t o - l i n e RET 5 0 RET", &t->arena);
    TEST_CHECK(t, app_dev_window_selected(app) == 1 && buffer_line_of(test_win(app, 1)->buffer, test_win_point(app, 1)) == 49 &&
                  test_win_point(app, 0) == 0 && test_win_point(app, 2) == 0, "prompts: M-x goto-line runs in the second window and keeps it");
    app_dev_feed(app, "M-x g o C-g", &t->arena);
    TEST_CHECK(t, !app->mini.active && app_dev_window_selected(app) == 1, "prompts: C-g in the minibuffer keeps the second window");
    i64 p = test_win_point(app, 1);
    app_dev_feed(app, "C-s l i n e SPC 7 0 RET", &t->arena);
    TEST_CHECK(t, !app->isearch.active && app_dev_window_selected(app) == 1 && buffer_line_of(test_win(app, 1)->buffer, test_win_point(app, 1)) == 70,
               "prompts: an isearch ended with RET in the second window");
    app_dev_feed(app, "C-s l i n e SPC 9 9 C-g", &t->arena);
    TEST_CHECK(t, !app->isearch.active && app_dev_window_selected(app) == 1 && buffer_line_of(test_win(app, 1)->buffer, test_win_point(app, 1)) == 70,
               "prompts: an isearch quit with C-g goes back to where it started");
    (void)p;
    if (!test_app_destroy(t, app, "prompts")) return 0;
    LOG("test: ok: the minibuffer and isearch from the second of three windows (RET, C-g): that window stays selected");
    return 1;
}

// A frame too small for its windows: nothing negative, rows and columns at least 1, commands still run,
// and the windows come back as they were.
static b32 test_window_tiny(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "tiny: app_create failed");
    test_win_lines(t, app, 300);
    for (i32 k = 0; k < 10 && app_dev_window_count(app) < WINDOW_MAX; k++) app_dev_feed(app, "C-x 3 C-x +", &t->arena);
    app_dev_feed(app, "C-x o C-x o", &t->arena); // window 2 selected
    i32 rects[WINDOW_MAX][4];
    i64 tops[WINDOW_MAX], points[WINDOW_MAX];
    for (i32 i = 0; i < WINDOW_MAX; i++) {
        View *v = test_win(app, i);
        rects[i][0] = v->x, rects[i][1] = v->y, rects[i][2] = v->w, rects[i][3] = v->h;
        tops[i] = view_top_line(v);
        points[i] = test_win_point(app, i);
    }
    app_dev_frame_size(app, 40, 30);
    app_dev_feed(app, "", &t->arena);
    app_dev_feed(app, "x y z C-v M-v C-v", &t->arena);
    Event wheel = { .kind = EVENT_MOUSE_WHEEL, .x = 1, .y = 1, .wheel = -120 };
    app_dev_feed_events(app, &wheel, 1, &t->arena);
    i32 wheeled = window_at(&app->windows, 1, 1);
    app_dev_feed(app, "C-x 2", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "Too many windows"), "tiny: C-x 2 with 8 windows");
    for (i32 k = 0; k < WINDOW_MAX; k++) app_dev_feed(app, "C-x o", &t->arena);
    app_dev_feed(app, "C-s x RET M-x C-g", &t->arena);
    test_win_mouse(t, app, EVENT_MOUSE_DOWN, 5, 5);
    test_win_mouse(t, app, EVENT_MOUSE_UP, 5, 5);
    for (i32 i = 0; i < WINDOW_MAX; i++) {
        View *v = test_win(app, i);
        TEST_CHECK(t, v->w >= 0 && v->h >= 0 && v->rows >= 1 && v->cols >= 1, "tiny: window %d is %dx%d, %d rows, %d columns", i, v->w, v->h,
                   v->rows, v->cols);
    }
    MouseCursor c = app_mouse_cursor(app, 3, 3);
    TEST_CHECK(t, c >= MOUSE_CURSOR_ARROW && c < MOUSE_CURSOR_COUNT, "tiny: the mouse cursor");
    // Three windows in the tiny frame: C-x 2 is too small now.
    app_dev_feed(app, "C-x 1 C-x 3 C-x 3", &t->arena);
    i32 three = app_dev_window_count(app);
    app_dev_feed(app, "C-x 2", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "Window too small for splitting") && app_dev_window_count(app) == three,
               "tiny: 'Window too small for splitting' (%d windows)", three);
    app_dev_frame_size(app, 1280, 800);
    app_dev_feed(app, "", &t->arena);
    TEST_CHECK(t, three == 1 || app_dev_window_count(app) == three, "tiny: the windows are still there");
    if (!test_app_destroy(t, app, "tiny")) return 0;

    // The same eight windows, shrunk and grown back without commands in between: identical rects, scroll
    // positions and points.
    app = test_app_create(t);
    test_win_lines(t, app, 300);
    for (i32 k = 0; k < 10 && app_dev_window_count(app) < WINDOW_MAX; k++) app_dev_feed(app, "C-x 3 C-x +", &t->arena);
    for (i32 i = 0; i < WINDOW_MAX; i++) {
        app_dev_feed(app, "C-x o M-g g 1 5 0 RET", &t->arena); // every window somewhere else
        View *v = test_win(app, i);
        (void)v;
    }
    for (i32 i = 0; i < WINDOW_MAX; i++) {
        View *v = test_win(app, i);
        rects[i][0] = v->x, rects[i][1] = v->y, rects[i][2] = v->w, rects[i][3] = v->h;
        tops[i] = view_top_line(v);
        points[i] = test_win_point(app, i);
    }
    static const i32 sizes[][2] = { { 40, 30 }, { 1, 0 }, { 8, 16 }, { 600, 100 }, { 1279, 799 } };
    for (i32 s = 0; s < ARRAY_COUNT(sizes); s++) {
        app_dev_frame_size(app, sizes[s][0], sizes[s][1]);
        app_dev_feed(app, "", &t->arena);
        app_dev_frame_size(app, 1280, 800);
        app_dev_feed(app, "", &t->arena);
        for (i32 i = 0; i < WINDOW_MAX; i++) {
            View *v = test_win(app, i);
            TEST_CHECK(t, v->x == rects[i][0] && v->y == rects[i][1] && v->w == rects[i][2] && v->h == rects[i][3] &&
                          view_top_line(v) == tops[i] && test_win_point(app, i) == points[i],
                       "tiny: after %dx%d, window %d is back as it was (top %D, want %D)", sizes[s][0], sizes[s][1], i, view_top_line(v), tops[i]);
        }
    }
    (void)wheeled;
    if (!test_app_destroy(t, app, "tiny")) return 0;
    LOG("test: ok: a frame too small for its windows (8 windows in 40x30: typing, C-v, M-v, the wheel, C-x 2, C-x o, isearch, M-x, a "
        "click; nothing negative, rows and columns >= 1; 'Window too small for splitting'; frames of 40x30, 1x0, 8x16, 600x100, "
        "1279x799 and back: identical rects, scroll positions and points)");
    return 1;
}

// startup_windows = 2 with two files on the command line.
static b32 test_window_startup(Test *t) {
    String8 dir = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p10windows", t->tmp_dir));
    os_make_dir(dir);
    String8 a = str8_fmt(&t->arena, "%S\\first.txt", dir), b = str8_fmt(&t->arena, "%S\\second.txt", dir);
    String8 conf = str8_fmt(&t->arena, "%S\\two.conf", dir);
    u8 *text = PUSH_ARRAY(&t->arena, u8, 100 * 16);
    i64 n = 0;
    for (i32 i = 0; i < 100; i++) n += fmt_buf(text + n, 16, "row %d\n", i);
    TEST_CHECK(t, os_write_file(a, str8(text, n)) && os_write_file(b, str8(text, n)) && os_write_file(conf, STR8_LIT("[settings]\nstartup_windows = 2\n")),
               "startup: cannot write the files");
    AppFileArg files[2] = { { a, 10, 3 }, { b, 50, 0 } };
    AppArgs args = { .dpi_scale = 1.0f, .headless = 1, .files = files, .file_count = 2, .config_path = conf };
    App *app = app_create(&t->arena, &args);
    TEST_CHECK(t, app, "startup: app_create failed");
    app_dev_feed_events(app, NULL, 0, &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 2 && app_dev_window_selected(app) == 0 &&
                  str8_equal(test_win(app, 0)->buffer->name, STR8_LIT("first.txt")) && str8_equal(test_win(app, 1)->buffer->name, STR8_LIT("second.txt")) &&
                  test_win(app, 1)->x == 640, "startup: startup_windows = 2 shows the two files side by side");
    Buffer *fa = test_win(app, 0)->buffer, *fb = test_win(app, 1)->buffer;
    TEST_CHECK(t, test_win_point(app, 0) == buffer_line_start(fa, 9) + 2 && test_win_point(app, 1) == buffer_line_start(fb, 49),
               "startup: each file at its own +LINE:COLUMN");
    if (!test_app_destroy(t, app, "startup")) return 0;
    files[1].path = str8(NULL, 0);
    args.file_count = 1;
    app = app_create(&t->arena, &args);
    app_dev_feed_events(app, NULL, 0, &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 2 && test_win(app, 0)->buffer == test_win(app, 1)->buffer &&
                  test_win_point(app, 1) == test_win_point(app, 0), "startup: one file: both windows show it");
    if (!test_app_destroy(t, app, "startup")) return 0;
    LOG("test: ok: startup_windows = 2 (two files side by side, each at its +LINE:COLUMN; one file in both windows)");
    return 1;
}

i32 test_run(u64 seed, String8 tmp_dir) {
    Test t = { 0 };
    t.arena = arena_create(GB(4));
    t.tmp_dir = tmp_dir;
    LOG("test: seed 0x%X (override with --seed), files in %S", seed, tmp_dir);
    u64 t0 = os_time_us();
    syntax_init();

    test_fuzz(&t, seed);
    arena_reset(&t.arena);
    test_markers(&t, seed);
    arena_reset(&t.arena);
    test_line_states(&t, seed);
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
    test_window_tree(&t, seed);
    arena_reset(&t.arena);
    test_undo_buffer(&t, seed);
    arena_reset(&t.arena);
    test_undo_commands(&t, seed);
    arena_reset(&t.arena);
    test_region(&t);
    arena_reset(&t.arena);
    test_clipboard(&t);
    arena_reset(&t.arena);
    test_kill(&t);
    arena_reset(&t.arena);
    test_syntax(&t, seed);
    arena_reset(&t.arena);
    test_match(&t);
    arena_reset(&t.arena);
    test_show_paren(&t);
    arena_reset(&t.arena);
    test_set_language(&t);
    arena_reset(&t.arena);
    test_indent(&t);
    arena_reset(&t.arena);
    test_electric_labels(&t);
    arena_reset(&t.arena);
    test_search(&t, seed);
    arena_reset(&t.arena);
    test_edit_commands(&t);
    arena_reset(&t.arena);
    test_undo_fuzz(&t, seed);
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
    test_config_minibuffer(&t);
    arena_reset(&t.arena);
    test_buffer_list(&t);
    arena_reset(&t.arena);
    test_hot_reload(&t);
    arena_reset(&t.arena);
    test_headless_app(&t);
    arena_reset(&t.arena);
    test_isearch(&t);
    arena_reset(&t.arena);
    test_replace(&t);
    arena_reset(&t.arena);
    test_window_commands(&t);
    arena_reset(&t.arena);
    test_window_popup(&t);
    arena_reset(&t.arena);
    test_window_same_buffer(&t);
    arena_reset(&t.arena);
    test_window_mouse(&t);
    arena_reset(&t.arena);
    test_window_prompts(&t);
    arena_reset(&t.arena);
    test_window_tiny(&t);
    arena_reset(&t.arena);
    test_window_startup(&t);
    arena_reset(&t.arena);
    test_list_dir(&t);
    arena_reset(&t.arena);
    test_matcher(&t);
    arena_reset(&t.arena);
    test_minibuffer(&t);
    arena_reset(&t.arena);
    test_completion(&t);
    arena_reset(&t.arena);
    test_buffers(&t);
    arena_reset(&t.arena);
    test_find_write(&t);
    arena_reset(&t.arena);
    test_goto_line(&t);
    arena_reset(&t.arena);
    test_save_some(&t);
    arena_reset(&t.arena);
    test_revert(&t);
    arena_reset(&t.arena);
    test_disk(&t);
    arena_reset(&t.arena);
    test_revert_highlight(&t);
    arena_reset(&t.arena);
    test_end_session(&t);
    arena_reset(&t.arena);
    test_manual_files(&t);
    arena_reset(&t.arena);
    test_manual_editing(&t);
    arena_reset(&t.arena);
    test_manual_syntax(&t);
    arena_reset(&t.arena);
    test_manual_minibuffer(&t);
    arena_reset(&t.arena);
    test_manual_config(&t);
    arena_reset(&t.arena);
    test_manual_disk_search(&t);
    arena_reset(&t.arena);

    LOG("test: %s, %d failure(s), %U ms", t.failures ? "FAIL" : "PASS", t.failures, (os_time_us() - t0) / 1000);
    os_release(t.arena.base);
    return t.failures;
}

#endif // TEAL_DEV
