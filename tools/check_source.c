/* ======================================================================
   ronin: build check: headers, module map, includes, C runtime

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// The source check (Phase 23; build-time tool, not part of the editor; build.bat compiles it on every build,
// verify.bat and the CI workflow run it: check_source <repository root>).
//
// Every .c, .h, .cpp and .hlsl under src\ and every file under tools\ must start with ronin's header, byte for byte
// but its "ronin: <description>" line (a line may end in CRLF), the description at most 72 characters and equal to
// the file's row in docs\ARCHITECTURE.md's module map: under a "### `src/<folder>/`" heading, a row "| `name` (,
// `name`) | description | details |". Every source file has exactly one row, every row a file. Two include rules on
// src\: outside src\platform\ only the system headers of SYSTEM_ALLOWED (so no Windows SDK header), and a quoted
// include is a file of the repository or one of the generated headers of GENERATED; nothing in src\base\ or
// src\text\ includes from src\editor\, src\project\ or src\app\. Only lines whose first non-blank character is '#'
// count. Each problem is one "path:line: message" line on stdout; the last line is "check_source: PASS: ..." (exit
// 0) or "check_source: FAIL: N problems" (exit 1). No C runtime calls: Win32 only.
//
// The C runtime (#36): in every .c, .h and .cpp under src\ and tools\, a call of a function of CRT_NAMES (an
// identifier of the list followed by '(', not after '.', '->' or '::'; in a #define body the bare name counts too),
// outside comments and string and character literals; an #include of a header of CRT_HEADERS in src\platform\ and
// tools\ (elsewhere in src\ the include rule above refuses it already); a byte >= 0x80 inside a string or character
// literal (a literal never depends on the code page the compiler reads its source in).
// "check_source --link MAP..." (the release and bench builds' linker maps) fails on a symbol of a C runtime library but
// the functions of LINK_FUNCTIONS and the data of LINK_OBJECTS: what a macro or the compiler brings in, which the source
// does not show.
// "check_source --self-test" runs the cases of the SELF_ tables.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef unsigned char u8;
typedef unsigned int u32;

#define CHECK_FILES_MAX 1024
#define CHECK_ROWS_MAX 1024
#define CHECK_PATH_CAP 260
#define CHECK_DESC_CAP 512
#define CHECK_DESC_MAX 72
#define CHECK_TEXT_CAP (64u << 20)

typedef struct SourceFile {
    char path[CHECK_PATH_CAP]; // relative to the root, '/' separated
} SourceFile;

typedef struct MapRow {
    char path[CHECK_PATH_CAP]; // the heading's folder and the row's name
    char desc[CHECK_DESC_CAP];
    u32 line;
    u32 used;
} MapRow;

static const char *const header_lines[8] = {
    "/* ======================================================================",
    0, // "   ronin: <description>"
    "",
    "   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see",
    "   LICENSE in the repository root. This software is provided \"as is\",",
    "   without warranty of any kind, express or implied.",
    "   https://github.com/Bskasan/ronin-text-editor",
    "   ====================================================================== */",
};
static const char *const DESC_PREFIX = "   ronin: ";
static const char *const SYSTEM_ALLOWED[] = { "stdint.h", "stddef.h", "stdarg.h", "string.h", "emmintrin.h", "intrin.h" };
static const char *const GENERATED[] = { "quad_vs.h", "quad_ps.h", "themes.h" }; // build\gen, through /Ibuild\gen
static const char *const MAP_FILE = "docs/ARCHITECTURE.md";

