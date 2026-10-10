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

#endif // PROJECT_H
