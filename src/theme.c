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

b32 theme_name_valid(String8 name) {
    if (name.len < 1 || name.len > THEME_NAME_CAP) return 0;
    for (i64 i = 0; i < name.len; i++) {
        u8 c = name.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
    }
    return 1;
}
