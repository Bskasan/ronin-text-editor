// test.c — dev-only tests (--test). Headless: buffers and files only, no window, no GPU.
// Results go to build\ronin.log; test_run returns the number of failures.

#if RONIN_DEV

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

// Valid UTF-8 only (whole characters), at least len bytes.
static String8 test_valid_text(Test *t, u8 *out, i64 len) {
    static const char *pieces[] = { "a", "b", "x", "Z", "0", " ", "\t", "\n", "\n", "\xC4\x9F", "\xE2\x82\xAC", "\xF0\x9F\x98\x80" };
    i64 n = 0;
    while (n < len) {
        const char *p = pieces[test_below(t, ARRAY_COUNT(pieces))];
        while (*p) out[n++] = (u8)*p++;
    }
    return str8(out, n);
}

#define TEST_BATCH_ROUNDS 600
#define TEST_BATCH_MARKERS 60

// Batches against the sequential path: two buffers with the same text and markers get the same edits, B inside a
// batch. Runs that are ascending, descending or mixed (flushes); markers set, created, destroyed and read in the
// middle; the swallowed flag against a naive one (strictly inside a removed range since the marker was last set).
// Valid UTF-8 with edits on boundaries: every marker equal. Invalid bytes: the same text, every marker of B on a
// boundary (snapped against the run's final text, not after each edit).
static b32 test_marker_batch(Test *t, u64 seed) {
    t->rng = seed ^ 0xba7c4ull;
    Buffer *a = buffer_create(STR8_LIT("seq")), *b = buffer_create(STR8_LIT("batch"));
    TEST_CHECK(t, a && b, "marker batch: buffer_create failed");
    buffer_undo_enable(a, 0);
    buffer_undo_enable(b, 0);
    u8 *text = PUSH_ARRAY(&t->arena, u8, KB(16));
    BufferMarker ma[TEST_BATCH_MARKERS], mb[TEST_BATCH_MARKERS];
    b32 sw[TEST_BATCH_MARKERS];
    i64 edits = 0, gets = 0, swallowed = 0, flushes_mixed = 0, joins = 0;
    for (i32 round = 0; round < TEST_BATCH_ROUNDS; round++) {
        b32 valid = round % 3 != 2;
        i32 kind = round % 5; // 0 ascending, 1 descending, 2 mixed, 3 ascending runs with deletes at cursors, 4 typing with reindents
        buffer_replace(a, 0, buffer_size(a), STR8_LIT(""));
        buffer_replace(b, 0, buffer_size(b), STR8_LIT(""));
        String8 init = valid ? test_valid_text(t, text, 200 + test_below(t, 1500)) : test_random_text(t, text, 200 + test_below(t, 1500));
        buffer_replace(a, 0, 0, init);
        buffer_replace(b, 0, 0, init);
        for (i32 k = 0; k < TEST_BATCH_MARKERS; k++) {
            b32 adv = (b32)test_below(t, 2);
            i64 pos = test_below(t, buffer_size(a) + 1);
            ma[k] = buffer_marker_create(a, pos, adv);
            mb[k] = buffer_marker_create(b, pos, adv);
            sw[k] = 0;
        }
        u64 flushes0 = b->dev_batch_flushes, joins0 = b->dev_batch_joins;
        buffer_batch_begin(b);
        i64 next = kind == 1 ? buffer_size(a) : buffer_snap_char(a, test_below(t, 64)); // ascending: the next edit starts here or after; descending: ends here or before
        i32 ops = 1 + (i32)test_below(t, 60);
        for (i32 op = 0; op < ops; op++) {
            i64 r = test_below(t, 100), size = buffer_size(a);
            if (kind == 4 && r < 75) {
                // An insertion at or after `next`, then perhaps a replacement that ends where it inserted, from at or after
                // `next` (RET emptying a whitespace-only line, a closing bracket reindenting its line): joined, no flush.
                i64 floor = next, step = test_below(t, 40), p = MIN(next + step, size);
                if (valid) p = MAX(buffer_snap_char(a, p), floor);
                u8 ins[64], rep_text[64];
                String8 x = valid ? test_valid_text(t, ins, 1 + test_below(t, 3)) : test_random_text(t, ins, 1 + test_below(t, 3));
                buffer_replace(a, p, p, x);
                buffer_replace(b, p, p, x);
                edits++;
                next = p + x.len;
                if (test_below(t, 2)) {
                    i64 back = test_below(t, p - floor + 1), from = p - back;
                    if (valid) {
                        from = buffer_snap_char(a, from);
                        if (from < floor) from = p;
                    }
                    i64 yl = test_below(t, 5);
                    String8 y = valid ? test_valid_text(t, rep_text, yl) : test_random_text(t, rep_text, yl);
                    if (!yl) y.len = 0;
                    for (i32 k = 0; k < TEST_BATCH_MARKERS; k++) {
                        i64 pk = buffer_marker_get(a, ma[k]);
                        if (from < pk && pk < p) sw[k] = 1;
                    }
                    buffer_replace(a, from, p, y);
                    buffer_replace(b, from, p, y);
                    edits++;
                    next = from + y.len + x.len;
                }
            } else if (r < 75) {
                i64 start, end, step = test_below(t, 40), len = test_below(t, 3) ? test_below(t, 6) : 0; // (MIN evaluates twice)
                if (kind == 0 || kind == 3) {
                    start = MIN(next + step, size);
                    end = MIN(start + len, size);
                } else if (kind == 1) {
                    end = MAX(next - step, 0);
                    start = MAX(end - len, 0);
                } else {
                    start = test_below(t, size + 1);
                    end = MIN(start + len, size);
                }
                if (valid) { // on boundaries; an ascending run keeps its order (next is a boundary)
                    start = buffer_snap_char(a, start);
                    if ((kind == 0 || kind == 3) && start < next) start = next;
                    end = MAX(buffer_snap_char(a, end), start);
                }
                if (kind == 1 && end > next) end = start; // snapping must not leave the descending order
                u8 ins[64];
                i64 il = test_below(t, 4) ? test_below(t, 12) : 0;
                if (kind == 3) il = test_below(t, 2); // typing and deleting at many places
                String8 s = valid ? test_valid_text(t, ins, il) : test_random_text(t, ins, il);
                if (!il) s.len = 0;
                for (i32 k = 0; k < TEST_BATCH_MARKERS; k++) {
                    i64 p = buffer_marker_get(a, ma[k]);
                    if (start < p && p < end) sw[k] = 1;
                }
                buffer_replace(a, start, end, s);
                buffer_replace(b, start, end, s);
                edits++;
                if (kind == 1) next = start;
                else next = start + s.len;
            } else if (r < 82) {
                i32 k = (i32)test_below(t, TEST_BATCH_MARKERS);
                i64 pos = test_below(t, size + 1);
                buffer_marker_set(a, ma[k], pos);
                buffer_marker_set(b, mb[k], pos);
                sw[k] = 0;
            } else if (r < 86) {
                i32 k = (i32)test_below(t, TEST_BATCH_MARKERS);
                b32 adv = (b32)test_below(t, 2);
                i64 pos = test_below(t, size + 1);
                buffer_marker_destroy(a, ma[k]);
                buffer_marker_destroy(b, mb[k]);
                ma[k] = buffer_marker_create(a, pos, adv);
                mb[k] = buffer_marker_create(b, pos, adv);
                sw[k] = 0;
            } else {
                i32 k = (i32)test_below(t, TEST_BATCH_MARKERS);
                i64 pa = buffer_marker_get(a, ma[k]), pb = buffer_marker_get(b, mb[k]);
                TEST_CHECK(t, !valid || pa == pb, "marker batch round %d op %d: marker %d at %D, sequential %D (seed 0x%X)", round, op, k,
                           pb, pa, seed);
                TEST_CHECK(t, !valid || buffer_marker_swallowed(b, mb[k]) == sw[k], "marker batch round %d op %d: marker %d swallowed %d, "
                           "naive %d (seed 0x%X)", round, op, k, buffer_marker_swallowed(b, mb[k]), sw[k], seed);
                gets++;
            }
        }
        for (i32 k = 0; k < TEST_BATCH_MARKERS; k++) {
            TEST_CHECK(t, !valid || buffer_marker_swallowed(b, mb[k]) == sw[k], "marker batch round %d: marker %d swallowed %d, naive %d (seed 0x%X)",
                       round, k, buffer_marker_swallowed(b, mb[k]), sw[k], seed);
            swallowed += sw[k];
        }
        buffer_batch_end(b);
        u64 flushes = b->dev_batch_flushes - flushes0;
        if (kind == 4) joins += (i64)(b->dev_batch_joins - joins0);
        if (kind == 2) flushes_mixed += (i64)flushes;
        else TEST_CHECK(t, flushes == 0, "marker batch round %d: %U flushes in an ordered run (seed 0x%X)", round, flushes, seed);
        String8 ta = buffer_text(a, &t->arena, 0, buffer_size(a)), tb = buffer_text(b, &t->arena, 0, buffer_size(b));
        TEST_CHECK(t, str8_equal(ta, tb), "marker batch round %d: the texts differ (seed 0x%X)", round, seed);
        for (i32 k = 0; k < TEST_BATCH_MARKERS; k++) {
            i64 pa = buffer_marker_get(a, ma[k]), pb = buffer_marker_get(b, mb[k]);
            TEST_CHECK(t, valid ? pa == pb : buffer_snap_char(b, pb) == pb, "marker batch round %d: marker %d at %D, sequential %D (%s, "
                       "seed 0x%X)", round, k, pb, pa, valid ? "valid" : "invalid bytes", seed);
            TEST_CHECK(t, !buffer_marker_swallowed(b, mb[k]) && !(b->markers[mb[k] - 1].flags & BUFFER_MARKER_SWALLOWED),
                       "marker batch round %d: marker %d still swallowed after the batch", round, k);
        }
        for (i32 k = 0; k < TEST_BATCH_MARKERS; k++) {
            buffer_marker_destroy(a, ma[k]);
            buffer_marker_destroy(b, mb[k]);
        }
    }
    TEST_CHECK(t, flushes_mixed > 0 && joins > 0 && b->marker_live == 0 && !b->batch_depth, "marker batch: %D flushes in mixed runs, "
               "%D joins, %D live", flushes_mixed, joins, b->marker_live);
    LOG("test: ok: marker batches against the sequential path (%d rounds, %d markers: ascending, descending, mixed and typing runs, "
        "typing with reindents (%D joined edits), valid UTF-8 and invalid bytes; %D edits, %D reads in the middle, %D swallowed, %D "
        "flushes in mixed runs, none in ordered ones; seed 0x%X)", TEST_BATCH_ROUNDS, TEST_BATCH_MARKERS, joins, edits, gets, swallowed,
        flushes_mixed, seed);
    buffer_destroy(a);
    buffer_destroy(b);
    return 1;
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
        } else if (!test_file_absent(str8_fmt(&t->arena, "%S.ronin~1", path))) {
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

// buffer_write_to (saving and the crash recovery) never moves the gap: with the gap at every byte offset of a text
// with 2-, 3- and 4-byte characters, a WTF-8 surrogate, invalid bytes and newlines (inside sequences too), each
// encoding and line-ending mode writes exactly the bytes it writes with the gap at the end.
static b32 test_write_gap(Test *t) {
    static const u8 text[] = "a\n\xc5\x9f\xe2\x82\xac\n\xf0\x9f\x98\x80x\xed\xa0\x80y\x80z\xc3\n\xe2\x82\nend";
    i64 len = (i64)sizeof(text) - 1;
    static const struct { BufferEncoding enc; BufferEol eol; } modes[] = {
        { BUFFER_UTF16LE, BUFFER_EOL_CRLF }, { BUFFER_UTF16BE, BUFFER_EOL_LF }, { BUFFER_UTF16LE, BUFFER_EOL_LF },
        { BUFFER_UTF8_BOM, BUFFER_EOL_CRLF }, { BUFFER_UTF8, BUFFER_EOL_MIXED },
    };
    String8 path = str8_fmt(&t->arena, "%S\\write_gap.bin", t->tmp_dir);
    u8 chunk[64]; // small: the chunk boundaries fall everywhere too
    i32 checked = 0;
    for (i32 m = 0; m < (i32)ARRAY_COUNT(modes); m++) {
        Buffer *buf = buffer_create(STR8_LIT("write_gap"));
        TEST_CHECK(t, buf, "write gap: buffer_create failed");
        buffer_replace(buf, 0, 0, str8((u8 *)text, len));
        buf->encoding = modes[m].enc;
        buf->eol = modes[m].eol;
        String8 want = { 0 };
        for (i64 gap = len; gap >= 0; gap--) {
            buffer_move_gap(buf, gap);
            OsFile f;
            TEST_CHECK(t, os_file_open_overwrite(path, &f) == OS_FILE_OK, "write gap: cannot open %S", path);
            OsFileStatus st = buffer_write_to(buf, f, chunk, sizeof(chunk));
            os_file_close(f);
            TEST_CHECK(t, st == OS_FILE_OK && buf->gap_start == gap, "write gap: mode %d, gap %D: status %d, the gap moved to %D", m, gap,
                       st, buf->gap_start);
            String8 got;
            TEST_CHECK(t, test_read_file(t, path, &got), "write gap: cannot read %S", path);
            if (gap == len) want = got; // the gap at the end: the contiguous text
            TEST_CHECK(t, str8_equal(got, want), "write gap: mode %d, the gap at %D: %D bytes, %D with the gap at the end", m, gap, got.len,
                       want.len);
            checked++;
        }
        buffer_destroy(buf);
    }
    os_file_delete(path);
    LOG("test: ok: write with the gap anywhere (%d writes: UTF-16 LE / BE, UTF-8 with BOM, CRLF, LF and mixed; the gap inside "
        "sequences, chunks of 64 bytes)", checked);
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
    TEST_CHECK(t, test_file_absent(str8_fmt(&t->arena, "%S.ronin~1", path)), "save failure: a temp file was left behind");

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
                                        .indent_width = 4, .max_cursors = 10000 };

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
static const Command TEST_CMD_EACH = { "test-each", "Add one to the goal column of each cursor", test_bump_cursor, 0 };
static const Command TEST_CMD_ONCE = { "test-once", "Add one to the goal column of the primary cursor", test_bump_cursor, COMMAND_ONCE };

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

// ---------------------------------------------------------------------------
// Multiple cursors (Phase 14)

// The cursors as text, in array order: "*" before the primary, the point, ":mark" when the region is active.
static char *test_mc_state(Test *t, View *v) {
    String8 s = STR8_LIT("");
    for (i32 i = 0; i < v->cursor_count; i++) {
        Cursor *c = &v->cursors[i];
        String8 one = str8_fmt(&t->arena, "%s%D", i == v->primary ? "*" : "", view_point(v, c));
        if (c->mark_set && c->mark_active) one = str8_fmt(&t->arena, "%S:%D", one, buffer_marker_get(v->buffer, c->mark));
        s = i ? str8_fmt(&t->arena, "%S %S", s, one) : one;
    }
    char *out = PUSH_ARRAY(&t->arena, char, s.len + 1);
    memcpy(out, s.data, (size_t)s.len);
    out[s.len] = 0;
    return out;
}

// The cursors of a view replaced by these, in this order (not normalized): points, marks (-1: no active region),
// the primary's index.
static void test_mc_set(View *v, const i64 *points, const i64 *marks, i32 count, i32 primary) {
    view_collapse_cursors(v);
    view_set_point(v, &v->cursors[0], points[0]);
    for (i32 i = 1; i < count; i++) view_add_cursor(v, points[i]);
    for (i32 i = 0; i < count; i++) {
        Cursor *c = &v->cursors[i];
        c->mark_active = c->mark_set = c->mark_shift = 0;
        if (marks && marks[i] >= 0) view_set_mark(v, c, marks[i], 1);
    }
    v->primary = primary;
}

static b32 test_echo_is(TestView *tv, const char *expected);

// The driver's rules: cursors swallowed by an earlier cursor's edit, line commands with two cursors on one line,
// the merge rule (normalization), the single-cursor commands of the view, keyboard-quit's two steps, max_cursors.
static b32 test_mc_driver(Test *t) {
    TestView tv;
    // Swallowed: a cursor strictly inside text an earlier cursor's edit removed is skipped; at either end it runs.
    struct { const char *before; const Command *cmd; const char *after; } swallow[] = {
        { "ab|cd|ef\nline2", &CMD_KILL_LINE, "ab|\nline2" },     // the second is skipped: the line below is not joined
        { "a|b|cd", &CMD_DELETE_CHAR, "a|d" },                    // adjacent: both delete
        { "ab|cd|\nxy", &CMD_KILL_LINE, "ab|xy" },                // at the end of the killed text: it kills the newline
        { "foo| b|ar baz", &CMD_KILL_WORD, "foo| baz" },          // inside the killed word
        { "foo| bar| baz", &CMD_KILL_WORD, "foo|" },              // at its end: it runs
    };
    for (i32 i = 0; i < ARRAY_COUNT(swallow); i++) {
        if (!test_view_open(t, &tv, swallow[i].before, 10, 40)) return 0;
        test_view_run(&tv, swallow[i].cmd);
        char *got = test_view_marked(t, &tv);
        TEST_CHECK(t, test_cstr_equal(got, swallow[i].after), "cursors swallowed %d (%s): got \"%s\", expected \"%s\"", i,
                   swallow[i].cmd->name, got, swallow[i].after);
        if (!test_view_close(t, &tv)) return 0;
    }
    // DEL with an active region: a bare cursor inside it (not merged yet) is swallowed, not deleting a character.
    if (!test_view_open(t, &tv, "abcdef", 10, 40)) return 0;
    {
        i64 points[] = { 5, 3 }, marks[] = { 1, -1 };
        test_mc_set(tv.view, points, marks, 2, 0);
        test_view_run(&tv, &CMD_DELETE_BACKWARD_CHAR);
        char *got = test_view_marked(t, &tv);
        TEST_CHECK(t, test_cstr_equal(got, "a|f") && tv.view->cursor_count == 1, "cursors swallowed: DEL over a region gave \"%s\"", got);
    }
    if (!test_view_close(t, &tv)) return 0;

    // Line commands: of several cursors on one line only the first runs it.
    struct { const char *before; const Command *cmd; const char *after; } lines[] = {
        { "a|b|c\nd", &CMD_COMMENT_LINE, "// ab|c\n|d" },               // toggled once (twice would undo it)
        { "x\n        a|b|c\ny", &CMD_UNINDENT, "x\n    a|b|c\ny" },     // one level, not two
        { "x\na\nb|c|d", &CMD_DELETE_INDENTATION, NULL },                // joined once: "x\na bcd"
        { "a|b|c\nd\ne", &CMD_KILL_WHOLE_LINE, "|d\ne" },                // one line killed
        { "  a|b|c\nd", &CMD_INDENT_FOR_TAB_COMMAND, NULL },             // reindented once
    };
    for (i32 i = 0; i < ARRAY_COUNT(lines); i++) {
        if (!test_view_open(t, &tv, lines[i].before, 10, 40)) return 0;
        test_view_run(&tv, lines[i].cmd);
        char *got = test_view_marked(t, &tv);
        if (lines[i].after) {
            TEST_CHECK(t, test_cstr_equal(got, lines[i].after), "cursors on one line %d (%s): got \"%s\", expected \"%s\"", i,
                       lines[i].cmd->name, got, lines[i].after);
        } else if (lines[i].cmd == &CMD_DELETE_INDENTATION) {
            String8 text = buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf));
            TEST_CHECK(t, str8_equal(text, STR8_LIT("x\na bcd")), "cursors on one line: delete-indentation gave '%S'", text);
        } else {
            String8 text = buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf));
            TEST_CHECK(t, str8_equal(text, STR8_LIT("abc\nd")), "cursors on one line: indent-for-tab-command gave '%S'", text);
        }
        if (!test_view_close(t, &tv)) return 0;
    }

    // The merge rule, on cursors set directly (out of order, overlapping), then normalized.
    struct { i64 points[4], marks[4]; i32 count, primary; const char *after; } merges[] = {
        { { 5, 3, 3 }, { -1, -1, -1 }, 3, 0, "3 *5" },                     // equal points: one left, sorted, the primary kept
        { { 6, 4 }, { 2, 9 }, 2, 0, "*9:2" },                              // overlapping regions: the union, point at its end
        { { 4, 6 }, { 9, 2 }, 2, 0, "*2:9" },                              // the survivor's point was at its region's start
        { { 5, 8 }, { 2, 5 }, 2, 0, "*5:2 8:5" },                          // touching regions stay apart
        { { 2, 5 }, { 8, -1 }, 2, 1, "*8:2" },                             // a bare primary inside a region: it survives
        { { 8, 2 }, { 2, -1 }, 2, 0, "2 *8:2" },                           // a bare cursor on a region's boundary stays
        { { 4, 7, 10, 20 }, { 0, 3, 6, -1 }, 4, 3, "10:0 *20" },           // a chain merges into its first
        { { 0, 3, 5 }, { 5, -1, -1 }, 3, 1, "*5:0" },                      // the merged point meets another: a second pass
    };
    if (!test_view_open(t, &tv, "0123456789abcdefghijklmn", 10, 40)) return 0;
    for (i32 i = 0; i < ARRAY_COUNT(merges); i++) {
        test_mc_set(tv.view, merges[i].points, merges[i].marks, merges[i].count, merges[i].primary);
        view_normalize_cursors(tv.view, &test_settings, &t->arena);
        char *got = test_mc_state(t, tv.view);
        TEST_CHECK(t, test_cstr_equal(got, merges[i].after), "cursors merge %d: got \"%s\", expected \"%s\"", i, got, merges[i].after);
    }
    // Inactive marks never merge.
    {
        i64 points[] = { 6, 4 }, marks[] = { 2, 9 };
        test_mc_set(tv.view, points, marks, 2, 0);
        for (i32 k = 0; k < 2; k++) tv.view->cursors[k].mark_active = 0;
        view_normalize_cursors(tv.view, &test_settings, &t->arena);
        char *got = test_mc_state(t, tv.view);
        TEST_CHECK(t, test_cstr_equal(got, "4 *6"), "cursors merge: inactive marks gave \"%s\"", got);
    }
    i64 live = tv.buf->marker_live;
    TEST_CHECK(t, live == 2 * tv.view->cursor_count + 1, "cursors merge: %D markers live for %d cursors", live, tv.view->cursor_count);

    // keyboard-quit: with an active mark it deactivates the marks and keeps the cursors; then it leaves the primary.
    {
        i64 points[] = { 2, 6, 9 }, marks[] = { -1, 4, -1 };
        test_mc_set(tv.view, points, marks, 3, 1);
        test_view_run(&tv, &CMD_KEYBOARD_QUIT);
        char *got = test_mc_state(t, tv.view);
        TEST_CHECK(t, test_cstr_equal(got, "2 *6 9") && test_echo_is(&tv, "Quit"), "cursors: the first C-g gave \"%s\"", got);
        test_view_run(&tv, &CMD_KEYBOARD_QUIT);
        got = test_mc_state(t, tv.view);
        TEST_CHECK(t, test_cstr_equal(got, "*6"), "cursors: the second C-g gave \"%s\"", got);
        // mark-whole-buffer collapses first.
        test_mc_set(tv.view, points, NULL, 3, 2);
        test_view_run(&tv, &CMD_MARK_WHOLE_BUFFER);
        got = test_mc_state(t, tv.view);
        TEST_CHECK(t, test_cstr_equal(got, "*0:24"), "cursors: mark-whole-buffer gave \"%s\"", got);
    }
    live = tv.buf->marker_live;
    TEST_CHECK(t, live == 3, "cursors: %D markers live after the collapse", live);
    if (!test_view_close(t, &tv)) return 0;

    // max_cursors: the default, the range, the check.
    Config c;
    config_init(&c);
    config_parse(&c, &t->arena, config_default_text(), STR8_LIT("defaults"));
    TEST_CHECK(t, c.settings.max_cursors == 10000, "cursors: max_cursors default %d", c.settings.max_cursors);
    config_parse(&c, &t->arena, STR8_LIT("[settings]\nmax_cursors = 0\n"), STR8_LIT("t.conf"));
    TEST_CHECK(t, c.diags.first && str8_equal(c.diags.first->text, STR8_LIT("t.conf:2: warning: max_cursors 0 is out of range (1 to 100000), using 1")) &&
                      c.settings.max_cursors == 1, "cursors: max_cursors 0: '%S'", c.diags.first ? c.diags.first->text : STR8_LIT(""));
    if (!test_view_open(t, &tv, "a|b|c", 10, 40)) return 0;
    Settings two = test_settings;
    two.max_cursors = 2;
    TEST_CHECK(t, !view_can_add_cursor(tv.view, &two, &tv.echo) && test_echo_is(&tv, "Cursor limit reached (max_cursors = 2)") &&
                      view_can_add_cursor(tv.view, &test_settings, NULL), "cursors: the max_cursors check");
    if (!test_view_close(t, &tv)) return 0;
    LOG("test: ok: the multiple-cursor driver (%d swallowed cases and DEL over a region; %d line commands with two cursors on one "
        "line; %d merge cases and inactive marks; keyboard-quit's two steps; mark-whole-buffer collapsing; max_cursors)",
        (i32)ARRAY_COUNT(swallow), (i32)ARRAY_COUNT(lines), (i32)ARRAY_COUNT(merges));
    return 1;
}

// Every cursor for undo comparisons: "*" before the primary, the point, ",mark" when the mark is set.
static char *test_mc_full(Test *t, View *v) {
    String8 s = STR8_LIT("");
    for (i32 i = 0; i < v->cursor_count; i++) {
        Cursor *c = &v->cursors[i];
        String8 one = str8_fmt(&t->arena, "%s%D", i == v->primary ? "*" : "", view_point(v, c));
        if (c->mark_set) one = str8_fmt(&t->arena, "%S,%D", one, buffer_marker_get(v->buffer, c->mark));
        s = i ? str8_fmt(&t->arena, "%S %S", s, one) : one;
    }
    char *out = PUSH_ARRAY(&t->arena, char, s.len + 1);
    memcpy(out, s.data, (size_t)s.len);
    out[s.len] = 0;
    return out;
}

#define TEST_MC_UNDO_ROUNDS 1500

// Undo with several cursors: the text and every cursor (points, marks, the primary) come back as they were before
// the undone command; undo-redo brings back the cursors of the moment undo ran; through the Emacs chain (two undos
// in a row) and over merged typing; a group made with one cursor undone with three; the log's memory per cursor.
static b32 test_mc_undo(Test *t, u64 seed) {
    t->rng = seed ^ 0x6d63756eull;
    TestView tv;
    // Fixed: three cursors, typing merged into one group, undo, undo-redo.
    if (!test_view_open(t, &tv, "a|b\nc|d\ne|f", 10, 40)) return 0;
    char *before = test_mc_full(t, tv.view);
    for (i32 k = 0; k < 3; k++) {
        tv.ctx.codepoint = 'x';
        test_view_run(&tv, &CMD_SELF_INSERT);
    }
    char *typed = test_view_marked(t, &tv), *typed_full = test_mc_full(t, tv.view);
    TEST_CHECK(t, test_cstr_equal(typed, "axxx|b\ncxxx|d\nexxx|f"), "cursors undo: typed \"%s\"", typed);
    test_view_run(&tv, &CMD_UNDO);
    char *got = test_mc_full(t, tv.view);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "a|b\nc|d\ne|f") && test_cstr_equal(got, before) && !tv.buf->modified,
               "cursors undo: merged typing undone to \"%s\" (\"%s\" before)", got, before);
    test_view_run(&tv, &CMD_UNDO_REDO);
    got = test_mc_full(t, tv.view);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), typed) && test_cstr_equal(got, typed_full), "cursors undo: undo-redo gave \"%s\"", got);
    // A group made with one cursor, undone with three: one cursor, where that command started.
    view_collapse_cursors(tv.view);
    view_set_point(tv.view, view_primary(tv.view), 0);
    tv.ctx.codepoint = 'y';
    test_view_run(&tv, &CMD_SELF_INSERT);
    i64 points[] = { 1, 6, 12 };
    test_mc_set(tv.view, points, NULL, 3, 2);
    test_view_run(&tv, &CMD_UNDO);
    got = test_mc_full(t, tv.view);
    TEST_CHECK(t, tv.view->cursor_count == 1 && view_point(tv.view, view_primary(tv.view)) == 0, "cursors undo: one-cursor group with three: \"%s\"", got);
    // The log keeps 24 bytes per cursor for a multiple-cursor command.
    test_mc_set(tv.view, points, NULL, 3, 0);
    u64 used = tv.buf->undo.log_used;
    test_view_run(&tv, &CMD_NEWLINE);
    u64 grew = tv.buf->undo.log_used - used;
    TEST_CHECK(t, grew >= 3 * sizeof(BufferUndoCursor) && tv.buf->undo.groups[tv.buf->undo.group_count - 1].cursor_count == 3,
               "cursors undo: the group's cursors (%U bytes)", grew);
    if (!test_view_close(t, &tv)) return 0;

    // Random: a command on random cursors (regions too), then undo (sometimes after a second command, through the
    // chain), then undo-redo.
    static const Command *cmds[] = { &CMD_SELF_INSERT, &CMD_DELETE_BACKWARD_CHAR, &CMD_DELETE_CHAR, &CMD_NEWLINE, &CMD_KILL_LINE,
                                     &CMD_KILL_WORD, &CMD_BACKWARD_KILL_WORD, &CMD_INDENT_FOR_TAB_COMMAND, &CMD_COMMENT_LINE,
                                     &CMD_KILL_REGION, &CMD_TRANSPOSE_CHARS, &CMD_OPEN_LINE, &CMD_JUST_ONE_SPACE, &CMD_UPCASE_WORD };
    i64 undos = 0, chains = 0;
    u8 *text = PUSH_ARRAY(&t->arena, u8, 512);
    for (i32 round = 0; round < TEST_MC_UNDO_ROUNDS; round++) {
        static const char alphabet[] = "ab c\n{};x\t";
        i64 len = 10 + test_below(t, 200);
        for (i64 i = 0; i < len; i++) text[i] = (u8)alphabet[test_below(t, sizeof(alphabet) - 1)];
        text[len] = 0;
        if (!test_view_open(t, &tv, (const char *)text, 10, 40)) return 0;
        i32 n = 2 + (i32)test_below(t, 7);
        i64 pts[8], mks[8];
        for (i32 i = 0; i < n; i++) {
            pts[i] = test_below(t, len + 1);
            mks[i] = test_below(t, 3) ? -1 : (i64)test_below(t, len + 1);
        }
        test_mc_set(tv.view, pts, mks, n, (i32)test_below(t, n));
        view_normalize_cursors(tv.view, &test_settings, &t->arena);
        String8 text0 = str8_copy(&t->arena, buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)));
        char *cur0 = test_mc_full(t, tv.view);
        // A command that starts with one cursor keeps Phase 6's undo: the point comes back, the mark is not logged.
        b32 one = tv.view->cursor_count == 1;
        i64 p0 = view_point(tv.view, view_primary(tv.view));
        const Command *cmd = cmds[test_below(t, ARRAY_COUNT(cmds))];
        tv.ctx.codepoint = "x}("[test_below(t, 3)];
        test_view_run(&tv, cmd);
        String8 text1 = str8_copy(&t->arena, buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)));
        char *cur1 = test_mc_full(t, tv.view);
        if (str8_equal(text0, text1)) {
            if (!test_view_close(t, &tv)) return 0;
            continue;
        }
        b32 chain = test_below(t, 3) == 0;
        if (chain) { // a second command (one that never merges with the first), undone first by the chain
            test_view_run(&tv, &CMD_OPEN_LINE);
            test_view_run(&tv, &CMD_UNDO);
            String8 t2 = str8_copy(&t->arena, buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)));
            char *c2 = test_mc_full(t, tv.view);
            TEST_CHECK(t, str8_equal(t2, text1) && test_cstr_equal(c2, cur1), "cursors undo round %d (%s): the chain's first undo: \"%s\", "
                       "expected \"%s\", text equal %d, cur0 \"%s\" (seed 0x%X)", round, cmd->name, c2, cur1, str8_equal(t2, text1), cur0, seed);
            chains++;
        }
        test_view_run(&tv, &CMD_UNDO);
        String8 text2 = str8_copy(&t->arena, buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)));
        char *cur2 = test_mc_full(t, tv.view);
        b32 back = one ? tv.view->cursor_count == 1 && view_point(tv.view, view_primary(tv.view)) == p0 : test_cstr_equal(cur2, cur0);
        TEST_CHECK(t, str8_equal(text2, text0) && back && !tv.buf->modified,
                   "cursors undo round %d (%s): text '%S', cursors \"%s\", expected '%S' \"%s\" (seed 0x%X)", round, cmd->name, text2, cur2,
                   text0, cur0, seed);
        // undo-redo puts back the cursors of the moment the undo it reverts ran (cur1, as the command left them);
        // undoing that returns to the state before the command with the cursors of the moment undo-redo ran (cur0).
        test_view_run(&tv, &CMD_UNDO_REDO);
        String8 text3 = str8_copy(&t->arena, buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)));
        char *cur3 = test_mc_full(t, tv.view);
        TEST_CHECK(t, str8_equal(text3, text1) && test_cstr_equal(cur3, cur1), "cursors undo round %d (%s): undo-redo gave '%S' \"%s\", "
                   "expected '%S' \"%s\" (seed 0x%X)", round, cmd->name, text3, cur3, text1, cur1, seed);
        test_view_run(&tv, &CMD_UNDO);
        String8 text4 = str8_copy(&t->arena, buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)));
        char *cur4 = test_mc_full(t, tv.view);
        TEST_CHECK(t, str8_equal(text4, text0) && (one || test_cstr_equal(cur4, cur0)), "cursors undo round %d: undo after undo-redo gave '%S' \"%s\" "
                   "(seed 0x%X)", round, text4, cur4, seed);
        undos++;
        if (!test_view_close(t, &tv)) return 0;
    }
    LOG("test: ok: multiple-cursor undo (merged typing, undo-redo, a one-cursor group undone with three, 24 bytes per cursor in the "
        "log; %D random commands undone and redone with every cursor back, %D through the chain; seed 0x%X)", undos, chains, seed);
    return 1;
}

// The newest kill's pieces as text: "a|b|c".
static String8 test_mc_pieces(Test *t, KillRing *k, i32 back) {
    String8 s = STR8_LIT("");
    for (i32 i = 0; i < kill_pieces(k, back); i++) s = i ? str8_fmt(&t->arena, "%S|%S", s, kill_piece(k, back, i)) : kill_piece(k, back, i);
    return str8_copy(&t->arena, s);
}

// Kill and yank with several cursors: one piece per cursor in one entry, the clipboard text (pieces joined by
// newlines), appending and prepending per piece, yank with the same count (each its piece) and another count (the
// whole text), yank-pop, a clipboard changed elsewhere, kill-ring-save.
static b32 test_mc_kill(Test *t) {
    TestView tv;
    if (!test_view_open(t, &tv, "a|bc\nd|ef\ng|hi", 10, 40)) return 0;
    test_view_run(&tv, &CMD_KILL_LINE);
    String8 got = test_mc_pieces(t, tv.kills, 0);
    String8 clip = { 0 };
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "a|\nd|\ng|") && str8_equal(got, STR8_LIT("bc|ef|hi")) && tv.kills->count == 1,
               "cursors kill: pieces '%S'", got);
    TEST_CHECK(t, str8_equal(kill_entry(tv.kills, 0), STR8_LIT("bc\nef\nhi")) && os_clipboard_get(&t->arena, &clip) &&
                      str8_equal(clip, STR8_LIT("bc\nef\nhi")), "cursors kill: the entry and the clipboard '%S'", clip);
    test_view_run(&tv, &CMD_YANK); // the same count: each its piece
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "abc|\ndef|\nghi|"), "cursors yank: same count \"%s\"", test_view_marked(t, &tv));
    // yank-pop with several cursors: an older multiple-cursor kill, piece by piece.
    test_view_run(&tv, &CMD_BACKWARD_KILL_WORD); // kills "abc", "def", "ghi"
    got = test_mc_pieces(t, tv.kills, 0);
    TEST_CHECK(t, str8_equal(got, STR8_LIT("abc|def|ghi")), "cursors kill: backward pieces '%S', text \"%s\"", got, test_view_marked(t, &tv));
    test_view_run(&tv, &CMD_YANK);
    test_view_run(&tv, &CMD_YANK_POP);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "bc|\nef|\nhi|"), "cursors yank-pop: \"%s\"", test_view_marked(t, &tv));
    // Another count: every cursor gets the whole text.
    view_collapse_cursors(tv.view);
    view_add_cursor(tv.view, buffer_size(tv.buf));
    test_view_run(&tv, &CMD_YANK);
    String8 text = str8_copy(&t->arena, buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)));
    TEST_CHECK(t, str8_equal(text, STR8_LIT("bcabc\ndef\nghi\nef\nhiabc\ndef\nghi")), "cursors yank: another count gave '%S'", text);
    if (!test_view_close(t, &tv)) return 0;

    // Appending and prepending per piece; a kill with another count makes a new entry.
    if (!test_view_open(t, &tv, "|foo bar\n|baz qux", 10, 40)) return 0;
    test_view_run(&tv, &CMD_KILL_WORD);
    test_view_run(&tv, &CMD_KILL_WORD);
    got = test_mc_pieces(t, tv.kills, 0);
    TEST_CHECK(t, str8_equal(got, STR8_LIT("foo bar|baz qux")) && tv.kills->count == 1, "cursors kill: appended '%S'", got);
    if (!test_view_close(t, &tv)) return 0;
    if (!test_view_open(t, &tv, "foo bar|\nbaz qux|", 10, 40)) return 0;
    test_view_run(&tv, &CMD_BACKWARD_KILL_WORD);
    test_view_run(&tv, &CMD_BACKWARD_KILL_WORD);
    got = test_mc_pieces(t, tv.kills, 0);
    TEST_CHECK(t, str8_equal(got, STR8_LIT("foo bar|baz qux")) && tv.kills->count == 1, "cursors kill: prepended '%S'", got);
    // A clipboard changed elsewhere: one piece, the whole text at every cursor.
    os_clipboard_set(STR8_LIT("X"));
    test_view_run(&tv, &CMD_YANK);
    TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "X|\nX|") && kill_pieces(tv.kills, 0) == 1, "cursors yank: the clipboard \"%s\"",
               test_view_marked(t, &tv));
    if (!test_view_close(t, &tv)) return 0;

    // kill-ring-save with regions: pieces, the text unchanged; a single-cursor kill after it makes a new entry.
    if (!test_view_open(t, &tv, "one two\nthree four", 10, 40)) return 0;
    i64 points[] = { 3, 13 }, marks[] = { 0, 8 };
    test_mc_set(tv.view, points, marks, 2, 0);
    test_view_run(&tv, &CMD_KILL_RING_SAVE);
    got = test_mc_pieces(t, tv.kills, 0);
    TEST_CHECK(t, str8_equal(got, STR8_LIT("one|three")), "cursors kill-ring-save: '%S'", got);
    view_collapse_cursors(tv.view);
    test_view_run(&tv, &CMD_KILL_WORD);
    TEST_CHECK(t, tv.kills->count == 2 && kill_pieces(tv.kills, 0) == 1 && str8_equal(kill_entry(tv.kills, 0), STR8_LIT(" two")),
               "cursors kill: one cursor after pieces '%S'", kill_entry(tv.kills, 0));
    if (!test_view_close(t, &tv)) return 0;
    LOG("test: ok: multiple-cursor kills and yanks (one piece per cursor, the clipboard text, the same count and another, yank-pop, "
        "appending and prepending per piece, a clipboard changed elsewhere, kill-ring-save, a one-cursor kill after pieces)");
    return 1;
}

// The multiple-cursor commands on tricky input (headless view: their searches end at once).
static b32 test_mc_commands(Test *t) {
    TestView tv;
    struct {
        const char *text;
        i64 point, mark; // the primary's region (mark -1: none)
        const Command *cmds[4];
        const char *after, *echo;
    } cases[] = {
        { "aaaa", 2, 0, { &CMD_MARK_NEXT_LIKE_THIS }, "2:0 *4:2", "" },                                  // not overlapping
        { "aaaa", 2, 0, { &CMD_MARK_NEXT_LIKE_THIS, &CMD_MARK_NEXT_LIKE_THIS }, "2:0 *4:2", "No more matches" },
        { "ab xx ab", 2, 0, { &CMD_MARK_NEXT_LIKE_THIS }, "2:0 *8:6", "" },                              // at the end
        { "ab xx ab", 2, 0, { &CMD_MARK_PREVIOUS_LIKE_THIS }, "*2:0", "No more matches" },
        { "ab xx ab", 8, 6, { &CMD_MARK_PREVIOUS_LIKE_THIS }, "*2:0 8:6", "" },                          // at the start
        { "ab xx ab", 6, 8, { &CMD_MARK_PREVIOUS_LIKE_THIS }, "*0:2 6:8", "" },                          // point at the start
        { "foo\nbar\nx foo\nbar", 7, 0, { &CMD_MARK_NEXT_LIKE_THIS }, "7:0 *17:10", "" },               // multi-line
        { "Foo foo", 7, 4, { &CMD_MARK_PREVIOUS_LIKE_THIS }, "*7:4", "No more matches" },                // case sensitive
        { "abcdef\nab\nabcdef", 4, -1, { &CMD_MARK_NEXT_LIKE_THIS }, "4 *9", "" },                      // a short line: its end
        { "abcdef\nab\nabcdef", 4, -1, { &CMD_MARK_NEXT_LIKE_THIS, &CMD_MARK_NEXT_LIKE_THIS }, "4 9 *14", "" }, // the column kept
        { "abcdef\nabcdef\nabcdef", 0, -1, { &CMD_NEXT_LINE, &CMD_MOVE_END_OF_LINE, &CMD_MARK_NEXT_LIKE_THIS }, "13 *20", "" }, // a stale goal column
        { "abcdef\nab\nabcdef", 14, -1, { &CMD_MARK_NEXT_LIKE_THIS }, "*14", "No more matches" },       // the last line
        { "abcdef\nab\nabcdef", 14, -1, { &CMD_MARK_PREVIOUS_LIKE_THIS }, "*9 14", "" },
        { "abcdef\nab\nabcdef", 4, -1, { &CMD_MARK_PREVIOUS_LIKE_THIS }, "*4", "No more matches" },     // the first line
        { "x a x b x c", 1, 0, { &CMD_MARK_NEXT_LIKE_THIS, &CMD_SKIP_TO_NEXT_LIKE_THIS }, "1:0 *9:8", "" },
        { "x a x b x c", 1, 0, { &CMD_MARK_NEXT_LIKE_THIS, &CMD_SKIP_TO_NEXT_LIKE_THIS, &CMD_SKIP_TO_NEXT_LIKE_THIS }, "1:0 *9:8",
          "No more matches" },
        { "x a x b x c", 1, 0, { &CMD_MARK_NEXT_LIKE_THIS, &CMD_MARK_NEXT_LIKE_THIS, &CMD_UNMARK_NEXT_LIKE_THIS }, "1:0 *5:4", "" },
        { "x a x", 1, 0, { &CMD_UNMARK_NEXT_LIKE_THIS }, "*1:0", "Only one cursor" },
        { "ab\nab\nab", 1, -1, { &CMD_MARK_NEXT_LIKE_THIS, &CMD_SKIP_TO_NEXT_LIKE_THIS }, "1 *7", "" },  // skip by lines
        { "foo bar foo baz foo", 11, 8, { &CMD_MARK_ALL_LIKE_THIS }, "3:0 *11:8 19:16", "Marked 3 occurrences" },
        { "aaaaa", 2, 0, { &CMD_MARK_ALL_LIKE_THIS }, "*2:0 4:2", "Marked 2 occurrences" },             // overlapping: from the start
        { "foo", 1, -1, { &CMD_MARK_ALL_LIKE_THIS }, "*1", "No region" },
        { "abcdef\nab\nabcdef\nxyz", 14, 2, { &CMD_EDIT_LINES }, "4 9 *14", "" },                        // short lines: their end
        { "abcdef\nab\nabcdef\nxyz", 14, 2, { &CMD_EDIT_ENDS_OF_LINES }, "6 9 *16", "" },
        { "abcdef\nab\nabcdef\nxyz", 14, 2, { &CMD_EDIT_BEGINNINGS_OF_LINES }, "0 7 *10", "" },
        { "abcdef\nab\nabcdef\nxyz", 2, 14, { &CMD_EDIT_LINES }, "*2 9 12", "" },                       // point on the first line
        { "abc", 1, -1, { &CMD_EDIT_LINES }, "*1", "No region" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        if (!test_view_open(t, &tv, cases[i].text, 10, 40)) return 0;
        i64 points[] = { cases[i].point }, marks[] = { cases[i].mark };
        test_mc_set(tv.view, points, marks, 1, 0);
        for (i32 k = 0; k < ARRAY_COUNT(cases[i].cmds) && cases[i].cmds[k]; k++) test_view_run(&tv, cases[i].cmds[k]);
        char *got = test_mc_state(t, tv.view);
        TEST_CHECK(t, test_cstr_equal(got, cases[i].after) && test_echo_is(&tv, cases[i].echo), "cursors command %d (%s): got \"%s\" echo '%S', "
                   "expected \"%s\" '%s'", i, cases[i].cmds[0]->name, got, str8(tv.echo.text, tv.echo.len), cases[i].after, cases[i].echo);
        if (!test_view_close(t, &tv)) return 0;
    }
    // The cap, max_cursors = 5: each adding command stops there and says so.
    Settings five = test_settings;
    five.max_cursors = 5;
    if (!test_view_open(t, &tv, "x x x x x x x x x x\n1\n2\n3\n4\n5\n6\n7", 10, 40)) return 0;
    tv.ctx.settings = &five;
    i64 p1[] = { 1 }, m1[] = { 0 };
    test_mc_set(tv.view, p1, m1, 1, 0);
    test_view_run(&tv, &CMD_MARK_ALL_LIKE_THIS);
    TEST_CHECK(t, tv.view->cursor_count == 5 && test_echo_is(&tv, "Cursor limit reached (max_cursors = 5): 5 occurrences marked"),
               "cursors cap: mark-all left %d cursors, '%S'", tv.view->cursor_count, str8(tv.echo.text, tv.echo.len));
    test_view_run(&tv, &CMD_MARK_NEXT_LIKE_THIS);
    TEST_CHECK(t, tv.view->cursor_count == 5 && test_echo_is(&tv, "Cursor limit reached (max_cursors = 5)"), "cursors cap: mark-next");
    test_view_run(&tv, &CMD_MARK_PREVIOUS_LIKE_THIS);
    TEST_CHECK(t, tv.view->cursor_count == 5 && test_echo_is(&tv, "Cursor limit reached (max_cursors = 5)"), "cursors cap: mark-previous");
    i64 p2[] = { buffer_size(tv.buf) }, m2[] = { 0 };
    test_mc_set(tv.view, p2, m2, 1, 0);
    test_view_run(&tv, &CMD_EDIT_LINES);
    TEST_CHECK(t, tv.view->cursor_count == 5 && test_echo_is(&tv, "Cursor limit reached (max_cursors = 5)"), "cursors cap: edit-lines %d",
               tv.view->cursor_count);
    i64 live = tv.buf->marker_live;
    TEST_CHECK(t, live == 2 * 5 + 1, "cursors cap: %D markers live", live);
    if (!test_view_close(t, &tv)) return 0;
    LOG("test: ok: multiple-cursor commands (%d cases: overlapping occurrences, at the buffer's ends, mark-previous, a multi-line "
        "region, case, lines with a short one and the column kept, the first and last lines, skip and unmark by text and by lines, "
        "mark-all, the edit-lines family; the cap at 5 for mark-all, mark-next, mark-previous and edit-lines)", (i32)ARRAY_COUNT(cases));
    return 1;
}

// ---------------------------------------------------------------------------
// The multiple-cursor differential fuzz: the driver against the single-cursor behavior applied cursor by cursor
// to a flat text, then normalized naively.

#define TEST_MC_FUZZ_STEPS 20000
#define TEST_MC_FUZZ_MAX 16      // cursors at the start of an epoch (the new commands add more)
#define TEST_MC_REF_MAX 256
#define TEST_MC_EDIT_LOG 4096

typedef struct TestMcCursor {
    i64 point, mark, goal;
    b32 mark_set, mark_active, mark_shift;
    u64 serial;
    b32 swallowed;
} TestMcCursor;

typedef struct TestMcRef {
    TestMcCursor c[TEST_MC_REF_MAX];
    i32 count, primary;
    u64 serial;     // the last serial given
    Buffer *buf;    // the flat text (one cursor's view runs each command on it)
    TestView one;
} TestMcRef;

static void test_mc_read(View *v, TestMcRef *r) {
    r->count = v->cursor_count;
    r->primary = v->primary;
    r->serial = v->cursor_serial;
    for (i32 i = 0; i < v->cursor_count; i++) {
        Cursor *c = &v->cursors[i];
        r->c[i] = (TestMcCursor){ view_point(v, c), buffer_marker_get(v->buffer, c->mark), c->goal_col, c->mark_set, c->mark_active,
                                  c->mark_shift, c->serial, 0 };
    }
}

// The marker rule of buffer.h (no snapping: the fuzz texts are valid UTF-8 and edits keep to boundaries).
static i64 test_mc_rule(i64 pos, i64 start, i64 end, i64 len, b32 advance, b32 *swallowed) {
    if (pos < start) return pos;
    if (pos > end) return pos + len - (end - start);
    if (pos == end && end > start) return start + len;
    if (pos > start && swallowed) *swallowed = 1;
    return advance ? start + len : start;
}

static void test_mc_span(const TestMcCursor *c, i64 *s, i64 *e) {
    *s = *e = c->point;
    if (c->mark_set && c->mark_active) {
        *s = MIN(c->point, c->mark);
        *e = MAX(c->point, c->mark);
    }
}

// The pairwise merge rule, order-independent.
static b32 test_mc_overlap(const TestMcCursor *a, const TestMcCursor *b) {
    if (a->point == b->point) return 1;
    i64 as, ae, bs, be;
    test_mc_span(a, &as, &ae);
    test_mc_span(b, &bs, &be);
    b32 ar = ae > as, br = be > bs;
    if (ar && br) return MAX(as, bs) < MIN(ae, be);
    if (ar) return as < b->point && b->point < ae;
    if (br) return bs < a->point && a->point < be;
    return 0;
}

static b32 test_mc_before(const TestMcCursor *a, i32 ai, const TestMcCursor *b, i32 bi) {
    i64 as, ae, bs, be;
    test_mc_span(a, &as, &ae);
    test_mc_span(b, &bs, &be);
    if (as != bs) return as < bs;
    if (a->point != b->point) return a->point < b->point;
    return ai < bi;
}

// Naive normalization: groups of the pairwise rule (transitive), merged at once, again until none overlap; sorted.
static void test_mc_normalize(TestMcRef *r) {
    for (;;) {
        i32 group[TEST_MC_REF_MAX];
        for (i32 i = 0; i < r->count; i++) group[i] = i;
        b32 any = 0;
        for (b32 changed = 1; changed;) { // the smallest index of each connected group
            changed = 0;
            for (i32 i = 0; i < r->count; i++) {
                for (i32 j = i + 1; j < r->count; j++) {
                    if (!test_mc_overlap(&r->c[i], &r->c[j])) continue;
                    any = 1;
                    i32 g = MIN(group[i], group[j]);
                    if (group[i] != g || group[j] != g) changed = 1;
                    group[i] = group[j] = g;
                }
            }
        }
        TestMcCursor out[TEST_MC_REF_MAX];
        i32 m = 0, primary = 0;
        for (i32 g = 0; g < r->count; g++) {
            i32 survivor = -1;
            i64 gs = I64_MAX, ge = -1;
            i32 members = 0;
            for (i32 i = 0; i < r->count; i++) {
                if (group[i] != g) continue;
                members++;
                i64 s, e;
                test_mc_span(&r->c[i], &s, &e);
                gs = MIN(gs, s);
                ge = MAX(ge, e);
                if (survivor < 0 || i == r->primary || (survivor != r->primary && test_mc_before(&r->c[i], i, &r->c[survivor], survivor))) survivor = i;
            }
            if (survivor < 0) continue;
            TestMcCursor c = r->c[survivor];
            if (members > 1 && ge > gs) {
                i64 s, e;
                test_mc_span(&c, &s, &e);
                b32 at_end = c.point == e, shift = c.mark_active && c.mark_shift;
                c.point = at_end ? ge : gs;
                c.mark = at_end ? gs : ge;
                c.mark_set = c.mark_active = 1;
                c.mark_shift = shift;
            }
            if (group[r->primary] == g) primary = m;
            out[m++] = c;
        }
        // Sorted by (span start, point).
        for (i32 i = 1; i < m; i++) {
            for (i32 j = i; j > 0 && test_mc_before(&out[j], j, &out[j - 1], j - 1); j--) {
                TestMcCursor x = out[j];
                out[j] = out[j - 1];
                out[j - 1] = x;
                if (primary == j) primary = j - 1;
                else if (primary == j - 1) primary = j;
            }
        }
        memcpy(r->c, out, sizeof(TestMcCursor) * (u64)m);
        r->count = m;
        r->primary = primary;
        if (!any) return;
    }
}

static i64 test_mc_line(Buffer *buf, i64 pos) {
    return buffer_line_of(buf, pos);
}

static void test_mc_add(TestMcRef *r, i64 point, i64 mark, b32 region, i64 goal) {
    TestMcCursor c = { point, region ? mark : point, goal, region, region, 0, ++r->serial, 0 };
    r->c[r->count] = c;
    r->primary = r->count++;
}

static void test_mc_ref_collapse(TestMcRef *r) {
    r->c[0] = r->c[r->primary];
    r->count = 1;
    r->primary = 0;
}

static i32 test_mc_newest(TestMcRef *r) {
    i32 k = 0;
    for (i32 i = 1; i < r->count; i++) if (r->c[i].serial > r->c[k].serial) k = i;
    return k;
}

// Naive search in the flat text: forward, the first start >= from; backward, the last match ending <= from. -1: none.
static i64 test_mc_find(Buffer *buf, Arena *arena, String8 needle, b32 forward, i64 from) {
    String8 text = str8_copy(arena, buffer_text(buf, arena, 0, buffer_size(buf)));
    if (forward) {
        for (i64 s = from; s + needle.len <= text.len; s++) if (mem_equal(text.data + s, needle.data, needle.len)) return s;
    } else {
        for (i64 s = from - needle.len; s >= 0; s--) if (mem_equal(text.data + s, needle.data, needle.len)) return s;
    }
    return -1;
}

// The reference of the COMMAND_ONCE commands in the fuzz (keyboard-quit and the new ones).
static void test_mc_ref_once(Test *t, TestMcRef *r, const Command *cmd, i32 max, const Command *last_command) {
    Buffer *buf = r->buf;
    const Command *lc = last_command;
    b32 vertical = lc == &CMD_NEXT_LINE || lc == &CMD_PREVIOUS_LINE || lc == &CMD_MARK_NEXT_LIKE_THIS || lc == &CMD_MARK_PREVIOUS_LIKE_THIS ||
                   lc == &CMD_SKIP_TO_NEXT_LIKE_THIS;
    TestMcCursor *p = &r->c[r->primary];
    i64 rs, re;
    test_mc_span(p, &rs, &re);
    String8 needle = str8_copy(&t->arena, buffer_text(buf, &t->arena, rs, re));
    b32 at_end = p->point >= p->mark;
    if (cmd == &CMD_KEYBOARD_QUIT) {
        b32 active = 0;
        for (i32 i = 0; i < r->count; i++) active |= r->c[i].mark_active;
        if (r->count > 1 && !active) test_mc_ref_collapse(r);
        for (i32 i = 0; i < r->count; i++) r->c[i].mark_active = r->c[i].mark_shift = 0;
    } else if (cmd == &CMD_MARK_NEXT_LIKE_THIS || cmd == &CMD_MARK_PREVIOUS_LIKE_THIS) {
        b32 fwd = cmd == &CMD_MARK_NEXT_LIKE_THIS;
        if (r->count >= max) return;
        TestMcCursor *edge = fwd ? &r->c[r->count - 1] : &r->c[0];
        i64 es, ee;
        test_mc_span(edge, &es, &ee);
        if (re > rs) {
            i64 m = test_mc_find(buf, &t->arena, needle, fwd, fwd ? ee : es);
            if (m >= 0) test_mc_add(r, at_end ? m + needle.len : m, at_end ? m : m + needle.len, 1, -1);
        } else {
            i64 line = test_mc_line(buf, edge->point) + (fwd ? 1 : -1);
            if (line < 0 || line >= buffer_line_count(buf)) return;
            i64 col = vertical && edge->goal >= 0 ? edge->goal : view_column_of(buf, edge->point);
            test_mc_add(r, view_offset_at_column(buf, line, col), 0, 0, col);
        }
    } else if (cmd == &CMD_SKIP_TO_NEXT_LIKE_THIS) {
        i32 k = test_mc_newest(r);
        TestMcCursor *n = &r->c[k];
        i64 ns, ne;
        test_mc_span(n, &ns, &ne);
        if (re > rs) {
            i64 m = test_mc_find(buf, &t->arena, needle, 1, ne);
            if (m < 0) return;
            n->point = at_end ? m + needle.len : m;
            n->mark = at_end ? m : m + needle.len;
            n->mark_set = n->mark_active = 1;
            n->mark_shift = 0;
        } else {
            i64 line = test_mc_line(buf, n->point) + 1;
            if (line >= buffer_line_count(buf)) return;
            i64 col = vertical && n->goal >= 0 ? n->goal : view_column_of(buf, n->point);
            n->point = view_offset_at_column(buf, line, col);
            n->goal = col;
        }
        r->primary = k;
    } else if (cmd == &CMD_UNMARK_NEXT_LIKE_THIS) {
        if (r->count <= 1) return;
        i32 k = test_mc_newest(r);
        memmove(&r->c[k], &r->c[k + 1], sizeof(TestMcCursor) * (u64)(r->count - k - 1));
        r->count--;
        r->primary = test_mc_newest(r);
    } else if (cmd == &CMD_MARK_ALL_LIKE_THIS) {
        if (re <= rs) return;
        test_mc_ref_collapse(r);
        i64 found[TEST_MC_REF_MAX], n = 0;
        for (i64 from = 0; n + 1 < max;) {
            i64 m = test_mc_find(buf, &t->arena, needle, 1, from);
            if (m < 0) break;
            if (m != rs) found[n++] = m;
            from = m + needle.len;
        }
        for (i64 i = 0; i < n; i++) {
            i32 keep = r->primary;
            test_mc_add(r, at_end ? found[i] + needle.len : found[i], at_end ? found[i] : found[i] + needle.len, 1, -1);
            r->primary = keep;
        }
    } else { // the edit-lines family
        if (!(p->mark_set && p->mark_active)) return;
        i32 mode = cmd == &CMD_EDIT_LINES ? 0 : cmd == &CMD_EDIT_ENDS_OF_LINES ? 1 : 2;
        i64 point = p->point, pline = test_mc_line(buf, point), col = view_column_of(buf, point);
        i64 first = test_mc_line(buf, rs), last = test_mc_line(buf, re);
        test_mc_ref_collapse(r);
        r->c[0].mark_active = r->c[0].mark_shift = 0;
        if (last - first + 1 > max) last = first + max - 1;
        for (i64 line = first; line <= last; line++) {
            i64 pos = mode == 0 ? view_offset_at_column(buf, line, col) : mode == 1 ? buffer_line_end(buf, line) : buffer_line_start(buf, line);
            if (line == first) {
                r->c[0].point = pos;
            } else {
                i32 keep = r->primary;
                test_mc_add(r, pos, 0, 0, -1);
                r->primary = keep;
            }
            r->c[r->count - 1].goal = mode == 0 ? col : -1;
            if (line == pline) r->primary = r->count - 1;
        }
        if (pline > last) r->primary = 0; // capped before point's line: the first cursor stays the primary
    }
}

static b32 test_mc_fuzz(Test *t, u64 seed) {
    t->rng = seed ^ 0x6d6366757aull;
    static const struct { const Command *cmd; u32 cp; b32 shift; } cmds[] = {
        { &CMD_FORWARD_CHAR }, { &CMD_BACKWARD_CHAR }, { &CMD_NEXT_LINE }, { &CMD_PREVIOUS_LINE }, { &CMD_FORWARD_WORD },
        { &CMD_BACKWARD_WORD }, { &CMD_MOVE_BEGINNING_OF_LINE }, { &CMD_MOVE_END_OF_LINE }, { &CMD_FORWARD_CHAR, 0, 1 },
        { &CMD_NEXT_LINE, 0, 1 }, { &CMD_BACKWARD_WORD, 0, 1 }, { &CMD_FORWARD_PARAGRAPH }, { &CMD_END_OF_BUFFER },
        { &CMD_SELF_INSERT, 'a' }, { &CMD_SELF_INSERT, ' ' }, { &CMD_SELF_INSERT, '}' }, { &CMD_SELF_INSERT, ')' },
        { &CMD_SELF_INSERT, ':' }, { &CMD_SELF_INSERT, 0x11F }, { &CMD_DELETE_BACKWARD_CHAR }, { &CMD_DELETE_CHAR },
        { &CMD_NEWLINE }, { &CMD_OPEN_LINE }, { &CMD_TRANSPOSE_CHARS }, { &CMD_UPCASE_WORD }, { &CMD_CAPITALIZE_WORD },
        { &CMD_JUST_ONE_SPACE }, { &CMD_DELETE_HORIZONTAL_SPACE }, { &CMD_TAB_TO_TAB_STOP },
        { &CMD_KILL_LINE }, { &CMD_KILL_WORD }, { &CMD_BACKWARD_KILL_WORD }, { &CMD_KILL_REGION }, { &CMD_KILL_WHOLE_LINE },
        { &CMD_KILL_RING_SAVE }, { &CMD_YANK }, { &CMD_YANK },
        { &CMD_INDENT_FOR_TAB_COMMAND }, { &CMD_UNINDENT }, { &CMD_COMMENT_LINE }, { &CMD_DELETE_INDENTATION },
        { &CMD_SET_MARK_COMMAND }, { &CMD_EXCHANGE_POINT_AND_MARK }, { &CMD_KEYBOARD_QUIT },
        { &CMD_MARK_NEXT_LIKE_THIS }, { &CMD_MARK_NEXT_LIKE_THIS }, { &CMD_MARK_PREVIOUS_LIKE_THIS }, { &CMD_MARK_ALL_LIKE_THIS },
        { &CMD_SKIP_TO_NEXT_LIKE_THIS }, { &CMD_UNMARK_NEXT_LIKE_THIS }, { &CMD_EDIT_LINES }, { &CMD_EDIT_ENDS_OF_LINES },
        { &CMD_EDIT_BEGINNINGS_OF_LINES },
    };
    static const char *pieces[] = { "a", "b", "x", " ", " ", "\n", "\n", "\t", "{", "}", "(", ")", ";", ":", "foo", "foo ", "\xC4\x9F", "\xE2\x82\xAC" };
    TestMcRef *r = PUSH_STRUCT(&t->arena, TestMcRef);
    r->buf = buffer_create(STR8_LIT("ref"));
    buffer_undo_enable(r->buf, 0);
    r->buf->dev_edits = PUSH_ARRAY(&t->arena, BufferBatchEdit, TEST_MC_EDIT_LOG);
    r->buf->dev_edit_cap = TEST_MC_EDIT_LOG;
    TestView *one = &r->one;
    if (!test_view_open(t, one, "", 10, 40)) return 0;
    view_destroy(one->view); // the one-cursor view runs on the reference buffer
    buffer_destroy(one->buf);
    one->buf = r->buf;
    view_init(one->view, r->buf);
    one->view->rows = 10;
    one->view->cols = 40;
    TestView tv;
    u8 *text = PUSH_ARRAY(&t->arena, u8, 1024);
    i64 steps = 0, epochs = 0, max_seen = 0, merges = 0, kills = 0;
    u64 epoch_mark = arena_pos(&t->arena);
    while (steps < TEST_MC_FUZZ_STEPS) {
        arena_pop_to(&t->arena, epoch_mark);
        // An epoch: a random text and random cursors.
        i64 len = 0, want = test_below(t, 220);
        while (len < want) {
            const char *pc = pieces[test_below(t, ARRAY_COUNT(pieces))];
            while (*pc) text[len++] = (u8)*pc++;
        }
        text[len] = 0;
        for (i64 i = 0; i < len; i++) if (text[i] == '|') text[i] = 'x';
        if (!test_view_open(t, &tv, (const char *)text, 10, 40)) return 0;
        i32 n = 1 + (i32)test_below(t, TEST_MC_FUZZ_MAX);
        i64 pts[TEST_MC_FUZZ_MAX], mks[TEST_MC_FUZZ_MAX];
        for (i32 i = 0; i < n; i++) {
            pts[i] = buffer_snap_char(tv.buf, test_below(t, len + 1));
            mks[i] = test_below(t, 3) ? -1 : buffer_snap_char(tv.buf, test_below(t, len + 1));
        }
        test_mc_set(tv.view, pts, mks, n, (i32)test_below(t, n));
        view_normalize_cursors(tv.view, &test_settings, &t->arena);
        u64 step_mark = arena_pos(&t->arena);
        for (i32 k = 0; k < 30 && steps < TEST_MC_FUZZ_STEPS; k++, steps++) {
            arena_pop_to(&t->arena, step_mark);
            View *v = tv.view;
            i32 pick = (i32)test_below(t, ARRAY_COUNT(cmds));
            const Command *cmd = cmds[pick].cmd;
            // The reference starts from the real state.
            test_mc_read(v, r);
            String8 before = str8_copy(&t->arena, buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)));
            buffer_replace(r->buf, 0, buffer_size(r->buf), before);
            i32 n0 = v->cursor_count;
            KillRing *k0 = tv.kills;
            i32 count0 = k0->count, pieces0 = kill_pieces(k0, 0);
            String8 old[TEST_MC_REF_MAX];
            for (i32 i = 0; i < pieces0 && i < TEST_MC_REF_MAX; i++) old[i] = str8_copy(&t->arena, kill_piece(k0, 0, i));
            String8 newest0 = str8_copy(&t->arena, kill_entry(k0, 0));
            const Command *last = tv.ctx.last_command;
            b32 append = (cmd->flags & COMMAND_KILL) && last && (last->flags & COMMAND_KILL);
            // The real one.
            tv.ctx.codepoint = cmds[pick].cp;
            tv.ctx.shift_translated = cmds[pick].shift;
            test_view_run(&tv, cmd);
            tv.ctx.shift_translated = 0;
            // The reference.
            String8 added[TEST_MC_REF_MAX];
            i32 added_count = 0;
            if (cmd->flags & COMMAND_ONCE) {
                test_mc_ref_once(t, r, cmd, test_settings.max_cursors, last);
            } else {
                i64 lines0[TEST_MC_REF_MAX];
                for (i32 i = 0; i < r->count; i++) lines0[i] = test_mc_line(r->buf, r->c[i].point);
                i64 last_line = -1;
                for (i32 i = 0; i < r->count; i++) {
                    if (r->c[i].swallowed) continue;
                    if (cmd->flags & COMMAND_LINE) {
                        if (lines0[i] == last_line) continue;
                        last_line = lines0[i];
                    }
                    View *ov = one->view;
                    Cursor *oc = &ov->cursors[0];
                    view_set_point(ov, oc, r->c[i].point);
                    buffer_marker_set(r->buf, oc->mark, r->c[i].mark);
                    oc->mark_set = r->c[i].mark_set;
                    oc->mark_active = r->c[i].mark_active;
                    oc->mark_shift = r->c[i].mark_shift;
                    oc->goal_col = r->c[i].goal;
                    KillRing *fk = one->kills;
                    fk->count = 0;
                    if (cmd == &CMD_YANK && count0) {
                        String8 y = pieces0 > 1 && pieces0 == n0 ? old[i] : newest0;
                        u8 *dst = kill_push(fk, y.len);
                        memcpy(dst, y.data, (size_t)y.len);
                    }
                    fk->clip_seq = os_clipboard_seq();
                    r->buf->dev_edit_count = 0;
                    one->ctx.last_command = last;
                    one->ctx.codepoint = cmds[pick].cp;
                    one->ctx.shift_translated = cmds[pick].shift;
                    view_run_command(&one->ctx, cmd);
                    r->c[i].point = view_point(ov, oc);
                    r->c[i].mark = buffer_marker_get(r->buf, oc->mark);
                    r->c[i].mark_set = oc->mark_set;
                    r->c[i].mark_active = oc->mark_active;
                    r->c[i].mark_shift = oc->mark_shift;
                    r->c[i].goal = oc->goal_col;
                    if (cmd->flags & COMMAND_KILL) added[added_count++] = fk->count ? str8_copy(&t->arena, kill_entry(fk, 0)) : STR8_LIT("");
                    for (i64 e = 0; e < r->buf->dev_edit_count; e++) {
                        BufferBatchEdit *ed = &r->buf->dev_edits[e];
                        for (i32 j = 0; j < r->count; j++) {
                            if (j == i) continue;
                            r->c[j].point = test_mc_rule(r->c[j].point, ed->start, ed->start + ed->removed, ed->inserted, 1, &r->c[j].swallowed);
                            r->c[j].mark = test_mc_rule(r->c[j].mark, ed->start, ed->start + ed->removed, ed->inserted, 0, NULL);
                        }
                    }
                }
                if (cmd->flags & (COMMAND_EDIT | COMMAND_KILL)) for (i32 i = 0; i < r->count; i++) r->c[i].mark_active = r->c[i].mark_shift = 0;
            }
            tv.kills->clip_seq = os_clipboard_seq(); // the one-cursor view's kills went to the (fake) clipboard
            i32 before_norm = r->count;
            test_mc_normalize(r);
            merges += before_norm - r->count;
            // Compare: the text, every cursor, the newest kill.
            String8 got = buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf)), want_text = buffer_text(r->buf, &t->arena, 0, buffer_size(r->buf));
            b32 same = str8_equal(got, want_text) && v->cursor_count == r->count && v->primary == r->primary;
            for (i32 i = 0; same && i < r->count; i++) {
                Cursor *c = &v->cursors[i];
                TestMcCursor *x = &r->c[i];
                same = view_point(v, c) == x->point && c->mark_set == x->mark_set && c->mark_active == x->mark_active &&
                       c->mark_shift == x->mark_shift && c->goal_col == x->goal && (!x->mark_set || buffer_marker_get(tv.buf, c->mark) == x->mark);
            }
            if (!same) {
                char *real = test_mc_full(t, v);
                String8 ref = STR8_LIT("");
                for (i32 i = 0; i < r->count; i++) {
                    ref = str8_fmt(&t->arena, "%S%s%s%D", ref, i ? " " : "", i == r->primary ? "*" : "", r->c[i].point);
                    if (r->c[i].mark_set) ref = str8_fmt(&t->arena, "%S,%D%s", ref, r->c[i].mark, r->c[i].mark_active ? "a" : "");
                }
                TEST_CHECK(t, 0, "cursors fuzz step %D (%s%s): text '%S' cursors \"%s\"; reference '%S' \"%S\"; before '%S' (seed 0x%X)", steps,
                           cmd->name, cmds[pick].shift ? ", shifted" : "", got, real, want_text, ref, before, seed);
            }
            // The kill ring.
            if (cmd->flags & COMMAND_KILL) {
                i32 want_count = count0;
                String8 want_pieces[TEST_MC_REF_MAX];
                i32 want_n = 0;
                i64 total = 0;
                for (i32 i = 0; i < added_count; i++) total += added[i].len;
                if (n0 == 1) {
                    if (total) {
                        b32 merge = append && count0 && pieces0 == 1;
                        b32 back = (cmd->flags & COMMAND_KILL_BACKWARD) != 0;
                        want_pieces[0] = merge ? (back ? str8_fmt(&t->arena, "%S%S", added[0], newest0) : str8_fmt(&t->arena, "%S%S", newest0, added[0])) : added[0];
                        want_n = 1;
                        if (!merge) want_count = MIN(count0 + 1, tv.kills->max);
                    }
                } else if (total) {
                    b32 merge = append && count0 && pieces0 == added_count;
                    b32 back = (cmd->flags & COMMAND_KILL_BACKWARD) != 0;
                    for (i32 i = 0; i < added_count; i++) {
                        want_pieces[i] = !merge ? added[i] : back ? str8_fmt(&t->arena, "%S%S", added[i], old[i]) : str8_fmt(&t->arena, "%S%S", old[i], added[i]);
                    }
                    want_n = added_count;
                    if (!merge) want_count = MIN(count0 + 1, tv.kills->max);
                }
                b32 ok = tv.kills->count == want_count;
                if (want_n) {
                    ok = ok && kill_pieces(tv.kills, 0) == want_n;
                    for (i32 i = 0; ok && i < want_n; i++) ok = str8_equal(kill_piece(tv.kills, 0, i), want_pieces[i]);
                }
                TEST_CHECK(t, ok, "cursors fuzz step %D (%s): the kill ring: %d entries, %d pieces, newest '%S'; expected %d entries, %d pieces "
                           "(seed 0x%X)", steps, cmd->name, tv.kills->count, kill_pieces(tv.kills, 0), kill_entry(tv.kills, 0), want_count, want_n, seed);
                kills++;
            }
            // The invariants.
            for (i32 i = 0; i < v->cursor_count; i++) {
                i64 p = view_point(v, &v->cursors[i]);
                TEST_CHECK(t, buffer_snap_char(tv.buf, p) == p && (i == 0 || p > view_point(v, &v->cursors[i - 1])) && v->cursor_count <= test_settings.max_cursors,
                           "cursors fuzz step %D: invariant at cursor %d (seed 0x%X)", steps, i, seed);
            }
            max_seen = MAX(max_seen, v->cursor_count);
        }
        if (!test_view_close(t, &tv)) return 0;
        epochs++;
    }
    view_destroy(one->view);
    kill_destroy(one->kills);
    i64 live = r->buf->marker_live;
    buffer_destroy(r->buf);
    TEST_CHECK(t, live == 0, "cursors fuzz: %D reference markers live", live);
    LOG("test: ok: multiple-cursor differential fuzz (%D commands of %d kinds in %D epochs against the single-cursor commands run "
        "cursor by cursor on a flat text and normalized naively; %D merges, %D kills compared, up to %D cursors; text, every cursor "
        "and the newest kill after each command; seed 0x%X)", steps, (i32)ARRAY_COUNT(cmds), epochs, merges, kills, max_seen, seed);
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

// One step of the random undo sequence: a motion, then an edit (newline, a typed character, or a
// delete-backward-char; a refused delete is followed by a newline, so every step edits).
typedef struct TestUndoOp {
    u8 motion;
    u8 action; // 0 newline, 1 self-insert ch, 2 delete-backward-char (a newline while the text has 4 bytes or fewer)
    u8 ch;
} TestUndoOp;

static const Command *test_undo_motions[] = { &CMD_FORWARD_CHAR, &CMD_BACKWARD_WORD, &CMD_NEXT_LINE, &CMD_MOVE_END_OF_LINE };

// Runs the steps, then as many undos: each must give the text after the step before it, and the last
// one the unmodified start. 0 when it holds; otherwise 1, with the first difference in `why`.
static b32 test_undo_replay(Test *t, const TestUndoOp *ops, i32 count, char *why) {
    u64 mark = arena_pos(&t->arena);
    TestView tv;
    test_view_open(t, &tv, "one two\nthree |four\nfive", 10, 40);
    char **texts = PUSH_ARRAY(&t->arena, char *, count + 1);
    texts[0] = test_view_marked(t, &tv);
    for (i32 k = 1; k <= count; k++) {
        const TestUndoOp *op = &ops[k - 1];
        test_view_run(&tv, test_undo_motions[op->motion]);
        i64 size = buffer_size(tv.buf);
        String8 before = buffer_text(tv.buf, &t->arena, 0, size);
        switch (op->action) {
        case 0: test_view_run(&tv, &CMD_NEWLINE); break;
        case 1: test_type_char(&tv, op->ch); break;
        case 2: test_view_run(&tv, size > 4 ? &CMD_DELETE_BACKWARD_CHAR : &CMD_NEWLINE); break;
        }
        // A refused delete (at the start of the buffer) changed nothing: the text decides, not the marked
        // text, since the motion may have moved point (seeds 22 and 33 counted such a step as an edit).
        if (buffer_size(tv.buf) == size && str8_equal(buffer_text(tv.buf, &t->arena, 0, size), before)) {
            test_view_run(&tv, &CMD_NEWLINE);
        }
        texts[k] = test_view_marked(t, &tv);
    }
    b32 failed = 0;
    for (i32 n = 1; n <= count && !failed; n++) {
        test_view_run(&tv, &CMD_UNDO);
        String8 text = buffer_text(tv.buf, &t->arena, 0, buffer_size(tv.buf));
        String8 expected = str8_cstr(texts[count - n]);
        // Compare the text only (point is where that command started, checked elsewhere).
        u8 *plain = PUSH_ARRAY(&t->arena, u8, expected.len);
        i64 m = 0;
        for (i64 i = 0; i < expected.len; i++) if (expected.data[i] != '|') plain[m++] = expected.data[i];
        if (!str8_equal(text, str8(plain, m))) {
            failed = 1;
            if (why) test_cstr(why, 256, "after %d undo(s) the text is '%S', not the text after step %d '%s' (after the steps '%s')", n,
                               text, count - n, texts[count - n], texts[count]);
        }
    }
    if (!failed && tv.buf->modified) {
        failed = 1;
        if (why) test_cstr(why, 256, "undoing everything leaves the buffer modified");
    }
    test_view_close(t, &tv);
    arena_pop_to(&t->arena, mark);
    return failed;
}

// ddmin over the steps: removes chunks while the sequence still fails, down to single steps. Returns
// the new count; `ops` holds the reduced sequence.
static i32 test_undo_reduce(Test *t, TestUndoOp *ops, i32 count) {
    TestUndoOp trial[64];
    ASSERT(count <= (i32)ARRAY_COUNT(trial));
    for (i32 chunk = count / 2; chunk >= 1;) {
        b32 removed = 0;
        for (i32 start = 0; start + chunk <= count;) {
            i32 n = 0;
            for (i32 i = 0; i < count; i++) if (i < start || i >= start + chunk) trial[n++] = ops[i];
            if (n > 0 && test_undo_replay(t, trial, n, NULL)) {
                memcpy(ops, trial, sizeof(TestUndoOp) * (size_t)n);
                count = n;
                removed = 1;
            } else {
                start += chunk;
            }
        }
        if (!removed) chunk /= 2;
        else chunk = MIN(chunk, count / 2);
    }
    return count;
}

static const char *test_undo_ops_text(Test *t, const TestUndoOp *ops, i32 count) {
    String8 s = { 0 };
    for (i32 i = 0; i < count; i++) {
        const TestUndoOp *op = &ops[i];
        const char *edit = op->action == 0 ? "newline" : op->action == 1 ? "self-insert" : "delete-backward-char";
        String8 step = op->action == 1 ? str8_fmt(&t->arena, "%s%s, %s %c", i ? "; " : "", test_undo_motions[op->motion]->name, edit, op->ch)
                                       : str8_fmt(&t->arena, "%s%s, %s", i ? "; " : "", test_undo_motions[op->motion]->name, edit);
        s = str8_fmt(&t->arena, "%S%S", s, step);
    }
    return (const char *)str8_fmt(&t->arena, "%S%c", s, 0).data;
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

    // K commands then N undos gives the text after command K - N. A failing sequence is reduced to a
    // smallest one that still fails (ddmin), which the failure message shows.
    enum { K = 30 };
    // Seeds 22 and 33 reduced: the last backward-word reaches the start of the buffer, where the delete is
    // refused ("Beginning of buffer") and nothing changes; one undo then undoes the second newline.
    {
        if (!test_view_open(t, &tv, "one two\nthree |four\nfive", 10, 40)) return 0;
        test_view_run(&tv, &CMD_BACKWARD_WORD);
        test_view_run(&tv, &CMD_NEWLINE);
        test_view_run(&tv, &CMD_BACKWARD_WORD);
        test_view_run(&tv, &CMD_NEWLINE);
        test_view_run(&tv, &CMD_BACKWARD_WORD);
        test_view_run(&tv, &CMD_DELETE_BACKWARD_CHAR);
        TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "|one \ntwo\n\nthree four\nfive") && test_echo_is(&tv, "Beginning of buffer"),
                   "undo: seeds 22 / 33: the refused delete changed something (%s)", test_view_marked(t, &tv));
        test_view_run(&tv, &CMD_UNDO);
        TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("one two\n\nthree four\nfive")), "undo: seeds 22 / 33: one undo is not the second newline");
        test_view_run(&tv, &CMD_UNDO);
        TEST_CHECK(t, test_text_is(t, tv.buf, STR8_LIT("one two\nthree four\nfive")) && !tv.buf->modified, "undo: seeds 22 / 33: two undos");
        if (!test_view_close(t, &tv)) return 0;
        static const TestUndoOp reduced[] = { { 1, 0, 0 }, { 1, 0, 0 }, { 1, 2, 0 } };
        TEST_CHECK(t, !test_undo_replay(t, reduced, ARRAY_COUNT(reduced), NULL), "undo: seeds 22 / 33: the reduced steps fail");
    }
    TestUndoOp ops[K];
    t->rng = seed ^ 0x1d0;
    for (i32 k = 0; k < K; k++) {
        ops[k].motion = (u8)test_below(t, ARRAY_COUNT(test_undo_motions));
        ops[k].action = (u8)test_below(t, 3);
        ops[k].ch = ops[k].action == 1 ? "abc"[test_below(t, 3)] : 0;
    }
    if (test_undo_replay(t, ops, K, NULL)) {
        i32 n = test_undo_reduce(t, ops, K);
        char why[256];
        test_undo_replay(t, ops, n, why);
        TEST_CHECK(t, 0, "undo: random commands (seed 0x%X), reduced to %d step(s): %s; %s", seed, n, test_undo_ops_text(t, ops, n), why);
    }

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
    String8 clip = { 0 };
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

// The glyph atlas grows its rows on demand: it starts at FONT_ATLAS_ROWS, ASCII inside them, and the glyphs that
// do not fit make it taller (up to its width) without a reset; every glyph lies inside the rows and is drawn there.
static b32 test_font_atlas(Test *t) {
    FontParams params = { STR8_LIT("Consolas"), 12.0f, 100, FB_RENDER_NATURAL_SYMMETRIC };
    Font *f = font_create(&t->arena, &params, 1.0f);
    TEST_CHECK(t, f, "font atlas: font_create failed");
    i32 first = f->atlas_h;
    u32 resets = f->reset_count;
    TEST_CHECK(t, first == MIN(FONT_ATLAS_ROWS, f->atlas_size) && f->atlas_size >= first, "font atlas: %d rows of %d at first", first,
               f->atlas_size);
    i32 drawn = 0;
    FontGlyph last = { 0 };
    for (u32 cp = 0x100; cp < 0x530 && f->atlas_h == first; cp++) { // Latin Extended, IPA, Greek, Cyrillic
        FontGlyph g = font_lookup(f, NULL, cp);
        if (g.w && (g.u != f->missing.u || g.v != f->missing.v)) {
            drawn++;
            last = g;
            TEST_CHECK(t, g.v + g.h <= f->atlas_h, "font atlas: U+%x at row %d, height %d, outside %d rows", cp, (i32)g.v, (i32)g.h, f->atlas_h);
        }
    }
    TEST_CHECK(t, f->atlas_h > first && f->atlas_h <= f->atlas_size && f->needs_bind && f->reset_count == resets,
                  "font atlas: %d glyphs, %d rows (from %d), needs_bind %d, resets %u", drawn, f->atlas_h, first, f->needs_bind,
                  f->reset_count - resets);
    b32 inked = 0;
    for (i32 y = last.v; y < last.v + last.h && !inked; y++) {
        for (i32 x = last.u; x < last.u + last.w && !inked; x++) inked = f->atlas[((i64)y * f->atlas_size + x) * 4] != 0;
    }
    TEST_CHECK(t, inked, "font atlas: the last glyph (row %d) has no coverage", (i32)last.v);
    i32 refs = font_shutdown(f);
    TEST_CHECK(t, refs == 0, "font atlas: %d DirectWrite references left", refs);
    LOG("test: ok: font atlas (%d rows at first, %d after %d glyphs, no reset, the last glyph inked)", first, f->atlas_h, drawn);
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
    // A line longer than SYNTAX_MATCH_MAX: no match even for neighbours on it (answered without lexing the line).
    u8 *long_line = PUSH_ARRAY(&t->arena, u8, n + 3);
    long_line[0] = 1;
    long_line[1] = '(';
    long_line[2] = ')';
    for (i64 i = 3; i < n + 3; i++) long_line[i] = 'x';
    if (!test_match_case(t, BUFFER_LANG_C, "a line longer than the budget", str8(long_line, n + 3))) return 0;
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
    // Three cursors completing three labels: every line reindented after the insertions (Command.after), in one batch
    // flush for the command instead of one a cursor; one undo for all of them.
    {
        TestView tv;
        if (!test_view_open(t, &tv, "switch (x) {\n        case 1|\n        case 2|\n    a();\n        case 3|\n}", 10, 60)) return 0;
        tv.buf->language = BUFFER_LANG_C;
        test_view_run(&tv, &CMD_FORWARD_CHAR);
        test_view_run(&tv, &CMD_BACKWARD_CHAR);
        u64 flushes = tv.buf->dev_batch_flushes;
        test_type_char(&tv, ':');
        char *got = test_view_marked(t, &tv);
        TEST_CHECK(t, test_cstr_equal(got, "switch (x) {\n    case 1:|\n    case 2:|\n    a();\n    case 3:|\n}") &&
                          tv.buf->dev_batch_flushes - flushes == 1,
                   "electric label: three cursors: got '%s', %U batch flushes", got, tv.buf->dev_batch_flushes - flushes);
        test_view_run(&tv, &CMD_UNDO);
        TEST_CHECK(t, test_cstr_equal(test_view_marked(t, &tv), "switch (x) {\n        case 1|\n        case 2|\n    a();\n        case 3|\n}"),
                   "electric label: three cursors: one undo");
        if (!test_view_close(t, &tv)) return 0;
    }
    LOG("test: ok: electric labels, %d cases with their undo; three cursors in one flush", (i32)ARRAY_COUNT(cases));
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
    SearchPattern *pat = PUSH_STRUCT(&t->arena, SearchPattern);
    TestRef ref = { 0 };
    ref.cap = KB(4);
    ref.data = PUSH_ARRAY(&t->arena, u8, ref.cap);
    i64 searches = 0, found = 0, straddled = 0, sliced_runs = 0, core_runs = 0, second_runs = 0;
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
            // The core on the plain bytes: the same answer.
            {
                i64 clo = CLAMP(lo, 0, ref.len), chi = CLAMP(hi, clo, ref.len), cfrom = CLAMP(from, clo, chi);
                search_pattern_init(pat, needle, fold);
                i64 core = forward ? search_bytes_forward(pat, ref.data, 0, ref.len, cfrom, chi)
                                   : search_bytes_backward(pat, ref.data, 0, ref.len, clo, cfrom);
                TEST_CHECK(t, core == want, "search: core: round %d query %d (seed 0x%X): '%S' fold %d forward %d from %D in [%D, %D): got %D, want %D",
                           round, q, seed, needle, fold, forward, from, lo, hi, core, want);
                core_runs++;
                second_runs += pat->second_on;
            }
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
        "%D on plain bytes (the core; %D with the second-byte filter), %d fixed cases, restart after an edit, needle limits, "
        "Turkish i (known limit), smart case, first bytes", searches, found, straddled, sliced_runs, core_runs, second_runs,
        (i32)ARRAY_COUNT(fixed));
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

// The rules of a description (Phase 18): one line of English, at most 60 bytes, printable ASCII, starting with an
// uppercase verb from this list, no trailing period, no leading or trailing space. Returns the broken rule, or NULL.
static const char *test_description_rule(String8 d) {
    static const char *const verbs[] = {
        "Abort", "Accept", "Add", "Answer", "Balance", "Cancel", "Capitalize", "Comment", "Complete", "Convert", "Copy",
        "Delete", "Describe", "End", "Enlarge", "Exchange", "Find", "Go", "Indent", "Insert", "Join", "Kill", "List",
        "Make", "Mark", "Move", "Open", "Quit", "Read", "Recover", "Redo", "Reload", "Remove", "Replace", "Reset",
        "Revert", "Run", "Save", "Scan", "Scroll", "Search", "Select", "Set", "Show", "Shrink", "Skip", "Split", "Stop",
        "Switch", "Toggle", "Transpose", "Undo", "Use", "Visit", "Write", "Yank",
    };
    if (d.len == 0) return "empty";
    if (d.len > 60) return "longer than 60 bytes";
    for (i64 i = 0; i < d.len; i++) {
        if (d.data[i] < 0x20 || d.data[i] > 0x7e) return "not printable ASCII";
    }
    if (d.data[0] == ' ' || d.data[d.len - 1] == ' ') return "a leading or trailing space";
    if (d.data[d.len - 1] == '.') return "a trailing period";
    if (d.data[0] < 'A' || d.data[0] > 'Z') return "not starting with an uppercase letter";
    i64 w = 0;
    while (w < d.len && d.data[w] != ' ') w++;
    String8 first = str8(d.data, w);
    for (i32 i = 0; i < (i32)ARRAY_COUNT(verbs); i++) {
        if (str8_equal(first, str8_cstr(verbs[i]))) return NULL;
    }
    return "not starting with a verb of the list";
}

// Every command of the table has a description that keeps the rules; the rules themselves refuse what they must.
static b32 test_command_descriptions(Test *t) {
    i32 n = command_count();
    i64 longest = 0;
    for (i32 i = 0; i < n; i++) {
        const Command *c = command_at(i);
        String8 d = str8_cstr(c->description ? c->description : "");
        const char *broken = test_description_rule(d);
        TEST_CHECK(t, !broken, "descriptions: '%s': \"%S\": %s", c->name, d, broken);
        longest = MAX(longest, d.len);
    }
    static const char *const bad[] = {
        "", "move point forward one character", "Move point forward one character.", " Move point", "Move point ",
        "Moves point forward one character", "Move point forward one character, then another one and one more",
        "Move point\tforward",
    };
    for (i32 i = 0; i < (i32)ARRAY_COUNT(bad); i++) {
        TEST_CHECK(t, test_description_rule(str8_cstr(bad[i])), "descriptions: \"%s\" must break a rule", bad[i]);
    }
    TEST_CHECK(t, !test_description_rule(STR8_LIT("Move point forward one character")), "descriptions: a good one refused");
    LOG("test: ok: command descriptions (%d commands, the longest %D bytes; %d broken ones refused)", n, longest,
        (i32)ARRAY_COUNT(bad));
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

// keymap_where_is's sequences as "C-f, <right>" (all it filled) and its total.
static String8 test_where_is(Test *t, Keymap **stack, i32 count, const Command *cmd, i32 cap, i32 *total) {
    KeySeq seqs[8];
    *total = keymap_where_is(stack, count, cmd, seqs, cap);
    u8 *out = PUSH_ARRAY(&t->arena, u8, 8 * (KEY_SEQ_TEXT_CAP + 2));
    i64 n = 0;
    for (i32 i = 0; i < MIN(*total, cap); i++) {
        if (i) { out[n++] = ','; out[n++] = ' '; }
        n += key_seq_print(&seqs[i], out + n, KEY_SEQ_TEXT_CAP);
    }
    return str8(out, n);
}

// where-is (Phase 18): bound once, twice, three times with room for two, not at all; shadowed by an earlier map,
// unreachable behind a prefix bound in an earlier map, bound in two maps to the same command; after a config
// that rebinds the command.
static b32 test_keymap_where_is(Test *t) {
    Config *c = PUSH_STRUCT(&t->arena, Config);
    config_init(c);
    config_parse(c, &t->arena, config_default_text(), STR8_LIT("defaults"));
    Keymap *global[] = { &c->global };
    Keymap *results[] = { &c->search_results, &c->global };
    struct { Keymap **stack; i32 count; const Command *cmd; i32 cap; i32 total; const char *want; } cases[] = {
        { global, 1, &CMD_SAVE_BUFFER, 2, 1, "C-x C-s" },
        { global, 1, &CMD_FORWARD_CHAR, 2, 2, "C-f, <right>" },
        { global, 1, &CMD_GOTO_LINE, 2, 2, "M-g g, M-g M-g" },
        { global, 1, &CMD_UNDO, 2, 3, "C-/, C-_" },              // C-x u has more chords
        { global, 1, &CMD_UNDO, 8, 3, "C-/, C-_, C-x u" },
        { global, 1, &CMD_UNDO, 0, 3, "" },
        { global, 1, &CMD_SKIP_TO_NEXT_LIKE_THIS, 2, 0, "" },
        { global, 1, &CMD_NEWLINE, 8, 3, "RET, C-m, C-j" },  // equal lengths: the config order
        { results, 2, &CMD_NEWLINE, 8, 1, "C-j" },                 // RET and C-m run compile-goto-error there
        { results, 2, &CMD_COMPILE_GOTO_ERROR, 8, 2, "RET, C-m" },
        { results, 2, &CMD_RECOMPILE, 8, 1, "g" },
    };
    for (i32 i = 0; i < (i32)ARRAY_COUNT(cases); i++) {
        i32 total;
        String8 got = test_where_is(t, cases[i].stack, cases[i].count, cases[i].cmd, cases[i].cap, &total);
        TEST_CHECK(t, total == cases[i].total && str8_equal(got, str8_cstr(cases[i].want)), "where-is: %s (cap %d): %d '%S', expected %d '%s'",
                   cases[i].cmd->name, cases[i].cap, total, got, cases[i].total, cases[i].want);
    }
    // A prefix bound in an earlier map hides the sequences behind it; a sequence bound in two maps counts once.
    Keymap *front = PUSH_STRUCT(&t->arena, Keymap), *back = PUSH_STRUCT(&t->arena, Keymap);
    test_bind(front, "C-c", &CMD_END_OF_BUFFER);
    test_bind(front, "C-q", &CMD_YANK);
    test_bind(back, "C-c r", &CMD_RELOAD_CONFIG);
    test_bind(back, "C-q", &CMD_YANK);
    test_bind(back, "C-y", &CMD_YANK);
    Keymap *both[] = { front, back };
    i32 total;
    String8 got = test_where_is(t, both, 2, &CMD_RELOAD_CONFIG, 2, &total);
    TEST_CHECK(t, total == 0, "where-is: C-c r behind a bound C-c: %d '%S'", total, got);
    got = test_where_is(t, both, 2, &CMD_YANK, 8, &total);
    TEST_CHECK(t, total == 2 && str8_equal(got, STR8_LIT("C-q, C-y")), "where-is: yank in two maps: %d '%S'", total, got);
    got = test_where_is(t, both + 1, 1, &CMD_RELOAD_CONFIG, 2, &total);
    TEST_CHECK(t, total == 1 && str8_equal(got, STR8_LIT("C-c r")), "where-is: C-c r alone: %d '%S'", total, got);
    // A config that moves save-buffer.
    config_parse(c, &t->arena, STR8_LIT("[keys]\nC-c s save-buffer\nC-x C-s none\n"), STR8_LIT("t.conf"));
    got = test_where_is(t, global, 1, &CMD_SAVE_BUFFER, 2, &total);
    TEST_CHECK(t, total == 1 && str8_equal(got, STR8_LIT("C-c s")), "where-is: save-buffer rebound: %d '%S'", total, got);
    LOG("test: ok: where-is (%d cases on the default keys, shadowed, behind a bound prefix, two maps, rebound)",
        (i32)ARRAY_COUNT(cases));
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
    String8 path = str8_fmt(&t->arena, "%S\\ronin.conf", dir);
    String8 temp = str8_fmt(&t->arena, "%S\\ronin.conf.tmp", dir);
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
    TEST_CHECK(t, c->diags.errors == 2 && c->diags.warnings == 1 && c->settings.completion_lines == 40 && !c->settings.auto_revert &&
                  c->theme.prompt == 0x123456, "config minibuffer: %d errors, %d warnings, completion_lines %d", c->diags.errors,
               c->diags.warnings, c->settings.completion_lines);
    TEST_CHECK(t, str8_equal(c->diags.first->text, STR8_LIT("t.conf:7: unknown section [keys bogus]")) &&
                  str8_equal(c->diags.first->next->text, STR8_LIT("t.conf:9: unknown section [keysminibuffer]")),
               "config minibuffer: diagnostics '%S'", c->diags.first->text);
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
    TEST_CHECK(t, c->diags.errors == 0 && c->diags.warnings == 0, "config: defaults: %d errors, %d warnings", c->diags.errors, c->diags.warnings);
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
        b32 clamped = w->settings.startup_windows == 2 && w->diags.warnings == 1;
        config_parse(w, &t->arena, STR8_LIT("[colors]\nmode_line_inactive_background = #303030\n"), STR8_LIT("b.conf"));
        config_parse(w, &t->arena, STR8_LIT("[colors]\nbackground = #404040\n"), STR8_LIT("c.conf"));
        b32 kept = w->theme.mode_line_inactive_background == 0x303030 && w->theme.mode_line_inactive_text == 0x202020;
        arena_pop_to(&t->arena, mark);
        TEST_CHECK(t, follows && clamped && kept, "config: derived inactive mode line colors (%d %d %d), startup_windows clamped",
                   follows, clamped, kept);
    }
    // The modal colors (Phase 18; past the 32nd color): their defaults, the two derived ones, one set and kept; the
    // [keys command-list] section binds in its own map.
    {
        u64 mark = arena_pos(&t->arena);
        Config *w = PUSH_STRUCT(&t->arena, Config);
        config_init(w);
        config_parse(w, &t->arena, config_default_text(), STR8_LIT("<built-in>"));
        Theme *m = &w->theme;
        b32 defaults = m->modal_background == 0x0c3434 && m->modal_border == 0x2e8b8b && m->modal_description == 0xa8977a &&
                       m->modal_selection == 0x0000ff && m->modal_key == 0x7ad0c6;
        config_parse(w, &t->arena, STR8_LIT("[colors]\nselection = #111111\nnumber = #222222\nmodal_key = #333333\n"
                                            "[keys command-list]\nC-n next-line\n"), STR8_LIT("a.conf"));
        config_parse(w, &t->arena, STR8_LIT("[colors]\nnumber = #444444\n"), STR8_LIT("b.conf"));
        KeySeq seq;
        const char *error;
        key_seq_parse(STR8_LIT("C-n"), &seq, &error);
        b32 derived = m->modal_selection == 0x111111 && m->modal_key == 0x333333 && m->search_line_number == 0x444444;
        KeySeq down;
        key_seq_parse(STR8_LIT("<down>"), &down, &error);
        b32 keys = keymap_get(&w->command_list, &seq) == &CMD_NEXT_LINE && keymap_get(&w->command_list, &down) == &CMD_MODAL_NEXT &&
                   w->command_list.count == 10 &&
                   str8_equal(str8_cstr(w->command_list.name), STR8_LIT("command-list")) && w->diags.errors == 0;
        arena_pop_to(&t->arena, mark);
        TEST_CHECK(t, defaults && derived && keys, "config: modal colors: defaults %d, derived and kept %d, [keys command-list] %d",
                   defaults, derived, keys);
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
    TEST_CHECK(t, c->diags.errors == 0 && c->diags.warnings == 0, "config: valid user file: %d errors, %d warnings", c->diags.errors, c->diags.warnings);
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
        "t.conf:1: line outside a section ([settings], [colors], [keys], [keys minibuffer], [keys isearch], [keys search-results], [keys compilation] or [keys command-list])",
        "t.conf:3: font_size: 'big' is not a number",
        "t.conf:4: unknown setting 'nosuch'",
        "t.conf:5: expected name = value",
        "t.conf:7: background: '123456' is not a color (#rrggbb)",
        "t.conf:8: unknown color 'purple'",
        "t.conf:10: bad key sequence 'C-xy': more than one character (separate chords with spaces)",
        "t.conf:11: unknown command 'no-such-command'",
        "t.conf:12: expected a key sequence and a command",
        "t.conf:13: unknown section [bogus]",
        "t.conf:15: bad section header (expected [settings], [colors], [keys], [keys minibuffer], [keys isearch], [keys search-results], [keys compilation] or [keys command-list])",
        "t.conf:17: warning: line_height 50 is out of range (80 to 300), using 80",
        "t.conf:18: warning: font_size 200 is out of range (4 to 96), using 96",
        "t.conf:19: render_mode: 'fancy' is not symmetric, natural or classic",
        "t.conf:20: warning: tab_width 0 is out of range (1 to 16), using 1",
        "t.conf:21: fsync_on_save: 'yes' is not true or false",
        "t.conf:24: warning: C-c q x removed 1 binding it conflicts with (one sequence is a prefix of the other)",
        "t.conf:25: bad key sequence 'font =': more than one character (separate chords with spaces)",
    };
    i32 k = 0;
    for (ConfigDiag *d = c->diags.first; d; d = d->next, k++) {
        TEST_CHECK(t, k < ARRAY_COUNT(expected) && str8_equal(d->text, str8_cstr(expected[k])),
                   "config: diagnostic %d is '%S', expected '%s'", k, d->text, k < ARRAY_COUNT(expected) ? expected[k] : "(none)");
    }
    TEST_CHECK(t, k == ARRAY_COUNT(expected) && c->diags.errors == 14 && c->diags.warnings == 4,
               "config: %d diagnostics (%d errors, %d warnings), expected %d", k, c->diags.errors, c->diags.warnings, (i32)ARRAY_COUNT(expected));
    TEST_CHECK(t, c->settings.line_height == 80 && c->settings.font_size == 96.0f && c->settings.tab_width == 1,
               "config: clamped values are applied");
    TEST_CHECK(t, !test_binding(c, "C-c q") && test_binding(c, "C-c q x") == &CMD_BACKWARD_CHAR, "config: the later binding wins");

    // Files: the user file layered over the defaults; a missing file leaves the defaults.
    String8 path = str8_fmt(&t->arena, "%S\\cfg_user.conf", t->tmp_dir);
    String8 user = STR8_LIT("[keys]\nC-x C-s none\n[colors]\ncursor = #ff0000\n");
    TEST_CHECK(t, os_write_file(path, user), "config: writing %S", path);
    OsFileInfo info;
    TEST_CHECK(t, config_load(c, &t->arena, path, &info) == OS_FILE_OK && info.size == user.len && c->diags.errors == 0 &&
                  !test_binding(c, "C-x C-s") && test_binding(c, "C-f") == &CMD_FORWARD_CHAR && c->theme.cursor == 0xff0000 &&
                  c->theme.background == 0x072626 && c->settings.font_size == 12.0f, "config: load layers the user file over the defaults");
    String8 missing = str8_fmt(&t->arena, "%S\\cfg_missing.conf", t->tmp_dir);
    os_file_delete(missing);
    TEST_CHECK(t, config_load(c, &t->arena, missing, &info) == OS_FILE_NOT_FOUND && test_binding(c, "C-x C-s") == &CMD_SAVE_BUFFER &&
                  config_load(c, &t->arena, str8(NULL, 0), &info) == OS_FILE_NOT_FOUND && c->diags.errors == 0,
               "config: no user file gives the defaults");
    TEST_CHECK(t, str8_equal(config_file_name(STR8_LIT("C:\\a\\b\\ronin.conf")), STR8_LIT("ronin.conf")), "config: file name");

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
        for (ConfigDiag *d = c->diags.first; d; d = d->next) stored++;
        TEST_CHECK(t, stored == MIN((i64)(c->diags.errors + c->diags.warnings), (i64)CONFIG_DIAG_CAP) && c->global.count <= KEYMAP_CAP,
                   "config: fuzz iteration %d: %D diagnostics stored of %d", iter, stored, c->diags.errors + c->diags.warnings);
        diags += c->diags.errors + c->diags.warnings;
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

// --bench-cursors: a C file of TEST_BENCH_CURSOR_BLOCKS blocks of 8 lines, each with a line for every class measured
// (made once): 0 "void fN(int a) {", 1 "    switch (a) {", 2 "    case 1" (':' at its end), 3 "x = 1;" (TAB,
// comment-line), 4 eight spaces (RET at its end: a whitespace-only line), 5 "" ('}' first on the line), 6 "    }",
// 7 "}".
#define TEST_BENCH_CURSOR_BLOCKS 12000

String8 test_bench_cursors_c(Arena *arena, String8 tmp_dir) {
    String8 path = str8_fmt(arena, "%S\\bench_cursors.c", tmp_dir);
    OsFileInfo info;
    if (os_file_info(path, &info) == OS_FILE_OK && info.size > 0) return path;
    Arena scratch = arena_create(MB(64));
    String8 text = STR8_LIT("");
    u8 *out = PUSH_ARRAY(&scratch, u8, MB(32));
    i64 n = 0;
    for (i32 b = 0; b < TEST_BENCH_CURSOR_BLOCKS; b++) {
        n += fmt_buf(out + n, MB(1), "void f%d(int a) {\n    switch (a) {\n    case 1\nx = 1;\n        \n\n    }\n}\n", b);
    }
    text = str8(out, n);
    if (!os_write_file(path, text)) LOG("bench-cursors: cannot write %S", path);
    os_release(scratch.base);
    return path;
}

// --bench-cursors: 1,000,000 lines of 64 bytes, each with "needle" once (made once).
String8 test_bench_cursors_words(Arena *arena, String8 tmp_dir) {
    String8 path = str8_fmt(arena, "%S\\bench_cursors_words.txt", tmp_dir);
    OsFileInfo info;
    if (os_file_info(path, &info) == OS_FILE_OK && info.size >= 64 * 1000000) return path;
    Arena scratch = arena_create(MB(128));
    u8 *out = PUSH_ARRAY(&scratch, u8, 64 * 1000000);
    String8 line = STR8_LIT("some ordinary words around a needle, then more words to fill it\n");
    for (i64 i = 0; i < 1000000; i++) memcpy(out + i * 64, line.data, 64);
    if (!os_write_file(path, str8(out, 64 * 1000000))) LOG("bench-cursors: cannot write %S", path);
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

static b32 test_current_is(App *app, const char *name);

static String8 test_echo(App *app);
static String8 test_results_conf(Test *t, const char *name, const char *text);

// The multiple-cursor commands through the app: the default keys; mark-all over frames on a large buffer (keys
// typed meanwhile wait, C-g stops it and adds nothing); refused in the minibuffer by key and by M-x.
static b32 test_mc_app(Test *t) {
    // ~1.2 MB: "needle" on every 1000th line, the rest without it.
    Arena *a = &t->arena;
    i64 lines = 20000, n = 0;
    u8 *text = PUSH_ARRAY(a, u8, lines * 64);
    for (i64 i = 0; i < lines; i++) {
        String8 l = i % 1000 == 999 ? STR8_LIT("a line with the needle in it, and more text after it to fill..\n")
                                    : STR8_LIT("an ordinary line of text that has nothing to find in it at all\n");
        memcpy(text + n, l.data, (size_t)l.len);
        n += l.len;
    }
    App *app = test_search_app(t, str8(text, n), 0);
    TEST_CHECK(t, app, "cursors app: no app");
    View *v = app_selected_view(app);
    // The keys: C-> and C-< on a region, C-S-c C-S-c on lines, C-c C-< over frames.
    i64 first = 999 * 63 + 16; // "needle" on line 999
    view_set_mark(v, view_primary(v), first, 1);
    view_set_point(v, view_primary(v), first + 6);
    app_dev_feed(app, "C->", &t->arena);
    TEST_CHECK(t, v->cursor_count == 2 && view_point(v, view_primary(v)) == 1999 * 63 + 22, "cursors app: C-> gave %d cursors", v->cursor_count);
    app_dev_feed(app, "C-<", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "No more matches") && v->cursor_count == 2, "cursors app: C-< at the first");
    app_dev_feed(app, "C-g C-g", &t->arena);
    TEST_CHECK(t, v->cursor_count == 1, "cursors app: C-g C-g left %d cursors", v->cursor_count);
    view_set_mark(v, view_primary(v), first, 1);
    view_set_point(v, view_primary(v), first + 6);
    app->dev_work_budget = KB(64);
    app_dev_feed(app, "C-c C-<", &t->arena);
    TEST_CHECK(t, cursor_job_pending(&app->cursor_job) && v->cursor_count == 1, "cursors app: mark-all ended in its first slice");
    app_dev_feed(app, "x C-n", &t->arena); // waits: nothing typed, no motion
    i32 frames = 1;
    while (cursor_job_pending(&app->cursor_job) && frames < 1000) {
        app_dev_feed_events(app, NULL, 0, &t->arena);
        frames++;
    }
    String8 now = test_app_text(t, app);
    TEST_CHECK(t, v->cursor_count == 20 && test_app_echo_is(app, "Marked 20 occurrences") && frames > 5 && str8_equal(now, str8(text, n)),
               "cursors app: mark-all gave %d cursors in %d frames, echo '%S'", v->cursor_count, frames, test_echo(app));
    // C-g while it runs: nothing added.
    app_dev_feed(app, "C-g C-g", &t->arena);
    view_set_mark(v, view_primary(v), first, 1);
    view_set_point(v, view_primary(v), first + 6);
    app_dev_feed(app, "C-c C-<", &t->arena);
    app_dev_feed(app, "C-g", &t->arena);
    TEST_CHECK(t, !cursor_job_pending(&app->cursor_job) && v->cursor_count == 1 && test_app_echo_is(app, "Quit"), "cursors app: C-g during mark-all");
    app->dev_work_budget = 0;
    // edit-lines by its key.
    view_set_mark(v, view_primary(v), 0, 1);
    view_set_point(v, view_primary(v), 3 * 63 + 5);
    app_dev_feed(app, "C-S-c C-S-c", &t->arena);
    TEST_CHECK(t, v->cursor_count == 4 && view_point(v, view_primary(v)) == 3 * 63 + 5, "cursors app: C-S-c C-S-c gave %d", v->cursor_count);
    app_dev_feed(app, "C-g", &t->arena);
    // In the minibuffer: refused by their keys (M-x does not run inside a prompt: the commands without keys are bound
    // here), one cursor, the input unchanged.
    String8 conf = test_results_conf(t, "p14keys.conf", "[keys]\nC-c 1 skip-to-next-like-this\nC-c 2 unmark-next-like-this\n"
                                                        "C-c 3 edit-ends-of-lines\nC-c 4 edit-beginnings-of-lines\n");
    app_dev_use_config(app, conf);
    static const char *keys[] = { "C->", "C-<", "C-c C-<", "C-S-c C-S-c", "C-c 1", "C-c 2", "C-c 3", "C-c 4" };
    for (i32 i = 0; i < ARRAY_COUNT(keys); i++) {
        app_dev_feed(app, "C-g C-g M-g g 1 2", &t->arena); // a prompt with "12" in it
        View *mv = app->mini.view;
        view_set_mark(mv, view_primary(mv), 0, 1); // a region in the input
        app_dev_feed(app, keys[i], &t->arena);
        String8 input = buffer_text(mv->buffer, &t->arena, 0, buffer_size(mv->buffer));
        TEST_CHECK(t, app->mini.active && mv->cursor_count == 1 && str8_equal(input, STR8_LIT("12")) &&
                          test_app_echo_is(app, "Multiple cursors are not available in the minibuffer"),
                   "cursors app: '%s' in the minibuffer: %d cursors, input '%S', echo '%S'", keys[i], mv->cursor_count, input, test_echo(app));
    }
    app_dev_feed(app, "C-g C-g C-g", &t->arena);
    if (!test_app_destroy(t, app, "cursors app")) return 0;
    LOG("test: ok: multiple-cursor commands in the app (C->, C-<, C-g C-g, C-S-c C-S-c; C-c C-< over %d frames at 64 KB a frame with "
        "keys waiting, C-g during it; %d refusals in the minibuffer, by the default keys and by keys bound in a config)", frames,
        (i32)ARRAY_COUNT(keys));
    return 1;
}

static b32 test_mode_line_has(Test *t, App *app, const char *what);
static void test_app_wheel(Test *t, App *app, i32 notches);

// A left click at a text cell of the headless frame (8 x 16 cells, a 4 px pad), with modifiers.
static void test_mc_click(Test *t, App *app, i32 col, i32 row, u32 mods) {
    Event e[2] = { { .kind = EVENT_MOUSE_DOWN, .button = MOUSE_LEFT, .clicks = 1, .x = 4 + col * 8 + 2, .y = row * 16 + 2, .mods = mods },
                   { .kind = EVENT_MOUSE_UP, .button = MOUSE_LEFT, .x = 4 + col * 8 + 2, .y = row * 16 + 2, .mods = mods } };
    app_dev_feed_events(app, e, 2, &t->arena);
}

// The mouse with several cursors: Ctrl+Shift+click adds a cursor (the primary) or removes the one there (never the
// last), a plain click leaves one; refused on the minibuffer. The mode line's count. Two windows on one buffer:
// each window's cursors are its own and follow the other window's edits.
static b32 test_mc_mouse(Test *t) {
    App *app = test_search_app(t, STR8_LIT("line0\nline1\nline2\nline3\n"), 0);
    TEST_CHECK(t, app, "cursors mouse: no app");
    View *v = app_selected_view(app);
    u32 cs = KEYMOD_CTRL | KEYMOD_SHIFT;
    test_mc_click(t, app, 3, 2, cs);
    TEST_CHECK(t, v->cursor_count == 2 && view_point(v, view_primary(v)) == 15 && test_mode_line_has(t, app, "L3 C3  mc:2"),
               "cursors mouse: Ctrl+Shift+click added (%d cursors)", v->cursor_count);
    test_mc_click(t, app, 3, 2, cs);
    TEST_CHECK(t, v->cursor_count == 1 && view_point(v, view_primary(v)) == 0 && !test_mode_line_has(t, app, "mc:"),
               "cursors mouse: Ctrl+Shift+click removed (%d cursors)", v->cursor_count);
    test_mc_click(t, app, 0, 0, cs); // the only cursor: stays
    TEST_CHECK(t, v->cursor_count == 1, "cursors mouse: the last cursor was removed");
    test_mc_click(t, app, 1, 1, cs);
    test_mc_click(t, app, 1, 3, cs);
    TEST_CHECK(t, v->cursor_count == 3 && test_mode_line_has(t, app, "mc:3"), "cursors mouse: three cursors (%d)", v->cursor_count);
    test_mc_click(t, app, 2, 0, 0); // a plain click: one cursor there
    TEST_CHECK(t, v->cursor_count == 1 && view_point(v, view_primary(v)) == 2, "cursors mouse: a plain click left %d", v->cursor_count);
    // On the minibuffer: refused.
    app_dev_feed(app, "M-g g", &t->arena);
    Event m[2] = { { .kind = EVENT_MOUSE_DOWN, .button = MOUSE_LEFT, .clicks = 1, .x = 4 + 20 * 8 + 2, .y = 800 - 16 + 2, .mods = cs },
                   { .kind = EVENT_MOUSE_UP, .button = MOUSE_LEFT, .x = 4 + 20 * 8 + 2, .y = 800 - 16 + 2, .mods = cs } };
    app_dev_feed_events(app, m, 2, &t->arena);
    TEST_CHECK(t, app->mini.active && app->mini.view->cursor_count == 1 && test_app_echo_is(app, "Multiple cursors are not available in the minibuffer"),
               "cursors mouse: on the minibuffer, '%S'", test_echo(app));
    app_dev_feed(app, "C-g", &t->arena);

    // Two windows on one buffer.
    app_dev_feed(app, "C-x 3", &t->arena);
    View *left = app_selected_view(app);
    app_dev_feed(app, "C-x o M->", &t->arena);
    View *right = app_selected_view(app);
    TEST_CHECK(t, left != right && left->buffer == right->buffer && view_point(right, view_primary(right)) == 24, "cursors windows: the setup");
    app_dev_feed(app, "C-x o", &t->arena);
    app_dev_cursors(app, 0, 1, 3); // the left window: lines 0-2
    app_dev_feed(app, "X", &t->arena);
    TEST_CHECK(t, left->cursor_count == 3 && right->cursor_count == 1 && view_point(right, view_primary(right)) == 27,
               "cursors windows: the right window's point followed (%D), %d / %d cursors", view_point(right, view_primary(right)),
               left->cursor_count, right->cursor_count);
    app_dev_feed(app, "C-x o C-a Y", &t->arena); // the right window types on the last line, after the left's cursors
    String8 text = test_app_text(t, app);
    TEST_CHECK(t, str8_equal(text, STR8_LIT("Xline0\nXline1\nXline2\nline3\nY")) && left->cursor_count == 3 && right->cursor_count == 1,
               "cursors windows: '%S'", text);
    app_dev_feed(app, "M-< Z", &t->arena); // the right window at the start: the left's cursors move along
    TEST_CHECK(t, view_point(left, &left->cursors[0]) == 2 && view_point(left, &left->cursors[2]) == 16 && left->cursor_count == 3,
               "cursors windows: the left window's cursors followed (%D, %D)", view_point(left, &left->cursors[0]), view_point(left, &left->cursors[2]));
    if (!test_app_destroy(t, app, "cursors mouse")) return 0;

    // The wheel drags the primary point into the window, past another cursor: the cursors are ordered again (the
    // soak found them out of order), and a dragged primary that meets another cursor merges with it.
    {
        u8 lines[100 * 8];
        i64 n = 0;
        for (i32 i = 0; i < 100; i++) n += fmt_buf(lines + n, 8, "l%03d\n", i); // 5 bytes a line
        App *w = test_search_app(t, str8(lines, n), 0);
        TEST_CHECK(t, w, "cursors wheel: no app");
        View *wv = app_selected_view(w);
        app_dev_cursors(w, 2, 3, 2); // lines 2 (the primary) and 5
        test_app_wheel(t, w, -4);    // 12 lines towards the end: the primary goes to line 12, the window's first
        TEST_CHECK(t, wv->cursor_count == 2 && view_point(wv, &wv->cursors[0]) == 25 && view_point(wv, &wv->cursors[1]) == 60 &&
                          view_primary(wv) == &wv->cursors[1],
                   "cursors wheel: %d cursors at %D, %D, the primary %d", wv->cursor_count, view_point(wv, &wv->cursors[0]),
                   view_point(wv, &wv->cursors[wv->cursor_count - 1]), wv->primary);
        app_dev_cursors(w, 2, 10, 2); // lines 2 (the primary) and 12
        view_set_top_line(wv, 0);
        test_app_wheel(t, w, -4);     // the primary dragged onto the other cursor: one
        TEST_CHECK(t, wv->cursor_count == 1 && view_point(wv, view_primary(wv)) == 60, "cursors wheel: the meeting cursors (%d, at %D)",
                   wv->cursor_count, view_point(wv, view_primary(wv)));
        if (!test_app_destroy(t, w, "cursors wheel")) return 0;
    }
    // view-echo-area-messages moves the point of the window that shows *Messages*, not of the command's own view:
    // that window's cursors are ordered again too (the soak found them out of order).
    {
        App *ma = test_search_app(t, STR8_LIT("abc"), 0);
        TEST_CHECK(t, ma, "cursors messages: no app");
        app_dev_feed(ma, "C-g C-g C-g C-h e C-x o", &t->arena); // *Messages* in the other window, selected
        View *mv = app_selected_view(ma);
        TEST_CHECK(t, mv->buffer == ma->messages && buffer_line_count(mv->buffer) >= 3, "cursors messages: the setup");
        app_dev_cursors(ma, 0, 1, 2); // lines 0 (the primary) and 1
        app_dev_feed(ma, "C-x o C-h e", &t->arena); // from the other window: the primary goes to the end of *Messages*
        i64 end = buffer_size(ma->messages);
        TEST_CHECK(t, mv->cursor_count == 2 && view_point(mv, &mv->cursors[0]) < view_point(mv, &mv->cursors[1]) &&
                          view_point(mv, view_primary(mv)) == end && view_primary(mv) == &mv->cursors[1],
                   "cursors messages: %d cursors at %D, %D, the primary %d (end %D)", mv->cursor_count, view_point(mv, &mv->cursors[0]),
                   view_point(mv, &mv->cursors[mv->cursor_count - 1]), mv->primary, end);
        if (!test_app_destroy(t, ma, "cursors messages")) return 0;
    }
    LOG("test: ok: multiple cursors and the mouse (Ctrl+Shift+click adding, removing, never the last; a plain click; refused on the "
        "minibuffer; the mode line's count; the wheel dragging the primary past and onto another cursor) and two windows on one "
        "buffer (independent cursors that follow each other's edits)");
    return 1;
}

// The commands that make sense with one cursor collapse to the primary first (through the keys); switching buffers
// leaves one cursor; a prompt aborted afterwards does not bring the others back.
static b32 test_mc_collapse(Test *t) {
    App *app = test_search_app(t, STR8_LIT("one\ntwo\nthree\nfour\nfive\nsix\n"), 0);
    TEST_CHECK(t, app, "cursors collapse: no app");
    View *v = app_selected_view(app);
    static const char *keys[] = { "C-s", "M-%", "M-x r e p l a c e - s t r i n g RET", "M-g g", "C-x b RET" };
    for (i32 i = 0; i < ARRAY_COUNT(keys); i++) {
        app_dev_cursors(app, 1, 1, 4); // lines 1-4, the primary on line 1
        TEST_CHECK(t, app_dev_cursor_count(app) == 4, "cursors collapse: setup %d", i);
        app_dev_feed(app, keys[i], &t->arena);
        v = app_selected_view(app);
        TEST_CHECK(t, v->cursor_count == 1, "cursors collapse: '%s' left %d cursors", keys[i], v->cursor_count);
        if (i < 4) {
            TEST_CHECK(t, view_point(v, view_primary(v)) == 4, "cursors collapse: '%s' did not keep the primary", keys[i]);
            app_dev_feed(app, "C-g C-g", &t->arena);
            TEST_CHECK(t, app_selected_view(app)->cursor_count == 1, "cursors collapse: '%s' then C-g", keys[i]);
        } else {
            app_dev_feed(app, "C-x b RET", &t->arena); // back
            TEST_CHECK(t, test_current_is(app, "*scratch*") && app_selected_view(app)->cursor_count == 1, "cursors collapse: switching back");
        }
    }
    if (!test_app_destroy(t, app, "cursors collapse")) return 0;
    LOG("test: ok: multiple cursors collapsing (isearch, query-replace, replace-string, goto-line, the primary kept; switching "
        "buffers; an aborted prompt)");
    return 1;
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

// A buffer's own keymap comes before the global one, only while that buffer is selected and no
// minibuffer or isearch is active.
static b32 test_buffer_keymap(Test *t) {
    String8 conf = str8_fmt(&t->arena, "%S\\p12keymap.conf", t->tmp_dir);
    TEST_CHECK(t, os_write_file(conf, STR8_LIT("[keys search-results]\nn  forward-char\nC-c z  end-of-buffer\n")), "buffer keymap: cannot write");
    App *app = test_search_app(t, STR8_LIT("abc"), 0);
    TEST_CHECK(t, app, "buffer keymap: app_create failed");
    app_dev_use_config(app, conf);
    TEST_CHECK(t, app->config->diags.errors == 0 && app->config->search_results.count >= 2, "buffer keymap: [keys search-results] not read");
    Buffer *buf = app_selected_view(app)->buffer;
    app_dev_feed(app, "n", &t->arena);
    TEST_CHECK(t, str8_equal(test_app_text(t, app), STR8_LIT("nabc")), "buffer keymap: without the keymap n types n");
    buf->keymap = BUFFER_KEYMAP_SEARCH_RESULTS;
    app_dev_feed(app, "n", &t->arena);
    TEST_CHECK(t, str8_equal(test_app_text(t, app), STR8_LIT("nabc")) && test_app_point(app) == 2, "buffer keymap: n runs forward-char (point %D)",
               test_app_point(app));
    app_dev_feed(app, "C-c z", &t->arena);
    TEST_CHECK(t, test_app_point(app) == 4, "buffer keymap: a prefix key of the buffer's map (C-c z)");
    app_dev_feed(app, "C-h k n", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "n runs the command forward-char"), "buffer keymap: describe-key says '%S'",
               str8(app->echo.text, app->echo.len));
    // The minibuffer reads: its keymap and the global one, not the buffer's.
    app_dev_feed(app, "M-x n", &t->arena);
    String8 input = minibuffer_input(&app->mini, &t->arena);
    TEST_CHECK(t, app->mini.active && str8_equal(input, STR8_LIT("n")), "buffer keymap: in the minibuffer n types n ('%S')", input);
    app_dev_feed(app, "C-g", &t->arena);
    // An isearch in that buffer: n extends the search string.
    app_dev_feed(app, "C-s n", &t->arena);
    TEST_CHECK(t, app->isearch.active && str8_equal(isearch_top(&app->isearch)->string, STR8_LIT("n")), "buffer keymap: in isearch n searches");
    app_dev_feed(app, "C-g C-g", &t->arena);
    buf->keymap = BUFFER_KEYMAP_NONE;
    if (!test_app_destroy(t, app, "buffer keymap")) return 0;
    LOG("test: ok: buffer keymap ([keys search-results] read; n types n without it, runs forward-char with it; a prefix of the buffer's "
        "map; describe-key; not in the minibuffer, not in isearch)");
    return 1;
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

static void test_clear_dir(Test *t, String8 dir);

// write-file under a new name again and again: the buffer's meta arena (its struct, path and name) must not grow with
// the number of renames, only with the longest name (Phase 16). Every other rename collides with another open buffer's
// name, so the name is made unique too ("same.txt<p16meta>").
static b32 test_write_file_meta(Test *t) {
    String8 dir = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p16meta", t->tmp_dir));
    test_clear_dir(t, dir);
    os_make_dir(dir);
    String8 first = str8_fmt(&t->arena, "%S\\start.txt", dir);
    TEST_CHECK(t, os_write_file(first, STR8_LIT("meta\n")), "write-file meta: cannot write %S", first);
    String8 sub = str8_fmt(&t->arena, "%S\\sub", dir);
    os_make_dir(sub);
    String8 other = str8_fmt(&t->arena, "%S\\same.txt", sub);
    TEST_CHECK(t, os_write_file(other, STR8_LIT("other\n")), "write-file meta: cannot write %S", other);
    App *app = test_app_create(t);
    TEST_CHECK(t, app && app_dev_visit(app, other) && app_dev_visit(app, first), "write-file meta: app or visit failed");
    Buffer *buf = test_current(app);
    u64 committed_100 = 0;
    for (i32 i = 1; i <= 1000; i++) {
        u64 mark = arena_pos(&t->arena);
        b32 same = i % 2 == 0;
        String8 name = same ? STR8_LIT("same.txt") : str8_fmt(&t->arena, "w%u.txt", (u32)(1000000 + i));
        String8 keys = STR8_LIT("C-x C-w");
        for (i64 k = 0; k < name.len; k++) keys = str8_fmt(&t->arena, "%S %c", keys, name.data[k]);
        app_dev_feed(app, (const char *)str8_fmt(&t->arena, "%S RET%s%c", keys, same && i > 2 ? " y" : "", 0).data, &t->arena);
        String8 want = same ? STR8_LIT("same.txt<p16meta>") : name;
        b32 ok = test_current(app) == buf && str8_equal(buf->name, want) && !app->mini.active;
        arena_pop_to(&t->arena, mark);
        TEST_CHECK(t, ok, "write-file meta: rename %d did not visit its file ('%S', the name '%S')", i, str8(app->echo.text, app->echo.len), buf->name);
        if (i == 100) committed_100 = buf->meta.committed;
    }
    u64 committed_1000 = buf->meta.committed;
    TEST_CHECK(t, committed_1000 == committed_100, "write-file meta: the buffer's meta arena grew from %U to %U bytes between rename 100 and 1000",
               committed_100, committed_1000);
    if (!test_app_destroy(t, app, "write-file meta")) return 0;
    test_clear_dir(t, sub);
    test_clear_dir(t, dir);
    LOG("test: ok: write-file meta (1,000 renames, half of them made unique, the buffer's meta arena at %U bytes after 100 and after 1,000)",
        committed_1000);
    return 1;
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
// Platform threads, the wake, tree watches, junctions, small files (Phase 11)

typedef struct TestWorker {
    OsLock lock;
    OsEvent go;
    i64 counter;
    volatile i32 done;
    volatile i32 added, swapped; // os_atomic_add and os_atomic_cas increments from both threads
    String8 file;                // read by the worker in small pieces (any thread)
    i64 file_got;                // its bytes as read, -1 on an error
    b32 file_same;
} TestWorker;

static void test_worker_main(void *data) {
    TestWorker *w = data;
    os_event_wait(w->go, 10000);
    for (i32 i = 0; i < 100000; i++) {
        os_lock(&w->lock);
        w->counter++;
        os_unlock(&w->lock);
        os_atomic_add(&w->added, 1);
        for (i32 old = os_atomic_get(&w->swapped); os_atomic_cas(&w->swapped, old, old + 1) != old;) old = os_atomic_get(&w->swapped);
    }
    // A file read in 7-byte pieces through the worker's own arena.
    Arena arena = arena_create(MB(1));
    OsFile f;
    i64 size;
    w->file_got = -1;
    if (os_file_open_read_arena(&arena, w->file, &f, &size) == OS_FILE_OK) {
        u8 *bytes = PUSH_ARRAY(&arena, u8, size + 1);
        i64 got = 0, n;
        while (os_file_read_some(f, bytes + got, MIN(7, size + 1 - got), &n) == OS_FILE_OK && n) got += n;
        os_file_close(f);
        w->file_got = got;
        w->file_same = got == size && got == 26 && mem_equal(bytes, "abcdefghijklmnopqrstuvwxyz", 26);
    }
    os_release(arena.base);
    os_atomic_set(&w->done, 1);
    os_wake_main();
}

static void test_waiter_main(void *data) {
    os_event_wait(*(OsEvent *)data, 10000);
}

static b32 test_threads(Test *t) {
    i32 threads0 = os_dev_live_threads(), events0 = os_dev_live_events();
    while (os_dev_wake_wait(0)) {} // nothing left over
    TestWorker w = { 0 };
    w.file = str8_fmt(&t->arena, "%S\\threads_read.txt", t->tmp_dir);
    TEST_CHECK(t, os_write_file(w.file, STR8_LIT("abcdefghijklmnopqrstuvwxyz")), "threads: cannot write %S", w.file);
    TEST_CHECK(t, os_event_create(&w.go), "threads: no event");
    OsThread th;
    TEST_CHECK(t, os_thread_start(&th, test_worker_main, &w, 0), "threads: no thread");
    TEST_CHECK(t, !os_thread_join(th, 0) && os_dev_live_threads() == threads0 + 1, "threads: a waiting worker is still running");
    TEST_CHECK(t, os_dev_thread_priority(th) == 0, "threads: a foreground worker's priority is %d, not normal", os_dev_thread_priority(th));
    {
        OsEvent quit;
        TEST_CHECK(t, os_event_create(&quit), "threads: no event");
        OsThread bg;
        TEST_CHECK(t, os_thread_start(&bg, test_waiter_main, &quit, 1), "threads: no background thread");
        i32 prio = os_dev_thread_priority(bg);
        os_event_set(quit);
        TEST_CHECK(t, os_thread_join(bg, 10000), "threads: the background worker did not end");
        os_event_destroy(quit);
        TEST_CHECK(t, prio == -1, "threads: a background worker's priority is %d, not one below normal (-1)", prio);
    }
    os_event_set(w.go);
    for (i32 i = 0; i < 100000; i++) {
        os_lock(&w.lock);
        w.counter++;
        os_unlock(&w.lock);
        os_atomic_add(&w.added, 1);
        for (i32 old = os_atomic_get(&w.swapped); os_atomic_cas(&w.swapped, old, old + 1) != old;) old = os_atomic_get(&w.swapped);
    }
    TEST_CHECK(t, os_thread_join(th, 10000), "threads: the worker did not end");
    TEST_CHECK(t, w.counter == 200000 && os_atomic_get(&w.done) == 1, "threads: the lock lost increments (%D)", w.counter);
    TEST_CHECK(t, os_atomic_get(&w.added) == 200000 && os_atomic_get(&w.swapped) == 200000, "threads: atomics lost increments (add %d, cas %d)",
               os_atomic_get(&w.added), os_atomic_get(&w.swapped));
    TEST_CHECK(t, w.file_same, "threads: the worker read %D bytes of the file, not the 26 written", w.file_got);
    TEST_CHECK(t, os_cpu_count() >= 1, "threads: os_cpu_count");
    TEST_CHECK(t, os_dev_wake_wait(0) && !os_dev_wake_wait(0), "threads: os_wake_main set the wake once (auto-reset)");
    os_event_destroy(w.go);
    TEST_CHECK(t, os_dev_live_threads() == threads0 && os_dev_live_events() == events0, "threads: counters back (%d threads, %d events)",
               os_dev_live_threads(), os_dev_live_events());
    LOG("test: ok: threads (start, join with a timeout, a lock and os_atomic_add / os_atomic_cas under contention, a file read in "
        "pieces on the worker, a background worker one step below normal priority, an auto-reset event, os_wake_main, the live "
        "counters; %d logical processors)", os_cpu_count());
    return 1;
}

// Polls the platform's watches until one event arrives or `ms` pass; returns the events seen.
static i32 test_poll_watch(OsWatch w, u32 ms) {
    OsEvent pause = { 0 };
    os_event_create(&pause);
    i32 seen = 0;
    for (u32 waited = 0; waited <= ms && !seen; waited += 10) {
        Event e[8];
        i32 n = os_dev_poll_watches(e, 8);
        for (i32 i = 0; i < MIN(n, 8); i++) seen += e[i].kind == EVENT_DIR_CHANGED && e[i].watch == w;
        if (!seen && ms) os_event_wait(pause, 10);
    }
    os_event_destroy(pause);
    return seen;
}

// A tree watch fires once for a storm of 500 new files (and their deletion) and stays quiet until it is
// re-armed; then one change fires it once more.
static b32 test_tree_watch(Test *t) {
    String8 root = str8_fmt(&t->arena, "%S\\p11watch", t->tmp_dir), sub = str8_fmt(&t->arena, "%S\\sub", root);
    os_make_dir(root);
    os_make_dir(sub);
    for (i32 i = 0; i < 500; i++) os_file_delete(str8_fmt(&t->arena, "%S\\w%03d.txt", sub, i));
    os_file_delete(str8_fmt(&t->arena, "%S\\again.txt", root));
    OsWatch w = os_watch_tree(root);
    TEST_CHECK(t, w, "tree watch: cannot watch %S", root);
    i32 events = 0;
    for (i32 i = 0; i < 500; i++) {
        TEST_CHECK(t, os_write_file(str8_fmt(&t->arena, "%S\\w%03d.txt", sub, i), STR8_LIT("x")), "tree watch: cannot write");
        if (i % 50 == 49) events += test_poll_watch(w, 0);
    }
    for (i32 i = 0; i < 500; i++) os_file_delete(str8_fmt(&t->arena, "%S\\w%03d.txt", sub, i));
    events += test_poll_watch(w, events ? 0 : 1000);
    events += test_poll_watch(w, 200); // nothing more, however long
    TEST_CHECK(t, events == 1, "tree watch: %d wake-up(s) for 500 files created and deleted, expected 1", events);
    TEST_CHECK(t, os_watch_rearm(w, root), "tree watch: cannot re-arm");
    TEST_CHECK(t, test_poll_watch(w, 0) == 0, "tree watch: re-armed, it does not fire for the earlier changes");
    TEST_CHECK(t, os_write_file(str8_fmt(&t->arena, "%S\\again.txt", root), STR8_LIT("x")), "tree watch: cannot write");
    i32 again = test_poll_watch(w, 1000);
    os_file_delete(str8_fmt(&t->arena, "%S\\again.txt", root));
    again += test_poll_watch(w, 100);
    TEST_CHECK(t, again == 1, "tree watch: %d wake-up(s) after the re-arm, expected 1", again);
    os_unwatch(w);
    LOG("test: ok: tree watch (500 files created and deleted in a subdirectory: 1 wake-up; quiet until re-armed; then 1 more)");
    return 1;
}

// A junction is listed as a reparse point; small files are read whole, a larger one is refused.
static b32 test_junction_small_file(Test *t) {
    String8 dir = str8_fmt(&t->arena, "%S\\p11junction", t->tmp_dir), target = str8_fmt(&t->arena, "%S\\target", dir);
    String8 link = str8_fmt(&t->arena, "%S\\link", dir);
    os_make_dir(dir);
    os_make_dir(target);
    os_dev_remove_dir(link);
    TEST_CHECK(t, os_dev_make_junction(link, target), "junction: cannot make %S", link);
    OsDirEntry *e;
    i64 n;
    TEST_CHECK(t, os_list_dir(&t->arena, dir, &e, &n) == OS_FILE_OK, "junction: cannot list");
    i32 seen = 0;
    for (i64 i = 0; i < n; i++) {
        if (str8_equal(e[i].name, STR8_LIT("link")) && e[i].is_dir && e[i].reparse) seen |= 1;
        if (str8_equal(e[i].name, STR8_LIT("target")) && e[i].is_dir && !e[i].reparse) seen |= 2;
    }
    TEST_CHECK(t, seen == 3, "junction: the link is a reparse directory, the target a plain one (%d)", seen);
    TEST_CHECK(t, os_dev_remove_dir(link) && os_file_info(target, &(OsFileInfo){ 0 }) == OS_FILE_OK, "junction: removed, the target stays");
    String8 short_file = str8_fmt(&t->arena, "%S\\small.txt", dir), got;
    TEST_CHECK(t, os_write_file(short_file, STR8_LIT("hello\n")), "small file: cannot write");
    TEST_CHECK(t, os_read_small_file(&t->arena, short_file, 6, &got) == OS_FILE_OK && str8_equal(got, STR8_LIT("hello\n")), "small file: read");
    TEST_CHECK(t, os_read_small_file(&t->arena, short_file, 5, &got) == OS_FILE_TOO_LARGE && !got.len, "small file: over the limit");
    TEST_CHECK(t, os_read_small_file(&t->arena, str8_fmt(&t->arena, "%S\\nope", dir), 5, &got) == OS_FILE_NOT_FOUND, "small file: missing");
    LOG("test: ok: junctions (listed as reparse points, removed without their target) and small file reads");
    return 1;
}

// ---------------------------------------------------------------------------
// Ignore rules (Phase 11): golden cases for every supported .gitignore feature.

static IgnoreScope test_ignore_scope(Test *t, const char *base, const char *text, const IgnoreScope *parent) {
    IgnoreScope s = { .parent = parent, .base = str8_cstr(base) };
    ignore_parse_lines(&t->arena, &s, str8_cstr(text), 0);
    return s;
}

static b32 test_ignore_rules(Test *t) {
    // The glob alone ('*' and '?' inside a segment, "**" across segments).
    static const struct { const char *glob, *path; b32 want; } globs[] = {
        { "*.c", "a.c", 1 },          { "*.c", "a.h", 0 },          { "*.c", "dir/a.c", 0 },     { "?.c", "a.c", 1 },
        { "?.c", "ab.c", 0 },         { "a?c", "a/c", 0 },          { "**/foo", "foo", 1 },      { "**/foo", "a/foo", 1 },
        { "**/foo", "a/b/foo", 1 },   { "**/foo", "afoo", 0 },      { "abc/**", "abc/x", 1 },    { "abc/**", "abc/x/y", 1 },
        { "abc/**", "abc", 0 },       { "a/**/b", "a/b", 1 },       { "a/**/b", "a/x/b", 1 },    { "a/**/b", "a/x/y/b", 1 },
        { "a/**/b", "a/xb", 0 },      { "a**b", "axyb", 1 },        { "a**b", "ax/b", 0 },       { "*", "anything", 1 },
        { "*", "a/b", 0 },            { "**", "a/b", 1 },           { "f*o*o", "fxoyo", 1 },     { "f*o*o", "fxoy", 0 },
        { "src/*.c", "SRC/Main.C", 1 }, { "", "", 1 },
    };
    for (i32 i = 0; i < ARRAY_COUNT(globs); i++) {
        TEST_CHECK(t, ignore_glob(str8_cstr(globs[i].glob), str8_cstr(globs[i].path)) == globs[i].want, "ignore: glob '%s' on '%s', expected %d",
                   globs[i].glob, globs[i].path, globs[i].want);
    }

    // Parsing: comments, blank lines, trailing blanks, '!', the anchors, directories only.
    IgnoreScope p = test_ignore_scope(t, "", "# a comment\n\n*.o  \r\n!keep.o\n/build/\ndoc/frotz/\n**/gen\n  \n", NULL);
    TEST_CHECK(t, p.count == 5, "ignore: 5 patterns parsed, got %d", p.count);
    TEST_CHECK(t, str8_equal(p.patterns[0].glob, STR8_LIT("*.o")) && p.patterns[0].flags == 0 &&
                  str8_equal(p.patterns[1].glob, STR8_LIT("keep.o")) && p.patterns[1].flags == IGNORE_NEGATE &&
                  str8_equal(p.patterns[2].glob, STR8_LIT("build")) && p.patterns[2].flags == (IGNORE_DIR_ONLY | IGNORE_ANCHORED) &&
                  str8_equal(p.patterns[3].glob, STR8_LIT("doc/frotz")) && p.patterns[3].flags == (IGNORE_DIR_ONLY | IGNORE_ANCHORED) &&
                  str8_equal(p.patterns[4].glob, STR8_LIT("**/gen")) && p.patterns[4].flags == IGNORE_ANCHORED,
               "ignore: parsed patterns and flags");

    // Decisions in one scope, and across scopes: { scope set, path, directory?, ignored? }.
    IgnoreScope root = test_ignore_scope(t, "", "*.log\n!keep.log\nbuild/\n/out/\ndoc/frotz/\n*.tmp\n**/cache\nlib/**\nTMP/\n", NULL);
    IgnoreScope sub = test_ignore_scope(t, "sub", "!*.tmp\nlocal/\n*.log\n", &root);
    IgnoreScope last = test_ignore_scope(t, "", "!keep.txt\n*.txt\n", NULL);
    IgnoreScope builtin = test_ignore_scope(t, "", "node_modules/\n*.exe\n", NULL);
    IgnoreScope gen = test_ignore_scope(t, "", "gen/\n", NULL);
    IgnoreScope top = { 0 };
    ignore_parse_lines(&t->arena, &top, STR8_LIT("*.md\n"), 0);
    IgnoreScope allow = { 0 };
    ignore_parse_lines(&t->arena, &allow, STR8_LIT("gen/\nnode_modules/\n"), 1);
    // .ronin-project as one scope: [ignore] first, [allow] after it (negations win as the later patterns).
    IgnoreScope project = { .base = str8(NULL, 0) };
    project.patterns = PUSH_ARRAY(&t->arena, IgnorePattern, top.count + allow.count);
    for (i32 i = 0; i < top.count; i++) project.patterns[project.count++] = top.patterns[i];
    for (i32 i = 0; i < allow.count; i++) project.patterns[project.count++] = allow.patterns[i];
    static const struct { i32 set; const char *path; b32 dir, want; const char *what; } cases[] = {
        { 0, "a.log", 0, 1, "a name pattern" },
        { 0, "x/y/a.log", 0, 1, "a name pattern at any depth" },
        { 0, "keep.log", 0, 0, "negation" },
        { 0, "x/keep.log", 0, 0, "negation at any depth" },
        { 0, "build", 1, 1, "a directory pattern on a directory" },
        { 0, "build", 0, 0, "a directory pattern on a file" },
        { 0, "src/build", 1, 1, "a directory pattern at any depth" },
        { 0, "out", 1, 1, "a leading slash anchors" },
        { 0, "src/out", 1, 0, "an anchored pattern below the root" },
        { 0, "doc/frotz", 1, 1, "an inner slash anchors" },
        { 0, "a/doc/frotz", 1, 0, "an inner-slash pattern elsewhere" },
        { 0, "a/b/cache", 1, 1, "**/ at the start" },
        { 0, "lib/x/y.c", 0, 1, "/** at the end" },
        { 0, "lib", 1, 0, "/** does not match the directory itself" },
        { 0, "tmp", 1, 1, "case-insensitive" },
        { 0, "a.TMP", 0, 1, "case-insensitive extension" },
        { 1, "sub/a.tmp", 0, 0, "a nested .gitignore negates its parent's pattern" },
        { 1, "a.tmp", 0, 1, "the nested file applies only below its directory" },
        { 1, "sub/local", 1, 1, "a nested name pattern" },
        { 1, "other/local", 1, 0, "the nested name pattern elsewhere" },
        { 1, "sub/keep.log", 0, 1, "the nested *.log after the root's !keep.log: the last match decides" },
        { 2, "keep.txt", 0, 1, "!keep.txt before *.txt: the last match decides" },
        { 3, "node_modules", 1, 0, "[allow] beats the built-in list" },
        { 3, "x/node_modules", 1, 0, "[allow] of a name pattern at any depth" },
        { 3, "gen", 1, 0, "[allow] beats .gitignore" },
        { 3, "README.md", 0, 1, "[ignore] adds to the rest" },
        { 3, "a.exe", 0, 1, "the built-in list" },
        { 4, "node_modules", 1, 1, "the built-in list without .ronin-project" },
        { 4, "gen", 1, 1, ".gitignore over the built-in list" },
        { 4, "src/main.c", 0, 0, "nothing matches: included" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        String8 rel = str8_cstr(cases[i].path);
        b32 got = 0;
        switch (cases[i].set) {
        case 0: got = ignore_path(NULL, &root, NULL, rel, cases[i].dir); break;
        case 1: got = ignore_path(NULL, &sub, NULL, rel, cases[i].dir); break;
        case 2: got = ignore_path(NULL, &last, NULL, rel, cases[i].dir); break;
        case 3: got = ignore_path(&project, &gen, &builtin, rel, cases[i].dir); break;
        case 4: got = ignore_path(NULL, &gen, &builtin, rel, cases[i].dir); break;
        }
        TEST_CHECK(t, got == cases[i].want, "ignore: %s: '%s'%s ignored %d, expected %d", cases[i].what, cases[i].path,
                   cases[i].dir ? " (directory)" : "", got, cases[i].want);
    }
    IgnoreScope words = { 0 };
    ignore_parse_words(&t->arena, &words, STR8_LIT("  .git/ *.exe\tnode_modules/ \n"));
    TEST_CHECK(t, words.count == 3 && ignore_scope_decide(&words, STR8_LIT("a/.git"), 1) == 1 &&
                  ignore_scope_decide(&words, STR8_LIT("a.c"), 0) == -1, "ignore: a blank-separated list");
    LOG("test: ok: ignore rules (%d globs; comments, blanks, '!', anchors, directories only, '**' in three places, case, nested "
        "files, the last match deciding, the built-in list, .ronin-project [ignore] and [allow] on top: %d cases)",
        (i32)ARRAY_COUNT(globs), (i32)ARRAY_COUNT(cases));
    return 1;
}

// A fake file system for root detection: the paths that exist (case-insensitive), and the queries made.
typedef struct TestFakeFs {
    const char *const *paths;
    i32 count;
    i32 queries;
} TestFakeFs;

static b32 test_fake_exists(void *data, String8 path) {
    TestFakeFs *fs = data;
    fs->queries++;
    for (i32 i = 0; i < fs->count; i++) {
        String8 p = str8_cstr(fs->paths[i]);
        if (p.len == path.len && project_path_inside(path, p)) return 1;
    }
    return 0;
}

// Root detection: each marker, nested repositories, no marker (the command-line directory only, never the
// current directory), *scratch*, drive and share roots; the cache; the built-in settings; the real
// file system.
static b32 test_project_roots(Test *t) {
    static const char *const paths[] = {
        "C:\\w\\repo\\.git", "C:\\w\\repo\\sub\\inner\\.hg", "C:\\w\\wt\\.git", "C:\\w\\tp\\.ronin-project", "C:\\w\\svn\\.svn",
        "C:\\.git", "\\\\srv\\share\\.git",
    };
    TestFakeFs fs = { paths, (i32)ARRAY_COUNT(paths), 0 };
    String8 markers = STR8_LIT(".git .hg .svn .ronin-project");
    static const struct { const char *dir, *cmdline, *want, *what; } cases[] = {
        { "C:\\w\\repo\\src\\x", "", "C:\\w\\repo", ".git above the file" },
        { "C:\\w\\repo", "", "C:\\w\\repo", ".git in the file's own directory" },
        { "C:\\w\\repo\\sub\\inner\\deep", "", "C:\\w\\repo\\sub\\inner", "nested repositories: the inner one" },
        { "C:\\w\\wt\\a", "", "C:\\w\\wt", ".git as a file (a worktree)" },
        { "C:\\w\\tp\\a\\b", "", "C:\\w\\tp", ".ronin-project" },
        { "C:\\w\\svn", "", "C:\\w\\svn", ".svn" },
        { "C:\\w\\none\\a", "", "", "no marker, no command-line directory: not in a project (a marker at C:\\ does not count)" },
        { "C:\\w\\none\\a", "C:\\w", "C:\\w", "no marker, inside the command-line directory" },
        { "C:\\w\\none\\a", "c:\\W\\", "c:\\W\\", "no marker, inside the command-line directory (case, trailing separator)" },
        { "C:\\other\\x", "C:\\w", "", "no marker, outside the command-line directory" },
        { "C:\\w\\repo2\\x", "C:\\w\\repo", "", "a sibling with the command-line directory as a name prefix" },
        { "C:\\w\\repo\\src\\x", "C:\\w\\repo\\src", "C:\\w\\repo\\src", "inside the command-line directory: a marker above it does not count" },
        { "C:\\w\\repo\\sub\\inner\\deep", "C:\\w\\repo", "C:\\w\\repo\\sub\\inner", "inside the command-line directory: a nearer nested marker wins" },
        { "C:\\", "", "", "a file at a drive root" },
        { "", "", "", "*scratch* without a command-line directory" },
        { "", "C:\\w", "C:\\w", "*scratch* with a command-line directory" },
        { "", "C:\\", "C:\\", "an explicit drive root on the command line is used as given" },
        { "\\\\srv\\share\\a", "", "", "a marker at a share root does not count" },
    };
    ProjectRootCache cache;
    project_cache_init(&cache);
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        String8 got = project_root_for(&cache, str8_cstr(cases[i].dir), str8_cstr(cases[i].cmdline), markers, test_fake_exists, &fs);
        TEST_CHECK(t, str8_equal(got, str8_cstr(cases[i].want)), "roots: %s: '%S', expected '%s'", cases[i].what, got, cases[i].want);
    }
    // The cache: the same directory again asks the file system nothing.
    i32 queries = fs.queries;
    u64 finds = cache.queries;
    String8 again = project_root_for(&cache, STR8_LIT("C:\\w\\repo\\src\\x"), str8(NULL, 0), markers, test_fake_exists, &fs);
    TEST_CHECK(t, str8_equal(again, STR8_LIT("C:\\w\\repo")) && fs.queries == queries && cache.queries == finds,
               "roots: a cached directory asked the file system again (%d queries)", fs.queries - queries);
    project_cache_reset(&cache);
    project_root_for(&cache, STR8_LIT("C:\\w\\repo\\src\\x"), str8(NULL, 0), markers, test_fake_exists, &fs);
    TEST_CHECK(t, fs.queries > queries, "roots: after a reset the file system is asked again");
    TEST_CHECK(t, project_is_drive_root(STR8_LIT("D:")) && project_is_drive_root(STR8_LIT("D:\\")) &&
                  project_is_drive_root(STR8_LIT("\\\\srv\\share\\")) && !project_is_drive_root(STR8_LIT("\\\\srv\\share\\x")) &&
                  !project_is_drive_root(STR8_LIT("D:\\x")), "roots: drive and share roots");

    // The real file system: build\tmp\p11roots\outer\.git (a directory) and outer\in\.ronin-project (a file).
    String8 base = str8_fmt(&t->arena, "%S\\p11roots", t->tmp_dir);
    String8 outer = str8_fmt(&t->arena, "%S\\outer", base), in = str8_fmt(&t->arena, "%S\\in", outer);
    String8 deep = str8_fmt(&t->arena, "%S\\deep", in), plain = str8_fmt(&t->arena, "%S\\plain", base);
    os_make_dir(base);
    os_make_dir(outer);
    os_make_dir(str8_fmt(&t->arena, "%S\\.git", outer));
    os_make_dir(in);
    os_make_dir(deep);
    os_make_dir(plain);
    TEST_CHECK(t, os_write_file(str8_fmt(&t->arena, "%S\\.ronin-project", in), STR8_LIT("[ignore]\n")), "roots: cannot write");
    project_cache_reset(&cache);
    String8 r1 = project_root_for(&cache, deep, str8(NULL, 0), markers, project_exists_fs, NULL);
    String8 r2 = project_root_for(&cache, str8_fmt(&t->arena, "%S\\.git", outer), str8(NULL, 0), markers, project_exists_fs, NULL);
    String8 r3 = project_root_for(&cache, plain, str8(NULL, 0), markers, project_exists_fs, NULL);
    String8 r4 = project_root_for(&cache, plain, base, markers, project_exists_fs, NULL);
    b32 in_repo = r3.len != 0; // build\tmp may itself be inside a repository with a marker (ronin's own .git)
    TEST_CHECK(t, str8_equal(r1, in) && str8_equal(r2, outer) && (in_repo || (!r3.len && str8_equal(r4, base))),
               "roots: real directories: '%S', '%S', '%S', '%S'", r1, r2, r3, r4);
    project_cache_destroy(&cache);

    // The settings' built-in values.
    Config *c = PUSH_STRUCT(&t->arena, Config);
    config_init(c);
    config_parse(c, &t->arena, config_default_text(), STR8_LIT("<built-in>"));
    Settings *s = &c->settings;
    String8 ignore = str8(s->project_ignore, s->project_ignore_len);
    IgnoreScope builtin = { 0 };
    ignore_parse_words(&t->arena, &builtin, ignore);
    TEST_CHECK(t, str8_equal(str8(s->project_markers, s->project_markers_len), STR8_LIT(".git .hg .svn .ronin-project")) &&
                  s->project_max_files == 1000000 && c->diags.errors == 0, "roots: the built-in project settings");
    static const struct { const char *path; b32 dir, want; } defaults[] = {
        { ".git", 1, 1 }, { "a/.hg", 1, 1 }, { ".svn", 1, 1 }, { ".vs", 1, 1 }, { "web/node_modules", 1, 1 }, { "x/__pycache__", 1, 1 },
        { "ronin.exe", 0, 1 }, { "a/b.pdb", 0, 1 }, { "img/logo.PNG", 0, 1 }, { "build", 1, 0 }, { "src/bin", 1, 0 }, { "out", 1, 0 },
        { "obj", 1, 0 }, { "target", 1, 0 }, { "dist", 1, 0 }, { "src/app.c", 0, 0 }, { ".gitignore", 0, 0 },
    };
    for (i32 i = 0; i < ARRAY_COUNT(defaults); i++) {
        b32 got = ignore_path(NULL, NULL, &builtin, str8_cstr(defaults[i].path), defaults[i].dir);
        TEST_CHECK(t, got == defaults[i].want, "roots: the built-in project_ignore on '%s': %d, expected %d", defaults[i].path, got,
                   defaults[i].want);
    }
    LOG("test: ok: project roots (%d cases: each marker, nested repositories, .git as a file, no marker with and without a "
        "command-line directory, *scratch*, drive and share roots; the cache; real directories; the built-in settings, build "
        "directories not ignored by default)", (i32)ARRAY_COUNT(cases));
    return 1;
}

// .ronin-project: [ignore] and [allow] with their diagnostics.
static b32 test_project_file(Test *t) {
    ProjectFile f;
    project_file_parse(&t->arena, STR8_LIT("# a comment\n[ignore]\nbuild/\n*.log\n\n[allow]\nbuild/keep/\n[bogus]\nx\n[ignore\nloose\n[ignore]\nsrc/[ab].c\n"),
                       STR8_LIT(".ronin-project"), &f);
    TEST_CHECK(t, str8_equal(f.ignore, STR8_LIT("build/\n*.log\nsrc/[ab].c\n")) && str8_equal(f.allow, STR8_LIT("build/keep/\n")),
               "project file: patterns '%S' / '%S'", f.ignore, f.allow);
    static const char *const want[] = {
        ".ronin-project:8: unknown section [bogus] (expected [ignore], [allow] or [build])",
        ".ronin-project:10: bad section header (expected [ignore], [allow] or [build])",
        ".ronin-project:13: warning: 'src/[ab].c': character classes and backslash escapes are not supported (matched literally)",
    };
    i32 k = 0;
    for (ConfigDiag *d = f.diags.first; d; d = d->next, k++) {
        TEST_CHECK(t, k < ARRAY_COUNT(want) && str8_equal(d->text, str8_cstr(want[k])), "project file: diagnostic %d '%S'", k, d->text);
    }
    TEST_CHECK(t, k == ARRAY_COUNT(want) && f.diags.errors == 2 && f.diags.warnings == 1, "project file: %d diagnostics", k);
    project_file_parse(&t->arena, STR8_LIT("stray\n"), STR8_LIT(".ronin-project"), &f);
    TEST_CHECK(t, f.diags.errors == 1 && str8_equal(f.diags.first->text, STR8_LIT(".ronin-project:1: line outside a section ([ignore], [allow] or [build])")),
               "project file: a line before any section");
    // [build]: command and directory (blanks around '=' trimmed, the value kept whole), and its diagnostics.
    project_file_parse(&t->arena, STR8_LIT("[build]\ncommand =  build.bat release  \ndirectory=src/sub\nrun = x\njust words\ncommand =\n[ignore]\n*.o\n"),
                       STR8_LIT(".ronin-project"), &f);
    TEST_CHECK(t, str8_equal(f.build_command, STR8_LIT("build.bat release")) && str8_equal(f.build_directory, STR8_LIT("src/sub")) &&
                      str8_equal(f.ignore, STR8_LIT("*.o\n")), "project file: [build] '%S' in '%S', ignore '%S'", f.build_command, f.build_directory, f.ignore);
    static const char *const want_build[] = {
        ".ronin-project:4: unknown name 'run' in [build] (expected command or directory)",
        ".ronin-project:5: expected name = value",
        ".ronin-project:6: expected name = value",
    };
    k = 0;
    for (ConfigDiag *d = f.diags.first; d; d = d->next, k++) {
        TEST_CHECK(t, k < ARRAY_COUNT(want_build) && str8_equal(d->text, str8_cstr(want_build[k])), "project file: [build] diagnostic %d '%S'", k, d->text);
    }
    TEST_CHECK(t, k == ARRAY_COUNT(want_build), "project file: [build] gave %d diagnostics", k);
    project_file_parse(&t->arena, STR8_LIT("[build]\ncommand = cl /W4 a.c && .\\a.exe\n"), STR8_LIT(".ronin-project"), &f);
    TEST_CHECK(t, str8_equal(f.build_command, STR8_LIT("cl /W4 a.c && .\\a.exe")) && !f.diags.errors && !f.diags.warnings,
               "project file: a command with a backslash '%S' (%d warnings)", f.build_command, f.diags.warnings);
    LOG("test: ok: .ronin-project ([ignore], [allow], [build] with command and directory, unknown and bad sections and names, a line "
        "outside a section, unsupported syntax, a backslash in a command)");
    return 1;
}

// An index from relative paths (directories looked up among the ones added so far).
static void test_index_from(ProjectIndex *x, const char *const *paths, i64 n) {
    project_index_reset(x);
    for (i64 i = 0; i < n; i++) {
        String8 p = str8_cstr(paths[i]);
        i64 slash = p.len;
        while (slash > 0 && p.data[slash - 1] != '/') slash--;
        String8 dir = str8(p.data, slash ? slash - 1 : 0), name = str8(p.data + slash, p.len - slash);
        i64 d = -1;
        for (i64 k = 0; k < x->dir_count && d < 0; k++) if (str8_equal(project_index_dir(x, k), dir)) d = k;
        if (d < 0) d = project_index_add_dir(x, dir);
        project_index_add_file(x, d, name, project_mask(p));
    }
}

// The ranked rows as "a,b,c" (the first `max`).
static String8 test_fuzzy_rows(Test *t, FuzzyRank *r, i32 max) {
    i32 n = fuzzy_rank_rows(r);
    String8 out = str8(NULL, 0);
    for (i32 i = 0; i < n && i < max; i++) {
        u8 *buf = PUSH_ARRAY(&t->arena, u8, PROJECT_PATH_MAX);
        String8 p = str8(buf, fuzzy_rank_path(r, r->rows[i], buf, PROJECT_PATH_MAX));
        out = out.len ? str8_fmt(&t->arena, "%S,%S", out, p) : p;
    }
    return out;
}

static b32 test_fuzzy_same(FuzzyRank *a, FuzzyRank *b) {
    i32 n = fuzzy_rank_rows(a);
    if (n != fuzzy_rank_rows(b) || a->total != b->total) return 0;
    for (i32 i = 0; i < n; i++) if (a->rows[i] != b->rows[i]) return 0;
    return 1;
}

// Fuzzy ranking goldens, spans, slicing, narrowing.
static b32 test_fuzzy(Test *t, u64 seed) {
    ProjectIndex x;
    project_index_init(&x);
    FuzzyRank r, full;
    fuzzy_rank_init(&r);
    fuzzy_rank_init(&full);
    static const char *const sets[][6] = {
        { "docs/app/readme.md", "src/map_app.c" },                 // the file name beats the directory part
        { "src/mapper.c", "src/apple.c" },                         // a segment start beats the middle of a word
        { "src/a1b2c.c", "src/abcdef.c" },                         // consecutive beats scattered
        { "src/Makefile.am", "src/makefile.old", "deep/dir/Makefile" }, // the exact file name first
        { "src/minibuffer.c", "src/ui/MiniBuffer.cpp" },           // smart case: uppercase is exact
        { "src/minibuffer.c", "src/ui/MiniBuffer.cpp" },           // ... lowercase matches both
        { "lib/slex.c", "src/lex_c.c" },                           // '/' in the query
        { "a/b/app.c", "app.c" },                                  // shorter on equal scores
        { "src/app.c", "src/notes.c", "docs/app/notes.md" },        // several terms
        { "src/win32_main.c", "src/winmain.c" },                    // a word start after '_'
        { "src/FooBar.c", "src/foobar_util.c" },                   // camel case start
        { "src/App.c", "src/app.c" },                              // an exact name with case: smart case decides
    };
    static const struct { i32 set; const char *query, *first; i64 matches; } cases[] = {
        { 0, "app", "src/map_app.c", 2 },
        { 1, "ap", "src/apple.c", 2 },
        { 2, "abc", "src/abcdef.c", 2 },
        { 3, "makefile", "deep/dir/Makefile", 3 },
        { 4, "MB", "src/ui/MiniBuffer.cpp", 1 },
        { 5, "mb", "src/ui/MiniBuffer.cpp", 2 },
        { 6, "s/l", "src/lex_c.c", 1 },
        { 7, "app", "app.c", 2 },
        { 8, "app notes", "docs/app/notes.md", 1 },
        { 9, "wm", "src/win32_main.c", 2 },
        { 10, "fb", "src/FooBar.c", 2 },
        { 11, "App.c", "src/App.c", 1 },
        { 11, "app.c", "src/App.c", 2 },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        i64 n = 0;
        while (n < 6 && sets[cases[i].set][n]) n++;
        test_index_from(&x, sets[cases[i].set], n);
        fuzzy_rank_begin(&r, &x, str8_cstr(cases[i].query), 0);
        fuzzy_rank_finish(&r);
        String8 rows = test_fuzzy_rows(t, &r, 6);
        String8 first = test_fuzzy_rows(t, &r, 1);
        TEST_CHECK(t, str8_equal(first, str8_cstr(cases[i].first)) && r.total == cases[i].matches,
                   "fuzzy: '%s': ranked '%S' (%D matches), expected '%s' first and %D matches", cases[i].query, rows, r.total,
                   cases[i].first, cases[i].matches);
    }
    // Spans: merged runs of matched characters.
    FuzzyQuery q = fuzzy_query(&t->arena, STR8_LIT("ap"));
    MatchSpan spans[8];
    i32 ns;
    i32 sc = fuzzy_score(&q, STR8_LIT("src/apple.c"), 4, spans, 8, &ns);
    TEST_CHECK(t, sc != FUZZY_NO_MATCH && ns == 1 && spans[0].start == 4 && spans[0].end == 6, "fuzzy: spans of 'ap' in src/apple.c");
    q = fuzzy_query(&t->arena, STR8_LIT("s/ac"));
    sc = fuzzy_score(&q, STR8_LIT("src/app.c"), 4, spans, 8, &ns);
    TEST_CHECK(t, sc != FUZZY_NO_MATCH && ns == 3 && spans[0].start == 0 && spans[0].end == 1 && spans[1].start == 3 && spans[1].end == 5 &&
                  spans[2].start == 8, "fuzzy: spans of 's/ac' in src/app.c (%d)", ns);
    q = fuzzy_query(&t->arena, STR8_LIT("zz"));
    TEST_CHECK(t, fuzzy_score(&q, STR8_LIT("src/app.c"), 4, NULL, 0, NULL) == FUZZY_NO_MATCH, "fuzzy: no match");

    // Slicing and narrowing against full scoring on 20,000 generated paths.
    static const char *const words[] = { "app", "main", "buffer", "view", "render", "font", "win32", "test", "config", "mini",
                                         "Search", "isearch", "lex", "syntax", "project", "fuzzy", "Window", "edit", "key", "map" };
    i32 nw = (i32)ARRAY_COUNT(words);
    u64 rng = seed ^ 0xF00Dull;
#define TEST_RNG() (rng = rng * 6364136223846793005ull + 1442695040888963407ull, (u32)(rng >> 33))
    project_index_reset(&x);
    for (i32 d = 0; d < 400; d++) {
        String8 dir = str8_fmt(&t->arena, "%s/%s_%d", words[TEST_RNG() % nw], words[TEST_RNG() % nw], d);
        i64 di = project_index_add_dir(&x, dir);
        for (i32 f = 0; f < 50; f++) {
            String8 name = str8_fmt(&t->arena, "%s%s_%d.%s", words[TEST_RNG() % nw], words[TEST_RNG() % nw], f, (TEST_RNG() & 1) ? "c" : "h");
            project_index_add_file(&x, di, name, project_mask(str8_fmt(&t->arena, "%S/%S", dir, name)));
        }
    }
    static const char *const queries[] = { "app", "mainbuf", "s/a", "win", "Search", "fz pr", "x", "con.h", "edit key", "map_1" };
    i32 compared = 0;
    for (i32 k = 0; k < ARRAY_COUNT(queries); k++) {
        String8 query = str8_cstr(queries[k]);
        // Typed one character at a time: narrowing (with passes cut short at random) equals full scoring.
        for (i64 len = 1; len <= query.len; len++) {
            String8 part = str8(query.data, len);
            fuzzy_rank_begin(&full, &x, part, 0);
            fuzzy_rank_finish(&full);
            fuzzy_rank_begin(&r, &x, part, len > 1);
            while (!fuzzy_rank_work(&r, 0, 1 + TEST_RNG() % 5000)) {} // sliced
            TEST_CHECK(t, test_fuzzy_same(&r, &full), "fuzzy: '%S' sliced and narrowed differs from full scoring (%D / %D matches)", part,
                       r.total, full.total);
            compared++;
        }
        // Cut short, then narrowed: the next pass keeps what was not looked at yet; finish equals full.
        fuzzy_rank_begin(&r, &x, str8(query.data, 1), 0);
        fuzzy_rank_work(&r, 0, 1 + TEST_RNG() % 15000);
        fuzzy_rank_begin(&r, &x, query, 1);
        fuzzy_rank_work(&r, 0, 1 + TEST_RNG() % 3000);
        fuzzy_rank_finish(&r);
        fuzzy_rank_begin(&full, &x, query, 0);
        fuzzy_rank_finish(&full);
        TEST_CHECK(t, test_fuzzy_same(&r, &full), "fuzzy: '%S' after a cut-short pass and a finish differs from full scoring", query);
        // Narrowed twice with no work in between (the first narrowing's list still being completed).
        if (query.len >= 3) {
            fuzzy_rank_begin(&r, &x, str8(query.data, 1), 0);
            fuzzy_rank_work(&r, 0, 1 + TEST_RNG() % 15000);
            fuzzy_rank_begin(&r, &x, str8(query.data, 2), 1);
            fuzzy_rank_begin(&r, &x, str8(query.data, 3), 1);
            fuzzy_rank_finish(&r);
            fuzzy_rank_begin(&full, &x, str8(query.data, 3), 0);
            fuzzy_rank_finish(&full);
            TEST_CHECK(t, test_fuzzy_same(&r, &full), "fuzzy: '%S' narrowed twice without work differs from full scoring", str8(query.data, 3));
        }
    }
    // Entries added during a pass are looked at too.
    fuzzy_rank_begin(&r, &x, STR8_LIT("zebra"), 0);
    fuzzy_rank_finish(&r);
    i64 before = r.total;
    project_index_add_file(&x, 0, STR8_LIT("zebra.c"), project_mask(str8_fmt(&t->arena, "%S/zebra.c", project_index_dir(&x, 0))));
    TEST_CHECK(t, !fuzzy_rank_done(&r), "fuzzy: a new entry makes the pass unfinished");
    fuzzy_rank_finish(&r);
    TEST_CHECK(t, r.total == before + 1, "fuzzy: the new entry matched (%D, was %D)", r.total, before);
#undef TEST_RNG
    fuzzy_rank_destroy(&r);
    fuzzy_rank_destroy(&full);
    project_index_destroy(&x);
    LOG("test: ok: fuzzy (%d ranking goldens: file name over directory, segment start, consecutive, exact name, smart case, '/', "
        "shorter on ties, several terms, word and camel starts; spans; %d prefixes sliced and narrowed equal to full scoring on "
        "20,000 paths; cut-short passes; entries added during a pass)", (i32)ARRAY_COUNT(cases), compared);
    return 1;
}

// Writes files (relative paths with '/'), making their directories.
static b32 test_make_tree(Test *t, String8 root, const char *const *files, i32 n) {
    os_make_dir(root);
    for (i32 i = 0; i < n; i++) {
        String8 rel = str8_cstr(files[i]);
        for (i64 k = 0; k < rel.len; k++) {
            if (rel.data[k] == '/') os_make_dir(str8_fmt(&t->arena, "%S\\%S", root, str8(rel.data, k)));
        }
        u8 *p = PUSH_ARRAY(&t->arena, u8, rel.len);
        for (i64 k = 0; k < rel.len; k++) p[k] = rel.data[k] == '/' ? '\\' : rel.data[k];
        if (!os_write_file(str8_fmt(&t->arena, "%S\\%S", root, str8(p, rel.len)), STR8_LIT("x\n"))) return 0;
    }
    return 1;
}

// Waits for the worker's batches until the scan ends; false after the timeout.
static b32 test_scan_finish(ProjectScan *s, ProjectIndex *x, u32 timeout_ms) {
    u64 start = os_time_us();
    while (!s->ended) {
        os_dev_wake_wait(20);
        project_scan_drain(s, x);
        if (!s->ended && os_time_us() - start > (u64)timeout_ms * 1000) return 0;
    }
    return 1;
}

// The index's paths, sorted, joined by ','.
static String8 test_index_paths(Test *t, ProjectIndex *x) {
    String8 *p = PUSH_ARRAY(&t->arena, String8, x->file_count);
    for (i64 i = 0; i < x->file_count; i++) {
        u8 *buf = PUSH_ARRAY(&t->arena, u8, PROJECT_PATH_MAX);
        p[i] = str8(buf, project_index_path(x, i, buf, PROJECT_PATH_MAX));
    }
    for (i64 i = 1; i < x->file_count; i++) { // insertion sort, bytewise
        String8 v = p[i];
        i64 j = i;
        for (; j > 0; j--) {
            String8 a = p[j - 1];
            i64 k = 0;
            while (k < a.len && k < v.len && a.data[k] == v.data[k]) k++;
            b32 greater = k < a.len && (k == v.len || a.data[k] > v.data[k]);
            if (!greater) break;
            p[j] = p[j - 1];
        }
        p[j] = v;
    }
    String8 out = str8(NULL, 0);
    for (i64 i = 0; i < x->file_count; i++) out = out.len ? str8_fmt(&t->arena, "%S,%S", out, p[i]) : p[i];
    return out;
}

static ProjectScan *test_scan_make(Test *t, String8 root, i64 max_files) {
    Config *c = PUSH_STRUCT(&t->arena, Config);
    config_init(c);
    config_parse(c, &t->arena, config_default_text(), STR8_LIT("<built-in>"));
    ProjectFile file;
    project_file_read(&t->arena, root, &file);
    return project_scan_create(root, str8(c->settings.project_ignore, c->settings.project_ignore_len), &file, max_files);
}

// The big generated tree (10,000 files in 100 directories), made once.
static String8 test_big_tree(Test *t) {
    String8 root = str8_fmt(&t->arena, "%S\\p11big", t->tmp_dir), done = str8_fmt(&t->arena, "%S\\p11big.done", t->tmp_dir);
    OsFileInfo info;
    if (os_file_info(done, &info) == OS_FILE_OK) return root;
    os_make_dir(root);
    for (i32 d = 0; d < 100; d++) {
        String8 dir = str8_fmt(&t->arena, "%S\\d%02d", root, d);
        os_make_dir(dir);
        u64 mark = arena_pos(&t->arena);
        for (i32 f = 0; f < 100; f++) os_write_file(str8_fmt(&t->arena, "%S\\file_%04d.c", dir, d * 100 + f), STR8_LIT("x"));
        arena_pop_to(&t->arena, mark);
    }
    os_write_file(done, STR8_LIT("10000 files\n"));
    return root;
}

// The scanner on a generated tree: the exact set (ignored directories, nested .gitignore, negation,
// .ronin-project, a junction looping back to the root), the ignored count, the file limit, a cancel in the
// middle, 300 rounds of start and cancel, a worker stuck in the file system.
static b32 test_scanner(Test *t, u64 seed) {
    i32 threads0 = os_dev_live_threads(), events0 = os_dev_live_events();
    String8 root = str8_fmt(&t->arena, "%S\\p11scan", t->tmp_dir);
    static const char *const files[] = {
        ".git/HEAD", "main.c", "readme.md", "app.log", "keep.log", "ronin.exe", "build/out.c", "node_modules/x.js",
        "src/a.c", "src/b.h", "src/gen/g.c", "src/x.log", "src/deep/d1/d2/z.c", "docs/draft/w.txt", "docs/guide.txt",
        "vendor/lib.c", "bin/tool.c",
    };
    TEST_CHECK(t, test_make_tree(t, root, files, (i32)ARRAY_COUNT(files)), "scanner: cannot make the tree");
    TEST_CHECK(t, os_write_file(str8_fmt(&t->arena, "%S\\.gitignore", root), STR8_LIT("*.log\nbuild/\nvendor/\n!keep.log\n")) &&
                  os_write_file(str8_fmt(&t->arena, "%S\\src\\.gitignore", root), STR8_LIT("gen/\n!*.log\n")) &&
                  os_write_file(str8_fmt(&t->arena, "%S\\.ronin-project", root), STR8_LIT("[ignore]\ndocs/draft/\n[allow]\nvendor/\n")),
               "scanner: cannot write the rules");
    String8 loop = str8_fmt(&t->arena, "%S\\src\\loop", root);
    os_dev_remove_dir(loop);
    TEST_CHECK(t, os_dev_make_junction(loop, root), "scanner: cannot make the junction");
    ProjectIndex x;
    project_index_init(&x);
    ProjectScan *s = test_scan_make(t, root, 1000000);
    TEST_CHECK(t, project_scan_start(s) && test_scan_finish(s, &x, 10000), "scanner: the scan did not end");
    String8 got = test_index_paths(t, &x);
    String8 want = STR8_LIT(".gitignore,.ronin-project,bin/tool.c,docs/guide.txt,keep.log,main.c,readme.md,src/.gitignore,src/a.c,"
                            "src/b.h,src/deep/d1/d2/z.c,src/x.log,vendor/lib.c");
    ProjectScanResult r = s->result;
    project_scan_free(s);
    TEST_CHECK(t, str8_equal(got, want), "scanner: listed '%S', expected '%S'", got, want);
    // Ignored: .git (built-in), app.log, ronin.exe (built-in), build (.gitignore), node_modules (built-in), src/gen (nested),
    // docs/draft (.ronin-project). The junction is not followed and not counted.
    TEST_CHECK(t, r.files == 13 && r.ignored == 7 && !r.limit && !r.cancelled && r.errors == 0, "scanner: %D files, %D ignored, %D errors",
               r.files, r.ignored, r.errors);
    // Breadth first: the root's files come before anything in a subdirectory.
    u8 first[PROJECT_PATH_MAX];
    i64 n0 = project_index_path(&x, 0, first, sizeof(first));
    b32 shallow = 1;
    for (i64 i = 0; i < x.file_count && i < 5; i++) shallow &= ((ProjectEntry *)x.files.base)[i].dir == 0;
    TEST_CHECK(t, n0 > 0 && shallow, "scanner: the root's files first");
    // The mask holds the path's characters, the directory part included.
    u64 m = project_mask(STR8_LIT("src/deep/d1/d2/z.c"));
    b32 mask_ok = 0;
    for (i64 i = 0; i < x.file_count; i++) mask_ok |= ((ProjectEntry *)x.files.base)[i].mask == m;
    TEST_CHECK(t, mask_ok, "scanner: the mask of a deep path");

    // The file limit.
    project_index_reset(&x);
    s = test_scan_make(t, root, 5);
    TEST_CHECK(t, project_scan_start(s) && test_scan_finish(s, &x, 10000), "scanner: limit: the scan did not end");
    r = s->result;
    project_scan_free(s);
    TEST_CHECK(t, r.limit && r.files == 5 && x.file_count == 5, "scanner: limit: %D files, limit %d", r.files, r.limit);
    // The junction loop goes again (git status --ignored would walk into it); test_run removes it too, should a
    // check above have returned first.
    OsFileInfo loop_info;
    TEST_CHECK(t, os_dev_remove_dir(loop) && os_file_info(loop, &loop_info) == OS_FILE_NOT_FOUND, "scanner: the junction was not removed");

    // A cancel in the middle: held after the first batch of the big tree, then cancelled.
    String8 big = test_big_tree(t);
    project_index_reset(&x);
    s = test_scan_make(t, big, 1000000);
    s->dev_hold = 1;
    s->dev_hold_after = 1;
    TEST_CHECK(t, project_scan_start(s), "scanner: cancel: no thread");
    for (i32 i = 0; i < 500 && !x.file_count; i++) {
        os_dev_wake_wait(20);
        project_scan_drain(s, &x);
    }
    TEST_CHECK(t, x.file_count > 0 && !s->ended, "scanner: cancel: the first batch (%D files) and still running", x.file_count);
    project_scan_cancel(s);
    TEST_CHECK(t, test_scan_finish(s, &x, 2000), "scanner: cancel: the worker did not end");
    r = s->result;
    project_scan_free(s);
    TEST_CHECK(t, r.cancelled && r.files < 10000 && x.file_count == r.files, "scanner: cancel: %D files of 10000, cancelled %d", r.files, r.cancelled);
    // The whole big tree, for the count.
    project_index_reset(&x);
    s = test_scan_make(t, big, 1000000);
    TEST_CHECK(t, project_scan_start(s) && test_scan_finish(s, &x, 20000), "scanner: big: the scan did not end");
    r = s->result;
    project_scan_free(s);
    TEST_CHECK(t, r.files == 10000 && x.file_count == 10000 && x.dir_count == 100, "scanner: big: %D files in %D directories", r.files, x.dir_count);

    // Stress: 300 rounds of start and cancel, at a random entry or at once; each worker must end within
    // 250 ms of its cancel.
    u64 rng = seed ^ 0x9e3779b97f4a7c15ull;
    u64 worst_us = 0;
    OsEvent pause;
    os_event_create(&pause);
    for (i32 round = 0; round < 300; round++) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        project_index_reset(&x);
        s = test_scan_make(t, big, 1000000);
        if (round % 3 == 0) { // it stands still at a random entry until the cancel
            s->dev_hold = 1;
            s->dev_hold_entries = 1 + (i64)((rng >> 33) % 10000);
        }
        TEST_CHECK(t, project_scan_start(s), "scanner: stress: no thread in round %d", round);
        if (round % 3 != 2) os_event_wait(pause, (u32)((rng >> 20) % 4)); // 0-3 ms in; every third round cancels at once
        u64 t0 = os_time_us();
        project_scan_cancel(s);
        b32 ended = test_scan_finish(s, &x, 250);
        u64 took = os_time_us() - t0;
        worst_us = MAX(worst_us, took);
        if (!ended) project_scan_join(s, 10000);
        project_scan_free(s);
        TEST_CHECK(t, ended, "scanner: stress: round %d: the worker took more than 250 ms after its cancel", round);
    }
    os_event_destroy(pause);
    TEST_CHECK(t, os_dev_live_threads() == threads0 && os_dev_live_events() == events0, "scanner: stress: %d threads, %d events left",
               os_dev_live_threads() - threads0, os_dev_live_events() - events0);

    // A worker stuck in the file system (deaf to cancel): the join gives up at its bound; released, it ends.
    s = test_scan_make(t, root, 1000000);
    s->dev_stuck = 1;
    TEST_CHECK(t, project_scan_start(s), "scanner: stuck: no thread");
    project_scan_cancel(s);
    u64 t0 = os_time_us();
    b32 joined = project_scan_join(s, 200);
    u64 waited = os_time_us() - t0;
    TEST_CHECK(t, !joined && waited >= 150 * 1000, "scanner: stuck: the join returned %d after %U us", joined, waited);
    os_atomic_set(&s->dev_stuck, 0);
    os_event_set(s->event);
    TEST_CHECK(t, project_scan_join(s, 10000), "scanner: stuck: released, it did not end");
    project_scan_free(s);
    project_index_destroy(&x);
    TEST_CHECK(t, os_dev_live_threads() == threads0 && os_dev_live_events() == events0, "scanner: threads or events left");
    LOG("test: ok: scanner (the exact set: built-in, .gitignore, nested .gitignore with negation, .ronin-project [ignore] and [allow], "
        "a junction not followed; 7 ignored; breadth first; the file limit; a cancel after the first batch; 10,000 files; 300 "
        "start / cancel rounds, the slowest end %U us after its cancel; a stuck worker's join bounded; no threads or events left)", worst_us);
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

    // ronin +120:8 file: line 120 centered, column 8 (1-based) is C7.
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

    // A directory (Phase 11): it becomes the project, *scratch* stays, no message.
    app = test_manual_app(t, dir, 0, 0, none);
    TEST_CHECK(t, app && test_current_is(app, "*scratch*") && !test_echo(app).len && str8_equal(app->finder.cmdline_dir, dir),
               "manual C4: a directory: echo '%S', the project '%S'", app ? test_echo(app) : none, app ? app->finder.cmdline_dir : none);
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

// I1 I2 I5 I6 I7: open-config creates ronin.conf; a saved change reloads it ("Reloaded"); tab_width,
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
    String8 dir = str8_fmt(&t->arena, "%S\\cfg", test_manual_dir(t)), conf = str8_fmt(&t->arena, "%S\\ronin.conf", dir);
    os_file_delete(conf);
    App *app = test_manual_app(t, str8(NULL, 0), 0, 0, conf);
    TEST_CHECK(t, app, "manual I1: app_create failed");
    app_dev_feed(app, "C-c ,", &t->arena);
    String8 created = str8_fmt(&t->arena, "Created %S from the built-in defaults", conf);
    TEST_CHECK(t, str8_equal(test_echo(app), created) && test_current_is(app, "ronin.conf") && test_file_is(t, conf, (const char *)config_default_text().data) &&
                  app->config_watch, "manual I1: open-config: '%S'", test_echo(app));

    TEST_CHECK(t, test_manual_reload(t, app, conf, STR8_LIT("[colors]\nbackground = #202020\n")) && test_echo_has(app, "Reloaded ronin.conf") &&
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
                  test_echo_has(app, "ronin.conf:2: unknown command 'no-such-command'"), "manual I7: echo '%S'", test_echo(app));
    app_dev_feed(app, "o k", &t->arena);
    TEST_CHECK(t, test_text_is(t, v->buffer, STR8_LIT("ok")), "manual I7: the editor keeps working");
    if (!test_app_destroy(t, app, "manual config")) return 0;
    LOG("test: ok: manual I1 I2 I5 I6 I7 (open-config creates and visits ronin.conf; a saved change reloads with 'Reloaded ronin.conf'; "
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

// Only the selected window shows its active region (Emacs' highlight-nonselected-windows nil), also
// while the minibuffer reads from it, and with two windows on one buffer.
static b32 test_window_region(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "region: app_create failed");
    test_win_lines(t, app, 300);
    app_dev_feed(app, "C-x 3 C-SPC C-n C-n", &t->arena);
    TEST_CHECK(t, app_dev_window_shows_region(app, 0) && !app_dev_window_shows_region(app, 1),
               "region: the selected window shows its region, the other (same buffer, no region) does not");
    app_dev_feed(app, "C-x o", &t->arena);
    TEST_CHECK(t, test_win(app, 0)->cursors[0].mark_active && !app_dev_window_shows_region(app, 0) && !app_dev_window_shows_region(app, 1),
               "region: after C-x o the left window's region is still active but not shown");
    app_dev_feed(app, "C-SPC C-n", &t->arena);
    TEST_CHECK(t, !app_dev_window_shows_region(app, 0) && app_dev_window_shows_region(app, 1), "region: the right window's own region is shown");
    app_dev_feed(app, "C-x o", &t->arena);
    TEST_CHECK(t, app_dev_window_shows_region(app, 0) && !app_dev_window_shows_region(app, 1), "region: back in the left window, its region is shown");
    app_dev_feed(app, "M-x", &t->arena);
    TEST_CHECK(t, app->mini.active && app_dev_window_shows_region(app, 0) && !app_dev_window_shows_region(app, 1),
               "region: while the minibuffer reads, the window it was called from still shows its region");
    app_dev_feed(app, "C-g", &t->arena);
    if (!test_app_destroy(t, app, "region")) return 0;
    LOG("test: ok: the region in windows (only the selected window shows its active region; C-x o and back; while the minibuffer "
        "reads; two windows on one buffer)");
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

// ---------------------------------------------------------------------------
// The project finder through the headless app (Phase 11)

static void test_frame(Test *t, App *app) {
    app_dev_feed_events(app, NULL, 0, &t->arena);
}

// Frames until the finder's ranking is done.
static void test_finder_settle(Test *t, App *app) {
    test_frame(t, app);
    for (i32 i = 0; i < 2000 && app->rank_pending; i++) test_frame(t, app);
}

static String8 test_finder_row(Test *t, App *app, i64 row) {
    u32 flags;
    String8 annotation;
    if (row >= app->mini.match_count) return STR8_LIT("(none)");
    return minibuffer_row_text(&app->mini, &t->arena, row, &flags, &annotation);
}

static String8 test_finder_status(Test *t, App *app) {
    const MiniMatcher *m = app->mini.matcher;
    return m && m->status ? m->status(app->mini.candidates_data, &t->arena) : str8(NULL, 0);
}

static b32 test_finder(Test *t) {
    i32 threads0 = os_dev_live_threads(), events0 = os_dev_live_events();
    String8 root = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p11find", t->tmp_dir));
    static const char *const files[] = { ".git/HEAD", "main.c", "src/app.c", "src/apple.c", "src/fuzzy.c", "src/map_app.c", "docs/readme.md",
                                         "build/out.c" };
    TEST_CHECK(t, test_make_tree(t, root, files, (i32)ARRAY_COUNT(files)), "finder: cannot make the tree");
    String8 newfile = str8_fmt(&t->arena, "%S\\src\\newfile.c", root);
    os_file_delete(newfile);
    String8 none = { 0 };

    // A file in a repository: C-x p f lists its tree, without waiting for the scan.
    App *app = test_manual_app(t, str8_fmt(&t->arena, "%S\\main.c", root), 0, 0, none);
    TEST_CHECK(t, app, "finder: app_create failed");
    app_dev_feed(app, "C-x p f", &t->arena);
    String8 prompt = app_dev_prompt(app);
    String8 want_prompt = str8_fmt(&t->arena, "Find file in %S/: ", root);
    for (i64 i = 0; i < want_prompt.len; i++) if (want_prompt.data[i] == '\\') want_prompt.data[i] = '/';
    TEST_CHECK(t, str8_equal(prompt, want_prompt), "finder: prompt '%S', expected '%S'", prompt, want_prompt);
    TEST_CHECK(t, finder_dev_wait(app, 5000), "finder: the scan did not end");
    TEST_CHECK(t, test_contains(test_echo(app), "7 files in ") && test_contains(test_echo(app), "/p11find/ (1 ignored)"),
               "finder: the end of the scan: '%S'", test_echo(app));
    test_finder_settle(t, app);
    TEST_CHECK(t, app->mini.match_count == 7 && str8_equal(test_finder_row(t, app, 0), STR8_LIT("main.c")),
               "finder: the empty query: %D rows, the first '%S' (the visited main.c)", app->mini.match_count, test_finder_row(t, app, 0));
    app_dev_feed(app, "f u z", &t->arena);
    test_finder_settle(t, app);
    TEST_CHECK(t, str8_equal(test_finder_row(t, app, 0), STR8_LIT("src/fuzzy.c")), "finder: 'fuz' ranks '%S' first", test_finder_row(t, app, 0));
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, !app->mini.active && test_current_is(app, "fuzzy.c"), "finder: RET visits src/fuzzy.c ('%S')", test_current(app)->name);
    // The empty query again: the files visited this session first, the most recent one first.
    app_dev_feed(app, "C-x p f", &t->arena);
    test_finder_settle(t, app);
    TEST_CHECK(t, str8_equal(test_finder_row(t, app, 0), STR8_LIT("src/fuzzy.c")) && str8_equal(test_finder_row(t, app, 1), STR8_LIT("main.c")) &&
                  app->mini.match_count == 7, "finder: visited first: '%S', '%S' (%D rows)", test_finder_row(t, app, 0), test_finder_row(t, app, 1),
               app->mini.match_count);
    app_dev_feed(app, "C-g", &t->arena);

    // One candidate a frame: the input and RET in one frame still visit the best match (RET finishes the
    // ranking first); typed without RET, the list has looked at one candidate.
    app->dev_work_budget = 1;
    test_app_feed_batch(t, app, "C-x p f a p p . c");
    TEST_CHECK(t, app->mini.active && app->mini.match_count <= 1 && app->rank_pending, "finder: one candidate a frame: %D rows after a frame",
               app->mini.match_count);
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, test_current_is(app, "app.c"), "finder: RET after one frame visits '%S', expected app.c", test_current(app)->name);
    test_app_feed_batch(t, app, "C-x p f m a p RET");
    TEST_CHECK(t, test_current_is(app, "map_app.c"), "finder: input and RET in one frame visit '%S', expected map_app.c", test_current(app)->name);
    app->dev_work_budget = 0;

    // A file added under the root: the tree watch marks the index stale; C-x p f shows the old list while
    // the rescan runs, then the new one.
    TEST_CHECK(t, os_write_file(newfile, STR8_LIT("new\n")), "finder: cannot write the new file");
    // Every notification within a while (the buffer directories' watches may come before the tree's).
    Event events[16];
    i32 got = 0;
    OsEvent pause;
    os_event_create(&pause);
    for (i32 i = 0; i < 100 && got < 16; i++) {
        got += os_dev_poll_watches(events + got, 16 - got);
        os_event_wait(pause, 10);
    }
    os_event_destroy(pause);
    TEST_CHECK(t, got > 0, "finder: no change notification for the new file");
    app_dev_feed_events(app, events, MIN(got, 16), &t->arena);
    TEST_CHECK(t, app->finder.open && app->finder.open->stale, "finder: the index is not stale");
    app_dev_feed(app, "C-x p f n e w f", &t->arena);
    test_finder_settle(t, app);
    TEST_CHECK(t, app->mini.match_count == 0 && str8_equal(test_finder_status(t, app), STR8_LIT("rescanning")),
               "finder: during the rescan the old list (%D rows, status '%S')", app->mini.match_count, test_finder_status(t, app));
    TEST_CHECK(t, finder_dev_wait(app, 5000), "finder: the rescan did not end");
    test_finder_settle(t, app);
    TEST_CHECK(t, str8_equal(test_finder_row(t, app, 0), STR8_LIT("src/newfile.c")), "finder: after the rescan '%S'", test_finder_row(t, app, 0));
    app_dev_feed(app, "C-g", &t->arena);
    os_file_delete(newfile);

    // The other window; C-x p f inside the minibuffer; project-refresh.
    app_dev_feed(app, "M-x p r o j e c t - f i n d - f i l e - o t h e r - w i n d o w RET", &t->arena);
    finder_dev_wait(app, 5000);
    app_dev_feed(app, "r e a d RET", &t->arena);
    TEST_CHECK(t, app_dev_window_count(app) == 2 && test_current_is(app, "readme.md"), "finder: the other window: %d windows, '%S'",
               app_dev_window_count(app), test_current(app)->name);
    app_dev_feed(app, "C-x 1 M-x C-x p f", &t->arena);
    TEST_CHECK(t, test_contains(test_echo(app), "Command attempted to use minibuffer while in minibuffer"), "finder: inside the minibuffer: '%S'",
               test_echo(app));
    app_dev_feed(app, "C-g M-x p r o j e c t - r e f r e s h RET", &t->arena);
    TEST_CHECK(t, test_contains(test_echo(app), "Scanning ") && test_contains(test_echo(app), "/p11find/..."), "finder: project-refresh: '%S'",
               test_echo(app));
    TEST_CHECK(t, finder_dev_wait(app, 5000) && test_contains(test_echo(app), "files in "), "finder: project-refresh ended: '%S'", test_echo(app));
    if (!test_app_destroy(t, app, "finder")) return 0;

    // ronin <directory> (no marker in it): *scratch* and C-x C-f start there, C-x p f lists it. A scan held
    // after its first batch: the finder says it is scanning and lists what is known; RET takes the best of
    // that; released, the list fills in.
    String8 big = os_full_path(&t->arena, test_big_tree(t));
    app = test_manual_app(t, big, 0, 0, none);
    TEST_CHECK(t, app && test_current_is(app, "*scratch*") && !test_echo(app).len, "finder: ronin <directory>: '%S', echo '%S'",
               app ? test_current(app)->name : none, app ? test_echo(app) : none);
    app_dev_feed(app, "C-x C-f", &t->arena);
    String8 initial = minibuffer_input(&app->mini, &t->arena);
    String8 want_initial = str8_fmt(&t->arena, "%S/", big);
    for (i64 i = 0; i < want_initial.len; i++) if (want_initial.data[i] == '\\') want_initial.data[i] = '/';
    TEST_CHECK(t, str8_equal(initial, want_initial), "finder: C-x C-f starts in the directory: '%S'", initial);
    app_dev_feed(app, "C-g", &t->arena);
    app->finder.dev_hold_after = 1;
    app_dev_feed(app, "C-x p f", &t->arena);
    Project *p = app->finder.open;
    for (i32 i = 0; i < 500 && p && !p->index[p->live].file_count; i++) {
        os_dev_wake_wait(20);
        app_wake(app);
    }
    test_finder_settle(t, app);
    String8 status = test_finder_status(t, app);
    i64 known = p ? p->index[p->live].file_count : 0;
    TEST_CHECK(t, p && p->scan && known > 0 && known < 10000 && str8_starts_with(status, STR8_LIT("scanning: ")) && app->mini.match_count > 0,
               "finder: while scanning: %D files known, status '%S', %D rows", known, status, app->mini.match_count);
    app_dev_feed(app, "f i l e _ 0 0 0 1 RET", &t->arena);
    TEST_CHECK(t, test_current_is(app, "file_0001.c") && p->scan, "finder: RET while scanning visits '%S'", test_current(app)->name);
    finder_dev_release(app);
    TEST_CHECK(t, finder_dev_wait(app, 10000) && p->index[p->live].file_count == 10000, "finder: released, the scan ends with %D files",
               p->index[p->live].file_count);
    app_dev_feed(app, "C-x p f f i l e _ 9 9 9 9", &t->arena);
    test_finder_settle(t, app);
    TEST_CHECK(t, str8_equal(test_finder_row(t, app, 0), STR8_LIT("d99/file_9999.c")), "finder: after the scan '%S'", test_finder_row(t, app, 0));
    app_dev_feed(app, "C-g", &t->arena);
    // Destroyed with a scan running: every worker ends quickly, none is abandoned.
    app_dev_feed(app, "M-x p r o j e c t - r e f r e s h RET", &t->arena);
    u64 t0 = os_time_us();
    if (!test_app_destroy(t, app, "finder: directory")) return 0;
    u64 took = os_time_us() - t0;
    TEST_CHECK(t, took < (u64)PROJECT_SHUTDOWN_MS * 1000 && app->finder.dev_abandoned == 0 && os_dev_live_threads() == threads0,
               "finder: shutdown with a scan running took %U us, %d abandoned, %d threads left", took, app->finder.dev_abandoned,
               os_dev_live_threads() - threads0);

    // project_max_files: the scan stops there and says so; the finder says where it stopped.
    String8 limit_conf = str8_fmt(&t->arena, "%S\\p11limit.conf", t->tmp_dir);
    TEST_CHECK(t, os_write_file(limit_conf, STR8_LIT("[settings]\nproject_max_files = 1000\n")), "finder: cannot write the config");
    app = test_manual_app(t, big, 0, 0, limit_conf);
    TEST_CHECK(t, app && app->config->settings.project_max_files == 1000, "finder: limit: the config");
    app_dev_feed(app, "C-x p f", &t->arena);
    TEST_CHECK(t, finder_dev_wait(app, 10000), "finder: limit: the scan did not end");
    test_finder_settle(t, app);
    TEST_CHECK(t, str8_equal(test_echo(app), STR8_LIT("Project has more than 1000 files; listed the first 1000")) &&
                  str8_equal(test_finder_status(t, app), STR8_LIT("stopped at 1000 files")) && app->mini.match_total == 1000,
               "finder: limit: echo '%S', status '%S', %D files", test_echo(app), test_finder_status(t, app), app->mini.match_total);
    app_dev_feed(app, "C-g", &t->arena);
    if (!test_app_destroy(t, app, "finder: limit")) return 0;

    // *scratch* started with no directory: not in a project.
    app = test_app_create(t);
    TEST_CHECK(t, app, "finder: app_create failed");
    app_dev_feed(app, "C-x p f", &t->arena);
    TEST_CHECK(t, !app->mini.active && str8_equal(test_echo(app), STR8_LIT(PROJECT_NOT_IN_PROJECT)), "finder: no project: '%S'", test_echo(app));
    if (!test_app_destroy(t, app, "finder: no project")) return 0;

    // A worker stuck in the file system at exit: the shutdown waits PROJECT_SHUTDOWN_MS in all, then leaves it.
    app = test_manual_app(t, str8_fmt(&t->arena, "%S\\main.c", root), 0, 0, none);
    TEST_CHECK(t, app, "finder: app_create failed");
    app->finder.dev_stuck_next = 1;
    app_dev_feed(app, "C-x p f", &t->arena);
    ProjectScan *stuck = app->finder.dev_last_scan;
    t0 = os_time_us();
    if (!test_app_destroy(t, app, "finder: stuck")) return 0;
    took = os_time_us() - t0;
    TEST_CHECK(t, stuck && app->finder.dev_abandoned == 1 && took >= (u64)(PROJECT_SHUTDOWN_MS - 100) * 1000 &&
                  took < (u64)(PROJECT_SHUTDOWN_MS + 500) * 1000, "finder: a stuck worker: shutdown in %U us, %d abandoned", took,
               app->finder.dev_abandoned);
    os_atomic_set(&stuck->dev_stuck, 0); // the test ends it after all
    os_event_set(stuck->event);
    os_dev_thread_reclaimed();
    TEST_CHECK(t, project_scan_join(stuck, 10000), "finder: the stuck worker did not end once released");
    project_scan_free(stuck);
    TEST_CHECK(t, os_dev_live_threads() == threads0 && os_dev_live_events() == events0, "finder: %d threads, %d events left",
               os_dev_live_threads() - threads0, os_dev_live_events() - events0);
    LOG("test: ok: project finder (the prompt at once, the end-of-scan message with the ignored count, the empty query with visited "
        "files first, a query and RET, RET with one candidate a frame and with the input in the same frame, a stale index shown "
        "until its rescan ends, the other window, inside the minibuffer, project-refresh, ronin <directory> with *scratch* and "
        "C-x C-f there, a held scan: status, partial list, RET, then filled; shutdown with a scan running in %U us; not in a project; "
        "a stuck worker abandoned after %d ms)", took, PROJECT_SHUTDOWN_MS);
    return 1;
}

// ---------------------------------------------------------------------------
// Phase 12: the search over files (grep.c)

// A file in memory, handed out in reads of at most `max_read` bytes (a random size each time when 0).
typedef struct TestGrepReader {
    Test *t;
    const u8 *data;
    i64 len, at, max_read;
} TestGrepReader;

static OsFileStatus test_grep_read(void *data, u8 *dst, i64 cap, i64 *got) {
    TestGrepReader *r = data;
    i64 most = r->max_read ? r->max_read : 1 + test_below(r->t, 700);
    *got = MIN(MIN(cap, most), r->len - r->at);
    memcpy(dst, r->data + r->at, (size_t)*got);
    r->at += *got;
    return OS_FILE_OK;
}

typedef struct TestGrepList {
    Test *t;
    GrepMatch *m;
    i32 count, cap;
} TestGrepList;

static b32 test_grep_keep(void *data, const GrepMatch *m) {
    TestGrepList *l = data;
    if (l->count == l->cap) return 0;
    GrepMatch *k = &l->m[l->count++];
    *k = *m;
    k->text = str8_copy(&l->t->arena, m->text);
    return 1;
}

// The rule of grep.h on a whole text: every match in order, its line, column and shown text.
static i32 test_grep_ref(Test *t, const SearchPattern *pat, String8 text, GrepMatch *out, i32 cap) {
    static const u8 ellipsis[3] = { 0xE2, 0x80, 0xA6 };
    i32 n = 0;
    i64 from = 0, line = 1, counted = 0, ls = 0;
    for (;;) {
        i64 c = search_bytes_forward(pat, text.data, 0, text.len, from, text.len);
        if (c < 0 || n == cap) break;
        for (; counted < c; counted++) {
            if (text.data[counted] != '\n') continue;
            line++;
            ls = counted + 1;
        }
        i64 le = c;
        while (le < text.len && text.data[le] != '\n') le++;
        i64 s0, s1;
        b32 pre = 0, post = 0;
        if (le - ls <= GREP_LINE_SHOW) {
            s0 = ls;
            s1 = le;
        } else {
            s0 = MAX(ls, c - GREP_CONTEXT);
            while (s0 < c && (text.data[s0] & 0xC0) == 0x80) s0++;
            s1 = MIN(le, s0 + GREP_LINE_SHOW);
            while (s1 > c && s1 < le && (text.data[s1] & 0xC0) == 0x80) s1--;
            pre = s0 > ls;
            post = s1 < le;
        }
        if (s1 > s0 && text.data[s1 - 1] == '\r' && !post) s1--;
        u8 *show = PUSH_ARRAY(&t->arena, u8, s1 - s0 + 6);
        i64 k = 0;
        if (pre) {
            memcpy(show, ellipsis, 3);
            k = 3;
        }
        memcpy(show + k, text.data + s0, (size_t)(s1 - s0));
        i64 off = k + c - s0;
        k += s1 - s0;
        if (post) {
            memcpy(show + k, ellipsis, 3);
            k += 3;
        }
        out[n++] = (GrepMatch){ .line = line, .col = c - ls, .len = pat->len, .text = str8(show, k), .match_off = off,
                                .match_shown = CLAMP(MIN(c + pat->len, s1) - c, 0, pat->len) };
        from = c + pat->len;
    }
    return n;
}

// grep_file on `file` (as stored) against the reference on `text` (its UTF-8 content); "" when equal,
// else what differs.
static String8 test_grep_compare(Test *t, GrepScan *sc, const SearchPattern *pat, String8 file, String8 text, i64 max_read,
                                 GrepFileResult *res) {
    GrepMatch *want = PUSH_ARRAY(&t->arena, GrepMatch, 4096), *got = PUSH_ARRAY(&t->arena, GrepMatch, 4096);
    i32 wn = test_grep_ref(t, pat, text, want, 4096);
    TestGrepReader r = { t, file.data, file.len, 0, max_read };
    TestGrepList l = { t, got, 0, 4096 };
    *res = grep_file(pat, sc, file.len, test_grep_read, &r, 4096, NULL, test_grep_keep, &l);
    if (l.count != wn) return str8_fmt(&t->arena, "%d matches, expected %d", l.count, wn);
    for (i32 i = 0; i < wn; i++) {
        GrepMatch *a = &got[i], *b = &want[i];
        if (a->line != b->line || a->col != b->col || a->len != b->len || !str8_equal(a->text, b->text) || a->match_off != b->match_off ||
            a->match_shown != b->match_shown) {
            return str8_fmt(&t->arena, "match %d: line %D col %D off %D shown %D '%S', expected line %D col %D off %D shown %D '%S'", i,
                            a->line, a->col, a->match_off, a->match_shown, a->text, b->line, b->col, b->match_off, b->match_shown, b->text);
        }
    }
    return str8(NULL, 0);
}

// UTF-8 (valid) to UTF-16 with a BOM.
static String8 test_grep_utf16(Test *t, String8 s, b32 big) {
    u8 *out = PUSH_ARRAY(&t->arena, u8, s.len * 4 + 2);
    i64 n = 0;
    out[n++] = big ? 0xFE : 0xFF;
    out[n++] = big ? 0xFF : 0xFE;
    for (i64 i = 0; i < s.len;) {
        i64 adv;
        u32 c = utf8_decode(s.data + i, s.len - i, &adv);
        i += adv;
        u32 units[2];
        i32 k = 0;
        if (c >= 0x10000) {
            units[k++] = 0xD800 + ((c - 0x10000) >> 10);
            units[k++] = 0xDC00 + ((c - 0x10000) & 0x3FF);
        } else {
            units[k++] = c;
        }
        for (i32 j = 0; j < k; j++) {
            out[n++] = (u8)(big ? units[j] >> 8 : units[j]);
            out[n++] = (u8)(big ? units[j] : units[j] >> 8);
        }
    }
    return str8(out, n);
}

static b32 test_grep_file(Test *t, u64 seed) {
    t->rng = seed ^ 0x6E3B;
    static const char *pieces[] = { "a", "b", "foo", "Foo", "FOO", "o", " ", "\t", "\n", "\n", "\r\n", "\xc4\x9f", "\xc4\x9e", "\xe2\x82\xac",
                                    "\xf0\x9f\x98\x80", "\x80", "\xc3", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
                                    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" };
    Arena *a = &t->arena;
    SearchPattern *pat = PUSH_STRUCT(a, SearchPattern);
    i64 rounds = 0, matches = 0, small_chunks = 0;
    u8 *text = PUSH_ARRAY(a, u8, KB(16));
    for (i32 round = 0; round < 2000; round++) {
        u64 mark = arena_pos(a);
        i64 n = 0, want_len = test_below(t, 4) ? test_below(t, 1500) : test_below(t, 9000);
        while (n < want_len) {
            String8 p = str8_cstr(pieces[test_below(t, ARRAY_COUNT(pieces))]);
            memcpy(text + n, p.data, (size_t)p.len);
            n += p.len;
        }
        String8 tx = str8(text, n);
        u8 nbuf[16];
        String8 needle;
        if (n && test_below(t, 2)) {
            i64 at = test_below(t, n), len = 1 + test_below(t, MIN(6, n - at));
            memcpy(nbuf, text + at, (size_t)len);
            needle = str8(nbuf, len);
        } else {
            needle = str8_cstr(test_below(t, 3) ? "foo" : test_below(t, 2) ? "o\no" : "a\r\n");
        }
        search_pattern_init(pat, needle, (b32)test_below(t, 2));
        i64 chunk = test_below(t, 3) ? 16 + test_below(t, 200) : 16 + test_below(t, 5000);
        small_chunks += chunk < 64;
        GrepScan *sc = PUSH_STRUCT(a, GrepScan);
        grep_scan_init(sc, a, chunk);
        GrepFileResult res;
        String8 diff = test_grep_compare(t, sc, pat, tx, tx, 0, &res);
        TEST_CHECK(t, !diff.len, "grep: round %d (seed 0x%X), needle '%S' fold %d, %D bytes, chunk %D: %S", round, seed, needle, pat->fold, n, chunk,
                   diff);
        matches += res.matches;
        rounds++;
        arena_pop_to(a, mark);
    }
    // Fixed cases, with chunks of 16 bytes (the minimum) unless said otherwise.
    struct { const char *text, *needle; i64 chunk; } fixed[] = {
        { "0123456789abcdefoo end", "foo", 16 },               // a match straddling the first two chunks
        { "0123456789abcde\nfoo\n", "foo", 16 },               // a newline exactly on a chunk boundary
        { "0123456789abcdef\nfoo\nfoo", "foo", 16 },           // ... one byte later
        { "fo", "foo", 16 },                                    // a file smaller than the needle
        { "", "foo", 16 },                                      // empty
        { "one o\nod line\nfoo\n", "o\no", 16 },               // a needle with a newline
        { "x\r\nfoo\r\nbar foo\r\n", "foo", 16 },              // CRLF: the CR is not shown
        { "foo\xc4\x9f\xc4\x9f\xc4\x9f", "\x9f", 16 },          // a needle starting inside a character: no match
    };
    GrepScan *sc = PUSH_STRUCT(a, GrepScan);
    grep_scan_init(sc, a, 16);
    for (i32 i = 0; i < ARRAY_COUNT(fixed); i++) {
        search_pattern_init(pat, str8_cstr(fixed[i].needle), 0);
        GrepFileResult res;
        String8 tx = str8_cstr(fixed[i].text);
        for (i64 rd = 1; rd <= 17; rd += 4) {
            String8 diff = test_grep_compare(t, sc, pat, tx, tx, rd, &res);
            TEST_CHECK(t, !diff.len, "grep: fixed case %d, reads of %D: %S", i, rd, diff);
        }
    }
    search_pattern_init(pat, STR8_LIT("foo"), 0);
    GrepFileResult res;
    // Exact expectations: the straddling match and the boundary newline.
    {
        GrepMatch m[4];
        TestGrepList l = { t, m, 0, 4 };
        TestGrepReader r = { t, (const u8 *)"0123456789abcdefoo end", 22, 0, 5 };
        res = grep_file(pat, sc, 22, test_grep_read, &r, 10, NULL, test_grep_keep, &l);
        TEST_CHECK(t, l.count == 1 && m[0].line == 1 && m[0].col == 15 && str8_equal(m[0].text, STR8_LIT("0123456789abcdefoo end")),
                   "grep: the match across the chunk boundary");
        l.count = 0;
        TestGrepReader r2 = { t, (const u8 *)"0123456789abcde\nfoo\n", 20, 0, 3 };
        res = grep_file(pat, sc, 20, test_grep_read, &r2, 10, NULL, test_grep_keep, &l);
        TEST_CHECK(t, l.count == 1 && m[0].line == 2 && m[0].col == 0 && str8_equal(m[0].text, STR8_LIT("foo")),
                   "grep: the newline on the chunk boundary counted once (line %D col %D)", l.count ? m[0].line : 0, l.count ? m[0].col : 0);
    }
    // A long line cut around the match, an invalid byte at the cut, a character cut in the middle.
    {
        u8 *long_line = PUSH_ARRAY(a, u8, 1200);
        i64 k = 0;
        for (i32 i = 0; i < 300; i++) long_line[k++] = "\xc4\x9f"[i & 1]; // 150 x ğ
        long_line[k++] = 0x80;
        memcpy(long_line + k, "foo", 3);
        k += 3;
        for (i32 i = 0; i < 400; i++) long_line[k++] = "\xe2\x82\xac"[i % 3];
        String8 tx = str8(long_line, k);
        for (i64 chunk = 16; chunk < 2000; chunk = chunk * 3 + 1) {
            GrepScan *s2 = PUSH_STRUCT(a, GrepScan);
            grep_scan_init(s2, a, chunk);
            String8 diff = test_grep_compare(t, s2, pat, tx, tx, 0, &res);
            TEST_CHECK(t, !diff.len, "grep: the long line, chunk %D: %S", chunk, diff);
        }
        GrepMatch m[2];
        TestGrepList l = { t, m, 0, 2 };
        TestGrepReader r = { t, long_line, k, 0, 0 };
        res = grep_file(pat, sc, k, test_grep_read, &r, 10, NULL, test_grep_keep, &l);
        TEST_CHECK(t, l.count == 1 && m[0].col == 301 && m[0].text.len <= GREP_SHOW_CAP && mem_equal(m[0].text.data, "\xe2\x80\xa6", 3) &&
                      mem_equal(m[0].text.data + m[0].text.len - 3, "\xe2\x80\xa6", 3) && mem_equal(m[0].text.data + m[0].match_off, "foo", 3),
                   "grep: the long line is cut around the match with an ellipsis at each end");
    }
    // Encodings and binary files.
    {
        String8 utf8 = STR8_LIT("x foo \xc4\x9f\n\xf0\x9f\x98\x80 FOO foo\nend");
        SearchPattern *fold = PUSH_STRUCT(a, SearchPattern);
        search_pattern_init(fold, STR8_LIT("foo"), 1);
        for (i32 big = 0; big < 2; big++) {
            String8 u16 = test_grep_utf16(t, utf8, big);
            for (i64 rd = 1; rd < 9; rd++) {
                String8 diff = test_grep_compare(t, sc, fold, u16, utf8, rd, &res);
                TEST_CHECK(t, !diff.len && res.kind == GREP_FILE_TEXT && res.matches == 3, "grep: UTF-16 %s, reads of %D: %S",
                           big ? "BE" : "LE", rd, diff);
            }
        }
        String8 bom = str8_fmt(a, "\xef\xbb\xbf%S", utf8);
        String8 diff = test_grep_compare(t, sc, fold, bom, utf8, 0, &res);
        TEST_CHECK(t, !diff.len && res.matches == 3, "grep: a UTF-8 BOM is skipped: %S", diff);
        u8 *bin = PUSH_ARRAY(a, u8, GREP_PROBE + 64);
        memset(bin, 'x', GREP_PROBE + 64);
        memcpy(bin + 100, "foo", 3);
        bin[GREP_PROBE - 1] = 0;
        TestGrepReader r = { t, bin, GREP_PROBE + 64, 0, 0 };
        TestGrepList l = { t, PUSH_ARRAY(a, GrepMatch, 4), 0, 4 };
        res = grep_file(pat, sc, GREP_PROBE + 64, test_grep_read, &r, 10, NULL, test_grep_keep, &l);
        TEST_CHECK(t, res.kind == GREP_FILE_BINARY && l.count == 0, "grep: a NUL in the first %d bytes: binary", (i32)GREP_PROBE);
        bin[GREP_PROBE - 1] = 'x';
        bin[GREP_PROBE] = 0;
        r.at = 0;
        res = grep_file(pat, sc, GREP_PROBE + 64, test_grep_read, &r, 10, NULL, test_grep_keep, &l);
        TEST_CHECK(t, res.kind == GREP_FILE_TEXT && l.count == 1, "grep: a NUL after the first %d bytes: searched", (i32)GREP_PROBE);
        // At most max_matches.
        l.count = 0;
        TestGrepReader r3 = { t, (const u8 *)"foo foo foo foo", 15, 0, 0 };
        res = grep_file(pat, sc, 15, test_grep_read, &r3, 2, NULL, test_grep_keep, &l);
        TEST_CHECK(t, l.count == 2 && res.stopped, "grep: max_matches 2 gave %d", l.count);
    }
    LOG("test: ok: grep file: %D random files against the reference (%D matches; %D with chunks under 64 bytes, reads of 1-700 bytes), "
        "%d fixed cases (a match across chunks, a newline on a boundary, a file smaller than the needle, a needle with a newline, CRLF, "
        "a needle inside a character), a long line cut with an invalid byte, UTF-16 LE / BE, a UTF-8 BOM, binary, max_matches",
        rounds, matches, small_chunks, (i32)ARRAY_COUNT(fixed));
    return 1;
}

// A generated tree's search through the workers.
typedef struct TestGrepRun {
    Test *t;
    i32 files;              // list size
    i32 *done;              // DONE records per file
    GrepFileKind *kind;
    String8 *lines;
    i32 line_count, line_cap;
    i64 records;
} TestGrepRun;

static void test_grep_record(void *data, const GrepRecord *r) {
    TestGrepRun *run = data;
    if (r->kind == GREP_REC_DONE) {
        if (r->file < (u32)run->files) {
            run->done[r->file]++;
            run->kind[r->file] = r->status;
        }
    } else if (run->line_count < run->line_cap) {
        run->lines[run->line_count++] = str8_fmt(&run->t->arena, "%u:%D:%D:%S", r->file, r->match.line, r->match.col, r->match.text);
    }
}

// Drains until every worker ended; false after the timeout.
static b32 test_grep_finish(GrepSearch *s, TestGrepRun *run, u32 timeout_ms) {
    u64 start = os_time_us();
    while (!s->ended) {
        os_dev_wake_wait(20);
        grep_drain(s, test_grep_record, run);
        if (!s->ended && os_time_us() - start > (u64)timeout_ms * 1000) return 0;
    }
    return 1;
}

static void test_grep_run_init(Test *t, TestGrepRun *run, i32 files) {
    memset(run, 0, sizeof(*run));
    run->t = t;
    run->files = files;
    run->done = PUSH_ARRAY(&t->arena, i32, files);
    run->kind = PUSH_ARRAY(&t->arena, GrepFileKind, files);
    run->line_cap = 4096;
    run->lines = PUSH_ARRAY(&t->arena, String8, run->line_cap);
}

// The collected match lines, sorted and joined by '|'.
static String8 test_grep_lines(Test *t, TestGrepRun *run) {
    for (i32 i = 1; i < run->line_count; i++) {
        String8 v = run->lines[i];
        i32 j = i;
        for (; j > 0 && grep_path_compare(run->lines[j - 1], v) > 0; j--) run->lines[j] = run->lines[j - 1];
        run->lines[j] = v;
    }
    String8 out = str8(NULL, 0);
    for (i32 i = 0; i < run->line_count; i++) out = out.len ? str8_fmt(&t->arena, "%S|%S", out, run->lines[i]) : run->lines[i];
    return out;
}

static b32 test_grep_search(Test *t, u64 seed) {
    i32 threads0 = os_dev_live_threads(), events0 = os_dev_live_events();
    while (os_dev_wake_wait(0)) {}
    String8 root = str8_fmt(&t->arena, "%S\\p12grep", t->tmp_dir);
    os_make_dir(root);
    os_make_dir(str8_fmt(&t->arena, "%S\\b", root));
    static const struct { const char *rel; const char *text; i64 len; } files[] = {
        { "a.c", "int foo;\nfoo(); foo\n", 20 },
        { "b/c.txt", "nothing here\n", 13 },
        { "b/d.h", "x\r\nFOO bar\r\n", 12 },
        { "bin.dat", "foo\0bar", 7 },
        { "mod.c", "foo on disk\n", 12 },
        { "u16.txt", "\xff\xfe" "f\0o\0o\0 \0\xe9\0\n\0", 14 },
        { "empty.txt", "", 0 },
        { "missing.txt", NULL, 0 }, // listed, not on disk: unreadable
    };
    i32 nf = (i32)ARRAY_COUNT(files);
    for (i32 i = 0; i < nf; i++) {
        String8 path = str8_fmt(&t->arena, "%S\\%s", root, files[i].rel);
        for (i64 k = root.len; k < path.len; k++) if (path.data[k] == '/') path.data[k] = '\\';
        if (files[i].text) TEST_CHECK(t, os_write_file(path, str8((u8 *)files[i].text, files[i].len)), "grep search: cannot write %S", path);
        else os_file_delete(path);
    }
    const char *want = "0:1:4:int foo;|0:2:0:foo(); foo|0:2:7:foo(); foo|2:2:0:FOO bar|5:1:0:foo \xc3\xa9";
    static const GrepFileKind want_kind[] = { GREP_FILE_TEXT, GREP_FILE_TEXT, GREP_FILE_TEXT, GREP_FILE_BINARY, GREP_FILE_MEMORY,
                                              GREP_FILE_TEXT, GREP_FILE_TEXT, GREP_FILE_UNREADABLE };
    String8 skip = STR8_LIT("MOD.c"); // case-insensitive
    static const i32 worker_counts[] = { 1, 2, 4, 8 };
    for (i32 w = 0; w < ARRAY_COUNT(worker_counts); w++) {
        for (i32 rep = 0; rep < 3; rep++) {
            GrepSearch *s = grep_create(root, STR8_LIT("foo"), 1, 100, worker_counts[w]);
            grep_skip(s, &skip, 1);
            // The list arrives in two pieces, as from a running scan: some before the start, the rest after.
            i32 first = rep == 0 ? nf : rep == 1 ? 0 : 3;
            for (i32 i = 0; i < first; i++) grep_list_add(s, str8_cstr(files[i].rel));
            grep_list_publish(s, first == nf);
            TEST_CHECK(t, grep_start(s), "grep search: no workers");
            for (i32 i = first; i < nf; i++) grep_list_add(s, str8_cstr(files[i].rel));
            if (first < nf) grep_list_publish(s, 1);
            TestGrepRun run;
            test_grep_run_init(t, &run, nf);
            TEST_CHECK(t, test_grep_finish(s, &run, 10000), "grep search: did not end (%d workers)", worker_counts[w]);
            grep_free(s);
            String8 got = test_grep_lines(t, &run);
            TEST_CHECK(t, str8_equal(got, str8_cstr(want)), "grep search, %d workers, list %d before the start: '%S', expected '%s'",
                       worker_counts[w], first, got, want);
            for (i32 i = 0; i < nf; i++) {
                TEST_CHECK(t, run.done[i] == 1 && run.kind[i] == want_kind[i], "grep search, %d workers: %s done %d time(s) as %d, expected once as %d",
                           worker_counts[w], files[i].rel, run.done[i], (i32)run.kind[i], (i32)want_kind[i]);
            }
        }
    }
    // At most max_matches per file.
    {
        GrepSearch *s = grep_create(root, STR8_LIT("foo"), 1, 1, 2);
        grep_skip(s, &skip, 1);
        for (i32 i = 0; i < nf; i++) grep_list_add(s, str8_cstr(files[i].rel));
        grep_list_publish(s, 1);
        TEST_CHECK(t, grep_start(s), "grep search: no workers");
        TestGrepRun run;
        test_grep_run_init(t, &run, nf);
        TEST_CHECK(t, test_grep_finish(s, &run, 10000), "grep search: max 1 did not end");
        grep_free(s);
        String8 got = test_grep_lines(t, &run);
        TEST_CHECK(t, str8_equal(got, STR8_LIT("0:1:4:int foo;|2:2:0:FOO bar|5:1:0:foo \xc3\xa9")),
                   "grep search: max_matches 1 per file: '%S'", got);
    }
    // The claim limit: nothing at or past it is claimed until it is raised.
    {
        GrepSearch *s = grep_create(root, STR8_LIT("foo"), 1, 100, 4);
        for (i32 i = 0; i < nf; i++) grep_list_add(s, str8_cstr(files[i].rel));
        grep_set_claim_limit(s, 2);
        grep_list_publish(s, 1);
        TEST_CHECK(t, grep_start(s), "grep search: no workers");
        TestGrepRun run;
        test_grep_run_init(t, &run, nf);
        u64 start = os_time_us();
        while (run.done[0] + run.done[1] < 2 && os_time_us() - start < 5000000) {
            os_dev_wake_wait(20);
            grep_drain(s, test_grep_record, &run);
        }
        os_dev_wake_wait(50);
        grep_drain(s, test_grep_record, &run);
        i32 beyond = 0;
        for (i32 i = 2; i < nf; i++) beyond += run.done[i];
        TEST_CHECK(t, run.done[0] == 1 && run.done[1] == 1 && beyond == 0 && os_atomic_get(&s->claimed) == 2,
                   "grep search: the claim limit 2 let %d file(s) past it (claimed %d)", beyond, os_atomic_get(&s->claimed));
        grep_set_claim_limit(s, nf);
        TEST_CHECK(t, test_grep_finish(s, &run, 10000), "grep search: did not end after the limit was raised");
        grep_free(s);
        for (i32 i = 0; i < nf; i++) TEST_CHECK(t, run.done[i] == 1, "grep search: after the limit, %s done %d time(s)", files[i].rel, run.done[i]);
    }
    // A cancel while every worker stands still after its first file: all end within 250 ms.
    u64 worst_us = 0;
    {
        GrepSearch *s = grep_create(root, STR8_LIT("foo"), 1, 100, 4);
        s->dev_hold = 1;
        s->dev_hold_after = 1;
        for (i32 i = 0; i < nf; i++) grep_list_add(s, str8_cstr(files[i].rel));
        grep_list_publish(s, 1);
        TEST_CHECK(t, grep_start(s), "grep search: no workers");
        TestGrepRun run;
        test_grep_run_init(t, &run, nf);
        os_dev_wake_wait(100);
        grep_drain(s, test_grep_record, &run);
        u64 t0 = os_time_us();
        grep_cancel(s);
        TEST_CHECK(t, test_grep_finish(s, &run, 250), "grep search: workers still running 250 ms after a cancel");
        worst_us = os_time_us() - t0;
        grep_free(s);
    }
    // 300 rounds of start and cancel on the 10,000-file tree, at random moments.
    t->rng = seed ^ 0x12C0;
    {
        String8 big = test_big_tree(t);
        u64 mark = arena_pos(&t->arena);
        OsEvent pause;
        TEST_CHECK(t, os_event_create(&pause), "grep stress: no event");
        for (i32 round = 0; round < 300; round++) {
            GrepSearch *s = grep_create(big, STR8_LIT("x"), 0, 100, 1 + (i32)test_below(t, 8));
            for (i32 d = 0; d < 100; d++) {
                for (i32 f = 0; f < 100; f++) grep_list_add(s, str8_fmt(&t->arena, "d%02d/file_%04d.c", d, d * 100 + f));
                arena_pop_to(&t->arena, mark);
            }
            if (round % 3 == 0) {
                s->dev_hold = 1;
                s->dev_hold_after = 1 + (i32)test_below(t, 200);
            }
            grep_list_publish(s, 1);
            TEST_CHECK(t, grep_start(s), "grep stress: no workers");
            if (round % 3 != 2) os_event_wait(pause, (u32)test_below(t, 4));
            u64 t0 = os_time_us();
            grep_cancel(s);
            TestGrepRun run;
            test_grep_run_init(t, &run, 10000);
            b32 ended = test_grep_finish(s, &run, 250);
            u64 took = os_time_us() - t0;
            worst_us = MAX(worst_us, took);
            if (!ended) {
                grep_join(s, os_time_us() + 10000000);
                grep_free(s);
                TEST_CHECK(t, 0, "grep stress: round %d: workers still running %U us after the cancel", round, took);
            }
            grep_free(s);
            arena_pop_to(&t->arena, mark);
        }
        os_event_destroy(pause);
    }
    // A worker stuck in the file system: the join gives up at its deadline; released, it ends.
    {
        GrepSearch *s = grep_create(root, STR8_LIT("foo"), 1, 100, 2);
        s->dev_stuck = 1;
        grep_list_add(s, STR8_LIT("a.c"));
        grep_list_publish(s, 1);
        TEST_CHECK(t, grep_start(s), "grep search: no workers");
        grep_cancel(s);
        u64 t0 = os_time_us();
        b32 joined = grep_join(s, t0 + 200 * 1000);
        u64 took = os_time_us() - t0;
        TEST_CHECK(t, !joined && took >= 190000 && took < 1000000, "grep search: a stuck worker's join returned %d after %U us", joined, took);
        grep_dev_release(s);
        TEST_CHECK(t, grep_join(s, os_time_us() + 10000000), "grep search: the stuck workers did not end once released");
        TestGrepRun run;
        test_grep_run_init(t, &run, 1);
        grep_free(s);
    }
    TEST_CHECK(t, os_dev_live_threads() == threads0 && os_dev_live_events() == events0, "grep search: %d threads, %d events left",
               os_dev_live_threads() - threads0, os_dev_live_events() - events0);
    LOG("test: ok: grep search (the exact matches with 1, 2, 4 and 8 workers, 3 runs each, the list complete before the start, "
        "after it, and in two pieces; binary, UTF-16, a skipped (modified) file, an empty one, a missing one: each file done once; "
        "max_matches; the claim limit; a cancel while held; 300 start / cancel rounds on 10,000 files, the slowest end %U us after "
        "its cancel; a stuck worker's bounded join)", worst_us);
    return 1;
}

// A search never makes a save, a rename or a delete fail: two files held open by the workers.
static b32 test_grep_sharing(Test *t) {
    String8 root = str8_fmt(&t->arena, "%S\\p12share", t->tmp_dir);
    os_make_dir(root);
    String8 p1 = str8_fmt(&t->arena, "%S\\held1.txt", root), p2 = str8_fmt(&t->arena, "%S\\held2.txt", root);
    String8 p2b = str8_fmt(&t->arena, "%S\\held2_renamed.txt", root);
    os_file_delete(p2b);
    TEST_CHECK(t, os_write_file(p1, STR8_LIT("foo one\n")) && os_write_file(p2, STR8_LIT("foo two\n")), "grep sharing: cannot write");
    GrepSearch *s = grep_create(root, STR8_LIT("foo"), 1, 100, 2);
    s->dev_hold = 1;
    s->dev_hold_open = 1;
    grep_list_add(s, STR8_LIT("held1.txt"));
    grep_list_add(s, STR8_LIT("held2.txt"));
    grep_list_publish(s, 1);
    TEST_CHECK(t, grep_start(s), "grep sharing: no workers");
    // Both files claimed, and each worker inside its first read (it holds the file open there).
    OsEvent pause;
    os_event_create(&pause);
    for (i32 i = 0; i < 200 && os_atomic_get(&s->claimed) < 2; i++) os_event_wait(pause, 5);
    os_event_wait(pause, 100);
    b32 claimed = os_atomic_get(&s->claimed) == 2;
    // Save held1.txt from a buffer: the temp file and ReplaceFileW.
    Buffer *buf = buffer_create(STR8_LIT("held1.txt"));
    OsFileStatus load = buffer_load_file(buf, p1);
    buffer_replace(buf, 0, 0, STR8_LIT("saved "));
    OsFileStatus save = buffer_save(buf);
    b32 swapped = buf->dev_saved_by_swap;
    buffer_destroy(buf);
    // Rename held2.txt, then delete it.
    OsFileStatus moved = os_file_replace(p2b, p2);
    OsFileInfo info;
    b32 renamed = os_file_info(p2, &info) == OS_FILE_NOT_FOUND && os_file_info(p2b, &info) == OS_FILE_OK;
    os_file_delete(p2b);
    OsFileStatus after = os_file_info(p2b, &info);
    grep_dev_release(s);
    TestGrepRun run;
    test_grep_run_init(t, &run, 2);
    b32 ended = test_grep_finish(s, &run, 10000);
    grep_free(s);
    os_event_destroy(pause);
    String8 content = { 0 };
    test_read_file(t, p1, &content);
    TEST_CHECK(t, claimed, "grep sharing: the workers did not claim both files");
    TEST_CHECK(t, load == OS_FILE_OK && save == OS_FILE_OK && swapped, "grep sharing: saving a file a worker holds open: load %d, save %d, "
               "through the swap %d", (i32)load, (i32)save, swapped);
    TEST_CHECK(t, str8_equal(content, STR8_LIT("saved foo one\n")), "grep sharing: the saved file holds '%S'", content);
    TEST_CHECK(t, moved == OS_FILE_OK && renamed, "grep sharing: renaming a file a worker holds open: %d", (i32)moved);
    TEST_CHECK(t, after == OS_FILE_NOT_FOUND, "grep sharing: deleting a file a worker holds open left it there (%d)", (i32)after);
    TEST_CHECK(t, ended && run.done[0] == 1 && run.done[1] == 1, "grep sharing: the search did not end normally after the release");
    LOG("test: ok: grep sharing (two files held open by the workers: one saved through the temp file and ReplaceFileW, one renamed "
        "and deleted; the search then ends normally)");
    return 1;
}

// ---------------------------------------------------------------------------
// Phase 12: results buffers, project-search, next-error (the headless app, real workers)

static Buffer *test_results_buffer(App *app) {
    return buffer_list_find_name(&app->buffers, STR8_LIT("*search*"));
}

// *search*'s text without the time at the end of its last line (", 0.02 s").
static String8 test_results_text(Test *t, App *app) {
    Buffer *b = test_results_buffer(app);
    if (!b) return STR8_LIT("(no *search*)");
    String8 s = buffer_text(b, &t->arena, 0, buffer_size(b));
    i64 end = s.len;
    while (end > 0 && s.data[end - 1] != ',') end--;
    if (end > 0 && s.len - end <= 12 && s.data[s.len - 1] == 's') s.len = end - 1;
    return str8_copy(&t->arena, s);
}

// A config with these settings, as a file (its path).
static String8 test_results_conf(Test *t, const char *name, const char *text) {
    String8 path = str8_fmt(&t->arena, "%S\\%s", t->tmp_dir, name);
    os_write_file(path, str8_cstr(text));
    return path;
}

// The point of the selected window and its buffer's name, as "name:offset".
static String8 test_results_where(Test *t, App *app) {
    return str8_fmt(&t->arena, "%S:%D", test_current(app)->name, test_app_point(app));
}

static b32 test_results(Test *t) {
    i32 threads0 = os_dev_live_threads(), events0 = os_dev_live_events();
    while (os_dev_wake_wait(0)) {}
    String8 root = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p12res", t->tmp_dir));
    os_make_dir(root);
    os_make_dir(str8_fmt(&t->arena, "%S\\.git", root));
    os_make_dir(str8_fmt(&t->arena, "%S\\b", root));
    os_write_file(str8_fmt(&t->arena, "%S\\.git\\HEAD", root), STR8_LIT("ref\n"));
    os_write_file(str8_fmt(&t->arena, "%S\\.ronin-project", root), STR8_LIT("[ignore]\nignored.c\n"));
    os_write_file(str8_fmt(&t->arena, "%S\\a.c", root), STR8_LIT("int foo;\nfoo(); foo\n"));
    os_write_file(str8_fmt(&t->arena, "%S\\bin.dat", root), str8((u8 *)"foo\0bar", 7));
    os_write_file(str8_fmt(&t->arena, "%S\\ignored.c", root), STR8_LIT("foo ignored\n"));
    os_write_file(str8_fmt(&t->arena, "%S\\u16.txt", root), str8((u8 *)"\xff\xfe" "f\0o\0o\0 \0\xe9\0\n\0", 14));
    os_write_file(str8_fmt(&t->arena, "%S\\b\\c.txt", root), STR8_LIT("foo on disk\n"));
    os_write_file(str8_fmt(&t->arena, "%S\\b\\d.h", root), STR8_LIT("x\r\nFOO bar\r\n"));
    os_file_delete(str8_fmt(&t->arena, "%S\\new.c", root));
    {   // A large file early in the list: the files after it finish first.
        u8 *big = PUSH_ARRAY(&t->arena, u8, 20000 * 21 + 64);
        i64 n = 0;
        for (i32 i = 0; i < 20000; i++) n += fmt_buf(big + n, 32, "line %05d xxxxxxxxx\n", i);
        n += fmt_buf(big + n, 32, "the foo at the end\n");
        os_write_file(str8_fmt(&t->arena, "%S\\big.txt", root), str8(big, n));
    }
    String8 shown_root = str8_fmt(&t->arena, "%S/", root);
    for (i64 i = 0; i < shown_root.len; i++) if (shown_root.data[i] == '\\') shown_root.data[i] = '/';
    String8 want = str8_fmt(&t->arena,
                            "Search for \"foo\" in %S (ignoring case)\n\n"
                            "a.c:1:5: int foo;\n"
                            "a.c:2:1: foo(); foo\n"
                            "a.c:2:8: foo(); foo\n"
                            "big.txt:20001:5: the foo at the end\n"
                            "u16.txt:1:1: foo \xc3\xa9\n"
                            "b/c.txt:1:1: foo on disk\n"
                            "b/d.h:2:1: FOO bar\n"
                            "7 matches in 5 files; 6 files searched, 1 binary skipped",
                            shown_root);
    String8 a_c = str8_fmt(&t->arena, "%S\\a.c", root);
    String8 none = { 0 };

    // The same text, and the same next-error order, with 1, 2, 4 and 8 workers, and run again with g.
    static const char *const confs[] = { "[settings]\nsearch_workers = 1\n", "[settings]\nsearch_workers = 2\n",
                                         "[settings]\nsearch_workers = 4\n", "[settings]\nsearch_workers = 8\n" };
    String8 order0 = { 0 };
    for (i32 c = 0; c < ARRAY_COUNT(confs); c++) {
        App *app = test_manual_app(t, a_c, 0, 0, test_results_conf(t, "p12res.conf", confs[c]));
        TEST_CHECK(t, app, "results: app_create failed");
        app_dev_feed(app, "C-x p g f o o RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "results: the search did not end (%d)", c);
        for (i32 rep = 0; rep < (c == 2 ? 5 : 1); rep++) {
            if (rep) {
                app_dev_feed(app, "g", &t->arena);
                TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "results: g did not end");
            }
            String8 got = test_results_text(t, app);
            TEST_CHECK(t, str8_equal(got, want), "results, config %d run %d: *search* holds\n%S\n---- expected\n%S", c, rep, got, want);
        }
        TEST_CHECK(t, test_current(app) == test_results_buffer(app) && app_dev_window_count(app) == 2,
                   "results: *search* is shown in another window and selected");
        // next-error from the other window, all the way, then past the end.
        app_dev_feed(app, "C-x o", &t->arena);
        String8 order = str8(NULL, 0);
        for (i32 i = 0; i < 7; i++) {
            app_dev_feed(app, "M-g n", &t->arena);
            order = str8_fmt(&t->arena, "%S %S", order, test_results_where(t, app));
        }
        if (!order0.len) order0 = order;
        TEST_CHECK(t, str8_equal(order, order0), "results: next-error order '%S', with one worker '%S'", order, order0);
        app_dev_feed(app, "M-g n", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "No more matches"), "results: next-error past the end says '%S'", test_echo(app));
        if (!test_app_destroy(t, app, "results")) return 0;
    }
    TEST_CHECK(t, str8_equal(order0, STR8_LIT(" a.c:4 a.c:9 a.c:16 big.txt:420004 u16.txt:0 c.txt:0 d.h:2")),
               "results: next-error visited '%S'", order0);

    // RET, n, p, g, q; previous-error at both ends; next-error with no results buffer.
    {
        App *app = test_manual_app(t, a_c, 0, 0, none);
        app_dev_feed(app, "M-g n", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "No next-error capable buffer found"), "results: next-error with nothing: '%S'", test_echo(app));
        app_dev_feed(app, "C-x p g f o o RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "results: the search did not end");
        Buffer *rb = test_results_buffer(app);
        TEST_CHECK(t, test_current(app) == rb && buffer_line_of(rb, test_app_point(app)) == 2, "results: point on the first match's line");
        app_dev_feed(app, "RET", &t->arena);
        TEST_CHECK(t, str8_equal(test_results_where(t, app), STR8_LIT("a.c:4")) && app_dev_window_count(app) == 2,
                   "results: RET visits a.c:1:5 in the other window and selects it ('%S')", test_results_where(t, app));
        app_dev_feed(app, "C-x o n", &t->arena);
        TEST_CHECK(t, test_current(app) == rb && buffer_line_of(rb, test_app_point(app)) == 3, "results: n stays in *search*, on the next line");
        View *views[WINDOW_MAX];
        i32 nv = app_views(app, views);
        View *other = views[0] == app_selected_view(app) ? views[1] : views[0];
        TEST_CHECK(t, nv == 2 && str8_equal(other->buffer->name, STR8_LIT("a.c")) && view_point(other, &other->cursors[0]) == 9,
                   "results: n shows a.c:2:1 in the other window");
        app_dev_feed(app, "p", &t->arena);
        TEST_CHECK(t, buffer_line_of(rb, test_app_point(app)) == 2, "results: p goes back");
        app_dev_feed(app, "p", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "No more matches"), "results: p before the first says '%S'", test_echo(app));
        app_dev_feed(app, "M-<", &t->arena);
        app_dev_feed(app, "RET", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "No match on this line"), "results: RET on the header says '%S'", test_echo(app));
        // previous-error (p left the first match current): to the second and back to the first, then the start.
        app_dev_feed(app, "M-g n M-g p", &t->arena);
        TEST_CHECK(t, str8_equal(test_results_where(t, app), STR8_LIT("a.c:4")), "results: previous-error to a.c:4 ('%S')", test_results_where(t, app));
        app_dev_feed(app, "M-g p", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "No more matches"), "results: previous-error before the first says '%S'", test_echo(app));
        // q closes the results window.
        app_dev_feed(app, "C-x o", &t->arena);
        if (test_current(app) != rb) app_dev_feed(app, "C-x o", &t->arena);
        app_dev_feed(app, "q", &t->arena);
        TEST_CHECK(t, app_dev_window_count(app) == 1 && test_current(app) != rb, "results: q closes the results window");
        // The keymap stays in *search*: n types n elsewhere (a read-only buffer would refuse it).
        app_dev_feed(app, "C-x b * s c r a t c h * RET n", &t->arena);
        TEST_CHECK(t, test_current_is(app, "*scratch*") && str8_equal(test_app_text(t, app), STR8_LIT("n")),
                   "results: n types n in *scratch* ('%S')", test_app_text(t, app));
        app_dev_feed(app, "C-x 3 C-x b * s e a r c h * RET C-x o n", &t->arena);
        TEST_CHECK(t, test_current_is(app, "*scratch*") && str8_equal(test_app_text(t, app), STR8_LIT("nn")),
                   "results: n types n in the window next to *search*");
        app_dev_feed(app, "C-x o C-h k n", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "n runs the command next-error-no-select"), "results: describe-key in *search*: '%S'", test_echo(app));
        if (!test_app_destroy(t, app, "results")) return 0;
    }

    // The prompt's default: the symbol at point, the region; not in a project.
    {
        App *app = test_manual_app(t, a_c, 1, 6, none); // point on "foo"
        app_dev_feed(app, "C-x p g", &t->arena);
        TEST_CHECK(t, str8_equal(app_dev_prompt(app), STR8_LIT("Search project for (default foo): ")), "results: the prompt '%S'", app_dev_prompt(app));
        app_dev_feed(app, "RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000) && str8_equal(test_results_text(t, app), want),
                   "results: RET took the default (%S)", test_results_text(t, app));
        app_dev_feed(app, "C-x o C-a C-SPC C-e C-x p g", &t->arena);
        TEST_CHECK(t, str8_equal(app_dev_prompt(app), STR8_LIT("Search project for (default int foo;): ")), "results: the region: '%S'",
                   app_dev_prompt(app));
        app_dev_feed(app, "C-g C-x b * s c r a t c h * RET C-x p g", &t->arena);
        TEST_CHECK(t, test_contains(test_echo(app), "Not in a project"), "results: *scratch* is not in a project: '%S'", test_echo(app));
        if (!test_app_destroy(t, app, "results")) return 0;
    }

    // Unsaved buffers: a modified file in its place (its disk text skipped), a modified ignored file and a
    // new file after the list, an empty new buffer and *scratch* not at all.
    {
        App *app = test_manual_app(t, a_c, 0, 0, none);
        app_dev_visit(app, str8_fmt(&t->arena, "%S\\b\\c.txt", root));
        app_dev_feed(app, "C-k c h a n g e d", &t->arena);
        app_dev_visit(app, str8_fmt(&t->arena, "%S\\ignored.c", root));
        app_dev_feed(app, "x", &t->arena);
        app_dev_visit(app, str8_fmt(&t->arena, "%S\\new.c", root));
        app_dev_feed(app, "f o o SPC n e w", &t->arena);
        app_dev_visit(app, str8_fmt(&t->arena, "%S\\empty_new.c", root));
        app_dev_feed(app, "C-x b * s c r a t c h * RET f o o C-x b a . c RET C-x p g f o o RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "results: the search did not end");
        String8 want2 = str8_fmt(&t->arena,
                                 "Search for \"foo\" in %S (ignoring case)\n\n"
                                 "a.c:1:5: int foo;\n"
                                 "a.c:2:1: foo(); foo\n"
                                 "a.c:2:8: foo(); foo\n"
                                 "big.txt:20001:5: the foo at the end\n"
                                 "u16.txt:1:1: foo \xc3\xa9\n"
                                 "b/d.h:2:1: FOO bar\n"
                                 "ignored.c:1:2: xfoo ignored\n"
                                 "new.c:1:1: foo new\n"
                                 "8 matches in 6 files; 8 files searched, 1 binary skipped, 3 from unsaved buffers",
                                 shown_root);
        String8 got = test_results_text(t, app);
        TEST_CHECK(t, str8_equal(got, want2), "results: unsaved buffers: *search* holds\n%S\n---- expected\n%S", got, want2);
        // The cap keeps the first matches in list order.
        app_dev_use_config(app, test_results_conf(t, "p12cap.conf", "[settings]\nsearch_max_results = 2\n"));
        app_dev_feed(app, "g", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "results: the capped search did not end");
        got = test_results_text(t, app);
        TEST_CHECK(t, test_contains(got, "a.c:1:5: int foo;\na.c:2:1: foo(); foo\nStopped at 2 matches (search_max_results) in 1 file;") &&
                          !test_contains(got, "a.c:2:8"), "results: search_max_results 2: '%S'", got);
        // A rebinding in [keys search-results].
        app_dev_use_config(app, test_results_conf(t, "p12keys.conf", "[keys search-results]\nz  next-error-no-select\n"));
        app_dev_feed(app, "g", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "results: g did not end");
        app_dev_feed(app, "z", &t->arena);
        TEST_CHECK(t, buffer_line_of(test_current(app), test_app_point(app)) == 3, "results: z (rebound) moves to the next match");
        if (!test_app_destroy(t, app, "results")) return 0;
    }

    // The claim window (2 files past the first not written) and a held-records limit of 1 byte: the same text.
    i32 max_ahead = 0, held_back = 0;
    {
        App *app = test_manual_app(t, a_c, 0, 0, test_results_conf(t, "p12res.conf", "[settings]\nsearch_workers = 4\n"));
        app->dev_claim_ahead = 2;
        app_dev_feed(app, "C-x p g f o o RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000) && str8_equal(test_results_text(t, app), want), "results: claim window 2: '%S'",
                   test_results_text(t, app));
        max_ahead = results_of(app, test_results_buffer(app))->dev_max_ahead;
        TEST_CHECK(t, max_ahead <= 2, "results: %d files claimed past the first one not written (window 2)", max_ahead);
        app->dev_claim_ahead = 0;
        app->dev_held_max = 1;
        app_dev_feed(app, "g", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000) && str8_equal(test_results_text(t, app), want), "results: held limit 1: '%S'",
                   test_results_text(t, app));
        held_back = results_of(app, test_results_buffer(app))->dev_held_back;
        if (!test_app_destroy(t, app, "results")) return 0;
    }

    // Killing *search* while its search runs; then a shutdown with a search running, and with a stuck worker.
    u64 took = 0;
    {
        App *app = test_manual_app(t, a_c, 0, 0, test_results_conf(t, "p12one.conf", "[settings]\nsearch_workers = 1\n"));
        app->dev_grep_hold_after = 1; // one worker, standing still after one file
        app_dev_feed(app, "C-x p g f o o RET", &t->arena);
        TEST_CHECK(t, test_results_buffer(app) && results_of(app, test_results_buffer(app))->running, "results: a held search runs");
        // The progress line while it stands still: "Searching... N/M files, K matches" (what the held workers
        // finished first is shown).
        for (i32 i = 0; i < 50; i++) {
            if (os_dev_wake_wait(10)) app_wake(app);
            app_dev_feed_events(app, NULL, 0, &t->arena);
        }
        {
            Buffer *rb = test_results_buffer(app);
            i64 last = buffer_line_count(rb) - 1;
            String8 progress = buffer_text(rb, &t->arena, buffer_line_start(rb, last), buffer_size(rb));
            i64 seen = 0, total = 0, at = 13;
            b32 ok = progress.len > 13 && mem_equal(progress.data, "Searching... ", 13);
            while (ok && at < progress.len && progress.data[at] >= '0' && progress.data[at] <= '9') seen = seen * 10 + progress.data[at++] - '0';
            ok = ok && at < progress.len && progress.data[at++] == '/';
            while (ok && at < progress.len && progress.data[at] >= '0' && progress.data[at] <= '9') total = total * 10 + progress.data[at++] - '0';
            ok = ok && test_contains(progress, " files, ") && seen >= 1 && seen <= total && total <= 20;
            TEST_CHECK(t, ok, "results: the progress line of a held search: '%S'", progress);
        }
        app_dev_feed(app, "C-x k RET", &t->arena);
        TEST_CHECK(t, !test_results_buffer(app) && app->grep_dying_count == 1, "results: killing *search* stops its search");
        results_dev_release(app);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 5000), "results: the killed search's workers did not end");
        app->dev_grep_hold_after = 1;
        app_dev_feed(app, "C-x p g f o o RET", &t->arena);
        u64 t0 = os_time_us();
        if (!test_app_destroy(t, app, "results: shutdown with a search running")) return 0;
        took = os_time_us() - t0;
        TEST_CHECK(t, took < (u64)PROJECT_SHUTDOWN_MS * 1000 && app->dev_grep_abandoned == 0, "results: shutdown with a held search: %U us", took);
    }
    {
        App *app = test_manual_app(t, a_c, 0, 0, none);
        app->dev_grep_stuck_next = 1;
        app_dev_feed(app, "C-x p g f o o RET", &t->arena);
        GrepSearch *stuck = app->dev_last_grep;
        u64 t0 = os_time_us();
        app_shutdown(app);
        u64 stuck_us = os_time_us() - t0;
        TEST_CHECK(t, stuck && app->dev_grep_abandoned >= 1 && stuck_us >= (u64)(PROJECT_SHUTDOWN_MS - 100) * 1000 &&
                          stuck_us < (u64)(PROJECT_SHUTDOWN_MS + 500) * 1000, "results: a stuck worker: shutdown in %U us, %d abandoned", stuck_us,
                   app->dev_grep_abandoned);
        for (i32 i = 0; i < app->dev_grep_abandoned; i++) os_dev_thread_reclaimed();
        grep_dev_release(stuck);
        TEST_CHECK(t, grep_join(stuck, os_time_us() + 10000000), "results: the stuck workers did not end once released");
        grep_free(stuck);
    }

    // A search started while the project's first scan stands still sees every file once (10,000 files).
    {
        String8 big = os_full_path(&t->arena, test_big_tree(t));
        App *app = test_manual_app(t, none, 0, 0, test_results_conf(t, "p12big.conf", "[settings]\nsearch_max_results = 20000\n"));
        memcpy(app->finder.cmdline_buf, big.data, (size_t)big.len);
        app->finder.cmdline_dir = str8(app->finder.cmdline_buf, big.len);
        app->finder.dev_hold_after = 1;
        app_dev_feed(app, "C-x p g x RET", &t->arena);
        Results *r = results_of(app, test_results_buffer(app));
        TEST_CHECK(t, r && r->running, "results: no search over the held scan");
        for (i32 i = 0; i < 20; i++) {
            if (os_dev_wake_wait(5)) app_wake(app);
            app_dev_feed_events(app, NULL, 0, &t->arena);
        }
        i64 during = r->list_count;
        finder_dev_release(app);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 20000), "results: the search over the scan did not end");
        TEST_CHECK(t, r->count == 10000 && r->list_count == 10000 && during < 10000, "results: %D matches of 10000 files (%D listed while held)",
                   r->count, during);
        b32 once = 1;
        for (i64 i = 1; i < r->count; i++) once &= r->locs[i].file != r->locs[i - 1].file;
        TEST_CHECK(t, once && r->file_count == 10000, "results: every file once (%d files)", r->file_count);
        if (!test_app_destroy(t, app, "results")) return 0;
    }
    TEST_CHECK(t, os_dev_live_threads() == threads0 && os_dev_live_events() == events0, "results: %d threads, %d events left",
               os_dev_live_threads() - threads0, os_dev_live_events() - events0);
    LOG("test: ok: results (*search*'s exact text with 1, 2, 4 and 8 workers, 5 runs of g, a large file first; the next-error order "
        "the same each time, past the end; RET, n, p, q, RET on the header, previous-error at both ends, none at all; the keymap only in "
        "*search*, describe-key; the default from the symbol and the region, not in a project; a modified buffer in its place, a "
        "modified ignored file and a new one after the list, *scratch* and an empty buffer not searched; search_max_results; a "
        "rebinding; the claim window 2 (at most %d past), a held limit of 1 byte (held back %d times): the same text; killing *search* "
        "while it runs; shutdown with a search running (%U us) and with a stuck worker; a search over a held scan sees each of "
        "10,000 files once)", max_ahead, held_back, took);
    return 1;
}

// occur: *occur* for one buffer (unsaved text too), searched on the main thread in slices; its keys and
// next-error; g; a large buffer over many frames; the searched buffer killed meanwhile.
// The results headers' text (Phase 16): 1,000 occur runs and 200 project searches must not grow files_arena past what
// the first ones needed (each run used to leave its header there).
static b32 test_results_files_arena(Test *t) {
    App *app = test_search_app(t, STR8_LIT("foo\nbar Foo\nnone\n"), 0);
    TEST_CHECK(t, app, "results files_arena: app_create failed");
    u64 committed_100 = 0;
    for (i32 i = 1; i <= 1000; i++) {
        app_dev_feed(app, "M-s o RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 5000), "results files_arena: occur %d did not end", i);
        app_dev_feed(app, "q", &t->arena);
        if (i == 100) committed_100 = app->files_arena.committed;
    }
    u64 occur_committed = app->files_arena.committed;
    TEST_CHECK(t, occur_committed == committed_100, "results files_arena: occur grew it from %U to %U bytes between run 100 and 1,000",
               committed_100, occur_committed);
    if (!test_app_destroy(t, app, "results files_arena")) return 0;
    String8 root = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p16grep", t->tmp_dir));
    os_make_dir(root);
    os_write_file(str8_fmt(&t->arena, "%S\\.ronin-project", root), STR8_LIT("# results files_arena\n"));
    String8 a_c = str8_fmt(&t->arena, "%S\\a.c", root);
    os_write_file(a_c, STR8_LIT("int foo;\n"));
    app = test_manual_app(t, a_c, 0, 0, test_results_conf(t, "p16grep.conf", "[settings]\nsearch_workers = 2\n"));
    TEST_CHECK(t, app, "results files_arena: the project app failed");
    u64 committed_20 = 0;
    for (i32 i = 1; i <= 200; i++) {
        app_dev_feed(app, "C-x p g C-a C-k f o o RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "results files_arena: search %d did not end", i);
        app_dev_feed(app, "q", &t->arena);
        if (i == 20) committed_20 = app->files_arena.committed;
    }
    u64 search_committed = app->files_arena.committed;
    TEST_CHECK(t, search_committed == committed_20, "results files_arena: project-search grew it from %U to %U bytes between search 20 and 200",
               committed_20, search_committed);
    if (!test_app_destroy(t, app, "results files_arena")) return 0;
    LOG("test: ok: results files_arena (1,000 occur runs: %U bytes after 100 and after 1,000; 200 project searches: %U after 20 and 200)",
        occur_committed, search_committed);
    return 1;
}

// The held records and the pending lines (Phase 17): committed to RESULTS_TRANSIENT_FLOOR at a buffer's first run, so
// a run that stays under it leaves the same memory however the workers' timing went (their high-water made the leak
// check's verdict depend on it); a minimize (app_trim) gives back what a larger run grew above it. Every match passes
// through both (results_record, results_add), so a run with matches has used them.
static String8 test_child(Test *t, const char *name, String8 script);

static b32 test_results_at_floor(Results *r) {
    return r && r->held[0].committed == RESULTS_TRANSIENT_FLOOR && r->held[1].committed == RESULTS_TRANSIENT_FLOOR &&
           r->pend.committed == RESULTS_TRANSIENT_FLOOR;
}

static b32 test_results_transient(Test *t) {
    String8 root = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p17transient", t->tmp_dir));
    os_make_dir(root);
    String8 a_c = str8_fmt(&t->arena, "%S\\a.c", root);
    for (i32 i = 0; i < 20; i++) os_write_file(str8_fmt(&t->arena, "%S\\f%d.c", root, i), STR8_LIT("int foo;\nint bar;\nfoo = 1;\n"));
    os_write_file(a_c, STR8_LIT("int foo;\n"));
    String8 cmd = test_child(t, "transient_build", STR8_LIT("out a.c(1,5): error C2065: 'foo': undeclared identifier\nexit 1\n"));
    os_write_file(str8_fmt(&t->arena, "%S\\.ronin-project", root), str8_fmt(&t->arena, "[build]\ncommand = %S\n", cmd));
    // Nothing at startup; a project search that ends on its own, five times: the floors exactly, every time.
    App *app = test_manual_app(t, a_c, 0, 0, test_results_conf(t, "p17transient.conf", "[settings]\nsearch_workers = 2\ncompilation_save_buffers = none\n"));
    TEST_CHECK(t, app, "results transient: app_create failed");
    b32 none_yet = 1;
    for (i32 k = 0; k < RESULTS_MAX; k++) none_yet &= !app->results[k].buffer && !app->results[k].held[0].committed;
    TEST_CHECK(t, none_yet, "results transient: a results slot made at startup");
    Results *r = NULL;
    i64 found = -1;
    for (i32 run = 0; run < 5; run++) {
        app_dev_feed(app, run ? "g" : "C-x p g C-a C-k f o o RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "results transient: search %d did not end", run);
        r = results_of(app, test_results_buffer(app));
        found = r ? r->found : -1;
        TEST_CHECK(t, found == 41 && test_results_at_floor(r), "results transient: after search %d (%D matches): held %U / %U, pend %U bytes committed",
                   run, found, r ? r->held[0].committed : 0, r ? r->held[1].committed : 0, r ? r->pend.committed : 0);
    }
    // Grown past the floor (as a large search grows them), then a minimize: what is above goes back.
    arena_push(&r->held[0], MB(3), 8);
    arena_push(&r->held[1], MB(2), 8);
    arena_push(&r->pend, MB(1), 8);
    app_trim(app);
    TEST_CHECK(t, test_results_at_floor(r) && !r->held[0].pos && !r->held[1].pos && !r->pend.pos,
               "results transient: grown past the floor, then minimized: held %U / %U, pend %U bytes committed", r->held[0].committed,
               r->held[1].committed, r->pend.committed);
    app_dev_feed(app, "C-x b a . c RET C-x p c RET", &t->arena); // a build from a.c (its project's command)
    TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "results transient: the build did not end");
    Results *c = results_of(app, buffer_list_find_name(&app->buffers, STR8_LIT("*compilation*")));
    TEST_CHECK(t, c && c->count == 1 && test_results_at_floor(c), "results transient: after a build (%D locations, echo '%S'): held %U / %U, pend %U bytes committed",
               c ? c->count : -1, test_echo(app), c ? c->held[0].committed : 0, c ? c->held[1].committed : 0, c ? c->pend.committed : 0);
    if (!test_app_destroy(t, app, "results transient")) return 0;
    // A running search is not trimmed; stopped (its only worker standing still after five files), it is.
    app = test_manual_app(t, a_c, 0, 0, test_results_conf(t, "p17transient1.conf", "[settings]\nsearch_workers = 1\n"));
    TEST_CHECK(t, app, "results transient: app_create failed (one worker)");
    app->dev_grep_hold_after = 5;
    app_dev_feed(app, "C-x p g C-a C-k f o o RET", &t->arena);
    r = results_of(app, test_results_buffer(app));
    for (i32 i = 0; i < 200 && r && r->running && !r->found; i++) {
        if (os_dev_wake_wait(10)) app_wake(app);
        app_dev_feed_events(app, NULL, 0, &t->arena);
    }
    TEST_CHECK(t, r && r->running && r->found, "results transient: the held search wrote nothing");
    arena_push(&r->held[0], MB(1), 8); // as if its records had grown past the floor
    u64 running_committed = r->held[0].committed, running_pos = r->held[0].pos;
    app_trim(app);
    TEST_CHECK(t, r->held[0].committed == running_committed && r->held[0].pos == running_pos, "results transient: a running search was trimmed");
    results_stop(app, r);
    app_trim(app);
    TEST_CHECK(t, test_results_at_floor(r), "results transient: stopped, then minimized: held %U / %U, pend %U bytes committed",
               r->held[0].committed, r->held[1].committed, r->pend.committed);
    results_dev_release(app);
    TEST_CHECK(t, results_dev_wait(app, &t->arena, 5000), "results transient: the stopped search's workers did not end");
    if (!test_app_destroy(t, app, "results transient")) return 0;
    // occur (the main thread's search: pend only).
    app = test_search_app(t, STR8_LIT("foo\nbar Foo\nnone\n"), 0);
    TEST_CHECK(t, app, "results transient: the occur app failed");
    app_dev_feed(app, "M-s o RET", &t->arena);
    TEST_CHECK(t, results_dev_wait(app, &t->arena, 5000), "results transient: occur did not end");
    Results *o = results_of(app, buffer_list_find_name(&app->buffers, STR8_LIT("*occur*")));
    TEST_CHECK(t, o && o->found && test_results_at_floor(o), "results transient: after occur: pend %U bytes committed", o ? o->pend.committed : 0);
    if (!test_app_destroy(t, app, "results transient")) return 0;
    LOG("test: ok: results transient (no slot at startup; held[0], held[1] and pend at exactly %U bytes committed after 5 project "
        "searches of %D matches, a build and occur; grown past it by 3 / 2 / 1 MB, a minimize gives that back; a running search is "
        "not trimmed, a stopped one is)", (u64)RESULTS_TRANSIENT_FLOOR, found);
    return 1;
}

static b32 test_occur(Test *t) {
    App *app = test_search_app(t, STR8_LIT("foo\nbar Foo\n\tfoo foo\nnone\n"), 0);
    TEST_CHECK(t, app, "occur: app_create failed");
    app_dev_feed(app, "M-s o", &t->arena);
    TEST_CHECK(t, str8_equal(app_dev_prompt(app), STR8_LIT("List lines matching (default foo): ")), "occur: the prompt '%S'", app_dev_prompt(app));
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, results_dev_wait(app, &t->arena, 5000), "occur: did not end");
    Buffer *ob = buffer_list_find_name(&app->buffers, STR8_LIT("*occur*"));
    TEST_CHECK(t, ob && test_current(app) == ob, "occur: *occur* is shown and selected");
    String8 got = buffer_text(ob, &t->arena, 0, buffer_size(ob));
    String8 want = STR8_LIT("Lines matching \"foo\" in buffer *scratch* (ignoring case)\n\n1:1: foo\n2:5: bar Foo\n3:2: \tfoo foo\n3:6: \tfoo foo\n4 matches, ");
    TEST_CHECK(t, got.len > want.len && mem_equal(got.data, want.data, want.len), "occur: *occur* holds '%S'", got);
    app_dev_feed(app, "n n", &t->arena);
    TEST_CHECK(t, buffer_line_of(ob, test_app_point(app)) == 4, "occur: n twice: line %D", buffer_line_of(ob, test_app_point(app)));
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, str8_equal(test_results_where(t, app), STR8_LIT("*scratch*:13")), "occur: RET visits *scratch* at 13 ('%S')",
               test_results_where(t, app));
    app_dev_feed(app, "M-g n", &t->arena);
    TEST_CHECK(t, str8_equal(test_results_where(t, app), STR8_LIT("*scratch*:17")), "occur: next-error to 17 ('%S')", test_results_where(t, app));
    app_dev_feed(app, "M-g n", &t->arena);
    TEST_CHECK(t, test_app_echo_is(app, "No more matches"), "occur: past the end: '%S'", test_echo(app));
    // The buffer edited, then g in *occur*: the new text.
    app_dev_feed(app, "M-< f o o SPC C-x o g", &t->arena);
    TEST_CHECK(t, results_dev_wait(app, &t->arena, 5000), "occur: g did not end");
    got = buffer_text(ob, &t->arena, 0, buffer_size(ob));
    TEST_CHECK(t, test_contains(got, "\n\n1:1: foo foo\n1:5: foo foo\n2:5: bar Foo\n") && test_contains(got, "5 matches, "), "occur: after g: '%S'", got);
    app_dev_feed(app, "M-s o", &t->arena);
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, test_contains(test_echo(app), "occur cannot search *occur*"), "occur: on *occur*: '%S'", test_echo(app));
    // A large buffer, a few positions a frame: many frames, the same answer as at once.
    app_dev_feed(app, "C-x o", &t->arena);
    u8 *big = PUSH_ARRAY(&t->arena, u8, 200 * 1000);
    i64 n = 0;
    for (i32 i = 0; i < 10000; i++) n += fmt_buf(big + n, 32, i % 100 == 0 ? "needle %05d\n" : "line %05d xxxxx\n", i);
    app_dev_show_scratch(app, str8(big, n));
    app->dev_work_budget = 4096;
    app_dev_feed(app, "M-s o n e e d l e RET", &t->arena);
    i32 frames = 0;
    Results *r = results_of(app, ob);
    while (r->running && frames < 10000) {
        app_dev_feed_events(app, NULL, 0, &t->arena);
        frames++;
    }
    app->dev_work_budget = 0;
    TEST_CHECK(t, r->count == 100 && frames > 20 && r->locs[99].line == 9901, "occur: a large buffer in %d frames: %D matches", frames, r->count);
    // The searched buffer killed while occur runs: it ends, nothing more is written.
    app_dev_visit(app, str8_fmt(&t->arena, "%S\\p12occur.txt", t->tmp_dir));
    app_dev_feed(app, "C-x h C-w", &t->arena);
    app_dev_append(app, str8(big, n));
    app->dev_work_budget = 1000;
    app_dev_feed(app, "M-s o n e e d l e RET", &t->arena);
    TEST_CHECK(t, r->running, "occur: a sliced occur runs");
    app_dev_feed(app, "C-x o C-x k RET y e s RET", &t->arena);
    app->dev_work_budget = 0;
    TEST_CHECK(t, results_dev_wait(app, &t->arena, 5000) && !r->running, "occur: the killed buffer's occur did not end");
    if (!test_app_destroy(t, app, "occur")) return 0;
    LOG("test: ok: occur (the exact *occur* text of an unsaved buffer, the default, n, RET, next-error past the end, g after an edit, "
        "not on *occur* itself, 10,000 lines at 4096 positions a frame in %d frames, the searched buffer killed meanwhile)", frames);
    return 1;
}

// The landing highlight: where RET, n, p, next-error and previous-error went, until the next key.
static b32 test_landing(Test *t) {
    App *app = test_search_app(t, STR8_LIT("a foo\nb\nfoo c\n"), 0);
    TEST_CHECK(t, app, "landing: app_create failed");
    Buffer *scratch = buffer_list_find_name(&app->buffers, STR8_LIT("*scratch*"));
    app_dev_feed(app, "M-s o f o o RET", &t->arena);
    TEST_CHECK(t, results_dev_wait(app, &t->arena, 5000), "landing: occur did not end");
    TEST_CHECK(t, !app->landing_buffer, "landing: set before any visit");
    app_dev_feed(app, "RET", &t->arena); // the first match (point is on its line), *scratch* selected
    TEST_CHECK(t, app->landing_buffer == scratch && app->landing_start == 2 && app->landing_end == 5 && test_current(app) == scratch,
               "landing: RET: %D-%D", app->landing_start, app->landing_end);
    app_dev_feed(app, "C-x", &t->arena); // a prefix key is a key too
    TEST_CHECK(t, !app->landing_buffer, "landing: still set after a prefix key");
    app_dev_feed(app, "o n", &t->arena); // back in *occur*; n shows the next match in the other window
    TEST_CHECK(t, app->landing_buffer == scratch && app->landing_start == 8 && app->landing_end == 11, "landing: n: %D-%D",
               app->landing_start, app->landing_end);
    app_dev_feed(app, "C-n", &t->arena); // any other key ends it
    TEST_CHECK(t, !app->landing_buffer, "landing: still set after another key");
    app_dev_feed(app, "p", &t->arena);
    TEST_CHECK(t, app->landing_buffer == scratch && app->landing_start == 8, "landing: p: %D", app->landing_start);
    app_dev_feed(app, "C-x o M-g p", &t->arena); // previous-error from *scratch*
    TEST_CHECK(t, app->landing_buffer == scratch && app->landing_start == 2 && app->landing_end == 5 && test_current(app) == scratch,
               "landing: previous-error: %D-%D", app->landing_start, app->landing_end);
    // A frame keeps it (no timer); killing its buffer ends it.
    app_dev_feed_events(app, NULL, 0, &t->arena);
    TEST_CHECK(t, app->landing_buffer == scratch, "landing: a frame without a key ended it");
    files_kill_buffer(app, scratch);
    TEST_CHECK(t, !app->landing_buffer, "landing: still set on a killed buffer");
    if (!test_app_destroy(t, app, "landing")) return 0;
    LOG("test: ok: the landing highlight (RET, a prefix key, n, another key, p, previous-error from the target window, a frame without "
        "a key, the buffer killed)");
    return 1;
}

// ---------------------------------------------------------------------------
// Phase 13: processes and builds

// tmp\p13\<name>.txt holding a --child script; the command that runs this executable on it.
static String8 test_child(Test *t, const char *name, String8 script) {
    String8 dir = str8_fmt(&t->arena, "%S\\p13", t->tmp_dir);
    os_make_dir(dir);
    String8 path = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\%s.txt", dir, name));
    os_write_file(path, script);
    return str8_fmt(&t->arena, "\"%S\" --child \"%S\"", os_dev_exe_path(&t->arena), path);
}

// A grandchild's id from "grandchild <pid>" in `out`, 0 when not there.
static u32 test_grandchild_pid(String8 out) {
    String8 key = STR8_LIT("grandchild ");
    for (i64 i = 0; i + key.len <= out.len; i++) {
        if (!mem_equal(out.data + i, key.data, key.len)) continue;
        u32 pid = 0;
        for (i64 k = i + key.len; k < out.len && out.data[k] >= '0' && out.data[k] <= '9'; k++) pid = pid * 10 + (out.data[k] - '0');
        return pid;
    }
    return 0;
}

typedef struct TestProcRun {
    OsFileStatus status;
    String8 out;   // everything read (at most 4 MB kept)
    u32 code;
    b32 exited;    // os_process_read reported the end
    u32 grandchild;
} TestProcRun;

// Runs a command through os_process_* on this thread until it ends; with `kill_after` set, kills the job
// once that text was read (and the grandchild's id, if any, is known).
static TestProcRun test_proc_run(Test *t, String8 command, String8 dir, const char *kill_after) {
    TestProcRun run = { 0 };
    OsProcess p;
    run.status = os_process_start(&t->arena, command, dir, &p);
    if (run.status != OS_FILE_OK) return run;
    OsEvent cancel;
    os_event_create(&cancel);
    i64 cap = MB(4);
    u8 *out = PUSH_ARRAY(&t->arena, u8, cap);
    i64 n = 0;
    b32 killed = 0;
    for (;;) {
        u8 chunk[4096];
        i64 got = 0;
        OsProcessRead r = os_process_read(&p, cancel, 0, chunk, sizeof(chunk), &got);
        if (r == OS_PROCESS_DATA) {
            i64 take = MIN(got, cap - n);
            memcpy(out + n, chunk, (size_t)take);
            n += take;
            if (kill_after && !killed) {
                String8 key = str8_cstr(kill_after);
                for (i64 i = 0; i + key.len <= n; i++) {
                    if (mem_equal(out + i, key.data, key.len)) {
                        killed = 1;
                        break;
                    }
                }
                if (killed) {
                    run.grandchild = test_grandchild_pid(str8(out, n));
                    os_process_kill(&p);
                }
            }
            continue;
        }
        run.exited = r == OS_PROCESS_EXITED;
        break;
    }
    os_process_wait(&p, 5000);
    run.code = os_process_exit_code(&p);
    if (!run.grandchild) run.grandchild = test_grandchild_pid(str8(out, n));
    os_process_close(&p, killed);
    os_event_destroy(cancel);
    run.out = str8(out, n);
    return run;
}

// Waits up to `ms` for a process id to be gone.
static b32 test_pid_gone(u32 pid, u32 ms) {
    OsEvent pause = { 0 };
    os_event_create(&pause);
    b32 gone = 0;
    for (u32 waited = 0; waited <= ms && !gone; waited += 10) {
        gone = !os_dev_process_alive(pid);
        if (!gone) os_event_wait(pause, 10);
    }
    os_event_destroy(pause);
    return gone;
}

static b32 test_process_platform(Test *t) {
    i32 procs0 = os_dev_live_processes(), events0 = os_dev_live_events();
    String8 dir = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p13", t->tmp_dir));
    os_make_dir(dir);
    // Output order across stdout and stderr (one pipe), CRLF kept, the exit code.
    String8 cmd = test_child(t, "order", STR8_LIT("out one\nerr two\nout three\nexit 3\n"));
    TestProcRun r = test_proc_run(t, cmd, dir, NULL);
    TEST_CHECK(t, r.status == OS_FILE_OK && r.exited, "process: the child did not run (status %d)", (i32)r.status);
    TEST_CHECK(t, str8_equal(r.out, STR8_LIT("one\r\ntwo\r\nthree\r\n")) && r.code == 3, "process: output '%S', exit code %u", r.out, r.code);
    // The working directory.
    r = test_proc_run(t, test_child(t, "cwd", STR8_LIT("cwd\n")), dir, NULL);
    TEST_CHECK(t, str8_equal(r.out, str8_fmt(&t->arena, "%S\r\n", dir)) && r.code == 0, "process: cwd '%S', not '%S'", r.out, dir);
    // A directory that does not exist: nothing starts, nothing is left open.
    i32 handles0 = os_dev_handle_count();
    OsProcess none;
    OsFileStatus st = os_process_start(&t->arena, cmd, str8_fmt(&t->arena, "%S\\no_such_dir", dir), &none);
    TEST_CHECK(t, st == OS_FILE_NOT_FOUND && os_dev_live_processes() == procs0 && os_dev_handle_count() <= handles0,
               "process: a missing directory gave status %d, %d processes, handles %d -> %d", (i32)st, os_dev_live_processes(), handles0,
               os_dev_handle_count());
    // A kill ends the child and its grandchild.
    test_child(t, "hang", STR8_LIT("hang\n"));
    String8 hang = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\hang.txt", dir));
    String8 tree = test_child(t, "tree", str8_fmt(&t->arena, "spawn %S\nout spawned\nhang\n", hang));
    r = test_proc_run(t, tree, dir, "spawned");
    TEST_CHECK(t, r.grandchild, "process: no grandchild id in '%S'", r.out);
    b32 gone = test_pid_gone(r.grandchild, 1000);
    if (!gone) os_dev_kill_pid(r.grandchild);
    TEST_CHECK(t, gone, "process: the grandchild %u survived the kill", r.grandchild);
    // A normal end while a grandchild still holds the pipe: the end is cmd.exe's, the grandchild is let go.
    r = test_proc_run(t, test_child(t, "linger", str8_fmt(&t->arena, "spawn %S\nout done\nexit 0\n", hang)), dir, NULL);
    b32 lingering = r.grandchild && os_dev_process_alive(r.grandchild);
    if (r.grandchild) os_dev_kill_pid(r.grandchild);
    TEST_CHECK(t, r.exited && r.code == 0 && lingering, "process: a lingering grandchild: ended %d, code %u, grandchild %u alive %d", r.exited,
               r.code, r.grandchild, lingering);
    // No handle growth over rounds.
    i32 after_first = 0;
    for (i32 round = 0; round < 20; round++) {
        r = test_proc_run(t, cmd, dir, NULL);
        TEST_CHECK(t, r.code == 3, "process: round %d exit code %u", round, r.code);
        if (!round) after_first = os_dev_handle_count();
    }
    TEST_CHECK(t, os_dev_handle_count() <= after_first, "process: handles grew from %d to %d over 20 runs", after_first, os_dev_handle_count());
    TEST_CHECK(t, os_dev_live_processes() == procs0 && os_dev_live_events() == events0, "process: counters back (%d processes, %d events)",
               os_dev_live_processes(), os_dev_live_events());
    LOG("test: ok: processes (stdout and stderr in order through one pipe, CRLF, the exit code, the working directory, a missing "
        "directory leaving nothing open, a kill ending the child and its grandchild, a normal end with a grandchild holding the pipe "
        "(let go), no handle growth over 20 runs)");
    return 1;
}

// Phase 19: a program started directly (no cmd.exe) read to its end, or until `idle_ms` passes without output
// (*timed_out, then killed).
static TestProcRun test_program_run(Test *t, String8 program, String8 args, String8 dir, String8 env, u32 idle_ms, b32 *timed_out) {
    TestProcRun run = { 0 };
    OsProcess p;
    *timed_out = 0;
    run.status = os_process_start_program(&t->arena, program, args, dir, env, &p);
    if (run.status != OS_FILE_OK) return run;
    OsEvent cancel;
    os_event_create(&cancel);
    i64 cap = MB(1), n = 0;
    u8 *out = PUSH_ARRAY(&t->arena, u8, cap);
    for (;;) {
        i64 got = 0;
        OsProcessRead r = os_process_read(&p, cancel, idle_ms, out + n, cap - n, &got);
        if (r == OS_PROCESS_DATA) {
            n += got;
            continue;
        }
        *timed_out = r == OS_PROCESS_TIMEOUT;
        run.exited = r == OS_PROCESS_EXITED;
        break;
    }
    if (*timed_out) os_process_kill(&p);
    os_process_wait(&p, 5000);
    run.code = os_process_exit_code(&p);
    os_process_close(&p, 1);
    os_event_destroy(cancel);
    run.out = str8(out, n);
    return run;
}

static b32 test_process_program(Test *t) {
    i32 procs0 = os_dev_live_processes(), events0 = os_dev_live_events(), ledger0 = os_dev_live_handles();
    String8 dir = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p13", t->tmp_dir));
    String8 exe = os_dev_exe_path(&t->arena);
    b32 timed_out = 0;
    // Started directly: the arguments as given (a quoted path with spaces), the working directory, the exit code.
    test_child(t, "prog one", STR8_LIT("out one\nerr two\nexit 4\n"));
    String8 script = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\prog one.txt", dir));
    TestProcRun r = test_program_run(t, exe, str8_fmt(&t->arena, "--child \"%S\"", script), dir, str8(NULL, 0), 0, &timed_out);
    TEST_CHECK(t, r.status == OS_FILE_OK && r.exited && r.code == 4 && str8_equal(r.out, STR8_LIT("one\r\ntwo\r\n")),
               "program: status %d, exit %u, output '%S'", (i32)r.status, r.code, r.out);
    // The environment: ronin's inherited, a same-named variable replaced (any case, once), a new one added.
    os_dev_set_env(STR8_LIT("RONIN_TEST_ENV"), STR8_LIT("parent"), 0);
    os_dev_set_env(STR8_LIT("RONIN_TEST_KEEP"), STR8_LIT("kept"), 0);
    test_child(t, "env", STR8_LIT("env RONIN_TEST_ENV\nenv RONIN_TEST_KEEP\nenv RONIN_TEST_NEW\nenv RONIN_TEST_GONE\n"));
    String8 env_script = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\env.txt", dir));
    String8 env_args = str8_fmt(&t->arena, "--child \"%S\"", env_script);
    r = test_program_run(t, exe, env_args, dir, STR8_LIT("ronin_test_env=child\nRONIN_TEST_NEW=a b=c"), 0, &timed_out);
    TEST_CHECK(t, str8_equal(r.out, STR8_LIT("child\r\nkept\r\na b=c\r\n(unset)\r\n")), "program: the environment block gave '%S'", r.out);
    r = test_program_run(t, exe, env_args, dir, str8(NULL, 0), 0, &timed_out);
    TEST_CHECK(t, str8_equal(r.out, STR8_LIT("parent\r\nkept\r\n(unset)\r\n(unset)\r\n")), "program: ronin's environment gave '%S'", r.out);
    os_dev_set_env(STR8_LIT("RONIN_TEST_ENV"), str8(NULL, 0), 1);
    os_dev_set_env(STR8_LIT("RONIN_TEST_KEEP"), str8(NULL, 0), 1);
    // The idle timeout: a child silent for 3 s is reported after 300 ms (and killed); output resets it.
    test_child(t, "silent", STR8_LIT("out a\nsleep 3000\nout b\n"));
    String8 silent = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\silent.txt", dir));
    u64 t0 = os_time_us();
    r = test_program_run(t, exe, str8_fmt(&t->arena, "--child \"%S\"", silent), dir, str8(NULL, 0), 300, &timed_out);
    u64 waited_ms = (os_time_us() - t0) / 1000;
    TEST_CHECK(t, timed_out && str8_equal(r.out, STR8_LIT("a\r\n")) && waited_ms < 2500, "program: timeout %d after %U ms, output '%S'",
               timed_out, waited_ms, r.out);
    test_child(t, "talker", STR8_LIT("out a\nsleep 200\nout b\nsleep 200\nout c\n"));
    String8 talker = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\talker.txt", dir));
    r = test_program_run(t, exe, str8_fmt(&t->arena, "--child \"%S\"", talker), dir, str8(NULL, 0), 1000, &timed_out);
    TEST_CHECK(t, !timed_out && r.exited && str8_equal(r.out, STR8_LIT("a\r\nb\r\nc\r\n")), "program: a talking child timed out (%d) '%S'",
               timed_out, r.out);
    // A program that does not exist, and a directory that does not: nothing starts, nothing is left open.
    r = test_program_run(t, str8_fmt(&t->arena, "%S\\no_such_program.exe", dir), str8(NULL, 0), dir, str8(NULL, 0), 0, &timed_out);
    TEST_CHECK(t, r.status == OS_FILE_NOT_FOUND, "program: a missing program gave status %d", (i32)r.status);
    r = test_program_run(t, exe, env_args, str8_fmt(&t->arena, "%S\\no_such_dir", dir), str8(NULL, 0), 0, &timed_out);
    TEST_CHECK(t, r.status == OS_FILE_NOT_FOUND, "program: a missing directory gave status %d", (i32)r.status);
    // PATH: the first entry holding the file, quoted entries, never the current directory, nothing when absent.
    String8 old_path = os_get_env(&t->arena, STR8_LIT("PATH"));
    String8 bin1 = str8_fmt(&t->arena, "%S\\p19path\\one", t->tmp_dir), bin2 = str8_fmt(&t->arena, "%S\\p19path\\two dir", t->tmp_dir);
    os_make_dir(str8_fmt(&t->arena, "%S\\p19path", t->tmp_dir));
    os_make_dir(bin1);
    os_make_dir(bin2);
    os_make_dir(str8_fmt(&t->arena, "%S\\tool.exe", bin1)); // a directory of that name does not count
    os_write_file(str8_fmt(&t->arena, "%S\\tool.exe", bin2), STR8_LIT("x"));
    String8 found = { 0 };
    os_dev_set_env(STR8_LIT("PATH"), str8_fmt(&t->arena, ";%S;\"%S\";C:\\no_such_dir", bin1, bin2), 0);
    b32 ok = os_find_program(&t->arena, STR8_LIT("tool.exe"), &found);
    TEST_CHECK(t, ok && str8_equal(found, str8_fmt(&t->arena, "%S\\tool.exe", bin2)), "program: PATH lookup gave %d '%S'", ok, found);
    ok = os_find_program(&t->arena, STR8_LIT("no_such_tool.exe"), &found);
    TEST_CHECK(t, !ok && !found.len, "program: a missing program found as '%S'", found);
    os_dev_set_env(STR8_LIT("PATH"), str8(NULL, 0), 1);
    ok = os_find_program(&t->arena, STR8_LIT("tool.exe"), &found);
    TEST_CHECK(t, !ok, "program: found '%S' without a PATH", found);
    os_dev_set_env(STR8_LIT("PATH"), old_path, 0);
    ok = os_find_program(&t->arena, STR8_LIT("cmd.exe"), &found);
    TEST_CHECK(t, ok && found.len > 8, "program: cmd.exe not found on PATH");
    TEST_CHECK(t, os_dev_live_processes() == procs0 && os_dev_live_events() == events0 && os_dev_live_handles() == ledger0,
               "program: counters back (%d processes, %d events, ledger %d -> %d)", os_dev_live_processes(), os_dev_live_events(), ledger0,
               os_dev_live_handles());
    LOG("test: ok: programs (started without cmd.exe, the environment block, the idle timeout, a missing program or directory, PATH)");
    return 1;
}

// A match as text: "kind line:col [path] (num) {kind word} <project>", "-" for none.
static String8 test_match_text(Arena *arena, String8 line, b32 ok, const CompileMatch *m) {
    if (!ok) return STR8_LIT("-");
    static const char *kinds[] = { "none", "error", "warning", "info" };
    String8 s = str8_fmt(arena, "%s", kinds[m->kind]);
    if (m->located) {
        s = str8_fmt(arena, "%S %D:%D [%S] (%S)", s, m->line, m->col, str8(line.data + m->path_off, m->path_len),
                     str8(line.data + m->num_off, m->num_len));
    }
    s = str8_fmt(arena, "%S {%S}", s, str8(line.data + m->kind_off, m->kind_len));
    if (m->proj_len) s = str8_fmt(arena, "%S <%S>", s, str8(line.data + m->proj_off, m->proj_len));
    return s;
}

static b32 test_compile_match(Test *t) {
    static const struct { const char *line, *want; } located[] = {
        // MSVC (build.bat compiles with /FC: full paths), csc, tsc's plain form, MSBuild
        { "C:\\web-projects\\text-editor\\src\\edit.c(120): error C2065: 'x': undeclared identifier",
          "error 120:0 [C:\\web-projects\\text-editor\\src\\edit.c] ((120)) {error}" },
        { "C:\\web-projects\\text-editor\\src\\app.c(55,9): warning C4100: 'y': unreferenced formal parameter",
          "warning 55:9 [C:\\web-projects\\text-editor\\src\\app.c] ((55,9)) {warning}" },
        { "C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.22621.0\\ucrt\\stdio.h(12): note: see declaration of 'printf'",
          "info 12:0 [C:\\Program Files (x86)\\Windows Kits\\10\\Include\\10.0.22621.0\\ucrt\\stdio.h] ((12)) {note}" },
        { "src\\ronin.c(3): fatal error C1083: Cannot open include file: 'nope.h': No such file or directory",
          "error 3:0 [src\\ronin.c] ((3)) {fatal error}" },
        { "C:\\dir with spaces\\my file.c(7) : warning C4996: 'strcpy': This function or variable may be unsafe.",
          "warning 7:0 [C:\\dir with spaces\\my file.c] ((7)) {warning}" },
        { "C:\\x\\Program.cs(10,5): error CS1002: ; expected [C:\\x\\app\\app.csproj]",
          "error 10:5 [C:\\x\\Program.cs] ((10,5)) {error} <C:\\x\\app\\app.csproj>" },
        { "src/a.ts(3,7): error TS2322: Type 'string' is not assignable to type 'number'.", "error 3:7 [src/a.ts] ((3,7)) {error}" },
        { "  1>C:\\x\\a.cpp(5,1): error C2143: syntax error: missing ';' before '}'", "error 5:1 [C:\\x\\a.cpp] ((5,1)) {error}" },
        { "12>C:\\x\\b.cs(1,2,3,4): warning CS0168: The variable 'e' is declared but never used [C:\\x\\b.csproj]",
          "warning 1:2 [C:\\x\\b.cs] ((1,2,3,4)) {warning} <C:\\x\\b.csproj>" },
        // GCC and Clang
        { "src/main.c:12:5: error: 'x' undeclared (first use in this function)", "error 12:5 [src/main.c] (12:5) {error}" },
        { "C:\\x\\main.c:12:5: warning: unused variable 'y' [-Wunused-variable]", "warning 12:5 [C:\\x\\main.c] (12:5) {warning}" },
        { "src/main.c:3:10: note: in expansion of macro 'X'", "info 3:10 [src/main.c] (3:10) {note}" },
        { "a.c:12: undefined reference to `foo'", "error 12:0 [a.c] (12) {}" },
        { "src/main.c:12: error: expected ';' before '}' token", "error 12:0 [src/main.c] (12) {error}" },
        { "./x.h:5:1: error: unknown type name 'foo'", "error 5:1 [./x.h] (5:1) {error}" },
        { "In file included from src/a.c:3:", "info 3:0 [src/a.c] (3) {}" },
        { "                 from src/b.h:4,", "info 4:0 [src/b.h] (4) {}" },
        { "In file included from C:/x/a.c:3:10:", "info 3:10 [C:/x/a.c] (3:10) {}" },
        // Jai
        { "C:/jai/modules/x/main.jai:12,5: Error: Undeclared identifier 'x'.", "error 12:5 [C:/jai/modules/x/main.jai] (12,5) {Error}" },
        { "D:\\src\\game\\main.jai:3,1: Warning: This is unused.", "warning 3:1 [D:\\src\\game\\main.jai] (3,1) {Warning}" },
        { "D:\\src\\game\\main.jai:40,9: Info: This is the declaration.", "info 40:9 [D:\\src\\game\\main.jai] (40,9) {Info}" },
        // tsc's pretty form (its colors stripped before)
        { "src/a.ts:3:7 - error TS2322: Type 'string' is not assignable to type 'number'.", "error 3:7 [src/a.ts] (3:7) {error}" },
        // Traps: the structure alone rejects them (every path exists here)
        { "12:30:45", "-" },
        { "Build started 12:30:45.", "-" },
        { "Time Elapsed 00:00:01.23", "-" },
        { "http://host:80/x", "-" },
        { "GET http://localhost:8080/index.html: 200", "-" },
        { "version 1.2.3", "-" },
        { "Microsoft (R) C/C++ Optimizing Compiler Version 19.40.33811 for x64", "-" },
        { "LINK : fatal error LNK1104: cannot open file 'ronin.exe'", "-" },
        { "ronin.obj : error LNK2019: unresolved external symbol foo referenced in function main", "-" },
        { "C:\\Program Files (x86)\\x\\y.h", "-" },
        { "1 error generated.", "-" },
        { "", "-" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(located); i++) {
        String8 line = str8_cstr(located[i].line);
        CompileMatch m;
        String8 got = test_match_text(&t->arena, line, compile_match(line, &m), &m);
        TEST_CHECK(t, str8_equal(got, str8_cstr(located[i].want)), "compile match %d: '%S' gave '%S', expected '%s'", i, line, got,
                   located[i].want);
    }
    static const struct { const char *line, *want; } kinds[] = {
        { "LINK : fatal error LNK1104: cannot open file 'ronin.exe'", "error {fatal error}" },
        { "ronin.obj : error LNK2019: unresolved external symbol foo referenced in function main", "error {error}" },
        { "cl : command line warning D9025 : overriding '/W3' with '/W4'", "warning {warning}" },
        { "error: linker command failed with exit code 1 (use -v to see invocation)", "error {error}" },
        { "ld: error: undefined symbol: foo", "error {error}" },
        { "clang: error: no such file or directory: 'x.c'", "error {error}" },
        { "fatal error: no input files", "error {fatal error}" },
        { "warning: unused variable", "warning {warning}" },
        { "C:\\x\\proj.csproj : error MSB4019: The imported project was not found.", "error {error}" },
        { "MSBUILD : error MSB1009: Project file does not exist.", "error {error}" },
        { "error MSB4019: The imported project was not found.", "error {error}" },
        { "  3>warning CS8618: Non-nullable property 'Name' must contain a non-null value", "warning {warning}" },
        { "Error: Could not find the module.", "error {Error}" },
        // Not errors
        { "0 errors", "-" },
        { "Build succeeded, 0 Warning(s), 0 Error(s)", "-" },
        { "    0 Error(s)", "-" },
        { "compiling error.c", "-" },
        { "error.c", "-" },
        { "cl /W4 -Werror a.c", "-" },
        { "errors: 3", "-" },
        { "1 error generated.", "-" },
        { "Compilation finished with no errors", "-" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(kinds); i++) {
        String8 line = str8_cstr(kinds[i].line);
        CompileMatch m;
        String8 got = test_match_text(&t->arena, line, compile_match_kind(line, &m), &m);
        TEST_CHECK(t, str8_equal(got, str8_cstr(kinds[i].want)), "compile kind %d: '%S' gave '%S', expected '%s'", i, line, got, kinds[i].want);
    }
    // A tool's summary: every count word ("kind {word}" each), "-" for none.
    static const struct { const char *line, *want; } summaries[] = {
        // Clang
        { "1 error generated.", "error {error}" },
        { "2 warnings generated.", "warning {warnings}" },
        { "3 warnings and 1 error generated.", "warning {warnings} error {error}" },
        // MSBuild
        { "    0 Warning(s)", "warning {Warning(s)}" },
        { "    2 Error(s)", "error {Error(s)}" },
        { "Build succeeded, 0 Warning(s), 0 Error(s)", "warning {Warning(s)} error {Error(s)}" },
        // tsc
        { "Found 2 errors.", "error {errors}" },
        // Jai
        { "Compilation failed (1 error)", "error {error}" },
        { "0 errors", "error {errors}" },
        // Not summaries: GCC prints no count of its own; make's "Error 1" has its number after the word
        { "make: *** [all] Error 1", "-" },
        { "cc1: all warnings being treated as errors", "-" },
        { "errors: 3", "-" },
        { "Compilation finished with no errors", "-" },
        { "compiling error.c", "-" },
        { "v1.2 errors", "-" },
        { "x2 errors", "-" },
        { "100errors", "-" },
        { "2 errorsome things", "-" },
        { "", "-" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(summaries); i++) {
        String8 line = str8_cstr(summaries[i].line);
        CompileMatch m;
        b32 ok = compile_match_summary(line, &m);
        String8 got = STR8_LIT("-");
        CompileKind kind;
        i64 len, first = -1;
        static const char *names[] = { "none", "error", "warning", "info" };
        for (i64 at = compile_count_word(line, 0, &kind, &len); at >= 0; at = compile_count_word(line, at + len, &kind, &len)) {
            String8 one = str8_fmt(&t->arena, "%s {%S}", names[kind], str8(line.data + at, len));
            got = first < 0 ? one : str8_fmt(&t->arena, "%S %S", got, one);
            if (first < 0) first = at;
        }
        TEST_CHECK(t, str8_equal(got, str8_cstr(summaries[i].want)), "compile summary %d: '%S' gave '%S', expected '%s'", i, line, got,
                   summaries[i].want);
        TEST_CHECK(t, ok == (first >= 0) && (!ok || (m.summary && m.kind_off == first && !m.located)), "compile summary %d: the match", i);
    }
    LOG("test: ok: compile matchers (%d located lines and traps: MSVC with drive letters, spaces and (x86), cl / csc / tsc / MSBuild "
        "with its prefix and project, GCC and Clang with and without a column, bare, notes, includes, Jai, tsc's pretty form; times, "
        "URLs, versions, linker lines; %d lines with a kind and no location, and what does not count; %d summaries: Clang, MSBuild, "
        "tsc, Jai, and traps with the number after the word)",
        (i32)ARRAY_COUNT(located), (i32)ARRAY_COUNT(kinds), (i32)ARRAY_COUNT(summaries));
    return 1;
}

typedef struct TestCompileFs {
    const char **files;
    i32 count;
    i64 asked;
} TestCompileFs;

static b32 test_compile_exists(void *data, Arena *arena, String8 path) {
    (void)arena;
    TestCompileFs *fs = data;
    fs->asked++;
    for (i32 i = 0; i < fs->count; i++) if (compile_path_equal(str8_cstr(fs->files[i]), path)) return 1;
    return 0;
}

static b32 test_compile_resolve(Test *t) {
    static const char *files[] = { "C:\\b\\x.c", "C:\\b\\sub\\y.c", "C:\\b\\app\\z.cs", "C:\\r\\lib\\w.c", "C:\\b\\both.c", "C:\\r\\both.c" };
    TestCompileFs fs = { files, ARRAY_COUNT(files), 0 };
    Arena arena = arena_create(MB(64));
    CompileCache c;
    compile_cache_init(&c, &arena, test_compile_exists, &fs);
    static const struct { const char *path, *proj, *want; } cases[] = {
        { "C:\\b\\x.c", "", "C:\\b\\x.c" },               // absolute, as given
        { "C:/b/x.c", "", "C:\\b\\x.c" },                 // ... with '/'
        { "C:\\nope\\x.c", "", "" },                      // absolute and missing: nothing else is tried
        { "x.c", "", "C:\\b\\x.c" },                      // the build directory
        { "sub/y.c", "", "C:\\b\\sub\\y.c" },
        { "z.cs", "C:\\b\\app\\app.csproj", "C:\\b\\app\\z.cs" }, // beside MSBuild's project file
        { "z.cs", "app\\app.csproj", "C:\\b\\app\\z.cs" },        // ... given relative to the build directory
        { "z.cs", "", "" },                               // ... and not without it
        { "lib/w.c", "", "C:\\r\\lib\\w.c" },             // the project root
        { "both.c", "", "C:\\b\\both.c" },                // the earlier step wins
        { "gone.c", "", "" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        String8 got = compile_resolve(&c, str8_cstr(cases[i].path), STR8_LIT("C:\\b"), str8_cstr(cases[i].proj), STR8_LIT("C:\\r"));
        TEST_CHECK(t, str8_equal(got, str8_cstr(cases[i].want)), "compile resolve %d: '%s' gave '%S', expected '%s'", i, cases[i].path, got,
                   cases[i].want);
    }
    i64 asked = fs.asked;
    for (i32 i = 0; i < 100; i++) compile_resolve(&c, STR8_LIT("x.c"), STR8_LIT("C:\\b"), str8(NULL, 0), STR8_LIT("C:\\r"));
    TEST_CHECK(t, fs.asked == asked && c.asked == asked, "compile resolve: a cached path was asked again (%D, %D)", fs.asked, asked);
    // No root (compile outside a project): that step is skipped.
    String8 none = compile_resolve(&c, STR8_LIT("lib/w.c"), STR8_LIT("C:\\b"), str8(NULL, 0), str8(NULL, 0));
    TEST_CHECK(t, !none.len, "compile resolve: no root, yet '%S'", none);
    // A full cache forgets everything and still answers.
    for (i32 i = 0; i < COMPILE_CACHE_CAP + 10; i++) compile_resolve(&c, str8_fmt(&t->arena, "f%d.c", i), STR8_LIT("C:\\b"), str8(NULL, 0), str8(NULL, 0));
    String8 again = compile_resolve(&c, STR8_LIT("x.c"), STR8_LIT("C:\\b"), str8(NULL, 0), str8(NULL, 0));
    TEST_CHECK(t, str8_equal(again, STR8_LIT("C:\\b\\x.c")) && c.count <= COMPILE_CACHE_CAP, "compile resolve: after a full cache '%S'", again);
    os_release(arena.base);
    // The path *compilation* shows (compilation_relative_paths): relative to the root only for an absolute printed path under it.
    static const struct { const char *printed, *file, *root, *want; } shown[] = {
        { "C:\\r\\src\\a.c", "C:\\r\\src\\a.c", "C:\\r", "src\\a.c" },
        { "C:/r/src/a.c", "C:\\r\\src\\a.c", "C:\\r", "src\\a.c" },          // printed with '/'
        { "c:\\R\\a.c", "c:\\R\\a.c", "C:\\r\\", "a.c" },                     // case, a root with a separator
        { "C:\\r\\src\\a.c", "C:\\r\\src\\a.c", "C:/r", "src\\a.c" },         // a root with '/'
        { "C:\\other\\a.c", "C:\\other\\a.c", "C:\\r", "" },                  // outside the root: as it is
        { "C:\\rr\\a.c", "C:\\rr\\a.c", "C:\\r", "" },                        // a sibling with the root as a prefix
        { "src\\a.c", "C:\\r\\src\\a.c", "C:\\r", "" },                       // printed relative: as printed
        { "C:\\r\\a.c", "C:\\r\\a.c", "", "" },                               // no root
        { "C:\\r", "C:\\r", "C:\\r", "" },                                    // the root itself
        { "\\\\srv\\share\\p\\a.c", "\\\\srv\\share\\p\\a.c", "\\\\srv\\share\\p", "a.c" }, // UNC
    };
    for (i32 i = 0; i < ARRAY_COUNT(shown); i++) {
        String8 got = compile_shown_path(str8_cstr(shown[i].printed), str8_cstr(shown[i].file), str8_cstr(shown[i].root));
        TEST_CHECK(t, str8_equal(got, str8_cstr(shown[i].want)), "compile shown path %d: '%s' under '%s' gave '%S', expected '%s'", i,
                   shown[i].printed, shown[i].root, got, shown[i].want);
    }
    LOG("test: ok: compile path resolution (absolute as given, the build directory, beside MSBuild's project file absolute or relative, "
        "the project root, the earlier step first, nowhere, the cache asked once per path, no root, a full cache); the shown path (%d "
        "cases: under the root, '/', case, outside, a sibling prefix, printed relative, no root, UNC)", (i32)ARRAY_COUNT(shown));
    return 1;
}

typedef struct TestLines {
    Arena *arena;
    String8 *lines;
    i32 count, cap;
} TestLines;

static void test_lines_out(void *data, String8 raw) {
    TestLines *l = data;
    if (l->count < l->cap) l->lines[l->count++] = str8_copy(l->arena, raw);
}

static b32 test_compile_clean(Test *t) {
    static const struct { const char *raw, *want; } cases[] = {
        { "plain", "plain" },
        { "ab\r", "ab" },
        { "a\rb\r", "ab" },
        { "\x1b[1;31merror\x1b[0m: x", "error: x" },
        { "\x1b[38;5;196mred\x1b[m and \x1b[Kgone", "red and gone" },
        { "\x1b]0;title\x07rest", "rest" },
        { "\x1b]8;;http://x\x1b\\link\x1b]8;;\x1b\\", "link" },
        { "\x1b(Bok", "ok" },
        { "ab\x1b", "ab" },
        { "\xc4\x9f\xc3\xbc\xc5\x9f", "\xc4\x9f\xc3\xbc\xc5\x9f" }, // valid UTF-8 stays
        { "tab\there", "tab\there" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        String8 got = compile_clean(&t->arena, str8_cstr(cases[i].raw));
        TEST_CHECK(t, str8_equal(got, str8_cstr(cases[i].want)), "compile clean %d: '%S', expected '%s'", i, got, cases[i].want);
    }
    // OEM: 0x81 0x82 are u-umlaut and e-acute on the Western and Turkish OEM code pages (437, 850, 852, 857, 858).
    String8 oem = compile_clean(&t->arena, STR8_LIT("x \x81\x82 y"));
    b32 oem_known = str8_equal(oem, STR8_LIT("x \xc3\xbc\xc3\xa9 y"));
    TEST_CHECK(t, compile_utf8_valid(oem) && oem.len > 6, "compile clean: OEM text '%S' is not valid UTF-8", oem);
    if (!oem_known) LOG("test: skipped: the OEM code page maps 0x81 0x82 to '%S' (not 437 / 850 / 852 / 857 / 858)", oem);
    TEST_CHECK(t, !compile_utf8_valid(STR8_LIT("\xc3")) && !compile_utf8_valid(STR8_LIT("\xed\xa0\x80")) && compile_utf8_valid(STR8_LIT("\xef\xbf\xbd")),
               "compile clean: UTF-8 validity");
    // Lines across reads: random slicing gives the lines of one whole read; a long line is cut on characters.
    String8 text;
    {
        u8 *p = PUSH_ARRAY(&t->arena, u8, MB(1));
        i64 n = 0;
        for (i32 i = 0; i < 2000; i++) {
            i64 len = test_below(t, 4) == 0 ? test_below(t, 300) : test_below(t, 40);
            for (i64 k = 0; k < len; k++) p[n++] = (u8)(test_below(t, 10) == 0 ? '\r' : 'a' + test_below(t, 26));
            p[n++] = '\n';
        }
        for (i32 k = 0; k < 75000; k++) { // one line of 150,000 bytes of two-byte characters
            p[n++] = 0xC3;
            p[n++] = 0xA9;
        }
        p[n++] = '\n';
        memcpy(p + n, "no newline at the end", 21);
        n += 21;
        text = str8(p, n);
    }
    TestLines whole = { &t->arena, PUSH_ARRAY(&t->arena, String8, 4000), 0, 4000 };
    CompileSplit s;
    compile_split_init(&s, &t->arena);
    compile_split_feed(&s, text.data, text.len, test_lines_out, &whole);
    compile_split_end(&s, test_lines_out, &whole);
    TEST_CHECK(t, whole.count == 2000 + 3 + 1, "compile split: %d lines from one read", whole.count);
    for (i32 i = 2000; i < 2003; i++) {
        TEST_CHECK(t, whole.lines[i].len <= COMPILE_LINE_MAX && compile_utf8_valid(whole.lines[i]), "compile split: the long line's piece %d", i);
    }
    TEST_CHECK(t, whole.lines[2000].len + whole.lines[2001].len + whole.lines[2002].len == 150000, "compile split: the long line's pieces");
    TEST_CHECK(t, str8_equal(whole.lines[2003], STR8_LIT("no newline at the end")), "compile split: the last line without a newline");
    for (i32 round = 0; round < 50; round++) {
        TestLines sliced = { &t->arena, PUSH_ARRAY(&t->arena, String8, 4000), 0, 4000 };
        compile_split_init(&s, &t->arena);
        for (i64 at = 0; at < text.len;) {
            i64 r = 1 + test_below(t, round % 5 == 1 ? 7 : 5000); // not inside MIN: it would be drawn twice
            i64 n = MIN(text.len - at, r);
            compile_split_feed(&s, text.data + at, n, test_lines_out, &sliced);
            at += n;
        }
        compile_split_end(&s, test_lines_out, &sliced);
        TEST_CHECK(t, sliced.count == whole.count, "compile split: round %d gave %d lines, not %d", round, sliced.count, whole.count);
        for (i32 i = 0; i < whole.count; i++) {
            TEST_CHECK(t, str8_equal(sliced.lines[i], whole.lines[i]), "compile split: round %d line %d differs (%D bytes, not %D)", round, i, sliced.lines[i].len,
                       whole.lines[i].len);
        }
        arena_pop_to(&t->arena, (u64)((u8 *)sliced.lines - t->arena.base));
    }
    LOG("test: ok: compile cleaning (CR, CSI / OSC / charset escapes, a lone ESC, valid UTF-8 kept, OEM %s, validity) and lines across "
        "reads (50 random slicings of 2,000 lines equal to one read, a 150,000-byte line cut on characters, the last line without a "
        "newline)", oem_known ? "converted" : "converted (code page not checked)");
    return 1;
}

// The build settings and colors: defaults, every value, the diagnostics.
static b32 test_compile_config(Test *t) {
    Config c;
    config_init(&c);
    config_parse(&c, &t->arena, config_default_text(), STR8_LIT("defaults"));
    Settings *s = &c.settings;
    TEST_CHECK(t, s->compilation_save_buffers == COMPILATION_SAVE_ASK && s->compilation_scroll_output == COMPILATION_SCROLL_FIRST_ERROR &&
                      s->compilation_max_output_mb == 16 && s->compilation_relative_paths, "compile config: defaults %d %d %d %d",
               s->compilation_save_buffers, s->compilation_scroll_output, s->compilation_max_output_mb, s->compilation_relative_paths);
    config_parse(&c, &t->arena, STR8_LIT("[settings]\ncompilation_relative_paths = false\n"), STR8_LIT("t.conf"));
    TEST_CHECK(t, c.diags.errors == 0 && !s->compilation_relative_paths, "compile config: compilation_relative_paths = false");
    TEST_CHECK(t, c.theme.compilation_error == 0xff0000 && c.theme.compilation_warning == 0xffaa00 && c.theme.compilation_info == 0x8cde94,
               "compile config: default colors %x %x %x", c.theme.compilation_error, c.theme.compilation_warning, c.theme.compilation_info);
    static const struct { const char *line; i32 save, scroll, mb; } values[] = {
        { "compilation_save_buffers = all", COMPILATION_SAVE_ALL, -1, -1 },
        { "compilation_save_buffers = none", COMPILATION_SAVE_NONE, -1, -1 },
        { "compilation_save_buffers = ask", COMPILATION_SAVE_ASK, -1, -1 },
        { "compilation_scroll_output = true", -1, COMPILATION_SCROLL_ALWAYS, -1 },
        { "compilation_scroll_output = false", -1, COMPILATION_SCROLL_NEVER, -1 },
        { "compilation_scroll_output = first-error", -1, COMPILATION_SCROLL_FIRST_ERROR, -1 },
        { "compilation_max_output_mb = 1024", -1, -1, 1024 },
    };
    for (i32 i = 0; i < ARRAY_COUNT(values); i++) {
        config_parse(&c, &t->arena, str8_fmt(&t->arena, "[settings]\n%s\n", values[i].line), STR8_LIT("t.conf"));
        TEST_CHECK(t, c.diags.errors == 0 && (values[i].save < 0 || s->compilation_save_buffers == values[i].save) &&
                          (values[i].scroll < 0 || s->compilation_scroll_output == values[i].scroll) &&
                          (values[i].mb < 0 || s->compilation_max_output_mb == values[i].mb), "compile config: '%s'", values[i].line);
    }
    Config bad;
    config_init(&bad);
    config_parse(&bad, &t->arena,
                 STR8_LIT("[settings]\ncompilation_save_buffers = some\ncompilation_scroll_output = yes\ncompilation_max_output_mb = 0\n"
                          "compilation_relative_paths = maybe\n[colors]\ncompilation_error = red\ncompilation_info = #010203\n"),
                 STR8_LIT("t.conf"));
    static const char *const want[] = {
        "t.conf:2: compilation_save_buffers: 'some' is not ask, all or none",
        "t.conf:3: compilation_scroll_output: 'yes' is not first-error, true or false",
        "t.conf:4: warning: compilation_max_output_mb 0 is out of range (1 to 1024), using 1",
        "t.conf:5: compilation_relative_paths: 'maybe' is not true or false",
        "t.conf:7: compilation_error: 'red' is not a color (#rrggbb)",
    };
    i32 k = 0;
    for (ConfigDiag *d = bad.diags.first; d; d = d->next, k++) {
        TEST_CHECK(t, k < ARRAY_COUNT(want) && str8_equal(d->text, str8_cstr(want[k])), "compile config: diagnostic %d '%S'", k, d->text);
    }
    TEST_CHECK(t, k == ARRAY_COUNT(want) && bad.settings.compilation_max_output_mb == 1 && bad.theme.compilation_info == 0x010203,
               "compile config: %d diagnostics", k);
    LOG("test: ok: build settings and colors (defaults, every value of compilation_save_buffers and compilation_scroll_output, "
        "compilation_max_output_mb clamped, compilation_relative_paths, the colors, each diagnostic)");
    return 1;
}

// ---------------------------------------------------------------------------
// Builds through the reader worker

typedef struct TestBuild {
    CompileRun *c;
    String8 text;      // every line and LF
    i32 lines, located, errors, warnings, infos, summaries;
    String8 first_file; // the first located line's file (in first_buf)
    u8 first_buf[1024];
    i64 first_line, first_col;
    b32 ended;
} TestBuild;

// Takes what arrived; stops (with the worker still running) once `until` appears in the text.
static void test_build_pump(TestBuild *b, u8 *out, i64 cap, i64 *n) {
    CompileBatch *batch = compile_take(b->c);
    while (batch) {
        CompileBatch *next = batch->next;
        CompileLine l;
        for (i64 at = 0; compile_next_line(batch, &at, &l);) {
            b->lines++;
            if (l.located) {
                if (!b->located) {
                    i64 fl = MIN(l.file.len, (i64)sizeof(b->first_buf));
                    memcpy(b->first_buf, l.file.data, (size_t)fl);
                    b->first_file = str8(b->first_buf, fl);
                    b->first_line = l.line;
                    b->first_col = l.col;
                }
                b->located++;
            }
            if (l.summary) {
                b->summaries++; // not counted, as results_compile_line does
            } else {
                b->errors += l.kind == COMPILE_ERROR;
                b->warnings += l.kind == COMPILE_WARNING;
                b->infos += l.kind == COMPILE_INFO;
            }
            i64 take = MIN(l.text.len, cap - *n - 1);
            if (take > 0) {
                memcpy(out + *n, l.text.data, (size_t)take);
                *n += take;
            }
            if (*n < cap) out[(*n)++] = '\n';
        }
        compile_give_back(b->c, batch);
        batch = next;
    }
}

// Runs a build until it ended (or the timeout); with `kill_at` set, kills it once that text was read.
static b32 test_build(Test *t, String8 command, String8 dir, String8 root, i64 cap, const char *kill_at, u32 timeout_ms, TestBuild *b) {
    *b = (TestBuild){ 0 };
    b->c = compile_create(command, dir, root, cap);
    if (!compile_start(b->c)) return 0;
    i64 out_cap = MB(8), n = 0;
    u8 *out = PUSH_ARRAY(&t->arena, u8, out_cap);
    u64 start = os_time_us();
    b32 killed = 0;
    while (!b->c->ended) {
        if (os_time_us() - start > (u64)timeout_ms * 1000) break;
        os_dev_wake_wait(5);
        test_build_pump(b, out, out_cap, &n);
        if (kill_at && !killed) {
            String8 key = str8_cstr(kill_at);
            for (i64 i = 0; i + key.len <= n && !killed; i++) killed = mem_equal(out + i, key.data, key.len);
            if (killed) compile_cancel(b->c);
        }
    }
    b->text = str8(out, n);
    b->ended = b->c->ended;
    return b->ended;
}

static void test_build_free(TestBuild *b) {
    if (b->c && (b->c->ended || !b->c->started)) compile_free(b->c);
    b->c = NULL;
}

static b32 test_compile_run(Test *t) {
    i32 procs0 = os_dev_live_processes(), threads0 = os_dev_live_threads(), events0 = os_dev_live_events();
    String8 dir = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p13", t->tmp_dir));
    os_make_dir(dir);
    TestBuild b;
    // Order, stderr in it, the exit code.
    TEST_CHECK(t, test_build(t, test_child(t, "b_order", STR8_LIT("out one\nerr two\nout three\nexit 3\n")), dir, str8(NULL, 0), MB(16), NULL, 10000, &b),
               "build: the child did not end");
    TEST_CHECK(t, str8_equal(b.text, STR8_LIT("one\ntwo\nthree\n")) && b.c->exit_code == 3 && !b.c->killed && b.c->start_status == OS_FILE_OK,
               "build: '%S', exit %u, killed %d", b.text, b.c->exit_code, b.c->killed);
    test_build_free(&b);
    static const struct { const char *script; u32 code; } codes[] = { { "exit 0\n", 0 }, { "exit 255\n", 255 }, { "exit -1\n", 0xFFFFFFFFu } };
    for (i32 i = 0; i < ARRAY_COUNT(codes); i++) {
        test_build(t, test_child(t, "b_code", str8_cstr(codes[i].script)), dir, str8(NULL, 0), MB(16), NULL, 10000, &b);
        TEST_CHECK(t, b.ended && b.c->exit_code == codes[i].code, "build: exit code %u, expected %u", b.c->exit_code, codes[i].code);
        test_build_free(&b);
    }
    // Cleaning on the way: CRLF, ANSI, OEM, a line across reads, the last line without a newline.
    test_build(t, test_child(t, "b_clean", STR8_LIT("raw \\e[1;31merror\\e[0m: red\\r\\n\nraw abc\nsleep 60\nraw def\\r\\n\nraw x \\x81\\x82 y\\r\\n\nraw tail\n")),
               dir, str8(NULL, 0), MB(16), NULL, 10000, &b);
    String8 want_clean = str8_fmt(&t->arena, "error: red\nabcdef\n%S\ntail\n", compile_clean(&t->arena, STR8_LIT("x \x81\x82 y")));
    TEST_CHECK(t, b.ended && str8_equal(b.text, want_clean) && b.errors == 1, "build: cleaned text '%S' (%d errors)", b.text, b.errors);
    test_build_free(&b);
    // The cap: one "[output truncated]", the rest read to the end (the child is never blocked).
    test_build(t, test_child(t, "b_cap", STR8_LIT("flood 2000000\nexit 0\n")), dir, str8(NULL, 0), 100000, NULL, 20000, &b);
    i32 marks = 0;
    for (i64 i = 0; i + 18 <= b.text.len; i++) marks += mem_equal(b.text.data + i, COMPILE_TRUNCATED, 18);
    TEST_CHECK(t, b.ended && b.c->truncated && marks == 1 && b.c->bytes == 2000000 && b.c->exit_code == 0 && b.text.len <= 100000 + 64,
               "build: the cap: ended %d, truncated %d, %d marks, %D bytes read, %D kept", b.ended, b.c->truncated, marks, b.c->bytes, b.text.len);
    test_build_free(&b);
    // A command that does not exist (cmd says so, exit 1); a directory that does not exist (no start).
    test_build(t, STR8_LIT("no_such_command_ronin_p13"), dir, str8(NULL, 0), MB(16), NULL, 10000, &b);
    TEST_CHECK(t, b.ended && b.c->exit_code == 1 && b.lines >= 1, "build: a missing command: exit %u, %d lines", b.c->exit_code, b.lines);
    test_build_free(&b);
    test_build(t, STR8_LIT("echo x"), str8_fmt(&t->arena, "%S\\no_such_dir", dir), str8(NULL, 0), MB(16), NULL, 10000, &b);
    TEST_CHECK(t, b.ended && b.c->start_status == OS_FILE_NOT_FOUND && !b.lines, "build: a missing directory: status %d, %d lines",
               (i32)b.c->start_status, b.lines);
    test_build_free(&b);
    test_build(t, test_child(t, "b_cwd", STR8_LIT("cwd\n")), dir, str8(NULL, 0), MB(16), NULL, 10000, &b);
    TEST_CHECK(t, str8_equal(b.text, str8_fmt(&t->arena, "%S\n", dir)), "build: cwd '%S'", b.text);
    test_build_free(&b);
    // Locations through the file system: a .bat that changes directory, MSBuild's project file, the root.
    String8 root = str8_fmt(&t->arena, "%S\\p13res", dir);
    String8 bdir = str8_fmt(&t->arena, "%S\\build", root), sub = str8_fmt(&t->arena, "%S\\sub", root);
    os_make_dir(root);
    os_make_dir(bdir);
    os_make_dir(sub);
    os_write_file(str8_fmt(&t->arena, "%S\\x.c", sub), STR8_LIT("int x;\n"));
    os_write_file(str8_fmt(&t->arena, "%S\\here.c", bdir), STR8_LIT("int h;\n"));
    String8 sub_fwd = str8_copy(&t->arena, sub); // the child unescapes backslashes
    for (i64 i = 0; i < sub_fwd.len; i++) if (sub_fwd.data[i] == 92) sub_fwd.data[i] = '/';
    String8 loc_cmd = test_child(t, "b_loc", str8_fmt(&t->arena,
        "out here.c(2,3): warning C4100: in the build directory\n"
        "out sub/x.c(3): error C1: under the root\n"
        "out x.c(4): error C2: beside the project [%S/s.vcxproj]\n"
        "out x.c(5): error C3: nowhere\n"
        "out x.c:6: nowhere and no kind\n"
        "out LINK : fatal error LNK1104: cannot open file 'a.exe'\n"
        "out 4 errors and 1 warning generated.\n"
        "exit 2\n", sub_fwd));
    os_write_file(str8_fmt(&t->arena, "%S\\go.bat", bdir), str8_fmt(&t->arena, "@cd ..\\sub\r\n@%S\r\n", loc_cmd));
    // ".\": cmd may be told not to look in the current directory (NoDefaultCurrentDirectoryInExePath).
    test_build(t, STR8_LIT(".\\go.bat"), bdir, root, MB(16), NULL, 10000, &b);
    TEST_CHECK(t, b.ended && b.lines == 7 && b.located == 3 && b.errors == 4 && b.warnings == 1 && b.summaries == 1 && b.c->exit_code == 2,
               "build: locations: %d lines, %d located, %d errors, %d warnings, %d summaries, exit %u: '%S'", b.lines, b.located, b.errors,
               b.warnings, b.summaries, b.c->exit_code, b.text);
    TEST_CHECK(t, str8_equal(b.first_file, str8_fmt(&t->arena, "%S\\here.c", bdir)) && b.first_line == 2 && b.first_col == 3,
               "build: the first location '%S' %D:%D", b.first_file, b.first_line, b.first_col);
    test_build_free(&b);
    TEST_CHECK(t, os_dev_live_processes() == procs0 && os_dev_live_threads() == threads0 && os_dev_live_events() == events0,
               "build: counters back (%d processes, %d threads, %d events)", os_dev_live_processes(), os_dev_live_threads(), os_dev_live_events());
    LOG("test: ok: builds (order with stderr, exit codes 3 0 255 -1, CRLF / ANSI / OEM / a line across reads / no last newline, the cap "
        "with one mark and the rest read, a missing command, a missing directory, the working directory, locations from a .bat that "
        "changes directory: the build directory, the root, MSBuild's project, nowhere as an error without a location, the linker, a "
        "summary not counted)");
    return 1;
}

static b32 test_compile_kill(Test *t) {
    i32 procs0 = os_dev_live_processes(), threads0 = os_dev_live_threads(), events0 = os_dev_live_events();
    String8 dir = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p13", t->tmp_dir));
    test_child(t, "hang", STR8_LIT("hang\n"));
    String8 hang = str8_fmt(&t->arena, "%S\\hang.txt", dir);
    TestBuild b;
    // The child and its grandchild end.
    u64 t0 = os_time_us();
    test_build(t, test_child(t, "k_tree", str8_fmt(&t->arena, "spawn %S\nout spawned\nhang\n", hang)), dir, str8(NULL, 0), MB(16), "spawned", 10000, &b);
    u64 took = os_time_us() - t0;
    u32 gpid = test_grandchild_pid(b.text);
    b32 gone = gpid && test_pid_gone(gpid, 1000);
    if (gpid && !gone) os_dev_kill_pid(gpid);
    TEST_CHECK(t, b.ended && b.c->killed && gone, "kill: ended %d, killed %d, grandchild %u gone %d ('%S')", b.ended, b.c->killed, gpid, gone, b.text);
    test_build_free(&b);
    // A kill during heavy output.
    t0 = os_time_us();
    test_build(t, test_child(t, "k_flood", STR8_LIT("flood 50000000\nexit 0\n")), dir, str8(NULL, 0), MB(128), "flood line 100:", 10000, &b);
    u64 flood_took = os_time_us() - t0;
    TEST_CHECK(t, b.ended && b.c->killed && b.c->bytes < 50000000, "kill: during a flood: ended %d, killed %d, %D bytes", b.ended, b.c->killed,
               b.c->bytes);
    test_build_free(&b);
    // A normal end leaves a lingering grandchild alone.
    test_build(t, test_child(t, "k_linger", str8_fmt(&t->arena, "spawn %S\nexit 0\n", hang)), dir, str8(NULL, 0), MB(16), NULL, 10000, &b);
    gpid = test_grandchild_pid(b.text);
    b32 alive = gpid && os_dev_process_alive(gpid);
    if (gpid) os_dev_kill_pid(gpid);
    TEST_CHECK(t, b.ended && !b.c->killed && alive, "kill: a normal end: ended %d, killed %d, grandchild %u alive %d", b.ended, b.c->killed, gpid, alive);
    test_build_free(&b);
    // Starts and kills, many times: each ends within 500 ms of its cancel; nothing grows.
    String8 slow = test_child(t, "k_slow", STR8_LIT("out a\nsleep 10\nout b\nhang\n"));
    i32 handles_first = 0;
    u64 worst = 0;
    for (i32 round = 0; round < 100; round++) {
        CompileRun *c = compile_create(slow, dir, str8(NULL, 0), MB(16));
        TEST_CHECK(t, compile_start(c), "kill: round %d did not start", round);
        if (round % 3 == 1) { // after some output
            os_dev_wake_wait(2000);
        } else if (round % 3 == 2) {
            u64 w0 = os_time_us();
            while (os_time_us() - w0 < (u64)(round % 7) * 1000) {}
        }
        u64 c0 = os_time_us();
        compile_cancel(c);
        while (!c->ended && os_time_us() - c0 < 3000000) {
            os_dev_wake_wait(5);
            for (CompileBatch *batch = compile_take(c), *next; batch; batch = next) {
                next = batch->next;
                compile_give_back(c, batch);
            }
        }
        u64 end_took = os_time_us() - c0;
        worst = MAX(worst, end_took);
        TEST_CHECK(t, c->ended && end_took <= 500000, "kill: round %d ended %d after %U ms", round, c->ended, end_took / 1000);
        compile_free(c);
        if (!round) handles_first = os_dev_handle_count();
    }
    TEST_CHECK(t, os_dev_handle_count() <= handles_first + 2, "kill: handles grew from %d to %d over 100 rounds", handles_first, os_dev_handle_count());
    TEST_CHECK(t, os_dev_live_processes() == procs0 && os_dev_live_threads() == threads0 && os_dev_live_events() == events0,
               "kill: counters back (%d processes, %d threads, %d events)", os_dev_live_processes(), os_dev_live_threads(), os_dev_live_events());
    LOG("test: ok: build kills (the child and its grandchild gone, %U ms; during a 50 MB flood, %U ms; a normal end lets a grandchild go; "
        "100 start / kill rounds, the worst %U ms from the cancel to the end; handles %d -> %d)", took / 1000, flood_took / 1000, worst / 1000,
        handles_first, os_dev_handle_count());
    return 1;
}

// ---------------------------------------------------------------------------
// Builds through the headless app

// Frames until *compilation* holds `what` (or the timeout).
// The build commands remembered per directory (Phase 16): 10,000 accepted prompts over three directories must not grow
// their arenas past what the first 100 needed, and each directory keeps its last command.
static b32 test_compile_remember_memory(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "compile remember: app_create failed");
    app->compile_next_dir = STR8_LIT("C:\\leak\\project");
    app->compile_next_root = STR8_LIT("C:\\leak\\project");
    u64 committed_100 = 0;
    for (i32 i = 1; i <= 10000; i++) {
        u64 mark = arena_pos(&t->arena);
        String8 key = str8_fmt(&t->arena, "C:\\leak\\project%d", i % 3);
        String8 command = str8_fmt(&t->arena, "build.bat release %d", i % 7); // the same length each time
        results_compile_remember(app, key, command);
        arena_pop_to(&t->arena, mark);
        if (i == 100) committed_100 = app->compile_arena.committed + app->compile_spare.committed;
    }
    u64 committed = app->compile_arena.committed + app->compile_spare.committed;
    b32 kept = app->compile_command_count == 3;
    for (i32 k = 0; k < 3 && kept; k++) {
        String8 got = results_compile_remembered(app, str8_fmt(&t->arena, "C:\\leak\\project%d", k));
        i32 last = 10000 - (10000 - k) % 3; // the last i with i % 3 == k
        kept = str8_equal(got, str8_fmt(&t->arena, "build.bat release %d", last % 7));
    }
    TEST_CHECK(t, kept && str8_equal(app->compile_last_dir, STR8_LIT("C:\\leak\\project")), "compile remember: the commands kept are wrong");
    TEST_CHECK(t, committed == committed_100, "compile remember: the arenas grew from %U to %U bytes between prompt 100 and 10,000",
               committed_100, committed);
    if (!test_app_destroy(t, app, "compile remember")) return 0;
    LOG("test: ok: compile remember (10,000 prompts over 3 directories, %U bytes committed after 100 and after 10,000)", committed);
    return 1;
}

static b32 test_compile_wait_text(Test *t, App *app, const char *what, u32 timeout_ms) {
    u64 start = os_time_us();
    while (os_time_us() - start < (u64)timeout_ms * 1000) {
        Buffer *b = buffer_list_find_name(&app->buffers, STR8_LIT("*compilation*"));
        if (b) {
            u64 mark = arena_pos(&t->arena);
            b32 has = test_contains(buffer_text(b, &t->arena, 0, buffer_size(b)), what);
            arena_pop_to(&t->arena, mark);
            if (has) return 1;
        }
        if (os_dev_wake_wait(app_wants_frame(app) ? 0 : 5)) app_wake(app);
        app_dev_feed_events(app, NULL, 0, &t->arena);
    }
    return 0;
}

static String8 test_compile_text(Test *t, App *app) {
    Buffer *b = buffer_list_find_name(&app->buffers, STR8_LIT("*compilation*"));
    return b ? buffer_text(b, &t->arena, 0, buffer_size(b)) : str8(NULL, 0);
}

static String8 test_compile_mode(Test *t, App *app) {
    Buffer *b = buffer_list_find_name(&app->buffers, STR8_LIT("*compilation*"));
    return b ? results_mode_name(app, b, &t->arena) : str8(NULL, 0);
}

// The point of the window that shows *compilation* (-1: none).
static i64 test_compile_window_point(App *app) {
    View *views[WINDOW_MAX];
    for (i32 i = 0, n = app_views(app, views); i < n; i++) {
        if (str8_equal(views[i]->buffer->name, STR8_LIT("*compilation*"))) return view_point(views[i], &views[i]->cursors[0]);
    }
    return -1;
}

static String8 test_mini_input(Test *t, App *app) {
    return buffer_text(app->mini.buffer, &t->arena, 0, buffer_size(app->mini.buffer));
}

static b32 test_compile_app(Test *t) {
    i32 procs0 = os_dev_live_processes(), threads0 = os_dev_live_threads(), events0 = os_dev_live_events();
    String8 root = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p13app", t->tmp_dir));
    os_make_dir(root);
    os_make_dir(str8_fmt(&t->arena, "%S\\sub", root));
    String8 a_c = str8_fmt(&t->arena, "%S\\a.c", root);
    os_write_file(a_c, STR8_LIT("line one\nint y = x;\nline three\nint z\nline five\n"));
    os_write_file(str8_fmt(&t->arena, "%S\\b.h", root), STR8_LIT("a\nb\nunused c\n"));
    os_write_file(str8_fmt(&t->arena, "%S\\note.h", root), STR8_LIT("int x;\n"));
    String8 shown = str8_fmt(&t->arena, "%S/", root);
    for (i64 i = 0; i < shown.len; i++) if (shown.data[i] == '\\') shown.data[i] = '/';
    String8 cmd = test_child(t, "app_build", STR8_LIT("out Building...\n"
                                                     "out a.c(2,5): error C2065: 'x': undeclared identifier\n"
                                                     "out note.h(1): note: see declaration of 'x'\n"
                                                     "out b.h:3:1: warning: unused [-Wunused]\n"
                                                     "out a.c(4): error C2143: missing ';'\n"
                                                     "out done\n"
                                                     "exit 2\n"));
    os_write_file(str8_fmt(&t->arena, "%S\\.ronin-project", root), str8_fmt(&t->arena, "[build]\ncommand = %S\n", cmd));
    String8 none = test_results_conf(t, "p13none.conf", "[settings]\ncompilation_save_buffers = none\n");
    String8 want_head = str8_fmt(&t->arena,
                                 "Command: %S\nDirectory: %S\n\nBuilding...\n"
                                 "a.c(2,5): error C2065: 'x': undeclared identifier\n"
                                 "note.h(1): note: see declaration of 'x'\n"
                                 "b.h:3:1: warning: unused [-Wunused]\n"
                                 "a.c(4): error C2143: missing ';'\n"
                                 "done\n\nCompilation exited abnormally with code 2 in ", cmd, shown);

    // The whole flow: the prompt with .ronin-project's command, *compilation* in the other window (not
    // selected), its exact text, the state, the summary, the locations; next-error with the landing
    // highlight; RET, n, p; a note skipped; g.
    {
        App *app = test_manual_app(t, a_c, 0, 0, none);
        Buffer *ab = test_current(app);
        TEST_CHECK(t, str8_equal(test_compile_mode(t, app), STR8_LIT("")), "compile app: a mode before any build");
        app_dev_feed(app, "C-x p c", &t->arena);
        TEST_CHECK(t, str8_equal(app_dev_prompt(app), STR8_LIT("Compile command: ")) && str8_equal(test_mini_input(t, app), cmd),
                   "compile app: the prompt '%S' with '%S'", app_dev_prompt(app), test_mini_input(t, app));
        TEST_CHECK(t, !buffer_list_find_name(&app->buffers, STR8_LIT("*compilation*")) && os_dev_live_processes() == procs0,
                   "compile app: something ran before RET");
        app_dev_feed(app, "RET", &t->arena);
        Buffer *cb = buffer_list_find_name(&app->buffers, STR8_LIT("*compilation*"));
        TEST_CHECK(t, cb && test_current(app) == ab && app_dev_window_count(app) == 2, "compile app: *compilation* not shown beside a.c");
        TEST_CHECK(t, str8_equal(test_compile_mode(t, app), STR8_LIT("Compilation:running")), "compile app: mode '%S' while running",
                   test_compile_mode(t, app));
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "compile app: the build did not end");
        String8 got = test_compile_text(t, app);
        TEST_CHECK(t, got.len > want_head.len && mem_equal(got.data, want_head.data, want_head.len) && got.data[got.len - 1] == 's' &&
                          got.data[got.len - 2] == ' ', "compile app: *compilation* holds '%S'", got);
        TEST_CHECK(t, str8_equal(test_compile_mode(t, app), STR8_LIT("Compilation:exit [2]")), "compile app: mode '%S'", test_compile_mode(t, app));
        TEST_CHECK(t, test_app_echo_is(app, "Compilation exited abnormally with code 2: 2 errors, 1 warning"), "compile app: echo '%S'", test_echo(app));
        Results *r = results_of(app, cb);
        TEST_CHECK(t, r && r->count == 4 && r->locs[0].row == 4 && r->locs[1].kind == LOCATION_INFO && r->locs[3].row == 7 && !r->mark_count,
                   "compile app: %D locations", r ? r->count : -1);
        TEST_CHECK(t, cb->read_only && !cb->modified && cb->keymap == BUFFER_KEYMAP_COMPILATION, "compile app: the buffer's flags");
        // next-error from a.c: here, the whole line highlighted until the next key.
        app_dev_feed(app, "M-g n", &t->arena);
        TEST_CHECK(t, str8_equal(test_results_where(t, app), STR8_LIT("a.c:13")), "compile app: next-error to '%S'", test_results_where(t, app));
        TEST_CHECK(t, app->landing_buffer == ab && app->landing_start == 9 && app->landing_end == 19, "compile app: landing %D-%D",
                   app->landing_start, app->landing_end);
        app_dev_feed(app, "M-g n", &t->arena); // the note is skipped
        TEST_CHECK(t, str8_equal(test_results_where(t, app), STR8_LIT("b.h:4")), "compile app: next-error skips the note: '%S'",
                   test_results_where(t, app));
        TEST_CHECK(t, app->landing_buffer == test_current(app) && app->landing_start == 4 && app->landing_end == 12, "compile app: landing on b.h");
        app_dev_feed(app, "C-f", &t->arena);
        TEST_CHECK(t, !app->landing_buffer, "compile app: the landing highlight outlived a key");
        app_dev_feed(app, "M-g n", &t->arena);
        TEST_CHECK(t, str8_equal(test_results_where(t, app), STR8_LIT("a.c:31")), "compile app: next-error to '%S'", test_results_where(t, app));
        app_dev_feed(app, "M-g n", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "No more matches"), "compile app: past the last: '%S'", test_echo(app));
        app_dev_feed(app, "M-g p", &t->arena);
        TEST_CHECK(t, str8_equal(test_results_where(t, app), STR8_LIT("b.h:4")), "compile app: previous-error skips nothing back to '%S'",
                   test_results_where(t, app));
        // In *compilation*: RET on the note visits it; RET on a plain line; n and p skip the note.
        app_dev_feed(app, "C-x o", &t->arena);
        if (test_current(app) != cb) app_dev_feed(app, "C-x o", &t->arena);
        app_dev_feed(app, "M-< C-n C-n C-n", &t->arena);
        app_dev_feed(app, "RET", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "No error on this line"), "compile app: RET on 'Building...': '%S'", test_echo(app));
        app_dev_feed(app, "n", &t->arena);
        TEST_CHECK(t, buffer_line_of(cb, test_app_point(app)) == 4, "compile app: n to row %D", buffer_line_of(cb, test_app_point(app)));
        app_dev_feed(app, "n", &t->arena);
        TEST_CHECK(t, buffer_line_of(cb, test_app_point(app)) == 6, "compile app: n skips the note: row %D", buffer_line_of(cb, test_app_point(app)));
        app_dev_feed(app, "p", &t->arena);
        TEST_CHECK(t, buffer_line_of(cb, test_app_point(app)) == 4, "compile app: p skips the note: row %D", buffer_line_of(cb, test_app_point(app)));
        app_dev_feed(app, "C-n RET", &t->arena);
        TEST_CHECK(t, test_current_is(app, "note.h") && test_app_point(app) == 0, "compile app: RET on the note: '%S'", test_results_where(t, app));
        // g in *compilation*: the same build again, without the prompt; M-x recompile from elsewhere too.
        app_dev_feed(app, "C-x b * c o m p i l a t i o n * RET g", &t->arena);
        TEST_CHECK(t, !app_dev_prompt(app).len && results_compile_running(app), "compile app: g did not start the build");
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "compile app: g's build did not end");
        got = test_compile_text(t, app);
        TEST_CHECK(t, got.len > want_head.len && mem_equal(got.data, want_head.data, want_head.len), "compile app: g gave '%S'", got);
        app_dev_feed(app, "C-x b a . c RET M-x r e c o m p i l e RET", &t->arena);
        TEST_CHECK(t, results_compile_running(app) && test_current(app) == ab, "compile app: M-x recompile");
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "compile app: recompile did not end");
        // The prompt now offers the command last used here (over .ronin-project's): an edited one.
        app_dev_feed(app, "C-x p c C-e SPC e x t r a RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "compile app: the edited command did not end");
        app_dev_feed(app, "C-x p c", &t->arena);
        TEST_CHECK(t, str8_equal(test_mini_input(t, app), str8_fmt(&t->arena, "%S extra", cmd)), "compile app: the prompt offers '%S'",
                   test_mini_input(t, app));
        app_dev_feed(app, "C-a C-k RET", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "No compile command"), "compile app: an empty command: '%S'", test_echo(app));
        // [keys compilation]: the defaults, describe-key and q in *compilation*.
        static const char *const keys_want[] = { "RET compile-goto-error", "C-m compile-goto-error", "n next-error-no-select",
                                                 "p previous-error-no-select", "g recompile", "q quit-window", "C-c C-k kill-compilation" };
        i32 nb = 0;
        for (;; nb++) {
            const char *command = NULL;
            String8 seq = app_dev_binding(app, &t->arena, 4, nb, &command);
            if (!seq.len) break;
            TEST_CHECK(t, nb < ARRAY_COUNT(keys_want) && str8_equal(str8_fmt(&t->arena, "%S %s", seq, command), str8_cstr(keys_want[nb])),
                       "compile app: [keys compilation] binding %d is '%S %s'", nb, seq, command);
        }
        TEST_CHECK(t, nb == ARRAY_COUNT(keys_want), "compile app: [keys compilation] has %d bindings", nb);
        app_dev_feed(app, "C-x b * c o m p i l a t i o n * RET C-h k q", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "q runs the command quit-window"), "compile app: describe-key q: '%S'", test_echo(app));
        i32 windows = app_dev_window_count(app);
        app_dev_feed(app, "q", &t->arena);
        TEST_CHECK(t, windows == 2 && app_dev_window_count(app) == 1, "compile app: q in *compilation*: %d windows, then %d", windows,
                   app_dev_window_count(app));
        app_dev_feed(app, "M-x k i l l - c o m p i l a t i o n RET", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "No compilation process"), "compile app: kill with none: '%S'", test_echo(app));
        if (!test_app_destroy(t, app, "compile app")) return 0;
    }

    // recompile before any build; compile in a buffer's directory.
    {
        App *app = test_manual_app(t, a_c, 0, 0, none);
        app_dev_feed(app, "M-x r e c o m p i l e RET", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "No previous compilation"), "compile app: recompile with none: '%S'", test_echo(app));
        app_dev_feed(app, "M-x c o m p i l e RET", &t->arena);
        TEST_CHECK(t, str8_equal(app_dev_prompt(app), STR8_LIT("Compile command: ")) && !test_mini_input(t, app).len,
                   "compile app: compile's prompt '%S' '%S'", app_dev_prompt(app), test_mini_input(t, app));
        app_dev_feed(app, "C-g", &t->arena);
        if (!test_app_destroy(t, app, "compile app")) return 0;
    }

    // compilation_scroll_output: the window follows until the first error, always, never.
    {
        static const struct { const char *conf; i32 want; } cases[] = {
            { "[settings]\ncompilation_save_buffers = none\ncompilation_scroll_output = first-error\n", 0 },
            { "[settings]\ncompilation_save_buffers = none\ncompilation_scroll_output = true\n", 1 },
            { "[settings]\ncompilation_save_buffers = none\ncompilation_scroll_output = false\n", 2 },
        };
        for (i32 c = 0; c < ARRAY_COUNT(cases); c++) {
            App *app = test_manual_app(t, a_c, 0, 0, test_results_conf(t, "p13scroll.conf", cases[c].conf));
            app_dev_feed(app, "C-x p c RET", &t->arena);
            TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "compile app: scroll %d did not end", c);
            Buffer *cb = buffer_list_find_name(&app->buffers, STR8_LIT("*compilation*"));
            i64 p = test_compile_window_point(app);
            i64 want = cases[c].want == 0 ? buffer_line_start(cb, 4) : cases[c].want == 1 ? buffer_size(cb) : 0;
            TEST_CHECK(t, p == want, "compile app: compilation_scroll_output case %d: point %D, expected %D", c, p, want);
            if (!test_app_destroy(t, app, "compile app")) return 0;
        }
    }

    // compilation_save_buffers: ask (y, n, q), all, none.
    {
        static const struct { const char *conf, *keys; b32 asked, saved; } cases[] = {
            { "[settings]\ncompilation_save_buffers = ask\n", "y", 1, 1 },
            { "[settings]\ncompilation_save_buffers = ask\n", "n", 1, 0 },
            { "[settings]\ncompilation_save_buffers = ask\n", "q", 1, 0 },
            { "[settings]\ncompilation_save_buffers = all\n", NULL, 0, 1 },
            { "[settings]\ncompilation_save_buffers = none\n", NULL, 0, 0 },
        };
        for (i32 c = 0; c < ARRAY_COUNT(cases); c++) {
            os_write_file(a_c, STR8_LIT("line one\nint y = x;\nline three\nint z\nline five\n"));
            App *app = test_manual_app(t, a_c, 0, 0, test_results_conf(t, "p13save.conf", cases[c].conf));
            Buffer *ab = test_current(app);
            app_dev_feed(app, "M-> x C-x p c RET", &t->arena);
            b32 asked = test_contains(app_dev_prompt(app), "Save file");
            if (asked && cases[c].keys) app_dev_feed(app, cases[c].keys, &t->arena);
            b32 started = results_compile_running(app) || buffer_list_find_name(&app->buffers, STR8_LIT("*compilation*"));
            TEST_CHECK(t, asked == cases[c].asked && ab->modified == !cases[c].saved && started, "compile app: save case %d: asked %d, modified %d, "
                       "started %d ('%S')", c, asked, ab->modified, started, app_dev_prompt(app));
            TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "compile app: save case %d did not end", c);
            if (ab->modified) app_dev_feed(app, "C-x C-s", &t->arena);
            if (!test_app_destroy(t, app, "compile app")) return 0;
        }
        os_write_file(a_c, STR8_LIT("line one\nint y = x;\nline three\nint z\nline five\n"));
    }

    // A second build while one runs (no, then yes); kill-compilation; killing the buffer; quitting (no,
    // then yes) and the shutdown ending the tree.
    String8 hang = str8_fmt(&t->arena, "%S\\p13\\hang.txt", t->tmp_dir);
    test_child(t, "hang", STR8_LIT("hang\n"));
    String8 tree = test_child(t, "app_tree", str8_fmt(&t->arena, "spawn %S\nout started\nhang\n", os_full_path(&t->arena, hang)));
    {
        App *app = test_manual_app(t, a_c, 0, 0, none);
        Buffer *ab = test_current(app);
        results_compile_remember(app, root, tree);
        app_dev_feed(app, "C-x p c RET", &t->arena);
        TEST_CHECK(t, test_compile_wait_text(t, app, "started", 10000), "compile app: the tree did not start");
        u32 g1 = test_grandchild_pid(test_compile_text(t, app));
        Results *r = results_compilation(app);
        CompileRun *first = r->run;
        app_dev_feed(app, "C-x p c RET", &t->arena);
        TEST_CHECK(t, str8_equal(app_dev_prompt(app), STR8_LIT("A compilation process is running; kill it? (yes or no) ")),
                   "compile app: the second build's question '%S'", app_dev_prompt(app));
        app_dev_feed(app, "n o RET", &t->arena);
        TEST_CHECK(t, r->run == first && results_compile_running(app), "compile app: 'no' stopped the build");
        app_dev_feed(app, "C-x p c RET y e s RET", &t->arena);
        TEST_CHECK(t, r->run && r->run != first && app->compile_dying_count == 1, "compile app: 'yes' did not start another build");
        TEST_CHECK(t, test_pid_gone(g1, 2000), "compile app: the first build's grandchild %u survived", g1);
        TEST_CHECK(t, test_compile_wait_text(t, app, "started", 10000), "compile app: the second build did not start");
        u32 g2 = test_grandchild_pid(test_compile_text(t, app));
        app_dev_feed(app, "C-x o", &t->arena);
        if (test_current(app) == ab) app_dev_feed(app, "C-x o", &t->arena);
        app_dev_feed(app, "C-c C-k", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 5000) && str8_equal(test_compile_mode(t, app), STR8_LIT("Compilation:killed")) &&
                          test_contains(test_compile_text(t, app), "\n\nCompilation killed") && test_app_echo_is(app, "Compilation killed"),
                   "compile app: C-c C-k: mode '%S', echo '%S'", test_compile_mode(t, app), test_echo(app));
        TEST_CHECK(t, test_pid_gone(g2, 2000), "compile app: the killed build's grandchild %u survived", g2);
        // Killing *compilation* while it runs kills the build.
        app_dev_feed(app, "C-x p c RET", &t->arena);
        TEST_CHECK(t, test_compile_wait_text(t, app, "started", 10000), "compile app: the third build did not start");
        u32 g3 = test_grandchild_pid(test_compile_text(t, app));
        files_kill_buffer(app, buffer_list_find_name(&app->buffers, STR8_LIT("*compilation*")));
        TEST_CHECK(t, !results_compile_running(app) && results_dev_wait(app, &t->arena, 5000) && test_pid_gone(g3, 2000),
                   "compile app: killing *compilation* left the build");
        // Quitting while a build runs: asked; no stays, yes quits and the shutdown ends the tree.
        app_dev_feed(app, "C-x p c RET", &t->arena);
        TEST_CHECK(t, test_compile_wait_text(t, app, "started", 10000), "compile app: the fourth build did not start");
        u32 g4 = test_grandchild_pid(test_compile_text(t, app));
        b32 alive = app_dev_feed(app, "C-x C-c", &t->arena);
        TEST_CHECK(t, alive && str8_equal(app_dev_prompt(app), STR8_LIT("A compilation is running; kill it and exit? (yes or no) ")),
                   "compile app: the exit question '%S'", app_dev_prompt(app));
        alive = app_dev_feed(app, "n o RET", &t->arena);
        TEST_CHECK(t, alive && results_compile_running(app), "compile app: 'no' quit or stopped the build");
        alive = app_dev_feed(app, "C-x C-c y e s RET", &t->arena);
        TEST_CHECK(t, !alive, "compile app: 'yes' did not quit");
        if (!test_app_destroy(t, app, "compile app")) return 0;
        TEST_CHECK(t, test_pid_gone(g4, 2000), "compile app: the shutdown left the grandchild %u", g4);
    }

    // A link-only failure: errors without a location, colored, counted, not visited.
    {
        String8 link = test_child(t, "app_link", STR8_LIT("out ronin.obj : error LNK2019: unresolved external symbol foo referenced in function main\n"
                                                         "out LINK : fatal error LNK1120: 1 unresolved externals\n"
                                                         "out Build FAILED: 2 Error(s), 0 Warning(s)\n"
                                                         "exit 1120\n"));
        App *app = test_manual_app(t, a_c, 0, 0, none);
        results_compile_remember(app, root, link);
        app_dev_feed(app, "C-x p c RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "compile app: the link build did not end");
        String8 want = str8_fmt(&t->arena, "Command: %S\nDirectory: %S\n\n"
                                           "ronin.obj : error LNK2019: unresolved external symbol foo referenced in function main\n"
                                           "LINK : fatal error LNK1120: 1 unresolved externals\n"
                                           "Build FAILED: 2 Error(s), 0 Warning(s)\n\n"
                                           "Compilation exited abnormally with code 1120 in ", link, shown);
        String8 got = test_compile_text(t, app);
        Results *r = results_compilation(app);
        TEST_CHECK(t, got.len > want.len && mem_equal(got.data, want.data, want.len), "compile app: the link text '%S'", got);
        TEST_CHECK(t, r->count == 0 && r->mark_count == 4 && r->marks[0].row == 3 && r->marks[0].off == 12 && r->marks[0].len == 5 &&
                          r->marks[1].off == 7 && r->marks[1].len == 11 && r->marks[1].kind == LOCATION_ERROR,
                   "compile app: the link marks (%D locations, %D marks)", r->count, r->mark_count);
        // The summary: both count words colored, neither counted (the echo below says 2 errors, 0 warnings).
        TEST_CHECK(t, r->mark_count == 4 && r->marks[2].row == 5 && r->marks[2].off == 16 && r->marks[2].len == 8 &&
                          r->marks[2].kind == LOCATION_ERROR && r->marks[3].row == 5 && r->marks[3].off == 28 && r->marks[3].len == 10 &&
                          r->marks[3].kind == LOCATION_WARNING,
                   "compile app: the summary's marks");
        TEST_CHECK(t, test_app_echo_is(app, "Compilation exited abnormally with code 1120: 2 errors, 0 warnings"), "compile app: the link echo '%S'",
                   test_echo(app));
        app_dev_feed(app, "M-g n", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "No more matches"), "compile app: next-error over the link errors: '%S'", test_echo(app));
        app_dev_feed(app, "C-x o", &t->arena);
        if (!test_current_is(app, "*compilation*")) app_dev_feed(app, "C-x o", &t->arena);
        app_dev_feed(app, "M-< C-n C-n C-n RET", &t->arena);
        TEST_CHECK(t, test_app_echo_is(app, "No error on this line"), "compile app: RET on a link error: '%S'", test_echo(app));
        if (!test_app_destroy(t, app, "compile app")) return 0;
    }

    // compilation_relative_paths: an absolute path under the root is shown relative to it (the location keeps the
    // full path, the offsets follow); one outside the root and a relative one stay as printed; false shows all as printed.
    {
        String8 outside = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\p13out.c", t->tmp_dir));
        os_write_file(outside, STR8_LIT("one\ntwo\n"));
        String8 root_fwd = str8_copy(&t->arena, root), out_fwd = str8_copy(&t->arena, outside); // the child unescapes backslashes
        for (i64 i = 0; i < root_fwd.len; i++) if (root_fwd.data[i] == '\\') root_fwd.data[i] = '/';
        for (i64 i = 0; i < out_fwd.len; i++) if (out_fwd.data[i] == '\\') out_fwd.data[i] = '/';
        String8 abs = test_child(t, "app_abs", str8_fmt(&t->arena, "out %S/a.c(2,5): error C1: under the root\n"
                                                                    "out %S(1): warning C2: outside\n"
                                                                    "out b.h:3:1: warning: relative\n"
                                                                    "exit 1\n", root_fwd, out_fwd));
        String8 rel_conf = test_results_conf(t, "p13rel.conf", "[settings]\ncompilation_save_buffers = none\n");
        String8 off_conf = test_results_conf(t, "p13reloff.conf", "[settings]\ncompilation_save_buffers = none\ncompilation_relative_paths = false\n");
        for (i32 on = 1; on >= 0; on--) {
            App *app = test_manual_app(t, a_c, 0, 0, on ? rel_conf : off_conf);
            results_compile_remember(app, root, abs);
            app_dev_feed(app, "C-x p c RET", &t->arena);
            TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "compile app: the absolute build did not end");
            Results *r = results_compilation(app);
            String8 got = test_compile_text(t, app);
            String8 first = on ? STR8_LIT("a.c") : str8_fmt(&t->arena, "%S/a.c", root_fwd);
            String8 want = str8_fmt(&t->arena, "Command: %S\nDirectory: %S\n\n%S(2,5): error C1: under the root\n%S(1): warning C2: outside\n"
                                               "b.h:3:1: warning: relative\n\n", abs, shown, first, out_fwd);
            TEST_CHECK(t, got.len > want.len && mem_equal(got.data, want.data, want.len), "compile app: relative paths %d: '%S'", on, got);
            TEST_CHECK(t, r && r->count == 3 && r->locs[0].path_off == 0 && r->locs[0].path_len == first.len &&
                              r->locs[0].num_off == first.len && r->locs[0].mark_off == first.len + 7,
                       "compile app: relative paths %d: the offsets", on);
            if (r && r->count == 3) {
                String8 f0 = r->files[r->locs[0].file], f1 = r->files[r->locs[1].file];
                TEST_CHECK(t, str8_equal(f0, a_c) && str8_equal(f1, outside), "compile app: relative paths %d: files '%S' '%S'",
                           on, f0, f1);
            }
            app_dev_feed(app, "M-g n", &t->arena);
            TEST_CHECK(t, str8_equal(test_results_where(t, app), STR8_LIT("a.c:13")), "compile app: relative paths %d: next-error to '%S'", on,
                       test_results_where(t, app));
            if (!test_app_destroy(t, app, "compile app")) return 0;
        }
    }

    // Outside changes after a build: an unmodified buffer whose file it rewrote is reverted at once, a
    // modified one flagged; a killed build checks too. No activation event.
    {
        String8 gen = str8_fmt(&t->arena, "%S\\gen.h", root), mod = str8_fmt(&t->arena, "%S\\mod.h", root);
        os_write_file(gen, STR8_LIT("generated 1\n"));
        os_write_file(mod, STR8_LIT("mod 1\n"));
        String8 gen_fwd = str8_copy(&t->arena, gen), mod_fwd = str8_copy(&t->arena, mod);
        for (i64 i = 0; i < gen_fwd.len; i++) if (gen_fwd.data[i] == 92) gen_fwd.data[i] = '/';
        for (i64 i = 0; i < mod_fwd.len; i++) if (mod_fwd.data[i] == 92) mod_fwd.data[i] = '/';
        String8 writes = test_child(t, "app_write", str8_fmt(&t->arena, "sleep 30\nwrite %S generated 2\\n\nwrite %S mod on disk\\n\nexit 0\n", gen_fwd, mod_fwd));
        App *app = test_manual_app(t, gen, 0, 0, none);
        Buffer *gb = test_current(app);
        app_dev_visit(app, mod);
        Buffer *mb = test_current(app);
        app_dev_feed(app, "x", &t->arena);
        app_dev_feed(app, "C-x b g e n . h RET", &t->arena);
        results_compile_remember(app, root, writes);
        app_dev_feed(app, "C-x p c RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "compile app: the writing build did not end");
        String8 gtext = buffer_text(gb, &t->arena, 0, buffer_size(gb));
        TEST_CHECK(t, str8_equal(gtext, STR8_LIT("generated 2\n")) && !gb->modified, "compile app: gen.h after the build: '%S'", gtext);
        TEST_CHECK(t, mb->modified && mb->disk_state == BUFFER_DISK_CHANGED, "compile app: mod.h not flagged (state %d)", (i32)mb->disk_state);
        String8 killed = test_child(t, "app_write2", str8_fmt(&t->arena, "write %S generated 3\\n\nout written\nhang\n", gen_fwd));
        results_compile_remember(app, root, killed);
        app_dev_feed(app, "C-x p c RET", &t->arena);
        TEST_CHECK(t, test_compile_wait_text(t, app, "written", 10000), "compile app: the second writing build did not write");
        app_dev_feed(app, "M-x k i l l - c o m p i l a t i o n RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 5000), "compile app: the killed writing build did not end");
        gtext = buffer_text(gb, &t->arena, 0, buffer_size(gb));
        TEST_CHECK(t, str8_equal(gtext, STR8_LIT("generated 3\n")), "compile app: gen.h after the killed build: '%S'", gtext);
        app_dev_feed(app, "C-x b m o d . h RET M-x r e v e r t - b u f f e r RET y e s RET", &t->arena);
        if (!test_app_destroy(t, app, "compile app")) return 0;
    }

    // .ronin-project: [build] directory, a diagnostic, the shutdown with a build running (no question).
    {
        String8 cwd = test_child(t, "app_cwd", STR8_LIT("cwd\n"));
        os_write_file(str8_fmt(&t->arena, "%S\\.ronin-project", root), str8_fmt(&t->arena, "[build]\ncommand = %S\ndirectory = sub\nbogus = 1\n", cwd));
        App *app = test_manual_app(t, a_c, 0, 0, none);
        app_dev_feed(app, "C-x p c", &t->arena);
        String8 log = buffer_text(app->messages, &t->arena, 0, buffer_size(app->messages));
        TEST_CHECK(t, test_contains(log, ".ronin-project:4: unknown name 'bogus' in [build]") && str8_equal(test_mini_input(t, app), cwd),
                   "compile app: [build] diagnostics in *Messages*, command '%S'", test_mini_input(t, app));
        app_dev_feed(app, "RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "compile app: the cwd build did not end");
        String8 text = test_compile_text(t, app);
        String8 want_dir = str8_fmt(&t->arena, "\n%S\\sub\n", root);
        b32 found = 0;
        for (i64 i = 0; i + want_dir.len <= text.len && !found; i++) found = mem_equal(text.data + i, want_dir.data, want_dir.len);
        TEST_CHECK(t, found, "compile app: [build] directory: '%S'", text);
        // A [build] directory that does not exist: the build cannot start, and says so.
        os_write_file(str8_fmt(&t->arena, "%S\\.ronin-project", root), str8_fmt(&t->arena, "[build]\ncommand = %S\ndirectory = no_such_dir\n", cwd));
        app_dev_feed(app, "C-x p c RET", &t->arena);
        TEST_CHECK(t, results_dev_wait(app, &t->arena, 10000), "compile app: the failed start did not end");
        TEST_CHECK(t, test_contains(test_compile_text(t, app), "\n\nCompilation could not start: ") &&
                          str8_equal(test_compile_mode(t, app), STR8_LIT("Compilation:failed")) && test_contains(test_echo(app), "Compilation could not start"),
                   "compile app: a missing [build] directory: mode '%S', echo '%S'", test_compile_mode(t, app), test_echo(app));
        os_write_file(str8_fmt(&t->arena, "%S\\.ronin-project", root), str8_fmt(&t->arena, "[build]\ncommand = %S\n", cmd));
        results_compile_remember(app, root, tree);
        app_dev_feed(app, "C-x p c RET", &t->arena);
        TEST_CHECK(t, test_compile_wait_text(t, app, "started", 10000), "compile app: the last tree did not start");
        u32 g = test_grandchild_pid(test_compile_text(t, app));
        if (!test_app_destroy(t, app, "compile app")) return 0;
        TEST_CHECK(t, test_pid_gone(g, 2000), "compile app: the shutdown left the grandchild %u", g);
        os_write_file(str8_fmt(&t->arena, "%S\\.ronin-project", root), str8_fmt(&t->arena, "[build]\ncommand = %S\n", cmd));
    }
    TEST_CHECK(t, os_dev_live_processes() == procs0 && os_dev_live_threads() == threads0 && os_dev_live_events() == events0,
               "compile app: counters back (%d processes, %d threads, %d events)", os_dev_live_processes(), os_dev_live_threads(),
               os_dev_live_events());
    LOG("test: ok: builds in the app (the prompt with .ronin-project's command, nothing run before RET, *compilation* beside and not "
        "selected, its exact text, the mode line's state, the summary, next-error with the landing highlight and a skipped note, RET, n, "
        "p, g, M-x recompile, the command last used here offered, an empty command, kill with none, recompile with none, compile's "
        "prompt; compilation_scroll_output first-error / true / false; compilation_save_buffers ask (y, n, q) / all / none; a second "
        "build while one runs (no, yes), C-c C-k, killing the buffer, quitting (no, yes), every tree gone; a link-only failure with a "
        "summary colored and not counted; compilation_relative_paths on and off (under the root, outside it, relative); "
        "outside changes after a build and after a kill; [build] directory and diagnostics, a [build] directory that does not exist (failed); "
        "[keys compilation]'s defaults, describe-key and q; the shutdown with a build running)");
    return 1;
}

// ---------------------------------------------------------------------------
// Phase 15: crash recovery

// `text` in --keys notation, one character at a time (SPC for a space).
static String8 test_spell(Test *t, const char *text) {
    String8 s = str8_cstr(text);
    u8 *out = PUSH_ARRAY(&t->arena, u8, s.len * 4 + 1);
    i64 n = 0;
    for (i64 i = 0; i < s.len; i++) {
        if (n) out[n++] = ' ';
        if (s.data[i] == ' ') {
            memcpy(out + n, "SPC", 3);
            n += 3;
        } else {
            out[n++] = s.data[i];
        }
    }
    return str8(out, n);
}

// Keys (--keys notation) one event a frame, as app_dev_feed, from a String8.
static void test_feed8(Test *t, App *app, String8 keys) {
    Event events[256];
    i32 n = app_dev_key_events(app, keys, events, ARRAY_COUNT(events));
    for (i32 i = 0; i < n; i++) app_dev_feed_events(app, &events[i], 1, &t->arena);
}

// Every file in `dir` deleted, then the directory.
static void test_clear_dir(Test *t, String8 dir) {
    OsDirEntry *entries;
    i64 count;
    if (os_list_dir(&t->arena, dir, &entries, &count) == OS_FILE_OK) {
        for (i64 i = 0; i < count; i++) {
            if (!entries[i].is_dir) os_file_delete(str8_fmt(&t->arena, "%S\\%S", dir, entries[i].name));
        }
    }
    os_dev_remove_dir(dir);
}

static b32 test_has(String8 text, String8 part) {
    for (i64 i = 0; i + part.len <= text.len; i++) {
        if (mem_equal(text.data + i, part.data, part.len)) return 1;
    }
    return 0;
}

// A child (this executable) opens six files, edits five of them with --keys (CRLF UTF-8, UTF-16 LE with CRLF and a
// character above U+FFFF, UTF-16 BE, UTF-8 with a BOM, mixed endings; the last edit of each leaves the gap inside the
// text, after the astral character in the UTF-16 ones) and crashes on purpose (--crash). Each recovery file must hold
// exactly what saving the same edited buffer writes (a headless app given the same keys saves it), its sidecar the
// path, the encoding and the line endings; the unmodified file gets none. Then a headless app with that directory:
// the echo area's notice, *Messages*, recover-file's two windows, nothing overwritten.
static b32 test_crash_recovery(Test *t) {
    String8 src = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\crash", t->tmp_dir));
    String8 rec = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\crash_rec", t->tmp_dir));
    String8 ref_dir = os_full_path(&t->arena, str8_fmt(&t->arena, "%S\\crash_ref", t->tmp_dir));
    test_clear_dir(t, src);
    test_clear_dir(t, rec);
    test_clear_dir(t, ref_dir);
    os_make_dir(src);
    os_make_dir(ref_dir);
    enum { FILES = 6, EDITED = 5 };
    static const char *names[FILES] = { "crlf.txt", "u16le.txt", "u16be.txt", "bom8.txt", "mixed.txt", "same.txt" };
    static const char *edits[FILES] = { "M-> X M-< C-e Z", "C-e Q M-< C-f C-f C-f C-f C-f C-f C-f Y", "C-n W M-< C-e V", "C-e V",
                                        "C-n C-e U", "" };
    String8 contents[FILES] = {
        STR8_LIT("line one\r\nline two\r\n"),
        test_grep_utf16(t, STR8_LIT("hello \xF0\x9F\x98\x80 world\r\nsecond\r\n"), 0),
        test_grep_utf16(t, STR8_LIT("alpha \xF0\x9F\x98\x80\nbeta\n"), 1),
        STR8_LIT("\xEF\xBB\xBFx\ny\n"),
        STR8_LIT("a\r\nb\nc\r\n"),
        STR8_LIT("untouched\n"),
    };
    String8 paths[FILES];
    String8 files_arg = str8(NULL, 0), keys = str8(NULL, 0);
    for (i32 i = 0; i < FILES; i++) {
        paths[i] = str8_fmt(&t->arena, "%S\\%s", src, names[i]);
        TEST_CHECK(t, os_write_file(paths[i], contents[i]), "crash: cannot write %S", paths[i]);
        files_arg = str8_fmt(&t->arena, "%S \"%S\"", files_arg, paths[i]);
        if (edits[i][0]) keys = str8_fmt(&t->arena, "%S%sC-x b %S RET %s", keys, keys.len ? " " : "", test_spell(t, names[i]), edits[i]);
    }
    String8 conf = str8_fmt(&t->arena, "%S\\crash.conf", src);
    TEST_CHECK(t, os_write_file(conf, STR8_LIT("# defaults\n")), "crash: cannot write the config");

    // The reference: the same keys in a headless app, each edited buffer saved.
    String8 expected[EDITED], sidecar[EDITED], text_crlf = str8(NULL, 0);
    App *ref = test_app_create(t);
    TEST_CHECK(t, ref, "crash: reference app_create failed");
    for (i32 i = 0; i < FILES; i++) TEST_CHECK(t, app_dev_visit(ref, paths[i]), "crash: reference cannot visit %S", paths[i]);
    test_feed8(t, ref, keys);
    for (i32 i = 0; i < FILES; i++) {
        Buffer *b = buffer_list_find_name(&ref->buffers, str8_cstr(names[i]));
        TEST_CHECK(t, b && b->modified == (i < EDITED), "crash: reference buffer %s (modified %d)", names[i], b ? b->modified : -1);
        if (i >= EDITED) continue;
        if (i == 0) text_crlf = buffer_text(b, &t->arena, 0, buffer_size(b));
        sidecar[i] = str8_fmt(&t->arena, "%S\nencoding: %s\nline endings: %s\n", b->path, app_encoding_name(b->encoding), app_eol_name(b->eol));
        String8 out = str8_fmt(&t->arena, "%S\\%s", ref_dir, names[i]);
        TEST_CHECK(t, buffer_save_as(b, out) == OS_FILE_OK, "crash: reference save of %s", names[i]);
        TEST_CHECK(t, os_read_small_file(&t->arena, out, MB(1), &expected[i]) == OS_FILE_OK, "crash: reference read of %s", names[i]);
    }
    if (!test_app_destroy(t, ref, "crash reference")) return 0;
    TEST_CHECK(t, test_has(sidecar[1], STR8_LIT("UTF-16LE")) && test_has(sidecar[1], STR8_LIT("CRLF")) &&
                  test_has(sidecar[2], STR8_LIT("UTF-16BE")) && expected[1].len > 2 && expected[1].data[0] == 0xFF,
               "crash: the reference's encodings ('%S', '%S')", sidecar[1], sidecar[2]);

    // The child: edits, then an access violation; the filter writes the recovery files.
    String8 cmd = str8_fmt(&t->arena, "\"%S\" --config \"%S\" --recovery-dir \"%S\"%S --keys \"%S\" --crash", os_dev_exe_path(&t->arena), conf,
                           rec, files_arg, keys);
    TestProcRun run = test_proc_run(t, cmd, src, NULL);
    TEST_CHECK(t, run.status == OS_FILE_OK && run.code == 0xC0000005u, "crash: the child's exit code 0x%x (status %d)", run.code, run.status);
    OsDirEntry *entries;
    i64 count = 0;
    TEST_CHECK(t, os_list_dir(&t->arena, rec, &entries, &count) == OS_FILE_OK && count == 2 * EDITED,
               "crash: %D files in the recovery directory, %d expected (each edited file and its sidecar)", count, 2 * EDITED);
    String8 crlf_entry = str8(NULL, 0);
    for (i32 i = 0; i < EDITED; i++) {
        u32 hash = 2166136261u;
        String8 path = paths[i];
        for (i64 k = 0; k < path.len; k++) hash = (hash ^ path.data[k]) * 16777619u;
        String8 prefix = str8_fmt(&t->arena, "%s %08x ", names[i], hash);
        String8 suffix = STR8_LIT(".recovered");
        String8 found = str8(NULL, 0);
        for (i64 e = 0; e < count; e++) {
            String8 n = entries[e].name;
            if (n.len == prefix.len + 15 + suffix.len && mem_equal(n.data, prefix.data, prefix.len) &&
                mem_equal(n.data + n.len - suffix.len, suffix.data, suffix.len)) {
                found = n;
            }
        }
        TEST_CHECK(t, found.len, "crash: no recovery file '%S<YYYYMMDD-HHMMSS>.recovered'", prefix);
        if (i == 0) crlf_entry = found;
        String8 got, side;
        TEST_CHECK(t, os_read_small_file(&t->arena, str8_fmt(&t->arena, "%S\\%S", rec, found), MB(1), &got) == OS_FILE_OK &&
                      str8_equal(got, expected[i]),
                   "crash: %s: the recovery file's %D bytes differ from the save's %D", names[i], got.len, expected[i].len);
        TEST_CHECK(t, os_read_small_file(&t->arena, str8_fmt(&t->arena, "%S\\%S.path", rec, found), KB(64), &side) == OS_FILE_OK &&
                      str8_equal(side, sidecar[i]),
                   "crash: %s: the sidecar says '%S', not '%S'", names[i], side, sidecar[i]);
    }

    // The next start: the notice, *Messages*, recover-file.
    String8 old_dir = str8_copy(&t->arena, os_recovery_dir());
    os_dev_set_recovery_dir(rec);
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "crash: app_create failed");
    String8 notice = str8_fmt(&t->arena, "%d recovery files from a crash in %S (listed in *Messages*): M-x recover-file", EDITED, rec);
    TEST_CHECK(t, str8_equal(test_echo(app), notice), "crash: the echo area says '%S'", test_echo(app));
    Buffer *messages = buffer_list_find_name(&app->buffers, STR8_LIT("*Messages*"));
    String8 log = messages ? buffer_text(messages, &t->arena, 0, buffer_size(messages)) : str8(NULL, 0);
    TEST_CHECK(t, test_has(log, str8_fmt(&t->arena, "Recovery file %S (of %S)", crlf_entry, paths[0])), "crash: *Messages* lacks the crlf.txt line");
    test_feed8(t, app, str8_fmt(&t->arena, "M-x %S RET c r l f RET", test_spell(t, "recover-file")));
    View *here = test_win(app, 0), *other = test_win(app, 1);
    TEST_CHECK(t, here && other && here == app_selected_view(app), "crash: recover-file did not give two windows, the first selected");
    TEST_CHECK(t, str8_equal(here->buffer->path, paths[0]) && str8_equal(test_app_text(t, app), STR8_LIT("line one\nline two\n")),
               "crash: the selected window shows '%S'", here->buffer->path);
    String8 shown = buffer_text(other->buffer, &t->arena, 0, buffer_size(other->buffer));
    TEST_CHECK(t, str8_equal(other->buffer->path, str8_fmt(&t->arena, "%S\\%S", rec, crlf_entry)) && str8_equal(shown, text_crlf),
               "crash: the other window shows '%S': '%S'", other->buffer->path, shown);
    TEST_CHECK(t, test_echo_has(app, "The recovered text is in the other window; crlf.txt itself is unchanged"), "crash: recover-file says '%S'",
               test_echo(app));
    if (!test_app_destroy(t, app, "crash")) return 0;
    String8 now;
    TEST_CHECK(t, os_read_small_file(&t->arena, paths[0], KB(64), &now) == OS_FILE_OK && str8_equal(now, contents[0]), "crash: crlf.txt changed");
    TEST_CHECK(t, os_list_dir(&t->arena, rec, &entries, &count) == OS_FILE_OK && count == 2 * EDITED, "crash: recovery files went (%D)", count);

    // Without recovery files: no notice.
    test_clear_dir(t, rec);
    app = test_app_create(t);
    TEST_CHECK(t, app && !test_has(test_echo(app), STR8_LIT("recovery")), "crash: a notice without recovery files: '%S'", test_echo(app));
    if (!test_app_destroy(t, app, "crash, none")) return 0;
    os_dev_set_recovery_dir(old_dir);
    test_clear_dir(t, src);
    test_clear_dir(t, ref_dir);
    LOG("test: ok: crash recovery (a child crashing after edits: CRLF UTF-8, UTF-16 LE with CRLF and an astral character, UTF-16 BE, "
        "a UTF-8 BOM and mixed endings each written as a save writes it, the sidecars, none for an unmodified file; the notice, "
        "*Messages*, recover-file's two windows, nothing overwritten; no notice without them)");
    return 1;
}

// The caps of the bounded caches --leak-check's prime step fills (Phase 16): *Messages* keeps ECHO_LOG_LINES lines of
// at most ECHO_CAP bytes, a history kind MINI_HISTORY_MAX entries in an arena compacted below max(64 KB, 2 x live) (plus
// the newest entry), the kill ring kill_ring_max entries; and their commit stops growing once the caps are reached.
static b32 test_bounded_caps(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "bounded caps: app_create failed");
    u8 line[ECHO_CAP + 100];
    memset(line, 'm', sizeof(line));
    i64 messages_commit = 0;
    for (i32 i = 0; i < 5 * ECHO_LOG_LINES; i++) {
        echo_log(&app->echo, str8(line, sizeof(line))); // longer than the cap: cut to it
        if (i == 2 * ECHO_LOG_LINES) messages_commit = app->messages->text_cap;
    }
    i64 lines = buffer_line_count(app->messages) - 1, size = buffer_size(app->messages);
    TEST_CHECK(t, lines == ECHO_LOG_LINES && size == (i64)ECHO_LOG_LINES * (ECHO_CAP + 1) && app->messages->text_cap == messages_commit,
               "bounded caps: *Messages* %D lines, %D bytes, text commit %D (%D after 2,000)", lines, size, app->messages->text_cap, messages_commit);
    Minibuffer *mb = &app->mini;
    u64 history_commit = 0;
    for (i32 i = 0; i < 10000; i++) {
        u8 text[64];
        memset(text, i & 1 ? 'a' : 'b', sizeof(text));
        fmt_buf(text, sizeof(text), "%d", i % 300); // 300 different entries in turn
        minibuffer_history_add(mb, MINI_HISTORY_TEXT, str8(text, sizeof(text)));
        TEST_CHECK(t, mb->histories[MINI_HISTORY_TEXT].count <= MINI_HISTORY_MAX &&
                      mb->history_arena.pos <= MAX(KB(64), 2 * mb->history_live) + sizeof(text),
                   "bounded caps: history at %d: %d entries, %U bytes for %U live", i, mb->histories[MINI_HISTORY_TEXT].count, mb->history_arena.pos,
                   mb->history_live);
        if (i == 5000) history_commit = mb->history_arena.committed;
    }
    TEST_CHECK(t, mb->history_arena.committed == history_commit, "bounded caps: the history arena's commit grew from %U to %U", history_commit,
               mb->history_arena.committed);
    i32 max = app->config->settings.kill_ring_max;
    for (i32 i = 0; i < 10 * max; i++) {
        u8 *d = kill_push(&app->kills, 64);
        TEST_CHECK(t, d && app->kills.count <= max, "bounded caps: the kill ring holds %d entries (max %d)", app->kills.count, max);
        memset(d, 'k', 64);
    }
    if (!test_app_destroy(t, app, "bounded caps")) return 0;
    LOG("test: ok: bounded caps (*Messages* %d lines of at most %d bytes; history %d entries, arena under max(64 KB, 2 x live); kill ring %d)",
        ECHO_LOG_LINES, ECHO_CAP, MINI_HISTORY_MAX, max);
    return 1;
}

// The leak check's sampler (Phase 16): sane values, the handle ledger following an open and a close, and every
// handle the tests opened closed again by their end.
static b32 test_leak_metrics(Test *t, i32 ledger0) {
    os_dev_thread_pool_end(); // the parked worker threads' handles
    OsDevMetrics m;
    os_dev_metrics(&m);
    TEST_CHECK(t, m.private_bytes && m.working_set && m.handles > 0 && m.reservations > 0 && m.reserved_committed && m.process_threads >= 1 &&
                  m.live_handles >= 0, "leak metrics: private %U, working set %U, handles %d, reservations %d (%U bytes), threads %d, ledger %d",
               m.private_bytes, m.working_set, m.handles, m.reservations, m.reserved_committed, m.process_threads, m.live_handles);
    String8 path = str8_fmt(&t->arena, "%S\\p16ledger.txt", t->tmp_dir);
    TEST_CHECK(t, os_write_file(path, STR8_LIT("ledger\n")) && os_dev_live_handles() == m.live_handles, "leak metrics: a write left a handle");
    OsFile file;
    OsFileInfo info;
    TEST_CHECK(t, os_file_open_read(path, &file, &info) == OS_FILE_OK && os_dev_live_handles() == m.live_handles + 1,
               "leak metrics: an open file is not in the ledger (%d, %d before)", os_dev_live_handles(), m.live_handles);
    os_file_close(file);
    os_file_delete(path);
    TEST_CHECK(t, os_dev_live_handles() == m.live_handles, "leak metrics: a closed file is still in the ledger");
    TEST_CHECK(t, m.live_handles == ledger0, "leak metrics: the tests left %d handle(s) open (the ledger %d, %d at the start)",
               m.live_handles - ledger0, m.live_handles, ledger0);
    LOG("test: ok: leak metrics (private %U KB, %d handles, %d reservations of %U KB, %d threads; the ledger back at %d)",
        m.private_bytes / 1024, m.handles, m.reservations, m.reserved_committed / 1024, m.process_threads, ledger0);
    return 1;
}

// ---------------------------------------------------------------------------
// The modal layer (Phase 18), with a test owner: two panes, no field, its own keymap.

typedef struct TestModal {
    Keymap keys;
    i32 closed, accepts, accept_pane, selects, select_pane;
    i64 accept_row, select_row;
    const Command *run; // accept: modal_run_after_close with it
} TestModal;

static Keymap *test_modal_keys(App *app, void *data) {
    (void)app;
    return &((TestModal *)data)->keys;
}
static void test_modal_accept(App *app, void *data, i32 pane, i64 row) {
    TestModal *tm = data;
    tm->accepts++;
    tm->accept_pane = pane;
    tm->accept_row = row;
    if (tm->run) modal_run_after_close(app, tm->run);
}
static void test_modal_closed(App *app, void *data) {
    (void)app;
    ((TestModal *)data)->closed++;
}
static void test_modal_selected(App *app, void *data, i32 pane, i64 row) {
    (void)app;
    TestModal *tm = data;
    tm->selects++;
    tm->select_pane = pane;
    tm->select_row = row;
}
static String8 test_modal_status(App *app, void *data, Arena *scratch) {
    (void)app;
    (void)data;
    (void)scratch;
    return STR8_LIT("status");
}

static b32 test_modal_rect_is(Rect r, f32 x0, f32 y0, f32 x1, f32 y1) {
    return r.x0 == x0 && r.y0 == y0 && r.x1 == x1 && r.y1 == y1;
}

static void test_modal_click(Test *t, App *app, i32 x, i32 y, i32 clicks) {
    Event e[2] = { { .kind = EVENT_MOUSE_DOWN, .button = MOUSE_LEFT, .clicks = clicks, .x = x, .y = y },
                   { .kind = EVENT_MOUSE_UP, .button = MOUSE_LEFT, .x = x, .y = y } };
    app_dev_feed_events(app, e, 2, &t->arena);
}

// The box's geometry (default and other percentages, the minimum), its keys (movement in the focused pane,
// modal-other-pane wrapping, paging, refusals, a cancelled prefix, accept, C-g and ESC), the mouse (a row of
// either pane, a double click, the wheel, a click outside), opening refused (already open, the minibuffer, too
// small), a resize closing it, and modal_run_after_close.
static b32 test_modal_layer(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "modal: no app");
    TestModal *tm = PUSH_STRUCT(&t->arena, TestModal);
    tm->keys.name = "test-modal";
    test_bind(&tm->keys, "<down>", &CMD_MODAL_NEXT);
    test_bind(&tm->keys, "<up>", &CMD_MODAL_PREVIOUS);
    test_bind(&tm->keys, "C-v", &CMD_MODAL_NEXT_PAGE);
    test_bind(&tm->keys, "M-v", &CMD_MODAL_PREVIOUS_PAGE);
    test_bind(&tm->keys, "RET", &CMD_MODAL_ACCEPT);
    test_bind(&tm->keys, "TAB", &CMD_MODAL_OTHER_PANE);
    ModalSpec spec = { .name = STR8_LIT("test modal"), .keymap = test_modal_keys, .panes = 2, .data = tm,
                       .accept = test_modal_accept, .closed = test_modal_closed };
    View *v = app_selected_view(app);
    app_dev_show_scratch(app, STR8_LIT("one\ntwo\nthree\n"));
    view_set_point(v, view_primary(v), 4);
    Modal *m = &app->modal;

    // 1280 x 800, cells 8 x 16: 160 columns, a window area of 784 px (49 rows). 70% is 112 columns (896 px); 60% is
    // 29 rows, 464 px: 28 list rows inside the 1 px frame. Centred: x 192, y (784 - 450) / 2 = 167. The panes split
    // 894 px at 50%, 447 px down to whole cells: 440.
    TEST_CHECK(t, modal_open(app, &spec) && modal_active(app), "modal: did not open");
    modal_set_rows(app, 0, 100);
    modal_set_rows(app, 1, 5);
    app_dev_feed_events(app, NULL, 0, &t->arena);
    ModalGeometry *g = &m->g;
    TEST_CHECK(t, test_modal_rect_is(g->box, 192, 167, 1088, 617) && g->rows == 28 && g->border == 1 &&
                      test_modal_rect_is(g->panes[0], 193, 168, 633, 616) && test_modal_rect_is(g->panes[1], 634, 168, 1087, 616),
               "modal: geometry: box %f %f %f %f, %d rows, panes %f-%f, %f-%f", g->box.x0, g->box.y0, g->box.x1, g->box.y1, g->rows,
               g->panes[0].x0, g->panes[0].x1, g->panes[1].x0, g->panes[1].x1);
    TEST_CHECK(t, !modal_open(app, &spec) && test_app_echo_is(app, "The test modal is already open"), "modal: opened twice");

    // Keys: movement in the focused pane only; modal-other-pane wraps; pages; the ends.
    app_dev_feed(app, "<down> <down>", &t->arena);
    TEST_CHECK(t, m->lists[0].selected == 2 && m->lists[1].selected == 0 && modal_focus_pane(app) == 0, "modal: <down> twice");
    app_dev_feed(app, "TAB <down> <down> <down> <down> <down> <down>", &t->arena);
    TEST_CHECK(t, modal_focus_pane(app) == 1 && m->lists[1].selected == 4 && m->lists[0].selected == 2, "modal: the other pane (%D)",
               m->lists[1].selected);
    app_dev_feed(app, "TAB", &t->arena);
    TEST_CHECK(t, modal_focus_pane(app) == 0, "modal: modal-other-pane wraps to the first pane");
    app_dev_feed(app, "C-v", &t->arena);
    TEST_CHECK(t, m->lists[0].selected == 30 && m->lists[0].top == 3, "modal: C-v: %D, top %D", m->lists[0].selected, m->lists[0].top);
    app_dev_feed(app, "M-v", &t->arena);
    TEST_CHECK(t, m->lists[0].selected == 2 && m->lists[0].top == 2, "modal: M-v: %D, top %D", m->lists[0].selected, m->lists[0].top);
    app_dev_feed(app, "<up> <up> <up> <up>", &t->arena);
    TEST_CHECK(t, m->lists[0].selected == 0 && m->lists[0].top == 0, "modal: <up> stops at the first row");
    app_dev_feed(app, "C-v C-v C-v C-v C-v <down>", &t->arena);
    TEST_CHECK(t, m->lists[0].selected == 99 && m->lists[0].top == 72, "modal: the last row: %D, top %D", m->lists[0].selected, m->lists[0].top);
    // Refusals, a cancelled prefix, an undefined sequence: the box stays.
    app_dev_feed(app, "C-x C-s", &t->arena);
    TEST_CHECK(t, modal_active(app) && test_app_echo_is(app, "save-buffer is not available in the test modal"), "modal: C-x C-s: '%S'",
               test_echo(app));
    app_dev_feed(app, "a", &t->arena);
    TEST_CHECK(t, modal_active(app) && test_app_echo_is(app, "self-insert-command is not available in the test modal") &&
                      str8_equal(test_app_text(t, app), STR8_LIT("one\ntwo\nthree\n")), "modal: a without a field: '%S'", test_echo(app));
    app_dev_feed(app, "C-x C-g", &t->arena);
    TEST_CHECK(t, modal_active(app) && test_app_echo_is(app, "Quit"), "modal: C-x C-g cancels the prefix only");
    app_dev_feed(app, "C-c q", &t->arena);
    TEST_CHECK(t, modal_active(app) && test_app_echo_is(app, "C-c q is undefined"), "modal: C-c q: '%S'", test_echo(app));
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, tm->accepts == 1 && tm->accept_pane == 0 && tm->accept_row == 99 && modal_active(app), "modal: RET accepts");

    // The mouse: a row of the other pane focuses it; a double click accepts; the wheel scrolls the pane under it.
    test_modal_click(t, app, 640, 167 + 1 + 3 * 16 + 5, 1);
    TEST_CHECK(t, modal_focus_pane(app) == 1 && m->lists[1].selected == 3 && tm->accepts == 1, "modal: a click on pane 1's row 3");
    test_modal_click(t, app, 640, 167 + 1 + 6 * 16 + 5, 1); // below its 5 rows: nothing
    TEST_CHECK(t, modal_focus_pane(app) == 1 && m->lists[1].selected == 3, "modal: a click below the rows");
    test_modal_click(t, app, 300, 167 + 1 + 5 * 16 + 5, 2);
    TEST_CHECK(t, modal_focus_pane(app) == 0 && tm->accepts == 2 && tm->accept_pane == 0 && tm->accept_row == 72 + 5,
               "modal: a double click accepts (%d, row %D)", tm->accepts, tm->accept_row);
    Event wheel = { .kind = EVENT_MOUSE_WHEEL, .x = 300, .y = 300, .wheel = 120 };
    app_dev_feed_events(app, &wheel, 1, &t->arena);
    TEST_CHECK(t, m->lists[0].top == 69 && m->lists[0].selected == 77 && app_dev_window_top(app, 0) == 0, "modal: the wheel: top %D",
               m->lists[0].top);
    TEST_CHECK(t, app_mouse_cursor(app, 300, 300) == MOUSE_CURSOR_ARROW, "modal: the mouse cursor");
    // A click outside closes it as C-g does: closed() ran, nothing said, the window underneath untouched.
    test_modal_click(t, app, 20, 20, 1);
    TEST_CHECK(t, !modal_active(app) && tm->closed == 1 && test_app_echo_is(app, "") && view_point(v, view_primary(v)) == 4 &&
                      !view_region_active(v, view_primary(v), &app->config->settings) && !app->dragging,
               "modal: a click outside: active %d, closed %d, echo '%S', point %D", modal_active(app), tm->closed, test_echo(app),
               view_point(v, view_primary(v)));
    TEST_CHECK(t, arena_pos(&m->arena) == 0, "modal: the arena is reset when it closes");
    // C-g and ESC close it, without a message.
    TEST_CHECK(t, modal_open(app, &spec), "modal: reopen");
    app_dev_feed(app, "C-g", &t->arena);
    TEST_CHECK(t, !modal_active(app) && tm->closed == 2 && test_app_echo_is(app, ""), "modal: C-g: echo '%S'", test_echo(app));
    TEST_CHECK(t, modal_open(app, &spec), "modal: reopen");
    app_dev_feed(app, "ESC", &t->arena);
    TEST_CHECK(t, !modal_active(app) && tm->closed == 3 && test_app_echo_is(app, ""), "modal: ESC");
    app_dev_feed(app, "C-n", &t->arena);
    TEST_CHECK(t, view_point(v, view_primary(v)) == 8, "modal: keys reach the window again");

    // Other percentages: 90% of 160 columns, 80% of 49 rows (39: 38 list rows), the panes split at 30%.
    ModalSpec wide = spec;
    wide.width_pct = 90;
    wide.height_pct = 80;
    wide.pane_split_pct = 30;
    TEST_CHECK(t, modal_open(app, &wide), "modal: wide: did not open");
    modal_set_rows(app, 0, 100);
    app_dev_feed_events(app, NULL, 0, &t->arena);
    TEST_CHECK(t, test_modal_rect_is(g->box, 64, 87, 1216, 697) && g->rows == 38 && test_modal_rect_is(g->panes[0], 65, 88, 409, 696) &&
                      test_modal_rect_is(g->panes[1], 410, 88, 1215, 696),
               "modal: wide: box %f %f %f %f, %d rows, panes %f-%f, %f-%f", g->box.x0, g->box.y0, g->box.x1, g->box.y1, g->rows,
               g->panes[0].x0, g->panes[0].x1, g->panes[1].x0, g->panes[1].x1);
    // Fewer rows than fit: the box is as high as they need.
    modal_close(app);
    TEST_CHECK(t, modal_open(app, &spec), "modal: short: did not open");
    modal_set_rows(app, 0, 3);
    app_dev_feed_events(app, NULL, 0, &t->arena);
    TEST_CHECK(t, g->rows == 3 && g->box.y1 - g->box.y0 == 2 + 3 * 16, "modal: three rows: %d rows", g->rows);
    // A resize below the minimum closes it, with the message an open would give.
    app_dev_frame_size(app, 456, 800);
    app_dev_feed_events(app, NULL, 0, &t->arena);
    TEST_CHECK(t, !modal_active(app) && tm->closed == 5 && test_app_echo_is(app, "The frame is too small for the test modal"),
               "modal: a resize too small: active %d, echo '%S'", modal_active(app), test_echo(app));
    // The minimum: 40 columns (58 * 70% = 40.6) and 6 rows (10 * 60%: an area of 160 px, a frame of 176).
    struct { i32 w, h; b32 fits; } sizes[] = { { 464, 176, 1 }, { 456, 176, 0 }, { 464, 175, 0 }, { 300, 100, 0 } };
    for (i32 i = 0; i < (i32)ARRAY_COUNT(sizes); i++) {
        app_dev_frame_size(app, sizes[i].w, sizes[i].h);
        app_dev_feed_events(app, NULL, 0, &t->arena);
        echo_clear(&app->echo);
        b32 opened = modal_open(app, &spec);
        TEST_CHECK(t, opened == sizes[i].fits && (opened || test_app_echo_is(app, "The frame is too small for the test modal")),
                   "modal: %d x %d: opened %d", sizes[i].w, sizes[i].h, opened);
        modal_close(app);
    }
    app_dev_frame_size(app, 1280, 800);
    app_dev_feed_events(app, NULL, 0, &t->arena);
    // One pane: modal-other-pane does nothing.
    ModalSpec one = spec;
    one.panes = 1;
    TEST_CHECK(t, modal_open(app, &one), "modal: one pane: did not open");
    modal_set_rows(app, 0, 4);
    app_dev_feed(app, "TAB <down>", &t->arena);
    TEST_CHECK(t, modal_focus_pane(app) == 0 && m->lists[0].selected == 1, "modal: one pane: TAB changes nothing");
    // Accept running a command after the box closed, on the selected window.
    tm->run = &CMD_END_OF_BUFFER;
    app_dev_feed(app, "RET", &t->arena);
    TEST_CHECK(t, !modal_active(app) && view_point(v, view_primary(v)) == 14, "modal: accept ran end-of-buffer after closing (%D)",
               view_point(v, view_primary(v)));
    tm->run = NULL;
    // Phase 19's shape: a status line instead of a field (90 / 80 / 30: 39 rows less the status line and its
    // separator, 37 list rows; y (784 - 611) / 2 = 86), the right pane without a selection (it scrolls), the
    // owner told of every selection that moved.
    ModalSpec git = spec;
    git.width_pct = 90;
    git.height_pct = 80;
    git.pane_split_pct = 30;
    git.status_line = 1;
    git.scroll_panes = 1u << 1;
    git.selected = test_modal_selected;
    git.status = test_modal_status;
    TEST_CHECK(t, modal_open(app, &git), "modal: status line: did not open");
    modal_set_rows(app, 0, 10);
    modal_set_rows(app, 1, 100);
    app_dev_feed_events(app, NULL, 0, &t->arena);
    TEST_CHECK(t, test_modal_rect_is(g->box, 64, 86, 1216, 697) && test_modal_rect_is(g->field, 65, 87, 1215, 103) && g->rows == 37 &&
                      test_modal_rect_is(g->panes[0], 65, 104, 409, 696) && test_modal_rect_is(g->panes[1], 410, 104, 1215, 696),
               "modal: status line: box %f %f %f %f, line %f %f, %d rows, panes %f-%f %f-%f", g->box.x0, g->box.y0, g->box.x1, g->box.y1,
               g->field.y0, g->field.y1, g->rows, g->panes[0].y0, g->panes[0].y1, g->panes[1].x0, g->panes[1].y1);
    TEST_CHECK(t, m->lists[0].selected == 0 && m->lists[1].selected == -1 && tm->selects == 0, "modal: scroll pane: selections %D %D",
               m->lists[0].selected, m->lists[1].selected);
    app_dev_feed(app, "<down> <up> <up>", &t->arena);
    TEST_CHECK(t, tm->selects == 2 && tm->select_pane == 0 && tm->select_row == 0 && m->lists[0].selected == 0,
               "modal: selected: %d calls, the last pane %d row %D", tm->selects, tm->select_pane, tm->select_row);
    app_dev_feed(app, "TAB <down> <down>", &t->arena);
    TEST_CHECK(t, m->lists[1].top == 2 && m->lists[1].selected == -1 && tm->selects == 2, "modal: scroll pane: <down> twice: top %D",
               m->lists[1].top);
    app_dev_feed(app, "C-v", &t->arena);
    TEST_CHECK(t, m->lists[1].top == 39, "modal: scroll pane: C-v: top %D", m->lists[1].top);
    app_dev_feed(app, "C-v C-v", &t->arena);
    TEST_CHECK(t, m->lists[1].top == 63, "modal: scroll pane: the last page stays full: top %D", m->lists[1].top);
    app_dev_feed(app, "M-v RET", &t->arena);
    TEST_CHECK(t, m->lists[1].top == 26 && tm->accept_pane == 1 && tm->accept_row == 26 && modal_active(app),
               "modal: scroll pane: RET accepts its top row (%d, %D)", tm->accept_pane, tm->accept_row);
    test_modal_click(t, app, 100, 104 + 3 * 16 + 5, 1);
    TEST_CHECK(t, modal_focus_pane(app) == 0 && m->lists[0].selected == 3 && tm->selects == 3 && tm->select_row == 3,
               "modal: a click selects row 3 and says so (%d)", tm->selects);
    test_modal_click(t, app, 300, 90, 1);
    TEST_CHECK(t, modal_active(app) && modal_focus_pane(app) == 0 && m->lists[0].selected == 3, "modal: a click on the status line");
    test_modal_click(t, app, 600, 104 + 2 * 16 + 5, 2);
    TEST_CHECK(t, modal_focus_pane(app) == 1 && m->lists[1].selected == -1 && tm->accept_pane == 1 && tm->accept_row == 28,
               "modal: a double click on the scroll pane accepts row %D", tm->accept_row);
    modal_grow_rows(app, 1, 120);
    modal_grow_rows(app, 0, 12);
    TEST_CHECK(t, m->lists[1].count == 120 && m->lists[1].top == 26 && m->lists[1].selected == -1 && m->lists[0].count == 12 &&
                      m->lists[0].selected == 3, "modal: rows grown");
    modal_close(app);
    // Not over the minibuffer; the layer's commands outside a modal.
    app_dev_feed(app, "M-x", &t->arena);
    TEST_CHECK(t, app->mini.active && !modal_open(app, &spec) && test_app_echo_is(app, "The test modal is not available in the minibuffer"),
               "modal: over the minibuffer");
    app_dev_feed(app, "C-g C-g", &t->arena);
    app_dev_feed(app, "M-x m o d a l - n e x t RET", &t->arena);
    TEST_CHECK(t, !app->mini.active && test_app_echo_is(app, "No modal is open"), "modal: modal-next without a modal: '%S'", test_echo(app));
    if (!test_app_destroy(t, app, "modal")) return 0;
    LOG("test: ok: modal layer (geometry at 70/60/50 and 90/80/30, a short box, the 40 x 6 minimum, movement, other pane, pages, "
        "refusals, a cancelled prefix, accept, mouse rows and double click, the wheel, a click outside, C-g, ESC, a resize, run after "
        "close, the minibuffer; a status line, a scroll pane, the selection callback, rows grown)");
    return 1;
}

// The open command list's state (NULL when it is not open).
static CmdList *test_cmdlist(App *app) {
    return app->modal.active && app->modal.spec.field_changed == cmdlist_field_changed ? app->modal.spec.data : NULL;
}

// The first `n` shown commands as "a,b,c".
static String8 test_cmdlist_top(Test *t, App *app, i32 n) {
    CmdList *cl = test_cmdlist(app);
    if (!cl) return STR8_LIT("(closed)");
    String8 out = str8(NULL, 0);
    for (i32 i = 0; i < MIN(n, cl->shown_count); i++) out = str8_fmt(&t->arena, "%S%s%S", out, i ? "," : "", cl->rows[cl->shown[i]].name);
    return out;
}

static String8 test_cmdlist_keys(App *app, const Command *cmd) {
    CmdList *cl = test_cmdlist(app);
    for (i32 i = 0; cl && i < cl->count; i++) if (cl->rows[i].cmd == cmd) return cl->rows[i].keys;
    return STR8_LIT("(none)");
}

static String8 test_cmdlist_field(Test *t, App *app) {
    Buffer *buf = app->modal.field_buffer;
    return buf ? buffer_text(buf, &t->arena, 0, buffer_size(buf)) : str8(NULL, 0);
}

// show-command-list through the headless app: C-S-p, every command in table order with its keys, the filter's
// goldens (a name, two words, a description only, a query split across name and description), the ends of the list,
// RET running a motion, a region command and a prompting one, C-g and ESC, the minibuffer, refusals, editing the
// field, the mouse, no allocation per keystroke, a config reload while open, a frame too small.
static b32 test_command_list(Test *t) {
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "command list: no app");
    app_dev_show_scratch(app, STR8_LIT("one\ntwo\nthree\n"));
    View *v = app_selected_view(app);
    view_set_point(v, view_primary(v), 4);
    app_dev_feed(app, "C-S-p", &t->arena);
    CmdList *cl = test_cmdlist(app);
    TEST_CHECK(t, cl && cl->count == command_count() && cl->shown_count == cl->count, "command list: C-S-p did not open it with every command");
    b32 in_order = 1;
    for (i32 i = 0; i < cl->count; i++) in_order &= cl->shown[i] == i && cl->rows[i].cmd == command_at(i);
    TEST_CHECK(t, in_order, "command list: the empty filter lists the table in its order");
    String8 status = cmdlist_status(app, cl, &t->arena);
    TEST_CHECK(t, str8_equal(status, str8_fmt(&t->arena, "%d/%d", command_count(), command_count())), "command list: status '%S'", status);
    struct { const Command *cmd; const char *keys; } keys[] = {
        { &CMD_SAVE_BUFFER, "C-x C-s" }, { &CMD_FORWARD_CHAR, "C-f, <right>" }, { &CMD_GOTO_LINE, "M-g g, M-g M-g" },
        { &CMD_UNDO, "C-/, C-_ +1" }, { &CMD_NEWLINE, "RET, C-m +1" }, { &CMD_SKIP_TO_NEXT_LIKE_THIS, "" },
        { &CMD_SHOW_COMMAND_LIST, "C-S-p" }, { &CMD_MODAL_OTHER_PANE, "" },
    };
    for (i32 i = 0; i < (i32)ARRAY_COUNT(keys); i++) {
        String8 got = test_cmdlist_keys(app, keys[i].cmd);
        TEST_CHECK(t, str8_equal(got, str8_cstr(keys[i].keys)), "command list: %s shows '%S', expected '%s'", keys[i].cmd->name, got, keys[i].keys);
    }
    // No allocation per keystroke: the arena stays where the open left it.
    u64 pos = arena_pos(&app->modal.arena);
    app_dev_feed(app, "s a v e SPC b u f f e r SPC k i l l SPC w o r d", &t->arena);
    TEST_CHECK(t, arena_pos(&app->modal.arena) == pos && str8_equal(test_cmdlist_field(t, app), STR8_LIT("save buffer kill word")),
               "command list: 21 keystrokes moved the arena from %U to %U", pos, arena_pos(&app->modal.arena));
    app_dev_feed(app, "C-a C-k", &t->arena);

    // The filter's goldens.
    struct { const char *query; const char *top; i32 n; } goldens[] = {
        { "gotol", "goto-line", 1 },
        { "sav buf", "save-buffer,save-buffers-kill-terminal,save-some-buffers", 3 },
        { "clipboard", "kill-ring-save,yank", 2 },
        { "undo", "undo,undo-redo", 2 },
        { "occur lines", "occur", 1 },
    };
    for (i32 i = 0; i < (i32)ARRAY_COUNT(goldens); i++) {
        app_dev_feed(app, "C-a C-k", &t->arena);
        String8 query = str8_cstr(goldens[i].query);
        for (i64 k = 0; k < query.len; k++) {
            u8 typed[4] = { query.data[k] };
            app_dev_feed(app, query.data[k] == ' ' ? "SPC" : (const char *)typed, &t->arena);
        }
        String8 top = test_cmdlist_top(t, app, goldens[i].n);
        TEST_CHECK(t, str8_equal(top, str8_cstr(goldens[i].top)), "command list: '%s' gives '%S', expected '%s'", goldens[i].query, top,
                   goldens[i].top);
    }
    // "occur lines" matches only across the name and the description: neither alone has both words.
    {
        FuzzyQuery q = fuzzy_query(&t->arena, STR8_LIT("occur lines"));
        CmdRow *row = &cl->rows[cl->shown[0]];
        TEST_CHECK(t, fuzzy_score(&q, row->name, 0, NULL, 0, NULL) == FUZZY_NO_MATCH &&
                          fuzzy_score(&q, row->description, 0, NULL, 0, NULL) == FUZZY_NO_MATCH && row->won == CMDLIST_BOTH,
                   "command list: 'occur lines' must match through name and description together only");
        MatchSpan name[64], desc[64];
        i32 nn, nd;
        cmdlist_spans(app, cl, row, &t->arena, name, &nn, desc, &nd);
        TEST_CHECK(t, nn == 1 && name[0].start == 0 && name[0].end == 5 && nd >= 1 && desc[0].start == 9 && desc[0].end == 14,
                   "command list: the split query's spans: %d in the name (%D-%D), %d in the description (%D-%D)", nn, nn ? name[0].start : -1,
                   nn ? name[0].end : -1, nd, nd ? desc[0].start : -1, nd ? desc[0].end : -1);
    }
    // The ends of the list.
    app_dev_feed(app, "C-a C-k", &t->arena);
    ModalList *list = modal_list(app, 0);
    app_dev_feed(app, "C-p <up>", &t->arena);
    TEST_CHECK(t, list->selected == 0 && list->top == 0, "command list: C-p and <up> at the first row");
    for (i32 i = 0; i < 8; i++) app_dev_feed(app, "C-v", &t->arena);
    app_dev_feed(app, "<down> C-n", &t->arena);
    TEST_CHECK(t, list->selected == cl->count - 1 && list->top == cl->count - app->modal.g.rows, "command list: the last row: %D, top %D",
               list->selected, list->top);
    app_dev_feed(app, "M-v <next> <prior>", &t->arena);
    TEST_CHECK(t, list->selected == cl->count - 1 - app->modal.g.rows, "command list: M-v, <next>, <prior>: %D", list->selected);

    // Refusals inside it; editing the field.
    app_dev_feed(app, "C-x C-s", &t->arena);
    TEST_CHECK(t, modal_active(app) && test_app_echo_is(app, "save-buffer is not available in the command list"), "command list: C-x C-s: '%S'",
               test_echo(app));
    app_dev_feed(app, "a b c SPC d e f M-DEL", &t->arena);
    TEST_CHECK(t, str8_equal(test_cmdlist_field(t, app), STR8_LIT("abc ")), "command list: M-DEL in the field");
    app_dev_feed(app, "C-a C-k", &t->arena);
    TEST_CHECK(t, test_cmdlist_field(t, app).len == 0 && cl->shown_count == cl->count, "command list: C-a C-k empties the filter");
    app_dev_feed(app, "C-y C-b C-b x", &t->arena);
    TEST_CHECK(t, str8_equal(test_cmdlist_field(t, app), STR8_LIT("abxc ")), "command list: C-y and C-b in the field: '%S'",
               test_cmdlist_field(t, app));
    TEST_CHECK(t, str8_equal(test_app_text(t, app), STR8_LIT("one\ntwo\nthree\n")) && view_point(v, view_primary(v)) == 4,
               "command list: the buffer underneath is untouched");

    // C-g and ESC: closed, nothing said, everything underneath as it was.
    app_dev_feed(app, "C-g", &t->arena);
    TEST_CHECK(t, !modal_active(app) && test_app_echo_is(app, "") && view_point(v, view_primary(v)) == 4 && app_dev_window_count(app) == 1 &&
                      arena_pos(&app->modal.arena) == 0, "command list: C-g");
    app_dev_feed(app, "C-S-p e n d ESC", &t->arena);
    TEST_CHECK(t, !modal_active(app) && test_app_echo_is(app, "") && view_point(v, view_primary(v)) == 4, "command list: ESC");
    // RET runs the selected command on the window, as M-x does.
    app_dev_feed(app, "C-S-p e n d - o f - b u f RET", &t->arena);
    TEST_CHECK(t, !modal_active(app) && view_point(v, view_primary(v)) == 14, "command list: RET ran end-of-buffer (%D)", view_point(v, view_primary(v)));
    app_dev_feed(app, "C-S-p m a r k - w h o l e RET", &t->arena);
    TEST_CHECK(t, !modal_active(app) && view_region_active(v, view_primary(v), &app->config->settings), "command list: mark-whole-buffer");
    app_dev_feed(app, "C-g C-S-p g o t o - l i n e RET", &t->arena);
    TEST_CHECK(t, !modal_active(app) && app->mini.active && str8_equal(app->mini.prompt, STR8_LIT("Goto line: ")),
               "command list: goto-line opened its prompt after the list closed ('%S')", app->mini.prompt);
    // In the minibuffer it is refused.
    app_dev_feed(app, "C-S-p", &t->arena);
    TEST_CHECK(t, !modal_active(app) && app->mini.active && test_app_echo_is(app, "The command list is not available in the minibuffer"),
               "command list: in the minibuffer: '%S'", test_echo(app));
    app_dev_feed(app, "C-g C-g", &t->arena);
    TEST_CHECK(t, !app->mini.active, "command list: the prompt cancelled");
    // From M-x too.
    app_dev_feed(app, "M-x s h o w - c o m m a n d - l i s t RET", &t->arena);
    TEST_CHECK(t, modal_active(app) && test_cmdlist(app), "command list: M-x show-command-list");

    // The mouse. The box: 27 list rows under the field line, y from 166; the rows from y 184.
    TEST_CHECK(t, app->modal.g.rows == 27 && app->modal.g.box.y0 == 166 && app->modal.g.panes[0].y0 == 184, "command list: geometry %d rows, y %f",
               app->modal.g.rows, app->modal.g.box.y0);
    test_modal_click(t, app, 400, 184 + 2 * 16 + 4, 1);
    TEST_CHECK(t, list->selected == 2, "command list: a click selects row 2");
    Event wheel = { .kind = EVENT_MOUSE_WHEEL, .x = 400, .y = 300, .wheel = -120 };
    app_dev_feed_events(app, &wheel, 1, &t->arena);
    TEST_CHECK(t, list->top == 3 && list->selected == 2, "command list: the wheel scrolls (top %D)", list->top);
    app_dev_feed(app, "e n d - o f - b u f", &t->arena);
    view_set_point(v, view_primary(v), 0);
    test_modal_click(t, app, 400, 184 + 4, 2);
    TEST_CHECK(t, !modal_active(app) && view_point(v, view_primary(v)) == 14, "command list: a double click ran end-of-buffer");
    app_dev_feed(app, "C-S-p", &t->arena);
    test_modal_click(t, app, 10, 10, 1);
    TEST_CHECK(t, !modal_active(app) && view_point(v, view_primary(v)) == 14 && test_app_echo_is(app, ""), "command list: a click outside");

    // A config applied while it is open: the bindings shown and its keys follow.
    app_dev_feed(app, "C-S-p", &t->arena);
    String8 conf = test_results_conf(t, "p18keys.conf", "[keys]\nC-c s save-buffer\nC-x C-s none\n[keys command-list]\nC-j modal-next\n"
                                                        "[colors]\nmodal_border = #123456\n");
    app_dev_use_config(app, conf);
    app_dev_feed(app, "C-j C-j", &t->arena);
    TEST_CHECK(t, modal_active(app) && str8_equal(test_cmdlist_keys(app, &CMD_SAVE_BUFFER), STR8_LIT("C-c s")) && list->selected == 2 &&
                      app->config->theme.modal_border == 0x123456,
               "command list: after a reload: save-buffer '%S', selected %D", test_cmdlist_keys(app, &CMD_SAVE_BUFFER), list->selected);
    app_dev_feed(app, "C-g", &t->arena);
    // Too small a frame.
    app_dev_frame_size(app, 300, 100);
    app_dev_feed(app, "C-S-p", &t->arena);
    TEST_CHECK(t, !modal_active(app) && test_app_echo_is(app, "The frame is too small for the command list"), "command list: too small: '%S'",
               test_echo(app));
    app_dev_frame_size(app, 1280, 800);
    if (!test_app_destroy(t, app, "command list")) return 0;
    LOG("test: ok: command list (%d commands, %d key texts, %d filter goldens, the split query's spans, the ends, refusals, the field, "
        "C-g, ESC, RET x 3, the minibuffer, M-x, the mouse, a reload while open, too small; no allocation over 21 keystrokes)",
        command_count(), (i32)ARRAY_COUNT(keys), (i32)ARRAY_COUNT(goldens));
    return 1;
}

// The command list's ranking done the slow way: every row scored on its name, its description and both (no mask
// prefilter), then the best one picked again and again (score, then table order). Returns the matches.
static i32 test_cmdlist_reference(Arena *a, CmdList *cl, String8 text, i32 *out) {
    text.len = MIN(text.len, (i64)CMDLIST_QUERY_CAP);
    if (!text.len) {
        for (i32 i = 0; i < cl->count; i++) out[i] = i;
        return cl->count;
    }
    FuzzyQuery q = fuzzy_query(a, text);
    i32 *score = PUSH_ARRAY(a, i32, cl->count);
    b32 *taken = PUSH_ARRAY(a, b32, cl->count);
    i32 n = 0;
    for (i32 i = 0; i < cl->count; i++) {
        CmdRow *row = &cl->rows[i];
        i32 s = fuzzy_score(&q, row->name, 0, NULL, 0, NULL);
        i32 d = fuzzy_score(&q, row->description, 0, NULL, 0, NULL), b = fuzzy_score(&q, row->both, 0, NULL, 0, NULL);
        if (d != FUZZY_NO_MATCH) s = s == FUZZY_NO_MATCH ? d - CMDLIST_DESC_PENALTY : MAX(s, d - CMDLIST_DESC_PENALTY);
        if (b != FUZZY_NO_MATCH) s = s == FUZZY_NO_MATCH ? b - CMDLIST_DESC_PENALTY : MAX(s, b - CMDLIST_DESC_PENALTY);
        score[i] = s;
        n += s != FUZZY_NO_MATCH;
    }
    for (i32 k = 0; k < n; k++) {
        i32 best = -1;
        for (i32 i = 0; i < cl->count; i++) {
            if (taken[i] || score[i] == FUZZY_NO_MATCH) continue;
            if (best < 0 || score[i] > score[best]) best = i;
        }
        taken[best] = 1;
        out[k] = best;
    }
    return n;
}

// Seeded (--fuzz-seeds): random sessions of the command list through the headless app (soak's flag on: commands that
// end the session or leave the directory do nothing). Typing, DEL, clearing, movement, pages, the wheel, clicks on
// rows and in the field, a click outside, C-g, ESC, RET and double clicks running whatever is selected. While open:
// the selection and the top in range, the shown order equal to the slow ranking of the field's text, the arena where
// the open left it. Closed by C-g, ESC or a click outside: the window underneath as before the open.
static b32 test_modal_fuzz(Test *t, u64 seed) {
    t->rng = seed ^ 0x6d6f64616cull;
    App *app = test_app_create(t);
    TEST_CHECK(t, app, "modal fuzz: no app");
    app->ctx.dev_soak = 1;
    app_dev_show_scratch(app, STR8_LIT("int main(void) {\n    return 0;\n}\n"));
    static const char chars[] = "abcdefgilnoprstuwxyz-";
    static const char *moves[] = { "<down>", "<up>", "C-n", "C-p", "C-v", "M-v", "<next>", "<prior>" };
    i32 ops = 0, opens = 0, runs = 0, closes = 0, checks = 0;
    Buffer *before_buf = NULL;
    i64 before_point = 0;
    i32 before_windows = 0;
    u64 open_pos = 0;
    i32 *ref = PUSH_ARRAY(&t->arena, i32, command_count());
    for (; ops < 600; ops++) {
        if (!modal_active(app)) {
            app_dev_feed(app, "C-g C-g C-g", &t->arena); // whatever the last command left: a prompt, a search
            if (app_dev_window_count(app) > 1) app_dev_feed(app, "C-x 1", &t->arena);
            View *v = app_selected_view(app);
            // Not *Messages*: a message logged while the box is open ("Beginning of buffer" from DEL in the field) moves its
            // point (the "Later" list); the soak skips it for the same reason.
            Buffer *other = v->buffer == app->messages ? app_other_buffer(app, app->messages) : NULL;
            if (other) app_switch_buffer(app, v, other);
            before_buf = v->buffer;
            before_point = view_point(v, view_primary(v));
            before_windows = app_dev_window_count(app);
            app_dev_feed(app, "C-S-p", &t->arena);
            TEST_CHECK(t, modal_active(app) && test_cmdlist(app), "modal fuzz %U: op %d: C-S-p did not open it (echo '%S')", seed, ops, test_echo(app));
            open_pos = arena_pos(&app->modal.arena);
            opens++;
            continue;
        }
        ModalGeometry *g = &app->modal.g;
        u64 r = test_below(t, 100);
        i32 closed = 0; // 1: closed as C-g does, 2: something ran
        if (r < 35) {
            char c = chars[test_below(t, (i64)sizeof(chars) - 1)];
            app_dev_feed(app, test_below(t, 8) == 0 ? "SPC" : (const char[2]){ c, 0 }, &t->arena);
        } else if (r < 45) {
            app_dev_feed(app, "DEL", &t->arena);
        } else if (r < 50) {
            app_dev_feed(app, "C-a C-k", &t->arena);
        } else if (r < 70) {
            app_dev_feed(app, moves[test_below(t, ARRAY_COUNT(moves))], &t->arena);
        } else if (r < 75) {
            Event e = { .kind = EVENT_MOUSE_WHEEL, .x = (i32)g->panes[0].x0 + 8, .y = (i32)g->panes[0].y0 + 8,
                        .wheel = ((i32)test_below(t, 7) - 3) * 120 };
            app_dev_feed_events(app, &e, 1, &t->arena);
        } else if (r < 80 || r >= 97) {
            i32 y = (i32)g->panes[0].y0 + (i32)test_below(t, (i64)(g->panes[0].y1 - g->panes[0].y0));
            b32 dbl = r >= 97;
            u64 opens0 = app->modal.dev_opens;
            test_modal_click(t, app, (i32)g->panes[0].x0 + 40, y, dbl ? 2 : 1);
            if (dbl && (!modal_active(app) || app->modal.dev_opens != opens0)) closed = 2; // ran (show-command-list opens it again)
        } else if (r < 83) {
            test_modal_click(t, app, (i32)g->field.x0 + (i32)test_below(t, 200), (i32)g->field.y0 + 4, 1);
        } else if (r < 87) {
            test_modal_click(t, app, (i32)test_below(t, (i64)g->box.x0), (i32)test_below(t, 700), 1);
            TEST_CHECK(t, !modal_active(app), "modal fuzz %U: op %d: a click outside did not close it", seed, ops);
            closed = 1;
        } else if (r < 92) {
            app_dev_feed(app, test_below(t, 2) ? "C-g" : "ESC", &t->arena);
            TEST_CHECK(t, !modal_active(app) && test_app_echo_is(app, ""), "modal fuzz %U: op %d: C-g / ESC did not close it quietly", seed, ops);
            closed = 1;
        } else {
            CmdList *cl = test_cmdlist(app);
            ModalList *list = modal_list(app, 0);
            const Command *sel = list->count ? cl->rows[cl->shown[list->selected]].cmd : NULL;
            app_dev_feed(app, "RET", &t->arena);
            TEST_CHECK(t, modal_active(app) == (!sel || sel == &CMD_SHOW_COMMAND_LIST), "modal fuzz %U: op %d: RET on %s left it open %d", seed,
                       ops, sel ? sel->name : "no row", modal_active(app));
            if (sel) closed = 2;
        }
        if (closed == 1) {
            View *v = app_selected_view(app);
            TEST_CHECK(t, v->buffer == before_buf && (view_point(v, view_primary(v)) == before_point || before_buf == app->messages) &&
                              app_dev_window_count(app) == before_windows &&
                              arena_pos(&app->modal.arena) == 0,
                       "modal fuzz %U: op %d: closing changed the window underneath (point %D, was %D)", seed, ops, view_point(v, view_primary(v)),
                       before_point);
            closes++;
        }
        if (closed == 2) {
            runs++;
            if (modal_active(app)) { // show-command-list ran: a new open
                View *v = app_selected_view(app);
                before_buf = v->buffer;
                before_point = view_point(v, view_primary(v));
                before_windows = app_dev_window_count(app);
                open_pos = arena_pos(&app->modal.arena);
                opens++;
            }
        }
        if (!modal_active(app)) continue;
        // The invariants of an open list.
        CmdList *cl = test_cmdlist(app);
        ModalList *list = modal_list(app, 0);
        i32 rows = MAX(app->modal.g.rows, 1);
        TEST_CHECK(t, cl && list->count == cl->shown_count && (list->count ? list->selected >= 0 && list->selected < list->count : list->selected == -1) &&
                          list->top >= 0 && list->top <= MAX(list->count - rows, 0) && arena_pos(&app->modal.arena) == open_pos,
                   "modal fuzz %U: op %d: count %D (shown %d), selected %D, top %D, rows %d, arena %U (open %U)", seed, ops, list->count,
                   cl ? cl->shown_count : -1, list->selected, list->top, rows, arena_pos(&app->modal.arena), open_pos);
        u64 mark = arena_pos(&t->arena);
        String8 text = test_cmdlist_field(t, app);
        i32 n = test_cmdlist_reference(&t->arena, cl, text, ref);
        b32 same = n == cl->shown_count && str8_equal(str8(cl->query, cl->query_len), str8(text.data, MIN(text.len, (i64)CMDLIST_QUERY_CAP)));
        for (i32 i = 0; same && i < n; i++) same = ref[i] == cl->shown[i];
        TEST_CHECK(t, same, "modal fuzz %U: op %d: '%S' shows %d rows, the slow ranking %d (or another order)", seed, ops, text, cl->shown_count, n);
        arena_pop_to(&t->arena, mark);
        checks++;
    }
    if (!test_app_destroy(t, app, "modal fuzz")) return 0;
    LOG("test: ok: modal fuzz (seed %U: %d operations, %d opens, %d closed by C-g / ESC / a click outside, %d commands run, %d checks)",
        seed, ops, opens, closes, runs, checks);
    return 1;
}

// ---------------------------------------------------------------------------
// Phase 19: the git changes view

// A diff's lines as "K text" lines (K: T text, H header, @ hunk, + added, - removed, C the cut), then "first N".
static String8 test_git_diff_text(Arena *arena, const GitDiff *d) {
    static const char kinds[] = "TH@+-C";
    String8 s = str8(NULL, 0);
    for (i64 i = 0; i < d->count; i++) {
        GitLine *l = &d->lines[i];
        s = str8_fmt(arena, "%S%c %S\n", s, kinds[l->kind], str8(d->text.data + l->off, l->len));
    }
    return str8_fmt(arena, "%Sfirst %D%s", s, d->first_changed, d->cut ? " cut" : "");
}

static String8 test_git_status_text(Arena *arena, const GitStatus *st) {
    String8 s = str8_fmt(arena, "branch '%S'%s%s\n", st->branch, st->no_head ? " no head" : "", st->cut ? " cut" : "");
    for (i64 i = 0; i < st->count; i++) {
        GitEntry *e = &st->entries[i];
        s = str8_fmt(arena, "%S%c%c|%S|%S\n", s, e->xy[0], e->xy[1], e->path, e->orig);
    }
    return s;
}

static b32 test_git_parse(Test *t) {
    // Status: every kind, a rename's and a copy's source after it, a UTF-8 path, the branch record and its forms.
    static const u8 status[] = "## main...origin/main [ahead 1]\0M  src/app.c\0 M README.md\0A  new.c\0 D gone.c\0"
                               "R  src/new name.c\0src/old name.c\0C  copy.c\0orig.c\0RM moved.c\0was.c\0"
                               "?? notes/\xc4\x9f\xc3\xbc\xc5\x9f.txt\0?? b.txt\0";
    GitStatus st;
    git_parse_status(&t->arena, str8((u8 *)status, sizeof(status) - 1), 5000, &st);
    String8 got = test_git_status_text(&t->arena, &st);
    String8 want = STR8_LIT("branch 'main'\nM |src/app.c|\n M|README.md|\nA |new.c|\n D|gone.c|\nR |src/new name.c|src/old name.c\n"
                            "C |copy.c|orig.c\nRM|moved.c|was.c\n??|notes/\xc4\x9f\xc3\xbc\xc5\x9f.txt|\n??|b.txt|\n");
    TEST_CHECK(t, str8_equal(got, want), "git parse: status:\n%S", got);
    static const struct { const char *raw; i64 len; const char *want; } branches[] = {
        { "## No commits yet on main\0A  a.c\0?? b.c\0", 40, "branch 'main' no head\nA |a.c|\n??|b.c|\n" },
        { "## Initial commit on master\0", 28, "branch 'master' no head\n" },
        { "## HEAD (no branch)\0 M x\0", 25, "branch 'HEAD (no branch)'\n M|x|\n" },
        { "## main\0", 8, "branch 'main'\n" },
        { "", 0, "branch ''\n" },
    };
    for (i32 i = 0; i < (i32)ARRAY_COUNT(branches); i++) {
        git_parse_status(&t->arena, str8((u8 *)branches[i].raw, branches[i].len), 5000, &st);
        got = test_git_status_text(&t->arena, &st);
        TEST_CHECK(t, str8_equal(got, str8_cstr(branches[i].want)), "git parse: status %d:\n%S", i, got);
    }
    // The file limit: 5,001 records give 5,000 and the cut; exactly 5,000 no cut.
    for (i32 extra = 0; extra < 2; extra++) {
        String8 many = str8(PUSH_ARRAY(&t->arena, u8, 5001 * 12), 0);
        for (i32 i = 0; i < 5000 + extra; i++) many.len += fmt_buf(many.data + many.len, 12, " M f%05d", i) + 1;
        git_parse_status(&t->arena, many, 5000, &st);
        TEST_CHECK(t, st.count == 5000 && st.cut == extra && str8_equal(st.entries[4999].path, STR8_LIT("f04999")),
                   "git parse: %d records: %D kept, cut %d", 5000 + extra, st.count, st.cut);
    }

    // Diffs: two hunks (the second one must be there), a removed line that starts with "--" and an added one with
    // "++" (inside a hunk the counts decide), "\ No newline", CRLF content, the first changed line.
    static const char two[] = "diff --git a/a.c b/a.c\nindex 1111111..2222222 100644\n--- a/a.c\n+++ b/a.c\n"
                              "@@ -1,3 +1,3 @@\n int a;\n--- old\n+++ new\n int b;\n"
                              "@@ -10,2 +10,3 @@ int f(void) {\n x\r\n+y\r\n z\n\\ No newline at end of file\n";
    GitDiff d;
    git_parse_diff(&t->arena, STR8_LIT(two), 20000, &d);
    got = test_git_diff_text(&t->arena, &d);
    want = STR8_LIT("H diff --git a/a.c b/a.c\nH index 1111111..2222222 100644\nH --- a/a.c\nH +++ b/a.c\n@ @@ -1,3 +1,3 @@\n"
                    "T  int a;\n- --- old\n+ +++ new\nT  int b;\n@ @@ -10,2 +10,3 @@ int f(void) {\nT  x\n+ +y\nT  z\n"
                    "T \\ No newline at end of file\nfirst 2");
    TEST_CHECK(t, str8_equal(got, want), "git parse: two hunks:\n%S", got);
    static const struct { const char *raw, *want; } diffs[] = {
        // A new file (untracked, through --no-index): from line 1.
        { "diff --git a/n.txt b/n.txt\nnew file mode 100644\nindex 0000000..e69de29\n--- /dev/null\n+++ b/n.txt\n@@ -0,0 +1,2 @@\n+a\n+b\n",
          "H diff --git a/n.txt b/n.txt\nH new file mode 100644\nH index 0000000..e69de29\nH --- /dev/null\nH +++ b/n.txt\n"
          "@ @@ -0,0 +1,2 @@\n+ +a\n+ +b\nfirst 1" },
        // A deleted file: everything removed, its first line.
        { "diff --git a/g.c b/g.c\ndeleted file mode 100644\n--- a/g.c\n+++ /dev/null\n@@ -1,2 +0,0 @@\n-x\n-y\n",
          "H diff --git a/g.c b/g.c\nH deleted file mode 100644\nH --- a/g.c\nH +++ /dev/null\n@ @@ -1,2 +0,0 @@\n- -x\n- -y\nfirst 1" },
        // Lines removed after line 4: the line after.
        { "@@ -5,2 +4,0 @@\n-p\n-q\n", "@ @@ -5,2 +4,0 @@\n- -p\n- -q\nfirst 5" },
        // A change further down: the new side's line of the first - line.
        { "@@ -7,3 +7,3 @@\n a\n-b\n+B\n c\n", "@ @@ -7,3 +7,3 @@\nT  a\n- -b\n+ +B\nT  c\nfirst 8" },
        // A rename with changes, and one without (similarity 100%: no hunk, no first line).
        { "diff --git a/old.c b/new.c\nsimilarity index 100%\nrename from old.c\nrename to new.c\n",
          "H diff --git a/old.c b/new.c\nH similarity index 100%\nH rename from old.c\nH rename to new.c\nfirst 0" },
        // A binary file: its header line, nothing to visit.
        { "diff --git a/i.png b/i.png\nindex 1..2 100644\nBinary files a/i.png and b/i.png differ\n",
          "H diff --git a/i.png b/i.png\nH index 1..2 100644\nH Binary files a/i.png and b/i.png differ\nfirst 0" },
        // A mode change only.
        { "diff --git a/s.sh b/s.sh\nold mode 100644\nnew mode 100755\n", "H diff --git a/s.sh b/s.sh\nH old mode 100644\nH new mode 100755\nfirst 0" },
        // Two files: the hunk of the first ends by its counts, the second file's header follows.
        { "@@ -1 +1 @@\n-a\n+b\ndiff --git a/y b/y\n--- a/y\n+++ b/y\n@@ -1 +1,2 @@\n y\n+z\n",
          "@ @@ -1 +1 @@\n- -a\n+ +b\nH diff --git a/y b/y\nH --- a/y\nH +++ b/y\n@ @@ -1 +1,2 @@\nT  y\n+ +z\nfirst 1" },
        // No trailing newline; empty output.
        { "@@ -1 +1 @@\n-a\n+b", "@ @@ -1 +1 @@\n- -a\n+ +b\nfirst 1" },
        { "", "first 0" },
        // Not a diff (a failed run's output): a header line here; the view draws a failed run in the text color.
        { "fatal: bad object HEAD\n", "H fatal: bad object HEAD\nfirst 0" },
    };
    for (i32 i = 0; i < (i32)ARRAY_COUNT(diffs); i++) {
        git_parse_diff(&t->arena, str8_cstr(diffs[i].raw), 20000, &d);
        got = test_git_diff_text(&t->arena, &d);
        TEST_CHECK(t, str8_equal(got, str8_cstr(diffs[i].want)), "git parse: diff %d:\n%S", i, got);
    }
    // The line limit: 20,001 lines give 20,000 and the cut line; exactly 20,000 no cut.
    for (i32 extra = 0; extra < 2; extra++) {
        i64 n = 20000 + extra - 1;
        String8 big = str8(PUSH_ARRAY(&t->arena, u8, 32 + n * 16), 0);
        big.len += fmt_buf(big.data, 32, "@@ -0,0 +1,%D @@\n", n);
        for (i64 i = 0; i < n; i++) big.len += fmt_buf(big.data + big.len, 16, "+line %D\n", i + 1);
        git_parse_diff(&t->arena, big, 20000, &d);
        TEST_CHECK(t, d.count == 20000 + extra && d.cut == extra && (!extra || d.lines[20000].kind == GIT_LINE_CUT) &&
                          d.lines[19999].kind == GIT_LINE_ADDED, "git parse: %D lines: %D kept, cut %d", n + 1, d.count, d.cut);
    }
    // A line over 4,096 bytes is cut there, never inside a character (a 2-byte one across byte 4,096).
    String8 longl = str8(PUSH_ARRAY(&t->arena, u8, 5000), 0);
    longl.data[longl.len++] = '+';
    while (longl.len < 4095) longl.data[longl.len++] = 'x';
    longl.data[longl.len++] = 0xC4;
    longl.data[longl.len++] = 0x9F;
    while (longl.len < 4990) longl.data[longl.len++] = 'y';
    longl.data[longl.len++] = '\n';
    git_parse_diff(&t->arena, str8_fmt(&t->arena, "@@ -0,0 +1 @@\n%S", longl), 20000, &d);
    TEST_CHECK(t, d.count == 2 && d.lines[1].len == 4095, "git parse: a long line kept %u bytes", d.count == 2 ? d.lines[1].len : 0);

    // The error line: the prefixed line wins, blank lines and CR skipped, the cut on a character boundary.
    static const struct { const char *out, *want; } errors[] = {
        { "fatal: not a git repository (or any of the parent directories): .git\n", "fatal: not a git repository (or any of the parent directories): .git" },
        { "\n   \nsomething first\r\nerror: pathspec 'x' did not match\r\n", "error: pathspec 'x' did not match" },
        { "fatal: detected dubious ownership in repository at 'C:/x/y'\n'C:/x/y' is owned by:\n\tS-1-5-32-544\nTo add an exception "
          "for this directory, call:\n\n\tgit config --global --add safe.directory C:/x/y\n",
          "fatal: detected dubious ownership in repository at 'C:/x/y'" },
        { "\r\n  plain words\r\nmore\n", "  plain words" },
        { "usage: git diff [<options>]\n", "usage: git diff [<options>]" },
        { "", "" },
    };
    for (i32 i = 0; i < (i32)ARRAY_COUNT(errors); i++) {
        got = git_error_line(str8_cstr(errors[i].out));
        TEST_CHECK(t, str8_equal(got, str8_cstr(errors[i].want)), "git parse: error line %d: '%S'", i, got);
    }
    String8 longf = str8(PUSH_ARRAY(&t->arena, u8, 400), 0);
    memcpy(longf.data, "fatal: ", 7);
    longf.len = 7;
    while (longf.len < 119) longf.data[longf.len++] = 'a';
    longf.data[longf.len++] = 0xE2; // a 3-byte character over bytes 119-121
    longf.data[longf.len++] = 0x80;
    longf.data[longf.len++] = 0xA6;
    while (longf.len < 300) longf.data[longf.len++] = 'b';
    got = git_error_line(longf);
    TEST_CHECK(t, got.len == 119, "git parse: a 300-byte error line cut at %D bytes", got.len);
    TEST_CHECK(t, git_not_a_repository(STR8_LIT("x\nfatal: not a git repository (or any parent up to mount point /)\n")) &&
                      !git_not_a_repository(STR8_LIT("fatal: detected dubious ownership in repository at 'C:/x'\n")),
               "git parse: not a git repository");
    LOG("test: ok: git parse (status records, renames and copies, a UTF-8 path, the branch forms, no commits yet, the file limit; "
        "two hunks, -- and ++ lines inside a hunk, CRLF, no newline, new, deleted, renamed, binary and mode-only files, two files, "
        "the first changed line, the line limit, a long line; the error line and its cut, not a git repository)");
    return 1;
}

i32 test_run(u64 seed, String8 tmp_dir) {
    i32 ledger0 = os_dev_live_handles();
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
    test_marker_batch(&t, seed);
    arena_reset(&t.arena);
    test_line_states(&t, seed);
    arena_reset(&t.arena);
    test_capacity(&t);
    arena_reset(&t.arena);
    test_read_only(&t);
    arena_reset(&t.arena);
    test_files(&t);
    arena_reset(&t.arena);
    test_write_gap(&t);
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
    test_mc_driver(&t);
    arena_reset(&t.arena);
    test_mc_collapse(&t);
    arena_reset(&t.arena);
    test_mc_undo(&t, seed);
    arena_reset(&t.arena);
    test_mc_kill(&t);
    arena_reset(&t.arena);
    test_mc_commands(&t);
    arena_reset(&t.arena);
    test_mc_app(&t);
    arena_reset(&t.arena);
    test_mc_mouse(&t);
    arena_reset(&t.arena);
    test_mc_fuzz(&t, seed);
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
    test_font_atlas(&t);
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
    test_command_descriptions(&t);
    arena_reset(&t.arena);
    test_kbd(&t, seed);
    arena_reset(&t.arena);
    test_chords(&t);
    arena_reset(&t.arena);
    test_key_input(&t);
    arena_reset(&t.arena);
    test_keymap_where_is(&t);
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
    test_buffer_keymap(&t);
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
    test_window_region(&t);
    arena_reset(&t.arena);
    test_window_mouse(&t);
    arena_reset(&t.arena);
    test_window_prompts(&t);
    arena_reset(&t.arena);
    test_window_tiny(&t);
    arena_reset(&t.arena);
    test_window_startup(&t);
    arena_reset(&t.arena);
    test_modal_layer(&t);
    arena_reset(&t.arena);
    test_command_list(&t);
    arena_reset(&t.arena);
    test_modal_fuzz(&t, seed);
    arena_reset(&t.arena);
    test_git_parse(&t);
    arena_reset(&t.arena);
    test_list_dir(&t);
    arena_reset(&t.arena);
    test_threads(&t);
    arena_reset(&t.arena);
    test_tree_watch(&t);
    arena_reset(&t.arena);
    test_junction_small_file(&t);
    arena_reset(&t.arena);
    test_ignore_rules(&t);
    arena_reset(&t.arena);
    test_project_roots(&t);
    arena_reset(&t.arena);
    test_project_file(&t);
    arena_reset(&t.arena);
    test_scanner(&t, seed);
    arena_reset(&t.arena);
    os_dev_remove_dir(str8_fmt(&t.arena, "%S\\p11scan\\src\\loop", tmp_dir)); // the scanner's junction, whatever its checks did
    arena_reset(&t.arena);
    test_fuzzy(&t, seed);
    arena_reset(&t.arena);
    test_grep_file(&t, seed);
    arena_reset(&t.arena);
    test_grep_search(&t, seed);
    arena_reset(&t.arena);
    test_grep_sharing(&t);
    arena_reset(&t.arena);
    test_results(&t);
    arena_reset(&t.arena);
    test_occur(&t);
    arena_reset(&t.arena);
    test_results_files_arena(&t);
    arena_reset(&t.arena);
    test_results_transient(&t);
    arena_reset(&t.arena);
    test_landing(&t);
    arena_reset(&t.arena);
    test_process_platform(&t);
    arena_reset(&t.arena);
    test_process_program(&t);
    arena_reset(&t.arena);
    test_compile_match(&t);
    arena_reset(&t.arena);
    test_compile_resolve(&t);
    arena_reset(&t.arena);
    test_compile_config(&t);
    arena_reset(&t.arena);
    test_compile_remember_memory(&t);
    arena_reset(&t.arena);
    test_compile_clean(&t);
    arena_reset(&t.arena);
    test_compile_run(&t);
    arena_reset(&t.arena);
    test_compile_kill(&t);
    arena_reset(&t.arena);
    test_compile_app(&t);
    arena_reset(&t.arena);
    test_finder(&t);
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
    test_write_file_meta(&t);
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
    test_crash_recovery(&t);
    arena_reset(&t.arena);
    test_bounded_caps(&t);
    arena_reset(&t.arena);
    test_leak_metrics(&t, ledger0);
    arena_reset(&t.arena);

    LOG("test: %s, %d failure(s), %U ms", t.failures ? "FAIL" : "PASS", t.failures, (os_time_us() - t0) / 1000);
    os_release(t.arena.base);
    return t.failures;
}

// ---------------------------------------------------------------------------
// Soak (--soak N): a long random stream of key and mouse events through the headless app (the real keymap,
// driver, prompts, isearch, query-replace, multiple cursors, windows and mouse) over several buffers, with
// the invariants checked every SOAK_CHECK_EVERY events. Its files live in build\tmp\soak, the only place
// it may write (os_dev_write_root); commands that would quit, run a build or open the user's config do
// nothing (CommandContext.dev_soak).

#define SOAK_CHECK_EVERY 1000
#define SOAK_BLOCK 100000
#define SOAK_MAX_BUFFERS 12
#define SOAK_MAX_SIZE MB(1)

typedef struct Soak {
    Test *t;
    App *app;
    String8 dir;
    String8 *seqs; // every bound key sequence of the global, minibuffer, isearch, search-results and command-list keymaps (kbd)
    i32 seq_count;
    i32 w, h;      // the frame (h 0: minimized)
    b32 after_key; // the last event was a key: the selected window's point must be visible
    u64 keys, texts, mice, resizes, focus, truncated, killed, modal_sessions;
    char recent[32][64]; // the last events, logged when a check fails
    u64 recent_count;
} Soak;

static void soak_note(Soak *s, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char *out = s->recent[s->recent_count++ % ARRAY_COUNT(s->recent)];
    i64 n = fmt_v((u8 *)out, sizeof(s->recent[0]) - 1, fmt, args);
    out[MIN(n, (i64)sizeof(s->recent[0]) - 1)] = 0;
    va_end(args);
}

static void soak_feed(Soak *s, Event *events, i32 n) {
    for (i32 i = 0; i < n; i++) {
        Event *e = &events[i];
        if (e->kind >= EVENT_MOUSE_DOWN && e->kind <= EVENT_MOUSE_WHEEL) {
            soak_note(s, "mouse %d at %d,%d clicks %d wheel %d mods %x", e->kind, e->x, e->y, e->clicks, e->wheel, e->mods);
        } else if (e->kind == EVENT_RESIZE) {
            soak_note(s, "resize %dx%d", e->width, e->height);
        } else if (e->kind == EVENT_FOCUS) {
            soak_note(s, "focus %d", e->focused);
        }
    }
    if (!n) soak_note(s, "frame");
    app_dev_feed_events(s->app, events, n, &s->t->arena);
}

static void soak_keys(Soak *s, String8 kbd, b32 batch) {
    Event events[16];
    i32 n = app_dev_key_events(s->app, kbd, events, ARRAY_COUNT(events));
    soak_note(s, "keys '%S'%s", kbd, batch ? " (one frame)" : "");
    if (batch) app_dev_feed_events(s->app, events, n, &s->t->arena); // typed ahead: every event of the sequence in one frame
    else for (i32 i = 0; i < n; i++) app_dev_feed_events(s->app, &events[i], 1, &s->t->arena);
    s->after_key = 1;
}

static void soak_collect(Soak *s, Keymap *map) {
    for (i32 i = 0; i < map->count; i++) {
        u8 text[KEY_SEQ_TEXT_CAP];
        i64 n = key_seq_print(&map->bindings[i].seq, text, sizeof(text));
        s->seqs[s->seq_count++] = str8_copy(&s->t->arena, str8(text, n));
    }
}

static b32 soak_busy(App *app) {
    return app->modal.active || app->mini.active || app->isearch.active || app->replace.state != REPLACE_OFF || cursor_job_pending(&app->cursor_job);
}

// The buffer against its own line index (a full scan), its markers, its batch and its modified flag.
static b32 soak_check_buffer(Test *t, Buffer *buf, u64 event) {
    i64 size = buffer_size(buf), lines = 1;
    TEST_CHECK(t, buf->batch_depth == 0, "soak event %U: %S: a batch left open", event, buf->name);
    String8 seg[2];
    buffer_segments(buf, &seg[0], &seg[1]);
    TEST_CHECK(t, seg[0].len + seg[1].len == size, "soak event %U: %S: segments %D + %D, size %D", event, buf->name, seg[0].len,
               seg[1].len, size);
    i64 pos = 0;
    for (i32 k = 0; k < 2; k++) {
        for (i64 i = 0; i < seg[k].len; i++, pos++) {
            if (seg[k].data[i] != '\n') continue;
            TEST_CHECK(t, buffer_line_start(buf, lines) == pos + 1, "soak event %U: %S: line %D starts at %D, expected %D", event,
                       buf->name, lines, buffer_line_start(buf, lines), pos + 1);
            lines++;
        }
    }
    TEST_CHECK(t, buffer_line_count(buf) == lines, "soak event %U: %S: %D lines, expected %D", event, buf->name,
               buffer_line_count(buf), lines);
    for (i64 i = 0; i < buf->marker_live; i++) {
        i64 p = buf->markers[buf->marker_live_list[i]].pos;
        TEST_CHECK(t, p >= 0 && p <= size, "soak event %U: %S: a marker at %D (size %D)", event, buf->name, p, size);
    }
    if (buf->path.len && buf->undo.enabled) {
        TEST_CHECK(t, buf->modified == (buf->undo.state != buf->undo.saved_state), "soak event %U: %S: modified %d, state %U, saved %U",
                   event, buf->name, buf->modified, buf->undo.state, buf->undo.saved_state);
    }
    return 1;
}

// Every cursor of a view on a boundary, strictly ordered; the top a line start when the window is drawn (a window
// too small to draw is laid out again when it grows).
static b32 soak_check_view(Test *t, View *v, b32 drawn, u64 event) {
    Buffer *buf = v->buffer;
    i64 size = buffer_size(buf), last = -1;
    TEST_CHECK(t, v->cursor_count >= 1 && v->primary >= 0 && v->primary < v->cursor_count, "soak event %U: %d cursors, primary %d", event,
               v->cursor_count, v->primary);
    for (i32 k = 0; k < v->cursor_count; k++) {
        i64 p = view_point(v, &v->cursors[k]);
        TEST_CHECK(t, p >= 0 && p <= size && buffer_snap_char(buf, p) == p, "soak event %U: %S: cursor %d at %D (size %D) is not a boundary",
                   event, buf->name, k, p, size);
        TEST_CHECK(t, p > last, "soak event %U: %S: cursor %d at %D after %D", event, buf->name, k, p, last);
        last = p;
    }
    i64 top = buffer_marker_get(buf, v->top);
    TEST_CHECK(t, !drawn || top == buffer_line_start(buf, buffer_line_of(buf, top)), "soak event %U: %S: top at %D is not a line start",
               event, buf->name, top);
    return 1;
}

static b32 soak_check(Soak *s, u64 event) {
    Test *t = s->t;
    App *app = s->app;
    if (s->h > 0) TEST_CHECK(t, test_window_tiles(t, &app->windows, "soak"), "soak event %U: the windows", event);
    View *views[WINDOW_MAX];
    i32 n = app_views(app, views);
    for (i32 i = 0; i < n; i++) if (!soak_check_view(t, views[i], s->h > 0 && !app_view_hidden(app, views[i]), event)) return 0;
    if (!soak_check_view(t, app->mini.view, 0, event)) return 0;
    if (app->modal.field_buffer && !soak_check_view(t, &app->modal.field_view, 0, event)) return 0;
    if (app->modal.active) { // the open command list: its rows, selection and top
        ModalList *list = &app->modal.lists[0];
        TEST_CHECK(t, (list->count ? list->selected >= 0 && list->selected < list->count : list->selected == -1) && list->top >= 0 &&
                          list->top <= MAX(list->count - MAX(app->modal.g.rows, 1), 0),
                   "soak event %U: the modal's list: %D rows, selected %D, top %D", event, list->count, list->selected, list->top);
    }
    View *sel = app_selected_view(app);
    // Not *Messages*: a message logged outside a command (an undefined key) moves its point without scrolling
    // until the next command ("Later").
    if (s->after_key && s->h > 0 && !soak_busy(app) && !app_view_hidden(app, sel) && sel->buffer != app->messages) {
        i64 p = view_point(sel, view_primary(sel));
        i64 line = buffer_line_of(sel->buffer, p), top = view_top_line(sel);
        TEST_CHECK(t, line >= top && line < top + sel->rows, "soak event %U: %S: point's line %D outside [%D, %D)", event, sel->buffer->name,
                   line, top, top + sel->rows);
    }
    for (i32 i = 0; i < app->buffers.count; i++) if (!soak_check_buffer(t, app->buffers.entries[i].buffer, event)) return 0;
    if (!soak_check_buffer(t, app->mini.buffer, event)) return 0;
    if (app->modal.field_buffer && !soak_check_buffer(t, app->modal.field_buffer, event)) return 0;
    TEST_CHECK(t, t->arena.committed <= MB(256), "soak event %U: the test arena committed %U MB", event, t->arena.committed >> 20);
    return 1;
}

// Keeps the soak bounded: at most SOAK_MAX_BUFFERS buffers (the newest ones killed) and SOAK_MAX_SIZE bytes a
// buffer (the second half deleted, as an edit). Only between prompts and searches.
static void soak_tidy(Soak *s, i32 max_buffers) {
    App *app = s->app;
    if (soak_busy(app)) return;
    while (app->buffers.count > max_buffers) {
        Buffer *victim = NULL;
        for (i32 i = app->buffers.count - 1; i >= 0 && !victim; i--) {
            Buffer *b = app->buffers.entries[i].buffer;
            if (b != app->messages && !str8_equal(b->name, STR8_LIT("*scratch*"))) victim = b;
        }
        if (!victim) break;
        files_kill_buffer(app, victim);
        s->killed++;
    }
    for (i32 i = 0; i < app->buffers.count; i++) {
        Buffer *b = app->buffers.entries[i].buffer;
        i64 size = buffer_size(b);
        if (size <= SOAK_MAX_SIZE || b->read_only) continue;
        buffer_replace(b, buffer_snap_char(b, size / 2), size, STR8_LIT(""));
        s->truncated++;
    }
}

// Back to a known shape for a memory reading: prompts and searches ended, the buffers made since the start killed,
// one window. Returns the private bytes.
static u64 soak_settle(Soak *s, i32 initial_buffers) {
    for (i32 i = 0; i < 10 && soak_busy(s->app); i++) soak_keys(s, STR8_LIT("C-g"), 0);
    soak_tidy(s, initial_buffers);
    soak_keys(s, STR8_LIT("C-x 1"), 0);
    soak_feed(s, NULL, 0);
    return os_dev_private_bytes();
}

// The same work every time, for the slowdown: 10,000 characters typed into a new buffer and deleted again, in the
// settled shape; the buffer is killed afterwards. Nanoseconds an event.
static u64 soak_probe(Soak *s) {
    App *app = s->app;
    Buffer *probe = app_new_buffer(app, STR8_LIT("soak-probe"));
    app_switch_buffer(app, app_selected_view(app), probe);
    u64 t0 = os_time_us();
    for (i32 i = 0; i < 10000; i++) {
        Event e[2];
        i32 n = app_dev_key_events(app, i % 50 == 49 ? STR8_LIT("RET") : str8((u8 *)"abcdefghij" + i % 10, 1), e, 2);
        app_dev_feed_events(app, e, n, &s->t->arena);
    }
    for (i32 i = 0; i < 10000; i++) {
        Event e[2];
        i32 n = app_dev_key_events(app, STR8_LIT("DEL"), e, 2);
        app_dev_feed_events(app, e, n, &s->t->arena);
    }
    u64 ns = (os_time_us() - t0) * 1000 / 20000;
    files_kill_buffer(app, probe);
    return ns;
}

static void soak_mouse(Soak *s) {
    Test *t = s->t;
    i32 w = MAX(s->w, 1), h = MAX(s->h, 1);
    i32 x = (i32)test_below(t, w), y = (i32)test_below(t, h);
    static const u32 mods[] = { 0, 0, 0, KEYMOD_SHIFT, KEYMOD_CTRL | KEYMOD_SHIFT };
    u32 m = mods[test_below(t, ARRAY_COUNT(mods))];
    s->mice++;
    s->after_key = 0;
    switch (test_below(t, 4)) {
    case 0: case 1: { // a click (single, double or triple), now and then dragged
        Event e[5];
        i32 n = 0;
        e[n++] = (Event){ .kind = EVENT_MOUSE_DOWN, .button = MOUSE_LEFT, .clicks = 1 + (i32)test_below(t, 3), .x = x, .y = y, .mods = m };
        for (i32 k = (i32)test_below(t, 3); k > 0; k--) {
            x = CLAMP(x + (i32)test_below(t, 161) - 80, 0, w - 1);
            y = CLAMP(y + (i32)test_below(t, 97) - 48, 0, h - 1);
            e[n++] = (Event){ .kind = EVENT_MOUSE_MOVE, .x = x, .y = y, .mods = m };
        }
        e[n++] = (Event){ .kind = EVENT_MOUSE_UP, .button = MOUSE_LEFT, .x = x, .y = y, .mods = m };
        for (i32 i = 0; i < n; i++) soak_feed(s, &e[i], 1);
        break;
    }
    case 2: { // the wheel
        Event e = { .kind = EVENT_MOUSE_WHEEL, .x = x, .y = y, .wheel = ((i32)test_below(t, 7) - 3) * 120, .mods = m & KEYMOD_CTRL ? 0 : m };
        soak_feed(s, &e, 1);
        break;
    }
    default: { // a move alone
        Event e = { .kind = EVENT_MOUSE_MOVE, .x = x, .y = y };
        soak_feed(s, &e, 1);
        break;
    }
    }
}

static void soak_resize(Soak *s) {
    Test *t = s->t;
    u64 r = test_below(t, 10);
    if (r == 0) { s->w = 1; s->h = 0; }                                // minimized
    else if (r == 1) { s->w = 8 + (i32)test_below(t, 40); s->h = 16 + (i32)test_below(t, 40); } // tiny
    else { s->w = 200 + (i32)test_below(t, 1400); s->h = 120 + (i32)test_below(t, 900); }
    app_dev_frame_size(s->app, s->w, s->h);
    Event e = { .kind = EVENT_RESIZE, .width = s->w, .height = s->h };
    soak_feed(s, &e, 1);
    s->resizes++;
    s->after_key = 0;
}

static void soak_event(Soak *s) {
    Test *t = s->t;
    App *app = s->app;
    // Typed text: code characters; in a prompt nothing that names another directory or drive.
    static const char code[] = "abcdefxyzABZ0129_ {}();:,.=+-*<>\"'#/";
    static const char prompt[] = "abcdefgilnoprstuwxyz0129_-";
    static const char *keys[] = { "RET", "TAB", "DEL", "C-d", "C-g", "SPC", "<left>", "<right>", "<up>", "<down>", "<home>", "<end>",
                                  "<prior>", "<next>", "S-<right>", "S-<down>", "C-<right>", "<backtab>", "<delete>", "ESC" };
    u64 r = test_below(t, 1000);
    if (r < 430) {
        char c = app->mini.active ? prompt[test_below(t, (i64)sizeof(prompt) - 1)] : code[test_below(t, (i64)sizeof(code) - 1)];
        soak_keys(s, c == ' ' ? STR8_LIT("SPC") : str8((u8 *)&c, 1), 0);
        s->texts++;
    } else if (r < 520) {
        soak_keys(s, str8_cstr(keys[test_below(t, ARRAY_COUNT(keys))]), 0);
        s->keys++;
    } else if (r < 850) {
        soak_keys(s, s->seqs[test_below(t, s->seq_count)], test_below(t, 10) == 0);
        s->keys++;
    } else if (r < 856) { // a command list session: open, filter, move, then C-g, RET or a click outside
        if (!soak_busy(app)) {
            static const char *const sessions[] = { "C-S-p", "C-S-p g o", "C-S-p s a v e", "C-S-p e n d <down>", "C-S-p o t h e r C-n C-v M-v",
                                                    "C-S-p w i n d o w <down> <down>", "C-S-p z q z", "C-S-p l i n e DEL DEL <up>" };
            soak_keys(s, str8_cstr(sessions[test_below(t, ARRAY_COUNT(sessions))]), 0);
            u64 how = test_below(t, 3);
            if (how == 0) soak_keys(s, STR8_LIT("C-g"), 0);
            else if (how == 1) soak_keys(s, STR8_LIT("RET"), 0);
            else if (app->modal.active) {
                Event e[2] = { { .kind = EVENT_MOUSE_DOWN, .button = MOUSE_LEFT, .clicks = 1, .x = 0, .y = 0 },
                               { .kind = EVENT_MOUSE_UP, .button = MOUSE_LEFT, .x = 0, .y = 0 } };
                soak_feed(s, e, 2);
            }
            s->modal_sessions++;
        }
    } else if (r < 860) { // a chord nothing may be bound to
        static const char *mods[] = { "C-", "M-", "C-M-", "C-x C-", "C-c ", "C-x " };
        char token[16];
        test_cstr(token, sizeof(token), "%s%c", mods[test_below(t, ARRAY_COUNT(mods))], (u32)('a' + test_below(t, 26)));
        soak_keys(s, str8_cstr(token), 0);
        s->keys++;
    } else if (r < 985) {
        soak_mouse(s);
    } else if (r < 990) {
        soak_resize(s);
    } else if (r < 995) {
        Event e = { .kind = EVENT_FOCUS, .focused = (b32)test_below(t, 2) };
        soak_feed(s, &e, 1);
        s->focus++;
    } else {
        soak_feed(s, NULL, 0); // a frame without events (background work)
    }
    if (s->h == 0 && test_below(t, 4) == 0) soak_resize(s); // not minimized for long
}

static String8 soak_file(Test *t, String8 dir, const char *name, String8 text) {
    String8 path = str8_fmt(&t->arena, "%S\\%s", dir, name);
    os_write_file(path, text);
    return path;
}

// One run; checks every `every` events (the workers' results and the tidying stay every SOAK_CHECK_EVERY, so a
// rerun with every = 1 sees the same events). *failed_at: the event whose check failed.
// Our reservations that grew from snapshot a to snapshot b (or are new), the largest first, each with the functions that
// made it: what the leak check and the soak log when our committed bytes moved (Phase 16).
static void dev_log_reservation_growth(const char *prefix, OsDevReservation *a, i32 an, OsDevReservation *b, i32 bn, Arena *arena) {
    u64 mark = arena_pos(arena);
    i64 *delta = PUSH_ARRAY(arena, i64, bn + 1);
    i32 gone = 0;
    for (i32 i = 0; i < bn; i++) {
        delta[i] = (i64)b[i].committed;
        for (i32 j = 0; j < an; j++) if (a[j].base == b[i].base) delta[i] -= (i64)a[j].committed;
    }
    for (i32 j = 0; j < an; j++) {
        b32 found = 0;
        for (i32 i = 0; i < bn && !found; i++) found = a[j].base == b[i].base;
        gone += !found;
    }
    LOG("%s: %d reservations before, %d after (%d released, %d new)", prefix, an, bn, gone, bn - an + gone);
    for (i32 shown = 0; shown < 8; shown++) {
        i32 best = -1;
        for (i32 i = 0; i < bn; i++) if (delta[i] > 0 && (best < 0 || delta[i] > delta[best])) best = i;
        if (best < 0) break;
        LOG("%s:   +%D bytes (%U committed now) in the reservation made by %S", prefix, delta[best], b[best].committed,
            os_dev_reservation_origin(arena, b[best].base));
        delta[best] = 0;
    }
    arena_pop_to(arena, mark);
}

// The leak metrics at each 100,000-event checkpoint (Phase 16), split as --leak-check splits them. Verdict metrics,
// exactly flat: the handle ledger, GDI objects, our threads, events, processes and watches. Informational (WATCH when
// they rise at each of the last three checkpoints): the memory and our reservations, process handles, USER objects,
// the working set and the threads. The memory is informational here: the random walk keeps raising the high-water
// marks of the persistent arenas (the minibuffer, the kill ring, *Messages*) with the largest text it happens to push
// through them, and the set of buffers it leaves (a buffer has 7 to 9 arenas) differs from one checkpoint to the
// next; the reservations that grew are listed by the functions that made them, and the memory verdict comes from
// --leak-check's deterministic scenarios. Applied over the last 500,000 events once there are 600,000; the undo logs
// (up to undo_limit_mb a buffer) are left out of the memory rows.
enum {
    SOAK_M_LEDGER,
    SOAK_M_GDI,
    SOAK_M_THREADS,
    SOAK_M_EVENTS,
    SOAK_M_PROCESSES,
    SOAK_M_WATCHES,
    SOAK_M_SLOTS,   // bounded: at most the cap
    SOAK_M_PRIVATE, // informational from here on
    SOAK_M_COMMITTED,
    SOAK_M_RESERVATIONS,
    SOAK_M_HANDLES,
    SOAK_M_USER,
    SOAK_M_WORKING_SET,
    SOAK_M_PROCESS_THREADS,
    SOAK_M_COUNT
};
#define SOAK_CHECKPOINTS 64
typedef i64 SoakMetrics[SOAK_M_COUNT];

static const char *const soak_metric_names[SOAK_M_COUNT] = {
    "handle ledger (ours)", "GDI objects", "our threads", "our events", "our processes", "directory watches", "worker thread slots",
    "private commit without the undo logs (bytes)", "our committed bytes without the undo logs", "our reservations", "process handles",
    "USER objects",
    "working set (bytes)", "process threads (Toolhelp)",
};

static void soak_sample(i64 *v, u64 settled_private, u64 undo, i64 *slots_max) {
    OsDevMetrics m;
    os_dev_metrics(&m);
    v[SOAK_M_RESERVATIONS] = m.reservations;
    v[SOAK_M_LEDGER] = m.live_handles;
    v[SOAK_M_GDI] = m.gdi_objects;
    v[SOAK_M_THREADS] = m.live_threads;
    v[SOAK_M_EVENTS] = m.live_events;
    v[SOAK_M_PROCESSES] = m.live_processes;
    v[SOAK_M_WATCHES] = m.watches;
    v[SOAK_M_SLOTS] = m.worker_slots;
    *slots_max = m.worker_slots_max;
    v[SOAK_M_PRIVATE] = (i64)settled_private;
    v[SOAK_M_COMMITTED] = (i64)(m.reserved_committed > undo ? m.reserved_committed - undo : 0);
    v[SOAK_M_HANDLES] = m.handles;
    v[SOAK_M_USER] = m.user_objects;
    v[SOAK_M_WORKING_SET] = (i64)m.working_set;
    v[SOAK_M_PROCESS_THREADS] = m.process_threads;
}

// The table over the checkpoints from 500,000 events on (index 4); returns the LEAK verdicts.
static i32 soak_leak_verdict(SoakMetrics *v, i32 count, i64 slots_max) {
    i32 first = 4, leaks = 0;
    LOG("soak: leak metrics, the checkpoints from %d00,000 to %d00,000 events:", first + 1, count);
    for (i32 k = 0; k < SOAK_M_COUNT; k++) {
        i32 steps = 0, rising = 0;
        for (i32 j = first + 1; j < count; j++) {
            i64 d = v[j][k] - v[j - 1][k];
            steps += d > 0;
            if (j >= count - 3) rising += d > 0;
        }
        const char *verdict = k >= SOAK_M_PRIVATE ? (rising == 3 ? "WATCH" : "") : steps ? "LEAK" : "OK";
        if (k == SOAK_M_SLOTS) { // bounded: the slots grow to the most tasks ever running at once, at most the cap
            verdict = "OK (under its cap)";
            for (i32 j = first; j < count; j++) if (v[j][k] > slots_max) verdict = "LEAK (over its cap)";
        }
        leaks += verdict[0] == 'L';
        u8 row[512];
        i64 n = 0;
        for (i32 j = first; j < count; j++) n += fmt_buf(row + n, (i64)sizeof(row) - n, "%D | ", v[j][k]);
        LOG("soak: | %s | %S%s |", soak_metric_names[k], str8(row, n), verdict);
    }
    return leaks;
}

static i32 soak_run(u64 events, u64 seed, String8 tmp_dir, u64 every, u64 *failed_at, i32 *leak_verdicts) {
    Test t = { 0 };
    t.arena = arena_create(GB(4));
    t.tmp_dir = tmp_dir;
    t.rng = seed ^ 0x50a6ull;
    syntax_init();
    Soak s = { .t = &t, .w = 1280, .h = 800 };
    s.dir = os_full_path(&t.arena, str8_fmt(&t.arena, "%S\\soak", tmp_dir));
    os_make_dir(s.dir);
    OsDirEntry *entries;
    i64 count;
    if (os_list_dir(&t.arena, s.dir, &entries, &count) == OS_FILE_OK) { // the last run's files
        for (i64 i = 0; i < count; i++) if (!entries[i].is_dir) os_file_delete(str8_fmt(&t.arena, "%S\\%S", s.dir, entries[i].name));
    }
    // The files: C, JavaScript, Jai, plain text, and a 300 KB C file; the directory is the project.
    u8 *big = PUSH_ARRAY(&t.arena, u8, KB(320));
    i64 big_len = 0;
    for (i32 i = 0; big_len < KB(300); i++) {
        big_len += fmt_buf(big + big_len, 160, "int f%d(int a) {\n    switch (a) {\n    case %d: return a + 1; // one\n    default:\n"
                                               "        return \"s%d\"[0];\n    }\n}\n\n", i, i, i);
    }
    soak_file(&t, s.dir, ".ronin-project", STR8_LIT("[ignore]\n*.log\n"));
    String8 files[] = {
        soak_file(&t, s.dir, "a.c", STR8_LIT("#include <stdio.h>\n\nint main(void) {\n    int x = 1;\n    if (x) {\n        printf(\"%d\\n\", x);\n"
                                            "    }\n    return 0;\n}\n")),
        soak_file(&t, s.dir, "b.js", STR8_LIT("function f(a, b) {\n    const s = `x ${a}`;\n    return [a, b].map((v) => v * 2);\n}\n")),
        soak_file(&t, s.dir, "c.jai", STR8_LIT("main :: () {\n    x := 3;\n    if x == {\n        case 3; print(\"three\\n\");\n    }\n}\n")),
        soak_file(&t, s.dir, "notes.txt", STR8_LIT("Plain text, a few words.\nAnd \xc5\x9f\xc4\x9f\xc3\xbc lines.\n")),
        soak_file(&t, s.dir, "big.c", str8(big, big_len)),
    };
    String8 conf = soak_file(&t, s.dir, "soak.conf", STR8_LIT("[settings]\nundo_limit_mb = 2\nmax_cursors = 100\nfsync_on_save = false\nsearch_workers = 2\n"));
    // The directory too, as on a command line: *scratch* and the file prompts start there.
    AppFileArg args_files[ARRAY_COUNT(files) + 1];
    for (i32 i = 0; i < (i32)ARRAY_COUNT(files); i++) args_files[i] = (AppFileArg){ files[i], 0, 0 };
    args_files[ARRAY_COUNT(files)] = (AppFileArg){ s.dir, 0, 0 };
    AppArgs args = { .dpi_scale = 1.0f, .headless = 1, .files = args_files, .file_count = ARRAY_COUNT(args_files), .config_path = conf };
    App *app = app_create(&t.arena, &args);
    if (!app) {
        LOG("soak: FAIL: app_create");
        return 1;
    }
    s.app = app;
    app->ctx.dev_soak = 1;
    os_dev_write_root(s.dir);
    app_dev_feed_events(app, NULL, 0, &t.arena);
    s.seqs = PUSH_ARRAY(&t.arena, String8, 5 * KEYMAP_CAP);
    soak_collect(&s, &app->config->global);
    soak_collect(&s, &app->config->minibuffer);
    soak_collect(&s, &app->config->isearch);
    soak_collect(&s, &app->config->search_results);
    soak_collect(&s, &app->config->command_list);
    SoakMetrics *metrics = PUSH_ARRAY(&t.arena, SoakMetrics, SOAK_CHECKPOINTS); // before the mark the checks pop to
    OsDevReservation *res_mid = PUSH_ARRAY(&t.arena, OsDevReservation, 16384), *res_last = PUSH_ARRAY(&t.arena, OsDevReservation, 16384);
    i32 res_mid_count = 0, res_last_count = 0, checkpoints = 0;
    i64 slots_max = 0;
    *leak_verdicts = 0;
    u64 base = arena_pos(&t.arena);
    i32 initial_buffers = app->buffers.count;
    LOG("soak: %U events, seed 0x%X, %d key sequences, files in %S", events, seed, s.seq_count, s.dir);

    u64 t0 = os_time_us(), block_t0 = t0, peak = 0, first_us = 0, second_us = 0, last_us = 0, mem_after_first = 0, mem_last = 0;
    u64 done = 0;
    struct { u64 us, event; char what[64]; } slow[5] = { 0 }; // the slowest events, to look for slow commands
    for (u64 e = 1; e <= events && !t.failures; e++) {
        u64 e0 = os_time_us();
        soak_event(&s);
        u64 took = os_time_us() - e0;
        if (took > slow[4].us) {
            i32 k = 4;
            while (k > 0 && slow[k - 1].us < took) {
                slow[k] = slow[k - 1];
                k--;
            }
            slow[k].us = took;
            slow[k].event = e;
            memcpy(slow[k].what, s.recent[(s.recent_count - 1) % ARRAY_COUNT(s.recent)], sizeof(slow[k].what));
        }
        if (e % SOAK_CHECK_EVERY == 0) while (os_dev_wake_wait(0)) app_wake(app); // the workers' results (project search, scans)
        if (e % every == 0 && !soak_check(&s, e)) {
            *failed_at = e;
            LOG("soak: FAIL at event %U (seed 0x%X); the events before it, the last one first:", e, seed);
            for (u64 i = 0; i < ARRAY_COUNT(s.recent) && i < s.recent_count; i++) {
                LOG("soak:   %s", s.recent[(s.recent_count - 1 - i) % ARRAY_COUNT(s.recent)]);
            }
            View *views[WINDOW_MAX];
            i32 nv = app_views(app, views);
            for (i32 i = 0; i <= nv; i++) {
                View *v = i < nv ? views[i] : app->mini.view;
                LOG("soak: %s %d: %S, %d cursors (primary %d): %D %D %D, top line %D, %d rows", i < nv ? "window" : "minibuffer", i,
                    v->buffer->name, v->cursor_count, v->primary, view_point(v, &v->cursors[0]),
                    v->cursor_count > 1 ? view_point(v, &v->cursors[1]) : -1, v->cursor_count > 2 ? view_point(v, &v->cursors[2]) : -1,
                    view_top_line(v), v->rows);
            }
            LOG("soak: selected %S, minibuffer %s, echo '%S'", app_selected_view(app)->buffer->name, app->mini.active ? "active" : "off",
                str8(app->echo.text, app->echo.len));
            break;
        }
        if (e % SOAK_CHECK_EVERY == 0) {
            soak_tidy(&s, SOAK_MAX_BUFFERS);
            arena_pop_to(&t.arena, base);
        }
        if (e % SOAK_BLOCK == 0) {
            u64 now = os_time_us(), mem = os_dev_private_bytes();
            u64 per = (now - block_t0) * 1000 / SOAK_BLOCK; // ns an event
            peak = MAX(peak, mem);
            LOG("soak: %U events: %U ns an event in this block, private bytes %U KB, %d buffers, %d windows, %d cursors; keys %U, "
                "text %U, mouse %U, resizes %U, focus %U, command list sessions %U; buffers killed %U, truncated %U; writes refused %U", e,
                per, mem >> 10, app->buffers.count, window_count(&app->windows), app_selected_view(app)->cursor_count, s.keys, s.texts,
                s.mice, s.resizes, s.focus, s.modal_sessions, s.killed, s.truncated, os_dev_writes_refused());
            u64 settled = soak_settle(&s, initial_buffers), undo = 0;
            for (i32 i = 0; i < app->buffers.count; i++) undo += buffer_undo_memory(app->buffers.entries[i].buffer);
            per = soak_probe(&s);
            LOG("soak:   settled: private bytes %U KB; undo logs %U KB, kill ring %U KB; the probe %U ns an event", settled >> 10,
                undo >> 10, app->kills.small_committed >> 10, per);
            // The undo logs grow up to their limit (undo_limit_mb a buffer): the rest is what must stay flat.
            settled = settled > undo ? settled - undo : 0;
            if (checkpoints < SOAK_CHECKPOINTS) soak_sample(metrics[checkpoints++], settled, undo, &slots_max);
            if (checkpoints == 5) res_mid_count = os_dev_reservations(res_mid, 16384);
            res_last_count = os_dev_reservations(res_last, 16384);
            if (e == SOAK_BLOCK) {
                first_us = per;
                mem_after_first = settled;
            }
            if (e == 2 * SOAK_BLOCK) second_us = per;
            last_us = per;
            mem_last = settled;
            block_t0 = now;
        }
        done = e;
    }
    // Slowdown and growth: the last probe against the faster of the first two, memory after the first block against
    // the last.
    if (!t.failures && events >= 3 * SOAK_BLOCK) {
        u64 ref = second_us ? MIN(first_us, second_us) : first_us;
        if (last_us * 2 > ref * 3) {
            LOG("test: FAIL: soak: the last probe took %U ns an event, the first ones %U", last_us, ref);
            t.failures++;
        }
        // Leaks: settled (the starting buffers in one window), without the undo logs, after the first block and the last.
        LOG("soak: settled, without the undo logs: %U KB after the first block, %U KB after the last", mem_after_first >> 10,
            mem_last >> 10);
        if (mem_last > mem_after_first + MB(2)) {
            LOG("test: FAIL: soak: private bytes grew from %U KB to %U KB", mem_after_first >> 10, mem_last >> 10);
            t.failures++;
        }
    }
    if (!t.failures && checkpoints >= 6) {
        *leak_verdicts = soak_leak_verdict(metrics, checkpoints, slots_max);
        if (metrics[checkpoints - 1][SOAK_M_COMMITTED] != metrics[4][SOAK_M_COMMITTED] ||
            metrics[checkpoints - 1][SOAK_M_RESERVATIONS] != metrics[4][SOAK_M_RESERVATIONS]) {
            dev_log_reservation_growth("soak: our reservations from 500,000 events to the end", res_mid, res_mid_count, res_last, res_last_count,
                                       &t.arena);
        }
        if (*leak_verdicts) LOG("soak: LEAK: %d leak metric(s) grew over the last 500,000 events", *leak_verdicts);
    } else if (!t.failures) {
        LOG("soak: the leak metrics' rule needs 600,000 events (the last 500,000 after a first checkpoint): skipped");
    }
    u64 refused = os_dev_writes_refused();
    for (i32 i = 0; i < 10 && soak_busy(app); i++) soak_keys(&s, STR8_LIT("C-g"), 0);
    i32 leaks = app_shutdown(app);
    if (leaks) {
        LOG("test: FAIL: soak: %d leak(s) at shutdown", leaks);
        t.failures++;
    }
    for (i32 k = 0; k < 5 && slow[k].us; k++) LOG("soak: slowest event %d: %U us at event %U: %s", k + 1, slow[k].us, slow[k].event, slow[k].what);
    LOG("soak: %s, %U events in %U ms (seed 0x%X), private bytes peak %U KB, at the end %U KB; writes refused outside the directory %U",
        t.failures || *leak_verdicts ? "FAIL" : "PASS", done, (os_time_us() - t0) / 1000, seed, peak >> 10, os_dev_private_bytes() >> 10,
        refused);
    os_dev_write_root(STR8_LIT(""));
    os_release(t.arena.base);
    return t.failures;
}

// --soak N. A failure found by the periodic check is run again with a check after every event, up to it, to
// name the event that broke the invariant.
i32 test_soak(u64 events, u64 seed, String8 tmp_dir, i32 *leaks) {
    u64 failed_at = 0;
    i32 failures = soak_run(events, seed, tmp_dir, SOAK_CHECK_EVERY, &failed_at, leaks);
    if (failed_at) {
        LOG("soak: again up to event %U, checked after every event", failed_at);
        u64 exact = 0;
        i32 again;
        soak_run(failed_at, seed, tmp_dir, 1, &exact, &again);
    }
    return failures;
}

// ---------------------------------------------------------------------------
// --map-report MAP SRC: the release exe's bytes by module, from the linker map (build.bat release writes
// build\ronin.map). A symbol's size is the distance to the next symbol in its section part; a function
// inlined under /LTCG counts toward its caller. Our symbols (ronin.obj) go to the source file that defines
// them at file scope (the files in SRC scanned), string literals and constants to their own rows; the static
// CRT's objects (libcmt, libvcruntime, libucrt) to "CRT"; import thunks and tables to "imports"; section
// parts without symbols (unwind tables, imports, resources) by their name.

typedef struct MapSym {
    u32 sec;
    u64 off;
    String8 name, obj;
} MapSym;

typedef struct MapRow {
    String8 module;
    u64 code, rdata, data, bss, pdata;
} MapRow;

typedef struct MapIdent {
    String8 name, file;
} MapIdent;

#define MAP_IDENTS 32768

static u64 map_hash(String8 s) {
    u64 h = 1469598103934665603ull;
    for (i64 i = 0; i < s.len; i++) h = (h ^ s.data[i]) * 1099511628211ull;
    return h;
}

static b32 map_ident_char(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// File-scope definitions and declarations start at column 0 in this code base: on such a line the identifier
// right before the first '(', '=', '[' or ';' is what the line defines. A .c file wins over a header.
static void map_index_file(Arena *arena, MapIdent *idents, String8 file, String8 text) {
    b32 header = file.len > 2 && file.data[file.len - 1] == 'h' && file.data[file.len - 2] == '.';
    for (i64 i = 0; i < text.len;) {
        i64 end = i;
        while (end < text.len && text.data[end] != '\n') end++;
        u8 c = text.data[i];
        if (end > i && (map_ident_char(c) && !(c >= '0' && c <= '9'))) {
            i64 stop = i;
            while (stop < end && text.data[stop] != '(' && text.data[stop] != '=' && text.data[stop] != '[' && text.data[stop] != ';') stop++;
            i64 b = stop;
            while (b > i && text.data[b - 1] == ' ') b--;
            i64 a = b;
            while (a > i && map_ident_char(text.data[a - 1])) a--;
            if (stop < end && b > a) {
                String8 name = str8(text.data + a, b - a);
                u64 h = map_hash(name);
                for (u64 k = 0; k < MAP_IDENTS; k++) {
                    MapIdent *e = &idents[(h + k) % MAP_IDENTS];
                    if (!e->name.len) {
                        e->name = str8_copy(arena, name);
                        e->file = file;
                        break;
                    }
                    if (str8_equal(e->name, name)) {
                        if (!header) e->file = file;
                        break;
                    }
                }
            }
        }
        i = end + 1;
    }
}

static String8 map_lookup(MapIdent *idents, String8 name) {
    u64 h = map_hash(name);
    for (u64 k = 0; k < MAP_IDENTS; k++) {
        MapIdent *e = &idents[(h + k) % MAP_IDENTS];
        if (!e->name.len) return str8(NULL, 0);
        if (str8_equal(e->name, name)) return e->file;
    }
    return str8(NULL, 0);
}

static String8 map_token(String8 line, i64 *at) {
    i64 i = *at;
    while (i < line.len && (line.data[i] == ' ' || line.data[i] == '\t' || line.data[i] == '\r')) i++;
    i64 start = i;
    while (i < line.len && line.data[i] != ' ' && line.data[i] != '\t' && line.data[i] != '\r') i++;
    *at = i;
    return str8(line.data + start, i - start);
}

static u64 map_hex(String8 s) {
    u64 v = 0;
    for (i64 i = 0; i < s.len; i++) {
        u8 c = s.data[i];
        u32 d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (d == 16) break;
        v = v * 16 + d;
    }
    return v;
}

static b32 map_contains(String8 s, const char *what) {
    String8 w = str8_cstr(what);
    for (i64 i = 0; i + w.len <= s.len; i++) if (mem_equal(s.data + i, w.data, w.len)) return 1;
    return 0;
}

static MapRow *map_row(MapRow *rows, i32 *count, String8 module) {
    for (i32 i = 0; i < *count; i++) if (str8_equal(rows[i].module, module)) return &rows[i];
    rows[*count] = (MapRow){ .module = module };
    return &rows[(*count)++];
}

static void map_add(MapRow *row, String8 part, u64 bytes) {
    if (str8_starts_with(part, STR8_LIT(".text"))) row->code += bytes;
    else if (str8_starts_with(part, STR8_LIT(".bss"))) row->bss += bytes;
    else if (str8_starts_with(part, STR8_LIT(".data"))) row->data += bytes;
    else if (str8_starts_with(part, STR8_LIT(".pdata")) || str8_starts_with(part, STR8_LIT(".xdata"))) row->pdata += bytes;
    else row->rdata += bytes;
}

i32 test_map_report(String8 map_path, String8 src_dir) {
    Arena arena = arena_create(GB(1));
    String8 map;
    if (os_read_small_file(&arena, map_path, MB(64), &map) != OS_FILE_OK) {
        LOG("map-report: cannot read %S", map_path);
        return 1;
    }
    // The source index.
    MapIdent *idents = PUSH_ARRAY(&arena, MapIdent, MAP_IDENTS);
    String8 dirs[2] = { src_dir, str8_fmt(&arena, "%S\\..\\build\\gen", src_dir) }; // the sources, the compiled shaders
    for (i32 d = 0; d < 2; d++) {
        OsDirEntry *entries;
        i64 entry_count;
        if (os_list_dir(&arena, dirs[d], &entries, &entry_count) != OS_FILE_OK) continue;
        for (i32 pass = 0; pass < 2; pass++) { // headers first, so the .c files win
            for (i64 i = 0; i < entry_count; i++) {
                String8 n = entries[i].name;
                b32 header = n.len > 2 && n.data[n.len - 1] == 'h' && n.data[n.len - 2] == '.';
                b32 source = (n.len > 2 && n.data[n.len - 1] == 'c' && n.data[n.len - 2] == '.') ||
                             (n.len > 4 && str8_equal(str8(n.data + n.len - 4, 4), STR8_LIT(".cpp")));
                if (entries[i].is_dir || (pass == 0 ? !header : !source)) continue;
                String8 text;
                if (os_read_small_file(&arena, str8_fmt(&arena, "%S\\%S", dirs[d], n), MB(16), &text) == OS_FILE_OK) {
                    map_index_file(&arena, idents, d ? str8_fmt(&arena, "shaders (%S)", n) : str8_copy(&arena, n), text);
                }
            }
        }
    }
    // The section parts and the symbols.
    typedef struct MapPart { u32 sec; u64 start, len; String8 name; } MapPart;
    MapPart *parts = PUSH_ARRAY(&arena, MapPart, 256);
    i32 part_count = 0;
    MapSym *syms = PUSH_ARRAY(&arena, MapSym, 65536);
    i64 sym_count = 0;
    for (i64 i = 0; i < map.len;) {
        i64 end = i;
        while (end < map.len && map.data[end] != '\n') end++;
        String8 line = str8(map.data + i, end - i);
        i = end + 1;
        i64 at = 0;
        String8 addr = map_token(line, &at);
        if (addr.len != 13 || addr.data[4] != ':') continue;
        u32 sec = (u32)map_hex(str8(addr.data, 4));
        u64 off = map_hex(str8(addr.data + 5, 8));
        String8 second = map_token(line, &at);
        if (second.len == 9 && second.data[8] == 'H') { // the section table: start, length, name, class
            String8 name = map_token(line, &at);
            if (part_count < 256) parts[part_count++] = (MapPart){ sec, off, map_hex(str8(second.data, 8)), name };
            continue;
        }
        if (sec == 0 || !second.len || second.data[0] == '$') continue; // absolute symbols, local labels
        String8 rest = map_token(line, &at), obj = rest;
        for (String8 tok = map_token(line, &at); tok.len; tok = map_token(line, &at)) obj = tok;
        if (sym_count < 65536) syms[sym_count++] = (MapSym){ sec, off, second, obj };
    }
    // Sorted by address (insertion sort over runs that are mostly in order: publics, then statics).
    for (i64 i = 1; i < sym_count; i++) {
        MapSym x = syms[i];
        i64 j = i;
        while (j > 0 && (syms[j - 1].sec > x.sec || (syms[j - 1].sec == x.sec && syms[j - 1].off > x.off))) {
            syms[j] = syms[j - 1];
            j--;
        }
        syms[j] = x;
    }
    MapRow *rows = PUSH_ARRAY(&arena, MapRow, 512);
    i32 row_count = 0;
    typedef struct MapBig { String8 name, module, part; u64 size; } MapBig;
    MapBig big[24] = { 0 };
    u64 unknown = 0;
    for (i32 p = 0; p < part_count; p++) {
        MapPart *part = &parts[p];
        u64 covered_from = part->start + part->len; // the first symbol inside the part
        for (i64 k = 0; k < sym_count; k++) {
            MapSym *sym = &syms[k];
            if (sym->sec != part->sec || sym->off < part->start || sym->off >= part->start + part->len) continue;
            covered_from = MIN(covered_from, sym->off);
            u64 next = part->start + part->len;
            for (i64 j = k + 1; j < sym_count; j++) {
                if (syms[j].sec != sym->sec) break;
                if (syms[j].off > sym->off) {
                    next = MIN(next, syms[j].off);
                    break;
                }
            }
            u64 size = next - sym->off;
            if (k + 1 < sym_count && syms[k + 1].sec == sym->sec && syms[k + 1].off == sym->off) size = 0; // folded (ICF): counted once
            String8 module;
            if (map_contains(sym->obj, "libcmt") || map_contains(sym->obj, "libvcruntime") || map_contains(sym->obj, "libucrt") ||
                map_contains(sym->obj, "LIBCMT")) {
                module = STR8_LIT("CRT (static)");
            } else if (map_contains(sym->obj, "dwrite.obj")) {
                module = STR8_LIT("win32_dwrite.cpp");
            } else if (map_contains(sym->obj, ".lib:") || str8_starts_with(sym->name, STR8_LIT("__imp_"))) {
                module = STR8_LIT("imports");
            } else if (str8_starts_with(sym->name, STR8_LIT("??_C@"))) {
                module = STR8_LIT("string literals");
            } else if (str8_starts_with(sym->name, STR8_LIT("__real@")) || str8_starts_with(sym->name, STR8_LIT("__xmm@")) ||
                       str8_starts_with(sym->name, STR8_LIT("__ymm@"))) {
                module = STR8_LIT("float and SSE constants");
            } else if (map_contains(sym->obj, "ronin.obj")) {
                String8 base = sym->name;
                for (i64 c = 0; c < base.len; c++) {
                    if (base.data[c] == '$' || base.data[c] == '.') {
                        base.len = c;
                        break;
                    }
                }
                module = map_lookup(idents, base);
                if (!module.len) {
                    module = STR8_LIT("ronin.obj, not found in src");
                    unknown += size;
                }
            } else {
                module = STR8_LIT("linker");
            }
            map_add(map_row(rows, &row_count, module), part->name, size);
            if (!str8_starts_with(part->name, STR8_LIT(".text")) && !str8_starts_with(part->name, STR8_LIT(".bss"))) {
                for (i32 b = 0; b < (i32)ARRAY_COUNT(big); b++) {
                    if (size <= big[b].size) continue;
                    for (i32 m = (i32)ARRAY_COUNT(big) - 1; m > b; m--) big[m] = big[m - 1];
                    big[b] = (MapBig){ sym->name, module, part->name, size };
                    break;
                }
            }
        }
        if (covered_from > part->start) { // bytes before the first symbol: the part as a whole
            String8 n = part->name;
            String8 module = str8_starts_with(n, STR8_LIT(".pdata")) || str8_starts_with(n, STR8_LIT(".xdata")) ? STR8_LIT("unwind tables")
                           : str8_starts_with(n, STR8_LIT(".idata")) ? STR8_LIT("imports")
                           : str8_starts_with(n, STR8_LIT(".rsrc")) ? STR8_LIT("resources")
                           : str8_starts_with(n, STR8_LIT(".CRT")) || str8_starts_with(n, STR8_LIT(".rtc")) || str8_starts_with(n, STR8_LIT(".00cfg")) ||
                                     str8_starts_with(n, STR8_LIT(".tls")) || str8_starts_with(n, STR8_LIT(".fptable")) || str8_starts_with(n, STR8_LIT(".gfids"))
                               ? STR8_LIT("CRT (static)")
                           : str8_starts_with(n, STR8_LIT(".rdata$zzzdbg")) || str8_starts_with(n, STR8_LIT(".rdata$voltmd")) ||
                                     str8_starts_with(n, STR8_LIT(".rdata$r")) || str8_starts_with(n, STR8_LIT(".edata"))
                               ? STR8_LIT("linker (debug directory, metadata)")
                               : str8_fmt(&arena, "section part %S", n);
            map_add(map_row(rows, &row_count, module), n, covered_from - part->start);
        }
    }
    // Largest first (code + read-only + initialized data: what takes room in the file).
    for (i32 i = 1; i < row_count; i++) {
        MapRow x = rows[i];
        u64 xs = x.code + x.rdata + x.data + x.pdata;
        i32 j = i;
        while (j > 0 && rows[j - 1].code + rows[j - 1].rdata + rows[j - 1].data + rows[j - 1].pdata < xs) {
            rows[j] = rows[j - 1];
            j--;
        }
        rows[j] = x;
    }
    u64 total = 0;
    for (i32 i = 0; i < row_count; i++) total += rows[i].code + rows[i].rdata + rows[i].data + rows[i].pdata;
    LOG("map-report: %S: %d symbols, %d section parts; %U bytes in sections (code, read-only data, initialized data, unwind "
        "tables; .bss takes no room in the file)", map_path, (i32)sym_count, part_count, total);
    LOG("map-report: | module | total | code | read-only data | data | unwind | bss |");
    for (i32 i = 0; i < row_count; i++) {
        MapRow *r = &rows[i];
        LOG("map-report: | %S | %U | %U | %U | %U | %U | %U |", r->module, r->code + r->rdata + r->data + r->pdata, r->code, r->rdata,
            r->data, r->pdata, r->bss);
    }
    LOG("map-report: the largest data symbols:");
    for (i32 b = 0; b < (i32)ARRAY_COUNT(big) && big[b].size; b++) {
        LOG("map-report:   %U bytes %S (%S, %S)", big[b].size, big[b].name, big[b].module, big[b].part);
    }
    LOG("map-report: %U bytes in ronin.obj symbols not found in src", unknown);
    os_release(arena.base);
    return 0;
}

// --fuzz-seeds (one worker process's share): every group that takes the seed, for the seeds first, first +
// step, ... below end (not the scanner and the project search: their seed only shapes trees on disk). Stops
// at the first seed that fails.
i32 test_fuzz_seeds(u64 first, u64 end, u64 step, String8 tmp_dir) {
    static const struct { const char *name; b32 (*run)(Test *, u64); } groups[] = {
        { "buffer fuzz", test_fuzz }, { "markers", test_markers }, { "marker batch", test_marker_batch },
        { "line states", test_line_states }, { "columns", test_columns }, { "mc undo", test_mc_undo },
        { "mc fuzz", test_mc_fuzz }, { "view fuzz", test_view_fuzz }, { "window tree", test_window_tree },
        { "undo buffer", test_undo_buffer }, { "undo commands", test_undo_commands }, { "syntax", test_syntax },
        { "search", test_search }, { "undo fuzz", test_undo_fuzz }, { "kbd", test_kbd }, { "config", test_config },
        { "fuzzy", test_fuzzy }, { "grep file", test_grep_file }, { "modal", test_modal_fuzz },
    };
    Test t = { 0 };
    t.arena = arena_create(GB(4));
    t.tmp_dir = tmp_dir;
    syntax_init();
    u64 group_us[ARRAY_COUNT(groups)] = { 0 };
    u64 t0 = os_time_us(), done = 0, last = first;
    LOG("fuzz-seeds: seeds %U, %U, ... below %U", first, first + step, end);
    for (u64 seed = first; seed < end && !t.failures; seed += step) {
        for (i32 g = 0; g < (i32)ARRAY_COUNT(groups) && !t.failures; g++) {
            u64 g0 = os_time_us();
            groups[g].run(&t, seed);
            arena_reset(&t.arena);
            group_us[g] += os_time_us() - g0;
            if (t.failures) LOG("fuzz-seeds: FAIL at seed %U (0x%X) in %s", seed, seed, groups[g].name);
        }
        if (!t.failures) done++;
        last = seed;
    }
    for (i32 g = 0; g < (i32)ARRAY_COUNT(groups); g++) {
        LOG("fuzz-seeds: %s: %U ms in all, %U ms a seed", groups[g].name, group_us[g] / 1000, done ? group_us[g] / 1000 / done : 0);
    }
    LOG("fuzz-seeds: %s, %U seeds passed (%U to %U, step %U), %U ms", t.failures ? "FAIL" : "PASS", done, first, last, step,
        (os_time_us() - t0) / 1000);
    os_release(t.arena.base);
    return t.failures;
}

#endif // RONIN_DEV
