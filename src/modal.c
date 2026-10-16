// modal.c — see modal.h. Part of the app (included after app.c): the box's geometry, opening and closing, its keys
// and mouse events, its drawing, and the layer's commands (modal-next ... modal-other-pane).

static b32 modal_inside(Rect r, i32 x, i32 y) {
    return (f32)x >= r.x0 && (f32)x < r.x1 && (f32)y >= r.y0 && (f32)y < r.y1;
}

// The box for the frame last laid out: width_pct of the frame's columns, at most height_pct of the window area's
// rows, centred on the window area (the frame above the echo line); `rows_wanted` list rows if they fit. Borders
// and separators are pixels inside it.
static ModalGeometry modal_geometry(Modal *m, const ModalSpec *s, i64 rows_wanted) {
    ModalGeometry g = { 0 };
    i32 cw = MAX(m->cell_w, 1), lh = MAX(m->line_h, 1), b = MAX(m->divider, 1);
    i32 area_h = MAX(m->frame_h - lh, 0); // the echo line is not covered
    i32 wpct = s->width_pct > 0 ? s->width_pct : MODAL_WIDTH_PCT;
    i32 hpct = s->height_pct > 0 ? s->height_pct : MODAL_HEIGHT_PCT;
    i32 cols = (m->frame_w / cw) * wpct / 100;
    i32 max_rows = (area_h / lh) * hpct / 100;
    g.border = b;
    g.fits = cols >= MODAL_MIN_COLS && max_rows >= MODAL_MIN_ROWS;
    if (!g.fits) return g;
    b32 top_line = s->field || s->status_line;
    i32 field_h = top_line ? lh + b : 0; // the field (or status) line and the separator under it
    i32 list_rows = (max_rows * lh - 2 * b - field_h) / lh;
    list_rows = (i32)MAX(MIN((i64)list_rows, MAX(rows_wanted, 1)), 1);
    i32 w = cols * cw, h = 2 * b + field_h + list_rows * lh;
    i32 x0 = (m->frame_w - w) / 2, y0 = MAX((area_h - h) / 2, 0);
    g.box = (Rect){ (f32)x0, (f32)y0, (f32)(x0 + w), (f32)(y0 + h) };
    i32 list_y = y0 + b + field_h;
    if (top_line) g.field = (Rect){ (f32)(x0 + b), (f32)(y0 + b), (f32)(x0 + w - b), (f32)(y0 + b + lh) };
    g.rows = list_rows;
    i32 inner_x = x0 + b, inner_w = w - 2 * b;
    if (s->panes > 1) {
        i32 split = s->pane_split_pct > 0 ? s->pane_split_pct : MODAL_PANE_SPLIT_PCT;
        i32 left = (inner_w * split / 100) / cw * cw; // whole cells
        left = CLAMP(left, cw, inner_w - b - cw);
        g.panes[0] = (Rect){ (f32)inner_x, (f32)list_y, (f32)(inner_x + left), (f32)(list_y + list_rows * lh) };
        g.panes[1] = (Rect){ (f32)(inner_x + left + b), (f32)list_y, (f32)(inner_x + inner_w), (f32)(list_y + list_rows * lh) };
    } else {
        g.panes[0] = (Rect){ (f32)inner_x, (f32)list_y, (f32)(inner_x + inner_w), (f32)(list_y + list_rows * lh) };
    }
    return g;
}

// The selection kept among the `rows` rows shown; the top never past the last full page.
static void modal_list_show(ModalList *l, i32 rows) {
    rows = MAX(rows, 1);
    if (l->selected >= 0) {
        if (l->selected < l->top) l->top = l->selected;
        if (l->selected >= l->top + rows) l->top = l->selected - rows + 1;
    }
    l->top = CLAMP(l->top, 0, MAX(l->count - rows, 0));
}

