// search.c — see search.h.

#include <emmintrin.h> // SSE2 scan for the first byte (compiler intrinsics, not the CRT)
#include <intrin.h>    // _BitScanForward / _BitScanReverse

static i64 search_utf8_len(u32 c) {
    return c < 0x80 ? 1 : c < 0x800 ? 2 : c < 0x10000 ? 3 : 4;
}

u32 search_fold_char(u32 c) {
    if (c < 0x80) return c >= 'A' && c <= 'Z' ? c + 32 : c;
    u32 l = unicode_lower(c);
    return search_utf8_len(l) == search_utf8_len(c) ? l : c;
}

b32 search_has_upper(String8 s) {
    for (i64 i = 0; i < s.len;) {
        u8 b = s.data[i];
        if (b < 0x80) {
            if (b >= 'A' && b <= 'Z') return 1;
            i++;
            continue;
        }
        i64 advance;
        u32 c = utf8_decode(s.data + i, s.len - i, &advance);
        if (c != UTF_REPLACEMENT && unicode_lower(c) != c) return 1;
        i += advance;
    }
    return 0;
}

i32 search_first_bytes(u32 f, u8 out[2]) {
    u8 enc[4];
    utf8_encode(f, enc);
    out[0] = enc[0];
    i32 n = 1;
    u32 u = unicode_upper(f);
    if (u != f && search_fold_char(u) == f) {
        utf8_encode(u, enc);
        if (enc[0] != out[0]) out[n++] = enc[0];
    }
    return n;
}

// The matcher's folding (match_fold): character by character, the lowercase form only when it has the
// same length; invalid bytes are kept. `in` and `out` are `len` bytes.
static void search_fold(u8 *out, const u8 *in, i64 len) {
    for (i64 i = 0; i < len;) {
        u8 b = in[i];
        if (b < 0x80) {
            out[i++] = b >= 'A' && b <= 'Z' ? (u8)(b + 32) : b;
            continue;
        }
        i64 advance;
        u32 c = utf8_decode((u8 *)in + i, len - i, &advance);
        u8 lower[4];
        i64 n = c == UTF_REPLACEMENT ? 0 : utf8_encode(unicode_lower(c), lower);
        if (n == advance) memcpy(out + i, lower, (size_t)n);
        else memcpy(out + i, in + i, (size_t)advance);
        i += advance;
    }
}

// `h` folded the same way equals the folded needle `n` (both `len` bytes). Characters of `h` are
// decoded within the window, as search_fold decodes a string of exactly these bytes.
static b32 search_equal_folded(const u8 *h, const u8 *n, i64 len) {
    for (i64 i = 0; i < len;) {
        u8 b = h[i];
        if (b < 0x80) {
            if ((b >= 'A' && b <= 'Z' ? b + 32 : b) != n[i]) return 0;
            i++;
            continue;
        }
        i64 advance;
        u32 c = utf8_decode((u8 *)h + i, len - i, &advance);
        u8 lower[4];
        i64 k = c == UTF_REPLACEMENT ? 0 : utf8_encode(unicode_lower(c), lower);
        if (!mem_equal(k == advance ? lower : h + i, n + i, advance)) return 0;
        i += advance;
    }
    return 1;
}

b32 search_begin(Search *s, Buffer *buf, String8 needle, b32 fold, b32 forward, i64 from, i64 lo, i64 hi) {
    s->fold = fold;
    s->len = needle.len > 0 && needle.len <= SEARCH_NEEDLE_MAX ? needle.len : 0;
    if (s->len) {
        if (fold) search_fold(s->needle, needle.data, s->len);
        else memcpy(s->needle, needle.data, (size_t)s->len);
        u8 b = s->needle[0];
        s->ascii_fold = fold && b >= 'a' && b <= 'z';
        s->first[0] = s->first[1] = b;
        s->first_count = 1;
        if (fold && b >= 0x80) {
            i64 advance;
            u32 f = utf8_decode(s->needle, s->len, &advance);
            if (f != UTF_REPLACEMENT) s->first_count = search_first_bytes(f, s->first);
            s->first[1] = s->first[s->first_count - 1];
        }
        s->check_boundary = (b & 0xC0) == 0x80;
    }
    search_restart(s, buf, forward, from, lo, hi);
    return s->len > 0;
}

void search_restart(Search *s, Buffer *buf, b32 forward, i64 from, i64 lo, i64 hi) {
    i64 size = buffer_size(buf);
    s->forward = forward;
    s->lo = CLAMP(lo, 0, size);
    s->hi = CLAMP(hi, s->lo, size);
    s->from = CLAMP(from, s->lo, s->hi);
    s->edits = buf->edit_count;
    s->examined = 0;
    s->match_start = s->match_end = -1;
    s->next = forward ? s->from : s->from - s->len; // backward: the largest start whose match ends by `from`
    s->status = SEARCH_RUNNING;
    if (!s->len || (forward ? s->next > s->hi - s->len : s->next < s->lo)) s->status = SEARCH_NOT_FOUND;
}

