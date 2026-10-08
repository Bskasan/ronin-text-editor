// buffer.c — see buffer.h.

#include <emmintrin.h> // SSE2 newline scan (compiler intrinsics, not the CRT)
#include <intrin.h>    // _BitScanForward

#define BUFFER_CHUNK KB(64) // fixed buffer for converting reads and writes
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
    BufferMarkerSlot *markers = (BufferMarkerSlot *)os_reserve(BUFFER_MARKER_RESERVE);
    u8 *undo_log = (u8 *)os_reserve(BUFFER_UNDO_RESERVE);
    BufferUndoGroup *undo_groups = (BufferUndoGroup *)os_reserve(BUFFER_UNDO_GROUP_RESERVE);
    if (!text || !nl || !markers || !undo_log || !undo_groups) {
        if (text) os_release(text);
        if (nl) os_release(nl);
        if (markers) os_release(markers);
        if (undo_log) os_release(undo_log);
        if (undo_groups) os_release(undo_groups);
        return NULL;
    }
    Arena meta = arena_create(BUFFER_META_RESERVE);
    Buffer *buf = PUSH_STRUCT(&meta, Buffer);
    buf->meta = meta;
    buf->text = text;
    buf->text_reserved = text_reserve;
    buf->nl = nl;
    buf->nl_reserved = text_reserve; // at most one newline per byte
    buf->markers = markers;
    buf->marker_reserved = (i64)(BUFFER_MARKER_RESERVE / sizeof(BufferMarkerSlot));
    buf->name = str8_copy(&buf->meta, name);
    buf->tab_width = BUFFER_DEFAULT_TAB_WIDTH;
    buf->undo.enabled = 1;
    buf->undo.log = undo_log;
    buf->undo.groups = undo_groups;
    buf->undo.limit = BUFFER_UNDO_DEFAULT_LIMIT;
    buf->undo.pending = -1;
    return buf;
}

Buffer *buffer_create(String8 name) {
    return buffer_create_reserve(name, BUFFER_TEXT_RESERVE);
}

