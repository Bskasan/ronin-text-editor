# Manual test list

Every step a person would check by hand. Each step has one status:

- **AUTOMATED**: a test that existed before this list covers it.
- **AUTOMATED-NEW**: a test was added for it when the list was written (through the real input
  harness, `--keys`, the headless app, a smoke probe, a bench check or a dev helper).
- **FIXED**: it was broken. The row names the failing test and the commit that fixed it.
- **HUMAN**: only a person can judge it (visual quality, the real clipboard, Task Manager, a DPI
  change, Windows logoff, real focus). The row says why. The steps are under "HUMAN steps" at the end.

"Note" marks a place where the step's wording and what ronin does differ by design (Emacs
behavior); nothing there is a bug.

Every phase appends its own steps here, with the same statuses (see CLAUDE.md).

Paths under `build/` (screenshots in build/shots, build\ronin.log, build\tmp) are local output of the
dev builds; they are not part of the repository.

## Where the evidence is

| Name used below | What it is |
|---|---|
| `--test` | `build/ronin_debug.exe --test` (or ronin_bench.exe): src/test.c, then `win32_dev_test_mods`, `win32_dev_test_keys` and the input harness. Each group logs a `test: ok: ...` line to build\ronin.log. |
| harness | src/win32_input_test.c, inside `--test`: real key messages (scan codes, flags, AltGr, TranslateMessage, ToUnicodeEx) to a window that is never shown, using the real window procedure, on US, United Kingdom, Turkish Q and Finnish layouts loaded for the test thread only. Log: `test: ok: input <layout> ...`. |
| smoke | `build/ronin_debug.exe --smoke`: pixel probes on rendered frames. Log: `smoke: ok: ...`. |
| manual tests | test.c `test_manual_*`: the steps of this list through the headless app. Log: `test: ok: manual <ids> ...`. |
| `--idle-check` | Shows the window without focus. After 1 s it measures 5 s of idle: CPU time, frames, private bytes. |
| `--dpi-check <dir>` | WM_DPICHANGED to 125% and back, with a screenshot at each step. Checks the scaling, the font set-ups, and that the frame after coming back is identical to the first. |
| `--clipboard-check` | The real clipboard, only when it holds plain text or nothing. It saves the text, puts it back, and keeps everything out of clipboard history. |
| `--log-keys` | Every keyboard message, what ronin read from it, and the command it ran. |
| `--type-ahead-check` | Real input (SendInput, letters and Shift only), run on demand only: types five keys while ronin starts and checks they arrived in order before the first frame. |
| `--bench-windows` | Frames with one and four windows, typing with one and two windows, on the 100 MB file. |
| `--bench-project` | Fuzzy ranking on 10,000 / 100,000 / 1,000,000 synthetic paths (frames, the longest frame, RET's whole ranking), scans of the generated tree and of `C:\Windows`, typing with and without a scan running. Never takes the focus. |
| `--bench-grep` | The project search on ~100 MB in 3,000 generated files with 1, 2, 4 and 8 workers (first result, total, MB/s, the longest frame and its results work, memory), typing while 8 workers search, the workers' priority. Never takes the focus. |
| search tests | The `--test` groups logged as "test: ok: grep file", "grep search", "grep sharing", "results" and "occur" (Phase 12). |
| `--child <script>` | Dev builds: the scripted program the build tests run (lines on stdout and stderr, raw bytes, sleeps, an exit code, a hang, a grandchild, a flood, the working directory, a file rewritten). |
| `--bench-compile` | A `--child` program printing 100 MB into *compilation* (with the cap raised and at the default 16 MB: time, frames, the longest frame, memory), typing at line 1,000,000 of the 100 MB file while builds print. Never takes the focus. |
| build tests | The `--test` groups logged as "test: ok: processes", "compile matchers", "compile path resolution", "build settings and colors", "compile cleaning", "builds", "build kills", "builds in the app" and "the landing highlight" (Phase 13). |
| `--bench-cursors` | Typing (self-insert, DEL, RET) at 1, 10, 100, 1,000 and 10,000 cursors on the 100 MB file (consecutive lines and spread) and on src/app.c; the language-aware classes on a C file at 1,000 and 10,000 cursors with the batch flushes per command; the undo of a 10,000-cursor edit; mark-all-like-this on a million occurrences and over the whole 100 MB file; the memory of 10,000 and 100,000 cursors. Never takes the focus. |
| cursor tests | The `--test` groups logged as "test: ok: marker batches", "the multiple-cursor driver", "multiple cursors collapsing", "multiple-cursor undo", "multiple-cursor kills and yanks", "multiple-cursor commands", "multiple-cursor commands in the app", "multiple cursors and the mouse" and "multiple-cursor differential fuzz" (Phase 14). |
| project tests | The `--test` groups logged as "test: ok: threads", "tree watch", "junctions", "ignore rules", "project roots", ".ronin-project", "scanner", "fuzzy", "project finder" and "the region in windows". |
| `--fuzz-seeds` / `--soak` | Phase 15, in `verify.bat phase`: every seeded `--test` group for seeds 1-256 (worker processes); a million random events through the headless app with invariants, settled memory and a typing probe checked as it goes. |
| crash test | The `--test` group logged as "test: ok: crash recovery" (Phase 15): a child crashing on purpose after edits (`--crash`), its recovery files against a save, the notice, recover-file. |
| `--startup-capture` / `--bench-latency` / `--bench-longline` / `--memory-report` | Phase 15 measurements, never taking the focus: the early window on screen after each composition; key arrival to Present and to the vertical blank that showed it; a 5 MB line; private bytes by owner. |
| `--leak-check` | Phase 16, debug build, in `verify.bat phase`: every scenario repeated after a prime step and 20 warm-up cycles, 4 windows of 50 cycles, one table a scenario in build\ronin.log (LEAK exits 6); the window never shown. The "exit-check:" block ends every dev exit with a window. |
| `--fake-git DIR ARGS` | Phase 19, dev builds: the tests' git. It appends its arguments to DIR\calls.txt and runs the `--child` script DIR\map.txt names for the first key found in them. |
| git tests | The `--test` groups logged as "test: ok: programs", "git parse", "git job", "git changes", "git errors", "git real" (or "test: skip: git real" without git on PATH) and the seeded "git fuzz" (Phase 19). |
| `--bench-git` | Phase 19: git's status and diff parsed from memory (5,000 files, 50,000 lines), and a 20,000-line diff copied into the open view: frames, the longest slice and frame build. Never takes the focus. |
| `--fake-terminal DIR ...` | Phase 20, dev builds: the tests' terminal. It appends its working directory, the program and console it stands for and its command line exactly as it came to DIR\calls.txt, then exits. |
| terminal tests | The `--test` groups logged as "test: ok: launch", "terminal config" and "terminal" (Phase 20), smoke stage 13 ("smoke: ok: terminal"), `--leak-check terminal-cycle` and the soak's "soak: terminals" line. |
| verify checks | `verify.bat`'s steps "check-imports" (the release exe's DLLs equal res\imports.txt, no CRT DLL) and "version" (`ronin.exe --version` prints "ronin X.Y.Z"). |
| shield | The input tests' protection against real input (`DevShield` in win32_main.c, its self-test "test: ok: input shield"): a case whose injected key state did not hold is logged as "test: inconclusive: ..." with the reason and repeated (at most 5 attempts, then "key state unstable"); a wrong result with the key state held is a failure. |

## A. Startup and window

| Step | What | Status | Evidence |
|---|---|---|---|
| A1 | Start from Explorer: no white flash; title "*scratch* - ronin" | HUMAN | The title is AUTOMATED: smoke `win32_smoke_check_title` ("window title '*scratch* - ronin', set 1 time(s)"). The flash needs eyes: the window is shown cloaked until the first Present. |
| A2 | Type as soon as the window is visible: characters go to ronin | AUTOMATED-NEW | `--type-ahead-check` (Phase 10, on demand): keys typed while ronin starts, even before its window is visible, arrive in order. |
| A3 | Same from a terminal and from the taskbar | HUMAN | Needs the shell. |
| A4 | Idle with the mouse outside: 0% CPU | AUTOMATED-NEW | `--idle-check`: "PASS: 5000 ms idle: CPU 0 us (0.00% of one core), 0 frame(s), 0 main loop pass(es)". Run on demand. |
| A5 | Drag-resize: crisp, no stretching, no black areas | HUMAN | Visual, during the modal size loop. |
| A6 | Minimize, restore, maximize | HUMAN | Window manager interaction. |
| A7 | Alt alone, F10, Alt+Space: no system menu, no beep | AUTOMATED-NEW | harness, every layout: "Alt alone, F10, Alt + Space": 0 messages left to DefWindowProc (the only source of the menu and the beep), 0 SC_KEYMENU. F10 describes as `<f10>`, Alt+Space as `M-SPC`. |
| A8 | Another app: hollow cursor; back: filled | HUMAN | Real focus. The drawing is AUTOMATED: smoke stage 0 "buffer: hollow cursor", stage 1 "buffer: filled cursor". |
| A9 | Display scale 125% and back: crisp at both | AUTOMATED-NEW | `--dpi-check build/shots/dpi`: "PASS: 96 -> 120 -> 96 dpi: client 1280x800 -> 1604x1010 -> 1280x800, font set up again at each change, the frame after coming back equals the first". The 100% and 125% screenshots were inspected: crisp. A real monitor change is still worth one look. |
| A10 | Alt+F4 with nothing modified: exits without asking | AUTOMATED-NEW | harness: Alt+F4's key is the one message ronin leaves to DefWindowProc, and SC_CLOSE makes the unmodified app quit. Windows only turns a *posted* Alt+F4 into SC_CLOSE for an active window, so the harness sends SC_CLOSE itself. |

## B. Keyboard and text

