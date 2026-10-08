// app.c — the editor core: the config (loaded before the font), keys through the keymap,
// views laid out in the frame, drawing of text in its token colors, cursors, mode lines and the
// echo area. View logic lives in view.c.

#define APP_MAX_VIEWS 8
#define APP_WHEEL_LINES 3 // per notch (120 units)
#define APP_PAD_PX 4      // left padding of a text area at 96 DPI
#define APP_CONFIG_RESERVE MB(16)
#define APP_HEADLESS_CELL_W 8  // the cell of a headless app (dev: --test), which has no font
#define APP_HEADLESS_LINE_H 16

// A range of buffer positions and a color (drawing).
typedef struct AppSpan {
    i64 start, end;
    u32 rgb;
} AppSpan;

// show_paren_mode, per view: the pair found for a buffer state and point (-1: none).
typedef struct AppParen {
    Buffer *buffer;
    u64 edit_count;
    i64 point, valid;
    i64 a, b;
} AppParen;

typedef struct AppWatch {
    String8 dir;
    OsWatch watch; // 0: the directory could not be watched (not tried again while it is displayed)
} AppWatch;

struct App {
    Font *font;
    BufferList buffers;          // every buffer, in creation order
    Buffer *messages;            // *Messages*: the echo area's log
    KillRing kills;              // one for every buffer
    View *views[APP_MAX_VIEWS];  // laid out side by side
    AppParen parens[APP_MAX_VIEWS]; // the matching brackets each view last showed
    i32 view_count;
    i32 active_view;
    Echo echo;
    CommandContext ctx;          // keeps last_command between events
    KeyInput keys;               // the key sequence state
    Minibuffer mini;             // prompts; while active its keymap comes before the global one
    Isearch isearch;             // while active its keymap comes before the global one
    Replace replace;             // query-replace and replace-string: while active, keys go to it first
    Search lazy;                 // the lazy highlight: the other matches on the drawn rows
    b32 search_pending;          // a search step is still being searched: another frame
    Arena files_arena;           // files.c: paths being built; reset by each use
    AppWatch watches[APP_MAX_VIEWS]; // the directories of the displayed file buffers
    i32 watch_count;
    Arena watch_arena;           // their names; rebuilt when the set changes
    b32 disk_pending;            // a change notification came: check the displayed buffers at disk_due_us
    u64 disk_due_us;
    i32 disk_attempts;           // reloads that met a sharing violation, in a row
    i32 unsaved_reported;        // the last os_set_unsaved_files value, -1 = none yet
    Config *config;              // in config_arenas[config_slot]
    Arena config_arenas[2];      // a load parses into the other arena, then switches
    i32 config_slot;
    String8 config_path;         // the user's teal.conf; empty = built-in defaults only
    ConfigSource config_source;  // when to read it again
    OsWatch config_watch;        // its directory
    i32 forced_render_mode;      // dev --render-mode, -1 = from the config
    i32 text_scale;              // text-scale-increase / decrease steps, session only
    i32 wheel_scale_accum;       // Ctrl + wheel units not yet turned into text scale steps
    b32 dragging;                // the left button went down in a text area and is held
    View *drag_view;             // ... of this view (or the minibuffer)
    i64 drag_anchor;             // where it went down (a drag selects from there)
    Renderer *renderer;          // during a frame: commands that change the font rebind the atlas at once
    b32 quit;
    b32 focused;                 // the window has keyboard focus
    i32 wheel_accum;             // wheel units * APP_WHEEL_LINES not yet turned into lines
    u8 title[256];               // the window title last set
    i32 title_len;
    i64 initial_line;            // 0-based line to visit on the first frame, -1 = none
    i32 laid_w, laid_h, laid_cell_w, laid_line_h; // the layout the views were last fitted to
    i64 initial_col;
    b32 syntax_pending;          // some view's visible lines still need lexer states: another frame
#if TEAL_DEV
    i32 force_focus;  // -1: follow focus events; 0 / 1: forced (smoke, screenshots)
    u64 dev_build_us; // last frame: time from the start of the frame to r_end_frame
    i64 dev_work_budget; // search positions per frame instead of the clock (deterministic tests); 0 = the clock
    b32 dev_log_keys;    // --log-keys: every key and text event's result goes to the log
#endif
};

typedef struct AppLayout {
    i32 cell_w, line_h;
    i32 pad;             // left padding of text areas, pixels
    i32 cols, rows;      // whole-frame grid
    i32 mode_line_y;     // the mode line of a full-height view
    i32 minibuffer_y;    // the echo area occupies [minibuffer_y, minibuffer_y + line_h)
} AppLayout;

static AppLayout app_layout(App *app, FrameInput *in) {
    AppLayout l;
    l.cell_w = app->font ? app->font->cell_w : APP_HEADLESS_CELL_W;
    l.line_h = app->font ? app->font->line_h : APP_HEADLESS_LINE_H;
    l.pad = MAX((i32)(APP_PAD_PX * in->dpi_scale + 0.5f), 1);
    l.minibuffer_y = in->height - l.line_h;
    l.mode_line_y = l.minibuffer_y - l.line_h;
    l.cols = MAX(in->width / l.cell_w, 1);
    l.rows = MAX(l.mode_line_y / l.line_h, 1);
    return l;
}

// Cells of a string: one per codepoint (or invalid byte), as font_draw_text.
static i64 app_text_cells(String8 s) {
    i64 cells = 0;
    for (i64 i = 0; i < s.len; i++) cells += (s.data[i] & 0xC0) != 0x80;
    return cells;
}

// Hands every view its rect: equal columns side by side above the echo area. Each view is its
// text area plus a mode line at the bottom.
static void app_layout_views(App *app, FrameInput *in, AppLayout *l) {
    i32 area_h = MAX(l->minibuffer_y, 0);
    for (i32 i = 0; i < app->view_count; i++) {
        View *v = app->views[i];
        v->x = in->width * i / app->view_count;
        v->w = in->width * (i + 1) / app->view_count - v->x;
        v->y = 0;
        v->h = area_h;
        v->rows = MAX((v->h - l->line_h) / l->line_h, 1);
        v->cols = MAX((v->w - l->pad) / l->cell_w, 1);
    }
    // The minibuffer: its text area starts after the prompt.
    View *m = app->mini.view;
    m->x = (i32)MIN(app_text_cells(app->mini.prompt), (i64)l->cols) * l->cell_w;
    m->y = l->minibuffer_y;
    m->w = MAX(in->width - m->x - (app->mini.kind == MINI_CHOICE ? 16 * l->cell_w : 0), l->pad + l->cell_w); // room for "3/41"
    m->h = l->line_h;
    m->rows = 1;
    m->cols = MAX((m->w - l->pad) / l->cell_w, 1);
}

static b32 app_has_focus(App *app) {
#if TEAL_DEV
    if (app->force_focus >= 0) return app->force_focus;
#endif
    return app->focused;
}

static const char *app_encoding_name(BufferEncoding e) {
    switch (e) {
    case BUFFER_UTF8:     return "UTF-8";
    case BUFFER_UTF8_BOM: return "UTF-8 BOM";
    case BUFFER_UTF16LE:  return "UTF-16LE";
    case BUFFER_UTF16BE:  return "UTF-16BE";
    }
    return "?";
}

static const char *app_eol_name(BufferEol e) {
    switch (e) {
    case BUFFER_EOL_LF:    return "LF";
    case BUFFER_EOL_CRLF:  return "CRLF";
    case BUFFER_EOL_MIXED: return "Mixed";
    }
    return "?";
}


// Text shorter than `cells` cells: one cell per codepoint (or invalid byte), as font_draw_text.
static String8 app_clip_cells(String8 s, i64 cells) {
    i64 i = 0;
    for (i64 c = 0; i < s.len && c < cells; c++) {
        i64 advance = 1;
        if (s.data[i] >= 0x80) utf8_decode(s.data + i, s.len - i, &advance);
        i += advance;
    }
    return str8(s.data, i);
}

// ---------------------------------------------------------------------------
// Drawing

// The two cells of a control character in caret notation (^@, ^M, ^?), each only if visible.
static void app_draw_caret(App *app, Renderer *r, i32 x0, i32 y, i64 col, i64 left, i64 right, u8 b, Color color) {
    u8 cells[2] = { '^', (u8)(b ^ 0x40) };
    for (i32 k = 0; k < 2; k++) {
        i64 c = col + k;
        if (c >= left && c < right) font_draw_text(app->font, r, x0 + (i32)(c - left) * app->font->cell_w, y, str8(cells + k, 1), color);
    }
}

static u32 app_kind_color(Theme *th, u32 kind) {
    switch (kind) {
    case SYN_COMMENT:   return th->comment;
    case SYN_STRING:    return th->string;
    case SYN_NUMBER:    return th->number;
    case SYN_KEYWORD:   return th->keyword;
    case SYN_TYPE:      return th->type;
    case SYN_CONSTANT:  return th->constant;
    case SYN_DIRECTIVE: return th->directive;
    case SYN_FUNCTION:  return th->function;
    case SYN_VARIABLE:  return th->variable;
    }
    return th->text; // text, punctuation, invalid
}

// One line of the buffer, only its columns [left, left + cols), in its token colors. x0 is the x
// of column `left`. `over`: the text of [over->start, over->end) is drawn in over->rgb instead (the
// current search match); NULL = none.
static void app_draw_buffer_line(App *app, Renderer *r, Buffer *buf, i64 line, i32 x0, i32 y, i64 left, i64 cols, AppSpan *over,
                                 Arena *scratch) {
    Theme *th = &app->config->theme;
    i32 cell_w = app->font->cell_w;
    i64 right = left + cols;
    i64 start = buffer_line_start(buf, line), end = buffer_line_end(buf, line);
    i64 col = 0;
    // The first character that reaches column `left` (a tab or ^X may start before it).
    i64 pos = view_walk(buf, start, end, &col, left);
    // Every visible column needs at most 4 bytes, so a huge line is never copied whole.
    String8 s = buffer_text(buf, scratch, pos, MIN(end, pos + (cols + 1) * 4));
    // The line's tokens up to its last visible byte; beyond SYNTAX_DRAW_MAX bytes into the line, plain.
    SyntaxTokens toks = { 0 };
    i64 lexed = 0;
    i64 lex_end = MIN(pos + s.len, start + SYNTAX_DRAW_MAX);
    if (buf->states_on && pos < lex_end) {
        String8 prefix = buffer_text(buf, scratch, start, lex_end);
        toks.cap = (i32)prefix.len + 2;
        toks.tokens = PUSH_ARRAY(scratch, SyntaxToken, toks.cap);
        if (syntax_line_tokens(buf, line, prefix, &toks)) lexed = prefix.len;
    }
    i32 k = 0;
    u32 run_rgb = th->text;
    i64 run = 0, run_col = col;
    i64 i = 0;
    while (i < s.len && col < right) {
        u8 b = s.data[i];
        i64 at = pos + i - start;
        u32 rgb = th->text;
        if (at < lexed) {
            while (k + 1 < toks.count && (i64)toks.tokens[k + 1].start <= at) k++;
            if (toks.count && (i64)toks.tokens[k].start <= at) rgb = app_kind_color(th, toks.tokens[k].kind);
        }
        if (over && pos + i >= over->start && pos + i < over->end) rgb = over->rgb;
        if (rgb != run_rgb) {
            if (i > run) font_draw_text(app->font, r, x0 + (i32)(run_col - left) * cell_w, y, str8(s.data + run, i - run), COLOR_HEX(run_rgb));
            run = i;
            run_col = col;
            run_rgb = rgb;
        }
        if (b == '\t' || view_is_control(b)) {
            if (i > run) font_draw_text(app->font, r, x0 + (i32)(run_col - left) * cell_w, y, str8(s.data + run, i - run), COLOR_HEX(run_rgb));
            if (b != '\t') app_draw_caret(app, r, x0, y, col, left, right, b, COLOR_HEX(th->number));
            col += view_char_width(b, col, buf->tab_width);
            run = ++i;
            run_col = col;
            continue;
        }
        i64 advance = 1;
        if (b >= 0x80) utf8_decode(s.data + i, s.len - i, &advance);
        i += advance;
        col++;
    }
    if (i > run) font_draw_text(app->font, r, x0 + (i32)(run_col - left) * cell_w, y, str8(s.data + run, i - run), COLOR_HEX(run_rgb));
}

// The cell rect of a cursor, or false when it is outside the window's first `rows` rows (only the
// primary cursor is kept visible).
static b32 app_cursor_rect(App *app, AppLayout *l, View *v, i64 pos, i32 rows, i32 *x0, i32 *y0, i32 *x1, i32 *y1) {
    Buffer *buf = v->buffer;
    i64 row = buffer_line_of(buf, pos) - view_top_line(v);
    if (row < 0 || row >= rows) return 0;
    i64 col = view_column_of(buf, pos);
    i64 w = pos < buffer_size(buf) && view_is_control(buffer_byte(buf, pos)) ? 2 : 1;
    if (col < v->left_col || col + w > v->left_col + v->cols) return 0;
    *x0 = v->x + l->pad + (i32)(col - v->left_col) * app->font->cell_w;
    *y0 = v->y + (i32)row * app->font->line_h;
    *x1 = *x0 + (i32)w * app->font->cell_w;
    *y1 = *y0 + app->font->line_h;
    return 1;
}

