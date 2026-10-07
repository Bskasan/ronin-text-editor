@echo off
rem teal build.  "build.bat" -> build\teal_debug.exe   "build.bat release" -> build\teal.exe
rem From Git Bash: cmd //c ".\build.bat" [release]
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

set MODE=debug
if /i "%~1"=="release" set MODE=release
if /i "%~1"=="bench" set MODE=bench

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

rem --- C (unity build) + the one C++ file (DirectWrite: dwrite.h is C++ only) ---
set COMMON=/nologo /utf-8 /W4 /WX /FC /external:anglebrackets /external:W0
set CFLAGS=%COMMON% /std:c11 /Ibuild\gen
rem C-style C++: no exceptions (no /EH), no RTTI.
set CPPFLAGS=%COMMON% /GR- /c
set LFLAGS=/SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:res\teal.manifest
set LIBS=kernel32.lib user32.lib gdi32.lib d3d11.lib dxgi.lib dwmapi.lib dwrite.lib dxguid.lib

if "%MODE%"=="release" (
    set NAME=teal
    set OPT=/O2 /GL /Gw /Gy /GS- /MT /DTEAL_DEV=0
    set LINKOPT=/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO
) else if "%MODE%"=="bench" (
    set NAME=teal_bench
    set OPT=/O2 /Zi /MT /DTEAL_DEV=1 /DTEAL_D3D_DEBUG=0
    set LINKOPT=/DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO
) else (
    set NAME=teal_debug
    set OPT=/Od /Zi /MTd /DTEAL_DEV=1
    set LINKOPT=/DEBUG /INCREMENTAL:NO
)
cl %CPPFLAGS% %OPT% src\win32_dwrite.cpp /Fobuild\%NAME%_dwrite.obj /Fdbuild\%NAME%_cl.pdb
if errorlevel 1 exit /b 1
cl %CFLAGS% %OPT% src\teal.c build\%NAME%_dwrite.obj /Fobuild\%NAME%.obj /Fdbuild\%NAME%_cl.pdb /Febuild\%NAME%.exe /link %LFLAGS% %LINKOPT% %LIBS%
if errorlevel 1 exit /b 1

for %%F in ("build\%NAME%.exe") do echo build\%NAME%.exe: %%~zF bytes
exit /b 0

:no_msvc
echo ERROR: MSVC (cl.exe, x64) not found.
echo Install Visual Studio 2022 or later, or the Build Tools for Visual Studio, with the
echo "Desktop development with C++" workload (MSVC x64/x86 build tools + Windows SDK).
exit /b 1
