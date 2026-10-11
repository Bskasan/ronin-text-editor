// results.c — see results.h. Part of the app (included after finder.c).

// ---------------------------------------------------------------------------
// Slots and the buffer's text

static Results *results_of(App *app, Buffer *buf) {
    for (i32 i = 0; i < RESULTS_MAX; i++) if (app->results[i].buffer && app->results[i].buffer == buf) return &app->results[i];
    return NULL;
}

// The program writes the read-only buffer (no undo, never modified).
static void results_replace(Results *r, i64 start, i64 end, String8 text) {
    Buffer *b = r->buffer;
    b->inhibit_read_only = 1;
    buffer_replace(b, start, end, text);
    b->inhibit_read_only = 0;
    b->modified = 0;
}

// A cancelled search goes on ending on its own; app_wake frees it once its workers are done.
static void results_drop_search(App *app, GrepSearch *s, b32 cancel) {
    if (s->ended) { // every worker already joined: nothing left to wait for
        grep_free(s);
        return;
    }
    if (cancel) grep_cancel(s);
    if (app->grep_dying_count == RESULTS_DYING_MAX) { // never in practice: wait for the oldest
        GrepSearch *old = app->grep_dying[0];
        if (grep_join(old, os_time_us() + (u64)PROJECT_SHUTDOWN_MS * 1000)) grep_free(old);
        memmove(app->grep_dying, app->grep_dying + 1, sizeof(app->grep_dying[0]) * (RESULTS_DYING_MAX - 1));
        app->grep_dying_count--;
    }
    app->grep_dying[app->grep_dying_count++] = s;
}

// Stops what fills the buffer (workers, the list's feeding, a buffer being searched).
static void results_stop(App *app, Results *r) {
    if (r->grep) results_drop_search(app, r->grep, 1);
    r->grep = NULL;
    r->follow = NULL;
    r->source = NULL;
    r->mem_active = 0;
    r->mem = NULL;
    r->running = 0;
}

// A slot's arenas (each a 1 GB reservation, committed as used), in one list.
#define RESULTS_ARENAS 6
static void results_arenas(Results *r, Arena **list) {
    list[0] = &r->arena;
    list[1] = &r->loc_arena;
    list[2] = &r->state_arena;
    list[3] = &r->held[0];
    list[4] = &r->held[1];
    list[5] = &r->pend;
}

// The slot for a results buffer named `name` (made, or reused with its search stopped), its text and
// state cleared. NULL when every slot is taken.
static Results *results_open(App *app, String8 name, ResultsKind kind) {
    Results *r = NULL;
    for (i32 i = 0; i < RESULTS_MAX && !r; i++) if (app->results[i].buffer && str8_equal(app->results[i].buffer->name, name)) r = &app->results[i];
    for (i32 i = 0; i < RESULTS_MAX && !r; i++) if (!app->results[i].buffer) r = &app->results[i];
    if (!r) return NULL;
    if (r->buffer) {
        results_stop(app, r);
    } else {
        Buffer *buf = buffer_list_find_name(&app->buffers, name);
        if (!buf) buf = app_new_buffer(app, name);
        buf->read_only = 1;
        buffer_undo_enable(buf, 0); // a program buffer: no undo
        buf->keymap = BUFFER_KEYMAP_SEARCH_RESULTS;
        r->buffer = buf;
        Arena *arenas[RESULTS_ARENAS];
        results_arenas(r, arenas);
        for (i32 i = 0; i < RESULTS_ARENAS; i++) *arenas[i] = arena_create(GB(1));
    }
    Buffer *buf = r->buffer;
    Arena keep[RESULTS_ARENAS], *arenas[RESULTS_ARENAS];
    results_arenas(r, arenas);
    for (i32 i = 0; i < RESULTS_ARENAS; i++) keep[i] = *arenas[i];
    memset((u8 *)r + offsetof(Results, kind), 0, sizeof(*r) - offsetof(Results, kind));
    r->buffer = buf;
    for (i32 i = 0; i < RESULTS_ARENAS; i++) {
        *arenas[i] = keep[i];
        arena_reset(arenas[i]);
    }
    r->kind = kind;
    r->current = -1;
    r->first_line = 2;
    r->locs = (Location *)r->loc_arena.base;
    r->state = (ResultsFile *)r->state_arena.base;
    if (app->results_last == r) app->results_last = NULL;
    return r;
}

static void results_free(Results *r) {
    Arena *arenas[RESULTS_ARENAS];
    results_arenas(r, arenas);
    for (i32 i = 0; i < RESULTS_ARENAS; i++) os_release(arenas[i]->base);
    memset(r, 0, sizeof(*r));
}

// A path for the header: '/' separators and a trailing '/'.
static String8 results_dir_shown(Arena *arena, String8 dir) {
    String8 s = str8_fmt(arena, "%S/", dir);
    for (i64 i = 0; i < s.len; i++) if (s.data[i] == '\\') s.data[i] = '/';
    return s;
}

// The buffer's text at the start: the header, a blank line, the progress line.
static void results_begin_text(App *app, Results *r, String8 header) {
    String8 text = str8_fmt(&app->files_arena, "%S\n\n", header);
    results_replace(r, 0, buffer_size(r->buffer), text);
    r->tail = text.len;
    r->progress = str8(NULL, 0);
    // Point at the start everywhere (a replace of the whole text moves points to its end), so the first
    // location can take it.
    View *views[WINDOW_MAX];
    for (i32 i = 0, k = app_views(app, views); i < k; i++) {
        if (views[i]->buffer == r->buffer) view_set_point(views[i], &views[i]->cursors[0], 0);
    }
    i32 e = buffer_list_index(&app->buffers, r->buffer);
    if (e >= 0) buffer_marker_set(r->buffer, app->buffers.entries[e].point, 0);
}

static void results_set_progress(Results *r, String8 line) {
    if (str8_equal(line, r->progress)) return;
    i64 n = MIN(line.len, (i64)sizeof(r->progress_buf));
    memmove(r->progress_buf, line.data, (size_t)n);
    r->progress = str8(r->progress_buf, n);
    results_replace(r, r->tail, buffer_size(r->buffer), r->progress);
}

static const char *results_plural(i64 n, const char *one, const char *many) {
    return n == 1 ? one : many;
}

static void results_progress(App *app, Results *r) {
    if (!r->running) return;
    u8 text[160];
    i64 n;
    if (r->kind == RESULTS_PROJECT) {
        n = fmt_buf(text, sizeof(text), "Searching... %D/%D%s files, %D %s", r->files_seen, r->list_count, r->list_done ? "" : "+", r->found,
                    results_plural(r->found, "match", "matches"));
    } else {
        n = fmt_buf(text, sizeof(text), "Searching... %D %s", r->found, results_plural(r->found, "match", "matches"));
    }
    (void)app;
    results_set_progress(r, str8(text, n));
}

