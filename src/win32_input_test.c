// win32_input_test.c — dev builds, part of --test: the real Win32 input path. Key messages go to a
// window that is never shown (it never gets focus or input) and uses the real window procedure; it is
// a top-level window, so Alt+F4 takes the system's close path. The thread's key state is set as Windows
// would set it (SetKeyboardState), the message Windows sends is posted with its scan code and flags,
// and the normal pump runs (PeekMessage, TranslateMessage, DispatchMessage). The events then go
// through a headless app. Keyboard layouts are loaded and activated for the test thread only; the
// session's layouts and the foreground window's layout are checked to be unchanged afterwards. No
// SendInput: nothing reaches another window.

#if TEAL_DEV

enum {
    IT_LSHIFT = 1 << 0,
    IT_RSHIFT = 1 << 1,
    IT_LCTRL  = 1 << 2,
    IT_RCTRL  = 1 << 3,
    IT_LALT   = 1 << 4,
    IT_RALT   = 1 << 5, // Right Alt: AltGr on a layout that has it (a synthetic Left Ctrl comes first)
};

typedef struct ItStroke {
    u32 mods; // IT_*
    u32 vk;   // 0: the modifiers alone
    b32 ext;  // the extended-key flag
} ItStroke;

typedef struct InputTest {
    Platform *p;
    HWND hwnd;
    HKL hkl;          // the layout under test
    HKL us;           // US, for "the US keys on this layout"
    const char *name;
    b32 altgr;        // Right Alt is AltGr on this layout
    BYTE state[256];  // the thread's key state, as Windows would have it
    b32 alt_alone;    // Alt went down and no other key since (its release is then WM_SYSKEYUP)
    App *app;
    Arena arena;
    b32 quit;         // the app quit
    i32 failures;
} InputTest;

static void it_fail(InputTest *t, const char *fmt, ...) {
    u8 text[512];
    va_list args;
    va_start(args, fmt);
    i64 n = fmt_v(text, sizeof(text), fmt, args);
    va_end(args);
    LOG("test: FAIL: input %s: %S", t->name, str8(text, MIN(n, (i64)sizeof(text))));
    t->failures++;
}

static void it_pump(void) {
    MSG msg;
    while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// The events the window procedure produced, through the app.
static void it_flush(InputTest *t) {
    Platform *p = t->p;
    if (p->event_count && !t->quit && !app_dev_feed_events(t->app, p->events, p->event_count, &t->arena)) t->quit = 1;
    p->event_count = 0;
}

static void it_set(InputTest *t, u32 vk, b32 down) {
    t->state[vk] = (BYTE)((down ? 0x80 : 0) | (t->state[vk] & 1));
    t->state[VK_SHIFT] = (t->state[VK_LSHIFT] | t->state[VK_RSHIFT]) & 0x80;
    t->state[VK_CONTROL] = (t->state[VK_LCONTROL] | t->state[VK_RCONTROL]) & 0x80;
    t->state[VK_MENU] = (t->state[VK_LMENU] | t->state[VK_RMENU]) & 0x80;
}

static b32 it_down(InputTest *t, u32 vk) {
    return (t->state[vk] & 0x80) != 0;
}

// The message for a transition, as Windows picks it with the state after it: WM_SYSKEY* while Alt is
// down without Ctrl, for F10, and for the release of an Alt pressed alone.
static void it_post(InputTest *t, u32 vk_msg, u32 scan, b32 ext, b32 down, b32 was_down, b32 alone_alt_up) {
    b32 alt = it_down(t, VK_MENU), ctrl = it_down(t, VK_CONTROL);
    b32 sys = (alt && !ctrl) || (vk_msg == VK_F10 && !ctrl) || alone_alt_up;
    UINT msg = down ? (sys ? WM_SYSKEYDOWN : WM_KEYDOWN) : (sys ? WM_SYSKEYUP : WM_KEYUP);
    LPARAM lp = (LPARAM)(1u | (scan & 0xFF) << 16 | (ext ? 1u : 0u) << 24 | (sys && alt ? 1u : 0u) << 29 |
                         (was_down ? 1u : 0u) << 30 | (down ? 0u : 1u) << 31);
    PostMessageW(t->hwnd, msg, vk_msg, lp);
}

// One key goes down or up: modifiers by their left / right virtual key.
static void it_key(InputTest *t, u32 vk, b32 down, b32 ext) {
    u32 vk_msg = vk, scan;
    switch (vk) {
    case VK_LSHIFT:   vk_msg = VK_SHIFT;   scan = 0x2A; break;
    case VK_RSHIFT:   vk_msg = VK_SHIFT;   scan = 0x36; break;
    case VK_LCONTROL: vk_msg = VK_CONTROL; scan = 0x1D; break;
    case VK_RCONTROL: vk_msg = VK_CONTROL; scan = 0x1D; ext = 1; break;
    case VK_LMENU:    vk_msg = VK_MENU;    scan = 0x38; break;
    case VK_RMENU:    vk_msg = VK_MENU;    scan = 0x38; ext = 1; break;
    default:          scan = MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, t->hkl); break;
    }
    b32 was_down = it_down(t, vk);
    b32 alone_alt_up = !down && vk_msg == VK_MENU && t->alt_alone;
    if (down) t->alt_alone = vk_msg == VK_MENU && !was_down ? !it_down(t, VK_CONTROL) : vk_msg == VK_MENU && t->alt_alone;
    else if (vk_msg == VK_MENU) t->alt_alone = 0;
    it_set(t, vk, down);
    SetKeyboardState(t->state);
    it_post(t, vk_msg, scan, ext, down, was_down, alone_alt_up);
    it_pump();
}

