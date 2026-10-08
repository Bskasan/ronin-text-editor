// minibuffer.c — see minibuffer.h.

// ---------------------------------------------------------------------------
// Matching

String8 match_fold(Arena *arena, String8 s) {
    u8 *out = PUSH_ARRAY(arena, u8, s.len);
    for (i64 i = 0; i < s.len;) {
        u8 b = s.data[i];
        if (b < 0x80) {
            out[i++] = b >= 'A' && b <= 'Z' ? (u8)(b + 32) : b;
            continue;
        }
        i64 advance;
        u32 c = utf8_decode(s.data + i, s.len - i, &advance);
        u8 lower[4];
        i64 n = utf8_encode(unicode_lower(c), lower);
        if (n == advance && c != UTF_REPLACEMENT) memcpy(out + i, lower, (size_t)n);
        else memcpy(out + i, s.data + i, (size_t)advance); // keeps the length (and invalid bytes)
        i += advance;
    }
    return str8(out, s.len);
}

MatchQuery match_query(Arena *arena, String8 input) {
    MatchQuery q = { 0 };
    q.whole = match_fold(arena, input);
    for (i64 i = 0; i < q.whole.len && q.count < MATCH_MAX_TERMS;) {
        while (i < q.whole.len && q.whole.data[i] == ' ') i++;
        i64 start = i;
        while (i < q.whole.len && q.whole.data[i] != ' ') i++;
        if (i > start) q.terms[q.count++] = str8(q.whole.data + start, i - start);
    }
    return q;
}

// The first occurrence of `term` in `s`, or -1.
static i64 match_find(String8 s, String8 term) {
    if (!term.len) return 0;
    u8 first = term.data[0];
    for (i64 i = 0, last = s.len - term.len; i <= last; i++) {
        if (s.data[i] == first && mem_equal(s.data + i, term.data, term.len)) return i;
    }
    return -1;
}

MatchScore match_score(MatchQuery *q, Candidate *c) {
    for (i32 k = 0; k < q->count; k++) {
        if (match_find(c->folded, q->terms[k]) < 0) return MATCH_NONE;
    }
    if (q->count && str8_equal(c->folded, q->whole)) return MATCH_EXACT;
    if (q->count && str8_starts_with(c->folded, q->terms[0])) {
        return MATCH_PREFIX;
    }
    return MATCH_SUBSTRING;
}

i32 match_spans(MatchQuery *q, Candidate *c, MatchSpan *out, i32 cap) {
    i32 n = 0;
    for (i32 k = 0; k < q->count && n < cap; k++) {
        i64 at = match_find(c->folded, q->terms[k]);
        if (at >= 0) out[n++] = (MatchSpan){ at, at + q->terms[k].len };
    }
    return n;
}

i64 match_rank(MatchQuery *q, Candidate *cands, i64 count, i32 *out) {
    i64 found[MATCH_EXACT + 1] = { 0 };
    for (i64 i = 0; i < count; i++) {
        MatchScore s = match_score(q, &cands[i]);
        cands[i].score = (u8)s;
        found[s]++;
    }
    // Stable: one pass per score, best first.
    i64 n = 0;
    for (i32 s = MATCH_EXACT; s > MATCH_NONE; s--) {
        if (!found[s]) continue;
        for (i64 i = 0; i < count; i++) if (cands[i].score == s) out[n++] = (i32)i;
    }
    return n;
}

// ---------------------------------------------------------------------------
// The minibuffer

void minibuffer_init(Minibuffer *mb, Arena *perm, Buffer *buffer) {
    memset(mb, 0, sizeof(*mb));
    mb->buffer = buffer;
    mb->view = view_create(perm, buffer);
    mb->arena = arena_create(MINI_ARENA_RESERVE);
    mb->cand_arena = arena_create(MINI_ARENA_RESERVE);
    mb->text_arena = arena_create(MINI_ARENA_RESERVE);
    mb->match_arena = arena_create(MINI_ARENA_RESERVE);
    mb->history_arena = arena_create(MINI_ARENA_RESERVE);
    mb->history_pos = -1;
}

