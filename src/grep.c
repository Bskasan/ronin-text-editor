// grep.c — see grep.h. Everything from grep_scan_init to grep_worker_main runs on a worker (or in tests on
// the main thread): it touches only the GrepSearch (its request read-only, the list below `published`, the
// atomics, the queue under the lock), the worker's own arena and the os_* functions platform.h marks "any
// thread". No LOG, no ASSERT.

// ---------------------------------------------------------------------------
// One file

void grep_scan_init(GrepScan *sc, Arena *arena, i64 chunk) {
    sc->chunk = MAX(chunk, 16);
    sc->win_cap = sc->chunk + GREP_WINDOW_EXTRA;
    sc->win = PUSH_ARRAY(arena, u8, sc->win_cap);
    sc->raw_cap = MAX(sc->chunk, (i64)GREP_PROBE) + 2;
    sc->raw = PUSH_ARRAY(arena, u8, sc->raw_cap);
}

// The '\n' bytes in p[0, n): their count, and the index of the last one in *last (-1: none). SSE2.
static i64 grep_newlines(const u8 *p, i64 n, i64 *last) {
    i64 count = 0, i = 0;
    *last = -1;
    __m128i nl = _mm_set1_epi8('\n');
    for (; i + 16 <= n; i += 16) {
        u32 m = (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)(p + i)), nl));
        if (!m) continue;
        unsigned long hi;
        _BitScanReverse(&hi, m);
        *last = i + (i64)hi;
        for (; m; m &= m - 1) count++;
    }
    for (; i < n; i++) {
        if (p[i] != '\n') continue;
        count++;
        *last = i;
    }
    return count;
}

// The file's bytes after its BOM: first what the head read left, then further reads; UTF-16 converted.
typedef struct GrepInput {
    GrepReadFn *read;
    void *data;
    u8 *raw;
    i64 head_at, head_n; // the head's bytes not handed out yet: raw[head_at, head_n)
    b32 utf16, big_endian;
    u32 pending;         // a high surrogate waiting for the next chunk
    u8 odd;              // UTF-16: a byte waiting for its pair
    b32 has_odd;
    b32 eof, error;
} GrepInput;

// Raw bytes: up to cap, 0 at the end.
static i64 grep_raw(GrepInput *in, u8 *dst, i64 cap) {
    if (in->head_at < in->head_n) {
        i64 n = MIN(cap, in->head_n - in->head_at);
        memcpy(dst, in->raw + in->head_at, (size_t)n);
        in->head_at += n;
        return n;
    }
    if (in->eof) return 0;
    i64 got = 0;
    if (in->read(in->data, dst, cap, &got) != OS_FILE_OK) {
        in->error = 1;
        got = 0;
    }
    if (!got) in->eof = 1;
    return got;
}

// Text bytes (UTF-8) into dst: at most cap (>= 8), 0 at the end.
static i64 grep_text(GrepInput *in, u8 *dst, i64 cap) {
    if (!in->utf16) return grep_raw(in, dst, cap);
    // UTF-16: at most (cap - 3) / 3 units, so their UTF-8 and a flushed surrogate fit.
    u8 units[KB(16)];
    i64 want = MIN((i64)sizeof(units), (cap - 3) / 3 * 2);
    for (;;) {
        i64 have = 0;
        if (in->has_odd) {
            units[have++] = in->odd;
            in->has_odd = 0;
        }
        i64 n = grep_raw(in, units + have, want - have);
        have += n;
        if (have & 1) {
            in->odd = units[--have];
            in->has_odd = 1;
        }
        i64 out = utf16_to_utf8_chunk(units, have, in->big_endian, &in->pending, dst);
        if (n == 0) { // the end: a pending surrogate, then nothing (an odd last byte is dropped)
            in->has_odd = 0;
            return out + utf16_flush(&in->pending, dst + out);
        }
        if (out) return out;
        // Only a held surrogate or an odd byte so far: read on.
    }
}

