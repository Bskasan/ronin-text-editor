// syntax.c — see syntax.h. The helpers every lexer shares (tokens, character classes, keyword
// tables, numbers), the catch-up of line states and the language table. The lexers are in
// lex_*.c, included after this file.

// ---------------------------------------------------------------------------
// Helpers for the lexers

// Starts a token at `start`. Zero-length tokens are overwritten; runs of punctuation are one
// token. When the array is about to fill up, the rest of the line becomes plain text.
static void syn_emit(SyntaxTokens *out, i64 start, SyntaxKind kind) {
    if (!out || out->count >= out->cap) return;
    if (out->count > 0) {
        SyntaxToken *last = &out->tokens[out->count - 1];
        if ((i64)last->start == start) {
            last->kind = kind;
            return;
        }
        if (last->kind == (u32)kind && kind == SYN_PUNCTUATION) return;
    }
    if (out->count == out->cap - 1) kind = SYN_TEXT;
    out->tokens[out->count++] = (SyntaxToken){ (u32)start, (u32)kind };
}

static b32 syn_is_blank(u8 c) {
    return c == ' ' || c == '\t' || c == '\f' || c == '\v' || c == '\r';
}

static b32 syn_is_digit(u8 c) {
    return c >= '0' && c <= '9';
}

static b32 syn_is_alpha(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c >= 0x80;
}

// Identifier bytes: ASCII letters, digits, '_' and every byte >= 0x80 (non-ASCII identifiers, and
// invalid bytes, which then never confuse a lexer).
static b32 syn_is_ident(u8 c) {
    return syn_is_alpha(c) || syn_is_digit(c);
}

static b32 syn_is_hex(u8 c) {
    return syn_is_digit(c) || ((c | 0x20) >= 'a' && (c | 0x20) <= 'f');
}

// ASCII bytes that can start no token of any of our languages: control characters.
static b32 syn_is_control(u8 c) {
    return (c < 0x20 && c != '\t') || c == 0x7F;
}

static i64 syn_skip_blanks(u8 *s, i64 n, i64 i) {
    while (i < n && syn_is_blank(s[i])) i++;
    return i;
}

static i64 syn_ident_end(u8 *s, i64 n, i64 i) {
    while (i < n && syn_is_ident(s[i])) i++;
    return i;
}

static b32 syn_word_is(u8 *s, i64 len, const char *word) {
    i64 i = 0;
    for (; i < len; i++) if (!word[i] || (u8)word[i] != s[i]) return 0;
    return word[i] == 0;
}

static u32 syn_hash(u8 *s, i64 len) { // FNV-1a
    u32 h = 2166136261u;
    for (i64 i = 0; i < len; i++) h = (h ^ s[i]) * 16777619u;
    return h;
}

// A number starting at i (a digit, or '.' then a digit): prefixes 0x / 0b / 0o (and 0h when
// `zero_h`), digits with the separator `sep` (0 = none), a fraction, an exponent, then any
// identifier bytes as the suffix (u, ull, f, m, n, _km ...). A '.' followed by another '.' is a
// range, not a fraction. Returns the end.
static i64 syn_number(u8 *s, i64 n, i64 i, u8 sep, b32 zero_h) {
    b32 hex = 0;
    if (s[i] == '0' && i + 1 < n) {
        u8 p = s[i + 1] | 0x20;
        if (p == 'x' || (zero_h && p == 'h')) {
            hex = 1;
            i += 2;
        } else if ((p == 'b' || p == 'o') && i + 2 < n && syn_is_digit(s[i + 2])) {
            i += 2;
        }
    }
    for (;;) {
        while (i < n && (hex ? syn_is_hex(s[i]) : syn_is_digit(s[i]))) i++;
        if (sep && i + 1 < n && s[i] == sep && (hex ? syn_is_hex(s[i + 1]) : syn_is_digit(s[i + 1]))) {
            i++;
            continue;
        }
        break;
    }
    if (i < n && s[i] == '.' && !(i + 1 < n && s[i + 1] == '.')) {
        i++;
        for (;;) {
            while (i < n && (hex ? syn_is_hex(s[i]) : syn_is_digit(s[i]))) i++;
            if (sep && i + 1 < n && s[i] == sep && syn_is_digit(s[i + 1])) {
                i++;
                continue;
            }
            break;
        }
    }
    if (i < n && ((s[i] | 0x20) == (hex ? 'p' : 'e'))) {
        i64 j = i + 1;
        if (j < n && (s[j] == '+' || s[j] == '-')) j++;
        if (j < n && syn_is_digit(s[j])) {
            i = j;
            while (i < n && (syn_is_digit(s[i]) || (sep && s[i] == sep))) i++;
        }
    }
    return syn_ident_end(s, n, i);
}

