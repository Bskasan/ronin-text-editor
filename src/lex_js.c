// lex_js.c — the JavaScript and TypeScript lexer (one, with a TypeScript flag). Comments; strings
// with backslash continuation; template literals with ${ } holes lexed as code, nested and across
// lines; regex literals told apart from division by the previous significant token; numbers;
// keywords; TypeScript's extra keywords and built-in type names.
//
// State: bits 0-2 how many contexts the next line starts in (template text or a hole, at most 5,
// the innermost kept), 4 bits each from bit 3 (0 = template text, 1 + d = a hole with d braces open
// inside it); bit 23 a '/' is a division (the previous significant token is a value); bits 24-25 a
// string continued with a backslash (1 = ", 2 = '); bit 29 inside a block comment;
// SYNTAX_STATE_LITERAL when the line starts inside a comment, a string or template text.

#define LEX_JS_STACK 16 // contexts within a line
#define LEX_JS_SAVED 5  // contexts carried to the next line
#define LEX_JS_DIVIDE (1u << 23)
#define LEX_JS_STRING_SHIFT 24
#define LEX_JS_COMMENT (1u << 29)
#define LEX_JS_TEMPLATE 0 // a context: template text (else a hole, 1 + its open braces)

static SynTable lex_js_words, lex_ts_words;

static void lex_js_add_common(SynTable *t) {
    syn_table_add(t, "break case catch class const continue debugger default delete do else export extends finally for "
                     "function if import in instanceof let new return super switch this throw try typeof var void while "
                     "with yield async await of static", SYN_KEYWORD);
    syn_table_add(t, "true false null undefined NaN Infinity", SYN_CONSTANT);
}

static void lex_js_init(void) {
    lex_js_add_common(&lex_js_words);
    lex_js_add_common(&lex_ts_words);
    syn_table_add(&lex_ts_words, "interface type enum implements declare namespace module abstract readonly keyof infer is as "
                                 "satisfies private protected public override unique asserts", SYN_KEYWORD);
    syn_table_add(&lex_ts_words, "string number boolean any unknown never object symbol bigint", SYN_TYPE);
}

// Whether a regex may follow a keyword: not after the ones that are values.
static b32 lex_js_value_word(u8 *s, i64 len) {
    return syn_word_is(s, len, "this") || syn_word_is(s, len, "super");
}

// A regex literal from its '/': the end after its flags (it cannot go past the end of the line).
static i64 lex_js_regex(u8 *s, i64 n, i64 i) {
    b32 class = 0;
    for (i++; i < n; i++) {
        u8 c = s[i];
        if (c == '\\') {
            i++;
        } else if (c == '[') {
            class = 1;
        } else if (c == ']') {
            class = 0;
        } else if (c == '/' && !class) {
            return syn_ident_end(s, n, i + 1);
        }
    }
    return n;
}

static u32 lex_js_pack(u8 *st, i32 count, b32 regex, u32 string, b32 comment) {
    i32 from = MAX(count - LEX_JS_SAVED, 0);
    u32 state = (u32)(count - from);
    for (i32 k = from; k < count; k++) state |= (u32)MIN(st[k], 15) << (3 + 4 * (k - from));
    if (!regex) state |= LEX_JS_DIVIDE;
    state |= string << LEX_JS_STRING_SHIFT;
    if (comment) state |= LEX_JS_COMMENT;
    if (comment || string || (count > 0 && st[count - 1] == LEX_JS_TEMPLATE)) state |= SYNTAX_STATE_LITERAL;
    return state;
}

// Template text from i: up to its closing ` (popped), a ${ (a hole pushed) or the end of the line.
static i64 lex_js_template(u8 *s, i64 n, i64 i, u8 *st, i32 *count, SyntaxTokens *out) {
    syn_emit(out, i, SYN_STRING);
    while (i < n) {
        if (s[i] == '\\') {
            i += 2;
        } else if (s[i] == '`') {
            (*count)--;
            return i + 1;
        } else if (s[i] == '$' && i + 1 < n && s[i + 1] == '{' && *count < LEX_JS_STACK) {
            syn_emit(out, i, SYN_PUNCTUATION);
            st[(*count)++] = 1;
            return i + 2;
        } else {
            i++;
        }
    }
    return n;
}

