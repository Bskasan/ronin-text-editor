# teal

Minimal native Windows code editor in the spirit of Jonathan Blow's Emacs setup, borrowing
ideas from Focus and 4coder. Emacs look and model (no tabs, scrollbars, menus or toolbars;
side-by-side windows, inverse-video mode line, minibuffer, block cursor, point/mark, kill
ring, isearch, M-x), pure Emacs default keys rebindable from a hot-reloaded text config.
Languages: Jai, C, C++, C#, JavaScript, TypeScript with hand-written lexers (no LSP, no
tree-sitter). Built in 15 phases; see docs/ARCHITECTURE.md for the roadmap, module map,
decisions and the "Later" list. Implement only the phase you are asked for.

**Priorities, in order:** (1) input latency and speed, (2) small and simple, (3) features.

## Hard rules

- C11, MSVC (cl.exe), x64 only, Windows 10 1903+. No C++, except src/win32_dwrite.cpp
  (dwrite.h cannot be consumed from C). That file is C-style C++: no exceptions (/EH off),
  no RTTI (/GR-), no STL, no new/delete, no classes, no static constructors, no CRT calls,
  nothing but DirectWrite calls behind the extern "C" API in src/font_backend.h.
- No third-party code or libraries. System DLLs only.
- One build.bat, unity build: src/teal.c includes the other .c files (win32_dwrite.cpp is
  the only second translation unit). No CMake, no .sln.
- No teal identifier may be a name a system header defines as a macro (winuser.h's MOD_ALT,
  rpcndr.h's small, minwindef.h's near / far). Every build first compiles src/check_macros.c with
  /Zs (every system header, then teal.c, syntax only) and fails on a collision. Fix by renaming,
  never by #undef.
- /W4 /WX clean, always.
- Write as if there were no C runtime: no stdio, no malloc/free, no str* functions, no CRT
  float formatting. memcpy/memset/memmove are fine. (Static CRT linked for now; Phase 15 may
  remove it.)
- Memory: arena allocators over VirtualAlloc (reserve large, commit on demand). One
  permanent arena, one per-frame scratch arena. No heap allocation per keystroke or frame.
- Text: UTF-8 internally, `String8 { u8 *data; i64 len; }`, not NUL-terminated. Convert to
  UTF-16 only at the Win32 boundary. W APIs only.
- Rendering is on demand. Idle = blocked in MsgWaitForMultipleObjectsEx, 0% CPU. No idle
  timers, no busy loops.
- Layering: editor core files use only platform.h and render.h (font.c also uses
  font_backend.h). Only win32_*.c/.cpp and render_d3d11.c touch Win32 / D3D / DWrite types.
- State lives in a few explicit structs (App, Platform, Renderer), not scattered globals.
- Naming: types PascalCase; functions and variables snake_case with a module prefix
  (arena_push, r_push_rect, os_write_file); macros and enum constants UPPER_SNAKE.
- Scope discipline: build only what the current phase asks for. No speculative
  abstractions. Park ideas in the "Later" list in docs/ARCHITECTURE.md.
- Verify every change: build all three configurations (debug, release, bench) before every
  commit, then debug build + `--smoke`, and bench build + `--test` (the optimized build runs
  the fuzz tests quickly). Run the full `--test` on the debug build once per phase, before
  its final commit. Take a screenshot after a visual change and look at it.
- Code, comments, docs and all communication with the user (plans, reports, questions) in English.
- Whenever the user asks for changes to a plan, show the full revised plan again with the
  changes marked and wait for approval before implementing.
- Every phase appends its own manual test steps to docs/MANUAL_TESTS.md, each with one status:
  AUTOMATED (an existing test, named), AUTOMATED-NEW (a test added for it in that phase: the input
  harness, --keys, the headless app, a smoke probe, a bench check or a dev helper), FIXED (the
  failing test and the fix commit), or HUMAN (why only a person can judge it, the shortest exact
  instructions, and a dev helper for most of it where possible). Check each step against its own
  wording, not against what the code does; a difference is a finding.

