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

OsFileStatus project_file_read(Arena *arena, String8 root, ProjectFile *out) {
    memset(out, 0, sizeof(*out));
    String8 path = str8_fmt(arena, "%S\\%s", root, PROJECT_FILE_NAME);
    String8 text;
    OsFileInfo info;
    OsFileStatus status = config_read_file(arena, path, &text, &info);
    if (status == OS_FILE_OK) project_file_parse(arena, text, STR8_LIT(PROJECT_FILE_NAME), out);
    return status;
}

// ---------------------------------------------------------------------------
// The index

void project_index_init(ProjectIndex *x) {
    memset(x, 0, sizeof(*x));
    x->dirs = arena_create(MB(256));
    x->files = arena_create(GB(1));
    x->names = arena_create(GB(2));
}

void project_index_reset(ProjectIndex *x) {
    arena_reset(&x->dirs);
    arena_reset(&x->files);
    arena_reset(&x->names);
    x->dir_count = x->file_count = 0;
}

void project_index_destroy(ProjectIndex *x) {
    if (x->dirs.base) os_release(x->dirs.base);
    if (x->files.base) os_release(x->files.base);
    if (x->names.base) os_release(x->names.base);
    memset(x, 0, sizeof(*x));
}

u64 project_index_memory(ProjectIndex *x) {
    return x->dirs.committed + x->files.committed + x->names.committed;
}

i64 project_index_add_dir(ProjectIndex *x, String8 rel) {
    u8 *bytes = PUSH_ARRAY(&x->names, u8, rel.len);
    memcpy(bytes, rel.data, (size_t)rel.len);
    ProjectDir *d = PUSH_STRUCT(&x->dirs, ProjectDir);
    d->path_off = (u32)(bytes - x->names.base);
    d->path_len = (u32)rel.len;
    return x->dir_count++;
}

void project_index_add_file(ProjectIndex *x, i64 dir, String8 name, u64 mask) {
    u8 *bytes = (u8 *)arena_push(&x->names, (u64)name.len + 2, 1);
    u16 len = (u16)name.len;
    memcpy(bytes, &len, 2);
    memcpy(bytes + 2, name.data, (size_t)name.len);
    ProjectEntry *e = PUSH_STRUCT(&x->files, ProjectEntry);
    e->mask = mask;
    e->dir = (u32)dir;
    e->name_off = (u32)(bytes - x->names.base);
    x->file_count++;
}

String8 project_index_dir(ProjectIndex *x, i64 dir) {
    ProjectDir *d = (ProjectDir *)x->dirs.base + dir;
    return str8(x->names.base + d->path_off, d->path_len);
}

String8 project_index_name(ProjectIndex *x, i64 file) {
    ProjectEntry *e = (ProjectEntry *)x->files.base + file;
    u16 len;
    memcpy(&len, x->names.base + e->name_off, 2);
    return str8(x->names.base + e->name_off + 2, len);
}

i64 project_index_path(ProjectIndex *x, i64 file, u8 *buf, i64 cap) {
    ProjectEntry *e = (ProjectEntry *)x->files.base + file;
    String8 dir = project_index_dir(x, e->dir), name = project_index_name(x, file);
    i64 len = dir.len + (dir.len ? 1 : 0) + name.len;
    if (len > cap) return 0;
    memcpy(buf, dir.data, (size_t)dir.len);
    if (dir.len) buf[dir.len] = '/';
    memcpy(buf + len - name.len, name.data, (size_t)name.len);
    return len;
}

u64 project_mask(String8 s) {
    u64 m = 0;
    for (i64 i = 0; i < s.len; i++) {
        u8 c = s.data[i];
        if (c >= 'A' && c <= 'Z') c = (u8)(c + 32);
        m |= c >= 'a' && c <= 'z' ? 1ull << (c - 'a')
           : c >= '0' && c <= '9' ? 1ull << (26 + c - '0')
           : c == '_' ? 1ull << 36 : c == '-' ? 1ull << 37 : c == '.' ? 1ull << 38 : c == '/' ? 1ull << 39
           : c == ' ' ? 1ull << 40 : c < 0x80 ? 1ull << 41 : 1ull << 42;
    }
    return m;
}

