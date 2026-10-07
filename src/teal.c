// teal.c — the unity translation unit. (src/win32_dwrite.cpp is compiled separately: dwrite.h is C++ only.)
// Headers first, then the editor core, then the backends that touch Win32 / D3D.

#include "base.h"
#include "font_backend.h" // implemented by win32_dwrite.cpp, the second translation unit
#include "platform.h"
#include "render.h"
#include "font.h"
#include "buffer.h"
#include "view.h"

#include "base.c"
#include "png.c"
#include "font.c"
#include "buffer.c"
#include "view.c"
#include "app.c"
#include "test.c" // TEAL_DEV only

#include "render_d3d11.c"
#include "win32_main.c"
