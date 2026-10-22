/* ======================================================================
   ronin: dev: the docs site's Markdown subset to HTML (tools/make_docs.c)

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// The converter of the documentation site (Phase 24): one page of docs/src/*.md into one HTML file of docs/, inside the
// site's template with its navigation. tools/make_docs.c includes this file (with its own u8 / i32 / u32 / i64 / b32 /
// u64 and MAKE_DOCS defined), so the converter --test checks is the one the tool runs. It includes nothing, uses only
// memcpy and memset, and writes into memory the caller gives; the output depends on nothing but its input (no date,
// no path).
//
// The subset, everything else refused with its line:
//   blocks   # to #### headings (one # per page, the title; every heading GitHub's id: md_slug, a repeat numbered by
//            md_unique_id);
//            paragraphs; ``` fences with an optional language (ini lines colored: comments, [sections], keys);
//            - and 1. lists, nested by indentation, tight (a blank line ends the list), continuation lines;
//            tables (a header row and a |---| row, no alignment colons, every row on one line);
//            > blockquotes (one level, inline formatting within a line);
//            <!-- comment --> lines, dropped, except <!-- include: PATH --> (a repository file, its relative links
//            made GitHub links) and <!-- generated: NAME --> (a --dump-docs fragment)
//   inline   `code` (any run of backticks), **bold**, [text](url), ![alt](src) (width and height from the PNG)
//   literal  everything else: no italics, no backslash escapes, no raw HTML (a line starting with '<' that is not a
//            comment is refused), no autolinks, no setext headings, no rules; & < > " always escaped
// Input is LF only (the tool turns CRLF into LF first).

#if RONIN_DEV || defined(MAKE_DOCS)

#define MD_REPO_URL "https://github.com/Bskasan/ronin-text-editor"
#define MD_IDS_CAP 512
#define MD_NAV_CAP 64
#define MD_LIST_DEPTH 8
#define MD_SLUG_CAP 256
#define MD_IDS_POOL 16384

typedef struct MdStr {
    const u8 *data;
    i64 len;
} MdStr;

// Output into the caller's memory; `full` once something did not fit (the rest is dropped).
typedef struct MdOut {
    u8 *data;
    i64 len, cap;
    b32 full;
} MdOut;

// The first problem: the file (as the caller or an include named it), its line (1-based) and what is wrong.
typedef struct MdError {
    MdStr file;
    i64 line;
    u8 text[256];
    i64 text_len;
} MdError;

typedef struct MdHost {
    void *user;
    // The text of <!-- generated: NAME --> (generated = 1) or <!-- include: PATH --> (PATH relative to docs/src), LF
    // only; for an include also the folder its relative links start from, relative to the repository's root ("" for
    // the root). 0 when there is no such file.
    b32 (*resolve)(void *user, b32 generated, MdStr name, MdStr *text, MdStr *link_dir);
    // An image's size in pixels (its PNG header); 0 when unknown: the tag then has no width and height.
    b32 (*image_size)(void *user, MdStr src, i32 *width, i32 *height);
} MdHost;

typedef struct MdNavItem {
    MdStr page;  // docs/src/<page>.md, docs/<page>.html
    MdStr label; // its name in the navigation
    MdStr group; // the [Group] it is under
} MdNavItem;

typedef struct MdNav {
    MdNavItem items[MD_NAV_CAP];
    i32 count;
} MdNav;

// ---------------------------------------------------------------------------
// Strings and output

static MdStr md_str(const u8 *data, i64 len) {
    MdStr s = { data, len };
    return s;
}

static MdStr md_cstr(const char *c) {
    i64 n = 0;
    while (c[n]) n++;
    return md_str((const u8 *)c, n);
}

static b32 md_equal(MdStr a, MdStr b) {
    if (a.len != b.len) return 0;
    for (i64 i = 0; i < a.len; i++) if (a.data[i] != b.data[i]) return 0;
    return 1;
}

static b32 md_starts(MdStr s, const char *prefix) {
    MdStr p = md_cstr(prefix);
    return s.len >= p.len && md_equal(md_str(s.data, p.len), p);
}

static b32 md_ends(MdStr s, const char *suffix) {
    MdStr p = md_cstr(suffix);
    return s.len >= p.len && md_equal(md_str(s.data + s.len - p.len, p.len), p);
}

static MdStr md_skip(MdStr s, i64 n) {
    return md_str(s.data + n, s.len - n);
}

static MdStr md_trim(MdStr s) {
    while (s.len && (s.data[0] == ' ' || s.data[0] == '\t')) s = md_skip(s, 1);
    while (s.len && (s.data[s.len - 1] == ' ' || s.data[s.len - 1] == '\t')) s.len--;
    return s;
}

static i64 md_indent(MdStr line) {
    i64 n = 0;
    while (n < line.len && line.data[n] == ' ') n++;
    return n;
}

static void md_put(MdOut *o, const u8 *data, i64 len) {
    if (o->full || len <= 0) return;
    if (o->len + len > o->cap) {
        o->full = 1;
        return;
    }
    memcpy(o->data + o->len, data, (size_t)len);
    o->len += len;
}

static void md_puts(MdOut *o, const char *c) {
    MdStr s = md_cstr(c);
    md_put(o, s.data, s.len);
}

static void md_put_str(MdOut *o, MdStr s) {
    md_put(o, s.data, s.len);
}

static void md_put_int(MdOut *o, i64 v) {
    u8 digits[24];
    i32 n = 0;
    if (v < 0) {
        md_puts(o, "-");
        v = -v;
    }
    do {
        digits[n++] = (u8)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) md_put(o, &digits[--n], 1);
}

// & < > " as entities; with `newline_space` a newline becomes a space (a code span across lines).
static void md_escape(MdOut *o, MdStr s, b32 newline_space) {
    for (i64 i = 0; i < s.len; i++) {
        u8 b = s.data[i];
        if (b == '&') md_puts(o, "&amp;");
        else if (b == '<') md_puts(o, "&lt;");
        else if (b == '>') md_puts(o, "&gt;");
        else if (b == '"') md_puts(o, "&quot;");
        else if (b == '\n' && newline_space) md_puts(o, " ");
        else md_put(o, &b, 1);
    }
}

static u64 md_hash(const u8 *data, i64 len) {
    u64 h = 0xcbf29ce484222325ull;
    for (i64 i = 0; i < len; i++) h = (h ^ data[i]) * 0x100000001b3ull;
    return h;
}

static void md_set_error(MdError *err, MdStr file, i64 line, const char *a, MdStr b, const char *z) {
    if (!err || err->text_len) return; // the first one counts
    err->file = file;
    err->line = line;
    MdOut o = { err->text, 0, (i64)sizeof(err->text) - 1, 0 };
    md_puts(&o, a);
    md_put_str(&o, b);
    md_puts(&o, z);
    err->text_len = o.len;
}

// ---------------------------------------------------------------------------
// Conversion state

// The ids a page has given its headings: each its 64-bit FNV-1a hash, its counter (github-slugger's occurrences) and
// its text in `pool`. Two ids are the same only when their texts are: the hash only skips the comparison.
typedef struct MdId {
    u64 hash;
    i32 count;
    i32 at, len;
} MdId;

typedef struct MdIds {
    MdId taken[MD_IDS_CAP];
    i32 count;
    i32 pool_len;
    u8 pool[MD_IDS_POOL];
} MdIds;

static i32 md_ids_find(const MdIds *ids, const u8 *s, i64 len) {
    u64 h = md_hash(s, len);
    for (i32 i = 0; i < ids->count; i++) {
        const MdId *id = &ids->taken[i];
        if (id->hash == h && id->len == len && md_equal(md_str(ids->pool + id->at, len), md_str(s, len))) return i;
    }
    return -1;
}

// Records `s` with the hash `h` (always md_hash of `s`, except in a test of a collision); 0 when `ids` is full.
static b32 md_ids_take(MdIds *ids, const u8 *s, i64 len, u64 h) {
    if (ids->count == MD_IDS_CAP || ids->pool_len + len > MD_IDS_POOL) return 0;
    MdId *id = &ids->taken[ids->count++];
    id->hash = h;
    id->count = 0;
    id->at = ids->pool_len;
    id->len = (i32)len;
    if (len) memcpy(ids->pool + ids->pool_len, s, (size_t)len);
    ids->pool_len += (i32)len;
    return 1;
}

// GitHub's id of a heading whose slug is `slug` (github-slugger's rule, #20): the slug while no heading of the page has
// that id; else the slug's counter goes up until "slug-N" is free. The id is recorded; its length, or -1 when `ids` is
// full or the id longer than `cap`. The README check uses it too.
i64 md_unique_id(MdIds *ids, MdStr slug, u8 *out, i64 cap) {
    if (slug.len > cap) return -1;
    if (slug.len) memcpy(out, slug.data, (size_t)slug.len);
    i64 n = slug.len;
    i32 base = md_ids_find(ids, slug.data, slug.len);
    while (md_ids_find(ids, out, n) >= 0) {
        u8 digits[12];
        i32 count = ++ids->taken[base].count, d = 0;
        do {
            digits[d++] = (u8)('0' + count % 10);
            count /= 10;
        } while (count);
        if (slug.len + 1 + d > cap) return -1;
        n = slug.len;
        out[n++] = '-';
        while (d) out[n++] = digits[--d];
    }
    return md_ids_take(ids, out, n, md_hash(out, n)) ? n : -1;
}

typedef struct MdPageState {
    MdIds ids;
    MdStr title; // the # heading's text
    i32 titles;
} MdPageState;

static void md_page_init(MdPageState *page) {
    memset(page, 0, sizeof(*page));
}

typedef struct MdCtx {
    const MdHost *host;
    MdOut *out;
    MdError *err;
    MdPageState *page;
    MdStr file;       // for messages
    b32 failed;
    b32 included;     // converting an included repository file (no further includes)
    b32 rewrite;      // its relative links become GitHub links...
    MdStr link_dir;   // ...relative to this folder of the repository
} MdCtx;

static void md_fail(MdCtx *c, i64 line, const char *a, MdStr b, const char *z) {
    md_set_error(c->err, c->file, line, a, b, z);
    c->failed = 1;
}

static i64 md_lines_before(MdStr s, i64 at) {
    i64 n = 0;
    for (i64 i = 0; i < at && i < s.len; i++) n += s.data[i] == '\n';
    return n;
}

// ---------------------------------------------------------------------------
// Inline formatting

// The end of the run of `n` backticks closing the code span whose opening run ends at `from`; -1 when none.
static i64 md_code_close(MdStr s, i64 from, i64 n) {
    for (i64 j = from; j < s.len;) {
        if (s.data[j] != '`') {
            j++;
            continue;
        }
        i64 run = 0;
        while (j + run < s.len && s.data[j + run] == '`') run++;
        if (run == n) return j;
        j += run;
    }
    return -1;
}

// Past a code span starting at `at` (a backtick), or `at` + 1 when it is not closed.
static i64 md_skip_code(MdStr s, i64 at) {
    i64 n = 0;
    while (at + n < s.len && s.data[at + n] == '`') n++;
    i64 close = md_code_close(s, at + n, n);
    return close < 0 ? at + n : close + n;
}

// [text](url) at `at` (the '['): its text, its url and the index past ')'. Brackets nest; code spans are skipped.
static b32 md_link_at(MdStr s, i64 at, MdStr *text, MdStr *url, i64 *end) {
    i64 depth = 0, i = at;
    while (i < s.len) {
        u8 b = s.data[i];
        if (b == '`') {
            i = md_skip_code(s, i);
            continue;
        }
        if (b == '[') depth++;
        if (b == ']' && --depth == 0) break;
        i++;
    }
    if (i >= s.len || i + 1 >= s.len || s.data[i + 1] != '(') return 0;
    i64 u = i + 2, k = u;
    while (k < s.len && s.data[k] != ')' && s.data[k] != ' ' && s.data[k] != '\n') k++;
    if (k >= s.len || s.data[k] != ')' || k == u) return 0;
    *text = md_str(s.data + at + 1, i - at - 1);
    *url = md_str(s.data + u, k - u);
    *end = k + 1;
    return 1;
}

static b32 md_external(MdStr url) {
    return md_starts(url, "http://") || md_starts(url, "https://") || md_starts(url, "mailto:");
}

static void md_slug_put(u8 *out, i64 *n, i64 cap, const u8 *bytes, i64 len) {
    if (*n + len > cap) {
        *n = cap + 1; // full: nothing more goes in
        return;
    }
    memcpy(out + *n, bytes, (size_t)len);
    *n += len;
}

// Plain text into anchor characters, GitHub's way (res/heading_ids.md shows each rule, #20): ASCII letters lowercased,
// digits, '-' and '_' kept, a space a '-', other ASCII dropped; Latin-1 and Latin Extended-A capitals lowercased (U+0130
// as "i" and U+0307), U+00D7, U+00F7 and General Punctuation (U+2010-U+205E: dashes, quotes, the ellipsis) dropped;
// every other character kept as it is.
static void md_slug_chars(MdStr s, u8 *out, i64 *n, i64 cap) {
    for (i64 i = 0; i < s.len && *n <= cap;) {
        u8 b = s.data[i];
        if (b < 0x80) {
            u8 c = b >= 'A' && b <= 'Z' ? (u8)(b - 'A' + 'a') : b == ' ' ? (u8)'-' : b;
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') md_slug_put(out, n, cap, &c, 1);
            i++;
            continue;
        }
        i64 len = 1;
        u32 cp = 0;
        if (b >= 0xc2 && b <= 0xdf && i + 1 < s.len && (s.data[i + 1] & 0xc0) == 0x80) {
            len = 2;
            cp = ((u32)(b & 0x1f) << 6) | (s.data[i + 1] & 0x3f);
        } else if (b == 0xe2 && i + 2 < s.len && (s.data[i + 1] & 0xc0) == 0x80 && (s.data[i + 2] & 0xc0) == 0x80) {
            len = 3;
            cp = 0x2000 | ((u32)(s.data[i + 1] & 0x3f) << 6) | (s.data[i + 2] & 0x3f);
        }
        u32 lower = cp;
        if (cp >= 0xc0 && cp <= 0xde) lower = cp + 0x20;
        else if ((cp >= 0x100 && cp <= 0x12f) || (cp >= 0x132 && cp <= 0x137) || (cp >= 0x14a && cp <= 0x177)) lower = cp | 1;
        else if ((cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17e)) lower = cp + (cp & 1);
        else if (cp == 0x178) lower = 0xff;
        if (cp == 0xd7 || cp == 0xf7 || (cp >= 0x2010 && cp <= 0x205e)) {
            // dropped
        } else if (cp == 0x130) {
            static const u8 dotted_i[3] = { 'i', 0xcc, 0x87 };
            md_slug_put(out, n, cap, dotted_i, 3);
        } else if (lower != cp) {
            u8 two[2] = { (u8)(0xc0 | (lower >> 6)), (u8)(0x80 | (lower & 0x3f)) };
            md_slug_put(out, n, cap, two, 2);
        } else {
            md_slug_put(out, n, cap, s.data + i, len);
        }
        i += len;
    }
}

// A heading's Markdown as the text GitHub reads (a code span its content, one padding space each side dropped as
// md_inline does; a link its text; an image nothing), through md_slug_chars.
static void md_slug_text(MdStr s, u8 *out, i64 *n, i64 cap) {
    i64 i = 0, plain = 0;
    while (i < s.len) {
        u8 b = s.data[i];
        MdStr text = { 0 }, url;
        i64 end = -1;
        b32 code = b == '`', image = 0, link = 0;
        if (b == '!' && i + 1 < s.len && s.data[i + 1] == '[' && md_link_at(s, i + 1, &text, &url, &end)) image = 1;
        else if (b == '[' && md_link_at(s, i, &text, &url, &end)) link = 1;
        if (!code && !image && !link) {
            i++;
            continue;
        }
        md_slug_chars(md_str(s.data + plain, i - plain), out, n, cap);
        if (code) {
            i64 k = 0;
            while (i + k < s.len && s.data[i + k] == '`') k++;
            i64 close = md_code_close(s, i + k, k);
            if (close < 0) { // literal backticks: punctuation
                i += k;
            } else {
                MdStr inner = md_str(s.data + i + k, close - i - k);
                b32 blank = 1;
                for (i64 j = 0; j < inner.len; j++) blank &= inner.data[j] == ' ';
                if (inner.len >= 2 && !blank && inner.data[0] == ' ' && inner.data[inner.len - 1] == ' ') inner = md_str(inner.data + 1, inner.len - 2);
                md_slug_chars(inner, out, n, cap);
                i = close + k;
            }
        } else {
            if (link) md_slug_text(text, out, n, cap);
            i = end;
        }
        plain = i;
    }
    md_slug_chars(md_str(s.data + plain, i - plain), out, n, cap);
}

// GitHub's anchor of one heading (its slug, before md_unique_id numbers a repeat). The README check uses it too. At
// most `cap` bytes; U+0130 makes the slug longer than the heading's text (2 bytes -> 3).
i64 md_slug(MdStr heading, u8 *out, i64 cap) {
    i64 n = 0;
    md_slug_text(heading, out, &n, cap);
    return n > cap ? cap : n;
}

// A link's target as the page writes it: in an included repository file a relative link becomes the GitHub page of
// that file (its #anchor stays).
static void md_put_url(MdCtx *c, MdStr url) {
    if (c->rewrite && !md_external(url) && !md_starts(url, "#")) {
        md_puts(c->out, MD_REPO_URL "/blob/main/");
        md_escape(c->out, c->link_dir, 0);
    }
    md_escape(c->out, url, 0);
}

static void md_inline(MdCtx *c, MdStr s, i64 line);

static void md_image(MdCtx *c, MdStr alt, MdStr src, i64 line) {
    if (c->rewrite) {
        md_fail(c, line, "an image in an included file (", src, ")");
        return;
    }
    md_puts(c->out, "<img src=\"");
    md_escape(c->out, src, 0);
    md_puts(c->out, "\" alt=\"");
    md_escape(c->out, alt, 1);
    md_puts(c->out, "\"");
    i32 w = 0, h = 0;
    if (c->host && c->host->image_size && c->host->image_size(c->host->user, src, &w, &h)) {
        md_puts(c->out, " width=\"");
        md_put_int(c->out, w);
        md_puts(c->out, "\" height=\"");
        md_put_int(c->out, h);
        md_puts(c->out, "\"");
    }
    md_puts(c->out, " loading=\"lazy\">");
}

static void md_inline(MdCtx *c, MdStr s, i64 line) {
    i64 i = 0;
    while (i < s.len && !c->failed) {
        u8 b = s.data[i];
        if (b == '`') {
            i64 n = 0;
            while (i + n < s.len && s.data[i + n] == '`') n++;
            i64 close = md_code_close(s, i + n, n);
            if (close < 0) {
                md_fail(c, line + md_lines_before(s, i), "an unclosed code span (", md_str(s.data + i, n), ")");
                return;
            }
            MdStr code = md_str(s.data + i + n, close - i - n);
            b32 blank = 1;
            for (i64 k = 0; k < code.len; k++) blank &= code.data[k] == ' ';
            if (code.len >= 2 && !blank && code.data[0] == ' ' && code.data[code.len - 1] == ' ') code = md_str(code.data + 1, code.len - 2);
            md_puts(c->out, "<code>");
            md_escape(c->out, code, 1);
            md_puts(c->out, "</code>");
            i = close + n;
            continue;
        }
        if (b == '*' && i + 1 < s.len && s.data[i + 1] == '*') {
            i64 j = i + 2;
            while (j + 1 < s.len && !(s.data[j] == '*' && s.data[j + 1] == '*')) j = s.data[j] == '`' ? md_skip_code(s, j) : j + 1;
            if (j + 1 >= s.len) {
                md_fail(c, line + md_lines_before(s, i), "an unclosed **", md_str(NULL, 0), "");
                return;
            }
            md_puts(c->out, "<strong>");
            md_inline(c, md_str(s.data + i + 2, j - i - 2), line + md_lines_before(s, i));
            md_puts(c->out, "</strong>");
            i = j + 2;
            continue;
        }
        MdStr text, url;
        i64 end;
        if (b == '!' && i + 1 < s.len && s.data[i + 1] == '[' && md_link_at(s, i + 1, &text, &url, &end)) {
            md_image(c, text, url, line + md_lines_before(s, i));
            i = end;
            continue;
        }
        if (b == '[' && md_link_at(s, i, &text, &url, &end)) {
            md_puts(c->out, "<a href=\"");
            md_put_url(c, url);
            md_puts(c->out, "\">");
            md_inline(c, text, line + md_lines_before(s, i));
            md_puts(c->out, "</a>");
            i = end;
            continue;
        }
        md_escape(c->out, md_str(&b, 1), 0);
        i++;
    }
}

// ---------------------------------------------------------------------------
// Blocks

typedef struct MdBlocks {
    MdStr text;       // the paragraph or list item being collected: from its first character to its last line's end
    i64 text_line;
    b32 text_open;
    b32 text_in_item; // a list item's text (no <p>)
    struct {
        i64 indent;
        b32 ordered;
    } lists[MD_LIST_DEPTH];
    i32 depth;
} MdBlocks;

static void md_flush_text(MdCtx *c, MdBlocks *st) {
    if (!st->text_open) return;
    st->text_open = 0;
    if (!st->text_in_item) md_puts(c->out, "<p>");
    md_inline(c, st->text, st->text_line);
    if (!st->text_in_item) md_puts(c->out, "</p>\n");
}

static void md_close_lists(MdCtx *c, MdBlocks *st, i32 depth) {
    md_flush_text(c, st);
    while (st->depth > depth) {
        st->depth--;
        md_puts(c->out, st->lists[st->depth].ordered ? "</li>\n</ol>\n" : "</li>\n</ul>\n");
    }
}

static void md_close_all(MdCtx *c, MdBlocks *st) {
    md_close_lists(c, st, 0);
}

// A list item: its indentation, ordered or not, and where its text starts. "- " or up to 9 digits and ". ".
static b32 md_item(MdStr line, i64 *indent, b32 *ordered, i64 *text_at) {
    i64 i = md_indent(line);
    if (i + 1 < line.len && line.data[i] == '-' && line.data[i + 1] == ' ') {
        *indent = i;
        *ordered = 0;
        *text_at = i + 2;
        return 1;
    }
    i64 d = i;
    while (d < line.len && d - i < 9 && line.data[d] >= '0' && line.data[d] <= '9') d++;
    if (d > i && d + 1 < line.len && line.data[d] == '.' && line.data[d + 1] == ' ') {
        *indent = i;
        *ordered = 1;
        *text_at = d + 2;
        return 1;
    }
    return 0;
}

// --- , *** , ___ , === : a rule or a setext underline, neither supported.
static b32 md_rule(MdStr line) {
    MdStr t = md_trim(line);
    if (t.len < 3) return 0;
    u8 b = t.data[0];
    if (b != '-' && b != '*' && b != '_' && b != '=') return 0;
    for (i64 i = 0; i < t.len; i++) if (t.data[i] != b && t.data[i] != ' ') return 0;
    return 1;
}

// The cells of a table row "| a | b |" (code spans may hold '|'); -1 when the line is not one.
static i32 md_cells(MdStr line, MdStr *cells, i32 cap) {
    MdStr t = md_trim(line);
    if (t.len < 2 || t.data[0] != '|' || t.data[t.len - 1] != '|') return -1;
    i32 n = 0;
    i64 start = 1;
    for (i64 i = 1; i < t.len;) {
        if (t.data[i] == '`') {
            i = md_skip_code(t, i);
            continue;
        }
        if (t.data[i] == '|') {
            if (n < cap) cells[n] = md_trim(md_str(t.data + start, i - start));
            n++;
            start = i + 1;
        }
        i++;
    }
    return n;
}

// |---|---| (alignment colons refused by the caller).
static b32 md_delimiter_row(MdStr line, b32 *colon) {
    MdStr t = md_trim(line);
    if (t.len < 3 || t.data[0] != '|') return 0;
    b32 dash = 0;
    *colon = 0;
    for (i64 i = 0; i < t.len; i++) {
        u8 b = t.data[i];
        if (b == '-') dash = 1;
        else if (b == ':') *colon = 1;
        else if (b != '|' && b != ' ') return 0;
    }
    return dash;
}

static MdStr md_line_at(MdStr text, i64 pos) {
    i64 e = pos;
    while (e < text.len && text.data[e] != '\n') e++;
    return md_str(text.data + pos, e - pos);
}

static i64 md_next(MdStr text, i64 pos) {
    while (pos < text.len && text.data[pos] != '\n') pos++;
    return pos < text.len ? pos + 1 : pos;
}

static void md_blocks(MdCtx *c, MdStr text, i64 first_line);

// "#", "##", ... followed by a space ("#include" is text).
static b32 md_heading_line(MdStr line) {
    i64 level = 0;
    while (level < line.len && line.data[level] == '#') level++;
    return level < line.len && line.data[level] == ' ';
}

static void md_heading(MdCtx *c, MdStr line, i64 n) {
    i64 level = 0;
    while (level < line.len && line.data[level] == '#') level++;
    if (level > 4) {
        md_fail(c, n, "a heading deeper than #### (", md_str(line.data, level), ")");
        return;
    }
    MdStr text = md_trim(md_skip(line, level));
    if (!text.len) {
        md_fail(c, n, "an empty heading", md_str(NULL, 0), "");
        return;
    }
    if (level == 1) {
        if (c->page->titles++) {
            md_fail(c, n, "a second # title (", text, ")");
            return;
        }
        c->page->title = text;
    }
    u8 slug[MD_SLUG_CAP], id[MD_SLUG_CAP + 16];
    i64 len = md_slug(text, slug, MD_SLUG_CAP);
    if (md_equal(md_str(slug, len), md_cstr("main"))) {
        md_fail(c, n, "a heading with the template's id (#main)", md_str(NULL, 0), "");
        return;
    }
    i64 id_len = md_unique_id(&c->page->ids, md_str(slug, len), id, (i64)sizeof(id));
    if (id_len < 0) {
        md_fail(c, n, "more headings than a page holds (", text, ")");
        return;
    }
    u8 tag = (u8)('0' + level);
    md_puts(c->out, "<h");
    md_put(c->out, &tag, 1);
    md_puts(c->out, " id=\"");
    md_escape(c->out, md_str(id, id_len), 0);
    md_puts(c->out, "\">");
    md_inline(c, text, n);
    md_puts(c->out, "</h");
    md_put(c->out, &tag, 1);
    md_puts(c->out, ">\n");
}

static void md_directive(MdCtx *c, MdStr line, i64 n) {
    MdStr inner = md_trim(md_str(line.data + 4, line.len - 7));
    b32 generated = md_starts(inner, "generated:");
    b32 include = md_starts(inner, "include:");
    if (!generated && !include) return; // a comment (a keymap marker)
    MdStr name = md_trim(md_skip(inner, generated ? 10 : 8));
    if (c->included) {
        md_fail(c, n, "an include inside an included file (", name, ")");
        return;
    }
    MdStr sub = { 0 }, dir = { 0 };
    if (!c->host || !c->host->resolve || !c->host->resolve(c->host->user, generated, name, &sub, &dir)) {
        md_fail(c, n, generated ? "no generated fragment " : "no file to include: ", name, "");
        return;
    }
    MdCtx child = *c;
    child.file = name;
    child.included = 1;
    child.rewrite = include;
    child.link_dir = dir;
    md_blocks(&child, sub, 1);
    c->failed |= child.failed;
}

static void md_fence(MdCtx *c, MdStr text, i64 *pos, i64 *n) {
    MdStr open = md_line_at(text, *pos);
    MdStr lang = md_trim(md_skip(open, 3));
    i64 start_line = *n;
    b32 ini = md_equal(lang, md_cstr("ini"));
    md_puts(c->out, ini ? "<pre class=\"ini\"><code>" : "<pre><code>");
    *pos = md_next(text, *pos);
    (*n)++;
    for (;;) {
        if (*pos >= text.len) {
            md_fail(c, start_line, "an unclosed ``` fence", md_str(NULL, 0), "");
            return;
        }
        MdStr line = md_line_at(text, *pos);
        *pos = md_next(text, *pos);
        (*n)++;
        if (md_equal(md_trim(line), md_cstr("```"))) break;
        MdStr t = md_trim(line);
        i64 eq = -1;
        for (i64 i = 0; i < line.len && eq < 0; i++) if (line.data[i] == '=') eq = i;
        if (ini && t.len && (t.data[0] == '#' || t.data[0] == ';')) {
            md_puts(c->out, "<span class=\"c\">");
            md_escape(c->out, line, 0);
            md_puts(c->out, "</span>");
        } else if (ini && t.len && t.data[0] == '[') {
            md_puts(c->out, "<span class=\"s\">");
            md_escape(c->out, line, 0);
            md_puts(c->out, "</span>");
        } else if (ini && eq > 0) {
            md_puts(c->out, "<span class=\"k\">");
            md_escape(c->out, md_str(line.data, eq), 0);
            md_puts(c->out, "</span>");
            md_escape(c->out, md_skip(line, eq), 0);
        } else {
            md_escape(c->out, line, 0);
        }
        md_puts(c->out, "\n");
    }
    md_puts(c->out, "</code></pre>\n");
}

static void md_table(MdCtx *c, MdStr text, i64 *pos, i64 *n) {
    MdStr cells[32];
    MdStr header = md_line_at(text, *pos);
    i32 columns = md_cells(header, cells, 32);
    if (columns < 1 || columns > 32) {
        md_fail(c, *n, "a table row with no cells or more than 32", md_str(NULL, 0), "");
        return;
    }
    md_puts(c->out, "<table>\n<thead>\n<tr>");
    for (i32 k = 0; k < columns; k++) {
        md_puts(c->out, "<th>");
        md_inline(c, cells[k], *n);
        md_puts(c->out, "</th>");
    }
    md_puts(c->out, "</tr>\n</thead>\n<tbody>\n");
    *pos = md_next(text, md_next(text, *pos)); // the header and the delimiter row
    *n += 2;
    while (*pos < text.len && !c->failed) {
        MdStr line = md_line_at(text, *pos);
        if (!md_trim(line).len || md_trim(line).data[0] != '|') break;
        i32 got = md_cells(line, cells, 32);
        if (got != columns) {
            md_fail(c, *n, "a table row whose cells are not the header's (", line, ")");
            return;
        }
        md_puts(c->out, "<tr>");
        for (i32 k = 0; k < columns; k++) {
            md_puts(c->out, "<td>");
            md_inline(c, cells[k], *n);
            md_puts(c->out, "</td>");
        }
        md_puts(c->out, "</tr>\n");
        *pos = md_next(text, *pos);
        (*n)++;
    }
    md_puts(c->out, "</tbody>\n</table>\n");
}

static void md_quote(MdCtx *c, MdStr text, i64 *pos, i64 *n) {
    md_puts(c->out, "<blockquote>\n");
    b32 open = 0;
    while (*pos < text.len && !c->failed) {
        MdStr line = md_line_at(text, *pos);
        if (!line.len || line.data[0] != '>') break;
        MdStr inner = md_trim(md_skip(line, 1));
        if (!inner.len) {
            if (open) md_puts(c->out, "</p>\n");
            open = 0;
        } else {
            md_puts(c->out, open ? "\n" : "<p>");
            open = 1;
            md_inline(c, inner, *n);
        }
        *pos = md_next(text, *pos);
        (*n)++;
    }
    if (open) md_puts(c->out, "</p>\n");
    md_puts(c->out, "</blockquote>\n");
}

static void md_blocks(MdCtx *c, MdStr text, i64 first_line) {
    MdBlocks st = { 0 };
    i64 pos = 0, n = first_line;
    while (pos < text.len && !c->failed) {
        MdStr line = md_line_at(text, pos);
        MdStr t = md_trim(line);
        for (i64 i = 0; i < line.len; i++) {
            if (line.data[i] == '\t') {
                md_fail(c, n, "a tab (use spaces)", md_str(NULL, 0), "");
                return;
            }
            if (line.data[i] == '\r') {
                md_fail(c, n, "a carriage return (LF line endings only)", md_str(NULL, 0), "");
                return;
            }
        }
        if (!t.len) { // a blank line ends paragraphs and lists
            md_close_all(c, &st);
            pos = md_next(text, pos);
            n++;
            continue;
        }
        if (md_starts(t, "```")) {
            md_close_all(c, &st);
            md_fence(c, text, &pos, &n);
            continue;
        }
        if (line.data[0] == '#' && md_heading_line(line)) {
            md_close_all(c, &st);
            md_heading(c, line, n);
            pos = md_next(text, pos);
            n++;
            continue;
        }
        if (t.data[0] == '<') {
            if (!md_starts(t, "<!--") || !md_ends(t, "-->") || t.len < 7) {
                md_fail(c, n, "raw HTML is not supported (", t, ")");
                return;
            }
            md_close_all(c, &st);
            md_directive(c, t, n);
            pos = md_next(text, pos);
            n++;
            continue;
        }
        if (md_rule(line)) {
            md_fail(c, n, "rules and setext headings are not supported (", t, ")");
            return;
        }
        if (t.data[0] == '|') {
            b32 colon = 0;
            i64 next = md_next(text, pos);
            if (next >= text.len || !md_delimiter_row(md_line_at(text, next), &colon)) {
                md_fail(c, n, "a table row without a |---| row under the header (", t, ")");
                return;
            }
            if (colon) {
                md_fail(c, n + 1, "table alignment (':') is not supported", md_str(NULL, 0), "");
                return;
            }
            md_close_all(c, &st);
            md_table(c, text, &pos, &n);
            continue;
        }
        if (t.data[0] == '>') {
            md_close_all(c, &st);
            md_quote(c, text, &pos, &n);
            continue;
        }
        i64 indent, text_at;
        b32 ordered;
        if (md_item(line, &indent, &ordered, &text_at)) {
            md_flush_text(c, &st);
            while (st.depth && st.lists[st.depth - 1].indent > indent) {
                st.depth--;
                md_puts(c->out, st.lists[st.depth].ordered ? "</li>\n</ol>\n" : "</li>\n</ul>\n");
            }
            if (st.depth && st.lists[st.depth - 1].indent == indent && st.lists[st.depth - 1].ordered != ordered) {
                st.depth--;
                md_puts(c->out, st.lists[st.depth].ordered ? "</li>\n</ol>\n" : "</li>\n</ul>\n");
            }
            if (st.depth && st.lists[st.depth - 1].indent == indent) {
                md_puts(c->out, "</li>\n<li>");
            } else {
                if (st.depth == MD_LIST_DEPTH) {
                    md_fail(c, n, "lists nested too deep", md_str(NULL, 0), "");
                    return;
                }
                if (st.depth) md_puts(c->out, "\n");
                md_puts(c->out, ordered ? "<ol>\n<li>" : "<ul>\n<li>");
                st.lists[st.depth].indent = indent;
                st.lists[st.depth].ordered = ordered;
                st.depth++;
            }
            st.text = md_str(line.data + text_at, line.len - text_at);
            st.text_line = n;
            st.text_open = 1;
            st.text_in_item = 1;
        } else if (st.text_open) { // a continuation line
            st.text.len = line.data + line.len - st.text.data;
        } else {
            md_close_all(c, &st);
            st.text = md_str(line.data + md_indent(line), line.len - md_indent(line));
            st.text_line = n;
            st.text_open = 1;
            st.text_in_item = 0;
        }
        pos = md_next(text, pos);
        n++;
    }
    if (!c->failed) md_close_all(c, &st);
}

// ---------------------------------------------------------------------------
// The site: navigation and pages

// nav.txt: "# comment" and blank lines; "[Group]" starts a group; "<page> <label>" (page: a-z, 0-9, '-').
b32 md_nav_parse(MdStr text, MdStr file, MdNav *nav, MdError *err) {
    nav->count = 0;
    MdStr group = { 0 };
    i64 n = 1;
    for (i64 pos = 0; pos < text.len; pos = md_next(text, pos), n++) {
        MdStr t = md_trim(md_line_at(text, pos));
        if (t.len && t.data[t.len - 1] == '\r') t = md_trim(md_str(t.data, t.len - 1));
        if (!t.len || t.data[0] == '#') continue;
        if (t.data[0] == '[') {
            if (t.data[t.len - 1] != ']' || t.len < 3) {
                md_set_error(err, file, n, "a group is \"[Name]\" (", t, ")");
                return 0;
            }
            group = md_str(t.data + 1, t.len - 2);
            continue;
        }
        i64 k = 0;
        while (k < t.len && t.data[k] != ' ') {
            u8 b = t.data[k];
            if (!((b >= 'a' && b <= 'z') || (b >= '0' && b <= '9') || b == '-')) {
                md_set_error(err, file, n, "a page name is a-z, 0-9 and '-' (", t, ")");
                return 0;
            }
            k++;
        }
        MdStr page = md_str(t.data, k), label = md_trim(md_skip(t, k));
        if (!label.len || !group.len) {
            md_set_error(err, file, n, !group.len ? "a page before the first [Group] (" : "a page without its label (", t, ")");
            return 0;
        }
        for (i32 i = 0; i < nav->count; i++) {
            if (md_equal(nav->items[i].page, page)) {
                md_set_error(err, file, n, "the page is listed twice (", page, ")");
                return 0;
            }
        }
        if (nav->count == MD_NAV_CAP) {
            md_set_error(err, file, n, "more pages than the navigation holds", md_str(NULL, 0), "");
            return 0;
        }
        MdNavItem item = { page, label, group };
        nav->items[nav->count++] = item;
    }
    if (!nav->count) md_set_error(err, file, 1, "no pages", md_str(NULL, 0), "");
    return nav->count > 0;
}

// The title as plain text (backticks and ** dropped), escaped.
static void md_put_title(MdOut *o, MdStr title) {
    for (i64 i = 0; i < title.len; i++) {
        if (title.data[i] == '`' || title.data[i] == '*') continue;
        md_escape(o, md_str(title.data + i, 1), 0);
    }
}

static void md_nav_link(MdOut *o, const MdNavItem *item, const char *rel, const char *before, const char *after) {
    md_puts(o, "<a rel=\"");
    md_puts(o, rel);
    md_puts(o, "\" href=\"");
    md_escape(o, item->page, 0);
    md_puts(o, ".html\">");
    md_puts(o, before);
    md_escape(o, item->label, 0);
    md_puts(o, after);
    md_puts(o, "</a>");
}

// Page `index` of the navigation from its Markdown `source` (`file` names it in messages): the body is converted into
// `scratch`, then the whole page written to `out`. 0 with `err` set on a problem (or when either buffer is too small).
b32 md_page(const MdNav *nav, i32 index, MdStr source, MdStr file, const MdHost *host, MdOut *scratch, MdOut *out, MdError *err) {
    MdPageState page;
    md_page_init(&page);
    MdCtx c = { host, scratch, err, &page, file, 0, 0, 0, { 0 } };
    md_blocks(&c, source, 1);
    if (c.failed) return 0;
    if (page.titles != 1) {
        md_set_error(err, file, 1, "a page has one # title", md_str(NULL, 0), "");
        return 0;
    }
    MdStr site = md_cstr("ronin");
    md_puts(out, "<!doctype html>\n<html lang=\"en\">\n<head>\n<meta charset=\"utf-8\">\n"
                 "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<title>");
    md_put_title(out, page.title);
    if (!md_equal(page.title, site)) md_puts(out, " &middot; ronin");
    md_puts(out, "</title>\n<link rel=\"stylesheet\" href=\"style.css\">\n<link rel=\"icon\" type=\"image/png\" href=\"images/logo.png\">\n"
                 "</head>\n<body>\n<a class=\"skip\" href=\"#main\">Skip to the text</a>\n<div class=\"page\">\n"
                 "<nav class=\"site\" aria-label=\"Documentation\">\n"
                 "<a class=\"brand\" href=\"index.html\"><img src=\"images/logo.png\" width=\"32\" height=\"32\" alt=\"\">ronin</a>\n");
    for (i32 i = 0; i < nav->count; i++) {
        const MdNavItem *item = &nav->items[i];
        if (!i || !md_equal(item->group, nav->items[i - 1].group)) {
            if (i) md_puts(out, "</ul>\n");
            md_puts(out, "<p class=\"group\">");
            md_escape(out, item->group, 0);
            md_puts(out, "</p>\n<ul>\n");
        }
        md_puts(out, "<li><a href=\"");
        md_escape(out, item->page, 0);
        md_puts(out, i == index ? ".html\" aria-current=\"page\">" : ".html\">");
        md_escape(out, item->label, 0);
        md_puts(out, "</a></li>\n");
    }
    md_puts(out, "</ul>\n<p class=\"repo\"><a href=\"" MD_REPO_URL "\">Source on GitHub</a></p>\n</nav>\n<main id=\"main\">\n");
    md_put(out, scratch->data, scratch->len);
    md_puts(out, "<nav class=\"pager\" aria-label=\"Previous and next page\">\n");
    if (index > 0) md_nav_link(out, &nav->items[index - 1], "prev", "&larr; ", "");
    if (index + 1 < nav->count) md_nav_link(out, &nav->items[index + 1], "next", "", " &rarr;");
    md_puts(out, "\n</nav>\n<footer>ronin is free software under the MIT license. <a href=\"" MD_REPO_URL "\">Source on GitHub</a></footer>\n"
                 "</main>\n</div>\n</body>\n</html>\n");
    if (scratch->full || out->full) {
        md_set_error(err, file, 1, "the page is larger than the output buffer", md_str(NULL, 0), "");
        return 0;
    }
    return 1;
}

#endif // RONIN_DEV || MAKE_DOCS
