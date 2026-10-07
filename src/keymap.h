// keymap.h — keys as Emacs sees them: chords, key sequences in kbd notation ("C-x C-s"),
// keymaps and the key sequence state machine. Plain C, headless.
//
// Keys match by the character they produce, not by their position: C-/ is Ctrl plus whatever
// produces "/" in the current layout. Letters keep Shift as a modifier (C-S-a); every other
// character absorbs Shift and AltGr (C-/ stays C-/ where "/" is Shift+7).

#ifndef KEYMAP_H
#define KEYMAP_H

// A chord: modifiers plus either a named key (a Key) or a character (a codepoint), packed so that
// chords compare as integers.
typedef u32 KeyChord;
#define CHORD_CODE_MASK 0x1FFFFFu // the codepoint, or the Key when CHORD_NAMED
#define CHORD_NAMED     (1u << 21)
#define CHORD_CTRL      (1u << 24)
#define CHORD_META      (1u << 25)
#define CHORD_SHIFT     (1u << 26)

#define KEY_SEQ_MAX 4        // chords in a key sequence
#define KEY_SEQ_TEXT_CAP 128 // bytes enough for any printed key sequence

typedef struct KeySeq {
    KeyChord chords[KEY_SEQ_MAX];
    i32 len;
} KeySeq;

u32      key_letter_lower(u32 codepoint); // the lowercase form of a cased letter (either case), 0 otherwise
b32      key_is_named(Key key);           // not a character key: function keys, arrows, RET, TAB, ESC, DEL, ...

// The chord of a KEY_DOWN. Named keys keep every modifier. A character key is a chord only with
// Ctrl or Meta (otherwise its text event follows and is the chord); letters are lowercased and
// keep Shift, other characters absorb it. False: no chord.
b32      key_chord_from_event(Key key, u32 codepoint, u32 mods, KeyChord *out);
KeyChord key_chord_from_text(u32 codepoint);

// kbd notation: chords separated by spaces; C- M- S- prefixes; RET TAB ESC SPC DEL; <delete>
// <insert> <left> <right> <up> <down> <home> <end> <prior> <next> (<pageup> <pagedown>), <f1>..<f24>,
// <pause> <apps>; otherwise exactly one character. A modified uppercase letter means S- plus the
// lowercase one ("C-A" = "C-S-a"). False with a reason in *error on bad input.
b32 key_chord_parse(String8 token, KeyChord *out, const char **error);
b32 key_seq_parse(String8 text, KeySeq *out, const char **error);
i64 key_chord_print(KeyChord chord, u8 *out, i64 cap); // canonical (C-M-S- order); returns the length
i64 key_seq_print(KeySeq *seq, u8 *out, i64 cap);
b32 key_seq_equal(KeySeq *a, KeySeq *b);

// ---------------------------------------------------------------------------
// Keymaps: named, a flat array of bindings searched linearly (a few dozen entries).

#define KEYMAP_CAP 512

typedef struct KeyBinding {
    KeySeq seq;
    const Command *command;
} KeyBinding;

typedef struct Keymap {
    const char *name; // "global"; context maps (minibuffer, isearch) come in Phases 7 and 9
    KeyBinding bindings[KEYMAP_CAP];
    i32 count;
} Keymap;

// Binds `seq` (replacing its old binding); command NULL removes it. Older bindings that conflict,
// one being a proper prefix of the other, are removed: the later binding wins, as in Emacs. Returns
// how many were removed that way, or -1 when the keymap is full.
i32            keymap_bind(Keymap *map, KeySeq *seq, const Command *command);
const Command *keymap_get(Keymap *map, KeySeq *seq);        // exact match, or NULL
b32            keymap_has_prefix(Keymap *map, KeySeq *seq); // seq is a proper prefix of some binding

// ---------------------------------------------------------------------------
// The key sequence state machine. Keymaps are searched as a stack (context maps first, then
// "global") with prefixes merged across maps, as in Emacs: the first map with an exact match for
// the sequence runs it; otherwise the sequence waits if it is a proper prefix in any map;
// otherwise it is undefined. A chord with Shift and no binding is looked up again without Shift
// (Emacs' shift-translation). No allocation.

typedef enum KeyResultKind {
    KEY_RESULT_IGNORED,     // not for the keymap: a plain character key (its text event follows), mouse, ...
    KEY_RESULT_DROPPED,     // text produced by a KEY_DOWN that was consumed
    KEY_RESULT_PREFIX,      // a proper prefix: waiting for more (seq so far)
    KEY_RESULT_COMMAND,     // run `command`
    KEY_RESULT_SELF_INSERT, // an unbound plain character outside a prefix: self-insert-command
    KEY_RESULT_UNDEFINED,   // "<seq> is undefined"; the state is reset
    KEY_RESULT_QUIT,        // keyboard-quit pressed while a prefix was pending: cancelled
    KEY_RESULT_DESCRIBE,    // describe-key: `command` is what seq runs (NULL = undefined); not run
} KeyResultKind;

typedef struct KeyResult {
    KeyResultKind kind;
    const Command *command;
    KeySeq seq;           // as typed (before shift-translation)
    u32 codepoint;        // the character of the last chord (TAB gives a tab), 0 for other named keys
    b32 shift_translated; // the binding was found by dropping Shift from the last chord
} KeyResult;

typedef struct KeyInput {
    KeySeq pending; // the proper prefix typed so far
    b32 drop_text;  // the last KEY_DOWN was consumed: drop text events until the next KEY_DOWN
    b32 describe;   // describe-key: describe the next complete sequence instead of running it
} KeyInput;

void key_input_feed(KeyInput *in, Keymap **stack, i32 count, Event *e, KeyResult *out);

#if TEAL_DEV
// --keys: the events one token produces, as the platform would deliver them. A token in kbd
// notation is a chord (a KEY_DOWN); a single character (or SPC) is typed: a KEY_DOWN without
// Ctrl / Alt, then its text event. Returns the event count (0 for a bad token, *error set).
i32 key_dev_events(String8 token, Event out[2], const char **error);
#endif

#endif // KEYMAP_H
