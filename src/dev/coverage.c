/* ======================================================================
   ronin: dev: the feature coverage map (--test) and its allow-list

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// The feature coverage map (#29). While --test runs, outside its seeded groups (test_seeded), the platform counts
// every command view_run_command runs, every default key binding that ran its command (any test, and those that came
// through the input harness's real key messages), and every setting whose effect a test checked (test_setting_set,
// then TEST_SETTING_EFFECT: parsing a value never counts). At the end coverage_check fails on:
//   - an item no test exercised that res/coverage_allow.txt does not list ("<kind> | <name> | <reason> | #<issue>",
//     kind command, binding, binding-real or setting; a binding's name "<keymap> <seq>");
//   - an entry of that list without a reason or an issue, naming no item, or naming one that is covered now (stale:
//     the list only shrinks);
//   - the counter table full, or a key too long for it (a hit never goes uncounted silently);
//   - a side effect --test must not have: ronin using the real clipboard (any open, read or write, counted in the
//     os_* layer; another program copying meanwhile is only logged), %APPDATA%\ronin or %LOCALAPPDATA%\ronin
//     changed, a real git, terminal or build refused (os_dev_real_refused).

#if RONIN_DEV

#define COVERAGE_ALLOW_FILE "res\\coverage_allow.txt"
#define COVERAGE_ALLOW_CAP 1024

typedef struct Coverage {
    Arena *arena;
    u32 clip_calls, clip_seq;  // ronin's real clipboard calls and Windows' sequence number when --test started
    String8 trees[2];          // %APPDATA%\ronin and %LOCALAPPDATA%\ronin then, as coverage_tree lists them
} Coverage;

typedef struct CoverageAllow {
    String8 kind, name, reason, issue;
    i32 line;
    b32 matched; // names an item that exists
    b32 used;    // ... and that item is not covered
} CoverageAllow;

static const char *const coverage_tree_vars[2] = { "APPDATA", "LOCALAPPDATA" };

// One line per entry under dir (relative path, size, write time, folder or not), depth first in the file system's
// order; "absent" when dir does not exist.
static void coverage_tree_walk(Arena *arena, String8 dir, String8 rel, String8 *out) {
    OsDirEntry *e;
    i64 n;
    if (os_list_dir(arena, dir, &e, &n) != OS_FILE_OK) return;
    for (i64 i = 0; i < n; i++) {
        String8 path = str8_fmt(arena, "%S\\%S", dir, e[i].name), sub = rel.len ? str8_fmt(arena, "%S\\%S", rel, e[i].name) : e[i].name;
        OsFileInfo info = { 0 };
        os_file_info(path, &info);
        *out = str8_fmt(arena, "%S%S|%D|%U|%d\n", *out, sub, info.size, info.write_time, e[i].is_dir);
        if (e[i].is_dir && !e[i].reparse) coverage_tree_walk(arena, path, sub, out);
    }
}

static String8 coverage_tree(Arena *arena, i32 which) {
    String8 base = os_get_env(arena, str8_cstr(coverage_tree_vars[which]));
    if (!base.len) return STR8_LIT("absent");
    String8 dir = str8_fmt(arena, "%S\\ronin", base), out = str8(NULL, 0);
    OsFileInfo info = { 0 };
    if (os_file_info(dir, &info) != OS_FILE_OK || !info.is_dir) return STR8_LIT("absent");
    coverage_tree_walk(arena, dir, str8(NULL, 0), &out);
    return out.len ? out : STR8_LIT("empty");
}

// --test starts: counting on, and what must not change recorded.
void coverage_begin(Coverage *c, Arena *arena) {
    c->arena = arena;
    c->clip_calls = os_dev_clipboard_real(&c->clip_seq);
    for (i32 i = 0; i < 2; i++) c->trees[i] = coverage_tree(arena, i);
    os_dev_cover_enable(1);
}

static String8 coverage_trim(String8 s) {
    while (s.len && (s.data[0] == ' ' || s.data[0] == '\t')) s.data++, s.len--;
    while (s.len && (s.data[s.len - 1] == ' ' || s.data[s.len - 1] == '\t' || s.data[s.len - 1] == '\r')) s.len--;
    return s;
}

// res/coverage_allow.txt into `out`; the count, or -1 when it cannot be read. A malformed line is a failure.
static i32 coverage_allow_read(Arena *arena, String8 root, CoverageAllow *out, i32 cap, i32 *failures) {
    String8 path = str8_fmt(arena, "%S\\%s", root, COVERAGE_ALLOW_FILE), text;
    if (os_read_small_file(arena, path, MB(1), &text) != OS_FILE_OK) {
        LOG("test: FAIL: coverage: cannot read %S", path);
        (*failures)++;
        return -1;
    }
    i32 n = 0, line = 0;
    for (i64 at = 0; at < text.len;) {
        i64 end = at;
        while (end < text.len && text.data[end] != '\n') end++;
        String8 s = coverage_trim(str8(text.data + at, end - at));
        at = end + 1;
        line++;
        if (!s.len || s.data[0] == '#') continue;
        String8 f[4] = { 0 };
        i32 k = 0;
        for (i64 i = 0, start = 0; i <= s.len && k < 4; i++) {
            if (i == s.len || (s.data[i] == '|' && k < 3)) {
                f[k++] = coverage_trim(str8(s.data + start, i - start));
                start = i + 1;
            }
        }
        b32 issue = f[3].len >= 2 && f[3].data[0] == '#';
        for (i64 i = 1; i < f[3].len && issue; i++) issue = f[3].data[i] >= '0' && f[3].data[i] <= '9';
        b32 kind = str8_equal(f[0], STR8_LIT("command")) || str8_equal(f[0], STR8_LIT("binding")) || str8_equal(f[0], STR8_LIT("binding-real")) ||
                   str8_equal(f[0], STR8_LIT("setting"));
        if (k < 4 || !kind || !f[1].len || !f[2].len || !issue) {
            LOG("test: FAIL: coverage: %s:%d: expected '<command|binding|binding-real|setting> | <name> | <reason> | #<issue>', got '%S'",
                COVERAGE_ALLOW_FILE, line, s);
            (*failures)++;
            continue;
        }
        if (n == cap) {
            LOG("test: FAIL: coverage: %s:%d: more than %d entries", COVERAGE_ALLOW_FILE, line, cap);
            (*failures)++;
            break;
        }
        out[n++] = (CoverageAllow){ f[0], f[1], f[2], f[3], line, 0, 0 };
    }
    return n;
}

// The item is on the list (marked as matched); covered items mark their entry stale by leaving `used` unset.
static b32 coverage_allowed(CoverageAllow *allow, i32 n, const char *kind, String8 name, b32 covered) {
    for (i32 i = 0; i < n; i++) {
        if (!str8_equal(allow[i].kind, str8_cstr(kind)) || !str8_equal(allow[i].name, name)) continue;
        allow[i].matched = 1;
        if (!covered) allow[i].used = 1;
        return 1;
    }
    return 0;
}

// The end of --test, after the input harness: counting off, every zero and every side effect reported. The number of
// failures.
i32 coverage_check(Coverage *c) {
    Arena *arena = c->arena;
    os_dev_cover_enable(0);
    i32 failures = 0;
    String8 root = str8_fmt(arena, "%S\\..", os_exe_dir(arena));
    CoverageAllow *allow = PUSH_ARRAY(arena, CoverageAllow, COVERAGE_ALLOW_CAP);
    i32 allow_count = MAX(0, coverage_allow_read(arena, root, allow, COVERAGE_ALLOW_CAP, &failures));
    i32 allowed = 0;

    // Commands.
    i32 commands = 0;
    for (i32 i = 0; i < command_count(); i++) {
        String8 name = str8_cstr(command_at(i)->name);
        b32 covered = os_dev_cover_count("command", str8(NULL, 0), name, 0) > 0;
        commands += covered;
        if (coverage_allowed(allow, allow_count, "command", name, covered)) allowed += !covered;
        else if (!covered) LOG("test: FAIL: coverage: command %S was never run by a test", name), failures++;
    }

    // The default bindings of every keymap: pressed by any test, and through the input harness.
    Config *cfg = PUSH_STRUCT(arena, Config);
    config_defaults(cfg, arena);
    i32 bindings = 0, bindings_real = 0, binding_total = 0;
    String8 per_map = str8(NULL, 0);
    for (i32 m = 0; m < ARRAY_COUNT(dump_sections); m++) {
        Keymap *map = (Keymap *)((u8 *)cfg + dump_sections[m].offset);
        String8 map_name = str8_cstr(map->name);
        i32 any_n = 0, real_n = 0;
        for (i32 i = 0; i < map->count; i++) {
            u8 text[KEY_SEQ_TEXT_CAP];
            String8 seq = str8(text, key_seq_print(&map->bindings[i].seq, text, sizeof(text)));
            String8 name = str8_fmt(arena, "%S %S", map_name, seq);
            b32 any = os_dev_cover_count("binding", map_name, seq, 0) > 0, real = os_dev_cover_count("binding", map_name, seq, 1) > 0;
            any_n += any;
            real_n += real;
            if (coverage_allowed(allow, allow_count, "binding", name, any)) allowed += !any;
            else if (!any) LOG("test: FAIL: coverage: binding %S (%S) was never pressed by a test", seq, map_name), failures++;
            if (coverage_allowed(allow, allow_count, "binding-real", name, real)) allowed += !real;
            else if (!real) LOG("test: FAIL: coverage: binding %S (%S) was never pressed through the input harness", seq, map_name), failures++;
        }
        bindings += any_n, bindings_real += real_n, binding_total += map->count;
        per_map = str8_fmt(arena, "%S%s%S %d/%d (real %d)", per_map, m ? ", " : "", map_name, any_n, map->count, real_n);
    }

    // Settings: their effect checked through TEST_SETTING_EFFECT.
    i32 settings = 0;
    for (i32 i = 0; i < config_dev_setting_count(); i++) {
        String8 name = config_dev_setting_name(i);
        b32 covered = os_dev_cover_count("setting", str8(NULL, 0), name, 0) > 0;
        settings += covered;
        if (coverage_allowed(allow, allow_count, "setting", name, covered)) allowed += !covered;
        else if (!covered) LOG("test: FAIL: coverage: setting %S: no test checked its effect", name), failures++;
    }

    // The list itself: every entry names an item, and one still not covered.
    for (i32 i = 0; i < allow_count; i++) {
        if (!allow[i].matched) {
            LOG("test: FAIL: coverage: %s:%d: no %S named '%S'", COVERAGE_ALLOW_FILE, allow[i].line, allow[i].kind, allow[i].name);
            failures++;
        } else if (!allow[i].used) {
            LOG("test: FAIL: coverage: %s:%d: %S %S is covered now: remove it from the allow-list", COVERAGE_ALLOW_FILE, allow[i].line,
                allow[i].kind, allow[i].name);
            failures++;
        } else {
            LOG("test: coverage: allowed: %S %S (%S, %S)", allow[i].kind, allow[i].name, allow[i].reason, allow[i].issue);
        }
    }

    // The table kept every hit.
    String8 first;
    i32 used, slots;
    u64 dropped = os_dev_cover_dropped(&first, &used, &slots);
    if (dropped) {
        if (first.len >= 63) LOG("test: FAIL: coverage: a key longer than 63 bytes: %S...", first);
        else LOG("test: FAIL: coverage: the counter table is full (%d slots, %U hits dropped, first: %S)", slots, dropped, first);
        failures++;
    }

    // What --test must leave as it was. The clipboard: ronin's own calls only; another program copying meanwhile
    // changes Windows' sequence number, which is logged and never a failure.
    u32 seq, calls = os_dev_clipboard_real(&seq);
    if (calls != c->clip_calls) {
        LOG("test: FAIL: the real clipboard was used %d time(s) during --test (opened, read or written by ronin)", (i32)(calls - c->clip_calls));
        failures++;
    }
    if (seq != c->clip_seq) LOG("test: coverage: the clipboard changed during the run, not by ronin (another program)");
    for (i32 i = 0; i < 2; i++) {
        String8 now = coverage_tree(arena, i), was = c->trees[i];
        if (str8_equal(now, was)) continue;
        i64 at = 0;
        while (at < now.len && at < was.len && now.data[at] == was.data[at]) at++;
        while (at > 0 && now.data[at - 1] != '\n') at--;
        i64 now_end = at, was_end = at;
        while (now_end < now.len && now.data[now_end] != '\n') now_end++;
        while (was_end < was.len && was.data[was_end] != '\n') was_end++;
        LOG("test: FAIL: %%%s%%\\ronin changed during --test: now '%S', before '%S' (an edit in a running ronin meanwhile? run again)",
            coverage_tree_vars[i], str8(now.data + at, now_end - at), str8(was.data + at, was_end - at));
        failures++;
    }
    if (os_dev_real_refused()) {
        LOG("test: FAIL: %d real process(es) refused during --test (git, terminal or build)", os_dev_real_refused());
        failures++;
    }

    LOG("test: coverage: commands %d/%d, bindings %d/%d (%S), real %d/%d, settings %d/%d; allowed %d; %d/%d counter slots",
        commands, command_count(), bindings, binding_total, per_map, bindings_real, binding_total, settings, config_dev_setting_count(),
        allowed, used, slots);
    LOG("test: coverage: %s, %d failure(s)", failures ? "FAIL" : "PASS", failures);
    return failures;
}

#endif // RONIN_DEV
