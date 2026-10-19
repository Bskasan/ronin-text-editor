// config.h — one plain-text config file holds settings, colors and key bindings. The built-in
// defaults (config_default.h) are the same format, parsed by the same code at startup; the user's
// file is applied on top. An error never stops a load: bad lines are skipped and reported with
// file and line number. Plain C, headless.

#ifndef CONFIG_H
#define CONFIG_H

#define CONFIG_FONT_CAP 128        // bytes of the font family name
#define CONFIG_LIST_CAP 2048       // bytes of a blank-separated list setting (project_markers, project_ignore)
#define CONFIG_PATH_CAP 260        // bytes of a path setting (git_path)
#define CONFIG_MAX_FILE_SIZE MB(4) // a larger ronin.conf is refused
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
    i32 undo_limit_mb;        // per buffer
    b32 transient_mark_mode;  // the region is highlighted (and region commands need it active)
    b32 delete_selection_mode; // typing replaces an active region
    i32 kill_ring_max;
    i32 indent_width;         // columns per indentation level
    b32 indent_with_tabs;     // indentation uses tabs (then spaces for the rest)
    b32 detect_indentation;   // a loaded file decides tabs or spaces for its buffer
    i32 completion_lines;     // rows of the minibuffer's candidate list
    b32 auto_revert;          // an unmodified buffer whose file changed on disk is reloaded
    b32 show_paren_mode;      // the bracket at point and its match are highlighted
    i32 split_width_threshold; // columns: a single window pops up a buffer side by side from this frame width on
    i32 startup_windows;      // 1, or 2: the frame starts split side by side
    b32 early_window;         // the window is shown in the background color as soon as the config is read
    u8 project_markers[CONFIG_LIST_CAP]; // blank-separated names whose presence makes a directory a project root
    i32 project_markers_len;
    u8 project_ignore[CONFIG_LIST_CAP];  // blank-separated .gitignore patterns never listed in a project
    i32 project_ignore_len;
    i32 project_max_files;    // a project scan stops there
    i32 search_workers;       // the project-wide search's threads; 0 = from the processor count
    i32 search_max_results;   // the matches it lists before it stops
    i32 compilation_save_buffers;  // COMPILATION_SAVE_*: the unsaved file buffers before a build
    i32 compilation_scroll_output; // COMPILATION_SCROLL_*: windows on *compilation* follow its output
    i32 compilation_max_output_mb; // a build's output kept in *compilation*
    i32 max_cursors;          // multiple cursors: commands that would add more stop there
    b32 compilation_relative_paths; // an absolute path under the project root shown relative to it in *compilation*
    u8 git_path[CONFIG_PATH_CAP]; // the git changes view (C-x g): the git program; empty = git.exe on PATH
    i32 git_path_len;
    i32 git_diff_max_lines;   // a diff is cut there (a last line says so)
    i32 git_status_max_files; // the file list is cut there
    i32 git_timeout_ms;       // a git that prints nothing for this long is stopped
    i32 ai_provider;          // AI_PROVIDER_*: what open-ai-terminal (C-c a) runs
    u8 ai_command[CONFIG_PATH_CAP]; // ... this command line instead, for any provider; empty = the provider's
    i32 ai_command_len;
    i32 terminal_program;     // TERMINAL_PROGRAM_* ("terminal" in a file): Windows Terminal, the classic console, or wt if found
    u8 theme[THEME_NAME_CAP]; // the color theme (Phase 21), lowercase
    i32 theme_len;
};

enum { AI_PROVIDER_CLAUDE, AI_PROVIDER_OPENAI, AI_PROVIDER_CUSTOM };                 // claude, codex, ai_command
enum { TERMINAL_PROGRAM_AUTO, TERMINAL_PROGRAM_WT, TERMINAL_PROGRAM_CMD };           // wt.exe if found else cmd, wt.exe, cmd.exe
enum { COMPILATION_SAVE_ASK, COMPILATION_SAVE_ALL, COMPILATION_SAVE_NONE };            // asked one by one, all saved, left
enum { COMPILATION_SCROLL_FIRST_ERROR, COMPILATION_SCROLL_ALWAYS, COMPILATION_SCROLL_NEVER }; // until the first error, always, never

