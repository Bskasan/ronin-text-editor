// finder.c — see finder.h. Part of the app (included after files.c).

// ---------------------------------------------------------------------------
// Projects: kept indexes, their scans and watches

static void finder_init(App *app) {
    Finder *f = &app->finder;
    project_cache_init(&f->roots);
    fuzzy_rank_init(&f->rank);
    f->row_arena = arena_create(MB(16));
}

// The root of the project a buffer belongs to, or empty (not in a project).
static String8 finder_root(App *app, Buffer *buf) {
    Settings *s = &app->config->settings;
    String8 dir = { 0 };
    if (buf->path.len) {
        dir = str8(buf->path.data, buf->path.len - config_file_name(buf->path).len);
        while (dir.len > 3 && (dir.data[dir.len - 1] == '\\' || dir.data[dir.len - 1] == '/')) dir.len--;
    }
    return project_root_for(&app->finder.roots, dir, app->finder.cmdline_dir, str8(s->project_markers, s->project_markers_len),
                            project_exists_fs, NULL);
}

static b32 finder_is_open(App *app);

// A scan that is not needed any more: it is cancelled and thrown away as it ends (app_wake).
static void finder_drop_scan(App *app, ProjectScan *s) {
    Finder *f = &app->finder;
    project_scan_cancel(s);
    if (f->dying_count == PROJECT_DYING_MAX) { // never in practice: wait for the oldest
        ProjectScan *old = f->dying[0];
        if (project_scan_join(old, PROJECT_SHUTDOWN_MS)) project_scan_free(old);
        memmove(f->dying, f->dying + 1, sizeof(f->dying[0]) * (PROJECT_DYING_MAX - 1));
        f->dying_count--;
    }
    f->dying[f->dying_count++] = s;
}

static void finder_close_project(App *app, Project *p) {
    if (p->scan) finder_drop_scan(app, p->scan);
    if (p->watch) os_unwatch(p->watch);
    project_index_destroy(&p->index[0]);
    project_index_destroy(&p->index[1]);
    memset(p, 0, sizeof(*p));
}

// The kept project with this root, else a free slot or the least recently used one.
static Project *finder_project(App *app, String8 root) {
    Finder *f = &app->finder;
    Project *found = NULL, *lru = NULL;
    for (i32 i = 0; i < PROJECT_KEPT && !found; i++) {
        Project *p = &f->projects[i];
        if (p->root.len == root.len && project_path_inside(root, p->root)) found = p;
        else if (!lru || (lru->root.len && (!p->root.len || p->used < lru->used))) lru = p; // a free slot, else the oldest
    }
    if (!found) {
        found = lru;
        if (found->root.len) finder_close_project(app, found);
        memcpy(found->root_buf, root.data, (size_t)MIN(root.len, (i64)PROJECT_PATH_MAX));
        found->root = str8(found->root_buf, MIN(root.len, (i64)PROJECT_PATH_MAX));
    }
    found->used = ++f->tick;
    return found;
}

// .teal-project's diagnostics: every one to *Messages*, the first error in the echo area.
static void finder_report(App *app, ProjectFile *file) {
    for (ConfigDiag *d = file->diags.first; d; d = d->next) echo_log(&app->echo, d->text);
    for (ConfigDiag *d = file->diags.first; file->diags.errors && d; d = d->next) {
        if (d->warning) continue;
        if (file->diags.errors == 1) echo_message(&app->echo, "%S", d->text);
        else echo_message(&app->echo, "%S (and %d more)", d->text, file->diags.errors - 1);
        break;
    }
}