// Keyword tables: open addressing over a hash of the word, filled once by syntax_init. A lookup
// allocates nothing.
#define SYN_TABLE_SIZE 512
#define SYN_WORD_MAX 24

typedef struct SynWord {
    const char *text; // its first `len` bytes, in a static list
    u8 len, kind;
} SynWord;

typedef struct SynTable {
    SynWord slots[SYN_TABLE_SIZE];
    i32 count;
} SynTable;

static b32 syn_lookup_slot(SynWord *w, u8 *s, i64 len) {
    if (w->len != len) return 0;
    for (i64 i = 0; i < len; i++) if ((u8)w->text[i] != s[i]) return 0;
    return 1;
}

// Adds the space-separated words of `list` with `kind`.
static void syn_table_add(SynTable *t, const char *list, SyntaxKind kind) {
    for (const char *p = list; *p;) {
        while (*p == ' ') p++;
        const char *w = p;
        while (*p && *p != ' ') p++;
        i64 len = p - w;
        if (!len) continue;
        ASSERT(len <= SYN_WORD_MAX && t->count < SYN_TABLE_SIZE * 3 / 4);
        u32 h = syn_hash((u8 *)w, len) & (SYN_TABLE_SIZE - 1);
        while (t->slots[h].text && syn_lookup_slot(&t->slots[h], (u8 *)w, len) == 0) h = (h + 1) & (SYN_TABLE_SIZE - 1);
        if (!t->slots[h].text) t->count++;
        // The words are static strings: a word is its first `len` bytes of the list.
        t->slots[h] = (SynWord){ w, (u8)len, (u8)kind };
    }
}

static SyntaxKind syn_lookup(SynTable *t, u8 *s, i64 len, SyntaxKind otherwise) {
    if (len > SYN_WORD_MAX) return otherwise;
    u32 h = syn_hash(s, len) & (SYN_TABLE_SIZE - 1);
    while (t->slots[h].text) {
        if (syn_lookup_slot(&t->slots[h], s, len)) return (SyntaxKind)t->slots[h].kind;
        h = (h + 1) & (SYN_TABLE_SIZE - 1);
    }
    return otherwise;
}

static u32 lex_c(u32 state, u8 *s, i64 n, SyntaxTokens *out);
static void lex_c_init(void);
static u32 lex_jai(u32 state, u8 *s, i64 n, SyntaxTokens *out);
static void lex_jai_init(void);
static u32 lex_cs(u32 state, u8 *s, i64 n, SyntaxTokens *out);
static void lex_cs_init(void);
static u32 lex_js(u32 state, u8 *s, i64 n, SyntaxTokens *out);
static u32 lex_ts(u32 state, u8 *s, i64 n, SyntaxTokens *out);
static void lex_js_init(void);

// ---------------------------------------------------------------------------
// Languages

typedef u32 SyntaxLexFn(u32 state, u8 *s, i64 n, SyntaxTokens *out);

static SyntaxLexFn *syntax_lexer(BufferLanguage language) {
    switch (language) {
    case BUFFER_LANG_FUNDAMENTAL: return NULL;
    case BUFFER_LANG_JAI:         return lex_jai;
    case BUFFER_LANG_C:           return lex_c;
    case BUFFER_LANG_CPP:         return lex_c;
    case BUFFER_LANG_CSHARP:      return lex_cs;
    case BUFFER_LANG_JAVASCRIPT:  return lex_js;
    case BUFFER_LANG_TYPESCRIPT:  return lex_ts;
    }
    return NULL;
}

static b32 syntax_ready;

void syntax_init(void) {
    if (syntax_ready) return;
    lex_c_init();
    lex_jai_init();
    lex_cs_init();
    lex_js_init();
    syntax_ready = 1;
}