typedef struct Theme { // 0xRRGGBB
    u32 background, text, cursor, selection, comment, string, keyword, number, type, variable;
    u32 constant, function, directive; // syntax colors (Phase 8)
    u32 paren_match;                   // the background of a bracket and its match
    u32 prompt, completion_selection, completion_match; // the minibuffer
    u32 isearch, isearch_text;         // the current search match: its background and text
    u32 lazy_highlight;                // the background of the other matches on screen
    u32 isearch_fail;                  // the background of the failing part of a search string
    u32 window_divider;                // the line between windows side by side
    u32 mode_line_background, mode_line_text; // the selected window's mode line (text and background unless set:
                                       // inverse video; Phase 21)
    u32 mode_line_inactive_background; // the mode lines of the windows that are not selected; unless a file
    u32 mode_line_inactive_text;       // sets them, they are background and text
    u32 search_file, search_line_number; // a results line's file name and line:column (keyword and number unless set)
    u32 compilation_error, compilation_warning, compilation_info; // a build's kind words in *compilation*
    u32 modal_background, modal_border; // a modal box (the command list): its inside and its frame and separators
    u32 modal_selection;               // its selected row (selection unless set)
    u32 modal_description, modal_key;  // the command list's descriptions and key bindings (number unless set)
    u32 diff_added, diff_removed;      // the git changes view (Phase 19): a diff's added and removed lines,
    u32 diff_hunk, diff_header;        // its @@ lines and file headers (number and variable unless set),
    u32 git_status;                    // the file list's two-letter status (string unless set)
} Theme;

typedef struct ConfigDiag {
    struct ConfigDiag *next;
    b32 warning;  // otherwise an error: the line was skipped
    String8 text; // "ronin.conf:12: unknown command 'foo'", "ronin.conf:3: warning: ..."
} ConfigDiag;

// The diagnostics of one load, of ronin.conf or of another file in the same format (.ronin-project).
typedef struct ConfigDiags {
    ConfigDiag *first, *last; // in the arena given to the parser, at most CONFIG_DIAG_CAP
    i32 errors, warnings;     // all of them
} ConfigDiags;

typedef struct Config {
    Settings settings;
    Theme theme;
    Keymap global;     // [keys]
    Keymap minibuffer; // [keys minibuffer]: searched before global while the minibuffer is active
    Keymap isearch;    // [keys isearch]: searched before global while an isearch is active
    Keymap search_results; // [keys search-results]: before global in a results buffer (*search*, *occur*)
    Keymap compilation;    // [keys compilation]: before global in *compilation*
    Keymap command_list;   // [keys command-list]: before global while the command list is open
    Keymap git_changes;    // [keys git-changes]: before global while the git changes view is open
    u64 colors_set;    // the colors some parsed text set (bit = index in the color table): the derived defaults
    u64 colors_last;   // the colors the last parsed text set (a theme goes under the user's [colors]; Phase 21)
    String8 theme_file; // the file and the line of the theme setting last parsed (its diagnostics)
    i64 theme_line;
    String8 theme_name; // the theme in effect (ronin when the chosen one could not be loaded)
    i32 theme_overrides; // the colors ronin.conf's [colors] set over a theme other than ronin (they win: reported)
    ConfigDiags diags;
} Config;

