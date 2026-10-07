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

#endif // VIEW_H
