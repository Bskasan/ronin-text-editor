// project.c — see project.h.

// ---------------------------------------------------------------------------
// Ignore rules

static u8 ignore_lower(u8 c) {
    return c >= 'A' && c <= 'Z' ? (u8)(c + 32) : c;
}

static b32 ignore_is_blank(u8 c) {
    return c == ' ' || c == '\t' || c == '\r';
}

// One pattern: "!" negates, a trailing "/" means directories only, a leading or inner "/" anchors.
static void ignore_add(Arena *arena, IgnorePattern *out, i32 *count, String8 p, b32 negate_all) {
    while (p.len && ignore_is_blank(p.data[p.len - 1])) p.len--;
    while (p.len && ignore_is_blank(p.data[0])) { p.data++; p.len--; }
    if (!p.len || p.data[0] == '#') return;
    u32 flags = negate_all ? IGNORE_NEGATE : 0;
    if (p.data[0] == '!') {
        flags ^= IGNORE_NEGATE;
        p.data++;
        p.len--;
    }
    if (p.len && p.data[p.len - 1] == '/') {
        flags |= IGNORE_DIR_ONLY;
        p.len--;
    }
    for (i64 i = 0; i < p.len; i++) if (p.data[i] == '/') flags |= IGNORE_ANCHORED;
    if (p.len && p.data[0] == '/') {
        p.data++;
        p.len--;
    }
    if (!p.len) return;
    u8 *glob = PUSH_ARRAY(arena, u8, p.len);
    for (i64 i = 0; i < p.len; i++) glob[i] = ignore_lower(p.data[i]);
    out[(*count)++] = (IgnorePattern){ str8(glob, p.len), flags };
}

void ignore_parse_lines(Arena *arena, IgnoreScope *scope, String8 text, b32 negate_all) {
    i64 lines = 1;
    for (i64 i = 0; i < text.len; i++) lines += text.data[i] == '\n';
    IgnorePattern *patterns = PUSH_ARRAY(arena, IgnorePattern, lines);
    i32 count = 0;
    for (i64 i = 0; i < text.len;) {
        i64 end = i;
        while (end < text.len && text.data[end] != '\n') end++;
        ignore_add(arena, patterns, &count, str8(text.data + i, end - i), negate_all);
        i = end + 1;
    }
    scope->patterns = patterns;
    scope->count = count;
}

void ignore_parse_words(Arena *arena, IgnoreScope *scope, String8 words) {
    i64 n = 1;
    for (i64 i = 0; i < words.len; i++) n += ignore_is_blank(words.data[i]) || words.data[i] == '\n';
    IgnorePattern *patterns = PUSH_ARRAY(arena, IgnorePattern, n);
    i32 count = 0;
    for (i64 i = 0; i < words.len;) {
        while (i < words.len && (ignore_is_blank(words.data[i]) || words.data[i] == '\n')) i++;
        i64 start = i;
        while (i < words.len && !ignore_is_blank(words.data[i]) && words.data[i] != '\n') i++;
        if (i > start) ignore_add(arena, patterns, &count, str8(words.data + start, i - start), 0);
    }
    scope->patterns = patterns;
    scope->count = count;
}

// The glob from g[gi] against s from s[si]. '*' and '?' stay inside one path segment; "**" as a whole
// segment matches any number of segments ("**/x", "a/**/b") or everything below ("a/**").
static b32 ignore_glob_from(String8 g, i64 gi, String8 s, i64 si) {
    while (gi < g.len) {
        u8 c = g.data[gi];
        if (c == '*') {
            b32 segment = gi == 0 || g.data[gi - 1] == '/';
            if (gi + 1 < g.len && g.data[gi + 1] == '*' && segment && (gi + 2 == g.len || g.data[gi + 2] == '/')) {
                if (gi + 2 == g.len) return 1;
                for (i64 k = si;;) { // zero or more whole directories, then the rest
                    if (ignore_glob_from(g, gi + 3, s, k)) return 1;
                    while (k < s.len && s.data[k] != '/') k++;
                    if (k >= s.len) return 0;
                    k++;
                }
            }
            i64 next = gi + 1;
            while (next < g.len && g.data[next] == '*') next++;
            for (i64 k = si;; k++) {
                if (ignore_glob_from(g, next, s, k)) return 1;
                if (k >= s.len || s.data[k] == '/') return 0;
            }
        }
        if (si >= s.len) return 0;
        if (c == '?' ? s.data[si] == '/' : c != ignore_lower(s.data[si])) return 0;
        gi++;
        si++;
    }
    return si == s.len;
}

