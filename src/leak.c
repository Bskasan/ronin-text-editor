// leak.c — dev only: --leak-check (Phase 16; docs/ARCHITECTURE.md, "Memory and resource leak audit"). Each scenario
// repeats an operation that ends in the state it started from. After a prime step that fills the bounded caches
// (*Messages*, the minibuffer history, the kill ring) come 20 warm-up cycles, then 4 windows of N/4 cycles; every
// metric is sampled at the end of the warm-up and of each window. A verdict metric that grows from one window to
// the next in any of the last three is a LEAK (exit 6), private commit only when it grows by more than 64 KB in two
// of them (one such step is a STEP); an informational one that rises in each of them is a WATCH.
// Every cycle must leave the state the first one left (exit 9 otherwise). Part of the app, as test.c: it reads App
// directly; the platform drives the window through LeakHost.

#if RONIN_DEV

#define LEAK_WARMUP 20
#define LEAK_WINDOWS 4
#define LEAK_PRIVATE_TOLERANCE KB(64) // private commit between windows (OS noise); every other verdict metric: exact
#define LEAK_STEP_MS 60000            // a step that does not settle within this fails the run

enum { LEAK_GROWTH, LEAK_BOUNDED, LEAK_INFO };

enum {
    LEAK_M_PRIVATE,
    LEAK_M_GDI,
    LEAK_M_LEDGER,
    LEAK_M_THREADS,
    LEAK_M_EVENTS,
    LEAK_M_PROCESSES,
    LEAK_M_WATCHES,
    LEAK_M_RESERVATIONS,
    LEAK_M_COMMITTED,
    LEAK_M_ATLAS_AREA,
    LEAK_M_GLYPHS,
    LEAK_M_D3D,
    LEAK_M_ATLAS_ROWS,
    LEAK_M_UNDO,
    LEAK_M_SLOTS,
    LEAK_M_HANDLES,
    LEAK_M_USER,
    LEAK_M_WORKING_SET,
    LEAK_M_PROCESS_THREADS,
    LEAK_M_VIDEO,
    LEAK_M_RESETS,
    LEAK_M_COUNT
};

typedef struct LeakMetricInfo {
    const char *name;
    i32 kind;
    i64 tolerance; // LEAK_GROWTH: allowed growth between windows
} LeakMetricInfo;

static const LeakMetricInfo leak_metrics[LEAK_M_COUNT] = {
    { "private commit (bytes)", LEAK_GROWTH, LEAK_PRIVATE_TOLERANCE },
    { "GDI objects", LEAK_GROWTH, 0 },
    { "handle ledger (ours)", LEAK_GROWTH, 0 },
    { "our threads", LEAK_GROWTH, 0 },
    { "our events", LEAK_GROWTH, 0 },
    { "our processes", LEAK_GROWTH, 0 },
    { "directory watches", LEAK_GROWTH, 0 },
    { "our reservations", LEAK_GROWTH, 0 },
    { "our committed bytes", LEAK_GROWTH, 0 },
    { "atlas used area (texels)", LEAK_GROWTH, 0 },
    { "cached glyphs", LEAK_GROWTH, 0 },
    { "D3D11 live objects", LEAK_GROWTH, 0 },
    { "atlas rows", LEAK_BOUNDED, 0 },
    { "undo log (bytes)", LEAK_BOUNDED, 0 },
    { "worker thread slots", LEAK_BOUNDED, 0 }, // reused threads: the most tasks ever running at once
    // Windows' thread pool, RPC runtime and graphics stack move these on their own (up to +8 and back over 2,000
    // config reloads): the ledger and our own counters are the verdict on handles.
    { "process handles", LEAK_INFO, 0 },
    { "USER objects", LEAK_INFO, 0 },
    { "working set (bytes)", LEAK_INFO, 0 },
    { "process threads (Toolhelp)", LEAK_INFO, 0 },
    { "video memory, local (bytes)", LEAK_INFO, 0 },
    { "font resets", LEAK_INFO, 0 },
};

typedef struct Leak {
    App *app;
    LeakHost *host;
    Arena *arena;
    String8 dir;          // tmp\leak
    String8 dir_fwd;      // ... with forward slashes, as find-file shows it
    String8 bench_file;   // tmp\bench_100mb.txt
    String8 shots_dir;    // build\shots: the screenshots before and after
    Buffer *home;         // home.txt: the buffer every scenario starts and ends on
    Buffer *undo_buffer;  // undo-cycle's buffer, kept across its cycles
    i32 failed;           // steps that failed (exit 9)
    i32 leaks;            // LEAK verdicts
    i32 mismatches;       // screenshots that differ
    char watch[1024];     // the WATCH verdicts, "scenario: metric; ..."
    i64 watch_len;
    OsDevHandleType types[LEAK_WINDOWS + 1][64]; // the process's handles by type at each sample
    i32 type_count[LEAK_WINDOWS + 1];
    OsDevMemoryOwners owners[LEAK_WINDOWS + 1];  // the private memory by owner at each sample
    OsDevReservation *res_first, *res_last;      // our reservations after window 1 and window 4 (what grew between)
    i32 res_first_count, res_last_count;
} Leak;

typedef struct LeakScenario {
    const char *name;
    b32 (*setup)(Leak *L);    // may be NULL
    b32 (*cycle)(Leak *L);    // false: a step failed
    void (*teardown)(Leak *L); // may be NULL
    b32 five;                 // 5 cycles: 1 warm-up, 4 windows of 1 (the 100 MB file)
    b32 shots;                // a screenshot before the first cycle and after the last, compared pixel-exact
    b32 from_start;           // the state is compared with the one before the first cycle (not after it)
} LeakScenario;

// ---------------------------------------------------------------------------
// Driving the app

static void leak_fail(Leak *L, const char *what, String8 detail) {
    LOG("leak: FAIL: %s %S", what, detail);
    L->failed++;
}

// Workers, prompts and timers that still have something to do: the app is not settled.
static b32 leak_busy(App *app) {
    b32 busy = app_wants_frame(app) || app_dev_results_busy(app) || app_dev_compile_busy(app) || app->finder.dying_count > 0 ||
               app_wait_ms(app) != CONFIG_WAIT_INFINITE || cursor_job_pending(&app->cursor_job) || replace_pending(&app->replace) ||
               app_dev_git_busy(app);
    for (i32 i = 0; i < PROJECT_KEPT; i++) busy |= app->finder.projects[i].scan != NULL;
    return busy;
}

// Until nothing is pending and a pass of the loop brings nothing new.
static b32 leak_settle(Leak *L) {
    u64 start = os_time_us();
    for (;;) {
        b32 busy = leak_busy(L->app);
        b32 did = L->host->pump(L->host->ctx, busy ? 2 : 0);
        if (!busy && !did) return 1;
        if (os_time_us() - start > (u64)LEAK_STEP_MS * 1000) {
            leak_fail(L, "the app did not settle within the step's time", str8(NULL, 0));
            return 0;
        }
    }
}

// Pumps until done(L) holds (the app need not be idle meanwhile).
static b32 leak_wait(Leak *L, b32 (*done)(Leak *L), const char *what) {
    u64 start = os_time_us();
    while (!done(L)) {
        L->host->pump(L->host->ctx, 2);
        if (os_time_us() - start > (u64)LEAK_STEP_MS * 1000) {
            leak_fail(L, "timed out waiting for", str8_cstr(what));
            return 0;
        }
    }
    return 1;
}

static void leak_feed(Leak *L, String8 keys, b32 frame_each) {
    u64 mark = arena_pos(L->arena);
    i32 cap = (i32)keys.len * 2 + 2;
    Event *events = PUSH_ARRAY(L->arena, Event, cap);
    i32 n = app_dev_key_events(L->app, keys, events, cap);
    L->host->feed(L->host->ctx, events, n, frame_each);
    arena_pop_to(L->arena, mark);
}

