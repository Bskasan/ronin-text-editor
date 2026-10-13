// platform.h — the only OS interface the editor core sees.
// Implemented by win32_main.c.

#ifndef PLATFORM_H
#define PLATFORM_H

// ---------------------------------------------------------------------------
// OS primitives

void *os_reserve(u64 size);
b32   os_commit(void *ptr, u64 size);
void  os_decommit(void *ptr, u64 size); // back to reserved only
b32   os_release(void *ptr); // a whole os_reserve range
b32   os_write_file(String8 path, String8 data);
void  os_fatal(String8 message); // does not return
u64   os_time_us(void);              // monotonic microseconds
void  os_set_window_title(String8 title);
void  os_set_caption_color(u32 rgb); // the title bar (Windows 11; ignored elsewhere)
// Whether some file-visiting buffer has unsaved changes. The window procedure answers Windows'
// end-of-session query from it at once; false also removes the shutdown block reason.
void  os_set_unsaved_files(b32 any);
String8 os_exe_dir(Arena *arena);                  // the directory of the executable, no trailing separator
String8 os_get_env(Arena *arena, String8 name);    // empty when not set
b32   os_make_dir(String8 path);                   // true if it exists afterwards (the parent must exist)
String8 os_key_name(Arena *arena, u32 scancode);  // the keyboard's name for a key (Event.scancode), empty if none
#if TEAL_DEV
void  os_log_write(String8 text);
// Startup timeline: records the time of a stage (a static string) of the first startup; the
// platform logs every stage once after the first frame. Recording costs a counter read.
void  os_dev_stage(const char *what);
#endif

// ---------------------------------------------------------------------------
// Files. Every failure is reported as a status the caller can show; nothing asserts.

typedef enum OsFileStatus {
    OS_FILE_OK,
    OS_FILE_NOT_FOUND,         // file or directory
    OS_FILE_ACCESS_DENIED,
    OS_FILE_SHARING_VIOLATION, // open elsewhere without sharing
    OS_FILE_TOO_LARGE,
    OS_FILE_DISK_FULL,
    OS_FILE_IS_DIRECTORY,
    OS_FILE_BAD_PATH,
    OS_FILE_READ_ONLY,
    OS_FILE_OUT_OF_MEMORY,
    OS_FILE_NO_PATH,           // the buffer is not visiting a file
    OS_FILE_IO_ERROR,          // anything else
} OsFileStatus;

typedef struct OsFile {
    void *handle;
} OsFile;

typedef struct OsFileInfo {
    b32 exists;
    b32 is_dir;
    b32 read_only;
    b32 swap_ok;    // replacing the file by another one keeps it intact: not a symlink, one hard link
    i64 size;
    u64 write_time; // FILETIME ticks
} OsFileInfo;

String8      os_full_path(Arena *arena, String8 path); // absolute, normalized; empty on failure
OsFileStatus os_file_info(String8 path, OsFileInfo *info);

typedef struct OsDirEntry {
    String8 name; // UTF-8, no directory part
    b32 is_dir;
    b32 reparse;  // a reparse point: a symlink, a junction, a cloud placeholder ...
} OsDirEntry;

// The entries of a directory, "." and ".." left out, in the order the file system gives them. The
// array, the names and the listing's temporary memory go into `arena`. Any thread.
OsFileStatus os_list_dir(Arena *arena, String8 dir, OsDirEntry **entries, i64 *count);
// A whole file of at most `max` bytes (else OS_FILE_TOO_LARGE), read into `arena`. Any thread.
OsFileStatus os_read_small_file(Arena *arena, String8 path, i64 max, String8 *out);
OsFileStatus os_file_open_read(String8 path, OsFile *file, OsFileInfo *info); // shares read, write, delete
OsFileStatus os_file_read(OsFile file, void *dst, i64 size); // exactly size bytes
// A new, empty file next to `target` (same directory, so it can replace it); its path goes into `arena`.
OsFileStatus os_file_create_temp(String8 target, Arena *arena, OsFile *file, String8 *temp_path);
OsFileStatus os_file_open_overwrite(String8 path, OsFile *file); // opened (or created) and truncated
OsFileStatus os_file_write(OsFile file, void *data, i64 size);
OsFileStatus os_file_flush(OsFile file); // to the disk, not just the cache
void         os_file_close(OsFile file);
// Swaps `temp` in as `target`, keeping the target's attributes; a plain move when target does not exist.
OsFileStatus os_file_replace(String8 target, String8 temp);
void         os_file_delete(String8 path);
#if TEAL_DEV
b32          os_dev_set_read_only(String8 path, b32 read_only);
b32          os_dev_hard_link(String8 existing, String8 link);
#endif

