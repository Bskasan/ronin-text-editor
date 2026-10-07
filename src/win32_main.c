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
    i32 exit_code;

    // Command-line options.
    b32 startup_ms;       // --startup-ms: exit after the first Present, exit code = ms since process creation
    f32 forced_scale;     // --scale <percent>, 0 = follow the monitor DPI
    String8 file_path;    // the first argument that is not a flag
    FbRenderMode render_mode;
#if TEAL_DEV
    HANDLE log_file;
    b32 smoke;
    b32 test;
    b32 sample;
    i64 top_line;
    b32 top_line_end;
    u64 seed;
    String8 exe_dir;
    b32 bench_text;
    String8 screenshot_path;
    String8 atlas_path;
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

b32 os_release(void *ptr) {
    return VirtualFree(ptr, 0, MEM_RELEASE) != 0;
}

// ---------------------------------------------------------------------------
// Files

static i64 win32_filetime_u64(FILETIME ft) {
    return (i64)(((u64)ft.dwHighDateTime << 32) | ft.dwLowDateTime);
}

static OsFileStatus win32_file_status(DWORD error) {
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:     return OS_FILE_NOT_FOUND;
    case ERROR_ACCESS_DENIED:      return OS_FILE_ACCESS_DENIED;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:     return OS_FILE_SHARING_VIOLATION;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:   return OS_FILE_DISK_FULL;
    case ERROR_WRITE_PROTECT:      return OS_FILE_READ_ONLY;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:        return OS_FILE_OUT_OF_MEMORY;
    case ERROR_INVALID_NAME:
    case ERROR_BAD_PATHNAME:
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_INVALID_DRIVE:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
    case ERROR_DIRECTORY:          return OS_FILE_BAD_PATH;
    default:                       return OS_FILE_IO_ERROR;
    }
}

static OsFileStatus win32_last_file_status(void) {
    return win32_file_status(GetLastError());
}

// Wide, NUL-terminated copy of a path in the scratch arena; the caller pops it.
static WCHAR *win32_path16(String8 path) {
    return (WCHAR *)str16_from_str8(&g_platform->scratch, path).data;
}

String8 os_full_path(Arena *arena, String8 path) {
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    String8 result = { 0 };
    WCHAR *path16 = win32_path16(path);
    DWORD cap = GetFullPathNameW(path16, 0, NULL, NULL);
    if (path.len && cap) {
        WCHAR *full = PUSH_ARRAY(scratch, WCHAR, cap);
        DWORD len = GetFullPathNameW(path16, cap, full, NULL);
        if (len && len < cap) result = str8_from_str16(scratch, (u16 *)full, len);
    }
    if (arena != scratch) {
        String8 copy = result.len ? str8_copy(arena, result) : result;
        arena_pop_to(scratch, mark);
        result = copy;
    }
    return result;
}

