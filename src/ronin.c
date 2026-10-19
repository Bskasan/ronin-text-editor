// ronin.c — the unity translation unit. (src/win32_dwrite.cpp is compiled separately: dwrite.h is C++ only.)
// Headers first, then the editor core, then the backends that touch Win32 / D3D.

#include "base.h"
#include "version.h"
#include "font_backend.h" // implemented by win32_dwrite.cpp, the second translation unit
#include "platform.h"
#include "render.h"
#include "font.h"
#include "buffer.h"
#include "syntax.h"
#include "command.h"
#include "view.h"
#include "window.h"
#include "edit.h"
#include "keymap.h"
#include "theme.h"
#include "config.h"
#include "project.h"
#include "minibuffer.h"
#include "fuzzy.h"
#include "finder.h"
#include "search.h"
#include "grep.h"
#include "compile.h"
#include "git.h"
#include "isearch.h"
#include "results.h"
#include "cursors.h"
#include "modal.h"
#include "git_changes.h"
#include "terminal.h"

#include "base.c"
#include "png.c"
#include "font.c"
#include "buffer.c"
#include "syntax.c"
#include "lex_c.c"
#include "lex_jai.c"
#include "lex_cs.c"
#include "lex_js.c"
#include "view.c"
#include "window.c"
#include "edit.c"
#include "keymap.c"
// Cold code is compiled for size (optimize "s"): the config, the file commands, the finder's glue, the command
// table, isearch's and the multiple-cursor commands. Measured against /O2 for speed (Phase 15); the modules
// whose benches got slower that way (project.c's scanner, compile.c, results.c) stay at /O2.
#pragma optimize("s", on)
#include "config.c"
#include "theme.c"
#pragma optimize("", on)
#include "project.c"
#include "minibuffer.c"
#include "fuzzy.c"
#include "search.c"
#include "grep.c"
#include "compile.c"
#include "git.c"
#pragma optimize("s", on)
#include "isearch.c"
#pragma optimize("", on)
#include "app.c"
#pragma optimize("s", on)
#include "files.c"
#pragma optimize("", on)
#pragma optimize("s", on)
#include "finder.c"
#pragma optimize("", on)
#include "results.c"
#include "modal.c"
#include "commands_list.c"
#include "git_changes.c"
#pragma optimize("s", on)
#include "terminal.c"
#pragma optimize("", on)
#pragma optimize("s", on)
#include "cursors.c"
#pragma optimize("", on)
#pragma optimize("s", on)
#include "command.c"
#pragma optimize("", on)
#include "test.c" // RONIN_DEV only
#include "leak.c" // RONIN_DEV only: --leak-check

#include "render_d3d11.c"
#include "win32_main.c"
#include "win32_input_test.c" // RONIN_DEV only
#include "win32_child.c"      // RONIN_DEV only