// "0.42 s"
static i64 results_seconds(u8 *out, i64 cap, u64 us) {
    u64 cs = (us + 5000) / 10000;
    return fmt_buf(out, cap, "%U.%02U s", cs / 100, cs % 100);
}

static void results_flush(App *app, Results *r);

static void results_finish(App *app, Results *r) {
    results_flush(app, r);
    r->running = 0;
    r->end_us = os_time_us();
    if (r->grep) results_drop_search(app, r->grep, r->limited); // ending on its own (or cancelled at the limit)
    r->grep = NULL;
    r->follow = NULL;
    r->mem_active = 0;
    u8 time[32], text[512];
    i64 tn = results_seconds(time, sizeof(time), r->end_us - r->start_us);
    i64 n;
    if (r->kind == RESULTS_PROJECT) {
        u8 head[96];
        i64 hn = r->limited ? fmt_buf(head, sizeof(head), "Stopped at %D %s (search_max_results) in %D %s", r->found,
                                       results_plural(r->found, "match", "matches"), r->files_with_matches,
                                       results_plural(r->files_with_matches, "file", "files"))
               : r->found ? fmt_buf(head, sizeof(head), "%D %s in %D %s", r->found, results_plural(r->found, "match", "matches"), r->files_with_matches,
                                    results_plural(r->files_with_matches, "file", "files"))
                          : fmt_buf(head, sizeof(head), "No matches");
        n = fmt_buf(text, sizeof(text), "%S; %D %s searched, %D binary skipped", str8(head, hn), r->files_searched,
                    results_plural(r->files_searched, "file", "files"), r->binary);
        if (r->unreadable) n += fmt_buf(text + n, (i64)sizeof(text) - n, ", %D unreadable", r->unreadable);
        if (r->from_memory) n += fmt_buf(text + n, (i64)sizeof(text) - n, ", %D from unsaved %s", r->from_memory,
                                         results_plural(r->from_memory, "buffer", "buffers"));
        n += fmt_buf(text + n, (i64)sizeof(text) - n, ", %S", str8(time, tn));
        if (r->incomplete) n += fmt_buf(text + n, (i64)sizeof(text) - n, " (the project's scan stopped: the file list is incomplete)");
    } else {
        n = r->found ? fmt_buf(text, sizeof(text), "%s%D %s, %S", r->limited ? "Stopped at " : "", r->found,
                               results_plural(r->found, "match", "matches"), str8(time, tn))
                     : fmt_buf(text, sizeof(text), "No matches, %S", str8(time, tn));
    }
    results_set_progress(r, str8(text, n));
    echo_message(&app->echo, "%S", str8(text, n));
}

// The file table entry of a path (or buffer name); the last one is reused while it is the same file.
static u32 results_file(Results *r, String8 name) {
    if (r->file_count && str8_equal(r->files[r->file_count - 1], name)) return (u32)(r->file_count - 1);
    if (r->file_count == r->file_cap) {
        i32 cap = MAX(64, r->file_cap * 2);
        String8 *files = PUSH_ARRAY(&r->arena, String8, cap);
        if (r->file_count) memcpy(files, r->files, sizeof(String8) * (size_t)r->file_count);
        r->files = files;
        r->file_cap = cap;
    }
    r->files[r->file_count] = str8_copy(&r->arena, name);
    return (u32)r->file_count++;
}

// One location: its line in the buffer ("path:line:col: text"), at the end of the list. False at the
// limit (search_max_results): nothing written, the run stops.
static b32 results_add(App *app, Results *r, String8 path, i64 line, i64 col, i64 len, String8 text, i64 match_off, i64 match_shown) {
    if (r->found >= app->config->settings.search_max_results) {
        r->limited = 1;
        return 0;
    }
    u8 buf[PROJECT_PATH_MAX + GREP_SHOW_CAP + 64];
    i64 pl = MIN(path.len, (i64)PROJECT_PATH_MAX);
    i64 n = 0;
    if (pl) {
        memcpy(buf, path.data, (size_t)pl);
        n = pl;
        buf[n++] = ':';
    }
    i64 num_at = n;
    n += fmt_buf(buf + n, 64, "%D:%D", line, col + 1);
    i64 num_len = n - num_at;
    buf[n++] = ':';
    buf[n++] = ' ';
    i64 tl = MIN(text.len, (i64)GREP_SHOW_CAP);
    memcpy(buf + n, text.data, (size_t)tl);
    n += tl;
    buf[n++] = '\n';
    Location *loc = PUSH_STRUCT(&r->loc_arena, Location);
    *loc = (Location){ .file = results_file(r, r->kind == RESULTS_OCCUR ? r->root : path), .col = (i32)MIN(col, (i64)0x7FFFFFFF), .line = line, .len = len, .path_len = (u16)pl,
                       .num_len = (u16)num_len, .match_off = (u16)MIN(match_off, tl), .match_shown = (u16)CLAMP(match_shown, 0, tl - MIN(match_off, tl)) };
    memcpy(PUSH_ARRAY(&r->pend, u8, n), buf, (size_t)n); // written with the frame's other lines (results_flush)
    r->count++;
    r->found++;
    if (r->found == 1) r->first_us = os_time_us();
    if (arena_pos(&r->pend) >= RESULTS_FLUSH_BYTES) results_flush(app, r);
    return 1;
}

// The lines added since the last flush go into the buffer with one insert, before the progress line.
static void results_flush(App *app, Results *r) {
    u64 n = arena_pos(&r->pend);
    if (!n) return;
    results_replace(r, r->tail, r->tail, str8(r->pend.base, (i64)n));
    r->tail += (i64)n;
    arena_reset(&r->pend);
    if (!r->point_set) { // the first location: point there in the windows that have not moved it (RET works at once)
        r->point_set = 1;
        i64 at = buffer_line_start(r->buffer, r->first_line);
        View *views[WINDOW_MAX];
        for (i32 i = 0, k = app_views(app, views); i < k; i++) {
            if (views[i]->buffer == r->buffer && view_point(views[i], &views[i]->cursors[0]) == 0) {
                view_set_point(views[i], &views[i]->cursors[0], at);
            }
        }
    }
}

// ---------------------------------------------------------------------------
// Buffers searched on the main thread: modified buffers in their file's place, unsaved ones after the
// list, occur's buffer.

