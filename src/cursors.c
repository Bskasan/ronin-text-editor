// cursors.c — see cursors.h.

// The cursor-adding commands keep the minibuffer at one cursor.
static b32 cursors_refused(CommandContext *ctx) {
    if (ctx->mini && ctx->mini->active && ctx->view == ctx->mini->view) {
        echo_message(ctx->echo, "Multiple cursors are not available in the minibuffer");
        return 1;
    }
    return 0;
}

// A cursor's span: its active region, or [point, point].
static void cursors_span(View *v, Cursor *c, const Settings *s, i64 *start, i64 *end) {
    *start = *end = view_point(v, c);
    if (view_region_active(v, c, s)) view_region(v, c, start, end);
}

// The newest cursor (the largest serial).
static i32 cursors_newest(View *v) {
    i32 k = 0;
    for (i32 i = 1; i < v->cursor_count; i++) if (v->cursors[i].serial > v->cursors[k].serial) k = i;
    return k;
}

// A cursor on [start, end): an active region with point at the end or the start, or a bare point.
static void cursors_place(View *v, Cursor *c, i64 start, i64 end, b32 point_at_end) {
    if (end > start) {
        view_set_point(v, c, point_at_end ? end : start);
        view_set_mark(v, c, point_at_end ? start : end, 1);
    } else {
        view_set_point(v, c, start);
        c->mark_active = c->mark_shift = 0;
    }
}

// After a job changed the cursors outside the driver.
static void cursors_settle(View *v, const Settings *s, Arena *scratch) {
    view_normalize_cursors(v, s, scratch);
    view_ensure_visible(v);
}

static void cursors_all_finish(CursorJob *job, const Settings *s, Arena *scratch) {
    View *v = job->view;
    i64 *pos = (i64 *)job->positions.base;
    for (i64 i = 0; i < job->count; i++) cursors_place(v, view_add_cursor(v, pos[i]), pos[i], pos[i] + job->needle_len, job->point_at_end);
    cursors_settle(v, s, scratch);
    if (job->capped) echo_message(job->echo, "Cursor limit reached (max_cursors = %d): %D occurrences marked", s->max_cursors, job->count + 1);
    else echo_message(job->echo, "Marked %D occurrences", job->count + 1);
    arena_reset(&job->positions);
    job->kind = CURSOR_JOB_OFF;
}

b32 cursor_job_pending(CursorJob *job) {
    return job->kind != CURSOR_JOB_OFF;
}

i64 cursor_job_work(CursorJob *job, const Settings *s, Arena *scratch, i64 budget) {
    i64 used = 0;
    while (job->kind != CURSOR_JOB_OFF && used < budget) {
        View *v = job->view;
        Buffer *buf = v->buffer;
        i64 before = job->search.examined;
        SearchStatus st = search_run(&job->search, buf, budget - used);
        used += MAX(job->search.examined - before, 1);
        if (st == SEARCH_RUNNING) continue;
        if (job->kind == CURSOR_JOB_ALL) {
            if (st != SEARCH_FOUND) {
                cursors_all_finish(job, s, scratch);
                break;
            }
            if (job->search.match_start != job->own_start) {
                *PUSH_STRUCT(&job->positions, i64) = job->search.match_start;
                job->count++;
                if (job->count + 1 >= s->max_cursors) {
                    job->capped = 1;
                    cursors_all_finish(job, s, scratch);
                    break;
                }
            }
            search_restart(&job->search, buf, 1, job->search.match_end, 0, buffer_size(buf)); // non-overlapping
            used += 16; // a match costs a little more than a position
            continue;
        }
        if (st == SEARCH_FOUND) {
            i64 ms = job->search.match_start, me = job->search.match_end;
            if (job->kind == CURSOR_JOB_SKIP) {
                i32 k = cursors_newest(v);
                cursors_place(v, &v->cursors[k], ms, me, job->point_at_end);
                v->primary = k;
            } else if (view_can_add_cursor(v, s, job->echo)) {
                Cursor *c = view_add_cursor(v, ms);
                cursors_place(v, c, ms, me, job->point_at_end);
                v->primary = (i32)(c - v->cursors);
            }
            cursors_settle(v, s, scratch);
        } else {
            echo_message(job->echo, "No more matches");
        }
        job->kind = CURSOR_JOB_OFF;
    }
    if (job->kind != CURSOR_JOB_OFF) {
        u8 text[64];
        i64 n = fmt_buf(text, sizeof(text), "Searching... %d%%", search_progress(&job->search));
        echo_set(job->echo, str8(text, n));
    }
    return used;
}

void cursor_job_stop(CursorJob *job) {
    if (job->kind == CURSOR_JOB_OFF) return;
    if (job->positions.base) arena_reset(&job->positions);
    job->kind = CURSOR_JOB_OFF;
    echo_message(job->echo, "Quit");
}

