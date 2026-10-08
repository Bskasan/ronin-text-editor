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

extern const Command CMD_UNDO, CMD_UNDO_REDO;
extern const Command CMD_KILL_REGION, CMD_KILL_RING_SAVE, CMD_KILL_LINE, CMD_KILL_WORD, CMD_BACKWARD_KILL_WORD;
extern const Command CMD_KILL_WHOLE_LINE, CMD_YANK, CMD_YANK_POP;

#endif // EDIT_H
