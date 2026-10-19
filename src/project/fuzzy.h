/* ======================================================================
   ronin: fuzzy matching of paths and the sliced ranking over a project index

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// fuzzy matching of project paths: subsequences, smart case, a score that prefers the file
// name, segment starts and consecutive characters; ranking over a ProjectIndex in slices within the
// frame budget, keeping the best FUZZY_TOP. Plain C, headless.

#ifndef FUZZY_H
#define FUZZY_H

#define FUZZY_TERMS 16       // space-separated terms; every one must match
#define FUZZY_TOP 1000       // results kept, best first
#define FUZZY_SLICE 512      // candidates between two looks at the clock (a slice of the costliest queries: ~75 us)
#define FUZZY_EXACT_NAME (1 << 20) // added when the file name is the whole (single-term) query

typedef struct FuzzyTerm {
    u32 *cps;  // its characters (folded unless the query is exact)
    i32 count;
    b32 slash; // it has a '/': matched over the whole path only
} FuzzyTerm;

typedef struct FuzzyQuery {
    FuzzyTerm terms[FUZZY_TERMS];
    i32 count;
    b32 exact;  // smart case: an uppercase letter anywhere makes the match case-sensitive
    u64 mask;   // project_mask of every term: a candidate without these characters cannot match
} FuzzyQuery;

#define FUZZY_NO_MATCH (-2147483647 - 1)

FuzzyQuery fuzzy_query(Arena *arena, String8 input);
// The score of a relative path whose file name starts at name_off, or FUZZY_NO_MATCH. With `spans`, the
// matched characters as merged byte ranges (at most cap; the count in *n).
i32 fuzzy_score(FuzzyQuery *q, String8 path, i64 name_off, MatchSpan *spans, i32 cap, i32 *n);

typedef struct FuzzyHit {
    i32 score;
    u32 len;   // the path's length: shorter first on equal scores
    u32 index; // then the index order
} FuzzyHit;

// Ranking over an index. A pass looks at "the candidates still possible": a survivor list (those that
// matched the previous input, in index order) for [0, covered), then every entry from `covered` to the
// index's current end (entries added during the pass extend it). Narrowing (the input only grew) keeps
// the previous pass's survivors, also those of a pass that was cut short.
typedef struct FuzzyRank {
    ProjectIndex *x;
    FuzzyQuery q;
    Arena query_arena;
    Arena lists[2];        // u32 survivor lists: `from` is read, the other written
    u32 *from;
    i64 from_count, covered;
    i32 to_list;           // the list being written
    i64 to_count;
    i64 at_from;           // progress in the survivor list
    i64 at;                // progress in [covered, file_count)
    u32 *copy_src;         // narrowing: the rest of the previous list, still to be appended to `from`
    i64 copy_left;
    b32 started;           // a query is set
    i64 total;             // matches so far in this pass
    FuzzyHit heap[FUZZY_TOP]; // the best so far: a min-heap, the worst at the root
    i32 heap_count;
    u32 rows[FUZZY_TOP];   // the heap sorted best first (fuzzy_rank_rows)
    i32 row_count;
    b32 rows_stale;
    u64 sort_us;           // the last sort of the rows: the work leaves that much of its deadline for it
    // Assembling paths: the directory part is copied once per directory.
    u8 path[PROJECT_PATH_MAX];
    i64 path_dir;          // the directory in `path`, -1 none
    i64 path_dir_len;      // its bytes plus the '/'
    u64 dev_scored;        // candidates scored (tests, the bench)
} FuzzyRank;

void fuzzy_rank_init(FuzzyRank *r);
void fuzzy_rank_destroy(FuzzyRank *r);
// A new input: narrow = it only grew since the last call on the same index (characters appended).
void fuzzy_rank_begin(FuzzyRank *r, ProjectIndex *x, String8 input, b32 narrow);
// Scores until `deadline` (os_time_us; 0 = no clock) or `budget` candidates (> 0); true when every entry
// of the index so far has been looked at.
b32  fuzzy_rank_work(FuzzyRank *r, u64 deadline, i64 budget);
b32  fuzzy_rank_done(FuzzyRank *r);
void fuzzy_rank_finish(FuzzyRank *r);  // the rest of the pass now
i32  fuzzy_rank_rows(FuzzyRank *r);    // sorts the kept hits into rows[] (only when they changed); returns their count
i64  fuzzy_rank_path(FuzzyRank *r, u32 index, u8 *buf, i64 cap); // a file's relative path

#endif // FUZZY_H
