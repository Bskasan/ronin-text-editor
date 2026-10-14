// make_icon.c: draws ronin's icon and writes it as an .ico (build-time tool, not part of the editor; build.bat
// compiles and runs it on every build: make_icon <out.ico>).
//
// The design: a lowercase "r" (a stem and a shoulder) in the theme's text color and a block cursor after it in the
// cursor color, on a rounded square of the background color. Each size is drawn on its own pixel grid with 8 x 8
// samples a pixel (anti-aliased edges), and stored as a PNG (32-bit RGBA, compressed with a small fixed-Huffman
// deflate). Windows 10 reads PNG images at every icon size; that keeps the resource a few KB instead of ~45 KB of DIBs.
// No C runtime calls: the file is written with Win32.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef unsigned char u8;
typedef unsigned int u32;

#define ICON_BACKGROUND 0x072626u
#define ICON_TEXT 0xd3b58du
#define ICON_CURSOR 0x90ee90u

// The shapes, in units of the icon's side (x right, y down).
static int in_round_square(float x, float y) {
    const float r = 0.2f;
    float cx = x < r ? r : x > 1 - r ? 1 - r : x;
    float cy = y < r ? r : y > 1 - r ? 1 - r : y;
    float dx = x - cx, dy = y - cy;
    return x >= 0 && x <= 1 && y >= 0 && y <= 1 && dx * dx + dy * dy <= r * r;
}

static int in_r(float x, float y) {
    if (x >= 0.22f && x <= 0.36f && y >= 0.36f && y <= 0.80f) return 1; // the stem (its top is the shoulder)
    // The shoulder: a ring around (0.52, 0.60), 0.16 to 0.30 wide, from the stem's left edge over the top down to
    // 30 degrees above the center line on the right.
    float dx = x - 0.52f, dy = 0.60f - y, d2 = dx * dx + dy * dy;
    return d2 >= 0.16f * 0.16f && d2 <= 0.30f * 0.30f && dy >= 0 && dy >= 0.57735f * dx;
}

static int in_cursor(float x, float y) {
    return x >= 0.62f && x <= 0.78f && y >= 0.58f && y <= 0.80f;
}

// size x size RGBA pixels (straight alpha).
static void draw(u8 *rgba, int size) {
    enum { S = 8 };
    for (int py = 0; py < size; py++) {
        for (int px = 0; px < size; px++) {
            u32 r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < S; sy++) {
                for (int sx = 0; sx < S; sx++) {
                    float x = (px + (sx + 0.5f) / S) / size, y = (py + (sy + 0.5f) / S) / size;
                    if (!in_round_square(x, y)) continue;
                    u32 c = in_r(x, y) ? ICON_TEXT : in_cursor(x, y) ? ICON_CURSOR : ICON_BACKGROUND;
                    r += (c >> 16) & 0xFF;
                    g += (c >> 8) & 0xFF;
                    b += c & 0xFF;
                    a++;
                }
            }
            u8 *o = rgba + ((size_t)py * size + px) * 4;
            o[0] = (u8)(a ? (r + a / 2) / a : 0);
            o[1] = (u8)(a ? (g + a / 2) / a : 0);
            o[2] = (u8)(a ? (b + a / 2) / a : 0);
            o[3] = (u8)((a * 255 + S * S / 2) / (S * S));
        }
    }
}

// ---------------------------------------------------------------------------
// Deflate: one fixed-Huffman block; matches only at the distances where flat pictures repeat (the pixel before and
// the pixel above), found greedily.

typedef struct Bits {
    u8 *out;
    size_t len;
    u32 acc;
    int count;
} Bits;

static void bits_put(Bits *b, u32 value, int count) { // LSB first
    b->acc |= value << b->count;
    b->count += count;
    while (b->count >= 8) {
        b->out[b->len++] = (u8)b->acc;
        b->acc >>= 8;
        b->count -= 8;
    }
}

static void bits_code(Bits *b, u32 code, int count) { // a Huffman code: MSB first
    u32 rev = 0;
    for (int i = 0; i < count; i++) rev |= ((code >> i) & 1) << (count - 1 - i);
    bits_put(b, rev, count);
}

static void put_symbol(Bits *b, u32 s) {
    if (s < 144) bits_code(b, 0x30 + s, 8);
    else if (s < 256) bits_code(b, 0x190 + s - 144, 9);
    else if (s < 280) bits_code(b, s - 256, 7);
    else bits_code(b, 0xC0 + s - 280, 8);
}

