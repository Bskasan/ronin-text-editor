// theme.h — color themes (Phase 21). A theme is a file in the config format holding only a [theme] block (name,
// source, commit, license, author) and a [colors] section. The built-in ones are themes\*.conf of the repository,
// embedded by tools\make_themes.c into build\gen\themes.h; ronin is the default and the base every other theme lies
// on. Plain C, headless.

#ifndef THEME_H
#define THEME_H

#define THEME_NAME_CAP 64 // bytes of a theme's name

i32     theme_builtin_count(void);
String8 theme_builtin_name(i32 i);    // in name order
String8 theme_builtin(String8 name);  // the embedded text of a built-in theme (ASCII case-insensitive), empty if none
b32     theme_name_valid(String8 name); // letters, digits, '_', '-', '.'; 1 to THEME_NAME_CAP bytes
b32     theme_name_equal(String8 a, String8 b); // ASCII case-insensitive

#endif // THEME_H
