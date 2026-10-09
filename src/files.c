// files.c — the file and buffer commands built on prompts: finding, writing and saving files,
// switching and killing buffers. Part of the app (included after app.c): it uses App directly.

// ---------------------------------------------------------------------------
// Paths. Shown with forward slashes, as Emacs does on Windows; both separators are accepted.

static b32 files_is_slash(u8 c) {
    return c == '/' || c == '\\';
}

// A message that may hold paths: shown with forward slashes.
static void files_echo(Echo *echo, const char *fmt, ...) {
    u8 text[ECHO_CAP];
    va_list args;
    va_start(args, fmt);
    i64 n = MIN(fmt_v(text, sizeof(text), fmt, args), (i64)sizeof(text));
    va_end(args);
    for (i64 i = 0; i < n; i++) if (text[i] == '\\') text[i] = '/';
    echo_message(echo, "%S", str8(text, n));
}

// The path an input names: what follows the last "/~/" or "/X:/" in it (Emacs' shadowed prefix:
// typing ~/ or c:/ after a directory starts over), with a leading "~" meaning the profile directory.
static String8 files_resolve(Arena *arena, String8 input) {
    i64 start = 0;
    for (i64 i = 1; i < input.len; i++) {
        if (!files_is_slash(input.data[i - 1])) continue;
        u8 c = input.data[i], lower = c | 0x20;
        b32 end = i + 1 == input.len || files_is_slash(input.data[i + 1]);
        if (c == '~' && end) start = i;
        else if (lower >= 'a' && lower <= 'z' && i + 1 < input.len && input.data[i + 1] == ':' &&
                 (i + 2 == input.len || files_is_slash(input.data[i + 2]))) start = i;
    }
    String8 s = str8(input.data + start, input.len - start);
    if (s.len && s.data[0] == '~' && (s.len == 1 || files_is_slash(s.data[1]))) {
        s = str8_fmt(arena, "%S%S", os_get_env(arena, STR8_LIT("USERPROFILE")), str8(s.data + 1, s.len - 1));
    }
    return s;
}

// The directory of a buffer's file (else the current directory), with forward slashes and a trailing
// slash: the initial input of find-file and write-file.
static String8 files_default_dir(Arena *arena, Buffer *buf) {
    String8 dir = buf->path.len ? str8(buf->path.data, buf->path.len - config_file_name(buf->path).len) : os_full_path(arena, STR8_LIT("."));
    String8 s = str8_fmt(arena, "%S/", dir);
    for (i64 i = 0; i < s.len; i++) if (s.data[i] == '\\') s.data[i] = '/';
    if (s.len >= 2 && s.data[s.len - 2] == '/') s.len--; // it had one already
    return s;
}

// MINI_CHOICE candidates for a file name: the entries of the directory part of the input (listed again
// only when it changes), directories first. Matching uses the part after the last slash.
static i64 files_file_candidates(Minibuffer *mb, void *data, String8 input) {
    App *app = data;
    i64 slash = input.len;
    while (slash > 0 && !files_is_slash(input.data[slash - 1])) slash--;
    arena_reset(&app->files_arena);
    String8 dir = files_resolve(&app->files_arena, str8(input.data, slash));
    String8 full = os_full_path(&app->files_arena, dir.len ? dir : STR8_LIT("."));
    if (mb->cand_count || mb->cand_key.len) {
        if (str8_equal(full, mb->cand_key)) return slash;
    }
    minibuffer_clear_candidates(mb);
    OsDirEntry *entries;
    i64 count;
    if (full.len && os_list_dir(&app->files_arena, full, &entries, &count) == OS_FILE_OK) {
        for (i32 pass = 1; pass >= 0; pass--) { // directories, then files; each in the file system's order
            for (i64 i = 0; i < count; i++) {
                if (entries[i].is_dir == (b32)pass) minibuffer_add_candidate(mb, entries[i].name, str8(NULL, 0), pass ? CANDIDATE_DIR : 0);
            }
        }
    }
    mb->cand_key = str8_copy(&mb->text_arena, full.len ? full : STR8_LIT("?"));
    return slash;
}

// ---------------------------------------------------------------------------
// Finding and writing files

