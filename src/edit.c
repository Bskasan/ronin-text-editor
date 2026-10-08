// edit.c — see edit.h.

// ---------------------------------------------------------------------------
// Undo

// Puts the primary point where the undone change was (the point before that command).
static void edit_undo_report(CommandContext *ctx, BufferUndoResult r, i64 point, const char *done, const char *nothing) {
    View *v = ctx->view;
    switch (r) {
    case BUFFER_UNDO_DONE:
        view_set_point(v, &v->cursors[0], point);
        echo_message(ctx->echo, "%s", done);
        break;
    case BUFFER_UNDO_NOTHING:
        echo_message(ctx->echo, "%s", nothing);
        break;
    case BUFFER_UNDO_FAILED:
        if (v->buffer->read_only) echo_message(ctx->echo, "Buffer is read-only: %S", v->buffer->name);
        else echo_message(ctx->echo, "Cannot undo: the buffer is full");
        break;
    }
}

// Emacs: consecutive undos walk back through history; after any other command (undo-redo
// included) undo first undoes the undos.
static void cmd_undo(CommandContext *ctx) {
    View *v = ctx->view;
    i64 point;
    BufferUndoResult r = buffer_undo(v->buffer, ctx->last_command == &CMD_UNDO, view_point(v, &v->cursors[0]), &point);
    edit_undo_report(ctx, r, point, "Undo", "No further undo information");
}

static void cmd_undo_redo(CommandContext *ctx) {
    View *v = ctx->view;
    i64 point;
    BufferUndoResult r = buffer_redo(v->buffer, view_point(v, &v->cursors[0]), &point);
    edit_undo_report(ctx, r, point, "Redo", "No further redo information");
}

const Command CMD_UNDO      = { "undo", cmd_undo, COMMAND_ONCE | COMMAND_EDIT };
const Command CMD_UNDO_REDO = { "undo-redo", cmd_undo_redo, COMMAND_ONCE | COMMAND_EDIT };
