// git.h — the git changes view's worker side (Phase 19): git's output parsed. git is an external program, run as
// a child process (platform.h's processes); no git code is in ronin. Plain C, headless: platform.h only. The app
// side (the modal, its commands) is git_changes.c.
//
// What is parsed, on the worker: `git status --porcelain=v1 -z --branch` (records of "XY PATH", a rename's or
// copy's second path after it, the "## branch" record first) and a unified diff (`git diff`), each line with its
// kind; a failed run's output gives the line shown to the user (git_error_line).

#ifndef GIT_H
#define GIT_H

#define GIT_LINE_MAX 4096        // a diff line is cut here when stored (the pane cuts it at its edge anyway)
#define GIT_ERROR_LINE_MAX 120   // git's error line shown in the echo area or the status line

typedef enum GitLineKind {
    GIT_LINE_TEXT,    // context, "\ No newline at end of file", anything outside a diff (a failed run's output)
    GIT_LINE_HEADER,  // a file's header: diff --git, index, ---, +++, modes, similarity, rename, copy, Binary files
    GIT_LINE_HUNK,    // @@ -a,b +c,d @@
    GIT_LINE_ADDED,
    GIT_LINE_REMOVED,
    GIT_LINE_CUT,     // the last line of a cut diff (no text of its own: the view says so)
} GitLineKind;

typedef struct GitLine {
    u32 off, len;     // into the diff's text (CR dropped, cut at GIT_LINE_MAX)
    u32 kind;         // GitLineKind
    u32 pad;
} GitLine;

typedef struct GitDiff {
    String8 text;     // the output the lines point into
    GitLine *lines;
    i64 count;
    i64 first_changed; // the new file's 1-based line of the first + or - line; 0: none (binary, a mode change)
    b32 cut;          // more lines than the limit: the last line is GIT_LINE_CUT
} GitDiff;

typedef struct GitEntry {
    u8 xy[2];         // as git prints them: "M ", " M", "A ", " D", "R ", "??" ...
    String8 path;     // relative to the repository's top level, '/' separated
    String8 orig;     // a rename's or copy's source; empty otherwise
} GitEntry;

typedef struct GitStatus {
    GitEntry *entries;
    i64 count;
    b32 cut;          // more files than the limit
    String8 branch;   // from the "## " record: "main", "HEAD (no branch)"; empty without one
    b32 no_head;      // "## No commits yet on ..." (or the older "## Initial commit on ..."): no HEAD to diff against
} GitStatus;

// ---------------------------------------------------------------------------
// The worker: a GitJob runs git's commands one after the other on a worker thread (CLAUDE.md has the threading
// rules), each process read to its end into the job's arena and parsed there, and hands each step's result to the
// main thread as one GitBatch through one locked queue (os_wake_main). The job lives at the start of its own
// reservation, so a worker abandoned at exit never touches freed memory. It owns its processes from their start to
// their close: git_cancel sets the cancel flag and event, the worker kills git's whole job object and ends; the main
// thread never waits for it (it joins after the last batch, when the worker has left its last os_* call).
//
// The command lines ("<git>" P ..., P = GIT_COMMON_ARGS):
//   OPEN    rev-parse --show-toplevel                                     in the request's directory, then STATUS
//   STATUS  status --porcelain=v1 -z --branch --untracked-files=all       in the top level
//   DIFF    diff HEAD --no-color --no-ext-diff -- "<path>"                in the top level
//           diff HEAD --no-color --no-ext-diff -M -- "<orig>" "<path>"    a rename or copy
//           diff --no-index --no-color --no-ext-diff -- /dev/null "<path>"  untracked, or no commits yet (exit 1 = ok)

#define GIT_COMMON_ARGS "--no-optional-locks --literal-pathspecs -c core.quotepath=false -c core.fsmonitor=false"
#define GIT_OUTPUT_MAX MB(64)    // more output than this is cut as the line limit cuts it (git is stopped)
#define GIT_READ_BYTES KB(64)