static void results_mem_start(Results *r, Buffer *buf) {
    r->mem = buf;
    r->mem_active = 1;
    search_begin(&r->msearch, buf, r->query, r->fold, 1, 0, 0, buffer_size(buf));
}

// One match of the buffer being searched: its line, column and shown text (the rule of grep_show_text).
static b32 results_mem_match(App *app, Results *r, String8 path, i64 c, i64 len) {
    Buffer *buf = r->mem;
    i64 size = buffer_size(buf);
    i64 line = buffer_line_of(buf, c), ls = buffer_line_start(buf, line);
    i64 base = MAX(ls, c - GREP_KEEP_BEFORE), end = MIN(size, c + len + GREP_LINE_SHOW + 1);
    u8 w[SEARCH_NEEDLE_MAX + 2 * GREP_LINE_SHOW + 16];
    buffer_copy(buf, base, end, w);
    u8 out[GREP_SHOW_CAP];
    GrepMatch m = { 0 };
    grep_show_text(out, w, end - base, ls >= base ? ls - base : -1, c - base, len, end == size, &m);
    return results_add(app, r, path, line + 1, c - ls, len, m.text, m.match_off, m.match_shown);
}

// Searches r->mem until the deadline (budget: positions, tests). True when the buffer is done.
static b32 results_mem_step(App *app, Results *r, String8 path, u64 deadline, i64 budget) {
    i64 used = 0;
    for (;;) {
        if (!r->mem) return 1; // killed meanwhile
        i64 slice = SEARCH_SLICE_BYTES;
        if (budget) {
            if (used >= budget) return 0;
            slice = MIN(slice, budget - used);
        } else if (used && os_time_us() >= deadline) {
            return 0;
        }
        i64 before = r->msearch.examined;
        SearchStatus st = search_run(&r->msearch, r->mem, slice);
        used += MAX(r->msearch.examined - before, 1);
        if (st == SEARCH_NOT_FOUND) return 1;
        if (st == SEARCH_FOUND) {
            i64 c = r->msearch.match_start, len = r->msearch.match_end - c;
            if (!results_mem_match(app, r, path, c, len)) return 1; // the limit
            // An edit later restarts from here: after the last match written.
            search_restart(&r->msearch, r->mem, 1, c + len, 0, buffer_size(r->mem));
        }
    }
}

// ---------------------------------------------------------------------------
// The project search: the list, the held records, the order

static ResultsCandidate *results_candidate(Results *r, String8 rel) {
    i32 lo = 0, hi = r->cand_count;
    while (lo < hi) {
        i32 mid = (lo + hi) / 2;
        i32 c = grep_path_compare(r->cands[mid].rel, rel);
        if (!c) return &r->cands[mid];
        if (c < 0) lo = mid + 1;
        else hi = mid;
    }
    return NULL;
}

// The index being filled by a running scan of p.
static b32 results_filling(Project *p, ProjectIndex *x) {
    return p->scan && &p->index[p->scan_shadow ? !p->live : p->live] == x;
}

// Copies index entries into the search's list until the deadline (0: all), then publishes them; once the
// index is complete and copied, the list is done.
static void results_feed(Results *r, u64 deadline) {
    if (!r->follow || !r->grep) return;
    ProjectIndex *x = r->source;
    u8 path[PROJECT_PATH_MAX];
    i64 n = 0, count = x->files.base ? x->file_count : 0;
    while (r->fed < count) {
        if (deadline && (++n & 255) == 0 && os_time_us() >= deadline) break;
        String8 rel = str8(path, project_index_path(x, r->fed, path, sizeof(path)));
        grep_list_add(r->grep, rel);
        PUSH_STRUCT(&r->state_arena, ResultsFile);
        ResultsCandidate *c = results_candidate(r, rel);
        if (c) c->listed = 1;
        r->fed++;
        r->list_count++;
    }
    if (r->fed == count && !results_filling(r->follow, x)) {
        r->list_done = 1;
        r->follow = NULL;
        r->source = NULL;
    }
    grep_list_publish(r->grep, r->list_done);
}

// finder.c, before an index is reset or destroyed: a search following it copies the rest now. incomplete:
// the scan that filled it did not finish.
static void results_index_changing(App *app, ProjectIndex *x, b32 incomplete) {
    for (i32 i = 0; i < RESULTS_MAX; i++) {
        Results *r = &app->results[i];
        if (!r->follow || r->source != x) continue;
        results_feed(r, 0); // the rest, now
        if (!r->list_done) { // still being filled: the list ends here
            r->list_done = 1;
            r->follow = NULL;
            r->source = NULL;
            if (r->grep) grep_list_publish(r->grep, 1);
            incomplete = 1;
        }
        r->incomplete |= incomplete;
    }
}

typedef struct ResultsHeld { // a record in the held arena, followed by its text
    u32 next;                // offset + 1 of the next record of the file, 0 = none
    u16 off, shown, tlen;
    i64 line, col, len;
} ResultsHeld;

static void results_record(void *data, const GrepRecord *rec) {
    Results *r = data;
    if (rec->file >= (u32)r->list_count) return;
    ResultsFile *f = &r->state[rec->file];
    if (rec->kind == GREP_REC_DONE) {
        f->state = rec->status == GREP_FILE_MEMORY ? RESULTS_FILE_MEMORY : RESULTS_FILE_DONE;
        f->kind = (u8)rec->status;
        r->files_seen++;
        r->bytes += rec->bytes;
        return;
    }
    Arena *h = &r->held[r->held_slot];
    u64 at = ALIGN_UP_POW2(arena_pos(h), 8);
    ResultsHeld *e = arena_push(h, sizeof(ResultsHeld) + (u64)rec->match.text.len, 8);
    *e = (ResultsHeld){ 0, (u16)rec->match.match_off, (u16)rec->match.match_shown, (u16)rec->match.text.len, rec->match.line, rec->match.col,
                        rec->match.len };
    memcpy(e + 1, rec->match.text.data, (size_t)rec->match.text.len);
    if (f->tail) ((ResultsHeld *)(h->base + f->tail - 1))->next = (u32)(at + 1);
    else f->head = (u32)(at + 1);
    f->tail = (u32)(at + 1);
    r->held_live += sizeof(ResultsHeld) + (u64)rec->match.text.len;
}

// The claim window and the held records' limit (tests lower them).
static i32 results_ahead(App *app) {
#if TEAL_DEV
    if (app->dev_claim_ahead) return app->dev_claim_ahead;
#endif
    (void)app;
    return RESULTS_CLAIM_AHEAD;
}

