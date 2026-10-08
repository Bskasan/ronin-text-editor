// search.h — literal text search over a buffer, forward and backward, exact or case-folded. One
// engine for isearch, query-replace and (Phase 12) project-wide search. Plain C, headless.
//
// It reads the gap buffer's two segments in place: nothing is copied except a candidate that
// straddles the gap, which is copied into a window of at most the needle's length (in the Search).
// It is resumable: search_run examines at most `budget` candidate start positions and returns, so a
// caller can spread a search over many frames; how much a slice is, is the caller's (the app slices
// by time). No allocation.
//
// Case folding is the matcher's (minibuffer.h): a character is replaced by its lowercase form only
// when that has the same UTF-8 length, so folding keeps byte lengths and a match is always exactly
// as long as the needle. (Turkish dotted / dotless i do not pair: İ -> i changes the length, and the
// default mapping of I is i, not ı.)

#ifndef SEARCH_H
#define SEARCH_H

#define SEARCH_NEEDLE_MAX 4096     // bytes of a search string
#define SEARCH_SLICE_BYTES KB(256) // candidate positions a caller examines per slice

typedef enum SearchStatus {
    SEARCH_RUNNING,   // more positions to examine
    SEARCH_FOUND,     // match_start / match_end
    SEARCH_NOT_FOUND, // no match in the range (also: an empty or too long needle)
} SearchStatus;

typedef struct Search {
    u8 needle[SEARCH_NEEDLE_MAX]; // folded when `fold`
    i64 len;
    b32 fold, forward;
    b32 ascii_fold;     // fast path: the first byte is an ASCII letter, matched as (b | 0x20) == first[0]
    u8 first[2];        // fast path: the bytes a match can start with
    i32 first_count;
    b32 check_boundary; // the needle starts with a continuation byte: a match must start on a character boundary
    u8 window[SEARCH_NEEDLE_MAX]; // a candidate straddling the gap, copied for the comparison
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