String8 config_default_text(void);
void    config_init(Config *c); // empty: no settings, no bindings
// Applies `text` on top of c. Diagnostics are allocated in `arena` and name `file_name`.
void    config_parse(Config *c, Arena *arena, String8 text, String8 file_name);
// A theme file's text (Phase 21): only its [theme] block and its [colors]; theme_name is the file's.
void    config_parse_theme(Config *c, Arena *arena, String8 text, String8 file_name, String8 theme_name);
// The built-in configuration and the ronin theme, into an empty config (what config_load starts from).
void    config_defaults(Config *c, Arena *arena);
// The built-in defaults and the ronin theme, then the user file at `path` on top (if path is not empty), then
// the chosen theme under the user file's [colors] (Phase 21: the theme setting, or theme->override; theme NULL:
// built-in themes only). Returns the status of reading the user file (OS_FILE_NOT_FOUND when it does not exist or
// path is empty); `info` gets the size and write time of what was read, `theme` what became of the theme.
OsFileStatus config_load(Config *c, Arena *arena, String8 path, OsFileInfo *info, ThemeLoad *theme);
OsFileStatus config_read_file(Arena *arena, String8 path, String8 *text, OsFileInfo *info);
String8 config_file_name(String8 path); // the last path component

// ---------------------------------------------------------------------------
// Reloading. Driven by directory change notifications, never by a timer: on a notification the
// file is read again only if its size or write time differ from the version last loaded. A file
// that is missing for a moment (editors that save through a temporary file and a rename) counts
// as unchanged. A sharing violation (another program still writing) is retried every
// CONFIG_RETRY_MS, at most CONFIG_RETRY_ATTEMPTS reads in all; the caller waits with that timeout
// only while a retry is pending. A notification is not acted on at once: the read waits
// CONFIG_SETTLE_MS after the last one, so an editor that truncates the file and then writes it
// is read once it is done, not while it is empty.

#define CONFIG_SETTLE_MS 50
#define CONFIG_RETRY_MS 100
#define CONFIG_RETRY_ATTEMPTS 5
#define CONFIG_WAIT_INFINITE 0xFFFFFFFFu

typedef enum ConfigPoll {
    CONFIG_POLL_UNCHANGED, // nothing to load (or a pending retry is not due yet)
    CONFIG_POLL_LOADED,    // `out` holds the new config
    CONFIG_POLL_RETRY,     // a sharing violation: try again in CONFIG_RETRY_MS (`out` has the defaults only)
    CONFIG_POLL_FAILED,    // the file could not be read (status says why); keep the old config
} ConfigPoll;

typedef struct ConfigSource {
    String8 path;        // the user's ronin.conf; empty = built-in defaults only
    b32 loaded_file;     // the version last loaded came from the file; its size and write time:
    i64 size;
    u64 write_time;
    i32 attempts;        // failed reads in a row; > 0 = a retry is pending
    u64 retry_at_us;
    b32 settling;        // a change notification arrived; read at settle_at_us
    u64 settle_at_us;
    OsFileStatus status; // of the last read
    // Themes (Phase 21): where theme files are looked for, and set-theme's choice for the session with ronin.conf's
    // theme when it was made (ronin.conf naming another theme since ends it).
    ThemeDirs themes;
    u8 override[THEME_NAME_CAP];
    i32 override_len;
    u8 override_file[THEME_NAME_CAP];
    i32 override_file_len;
    u8 theme[THEME_NAME_CAP];  // the theme asked for at the last load (found or not) and what its name found then:
    i32 theme_len;             // a notification reloads when that changed (its file written, added or removed)
    ThemeWhere theme_where;
    i64 theme_size;
    u64 theme_write_time;
} ConfigSource;

// force: read now (startup, reload-config). Otherwise only a changed file is read, once a settle
// delay or a retry is due.
ConfigPoll config_poll(ConfigSource *src, Config *out, Arena *arena, b32 force, u64 now_us);
void       config_notify(ConfigSource *src, u64 now_us);  // a change notification: read CONFIG_SETTLE_MS later
b32        config_pending(ConfigSource *src);             // a settle delay or a retry is pending
u32        config_wait_ms(ConfigSource *src, u64 now_us); // until the next pending read, else CONFIG_WAIT_INFINITE

#endif // CONFIG_H