// Every frame, before and after its events: the frame's metrics, the box, the field's View in its line. A box
// that no longer fits (a resize) closes, with the message an open would give.
static void modal_layout(App *app, AppLayout *l, FrameInput *in) {
    Modal *m = &app->modal;
    m->frame_w = in->width;
    m->frame_h = in->height;
    m->cell_w = l->cell_w;
    m->line_h = l->line_h;
    m->pad = l->pad;
    m->divider = app_divider_px(in);
    if (!m->active) return;
    m->g = modal_geometry(m, &m->spec, m->rows_high);
    if (!m->g.fits) {
        String8 name = m->spec.name;
        modal_close(app);
        echo_message(&app->echo, "The frame is too small for the %S", name);
        return;
    }
    for (i32 p = 0; p < MAX(m->spec.panes, 1); p++) { // the rows may have changed: the top stays in range (the wheel may
        ModalList *list = &m->lists[p];                // have scrolled the selection out of view: it stays there)
        list->top = CLAMP(list->top, 0, MAX(list->count - m->g.rows, 0));
    }
    if (m->spec.field) {
        View *v = &m->field_view;
        Rect f = m->g.field;
        v->x = (i32)f.x0;
        v->y = (i32)f.y0;
        v->w = MAX((i32)(f.x1 - f.x0) - 12 * l->cell_w, l->pad + l->cell_w); // room for the status ("137/137")
        v->h = l->line_h;
        v->rows = 1;
        v->cols = MAX((v->w - l->pad) / l->cell_w, 1);
        view_ensure_visible(v);
    }
}

b32 modal_active(App *app) {
    return app->modal.active;
}

Arena *modal_arena(App *app) {
    return &app->modal.arena;
}

void modal_set_data(App *app, void *data) {
    app->modal.spec.data = data;
}

ModalList *modal_list(App *app, i32 pane) {
    return &app->modal.lists[CLAMP(pane, 0, MODAL_PANES_MAX - 1)];
}

i32 modal_focus_pane(App *app) {
    return app->modal.focus;
}

Rect modal_rect(App *app) {
    return app->modal.g.box;
}

static b32 modal_scrolls(Modal *m, i32 pane) {
    return (m->spec.scroll_panes >> pane) & 1;
}

void modal_set_rows(App *app, i32 pane, i64 count) {
    Modal *m = &app->modal;
    ModalList *l = modal_list(app, pane);
    *l = (ModalList){ count, count && !modal_scrolls(m, pane) ? 0 : -1, 0 };
    m->rows_high = MAX(m->rows_high, count);
}

void modal_grow_rows(App *app, i32 pane, i64 count) {
    Modal *m = &app->modal;
    ModalList *l = modal_list(app, pane);
    if (count < l->count) return;
    if (l->selected < 0 && count && !modal_scrolls(m, pane)) l->selected = 0;
    l->count = count;
    m->rows_high = MAX(m->rows_high, count);
}

// The owner hears of a selection that moved.
static void modal_selection_moved(App *app, i32 pane, i64 before) {
    Modal *m = &app->modal;
    i64 now = m->lists[pane].selected;
    if (m->active && now != before && now >= 0 && m->spec.selected) m->spec.selected(app, m->spec.data, pane, now);
}

