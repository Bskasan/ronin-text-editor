// edit.h — editing commands beyond the basic ones in view.c: undo and undo-redo (this phase
// adds the kill ring, indentation and the other editing commands). Plain C, headless; commands
// take a CommandContext like every other command.

#ifndef EDIT_H
#define EDIT_H

extern const Command CMD_UNDO, CMD_UNDO_REDO;

#endif // EDIT_H
