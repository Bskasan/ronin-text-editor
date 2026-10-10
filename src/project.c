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
