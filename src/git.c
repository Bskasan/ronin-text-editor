// git.c — see git.h. The parsers of git's output; the worker that runs git.

// ---------------------------------------------------------------------------
// Parsing

// A line without its CR, cut at `max` bytes on a character boundary.
static String8 git_clean_line(String8 line, i64 max) {
    if (line.len && line.data[line.len - 1] == '\r') line.len--;
    if (line.len > max) {
        i64 n = max;
        while (n > 0 && (line.data[n] & 0xC0) == 0x80) n--; // never inside a character
        line.len = n;
    }
    return line;
}

static b32 git_starts(String8 s, const char *prefix) {
    return str8_starts_with(s, str8_cstr(prefix));
}

// "## main...origin/main [ahead 1]", "## No commits yet on main", "## Initial commit on master", "## HEAD (no branch)".
static void git_parse_branch(String8 rec, GitStatus *out) {
    String8 s = str8(rec.data + 3, rec.len - 3);
    static const char *const unborn[] = { "No commits yet on ", "Initial commit on " };
    for (i32 i = 0; i < 2; i++) {
        if (!git_starts(s, unborn[i])) continue;
        out->no_head = 1;
        i64 n = (i64)str8_cstr(unborn[i]).len;
        s = str8(s.data + n, s.len - n);
        break;
    }
    for (i64 i = 0; i + 2 < s.len; i++) { // the upstream after "..."
        if (s.data[i] == '.' && s.data[i + 1] == '.' && s.data[i + 2] == '.') {
            s.len = i;
            break;
        }
    }
    if (!git_starts(s, "HEAD (")) {
        for (i64 i = 0; i < s.len; i++) {
            if (s.data[i] == ' ') {
                s.len = i;
                break;
            }
        }
    }
    out->branch = s;
}

void git_parse_status(Arena *arena, String8 raw, i64 max_files, GitStatus *out) {
    memset(out, 0, sizeof(*out));
    i64 records = 0;
    for (i64 i = 0; i < raw.len; i++) records += raw.data[i] == 0;
    out->entries = PUSH_ARRAY(arena, GitEntry, MIN(records + 1, MAX(max_files, 0)) + 1);
    for (i64 at = 0; at < raw.len;) {
        i64 end = at;
        while (end < raw.len && raw.data[end]) end++;
        String8 rec = str8(raw.data + at, end - at);
        at = end + 1;
        if (rec.len >= 3 && rec.data[0] == '#' && rec.data[1] == '#' && rec.data[2] == ' ') {
            git_parse_branch(rec, out);
            continue;
        }
        if (rec.len < 4 || rec.data[2] != ' ') continue; // not a record of this format
        GitEntry e = { { rec.data[0], rec.data[1] }, str8(rec.data + 3, rec.len - 3), { 0 } };
        if (e.xy[0] == 'R' || e.xy[0] == 'C' || e.xy[1] == 'R' || e.xy[1] == 'C') { // the source is the next record
            i64 oend = at;
            while (oend < raw.len && raw.data[oend]) oend++;
            if (at < raw.len) e.orig = str8(raw.data + at, oend - at);
            at = oend + 1;
        }
        if (out->count >= max_files) {
            out->cut = 1;
            break;
        }
        out->entries[out->count++] = e;
    }
}

// "@@ -a[,b] +c[,d] @@ ...": the counts (b and d 1 when not given); false when it is not one.
static b32 git_parse_hunk(String8 s, i64 *old_count, i64 *new_start, i64 *new_count) {
    i64 i = 3, v[4] = { 0, 1, 0, 1 };
    for (i32 side = 0; side < 2; side++) {
        if (i >= s.len || s.data[i] != (side ? '+' : '-')) return 0;
        i++;
        if (i >= s.len || s.data[i] < '0' || s.data[i] > '9') return 0;
        for (v[side * 2] = 0; i < s.len && s.data[i] >= '0' && s.data[i] <= '9'; i++) v[side * 2] = v[side * 2] * 10 + (s.data[i] - '0');
        if (i < s.len && s.data[i] == ',') {
            i++;
            for (v[side * 2 + 1] = 0; i < s.len && s.data[i] >= '0' && s.data[i] <= '9'; i++) v[side * 2 + 1] = v[side * 2 + 1] * 10 + (s.data[i] - '0');
        }
        if (i >= s.len || s.data[i] != ' ') return 0;
        i++;
    }
    *old_count = v[1];
    *new_start = v[2];
    *new_count = v[3];
    return 1;
}

