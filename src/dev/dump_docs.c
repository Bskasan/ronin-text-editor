/* ======================================================================
   ronin: dev: --dump-docs, the docs site's keys and settings fragments

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// --dump-docs DIR (Phase 24) writes DIR\keys.md and DIR\settings.md, the generated parts of the documentation site's
// keys and settings pages, from the code itself, so those pages cannot disagree with the exe:
//   keys.md      one section per keymap section of the built-in configuration, in its order: the section's own
//                comment, then a key table (| keys | command | what it does |) of the commands bound there, in command
//                table order, with every sequence bound to them there as key_seq_print writes it; then every command
//                bound nowhere, with its description
//   settings.md  one section per comment of [settings] (a run of comment lines and the settings under it), headed by
//                its first setting: the comment as paragraphs (an example line "name = value" as code, indented lines
//                as a block), then the settings with their defaults as an ini block
// Only the built-in configuration is read: the output depends on nothing else. Every setting the parser knows must be
// in [settings] once, as a setting or an example line (exit 9 otherwise). The tool make_docs includes the fragments into
// docs/src/keys.md and settings.md.

#if RONIN_DEV

#define DUMP_CAP MB(2)

typedef struct DumpBuf {
    u8 *data;
    i64 len, cap;
    b32 full;
} DumpBuf;

static void dump_put(DumpBuf *b, String8 s) {
    if (b->full || b->len + s.len > b->cap) {
        b->full = 1;
        return;
    }
    memcpy(b->data + b->len, s.data, (size_t)s.len);
    b->len += s.len;
}

static void dump_puts(DumpBuf *b, const char *c) {
    dump_put(b, str8_cstr(c));
}

// Every setting the parser knows (config.c's tables and the settings it reads by name).
static i32 dump_setting_names(String8 *names, i32 cap) {
    i32 n = 0;
    for (i32 i = 0; i < config_dev_setting_count() && n < cap; i++) names[n++] = config_dev_setting_name(i);
    return n;
}

static String8 dump_trim(String8 s) {
    while (s.len && s.data[0] == ' ') s.data++, s.len--;
    while (s.len && (s.data[s.len - 1] == ' ' || s.data[s.len - 1] == '\r')) s.len--;
    return s;
}

// "name = value" with a setting's name: its index in names, else -1.
static i32 dump_setting_line(String8 line, String8 *names, i32 count) {
    i64 eq = 0;
    while (eq < line.len && line.data[eq] != '=') eq++;
    if (eq == line.len) return -1;
    String8 name = dump_trim(str8(line.data, eq));
    for (i32 i = 0; i < count; i++) if (str8_equal(name, names[i])) return i;
    return -1;
}

static b32 dump_nonblank(String8 line) {
    return dump_trim(line).len > 0;
}

static b32 dump_comment_line(String8 line) {
    return line.len && line.data[0] == '#';
}

// A comment line without its "#" and the one space after it.
static String8 dump_comment_text(String8 line) {
    line = str8(line.data + 1, line.len - 1);
    if (line.len && line.data[0] == ' ') line = str8(line.data + 1, line.len - 1);
    while (line.len && line.data[line.len - 1] == '\r') line.len--;
    return line;
}

// A code span around `s`, with enough backticks for one inside it (the key C-x `).
static void dump_code(DumpBuf *b, String8 s) {
    b32 tick = 0;
    for (i64 i = 0; i < s.len; i++) tick |= s.data[i] == '`';
    dump_puts(b, tick ? "`` " : "`");
    dump_put(b, s);
    dump_puts(b, tick ? " ``" : "`");
}

// Comment lines as Markdown: paragraphs (a lone "#" ends one), indented lines as a text block, an example setting as
// code on its own line. `used` counts each example's setting.
static void dump_comment(DumpBuf *b, String8 *lines, i64 count, String8 *names, i32 name_count, i32 *used) {
    b32 block = 0;
    for (i64 i = 0; i < count; i++) {
        String8 text = dump_comment_text(lines[i]);
        b32 indented = text.len >= 2 && text.data[0] == ' ' && text.data[1] == ' ';
        if (indented != block) {
            dump_puts(b, indented ? "\n```text\n" : "```\n\n");
            block = indented;
        }
        if (!text.len) {
            dump_puts(b, "\n");
            continue;
        }
        i32 example = indented ? -1 : dump_setting_line(text, names, name_count);
        if (example >= 0) {
            used[example]++;
            dump_code(b, text);
        } else {
            dump_put(b, text);
        }
        dump_puts(b, "\n");
    }
    if (block) dump_puts(b, "```\n");
    dump_puts(b, "\n");
}

typedef struct DumpSection {
    const char *header; // its line in the configuration
    const char *keymap; // the README check's keymap marker (NULL: the global map)
    u32 offset;         // its Keymap in Config
} DumpSection;

static const DumpSection dump_sections[] = {
    { "[keys]", NULL, offsetof(Config, global) },
    { "[keys minibuffer]", "minibuffer", offsetof(Config, minibuffer) },
    { "[keys search-results]", "search-results", offsetof(Config, search_results) },
    { "[keys compilation]", "compilation", offsetof(Config, compilation) },
    { "[keys command-list]", "command-list", offsetof(Config, command_list) },
    { "[keys git-changes]", "git-changes", offsetof(Config, git_changes) },
    { "[keys isearch]", "isearch", offsetof(Config, isearch) },
};

// The fragments' text. 0 when the built-in configuration does not have what they need (logged).
b32 dump_docs_text(Arena *arena, String8 *keys_out, String8 *settings_out) {
    Config *cfg = PUSH_STRUCT(arena, Config);
    config_defaults(cfg, arena);
    String8 text = config_default_text();
    i64 line_count = 0;
    for (i64 i = 0; i < text.len; i++) line_count += text.data[i] == '\n';
    String8 *lines = PUSH_ARRAY(arena, String8, line_count + 1);
    i64 n = 0, start = 0;
    for (i64 i = 0; i <= text.len; i++) {
        if (i < text.len && text.data[i] != '\n') continue;
        if (start < text.len) lines[n++] = str8(text.data + start, i - start);
        start = i + 1;
    }
    String8 names[64];
    i32 name_count = dump_setting_names(names, (i32)ARRAY_COUNT(names));
    i32 used[64] = { 0 };
    b32 ok = 1;

    // keys.md
    DumpBuf k = { PUSH_ARRAY(arena, u8, DUMP_CAP), 0, (i64)DUMP_CAP, 0 };
    dump_puts(&k, "<!-- generated by ronin_debug.exe --dump-docs from the command table and the built-in configuration -->\n\n");
    b32 *bound = PUSH_ARRAY(arena, b32, command_count());
    for (i32 s = 0; s < ARRAY_COUNT(dump_sections); s++) {
        const DumpSection *sec = &dump_sections[s];
        i64 h = 0;
        while (h < n && !str8_equal(dump_trim(lines[h]), str8_cstr(sec->header))) h++;
        if (h == n) {
            LOG("dump-docs: FAIL: no %s in the built-in configuration", sec->header);
            return 0;
        }
        dump_puts(&k, "## `");
        dump_puts(&k, sec->header);
        dump_puts(&k, "`\n\n");
        i64 c0 = h + 1;
        while (c0 < n && !dump_nonblank(lines[c0])) c0++;
        i64 c1 = c0;
        while (c1 < n && dump_comment_line(lines[c1])) c1++;
        dump_comment(&k, lines + c0, c1 - c0, names, name_count, used);
        if (sec->keymap) {
            dump_puts(&k, "<!-- keymap: ");
            dump_puts(&k, sec->keymap);
            dump_puts(&k, " -->\n");
        }
        dump_puts(&k, "| keys | command | what it does |\n|---|---|---|\n");
        Keymap *map = (Keymap *)((u8 *)cfg + sec->offset);
        for (i32 c = 0; c < command_count(); c++) {
            const Command *cmd = command_at(c);
            KeySeq seqs[16];
            i32 total = keymap_where_is(&map, 1, cmd, seqs, (i32)ARRAY_COUNT(seqs));
            if (!total) continue;
            bound[c] = 1;
            dump_puts(&k, "|");
            for (i32 q = 0; q < MIN(total, ARRAY_COUNT(seqs)); q++) {
                u8 key[64];
                i64 len = key_seq_print(&seqs[q], key, sizeof(key));
                dump_puts(&k, " ");
                dump_code(&k, str8(key, len));
            }
            dump_puts(&k, " | ");
            dump_code(&k, str8_cstr(cmd->name));
            dump_puts(&k, " | ");
            dump_puts(&k, cmd->description);
            dump_puts(&k, " |\n");
        }
        dump_puts(&k, "\n");
    }
    dump_puts(&k, "## Commands without a default key\n\n| command | what it does |\n|---|---|\n");
    for (i32 c = 0; c < command_count(); c++) {
        if (bound[c]) continue;
        dump_puts(&k, "| ");
        dump_code(&k, str8_cstr(command_at(c)->name));
        dump_puts(&k, " | ");
        dump_puts(&k, command_at(c)->description);
        dump_puts(&k, " |\n");
    }

    // settings.md: the groups of [settings], up to the next section.
    DumpBuf st = { PUSH_ARRAY(arena, u8, DUMP_CAP), 0, (i64)DUMP_CAP, 0 };
    dump_puts(&st, "<!-- generated by ronin_debug.exe --dump-docs from the built-in configuration -->\n\n");
    i64 i = 0;
    while (i < n && !str8_equal(dump_trim(lines[i]), STR8_LIT("[settings]"))) i++;
    for (i++; i < n && !(lines[i].len && lines[i].data[0] == '[');) {
        if (!dump_nonblank(lines[i])) {
            i++;
            continue;
        }
        i64 c0 = i;
        while (i < n && dump_comment_line(lines[i])) i++;
        i64 s0 = i;
        while (i < n && dump_nonblank(lines[i]) && !dump_comment_line(lines[i]) && lines[i].data[0] != '[') i++;
        if (s0 == c0 || i == s0) {
            LOG("dump-docs: FAIL: [settings] line %D: a %s", c0 + 1, s0 == c0 ? "setting without a comment above it" : "comment without a setting under it");
            return 0;
        }
        i64 eq = 0;
        while (eq < lines[s0].len && lines[s0].data[eq] != '=') eq++;
        String8 first = dump_trim(str8(lines[s0].data, eq));
        dump_puts(&st, "## ");
        dump_put(&st, first);
        dump_puts(&st, "\n\n");
        dump_comment(&st, lines + c0, s0 - c0, names, name_count, used);
        dump_puts(&st, "```ini\n");
        for (i64 q = s0; q < i; q++) {
            i32 known = dump_setting_line(lines[q], names, name_count);
            if (known < 0) {
                LOG("dump-docs: FAIL: [settings]: '%S' is no setting the parser knows", lines[q]);
                return 0;
            }
            used[known]++;
            dump_put(&st, dump_trim(lines[q]));
            dump_puts(&st, "\n");
        }
        dump_puts(&st, "```\n\n");
    }
    for (i32 s = 0; s < name_count; s++) {
        if (used[s] == 1) continue;
        LOG("dump-docs: FAIL: the setting %S is in [settings] %d times (once: as a setting or an example line)", names[s], used[s]);
        ok = 0;
    }
    if (k.full || st.full) {
        LOG("dump-docs: FAIL: the fragments are larger than %D bytes", (i64)DUMP_CAP);
        return 0;
    }
    *keys_out = str8(k.data, k.len);
    *settings_out = str8(st.data, st.len);
    return ok;
}

// --dump-docs DIR: 0 when both fragments were written; EXIT_OUTPUT_FILE's 7 when a file could not be; 9 when the
// configuration lacks something (the log says what).
i32 dump_docs(String8 dir) {
    Arena arena = arena_create(MB(64));
    String8 keys, settings;
    i32 code = 0;
    if (!dump_docs_text(&arena, &keys, &settings)) {
        code = 9;
    } else if (!os_make_dir(dir) || !os_write_file(str8_fmt(&arena, "%S\\keys.md", dir), keys) ||
               !os_write_file(str8_fmt(&arena, "%S\\settings.md", dir), settings)) {
        LOG("dump-docs: FAIL: cannot write %S\\keys.md and settings.md", dir);
        code = 7;
    } else {
        LOG("dump-docs: wrote %S\\keys.md (%D bytes) and settings.md (%D bytes)", dir, keys.len, settings.len);
    }
    os_release(arena.base);
    return code;
}

#endif // RONIN_DEV