typedef enum GitJobKind { GIT_JOB_OPEN, GIT_JOB_STATUS, GIT_JOB_DIFF } GitJobKind;
typedef enum GitDiffMode { GIT_DIFF_HEAD, GIT_DIFF_RENAME, GIT_DIFF_NEW } GitDiffMode;

typedef enum GitBatchKind {
    GIT_BATCH_TOPLEVEL,      // rev-parse answered: `text` the top level, `program` the git that ran
    GIT_BATCH_STATUS,        // `status`
    GIT_BATCH_DIFF,          // `diff`
    GIT_BATCH_NO_GIT,        // the program is nowhere: `text` the git_path looked for (empty: PATH)
    GIT_BATCH_NOT_WORK_TREE, // rev-parse said "not a git repository"
    GIT_BATCH_FAILED,        // git exited with an error (`exit_code`, `text` its output, `error` its line) or did not start
    GIT_BATCH_TIMEOUT,       // no output for the timeout: git was stopped
    GIT_BATCH_CANCELLED,
} GitBatchKind;

typedef struct GitBatch {
    struct GitBatch *next;
    GitBatchKind kind;
    b32 last;                // the job's last batch: the worker has nothing more to do
    u32 exit_code;
    String8 text, error, program;
    GitStatus status;
    GitDiff diff;
} GitBatch;

typedef struct GitRequest {
    GitJobKind kind;
    String8 program;         // git_path (a file); empty: git.exe on PATH (OPEN looks it up)
    String8 prefix;          // dev: arguments before git's (the fake git's "--fake-git <dir>")
    String8 env;             // dev: "NAME=VALUE" lines over ronin's environment (the real-git test)
    String8 dir;             // OPEN: where rev-parse runs; STATUS, DIFF: the top level
    String8 path, orig;      // DIFF
    GitDiffMode mode;
    i64 max_lines, max_files;
    u32 timeout_ms;          // no output for this long stops git (0: no limit)
} GitRequest;

typedef struct GitJob {
    GitRequest req;          // copied into the job's arena before the start: read only by the worker
    // Shared, under `lock` (held only to link or unlink a batch) or atomic.
    OsLock lock;
    GitBatch *ready, *ready_last;
    volatile i32 cancel;
    OsEvent cancel_event;
    // The worker's: its arena (the job itself is at its start), the outputs, the batches.
    Arena arena;
    // The main thread's.
    OsThread thread;
    b32 started, ended;      // ended: the last batch was taken and the worker joined
    u64 serial;              // the owner's: which open and which request this was
#if RONIN_DEV
    volatile i32 dev_processes; // processes started (the tests' count)
#endif
} GitJob;

GitJob   *git_job_create(const GitRequest *req); // NULL when out of address space
b32       git_job_start(GitJob *j);
void      git_job_cancel(GitJob *j);             // any time; the worker ends soon after (its last batch says CANCELLED)
GitBatch *git_job_take(GitJob *j);               // the batches ready, in order (the main thread; the list is its now)
void      git_job_ended(GitJob *j);              // after the last batch was taken: joins (the worker is past its last call)
b32       git_job_join(GitJob *j, u64 deadline_us); // shutdown: true when it ended by the deadline
void      git_job_free(GitJob *j);               // ended (or never started), or abandoned at exit (not freed then)

// Both parse a whole output (`raw` stays: the result points into it); the arrays in `arena`.
void    git_parse_status(Arena *arena, String8 raw, i64 max_files, GitStatus *out);
void    git_parse_diff(Arena *arena, String8 raw, i64 max_lines, GitDiff *out);
// The line of a failed run's output to show: the first non-empty line starting with "fatal:", "error:" or "usage:",
// else the first non-empty line; CR dropped, cut at GIT_ERROR_LINE_MAX bytes on a character boundary. Points into out.
String8 git_error_line(String8 out);
b32     git_not_a_repository(String8 out); // the output says "not a git repository"

#endif // GIT_H