i32 minibuffer_destroy(Minibuffer *mb) {
    view_destroy(mb->view);
    i32 leaks = (i32)mb->buffer->marker_live;
    if (!buffer_destroy(mb->buffer)) leaks++;
    Arena *arenas[] = { &mb->arena, &mb->cand_arena, &mb->text_arena, &mb->match_arena, &mb->history_arena };
    for (i32 i = 0; i < ARRAY_COUNT(arenas); i++) os_release(arenas[i]->base);
    return leaks;
}

String8 minibuffer_input(Minibuffer *mb, Arena *arena) {
    String8 s = buffer_text(mb->buffer, arena, 0, buffer_size(mb->buffer));
    return str8_copy(arena, s);
}

// Replaces the whole input (history, completion); point goes to its end. The replacement is
// an ordinary edit, so it can be undone.
static void minibuffer_set_input(Minibuffer *mb, String8 text) {
    Buffer *buf = mb->buffer;
    buffer_replace(buf, 0, buffer_size(buf), text);
    view_set_point(mb->view, &mb->view->cursors[0], buffer_size(buf));
}

// The input from scratch: no text, no undo history, no mark.
static void minibuffer_reset_input(Minibuffer *mb, String8 text) {
    Buffer *buf = mb->buffer;
    buffer_undo_enable(buf, 0);
    buffer_replace(buf, 0, buffer_size(buf), text);
    buffer_undo_enable(buf, 1);
    View *v = mb->view;
    Cursor *c = &v->cursors[0];
    view_set_point(v, c, buffer_size(buf));
    c->mark_set = c->mark_active = c->mark_shift = 0;
    buffer_marker_set(buf, v->top, 0);
    v->left_col = 0;
}

void minibuffer_clear_candidates(Minibuffer *mb) {
    arena_reset(&mb->cand_arena);
    arena_reset(&mb->text_arena);
    mb->cands = (Candidate *)mb->cand_arena.base;
    mb->cand_count = 0;
    mb->cand_key = str8(NULL, 0);
}

void minibuffer_add_candidate(Minibuffer *mb, String8 text, String8 annotation, u32 flags) {
    Candidate *c = PUSH_STRUCT(&mb->cand_arena, Candidate);
    ASSERT(c == mb->cands + mb->cand_count);
    c->text = str8_copy(&mb->text_arena, text);
    c->folded = match_fold(&mb->text_arena, text);
    c->annotation = annotation.len ? str8_copy(&mb->text_arena, annotation) : annotation;
    c->flags = flags;
    mb->cand_count++;
}

// Filters and ranks the candidates for the current input; the first match is selected.
static void minibuffer_filter(Minibuffer *mb) {
#if TEAL_DEV
    u64 t0 = os_time_us();
#endif
    arena_reset(&mb->match_arena);
    mb->seen_edits = mb->buffer->edit_count;
    mb->match_count = 0;
    mb->selected = mb->list_top = 0;
    if (mb->kind != MINI_CHOICE) return;
    String8 input = minibuffer_input(mb, &mb->match_arena);
    i64 from = mb->candidates_fn ? mb->candidates_fn(mb, mb->candidates_data, input) : 0;
    mb->match_from = CLAMP(from, 0, input.len);
    mb->query = match_query(&mb->match_arena, str8(input.data + mb->match_from, input.len - mb->match_from));
    mb->matches = PUSH_ARRAY(&mb->match_arena, i32, mb->cand_count);
    mb->match_count = match_rank(&mb->query, mb->cands, mb->cand_count, mb->matches);
#if TEAL_DEV
    mb->dev_filter_us = os_time_us() - t0;
    mb->dev_filters++;
#endif
}

