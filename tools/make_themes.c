// make_themes.c: embeds the built-in themes (build-time tool, not part of the editor; build.bat compiles and runs it
// on every build: make_themes <themes dir> <out.h>).
//
// Every <name>.conf of the folder becomes one C string in the header, in name order, with its comment lines and
// blank lines left out (the files keep their notes, the exe does not pay for them) and the blanks around each '='
// dropped (the config parser trims them anyway). ronin.conf must be there: it is the base every theme lies on. The
// build fails when the embedded texts reach THEMES_MAX_BYTES together (the Phase 21 budget), when a name is not a
// theme name, or when a file cannot be read. One literal per line keeps every literal far below MSVC's limit.
// No C runtime calls: the files are read and written with Win32.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef unsigned char u8;
typedef unsigned int u32;

#define THEMES_MAX 32
#define THEMES_MAX_BYTES 8192
#define THEMES_NAME_CAP 64
#define THEMES_FILE_CAP (64 * 1024)

typedef struct Theme {
    char name[THEMES_NAME_CAP + 1];
    u8 text[THEMES_FILE_CAP]; // the embedded text: the lines kept, each ending in '\n'
    u32 len;
} Theme;

static Theme themes[THEMES_MAX];
static u32 theme_count;
static u8 file[THEMES_FILE_CAP];
static u8 out[1 << 20];
static u32 out_len;

static void put(const char *s) {
    while (*s && out_len < sizeof(out)) out[out_len++] = (u8)*s++;
}

static void put_u32(u32 v) {
    char digits[12];
    int n = 0;
    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n > 0 && out_len < sizeof(out)) out[out_len++] = (u8)digits[--n];
}

static void say(const char *s) {
    DWORD written;
    u32 n = 0;
    while (s[n]) n++;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s, n, &written, NULL);
}

static int name_less(const char *a, const char *b) {
    while (*a && *a == *b) a++, b++;
    return (u8)*a < (u8)*b;
}

static int is_blank(u8 c) { return c == ' ' || c == '\t' || c == '\r'; }

