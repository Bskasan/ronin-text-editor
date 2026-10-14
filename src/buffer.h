// buffer.h — a text buffer: gap buffer of raw bytes (treated as UTF-8) with an incremental
// line index, file load / save. Plain C; files go through the os_* functions of platform.h.
//
// Positions are byte offsets into the logical text. Every modification goes through
// buffer_replace; nothing else writes buffer memory (loading fills a fresh buffer).

#ifndef BUFFER_H
#define BUFFER_H

#define BUFFER_TEXT_RESERVE GB(2) // text capacity; the line index reserves 4 bytes per text byte
#define BUFFER_META_RESERVE MB(1) // the Buffer struct, path and name
#define BUFFER_MAX_FILE_SIZE GB(1)
#define BUFFER_MARKER_RESERVE MB(64) // marker slots, 16 bytes each
#define BUFFER_DEFAULT_TAB_WIDTH 4
#define BUFFER_UNDO_RESERVE GB(4)          // the undo log of one buffer (address space)
#define BUFFER_UNDO_GROUP_RESERVE MB(256)  // its group index
#define BUFFER_UNDO_DEFAULT_LIMIT MB(64)   // the app sets undo_limit_mb
#define BUFFER_UNDO_MERGE_MAX 20           // consecutive self-inserts (or single deletes) per group

typedef enum BufferEncoding {
    BUFFER_UTF8,
    BUFFER_UTF8_BOM,
    BUFFER_UTF16LE,
    BUFFER_UTF16BE,
} BufferEncoding;

typedef enum BufferEol {
    BUFFER_EOL_LF,
    BUFFER_EOL_CRLF,  // stored as LF, written as CRLF
    BUFFER_EOL_MIXED, // stored and written byte for byte
} BufferEol;

typedef enum BufferLanguage {
    BUFFER_LANG_FUNDAMENTAL,
    BUFFER_LANG_JAI,
    BUFFER_LANG_C,
    BUFFER_LANG_CPP,
    BUFFER_LANG_CSHARP,
    BUFFER_LANG_JAVASCRIPT,
    BUFFER_LANG_TYPESCRIPT,
} BufferLanguage;

enum {
    BUFFER_KEYMAP_NONE,
    BUFFER_KEYMAP_SEARCH_RESULTS, // [keys search-results]: *search*, *occur*
    BUFFER_KEYMAP_COMPILATION,    // [keys compilation]: *compilation*
};

// A position that survives edits: buffer_replace adjusts every live marker. Handle = slot + 1.
typedef u32 BufferMarker;

enum {
    BUFFER_MARKER_LIVE      = 1 << 0,
    BUFFER_MARKER_ADVANCE   = 1 << 1, // insertion type: text inserted exactly at the marker goes before it
    BUFFER_MARKER_SWALLOWED = 1 << 2, // in a batch: it lay strictly inside a range an edit of the batch removed
};

// Batches (buffer_batch_begin / _end, Phase 14): the edits of a multiple-cursor command, or of a large undo,
// adjust the markers once instead of every marker on every edit. Inside a batch buffer_replace does all it
// does otherwise but leave the markers: it appends the edit to the batch's list. The list is one run of edits
// in order, ascending (each starts at or after the end of the text the previous one inserted) or descending
// (each ends at or before the start of the previous one); an edit out of order first applies the run to every
// marker (a flush) and starts a new one, so the order only decides the speed, never the result. Each live
// marker slot keeps the run's edit count when it was last set (its epoch, in next_free): buffer_marker_get maps
// its position through the edits since then (a binary search, then the marker rules while it touches the next
// edit). The end of a batch applies the run to every marker: O(markers x log edits) instead of O(markers x
// edits). A marker is snapped to a character boundary against the text at the end of the run (not after each
// edit; the two differ only when invalid bytes join across two nearby edits).
#define BUFFER_BATCH_RESERVE MB(256) // the edit list (address space, reserved by a buffer's first batch)

