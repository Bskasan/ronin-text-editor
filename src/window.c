// window.c — see window.h.

static i32 window_alloc(WindowTree *t) {
    for (i32 i = 0; i < WINDOW_NODE_MAX; i++) {
        if (t->nodes[i].used) continue;
        WindowNode *n = &t->nodes[i];
        memset(n, 0, sizeof(*n));
        n->used = 1;
        n->parent = n->child[0] = n->child[1] = -1;
        return i;
    }
    return -1;
}

void window_init(WindowTree *t, View *view) {
    memset(t, 0, sizeof(*t));
    t->root = window_alloc(t);
    t->nodes[t->root].split = WINDOW_LEAF;
    t->nodes[t->root].view = view;
    window_select(t, t->root);
}

static i32 window_collect(WindowTree *t, i32 n, i32 *out, i32 count) {
    WindowNode *node = &t->nodes[n];
    if (node->split == WINDOW_LEAF) {
        out[count++] = n;
        return count;
    }
    count = window_collect(t, node->child[0], out, count);
    return window_collect(t, node->child[1], out, count);
}

i32 window_leaves(WindowTree *t, i32 *out) {
    return window_collect(t, t->root, out, 0);
}

i32 window_count(WindowTree *t) {
    i32 leaves[WINDOW_MAX];
    return window_leaves(t, leaves);
}

i32 window_of_view(WindowTree *t, View *view) {
    for (i32 i = 0; i < WINDOW_NODE_MAX; i++) {
        if (t->nodes[i].used && t->nodes[i].split == WINDOW_LEAF && t->nodes[i].view == view) return i;
    }
    return -1;
}

void window_select(WindowTree *t, i32 leaf) {
    t->selected = leaf;
    t->nodes[leaf].used_tick = ++t->tick;
}

i32 window_next(WindowTree *t, i32 leaf, i32 step) {
    i32 leaves[WINDOW_MAX];
    i32 n = window_leaves(t, leaves), at = 0;
    for (i32 i = 0; i < n; i++) if (leaves[i] == leaf) at = i;
    return leaves[((at + step) % n + n) % n];
}

i32 window_lru(WindowTree *t, i32 except) {
    i32 leaves[WINDOW_MAX];
    i32 n = window_leaves(t, leaves), best = -1;
    for (i32 i = 0; i < n; i++) {
        if (leaves[i] == except) continue;
        if (best < 0 || t->nodes[leaves[i]].used_tick < t->nodes[best].used_tick) best = leaves[i];
    }
    return best;
}

// ---------------------------------------------------------------------------
// Sizes

static i32 window_unit(WindowTree *t, WindowSplit axis) {
    return MAX(axis == WINDOW_SIDE_BY_SIDE ? t->m.cell_w : t->m.line_h, 1);
}

static i32 window_size(WindowTree *t, i32 n, WindowSplit axis) {
    return axis == WINDOW_SIDE_BY_SIDE ? t->nodes[n].w : t->nodes[n].h;
}

// The smallest size of a subtree along `axis` that keeps every window in it at its minimum.
static i32 window_min(WindowTree *t, i32 n, WindowSplit axis) {
    WindowNode *node = &t->nodes[n];
    if (node->split == WINDOW_LEAF) {
        return axis == WINDOW_SIDE_BY_SIDE ? t->m.pad + WINDOW_MIN_COLS * t->m.cell_w + t->m.divider : WINDOW_MIN_LINES * t->m.line_h;
    }
    i32 a = window_min(t, node->child[0], axis), b = window_min(t, node->child[1], axis);
    return node->split == axis ? a + b : MAX(a, b);
}

// The first child's size: its ratio of `total`, in whole cells from the split's origin, clamped by the
// minimum sizes when both fit, never outside [0, total].
static i32 window_first_size(WindowTree *t, i32 n, i32 total) {
    WindowNode *node = &t->nodes[n];
    i32 unit = window_unit(t, node->split);
    i32 first = (i32)(node->ratio * (f32)total / (f32)unit + 0.5f) * unit;
    i32 min0 = window_min(t, node->child[0], node->split), min1 = window_min(t, node->child[1], node->split);
    if (min0 + min1 <= total) first = CLAMP(first, min0, total - min1);
    return CLAMP(first, 0, total);
}

static void window_place(WindowTree *t, i32 n, i32 x, i32 y, i32 w, i32 h) {
    WindowNode *node = &t->nodes[n];
    node->x = x;
    node->y = y;
    node->w = w;
    node->h = h;
    if (node->split == WINDOW_LEAF) return;
    if (node->split == WINDOW_SIDE_BY_SIDE) {
        i32 first = window_first_size(t, n, w);
        window_place(t, node->child[0], x, y, first, h);
        window_place(t, node->child[1], x + first, y, w - first, h);
    } else {
        i32 first = window_first_size(t, n, h);
        window_place(t, node->child[0], x, y, w, first);
        window_place(t, node->child[1], x, y + first, w, h - first);
    }
}