b32 modal_open(App *app, const ModalSpec *spec) {
    Modal *m = &app->modal;
    if (m->active) {
        echo_message(&app->echo, "The %S is already open", m->spec.name);
        return 0;
    }
    if (app->mini.active) {
        echo_message(&app->echo, "The %S is not available in the minibuffer", spec->name);
        return 0;
    }
    if (!modal_geometry(m, spec, 1).fits) {
        echo_message(&app->echo, "The frame is too small for the %S", spec->name);
        return 0;
    }
    if (!m->arena.base) m->arena = arena_create(MODAL_ARENA_RESERVE);
    if (spec->field && !m->field_buffer) {
        m->field_buffer = buffer_create(STR8_LIT(" *Modal*")); // not listed
        if (!m->field_buffer) os_fatal(STR8_LIT("Out of address space (buffer reserve failed)."));
        view_init(&m->field_view, m->field_buffer);
        app_buffer_settings(app, m->field_buffer);
    }
    m->active = 1;
    m->spec = *spec;
    m->spec.panes = CLAMP(spec->panes, 1, MODAL_PANES_MAX);
    m->focus = 0;
    m->rows_high = 0;
    m->then = NULL;
    for (i32 p = 0; p < MODAL_PANES_MAX; p++) m->lists[p] = (ModalList){ 0, -1, 0 };
    if (spec->field) { // empty, no undo history, point and the scroll position at the start
        Buffer *buf = m->field_buffer;
        View *v = &m->field_view;
        buffer_undo_enable(buf, 0);
        buffer_replace(buf, 0, buffer_size(buf), str8(NULL, 0));
        buffer_undo_enable(buf, 1);
        view_collapse_cursors(v);
        Cursor *c = view_primary(v);
        view_set_point(v, c, 0);
        c->mark_set = c->mark_active = c->mark_shift = 0;
        buffer_marker_set(buf, v->top, 0);
        v->left_col = 0;
        m->field_seen = buf->edit_count;
    }
    m->g = modal_geometry(m, &m->spec, 1);
    app->keys.pending.len = 0;
    app->dragging = 0;
    app->resizing = -1;
#if RONIN_DEV
    m->dev_opens++;
#endif
    return 1;
}

void modal_close(App *app) {
    Modal *m = &app->modal;
    if (!m->active) return;
    m->active = 0;
    if (m->spec.closed) m->spec.closed(app, m->spec.data);
    arena_reset(&m->arena);
    m->spec = (ModalSpec){ 0 };
    for (i32 p = 0; p < MODAL_PANES_MAX; p++) m->lists[p] = (ModalList){ 0, -1, 0 };
    m->rows_high = 0;
    m->g = (ModalGeometry){ 0 };
    app->keys.pending.len = 0;
}

void modal_run_after_close(App *app, const Command *cmd) {
    modal_close(app);
    app->modal.then = cmd;
}

// After a command or a click in the box: the owner hears of a changed field; an accepted command runs now, on the
// selected window (the modal is closed by then), as M-x runs it.
static void modal_after(App *app) {
    Modal *m = &app->modal;
    if (m->active && m->spec.field && m->field_buffer->edit_count != m->field_seen) {
        m->field_seen = m->field_buffer->edit_count;
        if (m->spec.field_changed) {
            Arena *scratch = app->ctx.scratch;
            u64 mark = arena_pos(scratch);
#if RONIN_DEV
            u64 t0 = os_time_us();
#endif
            m->spec.field_changed(app, m->spec.data, buffer_text(m->field_buffer, scratch, 0, buffer_size(m->field_buffer)));
#if RONIN_DEV
            m->dev_field_us = os_time_us() - t0;
            m->dev_field_calls++;
#endif
            arena_pop_to(scratch, mark);
        }
        for (i32 p = 0; p < m->spec.panes; p++) modal_list_show(&m->lists[p], m->g.rows);
    }
    const Command *then = m->then;
    if (then) {
        m->then = NULL;
        app_run_command(app, then, 0, 0);
    }
}

// The owner's keymap binds the command (any sequence).
static b32 modal_owns(Keymap *map, const Command *cmd) {
    for (i32 i = 0; map && i < map->count; i++) if (map->bindings[i].command == cmd) return 1;
    return 0;
}

