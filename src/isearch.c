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

// ---------------------------------------------------------------------------
// query-replace and replace-string

void replace_init(Replace *rp) {
    memset(rp, 0, sizeof(*rp));
    rp->arena = arena_create(REPLACE_ARENA_RESERVE);
    rp->pair_arena = arena_create(REPLACE_ARENA_RESERVE);
}

void replace_destroy(Replace *rp) {
    if (rp->state != REPLACE_OFF) buffer_marker_destroy(rp->view->buffer, rp->end);
    rp->state = REPLACE_OFF;
    os_release(rp->arena.base);
    os_release(rp->pair_arena.base);
}

b32 replace_pending(Replace *rp) {
    return rp->state == REPLACE_SEARCHING || rp->state == REPLACE_ALL;
}

// A word character for case conversion: letters and digits (every byte >= 0x80 counts as a letter, as
// for word motions).
static b32 replace_is_word(u32 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c >= 0x80;
}

String8 replace_case(Arena *arena, String8 to, String8 match) {
    // As Emacs' replace-match decides: what the match's letters look like.
    b32 some_lower = 0, some_upper = 0, some_nonupper_initial = 0, some_multiletter = 0, prev_word = 0;
    for (i64 i = 0; i < match.len;) {
        i64 advance;
        u32 c = utf8_decode(match.data + i, match.len - i, &advance);
        i += advance;
        b32 word = replace_is_word(c);
        if (unicode_upper(c) != c) { // lowercase
            some_lower = 1;
            if (!prev_word) some_nonupper_initial = 1;
            else some_multiletter = 1;
        } else if (unicode_lower(c) != c) { // uppercase
            some_upper = 1;
            if (prev_word) some_multiletter = 1;
        } else if (!prev_word && word) { // a caseless initial (a digit) counts as a lowercase one
            some_nonupper_initial = 1;
        }
        prev_word = word;
    }
    b32 all_caps = (!some_lower && some_multiletter) || (!some_nonupper_initial && !some_multiletter && some_upper);
    b32 cap_initial = !all_caps && !some_nonupper_initial && some_multiletter;
    if (!all_caps && !cap_initial) return to;
    u8 *out = PUSH_ARRAY(arena, u8, to.len * 4);
    i64 n = 0;
    prev_word = 0;
    for (i64 i = 0; i < to.len;) {
        i64 advance;
        u32 c = utf8_decode(to.data + i, to.len - i, &advance);
        b32 word = replace_is_word(c);
        if (c != UTF_REPLACEMENT && (all_caps || (word && !prev_word))) {
            n += utf8_encode(unicode_upper(c), out + n);
        } else {
            memcpy(out + n, to.data + i, (size_t)advance);
            n += advance;
        }
        prev_word = word;
        i += advance;
    }
    return str8(out, n);
}

void replace_finish(Replace *rp, ReplaceEnd how) {
    if (rp->state == REPLACE_OFF) return;
    View *v = rp->view;
    view_set_point(v, &v->cursors[0], rp->point);
    buffer_marker_destroy(v->buffer, rp->end);
    rp->state = REPLACE_OFF;
    if (how == REPLACE_END_QUIT) echo_message(rp->echo, "Quit");
    else echo_message(rp->echo, "Replaced %D occurrence%s%s", rp->count, rp->count == 1 ? "" : "s", how == REPLACE_END_STOPPED ? " (stopped)" : "");
}

// The next search, from rp->next to the end of the range (which replacements move).
static void replace_search_on(Replace *rp) {
    Buffer *buf = rp->view->buffer;
    search_restart(&rp->search, buf, 1, rp->next, rp->lo, buffer_marker_get(buf, rp->end));
}

// Replaces the current match. The next search starts after the replacement, so a replacement that
// contains the search string is never searched again. False (the session ended) if the buffer refused.
static b32 replace_one(Replace *rp, Arena *scratch) {
    Buffer *buf = rp->view->buffer;
    u64 mark = arena_pos(scratch);
    String8 text = rp->to;
    if (rp->convert) text = replace_case(scratch, rp->to, buffer_text(buf, scratch, rp->match_start, rp->match_end));
    b32 ok = buffer_replace(buf, rp->match_start, rp->match_end, text);
    i64 len = text.len;
    arena_pop_to(scratch, mark);
    if (!ok) {
        rp->point = rp->match_end;
        replace_finish(rp, REPLACE_END_DONE);
        echo_message(rp->echo, buf->read_only ? "Buffer is read-only: %S" : "Buffer is full: %S", buf->name);
        return 0;
    }
    rp->count++;
    rp->next = rp->point = rp->match_start + len;
    replace_search_on(rp);
    return 1;
}