b32 buffer_destroy(Buffer *buf) {
    Arena meta = buf->meta; // buf lives in it
    b32 ok = os_release(buf->text);
    ok &= os_release(buf->nl);
    ok &= os_release(buf->markers);
    ok &= os_release(buf->undo.log);
    ok &= os_release(buf->undo.groups);
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

i64 buffer_snap_char(Buffer *buf, i64 offset) {
    i64 size = buffer_size(buf);
    if (offset <= 0) return 0;
    if (offset >= size) return size;
    if ((buffer_byte(buf, offset) & 0xC0) != 0x80) return offset; // only continuation bytes can be inside a character
    // The nearest non-continuation byte within 3 bytes starts a unit; offset is inside it only
    // if it decodes as a valid sequence reaching past offset.
    for (i64 k = 1; k <= 3 && offset - k >= 0; k++) {
        if ((buffer_byte(buf, offset - k) & 0xC0) == 0x80) continue;
        u8 bytes[4];
        i64 n = buffer_peek4(buf, offset - k, bytes);
        i64 advance;
        utf8_decode(bytes, n, &advance);
        return advance > k ? offset - k : offset;
    }
    return offset; // a stray continuation byte is its own unit
}

// ---------------------------------------------------------------------------
// Markers

static BufferMarkerSlot *buffer_marker_slot(Buffer *buf, BufferMarker m) {
    ASSERT(m > 0 && (i64)m <= buf->marker_count && (buf->markers[m - 1].flags & BUFFER_MARKER_LIVE));
    return &buf->markers[m - 1];
}

BufferMarker buffer_marker_create(Buffer *buf, i64 pos, b32 advance) {
    u32 slot;
    if (buf->marker_free) {
        slot = buf->marker_free - 1;
        buf->marker_free = buf->markers[slot].next_free;
    } else {
        if (buf->marker_count == buf->marker_cap) {
            i64 per_commit = (i64)(BUFFER_COMMIT_GRANULARITY / sizeof(BufferMarkerSlot));
            if (buf->marker_cap + per_commit > buf->marker_reserved ||
                !os_commit(buf->markers + buf->marker_cap, BUFFER_COMMIT_GRANULARITY)) {
                os_fatal(STR8_LIT("Out of memory (markers)."));
            }
            buf->marker_cap += per_commit;
        }
        slot = (u32)buf->marker_count++;
    }
    BufferMarkerSlot *s = &buf->markers[slot];
    s->pos = buffer_snap_char(buf, pos);
    s->flags = BUFFER_MARKER_LIVE | (advance ? BUFFER_MARKER_ADVANCE : 0);
    s->next_free = 0;
    buf->marker_live++;
    return slot + 1;
}

void buffer_marker_destroy(Buffer *buf, BufferMarker m) {
    BufferMarkerSlot *s = buffer_marker_slot(buf, m);
    s->flags = 0;
    s->next_free = buf->marker_free;
    buf->marker_free = m;
    buf->marker_live--;
}

i64 buffer_marker_get(Buffer *buf, BufferMarker m) {
    return buffer_marker_slot(buf, m)->pos;
}

void buffer_marker_set(Buffer *buf, BufferMarker m, i64 pos) {
    buffer_marker_slot(buf, m)->pos = buffer_snap_char(buf, pos);
}

// After [start, end) became `len` bytes. See buffer.h for the rules.
static void buffer_adjust_markers(Buffer *buf, i64 start, i64 end, i64 len) {
    i64 delta = len - (end - start);
    i64 snap_lo = start - 3, snap_hi = start + len + 3; // where a boundary can have disappeared
    for (i64 i = 0; i < buf->marker_count; i++) {
        BufferMarkerSlot *m = &buf->markers[i];
        if (!(m->flags & BUFFER_MARKER_LIVE)) continue;
        i64 pos = m->pos;
        if (pos < start) {
        } else if (pos > end) {
            pos += delta;
        } else if (pos == end && end > start) {
            pos = start + len;
        } else {
            pos = (m->flags & BUFFER_MARKER_ADVANCE) ? start + len : start;
        }
        if (pos >= snap_lo && pos <= snap_hi) pos = buffer_snap_char(buf, pos);
        m->pos = pos;
    }
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

// ---------------------------------------------------------------------------
// Undo log

#define BUFFER_UNDO_COMMIT KB(64)

void buffer_mark_saved(Buffer *buf) {
    buf->undo.saved_state = buf->undo.state;
    buf->modified = 0;
}

u64 buffer_undo_memory(Buffer *buf) {
    return buf->undo.log_committed + (u64)buf->undo.group_committed * sizeof(BufferUndoGroup);
}

void buffer_undo_enable(Buffer *buf, b32 on) {
    BufferUndo *u = &buf->undo;
    u->enabled = on;
    u->log_used = 0;
    u->first_id += u->group_count;
    u->group_count = 0;
    u->open = 0;
    u->pending = -1;
}

void buffer_undo_set_limit(Buffer *buf, u64 bytes) {
    buf->undo.limit = MAX(bytes, (u64)1);
}

void buffer_undo_boundary(Buffer *buf, BufferUndoMerge merge, b32 consecutive, i64 point) {
    BufferUndo *u = &buf->undo;
    if (u->open && u->group_count > 0 && merge != BUFFER_UNDO_MERGE_NONE && consecutive) {
        BufferUndoGroup *g = &u->groups[u->group_count - 1];
        if (g->merge == (i32)merge && g->target < 0 && g->merge_count < BUFFER_UNDO_MERGE_MAX) {
            g->merge_count++;
            return;
        }
    }
    u->open = 0;
    u->want_merge = merge;
    u->want_point = point;
}

static b32 buffer_undo_commit(Buffer *buf, u64 need) {
    BufferUndo *u = &buf->undo;
    if (u->log_used + need <= u->log_committed) return 1;
    u64 new_committed = ALIGN_UP_POW2(u->log_used + need, BUFFER_UNDO_COMMIT);
    if (new_committed > BUFFER_UNDO_RESERVE) return 0;
    if (!os_commit(u->log + u->log_committed, new_committed - u->log_committed)) return 0;
    u->log_committed = new_committed;
    return 1;
}

static BufferUndoGroup *buffer_undo_new_group(Buffer *buf, i64 point, BufferUndoMerge merge, i64 target) {
    BufferUndo *u = &buf->undo;
    if ((u64)(u->group_count + 1) * sizeof(BufferUndoGroup) > BUFFER_UNDO_GROUP_RESERVE) return NULL;
    if (u->group_count == u->group_committed) {
        i64 more = (i64)(BUFFER_UNDO_COMMIT / sizeof(BufferUndoGroup));
        if (!os_commit(u->groups + u->group_committed, (u64)more * sizeof(BufferUndoGroup))) return NULL;
        u->group_committed += more;
    }
    BufferUndoGroup *g = &u->groups[u->group_count++];
    *g = (BufferUndoGroup){ .offset = u->log_used, .last = u->log_used, .point_before = point, .state_before = u->state,
                            .state_after = u->state, .target = target, .merge = (i32)merge, .merge_count = 1 };
    u->open = 1;
    return g;
}

// Logs the replacement of [start, end) by `inserted` bytes, before it happens. False if the
// record does not fit (the edit is then refused).
static b32 buffer_undo_record(Buffer *buf, i64 start, i64 end, i64 inserted) {
    BufferUndo *u = &buf->undo;
    i64 removed = end - start;
    u64 bytes = sizeof(BufferUndoRecord) + ALIGN_UP_POW2((u64)removed, 8);
    if (!buffer_undo_commit(buf, bytes)) return 0;
    if (!u->open && !buffer_undo_new_group(buf, u->want_point, u->want_merge, -1)) return 0;
    BufferUndoGroup *g = &u->groups[u->group_count - 1];
    BufferUndoRecord *r = (BufferUndoRecord *)(u->log + u->log_used);
    *r = (BufferUndoRecord){ start, removed, inserted, g->size ? u->log_used - g->last : 0 };
    buffer_copy(buf, start, end, u->log + u->log_used + sizeof(BufferUndoRecord));
    g->last = u->log_used;
    g->size += bytes;
    u->log_used += bytes;
    return 1;
}

// Drops the oldest groups while the log is over its limit, down to 3/4 of it (so the memmove is
// rare). The last group (the running or most recent command) always stays.
static void buffer_undo_trim(Buffer *buf) {
    BufferUndo *u = &buf->undo;
    if (u->log_used <= u->limit || u->group_count < 2) return;
    i64 drop = 0;
    u64 cut = 0;
    while (drop < u->group_count - 1 && u->log_used - cut > u->limit / 4 * 3) cut += u->groups[drop++].size;
    if (!drop) return;
    memmove(u->log, u->log + cut, (size_t)(u->log_used - cut));
    u->log_used -= cut;
    memmove(u->groups, u->groups + drop, (size_t)(u->group_count - drop) * sizeof(BufferUndoGroup));
    u->group_count -= drop;
    for (i64 i = 0; i < u->group_count; i++) {
        u->groups[i].offset -= cut;
        u->groups[i].last -= cut;
    }
    u->first_id += drop;
    if (u->pending < u->first_id) u->pending = -1;
}

// Applies the inverse of group `id` as a new group (an undo group reverting it). Its records
// are replayed newest first; each inverse goes through buffer_replace and is logged.
static BufferUndoResult buffer_undo_revert(Buffer *buf, i64 id, b32 redo, i64 point, i64 *point_out) {
    BufferUndo *u = &buf->undo;
    i64 index = id - u->first_id;
    if (index < 0 || index >= u->group_count) return BUFFER_UNDO_NOTHING;
    if (buf->read_only && !buf->inhibit_read_only) return BUFFER_UNDO_FAILED;
    BufferUndoGroup target = u->groups[index];
    u->open = 0;
    if (!buffer_undo_new_group(buf, point, BUFFER_UNDO_MERGE_NONE, id)) return BUFFER_UNDO_FAILED;
    i64 undo_index = u->group_count - 1;
    u->groups[undo_index].redo = redo;
    u->applying = 1;
    BufferUndoResult result = BUFFER_UNDO_DONE;
    // The target's records, newest first.
    for (u64 at = target.last, n = target.size ? 1 : 0; n; ) {
        BufferUndoRecord r = *(BufferUndoRecord *)(u->log + at);
        String8 removed = str8(u->log + at + sizeof(BufferUndoRecord), r.removed);
        if (!buffer_replace(buf, r.start, r.start + r.inserted, removed)) {
            result = BUFFER_UNDO_FAILED;
            break;
        }
        if (!r.prev) break;
        at -= r.prev;
    }
    u->applying = 0;
    BufferUndoGroup *g = &u->groups[undo_index];
    if (result == BUFFER_UNDO_DONE) {
        g->state_after = target.state_before; // back to the state before the reverted group
        u->state = g->state_after;
        buf->modified = u->state != u->saved_state;
    }
    u->open = 0;
    *point_out = target.point_before;
    buffer_undo_trim(buf);
    return result;
}

BufferUndoResult buffer_undo(Buffer *buf, b32 chain, i64 point, i64 *point_out) {
    BufferUndo *u = &buf->undo;
    *point_out = point;
    if (!u->enabled) return BUFFER_UNDO_NOTHING;
    i64 id = chain ? u->pending : u->first_id + u->group_count - 1;
    if (id < u->first_id || u->group_count == 0) {
        u->pending = -1;
        return BUFFER_UNDO_NOTHING;
    }
    BufferUndoResult r = buffer_undo_revert(buf, id, 0, point, point_out);
    if (r == BUFFER_UNDO_DONE) u->pending = id - 1 >= u->first_id ? id - 1 : -1;
    return r;
}

BufferUndoResult buffer_redo(Buffer *buf, i64 point, i64 *point_out) {
    BufferUndo *u = &buf->undo;
    *point_out = point;
    if (!u->enabled) return BUFFER_UNDO_NOTHING;
    // The most recent group made by undo whose result is the current state.
    for (i64 i = u->group_count - 1; i >= 0; i--) {
        BufferUndoGroup *g = &u->groups[i];
        if (g->target < 0 || g->redo || g->state_after != u->state) continue;
        i64 id = u->first_id + i;
        BufferUndoResult r = buffer_undo_revert(buf, id, 1, point, point_out);
        if (r == BUFFER_UNDO_DONE) u->pending = -1;
        return r;
    }
    return BUFFER_UNDO_NOTHING;
}

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
    if (buf->undo.enabled && !buffer_undo_record(buf, start, end, text.len)) return 0; // the last thing that can fail

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
    if (buf->marker_live) buffer_adjust_markers(buf, start, end, text.len);

    if (buf->undo.enabled) {
        buf->undo.state = ++buf->undo.next_state;
        buf->undo.groups[buf->undo.group_count - 1].state_after = buf->undo.state;
        buf->modified = buf->undo.state != buf->undo.saved_state;
        if (!buf->undo.applying) buffer_undo_trim(buf);
    } else {
        buf->modified = 1;
    }
    buf->edit_count++;
    return 1;
}

// ---------------------------------------------------------------------------
// Loading

static b32 buffer_push_newline(Buffer *buf, i64 pos) {
    if (buf->nl_front == buf->nl_back && !buffer_reserve_nl_gap(buf, MAX(buf->nl_front / 4, 1))) return 0;
    buf->nl[buf->nl_front++] = (u32)pos;
    return 1;
}

// Index of the first '\n' in t[from, n), or n. 16 bytes at a time.
static i64 buffer_find_newline(u8 *t, i64 from, i64 n) {
    __m128i newline = _mm_set1_epi8('\n');
    i64 i = from;
    for (; i + 16 <= n; i += 16) {
        int mask = _mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(t + i)), newline));
        if (mask) {
            unsigned long bit;
            _BitScanForward(&bit, (unsigned long)mask);
            return i + (i64)bit;
        }
    }
    for (; i < n; i++) {
        if (t[i] == '\n') return i;
    }
    return n;
}