// The shown text of a match at c (window offsets): the whole line when it fits in GREP_LINE_SHOW bytes,
// otherwise up to GREP_CONTEXT bytes before the match and GREP_LINE_SHOW in all, cut on character
// boundaries with "…" at each cut. ls: the line's start in the window (-1: before it). The window holds at
// least GREP_LINE_SHOW + 1 bytes from c on, or the rest of the file (at_end).
void grep_show_text(u8 *out, const u8 *w, i64 wn, i64 ls, i64 c, i64 len, b32 at_end, GrepMatch *m) {
    i64 limit = MIN(wn, c + GREP_LINE_SHOW + 1);
    i64 le = -1; // the line's end ('\n' or the end of the file), when found within reach
    for (i64 i = c; i < limit && le < 0; i++) if (w[i] == '\n') le = i;
    if (le < 0 && at_end && limit == wn) le = wn; // the file ends there
    i64 s0, s1;
    b32 pre, post;
    if (ls >= 0 && le >= 0 && le - ls <= GREP_LINE_SHOW) {
        s0 = ls;
        s1 = le;
        pre = post = 0;
    } else {
        s0 = MAX(ls >= 0 ? ls : 0, c - GREP_CONTEXT);
        while (s0 < c && (w[s0] & 0xC0) == 0x80) s0++;
        i64 end = le >= 0 ? le : limit;
        s1 = MIN(end, s0 + GREP_LINE_SHOW);
        while (s1 > c && s1 < end && (w[s1] & 0xC0) == 0x80) s1--;
        pre = ls < 0 || s0 > ls;
        post = s1 < end || le < 0;
    }
    if (s1 > s0 && w[s1 - 1] == '\r' && !post) s1--;
    static const u8 ellipsis[3] = { 0xE2, 0x80, 0xA6 };
    i64 n = 0;
    if (pre) {
        memcpy(out, ellipsis, 3);
        n = 3;
    }
    memcpy(out + n, w + s0, (size_t)(s1 - s0));
    i64 off = n + (c - s0);
    n += s1 - s0;
    if (post) {
        memcpy(out + n, ellipsis, 3);
        n += 3;
    }
    m->text = str8(out, n);
    m->match_off = off;
    m->match_shown = CLAMP(MIN(c + len, s1) - c, 0, len);
}

GrepFileResult grep_file(const SearchPattern *pat, GrepScan *sc, i64 size, GrepReadFn *read, void *read_data, i64 max_matches,
                         volatile i32 *cancel, GrepEmitFn *emit, void *emit_data) {
    GrepFileResult r = { 0 };
    GrepInput in = { .read = read, .data = read_data, .raw = sc->raw };
    // The head: the encoding, the binary probe.
    i64 head = 0;
    while (head < GREP_PROBE && !in.eof) {
        i64 got = 0;
        if (read(read_data, sc->raw + head, GREP_PROBE - head, &got) != OS_FILE_OK) {
            r.kind = GREP_FILE_UNREADABLE;
            return r;
        }
        if (!got) in.eof = 1;
        head += got;
    }
    u8 *h = sc->raw;
    i64 skip = 0;
    if (head >= 2 && size % 2 == 0 && ((h[0] == 0xFF && h[1] == 0xFE) || (h[0] == 0xFE && h[1] == 0xFF))) {
        in.utf16 = 1;
        in.big_endian = h[0] == 0xFE;
        skip = 2;
    } else {
        if (head >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF) skip = 3;
        for (i64 i = 0; i < head; i++) {
            if (!h[i]) {
                r.kind = GREP_FILE_BINARY;
                return r;
            }
        }
    }
    in.head_at = skip;
    in.head_n = head;
    if (!pat->len) return r;

    u8 *w = sc->win;
    i64 wn = 0, wbase = 0;       // the window holds text bytes [wbase, wbase + wn)
    i64 next = 0;                // the first match start not examined yet (window offset)
    i64 counted = 0;             // newlines before it are counted (window offset)
    i64 line = 1, line_start = 0; // at `counted`: its line and that line's start (file offsets)
    for (b32 eof = 0; !eof;) {
        if (cancel && os_atomic_get(cancel)) {
            r.stopped = 1;
            break;
        }
        i64 n = grep_text(&in, w + wn, MIN(sc->chunk, sc->win_cap - wn));
        if (in.error) {
            r.kind = GREP_FILE_UNREADABLE;
            break;
        }
        eof = n == 0;
        wn += n;
        r.bytes += n;
        // Matches are taken only while the context after them is in the window (all of them at the end).
        i64 accept = eof ? wn : wn - GREP_LINE_SHOW;
        for (;;) {
            if (r.matches >= max_matches) {
                r.stopped = 1;
                return r;
            }
            i64 c = search_bytes_forward(pat, w, 0, wn, next, accept);
            if (c < 0) break;
            i64 last;
            line += grep_newlines(w + counted, c - counted, &last);
            if (last >= 0) line_start = wbase + counted + last + 1;
            counted = c;
            GrepMatch m = { .line = line, .col = wbase + c - line_start, .len = pat->len };
            grep_show_text(sc->show, w, wn, line_start >= wbase ? line_start - wbase : -1, c, pat->len, eof, &m);
            r.matches++;
            next = c + pat->len;
            if (!emit(emit_data, &m)) {
                r.stopped = 1;
                return r;
            }
        }
        if (eof) break;
        // Every start below accept - len + 1 was examined; count the lines up to there, keep the overlap.
        next = MAX(next, accept - pat->len + 1);
        if (next > counted) {
            i64 last;
            line += grep_newlines(w + counted, next - counted, &last);
            if (last >= 0) line_start = wbase + counted + last + 1;
            counted = next;
        }
        i64 keep = MAX(0, next - GREP_KEEP_BEFORE);
        memmove(w, w + keep, (size_t)(wn - keep));
        wn -= keep;
        wbase += keep;
        next -= keep;
        counted -= keep;
    }
    return r;
}

