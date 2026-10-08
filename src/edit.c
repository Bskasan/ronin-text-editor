// edit.c — see edit.h.

// ---------------------------------------------------------------------------
// Undo

// Puts the primary point where the undone change was (the point before that command).
static void edit_undo_report(CommandContext *ctx, BufferUndoResult r, i64 point, const char *done, const char *nothing) {
    View *v = ctx->view;
    switch (r) {
    case BUFFER_UNDO_DONE:
        view_set_point(v, &v->cursors[0], point);
        echo_message(ctx->echo, "%s", done);
        break;
    case BUFFER_UNDO_NOTHING:
        echo_message(ctx->echo, "%s", nothing);
        break;
    case BUFFER_UNDO_FAILED:
        if (v->buffer->read_only) echo_message(ctx->echo, "Buffer is read-only: %S", v->buffer->name);
        else echo_message(ctx->echo, "Cannot undo: the buffer is full");
        break;
    }
}

// Emacs: consecutive undos walk back through history; after any other command (undo-redo
// included) undo first undoes the undos.
static void cmd_undo(CommandContext *ctx) {
    View *v = ctx->view;
    i64 point;
    BufferUndoResult r = buffer_undo(v->buffer, ctx->last_command == &CMD_UNDO, view_point(v, &v->cursors[0]), &point);
    edit_undo_report(ctx, r, point, "Undo", "No further undo information");
}

static void cmd_undo_redo(CommandContext *ctx) {
    View *v = ctx->view;
    i64 point;
    BufferUndoResult r = buffer_redo(v->buffer, view_point(v, &v->cursors[0]), &point);
    edit_undo_report(ctx, r, point, "Redo", "No further redo information");
}

const Command CMD_UNDO      = { "undo", cmd_undo, COMMAND_ONCE | COMMAND_EDIT };
const Command CMD_UNDO_REDO = { "undo-redo", cmd_undo_redo, COMMAND_ONCE | COMMAND_EDIT };

// ---------------------------------------------------------------------------
// Kill ring storage

b32 kill_init(KillRing *k, i32 max) {
    memset(k, 0, sizeof(*k));
    k->max = CLAMP(max, 1, KILL_RING_CAP);
    k->small = (u8 *)os_reserve(KILL_SMALL_RESERVE);
    k->scratch = arena_create(KILL_SCRATCH_RESERVE);
    k->clip_seq = os_clipboard_seq();
    return k->small != NULL;
}

static KillEntry *kill_at(KillRing *k, i32 back) {
    return &k->entries[(k->head - back + KILL_RING_CAP) % KILL_RING_CAP];
}

static void kill_free(KillRing *k, KillEntry *e) {
    if (e->large) os_release(e->data);
    else k->small_dead += (u64)e->len;
    memset(e, 0, sizeof(*e));
}

void kill_destroy(KillRing *k) {
    for (i32 i = 0; i < k->count; i++) kill_free(k, kill_at(k, i));
    if (k->small) os_release(k->small);
    if (k->scratch.base) os_release(k->scratch.base);
    k->count = 0;
}

void kill_set_max(KillRing *k, i32 max) {
    k->max = CLAMP(max, 1, KILL_RING_CAP);
    while (k->count > k->max) kill_free(k, kill_at(k, --k->count));
}

String8 kill_entry(KillRing *k, i32 back) {
    if (back < 0 || back >= k->count) return str8(NULL, 0);
    KillEntry *e = kill_at(k, back);
    return str8(e->data, e->len);
}

// Live small entries packed to the front of the arena, in address order.
static void kill_compact(KillRing *k) {
    KillEntry *live[KILL_RING_CAP];
    i32 n = 0;
    for (i32 i = 0; i < k->count; i++) {
        KillEntry *e = kill_at(k, i);
        if (e->large) continue;
        i32 j = n++;
        while (j > 0 && live[j - 1]->data > e->data) { live[j] = live[j - 1]; j--; }
        live[j] = e;
    }
    u64 used = 0;
    for (i32 i = 0; i < n; i++) {
        memmove(k->small + used, live[i]->data, (size_t)live[i]->len);
        live[i]->data = k->small + used;
        used += (u64)live[i]->len;
    }
    k->small_used = used;
    k->small_dead = 0;
}