// The C runtime's functions, by header: a call of one fails. memcpy, memset and memmove are not here (allowed).
static const char *const CRT_NAMES[] = {
    // <string.h>, <wchar.h> and MSVC's own string functions
    "memcmp", "memchr", "memccpy", "_memccpy", "_memicmp", "memcpy_s", "memmove_s", "memset_s",
    "strlen", "strnlen", "strnlen_s", "strcpy", "strcpy_s", "strncpy", "strncpy_s", "strcat", "strcat_s", "strncat",
    "strncat_s", "strcmp", "strncmp", "strcoll", "strxfrm", "strchr", "strrchr", "strstr", "strspn", "strcspn",
    "strpbrk", "strtok", "strtok_s", "strerror", "strerror_s", "strdup", "strndup", "_strdup", "_stricmp", "_strnicmp",
    "stricmp", "strnicmp", "strcasecmp", "strncasecmp", "_strlwr", "_strupr", "_strrev", "_strset",
    "wcslen", "wcsnlen", "wcscpy", "wcscpy_s", "wcsncpy", "wcsncpy_s", "wcscat", "wcscat_s", "wcsncat", "wcsncat_s",
    "wcscmp", "wcsncmp", "wcscoll", "wcsxfrm", "wcschr", "wcsrchr", "wcsstr", "wcsspn", "wcscspn", "wcspbrk", "wcstok",
    "wcstok_s", "wcsdup", "_wcsdup", "_wcsicmp", "_wcsnicmp", "_wcslwr", "_wcsupr", "_wcsrev",
    "wmemcmp", "wmemchr", "wmemcpy", "wmemmove", "wmemset", "wmemcpy_s", "wmemmove_s",
    // <stdio.h>
    "printf", "fprintf", "sprintf", "snprintf", "vprintf", "vfprintf", "vsprintf", "vsnprintf", "_snprintf",
    "_vsnprintf", "printf_s", "fprintf_s", "sprintf_s", "vsprintf_s", "_snprintf_s", "vsnprintf_s", "_vsnprintf_s",
    "wprintf", "fwprintf", "swprintf", "vwprintf", "vfwprintf", "vswprintf", "_snwprintf", "_vsnwprintf", "swprintf_s",
    "_snwprintf_s", "scanf", "fscanf", "sscanf", "vscanf", "vfscanf", "vsscanf", "scanf_s", "fscanf_s", "sscanf_s",
    "wscanf", "fwscanf", "swscanf", "swscanf_s", "fopen", "fopen_s", "_wfopen", "_wfopen_s", "freopen", "fclose",
    "fflush", "fread", "fwrite", "fgetc", "fgets", "fputc", "fputs", "getc", "getchar", "gets", "gets_s", "putc",
    "putchar", "puts", "ungetc", "fseek", "ftell", "_fseeki64", "_ftelli64", "rewind", "fgetpos", "fsetpos", "feof",
    "ferror", "clearerr", "perror", "remove", "rename", "_wremove", "_wrename", "tmpfile", "tmpnam", "setbuf", "setvbuf",
    "_fileno", "fgetwc", "fgetws", "fputwc", "fputws", "getwc", "putwc",
    // <stdlib.h>, <malloc.h>
    "malloc", "calloc", "realloc", "free", "_aligned_malloc", "_aligned_realloc", "_aligned_free", "_malloca", "_freea",
    "alloca", "_alloca", "_msize", "_expand", "atoi", "atol", "atoll", "atof", "_atoi64", "_wtoi", "_wtol", "_wtoi64",
    "_wtof", "strtol", "strtoul", "strtoll", "strtoull", "strtod", "strtof", "strtold", "_strtoi64", "_strtoui64",
    "wcstol", "wcstoul", "wcstoll", "wcstoull", "wcstod", "wcstof", "_wcstoi64", "_wcstoui64", "_itoa", "_itoa_s",
    "_ltoa", "_ltoa_s", "_ultoa", "_ultoa_s", "_i64toa", "_i64toa_s", "_ui64toa", "_ui64toa_s", "_itow", "_itow_s",
    "_ltow", "_ltow_s", "_ultow", "_ultow_s", "_i64tow", "_i64tow_s", "_ui64tow", "_ui64tow_s", "qsort", "qsort_s",
    "bsearch", "bsearch_s", "abs", "labs", "llabs", "_abs64", "div", "ldiv", "lldiv", "rand", "srand", "rand_s", "exit",
    "_exit", "_Exit", "abort", "atexit", "at_quick_exit", "quick_exit", "getenv", "getenv_s", "_wgetenv", "_wgetenv_s",
    "_putenv", "_wputenv", "system", "_wsystem", "mblen", "mbtowc", "wctomb", "mbstowcs", "wcstombs",
    // <ctype.h>, <wctype.h>
    "isalnum", "isalpha", "isblank", "iscntrl", "isdigit", "isgraph", "islower", "isprint", "ispunct", "isspace",
    "isupper", "isxdigit", "isascii", "__isascii", "tolower", "toupper", "_tolower", "_toupper", "iswalnum", "iswalpha",
    "iswblank", "iswcntrl", "iswdigit", "iswgraph", "iswlower", "iswprint", "iswpunct", "iswspace", "iswupper",
    "iswxdigit", "towlower", "towupper",
    // <math.h>
    "sqrt", "sqrtf", "pow", "powf", "exp", "expf", "exp2", "exp2f", "log", "logf", "log2", "log2f", "log10", "log10f",
    "sin", "sinf", "cos", "cosf", "tan", "tanf", "asin", "asinf", "acos", "acosf", "atan", "atanf", "atan2", "atan2f",
    "sinh", "sinhf", "cosh", "coshf", "tanh", "tanhf", "floor", "floorf", "ceil", "ceilf", "round", "roundf", "lround",
    "lroundf", "trunc", "truncf", "fmod", "fmodf", "modf", "modff", "frexp", "frexpf", "ldexp", "ldexpf", "fabs",
    "fabsf", "hypot", "hypotf", "cbrt", "cbrtf", "fmin", "fminf", "fmax", "fmaxf", "copysign", "copysignf", "rint",
    "rintf", "lrint", "lrintf", "nearbyint", "nearbyintf",
    // <time.h>
    "time", "_time64", "clock", "difftime", "mktime", "_mktime64", "gmtime", "gmtime_s", "_gmtime64", "localtime",
    "localtime_s", "_localtime64", "asctime", "asctime_s", "ctime", "ctime_s", "strftime", "wcsftime", "timespec_get",
    // the rest: <setjmp.h>, <signal.h>, <locale.h>, <assert.h>, <process.h>, <io.h>, <direct.h>, <conio.h>
    "setjmp", "longjmp", "signal", "raise", "setlocale", "_wsetlocale", "localeconv", "assert", "_wassert",
    "_beginthread", "_beginthreadex", "_endthread", "_endthreadex", "_open", "_wopen", "_read", "_write", "_close",
    "_lseek", "_access", "_waccess", "_getcwd", "_wgetcwd", "_chdir", "_wchdir", "_mkdir", "_wmkdir", "_getch",
    "_getwch", "_putch", "_kbhit", "_cprintf",
};

// C runtime headers (#include in src\platform\ and tools\ fails; <string.h> stays for memcpy, memset, memmove).
static const char *const CRT_HEADERS[] = {
    "stdio.h", "stdlib.h", "malloc.h", "math.h", "ctype.h", "wctype.h", "wchar.h", "time.h", "locale.h", "setjmp.h",
    "signal.h", "process.h", "io.h", "direct.h", "conio.h", "assert.h", "errno.h", "tchar.h", "search.h", "fcntl.h",
    "crtdbg.h", "inttypes.h", "uchar.h", "strsafe.h", // strsafe.h: its inline functions call the CRT
};

// The link check: the C runtime libraries' names in a linker map ("lib:object"), the functions that may come from them,
// and the objects whose data and local labels may (the data is no call, and its set differs between MSVC versions).
static const char *const CRT_LIBS[] = {
    "libcmt", "libcmtd", "libvcruntime", "libvcruntimed", "libucrt", "libucrtd", "msvcrt", "msvcrtd", "vcruntime",
    "ucrt", "oldnames", "legacy_stdio_definitions", "libcpmt", "libcpmtd", "libconcrt",
};
static const char *const LINK_FUNCTIONS[] = {
    "memcpy", "memmove",       // memcpy.obj: one function
    "memcpy_repmovs",          // memcpy.obj: its rep movsb path
    "memset", "memset_repstos", // memset.obj, and its rep stosb path
    "__chkstk", "_alloca_probe", // chkstk.obj: the stack probe the compiler calls for frames over 4 KB
    "__isa_available_init",    // cpu_disp.obj: the CPU flags memcpy / memset dispatch on (win32_entry calls it)
};
static const char *const LINK_OBJECTS[] = { "memcpy.obj", "memmove.obj", "memset.obj", "chkstk.obj", "cpu_disp.obj" };

