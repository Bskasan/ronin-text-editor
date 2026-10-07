// command.h — every user action is a named command (Emacs names) in one table. Keys only ever
// map to commands (keymap.h). A command takes one context argument and nothing else.

#ifndef COMMAND_H
#define COMMAND_H

typedef struct View View;
typedef struct Cursor Cursor;
typedef struct Echo Echo;
typedef struct Settings Settings;
typedef struct CommandContext CommandContext;
typedef void CommandFn(CommandContext *ctx);

enum {
    COMMAND_ONCE = 1 << 0, // acts on the View as a whole: runs once, with the primary cursor
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
    u32 codepoint;            // self-insert: the character of the key
    b32 shift_translated;     // the key had Shift and was found without it (shift-select, Phase 6)
    const Command *this_command;
    const Command *last_command;
};

const Command *command_find(String8 name); // NULL if there is no such command
i32            command_count(void);
const Command *command_at(i32 index);

#endif // COMMAND_H
