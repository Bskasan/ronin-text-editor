@echo off
rem ronin build.  "build.bat" -> build\ronin_debug.exe   "build.bat release" -> build\ronin.exe
rem "build.bat clean" removes build\ and dist\ (everything generated) and nothing else.
rem "build.bat docs" -> the debug build, then the documentation site in docs\ (generated, committed).
rem From Git Bash: cmd //c ".\build.bat" [release|bench|docs|clean]
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"
if /i "%~1"=="clean" goto clean

set MODE=debug
set DOCS=0
if /i "%~1"=="release" set MODE=release
if /i "%~1"=="bench" set MODE=bench
if /i "%~1"=="docs" set DOCS=1

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
if not exist build\tmp mkdir build\tmp
if not exist build\shots mkdir build\shots

rem --- shaders -> C headers --------------------------------------------------
fxc /nologo /WX /O3 /T vs_4_0 /E vs_main /Vn quad_vs_bytes /Fh build\gen\quad_vs.h src\platform\shaders\quad.hlsl >nul
if errorlevel 1 exit /b 1
fxc /nologo /WX /O3 /T ps_4_0 /E ps_main /Vn quad_ps_bytes /Fh build\gen\quad_ps.h src\platform\shaders\quad.hlsl >nul
if errorlevel 1 exit /b 1

rem --- the icon (drawn by tools\make_icon.c, never committed) and the version resource ---
cl /nologo /W4 /WX /O2 tools\make_icon.c /Fobuild\make_icon.obj /Fdbuild\make_icon_cl.pdb /Febuild\make_icon.exe /link /INCREMENTAL:NO >nul
if errorlevel 1 (
    echo ERROR: tools\make_icon.c did not compile
    exit /b 1
)
build\make_icon.exe build\gen\ronin.ico
if errorlevel 1 (
    echo ERROR: make_icon could not write build\gen\ronin.ico
    exit /b 1
)
rc /nologo /Isrc\app /fo build\gen\ronin.res res\ronin.rc
if errorlevel 1 exit /b 1

rem --- the built-in themes: themes\*.conf embedded by tools\make_themes.c into build\gen\themes.h ---
cl /nologo /W4 /WX /O2 tools\make_themes.c /Fobuild\make_themes.obj /Fdbuild\make_themes_cl.pdb /Febuild\make_themes.exe /link /INCREMENTAL:NO >nul
if errorlevel 1 (
    echo ERROR: tools\make_themes.c did not compile
    exit /b 1
)
build\make_themes.exe themes build\gen\themes.h
if errorlevel 1 (
    echo ERROR: make_themes could not embed themes\*.conf
    exit /b 1
)

rem --- the source check (tools\check_source.c): compiled here, run by verify.bat and the CI workflow ---
cl /nologo /W4 /WX /O2 tools\check_source.c /Fobuild\check_source.obj /Fdbuild\check_source_cl.pdb /Febuild\check_source.exe /link /INCREMENTAL:NO >nul
if errorlevel 1 (
    echo ERROR: tools\check_source.c did not compile
    exit /b 1
)

