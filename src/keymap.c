// keymap.c — see keymap.h.

// ---------------------------------------------------------------------------
// Letters

// Simple case pairs of the scripts keyboards produce directly: ASCII, Latin-1, Latin Extended-A,
// basic Greek and Cyrillic. Default Unicode mapping: 'I' lowercases to 'i' (also on Turkish layouts).
u32 key_letter_lower(u32 c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    if (c >= 'a' && c <= 'z') return c;
    if (c >= 0xC0 && c <= 0xDE && c != 0xD7) return c + 0x20;
    if (c >= 0xDF && c <= 0xFF && c != 0xF7) return c; // ß, à..ÿ
    if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130) return 'i';                                         // İ
        if (c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) return c; // ı ĸ ŉ ſ: lowercase only
        if (c == 0x178) return 0xFF;                                        // Ÿ
        b32 even_upper = c < 0x138 || (c >= 0x14A && c < 0x178);
        b32 upper = even_upper ? (c & 1) == 0 : (c & 1) == 1;
        return upper ? c + 1 : c;
    }
    if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) return c + 0x20;
    if (c >= 0x3B1 && c <= 0x3C9) return c;
    if (c >= 0x400 && c <= 0x40F) return c + 0x50;
    if (c >= 0x410 && c <= 0x42F) return c + 0x20;
    if (c >= 0x430 && c <= 0x45F) return c;
    return 0;
}

// ---------------------------------------------------------------------------
// Named keys

static const char *const key_names[KEY_COUNT] = {
    [KEY_ESCAPE] = "ESC", [KEY_TAB] = "TAB", [KEY_BACKSPACE] = "DEL", [KEY_ENTER] = "RET",
    [KEY_INSERT] = "<insert>", [KEY_DELETE] = "<delete>", [KEY_HOME] = "<home>", [KEY_END] = "<end>",
    [KEY_PAGE_UP] = "<prior>", [KEY_PAGE_DOWN] = "<next>",
    [KEY_LEFT] = "<left>", [KEY_RIGHT] = "<right>", [KEY_UP] = "<up>", [KEY_DOWN] = "<down>",
    [KEY_PAUSE] = "<pause>", [KEY_APPS] = "<apps>",
    [KEY_F1] = "<f1>", [KEY_F2] = "<f2>", [KEY_F3] = "<f3>", [KEY_F4] = "<f4>", [KEY_F5] = "<f5>",
    [KEY_F6] = "<f6>", [KEY_F7] = "<f7>", [KEY_F8] = "<f8>", [KEY_F9] = "<f9>", [KEY_F10] = "<f10>",
    [KEY_F11] = "<f11>", [KEY_F12] = "<f12>", [KEY_F13] = "<f13>", [KEY_F14] = "<f14>", [KEY_F15] = "<f15>",
    [KEY_F16] = "<f16>", [KEY_F17] = "<f17>", [KEY_F18] = "<f18>", [KEY_F19] = "<f19>", [KEY_F20] = "<f20>",
    [KEY_F21] = "<f21>", [KEY_F22] = "<f22>", [KEY_F23] = "<f23>", [KEY_F24] = "<f24>",
};

// Accepted on input only.
static const struct { const char *name; Key key; } key_aliases[] = {
    { "<pageup>", KEY_PAGE_UP }, { "<pagedown>", KEY_PAGE_DOWN }, { "<return>", KEY_ENTER },
    { "<tab>", KEY_TAB }, { "<escape>", KEY_ESCAPE }, { "<backspace>", KEY_BACKSPACE },
};

b32 key_is_named(Key key) {
    return key > KEY_NONE && key < KEY_COUNT && key_names[key] != NULL;
}

static Key key_named_from_text(String8 s) {
    for (i32 k = 0; k < KEY_COUNT; k++) {
        if (key_names[k] && str8_equal(s, str8_cstr(key_names[k]))) return (Key)k;
    }
    for (i32 i = 0; i < ARRAY_COUNT(key_aliases); i++) {
        if (str8_equal(s, str8_cstr(key_aliases[i].name))) return key_aliases[i].key;
    }
    return KEY_NONE;
}