// ---------------------------------------------------------------------------
// The scanner. Everything from project_scan_main down to the queue runs on the worker: it touches
// only the ProjectScan (its request read-only, the lists under the lock), its two arenas and the
// os_* functions platform.h marks "any thread". No LOG, no ASSERT.

enum { PROJECT_RECORD_DIR = 1, PROJECT_RECORD_FILE = 2 };

// A directory waiting to be walked.
typedef struct ProjectQueued {
    struct ProjectQueued *next;
    String8 rel;
    const IgnoreScope *chain; // its .gitignore scopes, innermost first
    i32 depth;
} ProjectQueued;

// The worker's state while it walks.
typedef struct ProjectWalk {
    ProjectScan *s;
    ProjectBatch *batch;
    i32 batches;    // allocated
    i32 sent;       // batches handed over
    i64 entries;    // directory entries looked at
    b32 dir_sent;   // the current directory's record is in the stream
    String8 dir_rel;
    u64 dir_mask;
    ProjectScanResult result;
} ProjectWalk;

static b32 project_cancelled(ProjectScan *s) {
    return os_atomic_get(&s->cancel) != 0;
}

// Hands the current batch to the main thread and takes an empty one (allocated while fewer than
// PROJECT_BATCH_POOL exist, else one that came back). False when cancelled while waiting.
static b32 project_send(ProjectWalk *w, b32 last) {
    ProjectScan *s = w->s;
    ProjectBatch *b = w->batch;
    b->last = last;
    if (last) b->result = w->result;
    os_lock(&s->lock);
    if (s->ready_last) s->ready_last->next = b;
    else s->ready = b;
    s->ready_last = b;
    os_unlock(&s->lock);
    os_wake_main();
    w->batch = NULL;
    w->sent++;
    if (last) return 1;
    // Dev: stand still after some batches until released or cancelled (the scan "still running").
    if (s->dev_hold_after && w->sent >= s->dev_hold_after) {
        while (os_atomic_get(&s->dev_hold) && !project_cancelled(s)) os_event_wait(s->event, 0xFFFFFFFFu);
    }
    for (;;) {
        os_lock(&s->lock);
        ProjectBatch *f = s->free;
        if (f) s->free = f->next;
        os_unlock(&s->lock);
        if (!f && w->batches < PROJECT_BATCH_POOL) {
            f = PUSH_STRUCT(&s->arena, ProjectBatch);
            w->batches++;
        }
        if (f) {
            f->next = NULL;
            f->used = 0;
            w->batch = f;
            return !project_cancelled(s);
        }
        if (project_cancelled(s)) return 0;
        os_event_wait(s->event, 0xFFFFFFFFu); // a batch comes back, or a cancel
    }
}

static b32 project_emit(ProjectWalk *w, u8 kind, String8 text, u64 mask) {
    i64 need = 3 + (kind == PROJECT_RECORD_FILE ? 8 : 0) + text.len;
    if (w->batch->used + need > PROJECT_BATCH_BYTES && !project_send(w, 0)) return 0;
    u8 *at = w->batch->data + w->batch->used;
    u16 len = (u16)text.len;
    at[0] = kind;
    memcpy(at + 1, &len, 2);
    at += 3;
    if (kind == PROJECT_RECORD_FILE) {
        memcpy(at, &mask, 8);
        at += 8;
    }
    memcpy(at, text.data, (size_t)text.len);
    w->batch->used += need;
    return 1;
}

