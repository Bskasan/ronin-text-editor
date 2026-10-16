// git_changes.h — the git changes view (Phase 19): show-git-changes (C-x g) opens a two-pane modal over the uncommitted
// changes of the project's git work tree, the files on the left, the selected file's diff on the right. Read only.
// git runs on workers (git.h); this is the app side: the job waiting for rev-parse before the box opens, the jobs
// of the open box, the jobs cancelled and still ending, and the copy of a diff into the modal's arena. Part of the
// app: git_changes.c is included after commands_list.c.

#ifndef GIT_CHANGES_H
#define GIT_CHANGES_H

#define GIT_DYING_MAX 16         // cancelled jobs still ending; with this many C-x g waits ("git is still stopping")
#define GIT_COPY_LINES_PER_CHECK 256 // a diff's copy into the modal arena looks at the clock this often

typedef struct GitView GitView; // the open box's state, in the modal's arena

typedef struct GitChanges {
    GitJob *pending;             // C-x g's job until rev-parse answers (the box opens then) or fails
    GitJob *dying[GIT_DYING_MAX]; // cancelled (the box closed, g) and still ending: freed when their last batch is in
    i32 dying_count;
    GitView *view;               // the open box, or NULL
    Arena arena;                 // the pending job's directory, git-changes-visit's target (made on first use)
    String8 pending_dir;
    String8 visit_path;          // git-changes-visit: the file and its 1-based line (0: where find-file puts point)
    i64 visit_line;
    b32 work_pending;            // a diff is being copied into the box: another frame
#if RONIN_DEV
    String8 dev_program, dev_prefix, dev_env; // tests: the fake git, or the real one with an isolated configuration
    i32 dev_abandoned;           // workers left to the process exit by the shutdown
    u64 dev_diff_jobs;           // diff jobs started (the cache: once per file per open)
    u64 dev_copy_max_us;         // the longest copy slice since it was last read (--bench-git)
#endif
} GitChanges;

extern const Command CMD_SHOW_GIT_CHANGES, CMD_GIT_CHANGES_REFRESH;

#endif // GIT_CHANGES_H