// Filled: a block with the character under it in the background color. Hollow: a box.
static void app_draw_cursor(App *app, Renderer *r, AppLayout *l, View *v, i64 pos, i32 rows, b32 filled, f32 dpi_scale, Arena *scratch) {
    i32 x0, y0, x1, y1;
    if (!app_cursor_rect(app, l, v, pos, rows, &x0, &y0, &x1, &y1)) return;
    Color cursor = COLOR_HEX(app->config->theme.cursor);
    if (!filled) {
        i32 t = MAX((i32)(dpi_scale + 0.5f), 1);
        r_push_rect(r, (Rect){ (f32)x0, (f32)y0, (f32)x1, (f32)(y0 + t) }, cursor);
        r_push_rect(r, (Rect){ (f32)x0, (f32)(y1 - t), (f32)x1, (f32)y1 }, cursor);
        r_push_rect(r, (Rect){ (f32)x0, (f32)(y0 + t), (f32)(x0 + t), (f32)(y1 - t) }, cursor);
        r_push_rect(r, (Rect){ (f32)(x1 - t), (f32)(y0 + t), (f32)x1, (f32)(y1 - t) }, cursor);
        return;
    }
    r_push_rect(r, (Rect){ (f32)x0, (f32)y0, (f32)x1, (f32)y1 }, cursor);
    Buffer *buf = v->buffer;
    if (pos >= buffer_line_end(buf, buffer_line_of(buf, pos))) return; // end of line: nothing under it
    u8 b = buffer_byte(buf, pos);
    if (b == '\t') return;
    Color under = COLOR_HEX(app->config->theme.background);
    if (view_is_control(b)) app_draw_caret(app, r, x0, y0, 0, 0, 2, b, under);
    else font_draw_text(app->font, r, x0, y0, buffer_text(buf, scratch, pos, buffer_next_char(buf, pos)), under);
}

// " -:**-  win32_main.c    37%   L120 C8    (C)    UTF-8 CRLF"
static String8 app_mode_line_text(View *v, Arena *arena) {
    Buffer *buf = v->buffer;
    const char *flags = buf->read_only ? "%%" : buf->modified ? "**" : "--";
    i64 name_cells = 0;
    for (i64 i = 0; i < buf->name.len; i++) name_cells += (buf->name.data[i] & 0xC0) != 0x80;
    String8 pad = str8((u8 *)"            ", MAX(12 - name_cells, 0)); // Emacs pads the name to 12 (%12b)
    const char *disk = buf->disk_state == BUFFER_DISK_CHANGED ? "  [changed on disk]" : buf->disk_state == BUFFER_DISK_DELETED ? "  [deleted on disk]" : "";
    i64 top_pos = buffer_marker_get(buf, v->top);
    i64 top = buffer_line_of(buf, top_pos);
    b32 bottom = top + v->rows >= buffer_line_count(buf);
    String8 where = top == 0 && bottom ? STR8_LIT("All") : top == 0 ? STR8_LIT("Top") : bottom ? STR8_LIT("Bot")
                  : str8_fmt(arena, "%D%%", top_pos * 100 / MAX(buffer_size(buf), 1));
    i64 p = view_point(v, &v->cursors[0]);
    return str8_fmt(arena, " -:%s-  %S%s%S    %S   L%D C%D    (%s)    %s %s", flags, buf->name, disk, pad, where,
                    buffer_line_of(buf, p) + 1, view_column_of(buf, p), buffer_language_name(buf->language),
                    app_encoding_name(buf->encoding), app_eol_name(buf->eol));
}

// A background color behind the cells of [start, end) on the visible rows; on a line whose newline is
// in the range it extends to the right edge of the window. Text is drawn over it.
static void app_draw_span(Renderer *r, AppLayout *l, View *v, i64 start, i64 end, i32 draw_rows, u32 rgb) {
    if (start >= end) return;
    Buffer *buf = v->buffer;
    i64 top = view_top_line(v), count = buffer_line_count(buf);
    i32 x0 = v->x + l->pad, right = v->x + v->w;
    Color color = COLOR_HEX(rgb);
    for (i32 row = 0; row < draw_rows && top + row < count; row++) {
        i64 line = top + row, ls = buffer_line_start(buf, line), le = buffer_line_end(buf, line);
        if (end <= ls || start > le) continue;
        i64 a = MAX(start, ls), b = MIN(end, le);
        i64 ca = view_column_of(buf, a) - v->left_col, cb = view_column_of(buf, b) - v->left_col;
        b32 newline = end > le; // the line's newline is in the range
        f32 xa = (f32)(x0 + (i32)MAX(ca, 0) * l->cell_w);
        f32 xb = newline ? (f32)right : (f32)MIN(x0 + (i32)MAX(cb, 0) * l->cell_w, right);
        f32 y = (f32)(v->y + row * l->line_h);
        if (xb > xa) r_push_rect(r, (Rect){ xa, y, xb, y + (f32)l->line_h }, color);
    }
}

// The selection color behind an active region.
static void app_draw_region(App *app, Renderer *r, AppLayout *l, View *v, Cursor *c, i32 draw_rows) {
    i64 start, end;
    if (!view_region_active(v, c, &app->config->settings) || !view_region(v, c, &start, &end)) return;
    app_draw_span(r, l, v, start, end, draw_rows, app->config->theme.selection);
}

// show_paren_mode: the closing bracket just before point (it wins, as in Emacs) or the opening one at
// point, and its match: false when there is none (a bracket in a comment or string, unmatched, too
// far). Cached per view while the buffer, its states and point stay the same.
static b32 app_paren(App *app, i32 index, Arena *scratch, i64 *a, i64 *b) {
    View *v = app->views[index];
    Buffer *buf = v->buffer;
    AppParen *c = &app->parens[index];
    i64 p = view_point(v, &v->cursors[0]);
    if (c->buffer != buf || c->edit_count != buf->edit_count || c->point != p || c->valid != buf->state_valid) {
        *c = (AppParen){ buf, buf->edit_count, p, buf->state_valid, -1, -1 };
        i64 size = buffer_size(buf);
        i64 at[2] = { -1, -1 };
        if (p > 0) {
            u8 before = buffer_byte(buf, p - 1);
            if (before == ')' || before == ']' || before == '}') at[0] = p - 1;
        }
        if (p < size) {
            u8 here = buffer_byte(buf, p);
            if (here == '(' || here == '[' || here == '{') at[1] = p;
        }
        for (i32 k = 0; k < 2 && c->a < 0; k++) {
            i64 m = at[k] >= 0 ? syntax_match_bracket(buf, at[k], scratch) : -1;
            if (m >= 0) {
                c->a = at[k];
                c->b = m;
            }
        }
    }
    *a = c->a;
    *b = c->b;
    return c->a >= 0;
}

// The lazy highlight: every match of `needle` that starts on a drawn row (and in [lo, hi)), except
// the current one. Each row is searched only within its on-screen bytes (plus the needle's length), the
// same bound line drawing has, so the cost never depends on the buffer's size.
static void app_draw_lazy(App *app, Renderer *r, AppLayout *l, View *v, i32 draw_rows, String8 needle, b32 fold,
                          i64 current, i64 lo, i64 hi) {
    Buffer *buf = v->buffer;
    Search *s = &app->lazy;
    if (!search_begin(s, buf, needle, fold, 1, 0, 0, 0)) return;
    i64 top = view_top_line(v), count = buffer_line_count(buf), size = buffer_size(buf);
    for (i32 row = 0; row < draw_rows && top + row < count; row++) {
        i64 line = top + row, ls = buffer_line_start(buf, line), le = buffer_line_end(buf, line);
        i64 col = 0;
        i64 shown = view_walk(buf, ls, le, &col, v->left_col); // the first byte on screen
        i64 row_lo = MAX(MAX(ls, shown - s->len + 1), lo);
        i64 row_hi = MIN(MIN(size, MIN(le, shown + ((i64)v->cols + 1) * 4) + s->len - 1), hi);
        for (i64 from = row_lo; from < row_hi;) {
            search_restart(s, buf, 1, from, row_lo, row_hi);
            if (search_run(s, buf, I64_MAX / 4) != SEARCH_FOUND) break;
            if (s->match_start != current) app_draw_span(r, l, v, s->match_start, s->match_end, draw_rows, app->config->theme.lazy_highlight);
            from = s->match_end;
        }
    }
}

// The search shown in a view (an isearch, or the question of a query-replace): the other matches, then
// the current match's background. Returns the current match's text color range for line drawing
// (start -1: none).
static AppSpan app_draw_search(App *app, Renderer *r, AppLayout *l, View *v, i32 draw_rows) {
    AppSpan current = { -1, -1, app->config->theme.isearch_text };
    Replace *rp = &app->replace;
    if (rp->state == REPLACE_ASKING && rp->view == v) {
        app_draw_lazy(app, r, l, v, draw_rows, rp->from, rp->fold, rp->match_start, rp->lo, buffer_marker_get(v->buffer, rp->end));
        current.start = rp->match_start;
        current.end = rp->match_end;
        app_draw_span(r, l, v, current.start, current.end, draw_rows, app->config->theme.isearch);
        return current;
    }
    Isearch *is = &app->isearch;
    if (!is->active || is->view != v) return current;
    IsearchStep *cur = isearch_current(is), *top = isearch_top(is);
    if (!isearch_pending(is) && top->success && top->string.len) {
        app_draw_lazy(app, r, l, v, draw_rows, top->string, top->fold, cur->match_start, 0, buffer_size(v->buffer));
    }
    if (cur->match_start >= 0) {
        current.start = cur->match_start;
        current.end = cur->match_end;
        app_draw_span(r, l, v, current.start, current.end, draw_rows, app->config->theme.isearch);
    }
    return current;
}

// `bottom`: the view is drawn above it (the candidate list covers the rest), its mode line moved up.
// Its rows and scroll position are not touched, so nothing scrolls when the list opens or closes.
static void app_draw_view(App *app, Renderer *r, AppLayout *l, FrameInput *in, i32 index, b32 active, i32 bottom) {
    View *v = app->views[index];
    Buffer *buf = v->buffer;
    i32 line_h = l->line_h;
    i32 text_x = v->x + l->pad;
    i32 mode_y = MAX(MIN(v->y + v->h, bottom) - line_h, v->y);
    i32 rows = MIN(v->rows, (mode_y - v->y) / line_h);
    // Only the visible lines (the partial one above the mode line too, which covers it).
    i64 top = view_top_line(v), count = buffer_line_count(buf);
    i32 draw_rows = (mode_y - v->y + line_h - 1) / line_h;
    for (i32 k = 0; k < v->cursor_count; k++) app_draw_region(app, r, l, v, &v->cursors[k], draw_rows);
    i64 pair[2];
    if (active && app->config->settings.show_paren_mode && app_paren(app, index, in->scratch, &pair[0], &pair[1])) {
        for (i32 k = 0; k < 2; k++) {
            i32 x0, y0, x1, y1;
            if (app_cursor_rect(app, l, v, pair[k], rows, &x0, &y0, &x1, &y1)) {
                r_push_rect(r, (Rect){ (f32)x0, (f32)y0, (f32)x1, (f32)y1 }, COLOR_HEX(app->config->theme.paren_match));
            }
        }
    }
    AppSpan current = app_draw_search(app, r, l, v, draw_rows);
    for (i32 row = 0; row < draw_rows && top + row < count; row++) {
        app_draw_buffer_line(app, r, buf, top + row, text_x, v->y + row * line_h, v->left_col, v->cols, current.start >= 0 ? &current : NULL, in->scratch);
    }
    b32 filled = active && app_has_focus(app);
    for (i32 k = 0; k < v->cursor_count; k++) {
        app_draw_cursor(app, r, l, v, view_point(v, &v->cursors[k]), rows, filled, in->dpi_scale, in->scratch);
    }
    // Mode line, inverse video.
    r_push_rect(r, (Rect){ (f32)v->x, (f32)mode_y, (f32)(v->x + v->w), (f32)(mode_y + line_h) }, COLOR_HEX(app->config->theme.text));
    String8 mode = app_mode_line_text(v, in->scratch);
    font_draw_text(app->font, r, text_x, mode_y, app_clip_cells(mode, MAX((v->w - l->pad) / l->cell_w, 0)), COLOR_HEX(app->config->theme.background));
}

