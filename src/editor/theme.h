/* ======================================================================
   ronin: the built-in themes and theme files: lookup, list, read

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// color themes (Phase 21). A theme is a file in the config format holding only a [theme] block (name,
// source, commit, license, author) and a [colors] section. The built-in ones are themes\*.conf of the repository,
// embedded by tools\make_themes.c into build\gen\themes.h; ronin is the default and the base every other theme lies
// on. A theme named N is <config dir>\themes\N.conf, else <exe dir>\themes\N.conf, else the built-in N: a file
// replaces the built-in theme of its name. config_load lays the chosen theme under ronin.conf's [colors]. Plain C,
// headless.

#ifndef THEME_H
#define THEME_H

#define THEME_NAME_CAP 64 // bytes of a theme's name

// The folders whose themes\ subfolder holds theme files: ronin.conf's and the executable's (empty when it is the
// same folder, a portable install, or when there is none).
typedef struct ThemeDirs {
    String8 config_dir, exe_dir;
} ThemeDirs;

typedef enum ThemeWhere {
    THEME_NONE,       // no theme of that name
    THEME_BUILTIN,
    THEME_CONFIG_DIR, // <config dir>\themes\<name>.conf
    THEME_EXE_DIR,    // <exe dir>\themes\<name>.conf
} ThemeWhere;

// Where a theme was found, and the size and write time of its file.
typedef struct ThemeLocation {
    ThemeWhere where;
    String8 path;     // the file (empty for a built-in theme)
    OsFileInfo info;
} ThemeLocation;

// What config_load is to do with themes, and what it did.
typedef struct ThemeLoad {
    ThemeDirs dirs;
    String8 override;      // set-theme's choice for the session (empty: ronin.conf's theme)
    String8 override_file; // ronin.conf's theme when it was made: ronin.conf naming another one ends it
    b32 override_dropped;  // out: it ended
    String8 name;          // out: the theme asked for (loaded, or not found: then ronin)
    ThemeLocation loc;     // out: where that name was found
    OsFileStatus status;   // out: reading it (OS_FILE_OK for a built-in one)
    u64 hash;              // out: a hash of the theme file's bytes as read (0 for a built-in theme)
} ThemeLoad;

i32     theme_builtin_count(void);
String8 theme_builtin_name(i32 i);    // in name order
String8 theme_builtin(String8 name);  // the embedded text of a built-in theme (ASCII case-insensitive), empty if none
b32     theme_name_valid(String8 name); // letters, digits, '_', '-', '.'; 1 to THEME_NAME_CAP bytes
b32     theme_name_equal(String8 a, String8 b); // ASCII case-insensitive
String8 theme_folder(Arena *arena, String8 dir); // dir\themes

// Finds the theme and reads it: its text in the arena (a built-in one's is the embedded text). The status of the
// read; OS_FILE_NOT_FOUND with loc->where THEME_NONE when no theme has that name.
OsFileStatus theme_read(Arena *arena, ThemeDirs *dirs, String8 name, ThemeLocation *loc, String8 *text);
// The same lookup without reading: where the theme is and its file's size and write time.
void theme_locate(Arena *arena, ThemeDirs *dirs, String8 name, ThemeLocation *loc);

#define THEME_LIST_CAP 256
// Every theme there is (set-theme's candidates): the built-in ones and the <name>.conf files of both themes\
// folders, lowercase, each once, in name order; where[i] is where the name is found (a file before the built-in
// theme it replaces). `replaces[i]`: a file whose name is also a built-in theme's. Returns the count.
i32 theme_list(Arena *arena, ThemeDirs *dirs, String8 *names, ThemeWhere *where, b32 *replaces, i32 cap);

#endif // THEME_H
