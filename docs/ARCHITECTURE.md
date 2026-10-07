# teal architecture

## Module map

| file | role |
|---|---|
| `src/teal.c` | the unity translation unit; includes everything below except the .cpp, in order |
| `src/base.h/.c` | types, ASSERT, Arena, String8/16, UTF-8<->UTF-16, mini formatter, dev LOG |
| `src/platform.h` | os_* primitives, Key/Event/FrameInput, app entry points — all the core sees of the OS |
| `src/render.h` | Rect, Color, r_* API (rects, glyphs, atlas) — all the core sees of the GPU |
| `src/font_backend.h` | extern "C" glyph rasterization API, no Win32 / DWrite types |
| `src/win32_dwrite.cpp` | the only C++ file: DirectWrite calls behind font_backend.h, nothing else |
| `src/font.h/.c` | metrics, CPU glyph atlas (shelf packer), glyph cache, `font_draw_text` |
| `src/app.c` | editor core (Phase 2: placeholder sample text, mode line, minibuffer echo, cursor) |
| `src/png.c` | dev-only PNG encoder (stored deflate, CRC32, Adler-32) |
| `src/render_d3d11.c` | D3D11 device, flip-model swap chain, instanced-quad pipeline, atlas texture, capture |
| `src/shaders/quad.hlsl` | vs/ps for the quad pipeline, compiled by fxc to `build/gen/*.h` |
| `src/win32_main.c` | wWinMain, window, message loop, input translation, os_* implementation, dev flags |
| `res/teal.manifest` | PerMonitorV2 DPI, longPathAware, supportedOS Windows 10 |

Core (`app.c`, `font.c`, later buffer/view/commands) includes only `platform.h` and `render.h`;
`font.c` additionally calls `font_backend.h`.

## Startup and frame loop

1. `wWinMain` allocates the Renderer and immediately starts a worker thread that runs only
   `D3D11CreateDevice` (`r_create_device`: no logging, no arenas, results go into Renderer
   fields). Meanwhile the main thread creates the window hidden on the monitor under the
   mouse, sizes it for that monitor's DPI (clamped to the work area) and creates the app,
   which opens DirectWrite and pre-rasterizes ASCII. Then it joins the worker, logs its
   results, and creates swap chain, pipeline and atlas texture (`r_finish_create`).
2. Shows the window cloaked (DWMWA_CLOAK), presents the first frame, uncloaks: no white flash.
3. Loop: if no redraw is pending, block in `MsgWaitForMultipleObjectsEx`. Drain every queued
   message; the window procedure turns them into `Event`s in a fixed array (consecutive
   mouse-move and resize events are coalesced). If the array is nearly full, run a frame
   mid-drain instead of dropping anything. If anything requested a redraw, run exactly one
   frame: `app_update_and_render` → `font_frame_begin`, `r_begin_frame`, `r_push_rect` /
   `font_draw_text`…, `r_end_frame` (flush + `Present(1, 0)`). Scratch resets after each frame.
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
same message time is the synthetic AltGr Ctrl (GLFW technique) and is dropped; while AltGr
is held, events carry neither CTRL nor ALT and WM_CHAR is delivered as text. Alt works as
Meta (WM_SYSKEY* handled, WM_SYSCHAR swallowed, SC_KEYMENU swallowed); Alt+F4 still closes.
Numpad: Enter arrives as KEY_ENTER, NumLock-off navigation keys as the ordinary ones, digits
as text events.

## Roadmap

- [x] 1. Skeleton, window, D3D11, rect renderer
- [x] 2. Font: DirectWrite-rasterized glyph atlas, text drawing
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
- Import list kept minimal for startup: kernel32, user32, gdi32, d3d11, dwmapi, dwrite (dxgi
  only in dev builds; dxguid.lib is static). No shell32 (own command-line parser), no shcore.
- No key events for lone modifier keys (Ctrl, Shift, Alt, Win, Caps Lock). Numpad: text events.
- Mouse moves are queued but do not request a redraw by themselves.
- Debug layer: break on CORRUPTION/ERROR only when a debugger is attached (otherwise a
  break kills the process and loses the message); messages are always logged and counted.
- Back-buffer capture (smoke, screenshot) is copied before Present: FLIP_DISCARD leaves the
  back buffer undefined afterwards.
- Monospace cell grid only. Every codepoint occupies exactly one cell. Column to x is a
  multiplication. cell_w and line_h are integers in physical pixels; every glyph quad lands
  on integer pixel coordinates.
- No shaping, no ligatures, no kerning: one codepoint -> one glyph. Combining marks and
  complex scripts are out of scope.
- One font face (Consolas, fallback Courier New), regular weight, 12 pt. No bold or italic.
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

## Later

- Waitable swap chain (DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) folded into the
  message wait, for lower input latency (Phase 15 candidate).
- Startup is ~190 ms, of which ~155 ms is `D3D11CreateDevice` (NVIDIA driver load) even on
  its own thread; everything else already overlaps it.
- Wide (East Asian) characters and emoji occupying two cells; color emoji.
- System font fallback for codepoints Consolas lacks.
- Bold / italic faces, if a theme ever wants them.
- Linear-space (gamma-corrected) ClearType blending for dark text on light backgrounds.