// ---------------------------------------------------------------------------
// The clipboard: CF_UNICODETEXT, CRLF by convention (converted here). A busy clipboard is retried
// a few times. Dev builds can replace it by an in-memory fake (tests, smoke, benches, screenshots).

b32  os_clipboard_set(String8 text);
b32  os_clipboard_get(Arena *arena, String8 *text); // false when it holds no text
u32  os_clipboard_seq(void);                       // changes whenever any program sets it
#if TEAL_DEV
void os_dev_clipboard_fake(b32 on);
void os_dev_clipboard_external(String8 text); // the fake: as if another program copied text
#endif

// ---------------------------------------------------------------------------
// Directory watches: a change notification (file names, sizes, write times) on one directory,
// delivered as EVENT_DIR_CHANGED. Waited on with the messages: no polling, no timers.

typedef u32 OsWatch; // 0 = none
struct Event;

OsWatch os_watch_dir(String8 dir); // 0 on failure (no such directory, too many watches)
// A whole tree (names of files and directories only), one-shot: it delivers one EVENT_DIR_CHANGED,
// then nothing until os_watch_rearm, however many changes follow (a build creating hundreds of files
// is one wake-up).
OsWatch os_watch_tree(String8 dir);
b32     os_watch_rearm(OsWatch watch, String8 dir); // a tree watch: a fresh notification (earlier changes forgotten)
void    os_unwatch(OsWatch watch);
#if TEAL_DEV
b32     os_dev_watch_wait(OsWatch watch, u32 timeout_ms); // true when it signalled (then re-armed)
i32     os_dev_watch_count(void);                         // watches still open
i32     os_dev_poll_watches(struct Event *out, i32 cap);  // the platform's watch poll once: its events (taken off the queue)
b32     os_dev_unsaved_files(void);                       // what the app last reported (os_set_unsaved_files)
OsFileStatus os_dev_lock_file(String8 path, OsFile *file); // opened without sharing: readers get a sharing violation
#endif

// ---------------------------------------------------------------------------
// Threads (CLAUDE.md has the rules). Everything in this section may be called from any thread, as
// may os_reserve / os_commit / os_decommit / os_release, os_time_us, os_list_dir,
// os_read_small_file and os_file_close. Nothing else in this file may be called from a worker.

// A file opened for reading, the path converted in the caller's arena (popped again). Shares read, write
// and delete, so a reader never makes a save, a rename or a delete by anyone else fail. *size: its size.
OsFileStatus os_file_open_read_arena(Arena *arena, String8 path, OsFile *file, i64 *size);
// One read of at most `cap` bytes; *got 0 at the end of the file (a file that shrank ends early).
OsFileStatus os_file_read_some(OsFile file, void *dst, i64 cap, i64 *got);
i32  os_cpu_count(void); // logical processors (at least 1)

typedef struct OsThread { void *handle; } OsThread;
typedef void OsThreadFn(void *data);
// `background`: a worker one step below normal priority (the project scan, the search), so the main
// thread wins when every core is busy.
b32  os_thread_start(OsThread *thread, OsThreadFn *fn, void *data, b32 background);
// True when the thread has ended within the timeout (its handle is then closed); false: still running.
b32  os_thread_join(OsThread thread, u32 timeout_ms);

typedef struct OsLock { void *state; } OsLock; // a slim reader-writer lock, exclusive only; zeroed = unlocked
void os_lock(OsLock *lock);
void os_unlock(OsLock *lock);

typedef struct OsEvent { void *handle; } OsEvent; // auto-reset: one wait consumes a set
b32  os_event_create(OsEvent *event);
void os_event_set(OsEvent event);
b32  os_event_wait(OsEvent event, u32 timeout_ms); // true when it was set
void os_event_destroy(OsEvent event);