// Keys in --keys notation, a frame after each, then settled.
static b32 leak_keys(Leak *L, const char *keys) {
    leak_feed(L, str8_cstr(keys), 1);
    return leak_settle(L);
}

// Text typed a character at a time.
static b32 leak_type(Leak *L, String8 text) {
    u64 mark = arena_pos(L->arena);
    u8 *keys = PUSH_ARRAY(L->arena, u8, text.len * 4 + 1);
    i64 n = 0;
    for (i64 i = 0; i < text.len; i++) {
        if (text.data[i] == ' ') {
            memcpy(keys + n, "SPC ", 4);
            n += 4;
        } else {
            keys[n++] = text.data[i];
            keys[n++] = ' ';
        }
    }
    leak_feed(L, str8(keys, n), 1);
    arena_pop_to(L->arena, mark);
    return leak_settle(L);
}

static Buffer *leak_current(Leak *L) {
    return app_selected_view(L->app)->buffer;
}

// kill-buffer on the selected buffer (C-x k RET), answering the question of a modified one.
static b32 leak_kill_current(Leak *L) {
    if (!leak_keys(L, "C-x k RET")) return 0;
    if (L->app->mini.active && L->app->mini.kind == MINI_YES_NO && !leak_keys(L, "y e s RET")) return 0;
    return !L->app->mini.active;
}

// find-file of a name in tmp\leak from a buffer there (its directory is the prompt's initial input).
static b32 leak_find_file(Leak *L, const char *name) {
    if (!leak_keys(L, "C-x C-f")) return 0;
    if (!leak_type(L, str8_cstr(name))) return 0;
    if (!leak_keys(L, "RET")) return 0;
    return !L->app->mini.active;
}

// ---------------------------------------------------------------------------
// The state a cycle must leave, as text (compared, and logged when it differs)

static u64 leak_hash(String8 s) {
    u64 h = 1469598103934665603ull;
    for (i64 i = 0; i < s.len; i++) h = (h ^ s.data[i]) * 1099511628211ull;
    return h;
}

static String8 leak_state(Leak *L, Arena *a) {
    App *app = L->app;
    String8 s = str8_fmt(a, "buffers %d:", app->buffers.count);
    for (i32 i = 0; i < app->buffers.count; i++) {
        Buffer *b = app->buffers.entries[i].buffer;
        b32 results = b == app->messages;
        for (i32 k = 0; k < RESULTS_MAX; k++) results |= app->results[k].buffer == b;
        if (results) { // its text holds times: the name only
            s = str8_fmt(a, "%S %S", s, b->name);
            continue;
        }
        s = str8_fmt(a, "%S %S%s(%D)", s, b->name, b->modified ? "*" : "", buffer_size(b));
        if (b == L->undo_buffer) s = str8_fmt(a, "%S#%X", s, leak_hash(buffer_text(b, a, 0, buffer_size(b))));
    }
    i32 leaves[WINDOW_MAX];
    i32 n = window_leaves(&app->windows, leaves);
    s = str8_fmt(a, "%S; windows %d:", s, n);
    for (i32 i = 0; i < n; i++) {
        WindowNode *w = &app->windows.nodes[leaves[i]];
        View *v = w->view;
        s = str8_fmt(a, "%S [%d %d %d %d %S point %D cursors %d%s]", s, w->x, w->y, w->w, w->h, v->buffer->name,
                     view_point(v, &v->cursors[v->primary]), v->cursor_count, leaves[i] == app->windows.selected ? " selected" : "");
    }
    i32 running = 0, scans = 0;
    for (i32 i = 0; i < RESULTS_MAX; i++) running += app->results[i].buffer && app->results[i].running;
    for (i32 i = 0; i < PROJECT_KEPT; i++) scans += app->finder.projects[i].scan != NULL;
    s = str8_fmt(a, "%S; modal %d minibuffer %d isearch %d replace %d cursor job %d; running %d scans %d dying %d %d %d; git %d %d %d", s,
                 app->modal.active, app->mini.active, app->isearch.active, (i32)app->replace.state, (i32)app->cursor_job.kind, running, scans, app->finder.dying_count,
                 app->grep_dying_count, app->compile_dying_count, app->git.pending != NULL, app->git.view != NULL, app->git.dying_count);
    return s;
}

// ---------------------------------------------------------------------------
// Metrics, the verdict and the table

static void leak_sample(Leak *L, i32 w, i64 *v, i64 *caps) {
    L->type_count[w] = os_dev_handle_types(L->types[w], ARRAY_COUNT(L->types[w]));
    os_dev_memory_owners(&L->owners[w]);
    if (w == 1) L->res_first_count = os_dev_reservations(L->res_first, 16384);
    if (w == LEAK_WINDOWS) L->res_last_count = os_dev_reservations(L->res_last, 16384);
    OsDevMetrics m;
    os_dev_metrics(&m);
    RDevGpuMetrics g;
    r_dev_gpu_metrics(L->host->renderer, &g);
    FontDevStats f;
    font_dev_stats(L->app->font, &f);
    v[LEAK_M_PRIVATE] = (i64)m.private_bytes;
    v[LEAK_M_HANDLES] = m.handles;
    v[LEAK_M_GDI] = m.gdi_objects;
    v[LEAK_M_USER] = m.user_objects;
    v[LEAK_M_LEDGER] = m.live_handles;
    v[LEAK_M_THREADS] = m.live_threads;
    v[LEAK_M_EVENTS] = m.live_events;
    v[LEAK_M_PROCESSES] = m.live_processes;
    v[LEAK_M_WATCHES] = m.watches;
    v[LEAK_M_RESERVATIONS] = m.reservations;
    v[LEAK_M_COMMITTED] = (i64)m.reserved_committed;
    v[LEAK_M_ATLAS_AREA] = f.used_area;
    v[LEAK_M_GLYPHS] = f.glyphs;
    v[LEAK_M_D3D] = g.live_objects;
    v[LEAK_M_ATLAS_ROWS] = f.atlas_rows;
    v[LEAK_M_UNDO] = L->undo_buffer ? (i64)buffer_dev_undo_used(L->undo_buffer) : -1;
    v[LEAK_M_SLOTS] = m.worker_slots;
    caps[LEAK_M_SLOTS] = m.worker_slots_max;
    v[LEAK_M_WORKING_SET] = (i64)m.working_set;
    v[LEAK_M_PROCESS_THREADS] = m.process_threads;
    v[LEAK_M_VIDEO] = (i64)g.video_local;
    v[LEAK_M_RESETS] = f.reset_count;
    caps[LEAK_M_ATLAS_ROWS] = f.atlas_size;
    caps[LEAK_M_UNDO] = (i64)L->app->config->settings.undo_limit_mb << 20;
}

static void leak_watch_note(Leak *L, const char *scenario, const char *metric, const char *verdict) {
    i64 n = fmt_buf((u8 *)L->watch + L->watch_len, (i64)sizeof(L->watch) - L->watch_len, "%s%s: %s (%s)", L->watch_len ? "; " : "", scenario,
                    metric, verdict);
    L->watch_len = MIN(L->watch_len + n, (i64)sizeof(L->watch) - 1);
}