// Starts a scan of the project: into the live index the first time (the finder fills in as it comes),
// into the other one afterwards (the old list stays in use until the new one is complete).
static void finder_scan(App *app, Project *p) {
    Finder *f = &app->finder;
    Settings *s = &app->config->settings;
    arena_reset(&app->files_arena);
    ProjectFile file;
    OsFileStatus status = project_file_read(&app->files_arena, p->root, &file);
    if (status == OS_FILE_OK) finder_report(app, &file);
    else if (status != OS_FILE_NOT_FOUND) files_echo(&app->echo, "Cannot read %S\\%s: %s", p->root, PROJECT_FILE_NAME, buffer_status_text(status));
    if (p->scan) {
        finder_drop_scan(app, p->scan);
        p->scan = NULL;
    }
    p->scan_shadow = p->scanned;
    ProjectIndex *x = &p->index[p->scan_shadow ? !p->live : p->live];
    if (!x->files.base) project_index_init(x);
    else project_index_reset(x);
    // The watch is re-armed first: a change during the walk makes the new index stale again.
    if (!p->watch) p->watch = os_watch_tree(p->root);
    else os_watch_rearm(p->watch, p->root);
    p->stale = 0;
    ProjectScan *scan = project_scan_create(p->root, str8(s->project_ignore, s->project_ignore_len), status == OS_FILE_OK ? &file : NULL,
                                            s->project_max_files);
#if TEAL_DEV
    if (f->dev_hold_after) {
        scan->dev_hold = 1;
        scan->dev_hold_after = f->dev_hold_after;
    }
    if (f->dev_stuck_next) {
        scan->dev_stuck = 1;
        f->dev_stuck_next = 0;
    }
    f->dev_last_scan = scan;
#endif
    (void)f;
    if (!project_scan_start(scan)) {
        project_scan_free(scan);
        files_echo(&app->echo, "Cannot scan %S: no thread", p->root);
        return;
    }
    p->scan = scan;
}

static void finder_rows_restart(App *app);

static void finder_scan_ended(App *app, Project *p) {
    ProjectScanResult r = p->scan->result;
    project_scan_free(p->scan);
    p->scan = NULL;
    if (r.cancelled) {
        if (p->scan_shadow) project_index_destroy(&p->index[!p->live]);
        return;
    }
    if (p->scan_shadow) { // the complete new list replaces the old one
        p->live = !p->live;
        project_index_destroy(&p->index[!p->live]);
        if (finder_is_open(app) && app->finder.open == p) finder_rows_restart(app);
    }
    p->scanned = 1;
    p->last = r;
    if (r.limit) {
        echo_message(&app->echo, "Project has more than %D files; listed the first %D", r.files, r.files);
    } else {
        files_echo(&app->echo, "%D files in %S/ (%D ignored%s)", r.files, p->root, r.ignored, r.errors ? ", some directories unreadable" : "");
    }
}

// The workers' results (the platform calls this when one of them woke the main loop).
b32 app_wake(App *app) {
    Finder *f = &app->finder;
    b32 frame = 0;
    for (i32 i = 0; i < PROJECT_KEPT; i++) {
        Project *p = &f->projects[i];
        if (!p->scan) continue;
        i64 added = project_scan_drain(p->scan, &p->index[p->scan_shadow ? !p->live : p->live]);
        if (added && !p->scan_shadow && finder_is_open(app) && f->open == p) {
            app->mini.matcher_pending = 1; // the ranking extends over them
            frame = 1;
        }
        if (p->scan->ended) {
            finder_scan_ended(app, p);
            frame = 1;
        }
    }
    for (i32 i = 0; i < f->dying_count;) {
        project_scan_drain(f->dying[i], NULL);
        if (f->dying[i]->ended) {
            project_scan_free(f->dying[i]);
            f->dying[i] = f->dying[--f->dying_count];
        } else {
            i++;
        }
    }
    return frame;
}

// EVENT_DIR_CHANGED: true when it is a project's tree watch (the index is then stale; nothing else
// happens until it is needed).
static b32 finder_watch_event(App *app, OsWatch watch) {
    for (i32 i = 0; i < PROJECT_KEPT; i++) {
        Project *p = &app->finder.projects[i];
        if (p->watch && p->watch == watch) {
            p->stale = 1;
            return 1;
        }
    }
    return 0;
}