b32 syntax_has_lexer(BufferLanguage language) {
    return syntax_lexer(language) != NULL;
}

u32 syntax_lex(BufferLanguage language, u32 state, String8 line, SyntaxTokens *out) {
    ASSERT(syntax_ready);
    SyntaxLexFn *lex = syntax_lexer(language);
    if (out) out->count = 0;
    return lex ? lex(state, line.data, line.len, out) : 0;
}

// ---------------------------------------------------------------------------
// Line states

#if TEAL_DEV
static b32 syntax_fake_clock;
static u64 syntax_lexed, syntax_converged;

void syntax_dev_fake_clock(b32 on) { syntax_fake_clock = on; }
u64  syntax_dev_lexed(void) { return syntax_lexed; }
u64  syntax_dev_converged(void) { return syntax_converged; }

static u64 syntax_now(void) {
    return syntax_fake_clock ? syntax_lexed : os_time_us();
}
#else
#define syntax_now os_time_us
#endif

b32 syntax_attach(Buffer *buf) {
    if (!syntax_has_lexer(buf->language)) {
        if (buf->states_on) buffer_states_enable(buf, 0);
        return 0;
    }
    if (!buf->states_on || buf->states_language != (i32)buf->language) {
        if (!buffer_states_enable(buf, 1)) return 0;
        buf->states_language = (i32)buf->language;
    }
    return 1;
}

b32 syntax_line_ready(Buffer *buf, i64 line) {
    return buf->states_on && line <= buf->state_valid;
}

b32 syntax_catch_up(Buffer *buf, i64 need_line, u64 budget_us, Arena *scratch) {
    if (!syntax_attach(buf)) return 1;
    need_line = MIN(need_line, buffer_line_count(buf) - 1);
    if (buf->state_valid >= need_line) return 1;
    SyntaxLexFn *lex = syntax_lexer(buf->language);
    u64 deadline = syntax_now() + budget_us;
    i64 lines = 0, bytes = 0;
    i64 line = buf->state_valid;
    u32 state = buffer_line_state(buf, line);
    while (line < need_line) {
        u64 mark = arena_pos(scratch);
        String8 text = buffer_line(buf, scratch, line);
        u32 end = lex(state, text.data, text.len, NULL);
        arena_pop_to(scratch, mark);
#if TEAL_DEV
        syntax_lexed += (u64)text.len + 1;
#endif
        i64 next = line + 1;
        if (next > buf->state_dirty && next < buf->state_known && buffer_line_state(buf, next) == end) {
            // Converged: the stored states from here on were computed from this same state over
            // text that has not changed since.
            line = buf->state_known - 1;
            state = buffer_line_state(buf, line);
#if TEAL_DEV
            syntax_converged++;
#endif
        } else {
            buffer_set_line_state(buf, next, end);
            if (next >= buf->state_known) buf->state_known = next + 1;
            line = next;
            state = end;
        }
        buf->state_valid = line;
        if (buf->state_dirty >= 0 && buf->state_dirty <= line) buf->state_dirty = -1;
        lines++;
        bytes += text.len + 1;
        if (lines >= SYNTAX_CHECK_LINES || bytes >= SYNTAX_CHECK_BYTES) {
            if (syntax_now() >= deadline) break;
            lines = bytes = 0;
        }
    }
    // The stored states after the last one written are from an earlier pass: they need not follow
    // from it. A later pass may only take them as converged after this line.
    if (buf->state_valid < buf->state_known - 1) buf->state_dirty = MAX(buf->state_dirty, buf->state_valid);
    return buf->state_valid >= need_line;
}

// ---------------------------------------------------------------------------
// Brackets and lines on tokens

static b32 syn_opener(u8 c) {
    return c == '(' || c == '[' || c == '{';
}

static b32 syn_closer(u8 c) {
    return c == ')' || c == ']' || c == '}';
}

static u8 syn_partner(u8 c) {
    switch (c) {
    case '(': return ')';
    case ')': return '(';
    case '[': return ']';
    case ']': return '[';
    case '{': return '}';
    case '}': return '{';
    }
    return 0;
}

