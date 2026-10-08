// minibuffer.c — see minibuffer.h.

// ---------------------------------------------------------------------------
// Matching

String8 match_fold(Arena *arena, String8 s) {
    u8 *out = PUSH_ARRAY(arena, u8, s.len);
    for (i64 i = 0; i < s.len;) {
        u8 b = s.data[i];
        if (b < 0x80) {
            out[i++] = b >= 'A' && b <= 'Z' ? (u8)(b + 32) : b;
            continue;
        }
        i64 advance;
        u32 c = utf8_decode(s.data + i, s.len - i, &advance);
        u8 lower[4];
        i64 n = utf8_encode(unicode_lower(c), lower);
        if (n == advance && c != UTF_REPLACEMENT) memcpy(out + i, lower, (size_t)n);
        else memcpy(out + i, s.data + i, (size_t)advance); // keeps the length (and invalid bytes)
        i += advance;
    }
    return str8(out, s.len);
}

MatchQuery match_query(Arena *arena, String8 input) {
    MatchQuery q = { 0 };
    q.whole = match_fold(arena, input);
    for (i64 i = 0; i < q.whole.len && q.count < MATCH_MAX_TERMS;) {
        while (i < q.whole.len && q.whole.data[i] == ' ') i++;
        i64 start = i;
        while (i < q.whole.len && q.whole.data[i] != ' ') i++;
        if (i > start) q.terms[q.count++] = str8(q.whole.data + start, i - start);
    }
    return q;
}

// The first occurrence of `term` in `s`, or -1.
static i64 match_find(String8 s, String8 term) {
    if (!term.len) return 0;
    u8 first = term.data[0];
    for (i64 i = 0, last = s.len - term.len; i <= last; i++) {
        if (s.data[i] == first && memcmp(s.data + i, term.data, (size_t)term.len) == 0) return i;
    }
    return -1;
}

MatchScore match_score(MatchQuery *q, Candidate *c) {
    for (i32 k = 0; k < q->count; k++) {
        if (match_find(c->folded, q->terms[k]) < 0) return MATCH_NONE;
    }
    if (q->count && str8_equal(c->folded, q->whole)) return MATCH_EXACT;
    if (q->count && c->folded.len >= q->terms[0].len && memcmp(c->folded.data, q->terms[0].data, (size_t)q->terms[0].len) == 0) {
        return MATCH_PREFIX;
    }
    return MATCH_SUBSTRING;
}

i32 match_spans(MatchQuery *q, Candidate *c, MatchSpan *out, i32 cap) {
    i32 n = 0;
    for (i32 k = 0; k < q->count && n < cap; k++) {
        i64 at = match_find(c->folded, q->terms[k]);
        if (at >= 0) out[n++] = (MatchSpan){ at, at + q->terms[k].len };
    }
    return n;
}

i64 match_rank(MatchQuery *q, Candidate *cands, i64 count, i32 *out) {
    i64 found[MATCH_EXACT + 1] = { 0 };
    for (i64 i = 0; i < count; i++) {
        MatchScore s = match_score(q, &cands[i]);
        cands[i].score = (u8)s;
        found[s]++;
    }
    // Stable: one pass per score, best first.
    i64 n = 0;
    for (i32 s = MATCH_EXACT; s > MATCH_NONE; s--) {
        if (!found[s]) continue;
        for (i64 i = 0; i < count; i++) if (cands[i].score == s) out[n++] = (i32)i;
    }
    return n;
}