// AltGr: a synthetic Left Ctrl, then Right Alt, with the same message time (the window procedure
// relies on it). Posted together; taken back and posted again if the tick changed in between.
static void it_altgr(InputTest *t, b32 down) {
    BYTE before[256];
    memcpy(before, t->state, sizeof(before));
    for (i32 attempt = 0; attempt < 20; attempt++) {
        it_set(t, VK_LCONTROL, down);
        it_set(t, VK_RMENU, down);
        SetKeyboardState(t->state);
        DWORD t0 = GetTickCount();
        PostMessageW(t->hwnd, down ? WM_KEYDOWN : WM_KEYUP, VK_CONTROL,
                     (LPARAM)(1u | 0x1Du << 16 | (down ? 0u : 3u << 30)));
        PostMessageW(t->hwnd, down ? WM_KEYDOWN : WM_KEYUP, VK_MENU,
                     (LPARAM)(1u | 0x38u << 16 | 1u << 24 | (down ? 0u : 3u << 30)));
        if (GetTickCount() == t0) {
            it_pump();
            return;
        }
        MSG msg;
        while (PeekMessageW(&msg, NULL, 0, 0, PM_REMOVE)) {}
        memcpy(t->state, before, sizeof(before));
    }
    it_fail(t, "AltGr: the two messages never got the same time");
}

static void it_mods(InputTest *t, u32 mods, b32 down) {
    static const struct { u32 bit, vk; } order[] = {
        { IT_LCTRL, VK_LCONTROL }, { IT_RCTRL, VK_RCONTROL }, { IT_LALT, VK_LMENU }, { IT_RALT, VK_RMENU },
        { IT_LSHIFT, VK_LSHIFT }, { IT_RSHIFT, VK_RSHIFT },
    };
    for (i32 k = 0; k < ARRAY_COUNT(order); k++) {
        i32 i = down ? k : ARRAY_COUNT(order) - 1 - k;
        if (!(mods & order[i].bit)) continue;
        if (order[i].bit == IT_RALT && t->altgr) it_altgr(t, down);
        else it_key(t, order[i].vk, down, 0);
    }
}

// A key pressed with modifiers, everything released, the events through the app.
static void it_stroke(InputTest *t, ItStroke s) {
    it_mods(t, s.mods, 1);
    if (s.vk) {
        it_key(t, s.vk, 1, s.ext);
        it_key(t, s.vk, 0, s.ext);
    }
    it_mods(t, s.mods, 0);
    it_flush(t);
}

// The character a key types with Shift / AltGr on the layout (a dead key: its accent, *dead set).
static u32 it_layout_char(InputTest *t, HKL hkl, u32 vk, u32 mods, b32 *dead) {
    BYTE state[256] = { 0 };
    if (mods & (IT_LSHIFT | IT_RSHIFT)) state[VK_SHIFT] = state[VK_LSHIFT] = 0x80;
    if (mods & IT_RALT) state[VK_CONTROL] = state[VK_LCONTROL] = state[VK_MENU] = state[VK_RMENU] = 0x80;
    WCHAR buf[8];
    int n = ToUnicodeEx(vk, MapVirtualKeyExW(vk, MAPVK_VK_TO_VSC, hkl), state, buf, ARRAY_COUNT(buf), 0x4, hkl);
    (void)t;
    if (dead) *dead = n < 0;
    return n == 1 || n == -1 ? buf[0] : 0;
}