// One pass over the freshly loaded text[0, n): builds the newline index and decides the
// line-ending mode. CRs before LFs are stripped in place while every LF so far was a CRLF; the
// first bare LF after that restores the processed prefix. Returns the final length, or -1 when
// the index does not fit.
static i64 buffer_scan_loaded(Buffer *buf, i64 n) {
    u8 *t = buf->text;
    enum { UNDECIDED, STRIP, KEEP } mode = UNDECIDED;
    i64 r = 0, w = 0; // read and write positions; w < r only while stripping
    i64 crlf = 0, bare = 0;
    for (;;) {
        i64 i = buffer_find_newline(t, r, n);
        if (i == n) break;
        // t[i - 1] is still original while stripping: writes stay below w, and w <= r <= i.
        b32 cr = i > 0 && t[i - 1] == '\r';
        crlf += cr;
        bare += !cr;
        if (mode == UNDECIDED) mode = cr ? STRIP : KEEP;
        if (mode == STRIP && !cr) {
            // Mixed: put the stripped CRs back. Walk the recorded newlines from the last one,
            // moving each segment right by the number of CRs before it.
            i64 src_end = w, dst_end = r;
            for (i64 j = buf->nl_front - 1; j >= 0; j--) {
                i64 p = buf->nl[j]; // compacted position of the '\n'
                i64 len = src_end - (p + 1);
                memmove(t + dst_end - len, t + p + 1, (size_t)len);
                dst_end -= len;
                t[--dst_end] = '\n';
                t[--dst_end] = '\r';
                buf->nl[j] = (u32)(dst_end + 1);
                src_end = p;
            }
            ASSERT(dst_end == src_end);
            w = r;
            mode = KEEP;
        }
        if (mode == STRIP) {
            i64 len = i - 1 - r; // the line without its CR
            memmove(t + w, t + r, (size_t)len);
            w += len;
            t[w] = '\n';
            if (!buffer_push_newline(buf, w)) return -1;
            w++;
        } else {
            if (!buffer_push_newline(buf, i)) return -1;
            w = i + 1;
        }
        r = i + 1;
    }
    if (mode == STRIP) {
        memmove(t + w, t + r, (size_t)(n - r));
        n = w + (n - r);
    }
    buf->eol = crlf && bare ? BUFFER_EOL_MIXED : crlf ? BUFFER_EOL_CRLF : BUFFER_EOL_LF;
    return n;
}