static b32 search_is_first(Search *s, u8 b) {
    return s->ascii_fold ? (u8)(b | 0x20) == s->first[0] : b == s->first[0] || b == s->first[1];
}

static u32 search_mask16(Search *s, const u8 *p) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i m;
    if (s->ascii_fold) {
        m = _mm_cmpeq_epi8(_mm_or_si128(v, _mm_set1_epi8(0x20)), _mm_set1_epi8((char)s->first[0]));
    } else {
        m = _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8((char)s->first[0])), _mm_cmpeq_epi8(v, _mm_set1_epi8((char)s->first[1])));
    }
    return (u32)_mm_movemask_epi8(m);
}

// The first i in [i, end) where p[i] can start a match, or -1. 16 bytes at a time.
static i64 search_scan_forward(Search *s, const u8 *p, i64 i, i64 end) {
    for (; i + 16 <= end; i += 16) {
        u32 mask = search_mask16(s, p + i);
        if (mask) {
            unsigned long bit;
            _BitScanForward(&bit, mask);
            return i + (i64)bit;
        }
    }
    for (; i < end; i++) if (search_is_first(s, p[i])) return i;
    return -1;
}

// The last i in [low, high] (inclusive) where p[i] can start a match, or -1.
static i64 search_scan_backward(Search *s, const u8 *p, i64 low, i64 high) {
    for (; high - 15 >= low; high -= 16) {
        u32 mask = search_mask16(s, p + high - 15);
        if (mask) {
            unsigned long bit;
            _BitScanReverse(&bit, mask);
            return high - 15 + (i64)bit;
        }
    }
    for (; high >= low; high--) if (search_is_first(s, p[high])) return high;
    return -1;
}

// Whether the needle matches at logical position c. A candidate straddling the gap is compared
// through the window.
static b32 search_verify(Search *s, Buffer *buf, String8 a, String8 b, i64 c) {
    if (s->check_boundary && buffer_snap_char(buf, c) != c) return 0;
    const u8 *h;
    if (c + s->len <= a.len) {
        h = a.data + c;
    } else if (c >= a.len) {
        h = b.data + (c - a.len);
    } else {
        buffer_copy(buf, c, c + s->len, s->window);
        h = s->window;
    }
    return s->fold ? search_equal_folded(h, s->needle, s->len) : mem_equal(h, s->needle, s->len);
}

static SearchStatus search_found(Search *s, i64 c) {
    s->status = SEARCH_FOUND;
    s->match_start = c;
    s->match_end = c + s->len;
    return s->status;
}

SearchStatus search_run(Search *s, Buffer *buf, i64 budget) {
    if (s->status == SEARCH_RUNNING && buf->edit_count != s->edits) search_restart(s, buf, s->forward, s->from, s->lo, s->hi);
    if (s->status != SEARCH_RUNNING) return s->status;
    String8 a, b;
    buffer_segments(buf, &a, &b);
    budget = MAX(budget, 1);
    if (s->forward) {
        i64 last = s->hi - s->len; // the last start whose match fits
        i64 stop = MIN(last + 1, s->next + budget);
        for (i64 pos = s->next; pos < stop;) {
            b32 front = pos < a.len;
            const u8 *seg = front ? a.data : b.data;
            i64 base = front ? 0 : a.len, end = front ? MIN(stop, a.len) : stop;
            i64 c = search_scan_forward(s, seg, pos - base, end - base);
            if (c < 0) {
                pos = end;
                continue;
            }
            c += base;
            if (search_verify(s, buf, a, b, c)) {
                s->examined += c + 1 - s->next;
                s->next = c + 1;
                return search_found(s, c);
            }
            pos = c + 1;
        }
        s->examined += stop - s->next;
        s->next = stop;
        if (s->next > last) s->status = SEARCH_NOT_FOUND;
    } else {
        i64 low = MAX(s->lo, s->next - budget + 1);
        for (i64 pos = s->next; pos >= low;) {
            b32 back = pos >= a.len;
            const u8 *seg = back ? b.data : a.data;
            i64 base = back ? a.len : 0, start = back ? MAX(low, a.len) : low;
            i64 c = search_scan_backward(s, seg, start - base, pos - base);
            if (c < 0) {
                pos = start - 1;
                continue;
            }
            c += base;
            if (search_verify(s, buf, a, b, c)) {
                s->examined += s->next - c + 1;
                s->next = c - 1;
                return search_found(s, c);
            }
            pos = c - 1;
        }
        s->examined += s->next - low + 1;
        s->next = low - 1;
        if (s->next < s->lo) s->status = SEARCH_NOT_FOUND;
    }
    return s->status;
}

i32 search_progress(Search *s) {
    i64 total = s->forward ? s->hi - s->len + 1 - s->from : s->from - s->len - s->lo + 1;
    if (s->status != SEARCH_RUNNING || total <= 0) return 100;
    i64 done = s->forward ? s->next - s->from : s->from - s->len - s->next;
    return (i32)CLAMP(done * 100 / total, 0, 100);
}