// The stroke that types `c` on a layout: VkKeyScanExW, else a search of every key with Shift / AltGr.
static b32 it_char_stroke(InputTest *t, HKL hkl, b32 altgr, u32 c, ItStroke *out) {
    SHORT r = VkKeyScanExW((WCHAR)c, hkl);
    u32 vk = 0, sh = 0;
    if (r != -1) {
        vk = (u32)r & 0xFF;
        sh = ((u32)r >> 8) & 0xFF;
    } else {
        static const u32 states[] = { 0, IT_LSHIFT, IT_RALT, IT_RALT | IT_LSHIFT };
        for (i32 s = 0; s < ARRAY_COUNT(states) && !vk; s++) {
            if ((states[s] & IT_RALT) && !altgr) continue;
            for (u32 k = 0x08; k < 0xFF && !vk; k++) {
                if (it_layout_char(t, hkl, k, states[s], NULL) == c) {
                    vk = k;
                    sh = ((states[s] & IT_LSHIFT) ? 1 : 0) | ((states[s] & IT_RALT) ? 6 : 0);
                }
            }
        }
        if (!vk) return 0;
    }
    if ((sh & ~7u) || (sh & 6) == 2 || (sh & 6) == 4) return 0; // a Ctrl-only or Alt-only character, or Kana
    if ((sh & 6) == 6 && !altgr) return 0;
    *out = (ItStroke){ .mods = ((sh & 1) ? IT_LSHIFT : 0) | ((sh & 6) == 6 ? IT_RALT : 0), .vk = vk };
    return 1;
}

static b32 it_is_extended(u32 vk) {
    return vk == VK_INSERT || vk == VK_DELETE || vk == VK_HOME || vk == VK_END || vk == VK_PRIOR || vk == VK_NEXT ||
           vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN || vk == VK_APPS;
}

// The keys of a chord on a layout: Ctrl is Left Ctrl (Right Ctrl with AltGr), Meta is Left Alt.
static b32 it_chord_stroke(InputTest *t, HKL hkl, b32 altgr, KeyChord c, ItStroke *out) {
    u32 code = c & CHORD_CODE_MASK;
    if (c & CHORD_NAMED) {
        u32 vk = 0;
        for (u32 k = 1; k < 0xFF && !vk; k++) if (win32_map_vk(k) == (Key)code) vk = k;
        if (!vk) return 0;
        *out = (ItStroke){ .vk = vk, .ext = it_is_extended(vk) };
    } else if (!it_char_stroke(t, hkl, altgr, code, out)) {
        return 0;
    }
    if (c & CHORD_SHIFT) out->mods |= IT_LSHIFT;
    if (c & CHORD_CTRL) out->mods |= (out->mods & IT_RALT) ? IT_RCTRL : IT_LCTRL;
    if (c & CHORD_META) out->mods |= IT_LALT;
    return 1;
}

// "LCtrl+LAlt+Shift+[,]": a stroke named by US key positions.
static String8 it_stroke_name(InputTest *t, Arena *a, ItStroke s) {
    String8 key = s.vk == VK_SPACE ? STR8_LIT("Space")
                : (s.vk >= 'A' && s.vk <= 'Z') || (s.vk >= '0' && s.vk <= '9')
                ? str8_fmt(a, "%c", (int)s.vk)
                : str8_fmt(a, "[%s]", win32_dev_us_legend(MapVirtualKeyExW(s.vk, MAPVK_VK_TO_VSC, t->hkl)));
    if (win32_map_vk(s.vk) != KEY_NONE && key_is_named(win32_map_vk(s.vk))) key = str8_cstr(key_names[win32_map_vk(s.vk)]);
    return str8_fmt(a, "%s%s%s%s%s%s%S", (s.mods & IT_LCTRL) ? "LCtrl+" : "", (s.mods & IT_RCTRL) ? "RCtrl+" : "",
                    (s.mods & IT_LALT) ? "LAlt+" : "", (s.mods & IT_RALT) ? (t->altgr ? "AltGr+" : "RAlt+") : "",
                    (s.mods & IT_LSHIFT) ? "Shift+" : "", (s.mods & IT_RSHIFT) ? "RShift+" : "", key);
}

