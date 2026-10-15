// win32_main.c — entry point, window, message loop, input translation, os_* primitives.

#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dwmapi.h>
#if RONIN_DEV
#define PSAPI_VERSION 2 // K32GetProcessMemoryInfo, exported by kernel32: no psapi.lib
#include <psapi.h>
#include <tlhelp32.h> // the leak check's process thread count
#endif

// platform.h's modifier bits are KEYMOD_*: winuser.h defines MOD_ALT (0x1) and MOD_SHIFT (0x4) for
// RegisterHotKey, which in this unity build replaced the old MOD_* names here (Alt read as Ctrl).
_Static_assert(KEYMOD_CTRL == 1 << 0 && KEYMOD_ALT == 1 << 1 && KEYMOD_SHIFT == 1 << 2, "platform.h's modifier bits");

#define EVENT_CAPACITY 256
#define EVENT_RESERVE 4 // free slots required before taking another message off the queue
#define WIN32_MAX_WATCHES 16 // the config's directory and those of the displayed buffers; the wait takes 63
#define WIN32_TYPE_AHEAD_TAG ((LPARAM)0x7EA1A11E) // --type-ahead-check: dwExtraInfo of its keys (real typing is told apart)

typedef struct Platform {
    HINSTANCE instance;
    HWND hwnd;
    HRESULT (WINAPI *dwm_set_attribute)(HWND, DWORD, LPCVOID, DWORD); // dwmapi.dll, loaded at the window's creation
    HRESULT (WINAPI *dwm_flush)(void);
    Arena perm;
    Arena scratch; // reset after every frame
    App *app;
    Renderer *renderer;

    Event events[EVENT_CAPACITY];
    i32 event_count;
    HANDLE watches[WIN32_MAX_WATCHES]; // change notifications; OsWatch = index + 1, NULL = free
    u8 watch_tree[WIN32_MAX_WATCHES];  // os_watch_tree: one-shot
    u8 watch_fired[WIN32_MAX_WATCHES]; // ... it fired: out of the wait until os_watch_rearm
    HANDLE wake;                       // os_wake_main: set by workers, part of the wait (auto-reset)
    HCURSOR cursors[MOUSE_CURSOR_COUNT]; // loaded once; app_mouse_cursor picks one
#if RONIN_DEV
    volatile LONG live_threads;        // os_thread_start minus joined
    volatile LONG live_events;         // os_event_create minus destroyed
    volatile LONG live_processes;      // os_process_start minus os_process_close
    volatile LONG live_handles;        // the ledger: kernel handles opened by the os_* layer and not closed
    b32 background_normal;            // --bench-grep: background workers at normal priority (the comparison)
    b32 clip_fake;           // the in-memory clipboard: UTF-16 with CRLF, as the real one holds it
    u16 *clip_text;          // in its own reservation
    i64 clip_len;
    u64 clip_committed;
    u32 clip_seq;
#endif

    i32 width, height; // client area, pixels
    u32 dpi;
    b32 redraw;
    b32 quit;
    b32 minimized;
    b32 in_frame;
    b32 in_size_move;

    b32 altgr;          // set by the synthetic Left Ctrl that precedes Right Alt
    u16 high_surrogate; // pending WM_CHAR high surrogate
    i32 clicks;         // the last mouse-down: its click count, button, time and place (double / triple clicks)
    MouseButton click_button;
    DWORD click_time;
    i32 click_x, click_y;

    i64 frame_count;
    LARGE_INTEGER qpc_start;
    i32 exit_code;

    // Command-line options.
    b32 startup_ms;       // --startup-ms: exit after the first Present, exit code = ms since process creation
    b32 no_activate;      // --no-activate: shown without taking the foreground (memory measurements)
    b32 version;          // --version: prints "ronin <version>" and exits
    f32 forced_scale;     // --scale <percent>, 0 = follow the monitor DPI
    AppFileArg *files;    // the arguments that are not flags, with the +LINE[:COLUMN] before each
    i32 file_count;
    b32 render_mode_forced;
    FbRenderMode render_mode;
    b32 unsaved;      // some file buffer has unsaved changes (os_set_unsaved_files)
    b32 block_reason; // a shutdown block reason is registered
    b32 shown_early;  // early_window: shown in the background color before the device exists (os_show_window_early)
    HBRUSH early_brush; // ... painting the client area until the first frame
    // Crash recovery: the directory (os_recovery_dir) and, for the exception filter, it and its parent in UTF-16.
    String8 recovery_dir;
    u16 recovery16[1024], recovery_parent16[1024];
#if RONIN_DEV
    HANDLE log_file;
    b32 smoke;
    b32 test;
    b32 leak_check;     // --leak-check [SCENARIO|all] [--cycles N] (debug build): the leak audit (leak.c)
    String8 leak_which;
    i32 leak_cycles;

    u64 seed;
    u64 fuzz_first, fuzz_count; // --fuzz-seeds FIRST COUNT
    i32 fuzz_worker, fuzz_workers; // --fuzz-worker K W FIRST COUNT: worker K + 1 of W (0: not a worker)
    String8 dev_write_root;     // the soak: writes outside this directory are refused (os_dev_write_root)
    u64 dev_writes_refused;
    u64 soak_events;            // --soak N
    String8 map_report;         // --map-report MAP
    String8 check_exe, check_list; // --check-imports EXE LIST
    i32 title_sets; // os_set_window_title calls (the smoke expects exactly one)
    String8 exe_dir;
    b32 bench_text;
    b32 bench_buffer;
    b32 bench_view;
    b32 bench_edit;
    b32 bench_complete;
    b32 bench_syntax;
    b32 bench_search;
    b32 bench_windows;
    b32 bench_project;
    b32 bench_grep;
    b32 bench_compile;
    b32 bench_cursors;
    b32 bench_longline;
    b32 dev_crash;           // --crash: an access violation after --keys (the recovery files' test)
    String8 recovery_arg;    // --recovery-dir DIR
    String8 screenshot_path;
    String8 atlas_path;
    String8 config_path;
    i32 scan_hold; // --scan-hold N: every project scan stands still after N batches (the finder while scanning)
    i32 grep_hold; // --grep-hold N: every search's workers stand still after N files each (a search still running)
    String8 keys; // --keys: injected after startup
    String8 keys_after; // --keys-after: a screenshot run's keys once the workers' results of --keys are in
    String8 touch; // --touch: rewritten after the keys, then the app is activated (the changed-on-disk check)
    u32 compile_wait_ms;  // --compile-wait MS: a screenshot run waits up to MS for running builds to end
    String8 child_script; // --child: run this script (win32_child.c) and exit: the tests' scripted program
    b32 log_keys;  // --log-keys: every keyboard message and what became of it, in the log
    u32 key_seq;   // key and text events pushed so far (Event.dev_seq)
    Event last_key_event; // the last of them
    i32 dev_menu_paths;   // WM_SYSKEY*, WM_SYSCHAR, WM_SYSDEADCHAR, SC_KEYMENU left to DefWindowProc (menu, beep)
    i32 dev_keymenu;      // WM_SYSCOMMAND SC_KEYMENU received
    b32 idle_check;       // --idle-check: shown without focus, idle; CPU time, frames and wakeups measured
    b32 memory_report;    // --memory-report: shown like --idle-check; the private bytes by owner after the settle
    b32 bench_latency;    // --bench-latency: posted keys, from arrival to the screen
    b32 startup_capture;  // --startup-capture: what the screen shows of the window from the start
    struct StartupCapture *capture;
    struct LatencyBench *latency;
    b32 clipboard_check;  // --clipboard-check: the real clipboard through the app, saved and put back
    String8 dpi_check;    // --dpi-check <dir>: WM_DPICHANGED to 125% and back, a screenshot at each step
    b32 clip_no_history;  // what ronin puts on the clipboard stays out of clipboard history (the check's text)
    u64 idle_wakeups;     // passes of the main loop
    u64 idle_start_cpu, idle_start_wakeups;
    b32 idle_measuring;   // between the two marks: the window procedure's messages are counted
    u32 idle_mouse_msgs;  // ... from the mouse (the pointer moving over the window, the cursor it asks for)
    u32 idle_other_msgs;  // ... anything else
    UINT idle_other[8];   // the first of those
    i64 idle_start_frames;
    const char *stage_what[32]; // os_dev_stage: the startup timeline, logged after the first frame
    LARGE_INTEGER stage_qpc[32];
    i32 stage_count;
    b32 stages_logged;
    // --type-ahead-check: a thread types letters into the window as soon as it is the foreground one.
    b32 type_ahead_check;
    DWORD main_thread_id;
    HANDLE type_ahead_thread;
    volatile LONG type_ahead_state;   // 0 waiting, 1 typed, 2 never the foreground window, 3 lost it while typing
    LARGE_INTEGER type_ahead_first, type_ahead_last; // the first and the last key sent
    LARGE_INTEGER first_present;      // the first frame done (its Present returned)
    i32 type_ahead_focus_lost;        // keys sent while the main thread had no focus window (GetGUIThreadInfo)
    i32 dev_keydowns, dev_syskeydowns; // the check's own WM_KEYDOWN / WM_SYSKEYDOWN received (no focus window: the latter)
    i32 dev_foreign_keys;              // key-downs that the check did not send (someone typing meanwhile)
#endif
} Platform;

static void win32_dwm_attribute(Platform *p, DWORD attribute, const void *value, DWORD size);
static void win32_stack_guarantee(void);
#if RONIN_DEV
static b32 win32_dev_batch_mode(Platform *p);
#endif

// The window procedure has no user pointer before WM_NCCREATE; this is its only way in.
static Platform *g_platform;

// The handle ledger (dev builds): +1 next to every kernel handle the os_* layer opens, -1 where it is closed.
#if RONIN_DEV
#define WIN32_DEV_HANDLES(n) InterlockedAdd(&g_platform->live_handles, (n))
#else
#define WIN32_DEV_HANDLES(n) ((void)0)
#endif

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

#if RONIN_DEV
// Every os_reserve range, for --memory-report (our arenas against everything else in the address space). Any
// thread; dev only. The only global besides g_platform: the first reservations come before the Platform exists.
typedef struct Win32DevReservations {
    SRWLOCK lock;
    void *base[16384];
    i32 count;
} Win32DevReservations;
static Win32DevReservations win32_dev_reservations = { SRWLOCK_INIT };
#endif

void *os_reserve(u64 size) {
    void *p = VirtualAlloc(NULL, size, MEM_RESERVE, PAGE_READWRITE);
#if RONIN_DEV
    Win32DevReservations *r = &win32_dev_reservations;
    if (p) {
        AcquireSRWLockExclusive(&r->lock);
        if (r->count < (i32)ARRAY_COUNT(r->base)) r->base[r->count++] = p;
        ReleaseSRWLockExclusive(&r->lock);
    }
#endif
    return p;
}

b32 os_commit(void *ptr, u64 size) {
    return VirtualAlloc(ptr, size, MEM_COMMIT, PAGE_READWRITE) != NULL;
}

void os_decommit(void *ptr, u64 size) {
    if (size) VirtualFree(ptr, size, MEM_DECOMMIT);
}

