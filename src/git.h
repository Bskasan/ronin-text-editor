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

// Both parse a whole output (`raw` stays: the result points into it); the arrays in `arena`.
void    git_parse_status(Arena *arena, String8 raw, i64 max_files, GitStatus *out);
void    git_parse_diff(Arena *arena, String8 raw, i64 max_lines, GitDiff *out);
// The line of a failed run's output to show: the first non-empty line starting with "fatal:", "error:" or "usage:",
// else the first non-empty line; CR dropped, cut at GIT_ERROR_LINE_MAX bytes on a character boundary. Points into out.
String8 git_error_line(String8 out);
b32     git_not_a_repository(String8 out); // the output says "not a git repository"

#endif // GIT_H
