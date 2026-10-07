// font.c — see font.h.

#define FONT_SLOT_EMPTY 0xFFFFFFFFu
#define FONT_ATLAS_MIN_GLYPHS 2000
#define FONT_ATLAS_MAX_SIZE 4096
#define FONT_ARENA_RESERVE MB(128)

// ---------------------------------------------------------------------------
// Atlas

// Shelf packer with 1 px of empty padding to the right of and below every glyph
// (and a 1 px border at the top-left of the atlas), so sampling never bleeds.
static b32 font_pack(Font *f, i32 w, i32 h, i32 *u, i32 *v) {
    if (f->shelf_x + w + 1 > f->atlas_size) {
        f->shelf_y += f->shelf_h;
        f->shelf_x = 1;
        f->shelf_h = 0;
    }
    if (w + 2 > f->atlas_size || f->shelf_y + h + 1 > f->atlas_size) return 0;
    *u = f->shelf_x;
    *v = f->shelf_y;
    f->shelf_x += w + 1;
    f->shelf_h = MAX(f->shelf_h, h + 1);
    return 1;
}

// Rasterizes one glyph into the atlas. Returns 0 only when the atlas is full.
static b32 font_make_glyph(Font *f, Renderer *r, u16 index, FontGlyph *out) {
    memset(out, 0, sizeof(*out));
    FbGlyph g;
    if (!fb_rasterize(&f->backend, index, &g)) {
        *out = f->missing;
        return 1;
    }
    i32 w = g.x1 - g.x0, h = g.y1 - g.y0;
    if (w <= 0 || h <= 0) return 1; // empty glyph: no quad

    i32 u, v;
    if (!font_pack(f, w, h, &u, &v)) return 0;
    for (i32 y = 0; y < h; y++) {
        const u8 *src = g.bgrx + y * g.stride;
        u8 *dst = f->atlas + ((i64)(v + y) * f->atlas_size + u) * 4;
        for (i32 x = 0; x < w; x++) {
            // DIB bytes are B, G, R, X; the atlas is R, G, B, A so red coverage weights red.
            dst[0] = src[2];
            dst[1] = src[1];
            dst[2] = src[0];
            dst[3] = 255;
            src += 4;
            dst += 4;
        }
    }
    if (r) r_atlas_mark_dirty(r, u, v, u + w, v + h);

    out->x = (i16)g.x0;
    out->y = (i16)(f->baseline + g.y0);
    out->u = (u16)u;
    out->v = (u16)v;
    out->w = (u16)w;
    out->h = (u16)h;
    return 1;
}

// The missing-glyph box: a hollow rectangle from roughly cap height to the baseline.
static void font_make_missing(Font *f) {
    i32 t = MAX((i32)(f->dpi_scale + 0.5f), 1);
    i32 w = MAX(f->cell_w - 2, 2 * t + 1);
    i32 top = MAX(f->baseline - (i32)(f->pixel_size * 0.7f + 0.5f), 0);
    i32 h = MAX(f->baseline - top, 2 * t + 1);
    i32 u, v;
    memset(&f->missing, 0, sizeof(f->missing));
    if (!font_pack(f, w, h, &u, &v)) return;
    for (i32 y = 0; y < h; y++) {
        for (i32 x = 0; x < w; x++) {
            if (x >= t && x < w - t && y >= t && y < h - t) continue;
            u8 *px = f->atlas + ((i64)(v + y) * f->atlas_size + u + x) * 4;
            px[0] = px[1] = px[2] = px[3] = 255;
        }
    }
    f->missing.x = 1;
    f->missing.y = (i16)top;
    f->missing.u = (u16)u;
    f->missing.v = (u16)v;
    f->missing.w = (u16)w;
    f->missing.h = (u16)h;
}