static void files_find_done(CommandContext *ctx, MiniResult *r) {
    App *app = ctx->app;
    arena_reset(&app->files_arena);
    String8 path = files_resolve(&app->files_arena, r->text);
    if (!path.len) return;
    OsFileInfo info;
    if (os_file_info(path, &info) == OS_FILE_OK && info.is_dir) {
        files_echo(ctx->echo, "%S is a directory", path);
        return;
    }
    Buffer *buf = app_find_file(app, path);
    if (!buf) return;
    app_switch_buffer(app, ctx->view, buf);
    ctx->cursor = &ctx->view->cursors[0];
}

static void cmd_find_file(CommandContext *ctx) {
    App *app = ctx->app;
    arena_reset(&app->files_arena);
    MiniRequest req = { .kind = MINI_CHOICE, .prompt = STR8_LIT("Find file: "), .initial = files_default_dir(&app->files_arena, ctx->view->buffer),
                        .history = MINI_HISTORY_FILE, .done = files_find_done, .candidates = files_file_candidates, .data = app, .file = 1 };
    minibuffer_read(ctx, &req);
}

// Saves the buffer under `path` (it then visits that file), with the message.
static void files_write(CommandContext *ctx, String8 path) {
    App *app = ctx->app;
    Buffer *buf = ctx->view->buffer;
    OsFileStatus status = buffer_save_as_opt(buf, path, ctx->settings->fsync_on_save);
    if (status != OS_FILE_OK) {
        files_echo(ctx->echo, "Cannot write %S: %s", path, buffer_status_text(status));
        return;
    }
    app_uniquify(app, buf);
    files_echo(ctx->echo, "Wrote %S", buf->path);
}

static void files_write_confirmed(CommandContext *ctx, MiniResult *r) {
    if (r->key == 'y') files_write(ctx, ctx->mini->state.text);
}

// A directory means a file named like the buffer in it; an existing file asks first.
static void files_write_done(CommandContext *ctx, MiniResult *r) {
    App *app = ctx->app;
    arena_reset(&app->files_arena);
    String8 path = files_resolve(&app->files_arena, r->text);
    if (!path.len) return;
    OsFileInfo info;
    OsFileStatus status = os_file_info(path, &info);
    if ((status == OS_FILE_OK && info.is_dir) || files_is_slash(path.data[path.len - 1])) {
        b32 slash = files_is_slash(path.data[path.len - 1]);
        path = str8_fmt(&app->files_arena, slash ? "%S%S" : "%S/%S", path, ctx->view->buffer->name);
        status = os_file_info(path, &info);
    }
    if (status == OS_FILE_OK) {
        MiniRequest req = { .kind = MINI_KEY, .prompt = STR8_LIT("File exists; overwrite? (y or n) "), .answers = "yn",
                            .done = files_write_confirmed };
        if (minibuffer_read(ctx, &req)) ctx->mini->state.text = str8_copy(&ctx->mini->arena, path);
        return;
    }
    files_write(ctx, path);
}

static void cmd_write_file(CommandContext *ctx) {
    App *app = ctx->app;
    arena_reset(&app->files_arena);
    MiniRequest req = { .kind = MINI_CHOICE, .prompt = STR8_LIT("Write file: "), .initial = files_default_dir(&app->files_arena, ctx->view->buffer),
                        .history = MINI_HISTORY_FILE, .done = files_write_done, .candidates = files_file_candidates, .data = app, .file = 1 };
    minibuffer_read(ctx, &req);
}

static void files_save(CommandContext *ctx, Buffer *buf) {
    OsFileStatus status = buffer_save_opt(buf, ctx->settings->fsync_on_save);
    if (status == OS_FILE_OK) files_echo(ctx->echo, "Wrote %S", buf->path);
    else echo_message(ctx->echo, "Cannot save %S: %s", buf->name, buffer_status_text(status));
}

// The file on disk is not the one last read or saved: it exists and its size or time differ.
static b32 files_changed_on_disk(Buffer *buf) {
    OsFileInfo info;
    return buf->path.len && os_file_info(buf->path, &info) == OS_FILE_OK && !info.is_dir &&
           (info.size != buf->file_size || info.write_time != buf->file_time);
}

static void files_save_some_next(CommandContext *ctx);