// UTF-8 for any 16-bit unit, lone surrogates included (3-byte WTF-8, which utf8_encode refuses).
static i64 buffer_encode_unit(u32 cp, u8 *out) {
    if (cp >= 0xD800 && cp <= 0xDFFF) {
        out[0] = (u8)(0xE0 | (cp >> 12));
        out[1] = (u8)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (u8)(0x80 | (cp & 0x3F));
        return 3;
    }
    return utf8_encode(cp, out);
}

// Streams UTF-16 into the buffer as UTF-8 through a fixed chunk: `prefix` (the even number of
// bytes already read after the BOM), then `bytes` more from the file. The text grows at
// gap_start. A pair split by a chunk boundary is joined; unpaired surrogates stay as WTF-8.
static OsFileStatus buffer_load_utf16(Buffer *buf, OsFile file, u8 *prefix, i64 prefix_len, i64 bytes, b32 big_endian) {
    u8 chunk[BUFFER_CHUNK];
    u32 high = 0; // pending high surrogate
    memcpy(chunk, prefix, (size_t)prefix_len);
    i64 have = prefix_len;
    while (have + bytes > 0) {
        i64 c = MIN(bytes, (i64)sizeof(chunk) - have); // keeps the chunk even: everything is
        OsFileStatus status = os_file_read(file, chunk + have, c);
        if (status != OS_FILE_OK) return status;
        bytes -= c;
        c += have;
        have = 0;
        // At most 3 bytes per unit, plus a flushed pending surrogate.
        if (!buffer_reserve_gap(buf, c / 2 * 3 + 3)) return OS_FILE_OUT_OF_MEMORY;
        u8 *out = buf->text + buf->gap_start;
        for (i64 i = 0; i < c; i += 2) {
            u32 u = big_endian ? ((u32)chunk[i] << 8 | chunk[i + 1]) : ((u32)chunk[i + 1] << 8 | chunk[i]);
            if (high) {
                if (u >= 0xDC00 && u <= 0xDFFF) {
                    out += utf8_encode(0x10000 + ((high - 0xD800) << 10) + (u - 0xDC00), out);
                    high = 0;
                    continue;
                }
                out += buffer_encode_unit(high, out);
                high = 0;
            }
            if (u >= 0xD800 && u <= 0xDBFF) high = u;
            else out += buffer_encode_unit(u, out);
        }
        buf->gap_start = out - buf->text;
    }
    if (high) {
        if (!buffer_reserve_gap(buf, 3)) return OS_FILE_OUT_OF_MEMORY;
        buf->gap_start += buffer_encode_unit(high, buf->text + buf->gap_start);
    }
    return OS_FILE_OK;
}

