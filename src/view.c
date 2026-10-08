// view.c — see view.h.

// ---------------------------------------------------------------------------
// Visual columns

b32 view_is_control(u8 b) {
    return (b < 0x20 && b != '\t' && b != '\n') || b == 0x7F;
}

i64 view_char_width(u8 first_byte, i64 col, i64 tab_width) {
    if (first_byte == '\t') return tab_width - col % tab_width;
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
            i64 w = view_char_width(b, col, buf->tab_width);
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
    i64 w = view_char_width(buffer_byte(buf, pos), c, buf->tab_width);
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
    echo_log(e, str8(e->text, e->len));
}

void echo_log(Echo *e, String8 text) {
    Buffer *b = e->log;
    if (!b) return;
    u8 line[ECHO_CAP + 1];
    i64 n = MIN(text.len, (i64)ECHO_CAP);
    memcpy(line, text.data, (size_t)n);
    line[n++] = '\n';
    b32 inhibit = b->inhibit_read_only;
    b->inhibit_read_only = 1;
    buffer_replace(b, buffer_size(b), buffer_size(b), str8(line, n));
    i64 lines = buffer_line_count(b) - 1; // every message ends with a newline
    if (lines > ECHO_LOG_LINES) buffer_replace(b, 0, buffer_line_start(b, lines - ECHO_LOG_LINES), STR8_LIT(""));
    b->inhibit_read_only = inhibit;
    b->modified = 0;
}

