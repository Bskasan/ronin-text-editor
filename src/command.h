// command.h — every user action is a named command (Emacs names) in one table. Keys only ever
// map to commands (keymap.h). A command takes one context argument and nothing else.

#ifndef COMMAND_H
#define COMMAND_H

typedef struct View View;
typedef struct Cursor Cursor;
typedef struct Echo Echo;
typedef struct Settings Settings;
typedef struct KillRing KillRing;
typedef struct CommandContext CommandContext;
typedef void CommandFn(CommandContext *ctx);

// The command driver (view_run_command) applies the rules these flags name; commands never do.
enum {
    COMMAND_ONCE         = 1 << 0, // acts on the View as a whole: runs once, with the primary cursor
    COMMAND_MERGE_INSERT = 1 << 1, // undo: consecutive calls share a group (up to 20): self-insert
    COMMAND_MERGE_DELETE = 1 << 2, // undo: the same for single-character deletes
    COMMAND_MOTION       = 1 << 3, // shift-select: with Shift it activates the mark first; without, it ends a shift region
    COMMAND_EDIT         = 1 << 4, // changes text: the mark is deactivated afterwards
    COMMAND_REGION_DELETE  = 1 << 5, // with an active region it deletes the region instead (delete-active-region)
    COMMAND_REGION_REPLACE = 1 << 6, // with delete_selection_mode, an active region is deleted first
    COMMAND_KILL           = 1 << 7, // after another kill it appends to the same kill ring entry
    COMMAND_KILL_BACKWARD  = 1 << 8, // ... and prepends instead
};

typedef struct Command {
    const char *name; // the Emacs name
    CommandFn *fn;
    u32 flags;
} Command;

// A command is written for one cursor; view_run_command (view.c) is the single place that loops
// over the cursors and runs the work after a command.
struct CommandContext {
    App *app;                 // app-level commands (app.c); NULL in the headless tests
    View *view;
    Cursor *cursor;           // the cursor being processed
    Echo *echo;
    const Settings *settings; // the config's settings (config.h)
    KillRing *kills;          // the kill ring (edit.h)
    b32 kill_append;          // set by the driver: this command's kills append to the newest entry
    u32 codepoint;            // self-insert: the character of the key
    b32 shift_translated;     // the key had Shift and was found without it (shift-select, Phase 6)
    const Command *this_command;
    const Command *last_command;
};

const Command *command_find(String8 name); // NULL if there is no such command
i32            command_count(void);
const Command *command_at(i32 index);

#endif // COMMAND_H