i32  os_atomic_get(volatile i32 *v);
void os_atomic_set(volatile i32 *v, i32 x);
i32  os_atomic_add(volatile i32 *v, i32 x);                 // returns the new value
i32  os_atomic_cas(volatile i32 *v, i32 expected, i32 x);   // sets x if *v == expected; returns the old value

// The main loop calls app_wake soon after (sets coalesce): a worker has results.
void os_wake_main(void);

// A file's attributes, the path converted in the caller's arena (popped again). swap_ok is not set.
OsFileStatus os_file_info_arena(Arena *arena, String8 path, OsFileInfo *info);
// Bytes in the system's OEM code page (a console program's output) as UTF-8, in `arena`.
String8 os_oem_to_utf8(Arena *arena, String8 text);

// Processes: a command run through cmd.exe /s /c in a directory, with no window, stdin from the NUL
// device, stdout and stderr into one pipe, the whole tree inside a job object (killing the job ends every
// process in it; so does teal's exit, a crash included). Only the pipe and NUL are inherited. Any thread;
// one OsProcess is used by one thread at a time (the build's reader worker: the main thread never touches
// one).
typedef struct OsProcess {
    void *process, *job, *pipe, *io_event;
    u32 pid;
    b32 exited; // cmd.exe has ended: only what is already in the pipe is left to read
} OsProcess;

typedef enum OsProcessRead {
    OS_PROCESS_DATA,      // *got > 0 bytes
    OS_PROCESS_EXITED,    // cmd.exe has ended and the pipe holds nothing more (or every writer closed it)
    OS_PROCESS_CANCELLED, // `cancel` was set (nothing read)
    OS_PROCESS_ERROR,
} OsProcessRead;

// Started, or a status (a directory that does not exist: OS_FILE_NOT_FOUND / OS_FILE_BAD_PATH) with
// nothing left open. Temporary memory in `arena` (popped again).
OsFileStatus  os_process_start(Arena *arena, String8 command, String8 dir, OsProcess *p);
// Waits for output, the end of cmd.exe or `cancel` (an event set only to cancel; it is left set), whichever
// comes first.
OsProcessRead os_process_read(OsProcess *p, OsEvent cancel, void *dst, i64 cap, i64 *got);
b32  os_process_wait(OsProcess *p, u32 timeout_ms); // cmd.exe has ended within the timeout
u32  os_process_exit_code(OsProcess *p);            // after it ended
void os_process_kill(OsProcess *p);                 // every process of the job, at once
// Every handle. kill_rest: processes still in the job end too; otherwise they are let go (build servers
// that a finished build left running).
void os_process_close(OsProcess *p, b32 kill_rest);
#if TEAL_DEV
b32  os_dev_wake_wait(u32 timeout_ms); // headless tests: the wake was set (and is consumed)
i32  os_dev_live_threads(void);        // started and not joined (nor abandoned)
void os_dev_thread_abandoned(void);    // a worker left to the process exit at shutdown: not a leak
void os_dev_thread_reclaimed(void);    // ... joined after all (tests): os_thread_join counts it again
i32  os_dev_live_events(void);         // created and not destroyed
void os_dev_background_normal(b32 on); // --bench-grep: background workers start at normal priority (the comparison)
i32  os_dev_thread_priority(OsThread thread); // the thread's priority (Win32 value: 0 normal, -1 below normal)
b32  os_dev_make_junction(String8 link, String8 target); // a new directory junction (no admin rights needed)
b32  os_dev_remove_dir(String8 path);                     // an empty directory, or a junction (not its target)
i32  os_dev_live_processes(void);      // os_process_start minus os_process_close
b32  os_dev_process_alive(u32 pid);    // a process with this id is running
void os_dev_kill_pid(u32 pid);         // tests: a leftover they made goes
i32  os_dev_handle_count(void);        // the process's open kernel handles
String8 os_dev_exe_path(Arena *arena); // this executable (the --child program of the tests)
#endif

// ---------------------------------------------------------------------------
// Input