static void modal_key(App *app, Event *e) {
    Modal *m = &app->modal;
    Keymap *own = m->spec.keymap ? m->spec.keymap(app, m->spec.data) : NULL;
    Keymap *stack[2];
    i32 count = 0;
    if (own) stack[count++] = own;
    stack[count++] = &app->config->global;
    KeyResult k;
    key_input_feed(&app->keys, stack, count, e, &k);
#if RONIN_DEV
    if (app->dev_log_keys) app_dev_log_key(app, e, &k, 0, 0);
#endif
    if (k.kind == KEY_RESULT_IGNORED || k.kind == KEY_RESULT_DROPPED) return;
    app->landing_buffer = NULL;
    u8 seq[KEY_SEQ_TEXT_CAP];
    i64 n = key_seq_print(&k.seq, seq, sizeof(seq) - 1);
    switch (k.kind) {
    case KEY_RESULT_PREFIX: // shown at once, as "C-x-"
        seq[n++] = '-';
        echo_set(&app->echo, str8(seq, n));
        return;
    case KEY_RESULT_UNDEFINED:
        echo_message(&app->echo, "%S is undefined", str8(seq, n));
        app->ctx.last_command = NULL;
        return;
    case KEY_RESULT_QUIT: // a pending prefix cancelled: the box stays
        echo_message(&app->echo, "Quit");
        app->ctx.last_command = NULL;
        return;
    case KEY_RESULT_COMMAND:
    case KEY_RESULT_SELF_INSERT:
        break;
    default: // describe-key and quoted-insert are refused below, so their results cannot come
        echo_clear(&app->echo);
        return;
    }
    const Command *cmd = k.command;
    echo_clear(&app->echo);
    if (cmd->flags & COMMAND_QUIT) { // C-g, ESC: closed, nothing said, the frame underneath as it was
        modal_close(app);
        app->ctx.last_command = NULL;
        return;
    }
    b32 field = m->spec.field && (cmd->flags & COMMAND_FIELD);
    if (!field && !modal_owns(own, cmd)) {
        echo_message(&app->echo, "%s is not available in the %S", cmd->name, m->spec.name);
        app->ctx.last_command = NULL;
        return;
    }
    app->ctx.view = m->spec.field ? &m->field_view : app_selected_view(app);
    app->ctx.codepoint = k.codepoint;
    app->ctx.shift_translated = k.shift_translated;
    view_run_command(&app->ctx, cmd);
    modal_after(app);
}

// A left press: outside the box it closes, as C-g does (the press does nothing else); in the field it moves
// point; on a row it selects it and focuses its pane, and a double click accepts it.
static void modal_click(App *app, AppLayout *l, i32 x, i32 y, i32 clicks) {
    Modal *m = &app->modal;
    ModalGeometry *g = &m->g;
    app->ctx.last_command = NULL;
    if (!modal_inside(g->box, x, y)) {
        modal_close(app);
        return;
    }
    if (m->spec.status_line && !m->spec.field && modal_inside(g->field, x, y)) return; // the status line: nothing
    if (m->spec.field && modal_inside(g->field, x, y)) {
        View *v = &m->field_view;
        view_deactivate_mark(v);
        view_set_point(v, view_primary(v), app_mouse_pos(l, v, x, y));
        view_ensure_visible(v);
        return;
    }
    for (i32 p = 0; p < m->spec.panes; p++) {
        if (!modal_inside(g->panes[p], x, y)) continue;
        ModalList *list = &m->lists[p];
        i64 row = list->top + (i64)((y - (i32)g->panes[p].y0) / MAX(l->line_h, 1));
        if (modal_scrolls(m, p)) { // no selection: the click focuses it; a double click accepts its row
            m->focus = p;
            if (clicks == 2 && m->spec.accept) {
                m->spec.accept(app, m->spec.data, p, row);
                modal_after(app);
            }
            return;
        }
        if (row >= list->count) return;
        m->focus = p;
        i64 before = list->selected;
        list->selected = row;
        modal_list_show(list, g->rows);
        modal_selection_moved(app, p, before);
        if (clicks == 2 && m->spec.accept && m->active) {
            m->spec.accept(app, m->spec.data, p, row);
            modal_after(app);
        }
        return;
    }
}

// The wheel scrolls the pane under the pointer; its selection stays where it is.
static void modal_wheel(App *app, i32 x, i32 y, i32 wheel) {
    Modal *m = &app->modal;
    app->wheel_accum += wheel * APP_WHEEL_LINES;
    i32 lines = app->wheel_accum / 120;
    app->wheel_accum -= lines * 120;
    for (i32 p = 0; lines && p < m->spec.panes; p++) {
        if (!modal_inside(m->g.panes[p], x, y)) continue;
        ModalList *list = &m->lists[p];
        list->top = CLAMP(list->top - lines, 0, MAX(list->count - m->g.rows, 0));
    }
}

