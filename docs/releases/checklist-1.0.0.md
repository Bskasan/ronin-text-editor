# ronin 1.0.0: the HUMAN checklist

Every step of docs/MANUAL_TESTS.md that only a person can do, for the owner to fill in before `git push origin v1.0.0`.
"What to do" is short: the full text of steps 1-59 is under "HUMAN steps" in docs/MANUAL_TESTS.md, and steps 60-66 are
section AA there. Use the zip's exe (dist\ronin-1.0.0-windows-x64.zip, unpacked) where a step says so, `build\ronin.exe`
elsewhere.

**Blocks the tag:** the tag waits until every step marked "yes" is done: 60 (SmartScreen), 61 (a clean folder), 62 (the
configuration created on the first run), 47 (themes) and 41 (`C-x g` on a real repository, with 40 for its look). Steps
63-66 come after the tag, in that order.

Do step 36 (`build.bat clean`) only after the release is published: it removes dist\, and the zip in dist\ is the file
whose SHA-256 the release notes give.

| step | tests | what to do | blocks the tag | status |
|---|---|---|---|---|
| 1 | A1, A3 | Start ronin from Explorer, a terminal and a pinned taskbar icon: no white flash, the title "*scratch* - ronin". | | |
| 2 | A2 | Nothing by hand: `--type-ahead-check` covers it. | | automated: --type-ahead-check |
| 3 | A5, A6 | Drag a window corner: crisp, nothing stretched, no black areas. Minimize, restore, maximize. | | |
| 4 | A8 | Alt+Tab away: the cursor is hollow; back: filled. | | |
| 5 | B4 | Hold a letter key: the repeat is smooth. | | |
| 6 | C9 | Open the 100 MB file (`--bench-buffer` makes it), M->, scroll with the wheel. | | |
| 7 | G11 | The 100 MB C file (`--bench-syntax` makes it): M->, colors within about a second, typing never blocked. | | |
| 8 | E14 | Two lines, M-w, paste in Notepad: two lines. | | |
| 9 | E15 | Copy text in a browser, C-y in ronin. | | |
| 10 | I4 | render_mode classic, natural, symmetric in ronin.conf: each looks different. | | |
| 11 | I8 | Edit ronin.conf in Notepad and save: applied, no flash of the default theme. | | |
| 12 | J9 | A modified file, then sign out: blocked with "Unsaved changes in ronin", ronin asks to save. | | |
| 13 | L22, L23 | Drag a divider and a mode line: the line follows the pointer; the resize cursors over them, the arrow elsewhere. | | |
| 14 | L25 | Dividers and inactive mode lines at 100% and 150%. | | |
| 15 | L30 | Four windows, the ronin window dragged to its smallest and back: every window comes back as it was. | | |
| 16 | M6 | A big tree (`ronin C:\Windows`), C-x p f and type at once: the list fills in, typing never held up. | | |
| 17 | M16 | The long-path project of the step, then its screenshot: the row starts with "…" and keeps the file name. | | |
| 18 | M23, M24 | Type in Notepad while `--test` and `--bench-project` run: Notepad keeps every key and the focus; `--test` passes. | | |
| 19 | N9 | C-x p g a common word in a big repository, type elsewhere at once: the list fills in, typing never held up. | | |
| 20 | N21 | Typing in Notepad, start `ronin.exe --no-activate`: Notepad keeps the focus and every key. | | |
| 21 | O23 | An error in a source file, C-x p c RET: the error in red, M-g n opens it; undo, build again: "Compilation finished". | | |
| 22 | O20 | `build.bat release` through C-x p c, type in another window: never held up. | | |
| 23 | N13 | A search, a build and `git status` / `git checkout` at once: nothing complains of a file in use. | | |
| 24 | O10 | M-x compile `ping -t localhost`, C-c C-k: "Compilation killed", no ping.exe or cmd.exe left. | | |
| 25 | P12 | Ctrl+Shift+click three places: three cursors, "mc:4"; again on one: it goes; a plain click: one cursor. | | |
| 26 | P8 | Three cursors, C-k, paste in Notepad: three lines in order. | | |
| 27 | P20 | A long file, C-x h C-S-c C-S-c (10,000 cursors), type, DEL, RET RET: nothing lags. | | |
| 28 | Q4 | The icon in the title bar, taskbar, Alt+Tab and Explorer at 100% and 150%: crisp. | | |
| 29 | Q3 | Explorer, ronin.exe, Properties, Details: version 1.0.0, "ronin text editor", the copyright. | | |
| 30 | Q7 | `early_window = true`, five starts from Explorer: no flash, no jump. Say whether it should be on by default. | | |
| 31 | Q9 | Minimize, wait, restore: back at once; Task Manager shows less memory while minimized. | | |
| 32 | Q6 | The crash command of the step, then ronin.exe: 1 recovery file; M-x recover-file shows both side by side. | | |
| 33 | Q2 | `ronin.exe --version` in cmd and PowerShell: "ronin 1.0.0", no window. | | |
| 34 | R17 | Ten minutes of ordinary editing: the commit size comes back within a few MB and stops rising. | | |
| 35 | R18 | Minimize for 5 seconds: the commit size drops by a few MB; restore at once. | | |
| 36 | S11 | `build.bat clean`, `build.bat`, `build\ronin_debug.exe`: *scratch*, the title, typing works. After the release (it removes dist\). | | |
| 37 | S12 | `build.bat release`, Properties, Details: version 1.0.0 and the icon. | | |
| 38 | T20 | C-S-p, "file": readable, columns aligned, keys right-aligned; also at 150%. | | |
| 39 | T21 | Ctrl+Shift+P "find" RET: "Find file:"; again and click outside: closes; again and ESC: closes. | | |
| 40 | U18 | C-x g in a work tree with changes: readable, status letters lined up, the kinds of lines told apart; also at 150%. | yes (with 41) | |
| 41 | U19 | With the zip's exe in ronin's repository with a change: C-x g, Down, TAB, C-v, M-v, TAB, RET; C-x g, g, C-g at once. | yes | |
| 42 | V1, V5 | C-c t in src\app\app.c, in *scratch*, and with `ronin src`: the prompt in the right directory. | | |
| 43 | V2 | C-c a: Windows Terminal in the project's root running claude; `/exit` leaves the prompt. | | |
| 44 | V3 | `terminal = cmd`: C-c t a classic console, C-c a one running claude. Remove the line. | | |
| 45 | V4 | C-x C-c with those terminals open: they stay, claude still answers. | | |
| 46 | V8 | A folder `C:\temp\a;b c` and `ronin C:\`: C-c t opens exactly there, one tab. | | |
| 47 | W1, W8 | With the zip's exe: `theme = vague` in ronin.conf, then M-x set-theme moonfly, oxide, mellifluous with a region and two windows: each like its picture, readable. | yes | |
| 48 | W12 | An old ronin.conf with every color: the override message; delete the [colors] lines: the theme appears. | | |
| 49 | X5 | README on GitHub, desktop and phone width: the top first, every image loads, tables readable. | | |
| 50 | X6 | The CI run's builds, source check, tests, imports and version steps are green; report the smoke step's last lines. | | |
| 51 | X7 | New issue offers two templates and no blank issue; a pull request opens with the checklist. | | |
| 52 | X8 | The external links of README and CONTRIBUTING lead where they say. | | |
| 53 | Y8 | `git clone` of GitHub's repository, `verify.bat`: `verify: PASS`, the source check PASS. | | |
| 54 | Z8 | Pages: Deploy from a branch, main, /docs; the site shows the overview. | | |
| 55 | Z7 | The site on a desktop and a phone: the navigation as described, no sideways scrolling, every page reached. | | |
| 56 | Z9 | Every image of the published site loads. | | |
| 57 | Z10 | README's first screen on GitHub: logo, name, line, badges, hero, links, about one screen. | | done: the owner, 2026-10-21 ("judged on GitHub; fine") |
| 58 | Z11 | The badges: MIT, build passing, the latest release (1.0.0 after step 64), Windows 10+ x64; each but the last a link. | | |
| 59 | Z14 | The CI run of the push is green, the docs-check step included; report the smoke step's last lines. | | smoke lines reported 2026-10-21 (Measurements, Phase 25); the run's green: to tick |
| 60 | AA7 | Copy the zip to a new folder, mark it downloaded in PowerShell: ``Set-Content <zip> -Stream Zone.Identifier -Value "[ZoneTransfer]`nZoneId=3"``; Explorer's Extract All; double-click ronin.exe: SmartScreen, More info, Run anyway; "*scratch* - ronin" opens. | yes | |
| 61 | AA4 | That ronin.exe with `%APPDATA%\ronin` renamed away for the test: ronin's own colors, and no `%APPDATA%\ronin` appears while you use it. | yes | |
| 62 | AA4 | Then C-c , : `%APPDATA%\ronin\ronin.conf` is created and shown; `theme = vague` under [settings], C-x C-s: the colors change. Put your own folder back. | yes | |
| 63 | AA8 | `git push`, then `git push origin v1.0.0`. | after the tag | |
| 64 | AA9 | The GitHub release from tag v1.0.0: title "ronin 1.0.0", the text of docs/releases/1.0.0.md, the zip and SHA256SUMS.txt from dist\ attached, set as the latest release. | after the tag | |
| 65 | AA10 | The Releases page lists ronin 1.0.0 with both files; README's release badge says v1.0.0. | after the tag | |
| 66 | AA11 | Download the zip from the release, `Get-FileHash`: the SHA-256 of the notes; unzip it and run ronin.exe once. | after the tag | |