rem --- the helper of release-check.bat (tools\release_check.c, #30): compiled here, run only by release-check.bat ---
cl /nologo /W4 /WX /O2 tools\release_check.c /Fobuild\release_check.obj /Fdbuild\release_check_cl.pdb /Febuild\release_check.exe /link /INCREMENTAL:NO user32.lib version.lib >nul
if errorlevel 1 (
    echo ERROR: tools\release_check.c did not compile
    exit /b 1
)

rem --- the documentation site's generator (tools\make_docs.c, with src\dev\markdown.c): run by build.bat docs and verify ---
cl /nologo /W4 /WX /O2 /std:c11 tools\make_docs.c /Fobuild\make_docs.obj /Fdbuild\make_docs_cl.pdb /Febuild\make_docs.exe /link /INCREMENTAL:NO >nul
if errorlevel 1 (
    echo ERROR: tools\make_docs.c did not compile
    exit /b 1
)

rem --- C (unity build) + the one C++ file (DirectWrite: dwrite.h is C++ only) ---
set COMMON=/nologo /utf-8 /W4 /WX /FC /external:anglebrackets /external:W0
set CFLAGS=%COMMON% /std:c11 /Ibuild\gen
rem C-style C++: no exceptions (no /EH), no RTTI.
set CPPFLAGS=%COMMON% /GR- /c
set LFLAGS=/SUBSYSTEM:WINDOWS /MANIFEST:EMBED /MANIFESTINPUT:res\ronin.manifest
set LIBS=kernel32.lib user32.lib gdi32.lib dxgi.lib dxguid.lib

if "%MODE%"=="release" (
    set NAME=ronin
    set OPT=/O2 /GL /Gw /Gy /GS- /MT
    set DEFS=/DRONIN_DEV=0 /DRONIN_NO_CRT=1
    set LINKOPT=/LTCG /OPT:REF /OPT:ICF /INCREMENTAL:NO /MAP:build\ronin.map /NODEFAULTLIB /ENTRY:win32_entry libcmt.lib libvcruntime.lib
) else if "%MODE%"=="bench" (
    set NAME=ronin_bench
    set OPT=/O2 /Zi /GS- /MT
    set DEFS=/DRONIN_DEV=1 /DRONIN_D3D_DEBUG=0 /DRONIN_NO_CRT=1
    set LINKOPT=/DEBUG /OPT:REF /OPT:ICF /INCREMENTAL:NO /NODEFAULTLIB /ENTRY:win32_entry libcmt.lib libvcruntime.lib
) else (
    set NAME=ronin_debug
    set OPT=/Od /Zi /MTd
    set DEFS=/DRONIN_DEV=1
    set LINKOPT=/DEBUG /INCREMENTAL:NO
)
rem No ronin identifier may collide with a system macro: every system header first, then the whole
rem unity build, syntax only (src\platform\check_macros.c says why).
cl %CFLAGS% %DEFS% /Zs src\platform\check_macros.c
if errorlevel 1 exit /b 1
cl %CPPFLAGS% %OPT% %DEFS% src\platform\win32_dwrite.cpp /Fobuild\%NAME%_dwrite.obj /Fdbuild\%NAME%_cl.pdb
if errorlevel 1 exit /b 1
cl %CFLAGS% %OPT% %DEFS% src\ronin.c build\%NAME%_dwrite.obj build\gen\ronin.res /Fobuild\%NAME%.obj /Fdbuild\%NAME%_cl.pdb /Febuild\%NAME%.exe /link %LFLAGS% %LINKOPT% %LIBS%
if errorlevel 1 exit /b 1

for %%F in ("build\%NAME%.exe") do echo build\%NAME%.exe: %%~zF bytes
if "%DOCS%"=="1" goto docs
exit /b 0

rem --- docs: the documentation site (Phase 24), after the debug build: the fragments from the exe, the logo, then
rem every page of docs\src\nav.txt into docs\*.html. The output is committed (GitHub Pages serves docs\ as files);
rem verify.bat's docs-check fails when it differs from what its sources make.
:docs
start "" /wait build\ronin_debug.exe --dump-docs docs\src\gen
if errorlevel 1 (
    echo ERROR: ronin_debug.exe --dump-docs failed: see build\ronin.log
    exit /b 1
)
build\make_icon.exe --png docs\images\logo.png
if errorlevel 1 (
    echo ERROR: make_icon could not write docs\images\logo.png
    exit /b 1
)
build\make_docs.exe docs docs\src\gen docs
if errorlevel 1 (
    echo ERROR: make_docs: the site was not generated
    exit /b 1
)
echo docs: the site generated in docs\
exit /b 0

:no_msvc
echo ERROR: MSVC (cl.exe, x64) not found.
echo Install Visual Studio 2022 or later, or the Build Tools for Visual Studio, with the
echo "Desktop development with C++" workload (MSVC x64/x86 build tools + Windows SDK).
exit /b 1

rem --- clean: build\ and dist\ entirely, nothing else (no partial clean) ------------------------------------
rem Acts on the directory holding this file, whatever the current directory. Refuses (exit 10, nothing removed)
rem unless that is ronin's repository root, and when build\ or dist\ is a junction, a link or a file. rd /s
rem removes the junctions inside the tree (the tests leave one, a loop) without following them.
:clean
if not exist "%ROOT%src\ronin.c" goto clean_not_root
if not exist "%ROOT%verify.bat" goto clean_not_root
call :clean_check build
if errorlevel 1 exit /b 10
call :clean_check dist
if errorlevel 1 exit /b 10
set CLEAN_ANY=0
if exist "%ROOT%build" set CLEAN_ANY=1
if exist "%ROOT%dist" set CLEAN_ANY=1
if "%CLEAN_ANY%"=="0" (
    echo clean: nothing to remove
    exit /b 0
)
if exist "%ROOT%build" rd /s /q "%ROOT%build"
if exist "%ROOT%dist" rd /s /q "%ROOT%dist"
if exist "%ROOT%build" goto clean_failed
if exist "%ROOT%dist" goto clean_failed
echo clean: removed build\ and dist\
exit /b 0

:clean_not_root
echo clean: refused: %ROOT% is not ronin's repository root (no src\ronin.c / verify.bat beside build.bat); nothing removed
exit /b 10

:clean_failed
echo clean: FAIL: could not remove build\ or dist\ (a file in use?)
exit /b 1

rem --- clean_check <name>: fails when it exists and is not a plain directory (a junction or link is "l") -----
:clean_check
if not exist "%ROOT%%1" exit /b 0
set "CLEAN_ATTR="
for %%A in ("%ROOT%%1") do set "CLEAN_ATTR=%%~aA"
if not "%CLEAN_ATTR:~0,1%"=="d" goto clean_bad
if not "%CLEAN_ATTR:l=%"=="%CLEAN_ATTR%" goto clean_bad
exit /b 0
:clean_bad
echo clean: refused: %1\ is a link or not a directory; nothing removed
exit /b 1
