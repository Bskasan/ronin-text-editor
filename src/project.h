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
// project_ignore, the .gitignore files from the root down, .teal-project's [ignore], then its [allow]
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
// The whole order: `top` (.teal-project), then the directory's chain (innermost first), then `builtin`.
b32  ignore_path(const IgnoreScope *top, const IgnoreScope *chain, const IgnoreScope *builtin, String8 rel, b32 is_dir);
b32  ignore_glob(String8 glob, String8 s); // '*', '?', "**" over a lowercased path (exposed for tests)

// ---------------------------------------------------------------------------
// Roots. A project needs no setup: its root is the nearest directory at or above the buffer's file
// that holds a marker (project_markers: .git, .hg, .svn, .teal-project; a file or a directory). A
// marker at a drive or share root does not count. With no marker, the only fallback is a directory
// given on the command line: for the files inside it and for buffers without a file. The current
// directory never becomes a root by itself. Paths are full Windows paths; roots have no trailing
// separator.

#define PROJECT_NOT_IN_PROJECT "Not in a project (no .git found; start teal with a directory, or add a .teal-project file)"
#define PROJECT_ROOT_CACHE 256

typedef b32 ProjectExistsFn(void *data, String8 path); // a file or directory exists at path

b32     project_is_drive_root(String8 dir);          // "C:\", "C:", "\\server\share" (and with a trailing separator)
b32     project_path_inside(String8 path, String8 dir); // path is dir or below it (ASCII case-insensitive, either separator)
// The nearest directory at or above `dir` holding a marker, or empty.
String8 project_root_find(Arena *arena, String8 dir, String8 markers, ProjectExistsFn *exists, void *data);
b32     project_exists_fs(void *data, String8 path); // the file system (main thread)

// The roots found per directory, so the file system is asked once per directory. Reset when full and on
// project-refresh.
typedef struct ProjectRootCache {
    Arena arena;
    i32 count;
    String8 dirs[PROJECT_ROOT_CACHE], roots[PROJECT_ROOT_CACHE]; // an empty root: none
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
// .teal-project at a root: the config format with [ignore] and [allow] sections, one pattern per line.

#define PROJECT_FILE_NAME ".teal-project"

typedef struct ProjectFile {
    String8 ignore, allow; // the patterns, one per line
    ConfigDiags diags;
} ProjectFile;

void project_file_parse(Arena *arena, String8 text, String8 file_name, ProjectFile *out);

#endif // PROJECT_H
