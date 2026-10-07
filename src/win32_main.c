// win32_main.c — entry point, window, message loop, input translation, os_* primitives.

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>

#define EVENT_CAPACITY 256
#define EVENT_RESERVE 4 // free slots required before taking another message off the queue

typedef struct Platform {
    HINSTANCE instance;
    HWND hwnd;
    Arena perm;
    Arena scratch; // reset after every frame
    App *app;
    Renderer *renderer;

    Event events[EVENT_CAPACITY];
    i32 event_count;

    i32 width, height; // client area, pixels
    u32 dpi;
    b32 redraw;
    b32 quit;
    b32 minimized;
    b32 in_frame;
    b32 in_size_move;

    b32 altgr;          // set by the synthetic Left Ctrl that precedes Right Alt
    u16 high_surrogate; // pending WM_CHAR high surrogate

    i64 frame_count;
    LARGE_INTEGER qpc_start;

#if TEAL_DEV
    HANDLE log_file;
    b32 smoke;
    String8 screenshot_path;
#endif
} Platform;

// The window procedure has no user pointer before WM_NCCREATE; this is its only way in.
static Platform *g_platform;

// ---------------------------------------------------------------------------
// os_* primitives

// UTF-8 -> NUL-terminated UTF-16 into a fixed buffer, truncating. For logging and fatal errors,
// which must work even when an arena cannot be used.
static void win32_utf8_to_wide_fixed(String8 s, u16 *out, i64 cap) {
    i64 n = 0;
    for (i64 i = 0; i < s.len && n < cap - 2;) {
        i64 advance;
        u32 cp = utf8_decode(s.data + i, s.len - i, &advance);
        i += advance;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out[n++] = (u16)(0xD800 + (cp >> 10));
            out[n++] = (u16)(0xDC00 + (cp & 0x3FF));
        } else {
            out[n++] = (u16)cp;
        }
    }
    out[n] = 0;
}

void *os_reserve(u64 size) {
    return VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
}

