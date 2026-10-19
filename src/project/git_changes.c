/* ======================================================================
   ronin: the git changes view (C-x g): status and diffs in a modal box

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// see git_changes.h. show-git-changes, the modal layer's second owner: the files git status lists
// on the left, the selected file's diff on the right, each diff fetched once per open and copied into the modal's
// arena inside the frame budget; RET visits the file at its first change; g runs git status again.
//
// The box opens only once rev-parse has answered, so a folder git does not know (or no git at all) opens nothing.
// Closing it cancels its jobs: they go to the dying list and are freed when their last batch comes; the main thread
// never waits for git.

#define GIT_RUNNING "Running git\xe2\x80\xa6"

extern const Command CMD_GIT_CHANGES_VISIT; // RET's internal command (outside the table)

typedef enum GitRowState {
    GIT_ROW_NONE,      // not fetched
    GIT_ROW_FETCHING,  // its diff runs (or waits for the running one)
    GIT_ROW_COPYING,   // its diff came: being copied into the modal's arena
    GIT_ROW_CACHED,
    GIT_ROW_FAILED,    // git said no (`error`), its output in the lines
} GitRowState;

typedef struct GitRow {
    u8 xy[2];
    u32 state;         // GitRowState
    String8 path, orig;
    String8 text;      // the diff's lines (cut, without CR), copied
    GitLine *lines;    // into `text`
    i64 count, total;  // copied so far, to copy
    i64 first_changed; // the new file's 1-based line of the first change; 0: none
    String8 error;     // FAILED: "git: ..." or "git timed out"
} GitRow;

struct GitView {
    String8 toplevel, program; // the work tree's top level (backslashes) and the git that answered: kept by g
    u64 mark;                  // the arena's position after them: g pops back to it
    // From here on, made again by g.
    String8 branch;
    b32 listed, cut, no_head;
    String8 message;           // the status line instead of the summary (a failed status)
    GitRow *rows;
    i64 count;
    GitJob *status_job;        // the open's job until its status came, or g's
    GitJob *diff_job;          // the selected file's diff while it runs (one at a time)
    i64 diff_row;
    GitJob *copy_job;          // the diff being copied (its batch lives in the job)
    GitBatch *copy_batch;
    i64 copy_row, copy_text;   // the row, the bytes copied so far
};

static Keymap *git_changes_keymap(App *app, void *data) {
    (void)data;
    return &app->config->git_changes;
}

static GitRow *git_changes_selected(App *app, GitView *v) {
    i64 sel = modal_list(app, 0)->selected;
    return v && sel >= 0 && sel < v->count ? &v->rows[sel] : NULL;
}

// A request with the settings of now (and, in dev builds, the tests' git).
static GitRequest git_changes_request(App *app, GitJobKind kind, String8 program, String8 dir) {
    Settings *s = &app->config->settings;
    GitRequest q = { .kind = kind, .program = program, .dir = dir, .max_lines = s->git_diff_max_lines, .max_files = s->git_status_max_files,
                     .timeout_ms = (u32)s->git_timeout_ms };
#if RONIN_DEV
    if (app->git.dev_program.len) q.program = app->git.dev_program;
    q.prefix = app->git.dev_prefix;
    q.env = app->git.dev_env;
#endif
    return q;
}

static GitJob *git_changes_start(GitRequest *q) {
    GitJob *j = git_job_create(q);
    if (j && !git_job_start(j)) {
        git_job_free(j);
        j = NULL;
    }
    return j;
}

// A job the app no longer wants: freed when it has ended, else cancelled and left to end on the dying list.
static void git_changes_bury(App *app, GitJob *j) {
    GitChanges *g = &app->git;
    if (!j) return;
    if (j->ended || !j->started) {
        git_job_free(j);
        return;
    }
    git_job_cancel(j);
    ASSERT(g->dying_count < GIT_DYING_MAX); // C-x g and g leave room for every job they may bury
    g->dying[g->dying_count++] = j;
}

// The diff of the selected file, if it is not fetched yet: started now, or after the running diff or copy.
static void git_changes_fetch(App *app, GitView *v) {
    GitRow *row = git_changes_selected(app, v);
    if (!row || row->state != GIT_ROW_NONE || v->diff_job || v->copy_job || v->status_job) return;
    GitDiffMode mode = v->no_head || (row->xy[0] == '?' && row->xy[1] == '?') ? GIT_DIFF_NEW
                     : row->orig.len                                        ? GIT_DIFF_RENAME
                                                                            : GIT_DIFF_HEAD;
    GitRequest q = git_changes_request(app, GIT_JOB_DIFF, v->program, v->toplevel);
    q.path = row->path;
    q.orig = row->orig;
    q.mode = mode;
    GitJob *j = git_changes_start(&q);
    if (!j) {
        row->state = GIT_ROW_FAILED;
        row->error = STR8_LIT("git: cannot start a worker");
        return;
    }
    row->state = GIT_ROW_FETCHING;
    v->diff_job = j;
    v->diff_row = row - v->rows;
#if RONIN_DEV
    app->git.dev_diff_jobs++;
#endif
}

// The right pane follows the selected file: its rows so far, from the top; its diff fetched if it is not.
static void git_changes_show(App *app, GitView *v) {
    GitRow *row = git_changes_selected(app, v);
    modal_set_rows(app, 1, row ? row->count : 0);
    git_changes_fetch(app, v);
}

static void git_changes_selected_moved(App *app, void *data, i32 pane, i64 row) {
    (void)row;
    if (pane == 0) git_changes_show(app, data);
}

// What a job that did not give its answer says ("git: <git's line>", "git timed out").
static String8 git_changes_failure(Arena *a, GitBatch *b) {
    if (b->kind == GIT_BATCH_TIMEOUT) return STR8_LIT("git timed out");
    if (b->error.len) return str8_fmt(a, "git: %S", b->error);
    return str8_fmt(a, "git: exited with code %u", b->exit_code);
}

// The status: the file list (and the cut row after it), the first file selected.
static void git_changes_listed(App *app, GitView *v, GitStatus *st) {
    Arena *a = modal_arena(app);
    v->rows = PUSH_ARRAY(a, GitRow, st->count + 1);
    for (i64 i = 0; i < st->count; i++) {
        GitRow *row = &v->rows[i];
        row->xy[0] = st->entries[i].xy[0];
        row->xy[1] = st->entries[i].xy[1];
        row->path = str8_copy(a, st->entries[i].path);
        row->orig = str8_copy(a, st->entries[i].orig);
    }
    v->count = st->count;
    v->cut = st->cut;
    v->no_head = st->no_head;
    v->branch = str8_copy(a, st->branch);
    v->listed = 1;
    modal_set_rows(app, 0, v->count + (v->cut ? 1 : 0));
    git_changes_show(app, v);
}

// A diff came for `row`: its lines are copied by git_changes_work (inside the frame budget).
static void git_changes_diff_came(App *app, GitView *v, GitJob *j, GitBatch *b) {
    GitRow *row = &v->rows[v->diff_row];
    Arena *a = modal_arena(app);
    GitDiff *d = &b->diff;
    i64 bytes = 0;
    for (i64 i = 0; i < d->count; i++) bytes += d->lines[i].len;
    row->text = str8(PUSH_ARRAY(a, u8, bytes + 1), 0);
    row->lines = PUSH_ARRAY(a, GitLine, d->count + 1);
    row->total = d->count;
    row->first_changed = d->first_changed;
    row->state = GIT_ROW_COPYING;
    v->copy_job = j;
    v->copy_batch = b;
    v->copy_row = v->diff_row;
    v->copy_text = 0;
    app->git.work_pending = 1;
}

// A diff that failed: its message, and git's output in the pane (in the text color).
static void git_changes_diff_failed(App *app, GitView *v, GitRow *row, GitBatch *b) {
    Arena *a = modal_arena(app);
    row->state = GIT_ROW_FAILED;
    row->error = git_changes_failure(a, b);
    GitDiff d;
    git_parse_diff(a, str8_copy(a, b->text), app->config->settings.git_diff_max_lines, &d);
    row->text = d.text;
    row->lines = d.lines;
    row->count = row->total = d.count;
    if (row == git_changes_selected(app, v)) modal_set_rows(app, 1, row->count);
}

// One batch of a job of the open box.
static void git_changes_batch(App *app, GitView *v, GitJob *j, GitBatch *b) {
    Arena *a = modal_arena(app);
    if (j == v->status_job) {
        if (b->kind == GIT_BATCH_STATUS) git_changes_listed(app, v, &b->status);
        else if (b->kind != GIT_BATCH_CANCELLED && b->kind != GIT_BATCH_TOPLEVEL) v->message = git_changes_failure(a, b);
        if (b->last) {
            git_job_ended(j);
            git_job_free(j);
            v->status_job = NULL;
            git_changes_fetch(app, v);
        }
        return;
    }
    if (j == v->diff_job) {
        GitRow *row = &v->rows[v->diff_row];
        b32 copying = 0;
        if (b->kind == GIT_BATCH_DIFF) {
            git_changes_diff_came(app, v, j, b);
            copying = 1;
        } else if (b->kind == GIT_BATCH_CANCELLED) {
            row->state = GIT_ROW_NONE;
        } else {
            git_changes_diff_failed(app, v, row, b);
        }
        if (b->last) {
            git_job_ended(j);
            if (!copying) git_job_free(j); // a copy keeps it: the batch lives there
            v->diff_job = NULL;
            git_changes_fetch(app, v);
        }
    }
}

static void git_changes_closed(App *app, void *data) {
    GitView *v = data;
    git_changes_bury(app, v->status_job);
    git_changes_bury(app, v->diff_job);
    git_changes_bury(app, v->copy_job);
    v->status_job = v->diff_job = v->copy_job = NULL;
    app->git.view = NULL;
    app->git.work_pending = 0;
}

static void git_changes_accept(App *app, void *data, i32 pane, i64 row_index) {
    GitView *v = data;
    (void)pane;
    (void)row_index; // either pane: the selected file
    GitRow *row = git_changes_selected(app, v);
    if (!row) return;
    if (row->xy[1] == 'D' || (row->xy[0] == 'D' && row->xy[1] == ' ')) {
        echo_message(&app->echo, "%S is deleted in the working tree", row->path);
        return;
    }
    GitChanges *g = &app->git;
    if (!g->arena.base) g->arena = arena_create(MB(1));
    arena_reset(&g->arena);
    String8 path = str8_fmt(&g->arena, "%S\\%S", v->toplevel, row->path);
    for (i64 i = 0; i < path.len; i++) if (path.data[i] == '/') path.data[i] = '\\';
    g->visit_path = path;
    g->visit_line = row->state == GIT_ROW_COPYING || row->state == GIT_ROW_CACHED ? row->first_changed : 0;
    g->pending_dir = str8(NULL, 0);
    modal_run_after_close(app, &CMD_GIT_CHANGES_VISIT);
}

static String8 git_changes_status(App *app, void *data, Arena *scratch) {
    GitView *v = data;
    if (v->message.len) return v->message;
    GitRow *row = git_changes_selected(app, v);
    if (row && row->state == GIT_ROW_FAILED) return row->error;
    if (!v->listed || v->status_job || v->diff_job || (row && row->state == GIT_ROW_FETCHING)) return STR8_LIT(GIT_RUNNING);
    String8 head = v->branch.len ? str8_fmt(scratch, "%S: ", v->branch) : str8(NULL, 0);
    if (!v->count) return str8_fmt(scratch, "%SNo changes", head);
    return str8_fmt(scratch, "%S%D%s changed file%s", head, v->count, v->cut ? "+" : "", v->count == 1 && !v->cut ? "" : "s");
}

static u32 git_changes_kind_color(Theme *th, u32 kind) {
    switch (kind) {
    case GIT_LINE_ADDED:   return th->diff_added;
    case GIT_LINE_REMOVED: return th->diff_removed;
    case GIT_LINE_HUNK:    return th->diff_hunk;
    case GIT_LINE_HEADER:  return th->diff_header;
    case GIT_LINE_CUT:     return th->modal_description;
    }
    return th->text;
}

// Left: the two-letter status, then the path ("old -> new" for a rename); right: one line of the selected diff.
static void git_changes_draw_row(App *app, void *data, Renderer *r, ModalRow *mr) {
    GitView *v = data;
    Theme *th = &app->config->theme;
    i32 cw = app->font->cell_w, y = (i32)mr->rect.y0;
    u8 note[64];
    if (mr->pane == 0) {
        if (mr->row == v->count && v->cut) {
            i64 n = fmt_buf(note, sizeof(note), "[list cut at %D files]", v->count);
            font_draw_text(app->font, r, mr->x, y, app_clip_cells(str8(note, n), mr->cols), COLOR_HEX(th->modal_description));
            return;
        }
        if (mr->row < 0 || mr->row >= v->count) return;
        GitRow *row = &v->rows[mr->row];
        font_draw_text(app->font, r, mr->x, y, app_clip_cells(str8(row->xy, 2), mr->cols), COLOR_HEX(th->git_status));
        i64 cells = mr->cols - 3;
        if (cells <= 0) return;
        i32 x = mr->x + 3 * cw;
        if (row->orig.len) {
            String8 from = app_clip_cells(row->orig, cells);
            x = font_draw_text(app->font, r, x, y, from, COLOR_HEX(th->text));
            cells -= app_text_cells(from);
            String8 arrow = app_clip_cells(STR8_LIT(" -> "), MAX(cells, 0));
            x = font_draw_text(app->font, r, x, y, arrow, COLOR_HEX(th->modal_description));
            cells -= app_text_cells(arrow);
        }
        if (cells > 0) font_draw_text(app->font, r, x, y, app_clip_cells(row->path, cells), COLOR_HEX(th->text));
        return;
    }
    GitRow *row = git_changes_selected(app, v);
    if (!row || mr->row < 0 || mr->row >= row->count) return;
    GitLine *l = &row->lines[mr->row];
    if (l->kind == GIT_LINE_CUT) {
        i64 n = fmt_buf(note, sizeof(note), "[diff cut at %D lines]", row->total - 1);
        font_draw_text(app->font, r, mr->x, y, app_clip_cells(str8(note, n), mr->cols), COLOR_HEX(th->modal_description));
        return;
    }
    u32 rgb = row->state == GIT_ROW_FAILED ? th->text : git_changes_kind_color(th, l->kind);
    app_draw_text_line(app, r, str8(row->text.data + l->off, l->len), mr->x, y, mr->cols, app->config->settings.tab_width, rgb);
}

// rev-parse answered: the box opens, the job's status goes to it. False when it could not open (modal_open said
// why: the minibuffer, a frame too small).
static b32 git_changes_open(App *app, GitJob *j, GitBatch *b) {
    ModalSpec spec = {
        .name = STR8_LIT("git changes view"), .keymap = git_changes_keymap, .status_line = 1, .panes = 2, .scroll_panes = 1u << 1,
        .width_pct = 90, .height_pct = 80, .pane_split_pct = 30, .full_height = 1, .accept = git_changes_accept,
        .selected = git_changes_selected_moved, .draw_row = git_changes_draw_row, .status = git_changes_status, .closed = git_changes_closed,
    };
    if (!modal_open(app, &spec)) return 0;
    Arena *a = modal_arena(app);
    GitView *v = PUSH_STRUCT(a, GitView);
    v->toplevel = str8_copy(a, b->text);
    v->program = str8_copy(a, b->program);
    v->mark = arena_pos(a);
    v->status_job = j;
    modal_set_data(app, v);
    app->git.view = v;
    echo_clear(&app->echo);
    return 1;
}

// What C-x g's job said before any box opened (always its last batch).
static void git_changes_refused(App *app, GitBatch *b) {
    GitChanges *g = &app->git;
    switch (b->kind) {
    case GIT_BATCH_NO_GIT:
        if (b->text.len) files_echo(&app->echo, "git not found: %S", b->text);
        else echo_message(&app->echo, "git not found on PATH (set git_path in [settings])");
        break;
    case GIT_BATCH_NOT_WORK_TREE:
        files_echo(&app->echo, "Not a git work tree: %S", g->pending_dir);
        break;
    case GIT_BATCH_FAILED:
    case GIT_BATCH_TIMEOUT:
        echo_message(&app->echo, "%S", git_changes_failure(&g->arena, b));
        break;
    default:
        break;
    }
}

static b32 git_changes_wake(App *app) {
    GitChanges *g = &app->git;
    b32 frame = 0;
    for (i32 i = 0; i < g->dying_count;) { // their batches are dropped; freed once the last came
        GitJob *j = g->dying[i];
        for (GitBatch *b = git_job_take(j); b; b = b->next) if (b->last) git_job_ended(j);
        if (j->ended) {
            git_job_free(j);
            g->dying[i] = g->dying[--g->dying_count];
        } else {
            i++;
        }
    }
    if (g->pending) {
        GitJob *j = g->pending;
        GitBatch *b = git_job_take(j);
        while (b) {
            GitBatch *next = b->next;
            if (b->last) git_job_ended(j);
            frame = 1;
            if (b->kind == GIT_BATCH_TOPLEVEL) {
                g->pending = NULL;
                if (!git_changes_open(app, j, b)) {
                    for (GitBatch *x = next; x; x = x->next) if (x->last) git_job_ended(j); // taken: the dying list never sees it
                    git_changes_bury(app, j);
                    return 1;
                }
                for (b = next; b; b = b->next) git_changes_batch(app, g->view, j, b); // its status may be here already
                return 1;
            }
            git_changes_refused(app, b);
            if (b->last) {
                g->pending = NULL;
                git_job_free(j);
                return 1;
            }
            b = next;
        }
    }
    GitView *v = g->view;
    if (v) {
        GitJob *jobs[2] = { v->status_job, v->diff_job };
        for (i32 k = 0; k < 2; k++) {
            if (!jobs[k]) continue;
            for (GitBatch *b = git_job_take(jobs[k]), *next; b; b = next) {
                next = b->next;
                git_changes_batch(app, v, jobs[k], b);
                frame = 1;
            }
        }
    }
    return frame;
}

// The frame's share of a diff's copy into the modal's arena (frames without input; `budget` lines per frame in
// tests). True while some is left.
static b32 git_changes_work(App *app, u64 deadline, i64 budget) {
    GitView *v = app->git.view;
    if (!v || !v->copy_job) return 0;
#if RONIN_DEV
    u64 t0 = os_time_us();
#endif
    GitRow *row = &v->rows[v->copy_row];
    GitDiff *d = &v->copy_batch->diff;
    i64 done = 0;
    while (row->count < row->total) {
        GitLine l = d->lines[row->count];
        memcpy(row->text.data + v->copy_text, d->text.data + l.off, l.len);
        row->lines[row->count++] = (GitLine){ (u32)v->copy_text, l.len, l.kind, 0 };
        v->copy_text += l.len;
        row->text.len = v->copy_text;
        done++;
        if (budget ? done >= budget : done % GIT_COPY_LINES_PER_CHECK == 0 && os_time_us() >= deadline) break;
    }
    if (row == git_changes_selected(app, v)) modal_grow_rows(app, 1, row->count);
#if RONIN_DEV
    app->git.dev_copy_max_us = MAX(app->git.dev_copy_max_us, os_time_us() - t0);
#endif
    if (row->count < row->total) return 1;
    row->state = GIT_ROW_CACHED;
    git_job_free(v->copy_job); // ended when its batch came
    v->copy_job = NULL;
    v->copy_batch = NULL;
    git_changes_fetch(app, v); // a selection that waited for this copy
    return 0;
}

static void git_changes_quit(App *app) {
    GitChanges *g = &app->git;
    if (!g->pending) return;
    git_changes_bury(app, g->pending);
    g->pending = NULL;
}

static void git_changes_shutdown(App *app, u64 deadline) {
    GitChanges *g = &app->git;
    GitView *v = g->view;
    GitJob *all[GIT_DYING_MAX + 4];
    i32 n = 0;
    if (g->pending) all[n++] = g->pending;
    if (v) {
        if (v->status_job) all[n++] = v->status_job;
        if (v->diff_job) all[n++] = v->diff_job;
        if (v->copy_job) all[n++] = v->copy_job;
        v->status_job = v->diff_job = v->copy_job = NULL;
    }
    for (i32 i = 0; i < g->dying_count; i++) all[n++] = g->dying[i];
    g->pending = NULL;
    g->dying_count = 0;
    for (i32 i = 0; i < n; i++) if (!all[i]->ended) git_job_cancel(all[i]); // all of them first, then the one bound
    for (i32 i = 0; i < n; i++) {
        if (git_job_join(all[i], deadline)) {
            git_job_free(all[i]);
        } else {
            LOG("git: a git worker did not end within %d ms of the exit: left to the process exit (abandoned)", PROJECT_SHUTDOWN_MS);
#if RONIN_DEV
            g->dev_abandoned++;
            os_dev_thread_abandoned();
#endif
        }
    }
    if (g->arena.base) os_release(g->arena.base);
    g->arena = (Arena){ 0 };
    g->work_pending = 0;
}

// ---------------------------------------------------------------------------
// The commands

static void cmd_show_git_changes(CommandContext *ctx) {
    App *app = ctx->app;
    if (!app) {
        echo_message(ctx->echo, "No git changes view here");
        return;
    }
    GitChanges *g = &app->git;
    if (app->mini.active) {
        echo_message(ctx->echo, "The git changes view is not available in the minibuffer");
        return;
    }
    if (g->pending) {
        echo_set(ctx->echo, STR8_LIT(GIT_RUNNING));
        return;
    }
#if RONIN_DEV
    // A batch run starts git only when a test chose it: the fake git, or the real one with its configuration shut out.
    if (!g->dev_program.len && !g->dev_allow_real && os_dev_batch_run()) {
        os_dev_real_refused_add(1);
        echo_message(ctx->echo, "git: no real git in a test run");
        return;
    }
#endif
    if (g->dying_count > GIT_DYING_MAX - 4) { // room for this job and the open's two
        echo_message(ctx->echo, "git is still stopping; try again");
        return;
    }
    Buffer *buf = ctx->view->buffer;
    if (!g->arena.base) g->arena = arena_create(MB(1));
    arena_reset(&g->arena);
    // Where rev-parse runs: the project's root, else the buffer's directory, else the default one.
    String8 dir = finder_root(app, buf);
    if (!dir.len && buf->path.len) dir = str8(buf->path.data, buf->path.len - config_file_name(buf->path).len);
    if (!dir.len) dir = app->finder.cmdline_dir.len ? app->finder.cmdline_dir : os_full_path(&g->arena, STR8_LIT("."));
    g->pending_dir = str8_copy(&g->arena, dir);
    g->visit_path = str8(NULL, 0);
    Settings *s = &app->config->settings;
    GitRequest q = git_changes_request(app, GIT_JOB_OPEN, str8(s->git_path, s->git_path_len), g->pending_dir);
    g->pending = git_changes_start(&q);
    if (!g->pending) {
        echo_message(ctx->echo, "git: cannot start a worker");
        return;
    }
    echo_set(ctx->echo, STR8_LIT(GIT_RUNNING));
}

static void cmd_git_changes_refresh(CommandContext *ctx) {
    App *app = ctx->app;
    GitView *v = app ? app->git.view : NULL;
    if (!v) {
        echo_message(ctx->echo, "The git changes view is not open");
        return;
    }
    GitChanges *g = &app->git;
    if (g->dying_count > GIT_DYING_MAX - 3) {
        echo_message(ctx->echo, "git is still stopping; try again");
        return;
    }
    git_changes_bury(app, v->status_job);
    git_changes_bury(app, v->diff_job);
    git_changes_bury(app, v->copy_job);
    arena_pop_to(modal_arena(app), v->mark); // the list and every diff fetched
    memset((u8 *)v + offsetof(GitView, branch), 0, sizeof(GitView) - offsetof(GitView, branch));
    g->work_pending = 0;
    modal_set_rows(app, 0, 0);
    modal_set_rows(app, 1, 0);
    Settings *s = &app->config->settings;
    String8 program = s->git_path_len ? str8(s->git_path, s->git_path_len) : v->program;
    GitRequest q = git_changes_request(app, GIT_JOB_STATUS, program, v->toplevel);
    v->status_job = git_changes_start(&q);
    if (!v->status_job) v->message = STR8_LIT("git: cannot start a worker");
}

// RET in the view: the file it chose, in the selected window, at its first change.
static void cmd_git_changes_visit(CommandContext *ctx) {
    App *app = ctx->app;
    if (!app || !app->git.visit_path.len) return;
    Buffer *buf = app_find_file(app, app->git.visit_path);
    i64 line = app->git.visit_line;
    app->git.visit_path = str8(NULL, 0);
    if (!buf) return;
    View *v = ctx->view;
    app_switch_buffer(app, v, buf);
    if (line > 0) view_goto_line_column(v, line - 1, 0);
    view_ensure_visible(v);
    ctx->cursor = view_primary(v);
    buffer_list_touch(&app->buffers, buf);
}

const Command CMD_SHOW_GIT_CHANGES = { "show-git-changes", "Show the uncommitted changes of the project", cmd_show_git_changes, COMMAND_ONCE };
const Command CMD_GIT_CHANGES_REFRESH = { "git-changes-refresh", "Run git status again in the git changes view", cmd_git_changes_refresh, COMMAND_ONCE };
const Command CMD_GIT_CHANGES_VISIT = { "git-changes-visit", "Visit the file chosen in the git changes view", cmd_git_changes_visit, COMMAND_ONCE };

#if RONIN_DEV
// Tests, the smoke, the leak check: a job of the view still running, dying or a copy left.
static b32 app_dev_git_busy(App *app) {
    GitChanges *g = &app->git;
    GitView *v = g->view;
    return g->pending || g->dying_count || g->work_pending || (v && (v->status_job || v->diff_job || v->copy_job));
}

// One git command on this thread (dev setup only), to its end; its exit code (0xFFFFFFFF: it did not start).
static u32 git_dev_run(Arena *arena, String8 git, String8 args, String8 dir, String8 env, String8 *out) {
    OsProcess p;
    *out = str8(NULL, 0);
    if (os_process_start_program(arena, git, args, dir, env, &p) != OS_FILE_OK) return 0xFFFFFFFFu;
    OsEvent cancel;
    os_event_create(&cancel);
    i64 cap = KB(256), n = 0;
    u8 *buf = PUSH_ARRAY(arena, u8, cap);
    for (;;) {
        i64 got = 0;
        if (os_process_read(&p, cancel, 30000, buf + n, cap - n, &got) != OS_PROCESS_DATA) break;
        n += got;
        if (n == cap) break;
    }
    os_process_kill(&p);
    os_process_wait(&p, 5000);
    u32 code = os_process_exit_code(&p);
    os_process_close(&p, 1);
    os_event_destroy(cancel);
    *out = str8(buf, n);
    return code;
}

// The real-git test's and smoke stage 12's repository, tmp\gitrepo\repo: made once (git init, a.c b.c c.c, one
// commit), then its working tree set again each time: a.c changed, c.c deleted, d.txt untracked. Every git of it runs
// with *env: the system and global configuration shut out (GIT_CONFIG_NOSYSTEM, GIT_CONFIG_GLOBAL to an empty file,
// HOME and XDG_CONFIG_HOME for a git older than 2.32). False when git.exe is not on PATH (or the setup failed).
static b32 git_dev_make_repo(Arena *arena, String8 tmp_dir, String8 *git, String8 *repo, String8 *env) {
    if (!os_find_program(arena, STR8_LIT("git.exe"), git)) return 0;
    String8 home = os_full_path(arena, str8_fmt(arena, "%S\\gitrepo-home", tmp_dir));
    String8 root = os_full_path(arena, str8_fmt(arena, "%S\\gitrepo", tmp_dir));
    *repo = str8_fmt(arena, "%S\\repo", root);
    os_make_dir(home);
    os_make_dir(root);
    os_make_dir(*repo);
    os_write_file(str8_fmt(arena, "%S\\.gitconfig", home), str8(NULL, 0));
    *env = str8_fmt(arena, "GIT_CONFIG_NOSYSTEM=1\nGIT_CONFIG_GLOBAL=%S\\.gitconfig\nHOME=%S\nXDG_CONFIG_HOME=%S", home, home, home);
    String8 out;
    u32 code = git_dev_run(arena, *git, STR8_LIT("--version"), *repo, *env, &out);
    LOG("git repo: %S (%S): exit %u", out.len && out.data[out.len - 1] == '\n' ? str8(out.data, out.len - 1) : out, *git, code);
    if (code != 0) return 0;
    OsFileInfo info;
    if (os_file_info(str8_fmt(arena, "%S\\.git", *repo), &info) != OS_FILE_OK || !info.exists) {
        static const char *const setup[] = { "-c init.defaultBranch=main init -q", "add -A",
                                             "-c user.name=ronin -c user.email=ronin@localhost commit -q -m initial" };
        os_write_file(str8_fmt(arena, "%S\\a.c", *repo), STR8_LIT("int a;\nint b;\nint c;\n"));
        os_write_file(str8_fmt(arena, "%S\\b.c", *repo), STR8_LIT("int x;\n"));
        os_write_file(str8_fmt(arena, "%S\\c.c", *repo), STR8_LIT("gone\n"));
        for (i32 i = 0; i < (i32)ARRAY_COUNT(setup); i++) {
            code = git_dev_run(arena, *git, str8_cstr(setup[i]), *repo, *env, &out);
            if (code != 0) {
                LOG("git repo: '%s' failed (exit %u): %S", setup[i], code, out);
                return 0;
            }
        }
    }
    os_write_file(str8_fmt(arena, "%S\\a.c", *repo), STR8_LIT("int a;\nint B;\nint c;\n"));
    os_write_file(str8_fmt(arena, "%S\\b.c", *repo), STR8_LIT("int x;\n"));
    os_file_delete(str8_fmt(arena, "%S\\c.c", *repo));
    os_write_file(str8_fmt(arena, "%S\\d.txt", *repo), STR8_LIT("new\n"));
    return 1;
}

// Smoke stage 12: the real git with that configuration, b.c of the repository in the window, point at its line's end, focus on.
void app_dev_smoke_git(App *app, String8 git, String8 repo, String8 env) {
    GitChanges *g = &app->git;
    g->dev_program = git;
    g->dev_prefix = str8(NULL, 0);
    g->dev_env = env;
    if (!g->arena.base) g->arena = arena_create(MB(1));
    Buffer *b = app_find_file(app, str8_fmt(&app->files_arena, "%S\\b.c", repo));
    View *v = app_selected_view(app);
    if (b) app_switch_buffer(app, v, b);
    view_set_point(v, view_primary(v), 6); // the end of "int x;"
    echo_clear(&app->echo);
    app->force_focus = 1;
}

// RET visited a.c at its first change (line 2).
b32 app_dev_smoke_git_visited(App *app) {
    View *v = app_selected_view(app);
    b32 ok = str8_equal(v->buffer->name, STR8_LIT("a.c")) && buffer_line_of(v->buffer, view_point(v, view_primary(v))) == 1 && !modal_active(app);
    LOG("smoke: %s: git changes: RET visited %S at line %D", ok ? "ok" : "FAIL", v->buffer->name,
        buffer_line_of(v->buffer, view_point(v, view_primary(v))) + 1);
    return ok;
}

void app_dev_smoke_git_end(App *app) {
    modal_close(app);
    app_switch_buffer(app, app_selected_view(app), buffer_list_find_name(&app->buffers, STR8_LIT("*scratch*")));
    static const char *const names[] = { "a.c", "b.c" };
    for (i32 i = 0; i < 2; i++) {
        Buffer *b = buffer_list_find_name(&app->buffers, str8_cstr(names[i]));
        if (b) files_kill_buffer(app, b);
    }
    app->git.dev_program = app->git.dev_env = str8(NULL, 0);
    app->force_focus = 0;
}
#endif