static u64 results_held_max(App *app) {
#if TEAL_DEV
    if (app->dev_held_max) return app->dev_held_max;
#endif
    (void)app;
    return RESULTS_HELD_MAX;
}

// The held records move to the other arena (the files not in the buffer yet are at most the claim window
// past emit_next), so written records give their space back.
static void results_compact(Results *r) {
    Arena *from = &r->held[r->held_slot], *to = &r->held[!r->held_slot];
    arena_reset(to);
    i32 end = MIN(r->list_count, r->emit_next + RESULTS_CLAIM_AHEAD + GREP_WORKERS_MAX);
    for (i32 i = r->emit_next; i < end; i++) {
        ResultsFile *f = &r->state[i];
        u32 prev = 0;
        for (u32 at = f->head; at;) {
            ResultsHeld *e = (ResultsHeld *)(from->base + at - 1);
            u64 pos = ALIGN_UP_POW2(arena_pos(to), 8);
            ResultsHeld *n = arena_push(to, sizeof(ResultsHeld) + e->tlen, 8);
            memcpy(n, e, sizeof(ResultsHeld) + e->tlen);
            n->next = 0;
            if (prev) ((ResultsHeld *)(to->base + prev - 1))->next = (u32)(pos + 1);
            else f->head = (u32)(pos + 1);
            prev = (u32)(pos + 1);
            at = e->next;
        }
        f->tail = prev;
    }
    arena_reset(from);
    r->held_slot = !r->held_slot;
}

// Writes the list's files in order until the deadline: each one as soon as it and every earlier one are
// done (a MEMORY entry: its buffer searched now, in its place), then the unsaved buffers not in the list,
// then the summary. True while there is work it can do now (not waiting for the workers).
static b32 results_emit(App *app, Results *r, u64 deadline, i64 budget) {
    i64 lines = 0;
    for (;;) {
        if (!r->running) return 0;
        if (r->mem_active) {
            if (!results_mem_step(app, r, r->mem_path, deadline, budget)) return 1;
            r->mem_active = 0;
            r->mem = NULL;
            if (r->limited) {
                results_finish(app, r);
                return 0;
            }
            if (r->kind == RESULTS_OCCUR) {
                results_finish(app, r);
                return 0;
            }
            if (r->found > r->mem_found_before) r->files_with_matches++;
            continue;
        }
        if (r->emit_next < r->list_count) {
            ResultsFile *f = &r->state[r->emit_next];
            if (f->state == RESULTS_FILE_PENDING) return 0; // the workers have it
            String8 rel = grep_list_path(r->grep, r->emit_next);
            if (f->state == RESULTS_FILE_MEMORY) {
                f->state = RESULTS_FILE_DONE;
                f->kind = GREP_FILE_TEXT;
                r->emit_next++;
                ResultsCandidate *c = results_candidate(r, rel);
                r->files_searched++;
                if (c && c->buffer) {
                    r->from_memory++;
                    r->mem_path = str8_copy(&r->arena, rel);
                    r->mem_found_before = r->found;
                    results_mem_start(r, c->buffer);
                }
                continue;
            }
            // Its held records, in order.
            Arena *h = &r->held[r->held_slot];
            i64 before = r->found;
            b32 stop = 0;
            for (u32 at = f->head; at && !stop;) {
                ResultsHeld *e = (ResultsHeld *)(h->base + at - 1);
                if (!results_add(app, r, rel, e->line, e->col, e->len, str8((u8 *)(e + 1), e->tlen), e->off, e->shown)) stop = 1;
                r->held_live -= sizeof(ResultsHeld) + e->tlen;
                at = e->next;
                lines++;
            }
            f->head = f->tail = 0;
            if (r->found > before) r->files_with_matches++;
            if (f->kind == GREP_FILE_BINARY) r->binary++;
            else if (f->kind == GREP_FILE_UNREADABLE) r->unreadable++;
            else r->files_searched++;
            r->emit_next++;
            if (stop || r->limited) {
                results_finish(app, r);
                return 0;
            }
            if (!r->held_live) arena_reset(h);
            else if (arena_pos(h) > 2 * r->held_live + MB(4)) results_compact(r);
            lines++; // a file without matches counts too
            if (lines >= RESULTS_LINES_PER_CHECK) {
                lines = 0;
                if (budget ? 0 : os_time_us() >= deadline) return 1;
            }
            continue;
        }
        if (!r->list_done) return 0; // more files to come
        // The unsaved buffers whose files are not in the list, in the order of their paths.
        while (r->after_next < r->cand_count) {
            ResultsCandidate *c = &r->cands[r->after_next++];
            if (c->listed || !c->buffer || !buffer_size(c->buffer)) continue;
            r->from_memory++;
            r->files_searched++;
            r->mem_path = c->rel;
            r->mem_found_before = r->found;
            results_mem_start(r, c->buffer);
            break;
        }
        if (r->mem_active) continue;
        results_finish(app, r);
        return 0;
    }
}

// Some results buffer is being filled.
static b32 results_running(App *app) {
    for (i32 i = 0; i < RESULTS_MAX; i++) if (app->results[i].buffer && app->results[i].running) return 1;
    return 0;
}

// The frame's results work within its deadline: the lists fed, the files written in order, buffers
// searched. True while some of it can go on at once (another frame).
static b32 results_work(App *app, u64 deadline, i64 budget) {
    b32 pending = 0;
#if TEAL_DEV
    u64 t0 = os_time_us();
#endif
    for (i32 i = 0; i < RESULTS_MAX; i++) {
        Results *r = &app->results[i];
        if (!r->buffer || !r->running) continue;
        results_feed(r, budget ? 0 : deadline);
        pending |= results_emit(app, r, deadline, budget);
        // The claim window moves once a frame (it wakes every worker): emit_next + the window, unless the
        // held records are over their limit.
        if (r->grep && r->running) {
            if (r->held_live <= results_held_max(app)) grep_set_claim_limit(r->grep, r->emit_next + results_ahead(app));
#if TEAL_DEV
            else r->dev_held_back++;
#endif
        }
        results_flush(app, r);
        results_progress(app, r);
        if (r->follow && r->source && r->source->files.base && r->fed < r->source->file_count) pending = 1;
    }
#if TEAL_DEV
    u64 took = os_time_us() - t0;
    app->dev_results_max_us = MAX(app->dev_results_max_us, took);
#endif
    return pending;
}