enum {
    FILES_SAVE_ALL  = 1 << 0, // "!": save the rest without asking
    FILES_QUIT      = 1 << 1, // save-buffers-kill-terminal: then the exit question
    FILES_ASKED     = 1 << 2, // some buffer needed saving
    FILES_WALK      = 1 << 3, // a save-some-buffers walk: it goes on after the guard's answer
};

static void files_guard_answer(CommandContext *ctx, MiniResult *r) {
    MiniState *s = &ctx->mini->state;
    if (r->yes) files_save(ctx, s->buffer);
    if (s->flags & FILES_WALK) {
        s->index++;
        files_save_some_next(ctx);
    }
}

// Saves, or first asks when the file changed on disk since it was read or saved. True when it
// asked (the answer continues a save-some-buffers walk).
static b32 files_save_checked(CommandContext *ctx, Buffer *buf, b32 walk) {
    if (!files_changed_on_disk(buf)) {
        files_save(ctx, buf);
        return 0;
    }
    u8 prompt[512];
    i64 n = fmt_buf(prompt, sizeof(prompt), "%S has changed since visited or saved. Save anyway? (yes or no) ", buf->name);
    MiniRequest req = { .kind = MINI_YES_NO, .prompt = str8(prompt, n), .done = files_guard_answer };
    if (!minibuffer_read(ctx, &req)) return 1;
    if (!walk) ctx->mini->state = (MiniState){ 0 };
    ctx->mini->state.buffer = buf;
    return 1;
}

// A buffer without a file asks for one (write-file). Headless view tests have no minibuffer.
static void cmd_save_buffer(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    if (!buf->path.len && ctx->mini && ctx->app) {
        cmd_write_file(ctx);
        return;
    }
    if (!buf->modified) {
        echo_message(ctx->echo, "(No changes need to be saved)");
        return;
    }
    if (ctx->mini && ctx->app) files_save_checked(ctx, buf, 0);
    else files_save(ctx, buf);
}

// ---------------------------------------------------------------------------
// Reverting

// Rereads the buffer's file through buffer_replace (buffer_revert): markers, and so every point,
// follow the edit; it can be undone.
static void files_revert(CommandContext *ctx, Buffer *buf) {
    i64 point = ctx->view->buffer == buf ? view_point(ctx->view, &ctx->view->cursors[0]) : 0;
    OsFileStatus status = buffer_revert(buf, point);
    if (status == OS_FILE_OK) echo_message(ctx->echo, "Reverted %S", buf->name);
    else files_echo(ctx->echo, "Cannot revert %S: %s", buf->path, buffer_status_text(status));
}

static void files_revert_confirmed(CommandContext *ctx, MiniResult *r) {
    if (r->yes) files_revert(ctx, ctx->view->buffer);
}

// A modified buffer asks first.
static void cmd_revert_buffer(CommandContext *ctx) {
    Buffer *buf = ctx->view->buffer;
    if (!buf->path.len) {
        echo_message(ctx->echo, "Buffer does not seem to be associated with any file");
        return;
    }
    if (!buf->modified) {
        files_revert(ctx, buf);
        return;
    }
    arena_reset(&ctx->app->files_arena);
    String8 prompt = str8_fmt(&ctx->app->files_arena, "Discard edits and reread from %S? (yes or no) ", buf->path);
    for (i64 i = 0; i < prompt.len; i++) if (prompt.data[i] == '\\') prompt.data[i] = '/';
    MiniRequest req = { .kind = MINI_YES_NO, .prompt = prompt, .done = files_revert_confirmed };
    minibuffer_read(ctx, &req);
}

const Command CMD_REVERT_BUFFER = { "revert-buffer", cmd_revert_buffer, COMMAND_ONCE };

// ---------------------------------------------------------------------------
// save-some-buffers and quitting. One chain walks the buffer list: state.index is the next entry to
// look at, state.flags what was asked for.

static b32 files_unsaved(Buffer *b) {
    return b->modified && b->path.len;
}

static void files_quit_confirmed(CommandContext *ctx, MiniResult *r) {
    if (r->yes) ctx->app->quit = 1;
}

