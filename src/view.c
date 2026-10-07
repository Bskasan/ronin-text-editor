// view.c — see view.h.

// ---------------------------------------------------------------------------
// Visual columns

b32 view_is_control(u8 b) {
    return (b < 0x20 && b != '\t' && b != '\n') || b == 0x7F;
}

i64 view_char_width(u8 first_byte, i64 col) {
    if (first_byte == '\t') return VIEW_TAB_WIDTH - col % VIEW_TAB_WIDTH;
    return view_is_control(first_byte) ? 2 : 1;
}

i64 view_walk(Buffer *buf, i64 pos, i64 end, i64 *io_col, i64 stop_col) {
    i64 col = *io_col;
    i64 gap = buf->gap_end - buf->gap_start;
    while (pos < end) {
        // A contiguous run on one side of the gap; p[pos] is the byte at pos.
        u8 *p = pos < buf->gap_start ? buf->text : buf->text + gap;
        i64 run_end = pos < buf->gap_start ? MIN(end, buf->gap_start) : end;
        while (pos < run_end) {
            u8 b = p[pos];
            if (b >= 0x20 && b < 0x7F) { // printable ASCII: the common case
                if (col + 1 > stop_col) goto done;
                col++;
                pos++;
                continue;
            }
            if (b >= 0x80) {
                if (col + 1 > stop_col) goto done;
                i64 advance;
                i64 avail = run_end - pos;
                if (avail >= 4 || run_end == end) utf8_decode(p + pos, avail, &advance); // cannot straddle anything
                else advance = buffer_next_char(buf, pos) - pos;                         // may straddle the gap
                col++;
                pos += advance;
                continue;
            }
            i64 w = view_char_width(b, col);
            if (col + w > stop_col) goto done;
            col += w;
            pos++;
        }
    }
done:
    *io_col = col;
    return pos;
}

i64 view_column_of(Buffer *buf, i64 offset) {
    i64 col = 0;
    view_walk(buf, buffer_line_start(buf, buffer_line_of(buf, offset)), offset, &col, I64_MAX);
    return col;
}

i64 view_offset_at_column(Buffer *buf, i64 line, i64 col) {
    i64 c = 0;
    i64 end = buffer_line_end(buf, line);
    i64 pos = view_walk(buf, buffer_line_start(buf, line), end, &c, col);
    if (pos == end) return end;
    // The character at pos covers [c, c + w) and col is inside it.
    i64 w = view_char_width(buffer_byte(buf, pos), c);
    return 2 * (col - c) <= w ? pos : buffer_next_char(buf, pos);
}

// ---------------------------------------------------------------------------
// Echo area

void echo_message(Echo *e, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    i64 n = fmt_v(e->text, ECHO_CAP, fmt, args);
    va_end(args);
    e->len = (i32)MIN(n, (i64)ECHO_CAP);
}

void echo_clear(Echo *e) {
    e->len = 0;
}

// ---------------------------------------------------------------------------
// Views

View *view_create(Arena *arena, Buffer *buf) {
    View *v = PUSH_STRUCT(arena, View);
    v->buffer = buf;
    v->cursor_arena = arena_create(VIEW_CURSOR_RESERVE);
    v->cursors = (Cursor *)v->cursor_arena.base;
    v->top = buffer_marker_create(buf, 0, 0);
    v->rows = v->cols = 1;
    v->recenter_row = -1;
    view_add_cursor(v, 0);
    return v;
}

void view_destroy(View *v) {
    for (i32 i = 0; i < v->cursor_count; i++) {
        buffer_marker_destroy(v->buffer, v->cursors[i].point);
        buffer_marker_destroy(v->buffer, v->cursors[i].mark);
    }
    buffer_marker_destroy(v->buffer, v->top);
    os_release(v->cursor_arena.base);
    v->cursor_count = 0;
}

Cursor *view_add_cursor(View *v, i64 pos) {
    Cursor *c = PUSH_STRUCT(&v->cursor_arena, Cursor);
    ASSERT(c == v->cursors + v->cursor_count);
    c->point = buffer_marker_create(v->buffer, pos, 1);
    c->mark = buffer_marker_create(v->buffer, pos, 0);
    c->goal_col = -1;
    v->cursor_count++;
    return c;
}

