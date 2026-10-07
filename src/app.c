// app.c — the editor core: views laid out in the frame, drawing of text, cursors, mode lines
// and the echo area, and (temporarily, until the keymap of Phase 5) the key bindings. View
// logic lives in view.c. Dev builds keep the Phase 2 hand-colored sample behind --sample for
// the smoke probes and screenshots (until highlighting in Phase 8).

// Theme (source of truth until the theme file arrives in Phase 7).
#define THEME_BACKGROUND 0x072626
#define THEME_TEXT       0xd3b58d // also the mode line
#define THEME_CURSOR     0x90ee90
#define THEME_SELECTION  0x0000ff
#define THEME_COMMENT    0x3fdf1f
#define THEME_STRING     0x0fdfaf
#define THEME_KEYWORD    0xffffff
#define THEME_NUMBER     0x7ad0c6
#define THEME_TYPE       0x8cde94
#define THEME_VARIABLE   0xc1d1e3

#if TEAL_DEV
// Sample text markup: these bytes switch the color of what follows and take no cell.
#define P "\x01" // plain text
#define K "\x02" // keyword
#define T "\x03" // type
#define S "\x04" // string
#define N "\x05" // number / constant
#define C "\x06" // comment
#define V "\x07" // variable

static const u32 app_markup_colors[8] = {
    0, THEME_TEXT, THEME_KEYWORD, THEME_TYPE, THEME_STRING, THEME_NUMBER, THEME_COMMENT, THEME_VARIABLE,
};

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

struct App {
    Font *font;
    Buffer *buffer;              // the only buffer until buffer switching (Phase 9)
    View *views[APP_MAX_VIEWS];  // laid out side by side
    i32 view_count;
    i32 active_view;
    Echo echo;
    CommandContext ctx;          // keeps last_command between events
    b32 focused;                 // the window has keyboard focus
    i32 wheel_accum;             // wheel units * APP_WHEEL_LINES not yet turned into lines
    u8 title[256];               // the window title last set
    i32 title_len;
    i64 initial_line;            // 0-based line to visit on the first frame, -1 = none
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
    l.cell_w = app->font->cell_w;
    l.line_h = app->font->line_h;
    l.pad = MAX((i32)(APP_PAD_PX * in->dpi_scale + 0.5f), 1);
    l.minibuffer_y = in->height - l.line_h;
    l.mode_line_y = l.minibuffer_y - l.line_h;
    l.cols = MAX(in->width / l.cell_w, 1);
    l.rows = MAX(l.mode_line_y / l.line_h, 1);
    return l;
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
            if (i > run) font_draw_text(app->font, r, x0 + (i32)(run_col - left) * cell_w, y, str8(s.data + run, i - run), COLOR_HEX(THEME_TEXT));
            if (b != '\t') app_draw_caret(app, r, x0, y, col, left, right, b, COLOR_HEX(THEME_NUMBER));
            col += view_char_width(b, col);
            run = ++i;
            run_col = col;
            continue;
        }
        i64 advance = 1;
        if (b >= 0x80) utf8_decode(s.data + i, s.len - i, &advance);
        i += advance;
        col++;
    }
    if (i > run) font_draw_text(app->font, r, x0 + (i32)(run_col - left) * cell_w, y, str8(s.data + run, i - run), COLOR_HEX(THEME_TEXT));
}

