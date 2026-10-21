# Roadmap

What may come after 1.0, in about this order and without dates:

- Terminals: a terminal inside ronin, beside the code, for a shell or an AI tool (today they open in their own window).
- Editing: auto-save, prefix arguments (`C-u`), the mark ring, rectangles, `ESC` as Meta.
- Search: regular expressions, whole-word search, project-wide replace, a match count in isearch.
- Text: line numbers and line wrapping, wide characters and emoji, a fallback font, legacy code pages and UTF-16
  without a byte order mark.
- git: staging, committing and reverting from the git changes view.
- Themes: light themes.
- Windows: undo of window changes.
- Projects and builds: an index kept current from change notifications, run and debug commands, several build commands
  per project, user-defined error formats.
- Speed on extreme input: very long lines, and saving on a background thread.

Not planned, by design: a language server or tree-sitter, proportional fonts, ligatures, bold and italic, tabs, menus,
toolbars and scroll bars.

The full list, with the reasons, is the "After 1.0" section of
[docs/ARCHITECTURE.md](https://github.com/Bskasan/ronin-text-editor/blob/main/docs/ARCHITECTURE.md#after-10).