static String8 buffer_file_name(String8 path) {
    i64 i = path.len;
    while (i > 0 && path.data[i - 1] != '\\' && path.data[i - 1] != '/') i--;
    return str8(path.data + i, path.len - i);
}

// The language of a file name, by its extension (ASCII case-insensitive).
static BufferLanguage buffer_language_of(String8 name) {
    static const struct { const char *ext; BufferLanguage language; } table[] = {
        { "jai", BUFFER_LANG_JAI },
        { "c", BUFFER_LANG_C }, { "h", BUFFER_LANG_C },
        { "cpp", BUFFER_LANG_CPP }, { "hpp", BUFFER_LANG_CPP }, { "cc", BUFFER_LANG_CPP },
        { "cxx", BUFFER_LANG_CPP }, { "hh", BUFFER_LANG_CPP },
        { "cs", BUFFER_LANG_CSHARP },
        { "js", BUFFER_LANG_JAVASCRIPT }, { "mjs", BUFFER_LANG_JAVASCRIPT },
        { "cjs", BUFFER_LANG_JAVASCRIPT }, { "jsx", BUFFER_LANG_JAVASCRIPT },
        { "ts", BUFFER_LANG_TYPESCRIPT }, { "tsx", BUFFER_LANG_TYPESCRIPT },
    };
    i64 dot = name.len;
    while (dot > 0 && name.data[dot - 1] != '.') dot--;
    if (dot == 0) return BUFFER_LANG_FUNDAMENTAL;
    String8 ext = str8(name.data + dot, name.len - dot);
    for (i64 k = 0; k < ARRAY_COUNT(table); k++) {
        String8 e = str8_cstr(table[k].ext);
        b32 same = e.len == ext.len;
        for (i64 i = 0; same && i < e.len; i++) {
            u8 c = ext.data[i];
            if (c >= 'A' && c <= 'Z') c = (u8)(c + 32);
            same = c == e.data[i];
        }
        if (same) return table[k].language;
    }
    return BUFFER_LANG_FUNDAMENTAL;
}