typedef struct BufferBatchEdit {
    i64 start, removed, inserted; // as buffer_replace got them
    i64 delta_before;             // the sum of (inserted - removed) over the run's earlier edits
    i64 split;                    // inserted, or for a joined edit the length of its first part (buffer.c)
} BufferBatchEdit;

// The visited file, as the app last saw it on disk (files changed outside the editor).
typedef enum BufferDiskState {
    BUFFER_DISK_OK,      // as read or saved
    BUFFER_DISK_CHANGED, // changed since, not reloaded (the buffer has unsaved changes, or auto_revert is off)
    BUFFER_DISK_DELETED, // gone; the buffer stays
} BufferDiskState;

typedef struct BufferMarkerSlot {
    i64 pos;
    u32 flags;
    u32 next_free; // free list: next free slot + 1, 0 = end
} BufferMarkerSlot;

// ---------------------------------------------------------------------------
// Undo. buffer_replace logs every edit: a record is {start, removed, inserted} followed by the
// removed bytes only (undoing an insertion needs just its length). Records form groups, one per
// command (the command driver calls buffer_undo_boundary); consecutive self-inserts or single
// deletes merge into groups of up to BUFFER_UNDO_MERGE_MAX. Undo applies a group's inverse
// through buffer_replace, so it is logged as a group too and can itself be undone (Emacs).
//
// Every logged edit yields a new state id; an undo group's state_after is the state before the
// group it reverts. The buffer is unmodified exactly when its state is the state at the last
// load or save, so undoing (or redoing) back to it clears the modified flag.

typedef enum BufferUndoMerge {
    BUFFER_UNDO_MERGE_NONE,
    BUFFER_UNDO_MERGE_INSERT,
    BUFFER_UNDO_MERGE_DELETE,
} BufferUndoMerge;

typedef enum BufferUndoResult {
    BUFFER_UNDO_DONE,
    BUFFER_UNDO_NOTHING, // no (further) undo / redo information
    BUFFER_UNDO_FAILED,  // read-only, or an edit did not fit
} BufferUndoResult;

typedef struct BufferUndoRecord { // in the log, followed by `removed` bytes (padded to 8)
    i64 start;
    i64 removed;
    i64 inserted;
    u64 prev; // bytes back to the previous record of its group, 0 for the first
} BufferUndoRecord;

// A cursor as a multiple-cursor command found it (Phase 14): kept with the command's group, put back by undo.
typedef struct BufferUndoCursor {
    i64 point, mark;
    u32 flags; // BUFFER_UNDO_CURSOR_*
    u32 unused;
} BufferUndoCursor;

enum { BUFFER_UNDO_CURSOR_MARK_SET = 1 << 0, BUFFER_UNDO_CURSOR_PRIMARY = 1 << 1 };

typedef struct BufferUndoGroup {
    u64 offset, size;             // its records in the log (after its cursors, if it has them)
    u64 last;                     // offset of its last record
    u64 cursors_off;              // its cursors in the log (cursor_count of them, before its first record)
    i64 cursor_count;             // 0: made with one cursor (point_before says where it was)
    i64 point_before;             // the primary point before the command; where undo puts point
    u64 state_before, state_after;
    i64 target;                   // an undo group: the id of the group it reverts; -1 otherwise
    i64 records;                  // edits logged in it
    i32 merge, merge_count;       // BufferUndoMerge of the commands merged into it
    b32 backward;                 // it moved back in history: the undo of a forward group (normal groups go forward)
} BufferUndoGroup;