// v[0]: after the warm-up, v[1..4]: after each window of `per` cycles. Logs the table; returns the LEAK verdicts.
static i32 leak_verdict(Leak *L, const char *scenario, i64 v[LEAK_WINDOWS + 1][LEAK_M_COUNT], i64 *caps, i32 per) {
    i32 leaks = 0;
    LOG("leak: | %s | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle (last 3 windows) | verdict |", scenario);
    LOG("leak: |---|---|---|---|---|---|---|---|");
    for (i32 k = 0; k < LEAK_M_COUNT; k++) {
        const LeakMetricInfo *mi = &leak_metrics[k];
        if (v[0][k] < 0) continue; // not measured here (the undo log outside undo-cycle, live objects without the debug layer)
        const char *verdict = "OK";
        if (mi->kind == LEAK_GROWTH) {
            i32 steps = 0;
            for (i32 w = 2; w <= LEAK_WINDOWS; w++) steps += v[w][k] - v[w - 1][k] > mi->tolerance;
            if (k == LEAK_M_PRIVATE) {
                // Steady growth only: a single step (the process heap committing one more chunk for threads Windows
                // starts in the process, which the GPU driver charges) is a STEP, listed but not failing the run.
                verdict = steps >= 2 ? "LEAK" : steps ? "STEP" : "OK";
                if (steps == 1) leak_watch_note(L, scenario, mi->name, "STEP");
            } else if (steps) {
                verdict = "LEAK";
            }
        } else if (mi->kind == LEAK_BOUNDED) {
            for (i32 w = 0; w <= LEAK_WINDOWS; w++) if (v[w][k] > caps[k]) verdict = "LEAK (over its cap)";
        } else {
            b32 rising = 1;
            for (i32 w = 2; w <= LEAK_WINDOWS; w++) rising &= v[w][k] > v[w - 1][k];
            verdict = rising ? "WATCH" : "";
            if (rising) leak_watch_note(L, scenario, mi->name, "WATCH");
        }
        if (verdict[0] == 'L') leaks++;
        i64 delta = v[LEAK_WINDOWS][k] - v[1][k], cycles = (i64)(LEAK_WINDOWS - 1) * per;
        i64 tenths = delta * 10 / cycles;
        i64 mag = tenths < 0 ? -tenths : tenths;
        u8 cap_text[64];
        i64 cap_len = mi->kind == LEAK_BOUNDED ? fmt_buf(cap_text, sizeof(cap_text), " (cap %D)", caps[k]) : 0;
        LOG("leak: | %s | %D | %D | %D | %D | %D | %s%D.%D | %s%S |", mi->name, v[0][k], v[1][k], v[2][k], v[3][k], v[4][k],
            tenths < 0 ? "-" : "", mag / 10, mag % 10, verdict, str8(cap_text, cap_len));
    }
    // Our reservations that grew between window 1 and window 4, by the functions that made them.
    if (v[LEAK_WINDOWS][LEAK_M_COMMITTED] != v[1][LEAK_M_COMMITTED] || v[LEAK_WINDOWS][LEAK_M_RESERVATIONS] != v[1][LEAK_M_RESERVATIONS]) {
        u8 prefix[96];
        fmt_buf(prefix, sizeof(prefix), "leak:   %s: our reservations from window 1 to window 4%c", scenario, 0);
        dev_log_reservation_growth((const char *)prefix, L->res_first, L->res_first_count, L->res_last, L->res_last_count, L->arena);
    }
    // The private memory by owner when it moved between the windows, and the process's handles by type wherever they
    // moved between the samples.
    if (v[LEAK_WINDOWS][LEAK_M_PRIVATE] != v[1][LEAK_M_PRIVATE]) {
        static const char *names[] = { "our arenas", "process heaps", "thread stacks", "other private (driver, runtime)", "writable image pages", "writable mapped views" };
        for (i32 k = 0; k < 6; k++) {
            u64 *o[LEAK_WINDOWS + 1];
            for (i32 w = 0; w <= LEAK_WINDOWS; w++) o[w] = &L->owners[w].arenas + k;
            LOG("leak:   private memory, %s: %U | %U | %U | %U | %U", names[k], *o[0], *o[1], *o[2], *o[3], *o[4]);
        }
    }
    for (i32 i = 0; i < L->type_count[0]; i++) {
        String8 name = str8(L->types[0][i].name, L->types[0][i].name_len);
        i32 c[LEAK_WINDOWS + 1];
        b32 moved = 0;
        for (i32 w = 0; w <= LEAK_WINDOWS; w++) {
            c[w] = 0;
            for (i32 j = 0; j < L->type_count[w]; j++) {
                if (str8_equal(str8(L->types[w][j].name, L->types[w][j].name_len), name)) c[w] = L->types[w][j].count;
            }
            moved |= c[w] != c[0];
        }
        if (moved) LOG("leak:   handles of type %S: %d | %d | %d | %d | %d", name, c[0], c[1], c[2], c[3], c[4]);
    }
    return leaks;
}

// ---------------------------------------------------------------------------
// Screenshots: the frame before the first cycle against the frame after the last

static u8 *leak_shot(Leak *L, Arena *a, i32 *w, i32 *h) {
    echo_clear(&L->app->echo);
    leak_settle(L);
    return L->host->capture(L->host->ctx, a, w, h);
}

static void leak_compare_shots(Leak *L, const char *scenario, u8 *before, i32 bw, i32 bh, u8 *after, i32 aw, i32 ah) {
    String8 name = str8_cstr(scenario);
    u8 file[96];
    i64 n = 0;
    for (i64 i = 0; i < name.len && n < (i64)sizeof(file) - 1; i++) file[n++] = name.data[i] == '/' ? '_' : name.data[i];
    os_make_dir(L->shots_dir);
    String8 base = str8_fmt(L->arena, "%S\\leak_%S", L->shots_dir, str8(file, n));
    if (before) L->host->write_png(L->host->ctx, str8_fmt(L->arena, "%S_before.png", base), before, bw, bh);
    if (after) L->host->write_png(L->host->ctx, str8_fmt(L->arena, "%S_after.png", base), after, aw, ah);
    i64 differ = -1;
    if (before && after && bw == aw && bh == ah) {
        differ = 0;
        for (i64 i = 0; i < (i64)bw * bh * 4; i += 4) differ += before[i] != after[i] || before[i + 1] != after[i + 1] || before[i + 2] != after[i + 2];
    }
    if (differ) {
        LOG("leak: FAIL: %s: the frame after the last cycle differs from the one before the first (%D pixels, %dx%d against %dx%d)",
            scenario, differ, aw, ah, bw, bh);
        L->mismatches++;
    } else {
        LOG("leak: %s: the frame after the last cycle equals the one before the first, pixel for pixel (%dx%d)", scenario, bw, bh);
    }
}

// ---------------------------------------------------------------------------
// The scenarios

static b32 leak_file_cycle(Leak *L) {
    if (!leak_find_file(L, "sample.c") || !str8_equal(leak_current(L)->name, STR8_LIT("sample.c"))) return 0;
    if (!leak_keys(L, "M-> h e l l o SPC w o r l d DEL DEL DEL DEL DEL DEL DEL DEL DEL DEL DEL")) return 0;
    return leak_kill_current(L) && leak_current(L) == L->home;
}

static b32 leak_file_100mb_cycle(Leak *L) {
    if (!leak_keys(L, "C-x C-f DEL")) return 0; // up from tmp\leak to tmp
    if (!leak_type(L, STR8_LIT("bench_100mb.txt")) || !leak_keys(L, "RET")) return 0;
    if (!str8_equal(leak_current(L)->name, STR8_LIT("bench_100mb.txt"))) return 0;
    if (!leak_keys(L, "M-> h e l l o DEL DEL DEL DEL DEL")) return 0;
    return leak_kill_current(L) && leak_current(L) == L->home;
}

