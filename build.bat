@echo off
rem teal build.  "build.bat" -> build\teal_debug.exe   "build.bat release" -> build\teal.exe
rem From Git Bash: cmd //c build.bat [release]
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

set MODE=debug
if /i "%~1"=="release" set MODE=release

rem --- toolchain -------------------------------------------------------------
where cl >nul 2>nul
if not errorlevel 1 goto have_cl
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" goto no_msvc
set "VSDIR="
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR goto no_msvc
if not exist "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" goto no_msvc
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "%ROOT%"
where cl >nul 2>nul
if errorlevel 1 goto no_msvc
:have_cl
where fxc >nul 2>nul
if errorlevel 1 (
    echo ERROR: fxc.exe not found. Install the Windows 10/11 SDK with Visual Studio.
    exit /b 1
)

if not exist build\gen mkdir build\gen

rem --- shaders -> C headers --------------------------------------------------
fxc /nologo /WX /O3 /T vs_4_0 /E vs_main /Vn quad_vs_bytes /Fh build\gen\quad_vs.h src\shaders\quad.hlsl >nul
if errorlevel 1 exit /b 1
fxc /nologo /WX /O3 /T ps_4_0 /E ps_main /Vn quad_ps_bytes /Fh build\gen\quad_ps.h src\shaders\quad.hlsl >nul
if errorlevel 1 exit /b 1

rem --- C ---------------------------------------------------------------------
set CFLAGS=/nologo /std:c11 /utf-8 /W4 /WX /FC /external:anglebrackets /external:W0 /Ibuild\gen
set LFLAGS=/SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:res\teal.manifest
set LIBS=kernel32.lib user32.lib d3d11.lib dxgi.lib dwmapi.lib dxguid.lib

if "%MODE%"=="release" (
    set OUT=build\teal.exe
    cl %CFLAGS% /O2 /GL /Gw /Gy /GS- /MT /DTEAL_DEV=0 src\teal.c /Fobuild\teal.obj /Febuild\teal.exe /link %LFLAGS% /LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO %LIBS%
) else (
    set OUT=build\teal_debug.exe
    cl %CFLAGS% /Od /Zi /MTd /DTEAL_DEV=1 src\teal.c /Fobuild\teal_debug.obj /Fdbuild\teal_debug_cl.pdb /Febuild\teal_debug.exe /link %LFLAGS% /DEBUG /INCREMENTAL:NO %LIBS%
)
if errorlevel 1 exit /b 1

for %%F in ("%OUT%") do echo %OUT%: %%~zF bytes
exit /b 0

:no_msvc
echo ERROR: MSVC (cl.exe, x64) not found.
echo Install Visual Studio 2022 or later, or the Build Tools for Visual Studio, with the
echo "Desktop development with C++" workload (MSVC x64/x86 build tools + Windows SDK).
exit /b 1
