# Manual test list

Every step a person would check by hand. Each step has one status:

- **AUTOMATED**: a test that existed before this list covers it.
- **AUTOMATED-NEW**: a test was added for it when the list was written (through the real input
  harness, `--keys`, the headless app, a smoke probe, a bench check or a dev helper).
- **FIXED**: it was broken. The row names the failing test and the commit that fixed it.
- **HUMAN**: only a person can judge it (visual quality, the real clipboard, Task Manager, a DPI
  change, Windows logoff, real focus). The row says why. The steps are under "HUMAN steps" at the end.

"Note" marks a place where the step's wording and what teal does differ by design (Emacs
behavior); nothing there is a bug.

Every phase appends its own steps here, with the same statuses (see CLAUDE.md).

## Where the evidence is

| Name used below | What it is |
|---|---|
| `--test` | `build/teal_debug.exe --test` (or teal_bench.exe): src/test.c, then `win32_dev_test_mods`, `win32_dev_test_keys` and the input harness. Each group logs a `test: ok: ...` line to build\teal.log. |
| harness | src/win32_input_test.c, inside `--test`: real key messages (scan codes, flags, AltGr, TranslateMessage, ToUnicodeEx) to a window that is never shown, using the real window procedure, on US, United Kingdom, Turkish Q and Finnish layouts loaded for the test thread only. Log: `test: ok: input <layout> ...`. |
| smoke | `build/teal_debug.exe --smoke`: pixel probes on rendered frames. Log: `smoke: ok: ...`. |
| manual tests | test.c `test_manual_*`: the steps of this list through the headless app. Log: `test: ok: manual <ids> ...`. |
| `--idle-check` | Shows the window without focus. After 1 s it measures 5 s of idle: CPU time, frames, private bytes. |
| `--dpi-check <dir>` | WM_DPICHANGED to 125% and back, with a screenshot at each step. Checks the scaling, the font set-ups, and that the frame after coming back is identical to the first. |
| `--clipboard-check` | The real clipboard, only when it holds plain text or nothing. It saves the text, puts it back, and keeps everything out of clipboard history. |
| `--log-keys` | Every keyboard message, what teal read from it, and the command it ran. |

## A. Startup and window

| Step | What | Status | Evidence |
|---|---|---|---|
| A1 | Start from Explorer: no white flash; title "*scratch* - teal" | HUMAN | The title is AUTOMATED: smoke `win32_smoke_check_title` ("window title '*scratch* - teal', set 1 time(s)"). The flash needs eyes: the window is shown cloaked until the first Present. |
| A2 | Type as soon as the window is visible: characters go to teal | HUMAN | Needs real focus. The smoke never takes the focus. |
| A3 | Same from a terminal and from the taskbar | HUMAN | Needs the shell. |
| A4 | Idle with the mouse outside: 0% CPU | AUTOMATED-NEW | `--idle-check`: "PASS: 5000 ms idle: CPU 0 us (0.00% of one core), 0 frame(s), 0 main loop pass(es)". Run on demand. |
| A5 | Drag-resize: crisp, no stretching, no black areas | HUMAN | Visual, during the modal size loop. |
| A6 | Minimize, restore, maximize | HUMAN | Window manager interaction. |
| A7 | Alt alone, F10, Alt+Space: no system menu, no beep | AUTOMATED-NEW | harness, every layout: "Alt alone, F10, Alt + Space": 0 messages left to DefWindowProc (the only source of the menu and the beep), 0 SC_KEYMENU. F10 describes as `<f10>`, Alt+Space as `M-SPC`. |
| A8 | Another app: hollow cursor; back: filled | HUMAN | Real focus. The drawing is AUTOMATED: smoke stage 0 "buffer: hollow cursor", stage 1 "buffer: filled cursor". |
| A9 | Display scale 125% and back: crisp at both | AUTOMATED-NEW | `--dpi-check build/shots/dpi`: "PASS: 96 -> 120 -> 96 dpi: client 1280x800 -> 1604x1010 -> 1280x800, font set up again at each change, the frame after coming back equals the first". The 100% and 125% screenshots were inspected: crisp. A real monitor change is still worth one look. |
| A10 | Alt+F4 with nothing modified: exits without asking | AUTOMATED-NEW | harness: Alt+F4's key is the one message teal leaves to DefWindowProc, and SC_CLOSE makes the unmodified app quit. Windows only turns a *posted* Alt+F4 into SC_CLOSE for an active window, so the harness sends SC_CLOSE itself. |

## B. Keyboard and text

