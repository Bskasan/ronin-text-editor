/* ======================================================================
   ronin: literal text search over plain bytes and the gap buffer, resumable

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// literal text search, forward and backward, exact or case-folded. One engine for isearch,
// query-replace, occur and the project-wide search (grep.c). Plain C, headless.
//
// Two layers. The core works on plain byte ranges: a SearchPattern (the needle, folded when folding) and
// the functions that find it in one contiguous range (search_bytes_forward / _backward); grep.c runs it
// over a file's chunks. On top of it, a Search runs the pattern over a gap buffer's two segments in
// place: nothing is copied except a candidate that straddles the gap, which is compared through a window
// of at most the needle's length. A Search is resumable: search_run examines at most `budget` candidate
// start positions and returns, so a caller can spread a search over many frames; how much a slice is, is
// the caller's (the app slices by time). No allocation.
//
// Case folding is the matcher's (minibuffer.h): a character is replaced by its lowercase form only
// when that has the same UTF-8 length, so folding keeps byte lengths and a match is always exactly
// as long as the needle. (Turkish dotted / dotless i do not pair: İ -> i changes the length, and the
// default mapping of I is i, not ı.)

#ifndef SEARCH_H
#define SEARCH_H

#define SEARCH_NEEDLE_MAX 4096     // bytes of a search string
#define SEARCH_SLICE_BYTES KB(256) // candidate positions a caller examines per slice

// ---------------------------------------------------------------------------
// The core: a pattern and plain byte ranges.

typedef struct SearchPattern {
    u8 needle[SEARCH_NEEDLE_MAX]; // folded when `fold`
    i64 len;                      // 0: no pattern (empty or too long a needle)
    b32 fold;
    b32 ascii_fold;     // fast path: the first byte is an ASCII letter, matched as (b | 0x20) == first[0]
    u8 first[2];        // fast path: the bytes a match can start with
    i32 first_count;
    b32 check_boundary; // the needle starts with a continuation byte: a match must start on a character boundary
    // The second-byte filter: a candidate's next byte must be `second` too ((b | 0x20) == second for a
    // folded ASCII letter). On for exact needles of two bytes or more, and for folded ones whose first two
    // bytes are ASCII (a non-ASCII character never folds to an ASCII one).
    b32 second_on, second_fold;
    u8 second;
} SearchPattern;

// The pattern of `needle` (folded first when `fold`). False, with len 0, for an empty needle or one
// longer than SEARCH_NEEDLE_MAX.
b32 search_pattern_init(SearchPattern *pat, String8 needle, b32 fold);
// The first i in [i, end) where p[i] can start a match, or -1 (the first byte's filter only; bytes of p
// are read below `valid`, end <= valid).
i64 search_candidate_forward(const SearchPattern *pat, const u8 *p, i64 i, i64 end, i64 valid);
// The last i in [low, high] where p[i] can start a match, or -1 (bytes of p read below `valid`).
i64 search_candidate_backward(const SearchPattern *pat, const u8 *p, i64 low, i64 high, i64 valid);
// The `len` bytes at h equal the pattern (folded the same way when folding).
b32 search_pattern_equal(const SearchPattern *pat, const u8 *h);
// The bytes p[lo, hi) are valid. Forward: the smallest start c in [from, to - len] with a match at c;
// backward: the largest one. -1 when none. With check_boundary, c must start a character as far as the
// bytes in [lo, hi) tell (a sequence reaching past hi is treated as cut there).
i64 search_bytes_forward(const SearchPattern *pat, const u8 *p, i64 lo, i64 hi, i64 from, i64 to);
i64 search_bytes_backward(const SearchPattern *pat, const u8 *p, i64 lo, i64 hi, i64 from, i64 to);

// ---------------------------------------------------------------------------
// A search over a gap buffer.

typedef enum SearchStatus {
    SEARCH_RUNNING,   // more positions to examine
    SEARCH_FOUND,     // match_start / match_end
    SEARCH_NOT_FOUND, // no match in the range (also: an empty or too long needle)
} SearchStatus;

typedef struct Search {
    SearchPattern pat;
    u8 window[SEARCH_NEEDLE_MAX]; // a candidate straddling the gap, copied for the comparison
    b32 forward;
    i64 lo, hi;         // matches lie inside [lo, hi)
    i64 from;           // forward: the smallest start allowed; backward: the largest end allowed
    i64 next;           // the next start to examine (ascending forward, descending backward)
    i64 examined;       // positions examined since search_restart (progress)
    u64 edits;          // the buffer's edit_count when the search (re)started; a change restarts it
    SearchStatus status;
    i64 match_start, match_end;
} Search;

// Smart case: the string has an uppercase letter (a character whose lowercase form differs).
b32  search_has_upper(String8 s);
// Folds `len` bytes of `in` into `out` (the same length): every character to search_fold_char.
void search_fold_bytes(u8 *out, const u8 *in, i64 len);
// The folded form of one character, as the search compares it: lowercase when that keeps the length.
u32  search_fold_char(u32 c);
// The lead bytes a character folding to `f` can start with (f non-ASCII): f's own and its uppercase
// form's, when that folds back to f. Returns the count (1 or 2).
i32  search_first_bytes(u32 f, u8 out[2]);

// Starts a search for `needle` (folded first when `fold`). Forward from F: the match with the smallest
// start >= F. Backward from B: the match with the largest start whose end is <= B. Only matches inside
// [lo, hi) count (clamped to the buffer). False, with SEARCH_NOT_FOUND, for an empty needle or one
// longer than SEARCH_NEEDLE_MAX.
b32  search_begin(Search *s, Buffer *buf, String8 needle, b32 fold, b32 forward, i64 from, i64 lo, i64 hi);
// The same needle, a new direction, start and range.
void search_restart(Search *s, Buffer *buf, b32 forward, i64 from, i64 lo, i64 hi);
// Examines at most `budget` more start positions (>= 1). If the buffer was edited since the search
// started, it starts over from `from` (clamped).
SearchStatus search_run(Search *s, Buffer *buf, i64 budget);
// How much of the range is done, 0..100 (the echo area while a search runs).
i32  search_progress(Search *s);

#endif // SEARCH_H
