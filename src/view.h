// view.h — an Emacs window onto a buffer: cursors, scrolling, motion and editing commands.
// Plain C, headless: positions and logic only; drawing lives in app.c.

#ifndef VIEW_H
#define VIEW_H

// ---------------------------------------------------------------------------
// Visual columns: a tab advances to the next multiple of the buffer's tab width, an ASCII
// control character (drawn as ^X) takes 2 cells, anything else (multi-byte, invalid byte) takes 1.

b32 view_is_control(u8 b); // 0x00-0x1F except tab and newline, and 0x7F
i64 view_char_width(u8 first_byte, i64 col, i64 tab_width); // cells of the character starting with first_byte at col

// Walks characters from `pos` (at column *col) towards `end`, stopping before the first one
// that would end past `stop_col`; *col becomes the column of the returned position. Scans
// only what it passes. `end` must be a character boundary on the same line.
i64 view_walk(Buffer *buf, i64 pos, i64 end, i64 *col, i64 stop_col);

i64 view_column_of(Buffer *buf, i64 offset); // visual column of offset in its line
// The character boundary on `line` nearest to visual column `col`: the start of the character
// covering col when col is in its first half (ties included), its end otherwise; the end of
// the line past its last column.
i64 view_offset_at_column(Buffer *buf, i64 line, i64 col);

// ---------------------------------------------------------------------------
// Echo area messages. A message stays until the next key event (the app clears it). Every
// message is also appended to the log buffer (*Messages*), which keeps the last ECHO_LOG_LINES.

#define ECHO_CAP 1024
#define ECHO_LOG_LINES 1000

struct Echo {
    u8 text[ECHO_CAP];
    i32 len;
    Buffer *log; // *Messages*, read-only, written through inhibit_read_only; NULL = not logged
};

void echo_message(Echo *e, const char *fmt, ...); // shown and logged (the base.h formatter)
void echo_set(Echo *e, String8 text);              // shown only (a key prefix such as "C-x-")
void echo_log(Echo *e, String8 text);              // logged only (config diagnostics)
void echo_clear(Echo *e);

// ---------------------------------------------------------------------------
// Views

#define VIEW_CURSOR_RESERVE MB(16)  // address space for the cursor array (~1M cursors)
#define VIEW_CONTEXT_LINES 2        // kept on screen by scroll-up/down-command (Emacs next-screen-context-lines)

struct Cursor {
    BufferMarker point; // advances over text inserted at it
    BufferMarker mark;  // does not
    i64 goal_col;       // visual column kept across consecutive vertical motions, -1 = none
    b32 mark_set;       // the mark has been set in this buffer (the region exists)
    b32 mark_active;    // transient mark mode: the region is active (highlighted)
    b32 mark_shift;     // activated by shift-select: an unshifted motion deactivates it
};

struct View {
    Buffer *buffer;
    Arena cursor_arena; // holds only `cursors`, so the array stays contiguous
    Cursor *cursors;    // [0] is the primary cursor; visibility follows it
    i32 cursor_count;
    BufferMarker top;   // start of the first visible line; does not advance
    i64 left_col;       // first visible visual column
    i32 x, y, w, h;     // pixel rect, from the layout
    i32 rows, cols;     // full text cells, from the layout (or a test)
    i32 recenter_step;  // recenter-top-bottom: 0 center, 1 top, 2 bottom
    i32 recenter_row;   // a command's request: if point is off screen, put its line on this row; -1 = center
};

View   *view_create(Arena *arena, Buffer *buf); // one cursor at 0, scrolled to the top
void    view_destroy(View *view);               // releases its markers and the cursor array
Cursor *view_add_cursor(View *view, i64 pos);
i64     view_point(View *view, Cursor *cursor);
void    view_set_point(View *view, Cursor *cursor, i64 pos);
i64     view_top_line(View *view);