static b32 leak_window_split_cycle(Leak *L) {
    if (!leak_keys(L, "C-x 2 C-x 3 C-x o C-x 0 C-x 0")) return 0;
    return window_count(&L->app->windows) == 1;
}

static b32 leak_window_one_cycle(Leak *L) {
    // Four equal columns, then each column split once (C-x o twice reaches the next column's window).
    if (!leak_keys(L, "C-x 3 C-x 3 C-x 3 C-x + C-x 2 C-x o C-x o C-x 2 C-x o C-x o C-x 2 C-x o C-x o C-x 2")) return 0;
    if (window_count(&L->app->windows) != 8) return 0;
    if (!leak_keys(L, "C-x 1")) return 0;
    return window_count(&L->app->windows) == 1;
}

static b32 leak_mx_cycle(Leak *L) {
    return leak_keys(L, "M-x m i n i b") && L->app->mini.active && leak_keys(L, "C-g") && !L->app->mini.active;
}

// The command list: open, filter, move, C-g (the frame and the state as before the first cycle); and open, run
// goto-line through it, cancel its prompt.
static b32 leak_modal_setup(Leak *L) {
    echo_clear(&L->app->echo); // the frames before and after are compared: no message left from earlier scenarios
    return 1;
}

static b32 leak_modal_browse_cycle(Leak *L) {
    return leak_keys(L, "C-S-p g o t o <down> <down> <up>") && L->app->modal.active && leak_keys(L, "C-g") && !L->app->modal.active;
}

static b32 leak_modal_run_cycle(Leak *L) {
    return leak_keys(L, "C-S-p g o t o - l i n e RET") && !L->app->modal.active && L->app->mini.active && leak_keys(L, "C-g") &&
           !L->app->mini.active;
}

// The git changes view with the fake git (tmp\leak\git: two changed files): open, a diff each, TAB, scroll, close (the
// frame and the state as before the first cycle); and opened, then closed while its status still runs (tmp\leak\git_hang):
// git cancelled, its job freed when it ended.
static b32 leak_git_use(Leak *L, const char *name) {
    App *app = L->app;
    app->git.dev_program = os_dev_exe_path(L->arena);
    app->git.dev_prefix = str8_fmt(L->arena, "--fake-git \"%S\\%s\"", L->dir, name);
    app->git.dev_env = str8(NULL, 0);
    echo_clear(&app->echo);
    return 1;
}

static b32 leak_git_setup(Leak *L) {
    return leak_git_use(L, "git");
}

static b32 leak_git_hang_setup(Leak *L) {
    return leak_git_use(L, "git_hang");
}

static void leak_git_teardown(Leak *L) {
    L->app->git.dev_program = L->app->git.dev_prefix = str8(NULL, 0);
}

static b32 leak_git_open(Leak *L) {
    return modal_active(L->app);
}

static b32 leak_git_browse_cycle(Leak *L) {
    if (!leak_keys(L, "C-x g") || !modal_active(L->app) || !L->app->git.view || L->app->git.view->count != 2) return 0;
    return leak_keys(L, "<down> TAB <down> C-v M-v TAB <up>") && modal_active(L->app) && leak_keys(L, "C-g") && !modal_active(L->app);
}

static b32 leak_git_cancel_cycle(Leak *L) {
    leak_feed(L, STR8_LIT("C-x g"), 1);
    if (!leak_wait(L, leak_git_open, "the box (rev-parse answered, status hanging)")) return 0;
    if (!L->app->git.view || !L->app->git.view->status_job) return 0;
    leak_feed(L, STR8_LIT("C-g"), 1);
    return !modal_active(L->app) && L->app->git.dying_count == 1 && leak_settle(L) && !L->app->git.dying_count;
}

// The terminals with the fake terminal (tmp\leak\term): tmp\leak\term\proj\sub\a.c shown (proj a project), one launch a
// cycle, C-c t and C-c a by turns; the teardown waits for every call and checks each line.
static b32 leak_terminal_setup(Leak *L) {
    App *app = L->app;
    String8 dir = str8_fmt(L->arena, "%S\\term", L->dir), proj = str8_fmt(L->arena, "%S\\proj", dir);
    os_make_dir(dir);
    os_make_dir(proj);
    os_make_dir(str8_fmt(L->arena, "%S\\sub", proj));
    os_write_file(str8_fmt(L->arena, "%S\\.ronin-project", proj), str8(NULL, 0));
    os_write_file(str8_fmt(L->arena, "%S\\sub\\a.c", proj), STR8_LIT("int a;\n"));
    os_write_file(str8_fmt(L->arena, "%S\\calls.txt", dir), str8(NULL, 0));
    app->terminal.dev_program = os_dev_exe_path(L->arena);
    app->terminal.dev_prefix = str8_fmt(L->arena, "--fake-terminal \"%S\"", dir);
    app->terminal.dev_wt_present = 1;
    app->terminal.dev_launches = 0;
    return app_dev_visit(app, str8_fmt(L->arena, "%S\\sub\\a.c", proj));
}

static b32 leak_terminal_cycle(Leak *L) {
    App *app = L->app;
    u64 before = app->terminal.dev_launches;
    b32 ai = before % 2;
    if (!leak_keys(L, ai ? "C-c a" : "C-c t")) return 0;
    String8 echo = str8(app->echo.text, app->echo.len);
    return app->terminal.dev_launches == before + 1 && str8_starts_with(echo, ai ? STR8_LIT("Opened claude in ") : STR8_LIT("Opened terminal in "));
}

static void leak_terminal_teardown(Leak *L) {
    App *app = L->app;
    String8 dir = str8_fmt(L->arena, "%S\\term", L->dir), proj = str8_fmt(L->arena, "%S\\proj", dir), sub = str8_fmt(L->arena, "%S\\sub", proj);
    String8 want[2] = { str8_fmt(L->arena, "%S|wt.exe|0|-d \"%S\"", sub, sub),
                        str8_fmt(L->arena, "%S|wt.exe|0|-d \"%S\" \"%S\" /s /k \"claude\"", proj, proj,
                                 os_system_program(L->arena, STR8_LIT("cmd.exe"))) };
    i64 launches = (i64)app->terminal.dev_launches, lines = 0, kinds[3] = { 0 };
    String8 text = { 0 };
    OsEvent tick;
    os_event_create(&tick);
    u64 deadline = os_time_us() + 30000 * 1000ull; // every fake has written its line (each one a process start)
    for (;;) {
        text = str8(NULL, 0);
        os_read_small_file(L->arena, str8_fmt(L->arena, "%S\\calls.txt", dir), MB(1), &text);
        lines = 0;
        for (i64 i = 0; i < text.len; i++) lines += text.data[i] == '\n';
        if (lines >= launches || os_time_us() > deadline) break;
        os_event_wait(tick, 10);
    }
    os_event_destroy(tick);
    for (i64 at = 0; at < text.len;) {
        i64 end = at;
        while (end < text.len && text.data[end] != '\n') end++;
        String8 line = str8(text.data + at, end - at);
        kinds[str8_equal(line, want[0]) ? 0 : str8_equal(line, want[1]) ? 1 : 2]++;
        at = end + 1;
    }
    if (lines != launches || kinds[0] != (launches + 1) / 2 || kinds[1] != launches / 2 || kinds[2]) {
        leak_fail(L, "terminal-cycle", str8_fmt(L->arena, "%D launches, %D calls (%D open-terminal, %D open-ai-terminal, %D other)", launches,
                                                lines, kinds[0], kinds[1], kinds[2]));
    } else {
        LOG("leak: terminal-cycle: %D launches, every call as expected (%D open-terminal, %D open-ai-terminal)", launches, kinds[0], kinds[1]);
    }
    memset(&app->terminal, 0, sizeof(app->terminal));
    leak_kill_current(L);
}