// ---------------------------------------------------------------------------
// The workers

enum { GREP_DONE_REC_BYTES = 1 + 4 + 1 + 8, GREP_MATCH_REC_HEAD = 1 + 4 + 8 + 8 + 8 + 2 + 2 + 2 };

i32 grep_path_compare(String8 a, String8 b) {
    for (i64 i = 0; i < a.len && i < b.len; i++) {
        u8 x = a.data[i], y = b.data[i];
        if (x == '\\') x = '/';
        if (y == '\\') y = '/';
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return x < y ? -1 : 1;
    }
    return a.len < b.len ? -1 : a.len > b.len ? 1 : 0;
}

String8 grep_list_path(GrepSearch *s, i32 file) {
    u8 *at = s->paths + s->offsets[file];
    u16 len;
    memcpy(&len, at, 2);
    return str8(at + 2, len);
}

static b32 grep_cancelled(GrepSearch *s) {
    return os_atomic_get(&s->cancel) != 0;
}

// Hands the current batch to the main thread and takes an empty one of its pool (allocated at the start;
// it waits for one to come back). False when cancelled while waiting.
static b32 grep_send(GrepWorker *wk, GrepBatch **batch, b32 last) {
    GrepSearch *s = wk->s;
    GrepBatch *b = *batch;
    b->last = last;
    os_lock(&s->lock);
    if (s->ready_last) s->ready_last->next = b;
    else s->ready = b;
    s->ready_last = b;
    os_unlock(&s->lock);
    os_wake_main();
    *batch = NULL;
    if (last) return 1;
    for (;;) {
        os_lock(&s->lock);
        GrepBatch *f = wk->free;
        if (f) wk->free = f->next;
        os_unlock(&s->lock);
        if (f) {
            f->next = NULL;
            f->used = 0;
            *batch = f;
            return !grep_cancelled(s);
        }
        if (grep_cancelled(s)) return 0;
        os_event_wait(wk->event, 0xFFFFFFFFu); // a batch comes back, or a cancel
    }
}

typedef struct GrepWorkerState {
    GrepWorker *wk;
    GrepBatch *batch;
    u64 last_send;
    u32 file;
    // The reader of the current file.
    OsFile handle;
    b32 first_read_done;
} GrepWorkerState;

// Room for `need` bytes in the batch: the batch is sent first when full. False: cancelled.
static b32 grep_room(GrepWorkerState *st, i64 need) {
    if (st->batch->used + need <= GREP_BATCH_BYTES) return 1;
    st->last_send = os_time_us();
    return grep_send(st->wk, &st->batch, 0);
}

static b32 grep_emit_match(void *data, const GrepMatch *m) {
    GrepWorkerState *st = data;
    i64 tlen = MIN(m->text.len, (i64)0xFFFF);
    if (!grep_room(st, GREP_MATCH_REC_HEAD + tlen)) return 0;
    u8 *at = st->batch->data + st->batch->used;
    u16 off = (u16)MIN(m->match_off, (i64)0xFFFF), shown = (u16)MIN(m->match_shown, (i64)0xFFFF), tl = (u16)tlen;
    at[0] = GREP_REC_MATCH;
    memcpy(at + 1, &st->file, 4);
    memcpy(at + 5, &m->line, 8);
    memcpy(at + 13, &m->col, 8);
    memcpy(at + 21, &m->len, 8);
    memcpy(at + 29, &off, 2);
    memcpy(at + 31, &shown, 2);
    memcpy(at + 33, &tl, 2);
    memcpy(at + GREP_MATCH_REC_HEAD, m->text.data, (size_t)tlen);
    st->batch->used += GREP_MATCH_REC_HEAD + tlen;
    return 1;
}