static void win32_fill_info(OsFileInfo *info, BY_HANDLE_FILE_INFORMATION *bhfi) {
    info->exists = 1;
    info->is_dir = (bhfi->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    info->read_only = (bhfi->dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    info->size = (i64)(((u64)bhfi->nFileSizeHigh << 32) | bhfi->nFileSizeLow);
    info->write_time = (u64)win32_filetime_u64(bhfi->ftLastWriteTime);
}

OsFileStatus os_file_info(String8 path, OsFileInfo *info) {
    memset(info, 0, sizeof(*info));
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    WCHAR *path16 = win32_path16(path);
    OsFileStatus status = OS_FILE_OK;
    WIN32_FILE_ATTRIBUTE_DATA data;
    if (!GetFileAttributesExW(path16, GetFileExInfoStandard, &data)) {
        status = win32_last_file_status();
    } else {
        info->exists = 1;
        info->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        info->read_only = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
        info->size = (i64)(((u64)data.nFileSizeHigh << 32) | data.nFileSizeLow);
        info->write_time = (u64)win32_filetime_u64(data.ftLastWriteTime);
        info->swap_ok = !(data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        if (info->swap_ok && !info->is_dir) {
            // Replacing a file that has other hard links would detach them.
            HANDLE h = CreateFileW(path16, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
            BY_HANDLE_FILE_INFORMATION bhfi;
            if (h != INVALID_HANDLE_VALUE) {
                if (GetFileInformationByHandle(h, &bhfi) && bhfi.nNumberOfLinks > 1) info->swap_ok = 0;
                CloseHandle(h);
            }
        }
    }
    arena_pop_to(scratch, mark);
    return status;
}

OsFileStatus os_file_open_read(String8 path, OsFile *file, OsFileInfo *info) {
    memset(info, 0, sizeof(*info));
    file->handle = NULL;
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    WCHAR *path16 = win32_path16(path);
    DWORD attrs = GetFileAttributesW(path16);
    OsFileStatus status = OS_FILE_OK;
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        status = OS_FILE_IS_DIRECTORY;
    } else {
        HANDLE h = CreateFileW(path16, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
        BY_HANDLE_FILE_INFORMATION bhfi;
        if (h == INVALID_HANDLE_VALUE) {
            status = win32_last_file_status();
        } else if (!GetFileInformationByHandle(h, &bhfi)) {
            status = win32_last_file_status();
            CloseHandle(h);
        } else {
            win32_fill_info(info, &bhfi);
            file->handle = h;
        }
    }
    arena_pop_to(scratch, mark);
    return status;
}

OsFileStatus os_file_read(OsFile file, void *dst, i64 size) {
    u8 *p = (u8 *)dst;
    while (size > 0) {
        DWORD chunk = (DWORD)MIN(size, (i64)MB(256));
        DWORD got = 0;
        if (!ReadFile((HANDLE)file.handle, p, chunk, &got, NULL)) return win32_last_file_status();
        if (got == 0) return OS_FILE_IO_ERROR; // the file shrank while we read it
        p += got;
        size -= got;
    }
    return OS_FILE_OK;
}

OsFileStatus os_file_create_temp(String8 target, Arena *arena, OsFile *file, String8 *temp_path) {
    file->handle = NULL;
    *temp_path = str8(NULL, 0);
    Arena *scratch = &g_platform->scratch;
    for (u32 n = 1; n < 100; n++) {
        u64 mark = arena_pos(scratch);
        String8 name = str8_fmt(scratch, "%S.teal~%u", target, n);
        HANDLE h = CreateFileW(win32_path16(name), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD error = GetLastError();
        if (h != INVALID_HANDLE_VALUE) {
            String8 copy = str8_copy(arena, name);
            if (arena != scratch) arena_pop_to(scratch, mark);
            file->handle = h;
            *temp_path = copy;
            return OS_FILE_OK;
        }
        arena_pop_to(scratch, mark);
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) return win32_file_status(error);
    }
    return OS_FILE_IO_ERROR;
}

OsFileStatus os_file_open_overwrite(String8 path, OsFile *file) {
    file->handle = NULL;
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    // OPEN_ALWAYS + SetEndOfFile instead of CREATE_ALWAYS, which refuses hidden and system files.
    HANDLE h = CreateFileW(win32_path16(path), GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    arena_pop_to(scratch, mark);
    if (h == INVALID_HANDLE_VALUE) return win32_last_file_status();
    if (!SetEndOfFile(h)) {
        OsFileStatus status = win32_last_file_status();
        CloseHandle(h);
        return status;
    }
    file->handle = h;
    return OS_FILE_OK;
}

OsFileStatus os_file_write(OsFile file, void *data, i64 size) {
    u8 *p = (u8 *)data;
    while (size > 0) {
        DWORD chunk = (DWORD)MIN(size, (i64)MB(256));
        DWORD written = 0;
        if (!WriteFile((HANDLE)file.handle, p, chunk, &written, NULL)) return win32_last_file_status();
        if (written == 0) return OS_FILE_IO_ERROR;
        p += written;
        size -= written;
    }
    return OS_FILE_OK;
}

OsFileStatus os_file_flush(OsFile file) {
    return FlushFileBuffers((HANDLE)file.handle) ? OS_FILE_OK : win32_last_file_status();
}

void os_file_close(OsFile file) {
    if (file.handle) CloseHandle((HANDLE)file.handle);
}

OsFileStatus os_file_replace(String8 target, String8 temp) {
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    WCHAR *target16 = win32_path16(target);
    WCHAR *temp16 = win32_path16(temp);
    BOOL ok;
    if (GetFileAttributesW(target16) == INVALID_FILE_ATTRIBUTES) {
        ok = MoveFileExW(temp16, target16, MOVEFILE_WRITE_THROUGH);
    } else {
        // Keeps the target's attributes, ACLs, creation time and short name. On failure the
        // target is still there under its own name (we pass no backup file).
        ok = ReplaceFileW(target16, temp16, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                          NULL, NULL);
    }
    OsFileStatus status = ok ? OS_FILE_OK : win32_last_file_status();
    arena_pop_to(scratch, mark);
    return status;
}

void os_file_delete(String8 path) {
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    DeleteFileW(win32_path16(path));
    arena_pop_to(scratch, mark);
}

#if TEAL_DEV
b32 os_dev_set_read_only(String8 path, b32 read_only) {
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    WCHAR *path16 = win32_path16(path);
    DWORD attrs = GetFileAttributesW(path16);
    b32 ok = 0;
    if (attrs != INVALID_FILE_ATTRIBUTES) {
        attrs = read_only ? (attrs | FILE_ATTRIBUTE_READONLY) : (attrs & ~(DWORD)FILE_ATTRIBUTE_READONLY);
        ok = SetFileAttributesW(path16, attrs) != 0;
    }
    arena_pop_to(scratch, mark);
    return ok;
}
#endif

#if TEAL_DEV
b32 os_dev_hard_link(String8 existing, String8 link) {
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    b32 ok = CreateHardLinkW(win32_path16(link), win32_path16(existing), NULL) != 0;
    arena_pop_to(scratch, mark);
    return ok;
}
#endif

u64 os_time_us(void) {
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    return (u64)(now.QuadPart / freq.QuadPart * 1000000 + now.QuadPart % freq.QuadPart * 1000000 / freq.QuadPart);
}

void os_fatal(String8 message) {
    b32 interactive = 1;
#if TEAL_DEV
    LOG("fatal: %S", message);
    if (g_platform && (g_platform->smoke || g_platform->screenshot_path.len || g_platform->atlas_path.len ||
                       g_platform->bench_text)) interactive = 0;
#endif
    if (g_platform && g_platform->startup_ms) interactive = 0;
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

// ---------------------------------------------------------------------------
// Frames and events

static f32 win32_dpi_scale(Platform *p) {
    return p->forced_scale > 0 ? p->forced_scale : (f32)p->dpi / 96.0f;
}

static FrameInput win32_frame_input(Platform *p) {
    FrameInput input = { p->events, p->event_count, p->width, p->height, win32_dpi_scale(p), &p->scratch };
    return input;
}

static void win32_frame(Platform *p) {
    if (p->in_frame || !p->renderer || !p->app) return;
    p->in_frame = 1;

    FrameInput input = win32_frame_input(p);
    if (!app_update_and_render(p->app, &input, p->renderer)) p->quit = 1;
    p->event_count = 0;
    p->redraw = r_wants_redraw(p->renderer);
    arena_reset(&p->scratch);
    p->frame_count++;

    if (p->frame_count == 1) {
        FILETIME created, exited, kernel, user, now_ft;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        GetSystemTimePreciseAsFileTime(&now_ft);
        i64 us_process = (win32_filetime_u64(now_ft) - win32_filetime_u64(created)) / 10;
        if (p->startup_ms) {
            p->exit_code = (i32)((us_process + 500) / 1000);
            p->quit = 1;
        }
#if TEAL_DEV
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        i64 us_main = (now.QuadPart - p->qpc_start.QuadPart) * 1000000 / freq.QuadPart;
        LOG("startup: first Present %D.%03D ms after WinMain, %D.%03D ms after process creation",
            us_main / 1000, us_main % 1000, us_process / 1000, us_process % 1000);
#endif
    }
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
        if (p->forced_scale > 0) return 0; // --scale: keep the forced scale and size
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
    EXIT_OUTPUT_FILE = 7,
    EXIT_FONT = 8,
    EXIT_TEST = 9,
};

static u32 win32_pixel_rgb(u8 *bgra, i32 w, i32 x, i32 y) {
    u8 *px = bgra + ((i64)y * w + x) * 4;
    return ((u32)px[2] << 16) | ((u32)px[1] << 8) | px[0];
}

// Compares the normalized coverage of two channels of `out` without dividing:
// cov(c) = (out_c - bg_c) / (text_c - bg_c); returns a value with the sign of cov(a) - cov(b).
// Both denominators are positive for light text on a dark background.
static i64 win32_coverage_compare(u32 out, u32 bg, u32 text, i32 shift_a, i32 shift_b) {
    i64 oa = (out >> shift_a) & 0xFF, ob = (out >> shift_b) & 0xFF;
    i64 ba = (bg >> shift_a) & 0xFF, bb = (bg >> shift_b) & 0xFF;
    i64 ta = (text >> shift_a) & 0xFF, tb = (text >> shift_b) & 0xFF;
    return (oa - ba) * (tb - bb) - (ob - bb) * (ta - ba);
}

static b32 win32_check_probe(DevProbe *pr, u8 *pixels, i32 w, i32 h) {
    i32 x1 = pr->kind == DEV_PROBE_PIXEL_EQ ? pr->x0 + 1 : pr->x1;
    i32 y1 = pr->kind == DEV_PROBE_PIXEL_EQ ? pr->y0 + 1 : pr->y1;
    if (pr->x0 < 0 || pr->y0 < 0 || x1 > w || y1 > h || pr->x0 >= x1 || pr->y0 >= y1) {
        LOG("smoke: FAIL: '%s': region (%d,%d)-(%d,%d) is outside %dx%d", pr->what, pr->x0, pr->y0, x1, y1, w, h);
        return 0;
    }
    switch (pr->kind) {
    case DEV_PROBE_PIXEL_EQ:
    case DEV_PROBE_REGION_EQ:
        for (i32 y = pr->y0; y < y1; y++) {
            for (i32 x = pr->x0; x < x1; x++) {
                u32 got = win32_pixel_rgb(pixels, w, x, y);
                if (got != pr->rgb) {
                    LOG("smoke: FAIL: '%s': (%d, %d) is #%06x, expected #%06x", pr->what, x, y, got, pr->rgb);
                    return 0;
                }
            }
        }
        LOG("smoke: ok: '%s': (%d,%d)-(%d,%d) all #%06x", pr->what, pr->x0, pr->y0, x1, y1, pr->rgb);
        return 1;
    case DEV_PROBE_REGION_DIFFERS:
        for (i32 y = pr->y0; y < y1; y++) {
            for (i32 x = pr->x0; x < x1; x++) {
                u32 got = win32_pixel_rgb(pixels, w, x, y);
                if (got != pr->rgb) {
                    LOG("smoke: ok: '%s': (%d, %d) is #%06x, differs from #%06x", pr->what, x, y, got, pr->rgb);
                    return 1;
                }
            }
        }
        LOG("smoke: FAIL: '%s': (%d,%d)-(%d,%d) is entirely #%06x", pr->what, pr->x0, pr->y0, x1, y1, pr->rgb);
        return 0;
    case DEV_PROBE_CLEARTYPE: {
        if (pr->geometry == FB_PIXELS_FLAT) {
            LOG("smoke: skip: '%s': pixel geometry is FLAT, no channel order to check", pr->what);
            return 1;
        }
        i32 y = (pr->y0 + y1) / 2, left = -1, right = -1;
        for (i32 x = pr->x0; x < x1; x++) {
            if (win32_pixel_rgb(pixels, w, x, y) != pr->rgb) {
                if (left < 0) left = x;
                right = x;
            }
        }
        if (left < 0) {
            LOG("smoke: FAIL: '%s': no stem found on row %d", pr->what, y);
            return 0;
        }
        u32 lp = win32_pixel_rgb(pixels, w, left, y), rp = win32_pixel_rgb(pixels, w, right, y);
        // RGB stripe: the left edge of a light stem lights its rightmost subpixel (blue) at least as
        // much as red; the right edge the other way round. BGR is mirrored.
        i64 left_ok, right_ok;
        if (pr->geometry == FB_PIXELS_RGB) {
            left_ok = win32_coverage_compare(lp, pr->rgb, pr->text_rgb, 0, 16);  // cov(B) - cov(R)
            right_ok = win32_coverage_compare(rp, pr->rgb, pr->text_rgb, 16, 0); // cov(R) - cov(B)
        } else {
            left_ok = win32_coverage_compare(lp, pr->rgb, pr->text_rgb, 16, 0);
            right_ok = win32_coverage_compare(rp, pr->rgb, pr->text_rgb, 0, 16);
        }
        b32 ok = left_ok >= 0 && right_ok >= 0;
        LOG("smoke: %s: '%s': row %d, left edge x=%d #%06x, right edge x=%d #%06x, geometry %s",
            ok ? "ok" : "FAIL", pr->what, y, left, lp, right, rp, pr->geometry == FB_PIXELS_RGB ? "RGB" : "BGR");
        return ok;
    }
    }
    return 0;
}

// Called right after the captured frame.
static i32 win32_smoke_check_frame(Platform *p) {
    i32 w, h;
    u8 *pixels = r_read_capture(p->renderer, &p->scratch, &w, &h);
    if (!pixels) {
        LOG("smoke: FAIL: back buffer capture failed");
        return EXIT_PIXEL_MISMATCH;
    }
    FrameInput input = win32_frame_input(p);
    DevProbe probes[16];
    i32 count = app_dev_probes(p->app, &input, probes, ARRAY_COUNT(probes));
    i32 result = EXIT_OK;
    for (i32 i = 0; i < count; i++) {
        if (!win32_check_probe(&probes[i], pixels, w, h) && result == EXIT_OK) {
            result = probes[i].kind == DEV_PROBE_CLEARTYPE ? EXIT_FONT : EXIT_PIXEL_MISMATCH;
        }
    }
    if (count < 8) {
        LOG("smoke: FAIL: expected 8 probes, the app produced %d", count);
        if (result == EXIT_OK) result = EXIT_PIXEL_MISMATCH;
    }
    if (!app_dev_atlas_has_coverage(p->app)) {
        LOG("smoke: FAIL: the glyph atlas has no coverage");
        if (result == EXIT_OK) result = EXIT_FONT;
    } else {
        LOG("smoke: ok: the glyph atlas has coverage");
    }
    arena_reset(&p->scratch);
    return result;
}

static b32 win32_write_png(Platform *p, String8 path, u8 *bgra, i32 w, i32 h) {
    String8 png = png_encode_bgra(&p->scratch, bgra, w, h, (i64)w * 4);
    if (!os_write_file(path, png)) {
        LOG("could not write %S", path);
        return 0;
    }
    LOG("wrote %S (%dx%d, %D bytes)", path, w, h, png.len);
    return 1;
}

// --screenshot and --dump-atlas: one frame with the window hidden, then write the files.
static i32 win32_write_outputs(Platform *p) {
    r_request_capture(p->renderer);
    win32_frame(p);
    i32 result = EXIT_OK;
    if (p->screenshot_path.len) {
        i32 w, h;
        u8 *pixels = r_read_capture(p->renderer, &p->scratch, &w, &h);
        if (!pixels || !win32_write_png(p, p->screenshot_path, pixels, w, h)) result = EXIT_OUTPUT_FILE;
    }
    if (p->atlas_path.len) {
        i32 size;
        u8 *rgba = app_dev_atlas(p->app, &size);
        u8 *bgra = PUSH_ARRAY(&p->scratch, u8, (i64)size * size * 4);
        for (i64 i = 0; i < (i64)size * size * 4; i += 4) {
            bgra[i + 0] = rgba[i + 2];
            bgra[i + 1] = rgba[i + 1];
            bgra[i + 2] = rgba[i + 0];
            bgra[i + 3] = 255;
        }
        if (!win32_write_png(p, p->atlas_path, bgra, size, size)) result = EXIT_OUTPUT_FILE;
    }
    arena_reset(&p->scratch);
    return result;
}

// --bench-text: 300 frames of a window full of text, presented with Present(0, 0).
static void win32_bench_text(Platform *p) {
    enum { FRAMES = 300 };
    r_dev_set_present_interval(p->renderer, 0);
    u64 build_sum = 0, build_max = 0, submit_sum = 0, submit_max = 0;
    i32 glyphs = 0;
    for (i32 i = 0; i < FRAMES; i++) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        p->event_count = 0;
        FrameInput input = win32_frame_input(p);
        u64 build_us, submit_us;
        glyphs = app_dev_bench_frame(p->app, &input, p->renderer, &build_us, &submit_us);
        arena_reset(&p->scratch);
        build_sum += build_us;
        submit_sum += submit_us;
        build_max = MAX(build_max, build_us);
        submit_max = MAX(submit_max, submit_us);
    }
    r_dev_set_present_interval(p->renderer, 1);
    LOG("bench-text: %d frames, %dx%d client, %d glyphs per frame", (i32)FRAMES, p->width, p->height, glyphs);
    LOG("bench-text: build (push calls): avg %U us, worst %U us", build_sum / FRAMES, build_max);
    LOG("bench-text: flush + Present:    avg %U us, worst %U us", submit_sum / FRAMES, submit_max);
}

static b32 win32_dev_batch_mode(Platform *p) {
    return p->smoke || p->bench_text || p->screenshot_path.len || p->atlas_path.len;
}
#endif

// ---------------------------------------------------------------------------
// Entry point

// D3D11CreateDevice is most of the startup time (driver load), so it runs on a worker thread
// while the main thread creates the window and the font. The worker touches only the Renderer
// fields r_create_device writes: no logging, no arenas. The main thread joins before any other
// D3D call; the device stays SINGLETHREADED and is never used by two threads at once.
static DWORD WINAPI win32_device_thread(LPVOID param) {
    r_create_device((Renderer *)param);
    return 0;
}

static i32 win32_parse_i32(String8 s) {
    i32 v = 0;
    for (i64 i = 0; i < s.len && s.data[i] >= '0' && s.data[i] <= '9'; i++) v = v * 10 + (s.data[i] - '0');
    return v;
}

#if TEAL_DEV
// Decimal, or hexadecimal with 0x.
static u64 win32_parse_u64(String8 s) {
    u64 v = 0;
    if (s.len > 2 && s.data[0] == '0' && (s.data[1] == 'x' || s.data[1] == 'X')) {
        for (i64 i = 2; i < s.len; i++) {
            u8 c = s.data[i];
            u32 d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
            if (d == 16) break;
            v = v * 16 + d;
        }
    } else {
        for (i64 i = 0; i < s.len && s.data[i] >= '0' && s.data[i] <= '9'; i++) v = v * 10 + (s.data[i] - '0');
    }
    return v;
}
#endif

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
    p->render_mode = FB_RENDER_NATURAL_SYMMETRIC;
#if TEAL_DEV
    p->seed = 0x7ea1;
#endif
    g_platform = p;

    String8 *args;
    i32 arg_count = win32_parse_args(&p->perm, &args);
    for (i32 i = 1; i < arg_count; i++) {
        String8 a = args[i];
        if (str8_equal(a, STR8_LIT("--startup-ms"))) p->startup_ms = 1;
        else if (!p->file_path.len && !(a.len >= 2 && a.data[0] == '-' && a.data[1] == '-')) p->file_path = a;
#if TEAL_DEV
        b32 has_value = i + 1 < arg_count;
        if (str8_equal(a, STR8_LIT("--smoke"))) p->smoke = 1;
        else if (str8_equal(a, STR8_LIT("--test"))) p->test = 1;
        else if (str8_equal(a, STR8_LIT("--sample"))) p->sample = 1;
        else if (str8_equal(a, STR8_LIT("--top-line")) && has_value) {
            String8 v = args[++i];
            if (str8_equal(v, STR8_LIT("end"))) p->top_line_end = 1;
            else p->top_line = (i64)win32_parse_u64(v);
        }
        else if (str8_equal(a, STR8_LIT("--seed")) && has_value) p->seed = win32_parse_u64(args[++i]);
        else if (str8_equal(a, STR8_LIT("--bench-text"))) p->bench_text = 1;
        else if (str8_equal(a, STR8_LIT("--screenshot")) && has_value) p->screenshot_path = args[++i];
        else if (str8_equal(a, STR8_LIT("--dump-atlas")) && has_value) p->atlas_path = args[++i];
        else if (str8_equal(a, STR8_LIT("--scale")) && has_value) p->forced_scale = (f32)win32_parse_i32(args[++i]) / 100.0f;
        else if (str8_equal(a, STR8_LIT("--render-mode")) && has_value) {
            String8 mode = args[++i];
            if (str8_equal(mode, STR8_LIT("classic"))) p->render_mode = FB_RENDER_GDI_CLASSIC;
            else if (str8_equal(mode, STR8_LIT("natural"))) p->render_mode = FB_RENDER_NATURAL;
            else p->render_mode = FB_RENDER_NATURAL_SYMMETRIC;
        }
#endif
    }

#if TEAL_DEV
    // Log next to the executable (build\teal.log), independent of the working directory.
    u16 exe_path[MAX_PATH * 4];
    DWORD exe_len = GetModuleFileNameW(NULL, (WCHAR *)exe_path, ARRAY_COUNT(exe_path));
    while (exe_len > 0 && exe_path[exe_len - 1] != '\\') exe_len--;
    String8 exe_dir = str8_from_str16(&p->perm, exe_path, exe_len > 0 ? exe_len - 1 : 0);
    String8 log_path = str8_fmt(&p->perm, "%S\\teal.log", exe_dir);
    String16 log_path16 = str16_from_str8(&p->perm, log_path);
    p->log_file = CreateFileW((WCHAR *)log_path16.data, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    p->exe_dir = exe_dir;
    LOG("teal dev build, mode: %s", p->test ? "test" : p->smoke ? "smoke" : p->bench_text ? "bench-text"
                                   : (p->screenshot_path.len || p->atlas_path.len) ? "capture" : "interactive");
    if (p->test) { // headless: no window, no device, no font
        i32 failures = test_run(p->seed, str8_fmt(&p->perm, "%S\\tmp", exe_dir));
        if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) CloseHandle(p->log_file);
        return failures ? EXIT_TEST : EXIT_OK;
    }
#endif

    // Argument parsing above takes microseconds; the device thread starts right after it.
    Renderer *renderer = r_alloc(&p->perm); // allocated here, before the worker starts
    HANDLE device_thread = CreateThread(NULL, 0, win32_device_thread, renderer, 0, NULL);


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

    f32 scale = win32_dpi_scale(p);
    RECT rect = { 0, 0, (LONG)(1280 * scale + 0.5f), (LONG)(800 * scale + 0.5f) };
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

    AppConfig config = { .dpi_scale = scale, .render_mode = p->render_mode, .file_path = p->file_path };
#if TEAL_DEV
    config.sample = p->sample || p->smoke; // the smoke probes check the sample
    config.top_line = p->top_line;
    config.top_line_end = p->top_line_end;
#endif
    p->app = app_create(&p->perm, &config);
    if (!p->app) {
#if TEAL_DEV
        if (win32_dev_batch_mode(p)) {
            LOG("font initialization failed");
            return EXIT_FONT;
        }
#endif
        os_fatal(STR8_LIT("Could not open the font (Consolas or Courier New)."));
    }

#if TEAL_DEV
    u64 join_start = os_time_us();
#endif
    if (device_thread) {
        WaitForSingleObject(device_thread, INFINITE);
        CloseHandle(device_thread);
    } else {
        r_create_device(renderer); // no thread: create it here
    }
#if TEAL_DEV
    LOG("startup: device thread %s, main thread waited %U us for it", device_thread ? "used" : "unavailable",
        os_time_us() - join_start);
#endif
    if (!r_finish_create(renderer, p->hwnd, p->width, p->height)) {
#if TEAL_DEV
        if (win32_dev_batch_mode(p)) {
            LOG("renderer initialization failed");
            return EXIT_RENDERER_INIT;
        }
#endif
        os_fatal(STR8_LIT("Could not initialize Direct3D 11."));
    }
    p->renderer = renderer; // from here on WM_SIZE / WM_PAINT may render

#if TEAL_DEV
    if (p->screenshot_path.len || p->atlas_path.len) {
        p->exit_code = win32_write_outputs(p);
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

#if TEAL_DEV
    if (p->bench_text && !p->quit) {
        win32_bench_text(p);
        p->quit = 1;
    }
#endif

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
                p->exit_code = win32_smoke_check_frame(p);
                p->quit = 1;
                break;
            }
            p->redraw = 1;
        }
#endif
        if (p->redraw) win32_frame(p);
    }

    u32 leaks = r_shutdown(p->renderer);
    i32 font_refs = app_shutdown(p->app); // DirectWrite references + unreleased buffers
    DestroyWindow(p->hwnd);

#if TEAL_DEV
    LOG("app: resources still held after close (DirectWrite references, buffers): %d", font_refs);
    if (p->smoke) {
        i32 code = p->exit_code;
#if TEAL_D3D_DEBUG
        if (code == EXIT_OK && !r_dev_debug_layer_active(p->renderer)) {
            LOG("smoke: FAIL: D3D11 debug layer is not active");
            code = EXIT_NO_DEBUG_LAYER;
        }
#else
        LOG("smoke: skip: the D3D11 debug layer is off in this build");
#endif
        if (code == EXIT_OK && r_dev_message_count(p->renderer) != 0) {
            LOG("smoke: FAIL: %u debug-layer message(s)", r_dev_message_count(p->renderer));
            code = EXIT_DEBUG_MESSAGES;
        }
        if (code == EXIT_OK && (leaks != 0 || font_refs != 0)) {
            LOG("smoke: FAIL: %u leaked D3D reference(s) / live object(s), %d app resource(s) (DirectWrite, buffers)", leaks, font_refs);
            code = EXIT_LEAK;
        }
        LOG("smoke: %s (exit %d, %D frames)", code == EXIT_OK ? "PASS" : "FAIL", code, p->frame_count);
        p->exit_code = code;
    }
    if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) CloseHandle(p->log_file);
#else
    (void)leaks;
    (void)font_refs;
#endif
    return p->exit_code;
}