// The workers' records (app_wake): true when a frame is due.
static b32 results_wake(App *app) {
    b32 frame = 0;
    for (i32 i = 0; i < RESULTS_MAX; i++) {
        Results *r = &app->results[i];
        if (!r->buffer || !r->running) continue;
        if (r->grep && grep_drain(r->grep, results_record, r)) frame = 1;
#if TEAL_DEV
        if (r->grep) r->dev_max_ahead = MAX(r->dev_max_ahead, os_atomic_get(&r->grep->claimed) - r->emit_next);
#endif
        if (r->follow && r->source && r->source->files.base && r->fed < r->source->file_count) frame = 1;
    }
    for (i32 i = 0; i < app->grep_dying_count;) {
        grep_drain(app->grep_dying[i], NULL, NULL);
        if (app->grep_dying[i]->ended) {
            grep_free(app->grep_dying[i]);
            app->grep_dying[i] = app->grep_dying[--app->grep_dying_count];
        } else {
            i++;
        }
    }
    return frame;
}

// files.c, before a buffer is killed: a results buffer stops and goes; a buffer being searched is dropped.
static void results_buffer_killed(App *app, Buffer *buf) {
    for (i32 i = 0; i < RESULTS_MAX; i++) {
        Results *r = &app->results[i];
        if (!r->buffer) continue;
        for (i32 k = 0; k < r->cand_count; k++) if (r->cands[k].buffer == buf) r->cands[k].buffer = NULL;
        if (r->mem == buf) {
            r->mem = NULL;
        }
        if (r->buffer == buf) {
            results_stop(app, r);
            if (app->results_last == r) app->results_last = NULL;
            results_free(r);
        }
    }
}

// Exit: every search is cancelled (its workers then get the shared bound with the scans).
static void results_cancel_all(App *app) {
    for (i32 i = 0; i < RESULTS_MAX; i++) if (app->results[i].buffer) results_stop(app, &app->results[i]);
}

// ... and joined by the shared deadline; a worker still inside a file-system call is left to the exit.
static void results_shutdown(App *app, u64 deadline) {
    for (i32 i = 0; i < app->grep_dying_count; i++) {
        GrepSearch *s = app->grep_dying[i];
        grep_drain(s, NULL, NULL);
        if (grep_join(s, deadline)) {
            grep_free(s);
        } else {
            for (i32 k = 0; k < s->worker_count; k++) {
                if (!s->workers[k].started || s->workers[k].ended) continue;
                LOG("search: a worker of the search under %S did not end within %d ms of the exit: left to the process exit (abandoned)",
                    s->root, PROJECT_SHUTDOWN_MS);
#if TEAL_DEV
                app->dev_grep_abandoned++;
                os_dev_thread_abandoned();
#endif
            }
        }
    }
    app->grep_dying_count = 0;
    for (i32 i = 0; i < RESULTS_MAX; i++) if (app->results[i].buffer) results_free(&app->results[i]);
}

// ---------------------------------------------------------------------------
// Starting a search

// The project search for `query` under `root` into *search*, shown through the pop-up rule and selected.
static Results *results_project_search(CommandContext *ctx, String8 root, String8 query, b32 fold, b32 select) {
    App *app = ctx->app;
    Results *r = results_open(app, STR8_LIT("*search*"), RESULTS_PROJECT);
    if (!r) {
        echo_message(ctx->echo, "Too many results buffers");
        return NULL;
    }
    r->query = str8_copy(&r->arena, query);
    r->root = str8_copy(&r->arena, root);
    r->fold = fold;
    results_begin_text(app, r, str8_fmt(&app->files_arena, "Search for \"%S\" in %S (%s)", query, results_dir_shown(&app->files_arena, root),
                                        fold ? "ignoring case" : "case-sensitive"));
    // The buffers with unsaved changes under the root: searched from memory.
    i32 cap = app->buffers.count;
    r->cands = PUSH_ARRAY(&r->arena, ResultsCandidate, MAX(cap, 1));
    String8 *skip = PUSH_ARRAY(&r->arena, String8, MAX(cap, 1));
    for (i32 i = 0; i < app->buffers.count; i++) {
        Buffer *b = app->buffers.entries[i].buffer;
        if (!b->path.len || !b->modified || results_of(app, b) || b->path.len <= root.len + 1 || !project_path_inside(b->path, root)) continue;
        String8 rel = str8_copy(&r->arena, str8(b->path.data + root.len + 1, b->path.len - root.len - 1));
        for (i64 k = 0; k < rel.len; k++) if (rel.data[k] == '\\') rel.data[k] = '/';
        i32 k = r->cand_count++;
        while (k > 0 && grep_path_compare(r->cands[k - 1].rel, rel) > 0) {
            r->cands[k] = r->cands[k - 1];
            k--;
        }
        r->cands[k] = (ResultsCandidate){ b, rel, 0 };
    }
    for (i32 i = 0; i < r->cand_count; i++) skip[i] = r->cands[i].rel;
    // The file list: the index if it is fresh, else the running scan's files as they arrive.
    Project *p = finder_project(app, root);
    if (!p->scan && (!p->scanned || p->stale)) finder_scan(app, p);
    r->follow = p;
    r->source = &p->index[p->scan && p->scan_shadow ? !p->live : p->live];
    Settings *st = &app->config->settings;
    i32 workers = st->search_workers ? st->search_workers : grep_auto_workers();
    GrepSearch *s = grep_create(root, query, fold, st->search_max_results, workers);
    grep_skip(s, skip, r->cand_count);
    grep_set_claim_limit(s, results_ahead(app));
#if TEAL_DEV
    if (app->dev_grep_hold_after) {
        s->dev_hold = 1;
        s->dev_hold_after = app->dev_grep_hold_after;
    }
    if (app->dev_grep_stuck_next) {
        s->dev_stuck = 1;
        app->dev_grep_stuck_next = 0;
    }
    app->dev_last_grep = s;
#endif
    if (!grep_start(s)) {
        grep_free(s);
        r->follow = NULL;
        r->source = NULL;
        results_set_progress(r, STR8_LIT("Cannot search: no thread"));
        echo_message(ctx->echo, "Cannot search: no thread");
        return r;
    }
    r->grep = s;
    r->running = 1;
    r->start_us = os_time_us();
    results_progress(app, r);
    app->results_last = r;
    // A window that showed it before remembers an old point: the start, until the first location.
    if (select) {
        app_pop_to_buffer(ctx, r->buffer);
        view_set_point(ctx->view, ctx->cursor, 0);
    } else {
        View *v = app_display_buffer(app, r->buffer, 0);
        view_set_point(v, &v->cursors[0], 0);
    }
    return r;
}

