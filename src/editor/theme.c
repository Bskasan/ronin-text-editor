/* ======================================================================
   ronin: the built-in themes and theme files: lookup, list, read

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// see theme.h.

#include "themes.h" // generated: theme_builtins[], THEME_BUILTIN_COUNT

i32 theme_builtin_count(void) {
    return THEME_BUILTIN_COUNT;
}

String8 theme_builtin_name(i32 i) {
    return str8_cstr(theme_builtins[i].name);
}

b32 theme_name_equal(String8 a, String8 b) {
    if (a.len != b.len) return 0;
    for (i64 i = 0; i < a.len; i++) {
        u8 x = a.data[i], y = b.data[i];
        if (x >= 'A' && x <= 'Z') x += 32;
        if (y >= 'A' && y <= 'Z') y += 32;
        if (x != y) return 0;
    }
    return 1;
}

String8 theme_builtin(String8 name) {
    for (i32 i = 0; i < THEME_BUILTIN_COUNT; i++) {
        if (theme_name_equal(name, str8_cstr(theme_builtins[i].name))) return str8((u8 *)theme_builtins[i].text, theme_builtins[i].len);
    }
    return str8(NULL, 0);
}

String8 theme_folder(Arena *arena, String8 dir) {
    return str8_fmt(arena, "%S\\themes", dir);
}

// <dir>\themes\<name>.conf for the two folders, in their order.
static String8 theme_file_path(Arena *arena, ThemeDirs *dirs, i32 which, String8 name) {
    String8 dir = which == 0 ? dirs->config_dir : dirs->exe_dir;
    return dir.len ? str8_fmt(arena, "%S\\themes\\%S.conf", dir, name) : str8(NULL, 0);
}

void theme_locate(Arena *arena, ThemeDirs *dirs, String8 name, ThemeLocation *loc) {
    memset(loc, 0, sizeof(*loc));
    for (i32 which = 0; which < 2; which++) {
        String8 path = theme_file_path(arena, dirs, which, name);
        if (path.len && os_file_info(path, &loc->info) == OS_FILE_OK && !loc->info.is_dir) {
            loc->where = which == 0 ? THEME_CONFIG_DIR : THEME_EXE_DIR;
            loc->path = path;
            return;
        }
    }
    memset(&loc->info, 0, sizeof(loc->info));
    loc->where = theme_builtin(name).len ? THEME_BUILTIN : THEME_NONE;
}

OsFileStatus theme_read(Arena *arena, ThemeDirs *dirs, String8 name, ThemeLocation *loc, String8 *text) {
    memset(loc, 0, sizeof(*loc));
    *text = str8(NULL, 0);
    for (i32 which = 0; which < 2; which++) {
        String8 path = theme_file_path(arena, dirs, which, name);
        if (!path.len) continue;
        OsFileStatus status = config_read_file(arena, path, text, &loc->info);
        if (status == OS_FILE_NOT_FOUND) continue;
        loc->where = which == 0 ? THEME_CONFIG_DIR : THEME_EXE_DIR; // found: read, or not readable now
        loc->path = path;
        return status;
    }
    memset(&loc->info, 0, sizeof(loc->info));
    *text = theme_builtin(name);
    loc->where = text->len ? THEME_BUILTIN : THEME_NONE;
    return text->len ? OS_FILE_OK : OS_FILE_NOT_FOUND;
}

i32 theme_list(Arena *arena, ThemeDirs *dirs, String8 *names, ThemeWhere *where, b32 *replaces, i32 cap) {
    i32 n = 0;
    for (i32 i = 0; i < THEME_BUILTIN_COUNT && n < cap; i++, n++) {
        names[n] = str8_cstr(theme_builtins[i].name);
        where[n] = THEME_BUILTIN;
        replaces[n] = 0;
    }
    for (i32 which = 0; which < 2; which++) {
        String8 dir = which == 0 ? dirs->config_dir : dirs->exe_dir;
        OsDirEntry *entries;
        i64 count;
        if (!dir.len || os_list_dir(arena, theme_folder(arena, dir), &entries, &count) != OS_FILE_OK) continue;
        for (i64 e = 0; e < count; e++) {
            String8 file = entries[e].name;
            if (entries[e].is_dir || file.len < 6 || !theme_name_equal(str8(file.data + file.len - 5, 5), STR8_LIT(".conf"))) continue;
            String8 name = str8_copy(arena, str8(file.data, file.len - 5));
            if (!theme_name_valid(name)) continue;
            for (i64 k = 0; k < name.len; k++) if (name.data[k] >= 'A' && name.data[k] <= 'Z') name.data[k] += 32;
            i32 at = 0;
            while (at < n && !str8_equal(names[at], name)) at++;
            if (at < n) { // a built-in theme it replaces, or the same name in the first folder (which wins)
                if (where[at] == THEME_BUILTIN) {
                    where[at] = which == 0 ? THEME_CONFIG_DIR : THEME_EXE_DIR;
                    replaces[at] = 1;
                }
                continue;
            }
            if (n == cap) break;
            names[n] = name;
            where[n] = which == 0 ? THEME_CONFIG_DIR : THEME_EXE_DIR;
            replaces[n++] = 0;
        }
    }
    for (i32 i = 1; i < n; i++) { // name order (insertion sort: a few names)
        for (i32 j = i; j > 0; j--) {
            String8 a = names[j - 1], b = names[j];
            i64 k = 0;
            while (k < a.len && k < b.len && a.data[k] == b.data[k]) k++;
            b32 in_order = k < b.len && (k == a.len || a.data[k] < b.data[k]); // a < b
            if (in_order) break;
            names[j - 1] = b;
            names[j] = a;
            ThemeWhere w = where[j - 1];
            where[j - 1] = where[j];
            where[j] = w;
            b32 r = replaces[j - 1];
            replaces[j - 1] = replaces[j];
            replaces[j] = r;
        }
    }
    return n;
}

b32 theme_name_valid(String8 name) {
    if (name.len < 1 || name.len > THEME_NAME_CAP) return 0;
    for (i64 i = 0; i < name.len; i++) {
        u8 c = name.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}
