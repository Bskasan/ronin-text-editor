// grep.h — the project-wide search's worker side: files searched for a literal string on worker threads
// (CLAUDE.md has the threading rules). Plain C, headless: platform.h only. The app side (the results
// buffer, the order, the file list's source) is results.c.
//
// A GrepSearch lives at the start of its own reservation, so a worker abandoned at exit never touches
// freed memory. Its request (the pattern, the root, the files to skip) is set before grep_start and never
// changes. Its file list is append-only: the main thread appends relative paths and publishes their count
// (grep_list_publish); a worker only reads entries below the count it read, and they never change. Workers
// claim files in list order (an atomic compare-and-swap below min(published, claim_limit)), read each one
// in fixed chunks that overlap by the needle's length plus the shown context (GREP_KEEP_BEFORE before the
// next start, GREP_LINE_SHOW after the last accepted one), count lines while scanning,
// and hand their records to the main thread in batches through one locked queue (os_wake_main). A file is
// opened sharing read, write and delete and closed before the next one is claimed: a search never makes a
// save, a build or git fail.

#ifndef GREP_H
#define GREP_H

#define GREP_CHUNK KB(256)        // text bytes added to a worker's window per step
#define GREP_PROBE KB(8)          // a NUL in the first bytes: a binary file
#define GREP_CONTEXT 64           // bytes of a long line shown before its match
#define GREP_LINE_SHOW 200        // a longer line is cut around the match ("…" at each cut)
#define GREP_SHOW_CAP (GREP_LINE_SHOW + 8) // and "…" at both ends
// The overlap a window keeps: GREP_LINE_SHOW + 4 bytes before the next start (a line that short is shown
// whole; 3 bytes for a boundary check) and GREP_LINE_SHOW + the needle after it.
#define GREP_KEEP_BEFORE (GREP_LINE_SHOW + 4)
#define GREP_WINDOW_EXTRA (SEARCH_NEEDLE_MAX + 2 * GREP_LINE_SHOW + 64)
#define GREP_BATCH_BYTES KB(64)
#define GREP_BATCH_POOL 4         // batches in flight per worker; it waits for one to come back
#define GREP_WORKERS_MAX 16
#define GREP_SEND_US 10000        // a worker sends a batch at least this often while it has records

// ---------------------------------------------------------------------------
// One file (any thread): the scan over a reader, with fixed buffers.

typedef struct GrepMatch {
    i64 line;        // 1-based
    i64 col;         // 0-based byte offset in its line (shown 1-based)
    i64 len;         // bytes
    String8 text;    // the line as shown: without CR / LF, cut around the match when long
    i64 match_off;   // where the match starts in `text`
    i64 match_shown; // how many of its bytes `text` shows
} GrepMatch;

typedef enum GrepFileKind {
    GREP_FILE_TEXT,       // searched
    GREP_FILE_BINARY,     // a NUL in the first GREP_PROBE bytes: skipped
    GREP_FILE_UNREADABLE, // could not be opened or read
    GREP_FILE_MEMORY,     // a modified buffer visits it: the main thread searches the buffer instead
} GrepFileKind;

typedef struct GrepFileResult {
    GrepFileKind kind;
    i64 matches;
    i64 bytes;   // text bytes searched
    b32 stopped; // at max_matches, or cancelled, or the emit callback said stop
} GrepFileResult;

// A worker's buffers: the window (chunk + GREP_WINDOW_EXTRA), the raw input (the head and UTF-16 bytes),
// the shown text of a match.
typedef struct GrepScan {
    u8 *win;
    i64 win_cap;
    u8 *raw;
    i64 raw_cap;
    u8 show[GREP_SHOW_CAP];
    i64 chunk;
} GrepScan;

typedef OsFileStatus GrepReadFn(void *data, u8 *dst, i64 cap, i64 *got); // like os_file_read_some
typedef b32 GrepEmitFn(void *data, const GrepMatch *m);                  // false: stop this file

// The shown text of a match at w[c] (len bytes) into `out` (GREP_SHOW_CAP bytes): the whole line when it
// fits in GREP_LINE_SHOW bytes, otherwise up to GREP_CONTEXT bytes before the match and GREP_LINE_SHOW in
// all, cut on character boundaries with "…" at each cut, without its CR. ls: the line's start in w (-1:
// before w). w holds GREP_LINE_SHOW + 1 bytes from c on, or the rest of the text (at_end). Sets m->text,
// match_off and match_shown. (Files here; buffers searched on the main thread in results.c.)
void grep_show_text(u8 *out, const u8 *w, i64 wn, i64 ls, i64 c, i64 len, b32 at_end, GrepMatch *m);
// The buffers for chunks of `chunk` text bytes (tests use tiny ones), in `arena`.
void grep_scan_init(GrepScan *sc, Arena *arena, i64 chunk);
// Searches one file of `size` bytes read through `read`. A UTF-8 BOM is skipped; UTF-16 with a BOM (and an
// even size) is converted; otherwise a NUL in the first GREP_PROBE bytes makes it binary. Every match, at
// most max_matches, goes to `emit`; `cancel` (may be NULL) is checked before every chunk.
GrepFileResult grep_file(const SearchPattern *pat, GrepScan *sc, i64 size, GrepReadFn *read, void *read_data, i64 max_matches,
                         volatile i32 *cancel, GrepEmitFn *emit, void *emit_data);