// ---------------------------------------------------------------------------
// Chords from events

b32 key_chord_from_event(Key key, u32 codepoint, u32 mods, KeyChord *out) {
    u32 cm = ((mods & MOD_CTRL) ? CHORD_CTRL : 0) | ((mods & MOD_ALT) ? CHORD_META : 0);
    u32 shift = (mods & MOD_SHIFT) ? CHORD_SHIFT : 0;
    if (key_is_named(key)) {
        *out = CHORD_NAMED | (u32)key | cm | shift;
        return 1;
    }
    if (!cm || !codepoint || codepoint > CHORD_CODE_MASK) return 0; // plain: the text event is the chord
    u32 lower = key_letter_lower(codepoint);
    *out = lower ? (lower | cm | shift) : (codepoint | cm); // Shift stays only on letters
    return 1;
}

KeyChord key_chord_from_text(u32 codepoint) {
    return codepoint & CHORD_CODE_MASK;
}

// ---------------------------------------------------------------------------
// kbd notation

static b32 key_is_modifier_prefix(String8 t, i64 i) {
    return i + 2 < t.len && t.data[i + 1] == '-' && (t.data[i] == 'C' || t.data[i] == 'M' || t.data[i] == 'S');
}

b32 key_chord_parse(String8 t, KeyChord *out, const char **error) {
    u32 mods = 0;
    i64 i = 0;
    while (key_is_modifier_prefix(t, i)) {
        u32 m = t.data[i] == 'C' ? CHORD_CTRL : t.data[i] == 'M' ? CHORD_META : CHORD_SHIFT;
        if (mods & m) {
            *error = "repeated modifier";
            return 0;
        }
        mods |= m;
        i += 2;
    }
    String8 rest = str8(t.data + i, t.len - i);
    if (rest.len == 0) {
        *error = "empty key";
        return 0;
    }
    if (rest.len == 2 && rest.data[1] == '-' && (rest.data[0] == 'C' || rest.data[0] == 'M' || rest.data[0] == 'S')) {
        *error = "modifier without a key";
        return 0;
    }
    if (str8_equal(rest, STR8_LIT("SPC"))) rest = STR8_LIT(" ");
    Key named = KEY_NONE;
    if (rest.len >= 3 && rest.data[0] == '<' && rest.data[rest.len - 1] == '>') {
        named = key_named_from_text(rest);
        if (!named) {
            *error = "unknown key name";
            return 0;
        }
    } else if (rest.len == 3) {
        named = key_named_from_text(rest); // RET TAB ESC DEL
    }
    if (named) {
        *out = CHORD_NAMED | (u32)named | mods;
        return 1;
    }
    i64 advance;
    u32 cp = utf8_decode(rest.data, rest.len, &advance);
    if (advance != rest.len) {
        *error = "more than one character (separate chords with spaces)";
        return 0;
    }
    if ((cp == UTF_REPLACEMENT && advance == 1) || cp < 0x20 || cp == 0x7F || (cp >= 0x80 && cp <= 0x9F)) {
        *error = "not a printable character";
        return 0;
    }
    u32 lower = key_letter_lower(cp);
    if (mods & (CHORD_CTRL | CHORD_META)) {
        if (lower && lower != cp) { // C-A = C-S-a
            cp = lower;
            mods |= CHORD_SHIFT;
        } else if (!lower && (mods & CHORD_SHIFT)) {
            *error = "S- applies only to letters and named keys (Shift is part of the character)";
            return 0;
        }
    } else if (mods & CHORD_SHIFT) {
        *error = "S- with a character needs C- or M- (write the shifted character itself)";
        return 0;
    }
    *out = cp | mods;
    return 1;
}

static b32 key_is_space(u8 c) {
    return c == ' ' || c == '\t';
}

b32 key_seq_parse(String8 text, KeySeq *out, const char **error) {
    out->len = 0;
    i64 i = 0;
    for (;;) {
        while (i < text.len && key_is_space(text.data[i])) i++;
        if (i >= text.len) break;
        i64 start = i;
        while (i < text.len && !key_is_space(text.data[i])) i++;
        if (out->len == KEY_SEQ_MAX) {
            *error = "more than 4 keys";
            return 0;
        }
        if (!key_chord_parse(str8(text.data + start, i - start), &out->chords[out->len], error)) return 0;
        out->len++;
    }
    if (out->len == 0) {
        *error = "empty key sequence";
        return 0;
    }
    return 1;
}

