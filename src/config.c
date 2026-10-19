// config.c — see config.h.

#include "config_default.h"

String8 config_default_text(void) {
    return str8((u8 *)config_default, (i64)sizeof(config_default) - 1);
}

void config_init(Config *c) {
    memset(c, 0, sizeof(*c));
    c->global.name = "global";
    c->minibuffer.name = "minibuffer";
    c->isearch.name = "isearch";
    c->search_results.name = "search-results";
    c->compilation.name = "compilation";
    c->command_list.name = "command-list";
    c->git_changes.name = "git-changes";
}

// ---------------------------------------------------------------------------
// Diagnostics

// The state of one parse. Files in the config format other than ronin.conf (.ronin-project) use the
// lines, the diagnostics and the helpers below with their own sections (config == NULL).
typedef struct ConfigParser {
    Config *config;
    ConfigDiags *diags;
    Arena *arena;
    String8 file;
    String8 text;
    i64 pos;      // the next line starts here
    i64 line;     // 1-based: the line last returned
    Keymap *keys; // the map of the current [keys ...] section
    b32 theme;    // a theme file: only [theme] and [colors] (Phase 21)
    String8 theme_name; // ... its name (the file's), which its [theme] name should repeat
} ConfigParser;

static void config_diag(ConfigParser *p, b32 warning, const char *fmt, ...) {
    ConfigDiags *c = p->diags;
    if (warning) c->warnings++;
    else c->errors++;
    if (c->errors + c->warnings > CONFIG_DIAG_CAP) return;
    u8 message[512];
    va_list args;
    va_start(args, fmt);
    i64 n = MIN(fmt_v(message, sizeof(message), fmt, args), (i64)sizeof(message));
    va_end(args);
    ConfigDiag *d = PUSH_STRUCT(p->arena, ConfigDiag);
    d->warning = warning;
    d->text = str8_fmt(p->arena, "%S:%D: %s%S", p->file, p->line, warning ? "warning: " : "", str8(message, n));
    if (c->last) c->last->next = d;
    else c->first = d;
    c->last = d;
}

// User text quoted in a message: at most 60 bytes, cut on a character boundary.
static String8 config_quote(String8 s) {
    if (s.len <= 60) return s;
    i64 n = 60;
    while (n > 0 && (s.data[n] & 0xC0) == 0x80) n--;
    return str8(s.data, n);
}

// ---------------------------------------------------------------------------
// Values

static b32 config_is_blank(u8 c) {
    return c == ' ' || c == '\t' || c == '\r';
}

static String8 config_trim(String8 s) {
    while (s.len && config_is_blank(s.data[0])) { s.data++; s.len--; }
    while (s.len && config_is_blank(s.data[s.len - 1])) s.len--;
    return s;
}

static b32 config_parse_int(String8 v, i64 *out) {
    i64 i = 0, n = 0, sign = 1;
    if (v.len && v.data[0] == '-') { sign = -1; i = 1; }
    if (i == v.len) return 0;
    for (; i < v.len; i++) {
        if (v.data[i] < '0' || v.data[i] > '9') return 0;
        n = MIN(n * 10 + (v.data[i] - '0'), (i64)1000000000);
    }
    *out = sign * n;
    return 1;
}

// [-]digits[.digits]
static b32 config_parse_number(String8 v, f32 *out) {
    i64 i = 0, whole = 0, frac = 0, scale = 1, digits = 0;
    b32 negative = v.len && v.data[0] == '-';
    if (negative) i = 1;
    for (; i < v.len && v.data[i] >= '0' && v.data[i] <= '9'; i++, digits++) whole = MIN(whole * 10 + (v.data[i] - '0'), (i64)1000000);
    if (i < v.len && v.data[i] == '.') {
        for (i++; i < v.len && v.data[i] >= '0' && v.data[i] <= '9'; i++, digits++) {
            if (scale < 1000000) {
                frac = frac * 10 + (v.data[i] - '0');
                scale *= 10;
            }
        }
    }
    if (!digits || i != v.len) return 0;
    f32 x = (f32)whole + (f32)frac / (f32)scale;
    *out = negative ? -x : x;
    return 1;
}

