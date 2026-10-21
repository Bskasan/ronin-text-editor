# Contributing to ronin

Thank you for looking. ronin has three goals, in this order: input latency and speed, a small and simple
program, then features. Every change is judged by that order, so read this page before you write code. How to use
ronin is on the [documentation site](https://bskasan.github.io/ronin-text-editor/); this page is about changing it.

## Building

You need Windows 10 or later (x64) and Visual Studio 2022 or later, or the Build Tools for Visual Studio, with the
"Desktop development with C++" workload (the MSVC x64 tools and the Windows SDK). `build.bat` finds them itself.

| command | makes |
|---|---|
| `build.bat` | `build\ronin_debug.exe`: debug, the dev flags (tests, benchmarks), the D3D11 debug layer |
| `build.bat release` | `build\ronin.exe`: optimized, no C runtime, no dev code |
| `build.bat bench` | `build\ronin_bench.exe`: optimized, with the dev flags |
| `build.bat clean` | removes `build\` and `dist\`, everything generated, and nothing else |
| `build.bat docs` | the debug build, then the documentation site: `docs\*.html` from `docs\src` (committed, as GitHub Pages serves it) |

- Run `verify.bat` before every commit. It builds all three, runs the source check (see [New files](#new-files)), the
  docs-check (the site in `docs\` is what its sources make), the smoke test (real frames, pixel checks) and the tests,
  checks the release exe's imports, and prints one last line: only `verify: PASS` counts.
- Run `verify.bat phase` before you open a pull request (about 9 minutes): it adds the full tests on the debug build,
  a sweep of 256 fuzz seeds, a million-event soak and the leak check.
- Both run on your machine only. The CI workflow builds the three configurations and runs the source check, the tests,
  the docs-check, the import check and `--version`; it cannot run the smoke gate (no GPU, no D3D11 debug layer), so a
  local `verify.bat` is required.

From Git Bash, call them as `cmd //c ".\verify.bat"`. The dev flags are listed in [CLAUDE.md](CLAUDE.md), the design
in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## What a pull request must meet

- C11 for MSVC, `/W4 /WX` clean. C++ only in `src/platform/win32_dwrite.cpp`, and only C-style there.
- No C runtime functions: no stdio, no malloc / free, no `str*` (memcpy, memset and memmove are fine).
- No third-party code or libraries, and no new DLL import: `res/imports.txt` stays as it is.
- Memory from the arenas only; nothing allocated per keystroke or per frame; no idle timers or busy loops.
- Every feature is a named command with a one-line description, so it can be bound to a key and found with M-x and
  `C-S-p`.
- Tests for the change, and you have seen them fail without it: say how in the pull request.
- README.md, the site's pages (`docs/src`, then `build.bat docs`), [docs/MANUAL_TESTS.md](docs/MANUAL_TESTS.md) and
  docs/ARCHITECTURE.md updated where they describe what you changed. `--test` checks the keys, settings and links of
  README and of every page of the site against the code. The site's
  [keys](https://bskasan.github.io/ronin-text-editor/keys.html) and
  [settings](https://bskasan.github.io/ronin-text-editor/settings.html) pages are generated from the exe: a new command
  or setting appears there by itself.
- One logical change per commit, each message `<type>(<scope>): <summary>` with type one of feat, fix, perf,
  refactor, test, docs, build, chore. Example: `fix(isearch): wrap past the last match`.
- A screenshot (`--screenshot`) for a visual change.
- A new source file follows [New files](#new-files).

## New files

`src/` is in folders by layer, and the folder a file is in says what it may include: `base/`, `platform/` (the only
place for Windows headers: everything that touches Win32, Direct3D or DirectWrite), `text/`, `editor/`, `project/`,
`app/` and `dev/`; `src/ronin.c` includes every file in order. Nothing in `src/base/` or `src/text/` includes from
`src/editor/`, `src/project/` or `src/app/`. The [module map](docs/ARCHITECTURE.md#module-map) lists every file.

Every `.c`, `.h`, `.cpp` and `.hlsl` file under `src/`, and every file under `tools/`, starts with this header, the
second line saying in at most 72 characters what the file does:

```c
/* ======================================================================
   ronin: <one line saying what this file does>

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */
```

That line is also the file's description in the module map, word for word, in a row under its folder's heading.
`verify.bat` runs the source check (`build\check_source.exe .`, built by `build.bat`): it names each file without the
header or a row, each description that differs from the map, each row without a file, and each include that breaks
the two rules, as `path:line: message`.

## Proposing a feature

Open an issue with the feature request template before you write code. Say what the command is called, which keys it
would take, and what it costs in latency and size: the priority order decides what gets in, and a feature that slows
typing down will not.

## Reporting a bug

Use the bug report template and include the output of `ronin --version`. The release build writes no log, so attach
instead: the exact keys you pressed, the file (or a small one that shows the bug), the lines of your `ronin.conf`
that differ from the defaults, the text of `*Messages*` (`C-h e`), a screenshot, and your Windows version, display
scale and keyboard layout. After a crash, say whether files appeared in `%LOCALAPPDATA%\ronin\recovery`.

## Out of scope

- A language server or tree-sitter: they cost size, startup and latency, and add dependencies.
- Proportional fonts, ligatures, bold and italic: ronin draws one character per cell of a grid.
- Tabs, menus, toolbars and scroll bars: the Emacs look is the point.
- Plugins or a scripting language: a small and simple program comes before features.
- Other platforms: ronin is a Win32 program from its window to its threads.

What may come later is the "After 1.0" list in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md#after-10).

## Review

The maintainer reviews every pull request against the priority order: latency and speed, then small and simple, then
features.

Pull requests are judged the same way whoever or whatever wrote them: you must have read, understood and tested every
line you submit.