// The config was applied again: the owner recomputes what it took from it (bindings).
static void modal_config_applied(App *app) {
    Modal *m = &app->modal;
    if (m->field_buffer) app_buffer_settings(app, m->field_buffer);
    if (m->active && m->spec.reload) m->spec.reload(app, m->spec.data);
}

static i32 modal_shutdown(App *app) {
    Modal *m = &app->modal;
    modal_close(app);
    i32 leaks = 0;
    if (m->field_buffer) {
        view_destroy(&m->field_view);
        leaks += (i32)m->field_buffer->marker_live;
        if (!buffer_destroy(m->field_buffer)) leaks++;
        m->field_buffer = NULL;
    }
    if (m->arena.base) os_release(m->arena.base);
    m->arena = (Arena){ 0 };
    return leaks;
}

static void modal_fill(Renderer *r, Rect rect, u32 rgb) {
    if (rect.x1 > rect.x0 && rect.y1 > rect.y0) r_push_rect(r, rect, COLOR_HEX(rgb));
}

// Drawn last: the frame, the field line and its status, the separator, each pane's rows (the selected row of the
// focused pane in modal_selection, of the other pane in modal_border), all cut to the box.
static void modal_draw(App *app, Renderer *r, AppLayout *l, FrameInput *in) {
    Modal *m = &app->modal;
    if (!m->active || !m->g.fits) return;
    Theme *th = &app->config->theme;
    ModalGeometry *g = &m->g;
    i32 b = g->border;
    Rect box = g->box;
    r_set_clip(r, box);
    modal_fill(r, box, th->modal_border);
    modal_fill(r, (Rect){ box.x0 + b, box.y0 + b, box.x1 - b, box.y1 - b }, th->modal_background);
    if (m->spec.status_line && !m->spec.field) { // the status alone, from the left
        Rect f = g->field;
        modal_fill(r, (Rect){ f.x0, f.y1, f.x1, f.y1 + b }, th->modal_border);
        r_set_clip(r, f);
        String8 status = m->spec.status ? m->spec.status(app, m->spec.data, in->scratch) : str8(NULL, 0);
        if (status.len) font_draw_text(app->font, r, (i32)f.x0 + l->pad, (i32)f.y0, status, COLOR_HEX(th->modal_description));
    }
    if (m->spec.field) {
        Rect f = g->field;
        modal_fill(r, (Rect){ f.x0, f.y1, f.x1, f.y1 + b }, th->modal_border);
        r_set_clip(r, f);
        View *v = &m->field_view;
        app_draw_region(app, r, l, v, view_primary(v), 1);
        app_draw_buffer_line(app, r, v->buffer, view_top_line(v), v->x + l->pad, v->y, v->left_col, v->cols, NULL, 0, in->scratch);
        app_draw_cursor(app, r, l, v, view_point(v, view_primary(v)), 1, app_has_focus(app), in->dpi_scale, in->scratch);
        String8 status = m->spec.status ? m->spec.status(app, m->spec.data, in->scratch) : str8(NULL, 0);
        i32 x = (i32)f.x1 - l->pad - (i32)app_text_cells(status) * l->cell_w;
        if (status.len && x > v->x + v->w) font_draw_text(app->font, r, x, (i32)f.y0, status, COLOR_HEX(th->modal_description));
    }
    if (m->spec.panes > 1) {
        Rect a = g->panes[0];
        r_set_clip(r, box);
        modal_fill(r, (Rect){ a.x1, a.y0, a.x1 + b, a.y1 }, th->modal_border);
    }
    for (i32 p = 0; p < m->spec.panes; p++) {
        Rect pane = g->panes[p];
        ModalList *list = &m->lists[p];
        r_set_clip(r, pane);
        for (i32 i = 0; i < g->rows && list->top + i < list->count; i++) {
            i64 row = list->top + i;
            Rect rr = { pane.x0, pane.y0 + (f32)(i * l->line_h), pane.x1, pane.y0 + (f32)((i + 1) * l->line_h) };
            b32 selected = row == list->selected;
            if (selected) modal_fill(r, rr, p == m->focus ? th->modal_selection : th->modal_border);
            if (!m->spec.draw_row) continue;
            ModalRow mr = { p, row, rr, (i32)pane.x0 + l->pad, MAX(((i32)(pane.x1 - pane.x0) - 2 * l->pad) / l->cell_w, 0), selected };
            m->spec.draw_row(app, m->spec.data, r, &mr);
        }
    }
    r_set_clip(r, (Rect){ 0, 0, (f32)in->width, (f32)in->height });
}