| Step | What | Status | Evidence |
|---|---|---|---|
| B1 | Type ğüşıöç ĞÜŞİÖÇ | AUTOMATED-NEW | harness Turkish Q: "12 of 12 Turkish letters typed from their keys". Pasting is AUTOMATED: `test_clipboard` round trip with ş. |
| B2 | { } [ ] \ \| @ # $ ~ each insert one character | AUTOMATED-NEW | harness, all four layouts: "symbols typed from their keys" (AltGr, and a dead ~ followed by Space on Finnish). |
| B3 | Dead key then letter: composed character, no command | AUTOMATED-NEW | harness Turkish Q and Finnish: "dead key then e" for ´ ` ^ ¨ gives é è ê ë. "e right after Left Alt + dead key" gives e: this found bug 3. |
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
| C2 | teal +120:8 file: line 120 centered, L120 C7 | AUTOMATED-NEW | manual tests C2: "L120 C7", the top line within 1 of 119 − rows/2. |
| C3 | Nonexistent path: empty buffer, "(New file)" | AUTOMATED-NEW | manual tests C3 (from the command line). find-file is AUTOMATED: `test_find_write` "(New file)". |
| C4 | Directory path: error message, *scratch* stays | AUTOMATED-NEW | manual tests C4: "Cannot open <path>: is a directory", *scratch* current. |
| C5 | Edit: "**"; C-x C-s: "Wrote ...", "**" gone | AUTOMATED-NEW | manual tests C5. |
| C6 | CRLF file: edit one line, save: still CRLF, only that line differs | AUTOMATED | `test_edits` edit_crlf.txt (whole-file byte compare); "test: ok: load, edit, save, reload". |
| C7 | Windows-1254 file: boxes; insert ASCII, save: other bytes unchanged | AUTOMATED-NEW | manual tests C7 (the bytes FE FD F0 DE DD D0 kept byte for byte, CRLF kept). Boxes inspected on build/shots/c7.png. |
| C8 | Read-only file: "%%"; typing: "Buffer is read-only", no sound | AUTOMATED-NEW | manual tests C8. **Note:** the message is "Buffer is read-only: ro.txt" (with the buffer name, as Emacs). No sound: teal never calls MessageBeep, and nothing it gets reaches DefWindowProc's beep (A7). |
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
| E15 | Copy in a browser, C-y in teal | HUMAN | Needs a browser. Most of it: `--clipboard-check` (CRLF text put there as by another program yanks as LF lines); fake clipboard: `test_kill` "clipboard: yanked with LF". |
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
| I1 | C-c , opens teal.conf (created under %APPDATA%\teal) | AUTOMATED-NEW | manual tests I1: open-config creates the file from the defaults ("Created <path> from the built-in defaults"), visits it, and watches its new directory. The %APPDATA%\teal location is `app_config_path`. **Note:** a teal.conf next to teal.exe wins (portable). |
| I2 | background = #202020, save: applied, "Reloaded teal.conf" | AUTOMATED-NEW | manual tests I2 (a directory notification, then the settle: "Reloaded teal.conf", background 202020); pixels: smoke "background after loading a config with background = #102030". |
| I3 | font_size = 14: applied | AUTOMATED-NEW | smoke "font_size = 14 in the config: 10x22, 1 more set-up". |
| I4 | render_mode classic / natural / symmetric: visibly different | HUMAN | "Visibly" needs eyes. The three screenshots differ (build/shots/mode_*.png, different MD5s); the differences are subtle at this size. |
| I5 | tab_width = 8 | AUTOMATED-NEW | manual tests I5 (column 8 after a tab, after a reload). |
| I6 | "C-z undo" works | AUTOMATED-NEW | manual tests I6. |
| I7 | Unknown command: "teal.conf:N: unknown command ...", editing goes on | AUTOMATED-NEW | manual tests I7 ("teal.conf:2: unknown command 'no-such-command'", then typing). |
| I8 | Save teal.conf from Notepad: applied; no default-theme flash | HUMAN | Notepad. Mechanism AUTOMATED: `test_hot_reload` (save by rename, truncate-then-write read once after the 50 ms settle, a locked file retried). **Note:** a live reload that cannot read the file keeps the old config; only at startup is a locked config replaced by the defaults until the retry. |

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
| J9 | Logoff with an unsaved file: blocked, "Unsaved changes in teal" | HUMAN | A real logoff. Everything teal does in it is AUTOMATED-NEW in the smoke: WM_QUERYENDSESSION refused, the reason reads exactly "Unsaved changes in teal", the save question, a repeated query, an abort, a cancel, and allowed once saved. |

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

## HUMAN steps

In this order, with `build\teal.exe` (or teal_debug.exe):

1. **A1, A3**: Start teal from Explorer, then from a terminal, then from a pinned taskbar icon. There
   must be no white flash, and the title must read "*scratch* - teal".
2. **A2**: Start it and type `abc` right away. The text must land in teal.
3. **A5, A6**: Drag a window corner around. The text stays crisp, nothing is stretched, and no black
   areas appear. Then minimize, restore and maximize.
4. **A8**: Alt+Tab to another app: the cursor is hollow. Come back: it is filled.
5. **B4**: Hold a letter key. The repeat must be smooth.
6. **C9**: Open a 100 MB file (`build\tmp\bench_100mb.txt`), press M->, and scroll with the wheel.
7. **G11**: Same file renamed to .c: M->, the colors appear within about a second, and typing is never blocked.
8. **E14**: Select two lines in teal, M-w, paste in Notepad: two lines.
9. **E15**: Copy text from a browser, C-y in teal.
10. **I4**: Set `render_mode = classic`, then natural, then symmetric in teal.conf. Each looks different.
11. **I8**: Edit teal.conf in Notepad and save. The change applies; the default theme never flashes.
12. **J9**: Leave a modified file and sign out of Windows. The sign-out is blocked with "Unsaved
    changes in teal", and teal asks to save.

Dev helpers that take most of the work out of the others: `teal_debug.exe --idle-check` (A4, Z1),
`teal_debug.exe --dpi-check build\shots\dpi` (A9), and `teal_debug.exe --clipboard-check` (E14,
E15; copy some plain text, e.g. from Notepad, first).