b32 ignore_glob(String8 glob, String8 s) {
    return ignore_glob_from(glob, 0, s, 0);
}

i32 ignore_scope_decide(const IgnoreScope *scope, String8 rel, b32 is_dir) {
    String8 sub = rel;
    if (scope->base.len) {
        if (rel.len <= scope->base.len || rel.data[scope->base.len] != '/' || !mem_equal(rel.data, scope->base.data, scope->base.len)) return -1;
        sub = str8(rel.data + scope->base.len + 1, rel.len - scope->base.len - 1);
    }
    i64 slash = sub.len;
    while (slash > 0 && sub.data[slash - 1] != '/') slash--;
    String8 name = str8(sub.data + slash, sub.len - slash);
    for (i32 i = scope->count - 1; i >= 0; i--) { // the last matching pattern decides
        const IgnorePattern *p = &scope->patterns[i];
        if ((p->flags & IGNORE_DIR_ONLY) && !is_dir) continue;
        if (ignore_glob(p->glob, (p->flags & IGNORE_ANCHORED) ? sub : name)) return (p->flags & IGNORE_NEGATE) ? 0 : 1;
    }
    return -1;
}

b32 ignore_path(const IgnoreScope *top, const IgnoreScope *chain, const IgnoreScope *builtin, String8 rel, b32 is_dir) {
    i32 d = top ? ignore_scope_decide(top, rel, is_dir) : -1;
    for (const IgnoreScope *s = chain; d < 0 && s; s = s->parent) d = ignore_scope_decide(s, rel, is_dir);
    if (d < 0 && builtin) d = ignore_scope_decide(builtin, rel, is_dir);
    return d == 1;
}

// ---------------------------------------------------------------------------
// Roots

static b32 project_is_sep(u8 c) {
    return c == '\\' || c == '/';
}

static String8 project_strip_seps(String8 s) {
    while (s.len && project_is_sep(s.data[s.len - 1])) s.len--;
    return s;
}

b32 project_is_drive_root(String8 dir) {
    dir = project_strip_seps(dir);
    if (dir.len == 2 && dir.data[1] == ':') return 1;
    if (dir.len > 2 && project_is_sep(dir.data[0]) && project_is_sep(dir.data[1])) { // \\server\share
        i32 seps = 0;
        for (i64 i = 2; i < dir.len; i++) seps += project_is_sep(dir.data[i]);
        return seps <= 1;
    }
    return 0;
}

b32 project_path_inside(String8 path, String8 dir) {
    dir = project_strip_seps(dir);
    if (!dir.len || path.len < dir.len) return 0;
    for (i64 i = 0; i < dir.len; i++) {
        u8 a = path.data[i], b = dir.data[i];
        if (project_is_sep(a) && project_is_sep(b)) continue;
        if (ignore_lower(a) != ignore_lower(b)) return 0;
    }
    return path.len == dir.len || project_is_sep(path.data[dir.len]);
}

String8 project_root_find(Arena *arena, String8 dir, String8 markers, ProjectExistsFn *exists, void *data) {
    String8 cur = project_strip_seps(dir);
    while (cur.len && !project_is_drive_root(cur)) {
        for (i64 i = 0; i < markers.len;) {
            while (i < markers.len && ignore_is_blank(markers.data[i])) i++;
            i64 start = i;
            while (i < markers.len && !ignore_is_blank(markers.data[i])) i++;
            if (i == start) continue;
            u64 mark = arena_pos(arena);
            String8 path = str8_fmt(arena, "%S\\%S", cur, str8(markers.data + start, i - start));
            b32 found = exists(data, path);
            arena_pop_to(arena, mark);
            if (found) return str8_copy(arena, cur);
        }
        i64 sep = cur.len;
        while (sep > 0 && !project_is_sep(cur.data[sep - 1])) sep--;
        if (sep == 0) break;
        cur = project_strip_seps(str8(cur.data, sep));
    }
    return str8(NULL, 0);
}

b32 project_exists_fs(void *data, String8 path) {
    (void)data;
    OsFileInfo info;
    return os_file_info(path, &info) == OS_FILE_OK && info.exists;
}