b32 minibuffer_read(CommandContext *ctx, MiniRequest *req) {
    Minibuffer *mb = ctx->mini;
    if (!mb) {
        echo_message(ctx->echo, "No minibuffer");
        return 0;
    }
    if (mb->active) {
        echo_message(ctx->echo, "Command attempted to use minibuffer while in minibuffer");
        return 0;
    }
    if (!mb->in_continuation) arena_reset(&mb->arena); // a new chain (its state is the command's to set)
    mb->active = 1;
    mb->finished = 0;
    mb->kind = req->kind;
    mb->prompt = str8_copy(&mb->arena, req->prompt);
    mb->done = req->done;
    mb->then_command = NULL;
    mb->caller = ctx->view;
    mb->answers = req->answers;
    mb->candidates_fn = req->candidates;
    mb->candidates_data = req->data;
    mb->require_match = req->require_match;
    mb->file = req->file;
    mb->run_command = req->run_command;
    mb->history = req->history;
    mb->history_pos = -1;
    mb->typed = str8(NULL, 0);
    if (req->kind != MINI_CHOICE) minibuffer_clear_candidates(mb);
    else if (req->candidates) minibuffer_clear_candidates(mb); // the callback fills them
    minibuffer_reset_input(mb, req->kind == MINI_KEY ? str8(NULL, 0) : req->initial);
    minibuffer_filter(mb);
    echo_clear(ctx->echo);
    return 1;
}

// The calling View stays recorded until the continuation has run.
static void minibuffer_close(Minibuffer *mb) {
    mb->active = 0;
    minibuffer_reset_input(mb, str8(NULL, 0));
    minibuffer_clear_candidates(mb);
    arena_reset(&mb->match_arena);
    mb->match_count = 0;
}

void minibuffer_abort(Minibuffer *mb) {
    if (!mb->active) return;
    minibuffer_close(mb);
    mb->finished = 0;
    mb->done = NULL;
    mb->then_command = NULL;
    mb->state = (MiniState){ 0 };
    arena_reset(&mb->arena);
}

// Accepts `text` (copied into the chain arena): the prompt closes and the driver runs the
// continuation after the running command.
static void minibuffer_finish(Minibuffer *mb, String8 text, i32 candidate, u32 flags) {
    MiniResult *r = &mb->result;
    *r = (MiniResult){ .text = str8_copy(&mb->arena, text), .candidate = candidate, .flags = flags };
    if (mb->history != MINI_HISTORY_NONE && mb->kind != MINI_KEY && mb->kind != MINI_YES_NO) {
        minibuffer_history_add(mb, mb->history, r->text);
    }
    if (mb->run_command) mb->then_command = command_find(r->text);
    minibuffer_close(mb);
    mb->finished = 1;
}

static void cmd_minibuffer_done(CommandContext *ctx) {
    Minibuffer *mb = ctx->mini;
    MiniDoneFn *done = mb->done;
    mb->done = NULL;
    if (done) done(ctx, &mb->result);
}

// Internal: never bound, not in the command table.
static const Command CMD_MINIBUFFER_DONE = { "minibuffer-done", cmd_minibuffer_done, COMMAND_ONCE };

void minibuffer_after_command(CommandContext *ctx) {
    Minibuffer *mb = ctx->mini;
    if (mb->active && mb->buffer->edit_count != mb->seen_edits) minibuffer_filter(mb);
    if (!mb->finished) return;
    mb->finished = 0;
    // The continuation is a command of its own on the calling View: its own undo boundary and
    // ensure_visible; with M-x the chosen command itself.
    CommandContext c = *ctx;
    c.view = mb->caller ? mb->caller : ctx->view;
    c.cursor = NULL;
    c.codepoint = 0;
    c.shift_translated = 0;
    const Command *cmd = mb->then_command ? mb->then_command : &CMD_MINIBUFFER_DONE;
    mb->then_command = NULL;
    mb->in_continuation++;
    view_run_command(&c, cmd);
    mb->in_continuation--;
    ctx->last_command = c.last_command;
}

// ---------------------------------------------------------------------------
// History: per category, session only, no consecutive duplicates.