// Exit: every worker is cancelled, then joined within PROJECT_SHUTDOWN_MS in all. A worker still inside a
// file-system call then (a dead network path) is left to the process exit, its memory untouched.
static void finder_shutdown(App *app) {
    Finder *f = &app->finder;
    ProjectScan *all[PROJECT_KEPT + PROJECT_DYING_MAX];
    i32 n = 0;
    for (i32 i = 0; i < PROJECT_KEPT; i++) if (f->projects[i].scan) all[n++] = f->projects[i].scan;
    for (i32 i = 0; i < f->dying_count; i++) all[n++] = f->dying[i];
    for (i32 i = 0; i < n; i++) project_scan_cancel(all[i]);
    u64 deadline = os_time_us() + (u64)PROJECT_SHUTDOWN_MS * 1000;
    for (i32 i = 0; i < n; i++) {
        u64 now = os_time_us();
        u32 left = now >= deadline ? 0 : (u32)((deadline - now) / 1000);
        if (project_scan_join(all[i], left)) {
            project_scan_free(all[i]);
        } else {
            LOG("project: the scan of %S did not end within %d ms of the exit: left to the process exit (abandoned)", all[i]->root,
                PROJECT_SHUTDOWN_MS);
#if TEAL_DEV
            f->dev_abandoned++;
            os_dev_thread_abandoned();
#endif
        }
    }
    f->dying_count = 0;
    for (i32 i = 0; i < PROJECT_KEPT; i++) {
        Project *p = &f->projects[i];
        p->scan = NULL;
        if (p->watch) os_unwatch(p->watch);
        project_index_destroy(&p->index[0]);
        project_index_destroy(&p->index[1]);
    }
    fuzzy_rank_destroy(&f->rank);
    project_cache_destroy(&f->roots);
    if (f->row_arena.base) os_release(f->row_arena.base);
}

// ---------------------------------------------------------------------------
// The finder's matcher (MiniMatcher)

static void finder_begin(Minibuffer *mb, void *data, String8 input, b32 narrow);

static b32 finder_is_open(App *app) {
    return app->mini.active && app->mini.matcher && app->mini.matcher->begin == finder_begin && app->finder.open;
}

static ProjectIndex *finder_index(Finder *f) {
    return &f->open->index[f->open->live];
}

// The empty query's first rows: the files visited this session under the root, most recent first.
static void finder_visited_rows(App *app) {
    Finder *f = &app->finder;
    arena_reset(&f->row_arena);
    f->visited_count = 0;
    MiniHistory *h = &app->mini.histories[MINI_HISTORY_VISITED];
    String8 root = f->open->root;
    for (i32 i = 0; i < h->count && f->visited_count < MINI_HISTORY_MAX; i++) {
        String8 path = h->items[i];
        if (path.len <= root.len + 1 || !project_path_inside(path, root)) continue;
        String8 rel = str8_copy(&f->row_arena, str8(path.data + root.len + 1, path.len - root.len - 1));
        for (i64 k = 0; k < rel.len; k++) if (rel.data[k] == '\\') rel.data[k] = '/';
        b32 seen = 0;
        for (i32 k = 0; k < f->visited_count && !seen; k++) seen = project_path_inside(rel, f->visited[k]) && rel.len == f->visited[k].len;
        if (!seen) f->visited[f->visited_count++] = rel;
    }
    f->empty_count = 0;
    f->empty_scanned = 0;
}

// The ranking starts over on the same input (the index was swapped for a new one).
static void finder_rows_restart(App *app) {
    Finder *f = &app->finder;
    String8 input = minibuffer_input(&app->mini, &app->mini.match_arena);
    if (f->empty) finder_visited_rows(app);
    else fuzzy_rank_begin(&f->rank, finder_index(f), input, 0);
    app->mini.matcher_pending = 1;
}

static void finder_begin(Minibuffer *mb, void *data, String8 input, b32 narrow) {
    (void)mb;
    App *app = data;
    Finder *f = &app->finder;
    b32 was_empty = f->empty;
    f->empty = 1;
    for (i64 i = 0; i < input.len; i++) f->empty &= input.data[i] == ' ';
    if (f->empty) {
        finder_visited_rows(app);
        return;
    }
    fuzzy_rank_begin(&f->rank, finder_index(f), input, narrow && !was_empty && f->rank.x == finder_index(f));
}