#define CAPTURE_CAP 16384

static SourceFile files[CHECK_FILES_MAX];
static u32 file_count;
static MapRow rows[CHECK_ROWS_MAX];
static u32 row_count;
static u8 *text; // one file at a time, CHECK_TEXT_CAP
static char line_out[4096];
static u32 line_len;
static u32 problems, include_count, call_count;
static int capturing; // the self-test: problem lines go to `capture`, not to stdout
static char capture[CAPTURE_CAP];
static u32 capture_len;

// --- strings and output ---------------------------------------------------------------------------------------

static u32 str_len(const char *s) {
    u32 n = 0;
    while (s[n]) n++;
    return n;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return *a == *b;
}

static int starts_with(const char *s, const char *prefix) {
    while (*prefix && *s == *prefix) s++, prefix++;
    return *prefix == 0;
}

static int span_eq(const u8 *s, u32 n, const char *z) {
    u32 i = 0;
    for (; i < n; i++) if (!z[i] || (u8)z[i] != s[i]) return 0;
    return z[i] == 0;
}

static void copy_span(char *dst, u32 cap, const u8 *src, u32 n) {
    if (n >= cap) n = cap - 1;
    for (u32 i = 0; i < n; i++) dst[i] = (char)src[i];
    dst[n] = 0;
}

static int str_less(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (u8)*a < (u8)*b;
}

static void put_n(const char *s, u32 n) {
    for (u32 i = 0; i < n && line_len < sizeof(line_out) - 2; i++) line_out[line_len++] = s[i];
}

static void put(const char *s) { put_n(s, str_len(s)); }

static void put_u32(u32 v) {
    char digits[12];
    u32 n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n > 0) put_n(&digits[--n], 1);
}

static void end_line(void) {
    if (capturing) {
        for (u32 i = 0; i < line_len && capture_len < CAPTURE_CAP - 1; i++) capture[capture_len++] = line_out[i];
        if (capture_len < CAPTURE_CAP - 1) capture[capture_len++] = '\n';
        line_len = 0;
        return;
    }
    line_out[line_len++] = '\r';
    line_out[line_len++] = '\n';
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line_out, line_len, &written, NULL);
    line_len = 0;
}

// "path:line: " (line 0: "path: ") starts a problem's line.
static void problem(const char *path, u32 line) {
    problems++;
    put(path);
    put(":");
    if (line) {
        put_u32(line);
        put(":");
    }
    put(" ");
}

// --- files ----------------------------------------------------------------------------------------------------

static int widen(const char *utf8, WCHAR *out, int cap) {
    return MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out, cap) > 0;
}