// Moves the live strings down over the dead ones once more than half of the arena is dead.
static void minibuffer_history_compact(Minibuffer *mb) {
    Arena *a = &mb->history_arena;
    if (a->pos < KB(64) || a->pos < 2 * mb->history_live) return;
    String8 *items[MINI_HISTORY_COUNT * MINI_HISTORY_MAX];
    i32 n = 0;
    for (i32 k = 0; k < MINI_HISTORY_COUNT; k++) {
        for (i32 i = 0; i < mb->histories[k].count; i++) items[n++] = &mb->histories[k].items[i];
    }
    for (i32 i = 1; i < n; i++) { // by address, so every move goes down (insertion sort: at most 600)
        String8 *x = items[i];
        i32 j = i;
        for (; j > 0 && items[j - 1]->data > x->data; j--) items[j] = items[j - 1];
        items[j] = x;
    }
    u64 pos = 0;
    for (i32 i = 0; i < n; i++) {
        memmove(a->base + pos, items[i]->data, (size_t)items[i]->len);
        items[i]->data = a->base + pos;
        pos += (u64)items[i]->len;
    }
    arena_pop_to(a, pos);
}

void minibuffer_history_add(Minibuffer *mb, MiniHistoryKind kind, String8 text) {
    if (kind <= MINI_HISTORY_NONE || kind >= MINI_HISTORY_COUNT || !text.len) return;
    MiniHistory *h = &mb->histories[kind];
    if (h->count && str8_equal(h->items[0], text)) return;
    if (h->count == MINI_HISTORY_MAX) mb->history_live -= (u64)h->items[--h->count].len;
    memmove(h->items + 1, h->items, (size_t)h->count * sizeof(String8));
    h->items[0] = str8_copy(&mb->history_arena, text);
    h->count++;
    mb->history_live += (u64)text.len;
    minibuffer_history_compact(mb);
}

// ---------------------------------------------------------------------------
// Commands

// A transient note after the input, as Emacs' minibuffer-message: "[No match]". Not logged.
static void minibuffer_note(CommandContext *ctx, const char *text) {
    echo_set(ctx->echo, str8_cstr(text));
}

// The minibuffer, when the command runs in it; otherwise "Not in a minibuffer".
static Minibuffer *minibuffer_here(CommandContext *ctx) {
    Minibuffer *mb = ctx->mini;
    if (!mb || !mb->active || ctx->view != mb->view) {
        echo_message(ctx->echo, "Not in a minibuffer");
        return NULL;
    }
    return mb;
}

// "N" or "N:M", decimal, blanks around it allowed.
static b32 minibuffer_parse_numbers(String8 s, MiniResult *r) {
    i64 i = 0;
    while (i < s.len && s.data[i] == ' ') i++;
    r->numbers = 0;
    for (;;) {
        i64 start = i, v = 0;
        while (i < s.len && s.data[i] >= '0' && s.data[i] <= '9' && i - start < 18) v = v * 10 + (s.data[i++] - '0');
        if (i == start || (i < s.len && s.data[i] >= '0' && s.data[i] <= '9')) return 0; // no digits, or too many
        r->number[r->numbers++] = v;
        if (i < s.len && s.data[i] == ':' && r->numbers == 1) {
            i++;
            continue;
        }
        break;
    }
    while (i < s.len && s.data[i] == ' ') i++;
    return i == s.len;
}

// The candidate whose text equals the matched part of the input (case-insensitively), or -1.
static i64 minibuffer_exact_candidate(Minibuffer *mb, String8 input) {
    u64 mark = arena_pos(&mb->match_arena);
    String8 want = match_fold(&mb->match_arena, str8(input.data + mb->match_from, input.len - mb->match_from));
    i64 found = -1;
    for (i64 i = 0; i < mb->cand_count && found < 0; i++) if (str8_equal(mb->cands[i].folded, want)) found = i;
    arena_pop_to(&mb->match_arena, mark);
    return found;
}

