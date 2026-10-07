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

- C11, MSVC (cl.exe), x64 only, Windows 10 1903+. No C++. (Possible exception, decided in
  Phase 2: one isolated .cpp glue file for DirectWrite if dwrite.h cannot be used from C.)
- No third-party code or libraries. System DLLs only.
- One build.bat, unity build: src/teal.c includes the other .c files. No CMake, no .sln.
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
- Layering: editor core files use only platform.h and render.h. Only win32_*.c and
  render_d3d11.c touch Win32 / D3D types.
- State lives in a few explicit structs (App, Platform, Renderer), not scattered globals.
- Naming: types PascalCase; functions and variables snake_case with a module prefix
  (arena_push, r_push_rect, os_write_file); macros and enum constants UPPER_SNAKE.
- Scope discipline: build only what the current phase asks for. No speculative
  abstractions. Park ideas in the "Later" list in docs/ARCHITECTURE.md.
- Verify every change: build, run the smoke test, take a screenshot and look at it.
- Code, comments and docs in English. Talk to the user in Turkish.

## Build / run / verify

From Git Bash (this environment sets `NoDefaultCurrentDirectoryInExePath=1`, so cmd needs
the explicit `.\`):

    cmd //c ".\build.bat"            # build\teal_debug.exe  (/Od /Zi /MTd, TEAL_DEV=1)
    cmd //c ".\build.bat release"    # build\teal.exe        (/O2 /GL ..., TEAL_DEV=0)

build.bat finds MSVC through vswhere + vcvars64 when cl is not on PATH and compiles
src/shaders/*.hlsl with fxc into build\gen\*.h first. It prints the exe size.

Dev-build flags (TEAL_DEV=1 only); everything is logged to build\teal.log:

    build/teal_debug.exe --smoke                    # exit 0 = pass
    build/teal_debug.exe --screenshot build/shot.png

`--smoke` shows the window without activating it, renders 3 frames, reads back the third
and checks probe pixels (app_dev_probes) for exact theme colors, requires the D3D11 debug
layer to be active with zero WARNING+ messages, and checks for leaks (device refcount 0
after the final Release, empty DXGI live-object report). Exit codes: 1 fatal, 2 renderer
init, 3 pixel mismatch, 4 no debug layer, 5 debug-layer messages, 6 leak, 7 screenshot.

`--screenshot` renders one frame with the window hidden and writes a PNG (own encoder,
src/png.c). Open the PNG and look at it after every visual change; check exact colors with
an independent decoder, e.g. PowerShell `System.Drawing.Bitmap.GetPixel`.

## Theme (source of truth until the theme file arrives in Phase 7)

| role             | color   |   | role            | color   |
|------------------|---------|---|-----------------|---------|
| background       | #072626 |   | comment         | #3fdf1f |
| text / mode line | #d3b58d |   | string          | #0fdfaf |
| cursor           | #90ee90 |   | keyword         | #ffffff |
| selection        | #0000ff |   | number/constant | #7ad0c6 |
| type             | #8cde94 |   | variable        | #c1d1e3 |

The swap chain is B8G8R8A8_UNORM (not sRGB): theme colors must reach the screen bit-exact.
