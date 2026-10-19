# ronin

Minimal native Windows code editor in the spirit of Jonathan Blow's Emacs setup, borrowing
ideas from Focus and 4coder. Emacs look and model (no tabs, scrollbars, menus or toolbars;
side-by-side windows, inverse-video mode line, minibuffer, block cursor, point/mark, kill
ring, isearch, M-x), pure Emacs default keys rebindable from a hot-reloaded text config.
Languages: Jai, C, C++, C#, JavaScript, TypeScript with hand-written lexers (no LSP, no
tree-sitter). Built in phases; see docs/ARCHITECTURE.md for the roadmap, module map,
decisions and the "Later" list. Implement only the phase you are asked for.

**Priorities, in order:** (1) input latency and speed, (2) small and simple, (3) features.

## Hard rules

- C11, MSVC (cl.exe), Windows 10 or later, x64; developed and tested on Windows 11 only. No C++, except src/win32_dwrite.cpp
  (dwrite.h cannot be consumed from C). That file is C-style C++: no exceptions (/EH off),
  no RTTI (/GR-), no STL, no new/delete, no classes, no static constructors, no CRT calls,
  nothing but DirectWrite calls behind the extern "C" API in src/font_backend.h.
- No third-party code or libraries. System DLLs only.
- One build.bat, unity build: src/ronin.c includes the other .c files (win32_dwrite.cpp is
  the only second translation unit). No CMake, no .sln. tools/ holds build-time generators
  (tools/make_icon.c draws the icon into build\gen\ronin.ico, tools/make_themes.c embeds themes\*.conf into
  build\gen\themes.h, on every build): compiled and run by
  build.bat, never part of the exe, their output never committed; the same C rules apply to them.
- No ronin identifier may be a name a system header defines as a macro (winuser.h's MOD_ALT,
  rpcndr.h's small, minwindef.h's near / far). Every build first compiles src/check_macros.c with
  /Zs (every system header, then ronin.c, syntax only) and fails on a collision. Fix by renaming,
  never by #undef.
- /W4 /WX clean, always.
- Write as if there were no C runtime: no stdio, no malloc/free, no str* functions, no CRT
  float formatting. memcpy/memset/memmove are fine. Release and bench link no C runtime
  (/NODEFAULTLIB, entry point win32_entry; from the compiler's static libraries only the stack
  probe, memcpy / memset / memmove and their CPU dispatch); the debug build keeps the static CRT
  (/MTd, /GS). The release exe imports only KERNEL32, USER32 and GDI32 (res/imports.txt, checked
  by verify.bat); d3d11, dwmapi and dwrite are loaded with LoadLibraryW where first used. A new
  import or loaded DLL updates res/imports.txt and the import decision in docs/ARCHITECTURE.md.
- Memory: arena allocators over VirtualAlloc (reserve large, commit on demand). One
  permanent arena, one per-frame scratch arena. No heap allocation per keystroke or frame.
- Text: UTF-8 internally, `String8 { u8 *data; i64 len; }`, not NUL-terminated. Convert to
  UTF-16 only at the Win32 boundary. W APIs only.
- Rendering is on demand. Idle = blocked in MsgWaitForMultipleObjectsEx, 0% CPU. No idle
  timers, no busy loops.
- Threads (the device creation, the project scanner): a worker owns its own arena and reads only
  the immutable request it was started with. It never touches App, Platform, the log (no LOG, no
  ASSERT in worker code), the scratch arenas or any other arena, and calls only the os_* functions
  platform.h marks "any thread". It hands finished batches to the main thread through one
  synchronized queue (the lock held only to link or unlink a batch) and wakes the main loop with
  os_wake_main. It can be cancelled at any moment (an atomic flag checked at least once per directory
  entry, and its event for any wait). The main thread joins it when it reports that it is done (no
  wait then); at exit app_shutdown cancels every worker, then joins them within one shared bound of
  1,000 ms; a worker still inside a file-system call after that (a dead network path) is left to the
  process exit and logged in dev builds. The main thread never waits for a worker otherwise. Dev
  shutdown counts live threads, scan arenas and queued batches as leaks. A worker's OS thread is reused: when its task
  ends it parks on an event (0% CPU) and takes the next os_thread_start (the GPU driver keeps heap for every thread
  that ends); joining a worker waits for its task's end.
- Layering: editor core files use only platform.h and render.h (font.c also uses
  font_backend.h). Only win32_*.c/.cpp and render_d3d11.c touch Win32 / D3D / DWrite types.
- State lives in a few explicit structs (App, Platform, Renderer), not scattered globals.
- Naming: types PascalCase; functions and variables snake_case with a module prefix
  (arena_push, r_push_rect, os_write_file); macros and enum constants UPPER_SNAKE.
- Scope discipline: build only what the current phase asks for. No speculative
  abstractions. Park ideas in the "Later" list in docs/ARCHITECTURE.md.