// RET (as_typed = 0) and C-j (as_typed = 1).
static void minibuffer_accept(CommandContext *ctx, b32 as_typed) {
    Minibuffer *mb = minibuffer_here(ctx);
    if (!mb || mb->kind == MINI_KEY) return;
    String8 input = minibuffer_input(mb, &mb->arena);
    switch (mb->kind) {
    case MINI_TEXT:
        minibuffer_finish(mb, input, -1, 0);
        break;
    case MINI_NUMBER: {
        MiniResult r = { 0 };
        if (!minibuffer_parse_numbers(input, &r)) {
            minibuffer_note(ctx, "Please enter a number");
            return;
        }
        minibuffer_finish(mb, input, -1, 0);
        mb->result.numbers = r.numbers;
        mb->result.number[0] = r.number[0];
        mb->result.number[1] = r.number[1];
    } break;
    case MINI_YES_NO: {
        String8 answer = match_fold(&mb->arena, input);
        b32 yes = str8_equal(answer, STR8_LIT("yes"));
        if (!yes && !str8_equal(answer, STR8_LIT("no"))) {
            minibuffer_set_input(mb, str8(NULL, 0));
            minibuffer_note(ctx, "Please answer yes or no");
            return;
        }
        minibuffer_finish(mb, input, -1, 0);
        mb->result.yes = yes;
    } break;
    case MINI_CHOICE: {
        i64 index = -1;
        if (!as_typed && mb->match_count > 0) index = mb->matches[mb->selected];
        else if (mb->require_match) index = minibuffer_exact_candidate(mb, input);
        if (index < 0) {
            if (mb->require_match) {
                minibuffer_note(ctx, "No match");
                return;
            }
            minibuffer_finish(mb, input, -1, 0);
            return;
        }
        Candidate *c = &mb->cands[index];
        String8 text = str8_fmt(&mb->arena, "%S%S", str8(input.data, mb->match_from), c->text);
        if (mb->file && (c->flags & CANDIDATE_DIR) && !as_typed) { // a directory: descend into it
            minibuffer_set_input(mb, str8_fmt(&mb->arena, "%S/", text));
            return;
        }
        minibuffer_finish(mb, text, (i32)index, c->flags);
    } break;
    case MINI_KEY:
        break;
    }
}

static void cmd_exit_minibuffer(CommandContext *ctx) { minibuffer_accept(ctx, 0); }
static void cmd_exit_minibuffer_input(CommandContext *ctx) { minibuffer_accept(ctx, 1); }

static void cmd_abort_minibuffers(CommandContext *ctx) {
    Minibuffer *mb = minibuffer_here(ctx);
    if (!mb) return;
    minibuffer_abort(mb);
    echo_message(ctx->echo, "Quit");
}

// MINI_KEY: the answer is ctx->codepoint.
static void cmd_minibuffer_answer(CommandContext *ctx) {
    Minibuffer *mb = ctx->mini;
    u8 bytes[4];
    i64 n = utf8_encode(ctx->codepoint, bytes);
    minibuffer_finish(mb, str8(bytes, n), -1, 0);
    mb->result.key = ctx->codepoint;
}

static const Command CMD_MINIBUFFER_ANSWER = { "minibuffer-answer", cmd_minibuffer_answer, COMMAND_ONCE }; // internal

void minibuffer_key(CommandContext *ctx, const Command *command, u32 chord_char) {
    Minibuffer *mb = ctx->mini;
    if (command && (command->flags & COMMAND_QUIT)) { // first: no answer can shadow an abort
        view_run_command(ctx, &CMD_ABORT_MINIBUFFERS);
        return;
    }
    u32 key = unicode_lower(chord_char);
    for (const char *a = mb->answers; key && a && *a; a++) {
        if ((u32)(u8)*a == key) {
            ctx->codepoint = key;
            view_run_command(ctx, &CMD_MINIBUFFER_ANSWER);
            return;
        }
    }
    // "Please answer y or n", "Please answer y, n, ! or q"
    u8 text[128];
    i64 len = fmt_buf(text, sizeof(text), "Please answer ");
    i32 count = 0;
    for (const char *a = mb->answers; a && *a; a++) count++;
    for (i32 i = 0; i < count && len < (i64)sizeof(text) - 8; i++) {
        if (i) len += fmt_buf(text + len, (i64)sizeof(text) - len, i == count - 1 ? " or " : ", ");
        text[len++] = (u8)mb->answers[i];
    }
    echo_set(ctx->echo, str8(text, len));
}

