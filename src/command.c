// command.c — see command.h. The table lists every command; lookup is by name (a linear scan,
// only done while parsing the config, never per key).

static const Command *const command_table[] = {
    &CMD_FORWARD_CHAR, &CMD_BACKWARD_CHAR, &CMD_NEXT_LINE, &CMD_PREVIOUS_LINE,
    &CMD_MOVE_BEGINNING_OF_LINE, &CMD_MOVE_END_OF_LINE, &CMD_FORWARD_WORD, &CMD_BACKWARD_WORD,
    &CMD_FORWARD_PARAGRAPH, &CMD_BACKWARD_PARAGRAPH, &CMD_BEGINNING_OF_BUFFER, &CMD_END_OF_BUFFER,
    &CMD_SCROLL_UP_COMMAND, &CMD_SCROLL_DOWN_COMMAND, &CMD_RECENTER_TOP_BOTTOM,
    &CMD_SELF_INSERT, &CMD_NEWLINE, &CMD_DELETE_BACKWARD_CHAR, &CMD_DELETE_CHAR, &CMD_SAVE_BUFFER,
};

const Command *command_find(String8 name) {
    for (i32 i = 0; i < ARRAY_COUNT(command_table); i++) {
        if (str8_equal(name, str8_cstr(command_table[i]->name))) return command_table[i];
    }
    return NULL;
}

i32 command_count(void) {
    return (i32)ARRAY_COUNT(command_table);
}

const Command *command_at(i32 index) {
    return index >= 0 && index < ARRAY_COUNT(command_table) ? command_table[index] : NULL;
}