// The end of the walk when quitting: modified file buffers left over ask once more.
static void files_quit_check(CommandContext *ctx) {
    App *app = ctx->app;
    for (i32 i = 0; i < app->buffers.count; i++) {
        if (!files_unsaved(app->buffers.entries[i].buffer)) continue;
        MiniRequest req = { .kind = MINI_YES_NO, .prompt = STR8_LIT("Modified buffers exist; exit anyway? (yes or no) "),
                            .done = files_quit_confirmed };
        minibuffer_read(ctx, &req);
        return;
    }
    app->quit = 1;
}

static void files_save_some_answer(CommandContext *ctx, MiniResult *r);

// ---------------------------------------------------------------------------
// Files changed outside the editor. Checked when the window is activated (every file buffer), and
// while it has focus through directory watches on the displayed buffers' directories (a change
// notification is acted on CONFIG_SETTLE_MS after the last one; a reload that meets a sharing
// violation is tried again every CONFIG_RETRY_MS). An unmodified buffer is reloaded (auto_revert);
// a modified one is never touched, only flagged and reported once.

// Where undoing a reload puts point: the first view showing the buffer, else where it was last shown.
static i64 files_point_of(App *app, Buffer *buf) {
    View *views[WINDOW_MAX];
    for (i32 i = 0, n = app_views(app, views); i < n; i++) {
        if (views[i]->buffer == buf) return view_point(views[i], &views[i]->cursors[0]);
    }
    i32 e = buffer_list_index(&app->buffers, buf);
    return e >= 0 ? buffer_marker_get(buf, app->buffers.entries[e].point) : 0;
}

// True when a reload met a sharing violation (try again later).
static b32 files_check_buffer(App *app, Buffer *buf) {
    if (!buf->path.len) return 0;
    OsFileInfo info;
    OsFileStatus status = os_file_info(buf->path, &info);
    if (status == OS_FILE_NOT_FOUND) {
        if (buf->disk_state != BUFFER_DISK_DELETED) echo_message(&app->echo, "%S deleted on disk", buf->name);
        buf->disk_state = BUFFER_DISK_DELETED;
        return 0;
    }
    if (status != OS_FILE_OK || info.is_dir) return 0;
    if (info.size == buf->file_size && info.write_time == buf->file_time) {
        buf->disk_state = BUFFER_DISK_OK; // as read or saved (also: back after a deletion)
        return 0;
    }
    if (!buf->modified && app->config->settings.auto_revert) {
        status = buffer_revert(buf, files_point_of(app, buf));
        if (status == OS_FILE_OK) {
            echo_message(&app->echo, "Reverted %S", buf->name);
            return 0;
        }
        if (status == OS_FILE_SHARING_VIOLATION) return 1;
    }
    if (buf->disk_state != BUFFER_DISK_CHANGED) echo_message(&app->echo, "%S changed on disk", buf->name);
    buf->disk_state = BUFFER_DISK_CHANGED;
    return 0;
}

static void files_check_all(App *app) {
    for (i32 i = 0; i < app->buffers.count; i++) files_check_buffer(app, app->buffers.entries[i].buffer);
}

static void files_notify(App *app, OsWatch watch) {
    for (i32 i = 0; i < app->watch_count; i++) {
        if (app->watches[i].watch != watch || !watch) continue;
        app->disk_pending = 1;
        app->disk_due_us = os_time_us() + CONFIG_SETTLE_MS * 1000ull;
        app->disk_attempts = 0;
    }
}

// The displayed buffers, once a notification has settled.
static void files_poll(App *app) {
    u64 now = os_time_us();
    if (!app->disk_pending || now < app->disk_due_us) return;
    app->disk_pending = 0;
    b32 retry = 0;
    View *views[WINDOW_MAX];
    for (i32 i = 0, n = app_views(app, views); i < n; i++) {
        b32 seen = 0;
        for (i32 k = 0; k < i; k++) seen |= views[k]->buffer == views[i]->buffer;
        if (!seen) retry |= files_check_buffer(app, views[i]->buffer);
    }
    if (retry && ++app->disk_attempts < CONFIG_RETRY_ATTEMPTS) {
        app->disk_pending = 1;
        app->disk_due_us = now + CONFIG_RETRY_MS * 1000ull;
    }
}

