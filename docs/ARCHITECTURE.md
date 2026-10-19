# ronin architecture

## How this project is made

ronin is a learning project. I am building it to learn how a text editor works, from the renderer
and the text buffer to the keymap and the search, and it was developed with the support of AI: the
code was written with Claude Code under my direction, one phase at a time. I review the source code
day by day and improve it as I come to understand each part of it. It is an experimental project,
not a finished product.

## Module map

| file | role |
|---|---|
| `src/ronin.c` | the unity translation unit; includes everything below except the .cpp, in order |
| `src/check_macros.c` | compiled by build.bat with /Zs only (never linked): every system header ronin uses, then `ronin.c`, so a ronin identifier that a system header defines as a macro fails the build |
| `src/base.h/.c` | types, ASSERT, Arena, String8/16, UTF-8<->UTF-16, clipboard text conversion (CRLF), letter case, mini formatter, dev LOG |
| `src/platform.h` | os_* primitives, Key/Event/FrameInput, app entry points — all the core sees of the OS; threads, locks, events, atomics and the main loop's wake (and which os_* functions a worker may call); processes (a command through cmd.exe in a job object, one overlapped pipe; since Phase 19 also a program started directly with an environment laid over ronin's, a read's idle timeout, a program found on PATH; since Phase 20 a program started and let go, `os_process_launch`, and the system directory's programs) |
| `src/render.h` | Rect, Color, r_* API (rects, glyphs, atlas) — all the core sees of the GPU |
| `src/font_backend.h` | extern "C" glyph rasterization API, no Win32 / DWrite types |
| `src/win32_dwrite.cpp` | the only C++ file: DirectWrite calls behind font_backend.h, nothing else |
| `src/font.h/.c` | metrics, CPU glyph atlas (shelf packer), glyph cache, `font_draw_text` |
| `src/buffer.h/.c` | gap buffer, incremental newline index, `buffer_replace`, the undo log (groups, state ids, undo / redo, limit, a multiple-cursor command's cursors), markers (and their live list), batches (deferred marker adjustment for ordered edit runs), language from the extension, file load (encodings, line endings) and save |
| `src/syntax.h/.c` | token kinds, the line lexer interface, keyword tables, the catch-up of line states (budget, convergence), bracket matching and line summaries on tokens (indentation, show-paren) |
| `src/lex_c.c`, `lex_jai.c`, `lex_cs.c`, `lex_js.c` | the lexers: C and C++ (one), Jai, C#, JavaScript and TypeScript (one) |
| `src/command.h/.c` | `Command` (its Emacs name, its one-line description, its flags) and `CommandContext`; the table of every command, lookup by Emacs name |
| `src/view.h/.c` | headless view logic: visual columns, View (cursors, scroll, the position it remembers for every buffer it showed), the command driver `view_run_command` (undo boundaries and the cursors kept with them, shift-select, region rules, kill appending, clipboard; with several cursors the ordered pass in one batch, the swallowed and line rules, the kill pieces, the normalization), mark and region, motion and basic editing commands, the buffer list (`view_switch_buffer`), echo messages and their *Messages* log |
| `src/window.h/.c` | headless: the frame's windows, a binary tree of splits whose leaves are Views (fixed node array), the selected window and use ticks (LRU / MRU), layout in whole cells by ratios (minimum sizes, frames too small for them), split, delete, move a split line (adjacent windows only), resize, balance, cyclic order, hit tests (window, divider, mode line) |
| `src/edit.h/.c` | the kill ring (shared arena, large entries, clipboard link, a multiple-cursor kill's pieces), undo / undo-redo (every cursor back), kill and yank commands, rule-based indentation and its commands, the other editing commands (open-line, whitespace, transpose, case, comment-line) |
| `src/keymap.h/.c` | chords, kbd notation, chords from key events, keymaps, the key sequence state machine, where-is (the sequences that run a command with a keymap stack); dev: `--keys` events |
| `src/config.h/.c` | the config parser (settings, colors, keys), defaults + user file layering, diagnostics, the reload state machine (`config_poll`); since Phase 21 a theme file's parse mode ([theme] and [colors] only), the theme setting, the chosen theme laid under the user file's [colors] (`config_resolve_theme`), the theme file's identity in the reload's check |
| `src/config_default.h` | the built-in configuration (the same format as ronin.conf), embedded as a C string; since Phase 21 its [colors] section is a commented list of the keys (the values are in themes\ronin.conf) |
| `src/theme.h/.c` | Phase 21: the built-in themes (build\gen\themes.h), the theme names, the lookup in the two themes\ folders before the built-in theme (`theme_read`, `theme_locate`), every theme there is (`theme_list`, set-theme's candidates) |
| `themes/*.conf` | the built-in themes' sources (ronin, moonfly, oxide, vague, mellifluous): a [theme] block (name, source, commit, license, author) and [colors]; embedded by tools/make_themes.c |
| `src/minibuffer.h/.c` | the matcher (folding, terms, exact / prefix / substring ranking, narrowing); the minibuffer: a one-line View on its own Buffer, prompt kinds, continuations and chains, abort, history, candidates and filtering, a prompt's own matcher (`MiniMatcher`: ranking across frames, rows, spans, a status), the minibuffer commands, M-x, goto-line |
| `src/project.h/.c` | ignore rules (a .gitignore subset, scopes chained per directory), project roots (markers, the command-line directory, drive roots) and their cache, `.ronin-project` (the config parser's lines and diagnostics), the file index (relative paths, 16 bytes a file, a character mask each), the scanner: a worker thread walking a root breadth first, its batches through one locked queue, cancel, join |
| `src/fuzzy.h/.c` | fuzzy matching of paths (subsequences, smart case, terms, '/', the score and its spans) and the resumable ranking over an index (slices against a deadline, narrowing, the best 1,000 in a heap) |
| `src/finder.h/.c` | part of the app (included after files.c): the projects kept (4), their scans and tree watches, `app_wake` (the workers' results), staleness and rescans, the project finder's matcher and its commands (project-find-file, -other-window, project-refresh); dev: smoke and bench helpers |
| `src/search.h/.c` | the search engine: literal text, forward and backward, exact or folded; its core (a pattern, the SSE2 candidate scan on the first and second byte, the comparison) on plain byte ranges, and on top of it the search over the gap buffer's two segments in place, resumable with a budget of positions |
| `src/grep.h/.c` | the project-wide search's worker side: one file in fixed chunks that overlap (encodings, binary files, lines counted, long lines cut), the workers claiming files of an append-only list in order, their batches through one locked queue, cancel, bounded join |
| `src/git.h/.c` | the git changes view's worker side (Phase 19): a GitJob runs git's commands on a worker (rev-parse, status, one diff), each read into its arena with the idle timeout and stopped past the limits, parsed there (status `-z` into entries, a unified diff into lines with a kind, the first changed line, git's error line), one batch per step through one locked queue, cancel, bounded join |
| `src/compile.h/.c` | builds, the worker side: the reader worker owning the process (start, read, kill, close), the lines (split across reads, CR, ANSI, OEM, the cut, the output cap), the error matchers (MSVC, GCC / Clang, includes, Jai, tsc, MSBuild; kinds without a location), the path resolution and its cache, batches through one locked queue |
| `src/results.h/.c` | part of the app (included after finder.c): results buffers and their locations (*search*, *occur*, *compilation*: each location's row and line offsets, a build's marks without a location), the landing highlight, the project search's file list (the index copied, or a running scan followed), the files written strictly in list order (held records, the claim window, buffers searched from memory in their place), project-search, occur, next-error / previous-error and the [keys search-results] commands; *compilation* (its lines inside the frame budget, the scroll rule, the end and its check of rewritten files), project-compile, compile, recompile, kill-compilation, the save, kill and exit questions' chain, the commands remembered per directory; dev: smoke and bench helpers |
| `src/modal.h/.c` | part of the app (included after results.c): a box drawn over the windows, one at a time, taking every key and mouse event while open; its geometry (percentages of the frame, the 40 x 6 minimum, one or two panes), an optional one-line field (its own Buffer and View), the lists it moves, pages, scrolls and hit-tests, the key routing ([owner keymap, global]: quit closes, COMMAND_FIELD edits the field, the rest refused), its drawing, the owner's callbacks, the layer's commands (modal-next ... modal-other-pane); since Phase 19 a status line instead of a field, panes without a selection that scroll, the selection callback, a full height from the open |
| `src/git_changes.h/.c` | part of the app (included after commands_list.c): show-git-changes (C-x g), the modal layer's second owner: the job waiting for rev-parse before the box opens, the box's jobs (status, one diff at a time), the rows and each diff fetched once per open, the copy of a diff into the modal's arena inside the frame budget, RET's visit, g, the dying list and the shutdown; dev: the smoke's and the tests' repository (`git_dev_make_repo`) |
| `src/terminal.h/.c` | part of the app (included after git_changes.c, Phase 20): open-terminal (C-c t) and open-ai-terminal (C-c a): the directory (the file's, the project's root, the default one), the program (wt.exe on PATH or the system's cmd.exe), the command lines and their quoting, the start through `os_process_launch` (fire and forget), the messages; dev: the fake terminal (`dev_program`), the guard, smoke stage 13's setup |
| `src/commands_list.c` | part of the app (included after modal.c): show-command-list, the modal layer's first owner: every command with its description and its bindings (where-is), the fuzzy filter over the name, the description and both, the rows' drawing, RET running the command as M-x |
| `src/cursors.h/.c` | the multiple-cursors commands (mark-next / previous / all-like-this, skip, unmark, the edit-lines family) and the CursorJob, their search spread over frames |
| `src/isearch.h/.c` | isearch (a stack of steps resolved by sliced searches, its commands, the echo prompt), query-replace and replace-string (the prompts, the session, its answers and replace-all across frames, case conversion) |
| `src/app.c` | editor core: config (read before the font, reloaded live), keys through the keymap stack (the isearch and query-replace routing), buffers (unique names), the kill ring, app commands (buffer cycling, open/reload config, text scale, describe-key, quoted-insert), the windows (their View slots, the window commands, the pop-up rule `app_display_buffer`), `app_update` (events, commands, the frame's background work: search slices then lexer states, layout; while a modal box is open every key and mouse event goes to modal.c) and drawing (each window cut to its rect: text, region, the search highlighting, mode line, divider; the minibuffer line, the search echo line, the candidate list), window title, mouse (click, drag, double / triple click, divider and mode-line drags, `app_mouse_cursor`); dev: smoke probes, the headless app of `--test`, `--bench-edit` memory, `--bench-complete` candidates, `--bench-search` helpers |
| `src/files.c` | part of the app (included after app.c): find-file, write-file, save-buffer, switch-to-buffer, kill-buffer (and their other-window forms, kill-buffer-and-window), revert-buffer, save-some-buffers and quitting, the end of the Windows session, files changed on disk (checks, watches, the save guard) |
| `src/test.c` | dev only: `--test` (buffer, marker, column, view, key, config, buffer list and hot reload tests, file round trips, failures, the steps of docs/MANUAL_TESTS.md through the headless app; Phase 11: threads and tree watches, ignore rules, roots, the scanner and its stress, fuzzy ranking, the finder; Phase 12: the search over files and results; Phase 13: processes, the matchers' goldens, path resolution, cleaning and lines, builds and kills of `--child` programs, builds through the headless app, the landing highlight; Phase 14: marker batches, the multiple-cursor driver, undo, kills, commands, the mouse and the differential fuzz; Phase 15: the undo reducer, the crash recovery test, the seed sweep's groups (`--fuzz-seeds`), the soak (`--soak`), the map report; Phase 16: the soak's leak metrics and their rule, the reservations that grew by origin, the regression tests of the leaks found, the caps of the bounded caches, the sampler and the ledger; Phase 18: the command descriptions, where-is, the modal layer with a two-pane test owner, the command list, the seeded modal group, command list sessions in the soak; Phase 19: programs started directly, the git parsers' goldens, git's jobs against the fake git, the git changes view and its error paths, the real git, the seeded git group, git sessions in the soak; Phase 20: the launch and its breakaway retry, the terminal settings, the terminal commands with the fake terminal, the guard, the terminals in the soak; Phase 21: the built-in themes, the resolution order and its seeded group "themes", the theme diagnostics and folders, the masked-theme note, set-theme, the themes' reload through real watches, theme sessions in the soak and set-theme in the modal fuzz, a slow soak event named with its session and its waits; Phase 22: the README check, `test_readme`: README.md's key tables through describe-key or their keymap, its settings tables and ini blocks through the config parser, the prose keys, commands and names, README.md's and CONTRIBUTING.md's links and anchors, and the checker's own broken fragments) and the `--bench-buffer` core |
| `src/leak.c` | dev only (Phase 16): `--leak-check`, the scenarios (modal-cycle since Phase 18, git-cycle since Phase 19, terminal-cycle since Phase 20, theme-cycle since Phase 21) (each a setup, a cycle and a teardown driving the app through the window's queue), the prime step, the state each cycle must leave, the metrics, the verdict (LEAK, STEP, WATCH) and the tables, the screenshots before and after; the platform drives it through `LeakHost` |
| `src/png.c` | dev-only PNG encoder (stored deflate, CRC32, Adler-32) |
| `src/render_d3d11.c` | D3D11 device, flip-model swap chain, instanced-quad pipeline, atlas texture, capture |
| `src/shaders/quad.hlsl` | vs/ps for the quad pipeline, compiled by fxc to `build/gen/*.h` |
| `src/win32_main.c` | wWinMain (command line, the files and their +LINE:COLUMN), window, message loop (with directory watches, one-shot tree watches and the workers' wake event), input translation (click counts, `win32_translate`), the mouse cursor (WM_SETCURSOR), clipboard (and the dev fake), directory listing and small-file reads (any thread), threads, locks and events, processes (the job object, the named pipe, the handle list, the overlapped read with the process and a cancel), the end-of-session messages, os_* implementation, dev flags and helpers (`--log-keys`, `--idle-check`, `--dpi-check`, `--clipboard-check`, `--type-ahead-check`, `--bench-project`, `--bench-compile`, `--bench-cursors`, `--scan-hold`, `--compile-wait`, junctions, the input tests' shield; Phase 15: `--check-imports`, `--memory-report`, `--bench-latency`, `--startup-capture`, `--bench-longline`, `--crash`, the fuzz workers; Phase 16: `--leak-check`'s host (the pass of the main loop, the capture), the metrics sampler (`os_dev_metrics`, the handles by type, the memory by owner, the reservations and their origins), the handle ledger, the exit-time check, the main loop's wait shared with it (`win32_wait`, `win32_after_wait`), the worker thread slots reused; Phase 18: `--bench-modal`, the smoke's stage 11 and its frame comparison; Phase 19: `--bench-git`, the smoke's stage 12, the git view's wait in screenshot runs, `--fake-git`'s dispatch; Phase 20: `os_process_launch`, the smoke's stage 13, `--fake-terminal`'s dispatch, the guard's `os_dev_batch_run` and its count; Phase 21: the smoke's stage 14, `os_watch_dir`'s subdirs flag), the CRT-free entry point, the crash filter and the recovery files' os_* calls, `--version` |
| `src/win32_child.c` | dev only: `--child <script>`, the scripted program the build tests run (lines on stdout and stderr, raw bytes, sleeps, an exit code, a hang, a grandchild, a flood, the working directory, a file rewritten, an environment variable, numbered lines); `--fake-git DIR ARGS`, the tests' git (Phase 19): the calls logged, a script picked by the arguments; Phase 20: `launch FILE` (a grandchild through `os_process_launch`) and `--fake-terminal DIR ...`, the tests' terminal (its working directory and raw command line logged) |
| `src/win32_input_test.c` | dev only, in `--test`: the real input path (posted key messages, TranslateMessage, the window procedure) on US, United Kingdom, Turkish Q and Finnish layouts loaded for its thread, into a headless app |
| `res/ronin.manifest` | PerMonitorV2 DPI, longPathAware, supportedOS Windows 10 |
| `res/ronin.rc`, `src/version.h` | the icon (build\gen\ronin.ico) and the version resource; the version for the code (`--version`) |
| `res/imports.txt` | the DLLs the release exe may import (checked by verify.bat with `--check-imports`) |
| `tools/make_icon.c` | build-time tool, not part of the exe: draws the icon into build\gen\ronin.ico on every build |
| `tools/make_themes.c` | build-time tool, not part of the exe (Phase 21): embeds themes\*.conf into build\gen\themes.h on every build (comment and blank lines dropped, name order, ronin required, at most 8 KB in all) |
| `build.bat`, `verify.bat`, `dist.bat` | the build, the gate before every commit, the release zip |

Core (`app.c`, `files.c`, `finder.c`, `results.c`, `modal.c`, `commands_list.c`, `git_changes.c`, `terminal.c`, `git.c`, `cursors.c`, `font.c`, `buffer.c`, `view.c`, `window.c`, `edit.c`, `minibuffer.c`, `project.c`,
`fuzzy.c`, `search.c`, `grep.c`, `compile.c`, `command.c`, `keymap.c`, `config.c`, `theme.c`, `test.c`) includes only `platform.h` and `render.h`; `font.c`
additionally calls `font_backend.h`. The scanner's worker (project.c), the search's workers (grep.c), the build's reader
(compile.c) and git's worker (git.c) call only the os_* functions platform.h marks "any thread".

## Startup and frame loop

1. `wWinMain` (reached through `win32_entry` in the CRT-free builds) installs the crash filter, parses
   the command line (`--version` prints and exits; `--test` runs headless here and exits), then allocates
   the Renderer and immediately starts a worker thread that loads d3d11.dll and runs only
   `D3D11CreateDevice` (`r_create_device`: no logging, no arenas, results go into Renderer
   fields). Meanwhile the main thread creates the window hidden on the monitor under the
   mouse, sizes it for that monitor's DPI (clamped to the work area) and creates the app,
   which reads the config (built-in defaults, then the user's ronin.conf), opens DirectWrite
   once with the configured font, pre-rasterizes ASCII and opens the files of the command line. With
   `early_window` the window is shown here, painted in the background color, before the device exists.
2. Shows the window cloaked (DWMWA_CLOAK) and activates it (SW_SHOW, SetForegroundWindow) right
   after the app is created: keys typed from then on queue for ronin, and the activation (the focus
   sets up the IME and text services, ~15 ms) overlaps the device creation. (Activating right after
   the window was created instead slowed the device thread down: first Present 175 ms against 169.)
   It joins the worker with MsgWaitForMultipleObjects on QS_SENDMESSAGE, so only sent messages are
   handled meanwhile and posted input stays queued, in order, with its key state; logs the worker's
   results and creates swap chain, pipeline and atlas texture (`r_finish_create`). The first frame is
   presented while still cloaked, then the window is uncloaked: no white flash. The main loop then
   reads the keys typed ahead. If Windows refuses the foreground, the window is still shown and
   appears when uncloaked. The smoke and `--idle-check` show it without activation, screenshot runs
   and `--dpi-check` never show it.
3. Loop: if no redraw is pending, block in `MsgWaitForMultipleObjectsEx` on the messages, the
   directory watches (a project's tree watch only while armed) and the wake event of the workers, with
   no timeout unless the app asks for one (`app_wait_ms`: only while a config read is to be retried).
   Signalled watches become `EVENT_DIR_CHANGED`; the wake event makes the loop call `app_wake`, which
   takes the workers' batches and asks for a frame only when something shown changed (also when the wake
   itself ended the wait, which resets that auto-reset event: until Phase 16 those results waited for the
   next wake that came while a frame was due). Drain every queued
   message; the window procedure turns them into `Event`s in a fixed array (consecutive
   mouse-move and resize events are coalesced). If the array is nearly full, run a frame
   mid-drain instead of dropping anything. If anything requested a redraw, run exactly one
   frame: `app_update_and_render` → `font_frame_begin`, `r_begin_frame`, `r_push_rect` /
   `font_draw_text`…, `r_end_frame` (flush + `Present(1, 0)`). Scratch resets after each frame.
   Background work has one deadline per frame (2 ms): search slices first (isearch, query-replace,
   replace-all), then a prompt's ranking (the project finder), then the results buffers' work (the
   project search's list and its files written in order, buffers searched from memory; not in frames
   that carry input), then a diff copied into the git changes view (Phase 19; not in frames that carry input either),
   then lexer states with what is left; while any is pending the app asks for another
   frame, otherwise none.
4. `WM_SIZE` resizes the swap chain and renders immediately, so live resize stays crisp
   inside the modal size loop. Minimized = frames are no-ops, and the driver's temporary
   allocations are trimmed (IDXGIDevice3::Trim).

Renderer: one pipeline, instanced quads. VS builds the strip corners from `SV_VertexID`;
the only vertex buffer is a dynamic per-instance buffer (rect, uv rect, RGBA8 color, kind;
40 bytes, 16,384 per flush). Kind 0 = solid, kind 1 = glyph: the PS fetches coverage with
`Texture2D.Load` (quads are 1:1 with texels, no sampler) and outputs color in SV_Target0 and
per-channel weights in SV_Target1 (dual-source blending, SRC1_COLOR / INV_SRC1_COLOR). Each
flush first uploads the atlas dirty rectangle. Pipeline state is rebound every frame because
the flip model unbinds the back buffer on Present.

Text: `font_draw_text` walks UTF-8; a byte in 32..126 is a table lookup plus one instance
write, anything else goes through an open-addressing hash keyed by codepoint and is
rasterized on first use (DirectWrite, white on black, into a GDI DIB; copied as R, G, B
coverage into the CPU atlas, which commits 128 rows first and doubles them as shelves need, its
texture made again at the new height). When the atlas or the hash table (load factor > 70%) fills up,
pending quads are flushed, atlas and cache are cleared, ASCII is re-rasterized, drawing goes on.

Input: modifiers come from `GetKeyState` at message time. The only tracked modifier state
is AltGr: a non-extended Left Ctrl immediately followed by an extended Right Alt with the
same message time is the synthetic AltGr Ctrl (GLFW technique) and is dropped. While AltGr
is held, its own Ctrl and Alt are not modifiers, but Left Alt still counts as ALT and Right
Ctrl as CTRL; WM_CHAR becomes text only without Ctrl and Alt. KEY_DOWN carries the character
the key produces with the current Shift / AltGr state (ToUnicodeEx with Ctrl and Alt cleared,
flag 0x4 so the kernel's dead-key state is untouched; a dead key gives its spacing accent).
Alt works as Meta (WM_SYSKEY* handled, WM_SYSCHAR swallowed, SC_KEYMENU swallowed); Alt+F4
still closes, through save-buffers-kill-terminal. Numpad: Enter arrives as KEY_ENTER,
NumLock-off navigation keys as the ordinary ones, digits as text events.
Every key but the modifier and lock keys sends a KEY_DOWN, with KEY_NONE when ronin has no Key
for it (keypad digits and operators, VK_OEM_8, media keys): it ends the previous chord's text,
and makes a chord when it types a character (C-? on Turkish Q's VK_OEM_8); the keypad carries no
character, so it makes no chords and Alt + keypad digits still enter a character code. A KEY_DOWN
also says whether the key is dead and whether no text event follows (TranslateMessage has queued
its characters before the window procedure runs), so describe-key can report a key press that is
no chord; KEYMOD_ALTGR marks AltGr, for information only. After a Ctrl or Alt chord a pending
dead-key accent is consumed: TranslateMessage stored it (M-^ where ^ is dead) and the next key
would compose with it. The modifier bits are KEYMOD_*: winuser.h's MOD_ALT / MOD_SHIFT
(RegisterHotKey) replaced the former MOD_* names in win32_main.c (Alt read as Ctrl).

Keys: KEY_DOWN and text events go through the key sequence state machine (keymap.c) with the
keymap stack; the result is a command run through `view_run_command`, a prefix shown at once
("C-x-"), an undefined sequence, or a description (describe-key).

## Measurements (Phase 3)

`build\ronin_bench.exe` (/O2, dev flags, no debug layer), 1280x800 client, Consolas 16 px.
`--bench-buffer` on a generated 104,857,642-byte file with 1,963,818 lines (warm file cache):

| what | time |
|---|---|
| load (read straight into the buffer + SSE2 index pass) | 35-40 ms (~2.9 GB/s) |
| 10,000 single-character inserts at one spot | first 5.0 ms (gap moves to the middle), then 37 ns avg, 1 us worst |
| 1,000 inserts at random positions (each moves the gap) | 2.3 ms avg, 7.8 ms worst |
| 1,000,000 `buffer_line_of` | 73 ns avg |
| save-as, flushed / not flushed | 72 ms / 58 ms |
| frame build (`app_update_and_render` to `r_end_frame`) at top / middle / end | 10 / 10 / 9 us avg |

Startup with the 100 MB file on the command line: first Present ~190 ms, same as with no file
(the load overlaps the device creation). `--bench-text` (5,680 glyphs): build 33 us avg.

## Measurements (Phase 4)

`build\ronin_bench.exe`, same machine and file. `--bench-view`, one event per frame through
`app_update_and_render` (command + frame build; flush + Present(0, 0) separately, ~0.1 ms):

| what | command + frame build |
|---|---|
| 10,000 next-line from the top | 12 us avg, 246 us worst |
| 1,000 PageDown | 15 us avg, 75 us worst |
| 100 C-End / 100 C-Home | 11 / 12 us avg, 47 / 42 us worst |
| 10,000 self-inserts at line 1,000,000 | 25 us avg; worst 5.0 ms = the first insert moving the gap to the middle |

`--bench-buffer` frames (now with cursor and mode line): 13 / 35 / 15 us avg at top / middle /
end. Release exe 156,672 bytes (132,608 before Phase 4), same six imports. Startup ~187 ms,
with or without the 100 MB file.

## Measurements (Phase 5)

`build\ronin_bench.exe`, same machine (2560x1440 at 144 Hz), 1280x800 client.

- The ~7 ms "flush + Present" of the Phase 4 bench-buffer run: the benches now split it and log
  the presentation mode. Whenever Present takes ~6.9 ms (one 144 Hz refresh interval), DWM
  reports the swap chain as "overlay": it has promoted the window to a hardware overlay plane
  after it presented for a while, the plane holds the buffer on screen until the next vertical
  blank, and with two buffers Present(0, 0) must wait for it. Runs reported "composed by DWM"
  present in ~0.1 ms. Both happen run to run. The frames upload no atlas texels and the final
  flush takes 1-3 us, so it is not CPU work of ours; real frames present with vsync anyway.
- Command + frame build (bench-view): next-line 12-23 us avg, PageDown 15-21, C-End / C-Home
  12-20, self-insert 18-29 (worst 5 ms: the first insert moving the gap): the keymap adds
  nothing measurable to Phase 4.
- Config: parsing the built-in config (4839 bytes, 49 bindings) 8 us; reading and parsing the
  defaults plus a 4.8 KB ronin.conf at startup 140-230 us (mostly opening the file).
- Startup (`--startup-ms`, release, 10 runs alternating): 189 ms median before Phase 5, 190 ms
  without a ronin.conf, 191 ms with a full one. The font is set up exactly once.
- Idle with the config directory watched: 0 ms CPU over 10 s.
- Release exe 188,928 bytes (156,672 after Phase 4); the same six imports (kernel32 adds
  GetEnvironmentVariableW, Find*ChangeNotification, CreateDirectoryW; user32 adds ToUnicodeEx,
  GetKeyboardState, GetKeyboardLayout).

## Measurements (Phase 6)

`build\ronin_bench.exe`, same machine; the release build for the memory figures.

- Typing (bench-view, 10,000 self-inserts on one line at line 1,000,000, command + frame build):
  Phase 5 build 22-38 us, Phase 6 build 29-38 us in the same runs (alternated; the spread is
  the machine, composed vs overlay presentation). Logging an insert into the undo log costs
  17 ns at the buffer level (37 -> 54 ns per insert). Point's column is scanned from the line
  start on every view_ensure_visible; on the 10,000-character line that is ~5 us a call, and
  the frame made three of them; it now makes two (perf commit), which keeps typing in the
  Phase 5 range.
- --bench-edit on the 100 MB file: 10,000 self-inserts with undo on, 29 us a frame; undo log
  after them 383 KB committed (groups of 20). kill-region of the second half (~50 MB) 101 ms
  command + frame (one copy into the kill ring, one into the undo log, one into the clipboard
  as UTF-16); its undo 36 ms; a yank of it at the end 39 ms; the undo of that 8 ms. Undo log
  after that 51 MB committed (limit 64 MB: older groups dropped, the commit given back), kill
  ring 51 MB (the entry has its own reservation).
- Startup (release, 7 runs) 191 ms median (190 in Phase 5). Release exe 219,648 bytes (188,928
  after Phase 5); the same six DLLs (user32 adds the clipboard functions, GetDoubleClickTime,
  GetSystemMetricsForDpi; kernel32 adds Global* and VirtualFree).
- Memory, release build, private bytes idle (3 runs each): *scratch* only 78-80 MB; with
  src\keymap.c (14 KB) 79-80 MB; with the 100 MB file 189-190 MB. Dev build at the startup
  stages: 4.3 MB before the app, +5 MB for the app (font atlas 4 MB, DirectWrite, buffers),
  +50 MB with the D3D11 device (driver), +4.4 MB swap chain and pipeline, +12.6 MB after the
  first frame (driver): of ~76 MB, ~67 MB is Direct3D and the driver. An extra open buffer
  (2 KB file) adds 439 KB of private bytes; its own commit is 384 KB: meta 64, text 128, line
  index 128, markers 64 KB (64 KB commit steps; undo commits nothing until the first edit).

## Measurements (Phase 7)

`build\ronin_bench.exe`, same machine (2560x1440 at 144 Hz), 1280x800 client; the release build for
memory, startup and size.

- Small buffers: commit steps grow 4, 8, 16, 32, then 64 KB. An extra 2 KB buffer: 71 KB of private
  bytes (439 KB in Phase 6), its own commit 16 KB (384 KB): meta 4, text 4, line index 4, markers 4,
  undo 0. A load gets size / 16 of gap slack, as every later growth does (without it the first
  growth while typing moved half of a 100 MB file: a 4 ms spike).
- --bench-complete, per keystroke that changes the input (147, three rounds of typing, deleting
  and clearing), filter + rank / command + frame build:

| candidates | avg | worst |
|---|---|---|
| 10,000 | 67 us / 73 us | 232 us / 300 us |
| 100,000 | 0.65 ms / 0.65 ms | 2.0-2.1 ms / 2.1 ms |

  Building, folding and ranking a set: 1.1 ms for 10,000, 11 ms for 100,000. The first version
  (a byte loop, one pass per score group, every keystroke over all candidates) took 2.2 ms avg,
  3.2 ms worst for 100,000; the SSE2 scan for a term's first byte and one placing pass gave
  1.85 / 2.5 ms; narrowing while the input only grows gave the numbers above. The cost is ~18 ns per
  candidate scored at 10,000 and at 100,000 alike: compute, not memory. The worst case is a DEL
  that widens a three-word input back to a full pass.
- Typing (bench-view, self-insert at line 1,000,000, command + frame build, alternated with the
  Phase 6 build): Phase 6 25-26 us avg, Phase 7 22-23 us; worst ~5 ms in both (the first insert
  moves the gap to the middle). next-line 13 us in both.
- Revert of the 100 MB file after a one-line change outside: ~120 ms (load into a temporary buffer,
  compare, one replace); the undo log grows by one small record.
- Idle with a file open (its directory watched, plus the config's): 0 ms CPU over 10 s.
- Startup (release, 7 runs) 193 ms median (191 in Phase 6).
- Memory, release build, private bytes idle (3 runs each): *scratch* only 77.7-78.2 MB; with
  src\keymap.c 78.5-79.1 MB; with the 100 MB file 196.6-197.0 MB (189-190 in Phase 6: the gap slack
  of the load, ~6.5 MB). Dev build at the startup stages: 4.7 MB before the app, +5.1 MB for the
  app, +49 MB with the D3D11 device, +4.2 MB swap chain and pipeline, +12.8 MB after the first frame.
- Release exe 260,096 bytes (219,648 after Phase 6); the same six DLLs; user32 adds
  ShutdownBlockReasonCreate and ShutdownBlockReasonDestroy.

## Measurements (Phase 8)

`build\ronin_bench.exe --bench-syntax`, same machine (2560x1440 at 144 Hz), ~100 MB of generated source per language:

| language | lexer, state only | lexer, with tokens | catch-up over a buffer |
|---|---|---|---|
| Jai | 688 MB/s | 644 MB/s | 794 MB/s |
| C | 548-565 MB/s | 519-540 MB/s | 540-642 MB/s |
| C++ | 526 MB/s | 504 MB/s | 598 MB/s |
| C# | 581 MB/s | 550 MB/s | 601 MB/s |
| JavaScript | 487 MB/s | 460 MB/s | 498 MB/s |
| TypeScript | 452 MB/s | 439 MB/s | 511 MB/s |

- 100 MB C file (3.5 M lines): "/*" typed on line 1 (40 us), then M->: 79-88 frames of ~2 ms (longest
  2.05 ms) until the colors are right, 164-197 ms with Present(0, 0), ~0.6 s at one frame per 144 Hz
  refresh. The whole file had never been lexed (states are computed only as far as a view needs).
- Typing (10,000 self-inserts at line 1,000,000, command + frame build, alternated runs): Phase 7
  bench-view 24-33 us; Phase 8 highlighted C 22-33 us, the same buffer in Fundamental 17-28 us. Worst
  ~2.1 ms: the first insert moving the gap from the top (5-6 ms in bench-view).
- Idle after catching up 50 MB (opened at line 1,000,000): 0 ms CPU over 10 s.
- Memory (release, private bytes idle, 3 runs): *scratch* 75.8-77.6 MB; src\keymap.c 75.9-77.6 MB; the
  100 MB .txt 192.4-193.6 MB; the 100 MB .c 216.4-217.7 MB (3.5 M lines: line index and states 17 MB
  each; states are committed with the index, 4 bytes per line, only for languages with a lexer).
- Startup (release, 7 runs) 198 ms median (193 in Phase 7). Release exe 296,448 bytes (260,096 after
  Phase 7); the same imports.

## Measurements (Phase 9)

Same machine (AMD Ryzen 7 7700X, 2560x1440 at 144 Hz), 1280x800 client.

- Startup, where the time goes (bench build, "startup:" lines): process creation to WinMain 12-15 ms
  (loader: imports, static CRT); arguments and the device thread's start 0.6 ms; the hidden window
  4-6 ms; the app 2-20 ms (DirectWrite's first use dominates: 2 ms warm, ~20 ms cold; config 0.25 ms,
  syntax_init 0.07 ms, buffers, minibuffer and kill ring 0.15 ms), all overlapping the device thread;
  waiting for D3D11CreateDevice 125-150 ms; swap chain, pipeline and atlas texture 3-4 ms. Then
  ShowWindow took 13-17 ms before the first frame could start: ~10 ms of it between WM_ACTIVATE and
  WM_IME_NOTIFY (the focus sets up the IME and text services), the rest DWM. The first frame: update
  0.3 ms, build 0.04 ms, flush (atlas upload) 0.6 ms, Present 1.1 ms; a second frame, whose Present
  waited 3-6 ms, was drawn before uncloaking. Nothing of ours above 0.2 ms was unneeded for the first
  frame (syntax_init is 0.07 ms and runs while the device is created, so it stayed); the window is now
  activated after the first frame is on screen and the second frame is gone. `--startup-ms` (release,
  9 runs alternating with Phase 8): 186 -> 175 ms median; again at the end of the phase 192 -> 178 ms
  (205 -> 190 under load).
- `--bench-search` on the 100 MB file, a needle only at its very end and one that does not occur;
  frames with Present(0, 0); build = command + frame build including the search slices:

| needle | typed, 34 key events | pasted, one step |
|---|---|---|
| at the end, folded | 41-51 ms, 34 frames, longest 2.13 ms | 39-41 ms, 19-20 frames, longest 2.11 ms |
| at the end, exact | 8-10 ms, 34 frames, longest 2.05 ms | 4 ms, 4 frames (M-c C-y), longest 2.04 ms |
| not in the file, folded | 41-42 ms, 34 frames, longest 2.15 ms | 39-45 ms, 19-20 frames, longest 2.15 ms |
| not in the file, exact | 8-10 ms, 34 frames, longest 2.05 ms | 4-6 ms, 3-4 frames, longest 2.03 ms |

  Folded runs at ~2.6 GB/s (the needle's first letter is common, every hit is verified); exact at
  ~25 GB/s (its first byte, a capital, never occurs). No frame exceeded the 2 ms budget by more than a
  slice. isearch keystrokes on src/app.c (typing, repeats, DEL, RET, with the lazy highlight): command +
  frame build 27-36 us avg, worst 156-278 us (one run had a 4.3 ms outlier).
- replace-string foo -> baz over 1,000,000 occurrences (8 MB): 70-80 ms in 32-35 frames, longest frame
  2.24-2.30 ms; the undo log grows by 39 MB (a 32-byte record and 8 bytes of removed text per
  replacement). The one undo of those million replacements: 32-38 ms in one frame; its undo-redo the same.
- Typing (bench-view, 10,000 self-inserts on one line at line 1,000,000, command + frame build): the
  phase's commits measured 17-26 us without following the code (code layout: a commit that only added
  a dev log moved it by 5 us, padding the App struct moved nothing, a timing patch flipped which build
  was faster); the column scan of that long line was most of the frame. With printable ASCII scanned
  16 bytes at a time (perf commit): 6-7 us (Phase 8 build in the same runs: 17-19 us); next-line 15 us
  in both.
- Idle after an isearch left active, an isearch ended, and a query-replace asking: 0 ms CPU over 10 s.
- Memory (release, private bytes idle, 3 runs): *scratch* 77.0-77.4 MB; src\keymap.c 76.3-77.4 MB; the
  100 MB .txt 191.9-193.4 MB; the 100 MB .c 216.1-217.3 MB (Phase 8: 75.8-77.6, 75.9-77.6,
  192.4-193.6, 216.4-217.7).
- Release exe 323,584 bytes (296,448 after Phase 8); the same six DLLs and the same imported functions.

## Measurements (Phase 10)

Same machine; the display was 1920x1080 at 75 Hz during this phase (2560x1440 at 144 Hz before),
1280x800 client.

- The macro check (`cl /Zs src\check_macros.c`, every build): ~0.3 s. It found winuser.h's (and imm.h's)
  MOD_ALT / MOD_SHIFT against platform.h's modifier bits, rpcndr.h's `small` (through d3d11.h) against
  KillRing.small, and minwindef.h's `near` / `far` against two test locals; all renamed. During the phase
  it caught a new `far` local in window.c on its first build.
- Startup (`--startup-ms`, release, 15 runs each, the builds in rotating order): Phase 9 169 ms median,
  window shown and activated right after the window was created 175, after the app (kept) 170. Run in
  a fixed order (new, then old) the new build looked 9 ms slower in three series; rotating the order
  removed that. `--type-ahead-check`: the window is the foreground one ~35-50 ms after process
  creation; keys sent 20-83 ms after WinMain, the first Present at 160 ms, all five in *scratch* in
  order.
- `--bench-windows` on the 100 MB file (command + frame build, two rounds): 300 frames with one window
  18-21 us avg (worst 62-215 us); with four windows at the top, middle, line 1,000,000 and end 32-37 us
  avg (worst 75-224 us). 10,000 self-inserts at line 1,000,000: one window 8-10 us avg, two windows on
  that place 15-22 us avg (worst 271-310 us; the first insert's gap move 6 ms in the first round).
- Typing with one window (bench-view, self-insert at line 1,000,000, alternated with the Phase 9 bench
  build): Phase 9 12-13 us avg, Phase 10 13-14 us; next-line 21-28 / 25-29 us.
- Idle (`--idle-check`): 0 frames, 0-15 ms of CPU over 5 s. Runs disturbed by real mouse or keyboard
  input (the window opens under the mouse) now show why: the messages that reached the window.
- Memory (release, private bytes idle, 3 runs): *scratch* 76.1-77.3 MB; src\keymap.c 77.1-77.8 MB; the
  100 MB .txt 191.8-193.4 MB; the 100 MB .c 216.2-216.9 MB (Phase 9: 77.0-77.4, 76.3-77.4, 191.9-193.4,
  216.1-217.3). A window costs a View slot inside App and two markers per buffer it remembers.
- Release exe 343,552 bytes (323,584 after Phase 9); the same six DLLs; user32 adds SetCursor and
  MsgWaitForMultipleObjects.

## Measurements (Phase 11)

Same machine (AMD Ryzen 7 7700X, 2560x1440 at 144 Hz), 1280x800 client; `ronin_bench.exe --bench-project`.

- Ranking a query typed a character at a time ("winmai"), then deleted, twice (24 keystrokes); frame =
  command + frame build; "until ranked" includes the Presents of those frames (one refresh each when DWM
  shows the window as an overlay):

| paths | index | keystroke's frame avg / worst | frames until ranked avg / worst | longest frame | a whole ranking at once (RET) |
|---|---|---|---|---|---|
| 10,000 | 388 KB | 274 / 776 us | 1 / 1 | 776 us | 85 us |
| 100,000 | 3.3 MB | 1,462 / 2,062 us | 1 / 2 | 2,062 us | 1,046 us |
| 1,000,000 | 32 MB | 1,884 / 2,097 us | 8 / 14 | 2,097 us | 10.1 ms |

  No frame was over 2.1 ms (the 2 ms budget plus 100 us). The first version reached 2.8 ms: narrowing
  copied up to 4 MB of survivors inside the command, a slice of 4096 candidates cost up to ~600 us on a
  one-letter query (nearly everything matches), and sorting the kept rows came after the deadline (perf
  commit: the copy is part of the budgeted work, slices of 512, the sort's time reserved).
- Scans (read only): the generated tree, 10,000 files in 100 directories: 16 ms, index 324 KB (33 bytes
  a file). `C:\Windows`: 156,905 files, 55,316 ignored (binary extensions), 46 directories unreadable,
  11.8 s cold / 7.6 s warm, index 15 MB (98 bytes a file: long paths), private bytes +11 MB.
- Typing at line 1,000,000 of the 100 MB file, 10,000 self-inserts (command + frame build), alternated:
  no scan 9-20 us avg, worst 251-282 us; while `C:\Windows` was scanned (44,000-56,000 files listed during
  the typing) 9-19 us avg, worst 221-420 us. Unchanged. The worst first insert of a run (5.4-5.9 ms) is the
  gap moving to line 1,000,000, as in earlier phases.
- `--test`'s thread stress: 300 rounds of start and cancel on the 10,000-file tree, the slowest worker
  ended 263-286 us after its cancel; a worker stuck in the file system is left after exactly the bound.
- Idle after C-x p f on ronin's tree (`--idle-check`): 0 frames, 0 main loop passes, 15.6 ms of CPU (one
  scheduler tick) over 5 s. A run disturbed by the mouse over the window failed and said so.
- Memory (release, private bytes idle, 3 runs): *scratch* 76.1-77.6 MB; src\keymap.c 76.3-77.5 MB; the
  100 MB .txt 192.3-193.3 MB; the 100 MB .c 216.3-217.7 MB (Phase 10: 76.1-77.3, 77.1-77.8, 191.8-193.4,
  216.2-216.9). A project costs its index (above) and, while a scan runs, the worker's batches (8 x 64 KB)
  and directory queue.
- Release exe 378,368 bytes (343,552 after Phase 10); the same six DLLs; kernel32 adds CreateEventW,
  SetEvent, AcquireSRWLockExclusive, ReleaseSRWLockExclusive and GetFileSizeEx.

## Measurements (Phase 12)

Same machine (AMD Ryzen 7 7700X, 16 logical processors, 2560x1440 at 144 Hz), 1280x800 client; `ronin_bench.exe`.

- The second-byte filter (`--bench-search`, 100 MB, the needle pasted at once): folded, needle at the end
  43 -> 17 ms (20 -> 9 frames), missing 41 -> 17 ms; exact 5-7 ms before and after.
- `--bench-grep` on build\tmp\p12bench (101,815,831 bytes in 3,001 files, warm file cache, best of three; the
  list is the fresh index; *search* filling in the other window):

| workers | common word (52,118 matches): first result / all / MB/s | missing word: all / MB/s | longest frame (its results work) |
|---|---|---|---|
| 1 | 1.7 ms / 139 ms / 722 | 124 ms / 811 | 368 us (313 us) |
| 2 | 1.9 ms / 74 ms / 1,358 | 67 ms / 1,506 | 384 us (350 us) |
| 4 | 2.0 ms / 42 ms / 2,401 | 36 ms / 2,820 | 462 us (432 us) |
| 8 | 2.2 ms / 28 ms / 3,619 | 23 ms / 4,301 | 981 us (948 us) |

  No frame over 2,100 us. The default cap (10,000 matches) stops the common word after 9.7-11 ms. Private
  bytes at the peak: +3.6 MB for the missing word, +13 to +22 MB for 52,118 matches (the results buffer's
  text and locations, the held records, the workers' windows and batches); after the searches +3.5 MB.
  search_workers = 0 now takes half the logical processors, at most 8 (8 here).
- The first version wrote every line with its own buffer_replace and did the results work in every frame:
  the frames between keys reached 2.7 ms of results work and a key's frame averaged ~370 us while 8 workers
  searched. Now the lines of a frame go in with one insert, frames with input leave the results work to
  the next frame, and the claim window moves once a frame.
- Typing at line 1,000,000 of the 100 MB file (left window), 3,000 keys, searches for the common word with
  8 workers running back to back (127-128 searches of ~30 ms ended meanwhile), alternated with no search and
  a finished *search* shown in the same two windows: a key's frame (command + build) 36-38 us avg, worst
  309-359 us, against 28-39 us avg, worst 112 us without (and one 5.8 ms first insert moving the gap). The
  frames the main loop runs between keys: 228-235 us avg, worst 1.6-1.9 ms, results work at most 1.6-1.8 ms.
- Priority, alternated (8 workers, typing as above): one step below normal 41 / 47 us avg, worst 330 / 366
  us, a search 29.8 / 30.3 ms; normal 43 / 45 us avg, worst 1,124 / 539 us, a search 29.9 / 29.4 ms. Kept
  below normal: the same search time, a better worst case for typing.
- The project search on ronin's own tree (71 files): 0.01-0.02 s. `--idle-check` after it: 0 frames, 0 us of
  CPU in 5 s.
- Memory (release, private bytes 3 s after the start, 3 runs, `--no-activate`, which never takes the
  foreground): *scratch* 75.6-77.2 MB; src\keymap.c 75.8-76.2 MB; the 100 MB .txt 191.9-193.5 MB; the 100 MB
  .c 216.0-216.8 MB (Phase 11, activated: 76.1-77.6, 76.3-77.5, 192.3-193.3, 216.3-217.7). Measured once the
  old way too, activated, in the same session: 76.2-76.5, 76.1-78.0, 192.2-193.3, 217.0-217.9 MB, so
  --no-activate reads 0-1 MB lower (the focus sets up the IME and text services); the tables before
  Phase 12 compare with the activated figures.
- Startup (`--startup-ms`, release, 15 runs each, the order rotating with the Phase 11 build from 38afcb5):
  174 ms median (172-194) against 175 ms (172-181).
- Release exe 416,768 bytes (378,368 after Phase 11); the same six DLLs; kernel32 adds SetThreadPriority and
  GetActiveProcessorCount.

## Measurements (Phase 13)

Same machine (AMD Ryzen 7 7700X, 16 logical processors, 2560x1440 at 144 Hz), 1280x800 client; `ronin_bench.exe`.

- `--bench-compile`: a `--child` program writes 100 MB of numbered lines (every 64th an MSVC warning on a
  file that exists: 21,808 locations) as fast as it can, best of three. With compilation_max_output_mb =
  128: all of it in *compilation* (98 MB, 1,395,760 lines) 0.33 s after the start (318 MB/s), 1,492
  frames, the longest 278 us (its results work at most 260 us), none over 2,100 us; private bytes +110 MB
  at the peak (the buffer's text and line index, the locations). At the default 16 MB: 16 MB kept
  (228,462 lines), the rest read and dropped, 0.14 s in all (770 MB/s), the longest frame 291 us. After
  the builds, with *compilation* killed: +3 MB.
- Typing at line 1,000,000 of the 100 MB file (left window), 3,000 keys, while 100 MB builds run one after
  another (2-3 ended meanwhile), alternated with a finished *compilation* shown in the same two windows:
  a key's frame 14-18 us avg, worst 217-237 us, against 13-21 us avg, worst 243 us without (and one 5.8 ms
  first insert moving the gap). The frames between keys (the output written): 78-97 us avg, worst 403-422
  us, results work at most 380-400 us.
- ronin built from inside ronin (ronin_bench.exe on the repository, C-x p c RET with .\build.bat, debug): 2.39 s,
  exit [0]; a copy with an error and a warning in src/edit.c: 1.62 s, both listed with their locations.
- `--test` takes ~21 s on ronin_bench.exe (~20 s before): the build tests start a few hundred processes (cmd.exe
  and the `--child` program each time).
- Memory (release, private bytes 3 s after the start, 3 runs, `--no-activate`): *scratch* 76.7-77.0 MB;
  src\keymap.c 76.4-77.3 MB; the 100 MB .txt 191.8-193.0 MB; the 100 MB .c 216.7-217.5 MB (Phase 12:
  75.6-77.2, 75.8-76.2, 191.9-193.5, 216.0-216.8).
- Startup (`--startup-ms`, release, 15 runs each, alternating with the Phase 12 build rebuilt from 6d57cee):
  177 ms median (175-186) against 178 ms (175-182).
- `--idle-check` (debug): 0 frames, 0 us of CPU in 5 s, with no build and right after C-x p c RET on a small
  project (a 0.05 s build that ended in the check's first second).
- Release exe 453,632 bytes (416,768 after Phase 12, rebuilt from 6d57cee to compare); the same six DLLs;
  kernel32 adds 18 functions: CreateProcessW, ResumeThread, GetExitCodeProcess, CreateJobObjectW,
  AssignProcessToJobObject, SetInformationJobObject, TerminateJobObject, CreateNamedPipeW, PeekNamedPipe,
  GetOverlappedResult, CancelIoEx, ResetEvent, WaitForMultipleObjects, InitializeProcThreadAttributeList,
  UpdateProcThreadAttribute, DeleteProcThreadAttributeList, GetSystemDirectoryW and GetTickCount64.

## Measurements (Phase 14)

Same machine (AMD Ryzen 7 7700X, 2560x1440 at 144 Hz), 1280x800 client; `ronin_bench.exe --bench-cursors`, build = the
command plus the frame's build (Present not included). Averages over 100 commands (10 at 10,000 cursors); ranges are
over several runs.

- The old pass first (commit 6d7ea9a, every edit adjusting every marker), on the 100 MB file with the cursors on
  consecutive lines from line 1,000,000: a self-insert at 1,000 cursors 2.1 ms, at 10,000 177 ms (on src/app.c 192
  ms). That is what chose the batch.
- With the batch, the same layout: a self-insert at 10 cursors 31-34 us (one cursor in the same bench: DEL 39-47
  us, RET 36-39 us; the self-insert row's average includes one ~6 ms first insert moving the gap), at 100 cursors
  59-71 us, at 1,000 0.23-0.26 ms, at 10,000 1.7-2.1 ms; DEL the same. Targets: 10 cursors within the one-cursor
  range, 1,000 under 1 ms, 10,000 under 10 ms: met. 0 batch flushes.
- src/app.c (140 KB) with the cursors spread over it (every 14 bytes at 10,000): a self-insert at 1,000 cursors
  0.19-0.29 ms, at 10,000 1.95-2.25 ms; RET 4.8 ms at 1,000, 29-45 ms at 10,000.
- Cursors spread evenly over the whole 100 MB file: 13-22 ms per key from 10 cursors on (one cursor 26-109 us):
  the gap travels ~100 MB every command (back to the first cursor, then forward), whatever the markers cost. Not
  reordered (the approved plan); see "Later".
- RET on the 100 MB file's consecutive lines: 0.16 ms at 10 cursors, 4.4-5.5 ms at 100, 0.38-0.5 s at 1,000, 5.5-7 s
  at 10,000, with 0 flushes: each new line's indentation looks back for a matching brace through the bench text's
  random, unbalanced braces (up to SYNTAX_MATCH_MAX, 256 KB, with a 256 KB stack zeroed per walk), ~0.4-0.6 ms a
  cursor, the same as for one cursor on those lines. On real code (src/app.c, the C file below) RET is 0.7-7.0 ms.
- The classes that reindent (a C file of 12,000 8-line blocks, a fresh copy and fresh cursors per command; batch
  flushes per command):

  | class | 1,000 cursors | 10,000 cursors | flushes |
  |---|---|---|---|
  | self-insert | 0.26-0.29 ms | 1.8-2.0 ms | 0 |
  | DEL | 0.17 ms | 1.8-2.2 ms | 0 |
  | RET | 0.73-0.86 ms | 7.0-7.8 ms | 0 |
  | TAB | 0.68-0.71 ms | 7.0-7.9 ms | 0 |
  | comment-line | 0.31-0.33 ms | 3.4-3.9 ms | 0 |
  | RET at the end of a whitespace-only line | 8.9 ms -> 0.77-0.82 ms | 913 ms -> 6.9-7.4 ms | one per cursor -> 0 |
  | '}' first on a line (electric reindent) | 12.5 ms -> 2.6-3.0 ms | 908 ms -> 26-32 ms | one per cursor -> 0 |
  | ':' ending a case label (electric reindent) | 11-12.8 ms | 0.92-1.07 s | one per cursor |

  The two classes that flushed once per cursor (an edit before point after inserting at point) were fixed by the
  join (commit f8fff0f, the arrows); the case label's colon keeps text between its two edits and still flushes
  ("Later").
- The undo of a self-insert at 10,000 cursors (one group of 10,000 records and its cursors): 1.07-1.53 ms, with the
  10,000 cursors back; its undo-redo 1.17-1.73 ms. The undo of 20 merged self-inserts at 10,000 cursors (200,000
  records, 20 descending runs): 15-19.5 ms. The undo log grows 576 KB for the first command (10,000 records of 32
  bytes and the 240 KB of cursors); 20 more commands bring it from 42 MB to 57.7 MB committed (the 100 MB file's
  earlier benches made the first 41 MB).
- mark-all-like-this on a word with 1,000,000 occurrences (1,000,000 lines of 64 bytes): at max_cursors 10,000 one
  frame of 0.57-0.67 ms; at 100,000 one frame of 4.9-5.5 ms (the cursors made at the end, in one pass). A word only
  at the end of the 100 MB file (the whole file searched): 5 frames, the longest 2.3-2.4 ms, 8.4-9.2 ms of work in
  all. A self-insert at 10,000 cursors there 7.3-8.0 ms (the first edit after loading moves the gap over the file),
  its undo 1.2-1.3 ms; at 100,000 cursors 21-22 ms, its undo 12.6-13.9 ms.
- Memory: 10,000 cursors +760-916 KB of private bytes, 100,000 +7.1-8.7 MB (the cursor array, 40 bytes a cursor,
  and two markers of 16 bytes plus 8 bytes of the live list each).
- A single cursor after 100,000 cursors were made and collapsed: DEL 106 us and RET 265 us before the live marker
  list (every pass walked the 200,000 slots ever used), 39 us after it (commit 467f493).
- One cursor, against Phase 13 (rebuilt from 1fd5708, alternated): `--bench-view` self-insert at line 1,000,000
  8-22 us avg against 8-20 us (the quiet rounds 9 us against 8 us); `--bench-edit` (undo on) 16-24 us against
  9-23 us. Unchanged within the noise.
- `--test` takes ~22.5 s on ronin_bench.exe; the Phase 13 build took 23.8-24.7 s on the same day (Phase 13 recorded ~21 s):
  the machine's noise is larger than what the new groups add.
- Memory (release, private bytes 3 s after the start, 3 runs, `--no-activate`): *scratch* 75.9-77.5 MB; src\keymap.c
  76.5-77.7 MB; the 100 MB .txt 192.0-193.5 MB; the 100 MB .c 216.0-217.5 MB (Phase 13: 76.7-77.0, 76.4-77.3,
  191.8-193.0, 216.7-217.5).
- Startup (`--startup-ms --no-activate`, release, 15 runs each): 176 ms median (174-183) against Phase 13's 176 ms
  (173-184), alternating; after the last two commits 180 ms (177-205), alone.
- `--idle-check` (debug): 0 frames, 0 us of CPU in 5 s, with one cursor and with 10,000 (`build\tmp\bench_cursors.c
  --keys "C-x h C-S-c C-S-c" --idle-check`, mc:10000 checked in a screenshot of the same keys); the bench also finds
  no frame wanted with 10,000 and 100,000 cursors.
- Release exe 482,304 bytes (453,632 after Phase 13); the same six DLLs and the same 195 imported functions.

## Measurements (Phase 15)

Same machine (AMD Ryzen 7 7700X, NVIDIA GPU, 2560x1440 at 144 Hz), 1280x800 client. The "before" builds are Phase 14
(e12cc29) built from a clean copy into build\base\, run alternately with the new ones.

### Baseline (before the Phase 15 changes)

**Size.** Release exe 482,304 bytes, the same as Phase 14 (the rename changed no size). By module from the linker map
(`ronin_debug.exe --map-report build\ronin.map`; 477,324 bytes in sections, the rest is headers and alignment; a
function inlined under /LTCG counts toward its caller):

| module | bytes | | module | bytes |
|---|---|---|---|---|
| static CRT (code 48,284, tables 31,428, data 2,768) | 82,612 | | compile.c | 15,808 |
| win32_main.c | 43,312 | | files.c | 13,456 |
| unwind tables (.pdata / .xdata, ours and the CRT's) | 38,224 | | minibuffer.c | 12,104 |
| app.c | 34,952 | | project.c, config.c | 10,368, 10,304 |
| results.c | 24,496 | | finder.c, isearch.c | 9,432, 9,392 |
| view.c | 19,632 | | grep.c, cursors.c | 7,728, 7,184 |
| config_default.h (the built-in config text) | 19,128 | | fuzzy.c, font.c, search.c | 4,736, 4,592, 4,400 |
| buffer.c | 18,976 | | imports (thunks and tables) | 4,148 |
| edit.c | 18,136 | | render_d3d11.c, base.c, lex_js.c | 3,904, 3,856, 3,488 |
| string literals | 17,296 | | keymap.c, window.c, the shaders | 2,960, 2,800, 2,136 |
| syntax.c | 17,072 | | win32_dwrite.cpp, resources (manifest), command.c | 1,680, 1,448, 1,184 |

The largest data: the built-in config text (19,128), the CRT's locale tables (2 x 3,648) and log tables (3 x ~2,070),
the vertex shader (1,264), the command table (1,024). No dev code: the map holds no test, bench, png, child or
input-test symbol, and the exe no such string.

**Startup.** Release `--startup-ms --no-activate`, 15 runs alternating: 167 ms median (163-178), Phase 14 169 ms (164-185).
The stages (bench build, ms since process creation): WinMain 13.9; arguments and the device thread 14.5; the hidden
window 18.9; the app 21.4 (DirectWrite and the font 2.0 ms of it); waiting for the device thread until 167.9 (146 ms);
swap chain, pipeline and atlas texture 172.0; the first frame 173.9-176.1 (update 0.26, build 0.06, flush 0.70, Present
1.09); uncloaked 176.2.

**Memory.** Release, private bytes 3 s after the start with `--no-activate`: 76.4-76.9 MB (Phase 14 76.1-77.1).
By owner (`ronin_bench.exe --memory-report`, 78 MB in the dev build):

| owner | MB |
|---|---|
| the NVIDIA user-mode driver's DLL data (nvwgf2umx 25.0, nvppex 3.7, nvgpucomp64 3.3: copy-on-write image pages) | 33.1 |
| other private allocations: the driver and the D3D runtime (21 regions, the largest 17.5 MB) | 27.9 |
| our arenas: the font's (atlas, glyph table) 4.1, the permanent one 2.8, the frame scratch 1.3 | 8.3 |
| process heaps (Windows, DirectWrite, the D3D runtime, the CRT) | 6.0 |
| thread stacks (59, nearly all the driver's) | 1.3 |
| mapped views (the GDI DIB of the rasterizer, shared sections) | 0.9 |

Ours is ~9 MB (arenas and DirectWrite's share of the heaps); the graphics stack ~67 MB.

**Typing.** `--bench-view`, self-insert at line 1,000,000 of the 100 MB file, command + frame build: 9-10 us avg (worst
5-7 ms: the first insert moving the gap). New, `--bench-latency` (500 keys posted to the shown window, Present(1, 0)):

| from the key's arrival to | p50 | p90 | p99 | max |
|---|---|---|---|---|
| its dequeue | 0.09 ms | 0.17 ms | 0.36-0.87 ms | 1.3 ms |
| command + frame build | 0.02-0.03 ms | 0.03 ms | 0.04-0.06 ms | 0.1-0.2 ms |
| Present returned | 0.41-0.49 ms | 0.7-0.96 ms | 2.0-4.5 ms | 3.4-6.2 ms |
| the vertical blank that showed it, composed by DWM (the first ~100 keys) | 16.8-17.4 ms | 20.2-20.4 ms | 21.4-40 ms | 40 ms |
| the vertical blank that showed it, overlay (DWM promotes the window after a while) | 4.0-4.4 ms | 6.9-7.2 ms | 9.9-11.6 ms | 14-15 ms |

One refresh is 6.9 ms: composed frames reach the screen ~2.5 refreshes after the key, overlay ones within one.

### Changes and results

Each "before" is the Phase 14 build (or the commit before the change), run alternately with the new one.

**Correctness.** Seeds 22 and 33 failed `test_undo_commands`; a ddmin reducer took both to three steps
(backward-word, newline twice, then backward-word to the buffer's start and a delete-backward-char that is
refused). The test's bug: it took a refused delete as an edit when point had moved (760277f); undo was right.
The seed sweep (`--fuzz-seeds 1 256`, 8 workers) takes ~160 s and passes; with the fix reverted in a scratch
copy, the fixed case fails, and without that case the sweep stops at seed 22. The soak (1,000,000 events,
~9-11 s on the bench build) found two real bugs, the wheel and other windows' commands leaving several cursors
out of order (6e4ca11, 18d6e94); it now passes on both builds: peak ~13 MB of our memory, the settled memory
without the undo logs 7.0 -> 7.9 MB, the typing probe flat, no leak. Its five slowest events are saves (5-7
ms in ReplaceFileW, once 146 ms).

**Size** (release; the map report attributes the rest):

| step | bytes | |
|---|---|---|
| Phase 14 | 482,304 | six DLLs, 195 functions |
| CRT-free (own entry point; chkstk, memcpy / memset / memmove and cpu_disp only) | 395,264 | KERNEL32 120 -> 69 functions |
| config.c, files.c, finder.c, command.c, isearch.c, cursors.c at `optimize("s")` | 375,808 | their benches unchanged |
| d3d11, dwmapi and dwrite loaded on first use | | KERNEL32, USER32, GDI32 only |
| crash recovery, early_window, the long-line and cursor fixes, --version | 385,536 | |
| the icon (9,230 bytes) and the version resource | 397,312 | .rsrc 12,288 raw; 3 DLLs, 133 functions |

Left at /O2 because their benches got slower at "s": compile.c (100 MB of output 8-12% slower), results.c
(2-5%), project.c (a scan 14 -> 16 ms). /O1 everywhere would give 286,208 bytes at the hot paths' cost.
Dead code: two dev helpers nothing called (compile_memory, grep_memory); nothing else.

**Startup** (release, `--startup-ms --no-activate`, 15 runs alternating): 167 ms median before the lazy DLL
loads, 164-165 after (WinMain ~1 ms earlier); today 164 ms (162-181) against Phase 14's 165 (163-174), and 164
with or without the icon. `D3D11CreateDevice` variants (the device creation's own time, medians, alternating): a first round
of all five gave the default 154.8 ms, feature level 11_0 only 151.3, without BGRA_SUPPORT 151.8,
PREVENT_INTERNAL_THREADING_OPTIMIZATIONS 152.9, an explicit adapter from EnumAdapters1 151.4; a second round of
15 runs each of the first three gave 152.1, 152.1 and 151.9 ms: within the noise, none kept. `early_window` (bench,
`--startup-capture`, 15 runs each, alternating): visible after 39.9 ms (36.4-61.8) with it on against 180.7 ms
off; the first frame 185.2 against 180.7 ms; 14 of 15 runs with no other image between (one changed a corner
pixel during a slow start). Default off until the HUMAN step decides.

**Input latency** (`--bench-latency`, 3 runs each, alternating): Present(1, 0) (the baseline), Present(0, 0)
for the key's frame and a waitable swap chain (max latency 1, waited on only while a frame is pending) gave
the same: composed p50 16.2-17.8 ms, overlay p50 4.4-4.5 ms, arrival to Present returned 0.42-0.46 ms. None
kept. Final run: arrival to Present p50 0.41 ms (p99 0.89), on screen composed p50 17.1 ms (p99 21.3), overlay
p50 4.9 ms; command + frame build p50 23 us.

**Hot spots.**
- ':' completing a case label at many cursors (C file): 1,000 cursors 12.5 -> 3.1 ms, 10,000 925 -> 28 ms
  (Command.after: the label reindent runs in a second ordered pass, one flush per command).
- A bracket on a line longer than SYNTAX_MATCH_MAX with show_paren_mode (5 MB minified line, moving): 6.5 ms
  (worst 16-19) -> 0.48 ms a key.
- `--bench-longline` today (4 runs): typing at the end of the 5 MB line 0.75-1.14 ms avg (worst 1.3-1.65),
  in its middle 0.38-0.64, moving right / left 0.47-0.74 ("Later": a column cache).
- RET's bracket walk: pushing its 256 KB stack without zeroing measured no gain (reverted); the cost is
  lexing up to 256 KB back, 17-57 us for one cursor on real code, ~0.4-0.6 ms a cursor on the brace-heavy
  bench text ("Later").

**Memory** (release, idle, private bytes, `--no-activate`, 3-4 runs alternating):

| step | MB | Phase 14 |
|---|---|---|
| CRT-free | 75.3-75.7 | 76.3-77.4 |
| the glyph atlas grows its rows on demand (128 rows first) | 65.0-66.6 | 77.0-77.6 |
| 16,384 quad instances per flush instead of 65,536 | 57.3-58.1 | |
| today (*scratch*) | 57.4-59.3 | 75.9-77.4 |
| today, the 100 MB .txt open | 174.4-175.0 | 192.2-193.4 |

By owner now (`--memory-report`, dev build, 60 MB): our arenas 3.0 MB (8.3 before), process heaps 6.4, thread
stacks 1.4, the driver's other private allocations 15.1 (27.9), its DLLs' writable image pages 33.9, mapped
views 1.1. Minimized: IDXGIDevice3::Trim after clearing the context, 60.5 -> 58.1 MB while minimized, the first
frame after restoring 1.5-1.8 ms either way. Tried and left out: a 1 x 1 swap chain while minimized (no further
gain, the restore 3.1 ms) and trimming on losing the focus (the next frame takes it all back).

**Crash safety net.** The filter writes through `buffer_write_to` with a 16 KB stack chunk: the UTF-16 path
reads across the gap without moving it, so the UTF-8 fallback the plan allowed was not needed. The test
compares each recovery file with a save of the same edited buffer: CRLF UTF-8, UTF-16 LE with CRLF and an
astral character, UTF-16 BE, a UTF-8 BOM, mixed endings; broken in a scratch copy (the contents not written),
`--test` fails on it.

**Against Phase 1 and Phase 14** (Phase 1 = 05a45e5, a window and a rectangle renderer, rebuilt for its size;
its startup from the Phase 1 notes):

| | Phase 1 | Phase 14 | Phase 15 |
|---|---|---|---|
| release exe | 112,128 bytes, static CRT | 482,304 bytes, static CRT | 397,312 bytes, no CRT, icon and version included |
| imports | kernel32, user32, d3d11, dwmapi (97 functions) | six DLLs (195) | kernel32, user32, gdi32 (133); three loaded on first use |
| startup to the first Present | ~170 ms (~160 ms D3D11CreateDevice) | 165-176 ms | 164 ms (~146 ms the device); window at ~40 ms with early_window |
| idle CPU | 0 (blocked in MsgWaitForMultipleObjectsEx) | 0 frames, 0 us in 5 s | 0 frames, 0 us in 5 s |
| typing, command + frame | (no editor) | 8-24 us | 8-24 us; arrival to screen p50 17 ms composed, 4.4-4.9 overlay |
| idle memory | not measured | 76-77 MB | 57-59 MB |
| allocation per keystroke or frame | none (rule) | none | none |

## Measurements (Phase 16)

Same machine (AMD Ryzen 7 7700X, NVIDIA GPU, 2560x1440 at 144 Hz), the debug build (D3D11 debug layer on) for
`--leak-check`, the bench and the debug build for the soak. The tables below are the `--leak-check all` of the last
`verify.bat phase` (build\verify\leak-check.log), copied by a script from the log.

### What was found and fixed

| leak | module | cause | fix |
|---|---|---|---|
| a finished scan's, search's or build's results not taken while idle, its workers not joined | platform (win32_main.c) | the workers' wake is an auto-reset event in the main loop's wait: when it ended the wait, the wait reset it and the look after it found nothing, so app_wake did not run until a wake came while a frame was due. Found by finder-, grep- and compile-cycle never settling; confirmed in the real main loop (the wait ended by the wake three times, app_wake never called) | e335924 |
| ~215 bytes of process heap per worker thread that ended | platform (win32_main.c) | with a hardware D3D11 device the NVIDIA driver keeps that much for every thread that ends (0 with nothing loaded, with only d3d11.dll and dxgi.dll, or with WARP: 2,000 threads each); ronin started a thread for every scan, search worker and build (finder-cycle/rescan, grep-cycle/cancel: ~700 bytes a cycle over 800 cycles) | e11ba96 (threads reused) |
| the remembered build commands' arena grew on every accepted compile prompt | results.c | results_compile_remember copied the command, the key and recompile's directory and root into an append-only arena (compile-cycle: ~160 bytes a cycle; 64 MB reserved, then os_fatal) | 11e4ab5 |
| files_arena grew on every project search and occur | results.c | their header's text was built there and only the file commands reset it (grep-cycle/done: ~330 bytes a cycle) | 6840e88 |
| a buffer's meta arena grew on every write-file under a new name and every name made unique | buffer.c, app.c | the old path and the names tried stayed after the struct (1 MB reserved, then os_fatal) | 0507553 |
| the font's atlas and glyph table never released | font.c | font_shutdown closed DirectWrite only (once a process; the exit check requires every arena released) | 338173e |

Each fix has a regression test shown to fail on a scratch copy with the fix reverted: "wake (the wait ended by the
wake, app_wake called 0 time(s))"; "thread reuse (2,000 tasks with the GPU driver loaded: 2000 ran, the process
heaps grew 417792 bytes)"; "compile remember: the arenas grew from 8192 to 524288 bytes between prompt 100 and
10,000"; "results files_arena: occur grew it from 16384 to 131072 bytes between run 100 and 1,000"; "write-file
meta: the buffer's meta arena grew from 16384 to 131072 bytes between rename 100 and 1000" (all `--test`, exit 9);
the smoke's "2 of the font's arenas (atlas, glyph table) still reserved after close" (exit 6).

### What moved and is not ronin's (measured, then decided)

- The process handle count read right after a frame is +2 in about one sample in ten (the graphics stack holds a
  handle or two for microseconds; never in the handle snapshot taken next): the sample reads it until two reads
  1 ms apart agree.
- Private commit: one 68 KB step of the process heap in one window, then flat (about 1 run in 5 of the 5-cycle
  100 MB scenario; the heap commits a chunk, most likely for threads Windows starts in the process, which the
  driver charges): a STEP, not a LEAK (decision above).
- The process handle total and USER objects follow Windows' thread pool, RPC runtime and graphics stack: over 2,000
  config reloads 490, 496, 498, 494, 494 handles (Event, IoCompletion, Timer, Thread, Semaphore, ALPC Port) and USER
  objects 9 -> 11, the ledger and our counters exactly flat: informational.
- Builds with the device present: process creation moves the process heap irregularly (300 builds: +16 KB, +86 KB,
  0; headless +37 KB, 0, +4 KB): within the private commit rule, in compile-cycle 37-53 KB a window over 800 cycles.
- The soak's memory: the random walk raises the high-water marks of the minibuffer (+1.8 MB once), the starting
  buffers, *Messages*, the kill ring and the dev clipboard fake, and leaves a different set of buffers (7 to 9
  arenas each) at each checkpoint: informational in the soak (decision above).
- `--cycles 40` (windows of 10) can flag a high-water mark reached after the warm-up (*search*'s results arena,
  +48 KB once); at the default 200 it is OK.

### The leak check (`--leak-check all`)

The run of the last `verify.bat phase`: 22 scenarios, 0 LEAK, 0 failures, the four screenshot pairs equal pixel for pixel
(window-cycle and config-reload-cycle; also by an independent decoder, System.Drawing: 0 differing pixels of
1280x800 each). 119 s of scenarios, about 2 minutes for the whole run (`time`: 1 min 59 s for the final
code alone; 2 min 2-3 s for the detector-proof copies); the phase gate took 6 min 19 s with it. WATCH and STEP:
config-reload-cycle/color's private commit (one step) and working set. undo-cycle's log reached its 1 MB cap in cycle 4 (888,800 bytes of records), inside the warm-up.

Per scenario (our committed bytes jump between scenarios, never within one: the frames read back for the
screenshots go into the permanent arena, 4 MB each, which keeps its high-water mark, and the 10,000-file project's
index is made by the first project scenario):

| scenario | cycles | time | private commit, MB (warm-up -> window 4) | our committed, MB | ledger | D3D11 objects | worker slots | verdict |
|---|---|---|---|---|---|---|---|---|
| file-cycle | 220 | 1.8 s | 62.6 -> 62.6 | 4.8 -> 4.8 | 2 -> 2 | 13 -> 13 | 0 -> 0 | OK |
| file-cycle-100mb | 5 | 0.5 s | 62.6 -> 62.6 | 4.8 -> 4.8 | 2 -> 2 | 13 -> 13 | 0 -> 0 | OK |
| window-cycle/split | 220 | 0.6 s | 70.7 -> 70.7 | 8.8 -> 8.8 | 2 -> 2 | 14 -> 14 | 0 -> 0 | OK |
| window-cycle/one | 220 | 1.4 s | 79.1 -> 79.1 | 17.2 -> 17.2 | 2 -> 2 | 14 -> 14 | 0 -> 0 | OK |
| minibuffer-cycle/mx | 220 | 0.6 s | 79.1 -> 79.1 | 17.2 -> 17.2 | 2 -> 2 | 14 -> 14 | 0 -> 0 | OK |
| minibuffer-cycle/find-file | 220 | 0.3 s | 79.1 -> 79.1 | 17.2 -> 17.2 | 2 -> 2 | 14 -> 14 | 0 -> 0 | OK |
| minibuffer-cycle/goto-line | 220 | 0.4 s | 79.1 -> 79.1 | 17.2 -> 17.2 | 2 -> 2 | 14 -> 14 | 0 -> 0 | OK |
| minibuffer-cycle/switch | 220 | 0.6 s | 79.1 -> 79.1 | 17.2 -> 17.2 | 2 -> 2 | 14 -> 14 | 0 -> 0 | OK |
| isearch-cycle/lazy | 220 | 0.5 s | 78.8 -> 78.8 | 17.3 -> 17.3 | 2 -> 2 | 14 -> 14 | 0 -> 0 | OK |
| isearch-cycle/query-replace | 220 | 1.5 s | 78.7 -> 78.7 | 17.2 -> 17.2 | 2 -> 2 | 14 -> 14 | 0 -> 0 | OK |
| multi-cursor-cycle | 220 | 1.5 s | 78.7 -> 78.7 | 17.3 -> 17.3 | 2 -> 2 | 14 -> 14 | 0 -> 0 | OK |
| undo-cycle | 220 | 1.7 s | 80.0 -> 80.0 | 18.6 -> 18.6 | 2 -> 2 | 14 -> 14 | 0 -> 0 | OK |
| finder-cycle/open | 220 | 1.6 s | 79.3 -> 79.3 | 17.7 -> 17.7 | 3 -> 3 | 14 -> 14 | 1 -> 1 | OK |
| finder-cycle/cancel | 220 | 1.4 s | 79.3 -> 79.3 | 17.7 -> 17.7 | 3 -> 3 | 14 -> 14 | 1 -> 1 | OK |
| finder-cycle/rescan | 220 | 9.7 s | 79.4 -> 79.4 | 17.7 -> 17.7 | 3 -> 3 | 14 -> 14 | 2 -> 2 | OK |
| grep-cycle/done | 220 | 25.7 s | 80.1 -> 80.1 | 18.1 -> 18.1 | 3 -> 3 | 14 -> 14 | 4 -> 4 | OK |
| grep-cycle/cancel | 220 | 1.0 s | 79.6 -> 79.6 | 17.7 -> 17.7 | 3 -> 3 | 14 -> 14 | 4 -> 4 | OK |
| compile-cycle/echo | 220 | 5.4 s | 79.7 -> 79.8 | 17.7 -> 17.7 | 3 -> 3 | 14 -> 14 | 4 -> 4 | OK |
| compile-cycle/error | 220 | 7.6 s | 79.8 -> 79.7 | 17.7 -> 17.7 | 3 -> 3 | 14 -> 14 | 4 -> 4 | OK |
| config-reload-cycle/color | 220 | 27.7 s | 79.8 -> 79.9 | 17.8 -> 17.8 | 3 -> 3 | 14 -> 14 | 4 -> 4 | OK |
| config-reload-cycle/scale | 220 | 0.6 s | 81.0 -> 81.0 | 17.8 -> 17.8 | 3 -> 3 | 14 -> 14 | 4 -> 4 | OK |
| external-change-cycle | 220 | 27.9 s | 81.1 -> 81.1 | 17.8 -> 17.8 | 3 -> 3 | 14 -> 14 | 4 -> 4 | OK |

### The soak

`--soak 1000000` in the same phase gate: bench 9.2 s, debug 12.5 s, both PASS. The checkpoints from 500,000 to
1,000,000 events (bench; the debug build's verdict rows are the same):

| metric | 500k | 600k | 700k | 800k | 900k | 1M | verdict |
|---|---|---|---|---|---|---|---|
| handle ledger (ours) | 3 | 3 | 3 | 3 | 3 | 3 | OK |
| GDI objects | 0 | 0 | 0 | 0 | 0 | 0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0 | OK |
| directory watches | 3 | 3 | 3 | 3 | 3 | 3 | OK |
| worker thread slots | 6 | 8 | 8 | 8 | 8 | 8 | OK (under its cap) |
| private commit without the undo logs (bytes) | 8945792 | 9322624 | 9433184 | 9375968 | 9547968 | 9990304 |  |
| our committed bytes without the undo logs | 4141184 | 4173952 | 4284512 | 4247776 | 4366528 | 4825248 |  |
| our reservations | 103 | 104 | 104 | 103 | 101 | 101 |  |
| process handles | 123 | 129 | 129 | 129 | 129 | 129 |  |
| USER objects | 1 | 1 | 1 | 1 | 1 | 1 |  |
| working set (bytes) | 14913536 | 15757312 | 16801792 | 16273408 | 15532032 | 16494592 |  |
| process threads (Toolhelp) | 10 | 12 | 12 | 12 | 12 | 12 |  |

The memory rows grow with the random walk's high-water marks: from 500,000 events to the end the largest growth was in
the starting buffers (+0.85 MB, +0.72 MB: buffer_create_reserve <- app_find_file <- app_create), buffers visited
during the walk and the minibuffer (+0.26 MB: minibuffer_init); 25 reservations released and 23 made (buffers
replaced). The existing settled-growth check (2 MB) passes.

### The exit check

The smoke and the leak check's exit: ReportLiveDeviceObjects lists 1 live object with everything else released, the
device itself (refcount 4: the debug interfaces' references), none other; 0 arenas besides the permanent one and the
frame scratch; the handle ledger 0. The process's handle total: 147 after the window was created, 484 after the first
frame, 256 at the smoke's exit (150, 487 and 257 in the leak check); by type, at exit against the window's creation:
EtwRegistration 47 -> 119, Semaphore 4 -> 16, Event 23 -> 29, File 17 -> 22, Key 10 -> 15, Mutant 4 -> 9, Section
12 -> 15, ALPC Port 4 -> 6, WaitCompletionPacket 6 -> 7, IoCompletion 4 -> 3, Thread 3 -> 2: the loaded DLLs' (the
driver, the D3D runtime, DirectWrite) for the life of the process.

### The detector proof

A scratch copy (build\proof\detector2, never committed) of the final code with two deliberate leaks, 64 KB committed
with VirtualAlloc and never freed in split-window-right only, and the pipe handle never closed in os_process_close.
`--leak-check all` exits 6:

```
leak: file-cycle: OK, 1768 ms
leak: file-cycle-100mb: OK, 473 ms
leak: | private commit (bytes) | 76677120 | 79958016 | 83243008 | 86528000 | 89808896 | 65672.5 | LEAK |
leak: window-cycle/split: LEAK, 628 ms
leak: | private commit (bytes) | 102629376 | 112435200 | 122277888 | 132124672 | 141991936 | 197044.9 | LEAK |
leak: window-cycle/one: LEAK, 1314 ms
leak: minibuffer-cycle/mx: OK, 505 ms
leak: minibuffer-cycle/find-file: OK, 298 ms
leak: minibuffer-cycle/goto-line: OK, 360 ms
leak: minibuffer-cycle/switch: OK, 594 ms
leak: isearch-cycle/lazy: OK, 497 ms
leak: isearch-cycle/query-replace: OK, 1475 ms
leak: multi-cursor-cycle: OK, 1471 ms
leak: undo-cycle: OK, 1601 ms
leak: finder-cycle/open: OK, 1575 ms
leak: finder-cycle/cancel: OK, 1276 ms
leak: finder-cycle/rescan: OK, 9342 ms
leak: grep-cycle/done: OK, 24826 ms
leak: grep-cycle/cancel: OK, 988 ms
leak: | handle ledger (ours) | 23 | 73 | 123 | 173 | 223 | 1.0 | LEAK |
leak: compile-cycle/echo: LEAK, 5441 ms
leak: | handle ledger (ours) | 243 | 293 | 343 | 393 | 443 | 1.0 | LEAK |
leak: compile-cycle/error: LEAK, 7633 ms
leak: config-reload-cycle/color: OK, 27729 ms
leak: config-reload-cycle/scale: OK, 580 ms
leak: external-change-cycle: OK, 27733 ms
leak: FAIL: 22 scenario(s), 4 LEAK, 0 failure(s), 0 screenshot mismatch(es), 118 s; WATCH and STEP: window-cycle/split: working set (bytes) (WATCH); window-cycle/one: working set (bytes) (WATCH); compile-cycle/echo: process handles (WATCH); compile-cycle/error: process handles (WATCH); config-reload-cycle/color: private commit (bytes) (STEP)
exit-check: FAIL: 440 handle(s) the os_* layer opened are still open
exit-check: FAIL: 0 arena(s) besides the permanent one and the frame scratch, the handle ledger at 440
leak-check: FAIL (exit 6, 147424 frames)
```

LEAK exactly for the four scenarios that run C-x 3 or a build (private commit +65,672 bytes a cycle with one C-x 3,
+197,045 with three; the ledger +1.0 a cycle); every other scenario OK, and the exit check fails too (440 handles still
open).

### Size and imports

Release exe 398,336 bytes (397,312 after Phase 15: +1,024, the fixes); the same three DLLs (KERNEL32, USER32, GDI32)
and the same 145 imported functions (dumpbin /imports against the Phase 15 build, d81c785). Everything else of the
phase is in the dev builds only.

### Every scenario
**file-cycle** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 65609728 | 65609728 | 65609728 | 65609728 | 65609728 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our reservations | 59 | 59 | 59 | 59 | 59 | 0.0 | OK |
| our committed bytes | 5009408 | 5009408 | 5009408 | 5009408 | 5009408 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 13 | 13 | 13 | 13 | 13 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 0 | 0 | 0 | 0 | 0 | 0.0 | OK (cap 256) |
| process handles | 494 | 494 | 494 | 494 | 494 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 51515392 | 51539968 | 51539968 | 51539968 | 51539968 | 0.0 |  |
| process threads (Toolhelp) | 60 | 60 | 60 | 60 | 60 | 0.0 |  |
| video memory, local (bytes) | 16916480 | 16916480 | 16916480 | 16916480 | 16916480 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

file-cycle: OK in 1782 ms.

**file-cycle-100mb** (5 cycles: 1 warm-up, 4 windows of 1)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 65654784 | 65654784 | 65654784 | 65654784 | 65654784 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our reservations | 59 | 59 | 59 | 59 | 59 | 0.0 | OK |
| our committed bytes | 5054464 | 5054464 | 5054464 | 5054464 | 5054464 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 13 | 13 | 13 | 13 | 13 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 0 | 0 | 0 | 0 | 0 | 0.0 | OK (cap 256) |
| process handles | 494 | 494 | 494 | 494 | 494 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 51572736 | 51572736 | 51572736 | 51572736 | 51572736 | 0.0 |  |
| process threads (Toolhelp) | 60 | 60 | 60 | 60 | 60 | 0.0 |  |
| video memory, local (bytes) | 16916480 | 16916480 | 16916480 | 16916480 | 16916480 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

file-cycle-100mb: OK in 469 ms.

**window-cycle/split** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 74129408 | 74129408 | 74129408 | 74129408 | 74129408 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our reservations | 59 | 59 | 59 | 59 | 59 | 0.0 | OK |
| our committed bytes | 9179136 | 9179136 | 9179136 | 9179136 | 9179136 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 0 | 0 | 0 | 0 | 0 | 0.0 | OK (cap 256) |
| process handles | 494 | 494 | 494 | 494 | 494 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 59981824 | 59981824 | 59981824 | 59981824 | 59981824 | 0.0 |  |
| process threads (Toolhelp) | 60 | 60 | 60 | 60 | 60 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

window-cycle/split: OK in 626 ms.

**window-cycle/one** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 82993152 | 82948096 | 82948096 | 82948096 | 82948096 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our reservations | 59 | 59 | 59 | 59 | 59 | 0.0 | OK |
| our committed bytes | 18026496 | 18026496 | 18026496 | 18026496 | 18026496 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 0 | 0 | 0 | 0 | 0 | 0.0 | OK (cap 256) |
| process handles | 494 | 494 | 494 | 494 | 494 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 68837376 | 68837376 | 68837376 | 68837376 | 68837376 | 0.0 |  |
| process threads (Toolhelp) | 60 | 60 | 60 | 60 | 60 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

window-cycle/one: OK in 1350 ms.

**minibuffer-cycle/mx** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 82960384 | 82960384 | 82960384 | 82960384 | 82960384 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our reservations | 59 | 59 | 59 | 59 | 59 | 0.0 | OK |
| our committed bytes | 18038784 | 18038784 | 18038784 | 18038784 | 18038784 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 0 | 0 | 0 | 0 | 0 | 0.0 | OK (cap 256) |
| process handles | 494 | 494 | 494 | 494 | 494 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 68849664 | 68849664 | 68849664 | 68849664 | 68849664 | 0.0 |  |
| process threads (Toolhelp) | 60 | 60 | 60 | 60 | 60 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

minibuffer-cycle/mx: OK in 564 ms.

**minibuffer-cycle/find-file** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 82960384 | 82960384 | 82960384 | 82960384 | 82960384 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our reservations | 59 | 59 | 59 | 59 | 59 | 0.0 | OK |
| our committed bytes | 18038784 | 18038784 | 18038784 | 18038784 | 18038784 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 0 | 0 | 0 | 0 | 0 | 0.0 | OK (cap 256) |
| process handles | 494 | 494 | 494 | 494 | 494 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 68898816 | 68898816 | 68898816 | 68898816 | 68898816 | 0.0 |  |
| process threads (Toolhelp) | 60 | 60 | 60 | 60 | 60 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

minibuffer-cycle/find-file: OK in 298 ms.

**minibuffer-cycle/goto-line** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 82960384 | 82960384 | 82960384 | 82960384 | 82960384 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our reservations | 59 | 59 | 59 | 59 | 59 | 0.0 | OK |
| our committed bytes | 18038784 | 18038784 | 18038784 | 18038784 | 18038784 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 0 | 0 | 0 | 0 | 0 | 0.0 | OK (cap 256) |
| process handles | 494 | 494 | 494 | 494 | 494 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 68898816 | 68898816 | 68898816 | 68898816 | 68898816 | 0.0 |  |
| process threads (Toolhelp) | 60 | 60 | 60 | 60 | 60 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

minibuffer-cycle/goto-line: OK in 376 ms.

**minibuffer-cycle/switch** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 82964480 | 82964480 | 82964480 | 82964480 | 82964480 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our reservations | 59 | 59 | 59 | 59 | 59 | 0.0 | OK |
| our committed bytes | 18042880 | 18042880 | 18042880 | 18042880 | 18042880 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 0 | 0 | 0 | 0 | 0 | 0.0 | OK (cap 256) |
| process handles | 494 | 494 | 494 | 494 | 494 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 68902912 | 68902912 | 68902912 | 68902912 | 68902912 | 0.0 |  |
| process threads (Toolhelp) | 60 | 60 | 60 | 60 | 60 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

minibuffer-cycle/switch: OK in 644 ms.

**isearch-cycle/lazy** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 82587648 | 82587648 | 82587648 | 82587648 | 82587648 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our reservations | 66 | 66 | 66 | 66 | 66 | 0.0 | OK |
| our committed bytes | 18137088 | 18137088 | 18137088 | 18137088 | 18137088 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 0 | 0 | 0 | 0 | 0 | 0.0 | OK (cap 256) |
| process handles | 494 | 494 | 494 | 494 | 494 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 68489216 | 68489216 | 68489216 | 68489216 | 68489216 | 0.0 |  |
| process threads (Toolhelp) | 60 | 60 | 60 | 60 | 60 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

isearch-cycle/lazy: OK in 503 ms.

**isearch-cycle/query-replace** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 82477056 | 82477056 | 82477056 | 82477056 | 82477056 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our reservations | 59 | 59 | 59 | 59 | 59 | 0.0 | OK |
| our committed bytes | 18059264 | 18059264 | 18059264 | 18059264 | 18059264 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 0 | 0 | 0 | 0 | 0 | 0.0 | OK (cap 256) |
| process handles | 494 | 494 | 494 | 494 | 494 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 68415488 | 68415488 | 68415488 | 68415488 | 68415488 | 0.0 |  |
| process threads (Toolhelp) | 60 | 60 | 60 | 60 | 60 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

isearch-cycle/query-replace: OK in 1488 ms.

**multi-cursor-cycle** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 82554880 | 82489344 | 82489344 | 82489344 | 82489344 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our reservations | 60 | 60 | 60 | 60 | 60 | 0.0 | OK |
| our committed bytes | 18128896 | 18128896 | 18128896 | 18128896 | 18128896 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 0 | 0 | 0 | 0 | 0 | 0.0 | OK (cap 256) |
| process handles | 494 | 494 | 494 | 494 | 494 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 68472832 | 68407296 | 68407296 | 68407296 | 68407296 | 0.0 |  |
| process threads (Toolhelp) | 60 | 60 | 60 | 60 | 60 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

multi-cursor-cycle: OK in 1537 ms.

**undo-cycle** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 83881984 | 83881984 | 83881984 | 83881984 | 83881984 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 2 | 2 | 2 | 2 | 2 | 0.0 | OK |
| our reservations | 67 | 67 | 67 | 67 | 67 | 0.0 | OK |
| our committed bytes | 19464192 | 19464192 | 19464192 | 19464192 | 19464192 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| undo log (bytes) | 1031200 | 983680 | 936800 | 889600 | 842240 | -942.9 | OK (cap 1048576) |
| worker thread slots | 0 | 0 | 0 | 0 | 0 | 0.0 | OK (cap 256) |
| process handles | 494 | 494 | 494 | 494 | 494 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 69689344 | 69689344 | 69689344 | 69689344 | 69689344 | 0.0 |  |
| process threads (Toolhelp) | 60 | 60 | 60 | 60 | 60 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

undo-cycle: OK in 1651 ms.

**finder-cycle/open** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 83144704 | 83144704 | 83144704 | 83144704 | 83144704 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our reservations | 70 | 70 | 70 | 70 | 70 | 0.0 | OK |
| our committed bytes | 18583552 | 18583552 | 18583552 | 18583552 | 18583552 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 1 | 1 | 1 | 1 | 1 | 0.0 | OK (cap 256) |
| process handles | 500 | 500 | 500 | 500 | 500 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 68939776 | 68939776 | 68939776 | 68939776 | 68939776 | 0.0 |  |
| process threads (Toolhelp) | 61 | 61 | 61 | 61 | 61 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

  handles of type DxgkCompositionObject: 1 | 2 | 1 | 1 | 1

  handles of type DxgkSharedSyncObject: 1 | 2 | 1 | 1 | 1

finder-cycle/open: OK in 1599 ms.

**finder-cycle/cancel** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 83144704 | 83144704 | 83144704 | 83144704 | 83144704 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our reservations | 70 | 70 | 70 | 70 | 70 | 0.0 | OK |
| our committed bytes | 18583552 | 18583552 | 18583552 | 18583552 | 18583552 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 1 | 1 | 1 | 1 | 1 | 0.0 | OK (cap 256) |
| process handles | 500 | 500 | 500 | 500 | 500 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 68939776 | 68939776 | 68939776 | 68939776 | 68939776 | 0.0 |  |
| process threads (Toolhelp) | 61 | 61 | 61 | 61 | 61 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

finder-cycle/cancel: OK in 1351 ms.

**finder-cycle/rescan** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 83238912 | 83238912 | 83238912 | 83267584 | 83267584 | 191.1 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our reservations | 70 | 70 | 70 | 70 | 70 | 0.0 | OK |
| our committed bytes | 18583552 | 18583552 | 18583552 | 18583552 | 18583552 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 2 | 2 | 2 | 2 | 2 | 0.0 | OK (cap 256) |
| process handles | 505 | 505 | 505 | 507 | 507 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 68980736 | 68980736 | 68980736 | 69013504 | 69013504 | 218.4 |  |
| process threads (Toolhelp) | 62 | 62 | 62 | 63 | 63 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

  private memory, our arenas: 18583552 | 18583552 | 18583552 | 18583552 | 18583552

  private memory, process heaps: 6766592 | 6766592 | 6766592 | 6766592 | 6766592

  private memory, thread stacks: 1642496 | 1642496 | 1642496 | 1662976 | 1662976

  private memory, other private (driver, runtime): 18173952 | 18173952 | 18173952 | 18182144 | 18182144

  private memory, writable image pages: 35676160 | 35676160 | 35676160 | 35676160 | 35676160

  private memory, writable mapped views: 1155072 | 1155072 | 1155072 | 1155072 | 1155072

  handles of type Semaphore: 76 | 76 | 76 | 77 | 77

  handles of type DxgkCompositionObject: 1 | 1 | 2 | 1 | 1

  handles of type Timer: 57 | 57 | 57 | 58 | 58

  handles of type DxgkSharedSyncObject: 1 | 1 | 2 | 1 | 1

finder-cycle/rescan: OK in 9685 ms.

**grep-cycle/done** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 83955712 | 83947520 | 83947520 | 83951616 | 83951616 | 27.3 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our reservations | 84 | 84 | 84 | 84 | 84 | 0.0 | OK |
| our committed bytes | 18968576 | 18968576 | 18968576 | 18968576 | 18968576 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 4 | 4 | 4 | 4 | 4 | 0.0 | OK (cap 256) |
| process handles | 517 | 517 | 517 | 517 | 517 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 69509120 | 69558272 | 69566464 | 69570560 | 69570560 | 81.9 |  |
| process threads (Toolhelp) | 65 | 65 | 65 | 65 | 65 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

  private memory, our arenas: 18968576 | 18968576 | 18968576 | 18968576 | 18968576

  private memory, process heaps: 6770688 | 6770688 | 6770688 | 6774784 | 6774784

  private memory, thread stacks: 1859584 | 1847296 | 1847296 | 1847296 | 1847296

  private memory, other private (driver, runtime): 18198528 | 18198528 | 18198528 | 18198528 | 18198528

  private memory, writable image pages: 35676160 | 35676160 | 35676160 | 35676160 | 35676160

  private memory, writable mapped views: 1159168 | 1159168 | 1159168 | 1159168 | 1159168

  handles of type ALPC Port: 6 | 4 | 4 | 4 | 4

  handles of type Semaphore: 79 | 80 | 80 | 80 | 80

  handles of type Timer: 60 | 61 | 61 | 61 | 61

grep-cycle/done: OK in 25728 ms.

**grep-cycle/cancel** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 83439616 | 83439616 | 83439616 | 83439616 | 83439616 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our reservations | 70 | 70 | 70 | 70 | 70 | 0.0 | OK |
| our committed bytes | 18522112 | 18522112 | 18522112 | 18522112 | 18522112 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 4 | 4 | 4 | 4 | 4 | 0.0 | OK (cap 256) |
| process handles | 517 | 517 | 517 | 517 | 517 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 69193728 | 69193728 | 69193728 | 69193728 | 69193728 | 0.0 |  |
| process threads (Toolhelp) | 65 | 65 | 65 | 65 | 65 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

grep-cycle/cancel: OK in 1003 ms.

**compile-cycle/echo** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 83566592 | 83603456 | 83603456 | 83640320 | 83640320 | 245.7 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our reservations | 87 | 87 | 87 | 87 | 87 | 0.0 | OK |
| our committed bytes | 18567168 | 18567168 | 18567168 | 18567168 | 18567168 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 4 | 4 | 4 | 4 | 4 | 0.0 | OK (cap 256) |
| process handles | 519 | 519 | 519 | 519 | 519 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 69382144 | 69423104 | 69423104 | 69455872 | 69455872 | 218.4 |  |
| process threads (Toolhelp) | 65 | 65 | 65 | 65 | 65 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

  private memory, our arenas: 18567168 | 18567168 | 18567168 | 18567168 | 18567168

  private memory, process heaps: 6791168 | 6828032 | 6828032 | 6864896 | 6864896

  private memory, thread stacks: 1847296 | 1847296 | 1847296 | 1847296 | 1847296

  private memory, other private (driver, runtime): 18198528 | 18198528 | 18198528 | 18198528 | 18198528

  private memory, writable image pages: 35676160 | 35676160 | 35676160 | 35676160 | 35676160

  private memory, writable mapped views: 1159168 | 1159168 | 1159168 | 1159168 | 1159168

compile-cycle/echo: OK in 5396 ms.

**compile-cycle/error** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 83652608 | 83652608 | 83591168 | 83591168 | 83591168 | -409.6 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our reservations | 87 | 87 | 87 | 87 | 87 | 0.0 | OK |
| our committed bytes | 18571264 | 18571264 | 18571264 | 18571264 | 18571264 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 4 | 4 | 4 | 4 | 4 | 0.0 | OK (cap 256) |
| process handles | 519 | 519 | 520 | 520 | 520 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 9 | 9 | 0.0 |  |
| working set (bytes) | 69459968 | 69459968 | 69435392 | 69435392 | 69435392 | -163.8 |  |
| process threads (Toolhelp) | 65 | 65 | 62 | 62 | 62 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

  private memory, our arenas: 18571264 | 18571264 | 18571264 | 18571264 | 18571264

  private memory, process heaps: 6864896 | 6864896 | 6901760 | 6901760 | 6901760

  private memory, thread stacks: 1847296 | 1847296 | 1777664 | 1777664 | 1777664

  private memory, other private (driver, runtime): 18198528 | 18198528 | 18173952 | 18173952 | 18173952

  private memory, writable image pages: 35676160 | 35676160 | 35676160 | 35676160 | 35676160

  private memory, writable mapped views: 1159168 | 1159168 | 1159168 | 1159168 | 1159168

  handles of type Event: 87 | 87 | 88 | 88 | 88

  handles of type ALPC Port: 4 | 4 | 3 | 3 | 3

  handles of type Thread: 61 | 61 | 62 | 62 | 62

compile-cycle/error: OK in 7597 ms.

**config-reload-cycle/color** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 83623936 | 83623936 | 83623936 | 83738624 | 83738624 | 764.5 | STEP |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our reservations | 80 | 80 | 80 | 80 | 80 | 0.0 | OK |
| our committed bytes | 18636800 | 18636800 | 18636800 | 18636800 | 18636800 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 4 | 4 | 4 | 4 | 4 | 0.0 | OK (cap 256) |
| process handles | 520 | 520 | 520 | 522 | 522 | 0.0 |  |
| USER objects | 9 | 9 | 9 | 10 | 10 | 0.0 |  |
| working set (bytes) | 69500928 | 69500928 | 69505024 | 69599232 | 69603328 | 682.6 | WATCH |
| process threads (Toolhelp) | 62 | 62 | 62 | 62 | 62 | 0.0 |  |
| video memory, local (bytes) | 16982016 | 16982016 | 16982016 | 16982016 | 16982016 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

  private memory, our arenas: 18636800 | 18636800 | 18636800 | 18636800 | 18636800

  private memory, process heaps: 6901760 | 6901760 | 6901760 | 7016448 | 7016448

  private memory, thread stacks: 1777664 | 1777664 | 1777664 | 1777664 | 1777664

  private memory, other private (driver, runtime): 18173952 | 18173952 | 18173952 | 18173952 | 18173952

  private memory, writable image pages: 35676160 | 35676160 | 35676160 | 35676160 | 35676160

  private memory, writable mapped views: 1159168 | 1159168 | 1159168 | 1159168 | 1159168

  handles of type Event: 88 | 88 | 88 | 89 | 89

  handles of type IoCompletion: 6 | 6 | 6 | 7 | 7

  handles of type DxgkCompositionObject: 1 | 1 | 1 | 2 | 1

  handles of type DxgkSharedSyncObject: 1 | 1 | 1 | 2 | 1

config-reload-cycle/color: OK in 27725 ms.

**config-reload-cycle/scale** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 84901888 | 84901888 | 84901888 | 84901888 | 84901888 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our reservations | 80 | 80 | 80 | 80 | 80 | 0.0 | OK |
| our committed bytes | 18702336 | 18702336 | 18702336 | 18702336 | 18702336 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 4 | 4 | 4 | 4 | 4 | 0.0 | OK (cap 256) |
| process handles | 522 | 522 | 522 | 522 | 522 | 0.0 |  |
| USER objects | 10 | 10 | 10 | 10 | 10 | 0.0 |  |
| working set (bytes) | 70275072 | 70279168 | 70283264 | 70283264 | 70283264 | 27.3 |  |
| process threads (Toolhelp) | 62 | 62 | 62 | 62 | 62 | 0.0 |  |
| video memory, local (bytes) | 17506304 | 17506304 | 17506304 | 17506304 | 17506304 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

config-reload-cycle/scale: OK in 550 ms.

**external-change-cycle** (220 cycles: 20 warm-up, 4 windows of 50)

| metric | after warm-up | window 1 | window 2 | window 3 | window 4 | per cycle | verdict |
|---|---|---|---|---|---|---|---|
| private commit (bytes) | 84996096 | 84996096 | 84996096 | 84996096 | 84996096 | 0.0 | OK |
| GDI objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| handle ledger (ours) | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our threads | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our events | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| our processes | 0 | 0 | 0 | 0 | 0 | 0.0 | OK |
| directory watches | 3 | 3 | 3 | 3 | 3 | 0.0 | OK |
| our reservations | 80 | 80 | 80 | 80 | 80 | 0.0 | OK |
| our committed bytes | 18702336 | 18702336 | 18702336 | 18702336 | 18702336 | 0.0 | OK |
| atlas used area (texels) | 15661 | 15661 | 15661 | 15661 | 15661 | 0.0 | OK |
| cached glyphs | 96 | 96 | 96 | 96 | 96 | 0.0 | OK |
| D3D11 live objects | 14 | 14 | 14 | 14 | 14 | 0.0 | OK |
| atlas rows | 128 | 128 | 128 | 128 | 128 | 0.0 | OK (cap 1024) |
| worker thread slots | 4 | 4 | 4 | 4 | 4 | 0.0 | OK (cap 256) |
| process handles | 522 | 522 | 522 | 524 | 524 | 0.0 |  |
| USER objects | 10 | 10 | 10 | 11 | 11 | 0.0 |  |
| working set (bytes) | 70373376 | 70373376 | 70373376 | 70373376 | 70373376 | 0.0 |  |
| process threads (Toolhelp) | 62 | 62 | 62 | 62 | 62 | 0.0 |  |
| video memory, local (bytes) | 17506304 | 17506304 | 17506304 | 17506304 | 17506304 | 0.0 |  |
| font resets | 1 | 1 | 1 | 1 | 1 | 0.0 |  |

  handles of type Event: 89 | 89 | 89 | 90 | 90

  handles of type IoCompletion: 7 | 7 | 7 | 8 | 8

external-change-cycle: OK in 27890 ms.

Summary: PASS: 22 scenario(s), 0 LEAK, 0 failure(s), 0 screenshot mismatch(es), 119 s; WATCH and STEP: config-reload-cycle/color: private commit (bytes) (STEP); config-reload-cycle/color: working set (bytes) (WATCH)

## Measurements (Phase 17)

Same machine. Sizes are bytes of files, junctions not followed (`--test` leaves a junction loop in
build\tmp\p11scan\src\loop).

### The tracked files

91 files, every one kept: the sources (63 under src\, each included by src\ronin.c or compiled by build.bat:
check_macros.c, win32_dwrite.cpp, shaders\quad.hlsl), tools\make_icon.c, res\ (imports.txt, ronin.manifest,
ronin.rc), the scripts (build.bat, verify.bat, dist.bat), the documents (README.md, CHANGELOG.md, LICENSE,
CLAUDE.md, docs\ARCHITECTURE.md, docs\MANUAL_TESTS.md), the seven README images in docs\images (each referenced
by README.md), samples\ (one file per language, now named in README.md), .gitattributes, .gitignore and
.ronin-project. None was generated, so nothing left the index; none was empty. "teal" survives only in history
notes (CHANGELOG.md, the Phase 15 decisions, Q1, and N3's color).

.gitignore: `/build/` and `/dist/` anchored at the root; `*.map`, `*.res`, `*.ico`, `*.zip` added; `*.png`
everywhere except `/docs/images/*.png` (was `/*.png`). `git ls-files -ci --exclude-standard` prints nothing;
`git status --ignored` lists only `build/` (its "Filename too long" warnings come from the junction loop).

docs\images\cursors.png was stored uncompressed: 3,073,098 -> 90,120 bytes, re-encoded with System.Drawing; both
decoded as 1280 x 800 24 bpp (LockBits) and compared byte for byte: every one of the 1,024,000 pixels identical.
Its chunks are now those of the other six images (IHDR, sRGB, gAMA, IDAT, IEND). History keeps the old blob.

### build\ before and after

| what | bytes |
|---|---|
| the teal era (build\teal*, build\base) | 35,355,162 |
| stale exes and a screenshot (p14*.exe, *_prev*.exe, ronin_bench_ll0 / _o2.exe, shot.png) | 10,755,658 |
| build\proof (scratch copies of the repository from Phases 15 and 16) | 773,695,623 |
| build\shots | 456,416,086 |
| build\tmp: ta_* and the comparison logs (cmp*, inst_text*, ret*, colon*, seed*, leak_*.log, ...) | 1,251,116 |
| build\tmp: made by hand (demo_build, crash_manual, recovery_manual, readme*, shots_jai, shots_run, ...) | 3,390,252 |
| the rest (the current outputs, the fixtures the runs make again, logs) | 436,862,466 |
| **build\ before** | **1,717,726,363** |
| dist\ before (the 0.1.0 zip) | 224,440 |
| `build.bat clean` (20.4 s, exit 0) | build\ and dist\ gone |
| after verify.bat from nothing (42.7 s, PASS) | 47,283,953 |
| after verify.bat phase (and this phase's --bench-grep runs: their 100 MB file and p12bench tree, 9,721,247 bytes of measurement copies) | 293,076,300 |

### build.bat clean and verify's clean-check

clean-check builds a scratch tree in build\tmp\clean_check and runs a copy of build.bat from another directory
that looks like a repository: build\ present with a junction to a directory outside it (exit 0, both gone, the
target's sentinel kept), missing (exit 0), build\ a junction (exit 10, the junction and its target kept), dist\ a
file (exit 10), not the repository root (exit 10, its build\ kept); the current directory's build\ kept; then
the tree goes (rd /s, junctions removed, not followed) and must be gone. 110 ms. With the junction refusal
removed it fails: "clean-check: junction (exit 0); junction: removed" (even then rd /s removed only the link).

### The fresh clone

`git clone` of the repository into build\tmp\fresh, then there, with nothing else; twice: after the build
changes (be293bd) and with the final code (f21521a; the commits after it change only documents):

| step | be293bd | f21521a |
|---|---|---|
| git clone (91 files) | 1.1 s | 1.2 s |
| build.bat | 2.9 s, build\ronin_debug.exe 2,581,504 bytes | 2.8 s, 2,588,672 bytes |
| build.bat release | 3.7 s, build\ronin.exe 398,336 bytes | 3.5 s, 398,848 bytes |
| build.bat bench | 6.2 s, build\ronin_bench.exe 1,355,776 bytes | 5.9 s, 1,361,408 bytes |
| verify.bat | 40.6 s, PASS (clean-check 110 ms) | 40.1 s, PASS (clean-check 100 ms) |

Both: check-imports KERNEL32 77, USER32 50, GDI32 6 functions; "ronin 0.1.0". The release exe was unchanged from
Phase 16 until f21521a (398,336 bytes) and is 398,848 bytes after it (the results fix: release code); the same
three DLLs. dumpbin /imports, counting every line that starts with an address, gives 145 as in Phase 16; 12 of
them are the four header lines of each DLL, so 133 functions, as --check-imports counts.

### The measurement flags and the built-in config

Before be293bd `--memory-report`, `--bench-latency`, `--startup-capture` and `--bench-longline` read
%APPDATA%\ronin\ronin.conf. Run once each afterwards (built-in config, bench build), against Phase 15:

| `--memory-report` (KB) | built-in, run 1 | built-in, run 2 | `--config` the user's ronin.conf | Phase 15 (MB) |
|---|---|---|---|---|
| private bytes | 61,476 | 62,412 | 63,200 | 60 |
| our arenas | 3,032 | 3,032 | 3,136 | 3.0 |
| process heaps | 6,320 | 6,232 | 6,132 | 6.4 |
| thread stacks | 1,400 | 1,388 | 1,388 | 1.4 |
| the driver's other private allocations | 14,224 | 15,304 | 16,032 | 15.1 |
| writable image pages | 34,540 | 34,540 | 34,540 | 33.9 |
| writable mapped views | 1,128 | 1,128 | 1,128 | 1.1 |

The same as Phase 15: the driver's row moves by ~1 MB from run to run with either config; with the user's config our
arenas hold ~100 KB more.

`--bench-latency`, composed by DWM: arrival to Present returned p50 401 us, p99 704 us (Phase 15: 0.41 ms, 0.89
ms); on screen p50 17.9 ms, p99 21.3 ms (17.1, 21.3); command + frame build p50 26 us (23). An overlay run: on
screen p50 4.8 ms (4.9). The same as Phase 15 within the run-to-run spread. The first of six runs exited 9:
482 of the 500 keys had a Present of their own (the bench wants all 500); the five after it, two of them with
`--config` naming the user's ronin.conf, passed with 500. Intermittent and not the config ("Later").

### The leak check's grep-cycle/done and the results arenas

The first `verify.bat phase` of this phase failed: `--leak-check` gave grep-cycle/done LEAK at 200 cycles, "our
committed bytes" +16 KB then +32 KB between windows 1 and 2, then flat; run alone it was LEAK in 1 of 3 runs, all
ending at 5,742,592 bytes. Logging each of the slot's seven arenas at every search's end (3 runs, 220 searches):
only held[0] moved, 8 -> 16 -> 32 -> 64 KB, reaching 64 KB at search 1, 57 or 67 depending on the run; arena 64 KB,
loc 64 KB, state 128 KB, held[1] 0, pend 4 KB and mark 0 the same in every search. held[0] is reset only when
nothing is held, so its high-water is everything appended while the writer waited on a file the workers had not
finished: thread timing.

Three fixes, each measured with `--bench-grep` against the old code (fix-off), "ronin_grep_common" (52,138 matches
in 3,001 files), runs alternated, the display kept on (SetThreadExecutionState from the script running them; the
first attempts were spoiled by the display going to sleep: every Present 6.9 ms, first results ~6 ms in both
builds, those runs discarded); idle Present 81-88 us in every run kept:

| workers | first result, fix-off (ms) | longest frame, fix-off (us) | all given back at the end | 1 MB floors, above given back at the end | 256 KB floors kept, above given back on minimize (committed) |
|---|---|---|---|---|---|
| 1 | 1.51-1.82 | 179-329 | 1.58-1.90 / 319-366 | 1.50-1.68 / 254-522 | 1.53-1.55 / 190-258 |
| 2 | 1.58-2.07 | 258-442 | 1.68-1.97 / 468-555 | 1.62-1.81 / 504-592 | 1.67-1.71 / 282-379 |
| 4 | 1.74-2.55 | 411-618 | 2.06-2.50 / 646-763 | 1.84-2.04 / 713-736 | 2.07-2.39 / 405-530 |
| 8 | 2.41-2.96 | 849-1,713 | 2.52-2.92 / 1,120-1,333 | 2.35-3.03 / 1,015-1,678 | 2.69-3.21 / 936-1,180 |
| cap 10,000 | 2.41-3.00 | 723-1,061 | 2.57-2.80 / 777-986 | 2.49-2.66 / 628-1,117 | 1.94-2.64 / 655-978 |

(the variants' cells: first result ms / longest frame us; fix-off over its 10 clean runs, the first variant against
3 runs before it taken back to back, the other two alternated with 3 fix-off runs each.) Giving back at a run's
end cost the final frame: timed in one run, 1,498 runs ended, 762 with nothing above the 1 MB floors (free), the
others held[0] 4.5-4.9 MB, held[1] 2.0-2.2 MB, pend 320 KB above them, given back in 294-456 us at most. The kept
floor runs exactly the old code per search (one 768 KB commit at the buffer's first run), within noise everywhere.

After f21521a, `--leak-check grep-cycle` 5 times: grep-cycle/done OK in every run, "our committed bytes"
6,459,392 in every sample of every run (5,742,592 before: three 256 KB floors in place of held[0]'s 64 KB and pend's 4 KB); grep-cycle/
cancel OK, 5,369,856. The regression test "results transient" fails with the floor off (held 4096 / 0, pend 4096
after a search) and with the trim off (3,145,728 / 2,097,152 / 1,048,576 after a minimize).

The release exe: 398,848 bytes, 512 more than Phase 16's 398,336 (the floor's commit and app_trim, release code);
the same three DLLs.

### verify.bat phase

At f21521a: PASS in 387 s. The seed sweep: 256 seeds PASS (166.7 s). The soak: bench 1,000,000 events PASS in
11.6 s (private bytes peak 16,772 KB, 14,264 at the end), debug PASS in 13.0 s (16,744 / 17,040 KB). The leak
check: 22 scenarios, 0 LEAK, 126 s; STEP: file-cycle-100mb and config-reload-cycle/color private commit (the
process heap's irregular moves Phase 16 accepted); the exit check PASS.

### Outside the repository

Nothing there was deleted. Left for the owner: %APPDATA%\teal\teal.conf (9,142 bytes, 2026-10-08),
%TEMP%\teal_ids.txt, C:\web-projects\teal-test\, and possibly Windows Error Reporting entries from the crash
tests (%LOCALAPPDATA%\Microsoft\Windows\WER, not checked). %APPDATA%\ronin\ronin.conf is the live config.

## Measurements (Phase 18)

Same machine (AMD Ryzen 7 7700X, a 2560x1440 display at 144 Hz, a 1280x800 window).

### The command list (`--bench-modal`)

Bench build, the built-in config, 136 commands; five rounds of open (C-S-p), 20 filter keystrokes ("save buffer
kill wor"), 20 movements (<down>, C-v, <up>, M-v, five each) and close (C-g), one event per frame. Filter = the
owner's scoring and ranking of every command; build = the command and the frame build (Present not counted).
Three runs:

| part | filter median | filter max | build median | build max |
|---|---|---|---|---|
| open (5) | | | 145-199 us | 160-382 us |
| filter keystroke (100) | 8 us | 49-158 us | 25-27 us | 184-190 us |
| movement (100) | | | 10 us | 20-166 us |
| close (5) | | | 8-10 us | 11-17 us |

Before the character masks were added the filter's median was 87 us and its max 353 us (the 0.2 ms budget was met
by the median, not by every keystroke); with them both are well inside it. Opening builds the 136 rows, their
"name description" strings and their key texts (136 where-is lookups over the default keymaps).

Memory: private bytes +1,076 to +1,228 KB at the first open, of which our arenas' commit +64 KB (1,708 -> 1,772
KB) and 10 reservations (the field's Buffer and View, the modal's arena); after the first round nothing more (+0
to +28 KB over the next four). The rest of the first open's growth is not ours (classified in Measurements (Phase 19)).

### Idle and latency

`ronin_debug.exe --keys "C-S-p a" --idle-check`: PASS, 0 frames, 0 us of CPU in 5 s with the command list open
(the same as without it: 10 main loop passes for the shell's WM_GETICON messages in both).

`--bench-latency` against the Phase 17 code (2079239, a bench build of it in build\proof), alternated, 3 runs
each, the window composed by DWM for its first 100 keys and shown directly (overlay) after:

| | arrival to Present returned p50 / p99 | command + frame build p50 | on screen (DWM) p50 | on screen (overlay) p50 |
|---|---|---|---|---|
| Phase 17 | 426-436 us / 4.61-4.88 ms | 16-17 us | 17.7-17.8 ms | 4.3-4.4 ms |
| Phase 18 | 427-439 us / 4.27-4.85 ms | 17 us | 17.8-18.1 ms | 4.2-4.8 ms |

Unchanged within noise. (Both builds' p99 to Present is higher today than Phase 17 recorded, 704 us: the machine,
not the code.)

### Size and imports

The release exe: 422,400 bytes, 23,552 more than Phase 17's 398,848. By module (the linker's map, `--map-report`,
bytes in sections): string literals +7,080 (the descriptions), commands_list.c +5,928, modal.c +3,264,
config_default.h +1,328 (the new section and colors), unwind tables +1,496, win32_main.c +1,168, keymap.c +1,020
(where-is), app.c +1,000 (the routing, the drawing of the field), the rest +1,084 together (23,368 in sections in all). Imports unchanged:
KERNEL32 77, USER32 50, GDI32 6 functions.

### The new tests fail when they should

On a scratch copy (build\proof, deleted afterwards), each break alone, `--test` (debug):

| break | result |
|---|---|
| where-is ignores shadowing (no earlier-map or prefix check) | exit 9: "where-is: newline (cap 8): 3 'RET, C-m, C-j', expected 1 'C-j'" |
| where-is keeps only one sequence | exit 9: "where-is: undo (cap 2): 3 'C-/, C-x u', expected 3 'C-/, C-_'" and the command list's key text for undo |
| forward-char's description 61 bytes long | exit 9: "descriptions: 'forward-char': ...: longer than 60 bytes" |
| forward-char's description ending with a period | exit 9: "descriptions: 'forward-char': \"Move point forward one character.\": a trailing period" |

### Screenshots

docs/images/command-list.png: `ronin_debug.exe --config build\tmp\none.conf src\modal.c --keys "C-S-p f i l e"
--screenshot`, re-encoded with System.Drawing (3,073,098 -> 141,305 bytes, every pixel identical). Checked with
System.Drawing: the frame #2e8b8b, the field and an unselected row's empty part #0c3434, the selected row #0000ff,
the window below the box #072626; the first row's keys contain #7ad0c6 and a description #a8977a.

### verify.bat phase

At 4003df9 (the code of this phase): PASS. The seed sweep: 256 seeds PASS (168.9 s; the new "modal" group ~34 ms a
seed). The soak: bench 1,000,000 events PASS in 10.8 s (private bytes peak 17,164 KB, 14,632 at the end; 3,082
scripted command list sessions besides the random keys that open it), debug PASS in 14.8 s (17,760 / 16,984 KB).
The leak check: 24 scenarios, 0 LEAK, 120 s, modal-cycle/browse and modal-cycle/run OK (browse's frames before and
after equal); STEP: config-reload-cycle/color private commit (the process heap's irregular moves Phase 16
accepted); the exit check PASS.

## Measurements (Phase 19)

Same machine (AMD Ryzen 7 7700X, a 2560x1440 display at 144 Hz, a 1280x800 window).

### The command list's first open, by owner

`ronin_bench.exe --bench-modal` now walks the memory (`--memory-report`'s classification) before the first open and after
the first round (open, 20 filter keystrokes, 20 movements, close); the walk's own scratch (~1.1 MB of heap ranges) is
committed before the first reading. Four runs:

| owner | first round |
|---|---|
| our arenas | +64 KB (10 reservations: the field's Buffer and View, the modal's arena) |
| process heaps | +4 to +284 KB (5 heaps before and after) |
| thread stacks | +44 to +52 KB: one more thread in the process (59 -> 60), not ours (our threads 0 and 0) |
| other private allocations (the D3D11 driver and runtime) | +936 KB every run: one region more and the largest one +656 KB |
| writable image pages, mapped views | +0 |

Private bytes moved +840 to +1,128 KB. Ours is the 64 KB the design makes (kept until the exit, as the minibuffer's);
ronin allocates nothing from a heap (the release build has no C runtime), so the heaps' growth belongs to the system DLLs,
and the new thread and the 936 KB to the graphics driver (the first frames with ~5,000 more glyph quads). Windows' and the
driver's: recorded, not pursued (the Phase 19 instruction). Later opens add nothing.

### git's output (`--bench-git`)

Bench build, three runs. The parsers on generated output in memory, 20 runs each: a diff of one file with 50,000 lines
(3,032,869 bytes: hunks of 166 context, 166 removed and 166 added lines of ~60 bytes) and a status of 5,000 files in
50 directories (181,522 bytes, every tenth a rename with its source). Then a 20,000-line diff of the fake git
(tmp\p19bench) copied into the open view, 5 rounds of C-x g, the copy, C-g:

| part | median | max |
|---|---|---|
| parse the 50,000-line diff (on git's worker) | 1,436-1,517 us | 1,661-1,810 us |
| parse the 5,000-file status (on git's worker) | 135-136 us | 146-169 us |
| the copy of the 20,000-line diff: frames | 1 | 1 |
| ... its longest slice (the frame's background work) | 69-91 us | 75-131 us |
| ... the longest frame build while it runs | 154-197 us | 168-242 us |
| the frame showing the whole diff afterwards | 14-17 us | 17-22 us |

The copy fits the 2 ms budget about 15 times over: it is never sliced at the default limits; the slicing (the clock
every 256 lines) is exercised by the tests with `dev_work_budget` (4,096 lines a frame). One fake-git process (the
bench exe started with `--fake-git`, its script, its exit) takes ~15 ms on the worker ("git job: the cut diff took
15 ms").

### Idle and latency

`ronin_debug.exe --keys "C-x g" --idle-check` from the repository root (the real git on ronin's own tree): "a modal box
open 1, git jobs running or pending 0" when the 5 s began, then PASS, 0 frames, 0 us of CPU, 0 main loop passes
besides the marks.

`--bench-latency` against the Phase 18 code (28e614a, a bench build of it in build\proof\p18), alternated, 3 runs each;
DWM composed the window throughout this time (no overlay rows):

| | arrival to Present returned p50 / p99 | command + frame build p50 | on screen (DWM) p50 |
|---|---|---|---|
| Phase 18 | 359-407 us / 729-844 us | 22-27 us | 17.8-18.1 ms |
| Phase 19 | 369-412 us / 779-855 us | 21-29 us | 17.6-17.9 ms |

Unchanged within noise.

### Size and imports

The release exe: 448,512 bytes, 26,112 more than Phase 18's 422,400. By module (the linker's map, `--map-report`,
bytes in sections, 26,540 in all): git_changes.c +9,616, git.c +7,736, app.c +2,696 (the hooks and `app_draw_text_line`),
unwind tables +1,992, config_default.h +1,832, string literals +1,496, win32_main.c
+1,456 (the program start, the environment block, the PATH walk), modal.c +1,168, minibuffer.c +432; compile.c -1,608
and commands_list.c -928 (inlining moved by /GL); the rest +240 together. Imports: KERNEL32 79 (+2:
GetEnvironmentStringsW, FreeEnvironmentStringsW), USER32 50, GDI32 6 functions; no new DLL.

### The new tests fail when they should

On a scratch copy (build\proof\p19, deleted afterwards), each break alone, `--test` (debug):

| break | result |
|---|---|
| the parser drops a diff's last hunk | exit 9: "git parse: two hunks:" (the second hunk missing from the golden) and "git changes: status 'main: 8 changed files', panes 8 / 10" (a.c's 14 lines cut to 10) |
| a cached diff fetched again when its file is shown again | exit 9: "git changes: the diffs fetched: 'a.c\|old name.c new name.c\|a.c\|old name.c new name.c\|gone.c' (5 jobs)" |

A first try of the second break (a cached row fetching itself at once) failed too, less directly: the view never
settled ("git changes: did not open").

### The soak's checks under the new event stream

The first `verify.bat phase` of this phase failed in soak-debug (exit 9); the bench soak passed with 1.9 MB of growth,
just under the limit. Three findings, each measured before anything changed (the details in Decisions 15):

- At the 600,000 checkpoint the soak counted 2 more threads, 2 more events and 2 more handles of ours, back by 700,000
  (LEAK rows). Logged at the sample: no git job busy, a project search running ("results busy 1", its two workers). The
  settle had never waited for searches; this stream starts one just before a checkpoint. Fixed (64fec0c).
- Settled memory without the undo logs, 2,000,000 debug events: 8,469 KB after the first block, then 10.3-14.5 MB with
  no trend (12.2 MB at 1,000,000, 12.8 MB at 2,000,000). By owner from the first block to the plateau: our arenas
  without the undo logs +1.95 MB (buffers of the random walk, as Phase 16 found), process heaps +0.4 MB (560 -> 976 KB,
  flat from the middle on; CreateProcess's), thread stacks +0.15 MB (worker slots 9 -> 11), image pages and views +0,
  and +1.8 MB the walk does not classify (page tables and the like; Phase 18's soak, run alone with the same log: +1.1
  MB). Phase 18 on the same machine: 11.6 -> 12.1 MB, its first block already high. A walk that reaches its large states
  later, not a leak: judged now against the first half's highest (fec6d72).
- The debug probe stepped from ~1.6 to ~2.4 us after the first block in some runs and not in others, with the same code
  and seed (the walk is not fully deterministic: when workers' answers come depends on the clock). The same state probed
  twice in a row: up to 35% apart (1,941 and 2,325 ns; 1,763 and 2,293); a fixed computation of 20,000,000 multiplies
  slowed at the same checkpoints (15.2 -> 16.6-17.4 ms); the app's state at the probe (buffers, windows, the modal, jobs,
  *Messages*, the kill ring, the history) the same in fast and slow blocks; during one soak Chrome used 35.6 s of CPU in
  25 s and NVIDIA Broadcast 4.6 s. The bench build's probe stayed flat (457-565 ns). No cost of ronin's: the probe is now
  the fastest of three runs (f1b92ca). After the three changes: debug PASS (8,520 KB, the first half's highest 12,588 KB,
  12,992 KB at the end; probes 1,570-1,763 ns), bench PASS (11,704 / 12,368 KB; 457-565 ns), every leak metric OK.

### Screenshots

docs/images/git-changes.png: `ronin_debug.exe --config build\tmp\none.conf build\tmp\gitrepo\repo\b.c --keys "C-x g"
--screenshot`, re-encoded with System.Drawing (3,073,098 -> 23,547 bytes, every pixel identical). Checked with
System.Drawing: diff_removed #ff6e6e, diff_added #73e673, diff_hunk #7ad0c6, diff_header #c1d1e3, git_status #0fdfaf,
the selected row #0000ff, the box #0c3434 inside a #2e8b8b frame, the status line #a8977a, the window below #072626.

### verify.bat phase

At f1b92ca (the code of this phase): PASS in 7 min 56 s. The seed sweep: 256 seeds PASS (199.0 s, 168.9 s in Phase 18;
the new "git" group ~0.5 s a seed). The soak: bench 1,000,000 events PASS in 21.2 s (10.8 s in Phase 18; 148 git
sessions besides the random keys that reach C-x g; settled 8,460 KB after the first block, 11,712 KB at most in the
first half, 11,656 KB at the end), debug PASS in 27.4 s (8,396 / 13,092 / 13,396 KB). The leak check: 26 scenarios,
0 LEAK, 141 s, git-cycle/browse and git-cycle/cancel OK (browse's frames before and after equal); STEP:
config-reload-cycle/color and /scale private commit (the process heap's irregular moves Phase 16 accepted); the exit
check PASS. The first run of this phase failed in soak-debug: "The soak's checks under the new event stream" above.

## Measurements (Phase 20)

Same machine (AMD Ryzen 7 7700X, a 2560x1440 display at 144 Hz, a 1280x800 window).

### Idle and latency

`ronin_debug.exe --idle-check`: PASS, 0 frames, 0 us of CPU in 5 s, 0 main loop passes besides the marks.

`--bench-latency` against the Phase 19 code (db8061e, a bench build of it in build\proof\p19), alternated, 3 runs each;
DWM composed the window throughout:

| | arrival to Present returned p50 / p99 | command + frame build p50 | on screen (DWM) p50 |
|---|---|---|---|
| Phase 19 | 362-368 us / 857-1,009 us | 21 us | 18.0-18.4 ms |
| Phase 20 | 365-374 us / 743-751 us | 21-22 us | 17.3-18.6 ms |

Unchanged within noise: nothing on the typing path changed.

### Size and imports

The release exe: 453,120 bytes, 4,608 more than Phase 19's 448,512. By module (`--map-report`, bytes in sections,
3,884 in all): terminal.c +2,256, config_default.h +704 (the settings' comments and the two bindings), win32_main.c
+672 (`os_process_launch`, `os_system_program`), string literals +584, unwind tables +172, ronin.obj +24, command.c
+16; git.c -368 and config.c -176 (inlining moved by /GL). Imports: KERNEL32 79, USER32 50, GDI32 6 functions,
unchanged (CreateProcessW and GetSystemDirectoryW were imported already); no new DLL.

### The new tests fail when they should

On a scratch copy (build\proof\p20, deleted afterwards), each break alone, `--test` (debug):

| break | result |
|---|---|
| open-terminal resolves the project's root instead of the file's directory | exit 9: "terminal: line 1: got '...\term proj\|wt.exe\|0\|-d "...\term proj"' want '...\term proj\sub\|...'" |
| `os_process_launch` leaves the thread handle open | exit 9: "launch: 100 launches: ... ledger 0 -> 101, ... handles 189 -> 288" and "terminal: 200 launches: ledger 119 -> 319, ... handles 307 -> 506" |
| git.c before d941010 | exit 9: "git parse: a warning before a source" and "git changes: status 'main: 8 changed files', panes 8 / 15" |
| only the hunk body's warning skip taken out | exit 9: "git parse: diff 9" (the warning a context line, "-b" and "+B" headers) and the same "panes 8 / 15" |

### The terminals under the soak and the leak check

The soak's random keys reach C-c t and C-c a: 1,000,000 events started 1,924 fake terminals in the bench soak (960 /
964) and 1,919 in the debug one, every call in an existing directory with wt's command line. The bench soak took 30.0 s,
19.7 s for Phase 19's code on the same seed (rerun for this): the ~10 s are those process starts, ~5 ms each on the main
thread (CreateProcessW of the fake, with ~1,900 short-lived processes competing for the cores). A real C-c t pays that
once per key, never on the typing path. The five slowest soak events (70-99 ms: C-g and RET at events ~111,000,
~112,000 and ~536,000) are the same in Phase 19's run: not this phase's. `--leak-check terminal-cycle`: 220 launches,
every call as expected, 0 LEAK (the ledger and our processes the same in every window), 1.2 s.

The bench soak of `verify.bat phase` settled at 38,096 KB after its first block (our arenas 33,600 KB), 39,620 KB at
most in the first half and 40,024 KB at the end: within the +2 MB rule, every leak metric OK. A rerun of the same
100,000 events settled with our arenas at 6,108 KB, as the debug soak (6,428 KB) and Phase 19's (5,644 KB): the walk
is not fully deterministic (Phase 19: when the workers' answers come depends on the clock), and that run reached a
larger state early. Recorded, not pursued.

### verify.bat phase

At 24dc383 (the code of this phase): PASS in 7 min 45 s. The seed sweep: 256 seeds PASS (175.0 s). The soak: bench
PASS in 30.0 s (above), debug PASS in 33.4 s (settled 10,196 KB after the first block, 12,688 KB at most in the first
half, 13,732 KB at the end). The leak check: 27 scenarios, 0 LEAK, 137 s, terminal-cycle OK; STEP:
config-reload-cycle/color private commit (as in Phase 19); the exit check PASS. No run reached the guard
(`os_dev_real_refused` 0 at every end).

## Measurements (Phase 21)

Same machine (AMD Ryzen 7 7700X, a 2560x1440 display at 144 Hz, a 1280x800 window).

### The soak's slowest events (the Phase 20 question)

The 70-99 ms "C-g" and "RET" events of Phase 19's and 20's soaks are not those keys. `soak_event` runs a whole git
changes session as one event (C-x g, then `soak_git_wait` pumping until the fake git is idle, up to 3 moves, half of
them waited for, then C-g, RET or a click), the timer wraps all of it, and the log named an event by the last note of
the ring: the closing key. Each wait covers fake-git processes (the bench exe started with `--fake-git`, ~15 ms each
on git's worker): opening takes rev-parse, status and the first diff, a waited move one diff or (g) status and a diff.
C-g closes the box with a cancel flag and no join, RET visits a small file; the soak runs no builds and saves without
FlushFileBuffers. A soak artefact, by your rule: recorded, no fix. Since 6c07f4f a slow event names its kind, its first
and last notes, how many it made and its time in `soak_git_wait`, and a second list ranks the events without those
waits. The confirming run (bench, `--soak 1000000`, seed 0x7ea1, PASS in 26.2 s):

| | event | kind | notes | in all | waiting for git |
|---|---|---|---|---|---|
| 1 | 648,820 | git session: C-x g .. a click | 20 | 68,446 us | 68,351 us |
| 2 | 112,295 | git session: C-x g .. C-g | 14 | 66,159 us | 66,068 us |
| 3 | 536,762 | git session: C-x g .. RET | 13 | 64,770 us | 64,319 us |
| 4 | 992,418 | git session: C-x g .. C-g | 12 | 59,545 us | 59,474 us |
| 5 | 111,238 | git session: C-x g .. C-g | 13 | 58,776 us | 58,659 us |

Without the waits these sessions take 71-451 us. The slowest events then are saves: C-x C-s 45.0, 22.6 and 20.6 ms
and the save question's `y` 20.7 and 18.7 ms (ReplaceFileW; the soak saves without fsync), the Later item "saving on a
background thread".

### Startup, idle, latency

`--startup-ms` (release, with the user's own ronin.conf, run with the user's permission), alternated, 3 runs each:
Phase 20's build (44d40c2, build\proof\p20) 207 / 182 / 175 ms, Phase 21's 187 / 175 / 173 ms: unchanged. The config's
parse at startup, with the defaults and the ronin theme: 362 us in the bench build (Phase 20: 294 us; the built-in
configuration is the same size, the theme is one more parse of 601 bytes).

`ronin_debug.exe --idle-check`: PASS, 0 frames, 0 us of CPU in 5 s, 0 main loop passes besides the marks.

`--bench-latency` against Phase 20, alternated, 3 runs each, nobody at the desktop (DWM composed the first 99 keys
and showed the window directly afterwards in every run):

| | arrival to Present returned p50 / p99 | command + frame build p50 | on screen p50 (composed / overlay) |
|---|---|---|---|
| Phase 20 | 480-500 us / 3,398-4,976 us | 18-20 us | 17.8-18.0 ms / 4.1-4.2 ms |
| Phase 21 | 458-497 us / 3,075-4,800 us | 18-19 us | 17.8-18.1 ms / 4.2-4.4 ms |

Unchanged: nothing on the typing path changed. Earlier runs the same day, with someone at the desktop, lost keys in
both builds (Phase 20 in 1 of 15 runs, Phase 21 in 8 of 26: exit 9, some keys without a frame). Instrumented once (not
committed): for 46-62 ms per key the main loop drew no frame while a redraw was pending, the window shown and not
minimized; the cause logged was WM_NCLBUTTONDOWN dispatched for 362 and 480 ms (Windows' modal move loop: a click on
the bench window's title bar, which opens under the pointer), once Ctrl held. Not ronin's; the Later list has the move
loop.

### Switching a theme

M-x set-theme reads ronin.conf again and parses the defaults, the ronin theme and the chosen one: 243-320 us a switch
in the bench build (the log's "parsed with the defaults"), then one frame; the font is not set up again (smoke stage 14
and the smoke's later check; `--leak-check theme-cycle`: 220 cycles of five switches, every metric flat, the font never
reset, the screenshots equal, 5.4 s).

### Size and imports

The release exe: 470,528 bytes, 17,408 more than Phase 20's 453,120. By module (`--map-report`, bytes in sections):
the built-in themes (build\gen\themes.h) 4,752 (their texts 4,730); app.c +9,424 (`config_poll` and the theme lookup
inlined into `app_poll_config` by /GL, ~4.7 KB that left config.c; set-theme's candidates with `theme_list` inlined
2,048, its done callback 880, the shared diagnostics log 704, the themes\ watches 528); string literals +1,024;
config.c +892 (the theme mode and the resolution, after what moved to app.c); config_default.h +544 (the commented
list of colors and the theme setting); unwind tables +532; ronin.obj +264; theme.c 212. No dev code in the release map.
Imports: KERNEL32, USER32, GDI32, unchanged (`--check-imports`); no new DLL.

### The new tests fail when they should

On a scratch copy (build\proof\p21, deleted afterwards), each break alone, `--test` (debug):

| break | result |
|---|---|
| the theme laid over ronin.conf's [colors] (the user's keys not restored) | exit 9: "themes: order: [colors] over the theme (keyword 222222)", "themes: app: 30 overrides: echo ..., 1 in *Messages*", "themes fuzz: round 0 (seed 0x7ea1): text is d3297c, want b679e0" |
| the themes\ folders never watched | exit 9: "theme reload: themes\ created after the start: 1 notification(s), watched 0" |

`tools\make_themes.c` on scratch folders: one with a 9 KB theme ("the embedded themes take 8192 bytes or more"), one
without ronin.conf ("themes\ronin.conf is missing") and one with "bad name.conf" ("not a theme name"): each exit 1, no
header written.

### Screenshots

docs\images\themes\moonfly.png, oxide.png, vague.png, mellifluous.png: `ronin_debug.exe --config
build\tmp\shot_<name>.conf +17:5 samples\ring.c --keys "C-x 3 C-x o C-x C-f s h a p e s . t s RET C-x o C-SPC C-n C-n
C-e" --screenshot ...` (a config with `theme = <name>` only), each looked at, re-encoded with System.Drawing (3,073,098
-> 103,827-107,738 bytes, every pixel identical) and six pixels read back against its file: an empty cell, the region,
the cursor, both mode lines' backgrounds and the divider, all equal.

### verify.bat phase

At 90fe03e (the code of this phase): PASS in 8 min 51 s. `--test` (debug) PASS in 38.5 s. The seed sweep: 256 seeds
PASS (242.6 s; the new group "themes" with them). The soak, now with theme sessions (485 in a million events) and
set-theme in the command list sessions: bench PASS in 22.4 s (settled 9,940 KB after the first block, 18,600 KB at most
in the first half, 12,380 KB after the last), debug PASS in 28.1 s (10,528 / 18,452 / 12,812 KB); the slowest event of
each a git session (61.7 and 60.6 ms, all but ~0.1 ms of it waiting for git), the slowest without the waits a C-x C-s
(39.0 and 36.8 ms). The leak check: 28 scenarios, 0 LEAK, 148 s, theme-cycle OK (its frames equal pixel for pixel);
STEP: config-reload-cycle/color private commit (as in Phases 19 and 20); the exit check PASS. No run reached the guard.

## Measurements (Phase 22)

Same machine.

### Size and imports

The release exe: 471,040 bytes, 512 more than Phase 21's 470,528: oxide's `diff_added` line made the embedded themes 4,749
bytes (from 4,730), which crossed one section alignment. The README check is dev code (not in the release exe). Imports:
KERNEL32, USER32, GDI32, unchanged (`--check-imports`); no new DLL.

### The README check

`test: ok: readme` in 3-6 ms (bench and debug): README.md's 59 keys in 11 key tables (global ones through describe-key
itself, the search-results and compilation ones through describe-key in a buffer with that keymap, isearch and
git-changes through their map then the global one), 12 settings, 2 config blocks, and in README.md and CONTRIBUTING.md 22
prose keys, `M-x` commands and names, 34 links (29 relative, 5 external). The 16 broken fragments (a wrong command, a key
not written as describe-key prints it, an unbound key, a key of the git-changes map, a row without `code`, an unknown
setting, a value out of range, a bad color in an ini block, an unknown name, a [keys] section in a theme block, an
unknown `M-x` command, an unbound prose key, a missing file, a missing anchor here and in another file, a README without
a key table) are each caught at their line in every run.

### The new test fails when it should

On a scratch copy (build\proof\p22, deleted afterwards), each break alone, `--test` (debug):

| break | result |
|---|---|
| README.md:180, `project-find-file` written `project-find-files` | exit 9, exactly one failure: "README.md:180: `C-x p f`: describe-key says "C-x p f runs the command project-find-file", the table says project-find-files" |
| README.md:107, `font_size` written `font_sise` | exit 9, exactly one failure: "README.md:107: unknown setting 'font_sise'" |

### Screenshots

Every image README showed was retaken with `ronin_debug.exe --config build\tmp\none.conf` (an empty file: the built-in
configuration) and compared pixel for pixel with System.Drawing:

| image | retaken with | result |
|---|---|---|
| hero.png (new) | `--config build\tmp\shot_hero.conf` (`startup_windows = 2`) `+17:1 samples\particles.jai +13:4 samples\ring.c --keys "M-x w i n d o w"` | added, 3,073,098 -> 101,075 bytes, every pixel identical; read back: the divider #126367, the selected mode line #d3b58d, the other #072626, the selected candidate #0000ff, the cursor #90ee90 |
| command-list.png | `src\modal.c --keys "C-S-p f i l e"` (as in Phase 18) | stale (31/136 rows, no open-terminal): replaced, 142,539 bytes, every pixel identical; the frame #2e8b8b, inside #0c3434, the selection #0000ff |
| isearch.png | `+13:1 samples\ring.c --keys "C-s r i n g"` | identical, kept |
| query-replace.png | `+26:1 samples\ring.c --keys "M-% r i n g RET q u e u e RET"` | identical; no longer shown, removed |
| mx.png | `+13:1 samples\ring.c --keys "M-x w i n d o w"` | 64 pixels (1/15 -> 1/16 commands); the hero shows M-x now, removed |
| windows.png | (the hero replaces it) | removed |
| themes\*.png (4) | as in Phase 21 | identical, kept |
| compilation.png | a stand-in Jai build in build\tmp\demo_build, `--keys "C-x p c RET" --compile-wait 5000 --keys-after "M-g n"` | one cell differs (the build's time, 0.05 against 0.04 s): kept |
| cursors.png | `+131:62 src\cursors.c --keys "C-SPC C-f C-f C-f C-c C-<"` | the same UI (mc:111, "Marked 111 occurrences") over newer source: kept |
| finder.png | `+406:1 src\finder.c --keys "C-x p f w i n"` | the same UI over newer source and file counts: kept |
| git-changes.png | `build\tmp\gitrepo\repo\b.c --keys "C-x g"` (as in Phase 19) | cannot be retaken: "git: no real git in a test run" (Phase 20's guard, every batch run); the view is unchanged since Phase 19 and smoke stage 12 checks its colors on every verify: kept (Later) |

### The CI workflow

Its steps were tried locally in Windows PowerShell as written (the runner's pwsh 7 runs the same lines):
`--check-imports` PASS, `--version` "ronin 0.1.0", `ronin_bench.exe --test` PASS in 32 s, `ronin_bench.exe --smoke` PASS
(89 frames, "d3d11: device created ..., debug layer off"). The workflow itself runs only on GitHub: its first run is
HUMAN step 50, and the informational smoke's outcome is recorded here once the user reports it.

### verify.bat phase

Twice at b49ff05 (the phase's code before the soak fix), 572 s and 553 s: FAIL, both times only `soak-debug` (exit 6); every other step
PASS. `--test` (debug) PASS in 40 s (the README check 5-6 ms of it); the seed sweep 256 seeds PASS (266 s); the bench soak
PASS (22 s; settled 9,196 KB after the first block, 14,136 KB at most in the first half, 15,484 KB after the last); the
leak check 28 scenarios, 0 LEAK (142 s; STEP: config-reload-cycle/color and external-change-cycle private commit); the exit
check PASS. No run reached the guard.

The debug soak's verdict: "directory watches 3 | 3 | 4 | 4 | 4 | 4 | LEAK" and the handle ledger with it, i.e. one more
watch from about event 600,000 to the end. It is not this phase's code:

- The soak deletes only the top-level files of build\tmp\soak. Its git\ folder keeps what random write-files put there,
  and the bench soak, which verify.bat phase runs just before, leaves about 150 such files; the debug soak with the same
  seed then opens a buffer in git\ late in the run and keeps it (12 buffers at the end, the exit check clean: live state).
- Phase 21's code (b099c97, built in a scratch copy) does exactly the same on the same sequence: from an empty
  build\tmp\soak its bench soak PASS (3 watches throughout), then its debug soak exit 6 on the same 3 -> 4 row. On the
  folder as the first run left it, Phase 21's debug soak alone: exit 6 as well.
- From an empty folder, the debug soak alone passes the watch rule, but its settled-memory rule (+2 MB over the first
  half's highest) sits at the edge: Phase 22's code exit 9 / 0 / 0 (16,208, 14,760, 15,532 KB at the end against 13,472,
  13,408, 13,736), Phase 21's 0 / 0 / 9 (15,832, 16,040, 16,992 against 14,904, 14,168, 14,380).

The user decided: one fix in this phase, d1691cc, the soak empties build\tmp\soak, subfolders included, at its start; the
memory rule unchanged here. Proof before the rerun, bench then debug on the old leftovers (168 files in git\): both PASS,
3 watches throughout, and each refused as many writes as on an empty folder (818 bench, 946 debug: the same events).

The rerun at d1691cc, 520 s: FAIL, only `soak-bench` (exit 9, the memory rule). The debug soak PASS (3 watches throughout;
settled 9,544 / 13,776 / 15,372 KB); `--test` (debug) PASS in 38.4 s (the README check 5 ms); the seed sweep PASS (243 s);
the leak check 28 scenarios, 0 LEAK (140 s; STEP: config-reload-cycle/color and theme-cycle private commit); the exit check
PASS. As the user asked, not rerun: the bench soak's memory, by owner, at each checkpoint (KB; "settled" without the undo
logs, "arenas" ours without the undo logs, from the soak's "settled" and "by owner" lines):

| events | settled | arenas | process heaps | thread stacks (worker slots) | other | undo logs |
|---|---|---|---|---|---|---|
| 100,000 | 9,304 | 5,308 | 700 | 812 (7) | 112 | 956 |
| 200,000 | 11,149 | 5,421 | 1,044 | 900 (8) | 120 | 1,307 |
| 300,000 | 11,477 | 5,741 | 1,048 | 900 (8) | 120 | 1,935 |
| 400,000 | 13,029 | 5,989 | 1,384 | 1,780 (18) | 200 | 2,355 |
| 500,000 | 14,777 | 6,893 | 1,596 | 2,308 (24) | 248 | 2,727 |
| 600,000 | 14,613 | 6,545 | 1,656 | 2,472 (26) | 264 | 2,975 |
| 700,000 | 16,333 | 6,493 | 3,436 | 2,472 (26) | 264 | 2,607 |
| 800,000 | 16,473 | 6,493 | 3,568 | 2,472 (26) | 264 | 3,151 |
| 900,000 | 17,209 | 6,493 | 3,724 | 2,976 (32) | 312 | 3,563 |
| 1,000,000 | 17,561 | 6,841 | 3,724 | 2,976 (32) | 312 | 4,299 |

(Image pages 1,116 and mapped views 68 throughout.) The limit was 14,777 + 2,048 = 16,825 KB; the first checkpoint over it
is 900,000. From the first half's highest (500,000) to the end, +2,784 KB: process heaps +2,128 (1,780 of it between
600,000 and 700,000), thread stacks +668 (worker slots 24 -> 32: parked threads keep their stacks, bounded by the cap of
256, Phase 16's Later item), other +64; our arenas without the undo logs -52. Nothing of it is ronin's arenas: ronin
allocates no heap itself, so the heaps are the system's (process creation for the fake git and terminal, directory
listings, the worker threads' own). The soak stopped at the memory check, so no leak-metric table was logged. The same
build and seed passed the bench soak in the proof run just before (and 14,136 -> 15,484 KB in this phase's first run):
which run crosses depends on the heaps and the slots, not on the events, which are the same.

The user decided the rule (Decisions, Phase 22, 8): 3e2f1ed judges our arenas without the undo logs, the rest shown. The
final run at 3e2f1ed, 502 s: PASS, every step. `--test` (debug) PASS in 37.7 s (the README check 5 ms); the seed sweep 256
seeds PASS (227 s); the bench soak PASS in 18.5 s and the debug soak PASS in 22.5 s, both with our arenas without the
undo logs at 5,308 KB after the first block, 6,056 KB at most in the first half and 6,336 KB after the last (+280 KB;
settled in all, informational: 13,392 -> 14,572 and 13,680 -> 15,048 KB), 3 directory watches throughout, the only WATCH
row the working set; the leak check 28 scenarios, 0 LEAK (147 s; WATCH git-cycle/browse working set, STEP
config-reload-cycle/color private commit); the exit check PASS. No run reached the guard.

## Roadmap

- [x] 1. Skeleton, window, D3D11, rect renderer
- [x] 2. Font: DirectWrite-rasterized glyph atlas, text drawing
- [x] 3. Text buffer: gap buffer, line index, UTF-8, file load/save
- [x] 4. View: cursors (stored as an array from day one), scrolling, layout
- [x] 5. Commands, keymap with prefix keys, config file, hot reload
- [x] 6. Editing: mark/region, kill ring, undo/redo, auto-indent
- [x] 7. Minibuffer, prompts, file and buffer commands (goto-line and buffer switching from 9)
- [x] 8. Lexers and incremental highlighting; token-aware indentation: bracket matching on tokens
  replaces bracket counting. A line that starts with a closer takes the indentation of the line
  holding its matching opener; a line after one that leaves any bracket open gets one level more,
  however many it opened (fixes `f((x) => {` giving two levels).
- [x] 9. isearch and query-replace
- [x] 10. Window splitting
- [x] 11. Project + fuzzy file open
- [x] 12. Project-wide search
- [x] 13. Build + jump to error
- [x] 14. Multiple cursors
- [x] 15. Performance and size pass (including an optional CRT-free build)
- [x] 16. Memory and resource leak audit
- [x] 17. Repository and build-folder cleanup
- [x] 18. Modal overlay and the command list
- [x] 19. The git changes modal
- [x] 20. Terminal and AI terminal shortcuts
- [x] 21. Themes: built-in themes, theme files, set-theme
- [ ] 22. README, contribution guide and repository files

## Decisions

- Swap chain B8G8R8A8_UNORM, not sRGB: theme colors reach the screen bit-exact.
- Flip model (FLIP_DISCARD, 2 buffers, SCALING_NONE), max frame latency 1, vsync Present.
- First frame is presented while the window is DWM-cloaked, then uncloaked.
- `/external:anglebrackets /external:W0`: SDK headers do not break our /W4 /WX.
- No ronin identifier may be a name a system header defines as a macro: build.bat compiles
  `src/check_macros.c` (every system header we use, then the whole unity build) with /Zs in every
  configuration, so such a collision, or a ronin macro redefining a system one, fails the build where
  /external:W0 would have hidden it. Fixes are renames (KEYMOD_*, never #undef).
- Import list kept minimal for startup: kernel32, user32 and gdi32 since Phase 15 (res/imports.txt,
  checked by verify.bat); d3d11 (on the device thread), dwmapi (the first window attribute) and dwrite
  (the first font) are loaded with LoadLibraryW where first used; dxgi only in dev builds; dxguid.lib is
  static. No shell32 (own command-line parser), no shcore.
- No key events for lone modifier keys (Ctrl, Shift, Alt, Win, Caps Lock). Numpad: text events.
- Mouse moves are queued but do not request a redraw by themselves.
- Debug layer: break on CORRUPTION/ERROR only when a debugger is attached (otherwise a
  break kills the process and loses the message); messages are always logged and counted.
- Back-buffer capture (smoke, screenshot) is copied before Present: FLIP_DISCARD leaves the
  back buffer undefined afterwards.
- Monospace cell grid only. Every codepoint occupies exactly one cell, except in the buffer
  view: a tab advances to the next tab stop and an ASCII control character takes two cells
  (caret notation, Phase 4). Column to x is a multiplication. cell_w and line_h are integers in physical pixels; every glyph quad lands
  on integer pixel coordinates.
- No shaping, no ligatures, no kerning: one codepoint -> one glyph. Combining marks and
  complex scripts are out of scope.
- One font face (the font setting, default Consolas; a missing family falls back to Consolas,
  then Courier New, with a message), regular weight, font_size points. No bold or italic.
- Missing glyph -> hollow box (drawn by font.c, one atlas entry shared by all missing
  codepoints and by invalid UTF-8 bytes).
- ClearType via dual-source blending; glyphs are rasterized white on black and their R, G, B
  are per-channel coverage. Blending happens in gamma space with the coverage DirectWrite
  produced for white on black (its gamma / contrast from the monitor's rendering params).
- dwrite.h is C++ only (verified: `interface X : public IUnknown`, no C vtables), hence the
  single C-style C++ file.
- Rendering mode is a parameter (GDI_CLASSIC, NATURAL, NATURAL_SYMMETRIC; default
  NATURAL_SYMMETRIC). Metrics use the matching measuring mode.
- DIB row order is detected at runtime (SetPixelV at (0, 0) and look at the first row):
  GetObject reported a positive biHeight for the top-down DIB of the bitmap render target.
- DirectWrite leak check counts our own references (`FontBackend.live_refs`), because the
  shared factory and its font cache keep internal references.
- Glyph hash load factor capped at 70%: missing codepoints take slots without taking atlas
  space; exceeding the cap takes the "atlas full" reset path, so probing always terminates.
- D3D11CreateDevice runs on a worker thread started at the top of wWinMain; the device stays
  SINGLETHREADED and is only used after the join.
- Line height is the font's natural height times the line_height setting (percent). The render
  mode is the render_mode setting (default NATURAL_SYMMETRIC).

### Buffers (Phase 3)

- A buffer is raw bytes, treated as UTF-8. Loading and saving never lose or alter bytes:
  invalid UTF-8 and NUL bytes stay in the buffer, draw as the missing-glyph box, and are
  written back unchanged. Control bytes other than tab draw in caret notation (Phase 4), so
  the CRs of a mixed-ending file are visible as ^M.
- Storage is a gap buffer. Positions are byte offsets (i64) into the logical text and always
  sit on a codepoint boundary (an invalid byte counts as one unit).
- Every modification goes through one function, `buffer_replace(buf, start, end, text)`.
  Insert is an empty range, delete is empty text. It maintains the gap, the line index, the
  modified flag and an edit counter. Undo (6), highlighting (8) and cursor fix-ups (4, 14)
  hook in there; nothing else writes buffer memory (except loading, which fills a fresh
  buffer before anyone sees it).
- A read-only buffer rejects `buffer_replace` unless its `inhibit_read_only` flag is set
  (Emacs' inhibit-read-only): program-written buffers such as build output still go through
  the single entry point.
- Each buffer owns three reservations and never reallocates: a 1 MB meta arena (the Buffer
  struct, path, name), 2 GB of text and 8 GB of line index (address space only; the worst case
  is one newline per byte). Files over 1 GB are refused with a message. An edit that would
  exceed capacity, or whose commit fails, is rejected and leaves the buffer unchanged.
- Line index: the positions of the '\n' bytes as u32 (capacity < 2^31), in an array with its
  own gap paired with the text gap. Entries before the gap are absolute positions, entries
  after it are distances from the end of the text, so an edit at the gap only touches the
  entries of the newlines it inserts or deletes; moving the text gap converts the entries it
  passes. Never rebuilt by rescanning. Line count = newlines + 1 (Emacs).
- Line endings: a file that is entirely CRLF is stored with LF and written back as CRLF; a
  file that is entirely LF stays LF; a file with mixed endings is stored and saved byte for
  byte, CRs left in the text. A file without newlines and a new buffer are LF. Loading strips
  CRs optimistically in the same pass that builds the line index (SSE2 scan for '\n'); the
  first bare LF after stripping started restores the processed prefix in one backward pass.
- Encodings: UTF-8 with or without BOM (BOM stripped, restored on save). UTF-16 LE / BE with
  BOM (and an even length) is converted to UTF-8 on load and written back in its encoding;
  unpaired surrogates are kept as 3-byte WTF-8 so the round trip stays exact. Everything else
  is raw bytes.
- Saving writes a temp file `<name>.ronin~N` in the same directory, streaming through a fixed
  64 KB chunk when converting (no second copy of the text), flushes it (FlushFileBuffers), then
  swaps it in with ReplaceFileW (attributes, ACLs, creation time preserved; MoveFileExW when
  the target does not exist). It writes in place instead (also flushed) when the target is a
  symlink or has several hard links, when the temp file cannot be created, or when
  ReplaceFileW fails. A read-only target is refused before anything is written.
- Paths are stored as full, normalized UTF-8 paths (GetFullPathNameW). Opening a path that
  does not exist gives an empty buffer visiting it, "(New file)", as in Emacs.
- Every file failure returns an `OsFileStatus` the caller turns into a minibuffer message.
- `RONIN_D3D_DEBUG` (defaults to RONIN_DEV) controls the D3D11 debug layer, so `build.bat bench`
  is an optimized dev build without it.

### Views, cursors, markers (Phase 4)

- A View is an Emacs window: one buffer, an array of cursors, a scroll position (a top-of-window
  marker and a left column) and a pixel rect. Layout hands each View its rect (the window tree,
  Phase 10), each with its own mode line; nothing assumes a single View.
- Cursors are an array from the start. Every motion and edit command is written for a single
  cursor (its context carries the View and that Cursor). `view_run_command` is the only place
  that loops over cursors, and it does the work after the loop (keep point visible, update
  `last_command`). Commands that act on the View as a whole (scroll-up-command,
  scroll-down-command, recenter-top-bottom, save-buffer) are flagged COMMAND_ONCE and run once
  with the primary cursor. Phase 14 added cursor creation, merging and the ordered edit pass to that
  driver only (see "Multiple cursors (Phase 14)").
- Markers: every position that must survive edits (each cursor's point and mark, each View's
  top of window) is a marker registered with its buffer, and `buffer_replace` is the only
  thing that adjusts positions. For a replace of [start, end) by len bytes: a marker before
  start stays; after end it shifts; exactly at end (of a non-empty range) it goes to
  start + len; inside the range, or at start, it goes to start, or to start + len if it is an
  advancing marker (insertion type: point markers advance, the top-of-window marker does not).
  Markers within 3 bytes of the edit are then snapped back to a character boundary, since
  joined invalid bytes can form a valid sequence. Storage: a flat slot array with a free list
  in its own reservation, one linear pass per edit over the live markers (a dense list of the live
  slots since Phase 14; inside a batch the pass waits for the batch's end).
- Point is always visible, as in Emacs: scrolling drags point along (to the start of the
  nearest visible line), and moving point off screen recenters the window on it. The top line
  may scroll as far as the last line of the buffer. Visibility follows the primary cursor.
- No line wrapping in v1: long lines are truncated and the window scrolls horizontally to
  center point when its column leaves the visible range, back to column 0 when it fits.
- No cursor blink, no smooth scrolling, no line numbers, no scroll bars: nothing that needs a
  timer.
- Visual columns: a tab advances to the next multiple of the buffer's tab_width; an ASCII
  control character (0x00-0x1F except tab and newline, and 0x7F) is 2 cells, drawn as ^@, ^M,
  ^? in the number/constant color; anything else is 1. Invalid bytes and missing glyphs keep
  the box. (line, column) -> offset picks the nearest character boundary (the start of a
  character that covers the column when the column is in its first half).
- A word is a run of letters and digits; underscore is not a word character (Emacs' default in
  C mode) unless underscore_is_word is set; every byte >= 0x80 counts as a letter, so word
  scanning is bytewise and always stops on a character boundary.
- View logic (positions, motions, scrolling, commands) lives in view.c and runs headless in
  --test; drawing is in app.c.
- Commands take one context argument and nothing else; they are registered in one table
  (Phase 5).
- Messages go to the echo area through `echo_message`; a message stays until the next key
  event. Hitting a limit shows a message and never makes a sound.
- Cursor: a filled block with the character under it in the background color while the window
  has keyboard focus (and the View is active); a hollow box otherwise. One cell wide at the end
  of a line and on a tab, two on a control character.
- Command line: `ronin [+LINE[:COLUMN]] [file]`, LINE and COLUMN 1-based as in Emacs (COLUMN is a
  visual column); the mode line shows the column 0-based. Any unrecognized argument starting
  with "--" exits immediately with code 10, before a window exists.
- The window title is "<buffer name> - ronin", set only when it changes.
- Motions follow Emacs: next-line on the last line goes to the end of the buffer ("End of
  buffer"), previous-line on the first to the beginning; C-f / C-b / C-d / DEL at a limit show
  the message and do nothing; word and paragraph motions stop silently. A paragraph separator
  is a line of spaces and tabs; backward-paragraph at the start of a line right after an empty
  line stops on that line (Emacs' special case). scroll-up/down-command move by the window
  height minus 2 and refuse at the limits with a message; recenter-top-bottom cycles center,
  top, bottom; end-of-buffer does Emacs' (recenter -3) when the end is not visible.
- Mode line: ` -:**-  name  NN%  Lline Ccol  (Language)  encoding eol`, the name padded to 12
  cells (Emacs %12b), position Top / Bot / All / NN% (of the top of the window), line 1-based,
  column 0-based. Text areas have a 4 px (DPI-scaled) left padding.
- The mouse: a left click activates the view and sets point; the wheel scrolls the view under
  the mouse, 3 lines per notch.

### Commands, keys, config (Phase 5)

- Everything the user can do is a named command in one table (command.c), with Emacs names.
  Keys only ever map to command names. App-level commands get the App through the context.
- Keys match by the character they produce, as in Emacs, not by physical position: C-/ is Ctrl
  plus whatever produces "/" in the current layout. Letters keep Shift as a modifier (C-S-a);
  every other character absorbs Shift and AltGr (C-/ where "/" is Shift+7 is still C-/). A
  modified uppercase letter in the config means S- plus the letter; S- with a non-letter
  character is rejected (it can never match). Letter case: a small table (ASCII, Latin-1,
  Latin Extended-A, Greek, Cyrillic) with the default Unicode mapping, so on Turkish Q
  Ctrl+Shift+ı is C-S-i.
- A chord is modifiers (C, M, S) plus a named key or a character, packed in a u32; a key
  sequence is 1 to 4 chords. kbd notation as Emacs, with <prior> / <next> canonical and
  <pageup> / <pagedown> (and <return> <tab> <escape> <backspace>) accepted.
- A character key without Ctrl or Alt is not a chord: its text event (after dead keys have
  composed) is. So plain characters can be bound too (C-x o), and an unbound plain character
  outside a prefix runs self-insert-command. When a KEY_DOWN is consumed, the text events it
  produced are dropped (up to the next KEY_DOWN).
- Keymaps are named and searched as a stack, context maps first, then "global" (only global in
  Phase 5). Prefixes merge across maps as in Emacs: the first map with an exact match runs it;
  otherwise the sequence waits if it is a proper prefix in any map; otherwise it is undefined.
  A keymap is a flat array searched linearly (a few dozen bindings, well under a microsecond).
- The sequence state machine: an exact match runs the command; a proper prefix waits and shows
  "C-x-" in the echo area at once; no match shows "<sequence> is undefined" and resets;
  keyboard-quit (C-g, ESC) cancels a pending prefix with "Quit". A chord with Shift and no
  binding is looked up again without Shift and the context records shift_translated (Emacs'
  shift-translation, for shift-select in Phase 6). describe-key describes the next complete
  sequence instead of running it. No allocation per keystroke.
- Escape is bound to keyboard-quit by default. This is the one deliberate deviation from
  Emacs, where ESC is the Meta prefix.
- One config file in plain text holds settings, colors and keys: sections [settings],
  [colors], [keys]; `name = value`; in [keys] the last word is the command and everything
  before it the key sequence (so "=" needs no escaping); "none" removes a binding. A line
  starting with '#' is a comment (only there: colors contain '#'), so a lone "#" key cannot be
  bound. The built-in defaults are the same format, embedded as a C string
  (config_default.h, fully commented) and parsed by the same code at startup; the user's file
  is applied on top. Dev builds assert that the defaults parse without diagnostics.
- Location: ronin.conf next to the exe if it exists (portable), otherwise
  %APPDATA%\ronin\ronin.conf (APPDATA from the environment, no shell32). Startup never creates a
  file; open-config creates it from the defaults.
- A config error never blocks startup or a reload: bad lines are skipped and reported with file
  and line number; out-of-range values are clamped with a warning. A binding removes the older
  bindings it conflicts with (one is a proper prefix of the other) with a warning naming how
  many. Every diagnostic goes to *Messages* (at most 1000 kept per load); the echo area shows
  the first error and "(and N more)".
- The config is read before the font, so the font is set up exactly once at startup with the
  configured face, size, render mode and line height. A load parses into the second of two
  arenas and switches to it, so a config is never half applied.
- Settings: font, font_size (4-96 pt), line_height (80-300 %), render_mode, tab_width (1-16,
  stored per buffer as Emacs' buffer-local tab-width), underscore_is_word, fsync_on_save.
  Colors: the ten theme roles; the Windows 11 caption color follows background.
- Hot reload is driven by a directory change notification (FindFirstChangeNotification) waited
  on with the messages; no polling and no timers. On a notification the file is read again only
  if its size or write time changed; a file missing for a moment (saved through a temporary
  file and a rename) counts as unchanged. A sharing violation is retried: the app asks the
  platform for a 100 ms wait timeout only while a retry is pending, at most 5 reads, then
  reports the failure; otherwise the wait is infinite. A file that cannot be read leaves the
  config as it was. Everything applies live: colors, keys, font and size, render mode, line
  height, tab width, the other settings.
- The app keeps a list of buffers; visiting a path that is already open switches to it (full
  paths compared case-insensitively for ASCII). Each entry keeps markers for where its buffer was
  last shown, so switching a view away and back restores point and scroll; a view shows one
  cursor after a switch.
- *Messages*: every echo-area message is also appended to this read-only buffer through the
  inhibit_read_only path, keeping the last 1000 lines. Key prefixes and the describe-key prompt
  are shown, not logged. *scratch* always exists (Phase 6), as in Emacs; the list is the file,
  *scratch*, *Messages*, and next-buffer keeps *Messages* in the cycle.
- A config change notification is read 50 ms after the last one (Phase 6), through the same
  pending-wait mechanism as the retries, so an editor that truncates and then writes the file
  is read once, full.
- save-buffers-kill-terminal refused once while file-visiting buffers were modified and quit on
  an immediate repeat (replaced by the prompts of Phase 7); the window's close button and Alt+F4
  run it too. Text scale: 1.2 per step for the session (never written to
  the config); Ctrl + wheel does the same; other mouse input stays hard-coded.

### Editing: mark and region, kill ring, undo, indentation (Phase 6)

- Undo is logged inside buffer_replace, the single entry point: a record {start, removed,
  inserted} and the removed bytes only (undoing an insertion needs just its length), in a
  reserved range per buffer. Program buffers (*Messages*) have undo disabled. Loading a file
  records nothing.
- Undo follows Emacs: undo is itself undoable (an undo applies the inverse of a group through
  buffer_replace and is logged as a group). Consecutive undo commands walk back through history
  (a pending group); any other command ends the run, after which undo first undoes the undos.
  undo-redo is the direct way forward: it undoes the most recent group that moved back in
  history (groups record their direction) and led to the current state.
- The command driver sets undo boundaries: one command, applied to all cursors, is one undo
  group. Consecutive self-insert-command calls are merged into groups of up to 20 characters,
  and so are consecutive single-character deletes, as in Emacs. After undo, point goes to where
  the undone command started and the mark is deactivated.
- A buffer is unmodified exactly when its undo position equals the position at the last save:
  every logged edit yields a state id, an undo group returns to the state before the group it
  reverts, and modified = (state != saved state). Undoing (or redoing) back to the saved
  state clears the modified flag.
- undo_limit_mb bounds the log: the oldest groups are dropped first (down to 3/4 of the limit,
  so the memmove is rare) and the commit beyond what is kept is given back; the most recent
  command always stays undoable, even alone over the limit.
- Everything the driver does is driven by command flags; no command implements these rules:
  COMMAND_MOTION (shift-select), COMMAND_EDIT (deactivates the mark afterwards), COMMAND_KILL /
  COMMAND_KILL_BACKWARD (a kill directly after a kill appends to the same entry, a backward kill
  prepends; with several cursors one piece per cursor, Phase 14), COMMAND_MERGE_INSERT / MERGE_DELETE (undo
  merging), COMMAND_REGION_DELETE (DEL and C-d delete an active region, Emacs'
  delete-active-region) and COMMAND_REGION_REPLACE (typing, RET and yank replace an active
  region with delete_selection_mode).
- Transient mark mode is on by default: the region is highlighted while the mark is active; the
  selection color is drawn behind the cells, to the window edge on lines whose newline is
  selected, and text keeps its color. Region commands use the mark whenever it is set (Emacs'
  mark-even-if-inactive). Shift-select uses the shift-translation of Phase 5: a shifted motion
  activates the mark at point first (an active region extends), an unshifted motion ends a
  region that shift started. C-SPC C-SPC deactivates the mark again.
- Typing does not replace an active region by default (Emacs); delete_selection_mode is a
  setting.
- The mouse: a press puts point at the cell and deactivates the region, a drag selects from
  there (captured; rows and columns clamped to the text area outside it), a double click
  selects the word (or a run of blanks, or one other character), a triple click the line with
  its newline. The platform counts clicks with the system's double-click time and area.
- The kill ring and the Windows clipboard are linked, like Emacs' select-enable-clipboard:
  every kill command also puts its entry on the clipboard (once per command), and yank takes
  the clipboard when its sequence number changed since our last kill
  (GetClipboardSequenceNumber; no clipboard listener). A kill in a read-only buffer copies the
  text and says the buffer is read-only. kill-line kills through the newline when only blanks
  are left (Emacs).
- Kill ring storage: small entries share one arena (dead bytes compacted away when they pass
  half of it); an entry over 1 MB has its own reservation with a geometrically growing commit.
  A kill is copied once into its entry, from the buffer; appending grows the newest entry in
  place, so a large entry is never copied again.
- Clipboard text is CRLF by convention: LF -> CRLF when copying (in one pass, straight into the
  clipboard's memory), CRLF and lone CR -> LF when pasting. Busy clipboards are retried 5 times.
  Tests, the smoke, the benches and screenshot runs use an in-memory fake.
- Indentation is rule based, with no parser: a line's indentation is the indentation of the
  previous non-blank line, plus that line's net bracket balance (its ( [ { minus its ) ] }, not
  counting the closing brackets that start it, which were applied to its own indentation),
  minus one level per closing bracket that starts the current line; never below column 0.
  Brackets inside strings and comments are counted naively for now; Phase 8 makes this
  token-aware. Indentation is rewritten only when it differs (no no-op undo records).
- RET indents the new line and empties a whitespace-only line it leaves; TAB reindents the line
  (point keeps its place in the text, from the indentation it goes to the text) or every line
  of an active region (blank lines emptied); a closing bracket typed first on a line reindents
  it in the same undo group. indent_with_tabs, or per buffer what detect_indentation found in
  the file's first indented lines (tab_width and the tab choice are buffer-local, Emacs).
- comment-line uses "//" for every language; it toggles on the line or the region's lines at
  their smallest indentation and, without a region, moves to the next line (Emacs).
- <backtab> is Emacs' name for S-TAB in the kbd notation (and how it prints).

### Minibuffer, prompts, files and buffers (Phase 7)

- The minibuffer is a real one-line View on its own Buffer. All editing commands, the kill ring,
  undo, region and mouse work in it unchanged. While it is active the keymap stack is
  [minibuffer, global].
- Prompts never block. A command asks for input and returns; a continuation runs with the result
  when the user accepts. Multi-step prompts chain continuations. No nested event loop.
- No recursive minibuffers. A command that needs the minibuffer while it is active reports
  "Command attempted to use minibuffer while in minibuffer".
- Completion is a vertical candidate list above the minibuffer line (like Emacs' fido-vertical),
  filtered as the user types. No *Completions* buffer.
- Matching: case-insensitive; space-separated terms must all match as substrings; prefix matches
  rank before other matches; an exact match ranks first. The matcher sits behind a small
  interface: Phase 11 replaces it with fuzzy scoring for project files.
- Opening and closing the minibuffer never changes the scroll position of any View.
- Paths are shown with forward slashes, as Emacs does on Windows. Both separators are accepted as
  input. "~" means the user's profile directory.
- Files changed outside the editor: an unmodified buffer is reloaded silently; a modified buffer is
  never touched, only flagged.
- The prompt is not part of the minibuffer's text: it is drawn before the input and the View's rect
  starts after it, so M-< and C-a go to the start of the input. The buffer is " *Minibuf-1*", not
  listed; its undo log starts empty with every prompt (the initial input is not undoable).
- Kinds: text, choice (candidates from a callback that may rebuild them for the input: find-file
  lists the directory part only when it changes), number (N or N:M), single key (its keys bypass the
  keymap, except that a key bound to a COMMAND_QUIT command aborts first, so no answer can shadow C-g
  or ESC; other keys say "[Please answer y or n]"), yes-or-no typed in full.
- The command driver runs an accepted prompt's continuation after the command, as a command of its
  own on the calling View (its undo boundary, ensure_visible, last_command); M-x runs the chosen
  command itself. A command that starts a chain sets the chain's state; a continuation may open the
  next prompt. Abort (C-g, ESC, the close button, the end of the session) drops the whole chain: what
  earlier links did stays done (files saved stay saved), nothing later runs, an exit is cancelled.
- C-g inside the minibuffer with a key prefix pending cancels only the prefix (COMMAND_QUIT on
  keyboard-quit and abort-minibuffers lets either cancel a prefix in any keymap).
- While the minibuffer reads, messages are shown as bracketed notes after the input (Emacs 27+):
  "[No match]", "[Sole completion]", "[Complete, but not unique]", and any command's message.
  Notes are not logged.
- The candidate list covers the bottom of the views: they are drawn shorter with their mode line
  moved up, their rows and scroll positions untouched, so lines near the bottom are hidden, not
  scrolled away. The first match is selected after every change of the input. TAB completes to the
  longest common prefix of the matches when the input is a prefix of it, then to the selection.
  Clicks outside the minibuffer line are ignored while it reads; buffer-switching commands refuse
  to run in the minibuffer window.
- Case folding keeps byte offsets (a character whose folded form has another UTF-8 length is kept),
  so match spans are positions in the shown text. While the input only grows on the same candidate
  set, only the previous matches are scored (narrowing).
- History per category (commands, files, buffers, lines, text), 100 entries, no consecutive
  duplicates, session only; dead strings are compacted when they pass half of the arena.
- M-x lists every command alphabetically with its shortest global binding. find-file and
  write-file start in the current buffer's directory (no file: the current directory), with
  directories first (in the file system's order, then the files); RET or TAB on a directory
  descends; "/~/" or "/X:/" in the input starts over there (not dimmed). RET on an empty file name
  takes the selected entry; C-j on a directory says it is a directory (no dired).
- Buffer names are unique: a newcomer whose name is taken gets "name<parent dir>", then "name<N>"
  (Emacs' uniquify renames both; ronin only the newcomer). switch-to-buffer offers buffers most
  recently shown first with the default (the most recently shown other buffer) selected; killing a
  buffer shows that one in its views; *scratch* is recreated at once; *Messages* cannot be killed.
- Revert goes through buffer_replace: the file is loaded into a temporary buffer, the common prefix
  and suffix are skipped (edges moved out of UTF-8 sequences) and the middle is replaced once, as an
  undo group of its own. Markers inside the replaced range, or exactly at its start, are put back
  by line and character column, clamped (Phase 8: an outside tool rewriting most of a file sent
  point to the end); markers outside it adjust as for any edit. The undo log is kept, so an outside change can be
  undone (the buffer is then modified) and undo-redo returns to the file. Encoding and line endings
  are taken over; a change of line endings only is no edit and no undo step.
- Changed on disk is decided by size and write time (as Emacs): a rewrite within the same file-time
  tick at the same size is not seen. Checks: every file buffer when the window is activated; the
  displayed buffers after a watch notification while focused (50 ms settle, sharing violations
  retried every 100 ms, 5 times). One watch per distinct directory of the displayed buffers. Saving
  a buffer whose file changed asks "<name> has changed since visited or saved. Save anyway?"; a
  deleted file is reported once and the buffer stays; the mode line shows "[changed on disk]" or
  "[deleted on disk]".
- The end of the Windows session: the app keeps the platform told whether some file buffer is
  unsaved; WM_QUERYENDSESSION is allowed at once when nothing is, otherwise refused with a shutdown
  block reason while the quit chain asks (a repeated query does not restart it). The reason goes
  away when nothing is unsaved, on "exit anyway", or when Windows cancels the shutdown.
  WM_ENDSESSION(TRUE) releases and exits without writing anything.

### Highlighting, token-aware indentation (Phase 8)

- Hand-written lexers, one per language family (C/C++ share one, as do JavaScript/TypeScript). No
  regex engine, no parser, no grammar. Keyword lookup is an open-addressing table filled once at
  startup; it allocates nothing.
- Highlighting is line based: each line has a u32 lexer state for its start; tokens are never
  stored, visible lines are lexed when drawn. Where a delimiter cannot fit in the state (a C++ raw
  string delimiter, a Jai here-string terminator) the state holds a hash of it; JS templates and C#
  interpolations keep a small stack of contexts (the innermost 5 / 3 carried across lines).
- The states live in the buffer, in an array paired with the newline index (same gap, same
  indexes), committed only for languages with a lexer. buffer_replace keeps them with their lines
  and tracks state_valid, state_dirty and state_known; buffer.c never interprets a state.
- Incremental and lazy: catch-up lexes from the first untrusted line and stops when a line's new
  start state equals the stored one after the last changed line (and after the last line written by
  an interrupted pass); only as far as some view needs; at most ~2 ms per frame (the clock read every
  64 lines or 16 KB); while catching up, lines are drawn with their previous states (plain if none)
  and another frame is requested; nothing runs when nothing is pending.
- Beyond 20,000 bytes into a line text is drawn plain; the line's end state is still lexed whole
  (a very long line costs its full lex on each edit).
- Not highlighted by choice: calls, ordinary identifiers, JSX. C/C++ function names: the identifier
  directly before the first '(' on a line that starts in column 0 with a word (not a keyword; a
  lone word ending in ';' is a call). C/C++ preprocessor lines: only the directive word is a
  directive; the name of #define is a function.
- Indentation on tokens (edit.h has the rules): brackets match on tokens, bounded to 256 KB;
  closers return to their opener's line; one level after a line leaving brackets open; brace-less
  bodies, else binding, case labels; lines inside multi-line comments and strings and C/C++
  preprocessor lines are left alone by TAB. Indentation brings states up to its line first (250 us).
  Electric labels (Phase 9): see below.
- show_paren_mode: the closer before point wins, else the opener at point; same matcher; cached per
  view.

### Search, isearch, query-replace (Phase 9)

- One search engine in the core (search.c), used by isearch and query-replace now and by
  project-wide search in Phase 12. Literal text only; regular expressions are in Later.
- The engine works on the gap buffer's two segments without copying the text (only a candidate that
  straddles the gap is copied, into a window of at most the needle's length), finds matches across
  the gap, searches forward (the smallest start >= F) and backward (the largest start whose match
  ends by B) inside a range, and is resumable: it examines at most a budget of start positions per
  call. The app slices it by time: 256 KB of positions a slice, slices until the frame's 2 ms
  deadline (search before lexer states), so a frame overruns by at most one slice and a keystroke
  never waits for a search on a huge buffer. An edit between slices starts it over. No allocation.
- Fast path: the needle's first byte, 16 bytes at a time (SSE2): one compare exact, (b | 0x20) for a
  folded ASCII letter, the lead bytes of a folded non-ASCII character and of its uppercase form.
  Every hit is verified. --test checks for every character below U+3000 that the lead bytes cover
  everything that folds to it.
- Case: smart, as in Emacs. A search string without uppercase letters matches case-insensitively;
  one with any uppercase letter matches exactly. M-c toggles for the rest of the search. Folding is
  the matcher's: a character becomes its lowercase form only when that has the same UTF-8 length, so
  a match is always as long as the needle.
- isearch is a mode with its own keymap on the keymap stack ([keys isearch], searched before the
  global one), not a minibuffer prompt. Text typed while it is active extends the search string. A
  key bound to any other command ends the search at the current match and then runs that command
  (a prefix that only the global keymap has, such as C-x, ends it at once so "C-x-" shows), as in
  Emacs; so do clicks and the close button. Commands that run inside it carry COMMAND_ISEARCH.
- isearch's state is a stack of steps (a typed character, a repeat, a direction change, a yank, a
  history entry, a case switch). A step is resolved from the step below it, by a search or at once;
  steps typed while one is being searched wait their turn; DEL pops one (cancelling its search) and
  restores the step below's match and point. Emacs' semantics: extending searches from the current
  match's start (backward: from its end), repeats do not overlap the current match, a direction
  change keeps the match and moves point to its other end (Emacs 28's default), repeating a failing
  search wraps ("Wrapped", then "Overwrapped" past the start), C-g first removes the failing (or
  still searched) part, then quits to the start, RET / ESC / C-m end at the match and set the mark
  (inactive) at the start with "Mark saved where search started" when point moved and no region is
  active. The failing part of the string is what follows the newest successful step whose string is
  a prefix of it (Emacs' isearch-fail-pos).
- C-w and C-y lowercase the text they append while the search folds, so a yank never makes the
  search exact (Emacs' search-upper-case = not-yanks). C-s C-s with an empty string reuses the last
  search string; M-p / M-n go around the search history (the minibuffer's history storage, its own
  category; a search ended normally is added, a quit one is not).
- The session's position lives in its steps, not in the cursor. The mouse wheel scrolls during an
  isearch and while query-replace asks, dragging point as always; every session key (a repeat, DEL,
  typing, C-w, an answer, exit) first puts point back on the session's match and the view on it. A
  key that ends the session and runs as a command runs after that.
- Keys typed ahead of a pending search (fast typing, --keys: in the same frame): an answer or an exit
  first gives the pending search one slice, so it is found at once in all but huge buffers; past a
  slice an exit uses the last resolved step and query-replace ignores the answer (keys other than
  C-g are ignored while query-replace searches for its next match; queueing them is in Later).
- Highlighting: in the searched view only, the current match on the isearch background with its text
  in isearch_text; every other match that starts on a drawn row on lazy_highlight, searched within
  each row's on-screen bytes plus the needle's length (the bound line drawing has), so the cost is
  per visible line and never depends on the buffer's size. None while failing. query-replace draws its
  question's match and the other matches in its range the same way.
- query-replace (M-%): two chained minibuffer prompts with their history; an empty first answer repeats
  the last pair (an empty search string without one is refused). From point, or inside the active
  region (then deactivated); the mark (inactive) goes to where it started. Answers bypass the keymap
  (as a single-key prompt): y / SPC, n / DEL, !, . , q / RET, C-g (Quit); any other key ends the
  session ("Replaced N occurrences") and runs. The next search starts after each replacement, so a
  replacement containing the search string never loops. An answer after an outside change (a revert)
  searches for its match again instead of replacing a stale range.
- replace-string (no default key) and "!" replace across frames: "Replacing... N%" in the echo
  area, frames keep being drawn, C-g stops it and keeps what was done ("Replaced N occurrences
  (stopped)"), every other key and click is ignored until it finishes or is stopped, and the view does
  not follow the progress (point goes after the last replacement at the end).
- Case conversion as Emacs' case-replace (replace-match), when the search folded and the replacement
  has no uppercase letter: an all-caps match with a word of two or more letters gets an all-caps
  replacement (so does an all-caps match of one-letter words: "X" -> "BAR", as in Emacs); a match
  whose words all start with a capital gets each word of the replacement capitalized; anything else
  gets the replacement as typed. Words are letters and digits (every byte >= 0x80 a letter).
- A whole query-replace or replace-string session is one undo group: its answers (and the C-g that
  stops replace-all) carry COMMAND_UNDO_CONTINUE, so the driver sets no undo boundary for them, and the
  replacements made between frames are outside any command. Undo restores everything and puts point
  where the session started; undo-redo applies it again. Any other command ends the session first, and
  its own boundary closes the group.
- Electric labels: the character that completes a case or default label (":" in the C family, C# and
  JavaScript / TypeScript, ";" in Jai) reindents its line when only blanks follow it and the line is a
  label by the indentation rule (case or default first on the line), in the same undo group as the
  character, as closing brackets do. RET does not reindent the line it leaves.

#### Known limits

- Case-insensitive search does not pair Turkish dotted and dotless i (İ / i, I / ı): the fold only
  maps characters whose encoded length stays the same (İ -> i would shorten), and the default
  mapping pairs I with i, not with ı.
- While query-replace searches for its next match beyond one slice (huge buffers), keys other than
  C-g are ignored, not queued.

### Windows (Phase 10)

- Windows follow the Emacs model: a binary tree of splits whose leaves are Views, with one selected
  window. The minibuffer stays a single line across the bottom of the frame and is not part of the
  tree. At most 8 windows ("Too many windows"); the tree is a fixed array of 15 nodes in App, and the
  windows' Views live in 8 slots inside App: nothing is allocated when windows come and go. A window
  keeps its node index for its whole life.
- Each window has its own point and scroll position, also when two windows show the same buffer. A
  window remembers its position for every buffer it has shown (Emacs' window-prev-buffers), so
  switching buffers back and forth in one window does not disturb another. A window new to a buffer
  starts where the most recently used other window showing it is, else where the buffer was last
  shown anywhere (the buffer list's entry), else at the start. Marks are per window too (per cursor,
  as before); only the selected window highlights its active region (Emacs'
  highlight-nonselected-windows nil, Phase 11; also while the minibuffer reads from it), the others
  keep theirs unhighlighted.
- Layout: a split gives its first child its ratio of the size in whole cells from the split's own
  origin, clamped by the minimum sizes (4 lines with the mode line, 10 columns: Emacs'
  window-min-height and window-min-width) when they fit, the second child the rest; the leaves tile
  the frame exactly. Only split, resize, drag and balance change a ratio, so resizing the frame keeps
  the proportions and a frame resized and back gives the same rects.
- A window with another window on its right has the vertical divider (window_divider, 1 px at 96
  DPI) in its own last pixels; stacked windows are separated by their mode lines. Nothing draws
  outside its window's rect: each window is drawn with the renderer's clip rectangle (r_set_clip, a
  CPU cut of every quad, glyphs included), its text cut to the text area so a half-visible last
  column is drawn clipped, then its mode line and divider.
- The selected window's mode line is inverse video (also while the minibuffer reads: the caller's, as
  Emacs' mode-line-window-selected-p); the others use mode_line_inactive_background / _text, which
  follow background and text unless a file sets them. The cursor is filled only in the selected
  window with focus and no minibuffer, hollow elsewhere. The paren highlight stays in the selected
  window; isearch and query-replace highlight only their own window.
- Split gives each half of the selected window (Emacs' split-window-below / -right); the new window
  shows the same buffer, point, scroll position and mark (inactive) and the selected window stays
  selected. Deleting a window gives its space to the windows next to it only and selects the most
  recently used window (Emacs 28+). Moving a split line (a drag, enlarge / shrink) also changes only
  the windows on either side of it, never below their minimums; enlarge / shrink take from the
  window after (right, below) first, then the one before. Balance gives equal shares to the members
  of a run of same-direction splits.
- One place decides where a buffer "pops up" for commands that show something in another window
  (`app_display_buffer`): a window other than the selected one already showing it; else, with one
  window, a split (side by side when the frame is at least split_width_threshold columns wide,
  otherwise one above the other; the other way if that is too small; the selected window itself if
  neither fits); else the least recently used other window. C-x 4 f / C-x 4 b select it, C-h e does
  not (and shows the end of *Messages*). Phases 12 and 13 use it for search results and build output.
- In the minibuffer the window commands that act on the window they run in refuse with Emacs'
  messages ("Attempt to split minibuffer window", "Attempt to delete minibuffer or sole ordinary
  window", "Can't expand minibuffer to full frame"); other-window says "Cannot select another window
  from the minibuffer" (Emacs would leave the minibuffer: Later). Resize, balance and C-M-v act on
  the window the minibuffer was called from. A command that deletes the window it runs in (C-x 0,
  C-x 4 0) continues in the newly selected window: the driver keeps point visible in ctx->view.
- Mouse: a click selects the window under the pointer, then sets point; a drag selection stays in
  the window it started in. A press on a divider (within 3 px at 96 DPI) or on a mode line with a
  window below it drags that split line, snapped to cells; the bottom mode lines only select. While
  the minibuffer reads, presses outside its line stay ignored, dividers included. The wheel scrolls
  the window under the pointer without selecting it.
- The resize cursor: on WM_SETCURSOR the platform asks the app (`app_mouse_cursor`, a hit test on the
  last layout: no state change, no event, no frame); the horizontal resize cursor over a draggable
  divider, the vertical one over a draggable mode line, the arrow elsewhere. A layout change under a
  still pointer shows on the next mouse move.
- A frame too small for its windows is split by the ratios alone: rects may be empty, never negative;
  rows and columns stay at least 1 for the commands' arithmetic, the drawing goes by the pixels. A
  window without room for a text row and its mode line is not drawn, and the layout pass leaves its
  scroll position alone, so every window comes back as it was when the frame grows.
- Command line: `ronin [+LINE[:COLUMN]] file ...`: every file is opened (the buffer list is the files,
  *scratch*, *Messages*), each at the +LINE[:COLUMN] before it; the first is shown; with
  startup_windows = 2 the frame starts side by side and the second window shows the second file.
- Type-ahead: keys pressed while no window has the keyboard focus (inside an activation) are posted
  as WM_SYSKEY* without the Alt context bit; every pump turns them back into WM_KEY* before
  TranslateMessage (`win32_translate`; F10 excepted), so they type their characters instead of
  becoming Alt chords.

### Projects and fuzzy file open (Phase 11)

- A project needs no setup. Its root is the nearest directory at or above the buffer's file that holds a
  marker (project_markers: .git, .hg, .svn or a .ronin-project file; a marker may be a file, as .git is in
  a worktree). A marker at a drive or share root does not count, so nothing ever scans a whole drive by
  itself. A directory given on the command line (`ronin <directory>`) is a project too: for the files
  inside it, unless a marker below it is nearer (the search stops at it, so a repository around it does
  not take over), and for buffers without a file (*scratch*). The current directory never becomes a root
  by itself: a terminal usually starts in the profile, and `ronin Documents\a.txt` must not make the profile
  a project. Otherwise: "Not in a project (no .git found; start ronin with a directory, or add a
  .ronin-project file)". Roots are cached per directory (256, reset by project-refresh).
- `ronin <directory>` opens no buffer for the directory (it used to say "is a directory"): *scratch* is
  shown, and *scratch* and C-x C-f start in it. The process's current directory is not changed.
- The file list comes from a worker thread (CLAUDE.md has the threading rules). It walks the root breadth
  first (shallow files first) with its own two arenas, hands records to the main thread in 64 KB batches
  through one locked queue (at most 8 in flight: it waits for one to come back), and wakes the main loop
  (one wake event for all workers; `app_wake` takes the batches without a frame unless the finder shows
  them). The last batch carries the counts. Cancel is an atomic flag checked at every directory entry
  plus the worker's event; the main thread joins a worker when its last batch arrives. A scan lives at the
  start of its own reservation, so a worker that never comes back from the file system touches no freed
  memory: at exit every worker is cancelled, then joined within 1,000 ms in all; one still inside a
  file-system call is left to the process exit (logged in dev builds).
- Reparse points: a directory that is one (symlink, junction) is not entered; a file that is one (a cloud
  placeholder) is listed. Depth is capped at 256, a relative path at 4,096 bytes (counted as errors).
- Ignore rules, one ordered list in which the last matching pattern decides: the built-in project_ignore,
  the .gitignore files from the root down (each scoped to its directory, read by the worker as it enters
  it), then .ronin-project's [ignore], then its [allow] as negations. Supported: comments, blank lines,
  trailing blanks trimmed, '*' and '?' within a segment, "**" as a leading "**/", a trailing "/**" and a
  middle "/**/", a leading or inner '/' anchoring to the file's directory, a trailing '/' for directories
  only, '!' negation; ASCII is matched case-insensitively. Not supported: character classes ([a-z]),
  backslash escapes (\#, \!, "\ "), .git/info/exclude, core.excludesFile and the global gitignore. An
  ignored directory is never entered, so nothing below it can be included again (git behaves the same). In
  .ronin-project a pattern cannot start with '[' (that is a section header).
- The built-in list holds only names that cannot be source directories (.git/ .hg/ .svn/ .vs/
  node_modules/ __pycache__/) and binary extensions. build/, out/, bin/, obj/, target/ and dist/ are not
  in it: a name pattern matches at any depth and would silently hide real source directories; they belong
  in the project's .gitignore or .ronin-project. When a scan ends the echo area says "<N> files in <root>/
  (<M> ignored)" (M counts what a rule left out, not what was inside an ignored directory).
- The index: per file 16 bytes (the 64-bit character mask of its relative path, its directory, its name's
  offset), the name once (u16 length first), each directory's path once; about 33 bytes a file for short
  names. At most 4 projects are kept; the least recently used goes (its scan cancelled, its arenas
  released). project_max_files (1,000,000) stops a scan: "Project has more than N files; listed the first
  N" and "stopped at N files" in the finder.
- Staleness: a tree watch on the root (file and directory names) marks the index stale on its first
  change and is not re-armed until the next scan starts (one wake-up per index, however many files a build
  creates; re-armed with a fresh notification just before the walk, so a change during the walk marks the
  new index stale again). A stale index is rescanned the next time C-x p f needs it, into a second index
  while the first stays in use; the two are swapped when the scan is complete. The first scan of a root
  fills the live index directly, so the finder fills in as batches arrive.
- The finder never waits: the prompt opens at once with what is known; while the first scan runs it says
  "scanning: N files" after the count, "rescanning" during a rescan. An empty input lists the files
  visited this session (find-file, the finder, the command line; most recent first) that lie under the
  root, then the index in scan order. Rows show the path relative to the root; a row too long loses its
  left part ("…" first); matched characters are in completion_match. RET while the ranking for the
  current input is unfinished completes it first (bounded by the index: ~10 ms for a million paths), then
  takes the selected row; while the scan itself still runs RET uses what is known. C-j takes the input as
  typed, relative to the root (a new file). TAB takes the selected row.
- Fuzzy matching (fuzzy.c) is the finder's own matcher behind the minibuffer's `MiniMatcher`; M-x, buffers
  and find-file keep the Phase 7 matcher. A query is space-separated terms that must all match as
  subsequences; smart case (an uppercase letter anywhere makes it exact); '/' or '\' matches the separator.
  A term without '/' is tried inside the file name first (6 a character more), else over the whole path,
  from its leftmost place and from up to 8 later segment or word starts; the score: 16 a character, a
  segment start (after '/') 10, a word start (after _ - . or space) 8, a lower-to-upper change 7, the
  first character's bonus doubled, a run of consecutive characters keeping its first one's bonus (at least
  4), a gap -3 and -1 a byte more. A file name equal to the whole query ranks first; equal scores go to the
  shorter path, then index order.
- Ranking stays inside the frame budget: the clock is read every 512 candidates (a slice), the 64-bit
  character mask rejects first, the best 1,000 are kept in a heap and only they are sorted (after the work,
  whose deadline leaves the last sort's time). When the input only grows, the next pass looks only at the
  previous survivors (also those of a pass cut short), then at what it never reached; entries that arrive
  during a pass extend it. Spans are computed only for the rows drawn.
- The input tests' shield (Phase 10): the earlier retry (1b48e43) is gone. Each case waits (at most
  2 s) until no physical modifier is down and records GetLastInputInfo. Since Phase 12: a case whose
  injected key state did not hold (or that started with a modifier still down) tested nothing and is
  inconclusive whether it passed or failed, and whether or not real input was seen (the log says which key
  and whether real input came); it is repeated, at most 5 attempts, then fails as "key state unstable". A
  case whose key state held and whose result is wrong is a failure at once, never repeated (Phase 11 also
  called a failure with real input during the case inconclusive). Measured: Windows replaces a thread's key state
  with the keyboard's some time after real input, also input from before the case (16-625 ms in the
  failures seen). One failure without real input in the 2 s before was seen once afterwards (not explained yet).
- The benches never take the focus: their window is shown cloaked without activation, as the smoke's and
  --idle-check's. Screenshot runs take the workers' results (until none came for 300 ms) before the frame.

### Search over files and results (Phase 12)

- Literal search with isearch's smart case (an uppercase letter makes it exact). Regular expressions,
  whole words, project-wide replace and locations that follow later edits are in Later.
- The search engine's core works on plain byte ranges (a SearchPattern; the SSE2 candidate scan; the
  comparison); the gap-buffer search sits on top of it (each segment through the core's scan, a candidate
  across the gap compared through the window). The second-byte filter: the 16-byte mask of the first byte
  is ANDed with the mask of the second byte one position on ((b | 0x20) for a folded ASCII letter,
  equality otherwise), for exact needles of two bytes or more and for folded needles whose first two bytes
  are ASCII (a non-ASCII character never folds to an ASCII one); the vector path stops where the 17th byte
  would leave the valid range. Folded search over 100 MB: 41-43 ms -> 17 ms.
- Files are searched on worker threads under the threading rules (CLAUDE.md). A search lives at the start
  of its own reservation; each worker owns its arena and one fixed window (256 KB of text a step plus the
  overlap). Chunks overlap by the needle's length plus the shown context (GREP_LINE_SHOW + 4 bytes before
  the next start, GREP_LINE_SHOW after the last accepted one): a match is taken only while that much
  follows it, except at the end of the file, so nothing is reported twice or missed and the shown line
  needs no second read. Lines are counted while scanning (SSE2, up to each match and to the end of each
  step). Memory does not depend on file size. The number of workers is search_workers (1-16), or with 0
  half the logical processors, at most 8 (--bench-grep: 8 read 50% faster than 4 without slowing typing).
- A file is opened sharing read, write and delete and closed before the worker claims the next one: a
  search never makes a save (ReplaceFileW included), a build or git fail. --test holds two files open in
  the workers and saves, renames and deletes them.
- Binary files (a NUL in the first 8 KB) are skipped and counted; UTF-16 with a BOM (and an even size, the
  loader's rule) is converted chunk by chunk (utf16_to_utf8_chunk, shared with the loader); a UTF-8 BOM is
  skipped. Columns are 0-based byte offsets in the line (as the buffer holds the line), shown 1-based (grep
  and ripgrep do the same). A line longer than 200 bytes is shown cut around the match on character
  boundaries, "…" at each cut.
- The file list is the search's own, append-only: the main thread copies relative paths into it and
  publishes the count; a worker reads only entries below the count it read, which never change. The list
  is the project's index when it is fresh (copied inside the frame budget, the workers start at once),
  otherwise the running scan's files as they arrive (a first scan's live index, a rescan's shadow). Before
  finder.c resets or destroys an index a search follows, the search copies the rest of it; a followed scan
  that was cancelled makes the summary say the list is incomplete. So the list a search works on stays
  valid until it ends or is cancelled.
- Deterministic order: the results buffer receives files strictly in the order of the list, whatever the
  number of workers or the timing. Workers claim files in list order (a compare-and-swap below
  min(published, claim_limit)); a file's records are held until its DONE record and every earlier file is
  written (emit_next, the low-water mark). The claim limit is emit_next + 1,024 and is not raised while the
  held records pass 16 MB, so what is held stays bounded; the file at emit_next is always claimed already,
  so progress never stops. search_max_results keeps the first N matches in that order (a file that crosses
  it is cut at N, the workers are cancelled). The same search on the same files gives the same text and the
  same next-error order every time (--test: 1, 2, 4 and 8 workers, reruns, a large file early in the list).
- Buffers with unsaved changes under the root are searched from their text on the main thread, inside the
  frame budget: a listed one in its file's place (its file on disk is skipped by the workers), an unlisted
  one (a new file not yet on disk, or a file the ignore rules leave out) after the list, in the order of
  their paths. Unmodified buffers are their files and are searched from disk; buffers without a file, the
  results buffers and *Messages* are never searched.
- Results stream into the read-only buffer through the inhibit_read_only path: the workers' batches are
  taken by app_wake (a copy), and the frame's results work (after the search slices and the ranking, before
  the lexer states) feeds the list, writes the files in order and searches buffers, within the frame's
  deadline; the lines of a frame go into the buffer with one insert. Frames that carry input (keys, clicks,
  the wheel) leave the results work to the next frame, which is requested at once: a keystroke's frame
  never waits for it. The UI never waits for a search.
- One "locations" mechanism: a results buffer owns its locations (a file or buffer, a line, a byte column,
  a length); location i is on line first_line + i, so drawing, RET, n and p need no lookup. next-error and
  previous-error walk the most recent results buffer (the one last started, or used with RET, n or p) from
  any window; from a window showing it they act as RET, elsewhere they visit in that window (Emacs).
  Phase 13's build errors will use the same mechanism.
- A buffer can have its own keymap (Buffer.keymap), placed before the global one while that buffer is
  selected and neither the minibuffer nor an isearch is active. [keys search-results] is the first: RET
  compile-goto-error, n / p next- / previous-error-no-select, g recompile, q quit-window (Emacs' names).
- *search* is shown through the pop-up rule and selected (Emacs does so for grep), so RET works at once:
  point goes to the first match when it arrives, unless it was moved. Starting a new search or killing the
  buffer cancels the running one; a cancelled search ends on its own and is freed once its workers are
  done. At exit the searches and the scans share one shutdown bound of 1,000 ms.
- Search workers and scan workers run one step below normal priority (THREAD_PRIORITY_BELOW_NORMAL), so the
  main thread wins when every core is busy (Measurements (Phase 12) has the comparison).
- Colors: search_file (the keyword color unless set: white file names read best next to the line numbers)
  and search_line_number (the number color); the match on lazy_highlight.

### Builds (Phase 13)

- A build is a child process started through `<system directory>\cmd.exe /s /c "<command>"` (no search of the
  PATH or the current directory for cmd itself), with CREATE_NO_WINDOW, stdin from the NUL device (a read
  ends at once, as with a closed stdin, but no program fails on an invalid handle), stdout and stderr in one
  pipe. AutoRun commands of cmd still run, as in a console.
- The pipe is an overlapped named pipe (a unique name, one instance, remote clients refused): an anonymous
  pipe has no overlapped reads, and the reader must wait on the read, the process and its cancel event at
  once. Only the pipe's write end and NUL are inherited (PROC_THREAD_ATTRIBUTE_HANDLE_LIST): no other handle
  of ronin reaches the child, so one build never holds another's pipe open.
- The child is created suspended, put into a job object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, then
  resumed, so nothing it starts escapes the job. Killing the build terminates the job: the whole tree. ronin's
  exit (a crash too) closes the job handle, which ends the tree.
- The build ends when cmd.exe exits, not when the pipe reaches its end: a process the build leaves running
  (mspdbsrv.exe started by cl /Zi, a build server) can hold the pipe open forever. What is already in the
  pipe is still read (PeekNamedPipe). Then such leftovers are let go (the kill-on-close limit is cleared
  before the job is closed), as in Emacs; a kill, or an exit while the build runs, still ends them.
- The build's reader worker owns the process from its start to its close (start, read, kill, close), under
  the threading rules: the main thread never waits for CreateProcess or for the process; kill-compilation
  sets the cancel flag and the cancel event. The worker hands batches of lines to the main thread through
  one locked queue; app_wake only takes the list, the frame's results work writes the lines inside the
  frame budget, so typing never waits for build output (--bench-compile). A killed build ends on its own
  (app_wake frees it); at exit it shares the 1,000 ms bound with the scans and searches.
- Lines are cleaned on the worker: CR dropped, ANSI escape sequences stripped (CSI, OSC, two-byte), kept when
  valid UTF-8, otherwise converted from the OEM code page, decided per line. A line over 60,000 bytes is cut
  on a character boundary. A line shows when its newline arrives (and the last one at the end).
- The output cap (compilation_max_output_mb, 16) counts what goes into *compilation*; past it one
  "[output truncated]" line, and the rest is read and dropped so the child never blocks on a full pipe.
- Error lines are recognized by hand-written matchers, one per format, on the worker (no regular
  expressions): MSVC's form (cl, link with a location, csc, tsc: "path(L)" / "path(L,C)" and up to four
  numbers, then a kind word), GCC and Clang (with and without a column, and the bare "path:L:" as an error,
  as Emacs), "In file included from" (info), Jai ("path:L,C: Error|Warning|Info:"), tsc's pretty form, after
  MSBuild's "N>" prefix. A path may hold spaces, parentheses and a drive letter; a ':' elsewhere, the
  characters Windows forbids and "://" reject it. A candidate counts only when its file exists: as given
  when absolute; else under the build directory, beside the MSBuild project file the line names in
  brackets, under the project root, in that order; the answers are cached on the worker. There is no
  "Entering directory" tracking.
- Lines with a kind word but no file position ("LINK : fatal error LNK1104", "x.obj : error LNK2019",
  "error: ...", "ld: error: ...", "error MSB4019: ...") are still errors or warnings: their kind word is
  colored and they are counted in the summary, but they are not locations, so next-error never stops on
  them. A location whose file is nowhere falls back to that, when it had a kind word. A build whose output
  has an error line never reports "0 errors".
- A tool's summary (Phase 14): a line without a location whose text has a number right before an error or
  warning word ("1 error generated.", "3 warnings and 1 error generated.", "0 Error(s)", "Found 2 errors.",
  "Compilation failed (1 error)") has every count word colored in its kind's color and none counted. A number
  after the word ("errors: 3", make's "Error 1") is not a count.
- compilation_relative_paths (Phase 14, true): a location printed with an absolute path under the project
  root is shown with the path relative to the root (from the resolved file, backslashes); the location keeps
  the full path and its offsets follow. Paths printed relative, files outside the root and builds without a
  root are shown as printed.
- *compilation* is a results buffer: the Phase 12 locations mechanism with next-error, previous-error,
  RET, n and p. A Location now knows its row and where its file name, line:column and marked part are in
  its line (for a search the match, for a build the kind word), so a results buffer's lines need not be
  consecutive locations (this replaces Phase 12's "location i is on line first_line + i"); 16 bytes more a
  location. next-error, n and p skip notes (Emacs' compilation-skip-threshold 1); RET on a note's line
  visits it.
- Where RET, n, p, next-error and previous-error land stays highlighted (lazy_highlight) until the next key
  event (a prefix key too) or until its buffer is killed: the match in *search* and *occur*, the whole line
  for a build's location. No timer.
- ronin never runs a command it has not shown: C-x p c fills the prompt with the command last used for the
  project in this session, else .ronin-project's [build] command, and waits for RET; compile does the same
  keyed by the buffer's directory. recompile reruns *search* and *occur* in those buffers (as g did since
  Phase 12) and the last build anywhere else, without the prompt.
- The order before a start follows Emacs: the prompt, then the unsaved file buffers by
  compilation_save_buffers (ask: the save-some-buffers walk, where q stops asking and the build goes on;
  all; none), then "A compilation process is running; kill it?" when one runs. Exiting asks "A compilation
  is running; kill it and exit?" after the modified-buffers question. A running build does not block the end
  of the Windows session (only unsaved files do); WM_ENDSESSION's shutdown kills it. Killing *compilation*
  kills its build without asking, as killing *search* does.
- *compilation* is shown through the pop-up rule and not selected, as in Emacs (the selected window keeps it
  when it shows it already, so g does not split). compilation_scroll_output: first-error (windows whose
  point is at the end follow the output until the first error with a location is written, then go to it
  and stop), true (always follow), false (point stays at the start). Its mode line shows
  Compilation:running, :exit [N] (a code of 0xC0000000 and above in hex), :killed or :failed.
- When a build ends (finished, exited abnormally or killed; not when it could not start), every file buffer
  is checked against its file at once, as on an activation: what the build rewrote is reverted when
  unmodified (auto_revert) and flagged otherwise.
- `--child <script>` (dev builds) is the program the tests run: it runs before the log is opened and before
  any window, so the run that started it keeps its log. The tests prove they can fail: a matcher without
  its column, and a kill that ends only cmd.exe (the grandchild survived and three tests said so).

### Multiple cursors (Phase 14)

- Multiple cursors build on what exists since Phase 4: the cursor array per View, per-cursor commands, the
  single driver loop, markers, and one undo group per command. No command has a cursor loop of its own; the
  new commands (cursors.c) act on the View once (COMMAND_ONCE).
- The array is sorted by position after every command; View.primary names the primary (the view follows it,
  the mode line reports it and shows "mc:N"; show-paren follows it), and each cursor has a creation serial
  (the newest is skip's and unmark's target).
- Normalization, after every command and every mouse action: a cursor's span is its active region, or its
  point. Two cursors merge when their points are equal, when their regions share more than a boundary, or
  when a bare cursor lies strictly inside the other's region; touching regions and a cursor on a region's
  boundary stay apart; inactive marks never merge. Merges are transitive (one sweep over the spans sorted by
  start, then point). The merged cursor is the primary if it is one of them, else the first; with a region
  the union becomes its region, point at the union's end unless the survivor's point was at its region's
  start. A merged point that meets another cursor merges in a further pass. A linear check is the fast path.
- The ordered edit pass: with several cursors the driver runs them in position order inside one buffer
  batch (buffer.h): buffer_replace logs the edits of an ascending run instead of adjusting every marker, a
  marker is mapped through the run when read (its epoch, a binary search, the rules while it touches the next
  edit), and the batch's end applies the run to every marker once: O(markers x log edits) instead of
  O(markers x edits). An edit out of order flushes the run first, so the order decides only the speed. An
  edit that replaces the text ending exactly where the run's last edit inserted joins that insertion (one
  edit with an inner split that keeps both edits' marker rules), so RET leaving a whitespace-only line and a
  closing bracket reindenting its line stay one run. Chosen by measurement against the old pass (the
  numbers below) and checked against the sequential path (test_marker_batch: ascending, descending, mixed and
  joined runs, valid UTF-8 and invalid bytes). The gap moves one way within a command. One cursor keeps the
  old path.
- Swallowed cursors: within one command a cursor whose point lay strictly inside text an earlier cursor's edit
  removed is skipped (the batch reports it); a cursor at either end of a removed range still runs. Two
  cursors on one line with C-k: the second does nothing and the line below is not joined; adjacent cursors
  with C-d delete two characters.
- Line commands (COMMAND_LINE: indent-for-tab-command, unindent, comment-line, kill-whole-line,
  delete-indentation) run once per line: a cursor on the same line as the cursor before it (lines as the
  command found them) is skipped.
- COMMAND_SINGLE commands collapse to the primary first: isearch-forward and -backward, query-replace,
  replace-string, goto-line, mark-whole-buffer. A plain click collapses; switching buffers leaves one cursor
  (view_switch_buffer), a prompt aborted before it switches keeps them. keyboard-quit with several cursors:
  if any mark is active it deactivates the marks and keeps the cursors, otherwise it collapses (a C-g that
  cancels a prefix is keyboard-quit too).
- Undo restores every cursor: a command that starts with several cursors hands them to the buffer, and the
  group it starts keeps them in the log in front of its records (24 bytes each: point, mark, mark set, the
  primary); undoing that group rebuilds the view's cursors from them, marks inactive (Phase 6's rule). A
  merged typing group keeps the cursors of its first command; an undo group keeps those of the moment it
  ran, so undo-redo brings them back. A group made with one cursor restores one cursor at its point, as
  before. Undo of a group of more than 64 records runs in a batch (its records replayed newest first are
  descending runs). The cursors of the last undo stay valid until the next edit, which trims instead.
- The kill ring: a kill with several cursors keeps one piece per cursor that ran it, in order, in one entry:
  its text is the pieces joined by newlines (the clipboard's text), its piece offsets follow the text. A
  kill right after a kill with as many pieces adds each to its own piece (prepends for backward kills);
  otherwise it is a new entry. A yank with as many cursors as pieces gives each cursor its piece, with any
  other number the whole text; text from the clipboard is one piece; the first cursor checks the clipboard
  and yank-pop's index moves once.
- The commands (Emacs has none built in; the names and keys are the multiple-cursors package's without
  "mc/"): mark-next-like-this (C->) and mark-previous-like-this (C-<): with an active region, a cursor with
  the same region and direction at the next occurrence (literal, case sensitive, not overlapping the last
  cursor's span) after the last cursor / before the first; without one, a cursor on the next / previous line
  at that cursor's column (its goal column across vertical motions, as next-line). The new cursor is the
  primary; "No more matches" at the end. mark-all-like-this (C-c C-<): every occurrence from the start of
  the buffer, not overlapping, the others dropped first and the primary's own kept; "Marked N
  occurrences". skip-to-next-like-this moves the newest cursor on, unmark-next-like-this removes it (no
  keys). edit-lines (C-S-c C-S-c): a cursor on each line of the region, both ends included, at point's
  column or at the end of a shorter line; edit-ends-of-lines and edit-beginnings-of-lines (no keys).
  Ctrl+Shift+click adds a cursor (the primary) or removes the one there, never the last.
- A search that does not end within one slice inside the command goes on as the app's CursorJob in the
  frame's search work, like replace-string's replace-all; C-g stops it (nothing added), other keys and
  clicks wait. mark-all collects the positions and makes the cursors at the end, in one sorted pass.
- The cursor commands refuse in the minibuffer ("Multiple cursors are not available in the minibuffer"),
  which keeps one cursor; M-x does not run inside a prompt, so there they are only reached by a key binding.
- At most max_cursors (10000, 1 to 100000): every adding command stops there and says so ("Cursor limit
  reached (max_cursors = N)"). The cursor array's reservation holds about 419,000 (40 bytes a cursor).
- Drawing finds the cursors and regions on the visible lines by binary search on the sorted array (their
  spans' starts and ends both increase): cursors elsewhere cost nothing per frame. Every visible cursor and
  region is drawn in the selected window as before; other windows show their own cursors hollow and no
  regions.
- The differential fuzz (20,000 random commands of 53 kinds on random texts and cursor sets) runs each
  cursor's command through the one-cursor path on a flat text, moves the other cursors by the marker rules
  from a dev edit log of buffer_replace, applies the swallowed, line and piece rules, and normalizes with a
  naive pairwise implementation; the text, every cursor and the newest kill are compared after each command.
  The tests were shown to fail with the merge rule broken (touching regions merging: the fuzz) and with the
  cursor restore in undo broken (two undo tests), each in a scratch copy.

### The last pass (Phase 15)

- The name is ronin (teal until Phase 15); no migration code: a teal.conf is moved by hand.
- Release and bench link no C runtime: `/NODEFAULTLIB`, entry point `win32_entry` (`__isa_available_init`,
  wWinMain, ExitProcess); libcmt.lib and libvcruntime.lib are named on the link line only for the stack probe,
  memcpy / memset / memmove and their CPU dispatch; base.c defines `_fltused`. `__try` needs
  `__C_specific_handler`, which is forwarded to ntdll's export (found at startup) instead of pulling in the
  CRT's exception machinery. The debug build keeps the static CRT for /GS and /MTd.
- Cold modules (config, files, finder, command, isearch, cursors) are compiled for size with
  `#pragma optimize("s", on)` in the unity file; a module stays at /O2 when its bench gets slower.
- The glyph atlas commits 128 rows first and doubles them on demand up to its width, the texture made again
  at once, so glyphs keep their coordinates. A flush holds 16,384 quad instances (a 1280x800 frame of text is
  ~6,000); more flush more often.
- On minimizing, the device context is cleared and IDXGIDevice3::Trim gives the driver's temporary
  allocations back.
- `Command.after`: an optional step the driver runs after fn, with several cursors in a second ordered pass
  once fn ran at every cursor (self-insert's case-label reindent), so an edit before the cursor's own
  insertion does not flush the marker batch once per cursor.
- `early_window` (default off): the window is shown in the background color as soon as the config and the
  buffers exist (cloaked, painted, one composition, uncloaked, DWM transitions off), activated as the
  normal start; dev batch runs never show it early. The default is the user's call (HUMAN step).
- Crash safety net: an unhandled-exception filter (and a 64 KB stack guarantee on each of our threads, so a
  stack overflow can still run it; not tested) calls `app_crash_write` for every buffer, each inside its own
  `__try`; a modified file buffer is written with `buffer_write_to` (the save's encoder) through a 16 KB
  stack chunk to `%LOCALAPPDATA%\ronin\recovery`, plus a `.path` sidecar; no arena, no allocation, the gap
  never moved. The filter returns EXCEPTION_CONTINUE_SEARCH: Windows Error Reporting runs as before.
  Nothing deletes recovery files; recover-file opens the original and the copy side by side.
- The version lives in src/version.h, for the code and res/ronin.rc. The icon is drawn by
  tools/make_icon.c on every build (PNG images at 16-256 px with its own fixed-Huffman deflate, 9,230
  bytes) into build\gen; the .ico is never committed.
- Tried and not kept (numbers under "Measurements (Phase 15)"): a waitable swap chain and Present(0, 0)
  for the key's frame, four `D3D11CreateDevice` variants, the walk's stack without zeroing, a 1 x 1 swap
  chain while minimized, trimming on focus loss, /O1 or "s" for compile.c, results.c and project.c.

### Memory and resource leak audit (Phase 16)

- A leak is steady growth per cycle of a verdict metric while an operation that ends in the state it started
  from is repeated, measured after a warm-up. Bounded growth is not a leak: an arena may grow to a high-water
  mark and stay there.
- Verdict metrics: private commit; GDI objects (GetGuiResources); the handle ledger (below); our threads, events,
  processes and directory watches; the number of our reservations and their committed bytes (the registry
  `--memory-report` classifies by); the atlas's used area and cached glyphs; D3D11 live objects
  (ReportLiveDeviceObjects). Every one but private commit must be exactly flat. Private commit is a LEAK when it
  grows by more than 64 KB in at least two of the three steps between the last windows; a single such step is a
  STEP, listed with the WATCH verdicts but not failing the run (the process heap commits a chunk for threads
  Windows starts in the process, which the GPU driver charges: ~68 KB once, then flat). Atlas rows (at most the
  atlas width) and, in undo-cycle, the undo log (at most undo_limit_mb) are bounded: their verdict is the cap.
- Informational metrics are logged in the same tables and never make a leak; one that rises in each of the last
  three windows is marked WATCH and reported for a decision: the process handle total and USER objects (Windows'
  thread pool, RPC runtime and graphics stack move them: up to +8 and back over 2,000 config reloads, while the
  ledger and our own counters stay exactly flat, so those are the verdict on handles), the working set (OS
  trimming), the process thread count from Toolhelp (driver and thread-pool threads), the DXGI video memory in use
  (driver allocations) and the font's reset count (it rises by design when the text scale changes).
- Method (`--leak-check [SCENARIO|all] [--cycles N]`, debug build only, N = 200 by default): per scenario, after a
  prime step that fills the bounded caches to their caps once (*Messages*, every minibuffer history kind and its
  arena past a compaction, the kill ring), 20 warm-up cycles, then 4 windows of N/4 cycles with every metric
  sampled at the end of the warm-up and of each window. LEAK exits 6. Every cycle must leave the state the first
  one left (a text of the buffers, windows, points, the modified flags, prompts and workers; undo-cycle compares
  with the state before its first cycle, including the text's hash), or the run exits 9. It runs with the window
  never shown, focused by a focus event, keys through the window's queue and the main loop's pass, so every
  scenario has the renderer and atlas metrics. Where a cycle edits, it edits a buffer it visits and kills; only
  undo-cycle keeps its buffer, and its log must reach the cap during the warm-up ("warm-up did not reach undo cap",
  exit 9). When our memory moved, the reservations that grew are logged with the functions that made them
  (os_reserve records its callers in dev builds; dbghelp.dll names them, loaded on use). The caps of the bounded
  caches are asserted by `--test` (test_bounded_caps).
- The soak (`--soak`, headless) samples the leak metrics every 100,000 events and judges the last 500,000 once
  there are 600,000. Its exact verdicts are the ledger, GDI objects, our threads, events, processes and watches; its
  memory (private commit, our committed bytes, our reservations) is informational: the random walk keeps raising
  the high-water marks of the persistent arenas (the minibuffer, the kill ring, *Messages*) with the largest text
  it happens to push through them, and leaves a different set of buffers at each checkpoint (7 to 9 arenas each).
  The memory verdict comes from the deterministic `--leak-check` scenarios.
- The exit-time handle check is a ledger, not the process total: the os_* layer counts every kernel handle it
  opens and closes (dev builds), and the ledger must be exactly 0 at exit. The process total is logged after the
  window is created, after the first frame and at exit, with the difference by handle type. Equality of the
  process total with a startup baseline cannot hold: the device thread loads d3d11 and the driver while the
  window is created (the baseline depends on timing), and the driver and DirectWrite's shared factory keep
  handles for the life of the process after ronin released everything.
- At exit (dev builds): ReportLiveDeviceObjects may list only the device and its immediate context, the
  reservation registry may hold only the permanent and the frame scratch arenas, and the handle ledger must be
  0; the smoke and `--leak-check` exit 6 otherwise.
- Worker threads are reused: once a hardware D3D11 device exists, the GPU driver keeps ~215 bytes of the process
  heap for every thread that ends (measured with an NVIDIA driver; 0 with WARP, with only d3d11.dll and dxgi.dll
  loaded, or without a device). os_thread_start hands a task to a parked slot (a thread, a go and a done event),
  making one only when none is free; a finished task's thread parks on its event (0% CPU); joining waits for the
  task's end. Slots never move; their number is the most tasks ever running at once (at most 256); a task that
  never ends (a worker abandoned at shutdown) keeps its slot. The slots' handles are not in the ledger (a new peak
  of tasks would raise it): the slots are a bounded row of their own in the leak tables and the soak's, the cap
  asserted. Dev builds end the parked threads before the exit check.
- The main loop takes the workers' results when their wake event ended the wait: the wait resets that auto-reset
  event, so the look after it no longer decides alone (win32_wait / win32_after_wait, shared with the leak check).
- A buffer's path and name are rebuilt right after its struct in its meta arena when they change
  (buffer_set_names); the remembered build commands are rebuilt into a spare arena at each change.
- Bounded by design, with their caps: *Messages* (1,000 lines of at most 1,024 bytes), each minibuffer history
  kind (100 entries; the arena compacted below max(64 KB, 2 x live)), the kill ring (kill_ring_max entries), the
  remembered build commands (64), the root cache (256), the undo log (undo_limit_mb, trimmed to 3/4), the atlas
  rows (at most its width), the worker thread slots (256), and every arena's commit (high-water, never decommitted; Phase 17 amends this for a results buffer's held and pend arenas). The dying lists of scans,
  searches and builds (16 each) drop an object only when full and the oldest does not end within the shutdown
  bound, which never happens in practice; left as it is.
- A leak is fixed, never hidden by a wider tolerance; growth that is bounded by design gets its cap documented
  here and asserted by a test instead. A verdict metric that moves for reasons outside ronin is taken out of the
  verdict only with the evidence (by-type handle counts, memory owners, isolated experiments) and the user's
  decision.

### Repository and build-folder cleanup (Phase 17)

- Tracked files: sources, scripts, documentation, samples, LICENSE and the files GitHub needs. Nothing
  generated is tracked: no .exe, .obj, .pdb, .map, .ico, shader headers, logs, screenshots other than the
  README images (docs/images), fixtures or scratch copies. A generated file found tracked leaves the index
  (git rm --cached) and is ignored; history is never rewritten.
- One ignore file, .gitignore at the root: build/, dist/ and every generated pattern. No per-directory ignore
  files.
- Everything generated lives under build\ or dist\. Fixtures (the 100 MB bench files, the leak check's
  project, recovery directories, the tests' trees) are made on demand by the run that needs them, never
  required beforehand; build.bat makes build\tmp and build\shots. Proof copies live under build\proof\.
- `build.bat clean` removes build\ and dist\ entirely and nothing else; there is no partial clean. It acts on
  the directory holding build.bat whatever the current directory, and refuses (exit 10, nothing removed) when
  that directory is not ronin's repository root (src\ronin.c and verify.bat beside build.bat) or when build\ or
  dist\ is a junction, a link or a file. `rd /s` removes the junctions inside the tree (the tests left one, a
  loop, until Phase 18) without following them. verify.bat's clean-check proves each case on a scratch tree every run.
- A fresh clone builds and passes verify.bat with build.bat as its only step: the test that no required file
  is untracked or ignored.
- Nothing outside the repository is deleted by the cleanup; leftovers there are reported to the owner.
- The measurement flags (`--memory-report`, `--bench-latency`, `--startup-capture`, `--bench-longline`) read
  only the built-in config unless `--config` is given, as the smoke and the other benches do.
- A results buffer's transient arenas (held[0], held[1]: the records of files the workers finished ahead of the
  writer; pend: the lines of one frame) are committed to a fixed floor, RESULTS_TRANSIENT_FLOOR = 256 KB each, at
  the buffer's first run (never at startup: the startup memory is unchanged) and never given back below it.
  Why: their high-water depends on timing (how far the workers ran ahead, how many lines one frame wrote), and
  with commit never given back the memory left after a search depended on it; `--leak-check grep-cycle/done`
  flagged LEAK in 2 of 4 runs at 200 cycles (held[0] reaching its 64 KB high-water after the first window). Under
  the floor a run leaves the same memory every time: grep-cycle/done then showed 6,459,392 bytes in every sample of
  5 runs. The size: the leak scenario peaks at 64 KB (held[0]) and 4 KB (pend), and 256 KB, four times that, is
  the smallest power of two that leaves room for timing; for pend it is also RESULTS_FLUSH_BYTES, so a frame's
  lines normally fit. The cost: 768 KB of commit per results buffer once it has run (at most RESULTS_MAX = 4).
- What a larger run grew above the floor is given back when the window is minimized (app_trim, beside the
  renderer's IDXGIDevice3::Trim, outside any frame), never at a run's end: giving it back at every run's end cost
  the last frame of a 52,138-match search (held[0] 4.5-4.9 MB, held[1] 2.0-2.2 MB, pend 320 KB) 294-456 us of
  VirtualFree and lengthened the longest frame of `--bench-grep` at 2 and 4 workers beyond the noise (fix-off
  273-308 / 454-618 us against 504-592 / 713-736 us with 1 MB floors given back above). With the floor alone,
  first result and longest frame are within noise of the old code at 1, 2, 4 and 8 workers and at the cap
  (Measurements (Phase 17)). The trim touches only slots that are not running; such a slot is detached from every
  producer, because results_finish, results_compile_finish and results_stop clear r->grep, r->run and r->batches in
  the same call that ends the run (a dev ASSERT checks it). The arenas are written only on the main thread, from a
  search drained into the slot or the batches the slot took from its build; a detached search's batches are drained
  into nothing (results_wake) and a detached build's given back unread (results_drop_run), so workers still ending
  (joined later from the dying lists, the main thread never waiting) cannot reach them.
- This amends Phase 16's "every arena's commit (high-water, never decommitted)" for those three arenas.
- Their bound is not 2 x RESULTS_HELD_MAX + 4 MB, as first stated in this phase: the code compacts the held
  records when an arena passes 2 x what is still held + 4 MB (checked after each file written), and the workers
  stop claiming files once more than RESULTS_HELD_MAX is held, but files already claimed (up to
  RESULTS_CLAIM_AHEAD + the workers past the writer) still add their records, each up to search_max_results
  matches. The only hard limit today is the 1 GB reservation (os_fatal beyond it). A real cap is under "Later".

### Modal overlay and the command list (Phase 18)

The modal layer (modal.h / modal.c, part of the app):
- A modal is a box drawn last in the frame, over the windows, with its own clip, background and frame colors and
  its own keymap on top of the global one. At most one is open. While it is open every key and mouse event goes
  to it; the minibuffer and the windows get none. It needs no timer and no frame source of its own: every event
  already makes one frame, and the owner's work runs inside it. `app_wants_frame` is unchanged.
- An owner module gives a `ModalSpec`: a name (for messages), its keymap (a callback with the owner's data, so a
  reloaded config is found), an optional one-line field, one or two panes, `width_pct` (default 70),
  `height_pct` (60) and `pane_split_pct` (50), and callbacks: the field's text changed, accept (RET or a double
  click on a row), draw a row, the status at the field line's right end, the config applied again, closed. The
  layer knows nothing about commands or git.
- The API Phase 19 sees: `modal_open`, `modal_close`, `modal_active`, `modal_arena` (the owner's per-open memory,
  reset at the close), `modal_set_data` (the owner's state made in that arena after the open), `modal_set_rows`,
  `modal_list`, `modal_focus_pane`, `modal_run_after_close`, `modal_rect`. `field_changed` does not run at the
  open: the owner fills its rows itself after `modal_open` (its data does not exist before).
- Geometry: `width_pct` of the frame's columns and at most `height_pct` of the window area's rows (the frame above
  the echo line), centred on the window area, not on the selected window (70% of a narrow window is often under
  40 columns). Below 40 columns x 6 rows it does not open ("The frame is too small for the command list"); a
  resize below that closes it with the same message. The height follows the most rows any pane had since the open,
  so the box does not shrink while a filter narrows the list. It is computed from the layout every frame, so a DPI
  or font change only reflows it. With two panes the list area is split at `pane_split_pct` in whole cells, with a
  1-pixel modal_border line between; the focused pane's selected row is in modal_selection, the other's in
  modal_border.
- Keys go through [owner keymap, global] with the normal key state machine: a quit command (C-g, ESC) closes the
  box and says nothing (the frame underneath comes back as it was); a pending prefix cancelled by C-g only echoes
  "Quit" and the box stays; a command bound in the owner's keymap runs; a command flagged COMMAND_FIELD (the line
  editing ones: self-insert, the character and word motions, C-a / C-e, the deletes and kills of one line, yank,
  yank-pop, undo, undo-redo, the mark, transpose-chars, the case and whitespace commands) runs on the field; any
  other command is refused by name ("save-buffer is not available in the command list"), so nothing reaches the
  windows underneath.
- The commands run through `view_run_command` on the field's View (on the selected window's View for an owner
  without a field). Accept closes the box first and runs the chosen command after the running one, through
  `app_run_command` on the selected window: exactly the path of M-x, so a prompting command asks in the minibuffer
  afterwards.
- The mouse: a click outside the box closes it as C-g does (that press reaches nothing else); a click on a row
  selects it and focuses its pane; a double click accepts it; a click in the field moves point there; drags are
  ignored; the wheel scrolls the pane under the pointer and leaves the selection where it is (the per-frame layout
  only keeps the top in range). The pointer is the arrow over the whole frame.
- The field is a one-line View on its own Buffer (" *Modal*", not listed), made at the first open that needs one
  and kept until the shutdown, as the minibuffer has its own; startup memory is unchanged. Each open empties it
  and its undo history. Its kills go to the shared kill ring.
- The windows underneath draw their cursors hollow while the box has the keys, as under the minibuffer.
- The layer's commands are generic, so Phase 19 binds the same ones in its section: modal-next, modal-previous,
  modal-next-page, modal-previous-page (a page is the rows shown), modal-accept, modal-other-pane (the next pane,
  wrapping; nothing with one pane). Outside a modal they say "No modal is open".
- An open modal is not "busy" for the leak check or the soak's settling (it has no pending work, as the
  minibuffer); the soak presses C-g to close it before a memory reading, as for prompts.

The command list (commands_list.c, the layer's first owner):
- show-command-list (C-S-p; also M-x show-command-list) opens a box with a filter field and one row per command of
  the table: the name, the description, and the bindings. Rows, their "name description" strings, their key texts
  and the shown order are made in the modal's arena at the open; a keystroke scores the rows again into the same
  arrays and allocates nothing (a test checks the arena's position over 21 keystrokes and the seeded group after
  every operation).
- The filter is fuzzy.c's `fuzzy_score` with the finder's weights unchanged. A row's score is the best of three:
  its name; its description, less 32; and "name description", less 32, so a query whose words are split between
  the two ("occur lines") still matches. Name matches rank first; equal scores keep the table's order; the empty
  filter lists the whole table in its order. The spans are drawn in the candidate that won (those of "name
  description" split back into the two fields). Each row keeps the character masks (`project_mask`) of its name
  and description: a query with a character neither has is not scored (the filter's median went from 87 to
  10 us). The filter reads the first 256 bytes of the field.
- The bindings shown are computed, not stored: `keymap_where_is` walks the stack the selected window would have
  without the box (its buffer's keymap, then the global one) and keeps the sequences `key_input_feed` would run the
  command for: a sequence an earlier map binds to something else, or whose proper prefix is bound, does not count,
  one in two maps counts once. The two shortest (fewest chords, then fewest characters, then config order) are
  shown, then "+N". Computed at the open and when the config is applied again, never per frame.
- RET runs the selected row's command with `modal_run_after_close`; a list with no rows does nothing.
- Its keys: `[keys command-list]` (<down> C-n modal-next, <up> C-p modal-previous, C-v <next> and M-v <prior> the
  pages, RET C-m modal-accept); C-g and ESC are the global keyboard-quit. `C-S-p show-command-list` in `[keys]`:
  it takes C-S-p from shift-translation (until now a shift-selecting C-p, as in Emacs); S-<up> still selects.
- Every command has a description, a new field next to its name: one line of English, at most 60 bytes, printable
  ASCII, starting with a verb of a fixed list (the test's), no trailing period. The internal commands outside the
  table have one too (the initializers are positional). 136 commands are in the table now (129, the six modal-*
  commands and show-command-list).
- Colors: modal_background (#0c3434), modal_border (#2e8b8b) and modal_description (#a8977a) have their own
  defaults; modal_selection and modal_key follow selection and number unless a file sets them. They are the 30th to
  34th colors, so `Config.colors_set` became a u64.

Checks: the smoke's stage 11 (the box's colors, then the frame after C-g must equal the frame before C-S-p byte for
byte, then C-S-p "end" RET must have run end-of-buffer); `--bench-modal` (exit 9 when a filter keystroke's median
reaches 200 us); `--leak-check modal-cycle` (browse: the frames before the first cycle and after the last compared;
run: goto-line through it, its prompt cancelled); the soak's command list sessions; the seeded group "modal".

`--test` removes its junction loop (build\tmp\p11scan\src\loop) once the scanner test no longer needs it, and
test_run removes it again after the group.

### The git changes modal (Phase 19)

1. git is an external program: ronin runs the git executable as a child process through the Phase 13 process layer
   (job object, one overlapped pipe, the worker owning the process). No git code is in ronin. It is started directly,
   not through cmd.exe (`os_process_start_program`: no cmd quoting, no `%`, `&` or `^` in file names to fear, no
   AutoRun): `git_path` in [settings] when set (a file), else the first `git.exe` of PATH (`os_find_program`: every
   entry, quoted ones too, never the current directory). Both are KERNEL32 calls: no new DLL.
2. show-git-changes (C-x g) opens a box of two panes (90% x 80%, the left one 30%), its keys in `[keys git-changes]`.
   The box opens only once `git rev-parse --show-toplevel` has answered (decided with the user): the command echoes
   "Running git…" and starts a worker; no git, a folder git does not know, or a refusal of git's say so in the echo
   area and nothing ever opens (keys typed in those 20-50 ms go to the window). Then "Running git…" stands in the
   box's status line while git status runs. The main thread never waits for git.
3. The command lines, P = `--no-optional-locks --literal-pathspecs -c core.quotepath=false -c core.fsmonitor=false`:
   - `"<git>" P rev-parse --show-toplevel` in the project's root (`finder_root`), else the buffer's directory, else
     the default one;
   - `"<git>" P status --porcelain=v1 -z --branch --untracked-files=all` in the top level it printed;
   - `"<git>" P diff HEAD --no-color --no-ext-diff -- "<path>"`; for a rename or copy
     `... diff HEAD --no-color --no-ext-diff -M -- "<orig>" "<path>"`; for an untracked file, or in a repository with
     no commits yet, `... diff --no-index --no-color --no-ext-diff -- /dev/null "<path>"` (its exit code 1 means
     "differences").

   --no-optional-locks: status never takes index.lock while the user runs git; --literal-pathspecs: `[a].c` is a name,
   not a glob; core.fsmonitor=false: no fsmonitor daemon starts inside our job; --untracked-files=all: files, never a
   bare directory; --branch: the status line's branch, and "## No commits yet on" (or the older "## Initial commit
   on") for a repository without HEAD. Production runs keep the user's configuration otherwise.
4. The hand-off: a `GitJob` lives at the start of its own reservation (as a CompileRun); the request is copied into it
   before the start and only read by the worker. The worker runs its steps (OPEN: rev-parse, then status; STATUS; one
   DIFF), reads each process into its arena (stopped past the limit's lines or records, or 64 MB), parses there, and
   links one `GitBatch` per step to a list under the lock (held only to link), then `os_wake_main`. `app_wake` takes
   the list; TOPLEVEL opens the box, STATUS fills the left pane (5,000 entries copied at once: 0.14 ms to parse), a
   DIFF is copied into the modal's arena by the frame's background work, after the results' work, inside the same
   2 ms deadline and never in a frame with input, checking the clock every 256 lines (`--bench-git`: 20,000 lines
   take ~0.1 ms, one frame). The last batch says the worker made its last os_* call: the join does not wait.
5. Cancellation: closing the box (C-g, ESC, a click outside, RET, a resize too small), g, C-g while the open is
   pending, and the shutdown cancel a job: its atomic flag and its event, on which the worker's read waits with the
   process. The worker kills git's job object (git and anything it started, e.g. Git for Windows' launcher and the real
   git.exe), closes it, links a CANCELLED batch. The job goes to a dying list (16) and is freed when that batch comes;
   C-x g refuses to start when fewer than 4 slots are free ("git is still stopping; try again"), so nothing ever waits.
   The shutdown cancels every job, then joins them within the shared 1,000 ms bound.
6. The idle timeout: `os_process_read` gets `idle_ms` (builds pass 0) and OS_PROCESS_TIMEOUT; git silent for
   `git_timeout_ms` is killed and the view says "git timed out". The wait has a timeout only while git runs, on the
   worker: no idle timer.
7. Fetching: at most one git process per open box. Selecting a file not fetched starts its diff when none runs or is
   being copied; otherwise the selection waits and, when the running one ends, the file then selected is fetched (the
   files passed over are not). A diff is fetched once per open: each row is none, fetching, copying, cached or failed.
   g cancels the box's jobs, pops the modal arena back to the view's top level and program (the list and every diff go)
   and runs status again.
8. What is shown: the left rows are git's XY in git_status and the path ("old -> new" for a rename), in git's order,
   past `git_status_max_files` one row "[list cut at N files]". The right pane is the selected file's diff, kinds
   colored (diff_added, diff_removed, diff_hunk, diff_header; context and "\ No newline" in text), lines drawn as
   buffer lines are (tabs, caret notation: `app_draw_text_line`) and cut at the pane's edge; past `git_diff_max_lines`
   a last line "[diff cut at N lines]". Hunk bodies are read by their counts, so "--- x" inside a hunk is a removed
   line. One trailing CR is dropped (CRLF content; a change of line endings alone then shows as two equal lines, as
   accepted). A diff line is stored cut at 4,096 bytes.
9. stdout and stderr share the one pipe, so git's warnings ("LF will be replaced by CRLF") arrive inside its output: a
   `warning: ` line outside a hunk is not part of the diff, and a status record is what follows the last newline in
   it (no record holds one on Windows). A failed run's message is git's own line (decided with the user): its first
   non-empty line starting with `fatal:`, `error:` or `usage:`, else its first non-empty line, CR dropped, cut at
   120 bytes on a character boundary; "git: <line>" in the echo area (before the box) or the status line. Output
   holding "not a git repository" says "Not a git work tree: <dir>" instead. No git, and the timeout, have fixed
   texts (git said nothing). A second pipe for stderr alone was not made: two more handles and a second read a run.
10. RET (or a double click, in either pane) closes the box and runs the internal command git-changes-visit as M-x
    would: the file at the work tree's top level in the selected window, at its first change (the new side's line of the
    first + or - line) when its diff is in, otherwise where find-file puts point. A file deleted in the working tree is
    refused ("x is deleted in the working tree") and the box stays.
11. Nothing else changes while the box is open: no buffer is touched, ronin reads no file except git's output, nothing
    refreshes by itself (no watch, no timer). It opens over neither the minibuffer nor another box.
12. The modal layer, additively (the command list unchanged): `status_line` (a top line with the owner's status, no
    field), `scroll_panes` (a pane without a selection: the movement commands scroll it, accept gets its top row),
    `selected()` when a pane's selection moves, `full_height` (the box at its height from the open, its rows coming
    later) and `modal_grow_rows` (rows that arrive over frames).
13. Config: `git_path` (text, 259 bytes; empty is PATH; it cannot be written empty in a file), `git_diff_max_lines`
    (20,000; 100 to 1,000,000), `git_status_max_files` (5,000; 10 to 100,000), `git_timeout_ms` (30,000; 1,000 to
    600,000), applied from the next git run (`git_path` from the next C-x g or g); colors diff_added (#73e673) and
    diff_removed (#ff6e6e) with their own defaults, diff_hunk, diff_header and git_status following number, variable
    and string (decided with the user: hunk lines must not be green beside added ones); 39 colors now.
14. Tests: `--fake-git DIR ARGS` (dev builds) is the tests' git: it logs its arguments to DIR\calls.txt and runs the
    `--child` script that DIR\map.txt names for the first key found in them. The real-git test and smoke stage 12 run
    on build\tmp\gitrepo\repo (made once, its working tree set each time) with the system and global configuration shut
    out through `env_extra` (GIT_CONFIG_NOSYSTEM=1, GIT_CONFIG_GLOBAL to an empty file, HOME and XDG_CONFIG_HOME for a
    git older than 2.32); without git on PATH they log a skip, never a failure. The fake git's canned answers for that
    repository and the real git's give the same text.
15. The soak (found by this phase's `verify.bat phase`, decided with the user): its settle waits, 10 s at most, for a
    running project search and git's jobs before the checkpoint's sample (a search still running had counted its two
    workers as LEAK); the settled memory after the last block is judged against the highest settled value of the first
    half of the run, +2 MB, matching the Phase 16 plateau rule (one first block alone was too low a reference: with this
    phase's event stream it is 8.4 MB, the plateau of 12-13 MB comes later); the typing probe is the fastest of three
    runs (other programs' load slowed one run of the debug build by up to a third), the 1.5x rule unchanged.

### Terminals (Phase 20)

1. Two commands, one mechanism (src/terminal.c, part of the app, included after git_changes.c, compiled for size):
   open-terminal (C-c t) opens a terminal window in the selected window's file's directory, else the directory given on
   the command line, else the current one (the order of `files_default_dir`); open-ai-terminal (C-c a) one in the
   project's root (`finder_root`), else the file's directory, else the default one, running the AI command. The
   directory is normalised as a build's (`results_compile_dir`: backslashes, no trailing one but a drive root's). The
   echo area says "Opened terminal in C:/x/y" or "Opened claude in C:/x/y" (the path with forward slashes, as every
   path in the echo area; the command as written). Refused in the minibuffer ("A terminal cannot be opened from the
   minibuffer"); a modal box refuses them as every command it does not own.
2. Fire and forget: ronin does not read from or write to the terminal, keeps no handle to it, puts it in no job and
   never waits for it; the terminal outlives ronin. `os_process_launch` (KERNEL32's CreateProcessW: no ShellExecute,
   no shell32, no new DLL or import) starts `"<program>" <args>` in the directory with nothing inherited, the
   inherited environment, CREATE_NEW_CONSOLE for the classic console or CREATE_NO_WINDOW otherwise (ignored by a GUI
   program such as wt.exe), and closes both handles at once (the ledger +2 -2; `live_processes`, "started minus
   closed", is not touched). It returns 0 or the Win32 error.
3. ronin's own job (decided with the user): the first attempt adds CREATE_BREAKAWAY_FROM_JOB (no effect when ronin is
   in no job); on ERROR_ACCESS_DENIED (a job that does not allow breaking away) it retries once without it. Dev builds
   log which ("launch: <program> started outside any job (breakaway)" or "launch: breakaway refused (error 5),
   <program> started inside ronin's job"; `os_dev_last_launch`). When the retry starts it inside a launcher's
   kill-on-close job, the terminal ends with that job (a known limit).
4. Settings in [settings]: `ai_provider` (claude, openai, custom; claude), `ai_command` (text, 259 bytes, as git_path;
   when set it runs for any provider), `terminal` (auto, wt, cmd; auto). claude runs `claude`, openai `codex`; custom
   with no `ai_command` says "ai_command is empty" and starts nothing. A bad value is reported at load with its line
   and the value before it stays (the built-in one unless the same file set it before). They are read when the
   command runs: a reload applies from the next C-c t / C-c a with no code of its own.
5. The program: `terminal = wt` is Windows Terminal, `wt.exe` found on PATH by `os_find_program` (the App Execution
   Alias in WindowsApps counts: GetFileAttributesW succeeds on it, CreateProcessW starts it); missing: "Windows
   Terminal (wt.exe) not found". `cmd` is the classic console, `<GetSystemDirectoryW>\cmd.exe` (`os_system_program`,
   never PATH's). `auto` is wt when found, else cmd. A failed start: "Could not start <program>: error N". A missing
   AI command is reported by cmd inside the new window: ronin does not look for it first.
6. The command lines (W wt.exe's full path, C the system's cmd.exe; `<dir>` quoted as CommandLineToArgvW reads it back,
   so a drive root is `"C:\\"`):

   | | wt | cmd (a new console) |
   |---|---|---|
   | open-terminal | `"W" -d "<dir>"` | `"C"` |
   | open-ai-terminal | `"W" -d "<dir>" "C" /s /k "<command>"` | `"C" /s /k "<command>"` |

   each started in `<dir>`. cmd /k is always the AI command's shell: npm's `claude.cmd` resolves there and the window
   stays open after the command ends. /s (decided with the user) makes cmd strip exactly the outer quotes, so a quoted
   program path in `ai_command` (`"C:\Program Files\x\claude.exe" --continue`) survives; Phase 13's builds use /s /c
   too. The wt form passes cmd by its full system path (decided with the user): PATH cannot change which cmd runs.
   wt splits its command line into tabs at `;`, so in the wt form only a `;` (in the directory or the command) is
   written `\;`, wt's escape.
7. Tests never open a real terminal. Dev builds: `--fake-terminal DIR PROGRAM CONSOLE ARGS` (win32_child.c, before
   the log opens) appends "<its working directory>|PROGRAM|CONSOLE|ARGS" to DIR\calls.txt in one write, ARGS exactly
   as they stand in its raw command line (GetCommandLineW), so the quoting is checked byte for byte. The app starts it
   instead of the real program when `app->terminal.dev_program` is set (as git's `dev_program`), always without a
   console; `dev_wt_present` makes the wt lookup deterministic. Opening a real terminal takes the foreground: HUMAN
   only (V1-V5).
8. One guard for git and the terminals (decided with the user): in a dev batch run (`--test`, `--smoke`, a bench,
   `--soak`, `--fuzz-seeds`, `--leak-check`, a screenshot run: `os_dev_batch_run`), open-terminal and open-ai-terminal
   without the fake, and show-git-changes without a chosen git (the fake, or the real one with its configuration shut
   out, as the real-git test and smoke stage 12 set it), start nothing: "terminal: no real terminal in a test run",
   "git: no real git in a test run", counted by `os_dev_real_refused`, which must be 0 at the end of `--test`, every
   fuzz worker, the soak and the leak check (each guard's own test takes its one back). The "no git on PATH" test sets
   `dev_allow_real` (its PATH holds no git). The modal fuzz, whose RET runs whatever command is selected, installs the
   fake git and the fake terminal; it had no fake git before (found while planning this phase).
9. git's warnings (the Phase 19 question): inside a hunk body a `warning: ` line was counted as a context line, so the
   hunk ended one line early and its last line was drawn as a header (and missed by the first changed line). Now it is
   skipped there without touching the counts (no body line starts with `w`; `+warning: x` stays an added line), and a
   rename's or copy's source drops a warning in front of it as a record does (d941010).

### Themes (Phase 21)

1. A theme is a file in the config format holding a `[theme]` block (`name`, `source`, `commit`, `license`,
   `author`: informational, the file name is the theme's name; a `name` that differs is a warning, an unknown key an
   error) and a `[colors]` section with the keys ronin.conf takes. It is read by `config_parse` in a theme mode
   (`config_parse_theme`): any other section is an error ("a theme has only [theme] and [colors] sections") and its
   lines are skipped, the rest applies. No second parser. Every key is optional.
2. Built-in themes: ronin (the colors of every phase before, still the default), moonfly, oxide, vague and
   mellifluous. Their sources are `themes\*.conf` in the repository; `tools\make_themes.c` embeds them into
   `build\gen\themes.h` on every build (comment and blank lines dropped, blanks around `=` dropped, name order, ronin
   required) and stops the build when they reach 8,192 bytes together (4,730 today), when a file name is no theme name
   or ronin.conf is missing. A theme named N is `<config dir>\themes\N.conf`, else `<ronin.exe's dir>\themes\N.conf`
   (when that is another folder: not in a portable install), else the built-in N; a file replaces the built-in theme
   of its name. The config dir is the folder of the ronin.conf in use; without one (the dev batch runs' built-in
   configuration) only the built-in themes. Names: letters, digits, `_`, `-`, `.`, at most 64 bytes, compared ASCII
   case-insensitively and kept lowercase.
3. `[settings] theme = <name>` (default ronin). The colors resolve in this order: the built-in configuration, the
   ronin theme, ronin.conf, the chosen theme's colors, then ronin.conf's `[colors]` keys restored on top, then what
   derives (`config_derived_colors`). `config_parse` records the keys the text it parsed set (`colors_last`); after
   the user file the user's keys are kept, the theme parsed over everything, the kept keys copied back and the derived
   ones derived again: nothing is parsed twice. An unknown theme is an error on the theme setting's line,
   `ronin.conf:N: theme: no theme 'foo' (using ronin)` (a file that cannot be read the same, "cannot read"), and the
   colors stay ronin's; a theme file being written (a sharing violation) is retried as ronin.conf is. The built-in
   configuration's `[colors]` is a commented list of the keys and what derives from what (decided with the user): a
   ronin.conf written by C-c , pins no color.
4. Eight more keys derive, all pairs that had one value in ronin, so ronin is unchanged bit for bit and a theme that
   leaves them out gets its own colors (decided with the user): constant from number, function and completion_match
   from keyword, directive and compilation_info from type, prompt from string, completion_selection from selection.
   The selected mode line has two keys of its own, `mode_line_background` and `mode_line_text`, derived from text and
   background (inverse video as before), so a theme's StatusLine has somewhere to go. 41 keys, within the 64-bit mask.
   A key that cannot derive and that a theme leaves out keeps ronin's color, by the order of 3.
5. The masked-theme note (decided with the user): when the theme in effect is not ronin and ronin.conf's `[colors]`
   set N keys, every load (startup, a reload, set-theme) says "ronin.conf: N [colors] keys override theme X" in
   *Messages* and, unless the load shows an error there, in the echo area. Not an error: the colors apply as resolved.
   A ronin.conf written by C-c , before this phase sets 30 keys.
6. Hot reload: `ConfigSource` also remembers the theme asked for and what its name found (where, size, write time); a
   notification reloads when ronin.conf changed or when that changed (the theme file written, added, removed), with the
   same 50 ms settle; another theme's file reloads nothing. The app watches both themes\ folders that exist with
   `os_watch_dir`; the config folder's watch also reports folder names (`os_watch_dir`'s `subdirs` flag:
   FILE_NOTIFY_CHANGE_DIR_NAME, decided with the user), so a themes\ created next to ronin.conf later is watched from
   then on and one removed is let go. 15 watch slots of 16 at most (config, two themes\, 8 buffer folders, 4 projects).
   An exe-side themes\ created after the start is seen at the next start (Later).
7. M-x set-theme (no key; "Switch the colors to another theme for this session"): a MINI_CHOICE prompt with
   require_match over every theme there is (`theme_list`: the built-in ones and both folders' `*.conf`, each once, in
   name order), each annotated with where it comes from ("built-in", "themes\ next to ronin.conf", "themes\ next to
   ronin.exe", ", replaces the built-in one") and the one in effect marked ", current" (instead of a default: the
   minibuffer has no default input). The choice is a session override in `ConfigSource` applied through the normal
   reload (ronin.conf is read again: one load, ~0.25-0.3 ms in the bench build, and one frame); nothing is written. It
   holds across reloads until ronin.conf's own theme setting changes (decided with the user), and choosing ronin.conf's
   theme ends it. The echo: "Theme vague", "(ronin.conf says ronin)" while they differ, "; ronin.conf: N [colors] keys
   override it", "; 1 error, see *Messages*". It is refused in the minibuffer (as every prompting command) and runs from
   the command list. The command lives in app.c, beside set-language: commands need the App, which theme.c, a core
   file included before app.c, does not see.
8. Switching costs no atlas work: glyphs are white-on-black coverage and the color is per instance in quad.hlsl, and
   `font_reconfigure` returns at once when the font parameters are unchanged. The caption color follows background
   (`os_set_caption_color` on every apply).
9. Tests keep ronin: every pixel probe of the smoke and the tests runs on it. Smoke stage 14 switches to each built-in
   theme through M-x set-theme and probes its frame against the theme's file resolved by the smoke itself (not the
   app's colors), then switches back to ronin and compares the frame byte for byte.
10. Porting (decided with the user): each theme from its repository's Lua source at a pinned commit, fetched into a
    scratch folder outside the repository (deleted afterwards). moonfly, oxide and vague were read as text (their hex
    values, links resolved by hand). mellifluous writes no color values (it computes them from OkHSL when it loads):
    its colors were read from the installed Neovim (v0.11.5) running it, with the exact command in its file. Only
    color values and their names were taken; all four are MIT (THIRD_PARTY.md).

The mapping from Neovim highlight groups to ronin's keys. `A → B`: the first group the theme defines wins; a group
the theme does not define but Neovim resolves by its own default link to one the theme does define counts (Keyword →
Statement, Number → Constant, Boolean → Constant, PreProc's Include and Macro, and syntax/diff.vim's `diffAdded` →
Added, `diffRemoved` → Removed, `diffLine` → Statement, `diffFile` → Type); a color of Neovim's own default color
scheme does not. A group with a background and no foreground keeps the text in Normal's color; an undefined Cursor
is reverse video, Normal's foreground. Neovim's DiffAdd / DiffDelete / DiffChange / DiffText are line backgrounds
while ronin's diff keys color text, so the foreground groups come first (decided with the user).

| ronin key | Neovim group (attribute) |
|---|---|
| background / text | Normal bg / fg |
| cursor | Cursor bg (undefined: Normal fg) |
| selection | Visual bg |
| comment, string | Comment fg, String fg |
| keyword | Keyword fg (→ Statement) |
| number | Number fg (→ Constant) |
| constant | Boolean fg (→ Constant): true, false, null |
| type, variable, function, directive | Type fg, Identifier fg, Function fg, PreProc fg |
| paren_match | MatchParen bg |
| prompt | Question fg → Title fg |
| completion_selection | PmenuSel bg |
| completion_match | PmenuMatch fg → CmpItemAbbrMatch fg → BlinkCmpLabelMatch fg |
| isearch / isearch_text | CurSearch bg / fg (no fg: Normal fg) → IncSearch bg / fg |
| lazy_highlight | Search bg |
| isearch_fail | DiffDelete bg (the failing text stays in text over it) |
| window_divider | WinSeparator fg (→ VertSplit) |
| mode_line_background / mode_line_text | StatusLine bg / fg |
| mode_line_inactive_background / _text | StatusLineNC bg / fg |
| search_file, search_line_number | Directory fg, LineNr fg |
| compilation_error / _warning / _info | DiagnosticError / DiagnosticWarn / DiagnosticInfo fg |
| modal_background | NormalFloat bg → Pmenu bg |
| modal_border | FloatBorder fg (equal to the float's bg, i.e. invisible: WinSeparator fg) |
| modal_selection | PmenuSel bg |
| modal_description, modal_key | Comment fg, Special fg |
| diff_added | diffAdded fg → Added fg → GitSignsAdd fg → DiagnosticOk fg → GitGutterAdd fg (decided with the user) |
| diff_removed | diffRemoved fg → Removed fg → GitSignsDelete fg → DiagnosticError fg → GitGutterDelete fg |
| diff_hunk | diffLine fg → Changed fg |
| diff_header | diffFile fg → Title fg |
| git_status | GitSignsChange fg → Changed fg |

What the four themes have no group for (each file says so): oxide defines no completion-match group (it follows
keyword), no Changed or GitSignsChange (git_status follows string) and none of diff_added's chain (since Phase 22
diff_added is its palette's `green`, #5bb661, which lua/oxide/colors.lua:26 documents as "Strings, success, added
items"; before, it kept ronin's #73e673; decided with the user; diff_removed is its DiagnosticError red); vague no PmenuSel (completion_selection and modal_selection follow
selection), no StatusLineNC background (follows background), no FloatBorder (WinSeparator) and no Cursor (Normal fg);
mellifluous's FloatBorder is invisible (WinSeparator) and its CurSearch has no foreground (Normal fg); moonfly has a
group for every key. vague and mellifluous draw the current search match like the others (CurSearch = Search):
faithful to them, the current match is not set apart.

### Decisions, Phase 22 (README, contribution guide and repository files)

1. README.md is for a user who has never seen ronin, in this order: one sentence, a line of links (Install, Keys,
   Configuration, Themes, Contributing), the hero screenshot (two windows, both mode lines, M-x in the minibuffer, on
   ronin), why (the three priorities and three numbers), what it does, install (the latest
   `ronin-<version>-windows-x64.zip` from Releases, the unsigned exe's SmartScreen warning, the command line), the first
   five minutes (a key table), configuration (where ronin.conf is, `C-c ,`, hot reload, the common settings as a table),
   themes (the four pictures, picking and adding one, credits), one section each for projects, search, builds, git,
   terminals and multiple cursors with their key tables, known limits, the roadmap after 1.0, contributing, license and
   credits. No version is written in it (decided with the user): the zip's name comes from dist.bat and src/version.h.
   The numbers table and the testing notes stay here, the build instructions moved to CONTRIBUTING.md, the
   learning-project paragraph moved to this file's introduction (decided with the user).
2. The README check is a `--test` group (`test_readme`, src/test.c), so `verify.bat` runs it on every commit (3-6 ms).
   It reads README.md and CONTRIBUTING.md from the exe's folder's parent. Its markers are what a reader sees anyway: a
   key table is headed exactly `| keys | command | what it does |`, a settings table `| setting | default | what it
   does |`; a key table outside the global keymap has `<!-- keymap: NAME -->` (invisible on GitHub) on the line before.
   Each key must be written as `key_seq_print` writes it, and run its row's command: through describe-key itself (the
   keys "C-h k ..." fed to a headless app, the echo compared exactly) where describe-key can run (the global keymap, and
   *scratch* given the search-results or compilation keymap), else through `keymap_get` on the section and then the
   global map (minibuffer, isearch, command-list and git-changes: a box refuses describe-key, and it would end a prompt
   or an isearch), the order key_lookup's stack has there. A settings row is parsed as `name = default` by
   `config_parse` over the built-in configuration and must give no diagnostic, so the name exists and its default is
   valid, through the real parser (config.c has no table to ask, and needs none). ini blocks of Configuration and Themes
   parse the same way (a `[theme]` block in theme mode), laid out on their own lines, so a diagnostic already names the
   README line. Beyond the user's two checks, cheap ones of the same kind: `M-x name` names a command, a C- / M- key in
   prose is bound in some keymap (Known limits and Roadmap name keys that do not exist yet), names with '_' in
   Configuration are a setting or a color, relative links and #anchors resolve (GitHub's slug rule), external links are
   logged. Every failure is counted, each names its file and line; a README without a key table fails, so a
   restructured README cannot switch the check off. 16 broken fragments are checked in every run.
3. CONTRIBUTING.md fits one screen of scrolling (73 lines): building and the gate, the rules of a pull request from
   CLAUDE.md, a feature as an issue first, a bug report without a log (release builds write none: the keys, the file,
   ronin.conf's changed lines, *Messages*, a screenshot, Windows' version, scale and layout, the recovery folder), what
   is out of scope and why, the review against the priority order.
4. .github/ISSUE_TEMPLATE: bug_report.md, feature_request.md, config.yml (blank issues off); .github/
   PULL_REQUEST_TEMPLATE.md, the checklist of CONTRIBUTING.md (named, not linked: a relative link in a pull request's
   text does not resolve).
5. CI, .github/workflows/build.yml on windows-latest, read-only, no secret: the three builds with the runner's Visual
   Studio (nothing pinned: the flags need VS 2019 16.10 or later), the toolset and the sizes in the job summary,
   `--test` on debug and bench, `--check-imports`, `--version`; each GUI exe started and waited for itself (not the
   grandchildren some tests let go), its log uploaded. The debug smoke cannot run there: it requires the D3D11 debug
   layer (exit 4), an optional Windows feature. The bench smoke runs as an informational step (`continue-on-error`,
   decided with the user) that prints the session, the exit code's meaning and the device lines of the log, so the first
   run says whether WARP renders on the runner's desktop; its outcome goes into the Measurements once the user reports
   it. The smoke gate and `verify.bat phase` stay local (CONTRIBUTING.md says so).
6. How the code is written, said once in each place (decided with the user): README, "ronin's code is written with
   Claude Code, under the direction and review of its owner; the Co-Authored-By line in every commit says so."
   CONTRIBUTING, the same and that a pull request is judged the same way whoever or whatever wrote it.
7. Nothing in src/ changed but oxide's diff_added (its palette's documented "added" green, #5bb661, decided with the
   user), the test, and two soak fixes the phase gate needed (decided with the user): the soak empties build\tmp\soak,
   subfolders included, at its start (`soak_clear`), so a seed gives the same run whatever ran before; and the rule below.
8. The soak's memory rule (decided by the user): the +2 MB settled-growth check (the last checkpoint against the first
   half's highest) applies to our arenas without the undo logs only, the number ronin controls, which was flat (-52 KB)
   in the failing run. The total settled memory, the process heaps, the thread stacks and "other" are informational rows
   of the leak-metric table (WATCH when they rose at every checkpoint of the second half), never a verdict; the counters
   are measured as before. Reason: the growth came from the system's heaps around process creation and from parked
   worker stacks, neither of them ronin's memory, and it varies between runs of identical events.

## Later

- Leaving an active minibuffer with C-x o (Emacs lets you edit elsewhere and come back); now refused.
- Window configurations (winner-mode undo of window changes, C-x r w / C-x r j).
- Copying the window's remembered positions into the new window on a split (Emacs does).
- Legacy code pages (Windows-1254, Latin-1, ...) and BOM-less UTF-16 detection.
- ESC as the Meta prefix, as in Emacs (ESC is keyboard-quit for now).
- *Messages*: collapse a repeated message into "msg [2 times]", as Emacs does.
- Chords on Turkish Q: Ctrl+Shift+ı gives C-S-i (default case mapping); a layout-aware letter
  case would make it C-S-ı.
- Wide (East Asian) characters and emoji occupying two cells; color emoji.
- System font fallback for codepoints Consolas lacks.
- Bold / italic faces, if a theme ever wants them.
- Themes (Phase 21): an exe-side themes\ folder created after the start (a non-portable install) is seen at the next
  start (the exe's folder is not watched for folder names); a default input for set-theme's prompt (the minibuffer has
  none; the current theme is marked instead); light themes wait for the gamma-correct ClearType blending above.
- Keys typed while ronin's window is being moved or sized by its frame (Windows' modal move loop, a click held on the
  title bar) are handled but drawn only when the move ends (found while measuring Phase 21: `--bench-latency` loses
  keys when its window is clicked).
- Linear-space (gamma-corrected) ClearType blending for dark text on light backgrounds.
- Line wrapping (visual lines).
- Cursor blink, smooth scrolling, line numbers: config candidates (each needs a timer or
  more layout).
- Column cache for very long lines: point's column is found by scanning from the line start
  (each view_ensure_visible and the drawing scan it; printable ASCII 16 bytes at a time since
  Phase 9, other text byte by byte). On a 5 MB minified line typing costs 0.75-1.14 ms a key at its end
  and 0.38-0.64 in its middle, moving 0.47-0.74 (`--bench-longline`, Phase 15). Resumable lexing inside
  such a line would come after it, if the lexing still shows.
- Prefix arguments (C-u), the mark ring, rectangle commands, overwrite mode.
- Clipboard delayed rendering (a 50 MB kill is converted to UTF-16 at once).
- Auto-scroll while dragging outside the window; after a double or triple click, a drag that
  extends by words or lines.
- Multiple cursors spread over a whole 100 MB file cost ~20 ms per key (the gap travels through ~100 MB each
  command); processing the cursors from the gap's end when it is past the last cursor would halve it.
- RET on lines with unbalanced braces far apart (the 100 MB bench text) costs ~0.4-0.6 ms per cursor, the
  same as with one cursor there: the indentation's bracket match lexes back up to SYNTAX_MATCH_MAX (256 KB).
  Not zeroing the walk's stack gained nothing (Phase 15); a per-line bracket summary (lines that are
  balanced skipped whole) would.
- A mark-all with 100,000 cursors (max_cursors at the top of its range) makes them all in one frame (~5.5
  ms); and keys typed while a long mark-all searches wait instead of being queued.
- Per-cursor kill rings (the multiple-cursors package's mc/kill-ring); undo with one cursor still restores
  only its point, not its mark.
- Auto-save (the crash recovery of Phase 15 writes only on an unhandled exception inside ronin).
- *Messages*: a message logged outside a command (an undefined key) moves its point without scrolling the
  window that shows it until the next command (the soak's visibility check skips *Messages*).
- Saving on a background thread: a save waits for ReplaceFileW, 5-7 ms, once 146 ms in a million soak
  events (the file system).
- Writing to a path another buffer visits (Emacs asks first; ronin then has two buffers on it).
- Clicking a candidate in the list; dimming the shadowed part of a file name ("c:/a/~/b").
- Candidates' bindings from the minibuffer keymap in M-x annotations (only global ones are shown).
- A change on disk within the same file-time tick at the same size goes unseen (a content hash on
  activation would catch it, at the cost of reading every file).
- Slow directory listings (network shares) block the UI while find-file lists a directory (the scanner's
  worker could list it now).
- Projects: updating an index from the change notifications (ReadDirectoryChangesW) instead of rescanning;
  following symlinks and junctions with loop detection; character classes and escapes in ignore patterns,
  .git/info/exclude and the global excludes file; frecency (a bonus for visited files in non-empty
  queries); an optimal alignment for scoring (fzf v2's) instead of a few greedy starts; ranking a million
  paths on several threads.
- Revert: map positions through a line diff of the old and new text, so a marker in an unchanged
  line that moved keeps its place in it (now: line and column inside the replaced range).
- JSX tags in JavaScript / TypeScript highlighting.
- Regular-expression search (isearch-forward-regexp, query-replace-regexp, and in project-search and occur).
- A total match count in isearch ("3/41").
- Editing the search string in the minibuffer (M-e in isearch).
- Queueing the keys typed while query-replace searches for its next match on a huge buffer (now
  ignored beyond one slice).
- Turkish-aware case folding for search (İ / i, I / ı).
- Project-wide replace (query-replace over the locations of *search*).
- Whole-word search (project-search, occur, isearch).
- Locations that follow later edits (markers in the visited buffers; now a location is its line and
  byte column as found, so an edit above it moves the target).
- Columns in characters instead of bytes in results lines (now 1-based byte columns, as grep and ripgrep).
- Worker thread slots stay until the exit (as many as tasks ever ran at once, each parked thread keeping its
  stack); ending a slot idle for a while would give that back, at the GPU driver's per-thread cost (Phase 16).
- Parked worker threads above a small number (say 8) ended in the minimize trim path, next to the results buffers'
  give-back, so the pool shrinks when the user minimizes (Phase 22, from the user: the soak's stacks grew 2.3 -> 3.0 MB as
  the slots went 24 -> 32).
- Builds: run and debug commands; several named build commands per project; input to the process;
  "Entering directory" tracking; user-defined error formats; showing a line before its newline (a progress
  line); a save question limited to the project's buffers; a unique-suffix lookup in the project index for
  error paths that still do not resolve.
- `--bench-latency` exited 9 once in six runs: 482 of the 500 keys had no Present of their own (Phase 17; the
  cause not looked into).
- The command list (Phase 18): the descriptions in M-x's list and in describe-key; M-< / M-> (first and last row);
  the bindings of the minibuffer's and isearch's keymaps (only the current window's are shown); a field longer
  than its line scrolls but cannot wrap. The input harness checks `[keys command-list]` only through the global bindings
  it shares.
- The git changes view (Phase 19): staging, unstaging, committing and reverting; a filter field over the file list;
  horizontal scrolling of the diff; word-level highlighting inside changed lines; a refresh on a change in the work
  tree (now g only); stderr in its own pipe (git's messages are picked from the shared one); the diff of the files a
  selection passed over fetched ahead; keys typed before the box opens queued for it; showing a line-ending change
  (now one CR is dropped from each line).
- Terminals (Phase 20): a terminal program of the user's choice (any command line, a Windows Terminal profile); the
  region or the file's name handed to the AI tool; looking for the AI command before the window opens (cmd says it is
  missing inside the new window now).
- The soak's watch rule (found in Phase 22; its folder is emptied at its start since then, so a seed is one run): it
  counts a buffer opened late in a new folder (live state) as LEAK; it could count only the watches the open buffers'
  folders do not explain. (Its memory rule judges our arenas since Phase 22: Decisions, Phase 22, 8.)
- README's git changes picture (Phase 22): a screenshot run cannot start the real git (Phase 20's guard refuses it in
  every batch run), so docs/images/git-changes.png is Phase 19's; a screenshot flag that chooses the real git, as smoke
  stage 12 does, would let it be retaken.
- A hard cap on a search's held records (Phase 17): the claim stop at RESULTS_HELD_MAX does not bound them (files
  already claimed still add up to search_max_results records each); stop draining or pause the workers above a
  cap, and assert it.
