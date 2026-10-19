/* ======================================================================
   ronin: the command list (C-S-p): every command, filtered, run on RET

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// show-command-list (Phase 18): every command of the table in a modal box, with its description
// and its key bindings in the active keymaps; the field filters it with the fuzzy matcher, RET runs the selected
// command on the selected window as M-x does. Part of the app (included after modal.c). Its own file rather than
// command.c: command.c is the bare table (headless, compiled for size); this needs the app, drawing, fuzzy.c and
// the config's keymaps.
//
// Everything lives in the modal's arena, made at the open: the rows (name, description, "name description", the
// key text) and the shown order. A keystroke allocates nothing: it scores the rows again into the same arrays.

#define CMDLIST_DESC_PENALTY 32 // a match in the description (or across name and description) ranks below one in the name
#define CMDLIST_KEYS_SHOWN 2    // sequences shown per command; more are "+N"
#define CMDLIST_QUERY_CAP 256   // the filter's bytes used (the field may hold more)

typedef enum CmdListField { CMDLIST_NAME, CMDLIST_DESCRIPTION, CMDLIST_BOTH } CmdListField;

typedef struct CmdRow {
    const Command *cmd;
    String8 name, description;
    String8 both;   // "name description": a query split across the two matches here
    String8 keys;   // "C-f, <right>", "M-g g, M-g M-g +1", empty when unbound
    u64 name_mask, desc_mask; // project_mask of each: a query with a character neither has is not scored (fuzzy.h)
    i32 score;      // the last filter's
    u8 won;         // CmdListField: where the score came from (the spans are drawn there)
} CmdRow;

typedef struct CmdList {
    CmdRow *rows;
    i32 count;
    i32 *shown;     // row indices, best first (table order for an empty filter)
    i32 shown_count;
    i32 name_cells; // the longest name
    u8 query[CMDLIST_QUERY_CAP];
    i32 query_len;
} CmdList;

static Keymap *cmdlist_keymap(App *app, void *data) {
    (void)data;
    return &app->config->command_list;
}

// The bindings of every row in the active view's keymaps (its buffer's keymap, then the global one): computed at the
// open and when the config is applied again, never per frame.
static void cmdlist_where_is(App *app, CmdList *cl) {
    Arena *a = modal_arena(app);
    Keymap *stack[2];
    i32 count = 0;
    Keymap *own = app_buffer_keymap(app, app_selected_view(app)->buffer);
    if (own) stack[count++] = own;
    stack[count++] = &app->config->global;
    for (i32 i = 0; i < cl->count; i++) {
        KeySeq seqs[CMDLIST_KEYS_SHOWN];
        i32 total = keymap_where_is(stack, count, cl->rows[i].cmd, seqs, CMDLIST_KEYS_SHOWN);
        u8 text[CMDLIST_KEYS_SHOWN * (KEY_SEQ_TEXT_CAP + 2) + 16];
        i64 n = 0;
        for (i32 k = 0; k < MIN(total, CMDLIST_KEYS_SHOWN); k++) {
            if (k) { text[n++] = ','; text[n++] = ' '; }
            n += key_seq_print(&seqs[k], text + n, KEY_SEQ_TEXT_CAP);
        }
        if (total > CMDLIST_KEYS_SHOWN) n += fmt_buf(text + n, (i64)sizeof(text) - n, " +%d", total - CMDLIST_KEYS_SHOWN);
        cl->rows[i].keys = str8_copy(a, str8(text, n));
    }
}

// The score of a row for a query (FUZZY_NO_MATCH: none) and where it came from.
static i32 cmdlist_score(FuzzyQuery *q, CmdRow *row, u8 *won) {
    *won = CMDLIST_NAME;
    if (((row->name_mask | row->desc_mask) & q->mask) != q->mask) return FUZZY_NO_MATCH; // not even "name description"
    i32 best = (row->name_mask & q->mask) == q->mask ? fuzzy_score(q, row->name, 0, NULL, 0, NULL) : FUZZY_NO_MATCH;
    i32 d = (row->desc_mask & q->mask) == q->mask ? fuzzy_score(q, row->description, 0, NULL, 0, NULL) : FUZZY_NO_MATCH;
    if (d != FUZZY_NO_MATCH && (best == FUZZY_NO_MATCH || d - CMDLIST_DESC_PENALTY > best)) {
        best = d - CMDLIST_DESC_PENALTY;
        *won = CMDLIST_DESCRIPTION;
    }
    i32 b = fuzzy_score(q, row->both, 0, NULL, 0, NULL);
    if (b != FUZZY_NO_MATCH && (best == FUZZY_NO_MATCH || b - CMDLIST_DESC_PENALTY > best)) {
        best = b - CMDLIST_DESC_PENALTY;
        *won = CMDLIST_BOTH;
    }
    return best;
}

// The filter: every row scored again, the matches ranked by score, then table order. The empty filter lists the
// whole table in its order.
static void cmdlist_filter(App *app, CmdList *cl, String8 text) {
    cl->query_len = (i32)MIN(text.len, (i64)CMDLIST_QUERY_CAP);
    memcpy(cl->query, text.data, (size_t)cl->query_len);
    String8 input = str8(cl->query, cl->query_len);
    cl->shown_count = 0;
    if (!input.len) {
        for (i32 i = 0; i < cl->count; i++) cl->shown[cl->shown_count++] = i;
    } else {
        Arena *scratch = app->ctx.scratch;
        u64 mark = arena_pos(scratch);
        FuzzyQuery q = fuzzy_query(scratch, input);
        for (i32 i = 0; i < cl->count; i++) {
            CmdRow *row = &cl->rows[i];
            row->score = cmdlist_score(&q, row, &row->won);
            if (row->score == FUZZY_NO_MATCH) continue;
            i32 at = cl->shown_count++; // insertion: better scores first, equal ones in table order
            while (at > 0 && cl->rows[cl->shown[at - 1]].score < row->score) {
                cl->shown[at] = cl->shown[at - 1];
                at--;
            }
            cl->shown[at] = i;
        }
        arena_pop_to(scratch, mark);
    }
    modal_set_rows(app, 0, cl->shown_count);
}

static void cmdlist_field_changed(App *app, void *data, String8 text) {
    cmdlist_filter(app, data, text);
}

static void cmdlist_accept(App *app, void *data, i32 pane, i64 row) {
    CmdList *cl = data;
    (void)pane;
    if (row < 0 || row >= cl->shown_count) return;
    modal_run_after_close(app, cl->rows[cl->shown[row]].cmd);
}

static String8 cmdlist_status(App *app, void *data, Arena *scratch) {
    (void)app;
    CmdList *cl = data;
    return str8_fmt(scratch, "%d/%d", cl->shown_count, cl->count);
}

static void cmdlist_reload(App *app, void *data) {
    cmdlist_where_is(app, data);
}

// `text` from x, at most `cells` cells, in `rgb`, its matched spans in completion_match. Returns the x after it.
static i32 cmdlist_draw_text(App *app, Renderer *r, i32 x, i32 y, String8 text, MatchSpan *spans, i32 n, i64 cells, u32 rgb) {
    text = app_clip_cells(text, MAX(cells, 0));
    i64 at = 0;
    while (at < text.len) {
        b32 matched = 0;
        i64 next = text.len;
        for (i32 k = 0; k < n; k++) {
            if (spans[k].start <= at && at < spans[k].end) {
                matched = 1;
                next = MIN(next, spans[k].end);
            } else if (spans[k].start > at) {
                next = MIN(next, spans[k].start);
            }
        }
        next = MIN(next, text.len);
        x = font_draw_text(app->font, r, x, y, str8(text.data + at, next - at), COLOR_HEX(matched ? app->config->theme.completion_match : rgb));
        at = next;
    }
    return x;
}

// The matched characters of a drawn row, split into its name's and its description's (spans in "name description"
// are moved to the field they fall in; one across the space is cut there).
static void cmdlist_spans(App *app, CmdList *cl, CmdRow *row, Arena *scratch, MatchSpan *name, i32 *nn, MatchSpan *desc, i32 *nd) {
    *nn = *nd = 0;
    if (!cl->query_len) return;
    (void)app;
    FuzzyQuery q = fuzzy_query(scratch, str8(cl->query, cl->query_len));
    MatchSpan spans[64];
    i32 n = 0;
    String8 target = row->won == CMDLIST_NAME ? row->name : row->won == CMDLIST_DESCRIPTION ? row->description : row->both;
    fuzzy_score(&q, target, 0, spans, (i32)ARRAY_COUNT(spans), &n);
    for (i32 k = 0; k < n; k++) {
        MatchSpan s = spans[k];
        if (row->won == CMDLIST_NAME) {
            name[(*nn)++] = s;
        } else if (row->won == CMDLIST_DESCRIPTION) {
            desc[(*nd)++] = s;
        } else {
            i64 cut = row->name.len; // the space
            if (s.start < cut) name[(*nn)++] = (MatchSpan){ s.start, MIN(s.end, cut) };
            if (s.end > cut + 1) desc[(*nd)++] = (MatchSpan){ MAX(s.start, cut + 1) - cut - 1, s.end - cut - 1 };
        }
    }
}

// A row: the name (in a column as wide as the longest name, at most 40% of the row), the description, the keys
// right-aligned. The keys come first: the description and then the name give way to them.
static void cmdlist_draw_row(App *app, void *data, Renderer *r, ModalRow *mr) {
    CmdList *cl = data;
    if (mr->row < 0 || mr->row >= cl->shown_count) return;
    CmdRow *row = &cl->rows[cl->shown[mr->row]];
    Theme *th = &app->config->theme;
    i32 cw = app->font->cell_w, y = (i32)mr->rect.y0;
    i64 cols = mr->cols;
    i64 key_cells = MIN(app_text_cells(row->keys), cols);
    i64 left = key_cells ? cols - key_cells - 2 : cols; // the cells before the keys
    i64 name_w = MIN((i64)cl->name_cells, cols * 40 / 100);
    Arena *scratch = app->ctx.scratch; // the frame's
    MatchSpan name_spans[64], desc_spans[64];
    i32 nn = 0, nd = 0;
    u64 mark = 0;
    if (scratch) {
        mark = arena_pos(scratch);
        cmdlist_spans(app, cl, row, scratch, name_spans, &nn, desc_spans, &nd);
    }
    if (key_cells) font_draw_text(app->font, r, mr->x + (i32)(cols - key_cells) * cw, y, app_clip_cells(row->keys, key_cells), COLOR_HEX(th->modal_key));
    cmdlist_draw_text(app, r, mr->x, y, row->name, name_spans, nn, MIN(name_w, left), th->text);
    i64 desc_cells = left - name_w - 2;
    if (desc_cells > 0) {
        cmdlist_draw_text(app, r, mr->x + (i32)(name_w + 2) * cw, y, row->description, desc_spans, nd, desc_cells, th->modal_description);
    }
    if (scratch) arena_pop_to(scratch, mark);
}

static void cmd_show_command_list(CommandContext *ctx) {
    App *app = ctx->app;
    if (!app) {
        echo_message(ctx->echo, "No command list here");
        return;
    }
    ModalSpec spec = {
        .name = STR8_LIT("command list"), .keymap = cmdlist_keymap, .field = 1, .panes = 1,
        .field_changed = cmdlist_field_changed, .accept = cmdlist_accept, .draw_row = cmdlist_draw_row,
        .status = cmdlist_status, .reload = cmdlist_reload,
    };
    if (!modal_open(app, &spec)) return;
    Arena *a = modal_arena(app);
    CmdList *cl = PUSH_STRUCT(a, CmdList);
    cl->count = command_count();
    cl->rows = PUSH_ARRAY(a, CmdRow, cl->count);
    cl->shown = PUSH_ARRAY(a, i32, cl->count);
    for (i32 i = 0; i < cl->count; i++) {
        CmdRow *row = &cl->rows[i];
        row->cmd = command_at(i);
        row->name = str8_cstr(row->cmd->name);
        row->description = str8_cstr(row->cmd->description ? row->cmd->description : "");
        row->both = str8_fmt(a, "%S %S", row->name, row->description);
        row->name_mask = project_mask(row->name);
        row->desc_mask = project_mask(row->description);
        cl->name_cells = (i32)MAX((i64)cl->name_cells, app_text_cells(row->name));
    }
    cmdlist_where_is(app, cl);
    modal_set_data(app, cl);
    cmdlist_filter(app, cl, str8(NULL, 0));
}

const Command CMD_SHOW_COMMAND_LIST = { "show-command-list", "List every command with its keys and run one", cmd_show_command_list, COMMAND_ONCE };
