// app.c — the editor core: the config (loaded before the font), keys through the keymap,
// views laid out in the frame, drawing of text, cursors, mode lines and the echo area. View
// logic lives in view.c. Dev builds keep the Phase 2 hand-colored sample behind --sample for
// the smoke probes and screenshots (until highlighting in Phase 8).

#if TEAL_DEV
// Sample text markup: these bytes switch the color of what follows and take no cell.
#define P "\x01" // plain text
#define K "\x02" // keyword
#define T "\x03" // type
#define S "\x04" // string
#define N "\x05" // number / constant
#define C "\x06" // comment
#define V "\x07" // variable

static u32 app_markup_color(Theme *th, u8 markup) {
    u32 colors[8] = { 0, th->text, th->keyword, th->type, th->string, th->number, th->comment, th->variable };
    return colors[markup & 7];
}

static const char *app_sample[] = {
    C "// sample.jai: a small Jai program to judge the theme.",
    K "#import" P " " S "\"Basic\"" P ";",
    K "#import" P " " S "\"Math\"" P ";",
    "",
    T "Vector2" P " :: " K "struct" P " {",
    "    " V "x" P ": " T "float" P ";",
    "    " V "y" P ": " T "float" P ";",
    "}",
    "",
    N "MAX_ITEMS" P " :: " N "128" P ";",
    "",
    "length :: (" V "v" P ": " T "Vector2" P ") -> " T "float" P " {",
    "    " K "return" P " sqrt(v.x * v.x + v.y * v.y);   " C "// hypotenuse",
    "}",
    "",
    "main :: () {",
    "    " V "items" P ": [..] " T "Vector2" P ";",
    "    " K "for" P " " N "0" P ".." N "MAX_ITEMS" P "-" N "1" P " {",
    "        array_add(*items, .{" K "xx" P " it, " N "0.5" P "});",
    "    }",
    "    print(" S "\"% items, first = %\\n\"" P ", items.count, items[" N "0" P "]);",
    "}",
    "",
    C "/* sample.c: the same idea in C. */",
    K "#include" P " " S "<stdio.h>",
    K "typedef" P " " K "struct" P " { " T "float" P " x, y; } " T "Vec2" P ";",
    "",
    K "static" P " " T "int" P " count_nonzero(" K "const" P " " T "int" P " *v, " T "int" P " n) {",
    "    " T "int" P " " V "count" P " = " N "0" P ";",
    "    " K "for" P " (" T "int" P " i = " N "0" P "; i < n; i++) count += (v[i] != " N "0" P ") " K "|" P " (v[i] < " N "0" P ");",
    "    " K "return" P " count;",
    "}",
    "",
    T "int" P " main(" T "void" P ") { printf(" S "\"%d\\n\"" P ", " N "0x2A" P "); " K "return" P " " N "0" P "; }",
    "",
    C "// Türkçe:" P " ğüşıöç ĞÜŞİÖÇ",
    C "// Missing glyphs:" P " 漢 😀 (CJK, emoji)",
    C "// Clipping:" P " g j y Ğ İ | _ []{} @#$%",
};

#undef P
#undef K
#undef T
#undef S
#undef N
#undef C
#undef V
#endif // TEAL_DEV

#define APP_MAX_VIEWS 8
#define APP_WHEEL_LINES 3 // per notch (120 units)
#define APP_PAD_PX 4      // left padding of a text area at 96 DPI
#define APP_CONFIG_RESERVE MB(16)
#define APP_HEADLESS_CELL_W 8  // the cell of a headless app (dev: --test), which has no font
#define APP_HEADLESS_LINE_H 16

struct App {
    Font *font;
    BufferList buffers;          // every buffer, in creation order
    Buffer *messages;            // *Messages*: the echo area's log
    KillRing kills;              // one for every buffer
    View *views[APP_MAX_VIEWS];  // laid out side by side
    i32 view_count;
    i32 active_view;
    Echo echo;
    CommandContext ctx;          // keeps last_command between events
    KeyInput keys;               // the key sequence state
    Minibuffer mini;             // prompts; while active its keymap comes before the global one
    Arena files_arena;           // files.c: paths being built; reset by each use
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
#if TEAL_DEV
    b32 sample; // --sample: the Phase 2 display
    i32 cursor_col, cursor_row;
    i32 force_focus;  // -1: follow focus events; 0 / 1: forced (smoke, screenshots)
    u64 dev_build_us; // last frame: time from the start of the frame to r_end_frame
#endif
};

