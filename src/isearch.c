// isearch.c — see isearch.h.

void isearch_init(Isearch *is, Minibuffer *mini) {
    memset(is, 0, sizeof(*is));
    is->step_arena = arena_create(ISEARCH_ARENA_RESERVE);
    is->text_arena = arena_create(ISEARCH_ARENA_RESERVE);
    is->searching = -1;
    is->mini = mini;
}

void isearch_destroy(Isearch *is) {
    os_release(is->step_arena.base);
    os_release(is->text_arena.base);
}

b32 isearch_pending(Isearch *is) {
    return is->active && is->searching >= 0;
}

IsearchStep *isearch_top(Isearch *is) {
    return &is->steps[is->count - 1];
}

IsearchStep *isearch_current(Isearch *is) {
    return &is->steps[is->searching >= 0 ? is->searching - 1 : is->count - 1];
}

// Point goes to the current match (wherever the wheel dragged it meanwhile).
static void isearch_show(Isearch *is) {
    View *v = is->view;
    view_set_point(v, &v->cursors[0], isearch_current(is)->point);
}

// Starts resolving step k from the step below it: true when that takes a search (then the engine is
// set up), false when the step is resolved at once.
static b32 isearch_begin_step(Isearch *is, i32 k) {
    IsearchStep *p = &is->steps[k - 1], *s = &is->steps[k];
    Buffer *buf = is->view->buffer;
    i64 size = buffer_size(buf);
    // What the step shows unless its search finds something: the step below's match and point.
    s->match_start = p->match_start;
    s->match_end = p->match_end;
    s->point = p->point;
    s->success = p->success;
    s->wrapped = p->wrapped;
    s->overwrapped = p->overwrapped;
    s->resolved = 1;
    if (!s->string.len) { // a direction change or M-c with nothing to search for
        s->success = 1;
        return 0;
    }
    b32 has_match = p->match_start >= 0;
    i64 from;
    switch (s->kind) {
    case ISEARCH_STEP_TYPE:
        if (!p->success) return 0; // a longer string fails where the shorter one did
        // From the current match, so a longer string can match in the same place.
        from = !has_match ? p->point : s->forward ? p->match_start : p->match_start + s->string.len;
        break;
    case ISEARCH_STEP_CASE:
        from = !has_match ? p->point : s->forward ? p->match_start : p->match_start + s->string.len;
        break;
    case ISEARCH_STEP_HISTORY:
        from = is->origin;
        s->wrapped = s->overwrapped = 0;
        break;
    case ISEARCH_STEP_REVERSE:
        if (p->success && has_match) { // the same match, point to its other end
            s->point = s->forward ? p->match_end : p->match_start;
            return 0;
        }
        from = p->point;
        break;
    case ISEARCH_STEP_REPEAT:
        if (!p->string.len) {
            from = p->point; // the last search string: a first search from point
        } else if (p->success) {
            from = !has_match ? p->point : s->forward ? p->match_end : p->match_start;
        } else { // repeating a failing search wraps around
            from = s->forward ? 0 : size;
            s->wrapped = 1;
        }
        break;
    default:
        return 0;
    }
    search_begin(&is->search, buf, s->string, s->fold, s->forward, from, 0, size);
    s->resolved = 0;
    return 1;
}

// Resolves the steps from k on until one needs a search (the engine then works on it).
static void isearch_resolve_from(Isearch *is, i32 k) {
    for (; k < is->count; k++) {
        if (isearch_begin_step(is, k)) {
            is->searching = k;
            return;
        }
    }
    is->searching = -1;
    isearch_show(is);
}

// The engine finished the step it worked on.
static void isearch_finish_step(Isearch *is) {
    IsearchStep *s = &is->steps[is->searching];
    if (is->search.status == SEARCH_FOUND) {
        s->success = 1;
        s->match_start = is->search.match_start;
        s->match_end = is->search.match_end;
        s->point = s->forward ? s->match_end : s->match_start;
        if (s->wrapped) s->overwrapped = s->forward ? s->point > is->origin : s->point < is->origin;
    } else {
        s->success = 0; // the match and point stay the step below's
    }
    s->resolved = 1;
    isearch_resolve_from(is, is->searching + 1);
}

