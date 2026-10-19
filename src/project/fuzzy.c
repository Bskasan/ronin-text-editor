// fuzzy.c — see fuzzy.h.

// The score: every matched character earns FUZZY_MATCH and the bonus of its position (a segment start
// after '/' or at the path's start, a word start after '_', '-', '.' or ' ', a lower-to-upper change),
// the query's first character twice; a run of consecutive characters keeps the bonus of its first one
// (at least FUZZY_CONSECUTIVE); a gap costs FUZZY_GAP_START plus FUZZY_GAP_EXTEND per further byte; a
// term matched inside the file name earns FUZZY_NAME per character.
// Credit: the match, boundary, camel-case, consecutive and gap values follow fzf's (Junegunn Choi, MIT).
#define FUZZY_MATCH 16
#define FUZZY_SEGMENT 10
#define FUZZY_WORD 8
#define FUZZY_CAMEL 7
#define FUZZY_CONSECUTIVE 4
#define FUZZY_GAP_START (-3)
#define FUZZY_GAP_EXTEND (-1)
#define FUZZY_NAME 6
#define FUZZY_STARTS 8 // starting places tried per term

// The character at s[i], folded unless exact, and its length.
static u32 fuzzy_char(String8 s, i64 i, b32 exact, i64 *adv) {
    u8 b = s.data[i];
    if (b < 0x80) {
        *adv = 1;
        return exact || b < 'A' || b > 'Z' ? b : (u32)b + 32;
    }
    u32 c = utf8_decode(s.data + i, s.len - i, adv);
    return exact ? c : unicode_lower(c);
}

static b32 fuzzy_is_lower(u8 c) { return c >= 'a' && c <= 'z'; }
static b32 fuzzy_is_upper(u8 c) { return c >= 'A' && c <= 'Z'; }

// The bonus of a match at path[i].
static i32 fuzzy_bonus(String8 path, i64 i) {
    u8 prev = i == 0 ? '/' : path.data[i - 1], cur = path.data[i];
    if (prev == '/') return FUZZY_SEGMENT;
    if (prev == '_' || prev == '-' || prev == '.' || prev == ' ') return FUZZY_WORD;
    if (fuzzy_is_lower(prev) && fuzzy_is_upper(cur)) return FUZZY_CAMEL;
    return 0;
}

FuzzyQuery fuzzy_query(Arena *arena, String8 input) {
    FuzzyQuery q = { 0 };
    for (i64 i = 0; i < input.len;) {
        i64 adv;
        u32 c = utf8_decode(input.data + i, input.len - i, &adv);
        if (unicode_lower(c) != c) q.exact = 1;
        i += adv;
    }
    for (i64 i = 0; i < input.len && q.count < FUZZY_TERMS;) {
        while (i < input.len && input.data[i] == ' ') i++;
        i64 start = i;
        while (i < input.len && input.data[i] != ' ') i++;
        if (i == start) continue;
        FuzzyTerm *t = &q.terms[q.count++];
        t->cps = PUSH_ARRAY(arena, u32, i - start);
        for (i64 k = start; k < i;) {
            i64 adv;
            u32 c = fuzzy_char(input, k, q.exact, &adv);
            if (c == '\\') c = '/'; // either separator in the query matches '/'
            t->cps[t->count++] = c;
            t->slash |= c == '/';
            u8 b = c < 0x80 ? (u8)c : 0x80;
            q.mask |= project_mask(str8(&b, 1));
            k += adv;
        }
    }
    return q;
}

// A greedy match of the term from `start` (a place where its first character is), scored; FUZZY_NO_MATCH
// when the rest does not follow. Spans: merged byte ranges.
static i32 fuzzy_from(FuzzyTerm *t, b32 exact, String8 path, i64 start, MatchSpan *spans, i32 cap, i32 *n) {
    i32 score = 0, run = 0, j = 0;
    i64 prev_end = -1;
    for (i64 i = start; i < path.len && j < t->count;) {
        i64 adv;
        u32 c = fuzzy_char(path, i, exact, &adv);
        if (c == t->cps[j]) {
            i32 bonus = fuzzy_bonus(path, i);
            if (prev_end == i) { // a run: the bonus of its first character, at least FUZZY_CONSECUTIVE
                run = MAX(run, MAX(bonus, FUZZY_CONSECUTIVE));
                bonus = run;
            } else {
                if (prev_end >= 0) score += FUZZY_GAP_START + FUZZY_GAP_EXTEND * (i32)(i - prev_end - 1);
                run = bonus;
            }
            score += FUZZY_MATCH + (j == 0 ? 2 * bonus : bonus);
            if (spans) {
                if (*n > 0 && spans[*n - 1].end == i) spans[*n - 1].end = i + adv;
                else if (*n < cap) spans[(*n)++] = (MatchSpan){ i, i + adv };
            }
            prev_end = i + adv;
            j++;
        }
        i += adv;
    }
    return j == t->count ? score : FUZZY_NO_MATCH;
}

