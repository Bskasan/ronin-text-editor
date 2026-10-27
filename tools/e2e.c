/* ======================================================================
   ronin: e2e driver: the shipped exe driven with real input

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// The end-to-end driver (#31; build-time tool, not part of the editor; build.bat compiles it on every build, only the
// maintainer runs it, never CI or verify.bat). It drives the shipped ronin.exe as a user does and checks only what a
// user could see or get; it adds nothing to the exe:
//   e2e (--zip <zip> | --exe <ronin.exe>) [--scenario 1,2] [--repeat N] [--out <dir>] [--demo-abort layout|foreground]
// --zip unpacks the dist zip with Windows' tar into build\tmp\e2e\<run>\İndirilenler ronin; --exe copies the exe there
// (a running build\ronin.exe would break the next release link). Every start of ronin gets its own empty APPDATA and
// LOCALAPPDATA (the driver's environment, inherited) and runs in a job that ends it with the driver.
// It takes the keyboard and the mouse (SendInput; keys through VkKeyScanExW for the layout of ronin's thread, with scan
// codes), so by CLAUDE.md it runs only with the maintainer's permission for each run, the machine left alone. It first
// waits for 5 s without physical input, then aborts at once on any physical key or mouse input (low-level hooks, injected
// events ignored), a foreground change away from ronin, or a change of ronin's keyboard layout, each with one line.
// After every key it waits for ronin's next frame (the client area as composed by DWM changed); a step can also wait
// for the title, a saved file or the process's end. No fixed sleeps: every wait has an event and a generous bound.
// The machine's state is recorded before and checked after (also after an abort): the keyboard layouts (the loaded
// list, HKCU\Keyboard Layout\Preload and Substitutes, the User Profile, the active one), the clipboard (every format
// saved byte for byte, put back, read back and compared; Windows clipboard history must be off), the maintainer's
// %APPDATA%\ronin and %LOCALAPPDATA%\ronin, no process of the run left,
// the pointer's position. The log and screenshots (BMP) go to --out, by default build\e2e\<yyyymmdd-hhmmss>.
// --demo-abort shows an abort once: a layout switch posted to ronin, or a second ronin taking the foreground.
// Every line it prints starts with "e2e:"; the last one is "e2e: PASS (...)", "e2e: FAIL: ..." or "e2e: ABORT: ...".
// Exit 0 = pass, 1 = a failed check, 2 = wrong arguments or a precondition not met (nothing typed), 3 = aborted.
// No C runtime calls: Win32 only (the CRT's startup aside, as the other tools).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <dwmapi.h>

typedef unsigned char u8;
typedef unsigned short u16;
typedef int i32;
typedef unsigned int u32;
typedef long long i64;
typedef unsigned long long u64;
typedef int b32;

#define PATH_CAP 1024
#define LINE_CAP 8192
#define ENTRIES_MAX 8192
#define STATE_CAP (1 << 16)
#define SNAP_CAP (1 << 20)
#define CLIP_FORMATS_MAX 64                  // clipboard formats saved
#define CLIP_BYTES_MAX ((SIZE_T)64 << 20)    // and their bytes in all
#define LAYOUTS_MAX 64
#define SCENARIOS 2
// "İndirilenler ronin" (a space and a non-ASCII letter, as a user's Downloads folder), spelled out: build.bat compiles
// the tools without /utf-8 and literals stay ASCII.
static const wchar_t UNPACK_FOLDER[] = { 0x0130, 'n', 'd', 'i', 'r', 'i', 'l', 'e', 'n', 'l', 'e', 'r', ' ', 'r', 'o', 'n', 'i', 'n', 0 };
#define INPUT_TAG 0x7EA1E2E0                       // dwExtraInfo of every injected event

// The bounds: each only turns a hang into a failure.
#define QUIET_MS 5000        // no physical input for this long before the first key
#define QUIET_BOUND_MS 120000
#define FRAME_MS 2000        // a key's frame
#define SETTLE_MS 1000       // the frames after it
#define TITLE_MS 5000
#define FILE_MS 5000
#define WINDOW_MS 10000      // the window, the foreground, the first frame
#define EXIT_MS 10000

// ronin's default theme (themes/ronin.conf), bit-exact on the screen.
#define THEME_BACKGROUND 0x072626u
#define THEME_CURSOR 0x90ee90u
#define THEME_MODE_LINE 0xd3b58du

typedef struct Str {
    const u8 *data;
    i64 len;
} Str;

typedef enum Result { R_OK, R_FAIL, R_ABORT } Result;

// ---------------------------------------------------------------------------
// Strings

static Str str(const u8 *data, i64 len) {
    Str s = { data, len };
    return s;
}

static i64 cstr_len(const char *c) {
    i64 n = 0;
    while (c[n]) n++;
    return n;
}

static Str cstr(const char *c) { return str((const u8 *)c, cstr_len(c)); }

static b32 mem_eq(const u8 *a, const u8 *b, i64 n) {
    for (i64 i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static b32 str_eq(Str a, Str b) { return a.len == b.len && (a.len == 0 || mem_eq(a.data, b.data, a.len)); }

static b32 str_is(Str s, const char *c) { return str_eq(s, cstr(c)); }

static i64 wlen(const wchar_t *w) {
    i64 n = 0;
    while (w[n]) n++;
    return n;
}

static b32 weq(const wchar_t *a, const wchar_t *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

// dst = a, then '\' and b when b is given.
static void wjoin(wchar_t *dst, const wchar_t *a, const wchar_t *b) {
    i64 n = 0;
    for (i64 i = 0; a[i] && n < PATH_CAP - 1; i++) dst[n++] = a[i];
    if (b) {
        if (n && dst[n - 1] != '\\' && n < PATH_CAP - 1) dst[n++] = '\\';
        for (i64 i = 0; b[i] && n < PATH_CAP - 1; i++) dst[n++] = b[i];
    }
    dst[n] = 0;
}

static void wfrom_utf8(wchar_t *dst, i64 cap, Str s) {
    i32 n = s.len ? MultiByteToWideChar(CP_UTF8, 0, (const char *)s.data, (i32)s.len, dst, (i32)(cap - 1)) : 0;
    dst[n < 0 ? 0 : n] = 0;
}

// The code point at s[*at] (UTF-8), *at moved past it; a bad byte is taken as itself.
static u32 utf8_next(Str s, i64 *at) {
    u8 b = s.data[*at];
    i32 n = b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    if (*at + n > s.len) n = 1;
    u32 cp = n == 1 ? b : n == 2 ? b & 0x1Fu : n == 3 ? b & 0x0Fu : b & 0x07u;
    for (i32 i = 1; i < n; i++) cp = (cp << 6) | (s.data[*at + i] & 0x3Fu);
    *at += n;
    return cp;
}

// ---------------------------------------------------------------------------
// Buffers of text: lines, snapshots, the machine's state

typedef struct Buf {
    u8 *data;
    i64 len, cap;
} Buf;

static void bput_n(Buf *b, const u8 *s, i64 n) {
    for (i64 i = 0; i < n && b->len < b->cap - 2; i++) b->data[b->len++] = s[i];
}

static void bput(Buf *b, const char *c) { bput_n(b, (const u8 *)c, cstr_len(c)); }

static void bput_w(Buf *b, const wchar_t *w, i64 n) {
    u8 tmp[PATH_CAP * 3];
    i32 got = n ? WideCharToMultiByte(CP_UTF8, 0, w, (i32)n, (char *)tmp, (i32)sizeof(tmp), NULL, NULL) : 0;
    if (got > 0) bput_n(b, tmp, got);
}

static void bput_i64(Buf *b, i64 v) {
    u8 digits[24];
    i32 n = 0;
    b32 neg = v < 0;
    u64 u = neg ? (u64)(-(v + 1)) + 1 : (u64)v;
    do {
        digits[n++] = (u8)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg) bput(b, "-");
    while (n > 0) bput_n(b, &digits[--n], 1);
}

static void bput_hex(Buf *b, u64 v, i32 digits) {
    static const char hex[] = "0123456789abcdef";
    for (i32 i = digits - 1; i >= 0; i--) bput_n(b, (const u8 *)&hex[(v >> (i * 4)) & 15], 1);
}

// Milliseconds as seconds with one decimal: 12345 -> "12.3 s".
static void bput_secs(Buf *b, u64 ms) {
    bput_i64(b, (i64)(ms / 1000));
    bput(b, ".");
    bput_i64(b, (i64)(ms % 1000 / 100));
    bput(b, " s");
}

static void bput_hkl(Buf *b, HKL h) { bput_hex(b, (u64)(uintptr_t)h & 0xFFFFFFFFu, 8); }

static Buf buf_alloc(i64 cap) {
    Buf b = { (u8 *)VirtualAlloc(NULL, (SIZE_T)cap, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE), 0, cap };
    return b;
}

// ---------------------------------------------------------------------------
// Output: one line at a time, UTF-8, CRLF, to the standard output and the log

static u8 line_mem[LINE_CAP];
static Buf line = { line_mem, 0, LINE_CAP };
static HANDLE log_file = INVALID_HANDLE_VALUE;

static void put(const char *c) { bput(&line, c); }
static void put_w(const wchar_t *w) { bput_w(&line, w, wlen(w)); }
static void put_i64(i64 v) { bput_i64(&line, v); }
static void put_hex(u64 v, i32 digits) { bput_hex(&line, v, digits); }

// UTF-8 text to the standard output: a console gets it as UTF-16 through WriteConsoleW, a file or a pipe as bytes.
static void out_bytes(const u8 *data, i64 len) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode, written;
    if (!GetConsoleMode(out, &mode)) {
        WriteFile(out, data, (DWORD)len, &written, NULL);
        return;
    }
    static wchar_t wide[LINE_CAP];
    i32 w = MultiByteToWideChar(CP_UTF8, 0, (const char *)data, (i32)len, wide, LINE_CAP);
    if (w > 0) WriteConsoleW(out, wide, (DWORD)w, &written, NULL);
}

static void end_line(void) {
    line.data[line.len++] = '\r';
    line.data[line.len++] = '\n';
    out_bytes(line.data, line.len);
    DWORD written;
    if (log_file != INVALID_HANDLE_VALUE) WriteFile(log_file, line.data, (DWORD)line.len, &written, NULL);
    line.len = 0;
}

static void say(const char *a) {
    put(a);
    end_line();
}

// Lines of text (CRLF) as they are, to the standard output and the log.
static void out_text(Buf *b) {
    out_bytes(b->data, b->len);
    DWORD written;
    if (log_file != INVALID_HANDLE_VALUE) WriteFile(log_file, b->data, (DWORD)b->len, &written, NULL);
}

// ---------------------------------------------------------------------------
// Files

// The whole file in fresh pages (free_file releases them); data 0 when it cannot be read.
static Str read_file(const wchar_t *path) {
    Str s = { 0, 0 };
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return s;
    LARGE_INTEGER size;
    if (GetFileSizeEx(f, &size) && size.QuadPart < ((i64)1 << 30)) {
        u8 *data = (u8 *)VirtualAlloc(NULL, (SIZE_T)size.QuadPart + 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        DWORD got = 0;
        if (data && (size.QuadPart == 0 || (ReadFile(f, data, (DWORD)size.QuadPart, &got, NULL) && got == size.QuadPart))) {
            s.data = data;
            s.len = size.QuadPart;
        } else if (data) {
            VirtualFree(data, 0, MEM_RELEASE);
        }
    }
    CloseHandle(f);
    return s;
}

static void free_file(Str s) {
    if (s.data) VirtualFree((void *)s.data, 0, MEM_RELEASE);
}

static b32 write_file(const wchar_t *path, const u8 *data, i64 len) {
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    b32 ok = WriteFile(f, data, (DWORD)len, &written, NULL) && written == (DWORD)len;
    CloseHandle(f);
    return ok;
}

static b32 is_file(const wchar_t *path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static b32 is_dir(const wchar_t *path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void full_path(wchar_t *dst, const wchar_t *path) {
    DWORD n = GetFullPathNameW(path, PATH_CAP, dst, NULL);
    if (n == 0 || n >= PATH_CAP) wjoin(dst, path, NULL);
}

static b32 make_dir(const wchar_t *path) { return CreateDirectoryW(path, NULL) || GetLastError() == ERROR_ALREADY_EXISTS; }

// Removes everything below dir and dir itself; a junction or a link is removed, never followed.
static void remove_tree(const wchar_t *dir) {
    wchar_t pattern[PATH_CAP];
    wjoin(pattern, dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (weq(fd.cFileName, L".") || weq(fd.cFileName, L"..")) continue;
            wchar_t child[PATH_CAP];
            wjoin(child, dir, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) remove_tree(child);
                else RemoveDirectoryW(child);
            } else {
                SetFileAttributesW(child, FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(child);
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    RemoveDirectoryW(dir);
}

// ---------------------------------------------------------------------------
// Walking a folder: every entry below it as "rel/path" (folders with a trailing '/'), sorted

typedef struct Entry {
    u8 path[PATH_CAP];
    i32 path_len;
    b32 folder;
    u64 size, written, hash;
    b32 unreadable;
} Entry;

typedef struct Walk {
    Entry *items;
    i32 count;
    b32 full; // more than ENTRIES_MAX
} Walk;

static u64 hash_file(const wchar_t *path, b32 *unreadable) {
    u64 h = 0xcbf29ce484222325ull;
    HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) {
        *unreadable = 1;
        return 0;
    }
    static u8 chunk[1 << 16];
    DWORD got;
    while (ReadFile(f, chunk, sizeof(chunk), &got, NULL) && got) {
        for (DWORD i = 0; i < got; i++) h = (h ^ chunk[i]) * 0x100000001b3ull;
    }
    CloseHandle(f);
    return h;
}

static void walk_dir(Walk *w, const wchar_t *dir, const u8 *rel, i32 rel_len) {
    wchar_t pattern[PATH_CAP];
    wjoin(pattern, dir, L"*");
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        if (weq(fd.cFileName, L".") || weq(fd.cFileName, L"..")) continue;
        if (w->count >= ENTRIES_MAX) {
            w->full = 1;
            break;
        }
        Entry *e = &w->items[w->count++];
        memset(e, 0, sizeof(*e));
        if (rel_len) {
            memcpy(e->path, rel, (size_t)rel_len);
            e->path[rel_len] = '/';
            e->path_len = rel_len + 1;
        }
        i32 n = WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, (i32)wlen(fd.cFileName), (char *)e->path + e->path_len,
                                    PATH_CAP - e->path_len - 2, NULL, NULL);
        e->path_len += n > 0 ? n : 0;
        e->folder = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e->written = ((u64)fd.ftLastWriteTime.dwHighDateTime << 32) | fd.ftLastWriteTime.dwLowDateTime;
        wchar_t child[PATH_CAP];
        wjoin(child, dir, fd.cFileName);
        if (e->folder) {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) walk_dir(w, child, e->path, e->path_len);
        } else {
            e->size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            e->hash = hash_file(child, &e->unreadable);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

static b32 entry_less(const Entry *a, const Entry *b) {
    i32 n = a->path_len < b->path_len ? a->path_len : b->path_len;
    for (i32 i = 0; i < n; i++) if (a->path[i] != b->path[i]) return a->path[i] < b->path[i];
    return a->path_len < b->path_len;
}

static Walk walk_buf;

static void walk(const wchar_t *dir) {
    Walk *w = &walk_buf;
    if (!w->items) w->items = (Entry *)VirtualAlloc(NULL, sizeof(Entry) * ENTRIES_MAX, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    w->count = 0;
    w->full = 0;
    walk_dir(w, dir, NULL, 0);
    for (i32 i = 1; i < w->count; i++) { // insertion sort: a few entries
        Entry tmp = w->items[i];
        i32 j = i - 1;
        while (j >= 0 && entry_less(&tmp, &w->items[j])) {
            w->items[j + 1] = w->items[j];
            j--;
        }
        w->items[j + 1] = tmp;
    }
}

// A folder as text: one line per entry (path, size, last write time, FNV-1a 64), or "(missing)".
static void snapshot(Buf *b, const wchar_t *dir) {
    b->len = 0;
    if (!is_dir(dir)) {
        bput(b, "(missing)\r\n");
        return;
    }
    walk(dir);
    for (i32 i = 0; i < walk_buf.count; i++) {
        Entry *e = &walk_buf.items[i];
        bput_n(b, e->path, e->path_len);
        if (e->folder) {
            bput(b, "/");
        } else {
            bput(b, " ");
            bput_i64(b, (i64)e->size);
            bput(b, " ");
            bput_hex(b, e->written, 16);
            bput(b, " ");
            if (e->unreadable) bput(b, "(unreadable)");
            else bput_hex(b, e->hash, 16);
        }
        bput(b, "\r\n");
    }
    if (walk_buf.full) bput(b, "(more entries than the snapshot holds)\r\n");
}

static b32 buf_eq(Buf *a, Buf *b) { return a->len == b->len && mem_eq(a->data, b->data, a->len); }

// ---------------------------------------------------------------------------
// The watch thread: the low-level hooks (physical input) and the WinEvent hooks (foreground, title, shown windows)

typedef struct Watch {
    volatile LONG aborted;      // 1 once an abort was reported (the first one wins)
    u8 abort_mem[1024];         // its description, written before `aborted`
    Buf abort_text;
    volatile LONG armed;        // 0: physical input only restarts the quiet wait; 1: it aborts
    volatile LONG pid;          // ronin's process (0 between starts)
    volatile LONG fg_armed;     // the foreground leaving ronin aborts
    volatile LONG exiting;      // ronin is being ended: the foreground may go
    volatile LONG64 last_physical;
    volatile LONG shows, names, foregrounds; // window events seen for ronin's process (for a failure's line)
    HANDLE abort_event, quiet_event, show_event, title_event, fg_event, ready;
    DWORD thread_id;
    HANDLE thread;
} Watch;

static Watch watch;

// The first abort's description; it also wakes every wait. Any thread.
static void watch_abort(Buf *desc) {
    static volatile LONG claimed;
    if (InterlockedCompareExchange(&claimed, 1, 0) != 0) return;
    watch.abort_text.len = 0;
    bput_n(&watch.abort_text, desc->data, desc->len);
    InterlockedExchange(&watch.aborted, 1);
    SetEvent(watch.abort_event);
}

static b32 aborted(void) { return watch.aborted != 0; }

static void watch_physical(Buf *desc) {
    if (!watch.armed) {
        InterlockedExchange64(&watch.last_physical, (LONG64)GetTickCount64());
        SetEvent(watch.quiet_event);
        return;
    }
    watch_abort(desc);
}

static LRESULT CALLBACK watch_key(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        if (!(k->flags & LLKHF_INJECTED)) {
            u8 mem[128];
            Buf d = { mem, 0, sizeof(mem) };
            bput(&d, "a physical key (VK 0x");
            bput_hex(&d, k->vkCode, 2);
            bput(&d, ", scan 0x");
            bput_hex(&d, k->scanCode, 2);
            bput(&d, (k->flags & LLKHF_UP) ? ", up)" : ", down)");
            watch_physical(&d);
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

static LRESULT CALLBACK watch_mouse(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        MSLLHOOKSTRUCT *m = (MSLLHOOKSTRUCT *)lp;
        if (!(m->flags & LLMHF_INJECTED)) {
            u8 mem[128];
            Buf d = { mem, 0, sizeof(mem) };
            if (wp == WM_MOUSEMOVE) bput(&d, "a physical mouse move");
            else if (wp == WM_MOUSEWHEEL || wp == WM_MOUSEHWHEEL) bput(&d, "a physical mouse wheel");
            else {
                bput(&d, "a physical mouse button (message 0x");
                bput_hex(&d, (u64)wp, 4);
                bput(&d, ")");
            }
            watch_physical(&d);
        }
    }
    return CallNextHookEx(NULL, code, wp, lp);
}

// "<title>" (<exe>, pid N) of a window, into d.
static void describe_window(Buf *d, HWND hwnd) {
    wchar_t title[256];
    title[0] = 0;
    if (hwnd) GetWindowTextW(hwnd, title, 256);
    DWORD pid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
    wchar_t image[PATH_CAP];
    image[0] = 0;
    HANDLE h = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : NULL;
    if (h) {
        DWORD n = PATH_CAP;
        if (!QueryFullProcessImageNameW(h, 0, image, &n)) image[0] = 0;
        CloseHandle(h);
    }
    const wchar_t *base = image;
    for (const wchar_t *c = image; *c; c++) if (*c == '\\') base = c + 1;
    bput(d, "\"");
    bput_w(d, title, wlen(title));
    bput(d, "\" (");
    bput_w(d, base[0] ? base : L"?", base[0] ? wlen(base) : 1);
    bput(d, ", pid ");
    bput_i64(d, pid);
    bput(d, ")");
}

static void CALLBACK watch_event(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child, DWORD thread, DWORD ms) {
    (void)hook, (void)child, (void)thread, (void)ms;
    DWORD pid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
    DWORD watched = (DWORD)watch.pid;
    if (event == EVENT_SYSTEM_FOREGROUND) {
        if (watched && pid == watched) {
            InterlockedIncrement(&watch.foregrounds);
            SetEvent(watch.fg_event);
        } else if (watch.fg_armed && !watch.exiting) {
            u8 mem[768];
            Buf d = { mem, 0, sizeof(mem) };
            bput(&d, "the foreground went to ");
            describe_window(&d, hwnd);
            bput(&d, "; ronin under test is pid ");
            bput_i64(&d, watched);
            watch_abort(&d);
        }
    } else if (watched && pid == watched && object == OBJID_WINDOW) {
        InterlockedIncrement(event == EVENT_OBJECT_NAMECHANGE ? &watch.names : &watch.shows);
        SetEvent(event == EVENT_OBJECT_NAMECHANGE ? watch.title_event : watch.show_event);
    }
}

static DWORD WINAPI watch_main(LPVOID param) {
    (void)param;
    HINSTANCE self = GetModuleHandleW(NULL);
    HHOOK key = SetWindowsHookExW(WH_KEYBOARD_LL, watch_key, self, 0);
    HHOOK mouse = SetWindowsHookExW(WH_MOUSE_LL, watch_mouse, self, 0);
    DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    HWINEVENTHOOK fg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, watch_event, 0, 0, flags);
    HWINEVENTHOOK show = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, NULL, watch_event, 0, 0, flags);
    HWINEVENTHOOK name = SetWinEventHook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE, NULL, watch_event, 0, 0, flags);
    b32 ok = key && mouse && fg && show && name;
    MSG m;
    PeekMessageW(&m, NULL, 0, 0, PM_NOREMOVE); // the queue exists before the main thread may post WM_QUIT
    if (ok) InterlockedExchange(&watch.armed, 0);
    SetEvent(watch.ready);
    if (ok) {
        while (GetMessageW(&m, NULL, 0, 0) > 0) DispatchMessageW(&m);
    }
    if (key) UnhookWindowsHookEx(key);
    if (mouse) UnhookWindowsHookEx(mouse);
    if (fg) UnhookWinEvent(fg);
    if (show) UnhookWinEvent(show);
    if (name) UnhookWinEvent(name);
    return ok ? 0 : 1;
}

static b32 watch_start(void) {
    watch.abort_text.data = watch.abort_mem;
    watch.abort_text.cap = sizeof(watch.abort_mem);
    watch.abort_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    watch.quiet_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    watch.show_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    watch.title_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    watch.fg_event = CreateEventW(NULL, FALSE, FALSE, NULL);
    watch.ready = CreateEventW(NULL, TRUE, FALSE, NULL);
    watch.thread = CreateThread(NULL, 0, watch_main, NULL, 0, &watch.thread_id);
    if (!watch.thread) return 0;
    HANDLE both[2] = { watch.ready, watch.thread };
    return WaitForMultipleObjects(2, both, FALSE, 10000) == WAIT_OBJECT_0;
}

static void watch_stop(void) {
    if (!watch.thread) return;
    PostThreadMessageW(watch.thread_id, WM_QUIT, 0, 0);
    WaitForSingleObject(watch.thread, 5000);
    CloseHandle(watch.thread);
    watch.thread = NULL;
}

static BOOL WINAPI console_ctrl(DWORD type) {
    (void)type;
    u8 mem[96];
    Buf d = { mem, 0, sizeof(mem) };
    bput(&d, "Ctrl+C (or the console closing) on the driver's console");
    watch_abort(&d);
    return TRUE;
}

// ---------------------------------------------------------------------------
// Waiting on this thread: its messages pumped meanwhile (the clipboard window gets WM_DESTROYCLIPBOARD from ronin)

typedef enum Wait { W_OK, W_TIMEOUT, W_ABORT } Wait;

static void pump(void) {
    MSG m;
    while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// Until h is signaled (W_OK), the deadline (GetTickCount64) passes, or, when abortable, an abort.
static Wait wait_handle(HANDLE h, u64 deadline, b32 abortable) {
    for (;;) {
        pump();
        if (abortable && aborted()) return W_ABORT;
        if (h && WaitForSingleObject(h, 0) == WAIT_OBJECT_0) return W_OK;
        u64 now = GetTickCount64();
        if (now >= deadline) return W_TIMEOUT;
        HANDLE hs[2];
        DWORD n = 0;
        if (abortable) hs[n++] = watch.abort_event;
        if (h) hs[n++] = h;
        MsgWaitForMultipleObjectsEx(n, n ? hs : NULL, (DWORD)(deadline - now), QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

// One DWM composition (the screen's next state), this thread's messages pumped.
static void tick(void) {
    DwmFlush();
    pump();
}

// ---------------------------------------------------------------------------
// The run

typedef enum Demo { DEMO_NONE, DEMO_LAYOUT, DEMO_FOREGROUND } Demo;

typedef struct Run {
    wchar_t root[PATH_CAP];       // the repository (the parent of build\, where e2e.exe is)
    wchar_t work[PATH_CAP];       // build\tmp\e2e
    wchar_t exe_dir[PATH_CAP];    // ...\İndirilenler ronin\ronin-<version>
    wchar_t exe[PATH_CAP];
    wchar_t out[PATH_CAP];
    wchar_t real_appdata[PATH_CAP], real_localappdata[PATH_CAP];
    HANDLE job;
    HWND clip_window;
    Demo demo;
    i32 repeat_index;
} Run;

static Run run;

// A start of ronin and what the driver knows of it.
typedef struct Session {
    const char *name;             // "s1"
    const char *part;             // the start within the scenario ("crlf.c"), or NULL
    i32 step;
    wchar_t dir[PATH_CAP];        // the scenario run's folder: work\, appdata\, localappdata\ below it
    wchar_t cwd[PATH_CAP];
    PROCESS_INFORMATION pi;
    b32 running;
    HWND hwnd;
    DWORD tid;
    HKL hkl;                      // ronin's layout for this start
    POINT origin;                 // the client area on the screen
    i32 w, h;
    HDC screen, mem;
    HBITMAP bmp, old;
    u32 *dib;                     // the capture's pixels (0x00RRGGBB, top-down)
    u32 *frame, *cur, *spare;     // the settled frame; captures
    i32 cell_w, cell_h;           // measured from the first frame's cursor block
    RECT cursor0;
    i32 shots;
} Session;

static u8 where_mem[512];
static Buf where = { where_mem, 0, sizeof(where_mem) }; // the last key sent: "\"M-f\" (s2 step 9)"

static void where_set(Session *s, const char *key) {
    where.len = 0;
    bput(&where, "\"");
    bput(&where, key);
    bput(&where, "\" (");
    bput(&where, s->name);
    bput(&where, " step ");
    bput_i64(&where, s->step);
    bput(&where, ")");
}

static void put_prefix(Session *s) {
    put("e2e: ");
    put(s->name);
    put(": ");
}

static void session_dir_path(Session *s, wchar_t *dst, const wchar_t *sub) { wjoin(dst, s->dir, sub); }

// Saves the last capture as <out>\<scenario>-r<run>-<n>-<label>.bmp; prints its name.
static void save_shot(Session *s, const u32 *px, const char *label) {
    if (!px || !s->w || !s->h) return;
    u8 name_mem[256];
    Buf name = { name_mem, 0, sizeof(name_mem) };
    bput(&name, s->name);
    bput(&name, "-r");
    bput_i64(&name, run.repeat_index);
    if (s->part) {
        bput(&name, "-");
        bput(&name, s->part);
    }
    bput(&name, "-");
    bput_i64(&name, ++s->shots);
    bput(&name, "-");
    bput(&name, label);
    bput(&name, ".bmp");
    wchar_t wname[256], path[PATH_CAP];
    wfrom_utf8(wname, 256, str(name.data, name.len));
    wjoin(path, run.out, wname);
    BITMAPFILEHEADER fh;
    BITMAPINFOHEADER ih;
    memset(&fh, 0, sizeof(fh));
    memset(&ih, 0, sizeof(ih));
    DWORD pixels = (DWORD)s->w * (DWORD)s->h * 4;
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + pixels;
    ih.biSize = sizeof(ih);
    ih.biWidth = s->w;
    ih.biHeight = -s->h;
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD written;
    WriteFile(f, &fh, sizeof(fh), &written, NULL);
    WriteFile(f, &ih, sizeof(ih), &written, NULL);
    WriteFile(f, px, pixels, &written, NULL);
    CloseHandle(f);
    put_prefix(s);
    put("screenshot ");
    put_w(path);
    end_line();
}

// A failed check: the line (already begun with put_prefix and the message), then the screenshot.
static Result fail_end(Session *s) {
    end_line();
    save_shot(s, s->cur ? s->cur : NULL, "fail");
    return R_FAIL;
}

// ---------------------------------------------------------------------------
// The screen: ronin's client area as composed, copied from the screen DC

static b32 capture_setup(Session *s) {
    RECT rc;
    if (!GetClientRect(s->hwnd, &rc)) return 0;
    POINT o = { 0, 0 };
    ClientToScreen(s->hwnd, &o);
    s->origin = o;
    s->w = rc.right - rc.left;
    s->h = rc.bottom - rc.top;
    if (s->w <= 0 || s->h <= 0) return 0;
    s->screen = GetDC(NULL);
    s->mem = CreateCompatibleDC(s->screen);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = s->w;
    bi.bmiHeader.biHeight = -s->h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    s->bmp = CreateDIBSection(s->mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!s->bmp) return 0;
    s->old = (HBITMAP)SelectObject(s->mem, s->bmp);
    s->dib = (u32 *)bits;
    SIZE_T bytes = (SIZE_T)s->w * (SIZE_T)s->h * 4;
    s->frame = (u32 *)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s->cur = (u32 *)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    s->spare = (u32 *)VirtualAlloc(NULL, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return s->frame && s->cur && s->spare;
}

static void capture_release(Session *s) {
    if (s->mem) {
        SelectObject(s->mem, s->old);
        DeleteObject(s->bmp);
        DeleteDC(s->mem);
    }
    if (s->screen) ReleaseDC(NULL, s->screen);
    if (s->frame) VirtualFree(s->frame, 0, MEM_RELEASE);
    if (s->cur) VirtualFree(s->cur, 0, MEM_RELEASE);
    if (s->spare) VirtualFree(s->spare, 0, MEM_RELEASE);
    s->mem = NULL;
    s->screen = NULL;
    s->frame = s->cur = s->spare = NULL;
}

static u8 covered_mem[512];
static Buf covered = { covered_mem, 0, sizeof(covered_mem) }; // what covers ronin's window, after a failed capture

// The client area into dst; 0 when another window covers part of it (named in `covered`).
static b32 capture(Session *s, u32 *dst) {
    POINT probes[5] = { { s->origin.x + 1, s->origin.y + 1 }, { s->origin.x + s->w - 2, s->origin.y + 1 },
                        { s->origin.x + 1, s->origin.y + s->h - 2 }, { s->origin.x + s->w - 2, s->origin.y + s->h - 2 },
                        { s->origin.x + s->w / 2, s->origin.y + s->h / 2 } };
    for (i32 i = 0; i < 5; i++) {
        HWND at = WindowFromPoint(probes[i]);
        HWND top = at ? GetAncestor(at, GA_ROOT) : NULL;
        if (top != s->hwnd) {
            covered.len = 0;
            describe_window(&covered, top);
            return 0;
        }
    }
    if (!BitBlt(s->mem, 0, 0, s->w, s->h, s->screen, s->origin.x, s->origin.y, SRCCOPY)) return 0;
    GdiFlush();
    i64 n = (i64)s->w * s->h;
    for (i64 i = 0; i < n; i++) dst[i] = s->dib[i] & 0xFFFFFFu;
    return 1;
}

static b32 frames_eq(const u32 *a, const u32 *b, i64 n) {
    for (i64 i = 0; i < n; i++) if (a[i] != b[i]) return 0;
    return 1;
}

static u32 pixel(Session *s, const u32 *px, i32 x, i32 y) {
    if (x < 0 || y < 0 || x >= s->w || y >= s->h) return 0xFFFFFFFFu;
    return px[(i64)y * s->w + x];
}

// The bounding box of every pixel in color c; 0 when there is none.
static b32 color_box(Session *s, const u32 *px, u32 c, RECT *box) {
    b32 any = 0;
    for (i32 y = 0; y < s->h; y++) {
        for (i32 x = 0; x < s->w; x++) {
            if (px[(i64)y * s->w + x] != c) continue;
            if (!any) {
                box->left = box->right = x;
                box->top = box->bottom = y;
                any = 1;
            }
            if (x < box->left) box->left = x;
            if (x > box->right) box->right = x;
            if (y < box->top) box->top = y;
            if (y > box->bottom) box->bottom = y;
        }
    }
    return any;
}

static void swap_frames(u32 **a, u32 **b) {
    u32 *t = *a;
    *a = *b;
    *b = t;
}

// Once the screen has changed: the captures at the next compositions until one equals the one before it; that one is
// the settled frame (s->frame).
static Wait settle(Session *s, u64 deadline) {
    i64 n = (i64)s->w * s->h;
    for (;;) {
        if (aborted()) return W_ABORT;
        tick();
        if (!capture(s, s->spare)) return W_TIMEOUT;
        b32 same = frames_eq(s->spare, s->cur, n);
        swap_frames(&s->cur, &s->spare);
        if (same) break;
        if (GetTickCount64() >= deadline) return W_TIMEOUT;
    }
    memcpy(s->frame, s->cur, (size_t)n * 4);
    return W_OK;
}

// After a key: the client area at each composition until it differs from the settled frame, then settled again.
static Result wait_frame(Session *s) {
    i64 n = (i64)s->w * s->h;
    u64 deadline = GetTickCount64() + FRAME_MS;
    for (;;) {
        if (aborted()) return R_ABORT;
        tick();
        if (!capture(s, s->cur)) {
            put_prefix(s);
            put("step ");
            put_i64(s->step);
            put(": the window is covered by ");
            bput_n(&line, covered.data, covered.len);
            return fail_end(s);
        }
        if (!frames_eq(s->cur, s->frame, n)) break;
        if (GetTickCount64() >= deadline) {
            put_prefix(s);
            put("no new frame within 2 s after ");
            bput_n(&line, where.data, where.len);
            return fail_end(s);
        }
    }
    Wait w = settle(s, GetTickCount64() + SETTLE_MS);
    if (w == W_ABORT) return R_ABORT;
    if (w == W_TIMEOUT) {
        put_prefix(s);
        put("the frames did not settle within 1 s after ");
        bput_n(&line, where.data, where.len);
        return fail_end(s);
    }
    return R_OK;
}

// ---------------------------------------------------------------------------
// Keys and the mouse through SendInput

enum { KM_CTRL = 1, KM_META = 2, KM_SHIFT = 4, KM_ALTGR = 8 };

typedef struct Stroke {
    WORD vk;
    u32 mods;
} Stroke;

// The key and modifiers that type c on hkl (VkKeyScanExW); 0 when the layout has none.
static b32 char_stroke(HKL hkl, u32 c, Stroke *st) {
    if (c > 0xFFFF) return 0;
    SHORT r = VkKeyScanExW((WCHAR)c, hkl);
    if (r == -1) return 0;
    u32 shift = (u32)(r >> 8) & 0xFF;
    if (shift & ~7u) return 0; // Hankaku and other states: not a plain key
    st->vk = (WORD)(r & 0xFF);
    st->mods = 0;
    if (shift & 1) st->mods |= KM_SHIFT;
    if ((shift & 6) == 6) st->mods |= KM_ALTGR;
    else if (shift & 2) st->mods |= KM_CTRL;
    else if (shift & 4) st->mods |= KM_META;
    return 1;
}

static const struct { const char *name; WORD vk; } named_keys[] = {
    { "RET", VK_RETURN }, { "SPC", VK_SPACE }, { "TAB", VK_TAB }, { "DEL", VK_BACK }, { "ESC", VK_ESCAPE },
    { "<left>", VK_LEFT }, { "<right>", VK_RIGHT }, { "<up>", VK_UP }, { "<down>", VK_DOWN },
    { "<home>", VK_HOME }, { "<end>", VK_END }, { "<prior>", VK_PRIOR }, { "<next>", VK_NEXT }, { "<delete>", VK_DELETE },
};

// One chord in kbd notation ("C-x", "M-%", "RET", "a"); 0 when it is not one or the layout cannot type it.
static b32 parse_stroke(HKL hkl, Str t, Stroke *st) {
    u32 mods = 0;
    while (t.len > 2 && t.data[1] == '-' && (t.data[0] == 'C' || t.data[0] == 'M' || t.data[0] == 'S')) {
        mods |= t.data[0] == 'C' ? KM_CTRL : t.data[0] == 'M' ? KM_META : KM_SHIFT;
        t = str(t.data + 2, t.len - 2);
    }
    for (i32 i = 0; i < (i32)(sizeof(named_keys) / sizeof(named_keys[0])); i++) {
        if (str_is(t, named_keys[i].name)) {
            st->vk = named_keys[i].vk;
            st->mods = mods;
            return 1;
        }
    }
    i64 at = 0;
    u32 c = utf8_next(t, &at);
    if (at != t.len || !char_stroke(hkl, c, st)) return 0;
    st->mods |= mods;
    return 1;
}

static b32 vk_extended(WORD vk) {
    return vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_HOME || vk == VK_END ||
           vk == VK_PRIOR || vk == VK_NEXT || vk == VK_DELETE || vk == VK_INSERT;
}

static void key_input(INPUT *in, WORD vk, WORD scan, b32 extended, b32 up) {
    memset(in, 0, sizeof(*in));
    in->type = INPUT_KEYBOARD;
    in->ki.wVk = vk;
    in->ki.wScan = scan;
    in->ki.dwFlags = (extended ? KEYEVENTF_EXTENDEDKEY : 0) | (up ? KEYEVENTF_KEYUP : 0);
    in->ki.dwExtraInfo = (ULONG_PTR)INPUT_TAG;
}

// One SendInput call with all of the stroke's events: modifiers down, the key down and up, modifiers up. No key is ever
// left held. Modifiers are the left ones (as typed), AltGr the right Alt (the layout adds its Ctrl).
static void send_stroke(HKL hkl, Stroke st) {
    static const struct { u32 mod; WORD vk; WORD scan; b32 ext; } mods[] = {
        { KM_CTRL, VK_CONTROL, 0x1D, 0 }, { KM_META, VK_MENU, 0x38, 0 }, { KM_SHIFT, VK_SHIFT, 0x2A, 0 },
        { KM_ALTGR, VK_MENU, 0x38, 1 },
    };
    INPUT in[12];
    UINT n = 0;
    for (i32 i = 0; i < 4; i++) if (st.mods & mods[i].mod) key_input(&in[n++], mods[i].vk, mods[i].scan, mods[i].ext, 0);
    UINT sc = MapVirtualKeyExW(st.vk, MAPVK_VK_TO_VSC_EX, hkl);
    b32 ext = vk_extended(st.vk) || (sc & 0xFF00) == 0xE000;
    key_input(&in[n++], st.vk, (WORD)(sc & 0xFF), ext, 0);
    key_input(&in[n++], st.vk, (WORD)(sc & 0xFF), ext, 1);
    for (i32 i = 3; i >= 0; i--) if (st.mods & mods[i].mod) key_input(&in[n++], mods[i].vk, mods[i].scan, mods[i].ext, 1);
    SendInput(n, in, sizeof(INPUT));
}

// The pointer to a point of the virtual screen (absolute, injected).
static void mouse_move(i32 x, i32 y) {
    i32 vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    i32 vw = GetSystemMetrics(SM_CXVIRTUALSCREEN), vh = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    INPUT in;
    memset(&in, 0, sizeof(in));
    in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(((i64)(x - vx) * 65535 + (vw - 1) / 2) / (vw > 1 ? vw - 1 : 1));
    in.mi.dy = (LONG)(((i64)(y - vy) * 65535 + (vh - 1) / 2) / (vh > 1 ? vh - 1 : 1));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    in.mi.dwExtraInfo = (ULONG_PTR)INPUT_TAG;
    SendInput(1, &in, sizeof(INPUT));
}

// ---------------------------------------------------------------------------
// Steps: each key checked before (the abort, the foreground and focus, the layout), then its handshake, then the
// layout again

static Result layout_abort(Session *s, HKL now) {
    u8 mem[256];
    Buf d = { mem, 0, sizeof(mem) };
    bput(&d, "the keyboard layout of ronin's thread switched from ");
    bput_hkl(&d, s->hkl);
    bput(&d, " to ");
    bput_hkl(&d, now);
    watch_abort(&d);
    return R_ABORT;
}

static Result before_key(Session *s) {
    if (aborted()) return R_ABORT;
    HWND fg = GetForegroundWindow();
    if (fg != s->hwnd) {
        u8 mem[768];
        Buf d = { mem, 0, sizeof(mem) };
        bput(&d, "the foreground is ");
        describe_window(&d, fg);
        bput(&d, ", not ronin's window");
        watch_abort(&d);
        return R_ABORT;
    }
    GUITHREADINFO gui;
    memset(&gui, 0, sizeof(gui));
    gui.cbSize = sizeof(gui);
    if (GetGUIThreadInfo(s->tid, &gui) && gui.hwndFocus != s->hwnd) {
        u8 mem[128];
        Buf d = { mem, 0, sizeof(mem) };
        bput(&d, "the keyboard focus left ronin's window");
        watch_abort(&d);
        return R_ABORT;
    }
    HKL now = GetKeyboardLayout(s->tid);
    if (now != s->hkl) return layout_abort(s, now);
    return R_OK;
}

static Result after_step(Session *s) {
    if (aborted()) return R_ABORT;
    if (!s->running) return R_OK;
    HKL now = GetKeyboardLayout(s->tid);
    if (now != s->hkl) return layout_abort(s, now);
    return R_OK;
}

// The checks first, so an abort found before this key names the last key sent ("where" still holds it).
static Result send_key(Session *s, const char *key) {
    Stroke st;
    if (!parse_stroke(s->hkl, cstr(key), &st)) {
        put_prefix(s);
        put("step ");
        put_i64(s->step + 1);
        put(": \"");
        put(key);
        put("\" cannot be typed on layout ");
        bput_hkl(&line, s->hkl);
        return fail_end(s);
    }
    Result r = before_key(s);
    if (r != R_OK) return r;
    s->step++;
    where_set(s, key);
    send_stroke(s->hkl, st);
    return R_OK;
}

// One key, its frame.
static Result key(Session *s, const char *k) {
    Result r = send_key(s, k);
    if (r == R_OK) r = wait_frame(s);
    if (r == R_OK) r = after_step(s);
    return r;
}

// Each character of text typed as its own key, each with its frame.
static Result type(Session *s, const char *text) {
    Str t = cstr(text);
    for (i64 at = 0; at < t.len;) {
        i64 from = at;
        utf8_next(t, &at);
        char one[8];
        i64 n = at - from;
        memcpy(one, t.data + from, (size_t)n);
        one[n] = 0;
        Result r = key(s, one);
        if (r != R_OK) return r;
    }
    return R_OK;
}

// The window's title until it equals expected: looked at again at each composition (the title is set with a frame).
static Result wait_title(Session *s, const wchar_t *expected) {
    u64 deadline = GetTickCount64() + TITLE_MS;
    wchar_t title[256];
    for (;;) {
        title[0] = 0;
        GetWindowTextW(s->hwnd, title, 256);
        if (weq(title, expected)) return R_OK;
        if (aborted()) return R_ABORT;
        if (GetTickCount64() >= deadline) {
            put_prefix(s);
            put("the title is \"");
            put_w(title);
            put("\", not \"");
            put_w(expected);
            put("\", 5 s after ");
            bput_n(&line, where.data, where.len);
            put(" (window events seen for ronin: ");
            put_i64(watch.shows);
            put(" shown, ");
            put_i64(watch.names);
            put(" name changes, ");
            put_i64(watch.foregrounds);
            put(" foreground)");
            return fail_end(s);
        }
        tick();
    }
}

// A key that visits a file: its frame, then the title.
static Result key_title(Session *s, const char *k, const wchar_t *expected) {
    Result r = key(s, k);
    if (r == R_OK) r = wait_title(s, expected);
    if (r == R_OK) {
        put_prefix(s);
        put("title \"");
        put_w(expected);
        put("\"");
        end_line();
    }
    return r;
}

// got against expected, byte for byte; the first difference named.
static Result compare_bytes(Session *s, const char *name, Str got, Str expected) {
    i64 n = got.len < expected.len ? got.len : expected.len;
    i64 i = 0;
    while (i < n && got.data[i] == expected.data[i]) i++;
    if (i == n && got.len == expected.len) {
        put_prefix(s);
        put(name);
        put(": ");
        put_i64(got.len);
        put(" bytes, as expected");
        end_line();
        return R_OK;
    }
    put_prefix(s);
    put(name);
    put(": ");
    if (i < n) {
        put("byte ");
        put_i64(i);
        put(" is 0x");
        put_hex(got.data[i], 2);
        put(", expected 0x");
        put_hex(expected.data[i], 2);
        put(" (");
    } else {
        put("equal up to byte ");
        put_i64(n);
        put(" (");
    }
    put("saved ");
    put_i64(got.len);
    put(" bytes, expected ");
    put_i64(expected.len);
    put(")");
    return fail_end(s);
}

// A key that saves `name` in the working folder: its frame, then the file changed (a change notification on the
// folder, armed before the key), then its bytes against expected.
static Result key_save(Session *s, const char *k, const char *name, Str expected) {
    wchar_t wname[PATH_CAP], path[PATH_CAP];
    wfrom_utf8(wname, PATH_CAP, cstr(name));
    wjoin(path, s->cwd, wname);
    Str before = read_file(path);
    HANDLE note = FindFirstChangeNotificationW(s->cwd, FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                                                                 FILE_NOTIFY_CHANGE_SIZE);
    Result r = key(s, k);
    u64 deadline = GetTickCount64() + FILE_MS;
    Str got = { 0, 0 };
    while (r == R_OK) {
        got = read_file(path);
        b32 changed = got.data && (!before.data || !str_eq(got, before));
        if (changed) break;
        free_file(got);
        got.data = NULL;
        Wait w = note != INVALID_HANDLE_VALUE ? wait_handle(note, deadline, 1) : W_TIMEOUT;
        if (w == W_ABORT) r = R_ABORT;
        else if (w == W_TIMEOUT) {
            put_prefix(s);
            put(name);
            put(": not written within 5 s after ");
            bput_n(&line, where.data, where.len);
            r = fail_end(s);
        } else {
            FindNextChangeNotification(note);
        }
    }
    if (note != INVALID_HANDLE_VALUE) FindCloseChangeNotification(note);
    free_file(before);
    if (r == R_OK) r = compare_bytes(s, name, got, expected);
    free_file(got);
    if (r == R_OK) save_shot(s, s->frame, "saved");
    return r;
}

// A key that ends ronin: the process's end and its exit code 0.
static Result key_exit(Session *s, const char *k) {
    InterlockedExchange(&watch.exiting, 1);
    Result r = send_key(s, k);
    if (r != R_OK) return r;
    Wait w = wait_handle(s->pi.hProcess, GetTickCount64() + EXIT_MS, 1);
    if (w == W_ABORT) return R_ABORT;
    if (w == W_TIMEOUT) {
        put_prefix(s);
        put("still running 10 s after ");
        bput_n(&line, where.data, where.len);
        return fail_end(s);
    }
    s->running = 0;
    InterlockedExchange(&watch.pid, 0);
    DWORD code = 0;
    GetExitCodeProcess(s->pi.hProcess, &code);
    put_prefix(s);
    put("ronin ended, exit code ");
    put_i64(code);
    end_line();
    if (code != 0) {
        put_prefix(s);
        put("exit code ");
        put_i64(code);
        put(", expected 0");
        return fail_end(s);
    }
    return R_OK;
}

// ---------------------------------------------------------------------------
// Starting ronin

typedef struct FindCtx {
    DWORD pid;
    HWND found;
} FindCtx;

static BOOL CALLBACK find_window_cb(HWND hwnd, LPARAM lparam) {
    FindCtx *c = (FindCtx *)lparam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == c->pid && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
        c->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

// Every top-level window of the process into the line: its class, visible or not, owned or not (a failed start).
static BOOL CALLBACK list_windows_cb(HWND hwnd, LPARAM lparam) {
    FindCtx *c = (FindCtx *)lparam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != c->pid) return TRUE;
    c->found = hwnd;
    wchar_t cls[128];
    cls[0] = 0;
    GetClassNameW(hwnd, cls, 128);
    put(" [");
    put_w(cls);
    put(IsWindowVisible(hwnd) ? ", visible" : ", hidden");
    put(GetWindow(hwnd, GW_OWNER) ? ", owned" : "");
    BOOL cloaked = 0;
    if (DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)) == S_OK && cloaked) put(", cloaked");
    put("]");
    return TRUE;
}

// The session's folders: <work>\<name>-r<run>[-<part>]\work (the working folder), appdata, localappdata; APPDATA and
// LOCALAPPDATA of this process (inherited by ronin) pointed at the empty ones.
static b32 session_prepare(Session *s, const char *name, const char *part) {
    memset(s, 0, sizeof(*s));
    s->name = name;
    s->part = part;
    u8 mem[128];
    Buf b = { mem, 0, sizeof(mem) };
    bput(&b, name);
    bput(&b, "-r");
    bput_i64(&b, run.repeat_index);
    if (part) {
        bput(&b, "-");
        bput(&b, part);
    }
    wchar_t sub[128], p[PATH_CAP];
    wfrom_utf8(sub, 128, str(b.data, b.len));
    wjoin(s->dir, run.work, sub);
    if (!make_dir(s->dir)) return 0;
    wjoin(s->cwd, s->dir, L"work");
    wjoin(p, s->dir, L"appdata");
    if (!make_dir(s->cwd) || !make_dir(p)) return 0;
    SetEnvironmentVariableW(L"APPDATA", p);
    wjoin(p, s->dir, L"localappdata");
    if (!make_dir(p)) return 0;
    SetEnvironmentVariableW(L"LOCALAPPDATA", p);
    return 1;
}

// A file in the session's working folder.
static b32 session_write(Session *s, const char *name, Str data) {
    wchar_t wname[PATH_CAP], path[PATH_CAP];
    wfrom_utf8(wname, PATH_CAP, cstr(name));
    wjoin(path, s->cwd, wname);
    return write_file(path, data.data, data.len);
}

// Starts a process of the exe in the session's folder, in the run's job; 0 when it could not.
static b32 start_in_job(Session *s, const wchar_t *const *args, i32 count, PROCESS_INFORMATION *pi) {
    static wchar_t cmdline[4 * PATH_CAP];
    i64 n = 0;
#define CMD_PUT(w) do { const wchar_t *w_ = (w); for (i64 k_ = 0; w_[k_] && n < 4 * PATH_CAP - 2; k_++) cmdline[n++] = w_[k_]; } while (0)
    CMD_PUT(L"\"");
    CMD_PUT(run.exe);
    CMD_PUT(L"\"");
    for (i32 i = 0; i < count; i++) {
        CMD_PUT(L" \"");
        CMD_PUT(args[i]);
        CMD_PUT(L"\"");
    }
#undef CMD_PUT
    cmdline[n] = 0;
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(pi, 0, sizeof(*pi));
    if (!CreateProcessW(run.exe, cmdline, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, s->cwd, &si, pi)) return 0;
    if (!AssignProcessToJobObject(run.job, pi->hProcess)) {
        TerminateProcess(pi->hProcess, 1);
        CloseHandle(pi->hThread);
        CloseHandle(pi->hProcess);
        return 0;
    }
    return 1;
}

// ronin started with args in the session's working folder: its window, the foreground, the title, the first frame
// (the filled cursor block, which gives the cell size), the pointer moved off the window.
static Result session_start(Session *s, const wchar_t *const *args, i32 count, const wchar_t *title) {
    u64 t0 = GetTickCount64();
    InterlockedExchange(&watch.fg_armed, 0);
    InterlockedExchange(&watch.exiting, 0);
    ResetEvent(watch.show_event);
    ResetEvent(watch.title_event);
    ResetEvent(watch.fg_event);
    if (!start_in_job(s, args, count, &s->pi)) {
        put_prefix(s);
        put("could not start ");
        put_w(run.exe);
        put(" (error ");
        put_i64(GetLastError());
        put(")");
        end_line();
        return R_FAIL;
    }
    InterlockedExchange(&watch.pid, (LONG)s->pi.dwProcessId);
    ResumeThread(s->pi.hThread);
    s->running = 1;

    u64 deadline = GetTickCount64() + WINDOW_MS;
    FindCtx ctx = { s->pi.dwProcessId, NULL };
    for (;;) {
        // Looked for again at each composition: a window shown cloaked (or before the hook saw it) may send no
        // EVENT_OBJECT_SHOW this hook gets.
        EnumWindows(find_window_cb, (LPARAM)&ctx);
        if (ctx.found) break;
        if (aborted()) return R_ABORT;
        b32 ended = WaitForSingleObject(s->pi.hProcess, 0) == WAIT_OBJECT_0;
        if (ended || GetTickCount64() >= deadline) {
            put_prefix(s);
            if (ended) {
                DWORD code = 0;
                GetExitCodeProcess(s->pi.hProcess, &code);
                put("ronin ended before it showed a window, exit code ");
                put_i64(code);
            } else {
                put("no visible window within 10 s; its top-level windows:");
                FindCtx any = { s->pi.dwProcessId, NULL };
                EnumWindows(list_windows_cb, (LPARAM)&any);
                if (!any.found) put(" none");
            }
            end_line();
            return R_FAIL;
        }
        tick();
    }
    s->hwnd = ctx.found;
    s->tid = GetWindowThreadProcessId(s->hwnd, NULL);
    for (;;) {
        if (GetForegroundWindow() == s->hwnd) break;
        Wait w = wait_handle(watch.fg_event, deadline, 1);
        if (w == W_ABORT) return R_ABORT;
        if (w == W_TIMEOUT) {
            put_prefix(s);
            put("ronin's window did not become the foreground window (the foreground is ");
            describe_window(&line, GetForegroundWindow());
            put("; Windows refused the activation?)");
            end_line();
            return R_FAIL;
        }
    }
    InterlockedExchange(&watch.fg_armed, 1);
    if (GetForegroundWindow() != s->hwnd) return before_key(s); // lost again before the hook was armed
    where.len = 0;
    bput(&where, "the start (");
    bput(&where, s->name);
    bput(&where, ")");
    Result r = wait_title(s, title);
    if (r != R_OK) return r;
    if (!capture_setup(s)) {
        put_prefix(s);
        put("the client area cannot be captured");
        end_line();
        return R_FAIL;
    }
    // The first frame: the filled cursor block drawn and the screen settled.
    for (;;) {
        if (aborted()) return R_ABORT;
        tick();
        if (!capture(s, s->cur)) {
            put_prefix(s);
            put("the window is covered by ");
            bput_n(&line, covered.data, covered.len);
            return fail_end(s);
        }
        if (color_box(s, s->cur, THEME_CURSOR, &s->cursor0) && settle(s, deadline) == W_OK) break;
        if (GetTickCount64() >= deadline) {
            put_prefix(s);
            put("no frame with the filled cursor (#90ee90) within 10 s");
            return fail_end(s);
        }
    }
    s->cell_w = s->cursor0.right - s->cursor0.left + 1;
    s->cell_h = s->cursor0.bottom - s->cursor0.top + 1;
    s->hkl = GetKeyboardLayout(s->tid);
    RECT wr;
    GetWindowRect(s->hwnd, &wr);
    i32 vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    i32 px = wr.right + 8 < vx + GetSystemMetrics(SM_CXVIRTUALSCREEN) ? wr.right + 8 : wr.left - 8;
    if (px < vx) px = vx;
    i32 py = wr.top > vy ? wr.top : vy;
    mouse_move(px, py);
    put_prefix(s);
    put("started in ");
    bput_secs(&line, GetTickCount64() - t0);
    put(": pid ");
    put_i64(s->pi.dwProcessId);
    put(", client area ");
    put_i64(s->w);
    put("x");
    put_i64(s->h);
    put(" at ");
    put_i64(s->origin.x);
    put(",");
    put_i64(s->origin.y);
    put(", cell ");
    put_i64(s->cell_w);
    put("x");
    put_i64(s->cell_h);
    put(", layout ");
    bput_hkl(&line, s->hkl);
    end_line();
    save_shot(s, s->frame, "start");
    return R_OK;
}

// Ends whatever of the session still runs (the job's processes) and releases it.
static void session_end(Session *s) {
    if (s->running && s->hkl && GetKeyboardLayout(s->tid) != s->hkl) {
        // Its layout switched (an abort): given back first, so the session's input language is the recorded one again.
        PostMessageW(s->hwnd, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)s->hkl);
        u64 deadline = GetTickCount64() + 2000;
        while (GetKeyboardLayout(s->tid) != s->hkl && GetTickCount64() < deadline) tick();
        put_prefix(s);
        put("layout ");
        bput_hkl(&line, s->hkl);
        put(GetKeyboardLayout(s->tid) == s->hkl ? " given back to ronin" : " could not be given back to ronin");
        end_line();
    }
    if (s->running) {
        InterlockedExchange(&watch.exiting, 1);
        TerminateJobObject(run.job, 1);
        WaitForSingleObject(s->pi.hProcess, 5000);
        s->running = 0;
    }
    InterlockedExchange(&watch.fg_armed, 0);
    InterlockedExchange(&watch.pid, 0);
    if (s->pi.hProcess) CloseHandle(s->pi.hProcess);
    if (s->pi.hThread) CloseHandle(s->pi.hThread);
    s->pi.hProcess = s->pi.hThread = NULL;
    capture_release(s);
}

// A pixel of the settled frame in an exact color.
static Result probe(Session *s, const char *what, i32 x, i32 y, u32 color) {
    u32 got = pixel(s, s->frame, x, y);
    if (got == color) return R_OK;
    put_prefix(s);
    put(what);
    put(" at ");
    put_i64(x);
    put(",");
    put_i64(y);
    put(" is #");
    put_hex(got, 6);
    put(", expected #");
    put_hex(color, 6);
    swap_frames(&s->cur, &s->frame);
    Result r = fail_end(s);
    swap_frames(&s->cur, &s->frame);
    return r;
}

// No file below dir (folders are allowed and listed).
static Result no_files(Session *s, const char *label, const wchar_t *dir) {
    walk(dir);
    i32 folders = 0;
    for (i32 i = 0; i < walk_buf.count; i++) {
        Entry *e = &walk_buf.items[i];
        if (e->folder) {
            folders++;
            continue;
        }
        put_prefix(s);
        put(label);
        put(": a file was written: ");
        bput_n(&line, e->path, e->path_len);
        end_line();
        return R_FAIL;
    }
    put_prefix(s);
    put(label);
    put(": no file written");
    if (folders) {
        put(" (folders:");
        for (i32 i = 0; i < walk_buf.count; i++) {
            put(" ");
            bput_n(&line, walk_buf.items[i].path, walk_buf.items[i].path_len);
            put("/");
        }
        put(")");
    }
    end_line();
    return R_OK;
}

// ---------------------------------------------------------------------------
// Scenario 1: the first start from the unpacked folder, no configuration

static Result scenario_1_body(Session *s) {
    static const wchar_t *const none[1] = { NULL };
    Result r = session_start(s, none, 0, L"*scratch* - ronin");
    if (r != R_OK) return r;
    // ronin's own colors on the real screen: the filled cursor, the selected mode line, the echo area's background.
    // Probed two cells in from the right edge: Windows 11 rounds the window's corners over the client area.
    i32 ml = -1, px = s->w - 2 * s->cell_w;
    for (i32 y = s->h - 1; y >= 0 && ml < 0; y--) if (pixel(s, s->frame, px, y) == THEME_MODE_LINE) ml = y;
    if ((r = probe(s, "the cursor", (s->cursor0.left + s->cursor0.right) / 2, (s->cursor0.top + s->cursor0.bottom) / 2,
                   THEME_CURSOR)) != R_OK)
        return r;
    if ((r = probe(s, "the echo area", px, s->h - s->cell_h / 2, THEME_BACKGROUND)) != R_OK) return r;
    if (ml < 0) {
        put_prefix(s);
        put("no selected mode line (#d3b58d) two cells in from the right edge");
        return fail_end(s);
    }
    put_prefix(s);
    put("pixels: cursor #90ee90, background #072626, mode line #d3b58d");
    end_line();

    if ((r = key(s, "C-x")) != R_OK || (r = key(s, "C-f")) != R_OK || (r = type(s, "first.txt")) != R_OK) return r;
    if ((r = key_title(s, "RET", L"first.txt - ronin")) != R_OK) return r;
    if (run.demo == DEMO_LAYOUT) {
        HKL list[LAYOUTS_MAX];
        i32 n = GetKeyboardLayoutList(LAYOUTS_MAX, list);
        HKL other = NULL;
        for (i32 i = 0; i < n && !other; i++) if (list[i] != s->hkl) other = list[i];
        put_prefix(s);
        put("demo: WM_INPUTLANGCHANGEREQUEST for ");
        bput_hkl(&line, other);
        put(" posted to ronin");
        end_line();
        if (other) PostMessageW(s->hwnd, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)other);
    }
    if (run.demo == DEMO_FOREGROUND) {
        static const wchar_t *const no_args[1] = { NULL };
        PROCESS_INFORMATION second;
        put_prefix(s);
        put("demo: a second ronin started");
        end_line();
        if (start_in_job(s, no_args, 0, &second)) {
            ResumeThread(second.hThread);
            CloseHandle(second.hThread);
            CloseHandle(second.hProcess);
        }
    }
    if ((r = type(s, "Hello, ronin!")) != R_OK || (r = key(s, "RET")) != R_OK || (r = type(s, "second line: 1 + 2 = 3;")) != R_OK)
        return r;
    static const char saved[] = "Hello, ronin!\nsecond line: 1 + 2 = 3;";
    if ((r = key(s, "C-x")) != R_OK) return r;
    if ((r = key_save(s, "C-s", "first.txt", str((const u8 *)saved, sizeof(saved) - 1))) != R_OK) return r;
    if ((r = key(s, "C-x")) != R_OK) return r;
    return key_exit(s, "C-c");
}

static Result scenario_1(void) {
    Session s;
    if (!session_prepare(&s, "s1", NULL)) return R_FAIL;
    static u8 before_mem[SNAP_CAP], after_mem[SNAP_CAP];
    Buf before = { before_mem, 0, SNAP_CAP }, after = { after_mem, 0, SNAP_CAP };
    snapshot(&before, run.exe_dir);
    Result r = scenario_1_body(&s);
    session_end(&s);
    if (r != R_OK) return r;
    wchar_t p[PATH_CAP];
    session_dir_path(&s, p, L"appdata");
    if ((r = no_files(&s, "APPDATA", p)) != R_OK) return r;
    session_dir_path(&s, p, L"localappdata");
    if ((r = no_files(&s, "LOCALAPPDATA", p)) != R_OK) return r;
    snapshot(&after, run.exe_dir);
    put_prefix(&s);
    if (!buf_eq(&before, &after)) {
        put("the unpacked folder changed");
        end_line();
        return R_FAIL;
    }
    put("the unpacked folder unchanged");
    end_line();
    return R_OK;
}

// ---------------------------------------------------------------------------
// Scenario 2: editing real files opened as ronin +LINE:COL file (a CRLF file and a UTF-8 file with a BOM)

// One file of s2: written fresh, ronin started on it from +LINE:COL, the keys, C-s and its bytes, C-x C-c.
static Result scenario_2_file(const char *name, const wchar_t *const *args, const wchar_t *title, Str content,
                              Result (*keys)(Session *), Str expected) {
    Session s;
    if (!session_prepare(&s, "s2", name)) return R_FAIL;
    if (!session_write(&s, name, content)) {
        put("e2e: s2: cannot write ");
        put(name);
        end_line();
        return R_FAIL;
    }
    Result r = session_start(&s, args, 2, title);
    if (r == R_OK) r = keys(&s);
    if (r == R_OK) r = key_save(&s, "C-s", name, expected);
    if (r == R_OK) r = key(&s, "C-x");
    if (r == R_OK) r = key_exit(&s, "C-c");
    session_end(&s);
    return r;
}

// crlf.c from +2:5: the end of line 2, a comment typed, the line's start, a word, kill, yank twice, undo, isearch,
// query-replace from point; then C-x (C-s follows).
static Result scenario_2_crlf_keys(Session *s) {
    Result r;
    if ((r = key(s, "C-e")) != R_OK || (r = type(s, " // x")) != R_OK || (r = key(s, "C-a")) != R_OK) return r;
    if ((r = key(s, "M-f")) != R_OK || (r = key(s, "C-k")) != R_OK || (r = key(s, "C-y")) != R_OK) return r;
    if ((r = key(s, "C-y")) != R_OK || (r = key(s, "C-/")) != R_OK) return r;
    if ((r = key(s, "C-s")) != R_OK || (r = type(s, "beta")) != R_OK || (r = key(s, "RET")) != R_OK) return r;
    if ((r = key(s, "M-%")) != R_OK || (r = type(s, "alpha")) != R_OK || (r = key(s, "RET")) != R_OK) return r;
    if ((r = type(s, "gamma")) != R_OK || (r = key(s, "RET")) != R_OK || (r = key(s, "!")) != R_OK) return r;
    return key(s, "C-x");
}

// bom.txt from +2:5 (after three two-byte letters and a space): the end of the line, typing, the line's start, M-f over
// the non-ASCII word, kill, yank, undo, a character typed there, isearch, query-replace from point; then C-x.
static Result scenario_2_bom_keys(Session *s) {
    Result r;
    if ((r = key(s, "C-e")) != R_OK || (r = type(s, " x")) != R_OK || (r = key(s, "C-a")) != R_OK) return r;
    if ((r = key(s, "M-f")) != R_OK || (r = key(s, "C-k")) != R_OK || (r = key(s, "C-y")) != R_OK) return r;
    if ((r = key(s, "C-/")) != R_OK || (r = type(s, "!")) != R_OK) return r;
    if ((r = key(s, "C-s")) != R_OK || (r = type(s, "alpha")) != R_OK || (r = key(s, "RET")) != R_OK) return r;
    if ((r = key(s, "M-%")) != R_OK || (r = type(s, "line")) != R_OK || (r = key(s, "RET")) != R_OK) return r;
    if ((r = type(s, "row")) != R_OK || (r = key(s, "RET")) != R_OK || (r = key(s, "!")) != R_OK) return r;
    return key(s, "C-x");
}

static Result scenario_2(void) {
    static const char crlf[] = "int main(void) {\r\n    int alpha = 1;\r\n    int beta = 2;\r\n    return alpha + beta;\r\n}\r\n";
    static const char crlf_saved[] =
        "int main(void) {\r\n    int alpha = 1; // x\r\n    int beta = 2;\r\n    return gamma + beta;\r\n}\r\n";
    // café au lait / ğüş alpha beta / last alpha line, with a BOM and LF.
    static const char bom[] = "\xEF\xBB\xBF" "caf\xC3\xA9 au lait\n\xC4\x9F\xC3\xBC\xC5\x9F alpha beta\nlast alpha line\n";
    static const char bom_saved[] = "\xEF\xBB\xBF" "caf\xC3\xA9 au lait\n\xC4\x9F\xC3\xBC\xC5\x9F!\nlast alpha row\n";
    static const wchar_t *const crlf_args[2] = { L"+2:5", L"crlf.c" };
    static const wchar_t *const bom_args[2] = { L"+2:5", L"bom.txt" };
    Result r = scenario_2_file("crlf.c", crlf_args, L"crlf.c - ronin", str((const u8 *)crlf, sizeof(crlf) - 1),
                               scenario_2_crlf_keys, str((const u8 *)crlf_saved, sizeof(crlf_saved) - 1));
    if (r != R_OK) return r;
    return scenario_2_file("bom.txt", bom_args, L"bom.txt - ronin", str((const u8 *)bom, sizeof(bom) - 1), scenario_2_bom_keys,
                           str((const u8 *)bom_saved, sizeof(bom_saved) - 1));
}

// ---------------------------------------------------------------------------
// The machine's state: recorded before the run, restored and compared after it

// A registry key's values (and its subkeys' to depth) as text, exact (the data in hex).
static void reg_dump(Buf *b, HKEY root, const wchar_t *path, i32 depth) {
    HKEY k;
    bput_w(b, path, wlen(path));
    if (RegOpenKeyExW(root, path, 0, KEY_READ, &k) != ERROR_SUCCESS) {
        bput(b, " (missing)\r\n");
        return;
    }
    bput(b, "\r\n");
    for (DWORD i = 0;; i++) {
        wchar_t name[256];
        static u8 data[4096];
        DWORD name_len = 256, data_len = sizeof(data), type = 0;
        LSTATUS st = RegEnumValueW(k, i, name, &name_len, NULL, &type, data, &data_len);
        if (st == ERROR_NO_MORE_ITEMS) break;
        if (st != ERROR_SUCCESS) {
            bput(b, "  (a value too large)\r\n");
            continue;
        }
        bput(b, "  ");
        bput_w(b, name, name_len);
        bput(b, " = ");
        bput_i64(b, type);
        bput(b, ":");
        for (DWORD j = 0; j < data_len; j++) bput_hex(b, data[j], 2);
        bput(b, "\r\n");
    }
    if (depth > 0) {
        for (DWORD i = 0;; i++) {
            wchar_t sub[256], child[PATH_CAP];
            DWORD sub_len = 256;
            if (RegEnumKeyExW(k, i, sub, &sub_len, NULL, NULL, NULL, NULL) != ERROR_SUCCESS) break;
            wjoin(child, path, sub);
            reg_dump(b, root, child, depth - 1);
        }
    }
    RegCloseKey(k);
}

// The keyboard layouts: the loaded list, Preload, Substitutes, the User Profile and the foreground thread's layout.
static void layout_state(Buf *b, HKL *list, i32 *count, HKL *active) {
    b->len = 0;
    *count = GetKeyboardLayoutList(LAYOUTS_MAX, list);
    bput(b, "loaded:");
    for (i32 i = 0; i < *count; i++) {
        bput(b, " ");
        bput_hkl(b, list[i]);
    }
    bput(b, "\r\n");
    HWND fg = GetForegroundWindow();
    *active = GetKeyboardLayout(fg ? GetWindowThreadProcessId(fg, NULL) : 0);
    bput(b, "active: ");
    bput_hkl(b, *active);
    bput(b, "\r\n");
    reg_dump(b, HKEY_CURRENT_USER, L"Keyboard Layout\\Preload", 0);
    reg_dump(b, HKEY_CURRENT_USER, L"Keyboard Layout\\Substitutes", 0);
    reg_dump(b, HKEY_CURRENT_USER, L"Control Panel\\International\\User Profile", 2);
}

// A clipboard format as it was: its bytes (a copy of its global memory). A synthesized one is never set back by hand:
// Windows makes it again from the one it came from; its bytes are kept to compare the read-back with.
typedef struct ClipFormat {
    UINT format;
    b32 synthesized;
    u8 *data;
    SIZE_T size;
} ClipFormat;

typedef struct MachineState {
    Buf layouts;
    HKL list[LAYOUTS_MAX];
    i32 list_count;
    HKL active;
    ClipFormat clip[CLIP_FORMATS_MAX]; // every format on the clipboard, in its enumeration order
    i32 clip_count;
    Buf appdata, localappdata;
    POINT pointer;
} MachineState;

static MachineState before_state;

static b32 clip_open(void) {
    u64 deadline = GetTickCount64() + 2000;
    for (;;) {
        if (OpenClipboard(run.clip_window)) return 1;
        if (GetTickCount64() >= deadline) return 0;
        wait_handle(NULL, GetTickCount64() + 10, 0); // another program holds it: its next look
    }
}

// A clipboard format as "13 CF_UNICODETEXT" or "49849 \"Chromium internal source RFH token\"".
static void put_format(UINT f) {
    static const struct { UINT f; const char *name; } standard[] = {
        { CF_TEXT, "CF_TEXT" }, { CF_BITMAP, "CF_BITMAP" }, { CF_METAFILEPICT, "CF_METAFILEPICT" }, { CF_SYLK, "CF_SYLK" },
        { CF_DIF, "CF_DIF" }, { CF_TIFF, "CF_TIFF" }, { CF_OEMTEXT, "CF_OEMTEXT" }, { CF_DIB, "CF_DIB" },
        { CF_PALETTE, "CF_PALETTE" }, { CF_PENDATA, "CF_PENDATA" }, { CF_RIFF, "CF_RIFF" }, { CF_WAVE, "CF_WAVE" },
        { CF_UNICODETEXT, "CF_UNICODETEXT" }, { CF_ENHMETAFILE, "CF_ENHMETAFILE" }, { CF_HDROP, "CF_HDROP" },
        { CF_LOCALE, "CF_LOCALE" }, { CF_DIBV5, "CF_DIBV5" }, { CF_OWNERDISPLAY, "CF_OWNERDISPLAY" },
        { CF_DSPTEXT, "CF_DSPTEXT" }, { CF_DSPBITMAP, "CF_DSPBITMAP" }, { CF_DSPMETAFILEPICT, "CF_DSPMETAFILEPICT" },
        { CF_DSPENHMETAFILE, "CF_DSPENHMETAFILE" },
    };
    put_i64(f);
    for (i32 i = 0; i < (i32)(sizeof(standard) / sizeof(standard[0])); i++) {
        if (standard[i].f != f) continue;
        put(" ");
        put(standard[i].name);
        return;
    }
    wchar_t name[128];
    if (GetClipboardFormatNameW(f, name, 128) > 0) {
        put(" \"");
        put_w(name);
        put("\"");
    }
}

// Formats whose data is a GDI or private handle, not global memory: they cannot be copied byte for byte.
static b32 clip_handle_format(UINT f) {
    return f == CF_BITMAP || f == CF_METAFILEPICT || f == CF_PALETTE || f == CF_ENHMETAFILE || f == CF_OWNERDISPLAY ||
           f == CF_DSPBITMAP || f == CF_DSPMETAFILEPICT || f == CF_DSPENHMETAFILE ||
           (f >= CF_PRIVATEFIRST && f <= CF_PRIVATELAST) || (f >= CF_GDIOBJFIRST && f <= CF_GDIOBJLAST);
}

static b32 clip_is_text(UINT f) { return f == CF_UNICODETEXT || f == CF_TEXT || f == CF_OEMTEXT; }

// Every format on the clipboard saved byte for byte, in its enumeration order. Synthesized (never set back by hand,
// made again by Windows): CF_LOCALE, and of CF_UNICODETEXT, CF_TEXT and CF_OEMTEXT every one but the first enumerated
// (Windows lists a program's own formats before the ones it synthesizes from them). A handle format (a bitmap, a
// metafile, a palette, a private or GDI handle), a format that cannot be read, or more than 64 formats or 64 MB:
// 0, with the line printed, before anything is typed.
static b32 clip_save(MachineState *m) {
    if (!clip_open()) {
        say("e2e: the clipboard cannot be opened (another program holds it)");
        return 0;
    }
    b32 ok = 1, text_seen = 0;
    SIZE_T total = 0;
    m->clip_count = 0;
    for (UINT f = EnumClipboardFormats(0); f && ok; f = EnumClipboardFormats(f)) {
        const char *refusal = NULL;
        HANDLE h = NULL;
        SIZE_T size = 0;
        if (clip_handle_format(f)) refusal = "is a handle, not memory: it cannot be saved byte for byte";
        else if (m->clip_count >= CLIP_FORMATS_MAX) refusal = "is one format too many (64 at most)";
        else if (!(h = GetClipboardData(f))) refusal = "cannot be read";
        else if (!(size = GlobalSize(h))) refusal = "is not global memory: it cannot be saved byte for byte";
        else if (total + size > CLIP_BYTES_MAX) refusal = "makes the clipboard too large to keep (64 MB at most)";
        if (refusal) {
            put("e2e: the clipboard's format ");
            put_format(f);
            put(" ");
            put(refusal);
            put("; copy something else or empty the clipboard, then run again");
            end_line();
            ok = 0;
            break;
        }
        ClipFormat *c = &m->clip[m->clip_count++];
        c->format = f;
        c->synthesized = f == CF_LOCALE || (clip_is_text(f) && text_seen);
        if (clip_is_text(f)) text_seen = 1;
        c->size = size;
        c->data = (u8 *)VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        const u8 *p = (const u8 *)GlobalLock(h);
        if (!c->data || !p) {
            if (p) GlobalUnlock(h);
            put("e2e: the clipboard's format ");
            put_format(f);
            put(" cannot be copied");
            end_line();
            ok = 0;
            break;
        }
        memcpy(c->data, p, size);
        GlobalUnlock(h);
        total += size;
    }
    CloseClipboard();
    return ok;
}

static void clip_empty(void) {
    if (!clip_open()) return;
    EmptyClipboard();
    CloseClipboard();
}

static void put_clip_formats(MachineState *m) {
    put_i64(m->clip_count);
    put(m->clip_count == 1 ? " format" : " formats");
    for (i32 i = 0; i < m->clip_count; i++) {
        put(i ? ", " : ": ");
        put_format(m->clip[i].format);
        put(" (");
        put_i64((i64)m->clip[i].size);
        put(m->clip[i].synthesized ? " bytes, synthesized)" : " bytes)");
    }
}

// Every saved format that is not synthesized set back as it was; then every saved format read back and compared
// byte for byte, and the formats now on the clipboard compared with the saved list. Each difference is a line naming
// the format; 1 when there is none. History is off for the whole run (a precondition), so nothing is added to it.
static b32 clip_restore(MachineState *m) {
    if (!clip_open()) {
        say("e2e: state: FAIL: the clipboard cannot be opened to put it back");
        return 0;
    }
    b32 ok = 1;
    EmptyClipboard();
    for (i32 i = 0; i < m->clip_count; i++) {
        ClipFormat *c = &m->clip[i];
        if (c->synthesized) continue;
        HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, c->size);
        u8 *p = g ? (u8 *)GlobalLock(g) : NULL;
        if (p) {
            memcpy(p, c->data, c->size);
            GlobalUnlock(g);
        }
        if (!p || !SetClipboardData(c->format, g)) {
            if (g) GlobalFree(g);
            put("e2e: state: FAIL: the clipboard's format ");
            put_format(c->format);
            put(" could not be set back");
            end_line();
            ok = 0;
        }
    }
    CloseClipboard();
    if (!clip_open()) {
        say("e2e: state: FAIL: the clipboard cannot be opened to read it back");
        return 0;
    }
    for (i32 i = 0; i < m->clip_count; i++) {
        ClipFormat *c = &m->clip[i];
        HANDLE h = GetClipboardData(c->format);
        SIZE_T size = h ? GlobalSize(h) : 0;
        const u8 *p = size ? (const u8 *)GlobalLock(h) : NULL;
        SIZE_T at = 0;
        while (p && at < c->size && at < size && p[at] == c->data[at]) at++;
        b32 same = p && size == c->size && at == c->size;
        if (!same) {
            put("e2e: state: FAIL: the clipboard's format ");
            put_format(c->format);
            if (!p) {
                put(" is missing after the restore");
            } else if (at < c->size && at < size) {
                put(": byte ");
                put_i64((i64)at);
                put(" is 0x");
                put_hex(p[at], 2);
                put(", saved 0x");
                put_hex(c->data[at], 2);
                put(" (");
                put_i64((i64)size);
                put(" bytes, saved ");
                put_i64((i64)c->size);
                put(")");
            } else {
                put(": ");
                put_i64((i64)size);
                put(" bytes, saved ");
                put_i64((i64)c->size);
                put(" (equal up to the shorter)");
            }
            end_line();
            ok = 0;
        }
        if (p) GlobalUnlock(h);
    }
    for (UINT f = EnumClipboardFormats(0); f; f = EnumClipboardFormats(f)) {
        b32 saved = 0;
        for (i32 i = 0; i < m->clip_count; i++) if (m->clip[i].format == f) saved = 1;
        if (saved) continue;
        put("e2e: state: FAIL: the clipboard's format ");
        put_format(f);
        put(" is there after the restore, not before the run");
        end_line();
        ok = 0;
    }
    CloseClipboard();
    return ok;
}

static b32 clipboard_history_on(void) {
    DWORD v = 0, size = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Clipboard", L"EnableClipboardHistory", RRF_RT_REG_DWORD, NULL,
                        &v, &size) == ERROR_SUCCESS &&
           v != 0;
}

static void record_folders(MachineState *m) {
    wchar_t p[PATH_CAP];
    wjoin(p, run.real_appdata, L"ronin");
    snapshot(&m->appdata, p);
    wjoin(p, run.real_localappdata, L"ronin");
    snapshot(&m->localappdata, p);
}

static void put_layout_list(HKL *list, i32 n, HKL active) {
    for (i32 i = 0; i < n; i++) {
        if (i) put(" ");
        bput_hkl(&line, list[i]);
    }
    put("; active ");
    bput_hkl(&line, active);
}

// Puts back what the run may have changed, then compares everything with the record; 1 when all equals it.
static b32 restore_and_check(void) {
    MachineState *b = &before_state;
    static MachineState now;
    if (!now.layouts.data) {
        now.layouts = buf_alloc(STATE_CAP);
        now.appdata = buf_alloc(SNAP_CAP);
        now.localappdata = buf_alloc(SNAP_CAP);
    }
    b32 ok = 1;

    // Layouts: what was loaded since unloaded; the foreground thread given the recorded layout back.
    layout_state(&now.layouts, now.list, &now.list_count, &now.active);
    for (i32 i = 0; i < now.list_count; i++) {
        b32 known = 0;
        for (i32 j = 0; j < b->list_count; j++) if (now.list[i] == b->list[j]) known = 1;
        if (known) continue;
        put("e2e: state: unloading layout ");
        bput_hkl(&line, now.list[i]);
        put(UnloadKeyboardLayout(now.list[i]) ? "" : " (refused)");
        end_line();
    }
    if (now.active != b->active) {
        HWND fg = GetForegroundWindow();
        put("e2e: state: the foreground window's layout is ");
        bput_hkl(&line, now.active);
        put(": ");
        bput_hkl(&line, b->active);
        put(" requested for it");
        end_line();
        if (fg) {
            PostMessageW(fg, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)b->active);
            DWORD tid = GetWindowThreadProcessId(fg, NULL);
            u64 deadline = GetTickCount64() + 2000;
            while (GetKeyboardLayout(tid) != b->active && GetTickCount64() < deadline) tick();
        }
    }
    layout_state(&now.layouts, now.list, &now.list_count, &now.active);
    if (buf_eq(&now.layouts, &b->layouts)) {
        put("e2e: state: keyboard layouts as before (");
        put_layout_list(now.list, now.list_count, now.active);
        put("; Preload, Substitutes and User Profile equal)");
        end_line();
    } else {
        say("e2e: state: FAIL: the keyboard layouts differ from the record; before:");
        out_text(&b->layouts);
        say("e2e: state: after:");
        out_text(&now.layouts);
        ok = 0;
    }

    // The clipboard.
    if (clip_restore(b)) {
        put("e2e: state: clipboard put back and read back byte for byte equal, no other format: ");
        if (b->clip_count) put_clip_formats(b);
        else put("empty");
        end_line();
    } else {
        say("e2e: state: FAIL: the clipboard is not as it was (the formats above)");
        ok = 0;
    }

    // The pointer.
    SetCursorPos(b->pointer.x, b->pointer.y);

    // The maintainer's folders.
    record_folders(&now);
    if (buf_eq(&now.appdata, &b->appdata) && buf_eq(&now.localappdata, &b->localappdata)) {
        put("e2e: state: ");
        put_w(run.real_appdata);
        put("\\ronin and ");
        put_w(run.real_localappdata);
        put("\\ronin unchanged");
        end_line();
    } else {
        put("e2e: state: FAIL: ");
        put_w(buf_eq(&now.appdata, &b->appdata) ? run.real_localappdata : run.real_appdata);
        put("\\ronin changed during the run");
        end_line();
        ok = 0;
    }

    // The processes: none left in the job, no ronin.exe running from the run's folder.
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct;
    memset(&acct, 0, sizeof(acct));
    QueryInformationJobObject(run.job, JobObjectBasicAccountingInformation, &acct, sizeof(acct), NULL);
    i32 left = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe;
        pe.dwSize = sizeof(pe);
        i64 work_len = wlen(run.work);
        for (BOOL more = Process32FirstW(snap, &pe); more; more = Process32NextW(snap, &pe)) {
            HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
            if (!h) continue;
            wchar_t image[PATH_CAP];
            DWORD n = PATH_CAP;
            if (QueryFullProcessImageNameW(h, 0, image, &n) && (i64)n >= work_len &&
                CompareStringOrdinal(image, (i32)work_len, run.work, (i32)work_len, TRUE) == CSTR_EQUAL) {
                put("e2e: state: FAIL: still running: ");
                put_w(image);
                put(", pid ");
                put_i64(pe.th32ProcessID);
                end_line();
                left++;
            }
            CloseHandle(h);
        }
        CloseHandle(snap);
    }
    if (acct.ActiveProcesses == 0 && left == 0) {
        say("e2e: state: no process of the run left");
    } else {
        ok = 0;
        if (acct.ActiveProcesses) {
            put("e2e: state: FAIL: ");
            put_i64(acct.ActiveProcesses);
            put(" process(es) left in the run's job");
            end_line();
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------
// Unpacking

// The zip unpacked by Windows' tar into <work>\İndirilenler ronin (tar runs there); exactly one folder ronin-<version>.
static b32 unpack_zip(const wchar_t *zip) {
    wchar_t unpack[PATH_CAP], zip_full[PATH_CAP], tar[PATH_CAP], sys[PATH_CAP];
    wjoin(unpack, run.work, UNPACK_FOLDER);
    if (!make_dir(unpack)) return 0;
    full_path(zip_full, zip);
    GetSystemDirectoryW(sys, PATH_CAP);
    wjoin(tar, sys, L"tar.exe");
    static wchar_t cmdline[4 * PATH_CAP];
    wjoin(cmdline, L"\"", NULL);
    wjoin(cmdline + 1, tar, NULL);
    i64 n = wlen(cmdline);
    const wchar_t *mid = L"\" -x -f \"";
    for (i64 i = 0; mid[i]; i++) cmdline[n++] = mid[i];
    for (i64 i = 0; zip_full[i]; i++) cmdline[n++] = zip_full[i];
    cmdline[n++] = '"';
    cmdline[n] = 0;
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    if (!CreateProcessW(tar, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, unpack, &si, &pi)) {
        say("e2e: FAIL: cannot start tar.exe");
        return 0;
    }
    CloseHandle(pi.hThread);
    DWORD code = 1;
    if (WaitForSingleObject(pi.hProcess, 120000) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    if (code != 0) {
        put("e2e: FAIL: tar.exe could not unpack ");
        put_w(zip_full);
        end_line();
        return 0;
    }
    wchar_t pattern[PATH_CAP];
    wjoin(pattern, unpack, L"ronin-*");
    WIN32_FIND_DATAW fd;
    HANDLE find = FindFirstFileW(pattern, &fd);
    i32 folders = 0;
    if (find != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                folders++;
                wjoin(run.exe_dir, unpack, fd.cFileName);
            }
        } while (FindNextFileW(find, &fd));
        FindClose(find);
    }
    if (folders != 1) {
        say("e2e: FAIL: the zip does not hold exactly one ronin-<version> folder");
        return 0;
    }
    wjoin(run.exe, run.exe_dir, L"ronin.exe");
    return is_file(run.exe);
}

// The exe copied to <work>\İndirilenler ronin\ronin-exe\ronin.exe (never run where it was built).
static b32 copy_exe(const wchar_t *exe) {
    wchar_t unpack[PATH_CAP], src[PATH_CAP];
    wjoin(unpack, run.work, UNPACK_FOLDER);
    wjoin(run.exe_dir, unpack, L"ronin-exe");
    if (!make_dir(unpack) || !make_dir(run.exe_dir)) return 0;
    full_path(src, exe);
    wjoin(run.exe, run.exe_dir, L"ronin.exe");
    if (!CopyFileW(src, run.exe, FALSE)) {
        put("e2e: FAIL: cannot copy ");
        put_w(src);
        end_line();
        return 0;
    }
    return 1;
}

// ---------------------------------------------------------------------------
// The command line and the run

static i32 usage(void) {
    say("e2e: usage: e2e (--zip <zip> | --exe <ronin.exe>) [--scenario 1,2] [--repeat N] [--out <dir>] "
        "[--demo-abort layout|foreground]");
    return 2;
}

static b32 parse_u32(const wchar_t *w, u32 *v) {
    u32 n = 0;
    if (!*w) return 0;
    for (; *w; w++) {
        if (*w < '0' || *w > '9' || n > 100000) return 0;
        n = n * 10 + (u32)(*w - '0');
    }
    *v = n;
    return 1;
}

static void stamp(wchar_t *dst) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    u32 parts[6] = { t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond };
    i32 widths[6] = { 4, 2, 2, 2, 2, 2 };
    i32 n = 0;
    for (i32 i = 0; i < 6; i++) {
        if (i == 3) dst[n++] = '-';
        for (i32 d = widths[i] - 1; d >= 0; d--) {
            u32 v = parts[i];
            for (i32 k = 0; k < d; k++) v /= 10;
            dst[n++] = (wchar_t)('0' + v % 10);
        }
    }
    dst[n] = 0;
}

static LRESULT CALLBACK clip_window_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) { return DefWindowProcW(hwnd, msg, wp, lp); }

int wmain(int argc, wchar_t **argv) {
    const wchar_t *zip = NULL, *exe = NULL, *out = NULL;
    b32 wanted[SCENARIOS + 1] = { 0 };
    b32 any_wanted = 0;
    u32 repeat = 1;
    for (i32 i = 1; i < argc; i++) {
        if (weq(argv[i], L"--zip") && i + 1 < argc) zip = argv[++i];
        else if (weq(argv[i], L"--exe") && i + 1 < argc) exe = argv[++i];
        else if (weq(argv[i], L"--out") && i + 1 < argc) out = argv[++i];
        else if (weq(argv[i], L"--repeat") && i + 1 < argc) {
            if (!parse_u32(argv[++i], &repeat) || repeat < 1 || repeat > 100) return usage();
        } else if (weq(argv[i], L"--demo-abort") && i + 1 < argc) {
            i++;
            if (weq(argv[i], L"layout")) run.demo = DEMO_LAYOUT;
            else if (weq(argv[i], L"foreground")) run.demo = DEMO_FOREGROUND;
            else return usage();
        } else if (weq(argv[i], L"--scenario") && i + 1 < argc) {
            const wchar_t *w = argv[++i];
            while (*w) {
                u32 v = 0;
                while (*w >= '0' && *w <= '9') v = v * 10 + (u32)(*w++ - '0');
                if (v < 1 || v > SCENARIOS || (*w && *w != ',')) return usage();
                wanted[v] = 1;
                any_wanted = 1;
                if (*w == ',') w++;
            }
        } else {
            return usage();
        }
    }
    if (!zip == !exe) return usage();
    if (!any_wanted) for (i32 i = 1; i <= SCENARIOS; i++) wanted[i] = 1;
    if (run.demo) {
        for (i32 i = 2; i <= SCENARIOS; i++) wanted[i] = 0;
        wanted[1] = 1;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // The repository: e2e.exe is <root>\build\e2e.exe.
    wchar_t self[PATH_CAP];
    DWORD sn = GetModuleFileNameW(NULL, self, PATH_CAP);
    i64 cut = sn;
    while (cut > 0 && self[cut - 1] != '\\') cut--;
    self[cut > 0 ? cut - 1 : 0] = 0; // <root>\build
    i64 bl = wlen(self);
    if (bl < 6 || CompareStringOrdinal(self + bl - 6, 6, L"\\build", 6, TRUE) != CSTR_EQUAL) {
        say("e2e: FAIL: run build\\e2e.exe from the repository's build folder");
        return 2;
    }
    self[bl - 6] = 0;
    wjoin(run.root, self, NULL);
    wchar_t tmp[PATH_CAP], name[32];
    stamp(name);
    if (out) {
        full_path(run.out, out);
    } else {
        wjoin(tmp, run.root, L"build\\e2e");
        make_dir(tmp);
        wjoin(run.out, tmp, name);
    }
    if (!make_dir(run.out)) {
        put("e2e: FAIL: cannot make ");
        put_w(run.out);
        end_line();
        return 2;
    }
    wjoin(tmp, run.out, L"e2e.log");
    log_file = CreateFileW(tmp, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    put("e2e: run ");
    put_w(name);
    put(", log ");
    put_w(tmp);
    end_line();

    // The record of the machine's state, and the preconditions (nothing typed yet).
    GetEnvironmentVariableW(L"APPDATA", run.real_appdata, PATH_CAP);
    GetEnvironmentVariableW(L"LOCALAPPDATA", run.real_localappdata, PATH_CAP);
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = clip_window_proc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = L"ronin_e2e_clipboard";
    RegisterClassW(&wc);
    run.clip_window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, wc.hInstance, NULL);
    MachineState *b = &before_state;
    b->layouts = buf_alloc(STATE_CAP);
    b->appdata = buf_alloc(SNAP_CAP);
    b->localappdata = buf_alloc(SNAP_CAP);
    if (GetKeyState(VK_CAPITAL) & 1) {
        say("e2e: FAIL: Caps Lock is on: turn it off, then run again (the driver never toggles it)");
        return 2;
    }
    if (clipboard_history_on()) {
        say("e2e: FAIL: Windows clipboard history is on: ronin's copies would stay in it; turn it off for the run");
        return 2;
    }
    if (!clip_save(b)) return 2;
    layout_state(&b->layouts, b->list, &b->list_count, &b->active);
    record_folders(b);
    GetCursorPos(&b->pointer);
    put("e2e: state: recorded: keyboard layouts ");
    put_layout_list(b->list, b->list_count, b->active);
    put("; clipboard ");
    if (b->clip_count) put_clip_formats(b);
    else put("empty");
    put("; ");
    put_w(run.real_appdata);
    put("\\ronin and ");
    put_w(run.real_localappdata);
    put("\\ronin");
    end_line();

    // The run's folder, the exe, the job, the watch.
    wjoin(run.work, run.root, L"build\\tmp\\e2e");
    DWORD wa = GetFileAttributesW(run.work);
    if (wa != INVALID_FILE_ATTRIBUTES && (wa & FILE_ATTRIBUTE_REPARSE_POINT)) {
        say("e2e: FAIL: build\\tmp\\e2e is a junction or a link");
        return 2;
    }
    remove_tree(run.work);
    wjoin(tmp, run.root, L"build\\tmp");
    make_dir(tmp);
    if (!make_dir(run.work)) {
        say("e2e: FAIL: cannot make build\\tmp\\e2e");
        return 2;
    }
    if (zip ? !unpack_zip(zip) : !copy_exe(exe)) return 2;
    put("e2e: exe ");
    put_w(run.exe);
    end_line();
    run.job = CreateJobObjectW(NULL, NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
    memset(&lim, 0, sizeof(lim));
    lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!run.job || !SetInformationJobObject(run.job, JobObjectExtendedLimitInformation, &lim, sizeof(lim))) {
        say("e2e: FAIL: cannot make a job object");
        return 2;
    }
    SetConsoleCtrlHandler(console_ctrl, TRUE);
    if (!watch_start()) {
        say("e2e: FAIL: cannot install the input and window hooks");
        return 2;
    }

    // The machine left alone: QUIET_MS without physical input.
    u64 t_start = GetTickCount64();
    InterlockedExchange64(&watch.last_physical, (LONG64)t_start);
    say("e2e: the keyboard and the mouse are taken next: waiting for 5 s without physical input (leave the machine now)");
    b32 quiet = 0;
    for (;;) {
        u64 now = GetTickCount64();
        u64 last = (u64)watch.last_physical;
        if (now >= last + QUIET_MS) {
            quiet = 1;
            break;
        }
        if (now >= t_start + QUIET_BOUND_MS || aborted()) break;
        wait_handle(watch.quiet_event, last + QUIET_MS, 1);
    }
    i32 exit_code = 0;
    u8 final_mem[1024];
    Buf final = { final_mem, 0, sizeof(final_mem) };
    i32 runs = 0;
    u64 t_run = GetTickCount64();
    if (!quiet) {
        bput(&final, aborted() ? "e2e: ABORT: " : "e2e: FAIL: the machine was not left alone (physical input within every 5 s for 2 minutes)");
        if (aborted()) bput_n(&final, watch.abort_text.data, watch.abort_text.len);
        exit_code = aborted() ? 3 : 1;
    } else {
        InterlockedExchange(&watch.armed, 1);
        say("e2e: the machine is left alone: any physical key or mouse input from now on aborts the run");
        typedef Result (*ScenarioFn)(void);
        static const ScenarioFn fns[SCENARIOS + 1] = { NULL, scenario_1, scenario_2 };
        for (u32 rep = 1; rep <= repeat && exit_code == 0; rep++) {
            run.repeat_index = (i32)rep;
            for (i32 i = 1; i <= SCENARIOS && exit_code == 0; i++) {
                if (!wanted[i]) continue;
                clip_empty();
                u64 t0 = GetTickCount64();
                put("e2e: s");
                put_i64(i);
                put(": run ");
                put_i64(rep);
                put(" of ");
                put_i64(repeat);
                end_line();
                Result r = fns[i]();
                runs++;
                put("e2e: s");
                put_i64(i);
                put(r == R_OK ? ": PASS (" : r == R_FAIL ? ": FAIL (" : ": ABORT (");
                bput_secs(&line, GetTickCount64() - t0);
                put(")");
                end_line();
                if (r == R_FAIL) {
                    exit_code = 1;
                    bput(&final, "e2e: FAIL: s");
                    bput_i64(&final, i);
                    bput(&final, " (run ");
                    bput_i64(&final, rep);
                    bput(&final, "; its lines above)");
                } else if (r == R_ABORT) {
                    exit_code = 3;
                    bput(&final, "e2e: ABORT: ");
                    bput_n(&final, watch.abort_text.data, watch.abort_text.len);
                    if (where.len) {
                        bput(&final, ", after ");
                        bput_n(&final, where.data, where.len);
                    }
                    bput(&final, " (not retried)");
                }
            }
        }
    }

    // Everything of the run ended, the machine put back and checked, also after an abort.
    InterlockedExchange(&watch.armed, 0);
    InterlockedExchange(&watch.exiting, 1);
    TerminateJobObject(run.job, 1);
    watch_stop();
    b32 state_ok = restore_and_check();
    if (!state_ok && exit_code == 0) {
        exit_code = 1;
        bput(&final, "e2e: FAIL: state (its lines above)");
    }
    if (exit_code == 0) {
        bput(&final, "e2e: PASS (");
        bput_i64(&final, runs);
        bput(&final, " scenario run");
        bput(&final, runs == 1 ? ", " : "s, ");
        bput_secs(&final, GetTickCount64() - t_run);
        bput(&final, ")");
    }
    bput_n(&line, final.data, final.len);
    end_line();
    CloseHandle(run.job);
    if (log_file != INVALID_HANDLE_VALUE) CloseHandle(log_file);
    return exit_code;
}