const char *buffer_language_name(BufferLanguage language) {
    switch (language) {
    case BUFFER_LANG_FUNDAMENTAL: return "Fundamental";
    case BUFFER_LANG_JAI:         return "Jai";
    case BUFFER_LANG_C:           return "C";
    case BUFFER_LANG_CPP:         return "C++";
    case BUFFER_LANG_CSHARP:      return "C#";
    case BUFFER_LANG_JAVASCRIPT:  return "JavaScript";
    case BUFFER_LANG_TYPESCRIPT:  return "TypeScript";
    }
    return "?";
}

void buffer_set_path(Buffer *buf, String8 full_path) {
    buf->path = str8_copy(&buf->meta, full_path);
    buf->name = buffer_file_name(buf->path);
    buf->language = buffer_language_of(buf->name);
}

OsFileStatus buffer_load_file(Buffer *buf, String8 path) {
    ASSERT(buffer_size(buf) == 0 && buffer_nl_count(buf) == 0);
    u64 meta_mark = arena_pos(&buf->meta);
    String8 full = os_full_path(&buf->meta, path);
    if (!full.len) return OS_FILE_BAD_PATH;

    OsFile file;
    OsFileInfo info;
    OsFileStatus status = os_file_open_read(full, &file, &info);
    if (status != OS_FILE_OK) {
        arena_pop_to(&buf->meta, meta_mark);
        return status;
    }
    if (info.size > (i64)BUFFER_MAX_FILE_SIZE) status = OS_FILE_TOO_LARGE;

    u8 head[4] = { 0 };
    i64 head_len = MIN(info.size, 4);
    if (status == OS_FILE_OK) status = os_file_read(file, head, head_len);
    if (status == OS_FILE_OK) {
        b32 even = (info.size & 1) == 0;
        if (head_len >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) buf->encoding = BUFFER_UTF8_BOM;
        else if (head_len >= 2 && even && head[0] == 0xFF && head[1] == 0xFE) buf->encoding = BUFFER_UTF16LE;
        else if (head_len >= 2 && even && head[0] == 0xFE && head[1] == 0xFF) buf->encoding = BUFFER_UTF16BE;
        else buf->encoding = BUFFER_UTF8;

        if (buf->encoding == BUFFER_UTF8 || buf->encoding == BUFFER_UTF8_BOM) {
            // Straight into buffer memory: the head bytes after the BOM, then the rest of the file.
            i64 skip = buf->encoding == BUFFER_UTF8_BOM ? 3 : 0;
            i64 n = info.size - skip;
            if (!buffer_reserve_gap(buf, n)) {
                status = OS_FILE_OUT_OF_MEMORY;
            } else {
                memcpy(buf->text, head + skip, (size_t)(head_len - skip));
                status = os_file_read(file, buf->text + head_len - skip, info.size - head_len);
                buf->gap_start = n;
            }
        } else {
            status = buffer_load_utf16(buf, file, head + 2, head_len - 2, info.size - head_len,
                                       buf->encoding == BUFFER_UTF16BE);
        }
    }
    os_file_close(file);

    if (status == OS_FILE_OK) {
        i64 n = buffer_scan_loaded(buf, buf->gap_start);
        if (n < 0) status = OS_FILE_OUT_OF_MEMORY;
        else buf->gap_start = n;
    }
    if (status != OS_FILE_OK) {
        // Back to an empty buffer; committed memory stays for reuse.
        buf->gap_start = 0;
        buf->gap_end = buf->text_cap;
        buf->nl_front = 0;
        buf->nl_back = buf->nl_cap;
        buf->encoding = BUFFER_UTF8;
        buf->eol = BUFFER_EOL_LF;
        arena_pop_to(&buf->meta, meta_mark);
        return status;
    }
    buffer_set_path(buf, full);
    buf->read_only = info.read_only;
    buffer_mark_saved(buf);
    buf->file_size = info.size;
    buf->file_time = info.write_time;
    return OS_FILE_OK;
}