// ---------------------------------------------------------------------------
// The search: workers over a file list.

enum { GREP_REC_MATCH = 1, GREP_REC_DONE = 2 };

// A record as grep_drain hands it over.
typedef struct GrepRecord {
    i32 kind;           // GREP_REC_*
    u32 file;           // its index in the list
    GrepMatch match;    // GREP_REC_MATCH (text points into the batch: copy it)
    GrepFileKind status; // GREP_REC_DONE
    i64 bytes;          // GREP_REC_DONE: text bytes searched
} GrepRecord;

typedef struct GrepBatch {
    struct GrepBatch *next;
    i32 worker;
    i64 used;
    b32 last; // the worker's final batch
    u8 data[GREP_BATCH_BYTES];
} GrepBatch;

struct GrepSearch;

typedef struct GrepWorker {
    struct GrepSearch *s;
    i32 index;
    Arena arena;        // its own: the scan's buffers, the batches, one path at a time
    OsEvent event;      // it waits on it: more files, a batch back, a cancel, a raised limit
    GrepBatch *free;    // batches back from the main thread (under s->lock)
    // The main thread's.
    OsThread thread;
    b32 started, ended; // ended: its last batch arrived and it was joined
} GrepWorker;

typedef struct GrepSearch {
    // The request: set before grep_start, read-only while the workers run.
    SearchPattern pat;
    String8 root;          // full path, no trailing separator
    i64 max_matches;       // per file (the main thread applies the search's cap in list order)
    String8 *skip;         // relative paths, sorted (grep_path_compare): modified buffers, searched from memory
    i32 skip_count;
    i32 worker_count;
    // The file list: entry i is `offsets[i]` into `paths` (a u16 length, then the bytes). Appended by the
    // main thread; entries below `published` never change.
    u8 *paths;
    u32 *offsets;
    // Shared.
    volatile i32 published;   // list entries the workers may read
    volatile i32 claimed;     // the next entry to take
    volatile i32 claim_limit; // no entry at or above it is claimed (the main thread's window)
    volatile i32 list_done;   // no more entries will be published
    volatile i32 cancel;
    OsLock lock;              // the ready queue and the workers' free lists
    GrepBatch *ready, *ready_last;
    GrepWorker workers[GREP_WORKERS_MAX];
    volatile i32 dev_stuck;     // dev: every worker waits while set, deaf to cancel (a hung file system)
    volatile i32 dev_hold;      // dev: holds below stand still while set (or until cancelled)
    i32 dev_hold_after;         // dev: a worker stands still after this many files (0 = never)
    b32 dev_hold_open;          // dev: each worker stands still with its file open after the first read (sharing tests)
    // The main thread's.
    Arena arena;              // this struct, the request's strings
    Arena path_arena, offset_arena;
    i32 count;                // entries appended (published or not)
    i32 workers_ended;
    b32 ended;                // every worker that started has ended and was joined
} GrepSearch;

i32  grep_path_compare(String8 a, String8 b); // ASCII case-insensitive, '\' = '/'; <0, 0, >0
// A search for `needle` under `root` with `workers` threads (1..GREP_WORKERS_MAX), in its own reservation.
GrepSearch *grep_create(String8 root, String8 needle, b32 fold, i64 max_matches, i32 workers);
void grep_skip(GrepSearch *s, String8 *rel, i32 count); // before grep_start: the relative paths to skip
b32  grep_start(GrepSearch *s);                          // false: no thread could start (free it)
void grep_list_add(GrepSearch *s, String8 rel);          // main thread: one more entry (not visible yet)
void grep_list_publish(GrepSearch *s, b32 done);         // main thread: the entries so far are visible; done: no more
void grep_set_claim_limit(GrepSearch *s, i32 limit);     // main thread
String8 grep_list_path(GrepSearch *s, i32 file);         // an entry below `count` (main) or `published` (worker)
void grep_cancel(GrepSearch *s);                         // any time
typedef void GrepRecordFn(void *data, const GrepRecord *r);
// Main thread: hands every record that arrived to `fn`, in each worker's order; gives the batches back;
// joins a worker whose last batch came (s->ended once all have). Returns the records handed over.
i64  grep_drain(GrepSearch *s, GrepRecordFn *fn, void *data);
// Joins the workers until the deadline (os_time_us): false when one is still running (stuck in the file system).
b32  grep_join(GrepSearch *s, u64 deadline_us);
void grep_free(GrepSearch *s); // only once it ended (or was joined)
i32  grep_auto_workers(void);  // search_workers = 0: from the processor count
#if RONIN_DEV
u64  grep_memory(GrepSearch *s);      // committed bytes of the search and its workers
void grep_dev_release(GrepSearch *s); // ends dev_hold and dev_stuck
#endif

#endif // GREP_H
