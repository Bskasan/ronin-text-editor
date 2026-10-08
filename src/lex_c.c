// lex_c.c — the C and C++ lexer (one for both: a .h file can be either). Comments, strings and
// chars with escapes, raw strings, numbers, keywords of C11 and C++20, built-in and fixed-width
// types, preprocessor lines (the directive word; the <header> of #include as a string; the name of
// #define as a function), and function names in definitions at column 0.
//
// State: bits 0-3 the mode a line starts in; for a raw string, bits 4-8 the delimiter's length and
// bits 9-28 a hash of it; SYNTAX_STATE_DIRECTIVE when the line continues a preprocessor line;
// SYNTAX_STATE_LITERAL whenever the mode is not code.

enum {
    LEX_C_CODE,
    LEX_C_BLOCK,   // inside /* */
    LEX_C_STRING,  // a "..." continued with a backslash at the end of the line
    LEX_C_CHAR,    // a '...' continued the same way
    LEX_C_RAW,     // inside R"delim( ... )delim"
    LEX_C_LINE,    // a // comment continued with a backslash
};

#define LEX_C_RAW_MAX 16 // the longest raw string delimiter

static SynTable lex_c_words;

static void lex_c_init(void) {
    SynTable *t = &lex_c_words;
    syn_table_add(t, "auto break case const continue default do else enum extern for goto if inline register "
                     "restrict return sizeof static struct switch typedef union volatile while _Alignas _Alignof "
                     "_Atomic _Generic _Noreturn _Static_assert _Thread_local", SYN_KEYWORD);
    syn_table_add(t, "alignas alignof and and_eq asm bitand bitor catch class compl concept consteval constexpr "
                     "constinit const_cast co_await co_return co_yield decltype delete dynamic_cast explicit export "
                     "friend mutable namespace new noexcept not not_eq operator or or_eq private protected public "
                     "reinterpret_cast requires static_assert static_cast template this thread_local throw try typeid "
                     "typename using virtual xor xor_eq final override import module typeof", SYN_KEYWORD);
    syn_table_add(t, "char double float int long short signed unsigned void _Bool _Complex _Imaginary bool char8_t "
                     "char16_t char32_t wchar_t size_t ssize_t ptrdiff_t intptr_t uintptr_t intmax_t uintmax_t "
                     "max_align_t nullptr_t int8_t int16_t int32_t int64_t uint8_t uint16_t uint32_t uint64_t "
                     "int_least8_t int_least16_t int_least32_t int_least64_t uint_least8_t uint_least16_t "
                     "uint_least32_t uint_least64_t int_fast8_t int_fast16_t int_fast32_t int_fast64_t uint_fast8_t "
                     "uint_fast16_t uint_fast32_t uint_fast64_t u8 u16 u32 u64 i8 i16 i32 i64 f32 f64 b32", SYN_TYPE);
    syn_table_add(t, "true false NULL nullptr", SYN_CONSTANT);
}

static u32 lex_c_raw_hash(u8 *s, i64 len) {
    return syn_hash(s, len) & 0xFFFFF;
}

static u32 lex_c_raw_state(u8 *delim, i64 len) {
    return LEX_C_RAW | SYNTAX_STATE_LITERAL | (u32)len << 4 | lex_c_raw_hash(delim, len) << 9;
}

// The end of a raw string's closing )delim" in s[i, n) for a delimiter of `len` bytes with hash
// `hash`, or -1.
static i64 lex_c_raw_end(u8 *s, i64 n, i64 i, i64 len, u32 hash) {
    for (; i + len + 1 < n; i++) {
        if (s[i] == ')' && s[i + 1 + len] == '"' && lex_c_raw_hash(s + i + 1, len) == hash) return i + len + 2;
    }
    return -1;
}

// The end of a quoted literal from s[i] (just after the opening quote): after the closing quote,
// or n. *open = it reached the end of the line still open.
static i64 lex_c_quoted(u8 *s, i64 n, i64 i, u8 quote, b32 *open) {
    while (i < n) {
        if (s[i] == '\\') {
            i += 2;
            continue;
        }
        if (s[i++] == quote) {
            *open = 0;
            return i;
        }
    }
    *open = 1;
    return n;
}

// The end of a block comment's "*/" from i, or -1.
static i64 lex_c_block_end(u8 *s, i64 n, i64 i) {
    for (; i + 1 < n; i++) if (s[i] == '*' && s[i + 1] == '/') return i + 2;
    return -1;
}