// ---------------------------------------------------------------------------
// Saving

typedef struct BufferWriter {
    OsFile file;
    OsFileStatus status;
    i64 used;
    u8 chunk[BUFFER_CHUNK];
} BufferWriter;

static void buffer_writer_flush(BufferWriter *w) {
    if (w->used && w->status == OS_FILE_OK) w->status = os_file_write(w->file, w->chunk, w->used);
    w->used = 0;
}

static void buffer_writer_put(BufferWriter *w, u8 *data, i64 len) {
    if (w->used + len > (i64)sizeof(w->chunk)) buffer_writer_flush(w);
    if (len >= (i64)sizeof(w->chunk)) { // large runs go straight to the file
        if (w->status == OS_FILE_OK) w->status = os_file_write(w->file, data, len);
        return;
    }
    memcpy(w->chunk + w->used, data, (size_t)len);
    w->used += len;
}

// Like utf8_decode, but a 3-byte encoded surrogate (WTF-8, from a UTF-16 load) is that unit.
static u32 buffer_decode_unit(u8 *s, i64 len, i64 *advance) {
    if (len >= 3 && s[0] == 0xED && (s[1] & 0xE0) == 0xA0 && (s[2] & 0xC0) == 0x80) {
        *advance = 3;
        return 0xD000u | ((u32)(s[1] & 0x3F) << 6) | (s[2] & 0x3Fu);
    }
    return utf8_decode(s, len, advance);
}

static void buffer_writer_put_unit(BufferWriter *w, u32 unit, b32 big_endian) {
    u8 b[2] = { (u8)(big_endian ? unit >> 8 : unit), (u8)(big_endian ? unit : unit >> 8) };
    buffer_writer_put(w, b, 2);
}

// Writes the text in the buffer's encoding and line-ending mode. Without conversion the two
// segments go to the file as they are; otherwise everything streams through the fixed chunk.
static OsFileStatus buffer_write_contents(Buffer *buf, OsFile file) {
    BufferWriter *w = (BufferWriter *)os_reserve(sizeof(BufferWriter)); // 64 KB: too big for the stack
    if (!w || !os_commit(w, sizeof(BufferWriter))) {
        if (w) os_release(w);
        return OS_FILE_OUT_OF_MEMORY;
    }
    w->file = file;
    w->status = OS_FILE_OK;
    w->used = 0;
    b32 crlf = buf->eol == BUFFER_EOL_CRLF;
    String8 seg[2];

    if (buf->encoding == BUFFER_UTF16LE || buf->encoding == BUFFER_UTF16BE) {
        b32 big = buf->encoding == BUFFER_UTF16BE;
        buffer_move_gap(buf, buffer_size(buf)); // one contiguous run, so no character straddles the gap
        u8 *t = buf->text;
        i64 n = buf->gap_start;
        buffer_writer_put_unit(w, 0xFEFF, big);
        for (i64 i = 0; i < n && w->status == OS_FILE_OK;) {
            i64 advance;
            u32 cp = buffer_decode_unit(t + i, n - i, &advance);
            i += advance;
            if (cp == '\n' && crlf) buffer_writer_put_unit(w, '\r', big);
            if (cp >= 0x10000) {
                cp -= 0x10000;
                buffer_writer_put_unit(w, 0xD800 + (cp >> 10), big);
                buffer_writer_put_unit(w, 0xDC00 + (cp & 0x3FF), big);
            } else {
                buffer_writer_put_unit(w, cp, big);
            }
        }
    } else {
        if (buf->encoding == BUFFER_UTF8_BOM) buffer_writer_put(w, (u8 *)"\xEF\xBB\xBF", 3);
        buffer_segments(buf, &seg[0], &seg[1]);
        for (i32 k = 0; k < 2; k++) {
            if (!crlf) {
                buffer_writer_put(w, seg[k].data, seg[k].len);
                continue;
            }
            for (i64 i = 0; i < seg[k].len && w->status == OS_FILE_OK;) {
                i64 nl = buffer_find_newline(seg[k].data, i, seg[k].len);
                buffer_writer_put(w, seg[k].data + i, nl - i);
                if (nl < seg[k].len) buffer_writer_put(w, (u8 *)"\r\n", 2);
                i = nl + 1;
            }
        }
    }
    buffer_writer_flush(w);
    OsFileStatus status = w->status;
    os_release(w);
    return status;
}