static b32 grep_emit_done(GrepWorkerState *st, GrepFileKind kind, i64 bytes) {
    if (!grep_room(st, GREP_DONE_REC_BYTES)) return 0;
    u8 *at = st->batch->data + st->batch->used;
    at[0] = GREP_REC_DONE;
    memcpy(at + 1, &st->file, 4);
    at[5] = (u8)kind;
    memcpy(at + 6, &bytes, 8);
    st->batch->used += GREP_DONE_REC_BYTES;
    return 1;
}

static OsFileStatus grep_read_file(void *data, u8 *dst, i64 cap, i64 *got) {
    GrepWorkerState *st = data;
    OsFileStatus status = os_file_read_some(st->handle, dst, cap, got);
    GrepSearch *s = st->wk->s;
    if (!st->first_read_done) { // dev: stand still with this file open (sharing tests)
        st->first_read_done = 1;
        if (s->dev_hold_open) {
            while (os_atomic_get(&s->dev_hold) && !grep_cancelled(s)) os_event_wait(st->wk->event, 0xFFFFFFFFu);
        }
    }
    return status;
}

// The next file in list order, or -1 when none can be claimed now.
static i32 grep_claim(GrepSearch *s) {
    for (;;) {
        i32 c = os_atomic_get(&s->claimed);
        i32 limit = MIN(os_atomic_get(&s->published), os_atomic_get(&s->claim_limit));
        if (c >= limit) return -1;
        if (os_atomic_cas(&s->claimed, c, c + 1) == c) return c;
    }
}

