// project.h — projects without setup: ignore rules (a gitignore subset). Plain C, headless.

#ifndef PROJECT_H
#define PROJECT_H

// ---------------------------------------------------------------------------
// Ignore rules: a subset of .gitignore. Supported: blank lines and '#' comments, trailing blanks
// trimmed, '*' and '?' (never matching '/'), "**" as a leading "**/", a trailing "/**" and a middle
// "/**/", a leading or inner '/' anchoring the pattern to its file's directory (otherwise it matches a
// name at any depth below it), a trailing '/' for directories only, '!' negation, nested .gitignore
// files scoped to their directories; ASCII is matched case-insensitively (Windows' core.ignorecase).
// Not supported: character classes ([a-z]), backslash escapes (\#, \!, "\ "), .git/info/exclude,
// core.excludesFile and the global gitignore.
//
// Evaluation is one ordered list in which the last matching pattern decides: the built-in
// project_ignore, the .gitignore files from the root down, .ronin-project's [ignore], then its [allow]
// as negations. An ignored directory is never entered, so nothing below it can be included again
// (as in git).

enum {
    IGNORE_NEGATE   = 1 << 0, // '!': a match includes
    IGNORE_DIR_ONLY = 1 << 1, // a trailing '/'
    IGNORE_ANCHORED = 1 << 2, // a leading or inner '/': matched against the path below the scope's directory
};

typedef struct IgnorePattern {
    String8 glob; // ASCII lowercased; without the '!', the leading '/' and the trailing '/'
    u32 flags;    // IGNORE_*
} IgnorePattern;

// The patterns of one source. Scopes are immutable once built and chained to the scope of the parent
// directory, so a directory's chain is shared by its subdirectories.
typedef struct IgnoreScope {
    const struct IgnoreScope *parent; // the scope of an outer directory (evaluated before this one)
    String8 base;                     // the directory, relative to the root ("" = the root), '/' separators
    IgnorePattern *patterns;
    i32 count;
} IgnoreScope;

// Lines of .gitignore text (or one pattern per line) into a scope; `negate_all` makes every pattern a
// negation ([allow]). The scope's strings go into `arena`.
void ignore_parse_lines(Arena *arena, IgnoreScope *scope, String8 text, b32 negate_all);
// Blank-separated patterns (the project_ignore setting).
void ignore_parse_words(Arena *arena, IgnoreScope *scope, String8 words);
// The scope's own decision for a path relative to the root: 1 ignored, 0 included, -1 no pattern matched.
i32  ignore_scope_decide(const IgnoreScope *scope, String8 rel, b32 is_dir);
// The whole order: `top` (.ronin-project), then the directory's chain (innermost first), then `builtin`.
b32  ignore_path(const IgnoreScope *top, const IgnoreScope *chain, const IgnoreScope *builtin, String8 rel, b32 is_dir);
b32  ignore_glob(String8 glob, String8 s); // '*', '?', "**" over a lowercased path (exposed for tests)

// ---------------------------------------------------------------------------
// Roots. A project needs no setup: its root is the nearest directory at or above the buffer's file
// that holds a marker (project_markers: .git, .hg, .svn, .ronin-project; a file or a directory). A
// marker at a drive or share root does not count. A directory given on the command line is a root
// itself: for the files inside it (unless a marker below it is nearer; the search stops there) and for
// buffers without a file. The current directory never becomes a root by itself. Paths are full Windows
// paths; roots have no trailing separator.

#define PROJECT_NOT_IN_PROJECT "Not in a project (no .git found; start ronin with a directory, or add a .ronin-project file)"
#define PROJECT_ROOT_CACHE 256

typedef b32 ProjectExistsFn(void *data, String8 path); // a file or directory exists at path

b32     project_is_drive_root(String8 dir);          // "C:\", "C:", "\\server\share" (and with a trailing separator)
b32     project_path_inside(String8 path, String8 dir); // path is dir or below it (ASCII case-insensitive, either separator)
// The nearest directory at or above `dir` holding a marker, not looking above `stop` (if not empty), or empty.
String8 project_root_find(Arena *arena, String8 dir, String8 markers, String8 stop, ProjectExistsFn *exists, void *data);
b32     project_exists_fs(void *data, String8 path); // the file system (main thread)

// The roots found per directory, so the file system is asked once per directory. Reset when full and on
// project-refresh.
typedef struct ProjectRootCache {
    Arena arena;
    i32 count;
    String8 dirs[PROJECT_ROOT_CACHE], stops[PROJECT_ROOT_CACHE], roots[PROJECT_ROOT_CACHE]; // an empty root: none
    u64 queries;                                                // project_root_find calls (tests)
} ProjectRootCache;

void    project_cache_init(ProjectRootCache *c);
void    project_cache_reset(ProjectRootCache *c);
void    project_cache_destroy(ProjectRootCache *c);
// The project root of a buffer whose file is in `file_dir` (empty: no file), with the command-line
// directory (empty: none). Empty: not in a project. The result lives in the cache's arena until a reset.
String8 project_root_for(ProjectRootCache *c, String8 file_dir, String8 cmdline_dir, String8 markers, ProjectExistsFn *exists,
                         void *data);

// ---------------------------------------------------------------------------
// .ronin-project at a root: the config format with [ignore] and [allow] sections, one pattern per line
// (a pattern cannot start with '[': such a line is a section header), and a [build] section of
// "name = value" lines: command (what C-x p c offers; never run without being shown) and directory
// (where it runs, relative to the root).

#define PROJECT_FILE_NAME ".ronin-project"

typedef struct ProjectFile {
    String8 ignore, allow; // the patterns, one per line
    String8 build_command, build_directory; // [build]; empty when not given
    ConfigDiags diags;
} ProjectFile;

