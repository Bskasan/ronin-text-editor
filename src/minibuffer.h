// minibuffer.h — the minibuffer: prompts that read a line (or a key) from the user, with a
// vertical list of completion candidates above it. Plain C, headless.

#ifndef MINIBUFFER_H
#define MINIBUFFER_H

// ---------------------------------------------------------------------------
// Candidates and matching. The matcher is the only code that knows how input matches a candidate:
// case-insensitive; the input's space-separated terms must all occur as substrings; a candidate
// that starts with the first term ranks before the other matches, and one equal to the whole input
// ranks first. Phase 11 replaces it with fuzzy scoring.
//
// Case folding keeps byte offsets: a character whose folded form has another UTF-8 length is kept
// as it is, so a match position in the folded text is the same position in the shown text.

#define MATCH_MAX_TERMS 16

enum {
    CANDIDATE_DIR = 1 << 0, // find-file: a directory (shown with a trailing slash)
};

typedef struct Candidate {
    String8 text;       // shown, and the result when it is accepted
    String8 folded;     // text, case-folded once when the set is built
    String8 annotation; // shown right-aligned (M-x: the key binding); may be empty
    u32 flags;          // CANDIDATE_*
    u8 score;           // the last match_rank: MATCH_*
} Candidate;

typedef enum MatchScore {
    MATCH_NONE,
    MATCH_SUBSTRING, // every term occurs (also: the empty input matches everything)
    MATCH_PREFIX,    // ... and the candidate starts with the first term
    MATCH_EXACT,     // the candidate is the whole input
} MatchScore;

typedef struct MatchQuery {
    String8 whole; // the folded input
    String8 terms[MATCH_MAX_TERMS];
    i32 count;     // terms past MATCH_MAX_TERMS are ignored
} MatchQuery;

typedef struct MatchSpan {
    i64 start, end; // byte offsets in the candidate's text
} MatchSpan;

String8    match_fold(Arena *arena, String8 s); // the folded copy (same length)
MatchQuery match_query(Arena *arena, String8 input);
MatchScore match_score(MatchQuery *q, Candidate *c);
// The matched substrings of a candidate, for drawing: each term's first occurrence (the prefix for
// the first term of a prefix match). Returns the count.
i32        match_spans(MatchQuery *q, Candidate *c, MatchSpan *out, i32 cap);
// Scores every candidate and writes the indices of the matches to `out`: exact matches first, then
// prefix matches, then the others, each group in the candidates' own order. Returns the count.
i64        match_rank(MatchQuery *q, Candidate *cands, i64 count, i32 *out);

#endif // MINIBUFFER_H