static u32 files_wait_ms(App *app) {
    if (!app->disk_pending) return CONFIG_WAIT_INFINITE;
    u64 now = os_time_us();
    return app->disk_due_us <= now ? 0 : (u32)((app->disk_due_us - now + 999) / 1000);
}

static void files_unwatch_all(App *app) {
    for (i32 i = 0; i < app->watch_count; i++) os_unwatch(app->watches[i].watch);
    app->watch_count = 0;
}

// One watch per distinct directory of the displayed file buffers; changed only when that set changes.
static void files_update_watches(App *app) {
    String8 dirs[WINDOW_MAX];
    i32 n = 0;
    View *views[WINDOW_MAX];
    for (i32 i = 0, count = app_views(app, views); i < count; i++) {
        Buffer *b = views[i]->buffer;
        if (!b->path.len) continue;
        String8 dir = str8(b->path.data, b->path.len - config_file_name(b->path).len);
        if (dir.len > 1 && files_is_slash(dir.data[dir.len - 1]) && dir.data[dir.len - 2] != ':') dir.len--; // not "C:\"
        b32 seen = 0;
        for (i32 k = 0; k < n; k++) seen |= str8_equal(dirs[k], dir);
        if (!seen) dirs[n++] = dir;
    }
    b32 same = n == app->watch_count;
    for (i32 k = 0; k < n && same; k++) {
        b32 found = 0;
        for (i32 i = 0; i < app->watch_count; i++) found |= str8_equal(app->watches[i].dir, dirs[k]);
        same = found;
    }
    if (same) return;
    AppWatch next[WINDOW_MAX];
    for (i32 k = 0; k < n; k++) {
        next[k] = (AppWatch){ dirs[k], 0 };
        for (i32 i = 0; i < app->watch_count; i++) {
            if (!str8_equal(app->watches[i].dir, dirs[k])) continue;
            next[k].watch = app->watches[i].watch; // kept
            app->watches[i].watch = 0;
        }
        if (!next[k].watch) next[k].watch = os_watch_dir(dirs[k]);
    }
    files_unwatch_all(app); // the ones no longer displayed (kept ones were taken out)
    arena_reset(&app->watch_arena);
    for (i32 k = 0; k < n; k++) {
        app->watches[k] = next[k];
        app->watches[k].dir = str8_copy(&app->watch_arena, dirs[k]);
    }
    app->watch_count = n;
}

// Asks about the next modified file buffer (or saves it, after "!"); at the end, the exit question
// when quitting.
static void files_save_some_next(CommandContext *ctx) {
    App *app = ctx->app;
    MiniState *s = &ctx->mini->state;
    for (; s->index < app->buffers.count; s->index++) {
        Buffer *b = app->buffers.entries[s->index].buffer;
        if (!files_unsaved(b)) continue;
        s->flags |= FILES_ASKED;
        if (s->flags & FILES_SAVE_ALL) {
            if (files_save_checked(ctx, b, 1)) return;
            continue;
        }
        arena_reset(&app->files_arena);
        String8 prompt = str8_fmt(&app->files_arena, "Save file %S? (y, n, !, q) ", b->path);
        for (i64 i = 0; i < prompt.len; i++) if (prompt.data[i] == '\\') prompt.data[i] = '/';
        MiniRequest req = { .kind = MINI_KEY, .prompt = prompt, .answers = "yn!q", .done = files_save_some_answer };
        if (minibuffer_read(ctx, &req)) s->buffer = b;
        return;
    }
    if (s->flags & FILES_QUIT) files_quit_check(ctx);
    else if (!(s->flags & FILES_ASKED)) echo_message(ctx->echo, "(No files need saving)");
}

static void files_save_some_answer(CommandContext *ctx, MiniResult *r) {
    MiniState *s = &ctx->mini->state;
    if (r->key == 'q') {
        if (s->flags & FILES_QUIT) files_quit_check(ctx);
        return;
    }
    if (r->key == '!') s->flags |= FILES_SAVE_ALL;
    if (r->key != 'n' && files_save_checked(ctx, s->buffer, 1)) return;
    s->index++;
    files_save_some_next(ctx);
}

static void files_save_some_start(CommandContext *ctx, u32 flags) {
    if (ctx->mini->active) { // refused with Emacs' message: no recursive minibuffers
        minibuffer_read(ctx, &(MiniRequest){ .kind = MINI_TEXT });
        return;
    }
    ctx->mini->state = (MiniState){ .flags = flags | FILES_WALK };
    files_save_some_next(ctx);
}