void echo_set(Echo *e, String8 text) {
    e->len = (i32)MIN(text.len, (i64)ECHO_CAP);
    memcpy(e->text, text.data, (size_t)e->len);
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

// ---------------------------------------------------------------------------
// The buffer list

void buffer_list_init(BufferList *list) {
    memset(list, 0, sizeof(*list));
    list->arena = arena_create(BUFFER_LIST_RESERVE);
    list->entries = (BufferEntry *)list->arena.base;
}

i32 buffer_list_destroy(BufferList *list) {
    i32 leaks = 0;
    for (i32 i = 0; i < list->count; i++) {
        BufferEntry *e = &list->entries[i];
        buffer_marker_destroy(e->buffer, e->point);
        buffer_marker_destroy(e->buffer, e->top);
        leaks += (i32)e->buffer->marker_live;
        if (!buffer_destroy(e->buffer)) leaks++;
    }
    os_release(list->arena.base);
    list->count = 0;
    return leaks;
}

BufferEntry *buffer_list_add(BufferList *list, Buffer *buf) {
    BufferEntry *e = PUSH_STRUCT(&list->arena, BufferEntry);
    ASSERT(e == list->entries + list->count);
    e->buffer = buf;
    e->point = buffer_marker_create(buf, 0, 1);
    e->top = buffer_marker_create(buf, 0, 0);
    list->count++;
    return e;
}

i32 buffer_list_index(BufferList *list, Buffer *buf) {
    for (i32 i = 0; i < list->count; i++) if (list->entries[i].buffer == buf) return i;
    return -1;
}

static u8 view_ascii_lower(u8 c) {
    return c >= 'A' && c <= 'Z' ? (u8)(c + 32) : c;
}

Buffer *buffer_list_find_path(BufferList *list, String8 full_path) {
    for (i32 i = 0; i < list->count; i++) {
        String8 p = list->entries[i].buffer->path;
        if (!p.len || p.len != full_path.len) continue;
        i64 k = 0;
        while (k < p.len && view_ascii_lower(p.data[k]) == view_ascii_lower(full_path.data[k])) k++;
        if (k == p.len) return list->entries[i].buffer;
    }
    return NULL;
}

void view_switch_buffer(View *v, BufferList *list, Buffer *buf) {
    if (v->buffer == buf) return;
    i32 old = buffer_list_index(list, v->buffer);
    if (old >= 0) {
        BufferEntry *e = &list->entries[old];
        buffer_marker_set(v->buffer, e->point, view_point(v, &v->cursors[0]));
        buffer_marker_set(v->buffer, e->top, buffer_marker_get(v->buffer, v->top));
        e->left_col = v->left_col;
    }
    for (i32 i = 0; i < v->cursor_count; i++) {
        buffer_marker_destroy(v->buffer, v->cursors[i].point);
        buffer_marker_destroy(v->buffer, v->cursors[i].mark);
    }
    buffer_marker_destroy(v->buffer, v->top);
    arena_reset(&v->cursor_arena);
    v->cursor_count = 0;

    i32 now = buffer_list_index(list, buf);
    BufferEntry *e = now >= 0 ? &list->entries[now] : NULL;
    v->buffer = buf;
    v->top = buffer_marker_create(buf, e ? buffer_marker_get(buf, e->top) : 0, 0);
    view_add_cursor(v, e ? buffer_marker_get(buf, e->point) : 0);
    v->left_col = e ? e->left_col : 0;
    v->recenter_step = 0;
    v->recenter_row = -1;
}

void view_set_mark(View *v, Cursor *c, i64 pos, b32 active) {
    buffer_marker_set(v->buffer, c->mark, pos);
    c->mark_set = 1;
    c->mark_active = active;
    c->mark_shift = 0;
}

void view_deactivate_mark(View *v) {
    for (i32 i = 0; i < v->cursor_count; i++) v->cursors[i].mark_active = v->cursors[i].mark_shift = 0;
}

b32 view_region(View *v, Cursor *c, i64 *start, i64 *end) {
    if (!c->mark_set) return 0;
    i64 p = view_point(v, c), m = buffer_marker_get(v->buffer, c->mark);
    *start = MIN(p, m);
    *end = MAX(p, m);
    return 1;
}

b32 view_region_active(View *v, Cursor *c, const Settings *settings) {
    (void)v;
    return settings->transient_mark_mode && c->mark_set && c->mark_active;
}

static b32 view_is_word_byte(u8 b, b32 underscore);

void view_word_bounds(Buffer *buf, i64 pos, b32 underscore, i64 *start, i64 *end) {
    i64 size = buffer_size(buf);
    *start = *end = pos;
    if (pos >= size) return;
    u8 b = buffer_byte(buf, pos);
    if (b == '\n') {
        *end = pos + 1;
        return;
    }
    i32 cls = view_is_word_byte(b, underscore) ? 1 : (b == ' ' || b == '\t') ? 2 : 0;
    if (cls == 0) {
        *end = buffer_next_char(buf, pos);
        return;
    }
    i64 s = pos, e = pos;
    while (s > 0) {
        u8 c = buffer_byte(buf, s - 1);
        if ((cls == 1 && !view_is_word_byte(c, underscore)) || (cls == 2 && c != ' ' && c != '\t')) break;
        s--;
    }
    while (e < size) {
        u8 c = buffer_byte(buf, e);
        if ((cls == 1 && !view_is_word_byte(c, underscore)) || (cls == 2 && c != ' ' && c != '\t')) break;
        e++;
    }
    *start = s;
    *end = e;
}

// Deletes the cursor's region (delete-active-region, delete_selection_mode).
static void view_delete_region(CommandContext *ctx, Cursor *c) {
    View *v = ctx->view;
    i64 start, end;
    if (!view_region(v, c, &start, &end)) return;
    if (v->buffer->read_only && !v->buffer->inhibit_read_only) {
        echo_message(ctx->echo, "Buffer is read-only: %S", v->buffer->name);
        return;
    }
    buffer_replace(v->buffer, start, end, STR8_LIT(""));
}

// Shift-select (Emacs' handle-shift-selection): a shift-translated motion activates the mark at
// point first (unless a region is already active, which it then extends); an unshifted motion
// ends a region that shift started.
static void view_shift_select(CommandContext *ctx, Cursor *c) {
    View *v = ctx->view;
    if (ctx->shift_translated) {
        if (!c->mark_active) {
            view_set_mark(v, c, view_point(v, c), 1);
            c->mark_shift = 1;
        }
    } else if (c->mark_active && c->mark_shift) {
        c->mark_active = c->mark_shift = 0;
    }
}

// One cursor's turn of a command, with the region rules its flags ask for.
static void view_run_for_cursor(CommandContext *ctx, const Command *cmd, Cursor *c) {
    ctx->cursor = c;
    if (cmd->flags & COMMAND_MOTION) view_shift_select(ctx, c);
    b32 active = view_region_active(ctx->view, c, ctx->settings);
    if (active && (cmd->flags & COMMAND_REGION_DELETE)) {
        view_delete_region(ctx, c);
        return;
    }
    if (active && (cmd->flags & COMMAND_REGION_REPLACE) && ctx->settings->delete_selection_mode) view_delete_region(ctx, c);
    cmd->fn(ctx);
}

void view_run_command(CommandContext *ctx, const Command *cmd) {
    View *v = ctx->view;
    ctx->this_command = cmd;
    // One command, over all cursors, is one undo group; consecutive self-inserts and single
    // deletes merge (the buffer caps a group at 20 commands).
    BufferUndoMerge merge = (cmd->flags & COMMAND_MERGE_INSERT) ? BUFFER_UNDO_MERGE_INSERT
                          : (cmd->flags & COMMAND_MERGE_DELETE) ? BUFFER_UNDO_MERGE_DELETE : BUFFER_UNDO_MERGE_NONE;
    buffer_undo_boundary(v->buffer, merge, ctx->last_command == cmd, view_point(v, &v->cursors[0]));
    // A kill right after a kill appends to the same kill ring entry.
    ctx->kill_append = (cmd->flags & COMMAND_KILL) && ctx->last_command && (ctx->last_command->flags & COMMAND_KILL);
    if (cmd->flags & COMMAND_ONCE) {
        view_run_for_cursor(ctx, cmd, &v->cursors[0]);
    } else {
        for (i32 i = 0; i < v->cursor_count; i++) view_run_for_cursor(ctx, cmd, &v->cursors[i]);
    }
    ctx->cursor = NULL;
    if (cmd->flags & (COMMAND_EDIT | COMMAND_KILL)) view_deactivate_mark(ctx->view); // the view may have changed buffers
    if (ctx->kills) kill_to_clipboard(ctx->kills); // once per command, when it killed
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

// Letters and digits (and '_' with underscore_is_word); every byte >= 0x80 counts as a letter,
// so the bytes of a multi-byte character are all word bytes and a bytewise scan always stops on
// a character boundary.
static b32 view_is_word_byte(u8 b, b32 underscore) {
    return (b >= 'a' && b <= 'z') || (b >= 'A' && b <= 'Z') || (b >= '0' && b <= '9') || b >= 0x80 || (b == '_' && underscore);
}

b32 view_is_word_char(Buffer *buf, i64 pos, b32 u) {
    return pos < buffer_size(buf) && view_is_word_byte(buffer_byte(buf, pos), u);
}

i64 view_forward_word(Buffer *buf, i64 p, b32 u) {
    i64 size = buffer_size(buf);
    while (p < size && !view_is_word_byte(buffer_byte(buf, p), u)) p++;
    while (p < size && view_is_word_byte(buffer_byte(buf, p), u)) p++;
    return p;
}

i64 view_backward_word(Buffer *buf, i64 p, b32 u) {
    while (p > 0 && !view_is_word_byte(buffer_byte(buf, p - 1), u)) p--;
    while (p > 0 && view_is_word_byte(buffer_byte(buf, p - 1), u)) p--;
    return p;
}

static void cmd_forward_word(CommandContext *ctx) {
    cmd_goto(ctx, view_forward_word(ctx->view->buffer, cmd_point(ctx), ctx->settings->underscore_is_word));
}

static void cmd_backward_word(CommandContext *ctx) {
    cmd_goto(ctx, view_backward_word(ctx->view->buffer, cmd_point(ctx), ctx->settings->underscore_is_word));
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
// Editing commands (one cursor each). Other cursors and the top of the window follow
// through their markers.

static b32 cmd_writable(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    if (buf->read_only && !buf->inhibit_read_only) {
        echo_message(ctx->echo, "Buffer is read-only: %S", buf->name);
        return 0;
    }
    return 1;
}

static void cmd_replace(CommandContext *ctx, i64 start, i64 end, String8 text) {
    Buffer *buf = ctx->view->buffer;
    if (!buffer_replace(buf, start, end, text)) echo_message(ctx->echo, "Buffer is full: %S", buf->name);
}

static void cmd_insert(CommandContext *ctx, String8 text) {
    if (!cmd_writable(ctx)) return;
    i64 p = cmd_point(ctx);
    cmd_replace(ctx, p, p, text); // point advances over it
}

// A closing ) ] } typed as the first non-blank character of a line reindents the line.
static void cmd_self_insert(CommandContext *ctx) {
    if (!ctx->codepoint) return; // bound to a key without a character
    u8 bytes[4];
    i64 n = utf8_encode(ctx->codepoint, bytes);
    cmd_insert(ctx, str8(bytes, n));
    u32 c = ctx->codepoint;
    if (c == ')' || c == ']' || c == '}') edit_electric_close(ctx);
}

static void cmd_delete_backward_char(CommandContext *ctx) {
    if (!cmd_writable(ctx)) return;
    i64 p = cmd_point(ctx);
    if (p <= 0) echo_message(ctx->echo, "Beginning of buffer");
    else cmd_replace(ctx, buffer_prev_char(ctx->view->buffer, p), p, STR8_LIT(""));
}

static void cmd_delete_char(CommandContext *ctx) {
    if (!cmd_writable(ctx)) return;
    Buffer *buf = ctx->view->buffer;
    i64 p = cmd_point(ctx);
    if (p >= buffer_size(buf)) echo_message(ctx->echo, "End of buffer");
    else cmd_replace(ctx, p, buffer_next_char(buf, p), STR8_LIT(""));
}

// ---------------------------------------------------------------------------
// Commands on the whole View (COMMAND_ONCE)

// The keymap also uses it to cancel a pending prefix (KEY_RESULT_QUIT).
// Also deactivates the mark (of every cursor).
static void cmd_keyboard_quit(CommandContext *ctx) {
    view_deactivate_mark(ctx->view);
    echo_message(ctx->echo, "Quit");
}

// ---------------------------------------------------------------------------
// Mark commands

// Repeated at once, it deactivates the mark again (Emacs' C-SPC C-SPC).
static void cmd_set_mark_command(CommandContext *ctx) {
    Cursor *c = ctx->cursor;
    if (ctx->last_command == &CMD_SET_MARK_COMMAND && c->mark_active) {
        c->mark_active = c->mark_shift = 0;
        echo_message(ctx->echo, "Mark deactivated");
        return;
    }
    view_set_mark(ctx->view, c, cmd_point(ctx), 1);
    echo_message(ctx->echo, "Mark set");
}

static void cmd_exchange_point_and_mark(CommandContext *ctx) {
    View *v = ctx->view;
    Cursor *c = ctx->cursor;
    if (!c->mark_set) {
        echo_message(ctx->echo, "No mark set in this buffer");
        return;
    }
    i64 p = cmd_point(ctx), m = buffer_marker_get(v->buffer, c->mark);
    view_set_mark(v, c, p, 1);
    cmd_goto(ctx, m);
}

static void cmd_mark_whole_buffer(CommandContext *ctx) {
    View *v = ctx->view;
    view_set_mark(v, ctx->cursor, buffer_size(v->buffer), 1);
    cmd_goto(ctx, 0);
    echo_message(ctx->echo, "Mark set");
}

static void cmd_save_buffer(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    if (!buf->modified) {
        echo_message(ctx->echo, "(No changes need to be saved)");
        return;
    }
    OsFileStatus status = buffer_save_opt(buf, ctx->settings->fsync_on_save);
    if (status == OS_FILE_OK) echo_message(ctx->echo, "Wrote %S", buf->path);
    else echo_message(ctx->echo, "Cannot save %S: %s", buf->name, buffer_status_text(status));
}

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

#define MOTION COMMAND_MOTION
#define EDIT COMMAND_EDIT
const Command CMD_FORWARD_CHAR           = { "forward-char", cmd_forward_char, MOTION };
const Command CMD_BACKWARD_CHAR          = { "backward-char", cmd_backward_char, MOTION };
const Command CMD_NEXT_LINE              = { "next-line", cmd_next_line, MOTION };
const Command CMD_PREVIOUS_LINE          = { "previous-line", cmd_previous_line, MOTION };
const Command CMD_MOVE_BEGINNING_OF_LINE = { "move-beginning-of-line", cmd_move_beginning_of_line, MOTION };
const Command CMD_MOVE_END_OF_LINE       = { "move-end-of-line", cmd_move_end_of_line, MOTION };
const Command CMD_FORWARD_WORD           = { "forward-word", cmd_forward_word, MOTION };
const Command CMD_BACKWARD_WORD          = { "backward-word", cmd_backward_word, MOTION };
const Command CMD_FORWARD_PARAGRAPH      = { "forward-paragraph", cmd_forward_paragraph, MOTION };
const Command CMD_BACKWARD_PARAGRAPH     = { "backward-paragraph", cmd_backward_paragraph, MOTION };
const Command CMD_BEGINNING_OF_BUFFER    = { "beginning-of-buffer", cmd_beginning_of_buffer, MOTION };
const Command CMD_END_OF_BUFFER          = { "end-of-buffer", cmd_end_of_buffer, MOTION };
const Command CMD_SCROLL_UP_COMMAND      = { "scroll-up-command", cmd_scroll_up_command, COMMAND_ONCE | MOTION };
const Command CMD_SCROLL_DOWN_COMMAND    = { "scroll-down-command", cmd_scroll_down_command, COMMAND_ONCE | MOTION };
const Command CMD_RECENTER_TOP_BOTTOM    = { "recenter-top-bottom", cmd_recenter_top_bottom, COMMAND_ONCE };
const Command CMD_SELF_INSERT            = { "self-insert-command", cmd_self_insert, EDIT | COMMAND_MERGE_INSERT | COMMAND_REGION_REPLACE };
const Command CMD_DELETE_BACKWARD_CHAR   = { "delete-backward-char", cmd_delete_backward_char, EDIT | COMMAND_MERGE_DELETE | COMMAND_REGION_DELETE };
const Command CMD_DELETE_CHAR            = { "delete-char", cmd_delete_char, EDIT | COMMAND_MERGE_DELETE | COMMAND_REGION_DELETE };
const Command CMD_SAVE_BUFFER            = { "save-buffer", cmd_save_buffer, COMMAND_ONCE };
const Command CMD_KEYBOARD_QUIT          = { "keyboard-quit", cmd_keyboard_quit, COMMAND_ONCE };
const Command CMD_SET_MARK_COMMAND       = { "set-mark-command", cmd_set_mark_command, 0 };
const Command CMD_EXCHANGE_POINT_AND_MARK = { "exchange-point-and-mark", cmd_exchange_point_and_mark, 0 };
const Command CMD_MARK_WHOLE_BUFFER      = { "mark-whole-buffer", cmd_mark_whole_buffer, COMMAND_ONCE };
#undef MOTION
#undef EDIT