// The whole file into `text`; 0 when it cannot be read or is too large.
static int read_file(const char *path, u32 *len) {
    WCHAR w[CHECK_PATH_CAP];
    if (!widen(path, w, CHECK_PATH_CAP)) return 0;
    HANDLE f = CreateFileW(w, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD size = 0;
    BOOL ok = ReadFile(f, text, CHECK_TEXT_CAP, &size, NULL);
    CloseHandle(f);
    if (!ok || size == CHECK_TEXT_CAP) return 0;
    *len = size;
    return 1;
}

static int is_file(const char *path) {
    WCHAR w[CHECK_PATH_CAP];
    if (!widen(path, w, CHECK_PATH_CAP)) return 0;
    DWORD a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

// Every file below dir (relative, '/' separated), junctions and links not followed.
static void walk(const char *dir) {
    char pattern[CHECK_PATH_CAP];
    u32 n = str_len(dir);
    if (n + 3 >= CHECK_PATH_CAP) return;
    for (u32 i = 0; i < n; i++) pattern[i] = dir[i];
    pattern[n] = '/';
    pattern[n + 1] = '*';
    pattern[n + 2] = 0;
    WCHAR w[CHECK_PATH_CAP];
    if (!widen(pattern, w, CHECK_PATH_CAP)) return;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char name[CHECK_PATH_CAP];
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, CHECK_PATH_CAP, NULL, NULL)) continue;
        if (str_eq(name, ".") || str_eq(name, "..")) continue;
        u32 m = str_len(name);
        if (n + 1 + m >= CHECK_PATH_CAP) continue;
        char child[CHECK_PATH_CAP];
        for (u32 i = 0; i < n; i++) child[i] = dir[i];
        child[n] = '/';
        for (u32 i = 0; i <= m; i++) child[n + 1 + i] = name[i];
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            walk(child);
        } else if (file_count < CHECK_FILES_MAX) {
            for (u32 i = 0; i <= n + 1 + m; i++) files[file_count].path[i] = child[i];
            file_count++;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// --- lines ----------------------------------------------------------------------------------------------------

// The line at *pos (without its LF and a CR before it); advances *pos past the LF. 0 at the end.
static int next_line(u32 len, u32 *pos, const u8 **start, u32 *n) {
    if (*pos >= len) return 0;
    u32 a = *pos, b = a;
    while (b < len && text[b] != '\n') b++;
    *pos = b < len ? b + 1 : b;
    if (b > a && text[b - 1] == '\r') b--;
    *start = text + a;
    *n = b - a;
    return 1;
}

// --- the module map -------------------------------------------------------------------------------------------

static MapRow *map_find(const char *path) {
    for (u32 i = 0; i < row_count; i++) if (str_eq(rows[i].path, path)) return &rows[i];
    return 0;
}

// The span of " | " in s[from, n), or n.
static u32 find_bar(const u8 *s, u32 from, u32 n) {
    for (u32 i = from; i + 3 <= n; i++) if (s[i] == ' ' && s[i + 1] == '|' && s[i + 2] == ' ') return i;
    return n;
}

static void read_map(void) {
    u32 len;
    if (!read_file(MAP_FILE, &len)) {
        problem(MAP_FILE, 0);
        put("cannot be read");
        end_line();
        return;
    }
    char folder[CHECK_PATH_CAP] = { 0 };
    int in_map = 0;
    u32 pos = 0, number = 0, n;
    const u8 *s;
    while (next_line(len, &pos, &s, &n)) {
        number++;
        if (n >= 3 && s[0] == '#' && s[1] == '#' && s[2] == ' ') {
            in_map = span_eq(s, n, "## Module map");
            folder[0] = 0;
            continue;
        }
        if (!in_map) continue;
        if (n >= 4 && s[0] == '#' && s[1] == '#' && s[2] == '#' && s[3] == ' ') {
            // "### `src/<folder>/`" or "### `tools/`"; any other heading ends the folder
            folder[0] = 0;
            if (n >= 8 && s[4] == '`' && s[n - 1] == '`' && s[n - 2] == '/') {
                copy_span(folder, CHECK_PATH_CAP, s + 5, n - 6);
                if (!starts_with(folder, "src/") && !starts_with(folder, "tools/")) folder[0] = 0;
            }
            continue;
        }
        if (!folder[0] || n < 3 || s[0] != '|' || s[1] != ' ' || s[2] != '`') continue;
        u32 a = find_bar(s, 1, n);
        if (a == n) continue;
        u32 b = find_bar(s, a + 3, n);
        if (b == n) continue; // not a row of three cells
        for (u32 i = 2; i < a; i++) {
            if (s[i] != '`') continue;
            u32 j = i + 1;
            while (j < a && s[j] != '`') j++;
            if (j == a) break;
            char path[CHECK_PATH_CAP];
            u32 fl = str_len(folder);
            for (u32 k = 0; k < fl; k++) path[k] = folder[k];
            copy_span(path + fl, CHECK_PATH_CAP - fl, s + i + 1, j - i - 1);
            MapRow *twin = map_find(path);
            if (twin) {
                problem(MAP_FILE, number);
                put("'");
                put(path);
                put("' listed twice (also line ");
                put_u32(twin->line);
                put(")");
                end_line();
            } else if (row_count < CHECK_ROWS_MAX) {
                MapRow *r = &rows[row_count++];
                for (u32 k = 0; k <= str_len(path); k++) r->path[k] = path[k];
                copy_span(r->desc, CHECK_DESC_CAP, s + a + 3, b - a - 3);
                r->line = number;
            }
            i = j;
        }
    }
}

// --- one file -------------------------------------------------------------------------------------------------

static int has_suffix(const char *s, const char *suffix) {
    u32 a = str_len(s), b = str_len(suffix);
    return a >= b && str_eq(s + a - b, suffix);
}

static void check_header(const char *path, u32 len, MapRow *row) {
    u32 pos = 0, n;
    const u8 *s;
    for (u32 k = 0; k < 8; k++) {
        if (!next_line(len, &pos, &s, &n)) n = 0, s = text;
        if (k == 0 && !span_eq(s, n, header_lines[0])) {
            problem(path, 1);
            put("does not start with ronin's header (CONTRIBUTING.md, \"New files\")");
            end_line();
            return;
        }
        if (k != 1) {
            if (!span_eq(s, n, header_lines[k])) {
                problem(path, k + 1);
                put("header line ");
                put_u32(k + 1);
                put(" differs from ronin's header (CONTRIBUTING.md, \"New files\")");
                end_line();
            }
            continue;
        }
        u32 p = str_len(DESC_PREFIX);
        if (n < p || !span_eq(s, p, DESC_PREFIX)) {
            problem(path, 2);
            put("the header's line 2 is not \"   ronin: <description>\"");
            end_line();
            continue;
        }
        const u8 *d = s + p;
        u32 dn = n - p, chars = 0;
        for (u32 i = 0; i < dn; i++) chars += (d[i] & 0xC0) != 0x80;
        if (dn == 0) {
            problem(path, 2);
            put("the description is empty");
            end_line();
        } else if (chars > CHECK_DESC_MAX) {
            problem(path, 2);
            put("the description is longer than 72 characters (");
            put_u32(chars);
            put(")");
            end_line();
        } else if (d[dn - 1] == ' ' || d[dn - 1] == '\t') {
            problem(path, 2);
            put("the description ends in a blank");
            end_line();
        }
        if (row && !span_eq(d, dn, row->desc)) {
            problem(path, 2);
            put("description '");
            put_n((const char *)d, dn);
            put("' differs from ");
            put(MAP_FILE);
            put(":");
            put_u32(row->line);
            put("'s '");
            put(row->desc);
            put("'");
            end_line();
        }
    }
}

// path's folder ("src/text/" of "src/text/a.c"; "src/" of "src/ronin.c"), its length.
static u32 folder_of(const char *path, u32 depth) {
    u32 slashes = 0, i = 0;
    for (; path[i]; i++) {
        if (path[i] == '/' && ++slashes == depth) return i + 1;
    }
    return 0;
}

// dir (with its '/') + name, "." and ".." resolved; 0 when it leaves the root.
static int resolve(const char *path, const u8 *name, u32 name_len, char *out) {
    u32 last = 0;
    for (u32 i = 0; path[i]; i++) if (path[i] == '/') last = i + 1;
    char joined[CHECK_PATH_CAP];
    if (last + name_len >= CHECK_PATH_CAP) return 0;
    for (u32 i = 0; i < last; i++) joined[i] = path[i];
    for (u32 i = 0; i < name_len; i++) joined[last + i] = name[i] == '\\' ? '/' : (char)name[i];
    u32 jn = last + name_len, on = 0;
    for (u32 i = 0; i < jn;) {
        u32 e = i;
        while (e < jn && joined[e] != '/') e++;
        u32 sn = e - i;
        if (sn == 0 || (sn == 1 && joined[i] == '.')) {
            // nothing
        } else if (sn == 2 && joined[i] == '.' && joined[i + 1] == '.') {
            if (on == 0) return 0;
            on--;
            while (on > 0 && out[on - 1] != '/') on--;
        } else {
            for (u32 k = i; k < e; k++) out[on++] = joined[k];
            if (e < jn) out[on++] = '/';
        }
        i = e + 1;
    }
    out[on] = 0;
    return on > 0;
}

static void check_includes(const char *path, u32 len) {
    if (!starts_with(path, "src/")) return; // tools\ are standalone Win32 programs, never part of the exe
    int platform = starts_with(path, "src/platform/");
    int low = starts_with(path, "src/base/") || starts_with(path, "src/text/");
    u32 pos = 0, number = 0, n;
    const u8 *s;
    while (next_line(len, &pos, &s, &n)) {
        number++;
        u32 i = 0;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i == n || s[i] != '#') continue;
        i++;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (n - i < 7 || !span_eq(s + i, 7, "include")) continue;
        i += 7;
        while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
        if (i == n || (s[i] != '<' && s[i] != '"')) continue;
        u8 close = (u8)(s[i] == '<' ? '>' : '"');
        u32 a = ++i;
        while (i < n && s[i] != close) i++;
        const u8 *name = s + a;
        u32 nn = i - a;
        include_count++;
        if (close == '>') {
            if (platform) continue;
            int allowed = 0;
            for (u32 k = 0; k < sizeof(SYSTEM_ALLOWED) / sizeof(SYSTEM_ALLOWED[0]); k++) allowed |= span_eq(name, nn, SYSTEM_ALLOWED[k]);
            if (!allowed) {
                problem(path, number);
                put("#include <");
                put_n((const char *)name, nn);
                put(">: only src/platform/ may include system headers other than <stdint.h>, <stddef.h>, <stdarg.h>, "
                    "<string.h>, <emmintrin.h> and <intrin.h>");
                end_line();
            }
            continue;
        }
        char target[CHECK_PATH_CAP];
        int found = resolve(path, name, nn, target) && is_file(target);
        if (!found) {
            int generated = 0;
            for (u32 k = 0; k < sizeof(GENERATED) / sizeof(GENERATED[0]); k++) generated |= span_eq(name, nn, GENERATED[k]);
            if (!generated) {
                problem(path, number);
                put("#include \"");
                put_n((const char *)name, nn);
                put("\": not a file of the repository nor a generated header (quad_vs.h, quad_ps.h, themes.h)");
                end_line();
            }
            continue;
        }
        if (low && (starts_with(target, "src/editor/") || starts_with(target, "src/project/") || starts_with(target, "src/app/"))) {
            problem(path, number);
            put("#include \"");
            put_n((const char *)name, nn);
            put("\": ");
            put_n(path, folder_of(path, 2));
            put(" may not include from ");
            put_n(target, folder_of(target, 2));
            end_line();
        }
    }
}