// Rows of the candidate list: up to completion_lines, never the whole window.
static i32 app_list_rows(App *app, AppLayout *l) {
    Minibuffer *mb = &app->mini;
    if (!mb->active || mb->kind != MINI_CHOICE) return 0;
    i64 rows = MIN(mb->match_count, (i64)app->config->settings.completion_lines);
    return (i32)MAX(MIN(rows, (i64)(l->minibuffer_y / l->line_h) - 2), 0);
}

// The text of a candidate from column 0, the matched spans in the match color, clipped to `cells`.
static void app_draw_candidate_text(App *app, Renderer *r, i32 x, i32 y, Candidate *c, MatchSpan *spans, i32 n, i64 cells) {
    Theme *th = &app->config->theme;
    String8 text = app_clip_cells(c->text, cells);
    i64 at = 0;
    while (at < text.len) {
        // The next boundary: the start or end of a span, whichever comes first after `at`.
        b32 matched = 0;
        i64 next = text.len;
        for (i32 k = 0; k < n; k++) {
            if (spans[k].start <= at && at < spans[k].end) {
                matched = 1;
                next = MIN(next, spans[k].end);
            } else if (spans[k].start > at) {
                next = MIN(next, spans[k].start);
            }
        }
        if (matched) { // overlapping spans: run to the end of the furthest one that covers `at`
            for (b32 grew = 1; grew;) {
                grew = 0;
                for (i32 k = 0; k < n; k++) {
                    if (spans[k].start <= next && next < spans[k].end) {
                        next = spans[k].end;
                        grew = 1;
                    }
                }
            }
        }
        next = MIN(next, text.len);
        String8 run = str8(text.data + at, next - at);
        x = font_draw_text(app->font, r, x, y, run, COLOR_HEX(matched ? th->completion_match : th->text));
        at = next;
    }
    if (c->flags & CANDIDATE_DIR && text.len == c->text.len) font_draw_text(app->font, r, x, y, STR8_LIT("/"), COLOR_HEX(th->text));
}

// The candidate list above the minibuffer line: the selected row highlighted, matched substrings in
// their color, annotations right-aligned.
static void app_draw_candidates(App *app, Renderer *r, AppLayout *l, FrameInput *in, i32 rows) {
    Minibuffer *mb = &app->mini;
    Theme *th = &app->config->theme;
    i32 y0 = l->minibuffer_y - rows * l->line_h;
    r_push_rect(r, (Rect){ 0, (f32)y0, (f32)in->width, (f32)l->minibuffer_y }, COLOR_HEX(th->background));
    i64 top = minibuffer_list_top(mb, rows);
    i64 cols = MAX((in->width - 2 * l->pad) / l->cell_w, 1);
    for (i32 row = 0; row < rows && top + row < mb->match_count; row++) {
        i64 index = top + row;
        Candidate *c = &mb->cands[mb->matches[index]];
        i32 y = y0 + row * l->line_h;
        if (index == mb->selected) {
            r_push_rect(r, (Rect){ 0, (f32)y, (f32)in->width, (f32)(y + l->line_h) }, COLOR_HEX(th->completion_selection));
        }
        i64 ann = app_text_cells(c->annotation);
        i64 room = ann && ann + 2 < cols ? cols - ann - 2 : cols;
        MatchSpan spans[MATCH_MAX_TERMS];
        i32 n = match_spans(&mb->query, c, spans, MATCH_MAX_TERMS);
        app_draw_candidate_text(app, r, l->pad, y, c, spans, n, room);
        if (room < cols) {
            i32 x = l->pad + (i32)(cols - ann) * l->cell_w;
            font_draw_text(app->font, r, x, y, c->annotation, COLOR_HEX(th->text));
        }
    }
}

// The minibuffer line: the prompt, the input (a one-line view), and the echo text as a transient
// note in brackets after the input ("[No match]"), as Emacs shows messages while it reads.
static void app_draw_minibuffer(App *app, Renderer *r, AppLayout *l, FrameInput *in) {
    Minibuffer *mb = &app->mini;
    Theme *th = &app->config->theme;
    View *v = mb->view;
    font_draw_text(app->font, r, l->pad, l->minibuffer_y, app_clip_cells(mb->prompt, l->cols), COLOR_HEX(th->prompt));
    app_draw_region(app, r, l, v, &v->cursors[0], 1);
    i64 line = view_top_line(v);
    i32 text_x = v->x + l->pad;
    app_draw_buffer_line(app, r, v->buffer, line, text_x, v->y, v->left_col, v->cols, NULL, in->scratch);
    app_draw_cursor(app, r, l, v, view_point(v, &v->cursors[0]), 1, app_has_focus(app), in->dpi_scale, in->scratch);
    if (mb->kind == MINI_CHOICE) { // "3/41" at the right end
        String8 count = str8_fmt(in->scratch, "%D/%D", mb->match_count ? mb->selected + 1 : 0, mb->match_count);
        i32 x = in->width - l->pad - (i32)count.len * l->cell_w;
        if (x > text_x) font_draw_text(app->font, r, x, v->y, count, COLOR_HEX(th->text));
    }
    if (app->echo.len) {
        i64 end_col = view_column_of(v->buffer, buffer_line_end(v->buffer, line)) - v->left_col + 1;
        i64 room = v->cols - end_col;
        String8 note = str8_fmt(in->scratch, "[%S]", str8(app->echo.text, app->echo.len));
        if (room > 2) font_draw_text(app->font, r, text_x + (i32)end_col * l->cell_w, v->y, app_clip_cells(note, room), COLOR_HEX(th->text));
    }
}

// The echo line of a search session: the prompt in the prompt color, then `text` (the search string)
// with its part from `fail` on the isearch_fail background, then the echo text as a note in brackets
// and `progress` (>= 0: still searching).
static void app_draw_search_line(App *app, Renderer *r, AppLayout *l, FrameInput *in, String8 prompt, String8 text, i64 fail,
                                 String8 progress) {
    Theme *th = &app->config->theme;
    i64 cols = MAX((in->width - l->pad) / l->cell_w, 0);
    i32 y = l->minibuffer_y;
    prompt = app_clip_cells(prompt, cols);
    i32 x = font_draw_text(app->font, r, l->pad, y, prompt, COLOR_HEX(th->prompt));
    cols -= app_text_cells(prompt);
    text = app_clip_cells(text, cols);
    fail = MIN(fail, text.len);
    if (fail < text.len) {
        f32 x0 = (f32)(x + (i32)app_text_cells(str8(text.data, fail)) * l->cell_w);
        f32 x1 = (f32)(x + (i32)app_text_cells(text) * l->cell_w);
        r_push_rect(r, (Rect){ x0, (f32)y, x1, (f32)(y + l->line_h) }, COLOR_HEX(th->isearch_fail));
    }
    x = font_draw_text(app->font, r, x, y, text, COLOR_HEX(th->text));
    cols -= app_text_cells(text);
    String8 note = str8_fmt(in->scratch, "%s%S%s%S", app->echo.len ? "  [" : "", str8(app->echo.text, app->echo.len),
                            app->echo.len ? "]" : "", progress);
    font_draw_text(app->font, r, x, y, app_clip_cells(note, MAX(cols, 0)), COLOR_HEX(th->text));
}

static void app_draw_isearch_line(App *app, Renderer *r, AppLayout *l, FrameInput *in) {
    Isearch *is = &app->isearch;
    String8 progress = isearch_pending(is) ? str8_fmt(in->scratch, "  [searching... %d%%]", search_progress(&is->search)) : str8(NULL, 0);
    app_draw_search_line(app, r, l, in, isearch_prompt(is, in->scratch), isearch_top(is)->string, isearch_fail_pos(is), progress);
}

// ---------------------------------------------------------------------------
// Startup and frames

// ---------------------------------------------------------------------------
// Config

// teal.conf next to the exe if it exists (portable), else %APPDATA%\teal\teal.conf. Nothing is
// created here. Empty: built-in defaults only.
static String8 app_config_path(Arena *perm, AppArgs *args) {
#if TEAL_DEV
    if (args->config_path.len) {
        String8 full = os_full_path(perm, args->config_path);
        return full.len ? full : args->config_path;
    }
    if (!args->user_config) return str8(NULL, 0);
#else
    (void)args;
#endif
    String8 portable = str8_fmt(perm, "%S\\teal.conf", os_exe_dir(perm));
    OsFileInfo info;
    if (os_file_info(portable, &info) == OS_FILE_OK && !info.is_dir) return portable;
    String8 appdata = os_get_env(perm, STR8_LIT("APPDATA"));
    if (!appdata.len) return str8(NULL, 0);
    return str8_fmt(perm, "%S\\teal\\teal.conf", appdata);
}

// The font the config asks for, with the session's text scale (1.2 per step, as in Emacs).
static FontParams app_font_params(App *app) {
    Settings *s = &app->config->settings;
    f32 size = s->font_size;
    for (i32 i = 0; i < app->text_scale; i++) size *= 1.2f;
    for (i32 i = 0; i > app->text_scale; i--) size /= 1.2f;
    FontParams fp = {
        .family = str8(s->font, s->font_len),
        .size_pt = CLAMP(size, 4.0f, 96.0f),
        .line_height_percent = s->line_height,
        .mode = app->forced_render_mode >= 0 ? (FbRenderMode)app->forced_render_mode : s->render_mode,
    };
    return fp;
}

// Polls the config source (config_poll): a read parses into the other arena, and the app switches
// to it when it loaded (at startup also with the defaults alone, while a retry is pending).
static ConfigPoll app_poll_config(App *app, b32 force, b32 startup) {
#if TEAL_DEV
    u64 t0 = os_time_us();
#endif
    i32 slot = app->config_slot ^ 1;
    arena_reset(&app->config_arenas[slot]);
    Config *c = PUSH_STRUCT(&app->config_arenas[slot], Config);
    ConfigPoll result = config_poll(&app->config_source, c, &app->config_arenas[slot], force, os_time_us());
    if (result == CONFIG_POLL_UNCHANGED) return result;
    if (result == CONFIG_POLL_LOADED || startup) {
        app->config = c;
        app->config_slot = slot;
    }
    LOG("config: %S: %s (%s), %D bytes; parsed with the defaults in %U us, %d error(s), %d warning(s)",
        app->config_path.len ? app->config_path : STR8_LIT("(none, built-in defaults only)"),
        buffer_status_text(app->config_source.status),
        result == CONFIG_POLL_LOADED ? "loaded" : result == CONFIG_POLL_RETRY ? "retry" : "failed",
        app->config_source.size, os_time_us() - t0, c->errors, c->warnings);
#if TEAL_DEV
    if (result == CONFIG_POLL_LOADED) for (ConfigDiag *d = c->first_diag; d; d = d->next) LOG("config: %S", d->text);
#endif
    return result;
}

// Watches the config file's directory (when it exists), so edits apply live.
static void app_watch_config(App *app) {
    if (app->config_watch || !app->config_path.len) return;
    String8 dir = str8(app->config_path.data, app->config_path.len - config_file_name(app->config_path).len);
    if (dir.len > 1 && dir.data[dir.len - 1] == '\\' && dir.data[dir.len - 2] != ':') dir.len--;
    app->config_watch = os_watch_dir(dir);
    LOG("config: watching %S: %s", dir, app->config_watch ? "yes" : "no (no such directory)");
}

// Reports a load: every diagnostic goes to *Messages*; the echo area shows the first error and
// how many more, a failed read, or "Reloaded". Nothing while a retry is pending.
static void app_report_config(App *app, ConfigPoll result, b32 reload) {
    String8 name = app->config_path.len ? config_file_name(app->config_path) : STR8_LIT("teal.conf");
    if (result == CONFIG_POLL_FAILED) {
        echo_message(&app->echo, "Cannot read %S: %s", name, buffer_status_text(app->config_source.status));
        return;
    }
    if (result != CONFIG_POLL_LOADED) return;
    Config *c = app->config;
    i32 stored = 0;
    for (ConfigDiag *d = c->first_diag; d; d = d->next, stored++) echo_log(&app->echo, d->text);
    if (stored < c->errors + c->warnings) {
        u8 text[128];
        i64 n = fmt_buf(text, sizeof(text), "%S: %d more not listed", name, c->errors + c->warnings - stored);
        echo_log(&app->echo, str8(text, n));
    }
    for (ConfigDiag *d = c->first_diag; c->errors && d; d = d->next) {
        if (d->warning) continue;
        u8 text[ECHO_CAP];
        i64 n = c->errors == 1 ? fmt_buf(text, sizeof(text), "%S", d->text)
                               : fmt_buf(text, sizeof(text), "%S (and %d more)", d->text, c->errors - 1);
        echo_set(&app->echo, str8(text, n)); // the errors themselves are logged above
        return;
    }
    if (c->errors) echo_message(&app->echo, "%S: %d errors", name, c->errors);
    else if (reload) echo_message(&app->echo, "Reloaded %S", name);
}

// Applies the current config: font, caption color, tab width, keys, settings.
// The settings a buffer keeps per buffer (Emacs' buffer-local variables).
static void app_buffer_settings(App *app, Buffer *buf) {
    Settings *s = &app->config->settings;
    buf->tab_width = s->tab_width;
    if (!buf->indent_detected) buf->indent_tabs = s->indent_with_tabs;
    buffer_undo_set_limit(buf, (u64)s->undo_limit_mb << 20);
}