// The default of a search prompt: the active region's text (one line, at most 256 bytes), else the symbol
// at point (letters, digits and '_'); empty when there is none.
static String8 results_default(CommandContext *ctx) {
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    Cursor *c = &v->cursors[0];
    i64 lo, hi;
    if (view_region_active(v, c, ctx->settings) && view_region(v, c, &lo, &hi) && hi > lo && hi - lo <= 256) {
        String8 t = buffer_text(buf, ctx->scratch, lo, hi);
        b32 newline = 0;
        for (i64 i = 0; i < t.len; i++) newline |= t.data[i] == '\n';
        if (!newline) return t;
    }
    i64 p = view_point(v, c);
    for (i32 k = 0; k < 2; k++) { // at point, else just before it (point at the end of a symbol)
        i64 at = p - k;
        if (at < 0 || at >= buffer_size(buf) || !view_is_word_char(buf, at, 1)) continue;
        i64 s, e;
        view_word_bounds(buf, at, 1, &s, &e);
        if (e > s) return buffer_text(buf, ctx->scratch, s, e);
    }
    return str8(NULL, 0);
}

static void results_project_done(CommandContext *ctx, MiniResult *res) {
    App *app = ctx->app;
    String8 query = res->text.len ? res->text : ctx->mini->state.text;
    if (!query.len) {
        echo_message(ctx->echo, "Nothing to search for");
        return;
    }
    if (query.len > SEARCH_NEEDLE_MAX) {
        echo_message(ctx->echo, "Search string longer than %d bytes", SEARCH_NEEDLE_MAX);
        return;
    }
    String8 root = finder_root(app, ctx->view->buffer);
    if (!root.len) {
        echo_message(ctx->echo, PROJECT_NOT_IN_PROJECT);
        return;
    }
    if (!res->text.len) minibuffer_history_add(ctx->mini, MINI_HISTORY_SEARCH, query);
    results_project_search(ctx, root, query, !search_has_upper(query), 1);
}

static void cmd_project_search(CommandContext *ctx) {
    App *app = ctx->app;
    if (ctx->mini->active) {
        echo_message(ctx->echo, "Command attempted to use minibuffer while in minibuffer");
        return;
    }
    if (!finder_root(app, ctx->view->buffer).len) {
        echo_message(ctx->echo, PROJECT_NOT_IN_PROJECT);
        return;
    }
    String8 def = results_default(ctx);
    String8 prompt = def.len ? str8_fmt(ctx->scratch, "Search project for (default %S): ", def) : STR8_LIT("Search project for: ");
    MiniRequest req = { .kind = MINI_TEXT, .prompt = prompt, .history = MINI_HISTORY_SEARCH, .done = results_project_done };
    if (minibuffer_read(ctx, &req)) ctx->mini->state.text = str8_copy(&ctx->mini->arena, def);
}

// ---------------------------------------------------------------------------
// Visiting locations

typedef enum ResultsVisit {
    RESULTS_VISIT_SELECT, // in another window, selected (RET; next-error from the results window)
    RESULTS_VISIT_SHOW,   // in another window, the results window stays selected (n, p)
    RESULTS_VISIT_HERE,   // in the selected window (next-error from elsewhere)
} ResultsVisit;

// The windows showing the results buffer: point on the location's line.
static void results_show_entry(App *app, Results *r, i64 idx) {
    i64 at = buffer_line_start(r->buffer, r->first_line + idx);
    View *views[WINDOW_MAX];
    for (i32 i = 0, k = app_views(app, views); i < k; i++) {
        if (views[i]->buffer != r->buffer) continue;
        view_set_point(views[i], &views[i]->cursors[0], at);
        view_ensure_visible(views[i]);
    }
}

static b32 results_visit(CommandContext *ctx, Results *r, i64 idx, ResultsVisit how) {
    App *app = ctx->app;
    Location *loc = &r->locs[idx];
    String8 name = r->files[loc->file];
    Buffer *buf = NULL;
    if (r->kind == RESULTS_OCCUR) {
        buf = buffer_list_find_name(&app->buffers, name);
        if (!buf) {
            echo_message(ctx->echo, "No buffer named %S", name);
            return 0;
        }
    } else {
        arena_reset(&app->files_arena);
        String8 path = str8_fmt(&app->files_arena, "%S\\%S", r->root, name);
        for (i64 i = r->root.len; i < path.len; i++) if (path.data[i] == '/') path.data[i] = '\\';
        String8 full = os_full_path(&app->files_arena, path);
        buf = buffer_list_find_path(&app->buffers, full.len ? full : path);
        OsFileInfo info;
        if (!buf && os_file_info(path, &info) != OS_FILE_OK) {
            files_echo(ctx->echo, "%S no longer exists", path);
            return 0;
        }
        if (!buf) buf = app_find_file(app, path);
        if (!buf) return 0;
    }
    r->current = idx;
    app->results_last = r;
    results_show_entry(app, r, idx);
    View *v;
    if (how == RESULTS_VISIT_HERE) {
        v = ctx->view;
        app_switch_buffer(app, v, buf);
    } else {
        v = app_display_buffer(app, buf, how == RESULTS_VISIT_SELECT);
        if (how == RESULTS_VISIT_SELECT) ctx->view = v;
    }
    i64 line = CLAMP(loc->line - 1, 0, buffer_line_count(buf) - 1);
    i64 ls = buffer_line_start(buf, line), le = buffer_line_end(buf, line);
    view_set_point(v, &v->cursors[0], buffer_snap_char(buf, MIN(ls + loc->col, le)));
    view_ensure_visible(v);
    if (ctx->view == v) ctx->cursor = &v->cursors[0];
    buffer_list_touch(&app->buffers, buf);
    return 1;
}

// The location on point's line in a results buffer, or -1.
static i64 results_at_point(Results *r, View *v) {
    i64 idx = buffer_line_of(r->buffer, view_point(v, &v->cursors[0])) - r->first_line;
    return idx >= 0 && idx < r->count ? idx : -1;
}

// The results buffer of the command's window, or NULL with a message.
static Results *results_here(CommandContext *ctx) {
    Results *r = results_of(ctx->app, ctx->view->buffer);
    if (!r) echo_message(ctx->echo, "Not in a search results buffer");
    return r;
}

static void cmd_compile_goto_error(CommandContext *ctx) {
    Results *r = results_here(ctx);
    if (!r) return;
    i64 idx = results_at_point(r, ctx->view);
    if (idx < 0) {
        echo_message(ctx->echo, "No match on this line");
        return;
    }
    results_visit(ctx, r, idx, RESULTS_VISIT_SELECT);
}

