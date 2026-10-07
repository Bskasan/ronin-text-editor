// config.c — see config.h.

#include "config_default.h"

String8 config_default_text(void) {
    return str8((u8 *)config_default, (i64)sizeof(config_default) - 1);
}

void config_init(Config *c) {
    memset(c, 0, sizeof(*c));
    c->global.name = "global";
}

// ---------------------------------------------------------------------------
// Diagnostics

typedef struct ConfigParser {
    Config *config;
    Arena *arena;
    String8 file;
    i64 line; // 1-based
} ConfigParser;

static void config_diag(ConfigParser *p, b32 warning, const char *fmt, ...) {
    Config *c = p->config;
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
    if (c->last_diag) c->last_diag->next = d;
    else c->first_diag = d;
    c->last_diag = d;
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

static void config_setting(ConfigParser *p, String8 name, String8 value) {
    Settings *s = &p->config->settings;
    i64 n;
    f32 x;
    b32 b;
    if (str8_equal(name, STR8_LIT("font"))) {
        if (value.len > CONFIG_FONT_CAP - 1) {
            config_diag(p, 0, "font name longer than %d bytes", CONFIG_FONT_CAP - 1);
            return;
        }
        memcpy(s->font, value.data, (size_t)value.len);
        s->font_len = (i32)value.len;
    } else if (str8_equal(name, STR8_LIT("font_size"))) {
        if (!config_parse_number(value, &x)) {
            config_diag(p, 0, "font_size: '%S' is not a number", config_quote(value));
            return;
        }
        if (x < 4.0f || x > 96.0f) config_diag(p, 1, "font_size %S is out of range (4 to 96), using %d", value, x < 4.0f ? 4 : 96);
        s->font_size = CLAMP(x, 4.0f, 96.0f);
    } else if (str8_equal(name, STR8_LIT("line_height"))) {
        if (!config_parse_int(value, &n)) {
            config_diag(p, 0, "line_height: '%S' is not a whole number (percent)", config_quote(value));
            return;
        }
        s->line_height = config_clamp_int(p, name, value, n, 80, 300);
    } else if (str8_equal(name, STR8_LIT("render_mode"))) {
        if (str8_equal(value, STR8_LIT("symmetric"))) s->render_mode = FB_RENDER_NATURAL_SYMMETRIC;
        else if (str8_equal(value, STR8_LIT("natural"))) s->render_mode = FB_RENDER_NATURAL;
        else if (str8_equal(value, STR8_LIT("classic"))) s->render_mode = FB_RENDER_GDI_CLASSIC;
        else config_diag(p, 0, "render_mode: '%S' is not symmetric, natural or classic", config_quote(value));
    } else if (str8_equal(name, STR8_LIT("tab_width"))) {
        if (!config_parse_int(value, &n)) {
            config_diag(p, 0, "tab_width: '%S' is not a whole number", config_quote(value));
            return;
        }
        s->tab_width = config_clamp_int(p, name, value, n, 1, 16);
    } else if (str8_equal(name, STR8_LIT("underscore_is_word")) || str8_equal(name, STR8_LIT("fsync_on_save"))) {
        if (!config_parse_bool(value, &b)) {
            config_diag(p, 0, "%S: '%S' is not true or false", name, config_quote(value));
            return;
        }
        if (name.data[0] == 'u') s->underscore_is_word = b;
        else s->fsync_on_save = b;
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
};

static void config_color(ConfigParser *p, String8 name, String8 value) {
    for (i32 i = 0; i < ARRAY_COUNT(config_colors); i++) {
        if (!str8_equal(name, str8_cstr(config_colors[i].name))) continue;
        u32 rgb;
        if (!config_parse_color(value, &rgb)) {
            config_diag(p, 0, "%S: '%S' is not a color (#rrggbb)", name, config_quote(value));
            return;
        }
        *(u32 *)((u8 *)&p->config->theme + config_colors[i].offset) = rgb;
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
    i32 removed = keymap_bind(&p->config->global, &seq, command);
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
} ConfigSection;

void config_parse(Config *c, Arena *arena, String8 text, String8 file_name) {
    ConfigParser p = { c, arena, file_name, 0 };
    ConfigSection section = CONFIG_SECTION_NONE;
    for (i64 i = 0; i < text.len;) {
        i64 end = i;
        while (end < text.len && text.data[end] != '\n') end++;
        String8 line = config_trim(str8(text.data + i, end - i));
        i = end + 1;
        p.line++;
        if (!line.len || line.data[0] == '#') continue;
        if (line.data[0] == '[') {
            if (line.data[line.len - 1] != ']') {
                config_diag(&p, 0, "bad section header (expected [settings], [colors] or [keys])");
                section = CONFIG_SECTION_UNKNOWN;
                continue;
            }
            String8 name = config_trim(str8(line.data + 1, line.len - 2));
            section = str8_equal(name, STR8_LIT("settings")) ? CONFIG_SECTION_SETTINGS
                    : str8_equal(name, STR8_LIT("colors"))   ? CONFIG_SECTION_COLORS
                    : str8_equal(name, STR8_LIT("keys"))     ? CONFIG_SECTION_KEYS
                    : CONFIG_SECTION_UNKNOWN;
            if (section == CONFIG_SECTION_UNKNOWN) config_diag(&p, 0, "unknown section [%S]", config_quote(name));
            continue;
        }
        switch (section) {
        case CONFIG_SECTION_NONE:
            config_diag(&p, 0, "line outside a section ([settings], [colors] or [keys])");
            break;
        case CONFIG_SECTION_UNKNOWN:
            break;
        case CONFIG_SECTION_KEYS:
            config_key(&p, line);
            break;
        case CONFIG_SECTION_SETTINGS:
        case CONFIG_SECTION_COLORS: {
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
            else config_color(&p, name, value);
        } break;
        }
    }
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

OsFileStatus config_load(Config *c, Arena *arena, String8 path, OsFileInfo *info) {
    config_init(c);
    config_parse(c, arena, config_default_text(), STR8_LIT("<built-in>"));
    // Dev: the built-in config parses cleanly, so every command it names exists.
    ASSERT(c->errors == 0 && c->warnings == 0);
    memset(info, 0, sizeof(*info));
    if (!path.len) return OS_FILE_NOT_FOUND;
    String8 text;
    OsFileStatus status = config_read_file(arena, path, &text, info);
    if (status == OS_FILE_OK) config_parse(c, arena, text, config_file_name(path));
    return status;
}

// ---------------------------------------------------------------------------
// Reloading

ConfigPoll config_poll(ConfigSource *src, Config *out, Arena *arena, b32 force, u64 now_us) {
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

u32 config_wait_ms(ConfigSource *src, u64 now_us) {
    if (src->attempts == 0) return CONFIG_WAIT_INFINITE;
    return now_us >= src->retry_at_us ? 0 : (u32)((src->retry_at_us - now_us + 999) / 1000);
}