// Point is always visible: if the primary point is outside the window, the window recenters
// on it (or uses recenter_row). Also puts the top on a line start and scrolls horizontally.
// Runs after every command (view_run_command) and after every layout.
void view_ensure_visible(View *view);
void view_scroll_lines(View *view, i64 lines);             // the wheel: moves the top, drags point along
void view_set_point_at(View *view, i64 row, i64 col);      // a click: text row and absolute visual column
void view_goto_line_column(View *view, i64 line, i64 col); // 0-based; the line is clamped

// Mark and region. The region is between point and mark; it exists once the mark is set and is
// active (highlighted, used by region commands) while mark_active.
void view_set_mark(View *view, Cursor *cursor, i64 pos, b32 active);
void view_deactivate_mark(View *view); // every cursor
b32  view_region(View *view, Cursor *cursor, i64 *start, i64 *end); // false when the mark is not set
b32  view_region_active(View *view, Cursor *cursor, const Settings *settings); // transient mark mode and active
// The run around pos that a double click selects: a word, a run of blanks, or one other character.
void view_word_bounds(Buffer *buf, i64 pos, b32 underscore_is_word, i64 *start, i64 *end);
i64  view_forward_word(Buffer *buf, i64 pos, b32 underscore_is_word);  // the end of the next word
i64  view_backward_word(Buffer *buf, i64 pos, b32 underscore_is_word); // the start of the previous word

// ---------------------------------------------------------------------------
// The buffer list. Each entry remembers where its buffer was last shown, so switching a view
// away and back restores point and the scroll position (Emacs keeps a point per buffer).

#define BUFFER_LIST_RESERVE MB(16) // address space for the entries

typedef struct BufferEntry {
    Buffer *buffer;
    BufferMarker point, top; // saved when a view switches away from the buffer
    i64 left_col;
} BufferEntry;

typedef struct BufferList {
    Arena arena; // holds only `entries`, so the array stays contiguous
    BufferEntry *entries;
    i32 count;
} BufferList;

void         buffer_list_init(BufferList *list);
// Releases its markers, then destroys every buffer and the array. Returns what leaked: markers
// still live (views must be destroyed first) and buffers whose memory could not be released.
i32          buffer_list_destroy(BufferList *list);
BufferEntry *buffer_list_add(BufferList *list, Buffer *buf);
i32          buffer_list_index(BufferList *list, Buffer *buf); // -1 if not listed
// The buffer visiting `full_path` (compared case-insensitively for ASCII, as Windows does), or NULL.
Buffer      *buffer_list_find_path(BufferList *list, String8 full_path);
// Shows `buf` in the view: saves where the view was in its old buffer, then restores where `buf`
// was last shown (the start for a new one). One cursor afterwards.
void         view_switch_buffer(View *view, BufferList *list, Buffer *buf);

// ---------------------------------------------------------------------------
// Commands (command.h). view_run_command is the single place that loops over the cursors
// (Phase 14 adds merging and ordered edits there); COMMAND_ONCE commands act on the View as a
// whole and run once, with the primary cursor.

void view_run_command(CommandContext *ctx, const Command *cmd);

extern const Command CMD_FORWARD_CHAR, CMD_BACKWARD_CHAR, CMD_NEXT_LINE, CMD_PREVIOUS_LINE;
extern const Command CMD_MOVE_BEGINNING_OF_LINE, CMD_MOVE_END_OF_LINE, CMD_FORWARD_WORD, CMD_BACKWARD_WORD;
extern const Command CMD_FORWARD_PARAGRAPH, CMD_BACKWARD_PARAGRAPH, CMD_BEGINNING_OF_BUFFER, CMD_END_OF_BUFFER;
extern const Command CMD_SCROLL_UP_COMMAND, CMD_SCROLL_DOWN_COMMAND, CMD_RECENTER_TOP_BOTTOM;
extern const Command CMD_SELF_INSERT, CMD_NEWLINE, CMD_DELETE_BACKWARD_CHAR, CMD_DELETE_CHAR, CMD_SAVE_BUFFER;
extern const Command CMD_KEYBOARD_QUIT, CMD_SET_MARK_COMMAND, CMD_EXCHANGE_POINT_AND_MARK, CMD_MARK_WHOLE_BUFFER;

#endif // VIEW_H