// n / p: from point's line to the next (previous) location, shown in another window.
static void results_step_here(CommandContext *ctx, i32 dir) {
    Results *r = results_here(ctx);
    if (!r) return;
    i64 line = buffer_line_of(r->buffer, view_point(ctx->view, &ctx->view->cursors[0])) - r->first_line;
    i64 idx = dir > 0 ? (line < 0 ? 0 : line + 1) : (line >= r->count ? r->count - 1 : line - 1);
    if (idx < 0 || idx >= r->count) {
        echo_message(ctx->echo, "No more matches");
        return;
    }
    results_visit(ctx, r, idx, RESULTS_VISIT_SHOW);
}

static void cmd_next_error_no_select(CommandContext *ctx) { results_step_here(ctx, 1); }
static void cmd_previous_error_no_select(CommandContext *ctx) { results_step_here(ctx, -1); }

// next-error / previous-error: the most recent results buffer's next (previous) location, from any window.
static void results_step(CommandContext *ctx, i32 dir) {
    App *app = ctx->app;
    Results *r = app->results_last && app->results_last->buffer ? app->results_last : NULL;
    if (!r) {
        echo_message(ctx->echo, "No next-error capable buffer found");
        return;
    }
    if (ctx->view == app->mini.view) {
        echo_message(ctx->echo, "Command attempted to use minibuffer while in minibuffer");
        return;
    }
    i64 idx = r->current + dir;
    if (r->current < 0 && dir < 0) idx = -1;
    if (idx < 0 || idx >= r->count) {
        echo_message(ctx->echo, "No more matches");
        return;
    }
    results_visit(ctx, r, idx, ctx->view->buffer == r->buffer ? RESULTS_VISIT_SELECT : RESULTS_VISIT_HERE);
}

static void cmd_next_error(CommandContext *ctx) { results_step(ctx, 1); }
static void cmd_previous_error(CommandContext *ctx) { results_step(ctx, -1); }

// ---------------------------------------------------------------------------
// occur: the same results for one buffer, searched on the main thread inside the frame budget

static void results_occur(CommandContext *ctx, Buffer *target, String8 query, b32 fold) {
    App *app = ctx->app;
    if (target->name.len == 7 && str8_equal(target->name, STR8_LIT("*occur*"))) {
        echo_message(ctx->echo, "occur cannot search *occur* itself");
        return;
    }
    Results *r = results_open(app, STR8_LIT("*occur*"), RESULTS_OCCUR);
    if (!r) {
        echo_message(ctx->echo, "Too many results buffers");
        return;
    }
    r->query = str8_copy(&r->arena, query);
    r->root = str8_copy(&r->arena, target->name); // occur's "root": the buffer, by name
    r->fold = fold;
    results_begin_text(app, r, str8_fmt(&app->files_arena, "Lines matching \"%S\" in buffer %S (%s)", query, target->name,
                                        fold ? "ignoring case" : "case-sensitive"));
    r->running = 1;
    r->start_us = os_time_us();
    r->mem_path = str8(NULL, 0);
    r->mem_found_before = 0;
    results_mem_start(r, target);
    results_progress(app, r);
    app->results_last = r;
    app_pop_to_buffer(ctx, r->buffer);
    view_set_point(ctx->view, ctx->cursor, 0);
}

static void results_occur_done(CommandContext *ctx, MiniResult *res) {
    String8 query = res->text.len ? res->text : ctx->mini->state.text;
    if (!query.len) {
        echo_message(ctx->echo, "Nothing to search for");
        return;
    }
    if (query.len > SEARCH_NEEDLE_MAX) {
        echo_message(ctx->echo, "Search string longer than %d bytes", SEARCH_NEEDLE_MAX);
        return;
    }
    if (!res->text.len) minibuffer_history_add(ctx->mini, MINI_HISTORY_SEARCH, query);
    results_occur(ctx, ctx->view->buffer, query, !search_has_upper(query));
}

static void cmd_occur(CommandContext *ctx) {
    if (ctx->mini->active) {
        echo_message(ctx->echo, "Command attempted to use minibuffer while in minibuffer");
        return;
    }
    String8 def = results_default(ctx);
    String8 prompt = def.len ? str8_fmt(ctx->scratch, "List lines matching (default %S): ", def) : STR8_LIT("List lines matching: ");
    MiniRequest req = { .kind = MINI_TEXT, .prompt = prompt, .history = MINI_HISTORY_SEARCH, .done = results_occur_done };
    if (minibuffer_read(ctx, &req)) ctx->mini->state.text = str8_copy(&ctx->mini->arena, def);
}

static void cmd_recompile(CommandContext *ctx) {
    Results *r = results_here(ctx);
    if (!r) return;
    String8 root = str8_copy(ctx->scratch, r->root), query = str8_copy(ctx->scratch, r->query);
    if (r->kind == RESULTS_PROJECT) {
        results_project_search(ctx, root, query, r->fold, 1);
        return;
    }
    Buffer *target = buffer_list_find_name(&ctx->app->buffers, root);
    if (!target) {
        echo_message(ctx->echo, "No buffer named %S", root);
        return;
    }
    results_occur(ctx, target, query, r->fold);
}

// q: the results window goes (when there are others), else the window shows another buffer.
static void cmd_quit_window(CommandContext *ctx) {
    App *app = ctx->app;
    if (ctx->view == app->mini.view) return;
    if (window_count(&app->windows) > 1) {
        app_delete_window(ctx, app->windows.selected);
        return;
    }
    Buffer *other = app_other_buffer(app, ctx->view->buffer);
    if (other) app_switch_buffer(app, ctx->view, other);
    ctx->cursor = &ctx->view->cursors[0];
}

#if TEAL_DEV
// The state of every search (a test or a bench that waited too long).
void app_dev_results_log(App *app, const char *why) {
    for (i32 i = 0; i < RESULTS_MAX; i++) {
        Results *r = &app->results[i];
        if (!r->buffer || !r->running) continue;
        LOG("results: %s: list %d%s, fed %D, follows %s, emit_next %d (state %d), files seen %D, found %D, held %U bytes, mem %d, grep %s "
            "(claimed %d, limit %d, published %d, done %d, ended %d of %d workers), wants a frame %d", why, r->list_count,
            r->list_done ? " (done)" : "", r->fed, r->follow ? (r->follow->scan ? "a running scan" : "an index") : "nothing", r->emit_next,
            r->emit_next < r->list_count ? (i32)r->state[r->emit_next].state : -1, r->files_seen, r->found, r->held_live, r->mem_active,
            r->grep ? "yes" : "no", r->grep ? os_atomic_get(&r->grep->claimed) : 0, r->grep ? os_atomic_get(&r->grep->claim_limit) : 0,
            r->grep ? os_atomic_get(&r->grep->published) : 0, r->grep ? os_atomic_get(&r->grep->list_done) : 0,
            r->grep ? r->grep->workers_ended : 0, r->grep ? r->grep->worker_count : 0, app_wants_frame(app));
    }
    for (i32 i = 0; i < app->grep_dying_count; i++) {
        GrepSearch *s = app->grep_dying[i];
        LOG("results: %s: a search ending: claimed %d, published %d, done %d, cancel %d, ended %d of %d workers", why,
            os_atomic_get(&s->claimed), os_atomic_get(&s->published), os_atomic_get(&s->list_done), os_atomic_get(&s->cancel),
            s->workers_ended, s->worker_count);
    }
}

