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
- Verify every change: build, run the tests and the smoke test (`--test`, `--smoke`), take a
  screenshot and look at it.
- Code, comments and docs in English. Talk to the user in Turkish.
- Whenever the user asks for changes to a plan, show the full revised plan again with the
  changes marked and wait for approval before implementing.

## Commit policy (every phase)

- Commit as you go: one logical change per commit, each one building with zero warnings and
  passing the tests and smoke. No single giant commit at the end of a phase.
- Categorized messages: `<type>(<scope>): <summary>`, type one of feat, fix, perf, refactor,
  test, docs, build, chore. Example: `feat(buffer): gap buffer with incremental line index`.
- Never push. Never amend or rewrite existing commits. The user pushes.
- Never commit build outputs or scratch files. If a pending change looks unintended, list it
  and ask instead of committing it.
- A phase ends with a clean working tree. Its last commit is `docs: mark phase N done`.

## Build / run / verify

From Git Bash (this environment sets `NoDefaultCurrentDirectoryInExePath=1`, so cmd needs
the explicit `.\`):

    cmd //c ".\build.bat"            # build\teal_debug.exe  (/Od /Zi /MTd, TEAL_DEV=1)
    cmd //c ".\build.bat release"    # build\teal.exe        (/O2 /GL ..., TEAL_DEV=0)
    cmd //c ".\build.bat bench"      # build\teal_bench.exe  (/O2 /MT, TEAL_DEV=1, no D3D debug layer)

Report benchmark numbers from teal_bench.exe (dev flags, optimized code). Its smoke run skips
the debug-layer check; the smoke acceptance criterion is the debug build.

build.bat finds MSVC through vswhere + vcvars64 when cl is not on PATH, compiles
src/shaders/*.hlsl with fxc into build\gen\*.h, compiles win32_dwrite.cpp, then the unity
build, and prints the exe size.

`--startup-ms` works in both builds: it exits right after the first Present with exit code =
ms since process creation. Git Bash `$?` truncates exit codes to 8 bits, so read it from
PowerShell: `(Start-Process build\teal.exe -ArgumentList --startup-ms -PassThru -Wait).ExitCode`.

Dev-build flags (TEAL_DEV=1 only); everything is logged to build\teal.log:

    build/teal_debug.exe --test [--seed N]                # headless buffer/file tests, exit 0 = pass
    build/teal_debug.exe --smoke                          # exit 0 = pass
    build/teal_debug.exe --screenshot build/shots/x.png   # window hidden, one frame
    build/teal_debug.exe --sample ...                     # the Phase 2 colored sample (smoke implies it)
    build/teal_debug.exe <file> --top-line N|end ...      # open a file, initial scroll (for screenshots)
    build/teal_debug.exe --dump-atlas build/shots/a.png   # the CPU glyph atlas
    build/teal_debug.exe --render-mode classic|natural|symmetric   (default symmetric)
    build/teal_debug.exe --scale 150                      # force the DPI scale (percent)
    build/teal_debug.exe --bench-text                     # 300 frames, Present(0, 0)

`--smoke` shows the window without activating it, renders 3 frames, reads back the third
and checks the probes from app_dev_probes: exact background / mode line / cursor pixels, a
text cell that is not background, a space cell that is exactly background, '_' inked only
in its lower part (catches upside-down bitmaps), and the ClearType channel order on a white
'|' (normalized coverage; pixel geometry from the rendering params). It also requires a
non-empty atlas, the D3D11 debug layer active with zero WARNING+ messages, and no leaks
(device refcount 0, empty DXGI live-object report, DirectWrite references 0).
Exit codes: 1 fatal, 2 renderer init, 3 pixel mismatch, 4 no debug layer, 5 debug-layer
messages, 6 leak, 7 output file, 8 font / ClearType, 9 test failure (--test).

`--test` runs without a window or device: a differential fuzz of `buffer_replace` against a
flat-array reference (100,000 ops, fixed seed printed in the log and on failure, `--seed`
overrides; decimal or 0x hex), capacity and read-only checks. A failed dev ASSERT logs its
file, line and condition before breaking, so a crash shows up in build\teal.log.

Open every screenshot after a visual change and look at it (crop and enlarge for detail);
check exact colors with an independent decoder, e.g. PowerShell `System.Drawing.Bitmap`.
When touching rasterization, check that the three render modes still produce different files.

## Theme (source of truth until the theme file arrives in Phase 7)

| role             | color   |   | role            | color   |
|------------------|---------|---|-----------------|---------|
| background       | #072626 |   | comment         | #3fdf1f |
| text / mode line | #d3b58d |   | string          | #0fdfaf |
| cursor           | #90ee90 |   | keyword         | #ffffff |
| selection        | #0000ff |   | number/constant | #7ad0c6 |
| type             | #8cde94 |   | variable        | #c1d1e3 |

The swap chain is B8G8R8A8_UNORM (not sRGB): theme colors must reach the screen bit-exact.