// M-p (dir 1, older) and M-n (dir -1, newer). Going past the newest gives back what was typed.
static void minibuffer_history_step(CommandContext *ctx, i32 dir) {
    Minibuffer *mb = minibuffer_here(ctx);
    if (!mb) return;
    MiniHistory *h = mb->history > MINI_HISTORY_NONE && mb->history < MINI_HISTORY_COUNT ? &mb->histories[mb->history] : NULL;
    i32 pos = mb->history_pos + dir;
    if (pos >= (h ? h->count : 0)) {
        minibuffer_note(ctx, "Beginning of history; no preceding item");
        return;
    }
    if (pos < -1) {
        minibuffer_note(ctx, "End of history; no default available");
        return;
    }
    if (mb->history_pos == -1) mb->typed = minibuffer_input(mb, &mb->arena);
    mb->history_pos = pos;
    minibuffer_set_input(mb, pos < 0 ? mb->typed : h->items[pos]);
}

static void cmd_previous_history_element(CommandContext *ctx) { minibuffer_history_step(ctx, 1); }
static void cmd_next_history_element(CommandContext *ctx) { minibuffer_history_step(ctx, -1); }

static b32 minibuffer_is_slash(u8 b) { return b == '/' || b == '\\'; }

// DEL in a file name prompt: directly after a slash it removes the whole last component
// ("c:/a/src/" -> "c:/a/"); otherwise it is delete-backward-char.
static void cmd_minibuffer_backward_updir(CommandContext *ctx) {
    Minibuffer *mb = ctx->mini;
    if (mb && mb->active && mb->file && ctx->view == mb->view) {
        Buffer *buf = mb->buffer;
        i64 p = view_point(ctx->view, ctx->cursor);
        if (p > 0 && minibuffer_is_slash(buffer_byte(buf, p - 1))) {
            i64 s = p - 1;
            while (s > 0 && !minibuffer_is_slash(buffer_byte(buf, s - 1))) s--;
            if (s > 0) {
                buffer_replace(buf, s, p, STR8_LIT(""));
                return;
            }
        }
    }
    CMD_DELETE_BACKWARD_CHAR.fn(ctx);
}

const Command CMD_EXIT_MINIBUFFER           = { "exit-minibuffer", cmd_exit_minibuffer, COMMAND_ONCE };
const Command CMD_EXIT_MINIBUFFER_INPUT     = { "exit-minibuffer-input", cmd_exit_minibuffer_input, COMMAND_ONCE };
const Command CMD_ABORT_MINIBUFFERS         = { "abort-minibuffers", cmd_abort_minibuffers, COMMAND_ONCE | COMMAND_QUIT };
const Command CMD_PREVIOUS_HISTORY_ELEMENT  = { "previous-history-element", cmd_previous_history_element, COMMAND_ONCE };
const Command CMD_NEXT_HISTORY_ELEMENT      = { "next-history-element", cmd_next_history_element, COMMAND_ONCE };
const Command CMD_MINIBUFFER_BACKWARD_UPDIR = { "minibuffer-backward-updir", cmd_minibuffer_backward_updir,
                                                COMMAND_EDIT | COMMAND_MERGE_DELETE | COMMAND_REGION_DELETE };

// ---------------------------------------------------------------------------
// Completion commands