static const unsigned short len_base[29] = { 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                                             35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const u8 len_extra[29] = { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const unsigned short dist_base[30] = { 1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385,
                                              513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const u8 dist_extra[30] = { 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void put_match(Bits *b, u32 length, u32 distance) {
    int i = 28;
    while (len_base[i] > length) i--;
    put_symbol(b, 257 + (u32)i);
    bits_put(b, length - len_base[i], len_extra[i]);
    int d = 29;
    while (dist_base[d] > distance) d--;
    bits_code(b, (u32)d, 5);
    bits_put(b, distance - dist_base[d], dist_extra[d]);
}

// zlib stream of `data` into `out` (room for len + len / 8 + 64 bytes); returns its length.
static size_t zlib(const u8 *data, size_t len, u8 *out, size_t row) {
    Bits b = { out, 0, 0, 0 };
    out[b.len++] = 0x78;
    out[b.len++] = 0x01;
    bits_put(&b, 1, 1); // the last block
    bits_put(&b, 1, 2); // fixed Huffman codes
    size_t dists[2] = { 4, row };
    for (size_t i = 0; i < len;) {
        size_t best = 0, best_dist = 0;
        for (int k = 0; k < 2; k++) {
            size_t d = dists[k];
            if (d > i || d > 32768) continue;
            size_t n = 0;
            while (n < 258 && i + n < len && data[i + n] == data[i + n - d]) n++;
            if (n > best) {
                best = n;
                best_dist = d;
            }
        }
        if (best >= 3) {
            put_match(&b, (u32)best, (u32)best_dist);
            i += best;
        } else {
            put_symbol(&b, data[i++]);
        }
    }
    put_symbol(&b, 256);
    if (b.count) bits_put(&b, 0, 8 - b.count);
    u32 s1 = 1, s2 = 0;
    for (size_t i = 0; i < len; i++) {
        s1 = (s1 + data[i]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    u32 adler = (s2 << 16) | s1;
    for (int k = 3; k >= 0; k--) out[b.len++] = (u8)(adler >> (k * 8));
    return b.len;
}

// ---------------------------------------------------------------------------
// PNG

static u32 crc_table[256];

static u32 crc(const u8 *p, size_t n) {
    u32 c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static u8 *put_be(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24);
    p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);
    p[3] = (u8)v;
    return p + 4;
}

static u8 *chunk(u8 *p, const char *type, const u8 *data, u32 len) {
    put_be(p, len);
    p[4] = (u8)type[0];
    p[5] = (u8)type[1];
    p[6] = (u8)type[2];
    p[7] = (u8)type[3];
    for (u32 i = 0; i < len; i++) p[8 + i] = data[i];
    put_be(p + 8 + len, crc(p + 4, 4 + (size_t)len));
    return p + 12 + len;
}

#define MAX_SIZE 256
static u8 pixels[MAX_SIZE * MAX_SIZE * 4];
static u8 raw[MAX_SIZE * (MAX_SIZE * 4 + 1)];
static u8 packed[sizeof(raw) + sizeof(raw) / 8 + 64];

// The PNG of one size into `out`; returns its length.
static size_t png(int size, u8 *out) {
    draw(pixels, size);
    size_t row = (size_t)size * 4 + 1, n = 0;
    for (int y = 0; y < size; y++) {
        raw[n++] = 0; // filter: none
        for (size_t x = 0; x < (size_t)size * 4; x++) raw[n++] = pixels[(size_t)y * size * 4 + x];
    }
    size_t z = zlib(raw, n, packed, row);
    static const u8 signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
    u8 *p = out;
    for (int i = 0; i < 8; i++) *p++ = signature[i];
    u8 ihdr[13];
    put_be(ihdr, (u32)size);
    put_be(ihdr + 4, (u32)size);
    ihdr[8] = 8;  // bits per channel
    ihdr[9] = 6;  // RGBA
    ihdr[10] = 0; // deflate
    ihdr[11] = 0; // adaptive filters
    ihdr[12] = 0; // no interlace
    p = chunk(p, "IHDR", ihdr, 13);
    p = chunk(p, "IDAT", packed, (u32)z);
    p = chunk(p, "IEND", NULL, 0);
    return (size_t)(p - out);
}

static u8 ico[1 << 20];

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    for (u32 n = 0; n < 256; n++) {
        u32 c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    static const int sizes[] = { 16, 20, 24, 32, 40, 48, 64, 256 };
    enum { COUNT = sizeof(sizes) / sizeof(sizes[0]) };
    // ICONDIR, then one ICONDIRENTRY per image, then the images.
    size_t at = 6 + 16 * COUNT;
    ico[0] = 0;
    ico[1] = 0;
    ico[2] = 1; // icon
    ico[3] = 0;
    ico[4] = COUNT;
    ico[5] = 0;
    for (int i = 0; i < COUNT; i++) {
        size_t len = png(sizes[i], ico + at);
        u8 *e = ico + 6 + 16 * i;
        e[0] = (u8)(sizes[i] & 0xFF); // 256 is stored as 0
        e[1] = (u8)(sizes[i] & 0xFF);
        e[2] = 0; // no palette
        e[3] = 0;
        e[4] = 1; // planes
        e[5] = 0;
        e[6] = 32; // bits per pixel
        e[7] = 0;
        for (int k = 0; k < 4; k++) e[8 + k] = (u8)(len >> (8 * k));
        for (int k = 0; k < 4; k++) e[12 + k] = (u8)(at >> (8 * k));
        at += len;
    }
    HANDLE f = CreateFileA(argv[1], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 1;
    DWORD written = 0;
    BOOL ok = WriteFile(f, ico, (DWORD)at, &written, NULL) && written == at;
    CloseHandle(f);
    return ok ? 0 : 1;
}
