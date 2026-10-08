// lex_cs.c — the C# lexer. Comments; regular, verbatim (@"", multi-line), interpolated ($"", $@"",
// @$"") and raw ("""...""", multi-line, $"""...""" with $ count) strings, interpolation holes as
// code; chars; numbers; keywords including the contextual ones; built-in types; preprocessor lines
// (the directive word, the rest lexed).
//
// Strings and holes nest (a string in a hole in a string ...): the lexer keeps a stack of contexts.
// State: bits 0-1 how many contexts the next line starts in (at most 3, the innermost kept), then
// one byte per context from bit 2 (see lex_cs_pack); bit 29 inside a block comment;
// SYNTAX_STATE_LITERAL when the line starts inside a comment or a string's text.

enum {
    LEX_CS_HOLE,     // code inside { } of an interpolated string
    LEX_CS_REGULAR,  // "..." or $"..." (ends at the end of the line)
    LEX_CS_VERBATIM, // @"..." or $@"...", multi-line
    LEX_CS_RAW,      // """...""" with `quotes` quotes, or $"""...""" with `dollars` dollars
};

#define LEX_CS_STACK 16 // contexts within a line
#define LEX_CS_SAVED 3  // contexts carried to the next line
#define LEX_CS_COMMENT (1u << 29)

typedef struct LexCsCtx {
    u8 kind;
    u8 interp;  // a string with holes
    u8 quotes;  // raw: the quotes that close it (3..10)
    u8 dollars; // raw: the braces that open a hole (1..3); 0 = not interpolated
    u8 depth;   // a hole: braces open inside it
} LexCsCtx;

static SynTable lex_cs_words;

static void lex_cs_init(void) {
    SynTable *t = &lex_cs_words;
    syn_table_add(t, "abstract as base break case catch checked class const continue default delegate do else enum "
                     "event explicit extern finally fixed for foreach goto if implicit in interface internal is lock "
                     "namespace new operator out override params private protected public readonly ref return sealed "
                     "sizeof stackalloc static struct switch this throw try typeof unchecked unsafe using virtual "
                     "volatile while", SYN_KEYWORD);
    syn_table_add(t, "add alias and ascending async await by descending equals file from get global group init into "
                     "join let managed nameof not notnull on or orderby partial record remove required scoped select "
                     "set unmanaged value var when where with yield", SYN_KEYWORD);
    syn_table_add(t, "bool byte sbyte char decimal double float int uint nint nuint long ulong short ushort object "
                     "string void dynamic", SYN_TYPE);
    syn_table_add(t, "true false null", SYN_CONSTANT);
}

// One byte per context: kind (bits 0-1); a hole: its depth (bits 2-7); a string: interp (bit 2),
// quotes - 3 (bits 3-5), dollars (bits 6-7).
static u32 lex_cs_pack(LexCsCtx *st, i32 count, b32 comment) {
    i32 from = MAX(count - LEX_CS_SAVED, 0);
    u32 state = (u32)(count - from);
    for (i32 k = from; k < count; k++) {
        LexCsCtx *c = &st[k];
        u32 b = c->kind == LEX_CS_HOLE ? (u32)LEX_CS_HOLE | (u32)MIN(c->depth, 63) << 2
              : c->kind | (u32)c->interp << 2 | (u32)(MIN(c->quotes, 10) - MIN(c->quotes, 3)) << 3 | (u32)MIN(c->dollars, 3) << 6;
        state |= b << (2 + 8 * (k - from));
    }
    b32 in_text = count > 0 && st[count - 1].kind != LEX_CS_HOLE;
    if (comment) state |= LEX_CS_COMMENT;
    if (comment || in_text) state |= SYNTAX_STATE_LITERAL;
    return state;
}