OsFileStatus buffer_save_as_opt(Buffer *buf, String8 path, b32 flush) {
    u64 mark = arena_pos(&buf->meta);
    String8 full = os_full_path(&buf->meta, path);
    if (!full.len) return OS_FILE_BAD_PATH;
    u64 mark_full = arena_pos(&buf->meta);

    OsFileInfo info;
    OsFileStatus status = os_file_info(full, &info);
    b32 exists = status == OS_FILE_OK;
    if (exists && info.is_dir) status = OS_FILE_IS_DIRECTORY;
    else if (exists && info.read_only) status = OS_FILE_READ_ONLY; // refused before anything is written
    else if (status == OS_FILE_NOT_FOUND) status = OS_FILE_OK;

    b32 done = 0;
    if (status == OS_FILE_OK && (!exists || info.swap_ok)) {
        // Write a temp file next to the target, flush it, swap it in. A failed write never
        // touches the original. Only creating or swapping the temp file falls back to in place.
        OsFile temp;
        String8 temp_path;
        OsFileStatus temp_status = os_file_create_temp(full, &buf->meta, &temp, &temp_path);
        if (temp_status == OS_FILE_OK) {
            status = buffer_write_contents(buf, temp);
            if (status == OS_FILE_OK && flush) status = os_file_flush(temp);
            os_file_close(temp);
            if (status == OS_FILE_OK) done = os_file_replace(full, temp_path) == OS_FILE_OK;
            if (!done) os_file_delete(temp_path);
        }
    }
    if (status == OS_FILE_OK && !done) {
        // In place: a symlink or a file with several hard links (a swap would detach them), a
        // directory that refuses the temp file, or a swap that failed.
        OsFile file;
        status = os_file_open_overwrite(full, &file);
        if (status == OS_FILE_OK) {
            status = buffer_write_contents(buf, file);
            if (status == OS_FILE_OK && flush) status = os_file_flush(file);
            os_file_close(file);
        }
    }
    arena_pop_to(&buf->meta, mark_full);
    if (status != OS_FILE_OK) {
        arena_pop_to(&buf->meta, mark);
        return status;
    }

    if (str8_equal(full, buf->path)) {
        arena_pop_to(&buf->meta, mark);
    } else {
        buf->path = full; // keeps the copy made by os_full_path
        buf->name = buffer_file_name(full);
        buf->language = buffer_language_of(buf->name);
    }
    buffer_mark_saved(buf);
    if (os_file_info(buf->path, &info) == OS_FILE_OK) {
        buf->file_size = info.size;
        buf->file_time = info.write_time;
    }
    return OS_FILE_OK;
}

OsFileStatus buffer_save_as(Buffer *buf, String8 path) {
    return buffer_save_as_opt(buf, path, 1);
}

OsFileStatus buffer_save(Buffer *buf) {
    return buffer_save_opt(buf, 1);
}

OsFileStatus buffer_save_opt(Buffer *buf, b32 flush) {
    if (!buf->path.len) return OS_FILE_NO_PATH;
    return buffer_save_as_opt(buf, buf->path, flush);
}

const char *buffer_status_text(OsFileStatus status) {
    switch (status) {
    case OS_FILE_OK:                return "ok";
    case OS_FILE_NOT_FOUND:         return "no such file or directory";
    case OS_FILE_ACCESS_DENIED:     return "access denied";
    case OS_FILE_SHARING_VIOLATION: return "in use by another program";
    case OS_FILE_TOO_LARGE:         return "larger than 1024 MB";
    case OS_FILE_DISK_FULL:         return "disk full";
    case OS_FILE_IS_DIRECTORY:      return "is a directory";
    case OS_FILE_BAD_PATH:          return "invalid path";
    case OS_FILE_READ_ONLY:         return "file is read-only";
    case OS_FILE_OUT_OF_MEMORY:     return "out of memory";
    case OS_FILE_NO_PATH:           return "buffer is not visiting a file";
    case OS_FILE_IO_ERROR:          return "I/O error";
    }
    return "unknown error";
}
