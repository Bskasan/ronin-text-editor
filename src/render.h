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

// Creation is split so the slow device creation can run on a worker thread:
//   r_alloc (main) -> r_create_device (any one thread; no logging, no arenas) -> r_finish_create (main).
Renderer *r_alloc(Arena *perm);
b32  r_create_device(Renderer *r);
b32  r_finish_create(Renderer *r, void *native_window, i32 width, i32 height); // 0 on failure (logged)
u32  r_shutdown(Renderer *r); // dev: number of leaked references / live objects, 0 = clean

void r_resize(Renderer *r, i32 width, i32 height); // 0 x 0 = minimized, frames become no-ops
void r_begin_frame(Renderer *r, Color clear);
void r_push_rect(Renderer *r, Rect rect, Color color);
void r_push_glyph(Renderer *r, Rect dst, Rect atlas_texels, Color color); // dst is 1:1 with the texels
void r_flush(Renderer *r);     // uploads the atlas dirty rect, draws pending quads
void r_end_frame(Renderer *r); // flush + Present
b32  r_wants_redraw(Renderer *r); // true after device-loss recovery; cleared by r_begin_frame

// Glyph atlas: the CPU copy (RGBA8, R/G/B = ClearType coverage) stays owned by the caller and must
// outlive the binding. The renderer uploads the dirty rectangle at every flush, and the whole
// atlas after a device loss.
void r_atlas_bind(Renderer *r, u8 *pixels, i32 width, i32 height);
void r_atlas_mark_dirty(Renderer *r, i32 x0, i32 y0, i32 x1, i32 y1);

#if TEAL_DEV
void r_request_capture(Renderer *r); // the next r_end_frame copies the back buffer before Present
u8  *r_read_capture(Renderer *r, Arena *arena, i32 *width, i32 *height); // tightly packed BGRA8
b32  r_dev_debug_layer_active(Renderer *r);
u32  r_dev_message_count(Renderer *r); // debug-layer messages of severity WARNING or worse
void r_dev_set_present_interval(Renderer *r, u32 interval);
#endif

#endif // RENDER_H