static i32 lex_cs_unpack(u32 state, LexCsCtx *st) {
    i32 count = (i32)(state & 3);
    for (i32 k = 0; k < count; k++) {
        u32 b = (state >> (2 + 8 * k)) & 0xFF;
        LexCsCtx *c = &st[k];
        c->kind = (u8)(b & 3);
        c->depth = c->kind == LEX_CS_HOLE ? (u8)(b >> 2) : 0;
        c->interp = c->kind == LEX_CS_HOLE ? 0 : (u8)((b >> 2) & 1);
        c->quotes = c->kind == LEX_CS_RAW ? (u8)(3 + ((b >> 3) & 7)) : 0;
        c->dollars = c->kind == LEX_CS_RAW ? (u8)(b >> 6) : 0;
    }
    return count;
}

static i64 lex_cs_run(u8 *s, i64 n, i64 i, u8 c) {
    i64 j = i;
    while (j < n && s[j] == c) j++;
    return j - i;
}

// The text of the string on top of the stack from i: up to its end (popped), a hole (pushed) or
// the end of the line.
static i64 lex_cs_string(u8 *s, i64 n, i64 i, LexCsCtx *st, i32 *count, SyntaxTokens *out) {
    LexCsCtx *c = &st[*count - 1];
    syn_emit(out, i, SYN_STRING);
    while (i < n) {
        u8 b = s[i];
        if (c->kind == LEX_CS_REGULAR && b == '\\') {
            i += 2;
            continue;
        }
        if (b == '"') {
            if (c->kind == LEX_CS_VERBATIM && i + 1 < n && s[i + 1] == '"') {
                i += 2;
                continue;
            }
            i64 run = c->kind == LEX_CS_RAW ? lex_cs_run(s, n, i, '"') : 1;
            if (run >= (c->kind == LEX_CS_RAW ? c->quotes : 1)) {
                (*count)--;
                return i + run;
            }
            i += run;
            continue;
        }
        if (b == '{' && c->interp) {
            i64 run = lex_cs_run(s, n, i, '{');
            i64 need = c->kind == LEX_CS_RAW ? MAX(c->dollars, 1) : 1;
            if (c->kind != LEX_CS_RAW && run >= 2) { // {{ is a brace
                i += 2;
                continue;
            }
            if (run >= need && *count < LEX_CS_STACK) {
                i64 open = i + run - need; // extra braces before the opening ones are text
                syn_emit(out, open, SYN_PUNCTUATION);
                st[(*count)++] = (LexCsCtx){ .kind = LEX_CS_HOLE };
                return i + run;
            }
            i += run;
            continue;
        }
        if (b == '}' && c->interp && c->kind != LEX_CS_RAW && i + 1 < n && s[i + 1] == '}') {
            i += 2;
            continue;
        }
        i++;
    }
    return n;
}