#if RONIN_DEV
// The soak's guard (os_dev_write_root): while a root is set, a write, delete or new directory outside it
// is refused and counted, whatever path random keys reached in a file prompt.
static b32 win32_dev_write_refused(String8 path) {
    Platform *p = g_platform;
    if (!p || !p->dev_write_root.len) return 0;
    WCHAR in[1024], full[1024];
    win32_utf8_to_wide_fixed(path, (u16 *)in, ARRAY_COUNT(in));
    DWORD n = GetFullPathNameW(in, ARRAY_COUNT(full), full, NULL);
    String8 root = p->dev_write_root;
    b32 inside = n > (DWORD)root.len && n < ARRAY_COUNT(full) && full[root.len] == L'\\';
    for (i64 i = 0; i < root.len && inside; i++) {
        u32 a = full[i], b = root.data[i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (b >= 'A' && b <= 'Z') b += 32;
        inside = a == b;
    }
    if (inside) return 0;
    p->dev_writes_refused++;
    LOG("soak: refused a write outside %S: %S", root, path);
    return 1;
}

void os_dev_write_root(String8 dir) {
    g_platform->dev_write_root = dir;
    g_platform->dev_writes_refused = 0;
}

u64 os_dev_writes_refused(void) {
    return g_platform->dev_writes_refused;
}
#endif

b32 os_write_file(String8 path, String8 data) {
#if RONIN_DEV
    if (win32_dev_write_refused(path)) return 0;
#endif
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    String16 path16 = str16_from_str8(scratch, path);
    HANDLE file = CreateFileW((WCHAR *)path16.data, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    arena_pop_to(scratch, mark);
    if (file == INVALID_HANDLE_VALUE) return 0;
    WIN32_DEV_HANDLES(1);
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
    WIN32_DEV_HANDLES(-1);
    return ok;
}

b32 os_release(void *ptr) {
#if RONIN_DEV
    Win32DevReservations *r = &win32_dev_reservations;
    AcquireSRWLockExclusive(&r->lock);
    for (i32 i = 0; i < r->count; i++) {
        if (r->base[i] == ptr) {
            r->base[i] = r->base[--r->count];
            break;
        }
    }
    ReleaseSRWLockExclusive(&r->lock);
#endif
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

// Any thread: the pattern, the names and the list nodes all go into the caller's arena (the platform
// scratch is the main thread's).
OsFileStatus os_list_dir(Arena *arena, String8 dir, OsDirEntry **entries, i64 *count) {
    typedef struct Node { struct Node *next; OsDirEntry e; } Node;
    *entries = NULL;
    *count = 0;
    b32 slash = dir.len && (dir.data[dir.len - 1] == '\\' || dir.data[dir.len - 1] == '/');
    WCHAR *pattern = (WCHAR *)str16_from_str8(arena, str8_fmt(arena, slash ? "%S*" : "%S\\*", dir)).data;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW(pattern, FindExInfoBasic, &fd, FindExSearchNameMatch, NULL, FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND ? OS_FILE_OK : win32_file_status(error); // an empty directory (a root)
    }
    WIN32_DEV_HANDLES(1);
    // The names and the nodes as they come, then one array.
    Node *first = NULL, *last = NULL;
    i64 n = 0;
    do {
        WCHAR *name = fd.cFileName;
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0))) continue;
        i64 len = 0;
        while (name[len]) len++;
        Node *node = PUSH_STRUCT(arena, Node);
        node->e.name = str8_from_str16(arena, (u16 *)name, len);
        node->e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        node->e.reparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (last) last->next = node;
        else first = node;
        last = node;
        n++;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    WIN32_DEV_HANDLES(-1);
    OsDirEntry *out = PUSH_ARRAY(arena, OsDirEntry, n);
    i64 i = 0;
    for (Node *node = first; node; node = node->next) out[i++] = node->e;
    *entries = out;
    *count = n;
    return OS_FILE_OK;
}

// Any thread: everything in the caller's arena.
OsFileStatus os_read_small_file(Arena *arena, String8 path, i64 max, String8 *out) {
    *out = str8(NULL, 0);
    u64 mark = arena_pos(arena);
    WCHAR *path16 = (WCHAR *)str16_from_str8(arena, path).data;
    HANDLE h = CreateFileW(path16, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    arena_pop_to(arena, mark);
    if (h == INVALID_HANDLE_VALUE) return win32_last_file_status();
    WIN32_DEV_HANDLES(1);
    LARGE_INTEGER size;
    OsFileStatus status = OS_FILE_OK;
    if (!GetFileSizeEx(h, &size)) status = win32_last_file_status();
    else if (size.QuadPart > max) status = OS_FILE_TOO_LARGE;
    if (status == OS_FILE_OK) {
        u8 *data = PUSH_ARRAY(arena, u8, size.QuadPart);
        DWORD got = 0;
        if (size.QuadPart && (!ReadFile(h, data, (DWORD)size.QuadPart, &got, NULL) || got != (DWORD)size.QuadPart)) {
            status = OS_FILE_IO_ERROR;
            arena_pop_to(arena, mark);
        } else {
            *out = str8(data, size.QuadPart);
        }
    }
    CloseHandle(h);
    WIN32_DEV_HANDLES(-1);
    return status;
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
                WIN32_DEV_HANDLES(1);
                if (GetFileInformationByHandle(h, &bhfi) && bhfi.nNumberOfLinks > 1) info->swap_ok = 0;
                CloseHandle(h);
                WIN32_DEV_HANDLES(-1);
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
            WIN32_DEV_HANDLES(1);
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
#if RONIN_DEV
    if (win32_dev_write_refused(target)) return OS_FILE_ACCESS_DENIED;
#endif
    Arena *scratch = &g_platform->scratch;
    for (u32 n = 1; n < 100; n++) {
        u64 mark = arena_pos(scratch);
        String8 name = str8_fmt(scratch, "%S.ronin~%u", target, n);
        HANDLE h = CreateFileW(win32_path16(name), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        DWORD error = GetLastError();
        if (h != INVALID_HANDLE_VALUE) {
            String8 copy = str8_copy(arena, name);
            if (arena != scratch) arena_pop_to(scratch, mark);
            file->handle = h;
            WIN32_DEV_HANDLES(1);
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
#if RONIN_DEV
    if (win32_dev_write_refused(path)) return OS_FILE_ACCESS_DENIED;
#endif
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
    WIN32_DEV_HANDLES(1);
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
    if (!file.handle) return;
    CloseHandle((HANDLE)file.handle);
    WIN32_DEV_HANDLES(-1);
}

// Any thread: the path in the caller's arena.
OsFileStatus os_file_open_read_arena(Arena *arena, String8 path, OsFile *file, i64 *size) {
    file->handle = NULL;
    *size = 0;
    u64 mark = arena_pos(arena);
    WCHAR *path16 = (WCHAR *)str16_from_str8(arena, path).data;
    HANDLE h = CreateFileW(path16, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    arena_pop_to(arena, mark);
    if (h == INVALID_HANDLE_VALUE) return win32_last_file_status();
    LARGE_INTEGER s;
    if (!GetFileSizeEx(h, &s)) {
        OsFileStatus status = win32_last_file_status();
        CloseHandle(h);
        return status;
    }
    file->handle = h;
    WIN32_DEV_HANDLES(1);
    *size = s.QuadPart;
    return OS_FILE_OK;
}

OsFileStatus os_file_read_some(OsFile file, void *dst, i64 cap, i64 *got) {
    DWORD n = 0;
    *got = 0;
    if (!ReadFile((HANDLE)file.handle, dst, (DWORD)MIN(cap, (i64)MB(256)), &n, NULL)) return win32_last_file_status();
    *got = n;
    return OS_FILE_OK;
}

i32 os_cpu_count(void) {
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? (i32)n : 1;
}

OsFileStatus os_file_replace(String8 target, String8 temp) {
#if RONIN_DEV
    if (win32_dev_write_refused(target)) return OS_FILE_ACCESS_DENIED;
#endif
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
#if RONIN_DEV
    if (win32_dev_write_refused(path)) return;
#endif
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    DeleteFileW(win32_path16(path));
    arena_pop_to(scratch, mark);
}

// ---------------------------------------------------------------------------
// Clipboard

#define WIN32_CLIP_RESERVE GB(8)

static b32 win32_open_clipboard(Platform *p) {
    for (i32 attempt = 0; attempt < 5; attempt++) {
        if (OpenClipboard(p->hwnd)) return 1;
        Sleep(2); // another program holds it: try again shortly
    }
    return 0;
}

#if RONIN_DEV
// Stores UTF-16 units in the fake, growing its commit.
static u16 *win32_fake_clip_space(Platform *p, i64 units) {
    if (!p->clip_text) p->clip_text = (u16 *)os_reserve(WIN32_CLIP_RESERVE);
    u64 need = ALIGN_UP_POW2((u64)(units + 1) * 2, KB(64));
    if (!p->clip_text || need > WIN32_CLIP_RESERVE) return NULL;
    if (need > p->clip_committed) {
        if (!os_commit((u8 *)p->clip_text + p->clip_committed, need - p->clip_committed)) return NULL;
        p->clip_committed = need;
    }
    return p->clip_text;
}

void os_dev_clipboard_fake(b32 on) {
    g_platform->clip_fake = on;
}

void os_dev_clipboard_external(String8 text) {
    Platform *p = g_platform;
    u16 *out = win32_fake_clip_space(p, clip_utf16_len(text));
    if (!out) return;
    p->clip_len = clip_utf16_write(text, out);
    p->clip_seq++;
}
#endif

#if RONIN_DEV
// With the clipboard open and text set: keeps it out of Windows' clipboard history and cloud
// clipboard (--clipboard-check: its test text must not stay anywhere).
static void win32_dev_clip_exclude(void) {
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    DWORD *zero = h ? (DWORD *)GlobalLock(h) : NULL;
    if (!zero) {
        if (h) GlobalFree(h);
        return;
    }
    *zero = 0;
    GlobalUnlock(h);
    if (!SetClipboardData(RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing"), h)) GlobalFree(h);
}
#endif

b32 os_clipboard_set(String8 text) {
    Platform *p = g_platform;
    i64 units = clip_utf16_len(text);
#if RONIN_DEV
    if (p->clip_fake) {
        u16 *out = win32_fake_clip_space(p, units);
        if (!out) return 0;
        p->clip_len = clip_utf16_write(text, out);
        p->clip_seq++;
        return 1;
    }
#endif
    if (!win32_open_clipboard(p)) return 0;
    b32 ok = 0;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(units + 1) * 2);
    u16 *out = h ? (u16 *)GlobalLock(h) : NULL;
    if (out) {
        out[clip_utf16_write(text, out)] = 0; // straight into the clipboard's memory: no other copy
        GlobalUnlock(h);
        ok = SetClipboardData(CF_UNICODETEXT, h) != NULL;
    }
#if RONIN_DEV
    if (ok && p->clip_no_history) win32_dev_clip_exclude();
#endif
    if (h && !ok) GlobalFree(h);
    CloseClipboard();
    return ok;
}

b32 os_clipboard_get(Arena *arena, String8 *text) {
    Platform *p = g_platform;
    *text = str8(NULL, 0);
#if RONIN_DEV
    if (p->clip_fake) {
        if (!p->clip_text) return 0;
        *text = clip_utf8_from_utf16(arena, p->clip_text, p->clip_len);
        return 1;
    }
#endif
    if (!win32_open_clipboard(p)) return 0;
    b32 ok = 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    u16 *data = h ? (u16 *)GlobalLock(h) : NULL;
    if (data) {
        i64 cap = (i64)(GlobalSize(h) / 2), len = 0;
        while (len < cap && data[len]) len++;
        *text = clip_utf8_from_utf16(arena, data, len);
        GlobalUnlock(h);
        ok = 1;
    }
    CloseClipboard();
    return ok;
}

u32 os_clipboard_seq(void) {
#if RONIN_DEV
    if (g_platform->clip_fake) return g_platform->clip_seq;
#endif
    return (u32)GetClipboardSequenceNumber();
}

// ---------------------------------------------------------------------------
// Directory watches

OsWatch os_watch_dir(String8 dir) {
    Platform *p = g_platform;
    i32 slot = 0;
    while (slot < WIN32_MAX_WATCHES && p->watches[slot]) slot++;
    if (slot == WIN32_MAX_WATCHES) return 0;
    u64 mark = arena_pos(&p->scratch);
    HANDLE h = FindFirstChangeNotificationW(win32_path16(dir), FALSE,
                                            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    arena_pop_to(&p->scratch, mark);
    if (h == INVALID_HANDLE_VALUE) return 0;
    WIN32_DEV_HANDLES(1);
    p->watches[slot] = h;
    return (OsWatch)(slot + 1);
}

static HANDLE win32_tree_notification(Platform *p, String8 dir) {
    u64 mark = arena_pos(&p->scratch);
    HANDLE h = FindFirstChangeNotificationW(win32_path16(dir), TRUE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME);
    arena_pop_to(&p->scratch, mark);
    return h;
}

OsWatch os_watch_tree(String8 dir) {
    Platform *p = g_platform;
    i32 slot = 0;
    while (slot < WIN32_MAX_WATCHES && p->watches[slot]) slot++;
    if (slot == WIN32_MAX_WATCHES) return 0;
    HANDLE h = win32_tree_notification(p, dir);
    if (h == INVALID_HANDLE_VALUE) return 0;
    WIN32_DEV_HANDLES(1);
    p->watches[slot] = h;
    p->watch_tree[slot] = 1;
    p->watch_fired[slot] = 0;
    return (OsWatch)(slot + 1);
}

// A fresh notification instead of FindNextChangeNotification: the changes made since it fired would
// signal that one at once.
b32 os_watch_rearm(OsWatch watch, String8 dir) {
    Platform *p = g_platform;
    if (watch < 1 || watch > WIN32_MAX_WATCHES || !p->watches[watch - 1] || !p->watch_tree[watch - 1]) return 0;
    HANDLE h = win32_tree_notification(p, dir);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindCloseChangeNotification(p->watches[watch - 1]); // the ledger: one opened, one closed
    p->watches[watch - 1] = h;
    p->watch_fired[watch - 1] = 0;
    return 1;
}

void os_unwatch(OsWatch watch) {
    Platform *p = g_platform;
    if (watch < 1 || watch > WIN32_MAX_WATCHES || !p->watches[watch - 1]) return;
    FindCloseChangeNotification(p->watches[watch - 1]);
    WIN32_DEV_HANDLES(-1);
    p->watches[watch - 1] = NULL;
    p->watch_tree[watch - 1] = p->watch_fired[watch - 1] = 0;
}

// ---------------------------------------------------------------------------
// Threads: any thread may call these (platform.h). The dev counters are interlocked.

typedef struct Win32ThreadStart {
    OsThreadFn *fn;
    void *data;
} Win32ThreadStart;

static DWORD WINAPI win32_thread_main(void *param) {
    Win32ThreadStart start = *(Win32ThreadStart *)param;
    VirtualFree(param, 0, MEM_RELEASE);
    win32_stack_guarantee();
    start.fn(start.data);
    return 0;
}

b32 os_thread_start(OsThread *thread, OsThreadFn *fn, void *data, b32 background) {
    thread->handle = NULL;
    // The start block lives in its own page, freed by the thread: no allocator is shared between threads.
    Win32ThreadStart *start = VirtualAlloc(NULL, sizeof(Win32ThreadStart), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!start) return 0;
    start->fn = fn;
    start->data = data;
    HANDLE h = CreateThread(NULL, 0, win32_thread_main, start, 0, NULL);
    if (!h) {
        VirtualFree(start, 0, MEM_RELEASE);
        return 0;
    }
    thread->handle = h;
    // Background workers one step below normal: when every core is busy, the main thread (input,
    // frames) wins.
    b32 lower = background;
#if RONIN_DEV
    lower = lower && !g_platform->background_normal;
    InterlockedIncrement(&g_platform->live_threads);
#endif
    WIN32_DEV_HANDLES(1);
    if (lower) SetThreadPriority(h, THREAD_PRIORITY_BELOW_NORMAL);
    return 1;
}

#if RONIN_DEV
void os_dev_background_normal(b32 on) {
    g_platform->background_normal = on;
}

i32 os_dev_thread_priority(OsThread thread) {
    return GetThreadPriority(thread.handle);
}
#endif

b32 os_thread_join(OsThread thread, u32 timeout_ms) {
    if (!thread.handle) return 1;
    if (WaitForSingleObject(thread.handle, timeout_ms) != WAIT_OBJECT_0) return 0;
    CloseHandle(thread.handle);
    WIN32_DEV_HANDLES(-1);
#if RONIN_DEV
    InterlockedDecrement(&g_platform->live_threads);
#endif
    return 1;
}

_Static_assert(sizeof(OsLock) == sizeof(SRWLOCK), "OsLock holds an SRWLOCK");

void os_lock(OsLock *lock) {
    AcquireSRWLockExclusive((PSRWLOCK)lock);
}

void os_unlock(OsLock *lock) {
    ReleaseSRWLockExclusive((PSRWLOCK)lock);
}

b32 os_event_create(OsEvent *event) {
    event->handle = CreateEventW(NULL, FALSE, FALSE, NULL);
#if RONIN_DEV
    if (event->handle) InterlockedIncrement(&g_platform->live_events);
#endif
    if (event->handle) WIN32_DEV_HANDLES(1);
    return event->handle != NULL;
}

void os_event_set(OsEvent event) {
    if (event.handle) SetEvent(event.handle);
}

b32 os_event_wait(OsEvent event, u32 timeout_ms) {
    return event.handle && WaitForSingleObject(event.handle, timeout_ms) == WAIT_OBJECT_0;
}

void os_event_destroy(OsEvent event) {
    if (!event.handle) return;
    CloseHandle(event.handle);
    WIN32_DEV_HANDLES(-1);
#if RONIN_DEV
    InterlockedDecrement(&g_platform->live_events);
#endif
}

i32 os_atomic_get(volatile i32 *v) {
    return (i32)InterlockedOr((volatile LONG *)v, 0);
}

void os_atomic_set(volatile i32 *v, i32 x) {
    InterlockedExchange((volatile LONG *)v, x);
}

i32 os_atomic_add(volatile i32 *v, i32 x) {
    return (i32)InterlockedAdd((volatile LONG *)v, x);
}

i32 os_atomic_cas(volatile i32 *v, i32 expected, i32 x) {
    return (i32)InterlockedCompareExchange((volatile LONG *)v, x, expected);
}

void os_wake_main(void) {
    SetEvent(g_platform->wake);
}

OsFileStatus os_file_info_arena(Arena *arena, String8 path, OsFileInfo *info) {
    memset(info, 0, sizeof(*info));
    u64 mark = arena_pos(arena);
    WCHAR *path16 = (WCHAR *)str16_from_str8(arena, path).data;
    WIN32_FILE_ATTRIBUTE_DATA data;
    OsFileStatus status = OS_FILE_OK;
    if (!GetFileAttributesExW(path16, GetFileExInfoStandard, &data)) {
        status = win32_last_file_status();
    } else {
        info->exists = 1;
        info->is_dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        info->read_only = (data.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
        info->size = (i64)(((u64)data.nFileSizeHigh << 32) | data.nFileSizeLow);
        info->write_time = (u64)win32_filetime_u64(data.ftLastWriteTime);
    }
    arena_pop_to(arena, mark);
    return status;
}

String8 os_oem_to_utf8(Arena *arena, String8 text) {
    i32 n = (i32)MIN(text.len, (i64)0x3FFFFFFF);
    if (n <= 0) return str8(NULL, 0);
    i32 wn = MultiByteToWideChar(CP_OEMCP, 0, (const char *)text.data, n, NULL, 0);
    if (wn <= 0) return str8_copy(arena, text);
    u16 *w = PUSH_ARRAY(arena, u16, wn);
    u64 w_pos = arena_pos(arena) - (u64)wn * sizeof(u16);
    MultiByteToWideChar(CP_OEMCP, 0, (const char *)text.data, n, (WCHAR *)w, wn);
    String8 s = str8_from_str16(arena, w, wn);
    memmove(w, s.data, (size_t)s.len); // the UTF-8 text where the UTF-16 one was
    arena_pop_to(arena, w_pos + (u64)s.len);
    return str8((u8 *)w, s.len);
}

// ---------------------------------------------------------------------------
// Processes: any thread (platform.h). Every path closes what it opened.

// Closes one of the process handles os_process_start counted in the ledger.
static void win32_close(HANDLE *h) {
    if (*h && *h != INVALID_HANDLE_VALUE) {
        CloseHandle(*h);
        WIN32_DEV_HANDLES(-1);
    }
    *h = NULL;
}

OsFileStatus os_process_start(Arena *arena, String8 command, String8 dir, OsProcess *p) {
    memset(p, 0, sizeof(*p));
    static volatile LONG pipe_counter;
    u64 mark = arena_pos(arena);
    HANDLE job = NULL, server = NULL, client = NULL, nul = NULL, io_event = NULL;
    LPPROC_THREAD_ATTRIBUTE_LIST list = NULL;
    OsFileStatus status = OS_FILE_OK;
    PROCESS_INFORMATION pi = { 0 };
    // The command line: "<system>\cmd.exe" /s /c "<command>" (no search of PATH or the current
    // directory for cmd itself).
    WCHAR sys[MAX_PATH];
    UINT sys_len = GetSystemDirectoryW(sys, MAX_PATH);
    String16 cmd16 = str16_from_str8(arena, command);
    String16 dir16 = str16_from_str8(arena, dir);
    i64 line_cap = (i64)sys_len + cmd16.len + 32;
    if (!sys_len || sys_len >= MAX_PATH - 16 || line_cap > 32000) {
        status = sys_len ? OS_FILE_TOO_LARGE : OS_FILE_IO_ERROR;
        goto done;
    }
    WCHAR *app = PUSH_ARRAY(arena, WCHAR, sys_len + 16);
    memcpy(app, sys, sizeof(WCHAR) * sys_len);
    memcpy(app + sys_len, L"\\cmd.exe", sizeof(L"\\cmd.exe"));
    WCHAR *line = PUSH_ARRAY(arena, WCHAR, line_cap);
    i64 n = 0;
    line[n++] = '"';
    for (WCHAR *c = app; *c; c++) line[n++] = *c;
    static const WCHAR mid[] = L"\" /s /c \"";
    for (i32 i = 0; mid[i]; i++) line[n++] = mid[i];
    memcpy(line + n, cmd16.data, sizeof(WCHAR) * (size_t)cmd16.len);
    n += cmd16.len;
    line[n++] = '"';
    line[n] = 0;
    // One pipe for stdout and stderr: a named pipe, so the reader can wait on it together with the process and
    // a cancel (anonymous pipes have no overlapped reads). Local only, one instance, a unique name.
    WCHAR name[96];
    {
        u8 name8[96];
        i64 nn = fmt_buf(name8, sizeof(name8), "\\\\.\\pipe\\ronin-%u-%u-%U", (u32)GetCurrentProcessId(),
                         (u32)InterlockedIncrement(&pipe_counter), (u64)GetTickCount64());
        win32_utf8_to_wide_fixed(str8(name8, nn), (u16 *)name, ARRAY_COUNT(name));
    }
    server = CreateNamedPipeW(name, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
                              PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 65536, 0, NULL);
    if (server == INVALID_HANDLE_VALUE) {
        server = NULL;
        status = win32_last_file_status();
        goto done;
    }
    WIN32_DEV_HANDLES(1);
    SECURITY_ATTRIBUTES inherit = { sizeof(inherit), NULL, TRUE };
    client = CreateFileW(name, GENERIC_WRITE, 0, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    io_event = CreateEventW(NULL, TRUE, FALSE, NULL); // manual reset, as OVERLAPPED wants
    job = CreateJobObjectW(NULL, NULL);
    WIN32_DEV_HANDLES((client != INVALID_HANDLE_VALUE) + (nul != INVALID_HANDLE_VALUE) + (io_event != NULL) + (job != NULL));
    if (client == INVALID_HANDLE_VALUE || nul == INVALID_HANDLE_VALUE || !io_event || !job) {
        if (client == INVALID_HANDLE_VALUE) client = NULL;
        if (nul == INVALID_HANDLE_VALUE) nul = NULL;
        status = win32_last_file_status();
        goto done;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = { 0 };
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; // ronin's exit (a crash too) ends the tree
    if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        status = win32_last_file_status();
        goto done;
    }
    // Only the pipe's write end and NUL are inherited: no other handle of ronin (another build's pipe
    // included) reaches the child.
    SIZE_T list_size = 0;
    InitializeProcThreadAttributeList(NULL, 1, 0, &list_size);
    list = (LPPROC_THREAD_ATTRIBUTE_LIST)arena_push(arena, list_size, 16);
    HANDLE inherited[2] = { client, nul };
    if (!InitializeProcThreadAttributeList(list, 1, 0, &list_size)) {
        list = NULL;
        status = win32_last_file_status();
        goto done;
    }
    if (!UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), NULL, NULL)) {
        status = win32_last_file_status();
        goto done;
    }
    STARTUPINFOEXW si = { 0 };
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = nul;
    si.StartupInfo.hStdOutput = client;
    si.StartupInfo.hStdError = client;
    si.lpAttributeList = list;
    // Suspended until it is in the job, so nothing it starts can escape the job.
    DWORD flags = CREATE_NO_WINDOW | CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT;
    if (!CreateProcessW(app, line, NULL, NULL, TRUE, flags, NULL, dir.len ? (WCHAR *)dir16.data : NULL, &si.StartupInfo, &pi)) {
        DWORD error = GetLastError();
        status = error == ERROR_DIRECTORY ? OS_FILE_NOT_FOUND : win32_file_status(error);
        goto done;
    }
    WIN32_DEV_HANDLES(2); // the process and its thread
    if (!AssignProcessToJobObject(job, pi.hProcess)) {
        status = win32_last_file_status();
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 1000);
        win32_close(&pi.hProcess);
        win32_close(&pi.hThread);
        goto done;
    }
    ResumeThread(pi.hThread);
    win32_close(&pi.hThread);
    p->process = pi.hProcess;
    p->pid = pi.dwProcessId;
    p->job = job;
    p->pipe = server;
    p->io_event = io_event;
    job = server = io_event = NULL; // the process's now
#if RONIN_DEV
    InterlockedIncrement(&g_platform->live_processes);
#endif
done:
    if (list) DeleteProcThreadAttributeList(list);
    win32_close(&client); // the child holds its own copies
    win32_close(&nul);
    win32_close(&server);
    win32_close(&io_event);
    win32_close(&job);
    arena_pop_to(arena, mark);
    return status;
}

// One overlapped read of the pipe, waiting also on `others` (the process, the cancel event). Returns the
// index of what ended the wait: 0 the read (*got bytes, 0 = the pipe is broken), 1 + i others[i]; -1 error.
// Whatever ended it, no read is left in flight.
static i32 win32_process_read_wait(OsProcess *p, HANDLE *others, i32 other_count, void *dst, DWORD cap, i64 *got) {
    *got = 0;
    OVERLAPPED ov = { 0 };
    ov.hEvent = p->io_event;
    ResetEvent(p->io_event);
    DWORD n = 0;
    if (ReadFile(p->pipe, dst, cap, &n, &ov)) {
        GetOverlappedResult(p->pipe, &ov, &n, TRUE);
        *got = n;
        return 0;
    }
    DWORD error = GetLastError();
    if (error == ERROR_BROKEN_PIPE) return 0;
    if (error != ERROR_IO_PENDING) return -1;
    HANDLE handles[4] = { p->io_event };
    for (i32 i = 0; i < other_count; i++) handles[1 + i] = others[i];
    DWORD w = WaitForMultipleObjects((DWORD)(1 + other_count), handles, FALSE, INFINITE);
    i32 which = w >= WAIT_OBJECT_0 && w < WAIT_OBJECT_0 + 1 + (DWORD)other_count ? (i32)(w - WAIT_OBJECT_0) : -1;
    if (which != 0) CancelIoEx(p->pipe, &ov);
    // The read is complete, or cancelled (it may still have taken bytes before the cancel: they count).
    if (GetOverlappedResult(p->pipe, &ov, &n, TRUE)) *got = n;
    else if (GetLastError() != ERROR_BROKEN_PIPE && GetLastError() != ERROR_OPERATION_ABORTED && which == 0) return -1;
    return which;
}

OsProcessRead os_process_read(OsProcess *p, OsEvent cancel, void *dst, i64 cap, i64 *got) {
    *got = 0;
    DWORD c = (DWORD)MIN(cap, (i64)MB(64));
    if (WaitForSingleObject(cancel.handle, 0) == WAIT_OBJECT_0) {
        SetEvent(cancel.handle); // auto-reset: left set for the caller's next look
        return OS_PROCESS_CANCELLED;
    }
    while (!p->exited) {
        HANDLE others[2] = { p->process, cancel.handle };
        i32 which = win32_process_read_wait(p, others, 2, dst, c, got);
        if (which == 0) {
            if (*got) return OS_PROCESS_DATA;
            // Every writer closed the pipe: cmd.exe is ending (or closed its output); its end is the build's.
            HANDLE ends[2] = { p->process, cancel.handle };
            DWORD w = WaitForMultipleObjects(2, ends, FALSE, INFINITE);
            if (w == WAIT_OBJECT_0 + 1) {
                SetEvent(cancel.handle);
                return OS_PROCESS_CANCELLED;
            }
            p->exited = 1;
            return OS_PROCESS_EXITED;
        }
        if (which == 2) {
            SetEvent(cancel.handle);
            return OS_PROCESS_CANCELLED;
        }
        if (which < 0) return OS_PROCESS_ERROR;
        p->exited = 1; // cmd.exe ended: what is in the pipe is the rest
        if (*got) return OS_PROCESS_DATA;
    }
    // After the end: only what is already in the pipe (a process the build left running may hold it open).
    DWORD avail = 0;
    if (!PeekNamedPipe(p->pipe, NULL, 0, NULL, &avail, NULL) || !avail) return OS_PROCESS_EXITED;
    HANDLE others[1] = { cancel.handle };
    i32 which = win32_process_read_wait(p, others, 1, dst, MIN(c, avail), got);
    if (which == 1) {
        SetEvent(cancel.handle);
        return OS_PROCESS_CANCELLED;
    }
    return which == 0 && *got ? OS_PROCESS_DATA : OS_PROCESS_EXITED;
}

b32 os_process_wait(OsProcess *p, u32 timeout_ms) {
    return p->process && WaitForSingleObject(p->process, timeout_ms) == WAIT_OBJECT_0;
}

u32 os_process_exit_code(OsProcess *p) {
    DWORD code = 0;
    if (!p->process || !GetExitCodeProcess(p->process, &code)) return 0xFFFFFFFFu;
    return code;
}

void os_process_kill(OsProcess *p) {
    if (p->job) TerminateJobObject(p->job, 1);
}

void os_process_close(OsProcess *p, b32 kill_rest) {
    if (!p->process) return;
    if (!kill_rest && p->job) { // processes the build left running are let go
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits = { 0 };
        SetInformationJobObject(p->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    HANDLE job = p->job, process = p->process, pipe = p->pipe, io_event = p->io_event;
    win32_close(&job); // with kill_rest: KILL_ON_JOB_CLOSE ends what is left
    win32_close(&process);
    win32_close(&pipe);
    win32_close(&io_event);
    memset(p, 0, sizeof(*p));
#if RONIN_DEV
    InterlockedDecrement(&g_platform->live_processes);
#endif
}

#if RONIN_DEV
b32 os_dev_wake_wait(u32 timeout_ms) {
    return WaitForSingleObject(g_platform->wake, timeout_ms) == WAIT_OBJECT_0;
}

i32 os_dev_live_threads(void) {
    return (i32)InterlockedOr(&g_platform->live_threads, 0);
}

void os_dev_thread_abandoned(void) {
    InterlockedDecrement(&g_platform->live_threads); // still running, but the process exit ends it: not a leak
    WIN32_DEV_HANDLES(-1);                           // ... its handle too
}

void os_dev_thread_reclaimed(void) {
    InterlockedIncrement(&g_platform->live_threads);
    WIN32_DEV_HANDLES(1);
}

i32 os_dev_live_events(void) {
    return (i32)InterlockedOr(&g_platform->live_events, 0);
}

i32 os_dev_live_processes(void) {
    return (i32)InterlockedOr(&g_platform->live_processes, 0);
}

b32 os_dev_process_alive(u32 pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return 0; // gone (its id may be reused later, not within a test)
    b32 alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
}

void os_dev_kill_pid(u32 pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return;
    TerminateProcess(h, 1);
    WaitForSingleObject(h, 2000);
    CloseHandle(h);
}

i32 os_dev_handle_count(void) {
    DWORD n = 0;
    GetProcessHandleCount(GetCurrentProcess(), &n);
    return (i32)n;
}

i32 os_dev_live_handles(void) {
    return (i32)InterlockedOr(&g_platform->live_handles, 0);
}

// The committed bytes of one os_reserve range: its regions while they belong to that allocation.
static u64 win32_dev_reservation_committed(void *base) {
    u64 committed = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (u8 *at = (u8 *)base; VirtualQuery(at, &mbi, sizeof(mbi)) && mbi.AllocationBase == base; at += mbi.RegionSize) {
        if (mbi.State == MEM_COMMIT) committed += mbi.RegionSize;
    }
    return committed;
}

void os_dev_metrics(OsDevMetrics *m) {
    memset(m, 0, sizeof(*m));
    PROCESS_MEMORY_COUNTERS_EX pmc = { .cb = sizeof(pmc) };
    if (K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) {
        m->private_bytes = pmc.PrivateUsage;
        m->working_set = pmc.WorkingSetSize;
    }
    m->gdi_objects = (i32)GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
    m->user_objects = (i32)GetGuiResources(GetCurrentProcess(), GR_USEROBJECTS);
    m->live_handles = os_dev_live_handles();
    m->live_threads = os_dev_live_threads();
    m->live_events = os_dev_live_events();
    m->live_processes = os_dev_live_processes();
    m->watches = os_dev_watch_count();
    Win32DevReservations *r = &win32_dev_reservations;
    AcquireSRWLockExclusive(&r->lock);
    m->reservations = r->count;
    for (i32 i = 0; i < r->count; i++) m->reserved_committed += win32_dev_reservation_committed(r->base[i]);
    ReleaseSRWLockExclusive(&r->lock);
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        DWORD pid = GetCurrentProcessId();
        THREADENTRY32 te = { .dwSize = sizeof(te) };
        for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) m->process_threads += te.th32OwnerProcessID == pid;
        CloseHandle(snap);
    }
    // Last, once two reads 1 ms apart agree: right after a frame the graphics stack holds a handle or two for a few
    // microseconds (seen as +2 in about one sample in ten, never in the handle snapshot taken next); a leak stays.
    m->handles = os_dev_handle_count();
    for (i32 i = 0; i < 20; i++) {
        Sleep(1);
        i32 again = os_dev_handle_count();
        if (again == m->handles) break;
        m->handles = again;
    }
}

// ntdll's handle snapshot of this process (ProcessHandleInformation, Windows 8 and later) and each type's name
// (ObjectTypeInformation): their layouts, spelled out here as the driver kit has them.
typedef struct Win32DevHandleEntry {
    HANDLE handle;
    SIZE_T handle_count, pointer_count;
    ULONG granted_access, type_index, attributes, reserved;
} Win32DevHandleEntry;
typedef struct Win32DevHandleSnapshot {
    SIZE_T count;
    SIZE_T reserved;
    Win32DevHandleEntry entries[1];
} Win32DevHandleSnapshot;
typedef struct Win32DevTypeName {
    USHORT length, maximum_length;
    WCHAR *buffer;
} Win32DevTypeName;
typedef LONG (NTAPI *Win32DevNtQueryInformationProcess)(HANDLE, ULONG, void *, ULONG, ULONG *);
typedef LONG (NTAPI *Win32DevNtQueryObject)(HANDLE, ULONG, void *, ULONG, ULONG *);
#define WIN32_DEV_PROCESS_HANDLE_INFORMATION 51
#define WIN32_DEV_OBJECT_TYPE_INFORMATION 2

i32 os_dev_handle_types(OsDevHandleType *out, i32 cap) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    Win32DevNtQueryInformationProcess query_process =
        ntdll ? (Win32DevNtQueryInformationProcess)(void *)GetProcAddress(ntdll, "NtQueryInformationProcess") : NULL;
    Win32DevNtQueryObject query_object = ntdll ? (Win32DevNtQueryObject)(void *)GetProcAddress(ntdll, "NtQueryObject") : NULL;
    if (!query_process || !query_object) return 0;
    // Its own pages, outside the arenas, so the sample does not count itself.
    ULONG size = 1 << 16;
    Win32DevHandleSnapshot *snap = NULL;
    for (i32 attempt = 0; attempt < 8; attempt++) {
        snap = (Win32DevHandleSnapshot *)VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!snap) return 0;
        ULONG needed = 0;
        LONG status = query_process(GetCurrentProcess(), WIN32_DEV_PROCESS_HANDLE_INFORMATION, snap, size, &needed);
        if (status >= 0) break;
        VirtualFree(snap, 0, MEM_RELEASE);
        snap = NULL;
        size = MAX(size * 2, needed + 4096);
    }
    if (!snap) return 0;
    i32 types = 0;
    i32 index_of[256];
    for (i32 i = 0; i < 256; i++) index_of[i] = -1;
    for (SIZE_T i = 0; i < snap->count; i++) {
        ULONG type = snap->entries[i].type_index & 0xFF;
        if (index_of[type] < 0) {
            if (types == cap) continue;
            OsDevHandleType *t = &out[types];
            memset(t, 0, sizeof(*t));
            union { Win32DevTypeName name; u8 bytes[1024]; } info;
            ULONG got = 0;
            if (query_object(snap->entries[i].handle, WIN32_DEV_OBJECT_TYPE_INFORMATION, &info, sizeof(info), &got) >= 0 && info.name.buffer) {
                i32 n = MIN((i32)(info.name.length / 2), (i32)sizeof(t->name) - 1);
                for (i32 k = 0; k < n; k++) t->name[k] = info.name.buffer[k] < 128 ? (u8)info.name.buffer[k] : '?';
                t->name_len = n;
            } else {
                t->name_len = (i32)fmt_buf(t->name, sizeof(t->name), "type %u", (u32)type);
            }
            index_of[type] = types++;
        }
        out[index_of[type]].count++;
    }
    VirtualFree(snap, 0, MEM_RELEASE);
    return types;
}

String8 os_dev_exe_path(Arena *arena) {
    WCHAR path[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(NULL, path, ARRAY_COUNT(path));
    return n && n < ARRAY_COUNT(path) ? str8_from_str16(arena, (u16 *)path, n) : str8(NULL, 0);
}

// A directory junction (a mount point reparse point), as mklink /J makes it: no admin rights needed.
// REPARSE_DATA_BUFFER is a driver-kit type, so its mount-point layout is spelled out here.
#define RONIN_FSCTL_SET_REPARSE_POINT 0x000900A4
b32 os_dev_make_junction(String8 link, String8 target) {
    Platform *p = g_platform;
    u64 mark = arena_pos(&p->scratch);
    String16 substitute = str16_from_str8(&p->scratch, str8_fmt(&p->scratch, "\\??\\%S", target));
    String16 print = str16_from_str8(&p->scratch, target);
    WCHAR *link16 = win32_path16(link);
    b32 ok = 0;
    if (CreateDirectoryW(link16, NULL)) {
        HANDLE h = CreateFileW(link16, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
        if (h != INVALID_HANDLE_VALUE) {
            u16 sub_bytes = (u16)(substitute.len * 2), print_bytes = (u16)(print.len * 2);
            u16 path_bytes = (u16)(sub_bytes + 2 + print_bytes + 2);
            u32 total = 8 + 8 + path_bytes; // the header, the mount point fields, the two NUL-terminated names
            u8 *buf = PUSH_ARRAY(&p->scratch, u8, total);
            u32 tag = IO_REPARSE_TAG_MOUNT_POINT;
            u16 data_len = (u16)(8 + path_bytes), zero = 0, print_off = (u16)(sub_bytes + 2);
            memcpy(buf + 0, &tag, 4);
            memcpy(buf + 4, &data_len, 2);
            memcpy(buf + 6, &zero, 2);
            memcpy(buf + 8, &zero, 2);          // SubstituteNameOffset
            memcpy(buf + 10, &sub_bytes, 2);    // SubstituteNameLength
            memcpy(buf + 12, &print_off, 2);    // PrintNameOffset
            memcpy(buf + 14, &print_bytes, 2);  // PrintNameLength
            memcpy(buf + 16, substitute.data, sub_bytes + 2);
            memcpy(buf + 16 + print_off, print.data, print_bytes + 2);
            DWORD returned = 0;
            ok = DeviceIoControl(h, RONIN_FSCTL_SET_REPARSE_POINT, buf, total, NULL, 0, &returned, NULL) != 0;
            CloseHandle(h);
        }
        if (!ok) RemoveDirectoryW(link16);
    }
    arena_pop_to(&p->scratch, mark);
    return ok;
}

b32 os_dev_remove_dir(String8 path) {
    Platform *p = g_platform;
    u64 mark = arena_pos(&p->scratch);
    b32 ok = RemoveDirectoryW(win32_path16(path)) != 0;
    arena_pop_to(&p->scratch, mark);
    return ok;
}
#endif

static void win32_push_event(Platform *p, Event e);
#if RONIN_DEV
static i32 win32_dev_input_test(Platform *p); // win32_input_test.c
static i32 win32_dev_child(Platform *p, String8 script); // win32_child.c
static u64 win32_dev_hash(String8 s);
static void win32_dev_idle_check_mark(Platform *p, b32 end);
static void win32_dev_memory_report(Platform *p);
static void win32_dev_latency_key(Platform *p, i32 key);
static void win32_dev_latency_frame(Platform *p);
static void win32_dev_latency_finish(Platform *p);
static void win32_dev_capture_finish(Platform *p);
#endif

// Queues EVENT_DIR_CHANGED for every signalled watch and re-arms it; a tree watch is not re-armed (it
// stays out of the wait until os_watch_rearm).
static void win32_poll_watches(Platform *p) {
    for (i32 i = 0; i < WIN32_MAX_WATCHES; i++) {
        if (!p->watches[i] || p->watch_fired[i] || WaitForSingleObject(p->watches[i], 0) != WAIT_OBJECT_0) continue;
        if (p->watch_tree[i]) p->watch_fired[i] = 1;
        else FindNextChangeNotification(p->watches[i]);
        Event e = { .kind = EVENT_DIR_CHANGED, .watch = (OsWatch)(i + 1) };
        win32_push_event(p, e);
        p->redraw = 1;
    }
}

// The handles the main loop waits on besides the messages: the armed watches and the wake event.
static DWORD win32_wait_handles(Platform *p, HANDLE *out) {
    DWORD n = 0;
    for (i32 i = 0; i < WIN32_MAX_WATCHES; i++) if (p->watches[i] && !p->watch_fired[i]) out[n++] = p->watches[i];
    if (p->wake) out[n++] = p->wake;
    return n;
}

#if RONIN_DEV
b32 os_dev_watch_wait(OsWatch watch, u32 timeout_ms) {
    Platform *p = g_platform;
    if (watch < 1 || watch > WIN32_MAX_WATCHES || !p->watches[watch - 1]) return 0;
    if (WaitForSingleObject(p->watches[watch - 1], timeout_ms) != WAIT_OBJECT_0) return 0;
    FindNextChangeNotification(p->watches[watch - 1]);
    return 1;
}

i32 os_dev_watch_count(void) {
    i32 n = 0;
    for (i32 i = 0; i < WIN32_MAX_WATCHES; i++) n += g_platform->watches[i] != NULL;
    return n;
}

i32 os_dev_poll_watches(Event *out, i32 cap) {
    Platform *p = g_platform;
    i32 before = p->event_count;
    win32_poll_watches(p);
    i32 n = 0;
    for (i32 i = before; i < p->event_count; i++) if (n < cap) out[n++] = p->events[i];
    p->event_count = before;
    return n;
}

OsFileStatus os_dev_lock_file(String8 path, OsFile *file) {
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    HANDLE h = CreateFileW(win32_path16(path), GENERIC_READ, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    arena_pop_to(scratch, mark);
    file->handle = h == INVALID_HANDLE_VALUE ? NULL : h;
    if (file->handle) WIN32_DEV_HANDLES(1); // closed by os_file_close
    return file->handle ? OS_FILE_OK : win32_last_file_status();
}

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

#if RONIN_DEV
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

void os_set_window_title(String8 title) {
    Platform *p = g_platform;
    if (!p || !p->hwnd) return;
    u64 mark = arena_pos(&p->scratch);
    SetWindowTextW(p->hwnd, (WCHAR *)str16_from_str8(&p->scratch, title).data);
    arena_pop_to(&p->scratch, mark);
#if RONIN_DEV
    p->title_sets++;
#endif
}

// While unsaved files remain after Windows asked to end the session, the shutdown screen lists ronin
// with this reason.
static void win32_block_shutdown(Platform *p, b32 block) {
    if (block == p->block_reason || !p->hwnd) return;
    if (block) p->block_reason = ShutdownBlockReasonCreate(p->hwnd, L"Unsaved changes in ronin") != 0;
    else p->block_reason = !ShutdownBlockReasonDestroy(p->hwnd);
}

void os_set_unsaved_files(b32 any) {
    Platform *p = g_platform;
    if (!p) return;
    p->unsaved = any;
    if (!any) win32_block_shutdown(p, 0);
}

#if RONIN_DEV
b32 os_dev_unsaved_files(void) {
    return g_platform && g_platform->unsaved;
}
#endif

#if RONIN_DEV
// --startup-capture: topmost, inside the work area (nothing such as the taskbar over it), never activated.
static void win32_dev_capture_place(Platform *p) {
    MONITORINFO mi = { .cbSize = sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(p->hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    SetWindowPos(p->hwnd, HWND_TOPMOST, mi.rcWork.left + 16, mi.rcWork.top + 16, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}
#endif

// ---------------------------------------------------------------------------
// Crash recovery: the unhandled-exception filter gives every buffer to app_crash_write, each under its own guard,
// then lets the crash go on (Windows Error Reporting as usual). Only the stack, the buffers' own memory and plain
// file calls are used there: the arenas may be what broke.

// The directory: --recovery-dir (dev), build\tmp\recovery for --test and the batch modes (dev), else
// %LOCALAPPDATA%\ronin\recovery. Its parent is made too when a crash writes.
static void win32_recovery_init(Platform *p, String8 dev_dir) {
    String8 dir = { 0 };
    if (dev_dir.len) {
        dir = os_full_path(&p->perm, dev_dir);
    } else {
        WCHAR local[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
        if (n && n < MAX_PATH) dir = str8_fmt(&p->perm, "%S\\ronin\\recovery", str8_from_str16(&p->perm, (u16 *)local, n));
    }
    p->recovery_dir = dir;
    win32_utf8_to_wide_fixed(dir, p->recovery16, ARRAY_COUNT(p->recovery16));
    i64 cut = dir.len;
    while (cut > 0 && dir.data[cut - 1] != '\\') cut--;
    win32_utf8_to_wide_fixed(str8(dir.data, cut > 0 ? cut - 1 : 0), p->recovery_parent16, ARRAY_COUNT(p->recovery_parent16));
}

String8 os_recovery_dir(void) {
    return g_platform ? g_platform->recovery_dir : str8(NULL, 0);
}

#if RONIN_DEV
void os_dev_set_recovery_dir(String8 dir) {
    if (g_platform) win32_recovery_init(g_platform, dir);
}
#endif

OsFileStatus os_crash_create(String8 name, OsFile *file) {
    Platform *p = g_platform;
    file->handle = NULL;
    if (!p || !p->recovery16[0]) return OS_FILE_NO_PATH;
    CreateDirectoryW((WCHAR *)p->recovery_parent16, NULL);
    CreateDirectoryW((WCHAR *)p->recovery16, NULL);
    u16 path[2048];
    i64 n = 0;
    while (p->recovery16[n] && n < 1000) {
        path[n] = p->recovery16[n];
        n++;
    }
    path[n++] = '\\';
    win32_utf8_to_wide_fixed(name, path + n, (i64)ARRAY_COUNT(path) - n);
    HANDLE h = CreateFileW((WCHAR *)path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return win32_last_file_status();
    file->handle = h;
    return OS_FILE_OK;
}

i64 os_crash_stamp(u8 *out, i64 cap) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    return fmt_buf(out, cap, "%04d%02d%02d-%02d%02d%02d", (i32)t.wYear, (i32)t.wMonth, (i32)t.wDay, (i32)t.wHour, (i32)t.wMinute,
                   (i32)t.wSecond);
}

static LONG WINAPI win32_crash_filter(EXCEPTION_POINTERS *exception) {
    (void)exception;
    static volatile LONG entered;
    if (InterlockedExchange(&entered, 1)) return EXCEPTION_CONTINUE_SEARCH; // a crash in here: no second round
    App *app = g_platform ? g_platform->app : NULL;
    if (app) {
        i32 count = 0;
        __try {
            count = app_crash_buffer_count(app);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            count = 0;
        }
        for (i32 i = 0; i < count; i++) {
            __try {
                app_crash_write(app, i);
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                // this buffer's memory is what broke: the others are still tried
            }
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

// Room on the stack for the filter after a stack overflow (every thread of ours).
static void win32_stack_guarantee(void) {
    ULONG size = KB(64);
    SetThreadStackGuarantee(&size);
}

void os_show_window_early(u32 rgb) {
    Platform *p = g_platform;
    if (!p || !p->hwnd || p->shown_early || p->frame_count) return;
#if RONIN_DEV
    // The smoke, benches and screenshots: never early, whatever the config. --startup-capture measures the early window
    // and --type-ahead-check the activated start with it.
    if (win32_dev_batch_mode(p) && !p->startup_capture && !p->type_ahead_check) return;
#endif
    p->early_brush = CreateSolidBrush(RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
    os_set_caption_color(rgb); // the title bar as the first frame will leave it
    BOOL no_transitions = TRUE; // no fade-in: the window appears at once, opaque, as the cloaked start shows it
    win32_dwm_attribute(p, DWMWA_TRANSITIONS_FORCEDISABLED, &no_transitions, sizeof(no_transitions));
    p->shown_early = 1;
    // Shown cloaked and painted (WM_PAINT reaches a cloaked window), then uncloaked: the first composition that shows
    // the window shows it painted.
    BOOL cloak = TRUE;
    win32_dwm_attribute(p, DWMWA_CLOAK, &cloak, sizeof(cloak));
#if RONIN_DEV
    if (p->startup_capture) { // never activated, above everything for the screen capture
        win32_dev_capture_place(p);
        ShowWindow(p->hwnd, SW_SHOWNOACTIVATE);
    } else
#endif
    if (p->no_activate) {
        ShowWindow(p->hwnd, SW_SHOWNOACTIVATE);
    } else {
        ShowWindow(p->hwnd, SW_SHOW);
        SetForegroundWindow(p->hwnd);
    }
    UpdateWindow(p->hwnd); // painted now: until the device is ready the main thread handles only sent messages
    GdiFlush();
    if (p->dwm_flush) p->dwm_flush(); // one composition with the paint before uncloaking (the main thread waits for the device anyway)
    cloak = FALSE;
    win32_dwm_attribute(p, DWMWA_CLOAK, &cloak, sizeof(cloak));
#if RONIN_DEV
    os_dev_stage("shown early, painted");
#endif
}

void os_set_caption_color(u32 rgb) {
    Platform *p = g_platform;
    if (!p || !p->hwnd) return;
    COLORREF caption = RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF);
    win32_dwm_attribute(p, DWMWA_CAPTION_COLOR, &caption, sizeof(caption)); // fails before Windows 11
}

String8 os_exe_dir(Arena *arena) {
    u16 path[MAX_PATH * 4];
    DWORD len = GetModuleFileNameW(NULL, (WCHAR *)path, ARRAY_COUNT(path));
    if (len >= ARRAY_COUNT(path)) len = 0;
    while (len > 0 && path[len - 1] != '\\') len--;
    return str8_from_str16(arena, path, len > 0 ? len - 1 : 0);
}

String8 os_get_env(Arena *arena, String8 name) {
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    WCHAR *name16 = (WCHAR *)str16_from_str8(scratch, name).data;
    String8 result = { 0 };
    DWORD cap = GetEnvironmentVariableW(name16, NULL, 0);
    if (cap > 1) {
        WCHAR *value = PUSH_ARRAY(scratch, WCHAR, cap);
        DWORD len = GetEnvironmentVariableW(name16, value, cap);
        if (len && len < cap) result = str8_from_str16(scratch, (u16 *)value, len);
    }
    if (arena != scratch) {
        String8 copy = result.len ? str8_copy(arena, result) : result;
        arena_pop_to(scratch, mark);
        result = copy;
    }
    return result;
}

b32 os_make_dir(String8 path) {
#if RONIN_DEV
    if (win32_dev_write_refused(path)) return 0;
#endif
    Arena *scratch = &g_platform->scratch;
    u64 mark = arena_pos(scratch);
    b32 ok = CreateDirectoryW(win32_path16(path), NULL) || GetLastError() == ERROR_ALREADY_EXISTS;
    arena_pop_to(scratch, mark);
    return ok;
}

void os_fatal(String8 message) {
    b32 interactive = 1;
#if RONIN_DEV
    LOG("fatal: %S", message);
    if (g_platform && (g_platform->smoke || g_platform->leak_check || g_platform->screenshot_path.len || g_platform->atlas_path.len ||
                       g_platform->bench_text || g_platform->bench_buffer || g_platform->bench_view ||
                       g_platform->bench_edit || g_platform->bench_complete || g_platform->bench_syntax || g_platform->bench_search || g_platform->bench_windows ||
                       g_platform->bench_project || g_platform->bench_grep || g_platform->bench_compile || g_platform->bench_cursors)) interactive = 0;
#endif
    if (g_platform && g_platform->startup_ms) interactive = 0;
    if (interactive) {
        u16 text[1024];
        win32_utf8_to_wide_fixed(message, text, ARRAY_COUNT(text));
        MessageBoxW(NULL, (WCHAR *)text, L"ronin", MB_OK | MB_ICONERROR);
    }
    ExitProcess(1);
}

#if RONIN_DEV
void os_log_write(String8 text) {
    u16 wide[4096];
    win32_utf8_to_wide_fixed(text, wide, ARRAY_COUNT(wide));
    OutputDebugStringW((WCHAR *)wide);
    if (g_platform && g_platform->log_file && g_platform->log_file != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(g_platform->log_file, text.data, (DWORD)text.len, &written, NULL);
    }
}

void os_dev_stage(const char *what) {
    Platform *p = g_platform;
    if (!p || p->stages_logged || p->stage_count == ARRAY_COUNT(p->stage_what)) return;
    QueryPerformanceCounter(&p->stage_qpc[p->stage_count]);
    p->stage_what[p->stage_count++] = what;
}

// The startup timeline: each stage in ms since the process was created, and its own share.
static void win32_dev_log_stages(Platform *p) {
    FILETIME created, exited, kernel, user, now_ft;
    LARGE_INTEGER now, freq;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    GetSystemTimePreciseAsFileTime(&now_ft);
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    // The process creation time on the counter's scale.
    i64 since_created = (win32_filetime_u64(now_ft) - win32_filetime_u64(created)) / 10;
    i64 created_qpc_us = now.QuadPart * 1000000 / freq.QuadPart - since_created;
    i64 prev = 0;
    for (i32 i = 0; i < p->stage_count; i++) {
        i64 t = p->stage_qpc[i].QuadPart * 1000000 / freq.QuadPart - created_qpc_us;
        LOG("startup: %5D.%03D ms  +%5D us  %s", t / 1000, t % 1000, t - prev, p->stage_what[i]);
        prev = t;
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

// Every pump goes through this instead of TranslateMessage. While no window has the keyboard focus (in the
// middle of an activation; keys typed ahead at startup can arrive then), Windows posts plain keys to the
// active window as WM_SYSKEYDOWN / WM_SYSKEYUP with the Alt context bit clear: they would become Alt chords
// (M-a, M-d ...) and TranslateMessage would make WM_SYSCHAR instead of WM_CHAR. F10 is the one key that is
// a WM_SYSKEY* without Alt by design.
static void win32_translate(MSG *msg) {
    if ((msg->message == WM_SYSKEYDOWN || msg->message == WM_SYSKEYUP) && !(HIWORD(msg->lParam) & KF_ALTDOWN) &&
        msg->wParam != VK_F10) {
        msg->message = msg->message == WM_SYSKEYDOWN ? WM_KEYDOWN : WM_KEYUP;
    }
    TranslateMessage(msg);
}

static f32 win32_dpi_scale(Platform *p) {
    return p->forced_scale > 0 ? p->forced_scale : (f32)p->dpi / 96.0f;
}

static FrameInput win32_frame_input(Platform *p) {
    FrameInput input = { p->events, p->event_count, p->width, p->height, win32_dpi_scale(p), &p->scratch };
    return input;
}

// DwmSetWindowAttribute through dwmapi.dll loaded on first use (not an import: the loader maps nothing for it
// before WinMain). Nothing happens without it.
static void win32_dwm_attribute(Platform *p, DWORD attribute, const void *value, DWORD size) {
    if (!p->dwm_set_attribute) {
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll"); // kept until the process ends
        p->dwm_set_attribute = dwm ? (HRESULT (WINAPI *)(HWND, DWORD, LPCVOID, DWORD))(void *)GetProcAddress(dwm, "DwmSetWindowAttribute") : NULL;
        p->dwm_flush = dwm ? (HRESULT (WINAPI *)(void))(void *)GetProcAddress(dwm, "DwmFlush") : NULL;
        if (!p->dwm_set_attribute) return;
    }
    p->dwm_set_attribute(p->hwnd, attribute, value, size);
}

static void win32_frame(Platform *p) {
    if (p->in_frame || !p->renderer || !p->app) return;
    p->in_frame = 1;

#if RONIN_DEV
    if (p->frame_count == 0) os_dev_stage("first frame: start");
#endif
    FrameInput input = win32_frame_input(p);
    if (!app_update_and_render(p->app, &input, p->renderer)) p->quit = 1;
#if RONIN_DEV
    win32_dev_latency_frame(p);
#endif
    p->event_count = 0;
    p->redraw = r_wants_redraw(p->renderer) || app_wants_frame(p->app);
    arena_reset(&p->scratch);
    p->frame_count++;

    if (p->frame_count == 1) {
#if RONIN_DEV
        QueryPerformanceCounter(&p->first_present);
#endif
        FILETIME created, exited, kernel, user, now_ft;
        GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
        GetSystemTimePreciseAsFileTime(&now_ft);
        i64 us_process = (win32_filetime_u64(now_ft) - win32_filetime_u64(created)) / 10;
        if (p->startup_ms) {
            p->exit_code = (i32)((us_process + 500) / 1000);
            p->quit = 1;
        }
#if RONIN_DEV
        LARGE_INTEGER now, freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        i64 us_main = (now.QuadPart - p->qpc_start.QuadPart) * 1000000 / freq.QuadPart;
        LOG("memory: private bytes after the first frame: %U KB", os_dev_private_bytes() / 1024);
        LOG("startup: first Present %D.%03D ms after WinMain, %D.%03D ms after process creation",
            us_main / 1000, us_main % 1000, us_process / 1000, us_process % 1000);
#endif
    }
    p->in_frame = 0;
}

static void win32_push_event(Platform *p, Event e) {
#if RONIN_DEV
    if (e.kind == EVENT_KEY_DOWN || e.kind == EVENT_TEXT) {
        e.dev_seq = ++p->key_seq;
        p->last_key_event = e;
    }
#endif
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

// While AltGr is held Windows reports Ctrl and Alt as down; those are not modifiers then, but Left
// Alt still counts as ALT and Right Ctrl as CTRL, so a chord such as M-{ works where "{" needs AltGr.
static u32 win32_mods(Platform *p) {
    u32 mods = 0;
    if (GetKeyState(VK_SHIFT) & 0x8000) mods |= KEYMOD_SHIFT;
    if (p->altgr) {
        mods |= KEYMOD_ALTGR;
        if (GetKeyState(VK_RCONTROL) & 0x8000) mods |= KEYMOD_CTRL;
        if (GetKeyState(VK_LMENU) & 0x8000) mods |= KEYMOD_ALT;
    } else {
        if (GetKeyState(VK_CONTROL) & 0x8000) mods |= KEYMOD_CTRL;
        if (GetKeyState(VK_MENU) & 0x8000) mods |= KEYMOD_ALT;
    }
    return mods;
}

// The character a key produces with the current Shift / AltGr (and Caps Lock) state, Ctrl and
// Alt ignored. Flag 0x4 leaves the kernel's dead-key state untouched, so typing is not disturbed.
// A dead key yields its spacing accent. 0 when the key produces nothing printable. `ret` (if not
// NULL) gets ToUnicodeEx's result: the UTF-16 units written, -1 for a dead key.
static u32 win32_key_char(Platform *p, u32 vk, u32 scancode, i32 *ret) {
    BYTE state[256];
    if (ret) *ret = 0;
    if (!GetKeyboardState(state)) return 0;
    state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_RCONTROL] = 0;
    state[VK_MENU] = state[VK_LMENU] = state[VK_RMENU] = 0;
    if (p->altgr) state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_MENU] = state[VK_RMENU] = 0x80;
    WCHAR buf[8];
    int n = ToUnicodeEx(vk, scancode, state, buf, ARRAY_COUNT(buf), 0x4, GetKeyboardLayout(0));
    if (ret) *ret = n;
    if (n == 0) return 0;
    u32 c = buf[0];
    if (c >= 0xD800 && c <= 0xDBFF && n >= 2 && buf[1] >= 0xDC00 && buf[1] <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (buf[1] - 0xDC00);
    }
    if (c < 0x20 || (c >= 0x7F && c <= 0x9F) || (c >= 0xD800 && c <= 0xDFFF)) return 0;
    return c;
}

// TranslateMessage runs before the window procedure. For a chord on a dead key (M-^ where ^ is dead)
// it left the accent pending in the thread's keyboard state, and the next key would compose with it
// (e typing ê, C-h read as C-^). The chord took the key: a pending accent is consumed here.
static void win32_drop_dead_key(void) {
    BYTE state[256] = { 0 };
    WCHAR buf[8];
    HKL hkl = GetKeyboardLayout(0);
    UINT scan = MapVirtualKeyExW(VK_SPACE, MAPVK_VK_TO_VSC, hkl);
    // Flag 0x4 only looks: a space gives itself unless an accent is pending.
    if (ToUnicodeEx(VK_SPACE, scan, state, buf, ARRAY_COUNT(buf), 0x4, hkl) == 1 && buf[0] == ' ') return;
    ToUnicodeEx(VK_SPACE, scan, state, buf, ARRAY_COUNT(buf), 0, hkl);
}

String8 os_key_name(Arena *arena, u32 scancode) {
    WCHAR name[64];
    LONG lp = (LONG)((scancode & 0xFF) << 16 | ((scancode & KF_EXTENDED) ? 1 << 24 : 0));
    int n = GetKeyNameTextW(lp, name, ARRAY_COUNT(name));
    return str8_from_str16(arena, (u16 *)name, MAX(n, 0));
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
    default:            return KEY_NONE; // modifiers, keypad digits and operators, VK_OEM_8, IME, media keys
    }
}

static b32 win32_is_modifier_vk(u32 vk) {
    return vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL ||
           vk == VK_NUMLOCK || vk == VK_SCROLL;
}

static b32 win32_is_key_message(UINT msg) {
    return msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP;
}

static LRESULT win32_handle_message(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    Platform *p = g_platform;
    switch (msg) {
    // Shutdown or logoff. With unsaved files: refuse, give the reason, and let the app ask what to save
    // (the save-buffers-kill-terminal chain); it removes the reason once nothing is unsaved or it exits.
    case WM_QUERYENDSESSION:
        if (!p->unsaved) return TRUE;
        win32_block_shutdown(p, 1);
        {
            Event e = { .kind = EVENT_END_SESSION };
            win32_push_event(p, e);
        }
        p->redraw = 1;
        return FALSE;

    case WM_ENDSESSION:
        win32_block_shutdown(p, 0);
        if (wp) { // the session ends: the process is terminated after this returns; release, write nothing
            r_shutdown(p->renderer);
            app_shutdown(p->app);
            ExitProcess(0);
        }
        return 0; // the shutdown was cancelled

    case WM_CLOSE: {
        Event e = { .kind = EVENT_CLOSE };
        win32_push_event(p, e);
        p->redraw = 1;
        return 0;
    }

    case WM_SETCURSOR: { // the app says which cursor; nothing is queued and no frame is requested
        if (LOWORD(lp) != HTCLIENT) break;
        POINT pt;
        MouseCursor c = MOUSE_CURSOR_ARROW;
        if (p->app && GetCursorPos(&pt) && ScreenToClient(hwnd, &pt)) c = app_mouse_cursor(p->app, pt.x, pt.y);
        SetCursor(p->cursors[c] ? p->cursors[c] : p->cursors[MOUSE_CURSOR_ARROW]);
        return TRUE;
    }

    case WM_ERASEBKGND:
        return 1;

#if RONIN_DEV
    case WM_APP + 1: // --idle-check: the idle period starts
    case WM_APP + 2: // ... and ends
        win32_dev_idle_check_mark(p, msg == WM_APP + 2);
        return 0;
    case WM_APP + 3: // --memory-report: the settle is over
        win32_dev_memory_report(p);
        p->quit = 1;
        return 0;
    case WM_APP + 4: // --bench-latency: a key follows
        win32_dev_latency_key(p, (i32)wp);
        return 0;
    case WM_APP + 5: // ... and the last one has been shown
        win32_dev_latency_finish(p);
        p->quit = 1;
        return 0;
    case WM_APP + 6: // --startup-capture: the screen has stood still
        win32_dev_capture_finish(p);
        p->quit = 1;
        return 0;
#endif

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        if (p->early_brush && p->frame_count == 0) FillRect(dc, &ps.rcPaint, p->early_brush); // early_window, no device yet
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
        if (p->early_brush && p->frame_count == 0) { // early_window before the device: the new size painted at once
            HDC dc = GetDC(hwnd);
            RECT client;
            GetClientRect(hwnd, &client);
            FillRect(dc, &client, p->early_brush);
            ReleaseDC(hwnd, dc);
        }
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
#if RONIN_DEV
        if (p->type_ahead_check && (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN)) {
            if (GetMessageExtraInfo() != WIN32_TYPE_AHEAD_TAG) p->dev_foreign_keys++;
            else if (msg == WM_KEYDOWN) p->dev_keydowns++;
            else p->dev_syskeydowns++;
        }
#endif
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
            // Every key but the modifiers sends its KEY_DOWN, also one ronin has no Key for (VK_OEM_8,
            // the keypad's digits and operators, media keys): a chord if it types a character, and in
            // any case the end of the previous chord's text. The keypad makes no chords (Alt + keypad
            // digits enter a character code), nor do IME and injected characters.
            if (!win32_is_modifier_vk(vk)) {
                b32 no_chord = (vk >= VK_NUMPAD0 && vk <= VK_DIVIDE) || vk == VK_PROCESSKEY || vk == VK_PACKET;
                // TranslateMessage ran first: the key's characters, if any, are already queued.
                MSG next;
                b32 dead = PeekMessageW(&next, hwnd, WM_DEADCHAR, WM_DEADCHAR, PM_NOREMOVE) ||
                           PeekMessageW(&next, hwnd, WM_SYSDEADCHAR, WM_SYSDEADCHAR, PM_NOREMOVE);
                b32 text = PeekMessageW(&next, hwnd, WM_CHAR, WM_CHAR, PM_NOREMOVE) ||
                           PeekMessageW(&next, hwnd, WM_SYSCHAR, WM_SYSCHAR, PM_NOREMOVE);
                Event e = {
                    .kind = EVENT_KEY_DOWN,
                    .key = win32_map_vk(vk),
                    .mods = win32_mods(p),
                    .scancode = (u32)(HIWORD(lp) & (KF_EXTENDED | 0xFF)),
                    .repeat = (HIWORD(lp) & KF_REPEAT) != 0,
                    .codepoint = no_chord ? 0 : win32_key_char(p, vk, (u32)(HIWORD(lp) & 0xFF), NULL),
                    .dead = dead,
                    .no_text = !dead && !text,
                };
                win32_push_event(p, e);
                p->redraw = 1;
                if ((e.mods & (KEYMOD_CTRL | KEYMOD_ALT)) && e.codepoint) win32_drop_dead_key();
            }
            // We handle WM_SYSKEYDOWN ourselves so Alt works as Meta; let only Alt+F4
            // through to DefWindowProc so it still closes the window.
            if (msg == WM_SYSKEYDOWN && vk == VK_F4 && !p->altgr) {
#if RONIN_DEV
                p->dev_menu_paths++;
#endif
                return DefWindowProcW(hwnd, msg, wp, lp);
            }
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
        if (mods & (KEYMOD_CTRL | KEYMOD_ALT)) return 0; // with AltGr: only when Left Alt / Right Ctrl are added
        Event e = { .kind = EVENT_TEXT, .codepoint = c, .mods = mods };
        win32_push_event(p, e);
        p->redraw = 1;
        return 0;
    }

    case WM_SYSCHAR: // Alt+key: handled as a key event; returning 0 avoids the beep
        return 0;

    case WM_SYSCOMMAND:
        if ((wp & 0xFFF0) == SC_KEYMENU) { // Alt / F10 never open the system menu
#if RONIN_DEV
            p->dev_keymenu++;
#endif
            return 0;
        }
        break;

    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN:
    case WM_LBUTTONUP:   case WM_RBUTTONUP:   case WM_MBUTTONUP: {
        b32 down = (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN);
        MouseButton button = (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP) ? MOUSE_LEFT
                           : (msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP) ? MOUSE_RIGHT
                           : MOUSE_MIDDLE;
        if (down) SetCapture(hwnd);
        else if (!(wp & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON))) ReleaseCapture();
        i32 x = (i16)LOWORD(lp), y = (i16)HIWORD(lp);
        if (down) {
            // A press within the double-click time and area of the previous one counts on: 1, 2, 3, 1, ...
            DWORD now = (DWORD)GetMessageTime();
            i32 dx = GetSystemMetricsForDpi(SM_CXDOUBLECLK, p->dpi) / 2, dy = GetSystemMetricsForDpi(SM_CYDOUBLECLK, p->dpi) / 2;
            b32 again = button == p->click_button && now - p->click_time <= GetDoubleClickTime() &&
                        x >= p->click_x - dx && x <= p->click_x + dx && y >= p->click_y - dy && y <= p->click_y + dy;
            p->clicks = again ? p->clicks % 3 + 1 : 1;
            p->click_button = button;
            p->click_time = now;
            p->click_x = x;
            p->click_y = y;
        }
        Event e = {
            .kind = down ? EVENT_MOUSE_DOWN : EVENT_MOUSE_UP,
            .button = button,
            .x = x,
            .y = y,
            .mods = win32_mods(p),
            .clicks = down ? p->clicks : 0,
        };
        win32_push_event(p, e);
        p->redraw = 1;
        return 0;
    }

    case WM_MOUSEMOVE: {
        // Delivered with the next frame; a move alone does not request one, except while the left
        // button is held (a drag selects).
        Event e = { .kind = EVENT_MOUSE_MOVE, .x = (i16)LOWORD(lp), .y = (i16)HIWORD(lp), .mods = win32_mods(p) };
        win32_push_event(p, e);
        if (wp & MK_LBUTTON) p->redraw = 1;
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
#if RONIN_DEV
    if (msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP || msg == WM_SYSCHAR || msg == WM_SYSDEADCHAR ||
        (msg == WM_SYSCOMMAND && (wp & 0xFFF0) == SC_KEYMENU)) p->dev_menu_paths++;
#endif
    return DefWindowProcW(hwnd, msg, wp, lp);
}

#if RONIN_DEV
// ---------------------------------------------------------------------------
// --log-keys: every keyboard message, the state ronin reads with it, and what became of it. The app
// logs, under the same event number, the chord and the command (app_dev_log_key).

static b32 win32_dev_is_keyboard_message(UINT msg) {
    return win32_is_key_message(msg) || msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_DEADCHAR || msg == WM_SYSDEADCHAR ||
           msg == WM_SYSCOMMAND || msg == WM_INPUTLANGCHANGEREQUEST || msg == WM_INPUTLANGCHANGE;
}

static const char *win32_dev_message_name(UINT msg) {
    switch (msg) {
    case WM_KEYDOWN:                return "WM_KEYDOWN";
    case WM_KEYUP:                  return "WM_KEYUP";
    case WM_SYSKEYDOWN:             return "WM_SYSKEYDOWN";
    case WM_SYSKEYUP:               return "WM_SYSKEYUP";
    case WM_CHAR:                   return "WM_CHAR";
    case WM_SYSCHAR:                return "WM_SYSCHAR";
    case WM_DEADCHAR:               return "WM_DEADCHAR";
    case WM_SYSDEADCHAR:            return "WM_SYSDEADCHAR";
    case WM_SYSCOMMAND:             return "WM_SYSCOMMAND";
    case WM_INPUTLANGCHANGEREQUEST: return "WM_INPUTLANGCHANGEREQUEST";
    case WM_INPUTLANGCHANGE:        return "WM_INPUTLANGCHANGE";
    default:                        return "?";
    }
}

// The legend of a key on a US keyboard, by scan code: keys are named by their US position.
static const char *win32_dev_us_legend(u32 scancode) {
    static const char *const legends[0x5A] = {
        [0x29] = "`", [0x02] = "1", [0x03] = "2", [0x04] = "3", [0x05] = "4", [0x06] = "5", [0x07] = "6", [0x08] = "7",
        [0x09] = "8", [0x0A] = "9", [0x0B] = "0", [0x0C] = "-", [0x0D] = "=",
        [0x10] = "Q", [0x11] = "W", [0x12] = "E", [0x13] = "R", [0x14] = "T", [0x15] = "Y", [0x16] = "U", [0x17] = "I",
        [0x18] = "O", [0x19] = "P", [0x1A] = "[", [0x1B] = "]", [0x2B] = "\\",
        [0x1E] = "A", [0x1F] = "S", [0x20] = "D", [0x21] = "F", [0x22] = "G", [0x23] = "H", [0x24] = "J", [0x25] = "K",
        [0x26] = "L", [0x27] = ";", [0x28] = "'",
        [0x2C] = "Z", [0x2D] = "X", [0x2E] = "C", [0x2F] = "V", [0x30] = "B", [0x31] = "N", [0x32] = "M", [0x33] = ",",
        [0x34] = ".", [0x35] = "/", [0x39] = "Space", [0x56] = "ISO key left of Z (not on US keyboards)",
    };
    return scancode < ARRAY_COUNT(legends) && legends[scancode] ? legends[scancode] : "?";
}

// "U+0066 'f'", or "none".
static String8 win32_dev_char(Arena *a, u32 c) {
    if (!c) return STR8_LIT("none");
    u8 bytes[4];
    i64 n = (c >= 0x20 && c != 0x7F) ? utf8_encode(c, bytes) : 0;
    return n ? str8_fmt(a, "U+%04x '%S'", c, str8(bytes, n)) : str8_fmt(a, "U+%04x", c);
}

// The keys a symbol chord of the default config needs, on layout `hkl` (VkKeyScanExW).
static void win32_dev_log_key_table(HKL hkl) {
    static const struct { u32 c; const char *chords; } table[] = {
        { '/', "C-/" }, { '_', "C-_ C-M-_" }, { '?', "C-?" }, { '<', "M-<" }, { '>', "M->" }, { '{', "M-{" }, { '}', "M-}" },
        { '%', "M-%" }, { '^', "M-^" }, { '\\', "M-\\" }, { ';', "M-; C-x C-;" }, { '+', "C-x C-+" }, { '=', "C-x C-=" },
        { '-', "C-x C--" }, { '0', "C-x C-0" }, { '@', "C-@" }, { ',', "C-c ," },
    };
    LOG("keytable: layout 0x%08X: the key that types each character a default symbol chord needs (US positions)", (u64)(uintptr_t)hkl);
    for (i32 i = 0; i < ARRAY_COUNT(table); i++) {
        SHORT r = VkKeyScanExW((WCHAR)table[i].c, hkl);
        if (r == -1) {
            LOG("keytable:   '%c' (%s): no key types it on this layout", (int)table[i].c, table[i].chords);
            continue;
        }
        u32 vk = (u32)r & 0xFF, sh = ((u32)r >> 8) & 0xFF;
        u32 scan = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl);
        BYTE state[256] = { 0 };
        if (sh & 1) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
        if (sh & 2) state[VK_CONTROL] = state[VK_LCONTROL] = 0x80;
        if (sh & 4) state[VK_MENU] = state[VK_RMENU] = 0x80;
        WCHAR buf[8];
        int n = ToUnicodeEx(vk, scan, state, buf, ARRAY_COUNT(buf), 0x4, hkl);
        LOG("keytable:   '%c' (%s): %s%s%s[%s] (vk 0x%02x, scan 0x%02x)%s", (int)table[i].c, table[i].chords,
            (sh & 6) == 6 ? "AltGr + " : (sh & 2) ? "Ctrl + " : "", (sh & 6) != 6 && (sh & 4) ? "Alt + " : "",
            (sh & 1) ? "Shift + " : "", win32_dev_us_legend(scan), vk, scan, n < 0 ? ", a DEAD key" : "");
    }
}

// One line per keyboard message, after it was handled: `seq_before` and `altgr_before` are the state
// before, `result` what the window procedure returned.
static void win32_dev_log_message(Platform *p, UINT msg, WPARAM wp, LPARAM lp, u32 seq_before, b32 altgr_before, LRESULT result) {
    Arena *a = &p->scratch;
    u64 mark = arena_pos(a);
    HKL hkl = GetKeyboardLayout(0);
    String8 outcome = { 0 };
    b32 pushed = p->key_seq != seq_before;
    if (pushed) {
        Event *e = &p->last_key_event;
        outcome = str8_fmt(a, "event #%u %s", e->dev_seq, e->kind == EVENT_KEY_DOWN ? "KEY_DOWN" : "TEXT");
    }
    if (msg == WM_INPUTLANGCHANGEREQUEST) {
        LOG("keys: %s flags 0x%x (%s%s%s) requested layout 0x%08X, current 0x%08X -> %s", win32_dev_message_name(msg), (u32)wp,
            (wp & INPUTLANGCHANGE_SYSCHARSET) ? "SYSCHARSET " : "", (wp & INPUTLANGCHANGE_FORWARD) ? "FORWARD " : "",
            (wp & INPUTLANGCHANGE_BACKWARD) ? "BACKWARD" : "", (u64)(uintptr_t)lp, (u64)(uintptr_t)hkl,
            "passed to DefWindowProc (accepted)");
        return;
    }
    if (msg == WM_INPUTLANGCHANGE) {
        WCHAR klid[KL_NAMELENGTH] = { 0 };
        GetKeyboardLayoutNameW(klid);
        LOG("keys: %s charset %u, layout 0x%08X (KLID %S)", win32_dev_message_name(msg), (u32)wp, (u64)(uintptr_t)lp,
            str8_from_str16(a, (u16 *)klid, KL_NAMELENGTH - 1));
        win32_dev_log_key_table((HKL)lp);
        arena_pop_to(a, mark);
        return;
    }
    if (msg == WM_SYSCOMMAND) {
        LOG("keys: %s 0x%x%s -> %s", win32_dev_message_name(msg), (u32)wp, (wp & 0xFFF0) == SC_KEYMENU ? " (SC_KEYMENU)" : "",
            (wp & 0xFFF0) == SC_KEYMENU ? "swallowed: no system menu" : "passed to DefWindowProc");
        return;
    }

    // The keys down as the thread's key state has them, and ronin's reading of them.
    u8 down[64];
    i64 dn = 0;
    static const struct { int vk; const char *name; } mod_keys[] = {
        { VK_LSHIFT, "LShift" }, { VK_RSHIFT, "RShift" }, { VK_LCONTROL, "LCtrl" }, { VK_RCONTROL, "RCtrl" },
        { VK_LMENU, "LAlt" }, { VK_RMENU, "RAlt" }, { VK_LWIN, "LWin" }, { VK_RWIN, "RWin" },
    };
    for (i32 i = 0; i < ARRAY_COUNT(mod_keys); i++) {
        if (GetKeyState(mod_keys[i].vk) & 0x8000) dn += fmt_buf(down + dn, (i64)sizeof(down) - dn, "%s%s", dn ? "+" : "", mod_keys[i].name);
    }
    if (!dn) dn = fmt_buf(down, sizeof(down), "none");
    u32 mods = win32_mods(p);
    u8 m[5];
    i32 mn = 0;
    if (mods & KEYMOD_CTRL) m[mn++] = 'C';
    if (mods & KEYMOD_ALT) m[mn++] = 'M';
    if (mods & KEYMOD_SHIFT) m[mn++] = 'S';
    if (mods & KEYMOD_ALTGR) m[mn++] = 'G';
    if (!mn) m[mn++] = '-';
    String8 state = str8_fmt(a, "down %S, caps %d | ronin mods %S, altgr %d%s | layout 0x%08X", str8(down, dn),
                             (GetKeyState(VK_CAPITAL) & 1) != 0, str8(m, mn), p->altgr,
                             altgr_before != p->altgr ? (p->altgr ? " (set now)" : " (cleared now)") : "", (u64)(uintptr_t)hkl);
    u32 scan = (u32)(HIWORD(lp) & 0xFF);
    WCHAR name16[64];
    int name_len = GetKeyNameTextW((LONG)lp, name16, ARRAY_COUNT(name16));
    String8 key_name = str8_from_str16(a, (u16 *)name16, MAX(name_len, 0));
    String8 bits = str8_fmt(a, "scan 0x%02x [US %s] ext %d rep %u prev %d up %d altctx %d time %u", scan, win32_dev_us_legend(scan),
                            (HIWORD(lp) & KF_EXTENDED) != 0, (u32)LOWORD(lp), (HIWORD(lp) & KF_REPEAT) != 0, (HIWORD(lp) & KF_UP) != 0,
                            (HIWORD(lp) & KF_ALTDOWN) != 0, (u32)GetMessageTime());

    if (win32_is_key_message(msg)) {
        u32 vk = (u32)wp;
        b32 up = (HIWORD(lp) & KF_UP) != 0;
        i32 ret;
        u32 c = win32_key_char(p, vk, scan, &ret);
        if (!pushed) {
            outcome = up ? STR8_LIT("key up: no event")
                    : (vk == VK_CONTROL && !(HIWORD(lp) & KF_EXTENDED) && p->altgr) ? STR8_LIT("dropped: the synthetic Left Ctrl of AltGr")
                    : win32_is_modifier_vk(vk) ? STR8_LIT("no event: a modifier")
                    : STR8_LIT("no event");
            if (msg == WM_SYSKEYDOWN && vk == VK_F4 && !p->altgr) outcome = str8_fmt(a, "%S; passed to DefWindowProc (Alt+F4 closes)", outcome);
        }
        LOG("keys: %s vk 0x%02x '%S' %S | %S | ToUnicodeEx %d -> %S | -> %S", win32_dev_message_name(msg), vk, key_name, bits, state,
            ret, win32_dev_char(a, c), outcome);
    } else {
        u32 c = (u32)wp;
        if (!pushed) {
            outcome = msg == WM_SYSCHAR ? STR8_LIT("ignored: the WM_SYSKEYDOWN was the key (returning 0 avoids the beep)")
                    : msg == WM_DEADCHAR || msg == WM_SYSDEADCHAR ? STR8_LIT("not handled (DefWindowProc): a dead key, the next character is composed")
                    : (c >= 0xD800 && c <= 0xDBFF) ? STR8_LIT("waiting: a high surrogate")
                    : (c < 0x20 || (c >= 0x7F && c <= 0x9F)) ? STR8_LIT("ignored: a control character (the KEY_DOWN is the chord)")
                    : (mods & (KEYMOD_CTRL | KEYMOD_ALT)) ? STR8_LIT("ignored: Ctrl or Alt held (the KEY_DOWN is the chord)")
                    : STR8_LIT("no event");
        }
        LOG("keys: %s %S %S | %S | -> %S", win32_dev_message_name(msg), win32_dev_char(a, c), bits, state, outcome);
    }
    (void)result;
    arena_pop_to(a, mark);
}
#endif

#if RONIN_DEV
// The tests that drive the real input path (the thread's key state, posted key messages) can be disturbed
// by a person typing in another window meanwhile: Windows may synchronize the thread's key state with the
// keyboard's. Each case first waits (bounded) until no physical modifier is down, and records the time of
// the last real input (GetLastInputInfo; posted messages and SetKeyboardState never change it). A case
// whose injected key state did not hold (or that started with a modifier still down) has tested nothing:
// it is inconclusive whatever its result, logged with the reason and run again, at most
// DEV_SHIELD_ATTEMPTS times in all, then a failure ("key state unstable"). A case whose key state held
// and whose result is wrong is a failure, never repeated, whether or not real input was seen. A case's
// failures are held back until it ends, so only a decided failure is logged as one.
#define DEV_SHIELD_ATTEMPTS 5
#define DEV_SHIELD_WAIT_MS 2000
#define DEV_SHIELD_MSGS 4

typedef struct DevShield {
    const char *what;      // the running case
    i32 attempt;
    b32 running;
    b32 held;              // a modifier was still down after the wait
    b32 lost;              // the key state the case set did not hold
    const char *lost_what; // ... which key (for the log)
    u32 input_tick;        // the last real input when the attempt started
    i32 pending;           // failures of this attempt, not yet counted
    u8 msgs[DEV_SHIELD_MSGS][384];
    i64 msg_len[DEV_SHIELD_MSGS];
    i32 inconclusive;      // inconclusive attempts so far
    // The self-test's fakes and settings (0: the real ones).
    u32 (*input_tick_fn)(void);
    b32 (*mods_down_fn)(void);
    u32 wait_ms;
    b32 quiet;
} DevShield;

static u32 win32_shield_input_tick(DevShield *s) {
    if (s->input_tick_fn) return s->input_tick_fn();
    LASTINPUTINFO li = { .cbSize = sizeof(li) };
    return GetLastInputInfo(&li) ? li.dwTime : 0;
}

static b32 win32_shield_mods_down(DevShield *s) {
    if (s->mods_down_fn) return s->mods_down_fn();
    static const int vks[] = { VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN };
    for (i32 i = 0; i < ARRAY_COUNT(vks); i++) if (GetAsyncKeyState(vks[i]) & 0x8000) return 1;
    return 0;
}

static void win32_shield_case(DevShield *s, const char *what) {
    s->what = what;
    s->attempt = 0;
    s->running = 0;
}

// The loop condition of DEV_SHIELD_CASE: true when the case is to run (again).
static b32 win32_shield_again(DevShield *s, i32 *failures) {
    if (s->running) {
        s->running = 0;
        if (!s->held && !s->lost) { // the case ran with its own key state: its result counts
            for (i32 i = 0; i < MIN(s->pending, DEV_SHIELD_MSGS); i++) {
                if (!s->quiet) LOG("test: FAIL: %S", str8(s->msgs[i], s->msg_len[i]));
            }
            *failures += s->pending;
            return 0;
        }
        s->inconclusive++;
        if (!s->quiet) {
            u32 now_tick = win32_shield_input_tick(s);
            u32 ago = s->input_tick_fn ? 0 : GetTickCount() - now_tick;
            u8 seen[96];
            i64 seen_len = now_tick != s->input_tick ? fmt_buf(seen, sizeof(seen), "real input during the case")
                         : !s->input_tick_fn && ago < 5000 ? fmt_buf(seen, sizeof(seen), "real input %u ms before", ago)
                                                           : fmt_buf(seen, sizeof(seen), "no real input seen");
            LOG("test: inconclusive: %s: %s%s%s (%S); it %s, repeated (attempt %d of %d)%s%S", s->what,
                s->held ? "a modifier was held down" : "the key state it set did not hold", s->held || !s->lost_what ? "" : ": ",
                s->held || !s->lost_what ? "" : s->lost_what, str8(seen, seen_len), s->pending ? "failed" : "passed", s->attempt + 1,
                DEV_SHIELD_ATTEMPTS, s->pending ? ": " : "", s->pending ? str8(s->msgs[0], s->msg_len[0]) : str8(NULL, 0));
        }
        if (++s->attempt >= DEV_SHIELD_ATTEMPTS) {
            if (!s->quiet) LOG("test: FAIL: %s: key state unstable (inconclusive %d times)", s->what, DEV_SHIELD_ATTEMPTS);
            (*failures)++;
            return 0;
        }
    }
    u32 waited = 0, bound = s->wait_ms ? s->wait_ms : DEV_SHIELD_WAIT_MS;
    while (win32_shield_mods_down(s) && waited < bound) {
        Sleep(10); // test code only: waits for a person to let go of a modifier
        waited += 10;
    }
    s->held = win32_shield_mods_down(s);
    s->lost = 0;
    s->lost_what = NULL;
    s->input_tick = win32_shield_input_tick(s);
    s->pending = 0;
    s->running = 1;
    return 1;
}

// The key state a case set (SetKeyboardState) does not read back. Windows replaces a thread's key state
// with the keyboard's some time after real input anywhere (typing in another window), also input that
// came just before the case began; measured: 16 to 625 ms after the last input, with none during the
// case. Either way the case never ran with its state: inconclusive (the log says whether real input was
// seen). `what` names the key that did not hold.
static void win32_shield_state_lost(DevShield *s, const char *what) {
    s->lost = 1;
    if (!s->lost_what) s->lost_what = what;
}

// A failure inside a case: held until the case ends.
static void win32_shield_fail(DevShield *s, String8 text) {
    if (s->pending < DEV_SHIELD_MSGS) {
        s->msg_len[s->pending] = MIN(text.len, (i64)sizeof(s->msgs[0]));
        memcpy(s->msgs[s->pending], text.data, (size_t)s->msg_len[s->pending]);
    }
    s->pending++;
}

// for (each attempt) { the case }: no break inside the body.
#define DEV_SHIELD_CASE(s, what, failures) for (win32_shield_case((s), (what)); win32_shield_again((s), (failures));)

// --test: the shield's decisions, with a fake input clock and fake modifiers.
static u32 g_shield_fake_tick;
static i32 g_shield_fake_held; // calls left that report a modifier down
static u32 win32_dev_fake_tick(void) { return g_shield_fake_tick; }
static b32 win32_dev_fake_mods(void) {
    if (g_shield_fake_held > 0) {
        g_shield_fake_held--;
        return 1;
    }
    return 0;
}

static i32 win32_dev_test_shield(void) {
    DevShield s = { .input_tick_fn = win32_dev_fake_tick, .mods_down_fn = win32_dev_fake_mods, .wait_ms = 20, .quiet = 1 };
    i32 failures = 0, runs = 0;
    String8 msg = STR8_LIT("the case failed");
    // A pass: one run, nothing counted.
    DEV_SHIELD_CASE(&s, "pass", &failures) runs++;
    b32 ok = runs == 1 && failures == 0 && s.inconclusive == 0;
    // A failure with no real input: one run, one failure.
    runs = 0;
    DEV_SHIELD_CASE(&s, "fail", &failures) {
        runs++;
        win32_shield_fail(&s, msg);
    }
    ok = ok && runs == 1 && failures == 1 && s.inconclusive == 0;
    // Real input during the case but its key state held: a failure, not repeated.
    runs = failures = 0;
    DEV_SHIELD_CASE(&s, "input, state held, fail", &failures) {
        runs++;
        g_shield_fake_tick++;
        win32_shield_fail(&s, msg);
    }
    ok = ok && runs == 1 && failures == 1 && s.inconclusive == 0;
    // The key state did not hold, no real input seen, the case failed: inconclusive, repeated, then a pass.
    runs = failures = 0;
    DEV_SHIELD_CASE(&s, "lost, failed", &failures) {
        if (runs++ == 0) {
            win32_shield_state_lost(&s, "Ctrl");
            win32_shield_fail(&s, msg);
        }
    }
    ok = ok && runs == 2 && failures == 0 && s.inconclusive == 1;
    // The key state did not hold and the case passed: it tested nothing, so it is repeated too.
    runs = failures = 0;
    s.inconclusive = 0;
    DEV_SHIELD_CASE(&s, "lost, passed", &failures) {
        if (runs++ == 0) win32_shield_state_lost(&s, "Alt");
    }
    ok = ok && runs == 2 && failures == 0 && s.inconclusive == 1;
    // The key state never holds: 5 runs, then one failure (key state unstable), passing or not.
    runs = failures = 0;
    s.inconclusive = 0;
    DEV_SHIELD_CASE(&s, "always lost", &failures) {
        runs++;
        win32_shield_state_lost(&s, "Shift");
        if (runs & 1) win32_shield_fail(&s, msg);
    }
    ok = ok && runs == DEV_SHIELD_ATTEMPTS && failures == 1 && s.inconclusive == DEV_SHIELD_ATTEMPTS;
    // A modifier held past the bound: the attempt is inconclusive; once it is released the case passes.
    runs = failures = 0;
    s.inconclusive = 0;
    g_shield_fake_held = 1000;
    DEV_SHIELD_CASE(&s, "held", &failures) {
        if (runs++ == 0) {
            g_shield_fake_held = 0;
            win32_shield_fail(&s, msg);
        }
    }
    ok = ok && runs == 2 && failures == 0 && s.inconclusive == 1;
    // A held modifier released during the wait: it waited, then ran once.
    runs = 0;
    g_shield_fake_held = 3;
    DEV_SHIELD_CASE(&s, "released while waiting", &failures) runs++;
    ok = ok && runs == 1 && failures == 0 && g_shield_fake_held == 0;
    if (ok) {
        LOG("test: ok: input shield (a pass; a failure; real input with the key state held: a failure, not repeated; a key "
            "state that did not hold, failed or passed: inconclusive, repeated; never holding: key state unstable after %d "
            "attempts; a modifier held past the wait: inconclusive; released while waiting: one run)", DEV_SHIELD_ATTEMPTS);
    } else {
        LOG("test: FAIL: input shield: the decisions differ from the table");
    }
    return ok ? 0 : 1;
}

// --test, the Win32 side: modifier keys set in this thread's key state as Windows would have them,
// read by win32_mods and made into a chord. The state is restored afterwards.
static i32 win32_dev_test_mods(Platform *p) {
    static const struct { int vks[4]; u32 codepoint; KeyChord want; const char *what; } cases[] = {
        { { VK_MENU, VK_LMENU }, 'f', 'f' | CHORD_META, "Left Alt + f -> M-f" },
        { { VK_MENU, VK_RMENU }, 'f', 'f' | CHORD_META, "Right Alt + f (no AltGr) -> M-f" },
        { { VK_CONTROL, VK_LCONTROL }, 'f', 'f' | CHORD_CTRL, "Left Ctrl + f -> C-f" },
        { { VK_CONTROL, VK_RCONTROL, VK_MENU, VK_LMENU }, 'f', 'f' | CHORD_CTRL | CHORD_META, "Ctrl + Alt + f -> C-M-f" },
        { { VK_SHIFT, VK_LSHIFT, VK_MENU, VK_LMENU }, '<', '<' | CHORD_META, "Alt + Shift + , -> M-<" },
        { { VK_SHIFT, VK_LSHIFT, VK_CONTROL, VK_LCONTROL }, '?', '?' | CHORD_CTRL, "Ctrl + Shift + / -> C-?" },
    };
    BYTE saved[256];
    GetKeyboardState(saved);
    b32 altgr = p->altgr;
    p->altgr = 0;
    i32 failures = 0;
    DevShield shield = { 0 };
    for (i32 i = 0; i < ARRAY_COUNT(cases); i++) {
        DEV_SHIELD_CASE(&shield, cases[i].what, &failures) {
            BYTE state[256] = { 0 };
            for (i32 k = 0; k < 4 && cases[i].vks[k]; k++) state[cases[i].vks[k]] = 0x80;
            SetKeyboardState(state);
            KeyChord chord = 0;
            b32 made = key_chord_from_event(KEY_NONE, cases[i].codepoint, win32_mods(p), &chord);
            b32 held = 1;
            for (i32 k = 0; k < 4 && cases[i].vks[k]; k++) held &= (GetKeyState(cases[i].vks[k]) & 0x8000) != 0;
            if (!held) win32_shield_state_lost(&shield, "a modifier of the case");
            if (!made || chord != cases[i].want) {
                u8 got[32], want[32], text[256];
                i64 gn = made ? key_chord_print(chord, got, sizeof(got)) : fmt_buf(got, sizeof(got), "no chord");
                i64 wn = key_chord_print(cases[i].want, want, sizeof(want));
                BYTE now[256];
                GetKeyboardState(now);
                LASTINPUTINFO li = { .cbSize = sizeof(li) };
                GetLastInputInfo(&li);
                i64 n = fmt_buf(text, sizeof(text), "win32 modifiers: %s: got %S, expected %S (the key state reads back: Menu %x LMenu %x Ctrl %x "
                                "Shift %x; GetKeyState(VK_MENU) %x; altgr %d; the last real input %u ms before)", cases[i].what, str8(got, gn),
                                str8(want, wn), (u32)now[VK_MENU], (u32)now[VK_LMENU], (u32)now[VK_CONTROL], (u32)now[VK_SHIFT],
                                (u32)(u16)GetKeyState(VK_MENU), p->altgr, (u32)(GetTickCount() - li.dwTime));
                win32_shield_fail(&shield, str8(text, n));
            }
        }
    }
    SetKeyboardState(saved);
    p->altgr = altgr;
    if (!failures) {
        LOG("test: ok: win32 modifiers (Alt, Right Alt without AltGr, Ctrl, Ctrl+Alt, Alt+Shift, Ctrl+Shift from the thread's key "
            "state; %d inconclusive attempt(s))", shield.inconclusive);
    }
    return failures;
}

// --test: messages handed to the window procedure as the message loop would (TranslateMessage's
// characters included), the events through a headless app. A key ronin has no Key for, typed right
// after a chord: its text arrives.
static i32 win32_dev_test_keys(Platform *p) {
    Arena arena = arena_create(MB(256));
    AppArgs args = { .dpi_scale = 1.0f, .headless = 1 };
    App *app = app_create(&arena, &args);
    if (!app) {
        LOG("test: FAIL: win32 keys: app_create failed");
        return 1;
    }
    app_dev_feed_events(app, NULL, 0, &arena);
    BYTE saved[256], state[256] = { 0 };
    GetKeyboardState(saved);
    DevShield shield = { 0 };
    i32 failures = 0;
    DEV_SHIELD_CASE(&shield, "win32 keys: keypad 4 right after C-l", &failures) {
        app_dev_show_scratch(app, STR8_LIT(""));
        i32 count = p->event_count;
        memset(state, 0, sizeof(state));
        state[VK_CONTROL] = state[VK_LCONTROL] = 0x80; // C-l: a chord
        SetKeyboardState(state);
        if (!(GetKeyState(VK_CONTROL) & 0x8000)) win32_shield_state_lost(&shield, "Ctrl");
        win32_handle_message(NULL, WM_KEYDOWN, 'L', (LPARAM)(1 | 0x26 << 16));
        win32_handle_message(NULL, WM_CHAR, 0x0C, (LPARAM)(1 | 0x26 << 16));
        memset(state, 0, sizeof(state));
        state[VK_NUMLOCK] = 1; // keypad 4 with NumLock on
        SetKeyboardState(state);
        win32_handle_message(NULL, WM_KEYDOWN, VK_NUMPAD4, (LPARAM)(1 | 0x4B << 16));
        win32_handle_message(NULL, WM_CHAR, '4', (LPARAM)(1 | 0x4B << 16));
        SetKeyboardState(saved);
        app_dev_feed_events(app, p->events + count, p->event_count - count, &arena);
        p->event_count = count;
        i64 size = app_dev_size(app);
        if (size != 1) {
            u8 text[128];
            i64 n = fmt_buf(text, sizeof(text), "win32 keys: keypad 4 right after C-l: the buffer has %D byte(s), expected 1", size);
            win32_shield_fail(&shield, str8(text, n));
        }
    }
    i32 leaks = app_shutdown(app);
    if (leaks) LOG("test: FAIL: win32 keys: %d leak(s)", leaks);
    b32 ok = !failures && leaks == 0;
    if (ok) LOG("test: ok: win32 keys (keypad 4 right after C-l types its 4)");
    return ok ? 0 : 1;
}

// --log-keys at startup: the layouts loaded in this session, the active one and its key table.
static void win32_dev_log_layouts(Platform *p) {
    HKL list[32];
    i32 n = GetKeyboardLayoutList(ARRAY_COUNT(list), list);
    WCHAR klid[KL_NAMELENGTH] = { 0 };
    GetKeyboardLayoutNameW(klid);
    HKL active = GetKeyboardLayout(0);
    LOG("keys: layout of this thread 0x%08X (KLID %S); %d layout(s) loaded in the session:", (u64)(uintptr_t)active,
        str8_from_str16(&p->scratch, (u16 *)klid, KL_NAMELENGTH - 1), n);
    for (i32 i = 0; i < n; i++) LOG("keys:   0x%08X%s", (u64)(uintptr_t)list[i], list[i] == active ? " (active)" : "");
    win32_dev_log_key_table(active);
    for (i32 i = 0; i < n; i++) if (list[i] != active) win32_dev_log_key_table(list[i]);
    arena_reset(&p->scratch);
}
#endif

static LRESULT CALLBACK win32_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
#if RONIN_DEV
    Platform *p = g_platform;
    if (p->idle_measuring && msg != WM_APP + 2) {
        if (msg == WM_MOUSEMOVE || msg == WM_SETCURSOR || msg == WM_NCHITTEST || msg == WM_NCMOUSEMOVE || msg == WM_MOUSELEAVE) {
            p->idle_mouse_msgs++;
        } else {
            if (p->idle_other_msgs < ARRAY_COUNT(p->idle_other)) p->idle_other[p->idle_other_msgs] = msg;
            p->idle_other_msgs++;
        }
    }
    if (p->log_keys && win32_dev_is_keyboard_message(msg)) {
        u32 seq_before = p->key_seq;
        b32 altgr_before = p->altgr;
        LRESULT result = win32_handle_message(hwnd, msg, wp, lp);
        win32_dev_log_message(p, msg, wp, lp, seq_before, altgr_before, result);
        return result;
    }
#endif
    return win32_handle_message(hwnd, msg, wp, lp);
}

// Exit codes. Most are dev-only (smoke, tests); EXIT_USAGE applies to every build.
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
    EXIT_USAGE = 10, // an unrecognized "--" argument
};

// ---------------------------------------------------------------------------
// Dev tooling

#if RONIN_DEV
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
    case DEV_PROBE_REGION_HAS:
        for (i32 y = pr->y0; y < y1; y++) {
            for (i32 x = pr->x0; x < x1; x++) {
                if (win32_pixel_rgb(pixels, w, x, y) == pr->rgb) {
                    LOG("smoke: ok: '%s': (%d, %d) is #%06x", pr->what, x, y, pr->rgb);
                    return 1;
                }
            }
        }
        LOG("smoke: FAIL: '%s': no pixel of (%d,%d)-(%d,%d) is #%06x", pr->what, pr->x0, pr->y0, x1, y1, pr->rgb);
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

// Called right after the captured frame. `stage`: -1 a highlighted buffer with the rendering checks,
// -2 a highlighted buffer (app_dev_syntax_probes); 0 / 1 the smoke buffer with a hollow / filled
// cursor, 2 a region, 3 the M-x list, 4 an isearch (app_dev_buffer_probes).
static i32 win32_smoke_check_frame(Platform *p, i32 stage, const char *language) {
    b32 buffer_view = stage >= 0;
    i32 w, h;
    u8 *pixels = r_read_capture(p->renderer, &p->scratch, &w, &h);
    if (!pixels) {
        LOG("smoke: FAIL: back buffer capture failed");
        return EXIT_PIXEL_MISMATCH;
    }
    FrameInput input = win32_frame_input(p);
    DevProbe probes[16];
    i32 count = buffer_view ? app_dev_buffer_probes(p->app, &input, probes, ARRAY_COUNT(probes), stage)
                            : app_dev_syntax_probes(p->app, &input, probes, ARRAY_COUNT(probes), stage == -1);
    i32 expected = stage == 0 ? 13 : stage == 1 ? 3 : stage == 2 ? 6 : stage == 3 ? 6 : stage == 4 ? 6 : stage == 5 ? 1 : stage == 6 ? 10
                 : stage == 7 ? 6 : stage == 8 ? 7 : stage == 9 ? 5 : stage == 10 ? 11 : stage == -1 ? 13 : 5;
    i32 result = EXIT_OK;
    LOG("smoke: checking the %s frame%s%s", stage == 0 ? "buffer view (hollow cursor)" : stage == 1 ? "buffer view (filled cursor)"
                                         : stage == 2 ? "buffer view (region)" : stage == 3 ? "minibuffer (M-x list)"
                                         : stage == 4 ? "isearch (current match, lazy highlight)"
                                         : stage == 5 ? "isearch (failing)" : stage == 6 ? "two windows"
                                         : stage == 7 ? "project finder" : stage == 8 ? "results buffer (*search*)"
                                         : stage == 9 ? "build (*compilation*)" : stage == 10 ? "three cursors with regions"
                                         : stage == -1 ? "highlighted buffer (rendering and syntax)" : "highlighted buffer",
        language ? ": " : "", language ? language : "");
    for (i32 i = 0; i < count; i++) {
        if (!win32_check_probe(&probes[i], pixels, w, h) && result == EXIT_OK) {
            result = probes[i].kind == DEV_PROBE_CLEARTYPE ? EXIT_FONT : EXIT_PIXEL_MISMATCH;
        }
    }
    if (count < expected) {
        LOG("smoke: FAIL: expected %d probes, the app produced %d", expected, count);
        if (result == EXIT_OK) result = EXIT_PIXEL_MISMATCH;
    }
    if (stage != -1) {
        arena_reset(&p->scratch);
        return result;
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

// The scripted session of the smoke: the keys and the bytes saved.
#define SMOKE_SESSION_KEYS "i n t SPC f ( ) SPC { RET a ( ) ; RET b ( ) ; RET } RET " \
                           "M-< C-n C-SPC C-n C-w C-n C-y C-/ C-? M-> x x C-/ M-< C-x C-; C-x C-s"
#define SMOKE_SESSION_TEXT "// int f() {\n    b();\n    a();\n}\n"

// Visits `path` (written with `initial` first), types `keys` through the --keys path, then
// compares the saved bytes with `expected`.
static b32 win32_smoke_keys(Platform *p, String8 path, String8 initial, String8 keys, String8 expected) {
    if (!os_write_file(path, initial) || !app_dev_visit(p->app, path)) {
        LOG("smoke: FAIL: cannot set up %S", path);
        return 0;
    }
    Event events[256];
    i32 n = app_dev_key_events(p->app, keys, events, ARRAY_COUNT(events));
    for (i32 i = 0; i < n; i++) win32_push_event(p, events[i]);
    win32_frame(p);
    OsFile f;
    OsFileInfo info;
    u8 bytes[256];
    i64 len = -1;
    if (os_file_open_read(path, &f, &info) == OS_FILE_OK) {
        len = MIN(info.size, (i64)sizeof(bytes));
        if (os_file_read(f, bytes, len) != OS_FILE_OK) len = -1;
        os_file_close(f);
    }
    b32 ok = len >= 0 && str8_equal(str8(bytes, len), expected);
    LOG("smoke: %s: --keys \"%S\" saved %D bytes:\n%S\n(expected:\n%S)", ok ? "ok" : "FAIL", keys, len, str8(bytes, MAX(len, 0)), expected);
    return ok;
}

static void win32_smoke_feed_no_frame(Platform *p, const char *keys) {
    Event events[64];
    i32 n = app_dev_key_events(p->app, str8_cstr(keys), events, ARRAY_COUNT(events));
    for (i32 i = 0; i < n; i++) win32_push_event(p, events[i]);
}

static void win32_smoke_feed(Platform *p, const char *keys) {
    win32_smoke_feed_no_frame(p, keys);
    win32_frame(p);
}

static b32 win32_smoke_has_reason(Platform *p) {
    WCHAR reason[128];
    DWORD len = ARRAY_COUNT(reason);
    return ShutdownBlockReasonQuery(p->hwnd, reason, &len) != 0;
}

// Smoke: Windows' end-of-session query sent to our own window (no real shutdown). With an unsaved
// file it is refused with a block reason and the save question opens; a repeated query keeps that
// chain; an abort keeps the reason until Windows cancels; once everything is saved it is allowed.
static i32 win32_smoke_end_session(Platform *p) {
    String8 path = str8_fmt(&p->perm, "%S\\tmp\\smoke_end.txt", p->exe_dir);
    if (!os_write_file(path, STR8_LIT("end\n")) || !app_dev_visit(p->app, path)) {
        LOG("smoke: FAIL: cannot set up %S", path);
        return EXIT_TEST;
    }
    win32_smoke_feed(p, "x");
    LRESULT refused = SendMessageW(p->hwnd, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF);
    WCHAR reason_text[128];
    DWORD reason_len = ARRAY_COUNT(reason_text);
    b32 reason = ShutdownBlockReasonQuery(p->hwnd, reason_text, &reason_len) &&
                 str8_equal(str8_from_str16(&p->scratch, (u16 *)reason_text, lstrlenW(reason_text)), STR8_LIT("Unsaved changes in ronin"));
    win32_frame(p);
    String8 prompt = str8_copy(&p->perm, app_dev_prompt(p->app));
    b32 ok1 = !refused && reason && prompt.len > 10 && str8_starts_with(prompt, STR8_LIT("Save file "));
    SendMessageW(p->hwnd, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF);
    win32_frame(p);
    b32 ok2 = str8_equal(app_dev_prompt(p->app), prompt);
    win32_smoke_feed(p, "C-g");
    b32 ok3 = !app_dev_prompt(p->app).len && win32_smoke_has_reason(p);
    SendMessageW(p->hwnd, WM_ENDSESSION, FALSE, ENDSESSION_LOGOFF); // the user cancelled the shutdown
    b32 ok4 = !win32_smoke_has_reason(p);
    win32_smoke_feed(p, "C-x C-s");
    LRESULT allowed = SendMessageW(p->hwnd, WM_QUERYENDSESSION, 0, ENDSESSION_LOGOFF);
    b32 ok5 = allowed && !win32_smoke_has_reason(p) && !app_dev_prompt(p->app).len;
    b32 ok = ok1 && ok2 && ok3 && ok4 && ok5;
    LOG("smoke: %s: end of session: refused with the reason 'Unsaved changes in ronin' and the save question '%S' %d, a repeated query keeps it %d, "
        "an abort keeps the reason %d, a cancelled shutdown removes it %d, allowed once saved %d", ok ? "ok" : "FAIL", prompt,
        ok1, ok2, ok3, ok4, ok5);
    return ok ? EXIT_OK : EXIT_TEST;
}

// Smoke, after the probe frames: the font was set up once; a config with another background is
// loaded (as C-c r) and the pixel probed; a file is edited and saved through --keys input and its
// bytes compared. The color stage is the last frame's capture.
static i32 win32_smoke_config_and_keys(Platform *p) {
    i32 result = EXIT_OK;
    i32 setups = app_dev_font_setups(p->app);
    LOG("smoke: %s: font set up %d time(s) at startup, expected 1", setups == 1 ? "ok" : "FAIL", setups);
    if (setups != 1) result = EXIT_FONT;

    String8 tmp = str8_fmt(&p->perm, "%S\\tmp", p->exe_dir);
    if (!win32_smoke_keys(p, str8_fmt(&p->perm, "%S\\smoke_keys.txt", tmp), STR8_LIT("abc\n"), STR8_LIT("M-> RET h i C-x C-s"),
                          STR8_LIT("abc\n\nhi")) && result == EXIT_OK) result = EXIT_TEST;
    // A scripted editing session: RET indents, a region is killed and yanked, undo, undo-redo, a
    // merged run of typing undone, comment-line.
    if (!win32_smoke_keys(p, str8_fmt(&p->perm, "%S\\smoke_session.c", tmp), STR8_LIT(""), STR8_LIT(SMOKE_SESSION_KEYS),
                          STR8_LIT(SMOKE_SESSION_TEXT)) && result == EXIT_OK) result = EXIT_TEST;

    String8 conf = str8_fmt(&p->perm, "%S\\smoke.conf", tmp);
    if (!os_write_file(conf, STR8_LIT("[colors]\nbackground = #102030\n"))) {
        LOG("smoke: FAIL: cannot write %S", conf);
        return EXIT_OUTPUT_FILE;
    }
    app_dev_use_config(p->app, conf);
    r_request_capture(p->renderer);
    win32_frame(p);
    i32 w, h;
    u8 *pixels = r_read_capture(p->renderer, &p->scratch, &w, &h);
    DevProbe probe = { .kind = DEV_PROBE_PIXEL_EQ, .x0 = w - 1, .y0 = h - 1, .rgb = 0x102030,
                       .what = "background after loading a config with background = #102030" };
    if (!pixels || !win32_check_probe(&probe, pixels, w, h)) {
        if (!pixels) LOG("smoke: FAIL: back buffer capture failed");
        if (result == EXIT_OK) result = EXIT_PIXEL_MISMATCH;
    }
    setups = app_dev_font_setups(p->app);
    LOG("smoke: %s: a color-only config change keeps the font (%d set-up(s))", setups == 1 ? "ok" : "FAIL", setups);
    if (setups != 1 && result == EXIT_OK) result = EXIT_FONT;
    arena_reset(&p->scratch);
    return result;
}

// Smoke: the text scale (C-x C-+, C-x C-0, C-x C--, Ctrl + wheel) and a config's font_size each set up
// the font again with a larger or smaller cell, and C-x C-0 / the wheel come back to the same cell.
static i32 win32_smoke_text_scale(Platform *p) {
    i32 w0, h0, w1, h1, w2, h2, w3, h3, w4, h4, w5, h5;
    i32 s0 = app_dev_font_setups(p->app);
    app_dev_cell(p->app, &w0, &h0);
    win32_smoke_feed(p, "C-x C-+");
    app_dev_cell(p->app, &w1, &h1);
    win32_smoke_feed(p, "C-x C-0");
    app_dev_cell(p->app, &w2, &h2);
    win32_smoke_feed(p, "C-x C--");
    app_dev_cell(p->app, &w3, &h3);
    Event wheel = { .kind = EVENT_MOUSE_WHEEL, .x = 100, .y = 100, .wheel = 120, .mods = KEYMOD_CTRL };
    win32_push_event(p, wheel);
    win32_frame(p);
    app_dev_cell(p->app, &w4, &h4);
    i32 s4 = app_dev_font_setups(p->app);
    String8 conf = str8_fmt(&p->perm, "%S\\tmp\\smoke_font.conf", p->exe_dir);
    b32 written = os_write_file(conf, STR8_LIT("[settings]\nfont_size = 14\n"));
    app_dev_use_config(p->app, conf);
    win32_frame(p);
    app_dev_cell(p->app, &w5, &h5);
    i32 s5 = app_dev_font_setups(p->app);
    b32 ok = h1 > h0 && w2 == w0 && h2 == h0 && h3 < h0 && w4 == w0 && h4 == h0 && s4 == s0 + 4 && written && h5 > h0 && s5 == s4 + 1;
    LOG("smoke: %s: text scale: cell %dx%d, C-x C-+ %dx%d, C-x C-0 %dx%d, C-x C-- %dx%d, Ctrl+wheel %dx%d, %d font set-up(s); "
        "font_size = 14 in the config: %dx%d, %d more set-up", ok ? "ok" : "FAIL", w0, h0, w1, h1, w2, h2, w3, h3, w4, h4, s4 - s0,
        w5, h5, s5 - s4);
    return ok ? EXIT_OK : EXIT_FONT;
}

// The title shows the buffer name and was set exactly once over all the smoke frames.
static i32 win32_smoke_check_title(Platform *p) {
    WCHAR text[256];
    i32 len = GetWindowTextW(p->hwnd, text, ARRAY_COUNT(text));
    String8 title = str8_from_str16(&p->scratch, (u16 *)text, len);
    b32 ok = str8_equal(title, STR8_LIT("*scratch* - ronin")) && p->title_sets == 1;
    LOG("smoke: %s: window title '%S', set %d time(s); expected '*scratch* - ronin', once", ok ? "ok" : "FAIL", title, p->title_sets);
    arena_reset(&p->scratch);
    return ok ? EXIT_OK : EXIT_PIXEL_MISMATCH;
}

// The window class's icons came from the resources (res\ronin.rc, drawn by tools\make_icon.c): the 16 px image drawn
// into a DIB has a stem pixel in the text color, a pixel beside the "r" in the background color and a transparent
// corner (the rounded square).
static i32 win32_smoke_check_icon(Platform *p) {
    HICON class_big = (HICON)GetClassLongPtrW(p->hwnd, GCLP_HICON), class_small = (HICON)GetClassLongPtrW(p->hwnd, GCLP_HICONSM);
    HICON icon = LoadImageW(p->instance, MAKEINTRESOURCEW(1), IMAGE_ICON, 16, 16, 0);
    u32 stem = 0, beside = 0, corner = 1;
    if (icon) {
        BITMAPINFO bi = { .bmiHeader = { .biSize = sizeof(BITMAPINFOHEADER), .biWidth = 16, .biHeight = -16, .biPlanes = 1, .biBitCount = 32 } };
        void *bits = NULL;
        HDC dc = CreateCompatibleDC(NULL);
        HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (dib && bits) {
            HGDIOBJ old = SelectObject(dc, dib);
            memset(bits, 0, 16 * 16 * 4);
            DrawIconEx(dc, 0, 0, icon, 16, 16, 0, NULL, DI_NORMAL);
            GdiFlush();
            u32 *px = bits;
            stem = px[8 * 16 + 4] & 0xFFFFFF;
            beside = px[3 * 16 + 12] & 0xFFFFFF;
            corner = px[0] & 0xFFFFFF;
            SelectObject(dc, old);
        }
        if (dib) DeleteObject(dib);
        DeleteDC(dc);
        DestroyIcon(icon);
    }
    b32 ok = class_big && class_small && stem == 0xd3b58d && beside == 0x072626 && corner == 0;
    LOG("smoke: %s: class icons %s / %s; the 16 px icon: stem #%06x (expected #d3b58d), beside #%06x (#072626), corner #%06x (none)",
        ok ? "ok" : "FAIL", class_big ? "set" : "MISSING", class_small ? "set" : "MISSING", stem, beside, corner);
    return ok ? EXIT_OK : EXIT_PIXEL_MISMATCH;
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

// --keys: the tokens' events go through the normal queue (a full queue runs a frame).
static void win32_inject_keys(Platform *p) {
    if (!p->keys.len) return;
    i64 cap = p->keys.len * 2 + 2;
    Event *events = PUSH_ARRAY(&p->perm, Event, cap);
    i32 n = app_dev_key_events(p->app, p->keys, events, (i32)cap);
    LOG("--keys: %d events", n);
    for (i32 i = 0; i < n && !p->quit; i++) win32_push_event(p, events[i]);
    p->keys = str8(NULL, 0);
    p->redraw = 1;
}

// --crash: after --keys and their frames, an access violation, Windows Error Reporting's dialog off; the filter writes
// the recovery files as in a real crash. The window is never shown.
static void win32_dev_crash(Platform *p) {
    win32_inject_keys(p);
    if (p->event_count) win32_frame(p);
    for (i32 k = 0; k < 10000 && app_wants_frame(p->app); k++) win32_frame(p);
    LOG("crash: on purpose, after the keys");
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    volatile int *nowhere = NULL;
    *nowhere = 1;
}

// --screenshot and --dump-atlas: one frame with the window hidden (after --keys), then write the files.
static i32 win32_write_outputs(Platform *p) {
    app_dev_force_focus(p->app, 1); // the cursor as it looks while typing
    win32_inject_keys(p);
    if (p->event_count) win32_frame(p);
    if (p->touch.len && !p->quit) { // a file changed outside: the app checks it when activated
        os_write_file(p->touch, STR8_LIT("changed outside ronin\n"));
        Event off = { .kind = EVENT_FOCUS, .focused = 0 }, on = { .kind = EVENT_FOCUS, .focused = 1 };
        win32_push_event(p, off);
        win32_push_event(p, on);
        win32_frame(p);
    }
    if (p->quit) return EXIT_OK;
    // The workers' results (a project scan, a search, a build's output), taken until none came for 300 ms
    // (a held scan or search, a quiet build stops there; with --compile-wait, builds are waited for up to
    // that long), with the frames that work on them; then --keys-after, and their results.
    for (i32 pass = 0; pass < 2; pass++) {
        u64 pass_start = os_time_us();
        for (;;) {
            for (i32 k = 0; k < 10000 && app_wants_frame(p->app); k++) win32_frame(p);
            if (p->compile_wait_ms && app_dev_compile_busy(p->app) && os_time_us() - pass_start < (u64)p->compile_wait_ms * 1000) {
                if (os_dev_wake_wait(50) && app_wake(p->app)) win32_frame(p);
                continue;
            }
            if (!os_dev_wake_wait(300)) break;
            if (app_wake(p->app)) win32_frame(p);
        }
        if (pass || !p->keys_after.len || p->quit) break;
        p->keys = p->keys_after;
        win32_inject_keys(p);
        if (p->event_count) win32_frame(p);
    }
    for (i32 k = 0; k < 10000 && app_wants_frame(p->app); k++) win32_frame(p); // the colors of what is shown, a ranking
    r_request_capture(p->renderer);
    win32_frame(p);
    i32 result = EXIT_OK;
    if (p->screenshot_path.len) {
        i32 w, h;
        u8 *pixels = r_read_capture(p->renderer, &p->scratch, &w, &h);
        if (!pixels || !win32_write_png(p, p->screenshot_path, pixels, w, h)) result = EXIT_OUTPUT_FILE;
    }
    if (p->atlas_path.len) {
        i32 size, rows;
        u8 *rgba = app_dev_atlas(p->app, &size, &rows);
        u8 *bgra = PUSH_ARRAY(&p->scratch, u8, (i64)size * rows * 4);
        for (i64 i = 0; i < (i64)size * rows * 4; i += 4) {
            bgra[i + 0] = rgba[i + 2];
            bgra[i + 1] = rgba[i + 1];
            bgra[i + 2] = rgba[i + 0];
            bgra[i + 3] = 255;
        }
        if (!win32_write_png(p, p->atlas_path, bgra, size, rows)) result = EXIT_OUTPUT_FILE;
    }
    arena_reset(&p->scratch);
    return result;
}

// --type-ahead-check: real input, so only letters and Shift, nothing that could confirm or trigger
// anything if another window became the foreground one. Each key is one SendInput call with all of
// its inputs (Shift down, key down, key up, Shift up), so no key is ever left held, and the window
// must still be the foreground one before each call. Runs only with its own flag.
static const struct { u8 vk; b32 shift; } win32_type_ahead_keys[] = { { 'A', 0 }, { 'B', 0 }, { 'C', 1 }, { 'D', 0 }, { 'E', 0 } };

static DWORD WINAPI win32_dev_type_ahead_thread(LPVOID param) {
    Platform *p = param;
    ULONGLONG give_up = GetTickCount64() + 3000;
    while (GetForegroundWindow() != p->hwnd) {
        if (GetTickCount64() > give_up) {
            InterlockedExchange(&p->type_ahead_state, 2);
            return 0;
        }
        Sleep(1);
    }
    for (i32 i = 0; i < ARRAY_COUNT(win32_type_ahead_keys); i++) {
        if (GetForegroundWindow() != p->hwnd) {
            InterlockedExchange(&p->type_ahead_state, 3);
            return 0;
        }
        INPUT in[4] = { 0 };
        UINT n = 0;
        WORD vk = win32_type_ahead_keys[i].vk;
        if (win32_type_ahead_keys[i].shift) {
            in[n].type = INPUT_KEYBOARD;
            in[n].ki.wVk = VK_SHIFT;
            in[n++].ki.wScan = (WORD)MapVirtualKeyW(VK_LSHIFT, MAPVK_VK_TO_VSC);
        }
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = vk;
        in[n++].ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
        in[n] = in[n - 1];
        in[n++].ki.dwFlags = KEYEVENTF_KEYUP;
        if (win32_type_ahead_keys[i].shift) {
            in[n] = in[0];
            in[n++].ki.dwFlags = KEYEVENTF_KEYUP;
        }
        GUITHREADINFO gui = { .cbSize = sizeof(gui) };
        if (GetGUIThreadInfo(p->main_thread_id, &gui) && gui.hwndFocus != p->hwnd) p->type_ahead_focus_lost++;
        for (UINT k = 0; k < n; k++) in[k].ki.dwExtraInfo = (ULONG_PTR)WIN32_TYPE_AHEAD_TAG;
        SendInput(n, in, sizeof(INPUT));
        QueryPerformanceCounter(i == 0 ? &p->type_ahead_first : &p->type_ahead_last);
        if (i == 0) p->type_ahead_last = p->type_ahead_first;
        Sleep(5); // a fast typist
    }
    InterlockedExchange(&p->type_ahead_state, 1);
    return 0;
}

// After the first frame: waits for the typing thread, lets the queued keys through, and compares
// *scratch* with what was typed. Exit 0: every key was sent before the first Present and arrived,
// in order (or the window never became the foreground one: a skip). EXIT_TEST otherwise.
static i32 win32_dev_type_ahead_finish(Platform *p) {
    WaitForSingleObject(p->type_ahead_thread, 10000);
    CloseHandle(p->type_ahead_thread);
    LONG state = p->type_ahead_state;
    if (state == 2) {
        LOG("type-ahead: skip: the window never became the foreground window (Windows refused the activation)");
        return EXIT_OK;
    }
    // The keys still queued: through the normal pump, until every one arrived or 2 s passed.
    ULONGLONG give_up = GetTickCount64() + 2000;
    while (!p->quit && p->dev_keydowns + p->dev_syskeydowns < ARRAY_COUNT(win32_type_ahead_keys) + 1 && GetTickCount64() < give_up) {
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
            win32_translate(&msg);
            DispatchMessageW(&msg);
        }
        if (p->event_count) win32_frame(p);
        Sleep(10);
    }
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) { // the last key's characters and key-up
        win32_translate(&msg);
        DispatchMessageW(&msg);
    }
    if (p->event_count) win32_frame(p);
    b32 caps = (GetKeyState(VK_CAPITAL) & 1) != 0;
    u8 want[ARRAY_COUNT(win32_type_ahead_keys)];
    for (i32 i = 0; i < ARRAY_COUNT(win32_type_ahead_keys); i++) {
        u8 c = win32_type_ahead_keys[i].vk;
        want[i] = (win32_type_ahead_keys[i].shift != caps) ? c : (u8)(c + 32);
    }
    String8 text = app_dev_text(p->app, &p->scratch);
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    f64 first_ms = (f64)(p->type_ahead_first.QuadPart - p->qpc_start.QuadPart) * 1000.0 / (f64)freq.QuadPart;
    f64 last_ms = (f64)(p->type_ahead_last.QuadPart - p->qpc_start.QuadPart) * 1000.0 / (f64)freq.QuadPart;
    f64 present_ms = (f64)(p->first_present.QuadPart - p->qpc_start.QuadPart) * 1000.0 / (f64)freq.QuadPart;
    b32 before = state == 1 && p->type_ahead_last.QuadPart < p->first_present.QuadPart;
    b32 arrived = str8_equal(text, str8(want, sizeof(want)));
    LOG("type-ahead: keys sent %d.%03d to %d.%03d ms after WinMain, first Present at %d.%03d ms: %s",
        (i32)first_ms, (i32)(first_ms * 1000) % 1000, (i32)last_ms, (i32)(last_ms * 1000) % 1000, (i32)present_ms,
        (i32)(present_ms * 1000) % 1000, state == 3 ? "the window lost the foreground while typing"
                                         : before ? "all before it" : "not all before it");
    LOG("type-ahead: *scratch* holds '%S', typed '%S'%s", text, str8(want, sizeof(want)), caps ? " (Caps Lock on)" : "");
    LOG("type-ahead: %d key(s) sent while ronin had no focus window; received %d WM_KEYDOWN, %d WM_SYSKEYDOWN of its own, "
        "%d key-down(s) of other input", p->type_ahead_focus_lost, p->dev_keydowns, p->dev_syskeydowns, p->dev_foreign_keys);
    b32 ok = before && arrived;
    if (!ok && (p->dev_foreign_keys || state == 3)) {
        LOG("type-ahead: FAIL (inconclusive: someone typed or another window took the foreground meanwhile; run it again "
            "without touching the keyboard)");
    } else {
        LOG("type-ahead: %s", ok ? "PASS" : "FAIL");
    }
    arena_reset(&p->scratch);
    return ok ? EXIT_OK : EXIT_TEST;
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
            win32_translate(&msg);
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

// Per-frame timings of the bench loops: the app's frame build (from app_dev_build_us) and,
// from the renderer, the final flush, the Present and the atlas bytes uploaded.
typedef struct BenchStat {
    u64 count, build_sum, build_max, flush_sum, present_sum, present_max, slow, upload_bytes;
} BenchStat;

#define BENCH_SLOW_US 5000 // a Present this long waited for a vertical blank

static void win32_bench_record(Platform *p, BenchStat *st) {
    RDevFrameStats fs = r_dev_take_frame_stats(p->renderer);
    u64 build = app_dev_build_us(p->app);
    st->count++;
    st->build_sum += build;
    st->build_max = MAX(st->build_max, build);
    st->flush_sum += fs.flush_us;
    st->present_sum += fs.present_us;
    st->present_max = MAX(st->present_max, fs.present_us);
    st->slow += fs.present_us >= BENCH_SLOW_US;
    st->upload_bytes += fs.upload_bytes;
}

static void win32_bench_log(Platform *p, const char *bench, const char *what, BenchStat *st) {
    u64 n = MAX(st->count, 1);
    LOG("%s: %s: %U frames, build avg %U us, worst %U us; flush avg %U us; Present avg %U us, worst %U us, %U >= 5 ms; "
        "atlas upload %U bytes; presentation: %s", bench, what, st->count, st->build_sum / n, st->build_max, st->flush_sum / n,
        st->present_sum / n, st->present_max, st->slow, st->upload_bytes, r_dev_presentation_mode(p->renderer));
}

// The refresh rate of the window's monitor: a Present that blocks on a vertical blank takes up to 1/rate.
static void win32_bench_log_display(Platform *p) {
    MONITORINFOEXW mi = { .cbSize = sizeof(mi) };
    DEVMODEW dm = { .dmSize = sizeof(dm) };
    if (GetMonitorInfoW(MonitorFromWindow(p->hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) {
        LOG("bench: display %ux%u at %u Hz (one refresh = %u us), client %dx%d", (u32)dm.dmPelsWidth,
            (u32)dm.dmPelsHeight, (u32)dm.dmDisplayFrequency, 1000000u / MAX((u32)dm.dmDisplayFrequency, 1u),
            p->width, p->height);
    }
}

static void win32_bench_pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        win32_translate(&msg);
        DispatchMessageW(&msg);
    }
}

// --bench-buffer, frame part: the real app_update_and_render with the 100 MB file at its top,
// middle and end, presented with Present(0, 0).
static void win32_bench_buffer_frames(Platform *p) {
    enum { FRAMES = 100 };
    r_dev_set_present_interval(p->renderer, 0);
    win32_bench_log_display(p);
    i64 lines = app_dev_line_count(p->app);
    i64 tops[3] = { 0, lines / 2, -1 };
    const char *names[3] = { "frame at the top", "frame at the middle", "frame at the end" };
    for (i32 k = 0; k < 3; k++) {
        app_dev_goto_line(p->app, tops[k]);
        BenchStat st = { 0 };
        r_dev_take_frame_stats(p->renderer);
        for (i32 i = 0; i < FRAMES; i++) {
            win32_bench_pump();
            p->event_count = 0;
            FrameInput input = win32_frame_input(p);
            app_update_and_render(p->app, &input, p->renderer);
            arena_reset(&p->scratch);
            win32_bench_record(p, &st);
        }
        win32_bench_log(p, "bench-buffer", names[k], &st);
    }
    r_dev_set_present_interval(p->renderer, 1);
}

// --bench-view: one event per frame through the real app_update_and_render (command + frame
// build, then flush + Present(0, 0)).
static void win32_bench_view_step(Platform *p, Event e, BenchStat *st) {
    win32_bench_pump();
    p->events[0] = e;
    p->event_count = 1;
    FrameInput input = win32_frame_input(p);
    app_update_and_render(p->app, &input, p->renderer);
    p->event_count = 0;
    arena_reset(&p->scratch);
    win32_bench_record(p, st);
}

static void win32_bench_view(Platform *p) {
    r_dev_set_present_interval(p->renderer, 0);
    win32_bench_log_display(p);
    LOG("bench-view: %D lines; build = command + frame build", app_dev_line_count(p->app));
    Event down = { .kind = EVENT_KEY_DOWN, .key = KEY_DOWN };
    Event page = { .kind = EVENT_KEY_DOWN, .key = KEY_PAGE_DOWN };
    Event end = { .kind = EVENT_KEY_DOWN, .key = KEY_END, .mods = KEYMOD_CTRL };
    Event home = { .kind = EVENT_KEY_DOWN, .key = KEY_HOME, .mods = KEYMOD_CTRL };
    Event type = { .kind = EVENT_TEXT, .codepoint = 'x' };
    Event plain = { .kind = EVENT_KEY_DOWN, .key = KEY_X, .codepoint = 'x' }; // ends dropping text after C-Home
    BenchStat st = { 0 }, st2 = { 0 };
    r_dev_take_frame_stats(p->renderer);

    app_dev_goto_line(p->app, 0);
    for (i32 i = 0; i < 10000; i++) win32_bench_view_step(p, down, &st);
    win32_bench_log(p, "bench-view", "next-line", &st);
    st = (BenchStat){ 0 };
    for (i32 i = 0; i < 1000; i++) win32_bench_view_step(p, page, &st);
    win32_bench_log(p, "bench-view", "scroll-up-command (PageDown)", &st);
    st = (BenchStat){ 0 };
    for (i32 i = 0; i < 100; i++) {
        win32_bench_view_step(p, end, &st);
        win32_bench_view_step(p, home, &st2);
    }
    win32_bench_log(p, "bench-view", "end-of-buffer (C-End)", &st);
    win32_bench_log(p, "bench-view", "beginning-of-buffer (C-Home)", &st2);
    st = (BenchStat){ 0 };
    app_dev_goto_line(p->app, 1000000);
    win32_bench_view_step(p, plain, &st2);
    for (i32 i = 0; i < 10000; i++) win32_bench_view_step(p, type, &st);
    win32_bench_log(p, "bench-view", "self-insert at line 1,000,000", &st);
    r_dev_set_present_interval(p->renderer, 1);
}

static u64 win32_bench_keys(Platform *p, const char *keys);

// --bench-windows: frames with four windows on the 100 MB file (top, middle, line 1,000,000, end) against
// one window; typing at line 1,000,000 with one window and with two windows on the same place.
static void win32_bench_windows(Platform *p) {
    r_dev_set_present_interval(p->renderer, 0);
    win32_bench_log_display(p);
    i64 lines = app_dev_line_count(p->app);
    LOG("bench-windows: %D lines; build = command + frame build", lines);
    Event idle = { .kind = EVENT_WAKEUP };
    Event type = { .kind = EVENT_TEXT, .codepoint = 'x' };
    Event plain = { .kind = EVENT_KEY_DOWN, .key = KEY_X, .codepoint = 'x' };
    r_dev_take_frame_stats(p->renderer);
    for (i32 round = 0; round < 2; round++) {
        BenchStat one = { 0 }, four = { 0 };
        app_dev_goto_line(p->app, 0);
        for (i32 i = 0; i < 300; i++) win32_bench_view_step(p, idle, &one);
        win32_bench_keys(p, "C-x 2 C-x 3 C-x o C-x o C-x 3"); // top-left, top-right, bottom-left, bottom-right
        i64 at[4] = { 0, lines / 2, 1000000, -1 };
        for (i32 w = 0; w < 4; w++) {
            win32_bench_keys(p, "C-x o");
            app_dev_goto_line(p->app, at[w]);
        }
        for (i32 i = 0; i < 300; i++) win32_bench_view_step(p, idle, &four);
        LOG("bench-windows: round %d: %d windows", round + 1, app_dev_window_count(p->app));
        win32_bench_log(p, "bench-windows", "frame, one window at the top", &one);
        win32_bench_log(p, "bench-windows", "frame, four windows (top, middle, line 1,000,000, end)", &four);
        win32_bench_keys(p, "C-x 1");
    }
    for (i32 round = 0; round < 2; round++) {
        BenchStat one = { 0 }, two = { 0 }, skip = { 0 };
        app_dev_goto_line(p->app, 1000000);
        win32_bench_view_step(p, plain, &skip);
        for (i32 i = 0; i < 10000; i++) win32_bench_view_step(p, type, &one);
        win32_bench_keys(p, "C-x 3"); // the new window on the same place: both show every insert
        win32_bench_view_step(p, plain, &skip);
        for (i32 i = 0; i < 10000; i++) win32_bench_view_step(p, type, &two);
        win32_bench_keys(p, "C-x 1");
        win32_bench_log(p, "bench-windows", "self-insert at line 1,000,000, one window", &one);
        win32_bench_log(p, "bench-windows", "self-insert at line 1,000,000, two windows on it", &two);
    }
    r_dev_set_present_interval(p->renderer, 1);
}

// --bench-cursors: one layout at 1, 10, 100, 1,000 and 10,000 cursors: self-insert, DEL and RET, each command and its
// frame (build = command + frame build); 100 commands each (10 at 10,000 cursors), and the batch flushes per command.
// mode 0: consecutive lines from line 1,000,000; 1: spread over the whole buffer. The classes run in sequence at
// the same cursors: typing, DEL over what was typed, RET.
static void win32_bench_cursors_layout(Platform *p, const char *layout, i32 mode) {
    static const i32 counts[] = { 1, 10, 100, 1000, 10000 };
    Event plain = { .kind = EVENT_KEY_DOWN, .key = KEY_X, .codepoint = 'x' };
    Event type = { .kind = EVENT_TEXT, .codepoint = 'x' };
    Event del = { .kind = EVENT_KEY_DOWN, .key = KEY_BACKSPACE };
    Event ret = { .kind = EVENT_KEY_DOWN, .key = KEY_ENTER };
    for (i32 k = 0; k < ARRAY_COUNT(counts); k++) {
        i32 n = counts[k], commands = n >= 10000 ? 10 : 100;
        BenchStat skip = { 0 };
        Event events[3] = { type, del, ret };
        const char *names[3] = { "self-insert", "delete-backward-char (DEL)", "newline (RET)" };
        if (mode == 0) app_dev_cursors(p->app, 1000000, 1, n);
        else app_dev_cursors_spread(p->app, n);
        win32_bench_view_step(p, plain, &skip); // ends dropping text after a jump
        i64 lines0 = app_dev_line_count(p->app);
        for (i32 c = 0; c < 3; c++) {
            i32 have = app_dev_cursor_count(p->app);
            u64 f0 = app_dev_batch_flushes(p->app);
            BenchStat st = { 0 };
            for (i32 i = 0; i < commands; i++) win32_bench_view_step(p, events[c], &st);
            u64 flushes = app_dev_batch_flushes(p->app) - f0;
            u8 what[200];
            i64 wn = fmt_buf(what, sizeof(what) - 1, "%s, %d cursors, %s (%U batch flushes in %d commands)", layout, have, names[c], flushes, commands);
            what[wn] = 0;
            win32_bench_log(p, "bench-cursors", (const char *)what, &st);
            if ((c < 2 && app_dev_cursor_count(p->app) != have) || (c == 1 && app_dev_line_count(p->app) != lines0)) { // RET may merge
                LOG("bench-cursors: FAIL: %s: %d cursors after %s (%d before), %D lines (%D before)", layout, app_dev_cursor_count(p->app),
                    names[c], have, app_dev_line_count(p->app), lines0);
                p->exit_code = EXIT_TEST;
            }
        }
    }
}

// The undo of a 10,000-cursor self-insert (one group of 10,000 records with its cursors), its undo-redo, and the
// undo of a merged group of 20 such commands; the undo log's commit before and after.
static void win32_bench_cursors_undo(Platform *p) {
    BenchStat skip = { 0 };
    Event plain = { .kind = EVENT_KEY_DOWN, .key = KEY_X, .codepoint = 'x' };
    app_dev_cursors(p->app, 1000000, 1, 10000);
    win32_bench_view_step(p, plain, &skip); // one command: one group
    AppDevMemory m0 = app_dev_memory(p->app);
    win32_bench_keys(p, "C-f"); // ends the merge
    u64 type = win32_bench_keys(p, "x");
    AppDevMemory m1 = app_dev_memory(p->app);
    u64 undo = win32_bench_keys(p, "C-/");
    i32 after_undo = app_dev_cursor_count(p->app);
    u64 redo = win32_bench_keys(p, "C-?");
    i32 after_redo = app_dev_cursor_count(p->app);
    win32_bench_keys(p, "C-f");
    u64 sum20 = 0;
    for (i32 i = 0; i < 20; i++) sum20 += win32_bench_keys(p, "y");
    u64 undo20 = win32_bench_keys(p, "C-/");
    AppDevMemory m2 = app_dev_memory(p->app);
    LOG("bench-cursors: undo: a self-insert at 10,000 cursors %U us; its undo %U us (%d cursors back), undo-redo %U us (%d cursors); "
        "20 merged self-inserts %U us each, their undo %U us; undo log committed %U KB before, %U KB after the one command, %U KB at the "
        "end", type, undo, after_undo, redo, after_redo, sum20 / 20, undo20, m0.undo / 1024, m1.undo / 1024, m2.undo / 1024);
    if (after_undo != 10000 || after_redo != 10000) {
        LOG("bench-cursors: FAIL: undo restored %d cursors, undo-redo %d (10,000 expected)", after_undo, after_redo);
        p->exit_code = EXIT_TEST;
    }
}

// The language-aware classes on a C file of 8-line blocks (test_bench_cursors_c) at 1,000 and 10,000 cursors: a
// fresh file and fresh cursors for each command (the file read again), the command and its frame, and the batch
// flushes it caused. A class that flushes once per cursor costs a pass over every marker per cursor.
static void win32_bench_cursors_classes(Platform *p, String8 tmp) {
    String8 path = test_bench_cursors_c(&p->perm, tmp);
    if (!app_dev_visit(p->app, path)) {
        LOG("bench-cursors: cannot visit %S", path);
        return;
    }
    static const struct { const char *name, *keys; i64 line, col; } classes[] = {
        { "self-insert", "x", 3, 0 },
        { "delete-backward-char (DEL)", "DEL", 3, 1 },
        { "newline (RET)", "RET", 3, 0 },
        { "'}' first on a line (electric reindent)", "}", 5, 0 },
        { "':' ending a case label (electric reindent)", ":", 2, -1 },
        { "indent-for-tab-command (TAB)", "TAB", 3, 0 },
        { "comment-line (M-;)", "M-;", 3, 0 },
        { "RET at the end of a whitespace-only line", "RET", 4, -1 },
    };
    static const i32 counts[] = { 1000, 10000 };
    for (i32 k = 0; k < ARRAY_COUNT(counts); k++) {
        i32 n = counts[k], reps = n >= 10000 ? 3 : 5;
        for (i32 c = 0; c < ARRAY_COUNT(classes); c++) {
            u64 sum = 0, worst = 0, flushes = 0;
            for (i32 r = 0; r < reps; r++) {
                app_dev_revert(p->app);
                app_dev_cursors_at(p->app, classes[c].line, 8, n, classes[c].col);
                Event idle = { .kind = EVENT_WAKEUP };
                BenchStat skip = { 0 };
                win32_bench_view_step(p, idle, &skip); // a frame: the view settled
                u64 f0 = app_dev_batch_flushes(p->app);
                u64 build = win32_bench_keys(p, classes[c].keys);
                flushes += app_dev_batch_flushes(p->app) - f0;
                sum += build;
                worst = MAX(worst, build);
            }
            LOG("bench-cursors: (d) C file, %d cursors, %s: build avg %U us, worst %U us; %U batch flushes per command (%d commands)", n,
                classes[c].name, sum / reps, worst, flushes / reps, reps);
        }
    }
    app_dev_revert(p->app);
}

// mark-all-like-this: a word with a million occurrences (test_bench_cursors_words) at the default max_cursors (10,000)
// and at the range's top (100,000), then a word only at the end of the 100 MB file (the whole file searched over
// frames). The command's frame and the frames after it until the search ended: their count, the longest one, the
// work in all (builds) and the time in all (with the Presents). Then a self-insert and its undo at that many cursors.
static void win32_bench_cursors_mark_all(Platform *p, String8 tmp, String8 big) {
    Event idle = { .kind = EVENT_WAKEUP };
    String8 words = test_bench_cursors_words(&p->perm, tmp);
    String8 conf = str8_fmt(&p->perm, "%S\\bench_cursors.conf", tmp);
    for (i32 round = 0; round < 3; round++) {
        i32 cap = round == 1 ? 100000 : 10000;
        if (round == 1) {
            os_write_file(conf, STR8_LIT("[settings]\nmax_cursors = 100000\n"));
            app_dev_use_config(p->app, conf);
        }
        if (round < 2) {
            if (!app_dev_visit(p->app, words)) {
                LOG("bench-cursors: cannot visit %S", words);
                return;
            }
            app_dev_select(p->app, 29, 35); // the first "needle"
        } else {
            if (!app_dev_visit(p->app, big)) return;
            app_dev_append(p->app, STR8_LIT("\nzqxwvu\n"));
            i64 end = app_dev_buffer_size(p->app);
            app_dev_select(p->app, end - 7, end - 1);
        }
        BenchStat st = { 0 };
        u64 t0 = os_time_us();
        u64 first = win32_bench_keys(p, "C-c C-<");
        while (app_wants_frame(p->app) && st.count < 100000) win32_bench_view_step(p, idle, &st);
        u64 total = os_time_us() - t0;
        i32 have = app_dev_cursor_count(p->app);
        LOG("bench-cursors: mark-all-like-this %s: %d cursors; %U frames, the command's %U us, the longest %U us, %U us of work in all, "
            "%U ms in all with the Presents", round == 0 ? "on a word with 1,000,000 occurrences (max_cursors 10,000)"
            : round == 1 ? "on a word with 1,000,000 occurrences (max_cursors 100,000)" : "on a word only at the end of the 100 MB file",
            have, st.count + 1, first, MAX(first, st.build_max), first + st.build_sum, total / 1000);
        if (have != (round == 2 ? 1 : cap)) {
            LOG("bench-cursors: FAIL: mark-all gave %d cursors", have);
            p->exit_code = EXIT_TEST;
        }
        if (round < 2) {
            u64 type = win32_bench_keys(p, "x"), undo = win32_bench_keys(p, "C-/");
            LOG("bench-cursors: at %d cursors (one per line of the word file): a self-insert %U us, its undo %U us", have, type, undo);
            win32_bench_view_step(p, idle, &st);
            LOG("bench-cursors: idle with %d cursors: %s", app_dev_cursor_count(p->app), app_wants_frame(p->app) ? "a frame wanted" : "no frame wanted");
        }
        win32_bench_keys(p, "C-g C-g");
    }
}

static void win32_bench_cursors(Platform *p) {
    r_dev_set_present_interval(p->renderer, 0);
    win32_bench_log_display(p);
    LOG("bench-cursors: %D lines; build = command + frame build", app_dev_line_count(p->app));
    r_dev_take_frame_stats(p->renderer);
    String8 tmp = str8_fmt(&p->perm, "%S\\tmp", p->exe_dir), big = p->files[0].path;
    // The memory of 10,000 cursors (first: nothing committed for them yet), and of 100,000.
    u64 bytes0 = os_dev_private_bytes();
    app_dev_cursors(p->app, 1000000, 1, 10000);
    u64 bytes1 = os_dev_private_bytes();
    app_dev_cursors(p->app, 0, 1, 100000);
    u64 bytes2 = os_dev_private_bytes();
    LOG("bench-cursors: memory: 10,000 cursors +%U KB private bytes, 100,000 cursors +%U KB (the cursor array and the markers)",
        (bytes1 - MIN(bytes0, bytes1)) / 1024, (bytes2 - MIN(bytes0, bytes2)) / 1024);
    app_dev_cursors(p->app, 0, 1, 1);
    win32_bench_cursors_layout(p, "(a) 100 MB, consecutive lines from 1,000,000", 0);
    win32_bench_cursors_undo(p);
    win32_bench_cursors_layout(p, "(b) 100 MB, spread over the file", 1);
    String8 src = os_full_path(&p->perm, str8_fmt(&p->perm, "%S\\..\\src\\app.c", p->exe_dir));
    if (app_dev_visit(p->app, src)) win32_bench_cursors_layout(p, "(c) src/app.c, spread over the file", 1);
    else LOG("bench-cursors: cannot visit %S", src);
    win32_bench_cursors_classes(p, tmp);
    win32_bench_cursors_mark_all(p, tmp, big);
    r_dev_set_present_interval(p->renderer, 1);
}

u64 os_dev_private_bytes(void) {
    PROCESS_MEMORY_COUNTERS_EX pmc = { .cb = sizeof(pmc) };
    if (!K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof(pmc))) return 0;
    return (u64)pmc.PrivateUsage;
}

// Runs one key token (or several) through the app, one frame, and returns the frame's command +
// build time.
// --bench-longline: one line of 5 MB of minified JavaScript (build\tmp\longline.js, made once). Typing at its end
// and in its middle, moving in its middle (columns, the view scrolled far right): command + frame build per key.
#define BENCH_LONGLINE_SIZE MB(5)

static String8 win32_bench_longline_file(Platform *p) {
    String8 path = str8_fmt(&p->perm, "%S\\tmp\\longline.js", p->exe_dir);
    OsFileInfo info;
    if (os_file_info(path, &info) == OS_FILE_OK && info.size >= BENCH_LONGLINE_SIZE) return path;
    u8 *text = PUSH_ARRAY(&p->perm, u8, BENCH_LONGLINE_SIZE + 128);
    i64 n = 0;
    for (i32 i = 0; n < BENCH_LONGLINE_SIZE; i++) n += fmt_buf(text + n, 128, "var a%d=b(%d,\"s%d\")+c[%d];", i, i, i % 97, i % 13);
    text[n++] = '\n';
    if (!os_write_file(path, str8(text, n))) LOG("bench-longline: cannot write %S", path);
    return path;
}

static u64 win32_bench_keys(Platform *p, const char *keys);

static void win32_bench_longline(Platform *p) {
    r_dev_set_present_interval(p->renderer, 0);
    win32_bench_log_display(p);
    i64 size = app_dev_buffer_size(p->app);
    LOG("bench-longline: %D bytes on one line; build = command + frame build", size);
    Event type = { .kind = EVENT_TEXT, .codepoint = 'x' };
    Event right = { .kind = EVENT_KEY_DOWN, .key = KEY_RIGHT };
    Event left = { .kind = EVENT_KEY_DOWN, .key = KEY_LEFT };
    for (i32 round = 0; round < 2; round++) {
        BenchStat end = { 0 }, mid = { 0 }, move = { 0 };
        win32_bench_keys(p, "C-e");
        for (i32 i = 0; i < 200; i++) win32_bench_view_step(p, type, &end);
        app_dev_select(p->app, size / 2, size / 2);
        win32_bench_keys(p, "C-g");
        for (i32 i = 0; i < 200; i++) win32_bench_view_step(p, type, &mid);
        for (i32 i = 0; i < 200; i++) win32_bench_view_step(p, i % 2 ? left : right, &move);
        win32_bench_log(p, "bench-longline", "typing at the end of the line", &end);
        win32_bench_log(p, "bench-longline", "typing in the middle of the line (the view scrolled right)", &mid);
        win32_bench_log(p, "bench-longline", "right / left in the middle of the line", &move);
    }
    r_dev_set_present_interval(p->renderer, 1);
}

static u64 win32_bench_keys(Platform *p, const char *keys) {
    Event events[64];
    i32 n = app_dev_key_events(p->app, str8_cstr(keys), events, ARRAY_COUNT(events));
    win32_bench_pump();
    for (i32 i = 0; i < n; i++) p->events[i] = events[i];
    p->event_count = n;
    FrameInput input = win32_frame_input(p);
    app_update_and_render(p->app, &input, p->renderer);
    p->event_count = 0;
    arena_reset(&p->scratch);
    return app_dev_build_us(p->app);
}

// --bench-edit: typing with undo on, kill / yank / undo of a 50 MB region, and memory: the undo
// log, the kill ring, and the committed bytes each extra open buffer costs.
static void win32_bench_edit(Platform *p) {
    r_dev_set_present_interval(p->renderer, 0);
    win32_bench_log_display(p);
    i64 lines = app_dev_line_count(p->app);
    LOG("bench-edit: %D lines, private bytes after loading %U KB", lines, os_dev_private_bytes() / 1024);
    // 10,000 self-inserts at line 1,000,000, undo on (merged into groups of 20).
    app_dev_goto_line(p->app, 1000000);
    Event plain = { .kind = EVENT_KEY_DOWN, .key = KEY_X, .codepoint = 'x' };
    Event type = { .kind = EVENT_TEXT, .codepoint = 'x' };
    BenchStat st = { 0 };
    r_dev_take_frame_stats(p->renderer);
    win32_bench_view_step(p, plain, &st);
    st = (BenchStat){ 0 };
    for (i32 i = 0; i < 10000; i++) win32_bench_view_step(p, type, &st);
    win32_bench_log(p, "bench-edit", "self-insert at line 1,000,000 (undo on)", &st);
    AppDevMemory m = app_dev_memory(p->app);
    LOG("bench-edit: undo log after 10,000 inserts: %U KB committed", m.undo / 1024);
    // A 50 MB region: the second half of the file.
    app_dev_goto_line(p->app, lines / 2);
    win32_bench_keys(p, "C-SPC");
    win32_bench_keys(p, "M->");
    u64 before = os_dev_private_bytes();
    u64 kill = win32_bench_keys(p, "C-w");
    m = app_dev_memory(p->app);
    LOG("bench-edit: kill-region of ~50 MB: %U ms (command + frame), %D lines left; undo log %U MB, kill ring %U MB committed, "
        "private bytes +%U MB (the fake clipboard holds it once more, as UTF-16)", kill / 1000, app_dev_line_count(p->app),
        m.undo >> 20, m.kill_ring >> 20, (os_dev_private_bytes() - before) >> 20);
    u64 undo_kill = win32_bench_keys(p, "C-/");
    LOG("bench-edit: undo of the kill: %U ms, %D lines", undo_kill / 1000, app_dev_line_count(p->app));
    win32_bench_keys(p, "M->");
    u64 yank = win32_bench_keys(p, "C-y");
    LOG("bench-edit: yank of the 50 MB at the end: %U ms, %D lines", yank / 1000, app_dev_line_count(p->app));
    u64 undo_yank = win32_bench_keys(p, "C-/");
    m = app_dev_memory(p->app);
    LOG("bench-edit: undo of the yank: %U ms, %D lines; undo log %U MB committed (limit 64 MB: older groups dropped, the last "
        "one kept)", undo_yank / 1000, app_dev_line_count(p->app), m.undo >> 20);
    // The committed bytes of an extra open buffer: 20 small files.
    String8 tmp = str8_fmt(&p->perm, "%S\\tmp", p->exe_dir);
    u8 text[2048];
    for (i32 i = 0; i < (i32)sizeof(text); i++) text[i] = i % 64 == 63 ? '\n' : (u8)('a' + i % 26);
    for (i32 i = 0; i < 20; i++) os_write_file(str8_fmt(&p->perm, "%S\\bench_small_%02d.c", tmp, i), str8(text, sizeof(text)));
    before = os_dev_private_bytes();
    for (i32 i = 0; i < 20; i++) app_dev_visit(p->app, str8_fmt(&p->perm, "%S\\bench_small_%02d.c", tmp, i));
    u64 after = os_dev_private_bytes();
    m = app_dev_memory(p->app);
    LOG("bench-edit: 20 extra buffers of 2 KB: private bytes +%U KB, %U KB per buffer; one buffer's own commit %U KB: meta %U, "
        "text %U, line index %U, markers %U, undo %U KB", (after - before) / 1024, (after - before) / 20 / 1024, m.buffer / 1024,
        m.meta / 1024, m.text / 1024, m.line_index / 1024, m.markers / 1024, m.undo_log / 1024);
    r_dev_set_present_interval(p->renderer, 1);
}

// --bench-syntax, frame part: frames without events until the states of what is shown are complete.
// Logs the frames, the time and the longest frame (command + frame build); with vsync each frame
// would also wait one refresh interval.
static void win32_bench_catch_up(Platform *p, const char *what, u32 refresh_us) {
    BenchStat st = { 0 };
    u64 t0 = os_time_us();
    do { // at least one frame: a jump without a frame has not asked for states yet
        win32_bench_pump();
        p->event_count = 0;
        FrameInput input = win32_frame_input(p);
        app_update_and_render(p->app, &input, p->renderer);
        arena_reset(&p->scratch);
        win32_bench_record(p, &st);
    } while (app_wants_frame(p->app) && st.count < 1000000);
    u64 total = os_time_us() - t0;
    LOG("bench-syntax: %s: %U frames, %U ms until the colors are right (Present(0, 0)); longest frame build %U us, avg %U us; "
        "with vsync at one frame per refresh: ~%U ms", what, st.count, total / 1000, st.build_max, st.build_sum / MAX(st.count, 1),
        st.count * refresh_us / 1000);
}

// --bench-syntax: lexing throughput per language (test.c), then with the 100 MB C file: "/*" typed on
// the first line and a jump to the end, a jump to line 1,000,000 after undoing it, and 10,000
// self-inserts there with highlighting on and with the buffer in Fundamental.
static void win32_bench_syntax(Platform *p) {
    test_bench_syntax();
    r_dev_set_present_interval(p->renderer, 0);
    win32_bench_log_display(p);
    MONITORINFOEXW mi = { .cbSize = sizeof(mi) };
    DEVMODEW dm = { .dmSize = sizeof(dm) };
    u32 refresh_us = 16667;
    if (GetMonitorInfoW(MonitorFromWindow(p->hwnd, MONITOR_DEFAULTTONEAREST), (MONITORINFO *)&mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) {
        refresh_us = 1000000u / (u32)dm.dmDisplayFrequency;
    }
    LOG("bench-syntax: %D lines of C; build = command + frame build", app_dev_line_count(p->app));
    win32_bench_catch_up(p, "the top after loading", refresh_us);
    app_dev_goto_line(p->app, 0);
    win32_bench_keys(p, "/");
    u64 slash_star = win32_bench_keys(p, "*");
    LOG("bench-syntax: typing \"/*\" on the first line: %U us (command + frame build)", slash_star);
    win32_bench_keys(p, "M->");
    win32_bench_catch_up(p, "\"/*\" on the first line, then M->", refresh_us);
    win32_bench_keys(p, "C-/");
    app_dev_goto_line(p->app, 1000000);
    win32_bench_catch_up(p, "undone, then line 1,000,000", refresh_us);
    AppDevMemory m = app_dev_memory(p->app);
    LOG("bench-syntax: states of the 100 MB file: %U KB committed (line index %U KB)", m.states / 1024, m.line_index / 1024);
    Event type = { .kind = EVENT_TEXT, .codepoint = 'x' };
    Event plain = { .kind = EVENT_KEY_DOWN, .key = KEY_X, .codepoint = 'x' };
    for (i32 round = 0; round < 2; round++) {
        if (round == 1) {
            app_dev_set_language(p->app, BUFFER_LANG_FUNDAMENTAL);
            win32_bench_catch_up(p, "Fundamental", refresh_us);
        }
        BenchStat st = { 0 }, st2 = { 0 };
        app_dev_goto_line(p->app, 1000000);
        win32_bench_view_step(p, plain, &st2);
        win32_bench_catch_up(p, "before typing", refresh_us);
        for (i32 i = 0; i < 10000; i++) win32_bench_view_step(p, type, &st);
        win32_bench_log(p, "bench-syntax", round == 0 ? "self-insert at line 1,000,000, highlighting on (C)"
                                                      : "self-insert at line 1,000,000, Fundamental (no highlighting)", &st);
    }
    r_dev_set_present_interval(p->renderer, 1);
}

// --bench-search: `keys` (--keys notation) through the app, one event per frame, then frames without
// events until no search work is pending. Returns the time from the first event to the last frame.
static u64 win32_bench_search_keys(Platform *p, const char *keys, BenchStat *st) {
    Event events[256];
    i32 n = app_dev_key_events(p->app, str8_cstr(keys), events, ARRAY_COUNT(events));
    u64 t0 = os_time_us();
    for (i32 i = 0; i < n; i++) win32_bench_view_step(p, events[i], st);
    while (app_wants_frame(p->app) && st->count < 10000000) {
        win32_bench_pump();
        p->event_count = 0;
        FrameInput input = win32_frame_input(p);
        app_update_and_render(p->app, &input, p->renderer);
        arena_reset(&p->scratch);
        win32_bench_record(p, st);
    }
    return os_time_us() - t0;
}

// --bench-search: isearch on the 100 MB file for a needle only at its very end and one that does not
// occur, each folded and exact (typed one key per frame, then the frames until the search is done);
// keystroke latency of isearch on src/app.c; replace-string over 1,000,000 occurrences, then the undo
// of that one group and its undo-redo.
static void win32_bench_search(Platform *p) {
    r_dev_set_present_interval(p->renderer, 0);
    win32_bench_log_display(p);
    app_dev_append(p->app, STR8_LIT("Needle_At_The_End\n"));
    i64 size = app_dev_size(p->app);
    LOG("bench-search: %D bytes, %D lines; frames = typing the needle, then until the search is done; build = command + "
        "frame build (search slices included), Present(0, 0) not counted", size, app_dev_line_count(p->app));
    // Each needle typed (one key per frame) and pasted at once (C-y of the clipboard: one search step).
    static const struct { const char *keys, *needle, *what; b32 found, exact; } runs[] = {
        { "n e e d l e _ a t _ t h e _ e n d", "needle_at_the_end", "needle at the end, folded", 1, 0 },
        { "N e e d l e _ A t _ T h e _ E n d", "Needle_At_The_End", "needle at the end, exact", 1, 1 },
        { "n e v e r _ h e r e _ a t _ a l l", "never_here_at_all", "needle not in the file, folded", 0, 0 },
        { "N e v e r _ H e r e _ A t _ A l l", "Never_Here_At_All", "needle not in the file, exact", 0, 1 },
    };
    for (i32 i = 0; i < ARRAY_COUNT(runs); i++) {
        for (i32 pasted = 0; pasted < 2; pasted++) {
            app_dev_goto_line(p->app, 0);
            win32_bench_keys(p, "C-s");
            if (pasted) os_dev_clipboard_external(str8_cstr(runs[i].needle));
            BenchStat st = { 0 };
            r_dev_take_frame_stats(p->renderer);
            // Pasting lowercases while the search folds (a yank keeps it folded): M-c first makes the exact search exact.
            const char *paste = runs[i].exact ? "M-c C-y" : "C-y";
            u64 total = win32_bench_search_keys(p, pasted ? paste : runs[i].keys, &st);
            b32 ok = runs[i].found ? app_dev_point(p->app) == size - 1 : app_dev_isearch_failing(p->app);
            LOG("bench-search: %s, %s: %U ms in %U frames; longest frame build %U us, avg %U us; Present avg %U us; %s", runs[i].what,
                pasted ? (runs[i].exact ? "pasted (M-c C-y, one step)" : "pasted (C-y, one step)") : "typed (34 events)", total / 1000, st.count, st.build_max,
                st.build_sum / MAX(st.count, 1), st.present_sum / MAX(st.count, 1), ok ? "result right" : "RESULT WRONG");
            win32_bench_keys(p, runs[i].found ? "RET" : "C-g");
            if (!runs[i].found) win32_bench_keys(p, "C-g");
        }
    }

    // Keystroke latency on an ordinary source file: C-s, typing, repeating, DEL, RET; 50 rounds.
    String8 source = str8_fmt(&p->perm, "%S\\..\\src\\app.c", p->exe_dir);
    if (app_dev_visit(p->app, source)) {
        BenchStat st = { 0 };
        r_dev_take_frame_stats(p->renderer);
        for (i32 round = 0; round < 50; round++) {
            win32_bench_search_keys(p, "C-s v i e w C-s C-s C-s C-s C-r DEL DEL DEL _ d r a w RET", &st);
            app_dev_goto_line(p->app, round * 30);
        }
        win32_bench_log(p, "bench-search", "isearch keystrokes on src/app.c (C-s, typing, repeats, DEL, RET; with lazy highlight)", &st);
    }

    // replace-string over 1,000,000 occurrences ("foo bar\n" x 1,000,000 in *scratch*), its undo and undo-redo.
    i64 lines = 1000000;
    u8 *text = PUSH_ARRAY(&p->perm, u8, lines * 8);
    for (i64 i = 0; i < lines; i++) memcpy(text + i * 8, "foo bar\n", 8);
    app_dev_show_scratch(p->app, str8(text, lines * 8));
    u64 undo_before = app_dev_memory(p->app).undo;
    BenchStat st = { 0 };
    r_dev_take_frame_stats(p->renderer);
    win32_bench_keys(p, "M-x");
    win32_bench_search_keys(p, "r e p l a c e - s t r i n g RET f o o RET b a z", &st);
    st = (BenchStat){ 0 };
    u64 total = win32_bench_search_keys(p, "RET", &st);
    AppDevMemory m = app_dev_memory(p->app);
    LOG("bench-search: replace-string foo -> baz, 1,000,000 occurrences in 8 MB: %U ms in %U frames; longest frame build %U us, "
        "avg %U us; undo log %U KB committed (+%U KB)", total / 1000, st.count, st.build_max, st.build_sum / MAX(st.count, 1),
        m.undo / 1024, (m.undo - undo_before) / 1024);
    st = (BenchStat){ 0 };
    total = win32_bench_search_keys(p, "C-/", &st);
    LOG("bench-search: the one undo of those 1,000,000 replacements: %U ms in %U frame(s), longest frame build %U us", total / 1000,
        st.count, st.build_max);
    st = (BenchStat){ 0 };
    total = win32_bench_search_keys(p, "C-?", &st);
    LOG("bench-search: its undo-redo: %U ms in %U frame(s), longest frame build %U us", total / 1000, st.count, st.build_max);

    // The 100 MB file (manual K17): replace-string shows its progress, C-g stops it, one C-x u gives
    // back the file's text exactly. A wrong result fails the bench (exit 9).
    if (app_dev_visit(p->app, p->files[0].path)) {
        u64 hash0 = win32_dev_hash(app_dev_text(p->app, &p->scratch));
        arena_reset(&p->scratch);
        i64 size0 = app_dev_size(p->app);
        win32_bench_keys(p, "M-<");
        win32_bench_keys(p, "M-x r e p l a c e - s t r i n g RET b u f f e r RET B U F RET");
        for (i32 k = 0; k < 3; k++) win32_bench_keys(p, "");
        String8 progress = app_dev_replace_prompt(p->app, &p->perm);
        win32_bench_keys(p, "C-g");
        String8 stopped = str8_copy(&p->perm, app_dev_echo(p->app));
        i64 size1 = app_dev_size(p->app);
        u64 t0 = os_time_us();
        win32_bench_keys(p, "C-x u");
        for (i32 k = 0; k < 100000 && app_wants_frame(p->app); k++) win32_bench_keys(p, "");
        u64 undo_us = os_time_us() - t0;
        u64 hash1 = win32_dev_hash(app_dev_text(p->app, &p->scratch));
        arena_reset(&p->scratch);
        b32 ok = str8_starts_with(progress, STR8_LIT("Replacing... ")) && str8_starts_with(stopped, STR8_LIT("Replaced ")) &&
                 test_contains(stopped, "(stopped)") && size1 != size0 && app_dev_size(p->app) == size0 && hash1 == hash0;
        LOG("bench-search: %s: replace-string buffer -> BUF in the 100 MB file: after 4 frames '%S'; C-g: '%S' (%D bytes changed); "
            "one C-x u in %U ms gives back the exact text %d", ok ? "ok" : "FAIL", progress, stopped, size0 - size1, undo_us / 1000,
            hash1 == hash0 && app_dev_size(p->app) == size0);
        if (!ok) p->exit_code = EXIT_TEST;
    }
    r_dev_set_present_interval(p->renderer, 1);
}

// FNV-1a: comparing big texts by a hash.
static u64 win32_dev_hash(String8 s) {
    u64 h = 0xcbf29ce484222325ull;
    for (i64 i = 0; i < s.len; i++) h = (h ^ s.data[i]) * 0x100000001b3ull;
    return h;
}

// --bench-complete: filtering and ranking 10,000 and 100,000 candidates, per keystroke that changes the
// input (typing, deleting, clearing), three rounds of a scripted input. Filter = the matcher and the
// ranking alone; build = the command and the frame build (Present separately, not counted).
static void win32_bench_complete(Platform *p) {
    r_dev_set_present_interval(p->renderer, 0);
    win32_bench_log_display(p);
    static const char *script = "s r c SPC v i e w DEL DEL DEL DEL b u f f e r SPC 4 2 C-a C-k m a i n . c DEL DEL t x t "
                                "C-a C-k l i s t SPC d r a w SPC t e s t C-a C-k";
    i64 sizes[] = { 10000, 100000 };
    Event events[512];
    i32 n = app_dev_key_events(p->app, str8_cstr(script), events, ARRAY_COUNT(events));
    for (i32 s = 0; s < ARRAY_COUNT(sizes); s++) {
        u64 open_us = app_dev_bench_complete_open(p->app, sizes[s]);
        u64 filters, last, before;
        i64 matches;
        app_dev_filter_stats(p->app, &before, &last, &matches);
        LOG("bench-complete: %D candidates built, folded and ranked in %U us (first filter %U us)", sizes[s], open_us, last);
        u64 count = 0, f_sum = 0, f_max = 0, b_sum = 0, b_max = 0;
        i64 most = 0, least = I64_MAX;
        for (i32 round = 0; round < 3; round++) {
            for (i32 i = 0; i < n; i++) {
                win32_bench_pump();
                p->events[0] = events[i];
                p->event_count = 1;
                FrameInput input = win32_frame_input(p);
                app_update_and_render(p->app, &input, p->renderer);
                p->event_count = 0;
                arena_reset(&p->scratch);
                r_dev_take_frame_stats(p->renderer);
                app_dev_filter_stats(p->app, &filters, &last, &matches);
                if (filters == before) continue; // a KEY_DOWN whose text follows: nothing changed
                before = filters;
                u64 build = app_dev_build_us(p->app);
                count++;
                f_sum += last;
                f_max = MAX(f_max, last);
                b_sum += build;
                b_max = MAX(b_max, build);
                most = MAX(most, matches);
                least = MIN(least, matches);
            }
        }
        count = MAX(count, 1);
        LOG("bench-complete: %D candidates, %U keystrokes: filter + rank avg %U us, worst %U us; command + frame build avg %U us, "
            "worst %U us; %D to %D matches", sizes[s], count, f_sum / count, f_max, b_sum / count, b_max, least, most);
    }
    r_dev_set_present_interval(p->renderer, 1);
}

// --bench-project, scanning: a project scan of `root` through the app, waited for; its numbers.
static void win32_bench_scan(Platform *p, String8 root, const char *what) {
    u64 before = os_dev_private_bytes(), t0 = os_time_us();
    app_dev_bench_scan_start(p->app, root);
    i64 files = 0, ignored = 0, errors = 0;
    u64 memory = 0;
    while (app_dev_bench_scan_state(p->app, root, &files, &ignored, &errors, &memory)) {
        if (os_dev_wake_wait(100)) app_wake(p->app);
    }
    u64 took = os_time_us() - t0, after = os_dev_private_bytes();
    LOG("bench-project: scan of %s (%S): %D files, %D ignored, %D unreadable or too long, %U ms; index %U KB committed (%U bytes a "
        "file); private bytes +%U KB", what, root, files, ignored, errors, took / 1000, memory / 1024, files ? memory / (u64)files : 0,
        after > before ? (after - before) / 1024 : 0);
    app_dev_bench_scan_close(p->app, root);
}

// --bench-project: fuzzy ranking on 10,000, 100,000 and 1,000,000 synthetic paths (a query typed a
// character at a time, then deleted: each keystroke's frame, the frames until the ranking is done, the
// longest frame; a whole ranking at once, as RET does); scans of the generated tree and of %SystemRoot%
// (read only); typing at line 1,000,000 of the 100 MB file with and without a scan running.
static void win32_bench_project(Platform *p) {
    r_dev_set_present_interval(p->renderer, 0);
    win32_bench_log_display(p);
    static const char *const keys[] = { "w", "i", "n", "m", "a", "i", "DEL", "DEL", "DEL", "DEL", "DEL", "DEL" };
    static const i64 sizes[] = { 10000, 100000, 1000000 };
    for (i32 s = 0; s < ARRAY_COUNT(sizes); s++) {
        u64 t0 = os_time_us();
        u64 memory = app_dev_bench_finder_open(p->app, sizes[s]);
        u64 made = os_time_us() - t0;
        win32_bench_keys(p, "");
        u64 first_sum = 0, first_max = 0, frames_sum = 0, frames_max = 0, done_sum = 0, done_max = 0, longest = 0, count = 0, over = 0;
        for (i32 round = 0; round < 2; round++) {
            for (i32 k = 0; k < ARRAY_COUNT(keys); k++) {
                u64 start = os_time_us();
                u64 b = win32_bench_keys(p, keys[k]);
                first_sum += b;
                first_max = MAX(first_max, b);
                longest = MAX(longest, b);
                u64 frames = 1;
                if (b > SYNTAX_FRAME_BUDGET_US + 100) over++;
                while (app_wants_frame(p->app) && frames < 100000) {
                    u64 f = win32_bench_keys(p, "");
                    longest = MAX(longest, f);
                    if (f > SYNTAX_FRAME_BUDGET_US + 100) over++;
                    frames++;
                }
                u64 done = os_time_us() - start;
                frames_sum += frames;
                frames_max = MAX(frames_max, frames);
                done_sum += done;
                done_max = MAX(done_max, done);
                count++;
            }
        }
        u64 finish = app_dev_bench_finder_finish(p->app, STR8_LIT("winmai"));
        i64 matches = app_dev_bench_matches(p->app);
        LOG("bench-project: %D paths (index %U KB, made in %U ms), %U keystrokes: the keystroke's frame (command + build) avg %U us, "
            "worst %U us; frames until ranked avg %U, worst %U; time until ranked avg %U us, worst %U us; longest frame %U us, %U "
            "frame(s) over %d us (the budget plus 100 us); a whole ranking at once ('winmai', %D matches, as RET does) %U us",
            sizes[s], memory / 1024, made / 1000, count, first_sum / count, first_max, frames_sum / count, frames_max, done_sum / count,
            done_max, longest, over, SYNTAX_FRAME_BUDGET_US + 100, matches, finish);
        app_dev_bench_finder_close(p->app);
        win32_bench_keys(p, "");
    }

    // Scans: the generated tree (as --test makes it) and the Windows directory.
    String8 tree = str8_fmt(&p->perm, "%S\\tmp\\p11big", p->exe_dir);
    if (os_file_info(str8_fmt(&p->perm, "%S\\tmp\\p11big.done", p->exe_dir), &(OsFileInfo){ 0 }) != OS_FILE_OK) {
        os_make_dir(tree);
        for (i32 d = 0; d < 100; d++) {
            String8 dir = str8_fmt(&p->perm, "%S\\d%02d", tree, d);
            os_make_dir(dir);
            for (i32 f = 0; f < 100; f++) os_write_file(str8_fmt(&p->scratch, "%S\\file_%04d.c", dir, d * 100 + f), STR8_LIT("x"));
            arena_reset(&p->scratch);
        }
        os_write_file(str8_fmt(&p->perm, "%S\\tmp\\p11big.done", p->exe_dir), STR8_LIT("10000 files\n"));
    }
    String8 windows = os_get_env(&p->perm, STR8_LIT("SystemRoot"));
    win32_bench_scan(p, tree, "the generated tree");
    if (windows.len) win32_bench_scan(p, windows, "%SystemRoot%");

    // Typing at line 1,000,000 (a fresh line each time), without and with a scan of the Windows directory
    // running; its batches are taken between frames, as the main loop does. Only the frames while it runs count.
    Event type = { .kind = EVENT_TEXT, .codepoint = 'x' };
    Event plain = { .kind = EVENT_KEY_DOWN, .key = KEY_X, .codepoint = 'x' };
    i64 line = 1000000;
    for (i32 round = 0; round < 2 && windows.len; round++) {
        for (i32 with_scan = 0; with_scan < 2; with_scan++) {
            b32 scanning = (round == 0) == (with_scan == 1);
            BenchStat st = { 0 }, skip = { 0 };
            app_dev_goto_line(p->app, line);
            line += 2;
            if (scanning) app_dev_bench_scan_start(p->app, windows);
            win32_bench_view_step(p, plain, &skip);
            i64 files = 0, ignored, errors;
            u64 memory;
            for (i32 i = 0; i < 10000; i++) {
                if (scanning) {
                    if (os_dev_wake_wait(0)) app_wake(p->app);
                    if (!app_dev_bench_scan_state(p->app, windows, &files, &ignored, &errors, &memory)) break;
                }
                win32_bench_view_step(p, type, &st);
            }
            win32_bench_log(p, "bench-project", scanning ? "self-insert at line 1,000,000 while %SystemRoot% is scanned" : "self-insert at line 1,000,000, no scan", &st);
            if (scanning) {
                LOG("bench-project: the scan listed %D files during those %U frames", files, st.count);
                app_dev_bench_scan_close(p->app, windows);
            }
        }
    }
    r_dev_set_present_interval(p->renderer, 1);
}

// --bench-grep's tree: ~100 MB in 3,000 files (30 directories of 100), source-like lines; the common word
// about once per 2 KB (one word in 340), the missing one nowhere; every 100th file binary (a NUL first), every 300th UTF-16.
#define BENCH_GREP_COMMON "ronin_grep_common"
#define BENCH_GREP_MISSING "zq_never_here"
static String8 win32_bench_grep_tree(Platform *p) {
    String8 root = str8_fmt(&p->perm, "%S\\tmp\\p12bench", p->exe_dir), done = str8_fmt(&p->perm, "%S\\tmp\\p12bench.done", p->exe_dir);
    if (os_file_info(done, &(OsFileInfo){ 0 }) == OS_FILE_OK) return root;
    static const char *const words[] = { "static", "void", "buffer", "return", "if", "for", "while", "app", "view", "render", "search", "i64",
                                         "u8", "const", "struct", "line", "text", "count", "the", "index", "file", "path", "arena", "=" };
    os_make_dir(root);
    os_write_file(str8_fmt(&p->perm, "%S\\.ronin-project", root), STR8_LIT("[ignore]\n"));
    u64 rng = 0x12AB;
    i64 cap = KB(40), total = 0;
    u8 *text = PUSH_ARRAY(&p->perm, u8, cap), *wide = PUSH_ARRAY(&p->perm, u8, cap * 2 + 2);
    for (i32 d = 0; d < 30; d++) {
        String8 dir = str8_fmt(&p->scratch, "%S\\dir%02d", root, d);
        os_make_dir(dir);
        for (i32 f = 0; f < 100; f++) {
            i64 n = 0;
            i32 id = d * 100 + f;
            if (id % 100 == 7) text[n++] = 0; // binary
            while (n < KB(33)) {
                i32 words_n = 3 + (i32)((rng = rng * 6364136223846793005ull + 1442695040888963407ull) >> 60);
                for (i32 w = 0; w < words_n; w++) {
                    rng = rng * 6364136223846793005ull + 1442695040888963407ull;
                    const char *word = (rng >> 33) % 340 == 0 ? BENCH_GREP_COMMON : words[(rng >> 40) % ARRAY_COUNT(words)];
                    n += fmt_buf(text + n, cap - n, "%s%s", w ? " " : "    ", word);
                }
                text[n++] = '\n';
            }
            String8 content = str8(text, n);
            if (id % 300 == 150) { // UTF-16 LE with a BOM
                wide[0] = 0xFF;
                wide[1] = 0xFE;
                for (i64 i = 0; i < n; i++) {
                    wide[2 + 2 * i] = text[i];
                    wide[3 + 2 * i] = 0;
                }
                content = str8(wide, 2 + 2 * n);
            }
            os_write_file(str8_fmt(&p->scratch, "%S\\file_%04d.c", dir, id), content);
            total += content.len;
        }
        arena_reset(&p->scratch);
    }
    os_write_file(done, str8_fmt(&p->perm, "3000 files, %D bytes\n", total));
    return root;
}

// One search to its end through the main loop's way of working (a frame when a worker woke it or work is
// pending); its numbers.
typedef struct BenchGrepRun {
    i64 found, bytes, files;
    u64 first_us, total_us, longest_us, frames, over, work_max_us;
    b32 limited;
    u64 peak_private;
} BenchGrepRun;

static BenchGrepRun win32_bench_grep_run(Platform *p, String8 root, const char *needle, i32 workers, i32 max_results) {
    BenchGrepRun run = { 0 };
    app_dev_results_work_max(p->app);
    app_dev_bench_grep_start(p->app, root, str8_cstr(needle), workers, max_results);
    win32_bench_keys(p, ""); // the frame the command runs in (its results work starts there)
    u64 start = os_time_us();
    for (;;) {
        if (os_time_us() - start > 60 * 1000000ull) {
            app_dev_results_log(p->app, "bench-grep: a search over 60 s");
            break;
        }
        b32 woke = os_dev_wake_wait(app_wants_frame(p->app) ? 0 : 50);
        b32 frame = app_wants_frame(p->app);
        if (woke) frame |= app_wake(p->app);
        if (frame) {
            u64 b = win32_bench_keys(p, "");
            run.longest_us = MAX(run.longest_us, b);
            run.over += b > SYNTAX_FRAME_BUDGET_US + 100;
            run.frames++;
            if ((run.frames & 15) == 0) run.peak_private = MAX(run.peak_private, os_dev_private_bytes());
        }
        if (!app_dev_bench_grep_state(p->app, &run.found, &run.bytes, &run.files, &run.first_us, &run.total_us, &run.limited)) break;
    }
    run.peak_private = MAX(run.peak_private, os_dev_private_bytes());
    run.work_max_us = app_dev_results_work_max(p->app);
    return run;
}

// Typing at line `line` of the 100 MB file (left window) while searches for the common word run one after
// another with `workers` threads (or none). After each key's frame (measured in st), the frames the main
// loop runs before the next key: the workers' results and the pending work (at most 8; measured in bg).
static void win32_bench_grep_typing(Platform *p, String8 root, i32 workers, i64 line, i32 keys, BenchStat *st, BenchStat *bg, u64 *search_us,
                                    i32 *searches) {
    Event type = { .kind = EVENT_TEXT, .codepoint = 'x' };
    Event plain = { .kind = EVENT_KEY_DOWN, .key = KEY_X, .codepoint = 'x' };
    BenchStat skip = { 0 };
    app_dev_goto_line(p->app, line);
    win32_bench_view_step(p, plain, &skip);
    *search_us = 0;
    *searches = 0;
    b32 running = 0;
    // Without workers: a finished search stays shown in the other window (the same two windows to draw).
    if (!workers) win32_bench_grep_run(p, root, BENCH_GREP_COMMON, 8, 1000000);
    app_dev_results_work_max(p->app);
    for (i32 i = 0; i < keys; i++) {
        if (workers) {
            i64 found, bytes, files;
            u64 first, total;
            b32 limited;
            if (!running) {
                app_dev_bench_grep_start(p->app, root, str8_cstr(BENCH_GREP_COMMON), workers, 1000000);
                win32_bench_keys(p, "");
                running = 1;
            }
            if (os_dev_wake_wait(0)) app_wake(p->app);
            if (!app_dev_bench_grep_state(p->app, &found, &bytes, &files, &first, &total, &limited)) {
                *search_us += total;
                (*searches)++;
                running = 0;
                app_dev_bench_grep_close(p->app);
                continue;
            }
        }
        win32_bench_view_step(p, type, st);
        for (i32 k = 0; k < 8; k++) {
            b32 woke = os_dev_wake_wait(0);
            b32 frame = app_wants_frame(p->app);
            if (woke) frame |= app_wake(p->app);
            if (!frame) break;
            win32_bench_keys(p, "");
            win32_bench_record(p, bg);
        }
    }
    if (running) { // let it end before the next round
        i64 found, bytes, files;
        u64 first, total;
        b32 limited;
        while (app_dev_bench_grep_state(p->app, &found, &bytes, &files, &first, &total, &limited)) {
            if (os_dev_wake_wait(5)) app_wake(p->app);
            win32_bench_keys(p, "");
        }
        app_dev_bench_grep_close(p->app);
    }
    if (!workers) app_dev_bench_grep_close(p->app);
}

// ---------------------------------------------------------------------------
// --bench-compile: a --child program printing 100 MB as fast as it can into *compilation* (every 64th line an
// MSVC warning on a file that exists): the whole time, the frames, memory; typing at line 1,000,000 of the
// 100 MB file while it runs.

typedef struct BenchCompileRun {
    i64 bytes, lines, locations;
    u64 total_us, longest_us, frames, over, work_max_us, peak_private;
} BenchCompileRun;

static BenchCompileRun win32_bench_compile_run(Platform *p, String8 command, String8 dir, i32 cap_mb) {
    BenchCompileRun run = { 0 };
    app_dev_results_work_max(p->app);
    app_dev_bench_compile_start(p->app, command, dir, cap_mb);
    win32_bench_keys(p, "");
    u64 start = os_time_us();
    for (;;) {
        if (os_time_us() - start > 120 * 1000000ull) {
            LOG("bench-compile: a build over 120 s");
            break;
        }
        b32 woke = os_dev_wake_wait(app_wants_frame(p->app) ? 0 : 50);
        b32 frame = app_wants_frame(p->app);
        if (woke) frame |= app_wake(p->app);
        if (frame) {
            u64 b = win32_bench_keys(p, "");
            run.longest_us = MAX(run.longest_us, b);
            run.over += b > SYNTAX_FRAME_BUDGET_US + 100;
            run.frames++;
            if ((run.frames & 15) == 0) run.peak_private = MAX(run.peak_private, os_dev_private_bytes());
        }
        if (!app_dev_bench_compile_state(p->app, &run.bytes, &run.lines, &run.locations, &run.total_us)) break;
    }
    run.peak_private = MAX(run.peak_private, os_dev_private_bytes());
    run.work_max_us = app_dev_results_work_max(p->app);
    return run;
}

// Typing at `line` of the 100 MB file while builds run one after another (or with a finished one shown);
// the key frames in st, the frames between the keys (at most 8) in bg.
static void win32_bench_compile_typing(Platform *p, String8 command, String8 brief, String8 dir, b32 building, i64 line, i32 keys, BenchStat *st,
                                       BenchStat *bg, i32 *builds) {
    Event type = { .kind = EVENT_TEXT, .codepoint = 'x' };
    Event plain = { .kind = EVENT_KEY_DOWN, .key = KEY_X, .codepoint = 'x' };
    BenchStat skip = { 0 };
    app_dev_goto_line(p->app, line);
    win32_bench_view_step(p, plain, &skip);
    *builds = 0;
    b32 running = 0;
    if (!building) win32_bench_compile_run(p, brief, dir, 128); // the same two windows, nothing running
    app_dev_results_work_max(p->app);
    for (i32 i = 0; i < keys; i++) {
        if (building) {
            i64 bytes, lines, locations;
            u64 total;
            if (!running) {
                app_dev_bench_compile_start(p->app, command, dir, 128);
                win32_bench_keys(p, "");
                running = 1;
            }
            if (os_dev_wake_wait(0)) app_wake(p->app);
            if (!app_dev_bench_compile_state(p->app, &bytes, &lines, &locations, &total)) {
                (*builds)++;
                running = 0;
                continue;
            }
        }
        win32_bench_view_step(p, type, st);
        for (i32 k = 0; k < 8; k++) {
            b32 woke = os_dev_wake_wait(0);
            b32 frame = app_wants_frame(p->app);
            if (woke) frame |= app_wake(p->app);
            if (!frame) break;
            win32_bench_keys(p, "");
            win32_bench_record(p, bg);
        }
    }
    if (running) app_dev_bench_compile_close(p->app); // killed: the next round starts clean
    for (i32 k = 0; k < 2000 && app_dev_compile_busy(p->app); k++) {
        if (os_dev_wake_wait(5)) app_wake(p->app);
        win32_bench_keys(p, "");
    }
    app_dev_bench_compile_close(p->app);
}

static void win32_bench_compile(Platform *p) {
    r_dev_set_present_interval(p->renderer, 0);
    win32_bench_log_display(p);
    String8 dir = str8_fmt(&p->perm, "%S\\tmp\\p13bench", p->exe_dir);
    os_make_dir(dir);
    os_write_file(str8_fmt(&p->perm, "%S\\flood.c", dir), STR8_LIT("int x;\n"));
    String8 script = str8_fmt(&p->perm, "%S\\flood.txt", dir), brief_script = str8_fmt(&p->perm, "%S\\small.txt", dir);
    os_write_file(script, STR8_LIT("flood 104857600\nexit 0\n"));
    os_write_file(brief_script, STR8_LIT("flood 1048576\nexit 0\n"));
    String8 exe = os_dev_exe_path(&p->perm);
    String8 command = str8_fmt(&p->perm, "\"%S\" --child \"%S\"", exe, script);
    String8 brief = str8_fmt(&p->perm, "\"%S\" --child \"%S\"", exe, brief_script);
    u64 private0 = os_dev_private_bytes();
    win32_bench_compile_run(p, brief, dir, 128); // warm: the process start, the file cache
    app_dev_bench_compile_close(p->app);
    for (i32 k = 0; k < 2; k++) {
        i32 cap = k ? 16 : 128;
        BenchCompileRun best = { 0 };
        for (i32 rep = 0; rep < 3; rep++) {
            BenchCompileRun run = win32_bench_compile_run(p, command, dir, cap);
            if (!rep || run.total_us < best.total_us) best = run;
            app_dev_bench_compile_close(p->app);
        }
        LOG("bench-compile: 100 MB of output, compilation_max_output_mb = %d: *compilation* %D MB, %D lines, %D locations in %U us "
            "(%U MB/s of output); %U frames, the longest %U us (its results work at most %U us), %U over %d us; private bytes +%U KB at "
            "the peak (best of 3)", cap, best.bytes >> 20, best.lines, best.locations, best.total_us,
            best.total_us ? (u64)104857600 / best.total_us : 0, best.frames, best.longest_us, best.work_max_us, best.over,
            SYNTAX_FRAME_BUDGET_US + 100, best.peak_private > private0 ? (best.peak_private - private0) / 1024 : 0);
    }
    LOG("bench-compile: private bytes after the builds: %U KB (%D KB more than before the first)", os_dev_private_bytes() / 1024,
        ((i64)os_dev_private_bytes() - (i64)private0) / 1024);
    i64 line = 1000000;
    for (i32 round = 0; round < 2; round++) {
        for (i32 with = 0; with < 2; with++) {
            b32 building = (round == 0) == (with == 1);
            BenchStat st = { 0 }, bg = { 0 };
            i32 builds;
            win32_bench_compile_typing(p, command, brief, dir, building, line, 3000, &st, &bg, &builds);
            line += 2;
            win32_bench_log(p, "bench-compile", building ? "self-insert at line 1,000,000 while a build prints 100 MB (the key's frames)"
                                                          : "self-insert at line 1,000,000, no build (a finished *compilation* shown)", &st);
            if (building) {
                win32_bench_log(p, "bench-compile", "... the frames between the keys (the build's output)", &bg);
                LOG("bench-compile: ... the longest results work in one of those frames: %U us; %d builds ended meanwhile",
                    app_dev_results_work_max(p->app), builds);
            }
        }
    }
    r_dev_set_present_interval(p->renderer, 1);
}

static void win32_bench_grep(Platform *p) {
    r_dev_set_present_interval(p->renderer, 0);
    win32_bench_log_display(p);
    String8 root = win32_bench_grep_tree(p);
    String8 info = { 0 };
    os_read_small_file(&p->perm, str8_fmt(&p->perm, "%S\\tmp\\p12bench.done", p->exe_dir), 256, &info);
    LOG("bench-grep: the tree %S: %S (warm file cache after the first search); %d logical processors, automatic workers %d", root, info,
        os_cpu_count(), grep_auto_workers());
    u64 private0 = os_dev_private_bytes();
    win32_bench_grep_run(p, root, BENCH_GREP_MISSING, 4, 10000); // the project scan, the file cache
    app_dev_bench_grep_close(p->app);
    static const i32 workers[] = { 1, 2, 4, 8 };
    for (i32 k = 0; k < 2; k++) {
        const char *needle = k ? BENCH_GREP_MISSING : BENCH_GREP_COMMON;
        for (i32 w = 0; w < ARRAY_COUNT(workers); w++) {
            BenchGrepRun best = { 0 };
            for (i32 rep = 0; rep < 3; rep++) { // the best of three
                BenchGrepRun run = win32_bench_grep_run(p, root, needle, workers[w], 1000000);
                if (!rep || run.total_us < best.total_us) best = run;
                app_dev_bench_grep_close(p->app);
            }
            LOG("bench-grep: '%s', %d worker(s): %D matches in %D files, %D MB; first result %U us, all %U us (%U MB/s); %U frames, the "
                "longest %U us (its results work at most %U us), %U over %d us; private bytes +%U KB at the peak (best of 3)", needle, workers[w],
                best.found, best.files, best.bytes >> 20, best.first_us, best.total_us, best.total_us ? (u64)best.bytes / best.total_us : 0,
                best.frames, best.longest_us, best.work_max_us, best.over, SYNTAX_FRAME_BUDGET_US + 100, best.peak_private > private0 ? (best.peak_private - private0) / 1024 : 0);
        }
    }
    BenchGrepRun capped = win32_bench_grep_run(p, root, BENCH_GREP_COMMON, 0, 10000);
    LOG("bench-grep: '%s' at the default cap (10000), automatic workers: %D matches%s in %U us, first result %U us, the longest frame %U us",
        BENCH_GREP_COMMON, capped.found, capped.limited ? " (stopped)" : "", capped.total_us, capped.first_us, capped.longest_us);
    app_dev_bench_grep_close(p->app);
    LOG("bench-grep: private bytes after the searches: %U KB (%D KB more than before the first)", os_dev_private_bytes() / 1024,
        ((i64)os_dev_private_bytes() - (i64)private0) / 1024);

    // Typing at line 1,000,000 of the 100 MB file with *search* filling in the other window, against none.
    i64 line = 1000000;
    for (i32 round = 0; round < 2; round++) {
        for (i32 with = 0; with < 2; with++) {
            b32 searching = (round == 0) == (with == 1);
            BenchStat st = { 0 }, bg = { 0 };
            u64 search_us;
            i32 searches;
            win32_bench_grep_typing(p, root, searching ? 8 : 0, line, 3000, &st, &bg, &search_us, &searches);
            line += 2;
            win32_bench_log(p, "bench-grep", searching ? "self-insert at line 1,000,000 while 8 workers search (the key's frames)"
                                                       : "self-insert at line 1,000,000, no search (a finished *search* shown)", &st);
            if (searching) {
                win32_bench_log(p, "bench-grep", "... the frames between the keys (the search's results)", &bg);
                LOG("bench-grep: ... the longest results work in one of those frames: %U us", app_dev_results_work_max(p->app));
                LOG("bench-grep: %d searches ended during those frames, %U us each on average", searches, searches ? search_us / (u64)searches : 0);
            }
        }
    }
    // The workers' priority: one step below normal (kept) against normal, alternated.
    for (i32 round = 0; round < 2; round++) {
        for (i32 k = 0; k < 2; k++) {
            b32 normal = (round == 0) == (k == 1);
            os_dev_background_normal(normal);
            BenchStat st = { 0 }, bg = { 0 };
            u64 search_us;
            i32 searches;
            win32_bench_grep_typing(p, root, 8, line, 3000, &st, &bg, &search_us, &searches);
            line += 2;
            win32_bench_log(p, "bench-grep", normal ? "priority: workers at normal priority, typing" : "priority: workers below normal, typing", &st);
            LOG("bench-grep: priority %s: %d searches, %U us each on average", normal ? "normal" : "below normal", searches,
                searches ? search_us / (u64)searches : 0);
        }
    }
    os_dev_background_normal(0);
    r_dev_set_present_interval(p->renderer, 1);
}

// --idle-check: after a second to settle, five seconds without input: the process's CPU time, the
// frames drawn and the main loop's passes over them, and the private bytes. Two posted messages mark
// the period (the only wakeups it should have); the window is shown without taking the focus.
#define IDLE_SETTLE_MS 1000
#define IDLE_PERIOD_MS 5000

static u64 win32_dev_cpu_us(void) {
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    return (win32_filetime_u64(kernel) + win32_filetime_u64(user)) / 10;
}

static DWORD WINAPI win32_dev_idle_thread(void *param) {
    HWND hwnd = param;
    Sleep(IDLE_SETTLE_MS);
    PostMessageW(hwnd, WM_APP + 1, 0, 0);
    Sleep(IDLE_PERIOD_MS);
    PostMessageW(hwnd, WM_APP + 2, 0, 0);
    return 0;
}

static void win32_dev_idle_check_start(Platform *p) {
    HANDLE thread = CreateThread(NULL, 0, win32_dev_idle_thread, p->hwnd, 0, NULL);
    if (thread) CloseHandle(thread);
    else p->quit = 1, p->exit_code = EXIT_TEST;
}

static void win32_dev_idle_check_mark(Platform *p, b32 end) {
    if (!end) {
        p->idle_start_cpu = win32_dev_cpu_us();
        p->idle_start_frames = p->frame_count;
        p->idle_start_wakeups = p->idle_wakeups;
        p->idle_measuring = 1;
        return;
    }
    p->idle_measuring = 0;
    u64 cpu = win32_dev_cpu_us() - p->idle_start_cpu;
    i64 frames = p->frame_count - p->idle_start_frames;
    u64 wakeups = p->idle_wakeups - p->idle_start_wakeups;
    u64 bytes = os_dev_private_bytes();
    // 0.5% of one core over the period is what Task Manager still shows as 0.
    b32 ok = frames == 0 && cpu * 200 < (u64)IDLE_PERIOD_MS * 1000;
    LOG("idle-check: %s: %U ms idle: CPU %U us (%U.%02U%% of one core), %D frame(s), %U main loop pass(es) besides the two marks; "
        "private bytes %U KB (%U MB)", ok ? "PASS" : "FAIL", (u64)IDLE_PERIOD_MS, cpu, cpu / (IDLE_PERIOD_MS * 10),
        cpu * 100 / (IDLE_PERIOD_MS * 10) % 100, frames, wakeups > 1 ? wakeups - 1 : 0, bytes / 1024, bytes / (1024 * 1024));
    u8 others[128];
    i64 n = 0;
    for (u32 i = 0; i < MIN(p->idle_other_msgs, (u32)ARRAY_COUNT(p->idle_other)); i++) {
        n += fmt_buf(others + n, (i64)sizeof(others) - n, "%s0x%x", i ? " " : "", p->idle_other[i]);
    }
    LOG("idle-check: messages meanwhile: %u from the mouse (the pointer over the window), %u other%s%S%s", p->idle_mouse_msgs,
        p->idle_other_msgs, n ? " (" : "", str8(others, n), n ? ")" : "");
    if (!ok && (p->idle_mouse_msgs || p->idle_other_msgs)) {
        LOG("idle-check: the period was not idle (input reached the window): run it again without touching the mouse or keyboard");
    }
    p->exit_code = ok ? EXIT_OK : EXIT_TEST;
    p->quit = 1;
}

// --startup-capture: what the screen shows of the window from the process's start on. A thread started at the top of
// WinMain waits for every DWM composition (DwmFlush) and copies the window's client area and its title bar from the
// screen (the window is placed topmost, never activated); for each composition: the time since process creation,
// whether the window is shown, whether its client area is one color (and which), a hash of the client area and
// one of the title bar. It stops once the same client image has stood for 20 compositions (or after 600). The main
// thread reads them: when the window was first visible, when the first frame appeared, and any client image that
// was neither the background nor the final frame (a flash), with the share of its pixels that are the background or
// the final frame's. No image is written: until the window is opaque, what the screen shows there is other windows'.
// Exit 9 on a flash. Run it with a --config that sets early_window, and with one that does not.
#define CAPTURE_MAX 600
#define CAPTURE_STABLE 20
#define CAPTURE_IMAGES 8

typedef struct StartupCapture {
    i32 count;
    u64 us[CAPTURE_MAX];
    b32 shown[CAPTURE_MAX];
    b32 uniform[CAPTURE_MAX];
    u32 color[CAPTURE_MAX];      // the uniform color, 0xRRGGBB
    u64 hash[CAPTURE_MAX];       // the client area
    u64 title_hash[CAPTURE_MAX]; // the title bar
    // The first copy of each distinct client image (up to CAPTURE_IMAGES) and the last one, kept in memory only.
    u8 *final;
    i32 final_w, final_h;
    i32 image_count;
    u64 image_us[CAPTURE_IMAGES];
    u64 image_hash[CAPTURE_IMAGES];
    i32 image_w[CAPTURE_IMAGES], image_h[CAPTURE_IMAGES];
    u8 *image[CAPTURE_IMAGES];
} StartupCapture;

static u64 win32_capture_hash(u8 *px, i64 n) {
    u64 h = 1469598103934665603ull;
    for (i64 i = 0; i < n; i += 4) h = (h ^ (u64)(*(u32 *)(px + i) & 0xFFFFFF)) * 1099511628211ull;
    return h;
}

static DWORD WINAPI win32_dev_capture_thread(void *param) {
    Platform *p = param;
    StartupCapture *c = p->capture;
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    u64 created_us = win32_filetime_u64(created) / 10;
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll"); // not an import (win32_dwm_attribute)
    HRESULT (WINAPI *dwm_flush)(void) = dwm ? (HRESULT (WINAPI *)(void))(void *)GetProcAddress(dwm, "DwmFlush") : NULL;
    HRESULT (WINAPI *dwm_get)(HWND, DWORD, PVOID, DWORD) =
        dwm ? (HRESULT (WINAPI *)(HWND, DWORD, PVOID, DWORD))(void *)GetProcAddress(dwm, "DwmGetWindowAttribute") : NULL;
    if (!dwm_flush || !dwm_get) return 0;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    HBITMAP dib = NULL;
    u8 *bits = NULL;
    i32 dib_w = 0, dib_h = 0, stable = 0;
    while (c->count < CAPTURE_MAX && stable < CAPTURE_STABLE) {
        dwm_flush();
        FILETIME now;
        GetSystemTimePreciseAsFileTime(&now);
        i32 k = c->count++;
        c->us[k] = win32_filetime_u64(now) / 10 - created_us;
        HWND hwnd = *(HWND volatile *)&p->hwnd;
        BOOL cloaked = 0;
        if (hwnd) dwm_get(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (!hwnd || !IsWindowVisible(hwnd) || cloaked) continue;
        RECT wr, cr;
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        POINT origin = { 0, 0 };
        ClientToScreen(hwnd, &origin);
        i32 w = cr.right, h = cr.bottom, title_h = MAX((i32)(origin.y - wr.top), 1), full_w = wr.right - wr.left;
        if (w <= 0 || h <= 0) continue;
        i32 need_w = MAX(w, full_w), need_h = h + title_h;
        if (need_w != dib_w || need_h != dib_h) {
            if (dib) DeleteObject(dib);
            BITMAPINFO bi = { .bmiHeader = { .biSize = sizeof(BITMAPINFOHEADER), .biWidth = need_w, .biHeight = -need_h, .biPlanes = 1,
                                             .biBitCount = 32, .biCompression = BI_RGB } };
            dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, (void **)&bits, NULL, 0);
            if (!dib) break;
            SelectObject(mem, dib);
            dib_w = need_w;
            dib_h = need_h;
        }
        BitBlt(mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY);                         // the client area at the top
        BitBlt(mem, 0, h, full_w, title_h, screen, wr.left, wr.top, SRCCOPY);               // the title bar below it
        GdiFlush();
        c->shown[k] = 1;
        // The client area's rows are dib_w wide: hash row by row over w.
        u64 hash = 1469598103934665603ull;
        u32 first = *(u32 *)bits & 0xFFFFFF;
        b32 uniform = 1;
        for (i32 y = 0; y < h; y++) {
            u8 *row = bits + (i64)y * dib_w * 4;
            hash ^= win32_capture_hash(row, (i64)w * 4);
            hash *= 1099511628211ull;
            for (i32 x = 0; x < w && uniform; x += 7) {
                b32 corner = y >= h - 16 && (x < 16 || x >= w - 16); // Windows 11 rounds the window's bottom corners
                uniform = corner || (*(u32 *)(row + x * 4) & 0xFFFFFF) == first;
            }
        }
        c->hash[k] = hash;
        c->uniform[k] = uniform;
        c->color[k] = first;
        u64 th = 0;
        for (i32 y = h; y < h + title_h; y++) th = th * 31 + win32_capture_hash(bits + (i64)y * dib_w * 4, (i64)full_w * 4);
        c->title_hash[k] = th;
        stable = k > 0 && c->shown[k - 1] && c->hash[k - 1] == hash ? stable + 1 : 0;
        b32 seen = 0;
        for (i32 i = 0; i < c->image_count && !seen; i++) seen = c->image_hash[i] == hash;
        if (!seen && c->image_count < CAPTURE_IMAGES) {
            i32 i = c->image_count++;
            c->image_hash[i] = hash;
            c->image_us[i] = c->us[k];
            c->image_w[i] = w;
            c->image_h[i] = h;
            c->image[i] = VirtualAlloc(NULL, (SIZE_T)w * h * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
            if (c->image[i]) for (i32 y = 0; y < h; y++) memcpy(c->image[i] + (i64)y * w * 4, bits + (i64)y * dib_w * 4, (size_t)w * 4);
        }
    }
    if (bits && dib_w) { // the last image: the final frame
        i32 w = dib_w, h = dib_h;
        for (i32 i = 0; i < c->image_count; i++) if (c->image_hash[i] == c->hash[c->count - 1]) w = c->image_w[i], h = c->image_h[i];
        c->final = VirtualAlloc(NULL, (SIZE_T)w * h * 4, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (c->final) {
            for (i32 y = 0; y < h; y++) memcpy(c->final + (i64)y * w * 4, bits + (i64)y * dib_w * 4, (size_t)w * 4);
            c->final_w = w;
            c->final_h = h;
        }
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(NULL, screen);
    HWND hwnd = *(HWND volatile *)&p->hwnd;
    if (hwnd) PostMessageW(hwnd, WM_APP + 6, 0, 0);
    return 0;
}

static void win32_dev_capture_finish(Platform *p) {
    StartupCapture *c = p->capture;
    u32 bg = p->app ? p->app->config->theme.background : 0;
    i32 n = c->count, visible = -1, first_frame = -1, flashes = 0, titles = 0;
    u64 final_hash = n ? c->hash[n - 1] : 0, last_title = 0;
    for (i32 k = 0; k < n; k++) {
        if (!c->shown[k]) continue;
        if (visible < 0) visible = k;
        if (first_frame < 0 && c->hash[k] == final_hash) first_frame = k;
        b32 background = c->uniform[k] && c->color[k] == bg;
        if (c->hash[k] != final_hash && !(background && first_frame < 0)) flashes++;
        if (c->title_hash[k] != last_title) titles++;
        last_title = c->title_hash[k];
    }
    i32 between = visible >= 0 && first_frame >= 0 ? first_frame - visible : -1;
    LOG("startup-capture: %d compositions; early_window %d; the window first visible %U.%U ms after process creation (%s), the "
        "first frame on screen %U.%U ms; %d composition(s) between; %d composition(s) showing something else (a flash); title bar "
        "images %d", n, p->app ? p->app->config->settings.early_window : -1, visible >= 0 ? c->us[visible] / 1000 : 0,
        visible >= 0 ? c->us[visible] / 100 % 10 : 0, visible >= 0 && c->uniform[visible] ? (c->color[visible] == bg ? "the background" : "one other color") : "a frame",
        first_frame >= 0 ? c->us[first_frame] / 1000 : 0, first_frame >= 0 ? c->us[first_frame] / 100 % 10 : 0, between, flashes, titles);
    for (i32 i = 0; i < c->image_count; i++) {
        b32 background = 0;
        for (i32 k = 0; k < n; k++) {
            if (c->shown[k] && c->hash[k] == c->image_hash[i]) background = c->uniform[k] && c->color[k] == bg;
        }
        if (c->image_hash[i] == final_hash || background || !c->image[i]) continue;
        i64 bg_px = 0, final_px = 0, total = (i64)c->image_w[i] * c->image_h[i];
        i32 x0 = c->image_w[i], y0 = c->image_h[i], x1 = -1, y1 = -1;
        u32 other = 0;
        b32 same_size = c->final && c->final_w == c->image_w[i] && c->final_h == c->image_h[i];
        for (i64 k = 0; k < total; k++) {
            u32 px = ((u32 *)c->image[i])[k] & 0xFFFFFF;
            bg_px += px == bg;
            final_px += same_size && px == (((u32 *)c->final)[k] & 0xFFFFFF);
            if (px != bg) { // where the rest is
                i32 x = (i32)(k % c->image_w[i]), y = (i32)(k / c->image_w[i]);
                x0 = MIN(x0, x), y0 = MIN(y0, y), x1 = MAX(x1, x), y1 = MAX(y1, y);
                other = px;
            }
        }
        // Where the other pixels are, on a 16 x 10 grid: a digit per cell, 0 none, 9 all (no image is kept).
        u8 grid[10 * 17 + 1];
        i32 gn = 0;
        for (i32 gy = 0; gy < 10; gy++) {
            for (i32 gx = 0; gx < 16; gx++) {
                i32 cx0 = gx * c->image_w[i] / 16, cx1 = (gx + 1) * c->image_w[i] / 16, cy0 = gy * c->image_h[i] / 10, cy1 = (gy + 1) * c->image_h[i] / 10;
                i64 cell = 0, cell_total = MAX((i64)(cx1 - cx0) * (cy1 - cy0), 1);
                for (i32 y = cy0; y < cy1; y++) {
                    for (i32 x = cx0; x < cx1; x++) cell += (((u32 *)c->image[i])[(i64)y * c->image_w[i] + x] & 0xFFFFFF) != bg;
                }
                grid[gn++] = (u8)('0' + (cell ? 1 + cell * 8 / cell_total : 0));
            }
            grid[gn++] = '/';
        }
        LOG("startup-capture:   not the background, by area: %S", str8(grid, gn - 1));
        LOG("startup-capture: a flash at %U.%U ms: %D%% of its pixels the background, %D%% the final frame's; the others within "
            "(%d,%d)-(%d,%d) of %dx%d, one of them #%06x", c->image_us[i] / 1000, c->image_us[i] / 100 % 10, bg_px * 100 / MAX(total, 1),
            final_px * 100 / MAX(total, 1), x0, y0, x1, y1, c->image_w[i], c->image_h[i], other);
    }
    for (i32 i = 0; i < c->image_count; i++) if (c->image[i]) VirtualFree(c->image[i], 0, MEM_RELEASE);
    if (c->final) VirtualFree(c->final, 0, MEM_RELEASE);
    p->exit_code = flashes || first_frame < 0 ? EXIT_TEST : EXIT_OK;
}

// --bench-latency: typing as the screen sees it. The window is shown without activation (as --idle-check); after
// the settle a thread posts BENCH_LATENCY_KEYS key presses to it (WM_KEYDOWN / WM_KEYUP through the normal pump,
// TranslateMessage making the WM_CHAR), 25-55 ms apart so they land anywhere in the refresh. For each key: its
// arrival (just before PostMessage), the dequeue, command + frame build, Present returning, and the vertical blank
// that showed its frame (DXGI frame statistics: an estimate, the scan-out of that refresh follows it).
#define BENCH_LATENCY_KEYS 500

typedef struct LatencyBench {
    i64 post[BENCH_LATENCY_KEYS], dequeue[BENCH_LATENCY_KEYS], presented[BENCH_LATENCY_KEYS], shown[BENCH_LATENCY_KEYS];
    u64 build_us[BENCH_LATENCY_KEYS];
    u32 present_count[BENCH_LATENCY_KEYS];
    const char *mode[BENCH_LATENCY_KEYS]; // how DWM showed it (r_dev_presentation_mode)
    i32 current; // the key whose frame is due (-1: none)
} LatencyBench;

static DWORD WINAPI win32_dev_latency_thread(void *param) {
    Platform *p = param;
    LatencyBench *b = p->latency;
    Sleep(1000);
    u64 rng = 0x1a7e;
    for (i32 i = 0; i < BENCH_LATENCY_KEYS; i++) {
        rng = rng * 6364136223846793005ull + 1442695040888963407ull;
        Sleep(25 + (DWORD)((rng >> 33) % 31));
        UINT vk = i % 40 == 39 ? VK_RETURN : 'A' + (UINT)(i % 26);
        LPARAM lp = 1 | ((LPARAM)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC) << 16);
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        b->post[i] = t.QuadPart;
        PostMessageW(p->hwnd, WM_APP + 4, (WPARAM)i, 0);
        PostMessageW(p->hwnd, WM_KEYDOWN, vk, lp);
        PostMessageW(p->hwnd, WM_KEYUP, vk, lp | ((LPARAM)1 << 30) | ((LPARAM)1 << 31));
    }
    Sleep(300);
    PostMessageW(p->hwnd, WM_APP + 5, 0, 0);
    return 0;
}

static void win32_dev_latency_shown(Platform *p, i32 key) {
    LatencyBench *b = p->latency;
    u32 count;
    u64 qpc;
    if (key >= 0 && b->present_count[key] && r_dev_frame_shown(p->renderer, &count, &qpc) && count == b->present_count[key]) {
        b->shown[key] = (i64)qpc;
        b->mode[key] = r_dev_presentation_mode(p->renderer);
    }
}

static void win32_dev_latency_key(Platform *p, i32 key) {
    LatencyBench *b = p->latency;
    win32_dev_latency_shown(p, key - 1); // the key before: its frame is on screen by now
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    b->dequeue[key] = t.QuadPart;
    b->current = key;
}

static void win32_dev_latency_frame(Platform *p) {
    LatencyBench *b = p->latency;
    if (!b || b->current < 0) return;
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    b->presented[b->current] = t.QuadPart;
    b->build_us[b->current] = app_dev_build_us(p->app);
    b->present_count[b->current] = r_dev_last_present_count(p->renderer);
    b->current = -1;
}

static void win32_dev_latency_stat(Platform *p, const char *what, u64 *us, i32 n) {
    for (i32 i = 1; i < n; i++) { // insertion sort: a few hundred values
        u64 x = us[i];
        i32 j = i;
        while (j > 0 && us[j - 1] > x) {
            us[j] = us[j - 1];
            j--;
        }
        us[j] = x;
    }
    (void)p;
    if (!n) {
        LOG("bench-latency: %s: no samples", what);
        return;
    }
    LOG("bench-latency: %s (%d keys): min %U us, p50 %U us, p90 %U us, p99 %U us, max %U us", what, n, us[0], us[n / 2], us[n * 9 / 10],
        us[n * 99 / 100], us[n - 1]);
}

static void win32_dev_latency_finish(Platform *p) {
    LatencyBench *b = p->latency;
    win32_dev_latency_shown(p, BENCH_LATENCY_KEYS - 1);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    u64 *v[5];
    i32 n[5] = { 0 };
    for (i32 k = 0; k < 5; k++) v[k] = PUSH_ARRAY(&p->scratch, u64, BENCH_LATENCY_KEYS);
    for (i32 i = 0; i < BENCH_LATENCY_KEYS; i++) {
        if (!b->presented[i] || !b->post[i]) continue;
        v[0][n[0]++] = (u64)(b->dequeue[i] - b->post[i]) * 1000000 / (u64)f.QuadPart;
        v[1][n[1]++] = b->build_us[i];
        v[2][n[2]++] = (u64)(b->presented[i] - b->post[i]) * 1000000 / (u64)f.QuadPart;
        if (b->shown[i] > b->presented[i]) {
            v[3][n[3]++] = (u64)(b->shown[i] - b->presented[i]) * 1000000 / (u64)f.QuadPart;
            v[4][n[4]++] = (u64)(b->shown[i] - b->post[i]) * 1000000 / (u64)f.QuadPart;
        }
    }
    win32_bench_log_display(p);
    LOG("bench-latency: presentation %s, Present(%u, 0)", r_dev_presentation_mode(p->renderer), p->renderer->present_interval);
    win32_dev_latency_stat(p, "arrival to dequeue", v[0], n[0]);
    win32_dev_latency_stat(p, "command + frame build", v[1], n[1]);
    win32_dev_latency_stat(p, "arrival to Present returned", v[2], n[2]);
    win32_dev_latency_stat(p, "Present returned to the vertical blank that showed it", v[3], n[3]);
    win32_dev_latency_stat(p, "arrival to the vertical blank that showed it (on screen)", v[4], n[4]);
    // The same by presentation mode: DWM composes the window at first and may promote it to an overlay plane later.
    const char *modes[4] = { 0 };
    i32 mode_count = 0;
    for (i32 i = 0; i < BENCH_LATENCY_KEYS; i++) {
        if (!b->mode[i]) continue;
        i32 k = 0;
        while (k < mode_count && modes[k] != b->mode[i]) k++;
        if (k == mode_count && mode_count < 4) modes[mode_count++] = b->mode[i];
    }
    for (i32 k = 0; k < mode_count; k++) {
        i32 m = 0;
        for (i32 i = 0; i < BENCH_LATENCY_KEYS; i++) {
            if (b->mode[i] == modes[k] && b->shown[i] > b->presented[i]) v[4][m++] = (u64)(b->shown[i] - b->post[i]) * 1000000 / (u64)f.QuadPart;
        }
        u8 label[96];
        fmt_buf(label, sizeof(label), "on screen, %s%c", modes[k], 0);
        win32_dev_latency_stat(p, (const char *)label, v[4], m);
    }
    p->exit_code = n[2] == BENCH_LATENCY_KEYS ? EXIT_OK : EXIT_TEST;
}

// --check-imports EXE LIST: the DLLs EXE imports (its PE import directory, read here: no dumpbin) must be exactly
// those of LIST (one name a line, '#' comments, case ignored), and never a CRT DLL (ucrtbase, vcruntime*,
// api-ms-win-crt-*). Logs every DLL with its function count. Exit 9 on any difference.
static u32 win32_dev_rva_offset(u8 *file, i64 size, IMAGE_NT_HEADERS64 *nt, u32 rva) {
    IMAGE_SECTION_HEADER *s = IMAGE_FIRST_SECTION(nt);
    for (u32 i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (rva >= s[i].VirtualAddress && rva < s[i].VirtualAddress + MAX(s[i].Misc.VirtualSize, s[i].SizeOfRawData)) {
            u32 off = rva - s[i].VirtualAddress + s[i].PointerToRawData;
            return off < (u64)size ? off : 0;
        }
    }
    (void)file;
    return 0;
}

static b32 win32_dev_name_is(String8 a, String8 b) { // ASCII, case ignored
    if (a.len != b.len) return 0;
    for (i64 i = 0; i < a.len; i++) {
        u8 x = a.data[i], y = b.data[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

static i32 win32_dev_check_imports(Platform *p, String8 exe, String8 list_path) {
    Arena *a = &p->scratch;
    String8 file, list;
    if (os_read_small_file(a, exe, MB(64), &file) != OS_FILE_OK || os_read_small_file(a, list_path, MB(1), &list) != OS_FILE_OK) {
        LOG("check-imports: FAIL: cannot read %S or %S", exe, list_path);
        return EXIT_TEST;
    }
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)file.data;
    if (file.len < (i64)sizeof(*dos) || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew + (i64)sizeof(IMAGE_NT_HEADERS64) > file.len) {
        LOG("check-imports: FAIL: %S is not a PE file", exe);
        return EXIT_TEST;
    }
    IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(file.data + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    String8 found[64];
    i32 found_count = 0, functions = 0;
    b32 ok = 1;
    for (u32 off = win32_dev_rva_offset(file.data, file.len, nt, dir.VirtualAddress); off && off + sizeof(IMAGE_IMPORT_DESCRIPTOR) <= (u64)file.len;
         off += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(file.data + off);
        if (!d->Name) break;
        u32 name_off = win32_dev_rva_offset(file.data, file.len, nt, d->Name);
        String8 name = str8_cstr((const char *)file.data + name_off);
        i32 count = 0;
        for (u32 t = win32_dev_rva_offset(file.data, file.len, nt, d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk);
             t && t + 8 <= (u64)file.len && *(u64 *)(file.data + t); t += 8) {
            count++;
        }
        functions += count;
        LOG("check-imports: %S: %d function(s)", name, count);
        if (found_count < (i32)ARRAY_COUNT(found)) found[found_count++] = name;
        String8 lower = str8_copy(a, name);
        for (i64 i = 0; i < lower.len; i++) if (lower.data[i] >= 'A' && lower.data[i] <= 'Z') lower.data[i] += 32;
        if (str8_starts_with(lower, STR8_LIT("ucrtbase")) || str8_starts_with(lower, STR8_LIT("vcruntime")) ||
            str8_starts_with(lower, STR8_LIT("api-ms-win-crt-"))) {
            LOG("check-imports: FAIL: %S is a CRT DLL", name);
            ok = 0;
        }
    }
    // The expected list, one name a line.
    i32 expected = 0;
    for (i64 i = 0; i < list.len;) {
        i64 end = i;
        while (end < list.len && list.data[end] != '\n') end++;
        i64 s = i, e = end;
        while (s < e && (list.data[s] == ' ' || list.data[s] == '\t')) s++;
        while (e > s && (list.data[e - 1] == ' ' || list.data[e - 1] == '\t' || list.data[e - 1] == '\r')) e--;
        i = end + 1;
        if (e == s || list.data[s] == '#') continue;
        String8 want = str8(list.data + s, e - s);
        expected++;
        b32 there = 0;
        for (i32 k = 0; k < found_count && !there; k++) there = win32_dev_name_is(found[k], want);
        if (!there) {
            LOG("check-imports: FAIL: %S is in %S but not imported", want, list_path);
            ok = 0;
        }
    }
    if (found_count != expected) {
        LOG("check-imports: FAIL: %d DLLs imported, %d listed", found_count, expected);
        ok = 0;
    }
    LOG("check-imports: %s: %S imports %d DLLs, %d functions", ok ? "PASS" : "FAIL", exe, found_count, functions);
    return ok ? EXIT_OK : EXIT_TEST;
}

// --memory-report: shown like --idle-check, then after the settle the committed memory that counts as private
// bytes, by owner: our arenas (every os_reserve range), the process heaps (Windows, DirectWrite, the D3D runtime,
// the CRT), thread stacks (allocations with a guard page), writable image pages, writable mapped views, and the
// other private allocations (the D3D11 user-mode driver and the runtime's own VirtualAllocs).
#define MEMORY_SETTLE_MS 3000

static DWORD WINAPI win32_dev_memory_thread(void *param) {
    Sleep(MEMORY_SETTLE_MS);
    PostMessageW((HWND)param, WM_APP + 3, 0, 0);
    return 0;
}

typedef struct Win32DevRange {
    u8 *start, *end;
} Win32DevRange;

static b32 win32_dev_in_ranges(Win32DevRange *ranges, i32 count, u8 *p) {
    for (i32 i = 0; i < count; i++) if (p >= ranges[i].start && p < ranges[i].end) return 1;
    return 0;
}

static void win32_dev_memory_report(Platform *p) {
    Arena *scratch = &p->scratch;
    u64 mark = arena_pos(scratch);
    // Our reservations, as ranges.
    Win32DevReservations *r = &win32_dev_reservations;
    Win32DevRange *ours = PUSH_ARRAY(scratch, Win32DevRange, ARRAY_COUNT(r->base));
    i32 our_count = 0;
    AcquireSRWLockShared(&r->lock);
    for (i32 i = 0; i < r->count; i++) {
        MEMORY_BASIC_INFORMATION m;
        u8 *base = r->base[i], *end = base;
        while (VirtualQuery(end, &m, sizeof(m)) && m.AllocationBase == base) end = (u8 *)m.BaseAddress + m.RegionSize;
        ours[our_count++] = (Win32DevRange){ base, end };
    }
    ReleaseSRWLockShared(&r->lock);
    // The heaps' regions and their large blocks.
    HANDLE heaps[256];
    DWORD heap_count = GetProcessHeaps(ARRAY_COUNT(heaps), heaps);
    Win32DevRange *heap = PUSH_ARRAY(scratch, Win32DevRange, 65536);
    i32 heap_ranges = 0;
    for (DWORD h = 0; h < MIN(heap_count, (DWORD)ARRAY_COUNT(heaps)); h++) {
        if (!HeapLock(heaps[h])) continue;
        PROCESS_HEAP_ENTRY e = { 0 };
        while (HeapWalk(heaps[h], &e) && heap_ranges < 65536) {
            if (e.wFlags & PROCESS_HEAP_REGION) {
                heap[heap_ranges++] = (Win32DevRange){ (u8 *)e.lpData, (u8 *)e.Region.lpLastBlock };
            } else if ((e.wFlags & PROCESS_HEAP_ENTRY_BUSY) && e.cbData >= KB(512)) { // a large block, VirtualAlloc'd by the heap
                heap[heap_ranges++] = (Win32DevRange){ (u8 *)e.lpData, (u8 *)e.lpData + e.cbData };
            }
        }
        HeapUnlock(heaps[h]);
    }
    // Allocations that hold a guard page: thread stacks.
    void **stacks = PUSH_ARRAY(scratch, void *, 4096);
    i32 stack_count = 0;
    MEMORY_BASIC_INFORMATION m;
    for (u8 *a = 0; VirtualQuery(a, &m, sizeof(m)); a = (u8 *)m.BaseAddress + m.RegionSize) {
        if (m.State != MEM_COMMIT || !(m.Protect & PAGE_GUARD) || stack_count >= 4096) continue;
        b32 seen = 0;
        for (i32 s = 0; s < stack_count && !seen; s++) seen = stacks[s] == m.AllocationBase;
        if (!seen) stacks[stack_count++] = m.AllocationBase;
        if ((u8 *)m.BaseAddress + m.RegionSize < a) break;
    }
    u64 arenas = 0, heaps_b = 0, stack_b = 0, other = 0, image_w = 0, mapped_w = 0, other_regions = 0, other_largest = 0;
    // The modules with the most writable image pages, and our reservations with the most committed.
    struct { void *base; u64 bytes; } mods[64] = { 0 }, res[64] = { 0 };
    i32 mod_count = 0, res_count = 0;
    for (u8 *a = 0; VirtualQuery(a, &m, sizeof(m)); a = (u8 *)m.BaseAddress + m.RegionSize) {
        if ((u8 *)m.BaseAddress + m.RegionSize < a) break;
        if (m.State != MEM_COMMIT) continue;
        b32 writable = (m.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
        if (m.Type == MEM_IMAGE) {
            if (!writable) continue;
            image_w += m.RegionSize;
            i32 k = 0;
            while (k < mod_count && mods[k].base != m.AllocationBase) k++;
            if (k == mod_count && mod_count < 64) mods[mod_count++].base = m.AllocationBase;
            if (k < 64) mods[k].bytes += m.RegionSize;
        } else if (m.Type == MEM_MAPPED) {
            if (writable) mapped_w += m.RegionSize;
        } else if (win32_dev_in_ranges(ours, our_count, (u8 *)m.BaseAddress)) {
            arenas += m.RegionSize;
            i32 k = 0;
            while (k < res_count && res[k].base != m.AllocationBase) k++;
            if (k == res_count && res_count < 64) res[res_count++].base = m.AllocationBase;
            if (k < 64) res[k].bytes += m.RegionSize;
        } else if (win32_dev_in_ranges(heap, heap_ranges, (u8 *)m.BaseAddress)) {
            heaps_b += m.RegionSize;
        } else {
            b32 stack = 0;
            for (i32 s = 0; s < stack_count && !stack; s++) stack = stacks[s] == m.AllocationBase;
            if (stack) {
                stack_b += m.RegionSize;
            } else {
                other += m.RegionSize;
                other_regions++;
                other_largest = MAX(other_largest, (u64)m.RegionSize);
            }
        }
    }
    u64 bytes = os_dev_private_bytes();
    LOG("memory-report: private bytes %U KB (%U MB), %d threads alive of ours", bytes >> 10, bytes >> 20, os_dev_live_threads());
    LOG("memory-report: | owner | KB |");
    LOG("memory-report: | our arenas (%d reservations) | %U |", our_count, arenas >> 10);
    LOG("memory-report: | process heaps (%u: Windows, DirectWrite, the D3D runtime, the CRT) | %U |", heap_count, heaps_b >> 10);
    LOG("memory-report: | thread stacks (%d) | %U |", stack_count, stack_b >> 10);
    LOG("memory-report: | other private allocations (the D3D11 driver and runtime; %U regions, the largest %U KB) | %U |", other_regions,
        other_largest >> 10, other >> 10);
    LOG("memory-report: | writable image pages (DLL and exe data; copy-on-write, charged in full) | %U |", image_w >> 10);
    LOG("memory-report: | writable mapped views (shared sections, the GDI DIB) | %U |", mapped_w >> 10);
    LOG("memory-report: sum of the rows %U KB against private bytes %U KB", (arenas + heaps_b + stack_b + other + image_w + mapped_w) >> 10,
        bytes >> 10);
    for (i32 pass = 0; pass < 2; pass++) { // the largest first, 8 of each
        for (i32 shown = 0; shown < 8; shown++) {
            i32 best = -1;
            for (i32 k = 0; k < (pass ? res_count : mod_count); k++) {
                u64 b = pass ? res[k].bytes : mods[k].bytes;
                if (b && (best < 0 || b > (pass ? res[best].bytes : mods[best].bytes))) best = k;
            }
            if (best < 0) break;
            if (pass) {
                u64 reserved = 0;
                for (i32 k = 0; k < our_count; k++) if (ours[k].start == res[best].base) reserved = (u64)(ours[k].end - ours[k].start);
                LOG("memory-report:   our reservation at %X (%U MB reserved): %U KB committed", (u64)(uintptr_t)res[best].base, reserved >> 20,
                    res[best].bytes >> 10);
                res[best].bytes = 0;
            } else {
                WCHAR name[MAX_PATH];
                DWORD n = GetModuleFileNameW((HMODULE)mods[best].base, name, MAX_PATH);
                LOG("memory-report:   writable image pages of %S: %U KB", str8_from_str16(scratch, (u16 *)name, n), mods[best].bytes >> 10);
                mods[best].bytes = 0;
            }
        }
    }
    arena_pop_to(scratch, mark);
}

// --clipboard-check: the real clipboard through the app. Only when the clipboard holds plain text or
// nothing (what can be put back exactly); its text is saved first and put back afterwards. Everything
// written carries the format that keeps it out of clipboard history. kill-ring-save of two lines must
// give CRLF on the clipboard; another program's CRLF text must yank as LF lines.
static void win32_dev_clip_put(Platform *p, const u16 *text, i64 len) {
    if (!win32_open_clipboard(p)) return;
    EmptyClipboard();
    if (text) {
        HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (SIZE_T)(len + 1) * 2);
        u16 *out = h ? (u16 *)GlobalLock(h) : NULL;
        if (out) {
            memcpy(out, text, (size_t)len * 2);
            out[len] = 0;
            GlobalUnlock(h);
            if (!SetClipboardData(CF_UNICODETEXT, h)) GlobalFree(h);
            else win32_dev_clip_exclude();
        } else if (h) {
            GlobalFree(h);
        }
    }
    CloseClipboard();
}

// The clipboard's CF_UNICODETEXT as UTF-16 in `arena`; false when it has none.
static b32 win32_dev_clip_read(Platform *p, Arena *arena, u16 **text, i64 *len) {
    *text = NULL;
    *len = 0;
    if (!win32_open_clipboard(p)) return 0;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    u16 *data = h ? (u16 *)GlobalLock(h) : NULL;
    if (data) {
        i64 cap = (i64)(GlobalSize(h) / 2);
        while (*len < cap && data[*len]) (*len)++;
        *text = PUSH_ARRAY(arena, u16, *len + 1);
        memcpy(*text, data, (size_t)*len * 2);
        GlobalUnlock(h);
    }
    CloseClipboard();
    return data != NULL;
}

static b32 win32_dev_utf16_is(u16 *text, i64 len, const char *ascii) {
    i64 n = 0;
    while (ascii[n]) n++;
    if (!text || len != n) return 0;
    for (i64 i = 0; i < n; i++) if (text[i] != (u16)ascii[i]) return 0;
    return 1;
}

static i32 win32_dev_clipboard_check(Platform *p) {
    WNDCLASSEXW wc = { .cbSize = sizeof(wc), .lpfnWndProc = DefWindowProcW, .hInstance = p->instance, .lpszClassName = L"ronin_clip" };
    RegisterClassExW(&wc);
    p->hwnd = CreateWindowExW(0, L"ronin_clip", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, p->instance, NULL); // a clipboard owner
    if (!p->hwnd || !win32_open_clipboard(p)) {
        LOG("clipboard-check: FAIL: cannot open the clipboard");
        return EXIT_TEST;
    }
    UINT other = 0, count = 0;
    for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f)) {
        count++;
        if (!other && f != CF_UNICODETEXT && f != CF_TEXT && f != CF_OEMTEXT && f != CF_LOCALE) other = f;
    }
    CloseClipboard();
    if (other) {
        WCHAR name[128];
        int n = GetClipboardFormatNameW(other, name, ARRAY_COUNT(name));
        String8 what = n > 0 ? str8_fmt(&p->perm, "the format '%S'", str8_from_str16(&p->perm, (u16 *)name, n))
                             : str8_fmt(&p->perm, "the standard format %u", other);
        LOG("clipboard-check: SKIP: the clipboard holds %S, which could not be put back exactly; nothing was touched", what);
        return EXIT_OK;
    }
    u16 *saved;
    i64 saved_len;
    b32 had_text = win32_dev_clip_read(p, &p->perm, &saved, &saved_len);
    LOG("clipboard-check: the clipboard holds %s (%u format(s)); saved", had_text ? "plain text" : "nothing", count);

    p->clip_no_history = 1;
    Arena arena = arena_create(MB(256));
    AppArgs args = { .dpi_scale = 1.0f, .headless = 1 };
    App *app = app_create(&arena, &args);
    b32 ok_copy = 0, ok_yank = 0;
    String8 yanked = { 0 };
    u16 *got = NULL;
    i64 got_len = 0;
    if (app) {
        app_dev_feed_events(app, NULL, 0, &arena);
        // E14: two lines copied in ronin are CRLF lines on the clipboard (what Notepad reads).
        app_dev_show_scratch(app, STR8_LIT("line one\nline two\n"));
        app_dev_feed(app, "C-x h M-w", &arena);
        win32_dev_clip_read(p, &arena, &got, &got_len);
        ok_copy = win32_dev_utf16_is(got, got_len, "line one\r\nline two\r\n");
        // E15: text another program put there (CRLF) is yanked as LF lines.
        static const u16 outside[] = { 'f', 'r', 'o', 'm', ' ', 'o', 'u', 't', 's', 'i', 'd', 'e', '\r', '\n', 's', 'e', 'c', 'o', 'n', 'd' };
        win32_dev_clip_put(p, outside, ARRAY_COUNT(outside));
        app_dev_show_scratch(app, STR8_LIT(""));
        app_dev_feed(app, "C-y", &arena);
        yanked = app_dev_text(app, &arena);
        ok_yank = str8_equal(yanked, STR8_LIT("from outside\nsecond"));
        app_shutdown(app);
    }

    // Put back what was there, and check it.
    win32_dev_clip_put(p, had_text ? saved : NULL, saved_len);
    u16 *back;
    i64 back_len;
    b32 back_text = win32_dev_clip_read(p, &arena, &back, &back_len);
    b32 ok_restore = had_text ? back_text && back_len == saved_len && memcmp(back, saved, (size_t)saved_len * 2) == 0
                              : !back_text && CountClipboardFormats() == 0;
    b32 ok = app && ok_copy && ok_yank && ok_restore;
    LOG("clipboard-check: %s: kill-ring-save of two lines gives CRLF on the clipboard %d (%D units), another program's CRLF text "
        "yanks as LF lines %d ('%S'), the clipboard put back as it was %d", ok ? "PASS" : "FAIL", ok_copy, got_len, ok_yank, yanked,
        ok_restore);
    DestroyWindow(p->hwnd);
    p->hwnd = NULL;
    return ok ? EXIT_OK : EXIT_TEST;
}

// --dpi-check <dir>: the window (hidden) gets WM_DPICHANGED as when it moves to a 125% display and
// back, with the rectangle Windows suggests; a screenshot at each step (dpi_100.png, dpi_125.png,
// dpi_back.png). The font must be set up again at each change, the client area must scale, and the
// frame after coming back must equal the first one pixel for pixel.
static i32 win32_dev_dpi_capture(Platform *p, const char *name, u8 **pixels, i32 *w, i32 *h) {
    for (i32 k = 0; k < 10000 && app_wants_frame(p->app); k++) win32_frame(p);
    r_request_capture(p->renderer);
    p->redraw = 1;
    win32_frame(p);
    *pixels = r_read_capture(p->renderer, &p->perm, w, h); // kept: compared later
    String8 path = str8_fmt(&p->scratch, "%S\\%s", p->dpi_check, name);
    return *pixels && win32_write_png(p, path, *pixels, *w, *h);
}

static void win32_dev_dpi_send(Platform *p, u32 dpi) {
    RECT r;
    GetWindowRect(p->hwnd, &r);
    i32 w = MulDiv(r.right - r.left, (i32)dpi, (i32)p->dpi), h = MulDiv(r.bottom - r.top, (i32)dpi, (i32)p->dpi);
    RECT suggested = { r.left, r.top, r.left + w, r.top + h };
    SendMessageW(p->hwnd, WM_DPICHANGED, MAKEWPARAM(dpi, dpi), (LPARAM)&suggested);
}

static i32 win32_dev_dpi_check(Platform *p) {
    app_dev_force_focus(p->app, 1);
    u32 dpi0 = p->dpi, dpi1 = dpi0 * 5 / 4;
    u8 *a, *b, *c;
    i32 wa, ha, wb, hb, wc, hc;
    i32 setups0 = app_dev_font_setups(p->app);
    b32 ok = win32_dev_dpi_capture(p, "dpi_100.png", &a, &wa, &ha);
    win32_dev_dpi_send(p, dpi1);
    i32 setups1 = app_dev_font_setups(p->app);
    ok = win32_dev_dpi_capture(p, "dpi_125.png", &b, &wb, &hb) && ok;
    win32_dev_dpi_send(p, dpi0);
    i32 setups2 = app_dev_font_setups(p->app);
    ok = win32_dev_dpi_capture(p, "dpi_back.png", &c, &wc, &hc) && ok;
    if (!ok) return EXIT_OUTPUT_FILE;
    b32 scaled = wb * 4 >= wa * 5 - wa / 10 && wb * 4 <= wa * 5 + wa / 10 && hb * 4 >= ha * 5 - ha / 10; // the frame scales too
    b32 same = wa == wc && ha == hc && memcmp(a, c, (size_t)wa * ha * 4) == 0;
    b32 fonts = setups1 == setups0 + 1 && setups2 == setups1 + 1;
    b32 pass = scaled && same && fonts;
    LOG("dpi-check: %s: %u -> %u -> %u dpi: client %dx%d -> %dx%d -> %dx%d (scaled %d), font set up again at each change %d "
        "(%d, %d, %d), the frame after coming back equals the first %d", pass ? "PASS" : "FAIL", dpi0, dpi1, dpi0, wa, ha, wb, hb,
        wc, hc, scaled, fonts, setups0, setups1, setups2, same);
    arena_reset(&p->scratch);
    return pass ? EXIT_OK : EXIT_PIXEL_MISMATCH;
}

#if RONIN_D3D_DEBUG
// --leak-check (leak.c): the window is never shown (as a screenshot run); keys go through the queue, the loop's
// pass is the main loop's.
static void win32_leak_feed(void *ctx, Event *events, i32 count, b32 frame_each) {
    Platform *p = (Platform *)ctx;
    for (i32 i = 0; i < count && !p->quit; i++) {
        win32_push_event(p, events[i]);
        if (frame_each) win32_frame(p);
    }
    if (p->event_count) win32_frame(p);
}

static b32 win32_leak_pump(void *ctx, u32 wait_ms) {
    Platform *p = (Platform *)ctx;
    HANDLE handles[WIN32_MAX_WATCHES + 1];
    DWORD count = win32_wait_handles(p, handles);
    DWORD app_wait = app_wait_ms(p->app);
    DWORD w = MsgWaitForMultipleObjectsEx(count, handles, MIN((DWORD)wait_ms, app_wait), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    b32 did = w != WAIT_TIMEOUT;
    if (w == WAIT_TIMEOUT && wait_ms >= app_wait) { // the app's own timeout, as in the main loop
        Event e = { .kind = EVENT_WAKEUP };
        win32_push_event(p, e);
        p->redraw = 1;
        did = 1;
    }
    win32_poll_watches(p);
    if (p->wake && WaitForSingleObject(p->wake, 0) == WAIT_OBJECT_0 && app_wake(p->app)) p->redraw = 1;
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        if (p->event_count > EVENT_CAPACITY - EVENT_RESERVE) win32_frame(p);
        if (msg.message == WM_QUIT) {
            p->quit = 1;
            break;
        }
        win32_translate(&msg);
        DispatchMessageW(&msg);
        did = 1;
    }
    if (p->redraw || p->event_count) {
        win32_frame(p);
        did = 1;
    }
    return did;
}

static u8 *win32_leak_capture(void *ctx, Arena *arena, i32 *w, i32 *h) {
    Platform *p = (Platform *)ctx;
    r_request_capture(p->renderer);
    win32_frame(p);
    return r_read_capture(p->renderer, arena, w, h);
}

static b32 win32_leak_write_png(void *ctx, String8 path, u8 *bgra, i32 w, i32 h) {
    return win32_write_png((Platform *)ctx, path, bgra, w, h);
}

static i32 win32_leak_check(Platform *p) {
    r_dev_set_present_interval(p->renderer, 0);
    LeakHost host = { .ctx = p, .renderer = p->renderer, .feed = win32_leak_feed, .pump = win32_leak_pump,
                      .capture = win32_leak_capture, .write_png = win32_leak_write_png };
    return leak_check(p->app, &host, p->leak_which, p->leak_cycles, str8_fmt(&p->perm, "%S\\tmp", p->exe_dir), &p->perm);
}
#endif

static b32 win32_dev_batch_mode(Platform *p) {
    return p->smoke || p->leak_check || p->dev_crash || p->idle_check || p->memory_report || p->bench_latency || p->startup_capture || p->type_ahead_check || p->dpi_check.len || p->bench_text || p->bench_buffer || p->bench_view || p->bench_edit || p->bench_complete || p->bench_syntax || p->bench_longline ||
           p->bench_search || p->bench_windows || p->bench_project || p->bench_grep || p->bench_compile || p->bench_cursors ||
           p->screenshot_path.len ||
           p->atlas_path.len;
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

// "+LINE" or "+LINE:COLUMN" (Emacs): decimal, 1-based. False when `a` is not of that form, so
// it is a file name.
static b32 win32_parse_goto(String8 a, i64 *line, i64 *col) {
    if (a.len < 2 || a.data[0] != '+') return 0;
    i64 i = 1, l = 0, c = 0, start = 1;
    for (; i < a.len && a.data[i] >= '0' && a.data[i] <= '9'; i++) l = MIN(l * 10 + (a.data[i] - '0'), (i64)1 << 50);
    if (i == start) return 0;
    if (i < a.len) {
        if (a.data[i] != ':') return 0;
        start = ++i;
        for (; i < a.len && a.data[i] >= '0' && a.data[i] <= '9'; i++) c = MIN(c * 10 + (a.data[i] - '0'), (i64)1 << 50);
        if (i == start || i < a.len) return 0;
    }
    *line = MAX(l, 1);
    *col = c;
    return 1;
}

#if RONIN_DEV
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

// The files of the command line: a +LINE[:COLUMN] applies to the file after it (Emacs). True when `a` was a
// file or a +LINE[:COLUMN] (anything but a "--" flag); p->files has room for every argument.
static b32 win32_file_arg(Platform *p, String8 a, i64 *goto_line, i64 *goto_col) {
    if (win32_parse_goto(a, goto_line, goto_col)) return 1;
    if (a.len >= 2 && a.data[0] == '-' && a.data[1] == '-') return 0;
    p->files[p->file_count++] = (AppFileArg){ a, *goto_line, *goto_col };
    *goto_line = *goto_col = 0;
    return 1;
}

// After the last argument: a +LINE[:COLUMN] with no file after it goes to the file before it, else (no file
// at all) to *scratch*.
static void win32_files_done(Platform *p, i64 goto_line, i64 goto_col) {
    if (!goto_line) return;
    if (p->file_count) {
        p->files[p->file_count - 1].line = goto_line;
        p->files[p->file_count - 1].col = goto_col;
    } else {
        p->files[p->file_count++] = (AppFileArg){ { 0 }, goto_line, goto_col };
    }
}

// --version: "ronin <version>" on the standard output when it is redirected, else on the console that started ronin
// (a GUI program has none of its own); nothing when there is neither.
static i32 win32_print_version(void) {
    static const char text[] = "ronin " RONIN_VERSION "\r\n";
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    b32 attached = 0;
    if (!out || out == INVALID_HANDLE_VALUE) {
        attached = AttachConsole(ATTACH_PARENT_PROCESS);
        out = attached ? CreateFileW(L"CONOUT$", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL) : NULL;
    }
    if (out && out != INVALID_HANDLE_VALUE) {
        DWORD written;
        WriteFile(out, text, sizeof(text) - 1, &written, NULL);
    }
    if (attached) {
        if (out && out != INVALID_HANDLE_VALUE) CloseHandle(out);
        FreeConsole();
    }
    return 0;
}

// The flags of every build: true when `a` is one (and sets it).
static b32 win32_release_flag(Platform *p, String8 a) {
    if (str8_equal(a, STR8_LIT("--startup-ms"))) p->startup_ms = 1;
    else if (str8_equal(a, STR8_LIT("--no-activate"))) p->no_activate = 1;
    else if (str8_equal(a, STR8_LIT("--version"))) p->version = 1;
    else return 0;
    return 1;
}

#if RONIN_DEV
// --fuzz-seeds FIRST COUNT: the seeds spread over worker processes (one per two logical processors, at most 8,
// below normal priority), worker k taking FIRST + k, FIRST + k + workers, ...; each logs to
// build\fuzz\worker<k>.log and keeps its files in build\tmp\fuzz<k>. Exit 9 when any worker failed.
static i32 win32_dev_fuzz_seeds(Platform *p) {
    i32 workers = CLAMP((i32)(GetActiveProcessorCount(ALL_PROCESSOR_GROUPS) / 2), 1, 8);
    if ((u64)workers > p->fuzz_count) workers = (i32)p->fuzz_count;
    CreateDirectoryW((WCHAR *)str16_from_str8(&p->perm, str8_fmt(&p->perm, "%S\\fuzz", p->exe_dir)).data, NULL);
    WCHAR exe[MAX_PATH];
    DWORD exe_len = GetModuleFileNameW(NULL, exe, MAX_PATH);
    String8 exe8 = str8_from_str16(&p->perm, (u16 *)exe, exe_len);
    HANDLE procs[8] = { 0 };
    u64 t0 = os_time_us();
    LOG("fuzz-seeds: seeds %U to %U over %d worker processes", p->fuzz_first, p->fuzz_first + p->fuzz_count - 1, workers);
    for (i32 k = 0; k < workers; k++) {
        String8 line = str8_fmt(&p->perm, "\"%S\" --fuzz-worker %d %d %U %U", exe8, k, workers, p->fuzz_first, p->fuzz_count);
        String16 line16 = str16_from_str8(&p->perm, line);
        STARTUPINFOW si = { .cb = sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };
        if (!CreateProcessW(exe, (WCHAR *)line16.data, NULL, NULL, FALSE, BELOW_NORMAL_PRIORITY_CLASS, NULL, NULL, &si, &pi)) {
            LOG("fuzz-seeds: worker %d did not start (error %u)", k, (u32)GetLastError());
            continue;
        }
        CloseHandle(pi.hThread);
        procs[k] = pi.hProcess;
    }
    i32 failed = 0;
    for (i32 k = 0; k < workers; k++) {
        DWORD code = 1;
        if (procs[k]) {
            WaitForSingleObject(procs[k], INFINITE);
            GetExitCodeProcess(procs[k], &code);
            CloseHandle(procs[k]);
        }
        if (code != 0) failed++;
        LOG("fuzz-seeds: worker %d %s (exit %u), log build\\fuzz\\worker%d.log", k, code ? "FAILED" : "passed", (u32)code, k);
    }
    LOG("fuzz-seeds: %s, %U seeds (%U to %U), %d worker(s) failed, %U ms", failed ? "FAIL" : "PASS", p->fuzz_count, p->fuzz_first,
        p->fuzz_first + p->fuzz_count - 1, failed, (os_time_us() - t0) / 1000);
    return failed ? EXIT_TEST : EXIT_OK;
}

// --test: command lines through win32_file_arg / win32_files_done.
static i32 win32_dev_test_args(Platform *p) {
    static const struct { const char *args[5]; const char *want; } cases[] = {
        { { "a.c" }, "a.c 0:0" },
        { { "+3", "a.c", "b.c", "+5:2" }, "a.c 3:0 | b.c 5:2" },
        { { "a.c", "+4", "b.c" }, "a.c 0:0 | b.c 4:0" },
        { { "+7:9" }, " 7:9" },
        { { "+2", "--smoke", "x.txt", "y.txt", "z.txt" }, "x.txt 2:0 | y.txt 0:0 | z.txt 0:0" },
        { { "+abc", "+1:" }, "+abc 0:0 | +1: 0:0" },
    };
    AppFileArg *saved = p->files;
    i32 saved_count = p->file_count, failures = 0;
    AppFileArg files[8];
    for (i32 c = 0; c < ARRAY_COUNT(cases); c++) {
        p->files = files;
        p->file_count = 0;
        i64 line = 0, col = 0;
        for (i32 k = 0; k < 5 && cases[c].args[k]; k++) win32_file_arg(p, str8_cstr(cases[c].args[k]), &line, &col);
        win32_files_done(p, line, col);
        u8 got[256];
        i64 n = 0;
        for (i32 i = 0; i < p->file_count; i++) {
            n += fmt_buf(got + n, (i64)sizeof(got) - n, "%s%S %D:%D", i ? " | " : "", files[i].path, files[i].line, files[i].col);
        }
        if (!str8_equal(str8(got, n), str8_cstr(cases[c].want))) {
            LOG("test: FAIL: command line %d: '%S', expected '%s'", c, str8(got, n), cases[c].want);
            failures++;
        }
    }
    p->files = saved;
    p->file_count = saved_count;
    // The flags of every build.
    b32 saved_startup = p->startup_ms, saved_no_activate = p->no_activate, saved_version = p->version;
    p->startup_ms = p->no_activate = p->version = 0;
    b32 flags_ok = win32_release_flag(p, STR8_LIT("--no-activate")) && p->no_activate && !p->startup_ms &&
                   win32_release_flag(p, STR8_LIT("--startup-ms")) && p->startup_ms && !win32_release_flag(p, STR8_LIT("--no-activat")) &&
                   !p->version && win32_release_flag(p, STR8_LIT("--version")) && p->version &&
                   !win32_release_flag(p, STR8_LIT("--smoke"));
    p->startup_ms = saved_startup;
    p->no_activate = saved_no_activate;
    p->version = saved_version;
    if (!flags_ok) {
        LOG("test: FAIL: command line: --no-activate / --startup-ms / --version not recognized as flags of every build (or another one was)");
        failures++;
    }
    if (!failures) {
        LOG("test: ok: command line files (+LINE[:COLUMN] before each file, a trailing one, *scratch*, flags between, not a number); "
            "--no-activate, --startup-ms and --version in every build");
    }
    return failures;
}
#endif

#if RONIN_NO_CRT
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE prev_instance, PWSTR cmd_line, int show_cmd);

int __isa_available_init(void); // the compiler's support library: which instruction sets memcpy and memset may use

// __try / __except need the C language's exception handler, which the C runtime's library has (and with it the C++
// exception machinery); the system's own copy in ntdll.dll does the same work. Found once, before wWinMain.
typedef EXCEPTION_DISPOSITION (*Win32CHandler)(struct _EXCEPTION_RECORD *, void *, struct _CONTEXT *, struct _DISPATCHER_CONTEXT *);
static Win32CHandler win32_c_handler;

EXCEPTION_DISPOSITION __C_specific_handler(struct _EXCEPTION_RECORD *record, void *frame, struct _CONTEXT *context,
                                           struct _DISPATCHER_CONTEXT *dispatch) {
    return win32_c_handler ? win32_c_handler(record, frame, context, dispatch) : ExceptionContinueSearch;
}

// The CRT-free build's entry point (/ENTRY): what the CRT's startup did for us is choosing the copy routines'
// instruction sets, calling wWinMain and ending the process with its code.
void win32_entry(void) {
    __isa_available_init();
    win32_c_handler = (Win32CHandler)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "__C_specific_handler");
    ExitProcess((UINT)wWinMain(GetModuleHandleW(NULL), NULL, GetCommandLineW(), SW_SHOWDEFAULT));
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
    p->wake = CreateEventW(NULL, FALSE, FALSE, NULL); // os_wake_main (auto-reset); none: workers' results wait for input
#if RONIN_DEV
    p->seed = 0x7ea1;
    p->leak_cycles = 200;
#endif
    g_platform = p;
    SetUnhandledExceptionFilter(win32_crash_filter); // modified files to the recovery directory, then the crash goes on
    win32_stack_guarantee();
#if RONIN_DEV
    os_dev_stage("WinMain entered");
#endif

    String8 *args;
    i32 arg_count = win32_parse_args(&p->perm, &args);
    // Any "--" argument that is not recognized (or lacks its value) ends the process right
    // here: a stale or wrong executable never opens a window.
    String8 bad_arg = { 0 };
    p->files = PUSH_ARRAY(&p->perm, AppFileArg, arg_count);
    i64 goto_line = 0, goto_col = 0;
    for (i32 i = 1; i < arg_count && !bad_arg.len; i++) {
        String8 a = args[i];
        if (win32_file_arg(p, a, &goto_line, &goto_col)) continue;
        if (win32_release_flag(p, a)) continue;
#if RONIN_DEV
        b32 has_value = i + 1 < arg_count;
        if (str8_equal(a, STR8_LIT("--smoke"))) { p->smoke = 1; continue; }
        if (str8_equal(a, STR8_LIT("--test"))) { p->test = 1; continue; }
#if RONIN_D3D_DEBUG
        if (str8_equal(a, STR8_LIT("--leak-check"))) {
            p->leak_check = 1;
            p->leak_which = STR8_LIT("all");
            if (has_value && !(args[i + 1].len >= 2 && args[i + 1].data[0] == '-' && args[i + 1].data[1] == '-')) p->leak_which = args[++i];
            if (leak_scenario_known(p->leak_which)) continue;
        }
        if (str8_equal(a, STR8_LIT("--cycles")) && has_value) {
            p->leak_cycles = win32_parse_i32(args[++i]);
            if (p->leak_cycles >= 8 && p->leak_cycles % 4 == 0) continue;
        }
#endif

        if (str8_equal(a, STR8_LIT("--seed")) && has_value) { p->seed = win32_parse_u64(args[++i]); continue; }
        if (str8_equal(a, STR8_LIT("--fuzz-seeds")) && i + 2 < arg_count) {
            p->fuzz_first = win32_parse_u64(args[++i]);
            p->fuzz_count = win32_parse_u64(args[++i]);
            if (p->fuzz_count) continue;
        }
        if (str8_equal(a, STR8_LIT("--map-report")) && has_value) { p->map_report = args[++i]; continue; }
        if (str8_equal(a, STR8_LIT("--check-imports")) && i + 2 < arg_count) {
            p->check_exe = args[++i];
            p->check_list = args[++i];
            continue;
        }
        if (str8_equal(a, STR8_LIT("--soak")) && has_value) {
            p->soak_events = win32_parse_u64(args[++i]);
            if (p->soak_events) continue;
        }
        if (str8_equal(a, STR8_LIT("--fuzz-worker")) && i + 4 < arg_count) {
            p->fuzz_worker = win32_parse_i32(args[++i]) + 1;
            p->fuzz_workers = win32_parse_i32(args[++i]);
            p->fuzz_first = win32_parse_u64(args[++i]);
            p->fuzz_count = win32_parse_u64(args[++i]);
            if (p->fuzz_count && p->fuzz_workers > 0) continue;
        }
        if (str8_equal(a, STR8_LIT("--bench-text"))) { p->bench_text = 1; continue; }
        if (str8_equal(a, STR8_LIT("--bench-buffer"))) { p->bench_buffer = 1; continue; }
        if (str8_equal(a, STR8_LIT("--bench-view"))) { p->bench_view = 1; continue; }
        if (str8_equal(a, STR8_LIT("--bench-edit"))) { p->bench_edit = 1; continue; }
        if (str8_equal(a, STR8_LIT("--bench-complete"))) { p->bench_complete = 1; continue; }
        if (str8_equal(a, STR8_LIT("--bench-syntax"))) { p->bench_syntax = 1; continue; }
        if (str8_equal(a, STR8_LIT("--bench-longline"))) { p->bench_longline = 1; continue; }
        if (str8_equal(a, STR8_LIT("--crash"))) { p->dev_crash = 1; continue; }
        if (str8_equal(a, STR8_LIT("--recovery-dir")) && has_value) { p->recovery_arg = args[++i]; continue; }
        if (str8_equal(a, STR8_LIT("--bench-search"))) { p->bench_search = 1; continue; }
        if (str8_equal(a, STR8_LIT("--bench-windows"))) { p->bench_windows = 1; continue; }
        if (str8_equal(a, STR8_LIT("--bench-cursors"))) { p->bench_cursors = 1; continue; }
        if (str8_equal(a, STR8_LIT("--bench-project"))) { p->bench_project = 1; continue; }
        if (str8_equal(a, STR8_LIT("--screenshot")) && has_value) { p->screenshot_path = args[++i]; continue; }
        if (str8_equal(a, STR8_LIT("--dump-atlas")) && has_value) { p->atlas_path = args[++i]; continue; }
        if (str8_equal(a, STR8_LIT("--scale")) && has_value) { p->forced_scale = (f32)win32_parse_i32(args[++i]) / 100.0f; continue; }
        if (str8_equal(a, STR8_LIT("--config")) && has_value) { p->config_path = args[++i]; continue; }
        if (str8_equal(a, STR8_LIT("--scan-hold")) && has_value) { p->scan_hold = win32_parse_i32(args[++i]); continue; }
        if (str8_equal(a, STR8_LIT("--grep-hold")) && has_value) { p->grep_hold = win32_parse_i32(args[++i]); continue; }
        if (str8_equal(a, STR8_LIT("--bench-grep"))) { p->bench_grep = 1; continue; }
        if (str8_equal(a, STR8_LIT("--bench-compile"))) { p->bench_compile = 1; continue; }
        if (str8_equal(a, STR8_LIT("--keys")) && has_value) { p->keys = args[++i]; continue; }
        if (str8_equal(a, STR8_LIT("--keys-after")) && has_value) { p->keys_after = args[++i]; continue; }
        if (str8_equal(a, STR8_LIT("--touch")) && has_value) { p->touch = args[++i]; continue; }
        if (str8_equal(a, STR8_LIT("--child")) && has_value) { p->child_script = args[++i]; continue; }
        if (str8_equal(a, STR8_LIT("--compile-wait")) && has_value) { p->compile_wait_ms = (u32)win32_parse_i32(args[++i]); continue; }
        if (str8_equal(a, STR8_LIT("--log-keys"))) { p->log_keys = 1; continue; }
        if (str8_equal(a, STR8_LIT("--idle-check"))) { p->idle_check = 1; continue; }
        if (str8_equal(a, STR8_LIT("--memory-report"))) { p->memory_report = 1; continue; }
        if (str8_equal(a, STR8_LIT("--bench-latency"))) { p->bench_latency = 1; continue; }
        if (str8_equal(a, STR8_LIT("--startup-capture"))) { p->startup_capture = 1; continue; }
        if (str8_equal(a, STR8_LIT("--type-ahead-check"))) { p->type_ahead_check = 1; continue; }
        if (str8_equal(a, STR8_LIT("--clipboard-check"))) { p->clipboard_check = 1; continue; }
        if (str8_equal(a, STR8_LIT("--dpi-check")) && has_value) { p->dpi_check = args[++i]; continue; }
        if (str8_equal(a, STR8_LIT("--render-mode")) && has_value) {
            String8 mode = args[++i];
            p->render_mode_forced = 1;
            if (str8_equal(mode, STR8_LIT("classic"))) p->render_mode = FB_RENDER_GDI_CLASSIC;
            else if (str8_equal(mode, STR8_LIT("natural"))) p->render_mode = FB_RENDER_NATURAL;
            else p->render_mode = FB_RENDER_NATURAL_SYMMETRIC;
            continue;
        }
#endif
        bad_arg = a;
    }
    win32_files_done(p, goto_line, goto_col);
    if (p->version && !bad_arg.len) return win32_print_version(); // before any window or thread
    String8 recovery_dir = { 0 };
#if RONIN_DEV
    if (p->recovery_arg.len) recovery_dir = p->recovery_arg;
    else if (p->test || p->fuzz_count || p->soak_events || win32_dev_batch_mode(p)) recovery_dir = str8_fmt(&p->perm, "%S\\tmp\\recovery", os_exe_dir(&p->perm));
#endif
    win32_recovery_init(p, recovery_dir);
#if RONIN_DEV
    if (p->startup_capture) { // from as early as possible: the window may be shown before the device exists
        p->capture = PUSH_STRUCT(&p->perm, StartupCapture);
        HANDLE thread = CreateThread(NULL, 0, win32_dev_capture_thread, p, 0, NULL);
        if (thread) CloseHandle(thread);
    }
#endif
#if !RONIN_DEV
    if (bad_arg.len) return EXIT_USAGE;
#else
    // The tests' scripted program: before the log is opened (the run that started it writes that log), no
    // window.
    if (p->child_script.len) return bad_arg.len ? EXIT_USAGE : win32_dev_child(p, p->child_script);
#endif

#if RONIN_DEV
    // Log next to the executable (build\ronin.log), independent of the working directory.
    String8 exe_dir = os_exe_dir(&p->perm);
    String8 log_path = p->fuzz_worker ? str8_fmt(&p->perm, "%S\\fuzz\\worker%d.log", exe_dir, p->fuzz_worker - 1)
                     : p->dev_crash ? str8_fmt(&p->perm, "%S\\crash.log", exe_dir) // the crash test's child: the parent's log stays
                                      : str8_fmt(&p->perm, "%S\\ronin.log", exe_dir);
    String16 log_path16 = str16_from_str8(&p->perm, log_path);
    p->log_file = CreateFileW((WCHAR *)log_path16.data, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    p->exe_dir = exe_dir;
    if (bad_arg.len) {
        LOG("unknown argument '%S' (or missing value), exiting with %d", bad_arg, (i32)EXIT_USAGE);
        if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) CloseHandle(p->log_file);
        return EXIT_USAGE;
    }
    LOG("ronin dev build, mode: %s", p->test ? "test" : p->fuzz_count ? "fuzz-seeds" : p->soak_events ? "soak" : p->leak_check ? "leak-check" : p->map_report.len ? "map-report" : p->check_exe.len ? "check-imports" : p->smoke ? "smoke" : p->bench_text ? "bench-text"
                                   : p->bench_buffer ? "bench-buffer" : p->bench_view ? "bench-view" : p->bench_edit ? "bench-edit"
                                   : p->bench_complete ? "bench-complete" : p->bench_syntax ? "bench-syntax" : p->bench_longline ? "bench-longline" : p->bench_search ? "bench-search"
                                   : p->bench_windows ? "bench-windows" : p->bench_project ? "bench-project" : p->bench_grep ? "bench-grep" : p->bench_compile ? "bench-compile"
                                   : p->bench_cursors ? "bench-cursors"
                                   : (p->screenshot_path.len || p->atlas_path.len) ? "capture" : "interactive");
    // Tests, the smoke, benches and screenshots never touch the real clipboard.
    if (p->test || p->fuzz_count || p->soak_events || win32_dev_batch_mode(p)) os_dev_clipboard_fake(1);
    if (p->clipboard_check) { // headless, the real clipboard
        i32 code = win32_dev_clipboard_check(p);
        if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) CloseHandle(p->log_file);
        return code;
    }
    if (p->check_exe.len) { // the release exe's DLLs against the committed list
        i32 code = win32_dev_check_imports(p, p->check_exe, p->check_list);
        if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) CloseHandle(p->log_file);
        return code;
    }
    if (p->map_report.len) { // the release exe's bytes by module, from its linker map
        i32 failed = test_map_report(p->map_report, str8_fmt(&p->perm, "%S\\..\\src", exe_dir));
        if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) CloseHandle(p->log_file);
        return failed ? EXIT_TEST : EXIT_OK;
    }
    if (p->soak_events) { // headless: random key and mouse events through the app
        i32 failures = test_soak(p->soak_events, p->seed, str8_fmt(&p->perm, "%S\\tmp", exe_dir));
        if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) CloseHandle(p->log_file);
        return failures ? EXIT_TEST : EXIT_OK;
    }
    if (p->fuzz_count) { // headless: the seeded groups of --test over a range of seeds, in worker processes
        i32 code;
        if (p->fuzz_worker) {
            String8 tmp = str8_fmt(&p->perm, "%S\\tmp\\fuzz%d", exe_dir, p->fuzz_worker - 1);
            CreateDirectoryW((WCHAR *)str16_from_str8(&p->perm, tmp).data, NULL);
            code = test_fuzz_seeds(p->fuzz_first + (u64)(p->fuzz_worker - 1), p->fuzz_first + p->fuzz_count, (u64)p->fuzz_workers, tmp)
                 ? EXIT_TEST : EXIT_OK;
        } else {
            code = win32_dev_fuzz_seeds(p);
        }
        if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) CloseHandle(p->log_file);
        return code;
    }
    if (p->test) { // headless: no window, no device, no font
        i32 failures = test_run(p->seed, str8_fmt(&p->perm, "%S\\tmp", exe_dir));
        failures += win32_dev_test_shield();
        failures += win32_dev_test_mods(p);
        failures += win32_dev_test_args(p);
        failures += win32_dev_test_keys(p);
        failures += win32_dev_input_test(p);
        if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) CloseHandle(p->log_file);
        return failures ? EXIT_TEST : EXIT_OK;
    }
    if (p->bench_buffer || p->bench_view || p->bench_edit || p->bench_search || p->bench_windows || p->bench_project || p->bench_grep ||
        p->bench_compile || p->bench_cursors) { // generated before the app opens it; measured after startup
        p->files[0] = (AppFileArg){ test_bench_buffer_file(&p->perm, str8_fmt(&p->perm, "%S\\tmp", exe_dir)), 0, 0 };
        p->file_count = 1;
    }
    if (p->bench_longline) {
        p->files[0] = (AppFileArg){ win32_bench_longline_file(p), 0, 0 };
        p->file_count = 1;
    }
    if (p->bench_syntax) {
        p->files[0] = (AppFileArg){ test_bench_syntax_file(&p->perm, str8_fmt(&p->perm, "%S\\tmp", exe_dir)), 0, 0 };
        p->file_count = 1;
    }
#endif

    // Argument parsing above takes microseconds; the device thread starts right after it.
    Renderer *renderer = r_alloc(&p->perm); // allocated here, before the worker starts
    HANDLE device_thread = CreateThread(NULL, 0, win32_device_thread, renderer, 0, NULL);
#if RONIN_DEV
    os_dev_stage("arguments parsed, device thread started");
#endif


    p->cursors[MOUSE_CURSOR_ARROW] = LoadCursorW(NULL, IDC_ARROW);
    p->cursors[MOUSE_CURSOR_RESIZE_WE] = LoadCursorW(NULL, IDC_SIZEWE);
    p->cursors[MOUSE_CURSOR_RESIZE_NS] = LoadCursorW(NULL, IDC_SIZENS);
    WNDCLASSEXW wc = {
        .cbSize = sizeof(wc),
        .lpfnWndProc = win32_wndproc,
        .hInstance = instance,
        .hCursor = p->cursors[MOUSE_CURSOR_ARROW],
        .lpszClassName = L"ronin",
        // res\ronin.rc's icon (drawn by tools\make_icon.c) at the sizes of the title bar and of Alt+Tab.
        .hIcon = LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), 0),
        .hIconSm = LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0),
    };
    if (!RegisterClassExW(&wc)) os_fatal(STR8_LIT("RegisterClassExW failed."));

    // Create hidden on the monitor under the mouse cursor, then size it for that monitor's DPI.
    POINT cursor;
    GetCursorPos(&cursor);
    MONITORINFO monitor = { .cbSize = sizeof(monitor) };
    GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST), &monitor);
    RECT work = monitor.rcWork;

    DWORD style = WS_OVERLAPPEDWINDOW;
    p->hwnd = CreateWindowExW(0, L"ronin", L"ronin", style, work.left, work.top, 640, 400, NULL, NULL, instance, NULL);
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

    // Dark title bar. The app paints the caption in the background color (os_set_caption_color).
    BOOL dark = TRUE;
    win32_dwm_attribute(p, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    RECT client;
    GetClientRect(p->hwnd, &client);
    p->width = client.right - client.left;
    p->height = client.bottom - client.top;
#if RONIN_DEV
    os_dev_stage("window created (hidden), sized for its DPI");
    p->main_thread_id = GetCurrentThreadId();
    if (p->type_ahead_check) p->type_ahead_thread = CreateThread(NULL, 0, win32_dev_type_ahead_thread, p, 0, NULL);
#endif

    AppArgs app_args = { .dpi_scale = scale, .render_mode_forced = p->render_mode_forced, .render_mode = p->render_mode,
                         .files = p->files, .file_count = p->file_count };
#if RONIN_DEV
#if RONIN_D3D_DEBUG
    if (p->leak_check) { // its files (made once) and its config, unless --config names one
        String8 conf = leak_prepare(&p->perm, str8_fmt(&p->perm, "%S\\tmp", p->exe_dir));
        if (!p->config_path.len) p->config_path = conf;
    }
#endif
    app_args.config_path = p->config_path;
    app_args.scan_hold = p->scan_hold;
    app_args.grep_hold = p->grep_hold;
    app_args.user_config = !(p->smoke || p->bench_text || p->bench_buffer || p->bench_view || p->bench_edit || p->bench_complete ||
                             p->bench_syntax || p->bench_search || p->bench_windows || p->bench_project || p->bench_grep ||
                             p->bench_compile || p->bench_cursors || p->type_ahead_check || p->leak_check); // defaults
#endif
#if RONIN_DEV
    LOG("memory: private bytes before the app: %U KB", os_dev_private_bytes() / 1024);
#endif
    p->app = app_create(&p->perm, &app_args);
#if RONIN_DEV
    os_dev_stage("app created");
#endif
#if RONIN_DEV
    LOG("memory: private bytes with the app (config, font and atlas, buffers): %U KB", os_dev_private_bytes() / 1024);
    if (p->log_keys && p->app) {
        app_dev_log_keys(p->app, 1);
        win32_dev_log_layouts(p);
    }
#endif
    if (!p->app) {
#if RONIN_DEV
        if (win32_dev_batch_mode(p)) {
            LOG("font initialization failed");
            return EXIT_FONT;
        }
#endif
        os_fatal(STR8_LIT("Could not open the font (Consolas or Courier New)."));
    }

    // Shown and activated now, still cloaked (DWMWA_CLOAK): keys typed from here on queue for ronin
    // instead of going to whatever launched it, and the activation (the focus sets up the IME and text
    // services, ~15 ms) overlaps the device creation. Done after the app, not right after the window:
    // there it slowed the device thread down (first Present 174 ms median against 167; here 168). The
    // first frame is presented before uncloaking, so the window never shows an unpainted (white) client
    // area. If Windows refuses the foreground, the window is still shown and appears when it is
    // uncloaked. The smoke, --idle-check and the benches show it later without activation (they never take
    // the focus from whatever the user is typing in); screenshot runs and --dpi-check never show it.
    // --no-activate (every build) shows it as the benches do: for measurements that must not take the focus.
    b32 show_early = !p->no_activate;
#if RONIN_DEV
    b32 bench = p->bench_text || p->bench_buffer || p->bench_view || p->bench_edit || p->bench_complete || p->bench_syntax || p->bench_longline ||
                p->bench_search || p->bench_windows || p->bench_project || p->bench_grep || p->bench_compile || p->bench_cursors;
    show_early = show_early && !(p->smoke || p->leak_check || p->dev_crash || p->idle_check || p->memory_report || p->bench_latency || p->startup_capture || bench || p->screenshot_path.len || p->atlas_path.len || p->dpi_check.len);
#endif
    if (show_early && !p->shown_early) {
        BOOL cloak = TRUE;
        win32_dwm_attribute(p, DWMWA_CLOAK, &cloak, sizeof(cloak));
        ShowWindow(p->hwnd, SW_SHOW);
        BOOL fg = SetForegroundWindow(p->hwnd);
#if RONIN_DEV
        os_dev_stage("shown and activated (cloaked)");
        LOG("startup: SetForegroundWindow %d, foreground %s, active %s, focus %s", fg, GetForegroundWindow() == p->hwnd ? "ronin" : "other",
            GetActiveWindow() == p->hwnd ? "ronin" : "none", GetFocus() == p->hwnd ? "ronin" : "none");
#else
        (void)fg;
#endif
    }

#if RONIN_DEV
    u64 join_start = os_time_us();
#endif
    if (device_thread) {
        // Only sent messages are handled meanwhile (the activation, the IME and text services set-up):
        // posted input stays queued, in order, with its key state, until the main loop reads it.
        while (MsgWaitForMultipleObjects(1, &device_thread, FALSE, INFINITE, QS_SENDMESSAGE) == WAIT_OBJECT_0 + 1) {
            MSG msg;
            PeekMessageW(&msg, NULL, 0, 0, PM_NOREMOVE | PM_QS_SENDMESSAGE);
        }
        CloseHandle(device_thread);
    } else {
        r_create_device(renderer); // no thread: create it here
    }
#if RONIN_DEV
    os_dev_stage("device thread joined");
#endif
#if RONIN_DEV
    LOG("startup: device thread %s, main thread waited %U us for it", device_thread ? "used" : "unavailable",
        os_time_us() - join_start);
    LOG("memory: private bytes with the D3D11 device: %U KB", os_dev_private_bytes() / 1024);
#endif
    if (!r_finish_create(renderer, p->hwnd, p->width, p->height)) {
#if RONIN_DEV
        if (win32_dev_batch_mode(p)) {
            LOG("renderer initialization failed");
            return EXIT_RENDERER_INIT;
        }
#endif
        os_fatal(STR8_LIT("Could not initialize Direct3D 11."));
    }
    p->renderer = renderer; // from here on WM_SIZE / WM_PAINT may render
#if RONIN_DEV
    os_dev_stage("swap chain, pipeline and atlas texture created");
#endif
#if RONIN_DEV
    LOG("memory: private bytes with the swap chain, pipeline and atlas texture: %U KB", os_dev_private_bytes() / 1024);
#endif

#if RONIN_DEV
    if (p->dev_crash) win32_dev_crash(p);
    if (p->screenshot_path.len || p->atlas_path.len) {
        p->exit_code = win32_write_outputs(p);
        p->quit = 1;
    }
    if (p->dpi_check.len) {
        p->exit_code = win32_dev_dpi_check(p);
        p->quit = 1;
    }
#if RONIN_D3D_DEBUG
    if (p->leak_check && !p->quit) {
        p->exit_code = win32_leak_check(p);
        p->quit = 1;
    }
#endif
#endif

    if (!p->quit) {
        BOOL cloak = TRUE;
        if (!show_early && !p->shown_early) { // the smoke, --idle-check, the benches, --no-activate: shown cloaked, never activated
            win32_dwm_attribute(p, DWMWA_CLOAK, &cloak, sizeof(cloak));
#if RONIN_DEV
            if (p->startup_capture) win32_dev_capture_place(p);
#endif
            ShowWindow(p->hwnd, SW_SHOWNA);
        }
        if (p->frame_count == 0) win32_frame(p);
        if (p->early_brush) { // the first frame replaced the early window's paint
            DeleteObject(p->early_brush);
            p->early_brush = NULL;
        }
        cloak = FALSE;
        win32_dwm_attribute(p, DWMWA_CLOAK, &cloak, sizeof(cloak));
#if RONIN_DEV
        os_dev_stage("uncloaked");
        if (p->idle_check) win32_dev_idle_check_start(p);
        if (p->memory_report) {
            HANDLE thread = CreateThread(NULL, 0, win32_dev_memory_thread, p->hwnd, 0, NULL);
            if (thread) CloseHandle(thread);
        }
        if (p->bench_latency) {
            p->latency = PUSH_STRUCT(&p->perm, LatencyBench);
            p->latency->current = -1;
            HANDLE thread = CreateThread(NULL, 0, win32_dev_latency_thread, p, 0, NULL);
            if (thread) CloseHandle(thread);
        }
        if (p->smoke) p->redraw = 1; // its second plain frame, at once (nothing else would wake the loop)
        p->stages_logged = 1;
        win32_dev_log_stages(p);
        win32_inject_keys(p);
        if (p->type_ahead_check && p->type_ahead_thread) {
            p->exit_code = win32_dev_type_ahead_finish(p);
            p->quit = 1;
        }
#endif
    }

#if RONIN_DEV
    if (p->bench_text && !p->quit) {
        win32_bench_text(p);
        p->quit = 1;
    }
    if (p->bench_buffer && !p->quit) {
        test_bench_buffer(p->files[0].path, str8_fmt(&p->perm, "%S\\tmp", p->exe_dir));
        win32_bench_buffer_frames(p);
        p->quit = 1;
    }
    if (p->bench_edit && !p->quit) {
        win32_bench_edit(p);
        p->quit = 1;
    }
    if (p->bench_view && !p->quit) {
        win32_bench_view(p);
        p->quit = 1;
    }
    if (p->bench_syntax && !p->quit) {
        win32_bench_syntax(p);
        p->quit = 1;
    }
    if (p->bench_longline && !p->quit) {
        win32_bench_longline(p);
        p->quit = 1;
    }
    if (p->bench_search && !p->quit) {
        win32_bench_search(p);
        p->quit = 1;
    }
    if (p->bench_project && !p->quit) {
        win32_bench_project(p);
        p->quit = 1;
    }
    if (p->bench_grep && !p->quit) {
        win32_bench_grep(p);
        p->quit = 1;
    }
    if (p->bench_compile && !p->quit) {
        win32_bench_compile(p);
        p->quit = 1;
    }
    if (p->bench_windows && !p->quit) {
        win32_bench_windows(p);
        p->quit = 1;
    }
    if (p->bench_cursors && !p->quit) {
        win32_bench_cursors(p);
        p->quit = 1;
    }
    if (p->bench_complete && !p->quit) {
        win32_bench_complete(p);
        p->quit = 1;
    }
#endif

    while (!p->quit) {
        // Block until there is something to do: 0% CPU when idle. The directory watches are part of
        // the wait; a timeout only while the app asks for one (a config read to retry).
        if (!p->redraw) {
            HANDLE handles[WIN32_MAX_WATCHES + 1];
            DWORD count = win32_wait_handles(p, handles);
            DWORD timeout = app_wait_ms(p->app);
            if (MsgWaitForMultipleObjectsEx(count, handles, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_TIMEOUT) {
                Event e = { .kind = EVENT_WAKEUP };
                win32_push_event(p, e);
                p->redraw = 1;
            }
        }
        win32_poll_watches(p);
        // Workers' results: the app takes them without a frame unless something shown changed.
        if (p->wake && WaitForSingleObject(p->wake, 0) == WAIT_OBJECT_0 && app_wake(p->app)) p->redraw = 1;
#if RONIN_DEV
        p->idle_wakeups++;
#endif

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
            win32_translate(&msg);
            DispatchMessageW(&msg);
        }
        if (p->quit) break;

#if RONIN_DEV
        if (p->smoke) {
            // Two plain frames, then one highlighted buffer per language (the first with the
            // rendering checks), a known buffer without focus, the same after a click (focus forced
            // on), a region and the M-x list: each back buffer is read back and checked.
            if (p->frame_count >= 2) {
                static const BufferLanguage languages[] = { BUFFER_LANG_C, BUFFER_LANG_CPP, BUFFER_LANG_CSHARP,
                                                            BUFFER_LANG_JAVASCRIPT, BUFFER_LANG_TYPESCRIPT, BUFFER_LANG_JAI };
                i32 code = EXIT_OK;
                for (i32 k = 0; k < ARRAY_COUNT(languages); k++) {
                    app_dev_smoke_syntax(p->app, languages[k]);
                    r_request_capture(p->renderer);
                    win32_frame(p);
                    i32 c = win32_smoke_check_frame(p, k == 0 ? -1 : -2, buffer_language_name(languages[k]));
                    if (code == EXIT_OK) code = c;
                }
                app_dev_smoke_buffer_view(p->app);
                r_request_capture(p->renderer);
                win32_frame(p);
                i32 code2 = win32_smoke_check_frame(p, 0, NULL);
                FrameInput input = win32_frame_input(p);
                Event click = { .kind = EVENT_MOUSE_DOWN, .button = MOUSE_LEFT };
                app_dev_smoke_click_point(p->app, &input, &click.x, &click.y);
                win32_push_event(p, click);
                app_dev_force_focus(p->app, 1);
                r_request_capture(p->renderer);
                win32_frame(p);
                i32 code3 = win32_smoke_check_frame(p, 1, NULL);
                app_dev_smoke_region(p->app);
                r_request_capture(p->renderer);
                win32_frame(p);
                if (code3 == EXIT_OK) code3 = win32_smoke_check_frame(p, 2, NULL);
                win32_smoke_feed_no_frame(p, "M-x m i n i b"); // stage 3: a filtered candidate list
                r_request_capture(p->renderer);
                win32_frame(p);
                if (code3 == EXIT_OK) code3 = win32_smoke_check_frame(p, 3, NULL);
                win32_smoke_feed(p, "C-g");
                app_dev_smoke_isearch(p->app); // stage 4: an isearch with its current match and a lazy highlight
                win32_smoke_feed_no_frame(p, "C-s f o o C-s");
                r_request_capture(p->renderer);
                win32_frame(p);
                if (code3 == EXIT_OK) code3 = win32_smoke_check_frame(p, 4, NULL);
                win32_smoke_feed_no_frame(p, "x"); // stage 5: "foox" fails
                r_request_capture(p->renderer);
                win32_frame(p);
                if (code3 == EXIT_OK) code3 = win32_smoke_check_frame(p, 5, NULL);
                win32_smoke_feed(p, "C-g RET");
                app_dev_smoke_windows(p->app); // stage 6: two windows side by side
                r_request_capture(p->renderer);
                win32_frame(p);
                if (code3 == EXIT_OK) code3 = win32_smoke_check_frame(p, 6, NULL);
                app_dev_smoke_windows_end(p->app);
                app_dev_smoke_finder(p->app); // stage 7: the project finder over a known list, "ap" typed
                win32_smoke_feed_no_frame(p, "a p");
                r_request_capture(p->renderer);
                win32_frame(p);
                if (code3 == EXIT_OK) code3 = win32_smoke_check_frame(p, 7, NULL);
                app_dev_smoke_finder_end(p->app);
                if (code3 == EXIT_OK) { // stage 8: a results buffer over a known tree (the real search, its workers)
                    app_dev_smoke_results(p->app, str8_fmt(&p->perm, "%S\\tmp", p->exe_dir));
                    for (i32 k = 0; k < 5000 && app_dev_results_busy(p->app); k++) {
                        if (os_dev_wake_wait(app_wants_frame(p->app) ? 0 : 5)) app_wake(p->app);
                        win32_frame(p);
                    }
                    r_request_capture(p->renderer);
                    win32_frame(p);
                    code3 = win32_smoke_check_frame(p, 8, NULL);
                    app_dev_smoke_results_end(p->app);
                }
                if (code3 == EXIT_OK) { // stage 9: a build of the --child program (a real process, its worker)
                    app_dev_smoke_compile(p->app, str8_fmt(&p->perm, "%S\\tmp", p->exe_dir));
                    for (i32 k = 0; k < 5000 && app_dev_compile_busy(p->app); k++) {
                        if (os_dev_wake_wait(app_wants_frame(p->app) ? 0 : 5)) app_wake(p->app);
                        win32_frame(p);
                    }
                    for (i32 k = 0; k < 100 && app_wants_frame(p->app); k++) win32_frame(p);
                    r_request_capture(p->renderer);
                    win32_frame(p);
                    code3 = win32_smoke_check_frame(p, 9, NULL);
                    app_dev_smoke_compile_end(p->app);
                }
                if (code3 == EXIT_OK) { // stage 10: three cursors with regions
                    app_dev_smoke_cursors(p->app);
                    r_request_capture(p->renderer);
                    win32_frame(p);
                    code3 = win32_smoke_check_frame(p, 10, NULL);
                    app_dev_smoke_cursors_end(p->app);
                }
                i32 code4 = win32_smoke_check_title(p);
                if (code4 == EXIT_OK) code4 = win32_smoke_check_icon(p);
                i32 code5 = win32_smoke_config_and_keys(p);
                if (code5 == EXIT_OK) code5 = win32_smoke_text_scale(p);
                if (code5 == EXIT_OK) code5 = win32_smoke_end_session(p);
                p->exit_code = code != EXIT_OK ? code : code2 != EXIT_OK ? code2 : code3 != EXIT_OK ? code3
                             : code4 != EXIT_OK ? code4 : code5;
                p->quit = 1;
                break;
            }
            p->redraw = 1;
        }
#endif
        if (p->redraw) win32_frame(p);
    }

    win32_block_shutdown(p, 0); // "exit anyway": Windows' shutdown screen can go on
    u32 leaks = r_shutdown(p->renderer);
    i32 font_refs = app_shutdown(p->app); // DirectWrite references + unreleased buffers
    DestroyWindow(p->hwnd);
#if RONIN_DEV
    if (os_dev_watch_count()) {
        LOG("app: %d directory watch(es) still open after close", os_dev_watch_count());
        font_refs += os_dev_watch_count();
    }
    if (os_dev_live_threads() || os_dev_live_events()) {
        LOG("app: %d worker thread(s) not joined, %d event(s) not destroyed after close", os_dev_live_threads(), os_dev_live_events());
        font_refs += os_dev_live_threads() + os_dev_live_events();
    }
    if (os_dev_live_processes()) {
        LOG("app: %d build process(es) not closed after close", os_dev_live_processes());
        font_refs += os_dev_live_processes();
    }
#endif

#if RONIN_DEV
    LOG("app: resources still held after close (DirectWrite references, buffers): %d", font_refs);
    if (p->smoke || p->leak_check) {
        const char *what = p->smoke ? "smoke" : "leak-check";
        i32 code = p->exit_code;
#if RONIN_D3D_DEBUG
        if (code == EXIT_OK && !r_dev_debug_layer_active(p->renderer)) {
            LOG("%s: FAIL: D3D11 debug layer is not active", what);
            code = EXIT_NO_DEBUG_LAYER;
        }
#else
        LOG("%s: skip: the D3D11 debug layer is off in this build", what);
#endif
        if (code == EXIT_OK && r_dev_message_count(p->renderer) != 0) {
            LOG("%s: FAIL: %u debug-layer message(s)", what, r_dev_message_count(p->renderer));
            code = EXIT_DEBUG_MESSAGES;
        }
        if (code == EXIT_OK && (leaks != 0 || font_refs != 0)) {
            LOG("%s: FAIL: %u leaked D3D reference(s) / live object(s), %d app resource(s) (DirectWrite, buffers)", what, leaks, font_refs);
            code = EXIT_LEAK;
        }
        LOG("%s: %s (exit %d, %D frames)", what, code == EXIT_OK ? "PASS" : "FAIL", code, p->frame_count);
        p->exit_code = code;
    }
    if (p->log_file && p->log_file != INVALID_HANDLE_VALUE) CloseHandle(p->log_file);
#else
    (void)leaks;
    (void)font_refs;
#endif
    return p->exit_code;
}
