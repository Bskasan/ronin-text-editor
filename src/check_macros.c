// check_macros.c — compiled by build.bat with /Zs (syntax only, no output), never linked.
// Every system header teal uses comes first, then the whole unity build: a teal identifier that a
// system header defines as a macro (winuser.h's MOD_ALT, rpcndr.h's small, minwindef.h's near) is
// replaced before the compiler sees it and fails the build, and a teal macro that redefines a
// system one is a C4005 warning in our own code (an error with /WX), where /external:W0 cannot
// hide it. The defines are the ones win32_main.c and render_d3d11.c use.

#define COBJMACROS
#define UNICODE
#define _UNICODE
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define PSAPI_VERSION 2
#include <windows.h>
#include <dwmapi.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_3.h>
#include <dxgidebug.h>
#include <emmintrin.h>
#include <intrin.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <string.h>

#include "teal.c"
