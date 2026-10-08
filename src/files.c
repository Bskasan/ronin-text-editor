// files.c — the file and buffer commands built on prompts: switching and killing buffers. Part of
// the app (included after app.c): it uses App directly.

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
