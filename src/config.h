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
    i32 undo_limit_mb;        // per buffer
    b32 transient_mark_mode;  // the region is highlighted (and region commands need it active)
    b32 delete_selection_mode; // typing replaces an active region
    i32 kill_ring_max;
    i32 indent_width;         // columns per indentation level
    b32 indent_with_tabs;     // indentation uses tabs (then spaces for the rest)
    b32 detect_indentation;   // a loaded file decides tabs or spaces for its buffer
    i32 completion_lines;     // rows of the minibuffer's candidate list
    b32 auto_revert;          // an unmodified buffer whose file changed on disk is reloaded
};

typedef struct Theme { // 0xRRGGBB
    u32 background, text, cursor, selection, comment, string, keyword, number, type, variable;
    u32 constant, function, directive; // syntax colors (Phase 8)
    u32 paren_match;                   // the background of a bracket and its match
    u32 prompt, completion_selection, completion_match; // the minibuffer
} Theme;

typedef struct ConfigDiag {
    struct ConfigDiag *next;
    b32 warning;  // otherwise an error: the line was skipped
    String8 text; // "teal.conf:12: unknown command 'foo'", "teal.conf:3: warning: ..."
} ConfigDiag;

typedef struct Config {
    Settings settings;
    Theme theme;
    Keymap global;     // [keys]
    Keymap minibuffer; // [keys minibuffer]: searched before global while the minibuffer is active
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
    String8 path;        // the user's teal.conf; empty = built-in defaults only
    b32 loaded_file;     // the version last loaded came from the file; its size and write time:
    i64 size;
    u64 write_time;
    i32 attempts;        // failed reads in a row; > 0 = a retry is pending
    u64 retry_at_us;
    b32 settling;        // a change notification arrived; read at settle_at_us
    u64 settle_at_us;
    OsFileStatus status; // of the last read
} ConfigSource;

// force: read now (startup, reload-config). Otherwise only a changed file is read, once a settle
// delay or a retry is due.
ConfigPoll config_poll(ConfigSource *src, Config *out, Arena *arena, b32 force, u64 now_us);
void       config_notify(ConfigSource *src, u64 now_us);  // a change notification: read CONFIG_SETTLE_MS later
b32        config_pending(ConfigSource *src);             // a settle delay or a retry is pending
u32        config_wait_ms(ConfigSource *src, u64 now_us); // until the next pending read, else CONFIG_WAIT_INFINITE

#endif // CONFIG_H
