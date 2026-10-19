/* ======================================================================
   ronin: the unity build: every header, then every .c file, in order

   Copyright (c) 2026 Bekir Kasan. Licensed under the MIT License; see
   LICENSE in the repository root. This software is provided "as is",
   without warranty of any kind, express or implied.
   https://github.com/Bskasan/ronin-text-editor
   ====================================================================== */

// the unity translation unit. (src/platform/win32_dwrite.cpp is compiled separately: dwrite.h is C++ only.)
// Headers first, then the editor core, then the backends that touch Win32 / D3D.

#include "base/base.h"
#include "app/version.h"
#include "platform/font_backend.h" // implemented by win32_dwrite.cpp, the second translation unit
#include "platform/platform.h"
#include "platform/render.h"
#include "editor/font.h"
#include "text/buffer.h"
#include "text/syntax.h"
#include "editor/command.h"
#include "editor/view.h"
#include "editor/window.h"
#include "editor/edit.h"
#include "editor/keymap.h"
#include "editor/theme.h"
#include "editor/config.h"
#include "project/project.h"
#include "editor/minibuffer.h"
#include "project/fuzzy.h"
#include "project/finder.h"
#include "text/search.h"
#include "project/grep.h"
#include "project/compile.h"
#include "project/git.h"
#include "editor/isearch.h"
#include "project/results.h"
#include "editor/cursors.h"
#include "editor/modal.h"
#include "project/git_changes.h"
#include "project/terminal.h"

#include "base/base.c"
#include "dev/png.c"
#include "editor/font.c"
#include "text/buffer.c"
#include "text/syntax.c"
#include "text/lex_c.c"
#include "text/lex_jai.c"
#include "text/lex_cs.c"
#include "text/lex_js.c"
#include "editor/view.c"
#include "editor/window.c"
#include "editor/edit.c"
#include "editor/keymap.c"
// Cold code is compiled for size (optimize "s"): the config, the file commands, the finder's glue, the command
// table, isearch's and the multiple-cursor commands. Measured against /O2 for speed (Phase 15); the modules
// whose benches got slower that way (project.c's scanner, compile.c, results.c) stay at /O2.
#pragma optimize("s", on)
#include "editor/config.c"
#include "editor/theme.c"
#pragma optimize("", on)
#include "project/project.c"
#include "editor/minibuffer.c"
#include "project/fuzzy.c"
#include "text/search.c"
#include "project/grep.c"
#include "project/compile.c"
#include "project/git.c"
#pragma optimize("s", on)
#include "editor/isearch.c"
#pragma optimize("", on)
#include "app/app.c"
#pragma optimize("s", on)
#include "editor/files.c"
#pragma optimize("", on)
#pragma optimize("s", on)
#include "project/finder.c"
#pragma optimize("", on)
#include "project/results.c"
#include "editor/modal.c"
#include "editor/commands_list.c"
#include "project/git_changes.c"
#pragma optimize("s", on)
#include "project/terminal.c"
#pragma optimize("", on)
#pragma optimize("s", on)
#include "editor/cursors.c"
#pragma optimize("", on)
#pragma optimize("s", on)
#include "editor/command.c"
#pragma optimize("", on)
#include "dev/test.c" // RONIN_DEV only
#include "dev/leak.c" // RONIN_DEV only: --leak-check

#include "platform/render_d3d11.c"
#include "platform/win32_main.c"
#include "platform/win32_input_test.c" // RONIN_DEV only
#include "platform/win32_child.c"      // RONIN_DEV only