static b32 leak_find_file_prompt_cycle(Leak *L) {
    return leak_keys(L, "C-x C-f") && L->app->mini.active && L->app->mini.match_count > 0 && leak_keys(L, "C-g") && !L->app->mini.active;
}

static b32 leak_goto_line_cycle(Leak *L) {
    return leak_keys(L, "M-g g 2 RET") && !L->app->mini.active && leak_keys(L, "M-<");
}

static b32 leak_switch_cycle(Leak *L) {
    if (!leak_keys(L, "C-x b * M e s RET") || leak_current(L) != L->app->messages) return 0;
    return leak_keys(L, "C-x b RET") && leak_current(L) == L->home;
}

// isearch over a buffer with a match on every line (the lazy highlight), then C-g.
static b32 leak_isearch_setup(Leak *L) {
    return app_dev_visit(L->app, str8_fmt(L->arena, "%S\\search.txt", L->dir));
}

static b32 leak_isearch_cycle(Leak *L) {
    return leak_keys(L, "C-s w o r d") && L->app->isearch.active && leak_keys(L, "C-g") && !L->app->isearch.active;
}

static void leak_kill_teardown(Leak *L) {
    leak_kill_current(L);
}

static b32 leak_query_replace_cycle(Leak *L) {
    if (!leak_find_file(L, "replace.txt")) return 0;
    if (!leak_keys(L, "M-% f o o RET b a r RET") || L->app->replace.state != REPLACE_ASKING) return 0;
    if (!leak_keys(L, "y q") || L->app->replace.state != REPLACE_OFF || !leak_current(L)->modified) return 0;
    if (!leak_keys(L, "C-/") || leak_current(L)->modified) return 0;
    return leak_kill_current(L) && leak_current(L) == L->home;
}

// mark-all-like-this on a word with 1,000 occurrences, a character typed at every cursor, undone, the cursors gone.
static b32 leak_cursors_cycle(Leak *L) {
    if (!leak_find_file(L, "cursors.txt")) return 0;
    if (!leak_keys(L, "M-< C-SPC M-f C-c C-<") || app_selected_view(L->app)->cursor_count != 1000) return 0;
    if (!leak_keys(L, "z") || !leak_current(L)->modified) return 0;
    if (!leak_keys(L, "C-/") || leak_current(L)->modified) return 0;
    if (!leak_keys(L, "C-g C-g") || app_selected_view(L->app)->cursor_count != 1) return 0;
    return leak_kill_current(L) && leak_current(L) == L->home;
}

// undo-cycle keeps its buffer across cycles: what it measures is the undo log at its cap (undo_limit_mb = 1 in
// leak.conf). LEAK_UNDO_INSERTS self-inserts (groups of 20), undone to the saved state, undone again with
// undo-redo, and undone to the saved state once more.
#define LEAK_UNDO_INSERTS 2000
#define LEAK_UNDO_GROUPS (LEAK_UNDO_INSERTS / 20)

static b32 leak_undo_setup(Leak *L) {
    if (!app_dev_visit(L->app, str8_fmt(L->arena, "%S\\undo.txt", L->dir))) return 0;
    L->undo_buffer = leak_current(L);
    return 1;
}

static b32 leak_repeat(Leak *L, const char *key, i32 count) {
    u64 mark = arena_pos(L->arena);
    String8 k = str8_cstr(key);
    u8 *keys = PUSH_ARRAY(L->arena, u8, (k.len + 1) * count);
    i64 n = 0;
    for (i32 i = 0; i < count; i++) {
        memcpy(keys + n, k.data, (size_t)k.len);
        n += k.len;
        keys[n++] = ' ';
    }
    leak_feed(L, str8(keys, n), 0); // one batch: the frames run as the queue fills
    arena_pop_to(L->arena, mark);
    return leak_settle(L);
}

static b32 leak_undo_cycle(Leak *L) {
    Buffer *b = L->undo_buffer;
    if (leak_current(L) != b || b->modified) return 0;
    i64 size = buffer_size(b);
    if (!leak_repeat(L, "x", LEAK_UNDO_INSERTS) || buffer_size(b) != size + LEAK_UNDO_INSERTS) return 0;
    if (!leak_repeat(L, "C-/", LEAK_UNDO_GROUPS) || b->modified || buffer_size(b) != size) return 0;
    if (!leak_repeat(L, "C-?", LEAK_UNDO_GROUPS) || buffer_size(b) != size + LEAK_UNDO_INSERTS) return 0;
    if (!leak_keys(L, "C-f C-b")) return 0; // another command between: the next undo starts a new chain
    return leak_repeat(L, "C-/", LEAK_UNDO_GROUPS) && !b->modified && buffer_size(b) == size;
}

static void leak_undo_teardown(Leak *L) {
    leak_kill_current(L);
    L->undo_buffer = NULL;
}

// The project scenarios start on p10k\README.txt: C-x p f, C-x p g and C-x p c act on p10k (10,000 files).
static b32 leak_project_setup(Leak *L) {
    if (!app_dev_visit(L->app, str8_fmt(L->arena, "%S\\p10k\\README.txt", L->dir))) return 0;
    return leak_keys(L, "C-x p f") && leak_keys(L, "C-g"); // the first scan, settled before the warm-up
}

static b32 leak_finder_open_cycle(Leak *L) {
    Buffer *readme = leak_current(L);
    if (!leak_keys(L, "C-x p f") || !leak_type(L, STR8_LIT("f0000")) || !leak_keys(L, "RET")) return 0;
    if (!str8_equal(leak_current(L)->name, STR8_LIT("f0000.c"))) return 0;
    return leak_kill_current(L) && leak_current(L) == readme;
}

static b32 leak_finder_cancel_cycle(Leak *L) {
    return leak_keys(L, "C-x p f") && leak_type(L, STR8_LIT("f0000")) && L->app->mini.active && leak_keys(L, "C-g") && !L->app->mini.active;
}

static b32 leak_scan_held(Leak *L) {
    for (i32 i = 0; i < PROJECT_KEPT; i++) if (L->app->finder.projects[i].scan) return 1;
    return 0;
}

// project-refresh twice while the scans stand still (scan hold): the first is cancelled and joined, the second
// released and finished.
static b32 leak_finder_rescan_cycle(Leak *L) {
    L->app->finder.dev_hold_after = 1;
    leak_feed(L, STR8_LIT("M-x p r o j e c t - r e f r e s h RET"), 1);
    if (!leak_wait(L, leak_scan_held, "the first held scan")) return 0;
    leak_feed(L, STR8_LIT("M-x p r o j e c t - r e f r e s h RET"), 1);
    if (!leak_wait(L, leak_scan_held, "the second held scan")) return 0;
    finder_dev_release(L->app);
    return leak_settle(L) && !leak_scan_held(L) && L->app->finder.dying_count == 0;
}

static b32 leak_grep_done_cycle(Leak *L) {
    if (!leak_keys(L, "C-x p g C-a C-k") || !leak_type(L, STR8_LIT("needle")) || !leak_keys(L, "RET")) return 0;
    Results *r = NULL;
    for (i32 i = 0; i < RESULTS_MAX; i++) if (L->app->results[i].buffer && L->app->results[i].kind == RESULTS_PROJECT) r = &L->app->results[i];
    if (!r || r->running || r->count != 1000) return 0;
    if (leak_current(L) != r->buffer) return 0; // project-search selects *search*
    return leak_keys(L, "q C-x 1") && window_count(&L->app->windows) == 1;
}

static b32 leak_search_running(Leak *L) {
    for (i32 i = 0; i < RESULTS_MAX; i++) if (L->app->results[i].buffer && L->app->results[i].running) return 1;
    return 0;
}

