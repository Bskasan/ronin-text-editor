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
  link check (nothing of the C runtime in the release and bench links but memcpy, memset, memmove and the compiler's
  helpers), the docs-check (the site in `docs\` is what its sources make), the smoke test (real frames, pixel checks)
  and the tests, checks the release exe's imports, and prints one last line: only `verify: PASS` counts.
- Run `verify.bat phase` before you open a pull request for anything larger than a small fix (about 9 minutes): it
  adds the full tests on the debug build, a sweep of 256 fuzz seeds, a million-event soak and the leak check.
- Both run on your machine only. The CI workflow builds the three configurations and runs the source check, the link
  check, the tests, the docs-check, the import check and `--version`; it cannot run the smoke gate (no GPU, no D3D11
  debug layer), so a local `verify.bat` is required.

From Git Bash, call them as `cmd //c ".\verify.bat"`. The dev flags are listed in [CLAUDE.md](CLAUDE.md), the design
in [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## How a change gets in

Every change, from a typo to a feature, starts as an issue and reaches `main` through a pull request; nobody commits
to `main` directly, the maintainer included.

1. **An issue.** Look for one first, else open one with the bug report or the feature request template. For a feature,
   say what the command is called, which keys it would take, and what it costs in latency and size: the priority order
   decides what gets in, and a feature that slows typing down will not. Start work once the maintainer has labeled it
   (feature, bug, performance, docs, chore, backlog) and, for planned work, given it a milestone.
2. **A branch** from an up-to-date `main`, named `<type>/<issue>-<short-slug>` with a commit type: `feat/4-line-numbers`,
   `fix/1-git-visit-first-change`, `docs/…`, `perf/…`, `test/…`, `chore/…`. From outside the project, make it in your
   fork.
3. **A plan** in the issue for anything more than a few lines, agreed before the code.
4. **Commits**, one logical change each, `verify.bat` printing `verify: PASS` before each.
5. **A pull request** to `main`: the issue's summary, the template's checklist filled in, and `Closes #<issue>`. Run
   `verify.bat phase` first unless it is a small fix. The CI check `build and test` must pass.
6. **Review and merge** by the maintainer with "Rebase and merge": your commits land on `main` one by one, so each one
   must build and pass on its own. If `main` moves on, rebase your branch on it rather than merging `main` in.

## Versions and releases

ronin follows [Semantic Versioning](https://semver.org/): a PATCH release (1.0.1) has bug fixes only, a MINOR one
(1.1.0) features that keep every existing configuration and key working, and a MAJOR one is kept for a change that
breaks an existing ronin.conf, theme file or key. Every pull request that changes what a user sees, installs or
configures adds that change to the "Unreleased" section of [CHANGELOG.md](CHANGELOG.md); workflow and contributor-only
changes need no entry. A release is a pull request of its own (the version in `src/app/version.h`, the changelog's
section dated, the release notes in `docs/releases/`); the maintainer tags and publishes it. `release-check.bat` is the
maintainer's own check before a release (about 12 minutes); contributors never need to run it. `tools/e2e.c`
(`build\e2e.exe`) takes the keyboard and the mouse to drive the shipped exe; only the maintainer runs it.

## What a pull request must meet

- C11 for MSVC, `/W4 /WX` clean. C++ only in `src/platform/win32_dwrite.cpp`, and only C-style there.
- No C runtime functions: no stdio, no malloc / free, no `str*` (memcpy, memset and memmove are fine). The source
  check names each call of another one, and each C runtime header in src/platform/ and tools/, with its file and
  line; the link check catches what a macro or the compiler brings in (such as `RtlEqualMemory`, which is `memcmp`).
- String and character literals are ASCII: write other characters as `\u` escapes (`\x` for a single byte), so a
  literal never depends on the code page the compiler reads the file in. Comments may hold any UTF-8.
- In a .bat file, call every Windows program by its System32 path (`%SystemRoot%\System32\findstr.exe`), also after
  a pipe and inside `for /f`: Git for Windows' usr\bin has its own find, sort and tar, and can come first on PATH. A
  bare name is fine for cmd's builtins, cl, rc, fxc and git.
- No third-party code or libraries, and no new DLL import: `res/imports.txt` stays as it is.
- Memory from the arenas only; nothing allocated per keystroke or per frame; no idle timers or busy loops.
- Every feature is a named command with a one-line description, so it can be bound to a key and found with M-x and
  `C-S-p`.
- Tests for the change, and you have seen them fail without it: say how in the pull request.
- A new command, key binding or setting has a test that exercises it (a setting: its effect checked through
  `test_setting_set` / `TEST_SETTING_EFFECT` in src/dev/test.c), or a line in `res/coverage_allow.txt` with a reason
  and an issue. `--test` fails on any command, binding or setting no test exercised.
- README.md, the site's pages (`docs/src`, then `build.bat docs`), [docs/MANUAL_TESTS.md](docs/MANUAL_TESTS.md) and
  docs/ARCHITECTURE.md updated where they describe what you changed. `--test` checks the keys, settings and links of
  README and of every page of the site against the code. The site's
  [keys](https://bskasan.github.io/ronin-text-editor/keys.html) and
  [settings](https://bskasan.github.io/ronin-text-editor/settings.html) pages are generated from the exe: a new command
  or setting appears there by itself.
- CHANGELOG.md's "Unreleased" section updated for a change a user sees, installs or configures.
- One logical change per commit, each message `<type>(<scope>): <summary> (#<issue>)` with type one of feat, fix,
  perf, refactor, test, docs, build, chore. Example: `fix(isearch): wrap past the last match (#21)`.
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
header or a row, each description that differs from the map, each row without a file, each include that breaks the
two rules, each C runtime call, C runtime header and non-ASCII literal, and each program a .bat file calls by its bare
name, as `path:line: message`.

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