// The stored start state of a line, 0 when it has none yet.
static u32 syn_state_of(Buffer *buf, i64 line) {
    return buf->states_on && line < buf->state_known ? buffer_line_state(buf, line) : 0;
}

// A line lexed for brackets: its text, the kind of each byte (code, comment or string), whether
// it is a C or C++ preprocessor line, and its end state.
typedef struct SynLine {
    i64 start;
    String8 text;
    SyntaxTokens toks;
    b32 directive;
    u32 end;
} SynLine;

static void syn_line(Buffer *buf, i64 line, u32 state, Arena *scratch, SynLine *l) {
    l->start = buffer_line_start(buf, line);
    l->text = buffer_text(buf, scratch, l->start, buffer_line_end(buf, line));
    l->toks.cap = (i32)MIN(l->text.len + 2, (i64)SYNTAX_MATCH_MAX + 2);
    l->toks.tokens = PUSH_ARRAY(scratch, SyntaxToken, l->toks.cap);
    l->toks.count = 0;
    b32 lexer = syntax_has_lexer(buf->language);
    l->end = lexer ? syntax_lex(buf->language, state, l->text, &l->toks) : 0;
    l->directive = 0;
    if (buf->language == BUFFER_LANG_C || buf->language == BUFFER_LANG_CPP) {
        i64 first = syn_skip_blanks(l->text.data, l->text.len, 0);
        l->directive = (state & SYNTAX_STATE_DIRECTIVE) != 0;
        for (i32 k = 0; !l->directive && k < l->toks.count && (i64)l->toks.tokens[k].start <= first; k++) {
            l->directive = (i64)l->toks.tokens[k].start == first && l->toks.tokens[k].kind == SYN_DIRECTIVE;
        }
    }
}

// The kind of byte i of a lexed line; *k is a cursor into its tokens (bytes visited in order).
static u32 syn_kind_at(SynLine *l, i32 *k, i64 i) {
    SyntaxTokens *t = &l->toks;
    if (*k > 0 && (i64)t->tokens[*k].start > i) *k = 0;
    while (*k + 1 < t->count && (i64)t->tokens[*k + 1].start <= i) (*k)++;
    return t->count && (i64)t->tokens[*k].start <= i ? t->tokens[*k].kind : SYN_TEXT;
}

// Whether byte i is a bracket in code.
static b32 syn_code_bracket(SynLine *l, i32 *k, i64 i) {
    u8 c = l->text.data[i];
    if (l->directive || (!syn_opener(c) && !syn_closer(c))) return 0;
    u32 kind = syn_kind_at(l, k, i);
    return kind != SYN_COMMENT && kind != SYN_STRING;
}

// Walks brackets from `pos` (exclusive) forward or backward with a stack of the brackets expected
// to close the ones seen. `want` starts it: the partner of a bracket to match, or 0 to find the
// innermost unclosed opener (backward). Returns the position found, or -1.
static i64 syn_walk(Buffer *buf, i64 pos, b32 forward, u8 want, Arena *scratch) {
    u64 mark = arena_pos(scratch);
    u8 *stack = PUSH_ARRAY(scratch, u8, SYNTAX_MATCH_MAX + 1);
    i64 depth = 0;
    if (want) stack[depth++] = want;
    i64 line = buffer_line_of(buf, pos), count = buffer_line_count(buf), budget = SYNTAX_MATCH_MAX;
    u32 state = syn_state_of(buf, line);
    i64 found = -1;
    b32 done = 0;
    for (; line >= 0 && line < count && !done; line += forward ? 1 : -1) {
        u64 line_mark = arena_pos(scratch);
        i64 len = buffer_line_end(buf, line) - buffer_line_start(buf, line);
        if (len + 1 > budget) break;
        budget -= len + 1;
        SynLine l;
        syn_line(buf, line, forward ? state : syn_state_of(buf, line), scratch, &l);
        state = l.end;
        i32 k = 0;
        i64 lo = 0, hi = l.text.len;
        if (forward && l.start <= pos) lo = pos - l.start + 1;
        if (!forward && l.start + l.text.len >= pos) hi = pos - l.start;
        // The line's brackets in code, in order (the token cursor goes forward).
        i64 *at = PUSH_ARRAY(scratch, i64, hi - lo + 1);
        i64 n = 0;
        for (i64 i = lo; i < hi; i++) if (syn_code_bracket(&l, &k, i)) at[n++] = i;
        for (i64 j = 0; j < n && !done; j++) {
            i64 i = forward ? at[j] : at[n - 1 - j];
            u8 c = l.text.data[i];
            if (forward ? syn_opener(c) : syn_closer(c)) { // opens a pair (in the walking direction)
                stack[depth++] = syn_partner(c);
                continue;
            }
            if (!depth) { // nothing open: only the innermost-opener walk ends here
                found = !want ? l.start + i : -1;
                done = 1;
                continue;
            }
            if (stack[depth - 1] != c) { // a mismatched pair
                done = 1;
                continue;
            }
            if (--depth == 0 && want) {
                found = l.start + i;
                done = 1;
            }
        }
        arena_pop_to(scratch, line_mark);
    }
    arena_pop_to(scratch, mark);
    return found >= 0 ? found : -1;
}

