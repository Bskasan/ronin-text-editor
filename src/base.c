// base.c — see base.h.

// ---------------------------------------------------------------------------
// Arena

Arena arena_create(u64 reserve_size) {
    Arena arena = {0};
    reserve_size = ALIGN_UP_POW2(reserve_size, ARENA_COMMIT_GRANULARITY);
    arena.base = (u8 *)os_reserve(reserve_size);
    if (!arena.base) os_fatal(STR8_LIT("Out of address space (arena reserve failed)."));
    arena.reserved = reserve_size;
    return arena;
}

void *arena_push(Arena *arena, u64 size, u64 align) {
    u64 start = ALIGN_UP_POW2(arena->pos, align);
    u64 end = start + size;
    if (end > arena->reserved) os_fatal(STR8_LIT("Arena reservation exhausted."));
    if (end > arena->committed) {
        u64 new_committed = MIN(ALIGN_UP_POW2(end, ARENA_COMMIT_GRANULARITY), arena->reserved);
        if (!os_commit(arena->base + arena->committed, new_committed - arena->committed)) {
            os_fatal(STR8_LIT("Out of memory (arena commit failed)."));
        }
        arena->committed = new_committed;
    }
    arena->pos = end;
    u8 *result = arena->base + start;
    memset(result, 0, size);
    return result;
}

u64 arena_pos(Arena *arena) {
    return arena->pos;
}

void arena_pop_to(Arena *arena, u64 pos) {
    ASSERT(pos <= arena->pos);
    arena->pos = pos;
}

void arena_reset(Arena *arena) {
    arena->pos = 0;
}

// ---------------------------------------------------------------------------
// String8

String8 str8(u8 *data, i64 len) {
    String8 s = { data, len };
    return s;
}

String8 str8_cstr(const char *s) {
    i64 len = 0;
    while (s[len]) len++;
    return str8((u8 *)s, len);
}

b32 str8_equal(String8 a, String8 b) {
    if (a.len != b.len) return 0;
    for (i64 i = 0; i < a.len; i++) {
        if (a.data[i] != b.data[i]) return 0;
    }
    return 1;
}

String8 str8_copy(Arena *arena, String8 s) {
    u8 *data = PUSH_ARRAY(arena, u8, s.len);
    memcpy(data, s.data, (size_t)s.len);
    return str8(data, s.len);
}

// ---------------------------------------------------------------------------
// UTF-8 / UTF-16

u32 utf8_decode(u8 *s, i64 len, i64 *advance) {
    *advance = 1;
    if (len <= 0) return UTF_REPLACEMENT;
    u8 b0 = s[0];
    if (b0 < 0x80) return b0;

    i64 n;
    u32 cp, min;
    if ((b0 & 0xE0) == 0xC0)      { n = 2; cp = b0 & 0x1F; min = 0x80; }
    else if ((b0 & 0xF0) == 0xE0) { n = 3; cp = b0 & 0x0F; min = 0x800; }
    else if ((b0 & 0xF8) == 0xF0) { n = 4; cp = b0 & 0x07; min = 0x10000; }
    else return UTF_REPLACEMENT;

    if (len < n) return UTF_REPLACEMENT;
    for (i64 i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) return UTF_REPLACEMENT;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return UTF_REPLACEMENT;
    *advance = n;
    return cp;
}

i64 utf8_encode(u32 cp, u8 *out) {
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = UTF_REPLACEMENT;
    if (cp < 0x80) {
        out[0] = (u8)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (u8)(0xC0 | (cp >> 6));
        out[1] = (u8)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (u8)(0xE0 | (cp >> 12));
        out[1] = (u8)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (u8)(0xF0 | (cp >> 18));
    out[1] = (u8)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (u8)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (u8)(0x80 | (cp & 0x3F));
    return 4;
}

String16 str16_from_str8(Arena *arena, String8 s) {
    // Every UTF-8 byte yields at most one UTF-16 unit.
    u16 *out = PUSH_ARRAY(arena, u16, s.len + 1);
    i64 n = 0;
    for (i64 i = 0; i < s.len;) {
        i64 advance;
        u32 cp = utf8_decode(s.data + i, s.len - i, &advance);
        i += advance;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[n++] = (u16)(0xD800 + (cp >> 10));
            out[n++] = (u16)(0xDC00 + (cp & 0x3FF));
        } else {
            out[n++] = (u16)cp;
        }
    }
    out[n] = 0;
    arena_pop_to(arena, arena_pos(arena) - (u64)(s.len - n) * sizeof(u16));
    String16 result = { out, n };
    return result;
}

String8 str8_from_str16(Arena *arena, u16 *s, i64 len) {
    // Every UTF-16 unit yields at most three UTF-8 bytes.
    u8 *out = PUSH_ARRAY(arena, u8, len * 3);
    i64 n = 0;
    for (i64 i = 0; i < len; i++) {
        u32 cp = s[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00u);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = UTF_REPLACEMENT;
        }
        n += utf8_encode(cp, out + n);
    }
    arena_pop_to(arena, arena_pos(arena) - (u64)(len * 3 - n));
    return str8(out, n);
}

i64 clip_utf16_len(String8 s) {
    i64 n = 0;
    for (i64 i = 0; i < s.len;) {
        u8 b = s.data[i];
        if (b < 0x80) {
            n += b == '\n' ? 2 : 1;
            i++;
            continue;
        }
        i64 advance;
        u32 cp = utf8_decode(s.data + i, s.len - i, &advance);
        n += cp >= 0x10000 ? 2 : 1;
        i += advance;
    }
    return n;
}

