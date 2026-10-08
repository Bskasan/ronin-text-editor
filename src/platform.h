// platform.h — the only OS interface the editor core sees.
// Implemented by win32_main.c.

#ifndef PLATFORM_H
#define PLATFORM_H

// ---------------------------------------------------------------------------
// OS primitives

void *os_reserve(u64 size);
b32   os_commit(void *ptr, u64 size);
b32   os_release(void *ptr); // a whole os_reserve range
b32   os_write_file(String8 path, String8 data);
void  os_fatal(String8 message); // does not return
u64   os_time_us(void);              // monotonic microseconds
void  os_set_window_title(String8 title);
void  os_set_caption_color(u32 rgb); // the title bar (Windows 11; ignored elsewhere)
String8 os_exe_dir(Arena *arena);                  // the directory of the executable, no trailing separator
String8 os_get_env(Arena *arena, String8 name);    // empty when not set
b32   os_make_dir(String8 path);                   // true if it exists afterwards (the parent must exist)
#if TEAL_DEV
void  os_log_write(String8 text);
#endif

// ---------------------------------------------------------------------------
// Files. Every failure is reported as a status the caller can show; nothing asserts.

typedef enum OsFileStatus {
    OS_FILE_OK,
    OS_FILE_NOT_FOUND,         // file or directory
    OS_FILE_ACCESS_DENIED,
    OS_FILE_SHARING_VIOLATION, // open elsewhere without sharing
    OS_FILE_TOO_LARGE,
    OS_FILE_DISK_FULL,
    OS_FILE_IS_DIRECTORY,
    OS_FILE_BAD_PATH,
    OS_FILE_READ_ONLY,
    OS_FILE_OUT_OF_MEMORY,
    OS_FILE_NO_PATH,           // the buffer is not visiting a file
    OS_FILE_IO_ERROR,          // anything else
} OsFileStatus;

typedef struct OsFile {
    void *handle;
} OsFile;

typedef struct OsFileInfo {
    b32 exists;
    b32 is_dir;
    b32 read_only;
    b32 swap_ok;    // replacing the file by another one keeps it intact: not a symlink, one hard link
    i64 size;
    u64 write_time; // FILETIME ticks
} OsFileInfo;

String8      os_full_path(Arena *arena, String8 path); // absolute, normalized; empty on failure
OsFileStatus os_file_info(String8 path, OsFileInfo *info);
OsFileStatus os_file_open_read(String8 path, OsFile *file, OsFileInfo *info); // shares read, write, delete
OsFileStatus os_file_read(OsFile file, void *dst, i64 size); // exactly size bytes
// A new, empty file next to `target` (same directory, so it can replace it); its path goes into `arena`.
OsFileStatus os_file_create_temp(String8 target, Arena *arena, OsFile *file, String8 *temp_path);
OsFileStatus os_file_open_overwrite(String8 path, OsFile *file); // opened (or created) and truncated
OsFileStatus os_file_write(OsFile file, void *data, i64 size);
OsFileStatus os_file_flush(OsFile file); // to the disk, not just the cache
void         os_file_close(OsFile file);
// Swaps `temp` in as `target`, keeping the target's attributes; a plain move when target does not exist.
OsFileStatus os_file_replace(String8 target, String8 temp);
void         os_file_delete(String8 path);
#if TEAL_DEV
b32          os_dev_set_read_only(String8 path, b32 read_only);
b32          os_dev_hard_link(String8 existing, String8 link);
#endif

// ---------------------------------------------------------------------------
// Directory watches: a change notification (file names, sizes, write times) on one directory,
// delivered as EVENT_DIR_CHANGED. Waited on with the messages: no polling, no timers.

typedef u32 OsWatch; // 0 = none

OsWatch os_watch_dir(String8 dir); // 0 on failure (no such directory, too many watches)
void    os_unwatch(OsWatch watch);
#if TEAL_DEV
b32     os_dev_watch_wait(OsWatch watch, u32 timeout_ms); // true when it signalled (then re-armed)
i32     os_dev_watch_count(void);                         // watches still open
OsFileStatus os_dev_lock_file(String8 path, OsFile *file); // opened without sharing: readers get a sharing violation
#endif

// ---------------------------------------------------------------------------
// Input

typedef enum Key {
    KEY_NONE,
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
    KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18, KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24,
    KEY_ESCAPE, KEY_TAB, KEY_BACKSPACE, KEY_ENTER, KEY_SPACE,
    KEY_INSERT, KEY_DELETE, KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN,
    KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
    // Punctuation keys, named by their US-layout legend.
    KEY_MINUS, KEY_EQUALS, KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET, KEY_BACKSLASH,
    KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_COMMA, KEY_PERIOD, KEY_SLASH,
    KEY_OEM_102, // the extra key next to left shift on ISO keyboards
    KEY_PAUSE, KEY_APPS,
    KEY_COUNT
} Key;

enum {
    MOD_CTRL  = 1 << 0,
    MOD_ALT   = 1 << 1,
    MOD_SHIFT = 1 << 2,
};

typedef enum MouseButton {
    MOUSE_LEFT,
    MOUSE_RIGHT,
    MOUSE_MIDDLE,
} MouseButton;

typedef enum EventKind {
    EVENT_KEY_DOWN,
    EVENT_TEXT,
    EVENT_MOUSE_DOWN,
    EVENT_MOUSE_UP,
    EVENT_MOUSE_MOVE,
    EVENT_MOUSE_WHEEL,
    EVENT_RESIZE,
    EVENT_FOCUS,
    EVENT_CLOSE,
    EVENT_DIR_CHANGED, // a watched directory changed (`watch`)
    EVENT_WAKEUP,      // the wait timeout the app asked for (app_wait_ms) elapsed
} EventKind;

