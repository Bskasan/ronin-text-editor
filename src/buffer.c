// buffer.c — see buffer.h.

#define BUFFER_COMMIT_GRANULARITY KB(64)
#define BUFFER_GAP_MIN KB(64)
#define BUFFER_GAP_MAX MB(64)

// ---------------------------------------------------------------------------
// Create / destroy

Buffer *buffer_create_reserve(String8 name, i64 text_reserve) {
    text_reserve = (i64)ALIGN_UP_POW2((u64)text_reserve, BUFFER_COMMIT_GRANULARITY);
    ASSERT(text_reserve <= (i64)BUFFER_TEXT_RESERVE); // line index entries are u32
    u8 *text = (u8 *)os_reserve((u64)text_reserve);
    u32 *nl = (u32 *)os_reserve((u64)text_reserve * sizeof(u32));
    if (!text || !nl) {
        if (text) os_release(text);
        if (nl) os_release(nl);
        return NULL;
    }
    Arena meta = arena_create(BUFFER_META_RESERVE);
    Buffer *buf = PUSH_STRUCT(&meta, Buffer);
    buf->meta = meta;
    buf->text = text;
    buf->text_reserved = text_reserve;
    buf->nl = nl;
    buf->nl_reserved = text_reserve; // at most one newline per byte
    buf->name = str8_copy(&buf->meta, name);
    return buf;
}

Buffer *buffer_create(String8 name) {
    return buffer_create_reserve(name, BUFFER_TEXT_RESERVE);
}

b32 buffer_destroy(Buffer *buf) {
    Arena meta = buf->meta; // buf lives in it
    b32 ok = os_release(buf->text);
    ok &= os_release(buf->nl);
    ok &= os_release(meta.base);
    return ok;
}

// ---------------------------------------------------------------------------
// Queries

i64 buffer_size(Buffer *buf) {
    return buf->text_cap - (buf->gap_end - buf->gap_start);
}

static i64 buffer_nl_count(Buffer *buf) {
    return buf->nl_front + (buf->nl_cap - buf->nl_back);
}

i64 buffer_line_count(Buffer *buf) {
    return buffer_nl_count(buf) + 1;
}

// Position of the k-th newline.
static i64 buffer_nl_pos(Buffer *buf, i64 k) {
    if (k < buf->nl_front) return buf->nl[k];
    return buffer_size(buf) - buf->nl[buf->nl_back + (k - buf->nl_front)];
}

i64 buffer_line_start(Buffer *buf, i64 line) {
    if (line <= 0) return 0;
    i64 count = buffer_nl_count(buf);
    if (line > count) line = count;
    return buffer_nl_pos(buf, line - 1) + 1;
}

i64 buffer_line_end(Buffer *buf, i64 line) {
    if (line < 0) line = 0;
    if (line >= buffer_nl_count(buf)) return buffer_size(buf);
    return buffer_nl_pos(buf, line);
}

// Number of newlines before `offset`, which is the line containing it.
i64 buffer_line_of(Buffer *buf, i64 offset) {
    if (buf->nl_front > 0 && (i64)buf->nl[buf->nl_front - 1] >= offset) {
        i64 lo = 0, hi = buf->nl_front - 1; // first front entry >= offset
        while (lo < hi) {
            i64 mid = lo + (hi - lo) / 2;
            if ((i64)buf->nl[mid] < offset) lo = mid + 1;
            else hi = mid;
        }
        return lo;
    }
    // Back entries: pos = size - value, increasing, so values decrease. pos < offset <=> value > limit.
    i64 limit = buffer_size(buf) - offset;
    i64 lo = buf->nl_back, hi = buf->nl_cap;
    while (lo < hi) {
        i64 mid = lo + (hi - lo) / 2;
        if ((i64)buf->nl[mid] > limit) lo = mid + 1;
        else hi = mid;
    }
    return buf->nl_front + (lo - buf->nl_back);
}

u8 buffer_byte(Buffer *buf, i64 offset) {
    ASSERT(offset >= 0 && offset < buffer_size(buf));
    return offset < buf->gap_start ? buf->text[offset] : buf->text[offset + (buf->gap_end - buf->gap_start)];
}

void buffer_copy(Buffer *buf, i64 start, i64 end, u8 *dst) {
    ASSERT(0 <= start && start <= end && end <= buffer_size(buf));
    i64 gap = buf->gap_end - buf->gap_start;
    if (start < buf->gap_start) {
        i64 n = MIN(end, buf->gap_start) - start;
        memcpy(dst, buf->text + start, (size_t)n);
        dst += n;
        start += n;
    }
    if (start < end) memcpy(dst, buf->text + start + gap, (size_t)(end - start));
}

