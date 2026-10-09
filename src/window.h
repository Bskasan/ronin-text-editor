// window.h — the frame's windows, as in Emacs: a binary tree of splits whose leaves are Views, one of
// them selected. The minibuffer line is not part of it. Plain C, headless: geometry and structure only
// (the tree never looks inside a View); app.c draws and runs the commands.
//
// Layout: a split gives its first child round(ratio * size / cell) cells of its size along the split,
// measured from its own origin, clamped by the minimum sizes when they fit; the second child gets the
// rest, so the leaves always tile the frame exactly. Ratios change only through split, resize, drag and
// balance: a frame resize keeps the proportions. A frame too small for the minimum sizes is split by the
// ratios alone, clamped to [0, size]: a leaf may get an empty rect, never a negative one, and nothing is
// lost when the frame grows again.

#ifndef WINDOW_H
#define WINDOW_H

#define WINDOW_MAX 8                       // windows (leaves)
#define WINDOW_NODE_MAX (2 * WINDOW_MAX - 1)
#define WINDOW_MIN_LINES 4                 // a window's height in lines, its mode line included (Emacs window-min-height)
#define WINDOW_MIN_COLS 10                 // a window's text width in columns (Emacs window-min-width)

typedef enum WindowSplit {
    WINDOW_LEAF,         // a window
    WINDOW_SIDE_BY_SIDE, // child[0] left, child[1] right
    WINDOW_STACKED,      // child[0] above, child[1] below
} WindowSplit;

typedef enum WindowStatus {
    WINDOW_OK,
    WINDOW_TOO_SMALL, // "Window too small for splitting"
    WINDOW_TOO_MANY,  // "Too many windows"
} WindowStatus;

// What sizes are measured in, from the last layout: a cell, a line, a text area's left padding and the
// width of the vertical divider a window has on its right when another window is there.
typedef struct WindowMetrics {
    i32 cell_w, line_h, pad, divider;
} WindowMetrics;

typedef struct WindowNode {
    b32 used;
    WindowSplit split;
    i32 parent;     // -1: the root
    i32 child[2];   // splits
    f32 ratio;      // splits: the first child's share of the size along the split
    View *view;     // leaves
    u64 used_tick;  // leaves: when last selected (least / most recently used)
    i32 x, y, w, h; // the last layout, pixels
} WindowNode;

typedef struct WindowTree {
    WindowNode nodes[WINDOW_NODE_MAX]; // a window keeps its node index for its whole life
    i32 root;
    i32 selected;   // a leaf
    u64 tick;
    i32 x, y, w, h; // the frame area of the last layout
    WindowMetrics m;
} WindowTree;

void window_init(WindowTree *t, View *view); // one window
i32  window_count(WindowTree *t);
// The leaves in cyclic order (depth first, left / top first): other-window's order. Returns the count.
i32  window_leaves(WindowTree *t, i32 *out);
i32  window_of_view(WindowTree *t, View *view); // its leaf, or -1
void window_select(WindowTree *t, i32 leaf);
i32  window_next(WindowTree *t, i32 leaf, i32 step); // `step` leaves on in cyclic order (negative: back)
i32  window_lru(WindowTree *t, i32 except);           // the least recently selected leaf other than `except`, -1 if none

// Splits `leaf` in two: `view` is the new window, below (STACKED) or right (SIDE_BY_SIDE) of it, each
// with half. Refused when either half would be below the minimum size in the last layout, or when there
// are WINDOW_MAX windows. The selection does not change. Lays out again.
WindowStatus window_split(WindowTree *t, i32 leaf, WindowSplit split, View *view, i32 *new_leaf);
// Removes a leaf (never the last one): its sibling takes its place and space. If it was selected, the
// most recently used remaining window is. Returns its View (the caller destroys it). Lays out again.
View *window_delete(WindowTree *t, i32 leaf);

void window_layout(WindowTree *t, i32 x, i32 y, i32 w, i32 h, WindowMetrics m);
void window_relayout(WindowTree *t); // the same frame and metrics again (after a tree change)
b32  window_has_divider(WindowTree *t, i32 leaf); // another window is on its right

// Moves the split line of `node` so that its first child gets `first_px` (snapped to cells, clamped to
// the minimum sizes of the windows on either side of the line). Only the windows adjacent to the line
// change size: the others keep their pixel sizes. Returns the pixels it moved. Lays out again.
i32  window_move_split(WindowTree *t, i32 node, i32 first_px);
// enlarge-window / shrink-window: `cells` lines (STACKED) or columns (SIDE_BY_SIDE) more for `leaf`
// (negative: fewer), taken from or given to its neighbor across the nearest split line it touches
// that can move, the window after it (right, below) first. False when none can.
b32  window_resize(WindowTree *t, i32 leaf, WindowSplit axis, i32 cells);
// balance-windows: the members of a run of same-direction splits (Emacs' combination) get equal shares.
void window_balance(WindowTree *t);

i32  window_at(WindowTree *t, i32 x, i32 y); // the leaf whose (non-empty) rect holds the pixel, or -1
// The split whose line is under the pointer: a vertical divider (within `grab` px; *split SIDE_BY_SIDE) or
// a mode line with a window below it (*split STACKED). -1 when none.
i32  window_edge_at(WindowTree *t, i32 x, i32 y, i32 grab, WindowSplit *split);

#endif // WINDOW_H
