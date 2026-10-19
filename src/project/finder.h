/* ======================================================================
   ronin: the projects kept, their scans and watches, the project finder

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// the projects the app keeps and the project finder (C-x p f). Part of the app: finder.c is
// included after files.c and uses App directly.

#ifndef FINDER_H
#define FINDER_H

#define PROJECT_KEPT 4          // indexes kept at once, the least recently used one goes first
#define PROJECT_DYING_MAX 16    // cancelled scans still ending (their batches are thrown away)
#define PROJECT_SHUTDOWN_MS 1000 // at exit every worker gets this long, in all, to end

typedef struct Project {
    u8 root_buf[PROJECT_PATH_MAX];
    String8 root;            // full path, no trailing separator; empty = a free slot
    ProjectIndex index[2];   // the live one and the one a rescan fills
    i32 live;
    ProjectScan *scan;       // running, or NULL
    b32 scan_shadow;         // it fills the other index (a rescan): swapped in when it ends
    b32 scanned;             // a scan of it has finished
    b32 stale;               // names changed under the root since the last scan started
    OsWatch watch;           // a tree watch on the root, one-shot (re-armed when a scan starts)
    ProjectScanResult last;  // of the last scan that finished
    u64 used;                // the last C-x p f on it
} Project;

typedef struct Finder {
    Project projects[PROJECT_KEPT];
    u64 tick;
    ProjectScan *dying[PROJECT_DYING_MAX];
    i32 dying_count;
    ProjectRootCache roots;
    u8 cmdline_buf[PROJECT_PATH_MAX];
    String8 cmdline_dir;     // ronin <directory>: the only root without a marker
    // The prompt.
    Project *open;           // the project it lists
    FuzzyRank rank;
    b32 empty;               // an empty input: the files visited this session, then the index in scan order
    Arena row_arena;         // the visited rows' relative paths
    String8 visited[MINI_HISTORY_MAX];
    i32 visited_count;
    u32 empty_rows[FUZZY_TOP];
    i32 empty_count;
    i64 empty_scanned;       // how far the index was looked at for them
#if RONIN_DEV
    i32 dev_hold_after;      // --scan-hold: every scan stands still after this many batches until released
    b32 dev_stuck_next;      // the next scan's worker is stuck in the "file system" (shutdown tests)
    ProjectScan *dev_last_scan;
    i32 dev_abandoned;       // workers left to the process exit by the shutdown
#endif
} Finder;

#endif // FINDER_H