static void app_apply_config(App *app, Renderer *r, b32 startup) {
    Config *c = app->config;
    if (!startup && app->font) {
        FontParams fp = app_font_params(app);
        font_reconfigure(app->font, r, &fp);
    }
    os_set_caption_color(c->theme.background);
    for (i32 i = 0; i < app->buffers.count; i++) app_buffer_settings(app, app->buffers.entries[i].buffer);
    app_buffer_settings(app, app->mini.buffer);
    app->keys.pending.len = 0;
    app->ctx.settings = &c->settings;
    app->ctx.global = &c->global;
    kill_set_max(&app->kills, c->settings.kill_ring_max);
}

// ---------------------------------------------------------------------------
// Buffers

// Another listed buffer has this name.
static b32 app_name_taken(App *app, Buffer *buf, String8 name) {
    for (i32 i = 0; i < app->buffers.count; i++) {
        Buffer *b = app->buffers.entries[i].buffer;
        if (b != buf && str8_equal(b->name, name)) return 1;
    }
    return 0;
}

// A listed buffer whose name another listed buffer already has is renamed: "name<parent dir>", then
// "name<2>", "name<3>", ...
static void app_uniquify(App *app, Buffer *buf) {
    if (!app_name_taken(app, buf, buf->name)) return;
    String8 base = buf->name;
    String8 parent = buf->path.len ? config_file_name(str8(buf->path.data, MAX(buf->path.len - base.len - 1, 0))) : str8(NULL, 0);
    String8 name = parent.len ? str8_fmt(&buf->meta, "%S<%S>", base, parent) : base;
    for (i32 n = 2; app_name_taken(app, buf, name); n++) name = str8_fmt(&buf->meta, "%S<%d>", base, n);
    buf->name = name;
}

static Buffer *app_new_buffer(App *app, String8 name) {
    Buffer *buf = buffer_create(name);
    if (!buf) os_fatal(STR8_LIT("Out of address space (buffer reserve failed)."));
    app_buffer_settings(app, buf);
    buffer_list_add(&app->buffers, buf);
    return buf;
}

// The buffer visiting `path`: the one already open, a newly loaded one, or a new empty one
// visiting a path that does not exist (Emacs). NULL, with a message, when it cannot be opened.
static Buffer *app_find_file(App *app, String8 path) {
    Buffer *buf = buffer_create(STR8_LIT(""));
    if (!buf) {
        echo_message(&app->echo, "Cannot open %S: out of address space", path);
        return NULL;
    }
    String8 full = os_full_path(&buf->meta, path);
    Buffer *open = buffer_list_find_path(&app->buffers, full.len ? full : path);
    if (open) {
        buffer_destroy(buf);
        return open;
    }
#if TEAL_DEV
    u64 t0 = os_time_us();
#endif
    OsFileStatus status = buffer_load_file(buf, path);
    if (status == OS_FILE_OK) {
        LOG("app: loaded %S: %D bytes, %D lines, %s %s, %U us", buf->path, buffer_size(buf), buffer_line_count(buf),
            app_encoding_name(buf->encoding), app_eol_name(buf->eol), os_time_us() - t0);
        if (app->config->settings.detect_indentation) {
            i32 tabs = edit_detect_tabs(buf);
            buf->indent_detected = tabs >= 0;
            if (tabs >= 0) buf->indent_tabs = tabs;
        }
    } else if (status == OS_FILE_NOT_FOUND) {
        buffer_set_path(buf, full.len ? full : path); // as in Emacs: visit the path as a new file
        echo_message(&app->echo, "(New file)");
    } else {
        echo_message(&app->echo, "Cannot open %S: %s", path, buffer_status_text(status));
        buffer_destroy(buf);
        return NULL;
    }
    app_buffer_settings(app, buf);
    buffer_list_add(&app->buffers, buf);
    app_uniquify(app, buf);
    return buf;
}

// The buffer to show instead of `buf`: the most recently shown other buffer, else the first other
// one in the list; NULL when there is none.
static Buffer *app_other_buffer(App *app, Buffer *buf) {
    BufferEntry *best = NULL;
    for (i32 i = 0; i < app->buffers.count; i++) {
        BufferEntry *e = &app->buffers.entries[i];
        if (e->buffer != buf && (!best || e->last_shown > best->last_shown)) best = e;
    }
    return best ? best->buffer : NULL;
}

static View *app_active_view(App *app) {
    return app->views[app->active_view];
}

// files.c: files changed outside the editor.
static void files_check_all(App *app);
static void files_end_session(App *app);
static void files_report_unsaved(App *app);
static void files_notify(App *app, OsWatch watch);
static void files_poll(App *app);
static void files_update_watches(App *app);
static void files_unwatch_all(App *app);
static u32  files_wait_ms(App *app);

// ---------------------------------------------------------------------------
// Startup

// The config is read before the font, so the font is set up exactly once, as configured.
#if TEAL_DEV
#define APP_STAGE(what) os_dev_stage(what)
#else
#define APP_STAGE(what) ((void)0)
#endif

App *app_create(Arena *perm, AppArgs *args) {
    App *app = PUSH_STRUCT(perm, App);
    syntax_init();
    APP_STAGE("app: syntax_init");
    app->forced_render_mode = args->render_mode_forced ? (i32)args->render_mode : -1;
    app->config_arenas[0] = arena_create(APP_CONFIG_RESERVE);
    app->config_arenas[1] = arena_create(APP_CONFIG_RESERVE);
    app->config_path = app_config_path(perm, args);
    app->config_source.path = app->config_path;
    ConfigPoll config_result = app_poll_config(app, 1, 1);
    APP_STAGE("app: config read and parsed");
    FontParams fp = app_font_params(app);
#if TEAL_DEV
    if (!args->headless)
#endif
    {
        app->font = font_create(perm, &fp, args->dpi_scale);
        if (!app->font) return NULL;
        APP_STAGE("app: font (DirectWrite, metrics, ASCII rasterized)");
    }

    buffer_list_init(&app->buffers);
    app->messages = buffer_create(STR8_LIT("*Messages*")); // first, so everything below is logged
    if (!app->messages) os_fatal(STR8_LIT("Out of address space (buffer reserve failed)."));
    app->messages->read_only = 1;
    buffer_undo_enable(app->messages, 0); // a program buffer: no undo
    app->echo.log = app->messages;
    // As in Emacs: the file (if any), *scratch* (always), *Messages*.
    Buffer *initial = args->file_path.len ? app_find_file(app, args->file_path) : NULL;
    Buffer *scratch = app_new_buffer(app, STR8_LIT("*scratch*"));
    if (!initial) initial = scratch;
    app->messages->tab_width = app->config->settings.tab_width;
    buffer_list_add(&app->buffers, app->messages);

    app->views[0] = view_create(perm, initial);
    app->view_count = 1;
    buffer_list_touch(&app->buffers, initial);
    APP_STAGE("app: buffers and the view");
    Buffer *mini = buffer_create(STR8_LIT(" *Minibuf-1*")); // not listed, as in Emacs
    if (!mini) os_fatal(STR8_LIT("Out of address space (buffer reserve failed)."));
    minibuffer_init(&app->mini, perm, mini);
    isearch_init(&app->isearch, &app->mini);
    app->ctx.isearch = &app->isearch;
    replace_init(&app->replace);
    app->ctx.replace = &app->replace;
    APP_STAGE("app: minibuffer");
    app->files_arena = arena_create(GB(1));
    app->watch_arena = arena_create(MB(16));
    app->unsaved_reported = -1;
    app->ctx.mini = &app->mini;
    app->ctx.app = app;
    app->ctx.echo = &app->echo;
    if (!kill_init(&app->kills, app->config->settings.kill_ring_max)) os_fatal(STR8_LIT("Out of address space (kill ring)."));
    app->ctx.kills = &app->kills;
    APP_STAGE("app: kill ring");
    app_apply_config(app, NULL, 1);
    app_report_config(app, config_result, 0);
    app_watch_config(app);
    APP_STAGE("app: config applied, its directory watched");
    if (app->font && app->font->used_fallback) {
        echo_message(&app->echo, "Font '%S' not found, using Consolas", str8(app->font->family, app->font->family_len));
    }
    // +LINE:COLUMN, 1-based on the command line as in Emacs (move-to-column (1- COLUMN)).
    app->initial_line = args->goto_line > 0 ? args->goto_line - 1 : -1;
    app->initial_col = MAX(args->goto_col - 1, 0);
#if TEAL_DEV
    app->force_focus = -1;
#endif
    return app;
}

i32 app_shutdown(App *app) {
    i32 leaks = app->font ? font_shutdown(app->font) : 0;
    for (i32 i = 0; i < app->view_count; i++) view_destroy(app->views[i]);
    leaks += minibuffer_destroy(&app->mini);
    isearch_destroy(&app->isearch);
    replace_destroy(&app->replace); // before the buffers: its marker
    os_release(app->files_arena.base);
    files_unwatch_all(app);
    os_release(app->watch_arena.base);
    leaks += buffer_list_destroy(&app->buffers);
    kill_destroy(&app->kills);
    for (i32 i = 0; i < 2; i++) os_release(app->config_arenas[i].base);
    os_unwatch(app->config_watch);
    return leaks;
}

// The view under a pixel, or -1.
static i32 app_view_at(App *app, i32 x, i32 y) {
    for (i32 i = 0; i < app->view_count; i++) {
        View *v = app->views[i];
        if (x >= v->x && x < v->x + v->w && y >= v->y && y < v->y + v->h) return i;
    }
    return -1;
}

// The buffer position under a pixel of a view's text area; outside the area (a drag with the
// mouse captured) the row and column are clamped to it. The partial last row counts as the last one.
static i64 app_mouse_pos(AppLayout *l, View *v, i32 x, i32 y) {
    i64 row = CLAMP((i64)(y - v->y) / l->line_h, 0, (i64)v->rows - 1);
    if (y < v->y) row = 0;
    i64 col = v->left_col + MAX(x - (v->x + l->pad), 0) / l->cell_w;
    Buffer *buf = v->buffer;
    i64 line = MIN(view_top_line(v) + row, buffer_line_count(buf) - 1);
    return view_offset_at_column(buf, line, col);
}

// A left press in a text area activates its view. One click puts point at the cell (a drag from
// there selects); a double click selects the word, a triple click the line with its newline.
// While the minibuffer is active only its line takes clicks.
static void app_click(App *app, AppLayout *l, i32 x, i32 y, i32 clicks) {
    View *v;
    isearch_exit(&app->ctx); // a click ends a search at its match first
    if (app->replace.state == REPLACE_SEARCHING || app->replace.state == REPLACE_ALL) return; // clicks wait for it
    replace_finish(&app->replace, REPLACE_END_DONE); // a click ends a question
    if (app->mini.active) {
        v = app->mini.view;
        if (y < v->y || x < v->x) return;
    } else {
        i32 i = app_view_at(app, x, y);
        if (i < 0) return;
        v = app->views[i];
        if (y >= v->y + v->h - l->line_h) return; // the mode line
        app->active_view = i;
    }
    Buffer *buf = v->buffer;
    Cursor *c = &v->cursors[0];
    i64 pos = app_mouse_pos(l, v, x, y);
    view_deactivate_mark(v);
    if (clicks == 2) {
        i64 start, end;
        view_word_bounds(buf, pos, app->config->settings.underscore_is_word, &start, &end);
        view_set_mark(v, c, start, 1);
        pos = end;
    } else if (clicks == 3) {
        i64 line = buffer_line_of(buf, pos);
        view_set_mark(v, c, buffer_line_start(buf, line), 1);
        pos = line + 1 < buffer_line_count(buf) ? buffer_line_start(buf, line + 1) : buffer_size(buf);
    }
    view_set_point(v, c, pos);
    app->dragging = 1;
    app->drag_view = v;
    app->drag_anchor = c->mark_active ? buffer_marker_get(buf, c->mark) : pos;
    view_ensure_visible(v);
    app->ctx.last_command = NULL;
}

// While the left button is held: point follows the mouse, the region runs from where it went down.
static void app_drag(App *app, AppLayout *l, i32 x, i32 y) {
    View *v = app->drag_view;
    if (v == app->mini.view && !app->mini.active) return; // the prompt ended during the drag
    Cursor *c = &v->cursors[0];
    i64 pos = app_mouse_pos(l, v, x, y);
    if (pos != view_point(v, c) || c->mark_active) {
        if (!c->mark_active && pos != app->drag_anchor) view_set_mark(v, c, app->drag_anchor, 1);
        view_set_point(v, c, pos);
        view_ensure_visible(v);
    }
}