static b32 config_parse_bool(String8 v, b32 *out) {
    if (str8_equal(v, STR8_LIT("true"))) { *out = 1; return 1; }
    if (str8_equal(v, STR8_LIT("false"))) { *out = 0; return 1; }
    return 0;
}

static i32 config_hex_digit(u8 c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static b32 config_parse_color(String8 v, u32 *out) {
    if (v.len != 7 || v.data[0] != '#') return 0;
    u32 rgb = 0;
    for (i64 i = 1; i < 7; i++) {
        i32 d = config_hex_digit(v.data[i]);
        if (d < 0) return 0;
        rgb = rgb << 4 | (u32)d;
    }
    *out = rgb;
    return 1;
}

// Clamps an integer setting to [lo, hi], with a warning when it was outside.
static i32 config_clamp_int(ConfigParser *p, String8 name, String8 value, i64 v, i64 lo, i64 hi) {
    if (v < lo || v > hi) {
        config_diag(p, 1, "%S %S is out of range (%D to %D), using %D", name, value, lo, hi, v < lo ? lo : hi);
    }
    return (i32)CLAMP(v, lo, hi);
}

// ---------------------------------------------------------------------------
// Sections

// Whole-number settings: clamped to [lo, hi] with a warning.
static const struct { const char *name; u32 offset; i32 lo, hi; } config_int_settings[] = {
    { "line_height", offsetof(Settings, line_height), 80, 300 },
    { "tab_width", offsetof(Settings, tab_width), 1, 16 },
    { "undo_limit_mb", offsetof(Settings, undo_limit_mb), 1, 2048 },
    { "kill_ring_max", offsetof(Settings, kill_ring_max), 1, KILL_RING_CAP },
    { "indent_width", offsetof(Settings, indent_width), 1, 16 },
    { "completion_lines", offsetof(Settings, completion_lines), 1, 40 },
    { "split_width_threshold", offsetof(Settings, split_width_threshold), 1, 10000 },
    { "startup_windows", offsetof(Settings, startup_windows), 1, 2 },
    { "project_max_files", offsetof(Settings, project_max_files), 1000, 10000000 },
    { "search_workers", offsetof(Settings, search_workers), 0, GREP_WORKERS_MAX },
    { "search_max_results", offsetof(Settings, search_max_results), 1, 1000000 },
    { "compilation_max_output_mb", offsetof(Settings, compilation_max_output_mb), 1, 1024 },
    { "max_cursors", offsetof(Settings, max_cursors), 1, 100000 },
    { "git_diff_max_lines", offsetof(Settings, git_diff_max_lines), 100, 1000000 },
    { "git_status_max_files", offsetof(Settings, git_status_max_files), 10, 100000 },
    { "git_timeout_ms", offsetof(Settings, git_timeout_ms), 1000, 600000 },
};

// Text settings: the bytes and their length, at most cap - 1 bytes.
static const struct { const char *name; u32 offset, len_offset, cap; } config_text_settings[] = {
    { "font", offsetof(Settings, font), offsetof(Settings, font_len), CONFIG_FONT_CAP },
    { "project_markers", offsetof(Settings, project_markers), offsetof(Settings, project_markers_len), CONFIG_LIST_CAP },
    { "project_ignore", offsetof(Settings, project_ignore), offsetof(Settings, project_ignore_len), CONFIG_LIST_CAP },
    { "git_path", offsetof(Settings, git_path), offsetof(Settings, git_path_len), CONFIG_PATH_CAP },
    { "ai_command", offsetof(Settings, ai_command), offsetof(Settings, ai_command_len), CONFIG_PATH_CAP },
};

static const struct { const char *name; u32 offset; } config_bool_settings[] = {
    { "underscore_is_word", offsetof(Settings, underscore_is_word) },
    { "fsync_on_save", offsetof(Settings, fsync_on_save) },
    { "transient_mark_mode", offsetof(Settings, transient_mark_mode) },
    { "delete_selection_mode", offsetof(Settings, delete_selection_mode) },
    { "indent_with_tabs", offsetof(Settings, indent_with_tabs) },
    { "detect_indentation", offsetof(Settings, detect_indentation) },
    { "auto_revert", offsetof(Settings, auto_revert) },
    { "show_paren_mode", offsetof(Settings, show_paren_mode) },
    { "compilation_relative_paths", offsetof(Settings, compilation_relative_paths) },
    { "early_window", offsetof(Settings, early_window) },
};

static void config_setting(ConfigParser *p, String8 name, String8 value) {
    Settings *s = &p->config->settings;
    i64 n;
    f32 x;
    for (i32 i = 0; i < ARRAY_COUNT(config_int_settings); i++) {
        if (!str8_equal(name, str8_cstr(config_int_settings[i].name))) continue;
        if (!config_parse_int(value, &n)) {
            config_diag(p, 0, "%S: '%S' is not a whole number", name, config_quote(value));
            return;
        }
        *(i32 *)((u8 *)s + config_int_settings[i].offset) =
            config_clamp_int(p, name, value, n, config_int_settings[i].lo, config_int_settings[i].hi);
        return;
    }
    for (i32 i = 0; i < ARRAY_COUNT(config_bool_settings); i++) {
        if (!str8_equal(name, str8_cstr(config_bool_settings[i].name))) continue;
        if (!config_parse_bool(value, (b32 *)((u8 *)s + config_bool_settings[i].offset))) {
            config_diag(p, 0, "%S: '%S' is not true or false", name, config_quote(value));
        }
        return;
    }
    for (i32 i = 0; i < ARRAY_COUNT(config_text_settings); i++) {
        if (!str8_equal(name, str8_cstr(config_text_settings[i].name))) continue;
        if (value.len > (i64)config_text_settings[i].cap - 1) {
            config_diag(p, 0, "%S longer than %d bytes", name, (i32)config_text_settings[i].cap - 1);
            return;
        }
        memcpy((u8 *)s + config_text_settings[i].offset, value.data, (size_t)value.len);
        *(i32 *)((u8 *)s + config_text_settings[i].len_offset) = (i32)value.len;
        return;
    }
    if (str8_equal(name, STR8_LIT("font_size"))) {
        if (!config_parse_number(value, &x)) {
            config_diag(p, 0, "font_size: '%S' is not a number", config_quote(value));
            return;
        }
        if (x < 4.0f || x > 96.0f) config_diag(p, 1, "font_size %S is out of range (4 to 96), using %d", value, x < 4.0f ? 4 : 96);
        s->font_size = CLAMP(x, 4.0f, 96.0f);
    } else if (str8_equal(name, STR8_LIT("render_mode"))) {
        if (str8_equal(value, STR8_LIT("symmetric"))) s->render_mode = FB_RENDER_NATURAL_SYMMETRIC;
        else if (str8_equal(value, STR8_LIT("natural"))) s->render_mode = FB_RENDER_NATURAL;
        else if (str8_equal(value, STR8_LIT("classic"))) s->render_mode = FB_RENDER_GDI_CLASSIC;
        else config_diag(p, 0, "render_mode: '%S' is not symmetric, natural or classic", config_quote(value));
    } else if (str8_equal(name, STR8_LIT("compilation_save_buffers"))) {
        if (str8_equal(value, STR8_LIT("ask"))) s->compilation_save_buffers = COMPILATION_SAVE_ASK;
        else if (str8_equal(value, STR8_LIT("all"))) s->compilation_save_buffers = COMPILATION_SAVE_ALL;
        else if (str8_equal(value, STR8_LIT("none"))) s->compilation_save_buffers = COMPILATION_SAVE_NONE;
        else config_diag(p, 0, "compilation_save_buffers: '%S' is not ask, all or none", config_quote(value));
    } else if (str8_equal(name, STR8_LIT("compilation_scroll_output"))) {
        if (str8_equal(value, STR8_LIT("first-error"))) s->compilation_scroll_output = COMPILATION_SCROLL_FIRST_ERROR;
        else if (str8_equal(value, STR8_LIT("true"))) s->compilation_scroll_output = COMPILATION_SCROLL_ALWAYS;
        else if (str8_equal(value, STR8_LIT("false"))) s->compilation_scroll_output = COMPILATION_SCROLL_NEVER;
        else config_diag(p, 0, "compilation_scroll_output: '%S' is not first-error, true or false", config_quote(value));
    } else if (str8_equal(name, STR8_LIT("ai_provider"))) {
        if (str8_equal(value, STR8_LIT("claude"))) s->ai_provider = AI_PROVIDER_CLAUDE;
        else if (str8_equal(value, STR8_LIT("openai"))) s->ai_provider = AI_PROVIDER_OPENAI;
        else if (str8_equal(value, STR8_LIT("custom"))) s->ai_provider = AI_PROVIDER_CUSTOM;
        else config_diag(p, 0, "ai_provider: '%S' is not claude, openai or custom", config_quote(value));
    } else if (str8_equal(name, STR8_LIT("terminal"))) {
        if (str8_equal(value, STR8_LIT("auto"))) s->terminal_program = TERMINAL_PROGRAM_AUTO;
        else if (str8_equal(value, STR8_LIT("wt"))) s->terminal_program = TERMINAL_PROGRAM_WT;
        else if (str8_equal(value, STR8_LIT("cmd"))) s->terminal_program = TERMINAL_PROGRAM_CMD;
        else config_diag(p, 0, "terminal: '%S' is not auto, wt or cmd", config_quote(value));
    } else {
        config_diag(p, 0, "unknown setting '%S'", config_quote(name));
    }
}

static const struct { const char *name; u32 offset; } config_colors[] = {
    { "background", offsetof(Theme, background) }, { "text", offsetof(Theme, text) },
    { "cursor", offsetof(Theme, cursor) },         { "selection", offsetof(Theme, selection) },
    { "comment", offsetof(Theme, comment) },       { "string", offsetof(Theme, string) },
    { "keyword", offsetof(Theme, keyword) },       { "number", offsetof(Theme, number) },
    { "type", offsetof(Theme, type) },             { "variable", offsetof(Theme, variable) },
    { "constant", offsetof(Theme, constant) },     { "function", offsetof(Theme, function) },
    { "directive", offsetof(Theme, directive) },   { "paren_match", offsetof(Theme, paren_match) },
    { "prompt", offsetof(Theme, prompt) },
    { "completion_selection", offsetof(Theme, completion_selection) },
    { "completion_match", offsetof(Theme, completion_match) },
    { "isearch", offsetof(Theme, isearch) },       { "isearch_text", offsetof(Theme, isearch_text) },
    { "lazy_highlight", offsetof(Theme, lazy_highlight) }, { "isearch_fail", offsetof(Theme, isearch_fail) },
    { "window_divider", offsetof(Theme, window_divider) },
    { "mode_line_background", offsetof(Theme, mode_line_background) },
    { "mode_line_text", offsetof(Theme, mode_line_text) },
    { "mode_line_inactive_background", offsetof(Theme, mode_line_inactive_background) },
    { "mode_line_inactive_text", offsetof(Theme, mode_line_inactive_text) },
    { "search_file", offsetof(Theme, search_file) },
    { "search_line_number", offsetof(Theme, search_line_number) },
    { "compilation_error", offsetof(Theme, compilation_error) },
    { "compilation_warning", offsetof(Theme, compilation_warning) },
    { "compilation_info", offsetof(Theme, compilation_info) },
    { "modal_background", offsetof(Theme, modal_background) },
    { "modal_border", offsetof(Theme, modal_border) },
    { "modal_selection", offsetof(Theme, modal_selection) },
    { "modal_description", offsetof(Theme, modal_description) },
    { "modal_key", offsetof(Theme, modal_key) },
    { "diff_added", offsetof(Theme, diff_added) },
    { "diff_removed", offsetof(Theme, diff_removed) },
    { "diff_hunk", offsetof(Theme, diff_hunk) },
    { "diff_header", offsetof(Theme, diff_header) },
    { "git_status", offsetof(Theme, git_status) },
};
_Static_assert(ARRAY_COUNT(config_colors) <= 64, "Config.colors_set has a bit per color");

// The colors whose default is another color: set by no file so far, they follow it. Since Phase 21 also the
// pairs that had the same built-in value, so a theme that leaves one out gets its own color for it, not ronin's.
static void config_derived_colors(Config *c) {
    static const struct { u32 offset, from; } derived[] = {
        { offsetof(Theme, constant), offsetof(Theme, number) },
        { offsetof(Theme, function), offsetof(Theme, keyword) },
        { offsetof(Theme, directive), offsetof(Theme, type) },
        { offsetof(Theme, prompt), offsetof(Theme, string) },
        { offsetof(Theme, completion_selection), offsetof(Theme, selection) },
        { offsetof(Theme, completion_match), offsetof(Theme, keyword) },
        { offsetof(Theme, compilation_info), offsetof(Theme, type) },
        { offsetof(Theme, mode_line_background), offsetof(Theme, text) },
        { offsetof(Theme, mode_line_text), offsetof(Theme, background) },
        { offsetof(Theme, mode_line_inactive_background), offsetof(Theme, background) },
        { offsetof(Theme, mode_line_inactive_text), offsetof(Theme, text) },
        { offsetof(Theme, search_file), offsetof(Theme, keyword) },
        { offsetof(Theme, search_line_number), offsetof(Theme, number) },
        { offsetof(Theme, modal_selection), offsetof(Theme, selection) },
        { offsetof(Theme, modal_key), offsetof(Theme, number) },
        { offsetof(Theme, diff_hunk), offsetof(Theme, number) },
        { offsetof(Theme, diff_header), offsetof(Theme, variable) },
        { offsetof(Theme, git_status), offsetof(Theme, string) },
    };
    for (i32 d = 0; d < ARRAY_COUNT(derived); d++) {
        for (i32 i = 0; i < ARRAY_COUNT(config_colors); i++) {
            if (config_colors[i].offset != derived[d].offset || (c->colors_set & (1ull << i))) continue;
            *(u32 *)((u8 *)&c->theme + derived[d].offset) = *(u32 *)((u8 *)&c->theme + derived[d].from);
        }
    }
}

static void config_color(ConfigParser *p, String8 name, String8 value) {
    for (i32 i = 0; i < ARRAY_COUNT(config_colors); i++) {
        if (!str8_equal(name, str8_cstr(config_colors[i].name))) continue;
        u32 rgb;
        if (!config_parse_color(value, &rgb)) {
            config_diag(p, 0, "%S: '%S' is not a color (#rrggbb)", name, config_quote(value));
            return;
        }
        *(u32 *)((u8 *)&p->config->theme + config_colors[i].offset) = rgb;
        p->config->colors_set |= 1ull << i;
        p->config->colors_last |= 1ull << i;
        return;
    }
    config_diag(p, 0, "unknown color '%S'", config_quote(name));
}

// "<key sequence> <command>": the last word is the command.
static void config_key(ConfigParser *p, String8 line) {
    i64 split = line.len;
    while (split > 0 && !config_is_blank(line.data[split - 1])) split--;
    if (split == 0) {
        config_diag(p, 0, "expected a key sequence and a command");
        return;
    }
    String8 keys = config_trim(str8(line.data, split));
    String8 name = str8(line.data + split, line.len - split);
    KeySeq seq;
    const char *error;
    if (!key_seq_parse(keys, &seq, &error)) {
        config_diag(p, 0, "bad key sequence '%S': %s", config_quote(keys), error);
        return;
    }
    const Command *command = NULL;
    if (!str8_equal(name, STR8_LIT("none"))) {
        command = command_find(name);
        if (!command) {
            config_diag(p, 0, "unknown command '%S'", config_quote(name));
            return;
        }
    }
    i32 removed = keymap_bind(p->keys, &seq, command);
    if (removed < 0) {
        config_diag(p, 0, "too many key bindings (at most %d)", (i32)KEYMAP_CAP);
    } else if (removed > 0) {
        u8 text[KEY_SEQ_TEXT_CAP];
        config_diag(p, 1, "%S removed %d binding%s it conflicts with (one sequence is a prefix of the other)",
                    str8(text, key_seq_print(&seq, text, sizeof(text))), removed, removed == 1 ? "" : "s");
    }
}

typedef enum ConfigSection {
    CONFIG_SECTION_NONE,
    CONFIG_SECTION_UNKNOWN, // after an unknown [header]: lines are skipped silently
    CONFIG_SECTION_SETTINGS,
    CONFIG_SECTION_COLORS,
    CONFIG_SECTION_KEYS,
    CONFIG_SECTION_THEME,   // a theme file's [theme] block
} ConfigSection;

// The next line that is neither blank nor a comment ('#' first), trimmed; false at the end. p->line is
// its number.
static b32 config_next_line(ConfigParser *p, String8 *line) {
    while (p->pos < p->text.len) {
        i64 end = p->pos;
        while (end < p->text.len && p->text.data[end] != '\n') end++;
        *line = config_trim(str8(p->text.data + p->pos, end - p->pos));
        p->pos = end + 1;
        p->line++;
        if (line->len && line->data[0] != '#') return 1;
    }
    return 0;
}

// A "[name]" line: 1 with the name trimmed, -1 for a '[' line without its ']', 0 for any other line.
static i32 config_header(String8 line, String8 *name) {
    if (!line.len || line.data[0] != '[') return 0;
    if (line.data[line.len - 1] != ']') return -1;
    *name = config_trim(str8(line.data + 1, line.len - 2));
    return 1;
}

// A theme's [theme] block: what it is and where it comes from. Only the name is checked (against the file's).
static void config_theme_key(ConfigParser *p, String8 name, String8 value) {
    static const char *const keys[] = { "name", "source", "commit", "license", "author" };
    for (i32 i = 0; i < ARRAY_COUNT(keys); i++) {
        if (!str8_equal(name, str8_cstr(keys[i]))) continue;
        if (i == 0 && !theme_name_equal(value, p->theme_name)) {
            config_diag(p, 1, "name '%S' is not the file's name '%S'", config_quote(value), p->theme_name);
        }
        return;
    }
    config_diag(p, 0, "unknown theme key '%S' (name, source, commit, license or author)", config_quote(name));
}

static void config_parse_text(Config *c, Arena *arena, String8 text, String8 file_name, b32 theme, String8 theme_name) {
    ConfigParser p = { .config = c, .diags = &c->diags, .arena = arena, .file = file_name, .text = text, .keys = &c->global,
                       .theme = theme, .theme_name = theme_name };
    ConfigSection section = CONFIG_SECTION_NONE;
    String8 line;
    c->colors_last = 0;
    while (config_next_line(&p, &line)) {
        String8 head;
        i32 header = config_header(line, &head);
        if (header && theme) {
            section = header > 0 && str8_equal(head, STR8_LIT("theme"))  ? CONFIG_SECTION_THEME
                    : header > 0 && str8_equal(head, STR8_LIT("colors")) ? CONFIG_SECTION_COLORS
                    : CONFIG_SECTION_UNKNOWN;
            if (section == CONFIG_SECTION_UNKNOWN) config_diag(&p, 0, "a theme has only [theme] and [colors] sections");
            continue;
        }
        if (header) {
            if (header < 0) {
                config_diag(&p, 0, "bad section header (expected [settings], [colors], [keys], [keys minibuffer], [keys isearch], [keys search-results], [keys compilation], [keys command-list] or [keys git-changes])");
                section = CONFIG_SECTION_UNKNOWN;
                continue;
            }
            section = str8_equal(head, STR8_LIT("settings")) ? CONFIG_SECTION_SETTINGS
                    : str8_equal(head, STR8_LIT("colors"))   ? CONFIG_SECTION_COLORS
                    : CONFIG_SECTION_UNKNOWN;
            // [keys] and [keys global] bind in the global map, [keys minibuffer], [keys isearch],
            // [keys search-results], [keys compilation], [keys command-list] and [keys git-changes] in theirs.
            if (str8_starts_with(head, STR8_LIT("keys")) && (head.len == 4 || config_is_blank(head.data[4]))) {
                String8 map = config_trim(str8(head.data + 4, head.len - 4));
                p.keys = !map.len || str8_equal(map, STR8_LIT("global")) ? &c->global
                       : str8_equal(map, STR8_LIT("minibuffer"))       ? &c->minibuffer
                       : str8_equal(map, STR8_LIT("isearch"))          ? &c->isearch
                       : str8_equal(map, STR8_LIT("search-results"))   ? &c->search_results
                       : str8_equal(map, STR8_LIT("compilation"))      ? &c->compilation
                       : str8_equal(map, STR8_LIT("command-list"))     ? &c->command_list
                       : str8_equal(map, STR8_LIT("git-changes"))      ? &c->git_changes : NULL;
                if (p.keys) section = CONFIG_SECTION_KEYS;
            }
            if (section == CONFIG_SECTION_UNKNOWN) config_diag(&p, 0, "unknown section [%S]", config_quote(head));
            continue;
        }
        switch (section) {
        case CONFIG_SECTION_NONE:
            if (theme) config_diag(&p, 0, "line outside a section ([theme] or [colors])");
            else config_diag(&p, 0, "line outside a section ([settings], [colors], [keys], [keys minibuffer], [keys isearch], [keys search-results], [keys compilation], [keys command-list] or [keys git-changes])");
            break;
        case CONFIG_SECTION_UNKNOWN:
            break;
        case CONFIG_SECTION_KEYS:
            config_key(&p, line);
            break;
        case CONFIG_SECTION_SETTINGS:
        case CONFIG_SECTION_COLORS:
        case CONFIG_SECTION_THEME: {
            i64 eq = 0;
            while (eq < line.len && line.data[eq] != '=') eq++;
            if (eq == line.len) {
                config_diag(&p, 0, "expected name = value");
                break;
            }
            String8 name = config_trim(str8(line.data, eq));
            String8 value = config_trim(str8(line.data + eq + 1, line.len - eq - 1));
            if (!name.len || !value.len) {
                config_diag(&p, 0, "expected name = value");
                break;
            }
            if (section == CONFIG_SECTION_SETTINGS) config_setting(&p, name, value);
            else if (section == CONFIG_SECTION_THEME) config_theme_key(&p, name, value);
            else config_color(&p, name, value);
        } break;
        }
    }
    config_derived_colors(c);
}

void config_parse(Config *c, Arena *arena, String8 text, String8 file_name) {
    config_parse_text(c, arena, text, file_name, 0, str8(NULL, 0));
}

void config_parse_theme(Config *c, Arena *arena, String8 text, String8 file_name, String8 theme_name) {
    config_parse_text(c, arena, text, file_name, 1, theme_name);
}

// ---------------------------------------------------------------------------
// Files

String8 config_file_name(String8 path) {
    i64 i = path.len;
    while (i > 0 && path.data[i - 1] != '\\' && path.data[i - 1] != '/') i--;
    return str8(path.data + i, path.len - i);
}

OsFileStatus config_read_file(Arena *arena, String8 path, String8 *text, OsFileInfo *info) {
    *text = str8(NULL, 0);
    OsFile file;
    OsFileStatus status = os_file_open_read(path, &file, info);
    if (status != OS_FILE_OK) return status;
    if (info->size > (i64)CONFIG_MAX_FILE_SIZE) {
        os_file_close(file);
        return OS_FILE_TOO_LARGE;
    }
    u8 *data = PUSH_ARRAY(arena, u8, info->size);
    status = os_file_read(file, data, info->size);
    os_file_close(file);
    if (status == OS_FILE_OK) *text = str8(data, info->size);
    return status;
}

void config_defaults(Config *c, Arena *arena) {
    config_init(c);
    config_parse(c, arena, config_default_text(), STR8_LIT("<built-in>"));
    config_parse_theme(c, arena, theme_builtin(STR8_LIT("ronin")), STR8_LIT("<built-in theme ronin>"), STR8_LIT("ronin"));
}

OsFileStatus config_load(Config *c, Arena *arena, String8 path, OsFileInfo *info) {
    config_defaults(c, arena);
    // Dev: the built-in config parses cleanly, so every command it names exists.
#if RONIN_DEV
    for (ConfigDiag *d = c->diags.first; d; d = d->next) LOG("config: built-in: %S", d->text);
#endif
    ASSERT(c->diags.errors == 0 && c->diags.warnings == 0);
    memset(info, 0, sizeof(*info));
    if (!path.len) return OS_FILE_NOT_FOUND;
    String8 text;
    OsFileStatus status = config_read_file(arena, path, &text, info);
    if (status == OS_FILE_OK) config_parse(c, arena, text, config_file_name(path));
    return status;
}

// ---------------------------------------------------------------------------
// Reloading

void config_notify(ConfigSource *src, u64 now_us) {
    src->settling = 1;
    src->settle_at_us = now_us + (u64)CONFIG_SETTLE_MS * 1000;
}

b32 config_pending(ConfigSource *src) {
    return src->settling || src->attempts > 0;
}

ConfigPoll config_poll(ConfigSource *src, Config *out, Arena *arena, b32 force, u64 now_us) {
    if (src->settling && !force) {
        if (now_us < src->settle_at_us) return CONFIG_POLL_UNCHANGED;
        src->settling = 0;
    }
    if (force) src->settling = 0;
    if (src->attempts > 0) {
        if (!force && now_us < src->retry_at_us) return CONFIG_POLL_UNCHANGED;
    } else if (!force) {
        OsFileInfo now;
        if (!src->path.len || os_file_info(src->path, &now) != OS_FILE_OK) return CONFIG_POLL_UNCHANGED; // missing for now
        if (src->loaded_file && now.size == src->size && now.write_time == src->write_time) return CONFIG_POLL_UNCHANGED;
    }
    OsFileInfo info;
    src->status = config_load(out, arena, src->path, &info);
    if (src->status == OS_FILE_SHARING_VIOLATION && ++src->attempts < CONFIG_RETRY_ATTEMPTS) {
        src->retry_at_us = now_us + (u64)CONFIG_RETRY_MS * 1000;
        return CONFIG_POLL_RETRY;
    }
    src->attempts = 0;
    if (src->status != OS_FILE_OK && src->status != OS_FILE_NOT_FOUND) return CONFIG_POLL_FAILED;
    src->loaded_file = src->status == OS_FILE_OK;
    src->size = info.size;
    src->write_time = info.write_time;
    return CONFIG_POLL_LOADED;
}

static u32 config_ms_until(u64 at_us, u64 now_us) {
    return now_us >= at_us ? 0 : (u32)((at_us - now_us + 999) / 1000);
}

u32 config_wait_ms(ConfigSource *src, u64 now_us) {
    u32 wait = CONFIG_WAIT_INFINITE;
    if (src->settling) wait = config_ms_until(src->settle_at_us, now_us);
    if (src->attempts > 0) wait = MIN(wait, config_ms_until(src->retry_at_us, now_us));
    return wait;
}
