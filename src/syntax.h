// syntax.h — highlighting. Hand-written lexers, one per language family (lex_*.c), that lex one
// line at a time: a line's start state in, its tokens and its end state out. Line start states are
// stored in the buffer (buffer.h) and kept current lazily: only as far as some view needs them,
// within a time budget per frame, re-lexing from an edit until the states converge with the ones
// stored before. Tokens are never stored: visible lines are lexed again when drawn. Plain C,
// headless.

#ifndef SYNTAX_H
#define SYNTAX_H

typedef enum SyntaxKind {
    SYN_TEXT,
    SYN_COMMENT,
    SYN_STRING,
    SYN_NUMBER,
    SYN_KEYWORD,
    SYN_TYPE,
    SYN_CONSTANT,
    SYN_DIRECTIVE,
    SYN_FUNCTION,
    SYN_VARIABLE,
    SYN_PUNCTUATION,
    SYN_INVALID,
    SYN_KIND_COUNT
} SyntaxKind;

// A token's kind runs from its start to the next token's start (blanks take the kind before them).
typedef struct SyntaxToken {
    u32 start; // byte offset in the line
    u32 kind;  // SyntaxKind
} SyntaxToken;

typedef struct SyntaxTokens {
    SyntaxToken *tokens;
    i32 count, cap; // a full array stops taking tokens (the rest of the line keeps the last kind)
} SyntaxTokens;

// Every lexer keeps these two meanings in its state, so the core and indentation can ask.
#define SYNTAX_STATE_LITERAL   (1u << 31) // the line starts inside a multi-line comment or string
#define SYNTAX_STATE_DIRECTIVE (1u << 30) // the line continues a preprocessor line (C, C++)

#define SYNTAX_DRAW_MAX 20000          // beyond this many bytes into a line, text is drawn plain
#define SYNTAX_FRAME_BUDGET_US 2000    // lexing per frame while states catch up
#define SYNTAX_CHECK_LINES 64          // catch-up reads the clock after this many lines ...
#define SYNTAX_CHECK_BYTES KB(16)      // ... or this many bytes, whichever comes first

void syntax_init(void); // the keyword tables; once at startup
b32  syntax_has_lexer(BufferLanguage language);
// Lexes one line (without its '\n') from `state`; returns the state at the start of the next line.
// out = NULL: the state only.
u32  syntax_lex(BufferLanguage language, u32 state, String8 line, SyntaxTokens *out);

// States on for a buffer whose language has a lexer (all untrusted when the language changed),
// off otherwise. False: no highlighting for this buffer.
b32  syntax_attach(Buffer *buf);
// Lexes from the first line whose state is not known to be correct towards `need_line`, until
// that line's start state is correct or `budget_us` is used up (the clock is read every
// SYNTAX_CHECK_LINES lines or SYNTAX_CHECK_BYTES bytes). A line whose new start state equals the
// stored one after the last edited line ends the work: the rest is still correct. `scratch` holds
// a line that straddles the gap. True when need_line's start state is correct.
b32  syntax_catch_up(Buffer *buf, i64 need_line, u64 budget_us, Arena *scratch);
b32  syntax_line_ready(Buffer *buf, i64 line); // its start state is known to be correct
// The tokens of a line for drawing: `text` is the line's first bytes (at most SYNTAX_DRAW_MAX),
// lexed from the stored start state, which may be stale while states catch up. Returns false (no
// tokens: plain) when the buffer has no lexer or the line has no stored state yet.
b32  syntax_line_tokens(Buffer *buf, i64 line, String8 text, SyntaxTokens *out);

#if TEAL_DEV
void syntax_dev_fake_clock(b32 on); // the clock is the number of bytes lexed (deterministic budget tests)
u64  syntax_dev_lexed(void);        // bytes lexed by catch-up so far
u64  syntax_dev_converged(void);    // catch-ups that stopped early because states converged
#endif

#endif // SYNTAX_H