i64 isearch_work(Isearch *is, i64 budget) {
    if (!isearch_pending(is)) return 0;
    i64 before = is->search.examined;
    SearchStatus st = search_run(&is->search, is->view->buffer, budget);
    i64 used = is->search.examined - before;
    if (st != SEARCH_RUNNING) isearch_finish_step(is);
    return MAX(used, 1);
}

// A new step on top, inheriting the top's string, direction and case; resolved now unless a search
// is still running (then it waits its turn).
static IsearchStep *isearch_push(Isearch *is, IsearchStepKind kind) {
    IsearchStep *top = isearch_top(is);
    IsearchStep *s = PUSH_STRUCT(&is->step_arena, IsearchStep);
    ASSERT(s == is->steps + is->count);
    *s = *top;
    s->kind = kind;
    s->resolved = 0;
    is->count++;
    return s;
}

static void isearch_kick(Isearch *is) {
    if (is->searching < 0) isearch_resolve_from(is, is->count - 1);
}

static void isearch_pop(Isearch *is) {
    is->count--;
    arena_pop_to(&is->step_arena, (u64)is->count * sizeof(IsearchStep));
    if (is->searching >= is->count) is->searching = -1; // its search goes with it
    if (is->searching < 0) isearch_show(is);
}

static void isearch_set_string(IsearchStep *s, String8 string) {
    s->string = string;
    if (!s->explicit_case) s->fold = !search_has_upper(string); // smart case
}

// The isearch, when one is active (point back on its current match); otherwise "Not in an isearch".
static Isearch *isearch_here(CommandContext *ctx) {
    Isearch *is = ctx->isearch;
    if (!is || !is->active) {
        echo_message(ctx->echo, "Not in an isearch");
        return NULL;
    }
    isearch_show(is);
    return is;
}

// ---------------------------------------------------------------------------
// Commands

static void isearch_end(CommandContext *ctx, b32 abort) {
    Isearch *is = ctx->isearch;
    View *v = is->view;
    Cursor *c = &v->cursors[0];
    IsearchStep *cur = isearch_current(is);
    String8 string = isearch_top(is)->string;
    is->active = 0;
    is->searching = -1;
    if (abort) {
        view_set_point(v, c, is->origin);
        echo_message(ctx->echo, "Quit");
        return;
    }
    view_set_point(v, c, cur->point);
    if (string.len) minibuffer_history_add(is->mini, MINI_HISTORY_SEARCH, string);
    if (cur->point != is->origin && !view_region_active(v, c, ctx->settings)) {
        view_set_mark(v, c, is->origin, 0);
        echo_message(ctx->echo, "Mark saved where search started");
    }
}

void isearch_exit(CommandContext *ctx) {
    if (ctx->isearch && ctx->isearch->active) isearch_end(ctx, 0);
}

static void isearch_repeat(CommandContext *ctx, b32 forward) {
    Isearch *is = isearch_here(ctx);
    if (!is) return;
    IsearchStep *top = isearch_top(is);
    if (top->forward != forward) {
        isearch_push(is, ISEARCH_STEP_REVERSE)->forward = forward;
        isearch_kick(is);
        return;
    }
    String8 string = top->string;
    if (!string.len) { // C-s C-s: the last search string
        MiniHistory *h = &is->mini->histories[MINI_HISTORY_SEARCH];
        if (!h->count) {
            echo_message(ctx->echo, "No previous search string");
            return;
        }
        string = str8_copy(&is->text_arena, h->items[0]);
    }
    isearch_set_string(isearch_push(is, ISEARCH_STEP_REPEAT), string);
    isearch_kick(is);
}

static void isearch_start(CommandContext *ctx, b32 forward) {
    Isearch *is = ctx->isearch;
    if (!is) return;
    if (is->active) { // bound in the global map only: repeat
        isearch_repeat(ctx, forward);
        return;
    }
    if (ctx->mini && ctx->view == ctx->mini->view) {
        echo_message(ctx->echo, "isearch is not available in the minibuffer");
        return;
    }
    arena_reset(&is->step_arena);
    arena_reset(&is->text_arena);
    is->steps = (IsearchStep *)is->step_arena.base;
    is->view = ctx->view;
    is->origin = view_point(ctx->view, &ctx->view->cursors[0]);
    is->searching = -1;
    is->ring_pos = -1;
    IsearchStep *s = PUSH_STRUCT(&is->step_arena, IsearchStep);
    *s = (IsearchStep){ .kind = ISEARCH_STEP_START, .forward = forward, .fold = 1, .resolved = 1, .success = 1,
                        .match_start = -1, .match_end = -1, .point = is->origin };
    is->count = 1;
    is->active = 1;
}