static void cmd_save_some_buffers(CommandContext *ctx) { files_save_some_start(ctx, 0); }

// Windows asked to end the session while files were unsaved: the quit chain, as the close button,
// unless that chain is already asking (Windows may ask again).
static void files_end_session(App *app) {
    if (app->mini.active && (app->mini.state.flags & FILES_QUIT)) return;
    minibuffer_abort(&app->mini);
    app_run_command(app, &CMD_SAVE_BUFFERS_KILL_TERMINAL, 0, 0);
}

// Tells the platform when "some file buffer is unsaved" changes.
static void files_report_unsaved(App *app) {
    b32 any = 0;
    for (i32 i = 0; i < app->buffers.count && !any; i++) any = files_unsaved(app->buffers.entries[i].buffer);
    if (any == app->unsaved_reported) return;
    app->unsaved_reported = any;
    os_set_unsaved_files(any);
}

// C-x C-c, the close button, Alt+F4 and the end of the Windows session: save-some-buffers, then
// "Modified buffers exist; exit anyway?" if some are left.
static void cmd_save_buffers_kill_terminal(CommandContext *ctx) { files_save_some_start(ctx, FILES_QUIT); }

const Command CMD_SAVE_BUFFER = { "save-buffer", cmd_save_buffer, COMMAND_ONCE };
const Command CMD_SAVE_SOME_BUFFERS = { "save-some-buffers", cmd_save_some_buffers, COMMAND_ONCE };
const Command CMD_SAVE_BUFFERS_KILL_TERMINAL = { "save-buffers-kill-terminal", cmd_save_buffers_kill_terminal, COMMAND_ONCE };
const Command CMD_FIND_FILE   = { "find-file", cmd_find_file, COMMAND_ONCE };
const Command CMD_WRITE_FILE  = { "write-file", cmd_write_file, COMMAND_ONCE };

// ---------------------------------------------------------------------------
// Buffers

static u64 files_last_shown(App *app, Buffer *buf) {
    i32 i = buffer_list_index(&app->buffers, buf);
    return i >= 0 ? app->buffers.entries[i].last_shown : 0;
}

// Every listed buffer, most recently shown first (never shown ones in list order); `current` first
// (kill-buffer: the default) or last (switch-to-buffer: the default is the most recent other one).
static void files_add_buffers(Minibuffer *mb, App *app, Buffer *current, b32 current_first) {
    i32 n = app->buffers.count;
    u64 mark = arena_pos(&mb->match_arena);
    BufferEntry **order = PUSH_ARRAY(&mb->match_arena, BufferEntry *, n);
    for (i32 i = 0; i < n; i++) { // insertion sort, stable: buffers are few
        BufferEntry *e = &app->buffers.entries[i];
        i32 j = i;
        for (; j > 0 && order[j - 1]->last_shown < e->last_shown; j--) order[j] = order[j - 1];
        order[j] = e;
    }
    if (current_first) minibuffer_add_candidate(mb, current->name, str8(NULL, 0), 0);
    for (i32 i = 0; i < n; i++) if (order[i]->buffer != current) minibuffer_add_candidate(mb, order[i]->buffer->name, str8(NULL, 0), 0);
    if (!current_first) minibuffer_add_candidate(mb, current->name, str8(NULL, 0), 0);
    arena_pop_to(&mb->match_arena, mark);
}

static i64 files_switch_candidates(Minibuffer *mb, void *data, String8 input) {
    (void)input;
    if (!mb->cand_count) files_add_buffers(mb, (App *)data, mb->caller->buffer, 0);
    return 0;
}

static i64 files_kill_candidates(Minibuffer *mb, void *data, String8 input) {
    (void)input;
    if (!mb->cand_count) files_add_buffers(mb, (App *)data, mb->caller->buffer, 1);
    return 0;
}