b32 os_commit(void *ptr, u64 size) {
    return VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

b32 os_write_file(String8 path, String8 data) {
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    String16 path16 = str16_from_str8(scratch, path);
    HANDLE file = CreateFileW((WCHAR *)path16.data, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    arena_pop_to(scratch, mark);
    if (file == INVALID_HANDLE_VALUE) return 0;
    b32 ok = 1;
    for (i64 offset = 0; offset < data.len;) {
        DWORD chunk = (DWORD)MIN(data.len - offset, (i64)MB(64));
        DWORD written = 0;
        if (!WriteFile(file, data.data + offset, chunk, &written, NULL) || written != chunk) {
            ok = 0;
            break;
        }
        offset += written;
    }
    CloseHandle(file);
    return ok;
}

void os_fatal(String8 message) {
    b32 interactive = 1;
#if TEAL_DEV
    LOG("fatal: %S", message);
    if (g_platform && (g_platform->smoke || g_platform->screenshot_path.len)) interactive = 0;
#endif
    if (interactive) {
        u16 text[1024];
        win32_utf8_to_wide_fixed(message, text, ARRAY_COUNT(text));
        MessageBoxW(NULL, (WCHAR *)text, L"teal", MB_OK | MB_ICONERROR);
    }
    ExitProcess(1);
}

#if TEAL_DEV
void os_log_write(String8 text) {
    u16 wide[4096];
    win32_utf8_to_wide_fixed(text, wide, ARRAY_COUNT(wide));
    OutputDebugStringW((WCHAR *)wide);
    if (g_platform && g_platform->log_file && g_platform->log_file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(g_platform->log_file, text.data, (DWORD)text.len, &written, NULL);
    }
}
#endif

// ---------------------------------------------------------------------------
// Command line (same splitting rules as CommandLineToArgvW, without linking shell32).
// Only dev flags exist so far.

#if TEAL_DEV
static i32 win32_parse_args(Arena *arena, String8 **out_args) {
    u16 *cmd = (u16 *)GetCommandLineW();
    i64 len = 0;
    while (cmd[len]) len++;

    String8 *args = PUSH_ARRAY(arena, String8, len + 1);
    u16 *buf = PUSH_ARRAY(arena, u16, len + 1);
    i32 count = 0;
    u16 *p = cmd;
    i64 n = 0;

    // argv[0]: quotes delimit, backslashes are literal.
    if (*p == '"') {
        p++;
        while (*p && *p != '"') buf[n++] = *p++;
        if (*p) p++;
    } else {
        while (*p && *p != ' ' && *p != '\t') buf[n++] = *p++;
    }
    args[count++] = str8_from_str16(arena, buf, n);

    for (;;) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        n = 0;
        b32 quoted = 0;
        while (*p && (quoted || (*p != ' ' && *p != '\t'))) {
            if (*p == '\\') {
                i64 slashes = 0;
                while (*p == '\\') { slashes++; p++; }
                if (*p == '"') {
                    for (i64 i = 0; i < slashes / 2; i++) buf[n++] = '\\';
                    if (slashes & 1) { buf[n++] = '"'; p++; }
                } else {
                    for (i64 i = 0; i < slashes; i++) buf[n++] = '\\';
                }
            } else if (*p == '"') {
                p++;
                if (quoted && *p == '"') { buf[n++] = '"'; p++; }
                else quoted = !quoted;
            } else {
                buf[n++] = *p++;
            }
        }
        args[count++] = str8_from_str16(arena, buf, n);
    }

    *out_args = args;
    return count;
}
#endif

// ---------------------------------------------------------------------------
// Frames and events

static i64 win32_filetime_u64(FILETIME ft) {
    return (i64)(((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
}

static void win32_frame(Platform *p) {
    if (p->in_frame || !p->renderer || !p->app) return;
    p->in_frame = 1;

    FrameInput input = { p->events, p->event_count, p->width, p->height, (f32)p->dpi / 96.0f };
    if (!app_update_and_render(p->app, &input, p->renderer)) p->quit = 1;
    p->event_count = 0;
    p->redraw = r_wants_redraw(p->renderer);
    arena_reset(&p->scratch);
    p->frame_count++;

#if TEAL_DEV
    if (p->frame_count == 1) {
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        i64 us_main = (now.QuadPart - p->qpc_start.QuadPart) * 1000000 / freq.QuadPart;
        FILETIME created, exited, kernel, user, now_ft;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        GetSystemTimePreciseAsFileTime(&now_ft);
        i64 us_process = (win32_filetime_u64(now_ft) - win32_filetime_u64(created)) / 10;
        LOG("startup: first Present %D.%03D ms after WinMain, %D.%03D ms after process creation",
            us_main / 1000, us_main % 1000, us_process / 1000, us_process % 1000);
    }
#endif
    p->in_frame = 0;
}

static void win32_push_event(Platform *p, Event e) {
    if (p->event_count > 0) {
        Event *last = &p->events[p->event_count - 1];
        if (last->kind == e.kind && (e.kind == EVENT_MOUSE_MOVE || e.kind == EVENT_RESIZE)) {
            *last = e;
            return;
        }
    }
    // The drain loop keeps EVENT_RESERVE slots free, so this only triggers for events
    // produced outside it (sent messages, modal loops).
    if (p->event_count == EVENT_CAPACITY) win32_frame(p);
    ASSERT(p->event_count < EVENT_CAPACITY);
    if (p->event_count < EVENT_CAPACITY) p->events[p->event_count++] = e;
}

static u32 win32_mods(Platform *p) {
    u32 mods = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= MOD_SHIFT;
    // While AltGr is held Windows reports Ctrl and Alt as down; they are not modifiers then.
    if (!p->altgr) {
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= MOD_CTRL;
        if (GetKeyState(VK_MENU) & 0x8000) mods |= MOD_ALT;
    }
    return mods;
}

static Key win32_map_vk(u32 vk) {
    if (vk >= 'A' && vk <= 'Z') return (Key)(KEY_A + (vk - 'A'));
    if (vk >= '0' && vk <= '9') return (Key)(KEY_0 + (vk - '0'));
    if (vk >= VK_F1 && vk <= VK_F24) return (Key)(KEY_F1 + (vk - VK_F1));
    switch (vk) {
    case VK_ESCAPE:     return KEY_ESCAPE;
    case VK_TAB:        return KEY_TAB;
    case VK_BACK:       return KEY_BACKSPACE;
    case VK_RETURN:     return KEY_ENTER;
    case VK_SPACE:      return KEY_SPACE;
    case VK_INSERT:     return KEY_INSERT;
    case VK_DELETE:     return KEY_DELETE;
    case VK_HOME:       return KEY_HOME;
    case VK_END:        return KEY_END;
    case VK_PRIOR:      return KEY_PAGE_UP;
    case VK_NEXT:       return KEY_PAGE_DOWN;
    case VK_LEFT:       return KEY_LEFT;
    case VK_RIGHT:      return KEY_RIGHT;
    case VK_UP:         return KEY_UP;
    case VK_DOWN:       return KEY_DOWN;
    case VK_OEM_MINUS:  return KEY_MINUS;
    case VK_OEM_PLUS:   return KEY_EQUALS;
    case VK_OEM_4:      return KEY_LEFT_BRACKET;
    case VK_OEM_6:      return KEY_RIGHT_BRACKET;
    case VK_OEM_5:      return KEY_BACKSLASH;
    case VK_OEM_1:      return KEY_SEMICOLON;
    case VK_OEM_7:      return KEY_APOSTROPHE;
    case VK_OEM_3:      return KEY_GRAVE;
    case VK_OEM_COMMA:  return KEY_COMMA;
    case VK_OEM_PERIOD: return KEY_PERIOD;
    case VK_OEM_2:      return KEY_SLASH;
    case VK_OEM_102:    return KEY_OEM_102;
    case VK_PAUSE:      return KEY_PAUSE;
    case VK_APPS:       return KEY_APPS;
    default:            return KEY_NONE; // modifiers, numpad, IME, media keys
    }
}

static b32 win32_is_key_message(UINT msg) {
    return msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP;
}

static LRESULT CALLBACK win32_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Platform *p = g_platform;
    switch (msg) {
    case WM_CLOSE: {
        Event e = { .kind = EVENT_CLOSE };
        win32_push_event(p, e);
        p->redraw = 1;
        return 0;
    }

    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        EndPaint(hwnd, &ps);
        p->redraw = 1;
        if (p->in_size_move) win32_frame(p);
        return 0;
    }

    case WM_ENTERSIZEMOVE:
        p->in_size_move = 1;
        return 0;
    case WM_EXITSIZEMOVE:
        p->in_size_move = 0;
        return 0;

    case WM_SIZE: {
        i32 w = LOWORD(lp), h = HIWORD(lp);
        p->minimized = (wp == SIZE_MINIMIZED);
        if (p->minimized) {
            if (p->renderer) r_resize(p->renderer, 0, 0);
            return 0;
        }
        if (p->renderer) r_resize(p->renderer, w, h);
        if (w != p->width || h != p->height) {
            p->width = w;
            p->height = h;
            Event e = { .kind = EVENT_RESIZE, .width = w, .height = h };
            win32_push_event(p, e);
        }
        // Render right away: during a drag we are inside the modal size loop and the
        // main loop does not run until the drag ends.
        p->redraw = 1;
        win32_frame(p);
        return 0;
    }

    case WM_DPICHANGED: {
        p->dpi = HIWORD(wp);
        RECT *suggested = (RECT *)lp;
        SetWindowPos(hwnd, NULL, suggested->left, suggested->top, suggested->right - suggested->left,
                     suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
        p->redraw = 1;
        if (p->in_size_move) win32_frame(p);
        return 0;
    }

    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        if (msg == WM_KILLFOCUS) {
            p->altgr = 0;
            p->high_surrogate = 0;
        }
        Event e = { .kind = EVENT_FOCUS, .focused = (msg == WM_SETFOCUS) };
        win32_push_event(p, e);
        p->redraw = 1;
        return 0;
    }

    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        u32 vk = (u32)wp;
        b32 down = !(HIWORD(lp) & KF_UP);
        b32 extended = (HIWORD(lp) & KF_EXTENDED) != 0;

        // AltGr arrives as a synthetic Left Ctrl immediately followed by Right Alt with the
        // same message time (same technique as GLFW). Drop the Ctrl and remember AltGr.
        if (vk == VK_CONTROL && !extended) {
            MSG next;
            if (PeekMessageW(&next, NULL, 0, 0, PM_NOREMOVE) && win32_is_key_message(next.message) &&
                next.wParam == VK_MENU && (HIWORD(next.lParam) & KF_EXTENDED) &&
                next.time == (DWORD)GetMessageTime()) {
                if (down) p->altgr = 1;
                return 0;
            }
        }
        if (vk == VK_MENU && extended && !down) p->altgr = 0;

        if (down) {
            Key key = win32_map_vk(vk);
            if (key != KEY_NONE) {
                Event e = {
                    .kind = EVENT_KEY_DOWN,
                    .key = key,
                    .mods = win32_mods(p),
                    .scancode = (u32)(HIWORD(lp) & (KF_EXTENDED | 0xFF)),
                    .repeat = (HIWORD(lp) & KF_REPEAT) != 0,
                };
                win32_push_event(p, e);
                p->redraw = 1;
            }
            // We handle WM_SYSKEYDOWN ourselves so Alt works as Meta; let only Alt+F4
            // through to DefWindowProc so it still closes the window.
            if (msg == WM_SYSKEYDOWN && vk == VK_F4 && !p->altgr) return DefWindowProcW(hwnd, msg, wp, lp);
        }
        return 0;
    }

    case WM_CHAR: {
        u32 c = (u32)wp;
        if (c >= 0xD800 && c <= 0xDBFF) {
            p->high_surrogate = (u16)c;
            return 0;
        }
        if (c >= 0xDC00 && c <= 0xDFFF) {
            if (!p->high_surrogate) return 0;
            c = 0x10000 + (((u32)p->high_surrogate - 0xD800) << 10) + (c - 0xDC00);
        }
        p->high_surrogate = 0;
        if (c < 0x20 || (c >= 0x7F && c <= 0x9F)) return 0; // control characters
        u32 mods = win32_mods(p);
        if ((mods & (MOD_CTRL | MOD_ALT)) && !p->altgr) return 0;
        Event e = { .kind = EVENT_TEXT, .codepoint = c, .mods = mods };
        win32_push_event(p, e);
        p->redraw = 1;
        return 0;
    }

    case WM_SYSCHAR: // Alt+key: handled as a key event; returning 0 avoids the beep
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) return 0; // Alt / F10 never open the system menu
        break;

    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:   case WM_RBUTTONUP:   case WM_MBUTTONUP: {
        b32 down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
        MouseButton button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? MOUSE_LEFT
                           : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? MOUSE_RIGHT
                           : MOUSE_MIDDLE;
        if (down) SetCapture(hwnd);
        else if (!(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) ReleaseCapture();
        Event e = {
            .kind = down ? EVENT_MOUSE_DOWN : EVENT_MOUSE_UP,
            .button = button,
            .x = (i16)LOWORD(lp),
            .y = (i16)HIWORD(lp),
            .mods = win32_mods(p),
        };
        win32_push_event(p, e);
        p->redraw = 1;
        return 0;
    }

    case WM_MOUSEMOVE: {
        // Delivered with the next frame; a move alone does not request one.
        Event e = { .kind = EVENT_MOUSE_MOVE, .x = (i16)LOWORD(lp), .y = (i16)HIWORD(lp), .mods = win32_mods(p) };
        win32_push_event(p, e);
        return 0;
    }

    case WM_MOUSEWHEEL: {
        POINT pt = { (i16)LOWORD(lp), (i16)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        Event e = {
            .kind = EVENT_MOUSE_WHEEL,
            .x = pt.x,
            .y = pt.y,
            .wheel = GET_WHEEL_DELTA_WPARAM(wp),
            .mods = win32_mods(p),
        };
        win32_push_event(p, e);
        p->redraw = 1;
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------------------
// Dev tooling

#if TEAL_DEV
enum {
    EXIT_OK = 0,
    EXIT_FATAL = 1,
    EXIT_RENDERER_INIT = 2,
    EXIT_PIXEL_MISMATCH = 3,
    EXIT_NO_DEBUG_LAYER = 4,
    EXIT_DEBUG_MESSAGES = 5,
    EXIT_LEAK = 6,
    EXIT_SCREENSHOT = 7,
};

// Called right after the captured frame.
static i32 win32_smoke_check_pixels(Platform *p) {
    i32 w, h;
    u8 *pixels = r_read_capture(p->renderer, &p->scratch, &w, &h);
    if (!pixels) {
        LOG("smoke: FAIL: back buffer capture failed");
        return EXIT_PIXEL_MISMATCH;
    }
    FrameInput input = { NULL, 0, p->width, p->height, (f32)p->dpi / 96.0f };
    DevProbe probes[8];
    i32 count = app_dev_probes(p->app, &input, probes, ARRAY_COUNT(probes));
    i32 result = EXIT_OK;
    for (i32 i = 0; i < count; i++) {
        DevProbe *probe = &probes[i];
        if (probe->x < 0 || probe->y < 0 || probe->x >= w || probe->y >= h) {
            LOG("smoke: FAIL: probe '%s' at (%d, %d) is outside %dx%d", probe->what, probe->x, probe->y, w, h);
            result = EXIT_PIXEL_MISMATCH;
            continue;
        }
        u8 *px = pixels + ((i64)probe->y * w + probe->x) * 4; // BGRA
        u32 got = ((u32)px[2] << 16) | ((u32)px[1] << 8) | px[0];
        b32 ok = (got == probe->rgb);
        LOG("smoke: %s: probe '%s' at (%d, %d): expected #%06x, got #%06x",
            ok ? "ok" : "FAIL", probe->what, probe->x, probe->y, probe->rgb, got);
        if (!ok) result = EXIT_PIXEL_MISMATCH;
    }
    return result;
}

static i32 win32_screenshot(Platform *p) {
    r_request_capture(p->renderer);
    win32_frame(p);
    i32 w, h;
    u8 *pixels = r_read_capture(p->renderer, &p->scratch, &w, &h);
    if (!pixels) {
        LOG("screenshot: back buffer capture failed");
        return EXIT_SCREENSHOT;
    }
    String8 png = png_encode_bgra(&p->scratch, pixels, w, h, (i64)w * 4);
    if (!os_write_file(p->screenshot_path, png)) {
        LOG("screenshot: could not write %S", p->screenshot_path);
        return EXIT_SCREENSHOT;
    }
    LOG("screenshot: wrote %S (%dx%d, %D bytes)", p->screenshot_path, w, h, png.len);
    return EXIT_OK;
}
#endif

// ---------------------------------------------------------------------------
// Entry point

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev_instance, PWSTR cmd_line, int show_cmd) {
    (void)prev_instance;
    (void)cmd_line;
    (void)show_cmd;

    LARGE_INTEGER qpc_start;
    QueryPerformanceCounter(&qpc_start);

    Arena perm = arena_create(GB(1));
    Platform *p = PUSH_STRUCT(&perm, Platform);
    p->perm = perm;
    p->scratch = arena_create(MB(256));
    p->instance = instance;
    p->qpc_start = qpc_start;
    g_platform = p;
    i32 exit_code = 0;

#if TEAL_DEV
    String8 *args;
    i32 arg_count = win32_parse_args(&p->perm, &args);
    for (i32 i = 1; i < arg_count; i++) {
        if (str8_equal(args[i], STR8_LIT("--smoke"))) p->smoke = 1;
        else if (str8_equal(args[i], STR8_LIT("--screenshot")) && i + 1 < arg_count) p->screenshot_path = args[++i];
    }

    // Log next to the executable (build\teal.log), independent of the working directory.
    u16 exe_path[MAX_PATH * 4];
    DWORD exe_len = GetModuleFileNameW(NULL, (WCHAR *)exe_path, ARRAY_COUNT(exe_path));
    while (exe_len > 0 && exe_path[exe_len - 1] != '\\') exe_len--;
    String8 exe_dir = str8_from_str16(&p->perm, exe_path, exe_len > 0 ? exe_len - 1 : 0);
    String8 log_path = str8_fmt(&p->perm, "%S\\teal.log", exe_dir);
    String16 log_path16 = str16_from_str8(&p->perm, log_path);
    p->log_file = CreateFileW((WCHAR *)log_path16.data, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    LOG("teal dev build, mode: %s", p->smoke ? "smoke" : p->screenshot_path.len ? "screenshot" : "interactive");
#endif

    WNDCLASSEXW wc = {
        .cbSize = sizeof(wc),
        .lpfnWndProc = win32_wndproc,
        .hInstance = instance,
        .hCursor = LoadCursorW(NULL, IDC_ARROW),
        .lpszClassName = L"teal",
    };
    if (!RegisterClassExW(&wc)) os_fatal(STR8_LIT("RegisterClassExW failed."));

    // Create hidden on the monitor under the mouse cursor, then size it for that monitor's DPI.
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO monitor = { .cbSize = sizeof(monitor) };
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT work = monitor.rcWork;

    DWORD style = WS_OVERLAPPEDWINDOW;
    p->hwnd = CreateWindowExW(0, L"teal", L"teal", style, work.left, work.top, 640, 400, NULL, NULL, instance, NULL);
    if (!p->hwnd) os_fatal(STR8_LIT("CreateWindowExW failed."));
    p->dpi = GetDpiForWindow(p->hwnd);

    RECT rect = { 0, 0, MulDiv(1280, (int)p->dpi, 96), MulDiv(800, (int)p->dpi, 96) };
    AdjustWindowRectExForDpi(&rect, style, FALSE, 0, p->dpi);
    i32 work_w = work.right - work.left, work_h = work.bottom - work.top;
    i32 win_w = MIN(rect.right - rect.left, work_w);
    i32 win_h = MIN(rect.bottom - rect.top, work_h);
    SetWindowPos(p->hwnd, NULL, work.left + (work_w - win_w) / 2, work.top + (work_h - win_h) / 2, win_w, win_h,
                 SWP_NOZORDER | SWP_NOACTIVATE);

    // Dark title bar; on Windows 11 also paint the caption in the background color. Failures are fine.
    BOOL dark = TRUE;
    DwmSetWindowAttribute(p->hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF caption = RGB((THEME_BACKGROUND >> 16) & 0xFF, (THEME_BACKGROUND >> 8) & 0xFF, THEME_BACKGROUND & 0xFF);
    DwmSetWindowAttribute(p->hwnd, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));

    RECT client;
    GetClientRect(p->hwnd, &client);
    p->width = client.right - client.left;
    p->height = client.bottom - client.top;

    p->renderer = r_create(&p->perm, p->hwnd, p->width, p->height);
    if (!p->renderer) {
#if TEAL_DEV
        if (p->smoke || p->screenshot_path.len) {
            LOG("renderer initialization failed");
            return EXIT_RENDERER_INIT;
        }
#endif
        os_fatal(STR8_LIT("Could not initialize Direct3D 11."));
    }
    p->app = app_create(&p->perm);

#if TEAL_DEV
    if (p->screenshot_path.len) {
        exit_code = win32_screenshot(p);
        p->quit = 1;
    }
#endif

    if (!p->quit) {
        // Show cloaked, present the first frame, then uncloak: the window never shows
        // an unpainted (white) client area.
        BOOL cloak = TRUE;
        DwmSetWindowAttribute(p->hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
#if TEAL_DEV
        ShowWindow(p->hwnd, p->smoke ? SW_SHOWNOACTIVATE : SW_SHOW);
#else
        ShowWindow(p->hwnd, SW_SHOW);
#endif
        win32_frame(p);
        cloak = FALSE;
        DwmSetWindowAttribute(p->hwnd, DWMWA_CLOAK, &cloak, sizeof(cloak));
    }

    while (!p->quit) {
        // Block until there is something to do: 0% CPU when idle.
        if (!p->redraw) MsgWaitForMultipleObjectsEx(0, NULL, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);

        MSG msg;
        for (;;) {
            // Never drop events: when the array is nearly full, run a frame to consume it
            // and leave the remaining messages in the Windows queue meanwhile.
            if (p->event_count > EVENT_CAPACITY - EVENT_RESERVE) win32_frame(p);
            if (!PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) break;
            if (msg.message == WM_QUIT) {
                p->quit = 1;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (p->quit) break;

#if TEAL_DEV
        if (p->smoke) {
            // Two plain frames, then a third one whose back buffer is read back and checked.
            if (p->frame_count >= 2) {
                r_request_capture(p->renderer);
                win32_frame(p);
                exit_code = win32_smoke_check_pixels(p);
                p->quit = 1;
                break;
            }
            p->redraw = 1;
        }
#endif
        if (p->redraw) win32_frame(p);
    }

    u32 leaks = r_shutdown(p->renderer);
    DestroyWindow(p->hwnd);

#if TEAL_DEV
    if (p->smoke) {
        if (exit_code == EXIT_OK && !r_dev_debug_layer_active(p->renderer)) {
            LOG("smoke: FAIL: D3D11 debug layer is not active");
            exit_code = EXIT_NO_DEBUG_LAYER;
        }
        if (exit_code == EXIT_OK && r_dev_message_count(p->renderer) != 0) {
            LOG("smoke: FAIL: %u debug-layer message(s)", r_dev_message_count(p->renderer));
            exit_code = EXIT_DEBUG_MESSAGES;
        }
        if (exit_code == EXIT_OK && leaks != 0) {
            LOG("smoke: FAIL: %u leaked reference(s) / live object(s)", leaks);
            exit_code = EXIT_LEAK;
        }
        LOG("smoke: %s (exit %d, %D frames)", exit_code == EXIT_OK ? "PASS" : "FAIL", exit_code, p->frame_count);
    }
    if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) CloseHandle(p->log_file);
#else
    (void)leaks;
#endif
    return exit_code;
}
