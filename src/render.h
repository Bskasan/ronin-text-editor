// render.h — the renderer interface the editor core sees.
// Implemented by render_d3d11.c. Pixel coordinates, origin top-left.

#ifndef RENDER_H
#define RENDER_H

typedef struct Rect {
    f32 x0, y0, x1, y1;
} Rect;

// Bytes in memory order R, G, B, A (matches DXGI_FORMAT_R8G8B8A8_UNORM).
typedef struct Color {
    u8 r, g, b, a;
} Color;

#define COLOR_HEX(rgb) ((Color){ (u8)(((rgb) >> 16) & 0xFF), (u8)(((rgb) >> 8) & 0xFF), (u8)((rgb) & 0xFF), 0xFF })

typedef struct Renderer Renderer;

Renderer *r_create(Arena *perm, void *native_window, i32 width, i32 height); // NULL on failure (reason logged)
u32  r_shutdown(Renderer *r); // dev: number of leaked references / live objects, 0 = clean
void r_resize(Renderer *r, i32 width, i32 height); // 0 x 0 = minimized, frames become no-ops
void r_begin_frame(Renderer *r, Color clear);
void r_push_rect(Renderer *r, Rect rect, Color color);
void r_end_frame(Renderer *r); // flush + Present(1, 0)
b32  r_wants_redraw(Renderer *r); // true after device-loss recovery; cleared by r_begin_frame

#if TEAL_DEV
void r_request_capture(Renderer *r); // the next r_end_frame copies the back buffer before Present
u8  *r_read_capture(Renderer *r, Arena *arena, i32 *width, i32 *height); // tightly packed BGRA8
b32  r_dev_debug_layer_active(Renderer *r);
u32  r_dev_message_count(Renderer *r); // debug-layer messages of severity WARNING or worse
#endif

#endif // RENDER_H