extern const Command CMD_SAVE_BUFFERS_KILL_TERMINAL, CMD_TEXT_SCALE_INCREASE, CMD_TEXT_SCALE_DECREASE;
static void app_run_command(App *app, const Command *cmd, u32 codepoint, b32 shift_translated);

// The wheel scrolls the view under the mouse; point is dragged along to stay visible. With Ctrl it
// changes the text scale, one step per notch.
static void app_wheel(App *app, i32 x, i32 y, i32 wheel, u32 mods) {
    if (mods & MOD_CTRL) {
        app->wheel_scale_accum += wheel;
        for (; app->wheel_scale_accum >= 120; app->wheel_scale_accum -= 120) app_run_command(app, &CMD_TEXT_SCALE_INCREASE, 0, 0);
        for (; app->wheel_scale_accum <= -120; app->wheel_scale_accum += 120) app_run_command(app, &CMD_TEXT_SCALE_DECREASE, 0, 0);
        return;
    }
    app->wheel_accum += wheel * APP_WHEEL_LINES;
    i32 lines = app->wheel_accum / 120;
    app->wheel_accum -= lines * 120;
    i32 i = app_view_at(app, x, y);
    if (!lines || i < 0) return;
    view_scroll_lines(app->views[i], -lines); // positive = away from the user = towards the top
    view_ensure_visible(app->views[i]);
    app->ctx.last_command = NULL;
}

// "<buffer name> - teal" for the active view; the platform is called only when it changes.
static void app_update_title(App *app, Arena *scratch) {
    String8 title = str8_fmt(scratch, "%S - teal", app->views[app->active_view]->buffer->name);
    title.len = MIN(title.len, (i64)sizeof(app->title));
    if (str8_equal(title, str8(app->title, app->title_len))) return;
    memcpy(app->title, title.data, (size_t)title.len);
    app->title_len = (i32)title.len;
    os_set_window_title(title);
}

// ---------------------------------------------------------------------------
// App commands (COMMAND_ONCE)

// Reads the config (force: even if it looks unchanged) and applies it. A file that cannot be read
// leaves the config as it was.
static void app_reload_config(App *app, b32 force) {
    ConfigPoll result = app_poll_config(app, force, 0);
    if (result == CONFIG_POLL_UNCHANGED) return;
    if (result == CONFIG_POLL_LOADED) app_apply_config(app, app->renderer, 0);
    app_report_config(app, result, 1);
}

// Commands that show another buffer in ctx->view refuse to do it in the minibuffer.
static b32 app_can_switch(CommandContext *ctx) {
    if (ctx->view != ctx->app->mini.view) return 1;
    echo_message(ctx->echo, "Cannot switch buffers in minibuffer window");
    return 0;
}

static void app_cycle_buffer(CommandContext *ctx, i32 dir) {
    App *app = ctx->app;
    if (!app_can_switch(ctx)) return;
    i32 n = app->buffers.count;
    i32 i = buffer_list_index(&app->buffers, ctx->view->buffer);
    if (n < 2 || i < 0) return;
    view_switch_buffer(ctx->view, &app->buffers, app->buffers.entries[((i + dir) % n + n) % n].buffer);
    ctx->cursor = &ctx->view->cursors[0];
}

static void cmd_next_buffer(CommandContext *ctx) { app_cycle_buffer(ctx, 1); }
static void cmd_previous_buffer(CommandContext *ctx) { app_cycle_buffer(ctx, -1); }

// Visits the user's teal.conf, creating it from the built-in defaults when it does not exist.
static void cmd_open_config(CommandContext *ctx) {
    App *app = ctx->app;
    if (!app_can_switch(ctx)) return;
    String8 path = app->config_path;
    if (!path.len) {
        echo_message(ctx->echo, "No place for teal.conf: APPDATA is not set");
        return;
    }
    OsFileInfo info;
    b32 created = 0;
    if (os_file_info(path, &info) == OS_FILE_NOT_FOUND) {
        String8 dir = str8(path.data, path.len - config_file_name(path).len - 1);
        if (!os_make_dir(dir) || !os_write_file(path, config_default_text())) {
            echo_message(ctx->echo, "Cannot create %S", path);
            return;
        }
        created = 1;
        app_watch_config(app); // the directory may be new
    }
    Buffer *buf = app_find_file(app, path);
    if (!buf) return;
    view_switch_buffer(ctx->view, &app->buffers, buf);
    ctx->cursor = &ctx->view->cursors[0];
    if (created) echo_message(ctx->echo, "Created %S from the built-in defaults", path);
}

static void cmd_reload_config(CommandContext *ctx) {
    app_reload_config(ctx->app, 1);
}

// step 0 resets. 1.2 times per step (Emacs' text-scale-mode-step); the size stays within 4..96 pt.
static void app_text_scale(CommandContext *ctx, i32 step) {
    App *app = ctx->app;
    if (!app->font) return;
    i32 old = app->text_scale;
    f32 old_size = app_font_params(app).size_pt;
    app->text_scale = step ? app->text_scale + step : 0;
    FontParams fp = app_font_params(app);
    if (step && fp.size_pt == old_size) {
        app->text_scale = old;
        echo_message(ctx->echo, "Text size limit reached");
        return;
    }
    font_reconfigure(app->font, app->renderer, &fp);
    i32 pt = (i32)(fp.size_pt + 0.5f);
    if (app->text_scale) echo_message(ctx->echo, "Text scale %s%d (%d pt)", app->text_scale > 0 ? "+" : "", app->text_scale, pt);
    else echo_message(ctx->echo, "Text scale reset (%d pt)", pt);
}

static void cmd_text_scale_increase(CommandContext *ctx) { app_text_scale(ctx, 1); }
static void cmd_text_scale_decrease(CommandContext *ctx) { app_text_scale(ctx, -1); }
static void cmd_text_scale_reset(CommandContext *ctx) { app_text_scale(ctx, 0); }

// Reads one key sequence and describes it instead of running it (the keymap's describe state).
static void cmd_describe_key(CommandContext *ctx) {
    ctx->app->keys.describe = 1;
    echo_set(ctx->echo, STR8_LIT("Describe key: "));
}

// C-q: the next key is inserted literally (the keymap's quoted state).
static void cmd_quoted_insert(CommandContext *ctx) {
    ctx->app->keys.quoted = 1;
    echo_set(ctx->echo, STR8_LIT("C-q-"));
}

// set-language: the language of the current buffer (its highlighting, indentation), from a list.
static i64 app_language_candidates(Minibuffer *mb, void *data, String8 input) {
    (void)data;
    (void)input;
    if (mb->cand_count) return 0;
    for (i32 l = BUFFER_LANG_FUNDAMENTAL; l <= BUFFER_LANG_TYPESCRIPT; l++) {
        minibuffer_add_candidate(mb, str8_cstr(buffer_language_name((BufferLanguage)l)), str8(NULL, 0), 0);
    }
    return 0;
}

static void app_set_language_done(CommandContext *ctx, MiniResult *r) {
    Buffer *buf = ctx->view->buffer;
    for (i32 l = BUFFER_LANG_FUNDAMENTAL; l <= BUFFER_LANG_TYPESCRIPT; l++) {
        if (!str8_equal(r->text, str8_cstr(buffer_language_name((BufferLanguage)l)))) continue;
        buf->language = (BufferLanguage)l;
        syntax_attach(buf); // its states start over (or go, for Fundamental)
        echo_message(ctx->echo, "Language: %s", buffer_language_name(buf->language));
        return;
    }
}

static void cmd_set_language(CommandContext *ctx) {
    MiniRequest req = { .kind = MINI_CHOICE, .prompt = STR8_LIT("Language: "), .history = MINI_HISTORY_TEXT,
                        .candidates = app_language_candidates, .require_match = 1, .done = app_set_language_done };
    minibuffer_read(ctx, &req);
}

const Command CMD_SET_LANGUAGE               = { "set-language", cmd_set_language, COMMAND_ONCE };
const Command CMD_NEXT_BUFFER                = { "next-buffer", cmd_next_buffer, COMMAND_ONCE };
const Command CMD_PREVIOUS_BUFFER            = { "previous-buffer", cmd_previous_buffer, COMMAND_ONCE };
const Command CMD_OPEN_CONFIG                = { "open-config", cmd_open_config, COMMAND_ONCE };
const Command CMD_RELOAD_CONFIG              = { "reload-config", cmd_reload_config, COMMAND_ONCE };
const Command CMD_TEXT_SCALE_INCREASE        = { "text-scale-increase", cmd_text_scale_increase, COMMAND_ONCE };
const Command CMD_TEXT_SCALE_DECREASE        = { "text-scale-decrease", cmd_text_scale_decrease, COMMAND_ONCE };
const Command CMD_TEXT_SCALE_RESET           = { "text-scale-reset", cmd_text_scale_reset, COMMAND_ONCE };
const Command CMD_DESCRIBE_KEY               = { "describe-key", cmd_describe_key, COMMAND_ONCE };
const Command CMD_QUOTED_INSERT              = { "quoted-insert", cmd_quoted_insert, COMMAND_ONCE };

// Commands run in the minibuffer while it is active, otherwise in the active view.
static void app_run_command(App *app, const Command *cmd, u32 codepoint, b32 shift_translated) {
    app->ctx.view = app->mini.active ? app->mini.view : app->views[app->active_view];
    app->ctx.codepoint = codepoint;
    app->ctx.shift_translated = shift_translated;
    view_run_command(&app->ctx, cmd);
}

// While an isearch is active: a command of its own keymap runs inside it, a plain character extends
// the string; any other key ends the search at the match first (a global prefix at once, so "C-x-"
// shows), then goes on as usual. True when the key was taken.
static b32 app_isearch_key(App *app, KeyResult *k) {
    switch (k->kind) {
    case KEY_RESULT_IGNORED:
    case KEY_RESULT_DROPPED:
        return 1;
    case KEY_RESULT_PREFIX:
        if (keymap_has_prefix(&app->config->isearch, &k->seq)) return 1; // a sequence of the isearch keymap
        break;
    case KEY_RESULT_SELF_INSERT:
        echo_clear(&app->echo);
        app_run_command(app, &CMD_ISEARCH_PRINTING_CHAR, k->codepoint, 0);
        return 1;
    case KEY_RESULT_COMMAND:
        if (k->command == &CMD_SELF_INSERT && k->codepoint) {
            echo_clear(&app->echo);
            app_run_command(app, &CMD_ISEARCH_PRINTING_CHAR, k->codepoint, 0);
            return 1;
        }
        if (k->command->flags & COMMAND_ISEARCH) {
            echo_clear(&app->echo);
            app_run_command(app, k->command, k->codepoint, k->shift_translated);
            return 1;
        }
        break;
    default:
        break;
    }
    echo_clear(&app->echo);
    isearch_exit(&app->ctx);
    return 0;
}

// A key as an answer to query-replace's question ('y', ' ', 'n', 0x7f for DEL, '!', 'q', '\r' for RET,
// '.'; a capital letter as its small one), else 0.
static u32 app_replace_answer(KeyResult *k) {
    if (k->kind == KEY_RESULT_PREFIX || k->seq.len != 1) return 0;
    KeyChord c = k->seq.chords[0];
    if (c & (CHORD_CTRL | CHORD_META)) return 0;
    u32 code = c & CHORD_CODE_MASK;
    if (c & CHORD_NAMED) return (c & CHORD_SHIFT) ? 0 : code == KEY_ENTER ? '\r' : code == KEY_BACKSPACE ? 0x7f : 0;
    u32 a = unicode_lower(code);
    return a == 'y' || a == 'n' || a == 'q' || a == ' ' || a == '!' || a == '.' ? a : 0;
}