// The first match shown so that the selection is among the `lines` rows of the list.
i64 minibuffer_list_top(Minibuffer *mb, i32 lines) {
    lines = MAX(lines, 1);
    if (mb->selected < mb->list_top) mb->list_top = mb->selected;
    if (mb->selected >= mb->list_top + lines) mb->list_top = mb->selected - lines + 1;
    mb->list_top = CLAMP(mb->list_top, 0, MAX(mb->match_count - lines, 0));
    return mb->list_top;
}

// A choice prompt (else a note and NULL).
static Minibuffer *minibuffer_choice_here(CommandContext *ctx) {
    Minibuffer *mb = minibuffer_here(ctx);
    if (mb && mb->kind != MINI_CHOICE) {
        minibuffer_note(ctx, "No completions");
        return NULL;
    }
    return mb;
}

// Replaces the matched part of the input by a candidate's text (with a slash for a directory).
static void minibuffer_complete_to(Minibuffer *mb, String8 input, String8 text, b32 slash) {
    minibuffer_set_input(mb, str8_fmt(&mb->arena, slash ? "%S%S/" : "%S%S", str8(input.data, mb->match_from), text));
}

// TAB: first to the longest common prefix of the matches (when the input is a prefix of it), then
// to the selected candidate; a directory descends. "[Sole completion]" when nothing is left to do.
static void cmd_minibuffer_complete(CommandContext *ctx) {
    Minibuffer *mb = minibuffer_choice_here(ctx);
    if (!mb) return;
    if (!mb->match_count) {
        minibuffer_note(ctx, "No match");
        return;
    }
    String8 input = minibuffer_input(mb, &mb->arena);
    String8 part = match_fold(&mb->arena, str8(input.data + mb->match_from, input.len - mb->match_from));
    Candidate *first = &mb->cands[mb->matches[0]];
    i64 lcp = first->folded.len;
    for (i64 k = 1; k < mb->match_count && lcp > 0; k++) {
        Candidate *c = &mb->cands[mb->matches[k]];
        i64 n = 0;
        while (n < lcp && n < c->folded.len && c->folded.data[n] == first->folded.data[n]) n++;
        lcp = n;
    }
    while (lcp > 0 && lcp < first->text.len && (first->text.data[lcp] & 0xC0) == 0x80) lcp--; // a character boundary
    if (lcp > part.len && str8_starts_with(first->folded, part)) {
        minibuffer_complete_to(mb, input, str8(first->text.data, lcp), 0);
        return;
    }
    Candidate *sel = &mb->cands[mb->matches[mb->selected]];
    b32 dir = mb->file && (sel->flags & CANDIDATE_DIR);
    if (str8_equal(sel->folded, part) && !dir) {
        minibuffer_note(ctx, mb->match_count == 1 ? "Sole completion" : "Complete, but not unique");
        return;
    }
    minibuffer_complete_to(mb, input, sel->text, dir);
}

static void minibuffer_move_selection(CommandContext *ctx, i64 by) {
    Minibuffer *mb = minibuffer_choice_here(ctx);
    if (!mb || !mb->match_count) return;
    mb->selected = CLAMP(mb->selected + by, 0, mb->match_count - 1);
    minibuffer_list_top(mb, ctx->settings->completion_lines);
}

static void cmd_minibuffer_next_completion(CommandContext *ctx) { minibuffer_move_selection(ctx, 1); }
static void cmd_minibuffer_previous_completion(CommandContext *ctx) { minibuffer_move_selection(ctx, -1); }
static void cmd_minibuffer_next_page(CommandContext *ctx) { minibuffer_move_selection(ctx, ctx->settings->completion_lines); }
static void cmd_minibuffer_previous_page(CommandContext *ctx) { minibuffer_move_selection(ctx, -ctx->settings->completion_lines); }