// Appends text to the search string (one step). `yanked`: taken from the buffer or the kill ring, so
// it is lowercased while the search folds (a yank does not make the search exact; Emacs'
// search-upper-case = not-yanks).
static void isearch_append(CommandContext *ctx, Isearch *is, String8 text, b32 yanked) {
    IsearchStep *top = isearch_top(is);
    if (!text.len) return;
    if (top->string.len + text.len > SEARCH_NEEDLE_MAX) {
        echo_set(ctx->echo, STR8_LIT("Search string too long"));
        return;
    }
    u8 *data = PUSH_ARRAY(&is->text_arena, u8, top->string.len + text.len);
    memcpy(data, top->string.data, (size_t)top->string.len);
    if (yanked && top->fold) search_fold_bytes(data + top->string.len, text.data, text.len);
    else memcpy(data + top->string.len, text.data, (size_t)text.len);
    isearch_set_string(isearch_push(is, ISEARCH_STEP_TYPE), str8(data, top->string.len + text.len));
    isearch_kick(is);
}

static void cmd_isearch_forward(CommandContext *ctx) { isearch_start(ctx, 1); }
static void cmd_isearch_backward(CommandContext *ctx) { isearch_start(ctx, 0); }
static void cmd_isearch_repeat_forward(CommandContext *ctx) { isearch_repeat(ctx, 1); }
static void cmd_isearch_repeat_backward(CommandContext *ctx) { isearch_repeat(ctx, 0); }

static void cmd_isearch_printing_char(CommandContext *ctx) {
    Isearch *is = isearch_here(ctx);
    if (!is || !ctx->codepoint) return;
    u8 bytes[4];
    isearch_append(ctx, is, str8(bytes, utf8_encode(ctx->codepoint, bytes)), 0);
}

// DEL: back to the step before (a character or a repeat), restoring its match and point.
static void cmd_isearch_delete_char(CommandContext *ctx) {
    Isearch *is = isearch_here(ctx);
    if (is && is->count > 1) isearch_pop(is);
}

static void cmd_isearch_exit(CommandContext *ctx) {
    if (isearch_here(ctx)) isearch_end(ctx, 0);
}

// C-g: while the search is failing (or still searching), removes the part not found; otherwise back to
// where the search started.
static void cmd_isearch_abort(CommandContext *ctx) {
    Isearch *is = isearch_here(ctx);
    if (!is) return;
    if (is->searching >= 0 || !isearch_top(is)->success) {
        while (is->count > 1 && (is->searching >= 0 || !isearch_top(is)->success)) isearch_pop(is);
        return;
    }
    isearch_end(ctx, 1);
}

// C-w: the rest of the word after the current match (from point when there is none).
static void cmd_isearch_yank_word(CommandContext *ctx) {
    Isearch *is = isearch_here(ctx);
    if (!is) return;
    IsearchStep *cur = isearch_current(is);
    Buffer *buf = is->view->buffer;
    i64 from = cur->match_end >= 0 ? cur->match_end : cur->point;
    i64 to = view_forward_word(buf, from, ctx->settings->underscore_is_word);
    isearch_append(ctx, is, buffer_text(buf, ctx->scratch, from, to), 1);
}

// C-y: the latest kill (the clipboard, when another program changed it since).
static void cmd_isearch_yank_kill(CommandContext *ctx) {
    Isearch *is = isearch_here(ctx);
    if (!is || !ctx->kills) return;
    kill_from_clipboard(ctx->kills);
    isearch_append(ctx, is, kill_entry(ctx->kills, 0), 1);
}

// M-p (older) and M-n (newer): the search history, around the ring.
static void isearch_ring(CommandContext *ctx, i32 dir) {
    Isearch *is = isearch_here(ctx);
    if (!is) return;
    MiniHistory *h = &is->mini->histories[MINI_HISTORY_SEARCH];
    if (!h->count) {
        echo_message(ctx->echo, "No previous search string");
        return;
    }
    is->ring_pos = is->ring_pos < 0 ? (dir > 0 ? 0 : h->count - 1) : ((is->ring_pos + dir) % h->count + h->count) % h->count;
    isearch_set_string(isearch_push(is, ISEARCH_STEP_HISTORY), str8_copy(&is->text_arena, h->items[is->ring_pos]));
    isearch_kick(is);
}

