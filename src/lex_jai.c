// lex_jai.c — the Jai lexer. // comments and nested /* */; strings (ending at the end of the line);
// here-strings (#string TERM ... TERM); numbers with 0x, 0b, 0h and '_'; keywords and built-in
// types; every #identifier and @note as a directive; --- as a constant. Declarations: an identifier
// followed by :: is a function, followed by := or a single : a variable.
//
// State: bits 0-1 the mode a line starts in; in a block comment, bits 2-17 the nesting depth; in a
// here-string, bits 2-25 a hash of the terminator and bits 26-29 its length (clipped to 15);
// SYNTAX_STATE_LITERAL whenever the mode is not code.

enum {
    LEX_JAI_CODE,
    LEX_JAI_BLOCK, // inside /* */, nested
    LEX_JAI_HERE,  // inside a #string here-string
};

#define LEX_JAI_DEPTH_MAX 0xFFFF

static SynTable lex_jai_words;

static void lex_jai_init(void) {
    SynTable *t = &lex_jai_words;
    syn_table_add(t, "if ifx then else case for while break continue return defer using struct union enum enum_flags "
                     "cast xx inline no_inline remove push_context size_of type_of type_info initializer_of is_constant "
                     "operator interface context it it_index distinct", SYN_KEYWORD);
    syn_table_add(t, "int s8 s16 s32 s64 u8 u16 u32 u64 float float32 float64 bool string void Type Any Code", SYN_TYPE);
    syn_table_add(t, "true false null", SYN_CONSTANT);
}

static u32 lex_jai_here_state(u8 *term, i64 len) {
    return LEX_JAI_HERE | SYNTAX_STATE_LITERAL | (syn_hash(term, len) & 0xFFFFFF) << 2 | (u32)MIN(len, 15) << 26;
}

// Inside a nested comment from i at `depth`: returns the end of the outermost "*/", or -1 with
// *depth updated.
static i64 lex_jai_block(u8 *s, i64 n, i64 i, u32 *depth) {
    while (i + 1 < n) {
        if (s[i] == '/' && s[i + 1] == '*') {
            if (*depth < LEX_JAI_DEPTH_MAX) (*depth)++;
            i += 2;
        } else if (s[i] == '*' && s[i + 1] == '/') {
            i += 2;
            if (--(*depth) == 0) return i;
        } else {
            i++;
        }
    }
    return -1;
}

static u32 lex_jai(u32 state, u8 *s, i64 n, SyntaxTokens *out) {
    u32 mode = state & 3;
    i64 i = 0;
    if (mode == LEX_JAI_BLOCK) {
        u32 depth = (state >> 2) & 0xFFFF;
        syn_emit(out, 0, SYN_COMMENT);
        i64 e = lex_jai_block(s, n, 0, &depth);
        if (e < 0) return LEX_JAI_BLOCK | SYNTAX_STATE_LITERAL | depth << 2;
        i = e;
    } else if (mode == LEX_JAI_HERE) {
        // The string ends at a line whose first word is the terminator.
        syn_emit(out, 0, SYN_STRING);
        i64 w = syn_skip_blanks(s, n, 0), we = syn_ident_end(s, n, w);
        i64 len = we - w;
        if (!len || (syn_hash(s + w, len) & 0xFFFFFF) != ((state >> 2) & 0xFFFFFF) || (u32)MIN(len, 15) != ((state >> 26) & 15)) return state;
        i = we;
    }
    if (i > 0) syn_emit(out, i, SYN_TEXT);

    while (i < n) {
        u8 c = s[i];
        if (syn_is_blank(c)) {
            i++;
            continue;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            syn_emit(out, i, SYN_COMMENT);
            return 0;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            syn_emit(out, i, SYN_COMMENT);
            u32 depth = 1;
            i64 e = lex_jai_block(s, n, i + 2, &depth);
            if (e < 0) return LEX_JAI_BLOCK | SYNTAX_STATE_LITERAL | depth << 2;
            i = e;
            if (i < n) syn_emit(out, i, SYN_TEXT);
            continue;
        }
        if (c == '"') {
            syn_emit(out, i, SYN_STRING);
            b32 open;
            i = lex_c_quoted(s, n, i + 1, '"', &open); // unterminated: ends at the end of the line
            continue;
        }
        if (syn_is_digit(c) || (c == '.' && i + 1 < n && syn_is_digit(s[i + 1]) && !(i > 0 && s[i - 1] == '.'))) {
            syn_emit(out, i, SYN_NUMBER);
            i = syn_number(s, n, i, '_', 1);
            continue;
        }
        if ((c == '#' || c == '@') && i + 1 < n && syn_is_alpha(s[i + 1])) {
            i64 e = syn_ident_end(s, n, i + 1);
            syn_emit(out, i, SYN_DIRECTIVE);
            if (c == '#' && syn_word_is(s + i + 1, e - i - 1, "string")) {
                // #string TERM (modifiers such as ",cr" before it are skipped): a here-string from the next line.
                i64 j = e;
                if (j < n && s[j] == ',') while (j < n && !syn_is_blank(s[j])) j++;
                j = syn_skip_blanks(s, n, j);
                i64 te = syn_ident_end(s, n, j);
                if (te > j) {
                    syn_emit(out, j, SYN_STRING);
                    if (te < n) syn_emit(out, te, SYN_TEXT);
                    return lex_jai_here_state(s + j, te - j);
                }
            }
            i = e;
            continue;
        }
        if (syn_is_alpha(c)) {
            i64 e = syn_ident_end(s, n, i);
            SyntaxKind kind = syn_lookup(&lex_jai_words, s + i, e - i, SYN_TEXT);
            if (kind == SYN_TEXT) {
                // A declaration: name :: (a procedure, a struct, a constant), name := or name : (a variable).
                i64 j = syn_skip_blanks(s, n, e);
                if (j < n && s[j] == ':') kind = j + 1 < n && s[j + 1] == ':' ? SYN_FUNCTION : SYN_VARIABLE;
            }
            syn_emit(out, i, kind);
            i = e;
            continue;
        }
        if (c == '-' && i + 2 < n && s[i + 1] == '-' && s[i + 2] == '-') {
            syn_emit(out, i, SYN_CONSTANT);
            i += 3;
            continue;
        }
        syn_emit(out, i, syn_is_control(c) || c == '`' ? SYN_INVALID : SYN_PUNCTUATION);
        i++;
    }
    return 0;
}