#if TEAL_DEV
// --log-keys: what the keymap made of a key or text event, numbered as the platform's line for it.
static void app_dev_log_key(App *app, Event *e, KeyResult *k, b32 isearch, b32 replacing) {
    u8 ev[64], seq[KEY_SEQ_TEXT_CAP];
    i64 en = 0;
    u8 mods[8];
    i32 mn = 0;
    if (e->mods & MOD_CTRL) mods[mn++] = 'C';
    if (e->mods & MOD_ALT) mods[mn++] = 'M';
    if (e->mods & MOD_SHIFT) mods[mn++] = 'S';
    if (!mn) mods[mn++] = '-';
    if (e->kind == EVENT_KEY_DOWN) {
        u8 name[16];
        i64 nn = 0;
        if (e->key >= KEY_A && e->key <= KEY_Z) name[nn++] = (u8)('a' + (e->key - KEY_A));
        else if (e->key >= KEY_0 && e->key <= KEY_9) name[nn++] = (u8)('0' + (e->key - KEY_0));
        else if (key_is_named(e->key)) key_put(name, sizeof(name), &nn, str8_cstr(key_names[e->key]));
        else nn = fmt_buf(name, sizeof(name), "key %d", (i32)e->key);
        en = fmt_buf(ev, sizeof(ev), "KEY_DOWN %S mods %S char U+%04x", str8(name, nn), str8(mods, mn), e->codepoint);
    } else {
        en = fmt_buf(ev, sizeof(ev), "TEXT U+%04x mods %S", e->codepoint, str8(mods, mn));
    }
    i64 sn = key_seq_print(&k->seq, seq, sizeof(seq));
    const char *where = app->mini.active ? " [minibuffer]" : replacing ? " [query-replace]" : isearch ? " [isearch]" : "";
    switch (k->kind) {
    case KEY_RESULT_IGNORED:
        LOG("keys: app #%u: %S%s -> no chord (%s)", e->dev_seq, str8(ev, en), where,
            e->kind != EVENT_KEY_DOWN ? "not a key event"
            : !(e->mods & (MOD_CTRL | MOD_ALT)) ? "no Ctrl or Alt: its text event, if any, is the chord"
            : "Ctrl or Alt held but the key gives no character");
        break;
    case KEY_RESULT_DROPPED:
        LOG("keys: app #%u: %S%s -> dropped (the text of a key that already was a chord)", e->dev_seq, str8(ev, en), where);
        break;
    case KEY_RESULT_PREFIX:
        LOG("keys: app #%u: %S%s -> chord sequence %S: a prefix, waiting", e->dev_seq, str8(ev, en), where, str8(seq, sn));
        break;
    case KEY_RESULT_COMMAND:
    case KEY_RESULT_SELF_INSERT:
    case KEY_RESULT_QUIT:
        LOG("keys: app #%u: %S%s -> %S runs %s%s", e->dev_seq, str8(ev, en), where, str8(seq, sn), k->command->name,
            k->shift_translated ? " (shift-translated)" : "");
        break;
    case KEY_RESULT_UNDEFINED:
        LOG("keys: app #%u: %S%s -> %S is undefined", e->dev_seq, str8(ev, en), where, str8(seq, sn));
        break;
    case KEY_RESULT_DESCRIBE:
        LOG("keys: app #%u: %S%s -> describe-key: %S %s %s", e->dev_seq, str8(ev, en), where, str8(seq, sn),
            k->command ? "runs" : "is", k->command ? k->command->name : "undefined");
        break;
    case KEY_RESULT_QUOTED:
        LOG("keys: app #%u: %S%s -> quoted-insert %S: U+%04x", e->dev_seq, str8(ev, en), where, str8(seq, sn), k->codepoint);
        break;
    }
}

void app_dev_log_keys(App *app, b32 on) {
    app->dev_log_keys = on;
}

String8 app_dev_echo(App *app) {
    return str8(app->echo.text, app->echo.len);
}

String8 app_dev_text(App *app, Arena *arena) {
    Buffer *buf = app->views[app->active_view]->buffer;
    return buffer_text(buf, arena, 0, buffer_size(buf));
}

String8 app_dev_binding(App *app, Arena *arena, i32 keymap, i32 index, const char **command) {
    Keymap *map = keymap == 0 ? &app->config->global : keymap == 1 ? &app->config->minibuffer : &app->config->isearch;
    if (index < 0 || index >= map->count) return str8(NULL, 0);
    *command = map->bindings[index].command->name;
    u8 *text = PUSH_ARRAY(arena, u8, KEY_SEQ_TEXT_CAP);
    return str8(text, key_seq_print(&map->bindings[index].seq, text, KEY_SEQ_TEXT_CAP));
}
#endif

// A KEY_DOWN or text event through the keymap stack: [minibuffer, global] while the minibuffer
// is active, [isearch, global] while an isearch is, else [global]. A single-key prompt takes the key
// itself, after the keymap has said what the key is bound to, so the quit keys still abort it.
static void app_key_event(App *app, Event *e) {
    Keymap *stack[2];
    i32 count = 0;
    b32 isearch = app->isearch.active && !app->mini.active;
    b32 replacing = app->replace.state != REPLACE_OFF && !app->mini.active;
    if (app->mini.active) stack[count++] = &app->config->minibuffer;
    else if (isearch) stack[count++] = &app->config->isearch;
    stack[count++] = &app->config->global;
    KeyResult k;
    key_input_feed(&app->keys, stack, count, e, &k);
#if TEAL_DEV
    if (app->dev_log_keys) app_dev_log_key(app, e, &k, isearch, replacing);
#endif
    b32 ended = 0; // the key ended a search session: its message ("Mark saved ...") stays while the key runs
    if (replacing) {
        if (k.kind == KEY_RESULT_IGNORED || k.kind == KEY_RESULT_DROPPED) return;
        echo_clear(&app->echo);
        if (replace_key(&app->ctx, k.kind == KEY_RESULT_PREFIX ? NULL : k.command, app_replace_answer(&k))) {
            app->keys.pending.len = 0; // a key ignored while the session searches leaves no prefix behind
            return;
        }
        ended = 1;
    }
    if (isearch) {
        if (app_isearch_key(app, &k)) return;
        ended = 1;
    }
    if (app->mini.active && app->mini.kind == MINI_KEY) {
        if (k.kind == KEY_RESULT_IGNORED || k.kind == KEY_RESULT_DROPPED) return;
        app->keys.pending.len = 0;
        app->keys.describe = app->keys.quoted = 0;
        KeyChord last = k.seq.chords[k.seq.len - 1];
        u32 ch = (last & (CHORD_NAMED | CHORD_CTRL | CHORD_META)) ? 0 : (last & CHORD_CODE_MASK);
        echo_clear(&app->echo);
        app->ctx.view = app->mini.view;
        minibuffer_key(&app->ctx, k.kind == KEY_RESULT_PREFIX ? NULL : k.command, ch);
        return;
    }
    u8 seq[KEY_SEQ_TEXT_CAP];
    i64 n = key_seq_print(&k.seq, seq, sizeof(seq) - 1);
    switch (k.kind) {
    case KEY_RESULT_IGNORED:
    case KEY_RESULT_DROPPED:
        break;
    case KEY_RESULT_PREFIX: // shown at once, as "C-x-"
        seq[n++] = '-';
        echo_set(&app->echo, str8(seq, n));
        break;
    case KEY_RESULT_COMMAND:
    case KEY_RESULT_SELF_INSERT:
    case KEY_RESULT_QUIT:
        if (!ended) echo_clear(&app->echo); // a message stays until the next key
        app_run_command(app, k.command, k.codepoint, k.shift_translated);
        break;
    case KEY_RESULT_UNDEFINED:
        echo_message(&app->echo, "%S is undefined", str8(seq, n));
        app->ctx.last_command = NULL;
        break;
    case KEY_RESULT_QUOTED:
        echo_clear(&app->echo);
        if (k.codepoint) app_run_command(app, &CMD_SELF_INSERT, k.codepoint, 0);
        else echo_message(&app->echo, "%S cannot be inserted", str8(seq, n));
        break;
    case KEY_RESULT_DESCRIBE:
        if (k.command) echo_message(&app->echo, "%S runs the command %s", str8(seq, n), k.command->name);
        else echo_message(&app->echo, "%S is undefined", str8(seq, n));
        app->ctx.last_command = NULL;
        break;
    }
}

// Search work within the frame's deadline, one slice at a time (a frame overruns it by at most one
// slice). It comes first: the user is waiting for the match.
static void app_search_work(App *app, Arena *scratch, u64 deadline) {
    i64 used = 0;
    while (isearch_pending(&app->isearch) || replace_pending(&app->replace)) {
        i64 slice = SEARCH_SLICE_BYTES;
#if TEAL_DEV
        if (app->dev_work_budget) { // tests: a fixed number of positions per frame instead of the clock
            if (used >= app->dev_work_budget) break;
            slice = MIN(slice, app->dev_work_budget - used);
        } else
#endif
        if (used && os_time_us() >= deadline) break;
        used += isearch_pending(&app->isearch) ? isearch_work(&app->isearch, slice) : replace_work(&app->replace, scratch, slice);
    }
    app->search_pending = isearch_pending(&app->isearch) || replace_pending(&app->replace);
}

// Lexer states for the visible lines of every view, with what is left of the frame's deadline. While
// some view still needs states, another frame is requested; once they are there, nothing runs.
static void app_catch_up(App *app, Arena *scratch, u64 end) {
    b32 pending = 0;
    for (i32 i = 0; i < app->view_count; i++) {
        View *v = app->views[i];
        u64 now = os_time_us();
        u64 budget = now < end ? end - now : 1;
        if (!syntax_catch_up(v->buffer, view_top_line(v) + v->rows, budget, scratch)) pending = 1;
    }
    app->syntax_pending = pending;
}

b32 app_wants_frame(App *app) {
    return app->syntax_pending || app->search_pending;
}

u32 app_wait_ms(App *app) {
    return MIN(config_wait_ms(&app->config_source, os_time_us()), files_wait_ms(app));
}

// Events, commands and layout: everything but drawing (no font or renderer calls, so a headless app
// runs it too). False = quit.
static b32 app_update(App *app, FrameInput *in) {
    app->ctx.scratch = in->scratch;
    AppLayout l = app_layout(app, in);
    app_layout_views(app, in, &l);
    // Fit the views again before the events only when the size or the font changed (a click maps
    // through the scroll position); every frame does it after the events anyway.
    b32 relaid = in->width != app->laid_w || in->height != app->laid_h || l.cell_w != app->laid_cell_w || l.line_h != app->laid_line_h;
    if (app->initial_line >= 0) { // the first frame: the layout is known now
        view_goto_line_column(app->views[0], app->initial_line, app->initial_col);
        app->initial_line = -1;
        relaid = 1;
    }
    if (relaid) for (i32 i = 0; i < app->view_count; i++) view_ensure_visible(app->views[i]);

    for (i32 i = 0; i < in->event_count; i++) {
        Event *e = &in->events[i];
        switch (e->kind) {
        case EVENT_CLOSE: // the window's close button, Alt+F4: as C-x C-c, after ending any prompt
            isearch_exit(&app->ctx);
            replace_finish(&app->replace, app->replace.state == REPLACE_ALL ? REPLACE_END_STOPPED : REPLACE_END_DONE);
            minibuffer_abort(&app->mini);
            app_run_command(app, &CMD_SAVE_BUFFERS_KILL_TERMINAL, 0, 0);
            break;
        case EVENT_END_SESSION:
            files_end_session(app);
            break;
        case EVENT_FOCUS: // activation: every file buffer is checked against its file
            if (e->focused && !app->focused) files_check_all(app);
            app->focused = e->focused;
            break;
        case EVENT_KEY_DOWN:
        case EVENT_TEXT:
            app_key_event(app, e);
            break;
        case EVENT_MOUSE_DOWN:
            if (e->button == MOUSE_LEFT) app_click(app, &l, e->x, e->y, MAX(e->clicks, 1));
            break;
        case EVENT_MOUSE_MOVE:
            if (app->dragging) app_drag(app, &l, e->x, e->y);
            break;
        case EVENT_MOUSE_UP:
            if (e->button == MOUSE_LEFT) app->dragging = 0;
            break;
        case EVENT_MOUSE_WHEEL:
            app_wheel(app, e->x, e->y, e->wheel, e->mods);
            break;
        case EVENT_DIR_CHANGED:
            if (e->watch == app->config_watch) config_notify(&app->config_source, os_time_us());
            else if (app->focused) files_notify(app, e->watch); // without focus, the activation check covers it
            break;
        default:
            break;
        }
    }
    if (config_pending(&app->config_source)) app_reload_config(app, 0); // a settle delay or retry may be due (EVENT_WAKEUP)
    files_poll(app);
    if (app->quit) return 0;
    // One deadline for the background work of this frame: search slices, then lexer states.
    u64 deadline = os_time_us() + SYNTAX_FRAME_BUDGET_US;
    app_search_work(app, in->scratch, deadline);
    // A command may have changed the font (text scale, config): lay out again.
    l = app_layout(app, in);
    app_layout_views(app, in, &l);
    for (i32 i = 0; i < app->view_count; i++) view_ensure_visible(app->views[i]); // also views showing a buffer edited elsewhere
    if (app->mini.active) view_ensure_visible(app->mini.view);
    app->laid_w = in->width;
    app->laid_h = in->height;
    app->laid_cell_w = l.cell_w;
    app->laid_line_h = l.line_h;
    app_catch_up(app, in->scratch, deadline);
    app_update_title(app, in->scratch);
    files_update_watches(app);
    files_report_unsaved(app);
    return 1;
}