// --- the C runtime in C and C++ source (#36) ------------------------------------------------------------------

#define COUNT(a) ((u32)(sizeof(a) / sizeof((a)[0])))

static u8 lower(u8 c) { return c >= 'A' && c <= 'Z' ? (u8)(c + 32) : c; }

static int span_eq_ci(const u8 *s, u32 n, const char *z) {
    u32 i = 0;
    for (; i < n; i++) if (!z[i] || lower(s[i]) != (u8)z[i]) return 0;
    return z[i] == 0;
}

static int in_list(const u8 *s, u32 n, const char *const *list, u32 count) {
    for (u32 k = 0; k < count; k++) if (span_eq(s, n, list[k])) return 1;
    return 0;
}

static int in_list_ci(const u8 *s, u32 n, const char *const *list, u32 count) {
    for (u32 k = 0; k < count; k++) if (span_eq_ci(s, n, list[k])) return 1;
    return 0;
}

static int ident_char(u8 c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }

static void non_ascii(const char *path, u32 line) {
    problem(path, line);
    put("non-ASCII byte in a string or character literal (use \\u or \\x escapes: the compiler may read the file in "
        "another code page)");
    end_line();
}

// A string or character literal from its opening quote at text[i]; returns the index past its closing quote (or its
// line's end). *line counts the newlines escaped inside it.
static u32 skip_literal(const char *path, u32 len, u32 i, u32 *line) {
    u8 q = text[i++];
    int reported = 0;
    while (i < len && text[i] != q && text[i] != '\n') {
        if (text[i] >= 0x80 && !reported) non_ascii(path, *line), reported = 1;
        if (text[i] == '\\' && i + 1 < len) {
            if (text[i + 1] >= 0x80 && !reported) non_ascii(path, *line), reported = 1;
            if (text[i + 1] == '\n') (*line)++;
            if (text[i + 1] == '\r' && i + 2 < len && text[i + 2] == '\n') (*line)++, i++;
            i += 2;
            continue;
        }
        i++;
    }
    return i < len && text[i] == q ? i + 1 : i;
}

// C++'s raw string R"delim( ... )delim" from its quote at text[i].
static u32 skip_raw(const char *path, u32 len, u32 i, u32 *line) {
    u32 d = i + 1;
    while (d < len && text[d] != '(' && d - i <= 17) d++;
    if (d >= len || text[d] != '(') return skip_literal(path, len, i, line);
    const u8 *delim = text + i + 1;
    u32 dn = d - i - 1;
    int reported = 0;
    for (u32 k = d + 1; k < len; k++) {
        if (text[k] == '\n') (*line)++;
        if (text[k] >= 0x80 && !reported) non_ascii(path, *line), reported = 1;
        if (text[k] != ')' || k + 1 + dn >= len || text[k + 1 + dn] != '"') continue;
        u32 m = 0;
        while (m < dn && text[k + 1 + m] == delim[m]) m++;
        if (m == dn) return k + 2 + dn;
    }
    return len;
}

