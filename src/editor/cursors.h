/* ======================================================================
   ronin: the multiple-cursors commands and their search over frames

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// the multiple-cursors commands (Phase 14). The names and default keys follow Emacs' multiple-cursors
// package without its "mc/" prefix. Plain C, headless; commands take a CommandContext like every other command.
// The cursors themselves, their merging and the ordered edit pass are the driver's (view.c).
//
// mark-next / mark-previous-like-this add a cursor at the next / previous occurrence of the primary's region
// (literal, case sensitive) after the last / before the first cursor, with the same region; without a region, on
// the line after the last / before the first cursor at its column. mark-all-like-this marks every occurrence.
// skip-to-next-like-this moves the newest cursor on, unmark-next-like-this removes it. edit-lines and its two
// variants put a cursor on each line of the region. All refuse in the minibuffer and stop at max_cursors.
//
// A search that does not end within one slice inside the command goes on as the app's CursorJob, a slice of the
// frame's budget at a time (as replace-string's replace-all); C-g stops it, other keys wait for it to end.

#ifndef CURSORS_H
#define CURSORS_H

#define CURSORS_ALL_RESERVE MB(8) // mark-all's positions (one i64 each; max_cursors is at most 100,000)

typedef enum CursorJobKind {
    CURSOR_JOB_OFF,
    CURSOR_JOB_NEXT,     // mark-next / mark-previous-like-this: a cursor at the match
    CURSOR_JOB_SKIP,     // skip-to-next-like-this: the newest cursor moves to the match
    CURSOR_JOB_ALL,      // mark-all-like-this: every match, the cursors made at the end
} CursorJobKind;

struct CursorJob {
    CursorJobKind kind;
    View *view;
    Echo *echo;
    Search search;
    b32 point_at_end;     // the cursors' points at their match's end (the primary's direction)
    i64 own_start;        // mark-all: the primary's own occurrence, kept as it is
    i64 needle_len;
    Arena positions;      // mark-all: the starts found (reserved at the first mark-all)
    i64 count;
    b32 capped;           // mark-all stopped at max_cursors
};

b32  cursor_job_pending(CursorJob *job);
// One slice of the job's search: at most `budget` positions (the settings and scratch for the cursors it adds).
// Returns what it examined.
i64  cursor_job_work(CursorJob *job, const Settings *settings, Arena *scratch, i64 budget);
void cursor_job_stop(CursorJob *job);    // C-g: nothing is added ("Quit")
void cursor_job_destroy(CursorJob *job); // its reservation

extern const Command CMD_MARK_NEXT_LIKE_THIS, CMD_MARK_PREVIOUS_LIKE_THIS, CMD_MARK_ALL_LIKE_THIS;
extern const Command CMD_SKIP_TO_NEXT_LIKE_THIS, CMD_UNMARK_NEXT_LIKE_THIS;
extern const Command CMD_EDIT_LINES, CMD_EDIT_ENDS_OF_LINES, CMD_EDIT_BEGINNINGS_OF_LINES;

#endif // CURSORS_H