i64 clip_utf16_write(String8 s, u16 *out) {
    i64 n = 0;
    for (i64 i = 0; i < s.len;) {
        u8 b = s.data[i];
        if (b < 0x80) {
            if (b == '\n') out[n++] = '\r';
            out[n++] = b;
            i++;
            continue;
        }
        i64 advance;
        u32 cp = utf8_decode(s.data + i, s.len - i, &advance);
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[n++] = (u16)(0xD800 + (cp >> 10));
            out[n++] = (u16)(0xDC00 + (cp & 0x3FF));
        } else {
            out[n++] = (u16)cp;
        }
        i += advance;
    }
    return n;
}

String8 clip_utf8_from_utf16(Arena *arena, u16 *s, i64 len) {
    u8 *out = PUSH_ARRAY(arena, u8, len * 3);
    i64 n = 0;
    for (i64 i = 0; i < len; i++) {
        u32 cp = s[i];
        if (cp == '\r') {
            if (i + 1 < len && s[i + 1] == '\n') i++;
            out[n++] = '\n';
            continue;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00u);
            i++;
        } else if (cp >= 0xD800 && cp <= 0xDFFF) {
            cp = UTF_REPLACEMENT;
        }
        n += utf8_encode(cp, out + n);
    }
    arena_pop_to(arena, arena_pos(arena) - (u64)(len * 3 - n));
    return str8(out, n);
}

// ---------------------------------------------------------------------------
// Formatter

static void fmt_put(u8 *out, i64 cap, i64 *n, u8 c) {
    if (*n < cap) out[*n] = c;
    (*n)++;
}

static void fmt_put_uint(u8 *out, i64 cap, i64 *n, u64 v, u32 base, i32 width, b32 negative) {
    u8 digits[24];
    i32 count = 0;
    do {
        u32 d = (u32)(v % base);
        digits[count++] = (u8)(d < 10 ? '0' + d : 'a' + d - 10);
        v /= base;
    } while (v);
    if (negative) fmt_put(out, cap, n, '-');
    for (i32 i = count + (negative ? 1 : 0); i < width; i++) fmt_put(out, cap, n, '0');
    while (count) fmt_put(out, cap, n, digits[--count]);
}

i64 fmt_v(u8 *out, i64 cap, const char *fmt, va_list args) {
    i64 n = 0;
    for (const char *p = fmt; *p; p++) {
        if (*p != '%') {
            fmt_put(out, cap, &n, (u8)*p);
            continue;
        }
        p++;
        i32 width = 0;
        if (*p == '0') p++;
        while (*p >= '0' && *p <= '9') width = width * 10 + (*p++ - '0');
        switch (*p) {
        case 'd': {
            i32 v = va_arg(args, i32);
            u64 mag = v < 0 ? (u64)(-(i64)v) : (u64)v;
            fmt_put_uint(out, cap, &n, mag, 10, width, v < 0);
        } break;
        case 'D': {
            i64 v = va_arg(args, i64);
            u64 mag = v < 0 ? (u64)0 - (u64)v : (u64)v;
            fmt_put_uint(out, cap, &n, mag, 10, width, v < 0);
        } break;
        case 'u': fmt_put_uint(out, cap, &n, va_arg(args, u32), 10, width, 0); break;
        case 'U': fmt_put_uint(out, cap, &n, va_arg(args, u64), 10, width, 0); break;
        case 'x': fmt_put_uint(out, cap, &n, va_arg(args, u32), 16, width, 0); break;
        case 'X': fmt_put_uint(out, cap, &n, va_arg(args, u64), 16, width, 0); break;
        case 'c': fmt_put(out, cap, &n, (u8)va_arg(args, int)); break;
        case 's': {
            const char *s = va_arg(args, const char *);
            while (*s) fmt_put(out, cap, &n, (u8)*s++);
        } break;
        case 'S': {
            String8 s = va_arg(args, String8);
            for (i64 i = 0; i < s.len; i++) fmt_put(out, cap, &n, s.data[i]);
        } break;
        case '%': fmt_put(out, cap, &n, '%'); break;
        default: // unsupported specifier
#if TEAL_DEV
            __debugbreak();
#endif
            if (!*p) return n;
            break;
        }
    }
    return n;
}

String8 str8_fmtv(Arena *arena, const char *fmt, va_list args) {
    va_list args2;
    va_copy(args2, args);
    i64 len = fmt_v(NULL, 0, fmt, args);
    u8 *data = PUSH_ARRAY(arena, u8, len);
    fmt_v(data, len, fmt, args2);
    va_end(args2);
    return str8(data, len);
}

String8 str8_fmt(Arena *arena, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    String8 s = str8_fmtv(arena, fmt, args);
    va_end(args);
    return s;
}

i64 fmt_buf(u8 *out, i64 cap, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    i64 n = fmt_v(out, cap, fmt, args);
    va_end(args);
    return MIN(n, cap);
}

// ---------------------------------------------------------------------------
// Dev log

#if TEAL_DEV
void assert_log(const char *file, int line, const char *expr) {
    log_fmt("ASSERT failed: %s(%d): %s", file, (i32)line, expr);
}

void log_fmt(const char *fmt, ...) {
    u8 buffer[2048];
    va_list args;
    va_start(args, fmt);
    i64 len = fmt_v(buffer, sizeof(buffer) - 1, fmt, args);
    va_end(args);
    len = MIN(len, (i64)sizeof(buffer) - 1);
    buffer[len++] = '\n';
    os_log_write(str8(buffer, len));
}
#endif