// Clears the atlas and every cache, then re-rasterizes the missing box and ASCII.
// With a renderer, pending quads are flushed first: they still reference the old atlas.
static void font_reset_cache(Font *f, Renderer *r) {
    if (r) r_flush(r);
    memset(f->atlas, 0, (size_t)f->atlas_size * f->atlas_size * 4);
    memset(f->slots, 0xFF, sizeof(FontSlot) << f->slot_bits); // codepoint = FONT_SLOT_EMPTY
    f->slot_count = 0;
    f->shelf_x = f->shelf_y = 1;
    f->shelf_h = 0;
    f->reset_count++;

    font_make_missing(f);
    memset(f->ascii, 0, sizeof(f->ascii));
    for (u32 c = 32; c < 127; c++) {
        u16 index = fb_glyph_index(&f->backend, c);
        if (!index) f->ascii[c] = f->missing;
        else if (!font_make_glyph(f, NULL, index, &f->ascii[c])) f->ascii[c] = f->missing; // cannot happen at 2000-glyph capacity
    }
    if (r) r_atlas_mark_dirty(r, 0, 0, f->atlas_size, f->atlas_size);
}

// Opens the font at the current DPI scale and (re)allocates the atlas and cache.
static b32 font_setup(Font *f) {
    arena_reset(&f->arena);
    f->pixel_size = (f32)FONT_SIZE_PT * 96.0f / 72.0f * f->dpi_scale;

#if TEAL_DEV
    u64 t0 = os_time_us();
#endif
    FbMetrics m;
    if (!fb_open(&f->backend, (const u16 *)FONT_FAMILY, (const u16 *)FONT_FAMILY_FALLBACK, f->pixel_size, f->mode, &m)) {
        LOG("font: fb_open failed");
        return 0;
    }
#if TEAL_DEV
    u64 t1 = os_time_us();
#endif

    f->cell_w = MAX((i32)(m.advance + 0.5f), 1);
    f32 height = (m.ascent + m.descent + m.line_gap) * (f32)FONT_LINE_HEIGHT_PERCENT / 100.0f;
    // The baseline below centers the glyphs, so extra height splits evenly above and below.
    f->line_h = MAX((i32)height + ((f32)(i32)height < height ? 1 : 0), 1);
    f->baseline = (i32)(m.ascent + (f->line_h - (m.ascent + m.descent)) * 0.5f + 0.5f);

    // Atlas big enough for FONT_ATLAS_MIN_GLYPHS glyph boxes, with slack for shelf waste.
    u64 per_glyph = (u64)(f->cell_w + 3) * (u64)(f->line_h + 3);
    u64 needed = per_glyph * FONT_ATLAS_MIN_GLYPHS * 5 / 4;
    i32 size = 256;
    while ((u64)size * size < needed && size < FONT_ATLAS_MAX_SIZE) size *= 2;
    f->atlas_size = size;
    f->atlas = PUSH_ARRAY(&f->arena, u8, (i64)size * size * 4);

    // Hash table: at least twice the number of glyphs the atlas can hold.
    u64 capacity = (u64)size * size / per_glyph * 2;
    f->slot_bits = 8;
    while (((u64)1 << f->slot_bits) < capacity) f->slot_bits++;
    f->slots = PUSH_ARRAY(&f->arena, FontSlot, (u64)1 << f->slot_bits);

    f->reset_count = 0;
    font_reset_cache(f, NULL);
    f->needs_bind = 1;

#if TEAL_DEV
    u64 t2 = os_time_us();
    LOG("font: %s %d px (scale %d%%, mode %d): cell %dx%d, baseline %d, atlas %dx%d, %u hash slots",
        "Consolas", (i32)(f->pixel_size + 0.5f), (i32)(f->dpi_scale * 100.0f + 0.5f), (i32)f->mode,
        f->cell_w, f->line_h, f->baseline, size, size, 1u << f->slot_bits);
    LOG("font: DirectWrite init %D us, ASCII pre-rasterization %D us", (i64)(t1 - t0), (i64)(t2 - t1));
#endif
    return 1;
}