i64 view_point(View *v, Cursor *c) {
    return buffer_marker_get(v->buffer, c->point);
}

void view_set_point(View *v, Cursor *c, i64 pos) {
    buffer_marker_set(v->buffer, c->point, pos);
}

i64 view_top_line(View *v) {
    return buffer_line_of(v->buffer, buffer_marker_get(v->buffer, v->top));
}

static void view_set_top_line(View *v, i64 line) {
    buffer_marker_set(v->buffer, v->top, buffer_line_start(v->buffer, line));
}

// Cells the cursor covers at pos: 2 on a control character, 1 elsewhere (end of line, tab).
static i64 view_cursor_width(Buffer *buf, i64 pos) {
    return pos < buffer_size(buf) && view_is_control(buffer_byte(buf, pos)) ? 2 : 1;
}

void view_ensure_visible(View *v) {
    Buffer *buf = v->buffer;
    i64 rows = MAX(v->rows, 1), cols = MAX(v->cols, 1);
    i64 top = view_top_line(v);
    i64 p = view_point(v, &v->cursors[0]);
    i64 line = buffer_line_of(buf, p);
    if (line < top || line >= top + rows) {
        i64 row = v->recenter_row >= 0 ? MIN((i64)v->recenter_row, rows - 1) : rows / 2;
        top = MAX(line - row, 0);
    }
    v->recenter_row = -1;
    view_set_top_line(v, top); // also puts the top back on a line start after an edit

    i64 col = view_column_of(buf, p);
    i64 w = view_cursor_width(buf, p);
    if (col + w <= cols) v->left_col = 0; // everything up to point fits
    else if (col < v->left_col || col + w > v->left_col + cols) v->left_col = MAX(col - cols / 2, 0);
}

// After the top moved: a primary point left outside the window goes to the start of the
// nearest visible line (Emacs).
static void view_drag_point(View *v) {
    Buffer *buf = v->buffer;
    Cursor *c = &v->cursors[0];
    i64 top = view_top_line(v);
    i64 bottom = MIN(top + MAX(v->rows, 1) - 1, buffer_line_count(buf) - 1);
    i64 line = buffer_line_of(buf, view_point(v, c));
    if (line < top) view_set_point(v, c, buffer_line_start(buf, top));
    else if (line > bottom) view_set_point(v, c, buffer_line_start(buf, bottom));
}

void view_scroll_lines(View *v, i64 lines) {
    i64 last = buffer_line_count(v->buffer) - 1;
    view_set_top_line(v, CLAMP(view_top_line(v) + lines, 0, last));
    view_drag_point(v);
}

void view_set_point_at(View *v, i64 row, i64 col) {
    i64 last = buffer_line_count(v->buffer) - 1;
    i64 line = MIN(view_top_line(v) + MAX(row, 0), last);
    view_set_point(v, &v->cursors[0], view_offset_at_column(v->buffer, line, MAX(col, 0)));
}

void view_goto_line_column(View *v, i64 line, i64 col) {
    line = CLAMP(line, 0, buffer_line_count(v->buffer) - 1);
    view_set_point(v, &v->cursors[0], view_offset_at_column(v->buffer, line, MAX(col, 0)));
}

void view_run_command(CommandContext *ctx, const Command *cmd) {
    View *v = ctx->view;
    ctx->this_command = cmd;
    if (cmd->flags & COMMAND_ONCE) {
        ctx->cursor = &v->cursors[0];
        cmd->fn(ctx);
    } else {
        for (i32 i = 0; i < v->cursor_count; i++) {
            ctx->cursor = &v->cursors[i];
            cmd->fn(ctx);
        }
    }
    ctx->cursor = NULL;
    view_ensure_visible(v);
    ctx->last_command = cmd;
}

// ---------------------------------------------------------------------------
// Motion commands (one cursor each)

static i64 cmd_point(CommandContext *ctx) {
    return view_point(ctx->view, ctx->cursor);
}

static void cmd_goto(CommandContext *ctx, i64 pos) {
    view_set_point(ctx->view, ctx->cursor, pos);
}