// The entries of one directory: rules, files out, subdirectories queued. False: stop (cancelled, limit).
static b32 project_walk_dir(ProjectWalk *w, ProjectQueued *q, const IgnoreScope *top, const IgnoreScope *builtin,
                            ProjectQueued **tail) {
    ProjectScan *s = w->s;
    Arena *temp = &s->temp;
    arena_reset(temp);
    String8 full = s->root;
    if (q->rel.len) {
        u8 *p = PUSH_ARRAY(temp, u8, s->root.len + 1 + q->rel.len);
        memcpy(p, s->root.data, (size_t)s->root.len);
        p[s->root.len] = '\\';
        for (i64 i = 0; i < q->rel.len; i++) p[s->root.len + 1 + i] = q->rel.data[i] == '/' ? '\\' : q->rel.data[i];
        full = str8(p, s->root.len + 1 + q->rel.len);
    }
    OsDirEntry *e;
    i64 n;
    if (os_list_dir(temp, full, &e, &n) != OS_FILE_OK) {
        w->result.errors++;
        return 1;
    }
    w->result.dirs++;
    // Its .gitignore first: it applies to everything here and below.
    const IgnoreScope *chain = q->chain;
    for (i64 i = 0; i < n; i++) {
        if (e[i].is_dir || e[i].name.len != 10 || !ignore_glob(STR8_LIT(".gitignore"), e[i].name)) continue;
        String8 text;
        if (os_read_small_file(temp, str8_fmt(temp, "%S\\.gitignore", full), PROJECT_GITIGNORE_MAX, &text) != OS_FILE_OK) break;
        IgnoreScope *scope = PUSH_STRUCT(&s->arena, IgnoreScope);
        scope->parent = chain;
        scope->base = str8_copy(&s->arena, q->rel);
        ignore_parse_lines(&s->arena, scope, text, 0);
        if (scope->count) chain = scope;
        break;
    }
    w->dir_sent = 0;
    w->dir_rel = q->rel;
    w->dir_mask = project_mask(q->rel) | (q->rel.len ? project_mask(STR8_LIT("/")) : 0);
    u8 rel[PROJECT_PATH_MAX];
    for (i64 i = 0; i < n; i++) {
        if (project_cancelled(s)) return 0;
        w->entries++;
        if (s->dev_hold_entries && w->entries == s->dev_hold_entries) {
            while (os_atomic_get(&s->dev_hold) && !project_cancelled(s)) os_event_wait(s->event, 0xFFFFFFFFu);
        }
        String8 name = e[i].name;
        i64 len = q->rel.len + (q->rel.len ? 1 : 0) + name.len;
        if (len > PROJECT_PATH_MAX || name.len > 0xFFFF) {
            w->result.errors++;
            continue;
        }
        memcpy(rel, q->rel.data, (size_t)q->rel.len);
        if (q->rel.len) rel[q->rel.len] = '/';
        memcpy(rel + len - name.len, name.data, (size_t)name.len);
        String8 path = str8(rel, len);
        if (e[i].is_dir) {
            if (e[i].reparse || q->depth + 1 > PROJECT_MAX_DEPTH) continue; // a symlink or junction: not followed
            if (ignore_path(top, chain, builtin, path, 1)) {
                w->result.ignored++;
                continue;
            }
            ProjectQueued *child = PUSH_STRUCT(&s->arena, ProjectQueued);
            child->rel = str8_copy(&s->arena, path);
            child->chain = chain;
            child->depth = q->depth + 1;
            (*tail)->next = child;
            *tail = child;
            continue;
        }
        if (ignore_path(top, chain, builtin, path, 0)) {
            w->result.ignored++;
            continue;
        }
        if (w->result.files >= s->max_files) {
            w->result.limit = 1;
            return 0;
        }
        if (!w->dir_sent) {
            if (!project_emit(w, PROJECT_RECORD_DIR, w->dir_rel, 0)) return 0;
            w->dir_sent = 1;
        }
        if (!project_emit(w, PROJECT_RECORD_FILE, name, w->dir_mask | project_mask(name))) return 0;
        w->result.files++;
    }
    return 1;
}

static void project_scan_main(void *data) {
    ProjectScan *s = data;
    // Dev: a worker stuck in the file system, deaf to cancel, until the test releases it.
    while (os_atomic_get(&s->dev_stuck)) os_event_wait(s->event, 0xFFFFFFFFu);
    ProjectWalk w = { .s = s };
    IgnoreScope *builtin = PUSH_STRUCT(&s->arena, IgnoreScope);
    ignore_parse_words(&s->arena, builtin, s->builtin);
    // .teal-project as one scope: [ignore], then [allow] as negations after it (the later patterns win).
    IgnoreScope ign = { 0 }, all = { 0 };
    ignore_parse_lines(&s->arena, &ign, s->ignore, 0);
    ignore_parse_lines(&s->arena, &all, s->allow, 1);
    IgnoreScope *top = PUSH_STRUCT(&s->arena, IgnoreScope);
    top->patterns = PUSH_ARRAY(&s->arena, IgnorePattern, ign.count + all.count);
    for (i32 i = 0; i < ign.count; i++) top->patterns[top->count++] = ign.patterns[i];
    for (i32 i = 0; i < all.count; i++) top->patterns[top->count++] = all.patterns[i];
    w.batch = PUSH_STRUCT(&s->arena, ProjectBatch);
    w.batches = 1;
    ProjectQueued *head = PUSH_STRUCT(&s->arena, ProjectQueued), *tail = head;
    for (ProjectQueued *q = head; q; q = q->next) { // breadth first: the queue grows at its tail
        if (project_cancelled(s) || !project_walk_dir(&w, q, top, builtin, &tail)) break;
    }
    w.result.cancelled = project_cancelled(s) && !w.result.limit;
    if (!w.batch) { // cancelled while waiting for a batch: take one back, or make one
        os_lock(&s->lock);
        w.batch = s->free;
        if (w.batch) s->free = w.batch->next;
        os_unlock(&s->lock);
        if (!w.batch) w.batch = PUSH_STRUCT(&s->arena, ProjectBatch);
        w.batch->next = NULL;
        w.batch->used = 0;
    }
    project_send(&w, 1);
}