typedef enum Key {
    KEY_NONE,
    KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G, KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M,
    KEY_N, KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U, KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z,
    KEY_0, KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9,
    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6, KEY_F7, KEY_F8, KEY_F9, KEY_F10, KEY_F11, KEY_F12,
    KEY_F13, KEY_F14, KEY_F15, KEY_F16, KEY_F17, KEY_F18, KEY_F19, KEY_F20, KEY_F21, KEY_F22, KEY_F23, KEY_F24,
    KEY_ESCAPE, KEY_TAB, KEY_BACKSPACE, KEY_ENTER, KEY_SPACE,
    KEY_INSERT, KEY_DELETE, KEY_HOME, KEY_END, KEY_PAGE_UP, KEY_PAGE_DOWN,
    KEY_LEFT, KEY_RIGHT, KEY_UP, KEY_DOWN,
    // Punctuation keys, named by their US-layout legend.
    KEY_MINUS, KEY_EQUALS, KEY_LEFT_BRACKET, KEY_RIGHT_BRACKET, KEY_BACKSLASH,
    KEY_SEMICOLON, KEY_APOSTROPHE, KEY_GRAVE, KEY_COMMA, KEY_PERIOD, KEY_SLASH,
    KEY_OEM_102, // the extra key next to left shift on ISO keyboards
    KEY_PAUSE, KEY_APPS,
    KEY_COUNT
} Key;

enum {
    KEYMOD_CTRL  = 1 << 0,
    KEYMOD_ALT   = 1 << 1,
    KEYMOD_SHIFT = 1 << 2,
    KEYMOD_ALTGR = 1 << 3, // AltGr is held: information only (its Ctrl and Alt are not chord modifiers)
};

typedef enum MouseButton {
    MOUSE_LEFT,
    MOUSE_RIGHT,
    MOUSE_MIDDLE,
} MouseButton;

typedef enum EventKind {
    EVENT_KEY_DOWN,
    EVENT_TEXT,
    EVENT_MOUSE_DOWN,
    EVENT_MOUSE_UP,
    EVENT_MOUSE_MOVE,
    EVENT_MOUSE_WHEEL,
    EVENT_RESIZE,
    EVENT_FOCUS,
    EVENT_CLOSE,
    EVENT_DIR_CHANGED, // a watched directory changed (`watch`)
    EVENT_WAKEUP,      // the wait timeout the app asked for (app_wait_ms) elapsed
    EVENT_END_SESSION, // Windows is shutting down or logging off and unsaved files blocked it
} EventKind;

typedef struct Event {
    EventKind kind;
    u32 mods;          // MOD_* (key, text, mouse)
    Key key;           // EVENT_KEY_DOWN; KEY_NONE for a key teal has no Key for (keypad digits, VK_OEM_8, ...)
    u32 scancode;      // EVENT_KEY_DOWN, extended keys have bit 8 set
    b32 repeat;        // EVENT_KEY_DOWN
    // EVENT_TEXT: the character typed. EVENT_KEY_DOWN: the character the key produces with the
    // current Shift / AltGr state, Ctrl and Alt ignored (a dead key gives its spacing accent); 0 if none.
    u32 codepoint;
    b32 dead;          // EVENT_KEY_DOWN: a dead key; its accent composes with the next key
    b32 no_text;       // EVENT_KEY_DOWN: no text event follows (the key types nothing with this state)
    MouseButton button;// EVENT_MOUSE_DOWN / UP
    i32 clicks;        // EVENT_MOUSE_DOWN: 1, 2 (double click) or 3 (triple), by the system's double-click time and area
    i32 x, y;          // mouse events, client pixels
    i32 wheel;         // EVENT_MOUSE_WHEEL, 120 per notch, positive = away from user
    i32 width, height; // EVENT_RESIZE, client pixels
    b32 focused;       // EVENT_FOCUS
    OsWatch watch;     // EVENT_DIR_CHANGED
#if TEAL_DEV
    u32 dev_seq;       // --log-keys: the number of a key or text event, shared by the platform's and the app's lines
#endif
} Event;

typedef struct FrameInput {
    Event *events;
    i32 event_count;
    i32 width, height; // client area in pixels
    f32 dpi_scale;     // 1.0 at 96 DPI
    Arena *scratch;    // reset after every frame
} FrameInput;

// ---------------------------------------------------------------------------
// App entry points, called by the platform layer.

typedef struct App App;
typedef struct Renderer Renderer;

