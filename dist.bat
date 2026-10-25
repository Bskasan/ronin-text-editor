@echo off
rem ronin dist: runs verify.bat (which builds the release exe), then packs dist\ronin-<version>-windows-x64.zip with
rem the files of res\dist_files.txt (ronin.exe, README.md, LICENSE, THIRD_PARTY.md: the ported themes' licenses,
rem CHANGELOG.md and the samples) in a ronin-<version> folder, checks the zip's listing (exactly the list's files) and
rem writes its SHA-256 to dist\SHA256SUMS.txt (sha256sum's format). The version comes from src\app\version.h.
rem From Git Bash: cmd //c ".\dist.bat"
rem The last line is "dist: PASS: <zip> (<bytes> bytes, SHA-256 <hash>)" or "dist: FAIL: <what failed>".
setlocal
set "ROOT=%~dp0"
cd /d "%ROOT%"

set "VERSION="
for /f "tokens=3" %%v in ('findstr /b /c:"#define RONIN_VERSION " src\app\version.h') do set "VERSION=%%~v"
if not defined VERSION (
    echo dist: FAIL: no RONIN_VERSION in src\app\version.h
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
set "SUMS=dist\SHA256SUMS.txt"
rem The zip's files: res\dist_files.txt, the one list (release-check.bat checks the unpacked zip against it too).
set "LIST=res\dist_files.txt"
if exist build\dist rmdir /s /q build\dist
mkdir "build\dist\%NAME%"
set "LISTED=0"
for /f "usebackq eol=# delims=" %%F in ("%LIST%") do (
    call :copy_listed "%%F" || goto copy_failed
    set /a LISTED+=1
)
if not exist dist mkdir dist
if exist "%ZIP%" del /q "%ZIP%"
if exist "%SUMS%" del /q "%SUMS%"
rem Windows' own bsdtar (Windows 10 1803 and later) writes zip files with -a; another tar on PATH may not.
"%SystemRoot%\System32\tar.exe" -a -c -f "%ZIP%" -C build\dist "%NAME%"
if errorlevel 1 (
    echo dist: FAIL: tar
    exit /b 1
)

rem The zip's listing: the list's files, nothing else (folders are listed with a trailing '/').
"%SystemRoot%\System32\tar.exe" -t -f "%ZIP%" > build\dist\contents.txt
if errorlevel 1 (
    echo dist: FAIL: tar could not list the zip
    exit /b 1
)
set "FILES=0"
for /f "usebackq delims=" %%L in (`findstr /v /r /c:"/$" build\dist\contents.txt`) do set /a FILES+=1
if not "%FILES%"=="%LISTED%" (
    echo dist: FAIL: the zip holds %FILES% files, not the %LISTED% of %LIST% ^(build\dist\contents.txt^)
    exit /b 1
)
for /f "usebackq eol=# delims=" %%F in ("%LIST%") do (
    findstr /x /c:"%NAME%/%%F" build\dist\contents.txt >nul || goto listing_failed
)

rem The SHA-256: certutil's second line (older Windows put spaces between the bytes), 64 hex digits.
set "HASH="
for /f "skip=1 delims=" %%H in ('%SystemRoot%\System32\certutil.exe -hashfile "%ZIP%" SHA256') do if not defined HASH set "HASH=%%H"
if not defined HASH goto hash_failed
set "HASH=%HASH: =%"
if "%HASH:~63,1%"=="" goto hash_failed
if not "%HASH:~64%"=="" goto hash_failed
echo %HASH%| findstr /r /x /i "[0-9a-f]*" >nul || goto hash_failed
rem One line in sha256sum's format, without a line end (sha256sum -c would take a CR for part of the name).
<nul set /p "=%HASH%  %NAME%-windows-x64.zip" > "%SUMS%"
for %%F in ("%ZIP%") do echo dist: PASS: %ZIP% (%%~zF bytes, SHA-256 %HASH%)
exit /b 0

:listing_failed
echo dist: FAIL: the zip lacks a file of %LIST% (build\dist\contents.txt)
exit /b 1

rem --- copy_listed <path>: one file of the list ('/' separated) into the staging folder; ronin.exe comes from build\ ---
:copy_listed
set "REL=%~1"
set "SRC=%REL:/=\%"
if "%REL%"=="ronin.exe" set "SRC=build\ronin.exe"
set "DEST=build\dist\%NAME%\%REL:/=\%"
for %%D in ("%DEST%") do if not exist "%%~dpD" mkdir "%%~dpD"
copy /y "%SRC%" "%DEST%" >nul
exit /b %errorlevel%

:hash_failed
echo dist: FAIL: no SHA-256 from certutil
exit /b 1

:copy_failed
echo dist: FAIL: copying the files
exit /b 1