// describe-key through the real path: C-h k, then the strokes. The echo area's text.
static String8 it_describe(InputTest *t, ItStroke *s, i32 n) {
    it_stroke(t, (ItStroke){ IT_LCTRL, 'H', 0 });
    it_stroke(t, (ItStroke){ 0, 'K', 0 });
    for (i32 i = 0; i < n; i++) it_stroke(t, s[i]);
    String8 echo = str8_copy(&t->arena, app_dev_echo(t->app));
    app_dev_feed(t->app, "C-g", &t->arena); // ends a describe-key still waiting; clears the echo
    return echo;
}

// What describe-key says for a sequence in kbd notation, from the app's own keymap.
static String8 it_expected(InputTest *t, String8 want) {
    for (i32 i = 0;; i++) {
        const char *command;
        String8 b = app_dev_binding(t->app, &t->arena, 0, i, &command);
        if (!b.len) break;
        if (str8_equal(b, want)) return str8_fmt(&t->arena, "%S runs the command %s", want, command);
    }
    return str8_fmt(&t->arena, "%S is undefined", want);
}

static void it_expect_describe(InputTest *t, const char *what, ItStroke s, String8 seq) {
    String8 got = it_describe(t, &s, 1), want = it_expected(t, seq);
    if (!str8_equal(got, want)) it_fail(t, "%s (%S): echo '%S', expected '%S'", what, it_stroke_name(t, &t->arena, s), got, want);
}

static void it_expect_text(InputTest *t, const char *what, ItStroke *s, i32 n, String8 want) {
    app_dev_show_scratch(t->app, STR8_LIT(""));
    for (i32 i = 0; i < n; i++) it_stroke(t, s[i]);
    String8 got = app_dev_text(t->app, &t->arena);
    if (!str8_equal(got, want)) it_fail(t, "%s: the buffer has '%S', expected '%S'", what, got, want);
}

static String8 it_utf8(Arena *a, u32 c) {
    u8 *b = PUSH_ARRAY(a, u8, 4);
    return str8(b, utf8_encode(c, b));
}