static void cmd_forward_char(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    i64 p = cmd_point(ctx);
    if (p >= buffer_size(buf)) echo_message(ctx->echo, "End of buffer");
    else cmd_goto(ctx, buffer_next_char(buf, p));
}

static void cmd_backward_char(CommandContext *ctx) {
    i64 p = cmd_point(ctx);
    if (p <= 0) echo_message(ctx->echo, "Beginning of buffer");
    else cmd_goto(ctx, buffer_prev_char(ctx->view->buffer, p));
}

// next-line / previous-line: to the goal column of the line `dir` away. The goal is taken from
// point unless the previous command was a vertical motion too.
static void cmd_vertical(CommandContext *ctx, i64 dir) {
    Buffer *buf = ctx->view->buffer;
    Cursor *c = ctx->cursor;
    i64 p = cmd_point(ctx);
    b32 continuing = ctx->last_command == &CMD_NEXT_LINE || ctx->last_command == &CMD_PREVIOUS_LINE;
    if (!continuing || c->goal_col < 0) c->goal_col = view_column_of(buf, p);
    i64 target = buffer_line_of(buf, p) + dir;
    if (target < 0) {
        cmd_goto(ctx, 0);
        echo_message(ctx->echo, "Beginning of buffer");
    } else if (target >= buffer_line_count(buf)) {
        cmd_goto(ctx, buffer_size(buf));
        echo_message(ctx->echo, "End of buffer");
    } else {
        cmd_goto(ctx, view_offset_at_column(buf, target, c->goal_col));
    }
}

static void cmd_next_line(CommandContext *ctx) { cmd_vertical(ctx, 1); }
static void cmd_previous_line(CommandContext *ctx) { cmd_vertical(ctx, -1); }

static void cmd_move_beginning_of_line(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    cmd_goto(ctx, buffer_line_start(buf, buffer_line_of(buf, cmd_point(ctx))));
}

static void cmd_move_end_of_line(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    cmd_goto(ctx, buffer_line_end(buf, buffer_line_of(buf, cmd_point(ctx))));
}

// Letters and digits; every byte >= 0x80 counts as a letter, so the bytes of a multi-byte
// character are all word bytes and a bytewise scan always stops on a character boundary.
static b32 view_is_word_byte(u8 b) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b >= 0x80;
}

static void cmd_forward_word(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    i64 size = buffer_size(buf), p = cmd_point(ctx);
    while (p < size && !view_is_word_byte(buffer_byte(buf, p))) p++;
    while (p < size && view_is_word_byte(buffer_byte(buf, p))) p++;
    cmd_goto(ctx, p);
}

static void cmd_backward_word(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    i64 p = cmd_point(ctx);
    while (p > 0 && !view_is_word_byte(buffer_byte(buf, p - 1))) p--;
    while (p > 0 && view_is_word_byte(buffer_byte(buf, p - 1))) p--;
    cmd_goto(ctx, p);
}

// A paragraph separator is a line of only spaces and tabs (Emacs' default paragraph-start).
static b32 view_line_blank(Buffer *buf, i64 line) {
    i64 end = buffer_line_end(buf, line);
    for (i64 p = buffer_line_start(buf, line); p < end; p++) {
        u8 b = buffer_byte(buf, p);
        if (b != ' ' && b != '\t') return 0;
    }
    return 1;
}

// To the start of the separator after the paragraph (skipping separators first), or the end.
static void cmd_forward_paragraph(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    i64 n = buffer_line_count(buf);
    i64 line = buffer_line_of(buf, cmd_point(ctx));
    while (line < n && view_line_blank(buf, line)) line++;
    while (line < n && !view_line_blank(buf, line)) line++;
    cmd_goto(ctx, line < n ? buffer_line_start(buf, line) : buffer_size(buf));
}

// To the start of the separator before the paragraph, or the beginning. As in Emacs, at the
// start of a paragraph line right after an empty line, that empty line is the destination.
static void cmd_backward_paragraph(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    i64 p = cmd_point(ctx);
    i64 line = buffer_line_of(buf, p);
    if (p == buffer_line_start(buf, line) && line > 0 && !view_line_blank(buf, line) &&
        buffer_line_start(buf, line - 1) == buffer_line_end(buf, line - 1)) {
        cmd_goto(ctx, buffer_line_start(buf, line - 1));
        return;
    }
    if (p == buffer_line_start(buf, line)) line--;
    while (line >= 0 && view_line_blank(buf, line)) line--;
    while (line >= 0 && !view_line_blank(buf, line)) line--;
    cmd_goto(ctx, line >= 0 ? buffer_line_start(buf, line) : 0);
}