typedef struct Event {
    EventKind kind;
    u32 mods;          // MOD_* (key, text, mouse)
    Key key;           // EVENT_KEY_DOWN
    u32 scancode;      // EVENT_KEY_DOWN, extended keys have bit 8 set
    b32 repeat;        // EVENT_KEY_DOWN
    // EVENT_TEXT: the character typed. EVENT_KEY_DOWN: the character the key produces with the
    // current Shift / AltGr state, Ctrl and Alt ignored (a dead key gives its spacing accent); 0 if none.
    u32 codepoint;
    MouseButton button;// EVENT_MOUSE_DOWN / UP
    i32 clicks;        // EVENT_MOUSE_DOWN: 1, 2 (double click) or 3 (triple), by the system's double-click time and area
    i32 x, y;          // mouse events, client pixels
    i32 wheel;         // EVENT_MOUSE_WHEEL, 120 per notch, positive = away from user
    i32 width, height; // EVENT_RESIZE, client pixels
    b32 focused;       // EVENT_FOCUS
    OsWatch watch;     // EVENT_DIR_CHANGED
} Event;

typedef struct FrameInput {
    Event *events;
    i32 event_count;
    i32 width, height; // client area in pixels
    f32 dpi_scale;     // 1.0 at 96 DPI
    Arena *scratch;    // reset after every frame
} FrameInput;

// ---------------------------------------------------------------------------
// App entry points, called by the platform layer.

typedef struct App App;
typedef struct Renderer Renderer;

typedef struct AppArgs {
    f32 dpi_scale;
    b32 render_mode_forced; // dev --render-mode: overrides the config's render_mode
    FbRenderMode render_mode;
    String8 file_path; // the first non-flag argument; empty = *scratch*
    i64 goto_line;     // +LINE[:COLUMN], 1-based as in Emacs; 0 = not given
    i64 goto_col;      // visual column, 1-based; 0 = not given
#if TEAL_DEV
    b32 sample;          // --sample (and --smoke): the Phase 2 hand-colored sample instead of the buffer
    String8 config_path; // --config: this file instead of the user's teal.conf
    b32 user_config;     // read the user's teal.conf (off in the smoke and the benches: defaults only)
#endif
} AppArgs;

App *app_create(Arena *perm, AppArgs *args); // NULL on failure (logged)
b32  app_update_and_render(App *app, FrameInput *input, Renderer *r); // false = quit
u32  app_wait_ms(App *app); // how long the platform may block before EVENT_WAKEUP; 0xFFFFFFFF = until an event
i32  app_shutdown(App *app); // leaked resources (font backend references, unreleased buffers, live markers), 0 = clean

#if TEAL_DEV
typedef enum DevProbeKind {
    DEV_PROBE_PIXEL_EQ,       // pixel (x0, y0) == rgb
    DEV_PROBE_REGION_EQ,      // every pixel in [x0, x1) x [y0, y1) == rgb
    DEV_PROBE_REGION_DIFFERS, // at least one pixel in the region != rgb
    DEV_PROBE_CLEARTYPE,      // vertical stem in the region: edge coverage order matches `geometry`
} DevProbeKind;

typedef struct DevProbe {
    DevProbeKind kind;
    i32 x0, y0, x1, y1;
    u32 rgb;      // expected / background color, 0xRRGGBB
    u32 text_rgb; // DEV_PROBE_CLEARTYPE: the stem's color
    FbPixelGeometry geometry;
    const char *what;
} DevProbe;

i32  app_dev_probes(App *app, FrameInput *input, DevProbe *out, i32 cap); // the --sample frame
void app_dev_smoke_buffer_view(App *app); // leaves the sample and shows a known buffer, focus forced off
void app_dev_smoke_region(App *app);      // stage 2: an active region over lines 0-1
i32  app_dev_buffer_probes(App *app, FrameInput *input, DevProbe *out, i32 cap, i32 stage); // 0: hollow cursor, 1: filled after the click, 2: region
void app_dev_smoke_click_point(App *app, FrameInput *input, i32 *x, i32 *y); // the click of stage 1
void app_dev_force_focus(App *app, i32 focused); // -1: follow focus events
b32  app_dev_atlas_has_coverage(App *app);
u8  *app_dev_atlas(App *app, i32 *size); // RGBA8, size x size
i32  test_run(u64 seed, String8 tmp_dir); // --test: headless buffer and file tests; failures, details in the log
void app_dev_goto_line(App *app, i64 line); // point to the start of line (< 0: the last line), recentered
i32  app_dev_key_events(App *app, String8 keys, Event *out, i32 cap); // --keys: tokens to events (bad tokens logged, skipped)
void app_dev_use_config(App *app, String8 path); // switches to this config file and reloads it (as C-c r)
b32  app_dev_visit(App *app, String8 path);      // visits a file in the active view
i32  app_dev_font_setups(App *app);              // font set-ups so far (the startup does exactly one)
i64  app_dev_line_count(App *app);
u64  app_dev_build_us(App *app); // the last frame, from its start to r_end_frame
String8 test_bench_buffer_file(Arena *arena, String8 tmp_dir); // --bench-buffer: generates the 100 MB file once
void test_bench_buffer(String8 path, String8 tmp_dir);         // load, inserts, lookups, save
i32  app_dev_bench_frame(App *app, FrameInput *input, Renderer *r, u64 *build_us, u64 *submit_us); // returns glyphs drawn
#endif

#endif // PLATFORM_H
