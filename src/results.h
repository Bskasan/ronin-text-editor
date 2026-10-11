// results.h — results buffers and their locations: the project-wide search (*search*), occur (*occur*),
// and build errors in Phase 13. Part of the app: results.c is included after finder.c and uses App
// directly.
//
// A results buffer is read-only, written through inhibit_read_only, and owns a list of locations (a file
// or a buffer, a line, a column, a length). Its text: a header line, a blank line, one line per location
// ("path:line:col: text", 1-based line and byte column; occur: "line:col: text"), then a progress line
// that becomes the summary. Location i is on line first_line + i. next-error and previous-error walk the
// locations of the most recent results buffer from any window; its keymap ([keys search-results]) has
// RET, n, p, g and q.
//
// The project-wide search fills the buffer strictly in the order of its file list, whatever the number
// of workers or their timing: a file's records are held until it is done and every earlier file is in the
// buffer (emit_next is the low-water mark over the files the workers are on), a modified buffer is
// searched from memory in its file's place, and search_max_results keeps the first N matches in that
// order. The workers may claim files only below emit_next + RESULTS_CLAIM_AHEAD, and none at all while
// the held records pass RESULTS_HELD_MAX, so what is held stays bounded. The same search on the same files
// gives the same text and the same next-error order every time.

#ifndef RESULTS_H
#define RESULTS_H

#define RESULTS_MAX 4                // results buffers at once (*search*, *occur*; *compilation* in Phase 13)
#define RESULTS_CLAIM_AHEAD 1024     // files the workers may claim past the first one not in the buffer
#define RESULTS_HELD_MAX MB(16)      // records held for files not in the buffer yet: no new claims above it
#define RESULTS_DYING_MAX 16         // cancelled searches still ending
#define RESULTS_LINES_PER_CHECK 64   // lines written between two looks at the clock
#define RESULTS_FLUSH_BYTES KB(256)  // added lines go into the buffer at the end of the frame's work, or at this size

typedef enum ResultsKind {
    RESULTS_PROJECT, // project-search: files under a root
    RESULTS_OCCUR,   // occur: one buffer
} ResultsKind;

typedef struct Location {
    u32 file;            // its entry in the file table
    i32 col;             // 0-based byte column
    i64 line;            // 1-based
    i64 len;             // bytes
    u16 path_len;        // the path's bytes at the start of the line (0 for occur)
    u16 num_len;         // "line:col" after it
    u16 match_off;       // the match in the shown text (after the "path:line:col: " prefix)
    u16 match_shown;
} Location;

// A list entry of a project search on the main thread: its records wait here until every earlier file
// is in the buffer.
enum { RESULTS_FILE_PENDING, RESULTS_FILE_DONE, RESULTS_FILE_MEMORY };

typedef struct ResultsFile {
    u8 state;            // RESULTS_FILE_*
    u8 kind;             // GrepFileKind of its DONE record
    u32 head, tail;      // its held records: offsets + 1 in the held arena (0 = none)
} ResultsFile;

// A buffer with unsaved changes under the root, at the start: searched from memory instead of its file.
typedef struct ResultsCandidate {
    Buffer *buffer;      // NULL once killed
    String8 rel;         // relative to the root, '/' separators
    b32 listed;          // its file is in the list (searched in its place), else after the list
} ResultsCandidate;

typedef struct Results {
    Buffer *buffer;      // NULL: a free slot
    ResultsKind kind;
    Arena arena;         // the query, the root, the file table, the candidates (reset by every run)
    Arena loc_arena;     // Location[]
    Arena pend;          // the lines added this frame, written with one insert (results_flush)
    Location *locs;
    i64 count;
    i64 current;         // next-error's position: -1 before the first location
    i64 first_line;      // the 0-based line of location 0
    i64 tail;            // the start of the last line (the progress line, then the summary)
    String8 query, root; // root: the project (project-search) or the buffer's name (occur)
    b32 fold;
    String8 *files;      // the file table: relative paths (project) or the buffer's name (occur)
    i32 file_count, file_cap;
    b32 point_set;       // point was put on the first location
    // The run.
    b32 running;
    u64 start_us, end_us;
    i64 found;           // locations written
    i64 files_seen;      // list files done, in any order (progress)
    i64 files_searched, files_with_matches, binary, unreadable, from_memory;
    b32 limited;         // stopped at search_max_results
    b32 incomplete;      // the project's scan stopped before the list was complete
    i64 bytes;
    String8 progress;    // the last progress line written (in progress_buf)
    u8 progress_buf[160];
    // The project search.
    GrepSearch *grep;
    Project *follow;     // the project whose index feeds the list (NULL: the list is complete)
    ProjectIndex *source;
    i64 fed;             // index entries copied into the list
    Arena state_arena;   // ResultsFile per list entry
    ResultsFile *state;
    i32 emit_next;       // the first list entry not in the buffer yet
    i32 list_count;      // entries in the list
    b32 list_done;
    Arena held[2];       // records of files not in the buffer yet (two, for compaction)
    i32 held_slot;
    u64 held_live;       // bytes of records not written yet
    ResultsCandidate *cands; // sorted by rel
    i32 cand_count;
    i32 after_next;      // the next unlisted candidate searched after the list
    // A buffer searched on the main thread (a MEMORY entry, an unlisted candidate, occur).
    Buffer *mem;
    b32 mem_active;
    String8 mem_path;    // its path in the results (relative to the root; empty for occur)
    i64 mem_found_before; // r->found when it started (files with matches)
    Search msearch;
#if TEAL_DEV
    i32 dev_max_ahead;   // the most files claimed past emit_next seen
    i32 dev_held_back;   // times the claim limit was not raised (held records over the limit)
#endif
} Results;

#endif // RESULTS_H
