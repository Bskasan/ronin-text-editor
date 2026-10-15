// modal.h — a modal box drawn over the windows (Phase 18): one at a time, drawn last in the frame, owning every key
// and mouse event while it is open. An owner module (the command list; Phase 19's git changes) supplies its rows,
// their drawing and what accepting a row does; the layer knows nothing about commands or git. It has an optional
// one-line text field at the top and one or two lists side by side (panes), which it moves, pages, scrolls and
// hit-tests. Part of the app: modal.c is included after app.c.
//
// Keys go through [owner keymap, global]: a quit command (C-g, ESC) closes the modal without a message; a
// command of the owner's keymap runs; a command flagged COMMAND_FIELD edits the field; any other command is
// refused by name ("save-buffer is not available in the command list"). A click outside the box closes it too.
// Nothing here needs a timer or a frame of its own: every event already makes one.

#ifndef MODAL_H
#define MODAL_H

#define MODAL_PANES_MAX 2
#define MODAL_MIN_COLS 40           // a box narrower or lower than this does not open (and closes on a resize)
#define MODAL_MIN_ROWS 6
#define MODAL_WIDTH_PCT 70          // the defaults a 0 in ModalSpec stands for
#define MODAL_HEIGHT_PCT 60
#define MODAL_PANE_SPLIT_PCT 50
#define MODAL_ARENA_RESERVE MB(64)  // the owner's per-open memory

typedef struct ModalList {   // one pane's rows: the layer moves, pages, scrolls and hit-tests it
    i64 count;               // set by the owner (modal_set_rows)
    i64 selected;            // 0 .. count-1; -1 when count == 0
    i64 top;                 // the first row shown
} ModalList;

typedef struct ModalRow {    // what draw_row gets for one visible row
    i32 pane;
    i64 row;
    Rect rect;               // the row's pixels (inside the box's clip and its pane)
    i32 x, cols;             // where its text starts (after the padding) and how many cells it has
    b32 selected;            // the selected row of its pane (the layer already filled its background)
} ModalRow;

typedef struct ModalSpec {
    String8 name;                                    // "command list": "save-buffer is not available in the command list"
    Keymap *(*keymap)(App *app, void *data);         // its [keys ...] section, searched before the global keymap
    b32 field;                                       // a one-line text field at the top (a filter, a message ...)
    i32 panes;                                       // lists side by side: 1 (command list) or 2 (Phase 19)
    i32 width_pct;                                   // of the frame's width; 0 = MODAL_WIDTH_PCT
    i32 height_pct;                                  // at most this much of the window area's height; 0 = MODAL_HEIGHT_PCT
    i32 pane_split_pct;                              // two panes: the left pane's share of the width; 0 = MODAL_PANE_SPLIT_PCT
    void *data;                                      // the owner's state
    void (*field_changed)(App *app, void *data, String8 text); // the field's text changed (also once at open)
    void (*accept)(App *app, void *data, i32 pane, i64 row);   // modal-accept (RET) or a double-click on a row
    void (*draw_row)(App *app, void *data, Renderer *r, ModalRow *row);
    String8 (*status)(App *app, void *data, Arena *scratch);   // the right end of the field line ("12/137"), may be empty
    void (*reload)(App *app, void *data);            // the config was applied again while open (bindings, colors)
    void (*closed)(App *app, void *data);            // it closed: C-g, ESC, a click outside, accept, a resize too small
} ModalSpec;

// The box this frame, in pixels: its rect, the field line, each pane's list area and the rows that fit.
typedef struct ModalGeometry {
    b32 fits;                // at least MODAL_MIN_COLS x MODAL_MIN_ROWS
    Rect box;
    Rect field;              // the field line (empty without a field)
    Rect panes[MODAL_PANES_MAX];
    i32 rows;                // list rows shown in each pane
    i32 border;              // the frame's and the separators' width in pixels
} ModalGeometry;

typedef struct Modal {
    b32 active;
    ModalSpec spec;          // copied at open
    ModalList lists[MODAL_PANES_MAX];
    i32 focus;               // the pane the movement commands act on
    i64 rows_high;           // the most rows any pane had since the open: the box's height follows it, not the filter
    Arena arena;             // the owner's per-open memory: made at the first open, reset when it closes
    Buffer *field_buffer;    // the field's text: made at the first open that has a field, kept until shutdown
    View field_view;         // ... and its one-line View
    u64 field_seen;          // the field's edit_count when field_changed last ran
    const Command *then;     // modal_run_after_close: runs after the command that accepted, on the selected window
    ModalGeometry g;         // the last layout
    i32 frame_w, frame_h, cell_w, line_h, pad, divider; // the frame it was laid out for
#if RONIN_DEV
    u64 dev_opens;           // opens so far
    u64 dev_field_us;        // the last field_changed (--bench-modal)
    u64 dev_field_calls;
#endif
} Modal;

b32        modal_open(App *app, const ModalSpec *spec); // false + echo: one is open, the minibuffer is active, or the
                                                        // box would be smaller than 40 x 6
void       modal_close(App *app);                       // closed() runs; the arena is reset; no echo
b32        modal_active(App *app);
Arena     *modal_arena(App *app);                       // the owner's per-open memory; reset when it closes
void       modal_set_rows(App *app, i32 pane, i64 count); // selection back to 0, top to 0
ModalList *modal_list(App *app, i32 pane);
i32        modal_focus_pane(App *app);                  // the pane the movement commands act on (modal-other-pane)
void       modal_run_after_close(App *app, const Command *cmd); // accept: close, then cmd on the selected window's View
                                                        // through the normal path, exactly as M-x runs it
Rect       modal_rect(App *app);                        // where the box is (the last layout)

extern const Command CMD_MODAL_NEXT, CMD_MODAL_PREVIOUS, CMD_MODAL_NEXT_PAGE, CMD_MODAL_PREVIOUS_PAGE;
extern const Command CMD_MODAL_ACCEPT, CMD_MODAL_OTHER_PANE;

#endif // MODAL_H