| Step | What | Status | Evidence |
|---|---|---|---|
| B1 | Type ğüşıöç ĞÜŞİÖÇ | AUTOMATED-NEW | harness Turkish Q: "12 of 12 Turkish letters typed from their keys". Pasting is AUTOMATED: `test_clipboard` round trip with ş. |
| B2 | { } [ ] \ \| @ # $ ~ each insert one character | AUTOMATED-NEW | harness, all four layouts: "symbols typed from their keys" (AltGr, and a dead ~ followed by Space on Finnish). |
| B3 | Dead key then letter: composed character, no command | AUTOMATED-NEW | harness Turkish Q and Finnish: "dead key then e" for ´ ` ^ ¨ gives é è ê ë. "e right after Left Alt + dead key" gives e: this found a bug (a dead-key accent left pending after a Meta chord), fixed in 051b340. |
| B4 | Key repeat is smooth | HUMAN | Feel. Repeats are ordinary KEY_DOWNs (`repeat` flag). Typing frame cost: `--bench-view` "self-insert at line 1,000,000: build avg 7 us". |
| B5 | C-h k C-x C-s: "C-x C-s runs the command save-buffer" | AUTOMATED-NEW | harness: every default binding is described through the real keys with exactly "<keys> runs the command <name>". Before: `test_key_input` "keys: describe C-x C-s" (keymap only). |
| B6 | C-h k with C-/, M-<, M->, M-{, M-}, M-%: each bound | FIXED | Failing test: `win32_dev_test_mods` "Left Alt + f -> M-f: got C-f", "Alt + Shift + , -> M-<: got C-<". Fixed in 11a98be. Covered since by the harness (all 124 bindings reachable on US, also on UK, Turkish Q and Finnish). |
| B7 | C-x C-g: "Quit"; C-x q: "C-x q is undefined" | AUTOMATED-NEW | manual tests B7 (`test_manual_editing`). |
| B8 | C-x C-+, C-x C--, C-x C-0, Ctrl+wheel: text scale changes, crisp | AUTOMATED-NEW | smoke "text scale: cell 9x19, C-x C-+ 11x23, C-x C-0 9x19, C-x C-- 7x16, Ctrl+wheel 9x19, 4 font set-up(s)". Crispness was inspected on a screenshot at +2 steps (build/shots/b8_scale2.png). |
| B9 | Yank an emoji or a CJK character: box, no crash | AUTOMATED-NEW | manual tests B9 (yanked into the buffer). Screenshot build/shots/b9.png inspected: one box per character, the rest of the line in place. |

## C. Files

| Step | What | Status | Evidence |
|---|---|---|---|
| C1 | Mode line: name, percent, L1 C0, (C), encoding, line endings | AUTOMATED-NEW | manual tests C1: " -:---  m.c ... Top   L1 C0    (C)    UTF-8 CRLF", and a percent after M-g g 150. **Note:** at the top the position reads "Top" (and "Bot" / "All"), not a percent (Emacs). |
| C2 | ronin +120:8 file: line 120 centered, L120 C7 | AUTOMATED-NEW | manual tests C2: "L120 C7", the top line within 1 of 119 − rows/2. |
| C3 | Nonexistent path: empty buffer, "(New file)" | AUTOMATED-NEW | manual tests C3 (from the command line). find-file is AUTOMATED: `test_find_write` "(New file)". |
| C4 | Directory path: error message, *scratch* stays | AUTOMATED-NEW | manual tests C4. **Note (Phase 11):** a directory on the command line is now the project (`ronin <directory>`), so there is no error message; *scratch* stays and starts there (the test checks exactly that; step M13). |
| C5 | Edit: "**"; C-x C-s: "Wrote ...", "**" gone | AUTOMATED-NEW | manual tests C5. |
| C6 | CRLF file: edit one line, save: still CRLF, only that line differs | AUTOMATED | `test_edits` edit_crlf.txt (whole-file byte compare); "test: ok: load, edit, save, reload". |
| C7 | Windows-1254 file: boxes; insert ASCII, save: other bytes unchanged | AUTOMATED-NEW | manual tests C7 (the bytes FE FD F0 DE DD D0 kept byte for byte, CRLF kept). Boxes inspected on build/shots/c7.png. |
| C8 | Read-only file: "%%"; typing: "Buffer is read-only", no sound | AUTOMATED-NEW | manual tests C8. **Note:** the message is "Buffer is read-only: ro.txt" (with the buffer name, as Emacs). No sound: ronin never calls MessageBeep, and nothing it gets reaches DefWindowProc's beep (A7). |
| C9 | 100 MB file opens at once; M-> immediate; wheel smooth | HUMAN | The smoothness is a feel. Measured: `--bench-buffer` "load: 31 ms for 104857642 bytes"; `--bench-view` "end-of-buffer (C-End): build avg 13 us, worst 46 us". |

## D. Movement

| Step | What | Status | Evidence |
|---|---|---|---|
| D1 | Arrows, C-f C-b C-n C-p | AUTOMATED | `test_motions` ("motion table, 53 cases"); the keys: harness bindings. |
| D2 | C-a, C-e, Home, End | AUTOMATED | `test_motions`; harness: numpad 7 gives `<home>`. |
| D3 | M-f, M-b, C-<left>, C-<right>; stop at the underscore in foo_bar | AUTOMATED | `test_motions` "\|foo_bar baz" M-f gives "foo\|_bar baz". |
| D4 | Goal column kept through shorter lines | AUTOMATED | `test_motions` (N N over a short line). |
| D5 | M-<, M->, C-<home>, C-<end> | AUTOMATED | `test_motions`; keys: harness. |
| D6 | C-v, M-v, <next>, <prior>; "End of buffer" / "Beginning of buffer" | AUTOMATED | `test_scrolling` messages. |
| D7 | C-l three times: center, top, bottom | AUTOMATED | `test_scrolling` recenter-top-bottom: tops 45, 50, 41. |
| D8 | C-<up>, C-<down>, M-{, M-}: paragraphs | AUTOMATED | `test_motions` paragraph cases; keys: harness. |
| D9 | Long line: C-e scrolls horizontally; C-a returns | AUTOMATED-NEW | manual tests D9 (left column > 0, then 0). |
| D10 | Tabs: one step across a tab; C jumps by 4 | AUTOMATED-NEW | manual tests D10 ("L1 C4" after C-f, "L1 C0" after C-b); drawing: smoke stage 0 tab probes. |
| D11 | Click sets point; the wheel scrolls and drags point | AUTOMATED-NEW | manual tests D11: 5 notches give top line 15 with point inside; a click sets point to the cell. |
| D12 | M-g g 50 RET; M-g g 50:10 RET | AUTOMATED | `test_goto_line` (N, N:M). |

## E. Editing

| Step | What | Status | Evidence |
|---|---|---|---|
| E1 | Typing, DEL, <delete>, C-d, RET | AUTOMATED | `test_multi_cursor`, `test_headless_app`. |
| E2 | C-o, C-t, M-u, M-l, M-c (also Turkish letters) | AUTOMATED | `test_edit_commands` (34 cases, ıışß and ŞĞÜ). **Note:** cases use Unicode's default mapping: i becomes I, not İ. |
| E3 | M-^, M-\, M-SPC | AUTOMATED | `test_edit_commands`; Alt+Space gives M-SPC in the harness. |
| E4 | M-; toggles "// "; on a region every line | AUTOMATED | `test_edit_commands` comment-line cases. **Note:** M-; moves to the next line (Emacs), so a second M-; comments the next line rather than toggling back; blank lines in a region stay blank. |
| E5 | C-SPC then movement highlights; C-g deactivates | AUTOMATED | `test_region` ("C-g deactivates"); smoke stage 2 (selection color). |
| E6 | Shift+arrows select; unshifted deselects | AUTOMATED | `test_region` shift-select table. |
| E7 | C-x h; C-x C-x | AUTOMATED | `test_region`. |
| E8 | Typing with a region inserts and keeps its text; DEL deletes it | AUTOMATED | `test_region` ("typing inserts and deactivates", "DEL deletes the active region"), with the default delete_selection_mode = false. |
| E9 | Drag selects; double click a word; triple click a line | AUTOMATED-NEW | manual tests E9. **Note:** with underscore_is_word = false a double click on foo_bar selects foo. |
| E10 | C-k to end of line; again kills the newline | AUTOMATED | `test_kill` kill-line cases. |
| E11 | Three C-k, then C-y elsewhere: one piece | AUTOMATED-NEW | manual tests E11. |
| E12 | M-d, M-DEL, C-w, M-w, C-y | AUTOMATED | `test_kill`. |
| E13 | C-y then M-y cycles | AUTOMATED | `test_kill` yank-pop. |
| E14 | M-w lines, paste in Notepad: line breaks right | HUMAN | Needs Notepad. Most of it: `--clipboard-check` (kill-ring-save of two lines puts CRLF lines on the real clipboard). It is skipped while the clipboard holds anything but plain text. |
| E15 | Copy in a browser, C-y in ronin | HUMAN | Needs a browser. Most of it: `--clipboard-check` (CRLF text put there as by another program yanks as LF lines); fake clipboard: `test_kill` "clipboard: yanked with LF". |
| E16 | Type, C-x u / C-/ repeatedly: undone step by step | AUTOMATED | `test_undo_commands`. **Note:** typing is undone in runs of up to 20 characters (Emacs), not word by word. |
| E17 | After undoing, an arrow, then C-x u: the text returns | AUTOMATED | `test_undo_commands` "after another command undo undoes the undos". |
| E18 | Type, C-x u, C-?: redone | AUTOMATED | `test_undo_commands` undo-redo; C-? reachable: harness. |
| E19 | Undo back to the saved state: "**" clears | AUTOMATED | `test_undo_buffer` "modified follows the saved state". |

## F. Indentation

| Step | What | Status | Evidence |
|---|---|---|---|
| F1 | C nesting; "}" dedents when typed | AUTOMATED | `test_indent` rule rows and "a closing brace typed first reindents the line"; the smoke's typed session (SMOKE_SESSION_KEYS). |
| F2 | if without braces, then back | AUTOMATED | `test_indent` "C: a brace-less if followed by else". |
| F3 | switch: labels one in, bodies two; ":" jumps | AUTOMATED | `test_indent` typed switch; `test_electric_labels` "C case". Note: needs a C file (Fundamental has no electric labels). |
| F4 | foo(a, / b); / bar(); | AUTOMATED | `test_indent` "continued call arguments return to the call's line". |
| F5 | JS callback f((x) => { ... }); | AUTOMATED | `test_indent` "JavaScript: a callback". |
| F6 | s = "{"; RET: next line not indented | AUTOMATED | `test_indent` "brackets in strings, chars and comments do not count". |
| F7 | Jai if x == { case ...; } | AUTOMATED | `test_indent` "Jai: if x == { case ...; }"; `test_electric_labels` "Jai case". |
| F8 | Region, TAB: fixed | AUTOMATED | `test_indent` "TAB over a region". |
| F9 | <backtab> one level less; M-i next stop | AUTOMATED | `test_indent` "backtab", "M-i to the next stop". |
| F10 | Tab-indented file: RET indents with tabs | AUTOMATED-NEW | manual tests F10. Note: tabs are used where the indentation is a multiple of tab_width (both 4 by default). |
| F11 | C-q TAB inserts a tab | AUTOMATED-NEW | manual tests F11. |

## G. Highlighting

| Step | What | Status | Evidence |
|---|---|---|---|
| G1 | Comments, strings, keywords, numbers, types in theme colors | AUTOMATED-NEW | manual tests G1 (every token kind maps to its theme color); smoke: comment, keyword, string and (new) number pixels in all six languages. **Note:** function = keyword (#ffffff), directive = type (#8cde94), number = constant (#7ad0c6) in the default theme: by eye these pairs cannot be told apart. |
| G2 | Jai main ::, x :=, #import | AUTOMATED | syntax goldens "main :: () {" → function, "x := 1;" → variable, "#import" → directive + string; colors via G1. |
| G3 | Jai nested block comment | AUTOMATED | syntax golden "/* a /* b */ still */". |
| G4 | C: "/*" typed on line 1 comments out the rest; "*/" restores | AUTOMATED-NEW | manual tests G4 (line 3 becomes comment, then type again); `--bench-syntax` "/* on the first line, then M->: ~1133 ms with vsync". |
| G5 | #define MAX 10 // note | AUTOMATED-NEW | syntax golden "C: #define with a number and a comment (manual G5)". |
| G6 | static void foo(int x) in column 0; a call stays plain | AUTOMATED | syntax goldens "static void foo(int x)" and "bar(x)". |
| G7 | JS division, regex, template with ${ } | AUTOMATED | syntax goldens "x = a / b / c;", "x = /ab+c/gi;", template. |
| G8 | C# verbatim over two lines; C++ R"(raw "text")" | AUTOMATED-NEW | C#: golden (existing). C++: new golden "a raw string without a delimiter, quotes inside (manual G8)". |
| G9 | Matching bracket on "(" and after ")"; not in strings | AUTOMATED | `test_show_paren`; smoke "the matching ')' on the paren_match background". |
| G10 | M-x set-language Jai: "Language: Jai", recolored | AUTOMATED-NEW | manual tests G10; states recomputed: `test_set_language`. |
| G11 | Large file, M->: text at once, colors within ~1 s, typing not blocked | HUMAN | Feel. Measured: `--bench-syntax` "/* ... then M->: 85 frames, 177 ms (Present(0,0)), ~1133 ms with vsync; typing at line 1,000,000: build avg 19 us". |

## H. Minibuffer

| Step | What | Status | Evidence |
|---|---|---|---|
| H1 | M-x "sav buf" filters; save-buffer shows C-x C-s; RET runs it | AUTOMATED-NEW | manual tests H1. |
| H2 | C-n / C-p, TAB, C-g: "Quit" | AUTOMATED | `test_completion`, `test_minibuffer` "C-g aborts". |
| H3 | M-p recalls the previous command | AUTOMATED | `test_completion` "command history". |
| H4 | C-x C-f in the buffer's directory; filters; RET descends | AUTOMATED | `test_find_write`. |
| H5 | DEL after "/" removes the last component | AUTOMATED | `test_minibuffer` updir. |
| H6 | "~/" profile; "c:/" drive | AUTOMATED-NEW | ~/: `test_find_write`; c:/: manual tests H6. |
| H7 | New name: "(New file)"; type, C-x C-s: created | AUTOMATED-NEW | manual tests H7. |
| H8 | C-x b RET previous; C-x b <name> RET | AUTOMATED | `test_buffers`. |
| H9 | C-x <left> / <right>; position remembered | AUTOMATED-NEW | manual tests H9. |
| H10 | C-x k on a modified buffer asks; "no" keeps it | AUTOMATED | `test_buffers` "Buffer x.c modified; kill anyway? (yes or no) ". |
| H11 | C-x C-w onto an existing file: overwrite? n, then y | AUTOMATED | `test_find_write` "File exists; overwrite? (y or n) ". |
| H12 | C-x s, two modified files: y, n, !, q | AUTOMATED | `test_save_some`. |
| H13 | Close button, modified: save question, "exit anyway?"; "no" stays | AUTOMATED-NEW | manual tests H13 ("Modified buffers exist; exit anyway? (yes or no) ", no → still running). |
| H14 | C-g closes any prompt with no effect | AUTOMATED | `test_minibuffer` (every prompt kind), `test_save_some`, `test_goto_line`; new: find-file and set-language (manual tests H14). |
| H15 | C-x b *Messages* RET | AUTOMATED-NEW | manual tests H15. |

## I. Config

| Step | What | Status | Evidence |
|---|---|---|---|
| I1 | C-c , opens ronin.conf (created under %APPDATA%\ronin) | AUTOMATED-NEW | manual tests I1: open-config creates the file from the defaults ("Created <path> from the built-in defaults"), visits it, and watches its new directory. The %APPDATA%\ronin location is `app_config_path`. **Note:** a ronin.conf next to ronin.exe wins (portable). |
| I2 | background = #202020, save: applied, "Reloaded ronin.conf" | AUTOMATED-NEW | manual tests I2 (a directory notification, then the settle: "Reloaded ronin.conf", background 202020); pixels: smoke "background after loading a config with background = #102030". |
| I3 | font_size = 14: applied | AUTOMATED-NEW | smoke "font_size = 14 in the config: 10x22, 1 more set-up". |
| I4 | render_mode classic / natural / symmetric: visibly different | HUMAN | "Visibly" needs eyes. The three screenshots differ (build/shots/mode_*.png, different MD5s); the differences are subtle at this size. |
| I5 | tab_width = 8 | AUTOMATED-NEW | manual tests I5 (column 8 after a tab, after a reload). |
| I6 | "C-z undo" works | AUTOMATED-NEW | manual tests I6. |
| I7 | Unknown command: "ronin.conf:N: unknown command ...", editing goes on | AUTOMATED-NEW | manual tests I7 ("ronin.conf:2: unknown command 'no-such-command'", then typing). |
| I8 | Save ronin.conf from Notepad: applied; no default-theme flash | HUMAN | Notepad. Mechanism AUTOMATED: `test_hot_reload` (save by rename, truncate-then-write read once after the 50 ms settle, a locked file retried). **Note:** a live reload that cannot read the file keeps the old config; only at startup is a locked config replaced by the defaults until the retry. |

## J. Outside changes

| Step | What | Status | Evidence |
|---|---|---|---|
| J1 | Changed outside, come back: "Reverted <name>", point kept | AUTOMATED-NEW | manual tests J1 (on activation); `test_disk` point kept. |
| J2 | C-x u right after: previous text back, modified | AUTOMATED | `test_disk` "undo of a reload" (C-/, the same command). |
| J3 | Change while focused: updates by itself | AUTOMATED | `test_disk` "reloaded by the watch". |
| J4 | A change by another tool appears | AUTOMATED | `test_disk` (the same directory notification for any writer); a real directory watch: `test_hot_reload`. |
| J5 | Modified + outside change: not reloaded, "changed on disk", mode line flag | AUTOMATED | `test_disk` "d.txt changed on disk", "[changed on disk]". |
| J6 | Then C-x C-s: "Save anyway?" | AUTOMATED | `test_disk` "... Save anyway? (yes or no) ". |
| J7 | Deleted outside: "deleted on disk"; buffer stays | AUTOMATED | `test_disk`. |
| J8 | revert-buffer on modified asks; yes reloads | AUTOMATED | `test_revert`. |
| J9 | Logoff with an unsaved file: blocked, "Unsaved changes in ronin" | HUMAN | A real logoff. Everything ronin does in it is AUTOMATED-NEW in the smoke: WM_QUERYENDSESSION refused, the reason reads exactly "Unsaved changes in ronin", the save question, a repeated query, an abort, a cancel, and allowed once saved. |

## K. Search and replace

| Step | What | Status | Evidence |
|---|---|---|---|
| K1 | C-s jumps incrementally; other matches lazy-highlighted | AUTOMATED | `test_isearch` state table; smoke stage 4 lazy_highlight. |
| K2 | C-s next; C-r previous | AUTOMATED | `test_isearch`. |
| K3 | DEL steps back | AUTOMATED | `test_isearch` DEL unwinding. |
| K4 | RET: "Mark saved where search started"; C-x C-x returns | AUTOMATED-NEW | message: `test_isearch`; C-x C-x: manual tests K4. |
| K5 | C-g returns to the start | AUTOMATED | `test_isearch`. |
| K6 | Failing part highlighted; C-g removes it | AUTOMATED-NEW | smoke stage 5 "the failing part on the isearch_fail background"; C-g: `test_isearch`. |
| K7 | Past the last match: Failing, then Wrapped | AUTOMATED | `test_isearch` ("Failing I-search: ", "Wrapped I-search: " prompts). |
| K8 | Smart case; M-c toggles | AUTOMATED | `test_isearch`. |
| K9 | C-s C-s last string; C-w a word | AUTOMATED | `test_isearch`. |
| K10 | Wheel during a search, then C-s | AUTOMATED | `test_isearch` "the wheel". |
| K11 | Fast "C-s foo RET" ends at the match | AUTOMATED | `test_isearch` typed ahead in one batch. |
| K12 | M-% with y, n, ., q | AUTOMATED | `test_replace`. |
| K13 | "!" over foo / Foo / FOO; "Replaced N occurrences" | AUTOMATED-NEW | manual tests K13. |
| K14 | One C-x u restores everything | AUTOMATED | `test_replace` "one undo". |
| K15 | With a region, only the region | AUTOMATED | `test_replace` "region". |
| K16 | M-x replace-string | AUTOMATED | `test_replace`. |
| K17 | 100 MB replace-string: progress, C-g stops, C-x u restores | AUTOMATED-NEW | `--bench-search` "replace-string buffer -> BUF in the 100 MB file: after 4 frames 'Replacing... 9%'; C-g: 'Replaced 87987 occurrences (stopped)'; one C-x u ... gives back the exact text 1" (exit 9 if not). |
| Z1 | Idle 0% CPU; about 80 MB without the big file | AUTOMATED-NEW | `--idle-check`: CPU 0 us over 5 s, 0 frames, private bytes 75 MB. |

## L. Windows (Phase 10)

"window tests" below are the `--test` groups logged as "test: ok: window tree", "window commands",
"pop-up rule", "two windows on one buffer", "mouse in windows", "the minibuffer and isearch from the
second of three windows", "a frame too small for its windows", "startup_windows = 2" and "command line
files".

| Step | What | Status | Evidence |
|---|---|---|---|
| L1 | C-x 3: two windows side by side, both on the buffer at the same place, the left one selected | AUTOMATED-NEW | window tests "C-x 3"; harness, every layout: C-x 3 through posted key messages. Screenshot build/shots/p10_1_side_by_side.png. |
| L2 | C-x 2: two windows one above the other | AUTOMATED-NEW | window tests "C-x 2" (heights 400 / 384 in the 784 px frame). Screenshot build/shots/p10_2_stacked_same_file.png. |
| L3 | C-x o goes through the windows in order and back to the first | AUTOMATED-NEW | window tests "C-x o in cyclic order"; harness "C-x o selected 1". |
| L4 | C-x 0 deletes the window; on the only window it refuses | AUTOMATED-NEW | window tests: the most recently used window is selected; "Attempt to delete minibuffer or sole ordinary window". |
| L5 | C-x 1 leaves only the selected window | AUTOMATED-NEW | window tests "C-x 1". |
| L6 | Splitting stops at "Window too small for splitting", and past 8 windows "Too many windows" | AUTOMATED-NEW | window tests (4 stacked windows; 8 side by side) and the tree fuzz. |
| L7 | C-x + makes the windows equal | AUTOMATED-NEW | window tests: 8 windows of 160 px; tree test: thirds after C-x 3 C-x 3. |
| L8 | C-x ^, C-x }, C-x { change the size by one line / column; "Cannot enlarge selected window" when nothing can give | AUTOMATED-NEW | window tests "C-x ^", "C-x }", "C-x {" (416 / 368, 648 / 632, 640). |
| L9 | C-M-v / C-M-S-v scroll the other window; with one window "There is no other window" | AUTOMATED-NEW | window tests (46 lines, back again); harness, every layout. |
| L10 | C-x 4 f and C-x 4 b open a file / buffer in another window and select it | AUTOMATED-NEW | pop-up rule test (prompts "Find file in other window: ", "Switch to buffer in other window (default ..."). |
| L11 | C-x 4 0 kills the buffer and its window; on the only window the buffer goes and the window stays | AUTOMATED-NEW | pop-up rule test. Note: the message after it is Emacs' "Attempt to delete minibuffer or sole ordinary window". |
| L12 | C-h e shows *Messages* in another window, at its end, without selecting it | AUTOMATED-NEW | pop-up rule test "C-h e". |
| L13 | A buffer shown "in another window": one window splits side by side on a wide frame, one above the other on a narrow one; a window already showing it is reused; otherwise the least recently used window | AUTOMATED-NEW | pop-up rule test, each case (split_width_threshold = 200 for the narrow one). |
| L14 | Two windows on one file: each keeps its own point and scroll; typing in one moves the other's text correctly; killing the buffer replaces it in both | AUTOMATED-NEW | two windows on one buffer. |
| L15 | Switching buffers back and forth in one window returns to its place and leaves the other window alone | AUTOMATED-NEW | two windows on one buffer "C-x b back and forth"; buffer list test "each view remembers its own place". |
| L16 | M-x, goto-line and isearch from a window other than the first: that window stays selected (RET and C-g) | AUTOMATED-NEW | the minibuffer and isearch from the second of three windows. |
| L17 | Window commands in the minibuffer refuse with a message (C-M-v scrolls the window it was called from) | AUTOMATED-NEW | window tests "in the minibuffer". Note: Emacs' C-x o would leave the minibuffer; ronin says "Cannot select another window from the minibuffer" (Later). |
| L18 | C-x o during an isearch ends it | AUTOMATED-NEW | window tests "C-x o ends the isearch". |
| L19 | A click selects the window under the pointer and puts point there | AUTOMATED-NEW | mouse in windows "a click selects the window and sets point". |
| L20 | The wheel scrolls the window under the pointer, not the selected one | AUTOMATED-NEW | mouse in windows "the wheel over a window that is not selected". |
| L21 | Dragging the line between two windows, or a mode line, resizes the windows on either side | AUTOMATED-NEW | mouse in windows: divider to 720 px, mode line to 336 px, the third window unchanged. |
| L22 | The drag follows the pointer smoothly | HUMAN | How it feels needs a hand on the mouse. |
| L23 | The pointer turns into the resize cursor over a divider and over a mode line with a window below; the arrow elsewhere | AUTOMATED-NEW + HUMAN | Which cursor: mouse in windows (app_mouse_cursor at the divider, a mode line, the bottom mode line, text, with the minibuffer reading, in a tiny frame); harness: a mouse move and WM_SETCURSOR request no frame. That Windows shows that cursor: HUMAN. |
| L24 | A drag selection that leaves its window stays in it | AUTOMATED-NEW | mouse in windows "a drag selection stays in its window". |
| L25 | Dividers and inactive mode lines look right at 100% and 150% | HUMAN | Visual. Smoke stage 6 checks the exact colors at 100%; dev helper for 150%: `ronin_debug.exe src\app.c --scale 150 --keys "C-x 3 C-x 2" --screenshot build\shots\l25.png`. |
| L26 | A long line in the left window never draws into the right one | AUTOMATED-NEW | smoke stage 6: the divider exactly window_divider next to the line, the right window's first column exactly background. |
| L27 | window_divider, mode_line_inactive_background, mode_line_inactive_text in ronin.conf | AUTOMATED-NEW | config test "window settings and colors", "derived inactive mode line colors"; applying live is the hot reload's (AUTOMATED). |
| L28 | startup_windows = 2 with two files: side by side, each file at its +LINE | AUTOMATED-NEW | startup_windows = 2 test. Screenshot build/shots/p10_5_startup_windows_2.png. |
| L29 | Several files on the command line, each with its own +LINE[:COLUMN] | AUTOMATED-NEW | command line files (6 cases); startup_windows = 2 test. |
| L30 | The OS window dragged to its smallest with several windows, then back: nothing breaks, the windows come back as they were | AUTOMATED-NEW + HUMAN | a frame too small for its windows (8 windows at 40x30, 1x0, 8x16 ...: identical rects, scroll positions and points afterwards). How it looks while small: HUMAN. |
| L31 | Typing right after starting ronin (before its window appears) lands in ronin, in order | AUTOMATED-NEW | `--type-ahead-check` (on demand: real input): keys sent 20-83 ms after WinMain, first Present at 160 ms, "abCde". Keys typed in the first ~35-50 ms, before the window exists, still go to the launcher. |
| L32 | Startup is not slower | AUTOMATED | `--startup-ms`, release, 15 runs in rotating order: 170 ms median against 169 for Phase 9. |
| L33 | A ronin name that a Windows header defines as a macro breaks the build | AUTOMATED-NEW | build.bat's check (src/check_macros.c); the old MOD_ALT, KillRing.small and a test's near each fail it. |
| L34 | Typing with two windows on one buffer stays fast | AUTOMATED-NEW | `--bench-windows`: 15-22 us a keystroke with two windows, 8-10 us with one. |

## M. Projects and fuzzy file open (Phase 11)

"finder tests" below is the `--test` group logged as "test: ok: project finder"; the other project tests are
named as in the evidence table.

| Step | What | Status | Evidence |
|---|---|---|---|
| M1 | C-x p f in a file of a repository: the prompt opens at once, names the root, and lists its files | AUTOMATED-NEW | finder tests (the prompt "Find file in <root>/: " before the scan ends; 7 rows); harness: C-x p f is among the default bindings described through their keys on every layout. Screenshot build/shots/p11_1_finder_ap.png. |
| M2 | Typing narrows the list by fuzzy match, file-name matches first, the matched characters highlighted | AUTOMATED-NEW | fuzzy (13 ranking goldens: file name over directory, segment and word starts, consecutive, exact name, smart case, '/', several terms); smoke stage 7 (matched characters exactly completion_match). Screenshots p11_1 ("ap") and p11_2_finder_long.png ("src/find") inspected. |
| M3 | RET visits the selected file; C-g closes the finder | AUTOMATED-NEW | finder tests ("fuz" RET visits src/fuzzy.c; after C-g the next C-x p f opens a new prompt, which it could not inside one). |
| M4 | With nothing typed, the files visited in this session come first, most recent first | AUTOMATED-NEW | finder tests (fuzzy.c then main.c, then the rest of the index). |
| M5 | Typing a name and RET at once (typed ahead) visits the best match | AUTOMATED-NEW | finder tests: one candidate a frame, "app.c" RET visits app.c; "map RET" in the same frame visits map_app.c. |
| M6 | A large project: the finder opens at once during the scan, says it is scanning, and fills in | AUTOMATED-NEW + HUMAN | finder tests (a scan held after its first batch: "scanning: N files", a partial list, RET on what is known; released, all 10,000). Screenshot p11_3_scanning.png. How it feels on a big real repository: HUMAN. |
| M7 | Files and directories in .gitignore are not listed; nested .gitignore files; '!' includes again | AUTOMATED-NEW | ignore rules (30 cases, every supported feature); scanner (the exact set on a generated tree, 7 ignored). |
| M8 | build/, out/, bin/ and obj/ directories are listed unless .gitignore or .ronin-project leaves them out | AUTOMATED-NEW | project roots (the built-in project_ignore on build, src/bin, out, obj, target, dist: not ignored); scanner (bin/tool.c listed). |
| M9 | .ronin-project: [ignore] hides, [allow] shows what .gitignore hides; its mistakes are reported with line numbers | AUTOMATED-NEW | .ronin-project (diagnostics with file:line); scanner (docs/draft ignored, vendor/ allowed). |
| M10 | A junction or symlink pointing back into the tree does not make the scan endless | AUTOMATED-NEW | scanner (src/loop, a junction to the root, not entered). |
| M11 | A file added outside ronin shows up in the finder | AUTOMATED-NEW | finder tests (the tree watch marks the index stale; C-x p f shows the old list with "rescanning", then the new file). **Note:** it appears once the rescan that C-x p f starts has ended, not while the finder is closed. |
| M12 | M-x project-refresh scans again and says how many files and how many were ignored | AUTOMATED-NEW | finder tests ("Scanning <root>/...", then "<N> files in <root>/ (<M> ignored)"). |
| M13 | `ronin <directory>`: that directory is the project; *scratch* and C-x C-f start there | AUTOMATED-NEW | finder tests (*scratch*, no message, C-x C-f's input, C-x p f lists it); manual C4. |
| M14 | *scratch* started without a directory: C-x p f says how to get a project | AUTOMATED-NEW | finder tests (the exact message). Screenshot p11_4_not_in_project.png. |
| M15 | M-x project-find-file-other-window shows the file in another window | AUTOMATED-NEW | finder tests (two windows, readme.md in the selected one). |
| M16 | A path too long for the list is cut on the left, the file name kept | HUMAN | Visual. Screenshot build/shots/p11_5_long_path.png inspected: the row starts with "…", ends with the file name, the matched part highlighted. Dev helper: see HUMAN step 17. |
| M17 | Typing in a buffer stays as fast while a big project is scanned | AUTOMATED-NEW | `--bench-project`: 9-19 us a keystroke during a scan of C:\Windows, 9-20 us without. |
| M18 | Ranking a million files never holds a frame beyond the budget | AUTOMATED-NEW | `--bench-project`: longest frame 2,097 us, none over 2,100 us. |
| M19 | After the scan ronin is idle: 0% CPU | AUTOMATED-NEW | `ronin_debug.exe src\app.c --keys "C-x p f a p C-g" --idle-check`: 0 frames, 0 main loop passes. |
| M20 | Closing ronin during a scan exits at once, also when a network path hangs | AUTOMATED-NEW | finder tests: shutdown with a scan running well under 1 s; a worker stuck in the "file system" is left after 1,000 ms and the process exits. A real dead network path: not tried. |
| M21 | A huge tree stops at project_max_files and says so | AUTOMATED-NEW | finder tests (project_max_files = 1000: "Project has more than 1000 files; listed the first 1000", "stopped at 1000 files"). |
| M22 | Only the selected window shows its active region | AUTOMATED-NEW | the region in windows (C-x o and back, while the minibuffer reads); smoke stage 6 (the other window's region not drawn). |
| M23 | Typing in another window while `--test` runs does not make it fail | AUTOMATED-NEW + HUMAN | shield self-test (every decision); the cases check that their key state held. Real typing meanwhile: HUMAN step 18. |
| M24 | The benches and the smoke never take the focus | HUMAN | They show their window without activation (code); only a person typing elsewhere meanwhile can tell. HUMAN step 18. |

## N. Project-wide search (Phase 12)

"grep tests" below are the `--test` groups logged as "test: ok: grep file", "grep search" and "grep sharing";
"results tests" is "test: ok: results", "occur tests" is "test: ok: occur". Screenshots are in build/shots.

| Step | What | Status | Evidence |
|---|---|---|---|
| N1 | C-x p g in a file of a project asks "Search project for: ", offering the symbol at point (or the region) as the default | AUTOMATED-NEW | results tests: the prompt "Search project for (default foo): " with point on foo, "(default int foo;)" with a region; RET on the empty input searches the default. harness: C-x p g among the default bindings on every layout. |
| N2 | RET: *search* opens in another window, selected, with a header, one line per match ("path:line:col: text") and a summary line | AUTOMATED-NEW | results tests: the exact text of *search* (header, lines, "7 matches in 5 files; 6 files searched, 1 binary skipped"), shown in another window and selected, point on the first match. Screenshot p12_1_search_ronin.png ("search_begin" in ronin's tree: 16 matches in 6 files, 0.02 s). |
| N3 | File names and line numbers in their own colors, the matched text highlighted | AUTOMATED-NEW | smoke stage 8: the file name exactly search_file, "line:col" exactly search_line_number, the match on lazy_highlight with its text in the text color, the rest of the line on the background. Screenshots p12_1 and p12_3 inspected: white file names, teal line numbers, tan text, the matches highlighted. |
| N4 | RET on a match visits it in the other window and selects that window | AUTOMATED-NEW | results tests (RET visits a.c at 1:5, two windows, a.c selected); screenshot p12_3_visited.png (n n RET: src/isearch.c:87:5 in the upper window, selected, the cursor on the match). |
| N5 | n and p show the next and previous match in the other window; the results window stays selected | AUTOMATED-NEW | results tests (n: *search* stays selected on the next line, the other window shows a.c at 2:1; p back; p before the first: "No more matches"). |
| N6 | g searches again; q closes the results window | AUTOMATED-NEW | results tests (5 runs of g give the same text; q leaves one window). |
| N7 | M-g n / M-g p (and C-x \`) from another window walk the matches; at the ends "No more matches" | AUTOMATED-NEW | results tests (next-error from the other window through all 7 matches, past the end; previous-error back and before the first; "No next-error capable buffer found" with no results). |
| N8 | The same search gives the same list in the same order every time | AUTOMATED-NEW | results tests (the exact text and the next-error order with 1, 2, 4 and 8 workers and 5 reruns, a large file early in the list so later files finish first); grep tests (the same matches with 1-8 workers). |
| N9 | A big project: the list fills in while the search runs, the last line counts files and matches; typing stays fast | AUTOMATED-NEW + HUMAN | `--bench-grep` (100 MB in 3,000 files: first result after ~2 ms, all in 23-139 ms with 8 to 1 workers; a key's frame 36-38 us while 8 workers search, 28-39 us without); results tests (the progress line "Searching... N/M files, K matches" of a held search); screenshot p12_2_running.png ("Searching... 8/71 files, 9 matches"). On a big real repository: HUMAN step 19. |
| N10 | Binary files are skipped (and counted); UTF-16 files are found | AUTOMATED-NEW | grep tests (a NUL in the first 8 KB: binary; after it: searched; UTF-16 LE and BE with a BOM; a UTF-8 BOM skipped); results tests (bin.dat skipped and counted, u16.txt found). |
| N11 | A file with unsaved changes is searched as it is in ronin, not as on disk; a new file not saved yet too | AUTOMATED-NEW | results tests (b/c.txt edited in ronin: its disk text "foo on disk" not listed; a modified file the ignore rules leave out and a new file after the list, in path order; an empty new buffer and *scratch* not searched; "3 from unsaved buffers"). |
| N12 | At 10,000 matches the search stops and says so | AUTOMATED-NEW | results tests (search_max_results = 2: exactly the first two matches in list order, "Stopped at 2 matches (search_max_results) in 1 file"); `--bench-grep` (10,000 at the default cap, stopped). |
| N13 | Saving a file, a build or git while a search runs: nothing fails because of ronin | AUTOMATED-NEW | grep sharing (two files held open by the workers: one saved through the temp file and ReplaceFileW, one renamed and deleted; all succeed). A real build or git command during a search: not tried by hand. |
| N14 | A new search, or killing *search*, stops the running one | AUTOMATED-NEW | results tests (C-x k on a held search: the buffer goes, the workers end); grep tests (300 start / cancel rounds, each ending within 250 ms). |
| N15 | Closing ronin during a search exits at once, also with a stuck file system | AUTOMATED-NEW | results tests (shutdown with a search running: well under 1 s; a stuck worker left after the shared 1,000 ms bound). |
| N16 | C-x p g outside a project says how to get one | AUTOMATED-NEW | results tests (*scratch*: "Not in a project ..."). |
| N17 | M-s o (occur) lists the matching lines of the current buffer, unsaved text included, with the same keys | AUTOMATED-NEW | occur tests (the exact *occur* text, n, RET, next-error past the end, g after an edit, 10,000 lines over many frames, the buffer killed meanwhile); screenshot p12_4_occur.png. |
| N18 | The results keys can be rebound in [keys search-results], and they work only in results buffers | AUTOMATED-NEW | results tests (z bound to next-error-no-select; n types n in *scratch* and in the window next to *search*; describe-key in *search*); buffer keymap test. |
| N19 | After a search ronin is idle: 0% CPU | AUTOMATED-NEW | `ronin_debug.exe . --keys "C-x p g s e a r c h _ b e g i n RET" --idle-check`: 0 frames, 0 us of CPU in 5 s. |
| N20 | A very long line shows a part around the match with "…" at each cut | AUTOMATED-NEW | grep tests (a long line cut on character boundaries with an ellipsis at each end, an invalid byte at the cut). |
| N21 | `ronin --no-activate` shows the window without taking the focus | HUMAN | Real focus. Checked during this phase with a PowerShell script around the memory measurements: GetForegroundWindow before and after each of 12 starts, unchanged; the command-line test covers the flag in every build. Typing elsewhere meanwhile: HUMAN step 20. |

## O. Build and jump to error (Phase 13)

"build tests" are the `--test` groups listed above ("builds in the app" is the headless app driven through
the real keymap; the builds there are `--child` programs run through cmd.exe). Screenshots are in build/shots.

| Step | What | Status | Evidence |
|---|---|---|---|
| O1 | C-x p c in a project asks "Compile command: " with .ronin-project's [build] command filled in, ready to edit; nothing runs before RET | AUTOMATED-NEW | builds in the app (the prompt and its input; no *compilation* and no process before RET; an empty command: "No compile command"). harness: C-x p c among the default bindings on every layout. |
| O2 | The command offered next time is the one last used in that project | AUTOMATED-NEW | builds in the app (an edited command is offered over .ronin-project's on the next C-x p c). |
| O3 | RET: *compilation* opens in another window without being selected: "Command:", "Directory:", a blank line, the output as it arrives, then a blank line and how it ended | AUTOMATED-NEW | builds in the app (the exact text, a.c still selected, two windows); screenshots p13_jai.png, p13_selfbuild.png, p13_running.png. |
| O4 | The last line says "Compilation finished in 1.24 s", "Compilation exited abnormally with code N in ...", or "Compilation killed"; the echo area sums up errors and warnings | AUTOMATED-NEW | builds in the app ("Compilation exited abnormally with code 2: 2 errors, 1 warning", "Compilation killed", the link case with code 1120); screenshot p13_selfbuild.png ("Compilation finished in 2.39 s", "0 errors, 0 warnings"). |
| O5 | The mode line shows running, then exit [0] or exit [N] (killed when killed) | AUTOMATED-NEW | builds in the app (Compilation:running, :exit [2], :killed, and :failed when it cannot start); smoke stage 9 (the state's cells drawn); screenshots p13_running.png and p13_jai.png. |
| O6 | Error lines show the file and line in the colors of *search*, "error" red, "warning" orange, notes green | AUTOMATED-NEW | smoke stage 9 (the file name exactly search_file, "(1,5)" search_line_number, "error" compilation_error, "warning" compilation_warning); screenshots p13_jai.png (Jai), p13_selfbuild_error.png (cl) inspected. |
| O7 | M-g n / M-g p from any window go to the next / previous error (notes skipped), and the place stays highlighted until the next key | AUTOMATED-NEW | builds in the app (next-error to a.c 2:5, the note skipped, b.h, a.c 4, "No more matches", previous-error; the whole line highlighted, gone after C-f); the landing highlight test (search results too); screenshots p13_landing.png, p13_selfbuild_next_error.png. |
| O8 | In *compilation*: RET visits the error on its line, n / p show the next / previous one in the other window, q closes the window; RET on another line says so | AUTOMATED-NEW | builds in the app (RET on "Building...": "No error on this line"; n, n skipping the note, p; RET on the note visits note.h; q closes the window; [keys compilation]'s defaults and describe-key). |
| O9 | g in *compilation* and M-x recompile run the last command again without asking | AUTOMATED-NEW | builds in the app (g, M-x recompile from a.c, "No previous compilation" before any build); recompile in *search* still searches again (results tests). |
| O10 | C-c C-k (M-x kill-compilation) stops the build and every process it started | AUTOMATED-NEW | build kills (a child and its grandchild gone within 1 s; a kill during a 50 MB flood; 100 start / kill rounds); builds in the app (C-c C-k: "Compilation killed", the grandchild gone; "No compilation process" with none). |
| O11 | C-x p c while a build runs asks "A compilation process is running; kill it? (yes or no)": no keeps it, yes starts the new one | AUTOMATED-NEW | builds in the app (no: the same build runs on; yes: a new build, the old tree gone). |
| O12 | Unsaved files before a build: asked one by one (y, n, q) by default, or all saved, or left, by compilation_save_buffers | AUTOMATED-NEW | builds in the app (ask with y / n / q, all, none: asked or not, saved or not, the build started each time). |
| O13 | The *compilation* window follows the output until the first error (or always, or never, by compilation_scroll_output) | AUTOMATED-NEW | builds in the app (first-error: point on the first error's line; true: at the end; false: at the start); screenshot p13_running.png (point following the output). |
| O14 | Exiting ronin while a build runs asks "A compilation is running; kill it and exit? (yes or no)"; yes ends the whole tree | AUTOMATED-NEW | builds in the app (C-x C-c: the question; no stays; yes quits; the grandchild gone after the shutdown); app_shutdown with a build running and no question (the path of WM_ENDSESSION): the tree gone. |
| O15 | Paths with drive letters, spaces and parentheses parse; times, URLs and version numbers never become errors | AUTOMATED-NEW | compile matchers (34 lines: cl with /FC, "Program Files (x86)", a path with spaces, csc, tsc, MSBuild's "1>", GCC / Clang, includes, Jai; "12:30:45", "Build started 12:30:45.", URLs, "version 1.2.3", the compiler banner). |
| O16 | Linker errors without a file position are still errors: colored and counted, never "0 errors" | AUTOMATED-NEW | compile matchers (LNK1104, LNK2019, D9025, "error:", "ld: error:", MSB4019; "0 errors", "0 Error(s)", "-Werror", "errors: 3" not counted); builds in the app (a link-only failure: "2 errors, 0 warnings", both words marked, next-error and RET skip them). |
| O17 | A relative path in an error line opens the right file (the build directory, beside MSBuild's project, the project root) | AUTOMATED-NEW | compile path resolution (each step, the order, the cache); builds (a .bat that changes directory: here.c in the build directory, sub/x.c under the root, x.c beside the bracketed project, one found nowhere counted without a location). |
| O18 | Files the build rewrote show their new text at once (or are flagged when they have unsaved changes) | AUTOMATED-NEW | builds in the app (gen.h rewritten by the build: reverted with no activation; mod.h modified: "changed on disk"; after a kill too). |
| O19 | Output with colors (ANSI) and in the console's code page shows as text | AUTOMATED-NEW | compile cleaning (CSI, OSC, a lone ESC; 0x81 0x82 from the OEM code page as ü é; valid UTF-8 kept; random read slicing); builds (the same bytes from a real child process). |
| O20 | A build that prints a lot: *compilation* keeps 16 MB and says "[output truncated]"; the build is never blocked, typing stays fast | AUTOMATED-NEW + HUMAN | builds (a 100,000-byte cap: one mark, all 2,000,000 bytes read, exit 0); `--bench-compile` (100 MB in 0.33 s, no frame over 0.3 ms; a key's frame at line 1,000,000 14-18 us avg, worst 217-237 us while builds print, 13-21 us avg, worst 243 us without, after one 5.8 ms first insert); a real build: HUMAN step 22. |
| O21 | A command that does not exist, a directory that does not exist | AUTOMATED-NEW | builds (cmd's own message and exit 1; the directory: no start, nothing left open); builds in the app (a [build] directory that does not exist: "Compilation could not start: ...", mode Compilation:failed); processes (the same through the platform layer). |
| O22 | .ronin-project's [build] directory and its mistakes | AUTOMATED-NEW | .ronin-project tests ([build] command and directory, unknown names, a backslash in a command); builds in the app (the build runs in root\sub; the diagnostic in *Messages*). |
| O23 | Building ronin itself from ronin: a clean build, then one with an error | AUTOMATED-NEW + HUMAN | screenshots p13_selfbuild.png (ronin_bench.exe on the repository, C-x p c RET: .\build.bat, 2.39 s, exit [0]) and p13_selfbuild_error.png / p13_selfbuild_next_error.png (a copy with an error and a warning in edit.c: both listed and colored, M-g n on edit.c line 866). Looking at a real failing build: HUMAN step 21. |
| O24 | After a build ronin is idle again: 0% CPU | AUTOMATED-NEW | `ronin_debug.exe build\tmp\demo_build\particles.jai --keys "C-x p c RET" --compile-wait 10000 --idle-check` (demo_build: a copy of samples\particles.jai with a .ronin-project build command, made by hand; no tool makes it): 0 frames, 0 us of CPU in 5 s (and the same with no build). The build's worker waits in the kernel; nothing polls. |

## P. Multiple cursors (Phase 14)

"cursor tests" are the `--test` groups listed above ("the multiple-cursor differential fuzz" runs 20,000 random
commands against the one-cursor commands applied cursor by cursor to a flat text). Screenshots are in build/shots.

| Step | What | Status | Evidence |
|---|---|---|---|
| P1 | Select a word, C->: a cursor with the same region at its next occurrence, which becomes the main cursor (the view follows it); at the last one "No more matches" | AUTOMATED-NEW | multiple-cursor commands ("aaaa" with "aa": not overlapping, then "No more matches"; at the end of the buffer; a multi-line region; case sensitive); in the app (C-> by its key on a 1.2 MB buffer); screenshot p14_next.png (C-> three times: four regions). |
| P2 | C-< does the same backwards, before the first cursor | AUTOMATED-NEW | multiple-cursor commands (at the start, point at the region's start, "No more matches"); in the app (C-< at the first occurrence). |
| P3 | Without a region, C-> / C-< add a cursor on the next / previous line at the same column (at the end of a shorter line, the column kept for the next one) | FIXED | multiple-cursor commands (a short line, the column kept, the first and last lines); "a stale goal column" (C-n C-e C->: the column of the end of the line, not the one C-n left) failed in screenshot p14_typing.png and was fixed in 3356c01 (fix(cursors): the goal column only across vertical motions). |
| P4 | Select a word, C-c C-<: a cursor on every occurrence, the count in the echo area; on a huge file it runs over several frames, keys wait, C-g stops it | AUTOMATED-NEW | multiple-cursor commands ("Marked 3 occurrences", overlapping occurrences, "No region"); in the app (13 frames at 64 KB a frame, keys typed meanwhile do nothing, C-g: "Quit" and no cursor added); `--bench-cursors` (a million occurrences: the cap in one frame; the whole 100 MB file: 5 frames); screenshot p14_mark_all.png ("Marked 120 occurrences", mc:120). |
| P5 | M-x skip-to-next-like-this moves the newest cursor to the next occurrence, M-x unmark-next-like-this removes it | AUTOMATED-NEW | multiple-cursor commands (skip by text and by lines, "No more matches", unmark, "Only one cursor"). |
| P6 | Select some lines, C-S-c C-S-c: a cursor on each line at point's column (at the end of a shorter line); M-x edit-ends-of-lines / edit-beginnings-of-lines | AUTOMATED-NEW | multiple-cursor commands (short lines, point on the first or the last line, both variants, "No region"); in the app (C-S-c C-S-c by its key); screenshot p14_edit_lines.png. |
| P7 | Typing, DEL, C-d, RET, motions and C-SPC act at every cursor; cursors that meet become one | AUTOMATED-NEW | multiple-cursor differential fuzz (every cursor and the text after each command); three cursors; the multiple-cursor driver (8 merge cases); screenshot p14_typing.png. |
| P8 | Kills with several cursors: one piece per cursor; C-y with as many cursors gives each its piece, with another number the whole text; the clipboard gets the pieces one per line | AUTOMATED-NEW + HUMAN | multiple-cursor kills and yanks (pieces, appending and prepending, yank-pop, a clipboard changed elsewhere, kill-ring-save, the clipboard text) on the in-memory clipboard; the fuzz compares the newest kill after every kill. Pasting into another program: HUMAN step 26. |
| P9 | C-/ after a command with several cursors brings the text and every cursor back; C-? goes forward again | AUTOMATED-NEW | multiple-cursor undo (merged typing, undo-redo, the chain, 1,452 random commands undone and redone, a one-cursor group undone with three); proven to fail with the cursor restore broken in a scratch copy. |
| P10 | C-g with several cursors first deactivates their regions, then leaves the main cursor only | AUTOMATED-NEW | the multiple-cursor driver (keyboard-quit's two steps); in the app (C-g C-g). |
| P11 | C-s, M-%, M-x replace-string, M-g g, C-x h, a plain click and switching buffers leave one cursor | AUTOMATED-NEW | multiple cursors collapsing (through the keys; an aborted prompt keeps one cursor); the multiple-cursor driver (C-x h); multiple cursors and the mouse (a plain click). |
| P12 | Ctrl+Shift+click adds a cursor there, or removes the one already there (never the last) | AUTOMATED-NEW + HUMAN | multiple cursors and the mouse (adding, removing, the last one kept, refused on the minibuffer) through the app's mouse events. With a real mouse and keyboard: HUMAN step 25. |
| P13 | TAB, S-TAB, comment-line, kill-whole-line and M-^ with two cursors on one line act on that line once | AUTOMATED-NEW | the multiple-cursor driver (each line command with two cursors on one line). |
| P14 | Two cursors on one line, C-k: the first kills to the end of the line, the second (inside the killed text) does nothing; C-d at two adjacent cursors deletes two characters; a cursor at the end of the killed text still runs | AUTOMATED-NEW | the multiple-cursor driver (the three cases, DEL over a region, kill-word); the fuzz applies the same rule. |
| P15 | The mode line shows "mc:N" with more than one cursor | AUTOMATED-NEW | smoke stage 10 ("mc:3" drawn); multiple cursors and the mouse; screenshots p14_*.png and p14_idle_10000_modeline.png (mc:10000). |
| P16 | Every cursor and region is drawn in the selected window; the bracket match only at the main cursor | AUTOMATED-NEW | smoke stage 10 (each cursor cell, each region, the cells after the lines); screenshot p14_next.png (the paren match on the main cursor's line only). |
| P17 | In the minibuffer the cursor commands and Ctrl+Shift+click are refused with a message | AUTOMATED-NEW | multiple-cursor commands in the app (8 refusals, by the default keys and by keys bound in a config; M-x does not run inside a prompt); multiple cursors and the mouse. |
| P18 | max_cursors (10000) stops every adding command with "Cursor limit reached" | AUTOMATED-NEW | multiple-cursor commands (the cap at 5 for mark-all, mark-next, mark-previous and edit-lines); the multiple-cursor driver (the default, the range, the warning); `--bench-cursors` (100,000). |
| P19 | Two windows on one buffer: each keeps its own cursors, which follow the other window's edits | AUTOMATED-NEW | multiple cursors and the mouse (three cursors in one window, one in the other, typing in each). |
| P20 | Typing at 1,000 and 10,000 cursors stays fast, on a 100 MB file too | AUTOMATED-NEW + HUMAN | `--bench-cursors` (a self-insert at 1,000 cursors 0.2-0.3 ms, at 10,000 1.7-2.4 ms, command plus frame, in several runs); the feel: HUMAN step 27. |
| P21 | Idle with 10,000 cursors: 0% CPU | AUTOMATED-NEW | `ronin_debug.exe build\tmp\bench_cursors.c` (made by `ronin_bench.exe --bench-cursors`) `--keys "C-x h C-S-c C-S-c" --idle-check`: 0 frames, 0 us of CPU in 5 s. |
| P22 | C-> and C-< are Ctrl+Shift+. and Ctrl+Shift+, on a US keyboard | AUTOMATED-NEW | harness: every default binding on US and United Kingdom; on Turkish Q and Finnish they are listed as US keys landing elsewhere (the keys with > and < run them there). |
| P23 | A tool's summary line ("1 error generated.", "0 Error(s)", "Compilation failed (1 error)") is colored and not counted | AUTOMATED-NEW | compile matchers (19 summaries and traps); builds (a summary not counted); builds in the app (both count words of an MSBuild line marked, the echo's counts unchanged). |
| P24 | An error printed with an absolute path under the project root shows the path relative to the root; visiting it still works | AUTOMATED-NEW | compile path resolution (10 shown-path cases); builds in the app (on and off: under the root, outside it, relative; next-error to the file). |

## Q. The last pass and the release (Phase 15)

| Step | What | Status | Evidence |
|---|---|---|---|
| Q1 | Nothing says teal any more: the window title, the config file (`ronin.conf`, `%APPDATA%\ronin`), `.ronin-project`, messages, the log | AUTOMATED-NEW | smoke (the title "*scratch* - ronin"; the block reason "Unsaved changes in ronin"); the config and .ronin-project tests read the new names; `grep -rin teal` leaves N3's "teal line numbers" (a color), this step, the old-name notes in ARCHITECTURE.md (Phase 15 decisions) and CHANGELOG.md. |
| Q2 | `ronin --version` prints "ronin 0.1.0" and exits 0, without a window | AUTOMATED-NEW + HUMAN | verify.bat "version" (redirected output, the release exe); the command line test (`--version` a flag of every build); in a console (Start-Process -NoNewWindow): "ronin 0.1.0". In a terminal by hand: HUMAN step 33. |
| Q3 | ronin.exe's Properties show version 0.1.0, "ronin text editor", the copyright | HUMAN | `(Get-Item build\ronin.exe).VersionInfo` shows FileVersion / ProductVersion 0.1.0, the description and the copyright; Explorer's Details tab: HUMAN step 29. |
| Q4 | The icon in the title bar, the taskbar, Alt+Tab and Explorer | AUTOMATED-NEW + HUMAN | smoke "class icons set / set; the 16 px icon: stem #d3b58d, beside #072626, corner none" (shown to fail with the icon's text color changed by one); how it looks: HUMAN step 28. |
| Q5 | After a crash, each modified file is in `%LOCALAPPDATA%\ronin\recovery` as a save would write it (encoding, BOM, line endings), with a .path file; unmodified files are not | AUTOMATED-NEW | crash test (CRLF UTF-8, UTF-16 LE with CRLF and an astral character, UTF-16 BE, UTF-8 BOM, mixed: byte for byte against a save; the sidecars; none for the unmodified file); shown to fail with the contents not written (a scratch copy). |
| Q6 | The next start says how many recovery files there are and where; *Messages* lists each with its original; M-x recover-file shows the original and the copy side by side and overwrites nothing | AUTOMATED-NEW + HUMAN | crash test (the echo text, the *Messages* line, two windows, the original's bytes unchanged, the files kept; no notice without files). A crash in a real session: HUMAN step 32. |
| Q7 | `early_window = true`: the window appears at once in the background color, then the text, with no white or black flash; typed keys still reach ronin | AUTOMATED-NEW + HUMAN | `--startup-capture` (15 runs on and off: visible after ~40 ms against ~181, no other image in 14 of 15; one corner pixel changed during a slow start); the look and the default: HUMAN step 30. `--type-ahead-check` (run on request, with `--config`): off "abCde", every key before the first Present (174 ms); on (after d63f6f0 let the check show the window early: "shown early, painted" at 56.5 ms) "abCde", every key before the first Present (192 ms after WinMain), none while ronin had no focus window. |
| Q8 | Startup is not slower than Phase 14; idle is still 0% CPU | AUTOMATED-NEW | `--startup-ms --no-activate` 164 ms against 165 (15 runs alternating); `--idle-check`: 0 frames, 0 us in 5 s. |
| Q9 | Minimize, wait, restore: the window comes back at once and looks right; memory drops while minimized | AUTOMATED-NEW + HUMAN | measured (bench, a minimize and restore that never activates the window): 60.5 -> 58.1 MB while minimized, the first frame after restoring 1.5-1.8 ms; the look: HUMAN step 31. |
| Q10 | Typing the ':' of a case label at 10,000 cursors is fast | AUTOMATED-NEW | `--bench-cursors` (925 ms -> 28 ms at 10,000, one flush); the three-cursor case-label test (one flush, one undo). |
| Q11 | A 5 MB minified line: moving and typing stay around 1 ms a key | AUTOMATED-NEW | `--bench-longline` (typing at the end 0.75-1.14 ms, in the middle 0.38-0.64, moving 0.47-0.74); the bracket test on a line longer than the budget. |
| Q12 | Seeds 22 and 33 of the undo test pass | FIXED | `--test --seed 22` / `33` failed in test_undo_commands; fixed in 760277f (the test mistook a refused delete for an edit); the reduced case is a fixed test, and the sweep stops at seed 22 with the fix reverted. |
| Q13 | The wheel and commands that move another window's point keep several cursors ordered | FIXED | found by the soak; fixed in 6e4ca11 and 18d6e94 with regressions in the multiple-cursor mouse test. |
| Q14 | A million random events: no assert, no leak, no slowdown, flat memory | AUTOMATED-NEW | `--soak 1000000` on bench and debug in `verify.bat phase` (peak and final memory logged). |
| Q15 | Every seeded test group passes for 256 seeds | AUTOMATED-NEW | `--fuzz-seeds 1 256` in `verify.bat phase`. |
| Q16 | The release exe imports only kernel32, user32 and gdi32 and no C runtime DLL | AUTOMATED-NEW | verify.bat "check-imports" against res\imports.txt. |
| Q17 | `dist.bat` makes `dist\ronin-0.1.0-windows-x64.zip` with ronin.exe, README.md and LICENSE | AUTOMATED-NEW | dist.bat runs verify.bat, checks each copy and tar's exit code, and prints the zip's size (224,440 bytes; `unzip -t`: no errors; the three files in ronin-0.1.0\). |

## R. Memory and resource leaks (Phase 16)

| Step | What | Status | Evidence |
|---|---|---|---|
| R1 | Opening, editing and killing a file again and again (also the 100 MB one) does not grow memory, handles or the atlas | AUTOMATED-NEW | `--leak-check file-cycle` (220 cycles) and `file-cycle-100mb` (5 cycles): every verdict metric flat (docs/ARCHITECTURE.md, Measurements (Phase 16)). |
| R2 | Splitting and deleting windows, C-x 1 with 8 windows, hundreds of times: nothing grows, and the frame afterwards is the same | AUTOMATED-NEW | `--leak-check window-cycle` (both sub-cycles OK; the frames before the first and after the last cycle equal pixel for pixel, also by System.Drawing). |
| R3 | M-x, the find-file prompt, goto-line and switch-to-buffer opened and closed again and again | AUTOMATED-NEW | `--leak-check minibuffer-cycle` (four sub-cycles OK). |
| R4 | isearch with the lazy highlight, query-replace and its undo, mark-all-like-this on 1,000 occurrences, repeated | AUTOMATED-NEW | `--leak-check isearch-cycle`, `multi-cursor-cycle` (OK). |
| R5 | The undo log of a buffer edited and undone for a long time stays under undo_limit_mb | AUTOMATED-NEW | `--leak-check undo-cycle`: the cap reached in cycle 4 of the 20 warm-up cycles, then every sample under 1 MB, memory flat. |
| R6 | The project finder, the project search and builds, again and again (also cancelled while running): no memory, thread, event, process or handle left | AUTOMATED-NEW | `--leak-check finder-cycle`, `grep-cycle`, `compile-cycle` (all OK after the fixes R8-R12). |
| R7 | The config file rewritten and the text scale changed hundreds of times: the atlas does not grow, the frame comes back the same; a file changed on disk again and again: its directory's watch does not pile up | AUTOMATED-NEW | `--leak-check config-reload-cycle` (two sub-cycles, the frames equal), `external-change-cycle` (the watch count and the ledger flat). |
| R8 | A finished scan, search or build shows its results while ronin is idle (no key needed) | FIXED | found by `--leak-check` (the finder, grep and compile cycles never settled); the real main loop's wait ended by the wake three times with app_wake never called. Fixed in e335924; regression "test: ok: wake" (shown to fail with the fix reverted: "app_wake called 0 time(s)"). |
| R9 | Many builds do not grow the remembered build commands' memory | FIXED | `compile-cycle` grew "our committed bytes" (results_compile_remember); fixed in 11e4ab5; regression "test: ok: compile remember" (fails reverted: 8,192 -> 524,288 bytes). |
| R10 | Many project searches and occur runs do not grow memory | FIXED | `grep-cycle/done` grew "our committed bytes" (files_arena); fixed in 6840e88; regression "test: ok: results files_arena" (fails reverted: 16,384 -> 131,072 bytes). |
| R11 | Saving under new names again and again (also names made unique) does not grow the buffer's memory | FIXED | found by reading the code, confirmed by the test; fixed in 0507553; regression "test: ok: write-file meta" (fails reverted: 16,384 -> 131,072 bytes). |
| R12 | Scans, searches and builds do not grow the process heap (the GPU driver's cost for each thread that ends) | FIXED | `finder-cycle/rescan`, `grep-cycle/cancel` grew the process heaps ~700 bytes a cycle; isolated to the hardware D3D11 device (0 without it, with WARP or with only the DLLs loaded); fixed by reusing worker threads in e11ba96; regression "test: ok: thread reuse" (fails reverted: 417,792 bytes over 2,000 tasks). |
| R13 | At exit nothing of ronin's is left: Direct3D objects, arenas, handles | AUTOMATED-NEW | the "exit-check:" block of every dev exit with a window, in the smoke and `--leak-check` (exit 6): only the device listed, only the permanent and scratch arenas, the ledger at 0. |
| R14 | The font's memory is given back at exit | FIXED | the exit check's arena rule; fixed in 338173e; the smoke's check "of the font's arenas ... still reserved after close" fails with the fix reverted (exit 6). |
| R15 | A million random events: no handle, thread, event, process or watch left; memory reported | AUTOMATED-NEW | `--soak 1000000` (bench and debug) in `verify.bat phase`: the leak metrics every 100,000 events, the exact ones flat over the last 500,000. |
| R16 | The leak check catches a leak | AUTOMATED-NEW | the detector proof (a scratch copy): 64 KB never freed in C-x 3 and one pipe handle never closed gave LEAK for exactly window-cycle/split, window-cycle/one, compile-cycle/echo and compile-cycle/error (exit 6), every other scenario OK. |
| R17 | A 10-minute editing session in the release exe: Task Manager's memory stays flat | HUMAN | only real, varied use over time shows it; HUMAN step 34. |
| R18 | Minimizing still gives memory back | HUMAN | needs a real minimize and Task Manager (Q9 measured 60.5 -> 58.1 MB); HUMAN step 35. |

## S. Repository and build-folder cleanup (Phase 17)

| Step | What | Status | Evidence |
|---|---|---|---|
| S1 | No generated file is tracked; the README's images are the only pictures | AUTOMATED | the audit of all 91 tracked files (docs/ARCHITECTURE.md, Measurements (Phase 17)); `git ls-files -ci --exclude-standard` prints nothing; `git status --ignored` lists only build/ (and dist/ once dist.bat ran). |
| S2 | `build.bat clean` removes build\ and dist\ entirely; a junction inside build\ is removed, not followed | AUTOMATED-NEW | verify.bat clean-check, case "present": exit 0, both gone, the junction's target keeps its sentinel file. |
| S3 | `build.bat clean` with nothing to remove succeeds | AUTOMATED-NEW | clean-check, case "missing": exit 0, "clean: nothing to remove". |
| S4 | build\ or dist\ a junction or a file: refused, nothing removed | AUTOMATED-NEW | clean-check, cases "junction" and "file": exit 10, the junction and its target untouched; shown to fail with the junction check removed ("junction (exit 0); junction: removed"). |
| S5 | Outside ronin's repository root `build.bat clean` refuses with a clear message and exit 10; started from another directory it acts on its own directory only | AUTOMATED-NEW | clean-check, case "not-root": exit 10, "clean: refused: ... is not ronin's repository root (no src\ronin.c / verify.bat beside build.bat); nothing removed", its build\ untouched; every case runs from a directory that looks like a repository, whose build\ stays. |
| S6 | The clean-check leaves nothing behind and takes under a second | AUTOMATED-NEW | clean-check removes build\tmp\clean_check (its junctions included) and fails if it lingers; 110 ms (build\verify\clean-check.log). |
| S7 | After `build.bat clean`, verify.bat passes: every fixture comes back by itself | AUTOMATED | verify.bat on an empty build\: PASS (42.7 s); `verify.bat phase` at the end of the phase (f21521a): PASS (387 s; the leak check 22 scenarios, 0 LEAK). |
| S8 | A fresh clone builds and passes verify.bat with build.bat as its only step | AUTOMATED | verify.bat in a `git clone` in build\tmp\fresh, twice (be293bd, and the final code at f21521a): PASS; release 398,848 bytes at f21521a, KERNEL32, USER32, GDI32 (Measurements (Phase 17)). |
| S9 | The screenshot commands of this list work after a clean (`--screenshot build\shots\..`, `--dpi-check build\shots\dpi`) | FIXED | found by reading the code: nothing made build\shots but `--leak-check`, and `--dpi-check` wrote into a directory nothing made (exit 7); fixed in 464ca37 (build.bat makes build\shots, `--dpi-check` makes its directory); `--dpi-check build\shots\dpi` with the directory missing: PASS. |
| S10 | The measurement flags do not read the user's ronin.conf | FIXED | found by reading the code: `--memory-report`, `--bench-latency`, `--startup-capture` and `--bench-longline` read it; fixed in be293bd; regression in "test: ok: command line" (fails with --memory-report left out: "batch flag 14 of the built-in config list reads the user's config"). |
| S11 | `build.bat clean`, `build.bat`, then ronin starts and works | HUMAN | a real start from a fresh build; HUMAN step 36. |
| S12 | After a clean build the exe's Details tab still shows the version and the icon | HUMAN | `(Get-Item build\ronin.exe).VersionInfo` shows 0.1.0, the description and the copyright after the clean; Explorer's Details tab and icon: HUMAN step 37. |
| S13 | Project searches again and again leave the same memory behind, however the workers' timing went; a large search's extra memory goes back on a minimize | FIXED | found by `verify.bat phase`: `--leak-check grep-cycle/done` LEAK in 2 of 4 runs (*search*'s held[0] reaching its 64 KB high-water after the first window); fixed in f21521a (held and pend committed to a 256 KB floor at the buffer's first run, what grew above it given back by app_trim on a minimize); regression "test: ok: results transient" (fails with the floor off: held 4096 / 0, pend 4096; with the trim off: 3,145,728 / 2,097,152 / 1,048,576 after a minimize); `--leak-check grep-cycle` 5 times: OK, 6,459,392 bytes in every sample. |

## T. The modal layer and the command list (Phase 18)

| Step | What | Status | Evidence |
|---|---|---|---|
| T1 | C-S-p opens the command list over the windows: every command of the table, in table order, "136/136" | AUTOMATED-NEW | "test: ok: command list" (C-S-p, the rows against command_at, the status); smoke stage 11. |
| T2 | Each row shows the name, the description and the key bindings in the current window: the two shortest, then "+N", nothing when unbound | AUTOMATED-NEW | "test: ok: command list" (save-buffer "C-x C-s", forward-char "C-f, <right>", undo "C-/, C-_ +1", newline "RET, C-m +1", skip-to-next-like-this ""); smoke stage 11 (a key pixel in modal_key, a description pixel in modal_description). |
| T3 | Typing filters with the fuzzy matcher on the name, the description, or both; name matches first; the empty filter lists the table | AUTOMATED-NEW | "test: ok: command list" (goldens "gotol", "sav buf", "clipboard", "undo", the split query "occur lines" with its spans in both fields); the seeded "modal" group (the shown order equal to a slow full ranking, 256 seeds). |
| T4 | Up / Down, C-n / C-p, C-v / M-v, Page Up / Down move the selection; it stops at both ends | AUTOMATED-NEW | "test: ok: command list" (C-p and <up> at row 0, C-v past the end, <down> and C-n at the last row, M-v <next> <prior>); "test: ok: modal layer". |
| T5 | RET closes the box and runs the selected command on the window as M-x does; a prompting command asks afterwards | AUTOMATED-NEW | "test: ok: command list" (end-of-buffer, mark-whole-buffer's region, goto-line's "Goto line: " prompt after the close); smoke stage 11 ("RET ran end-of-buffer"). |
| T6 | C-g, ESC and a click outside close it without a message; the frame underneath is as before | AUTOMATED-NEW | smoke stage 11 (the frame after C-g equal to the one before C-S-p, byte for byte); `--leak-check modal-cycle/browse` (the frames before and after 220 cycles equal); "test: ok: modal layer" and the seeded "modal" group (point, buffer and windows as before). |
| T7 | The mouse: a click selects a row, a double click runs it, the wheel scrolls the list without moving the selection | AUTOMATED-NEW | "test: ok: command list" and "test: ok: modal layer". |
| T8 | The filter line takes the line editing keys (C-a, C-k, M-DEL, C-y, C-b ...); other commands are refused by name | AUTOMATED-NEW | "test: ok: command list" ("save-buffer is not available in the command list", the field after M-DEL, C-a C-k, C-y C-b C-b x); "test: ok: modal layer" (a cancelled prefix, an undefined key). |
| T9 | Not over the minibuffer; M-x show-command-list opens it too | AUTOMATED-NEW | "test: ok: command list" ("The command list is not available in the minibuffer"; M-x). |
| T10 | A frame too small does not open it; a resize below the minimum closes it, with the message | AUTOMATED-NEW | "test: ok: modal layer" (464 x 176 opens, 456 x 176 and 464 x 175 do not; a resize to 456 px closes it); "test: ok: command list" (300 x 100). |
| T11 | A config saved while it is open applies: the bindings shown, [keys command-list], the colors | AUTOMATED-NEW | "test: ok: command list" (save-buffer moved to C-c s, C-j bound to modal-next, modal_border changed, the box still open). |
| T12 | Every command has a description: at most 60 bytes, starting with a verb, no period | AUTOMATED-NEW | "test: ok: command descriptions"; shown to fail with a 61-byte description and with one ending in a period (Measurements (Phase 18)). |
| T13 | The bindings shown are those that run the command: shadowed ones and ones behind a bound prefix are left out | AUTOMATED-NEW | "test: ok: where-is"; shown to fail with the shadowing check removed and with only one sequence kept. |
| T14 | A filter keystroke takes under 0.2 ms over the whole table and allocates nothing | AUTOMATED-NEW | `ronin_bench.exe --bench-modal` (exit 9 at a 200 us median; measured median 8 us, max 49-158 us); "test: ok: command list" (the arena unchanged over 21 keystrokes); the seeded "modal" group (the arena unchanged while open). |
| T15 | Idle with the box open: 0 frames, 0% CPU | AUTOMATED-NEW | dev helper `ronin_debug.exe --keys "C-S-p a" --idle-check`: PASS, 0 frames, 0 us of CPU in 5 s. |
| T16 | Opening and closing it again and again leaks nothing | AUTOMATED-NEW | `--leak-check modal-cycle` (browse, run): OK, in `verify.bat phase`. |
| T17 | Random use with the box open keeps every invariant | AUTOMATED-NEW | `--soak` (command list sessions and the random keys that reach it; its field's View and Buffer and the list checked); the seeded "modal" group in `--fuzz-seeds`. |
| T18 | The layer as Phase 19 will use it: two panes, other percentages, modal-other-pane | AUTOMATED-NEW | "test: ok: modal layer" (90 / 80 / 30: the box and the panes in whole cells; focus 0 -> 1 -> 0; movement only in the focused pane; a click on the other pane focuses it). |
| T19 | C-S-p on other keyboard layouts | AUTOMATED | the input harness's "every default binding" on US, United Kingdom, Turkish Q and Finnish (C-S-p is a default binding). Note: binding C-S-p takes it from shift-translation (it was a shift-selecting C-p, as in Emacs); S-<up> still selects. |
| T20 | The box looks right: readable, columns aligned, keys right-aligned, at 100% and 150% | HUMAN | a look; HUMAN step 38 (dev helper for the picture). |
| T21 | On the real keyboard in the release exe: C-S-p, a filter, RET runs it; a click outside and ESC close it | HUMAN | real input and focus; HUMAN step 39. |
| T22 | `--test` leaves no junction behind (`git status --ignored` without "Filename too long") | AUTOMATED-NEW | "test: ok: scanner" removes build\tmp\p11scan\src\loop and checks it is gone; test_run removes it again after the group. |

## U. The git changes view (Phase 19)

| Step | What | Status | Evidence |
|---|---|---|---|
| U1 | C-x g in a git work tree opens a two-pane box (90% x 80%, the files 30%) once git has answered: the changed files with git's two-letter status in its order, the status line "<branch>: N changed files" | AUTOMATED-NEW | "test: ok: git changes" (nothing open before rev-parse answered, the 8 rows of the canned repository, "main: 8 changed files"); "test: ok: git real" on build\tmp\gitrepo\repo; smoke stage 12. |
| U2 | The selected file's diff on the right: added green, removed red, @@ lines and headers in their colors | AUTOMATED-NEW | smoke stage 12 (diff_header, diff_hunk, diff_removed, diff_added, git_status pixels with the real git); "test: ok: git parse" (the kinds of every line). Tabs and the cut at the pane's edge have no pixel probe: U18. |
| U3 | Every kind of change: modified, added, deleted, renamed (with its source), untracked (in full), binary, a UTF-8 path, CRLF lines, no commits yet | AUTOMATED-NEW | "test: ok: git parse" (goldens of each); "test: ok: git job" (the three diff command lines exactly, --no-index's exit 1); "test: ok: git changes" (the canned rename, untracked, deleted, ğ.c, binary and CRLF files). |
| U4 | A diff is fetched once per file per open; keys that pass over files fetch none of them | AUTOMATED-NEW | "test: ok: git changes" (calls.txt: each diff once, the two files passed over never); the seeded "git fuzz" (no diff twice since the open or g); shown to fail with a cache that fetches again (Measurements (Phase 19)). |
| U5 | The movement keys select a file; TAB moves to the diff, where they scroll it; TAB back; the wheel scrolls the pane under the pointer | AUTOMATED-NEW | "test: ok: git changes" (TAB, <down>, C-v a page, the wheel on the diff pane, TAB back); "test: ok: modal layer" (the scroll pane). |
| U6 | RET visits the selected file in the selected window at its first change; on a deleted file it says so and the box stays | AUTOMATED-NEW | "test: ok: git changes" (a.c at line 3, the untracked file and the renamed one at line 1, a binary one as find-file; "gone.c is deleted in the working tree"); smoke stage 12 (a.c at line 2 with the real git). |
| U7 | g runs git status again and forgets every diff | AUTOMATED-NEW | "test: ok: git changes" (status called again, big.c's diff gone, a.c fetched again). |
| U8 | C-g, ESC and a click outside close the box at once, also while git still runs; the git stops, nothing is waited for; the frame underneath is as before | AUTOMATED-NEW | "test: ok: git errors" (C-g under 50 ms with a hanging status, the process gone within 2 s, a click outside); `--leak-check git-cycle/cancel`; smoke stage 12 (the frame after C-g equal to the one before, byte for byte). |
| U9 | No git: a clear message, nothing opens; not a work tree: the same; a refusal of git's in git's own words | AUTOMATED-NEW | "test: ok: git errors" ("git not found: ...", "git not found on PATH (set git_path in [settings])", "Not a git work tree: ...", "git: fatal: detected dubious ownership in repository at 'C:/x/y'"); "test: ok: git job". |
| U10 | A failing status or diff shows git's line in the status line (the diff's output in the pane); a silent git is stopped after git_timeout_ms ("git timed out") | AUTOMATED-NEW | "test: ok: git errors" ("git: fatal: unable to read index", "git: fatal: bad object HEAD", the timeout before and after the open); "test: ok: git parse" (the line picked, its 120-byte cut). |
| U11 | Limits: a diff over 20,000 lines and a list over 5,000 files are cut with a last line saying so; git is stopped there | AUTOMATED-NEW | "test: ok: git parse" (both limits); "test: ok: git job" (git stopped past the line limit, not read to its end); "test: ok: git changes" (big.c's cut line). The list's cut row is drawn from the parser's cut flag; no test opens the view on more than 5,000 files. |
| U12 | The editor stays responsive while git runs: typing never waits; a 20,000-line diff is shown within one frame budget | AUTOMATED-NEW | git runs on a worker and the main thread never waits for it ("test: ok: git errors": C-g under 50 ms with git hanging); `--bench-git` (the copy in one frame, its longest slice 69-131 us; exit 9 over 2.5 ms); "test: ok: git changes" (the copy sliced at 4,096 lines a frame). |
| U13 | Idle with the box open after the diff arrived: 0 frames, 0% CPU | AUTOMATED-NEW | dev helper `ronin_debug.exe --keys "C-x g" --idle-check` from the repository root: "a modal box open 1, git jobs running or pending 0", then PASS, 0 frames, 0 us of CPU in 5 s. |
| U14 | Opening and closing it again and again leaks nothing: memory, handles, threads, processes | AUTOMATED-NEW | `--leak-check git-cycle` (browse, cancel): OK, the handle ledger and our processes exact, in `verify.bat phase`. |
| U15 | Random use with the box open keeps every invariant | AUTOMATED-NEW | `--soak` (git sessions with the fake git, the random keys that reach C-x g and [keys git-changes], the view's invariants at every check); the seeded "git" group in `--fuzz-seeds`. |
| U16 | A config saved while it is open applies: [keys git-changes], the colors; git_path, the limits and the timeout from the next run | AUTOMATED-NEW | "test: ok: git changes" (C-j bound to modal-previous, diff_added changed, diff_hunk following number); "test: ok: git errors" (git_timeout_ms). |
| U17 | The real git, whatever the machine's git configuration: the list and the diffs as git prints them | AUTOMATED-NEW | "test: ok: git real" (its system and global configuration shut out; equal to the fake git's canned answers); skipped with a log line without git on PATH. |
| U18 | The box looks right on a real repository: readable, the status letters lined up, the colors tell the kinds apart, tabs line up as in buffers, long lines end at the pane's edge, at 100% and 150% | HUMAN | a look; HUMAN step 40 (dev helper for the picture). |
| U19 | On the real keyboard in the release exe, in ronin's own repository: C-x g, select, TAB, scroll, RET, g, C-g while git runs | HUMAN | real input and focus, git's speed on a real tree; HUMAN step 41. |

## V. Terminals and the AI terminal (Phase 20)

| Step | What | Status | Evidence |
|---|---|---|---|
| V1 | C-c t in a file opens Windows Terminal in the file's directory | HUMAN | a real terminal takes the foreground, so no test opens one; HUMAN step 42. The command line and its directory are checked exactly with the fake terminal: "test: ok: terminal", smoke stage 13. |
| V2 | C-c a opens Windows Terminal in the project's root running claude | HUMAN | the same; HUMAN step 43. Automated with the fake: "test: ok: terminal" (the root, `"<cmd>" /s /k "claude"`), smoke stage 13. |
| V3 | `terminal = cmd` opens a classic console (C-c a: claude running in it) | HUMAN | the same; HUMAN step 44. Automated with the fake: "test: ok: terminal" (cmd.exe with a new console, `/s /k "claude"`). |
| V4 | Closing ronin leaves both terminals running | HUMAN | the processes of a real session; HUMAN step 45. What ronin keeps of them is checked: no handle and no job ("test: ok: launch": the ledger and the process's handles unchanged after 100 launches, outside any job). |
| V5 | A buffer without a file uses the default directory (the one given on the command line, `ronin <directory>`, else the current one) | HUMAN | the look of the terminal's prompt; HUMAN step 42. The directory itself: "test: ok: terminal" (*scratch* with and without a command-line directory, for both commands). |
| V6 | `ai_provider = openai` runs codex, `custom` runs `ai_command`, `ai_command` runs for any provider | AUTOMATED-NEW | "test: ok: terminal" (codex, my-ai --x, a quoted program path with openai); "test: ok: terminal config" (every value). |
| V7 | The messages: "ai_command is empty", "Windows Terminal (wt.exe) not found", "Could not start ...: error N", "Opened ... in ...", refused in the minibuffer and in the command list | AUTOMATED-NEW | "test: ok: terminal" (each message exactly, nothing started for the refusals). |
| V8 | A directory with a space or `;` and a drive root open wt there | HUMAN | wt's own parsing of `\;`; HUMAN step 46. The command lines (`-d "C:\\"`, `\;` for wt only): "test: ok: terminal". |
| V9 | A config saved with another ai_command applies to the next C-c a; bad values are reported with their line | AUTOMATED-NEW | "test: ok: terminal" (a reload); "test: ok: terminal config" (the diagnostics, the earlier value kept). |
| V10 | Launching again and again keeps nothing: handles, processes, memory | AUTOMATED-NEW | "test: ok: launch", "test: ok: terminal" (200 launches); `--leak-check terminal-cycle` (220 launches, 0 LEAK); the soak's terminals (one call per launch). |
| V11 | Started inside a job that refuses breaking away, the terminal still starts | AUTOMATED-NEW | "test: ok: launch" (a child in a build's job: "breakaway refused (error 5) ... started inside ronin's job", the grandchild's file written). |
| V12 | No test, smoke, bench, soak or leak check starts a real terminal or a real git it did not choose | AUTOMATED-NEW | the guard: "terminal: no real terminal in a test run", "git: no real git in a test run" ("test: ok: terminal", "test: ok: git errors"), `os_dev_real_refused` 0 at the end of `--test`, every fuzz worker, the soak and the leak check; the modal fuzz runs with the fake git and the fake terminal. |
| V13 | git's warning between two lines of a diff's hunk does not cut the hunk | FIXED | without the fix "git parse: diff 9" fails (the warning kept as a context line, `-b` and `+B` drawn as headers), and so does "git changes: status 'main: 8 changed files', panes 8 / 15" (the warning on the fake git's stderr made a row); with the rename's source left alone "git parse: a warning before a source" fails; fixed in d941010. |
| V14 | Typing is as fast as before | AUTOMATED | `--bench-latency` against Phase 19 (Measurements (Phase 20)). |

## W. Themes (Phase 21)

| Step | What | Status | Evidence |
|---|---|---|---|
| W1 | `theme = vague` in ronin.conf changes every color on the next save | AUTOMATED-NEW | "test: ok: set-theme" (a reload of ronin.conf naming another theme applies it), "test: ok: themes masked" (a theme named in ronin.conf at the start); the look: HUMAN step 47. |
| W2 | M-x set-theme switches live, with completion over every theme (built-in and files, each marked with where it comes from, the current one marked); nothing is written | AUTOMATED-NEW | "test: ok: set-theme" (the exact candidates and notes, ronin.conf's bytes unchanged); smoke stage 14 (every built-in theme through the keys, its frame against its file); `--leak-check theme-cycle`. |
| W3 | ronin.conf's `[colors]` still overrides single keys, over any theme; the masked-theme note ("ronin.conf: N [colors] keys override theme X") in the echo area and once in *Messages* per load, none for N = 0 or under ronin, an error first | AUTOMATED-NEW | "test: ok: themes" (the order), "test: ok: themes fuzz" (300 random themes and [colors] against a reference), "test: ok: themes masked" (30, 2, 1, 0 keys, ronin, an error), "test: ok: set-theme" (the combined echo). |
| W4 | An unknown theme falls back to ronin with a message naming its line | AUTOMATED-NEW | "test: ok: themes" ("ronin.conf:2: theme: no theme 'nosuch' (using ronin)"), "test: ok: theme reload" (the theme file removed). |
| W5 | A file in themes\ is picked up without a restart, also when themes\ itself is created after the start; another theme's file reloads nothing | AUTOMATED-NEW | "test: ok: theme reload" (real watches: the folder created, the file added, written while chosen, another written, removed). |
| W6 | A file with a built-in theme's name replaces it; ronin.conf's folder before ronin.exe's | AUTOMATED-NEW | "test: ok: themes" (ronin and vague replaced, then the built-in again; the two folders' order), "test: ok: set-theme" (marked "replaces the built-in one"). |
| W7 | A theme file with a bad color is reported with its file and line, the rest applies; [settings] or [keys] in a theme are reported and ignored | AUTOMATED-NEW | "test: ok: themes" (every diagnostic's exact text). |
| W8 | The four ported themes look right | HUMAN | a judgement by eye; HUMAN step 47 (docs/images/themes/*.png are the reference). Their colors are checked exactly: smoke stage 14 (background, selection, cursor, text, both mode lines, the divider against each file) and six pixels of each screenshot (Measurements (Phase 21)). |
| W9 | Switching a theme draws one frame: no font set-up, nothing grows, ronin's frame comes back byte for byte | AUTOMATED-NEW | smoke stage 14 (the frame back on ronin equal; the smoke's later check that the font was set up once); `--leak-check theme-cycle` (220 cycles of five switches, 0 LEAK, the screenshots equal). |
| W10 | The built-in themes stay under 8 KB in the exe; the build stops otherwise (and without themes\ronin.conf, or with a file name that is no theme name) | AUTOMATED | every build (make_themes: "5 themes, 4730 bytes embedded"); the three refusals were run once on scratch folders (Measurements (Phase 21)). |
| W11 | Typing, idle and startup are as before | AUTOMATED | `--bench-latency` and `--startup-ms` against Phase 20, `--idle-check` (Measurements (Phase 21)). |
| W12 | A ronin.conf created by C-c , before Phase 21 masks every theme and says so | AUTOMATED-NEW | "test: ok: themes masked" (a [colors] with the 30 keys); with your own file: HUMAN step 48. |

## X. README, contribution guide and repository files (Phase 22)

| Step | What | Status | Evidence |
|---|---|---|---|
| X1 | Every key in README's key tables runs the command named on its row, written as describe-key prints it | AUTOMATED-NEW | "test: ok: readme" (59 keys in 11 key tables: global, search-results and compilation through describe-key itself, isearch and git-changes through their keymap then the global one); the proof: `project-find-files` on README.md:180 of a scratch copy made `--test` exit 9 with "README.md:180: `C-x p f`: describe-key says ..." (Measurements (Phase 22)). |
| X2 | Every setting in README exists, with a valid default, and README's config examples load without a message | AUTOMATED-NEW | "test: ok: readme" (12 settings, 2 config blocks, the Configuration section's names); the proof: `font_sise` on README.md:107 made `--test` exit 9 with "README.md:107: unknown setting 'font_sise'". |
| X3 | Every relative link and #anchor of README and CONTRIBUTING resolves; `M-x` names and prose keys exist | AUTOMATED-NEW | "test: ok: readme" (34 links, 22 prose keys, commands and names); the checker's 16 broken fragments, each caught at its line, in every run. |
| X4 | oxide's diff_added is its palette's "added" green | HUMAN | A comparison with oxide's source, which no test can fetch; done once in Phase 22. To repeat it: open https://github.com/oxidescheme/nvim/blob/838760c820821cce6a79706430883439c80dad4c/lua/oxide/colors.lua, line 26 (`green = "#5bb661", ... added items`), against `diff_added` in themes\oxide.conf. That the file still parses clean is AUTOMATED: "test: ok: themes" and smoke stage 14. |
| X5 | README reads well on GitHub, also at phone width, and every image loads | HUMAN | GitHub's rendering after the push; HUMAN step 49. |
| X6 | The workflow's first run: the builds and the tests pass on windows-latest; the informational smoke says whether WARP renders there | HUMAN | the user pushes; HUMAN step 50. The steps were tried locally in Windows PowerShell (the imports, --version, `--test` on bench, the smoke on bench: each exit 0); the YAML itself runs only on GitHub. |
| X7 | New issue offers the two templates and no blank issue; a pull request opens with the checklist | HUMAN | GitHub's UI; HUMAN step 51. |
| X8 | The external links of README and CONTRIBUTING lead where they say | HUMAN | the test logs them ("test: readme: README.md:N: external link ..."); HUMAN step 52. |

## HUMAN steps

In this order, with `build\ronin.exe` (or ronin_debug.exe):

1. **A1, A3**: Start ronin from Explorer, then from a terminal, then from a pinned taskbar icon. There
   must be no white flash, and the title must read "*scratch* - ronin".
2. **A2**: Covered by `--type-ahead-check` since Phase 10; nothing to do by hand.
3. **A5, A6**: Drag a window corner around. The text stays crisp, nothing is stretched, and no black
   areas appear. Then minimize, restore and maximize.
4. **A8**: Alt+Tab to another app: the cursor is hollow. Come back: it is filled.
5. **B4**: Hold a letter key. The repeat must be smooth.
6. **C9**: Open a 100 MB file (`build\tmp\bench_100mb.txt`, made by `ronin_bench.exe --bench-buffer`), press M->,
   and scroll with the wheel.
7. **G11**: The same with a 100 MB C file (`build\tmp\bench_syntax_100mb.c`, made by `ronin_bench.exe
   --bench-syntax`): M->, the colors appear within about a second, and typing is never blocked.
8. **E14**: Select two lines in ronin, M-w, paste in Notepad: two lines.
9. **E15**: Copy text from a browser, C-y in ronin.
10. **I4**: Set `render_mode = classic`, then natural, then symmetric in ronin.conf. Each looks different.
11. **I8**: Edit ronin.conf in Notepad and save. The change applies; the default theme never flashes.
12. **J9**: Leave a modified file and sign out of Windows. The sign-out is blocked with "Unsaved
    changes in ronin", and ronin asks to save.

Phase 10, with two or three windows (C-x 3, C-x 2):

13. **L22, L23**: Drag the line between two windows, then a mode line with a window below it. The line
    follows the pointer without jumping; over the line and the mode line the pointer is the left-right /
    up-down resize cursor, the arrow over text and over the bottom mode line.
14. **L25**: Look at the dividers and the inactive mode lines at 100% and at 150% display scale (dev
    helper: `ronin_debug.exe src\app.c --scale 150 --keys "C-x 3 C-x 2" --screenshot build\shots\l25.png`).
15. **L30**: With four windows, drag the ronin window to its smallest size, then back. Nothing looks
    broken while it is small, and every window comes back as it was.

Phase 11, projects:

16. **M6**: Start ronin on a big real tree (a large repository, or `ronin C:\Windows`), C-x p f, and type at
    once. The list fills in while "scanning: N files" counts up, and the typing is never held up.
17. **M16**: Make a project first (no tool makes it; any file deep in a project works): `build\tmp\p11long` with
    a `.git` directory, a `short.c`, and a `thefile.c` several long directory names deep. Then
    `ronin_debug.exe build\tmp\p11long\short.c --keys "C-x p f t h e f i l e" --screenshot build\shots\m16.png`:
    the long row starts with "…" and keeps the file name; with the window narrowed by hand, the same.
18. **M23, M24**: Type in Notepad while `ronin_debug.exe --test` and `ronin_bench.exe --bench-project` run.
    Notepad keeps every key and the focus; `--test` passes (lines "test: inconclusive: ..." in build\ronin.log
    are allowed, a FAIL is not).

Phase 12, project-wide search:

19. **N9**: In a big real repository, C-x p g a common word and type in another window at once. The list
    fills in from the top, the last line counts up, and the typing is never held up.
20. **N21**: While typing in Notepad, start `build\ronin.exe --no-activate` from a terminal. Notepad keeps
    the focus and every key; ronin's window appears behind it.

Phase 13, builds:

21. **O23**: In ronin's repository put an error in a source file (say `int x = y;` in src/edit.c), C-x p c RET
    (`.\build.bat`). Look at *compilation*: the error in red with its file and line, M-g n opens the file at
    that line with the line highlighted. Undo the error and C-x p c RET again: "Compilation finished".
22. **O20**: Start `build.bat release` with C-x p c and type in another window at once. The typing is never
    held up while the output arrives.
23. **N13**: While a C-x p g search runs on a big repository, run a build (C-x p c) and `git status` /
    `git checkout -- <file>` in a terminal. Both succeed; nothing complains that a file is in use.
24. **O10**: M-x compile with `ping -t localhost`, wait a few lines, C-c C-k. *compilation* says
    "Compilation killed"; Task Manager shows no ping.exe and no cmd.exe started by ronin.

Phase 14, multiple cursors:

25. **P12**: In a file, Ctrl+Shift+click three places: three cursors, "mc:4" in the mode line. Ctrl+Shift+click one of
    them again: it goes. A plain click: one cursor.
26. **P8**: With three cursors on three lines, C-k, then paste in Notepad: three lines, the killed texts in order.
27. **P20**: Open a long file, C-x h C-S-c C-S-c (10,000 cursors), type a word, DEL, RET RET. Nothing lags (dev
    helper for the look, after `ronin_bench.exe --bench-cursors` made the file: `ronin_debug.exe
    build\tmp\bench_cursors.c --keys "C-x h C-S-c C-S-c" --screenshot build\shots\p14_many.png`).

Phase 15, the release:

28. **Q4**: Look at the icon in the title bar, the taskbar, Alt+Tab and Explorer (large icons view) at
    100% and 150% display scale: an "r" with a green block cursor on a rounded square of the background color, crisp.
29. **Q3**: Explorer, right-click build\ronin.exe, Properties, Details: version 0.1.0, "ronin text editor",
    the copyright.
30. **Q7**: Put `early_window = true` in ronin.conf (C-c ,) and start ronin from Explorer five times: the
    window appears at once in the background color and the text follows; no white or black flash, no
    jump. Then decide the default: say whether `early_window` should be on.
31. **Q9**: Minimize ronin, wait a few seconds, restore it: it comes back at once with its text, nothing
    garbled; Task Manager shows its memory lower while minimized.
32. **Q6**: `build\ronin_debug.exe x.txt --keys "h i" --crash --recovery-dir "%LOCALAPPDATA%\ronin\recovery"`
    (x.txt any scratch file), then start `build\ronin.exe`: the echo area says 1 recovery file;
    M-x recover-file RET shows x.txt and the copy with "hi" side by side. Delete the files afterwards.
33. **Q2**: In cmd and in PowerShell, `build\ronin.exe --version`: "ronin 0.1.0" appears (in cmd it may
    come after the next prompt, as for any windowed program); no window opens.

Phase 16, leaks:

34. **R17**: Start `build\ronin.exe` on ronin's repository, open Task Manager (Details tab, add the "Commit size"
    column) and note ronin's commit size. For 10 minutes, edit as usual: open and kill files, C-x p f, C-x p g,
    a build (C-x p c), isearch, split and join windows, undo a lot. Then kill the extra buffers, C-x 1, wait a
    few seconds: the commit size is back within a few MB of the start (the arenas keep their high-water marks)
    and does not keep rising when you repeat the same work.
35. **R18**: In the same session note the commit size, minimize ronin and wait 5 seconds: it drops by a few MB.
    Restore it: the window comes back at once with its text.

Phase 17, cleanup:

36. **S11**: In a command prompt in the repository (ronin closed): `build.bat clean`, then `build.bat`, then
    `build\ronin_debug.exe`. The window opens with *scratch* and the title "*scratch* - ronin"; typing works.
37. **S12**: `build.bat release`, then in Explorer right-click build\ronin.exe, Properties, Details: version 0.1.0,
    "ronin text editor", the copyright; Explorer shows the icon (the "r" with the green block cursor).

Phase 18, the command list:

38. **T20**: `build\ronin.exe src\app.c`, C-S-p, type "file": the box is readable, the three columns line up, the keys
    are right-aligned, the selected row stands out. The same at 150% display scale (dev helper for the picture:
    `ronin_debug.exe src\app.c --scale 150 --keys "C-S-p f i l e" --screenshot build\shots\t20.png`).
39. **T21**: In `build\ronin.exe` press Ctrl+Shift+P, type "find", RET: the box closes and "Find file:" asks;
    C-g. Ctrl+Shift+P again and click outside the box: it closes, nothing moved; once more and ESC: the same.

Phase 19, the git changes view:

40. **U18**: In a git work tree with a few changes, `build\ronin.exe <a changed file>`, C-x g: the files and the diff
    are readable, the status letters line up, added, removed, @@ and header lines are told apart at a glance; a line
    with tabs lines up as in the buffer, a long line ends at the box's edge. The
    same at 150% (dev helper for the picture: `ronin_debug.exe <file> --scale 150 --keys "C-x g" --screenshot
    build\shots\u18.png`).
41. **U19**: In `build\ronin.exe` on ronin's own repository with uncommitted changes: C-x g, Down to another file
    (its diff appears), TAB, C-v and M-v (the diff scrolls), TAB, RET (the file at its first change); C-x g again,
    g (the list again), C-g at once: it closes without waiting.

Phase 20, terminals (Windows Terminal installed, `claude` on PATH; each step opens real windows):

42. **V1, V5**: `build\ronin.exe src\app.c`, C-c t: Windows Terminal opens with its prompt in ...\text-editor\src and
    the echo area says "Opened terminal in .../src". Then C-x b `*scratch*` RET, C-c t: the prompt is in the directory
    ronin was started from (its current directory). Close ronin, start `build\ronin.exe src`, C-c t: the prompt is in src.
43. **V2**: In src\app.c, C-c a: Windows Terminal opens in ...\text-editor (the project's root) running claude. Leave
    claude (`/exit`): the cmd prompt stays open.
44. **V3**: C-c , and add `terminal = cmd` under [settings], save. C-c t: a classic console window opens in src; C-c a:
    one running claude in the root. Remove the line again.
45. **V4**: With the terminals of steps 42-44 still open, C-x C-c: ronin closes, every terminal stays open and claude
    still answers.
46. **V8**: Make a folder `C:\temp\a;b c` with a file x.txt in it, `build\ronin.exe "C:\temp\a;b c\x.txt"`, C-c t: wt's
    prompt is in exactly `C:\temp\a;b c` (one tab, not two). Then `ronin C:\`, C-x b `*scratch*` RET, C-c t: the prompt
    is in `C:\`.

Phase 21, themes:

47. **W1, W8**: With `build\ronin.exe src\app.c` open, add `theme = vague` under [settings] in ronin.conf (C-c ,) and
    save: every color changes at once. Then M-x set-theme RET moonfly, oxide and mellifluous in turn, with a region and a
    second window (C-x 3) on screen: each looks like its picture in docs/images/themes (and like the theme in Neovim, if at
    hand); the text stays readable on every background, the region and the cursor are visible.
48. **W12**: If your ronin.conf was written by C-c , before Phase 21, its [colors] section lists every color: with
    `theme = vague` the echo area says "ronin.conf: 30 [colors] keys override theme vague" and the colors stay ronin's.
    Delete the [colors] lines (keep the header) and save: vague's colors appear and the message does not.

Phase 22, the repository on GitHub (after you push):

49. **X5**: Open the repository's page on GitHub on a desktop browser and on a phone (or the browser's phone width): the
    title, the sentence, the link line and the hero picture come first; every image loads (the hero, the command list,
    the four themes, the finder, isearch, *compilation*, the git changes view, the cursors); the tables are readable
    (on a phone they scroll sideways); the links of the top line jump to their sections.
50. **X6**: The Actions tab: the first run of "build" on your push. The build, test, imports and version steps are green.
    Open the step "Smoke (bench build; informational: ...)" and report its last lines (the session, "--smoke exited N",
    the d3d11 lines): they go into Measurements (Phase 22). If a test step failed, the artifact "ronin-logs" holds its log.
51. **X7**: Issues, New issue: "Bug report" and "Feature request" are offered and no blank issue. Start a pull request
    from any branch (and close it): its text is the checklist.
52. **X8**: Follow the external links (the list is in the report of Phase 22, and in build\ronin.log after `--test` as
    "test: readme: ... external link ..."): Releases (empty until the first release), the four theme repositories.

Dev helpers that take most of the work out of the others: `ronin_debug.exe --idle-check` (A4, Z1),
`ronin_debug.exe --dpi-check build\shots\dpi` (A9), and `ronin_debug.exe --clipboard-check` (E14,
E15; copy some plain text, e.g. from Notepad, first).