String8 buffer_text(Buffer *buf, Arena *scratch, i64 start, i64 end) {
    ASSERT(0 <= start && start <= end && end <= buffer_size(buf));
    if (end <= buf->gap_start) return str8(buf->text + start, end - start);
    if (start >= buf->gap_start) return str8(buf->text + start + (buf->gap_end - buf->gap_start), end - start);
    u8 *data = PUSH_ARRAY(scratch, u8, end - start);
    buffer_copy(buf, start, end, data);
    return str8(data, end - start);
}

String8 buffer_line(Buffer *buf, Arena *scratch, i64 line) {
    return buffer_text(buf, scratch, buffer_line_start(buf, line), buffer_line_end(buf, line));
}

void buffer_segments(Buffer *buf, String8 *before, String8 *after) {
    *before = str8(buf->text, buf->gap_start);
    *after = str8(buf->text + buf->gap_end, buf->text_cap - buf->gap_end);
}

// Up to 4 bytes starting at offset, across the gap. Returns how many.
static i64 buffer_peek4(Buffer *buf, i64 offset, u8 *out) {
    i64 n = MIN(4, buffer_size(buf) - offset);
    for (i64 i = 0; i < n; i++) out[i] = buffer_byte(buf, offset + i);
    return n;
}

i64 buffer_next_char(Buffer *buf, i64 offset) {
    if (offset >= buffer_size(buf)) return buffer_size(buf);
    if (offset < 0) return 0;
    u8 bytes[4];
    i64 n = buffer_peek4(buf, offset, bytes);
    i64 advance;
    utf8_decode(bytes, n, &advance);
    return offset + advance;
}

i64 buffer_prev_char(Buffer *buf, i64 offset) {
    if (offset <= 0) return 0;
    if (offset > buffer_size(buf)) return buffer_size(buf);
    // Walk back over continuation bytes to the nearest other byte; it starts the character
    // that ends at `offset` only if it decodes as a valid sequence of exactly that length.
    for (i64 k = 1; k <= 4 && offset - k >= 0; k++) {
        u8 b = buffer_byte(buf, offset - k);
        if ((b & 0xC0) == 0x80) continue;
        if (k > 1) {
            u8 bytes[4];
            i64 n = buffer_peek4(buf, offset - k, bytes);
            i64 advance;
            utf8_decode(bytes, n, &advance);
            if (advance == k) return offset - k;
        }
        break;
    }
    return offset - 1;
}

// ---------------------------------------------------------------------------
// Gap management

// Makes the text gap at least `need` bytes. Commits memory and moves the text after the gap;
// the logical text is unchanged. False if it does not fit.
static b32 buffer_reserve_gap(Buffer *buf, i64 need) {
    i64 gap = buf->gap_end - buf->gap_start;
    if (gap >= need) return 1;
    i64 size = buffer_size(buf);
    i64 slack = CLAMP(size / 16, (i64)BUFFER_GAP_MIN, (i64)BUFFER_GAP_MAX);
    i64 min_cap = buf->text_cap + (need - gap);
    if (min_cap > buf->text_reserved) return 0;
    i64 new_cap = (i64)ALIGN_UP_POW2((u64)(min_cap + slack), BUFFER_COMMIT_GRANULARITY);
    new_cap = MIN(new_cap, buf->text_reserved);
    if (!os_commit(buf->text + buf->text_cap, (u64)(new_cap - buf->text_cap))) {
        // Retry without the slack before giving up.
        new_cap = (i64)ALIGN_UP_POW2((u64)min_cap, BUFFER_COMMIT_GRANULARITY);
        new_cap = MIN(new_cap, buf->text_reserved);
        if (!os_commit(buf->text + buf->text_cap, (u64)(new_cap - buf->text_cap))) return 0;
    }
    i64 after = buf->text_cap - buf->gap_end;
    memmove(buf->text + new_cap - after, buf->text + buf->gap_end, (size_t)after);
    buf->gap_end = new_cap - after;
    buf->text_cap = new_cap;
    return 1;
}

