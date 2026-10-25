/* ======================================================================
   ronin: release check: the helper of release-check.bat

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// The helper of release-check.bat (#30; build-time tool, not part of the editor; build.bat compiles it on every build,
// only release-check.bat runs it, never CI). It does what cmd cannot do reliably:
//   release_check stamp
//       the local time as yyyymmdd-hhmmss (the run's log name; %date% depends on the locale)
//   release_check versions <root>
//       src\app\version.h's RONIN_VERSION (equal to its MAJOR.MINOR.PATCH), CHANGELOG.md's first dated heading
//       "## X.Y.Z — YYYY-MM-DD" and docs\releases\X.Y.Z.md: one version, or one line naming the mismatch
//   release_check zip <zip> <version> <work dir> <dist list> <debug exe> <imports.txt>
//       the zip unpacked by Windows' tar into <work dir>\İndirilenler ronin (a space and a non-ASCII letter, as a
//       user's Downloads folder; tar runs there, so it never gets that name as an argument), then from there: its
//       files exactly those of <dist list> (res\dist_files.txt, dist.bat's list), --version, the version resource,
//       --check-imports through <debug exe>, a --no-activate start and close (start-close), nothing written (the
//       unpacked folder, APPDATA and LOCALAPPDATA, which the caller points at empty folders) and --startup-ms
//       --no-activate three times (informational)
//   release_check start-close <exe> <cwd> [flags...]
//       <exe> [flags] started with <cwd>: its window, the title "*scratch* - ronin", 3 s, WM_CLOSE, exit code 0; every
//       foreground change meanwhile is watched (SetWinEventHook, EVENT_SYSTEM_FOREGROUND, out of context), and one that
//       names a window of the process fails
//   release_check snapshot <dir> <out>
//       every file and folder below <dir>, sorted: path, size, last write time, an FNV-1a 64 of the content; or
//       "(missing)"; release-check.bat compares two with fc
//   release_check human <MANUAL_TESTS.md>
//       every row whose status is HUMAN or has "+ HUMAN" (mixed), with its id, its step, the HUMAN clauses of a mixed
//       row's evidence and the numbered HUMAN steps that name it
//   release_check coverage <cobertura.xml> <src dir>
//       line coverage per file under <src dir> (a line counted once per file, covered when any record of it has hits)
// Every line it prints starts with its mode. Exit 0 = ok, 1 = a failed check, 2 = wrong arguments or unreadable input.
// No C runtime calls: Win32 only (the CRT's startup aside, as the other tools).

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef unsigned char u8;
typedef int i32;
typedef unsigned int u32;
typedef long long i64;
typedef unsigned long long u64;
typedef int b32;

#define PATH_CAP 1024
#define LINE_CAP 8192
#define ENTRIES_MAX 8192
#define LIST_MAX 256
#define STEPS_MAX 256
#define COVER_FILES_MAX 512
#define COVER_LINES_MAX (1 << 18)
#define TITLE_EXPECTED L"*scratch* - ronin"
#define UNPACK_FOLDER L"İndirilenler ronin"

typedef struct Str {
    const u8 *data;
    i64 len;
} Str;

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

static b32 str_starts(Str s, const char *prefix) {
    Str p = cstr(prefix);
    return s.len >= p.len && mem_eq(s.data, p.data, p.len);
}

static i64 str_find(Str s, const char *needle, i64 from) {
    Str n = cstr(needle);
    for (i64 i = from; i + n.len <= s.len; i++) if (mem_eq(s.data + i, n.data, n.len)) return i;
    return -1;
}

static Str str_skip(Str s, i64 n) { return n >= s.len ? str(s.data + s.len, 0) : str(s.data + n, s.len - n); }

static Str str_trim(Str s) {
    while (s.len && (s.data[0] == ' ' || s.data[0] == '\t')) s.data++, s.len--;
    while (s.len && (s.data[s.len - 1] == ' ' || s.data[s.len - 1] == '\t' || s.data[s.len - 1] == '\r')) s.len--;
    return s;
}

static b32 is_digit(u8 c) { return c >= '0' && c <= '9'; }

// The next line of *rest (without its LF); 0 at the end.
static b32 next_line(Str *rest, Str *line) {
    if (rest->len <= 0) return 0;
    i64 i = 0;
    while (i < rest->len && rest->data[i] != '\n') i++;
    *line = str(rest->data, i);
    if (line->len && line->data[line->len - 1] == '\r') line->len--;
    *rest = str_skip(*rest, i + 1);
    return 1;
}

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

// ---------------------------------------------------------------------------
// Output: one line at a time, UTF-8, CRLF

static u8 line_out[LINE_CAP];
static i64 line_len;

static void put_n(const u8 *s, i64 n) {
    for (i64 i = 0; i < n && line_len < LINE_CAP - 2; i++) line_out[line_len++] = s[i];
}

static void put(const char *c) { put_n((const u8 *)c, cstr_len(c)); }

static void put_s(Str s) { put_n(s.data, s.len); }

static void put_w(const wchar_t *w) {
    u8 tmp[PATH_CAP * 3];
    i32 n = WideCharToMultiByte(CP_UTF8, 0, w, (i32)wlen(w), (char *)tmp, (i32)sizeof(tmp), NULL, NULL);
    if (n > 0) put_n(tmp, n);
}

static void put_i64(i64 v) {
    u8 digits[24];
    i32 n = 0;
    b32 neg = v < 0;
    u64 u = neg ? (u64)(-(v + 1)) + 1 : (u64)v;
    do {
        digits[n++] = (u8)('0' + u % 10);
        u /= 10;
    } while (u);
    if (neg) put("-");
    while (n > 0) put_n(&digits[--n], 1);
}

static void put_hex(u64 v, i32 digits) {
    static const char hex[] = "0123456789abcdef";
    for (i32 i = digits - 1; i >= 0; i--) put_n((const u8 *)&hex[(v >> (i * 4)) & 15], 1);
}

// One decimal: 123 of 456 -> "27.0%".
static void put_percent(i64 part, i64 whole) {
    i64 tenths = whole ? (part * 1000 + whole / 2) / whole : 0;
    put_i64(tenths / 10);
    put(".");
    put_i64(tenths % 10);
    put("%");
}

static void end_line(void) {
    line_out[line_len++] = '\r';
    line_out[line_len++] = '\n';
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line_out, (DWORD)line_len, &written, NULL);
    line_len = 0;
}

static void say(const char *a) {
    put(a);
    end_line();
}

// ---------------------------------------------------------------------------
// Files

// The whole file in fresh pages (released only at the exit); data 0 when it cannot be read.
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
        }
    }
    CloseHandle(f);
    return s;
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

static void walk_dir(Walk *w, const wchar_t *dir, const u8 *rel, i32 rel_len, b32 hash) {
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
            // A junction or a link is listed, never followed.
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) walk_dir(w, child, e->path, e->path_len, hash);
        } else {
            e->size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            if (hash) e->hash = hash_file(child, &e->unreadable);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

static b32 entry_less(const Entry *a, const Entry *b) {
    i32 n = a->path_len < b->path_len ? a->path_len : b->path_len;
    for (i32 i = 0; i < n; i++) if (a->path[i] != b->path[i]) return a->path[i] < b->path[i];
    return a->path_len < b->path_len;
}

static void walk(Walk *w, const wchar_t *dir, b32 hash) {
    if (!w->items) w->items = (Entry *)VirtualAlloc(NULL, sizeof(Entry) * ENTRIES_MAX, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    w->count = 0;
    w->full = 0;
    walk_dir(w, dir, NULL, 0, hash);
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

// The snapshot's text: one line per entry, or "(missing)".
static i64 snapshot_text(const wchar_t *dir, u8 *out, i64 cap) {
    static Walk w;
    i64 n = 0;
#define SNAP_PUT(p, l) do { i64 l_ = (l); if (n + l_ < cap) { memcpy(out + n, (p), (size_t)l_); n += l_; } } while (0)
    if (!is_dir(dir)) {
        SNAP_PUT("(missing)\r\n", 11);
        return n;
    }
    walk(&w, dir, 1);
    for (i32 i = 0; i < w.count; i++) {
        Entry *e = &w.items[i];
        line_len = 0;
        put_n(e->path, e->path_len);
        if (e->folder) {
            put("/");
        } else {
            put(" ");
            put_i64((i64)e->size);
            put(" ");
            put_hex(e->written, 16);
            put(" ");
            if (e->unreadable) put("(unreadable)");
            else put_hex(e->hash, 16);
        }
        put("\r\n");
        SNAP_PUT(line_out, line_len);
        line_len = 0;
    }
    if (w.full) SNAP_PUT("(more entries than the snapshot holds)\r\n", 40);
#undef SNAP_PUT
    return n;
}

// ---------------------------------------------------------------------------
// Processes

// Starts cmdline (modified in place by CreateProcessW) in cwd; out, when given, gets the child's stdout and stderr.
static b32 start_process(wchar_t *cmdline, const wchar_t *cwd, HANDLE out, DWORD flags, PROCESS_INFORMATION *pi) {
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    if (out) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = out;
        si.hStdError = out;
    }
    memset(pi, 0, sizeof(*pi));
    return CreateProcessW(NULL, cmdline, NULL, NULL, out != NULL, flags, NULL, cwd, &si, pi) != 0;
}

// Runs it to its end (at most timeout_ms); its exit code, or -1 when it could not start, -2 when it was stopped.
static i64 run_process(wchar_t *cmdline, const wchar_t *cwd, DWORD timeout_ms) {
    PROCESS_INFORMATION pi;
    if (!start_process(cmdline, cwd, NULL, 0, &pi)) return -1;
    CloseHandle(pi.hThread);
    i64 code = -2;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) {
        DWORD c = 0;
        GetExitCodeProcess(pi.hProcess, &c);
        code = c;
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    CloseHandle(pi.hProcess);
    return code;
}

// The same with stdout and stderr read into out (at most cap bytes kept).
static i64 run_capture(wchar_t *cmdline, const wchar_t *cwd, DWORD timeout_ms, u8 *out, i64 cap, i64 *out_len) {
    *out_len = 0;
    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    PROCESS_INFORMATION pi;
    b32 started = start_process(cmdline, cwd, wr, 0, &pi);
    CloseHandle(wr);
    if (!started) {
        CloseHandle(rd);
        return -1;
    }
    CloseHandle(pi.hThread);
    u8 chunk[4096];
    DWORD got;
    while (ReadFile(rd, chunk, sizeof(chunk), &got, NULL) && got) {
        for (DWORD i = 0; i < got && *out_len < cap; i++) out[(*out_len)++] = chunk[i];
    }
    CloseHandle(rd);
    i64 code = -2;
    if (WaitForSingleObject(pi.hProcess, timeout_ms) == WAIT_OBJECT_0) {
        DWORD c = 0;
        GetExitCodeProcess(pi.hProcess, &c);
        code = c;
    } else {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    }
    CloseHandle(pi.hProcess);
    return code;
}

// cmdline = "exe" followed by each argument, each quoted.
static void build_cmdline(wchar_t *dst, const wchar_t *exe, const wchar_t *const *args, i32 count) {
    i64 n = 0;
#define CMD_PUT(w) do { const wchar_t *w_ = (w); for (i64 k_ = 0; w_[k_] && n < 4 * PATH_CAP - 2; k_++) dst[n++] = w_[k_]; } while (0)
    CMD_PUT(L"\"");
    CMD_PUT(exe);
    CMD_PUT(L"\"");
    for (i32 i = 0; i < count; i++) {
        b32 quote = 0;
        for (const wchar_t *c = args[i]; *c; c++) if (*c == ' ') quote = 1;
        CMD_PUT(L" ");
        if (quote) CMD_PUT(L"\"");
        CMD_PUT(args[i]);
        if (quote) CMD_PUT(L"\"");
    }
#undef CMD_PUT
    dst[n] = 0;
}

// ---------------------------------------------------------------------------
// The foreground watch: every EVENT_SYSTEM_FOREGROUND while a process runs

typedef struct Watch {
    DWORD pid;
    u64 start;
    i32 events;          // every foreground change seen
    b32 hit;             // one named a window of the process
    wchar_t title[256];  // the first such window's title
    u64 hit_ms;          // when, since the start
    i32 others;          // changes to other programs' windows
} Watch;

static Watch watch;

static void watch_record(HWND hwnd) {
    if (watch.hit) return;
    watch.hit = 1;
    watch.hit_ms = GetTickCount64() - watch.start;
    watch.title[0] = 0;
    if (hwnd) GetWindowTextW(hwnd, watch.title, 256);
    if (!watch.title[0]) wjoin(watch.title, L"(no title, or gone)", NULL);
}

// The hook filtered to the process: any event it delivers is one of its windows, even one already destroyed.
static void CALLBACK watch_own(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child, DWORD thread, DWORD ms) {
    (void)hook, (void)event, (void)object, (void)child, (void)thread, (void)ms;
    watch_record(hwnd);
}

// The hook on every process: each change counted, attributed by the window's owner.
static void CALLBACK watch_any(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child, DWORD thread, DWORD ms) {
    (void)hook, (void)event, (void)object, (void)child, (void)thread, (void)ms;
    watch.events++;
    DWORD pid = 0;
    if (hwnd) GetWindowThreadProcessId(hwnd, &pid);
    if (pid && pid == watch.pid) watch_record(hwnd);
    else watch.others++;
}

// Pumps this thread's messages (the out-of-context hooks are delivered through them) for ms, or until handle is
// signaled (when given); 1 when it was.
static b32 pump(HANDLE handle, u64 ms) {
    u64 end = GetTickCount64() + ms;
    for (;;) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        if (handle && WaitForSingleObject(handle, 0) == WAIT_OBJECT_0) return 1;
        u64 now = GetTickCount64();
        if (now >= end) return 0;
        DWORD left = (DWORD)(end - now);
        MsgWaitForMultipleObjectsEx(handle ? 1 : 0, handle ? &handle : NULL, left, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
}

typedef struct FindWindowCtx {
    DWORD pid;
    HWND found;
} FindWindowCtx;

static BOOL CALLBACK find_window_cb(HWND hwnd, LPARAM lparam) {
    FindWindowCtx *c = (FindWindowCtx *)lparam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == c->pid && IsWindowVisible(hwnd) && !GetWindow(hwnd, GW_OWNER)) {
        c->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

typedef enum WatchMode {
    WATCH_CLOSE,   // wait for the window, check the title, 3 s, WM_CLOSE, exit code 0
    WATCH_TO_END,  // wait for the process to end by itself (--startup-ms); its exit code
} WatchMode;

// Starts exe with args in cwd, suspended, the foreground watched before it runs. Prints its lines under prefix;
// returns 1 when every check held. *exit_code gets the process's exit code (or -1).
static b32 watched_run(const char *prefix, const wchar_t *exe, const wchar_t *cwd, const wchar_t *const *args, i32 count,
                       WatchMode mode, i64 *exit_code) {
    static wchar_t cmdline[4 * PATH_CAP];
    build_cmdline(cmdline, exe, args, count);
    *exit_code = -1;
    PROCESS_INFORMATION pi;
    if (!start_process(cmdline, cwd, NULL, CREATE_SUSPENDED, &pi)) {
        put(prefix);
        put(": FAIL: could not start ");
        put_w(exe);
        end_line();
        return 0;
    }
    memset(&watch, 0, sizeof(watch));
    watch.pid = pi.dwProcessId;
    watch.start = GetTickCount64();
    HWINEVENTHOOK own = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, watch_own, pi.dwProcessId, 0,
                                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    HWINEVENTHOOK any = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, NULL, watch_any, 0, 0,
                                        WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (!own || !any) {
        if (own) UnhookWinEvent(own);
        if (any) UnhookWinEvent(any);
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        put(prefix);
        put(": FAIL: could not watch the foreground (SetWinEventHook)");
        end_line();
        return 0;
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);

    b32 ok = 1, ended = 0;
    if (mode == WATCH_CLOSE) {
        FindWindowCtx ctx = { pi.dwProcessId, NULL };
        u64 give_up = GetTickCount64() + 10000;
        while (!ctx.found && !ended && GetTickCount64() < give_up) {
            ended = pump(pi.hProcess, 50);
            EnumWindows(find_window_cb, (LPARAM)&ctx);
        }
        if (!ctx.found) {
            put(prefix);
            put(ended ? ": FAIL: the process ended before it showed a window" : ": FAIL: no window within 10 s");
            end_line();
            ok = 0;
        } else {
            ended = pump(pi.hProcess, 3000);
            wchar_t title[256];
            title[0] = 0;
            if (!ended) GetWindowTextW(ctx.found, title, 256);
            if (ended || !weq(title, TITLE_EXPECTED)) {
                put(prefix);
                put(": FAIL: ");
                if (ended) {
                    put("the process ended by itself within 3 s");
                } else {
                    put("the title is \"");
                    put_w(title);
                    put("\", not \"*scratch* - ronin\"");
                }
                end_line();
                ok = 0;
            }
            if (!ended) {
                PostMessageW(ctx.found, WM_CLOSE, 0, 0);
                ended = pump(pi.hProcess, 10000);
                if (!ended) {
                    put(prefix);
                    put(": FAIL: still running 10 s after WM_CLOSE");
                    end_line();
                    ok = 0;
                }
            }
        }
    } else {
        ended = pump(pi.hProcess, 60000);
        if (!ended) {
            put(prefix);
            put(": FAIL: still running after 60 s");
            end_line();
            ok = 0;
        }
    }
    if (!ended) {
        TerminateProcess(pi.hProcess, 1);
        WaitForSingleObject(pi.hProcess, 5000);
    } else {
        DWORD c = 0;
        GetExitCodeProcess(pi.hProcess, &c);
        *exit_code = c;
    }
    pump(NULL, 250); // events queued before the end arrive
    UnhookWinEvent(own);
    UnhookWinEvent(any);
    CloseHandle(pi.hProcess);

    if (mode == WATCH_CLOSE && ended && ok && *exit_code != 0) {
        put(prefix);
        put(": FAIL: exit code ");
        put_i64(*exit_code);
        put(" after WM_CLOSE");
        end_line();
        ok = 0;
    }
    if (watch.hit) {
        put(prefix);
        put(": FAIL: the foreground went to its window \"");
        put_w(watch.title);
        put("\" at +");
        put_i64((i64)watch.hit_ms);
        put(" ms");
        end_line();
        ok = 0;
    }
    put(prefix);
    put(": foreground changes while it ran: ");
    put_i64(watch.events);
    put(watch.hit ? " (one of them to its window)" : ", none to its window");
    if (watch.others) {
        put("; ");
        put_i64(watch.others);
        put(" to another program (someone used the machine?)");
    }
    end_line();
    return ok;
}

// ---------------------------------------------------------------------------
// versions

// The quoted value or the number after "#define NAME " in text; empty when there is none.
static Str define_value(Str text, const char *name) {
    Str rest = text, line;
    Str key = cstr(name);
    while (next_line(&rest, &line)) {
        if (!str_starts(line, "#define ")) continue;
        Str after = str_skip(line, 8);
        if (after.len <= key.len || !mem_eq(after.data, key.data, key.len) || after.data[key.len] != ' ') continue;
        Str v = str_trim(str_skip(after, key.len + 1));
        if (v.len >= 2 && v.data[0] == '"' && v.data[v.len - 1] == '"') v = str(v.data + 1, v.len - 2);
        return v;
    }
    return str(text.data, 0);
}

static i32 mode_versions(const wchar_t *root) {
    wchar_t path[PATH_CAP];
    wjoin(path, root, L"src\\app\\version.h");
    Str h = read_file(path);
    if (!h.data) {
        say("versions: FAIL: cannot read src\\app\\version.h");
        return 2;
    }
    Str v = define_value(h, "RONIN_VERSION");
    u8 parts[64];
    i64 pn = 0;
    const char *names[3] = { "RONIN_VERSION_MAJOR", "RONIN_VERSION_MINOR", "RONIN_VERSION_PATCH" };
    for (i32 i = 0; i < 3; i++) {
        Str p = define_value(h, names[i]);
        if (i) parts[pn++] = '.';
        for (i64 k = 0; k < p.len && pn < 60; k++) parts[pn++] = p.data[k];
    }
    if (!v.len) {
        say("versions: FAIL: src\\app\\version.h has no RONIN_VERSION");
        return 1;
    }
    if (!str_eq(v, str(parts, pn))) {
        put("versions: FAIL: src\\app\\version.h: RONIN_VERSION \"");
        put_s(v);
        put("\" but MAJOR.MINOR.PATCH ");
        put_s(str(parts, pn));
        end_line();
        return 1;
    }
    put("versions: src\\app\\version.h: ");
    put_s(v);
    end_line();

    wjoin(path, root, L"CHANGELOG.md");
    Str log = read_file(path);
    if (!log.data) {
        say("versions: FAIL: cannot read CHANGELOG.md");
        return 2;
    }
    Str rest = log, line;
    i64 number = 0, unreleased = 0;
    b32 found = 0;
    while (next_line(&rest, &line)) {
        number++;
        if (str_eq(str_trim(line), cstr("## Unreleased"))) unreleased = number;
        if (!str_starts(line, "## ") || line.len < 4 || !is_digit(line.data[3])) continue;
        found = 1;
        Str heading = str_skip(line, 3);
        i64 sp = 0;
        while (sp < heading.len && heading.data[sp] != ' ') sp++;
        Str hv = str(heading.data, sp);
        Str tail = str_skip(heading, sp);
        // " — YYYY-MM-DD": a space, U+2014 in UTF-8, a space, the date.
        b32 dated = tail.len == 15 && str_starts(tail, " \xE2\x80\x94 ");
        for (i64 k = 5; dated && k < 15; k++) dated = (k == 9 || k == 12) ? tail.data[k] == '-' : is_digit(tail.data[k]);
        if (!dated) {
            put("versions: FAIL: CHANGELOG.md:");
            put_i64(number);
            put(": the first version heading is not \"## X.Y.Z \xE2\x80\x94 YYYY-MM-DD\"");
            end_line();
            return 1;
        }
        if (!str_eq(hv, v)) {
            put("versions: FAIL: CHANGELOG.md:");
            put_i64(number);
            put(": its first dated section is ");
            put_s(hv);
            put(", src\\app\\version.h says ");
            put_s(v);
            end_line();
            return 1;
        }
        put("versions: CHANGELOG.md:");
        put_i64(number);
        put(": ");
        put_s(heading);
        if (unreleased) {
            put(" (an Unreleased section above it, line ");
            put_i64(unreleased);
            put(": a dry run, not a release)");
        }
        end_line();
        break;
    }
    if (!found) {
        say("versions: FAIL: CHANGELOG.md has no dated version section");
        return 1;
    }
    wchar_t notes[PATH_CAP], wv[64];
    wfrom_utf8(wv, 64, v);
    wjoin(notes, root, L"docs\\releases");
    wjoin(path, notes, wv);
    wjoin(notes, path, NULL);
    i64 nl = wlen(notes);
    wjoin(notes + nl, L".md", NULL);
    if (!is_file(notes)) {
        put("versions: FAIL: docs\\releases\\");
        put_s(v);
        put(".md does not exist");
        end_line();
        return 1;
    }
    put("versions: docs\\releases\\");
    put_s(v);
    put(".md: present");
    end_line();
    return 0;
}

// ---------------------------------------------------------------------------
// zip

// res\dist_files.txt: one path per line ('/' separated), '#' comments and blank lines skipped.
static i32 read_list(Str text, Str *items, i32 cap) {
    i32 n = 0;
    Str rest = text, line;
    while (next_line(&rest, &line)) {
        Str t = str_trim(line);
        if (!t.len || t.data[0] == '#') continue;
        if (n < cap) items[n] = t;
        n++;
    }
    return n;
}

static b32 check_version_resource(const wchar_t *exe, Str version) {
    DWORD ignored = 0;
    DWORD size = GetFileVersionInfoSizeW(exe, &ignored);
    static u8 info[1 << 16];
    if (!size || size > sizeof(info) || !GetFileVersionInfoW(exe, 0, size, info)) {
        say("zip: version-resource: FAIL: the exe has no version resource");
        return 0;
    }
    u32 want[4] = { 0, 0, 0, 0 };
    i32 part = 0;
    for (i64 i = 0; i < version.len; i++) {
        if (version.data[i] == '.') part++;
        else if (part < 3) want[part] = want[part] * 10 + (version.data[i] - '0');
    }
    VS_FIXEDFILEINFO *fixed = NULL;
    UINT len = 0;
    b32 ok = VerQueryValueW(info, L"\\", (void **)&fixed, &len) && fixed && len >= sizeof(*fixed);
    DWORD ms = (want[0] << 16) | want[1], ls = want[2] << 16;
    if (!ok || fixed->dwFileVersionMS != ms || fixed->dwFileVersionLS != ls || fixed->dwProductVersionMS != ms ||
        fixed->dwProductVersionLS != ls) {
        put("zip: version-resource: FAIL: the fixed FileVersion / ProductVersion are not ");
        put_s(version);
        put(".0");
        if (ok) {
            put(" (");
            put_i64(fixed->dwFileVersionMS >> 16);
            put(".");
            put_i64(fixed->dwFileVersionMS & 0xffff);
            put(".");
            put_i64(fixed->dwFileVersionLS >> 16);
            put(".");
            put_i64(fixed->dwFileVersionLS & 0xffff);
            put(")");
        }
        end_line();
        return 0;
    }
    WORD *translation = NULL;
    if (!VerQueryValueW(info, L"\\VarFileInfo\\Translation", (void **)&translation, &len) || len < 4) {
        say("zip: version-resource: FAIL: no Translation entry");
        return 0;
    }
    static const wchar_t hex[] = L"0123456789ABCDEF";
    wchar_t block[64] = L"\\StringFileInfo\\00000000\\";
    for (i32 i = 0; i < 4; i++) {
        block[16 + i] = hex[(translation[0] >> (12 - 4 * i)) & 15];
        block[20 + i] = hex[(translation[1] >> (12 - 4 * i)) & 15];
    }
    wchar_t wv[64];
    wfrom_utf8(wv, 64, version);
    const wchar_t *keys[2] = { L"FileVersion", L"ProductVersion" };
    for (i32 k = 0; k < 2; k++) {
        wchar_t query[128];
        wjoin(query, block, NULL);
        wjoin(query + wlen(query), keys[k], NULL);
        wchar_t *value = NULL;
        if (!VerQueryValueW(info, query, (void **)&value, &len) || !value || !weq(value, wv)) {
            put("zip: version-resource: FAIL: the string ");
            put_w(keys[k]);
            put(" is \"");
            if (value) put_w(value);
            put("\", not ");
            put_s(version);
            end_line();
            return 0;
        }
    }
    put("zip: version-resource: FileVersion and ProductVersion ");
    put_s(version);
    put(" (");
    put_s(version);
    put(".0)");
    end_line();
    return 1;
}

static b32 no_files_below(const char *name, const wchar_t *dir) {
    static Walk w;
    walk(&w, dir, 0);
    i32 files = 0;
    for (i32 i = 0; i < w.count; i++) {
        Entry *e = &w.items[i];
        put("zip: nothing-written: ");
        put(name);
        put(e->folder ? " has the empty folder " : ": FAIL: a file was written: ");
        put_n(e->path, e->path_len);
        end_line();
        if (!e->folder) files++;
    }
    if (w.full) files++;
    return files == 0;
}

static i32 mode_zip(const wchar_t *zip_arg, const wchar_t *version_arg, const wchar_t *work_arg, const wchar_t *list_arg,
                    const wchar_t *debug_arg, const wchar_t *imports_arg) {
    wchar_t zip[PATH_CAP], work[PATH_CAP], list_path[PATH_CAP], debug_exe[PATH_CAP], imports[PATH_CAP], here[PATH_CAP];
    full_path(zip, zip_arg);
    full_path(work, work_arg);
    full_path(list_path, list_arg);
    full_path(debug_exe, debug_arg);
    full_path(imports, imports_arg);
    GetCurrentDirectoryW(PATH_CAP, here);
    u8 version_buf[64];
    i32 vn = WideCharToMultiByte(CP_UTF8, 0, version_arg, (i32)wlen(version_arg), (char *)version_buf, 63, NULL, NULL);
    Str version = str(version_buf, vn > 0 ? vn : 0);

    // 1. Unpacked into a folder with a space and a non-ASCII letter.
    wchar_t folder[PATH_CAP], root_name[128], root[PATH_CAP], exe[PATH_CAP];
    wjoin(folder, work, UNPACK_FOLDER);
    if (!CreateDirectoryW(work, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        say("zip: unpack: FAIL: cannot create the work folder");
        return 1;
    }
    if (!CreateDirectoryW(folder, NULL)) {
        put("zip: unpack: FAIL: cannot create ");
        put_w(folder);
        put(" (it must not exist yet)");
        end_line();
        return 1;
    }
    wchar_t tar[PATH_CAP], system_dir[PATH_CAP];
    GetSystemDirectoryW(system_dir, PATH_CAP);
    wjoin(tar, system_dir, L"tar.exe");
    static wchar_t cmdline[4 * PATH_CAP];
    const wchar_t *tar_args[3] = { L"-x", L"-f", zip };
    build_cmdline(cmdline, tar, tar_args, 3);
    i64 code = run_process(cmdline, folder, 120000);
    if (code != 0) {
        put("zip: unpack: FAIL: tar -x exit ");
        put_i64(code);
        end_line();
        return 1;
    }
    put("zip: unpack: ");
    put_w(folder);
    end_line();

    // 2. Its contents: one ronin-<version> folder holding exactly the list's files.
    wjoin(root_name, L"ronin-", NULL);
    wjoin(root_name + 6, version_arg, NULL);
    wjoin(root, folder, root_name);
    wjoin(exe, root, L"ronin.exe");
    static Walk top, below;
    walk(&top, folder, 0);
    i32 at_top = 0;
    for (i32 i = 0; i < top.count; i++) {
        b32 nested = 0;
        for (i32 k = 0; k < top.items[i].path_len && !nested; k++) nested = top.items[i].path[k] == '/';
        if (!nested) at_top++;
    }
    if (at_top != 1 || !is_dir(root)) {
        put("zip: contents: FAIL: the zip must hold exactly one folder, ronin-");
        put_s(version);
        put(" (it holds ");
        put_i64(at_top);
        put(" entries at the top)");
        end_line();
        return 1;
    }
    Str list_text = read_file(list_path);
    if (!list_text.data) {
        put("zip: contents: FAIL: cannot read ");
        put_w(list_arg);
        end_line();
        return 2;
    }
    static Str expected[LIST_MAX];
    i32 expected_count = read_list(list_text, expected, LIST_MAX);
    if (expected_count <= 0 || expected_count > LIST_MAX) {
        say("zip: contents: FAIL: the list is empty or too long");
        return 1;
    }
    walk(&below, root, 0);
    i32 wrong = 0, files = 0;
    for (i32 i = 0; i < expected_count; i++) {
        b32 seen = 0;
        for (i32 j = 0; j < below.count && !seen; j++) {
            Entry *e = &below.items[j];
            seen = !e->folder && str_eq(str(e->path, e->path_len), expected[i]);
        }
        if (!seen) {
            put("zip: contents: FAIL: missing ");
            put_s(expected[i]);
            end_line();
            wrong++;
        }
    }
    for (i32 j = 0; j < below.count; j++) {
        Entry *e = &below.items[j];
        Str p = str(e->path, e->path_len);
        b32 listed = 0;
        for (i32 i = 0; i < expected_count && !listed; i++) {
            Str x = expected[i];
            // A folder counts only as the parent of a listed file.
            listed = e->folder ? (x.len > p.len && mem_eq(x.data, p.data, p.len) && x.data[p.len] == '/')
                               : str_eq(p, x);
        }
        if (!e->folder) files++;
        if (!listed) {
            put("zip: contents: FAIL: extra ");
            put_s(p);
            put(e->folder ? "/" : "");
            end_line();
            wrong++;
        }
    }
    if (wrong || below.full) return 1;
    put("zip: contents: ");
    put_i64(files);
    put(" files, exactly those of ");
    put_w(list_arg);
    end_line();

    // The folder before anything ran from it.
    static u8 before[1 << 20], after[1 << 20];
    i64 before_len = snapshot_text(root, before, sizeof(before));

    // 3. --version through a pipe.
    {
        const wchar_t *args[1] = { L"--version" };
        build_cmdline(cmdline, exe, args, 1);
        u8 out[512];
        i64 out_len = 0;
        code = run_capture(cmdline, root, 30000, out, sizeof(out), &out_len);
        Str printed = str(out, out_len);
        while (printed.len && (printed.data[printed.len - 1] == '\n' || printed.data[printed.len - 1] == '\r')) printed.len--;
        u8 want[80] = "ronin ";
        i64 wn = 6;
        for (i64 i = 0; i < version.len && wn < 78; i++) want[wn++] = version.data[i];
        if (code != 0 || !str_eq(printed, str(want, wn))) {
            put("zip: version: FAIL: --version printed \"");
            put_s(printed);
            put("\" with exit ");
            put_i64(code);
            put(", not \"");
            put_s(str(want, wn));
            put("\" with exit 0");
            end_line();
            return 1;
        }
        put("zip: version: \"");
        put_s(printed);
        put("\", exit 0");
        end_line();
    }

    // 4. The version resource.
    if (!check_version_resource(exe, version)) return 1;

    // 5. The imports, read by the debug build's --check-imports.
    {
        const wchar_t *args[3] = { L"--check-imports", exe, imports };
        build_cmdline(cmdline, debug_exe, args, 3);
        code = run_process(cmdline, here, 60000);
        if (code != 0) {
            put("zip: imports: FAIL: --check-imports exit ");
            put_i64(code);
            put(" (build\\ronin.log says why)");
            end_line();
            return 1;
        }
        put("zip: imports: as res\\imports.txt lists");
        end_line();
    }

    // 6. A start that never takes the foreground, closed by WM_CLOSE.
    {
        const wchar_t *args[1] = { L"--no-activate" };
        i64 exit_code;
        if (!watched_run("zip: start-close", exe, root, args, 1, WATCH_CLOSE, &exit_code)) return 1;
        say("zip: start-close: shown with --no-activate, the title \"*scratch* - ronin\", closed by WM_CLOSE, exit 0");
    }

    // 7. Nothing written: the unpacked folder, APPDATA and LOCALAPPDATA.
    {
        i64 after_len = snapshot_text(root, after, sizeof(after));
        if (after_len != before_len || !mem_eq(before, after, before_len)) {
            say("zip: nothing-written: FAIL: the unpacked folder changed; before, then after:");
            DWORD written;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), before, (DWORD)before_len, &written, NULL);
            say("--");
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), after, (DWORD)after_len, &written, NULL);
            return 1;
        }
        wchar_t appdata[PATH_CAP], local[PATH_CAP];
        if (!GetEnvironmentVariableW(L"APPDATA", appdata, PATH_CAP) || !GetEnvironmentVariableW(L"LOCALAPPDATA", local, PATH_CAP)) {
            say("zip: nothing-written: FAIL: APPDATA or LOCALAPPDATA is not set");
            return 1;
        }
        b32 a = no_files_below("APPDATA", appdata);
        b32 l = no_files_below("LOCALAPPDATA", local);
        if (!a || !l) return 1;
        say("zip: nothing-written: the unpacked folder unchanged, no file in APPDATA or LOCALAPPDATA");
    }

    // 8. --startup-ms with --no-activate, three times (informational).
    {
        const wchar_t *args[2] = { L"--startup-ms", L"--no-activate" };
        i64 ms[3];
        for (i32 i = 0; i < 3; i++) {
            if (!watched_run("zip: startup-ms", exe, root, args, 2, WATCH_TO_END, &ms[i])) return 1;
            if (ms[i] < 1) {
                put("zip: startup-ms: FAIL: exit code ");
                put_i64(ms[i]);
                end_line();
                return 1;
            }
        }
        i64 lo = ms[0] < ms[1] ? ms[0] : ms[1], hi = ms[0] < ms[1] ? ms[1] : ms[0];
        i64 median = ms[2] < lo ? lo : ms[2] > hi ? hi : ms[2];
        put("zip: startup-ms: ");
        put_i64(ms[0]);
        put(", ");
        put_i64(ms[1]);
        put(", ");
        put_i64(ms[2]);
        put(" ms, median ");
        put_i64(median);
        put(" ms (with --no-activate: informational, not comparable with Phase 25's foreground numbers)");
        end_line();
    }
    return 0;
}

static i32 mode_start_close(const wchar_t *exe_arg, const wchar_t *cwd_arg, const wchar_t *const *flags, i32 count) {
    wchar_t exe[PATH_CAP], cwd[PATH_CAP];
    full_path(exe, exe_arg);
    full_path(cwd, cwd_arg);
    i64 exit_code;
    if (!watched_run("start-close", exe, cwd, flags, count, WATCH_CLOSE, &exit_code)) {
        say("start-close: FAIL");
        return 1;
    }
    say("start-close: ok: its window never had the foreground, closed by WM_CLOSE, exit 0");
    return 0;
}

// ---------------------------------------------------------------------------
// human

// The cells of a table row as GitHub splits them: at every '|' but an escaped "\|" (code spans included).
static i32 row_cells(Str line, Str *cells, i32 cap) {
    Str t = str_trim(line);
    if (t.len < 2 || t.data[0] != '|' || t.data[t.len - 1] != '|') return -1;
    i32 n = 0;
    i64 start = 1;
    for (i64 i = 1; i < t.len; i++) {
        if (t.data[i] == '\\' && i + 1 < t.len && t.data[i + 1] == '|') {
            i++;
            continue;
        }
        if (t.data[i] == '|') {
            if (n < cap) cells[n] = str_trim(str(t.data + start, i - start));
            n++;
            start = i + 1;
        }
    }
    return n;
}

static b32 is_step_id(Str s) {
    i64 i = 0;
    while (i < s.len && s.data[i] >= 'A' && s.data[i] <= 'Z') i++;
    if (i < 1 || i > 2 || i == s.len) return 0;
    for (; i < s.len; i++) if (!is_digit(s.data[i])) return 0;
    return 1;
}

typedef struct HumanStep {
    i64 number;
    Str ids; // "A1, A3"
} HumanStep;

static b32 step_names(Str ids, Str id) {
    i64 i = 0;
    while (i < ids.len) {
        i64 j = i;
        while (j < ids.len && ids.data[j] != ',') j++;
        if (str_eq(str_trim(str(ids.data + i, j - i)), id)) return 1;
        i = j + 1;
    }
    return 0;
}

// The evidence's clauses that contain HUMAN: split after '.' (before a space or the end) and ';', outside parentheses
// and code spans.
static void put_human_clauses(Str evidence) {
    i64 start = 0, depth = 0;
    b32 code = 0, first = 1;
    for (i64 i = 0; i <= evidence.len; i++) {
        b32 cut = i == evidence.len;
        if (!cut) {
            u8 c = evidence.data[i];
            if (c == '`') code = !code;
            else if (!code && c == '(') depth++;
            else if (!code && c == ')' && depth > 0) depth--;
            else if (!code && depth == 0 && (c == ';' || (c == '.' && (i + 1 == evidence.len || evidence.data[i + 1] == ' ')))) cut = 1;
        }
        if (!cut) continue;
        i64 end = i < evidence.len ? i + 1 : i;
        Str clause = str_trim(str(evidence.data + start, end - start));
        if (str_find(clause, "HUMAN", 0) >= 0) {
            if (!first) put(" ");
            put_s(clause);
            first = 0;
        }
        start = end;
    }
}

static i32 mode_human(const wchar_t *path) {
    Str text = read_file(path);
    if (!text.data) {
        say("human: FAIL: cannot read the manual test list");
        return 2;
    }
    // The numbered HUMAN steps first: "N. **ids**: ..." under "## HUMAN steps".
    static HumanStep steps[STEPS_MAX];
    i32 step_count = 0;
    Str rest = text, line;
    b32 in_steps = 0;
    while (next_line(&rest, &line)) {
        if (str_starts(line, "## ")) in_steps = str_eq(str_trim(line), cstr("## HUMAN steps"));
        if (!in_steps || !line.len || !is_digit(line.data[0])) continue;
        i64 i = 0, number = 0;
        while (i < line.len && is_digit(line.data[i])) number = number * 10 + (line.data[i++] - '0');
        if (!str_starts(str_skip(line, i), ". **")) continue;
        i64 from = i + 4, to = str_find(line, "**", from);
        if (to < 0 || step_count >= STEPS_MAX) continue;
        steps[step_count].number = number;
        steps[step_count].ids = str(line.data + from, to - from);
        step_count++;
    }
    i32 human = 0, mixed = 0, unnumbered = 0;
    rest = text;
    while (next_line(&rest, &line)) {
        Str cells[8];
        i32 n = row_cells(line, cells, 8);
        if (n < 4 || !is_step_id(cells[0])) continue;
        Str status = cells[2];
        b32 is_human = str_eq(status, cstr("HUMAN"));
        b32 is_mixed = !is_human && str_find(status, "+ HUMAN", 0) >= 0;
        if (!is_human && !is_mixed) continue;
        if (is_human) human++;
        else mixed++;
        put("human: ");
        put_s(cells[0]);
        put(is_human ? " (HUMAN, " : " (mixed, ");
        b32 any = 0;
        for (i32 s = 0; s < step_count; s++) {
            if (!step_names(steps[s].ids, cells[0])) continue;
            put(any ? ", " : "step ");
            put_i64(steps[s].number);
            any = 1;
        }
        if (!any) {
            put("in no numbered step");
            unnumbered++;
        }
        put("): ");
        put_s(cells[1]);
        end_line();
        if (is_mixed) {
            put("human:     HUMAN part: ");
            put_human_clauses(n > 3 ? cells[3] : str(cells[0].data, 0));
            end_line();
        }
    }
    if (human + mixed == 0) {
        say("human: FAIL: no HUMAN row found");
        return 1;
    }
    put("human: ");
    put_i64(human + mixed);
    put(" rows (");
    put_i64(human);
    put(" HUMAN, ");
    put_i64(mixed);
    put(" mixed), ");
    put_i64(unnumbered);
    put(" of them in no numbered HUMAN step");
    end_line();
    return 0;
}

// ---------------------------------------------------------------------------
// coverage

typedef struct CoverFile {
    u8 path[PATH_CAP]; // relative to the src folder's parent, '/' separated
    i64 path_len;
    u8 *lines;         // per line number: 1 seen, 2 covered
    i64 max_line;
} CoverFile;

static CoverFile *cover_files;
static i32 cover_count;

static u8 lower(u8 c) { return (c >= 'A' && c <= 'Z') ? (u8)(c + 32) : c == '/' ? '\\' : c; }

// The attribute's value in a tag ("name=\"...\""), empty when absent.
static Str attribute(Str tag, const char *name) {
    i64 at = 0;
    Str key = cstr(name);
    while ((at = str_find(tag, name, at)) >= 0) {
        b32 starts = at > 0 && (tag.data[at - 1] == ' ' || tag.data[at - 1] == '\t' || tag.data[at - 1] == '\n');
        if (starts && at + key.len + 1 < tag.len && tag.data[at + key.len] == '=' && tag.data[at + key.len + 1] == '"') {
            i64 from = at + key.len + 2, to = from;
            while (to < tag.len && tag.data[to] != '"') to++;
            return str(tag.data + from, to - from);
        }
        at++;
    }
    return str(tag.data, 0);
}

static i64 to_i64(Str s) {
    i64 v = 0;
    for (i64 i = 0; i < s.len && is_digit(s.data[i]); i++) v = v * 10 + (s.data[i] - '0');
    return v;
}

// The file of a class's filename under src_dir (lower-case compare, either slash), or 0 for any other file.
static CoverFile *cover_file(Str filename, const u8 *src_dir, i64 src_len, i64 keep_from) {
    if (filename.len <= src_len) return NULL;
    for (i64 i = 0; i < src_len; i++) if (lower(filename.data[i]) != lower(src_dir[i])) return NULL;
    if (filename.data[src_len] != '\\' && filename.data[src_len] != '/') return NULL;
    Str rel = str(filename.data + keep_from, filename.len - keep_from);
    for (i32 i = 0; i < cover_count; i++) {
        CoverFile *f = &cover_files[i];
        if (f->path_len != rel.len) continue;
        b32 same = 1;
        for (i64 k = 0; k < rel.len && same; k++) same = lower(f->path[k]) == lower(rel.data[k]);
        if (same) return f;
    }
    if (cover_count >= COVER_FILES_MAX || rel.len >= PATH_CAP) return NULL;
    CoverFile *f = &cover_files[cover_count++];
    for (i64 k = 0; k < rel.len; k++) f->path[k] = rel.data[k] == '\\' ? '/' : rel.data[k];
    f->path_len = rel.len;
    f->lines = (u8 *)VirtualAlloc(NULL, COVER_LINES_MAX, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return f->lines ? f : NULL;
}

static b32 cover_less(const CoverFile *a, const CoverFile *b) {
    i64 n = a->path_len < b->path_len ? a->path_len : b->path_len;
    for (i64 i = 0; i < n; i++) if (a->path[i] != b->path[i]) return a->path[i] < b->path[i];
    return a->path_len < b->path_len;
}

static i32 mode_coverage(const wchar_t *xml_path, const wchar_t *src_arg) {
    Str xml = read_file(xml_path);
    if (!xml.data) {
        say("coverage: FAIL: cannot read the cobertura report");
        return 2;
    }
    wchar_t src_w[PATH_CAP];
    full_path(src_w, src_arg);
    i64 sl = wlen(src_w);
    while (sl && src_w[sl - 1] == '\\') src_w[--sl] = 0;
    static u8 src_dir[PATH_CAP * 3];
    i32 src_len = WideCharToMultiByte(CP_UTF8, 0, src_w, (i32)sl, (char *)src_dir, (i32)sizeof(src_dir), NULL, NULL);
    if (src_len <= 0) return 2;
    i64 keep_from = src_len; // "src/..." kept: back to the separator before the folder's own name
    while (keep_from > 0 && src_dir[keep_from - 1] != '\\') keep_from--;
    cover_files = (CoverFile *)VirtualAlloc(NULL, sizeof(CoverFile) * COVER_FILES_MAX, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!cover_files) return 2;
    i64 at = 0, classes = 0;
    while ((at = str_find(xml, "<class ", at)) >= 0) {
        i64 tag_end = str_find(xml, ">", at);
        i64 class_end = str_find(xml, "</class>", at);
        if (tag_end < 0) break;
        if (class_end < 0) class_end = xml.len;
        classes++;
        Str tag = str(xml.data + at, tag_end - at);
        CoverFile *f = cover_file(attribute(tag, "filename"), src_dir, src_len, keep_from);
        i64 l = tag_end;
        while (f && (l = str_find(xml, "<line ", l)) >= 0 && l < class_end) {
            i64 le = str_find(xml, ">", l);
            if (le < 0) break;
            Str line_tag = str(xml.data + l, le - l);
            i64 number = to_i64(attribute(line_tag, "number")), hits = to_i64(attribute(line_tag, "hits"));
            if (number > 0 && number < COVER_LINES_MAX) {
                if (hits > 0) f->lines[number] = 2;
                else if (!f->lines[number]) f->lines[number] = 1;
                if (number > f->max_line) f->max_line = number;
            }
            l = le;
        }
        at = class_end;
    }
    if (cover_count == 0) {
        put("coverage: FAIL: no file under ");
        put_s(str(src_dir, src_len));
        put(" in the report (");
        put_i64(classes);
        put(" classes)");
        end_line();
        return 1;
    }
    for (i32 i = 1; i < cover_count; i++) {
        CoverFile tmp = cover_files[i];
        i32 j = i - 1;
        while (j >= 0 && cover_less(&tmp, &cover_files[j])) {
            cover_files[j + 1] = cover_files[j];
            j--;
        }
        cover_files[j + 1] = tmp;
    }
    i64 all = 0, covered = 0;
    for (i32 i = 0; i < cover_count; i++) {
        CoverFile *f = &cover_files[i];
        i64 n = 0, c = 0;
        for (i64 k = 1; k <= f->max_line; k++) {
            if (f->lines[k]) n++;
            if (f->lines[k] == 2) c++;
        }
        all += n;
        covered += c;
        put("coverage: ");
        put_n(f->path, f->path_len);
        put("  ");
        put_i64(c);
        put(" / ");
        put_i64(n);
        put(" lines  ");
        put_percent(c, n);
        end_line();
    }
    put("coverage: total: ");
    put_i64(cover_count);
    put(" files, ");
    put_i64(covered);
    put(" / ");
    put_i64(all);
    put(" lines (");
    put_percent(covered, all);
    put(")");
    end_line();
    return 0;
}

// ---------------------------------------------------------------------------

static i32 mode_stamp(void) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    i64 parts[6] = { t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond };
    for (i32 i = 0; i < 6; i++) {
        if (i == 3) put("-");
        if (i && parts[i] < 10) put("0");
        put_i64(parts[i]);
    }
    end_line();
    return 0;
}

static i32 mode_snapshot(const wchar_t *dir, const wchar_t *out) {
    static u8 text[1 << 22];
    i64 n = snapshot_text(dir, text, sizeof(text));
    if (!write_file(out, text, n)) {
        say("snapshot: FAIL: cannot write the snapshot");
        return 2;
    }
    return 0;
}

static i32 usage(void) {
    say("release_check: usage: stamp | versions <root> | zip <zip> <version> <work dir> <dist list> <debug exe> "
        "<imports.txt> | start-close <exe> <cwd> [flags...] | snapshot <dir> <out> | human <MANUAL_TESTS.md> | "
        "coverage <cobertura.xml> <src dir>");
    return 2;
}

int wmain(int argc, wchar_t **argv) {
    if (argc < 2) return usage();
    const wchar_t *mode = argv[1];
    if (weq(mode, L"stamp") && argc == 2) return mode_stamp();
    if (weq(mode, L"versions") && argc == 3) return mode_versions(argv[2]);
    if (weq(mode, L"zip") && argc == 8) return mode_zip(argv[2], argv[3], argv[4], argv[5], argv[6], argv[7]);
    if (weq(mode, L"start-close") && argc >= 4) return mode_start_close(argv[2], argv[3], (const wchar_t *const *)argv + 4, argc - 4);
    if (weq(mode, L"snapshot") && argc == 4) return mode_snapshot(argv[2], argv[3]);
    if (weq(mode, L"human") && argc == 3) return mode_human(argv[2]);
    if (weq(mode, L"coverage") && argc == 4) return mode_coverage(argv[2], argv[3]);
    return usage();
}