// `len` more bytes at the end of the shared arena (entries do not move).
static u8 *kill_small_raw(KillRing *k, i64 len) {
    u64 need = k->small_used + (u64)len;
    if (need > KILL_SMALL_RESERVE) return NULL;
    if (need > k->small_committed) {
        u64 to = ALIGN_UP_POW2(need, KB(64));
        if (!os_commit(k->small + k->small_committed, to - k->small_committed)) return NULL;
        k->small_committed = to;
    }
    u8 *p = k->small + k->small_used;
    k->small_used = need;
    return p;
}

// The same after compacting when dead bytes pass half of the arena in use (entries may move).
static u8 *kill_small_alloc(KillRing *k, i64 len) {
    if (k->small_dead > k->small_used / 2) kill_compact(k);
    return kill_small_raw(k, len);
}

// A large entry's commit grows geometrically (at least 1 MB at a time).
static b32 kill_large_reserve(KillEntry *e, u64 need) {
    if (need > KILL_LARGE_RESERVE) return 0;
    if (!e->data) {
        e->data = (u8 *)os_reserve(KILL_LARGE_RESERVE);
        if (!e->data) return 0;
        e->large = 1;
        e->committed = 0;
    }
    if (need <= e->committed) return 1;
    u64 to = MIN(MAX(ALIGN_UP_POW2(need, MB(1)), e->committed * 2), (u64)KILL_LARGE_RESERVE);
    if (!os_commit(e->data + e->committed, to - e->committed)) return 0;
    e->committed = to;
    return 1;
}

u8 *kill_push(KillRing *k, i64 len) {
    if (k->count == k->max) kill_free(k, kill_at(k, --k->count));
    KillEntry e = { 0 };
    if (len > (i64)KILL_SMALL_MAX) {
        if (!kill_large_reserve(&e, (u64)len)) {
            if (e.data) os_release(e.data);
            return NULL;
        }
    } else {
        e.data = kill_small_alloc(k, len);
        if (!e.data) return NULL;
    }
    e.len = len;
    k->head = (k->head + 1) % KILL_RING_CAP;
    k->count++;
    *kill_at(k, 0) = e;
    k->yank_index = 0;
    k->clip_dirty = 1;
    return e.data;
}

u8 *kill_extend(KillRing *k, i64 len, b32 prepend) {
    if (k->count == 0) return kill_push(k, len);
    KillEntry *e = kill_at(k, 0);
    i64 total = e->len + len;
    if (!e->large && total > (i64)KILL_SMALL_MAX) {
        // Grows past the shared arena's limit: to a reservation of its own, once.
        KillEntry big = { 0 };
        if (!kill_large_reserve(&big, (u64)total)) {
            if (big.data) os_release(big.data);
            return NULL;
        }
        memcpy(big.data + (prepend ? len : 0), e->data, (size_t)e->len);
        k->small_dead += (u64)e->len;
        big.len = total;
        *e = big;
    } else if (e->large) {
        if (!kill_large_reserve(e, (u64)total)) return NULL;
        if (prepend) memmove(e->data + len, e->data, (size_t)e->len);
        e->len = total;
    } else {
        if (k->small_dead > k->small_used / 2) kill_compact(k);
        if (e->data + e->len != k->small + k->small_used) {
            // Not the last allocation: move it to the end (at most KILL_SMALL_MAX bytes).
            u8 *dst = kill_small_raw(k, e->len);
            if (!dst) return NULL;
            memcpy(dst, e->data, (size_t)e->len);
            k->small_dead += (u64)e->len;
            e->data = dst;
        }
        if (!kill_small_raw(k, len)) return NULL; // right after the entry
        if (prepend) memmove(e->data + len, e->data, (size_t)e->len);
        e->len = total;
    }
    k->yank_index = 0;
    k->clip_dirty = 1;
    return prepend ? e->data : e->data + e->len - len;
}