static u32 lex_js_any(u32 state, u8 *s, i64 n, SyntaxTokens *out, SynTable *words) {
    u8 st[LEX_JS_STACK];
    i32 count = (i32)(state & 7);
    for (i32 k = 0; k < count; k++) st[k] = (u8)((state >> (3 + 4 * k)) & 15);
    b32 regex = !(state & LEX_JS_DIVIDE); // a regex may start here
    u32 string = (state >> LEX_JS_STRING_SHIFT) & 3;
    i64 i = 0;
    if (state & LEX_JS_COMMENT) {
        syn_emit(out, 0, SYN_COMMENT);
        i64 e = lex_c_block_end(s, n, 0);
        if (e < 0) return state;
        i = e;
        if (i < n) syn_emit(out, i, SYN_TEXT);
    } else if (string) {
        syn_emit(out, 0, SYN_STRING);
        b32 open;
        i = lex_c_quoted(s, n, 0, string == 1 ? '"' : '\'', &open);
        if (open) return lex_js_pack(st, count, regex, lex_c_ends_with_backslash(s, n) ? string : 0, 0);
        regex = 0;
        if (i < n) syn_emit(out, i, SYN_TEXT);
    }
    while (i < n) {
        if (count > 0 && st[count - 1] == LEX_JS_TEMPLATE) {
            i = lex_js_template(s, n, i, st, &count, out);
            regex = 0; // after a template's end (inside a hole, set again below)
            if (count > 0 && st[count - 1] != LEX_JS_TEMPLATE) regex = 1; // a hole just opened
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
            if (e < 0) return lex_js_pack(st, count, regex, 0, 1);
            i = e;
            if (i < n) syn_emit(out, i, SYN_TEXT);
            continue;
        }
        if (c == '/' && regex) {
            syn_emit(out, i, SYN_STRING);
            i = lex_js_regex(s, n, i);
            regex = 0;
            continue;
        }
        if (c == '"' || c == '\'') {
            syn_emit(out, i, SYN_STRING);
            b32 open;
            i = lex_c_quoted(s, n, i + 1, c, &open);
            if (open && lex_c_ends_with_backslash(s, n)) return lex_js_pack(st, count, 0, c == '"' ? 1 : 2, 0);
            regex = 0;
            continue;
        }
        if (c == '`') {
            syn_emit(out, i, SYN_STRING);
            if (count < LEX_JS_STACK) st[count++] = LEX_JS_TEMPLATE;
            i++;
            continue;
        }
        if (syn_is_digit(c) || (c == '.' && i + 1 < n && syn_is_digit(s[i + 1]))) {
            syn_emit(out, i, SYN_NUMBER);
            i = syn_number(s, n, i, '_', 0);
            regex = 0;
            continue;
        }
        if (syn_is_alpha(c) || c == '$' || (c == '#' && i + 1 < n && syn_is_alpha(s[i + 1]))) {
            i64 e = i + 1;
            while (e < n && (syn_is_ident(s[e]) || s[e] == '$')) e++;
            SyntaxKind kind = c == '#' ? SYN_TEXT : syn_lookup(words, s + i, e - i, SYN_TEXT);
            syn_emit(out, i, kind);
            regex = kind == SYN_KEYWORD && !lex_js_value_word(s + i, e - i);
            i = e;
            continue;
        }
        if (count > 0 && (c == '{' || c == '}')) { // inside a hole
            syn_emit(out, i, SYN_PUNCTUATION);
            if (c == '{') {
                if (st[count - 1] < 15) st[count - 1]++;
                regex = 1;
            } else if (st[count - 1] > 1) {
                st[count - 1]--;
                regex = 1;
            } else { // the hole ends: back to the template's text
                count--;
            }
            i++;
            continue;
        }
        if (syn_is_control(c)) {
            syn_emit(out, i, SYN_INVALID);
            i++;
            continue;
        }
        syn_emit(out, i, SYN_PUNCTUATION);
        if ((c == '+' || c == '-') && i + 1 < n && s[i + 1] == c) {
            i += 2; // ++ and -- leave the regex rule as it was (x++ / 2, but = ++x)
            continue;
        }
        regex = !(c == ')' || c == ']');
        i++;
    }
    return lex_js_pack(st, count, regex, 0, 0);
}

static u32 lex_js(u32 state, u8 *s, i64 n, SyntaxTokens *out) {
    return lex_js_any(state, s, n, out, &lex_js_words);
}

static u32 lex_ts(u32 state, u8 *s, i64 n, SyntaxTokens *out) {
    return lex_js_any(state, s, n, out, &lex_ts_words);
}