i64 syntax_match_bracket(Buffer *buf, i64 pos, Arena *scratch) {
    if (pos < 0 || pos >= buffer_size(buf)) return -1;
    u8 c = buffer_byte(buf, pos);
    if (!syn_opener(c) && !syn_closer(c)) return -1;
    // Only a bracket in code has a match.
    u64 mark = arena_pos(scratch);
    i64 line = buffer_line_of(buf, pos);
    SynLine l;
    syn_line(buf, line, syn_state_of(buf, line), scratch, &l);
    i32 k = 0;
    b32 code = syn_code_bracket(&l, &k, pos - l.start);
    arena_pop_to(scratch, mark);
    if (!code) return -1;
    return syn_walk(buf, pos, syn_opener(c), syn_partner(c), scratch);
}

i64 syntax_enclosing_open(Buffer *buf, i64 pos, Arena *scratch) {
    return syn_walk(buf, pos, 0, 0, scratch);
}

void syntax_line_info(Buffer *buf, i64 line, Arena *scratch, SyntaxLine *info) {
    u64 mark = arena_pos(scratch);
    u32 state = syn_state_of(buf, line);
    SynLine l;
    syn_line(buf, line, state, scratch, &l);
    *info = (SyntaxLine){ .leading_closer = -1, .head = -1, .head_end = -1, .last_unmatched = -1 };
    info->directive = l.directive;
    info->in_literal = syntax_has_lexer(buf->language) && (state & SYNTAX_STATE_LITERAL);
    i32 k = 0;
    i64 depth = 0;
    b32 leading = 1; // still at the closers that start the line
    for (i64 i = 0; i < l.text.len; i++) {
        u8 c = l.text.data[i];
        if (syn_is_blank(c)) continue;
        u32 kind = syn_kind_at(&l, &k, i);
        if (kind == SYN_COMMENT) continue;
        info->code = 1;
        info->last = c;
        info->semicolons += c == ';' && kind != SYN_STRING;
        b32 bracket = syn_code_bracket(&l, &k, i);
        if (leading && bracket && syn_closer(c)) {
            if (info->leading_closer < 0) info->leading_closer = l.start + i;
        } else if (leading) {
            leading = 0;
            info->head = l.start + i;
            info->head_kind = kind;
            info->head_end = syn_is_ident(c) ? l.start + syn_ident_end(l.text.data, l.text.len, i) : l.start + i + 1;
        }
        if (!bracket) continue;
        if (syn_opener(c)) depth++;
        else if (depth) depth--;
        else info->last_unmatched = l.start + i;
    }
    info->open = (i32)MIN(depth, (i64)0x7FFFFFFF);
    arena_pop_to(scratch, mark);
}

b32 syntax_word_is(Buffer *buf, i64 start, i64 end, const char *word) {
    i64 i = 0;
    for (; start + i < end; i++) if (!word[i] || (u8)word[i] != buffer_byte(buf, start + i)) return 0;
    return word[i] == 0;
}

b32 syntax_line_tokens(Buffer *buf, i64 line, String8 text, SyntaxTokens *out) {
    out->count = 0;
    if (!buf->states_on || buf->states_language != (i32)buf->language || line >= buf->state_known) return 0;
    syntax_lex(buf->language, buffer_line_state(buf, line), text, out);
    return 1;
}