static b32 finder_work(void *data, u64 deadline, i64 budget) {
    App *app = data;
    Finder *f = &app->finder;
    if (!f->open) return 1;
    if (!f->empty) return fuzzy_rank_work(&f->rank, deadline, budget);
    // The empty query: the index in scan order after the visited files, without them again.
    ProjectIndex *x = finder_index(f);
    u8 path[PROJECT_PATH_MAX];
    for (; f->empty_scanned < x->file_count && f->visited_count + f->empty_count < FUZZY_TOP; f->empty_scanned++) {
        i64 len = project_index_path(x, f->empty_scanned, path, sizeof(path));
        b32 visited = 0;
        for (i32 k = 0; k < f->visited_count && !visited; k++) visited = str8_equal(f->visited[k], str8(path, len));
        if (!visited) f->empty_rows[f->empty_count++] = (u32)f->empty_scanned;
    }
    return 1;
}

static void finder_finish(void *data) {
    App *app = data;
    if (app->finder.open && !app->finder.empty) fuzzy_rank_finish(&app->finder.rank);
}

static i64 finder_rows(void *data, i64 *total) {
    App *app = data;
    Finder *f = &app->finder;
    if (!f->open) {
        *total = 0;
        return 0;
    }
    if (f->empty) {
        i64 rows = f->visited_count + f->empty_count;
        *total = MAX(rows, finder_index(f)->file_count);
        return rows;
    }
    *total = f->rank.total;
    return fuzzy_rank_rows(&f->rank);
}

static u64 finder_row_id(void *data, i64 row) {
    Finder *f = &((App *)data)->finder;
    if (f->empty) return row < f->visited_count ? (1ull << 32) + (u64)row : f->empty_rows[row - f->visited_count];
    return f->rank.rows[row];
}

static String8 finder_text(void *data, Arena *arena, i64 row) {
    Finder *f = &((App *)data)->finder;
    if (f->empty && row < f->visited_count) return str8_copy(arena, f->visited[row]);
    u32 index = f->empty ? f->empty_rows[row - f->visited_count] : f->rank.rows[row];
    u8 *buf = PUSH_ARRAY(arena, u8, PROJECT_PATH_MAX);
    return str8(buf, project_index_path(finder_index(f), index, buf, PROJECT_PATH_MAX));
}

static i32 finder_spans(void *data, i64 row, MatchSpan *out, i32 cap) {
    Finder *f = &((App *)data)->finder;
    if (f->empty) return 0;
    u8 buf[PROJECT_PATH_MAX];
    String8 path = str8(buf, project_index_path(finder_index(f), f->rank.rows[row], buf, sizeof(buf)));
    i64 name = path.len;
    while (name > 0 && path.data[name - 1] != '/') name--;
    i32 n = 0;
    fuzzy_score(&f->rank.q, path, name, out, cap, &n);
    return n;
}

static String8 finder_status(void *data, Arena *arena) {
    Finder *f = &((App *)data)->finder;
    Project *p = f->open;
    if (!p) return str8(NULL, 0);
    if (p->scan && !p->scan_shadow) return str8_fmt(arena, "scanning: %D files", finder_index(f)->file_count);
    if (p->scan) return STR8_LIT("rescanning");
    if (p->last.limit) return str8_fmt(arena, "stopped at %D files", p->last.files);
    return str8(NULL, 0);
}

static const MiniMatcher FINDER_MATCHER = {
    finder_begin, finder_work, finder_finish, finder_rows, finder_row_id, finder_text, finder_spans, finder_status, 1,
};

// ---------------------------------------------------------------------------
// Commands

// RET (or C-j: the input as typed, relative to the root): visits root/relative.
static void finder_visit(CommandContext *ctx, MiniResult *r, b32 other) {
    App *app = ctx->app;
    String8 root = ctx->mini->state.text;
    String8 rel = r->text;
    while (rel.len && (rel.data[0] == '/' || rel.data[0] == '\\')) rel = str8(rel.data + 1, rel.len - 1);
    if (!rel.len || !root.len) return;
    arena_reset(&app->files_arena);
    String8 path = str8_fmt(&app->files_arena, "%S\\%S", root, rel);
    for (i64 i = root.len; i < path.len; i++) if (path.data[i] == '/') path.data[i] = '\\';
    OsFileInfo info;
    if (os_file_info(path, &info) == OS_FILE_OK && info.is_dir) {
        files_echo(ctx->echo, "%S is a directory", path);
        return;
    }
    Buffer *buf = app_find_file(app, path);
    if (!buf) return;
    if (other) {
        app_pop_to_buffer(ctx, buf);
    } else {
        app_switch_buffer(app, ctx->view, buf);
        ctx->cursor = &ctx->view->cursors[0];
    }
}

