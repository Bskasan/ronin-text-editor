// font_backend.h — glyph rasterization interface, implemented by win32_dwrite.cpp (DirectWrite).
// Plain C, no Win32 or DirectWrite types. All caching, packing and atlas logic lives in font.c.

#ifndef FONT_BACKEND_H
#define FONT_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum FbRenderMode {
    FB_RENDER_GDI_CLASSIC,
    FB_RENDER_NATURAL,
    FB_RENDER_NATURAL_SYMMETRIC,
} FbRenderMode;

typedef enum FbPixelGeometry {
    FB_PIXELS_FLAT,
    FB_PIXELS_RGB,
    FB_PIXELS_BGR,
} FbPixelGeometry;

typedef struct FbMetrics {
    f32 ascent, descent, line_gap; // pixels
    f32 advance;                   // pixels, of '0' (the font is monospace)
} FbMetrics;

// A rasterized glyph: white-on-black ClearType coverage, BGRX bytes, top-down.
// The black box is relative to the baseline origin (y grows downward). Empty glyphs have x0 == x1.
typedef struct FbGlyph {
    i32 x0, y0, x1, y1;
    const u8 *bgrx; // first pixel of the black box
    i64 stride;     // bytes per row
} FbGlyph;

// Owned by the caller (C side). The pointers are DirectWrite / GDI objects, opaque here.
typedef struct FontBackend {
    void *factory;    // IDWriteFactory
    void *interop;    // IDWriteGdiInterop
    void *face;       // IDWriteFontFace
    void *params;     // IDWriteRenderingParams (custom)
    void *target;     // IDWriteBitmapRenderTarget
    u8 *dib_bits;     // the target's DIB, top-down
    i64 dib_stride;
    i32 target_w, target_h;
    i32 origin_x, origin_y; // baseline origin inside the target
    f32 pixel_size;
    FbRenderMode mode;
    FbPixelGeometry pixel_geometry;
    i32 live_refs;    // references we hold; must be 0 after fb_close
} FontBackend;

// Opens the first available family (UTF-16, NUL-terminated) at `pixel_size` pixels per em.
// Closes whatever was open before. Returns 0 on failure.
b32  fb_open(FontBackend *fb, const u16 *family, const u16 *fallback_family, f32 pixel_size,
             FbRenderMode mode, FbMetrics *out_metrics);
u16  fb_glyph_index(FontBackend *fb, u32 codepoint); // 0 = the font has no glyph
b32  fb_rasterize(FontBackend *fb, u16 glyph, FbGlyph *out); // valid until the next call
void fb_close(FontBackend *fb);

#ifdef __cplusplus
}
#endif

#endif // FONT_BACKEND_H
