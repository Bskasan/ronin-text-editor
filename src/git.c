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
        // stderr shares the pipe: a warning ("warning: in the working copy of 'x', LF will be replaced by CRLF ...")
        // ends with a newline, which no record holds on Windows; the record is what follows the last one.
        for (i64 i = rec.len; i > 0; i--) {
            if (rec.data[i - 1] == '\n') {
                rec = str8(rec.data + i, rec.len - i);
                break;
            }
        }
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
            if (git_starts(line, "warning: ")) continue; // git's stderr in the shared pipe (CRLF conversion ...): not the diff
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

// ---------------------------------------------------------------------------
// The worker

typedef enum GitRunEnd { GIT_RUN_DONE, GIT_RUN_CUT, GIT_RUN_CANCELLED, GIT_RUN_TIMEOUT, GIT_RUN_NO_START } GitRunEnd;

static b32 git_cancelled(GitJob *j) {
    return os_atomic_get(&j->cancel) != 0;
}

// One git command in `dir`, its output read to its end into the job's arena (from its current position, so the
// output is contiguous). Past `limit` separators (lines, records) or GIT_OUTPUT_MAX bytes git is stopped: CUT.
static GitRunEnd git_run(GitJob *j, String8 program, String8 args, String8 dir, u8 sep, i64 limit, String8 *out, u32 *exit_code) {
    Arena *a = &j->arena;
    *out = str8(NULL, 0);
    *exit_code = 0;
    String8 line = j->req.prefix.len ? str8_fmt(a, "%S %s %S", j->req.prefix, GIT_COMMON_ARGS, args) : str8_fmt(a, "%s %S", GIT_COMMON_ARGS, args);
    OsProcess proc;
    if (os_process_start_program(a, program, line, dir, j->req.env, &proc) != OS_FILE_OK) return GIT_RUN_NO_START;
#if RONIN_DEV
    os_atomic_add(&j->dev_processes, 1);
#endif
    u64 start = arena_pos(a);
    u8 *data = NULL;
    i64 n = 0, seps = 0;
    GitRunEnd end = GIT_RUN_DONE;
    b32 failed = 0;
    for (;;) {
        if (git_cancelled(j)) {
            end = GIT_RUN_CANCELLED;
            break;
        }
        u8 *chunk = (u8 *)arena_push(a, GIT_READ_BYTES, 1);
        if (!data) data = chunk;
        i64 got = 0;
        OsProcessRead r = os_process_read(&proc, j->cancel_event, j->req.timeout_ms, chunk, GIT_READ_BYTES, &got);
        arena_pop_to(a, start + (u64)(n + got));
        if (r == OS_PROCESS_DATA) {
            for (i64 i = 0; i < got; i++) seps += chunk[i] == sep;
            n += got;
            if (seps > limit || n >= GIT_OUTPUT_MAX) {
                end = GIT_RUN_CUT;
                break;
            }
            continue;
        }
        if (r == OS_PROCESS_CANCELLED) end = GIT_RUN_CANCELLED;
        else if (r == OS_PROCESS_TIMEOUT) end = GIT_RUN_TIMEOUT;
        else failed = r == OS_PROCESS_ERROR;
        break;
    }
    b32 stop = end != GIT_RUN_DONE;
    if (stop) os_process_kill(&proc); // git and everything it started
    os_process_wait(&proc, stop ? 1000 : 5000);
    *exit_code = failed ? 0xFFFFFFFFu : os_process_exit_code(&proc);
    os_process_close(&proc, 1);
    *out = str8(data, n);
    return end;
}

// The batch to the main thread: linked under the lock, then the main loop woken. After the last one the worker
// touches nothing of the job.
static void git_send(GitJob *j, GitBatch *b, b32 last) {
    b->last = last;
    os_lock(&j->lock);
    if (j->ready_last) j->ready_last->next = b;
    else j->ready = b;
    j->ready_last = b;
    os_unlock(&j->lock);
    os_wake_main();
}

// A step that did not end with git's answer: the batch that says why (always the last).
static void git_send_end(GitJob *j, GitRunEnd end, u32 exit_code, String8 out, String8 program) {
    GitBatch *b = PUSH_STRUCT(&j->arena, GitBatch);
    b->exit_code = exit_code;
    b->text = out;
    if (end == GIT_RUN_CANCELLED) {
        b->kind = GIT_BATCH_CANCELLED;
    } else if (end == GIT_RUN_TIMEOUT) {
        b->kind = GIT_BATCH_TIMEOUT;
    } else if (end == GIT_RUN_NO_START) {
        b->kind = GIT_BATCH_FAILED;
        b->exit_code = 0xFFFFFFFFu;
        b->error = str8_fmt(&j->arena, "cannot start %S", program);
    } else {
        b->kind = j->req.kind == GIT_JOB_OPEN && git_not_a_repository(out) ? GIT_BATCH_NOT_WORK_TREE : GIT_BATCH_FAILED;
        b->error = git_error_line(out);
    }
    git_send(j, b, 1);
}

// The top level as rev-parse printed it ("C:/x/y"), as a directory to run in ("C:\x\y").
static String8 git_toplevel(Arena *a, String8 out) {
    i64 n = 0;
    while (n < out.len && out.data[n] != '\n' && out.data[n] != '\r') n++;
    String8 top = str8_copy(a, str8(out.data, n));
    for (i64 i = 0; i < top.len; i++) if (top.data[i] == '/') top.data[i] = '\\';
    return top;
}

