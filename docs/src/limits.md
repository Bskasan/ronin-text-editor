# Known limits

- Monospace text on a cell grid: no proportional fonts, ligatures, bold or italic; wide (East Asian) characters and
  emoji show as a box; no fallback font for characters the font lacks.
- No line wrapping (long lines scroll horizontally), no line numbers, no cursor blink, no smooth scrolling.
- No language server and no parser: highlighting and indentation come from hand-written lexers and simple rules, for six
  languages. comment-line uses `//` in every language.
- Search is for literal text only: no regular expressions, no whole-word search, no project-wide replace.
- Missing Emacs features: prefix arguments (`C-u`), the mark ring, rectangles, overwrite mode, auto-save, `ESC` as
  Meta.
- Encodings: UTF-8, and UTF-16 with a byte order mark; anything else is shown and saved as raw bytes. Files over 1 GB
  are refused.
- At most 8 windows. One build at a time, with no input to it and only the built-in error formats.
- The git changes view only reads, needs git 2.15 or later, and does not scroll diffs sideways.
- Themes are colors only, and the built-in ones are dark: ClearType is blended for light text on a dark background.
- Terminals: Windows Terminal or the classic console only.
- Crash recovery is a best effort: it writes the unsaved files only when ronin itself crashes, not when the process is
  ended from outside or the power fails.

The full list, with the reasons, is the "Later" section of
[docs/ARCHITECTURE.md](https://github.com/Bskasan/ronin-text-editor/blob/main/docs/ARCHITECTURE.md#later).