// ---------------------------------------------------------------------------
// The layer's commands: bound in an owner's [keys ...] section; outside a modal they say so.

static Modal *modal_of(CommandContext *ctx) {
    if (ctx->app && ctx->app->modal.active) return &ctx->app->modal;
    echo_message(ctx->echo, "No modal is open");
    return NULL;
}

static void modal_move(CommandContext *ctx, i32 pages, i32 rows) {
    Modal *m = modal_of(ctx);
    if (!m) return;
    ModalList *l = &m->lists[m->focus];
    if (!l->count) return;
    i64 by = pages ? (i64)pages * MAX(m->g.rows, 1) : rows;
    if (modal_scrolls(m, m->focus)) { // no selection: the rows scroll, the last page stays full
        l->top = CLAMP(l->top + by, 0, MAX(l->count - m->g.rows, 0));
        return;
    }
    i64 before = l->selected;
    l->selected = CLAMP(l->selected + by, 0, l->count - 1);
    modal_list_show(l, m->g.rows);
    modal_selection_moved(ctx->app, m->focus, before);
}

static void cmd_modal_next(CommandContext *ctx) { modal_move(ctx, 0, 1); }
static void cmd_modal_previous(CommandContext *ctx) { modal_move(ctx, 0, -1); }
static void cmd_modal_next_page(CommandContext *ctx) { modal_move(ctx, 1, 0); }
static void cmd_modal_previous_page(CommandContext *ctx) { modal_move(ctx, -1, 0); }

static void cmd_modal_accept(CommandContext *ctx) {
    Modal *m = modal_of(ctx);
    if (!m) return;
    ModalList *l = &m->lists[m->focus];
    if (!m->spec.accept) return;
    if (modal_scrolls(m, m->focus)) m->spec.accept(ctx->app, m->spec.data, m->focus, l->top); // its top row (maybe none yet)
    else if (l->selected >= 0 && l->selected < l->count) m->spec.accept(ctx->app, m->spec.data, m->focus, l->selected);
}

static void cmd_modal_other_pane(CommandContext *ctx) {
    Modal *m = modal_of(ctx);
    if (m) m->focus = (m->focus + 1) % MAX(m->spec.panes, 1);
}

const Command CMD_MODAL_NEXT          = { "modal-next", "Select the next row of the open modal", cmd_modal_next, COMMAND_ONCE };
const Command CMD_MODAL_PREVIOUS      = { "modal-previous", "Select the previous row of the open modal", cmd_modal_previous, COMMAND_ONCE };
const Command CMD_MODAL_NEXT_PAGE     = { "modal-next-page", "Select the row one page down in the open modal", cmd_modal_next_page, COMMAND_ONCE };
const Command CMD_MODAL_PREVIOUS_PAGE = { "modal-previous-page", "Select the row one page up in the open modal", cmd_modal_previous_page, COMMAND_ONCE };
const Command CMD_MODAL_ACCEPT        = { "modal-accept", "Accept the selected row of the open modal", cmd_modal_accept, COMMAND_ONCE };
const Command CMD_MODAL_OTHER_PANE    = { "modal-other-pane", "Move the focus to the next pane of the open modal", cmd_modal_other_pane, COMMAND_ONCE };