// The cases every layout must pass.
static void it_cases(InputTest *t) {
    Arena *a = &t->arena;

    // Letters, plain and with Shift: the layout's characters reach the buffer as text.
    ItStroke letters[52];
    String8 want = { 0 };
    for (u32 i = 0; i < 52; i++) {
        letters[i] = (ItStroke){ i < 26 ? 0 : IT_LSHIFT, 'A' + i % 26, 0 };
        String8 c = it_utf8(a, it_layout_char(t, t->hkl, 'A' + i % 26, letters[i].mods, NULL));
        want = want.len ? str8_fmt(a, "%S%S", want, c) : c;
    }
    if (t->hkl == t->us && !str8_equal(want, STR8_LIT("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"))) {
        it_fail(t, "US letters from ToUnicodeEx: '%S'", want);
    }
    it_expect_text(t, "letters a-z, then with Shift", letters, 52, want);

    // Meta, Ctrl, Ctrl+Meta with a letter.
    it_expect_describe(t, "Left Alt + letter", (ItStroke){ IT_LALT, 'F', 0 }, STR8_LIT("M-f"));
    it_expect_describe(t, "Left Ctrl + letter", (ItStroke){ IT_LCTRL, 'F', 0 }, STR8_LIT("C-f"));
    it_expect_describe(t, "Left Ctrl + Left Alt + letter", (ItStroke){ IT_LCTRL | IT_LALT, 'F', 0 }, STR8_LIT("C-M-f"));
    it_expect_describe(t, "Right Ctrl + Left Alt + letter", (ItStroke){ IT_RCTRL | IT_LALT, 'X', 0 }, STR8_LIT("C-M-x"));
    if (!t->altgr) {
        it_expect_describe(t, "Right Alt + letter (no AltGr)", (ItStroke){ IT_RALT, 'F', 0 }, STR8_LIT("M-f"));
    } else {
        // AltGr + letter types the layout's AltGr character, and no command runs.
        ItStroke s = { IT_RALT, 'E', 0 };
        u32 c = it_layout_char(t, t->hkl, 'E', IT_RALT, NULL);
        if (!c) it_fail(t, "AltGr + E types nothing on this layout");
        it_expect_text(t, "AltGr + E", &s, 1, c ? it_utf8(a, c) : STR8_LIT(""));
        // Left Ctrl + Left Alt is AltGr inside ToUnicode: still C-M-e, and no text.
        ItStroke ce = { IT_LCTRL | IT_LALT, 'E', 0 };
        it_expect_text(t, "Left Ctrl + Left Alt + E types nothing", &ce, 1, STR8_LIT(""));
        it_expect_describe(t, "Left Ctrl + Left Alt + E (an AltGr character)", ce, STR8_LIT("C-M-e"));
    }

    // Alt + Shift + a symbol key: M- and the shifted character; Ctrl + Shift likewise.
    static const char meta_shifted[] = "<>{}%^";
    for (i32 i = 0; meta_shifted[i]; i++) {
        ItStroke s;
        b32 found = it_char_stroke(t, t->hkl, t->altgr, (u32)meta_shifted[i], &s);
        if (!found || s.mods != IT_LSHIFT) {
            if (t->hkl == t->us) it_fail(t, "'%c' is not a Shift character on US", meta_shifted[i]);
            continue;
        }
        s.mods |= IT_LALT;
        u8 seq[3] = { 'M', '-', (u8)meta_shifted[i] };
        it_expect_describe(t, "Left Alt + Shift + symbol", s, str8(seq, 3));
    }
    static const char ctrl_shifted[] = "?_@+";
    for (i32 i = 0; ctrl_shifted[i]; i++) {
        ItStroke s;
        if (!it_char_stroke(t, t->hkl, t->altgr, (u32)ctrl_shifted[i], &s) || s.mods != IT_LSHIFT) {
            if (t->hkl == t->us) it_fail(t, "'%c' is not a Shift character on US", ctrl_shifted[i]);
            continue;
        }
        s.mods |= IT_LCTRL;
        u8 seq[3] = { 'C', '-', (u8)ctrl_shifted[i] };
        it_expect_describe(t, "Left Ctrl + Shift + symbol", s, str8(seq, 3));
    }

    // Left Alt + AltGr + key: M- and the AltGr character.
    if (t->altgr) {
        static const u32 candidates[] = { '{', '}', '[', ']', '\\', '@', '|', 0x20AC };
        b32 done = 0;
        for (i32 i = 0; i < ARRAY_COUNT(candidates) && !done; i++) {
            ItStroke s;
            if (!it_char_stroke(t, t->hkl, 1, candidates[i], &s) || !(s.mods & IT_RALT)) continue;
            s.mods |= IT_LALT;
            String8 seq = str8_fmt(a, "M-%S", it_utf8(a, candidates[i]));
            it_expect_describe(t, "Left Alt + AltGr + key", s, seq);
            done = 1;
        }
        if (!done) it_fail(t, "no AltGr character found for Left Alt + AltGr");
    }

    // Dead keys: the accent and a letter compose, no command runs; as a Meta chord the accent leaves
    // nothing behind for the next key.
    static const struct { u32 accent, composed; } accents[] = { { 0xB4, 0xE9 }, { '`', 0xE8 }, { '^', 0xEA }, { 0xA8, 0xEB } };
    i32 dead_found = 0;
    for (i32 i = 0; i < ARRAY_COUNT(accents); i++) {
        static const u32 states[] = { 0, IT_LSHIFT, IT_RALT, IT_RALT | IT_LSHIFT };
        ItStroke dead = { 0 };
        for (i32 s = 0; s < ARRAY_COUNT(states) && !dead.vk; s++) {
            if ((states[s] & IT_RALT) && !t->altgr) continue;
            for (u32 k = 0x08; k < 0xFF && !dead.vk; k++) {
                b32 is_dead;
                if (it_layout_char(t, t->hkl, k, states[s], &is_dead) == accents[i].accent && is_dead) dead = (ItStroke){ states[s], k, 0 };
            }
        }
        if (!dead.vk) continue;
        dead_found++;
        ItStroke seq[2] = { dead, { 0, 'E', 0 } };
        it_expect_text(t, "dead key then e", seq, 2, it_utf8(a, accents[i].composed));
        ItStroke meta = dead;
        meta.mods |= IT_LALT;
        String8 chord = str8_fmt(a, "M-%S", it_utf8(a, accents[i].accent));
        it_expect_describe(t, "Left Alt + dead key", meta, chord);
        ItStroke e = { 0, 'E', 0 };
        it_expect_text(t, "e right after Left Alt + dead key", &e, 1, STR8_LIT("e"));
    }
    LOG("test: input %s: %d dead key accent(s) of ´ ` ^ ¨ tested", t->name, dead_found);

    // Alt alone, F10, Alt + Space: no system menu, no beep (nothing left to DefWindowProc).
    i32 paths = t->p->dev_menu_paths, keymenu = t->p->dev_keymenu;
    it_stroke(t, (ItStroke){ IT_LALT, 0, 0 });
    it_stroke(t, (ItStroke){ IT_RALT, 0, 0 });
    it_stroke(t, (ItStroke){ 0, VK_F10, 0 });
    it_stroke(t, (ItStroke){ IT_LALT, VK_SPACE, 0 });
    if (t->p->dev_menu_paths != paths || t->p->dev_keymenu != keymenu) {
        it_fail(t, "Alt alone, F10, Alt + Space: %d message(s) left to DefWindowProc, %d SC_KEYMENU", t->p->dev_menu_paths - paths,
                t->p->dev_keymenu - keymenu);
    }
    it_expect_describe(t, "F10", (ItStroke){ 0, VK_F10, 0 }, STR8_LIT("<f10>"));
    it_expect_describe(t, "Left Alt + Space", (ItStroke){ IT_LALT, VK_SPACE, 0 }, STR8_LIT("M-SPC"));

    // The numeric keypad: Enter; with NumLock off its navigation keys; with NumLock on its digits,
    // also right after a chord.
    it_expect_describe(t, "numpad Enter", (ItStroke){ 0, VK_RETURN, 1 }, STR8_LIT("RET"));
    it_expect_describe(t, "numpad 7, NumLock off", (ItStroke){ 0, VK_HOME, 0 }, STR8_LIT("<home>"));
    it_expect_describe(t, "numpad 9, NumLock off", (ItStroke){ 0, VK_PRIOR, 0 }, STR8_LIT("<prior>"));
    it_expect_describe(t, "numpad 4, NumLock off", (ItStroke){ 0, VK_LEFT, 0 }, STR8_LIT("<left>"));
    it_expect_describe(t, "numpad 0, NumLock off", (ItStroke){ 0, VK_INSERT, 0 }, STR8_LIT("<insert>"));
    t->state[VK_NUMLOCK] |= 1;
    ItStroke digits[4] = { { 0, VK_NUMPAD4, 0 }, { 0, VK_ADD, 0 }, { IT_LCTRL, 'L', 0 }, { 0, VK_NUMPAD5, 0 } };
    it_expect_text(t, "NumLock on: numpad 4, +, C-l, numpad 5", digits, 4, STR8_LIT("4+5"));
    t->state[VK_NUMLOCK] &= (BYTE)~1;
    SetKeyboardState(t->state);
}

