// edit.h — editing commands beyond the basic ones in view.c: undo and undo-redo, the kill ring
// and its commands (this phase adds indentation and the other editing commands). Plain C,
// headless; commands take a CommandContext like every other command.

#ifndef EDIT_H
#define EDIT_H

// ---------------------------------------------------------------------------
// The kill ring: one for the whole editor, linked to the Windows clipboard (Emacs'
// select-enable-clipboard). Every kill also goes to the clipboard; yank takes the clipboard
// when it changed since our last kill.
//
// Storage: small entries (most kills are a word or a line) sit one after the other in one shared
// arena; an entry over KILL_SMALL_MAX has a reservation of its own. A kill is copied once, from
// the buffer straight into its entry. Appending to the newest entry grows it in place (a small one
// not at the end of the arena moves there; one growing past KILL_SMALL_MAX moves to its own
// reservation once); a large entry is never copied again. Dropped small entries leave dead bytes
// that are compacted away when they pass half of the arena in use.

#define KILL_RING_CAP 1000         // kill_ring_max is at most this
#define KILL_SMALL_MAX MB(1)       // larger entries get their own reservation
#define KILL_SMALL_RESERVE GB(1)   // the shared arena (address space; committed as used)
#define KILL_LARGE_RESERVE GB(2)   // one large entry (as large as a buffer can be)
#define KILL_SCRATCH_RESERVE GB(8) // clipboard text being converted

typedef struct KillEntry {
    u8 *data;      // in the shared arena, or the base of its own reservation
    i64 len;
    b32 large;
    u64 committed; // large: committed bytes
} KillEntry;

typedef struct KillRing {
    KillEntry entries[KILL_RING_CAP]; // a ring: the newest at `head`
    i32 head, count, max;
    i32 yank_index;                   // entries back from the newest that the last yank inserted
    u8 *small;                        // the shared arena
    u64 small_committed, small_used, small_dead;
    Arena scratch;                    // clipboard conversions; reset after use
    u32 clip_seq;                     // the clipboard's sequence number after our last set or read
    b32 clip_dirty;                   // the newest entry changed: the driver sets the clipboard
} KillRing;

b32     kill_init(KillRing *k, i32 max);
void    kill_destroy(KillRing *k);
void    kill_set_max(KillRing *k, i32 max); // drops the oldest entries beyond it
String8 kill_entry(KillRing *k, i32 back); // 0 = the newest
// Space for a kill of `len` bytes: a new entry, or appended to the newest one (`prepend`: before
// its text). The caller copies the text there. NULL when out of memory.
u8     *kill_push(KillRing *k, i64 len);
u8     *kill_extend(KillRing *k, i64 len, b32 prepend);
void    kill_to_clipboard(KillRing *k); // after a command that killed: the clipboard gets the newest entry
void    kill_from_clipboard(KillRing *k); // before yank: a clipboard changed elsewhere becomes the newest entry

// ---------------------------------------------------------------------------
// Indentation by rules on tokens (no parser): brackets match on tokens (syntax.h), so brackets in
// comments and strings do not count. For a line L, with P the previous line that has code (blank
// and comment-only lines are skipped):
//   - L starts inside a multi-line comment or string: TAB leaves it; RET into one gets the
//     previous line's indentation.
//   - L starts with a closer: the indentation of the line holding its matching opener.
//   - P leaves a bracket open: P's indentation plus one level, however many it opened.
//   - Otherwise the indentation of the line P's statement started on: where the opener of P's
//     last unmatched closer is (a line that closes a bracket opened earlier returns to that
//     line's indentation), or P itself.
// Never below column 0. A level is indent_width columns.

#define EDIT_INDENT_MAX 512 // columns
#define EDIT_SYNC_US 250    // indentation brings lexer states up to its line within this, then uses what is stored

i64  edit_indent_cols(Buffer *buf, i64 line);                 // columns of the line's leading blanks
i64  edit_compute_indent(Buffer *buf, i64 line, i64 indent_width, Arena *scratch);
// A line TAB leaves where it is: it has text and starts inside a multi-line comment or string.
b32  edit_line_fixed(Buffer *buf, i64 line, Arena *scratch);
// Rewrites the line's leading blanks for `cols` (tabs then spaces with buf->indent_tabs), only if
// they differ. False if the buffer refused the edit.
b32  edit_set_indent(Buffer *buf, i64 line, i64 cols);
i32  edit_detect_tabs(Buffer *buf); // from the first indented lines: 1 tabs, 0 spaces, -1 cannot tell
void edit_electric_close(CommandContext *ctx); // after typing ) ] }: reindent if it starts the line

extern const Command CMD_QUOTED_INSERT; // app.c
extern const Command CMD_UNDO, CMD_UNDO_REDO, CMD_NEWLINE, CMD_INDENT_FOR_TAB_COMMAND, CMD_UNINDENT, CMD_TAB_TO_TAB_STOP;
extern const Command CMD_OPEN_LINE, CMD_DELETE_INDENTATION, CMD_DELETE_HORIZONTAL_SPACE, CMD_JUST_ONE_SPACE, CMD_TRANSPOSE_CHARS;
extern const Command CMD_UPCASE_WORD, CMD_DOWNCASE_WORD, CMD_CAPITALIZE_WORD, CMD_COMMENT_LINE;
extern const Command CMD_KILL_REGION, CMD_KILL_RING_SAVE, CMD_KILL_LINE, CMD_KILL_WORD, CMD_BACKWARD_KILL_WORD;
extern const Command CMD_KILL_WHOLE_LINE, CMD_YANK, CMD_YANK_POP;

#endif // EDIT_H