static void cmd_beginning_of_buffer(CommandContext *ctx) {
    cmd_goto(ctx, 0);
}

static void cmd_end_of_buffer(CommandContext *ctx) {
    cmd_goto(ctx, buffer_size(ctx->view->buffer));
    // Emacs' (recenter -3) when the end is not visible, honored by view_ensure_visible.
    if (ctx->cursor == &ctx->view->cursors[0]) ctx->view->recenter_row = MAX(ctx->view->rows - 3, 0);
}

// ---------------------------------------------------------------------------
// Commands on the whole View (COMMAND_ONCE)

static void cmd_scroll_up_command(CommandContext *ctx) {
    View *v = ctx->view;
    if (view_top_line(v) >= buffer_line_count(v->buffer) - 1) {
        echo_message(ctx->echo, "End of buffer");
        return;
    }
    view_scroll_lines(v, MAX(v->rows - VIEW_CONTEXT_LINES, 1));
}

static void cmd_scroll_down_command(CommandContext *ctx) {
    View *v = ctx->view;
    if (view_top_line(v) <= 0) {
        echo_message(ctx->echo, "Beginning of buffer");
        return;
    }
    view_scroll_lines(v, -MAX(v->rows - VIEW_CONTEXT_LINES, 1));
}

// Point's line at the center, then the top, then the bottom on consecutive calls.
static void cmd_recenter_top_bottom(CommandContext *ctx) {
    View *v = ctx->view;
    v->recenter_step = ctx->last_command == &CMD_RECENTER_TOP_BOTTOM ? (v->recenter_step + 1) % 3 : 0;
    i64 rows = MAX(v->rows, 1);
    i64 row = v->recenter_step == 0 ? rows / 2 : v->recenter_step == 1 ? 0 : rows - 1;
    i64 line = buffer_line_of(v->buffer, view_point(v, ctx->cursor));
    view_set_top_line(v, MAX(line - row, 0));
}

const Command CMD_FORWARD_CHAR           = { "forward-char", cmd_forward_char, 0 };
const Command CMD_BACKWARD_CHAR          = { "backward-char", cmd_backward_char, 0 };
const Command CMD_NEXT_LINE              = { "next-line", cmd_next_line, 0 };
const Command CMD_PREVIOUS_LINE          = { "previous-line", cmd_previous_line, 0 };
const Command CMD_MOVE_BEGINNING_OF_LINE = { "move-beginning-of-line", cmd_move_beginning_of_line, 0 };
const Command CMD_MOVE_END_OF_LINE       = { "move-end-of-line", cmd_move_end_of_line, 0 };
const Command CMD_FORWARD_WORD           = { "forward-word", cmd_forward_word, 0 };
const Command CMD_BACKWARD_WORD          = { "backward-word", cmd_backward_word, 0 };
const Command CMD_FORWARD_PARAGRAPH      = { "forward-paragraph", cmd_forward_paragraph, 0 };
const Command CMD_BACKWARD_PARAGRAPH     = { "backward-paragraph", cmd_backward_paragraph, 0 };
const Command CMD_BEGINNING_OF_BUFFER    = { "beginning-of-buffer", cmd_beginning_of_buffer, 0 };
const Command CMD_END_OF_BUFFER          = { "end-of-buffer", cmd_end_of_buffer, 0 };
const Command CMD_SCROLL_UP_COMMAND      = { "scroll-up-command", cmd_scroll_up_command, COMMAND_ONCE };
const Command CMD_SCROLL_DOWN_COMMAND    = { "scroll-down-command", cmd_scroll_down_command, COMMAND_ONCE };
const Command CMD_RECENTER_TOP_BOTTOM    = { "recenter-top-bottom", cmd_recenter_top_bottom, COMMAND_ONCE };
