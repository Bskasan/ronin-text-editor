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

The command list (Phase 18), the first box drawn over the windows:

- C-S-p lists every command with a one-line description and its key bindings in the current window. Typing
  filters it (fuzzy, on the name, the description or both), the arrows, C-n / C-p and C-v / M-v move, RET or a
  double click runs the selected command as M-x would, C-g, ESC or a click outside close it. Its keys are in the
  new `[keys command-list]` section, its colors are `modal_background`, `modal_border`, `modal_selection`,
  `modal_description` and `modal_key`.
- C-S-p is no longer a shift-selecting C-p (S-<up> still is).
- Every command has a description (M-x and describe-key do not show it yet).
- The tests no longer leave a junction loop in `build\tmp`.

The git changes view (Phase 19), the second box:

- C-x g shows the uncommitted changes of the project's git work tree: the changed files on the left with git's
  two-letter status, the selected file's diff against HEAD on the right, colored. Its diff is fetched once per
  file while the box is open; TAB moves to the diff and scrolls it, RET visits the file at its first change, g
  runs git status again, C-g, ESC or a click outside close it and stop a git that still runs. Read only.
- git runs in the background (`git.exe` on PATH, or the new `git_path` setting); the box opens once git has
  answered, so without git or outside a work tree a message says so and nothing opens. A refusal of git's shows
  git's own words.
- New settings `git_diff_max_lines` (20,000), `git_status_max_files` (5,000) and `git_timeout_ms` (30,000), new
  colors `diff_added`, `diff_removed`, `diff_hunk`, `diff_header` and `git_status`, the new section
  `[keys git-changes]`.

Terminals (Phase 20):

- C-c t (open-terminal) opens a terminal in the current file's directory; C-c a (open-ai-terminal) one in the
  project's root running an AI assistant's command line tool: `claude` by default, `codex` with
  `ai_provider = openai`, or `ai_command` (`ai_provider = custom`, or set for any provider). Windows Terminal when
  `wt.exe` is on PATH, else the classic console; the new setting `terminal` (auto, wt, cmd) chooses. ronin starts
  the terminal and lets it go; it stays open after ronin ends.
- The git changes view: a warning git printed while it was writing a diff (such as "LF will be replaced by CRLF")
  no longer ends that file's hunk one line early, and no longer shows up in a renamed file's old name.

Themes (Phase 21):

- Five built-in themes: ronin (the default, unchanged), and moonfly, oxide, vague and mellifluous, ported from
  their Neovim color schemes (MIT; credited in README.md and THIRD_PARTY.md). The new setting `theme` chooses one;
  M-x set-theme switches for the session, with completion over every theme. A theme is a file in the config format
  (a [theme] block and [colors]); `themes\<name>.conf` next to ronin.conf (or ronin.exe) adds one or replaces a
  built-in one, and theme files are watched and applied as soon as they are saved.
- ronin.conf's [colors] still pins single colors, over any theme; the built-in configuration (what C-c , writes
  into a new ronin.conf) no longer lists the color values, only the keys. A ronin.conf written before lists them
  all and pins every color: with a theme other than ronin, each load says "ronin.conf: N [colors] keys override
  theme X". Remove that section to use themes.
- Two new color keys, `mode_line_background` and `mode_line_text`, for the selected mode line (inverse video as
  before), and seven colors that had another color's value now follow it unless set: constant (number), function
  and completion_match (keyword), directive and compilation_info (type), prompt (string), completion_selection
  (selection). ronin's colors are unchanged; a ronin.conf that sets number but not constant now moves both.
- `dist.bat` puts THIRD_PARTY.md into the zip.

The repository for other people (Phase 22):

- README.md rewritten for someone who has never seen ronin: what it is, why, how to install it, the keys of the first
  five minutes, configuration, themes, one short section per feature with its keys, known limits, the roadmap. A test
  checks it against the code on every build: each key in its tables runs the command it names (through describe-key),
  each setting exists with a valid default, its configuration examples load cleanly, and its links resolve.
- CONTRIBUTING.md: how to build and verify, what a pull request must meet, how to propose a feature and report a bug.
  Issue templates for bugs and features, a pull request checklist, and a GitHub Actions build that compiles the three
  configurations and runs the tests on every push.
- The oxide theme's added lines in the git changes view now use oxide's own "added" green (#5bb661) instead of ronin's.

The sources in order (Phase 23), nothing changed for users (the release exe is the same, byte for byte, apart from
its link time):

- `src/` is in folders by layer: `base/`, `platform/` (the only one with Windows headers), `text/`, `editor/`,
  `project/`, `app/` and `dev/`.
- Every source file starts with a header: the copyright, the MIT notice, and one line saying what the file does, the
  same line as in the module map of docs/ARCHITECTURE.md.
- `verify.bat` and the CI build check both on every run, and that no file outside `src/platform/` includes a Windows
  header and nothing in `src/base/` or `src/text/` includes the layers above them. CONTRIBUTING.md explains it under
  "New files".

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