void git_parse_diff(Arena *arena, String8 raw, i64 max_lines, GitDiff *out) {
    memset(out, 0, sizeof(*out));
    out->text = raw;
    i64 lines = 0;
    for (i64 i = 0; i < raw.len; i++) lines += raw.data[i] == '\n';
    if (raw.len && raw.data[raw.len - 1] != '\n') lines++;
    max_lines = MAX(max_lines, 1);
    out->lines = PUSH_ARRAY(arena, GitLine, MIN(lines, max_lines) + 1);
    // Inside a hunk the counts decide what a line is ("--- x" there is a removed line, not a header).
    i64 old_left = 0, new_left = 0, new_line = 0;
    for (i64 at = 0; at < raw.len;) {
        i64 end = at;
        while (end < raw.len && raw.data[end] != '\n') end++;
        String8 line = git_clean_line(str8(raw.data + at, end - at), GIT_LINE_MAX);
        i64 off = at;
        at = end + 1;
        if (out->count >= max_lines) {
            out->cut = 1;
            out->lines[out->count++] = (GitLine){ (u32)off, 0, GIT_LINE_CUT, 0 };
            break;
        }
        u8 c = line.len ? line.data[0] : 0;
        GitLineKind kind = GIT_LINE_TEXT;
        if ((old_left > 0 || new_left > 0) && !git_starts(line, "diff --git ")) {
            if (c == '+') {
                kind = GIT_LINE_ADDED;
                if (!out->first_changed) out->first_changed = MAX(new_line, 1);
                new_left--;
                new_line++;
            } else if (c == '-') {
                kind = GIT_LINE_REMOVED;
                if (!out->first_changed) out->first_changed = MAX(new_line, 1);
                old_left--;
            } else if (c != '\\') { // context (an empty line counts as one)
                old_left--;
                new_left--;
                new_line++;
            }
        } else {
            i64 old_count, new_start, new_count;
            old_left = new_left = 0;
            if (git_starts(line, "@@ ") && git_parse_hunk(line, &old_count, &new_start, &new_count)) {
                kind = GIT_LINE_HUNK;
                old_left = old_count;
                new_left = new_count;
                new_line = new_count ? new_start : new_start + 1; // nothing on the new side: the line after
            } else if (c != '\\' && line.len) {
                kind = GIT_LINE_HEADER;
            }
        }
        out->lines[out->count++] = (GitLine){ (u32)off, (u32)line.len, (u32)kind, 0 };
    }
}

static b32 git_blank(String8 s) {
    for (i64 i = 0; i < s.len; i++) if (s.data[i] != ' ' && s.data[i] != '\t' && s.data[i] != '\r') return 0;
    return 1;
}

String8 git_error_line(String8 out) {
    String8 first = { 0 };
    for (i64 at = 0; at < out.len;) {
        i64 end = at;
        while (end < out.len && out.data[end] != '\n') end++;
        String8 line = git_clean_line(str8(out.data + at, end - at), out.len);
        at = end + 1;
        if (git_blank(line)) continue;
        if (git_starts(line, "fatal:") || git_starts(line, "error:") || git_starts(line, "usage:")) return git_clean_line(line, GIT_ERROR_LINE_MAX);
        if (!first.data) first = line;
    }
    return git_clean_line(first, GIT_ERROR_LINE_MAX);
}

b32 git_not_a_repository(String8 out) {
    String8 key = STR8_LIT("not a git repository");
    for (i64 i = 0; i + key.len <= out.len; i++) if (mem_equal(out.data + i, key.data, key.len)) return 1;
    return 0;
}
