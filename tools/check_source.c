/* ======================================================================
   ronin: build check: headers, module map and includes of every source file

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

static SourceFile files[CHECK_FILES_MAX];
static u32 file_count;
static MapRow rows[CHECK_ROWS_MAX];
static u32 row_count;
static u8 *text; // one file at a time, CHECK_TEXT_CAP
static char line_out[4096];
static u32 line_len;
static u32 problems, include_count;

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
}

int main(int argc, char **argv) {
    if (argc != 2) {
        put("usage: check_source <repository root>");
        end_line();
        return 2;
    }
    WCHAR root[CHECK_PATH_CAP];
    if (!MultiByteToWideChar(CP_ACP, 0, argv[1], -1, root, CHECK_PATH_CAP) || !SetCurrentDirectoryW(root)) {
        put("check_source: cannot enter the repository root");
        end_line();
        return 2;
    }
    text = (u8 *)VirtualAlloc(NULL, CHECK_TEXT_CAP, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!text) return 2;
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
    put(" map rows");
    end_line();
    return 0;
}