typedef struct BufferUndo {
    b32 enabled;
    u8 *log;
    u64 log_committed, log_used;
    BufferUndoGroup *groups;
    i64 group_committed, group_count; // entries
    i64 first_id;                     // the id of groups[0]; older groups were dropped by the limit
    u64 limit;                        // bytes of records
    b32 open;                         // the last group still takes records (merging, or the running command)
    BufferUndoMerge want_merge;       // the next group, set by buffer_undo_boundary
    i64 want_point;
    const BufferUndoCursor *want_cursors; // the next group's cursors (buffer_undo_cursors; the caller's memory)
    i64 want_cursor_count;
    u64 restored_off;                 // the cursors the last undo or redo put back (buffer_undo_restored)
    i64 restored_count;
    b32 applying;                     // inside buffer_undo / buffer_redo: no merging, no trimming
    u64 state, next_state, saved_state;
    i64 pending;                      // the undo chain: the next group to undo, -1 = none
} BufferUndo;

typedef struct Buffer {
    Arena meta; // holds this struct, path and name

    // Text: [0, gap_start) and [gap_end, text_cap) of `text`. text_cap is the committed size.
    u8 *text;
    i64 text_reserved;
    i64 text_cap;
    i64 gap_start, gap_end;

    // Line index: the positions of the '\n' bytes, in their own gap array paired with the text
    // gap. [0, nl_front) are absolute positions of the newlines before the text gap;
    // [nl_back, nl_cap) are distances from the end of the text (size - pos) of the newlines
    // after it, in increasing position order. An edit at the gap leaves the back entries valid.
    u32 *nl;
    i64 nl_reserved; // entries
    i64 nl_cap;      // committed entries
    i64 nl_front, nl_back;

    // Lexer states (highlighting): the state at the start of each line, next to the line index.
    // line_state[k] belongs to nl[k] (same gap, same indexes) and is the start state of line
    // k + 1; line 0 starts in state 0. Reserved and committed only while states_on. The values
    // mean nothing here: the syntax layer computes them; buffer_replace keeps them with their
    // lines and tracks what an edit made untrustworthy.
    u32 *line_state;
    b32 states_on;
    i32 states_language; // the BufferLanguage they were computed for (set by the syntax layer)
    i64 state_valid; // lines [0, state_valid] have their correct start state
    i64 state_dirty; // the last line whose text changed since; states may only converge after it (-1: none)
    i64 state_known; // lines [0, state_known) have a stored state (possibly stale); the rest have none

    // Markers: a flat slot array in its own reservation; freed slots are reused.
    BufferMarkerSlot *markers;
    i64 marker_reserved; // slots
    i64 marker_cap;      // committed slots
    i64 marker_count;    // slots ever used (live or free)
    u32 marker_free;     // first free slot + 1, 0 = none
    i64 marker_live;
    // The live slots, densely: the passes over every marker visit only these, so slots once used by many markers
    // (100,000 cursors gone again) cost nothing later. marker_live_at[slot] is the slot's index in the list.
    u32 *marker_live_list, *marker_live_at;

    // A batch (see above).
    i32 batch_depth;     // > 0: inside a batch (they nest; the outermost end applies)
    i32 batch_dir;       // the run's order: 1 ascending, -1 descending, 0 not decided yet (each pair fits both)
    i64 batch_pinned;    // the run's edit count when a marker was last set in it (a join keeps that edit as it is)
    BufferBatchEdit *batch_edits;
    i64 batch_count, batch_committed; // entries
#if RONIN_DEV
    u64 dev_batch_flushes; // runs applied before their batch ended (an edit out of order, or a full list)
    u64 dev_batch_joins;   // edits joined to the insertion before them (buffer.c)
    BufferBatchEdit *dev_edits; // tests: every successful edit appended here (NULL: off), dev_edit_cap at most
    i64 dev_edit_count, dev_edit_cap;
#endif

    String8 path; // full, normalized; empty when not visiting a file
    String8 name;
    BufferEncoding encoding;
    BufferEol eol;
    BufferLanguage language; // from the file extension, set with the path
    b32 modified;
    b32 read_only;
    b32 inhibit_read_only; // set around program-made edits of read-only buffers (build output)
    u64 edit_count;
    i64 file_size;  // as of the last load or save
    u64 file_time;  // last write time, same
    BufferDiskState disk_state; // set by the app's checks; a load, save or revert makes it OK
    i32 tab_width;  // columns per tab stop (Emacs' buffer-local tab-width; the app sets it from the config)
    b32 indent_tabs;     // indentation with tabs (indent-tabs-mode): from the config, or detected
    b32 indent_detected; // indent_tabs was decided from the file's contents
    BufferUndo undo;
    // The buffer's own keymap, placed before the global one in the keymap stack (BUFFER_KEYMAP_*; the app
    // owns the keymaps, buffer.c never reads this).
    i32 keymap;
#if RONIN_DEV
    b32 dev_saved_by_swap; // the last save went through the temp file and ReplaceFileW / MoveFileExW (not in place)
#endif
} Buffer;