void project_cache_init(ProjectRootCache *c) {
    memset(c, 0, sizeof(*c));
    c->arena = arena_create(MB(16));
}

void project_cache_reset(ProjectRootCache *c) {
    arena_reset(&c->arena);
    c->count = 0;
}

void project_cache_destroy(ProjectRootCache *c) {
    os_release(c->arena.base);
}

String8 project_root_for(ProjectRootCache *c, String8 file_dir, String8 cmdline_dir, String8 markers, ProjectExistsFn *exists,
                         void *data) {
    if (!file_dir.len) return cmdline_dir;
    String8 root = { 0 };
    i32 found = -1;
    for (i32 i = 0; i < c->count && found < 0; i++) if (str8_equal(c->dirs[i], file_dir)) found = i;
    if (found >= 0) {
        root = c->roots[found];
    } else {
        if (c->count == PROJECT_ROOT_CACHE) project_cache_reset(c);
        c->queries++;
        root = project_root_find(&c->arena, file_dir, markers, exists, data);
        c->dirs[c->count] = str8_copy(&c->arena, file_dir);
        c->roots[c->count++] = root;
    }
    if (root.len) return root;
    if (cmdline_dir.len && project_path_inside(file_dir, cmdline_dir)) return cmdline_dir;
    return str8(NULL, 0);
}

// ---------------------------------------------------------------------------
// .teal-project

typedef enum ProjectSection {
    PROJECT_SECTION_NONE,
    PROJECT_SECTION_UNKNOWN,
    PROJECT_SECTION_IGNORE,
    PROJECT_SECTION_ALLOW,
} ProjectSection;

// The patterns of a section joined with '\n'.
static String8 project_join(Arena *arena, String8 *lines, i64 n) {
    i64 len = 0;
    for (i64 i = 0; i < n; i++) len += lines[i].len + 1;
    u8 *out = PUSH_ARRAY(arena, u8, len);
    i64 at = 0;
    for (i64 i = 0; i < n; i++) {
        memcpy(out + at, lines[i].data, (size_t)lines[i].len);
        at += lines[i].len;
        out[at++] = '\n';
    }
    return str8(out, len);
}

void project_file_parse(Arena *arena, String8 text, String8 file_name, ProjectFile *out) {
    memset(out, 0, sizeof(*out));
    ConfigParser p = { .diags = &out->diags, .arena = arena, .file = file_name, .text = text };
    i64 cap = 1;
    for (i64 i = 0; i < text.len; i++) cap += text.data[i] == '\n';
    String8 *ignore = PUSH_ARRAY(arena, String8, cap), *allow = PUSH_ARRAY(arena, String8, cap);
    i64 n_ignore = 0, n_allow = 0;
    ProjectSection section = PROJECT_SECTION_NONE;
    String8 line;
    while (config_next_line(&p, &line)) {
        String8 head;
        i32 header = config_header(line, &head);
        if (header) {
            section = header < 0 ? PROJECT_SECTION_UNKNOWN
                    : str8_equal(head, STR8_LIT("ignore")) ? PROJECT_SECTION_IGNORE
                    : str8_equal(head, STR8_LIT("allow"))  ? PROJECT_SECTION_ALLOW : PROJECT_SECTION_UNKNOWN;
            if (header < 0) config_diag(&p, 0, "bad section header (expected [ignore] or [allow])");
            else if (section == PROJECT_SECTION_UNKNOWN) config_diag(&p, 0, "unknown section [%S] (expected [ignore] or [allow])", config_quote(head));
            continue;
        }
        if (section == PROJECT_SECTION_NONE) {
            config_diag(&p, 0, "line outside a section ([ignore] or [allow])");
            continue;
        }
        if (section == PROJECT_SECTION_UNKNOWN) continue;
        for (i64 i = 0; i < line.len; i++) {
            if (line.data[i] == '[' || line.data[i] == '\\') {
                config_diag(&p, 1, "'%S': character classes and backslash escapes are not supported (matched literally)", config_quote(line));
                break;
            }
        }
        if (section == PROJECT_SECTION_IGNORE) ignore[n_ignore++] = line;
        else allow[n_allow++] = line;
    }
    out->ignore = project_join(arena, ignore, n_ignore);
    out->allow = project_join(arena, allow, n_allow);
}
