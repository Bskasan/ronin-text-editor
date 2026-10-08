// minibuffer.h — the minibuffer: prompts that read a line (or a key) from the user, with a
// vertical list of completion candidates above it. Plain C, headless.

#ifndef MINIBUFFER_H
#define MINIBUFFER_H

// ---------------------------------------------------------------------------
// Candidates and matching. The matcher is the only code that knows how input matches a candidate:
// case-insensitive; the input's space-separated terms must all occur as substrings; a candidate
// that starts with the first term ranks before the other matches, and one equal to the whole input
// ranks first. Phase 11 replaces it with fuzzy scoring.
//
// Case folding keeps byte offsets: a character whose folded form has another UTF-8 length is kept
// as it is, so a match position in the folded text is the same position in the shown text.

#define MATCH_MAX_TERMS 16

enum {
    CANDIDATE_DIR = 1 << 0, // find-file: a directory (shown with a trailing slash)
};

typedef struct Candidate {
    String8 text;       // shown, and the result when it is accepted
    String8 folded;     // text, case-folded once when the set is built
    String8 annotation; // shown right-aligned (M-x: the key binding); may be empty
    u32 flags;          // CANDIDATE_*
    u8 score;           // the last match_rank: MATCH_*
} Candidate;

typedef enum MatchScore {
    MATCH_NONE,
    MATCH_SUBSTRING, // every term occurs (also: the empty input matches everything)
    MATCH_PREFIX,    // ... and the candidate starts with the first term
    MATCH_EXACT,     // the candidate is the whole input
} MatchScore;

typedef struct MatchQuery {
    String8 whole; // the folded input
    String8 terms[MATCH_MAX_TERMS];
    i32 count;     // terms past MATCH_MAX_TERMS are ignored
} MatchQuery;

typedef struct MatchSpan {
    i64 start, end; // byte offsets in the candidate's text
} MatchSpan;

String8    match_fold(Arena *arena, String8 s); // the folded copy (same length)
MatchQuery match_query(Arena *arena, String8 input);
MatchScore match_score(MatchQuery *q, Candidate *c);
// The matched substrings of a candidate, for drawing: each term's first occurrence (the prefix for
// the first term of a prefix match). Returns the count.
i32        match_spans(MatchQuery *q, Candidate *c, MatchSpan *out, i32 cap);
// Scores every candidate and writes the indices of the matches to `out`: exact matches first, then
// prefix matches, then the others, each group in the candidates' own order. Returns the count.
i64        match_rank(MatchQuery *q, Candidate *cands, i64 count, i32 *out);

// ---------------------------------------------------------------------------
// The minibuffer: a one-line View on its own Buffer, so every editing command works in it. The
// prompt is drawn before the input and is not part of the text.
//
// Prompts never block: minibuffer_read opens one and returns. When the user accepts, the command
// driver (view_run_command) runs the request's continuation as a command of its own on the View
// the prompt was opened from; a continuation may open the next prompt (a chain). Aborting (C-g,
// ESC) drops the whole chain. There is one minibuffer and no recursion.

#define MINI_HISTORY_MAX 100 // entries per category
#define MINI_ARENA_RESERVE GB(1)

typedef enum MiniKind {
    MINI_TEXT,   // a line of text
    MINI_CHOICE, // a line, completed from candidates
    MINI_NUMBER, // N or N:M
    MINI_KEY,    // one key from `answers`, no editing: keys bypass the keymap
    MINI_YES_NO, // "yes" or "no", typed in full
} MiniKind;

typedef enum MiniHistoryKind {
    MINI_HISTORY_NONE,
    MINI_HISTORY_COMMAND,
    MINI_HISTORY_FILE,
    MINI_HISTORY_BUFFER,
    MINI_HISTORY_LINE,
    MINI_HISTORY_TEXT, // tests and anything else
    MINI_HISTORY_COUNT,
} MiniHistoryKind;

typedef struct Minibuffer Minibuffer;

typedef struct MiniResult {
    String8 text;     // the accepted input, or the candidate's text (in the chain arena)
    i32 candidate;    // MINI_CHOICE: the accepted candidate, -1 = the input as typed
    u32 flags;        // ... and its CANDIDATE_* flags
    u32 key;          // MINI_KEY: the answer, lowercase
    b32 yes;          // MINI_YES_NO
    i64 number[2];    // MINI_NUMBER: N, and M when numbers == 2
    i32 numbers;
} MiniResult;

// The chain's own state, kept from one prompt to the next until the chain ends.
typedef struct MiniState {
    Buffer *buffer;   // the buffer a chain is about (kill-buffer, save-some-buffers ...)
    i32 index;        // a position in a loop (save-some-buffers)
    u32 flags;
    String8 text;     // in the chain arena
} MiniState;