void kill_to_clipboard(KillRing *k) {
    if (!k->clip_dirty) return;
    k->clip_dirty = 0;
    os_clipboard_set(kill_entry(k, 0));
    k->clip_seq = os_clipboard_seq();
}

void kill_from_clipboard(KillRing *k) {
    u32 seq = os_clipboard_seq();
    if (seq == k->clip_seq) return;
    k->clip_seq = seq;
    String8 text;
    if (os_clipboard_get(&k->scratch, &text) && text.len && !str8_equal(text, kill_entry(k, 0))) {
        u8 *dst = kill_push(k, text.len);
        if (dst) memcpy(dst, text.data, (size_t)text.len);
        k->clip_dirty = 0; // it came from the clipboard
    }
    arena_reset(&k->scratch);
}

// ---------------------------------------------------------------------------
// Kill commands. The driver sets ctx->kill_append when the previous command killed too; the
// first cursor's kill then appends (or prepends for backward kills), later cursors of the same
// command append to the same entry.

static void edit_kill(CommandContext *ctx, i64 start, i64 end, b32 remove) {
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    if (end <= start) return;
    b32 prepend = (ctx->this_command->flags & COMMAND_KILL_BACKWARD) != 0;
    u8 *dst = ctx->kill_append ? kill_extend(ctx->kills, end - start, prepend) : kill_push(ctx->kills, end - start);
    if (!dst) {
        echo_message(ctx->echo, "Kill ring: out of memory");
        return;
    }
    ctx->kill_append = 1;
    buffer_copy(buf, start, end, dst);
    if (!remove) return;
    if (buf->read_only && !buf->inhibit_read_only) {
        echo_message(ctx->echo, "Buffer is read-only: %S", buf->name); // copied, not removed
        return;
    }
    if (!buffer_replace(buf, start, end, STR8_LIT(""))) echo_message(ctx->echo, "Buffer is full: %S", buf->name);
}

static b32 edit_region(CommandContext *ctx, i64 *start, i64 *end) {
    if (view_region(ctx->view, ctx->cursor, start, end)) return 1;
    echo_message(ctx->echo, "The mark is not set now, so there is no region");
    return 0;
}

static void cmd_kill_region(CommandContext *ctx) {
    i64 start, end;
    if (edit_region(ctx, &start, &end)) edit_kill(ctx, start, end, 1);
}

static void cmd_kill_ring_save(CommandContext *ctx) {
    i64 start, end;
    if (edit_region(ctx, &start, &end)) edit_kill(ctx, start, end, 0);
}

// The rest of the line; through the newline when only blanks are left (Emacs).
static void cmd_kill_line(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    i64 p = view_point(ctx->view, ctx->cursor);
    if (p >= buffer_size(buf)) {
        echo_message(ctx->echo, "End of buffer");
        return;
    }
    i64 end = buffer_line_end(buf, buffer_line_of(buf, p));
    b32 blank = 1;
    for (i64 i = p; i < end && blank; i++) blank = buffer_byte(buf, i) == ' ' || buffer_byte(buf, i) == '\t';
    if (blank && end < buffer_size(buf)) end++;
    edit_kill(ctx, p, end, 1);
}

static void cmd_kill_word(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    i64 p = view_point(ctx->view, ctx->cursor);
    edit_kill(ctx, p, view_forward_word(buf, p, ctx->settings->underscore_is_word), 1);
}

static void cmd_backward_kill_word(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    i64 p = view_point(ctx->view, ctx->cursor);
    edit_kill(ctx, view_backward_word(buf, p, ctx->settings->underscore_is_word), p, 1);
}

// The line with its newline; the last line takes the newline before it instead.
static void cmd_kill_whole_line(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    i64 line = buffer_line_of(buf, view_point(ctx->view, ctx->cursor));
    i64 start = buffer_line_start(buf, line), end = buffer_line_end(buf, line);
    if (end < buffer_size(buf)) end++;
    else if (start > 0) start--;
    edit_kill(ctx, start, end, 1);
}

