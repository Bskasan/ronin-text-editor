# teal architecture

## Module map

| file | role |
|---|---|
| `src/teal.c` | the only translation unit; includes everything below in order |
| `src/base.h/.c` | types, ASSERT, Arena, String8/16, UTF-8<->UTF-16, mini formatter, dev LOG |
| `src/platform.h` | os_* primitives, Key/Event/FrameInput, app entry points — all the core sees of the OS |
| `src/render.h` | Rect, Color, r_* API — all the core sees of the GPU |
| `src/app.c` | editor core (Phase 1: placeholder grid cursor) |
| `src/png.c` | dev-only PNG encoder (stored deflate, CRC32, Adler-32) |
| `src/render_d3d11.c` | D3D11 device, flip-model swap chain, instanced-quad pipeline, capture |
| `src/shaders/quad.hlsl` | vs/ps for the quad pipeline, compiled by fxc to `build/gen/*.h` |
| `src/win32_main.c` | wWinMain, window, message loop, input translation, os_* implementation |
| `res/teal.manifest` | PerMonitorV2 DPI, longPathAware, supportedOS Windows 10 |

Core (`app.c`, later buffer/view/commands) includes only `platform.h` and `render.h`.

## Frame loop

1. `wWinMain` creates the window hidden on the monitor under the mouse, sizes it for that
   monitor's DPI (clamped to the work area), creates the renderer and the app.
2. Shows the window cloaked (DWMWA_CLOAK), presents the first frame, uncloaks: no white flash.
3. Loop: if no redraw is pending, block in `MsgWaitForMultipleObjectsEx`. Drain every queued
   message; the window procedure turns them into `Event`s in a fixed array (consecutive
   mouse-move and resize events are coalesced). If the array is nearly full, run a frame
   mid-drain instead of dropping anything. If anything requested a redraw, run exactly one
   frame: `app_update_and_render` → `r_begin_frame`, `r_push_rect`…, `r_end_frame`
   (flush + `Present(1, 0)`). Scratch arena resets after each frame.
4. `WM_SIZE` resizes the swap chain and renders immediately, so live resize stays crisp
   inside the modal size loop. Minimized = frames are no-ops.

Renderer: one pipeline, instanced quads. VS builds the strip corners from `SV_VertexID`;
the only vertex buffer is a dynamic per-instance buffer (rect, uv rect, RGBA8 color, kind;
40 bytes, 65536 per flush). Kind 0 = solid. Pipeline state is rebound every frame because
the flip model unbinds the back buffer on Present.

Input: modifiers come from `GetKeyState` at message time. The only tracked modifier state
is AltGr: a non-extended Left Ctrl immediately followed by an extended Right Alt with the
same message time is the synthetic AltGr Ctrl (GLFW technique) and is dropped; while AltGr
is held, events carry neither CTRL nor ALT and WM_CHAR is delivered as text. Alt works as
Meta (WM_SYSKEY* handled, WM_SYSCHAR swallowed, SC_KEYMENU swallowed); Alt+F4 still closes.

## Roadmap

- [x] 1. Skeleton, window, D3D11, rect renderer
- [ ] 2. Font: DirectWrite-rasterized glyph atlas, text drawing
- [ ] 3. Text buffer: gap buffer, line index, UTF-8, file load/save
- [ ] 4. View: cursors (stored as an array from day one), scrolling, layout
- [ ] 5. Commands, keymap with prefix keys, config file, hot reload
- [ ] 6. Editing: mark/region, kill ring, undo/redo, auto-indent
- [ ] 7. Theme file, mode line, minibuffer prompts
- [ ] 8. Lexers and incremental highlighting
- [ ] 9. isearch, query-replace, goto-line, buffer switching
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
- Import list kept minimal for startup: kernel32, user32, d3d11, dxgi, dwmapi (+ static
  dxguid.lib). No shell32 (own command-line parser), no shcore (DPI via GetDpiForWindow).
- Key events are not emitted for bare modifier keys (Ctrl, Shift, Alt, Win, Caps Lock) or
  numpad keys yet; Phase 5 decides what the keymap needs.
- Mouse moves are queued but do not request a redraw by themselves.
- Debug layer: break on CORRUPTION/ERROR only when a debugger is attached (otherwise a
  break kills the process and loses the message); messages are always logged and counted.
- Back-buffer capture (smoke, screenshot) is copied before Present: FLIP_DISCARD leaves the
  back buffer undefined afterwards.

## Later

- Waitable swap chain (DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) folded into the
  message wait, for lower input latency (Phase 15 candidate).
- `D3D11CreateDevice` is ~160 ms of the ~170 ms startup on the dev machine (NVIDIA driver
  load). Overlap it with other init on a worker thread once there is other init (fonts,
  file loading).
- Bare modifier and numpad key events if the keymap wants them.
