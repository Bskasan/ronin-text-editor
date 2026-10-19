@echo off
rem ronin dist: runs verify.bat (which builds the release exe), then packs dist\ronin-<version>-windows-x64.zip with
rem ronin.exe, README.md, LICENSE and THIRD_PARTY.md (the ported themes' licenses) in a ronin-<version> folder. The version comes from src\version.h.
rem From Git Bash: cmd //c ".\dist.bat"
rem The last line is "dist: PASS: <zip> (<bytes> bytes)" or "dist: FAIL: <what failed>".
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

set "VERSION="
for /f "tokens=3" %%v in ('findstr /b /c:"#define RONIN_VERSION " src\version.h') do set "VERSION=%%~v"
if not defined VERSION (
    echo dist: FAIL: no RONIN_VERSION in src\version.h
    exit /b 1
)

call "%ROOT%verify.bat"
if errorlevel 1 (
    echo dist: FAIL: verify
    exit /b 1
)
cd /d "%ROOT%"

set "NAME=ronin-%VERSION%"
set "ZIP=dist\%NAME%-windows-x64.zip"
if exist build\dist rmdir /s /q build\dist
mkdir "build\dist\%NAME%"
copy /y build\ronin.exe "build\dist\%NAME%\" >nul || goto copy_failed
copy /y README.md "build\dist\%NAME%\" >nul || goto copy_failed
copy /y LICENSE "build\dist\%NAME%\" >nul || goto copy_failed
copy /y THIRD_PARTY.md "build\dist\%NAME%\" >nul || goto copy_failed
if not exist dist mkdir dist
if exist "%ZIP%" del /q "%ZIP%"
rem Windows' own bsdtar (Windows 10 1803 and later) writes zip files with -a; another tar on PATH may not.
"%SystemRoot%\System32\tar.exe" -a -c -f "%ZIP%" -C build\dist "%NAME%"
if errorlevel 1 (
    echo dist: FAIL: tar
    exit /b 1
)
for %%F in ("%ZIP%") do echo dist: PASS: %ZIP% (%%~zF bytes)
exit /b 0

:copy_failed
echo dist: FAIL: copying the files
exit /b 1