Buffer *buffer_create(String8 name); // NULL if the address space cannot be reserved
Buffer *buffer_create_reserve(String8 name, i64 text_reserve);
b32     buffer_destroy(Buffer *buf); // false if some memory could not be released

i64 buffer_size(Buffer *buf);
i64 buffer_line_count(Buffer *buf); // newlines + 1

// Replaces [start, end) with `text`. Insert = empty range, delete = empty text. Returns false,
// leaving the buffer unchanged, for a read-only buffer (unless inhibit_read_only), an invalid
// range, or when the result would not fit (capacity or commit failure).
b32 buffer_replace(Buffer *buf, i64 start, i64 end, String8 text);

i64 buffer_line_start(Buffer *buf, i64 line); // line clamped to [0, line_count)
i64 buffer_line_end(Buffer *buf, i64 line);   // excluding the '\n'
i64 buffer_line_of(Buffer *buf, i64 offset);  // the line containing offset

u8      buffer_byte(Buffer *buf, i64 offset);
void    buffer_copy(Buffer *buf, i64 start, i64 end, u8 *dst);
// Points into buffer memory unless the range straddles the gap; then it is copied to `scratch`.
String8 buffer_text(Buffer *buf, Arena *scratch, i64 start, i64 end);
String8 buffer_line(Buffer *buf, Arena *scratch, i64 line);
void    buffer_segments(Buffer *buf, String8 *before, String8 *after); // the text before and after the gap

// Character boundaries: a valid UTF-8 sequence or a single invalid byte is one unit.
i64 buffer_next_char(Buffer *buf, i64 offset);
i64 buffer_prev_char(Buffer *buf, i64 offset);
i64 buffer_snap_char(Buffer *buf, i64 offset); // the boundary at or before offset (clamped to the buffer)

// Markers. In buffer_replace(start, end, text) a marker before start stays; after end it
// shifts; exactly at end (of a non-empty range) it goes to start + text.len; inside the range
// or at start it goes to start, or to start + text.len if it advances. Markers near the edit
// are then snapped to a character boundary (joined invalid bytes can form a valid character).
BufferMarker buffer_marker_create(Buffer *buf, i64 pos, b32 advance); // pos clamped and snapped
void         buffer_marker_destroy(Buffer *buf, BufferMarker m);
i64          buffer_marker_get(Buffer *buf, BufferMarker m);
void         buffer_marker_set(Buffer *buf, BufferMarker m, i64 pos); // clamped and snapped
// Batches (see above). A marker swallowed in the batch lay strictly inside a range one of its edits removed
// since the marker was last set (false outside a batch).
void buffer_batch_begin(Buffer *buf);
void buffer_batch_end(Buffer *buf);
b32  buffer_marker_swallowed(Buffer *buf, BufferMarker m);