static void git_worker_main(void *data) {
    GitJob *j = data;
    GitRequest *q = &j->req;
    Arena *a = &j->arena;
    String8 program = q->program, out;
    u32 code = 0;
    GitRunEnd end;
    String8 dir = q->dir;
    if (q->kind == GIT_JOB_OPEN) {
        // The program: git_path when set, else git.exe on PATH.
        OsFileInfo info;
        b32 found = program.len ? os_file_info_arena(a, program, &info) == OS_FILE_OK && info.exists && !info.is_dir
                                : os_find_program(a, STR8_LIT("git.exe"), &program);
        if (!found) {
            GitBatch *b = PUSH_STRUCT(a, GitBatch);
            b->kind = GIT_BATCH_NO_GIT;
            b->text = q->program;
            git_send(j, b, 1);
            return;
        }
        end = git_run(j, program, STR8_LIT("rev-parse --show-toplevel"), dir, '\n', 64, &out, &code);
        if (end != GIT_RUN_DONE || code != 0) {
            git_send_end(j, end, code, out, program);
            return;
        }
        GitBatch *b = PUSH_STRUCT(a, GitBatch);
        b->kind = GIT_BATCH_TOPLEVEL;
        b->text = git_toplevel(a, out);
        b->program = program;
        dir = b->text;
        git_send(j, b, 0);
    }
    if (q->kind == GIT_JOB_OPEN || q->kind == GIT_JOB_STATUS) {
        // Two records a file at most (a rename's source) and the branch's: past that, more files than the limit.
        end = git_run(j, program, STR8_LIT("status --porcelain=v1 -z --branch --untracked-files=all"), dir, 0, 2 * q->max_files + 2, &out,
                      &code);
        if (!(end == GIT_RUN_CUT || (end == GIT_RUN_DONE && code == 0))) {
            git_send_end(j, end, code, out, program);
            return;
        }
        GitBatch *b = PUSH_STRUCT(a, GitBatch);
        b->kind = GIT_BATCH_STATUS;
        git_parse_status(a, out, q->max_files, &b->status);
        git_send(j, b, 1);
        return;
    }
    // DIFF
    String8 args = q->mode == GIT_DIFF_NEW    ? str8_fmt(a, "diff --no-index --no-color --no-ext-diff -- /dev/null \"%S\"", q->path)
                 : q->mode == GIT_DIFF_RENAME ? str8_fmt(a, "diff HEAD --no-color --no-ext-diff -M -- \"%S\" \"%S\"", q->orig, q->path)
                                              : str8_fmt(a, "diff HEAD --no-color --no-ext-diff -- \"%S\"", q->path);
    end = git_run(j, program, args, dir, '\n', q->max_lines, &out, &code);
    if (!(end == GIT_RUN_CUT || (end == GIT_RUN_DONE && (code == 0 || (q->mode == GIT_DIFF_NEW && code == 1))))) {
        git_send_end(j, end, code, out, program);
        return;
    }
    GitBatch *b = PUSH_STRUCT(a, GitBatch);
    b->kind = GIT_BATCH_DIFF;
    git_parse_diff(a, out, q->max_lines, &b->diff);
    git_send(j, b, 1);
}

// ---------------------------------------------------------------------------
// The main thread's side

GitJob *git_job_create(const GitRequest *req) {
    i64 max_lines = MAX(req->max_lines, 1), max_files = MAX(req->max_files, 1);
    u64 reserve = GIT_OUTPUT_MAX + MB(16) + (u64)(max_lines + 2) * sizeof(GitLine) + (u64)(max_files + 2) * sizeof(GitEntry);
    Arena arena = arena_create(reserve);
    if (!arena.base) return NULL;
    GitJob *j = PUSH_STRUCT(&arena, GitJob);
    j->req = *req;
    j->req.program = str8_copy(&arena, req->program);
    j->req.prefix = str8_copy(&arena, req->prefix);
    j->req.env = str8_copy(&arena, req->env);
    j->req.dir = str8_copy(&arena, req->dir);
    j->req.path = str8_copy(&arena, req->path);
    j->req.orig = str8_copy(&arena, req->orig);
    j->req.max_lines = max_lines;
    j->req.max_files = max_files;
    j->arena = arena; // its position already counts this struct
    return j;
}

b32 git_job_start(GitJob *j) {
    if (!os_event_create(&j->cancel_event)) return 0;
    if (!os_thread_start(&j->thread, git_worker_main, j, 1)) return 0;
    j->started = 1;
    return 1;
}

void git_job_cancel(GitJob *j) {
    os_atomic_set(&j->cancel, 1);
    if (j->cancel_event.handle) os_event_set(j->cancel_event);
}

GitBatch *git_job_take(GitJob *j) {
    os_lock(&j->lock);
    GitBatch *b = j->ready;
    j->ready = j->ready_last = NULL;
    os_unlock(&j->lock);
    return b;
}

void git_job_ended(GitJob *j) {
    if (j->started && !j->ended) os_thread_join(j->thread, 0xFFFFFFFFu); // past its last call: no wait
    j->thread.handle = NULL;
    j->ended = 1;
}

b32 git_job_join(GitJob *j, u64 deadline_us) {
    if (!j->started || j->ended) return 1;
    u64 now = os_time_us();
    u32 left = now >= deadline_us ? 0 : (u32)((deadline_us - now) / 1000);
    if (!os_thread_join(j->thread, left)) return 0;
    j->thread.handle = NULL;
    j->ended = 1;
    return 1;
}

void git_job_free(GitJob *j) {
    if (j->cancel_event.handle) os_event_destroy(j->cancel_event);
    os_release(j->arena.base); // the struct itself lives there
}