// The best of a few starting places in [lo, path.len): the leftmost occurrence of the first character
// and the next ones at a segment or word start.
static i32 fuzzy_term(FuzzyTerm *t, b32 exact, String8 path, i64 lo, MatchSpan *spans, i32 cap, i32 *n) {
    if (!t->count) return 0;
    i32 best = FUZZY_NO_MATCH, tried = 0;
    i64 best_start = -1;
    for (i64 i = lo; i < path.len && tried < FUZZY_STARTS;) {
        i64 adv;
        u32 c = fuzzy_char(path, i, exact, &adv);
        if (c == t->cps[0] && (tried == 0 || fuzzy_bonus(path, i) > 0)) {
            i32 s = fuzzy_from(t, exact, path, i, NULL, 0, NULL);
            if (s == FUZZY_NO_MATCH) break; // the rest does not follow from here, nor from any later place
            tried++;
            if (s > best) {
                best = s;
                best_start = i;
            }
        }
        i += adv;
    }
    if (best != FUZZY_NO_MATCH && spans) fuzzy_from(t, exact, path, best_start, spans, cap, n);
    return best;
}

i32 fuzzy_score(FuzzyQuery *q, String8 path, i64 name_off, MatchSpan *spans, i32 cap, i32 *n) {
    if (n) *n = 0;
    i32 total = 0;
    for (i32 k = 0; k < q->count; k++) {
        FuzzyTerm *t = &q->terms[k];
        i32 s = FUZZY_NO_MATCH;
        if (!t->slash) { // the file name first
            s = fuzzy_term(t, q->exact, path, name_off, spans, cap, n);
            if (s != FUZZY_NO_MATCH) s += FUZZY_NAME * t->count;
        }
        if (s == FUZZY_NO_MATCH) s = fuzzy_term(t, q->exact, path, 0, spans, cap, n);
        if (s == FUZZY_NO_MATCH) return FUZZY_NO_MATCH;
        total += s;
    }
    if (q->count == 1) { // the file name is the whole query: first
        FuzzyTerm *t = &q->terms[0];
        i32 j = 0;
        i64 i = name_off;
        while (i < path.len && j < t->count) {
            i64 adv;
            if (fuzzy_char(path, i, q->exact, &adv) != t->cps[j]) break;
            i += adv;
            j++;
        }
        if (i == path.len && j == t->count) total += FUZZY_EXACT_NAME;
    }
    return total;
}

// ---------------------------------------------------------------------------
// Ranking

void fuzzy_rank_init(FuzzyRank *r) {
    memset(r, 0, sizeof(*r));
    r->query_arena = arena_create(MB(16));
    r->lists[0] = arena_create(MB(64)); // 4 bytes per survivor: 10 M files at most
    r->lists[1] = arena_create(MB(64));
    r->path_dir = -1;
}

void fuzzy_rank_destroy(FuzzyRank *r) {
    os_release(r->query_arena.base);
    os_release(r->lists[0].base);
    os_release(r->lists[1].base);
    memset(r, 0, sizeof(*r));
}

static b32 fuzzy_better(FuzzyHit a, FuzzyHit b) {
    return a.score != b.score ? a.score > b.score : a.len != b.len ? a.len < b.len : a.index < b.index;
}

// The min-heap keeps the worst kept hit at the root.
static void fuzzy_heap_down(FuzzyHit *h, i32 count, i32 i) {
    for (;;) {
        i32 l = 2 * i + 1, r = l + 1, worst = i;
        if (l < count && fuzzy_better(h[worst], h[l])) worst = l;
        if (r < count && fuzzy_better(h[worst], h[r])) worst = r;
        if (worst == i) return;
        FuzzyHit tmp = h[i];
        h[i] = h[worst];
        h[worst] = tmp;
        i = worst;
    }
}

static void fuzzy_heap_add(FuzzyRank *r, FuzzyHit hit) {
    FuzzyHit *h = r->heap;
    if (r->heap_count < FUZZY_TOP) {
        i32 i = r->heap_count++;
        h[i] = hit;
        while (i > 0 && fuzzy_better(h[(i - 1) / 2], h[i])) {
            FuzzyHit tmp = h[i];
            h[i] = h[(i - 1) / 2];
            h[(i - 1) / 2] = tmp;
            i = (i - 1) / 2;
        }
    } else if (fuzzy_better(hit, h[0])) {
        h[0] = hit;
        fuzzy_heap_down(h, r->heap_count, 0);
    } else {
        return;
    }
    r->rows_stale = 1;
}