// "<name>.conf" with a theme name: letters, digits, '_', '-', '.', at most THEMES_NAME_CAP bytes.
static int theme_name(const char *file_name, char *name) {
    u32 n = 0;
    while (file_name[n]) n++;
    if (n < 6 || n - 5 > THEMES_NAME_CAP) return 0;
    const char *ext = file_name + n - 5;
    if (ext[0] != '.' || (ext[1] | 0x20) != 'c' || (ext[2] | 0x20) != 'o' || (ext[3] | 0x20) != 'n' || (ext[4] | 0x20) != 'f') return 0;
    for (u32 i = 0; i < n - 5; i++) {
        char c = file_name[i];
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
        if (!ok) return 0;
        name[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c; // names compare case-insensitively: stored lowercase
    }
    name[n - 5] = 0;
    return 1;
}

// The lines that are neither blank nor comments, trimmed, "a = b" written "a=b".
static int embed(Theme *t, const char *path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD size = 0;
    BOOL ok = ReadFile(f, file, sizeof(file), &size, NULL);
    CloseHandle(f);
    if (!ok || size == sizeof(file)) return 0;
    u32 pos = 0;
    if (size >= 3 && file[0] == 0xEF && file[1] == 0xBB && file[2] == 0xBF) pos = 3; // a UTF-8 BOM
    while (pos < size) {
        u32 end = pos;
        while (end < size && file[end] != '\n') end++;
        u32 a = pos, b = end;
        pos = end + 1;
        while (a < b && is_blank(file[a])) a++;
        while (b > a && is_blank(file[b - 1])) b--;
        if (a == b || file[a] == '#') continue;
        u32 eq = a;
        while (eq < b && file[eq] != '=') eq++;
        if (eq == b) { // a [section] line (or a bad one, which the parser reports as the file would)
            for (u32 i = a; i < b; i++) t->text[t->len++] = file[i];
        } else {
            u32 name_end = eq, value = eq + 1;
            while (name_end > a && is_blank(file[name_end - 1])) name_end--;
            while (value < b && is_blank(file[value])) value++;
            for (u32 i = a; i < name_end; i++) t->text[t->len++] = file[i];
            t->text[t->len++] = '=';
            for (u32 i = value; i < b; i++) t->text[t->len++] = file[i];
        }
        t->text[t->len++] = '\n';
    }
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        say("usage: make_themes <themes dir> <out.h>\n");
        return 2;
    }
    char pattern[MAX_PATH];
    u32 n = 0;
    for (const char *s = argv[1]; *s && n < MAX_PATH - 8; s++) pattern[n++] = *s;
    u32 dir_len = n;
    const char *glob = "\\*.conf";
    for (u32 i = 0; glob[i]; i++) pattern[n++] = glob[i];
    pattern[n] = 0;
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE) {
        say("make_themes: no themes\\*.conf\n");
        return 1;
    }
    int failed = 0;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (theme_count == THEMES_MAX) {
            say("make_themes: too many themes\n");
            failed = 1;
            break;
        }
        Theme *t = &themes[theme_count];
        if (!theme_name(fd.cFileName, t->name)) {
            say("make_themes: not a theme name: ");
            say(fd.cFileName);
            say("\n");
            failed = 1;
            continue;
        }
        char path[MAX_PATH];
        u32 p = 0;
        for (u32 i = 0; i < dir_len; i++) path[p++] = pattern[i];
        path[p++] = '\\';
        for (const char *s = fd.cFileName; *s && p < MAX_PATH - 1; s++) path[p++] = *s;
        path[p] = 0;
        if (!embed(t, path)) {
            say("make_themes: cannot read ");
            say(path);
            say("\n");
            failed = 1;
            continue;
        }
        theme_count++;
    } while (FindNextFileA(find, &fd));
    FindClose(find);
    if (failed) return 1;

    // Name order (insertion sort: a handful of names).
    for (u32 i = 1; i < theme_count; i++) {
        for (u32 j = i; j > 0 && name_less(themes[j].name, themes[j - 1].name); j--) {
            Theme tmp = themes[j];
            themes[j] = themes[j - 1];
            themes[j - 1] = tmp;
        }
    }
    u32 total = 0, ronin = 0;
    for (u32 i = 0; i < theme_count; i++) {
        total += themes[i].len;
        const char *r = "ronin";
        u32 k = 0;
        while (r[k] && themes[i].name[k] == r[k]) k++;
        if (!r[k] && !themes[i].name[k]) ronin = 1;
    }
    if (!ronin) {
        say("make_themes: themes\\ronin.conf is missing (the base of every theme)\n");
        return 1;
    }
    if (total >= THEMES_MAX_BYTES) {
        say("make_themes: the embedded themes take 8192 bytes or more (the budget)\n");
        return 1;
    }

    put("// Generated by tools\\make_themes.c from themes\\*.conf on every build; never edited, never committed.\n\n");
    put("#define THEME_BUILTIN_COUNT ");
    put_u32(theme_count);
    put("\n#define THEME_BUILTIN_BYTES ");
    put_u32(total);
    put("\n\n");
    for (u32 i = 0; i < theme_count; i++) {
        Theme *t = &themes[i];
        put("static const char theme_text_");
        put_u32(i);
        put("[] =\n");
        u32 at = 0;
        while (at < t->len) {
            put("    \"");
            while (at < t->len) {
                u8 c = t->text[at++];
                if (c == '\n') {
                    put("\\n");
                    break;
                }
                if (c == '"' || c == '\\') put("\\");
                if (out_len < sizeof(out)) out[out_len++] = c;
            }
            put("\"\n");
        }
        put("    ;\n");
    }
    put("\nstatic const struct { const char *name; const char *text; int len; } theme_builtins[THEME_BUILTIN_COUNT] = {\n");
    for (u32 i = 0; i < theme_count; i++) {
        put("    { \"");
        put(themes[i].name);
        put("\", theme_text_");
        put_u32(i);
        put(", ");
        put_u32(themes[i].len);
        put(" },\n");
    }
    put("};\n");
    if (out_len == sizeof(out)) return 1;

    HANDLE f = CreateFileA(argv[2], GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 1;
    DWORD written = 0;
    BOOL ok = WriteFile(f, out, out_len, &written, NULL) && written == out_len;
    CloseHandle(f);
    if (!ok) return 1;
    say(argv[2]);
    say(": ");
    out_len = 0;
    put_u32(theme_count);
    put(" themes, ");
    put_u32(total);
    put(" bytes embedded\n");
    out[out_len] = 0;
    say((const char *)out);
    return 0;
}