// Calls of CRT_NAMES, CRT headers (in src/platform/ and tools/) and non-ASCII literals in the file in `text`.
static void check_c(const char *path, u32 len) {
    int crt_headers = starts_with(path, "src/platform/") || starts_with(path, "tools/");
    u32 line = 1, i = 0;
    int line_start = 1, define = 0, directive = 0, macro_name = 0;
    u8 last = 0, last2 = 0; // the two code characters before the current one, blanks and comments skipped
    while (i < len) {
        u8 c = text[i];
        if (c == '\n') {
            line++;
            i++;
            line_start = 1;
            define = directive = macro_name = 0;
            continue;
        }
        if (c == '\\' && i + 1 < len && (text[i + 1] == '\n' || (text[i + 1] == '\r' && i + 2 < len && text[i + 2] == '\n'))) {
            i += text[i + 1] == '\n' ? 2 : 3; // a continued line: a directive goes on
            line++;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
            i++;
            continue;
        }
        if (c == '/' && i + 1 < len && text[i + 1] == '/') {
            while (i < len && text[i] != '\n') i++;
            continue;
        }
        if (c == '/' && i + 1 < len && text[i + 1] == '*') {
            i += 2;
            while (i < len && !(text[i] == '*' && i + 1 < len && text[i + 1] == '/')) {
                if (text[i] == '\n') line++;
                i++;
            }
            i = i < len ? i + 2 : len;
            continue;
        }
        if (directive) { // #if, #pragma, #error, #include ...: nothing in them is a call
            i++;
            continue;
        }
        if (c == '#' && line_start) {
            line_start = 0;
            u32 a = i + 1;
            while (a < len && (text[a] == ' ' || text[a] == '\t')) a++;
            u32 b = a;
            while (b < len && ident_char(text[b])) b++;
            i = b;
            last = last2 = 0;
            if (span_eq(text + a, b - a, "define")) {
                define = macro_name = 1;
                continue;
            }
            directive = 1;
            if (!crt_headers || !span_eq(text + a, b - a, "include")) continue;
            u32 p = b;
            while (p < len && (text[p] == ' ' || text[p] == '\t')) p++;
            if (p >= len || text[p] != '<') continue;
            u32 q = ++p;
            while (q < len && text[q] != '>' && text[q] != '\n') q++;
            if (in_list(text + p, q - p, CRT_HEADERS, COUNT(CRT_HEADERS))) {
                problem(path, line);
                put("#include <");
                put_n((const char *)text + p, q - p);
                put(">: a C runtime header (of the C runtime only <string.h>, <stdint.h>, <stddef.h> and <stdarg.h>)");
                end_line();
            }
            continue;
        }
        line_start = 0;
        if (c == '"' || c == '\'') {
            i = skip_literal(path, len, i, &line);
            last2 = last, last = '"';
            continue;
        }
        if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < len && text[i + 1] >= '0' && text[i + 1] <= '9')) {
            // a number, with C++'s digit separators: the ' of 1'000 starts no character literal
            for (i++; i < len; i++) {
                u8 d = text[i];
                if (ident_char(d) || d == '.' || (d == '\'' && i + 1 < len && ident_char(text[i + 1]))) continue;
                if ((d == '+' || d == '-') && (lower(text[i - 1]) == 'e' || lower(text[i - 1]) == 'p')) continue;
                break;
            }
            last2 = last, last = '0';
            continue;
        }
        if (ident_char(c)) {
            u32 a = i;
            while (i < len && ident_char(text[i])) i++;
            u32 n = i - a;
            const u8 *w = text + a;
            if (i < len && text[i] == '"' && (span_eq(w, n, "R") || span_eq(w, n, "LR") || span_eq(w, n, "uR") ||
                                              span_eq(w, n, "UR") || span_eq(w, n, "u8R"))) {
                i = skip_raw(path, len, i, &line);
                last2 = last, last = '"';
                continue;
            }
            if (i < len && (text[i] == '"' || text[i] == '\'') &&
                (span_eq(w, n, "L") || span_eq(w, n, "u") || span_eq(w, n, "U") || span_eq(w, n, "u8"))) {
                continue; // a literal's prefix: the literal is next
            }
            int member = last == '.' || (last == '>' && last2 == '-') || (last == ':' && last2 == ':');
            last2 = last, last = 'a';
            if (macro_name) { // the macro's own name: a definition, not a use
                macro_name = 0;
                continue;
            }
            u32 p = i;
            while (p < len && (text[p] == ' ' || text[p] == '\t')) p++;
            int call = p < len && text[p] == '(';
            call_count += (u32)call;
            if ((call || define) && !member && in_list(w, n, CRT_NAMES, COUNT(CRT_NAMES))) {
                problem(path, line);
                put("C runtime function '");
                put_n((const char *)w, n);
                put("' (only memcpy, memset and memmove are allowed)");
                end_line();
            }
            continue;
        }
        last2 = last, last = c;
        i++;
    }
}

static void check_file(const char *path) {
    int in_src = starts_with(path, "src/");
    if (in_src && !has_suffix(path, ".c") && !has_suffix(path, ".h") && !has_suffix(path, ".cpp") && !has_suffix(path, ".hlsl")) {
        problem(path, 0);
        put("not a source file (.c, .h, .cpp, .hlsl): src/ holds only those");
        end_line();
        return;
    }
    MapRow *row = map_find(path);
    if (row) {
        row->used = 1;
    } else {
        problem(path, 0);
        put("not in ");
        put(MAP_FILE);
        put("'s module map (a row under ### `");
        put_n(path, folder_of(path, starts_with(path, "src/") && folder_of(path, 2) ? 2 : 1));
        put("`)");
        end_line();
    }
    u32 len;
    if (!read_file(path, &len)) {
        problem(path, 0);
        put("cannot be read");
        end_line();
        return;
    }
    check_header(path, len, row);
    check_includes(path, len);
    if (!has_suffix(path, ".hlsl")) check_c(path, len);
}

// --- the link check (#36) -------------------------------------------------------------------------------------

