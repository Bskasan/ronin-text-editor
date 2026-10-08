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

// A position that survives edits: buffer_replace adjusts every live marker. Handle = slot + 1.
typedef u32 BufferMarker;

enum {
    BUFFER_MARKER_LIVE    = 1 << 0,
    BUFFER_MARKER_ADVANCE = 1 << 1, // insertion type: text inserted exactly at the marker goes before it
};

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

typedef struct BufferUndoGroup {
    u64 offset, size;             // its records in the log
    u64 last;                     // offset of its last record
    i64 point_before;             // the primary point before the command; where undo puts point
    u64 state_before, state_after;
    i64 target;                   // an undo group: the id of the group it reverts; -1 otherwise
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

    // Markers: a flat slot array in its own reservation; freed slots are reused.
    BufferMarkerSlot *markers;
    i64 marker_reserved; // slots
    i64 marker_cap;      // committed slots
    i64 marker_count;    // slots ever used (live or free)
    u32 marker_free;     // first free slot + 1, 0 = none
    i64 marker_live;

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
    i32 tab_width;  // columns per tab stop (Emacs' buffer-local tab-width; the app sets it from the config)
    b32 indent_tabs;     // indentation with tabs (indent-tabs-mode): from the config, or detected
    b32 indent_detected; // indent_tabs was decided from the file's contents
    BufferUndo undo;
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

// Undo (see above). Undo is on for a new buffer; program buffers (*Messages*) turn it off.
void buffer_undo_enable(Buffer *buf, b32 on);      // off also drops the log
void buffer_undo_set_limit(Buffer *buf, u64 bytes); // the oldest groups are dropped above it
// Called by the command driver before every command: the next edit starts a new group unless
// `merge` is the open group's class, `consecutive` (the same command again) and the group has
// fewer than BUFFER_UNDO_MERGE_MAX commands. `point` is restored when the group is undone.
void buffer_undo_boundary(Buffer *buf, BufferUndoMerge merge, b32 consecutive, i64 point);
// Undoes one group: the most recent one, or with `chain` (the previous command was an undo) the
// one before the group undone last (Emacs' pending undo list). *point_out gets the
// point to restore.
BufferUndoResult buffer_undo(Buffer *buf, b32 chain, i64 point, i64 *point_out);
// Emacs' undo-redo: undoes the most recent group that moved back in history (the undo of an
// edit, not of an undo) and led to the current state.
BufferUndoResult buffer_redo(Buffer *buf, i64 point, i64 *point_out);
void buffer_mark_saved(Buffer *buf);  // the current state is the saved one: unmodified
u64  buffer_undo_memory(Buffer *buf); // committed bytes of the log and its group index

// Files. Loading needs an empty buffer; on failure it stays empty and the status says why.
// Opening a missing file gives OS_FILE_NOT_FOUND; the caller may then visit the path as a new file.
OsFileStatus buffer_load_file(Buffer *buf, String8 path);
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