static void app_render(App *app, FrameInput *in, Renderer *r) {
    AppLayout l = app_layout(app, in);
    r_begin_frame(r, COLOR_HEX(app->config->theme.background));
    // While the minibuffer is active the calling view's cursor is hollow; a candidate list covers the
    // bottom of the views.
    i32 list_rows = app_list_rows(app, &l);
    i32 bottom = l.minibuffer_y - list_rows * l.line_h;
    for (i32 i = 0; i < app->view_count; i++) {
        app_draw_view(app, r, &l, in, i, i == app->active_view && !app->mini.active, bottom);
    }
    if (app->mini.active) {
        if (list_rows) app_draw_candidates(app, r, &l, in, list_rows);
        app_draw_minibuffer(app, r, &l, in);
    } else if (app->isearch.active) {
        app_draw_isearch_line(app, r, &l, in);
    } else if (app->replace.state != REPLACE_OFF) {
        app_draw_search_line(app, r, &l, in, replace_prompt(&app->replace, in->scratch), str8(NULL, 0), 0, str8(NULL, 0));
    } else {
        String8 echo = app_clip_cells(str8(app->echo.text, app->echo.len), MAX((in->width - l.pad) / l.cell_w, 0));
        font_draw_text(app->font, r, l.pad, l.minibuffer_y, echo, COLOR_HEX(app->config->theme.text));
    }
}

b32 app_update_and_render(App *app, FrameInput *in, Renderer *r) {
#if TEAL_DEV
    u64 t0 = os_time_us();
#endif
    app->renderer = r;
    font_frame_begin(app->font, r, in->dpi_scale);
    b32 running = app_update(app, in);
    APP_STAGE("frame: updated (events, layout, catch-up)");
    app->renderer = NULL;
    if (!running) return 0;
    app_render(app, in, r);
#if TEAL_DEV
    app->dev_build_us = os_time_us() - t0;
    os_dev_stage("frame: built");
#endif
    r_end_frame(r);
    return 1;
}

#if TEAL_DEV
// Smoke, syntax frames: *scratch* holds a known text in `language`, point on the '(' of line 2, focus
// forced on (a filled cursor). The same text in every language:
//   line 0: "// a comment line"
//   line 1: "return \"some string text\";"   'return' at columns 0-5, the string at 7-24
//   line 2: "f(a, (b), c);"                  the '(' at column 1 matches the ')' at column 11
//   line 3: "    x_y = a | b;"               '_' at column 5, '|' at column 12 with spaces around it
//   line 4: "    z = 0;"                     column 1 is a space with spaces all around
//   line 5: "    w = 1;"
void app_dev_smoke_syntax(App *app, i32 language) {
    View *v = app->views[0];
    Buffer *buf = v->buffer;
    buffer_replace(buf, 0, buffer_size(buf), STR8_LIT("// a comment line\nreturn \"some string text\";\nf(a, (b), c);\n"
                                                      "    x_y = a | b;\n    z = 0;\n    w = 1;\n"));
    buf->language = (BufferLanguage)language;
    view_set_point(v, &v->cursors[0], buffer_line_start(buf, 2) + 1);
    buffer_marker_set(buf, v->top, 0);
    app->force_focus = 1;
}

// The probes of a syntax frame: a comment, a keyword and a string pixel in their exact colors, the
// match of the bracket at point on the paren_match background. With `rendering`, also the
// rendering checks: background, mode line, cursor, a text cell, a space cell, '_' inked only in its
// lower part (catches upside-down bitmaps), the ClearType channel order on '|'.
i32 app_dev_syntax_probes(App *app, FrameInput *in, DevProbe *out, i32 cap, b32 rendering) {
    AppLayout l = app_layout(app, in);
    Theme *th = &app->config->theme;
    i32 n = 0;
    i32 cw = l.cell_w, lh = l.line_h, x = l.pad;
#define APP_PUSH_PROBE(...) do { if (n < cap) out[n++] = (DevProbe){ __VA_ARGS__ }; } while (0)
#define CELLS(c0, c1, line) .x0 = x + (c0) * cw, .y0 = (line) * lh, .x1 = x + (c1) * cw, .y1 = ((line) + 1) * lh
    APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_HAS, CELLS(0, 17, 0), .rgb = th->comment, .what = "syntax: a comment pixel");
    APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_HAS, CELLS(0, 6, 1), .rgb = th->keyword, .what = "syntax: a keyword pixel ('return')");
    APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_HAS, CELLS(7, 25, 1), .rgb = th->string, .what = "syntax: a string pixel");
    APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_HAS, CELLS(11, 12, 2), .rgb = th->paren_match,
                   .what = "syntax: the matching ')' on the paren_match background");
    if (rendering) {
        i32 mode_y = l.minibuffer_y - lh;
        APP_PUSH_PROBE(.kind = DEV_PROBE_PIXEL_EQ, .x0 = in->width - 1, .y0 = in->height - 1, .rgb = th->background,
                       .what = "background (bottom-right corner)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_PIXEL_EQ, .x0 = in->width - 1, .y0 = mode_y + lh / 2, .rgb = th->text, .what = "mode line (right end)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_PIXEL_EQ, .x0 = x + cw, .y0 = 2 * lh, .rgb = th->cursor, .what = "cursor (top-left pixel)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, CELLS(0, 1, 0), .rgb = th->background, .what = "a text cell ('/')");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, CELLS(1, 2, 4), .rgb = th->background, .what = "a space cell");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x + 5 * cw, .y0 = 3 * lh, .x1 = x + 6 * cw, .y1 = 3 * lh + lh * 2 / 5,
                       .rgb = th->background, .what = "'_' upper part");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, .x0 = x + 5 * cw, .y0 = 3 * lh + lh / 2, .x1 = x + 6 * cw, .y1 = 4 * lh,
                       .rgb = th->background, .what = "'_' lower part");
        APP_PUSH_PROBE(.kind = DEV_PROBE_CLEARTYPE, .x0 = x + 12 * cw - 1, .y0 = 3 * lh, .x1 = x + 13 * cw + 1, .y1 = 4 * lh,
                       .rgb = th->background, .text_rgb = th->text, .geometry = app->font->backend.pixel_geometry,
                       .what = "ClearType '|' (text color)");
    }
#undef CELLS
#undef APP_PUSH_PROBE
    return n;
}

// Smoke, buffer frames: a known buffer in the buffer view.
//   line 0: "int x = 1;"
//   line 1: TAB "|"   columns 0-3 empty, '|' (centered, so no ClearType fringe reaches column 3) at 4
//   line 2: ""        column 5 is empty, and so are its neighbors above, below and to the sides
//   line 3: "a" ^A "b"  the control character takes columns 1-2, 'b' is at 3
// Stage 0: point on the empty line 2, no focus: a hollow cursor there. Stage 1 (after the
// platform clicks the 'x' of line 0 and forces focus): a filled cursor on that 'x'.
void app_dev_smoke_buffer_view(App *app) {
    Buffer *buf = app->views[0]->buffer;
    buf->language = BUFFER_LANG_FUNDAMENTAL;
    buffer_replace(buf, 0, buffer_size(buf), STR8_LIT("int x = 1;\n\t|\n\na\x01" "b\n"));
    View *v = app->views[0];
    view_set_point(v, &v->cursors[0], buffer_line_start(buf, 2));
    buffer_marker_set(buf, v->top, 0);
    app->force_focus = 0;
}

void app_dev_use_config(App *app, String8 path) {
    os_unwatch(app->config_watch);
    app->config_watch = 0;
    app->config_path = path;
    app->config_source = (ConfigSource){ .path = path };
    app_watch_config(app);
    app_reload_config(app, 1);
}

b32 app_dev_visit(App *app, String8 path) {
    Buffer *buf = app_find_file(app, path);
    if (buf) view_switch_buffer(app_active_view(app), &app->buffers, buf);
    return buf != NULL;
}

// --bench-complete: deterministic path-like names, "src/view/buffer_main_123.c" and the like.
static i64 app_dev_bench_candidates(Minibuffer *mb, void *data, String8 input) {
    (void)input;
    if (mb->cand_count) return 0;
    static const char *dirs[] = { "src", "lib", "tests", "tools", "docs", "build", "include", "third_party" };
    static const char *words[] = { "view", "buffer", "main", "config", "render", "font", "keymap", "edit", "file", "path",
                                   "window", "command", "minibuffer", "match", "list", "draw", "input", "event", "test", "util" };
    static const char *exts[] = { "c", "h", "cpp", "jai", "js", "ts", "md", "txt" };
    i64 n = (i64)(uintptr_t)data;
    u64 x = 0x9E3779B97F4A7C15ull;
    for (i64 i = 0; i < n; i++) {
        u8 name[128];
        u32 r[5];
        for (i32 k = 0; k < 5; k++) {
            x ^= x << 13, x ^= x >> 7, x ^= x << 17;
            r[k] = (u32)(x >> 32);
        }
        i64 len = fmt_buf(name, sizeof(name), "%s/%s/%s_%s_%D.%s", dirs[r[0] % 8], words[r[1] % 20], words[r[2] % 20],
                          words[r[3] % 20], i, exts[r[4] % 8]);
        minibuffer_add_candidate(mb, str8(name, len), str8(NULL, 0), 0);
    }
    return 0;
}

u64 app_dev_bench_complete_open(App *app, i64 count) {
    u64 t0 = os_time_us();
    minibuffer_abort(&app->mini);
    app->ctx.view = app->views[app->active_view];
    MiniRequest req = { .kind = MINI_CHOICE, .prompt = STR8_LIT("Bench: "), .candidates = app_dev_bench_candidates,
                        .data = (void *)(uintptr_t)count };
    minibuffer_read(&app->ctx, &req);
    return os_time_us() - t0;
}

void app_dev_filter_stats(App *app, u64 *filters, u64 *last_us, i64 *matches) {
    *filters = app->mini.dev_filters;
    *last_us = app->mini.dev_filter_us;
    *matches = app->mini.match_count;
}

String8 app_dev_prompt(App *app) {
    return app->mini.active ? app->mini.prompt : str8(NULL, 0);
}

i32 app_dev_font_setups(App *app) {
    return app->font->setup_count;
}

// Smoke stage 2: the region from line 0, column 5 to the start of line 2, active.
void app_dev_smoke_region(App *app) {
    View *v = app->views[0];
    Buffer *buf = v->buffer;
    view_set_mark(v, &v->cursors[0], 5, 1);
    view_set_point(v, &v->cursors[0], buffer_line_start(buf, 2));
}

// Smoke stage 4: "foo bar foo" from column 0, point 0, focus on; the smoke then types C-s f o o C-s, so
// the current match is the second foo (columns 8-10) and the first one (0-2) is lazily highlighted.
void app_dev_smoke_isearch(App *app) {
    View *v = app->views[0];
    Buffer *buf = v->buffer;
    buffer_replace(buf, 0, buffer_size(buf), STR8_LIT("foo bar foo\n"));
    view_deactivate_mark(v);
    view_set_point(v, &v->cursors[0], 0);
    buffer_marker_set(buf, v->top, 0);
    app->force_focus = 1;
}

// --bench-search: text appended to the active buffer, not undoable (as if loaded).
void app_dev_append(App *app, String8 text) {
    Buffer *buf = app_active_view(app)->buffer;
    buffer_undo_enable(buf, 0);
    buffer_replace(buf, buffer_size(buf), buffer_size(buf), text);
    buffer_undo_enable(buf, 1);
}

// --bench-search: *scratch* with `text` (not undoable) in the active view, point at the start.
void app_dev_show_scratch(App *app, String8 text) {
    Buffer *buf = buffer_list_find_name(&app->buffers, STR8_LIT("*scratch*"));
    View *v = app_active_view(app);
    view_switch_buffer(v, &app->buffers, buf);
    buffer_undo_enable(buf, 0);
    buffer_replace(buf, 0, buffer_size(buf), text);
    buffer_undo_enable(buf, 1);
    view_set_point(v, &v->cursors[0], 0);
    view_ensure_visible(v);
}

b32 app_dev_isearch_failing(App *app) {
    Isearch *is = &app->isearch;
    return is->active && !isearch_pending(is) && !isearch_top(is)->success;
}

i64 app_dev_point(App *app) {
    View *v = app_active_view(app);
    return view_point(v, &v->cursors[0]);
}

i64 app_dev_size(App *app) {
    return buffer_size(app_active_view(app)->buffer);
}

void app_dev_set_language(App *app, i32 language) {
    Buffer *buf = app_active_view(app)->buffer;
    buf->language = (BufferLanguage)language;
    syntax_attach(buf);
}

b32 app_dev_paren(App *app, Arena *scratch, i64 *a, i64 *b) {
    return app->config->settings.show_paren_mode && app_paren(app, app->active_view, scratch, a, b);
}

void app_dev_force_focus(App *app, i32 focused) {
    app->force_focus = focused;
}

// Where the smoke clicks: the center of the 'x' cell (line 0, column 4).
void app_dev_smoke_click_point(App *app, FrameInput *in, i32 *x, i32 *y) {
    AppLayout l = app_layout(app, in);
    *x = l.pad + 4 * l.cell_w + l.cell_w / 2;
    *y = l.line_h / 2;
}

