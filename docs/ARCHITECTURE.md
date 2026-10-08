# teal architecture

## Module map

| file | role |
|---|---|
| `src/teal.c` | the unity translation unit; includes everything below except the .cpp, in order |
| `src/base.h/.c` | types, ASSERT, Arena, String8/16, UTF-8<->UTF-16, clipboard text conversion (CRLF), letter case, mini formatter, dev LOG |
| `src/platform.h` | os_* primitives, Key/Event/FrameInput, app entry points — all the core sees of the OS |
| `src/render.h` | Rect, Color, r_* API (rects, glyphs, atlas) — all the core sees of the GPU |
| `src/font_backend.h` | extern "C" glyph rasterization API, no Win32 / DWrite types |
| `src/win32_dwrite.cpp` | the only C++ file: DirectWrite calls behind font_backend.h, nothing else |
| `src/font.h/.c` | metrics, CPU glyph atlas (shelf packer), glyph cache, `font_draw_text` |
| `src/buffer.h/.c` | gap buffer, incremental newline index, `buffer_replace`, the undo log (groups, state ids, undo / redo, limit), markers, language from the extension, file load (encodings, line endings) and save |
| `src/syntax.h/.c` | token kinds, the line lexer interface, keyword tables, the catch-up of line states (budget, convergence), bracket matching and line summaries on tokens (indentation, show-paren) |
| `src/lex_c.c`, `lex_jai.c`, `lex_cs.c`, `lex_js.c` | the lexers: C and C++ (one), Jai, C#, JavaScript and TypeScript (one) |
| `src/command.h/.c` | `Command` and `CommandContext`; the table of every command, lookup by Emacs name |
| `src/view.h/.c` | headless view logic: visual columns, View (cursors, scroll), the command driver `view_run_command` (undo boundaries, shift-select, region rules, kill appending, clipboard), mark and region, motion and basic editing commands, the buffer list (`view_switch_buffer`), echo messages and their *Messages* log |
| `src/edit.h/.c` | the kill ring (shared arena, large entries, clipboard link), undo / undo-redo, kill and yank commands, rule-based indentation and its commands, the other editing commands (open-line, whitespace, transpose, case, comment-line) |
| `src/keymap.h/.c` | chords, kbd notation, chords from key events, keymaps, the key sequence state machine; dev: `--keys` events |
| `src/config.h/.c` | the config parser (settings, colors, keys), defaults + user file layering, diagnostics, the reload state machine (`config_poll`) |
| `src/config_default.h` | the built-in configuration (the same format as teal.conf), embedded as a C string |
| `src/minibuffer.h/.c` | the matcher (folding, terms, exact / prefix / substring ranking, narrowing); the minibuffer: a one-line View on its own Buffer, prompt kinds, continuations and chains, abort, history, candidates and filtering, the minibuffer commands, M-x, goto-line |
| `src/search.h/.c` | the search engine: literal text, forward and backward, exact or folded, over the gap buffer's two segments in place, resumable with a budget of positions, SSE2 scan for the first byte |
| `src/isearch.h/.c` | isearch (a stack of steps resolved by sliced searches, its commands, the echo prompt), query-replace and replace-string (the prompts, the session, its answers and replace-all across frames, case conversion) |
| `src/app.c` | editor core: config (read before the font, reloaded live), keys through the keymap stack (the isearch and query-replace routing), buffers (unique names), the kill ring, app commands (buffer cycling, open/reload config, text scale, describe-key, quoted-insert), `app_update` (events, commands, the frame's background work: search slices then lexer states, layout) and drawing (views, region, the search highlighting, the minibuffer line, the search echo line, the candidate list), window title, mouse (click, drag, double / triple click); dev: smoke probes, the headless app of `--test`, `--bench-edit` memory, `--bench-complete` candidates, `--bench-search` helpers |
| `src/files.c` | part of the app (included after app.c): find-file, write-file, save-buffer, switch-to-buffer, kill-buffer, revert-buffer, save-some-buffers and quitting, the end of the Windows session, files changed on disk (checks, watches, the save guard) |
| `src/test.c` | dev only: `--test` (buffer, marker, column, view, key, config, buffer list and hot reload tests, file round trips, failures, the steps of docs/MANUAL_TESTS.md through the headless app) and the `--bench-buffer` core |
| `src/png.c` | dev-only PNG encoder (stored deflate, CRC32, Adler-32) |
| `src/render_d3d11.c` | D3D11 device, flip-model swap chain, instanced-quad pipeline, atlas texture, capture |
| `src/shaders/quad.hlsl` | vs/ps for the quad pipeline, compiled by fxc to `build/gen/*.h` |
| `src/win32_main.c` | wWinMain, window, message loop (with directory watches), input translation (click counts), clipboard (and the dev fake), directory listing, the end-of-session messages, os_* implementation, dev flags and helpers (`--log-keys`, `--idle-check`, `--dpi-check`, `--clipboard-check`) |
| `src/win32_input_test.c` | dev only, in `--test`: the real input path (posted key messages, TranslateMessage, the window procedure) on US, United Kingdom, Turkish Q and Finnish layouts loaded for its thread, into a headless app |
| `res/teal.manifest` | PerMonitorV2 DPI, longPathAware, supportedOS Windows 10 |

Core (`app.c`, `files.c`, `font.c`, `buffer.c`, `view.c`, `edit.c`, `minibuffer.c`, `command.c`, `keymap.c`, `config.c`, `test.c`)
includes only `platform.h` and `render.h`; `font.c` additionally calls `font_backend.h`.

## Startup and frame loop

1. `wWinMain` parses the command line (`--test` runs headless here and exits), then allocates
   the Renderer and immediately starts a worker thread that runs only
   `D3D11CreateDevice` (`r_create_device`: no logging, no arenas, results go into Renderer
   fields). Meanwhile the main thread creates the window hidden on the monitor under the
   mouse, sizes it for that monitor's DPI (clamped to the work area) and creates the app,
   which reads the config (built-in defaults, then the user's teal.conf), opens DirectWrite
   once with the configured font and pre-rasterizes ASCII. Then it joins the worker, logs its
   results, and creates swap chain, pipeline and atlas texture (`r_finish_create`).
2. Shows the window cloaked (DWMWA_CLOAK) and without activation (SW_SHOWNA; the first frame
   renders on its WM_SIZE), uncloaks, then activates it (SetForegroundWindow): no white flash, and
   the focus, with its IME and text services setup (~10 ms), comes after the first frame is on screen.
3. Loop: if no redraw is pending, block in `MsgWaitForMultipleObjectsEx` on the messages and the
   directory watches, with no timeout unless the app asks for one (`app_wait_ms`: only while a
   config read is to be retried). Signalled watches become `EVENT_DIR_CHANGED`. Drain every queued
   message; the window procedure turns them into `Event`s in a fixed array (consecutive
   mouse-move and resize events are coalesced). If the array is nearly full, run a frame
   mid-drain instead of dropping anything. If anything requested a redraw, run exactly one
   frame: `app_update_and_render` → `font_frame_begin`, `r_begin_frame`, `r_push_rect` /
   `font_draw_text`…, `r_end_frame` (flush + `Present(1, 0)`). Scratch resets after each frame.
   Background work has one deadline per frame (2 ms): search slices first (isearch, query-replace,
   replace-all), then lexer states with what is left; while either is pending the app asks for
   another frame, otherwise none.
4. `WM_SIZE` resizes the swap chain and renders immediately, so live resize stays crisp
   inside the modal size loop. Minimized = frames are no-ops.

Renderer: one pipeline, instanced quads. VS builds the strip corners from `SV_VertexID`;
the only vertex buffer is a dynamic per-instance buffer (rect, uv rect, RGBA8 color, kind;
40 bytes, 65536 per flush). Kind 0 = solid, kind 1 = glyph: the PS fetches coverage with
`Texture2D.Load` (quads are 1:1 with texels, no sampler) and outputs color in SV_Target0 and
per-channel weights in SV_Target1 (dual-source blending, SRC1_COLOR / INV_SRC1_COLOR). Each
flush first uploads the atlas dirty rectangle. Pipeline state is rebound every frame because
the flip model unbinds the back buffer on Present.

Text: `font_draw_text` walks UTF-8; a byte in 32..126 is a table lookup plus one instance
write, anything else goes through an open-addressing hash keyed by codepoint and is
rasterized on first use (DirectWrite, white on black, into a GDI DIB; copied as R, G, B
coverage into the CPU atlas). When the atlas or the hash table (load factor > 70%) fills up,
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
Every key but the modifier and lock keys sends a KEY_DOWN, with KEY_NONE when teal has no Key
for it (keypad digits and operators, VK_OEM_8, media keys): it ends the previous chord's text,
and makes a chord when it types a character (C-? on Turkish Q's VK_OEM_8); the keypad carries no
character, so it makes no chords and Alt + keypad digits still enter a character code. A KEY_DOWN
also says whether the key is dead and whether no text event follows (TranslateMessage has queued
its characters before the window procedure runs), so describe-key can report a key press that is
no chord; MOD_ALTGR marks AltGr, for information only. After a Ctrl or Alt chord a pending
dead-key accent is consumed: TranslateMessage stored it (M-^ where ^ is dead) and the next key
would compose with it. winuser.h's MOD_ALT / MOD_SHIFT (RegisterHotKey) are undefined in
win32_main.c: in the unity build MOD_ALT would equal MOD_CTRL there (Alt read as Ctrl).

Keys: KEY_DOWN and text events go through the key sequence state machine (keymap.c) with the
keymap stack; the result is a command run through `view_run_command`, a prefix shown at once
("C-x-"), an undefined sequence, or a description (describe-key).

## Measurements (Phase 3)

`build\teal_bench.exe` (/O2, dev flags, no debug layer), 1280x800 client, Consolas 16 px.
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

`build\teal_bench.exe`, same machine and file. `--bench-view`, one event per frame through
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

`build\teal_bench.exe`, same machine (2560x1440 at 144 Hz), 1280x800 client.

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
  defaults plus a 4.8 KB teal.conf at startup 140-230 us (mostly opening the file).
- Startup (`--startup-ms`, release, 10 runs alternating): 189 ms median before Phase 5, 190 ms
  without a teal.conf, 191 ms with a full one. The font is set up exactly once.
- Idle with the config directory watched: 0 ms CPU over 10 s.
- Release exe 188,928 bytes (156,672 after Phase 4); the same six imports (kernel32 adds
  GetEnvironmentVariableW, Find*ChangeNotification, CreateDirectoryW; user32 adds ToUnicodeEx,
  GetKeyboardState, GetKeyboardLayout).

## Measurements (Phase 6)

`build\teal_bench.exe`, same machine; the release build for the memory figures.

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

`build\teal_bench.exe`, same machine (2560x1440 at 144 Hz), 1280x800 client; the release build for
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

`build\teal_bench.exe --bench-syntax`, same machine (2560x1440 at 144 Hz), ~100 MB of generated source per language:

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
- [ ] 10. Window splitting
- [ ] 11. Project + fuzzy file open
- [ ] 12. Project-wide search
- [ ] 13. Build + jump to error
- [ ] 14. Multiple cursors
- [ ] 15. Performance and size pass (including an optional CRT-free build)

## Decisions

- Swap chain B8G8R8A8_UNORM, not sRGB: theme colors reach the screen bit-exact.
- Flip model (FLIP_DISCARD, 2 buffers, SCALING_NONE), max frame latency 1, vsync Present.
- First frame is presented while the window is DWM-cloaked, then uncloaked.
- `/external:anglebrackets /external:W0`: SDK headers do not break our /W4 /WX.
- Import list kept minimal for startup: kernel32, user32, gdi32, d3d11, dwmapi, dwrite (dxgi
  only in dev builds; dxguid.lib is static). No shell32 (own command-line parser), no shcore.
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
- Saving writes a temp file `<name>.teal~N` in the same directory, streaming through a fixed
  64 KB chunk when converting (no second copy of the text), flushes it (FlushFileBuffers), then
  swaps it in with ReplaceFileW (attributes, ACLs, creation time preserved; MoveFileExW when
  the target does not exist). It writes in place instead (also flushed) when the target is a
  symlink or has several hard links, when the temp file cannot be created, or when
  ReplaceFileW fails. A read-only target is refused before anything is written.
- Paths are stored as full, normalized UTF-8 paths (GetFullPathNameW). Opening a path that
  does not exist gives an empty buffer visiting it, "(New file)", as in Emacs.
- Every file failure returns an `OsFileStatus` the caller turns into a minibuffer message.
- `TEAL_D3D_DEBUG` (defaults to TEAL_DEV) controls the D3D11 debug layer, so `build.bat bench`
  is an optimized dev build without it.

### Views, cursors, markers (Phase 4)

- A View is an Emacs window: one buffer, an array of cursors, a scroll position (a top-of-window
  marker and a left column) and a pixel rect. Layout hands each View its rect (for now: equal
  side-by-side columns above the echo area, each with its own mode line); nothing assumes a
  single View. Phase 10 adds splitting.
- Cursors are an array from the start. Every motion and edit command is written for a single
  cursor (its context carries the View and that Cursor). `view_run_command` is the only place
  that loops over cursors, and it does the work after the loop (keep point visible, update
  `last_command`). Commands that act on the View as a whole (scroll-up-command,
  scroll-down-command, recenter-top-bottom, save-buffer) are flagged COMMAND_ONCE and run once
  with the primary cursor. Phase 14 adds cursor creation, merging and ordered edits to that
  driver only.
- Markers: every position that must survive edits (each cursor's point and mark, each View's
  top of window) is a marker registered with its buffer, and `buffer_replace` is the only
  thing that adjusts positions. For a replace of [start, end) by len bytes: a marker before
  start stays; after end it shifts; exactly at end (of a non-empty range) it goes to
  start + len; inside the range, or at start, it goes to start, or to start + len if it is an
  advancing marker (insertion type: point markers advance, the top-of-window marker does not).
  Markers within 3 bytes of the edit are then snapped back to a character boundary, since
  joined invalid bytes can form a valid sequence. Storage: a flat slot array with a free list
  in its own reservation, one linear pass per edit.
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
- Command line: `teal [+LINE[:COLUMN]] [file]`, LINE and COLUMN 1-based as in Emacs (COLUMN is a
  visual column); the mode line shows the column 0-based. Any unrecognized argument starting
  with "--" exits immediately with code 10, before a window exists.
- The window title is "<buffer name> - teal", set only when it changes.
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
- Location: teal.conf next to the exe if it exists (portable), otherwise
  %APPDATA%\teal\teal.conf (APPDATA from the environment, no shell32). Startup never creates a
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
  prepends; all cursors of one command share it), COMMAND_MERGE_INSERT / MERGE_DELETE (undo
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
  (Emacs' uniquify renames both; teal only the newcomer). switch-to-buffer offers buffers most
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

## Later

- Waitable swap chain (DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) folded into the
  message wait, for lower input latency (Phase 15 candidate).
- Startup is ~175 ms, of which ~125-150 ms is waiting for `D3D11CreateDevice` (driver load) even
  on its own thread; everything else of ours already overlaps it (Phase 9 measurements). Accepted.
  To make the launch feel instant: show the window immediately with the background color painted
  (GDI) until the first Present. The 12-15 ms before WinMain (the loader) could shrink by loading
  d3d11.dll on the device thread (LoadLibrary) instead of as an import.
- Legacy code pages (Windows-1254, Latin-1, ...) and BOM-less UTF-16 detection.
- ESC as the Meta prefix, as in Emacs (ESC is keyboard-quit for now).
- *Messages*: collapse a repeated message into "msg [2 times]", as Emacs does.
- Chords on Turkish Q: Ctrl+Shift+ı gives C-S-i (default case mapping); a layout-aware letter
  case would make it C-S-ı.
- Wide (East Asian) characters and emoji occupying two cells; color emoji.
- System font fallback for codepoints Consolas lacks.
- Bold / italic faces, if a theme ever wants them.
- Linear-space (gamma-corrected) ClearType blending for dark text on light backgrounds.
- Apply multi-cursor edits in one ordered pass so the gap moves monotonically (random-position
  edits on a 100 MB file cost ~2.3 ms each; Phase 14).
- Line wrapping (visual lines).
- Cursor blink, smooth scrolling, line numbers: config candidates (each needs a timer or
  more layout).
- Column cache for very long lines: point's column is found by scanning from the line start
  (each view_ensure_visible and the drawing scan it; printable ASCII 16 bytes at a time since
  Phase 9, other text byte by byte).
- Prefix arguments (C-u), the mark ring, rectangle commands, overwrite mode.
- Clipboard delayed rendering (a 50 MB kill is converted to UTF-16 at once).
- Auto-scroll while dragging outside the window; after a double or triple click, a drag that
  extends by words or lines.
- Multiple cursors and undo: undo restores only the primary cursor's point (Phase 14).
- Auto-save and crash recovery.
- Writing to a path another buffer visits (Emacs asks first; teal then has two buffers on it).
- Clicking a candidate in the list; dimming the shadowed part of a file name ("c:/a/~/b").
- Candidates' bindings from the minibuffer keymap in M-x annotations (only global ones are shown).
- A change on disk within the same file-time tick at the same size goes unseen (a content hash on
  activation would catch it, at the cost of reading every file).
- Slow directory listings (network shares) block the UI while find-file lists a directory.
- Revert: map positions through a line diff of the old and new text, so a marker in an unchanged
  line that moved keeps its place in it (now: line and column inside the replaced range).
- JSX tags in JavaScript / TypeScript highlighting.
- Resumable lexing inside very long lines (a 5 MB minified line costs ~10 ms per keystroke).
- Regular-expression search (isearch-forward-regexp, query-replace-regexp).
- A total match count in isearch ("3/41").
- Editing the search string in the minibuffer (M-e in isearch).
- occur.
- Queueing the keys typed while query-replace searches for its next match on a huge buffer (now
  ignored beyond one slice).
- Turkish-aware case folding for search (İ / i, I / ı).
- A second-byte filter for folded searches whose first letter is common (now ~2.6 GB/s against
  ~25 GB/s for a rare first byte).
