// compile.c — see compile.h. Builds, the worker side.

// ---------------------------------------------------------------------------
// Matching

static b32 compile_is_digit(u8 c) {
    return c >= '0' && c <= '9';
}

static b32 compile_is_alpha(u8 c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

static u8 compile_lower(u8 c) {
    return c >= 'A' && c <= 'Z' ? (u8)(c + 32) : c;
}

// Digits at s[at]: their end (== at when none); the value saturates.
static i64 compile_digits(String8 s, i64 at, i64 *value) {
    i64 v = 0, i = at;
    for (; i < s.len && compile_is_digit(s.data[i]); i++) v = MIN(v * 10 + (s.data[i] - '0'), (i64)1 << 40);
    *value = v;
    return i;
}

// `w` at s[at], ASCII case-insensitively.
static b32 compile_word_at(String8 s, i64 at, const char *w) {
    i64 i = 0;
    for (; w[i]; i++) {
        if (at + i >= s.len || compile_lower(s.data[at + i]) != compile_lower((u8)w[i])) return 0;
    }
    return 1;
}

// A kind word at s[at] followed by the end, a blank or ':'; its length (0: none).
static i64 compile_kind_word(String8 s, i64 at, CompileKind *kind) {
    static const struct { const char *word; CompileKind kind; } words[] = {
        { "fatal error", COMPILE_ERROR }, { "error", COMPILE_ERROR }, { "warning", COMPILE_WARNING },
        { "note", COMPILE_INFO },         { "info", COMPILE_INFO },
    };
    for (i32 i = 0; i < ARRAY_COUNT(words); i++) {
        if (!compile_word_at(s, at, words[i].word)) continue;
        i64 n = 0;
        while (words[i].word[n]) n++;
        u8 next = at + n < s.len ? s.data[at + n] : ' ';
        if (next != ' ' && next != ':' && next != '\t') continue;
        *kind = words[i].kind;
        return n;
    }
    return 0;
}

// A plausible path in s[from, to): not empty, no blank at either end, no character Windows forbids in a
// path, no "://" (a URL), and no ':' but a drive letter's.
static b32 compile_path_ok(String8 s, i64 from, i64 to) {
    if (to <= from) return 0;
    if (s.data[from] == ' ' || s.data[to - 1] == ' ' || s.data[from] == '\t') return 0;
    for (i64 i = from; i < to; i++) {
        u8 c = s.data[i];
        if (c < 0x20 || c == '<' || c == '>' || c == '|' || c == '"' || c == '?' || c == '*') return 0;
        if (c == ':' && !(i == from + 1 && compile_is_alpha(s.data[from]))) return 0;
    }
    return 1;
}

// Where the text of a line starts: past leading blanks and MSBuild's "N>" prefix.
static i64 compile_text_start(String8 s) {
    i64 i = 0;
    while (i < s.len && (s.data[i] == ' ' || s.data[i] == '\t')) i++;
    i64 v, d = compile_digits(s, i, &v);
    if (d > i && d < s.len && s.data[d] == '>') {
        i = d + 1;
        while (i < s.len && (s.data[i] == ' ' || s.data[i] == '\t')) i++;
    }
    return i;
}

// MSBuild's "[...proj]" at the end of a line (the path inside the brackets).
static void compile_project_file(String8 s, CompileMatch *m) {
    i64 end = s.len;
    while (end > 0 && (s.data[end - 1] == ' ' || s.data[end - 1] == '\t')) end--;
    if (end < 2 || s.data[end - 1] != ']') return;
    i64 open = end - 2;
    while (open >= 0 && s.data[open] != '[') open--;
    if (open < 0) return;
    i64 from = open + 1, to = end - 1;
    if (to - from < 6 || !compile_word_at(s, to - 4, "proj")) return;
    m->proj_off = (i32)from;
    m->proj_len = (i32)(to - from);
}

// "In file included from path:L[:C]:" / ",", and its continuations "                 from path:L:".
static b32 compile_match_include(String8 s, i64 start, CompileMatch *m) {
    i64 at;
    if (compile_word_at(s, start, "In file included from ")) at = start + 22;
    else if (start > 0 && compile_word_at(s, start, "from ")) at = start + 5;
    else return 0;
    for (i64 p = at + 1; p < s.len; p++) {
        if (s.data[p] != ':') continue;
        i64 line, col = 0;
        i64 q = compile_digits(s, p + 1, &line);
        if (q == p + 1) continue;
        if (q < s.len && s.data[q] == ':') {
            i64 r = compile_digits(s, q + 1, &col);
            if (r > q + 1) q = r;
        }
        if (q >= s.len || (s.data[q] != ':' && s.data[q] != ',') || !compile_path_ok(s, at, p)) return 0;
        for (i64 k = q + 1; k < s.len; k++) if (s.data[k] != ' ') return 0;
        *m = (CompileMatch){ .kind = COMPILE_INFO, .located = 1, .line = line, .col = col, .path_off = (i32)at, .path_len = (i32)(p - at),
                             .num_off = (i32)(p + 1), .num_len = (i32)(q - p - 1) };
        return 1;
    }
    return 0;
}

// MSVC and the tools that share its form: "path(L)", "path(L,C)" (up to four numbers), blanks, ':', blanks,
// a kind word. Every '(' is tried, so parentheses in the path ("Program Files (x86)") do not stop it.
static b32 compile_match_msvc(String8 s, i64 start, CompileMatch *m) {
    for (i64 p = start + 1; p < s.len; p++) {
        if (s.data[p] != '(') continue;
        i64 line, col = 0, v;
        i64 q = compile_digits(s, p + 1, &line);
        if (q == p + 1) continue;
        for (i32 k = 0; k < 3 && q < s.len && s.data[q] == ','; k++) {
            i64 r = compile_digits(s, q + 1, &v);
            if (r == q + 1) break;
            if (k == 0) col = v;
            q = r;
        }
        if (q >= s.len || s.data[q] != ')') continue;
        i64 close = q++;
        while (q < s.len && s.data[q] == ' ') q++;
        if (q >= s.len || s.data[q] != ':') continue;
        q++;
        while (q < s.len && s.data[q] == ' ') q++;
        CompileKind kind;
        i64 kn = compile_kind_word(s, q, &kind);
        if (!kn || !compile_path_ok(s, start, p)) continue;
        *m = (CompileMatch){ .kind = kind, .located = 1, .line = line, .col = col, .path_off = (i32)start, .path_len = (i32)(p - start),
                             .num_off = (i32)p, .num_len = (i32)(close + 1 - p), .kind_off = (i32)q, .kind_len = (i32)kn };
        return 1;
    }
    return 0;
}

// The colon forms: Jai "path:L,C: Error:", tsc "path:L:C - error", GCC "path:L:C: kind:", "path:L: kind:"
// and bare "path:L[:C]:" (an error, as Emacs).
static b32 compile_match_colon(String8 s, i64 start, CompileMatch *m) {
    for (i64 p = start + 1; p < s.len; p++) {
        if (s.data[p] != ':') continue;
        if (p == start + 1 && compile_is_alpha(s.data[start])) continue; // a drive letter
        if (!compile_path_ok(s, start, p)) return 0; // a ':' in the path: nothing further can match
        i64 line, col = 0;
        i64 q = compile_digits(s, p + 1, &line);
        if (q == p + 1) return 0;
        CompileMatch r = { .located = 1, .line = line, .path_off = (i32)start, .path_len = (i32)(p - start), .num_off = (i32)(p + 1) };
        if (q < s.len && s.data[q] == ',') { // Jai
            i64 c = compile_digits(s, q + 1, &col);
            if (c == q + 1 || c + 1 >= s.len || s.data[c] != ':' || s.data[c + 1] != ' ') return 0;
            CompileKind kind;
            i64 kn = compile_kind_word(s, c + 2, &kind);
            if (!kn || c + 2 + kn >= s.len || s.data[c + 2 + kn] != ':') return 0;
            r.kind = kind;
            r.col = col;
            r.num_len = (i32)(c - p - 1);
            r.kind_off = (i32)(c + 2);
            r.kind_len = (i32)kn;
            *m = r;
            return 1;
        }
        if (q >= s.len || s.data[q] != ':') return 0;
        i64 after = q + 1, c = compile_digits(s, q + 1, &col);
        if (c > q + 1) {
            if (c + 3 <= s.len && s.data[c] == ' ' && s.data[c + 1] == '-' && s.data[c + 2] == ' ') { // tsc's pretty form
                CompileKind kind;
                i64 kn = compile_kind_word(s, c + 3, &kind);
                if (!kn || kind == COMPILE_INFO) return 0;
                r.kind = kind;
                r.col = col;
                r.num_len = (i32)(c - p - 1);
                r.kind_off = (i32)(c + 3);
                r.kind_len = (i32)kn;
                *m = r;
                return 1;
            }
            if (c >= s.len || s.data[c] != ':') return 0;
            r.col = col;
            after = c + 1;
        } else {
            col = 0;
        }
        r.num_len = (i32)(after - 1 - p - 1);
        if (after < s.len && s.data[after] != ' ' && s.data[after] != '\t') return 0;
        CompileKind kind = COMPILE_ERROR;
        i64 kn = after + 1 < s.len ? compile_kind_word(s, after + 1, &kind) : 0;
        if (kn && (after + 1 + kn >= s.len || s.data[after + 1 + kn] != ':')) kn = 0, kind = COMPILE_ERROR;
        r.kind = kind;
        if (kn) {
            r.kind_off = (i32)(after + 1);
            r.kind_len = (i32)kn;
        }
        *m = r;
        return 1;
    }
    return 0;
}

b32 compile_match(String8 line, CompileMatch *m) {
    *m = (CompileMatch){ 0 };
    i64 start = compile_text_start(line);
    if (start >= line.len) return 0;
    b32 ok = compile_match_include(line, start, m) || compile_match_msvc(line, start, m) || compile_match_colon(line, start, m);
    if (ok) compile_project_file(line, m);
    return ok;
}

// A code after a kind word: letters then digits ("LNK2019", "C4100", "MSB4019", "CS8618", "D9025").
static i64 compile_code(String8 s, i64 at) {
    i64 i = at;
    while (i < s.len && compile_is_alpha(s.data[i])) i++;
    if (i == at) return at;
    i64 d = i;
    while (i < s.len && compile_is_digit(s.data[i])) i++;
    return i > d ? i : at;
}

// A kind word at `at` followed by a code then ':' (blanks allowed before the ':'); the kind word's length.
static i64 compile_kind_with_code(String8 s, i64 at, CompileKind *kind) {
    i64 kn = compile_kind_word(s, at, kind);
    if (!kn || *kind == COMPILE_INFO || at + kn >= s.len || s.data[at + kn] != ' ') return 0;
    i64 c = compile_code(s, at + kn + 1);
    if (c == at + kn + 1) return 0;
    while (c < s.len && s.data[c] == ' ') c++;
    return c < s.len && s.data[c] == ':' ? kn : 0;
}

b32 compile_match_kind(String8 line, CompileMatch *m) {
    *m = (CompileMatch){ 0 };
    String8 s = line;
    i64 start = compile_text_start(s);
    if (start >= s.len) return 0;
    CompileKind kind = COMPILE_NONE;
    i64 at = -1, kn = 0;
    // "<tool or file> : [command line ]error CODE: ..." (LINK, cl, an object file, MSBUILD).
    for (i64 p = start + 1; p + 3 <= s.len && at < 0; p++) {
        if (s.data[p] != ' ' || s.data[p + 1] != ':' || s.data[p + 2] != ' ') continue;
        i64 q = p + 3;
        if (compile_word_at(s, q, "command line ")) q += 13;
        kn = compile_kind_with_code(s, q, &kind);
        if (kn) at = q;
        break; // only the first " : "
    }
    // "error CODE: ..." at the start (MSBuild, csc without a file).
    if (at < 0 && (kn = compile_kind_with_code(s, start, &kind)) != 0) at = start;
    // "error: ..." / "warning: ..." / "note: ..." at the start, or after one "tool: " ("ld: error:").
    if (at < 0) {
        i64 q = start;
        for (i64 i = start; i < s.len; i++) {
            u8 c = s.data[i];
            if (compile_is_alpha(c) || compile_is_digit(c) || c == '_' || c == '.' || c == '+' || c == '-') continue;
            if (c == ':' && i > start && i + 1 < s.len && s.data[i + 1] == ' ') q = i + 2;
            break;
        }
        for (i32 k = 0; k < 2 && at < 0; k++) {
            i64 from = k ? q : start;
            if (k && q == start) break;
            kn = compile_kind_word(s, from, &kind);
            if (kn && from + kn < s.len && s.data[from + kn] == ':') at = from;
        }
    }
    if (at < 0) return 0;
    m->kind = kind;
    m->kind_off = (i32)at;
    m->kind_len = (i32)kn;
    return 1;
}

// ---------------------------------------------------------------------------
// File existence, cached

static u64 compile_hash(String8 s) {
    u64 h = 0xcbf29ce484222325ull;
    for (i64 i = 0; i < s.len; i++) {
        u8 c = compile_lower(s.data[i]);
        if (c == '/') c = '\\';
        h = (h ^ c) * 0x100000001b3ull;
    }
    return h ? h : 1;
}

static b32 compile_path_equal(String8 a, String8 b) {
    if (a.len != b.len) return 0;
    for (i64 i = 0; i < a.len; i++) {
        u8 x = compile_lower(a.data[i]), y = compile_lower(b.data[i]);
        if (x == '/') x = '\\';
        if (y == '/') y = '\\';
        if (x != y) return 0;
    }
    return 1;
}

#define COMPILE_CACHE_SLOTS (COMPILE_CACHE_CAP * 2)

static void compile_cache_table(CompileCache *c) {
    c->hashes = PUSH_ARRAY(c->arena, u64, COMPILE_CACHE_SLOTS);
    c->paths = PUSH_ARRAY(c->arena, String8, COMPILE_CACHE_SLOTS);
    c->answers = PUSH_ARRAY(c->arena, u8, COMPILE_CACHE_SLOTS);
    c->count = 0;
}

void compile_cache_init(CompileCache *c, Arena *arena, CompileExistsFn *exists, void *data) {
    *c = (CompileCache){ .arena = arena, .exists = exists, .data = data };
    compile_cache_table(c);
}

b32 compile_exists_fs(void *data, Arena *arena, String8 path) {
    (void)data;
    OsFileInfo info;
    return os_file_info_arena(arena, path, &info) == OS_FILE_OK && !info.is_dir;
}

// Whether `path` exists: the cached answer (then the copy pushed at `mark` is popped and *out is the stored
// one), or asked and kept with the path.
static b32 compile_cached(CompileCache *c, u64 mark, String8 path, String8 *out) {
    u64 h = compile_hash(path);
    u32 at = (u32)(h % COMPILE_CACHE_SLOTS);
    for (; c->hashes[at]; at = (at + 1) % COMPILE_CACHE_SLOTS) {
        if (c->hashes[at] == h && compile_path_equal(c->paths[at], path)) {
            arena_pop_to(c->arena, mark);
            *out = c->paths[at];
            return c->answers[at];
        }
    }
    c->asked++;
    b32 yes = c->exists(c->data, c->arena, path);
    c->hashes[at] = h;
    c->paths[at] = path;
    c->answers[at] = (u8)yes;
    c->count++;
    *out = path;
    return yes;
}

static b32 compile_absolute(String8 p) {
    if (p.len >= 3 && compile_is_alpha(p.data[0]) && p.data[1] == ':' && (p.data[2] == '\\' || p.data[2] == '/')) return 1;
    return p.len >= 1 && (p.data[0] == '\\' || p.data[0] == '/'); // UNC, or the drive's root
}

static String8 compile_path_join(Arena *arena, String8 dir, String8 path) {
    b32 sep = dir.len && (dir.data[dir.len - 1] == '\\' || dir.data[dir.len - 1] == '/');
    String8 s = dir.len ? str8_fmt(arena, sep ? "%S%S" : "%S\\%S", dir, path) : str8_copy(arena, path);
    for (i64 i = 0; i < s.len; i++) if (s.data[i] == '/') s.data[i] = '\\';
    return s;
}

static String8 compile_dirname(String8 p) {
    i64 i = p.len;
    while (i > 0 && p.data[i - 1] != '\\' && p.data[i - 1] != '/') i--;
    return str8(p.data, i > 0 ? i - 1 : 0);
}

String8 compile_resolve(CompileCache *c, String8 path, String8 dir, String8 proj, String8 root) {
    if (!path.len) return str8(NULL, 0);
    if (c->count >= COMPILE_CACHE_CAP) { // full: forgotten all at once
        arena_reset(c->arena);
        compile_cache_table(c);
    }
    String8 found;
    if (compile_absolute(path)) {
        u64 mark = arena_pos(c->arena);
        String8 p = compile_path_join(c->arena, str8(NULL, 0), path);
        return compile_cached(c, mark, p, &found) ? found : str8(NULL, 0);
    }
    String8 dirs[3];
    i32 n = 0;
    dirs[n++] = dir;
    if (proj.len) {
        String8 pd = compile_dirname(proj);
        dirs[n++] = compile_absolute(pd) ? pd : compile_path_join(c->arena, dir, pd);
    }
    dirs[n++] = root;
    for (i32 i = 0; i < n; i++) {
        if (!dirs[i].len) continue;
        b32 seen = 0;
        for (i32 k = 0; k < i; k++) seen |= compile_path_equal(dirs[k], dirs[i]);
        if (seen) continue;
        u64 mark = arena_pos(c->arena);
        String8 p = compile_path_join(c->arena, dirs[i], path);
        if (compile_cached(c, mark, p, &found)) return found;
    }
    return str8(NULL, 0);
}

// ---------------------------------------------------------------------------
// Cleaning and lines

b32 compile_utf8_valid(String8 s) {
    for (i64 i = 0; i < s.len;) {
        if (s.data[i] < 0x80) {
            i++;
            continue;
        }
        i64 adv;
        u32 cp = utf8_decode(s.data + i, s.len - i, &adv);
        if (cp == UTF_REPLACEMENT && !(adv == 3 && s.data[i] == 0xEF && s.data[i + 1] == 0xBF && s.data[i + 2] == 0xBD)) return 0;
        i += MAX(adv, 1);
    }
    return 1;
}

String8 compile_clean(Arena *arena, String8 raw) {
    u8 *out = PUSH_ARRAY(arena, u8, raw.len + 1);
    i64 n = 0;
    for (i64 i = 0; i < raw.len; i++) {
        u8 c = raw.data[i];
        if (c == '\r') continue;
        if (c != 0x1B) {
            out[n++] = c;
            continue;
        }
        if (i + 1 >= raw.len) break; // a lone ESC at the end
        u8 k = raw.data[++i];
        if (k == '[') { // CSI: parameters, intermediates, a final byte
            i++;
            while (i < raw.len && raw.data[i] >= 0x20 && raw.data[i] <= 0x3F) i++;
            while (i < raw.len && raw.data[i] >= 0x20 && raw.data[i] <= 0x2F) i++;
            // i is on the final byte (0x40-0x7E), or past the end
        } else if (k == ']') { // OSC: up to BEL or ESC '\'
            i++;
            while (i < raw.len && raw.data[i] != 0x07 && !(raw.data[i] == 0x1B && i + 1 < raw.len && raw.data[i + 1] == '\\')) i++;
            if (i < raw.len && raw.data[i] == 0x1B) i++;
        } else if (k == '(' || k == ')') { // character set selection: one more byte
            i++;
        }
        // any other two-byte sequence: both dropped
    }
    String8 s = str8(out, n);
    if (compile_utf8_valid(s)) return s;
    return os_oem_to_utf8(arena, s);
}

void compile_split_init(CompileSplit *s, Arena *arena) {
    s->buf = PUSH_ARRAY(arena, u8, COMPILE_LINE_MAX);
    s->len = 0;
}

// A full buffer goes out cut on a character boundary (a sequence the end splits stays for the next line).
static void compile_split_cut(CompileSplit *s, CompileLineOutFn *fn, void *data) {
    i64 cut = s->len, j = s->len - 1;
    while (j > 0 && j > s->len - 4 && (s->buf[j] & 0xC0) == 0x80) j--;
    u8 lead = s->buf[j];
    i64 need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    if (j + need > s->len && j > 0) cut = j;
    fn(data, str8(s->buf, cut));
    memmove(s->buf, s->buf + cut, (size_t)(s->len - cut));
    s->len -= cut;
}

void compile_split_feed(CompileSplit *s, const u8 *p, i64 n, CompileLineOutFn *fn, void *data) {
    i64 i = 0;
    while (i < n) {
        i64 nl = i;
        while (nl < n && p[nl] != '\n') nl++;
        i64 piece = nl - i;
        while (piece > 0) {
            i64 take = MIN(piece, (i64)COMPILE_LINE_MAX - s->len);
            memcpy(s->buf + s->len, p + i, (size_t)take);
            s->len += take;
            i += take;
            piece -= take;
            if (s->len == COMPILE_LINE_MAX) compile_split_cut(s, fn, data);
        }
        if (nl < n) {
            fn(data, str8(s->buf, s->len));
            s->len = 0;
            i = nl + 1;
        }
    }
}

void compile_split_end(CompileSplit *s, CompileLineOutFn *fn, void *data) {
    if (s->len) fn(data, str8(s->buf, s->len));
    s->len = 0;
}

// ---------------------------------------------------------------------------
// The worker

#define COMPILE_REC_HEAD 36

typedef struct CompileWorker {
    CompileRun *c;
    CompileBatch *batch;
    CompileCache cache;
    i64 kept;  // output bytes put in batches (the cap)
    b32 stop;  // cancelled while waiting for a batch
} CompileWorker;

static b32 compile_cancelled(CompileRun *c) {
    return os_atomic_get(&c->cancel) != 0;
}

// Hands the batch to the main thread and takes an empty one (waiting for one to come back). False when
// cancelled while waiting.
static b32 compile_send(CompileWorker *w, b32 last) {
    CompileRun *c = w->c;
    CompileBatch *b = w->batch;
    b->last = last;
    b->next = NULL;
    os_lock(&c->lock);
    if (c->ready_last) c->ready_last->next = b;
    else c->ready = b;
    c->ready_last = b;
    os_unlock(&c->lock);
    os_wake_main();
    w->batch = NULL;
    if (last) return 1;
    for (;;) {
        os_lock(&c->lock);
        CompileBatch *f = c->free;
        if (f) c->free = f->next;
        os_unlock(&c->lock);
        if (f) {
            f->next = NULL;
            f->used = 0;
            w->batch = f;
            return !compile_cancelled(c);
        }
        if (compile_cancelled(c)) return 0;
        os_event_wait(c->event, 0xFFFFFFFFu);
    }
}

static void compile_put(CompileWorker *w, String8 text, const CompileMatch *m, String8 file) {
    if (w->stop) return;
    i64 need = COMPILE_REC_HEAD + text.len + file.len;
    if (w->batch->used + need > COMPILE_BATCH_BYTES && !compile_send(w, 0)) {
        w->stop = 1;
        return;
    }
    u8 *at = w->batch->data + w->batch->used;
    u32 tl = (u32)text.len;
    u16 f[7] = { 0 };
    i64 line = 0, col = 0;
    u8 kind = 0, located = 0;
    if (m) {
        kind = (u8)m->kind;
        located = (u8)(m->located && file.len);
        f[4] = (u16)m->kind_off;
        f[5] = (u16)m->kind_len;
        if (located) {
            f[0] = (u16)m->path_off;
            f[1] = (u16)m->path_len;
            f[2] = (u16)m->num_off;
            f[3] = (u16)m->num_len;
            f[6] = (u16)file.len;
            line = m->line;
            col = m->col;
        }
    }
    memcpy(at, &tl, 4);
    at[4] = kind;
    at[5] = located;
    memcpy(at + 6, f, sizeof(f));
    memcpy(at + 20, &line, 8);
    memcpy(at + 28, &col, 8);
    memcpy(at + COMPILE_REC_HEAD, text.data, (size_t)text.len);
    if (located) memcpy(at + COMPILE_REC_HEAD + text.len, file.data, (size_t)file.len);
    w->batch->used += COMPILE_REC_HEAD + text.len + (located ? file.len : 0);
}

static void compile_worker_line(void *data, String8 raw) {
    CompileWorker *w = data;
    CompileRun *c = w->c;
    if (c->truncated || w->stop) return;
    arena_reset(&c->temp);
    String8 text = compile_clean(&c->temp, raw);
    if (text.len > COMPILE_LINE_MAX) text.len = COMPILE_LINE_MAX; // an OEM conversion grew it
    if (w->kept + text.len + 1 > c->max_output) {
        c->truncated = 1;
        compile_put(w, STR8_LIT(COMPILE_TRUNCATED), NULL, str8(NULL, 0));
        return;
    }
    w->kept += text.len + 1;
    CompileMatch m;
    String8 file = { 0 };
    b32 any = compile_match(text, &m);
    if (any) {
        String8 proj = m.proj_len ? str8(text.data + m.proj_off, m.proj_len) : str8(NULL, 0);
        file = compile_resolve(&w->cache, str8(text.data + m.path_off, m.path_len), c->dir, proj, c->root);
        if (file.len > 0x7FFF) file.len = 0;
        if (!file.len) { // a file that is nowhere: an error without a location if it said so
            m.located = 0;
            any = m.kind_len > 0 || compile_match_kind(text, &m);
        }
    } else {
        any = compile_match_kind(text, &m);
    }
    compile_put(w, text, any ? &m : NULL, file);
}

static void compile_worker_main(void *data) {
    CompileRun *c = data;
    CompileWorker w = { .c = c };
    Arena cache_arena = arena_create(MB(64));
    compile_cache_init(&w.cache, &cache_arena, compile_exists_fs, NULL);
    for (i32 i = 0; i < COMPILE_BATCH_POOL; i++) {
        CompileBatch *b = PUSH_STRUCT(&c->arena, CompileBatch);
        if (!i) {
            w.batch = b;
            continue;
        }
        os_lock(&c->lock);
        b->next = c->free;
        c->free = b;
        os_unlock(&c->lock);
    }
    CompileSplit split;
    compile_split_init(&split, &c->arena);
    u8 *buf = PUSH_ARRAY(&c->arena, u8, COMPILE_READ_BYTES);
    OsProcess proc;
    c->start_us = os_time_us();
    c->start_status = os_process_start(&c->arena, c->command, c->dir, &proc);
    if (c->start_status == OS_FILE_OK) {
        c->pid = proc.pid;
        b32 killed = 0;
        for (;;) {
            if (compile_cancelled(c) || w.stop) {
                killed = 1;
                break;
            }
            if (w.batch->used && !compile_send(&w, 0)) { // what it has goes before the read waits
                killed = 1;
                break;
            }
            i64 got = 0;
            OsProcessRead r = os_process_read(&proc, c->cancel_event, buf, COMPILE_READ_BYTES, &got);
            if (r == OS_PROCESS_DATA) {
                c->bytes += got;
                compile_split_feed(&split, buf, got, compile_worker_line, &w);
                continue;
            }
            killed = r == OS_PROCESS_CANCELLED;
            break; // the end (or a broken pipe)
        }
        if (killed) {
            os_process_kill(&proc);
            c->killed = 1;
        } else {
            compile_split_end(&split, compile_worker_line, &w);
        }
        os_process_wait(&proc, killed ? 1000 : 5000);
        c->exit_code = os_process_exit_code(&proc);
        os_process_close(&proc, killed);
    }
    c->end_us = os_time_us();
    os_release(cache_arena.base);
    if (!w.batch) { // cancelled while waiting for a batch: take one back, or make one
        os_lock(&c->lock);
        w.batch = c->free;
        if (w.batch) c->free = w.batch->next;
        os_unlock(&c->lock);
        if (!w.batch) w.batch = PUSH_STRUCT(&c->arena, CompileBatch);
        w.batch->used = 0;
    }
    compile_send(&w, 1);
}

// ---------------------------------------------------------------------------
// The main thread's side

CompileRun *compile_create(String8 command, String8 dir, String8 root, i64 max_output) {
    Arena arena = arena_create(MB(64));
    CompileRun *c = PUSH_STRUCT(&arena, CompileRun);
    c->command = str8_copy(&arena, command);
    c->dir = str8_copy(&arena, dir);
    c->root = str8_copy(&arena, root);
    c->max_output = MAX(max_output, 1);
    c->temp = arena_create(MB(16));
    c->arena = arena; // its position already counts this struct
    return c;
}

b32 compile_start(CompileRun *c) {
    if (!os_event_create(&c->event)) return 0;
    if (!os_event_create(&c->cancel_event)) return 0;
    if (!os_thread_start(&c->thread, compile_worker_main, c, 1)) return 0;
    c->started = 1;
    return 1;
}

void compile_cancel(CompileRun *c) {
    os_atomic_set(&c->cancel, 1);
    os_event_set(c->cancel_event);
    os_event_set(c->event);
}

CompileBatch *compile_take(CompileRun *c) {
    os_lock(&c->lock);
    CompileBatch *b = c->ready;
    c->ready = c->ready_last = NULL;
    os_unlock(&c->lock);
    return b;
}

b32 compile_next_line(CompileBatch *b, i64 *at, CompileLine *out) {
    if (*at >= b->used) return 0;
    u8 *p = b->data + *at;
    u32 tl;
    u16 f[7];
    memcpy(&tl, p, 4);
    memcpy(f, p + 6, sizeof(f));
    *out = (CompileLine){ 0 };
    out->kind = (CompileKind)p[4];
    out->located = p[5];
    out->path_off = f[0];
    out->path_len = f[1];
    out->num_off = f[2];
    out->num_len = f[3];
    out->kind_off = f[4];
    out->kind_len = f[5];
    memcpy(&out->line, p + 20, 8);
    memcpy(&out->col, p + 28, 8);
    out->text = str8(p + COMPILE_REC_HEAD, tl);
    i64 fl = out->located ? f[6] : 0;
    out->file = str8(p + COMPILE_REC_HEAD + tl, fl);
    *at += COMPILE_REC_HEAD + tl + fl;
    return 1;
}

void compile_give_back(CompileRun *c, CompileBatch *b) {
    if (b->last) { // the worker has returned from its last os_* call: the join does not wait
        os_thread_join(c->thread, 0xFFFFFFFFu);
        c->thread.handle = NULL;
        c->ended = 1;
        return;
    }
    os_lock(&c->lock);
    b->next = c->free;
    c->free = b;
    os_unlock(&c->lock);
    os_event_set(c->event);
}

b32 compile_join(CompileRun *c, u64 deadline_us) {
    if (!c->started || c->ended) return 1;
    u64 now = os_time_us();
    u32 left = now >= deadline_us ? 0 : (u32)((deadline_us - now) / 1000);
    if (!os_thread_join(c->thread, left)) return 0;
    c->thread.handle = NULL;
    c->ended = 1;
    return 1;
}

void compile_free(CompileRun *c) {
    if (c->event.handle) os_event_destroy(c->event);
    if (c->cancel_event.handle) os_event_destroy(c->cancel_event);
    os_release(c->temp.base);
    os_release(c->arena.base); // the struct itself lives there
}

#if TEAL_DEV
u64 compile_memory(CompileRun *c) {
    return c->arena.committed + c->temp.committed;
}
#endif
