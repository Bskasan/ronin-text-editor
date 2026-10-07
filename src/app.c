// app.c — the editor core. Phase 3: a throwaway display of one buffer (the real view arrives
// in Phase 4), a mode line and a minibuffer. Dev builds keep the Phase 2 hand-colored sample
// behind --sample for the smoke probes and screenshots (until highlighting in Phase 8).

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

#define APP_MINIBUFFER_CAP 1024

struct App {
    Font *font;
    u8 minibuffer[APP_MINIBUFFER_CAP];
    i32 minibuffer_len;
#if TEAL_DEV
    b32 sample; // --sample: the Phase 2 display
    i32 cursor_col, cursor_row;
#endif
};

typedef struct AppLayout {
    i32 cell_w, line_h;
    i32 cols, rows;      // text area grid
    i32 mode_line_y;     // the mode line occupies [mode_line_y, mode_line_y + line_h)
    i32 minibuffer_y;
} AppLayout;

static AppLayout app_layout(App *app, FrameInput *in) {
    AppLayout l;
    l.cell_w = app->font->cell_w;
    l.line_h = app->font->line_h;
    l.minibuffer_y = in->height - l.line_h;
    l.mode_line_y = l.minibuffer_y - l.line_h;
    l.cols = MAX(in->width / l.cell_w, 1);
    l.rows = MAX(l.mode_line_y / l.line_h, 1);
    return l;
}

static void app_draw_mode_line(App *app, Renderer *r, FrameInput *in, AppLayout *l, String8 text) {
    Rect mode_line = { 0, (f32)l->mode_line_y, (f32)in->width, (f32)(l->mode_line_y + l->line_h) };
    r_push_rect(r, mode_line, COLOR_HEX(THEME_TEXT)); // inverse video
    font_draw_text(app->font, r, 0, l->mode_line_y, text, COLOR_HEX(THEME_BACKGROUND));
}

#if TEAL_DEV
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
            if (e->key == KEY_BACKSPACE && app->minibuffer_len > 0) {
                do app->minibuffer_len--;
                while (app->minibuffer_len > 0 && (app->minibuffer[app->minibuffer_len] & 0xC0) == 0x80);
            }
            break;
        case EVENT_TEXT: {
            u8 bytes[4];
            i64 n = utf8_encode(e->codepoint, bytes);
            if (app->minibuffer_len + n <= APP_MINIBUFFER_CAP) {
                memcpy(app->minibuffer + app->minibuffer_len, bytes, (size_t)n);
                app->minibuffer_len += (i32)n;
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
    app_draw_mode_line(app, r, in, l, status);
    font_draw_text(app->font, r, 0, l->minibuffer_y, str8(app->minibuffer, app->minibuffer_len), COLOR_HEX(THEME_TEXT));

    r_end_frame(r);
    return 1;
}
#endif

App *app_create(Arena *perm, AppConfig *config) {
    App *app = PUSH_STRUCT(perm, App);
    app->font = font_create(perm, config->dpi_scale, config->render_mode);
    if (!app->font) return NULL;
#if TEAL_DEV
    app->sample = config->sample;
#endif
    return app;
}

i32 app_shutdown(App *app) {
    return font_shutdown(app->font);
}

b32 app_update_and_render(App *app, FrameInput *in, Renderer *r) {
    font_frame_begin(app->font, r, in->dpi_scale);
    AppLayout l = app_layout(app, in);
#if TEAL_DEV
    if (app->sample) return app_dev_sample_frame(app, in, r, &l);
#endif

    for (i32 i = 0; i < in->event_count; i++) {
        if (in->events[i].kind == EVENT_CLOSE) return 0;
    }

    r_begin_frame(r, COLOR_HEX(THEME_BACKGROUND));
    app_draw_mode_line(app, r, in, &l, STR8_LIT("-:---  *scratch*"));
    font_draw_text(app->font, r, 0, l.minibuffer_y, str8(app->minibuffer, app->minibuffer_len), COLOR_HEX(THEME_TEXT));
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