// A file named on the command line, with the +LINE[:COLUMN] given for it (Emacs: the one before it).
typedef struct AppFileArg {
    String8 path; // empty: the +LINE[:COLUMN] of a command line without files, for *scratch*
    i64 line;     // 1-based; 0 = not given
    i64 col;      // visual column, 1-based; 0 = not given
} AppFileArg;

typedef struct AppArgs {
    f32 dpi_scale;
    b32 render_mode_forced; // dev --render-mode: overrides the config's render_mode
    FbRenderMode render_mode;
    AppFileArg *files; // in command-line order: all are opened, the first is shown (the second too with
    i32 file_count;    // startup_windows = 2); none: *scratch*
#if TEAL_DEV

    String8 config_path; // --config: this file instead of the user's teal.conf
    b32 user_config;     // read the user's teal.conf (off in the smoke and the benches: defaults only)
    b32 headless;        // --test: no font, no window; driven through app_dev_feed
    i32 scan_hold;       // --scan-hold N: project scans stand still after N batches (0 = never)
    i32 grep_hold;       // --grep-hold N: a search's workers stand still after N files each (0 = never)
#endif
} AppArgs;

App *app_create(Arena *perm, AppArgs *args); // NULL on failure (logged)
b32  app_update_and_render(App *app, FrameInput *input, Renderer *r); // false = quit
u32  app_wait_ms(App *app); // how long the platform may block before EVENT_WAKEUP; 0xFFFFFFFF = until an event
// The mouse cursor for a point of the client area (WM_SETCURSOR): the resize cursors over what a press
// would drag (a divider between windows, a mode line with a window below it), else the arrow. A query on
// the last layout: no state changes, no event, no frame.
typedef enum MouseCursor {
    MOUSE_CURSOR_ARROW,
    MOUSE_CURSOR_RESIZE_WE, // a divider between windows side by side
    MOUSE_CURSOR_RESIZE_NS, // a mode line with a window below it
    MOUSE_CURSOR_COUNT,
} MouseCursor;
MouseCursor app_mouse_cursor(App *app, i32 x, i32 y);
b32  app_wants_frame(App *app); // another frame right away: highlighting is still catching up
b32  app_wake(App *app);        // os_wake_main was called: take the workers' results; true = draw a frame
i32  app_shutdown(App *app); // leaked resources (font backend references, unreleased buffers, live markers), 0 = clean

#if TEAL_DEV
typedef enum DevProbeKind {
    DEV_PROBE_PIXEL_EQ,       // pixel (x0, y0) == rgb
    DEV_PROBE_REGION_EQ,      // every pixel in [x0, x1) x [y0, y1) == rgb
    DEV_PROBE_REGION_DIFFERS, // at least one pixel in the region != rgb
    DEV_PROBE_REGION_HAS,     // at least one pixel in the region == rgb (a glyph's fully covered pixel: its exact color)
    DEV_PROBE_CLEARTYPE,      // vertical stem in the region: edge coverage order matches `geometry`
} DevProbeKind;

typedef struct DevProbe {
    DevProbeKind kind;
    i32 x0, y0, x1, y1;
    u32 rgb;      // expected / background color, 0xRRGGBB
    u32 text_rgb; // DEV_PROBE_CLEARTYPE: the stem's color
    FbPixelGeometry geometry;
    const char *what;
} DevProbe;

