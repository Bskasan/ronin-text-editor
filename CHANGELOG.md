# Changelog

## Unreleased

A memory and resource leak audit (Phase 16): `--leak-check` repeats 22 operations hundreds of times and
checks memory, handles, threads, the glyph atlas and Direct3D objects for growth; the soak checks the same
over a million random events; every dev exit checks that nothing of ronin's is left. What it found, fixed:

- A finished project scan, search or build now shows its results while ronin is idle. When the workers'
  wake was what ended the main loop's wait, the wait had already reset it and the results stayed queued
  (with their finished threads unjoined) until a wake came while a frame was due.
- Worker threads are reused instead of ending after each task: with a hardware Direct3D device the GPU
  driver kept ~215 bytes of memory for every thread that ended, and ronin started one for every scan,
  search worker and build.
- The build commands remembered per directory no longer grow their memory with every build (a long session
  would have run out of the 64 MB reserved for them).
- Project searches and occur no longer leave their header's text behind.
- Saving a buffer under a new name (and names made unique, "name<dir>") no longer keeps the old path and
  name in the buffer's memory (its 1 MB would have run out after a few thousand renames).
- The font's atlas and glyph table are released at exit.

A cleanup before the first public release (Phase 17):

- A large project search no longer keeps its working memory for its pending results (several MB after tens of
  thousands of matches) until *search* is killed: it goes back when the window is minimized. Each results
  buffer (*search*, *occur*, *compilation*) keeps 768 KB for them once it has run, so repeated searches leave
  the same memory behind.
- Building: `build.bat clean` removes `build\` and `dist\`, everything generated, and nothing else.

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
