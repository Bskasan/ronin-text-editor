// isearch.h — incremental search (Emacs' isearch), query-replace and replace-string, on the search
// engine (search.h). Plain C, headless: the app routes keys here and draws the echo line and the
// highlighting from this state.
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

// ---------------------------------------------------------------------------
// query-replace and replace-string
//
// Two chained minibuffer prompts ask what to replace and with what (an empty first answer repeats the
// last pair); then a session runs on the calling view: from point, or within the active region. The
// next match is searched in slices; query-replace shows it and asks (the answers bypass the keymap,
// as a single-key prompt does); "!" and replace-string replace the rest across frames, keys other than
// C-g ignored meanwhile. The session keeps its own match: whatever the wheel did to point, an answer
// acts on that match and puts point back on it. Every replacement of a session is in one undo group:
// the answers are COMMAND_UNDO_CONTINUE and the replacements made between frames are outside any command.

#define REPLACE_ARENA_RESERVE MB(64)
#define REPLACE_COST 64 // search positions a replacement counts for in a slice

typedef enum ReplaceState {
    REPLACE_OFF,
    REPLACE_SEARCHING, // looking for the next match (query-replace)
    REPLACE_ASKING,    // a match is shown with the question
    REPLACE_ALL,       // "!" or replace-string: replacing the rest
} ReplaceState;

typedef enum ReplaceEnd {
    REPLACE_END_DONE,    // "Replaced N occurrences"
    REPLACE_END_STOPPED, // C-g during replace-all: "Replaced N occurrences (stopped)"
    REPLACE_END_QUIT,    // C-g at a question: "Quit"
} ReplaceEnd;

struct Replace {
    ReplaceState state;
    b32 ask;              // query-replace (else replace-string)
    View *view;
    Echo *echo;
    Arena arena;          // from and to; reset per session
    String8 from, to;
    b32 fold;             // the search folds case (from has no capital letter)
    b32 convert;          // case conversion of the replacement (fold, and to has no capital letter)
    i64 lo;               // the range: [lo, end)
    BufferMarker end;     // does not advance
    i64 next;             // where the next search starts
    i64 point;           // where point is (and goes when the session ends): the match asked about, or after the last replacement
    i64 count;            // replacements made
    i64 match_start, match_end; // the match asked about
    i64 all_from;         // REPLACE_ALL began here (progress)
    Search search;
    Arena pair_arena;     // the last pair, for an empty answer to the first prompt
    String8 last_from, last_to;
    b32 has_last;
    b32 region;           // the prompt started with an active region: [region_lo, region_hi)
    i64 region_lo, region_hi;
};

void replace_init(Replace *rp);
void replace_destroy(Replace *rp); // also releases an active session's marker
b32  replace_pending(Replace *rp); // searching or replacing: the app wants frames
// One slice: at most `budget` positions searched, a replacement counting REPLACE_COST. `scratch` holds a
// case-converted replacement while it is inserted. Returns the cost used.
i64  replace_work(Replace *rp, Arena *scratch, i64 budget);
// A key while a session is active (not in the minibuffer). `command`: what the global keymap runs for it
// (NULL for a prefix); `answer`: the key as an answer ('y', ' ', 'n', 0x7f for DEL, '!', 'q', '\r' for
// RET, '.'), else 0. True when the key was taken (an answer; any key but C-g while searching or
// replacing); false when the session ended and the key goes on as usual.
b32  replace_key(CommandContext *ctx, const Command *command, u32 answer);
void replace_finish(Replace *rp, ReplaceEnd how); // point, the marker, the message
// The echo line: "Query replacing A with B: (y, n, !, q, .)", "... [searching... N%]", "Replacing... N%".
String8 replace_prompt(Replace *rp, Arena *arena);
// The replacement of `match` by `to` with Emacs' case conversion (case-replace): an all-caps match with
// a word of two or more letters (or an all-caps match of one-letter words) gives an all-caps replacement;
// a match whose words all start with a capital gives one with each word capitalized; otherwise `to`.
String8 replace_case(Arena *arena, String8 to, String8 match);

extern const Command CMD_QUERY_REPLACE, CMD_REPLACE_STRING;

#endif // ISEARCH_H
