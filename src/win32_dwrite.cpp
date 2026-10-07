// win32_dwrite.cpp — DirectWrite glyph rasterization behind font_backend.h.
//
// The only C++ file in teal, because the SDK's dwrite.h cannot be consumed from C.
// Written in C style: no exceptions (/EH off), no RTTI (/GR-), no STL, no new/delete,
// no classes of our own, no static objects with constructors, no CRT calls.
// Nothing but DirectWrite (and the few GDI calls needed to reach the DIB).

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwrite.h>

#include "base.h"
#include "font_backend.h"

#define FB_RELEASE(fb, p) do { if (p) { (p)->Release(); (p) = NULL; (fb)->live_refs--; } } while (0)

// The opaque void* fields are typed here, in one place.
#define FB_FACTORY(fb) ((IDWriteFactory *)(fb)->factory)
#define FB_INTEROP(fb) ((IDWriteGdiInterop *)(fb)->interop)
#define FB_FACE(fb)    ((IDWriteFontFace *)(fb)->face)
#define FB_PARAMS(fb)  ((IDWriteRenderingParams *)(fb)->params)
#define FB_TARGET(fb)  ((IDWriteBitmapRenderTarget *)(fb)->target)

static i32 fb_ceil(f32 x) {
    i32 i = (i32)x;
    return (f32)i < x ? i + 1 : i;
}

static DWRITE_MEASURING_MODE fb_measuring_mode(FbRenderMode mode) {
    return mode == FB_RENDER_GDI_CLASSIC ? DWRITE_MEASURING_MODE_GDI_CLASSIC : DWRITE_MEASURING_MODE_NATURAL;
}

extern "C" void fb_close(FontBackend *fb) {
    IDWriteBitmapRenderTarget *target = FB_TARGET(fb);
    IDWriteRenderingParams *params = FB_PARAMS(fb);
    IDWriteFontFace *face = FB_FACE(fb);
    IDWriteGdiInterop *interop = FB_INTEROP(fb);
    IDWriteFactory *factory = FB_FACTORY(fb);
    FB_RELEASE(fb, target);
    FB_RELEASE(fb, params);
    FB_RELEASE(fb, face);
    FB_RELEASE(fb, interop);
    FB_RELEASE(fb, factory);
    fb->target = fb->params = fb->face = fb->interop = fb->factory = NULL;
    fb->dib_bits = NULL;
}

// Finds the family in the system collection and creates a regular / normal / normal face.
static IDWriteFontFace *fb_create_face(FontBackend *fb, IDWriteFactory *factory, const u16 *family, const u16 *fallback,
                                       b32 *used_fallback) {
    IDWriteFontCollection *collection = NULL;
    IDWriteFontFamily *font_family = NULL;
    IDWriteFont *font = NULL;
    IDWriteFontFace *face = NULL;

    if (FAILED(factory->GetSystemFontCollection(&collection, FALSE))) return NULL;
    fb->live_refs++;

    UINT32 index = 0;
    BOOL exists = FALSE;
    if (family[0]) collection->FindFamilyName((const WCHAR *)family, &index, &exists);
    *used_fallback = !exists;
    if (!exists && fallback) collection->FindFamilyName((const WCHAR *)fallback, &index, &exists);

    if (exists && SUCCEEDED(collection->GetFontFamily(index, &font_family))) {
        fb->live_refs++;
        if (SUCCEEDED(font_family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STRETCH_NORMAL,
                                                        DWRITE_FONT_STYLE_NORMAL, &font))) {
            fb->live_refs++;
            if (SUCCEEDED(font->CreateFontFace(&face))) fb->live_refs++;
        }
    }
    FB_RELEASE(fb, font);
    FB_RELEASE(fb, font_family);
    FB_RELEASE(fb, collection);
    return face;
}

