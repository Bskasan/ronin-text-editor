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
    view_switch_buffer(ctx->view, &app->buffers, buf);
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
    OsFileStatus status = buffer_save_opt(buf, ctx->settings->fsync_on_save);
    if (status == OS_FILE_OK) files_echo(ctx->echo, "Wrote %S", buf->path);
    else echo_message(ctx->echo, "Cannot save %S: %s", buf->name, buffer_status_text(status));
}

const Command CMD_SAVE_BUFFER = { "save-buffer", cmd_save_buffer, COMMAND_ONCE };
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
    view_switch_buffer(ctx->view, &app->buffers, buf);
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
    for (i32 i = 0; i < app->view_count; i++) {
        if (app->views[i]->buffer == buf) view_switch_buffer(app->views[i], &app->buffers, other);
    }
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