Font *font_create(Arena *perm, f32 dpi_scale, FbRenderMode mode) {
    Font *f = PUSH_STRUCT(perm, Font);
    f->arena = arena_create(FONT_ARENA_RESERVE);
    f->dpi_scale = dpi_scale;
    f->mode = mode;
    if (!font_setup(f)) {
        fb_close(&f->backend);
        return NULL;
    }
    return f;
}

i32 font_shutdown(Font *f) {
    fb_close(&f->backend);
    return f->backend.live_refs;
}

void font_frame_begin(Font *f, Renderer *r, f32 dpi_scale) {
    if (dpi_scale != f->dpi_scale) {
        f->dpi_scale = dpi_scale;
        if (!font_setup(f)) os_fatal(STR8_LIT("Could not open the font at the new DPI."));
    }
    if (f->needs_bind) {
        r_atlas_bind(r, f->atlas, f->atlas_size, f->atlas_size);
        f->needs_bind = 0;
    }
}

// ---------------------------------------------------------------------------
// Glyph cache

static FontGlyph font_lookup(Font *f, Renderer *r, u32 codepoint) {
    for (i32 attempt = 0; attempt < 2; attempt++) {
        u32 mask = (1u << f->slot_bits) - 1;
        u32 i = (codepoint * 0x9E3779B1u) >> (32 - f->slot_bits);
        while (f->slots[i].codepoint != FONT_SLOT_EMPTY) {
            if (f->slots[i].codepoint == codepoint) return f->slots[i].glyph;
            i = (i + 1) & mask;
        }

        // Not cached. Missing glyphs share one atlas entry but still take a slot, so the
        // table can fill up without the atlas ever filling: keep the load factor <= 70%,
        // which also guarantees the probe loop above terminates.
        if ((u64)(f->slot_count + 1) * 10 > (u64)(mask + 1) * 7) {
            font_reset_cache(f, r);
            continue;
        }
        FontGlyph g;
        u16 index = fb_glyph_index(&f->backend, codepoint);
        if (!index) {
            g = f->missing;
        } else if (!font_make_glyph(f, r, index, &g)) {
            if (attempt == 0) { // atlas full: start over with an empty atlas
                font_reset_cache(f, r);
                continue;
            }
            g = f->missing; // larger than an empty atlas; cannot happen for a text font
        }
        f->slots[i].codepoint = codepoint;
        f->slots[i].glyph = g;
        f->slot_count++;
        return g;
    }
    return f->missing;
}

i32 font_draw_text(Font *f, Renderer *r, i32 x, i32 y, String8 text, Color color) {
    for (i64 i = 0; i < text.len;) {
        u8 b = text.data[i];
        FontGlyph g;
        if (b >= 32 && b < 127) { // ASCII: table lookup
            g = f->ascii[b];
            i++;
        } else {
            i64 advance;
            u32 cp = utf8_decode(text.data + i, text.len - i, &advance);
            // Invalid bytes decode as U+FFFD with advance 1; they get the missing-glyph box.
            g = (cp == UTF_REPLACEMENT && advance == 1) ? f->missing : font_lookup(f, r, cp);
            i += advance;
        }
        if (g.w) {
            Rect dst = { (f32)(x + g.x), (f32)(y + g.y), (f32)(x + g.x + g.w), (f32)(y + g.y + g.h) };
            Rect src = { (f32)g.u, (f32)g.v, (f32)(g.u + g.w), (f32)(g.v + g.h) };
            r_push_glyph(r, dst, src, color);
        }
        x += f->cell_w;
    }
    return x;
}

// ---------------------------------------------------------------------------
// Dev

#if TEAL_DEV
b32 font_dev_atlas_has_coverage(Font *f) {
    i64 n = (i64)f->atlas_size * f->atlas_size * 4;
    for (i64 i = 0; i < n; i += 4) {
        if (f->atlas[i] | f->atlas[i + 1] | f->atlas[i + 2]) return 1;
    }
    return 0;
}
#endif