## Commit policy (every phase)

- Commit as you go: one logical change per commit, each one building in all three
  configurations with zero warnings and passing the tests and smoke. No single giant commit at the end of a phase.
- Categorized messages: `<type>(<scope>): <summary>`, type one of feat, fix, perf, refactor,
  test, docs, build, chore. Example: `feat(buffer): gap buffer with incremental line index`.
- Never push. Never amend or rewrite existing commits. The user pushes.
- Never commit build outputs or scratch files. If a pending change looks unintended, list it
  and ask instead of committing it.
- Never make an empty commit.
- A phase ends with a clean working tree. Its last commit is `docs: mark phase N done`, and the
  phase's roadmap checkbox in docs/ARCHITECTURE.md changes in that commit only, together with
  nothing else.

## Build / run / verify

From Git Bash (this environment sets `NoDefaultCurrentDirectoryInExePath=1`, so cmd needs
the explicit `.\`):

    cmd //c ".\build.bat"            # build\teal_debug.exe  (/Od /Zi /MTd, TEAL_DEV=1)
    cmd //c ".\build.bat release"    # build\teal.exe        (/O2 /GL ..., TEAL_DEV=0)
    cmd //c ".\build.bat bench"      # build\teal_bench.exe  (/O2 /MT, TEAL_DEV=1, no D3D debug layer)

Report benchmark numbers from teal_bench.exe (dev flags, optimized code). Its smoke run skips
the debug-layer check; the smoke acceptance criterion is the debug build.

build.bat finds MSVC through vswhere + vcvars64 when cl is not on PATH, compiles
src/shaders/*.hlsl with fxc into build\gen\*.h, runs the macro-collision check
(src/check_macros.c, /Zs, ~0.3 s), compiles win32_dwrite.cpp, then the unity build, and prints
the exe size.

Command line, every build: `teal [[+LINE[:COLUMN]] file ...]` (1-based, as in Emacs): every file
is opened, each at the +LINE[:COLUMN] before it (one with no file after it: the file before it, or
*scratch*); the first is shown, and with startup_windows = 2 the second window shows the second.
Any unrecognized argument starting with `--` (or a flag missing its value) exits at once with
code 10, before a window exists.

`--startup-ms` works in both builds: it exits right after the first Present with exit code =
ms since process creation. Git Bash `$?` truncates exit codes to 8 bits, so read it from
PowerShell: `(Start-Process build\teal.exe -ArgumentList --startup-ms -PassThru -Wait).ExitCode`.

Dev-build flags (TEAL_DEV=1 only); everything is logged to build\teal.log:

    build/teal_debug.exe --test [--seed N]                # headless buffer/file/view tests, exit 0 = pass
    build/teal_debug.exe --smoke                          # exit 0 = pass
    build/teal_debug.exe --screenshot build/shots/x.png   # window hidden, one frame
    build/teal_debug.exe +LINE:COL <file> --screenshot .. # open a file at a position (cursor drawn focused)
    build/teal_debug.exe --dump-atlas build/shots/a.png   # the CPU glyph atlas
    build/teal_debug.exe --render-mode classic|natural|symmetric   (overrides the config)
    build/teal_debug.exe --config build/tmp/x.conf        # this file instead of the user's teal.conf
    build/teal_debug.exe <file> --keys "M-> RET h i C-x C-s"   # input after startup, kbd notation:
                                                          # a chord is a KEY_DOWN, a single character
                                                          # (or SPC) is typed; with --screenshot the
                                                          # frame is taken after the keys
    build/teal_debug.exe --scale 150                      # force the DPI scale (percent)
    build/teal_debug.exe --bench-text                     # 300 frames, Present(0, 0)
    build/teal_bench.exe --bench-buffer                   # 100 MB file (build\tmp): load, inserts,
                                                          # lookups, save, frames at top/middle/end
    build/teal_bench.exe --bench-view                     # same file, key/text events through the app:
                                                          # next-line, PageDown, C-End/C-Home, typing
    build/teal_bench.exe --bench-edit                     # same file: typing with undo, kill / yank /
                                                          # undo of 50 MB, undo log, kill ring, memory
                                                          # per extra buffer
    build/teal_bench.exe --bench-complete                 # filter and rank 10,000 / 100,000 candidates
                                                          # per keystroke (avg, worst)
    build/teal_bench.exe --bench-syntax                   # lexing MB/s per language; 100 MB C file: "/*" on
                                                          # line 1 then M-> (frames until colored), typing
    build/teal_bench.exe --bench-search                   # 100 MB file: isearch for a needle at the end and
                                                          # a missing one (folded / exact, typed / pasted):
                                                          # time, frames, longest frame; isearch keystrokes
                                                          # on src/app.c; replace-string x 1,000,000, its
                                                          # undo and undo-redo; replace-string on the
                                                          # 100 MB file, its progress, C-g, one C-x u
                                                          # (exit 9 if wrong)
    build/teal_debug.exe <file> --keys ".." --touch <file> --screenshot ..   # rewrite the file after the
                                                          # keys and activate the app (changed on disk)
    build/teal_debug.exe --log-keys                       # every keyboard message: bits, keys down, teal's
                                                          # modifiers and AltGr, ToUnicodeEx, the layout,
                                                          # the event, the chord and command (or why none);
                                                          # the key table of each loaded layout
    build/teal_debug.exe --idle-check                     # shown without focus; 5 s idle after 1 s: CPU
                                                          # time, frames (must be 0), private bytes, the
                                                          # messages that reached the window meanwhile
                                                          # (it opens under the mouse: don't touch it)
    build/teal_debug.exe <file> --dpi-check build/shots/dpi   # WM_DPICHANGED to 125% and back, three
                                                          # screenshots, the frame after coming back equal
    build/teal_debug.exe --clipboard-check                # the REAL clipboard (only when it holds plain
                                                          # text or nothing; saved, put back, kept out of
                                                          # clipboard history): CRLF out, LF in
    build/teal_debug.exe --type-ahead-check               # REAL input: once teal's window is the foreground
                                                          # one, a thread types a b S-c d e with SendInput
                                                          # (letters and Shift only, one SendInput call per
                                                          # key, the foreground checked before each) while
                                                          # the device is created; *scratch* must read
                                                          # "abCde", every key sent before the first
                                                          # Present. Exit 0 pass (or a skip without the
                                                          # foreground), 9 fail; "inconclusive" when other
                                                          # input arrived. Only ever run by this flag: never
                                                          # from --test, the smoke, a bench or the build.
    build/teal_bench.exe --bench-windows                  # 100 MB file: frames with one window and with four
                                                          # (top, middle, line 1,000,000, end); typing at
                                                          # line 1,000,000 with one window and with two

The smoke and the benches read only the built-in config (deterministic) unless --config is
given; every other run reads the user's teal.conf as usual. Use --config build\tmp\... for
checks, so open-config never touches the real %APPDATA%\teal\teal.conf. The benches report
the final flush and the Present separately, the Presents that waited a vertical blank (>= 5 ms)
and the presentation mode: "overlay" (DWM shows the window directly) makes Present(0, 0) wait
one refresh interval with two buffers; that is not CPU work of ours. --test, the smoke, the
benches and screenshot runs use an in-memory fake clipboard: they never touch the real one
(interactive dev runs and --clipboard-check do). Dev builds log the private bytes at the startup stages
("memory:" lines in build\teal.log) and the startup timeline: each stage in ms since process
creation ("startup:" lines, recorded by os_dev_stage, logged once after the first frame).

`--smoke` shows the window without activating it and renders 2 frames. Then *scratch* gets a
known text in each language (C, C++, C#, JavaScript, TypeScript, Jai; app_dev_smoke_syntax) and
each frame is read back and checked (app_dev_syntax_probes): a comment, a keyword and a string
pixel in their exact colors, and the match of the bracket at point on the paren_match
background. The C frame also has the rendering checks: exact background / mode line / cursor
pixels, a text cell that is not background, a space cell that is exactly background, '_' inked
only in its lower part (catches upside-down bitmaps), and the ClearType channel order on a '|'
in the text color (normalized coverage; pixel geometry from the rendering params). Then it
switches to a known Fundamental buffer and checks a frame without focus (app_dev_buffer_probes
stage 0): a text cell drawn, an empty cell exactly background, a tab leaving columns 0-3 empty
with the next character at column 4, ^A taking two cells, a hollow cursor (edges in the
cursor color, inside background), the mode line with the buffer name and nothing after its
text. A synthetic click on a character with focus forced on gives the next frame (stage 1): a
filled cursor there with the glyph drawn over it, the old cursor cell cleared. An active
region over lines 0-1 gives the next (stage 2): selected cells in the selection color, a
glyph drawn over it, the selection reaching the window edge on lines whose newline is selected,
unselected cells and edges in the background. M-x with "minib" gives the next (stage 3):
a pixel of the prompt in the prompt color, the selected row's empty part in completion_selection,
a pixel of a matched substring in completion_match, an unselected row in the background, the
calling view's hollow cursor. "foo bar foo" with C-s f o o C-s gives the last one (stage 4): the
current match's isearch background and an isearch_text pixel, the other match's lazy_highlight
background, the spaces beside them untouched, the prompt color on the echo line; then "x" makes it
fail (stage 5): the failing part on the isearch_fail background in the prompt line. Two windows side by
side (stage 6): the left one selected with focus and a 400-character line, the right one another
buffer: the divider in window_divider over the full height (nothing of the long line in it), the right
window's first column exactly background, the long line drawn up to the divider, the inverse mode
line on the left and mode_line_inactive_background / _text on the right, the filled cursor on the left
and the hollow one on the right; then back to one window. Every syntax
frame also has a number pixel in its color. The window
title must be "*scratch* - teal",
set exactly once. Then: the font was set up exactly once at startup; build\tmp\smoke_keys.txt is
edited and saved through the --keys path ("M-> RET h i C-x C-s") and its bytes compared, and
so is a scripted session in build\tmp\smoke_session.c (a function typed with RET only, a region
killed and yanked, undo, undo-redo, a merged run of typing undone, comment-line); a config with background = #102030 is loaded (as C-c r), the
pixel probed, and the font must not have been set up again. C-x C-+, C-x C-0, C-x C-- and Ctrl +
wheel, then a config with font_size = 14, each set up the font again with a larger or smaller cell
(C-x C-0 and the wheel back to the same one). WM_QUERYENDSESSION is sent to its own
window with an unsaved file (refused, the block reason "Unsaved changes in teal", the save question; a repeated query
keeps that chain; C-g keeps the reason; WM_ENDSESSION(FALSE) removes it) and again once it is
saved (allowed). It also requires a non-empty atlas,
the D3D11 debug layer active with zero WARNING+ messages, and no leaks (device refcount 0, empty
DXGI live-object report, DirectWrite references 0, no directory watch left open).
Exit codes: 1 fatal, 2 renderer init, 3 pixel mismatch, 4 no debug layer, 5 debug-layer
messages, 6 leak (D3D, DirectWrite, buffers, live markers, watches), 7 output file, 8 font /
ClearType (also: the font set up more than once), 9 test failure (--test; in the smoke: the
--keys edit saved the wrong bytes, or the end-of-session check), 10 unknown argument (every build).

`--test` runs without a device and without a visible window: a differential fuzz of `buffer_replace` against a
flat-array reference (100,000 ops, fixed seed printed in the log and on failure, `--seed`
overrides; decimal or 0x hex), capacity and read-only checks, byte-for-byte file round trips
(encodings, line endings, block and chunk boundaries; inputs stay in build\tmp\rt_*),
load-edit-save-reload, save / load failures, a marker differential fuzz (300 markers, both
insertion types), the language table, a column-mapping round trip on fuzzed lines, a table of
motion cases with their messages, scrolling and recentering, three cursors through the
command driver, read-only refusals and save messages, a view fuzz (point always on a
boundary and visible, valid scroll position), the command table, kbd notation (canonical
forms, rejections, 20,000 round trips), chords from key events, the key sequence state machine
(prefix, undefined, quit, shift-translation, dropped text, C-x o, describe-key, two keymaps
sharing a prefix, binding conflicts), the config (defaults, layering, every diagnostic with
its line, clamping, none, files, 10,000 random inputs, settings in the view), the buffer list
and *Messages*, hot reload with a real directory watch in build\tmp\watch (write, other file,
save by rename, a locked file retried and given up, the 50 ms settle), the undo log (undo and
redo of 40 random edits, the saved state, merging at 20, the limit and its memory), undo through
the driver (the Emacs chain, undo-redo, merging, point, three cursors), mark and region (the
shift-select state table, delete-active-region, delete_selection_mode, word bounds), the
clipboard conversions and the fake, the kill ring (kill-line cases, append and prepend, yank-pop,
the clipboard link, read-only, the storage), indentation (a table of C, Jai and JavaScript
snippets, tabs, detection, RET, closing brackets, TAB, backtab, M-i, C-q), the other editing
commands on tricky input, and an undo fuzz of 4,000 random commands through the driver (every
state id stands for one text, undo and redo return to known states, the modified flag). Phase 7
drives a headless app (no window, no font; keys in --keys notation through the real keymap and
driver) against temporary trees in build\tmp\p7*: the directory listing, the matcher (ranking,
terms, case, folding, spans, narrowing against full ranking), the minibuffer (RET / C-j, C-g and
ESC on every prompt kind, a prefix inside it, editing, kill ring, undo, mouse, numbers, yes-or-no,
single keys and the quit keys, history, recursion, chains, DEL after a slash, scroll positions),
completion and M-x, unique names, switch-to-buffer and kill-buffer, find-file and write-file,
goto-line, save-some-buffers and quitting (every answer, aborts at every link, the close button),
revert (one replace, markers, undo and undo-redo, line endings, no-ops, whole characters),
changed on disk (activation, watch and settle, modified, the save guard, deleted, auto_revert off,
watches released) and the end of the session. Phase 8: line states following their lines (a differential fuzz),
golden tokens per language, incremental equals full under random edits and partial catch-ups, the
catch-up budget with a deterministic clock, bracket matching, the indentation table on tokens, typed
RET cases, show-paren, set-language, and highlighting after an auto-revert. Phase 9: electric
case/default labels (C, C++, C#, JavaScript, Jai, with their undo); the search engine against a
naive reference (36,000 searches: both directions and case modes, random ranges, the gap inside the
match, overlaps, multi-byte text, each uncut and in random slices; fixed cases, a restart after an
edit, needle limits, the Turkish i limit, smart case, the fast path's first bytes); isearch through
the headless app (a state table of extend, repeat, fail, wrap, overwrap, reverse and DEL unwinding
with the exact prompts, C-g, RET / ESC and the mark, typed ahead, C-s C-s, M-p / M-n, another
command, a global prefix, C-w, C-y, smart case, M-c, the minibuffer, a search sliced at 64 KB a
frame, the mouse wheel); query-replace and replace-string (every answer, the last pair, typed ahead,
an outside change while asking, one undo and undo-redo, the region, case conversion, a -> aa, no
matches, read-only, the wheel, replace-all over 20,000 matches stopped midway with C-g and undone in
one step). Tests that need search work spread over frames set app->dev_work_budget (positions per
frame instead of the clock). Phase 10: the window tree (fixed cases and a fuzz of 16,000 operations on
400 frames, tiny and empty ones included: exact tiling, minimum sizes, splits at their ratios, frame
resizes there and back), each view's remembered positions, and through the headless app every window
command with its messages and refusals (in the minibuffer too), the order of C-x o, the pop-up rule in
each case, two windows on one buffer, the mouse (clicks, the wheel, divider and mode-line drags, the
cursor), prompts and isearch from the second of three windows, a frame too small for 8 windows (and
back: identical rects, scroll positions and points), startup_windows = 2 (app_dev_frame_size sets the
headless frame). The steps of docs/MANUAL_TESTS.md that had no test run through the
headless app (test_manual_*, logged as "test: ok: manual <ids>"). Then the Win32 side: modifier keys
set in the thread's key state read by win32_mods; keypad text right after a chord; and the input
harness (src/win32_input_test.c): key messages posted, with their scan codes and flags (AltGr as the
synthetic Left Ctrl and Right Alt), to a top-level window that is never shown and uses the real
window procedure, through the normal pump (TranslateMessage, DispatchMessage) into a headless app,
on US (must pass completely), United Kingdom, Turkish Q and Finnish, each loaded and activated for
the harness thread only (KLF_NOTELLSHELL; the session's layouts and the foreground layout are
checked unchanged afterwards; no SendInput). Per layout: letters, symbols and Turkish letters from
their keys, Alt / Ctrl / Ctrl+Alt chords, Right Alt as Meta or AltGr text and Left Alt + AltGr,
Alt+Shift and Ctrl+Shift symbols, dead keys (and none left behind by a chord), nothing to
DefWindowProc for Alt / F10 / Alt+Space, describe-key on keys without a chord, the keypad, a letter
posted as WM_SYSKEYDOWN without Alt (no focus window) typing its letter, C-x 3 / C-x o / C-M-v /
C-M-S-v, no frame for a mouse move or WM_SETCURSOR, every default binding through describe-key (a
plain character from a dead key typed as the dead key and Space; the bindings whose US keys run
something else on the layout are listed), Alt+F4. Also the command line's files and +LINE:COLUMN. A failed dev ASSERT logs its file, line and condition before breaking,
so a crash shows up in build\teal.log.

Open every screenshot after a visual change and look at it (crop and enlarge for detail);
check exact colors with an independent decoder, e.g. PowerShell `System.Drawing.Bitmap`.
When touching rasterization, check that the three render modes still produce different files.

## Theme (the built-in defaults: src/config_default.h, [colors])

The config file's color names are the roles below (text is also the mode line; number is
number/constant).

| role             | color   |   | role            | color   |
|------------------|---------|---|-----------------|---------|
| background       | #072626 |   | comment         | #3fdf1f |
| text / mode line | #d3b58d |   | string          | #0fdfaf |
| cursor           | #90ee90 |   | keyword         | #ffffff |
| selection        | #0000ff |   | number/constant | #7ad0c6 |
| type             | #8cde94 |   | variable        | #c1d1e3 |
| prompt           | #0fdfaf |   | completion_match | #ffffff |
| completion_selection | #0000ff | | function        | #ffffff |
| directive        | #8cde94 |   | constant        | #7ad0c6 |
| paren_match (background) | #4f94cd | | isearch (background) | #cd00cd |
| isearch_text     | #b0e2ff |   | lazy_highlight (background) | #668b8b |
| isearch_fail (background) | #8b0000 | | window_divider | #126367 |
| mode_line_inactive_background | background | | mode_line_inactive_text | text |

The swap chain is B8G8R8A8_UNORM (not sRGB): theme colors must reach the screen bit-exact.
