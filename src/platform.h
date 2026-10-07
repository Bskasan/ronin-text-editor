// platform.h — the only OS interface the editor core sees.
// Implemented by win32_main.c.

#ifndef PLATFORM_H
#define PLATFORM_H

// ---------------------------------------------------------------------------
// OS primitives

void *os_reserve(u64 size);
b32   os_commit(void *ptr, u64 size);
b32   os_write_file(String8 path, String8 data);
void  os_fatal(String8 message); // does not return
u64   os_time_us(void);              // monotonic microseconds
#if TEAL_DEV
void  os_log_write(String8 text);
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
} EventKind;

typedef struct Event {
    EventKind kind;
    u32 mods;          // MOD_* (key, text, mouse)
    Key key;           // EVENT_KEY_DOWN
    u32 scancode;      // EVENT_KEY_DOWN, extended keys have bit 8 set
    b32 repeat;        // EVENT_KEY_DOWN
    u32 codepoint;     // EVENT_TEXT
    MouseButton button;// EVENT_MOUSE_DOWN / UP
    i32 x, y;          // mouse events, client pixels
    i32 wheel;         // EVENT_MOUSE_WHEEL, 120 per notch, positive = away from user
    i32 width, height; // EVENT_RESIZE, client pixels
    b32 focused;       // EVENT_FOCUS
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

typedef struct AppConfig {
    f32 dpi_scale;
    FbRenderMode render_mode;
} AppConfig;

App *app_create(Arena *perm, AppConfig *config); // NULL on failure (logged)
b32  app_update_and_render(App *app, FrameInput *input, Renderer *r); // false = quit
i32  app_shutdown(App *app); // font backend references still held, 0 = clean

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

i32  app_dev_probes(App *app, FrameInput *input, DevProbe *out, i32 cap);
b32  app_dev_atlas_has_coverage(App *app);
u8  *app_dev_atlas(App *app, i32 *size); // RGBA8, size x size
i32  app_dev_bench_frame(App *app, FrameInput *input, Renderer *r, u64 *build_us, u64 *submit_us); // returns glyphs drawn
#endif

#endif // PLATFORM_H