// A linker map's symbol lines ("0001:00053b50  memcpy  0000000140054b50 f  libvcruntime:memcpy.obj", in "Publics by
// Value" and "Static symbols"): each from a C runtime library must be an allowed function or data of an allowed object.
// An object is reported once, at its first symbol that fails (the functions come first, its local labels after).
static u32 check_map(const char *path, u32 len) {
    u32 pos = 0, number = 0, n, crt = 0, seen_count = 0;
    char seen[32][CHECK_PATH_CAP];
    int publics = 0;
    const u8 *s;
    while (next_line(len, &pos, &s, &n)) {
        number++;
        for (u32 i = 0; i + 16 <= n && !publics; i++) publics = span_eq(s + i, 16, "Publics by Value");
        const u8 *tok[8];
        u32 tn[8], k = 0;
        for (u32 i = 0; i < n && k < 8;) {
            while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
            if (i >= n) break;
            u32 a = i;
            while (i < n && s[i] != ' ' && s[i] != '\t') i++;
            tok[k] = s + a;
            tn[k++] = i - a;
        }
        if (k < 4 || tn[0] != 13 || tok[0][4] != ':') continue;
        const u8 *obj = tok[k - 1];
        u32 on = tn[k - 1], colon = 0;
        while (colon < on && obj[colon] != ':') colon++;
        if (colon == on || !in_list_ci(obj, colon, CRT_LIBS, COUNT(CRT_LIBS))) continue;
        crt++;
        int function = 0;
        for (u32 j = 3; j + 1 < k; j++) function |= span_eq(tok[j], tn[j], "f");
        if (function ? in_list(tok[1], tn[1], LINK_FUNCTIONS, COUNT(LINK_FUNCTIONS))
                     : in_list_ci(obj + colon + 1, on - colon - 1, LINK_OBJECTS, COUNT(LINK_OBJECTS))) {
            continue;
        }
        int reported = 0;
        for (u32 j = 0; j < seen_count && !reported; j++) reported = span_eq(obj, on, seen[j]);
        if (reported) continue;
        if (seen_count < 32) copy_span(seen[seen_count++], CHECK_PATH_CAP, obj, on);
        problem(path, number);
        put(function ? "C runtime function '" : "C runtime symbol '");
        put_n((const char *)tok[1], tn[1]);
        put("' linked from ");
        put_n((const char *)obj, on);
        put(" (only memcpy, memset, memmove and the compiler helpers __chkstk and __isa_available_init are allowed)");
        end_line();
    }
    if (!publics) {
        problem(path, 0);
        put("not a linker map (no \"Publics by Value\")");
        end_line();
    }
    return crt;
}

static int link_check(int count, char **maps) {
    u32 crt = 0;
    for (int m = 0; m < count; m++) {
        u32 len;
        if (!read_file(maps[m], &len)) {
            problem(maps[m], 0);
            put("cannot be read (build.bat release and bench write the maps)");
            end_line();
            continue;
        }
        crt += check_map(maps[m], len);
    }
    if (problems) {
        put("check_source: link: FAIL: ");
        put_u32(problems);
        put(problems == 1 ? " problem" : " problems");
        end_line();
        return 1;
    }
    put("check_source: link: PASS: ");
    put_u32((u32)count);
    put(count == 1 ? " map, " : " maps, ");
    put_u32(crt);
    put(" C runtime symbols, each a function of the allowed list or data of its objects");
    end_line();
    return 0;
}

// --- the self-test (#36) --------------------------------------------------------------------------------------

typedef struct SelfCase {
    const char *path;
    const char *text;
    const char *expect[4]; // each problem line's start, in order
} SelfCase;

#define CRT_FN "C runtime function '"

static const SelfCase SELF_C[] = {
    { "src/x/a.c", "int f(void) { return memcmp(a, b, 4); }", { "src/x/a.c:1: " CRT_FN "memcmp'" } },
    { "src/x/a.c", "// memcmp(a, b, 4)\n/* strlen(s)\n   free(p) */ int x;", { 0 } },
    { "src/x/a.c", "const char *s = \"printf(\\\"%d\\\")\"; char c = 'm'; const char *t = \"memcmp(\";", { 0 } },
    { "src/x/a.c", "b32 e = mem_equal(a, b, 4) && str8_equal(x, y);\nvoid git_job_free(GitJob *j);\nstr_len(s);", { 0 } },
    { "src/x/a.cpp", "p->free(p); x.time(); ns::abs(1); f->clock (2);", { 0 } },
    { "src/x/a.c", "memcpy(a, b, 1); memset(a, 0, 1); memmove(a, b, 1);", { 0 } },
    { "src/x/a.c", "int y;\n#define CMP memcmp\n", { "src/x/a.c:2: " CRT_FN "memcmp'" } },
    { "src/x/a.c", "#define F(x) strlen(x)\n", { "src/x/a.c:1: " CRT_FN "strlen'" } },
    { "src/x/a.c", "#define abs(x) ((x) < 0 ? -(x) : (x))\nint y = time;", { 0 } },
    { "src/x/a.c", "#define LONG(a) \\\n    strlen(a)\nint z;\n", { "src/x/a.c:2: " CRT_FN "strlen'" } },
    { "src/x/a.c", "#if defined(strlen)\n#endif\n#pragma intrinsic(memcmp)\n#error don't (free)\n", { 0 } },
    { "src/platform/a.c", "#include <string.h>\n#include <windows.h>\n#include <stdio.h>\n",
      { "src/platform/a.c:3: #include <stdio.h>: a C runtime header" } },
    { "tools/t.c", "  #  include <math.h>\n", { "tools/t.c:1: #include <math.h>: a C runtime header" } },
    { "src/x/a.c", "#include <stdlib.h>\n", { 0 } }, // the include rule refuses it outside src/platform/
    { "src/x/a.c", "const char *s = \"caf\xC3\xA9\";\nchar c = '\xC3';", { "src/x/a.c:1: non-ASCII byte", "src/x/a.c:2: non-ASCII byte" } },
    { "src/x/a.c", "// caf\xC3\xA9\n/* \xC3\xA9 */ const char *s = \"caf\\u00e9\";", { 0 } },
    { "src/x/a.cpp", "int n = 1'000; int m = memchr(a, 0, n) != 0;", { "src/x/a.cpp:1: " CRT_FN "memchr'" } },
    { "src/x/a.c", "if (x) free (p);\nint q = sqrt(2.0);", { "src/x/a.c:1: " CRT_FN "free'", "src/x/a.c:2: " CRT_FN "sqrt'" } },
    { "src/x/a.c", "s = \"a\\\"b memcmp(\"; t = memcmp(a, b, 1);", { "src/x/a.c:1: " CRT_FN "memcmp'" } },
    { "src/x/a.c", "x = L\"memcmp(\"; y = u8\"strlen(\"; z = U'a'; w = toupper(c);", { "src/x/a.c:1: " CRT_FN "toupper'" } },
    { "src/x/a.c", "/* line 1\n   line 2 */ abort();", { "src/x/a.c:2: " CRT_FN "abort'" } },
    { "src/x/a.cpp", "auto s = R\"x(memcmp( \" )x\"; qsort(a, 1, 1, f);", { "src/x/a.cpp:1: " CRT_FN "qsort'" } },
    { "src/x/a.c", "char c = '\\''; int y = isdigit(c);", { "src/x/a.c:1: " CRT_FN "isdigit'" } },
    { "src/x/a.c", "int a = memcmp_like(x); int b = my_strlen(s); int c = strlen_s(s);", { 0 } },
};

