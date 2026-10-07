// teal.c — the single translation unit (unity build).
// Headers first, then the editor core, then the backends that touch Win32 / D3D.

#include "base.h"
#include "platform.h"
#include "render.h"

#include "base.c"
#include "png.c"
#include "app.c"

#include "render_d3d11.c"
#include "win32_main.c"
