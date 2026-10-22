/* ======================================================================
   ronin: builds, the worker side: the process, its lines, error matchers

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// builds, the worker side: a command run as a child process (platform.h's processes), its output
// read on a worker thread (CLAUDE.md has the threading rules), cleaned, split into lines and matched for
// error locations, handed to the main thread in batches through one locked queue (os_wake_main). Plain C,
// headless: platform.h only. The app side (*compilation*, the commands) is results.c.
//
// A CompileRun lives at the start of its own reservation, so a worker abandoned at exit never touches freed
// memory. The worker owns the process from its start to its close: the main thread never waits for it.
// kill (compile_cancel) sets the cancel flag and the cancel event; the worker then ends the whole job.
//
// Each line, on the worker: CR dropped, ANSI escape sequences stripped, kept when it is valid UTF-8, else
// converted from the OEM code page; cut at COMPILE_LINE_MAX. A line that names a location (compile_match)
// counts only when its file exists (compile_resolve, cached); a line with a kind word and no location
// (compile_match_kind) is still an error or a warning, without a location; a tool's summary
// (compile_match_summary: "2 errors generated.") is marked as one, colored and not counted.

#ifndef COMPILE_H
#define COMPILE_H

#define COMPILE_LINE_MAX 60000      // a longer line is cut (offsets in a line fit in 16 bits)
#define COMPILE_READ_BYTES KB(64)
#define COMPILE_BATCH_BYTES KB(256)
#define COMPILE_BATCH_POOL 4        // batches in flight; the worker waits for one to come back
#define COMPILE_CACHE_CAP 4096      // file existence answers kept (then forgotten all at once)
#define COMPILE_TRUNCATED "[output truncated]"

typedef enum CompileKind {
    COMPILE_NONE,    // plain output
    COMPILE_ERROR,
    COMPILE_WARNING,
    COMPILE_INFO,    // note, info, "In file included from"
} CompileKind;

// What a line says. Offsets are into the line.
typedef struct CompileMatch {
    CompileKind kind;
    b32 located;          // a file and a line (a candidate: its file must exist)
    i64 line, col;        // 1-based; col 0: none given
    i32 path_off, path_len;
    i32 num_off, num_len; // "(12,5)", "12:5", "12,5"
    i32 kind_off, kind_len; // the kind word ("error", "fatal error", "Warning"); 0 length: none (bare "path:line:")
    i32 proj_off, proj_len; // MSBuild's project file in brackets at the end of the line (inside them)
    b32 summary;          // a tool's summary (compile_match_summary): colored, not counted
} CompileMatch;

// A location: MSVC "path(L)" / "path(L,C)" then ": error|warning|note|fatal error"; GCC / Clang
// "path:L:C: kind:", "path:L: kind:", bare "path:L:"; "In file included from path:L:" (info); Jai
// "path:L,C: Error|Warning|Info:"; tsc's "path:L:C - error"; MSBuild's "  N>" prefix first.
b32 compile_match(String8 line, CompileMatch *m);
// No location, but a kind: "LINK : fatal error LNK1104: ...", "x.obj : error LNK2019: ...", "error: ...",
// "ld: error: ...", "error MSB4019: ...".
b32 compile_match_kind(String8 line, CompileMatch *m);
// A tool's summary, no location: a count right before an error or warning word ("1 error generated.",
// "3 warnings and 1 error generated.", "0 Error(s)", "Found 2 errors.", "Compilation failed (1 error)").
// m gets the first count word. A number after the word ("errors: 3", make's "Error 1") does not count.
b32 compile_match_summary(String8 line, CompileMatch *m);
// The next count word of a summary at or after `from`: its offset (-1: none), its kind and length.
i64 compile_count_word(String8 line, i64 from, CompileKind *kind, i64 *len);

// File existence, cached (the worker's arena). Tests give a fake `exists`.
typedef b32 CompileExistsFn(void *data, Arena *arena, String8 path);
typedef struct CompileCache {
    Arena *arena;          // the strings and the table
    CompileExistsFn *exists;
    void *data;
    u64 *hashes;           // 0: empty
    String8 *paths;
    u8 *answers;
    i32 count;
    i64 asked;             // exists calls (tests)
} CompileCache;
void compile_cache_init(CompileCache *c, Arena *arena, CompileExistsFn *exists, void *data);
b32  compile_exists_fs(void *data, Arena *arena, String8 path); // the file system (any thread)
// A located match's file: the path as given when absolute; else, in this order, under the build directory,
// beside the line's MSBuild project file (`proj`, relative to dir when not absolute), under the project
// root (empty: none). The first that exists, '\' separators, in the cache's arena; empty: none.
String8 compile_resolve(CompileCache *c, String8 path, String8 dir, String8 proj, String8 root);
// The path *compilation* shows for a located line printed as `printed` and resolved to `file`: `file` relative
// to `root` (a part of `file`) when `printed` is absolute and the file is under the root (compared as
// Windows does: ASCII case and both separators); empty: the line is shown as printed.
String8 compile_shown_path(String8 printed, String8 file, String8 root);

// The cleaning of one line (CR, ANSI, OEM) into `arena`.
String8 compile_clean(Arena *arena, String8 raw);
b32     compile_utf8_valid(String8 s);

// Lines across reads: bytes in, each complete line out (without its LF), a line over COMPILE_LINE_MAX cut.
typedef void CompileLineOutFn(void *data, String8 raw);
typedef struct CompileSplit {
    u8 *buf;
    i64 len;
} CompileSplit;
void compile_split_init(CompileSplit *s, Arena *arena);
void compile_split_feed(CompileSplit *s, const u8 *p, i64 n, CompileLineOutFn *fn, void *data);
void compile_split_end(CompileSplit *s, CompileLineOutFn *fn, void *data); // the last line without a newline

// ---------------------------------------------------------------------------
// The run

typedef struct CompileBatch {
    struct CompileBatch *next;
    i64 used;
    b32 last; // the worker's final batch: the run's result is set
    u8 data[COMPILE_BATCH_BYTES];
} CompileBatch;

// A line as the main thread reads it from a batch (text and file point into the batch).
typedef struct CompileLine {
    String8 text;
    CompileKind kind;
    b32 located;
    b32 summary;           // a tool's summary: its count words colored, not counted
    String8 file;          // located: the resolved path
    i64 line, col;
    u16 path_off, path_len, num_off, num_len, kind_off, kind_len;
} CompileLine;

#if RONIN_DEV
typedef enum CompileDevEnd { // why the read loop ended
    COMPILE_DEV_END_NONE,        // not reached (no process)
    COMPILE_DEV_END_FLAG,        // the cancel flag, seen at the loop's top
    COMPILE_DEV_END_SEND,        // cancelled while waiting for a batch to come back
    COMPILE_DEV_END_READ_CANCEL, // os_process_read: OS_PROCESS_CANCELLED
    COMPILE_DEV_END_EXITED,      // ... OS_PROCESS_EXITED
    COMPILE_DEV_END_ERROR,       // ... OS_PROCESS_ERROR or OS_PROCESS_TIMEOUT
} CompileDevEnd;

#define COMPILE_DEV_PROCS 8

// The stages of a run's end (#25): os_time_us at each, 0 when not reached.
typedef struct CompileDevStages {
    u64 entered, started, read_end, killed, waited, closed, sent;
    CompileDevEnd end;
    b32 wait_ok;                                // os_process_wait saw cmd.exe end (not its timeout)
    OsDevProcessId procs[COMPILE_DEV_PROCS];    // the job's processes just before the kill
    i32 proc_count;                             // ... recorded in procs; negative: the listing failed (OS_DEV_JOB_*_FAILED)
    i32 proc_total;                             // ... the job held (more than COMPILE_DEV_PROCS: not all recorded)
} CompileDevStages;
#endif

typedef struct CompileRun {
    // The request: set before compile_start, read-only while the worker runs.
    String8 command, dir, root; // dir: where it runs; root: the project (path resolution), may be empty
    i64 max_output;             // bytes of output kept; past it one COMPILE_TRUNCATED line, the rest read and dropped
#if RONIN_DEV
    // Tests (#25): faults injected into a run's end, part of the request (set before compile_start).
    u32 dev_kill_delay_ms;      // a wait before os_process_kill
    b32 dev_skip_kill;          // os_process_kill is not called (the job's close ends the tree)
    i32 dev_listing_fault;      // given to os_dev_job_processes (OS_DEV_JOB_FAULT_*)
#endif
    // Shared.
    volatile i32 cancel;
    OsEvent cancel_event;       // set only to cancel (the process read waits on it)
    OsEvent event;              // a batch came back, or a cancel
    OsLock lock;                // the ready queue and the free list
    CompileBatch *ready, *ready_last, *free;
    // The result, written by the worker before its last batch is queued (read once that batch arrived).
    OsFileStatus start_status;
    u32 exit_code;
    b32 killed, truncated;
    u64 start_us, end_us;
    i64 bytes;                  // output read
    u32 pid;
#if RONIN_DEV
    CompileDevStages dev;       // as the result: written before the last batch is queued under `lock`, read after the
                                // main thread took that batch and joined the worker (c->ended)
#endif
    // The worker's own.
    Arena arena;
    Arena temp;                 // one line's cleaning at a time
    // The main thread's.
    OsThread thread;
    b32 started, ended;         // ended: its last batch was given back and it was joined
} CompileRun;

CompileRun *compile_create(String8 command, String8 dir, String8 root, i64 max_output);
b32  compile_start(CompileRun *c);   // false: no thread (free it)
void compile_cancel(CompileRun *c);  // any time: the job is killed
// Main thread: the batches that arrived, oldest first; each goes back with compile_give_back once read.
CompileBatch *compile_take(CompileRun *c);
b32  compile_next_line(CompileBatch *b, i64 *at, CompileLine *out); // the record at *at (advanced); false at the end
// Main thread: a batch read; the last one joins the worker (c->ended).
void compile_give_back(CompileRun *c, CompileBatch *b);
// Joins the worker until the deadline (os_time_us): false when it is still running.
b32  compile_join(CompileRun *c, u64 deadline_us);
void compile_free(CompileRun *c); // only once it ended (or was joined)

#endif // COMPILE_H