static void edit_insert_yank(CommandContext *ctx, String8 text) {
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    if (buf->read_only && !buf->inhibit_read_only) {
        echo_message(ctx->echo, "Buffer is read-only: %S", buf->name);
        return;
    }
    i64 p = view_point(v, ctx->cursor);
    view_set_mark(v, ctx->cursor, p, 0);
    if (!buffer_replace(buf, p, p, text)) echo_message(ctx->echo, "Buffer is full: %S", buf->name);
}

// The newest kill (or what another program put on the clipboard since), mark at its start.
static void cmd_yank(CommandContext *ctx) {
    KillRing *k = ctx->kills;
    if (ctx->cursor == &ctx->view->cursors[0]) kill_from_clipboard(k);
    if (!k->count) {
        echo_message(ctx->echo, "Kill ring is empty");
        return;
    }
    k->yank_index = 0;
    edit_insert_yank(ctx, kill_entry(k, 0));
}

// Right after a yank: replaces the yanked text by the next older kill.
static void cmd_yank_pop(CommandContext *ctx) {
    View *v = ctx->view;
    KillRing *k = ctx->kills;
    if (ctx->last_command != &CMD_YANK && ctx->last_command != &CMD_YANK_POP) {
        echo_message(ctx->echo, "Previous command was not a yank");
        return;
    }
    if (!k->count) return;
    if (ctx->cursor == &v->cursors[0]) k->yank_index = (k->yank_index + 1) % k->count;
    i64 start, end;
    if (!view_region(v, ctx->cursor, &start, &end)) return;
    if (!buffer_replace(v->buffer, start, end, kill_entry(k, k->yank_index))) return;
    view_set_point(v, ctx->cursor, start + kill_entry(k, k->yank_index).len);
    view_set_mark(v, ctx->cursor, start, 0);
}

#define KILL (COMMAND_KILL | COMMAND_EDIT)
const Command CMD_KILL_REGION        = { "kill-region", cmd_kill_region, KILL };
const Command CMD_KILL_RING_SAVE     = { "kill-ring-save", cmd_kill_ring_save, COMMAND_KILL };
const Command CMD_KILL_LINE          = { "kill-line", cmd_kill_line, KILL };
const Command CMD_KILL_WORD          = { "kill-word", cmd_kill_word, KILL };
const Command CMD_BACKWARD_KILL_WORD = { "backward-kill-word", cmd_backward_kill_word, KILL | COMMAND_KILL_BACKWARD };
const Command CMD_KILL_WHOLE_LINE    = { "kill-whole-line", cmd_kill_whole_line, KILL };
const Command CMD_YANK               = { "yank", cmd_yank, COMMAND_EDIT | COMMAND_REGION_REPLACE };
const Command CMD_YANK_POP           = { "yank-pop", cmd_yank_pop, COMMAND_EDIT };
#undef KILL

// ---------------------------------------------------------------------------
// Indentation

static b32 edit_is_blank(u8 b) {
    return b == ' ' || b == '\t';
}

static b32 edit_line_blank(Buffer *buf, i64 line) {
    i64 end = buffer_line_end(buf, line);
    for (i64 p = buffer_line_start(buf, line); p < end; p++) if (!edit_is_blank(buffer_byte(buf, p))) return 0;
    return 1;
}

// The end of the line's leading blanks.
static i64 edit_indent_end(Buffer *buf, i64 line) {
    i64 p = buffer_line_start(buf, line), end = buffer_line_end(buf, line);
    while (p < end && edit_is_blank(buffer_byte(buf, p))) p++;
    return p;
}

i64 edit_indent_cols(Buffer *buf, i64 line) {
    i64 col = 0;
    for (i64 p = buffer_line_start(buf, line), end = edit_indent_end(buf, line); p < end; p++) {
        col += view_char_width(buffer_byte(buf, p), col, buf->tab_width);
    }
    return col;
}

static b32 edit_is_closer(u8 b) {
    return b == ')' || b == ']' || b == '}';
}