i64 replace_work(Replace *rp, Arena *scratch, i64 budget) {
    Buffer *buf = rp->view->buffer;
    i64 used = 0;
    while (used < budget && replace_pending(rp)) {
        i64 before = rp->search.examined;
        SearchStatus st = search_run(&rp->search, buf, budget - used);
        used += MAX(rp->search.examined - before, 1);
        if (st == SEARCH_RUNNING) continue;
        if (st == SEARCH_NOT_FOUND) {
            replace_finish(rp, REPLACE_END_DONE);
            break;
        }
        rp->match_start = rp->search.match_start;
        rp->match_end = rp->search.match_end;
        if (rp->state == REPLACE_SEARCHING) { // query-replace: ask about it
            rp->state = REPLACE_ASKING;
            rp->point = rp->match_end;
            rp->match_edits = buf->edit_count;
            view_set_point(rp->view, &rp->view->cursors[0], rp->point);
            break;
        }
        if (!replace_one(rp, scratch)) break;
        used += REPLACE_COST;
    }
    return used;
}

String8 replace_prompt(Replace *rp, Arena *arena) {
    if (rp->state == REPLACE_ALL) {
        i64 end = buffer_marker_get(rp->view->buffer, rp->end);
        i64 done = end > rp->all_from ? (rp->next - rp->all_from) * 100 / (end - rp->all_from) : 100;
        return str8_fmt(arena, "Replacing... %D%%", CLAMP(done, 0, 100));
    }
    if (rp->state == REPLACE_SEARCHING) {
        return str8_fmt(arena, "Query replacing %S with %S: [searching... %d%%]", rp->from, rp->to, search_progress(&rp->search));
    }
    return str8_fmt(arena, "Query replacing %S with %S: (y, n, !, q, .)", rp->from, rp->to);
}

// The session on the calling view: [point, end) or the region, the mark at the start.
static void replace_start(CommandContext *ctx, String8 from, String8 to) {
    Replace *rp = ctx->replace;
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    Cursor *c = &v->cursors[0];
    arena_reset(&rp->arena);
    rp->from = str8_copy(&rp->arena, from); // `from` and `to` may be the last pair itself
    rp->to = str8_copy(&rp->arena, to);
    arena_reset(&rp->pair_arena);
    rp->last_from = str8_copy(&rp->pair_arena, rp->from);
    rp->last_to = str8_copy(&rp->pair_arena, rp->to);
    rp->has_last = 1;
    i64 lo = rp->region ? rp->region_lo : view_point(v, c);
    i64 hi = rp->region ? rp->region_hi : buffer_size(buf);
    rp->view = v;
    rp->echo = ctx->echo;
    rp->fold = !search_has_upper(rp->from);
    rp->convert = rp->fold && !search_has_upper(rp->to);
    rp->lo = rp->next = rp->point = rp->all_from = lo;
    rp->end = buffer_marker_create(buf, hi, 0);
    rp->count = 0;
    view_deactivate_mark(v);
    view_set_point(v, c, lo);
    view_set_mark(v, c, lo, 0);
    rp->state = rp->ask ? REPLACE_SEARCHING : REPLACE_ALL;
    search_begin(&rp->search, buf, rp->from, rp->fold, 1, lo, lo, hi);
}

static void replace_to_done(CommandContext *ctx, MiniResult *r) {
    replace_start(ctx, ctx->mini->state.text, r->text);
}

static void replace_from_done(CommandContext *ctx, MiniResult *r) {
    Replace *rp = ctx->replace;
    if (!r->text.len) {
        if (rp->has_last) replace_start(ctx, rp->last_from, rp->last_to);
        else echo_message(ctx->echo, "Empty search string");
        return;
    }
    if (r->text.len > SEARCH_NEEDLE_MAX) {
        echo_message(ctx->echo, "Search string too long");
        return;
    }
    ctx->mini->state.text = r->text; // in the chain arena
    String8 prompt = str8_fmt(ctx->scratch, "%s %S%s with: ", rp->ask ? "Query replace" : "Replace string", r->text,
                              rp->region ? " in region" : "");
    MiniRequest req = { .kind = MINI_TEXT, .prompt = prompt, .history = MINI_HISTORY_REPLACE, .done = replace_to_done };
    minibuffer_read(ctx, &req);
}

