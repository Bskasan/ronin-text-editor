// png.c — dev-only PNG encoder. 8-bit RGB, filter 0, zlib stream made of stored
// (uncompressed) deflate blocks. Correct, not small or fast.

#if TEAL_DEV

static u32 png_crc_table[256];

static void png_crc_init(void) {
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (i32 k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        png_crc_table[n] = c;
    }
}

static u32 png_crc(u8 *data, i64 len) {
    u32 c = 0xFFFFFFFFu;
    for (i64 i = 0; i < len; i++) c = png_crc_table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static u8 *png_put_u32_be(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
    return p + 4;
}

// Writes length, type and CRC around `data_len` bytes already placed at chunk + 8.
static u8 *png_finish_chunk(u8 *chunk, const char *type, u32 data_len) {
    png_put_u32_be(chunk, data_len);
    memcpy(chunk + 4, type, 4);
    u8 *end = chunk + 8 + data_len;
    return png_put_u32_be(end, png_crc(chunk + 4, 4 + (i64)data_len));
}

// bgra: `height` rows of `width` BGRA8 pixels, `stride` bytes apart. Alpha is dropped.
String8 png_encode_bgra(Arena *arena, u8 *bgra, i32 width, i32 height, i64 stride) {
    png_crc_init();

    i64 row_len = 1 + (i64)width * 3;
    i64 raw_len = row_len * height;
    i64 block_count = MAX((raw_len + 65534) / 65535, 1);
    i64 zlib_len = 2 + block_count * 5 + raw_len + 4;
    i64 total = 8 + (12 + 13) + (12 + zlib_len) + 12;

    u8 *out = PUSH_ARRAY(arena, u8, total);
    u8 *p = out;

    static const u8 signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    memcpy(p, signature, 8);
    p += 8;

    // IHDR
    u8 *chunk = p;
    u8 *d = chunk + 8;
    d = png_put_u32_be(d, (u32)width);
    d = png_put_u32_be(d, (u32)height);
    d[0] = 8; // bit depth
    d[1] = 2; // color type: truecolor
    d[2] = 0; // compression
    d[3] = 0; // filter method
    d[4] = 0; // no interlace
    p = png_finish_chunk(chunk, "IHDR", 13);

    // IDAT: zlib header, stored blocks, Adler-32.
    chunk = p;
    d = chunk + 8;
    *d++ = 0x78;
    *d++ = 0x01;

    u32 adler_a = 1, adler_b = 0;
    i64 block_left = 0;
    i64 raw_left = raw_len;
    for (i32 y = 0; y < height; y++) {
        u8 *src = bgra + stride * y;
        for (i64 i = 0; i < row_len; i++) {
            if (block_left == 0) {
                i64 n = MIN(raw_left, 65535);
                *d++ = (u8)(raw_left <= 65535 ? 1 : 0); // BFINAL, BTYPE = 00
                *d++ = (u8)(n & 0xFF);
                *d++ = (u8)(n >> 8);
                *d++ = (u8)(~n & 0xFF);
                *d++ = (u8)((~n >> 8) & 0xFF);
                block_left = n;
            }
            u8 byte;
            if (i == 0) {
                byte = 0; // filter type: none
            } else {
                i64 px = (i - 1) / 3, ch = (i - 1) % 3;
                byte = src[px * 4 + (2 - ch)]; // BGRA -> RGB
            }
            *d++ = byte;
            adler_a = (adler_a + byte) % 65521;
            adler_b = (adler_b + adler_a) % 65521;
            block_left--;
            raw_left--;
        }
    }
    if (raw_len == 0) { // empty image still needs one final block
        *d++ = 1; *d++ = 0; *d++ = 0; *d++ = 0xFF; *d++ = 0xFF;
    }
    d = png_put_u32_be(d, (adler_b << 16) | adler_a);
    ASSERT(d - (chunk + 8) == zlib_len);
    p = png_finish_chunk(chunk, "IDAT", (u32)zlib_len);

    // IEND
    p = png_finish_chunk(p, "IEND", 0);
    ASSERT(p - out == total);

    return str8(out, total);
}

#endif // TEAL_DEV
