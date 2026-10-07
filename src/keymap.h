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

#endif // KEYMAP_H