// Same for the line index gap, in entries.
static b32 buffer_reserve_nl_gap(Buffer *buf, i64 need) {
    i64 gap = buf->nl_back - buf->nl_front;
    if (gap >= need) return 1;
    i64 count = buffer_nl_count(buf);
    i64 slack = CLAMP(count / 16, (i64)(BUFFER_GAP_MIN / sizeof(u32)), (i64)(BUFFER_GAP_MAX / sizeof(u32)));
    i64 per_page = (i64)(BUFFER_COMMIT_GRANULARITY / sizeof(u32));
    i64 min_cap = buf->nl_cap + (need - gap);
    if (min_cap > buf->nl_reserved) return 0;
    i64 new_cap = MIN((i64)ALIGN_UP_POW2((u64)(min_cap + slack), (u64)per_page), buf->nl_reserved);
    if (!os_commit(buf->nl + buf->nl_cap, (u64)(new_cap - buf->nl_cap) * sizeof(u32))) {
        new_cap = MIN((i64)ALIGN_UP_POW2((u64)min_cap, (u64)per_page), buf->nl_reserved);
        if (!os_commit(buf->nl + buf->nl_cap, (u64)(new_cap - buf->nl_cap) * sizeof(u32))) return 0;
    }
    i64 after = buf->nl_cap - buf->nl_back;
    memmove(buf->nl + new_cap - after, buf->nl + buf->nl_back, (size_t)after * sizeof(u32));
    buf->nl_back = new_cap - after;
    buf->nl_cap = new_cap;
    return 1;
}

// Moves the text gap to `pos`, and with it the line index gap: newline entries that change
// sides are converted between absolute and from-the-end form. The logical text is unchanged.
static void buffer_move_gap(Buffer *buf, i64 pos) {
    i64 size = buffer_size(buf);
    if (pos < buf->gap_start) {
        i64 n = buf->gap_start - pos;
        memmove(buf->text + buf->gap_end - n, buf->text + pos, (size_t)n);
        buf->gap_start -= n;
        buf->gap_end -= n;
        while (buf->nl_front > 0 && (i64)buf->nl[buf->nl_front - 1] >= pos) {
            u32 abs = buf->nl[--buf->nl_front];
            buf->nl[--buf->nl_back] = (u32)(size - abs);
        }
    } else if (pos > buf->gap_start) {
        i64 n = pos - buf->gap_start;
        memmove(buf->text + buf->gap_start, buf->text + buf->gap_end, (size_t)n);
        buf->gap_start += n;
        buf->gap_end += n;
        while (buf->nl_back < buf->nl_cap && size - (i64)buf->nl[buf->nl_back] < pos) {
            u32 from_end = buf->nl[buf->nl_back++];
            buf->nl[buf->nl_front++] = (u32)(size - from_end);
        }
    }
}

// ---------------------------------------------------------------------------
// The one modification function

b32 buffer_replace(Buffer *buf, i64 start, i64 end, String8 text) {
    i64 size = buffer_size(buf);
    ASSERT(0 <= start && start <= end && end <= size);
    if (start < 0 || start > end || end > size || text.len < 0) return 0;
    if (buf->read_only && !buf->inhibit_read_only) return 0;
    if (start == end && text.len == 0) return 1;

    // Everything that can fail happens first, so a refused edit leaves the buffer unchanged.
    i64 new_lines = 0;
    for (i64 i = 0; i < text.len; i++) new_lines += text.data[i] == '\n';
    if (size - (end - start) + text.len > buf->text_reserved) return 0;
    if (!buffer_reserve_gap(buf, text.len - (end - start))) return 0;
    if (!buffer_reserve_nl_gap(buf, new_lines)) return 0;

    // Put the gap inside [start, end], then widen it over the range.
    if (buf->gap_start < start) buffer_move_gap(buf, start);
    else if (buf->gap_start > end) buffer_move_gap(buf, end);
    while (buf->nl_back < buf->nl_cap && size - (i64)buf->nl[buf->nl_back] < end) buf->nl_back++;
    while (buf->nl_front > 0 && (i64)buf->nl[buf->nl_front - 1] >= start) buf->nl_front--;
    buf->gap_end += end - buf->gap_start;
    buf->gap_start = start;
    // Remaining back entries keep their value: position and size both dropped by end - start.

    memcpy(buf->text + buf->gap_start, text.data, (size_t)text.len);
    for (i64 i = 0; i < text.len; i++) {
        if (text.data[i] == '\n') buf->nl[buf->nl_front++] = (u32)(start + i);
    }
    buf->gap_start += text.len;

    buf->modified = 1;
    buf->edit_count++;
    return 1;
}
