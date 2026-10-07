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

i32 test_run(u64 seed) {
    Test t = { 0 };
    t.arena = arena_create(GB(4));
    LOG("test: seed 0x%X (override with --seed)", seed);
    u64 t0 = os_time_us();

    test_fuzz(&t, seed);
    arena_reset(&t.arena);
    test_capacity(&t);
    arena_reset(&t.arena);
    test_read_only(&t);
    arena_reset(&t.arena);

    LOG("test: %s, %d failure(s), %U ms", t.failures ? "FAIL" : "PASS", t.failures, (os_time_us() - t0) / 1000);
    os_release(t.arena.base);
    return t.failures;
}

#endif // TEAL_DEV
