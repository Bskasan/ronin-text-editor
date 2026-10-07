// view.h — an Emacs window onto a buffer: cursors, scrolling, motion and editing commands.
// Plain C, headless: positions and logic only; drawing lives in app.c.

#ifndef VIEW_H
#define VIEW_H

#define VIEW_TAB_WIDTH 4 // columns per tab stop (a setting later)

// ---------------------------------------------------------------------------
// Visual columns: a tab advances to the next multiple of VIEW_TAB_WIDTH, an ASCII control
// character (drawn as ^X) takes 2 cells, anything else (multi-byte, invalid byte) takes 1.

b32 view_is_control(u8 b); // 0x00-0x1F except tab and newline, and 0x7F
i64 view_char_width(u8 first_byte, i64 col); // cells of the character starting with first_byte at col

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
// Echo area messages. A message stays until the next key event (the app clears it).

#define ECHO_CAP 1024

typedef struct Echo {
    u8 text[ECHO_CAP];
    i32 len;
} Echo;

void echo_message(Echo *e, const char *fmt, ...); // the base.h formatter
void echo_clear(Echo *e);

// ---------------------------------------------------------------------------
// Views

#define VIEW_CURSOR_RESERVE MB(16)  // address space for the cursor array (~1M cursors)
#define VIEW_CONTEXT_LINES 2        // kept on screen by scroll-up/down-command (Emacs next-screen-context-lines)

typedef struct Cursor {
    BufferMarker point; // advances over text inserted at it
    BufferMarker mark;  // does not; unused until Phase 6
    i64 goal_col;       // visual column kept across consecutive vertical motions, -1 = none
} Cursor;

typedef struct View {
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
} View;

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

// ---------------------------------------------------------------------------
// Commands. A command is written for one cursor; its only argument is the context, which
// carries the View and that cursor. view_run_command is the single place that loops over the
// cursors (Phase 14 adds merging and ordered edits there); COMMAND_ONCE commands act on the
// View as a whole and run once, with the primary cursor.

typedef struct CommandContext CommandContext;
typedef void CommandFn(CommandContext *ctx);

enum {
    COMMAND_ONCE = 1 << 0,
};

typedef struct Command {
    const char *name; // the Emacs name
    CommandFn *fn;
    u32 flags;
} Command;

struct CommandContext {
    View *view;
    Cursor *cursor;   // the cursor being processed
    Echo *echo;
    u32 codepoint;    // self-insert: the typed character
    const Command *this_command;
    const Command *last_command;
};

void view_run_command(CommandContext *ctx, const Command *cmd);

extern const Command CMD_FORWARD_CHAR, CMD_BACKWARD_CHAR, CMD_NEXT_LINE, CMD_PREVIOUS_LINE;
extern const Command CMD_MOVE_BEGINNING_OF_LINE, CMD_MOVE_END_OF_LINE, CMD_FORWARD_WORD, CMD_BACKWARD_WORD;
extern const Command CMD_FORWARD_PARAGRAPH, CMD_BACKWARD_PARAGRAPH, CMD_BEGINNING_OF_BUFFER, CMD_END_OF_BUFFER;
extern const Command CMD_SCROLL_UP_COMMAND, CMD_SCROLL_DOWN_COMMAND, CMD_RECENTER_TOP_BOTTOM;

#endif // VIEW_H