static const SelfCase SELF_MAP[] = {
    { "x.map",
      "  Address         Publics by Value              Rva+Base               Lib:Object\n\n"
      " 0001:00053ad0       __chkstk                   0000000140054ad0 f   libcmt:chkstk.obj\n"
      " 0001:00053b50       memcpy                     0000000140054b50 f   libvcruntime:memcpy.obj\n"
      " 0003:00000020       __favor                    000000014006e020     libcmt:cpu_disp.obj\n"
      " 0001:00000010       app_main                   0000000140001010 f   ronin.obj\n"
      " 0001:00000020       __imp_CreateFileW          0000000140001020     kernel32:KERNEL32.dll\n",
      { 0 } },
    { "x.map",
      "  Address         Publics by Value              Rva+Base               Lib:Object\n"
      " 0001:00053b50       memset                     0000000140054b50 f   libvcruntime:memset.obj\n"
      " 0001:00054000       memcmp                     0000000140055000 f   libvcruntime:memcmp.obj\n"
      " 0003:00000000       __security_cookie          000000014006e000     libcmt:gs_cookie.obj\n"
      " 0001:00054010       __isa_available_x          0000000140055010 f   LIBCMT:cpu_disp.obj\n"
      " 0001:00054020       mcmp10                     0000000140055020     libvcruntime:memcmp.obj\n",
      { "x.map:3: C runtime function 'memcmp' linked from libvcruntime:memcmp.obj",
        "x.map:4: C runtime symbol '__security_cookie' linked from libcmt:gs_cookie.obj",
        "x.map:5: C runtime function '__isa_available_x'" } },
    { "x.map", "not a map\n", { "x.map: not a linker map" } },
};

// Each case's problem lines against its expected starts; returns the failures.
static u32 self_run(const char *kind, const SelfCase *cases, u32 count) {
    u32 failed = 0;
    for (u32 c = 0; c < count; c++) {
        const SelfCase *k = &cases[c];
        u32 len = str_len(k->text);
        for (u32 i = 0; i < len; i++) text[i] = (u8)k->text[i];
        capturing = 1;
        capture_len = 0;
        if (kind[0] == 'C') check_c(k->path, len);
        if (kind[0] == 'm') check_map(k->path, len);
        capturing = 0;
        u32 pos = 0, j = 0;
        int ok = 1;
        while (pos < capture_len) {
            u32 e = pos;
            while (e < capture_len && capture[e] != '\n') e++;
            const char *want = j < 4 ? k->expect[j] : 0;
            if (!want || e - pos < str_len(want) || !span_eq((const u8 *)capture + pos, str_len(want), want)) ok = 0;
            j++;
            pos = e + 1;
        }
        if (j < 4 && k->expect[j]) ok = 0;
        if (ok) continue;
        failed++;
        put("check_source: self-test: FAIL: ");
        put(kind);
        put(" case ");
        put_u32(c + 1);
        put(" (");
        put(k->path);
        put("): expected");
        end_line();
        for (u32 e = 0; e < 4 && k->expect[e]; e++) {
            put("  ");
            put(k->expect[e]);
            end_line();
        }
        put("got");
        end_line();
        for (pos = 0; pos < capture_len;) {
            u32 e = pos;
            while (e < capture_len && capture[e] != '\n') e++;
            put("  ");
            put_n(capture + pos, e - pos);
            end_line();
            pos = e + 1;
        }
    }
    return failed;
}

static int self_test(void) {
    u32 failed = 0, total = 0;
    failed += self_run("C", SELF_C, COUNT(SELF_C)), total += COUNT(SELF_C);
    failed += self_run("map", SELF_MAP, COUNT(SELF_MAP)), total += COUNT(SELF_MAP);
    put(failed ? "check_source: self-test: FAIL: " : "check_source: self-test: PASS: ");
    if (failed) {
        put_u32(failed);
        put(" of ");
    }
    put_u32(total);
    put(" cases");
    end_line();
    return failed ? 1 : 0;
}

int main(int argc, char **argv) {
    text = (u8 *)VirtualAlloc(NULL, CHECK_TEXT_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!text) return 2;
    if (argc == 2 && str_eq(argv[1], "--self-test")) return self_test();
    if (argc >= 3 && str_eq(argv[1], "--link")) return link_check(argc - 2, argv + 2);
    if (argc != 2) {
        put("usage: check_source <repository root> | --self-test");
        put(" | --link <linker map>...");
        end_line();
        return 2;
    }
    WCHAR root[CHECK_PATH_CAP];
    if (!MultiByteToWideChar(CP_ACP, 0, argv[1], -1, root, CHECK_PATH_CAP) || !SetCurrentDirectoryW(root)) {
        put("check_source: cannot enter the repository root");
        end_line();
        return 2;
    }
    read_map();
    walk("src");
    walk("tools");
    for (u32 i = 1; i < file_count; i++) { // name order, so the output is the same on every run
        SourceFile x = files[i];
        u32 j = i;
        while (j > 0 && str_less(x.path, files[j - 1].path)) {
            files[j] = files[j - 1];
            j--;
        }
        files[j] = x;
    }
    for (u32 i = 0; i < file_count; i++) check_file(files[i].path);
    for (u32 i = 0; i < row_count; i++) {
        if (rows[i].used) continue;
        problem(MAP_FILE, rows[i].line);
        put("'");
        put(rows[i].path);
        put("' in the module map: no such file");
        end_line();
    }
    if (problems) {
        put("check_source: FAIL: ");
        put_u32(problems);
        put(problems == 1 ? " problem" : " problems");
        end_line();
        return 1;
    }
    put("check_source: PASS: ");
    put_u32(file_count);
    put(" files with ronin's header and their module map row, ");
    put_u32(include_count);
    put(" includes, ");
    put_u32(row_count);
    put(" map rows, ");
    put_u32(call_count);
    put(" calls without the C runtime");
    end_line();
    return 0;
}