void project_file_parse(Arena *arena, String8 text, String8 file_name, ProjectFile *out);
// Reads and parses root\.ronin-project (OS_FILE_NOT_FOUND: none, `out` empty).
OsFileStatus project_file_read(Arena *arena, String8 root, ProjectFile *out);

// ---------------------------------------------------------------------------
// The index: the files of one root as paths relative to it, '/' separators, in scan order (breadth
// first: shallow files first). Compact for a million files: 16 bytes a file, its name once (u16 length
// first), each directory's path once.

#define PROJECT_PATH_MAX 4096 // bytes of a relative path; deeper ones are skipped (counted as errors)

typedef struct ProjectDir {
    u32 path_off, path_len; // in `names`: "" for the root, "src/sub"
} ProjectDir;

typedef struct ProjectEntry {
    u64 mask;     // project_mask of the whole relative path: the matcher's cheap rejection
    u32 dir;      // index into dirs
    u32 name_off; // in `names`: a u16 length, then the bytes
} ProjectEntry;

typedef struct ProjectIndex {
    Arena dirs, files, names; // ProjectDir[], ProjectEntry[], bytes; reserved large, committed on demand
    i64 dir_count, file_count;
} ProjectIndex;

void    project_index_init(ProjectIndex *x);
void    project_index_reset(ProjectIndex *x);
void    project_index_destroy(ProjectIndex *x);
u64     project_index_memory(ProjectIndex *x); // committed bytes
i64     project_index_add_dir(ProjectIndex *x, String8 rel);
void    project_index_add_file(ProjectIndex *x, i64 dir, String8 name, u64 mask);
String8 project_index_dir(ProjectIndex *x, i64 dir);
String8 project_index_name(ProjectIndex *x, i64 file);
// The relative path of a file into `buf`; its length (0 when it does not fit).
i64     project_index_path(ProjectIndex *x, i64 file, u8 *buf, i64 cap);
// The characters of a path as a 64-bit set: a-z (folded), 0-9, '_', '-', '.', '/', ' ', anything else
// ASCII, anything non-ASCII.
u64     project_mask(String8 s);

// ---------------------------------------------------------------------------
// The scanner: a worker thread walks a root with the ignore rules and hands the index records to the
// main thread in batches through one synchronized queue (CLAUDE.md has the threading rules). Reparse
// points that are directories (symlinks, junctions) are not entered; files that are reparse points
// (cloud placeholders) are listed.

#define PROJECT_BATCH_BYTES KB(64)
#define PROJECT_BATCH_POOL 8     // batches in flight; the worker waits for one to come back
#define PROJECT_MAX_DEPTH 256
#define PROJECT_GITIGNORE_MAX MB(1)

typedef struct ProjectScanResult {
    i64 files, dirs;   // listed, directories walked
    i64 ignored;       // entries a rule left out (not counting what was inside an ignored directory)
    i64 errors;        // directories that could not be listed, paths too long
    b32 limit;         // stopped at max_files
    b32 cancelled;
} ProjectScanResult;

typedef struct ProjectBatch {
    struct ProjectBatch *next;
    i64 used;
    b32 last;                 // the worker's final batch: `result` is filled
    ProjectScanResult result;
    u8 data[PROJECT_BATCH_BYTES];
} ProjectBatch;

// Lives at the start of its own reservation (the worker's arena), so a worker abandoned at exit never
// touches freed memory.
typedef struct ProjectScan {
    // The request: set before project_scan_start, read-only while the worker runs.
    String8 root;          // full path, no trailing separator
    String8 builtin;       // project_ignore: blank-separated patterns
    String8 ignore, allow; // .ronin-project's patterns, one per line
    i64 max_files;
    // Shared.
    OsLock lock;           // guards the two lists
    ProjectBatch *ready;   // to the main thread, oldest first
    ProjectBatch *ready_last;
    ProjectBatch *free;    // back to the worker
    volatile i32 cancel;
    OsEvent event;         // the worker waits on it: a batch came back, a cancel, a dev release
    volatile i32 dev_hold; // dev: after dev_hold_after batches, wait while this is set (or until cancelled)
    i32 dev_hold_after;
    i64 dev_hold_entries;  // dev: ... or after this many entries (0 = never)
    volatile i32 dev_stuck; // dev: ignore cancel and wait while set (a file system that hangs)
    // The worker's own.
    Arena arena;           // the scope chains, the directory queue, the batches (and this struct)
    Arena temp;            // one directory's listing at a time
    // The main thread's.
    OsThread thread;
    i64 drain_dir;         // the index directory of the last directory record drained
    b32 ended;             // the last batch arrived and the worker was joined
    ProjectScanResult result;
} ProjectScan;

// A scan for `root` in its own reservation; set the other request fields, then start it.
ProjectScan *project_scan_create(String8 root, String8 builtin, ProjectFile *file, i64 max_files);
b32  project_scan_start(ProjectScan *s); // false: no thread (the scan then never runs; free it)
void project_scan_cancel(ProjectScan *s); // any time; the worker ends soon after and sends its last batch
// Main thread: appends the batches that arrived to `x` (NULL: throws them away); returns how many files
// they held. When the last one arrives, the worker is joined and s->ended is set (s->result has the counts).
i64  project_scan_drain(ProjectScan *s, ProjectIndex *x);
// Joins within the timeout: false when the worker is still running (stuck in the file system).
b32  project_scan_join(ProjectScan *s, u32 timeout_ms);
void project_scan_free(ProjectScan *s); // only once it ended (or was joined)

#endif // PROJECT_H