static void cmd_isearch_ring_retreat(CommandContext *ctx) { isearch_ring(ctx, 1); }
static void cmd_isearch_ring_advance(CommandContext *ctx) { isearch_ring(ctx, -1); }

// M-c: case-insensitive <-> exact for the rest of this search (smart case no longer decides).
static void cmd_isearch_toggle_case_fold(CommandContext *ctx) {
    Isearch *is = isearch_here(ctx);
    if (!is) return;
    IsearchStep *s = isearch_push(is, ISEARCH_STEP_CASE);
    s->explicit_case = 1;
    s->fold = !s->fold;
    echo_set(ctx->echo, s->fold ? STR8_LIT("case insensitive") : STR8_LIT("case sensitive"));
    isearch_kick(is);
}

// ---------------------------------------------------------------------------
// The echo line

String8 isearch_prompt(Isearch *is, Arena *arena) {
    IsearchStep *top = isearch_top(is);
    b32 pending = is->searching >= 0;
    IsearchStep *s = pending ? isearch_current(is) : top;
    String8 p = str8_fmt(arena, "%s%sI-search%s: ", !pending && !top->success ? "failing " : "",
                         s->overwrapped ? "overwrapped " : s->wrapped ? "wrapped " : "", top->forward ? "" : " backward");
    if (p.data[0] >= 'a' && p.data[0] <= 'z') p.data[0] = (u8)(p.data[0] - 32);
    return p;
}

i64 isearch_fail_pos(Isearch *is) {
    IsearchStep *top = isearch_top(is);
    if (is->searching >= 0 || top->success) return top->string.len;
    for (i32 k = is->count - 1; k >= 0; k--) {
        IsearchStep *s = &is->steps[k];
        if (s->resolved && s->success && str8_starts_with(top->string, s->string)) return s->string.len;
    }
    return 0;
}

const Command CMD_ISEARCH_FORWARD          = { "isearch-forward", cmd_isearch_forward, COMMAND_ONCE };
const Command CMD_ISEARCH_BACKWARD         = { "isearch-backward", cmd_isearch_backward, COMMAND_ONCE };
const Command CMD_ISEARCH_REPEAT_FORWARD   = { "isearch-repeat-forward", cmd_isearch_repeat_forward, COMMAND_ONCE | COMMAND_ISEARCH };
const Command CMD_ISEARCH_REPEAT_BACKWARD  = { "isearch-repeat-backward", cmd_isearch_repeat_backward, COMMAND_ONCE | COMMAND_ISEARCH };
const Command CMD_ISEARCH_DELETE_CHAR      = { "isearch-delete-char", cmd_isearch_delete_char, COMMAND_ONCE | COMMAND_ISEARCH };
const Command CMD_ISEARCH_EXIT             = { "isearch-exit", cmd_isearch_exit, COMMAND_ONCE | COMMAND_ISEARCH };
const Command CMD_ISEARCH_ABORT            = { "isearch-abort", cmd_isearch_abort, COMMAND_ONCE | COMMAND_ISEARCH | COMMAND_QUIT };
const Command CMD_ISEARCH_YANK_WORD        = { "isearch-yank-word", cmd_isearch_yank_word, COMMAND_ONCE | COMMAND_ISEARCH };
const Command CMD_ISEARCH_YANK_KILL        = { "isearch-yank-kill", cmd_isearch_yank_kill, COMMAND_ONCE | COMMAND_ISEARCH };
const Command CMD_ISEARCH_RING_RETREAT     = { "isearch-ring-retreat", cmd_isearch_ring_retreat, COMMAND_ONCE | COMMAND_ISEARCH };
const Command CMD_ISEARCH_RING_ADVANCE     = { "isearch-ring-advance", cmd_isearch_ring_advance, COMMAND_ONCE | COMMAND_ISEARCH };
const Command CMD_ISEARCH_TOGGLE_CASE_FOLD = { "isearch-toggle-case-fold", cmd_isearch_toggle_case_fold, COMMAND_ONCE | COMMAND_ISEARCH };
// Internal (not in the command table): a plain character typed while searching.
const Command CMD_ISEARCH_PRINTING_CHAR    = { "isearch-printing-char", cmd_isearch_printing_char, COMMAND_ONCE | COMMAND_ISEARCH };