- Verify every change with `verify.bat`, the only gate before a commit: it builds all three
  configurations (each with the macro check), checks `build.bat clean` on a scratch tree
  (clean-check), runs `--smoke` on the debug and the bench build and
  `--test` on the bench build (the optimized build runs the fuzz tests quickly), checks the release
  exe's imports (`--check-imports`) and its `--version`, checks every exit code and prints one last
  line, `verify: PASS` or `verify: FAIL: <what failed>`. Only that line counts: never run the steps
  by hand and judge them by eye. `verify.bat phase` also runs the full `--test` on the debug build,
  the seed sweep (`--fuzz-seeds 1 256`, bench), the soak (`--soak 1000000`, bench and debug) and the leak
  check (`--leak-check all`, debug; ~2 minutes): once per phase, before its final commit. Each run's log
  is kept as build\verify\<step>.log. Take a screenshot after a visual change and look at it.
- Anything that takes the foreground or injects real input (`--type-ahead-check` today) is never
  run without asking the user first, and then only once per request: they may be typing in another
  window. Everything else that is run must keep not taking the focus.
- Code, comments, docs and all communication with the user (plans, reports, questions) in English.
- Whenever the user asks for changes to a plan, show the full revised plan again with the
  changes marked and wait for approval before implementing.
- Every phase appends its own manual test steps to docs/MANUAL_TESTS.md, each with one status:
  AUTOMATED (an existing test, named), AUTOMATED-NEW (a test added for it in that phase: the input
  harness, --keys, the headless app, a smoke probe, a bench check or a dev helper), FIXED (the
  failing test and the fix commit), or HUMAN (why only a person can judge it, the shortest exact
  instructions, and a dev helper for most of it where possible). Check each step against its own
  wording, not against what the code does; a difference is a finding.
- Every phase keeps README.md current: what the phase adds (features, keys, configuration,
  limits, status, numbers), each claim checked against the code, the tests and the measurements.

## Commit policy (every phase)

- Commit as you go: one logical change per commit, each one with `verify.bat` printing
  `verify: PASS` right before it. No single giant commit at the end of a phase.
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

    cmd //c ".\build.bat"            # build\ronin_debug.exe  (/Od /Zi /MTd, RONIN_DEV=1)
    cmd //c ".\build.bat release"    # build\ronin.exe        (/O2 /GL ..., RONIN_DEV=0, no CRT)
    cmd //c ".\build.bat bench"      # build\ronin_bench.exe  (/O2, RONIN_DEV=1, no CRT, no D3D debug layer)
    cmd //c ".\build.bat clean"      # removes build\ and dist\ entirely, nothing else; refuses (exit 10) outside
                                     # the repository root or when either is a junction, a link or a file

    cmd //c ".\verify.bat"           # the gate: three builds, clean-check, smoke (debug, bench), test (bench),
                                     # imports, --version; one PASS / FAIL line
    cmd //c ".\verify.bat phase"     # ... plus --test (debug), the seed sweep, the soak and --leak-check all
                                     # (once per phase)
    cmd //c ".\dist.bat"             # verify.bat, then dist\ronin-<version>-windows-x64.zip (ronin.exe,
                                     # README.md, LICENSE); the version is src/version.h's

Report benchmark numbers from ronin_bench.exe (dev flags, optimized code). Its smoke run skips
the debug-layer check; the smoke acceptance criterion is the debug build.