// Every binding of the default config: the keys that type its chords on this layout, and what
// describe-key reports for them (each must be reachable on US). Then the US keys of each binding
// pressed on this layout, and what they run here.
static void it_bindings(InputTest *t) {
    Arena *a = &t->arena;
    i32 total = 0, unreachable = 0, elsewhere = 0, wrong = 0;
    for (i32 map = 0; map < 3; map++) {
        for (i32 i = 0;; i++) {
            u64 mark = arena_pos(a);
            const char *command;
            String8 text = app_dev_binding(t->app, a, map, i, &command);
            if (!text.len) break;
            total++;
            KeySeq seq;
            const char *error;
            if (!key_seq_parse(text, &seq, &error)) {
                it_fail(t, "binding '%S' does not parse back: %s", text, error);
                continue;
            }
            // In [keys] describe-key names the command; the other keymaps' bindings only need their keys.
            String8 want = map == 0 ? str8_fmt(a, "%S runs the command %s", text, command) : text;
            ItStroke here[KEY_SEQ_MAX], us[KEY_SEQ_MAX];
            b32 reachable = 1, us_ok = 1;
            for (i32 k = 0; k < seq.len; k++) {
                reachable = reachable && it_chord_stroke(t, t->hkl, t->altgr, seq.chords[k], &here[k]);
                us_ok = us_ok && it_chord_stroke(t, t->us, 0, seq.chords[k], &us[k]);
                if (us_ok && !us[k].ext) { // the same physical key on this layout
                    u32 scan = MapVirtualKeyExW(us[k].vk, MAPVK_VK_TO_VSC, t->us);
                    us[k].vk = MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK, t->hkl);
                    us_ok = us[k].vk != 0;
                }
            }
            if (!reachable) {
                unreachable++;
                LOG("test: input %s: unreachable: %S (%s): a character no key types", t->name, text, command);
                if (t->hkl == t->us) it_fail(t, "binding %S unreachable", text);
            } else {
                String8 got = it_describe(t, here, seq.len);
                b32 ok = map == 0 ? str8_equal(got, want) : str8_starts_with(got, str8_fmt(a, "%S ", want));
                if (!ok) {
                    wrong++;
                    it_fail(t, "binding %S (%s) with keys %S: echo '%S'", text, command, it_stroke_name(t, a, here[0]), got);
                }
            }
            if (t->hkl != t->us && us_ok) {
                String8 got = it_describe(t, us, seq.len);
                b32 same = map == 0 ? str8_equal(got, want) : str8_starts_with(got, str8_fmt(a, "%S ", want));
                if (!same) {
                    elsewhere++;
                    String8 keys = it_stroke_name(t, a, us[0]);
                    for (i32 k = 1; k < seq.len; k++) keys = str8_fmt(a, "%S %S", keys, it_stroke_name(t, a, us[k]));
                    LOG("test: input %s: US keys land elsewhere: %S (%s), US keys %S -> '%S'", t->name, text, command, keys,
                        got.len ? got : STR8_LIT("(no message)"));
                }
            }
            arena_pop_to(a, mark);
        }
    }
    LOG("test: input %s: %d bindings: %d unreachable, %d wrong through their own keys, %d where the US keys run something else",
        t->name, total, unreachable, wrong, elsewhere);
}