// Undo (see above). Undo is on for a new buffer; program buffers (*Messages*) turn it off.
void buffer_undo_enable(Buffer *buf, b32 on);      // off also drops the log
void buffer_undo_set_limit(Buffer *buf, u64 bytes); // the oldest groups are dropped above it
// Called by the command driver before every command: the next edit starts a new group unless
// `merge` is the open group's class, `consecutive` (the same command again) and the group has
// fewer than BUFFER_UNDO_MERGE_MAX commands. `point` is restored when the group is undone.
void buffer_undo_boundary(Buffer *buf, BufferUndoMerge merge, b32 consecutive, i64 point);
// The cursors of the command about to run when there are several (the driver, after the boundary; NULL, 0 once
// it ran): a group the command starts keeps a copy, and undoing that group puts them back. The memory must stay
// valid while the command runs.
void buffer_undo_cursors(Buffer *buf, const BufferUndoCursor *cursors, i64 count);
// After an undo or redo that returned BUFFER_UNDO_DONE: the cursors of the group it reverted (count 0: that group
// was made with one cursor; *point_out says where). Valid until the buffer's next edit.
const BufferUndoCursor *buffer_undo_restored(Buffer *buf, i64 *count);
// Undoes one group: the most recent one, or with `chain` (the previous command was an undo) the
// one before the group undone last (Emacs' pending undo list). *point_out gets the
// point to restore.
BufferUndoResult buffer_undo(Buffer *buf, b32 chain, i64 point, i64 *point_out);
// Emacs' undo-redo: undoes the most recent group that moved back in history (the undo of an
// edit, not of an undo) and led to the current state.
BufferUndoResult buffer_redo(Buffer *buf, i64 point, i64 *point_out);
void buffer_mark_saved(Buffer *buf);  // the current state is the saved one: unmodified
u64  buffer_undo_memory(Buffer *buf); // committed bytes of the log and its group index

// Lexer states (see Buffer). Enabling commits one u32 per line, with every state untrusted (only
// line 0 is known); disabling gives the memory back. An edit replacing lines [a, b] by [a, a + n]
// lowers state_valid to a, raises state_dirty to a + n, and gives the new lines line a's state.
b32  buffer_states_enable(Buffer *buf, b32 on); // false if the memory could not be committed (then off)
u32  buffer_line_state(Buffer *buf, i64 line);   // the stored start state of a line (0 for line 0)
void buffer_set_line_state(Buffer *buf, i64 line, u32 state); // line >= 1
u64  buffer_states_memory(Buffer *buf);          // committed bytes

// Files. Loading needs an empty buffer; on failure it stays empty and the status says why.
// Opening a missing file gives OS_FILE_NOT_FOUND; the caller may then visit the path as a new file.
OsFileStatus buffer_load_file(Buffer *buf, String8 path);
// Reads the visited file again: loaded into a temporary buffer (so encodings and line endings are
// handled as on load), then the part between the common prefix and the common suffix of the old
// and new text is replaced with one buffer_replace, as an undo group of its own (`point` is where
// undoing it puts point). Markers inside the replaced range, or at its start, are put back at
// their line and character column (clamped); the others adjust as for any edit. The undo log is
// kept, so the change can be undone (the buffer is then modified). Encoding, line endings, the
// read-only attribute and the file's size and time are taken over and the buffer counts as saved.
// An identical text is no edit. On failure the buffer is untouched.
OsFileStatus buffer_revert(Buffer *buf, i64 point);
void         buffer_set_path(Buffer *buf, String8 full_path); // also sets the name and the language
const char  *buffer_language_name(BufferLanguage language);    // "C", "C++", "Fundamental", ...
// Saving writes a temp file next to the target, flushes it to disk and swaps it in with
// ReplaceFileW, or writes in place when a swap is not possible. Encoding and line-ending mode
// are the buffer's. A read-only target is refused untouched. Save-as visits the new path.
OsFileStatus buffer_save(Buffer *buf);
OsFileStatus buffer_save_opt(Buffer *buf, b32 flush); // flush = 0: no FlushFileBuffers (fsync_on_save = false)
OsFileStatus buffer_save_as(Buffer *buf, String8 path);
OsFileStatus buffer_save_as_opt(Buffer *buf, String8 path, b32 flush);
const char  *buffer_status_text(OsFileStatus status);           // "access denied", ...

#endif // BUFFER_H