// After the line's leading blanks: the run of closing brackets (blanks between them allowed).
// Returns their count; *after gets the position after the run.
static i64 edit_leading_closers(Buffer *buf, i64 line, i64 *after) {
    i64 p = edit_indent_end(buf, line), end = buffer_line_end(buf, line), n = 0, last = p;
    while (p < end) {
        u8 b = buffer_byte(buf, p);
        if (edit_is_closer(b)) {
            n++;
            last = ++p;
        } else if (edit_is_blank(b)) {
            p++;
        } else {
            break;
        }
    }
    *after = last;
    return n;
}

i64 edit_compute_indent(Buffer *buf, i64 line, i64 indent_width) {
    i64 after;
    i64 closers = edit_leading_closers(buf, line, &after);
    i64 prev = line - 1;
    while (prev >= 0 && edit_line_blank(buf, prev)) prev--;
    i64 base = 0, balance = 0;
    if (prev >= 0) {
        base = edit_indent_cols(buf, prev);
        edit_leading_closers(buf, prev, &after); // already applied to its own indentation
        for (i64 p = after, end = buffer_line_end(buf, prev); p < end; p++) {
            u8 b = buffer_byte(buf, p);
            if (b == '(' || b == '[' || b == '{') balance++;
            else if (edit_is_closer(b)) balance--;
        }
    }
    return CLAMP(base + (balance - closers) * indent_width, 0, (i64)EDIT_INDENT_MAX);
}

b32 edit_set_indent(Buffer *buf, i64 line, i64 cols) {
    u8 blanks[EDIT_INDENT_MAX];
    cols = CLAMP(cols, 0, (i64)EDIT_INDENT_MAX);
    i64 n = 0, col = 0, tw = MAX(buf->tab_width, 1);
    if (buf->indent_tabs) {
        while ((col / tw + 1) * tw <= cols) {
            blanks[n++] = '\t';
            col = (col / tw + 1) * tw;
        }
    }
    while (col < cols) {
        blanks[n++] = ' ';
        col++;
    }
    i64 start = buffer_line_start(buf, line), end = edit_indent_end(buf, line);
    if (end - start == n) {
        b32 same = 1;
        for (i64 i = 0; i < n && same; i++) same = buffer_byte(buf, start + i) == blanks[i];
        if (same) return 1; // nothing to change: no edit, no undo record
    }
    return buffer_replace(buf, start, end, str8(blanks, n));
}

i32 edit_detect_tabs(Buffer *buf) {
    i64 tabs = 0, spaces = 0, lines = MIN(buffer_line_count(buf), (i64)1000);
    for (i64 line = 0; line < lines && tabs + spaces < 100; line++) {
        i64 p = buffer_line_start(buf, line), end = buffer_line_end(buf, line);
        if (p >= end) continue;
        u8 b = buffer_byte(buf, p);
        if (b == '\t') tabs++;
        else if (b == ' ' && p + 1 < end && buffer_byte(buf, p + 1) == ' ') spaces++; // a lone space is often " *" in a comment
    }
    return tabs > spaces ? 1 : spaces > tabs ? 0 : -1;
}

static b32 edit_writable(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    if (buf->read_only && !buf->inhibit_read_only) {
        echo_message(ctx->echo, "Buffer is read-only: %S", buf->name);
        return 0;
    }
    return 1;
}

// Reindents a line by the rule; a blank line becomes empty (keep_blank: it is indented too).
static void edit_reindent_line(CommandContext *ctx, i64 line, b32 keep_blank) {
    Buffer *buf = ctx->view->buffer;
    if (!keep_blank && edit_line_blank(buf, line)) {
        buffer_replace(buf, buffer_line_start(buf, line), buffer_line_end(buf, line), STR8_LIT(""));
        return;
    }
    edit_set_indent(buf, line, edit_compute_indent(buf, line, ctx->settings->indent_width));
}

void edit_electric_close(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    i64 p = view_point(ctx->view, ctx->cursor);
    i64 line = buffer_line_of(buf, p);
    if (edit_indent_end(buf, line) == p - 1) edit_reindent_line(ctx, line, 0);
}