static void finder_done(CommandContext *ctx, MiniResult *r) { finder_visit(ctx, r, 0); }
static void finder_done_other(CommandContext *ctx, MiniResult *r) { finder_visit(ctx, r, 1); }

static void finder_open(CommandContext *ctx, b32 other) {
    App *app = ctx->app;
    Finder *f = &app->finder;
    if (ctx->mini->active) {
        echo_message(ctx->echo, "Command attempted to use minibuffer while in minibuffer");
        return;
    }
    String8 root = finder_root(app, ctx->view->buffer);
    if (!root.len) {
        echo_message(ctx->echo, PROJECT_NOT_IN_PROJECT);
        return;
    }
    Project *p = finder_project(app, root);
    if (!p->scan && (!p->scanned || p->stale)) finder_scan(app, p);
    f->open = p;
    f->empty = 1;
    arena_reset(&app->files_arena);
    String8 shown = str8_fmt(&app->files_arena, "Find file in %S/: ", p->root);
    for (i64 i = 0; i < shown.len; i++) if (shown.data[i] == '\\') shown.data[i] = '/';
    MiniRequest req = { .kind = MINI_CHOICE, .prompt = shown, .history = MINI_HISTORY_PROJECT, .done = other ? finder_done_other : finder_done,
                        .matcher = &FINDER_MATCHER, .data = app };
    if (minibuffer_read(ctx, &req)) ctx->mini->state.text = str8_copy(&ctx->mini->arena, p->root);
}

static void cmd_project_find_file(CommandContext *ctx) { finder_open(ctx, 0); }
static void cmd_project_find_file_other_window(CommandContext *ctx) { finder_open(ctx, 1); }

static void cmd_project_refresh(CommandContext *ctx) {
    App *app = ctx->app;
    project_cache_reset(&app->finder.roots);
    String8 root = finder_root(app, ctx->view->buffer);
    if (!root.len) {
        echo_message(ctx->echo, PROJECT_NOT_IN_PROJECT);
        return;
    }
    Project *p = finder_project(app, root);
    files_echo(ctx->echo, "Scanning %S/...", p->root);
    finder_scan(app, p);
}

const Command CMD_PROJECT_FIND_FILE = { "project-find-file", cmd_project_find_file, COMMAND_ONCE };
const Command CMD_PROJECT_FIND_FILE_OTHER_WINDOW = { "project-find-file-other-window", cmd_project_find_file_other_window, COMMAND_ONCE };
const Command CMD_PROJECT_REFRESH = { "project-refresh", cmd_project_refresh, COMMAND_ONCE };

#if TEAL_DEV
// Tests: waits for the workers' results until no scan runs (or the timeout); false on the timeout.
static b32 finder_dev_wait(App *app, u32 timeout_ms) {
    u64 start = os_time_us();
    for (;;) {
        b32 running = app->finder.dying_count > 0;
        for (i32 i = 0; i < PROJECT_KEPT; i++) running |= app->finder.projects[i].scan != NULL;
        if (!running) return 1;
        if (os_time_us() - start > (u64)timeout_ms * 1000) return 0;
        os_dev_wake_wait(20);
        app_wake(app);
    }
}

// Releases every held scan (--scan-hold).
static void finder_dev_release(App *app) {
    for (i32 i = 0; i < PROJECT_KEPT; i++) {
        ProjectScan *s = app->finder.projects[i].scan;
        if (!s) continue;
        os_atomic_set(&s->dev_hold, 0);
        os_event_set(s->event);
    }
    app->finder.dev_hold_after = 0;
}
#endif