static void replace_command(CommandContext *ctx, b32 ask) {
    Replace *rp = ctx->replace;
    if (!rp) return;
    View *v = ctx->view;
    Buffer *buf = v->buffer;
    if (buf->read_only && !(ctx->mini && v == ctx->mini->view)) {
        echo_message(ctx->echo, "Buffer is read-only: %S", buf->name);
        return;
    }
    Cursor *c = &v->cursors[0];
    i64 lo = 0, hi = 0;
    b32 region = view_region_active(v, c, ctx->settings) && view_region(v, c, &lo, &hi);
    const char *what = ask ? "Query replace" : "Replace string";
    String8 prompt = rp->has_last ? str8_fmt(ctx->scratch, "%s%s (default %S -> %S): ", what, region ? " in region" : "", rp->last_from, rp->last_to)
                                  : str8_fmt(ctx->scratch, "%s%s: ", what, region ? " in region" : "");
    MiniRequest req = { .kind = MINI_TEXT, .prompt = prompt, .history = MINI_HISTORY_REPLACE, .done = replace_from_done };
    if (!minibuffer_read(ctx, &req)) return;
    rp->ask = ask;
    rp->region = region;
    rp->region_lo = lo;
    rp->region_hi = hi;
}

static void cmd_query_replace(CommandContext *ctx) { replace_command(ctx, 1); }
static void cmd_replace_string(CommandContext *ctx) { replace_command(ctx, 0); }

// An answer to the question (ctx->codepoint), on the session's own match.
static void cmd_replace_answer(CommandContext *ctx) {
    Replace *rp = ctx->replace;
    if (!rp || rp->state != REPLACE_ASKING) return;
    view_set_point(rp->view, &rp->view->cursors[0], rp->point); // back on the match, wherever the wheel left point
    if (rp->view->buffer->edit_count != rp->match_edits) { // changed outside since (a revert): look for the match again
        rp->next = MIN(rp->match_start, buffer_size(rp->view->buffer));
        replace_search_on(rp);
        rp->state = REPLACE_SEARCHING;
        return;
    }
    switch (ctx->codepoint) {
    case 'y':
    case ' ':
        if (replace_one(rp, ctx->scratch)) rp->state = REPLACE_SEARCHING;
        break;
    case 'n':
    case 0x7f:
        rp->next = rp->match_end;
        replace_search_on(rp);
        rp->state = REPLACE_SEARCHING;
        break;
    case '!':
        if (replace_one(rp, ctx->scratch)) {
            rp->state = REPLACE_ALL;
            rp->all_from = rp->next;
        }
        break;
    case '.':
        if (replace_one(rp, ctx->scratch)) replace_finish(rp, REPLACE_END_DONE);
        break;
    case 'q':
    case '\r':
        replace_finish(rp, REPLACE_END_DONE);
        break;
    }
}

// C-g: stops the session, keeping what was replaced.
static void cmd_replace_quit(CommandContext *ctx) {
    Replace *rp = ctx->replace;
    if (!rp || rp->state == REPLACE_OFF) return;
    replace_finish(rp, rp->state == REPLACE_ALL ? REPLACE_END_STOPPED : REPLACE_END_QUIT);
}

// Internal (not in the command table). Both continue the session's undo group.
static const Command CMD_REPLACE_ANSWER = { "replace-answer", cmd_replace_answer, COMMAND_ONCE | COMMAND_UNDO_CONTINUE };
static const Command CMD_REPLACE_QUIT   = { "replace-quit", cmd_replace_quit, COMMAND_ONCE | COMMAND_UNDO_CONTINUE };

b32 replace_key(CommandContext *ctx, const Command *command, u32 answer) {
    Replace *rp = ctx->replace;
    ctx->view = rp->view;
    if (command && (command->flags & COMMAND_QUIT)) { // first: no answer can shadow C-g
        view_run_command(ctx, &CMD_REPLACE_QUIT);
        return 1;
    }
    if (rp->state != REPLACE_ASKING) return 1; // searching or replacing all: only C-g counts
    if (answer) {
        ctx->codepoint = answer;
        view_run_command(ctx, &CMD_REPLACE_ANSWER);
        return 1;
    }
    replace_finish(rp, REPLACE_END_DONE); // any other key ends the session, then runs (Emacs)
    return 0;
}

const Command CMD_QUERY_REPLACE  = { "query-replace", cmd_query_replace, COMMAND_ONCE };
const Command CMD_REPLACE_STRING = { "replace-string", cmd_replace_string, COMMAND_ONCE };