void app_dev_smoke_syntax(App *app, i32 language); // *scratch*: a known text in `language` (a BufferLanguage), focus forced on
// The probes of that frame: syntax colors and paren_match; with `rendering`, the rendering checks too.
i32  app_dev_syntax_probes(App *app, FrameInput *input, DevProbe *out, i32 cap, b32 rendering);
void app_dev_smoke_buffer_view(App *app); // *scratch*: a known text, Fundamental, focus forced off
void app_dev_smoke_region(App *app);      // stage 2: an active region over lines 0-1
void app_dev_smoke_isearch(App *app);     // stage 4: "foo bar foo", point 0 (then C-s f o o C-s)
void app_dev_smoke_windows(App *app);     // stage 6: two windows side by side, a long line in the left one
void app_dev_smoke_windows_end(App *app); // ... and back to one window
void app_dev_smoke_finder(App *app);      // stage 7: the project finder over a known list (then "a p" is typed)
void app_dev_smoke_finder_end(App *app);  // ... and closed
void app_dev_smoke_results(App *app, String8 tmp_dir); // stage 8: *search* for "foo" over tmp_dir\smoke_grep (not selected)
void app_dev_smoke_results_end(App *app);              // ... and gone, one window again
void app_dev_smoke_compile(App *app, String8 tmp_dir); // stage 9: *compilation* from the --child program in tmp_dir\smoke_compile
void app_dev_smoke_compile_end(App *app);              // ... and gone, one window again
void app_dev_smoke_cursors(App *app);                  // stage 10: three cursors with regions, focus on
void app_dev_smoke_cursors_end(App *app);              // ... and one cursor again
b32  app_dev_results_busy(App *app);                    // a search runs or is still ending
b32  app_dev_compile_busy(App *app);                    // a build runs or is still ending
// --bench-grep: a project search with these settings (*search* in the other window, not selected), its
// state (true while it runs or its workers end), and closed.
void app_dev_bench_grep_start(App *app, String8 root, String8 needle, i32 workers, i32 max_results);
b32  app_dev_bench_grep_state(App *app, i64 *found, i64 *bytes, i64 *files, u64 *first_us, u64 *total_us, b32 *limited);
void app_dev_bench_grep_close(App *app);
// --bench-compile: a build of `command` in `dir` with this output cap (*compilation* in the other window, not
// selected), its state (true while it runs or ends: its buffer's size, lines and locations, the time), and gone.
void app_dev_bench_compile_start(App *app, String8 command, String8 dir, i32 max_output_mb);
b32  app_dev_bench_compile_state(App *app, i64 *bytes, i64 *lines, i64 *locations, u64 *total_us);
void app_dev_bench_compile_close(App *app);
void app_dev_results_log(App *app, const char *why); // every search's state to the log
u64  app_dev_results_work_max(App *app); // the longest results work of a frame since the last call
// --bench-project: the finder on `count` synthetic paths (returns the index's memory), a whole ranking at
// once (its time), the matches; a scan owned by the app (its batches through app_wake) and its state.
u64  app_dev_bench_finder_open(App *app, i64 count);
void app_dev_bench_finder_close(App *app);
u64  app_dev_bench_finder_finish(App *app, String8 query);
i64  app_dev_bench_matches(App *app);
void app_dev_bench_scan_start(App *app, String8 root);
b32  app_dev_bench_scan_state(App *app, String8 root, i64 *files, i64 *ignored, i64 *errors, u64 *memory); // true while it runs
void app_dev_bench_scan_close(App *app, String8 root);
i32  app_dev_buffer_probes(App *app, FrameInput *input, DevProbe *out, i32 cap, i32 stage); // 0: hollow cursor, 1: filled after the click, 2: region, 3: M-x list, 4: isearch, 5: failing isearch, 6: two windows, 7: project finder, 8: results, 9: a build, 10: three cursors
void app_dev_smoke_click_point(App *app, FrameInput *input, i32 *x, i32 *y); // the click of stage 1
void app_dev_force_focus(App *app, i32 focused); // -1: follow focus events
b32  app_dev_paren(App *app, Arena *scratch, i64 *a, i64 *b); // show_paren_mode's pair in the active view
b32  app_dev_atlas_has_coverage(App *app);
u8  *app_dev_atlas(App *app, i32 *size); // RGBA8, size x size
i32  test_run(u64 seed, String8 tmp_dir); // --test: headless buffer and file tests; failures, details in the log
void app_dev_goto_line(App *app, i64 line); // point to the start of line (< 0: the last line), recentered
i32  app_dev_key_events(App *app, String8 keys, Event *out, i32 cap); // --keys: tokens to events (bad tokens logged, skipped)
b32  app_dev_feed_events(App *app, Event *events, i32 count, Arena *scratch); // headless: events through the app, false = quit
void app_dev_frame_size(App *app, i32 width, i32 height); // headless: the frame of the next frames (default 1280 x 800)
i32  app_dev_window_count(App *app);
i32  app_dev_window_selected(App *app);           // the selected window's index in cyclic order
i64  app_dev_window_top(App *app, i32 index);     // the top line of the index-th window in cyclic order
b32  app_dev_window_shows_region(App *app, i32 index); // the index-th window in cyclic order draws its active region
b32  app_dev_feed(App *app, const char *keys, Arena *scratch);               // headless: --keys notation, one event at a time
void app_dev_use_config(App *app, String8 path); // switches to this config file and reloads it (as C-c r)
b32  app_dev_visit(App *app, String8 path);      // visits a file in the active view
i32  app_dev_font_setups(App *app);              // font set-ups so far (the startup does exactly one)
String8 app_dev_prompt(App *app);                // the open prompt, empty when the minibuffer is not active
String8 app_dev_replace_prompt(App *app, Arena *arena); // query-replace's question or progress line, empty when none
// --bench-complete: a choice prompt over `count` generated path-like candidates; returns the build time.
u64  app_dev_bench_complete_open(App *app, i64 count);
void app_dev_filter_stats(App *app, u64 *filters, u64 *last_us, i64 *matches);
i64  app_dev_line_count(App *app);
// The selected view's cursors for a bench or a test: `count` of them at the starts of lines first, first + step, ...
// (or, spread, at byte positions k * size / count); the first is the primary.
void app_dev_cursors(App *app, i64 first_line, i64 step, i32 count);
void app_dev_cursors_at(App *app, i64 first_line, i64 step, i32 count, i64 col); // at a visual column (-1: the line's end)
void app_dev_select(App *app, i64 start, i64 end);  // one cursor with the active region [start, end), point at end
u64  app_dev_batch_flushes(App *app);               // the selected buffer's batch flushes so far
i64  app_dev_buffer_size(App *app);                 // the selected buffer's size
void app_dev_revert(App *app);                      // the selected buffer read again from its file (no question)
String8 test_bench_cursors_c(Arena *arena, String8 tmp_dir);     // --bench-cursors: the C file of 8-line blocks (made once)
String8 test_bench_cursors_words(Arena *arena, String8 tmp_dir); // --bench-cursors: a million lines with "needle" (made once)
void app_dev_cursors_spread(App *app, i32 count);
i32  app_dev_cursor_count(App *app);
u64  app_dev_build_us(App *app); // the last frame, from its start to r_end_frame
u64  os_dev_private_bytes(void);  // the process's committed private memory
typedef struct AppDevMemory {
    u64 undo;        // the active buffer's undo log and group index, committed
    u64 kill_ring;   // the kill ring's committed bytes (shared arena and large entries)
    u64 buffer;      // the active buffer itself: the sum of the five below
    u64 meta, text, line_index, markers, undo_log, states;
} AppDevMemory;
AppDevMemory app_dev_memory(App *app);
String8 test_bench_buffer_file(Arena *arena, String8 tmp_dir); // --bench-buffer: generates the 100 MB file once
void test_bench_buffer(String8 path, String8 tmp_dir);         // load, inserts, lookups, save
i32  app_dev_bench_frame(App *app, FrameInput *input, Renderer *r, u64 *build_us, u64 *submit_us); // returns glyphs drawn
void test_bench_syntax(void);                                  // --bench-syntax: lexing throughput per language
String8 test_bench_syntax_file(Arena *arena, String8 tmp_dir); // --bench-syntax: generates the 100 MB C file once
void app_dev_set_language(App *app, i32 language);             // the active view's buffer (a BufferLanguage)
void app_dev_append(App *app, String8 text);                  // --bench-search: appended to the active buffer, not undoable
void app_dev_show_scratch(App *app, String8 text);            // --bench-search: *scratch* with this text, point at 0
i64  app_dev_point(App *app);                                 // the active view's point
b32  app_dev_isearch_failing(App *app);                        // an isearch is active, done, and failing
void app_dev_cell(App *app, i32 *w, i32 *h);                  // the text cell in pixels
void app_dev_log_keys(App *app, b32 on);                      // --log-keys: log every key and text event's result
String8 app_dev_echo(App *app);                               // the echo area's text
String8 app_dev_text(App *app, Arena *arena);                 // the active view's buffer text
// The index-th binding of a keymap (0 [keys], 1 [keys minibuffer], 2 [keys isearch], 3 [keys search-results], 4 [keys compilation])
// in kbd notation, its command's name in *command; empty past the last one.
String8 app_dev_binding(App *app, Arena *arena, i32 keymap, i32 index, const char **command);
i64  app_dev_size(App *app);                                  // the size of its buffer
#endif

#endif // PLATFORM_H