// The search's workers stand still (grep hold) while *search* is killed: they are cancelled and joined.
static b32 leak_grep_cancel_cycle(Leak *L) {
    L->app->dev_grep_hold_after = 1;
    leak_feed(L, STR8_LIT("C-x p g C-a C-k n e e d l e RET"), 1);
    if (!leak_wait(L, leak_search_running, "the held search")) return 0;
    leak_feed(L, STR8_LIT("C-x k RET"), 1); // *search* is selected
    L->app->dev_grep_hold_after = 0;
    return !leak_search_running(L) && leak_keys(L, "C-x 1") && window_count(&L->app->windows) == 1;
}

static b32 leak_compile(Leak *L, String8 command) {
    if (!leak_keys(L, "C-x p c C-a C-k") || !leak_type(L, command) || !leak_keys(L, "RET")) return 0;
    if (app_dev_compile_busy(L->app)) return 0; // settled: the build ended
    return leak_keys(L, "C-x o q C-x 1") && window_count(&L->app->windows) == 1;
}

static b32 leak_compile_echo_cycle(Leak *L) {
    return leak_compile(L, STR8_LIT("echo done"));
}

static b32 leak_compile_error_cycle(Leak *L) {
    return leak_compile(L, STR8_LIT("echo a.c(1,5): error C2065: 'x': undeclared identifier"));
}

// leak.conf as leak_prepare writes it, and the variant config-reload-cycle writes in between (another background).
#define LEAK_CONF_TEXT "[settings]\nundo_limit_mb = 1\nsearch_workers = 4\n"
#define LEAK_CONF_OTHER "[settings]\nundo_limit_mb = 1\nsearch_workers = 4\n[colors]\nbackground = #072627\n"
#define LEAK_BACKGROUND 0x072626u
#define LEAK_BACKGROUND_OTHER 0x072627u

static b32 leak_background_other(Leak *L) {
    return L->app->config->theme.background == LEAK_BACKGROUND_OTHER;
}

static b32 leak_background_back(Leak *L) {
    return L->app->config->theme.background == LEAK_BACKGROUND;
}

// The config file rewritten: the reload through its directory watch and the settle, then back.
static b32 leak_config_color_cycle(Leak *L) {
    String8 conf = str8_fmt(L->arena, "%S\\conf\\leak.conf", L->dir);
    if (!os_write_file(conf, STR8_LIT(LEAK_CONF_OTHER)) || !leak_wait(L, leak_background_other, "the reload with the other background")) return 0;
    if (!leak_settle(L) || !os_write_file(conf, STR8_LIT(LEAK_CONF_TEXT)) || !leak_wait(L, leak_background_back, "the reload back")) return 0;
    return 1;
}

// text-scale-increase and -decrease: the font set up again twice, the atlas made again.
static b32 leak_config_scale_cycle(Leak *L) {
    i32 setups = L->app->font->setup_count;
    return leak_keys(L, "C-x C-+ C-x C--") && L->app->font->setup_count == setups + 2;
}

// ext\ext.txt visited, rewritten outside twice (auto_revert reads it again through its directory's watch; each revert
// is one undo group, so the buffer goes at the end of the cycle), killed: the directory's watch comes and goes.
#define LEAK_EXT_TEXT "The file external-change-cycle rewrites.\n"
#define LEAK_EXT_CHANGED "Changed outside ronin, a different size.\n\n"

static b32 leak_text_is(Leak *L, String8 text) {
    Buffer *b = leak_current(L);
    if (buffer_size(b) != text.len) return 0;
    u64 mark = arena_pos(L->arena);
    b32 same = str8_equal(buffer_text(b, L->arena, 0, buffer_size(b)), text);
    arena_pop_to(L->arena, mark);
    return same;
}

static b32 leak_ext_changed_in(Leak *L) {
    return leak_text_is(L, STR8_LIT(LEAK_EXT_CHANGED));
}

static b32 leak_ext_back(Leak *L) {
    return leak_text_is(L, STR8_LIT(LEAK_EXT_TEXT));
}

static b32 leak_external_cycle(Leak *L) {
    if (!leak_find_file(L, "ext/ext.txt") || !str8_equal(leak_current(L)->name, STR8_LIT("ext.txt"))) return 0;
    String8 path = str8_fmt(L->arena, "%S\\ext\\ext.txt", L->dir);
    if (!os_write_file(path, STR8_LIT(LEAK_EXT_CHANGED)) || !leak_wait(L, leak_ext_changed_in, "the revert to the changed file")) return 0;
    if (!leak_settle(L) || !os_write_file(path, STR8_LIT(LEAK_EXT_TEXT)) || !leak_wait(L, leak_ext_back, "the revert back")) return 0;
    return !leak_current(L)->modified && leak_kill_current(L) && leak_current(L) == L->home;
}

static const LeakScenario leak_scenarios[] = {
    { "file-cycle", NULL, leak_file_cycle, NULL, 0, 0, 0 },
    { "file-cycle-100mb", NULL, leak_file_100mb_cycle, NULL, 1, 0, 0 },
    { "window-cycle/split", NULL, leak_window_split_cycle, NULL, 0, 1, 1 },
    { "window-cycle/one", NULL, leak_window_one_cycle, NULL, 0, 1, 1 },
    { "minibuffer-cycle/mx", NULL, leak_mx_cycle, NULL, 0, 0, 0 },
    { "minibuffer-cycle/find-file", NULL, leak_find_file_prompt_cycle, NULL, 0, 0, 0 },
    { "minibuffer-cycle/goto-line", NULL, leak_goto_line_cycle, NULL, 0, 0, 0 },
    { "minibuffer-cycle/switch", NULL, leak_switch_cycle, NULL, 0, 0, 0 },
    { "isearch-cycle/lazy", leak_isearch_setup, leak_isearch_cycle, leak_kill_teardown, 0, 0, 0 },
    { "isearch-cycle/query-replace", NULL, leak_query_replace_cycle, NULL, 0, 0, 0 },
    { "multi-cursor-cycle", NULL, leak_cursors_cycle, NULL, 0, 0, 0 },
    { "modal-cycle/browse", leak_modal_setup, leak_modal_browse_cycle, NULL, 0, 1, 1 },
    { "modal-cycle/run", NULL, leak_modal_run_cycle, NULL, 0, 0, 0 },
    { "git-cycle/browse", leak_git_setup, leak_git_browse_cycle, leak_git_teardown, 0, 1, 1 },
    { "git-cycle/cancel", leak_git_hang_setup, leak_git_cancel_cycle, leak_git_teardown, 0, 0, 1 },
    { "terminal-cycle", leak_terminal_setup, leak_terminal_cycle, leak_terminal_teardown, 0, 0, 0 },
    { "undo-cycle", leak_undo_setup, leak_undo_cycle, leak_undo_teardown, 0, 0, 1 },
    { "finder-cycle/open", leak_project_setup, leak_finder_open_cycle, leak_kill_teardown, 0, 0, 0 },
    { "finder-cycle/cancel", leak_project_setup, leak_finder_cancel_cycle, leak_kill_teardown, 0, 0, 0 },
    { "finder-cycle/rescan", leak_project_setup, leak_finder_rescan_cycle, leak_kill_teardown, 0, 0, 0 },
    { "grep-cycle/done", leak_project_setup, leak_grep_done_cycle, leak_kill_teardown, 0, 0, 0 },
    { "grep-cycle/cancel", leak_project_setup, leak_grep_cancel_cycle, leak_kill_teardown, 0, 0, 0 },
    { "compile-cycle/echo", leak_project_setup, leak_compile_echo_cycle, leak_kill_teardown, 0, 0, 0 },
    { "compile-cycle/error", leak_project_setup, leak_compile_error_cycle, leak_kill_teardown, 0, 0, 0 },
    { "config-reload-cycle/color", NULL, leak_config_color_cycle, NULL, 0, 1, 1 },
    { "config-reload-cycle/scale", NULL, leak_config_scale_cycle, NULL, 0, 1, 1 },
    { "external-change-cycle", NULL, leak_external_cycle, NULL, 0, 0, 0 },
};

