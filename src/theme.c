// theme.c — see theme.h.

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

b32 theme_name_valid(String8 name) {
    if (name.len < 1 || name.len > THEME_NAME_CAP) return 0;
    for (i64 i = 0; i < name.len; i++) {
        u8 c = name.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}
