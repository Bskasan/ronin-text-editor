// isearch.h — incremental search (Emacs' isearch) on the search engine (search.h). Plain C, headless:
// the app routes keys here and draws the echo line and the highlighting from this state.
//
// isearch is a mode with its own keymap ([keys isearch], searched before the global one), not a
// minibuffer prompt. Its state is a stack of steps: each typed character, repeat, direction change,
// yank, history entry or case switch is one step, and DEL pops one. A step is resolved from the
// step below it: by a search (run in slices across frames, so a keystroke never waits), or at once.
// The session's position lives in its steps, not in the cursor: whatever the mouse wheel did to point,
// every isearch command first puts point back on the current match.

#ifndef ISEARCH_H
#define ISEARCH_H

#define ISEARCH_ARENA_RESERVE MB(256) // steps, and their strings, of one search

typedef enum IsearchStepKind {
    ISEARCH_STEP_START,   // the empty string at the origin
    ISEARCH_STEP_TYPE,    // characters appended (typed, C-w, C-y): search from the current match
    ISEARCH_STEP_REPEAT,  // C-s / C-r in the search's direction: the next match, or wrap
    ISEARCH_STEP_REVERSE, // C-s / C-r against it: the same match, point to its other end
    ISEARCH_STEP_CASE,    // M-c: search again with the other case mode
    ISEARCH_STEP_HISTORY, // M-p / M-n: another string, searched from the origin
} IsearchStepKind;

typedef struct IsearchStep {
    String8 string;      // in the text arena
    IsearchStepKind kind;
    b32 forward;
    b32 fold;            // in effect: case-insensitive
    b32 explicit_case;   // M-c decided `fold`; otherwise smart case (fold unless the string has a capital)
    b32 resolved;        // its search is done (or it needed none)
    b32 success;
    b32 wrapped, overwrapped;
    i64 match_start, match_end; // the match shown, -1 = none (a failing step keeps the last one)
    i64 point;
} IsearchStep;

struct Isearch {
    b32 active;
    View *view;
    i64 origin;          // where the search started
    Arena step_arena;    // the steps only, so they stay contiguous
    Arena text_arena;    // their strings
    IsearchStep *steps;
    i32 count;
    i32 searching;       // the step the engine works on, -1 = none
    i32 ring_pos;        // M-p / M-n: the history entry shown, -1 = none yet
    Search search;
    Minibuffer *mini;    // the history (MINI_HISTORY_SEARCH)
};

void isearch_init(Isearch *is, Minibuffer *mini);
void isearch_destroy(Isearch *is);
b32  isearch_pending(Isearch *is); // a step is still being searched: the app wants frames
// One slice of the pending search: at most `budget` positions. Returns the positions used.
i64  isearch_work(Isearch *is, i64 budget);
// The last resolved step: the current match and point.
IsearchStep *isearch_current(Isearch *is);
IsearchStep *isearch_top(Isearch *is);
// "Failing overwrapped I-search backward: ", as Emacs composes it.
String8 isearch_prompt(Isearch *is, Arena *arena);
// The length of the part of the top string that is found (the rest is drawn as failing).
i64  isearch_fail_pos(Isearch *is);
// Ends the search at the current match (RET): the history, the mark at the origin. For a key that
// ends the search before its own command runs.
void isearch_exit(CommandContext *ctx);
// Internal: a plain character typed while searching (KEY_RESULT_SELF_INSERT), appended to the string.
extern const Command CMD_ISEARCH_PRINTING_CHAR;

extern const Command CMD_ISEARCH_FORWARD, CMD_ISEARCH_BACKWARD, CMD_ISEARCH_REPEAT_FORWARD, CMD_ISEARCH_REPEAT_BACKWARD;
extern const Command CMD_ISEARCH_DELETE_CHAR, CMD_ISEARCH_EXIT, CMD_ISEARCH_ABORT, CMD_ISEARCH_YANK_WORD, CMD_ISEARCH_YANK_KILL;
extern const Command CMD_ISEARCH_RING_RETREAT, CMD_ISEARCH_RING_ADVANCE, CMD_ISEARCH_TOGGLE_CASE_FOLD;


#endif // ISEARCH_H