// "all", a scenario's whole name, or the part before '/' (all its sub-cycles).
static b32 leak_selected(String8 which, const char *name) {
    String8 n = str8_cstr(name);
    if (str8_equal(which, STR8_LIT("all")) || str8_equal(which, n)) return 1;
    return which.len < n.len && n.data[which.len] == '/' && mem_equal(n.data, which.data, which.len);
}

b32 leak_scenario_known(String8 name) {
    for (i32 i = 0; i < ARRAY_COUNT(leak_scenarios); i++) if (leak_selected(name, leak_scenarios[i].name)) return 1;
    return 0;
}

// ---------------------------------------------------------------------------
// The run

static void leak_write_if_changed(Arena *a, String8 path, String8 text) {
    String8 old;
    if (os_read_small_file(a, path, MB(1), &old) == OS_FILE_OK && str8_equal(old, text)) return;
    os_write_file(path, text);
}

String8 leak_prepare(Arena *arena, String8 tmp_dir) {
    String8 dir = str8_fmt(arena, "%S\\leak", tmp_dir);
    os_make_dir(dir);
    os_make_dir(str8_fmt(arena, "%S\\conf", dir));
    leak_write_if_changed(arena, str8_fmt(arena, "%S\\home.txt", dir), STR8_LIT("The leak check's home buffer.\n"));
    leak_write_if_changed(arena, str8_fmt(arena, "%S\\sample.c", dir),
                          STR8_LIT("#include <stdio.h>\n\n// A sample for file-cycle.\nint main(void) {\n    int total = 0;\n"
                                   "    for (int i = 0; i < 10; i++) total += i * i;\n    printf(\"%d\\n\", total);\n    return 0;\n}\n"));
    String8 conf = str8_fmt(arena, "%S\\conf\\leak.conf", dir);
    leak_write_if_changed(arena, conf, STR8_LIT(LEAK_CONF_TEXT));
    os_make_dir(str8_fmt(arena, "%S\\ext", dir));
    leak_write_if_changed(arena, str8_fmt(arena, "%S\\ext\\ext.txt", dir), STR8_LIT(LEAK_EXT_TEXT));
    leak_write_if_changed(arena, str8_fmt(arena, "%S\\replace.txt", dir), STR8_LIT("foo one\nfoo two\nfoo three\n"));
    leak_write_if_changed(arena, str8_fmt(arena, "%S\\undo.txt", dir), STR8_LIT("undo-cycle types before this line\n"));
    // git-cycle's fake gits: git (two changed files, their diffs) and git_hang (its status never answers).
    String8 fwd = str8_copy(arena, dir);
    for (i64 i = 0; i < fwd.len; i++) if (fwd.data[i] == '\\') fwd.data[i] = '/';
    static const char *const fakes[] = { "git", "git_hang" };
    for (i32 k = 0; k < 2; k++) {
        String8 g = str8_fmt(arena, "%S\\%s", dir, fakes[k]);
        os_make_dir(g);
        leak_write_if_changed(arena, str8_fmt(arena, "%S\\a.c", g), STR8_LIT("int a;\n"));
        leak_write_if_changed(arena, str8_fmt(arena, "%S\\b.c", g), STR8_LIT("int b;\n"));
        leak_write_if_changed(arena, str8_fmt(arena, "%S\\rev.txt", g), str8_fmt(arena, "out %S/%s\n", fwd, fakes[k]));
        leak_write_if_changed(arena, str8_fmt(arena, "%S\\status.txt", g),
                              k ? STR8_LIT("hang\n") : STR8_LIT("raw ## main\\x00 M a.c\\x00 M b.c\\x00\n"));
        leak_write_if_changed(arena, str8_fmt(arena, "%S\\a.txt", g),
                              STR8_LIT("raw diff --git a/a.c b/a.c\\n--- a/a.c\\n+++ b/a.c\\n@@ -1 +1 @@\\n-int A;\\n+int a;\\n\n"));
        leak_write_if_changed(arena, str8_fmt(arena, "%S\\b.txt", g),
                              STR8_LIT("raw diff --git a/b.c b/b.c\\n--- a/b.c\\n+++ b/b.c\\n@@ -1 +1 @@\\n-int B;\\n+int b;\\n\n"));
        leak_write_if_changed(arena, str8_fmt(arena, "%S\\map.txt", g),
                              STR8_LIT("rev-parse\trev.txt\nstatus\tstatus.txt\n-- a.c\ta.txt\n-- b.c\tb.txt\n"));
    }
    // search.txt: "word" on each of 2,000 lines; cursors.txt: "needle" 1,000 times, once a line.
    u64 mark = arena_pos(arena);
    i64 cap = KB(128), n = 0;
    u8 *text = PUSH_ARRAY(arena, u8, cap);
    for (i32 i = 0; i < 2000; i++) n += fmt_buf(text + n, cap - n, "line %d has the word in it\n", i);
    leak_write_if_changed(arena, str8_fmt(arena, "%S\\search.txt", dir), str8(text, n));
    n = 0;
    for (i32 i = 0; i < 1000; i++) n += fmt_buf(text + n, cap - n, "needle %d\n", i);
    leak_write_if_changed(arena, str8_fmt(arena, "%S\\cursors.txt", dir), str8(text, n));
    // p10k: a project of 10,000 small C files in 100 directories, "needle" in every tenth; made once.
    String8 project = str8_fmt(arena, "%S\\p10k", dir);
    OsFileInfo info;
    if (os_file_info(str8_fmt(arena, "%S\\d99\\f9999.c", project), &info) != OS_FILE_OK) {
        os_make_dir(project);
        os_write_file(str8_fmt(arena, "%S\\.ronin-project", project), STR8_LIT("# the leak check's project\n"));
        os_write_file(str8_fmt(arena, "%S\\README.txt", project), STR8_LIT("The leak check's project: 10,000 files.\n"));
        for (i32 d = 0; d < 100; d++) {
            u64 dmark = arena_pos(arena);
            String8 sub = str8_fmt(arena, "%S\\d%02d", project, d);
            os_make_dir(sub);
            for (i32 f = d * 100; f < d * 100 + 100; f++) {
                String8 body = str8_fmt(arena, "%sint f%04d(void) {\n    return %d;\n}\n", f % 10 == 0 ? "// needle\n" : "", f, f);
                os_write_file(str8_fmt(arena, "%S\\f%04d.c", sub, f), body);
            }
            arena_pop_to(arena, dmark);
        }
    }
    arena_pop_to(arena, mark);
    test_bench_buffer_file(arena, tmp_dir); // the 100 MB file, made once
    return conf;
}