static u32 lex_cs(u32 state, u8 *s, i64 n, SyntaxTokens *out) {
    LexCsCtx st[LEX_CS_STACK];
    i32 count = lex_cs_unpack(state, st);
    i64 i = 0;
    if (state & LEX_CS_COMMENT) {
        syn_emit(out, 0, SYN_COMMENT);
        i64 e = lex_c_block_end(s, n, 0);
        if (e < 0) return state;
        i = e;
        if (i < n) syn_emit(out, i, SYN_TEXT);
    }
    // A preprocessor line: the directive word, the rest lexed.
    i64 first = syn_skip_blanks(s, n, i);
    if (count == 0 && i == 0 && first < n && s[first] == '#') {
        i64 w = syn_skip_blanks(s, n, first + 1);
        syn_emit(out, first, SYN_DIRECTIVE);
        i = syn_ident_end(s, n, w);
        if (i < n) syn_emit(out, i, SYN_TEXT);
    }
    while (i < n) {
        if (count > 0 && st[count - 1].kind != LEX_CS_HOLE) {
            i = lex_cs_string(s, n, i, st, &count, out);
            if (i < n) syn_emit(out, i, SYN_TEXT);
            continue;
        }
        u8 c = s[i];
        if (syn_is_blank(c)) {
            i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            syn_emit(out, i, SYN_COMMENT);
            i = n;
            break;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            syn_emit(out, i, SYN_COMMENT);
            i64 e = lex_c_block_end(s, n, i + 2);
            if (e < 0) return lex_cs_pack(st, count, 1);
            i = e;
            if (i < n) syn_emit(out, i, SYN_TEXT);
            continue;
        }
        // Strings: $ and @ prefixes, then quotes.
        if (c == '"' || ((c == '$' || c == '@') && i + 1 < n && (s[i + 1] == '"' || s[i + 1] == '$' || s[i + 1] == '@'))) {
            i64 j = i, dollars = 0;
            b32 verbatim = 0;
            while (j < n && (s[j] == '$' || s[j] == '@')) {
                if (s[j] == '$') dollars++;
                else verbatim = 1;
                j++;
            }
            if (j < n && s[j] == '"') {
                syn_emit(out, i, SYN_STRING);
                i64 q = lex_cs_run(s, n, j, '"');
                if (count < LEX_CS_STACK) {
                    if (q >= 3) {
                        st[count++] = (LexCsCtx){ .kind = LEX_CS_RAW, .interp = dollars > 0, .quotes = (u8)MIN(q, 10), .dollars = (u8)MIN(dollars, 3) };
                        i = j + q;
                    } else if (q == 2) { // ""
                        i = j + 2;
                        if (i < n) syn_emit(out, i, SYN_TEXT);
                        continue;
                    } else {
                        st[count++] = (LexCsCtx){ .kind = (u8)(verbatim ? LEX_CS_VERBATIM : LEX_CS_REGULAR), .interp = dollars > 0 };
                        i = j + 1;
                    }
                    continue;
                }
                i = n;
                break;
            }
        }
        if (c == '\'') {
            syn_emit(out, i, SYN_STRING);
            b32 open;
            i = lex_c_quoted(s, n, i + 1, '\'', &open);
            continue;
        }
        if (syn_is_digit(c) || (c == '.' && i + 1 < n && syn_is_digit(s[i + 1]))) {
            syn_emit(out, i, SYN_NUMBER);
            i = syn_number(s, n, i, '_', 0);
            continue;
        }
        if (c == '@' && i + 1 < n && syn_is_alpha(s[i + 1])) { // @class: an identifier
            syn_emit(out, i, SYN_TEXT);
            i = syn_ident_end(s, n, i + 1);
            continue;
        }
        if (syn_is_alpha(c)) {
            i64 e = syn_ident_end(s, n, i);
            syn_emit(out, i, syn_lookup(&lex_cs_words, s + i, e - i, SYN_TEXT));
            i = e;
            continue;
        }
        if (count > 0 && (c == '{' || c == '}')) { // inside a hole
            LexCsCtx *h = &st[count - 1];
            syn_emit(out, i, SYN_PUNCTUATION);
            if (c == '{') {
                if (h->depth < 255) h->depth++;
                i++;
            } else if (h->depth > 0) {
                h->depth--;
                i++;
            } else { // the hole ends: back to the string (a raw string's hole closes with as many braces as it opened with)
                count--;
                LexCsCtx *str = count > 0 ? &st[count - 1] : NULL; // none when the line started deeper than LEX_CS_SAVED
                i64 need = str && str->kind == LEX_CS_RAW ? MAX(str->dollars, 1) : 1;
                i += MIN(lex_cs_run(s, n, i, '}'), need);
            }
            continue;
        }
        syn_emit(out, i, syn_is_control(c) || c == '`' ? SYN_INVALID : SYN_PUNCTUATION);
        i++;
    }
    // A regular string's text cannot go past the end of the line.
    while (count > 0 && st[count - 1].kind == LEX_CS_REGULAR) count--;
    return lex_cs_pack(st, count, 0);
}
