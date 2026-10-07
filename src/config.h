// config.h — one plain-text config file holds settings, colors and key bindings. The built-in
// defaults (config_default.h) are the same format, parsed by the same code at startup; the user's
// file is applied on top. An error never stops a load: bad lines are skipped and reported with
// file and line number. Plain C, headless.

#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_FONT_CAP 128        // bytes of the font family name
#define CONFIG_MAX_FILE_SIZE MB(4) // a larger teal.conf is refused
#define CONFIG_DIAG_CAP 1000       // diagnostics kept per load (all are counted)

struct Settings {
    u8 font[CONFIG_FONT_CAP]; // family, UTF-8
    i32 font_len;
    f32 font_size;            // points
    i32 line_height;          // percent of the font's natural line height
    FbRenderMode render_mode;
    i32 tab_width;
    b32 underscore_is_word;
    b32 fsync_on_save;
};

typedef struct Theme { // 0xRRGGBB
    u32 background, text, cursor, selection, comment, string, keyword, number, type, variable;
} Theme;

typedef struct ConfigDiag {
    struct ConfigDiag *next;
    b32 warning;  // otherwise an error: the line was skipped
    String8 text; // "teal.conf:12: unknown command 'foo'", "teal.conf:3: warning: ..."
} ConfigDiag;

typedef struct Config {
    Settings settings;
    Theme theme;
    Keymap global;
    ConfigDiag *first_diag, *last_diag; // in the arena given to config_parse, at most CONFIG_DIAG_CAP
    i32 errors, warnings;               // all of them
} Config;

String8 config_default_text(void);
void    config_init(Config *c); // empty: no settings, no bindings
// Applies `text` on top of c. Diagnostics are allocated in `arena` and name `file_name`.
void    config_parse(Config *c, Arena *arena, String8 text, String8 file_name);
// The built-in defaults, then the user file at `path` on top (if path is not empty). Returns the
// status of reading that file (OS_FILE_NOT_FOUND when it does not exist or path is empty); `info`
// gets the size and write time of what was read.
OsFileStatus config_load(Config *c, Arena *arena, String8 path, OsFileInfo *info);
OsFileStatus config_read_file(Arena *arena, String8 path, String8 *text, OsFileInfo *info);
String8 config_file_name(String8 path); // the last path component

#endif // CONFIG_H