// The cell rect of a cursor, or false when it is outside the window (only the primary cursor
// is kept visible).
static b32 app_cursor_rect(App *app, AppLayout *l, View *v, i64 pos, i32 *x0, i32 *y0, i32 *x1, i32 *y1) {
    Buffer *buf = v->buffer;
    i64 row = buffer_line_of(buf, pos) - view_top_line(v);
    if (row < 0 || row >= v->rows) return 0;
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
static void app_draw_cursor(App *app, Renderer *r, AppLayout *l, View *v, i64 pos, b32 filled, f32 dpi_scale, Arena *scratch) {
    i32 x0, y0, x1, y1;
    if (!app_cursor_rect(app, l, v, pos, &x0, &y0, &x1, &y1)) return;
    Color cursor = COLOR_HEX(THEME_CURSOR);
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
    Color under = COLOR_HEX(THEME_BACKGROUND);
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

static void app_draw_view(App *app, Renderer *r, AppLayout *l, FrameInput *in, View *v, b32 active) {
    Buffer *buf = v->buffer;
    i32 line_h = l->line_h;
    i32 text_x = v->x + l->pad;
    i32 mode_y = v->y + v->h - line_h;
    // Only the visible lines (the partial one above the mode line too, which covers it).
    i64 top = view_top_line(v), count = buffer_line_count(buf);
    i32 draw_rows = (mode_y - v->y + line_h - 1) / line_h;
    for (i32 row = 0; row < draw_rows && top + row < count; row++) {
        app_draw_buffer_line(app, r, buf, top + row, text_x, v->y + row * line_h, v->left_col, v->cols, in->scratch);
    }
    b32 filled = active && app_has_focus(app);
    for (i32 k = 0; k < v->cursor_count; k++) {
        app_draw_cursor(app, r, l, v, view_point(v, &v->cursors[k]), filled, in->dpi_scale, in->scratch);
    }
    // Mode line, inverse video.
    r_push_rect(r, (Rect){ (f32)v->x, (f32)mode_y, (f32)(v->x + v->w), (f32)(mode_y + line_h) }, COLOR_HEX(THEME_TEXT));
    String8 mode = app_mode_line_text(v, in->scratch);
    font_draw_text(app->font, r, text_x, mode_y, app_clip_cells(mode, MAX((v->w - l->pad) / l->cell_w, 0)), COLOR_HEX(THEME_BACKGROUND));
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

// Color of the character at `col` (the markup color in effect), or 0 if there is none.
static u32 app_color_at(i32 row, i32 col) {
    String8 line = app_sample_line(row);
    u32 color = THEME_TEXT;
    i32 c = 0;
    for (i64 i = 0; i < line.len;) {
        if (line.data[i] < 8) {
            color = app_markup_colors[line.data[i]];
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
    u32 color = THEME_TEXT;
    i64 run = 0;
    for (i64 i = 0; i <= line.len; i++) {
        if (i == line.len || line.data[i] < 8) {
            x = font_draw_text(app->font, r, x, y, str8(line.data + run, i - run), COLOR_HEX(color));
            if (i < line.len) color = app_markup_colors[line.data[i]];
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

    r_begin_frame(r, COLOR_HEX(THEME_BACKGROUND));

    for (i32 row = 0; row < l->rows && row < ARRAY_COUNT(app_sample); row++) {
        app_draw_sample_line(app, r, row * l->line_h, app_sample_line(row));
    }

    // Block cursor; the character under it is redrawn in the background color, as Emacs does.
    i32 cx = app->cursor_col * l->cell_w, cy = app->cursor_row * l->line_h;
    Rect cursor = { (f32)cx, (f32)cy, (f32)(cx + l->cell_w), (f32)(cy + l->line_h) };
    r_push_rect(r, cursor, COLOR_HEX(THEME_CURSOR));
    font_draw_text(app->font, r, cx, cy, app_char_at(app->cursor_row, app->cursor_col), COLOR_HEX(THEME_BACKGROUND));

    String8 status = str8_fmt(in->scratch, "-:---  sample.jai    (Jai)    L%d C%d", app->cursor_row + 1, app->cursor_col);
    r_push_rect(r, (Rect){ 0, (f32)l->mode_line_y, (f32)in->width, (f32)(l->mode_line_y + l->line_h) }, COLOR_HEX(THEME_TEXT));
    font_draw_text(app->font, r, 0, l->mode_line_y, status, COLOR_HEX(THEME_BACKGROUND));
    font_draw_text(app->font, r, 0, l->minibuffer_y, str8(app->echo.text, app->echo.len), COLOR_HEX(THEME_TEXT));

    r_end_frame(r);
    return 1;
}
#endif

// ---------------------------------------------------------------------------
// Startup and frames

// Opens config->file_path, or *scratch* without one or when it cannot be opened.
static void app_open_initial_buffer(App *app, AppConfig *config) {
    if (config->file_path.len) {
        Buffer *buf = buffer_create(STR8_LIT(""));
        if (buf) {
#if TEAL_DEV
            u64 t0 = os_time_us();
#endif
            OsFileStatus status = buffer_load_file(buf, config->file_path);
            if (status == OS_FILE_OK) {
                app->buffer = buf;
                LOG("app: loaded %S: %D bytes, %D lines, %s %s, %U us", buf->path, buffer_size(buf), buffer_line_count(buf),
                    app_encoding_name(buf->encoding), app_eol_name(buf->eol), os_time_us() - t0);
            } else if (status == OS_FILE_NOT_FOUND) {
                // As in Emacs: visit the path as a new file.
                String8 full = os_full_path(&buf->meta, config->file_path);
                buffer_set_path(buf, full.len ? full : config->file_path);
                app->buffer = buf;
                echo_message(&app->echo, "(New file)");
            } else {
                echo_message(&app->echo, "Cannot open %S: %s", config->file_path, buffer_status_text(status));
                buffer_destroy(buf);
            }
        }
    }
    if (!app->buffer) app->buffer = buffer_create(STR8_LIT("*scratch*"));
    if (!app->buffer) os_fatal(STR8_LIT("Out of address space (buffer reserve failed)."));
}

App *app_create(Arena *perm, AppConfig *config) {
    App *app = PUSH_STRUCT(perm, App);
    app->font = font_create(perm, config->dpi_scale, config->render_mode);
    if (!app->font) return NULL;
    app_open_initial_buffer(app, config);
    app->views[0] = view_create(perm, app->buffer);
    app->view_count = 1;
    app->ctx.echo = &app->echo;
    // +LINE:COLUMN, 1-based on the command line as in Emacs (move-to-column (1- COLUMN)).
    app->initial_line = config->goto_line > 0 ? config->goto_line - 1 : -1;
    app->initial_col = MAX(config->goto_col - 1, 0);
#if TEAL_DEV
    app->sample = config->sample;
    app->force_focus = -1;
#endif
    return app;
}

i32 app_shutdown(App *app) {
    i32 leaks = font_shutdown(app->font);
    for (i32 i = 0; i < app->view_count; i++) view_destroy(app->views[i]);
    leaks += (i32)app->buffer->marker_live;
    if (!buffer_destroy(app->buffer)) leaks++;
    return leaks;
}

// The temporary key bindings (Phase 5 replaces them with the keymap). Shift is ignored.
static const Command *app_key_command(Event *e, u32 *codepoint) {
    u32 mods = e->mods & ~(u32)MOD_SHIFT;
    b32 plain = mods == 0, ctrl = mods == MOD_CTRL, meta = mods == MOD_ALT;
    switch (e->key) {
    case KEY_LEFT:      return plain ? &CMD_BACKWARD_CHAR : ctrl ? &CMD_BACKWARD_WORD : NULL;
    case KEY_RIGHT:     return plain ? &CMD_FORWARD_CHAR : ctrl ? &CMD_FORWARD_WORD : NULL;
    case KEY_UP:        return plain ? &CMD_PREVIOUS_LINE : ctrl ? &CMD_BACKWARD_PARAGRAPH : NULL;
    case KEY_DOWN:      return plain ? &CMD_NEXT_LINE : ctrl ? &CMD_FORWARD_PARAGRAPH : NULL;
    case KEY_HOME:      return plain ? &CMD_MOVE_BEGINNING_OF_LINE : ctrl ? &CMD_BEGINNING_OF_BUFFER : NULL;
    case KEY_END:       return plain ? &CMD_MOVE_END_OF_LINE : ctrl ? &CMD_END_OF_BUFFER : NULL;
    case KEY_PAGE_UP:   return plain ? &CMD_SCROLL_DOWN_COMMAND : NULL;
    case KEY_PAGE_DOWN: return plain ? &CMD_SCROLL_UP_COMMAND : NULL;
    case KEY_BACKSPACE: return plain ? &CMD_DELETE_BACKWARD_CHAR : NULL;
    case KEY_DELETE:    return plain ? &CMD_DELETE_CHAR : NULL;
    case KEY_ENTER:     return plain ? &CMD_NEWLINE : NULL;
    case KEY_TAB:       *codepoint = '\t'; return plain ? &CMD_SELF_INSERT : NULL;
    case KEY_F:         return ctrl ? &CMD_FORWARD_CHAR : meta ? &CMD_FORWARD_WORD : NULL;
    case KEY_B:         return ctrl ? &CMD_BACKWARD_CHAR : meta ? &CMD_BACKWARD_WORD : NULL;
    case KEY_N:         return ctrl ? &CMD_NEXT_LINE : NULL;
    case KEY_P:         return ctrl ? &CMD_PREVIOUS_LINE : NULL;
    case KEY_A:         return ctrl ? &CMD_MOVE_BEGINNING_OF_LINE : NULL;
    case KEY_E:         return ctrl ? &CMD_MOVE_END_OF_LINE : NULL;
    case KEY_V:         return ctrl ? &CMD_SCROLL_UP_COMMAND : meta ? &CMD_SCROLL_DOWN_COMMAND : NULL;
    case KEY_L:         return ctrl ? &CMD_RECENTER_TOP_BOTTOM : NULL;
    case KEY_D:         return ctrl ? &CMD_DELETE_CHAR : NULL;
    case KEY_S:         return ctrl ? &CMD_SAVE_BUFFER : NULL; // temporary: C-x C-s in Phase 5
    default:            return NULL;
    }
}

// The view under a pixel, or -1.
static i32 app_view_at(App *app, i32 x, i32 y) {
    for (i32 i = 0; i < app->view_count; i++) {
        View *v = app->views[i];
        if (x >= v->x && x < v->x + v->w && y >= v->y && y < v->y + v->h) return i;
    }
    return -1;
}

// A left click in a text area activates its view and puts point at the clicked cell.
static void app_click(App *app, AppLayout *l, i32 x, i32 y) {
    i32 i = app_view_at(app, x, y);
    if (i < 0) return;
    View *v = app->views[i];
    if (y >= v->y + v->h - l->line_h) return; // the mode line
    i64 row = MIN((y - v->y) / l->line_h, v->rows - 1); // the partial row counts as the last one
    i64 col = v->left_col + MAX(x - (v->x + l->pad), 0) / l->cell_w;
    app->active_view = i;
    view_set_point_at(v, row, col);
    view_ensure_visible(v);
    app->ctx.last_command = NULL;
}

// The wheel scrolls the view under the mouse; point is dragged along to stay visible.
static void app_wheel(App *app, i32 x, i32 y, i32 wheel) {
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

static void app_run_command(App *app, const Command *cmd, u32 codepoint) {
    app->ctx.view = app->views[app->active_view];
    app->ctx.codepoint = codepoint;
    view_run_command(&app->ctx, cmd);
}

b32 app_update_and_render(App *app, FrameInput *in, Renderer *r) {
#if TEAL_DEV
    u64 t0 = os_time_us();
#endif
    font_frame_begin(app->font, r, in->dpi_scale);
    AppLayout l = app_layout(app, in);
#if TEAL_DEV
    if (app->sample) return app_dev_sample_frame(app, in, r, &l);
#endif
    app_layout_views(app, in, &l);
    if (app->initial_line >= 0) { // the first frame: the layout is known now
        view_goto_line_column(app->views[0], app->initial_line, app->initial_col);
        app->initial_line = -1;
    }
    for (i32 i = 0; i < app->view_count; i++) view_ensure_visible(app->views[i]); // the size may have changed

    for (i32 i = 0; i < in->event_count; i++) {
        Event *e = &in->events[i];
        switch (e->kind) {
        case EVENT_CLOSE:
            return 0;
        case EVENT_FOCUS:
            app->focused = e->focused;
            break;
        case EVENT_KEY_DOWN: {
            echo_clear(&app->echo); // a message stays until the next key
            u32 codepoint = 0;
            const Command *cmd = app_key_command(e, &codepoint);
            if (cmd) app_run_command(app, cmd, codepoint);
            else app->ctx.last_command = NULL;
        } break;
        case EVENT_TEXT:
            echo_clear(&app->echo);
            app_run_command(app, &CMD_SELF_INSERT, e->codepoint);
            break;
        case EVENT_MOUSE_DOWN:
            if (e->button == MOUSE_LEFT) app_click(app, &l, e->x, e->y);
            break;
        case EVENT_MOUSE_WHEEL:
            app_wheel(app, e->x, e->y, e->wheel);
            break;
        default:
            break;
        }
    }

    app_update_title(app, in->scratch);

    r_begin_frame(r, COLOR_HEX(THEME_BACKGROUND));
    for (i32 i = 0; i < app->view_count; i++) app_draw_view(app, r, &l, in, app->views[i], i == app->active_view);
    String8 echo = app_clip_cells(str8(app->echo.text, app->echo.len), MAX((in->width - l.pad) / l.cell_w, 0));
    font_draw_text(app->font, r, l.pad, l.minibuffer_y, echo, COLOR_HEX(THEME_TEXT));

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
                   .rgb = THEME_BACKGROUND, .what = "background (bottom-right corner)");
    APP_PUSH_PROBE(.kind = DEV_PROBE_PIXEL_EQ, .x0 = in->width - 1, .y0 = l.mode_line_y + l.line_h / 2,
                   .rgb = THEME_TEXT, .what = "mode line (right end)");
    i32 cx = app->cursor_col * l.cell_w, cy = app->cursor_row * l.line_h;
    APP_PUSH_PROBE(.kind = DEV_PROBE_PIXEL_EQ, .x0 = cx, .y0 = cy, .rgb = THEME_CURSOR, .what = "cursor (top-left pixel)");

    // First non-space character cell not under the cursor: some pixel must differ from the background.
    for (i32 row = 0; row < l.rows && row < ARRAY_COUNT(app_sample); row++) {
        b32 found = 0;
        for (i32 col = 0; col < l.cols; col++) {
            String8 ch = app_char_at(row, col);
            if (!ch.len) break;
            if (ch.data[0] == ' ' || (row == app->cursor_row && col == app->cursor_col)) continue;
            APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, .x0 = col * l.cell_w, .y0 = row * l.line_h,
                           .x1 = (col + 1) * l.cell_w, .y1 = (row + 1) * l.line_h,
                           .rgb = THEME_BACKGROUND, .what = "first text cell");
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
                           .rgb = THEME_BACKGROUND, .what = "space cell");
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
                               .y1 = y0 + l.line_h * 2 / 5, .rgb = THEME_BACKGROUND, .what = "'_' upper part");
                APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, .x0 = x0, .y0 = y0 + l.line_h / 2,
                               .x1 = x0 + l.cell_w, .y1 = y0 + l.line_h, .rgb = THEME_BACKGROUND,
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
            if (ch.data[0] == '|' && app_color_at(row, col) == 0xffffff &&
                !(row == app->cursor_row && col == app->cursor_col)) {
                APP_PUSH_PROBE(.kind = DEV_PROBE_CLEARTYPE, .x0 = col * l.cell_w - 1, .y0 = row * l.line_h,
                               .x1 = (col + 1) * l.cell_w + 1, .y1 = (row + 1) * l.line_h,
                               .rgb = THEME_BACKGROUND, .text_rgb = 0xffffff,
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
    Buffer *buf = app->buffer;
    buffer_replace(buf, 0, buffer_size(buf), STR8_LIT("int x = 1;\n\t|\n\na\x01" "b\n"));
    View *v = app->views[0];
    view_set_point(v, &v->cursors[0], buffer_line_start(buf, 2));
    buffer_marker_set(buf, v->top, 0);
    app->force_focus = 0;
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
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, CELL(0, 0), .rgb = THEME_BACKGROUND,
                       .what = "buffer: text cell 'i' (line 0, column 0)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, CELL(5, 2), .rgb = THEME_BACKGROUND,
                       .what = "buffer: empty cell (line 2, column 5)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x, .y0 = lh, .x1 = x + 4 * cw, .y1 = 2 * lh,
                       .rgb = THEME_BACKGROUND, .what = "buffer: tab, columns 0-3 of line 1 empty");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, CELL(4, 1), .rgb = THEME_BACKGROUND,
                       .what = "buffer: '|' after the tab drawn at column 4");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, CELL(2, 3), .rgb = THEME_BACKGROUND,
                       .what = "buffer: 'A' of ^A drawn at column 2");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, CELL(3, 3), .rgb = THEME_BACKGROUND,
                       .what = "buffer: 'b' after ^A drawn at column 3");
        // Hollow cursor on the empty line 2: edges in the cursor color, inside untouched.
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x, .y0 = 2 * lh, .x1 = x + t, .y1 = 3 * lh,
                       .rgb = THEME_CURSOR, .what = "buffer: hollow cursor, left edge");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x, .y0 = 2 * lh, .x1 = x + cw, .y1 = 2 * lh + t,
                       .rgb = THEME_CURSOR, .what = "buffer: hollow cursor, top edge");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x + t, .y0 = 2 * lh + t, .x1 = x + cw - t, .y1 = 3 * lh - t,
                       .rgb = THEME_BACKGROUND, .what = "buffer: hollow cursor, inside");
        // Mode line: inverse video to the right end, the buffer name drawn, nothing after the text.
        String8 mode = app_mode_line_text(app->views[0], in->scratch);
        i32 cells = (i32)mode.len; // ASCII here
        APP_PUSH_PROBE(.kind = DEV_PROBE_PIXEL_EQ, .x0 = in->width - 1, .y0 = mode_y + lh / 2,
                       .rgb = THEME_TEXT, .what = "buffer: mode line (right end)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, .x0 = x + cw, .y0 = mode_y, .x1 = x + 2 * cw, .y1 = mode_y + lh,
                       .rgb = THEME_TEXT, .what = "buffer: mode line text ('-' in cell 1)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, .x0 = x + 8 * cw, .y0 = mode_y, .x1 = x + 17 * cw, .y1 = mode_y + lh,
                       .rgb = THEME_TEXT, .what = "buffer: mode line buffer name (cells 8-16)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, .x0 = x + (cells + 1) * cw, .y0 = mode_y, .x1 = in->width, .y1 = mode_y + lh,
                       .rgb = THEME_TEXT, .what = "buffer: mode line empty after its text");
    } else {
        // Filled cursor on the clicked 'x' (line 0, column 4), the glyph in the background color.
        APP_PUSH_PROBE(.kind = DEV_PROBE_PIXEL_EQ, .x0 = x + 4 * cw, .y0 = 0, .rgb = THEME_CURSOR,
                       .what = "buffer: filled cursor on 'x' (top-left pixel)");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_DIFFERS, CELL(4, 0), .rgb = THEME_CURSOR,
                       .what = "buffer: filled cursor, 'x' drawn over it");
        APP_PUSH_PROBE(.kind = DEV_PROBE_REGION_EQ, CELL(0, 2), .rgb = THEME_BACKGROUND,
                       .what = "buffer: the old cursor cell is background again");
    }
#undef CELL
#undef APP_PUSH_PROBE
    return n;
}

// Point to the start of `line` (< 0: the last line), the window recentered on it if needed.
void app_dev_goto_line(App *app, i64 line) {
    View *v = app->views[0];
    view_goto_line_column(v, line < 0 ? I64_MAX : line, 0);
    view_ensure_visible(v);
}

i64 app_dev_line_count(App *app) {
    return buffer_line_count(app->buffer);
}

u64 app_dev_build_us(App *app) {
    return app->dev_build_us;
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
    static const u32 colors[] = { THEME_TEXT, THEME_KEYWORD, THEME_TYPE, THEME_STRING, THEME_NUMBER, THEME_COMMENT, THEME_VARIABLE };
    u64 t0 = os_time_us();
    font_frame_begin(app->font, r, in->dpi_scale);
    AppLayout l = app_layout(app, in);
    u8 *row_text = PUSH_ARRAY(in->scratch, u8, l.cols);
    r_begin_frame(r, COLOR_HEX(THEME_BACKGROUND));
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