// The bounded caches at their caps, so that the windows measure the operations themselves: *Messages* full of
// full-length lines, every history kind full and its arena past a compaction, the kill ring full.
static void leak_prime(Leak *L) {
    App *app = L->app;
    u8 line[ECHO_CAP];
    memset(line, 'p', sizeof(line));
    for (i32 i = 0; i < ECHO_LOG_LINES; i++) echo_log(&app->echo, str8(line, ECHO_CAP));
    Minibuffer *mb = &app->mini;
    for (i32 k = MINI_HISTORY_NONE + 1; k < MINI_HISTORY_COUNT; k++) {
        for (i32 i = 0; i < MINI_HISTORY_MAX; i++) {
            u8 text[64];
            memset(text, '.', sizeof(text));
            fmt_buf(text, sizeof(text), "prime %d %d", k, i);
            minibuffer_history_add(mb, (MiniHistoryKind)k, str8(text, sizeof(text)));
        }
    }
    i32 compactions = 0;
    u64 last = mb->history_arena.pos;
    for (i32 i = 0; compactions < 2 && i < 100000; i++) {
        u8 text[64];
        memset(text, i & 1 ? 'a' : 'b', sizeof(text));
        minibuffer_history_add(mb, MINI_HISTORY_TEXT, str8(text, sizeof(text)));
        if (mb->history_arena.pos < last) compactions++;
        last = mb->history_arena.pos;
    }
    for (i32 i = 0; i < app->config->settings.kill_ring_max; i++) {
        u8 *d = kill_push(&app->kills, 64);
        if (d) memset(d, 'k', 64);
    }
    LOG("leak: primed: *Messages* %D bytes, history arena %U bytes after %d compactions, kill ring %d entries",
        buffer_size(app->messages), mb->history_arena.pos, compactions, app->config->settings.kill_ring_max);
}

static void leak_run(Leak *L, const LeakScenario *s, i32 cycles) {
    u64 t0 = os_time_us();
    i32 warm = s->five ? 1 : LEAK_WARMUP, per = s->five ? 1 : cycles / LEAK_WINDOWS;
    i32 failed = L->failed;
    LOG("leak: %s: %d cycles (%d warm-up, %d windows of %d)", s->name, warm + LEAK_WINDOWS * per, warm, LEAK_WINDOWS, per);
    if (s->setup && !s->setup(L)) {
        leak_fail(L, s->name, STR8_LIT("setup failed"));
        return;
    }
    Event wakeup = { .kind = EVENT_WAKEUP }; // a frame: the layout and the watches of what the setup shows
    L->host->feed(L->host->ctx, &wakeup, 1, 1);
    if (!leak_settle(L)) return;
    u64 base = arena_pos(L->arena);
    L->res_first = PUSH_ARRAY(L->arena, OsDevReservation, 16384);
    L->res_last = PUSH_ARRAY(L->arena, OsDevReservation, 16384);
    L->res_first_count = L->res_last_count = 0;
    i32 bw = 0, bh = 0, aw = 0, ah = 0;
    u8 *before = s->shots ? leak_shot(L, L->arena, &bw, &bh) : NULL;
    String8 state0 = s->from_start ? leak_state(L, L->arena) : str8(NULL, 0);
    i64 v[LEAK_WINDOWS + 1][LEAK_M_COUNT], caps[LEAK_M_COUNT] = { 0 };
    i32 total = warm + LEAK_WINDOWS * per, first_trim = 0;
    for (i32 c = 0; c < total && L->failed == failed; c++) {
        b32 ok = s->cycle(L) && leak_settle(L);
        if (!ok) {
            leak_fail(L, s->name, str8_fmt(L->arena, "cycle %d: a step did not do what it should (the buffer %S, the echo '%S')", c + 1,
                                           leak_current(L)->name, str8(L->app->echo.text, L->app->echo.len)));
            break;
        }
        u64 mark = arena_pos(L->arena);
        String8 state = leak_state(L, L->arena);
        if (!state0.len) {
            state0 = state; // after the first cycle: what every later one must leave
            mark = arena_pos(L->arena);
        } else if (!str8_equal(state, state0)) {
            leak_fail(L, s->name, str8_fmt(L->arena, "cycle %d left another state:\n  %S\nthan the first:\n  %S", c + 1, state, state0));
            break;
        }
        arena_pop_to(L->arena, mark);
        i32 done = c + 1;
        if (L->undo_buffer && !first_trim && buffer_dev_undo_trims(L->undo_buffer)) {
            first_trim = done;
            LOG("leak: %s: the undo log reached its cap in cycle %d (%U bytes of records now)", s->name, done, buffer_dev_undo_used(L->undo_buffer));
        }
        if (done == warm && L->undo_buffer && !first_trim) { // growth inside the windows would be the cap being reached
            leak_fail(L, s->name, STR8_LIT("warm-up did not reach undo cap"));
            break;
        }
        if (done == warm) leak_sample(L, 0, v[0], caps);
        else if (done > warm && (done - warm) % per == 0) leak_sample(L, (done - warm) / per, v[(done - warm) / per], caps);
    }
    if (L->failed == failed) {
        u8 *after = s->shots ? leak_shot(L, L->arena, &aw, &ah) : NULL;
        if (s->shots) leak_compare_shots(L, s->name, before, bw, bh, after, aw, ah);
    }
    if (s->teardown) s->teardown(L);
    // Every scenario leaves the next one home.txt in one window (a teardown that kills a buffer shows another one).
    if (window_count(&L->app->windows) > 1) leak_keys(L, "C-x 1");
    if (leak_current(L) != L->home) app_switch_buffer(L->app, app_selected_view(L->app), L->home);
    leak_settle(L);
    if (L->failed == failed) {
        i32 leaks = leak_verdict(L, s->name, v, caps, per);
        L->leaks += leaks;
        LOG("leak: %s: %s, %U ms", s->name, leaks ? "LEAK" : "OK", (os_time_us() - t0) / 1000);
    }
    arena_pop_to(L->arena, base);
}

i32 leak_check(App *app, LeakHost *host, String8 which, i32 cycles, String8 tmp_dir, Arena *arena) {
    u64 t0 = os_time_us();
    Leak L = { .app = app, .host = host, .arena = arena };
    L.dir = os_full_path(arena, str8_fmt(arena, "%S\\leak", tmp_dir));
    L.dir_fwd = str8_copy(arena, L.dir);
    for (i64 i = 0; i < L.dir_fwd.len; i++) if (L.dir_fwd.data[i] == '\\') L.dir_fwd.data[i] = '/';
    L.bench_file = str8_fmt(arena, "%S\\bench_100mb.txt", tmp_dir);
    L.shots_dir = os_full_path(arena, str8_fmt(arena, "%S\\..\\shots", tmp_dir));
    app_dev_force_focus(app, 1);
    Event focus = { .kind = EVENT_FOCUS, .focused = 1 }; // as a focused editor: changes on disk are acted on through the watches
    host->feed(host->ctx, &focus, 1, 1);
    if (!app_dev_visit(app, str8_fmt(arena, "%S\\home.txt", L.dir))) {
        LOG("leak: FAIL: cannot visit home.txt in %S", L.dir);
        return 9;
    }
    L.home = leak_current(&L);
    leak_prime(&L);
    leak_settle(&L);
    i32 ran = 0;
    for (i32 i = 0; i < ARRAY_COUNT(leak_scenarios) && !L.failed; i++) {
        if (!leak_selected(which, leak_scenarios[i].name)) continue;
        leak_run(&L, &leak_scenarios[i], cycles);
        ran++;
    }
    if (os_dev_real_refused()) leak_fail(&L, "a scenario", str8_fmt(arena, "reached a real terminal or git %d time(s)", os_dev_real_refused()));
    i32 code = L.failed ? 9 : L.leaks ? 6 : L.mismatches ? 3 : 0;
    LOG("leak: %s: %d scenario(s), %d LEAK, %d failure(s), %d screenshot mismatch(es), %U s; WATCH and STEP: %s", code ? "FAIL" : "PASS", ran, L.leaks,
        L.failed, L.mismatches, (os_time_us() - t0) / 1000000, L.watch_len ? L.watch : "none");
    return code;
}

#endif