// ---------------------------------------------------------------------------
// The scanner, the main thread's side

ProjectScan *project_scan_create(String8 root, String8 builtin, ProjectFile *file, i64 max_files) {
    Arena arena = arena_create(GB(1));
    ProjectScan *s = PUSH_STRUCT(&arena, ProjectScan);
    s->root = str8_copy(&arena, root);
    s->builtin = str8_copy(&arena, builtin);
    if (file) {
        s->ignore = str8_copy(&arena, file->ignore);
        s->allow = str8_copy(&arena, file->allow);
    }
    s->max_files = max_files;
    s->drain_dir = -1;
    s->arena = arena; // its position already counts this struct
    s->temp = arena_create(GB(1));
    return s;
}

b32 project_scan_start(ProjectScan *s) {
    if (!os_event_create(&s->event)) return 0;
    if (!os_thread_start(&s->thread, project_scan_main, s)) {
        os_event_destroy(s->event);
        s->event.handle = NULL;
        return 0;
    }
    return 1;
}

void project_scan_cancel(ProjectScan *s) {
    os_atomic_set(&s->cancel, 1);
    os_event_set(s->event);
}

i64 project_scan_drain(ProjectScan *s, ProjectIndex *x) {
    if (s->ended || !s->thread.handle) return 0;
    os_lock(&s->lock);
    ProjectBatch *b = s->ready;
    s->ready = s->ready_last = NULL;
    os_unlock(&s->lock);
    i64 added = 0;
    b32 last = 0;
    ProjectBatch *back = NULL;
    while (b) {
        ProjectBatch *next = b->next;
        for (i64 at = 0; at < b->used;) {
            u8 kind = b->data[at];
            u16 len;
            memcpy(&len, b->data + at + 1, 2);
            at += 3;
            if (kind == PROJECT_RECORD_DIR) {
                s->drain_dir = project_index_add_dir(x, str8(b->data + at, len));
            } else {
                u64 mask;
                memcpy(&mask, b->data + at, 8);
                at += 8;
                project_index_add_file(x, s->drain_dir, str8(b->data + at, len), mask);
                added++;
            }
            at += len;
        }
        if (b->last) {
            last = 1;
            s->result = b->result;
        } else {
            b->next = back;
            back = b;
        }
        b = next;
    }
    if (back) {
        os_lock(&s->lock);
        for (ProjectBatch *r = back; r;) {
            ProjectBatch *next = r->next;
            r->next = s->free;
            s->free = r;
            r = next;
        }
        os_unlock(&s->lock);
        os_event_set(s->event);
    }
    if (last) { // the worker has returned from its last os_* call: the join does not wait
        os_thread_join(s->thread, 0xFFFFFFFFu);
        s->thread.handle = NULL;
        s->ended = 1;
    }
    return added;
}

b32 project_scan_join(ProjectScan *s, u32 timeout_ms) {
    if (!s->thread.handle) return 1;
    if (!os_thread_join(s->thread, timeout_ms)) return 0;
    s->thread.handle = NULL;
    s->ended = 1;
    return 1;
}

void project_scan_free(ProjectScan *s) {
    os_event_destroy(s->event);
    os_release(s->temp.base);
    os_release(s->arena.base); // the struct itself lives there
}