// Appends s to out[*n, cap), truncating.
static void key_put(u8 *out, i64 cap, i64 *n, String8 s) {
    i64 k = MIN(s.len, cap - *n);
    if (k > 0) memcpy(out + *n, s.data, (size_t)k);
    *n += MAX(k, 0);
}

i64 key_chord_print(KeyChord c, u8 *out, i64 cap) {
    i64 n = 0;
    if (c & CHORD_CTRL) key_put(out, cap, &n, STR8_LIT("C-"));
    if (c & CHORD_META) key_put(out, cap, &n, STR8_LIT("M-"));
    if (c & CHORD_SHIFT) key_put(out, cap, &n, STR8_LIT("S-"));
    u32 code = c & CHORD_CODE_MASK;
    if (c & CHORD_NAMED) {
        key_put(out, cap, &n, key_is_named((Key)code) ? str8_cstr(key_names[code]) : STR8_LIT("<?>"));
    } else if (code == ' ') {
        key_put(out, cap, &n, STR8_LIT("SPC"));
    } else {
        u8 bytes[4];
        key_put(out, cap, &n, str8(bytes, utf8_encode(code, bytes)));
    }
    return n;
}

i64 key_seq_print(KeySeq *seq, u8 *out, i64 cap) {
    i64 n = 0;
    for (i32 i = 0; i < seq->len; i++) {
        if (i > 0) key_put(out, cap, &n, STR8_LIT(" "));
        n += key_chord_print(seq->chords[i], out + n, cap - n);
    }
    return n;
}

b32 key_seq_equal(KeySeq *a, KeySeq *b) {
    if (a->len != b->len) return 0;
    for (i32 i = 0; i < a->len; i++) if (a->chords[i] != b->chords[i]) return 0;
    return 1;
}

// a is a proper prefix of b.
static b32 key_seq_is_prefix(KeySeq *a, KeySeq *b) {
    if (a->len >= b->len) return 0;
    for (i32 i = 0; i < a->len; i++) if (a->chords[i] != b->chords[i]) return 0;
    return 1;
}

// ---------------------------------------------------------------------------
// Keymaps

i32 keymap_bind(Keymap *map, KeySeq *seq, const Command *command) {
    i32 removed = 0, w = 0;
    b32 found = 0;
    for (i32 r = 0; r < map->count; r++) {
        KeyBinding b = map->bindings[r];
        if (key_seq_equal(&b.seq, seq)) {
            if (!command) continue;
            b.command = command;
            found = 1;
        } else if (command && (key_seq_is_prefix(&b.seq, seq) || key_seq_is_prefix(seq, &b.seq))) {
            removed++;
            continue;
        }
        map->bindings[w++] = b;
    }
    map->count = w;
    if (command && !found) {
        if (map->count == KEYMAP_CAP) return -1;
        map->bindings[map->count++] = (KeyBinding){ *seq, command };
    }
    return removed;
}

const Command *keymap_get(Keymap *map, KeySeq *seq) {
    for (i32 i = 0; i < map->count; i++) {
        if (key_seq_equal(&map->bindings[i].seq, seq)) return map->bindings[i].command;
    }
    return NULL;
}

b32 keymap_has_prefix(Keymap *map, KeySeq *seq) {
    for (i32 i = 0; i < map->count; i++) {
        if (key_seq_is_prefix(seq, &map->bindings[i].seq)) return 1;
    }
    return 0;
}

// The first exact match in stack order; failing that, whether seq is a prefix in any map.
static const Command *key_lookup(Keymap **stack, i32 count, KeySeq *seq, b32 *prefix) {
    *prefix = 0;
    for (i32 i = 0; i < count; i++) {
        const Command *c = keymap_get(stack[i], seq);
        if (c) return c;
    }
    for (i32 i = 0; i < count && !*prefix; i++) *prefix = keymap_has_prefix(stack[i], seq);
    return NULL;
}