typedef void MiniDoneFn(CommandContext *ctx, MiniResult *r);
// MINI_CHOICE: called whenever the input changes; may rebuild the candidates (minibuffer_add_candidate
// after minibuffer_clear_candidates). Returns where the part of the input to match starts.
typedef i64 MiniCandidatesFn(Minibuffer *mb, void *data, String8 input);

typedef struct MiniRequest {
    MiniKind kind;
    String8 prompt;              // copied
    String8 initial;             // the initial input, copied
    MiniHistoryKind history;
    MiniDoneFn *done;
    const char *answers;         // MINI_KEY: the accepted keys, such as "yn" or "yn!q"
    MiniCandidatesFn *candidates;
    void *data;                  // for `candidates`
    b32 require_match;           // MINI_CHOICE: input that matches no candidate is refused ("[No match]")
    b32 file;                    // a file name: DEL after a slash removes the last component; a directory candidate descends
    b32 run_command;             // M-x: the result names a command, which runs instead of `done`
} MiniRequest;

typedef struct MiniHistory {
    String8 items[MINI_HISTORY_MAX]; // [0] is the newest
    i32 count;
} MiniHistory;

struct Minibuffer {
    Buffer *buffer;
    View *view;
    b32 active;
    b32 finished;             // accepted: the driver runs the continuation after this command
    i32 in_continuation;      // a prompt opened now continues the chain (its arena is kept)
    MiniKind kind;
    String8 prompt;
    MiniDoneFn *done;
    const Command *then_command; // instead of `done`: run this command on the calling View (M-x)
    View *caller;
    MiniResult result;
    MiniState state;
    const char *answers;
    MiniCandidatesFn *candidates_fn;
    void *candidates_data;
    b32 require_match, file, run_command;
    MiniHistoryKind history;
    i32 history_pos;          // -1: the input as typed; otherwise an index into the history
    String8 typed;            // the input before M-p
    u64 seen_edits;           // the buffer's edit_count when the candidates were last filtered
    Arena arena;              // the chain: prompts, results, MiniState text; reset when a chain starts

    // Candidates (MINI_CHOICE).
    Arena cand_arena;         // the Candidate array only, so it stays contiguous
    Arena text_arena;         // their strings
    Arena match_arena;        // the current input, query and matches; reset on every filter
    Candidate *cands;
    i64 cand_count;
    i32 *matches;
    i64 match_count;
    MatchQuery query;
    i64 match_from;           // where the matched part of the input starts (file names: after the last slash)
    i64 selected;             // index into matches
    i64 list_top;             // the first match shown

    String8 cand_key;         // what the candidates were built for (find-file: the directory); in text_arena

    MiniHistory histories[MINI_HISTORY_COUNT];
    Arena history_arena;      // the history's strings; dead ones compacted away
    u64 history_live;         // bytes of live strings in it
};

void minibuffer_init(Minibuffer *mb, Arena *perm, Buffer *buffer); // the buffer is the minibuffer's own
i32  minibuffer_destroy(Minibuffer *mb); // returns leaked markers
// Opens a prompt. False (with Emacs' message) when the minibuffer is already active.
b32  minibuffer_read(CommandContext *ctx, MiniRequest *req);
// Ends the prompt without running anything; the chain is dropped.
void minibuffer_abort(Minibuffer *mb);
String8 minibuffer_input(Minibuffer *mb, Arena *arena);
// The command driver calls this after every command: it filters the candidates again when the
// input changed, and runs the continuation of an accepted prompt.
void minibuffer_after_command(CommandContext *ctx);
// MINI_KEY: what a key does (keys bypass the keymap). `command` is what the key is bound to in the
// keymap stack (NULL if nothing), `chord_char` its character when it is a plain character key (0
// otherwise). Runs the answer or the abort through the driver.
void minibuffer_key(CommandContext *ctx, const Command *command, u32 chord_char);
void minibuffer_history_add(Minibuffer *mb, MiniHistoryKind kind, String8 text);

// The first match the list shows, so that the selection is among its `lines` rows.
i64  minibuffer_list_top(Minibuffer *mb, i32 lines);
void minibuffer_clear_candidates(Minibuffer *mb);
void minibuffer_add_candidate(Minibuffer *mb, String8 text, String8 annotation, u32 flags);

extern const Command CMD_EXIT_MINIBUFFER, CMD_EXIT_MINIBUFFER_INPUT, CMD_ABORT_MINIBUFFERS;
extern const Command CMD_PREVIOUS_HISTORY_ELEMENT, CMD_NEXT_HISTORY_ELEMENT, CMD_MINIBUFFER_BACKWARD_UPDIR;
extern const Command CMD_MINIBUFFER_COMPLETE, CMD_MINIBUFFER_NEXT_COMPLETION, CMD_MINIBUFFER_PREVIOUS_COMPLETION;
extern const Command CMD_MINIBUFFER_NEXT_PAGE, CMD_MINIBUFFER_PREVIOUS_PAGE, CMD_EXECUTE_EXTENDED_COMMAND;

#endif // MINIBUFFER_H
