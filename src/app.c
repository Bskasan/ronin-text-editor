// app.c — Phase 1 placeholder. Throwaway: proves key events, text events and
// on-demand redraw through the rect pipeline.

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

#define APP_CELL_W 9  // logical pixels
#define APP_CELL_H 19

struct App {
    i32 cursor_col, cursor_row;
};

typedef struct AppLayout {
    i32 cell_w, cell_h;
    i32 cols, rows;      // text area grid
    i32 mode_line_y;     // the mode line occupies [mode_line_y, mode_line_y + cell_h)
} AppLayout;

static AppLayout app_layout(FrameInput *in) {
    AppLayout l;
    l.cell_w = MAX((i32)(APP_CELL_W * in->dpi_scale + 0.5f), 1);
    l.cell_h = MAX((i32)(APP_CELL_H * in->dpi_scale + 0.5f), 1);
    l.mode_line_y = in->height - 2 * l.cell_h; // one line of minibuffer below it
    l.cols = MAX(in->width / l.cell_w, 1);
    l.rows = MAX(l.mode_line_y / l.cell_h, 1);
    return l;
}

App *app_create(Arena *perm) {
    return PUSH_STRUCT(perm, App);
}

b32 app_update_and_render(App *app, FrameInput *in, Renderer *r) {
    AppLayout l = app_layout(in);

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
            break;
        case EVENT_TEXT:
            app->cursor_col++;
            if (app->cursor_col >= l.cols && app->cursor_row < l.rows - 1) {
                app->cursor_col = 0;
                app->cursor_row++;
            }
            break;
        default:
            break;
        }
        app->cursor_col = CLAMP(app->cursor_col, 0, l.cols - 1);
        app->cursor_row = CLAMP(app->cursor_row, 0, l.rows - 1);
    }
    app->cursor_col = CLAMP(app->cursor_col, 0, l.cols - 1);
    app->cursor_row = CLAMP(app->cursor_row, 0, l.rows - 1);

    r_begin_frame(r, COLOR_HEX(THEME_BACKGROUND));

    Rect mode_line = { 0, (f32)l.mode_line_y, (f32)in->width, (f32)(l.mode_line_y + l.cell_h) };
    r_push_rect(r, mode_line, COLOR_HEX(THEME_TEXT));

    f32 cx = (f32)(app->cursor_col * l.cell_w), cy = (f32)(app->cursor_row * l.cell_h);
    Rect cursor = { cx, cy, cx + l.cell_w, cy + l.cell_h };
    r_push_rect(r, cursor, COLOR_HEX(THEME_CURSOR));

    r_end_frame(r);
    return 1;
}

#if TEAL_DEV
i32 app_dev_probes(App *app, FrameInput *in, DevProbe *out, i32 cap) {
    AppLayout l = app_layout(in);
    DevProbe probes[] = {
        // Bottom-right corner of the minibuffer line: nothing is ever drawn there.
        { in->width - 1, in->height - 1, THEME_BACKGROUND, "background" },
        { in->width / 2, l.mode_line_y + l.cell_h / 2, THEME_TEXT, "mode line" },
        { app->cursor_col * l.cell_w + l.cell_w / 2, app->cursor_row * l.cell_h + l.cell_h / 2, THEME_CURSOR, "cursor" },
    };
    i32 n = (i32)MIN(ARRAY_COUNT(probes), (i64)cap);
    for (i32 i = 0; i < n; i++) out[i] = probes[i];
    return n;
}
#endif