extern "C" b32 fb_open(FontBackend *fb, const u16 *family, const u16 *fallback_family, f32 pixel_size,
                       FbRenderMode mode, FbMetrics *out) {
    fb_close(fb);
    fb->pixel_size = pixel_size;
    fb->mode = mode;

    IDWriteFactory *factory = NULL;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&factory))) return 0;
    fb->live_refs++;
    fb->factory = factory;

    out->used_fallback = 0;
    IDWriteFontFace *face = fb_create_face(fb, factory, family, fallback_family, &out->used_fallback);
    if (!face) return 0;
    fb->face = face;

    // Metrics, measured the way the chosen rendering mode lays glyphs out.
    UINT32 zero_codepoint = '0';
    UINT16 zero_glyph = 0;
    face->GetGlyphIndices(&zero_codepoint, 1, &zero_glyph);
    DWRITE_FONT_METRICS fm;
    DWRITE_GLYPH_METRICS gm;
    if (mode == FB_RENDER_GDI_CLASSIC) {
        if (FAILED(face->GetGdiCompatibleMetrics(pixel_size, 1.0f, NULL, &fm))) return 0;
        if (FAILED(face->GetGdiCompatibleGlyphMetrics(pixel_size, 1.0f, NULL, FALSE, &zero_glyph, 1, &gm, FALSE))) return 0;
    } else {
        face->GetMetrics(&fm);
        if (FAILED(face->GetDesignGlyphMetrics(&zero_glyph, 1, &gm, FALSE))) return 0;
    }
    f32 scale = pixel_size / (f32)fm.designUnitsPerEm;
    out->ascent = fm.ascent * scale;
    out->descent = fm.descent * scale;
    out->line_gap = fm.lineGap * scale;
    out->advance = gm.advanceWidth * scale;

    // Rendering params: the monitor defaults with the rendering mode overridden.
    IDWriteRenderingParams *defaults = NULL;
    if (FAILED(factory->CreateRenderingParams(&defaults))) return 0;
    fb->live_refs++;
    static const DWRITE_RENDERING_MODE modes[] = {
        DWRITE_RENDERING_MODE_GDI_CLASSIC,
        DWRITE_RENDERING_MODE_NATURAL,
        DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
    };
    DWRITE_PIXEL_GEOMETRY geometry = defaults->GetPixelGeometry();
    IDWriteRenderingParams *params = NULL;
    HRESULT hr = factory->CreateCustomRenderingParams(defaults->GetGamma(), defaults->GetEnhancedContrast(),
                                                      defaults->GetClearTypeLevel(), geometry, modes[mode], &params);
    FB_RELEASE(fb, defaults);
    if (FAILED(hr)) return 0;
    fb->live_refs++;
    fb->params = params;
    fb->pixel_geometry = geometry == DWRITE_PIXEL_GEOMETRY_RGB ? FB_PIXELS_RGB
                       : geometry == DWRITE_PIXEL_GEOMETRY_BGR ? FB_PIXELS_BGR
                       : FB_PIXELS_FLAT;

    // A GDI bitmap render target big enough for any glyph of this size, at 1 pixel per DIP
    // (we pass physical pixel sizes ourselves).
    IDWriteGdiInterop *interop = NULL;
    if (FAILED(factory->GetGdiInterop(&interop))) return 0;
    fb->live_refs++;
    fb->interop = interop;

    i32 em = MAX(fb_ceil(pixel_size), 1);
    fb->target_w = 4 * em;
    fb->target_h = 3 * em;
    fb->origin_x = em;
    fb->origin_y = 2 * em;
    IDWriteBitmapRenderTarget *target = NULL;
    if (FAILED(interop->CreateBitmapRenderTarget(NULL, (UINT32)fb->target_w, (UINT32)fb->target_h, &target))) return 0;
    fb->live_refs++;
    fb->target = target;
    target->SetPixelsPerDip(1.0f);

    DIBSECTION dib;
    HBITMAP bitmap = (HBITMAP)GetCurrentObject(target->GetMemoryDC(), OBJ_BITMAP);
    if (!bitmap || GetObjectW(bitmap, sizeof(dib), &dib) != sizeof(dib) || !dib.dsBm.bmBits || dib.dsBm.bmBitsPixel != 32) return 0;
    fb->dib_stride = dib.dsBm.bmWidthBytes;
    fb->dib_bits = (u8 *)dib.dsBm.bmBits;

    // Row order: GetObject does not reliably report the sign of biHeight for this DIB, so ask
    // GDI directly. Light the top-left pixel and see whether it lands in the first row in memory.
    HDC dc = target->GetMemoryDC();
    GdiFlush();
    memset(fb->dib_bits, 0, (size_t)fb->dib_stride * fb->target_h);
    SetPixelV(dc, 0, 0, RGB(255, 255, 255));
    GdiFlush();
    b32 top_down = fb->dib_bits[0] != 0;
    SetPixelV(dc, 0, 0, RGB(0, 0, 0));
    GdiFlush();
    if (!top_down) { // bottom-up: walk it top-down with a negative stride
        fb->dib_bits += (i64)(fb->target_h - 1) * fb->dib_stride;
        fb->dib_stride = -fb->dib_stride;
    }
    return 1;
}

extern "C" u16 fb_glyph_index(FontBackend *fb, u32 codepoint) {
    UINT32 cp = codepoint;
    UINT16 glyph = 0;
    if (FAILED(FB_FACE(fb)->GetGlyphIndices(&cp, 1, &glyph))) return 0;
    return glyph;
}

extern "C" b32 fb_rasterize(FontBackend *fb, u16 glyph, FbGlyph *out) {
    for (i32 y = 0; y < fb->target_h; y++) memset(fb->dib_bits + y * fb->dib_stride, 0, (size_t)fb->target_w * 4);

    UINT16 index = glyph;
    FLOAT advance = 0;
    DWRITE_GLYPH_RUN run = {};
    run.fontFace = FB_FACE(fb);
    run.fontEmSize = fb->pixel_size;
    run.glyphCount = 1;
    run.glyphIndices = &index;
    run.glyphAdvances = &advance;

    RECT box = {};
    HRESULT hr = FB_TARGET(fb)->DrawGlyphRun((FLOAT)fb->origin_x, (FLOAT)fb->origin_y, fb_measuring_mode(fb->mode),
                                             &run, FB_PARAMS(fb), RGB(255, 255, 255), &box);
    if (FAILED(hr)) return 0;

    box.left = CLAMP(box.left, 0, fb->target_w);
    box.right = CLAMP(box.right, box.left, fb->target_w);
    box.top = CLAMP(box.top, 0, fb->target_h);
    box.bottom = CLAMP(box.bottom, box.top, fb->target_h);
    if (box.left == box.right || box.top == box.bottom) box.left = box.right = box.top = box.bottom = 0;

    out->x0 = box.left - fb->origin_x;
    out->x1 = box.right - fb->origin_x;
    out->y0 = box.top - fb->origin_y;
    out->y1 = box.bottom - fb->origin_y;
    out->stride = fb->dib_stride;
    out->bgrx = fb->dib_bits + box.top * fb->dib_stride + box.left * 4;
    return 1;
}