void window_layout(WindowTree *t, i32 x, i32 y, i32 w, i32 h, WindowMetrics m) {
    t->x = x;
    t->y = y;
    t->w = MAX(w, 0);
    t->h = MAX(h, 0);
    t->m = m;
    window_place(t, t->root, t->x, t->y, t->w, t->h);
}

void window_relayout(WindowTree *t) {
    window_place(t, t->root, t->x, t->y, t->w, t->h);
}

b32 window_has_divider(WindowTree *t, i32 leaf) {
    WindowNode *l = &t->nodes[leaf];
    return l->x + l->w < t->x + t->w;
}

// A run of `axis` splits from n towards side `toward` (0: child[0], 1: child[1]) grows by `growth`
// pixels: each keeps the size of its child on the far side, so only the element at the end of the run
// (a window, or a split the other way) changes. Uses the sizes of the last layout.
static void window_keep_far(WindowTree *t, i32 n, WindowSplit axis, i32 toward, i32 growth) {
    while (t->nodes[n].split == axis) {
        WindowNode *node = &t->nodes[n];
        i32 size = window_size(t, n, axis) + growth;
        i32 far_size = window_size(t, node->child[1 - toward], axis);
        if (size > 0 && far_size <= size) node->ratio = (f32)(toward == 0 ? size - far_size : far_size) / (f32)size;
        n = node->child[toward];
    }
}

// The element of a run of `axis` splits at its end towards side `toward`.
static i32 window_run_end(WindowTree *t, i32 n, WindowSplit axis, i32 toward) {
    while (t->nodes[n].split == axis) n = t->nodes[n].child[toward];
    return n;
}

// ---------------------------------------------------------------------------
// Structure

WindowStatus window_split(WindowTree *t, i32 leaf, WindowSplit split, View *view, i32 *new_leaf) {
    if (window_count(t) >= WINDOW_MAX) return WINDOW_TOO_MANY;
    if (window_size(t, leaf, split) < 2 * window_min(t, leaf, split)) return WINDOW_TOO_SMALL;
    i32 s = window_alloc(t), n = window_alloc(t);
    ASSERT(s >= 0 && n >= 0);
    WindowNode *l = &t->nodes[leaf], *sn = &t->nodes[s], *nn = &t->nodes[n];
    sn->split = split;
    sn->parent = l->parent;
    sn->child[0] = leaf;
    sn->child[1] = n;
    sn->ratio = 0.5f;
    if (l->parent < 0) t->root = s;
    else t->nodes[l->parent].child[t->nodes[l->parent].child[0] == leaf ? 0 : 1] = s;
    sn->x = l->x;
    sn->y = l->y;
    sn->w = l->w;
    sn->h = l->h;
    l->parent = s;
    nn->split = WINDOW_LEAF;
    nn->parent = s;
    nn->view = view;
    if (new_leaf) *new_leaf = n;
    window_relayout(t);
    return WINDOW_OK;
}

View *window_delete(WindowTree *t, i32 leaf) {
    WindowNode *l = &t->nodes[leaf];
    ASSERT(l->parent >= 0);
    i32 p = l->parent;
    WindowNode *pn = &t->nodes[p];
    i32 side = pn->child[0] == leaf ? 0 : 1, sib = pn->child[1 - side];
    // The space goes to the windows of the sibling next to the deleted one only (Emacs: deleting resizes
    // one adjacent window), and the sibling takes the parent's place.
    window_keep_far(t, sib, pn->split, side, window_size(t, leaf, pn->split));
    i32 gp = pn->parent;
    t->nodes[sib].parent = gp;
    if (gp < 0) t->root = sib;
    else t->nodes[gp].child[t->nodes[gp].child[0] == p ? 0 : 1] = sib;
    View *view = l->view;
    l->used = 0;
    pn->used = 0;
    if (t->selected == leaf) { // the most recently used remaining window (Emacs 28+)
        i32 leaves[WINDOW_MAX];
        i32 n = window_leaves(t, leaves), best = leaves[0];
        for (i32 i = 1; i < n; i++) if (t->nodes[leaves[i]].used_tick > t->nodes[best].used_tick) best = leaves[i];
        window_select(t, best);
    }
    window_relayout(t);
    return view;
}

// ---------------------------------------------------------------------------
// Resizing