void cursor_job_destroy(CursorJob *job) {
    if (job->positions.base) os_release(job->positions.base);
    job->positions = (Arena){ 0 };
    job->kind = CURSOR_JOB_OFF;
}

// Starts a search for the primary's region text. False (with a message) when the region is too long.
static b32 cursors_job_begin(CommandContext *ctx, CursorJob *job, CursorJobKind kind, i64 rs, i64 re, b32 forward, i64 from) {
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    if (re - rs > SEARCH_NEEDLE_MAX) {
        echo_message(ctx->echo, "The region is too long (more than %d bytes)", SEARCH_NEEDLE_MAX);
        return 0;
    }
    u8 needle[SEARCH_NEEDLE_MAX];
    buffer_copy(buf, rs, re, needle);
    Cursor *p = view_primary(v);
    job->kind = kind;
    job->view = v;
    job->echo = ctx->echo;
    job->point_at_end = view_point(v, p) >= buffer_marker_get(buf, p->mark);
    job->own_start = rs;
    job->needle_len = re - rs;
    job->count = 0;
    job->capped = 0;
    search_begin(&job->search, buf, str8(needle, re - rs), 0, forward, from, 0, buffer_size(buf));
    return 1;
}

// One slice now; the rest in the app's frames (cursor_job_work), or, without an app (the headless view tests), to
// the end now in a job of the command's own.
static void cursors_job_run(CommandContext *ctx, CursorJob *job) {
    cursor_job_work(job, ctx->settings, ctx->scratch, SEARCH_SLICE_BYTES);
    if (ctx->cursor_job) return;
    while (job->kind != CURSOR_JOB_OFF) cursor_job_work(job, ctx->settings, ctx->scratch, SEARCH_SLICE_BYTES);
    cursor_job_destroy(job);
}

// The job to use: the app's, or one of the command's own (zeroed by the caller).
static CursorJob *cursors_job(CommandContext *ctx, CursorJob *local) {
    return ctx->cursor_job ? ctx->cursor_job : local;
}

// mark-next-like-this (forward) / mark-previous-like-this.
static void cursors_mark_like_this(CommandContext *ctx, b32 forward) {
    if (cursors_refused(ctx)) return;
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    if (!view_can_add_cursor(v, ctx->settings, ctx->echo)) return;
    Cursor *edge = forward ? &v->cursors[v->cursor_count - 1] : &v->cursors[0];
    i64 rs, re, es, ee;
    cursors_span(v, view_primary(v), ctx->settings, &rs, &re);
    cursors_span(v, edge, ctx->settings, &es, &ee);
    if (re > rs) { // the next occurrence of the region's text, past the last cursor (before the first)
        CursorJob local = { 0 }, *job = cursors_job(ctx, &local);
        if (cursors_job_begin(ctx, job, CURSOR_JOB_NEXT, rs, re, forward, forward ? ee : es)) cursors_job_run(ctx, job);
        return;
    }
    // The next line after the last cursor (before the first), at its column.
    i64 p = view_point(v, edge), line = buffer_line_of(buf, p) + (forward ? 1 : -1);
    if (line < 0 || line >= buffer_line_count(buf)) {
        echo_message(ctx->echo, "No more matches");
        return;
    }
    i64 col = edge->goal_col >= 0 ? edge->goal_col : view_column_of(buf, p);
    Cursor *c = view_add_cursor(v, view_offset_at_column(buf, line, col));
    c->goal_col = col;
    v->primary = (i32)(c - v->cursors);
}

static void cmd_mark_next_like_this(CommandContext *ctx) { cursors_mark_like_this(ctx, 1); }
static void cmd_mark_previous_like_this(CommandContext *ctx) { cursors_mark_like_this(ctx, 0); }

// Every occurrence of the primary's region text, from the start of the buffer (the others go first).
static void cmd_mark_all_like_this(CommandContext *ctx) {
    if (cursors_refused(ctx)) return;
    View *v = ctx->view;
    i64 rs, re;
    cursors_span(v, view_primary(v), ctx->settings, &rs, &re);
    if (re <= rs) {
        echo_message(ctx->echo, "No region");
        return;
    }
    view_collapse_cursors(v);
    CursorJob local = { 0 }, *job = cursors_job(ctx, &local);
    if (!job->positions.base) job->positions = arena_create(CURSORS_ALL_RESERVE);
    arena_reset(&job->positions);
    if (cursors_job_begin(ctx, job, CURSOR_JOB_ALL, rs, re, 1, 0)) cursors_job_run(ctx, job);
    else if (!ctx->cursor_job) cursor_job_destroy(job);
}

