// terminal.h — open-terminal (C-c t) and open-ai-terminal (C-c a), Phase 20: a terminal window in the file's directory,
// or in the project's root running an AI assistant's command line tool. Fire and forget: ronin starts it
// (os_process_launch) and keeps nothing of it; the terminal outlives ronin. Part of the app: terminal.c is included
// after git_changes.c.

#ifndef TERMINAL_H
#define TERMINAL_H

#if RONIN_DEV
typedef struct Terminal {
    String8 dev_program, dev_prefix; // tests: the fake terminal (--fake-terminal DIR) started instead of the real one
    i32 dev_wt_present;              // tests: 0 the real PATH lookup of wt.exe, 1 found, 2 not found
    u64 dev_launches;                // terminals started
} Terminal;
#endif

extern const Command CMD_OPEN_TERMINAL, CMD_OPEN_AI_TERMINAL;

#endif // TERMINAL_H
