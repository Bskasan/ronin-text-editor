// terminal.c — see terminal.h. The command lines (W wt.exe's full path, C the system's cmd.exe, <dir> quoted):
//
//                      terminal = wt (or auto with wt.exe on PATH)       terminal = cmd (or auto without it)
//   open-terminal      "W" -d "<dir>"                                    "C"                    (a new console)
//   open-ai-terminal   "W" -d "<dir>" "C" /s /k "<command>"              "C" /s /k "<command>"  (a new console)
//
// each started in <dir>. cmd.exe /k runs the command (npm's claude.cmd resolves there) and keeps the window open after
// it; /s makes cmd strip exactly the outer quotes, so a quoted program path in the command survives.

// One argument quoted as CommandLineToArgvW reads it back: backslashes before a quote, or before the closing one,
// doubled ("C:\" is "C:\\"), a quote escaped.
static String8 terminal_quote_arg(Arena *arena, String8 s) {
    u8 *out = PUSH_ARRAY(arena, u8, s.len * 2 + 3);
    i64 n = 0, slashes = 0;
    out[n++] = '"';
    for (i64 i = 0; i < s.len; i++) {
        u8 c = s.data[i];
        if (c == '\\') {
            slashes++;
            out[n++] = c;
            continue;
        }
        if (c == '"') {
            for (i64 k = 0; k < slashes + 1; k++) out[n++] = '\\';
        }
        slashes = 0;
        out[n++] = c;
    }
    for (i64 k = 0; k < slashes; k++) out[n++] = '\\';
    out[n++] = '"';
    return str8(out, n);
}

// wt splits its command line into tabs at ';': one meant as text is written "\;".
static String8 terminal_wt_escape(Arena *arena, String8 s) {
    u8 *out = PUSH_ARRAY(arena, u8, s.len * 2);
    i64 n = 0;
    for (i64 i = 0; i < s.len; i++) {
        if (s.data[i] == ';') out[n++] = '\\';
        out[n++] = s.data[i];
    }
    return str8(out, n);
}

// wt.exe on PATH (the App Execution Alias in WindowsApps counts).
static b32 terminal_find_wt(App *app, Arena *arena, String8 *path) {
#if RONIN_DEV
    if (app->terminal.dev_wt_present) {
        *path = app->terminal.dev_wt_present == 1 ? STR8_LIT("wt.exe") : str8(NULL, 0);
        return app->terminal.dev_wt_present == 1;
    }
#else
    (void)app;
#endif
    return os_find_program(arena, STR8_LIT("wt.exe"), path);
}

static void terminal_open(CommandContext *ctx, b32 ai) {
    App *app = ctx->app;
    if (!app) {
        echo_message(ctx->echo, "No terminal here");
        return;
    }
    if (app->mini.active) {
        echo_message(ctx->echo, "A terminal cannot be opened from the minibuffer");
        return;
    }
    Arena *a = ctx->scratch;
    Settings *s = &app->config->settings;
    String8 command = { 0 };
    if (ai) {
        command = str8(s->ai_command, s->ai_command_len);
        if (!command.len && s->ai_provider == AI_PROVIDER_CUSTOM) {
            echo_message(ctx->echo, "ai_command is empty");
            return;
        }
        if (!command.len) command = s->ai_provider == AI_PROVIDER_OPENAI ? STR8_LIT("codex") : STR8_LIT("claude");
    }
    // The directory: the project's root (the AI's only), else the file's directory, else the default one.
    Buffer *buf = ctx->view->buffer;
    String8 dir = ai ? finder_root(app, buf) : str8(NULL, 0);
    if (!dir.len && buf->path.len) dir = str8(buf->path.data, buf->path.len - config_file_name(buf->path).len);
    if (!dir.len) dir = app->finder.cmdline_dir.len ? app->finder.cmdline_dir : os_full_path(a, STR8_LIT("."));
    dir = results_compile_dir(a, dir);
    String8 wt = { 0 };
    b32 use_wt = 0;
    if (s->terminal_program != TERMINAL_PROGRAM_CMD) {
        use_wt = terminal_find_wt(app, a, &wt);
        if (!use_wt && s->terminal_program == TERMINAL_PROGRAM_WT) {
            echo_message(ctx->echo, "Windows Terminal (wt.exe) not found");
            return;
        }
    }
    String8 cmd = os_system_program(a, STR8_LIT("cmd.exe"));
    String8 program, args;
    b32 console;
    if (use_wt) {
        program = wt;
        args = str8_fmt(a, "-d %S", terminal_quote_arg(a, dir));
        if (ai) args = str8_fmt(a, "%S %S /s /k \"%S\"", args, terminal_quote_arg(a, cmd), command);
        args = terminal_wt_escape(a, args);
        console = 0; // wt.exe opens its own window
    } else {
        program = cmd;
        args = ai ? str8_fmt(a, "/s /k \"%S\"", command) : str8(NULL, 0);
        console = 1;
    }
#if RONIN_DEV
    // Tests start the fake terminal, told what would have run; a batch run never starts a real one.
    if (app->terminal.dev_program.len) {
        args = str8_fmt(a, "%S %S %d %S", app->terminal.dev_prefix, config_file_name(program), console, args);
        program = app->terminal.dev_program;
        console = 0;
    } else if (os_dev_batch_run()) {
        app->dev_real_refused++;
        echo_message(ctx->echo, "terminal: no real terminal in a test run");
        return;
    }
#endif
    u32 error = os_process_launch(a, program, args, dir, console);
    if (error) {
        files_echo(ctx->echo, "Could not start %S: error %u", program, error);
        return;
    }
#if RONIN_DEV
    app->terminal.dev_launches++;
#endif
    String8 shown = str8_copy(a, dir);
    for (i64 i = 0; i < shown.len; i++) if (shown.data[i] == '\\') shown.data[i] = '/';
    if (ai) echo_message(ctx->echo, "Opened %S in %S", command, shown);
    else echo_message(ctx->echo, "Opened terminal in %S", shown);
}

static void cmd_open_terminal(CommandContext *ctx) {
    terminal_open(ctx, 0);
}

static void cmd_open_ai_terminal(CommandContext *ctx) {
    terminal_open(ctx, 1);
}

const Command CMD_OPEN_TERMINAL = { "open-terminal", "Open a terminal in the file's directory", cmd_open_terminal, COMMAND_ONCE };
const Command CMD_OPEN_AI_TERMINAL = { "open-ai-terminal", "Open the AI assistant's CLI in a terminal", cmd_open_ai_terminal, COMMAND_ONCE };