i32 window_move_split(WindowTree *t, i32 node, i32 first_px) {
    WindowNode *n = &t->nodes[node];
    if (n->split == WINDOW_LEAF) return 0;
    WindowSplit axis = n->split;
    i32 total = window_size(t, node, axis), unit = window_unit(t, axis);
    if (total <= 0) return 0;
    i32 old = window_size(t, n->child[0], axis);
    i32 want = (i32)((f32)MAX(first_px, 0) / (f32)unit + 0.5f) * unit;
    // Only the elements touching the line give or take space, down to their minimum sizes.
    i32 a0 = window_run_end(t, n->child[0], axis, 1), a1 = window_run_end(t, n->child[1], axis, 0);
    i32 room0 = MAX(window_size(t, a0, axis) - window_min(t, a0, axis), 0);
    i32 room1 = MAX(window_size(t, a1, axis) - window_min(t, a1, axis), 0);
    i32 delta = CLAMP(want - old, -room0, room1);
    if (!delta) return 0;
    window_keep_far(t, n->child[0], axis, 1, delta);
    window_keep_far(t, n->child[1], axis, 0, -delta);
    n->ratio = (f32)(old + delta) / (f32)total;
    window_relayout(t);
    return delta;
}

b32 window_resize(WindowTree *t, i32 leaf, WindowSplit axis, i32 cells) {
    i32 px = cells * window_unit(t, axis);
    WindowNode *l = &t->nodes[leaf];
    // The window after it (right, below) first, then the one before it, as Emacs prefers.
    for (i32 pass = 0; pass < 2 && px; pass++) {
        for (i32 c = leaf, p = l->parent; p >= 0; c = p, p = t->nodes[p].parent) {
            WindowNode *pn = &t->nodes[p];
            b32 first = pn->child[0] == c;
            if (pn->split != axis || first != (pass == 0)) continue;
            // The line of this split must be the window's own edge.
            WindowNode *c0 = &t->nodes[pn->child[0]];
            i32 line = axis == WINDOW_SIDE_BY_SIDE ? c0->x + c0->w : c0->y + c0->h;
            i32 edge = axis == WINDOW_SIDE_BY_SIDE ? (first ? l->x + l->w : l->x) : (first ? l->y + l->h : l->y);
            if (edge != line) continue;
            if (window_move_split(t, p, window_size(t, pn->child[0], axis) + (first ? px : -px))) return 1;
        }
    }
    return 0;
}

// Members of a run of `axis` splits: a window or a split the other way counts as one.
static i32 window_weight(WindowTree *t, i32 n, WindowSplit axis) {
    WindowNode *node = &t->nodes[n];
    if (node->split != axis) return 1;
    return window_weight(t, node->child[0], axis) + window_weight(t, node->child[1], axis);
}

static void window_balance_node(WindowTree *t, i32 n) {
    WindowNode *node = &t->nodes[n];
    if (node->split == WINDOW_LEAF) return;
    node->ratio = (f32)window_weight(t, node->child[0], node->split) / (f32)window_weight(t, n, node->split);
    window_balance_node(t, node->child[0]);
    window_balance_node(t, node->child[1]);
}

void window_balance(WindowTree *t) {
    window_balance_node(t, t->root);
    window_relayout(t);
}

// ---------------------------------------------------------------------------
// Hit testing

i32 window_at(WindowTree *t, i32 x, i32 y) {
    i32 leaves[WINDOW_MAX];
    i32 n = window_leaves(t, leaves);
    for (i32 i = 0; i < n; i++) {
        WindowNode *l = &t->nodes[leaves[i]];
        if (x >= l->x && x < l->x + l->w && y >= l->y && y < l->y + l->h) return leaves[i];
    }
    return -1;
}

i32 window_edge_at(WindowTree *t, i32 x, i32 y, i32 grab, WindowSplit *split) {
    i32 best = -1;
    i64 best_area = 0;
    for (i32 i = 0; i < WINDOW_NODE_MAX; i++) {
        WindowNode *n = &t->nodes[i];
        if (!n->used || n->split == WINDOW_LEAF || n->w <= 0 || n->h <= 0) continue;
        WindowNode *c0 = &t->nodes[n->child[0]];
        b32 hit;
        if (n->split == WINDOW_SIDE_BY_SIDE) { // the divider at the right edge of the left side
            i32 line = c0->x + c0->w;
            hit = x >= line - t->m.divider - grab && x < line + grab && y >= n->y && y < n->y + n->h;
        } else { // the mode lines at the bottom of the upper side
            i32 line = c0->y + c0->h;
            hit = y >= line - t->m.line_h && y < line && x >= n->x && x < n->x + n->w;
        }
        i64 area = (i64)n->w * n->h;
        if (hit && (best < 0 || area < best_area || (area == best_area && n->split == WINDOW_SIDE_BY_SIDE))) {
            best = i;
            best_area = area;
        }
    }
    if (best >= 0) *split = t->nodes[best].split;
    return best;
}
