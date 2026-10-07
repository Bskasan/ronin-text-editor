// font.h — monospace cell-grid text: metrics, glyph atlas (CPU side), glyph cache, text drawing.
// Plain C. Rasterizes through font_backend.h, draws through render.h; never touches D3D.

#ifndef FONT_H
#define FONT_H

#define FONT_SIZE_PT 12
#define FONT_FAMILY          L"Consolas"
#define FONT_FAMILY_FALLBACK L"Courier New"

typedef struct FontGlyph {
    i16 x, y; // quad offset from the cell's top-left corner, pixels
    u16 u, v; // atlas texel position
    u16 w, h; // 0 = nothing to draw (space)
} FontGlyph;

typedef struct FontSlot {
    u32 codepoint; // FONT_SLOT_EMPTY when unused
    FontGlyph glyph;
} FontSlot;

typedef struct Font {
    FontBackend backend;
    FbRenderMode mode;
    f32 dpi_scale;
    f32 pixel_size;
    Arena arena; // atlas and hash table; reset when the DPI changes

    // Metrics, integers in physical pixels.
    i32 cell_w;
    i32 line_h;
    i32 baseline; // from the top of the line

    // Atlas: square, RGBA8 (R, G, B = ClearType coverage), shelf-packed.
    u8 *atlas;
    i32 atlas_size;
    i32 shelf_x, shelf_y, shelf_h;
    b32 needs_bind; // the renderer has not seen this atlas allocation yet

    // Glyph cache.
    FontGlyph ascii[128]; // 32..126, pre-rasterized
    FontGlyph missing;    // hollow box
    FontSlot *slots;      // open addressing, linear probing
    u32 slot_bits;
    u32 slot_count;
    u32 reset_count;
} Font;

Font *font_create(Arena *perm, f32 dpi_scale, FbRenderMode mode); // NULL on failure (logged)
i32   font_shutdown(Font *f); // backend references still held, 0 = clean
void  font_frame_begin(Font *f, Renderer *r, f32 dpi_scale); // DPI change, atlas (re)binding
i32   font_draw_text(Font *f, Renderer *r, i32 x, i32 y, String8 text, Color color); // returns x after the last cell

#if TEAL_DEV
b32   font_dev_atlas_has_coverage(Font *f);
#endif

#endif // FONT_H