// Empty input: the default. A name that no buffer has: a new empty buffer (Emacs).
static void files_switch_done(CommandContext *ctx, MiniResult *r) {
    App *app = ctx->app;
    Buffer *buf = r->text.len ? buffer_list_find_name(&app->buffers, r->text) : app_other_buffer(app, ctx->view->buffer);
    if (!buf && !r->text.len) return;
    if (!buf) buf = app_new_buffer(app, r->text);
    app_switch_buffer(app, ctx->view, buf);
    ctx->cursor = &ctx->view->cursors[0];
}

static void cmd_switch_to_buffer(CommandContext *ctx) {
    App *app = ctx->app;
    Buffer *def = app_other_buffer(app, ctx->view->buffer);
    u8 prompt[512];
    i64 n = def ? fmt_buf(prompt, sizeof(prompt), "Switch to buffer (default %S): ", def->name)
                : fmt_buf(prompt, sizeof(prompt), "Switch to buffer: ");
    MiniRequest req = { .kind = MINI_CHOICE, .prompt = str8(prompt, n), .history = MINI_HISTORY_BUFFER, .done = files_switch_done,
                        .candidates = files_switch_candidates, .data = app };
    minibuffer_read(ctx, &req);
}

// Kills a buffer: the views showing it show the most recently shown other buffer, then it leaves
// the list and its memory is released. *scratch* is created again at once.
static void files_kill_buffer(App *app, Buffer *buf) {
    Buffer *fresh = NULL;
    if (!buf->path.len && str8_equal(buf->name, STR8_LIT("*scratch*"))) {
        fresh = buffer_create(STR8_LIT("*scratch*"));
        if (!fresh) os_fatal(STR8_LIT("Out of address space (buffer reserve failed)."));
        app_buffer_settings(app, fresh);
    }
    Buffer *other = app_other_buffer(app, buf);
    if (fresh) {
        if (!other || !files_last_shown(app, other)) other = fresh; // rather the new *scratch* than a buffer never shown
        buffer_list_add(&app->buffers, fresh);
    }
    View *views[WINDOW_MAX];
    i32 n = app_views(app, views);
    for (i32 i = 0; i < n; i++) {
        if (views[i]->buffer == buf) app_switch_buffer(app, views[i], other);
    }
    for (i32 i = 0; i < n; i++) view_forget_buffer(views[i], buf); // every window's remembered position in it
    buffer_list_remove(&app->buffers, buf);
    ASSERT(buf->marker_live == 0);
    buffer_destroy(buf);
}

static void files_kill_confirmed(CommandContext *ctx, MiniResult *r) {
    if (!r->yes) return;
    files_kill_buffer(ctx->app, ctx->mini->state.buffer);
    ctx->cursor = &ctx->view->cursors[0];
}

static void files_kill_done(CommandContext *ctx, MiniResult *r) {
    App *app = ctx->app;
    Buffer *buf = r->text.len ? buffer_list_find_name(&app->buffers, r->text) : ctx->view->buffer;
    if (!buf) return;
    if (buf == app->messages) {
        echo_message(ctx->echo, "*Messages* cannot be killed");
        return;
    }
    if (buf->modified && buf->path.len) {
        u8 prompt[512];
        i64 n = fmt_buf(prompt, sizeof(prompt), "Buffer %S modified; kill anyway? (yes or no) ", buf->name);
        MiniRequest req = { .kind = MINI_YES_NO, .prompt = str8(prompt, n), .done = files_kill_confirmed };
        if (minibuffer_read(ctx, &req)) ctx->mini->state.buffer = buf;
        return;
    }
    files_kill_buffer(app, buf);
    ctx->cursor = &ctx->view->cursors[0];
}

static void cmd_kill_buffer(CommandContext *ctx) {
    u8 prompt[512];
    i64 n = fmt_buf(prompt, sizeof(prompt), "Kill buffer (default %S): ", ctx->view->buffer->name);
    MiniRequest req = { .kind = MINI_CHOICE, .prompt = str8(prompt, n), .history = MINI_HISTORY_BUFFER, .done = files_kill_done,
                        .candidates = files_kill_candidates, .data = ctx->app, .require_match = 1 };
    minibuffer_read(ctx, &req);
}

const Command CMD_SWITCH_TO_BUFFER = { "switch-to-buffer", cmd_switch_to_buffer, COMMAND_ONCE };
const Command CMD_KILL_BUFFER      = { "kill-buffer", cmd_kill_buffer, COMMAND_ONCE };
