/* ======================================================================
   ronin: build tool: converts docs/src/*.md into the documentation site

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// The documentation site's generator (Phase 24; build-time tool, not part of the editor; build.bat compiles it on
// every build, build.bat docs and verify.bat's docs-check run it):
//   make_docs <docs dir> <fragments dir> <out dir>
//       every page of <docs dir>\src\nav.txt, <docs dir>\src\<page>.md, into <out dir>\<page>.html; the fragments of
//       <!-- generated: NAME --> come from <fragments dir>\NAME.md (ronin_debug.exe --dump-docs), images are looked
//       up under <docs dir> (their size from the PNG header). Every .md of <docs dir>\src must be in nav.txt.
//   make_docs --compare <docs dir> <scratch dir>
//       the site as committed against one generated into <scratch dir> (its pages, gen\*.md and logo.png): every
//       difference, a page missing on either side or a stale fragment is one line; exit 1 when there is any.
// The conversion is src/dev/markdown.c, the file --test checks. Sources are read with CRLF turned into LF; every file
// written is LF only. Messages are "path:line: message". No C runtime calls: files are read and written with Win32.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>

typedef unsigned char u8;
typedef int i32;
typedef unsigned int u32;
typedef long long i64;
typedef unsigned long long u64;
typedef int b32;

#define MAKE_DOCS 1
#include "../src/dev/markdown.c"

#define FILE_CAP (4 << 20)
#define PATH_CAP 1024

static u8 source[FILE_CAP], fragment[FILE_CAP], body[FILE_CAP], page_out[FILE_CAP], other[FILE_CAP];
static MdNav nav;
static b32 failed;

// ---------------------------------------------------------------------------
// Text and paths

typedef struct Path {
    char text[PATH_CAP];
    i64 len;
} Path;

static void path_add(Path *p, const u8 *data, i64 len) {
    for (i64 i = 0; i < len && p->len < PATH_CAP - 1; i++) p->text[p->len++] = data[i] == '/' ? '\\' : (char)data[i];
    p->text[p->len] = 0;
}

static void path_adds(Path *p, const char *c) {
    MdStr s = md_cstr(c);
    path_add(p, s.data, s.len);
}

static Path path_of(const char *dir, const char *a, MdStr b, const char *z) {
    Path p = { { 0 }, 0 };
    path_adds(&p, dir);
    path_adds(&p, a);
    path_add(&p, b.data, b.len);
    path_adds(&p, z);
    return p;
}

static void out_write(MdStr s) {
    DWORD written;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), s.data, (DWORD)s.len, &written, NULL);
}

static void say(const char *a, MdStr b, const char *c) {
    out_write(md_cstr(a));
    out_write(b);
    out_write(md_cstr(c));
}

static void say_error(const MdError *err) {
    MdOut line = { other, 0, 64, 0 };
    md_put_int(&line, err->line);
    out_write(err->file);
    out_write(md_cstr(":"));
    out_write(md_str(other, line.len));
    out_write(md_cstr(": "));
    out_write(md_str(err->text, err->text_len));
    out_write(md_cstr("\n"));
    failed = 1;
}

// ---------------------------------------------------------------------------
// Files

// The whole file into buf, CRLF turned into LF; -1 when it cannot be read (or is larger than the buffer).
static i64 read_file(const char *path, u8 *buf, i64 cap) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER size;
    DWORD got = 0;
    b32 ok = GetFileSizeEx(f, &size) && size.QuadPart < cap && ReadFile(f, buf, (DWORD)size.QuadPart, &got, NULL) && got == size.QuadPart;
    CloseHandle(f);
    if (!ok) return -1;
    i64 n = 0;
    for (i64 i = 0; i < (i64)got; i++) if (!(buf[i] == '\r' && i + 1 < (i64)got && buf[i + 1] == '\n')) buf[n++] = buf[i];
    return n;
}

// The bytes as they are (a PNG, a page to compare); -1 when it cannot be read.
static i64 read_raw(const char *path, u8 *buf, i64 cap) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return -1;
    LARGE_INTEGER size;
    DWORD got = 0;
    b32 ok = GetFileSizeEx(f, &size) && size.QuadPart < cap && ReadFile(f, buf, (DWORD)size.QuadPart, &got, NULL) && got == size.QuadPart;
    CloseHandle(f);
    return ok ? (i64)got : -1;
}

static b32 write_file(const char *path, const u8 *data, i64 len) {
    HANDLE f = CreateFileA(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD written = 0;
    b32 ok = WriteFile(f, data, (DWORD)len, &written, NULL) && written == (DWORD)len;
    CloseHandle(f);
    return ok;
}

// ---------------------------------------------------------------------------
// The converter's host: fragments, includes, image sizes

typedef struct Host {
    const char *docs, *fragments;
} Host;

static b32 host_resolve(void *user, b32 generated, MdStr name, MdStr *text, MdStr *link_dir) {
    Host *h = (Host *)user;
    Path p = generated ? path_of(h->fragments, "\\", name, ".md") : path_of(h->docs, "\\src\\", name, "");
    i64 n = read_file(p.text, fragment, FILE_CAP);
    if (n < 0) return 0;
    *text = md_str(fragment, n);
    *link_dir = md_str(NULL, 0);
    if (!generated) {
        // An include names a repository file from docs/src: "../../PATH". Its links are relative to PATH's folder.
        if (!md_starts(name, "../../")) return 0;
        MdStr rest = md_skip(name, 6);
        i64 slash = rest.len;
        while (slash > 0 && rest.data[slash - 1] != '/') slash--;
        *link_dir = md_str(rest.data, slash);
    }
    return 1;
}

static MdStr missing_image;

static b32 host_image_size(void *user, MdStr src, i32 *width, i32 *height) {
    Host *h = (Host *)user;
    if (md_external(src)) return 0;
    Path p = path_of(h->docs, "\\", src, "");
    u8 head[24];
    HANDLE f = CreateFileA(p.text, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    DWORD got = 0;
    b32 ok = f != INVALID_HANDLE_VALUE && ReadFile(f, head, sizeof(head), &got, NULL) && got == sizeof(head) && head[1] == 'P' && head[2] == 'N' &&
             head[3] == 'G';
    if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
    if (!ok) {
        if (!missing_image.len) missing_image = src;
        return 0;
    }
    *width = (i32)((u32)head[16] << 24 | (u32)head[17] << 16 | (u32)head[18] << 8 | head[19]);
    *height = (i32)((u32)head[20] << 24 | (u32)head[21] << 16 | (u32)head[22] << 8 | head[23]);
    return 1;
}

// ---------------------------------------------------------------------------
// Generating

static u8 nav_text[1 << 16];

static b32 load_nav(const char *docs) {
    Path p = path_of(docs, "\\src\\nav.txt", md_str(NULL, 0), "");
    i64 n = read_file(p.text, nav_text, sizeof(nav_text));
    MdError err = { 0 };
    if (n < 0) {
        say("", md_cstr(p.text), ": cannot read the navigation\n");
        return 0;
    }
    if (!md_nav_parse(md_str(nav_text, n), md_cstr(p.text), &nav, &err)) {
        say_error(&err);
        return 0;
    }
    return 1;
}

static i32 generate(const char *docs, const char *fragments, const char *out_dir) {
    if (!load_nav(docs)) return 1;
    // Every page of docs\src is in the navigation (the converse is checked as each page is read).
    Path pattern = path_of(docs, "\\src\\*.md", md_str(NULL, 0), "");
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern.text, &fd);
    if (find != INVALID_HANDLE_VALUE) {
        do {
            MdStr name = md_cstr(fd.cFileName);
            MdStr page = md_str(name.data, name.len - 3);
            b32 listed = 0;
            for (i32 i = 0; i < nav.count; i++) listed |= md_equal(nav.items[i].page, page);
            if (!listed) {
                say("", md_cstr(pattern.text), "");
                say(": ", name, " is not in nav.txt\n");
                failed = 1;
            }
        } while (FindNextFileA(find, &fd));
        FindClose(find);
    }
    Host host = { docs, fragments };
    MdHost md_host = { &host, host_resolve, host_image_size };
    for (i32 i = 0; i < nav.count; i++) {
        Path src = path_of(docs, "\\src\\", nav.items[i].page, ".md");
        Path shown = path_of(docs, "/src/", nav.items[i].page, ".md");
        for (i64 k = 0; k < shown.len; k++) if (shown.text[k] == '\\') shown.text[k] = '/';
        i64 n = read_file(src.text, source, FILE_CAP);
        if (n < 0) {
            say("", md_cstr(shown.text), ": cannot read the page (it is in nav.txt)\n");
            failed = 1;
            continue;
        }
        MdOut scratch = { body, 0, FILE_CAP, 0 }, out = { page_out, 0, FILE_CAP, 0 };
        MdError err = { 0 };
        missing_image = md_str(NULL, 0);
        if (!md_page(&nav, i, md_str(source, n), md_cstr(shown.text), &md_host, &scratch, &out, &err)) {
            say_error(&err);
            continue;
        }
        if (missing_image.len) {
            say("", md_cstr(shown.text), ": no PNG image ");
            say("", missing_image, " under the docs folder\n");
            failed = 1;
            continue;
        }
        Path dst = path_of(out_dir, "\\", nav.items[i].page, ".html");
        if (!write_file(dst.text, out.data, out.len)) {
            say("", md_cstr(dst.text), ": cannot write the page\n");
            failed = 1;
        }
    }
    return failed ? 1 : 0;
}

// ---------------------------------------------------------------------------
// Comparing

static void compare_file(const char *committed, const char *fresh) {
    i64 a = read_raw(committed, source, FILE_CAP), b = read_raw(fresh, other, FILE_CAP);
    if (a < 0 && b < 0) return;
    if (a < 0) say("", md_cstr(committed), ": missing (run build.bat docs)\n");
    else if (b < 0) say("", md_cstr(committed), ": no source makes it any more (remove it, or run build.bat docs)\n");
    else if (a != b || !md_equal(md_str(source, a), md_str(other, b))) say("", md_cstr(committed), ": differs from its sources (run build.bat docs)\n");
    else return;
    failed = 1;
}

// The pages of `dir` (one side) by name against the other side; with only_missing just those the other side lacks.
static void compare_pages(const char *dir, const char *docs, const char *scratch, b32 only_missing) {
    Path pattern = path_of(dir, "\\*.html", md_str(NULL, 0), "");
    WIN32_FIND_DATAA fd;
    HANDLE find = FindFirstFileA(pattern.text, &fd);
    if (find == INVALID_HANDLE_VALUE) return;
    do {
        MdStr name = md_cstr(fd.cFileName);
        Path a = path_of(docs, "\\", name, ""), b = path_of(scratch, "\\", name, "");
        if (!only_missing || GetFileAttributesA(a.text) == INVALID_FILE_ATTRIBUTES) compare_file(a.text, b.text);
    } while (FindNextFileA(find, &fd));
    FindClose(find);
}

static i32 compare(const char *docs, const char *scratch) {
    compare_pages(docs, docs, scratch, 0);
    compare_pages(scratch, docs, scratch, 1); // pages only the new site has: missing from the committed one
    static const char *fragments[] = { "keys.md", "settings.md" };
    for (i32 i = 0; i < 2; i++) {
        Path a = path_of(docs, "\\src\\gen\\", md_cstr(fragments[i]), ""), b = path_of(scratch, "\\gen\\", md_cstr(fragments[i]), "");
        compare_file(a.text, b.text);
    }
    Path a = path_of(docs, "\\images\\logo.png", md_str(NULL, 0), ""), b = path_of(scratch, "\\logo.png", md_str(NULL, 0), "");
    compare_file(a.text, b.text);
    if (!failed) say("docs-check: PASS: the site in ", md_cstr(docs), " is what its sources make\n");
    return failed ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 4 && md_equal(md_cstr(argv[1]), md_cstr("--compare"))) return compare(argv[2], argv[3]);
    if (argc == 4) return generate(argv[1], argv[2], argv[3]);
    say("usage: make_docs <docs dir> <fragments dir> <out dir> | make_docs --compare <docs dir> <scratch dir>", md_str(NULL, 0), "\n");
    return 2;
}