const Command CMD_MINIBUFFER_COMPLETE            = { "minibuffer-complete", cmd_minibuffer_complete, COMMAND_ONCE };
const Command CMD_MINIBUFFER_NEXT_COMPLETION     = { "minibuffer-next-completion", cmd_minibuffer_next_completion, COMMAND_ONCE };
const Command CMD_MINIBUFFER_PREVIOUS_COMPLETION = { "minibuffer-previous-completion", cmd_minibuffer_previous_completion, COMMAND_ONCE };
const Command CMD_MINIBUFFER_NEXT_PAGE           = { "minibuffer-next-page", cmd_minibuffer_next_page, COMMAND_ONCE };
const Command CMD_MINIBUFFER_PREVIOUS_PAGE       = { "minibuffer-previous-page", cmd_minibuffer_previous_page, COMMAND_ONCE };

// ---------------------------------------------------------------------------
// M-x

static b32 minibuffer_name_less(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (u8)*a < (u8)*b;
}

// The shortest global binding of a command, printed ("C-x C-f"), or empty.
static String8 minibuffer_binding(Arena *arena, const Keymap *map, const Command *cmd) {
    const KeyBinding *best = NULL;
    for (i32 i = 0; map && i < map->count; i++) {
        const KeyBinding *b = &map->bindings[i];
        if (b->command == cmd && (!best || b->seq.len < best->seq.len)) best = b;
    }
    if (!best) return str8(NULL, 0);
    u8 *text = PUSH_ARRAY(arena, u8, KEY_SEQ_TEXT_CAP);
    KeySeq seq = best->seq;
    return str8(text, key_seq_print(&seq, text, KEY_SEQ_TEXT_CAP));
}

// Every command, alphabetically, annotated with its key binding. Built once per prompt.
static i64 minibuffer_command_candidates(Minibuffer *mb, void *data, String8 input) {
    (void)input;
    if (mb->cand_count) return 0;
    i32 n = command_count();
    const Command *sorted[256];
    n = MIN(n, (i32)ARRAY_COUNT(sorted));
    for (i32 i = 0; i < n; i++) { // insertion sort by name: a few dozen commands, once per prompt
        const Command *c = command_at(i);
        i32 j = i;
        for (; j > 0 && minibuffer_name_less(c->name, sorted[j - 1]->name); j--) sorted[j] = sorted[j - 1];
        sorted[j] = c;
    }
    u64 mark = arena_pos(&mb->match_arena);
    for (i32 i = 0; i < n; i++) {
        String8 binding = minibuffer_binding(&mb->match_arena, (const Keymap *)data, sorted[i]);
        minibuffer_add_candidate(mb, str8_cstr(sorted[i]->name), binding, 0);
    }
    arena_pop_to(&mb->match_arena, mark);
    return 0;
}

static void cmd_execute_extended_command(CommandContext *ctx) {
    MiniRequest req = { .kind = MINI_CHOICE, .prompt = STR8_LIT("M-x "), .history = MINI_HISTORY_COMMAND,
                        .candidates = minibuffer_command_candidates, .data = (void *)ctx->global, .require_match = 1,
                        .run_command = 1 };
    minibuffer_read(ctx, &req);
}

const Command CMD_EXECUTE_EXTENDED_COMMAND = { "execute-extended-command", cmd_execute_extended_command, COMMAND_ONCE };

// ---------------------------------------------------------------------------
// goto-line: LINE or LINE:COLUMN, 1-based as on the command line (COLUMN is a visual column); both
// are clamped. The driver recenters the window when the line is off screen.

static void minibuffer_goto_line_done(CommandContext *ctx, MiniResult *r) {
    i64 line = MAX(r->number[0], 1) - 1;
    i64 col = r->numbers == 2 ? MAX(r->number[1], 1) - 1 : 0;
    view_deactivate_mark(ctx->view);
    view_goto_line_column(ctx->view, line, col);
}

static void cmd_goto_line(CommandContext *ctx) {
    MiniRequest req = { .kind = MINI_NUMBER, .prompt = STR8_LIT("Goto line: "), .history = MINI_HISTORY_LINE,
                        .done = minibuffer_goto_line_done };
    minibuffer_read(ctx, &req);
}

const Command CMD_GOTO_LINE = { "goto-line", cmd_goto_line, COMMAND_ONCE };