typedef struct AppLayout {
    i32 cell_w, line_h;
    i32 pad;             // left padding of text areas, pixels
    i32 cols, rows;      // whole-frame grid (the --sample display)
    i32 mode_line_y;     // the --sample mode line
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

// One line of the buffer, only its columns [left, left + cols). x0 is the x of column `left`.
static void app_draw_buffer_line(App *app, Renderer *r, Buffer *buf, i64 line, i32 x0, i32 y, i64 left, i64 cols, Arena *scratch) {
    i32 cell_w = app->font->cell_w;
    i64 right = left + cols;
    i64 end = buffer_line_end(buf, line);
    i64 col = 0;
    // The first character that reaches column `left` (a tab or ^X may start before it).
    i64 pos = view_walk(buf, buffer_line_start(buf, line), end, &col, left);
    // Every visible column needs at most 4 bytes, so a huge line is never copied whole.
    String8 s = buffer_text(buf, scratch, pos, MIN(end, pos + (cols + 1) * 4));
    i64 run = 0, run_col = col;
    i64 i = 0;
    while (i < s.len && col < right) {
        u8 b = s.data[i];
        if (b == '\t' || view_is_control(b)) {
            if (i > run) font_draw_text(app->font, r, x0 + (i32)(run_col - left) * cell_w, y, str8(s.data + run, i - run), COLOR_HEX(app->config->theme.text));
            if (b != '\t') app_draw_caret(app, r, x0, y, col, left, right, b, COLOR_HEX(app->config->theme.number));
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
    if (i > run) font_draw_text(app->font, r, x0 + (i32)(run_col - left) * cell_w, y, str8(s.data + run, i - run), COLOR_HEX(app->config->theme.text));
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
    i64 top_pos = buffer_marker_get(buf, v->top);
    i64 top = buffer_line_of(buf, top_pos);
    b32 bottom = top + v->rows >= buffer_line_count(buf);
    String8 where = top == 0 && bottom ? STR8_LIT("All") : top == 0 ? STR8_LIT("Top") : bottom ? STR8_LIT("Bot")
                  : str8_fmt(arena, "%D%%", top_pos * 100 / MAX(buffer_size(buf), 1));
    i64 p = view_point(v, &v->cursors[0]);
    return str8_fmt(arena, " -:%s-  %S%S    %S   L%D C%D    (%s)    %s %s", flags, buf->name, pad, where,
                    buffer_line_of(buf, p) + 1, view_column_of(buf, p), buffer_language_name(buf->language),
                    app_encoding_name(buf->encoding), app_eol_name(buf->eol));
}

// The selection color behind the cells of an active region on the visible rows; on a line whose
// newline is selected it extends to the right edge of the window. Text is drawn over it.
static void app_draw_region(App *app, Renderer *r, AppLayout *l, View *v, Cursor *c, i32 draw_rows) {
    i64 start, end;
    if (!view_region_active(v, c, &app->config->settings) || !view_region(v, c, &start, &end) || start == end) return;
    Buffer *buf = v->buffer;
    i64 top = view_top_line(v), count = buffer_line_count(buf);
    i32 x0 = v->x + l->pad, right = v->x + v->w;
    Color color = COLOR_HEX(app->config->theme.selection);
    for (i32 row = 0; row < draw_rows && top + row < count; row++) {
        i64 line = top + row, ls = buffer_line_start(buf, line), le = buffer_line_end(buf, line);
        if (end <= ls || start > le) continue;
        i64 a = MAX(start, ls), b = MIN(end, le);
        i64 ca = view_column_of(buf, a) - v->left_col, cb = view_column_of(buf, b) - v->left_col;
        b32 newline = end > le; // the line's newline is in the region
        f32 xa = (f32)(x0 + (i32)MAX(ca, 0) * l->cell_w);
        f32 xb = newline ? (f32)right : (f32)MIN(x0 + (i32)MAX(cb, 0) * l->cell_w, right);
        f32 y = (f32)(v->y + row * l->line_h);
        if (xb > xa) r_push_rect(r, (Rect){ xa, y, xb, y + (f32)l->line_h }, color);
    }
}

// `bottom`: the view is drawn above it (the candidate list covers the rest), its mode line moved up.
// Its rows and scroll position are not touched, so nothing scrolls when the list opens or closes.
static void app_draw_view(App *app, Renderer *r, AppLayout *l, FrameInput *in, View *v, b32 active, i32 bottom) {
    Buffer *buf = v->buffer;
    i32 line_h = l->line_h;
    i32 text_x = v->x + l->pad;
    i32 mode_y = MAX(MIN(v->y + v->h, bottom) - line_h, v->y);
    i32 rows = MIN(v->rows, (mode_y - v->y) / line_h);
    // Only the visible lines (the partial one above the mode line too, which covers it).
    i64 top = view_top_line(v), count = buffer_line_count(buf);
    i32 draw_rows = (mode_y - v->y + line_h - 1) / line_h;
    for (i32 k = 0; k < v->cursor_count; k++) app_draw_region(app, r, l, v, &v->cursors[k], draw_rows);
    for (i32 row = 0; row < draw_rows && top + row < count; row++) {
        app_draw_buffer_line(app, r, buf, top + row, text_x, v->y + row * line_h, v->left_col, v->cols, in->scratch);
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
    app_draw_buffer_line(app, r, v->buffer, line, text_x, v->y, v->left_col, v->cols, in->scratch);
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

#if TEAL_DEV
// ---------------------------------------------------------------------------
// Dev: the Phase 2 sample

static String8 app_sample_line(i32 row) {
    if (row < 0 || row >= ARRAY_COUNT(app_sample)) return str8(NULL, 0);
    return str8_cstr(app_sample[row]);
}

// The UTF-8 bytes of the character at `col` in a sample line (markup skipped), or empty.
static String8 app_char_at(i32 row, i32 col) {
    String8 line = app_sample_line(row);
    i32 c = 0;
    for (i64 i = 0; i < line.len;) {
        if (line.data[i] < 8) {
            i++;
            continue;
        }
        i64 advance;
        utf8_decode(line.data + i, line.len - i, &advance);
        if (c == col) return str8(line.data + i, advance);
        c++;
        i += advance;
    }
    return str8(NULL, 0);
}

// The markup in effect at the character at `col` (1 = plain text), or 0 if there is no character.
static u8 app_markup_at(i32 row, i32 col) {
    String8 line = app_sample_line(row);
    u8 color = 1;
    i32 c = 0;
    for (i64 i = 0; i < line.len;) {
        if (line.data[i] < 8) {
            color = line.data[i];
            i++;
            continue;
        }
        i64 advance;
        utf8_decode(line.data + i, line.len - i, &advance);
        if (c == col) return color;
        c++;
        i += advance;
    }
    return 0;
}

static void app_draw_sample_line(App *app, Renderer *r, i32 y, String8 line) {
    i32 x = 0;
    u32 color = app->config->theme.text;
    i64 run = 0;
    for (i64 i = 0; i <= line.len; i++) {
        if (i == line.len || line.data[i] < 8) {
            x = font_draw_text(app->font, r, x, y, str8(line.data + run, i - run), COLOR_HEX(color));
            if (i < line.len) color = app_markup_color(&app->config->theme, line.data[i]);
            run = i + 1;
        }
    }
}

// --sample: the Phase 2 display. Hand-colored sample text, a block cursor moved by the arrow
// keys, a minibuffer that echoes typed text.
static b32 app_dev_sample_frame(App *app, FrameInput *in, Renderer *r, AppLayout *l) {
    for (i32 i = 0; i < in->event_count; i++) {
        Event *e = &in->events[i];
        switch (e->kind) {
        case EVENT_CLOSE:
            return 0;
        case EVENT_KEY_DOWN:
            if (e->key == KEY_LEFT)  app->cursor_col--;
            if (e->key == KEY_RIGHT) app->cursor_col++;
            if (e->key == KEY_UP)    app->cursor_row--;
            if (e->key == KEY_DOWN)  app->cursor_row++;
            if (e->key == KEY_BACKSPACE && app->echo.len > 0) {
                do app->echo.len--;
                while (app->echo.len > 0 && (app->echo.text[app->echo.len] & 0xC0) == 0x80);
            }
            break;
        case EVENT_TEXT: {
            u8 bytes[4];
            i64 n = utf8_encode(e->codepoint, bytes);
            if (app->echo.len + n <= ECHO_CAP) {
                memcpy(app->echo.text + app->echo.len, bytes, (size_t)n);
                app->echo.len += (i32)n;
            }
        } break;
        default:
            break;
        }
    }
    app->cursor_col = CLAMP(app->cursor_col, 0, l->cols - 1);
    app->cursor_row = CLAMP(app->cursor_row, 0, l->rows - 1);

    r_begin_frame(r, COLOR_HEX(app->config->theme.background));

    for (i32 row = 0; row < l->rows && row < ARRAY_COUNT(app_sample); row++) {
        app_draw_sample_line(app, r, row * l->line_h, app_sample_line(row));
    }

    // Block cursor; the character under it is redrawn in the background color, as Emacs does.
    i32 cx = app->cursor_col * l->cell_w, cy = app->cursor_row * l->line_h;
    Rect cursor = { (f32)cx, (f32)cy, (f32)(cx + l->cell_w), (f32)(cy + l->line_h) };
    r_push_rect(r, cursor, COLOR_HEX(app->config->theme.cursor));
    font_draw_text(app->font, r, cx, cy, app_char_at(app->cursor_row, app->cursor_col), COLOR_HEX(app->config->theme.background));

    String8 status = str8_fmt(in->scratch, "-:---  sample.jai    (Jai)    L%d C%d", app->cursor_row + 1, app->cursor_col);
    r_push_rect(r, (Rect){ 0, (f32)l->mode_line_y, (f32)in->width, (f32)(l->mode_line_y + l->line_h) }, COLOR_HEX(app->config->theme.text));
    font_draw_text(app->font, r, 0, l->mode_line_y, status, COLOR_HEX(app->config->theme.background));
    font_draw_text(app->font, r, 0, l->minibuffer_y, str8(app->echo.text, app->echo.len), COLOR_HEX(app->config->theme.text));

    r_end_frame(r);
    return 1;
}
#endif

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

// ---------------------------------------------------------------------------
// Startup

// The config is read before the font, so the font is set up exactly once, as configured.
App *app_create(Arena *perm, AppArgs *args) {
    App *app = PUSH_STRUCT(perm, App);
    app->forced_render_mode = args->render_mode_forced ? (i32)args->render_mode : -1;
    app->config_arenas[0] = arena_create(APP_CONFIG_RESERVE);
    app->config_arenas[1] = arena_create(APP_CONFIG_RESERVE);
    app->config_path = app_config_path(perm, args);
    app->config_source.path = app->config_path;
    ConfigPoll config_result = app_poll_config(app, 1, 1);
    FontParams fp = app_font_params(app);
#if TEAL_DEV
    if (!args->headless)
#endif
    {
        app->font = font_create(perm, &fp, args->dpi_scale);
        if (!app->font) return NULL;
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
    Buffer *mini = buffer_create(STR8_LIT(" *Minibuf-1*")); // not listed, as in Emacs
    if (!mini) os_fatal(STR8_LIT("Out of address space (buffer reserve failed)."));
    minibuffer_init(&app->mini, perm, mini);
    app->files_arena = arena_create(GB(1));
    app->ctx.mini = &app->mini;
    app->ctx.app = app;
    app->ctx.echo = &app->echo;
    if (!kill_init(&app->kills, app->config->settings.kill_ring_max)) os_fatal(STR8_LIT("Out of address space (kill ring)."));
    app->ctx.kills = &app->kills;
    app_apply_config(app, NULL, 1);
    app_report_config(app, config_result, 0);
    app_watch_config(app);
    if (app->font && app->font->used_fallback) {
        echo_message(&app->echo, "Font '%S' not found, using Consolas", str8(app->font->family, app->font->family_len));
    }
    // +LINE:COLUMN, 1-based on the command line as in Emacs (move-to-column (1- COLUMN)).
    app->initial_line = args->goto_line > 0 ? args->goto_line - 1 : -1;
    app->initial_col = MAX(args->goto_col - 1, 0);
#if TEAL_DEV
    app->sample = args->sample;
    app->force_focus = -1;
#endif
    return app;
}

i32 app_shutdown(App *app) {
    i32 leaks = app->font ? font_shutdown(app->font) : 0;
    for (i32 i = 0; i < app->view_count; i++) view_destroy(app->views[i]);
    leaks += minibuffer_destroy(&app->mini);
    os_release(app->files_arena.base);
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

// A KEY_DOWN or text event through the keymap stack: [minibuffer, global] while the minibuffer
// is active, else [global]. A single-key prompt takes the key itself, after the keymap has said
// what the key is bound to, so the quit keys still abort it.
static void app_key_event(App *app, Event *e) {
    Keymap *stack[2];
    i32 count = 0;
    if (app->mini.active) stack[count++] = &app->config->minibuffer;
    stack[count++] = &app->config->global;
    KeyResult k;
    key_input_feed(&app->keys, stack, count, e, &k);
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
        echo_clear(&app->echo); // a message stays until the next key
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

u32 app_wait_ms(App *app) {
    return config_wait_ms(&app->config_source, os_time_us());
}

// Events, commands and layout: everything but drawing (no font or renderer calls, so a headless app
// runs it too). False = quit.
static b32 app_update(App *app, FrameInput *in) {
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
            minibuffer_abort(&app->mini);
            app_run_command(app, &CMD_SAVE_BUFFERS_KILL_TERMINAL, 0, 0);
            break;
        case EVENT_FOCUS:
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
            break;
        default:
            break;
        }
    }
    if (config_pending(&app->config_source)) app_reload_config(app, 0); // a settle delay or retry may be due (EVENT_WAKEUP)
    if (app->quit) return 0;
    // A command may have changed the font (text scale, config): lay out again.
    l = app_layout(app, in);
    app_layout_views(app, in, &l);
    for (i32 i = 0; i < app->view_count; i++) view_ensure_visible(app->views[i]); // also views showing a buffer edited elsewhere
    if (app->mini.active) view_ensure_visible(app->mini.view);
    app->laid_w = in->width;
    app->laid_h = in->height;
    app->laid_cell_w = l.cell_w;
    app->laid_line_h = l.line_h;
    app_update_title(app, in->scratch);
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
        app_draw_view(app, r, &l, in, app->views[i], i == app->active_view && !app->mini.active, bottom);
    }
    if (app->mini.active) {
        if (list_rows) app_draw_candidates(app, r, &l, in, list_rows);
        app_draw_minibuffer(app, r, &l, in);
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
#if TEAL_DEV
    if (app->sample) {
        AppLayout l = app_layout(app, in);
        return app_dev_sample_frame(app, in, r, &l);
    }
#endif
    b32 running = app_update(app, in);
    app->renderer = NULL;
    if (!running) return 0;
    app_render(app, in, r);
#if TEAL_DEV
    app->dev_build_us = os_time_us() - t0;
#endif
    r_end_frame(r);
    return 1;
}

#if TEAL_DEV
i32 app_dev_probes(App *app, FrameInput *in, DevProbe *out, i32 cap) {
    AppLayout l = app_layout(app, in);
    i32 n = 0;
#define APP_PUSH_PROBE(...) do { if (n < cap) out[n++] = (DevProbe){ __VA_ARGS__ }; } while (0)

    APP_PUSH_PROBE(.kind = DEV_PROBE_PIXEL_EQ, .x0 = in->width - 1, .y0 = in->height - 1,
                   .rgb = app->config->theme.background, .what = "background (bottom-right corner)");
    APP_PUSH_PROBE(.kind = DEV_PROBE_PIXEL_EQ, .x0 = in->width - 1, .y0 = l.mode_line_y + l.line_h / 2,
                   .rgb = app->config->theme.text, .what = "mode line (right end)");
    i32 cx = app->cursor_col * l.cell_w, cy = app->cursor_row * l.line_h;
    APP_PUSH_PROBE(.kind = DEV_PROBE_PIXEL_EQ, .x0 = cx, .y0 = cy, .rgb = app->config->theme.cursor, .what = "cursor (top-left pixel)");

    // First non-space character cell not under the cursor: some pixel must differ from the background.
    for (i32 row = 0; row < l.rows && row < ARRAY_COUNT(app_sample); row++) {
        b32 found = 0;
        for (i32 col = 0; col < l.cols; col++) {
            String8 ch = app_char_at(row, col);
            if (!ch.len) break;
            if (ch.data[0] == ' ' || (row == app->cursor_row && col == app->cursor_col)) continue;
            APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, .x0 = col * l.cell_w, .y0 = row * l.line_h,
                           .x1 = (col + 1) * l.cell_w, .y1 = (row + 1) * l.line_h,
                           .rgb = app->config->theme.background, .what = "first text cell");
            found = 1;
            break;
        }
        if (found) break;
    }

    // A space cell surrounded by spaces (middle of an indentation, spaces above and below too):
    // exactly the background color.
    for (i32 row = 1; row + 1 < l.rows && row + 1 < ARRAY_COUNT(app_sample); row++) {
        String8 here = app_char_at(row, 1), left = app_char_at(row, 0), right = app_char_at(row, 2);
        String8 above = app_char_at(row - 1, 1), below = app_char_at(row + 1, 1);
        b32 spaces = here.len && left.len && right.len && here.data[0] == ' ' && left.data[0] == ' ' &&
                     right.data[0] == ' ' && (!above.len || above.data[0] == ' ') &&
                     (!below.len || below.data[0] == ' ');
        if (spaces && !(app->cursor_row == row && app->cursor_col == 1)) {
            APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = l.cell_w, .y0 = row * l.line_h,
                           .x1 = 2 * l.cell_w, .y1 = (row + 1) * l.line_h,
                           .rgb = app->config->theme.background, .what = "space cell");
            break;
        }
    }

    // Orientation: the first '_' must have ink only in the lower part of its cell. Catches glyph
    // bitmaps copied upside down, which every probe above would miss.
    for (i32 row = 0; row < l.rows && row < ARRAY_COUNT(app_sample); row++) {
        b32 found = 0;
        for (i32 col = 0; col < l.cols; col++) {
            String8 ch = app_char_at(row, col);
            if (!ch.len) break;
            if (ch.data[0] == '_' && !(row == app->cursor_row && col == app->cursor_col)) {
                i32 x0 = col * l.cell_w, y0 = row * l.line_h;
                APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x0, .y0 = y0, .x1 = x0 + l.cell_w,
                               .y1 = y0 + l.line_h * 2 / 5, .rgb = app->config->theme.background, .what = "'_' upper part");
                APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, .x0 = x0, .y0 = y0 + l.line_h / 2,
                               .x1 = x0 + l.cell_w, .y1 = y0 + l.line_h, .rgb = app->config->theme.background,
                               .what = "'_' lower part");
                found = 1;
                break;
            }
        }
        if (found) break;
    }

    // A pure white '|' (keyword color) for the ClearType channel-order check.
    for (i32 row = 0; row < l.rows && row < ARRAY_COUNT(app_sample); row++) {
        b32 found = 0;
        for (i32 col = 0; col < l.cols; col++) {
            String8 ch = app_char_at(row, col);
            if (!ch.len) break;
            if (ch.data[0] == '|' && app_markup_at(row, col) == 2 && app->config->theme.keyword == 0xffffff &&
                !(row == app->cursor_row && col == app->cursor_col)) {
                APP_PUSH_PROBE(.kind = DEV_PROBE_CLEARTYPE, .x0 = col * l.cell_w - 1, .y0 = row * l.line_h,
                               .x1 = (col + 1) * l.cell_w + 1, .y1 = (row + 1) * l.line_h,
                               .rgb = app->config->theme.background, .text_rgb = 0xffffff,
                               .geometry = app->font->backend.pixel_geometry, .what = "ClearType '|'");
                found = 1;
                break;
            }
        }
        if (found) break;
    }
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
    app->sample = 0;
    Buffer *buf = app->views[0]->buffer;
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
    m.buffer = m.meta + m.text + m.line_index + m.markers + m.undo_log;
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
