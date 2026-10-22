# Changelog

## Unreleased

**Changed**

- Contributing: every change now starts as a GitHub issue and reaches main through a pull request; CONTRIBUTING.md
  describes the branches, the flow and the versions.

**Fixed**

- The git changes view (`C-x g`): `RET` or a double click on a file whose diff has not arrived yet now opens the file at
  its first change once git answers, instead of at line 1. The status line says it is waiting; any other key, `C-g` or
  a click elsewhere cancels.

## 1.0.0 — 2026-10-21

ronin 1.0.0 is the first release for everyone: a small, fast, Emacs-style code editor for Windows 10 and 11 (x64), one
executable of under half a megabyte with no installer and nothing else to install. Since 0.1.0 it has a command list, a
git changes view, terminal shortcuts, five themes and a documentation site, and it has been through a long hunt for leaks
and slow paths. Files, keys and settings from 0.1.0 keep working; one thing to know about an old ronin.conf is under
Changed.

### Added

- **The command list** (`C-S-p`): every command with a one-line description and its keys. Type to filter (fuzzy, on the
  name and the description), `RET` or a double click runs the selected command, `C-g`, `ESC` or a click outside close
  it. Its keys are in `[keys command-list]`, its colors `modal_background`, `modal_border`, `modal_selection`,
  `modal_description` and `modal_key`.
- **The git changes view** (`C-x g`): the project's uncommitted changes, git's two-letter status on the left and the
  selected file's diff against HEAD on the right, colored. `TAB` moves into the diff, `RET` visits the file at its first
  change, `g` asks git again, `C-g` closes it and stops a git that still runs. It only reads, and needs git 2.15 or
  later (`git.exe` on PATH, or the `git_path` setting). New settings `git_diff_max_lines`, `git_status_max_files` and
  `git_timeout_ms`, new colors `diff_added`, `diff_removed`, `diff_hunk`, `diff_header` and `git_status`, and the
  section `[keys git-changes]`.
- **Terminals**: `C-c t` opens a terminal in the current file's directory, `C-c a` one in the project's root running
  an AI assistant's command line tool: `claude`, `codex` with `ai_provider = openai`, or any command with `ai_command`.
  Windows Terminal when it is installed, the classic console otherwise (the `terminal` setting chooses). The terminals
  stay open when ronin ends.
- **Themes**: ronin (the default, unchanged), moonfly, oxide, vague and mellifluous are built in. `theme = <name>` in
  ronin.conf chooses one and `M-x set-theme` switches for the session. A theme is a short text file:
  `themes\<name>.conf` beside ronin.conf or ronin.exe adds one or replaces a built-in one, and is applied as soon as it
  is saved. New colors `mode_line_background` and `mode_line_text` for the selected mode line.
- **Documentation**: https://bskasan.github.io/ronin-text-editor/ covers installing, the first steps, every feature,
  the configuration, every key and setting (generated from ronin itself), themes and the known limits.
- **The zip** also holds this changelog and `samples\`, one small file per language to try the highlighting and the
  indentation on.

### Changed

- **Colors and themes.** The colors come from the theme; `[colors]` in ronin.conf still sets single colors, on top of
  it. A ronin.conf made with `C-c ,` by 0.1.0 lists every color and so hides any other theme, and ronin says so
  ("ronin.conf: N [colors] keys override theme X"): delete the lines under `[colors]` (keep the header) to use themes.
- Seven colors now follow another one unless you set them: constant (number), function and completion_match (keyword),
  directive and compilation_info (type), prompt (string), completion_selection (selection). The default look is the
  same; a ronin.conf that sets `number` but not `constant` now changes both.
- `C-S-p` opens the command list instead of selecting up a line (`S-<up>` still does).
- Memory: the worker threads of scans, searches and builds are reused instead of started anew, and a large project
  search gives its working memory back when ronin is minimized; a long session no longer grows.

### Fixed

- A finished project scan, search or build could stay unshown while ronin was idle, until the next key.
- Memory that grew over a long session: the build commands remembered per directory, saving under new names, the
  headers of project searches and occur, and the graphics driver's share for every finished worker thread.
- The git changes view: a warning git prints while writing a diff (such as "LF will be replaced by CRLF") no longer
  cuts a hunk one line short or shows up in a renamed file's old name.
- Saving ronin.conf or a theme file twice within a few milliseconds, with texts of the same length, could leave the
  second version unapplied until the next change.

### Known limits

- Monospace text on a cell grid: no proportional fonts, bold or italic; wide East Asian characters and emoji show as a
  box, and there is no fallback font.
- No line wrapping and no line numbers.
- Search is for literal text: no regular expressions, no whole-word search, no project-wide replace.
- No prefix arguments, mark ring, rectangles or auto-save, and `ESC` is not Meta.
- UTF-8 and UTF-16 with a byte order mark; other encodings are kept as raw bytes. Files over 1 GB are refused.
- The git changes view only reads. One build at a time, at most 8 windows.
- The built-in themes are dark.
- The exe is not signed, so SmartScreen asks on its first start.

The whole list is on the [Known limits](https://bskasan.github.io/ronin-text-editor/limits.html) page.

## 0.1.0 (2026-10-14)

The first release: everything built in phases 1 to 15 (docs/ARCHITECTURE.md has the details of each).
Until Phase 15 the project was called teal.

- **Window and rendering** (1, 2): a Win32 window drawn with Direct3D 11 only when something changed (0% CPU
  idle); text rasterized with DirectWrite into a glyph atlas, ClearType, theme colors bit-exact on screen.
- **Text buffer** (3): a gap buffer with a line index; UTF-8 and UTF-16 with a BOM; LF, CRLF and mixed line
  endings kept byte for byte; safe saves through a temporary file.
- **Views and commands** (4, 5): point, scrolling, the mode line; a keymap with prefix keys and Emacs' default
  bindings; one text config file for settings, colors and keys, reloaded when saved.
- **Editing** (6): mark and region, shift-select, the kill ring linked to the clipboard, undo as in Emacs
  plus undo-redo, automatic indentation, comment-line and the other editing commands.
- **Minibuffer and files** (7): M-x, find-file, switch-to-buffer, write-file, goto-line, kill-buffer with
  completion and history; files changed on disk; the questions on quitting and signing out.
- **Highlighting** (8): hand-written lexers for Jai, C, C++, C#, JavaScript and TypeScript, incremental;
  indentation and show-paren on tokens.
- **Search** (9): isearch with the other matches highlighted, query-replace and replace-string.
- **Windows** (10): splitting, resizing with keys and the mouse, other-window commands.
- **Projects** (11): project roots, a background scan honoring .gitignore and .ronin-project, fuzzy file open.
- **Project-wide search** (12): literal search over a project's files on worker threads, *search* and occur.
- **Builds** (13): a build command run in the project, *compilation* with the errors of MSVC, GCC / Clang,
  tsc and Jai recognized, next-error.
- **Multiple cursors** (14): the commands of Emacs' multiple-cursors package; typing at 10,000 cursors in
  ~2 ms per key.
- **Last pass** (15): renamed to ronin; the release exe links no C runtime and imports only kernel32, user32
  and gdi32 (482,304 -> 397,312 bytes, icon and version included); idle memory 76-77 -> 57-59 MB; case labels
  at 10,000 cursors 0.93 s -> 28 ms; moving on a 5 MB line with show-paren 6.5 -> 0.5-0.7 ms a key; crash recovery files and
  M-x recover-file; `early_window`; `--version`, a version resource and an icon drawn at build time; a seed
  sweep and a million-event soak test in the phase gate; two cursor-ordering bugs the soak found, fixed.