// The newline, then the new line indented by the rule. A line left with only blanks is emptied.
static void cmd_newline(CommandContext *ctx) {
    if (!edit_writable(ctx)) return;
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    i64 p = view_point(v, ctx->cursor);
    if (!buffer_replace(buf, p, p, STR8_LIT("\n"))) {
        echo_message(ctx->echo, "Buffer is full: %S", buf->name);
        return;
    }
    i64 line = buffer_line_of(buf, view_point(v, ctx->cursor));
    if (edit_line_blank(buf, line - 1)) {
        buffer_replace(buf, buffer_line_start(buf, line - 1), buffer_line_end(buf, line - 1), STR8_LIT(""));
    }
    edit_reindent_line(ctx, line, 1);
}

// The lines a region command acts on: from the region's first line to its last (a region ending
// at the start of a line leaves that line out).
static b32 edit_region_lines(CommandContext *ctx, i64 *first, i64 *last) {
    View *v = ctx->view;
    i64 start, end;
    if (!view_region_active(v, ctx->cursor, ctx->settings) || !view_region(v, ctx->cursor, &start, &end)) return 0;
    Buffer *buf = v->buffer;
    *first = buffer_line_of(buf, start);
    *last = buffer_line_of(buf, end);
    if (*last > *first && end == buffer_line_start(buf, *last)) (*last)--;
    return 1;
}

// TAB: the line (or every line of an active region) reindented by the rule. Point keeps its place
// in the text; from inside the indentation it goes to the first non-blank character.
static void cmd_indent_for_tab_command(CommandContext *ctx) {
    if (!edit_writable(ctx)) return;
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    i64 first, last;
    if (edit_region_lines(ctx, &first, &last)) {
        for (i64 line = first; line <= last; line++) edit_reindent_line(ctx, line, 0);
        return;
    }
    i64 line = buffer_line_of(buf, view_point(v, ctx->cursor));
    b32 in_indent = view_point(v, ctx->cursor) <= edit_indent_end(buf, line);
    edit_reindent_line(ctx, line, 1);
    if (in_indent) view_set_point(v, ctx->cursor, edit_indent_end(buf, line));
}

// <backtab>: one level less (to the previous multiple of indent_width), for the line or region.
static void cmd_unindent(CommandContext *ctx) {
    if (!edit_writable(ctx)) return;
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    i64 first, last, w = ctx->settings->indent_width;
    if (!edit_region_lines(ctx, &first, &last)) first = last = buffer_line_of(buf, view_point(v, ctx->cursor));
    for (i64 line = first; line <= last; line++) {
        if (edit_line_blank(buf, line) && first != last) continue;
        i64 cols = edit_indent_cols(buf, line);
        edit_set_indent(buf, line, cols > 0 ? (cols - 1) / w * w : 0);
    }
}

// M-i: blanks from point to the next multiple of indent_width.
static void cmd_tab_to_tab_stop(CommandContext *ctx) {
    if (!edit_writable(ctx)) return;
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    i64 p = view_point(v, ctx->cursor), w = ctx->settings->indent_width, tw = MAX(buf->tab_width, 1);
    i64 col = view_column_of(buf, p), target = (col / w + 1) * w;
    u8 blanks[EDIT_INDENT_MAX];
    i64 n = 0;
    if (buf->indent_tabs) {
        while ((col / tw + 1) * tw <= target && n < EDIT_INDENT_MAX) {
            blanks[n++] = '\t';
            col = (col / tw + 1) * tw;
        }
    }
    while (col < target && n < EDIT_INDENT_MAX) {
        blanks[n++] = ' ';
        col++;
    }
    buffer_replace(buf, p, p, str8(blanks, n));
}

const Command CMD_NEWLINE                = { "newline", cmd_newline, COMMAND_EDIT | COMMAND_REGION_REPLACE };
const Command CMD_INDENT_FOR_TAB_COMMAND = { "indent-for-tab-command", cmd_indent_for_tab_command, COMMAND_EDIT };
const Command CMD_UNINDENT               = { "unindent", cmd_unindent, COMMAND_EDIT };
const Command CMD_TAB_TO_TAB_STOP        = { "tab-to-tab-stop", cmd_tab_to_tab_stop, COMMAND_EDIT };