// Tests: frames, and the workers' records as they come, until no search runs and none is ending (or the
// timeout); false on the timeout.
static b32 results_dev_wait(App *app, Arena *scratch, u32 timeout_ms) {
    u64 start = os_time_us();
    for (;;) {
        b32 busy = app->grep_dying_count > 0;
        for (i32 i = 0; i < RESULTS_MAX; i++) busy |= app->results[i].buffer && app->results[i].running;
        if (!busy) return 1;
        if (os_time_us() - start > (u64)timeout_ms * 1000) {
            app_dev_results_log(app, "still running");
            return 0;
        }
        if (os_dev_wake_wait(app_wants_frame(app) ? 0 : 5)) app_wake(app);
        app_dev_feed_events(app, NULL, 0, scratch);
    }
}

u64 app_dev_results_work_max(App *app) {
    u64 m = app->dev_results_max_us;
    app->dev_results_max_us = 0;
    return m;
}

b32 app_dev_results_busy(App *app) {
    b32 busy = app->grep_dying_count > 0;
    for (i32 i = 0; i < RESULTS_MAX; i++) busy |= app->results[i].buffer && app->results[i].running;
    return busy;
}

// Smoke stage 8: a known tree (one matching file, two that do not match), the real search for "foo" in
// *search*, shown in another window without selecting it (the title stays *scratch*).
void app_dev_smoke_results(App *app, String8 tmp_dir) {
    String8 root = str8_fmt(&app->files_arena, "%S\\smoke_grep", tmp_dir);
    os_make_dir(root);
    os_write_file(str8_fmt(&app->files_arena, "%S\\match.c", root), STR8_LIT("int foo = 1;\n"));
    os_write_file(str8_fmt(&app->files_arena, "%S\\other1.txt", root), STR8_LIT("nothing here\n"));
    os_write_file(str8_fmt(&app->files_arena, "%S\\other2.txt", root), STR8_LIT("bar\n"));
    String8 full = os_full_path(&app->files_arena, root);
    u8 buf[PROJECT_PATH_MAX];
    i64 n = MIN(full.len, (i64)sizeof(buf));
    memcpy(buf, full.data, (size_t)n);
    CommandContext ctx = app->ctx;
    ctx.view = app_selected_view(app);
    ctx.cursor = &ctx.view->cursors[0];
    results_project_search(&ctx, str8(buf, n), STR8_LIT("foo"), 1, 0);
}

void app_dev_smoke_results_end(App *app) {
    app_run_command(app, &CMD_DELETE_OTHER_WINDOWS, 0, 0);
    Buffer *b = buffer_list_find_name(&app->buffers, STR8_LIT("*search*"));
    if (b) files_kill_buffer(app, b);
}

// --bench-grep: a search for `needle` under `root` with these settings, *search* in the other window (not
// selected); its state; and gone again.
void app_dev_bench_grep_start(App *app, String8 root, String8 needle, i32 workers, i32 max_results) {
    app->config->settings.search_workers = workers;
    app->config->settings.search_max_results = max_results;
    CommandContext ctx = app->ctx;
    ctx.view = app_selected_view(app);
    ctx.cursor = &ctx.view->cursors[0];
    ctx.scratch = &app->files_arena;
    results_project_search(&ctx, root, needle, !search_has_upper(needle), 0);
}

b32 app_dev_bench_grep_state(App *app, i64 *found, i64 *bytes, i64 *files, u64 *first_us, u64 *total_us, b32 *limited) {
    Results *r = results_of(app, buffer_list_find_name(&app->buffers, STR8_LIT("*search*")));
    if (!r) return 0;
    *found = r->found;
    *bytes = r->bytes;
    *files = r->files_seen;
    *first_us = r->first_us ? r->first_us - r->start_us : 0;
    *total_us = (r->running ? os_time_us() : r->end_us) - r->start_us;
    *limited = r->limited;
    return r->running || app->grep_dying_count > 0;
}

void app_dev_bench_grep_close(App *app) {
    app_run_command(app, &CMD_DELETE_OTHER_WINDOWS, 0, 0);
    Buffer *b = buffer_list_find_name(&app->buffers, STR8_LIT("*search*"));
    if (b) files_kill_buffer(app, b);
}

// Releases every held search (--grep-hold, tests).
static void results_dev_release(App *app) {
    app->dev_grep_hold_after = 0;
    for (i32 i = 0; i < RESULTS_MAX; i++) if (app->results[i].grep) grep_dev_release(app->results[i].grep);
    for (i32 i = 0; i < app->grep_dying_count; i++) grep_dev_release(app->grep_dying[i]);
}
#endif

const Command CMD_PROJECT_SEARCH = { "project-search", cmd_project_search, COMMAND_ONCE };
const Command CMD_NEXT_ERROR = { "next-error", cmd_next_error, COMMAND_ONCE };
const Command CMD_PREVIOUS_ERROR = { "previous-error", cmd_previous_error, COMMAND_ONCE };
const Command CMD_COMPILE_GOTO_ERROR = { "compile-goto-error", cmd_compile_goto_error, COMMAND_ONCE };
const Command CMD_NEXT_ERROR_NO_SELECT = { "next-error-no-select", cmd_next_error_no_select, COMMAND_ONCE };
const Command CMD_PREVIOUS_ERROR_NO_SELECT = { "previous-error-no-select", cmd_previous_error_no_select, COMMAND_ONCE };
const Command CMD_RECOMPILE = { "recompile", cmd_recompile, COMMAND_ONCE };
const Command CMD_QUIT_WINDOW = { "quit-window", cmd_quit_window, COMMAND_ONCE };
const Command CMD_OCCUR = { "occur", cmd_occur, COMMAND_ONCE };