// The newest cursor moves to the next occurrence after it (or to the next line), and is the primary.
static void cmd_skip_to_next_like_this(CommandContext *ctx) {
    if (cursors_refused(ctx)) return;
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    i32 k = cursors_newest(v);
    Cursor *n = &v->cursors[k];
    i64 rs, re, ns, ne;
    cursors_span(v, view_primary(v), ctx->settings, &rs, &re);
    cursors_span(v, n, ctx->settings, &ns, &ne);
    if (re > rs) {
        CursorJob local = { 0 }, *job = cursors_job(ctx, &local);
        if (cursors_job_begin(ctx, job, CURSOR_JOB_SKIP, rs, re, 1, ne)) cursors_job_run(ctx, job);
        return;
    }
    i64 p = view_point(v, n), line = buffer_line_of(buf, p) + 1;
    if (line >= buffer_line_count(buf)) {
        echo_message(ctx->echo, "No more matches");
        return;
    }
    i64 col = n->goal_col >= 0 ? n->goal_col : view_column_of(buf, p);
    view_set_point(v, n, view_offset_at_column(buf, line, col));
    n->goal_col = col;
    v->primary = k;
}

// The newest cursor goes; the newest one left is the primary.
static void cmd_unmark_next_like_this(CommandContext *ctx) {
    if (cursors_refused(ctx)) return;
    View *v = ctx->view;
    if (v->cursor_count <= 1) {
        echo_message(ctx->echo, "Only one cursor");
        return;
    }
    i32 k = cursors_newest(v);
    buffer_marker_destroy(v->buffer, v->cursors[k].point);
    buffer_marker_destroy(v->buffer, v->cursors[k].mark);
    memmove(&v->cursors[k], &v->cursors[k + 1], sizeof(Cursor) * (u64)(v->cursor_count - k - 1));
    v->cursor_count--;
    arena_pop_to(&v->cursor_arena, sizeof(Cursor) * (u64)v->cursor_count);
    v->primary = cursors_newest(v);
}

// One cursor on each line of the region, from its first line to its last: at point's column (mode 0, or the end
// of a shorter line), at the line's end (1) or its start (2). The cursor on point's line is the primary.
static void cursors_edit_lines(CommandContext *ctx, i32 mode) {
    if (cursors_refused(ctx)) return;
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    Cursor *p = view_primary(v);
    i64 rs, re;
    if (!view_region_active(v, p, ctx->settings) || !view_region(v, p, &rs, &re)) {
        echo_message(ctx->echo, "No region");
        return;
    }
    i64 point = view_point(v, p), point_line = buffer_line_of(buf, point), col = view_column_of(buf, point);
    i64 first = buffer_line_of(buf, rs), last = buffer_line_of(buf, re);
    view_collapse_cursors(v);
    view_deactivate_mark(v);
    i32 max = MAX(ctx->settings->max_cursors, 1);
    if (last - first + 1 > max) {
        last = first + max - 1;
        echo_message(ctx->echo, "Cursor limit reached (max_cursors = %d)", max);
    }
    for (i64 line = first; line <= last; line++) {
        i64 pos = mode == 0 ? view_offset_at_column(buf, line, col) : mode == 1 ? buffer_line_end(buf, line) : buffer_line_start(buf, line);
        Cursor *c = line == first ? &v->cursors[0] : view_add_cursor(v, pos);
        if (line == first) view_set_point(v, c, pos);
        c->goal_col = mode == 0 ? col : -1;
        if (line == point_line) v->primary = (i32)(c - v->cursors);
    }
}

static void cmd_edit_lines(CommandContext *ctx) { cursors_edit_lines(ctx, 0); }
static void cmd_edit_ends_of_lines(CommandContext *ctx) { cursors_edit_lines(ctx, 1); }
static void cmd_edit_beginnings_of_lines(CommandContext *ctx) { cursors_edit_lines(ctx, 2); }

const Command CMD_MARK_NEXT_LIKE_THIS      = { "mark-next-like-this", cmd_mark_next_like_this, COMMAND_ONCE };
const Command CMD_MARK_PREVIOUS_LIKE_THIS  = { "mark-previous-like-this", cmd_mark_previous_like_this, COMMAND_ONCE };
const Command CMD_MARK_ALL_LIKE_THIS       = { "mark-all-like-this", cmd_mark_all_like_this, COMMAND_ONCE };
const Command CMD_SKIP_TO_NEXT_LIKE_THIS   = { "skip-to-next-like-this", cmd_skip_to_next_like_this, COMMAND_ONCE };
const Command CMD_UNMARK_NEXT_LIKE_THIS    = { "unmark-next-like-this", cmd_unmark_next_like_this, COMMAND_ONCE };
const Command CMD_EDIT_LINES               = { "edit-lines", cmd_edit_lines, COMMAND_ONCE };
const Command CMD_EDIT_ENDS_OF_LINES       = { "edit-ends-of-lines", cmd_edit_ends_of_lines, COMMAND_ONCE };
const Command CMD_EDIT_BEGINNINGS_OF_LINES = { "edit-beginnings-of-lines", cmd_edit_beginnings_of_lines, COMMAND_ONCE };