void fuzzy_rank_begin(FuzzyRank *r, ProjectIndex *x, String8 input, b32 narrow) {
    if (narrow && r->started && r->x == x) {
        // What is still possible: this pass's survivors so far, the rest of its survivor list (appended in
        // fuzzy_rank_work, in chunks within the budget: up to 4 MB for a million paths), then the range it
        // did not reach (its survivors are all before that range).
        // While that copy is still pending no candidate was looked at: the list stays as it is.
        if (!r->copy_left) {
            r->copy_src = r->from + r->at_from;
            r->copy_left = r->from_count - r->at_from;
            r->covered = r->at;
            r->from = (u32 *)r->lists[r->to_list].base;
            r->from_count = r->to_count + r->copy_left;
            r->to_list ^= 1;
        }
    } else {
        r->copy_left = 0;
        arena_reset(&r->lists[0]);
        r->from = NULL;
        r->from_count = 0;
        r->covered = 0;
        r->to_list = 1;
    }
    arena_reset(&r->lists[r->to_list]);
    r->to_count = 0;
    r->at_from = 0;
    r->at = r->covered;
    r->x = x;
    r->started = 1;
    r->total = 0;
    r->heap_count = 0;
    r->row_count = 0;
    r->rows_stale = 1;
    r->path_dir = -1;
    arena_reset(&r->query_arena);
    r->q = fuzzy_query(&r->query_arena, input);
}

static void fuzzy_consider(FuzzyRank *r, u32 index) {
    ProjectIndex *x = r->x;
    ProjectEntry *e = (ProjectEntry *)x->files.base + index;
    if ((e->mask & r->q.mask) != r->q.mask) return;
    if ((i64)e->dir != r->path_dir) {
        String8 dir = project_index_dir(x, e->dir);
        memcpy(r->path, dir.data, (size_t)dir.len);
        r->path_dir_len = dir.len;
        if (dir.len) r->path[r->path_dir_len++] = '/';
        r->path_dir = e->dir;
    }
    String8 name = project_index_name(x, index);
    if (r->path_dir_len + name.len > PROJECT_PATH_MAX) return;
    memcpy(r->path + r->path_dir_len, name.data, (size_t)name.len);
    String8 path = str8(r->path, r->path_dir_len + name.len);
    r->dev_scored++;
    i32 score = fuzzy_score(&r->q, path, r->path_dir_len, NULL, 0, NULL);
    if (score == FUZZY_NO_MATCH) return;
    *PUSH_STRUCT(&r->lists[r->to_list], u32) = index;
    r->to_count++;
    r->total++;
    fuzzy_heap_add(r, (FuzzyHit){ score, (u32)path.len, index });
}

b32 fuzzy_rank_done(FuzzyRank *r) {
    return !r->started || (!r->copy_left && r->at_from >= r->from_count && r->at >= r->x->file_count);
}

b32 fuzzy_rank_work(FuzzyRank *r, u64 deadline, i64 budget) {
    if (!r->started) return 1;
    // First the rest of the narrowed list, 64 K entries at a time: no candidate is looked at before it is whole.
    while (r->copy_left > 0) {
        i64 chunk = MIN(r->copy_left, (i64)65536);
        u32 *dst = PUSH_ARRAY(&r->lists[r->to_list ^ 1], u32, chunk);
        memcpy(dst, r->copy_src, (size_t)chunk * 4);
        r->copy_src += chunk;
        r->copy_left -= chunk;
        if (deadline && os_time_us() >= deadline) return 0;
    }
    i64 used = 0;
    while (!fuzzy_rank_done(r)) {
        i64 slice = budget > 0 ? MIN((i64)FUZZY_SLICE, budget - used) : FUZZY_SLICE;
        if (slice <= 0) break;
        i64 k = 0, n = r->x->file_count;
        for (; k < slice && r->at_from < r->from_count; k++) fuzzy_consider(r, r->from[r->at_from++]);
        for (; k < slice && r->at < n; k++) fuzzy_consider(r, (u32)r->at++);
        used += k;
        if (deadline && os_time_us() >= deadline) break;
    }
    return fuzzy_rank_done(r);
}

void fuzzy_rank_finish(FuzzyRank *r) {
    fuzzy_rank_work(r, 0, 0);
}

i32 fuzzy_rank_rows(FuzzyRank *r) {
    if (!r->rows_stale) return r->row_count;
    u64 t0 = os_time_us();
    FuzzyHit h[FUZZY_TOP];
    i32 count = r->heap_count;
    memcpy(h, r->heap, sizeof(FuzzyHit) * (size_t)count);
    for (i32 k = count - 1; k >= 0; k--) { // the worst first, from the back
        r->rows[k] = h[0].index;
        h[0] = h[k];
        fuzzy_heap_down(h, k, 0);
    }
    r->row_count = count;
    r->rows_stale = 0;
    r->sort_us = os_time_us() - t0;
    return count;
}

i64 fuzzy_rank_path(FuzzyRank *r, u32 index, u8 *buf, i64 cap) {
    return project_index_path(r->x, index, buf, cap);
}