static b32 lex_c_ends_with_backslash(u8 *s, i64 n) {
    return n > 0 && s[n - 1] == '\\';
}

static u32 lex_c(u32 state, u8 *s, i64 n, SyntaxTokens *out) {
    u32 start_mode = state & 15, mode = start_mode;
    b32 directive = (state & SYNTAX_STATE_DIRECTIVE) != 0; // this line continues a preprocessor line
    u32 dir_next = directive && lex_c_ends_with_backslash(s, n) ? SYNTAX_STATE_DIRECTIVE : 0;
    i64 i = 0;
    // A literal from the previous line first.
    switch (start_mode) {
    case LEX_C_BLOCK: {
        syn_emit(out, 0, SYN_COMMENT);
        i64 e = lex_c_block_end(s, n, 0);
        if (e < 0) return LEX_C_BLOCK | SYNTAX_STATE_LITERAL | dir_next;
        i = e;
    } break;
    case LEX_C_STRING:
    case LEX_C_CHAR: {
        syn_emit(out, 0, SYN_STRING);
        b32 open;
        i = lex_c_quoted(s, n, 0, start_mode == LEX_C_STRING ? '"' : '\'', &open);
        if (open) return (lex_c_ends_with_backslash(s, n) ? start_mode | SYNTAX_STATE_LITERAL : 0) | dir_next;
    } break;
    case LEX_C_RAW: {
        syn_emit(out, 0, SYN_STRING);
        i64 e = lex_c_raw_end(s, n, 0, (state >> 4) & 31, (state >> 9) & 0xFFFFF);
        if (e < 0) return (state & ~SYNTAX_STATE_DIRECTIVE) | dir_next;
        i = e;
    } break;
    case LEX_C_LINE:
        syn_emit(out, 0, SYN_COMMENT);
        return (lex_c_ends_with_backslash(s, n) ? LEX_C_LINE | SYNTAX_STATE_LITERAL : 0) | dir_next;
    }
    mode = LEX_C_CODE;
    if (i > 0) syn_emit(out, i, SYN_TEXT);

    // Function names in definitions: a line that starts in column 0 with a word (in code). The word
    // directly before the first '(' becomes a function, unless it is a keyword, or the only word
    // with a ';' ending the line (a call).
    b32 col0 = start_mode == LEX_C_CODE && !directive && n > 0 && syn_is_alpha(s[0]);
    i32 words = 0, fn_words = 0;
    i64 fn_token = -1, ident_token = -1; // the candidate; the plain identifier just before (-1: not one)
    b32 paren_seen = 0;
    u8 last = 0; // the last byte of the last token that is not a comment

    // A preprocessor line: the directive word, then the rest lexed as code.
    i64 first = syn_skip_blanks(s, n, i);
    if (start_mode == LEX_C_CODE && !directive && first < n && s[first] == '#') {
        directive = 1;
        i64 w = syn_skip_blanks(s, n, first + 1);
        i64 we = syn_ident_end(s, n, w);
        syn_emit(out, first, SYN_DIRECTIVE);
        i = we;
        u8 *word = s + w;
        i64 wl = we - w;
        i64 j = syn_skip_blanks(s, n, i);
        if ((syn_word_is(word, wl, "include") || syn_word_is(word, wl, "include_next") || syn_word_is(word, wl, "import")) &&
            j < n && s[j] == '<') {
            i64 e = j + 1;
            while (e < n && s[e] != '>') e++;
            if (e < n) e++;
            syn_emit(out, j, SYN_STRING);
            i = e;
            last = '>';
        } else if (syn_word_is(word, wl, "define") && j < n && syn_is_alpha(s[j])) {
            syn_emit(out, j, SYN_FUNCTION);
            i = syn_ident_end(s, n, j);
            last = 'x';
        }
        if (i < n) syn_emit(out, i, SYN_TEXT);
    }

    while (i < n) {
        u8 c = s[i];
        if (syn_is_blank(c)) {
            i++;
            continue;
        }
        i64 start = i;
        if (c == '/' && i + 1 < n && s[i + 1] == '/') {
            syn_emit(out, i, SYN_COMMENT);
            if (lex_c_ends_with_backslash(s, n)) mode = LEX_C_LINE;
            i = n;
            break;
        }
        if (c == '/' && i + 1 < n && s[i + 1] == '*') {
            syn_emit(out, i, SYN_COMMENT);
            i64 e = lex_c_block_end(s, n, i + 2);
            if (e < 0) {
                mode = LEX_C_BLOCK;
                i = n;
                break;
            }
            i = e;
            if (i < n) syn_emit(out, i, SYN_TEXT);
            continue;
        }
        if (c == '"' || c == '\'') {
            syn_emit(out, i, SYN_STRING);
            b32 open;
            i = lex_c_quoted(s, n, i + 1, c, &open);
            if (open && lex_c_ends_with_backslash(s, n)) mode = c == '"' ? LEX_C_STRING : LEX_C_CHAR;
            ident_token = -1;
            last = c;
            continue;
        }
        if (syn_is_digit(c) || (c == '.' && i + 1 < n && syn_is_digit(s[i + 1]))) {
            syn_emit(out, i, SYN_NUMBER);
            i = syn_number(s, n, i, '\'', 0);
            ident_token = -1;
            last = '0';
            continue;
        }
        if (syn_is_alpha(c) || c == '$') {
            i64 e = i;
            while (e < n && (syn_is_ident(s[e]) || s[e] == '$')) e++;
            u8 *w = s + i;
            i64 wl = e - i;
            // String and char prefixes; raw strings.
            if (e < n && (s[e] == '"' || s[e] == '\'')) {
                b32 raw = s[e] == '"' && wl <= 3 && w[wl - 1] == 'R' &&
                          (wl == 1 || syn_word_is(w, wl, "u8R") || syn_word_is(w, wl, "uR") || syn_word_is(w, wl, "UR") ||
                           syn_word_is(w, wl, "LR"));
                b32 prefix = syn_word_is(w, wl, "u8") || syn_word_is(w, wl, "u") || syn_word_is(w, wl, "U") || syn_word_is(w, wl, "L");
                if (raw) {
                    i64 d = e + 1, de = d;
                    while (de < n && de - d <= LEX_C_RAW_MAX && s[de] != '(' && s[de] != ')' && s[de] != '\\' && s[de] != '"' &&
                           !syn_is_blank(s[de])) de++;
                    if (de < n && s[de] == '(' && de - d <= LEX_C_RAW_MAX) {
                        syn_emit(out, i, SYN_STRING);
                        i64 len = de - d;
                        i64 end = lex_c_raw_end(s, n, de + 1, len, lex_c_raw_hash(s + d, len));
                        ident_token = -1;
                        last = '"';
                        if (end < 0) {
                            state = lex_c_raw_state(s + d, len);
                            mode = LEX_C_RAW;
                            i = n;
                            break;
                        }
                        i = end;
                        continue;
                    }
                }
                if (raw || prefix) {
                    syn_emit(out, i, SYN_STRING);
                    b32 open;
                    i = lex_c_quoted(s, n, e + 1, s[e], &open);
                    if (open && lex_c_ends_with_backslash(s, n)) mode = s[e] == '"' ? LEX_C_STRING : LEX_C_CHAR;
                    ident_token = -1;
                    last = '"';
                    continue;
                }
            }
            SyntaxKind kind = syn_lookup(&lex_c_words, w, wl, SYN_TEXT);
            syn_emit(out, i, kind);
            ident_token = kind == SYN_TEXT && out ? out->count - 1 : -1;
            words++;
            i = e;
            last = 'x';
            continue;
        }
        if (syn_is_control(c) || c == '@' || c == '`') {
            syn_emit(out, i, SYN_INVALID);
            ident_token = -1;
            last = c;
            i++;
            continue;
        }
        // Punctuation; '(' decides the function name of a definition at column 0.
        if (c == '(' && !paren_seen) {
            paren_seen = 1;
            fn_token = ident_token;
            fn_words = words;
        }
        syn_emit(out, start, SYN_PUNCTUATION);
        ident_token = -1;
        last = c;
        i++;
    }

    if (col0 && fn_token >= 0 && out && fn_token < out->count && !(fn_words == 1 && last == ';')) {
        out->tokens[fn_token].kind = SYN_FUNCTION;
    }
    u32 next;
    switch (mode) {
    case LEX_C_CODE:  next = 0; break;
    case LEX_C_RAW:   next = state; break; // set above with the delimiter
    default:          next = mode | SYNTAX_STATE_LITERAL; break;
    }
    if (directive && lex_c_ends_with_backslash(s, n)) next |= SYNTAX_STATE_DIRECTIVE;
    return next;
}