// A layout that has AltGr: some key types a character with Ctrl+Alt.
static b32 it_has_altgr(InputTest *t, HKL hkl) {
    for (u32 vk = 0x30; vk < 0xE3; vk++) {
        if (it_layout_char(t, hkl, vk, IT_RALT, NULL) || it_layout_char(t, hkl, vk, IT_RALT | IT_LSHIFT, NULL)) return 1;
    }
    return 0;
}

static void it_snapshot(HKL *list, i32 *n, HKL *foreground) {
    *n = GetKeyboardLayoutList(32, list);
    DWORD tid = GetWindowThreadProcessId(GetForegroundWindow(), NULL);
    *foreground = GetKeyboardLayout(tid);
}

typedef struct ItRun {
    Platform *p;
    i32 failures;
} ItRun;

static DWORD WINAPI it_thread(void *param) {
    ItRun *run = param;
    Platform *p = run->p;
    static const struct { const WCHAR *klid; const char *name; } layouts[] = {
        { L"00000409", "US" }, { L"00000809", "United Kingdom" }, { L"0000041F", "Turkish Q" }, { L"0000040B", "Finnish" },
    };
    HKL before[32], after[32], fg_before, fg_after;
    i32 n_before, n_after;
    it_snapshot(before, &n_before, &fg_before);
    HKL thread_layout = GetKeyboardLayout(0);

    WNDCLASSEXW wc = { .cbSize = sizeof(wc), .lpfnWndProc = win32_wndproc, .hInstance = p->instance, .lpszClassName = L"teal_input_test" };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowExW(0, L"teal_input_test", L"", WS_OVERLAPPEDWINDOW, 0, 0, 200, 100, NULL, NULL, p->instance, NULL);
    if (!hwnd) {
        LOG("test: FAIL: input: no window");
        run->failures++;
        return 0;
    }
    it_pump();
    p->event_count = 0;

    Arena arena = arena_create(GB(1));
    HKL loaded[ARRAY_COUNT(layouts)];
    i32 loaded_count = 0;
    HKL us = LoadKeyboardLayoutW(L"00000409", KLF_NOTELLSHELL);
    for (i32 l = 0; l < ARRAY_COUNT(layouts); l++) {
        HKL hkl = LoadKeyboardLayoutW(layouts[l].klid, KLF_NOTELLSHELL);
        if (!hkl) {
            LOG("test: skip: input %s: the layout cannot be loaded", layouts[l].name);
            continue;
        }
        b32 was_loaded = 0;
        for (i32 i = 0; i < n_before; i++) was_loaded |= before[i] == hkl;
        if (!was_loaded) loaded[loaded_count++] = hkl;
        if (!ActivateKeyboardLayout(hkl, 0) || GetKeyboardLayout(0) != hkl) {
            LOG("test: skip: input %s: the layout cannot be activated for this thread", layouts[l].name);
            continue;
        }
        InputTest t = { .p = p, .hwnd = hwnd, .hkl = hkl, .us = us, .name = layouts[l].name, .arena = arena };
        t.altgr = it_has_altgr(&t, hkl);
        AppArgs args = { .dpi_scale = 1.0f, .headless = 1 };
        t.app = app_create(&t.arena, &args);
        if (!t.app) {
            LOG("test: FAIL: input %s: app_create failed", t.name);
            run->failures++;
            continue;
        }
        app_dev_feed_events(t.app, NULL, 0, &t.arena);
        SetKeyboardState(t.state);
        i32 before_failures = t.failures;
        it_cases(&t);
        it_bindings(&t);

        // Alt + F4 last: its key is the one teal leaves to DefWindowProc, which makes it SC_CLOSE for an
        // active window (not for this one, never activated: SC_CLOSE is sent here); the unmodified app quits.
        i32 paths = p->dev_menu_paths;
        it_stroke(&t, (ItStroke){ IT_LALT, VK_F4, 0 });
        b32 to_default = p->dev_menu_paths == paths + 1;
        SendMessageW(hwnd, WM_SYSCOMMAND, SC_CLOSE, 0);
        it_flush(&t);
        if (!to_default || !t.quit) it_fail(&t, "Alt + F4: %d message(s) to DefWindowProc (1 expected), quit after SC_CLOSE %d", p->dev_menu_paths - paths, t.quit);

        i32 leaks = app_shutdown(t.app);
        if (leaks) it_fail(&t, "%d leak(s) at shutdown", leaks);
        if (t.failures == before_failures) {
            LOG("test: ok: input %s (layout 0x%08X, %s): letters, Alt / Ctrl / Ctrl+Alt chords, %s, Alt+Shift and Ctrl+Shift symbols, "
                "dead keys, no menu for Alt / F10 / Alt+Space, the keypad, every default binding, Alt+F4", t.name, (u64)(uintptr_t)hkl,
                t.altgr ? "AltGr" : "no AltGr", t.altgr ? "AltGr text and Left Alt + AltGr" : "Right Alt as Meta");
        }
        run->failures += t.failures;
        arena = t.arena;
        arena_reset(&arena);
        memset(t.state, 0, sizeof(t.state));
        SetKeyboardState(t.state);
    }

    ActivateKeyboardLayout(thread_layout, 0);
    for (i32 i = 0; i < loaded_count; i++) UnloadKeyboardLayout(loaded[i]);
    b32 us_new = 1;
    for (i32 i = 0; i < n_before; i++) us_new &= before[i] != us;
    if (us_new) UnloadKeyboardLayout(us);
    DestroyWindow(hwnd);
    UnregisterClassW(L"teal_input_test", p->instance);
    p->event_count = 0;

    it_snapshot(after, &n_after, &fg_after);
    b32 same = n_after == n_before && fg_after == fg_before;
    for (i32 i = 0; same && i < n_before; i++) same = before[i] == after[i];
    if (!same) {
        LOG("test: FAIL: input: the session's layouts changed: %d -> %d loaded, foreground 0x%08X -> 0x%08X", n_before, n_after,
            (u64)(uintptr_t)fg_before, (u64)(uintptr_t)fg_after);
        run->failures++;
    } else {
        LOG("test: ok: input: the session's %d layout(s) and the foreground layout 0x%08X are as before", n_before, (u64)(uintptr_t)fg_before);
    }
    return 0;
}

// --test: the real input path on its own thread (its keyboard layout is its own).
static i32 win32_dev_input_test(Platform *p) {
    ItRun run = { .p = p };
    HANDLE thread = CreateThread(NULL, 0, it_thread, &run, 0, NULL);
    if (!thread) {
        LOG("test: FAIL: input: no thread");
        return 1;
    }
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return run.failures;
}

#endif