#if TEAL_DEV
i32 key_dev_events(String8 token, Event out[2], const char **error) {
    KeyChord c;
    if (!key_chord_parse(token, &c, error)) return 0;
    u32 code = c & CHORD_CODE_MASK;
    u32 mods = ((c & CHORD_CTRL) ? MOD_CTRL : 0) | ((c & CHORD_META) ? MOD_ALT : 0) | ((c & CHORD_SHIFT) ? MOD_SHIFT : 0);
    if (c & CHORD_NAMED) {
        out[0] = (Event){ .kind = EVENT_KEY_DOWN, .key = (Key)code, .mods = mods };
        return 1;
    }
    if (mods & (MOD_CTRL | MOD_ALT)) {
        out[0] = (Event){ .kind = EVENT_KEY_DOWN, .key = code == ' ' ? KEY_SPACE : KEY_NONE, .codepoint = code, .mods = mods };
        return 1;
    }
    out[0] = (Event){ .kind = EVENT_KEY_DOWN, .key = code == ' ' ? KEY_SPACE : KEY_NONE, .codepoint = code };
    out[1] = (Event){ .kind = EVENT_TEXT, .codepoint = code };
    return 2;
}
#endif

// ---------------------------------------------------------------------------
// The state machine

void key_input_feed(KeyInput *in, Keymap **stack, i32 count, Event *e, KeyResult *out) {
    memset(out, 0, sizeof(*out));
    KeyChord chord;
    if (e->kind == EVENT_KEY_DOWN) {
        in->drop_text = 0;
        if (!key_chord_from_event(e->key, e->codepoint, e->mods, &chord)) {
            out->kind = KEY_RESULT_IGNORED;
            return;
        }
        in->drop_text = 1; // its text events (if any) belong to this chord
    } else if (e->kind == EVENT_TEXT) {
        if (in->drop_text) {
            out->kind = KEY_RESULT_DROPPED;
            return;
        }
        chord = key_chord_from_text(e->codepoint);
    } else {
        out->kind = KEY_RESULT_IGNORED;
        return;
    }

    KeySeq seq = in->pending;
    if (seq.len == KEY_SEQ_MAX) seq.len = 0; // cannot happen: a prefix is shorter than its binding
    seq.chords[seq.len++] = chord;
    out->seq = seq;
    u32 code = chord & CHORD_CODE_MASK;
    out->codepoint = !(chord & CHORD_NAMED) ? code : code == KEY_TAB ? '\t' : 0;

    b32 prefix;
    const Command *command = key_lookup(stack, count, &seq, &prefix);
    KeySeq used = seq;
    if (!command && !prefix && (chord & CHORD_SHIFT)) {
        used.chords[used.len - 1] &= ~CHORD_SHIFT;
        command = key_lookup(stack, count, &used, &prefix);
        out->shift_translated = command || prefix;
    }
    if (prefix) {
        in->pending = used;
        out->kind = KEY_RESULT_PREFIX;
        return;
    }
    b32 had_prefix = in->pending.len > 0;
    in->pending.len = 0;
    b32 describe = in->describe;
    in->describe = 0;
    if (!command && had_prefix) {
        // keyboard-quit (C-g, ESC) cancels a pending prefix.
        KeySeq single = { { chord & ~CHORD_SHIFT }, 1 };
        b32 unused;
        if (key_lookup(stack, count, &single, &unused) == &CMD_KEYBOARD_QUIT) {
            out->kind = KEY_RESULT_QUIT;
            out->command = &CMD_KEYBOARD_QUIT;
            return;
        }
    }
    b32 plain = !(chord & (CHORD_NAMED | CHORD_CTRL | CHORD_META | CHORD_SHIFT));
    if (!command && !had_prefix && plain) {
        out->command = &CMD_SELF_INSERT; // an unbound plain character
        out->kind = describe ? KEY_RESULT_DESCRIBE : KEY_RESULT_SELF_INSERT;
        return;
    }
    out->command = command;
    out->kind = describe ? KEY_RESULT_DESCRIBE : command ? KEY_RESULT_COMMAND : KEY_RESULT_UNDEFINED;
}