build.bat finds MSVC through vswhere + vcvars64 when cl is not on PATH, compiles
src/shaders/*.hlsl with fxc into build\gen\*.h, runs the macro-collision check
(src/check_macros.c, /Zs, ~0.3 s), compiles and runs tools/make_icon.c (build\gen\ronin.ico) and
compiles res/ronin.rc (the icon and the version resource, ~0.26 s with the icon), compiles
win32_dwrite.cpp, then the unity build, and prints the exe size.

Command line, every build: `ronin [[+LINE[:COLUMN]] file ...]` (1-based, as in Emacs): every file
is opened, each at the +LINE[:COLUMN] before it (one with no file after it: the file before it, or
*scratch*); the first is shown, and with startup_windows = 2 the second window shows the second.
A directory instead of a file is opened as the project (`ronin <directory>`): nothing is visited for it,
*scratch* and C-x C-f start there, C-x p f lists it. Any unrecognized argument starting with `--` (or a flag missing its value) exits at once with
code 10, before a window exists. `--version` prints "ronin 0.1.0" (through a redirected standard
output, else the console that started it) and exits 0 before any window or thread.

`--startup-ms` works in both builds: it exits right after the first Present with exit code =
ms since process creation. Git Bash `$?` truncates exit codes to 8 bits, so read it from
PowerShell: `(Start-Process build\ronin.exe -ArgumentList --startup-ms -PassThru -Wait).ExitCode`.

`--no-activate` works in both builds: the window is shown (cloaked for the first frame, as the benches
show it) without taking the foreground. The memory table is measured with it: release build, private
bytes (`Get-Process`) after the window settled, closed with CloseMainWindow; it may run without asking.

Dev-build flags (RONIN_DEV=1 only); everything is logged to build\ronin.log:

    build/ronin_debug.exe --test [--seed N]               # headless buffer/file/view tests, exit 0 = pass
    build/ronin_debug.exe --smoke                         # exit 0 = pass
    build/ronin_debug.exe --screenshot build/shots/x.png  # window hidden, one frame
    build/ronin_debug.exe +LINE:COL <file> --screenshot .. # open a file at a position (cursor drawn focused)
    build/ronin_debug.exe --dump-atlas build/shots/a.png  # the CPU glyph atlas
    build/ronin_debug.exe --render-mode classic|natural|symmetric   (overrides the config)
    build/ronin_debug.exe --config build/tmp/x.conf       # this file instead of the user's ronin.conf
    build/ronin_debug.exe <file> --keys "M-> RET h i C-x C-s"  # input after startup, kbd notation:
                                                          # a chord is a KEY_DOWN, a single character
                                                          # (or SPC) is typed; with --screenshot the
                                                          # frame is taken after the keys
    build/ronin_debug.exe --scale 150                     # force the DPI scale (percent)
    build/ronin_debug.exe --bench-text                    # 300 frames, Present(0, 0)
    build/ronin_bench.exe --bench-buffer                  # 100 MB file (build\tmp): load, inserts,
                                                          # lookups, save, frames at top/middle/end
    build/ronin_bench.exe --bench-view                    # same file, key/text events through the app:
                                                          # next-line, PageDown, C-End/C-Home, typing
    build/ronin_bench.exe --bench-edit                    # same file: typing with undo, kill / yank /
                                                          # undo of 50 MB, undo log, kill ring, memory
                                                          # per extra buffer
    build/ronin_bench.exe --bench-complete                # filter and rank 10,000 / 100,000 candidates
                                                          # per keystroke (avg, worst)
    build/ronin_bench.exe --bench-syntax                  # lexing MB/s per language; 100 MB C file: "/*" on
                                                          # line 1 then M-> (frames until colored), typing
    build/ronin_bench.exe --bench-search                  # 100 MB file: isearch for a needle at the end and
                                                          # a missing one (folded / exact, typed / pasted):
                                                          # time, frames, longest frame; isearch keystrokes
                                                          # on src/app.c; replace-string x 1,000,000, its
                                                          # undo and undo-redo; replace-string on the
                                                          # 100 MB file, its progress, C-g, one C-x u
                                                          # (exit 9 if wrong)
    build/ronin_debug.exe <file> --keys ".." --touch <file> --screenshot ..  # rewrite the file after the
                                                          # keys and activate the app (changed on disk)
    build/ronin_debug.exe --log-keys                      # every keyboard message: bits, keys down, ronin's
                                                          # modifiers and AltGr, ToUnicodeEx, the layout,
                                                          # the event, the chord and command (or why none);
                                                          # the key table of each loaded layout
    build/ronin_debug.exe --idle-check                    # shown without focus; 5 s idle after 1 s: CPU
                                                          # time, frames (must be 0), private bytes, the
                                                          # messages that reached the window meanwhile
                                                          # (it opens under the mouse: don't touch it); logs
                                                          # whether a modal box is open and git's jobs at its start
    build/ronin_debug.exe <file> --dpi-check build/shots/dpi  # WM_DPICHANGED to 125% and back, three
                                                          # screenshots, the frame after coming back equal
    build/ronin_debug.exe --clipboard-check               # the REAL clipboard (only when it holds plain
                                                          # text or nothing; saved, put back, kept out of
                                                          # clipboard history): CRLF out, LF in
    build/ronin_debug.exe --type-ahead-check              # REAL input: once ronin's window is the foreground
                                                          # one, a thread types a b S-c d e with SendInput
                                                          # (letters and Shift only, one SendInput call per
                                                          # key, the foreground checked before each) while
                                                          # the device is created; *scratch* must read
                                                          # "abCde", every key sent before the first
                                                          # Present. Exit 0 pass (or a skip without the
                                                          # foreground), 9 fail; "inconclusive" when other
                                                          # input arrived. Only ever run by this flag: never
                                                          # from --test, the smoke, a bench or the build.
    build/ronin_bench.exe --bench-windows                 # 100 MB file: frames with one window and with four
                                                          # (top, middle, line 1,000,000, end); typing at
                                                          # line 1,000,000 with one window and with two
    build/ronin_bench.exe --bench-project                 # fuzzy ranking on 10,000 / 100,000 / 1,000,000
                                                          # synthetic paths (each keystroke's frame, frames
                                                          # until ranked, the longest frame, RET's whole
                                                          # ranking); scans of build\tmp\p11big and of
                                                          # %SystemRoot% (read only); typing at line
                                                          # 1,000,000 with and without a scan running
    build/ronin_debug.exe <dir> --scan-hold N --keys "C-x p f ..." --screenshot ..  # project scans stand
                                                          # still after N batches (the finder while scanning)
    build/ronin_bench.exe --bench-grep                    # build\tmp\p12bench (~100 MB, 3,000 files, made
                                                          # once): a common and a missing word with 1, 2, 4
                                                          # and 8 workers (first result, total, MB/s, the
                                                          # longest frame and its results work, memory);
                                                          # the default cap; typing at line 1,000,000 of the
                                                          # 100 MB file while 8 workers search (the key's
                                                          # frames and the frames between keys) against a
                                                          # finished *search* shown; the workers' priority
                                                          # below normal against normal, alternated
    build/ronin_debug.exe <dir> --keys "C-x p g ..." --keys-after "n RET" --screenshot ..  # keys once the
                                                          # workers' results of --keys are in
    build/ronin_debug.exe <dir> --grep-hold N --keys "C-x p g ..." --screenshot ..  # a search's workers stand
                                                          # still after N files each (a search still running)
    build/ronin_debug.exe --child <script>                # the tests' scripted program (no window, no log):
                                                          # out / err TEXT, raw / rawerr TEXT (escapes \r \n
                                                          # \t \e \xHH), sleep MS, exit N, hang (120 s at
                                                          # most), spawn FILE (a grandchild), flood BYTES,
                                                          # cwd, write PATH TEXT, env NAME, repeat N TEXT,
                                                          # launch FILE (a grandchild let go: how it started)
    build/ronin_debug.exe --fake-git DIR ARGS...           # the tests' git (no window, no log; every argument after
                                                          # DIR is git's): ARGS appended to DIR\calls.txt, then the
                                                          # --child script DIR\map.txt names for the first KEY<TAB>
                                                          # SCRIPT line whose KEY is in ARGS (none: exit 128)
    build/ronin_debug.exe --fake-terminal DIR PROGRAM CONSOLE ARGS...  # the tests' terminal (no window, no log): appends
                                                          # "<its working directory>|PROGRAM|CONSOLE|ARGS" to
                                                          # DIR\calls.txt, ARGS as they stand in its raw command line
    build/ronin_bench.exe --bench-compile                 # a --child program printing 100 MB into *compilation*
                                                          # (cap 128 MB and the default 16 MB): time, frames,
                                                          # the longest frame, memory; typing at line
                                                          # 1,000,000 while builds print, against none
    build/ronin_debug.exe <dir> --keys "C-x p c RET" --compile-wait MS --screenshot ..  # waits up to MS for
                                                          # running builds to end (also before --keys-after)
    build/ronin_bench.exe --bench-cursors                 # self-insert, DEL and RET at 1 to 10,000 cursors (the
                                                          # 100 MB file: consecutive lines and spread; src/app.c)
                                                          # with the batch flushes per command; the classes that
                                                          # reindent on a generated C file at 1,000 / 10,000; the undo
                                                          # of a 10,000-cursor edit; mark-all on a million occurrences
                                                          # (max_cursors 10,000 and 100,000) and over the 100 MB file;
                                                          # the memory of 10,000 / 100,000 cursors (exit 9 if wrong)
    build/ronin_bench.exe --bench-modal                   # the command list over the full command table, 5 rounds of:
                                                          # open, 20 filter keystrokes, 20 movements, close; median and
                                                          # max of the filter and of command + frame build; the memory
                                                          # of the first open, by owner (exit 9 at a filter median of
                                                          # 200 us)
    build/ronin_bench.exe --bench-git                     # git's output parsed from memory (a 50,000-line diff, a 5,000-
                                                          # file status; 20 runs, median and max); a 20,000-line diff of
                                                          # the fake git (build\tmp\p19bench) copied into the open git
                                                          # changes view, 5 rounds: frames, the longest slice and frame
                                                          # build (exit 9 when a slice passes 2.5 ms)
    build/ronin_bench.exe --fuzz-seeds FIRST COUNT        # every seeded --test group for seeds FIRST.. (worker
                                                          # processes, at most 8, below normal priority; their logs
                                                          # in build\fuzz\worker<k>.log); exit 9 names the seed
    build/ronin_bench.exe --soak N [--seed S]             # N random events through the headless app (keys of every
                                                          # keymap, typed code, mouse, focus, resizes) over the files of
                                                          # build\tmp\soak, the only place it may write; invariants every
                                                          # 1,000 events, settled memory (searches and git waited for;
                                                          # the last against the first half's highest, +2 MB) and a typing
                                                          # probe (the fastest of three) every 100,000; logs peak / final
                                                          # memory, its owners, and the 5 slowest events;
                                                          # the leak metrics every 100,000, judged over the last 500,000
                                                          # (exit 6 on LEAK; memory informational: the walk's high-water)
    build/ronin_debug.exe --leak-check [SCENARIO|all] [--cycles N]  # debug build only: each scenario (file-cycle,
                                                          # window-cycle, minibuffer-cycle, isearch-cycle, finder-cycle,
                                                          # grep-cycle, compile-cycle, multi-cursor-cycle, modal-cycle,
                                                          # git-cycle, terminal-cycle, config-reload-cycle,
                                                          # undo-cycle, external-change-cycle; a
                                                          # name before '/' runs its sub-cycles) after a prime step, 20
                                                          # warm-up cycles and 4 windows of N/4 (N = 200, a multiple of
                                                          # 4): one table a scenario in the log, LEAK -> exit 6, a cycle
                                                          # leaving another state -> 9, a screenshot that differs -> 3;
                                                          # files in build\tmp\leak (made once), its own leak.conf;
                                                          # the window is never shown. Verdicts are meaningful only
                                                          # at --cycles 200 or more (fewer can flag the results
                                                          # arena's bounded growth)
    build/ronin_debug.exe --map-report build\ronin.map    # the release exe's bytes by module (its linker map; src\ scanned)
    build/ronin_debug.exe --check-imports build\ronin.exe res\imports.txt   # its DLLs must equal the list, no CRT DLL
    build/ronin_bench.exe --memory-report                 # shown without activation; private bytes by owner (our
                                                          # arenas, heaps, stacks, the driver, image pages, views)
    build/ronin_bench.exe --bench-latency                 # 500 keys posted to the shown, not activated window: arrival
                                                          # to dequeue, command + build, Present, the vertical blank that
                                                          # showed it (DXGI frame statistics), by presentation mode
    build/ronin_bench.exe --startup-capture               # early_window measured: the window topmost, never activated;
                                                          # a thread reads its screen area after every DWM composition:
                                                          # visible, first frame, any other image (exit 9); no image
                                                          # of the screen is written. Use --config with early_window
    build/ronin_bench.exe --bench-longline                # a 5 MB one-line minified JavaScript file: typing at its end
                                                          # and middle, moving in its middle
    build/ronin_debug.exe <file> --keys ".." --crash [--recovery-dir DIR]   # an access violation after the keys:
                                                          # the recovery files (default build\tmp\recovery in dev
                                                          # batch runs), its log in build\crash.log

The benches never take the focus (shown cloaked without activation, as the smoke and --idle-check);
screenshot runs take the workers' results (project scans, searches, builds' output; until none came for
300 ms, or with --compile-wait until the builds ended; git's answers to C-x g until its jobs are done, 10 s at most)
before their frame.

The smoke, the benches and the measurement flags (--memory-report, --bench-latency,
--startup-capture), --type-ahead-check and --leak-check read only the built-in config (deterministic)
unless --config is given (win32_dev_user_config); every other run reads the user's ronin.conf as
usual. Use --config build\tmp\... for checks, so open-config never touches the real %APPDATA%\ronin\ronin.conf. The benches report
the final flush and the Present separately, the Presents that waited a vertical blank (>= 5 ms)
and the presentation mode: "overlay" (DWM shows the window directly) makes Present(0, 0) wait
one refresh interval with two buffers; that is not CPU work of ours. --test, the smoke, the
benches and screenshot runs use an in-memory fake clipboard: they never touch the real one
(interactive dev runs and --clipboard-check do). Dev builds log the private bytes at the startup stages
("memory:" lines in build\ronin.log) and the startup timeline: each stage in ms since process
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
and the hollow one on the right, the right window's active region not drawn (only the selected window
shows its region); then back to one window. The project finder over a known in-memory list with "ap"
typed (stage 7): the prompt color, the selected row's empty part in completion_selection, the matched
characters of the selected and of another row in completion_match, the unmatched part in the text
color, an unselected row's empty part in the background. A results buffer (stage 8): the real project
search for "foo" over build\tmp\smoke_grep (one matching file, two that do not match) in *search*, shown in
the other window without selecting it, its workers done before the frame: the file name in search_file,
"line:col" in search_line_number, the match on lazy_highlight with its text over it, the other text in the
text color on the background, the window's inactive mode line; then the window and the buffer go. A build (stage 9): the --child program
in build\tmp\smoke_compile (an error and a warning on a.c, exit 2) into *compilation*, not selected: the file
name in search_file, "(1,5)" in search_line_number, "error" in compilation_error, "warning" in
compilation_warning, the mode line's "exit [2]" in its cells; then the window and the buffer go. Three cursors
with regions (stage 10): on lines 0-2 of *scratch*, focus on, each filled cursor cell, each region in the selection
color, the cells after the lines in the background, "mc:3" drawn in the mode line; then one cursor again. The command
list (stage 11): *scratch* with three short lines, point at the end of the first, the echo area empty, that frame kept;
C-S-p and "end": the box's frame in modal_border (left and top edges), the field line's empty middle in
modal_background with its filled cursor after the 3 characters, the selected row's empty part in modal_selection, its
keys in modal_key, the next row's description in modal_description, the window underneath untouched right of its lines
and its cursor hollow; then C-g, whose frame must equal the kept one byte for byte (exit 3 otherwise); then C-S-p "end"
RET must have run end-of-buffer and closed the box (exit 9 otherwise). Every
syntax frame also has a number pixel in its color. The window
title must be "*scratch* - ronin",
set exactly once, and the window class must have both icons, the 16 px one drawing a stem pixel in the
text color, the background beside the "r" and a transparent corner. Then the git changes view (stage 12, after the
title's check since it visits files; skipped with a log line when git.exe is not on PATH): build\tmp\gitrepo\repo
(made once by git_dev_make_repo, its working tree set again: a.c changed, c.c deleted, d.txt new) with the real git, its
system and global configuration shut out, b.c in the window: C-x g, the frames until git's answers are in: the diff
of a.c with "diff --git" in diff_header, the @@ line in diff_hunk, "-int b;" in diff_removed, "+int B;" in diff_added,
the empty pane below its lines in modal_background, the next row's " D" in git_status, the selected row's empty part in
modal_selection, the status line in modal_description, the window underneath untouched and its cursor hollow; RET must
visit a.c at line 2 (exit 9 otherwise); C-x g again and C-g: that frame must equal the one before byte for byte (exit
3). Then the terminals (stage 13) with the fake terminal: build\tmp\smoke_term (a .ronin-project, sub\a.c in the window,
wt.exe "found"), C-c t, then C-c a: its calls must be exactly `<sub>|wt.exe|0|-d "<sub>"` and `<root>|wt.exe|0|-d
"<root>" "<system>\cmd.exe" /s /k "claude"` (exit 9 otherwise), the frame above the echo line equal to the one before
byte for byte (exit 3), the echo "Opened claude in <root>". Then: the font was set up exactly once at startup; build\tmp\smoke_keys.txt is
edited and saved through the --keys path ("M-> RET h i C-x C-s") and its bytes compared, and
so is a scripted session in build\tmp\smoke_session.c (a function typed with RET only, a region
killed and yanked, undo, undo-redo, a merged run of typing undone, comment-line); a config with background = #102030 is loaded (as C-c r), the
pixel probed, and the font must not have been set up again. C-x C-+, C-x C-0, C-x C-- and Ctrl +
wheel, then a config with font_size = 14, each set up the font again with a larger or smaller cell
(C-x C-0 and the wheel back to the same one). WM_QUERYENDSESSION is sent to its own
window with an unsaved file (refused, the block reason "Unsaved changes in ronin", the save question; a repeated query
keeps that chain; C-g keeps the reason; WM_ENDSESSION(FALSE) removes it) and again once it is
saved (allowed). It also requires a non-empty atlas,
the D3D11 debug layer active with zero WARNING+ messages, and no leaks (device refcount 0, empty
DXGI live-object report, DirectWrite references 0, no directory watch left open).
Exit codes: 1 fatal, 2 renderer init, 3 pixel mismatch, 4 no debug layer, 5 debug-layer
messages, 6 leak (D3D, DirectWrite, buffers, live markers, watches, threads, events, build processes), 7 output file, 8 font /
ClearType (also: the font set up more than once), 9 test failure (--test; in the smoke: the
--keys edit saved the wrong bytes, the end-of-session check, or the command list's RET), 10 unknown argument (every build).
Every dev exit with a window ends with an "exit-check:" block in build\ronin.log (Phase 16): with everything else
released, ReportLiveDeviceObjects lists only the device and its context, every arena but the permanent one and the
frame scratch is released, and the handle ledger (the kernel handles the os_* layer opened) is 0; the smoke and
--leak-check exit 6 otherwise.

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
something else on the layout are listed), Alt+F4. Also the command line's files and +LINE:COLUMN. The
modifier test, the key test and every harness case are shielded from real input (DevShield): each waits
until no physical modifier is down and checks that its key state held; a case whose key state did not
hold (passed or failed) is logged as "test: inconclusive: ..." with the reason and repeated (at most 5
attempts, then "key state unstable"); a case whose state held and whose result is wrong fails at once; a
self-test drives those decisions with fakes. Phase 11: threads (a lock under contention, events, the wake), a tree watch (500 files: one
wake-up until re-armed), junctions and small reads; ignore rules (globs and 30 decisions); project roots (a
fake file system table, real directories, the cache, the built-in settings); .ronin-project; the scanner
on generated trees in build\tmp\p11scan and p11big (the exact set, a junction loop, the file limit, a
cancel, 300 start / cancel rounds each ending within 250 ms, a stuck worker's bounded join); fuzzy
ranking (goldens, spans, sliced and narrowed equal to full scoring on 20,000 paths); the project finder
through the headless app (build\tmp\p11find: query and RET, the empty query, RET with one candidate a
frame, a stale index, the other window, project-refresh, ronin <directory>, a held scan, the file limit,
not in a project, shutdown with a scan running and with a stuck worker); the region in windows. Phase 12:
the search core on plain bytes and the second-byte filter against the reference (in the search fuzz);
atomics and a file read on a worker, a background worker's priority; grep_file against a whole-text
reference (2,000 random files, chunks from 16 bytes, short reads) and fixed chunk cases (a match across
chunks, a newline on a boundary, a file smaller than the needle, a needle with a newline, CRLF, a long line
cut, UTF-16 LE / BE, a UTF-8 BOM, binary, max_matches); the workers on build\tmp\p12grep (the exact
matches with 1, 2, 4 and 8 workers, the list published in pieces, skipped and unreadable files, the claim
limit, a cancel, 300 start / cancel rounds, a stuck worker); sharing (a save through ReplaceFileW, a rename
and a delete of files the workers hold open); a buffer's keymap; through the headless app (build\tmp\p12res)
*search*'s exact text with 1, 2, 4 and 8 workers and reruns, the next-error order, RET, n, p, g, q, the
keymap only there, the prompt's default, unsaved buffers in their place and after the list, the cap, the
claim window, killing *search* while it runs, shutdown with a search running and with a stuck worker, a
search over a held scan seeing each of 10,000 files once; occur (its text, its keys, g, a large buffer over
many frames, the buffer killed meanwhile). Phase 13: the landing highlight (RET, n, p, next-error until the next
key); processes through the platform layer (stdout and stderr in order through one pipe, the exit code, the
working directory, a missing directory leaving nothing open, a kill ending a grandchild, a normal end with a
grandchild holding the pipe let go, no handle growth); the error matchers' goldens (MSVC, csc, tsc, MSBuild,
GCC / Clang, includes, Jai, tsc's pretty form; times, URLs, versions and linker lines as traps; kinds without
a location and what does not count); path resolution (each step, the order, the cache); the build settings
and colors; cleaning (CR, ANSI, OEM) and lines across reads (random slicing, the cut); builds of --child
programs (order, exit codes, the cap, a missing command or directory, locations from a .bat that changes
directory); kills (the tree gone, during a flood, 100 start / kill rounds, handles); builds through the
headless app (the prompt, the exact *compilation* text, the mode line's state, the summary, next-error over
errors and a note, RET, n, p, q, g, recompile, every compilation_scroll_output and compilation_save_buffers
value, a second build, C-c C-k, killing the buffer, quitting, a link-only failure, files rewritten by a build,
[build] directory and diagnostics, a build that cannot start, the shutdown with a build running); summary lines
(goldens, not counted) and compilation_relative_paths (Phase 14). Phase 14: marker batches against the sequential
path (ascending, descending, mixed and joined runs, valid UTF-8 and invalid bytes, the swallowed flag); the
multiple-cursor driver (swallowed cursors, line commands with two cursors on a line, the merge rule, keyboard-quit's
two steps, max_cursors); collapsing through the keys; undo with several cursors (merged typing, undo-redo, the chain,
random commands); kill pieces and yanks; the new commands on tricky input and through the headless app (their keys,
mark-all over frames, C-g, the minibuffer's refusals); the mouse and two windows on one buffer; a differential fuzz
of 20,000 commands against the one-cursor commands applied cursor by cursor to a flat text. Phase 15:
the streaming encoder with the gap at every offset; the reduced undo case of seeds 22 / 33 (a failing
random undo sequence is reduced with ddmin); the wheel and other windows keeping several cursors ordered;
the atlas growing without a reset; a bracket on a line longer than the match budget; case labels
completed at three cursors in one flush; a child (this executable) crashing after edits with --crash:
its recovery files byte for byte against a save of the same buffers (CRLF UTF-8, UTF-16 LE with CRLF
and an astral character, UTF-16 BE, a UTF-8 BOM, mixed endings), their sidecars, none for an unmodified
file, then the startup notice, *Messages* and recover-file's two windows; --version among the flags of
every build. Phase 18: every command's description against the rules (and broken ones refused); where-is on the
default keys, shadowed by a results buffer's keymap, behind a prefix bound in an earlier map, in two maps, after a
config that moves a binding; the modal layer through the headless app with a two-pane test owner (the geometry at
70 / 60 / 50 and 90 / 80 / 30, a short box, the 40 x 6 minimum, movement in the focused pane, modal-other-pane
wrapping, pages, refusals, a cancelled prefix, accept, mouse rows, a double click, the wheel, a click outside, C-g,
ESC, a resize closing it, run after close, not over the minibuffer); the command list (C-S-p, every command in table
order, the key texts, the filter's goldens and a query split across name and description with its spans, the ends of
the list, refusals, the field's editing, C-g, ESC, RET running a motion, a region command and a prompting one, the
minibuffer, M-x, the mouse, a config reload while open, a frame too small, no allocation over 21 keystrokes); the
seeded group "modal" (600 random operations: the shown order against a slow full ranking, the selection, the top and
the arena while open, the window underneath after a close); the scanner test's junction removed. Phase 19: programs
started without cmd.exe (the arguments, the environment block laid over ronin's, the idle timeout, a missing program
or directory, PATH with quoted entries and without the current directory); the git parsers' goldens (status records,
renames and copies, a UTF-8 path, the branch forms, no commits yet, git's warnings in the shared pipe (before a record,
before a rename's source, between two hunk-body lines; Phase 20), the file limit;
diffs with two hunks, "--" and "++" lines inside a hunk, CRLF, new, deleted, renamed, binary and mode-only files, the
first changed line, the line limit, a long line; the error line and its cut); git's jobs against the fake git
(--fake-git: the open's two command lines and the three diffs' exactly, exit 1 from --no-index only, not a work tree, a
refusal in git's words, a failing status and diff, git_path nowhere, no git on PATH, git stopped past the line limit,
the timeout, a cancel, the environment block); the view through the headless app on a canned repository (opened only
after rev-parse, the rows and the status line, each diff fetched once per open and none for the files passed over, a
deleted file refused, a diff copied over frames, TAB, scrolling, the wheel, a reload, refusals, g, RET at the first
change, the minibuffer, C-g while pending) and its error paths (nothing opens without git or a work tree, the
timeout before and after the open, C-g and a click outside cancelling without waiting, the shutdown with a hanging
git); the real git on build\tmp\gitrepo\repo (made once; its system and global configuration shut out) equal to the
fake git's canned answers, or "test: skip: git real" without git on PATH; the seeded group "git" (80 random operations:
git's order, no diff twice since the open or g, the view's invariants, the arena after a close). Phase 20: a program
launched and let go (its working directory, a missing program or directory, 100 launches keeping no handle, the
breakaway refused inside a build's job and the retry there); the terminal settings (every value, the length limit, each
diagnostic); the terminal commands through the headless app with the fake terminal (--fake-terminal: the directories of
both commands, every provider and program, the quoting of a drive root, a space and ';', each message, the minibuffer
and the command list, a reload, the guard, 200 launches keeping nothing and every call exactly). In a batch run no
test starts a real terminal, or a git it did not choose: the guard refuses it and counts it (os_dev_real_refused, 0 at
the end of --test, every fuzz worker, the soak and the leak check). A failed
dev ASSERT logs its file, line and condition before breaking, so a crash shows up in build\ronin.log.

Open every screenshot after a visual change and look at it (crop and enlarge for detail);
check exact colors with an independent decoder, e.g. PowerShell `System.Drawing.Bitmap`.
When touching rasterization, check that the three render modes still produce different files.

## Generated files

Everything generated lives under build\ (and dist\ at the root); none of it is tracked, all of it comes
back by itself, and `build.bat clean` removes both. In build\:

| path | made by |
|---|---|
| `ronin_debug.exe`, `ronin.exe`, `ronin_bench.exe`, their `.obj`, `.pdb`, `*_cl.pdb`, `*_dwrite.obj` | build.bat (debug, release, bench) |
| `ronin.map` | build.bat release (the linker's /MAP; read by --map-report) |
| `make_icon.exe`, `make_icon.obj` | build.bat (tools\make_icon.c) |
| `make_themes.exe`, `make_themes.obj` | build.bat (tools\make_themes.c) |
| `gen\themes.h` | build.bat (make_themes, from themes\*.conf: the built-in themes) |
| `gen\quad_vs.h`, `gen\quad_ps.h` | build.bat (fxc, from src\shaders\quad.hlsl) |
| `gen\ronin.ico`, `gen\ronin.res` | build.bat (make_icon, then rc with res\ronin.rc) |
| `ronin.log`, `crash.log` | every dev run; --crash |
| `tmp\` | made by build.bat; filled on demand by --test, the smoke, the benches (the 100 MB files, p11big, p12bench, ...), --soak (tmp\soak), --leak-check (tmp\leak), the --fuzz-seeds workers (tmp\fuzz<k>), the crash tests (tmp\recovery, tmp\crash_rec), the git tests' fake gits (tmp\p19), the real-git test's and the smoke's repository (tmp\gitrepo, tmp\gitrepo-home; made once), --bench-git (tmp\p19bench) and verify's clean-check (tmp\clean_check, removed after it) |
| `shots\` | made by build.bat; screenshots of --screenshot, --dump-atlas and --dpi-check run by hand, and --leak-check's frames |
| `fuzz\` | --fuzz-seeds (the workers' logs) |
| `verify\` | verify.bat (each step's log) |
| `dist\` | dist.bat (the staging folder; the zip goes to dist\ at the root) |
| `proof\` | scratch copies of the repository for a proof, made by hand; never required |

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
| modal_background | #0c3434 |   | modal_border    | #2e8b8b |
| modal_selection  | selection |  | modal_description | #a8977a |
| modal_key        | number  |   | diff_added      | #73e673 |
| diff_removed     | #ff6e6e |   | diff_hunk       | number  |
| diff_header      | variable |  | git_status      | string  |

The swap chain is B8G8R8A8_UNORM (not sRGB): theme colors must reach the screen bit-exact.
