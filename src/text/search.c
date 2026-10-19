/* ======================================================================
   ronin: literal text search over plain bytes and the gap buffer, resumable

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// see search.h.

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
void search_fold_bytes(u8 *out, const u8 *in, i64 len) {
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
// decoded within the window, as search_fold_bytes decodes a string of exactly these bytes.
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

// ---------------------------------------------------------------------------
// The core: plain byte ranges

b32 search_pattern_init(SearchPattern *pat, String8 needle, b32 fold) {
    pat->fold = fold;
    pat->len = needle.len > 0 && needle.len <= SEARCH_NEEDLE_MAX ? needle.len : 0;
    if (!pat->len) return 0;
    if (fold) search_fold_bytes(pat->needle, needle.data, pat->len);
    else memcpy(pat->needle, needle.data, (size_t)pat->len);
    u8 b = pat->needle[0];
    pat->ascii_fold = fold && b >= 'a' && b <= 'z';
    pat->first[0] = pat->first[1] = b;
    pat->first_count = 1;
    if (fold && b >= 0x80) {
        i64 advance;
        u32 f = utf8_decode(pat->needle, pat->len, &advance);
        if (f != UTF_REPLACEMENT) pat->first_count = search_first_bytes(f, pat->first);
        pat->first[1] = pat->first[pat->first_count - 1];
    }
    pat->check_boundary = (b & 0xC0) == 0x80;
    pat->second_on = pat->len >= 2 && (!fold || (b < 0x80 && pat->needle[1] < 0x80));
    pat->second = pat->second_on ? pat->needle[1] : 0;
    pat->second_fold = fold && pat->second >= 'a' && pat->second <= 'z';
    return 1;
}

static b32 search_is_first(const SearchPattern *pat, u8 b) {
    return pat->ascii_fold ? (u8)(b | 0x20) == pat->first[0] : b == pat->first[0] || b == pat->first[1];
}

// p[i] can start a match: its first byte, and its second when that filter is on and p[i + 1] is valid.
static b32 search_is_candidate(const SearchPattern *pat, const u8 *p, i64 i, i64 valid) {
    if (!search_is_first(pat, p[i])) return 0;
    if (!pat->second_on || i + 1 >= valid) return 1;
    return pat->second_fold ? (u8)(p[i + 1] | 0x20) == pat->second : p[i + 1] == pat->second;
}

// The candidates among p[0..16): the first byte, and the second (p[1..17), so p[16] must be valid).
static u32 search_mask16(const SearchPattern *pat, const u8 *p) {
    __m128i v = _mm_loadu_si128((const __m128i *)p);
    __m128i m;
    if (pat->ascii_fold) {
        m = _mm_cmpeq_epi8(_mm_or_si128(v, _mm_set1_epi8(0x20)), _mm_set1_epi8((char)pat->first[0]));
    } else {
        m = _mm_or_si128(_mm_cmpeq_epi8(v, _mm_set1_epi8((char)pat->first[0])), _mm_cmpeq_epi8(v, _mm_set1_epi8((char)pat->first[1])));
    }
    if (pat->second_on) {
        __m128i w = _mm_loadu_si128((const __m128i *)(p + 1));
        if (pat->second_fold) w = _mm_or_si128(w, _mm_set1_epi8(0x20));
        m = _mm_and_si128(m, _mm_cmpeq_epi8(w, _mm_set1_epi8((char)pat->second)));
    }
    return (u32)_mm_movemask_epi8(m);
}

// 16 bytes at a time; with the second-byte filter only while the 17th byte is valid.
i64 search_candidate_forward(const SearchPattern *pat, const u8 *p, i64 i, i64 end, i64 valid) {
    i64 vec_end = pat->second_on ? MIN(end, valid - 1) : end;
    for (; i + 16 <= vec_end; i += 16) {
        u32 mask = search_mask16(pat, p + i);
        if (mask) {
            unsigned long bit;
            _BitScanForward(&bit, mask);
            return i + (i64)bit;
        }
    }
    for (; i < end; i++) if (search_is_candidate(pat, p, i, valid)) return i;
    return -1;
}

i64 search_candidate_backward(const SearchPattern *pat, const u8 *p, i64 low, i64 high, i64 valid) {
    if (pat->second_on) { // the positions whose second byte is the last valid one or past it: one at a time
        for (; high >= low && high > valid - 2; high--) if (search_is_candidate(pat, p, high, valid)) return high;
    }
    for (; high - 15 >= low; high -= 16) {
        u32 mask = search_mask16(pat, p + high - 15);
        if (mask) {
            unsigned long bit;
            _BitScanReverse(&bit, mask);
            return high - 15 + (i64)bit;
        }
    }
    for (; high >= low; high--) if (search_is_candidate(pat, p, high, valid)) return high;
    return -1;
}

b32 search_pattern_equal(const SearchPattern *pat, const u8 *h) {
    return pat->fold ? search_equal_folded(h, pat->needle, pat->len) : mem_equal(h, pat->needle, pat->len);
}

// buffer_snap_char on plain bytes: c starts a character unless a valid sequence starting up to 3 bytes
// before it (not before lo) reaches past it.
static b32 search_bytes_boundary(const u8 *p, i64 lo, i64 hi, i64 c) {
    if (c <= lo || c >= hi || (p[c] & 0xC0) != 0x80) return 1;
    for (i64 k = 1; k <= 3 && c - k >= lo; k++) {
        if ((p[c - k] & 0xC0) == 0x80) continue;
        i64 advance;
        utf8_decode((u8 *)p + c - k, MIN(4, hi - (c - k)), &advance);
        return advance <= k;
    }
    return 1;
}

i64 search_bytes_forward(const SearchPattern *pat, const u8 *p, i64 lo, i64 hi, i64 from, i64 to) {
    if (!pat->len) return -1;
    from = MAX(from, lo);
    i64 end = MIN(to, hi) - pat->len + 1; // starts in [from, end)
    for (i64 i = from; i < end;) {
        i64 c = search_candidate_forward(pat, p, i, end, hi);
        if (c < 0) return -1;
        if ((!pat->check_boundary || search_bytes_boundary(p, lo, hi, c)) && search_pattern_equal(pat, p + c)) return c;
        i = c + 1;
    }
    return -1;
}

i64 search_bytes_backward(const SearchPattern *pat, const u8 *p, i64 lo, i64 hi, i64 from, i64 to) {
    if (!pat->len) return -1;
    from = MAX(from, lo);
    for (i64 high = MIN(to, hi) - pat->len; high >= from;) {
        i64 c = search_candidate_backward(pat, p, from, high, hi);
        if (c < 0) return -1;
        if ((!pat->check_boundary || search_bytes_boundary(p, lo, hi, c)) && search_pattern_equal(pat, p + c)) return c;
        high = c - 1;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// A search over a gap buffer: the core's scan on each segment, a candidate across the gap compared
// through the window.

b32 search_begin(Search *s, Buffer *buf, String8 needle, b32 fold, b32 forward, i64 from, i64 lo, i64 hi) {
    search_pattern_init(&s->pat, needle, fold);
    search_restart(s, buf, forward, from, lo, hi);
    return s->pat.len > 0;
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
    s->next = forward ? s->from : s->from - s->pat.len; // backward: the largest start whose match ends by `from`
    s->status = SEARCH_RUNNING;
    if (!s->pat.len || (forward ? s->next > s->hi - s->pat.len : s->next < s->lo)) s->status = SEARCH_NOT_FOUND;
}

// Whether the needle matches at logical position c. A candidate straddling the gap is compared
// through the window.
static b32 search_verify(Search *s, Buffer *buf, String8 a, String8 b, i64 c) {
    if (s->pat.check_boundary && buffer_snap_char(buf, c) != c) return 0;
    const u8 *h;
    if (c + s->pat.len <= a.len) {
        h = a.data + c;
    } else if (c >= a.len) {
        h = b.data + (c - a.len);
    } else {
        buffer_copy(buf, c, c + s->pat.len, s->window);
        h = s->window;
    }
    return search_pattern_equal(&s->pat, h);
}

static SearchStatus search_found(Search *s, i64 c) {
    s->status = SEARCH_FOUND;
    s->match_start = c;
    s->match_end = c + s->pat.len;
    return s->status;
}

SearchStatus search_run(Search *s, Buffer *buf, i64 budget) {
    if (s->status == SEARCH_RUNNING && buf->edit_count != s->edits) search_restart(s, buf, s->forward, s->from, s->lo, s->hi);
    if (s->status != SEARCH_RUNNING) return s->status;
    String8 a, b;
    buffer_segments(buf, &a, &b);
    budget = MAX(budget, 1);
    if (s->forward) {
        i64 last = s->hi - s->pat.len; // the last start whose match fits
        i64 stop = MIN(last + 1, s->next + budget);
        for (i64 pos = s->next; pos < stop;) {
            b32 front = pos < a.len;
            const u8 *seg = front ? a.data : b.data;
            i64 base = front ? 0 : a.len, end = front ? MIN(stop, a.len) : stop;
            i64 c = search_candidate_forward(&s->pat, seg, pos - base, end - base, front ? a.len : b.len);
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
            i64 c = search_candidate_backward(&s->pat, seg, start - base, pos - base, back ? b.len : a.len);
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
    i64 len = s->pat.len;
    i64 total = s->forward ? s->hi - len + 1 - s->from : s->from - len - s->lo + 1;
    if (s->status != SEARCH_RUNNING || total <= 0) return 100;
    i64 done = s->forward ? s->next - s->from : s->from - len - s->next;
    return (i32)CLAMP(done * 100 / total, 0, 100);
}