i32 app_dev_buffer_probes(App *app, FrameInput *in, DevProbe *out, i32 cap, i32 stage) {
    AppLayout l = app_layout(app, in);
    i32 n = 0;
    i32 cw = l.cell_w, lh = l.line_h, x = l.pad;
    i32 mode_y = l.minibuffer_y - lh;
    i32 t = MAX((i32)(in->dpi_scale + 0.5f), 1); // the hollow cursor's line width
#define APP_PUSH_PROBE(...) do { if (n < cap) out[n++] = (DevProbe){ __VA_ARGS__ }; } while (0)
#define CELL(col, line) .x0 = x + (col) * cw, .y0 = (line) * lh, .x1 = x + ((col) + 1) * cw, .y1 = ((line) + 1) * lh
    if (stage == 0) {
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, CELL(0, 0), .rgb = app->config->theme.background,
                       .what = "buffer: text cell 'i' (line 0, column 0)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, CELL(5, 2), .rgb = app->config->theme.background,
                       .what = "buffer: empty cell (line 2, column 5)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x, .y0 = lh, .x1 = x + 4 * cw, .y1 = 2 * lh,
                       .rgb = app->config->theme.background, .what = "buffer: tab, columns 0-3 of line 1 empty");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, CELL(4, 1), .rgb = app->config->theme.background,
                       .what = "buffer: '|' after the tab drawn at column 4");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, CELL(2, 3), .rgb = app->config->theme.background,
                       .what = "buffer: 'A' of ^A drawn at column 2");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, CELL(3, 3), .rgb = app->config->theme.background,
                       .what = "buffer: 'b' after ^A drawn at column 3");
        // Hollow cursor on the empty line 2: edges in the cursor color, inside untouched.
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x, .y0 = 2 * lh, .x1 = x + t, .y1 = 3 * lh,
                       .rgb = app->config->theme.cursor, .what = "buffer: hollow cursor, left edge");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x, .y0 = 2 * lh, .x1 = x + cw, .y1 = 2 * lh + t,
                       .rgb = app->config->theme.cursor, .what = "buffer: hollow cursor, top edge");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x + t, .y0 = 2 * lh + t, .x1 = x + cw - t, .y1 = 3 * lh - t,
                       .rgb = app->config->theme.background, .what = "buffer: hollow cursor, inside");
        // Mode line: inverse video to the right end, the buffer name drawn, nothing after the text.
        String8 mode = app_mode_line_text(app->views[0], in->scratch);
        i32 cells = (i32)mode.len; // ASCII here
        APP_PUSH_PROBE(.kind = DEV_PROBE_PIXEL_EQ, .x0 = in->width - 1, .y0 = mode_y + lh / 2,
                       .rgb = app->config->theme.text, .what = "buffer: mode line (right end)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, .x0 = x + cw, .y0 = mode_y, .x1 = x + 2 * cw, .y1 = mode_y + lh,
                       .rgb = app->config->theme.text, .what = "buffer: mode line text ('-' in cell 1)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, .x0 = x + 8 * cw, .y0 = mode_y, .x1 = x + 17 * cw, .y1 = mode_y + lh,
                       .rgb = app->config->theme.text, .what = "buffer: mode line buffer name (cells 8-16)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x + (cells + 1) * cw, .y0 = mode_y, .x1 = in->width, .y1 = mode_y + lh,
                       .rgb = app->config->theme.text, .what = "buffer: mode line empty after its text");
    } else if (stage == 3) {
        // M-x "minib": eight rows above the minibuffer line, the first selected ("minibuffer-backward-updir",
        // no global binding, so no annotation), the second "minibuffer-complete". Point is still on the
        // empty line 2: a hollow cursor while the minibuffer reads.
        Theme *th = &app->config->theme;
        i32 y0 = l.minibuffer_y - 8 * lh, lx = in->width - 1;
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_HAS, .x0 = x, .y0 = l.minibuffer_y, .x1 = x + 3 * cw, .y1 = l.minibuffer_y + lh,
                       .rgb = th->prompt, .what = "minibuffer: the prompt 'M-x' in the prompt color");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x + 30 * cw, .y0 = y0, .x1 = lx, .y1 = y0 + lh, .rgb = th->completion_selection,
                       .what = "minibuffer: the selected row's empty part");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_HAS, .x0 = x, .y0 = y0 + lh, .x1 = x + 5 * cw, .y1 = y0 + 2 * lh,
                       .rgb = th->completion_match, .what = "minibuffer: the matched 'minib' of the second row");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x + 30 * cw, .y0 = y0 + lh, .x1 = lx, .y1 = y0 + 2 * lh, .rgb = th->background,
                       .what = "minibuffer: an unselected row's empty part");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x, .y0 = 2 * lh, .x1 = x + t, .y1 = 3 * lh, .rgb = th->cursor,
                       .what = "minibuffer: the calling view's hollow cursor, left edge");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x + t, .y0 = 2 * lh + t, .x1 = x + cw - t, .y1 = 3 * lh - t, .rgb = th->background,
                       .what = "minibuffer: the calling view's hollow cursor, inside");
    } else if (stage == 4) {
        // app_dev_smoke_isearch after C-s f o o C-s: the current match at columns 8-10, a lazy highlight at
        // 0-2, the spaces at 3 and 7 untouched (their middle thirds, away from the neighbors' fringes).
        Theme *th = &app->config->theme;
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_HAS, .x0 = x + 8 * cw, .y0 = 0, .x1 = x + 11 * cw, .y1 = lh, .rgb = th->isearch,
                       .what = "isearch: the current match's background");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_HAS, .x0 = x + 8 * cw, .y0 = 0, .x1 = x + 11 * cw, .y1 = lh, .rgb = th->isearch_text,
                       .what = "isearch: the current match's text in isearch_text");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_HAS, .x0 = x, .y0 = 0, .x1 = x + 3 * cw, .y1 = lh, .rgb = th->lazy_highlight,
                       .what = "isearch: the other match's lazy_highlight background");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x + 3 * cw + cw / 3, .y0 = 0, .x1 = x + 3 * cw + 2 * cw / 3, .y1 = lh,
                       .rgb = th->background, .what = "isearch: the space at column 3 is not highlighted");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x + 7 * cw + cw / 3, .y0 = 0, .x1 = x + 7 * cw + 2 * cw / 3, .y1 = lh,
                       .rgb = th->background, .what = "isearch: the space at column 7 is not highlighted");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_HAS, .x0 = x, .y0 = l.minibuffer_y, .x1 = x + 9 * cw, .y1 = l.minibuffer_y + lh,
                       .rgb = th->prompt, .what = "isearch: 'I-search:' in the prompt color");
    } else if (stage == 2) {
        // The region of app_dev_smoke_region: line 0 from column 5 with its newline, all of line 1.
        Theme *th = &app->config->theme;
        i32 lx = in->width - 1;
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x, .y0 = lh, .x1 = x + 3 * cw, .y1 = 2 * lh, .rgb = th->selection,
                       .what = "region: selected tab cells (line 1, columns 0-2)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, CELL(4, 1), .rgb = th->selection,
                       .what = "region: '|' drawn over the selection (line 1, column 4)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = lx, .y0 = 0, .x1 = lx + 1, .y1 = lh, .rgb = th->selection,
                       .what = "region: line 0's newline selected: the selection reaches the window edge");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = lx, .y0 = lh, .x1 = lx + 1, .y1 = 2 * lh, .rgb = th->selection,
                       .what = "region: line 1's newline selected: the selection reaches the window edge");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x + 2 * cw, .y0 = 2 * lh, .x1 = in->width, .y1 = 3 * lh, .rgb = th->background,
                       .what = "region: line 2 (after the region) is not selected, edge included");
        // The space at column 3 of line 0, its middle third (away from the neighbors' ClearType fringes).
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x + 3 * cw + cw / 3, .y0 = 0, .x1 = x + 3 * cw + 2 * cw / 3, .y1 = lh,
                       .rgb = th->background, .what = "region: the space at column 3 of line 0 (before the region) is not selected");
    } else {
        // Filled cursor on the clicked 'x' (line 0, column 4), the glyph in the background color.
        APP_PUSH_PROBE(.kind = DEV_PROBE_PIXEL_EQ, .x0 = x + 4 * cw, .y0 = 0, .rgb = app->config->theme.cursor,
                       .what = "buffer: filled cursor on 'x' (top-left pixel)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, CELL(4, 0), .rgb = app->config->theme.cursor,
                       .what = "buffer: filled cursor, 'x' drawn over it");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, CELL(0, 2), .rgb = app->config->theme.background,
                       .what = "buffer: the old cursor cell is background again");
    }
#undef CELL
#undef APP_PUSH_PROBE
    return n;
}

// A headless app (--test): events through app_update with a fixed 1280x800 window. False = quit.
b32 app_dev_feed_events(App *app, Event *events, i32 count, Arena *scratch) {
    FrameInput in = { .events = events, .event_count = count, .width = 1280, .height = 800, .dpi_scale = 1.0f, .scratch = scratch };
    u64 mark = arena_pos(scratch);
    b32 running = app_update(app, &in);
    arena_pop_to(scratch, mark);
    return running;
}

// --keys notation through a headless app, one event at a time. False = quit.
b32 app_dev_feed(App *app, const char *keys, Arena *scratch) {
    Event events[256];
    i32 n = app_dev_key_events(app, str8_cstr(keys), events, ARRAY_COUNT(events));
    for (i32 i = 0; i < n; i++) if (!app_dev_feed_events(app, &events[i], 1, scratch)) return 0;
    return 1;
}

i32 app_dev_key_events(App *app, String8 keys, Event *out, i32 cap) {
    (void)app;
    i32 n = 0;
    for (i64 i = 0; i < keys.len;) {
        while (i < keys.len && keys.data[i] == ' ') i++;
        i64 start = i;
        while (i < keys.len && keys.data[i] != ' ') i++;
        if (i == start) break;
        String8 token = str8(keys.data + start, i - start);
        Event e[2];
        const char *error = NULL;
        i32 k = key_dev_events(token, e, &error);
        if (!k) LOG("--keys: bad token '%S': %s", token, error);
        for (i32 j = 0; j < k && n < cap; j++) out[n++] = e[j];
    }
    return n;
}

// Point to the start of `line` (< 0: the last line), the window recentered on it if needed.
void app_dev_goto_line(App *app, i64 line) {
    View *v = app->views[0];
    view_goto_line_column(v, line < 0 ? I64_MAX : line, 0);
    view_ensure_visible(v);
}

i64 app_dev_line_count(App *app) {
    return buffer_line_count(app->views[0]->buffer);
}

u64 app_dev_build_us(App *app) {
    return app->dev_build_us;
}

AppDevMemory app_dev_memory(App *app) {
    Buffer *buf = app_active_view(app)->buffer;
    AppDevMemory m = { 0 };
    m.undo = buffer_undo_memory(buf);
    KillRing *k = &app->kills;
    m.kill_ring = k->small_committed;
    for (i32 i = 0; i < k->count; i++) {
        KillEntry *e = &k->entries[(k->head - i + KILL_RING_CAP) % KILL_RING_CAP];
        if (e->large) m.kill_ring += e->committed;
    }
    m.meta = buf->meta.committed;
    m.text = (u64)buf->text_cap;
    m.line_index = (u64)buf->nl_cap * sizeof(u32);
    m.markers = (u64)buf->marker_cap * sizeof(BufferMarkerSlot);
    m.undo_log = m.undo;
    m.states = buffer_states_memory(buf);
    m.buffer = m.meta + m.text + m.line_index + m.markers + m.undo_log + m.states;
    return m;
}

b32 app_dev_atlas_has_coverage(App *app) {
    return font_dev_atlas_has_coverage(app->font);
}

u8 *app_dev_atlas(App *app, i32 *size) {
    *size = app->font->atlas_size;
    return app->font->atlas;
}

// Fills every text cell with ASCII in rotating theme colors.
i32 app_dev_bench_frame(App *app, FrameInput *in, Renderer *r, u64 *build_us, u64 *submit_us) {
    Theme *th = &app->config->theme;
    u32 colors[] = { th->text, th->keyword, th->type, th->string, th->number, th->comment, th->variable };
    u64 t0 = os_time_us();
    font_frame_begin(app->font, r, in->dpi_scale);
    AppLayout l = app_layout(app, in);
    u8 *row_text = PUSH_ARRAY(in->scratch, u8, l.cols);
    r_begin_frame(r, COLOR_HEX(app->config->theme.background));
    for (i32 row = 0; row < l.rows; row++) {
        for (i32 col = 0; col < l.cols; col++) row_text[col] = (u8)(33 + (row * 7 + col) % 94);
        // Eight-character runs so colors change along the line, as in highlighted code.
        for (i32 col = 0; col < l.cols; col += 8) {
            i32 len = MIN(8, l.cols - col);
            Color color = COLOR_HEX(colors[(row + col / 8) % ARRAY_COUNT(colors)]);
            font_draw_text(app->font, r, col * l.cell_w, row * l.line_h, str8(row_text + col, len), color);
        }
    }
    u64 t1 = os_time_us();
    r_end_frame(r);
    u64 t2 = os_time_us();
    *build_us = t1 - t0;
    *submit_us = t2 - t1;
    return l.rows * l.cols;
}
#endif