static b32 grep_skipped(GrepSearch *s, String8 rel) {
    i32 lo = 0, hi = s->skip_count;
    while (lo < hi) {
        i32 mid = (lo + hi) / 2;
        i32 c = grep_path_compare(s->skip[mid], rel);
        if (!c) return 1;
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return 0;
}

static void grep_worker_main(void *data) {
    GrepWorker *wk = data;
    GrepSearch *s = wk->s;
    while (os_atomic_get(&s->dev_stuck)) os_event_wait(wk->event, 0xFFFFFFFFu); // dev: a hung file system
    GrepScan *sc = PUSH_STRUCT(&wk->arena, GrepScan);
    grep_scan_init(sc, &wk->arena, GREP_CHUNK);
    GrepWorkerState st = { .wk = wk, .last_send = os_time_us() };
    // The whole pool now: one to fill, the others on its free list (paths are pushed and popped above them).
    for (i32 i = 0; i < GREP_BATCH_POOL; i++) {
        GrepBatch *b = PUSH_STRUCT(&wk->arena, GrepBatch);
        b->worker = wk->index;
        if (!i) {
            st.batch = b;
            continue;
        }
        os_lock(&s->lock);
        b->next = wk->free;
        wk->free = b;
        os_unlock(&s->lock);
    }
    u64 path_mark = arena_pos(&wk->arena);
    i32 files = 0;
    while (!grep_cancelled(s)) {
        i32 f = grep_claim(s);
        if (f < 0) {
            if (os_atomic_get(&s->list_done) && os_atomic_get(&s->claimed) >= os_atomic_get(&s->published)) break;
            if (st.batch->used) { // about to wait: what it has goes now
                st.last_send = os_time_us();
                if (!grep_send(wk, &st.batch, 0)) break;
            }
            os_event_wait(wk->event, 0xFFFFFFFFu);
            continue;
        }
        st.file = (u32)f;
        String8 rel = grep_list_path(s, f);
        GrepFileKind kind = GREP_FILE_TEXT;
        i64 bytes = 0;
        b32 go_on = 1;
        if (grep_skipped(s, rel)) {
            kind = GREP_FILE_MEMORY;
        } else {
            arena_pop_to(&wk->arena, path_mark);
            u8 *p = PUSH_ARRAY(&wk->arena, u8, s->root.len + 1 + rel.len);
            memcpy(p, s->root.data, (size_t)s->root.len);
            p[s->root.len] = '\\';
            for (i64 i = 0; i < rel.len; i++) p[s->root.len + 1 + i] = rel.data[i] == '/' ? '\\' : rel.data[i];
            i64 size;
            st.first_read_done = 0;
            if (os_file_open_read_arena(&wk->arena, str8(p, s->root.len + 1 + rel.len), &st.handle, &size) != OS_FILE_OK) {
                kind = GREP_FILE_UNREADABLE;
            } else {
                GrepFileResult r = grep_file(&s->pat, sc, size, grep_read_file, &st, s->max_matches, &s->cancel, grep_emit_match, &st);
                os_file_close(st.handle); // closed before the next file is claimed
                st.handle.handle = NULL;
                kind = r.kind;
                bytes = r.bytes;
                go_on = !grep_cancelled(s);
            }
        }
        if (!go_on || !grep_emit_done(&st, kind, bytes)) break;
        files++;
        if (s->dev_hold_after && files >= s->dev_hold_after && os_atomic_get(&s->dev_hold)) { // dev: stand still (a search "still running")
            if (st.batch->used) { // what it found so far shows meanwhile
                st.last_send = os_time_us();
                if (!grep_send(wk, &st.batch, 0)) break;
            }
            while (os_atomic_get(&s->dev_hold) && !grep_cancelled(s)) os_event_wait(wk->event, 0xFFFFFFFFu);
        }
        if (st.batch->used && os_time_us() - st.last_send >= GREP_SEND_US) {
            st.last_send = os_time_us();
            if (!grep_send(wk, &st.batch, 0)) break;
        }
    }
    if (!st.batch) { // cancelled while waiting for a batch: take one back, or make one
        os_lock(&s->lock);
        st.batch = wk->free;
        if (st.batch) wk->free = st.batch->next;
        os_unlock(&s->lock);
        if (!st.batch) st.batch = PUSH_STRUCT(&wk->arena, GrepBatch);
        st.batch->worker = wk->index;
        st.batch->next = NULL;
        st.batch->used = 0;
    }
    grep_send(wk, &st.batch, 1);
}

// ---------------------------------------------------------------------------
// The main thread's side

i32 grep_auto_workers(void) {
    return CLAMP(os_cpu_count() / 2, 1, 8); // --bench-grep: 8 of 16 read 100 MB 50% faster than 4, typing unchanged
}

GrepSearch *grep_create(String8 root, String8 needle, b32 fold, i64 max_matches, i32 workers) {
    Arena arena = arena_create(MB(64));
    GrepSearch *s = PUSH_STRUCT(&arena, GrepSearch);
    s->root = str8_copy(&arena, root);
    search_pattern_init(&s->pat, needle, fold);
    s->max_matches = MAX(max_matches, 1);
    s->worker_count = CLAMP(workers, 1, GREP_WORKERS_MAX);
    s->claim_limit = 0x7FFFFFFF;
    s->path_arena = arena_create(GB(4));
    s->offset_arena = arena_create(GB(1));
    s->paths = s->path_arena.base;
    s->offsets = (u32 *)s->offset_arena.base;
    for (i32 i = 0; i < s->worker_count; i++) {
        s->workers[i].s = s;
        s->workers[i].index = i;
        s->workers[i].arena = arena_create(MB(16));
    }
    s->arena = arena; // its position already counts this struct
    return s;
}

void grep_skip(GrepSearch *s, String8 *rel, i32 count) {
    s->skip = PUSH_ARRAY(&s->arena, String8, count);
    for (i32 i = 0; i < count; i++) {
        String8 r = str8_copy(&s->arena, rel[i]);
        i32 k = s->skip_count++;
        while (k > 0 && grep_path_compare(s->skip[k - 1], r) > 0) { // insertion sort: a handful
            s->skip[k] = s->skip[k - 1];
            k--;
        }
        s->skip[k] = r;
    }
}

b32 grep_start(GrepSearch *s) {
    i32 started = 0;
    for (i32 i = 0; i < s->worker_count; i++) {
        GrepWorker *wk = &s->workers[i];
        if (!os_event_create(&wk->event)) break;
        if (!os_thread_start(&wk->thread, grep_worker_main, wk, 1)) {
            os_event_destroy(wk->event);
            wk->event.handle = NULL;
            break;
        }
        wk->started = 1;
        started++;
    }
    s->worker_count = started;
    if (!started) s->ended = 1;
    return started > 0;
}

static void grep_wake_workers(GrepSearch *s) {
    for (i32 i = 0; i < s->worker_count; i++) if (s->workers[i].started && !s->workers[i].ended) os_event_set(s->workers[i].event);
}

void grep_list_add(GrepSearch *s, String8 rel) {
    u16 len = (u16)MIN(rel.len, (i64)0xFFFF);
    u32 *off = PUSH_STRUCT(&s->offset_arena, u32);
    *off = (u32)arena_pos(&s->path_arena);
    u8 *at = PUSH_ARRAY(&s->path_arena, u8, 2 + len);
    memcpy(at, &len, 2);
    memcpy(at + 2, rel.data, len);
    s->count++;
}

void grep_list_publish(GrepSearch *s, b32 done) {
    os_atomic_set(&s->published, s->count);
    if (done) os_atomic_set(&s->list_done, 1);
    grep_wake_workers(s);
}

void grep_set_claim_limit(GrepSearch *s, i32 limit) {
    if (os_atomic_get(&s->claim_limit) == limit) return;
    os_atomic_set(&s->claim_limit, limit);
    grep_wake_workers(s);
}

void grep_cancel(GrepSearch *s) {
    os_atomic_set(&s->cancel, 1);
    for (i32 i = 0; i < s->worker_count; i++) if (s->workers[i].started) os_event_set(s->workers[i].event);
}

#if TEAL_DEV
void grep_dev_release(GrepSearch *s) {
    os_atomic_set(&s->dev_hold, 0);
    os_atomic_set(&s->dev_stuck, 0);
    for (i32 i = 0; i < s->worker_count; i++) if (s->workers[i].started) os_event_set(s->workers[i].event);
}
#endif

i64 grep_drain(GrepSearch *s, GrepRecordFn *fn, void *data) {
    if (s->ended) return 0;
    os_lock(&s->lock);
    GrepBatch *b = s->ready;
    s->ready = s->ready_last = NULL;
    os_unlock(&s->lock);
    i64 records = 0;
    while (b) {
        GrepBatch *next = b->next;
        for (i64 at = 0; at < b->used;) {
            GrepRecord r = { 0 };
            u8 *p = b->data + at;
            r.kind = p[0];
            memcpy(&r.file, p + 1, 4);
            if (r.kind == GREP_REC_MATCH) {
                u16 off, shown, tl;
                memcpy(&r.match.line, p + 5, 8);
                memcpy(&r.match.col, p + 13, 8);
                memcpy(&r.match.len, p + 21, 8);
                memcpy(&off, p + 29, 2);
                memcpy(&shown, p + 31, 2);
                memcpy(&tl, p + 33, 2);
                r.match.match_off = off;
                r.match.match_shown = shown;
                r.match.text = str8(p + GREP_MATCH_REC_HEAD, tl);
                at += GREP_MATCH_REC_HEAD + tl;
            } else {
                r.status = (GrepFileKind)p[5];
                memcpy(&r.bytes, p + 6, 8);
                at += GREP_DONE_REC_BYTES;
            }
            if (fn) fn(data, &r);
            records++;
        }
        GrepWorker *wk = &s->workers[b->worker];
        if (b->last) { // the worker has returned from its last os_* call: the join does not wait
            os_thread_join(wk->thread, 0xFFFFFFFFu);
            wk->thread.handle = NULL;
            wk->ended = 1;
            s->workers_ended++;
        } else {
            os_lock(&s->lock);
            b->next = wk->free;
            wk->free = b;
            os_unlock(&s->lock);
            os_event_set(wk->event);
        }
        b = next;
    }
    if (s->workers_ended == s->worker_count) s->ended = 1;
    return records;
}

b32 grep_join(GrepSearch *s, u64 deadline_us) {
    b32 all = 1;
    for (i32 i = 0; i < s->worker_count; i++) {
        GrepWorker *wk = &s->workers[i];
        if (!wk->started || wk->ended) continue;
        u64 now = os_time_us();
        u32 left = now >= deadline_us ? 0 : (u32)((deadline_us - now) / 1000);
        if (os_thread_join(wk->thread, left)) {
            wk->thread.handle = NULL;
            wk->ended = 1;
            s->workers_ended++;
        } else {
            all = 0;
        }
    }
    if (all) s->ended = 1;
    return all;
}

void grep_free(GrepSearch *s) {
    for (i32 i = 0; i < GREP_WORKERS_MAX; i++) {
        GrepWorker *wk = &s->workers[i];
        if (wk->event.handle) os_event_destroy(wk->event);
        if (wk->arena.base) os_release(wk->arena.base);
    }
    os_release(s->path_arena.base);
    os_release(s->offset_arena.base);
    os_release(s->arena.base); // the struct itself lives there
}

#if TEAL_DEV
u64 grep_memory(GrepSearch *s) {
    u64 m = s->arena.committed + s->path_arena.committed + s->offset_arena.committed;
    for (i32 i = 0; i < GREP_WORKERS_MAX; i++) m += s->workers[i].arena.committed;
    return m;
}
#endif
