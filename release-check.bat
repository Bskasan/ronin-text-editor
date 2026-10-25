@echo off
rem ronin release-check (#30): the maintainer's check before a release pull request. Never run by CI, never needed
rem from contributors; a full run is one announced heavy run (about 20 minutes, the machine left alone).
rem   release-check.bat        the preconditions (a clean tree; HEAD published on origin and containing origin's main;
rem                            src\app\version.h, CHANGELOG.md's first dated section and docs\releases\<version>.md
rem                            agree), verify.bat phase, dist.bat, the code coverage of the debug build's --test (a
rem                            report, never a gate), the zip checked without taking the focus, the HUMAN list of
rem                            docs\MANUAL_TESTS.md, and the maintainer's %APPDATA%\ronin and %LOCALAPPDATA%\ronin
rem                            unchanged
rem   release-check.bat quick  for development runs: the preconditions (the tree and the commit reported, not
rem                            required), the zip checks on an existing dist\, the HUMAN list and the unchanged check
rem From Git Bash: cmd //c ".\release-check.bat" [quick]
rem It stops at the first failing step. The last line is "release-check: PASS" (quick: "release-check: PASS (quick)")
rem or "release-check: FAIL: <step>". The whole run is build\release-check\<yyyymmdd-hhmmss>.log, each step's output
rem is kept in build\release-check\<yyyymmdd-hhmmss>\. Nothing outside build\ is written but dist\ (dist.bat's zip).
rem The helper is tools\release_check.c (build\release_check.exe, made by build.bat).
setlocal EnableDelayedExpansion
set "ROOT=%~dp0"
cd /d "%ROOT%"
set "GIT_TERMINAL_PROMPT=0"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "RCT=build\tmp\release-check"
set QUICK=0
if /i "%~1"=="quick" set QUICK=1
if not "%~1"=="" if "!QUICK!"=="0" (
    echo release-check: FAIL: unknown argument "%~1" ^(quick, or nothing for the full run^)
    exit /b 1
)
call :clock
set "RUN_START=!CLOCK!"

rem --- tool: the debug build, so that build\release_check.exe and build\ronin_debug.exe are current ---------------
set "STEP=tool"
set "STEP_START=!CLOCK!"
echo release-check: tool
if not exist build\release-check mkdir build\release-check
set "BOOT=build\release-check\tool.log"
call "%ROOT%build.bat" > "%BOOT%" 2>&1
set CODE=!errorlevel!
cd /d "%ROOT%"
if not "!CODE!"=="0" (
    type "%BOOT%"
    echo release-check: build.bat exit !CODE!
    echo release-check: FAIL: tool
    exit /b 1
)
set "STAMP="
for /f %%s in ('build\release_check.exe stamp') do set "STAMP=%%s"
if not defined STAMP (
    echo release-check: build\release_check.exe stamp printed nothing
    echo release-check: FAIL: tool
    exit /b 1
)
set "RUN=build\release-check\!STAMP!"
set "LOG=build\release-check\!STAMP!.log"
mkdir "!RUN!"
move /y "%BOOT%" "!RUN!\tool.log" >nul
if "!QUICK!"=="1" (set "M=release-check: quick run !STAMP!, log !LOG!") else (set "M=release-check: full run !STAMP!, log !LOG!")
>> "!LOG!" echo(release-check: tool
call :say
call :end

rem --- preconditions --------------------------------------------------------------------------------------------
set "STEP=preconditions"
call :begin
where git >nul 2>nul || (set "M=git is not on PATH" & goto failed)
call :check_tree
call :require || goto failed
call :check_commit
call :require || goto failed
build\release_check.exe versions . > "!RUN!\versions.txt" 2>&1
set CODE=!errorlevel!
call :show "!RUN!\versions.txt"
if not "!CODE!"=="0" (set "M=the versions disagree (above)" & goto failed)
set "VERSION="
for /f "tokens=3" %%v in ('findstr /b /c:"#define RONIN_VERSION " src\app\version.h') do set "VERSION=%%~v"
if not defined VERSION (set "M=no RONIN_VERSION in src\app\version.h" & goto failed)
rem The maintainer's folders and the repository's untracked and ignored paths, compared again at the end.
build\release_check.exe snapshot "%APPDATA%\ronin" "!RUN!\appdata-before.txt" || (set "M=cannot snapshot %APPDATA%\ronin" & goto failed)
build\release_check.exe snapshot "%LOCALAPPDATA%\ronin" "!RUN!\localappdata-before.txt" || (set "M=cannot snapshot %LOCALAPPDATA%\ronin" & goto failed)
git status --porcelain --ignored > "!RUN!\paths-before.txt" 2>nul || (set "M=git status --ignored failed" & goto failed)
git status --porcelain > "!RUN!\status-before.txt" 2>nul
set "HEAD_BEFORE=!HEAD!"
set "M=release-check: snapshots of %APPDATA%\ronin and %LOCALAPPDATA%\ronin taken"
call :say
call :end
if "!QUICK!"=="1" goto zip

rem --- verify-phase: verify.bat phase, exit 0 and its last line exactly "verify: PASS" ------------------------------
set "STEP=verify-phase"
call :begin
call "%ROOT%verify.bat" phase > "!RUN!\verify-phase.log" 2>&1
set CODE=!errorlevel!
cd /d "%ROOT%"
call :show "!RUN!\verify-phase.log"
rem dist.bat's verify.bat empties build\verify: its logs are kept here first.
mkdir "!RUN!\verify-phase"
copy /y build\verify\*.log "!RUN!\verify-phase\" >nul 2>nul
call :last_line "!RUN!\verify-phase.log"
if not "!CODE!"=="0" (set "M=verify.bat phase exit !CODE! (its logs: !RUN!\verify-phase\)" & goto failed)
if not "!LAST!"=="verify: PASS" (set "M=verify.bat phase's last line is "!LAST!", not "verify: PASS"" & goto failed)
call :end

rem --- dist: dist.bat (its own verify.bat, the zip, SHA256SUMS.txt), exit 0 and a last line "dist: PASS: ..." --------
set "STEP=dist"
call :begin
call "%ROOT%dist.bat" > "!RUN!\dist.log" 2>&1
set CODE=!errorlevel!
cd /d "%ROOT%"
call :show "!RUN!\dist.log"
mkdir "!RUN!\dist-verify"
copy /y build\verify\*.log "!RUN!\dist-verify\" >nul 2>nul
call :last_line "!RUN!\dist.log"
if not "!CODE!"=="0" (set "M=dist.bat exit !CODE! (its verify's logs: !RUN!\dist-verify\)" & goto failed)
if not "!LAST:~0,12!"=="dist: PASS: " (set "M=dist.bat's last line is "!LAST!", not "dist: PASS: ..."" & goto failed)
call :end

rem --- coverage: the debug build's --test under Microsoft.CodeCoverage.Console; a report, never a gate --------------
set "STEP=coverage"
call :begin
call :coverage
call :end

rem --- zip: the zip of dist\ checked without taking the focus -------------------------------------------------------
:zip
set "STEP=zip"
call :begin
set "ZIPNAME=ronin-!VERSION!-windows-x64.zip"
if not exist "dist\!ZIPNAME!" (set "M=no dist\!ZIPNAME! (the zip of src\app\version.h's version; run dist.bat first)" & goto failed)
if not exist dist\SHA256SUMS.txt (set "M=no dist\SHA256SUMS.txt" & goto failed)
set "HASH="
for /f "skip=1 delims=" %%H in ('%SystemRoot%\System32\certutil.exe -hashfile "dist\!ZIPNAME!" SHA256') do if not defined HASH set "HASH=%%H"
if not defined HASH (set "M=no SHA-256 from certutil" & goto failed)
set "HASH=!HASH: =!"
set "SUMLINE="
set /p SUMLINE=<dist\SHA256SUMS.txt
if not "!SUMLINE!"=="!HASH!  !ZIPNAME!" (set "M=dist\SHA256SUMS.txt holds "!SUMLINE!"; the zip's SHA-256 line is "!HASH!  !ZIPNAME!"" & goto failed)
set "M=zip: dist\!ZIPNAME!, SHA-256 !HASH!, as dist\SHA256SUMS.txt says"
call :say
if exist "%RCT%" rd /s /q "%RCT%"
if exist "%RCT%" (set "M=cannot empty %RCT%" & goto failed)
mkdir "%RCT%\appdata" "%RCT%\localappdata"
setlocal
set "APPDATA=%ROOT%%RCT%\appdata"
set "LOCALAPPDATA=%ROOT%%RCT%\localappdata"
build\release_check.exe zip "dist\!ZIPNAME!" !VERSION! "%RCT%\zip" res\dist_files.txt build\ronin_debug.exe res\imports.txt > "!RUN!\zip.txt" 2>&1
set CODE=!errorlevel!
endlocal & set "CODE=%CODE%"
if exist build\ronin.log copy /y build\ronin.log "!RUN!\zip-check-imports.log" >nul
call :show "!RUN!\zip.txt"
if not "!CODE!"=="0" (set "M=the zip check failed (above)" & goto failed)
call :end

rem --- human: the HUMAN steps left, from docs\MANUAL_TESTS.md itself ---------------------------------------------------
set "STEP=human"
call :begin
build\release_check.exe human docs\MANUAL_TESTS.md > "!RUN!\human.txt" 2>&1
set CODE=!errorlevel!
call :show "!RUN!\human.txt"
if not "!CODE!"=="0" (set "M=the HUMAN list could not be read (above)" & goto failed)
call :end

rem --- unchanged: the maintainer's folders, the repository's paths, HEAD -------------------------------------------
set "STEP=unchanged"
call :begin
build\release_check.exe snapshot "%APPDATA%\ronin" "!RUN!\appdata-after.txt" || (set "M=cannot snapshot %APPDATA%\ronin" & goto failed)
build\release_check.exe snapshot "%LOCALAPPDATA%\ronin" "!RUN!\localappdata-after.txt" || (set "M=cannot snapshot %LOCALAPPDATA%\ronin" & goto failed)
fc /b "!RUN!\appdata-before.txt" "!RUN!\appdata-after.txt" >nul || (call :show_pair appdata & set "M=%APPDATA%\ronin changed during the run (before, then after, above)" & goto failed)
fc /b "!RUN!\localappdata-before.txt" "!RUN!\localappdata-after.txt" >nul || (call :show_pair localappdata & set "M=%LOCALAPPDATA%\ronin changed during the run (before, then after, above)" & goto failed)
git status --porcelain > "!RUN!\status-after.txt" 2>nul
fc /b "!RUN!\status-before.txt" "!RUN!\status-after.txt" >nul || (call :show "!RUN!\status-after.txt" & set "M=git status changed during the run (now above)" & goto failed)
git status --porcelain --ignored > "!RUN!\paths-after.txt" 2>nul
rem The untracked and ignored paths as at the start, but build\ and dist\ (where the run writes; git lists each as one
rem line, "!! build/"). git writes LF lines, so no findstr /x here (its line end needs a CR).
findstr /v /r /c:"^.. build/" /c:"^.. dist/" "!RUN!\paths-before.txt" > "!RUN!\outside-before.txt"
findstr /v /r /c:"^.. build/" /c:"^.. dist/" "!RUN!\paths-after.txt" > "!RUN!\outside-after.txt"
fc /b "!RUN!\outside-before.txt" "!RUN!\outside-after.txt" >nul || (call :show_pair outside & set "M=the run wrote outside build\ and dist\ (the untracked and ignored paths before, then after, above)" & goto failed)
set "HEAD_AFTER="
for /f %%h in ('git rev-parse HEAD') do set "HEAD_AFTER=%%h"
if not "!HEAD_AFTER!"=="!HEAD_BEFORE!" (set "M=HEAD moved during the run: !HEAD_BEFORE! to !HEAD_AFTER!" & goto failed)
set "M=release-check: %APPDATA%\ronin and %LOCALAPPDATA%\ronin unchanged; nothing new in the repository outside build\ and dist\; HEAD !HEAD_AFTER!"
call :say
set "M=release-check: not checked: %%TEMP%% and the rest of the disk"
call :say
call :end

call :clock
set /a CS=CLOCK-RUN_START
if !CS! lss 0 set /a CS+=8640000
set /a SECS=CS/100
set "M=release-check: total !SECS! s"
call :say
if "!QUICK!"=="1" (set "M=release-check: PASS (quick)") else (set "M=release-check: PASS")
call :say
exit /b 0

:failed
set "M=release-check: !M!"
call :say
set "M=release-check: FAIL: !STEP!"
call :say
exit /b 1

rem --- check_tree: PROBLEM when the working tree is not clean (untracked files count) -------------------------------
:check_tree
set "PROBLEM="
git status --porcelain > "!RUN!\tree.txt" 2>nul
if errorlevel 1 (
    set "PROBLEM=git status failed"
    exit /b 0
)
for %%A in ("!RUN!\tree.txt") do set "SIZE=%%~zA"
if "!SIZE!"=="0" (
    set "M=release-check: the working tree is clean"
    call :say
) else (
    call :show "!RUN!\tree.txt"
    set "PROBLEM=the working tree is not clean (git status --porcelain above)"
)
exit /b 0

rem --- check_commit: PROBLEM unless HEAD is the published commit: on main, origin's main; on another branch, that
rem branch on origin, with origin's main in it. origin is read with git ls-remote (no fetch: nothing written) -------
:check_commit
set "PROBLEM="
set "HEAD="
for /f %%h in ('git rev-parse HEAD 2^>nul') do set "HEAD=%%h"
set "BRANCH="
for /f "delims=" %%b in ('git symbolic-ref --quiet --short HEAD 2^>nul') do set "BRANCH=%%b"
if not defined BRANCH (
    set "PROBLEM=HEAD !HEAD! is detached: check out main or a release branch"
    exit /b 0
)
set "MAIN="
for /f "tokens=1" %%h in ('git ls-remote origin refs/heads/main 2^>nul') do set "MAIN=%%h"
if not defined MAIN (
    set "PROBLEM=cannot read origin's main (git ls-remote origin refs/heads/main)"
    exit /b 0
)
if "!BRANCH!"=="main" (
    if not "!HEAD!"=="!MAIN!" set "PROBLEM=HEAD !HEAD! is not origin's main !MAIN! (push or pull first)"
) else (
    set "PUB="
    for /f "tokens=1" %%h in ('git ls-remote origin "refs/heads/!BRANCH!" 2^>nul') do set "PUB=%%h"
    if not defined PUB (
        set "PROBLEM=the branch !BRANCH! is not on origin (push it first)"
    ) else if not "!PUB!"=="!HEAD!" (
        set "PROBLEM=HEAD !HEAD! is not origin's !BRANCH! !PUB! (push or pull first)"
    ) else (
        git cat-file -e "!MAIN!^^{commit}" 2>nul
        if errorlevel 1 (
            set "PROBLEM=origin's main !MAIN! is not in this clone: git fetch first"
        ) else (
            git merge-base --is-ancestor !MAIN! HEAD
            if errorlevel 1 set "PROBLEM=origin's main !MAIN! is not in !BRANCH!: bring the branch up to date with main"
        )
    )
)
if not defined PROBLEM (
    set "M=release-check: HEAD !HEAD! is origin's !BRANCH!"
    if not "!BRANCH!"=="main" set "M=!M!, and origin's main !MAIN! is in it"
    call :say
)
exit /b 0

rem --- require: exit 1 (M = the problem) when PROBLEM is set; in a quick run it is only reported --------------------
:require
if not defined PROBLEM exit /b 0
if "!QUICK!"=="1" (
    set "M=release-check: quick: not required: !PROBLEM!"
    call :say
    exit /b 0
)
set "M=!PROBLEM!"
exit /b 1

rem --- coverage: never fails the run; every outcome is a line ------------------------------------------------------
:coverage
set "CCX="
if exist "%VSWHERE%" for /f "usebackq delims=" %%p in (`"%VSWHERE%" -latest -products * -find Common7\IDE\Extensions\Microsoft\CodeCoverage.Console\Microsoft.CodeCoverage.Console.exe`) do if not defined CCX set "CCX=%%p"
if not defined CCX (
    set "M=coverage: skipped: Microsoft.CodeCoverage.Console not found (vswhere, Visual Studio 2022 or later)"
    call :say
    exit /b 0
)
set "M=coverage: !CCX!"
call :say
"!CCX!" --version > "!RUN!\coverage-version.txt" 2>&1
call :show "!RUN!\coverage-version.txt"
if exist "%RCT%\cov-temp" rd /s /q "%RCT%\cov-temp"
mkdir "%RCT%\cov-temp"
setlocal
set "TEMP=%ROOT%%RCT%\cov-temp"
set "TMP=%ROOT%%RCT%\cov-temp"
start "" /belownormal /b /wait "!CCX!" collect -f cobertura -o "%ROOT%!RUN!\coverage.cobertura.xml" -l "%ROOT%!RUN!\coverage-tool.log" --nologo build\ronin_debug.exe --test > "!RUN!\coverage-collect.txt" 2>&1
set CODE=!errorlevel!
endlocal & set "CODE=%CODE%"
call :show "!RUN!\coverage-collect.txt"
if exist build\ronin.log copy /y build\ronin.log "!RUN!\coverage-test.log" >nul
if not "!CODE!"=="0" (
    set "FAILS=0"
    for /f %%n in ('findstr /c:"test: FAIL" "!RUN!\coverage-test.log" ^| find /c /v ""') do set "FAILS=%%n"
    set "M=coverage: --test exited !CODE! under instrumentation, !FAILS! "test: FAIL" lines (!RUN!\coverage-test.log). Not a gate: the instrumentation changes function addresses and timing, and the uninstrumented --test passed in verify-phase; the report under-counts what --test runs"
    call :say
)
if not exist "!RUN!\coverage.cobertura.xml" (
    set "M=coverage: skipped: no report was written (!RUN!\coverage-tool.log)"
    call :say
    exit /b 0
)
build\release_check.exe coverage "!RUN!\coverage.cobertura.xml" src > "!RUN!\coverage.txt" 2>&1
call :show "!RUN!\coverage.txt"
set "M=coverage: report !RUN!\coverage.txt (from !RUN!\coverage.cobertura.xml)"
call :say
exit /b 0

rem --- small helpers ------------------------------------------------------------------------------------------------
:begin
call :clock
set "STEP_START=!CLOCK!"
set "M=release-check: !STEP!"
call :say
exit /b 0

:end
call :clock
set /a CS=CLOCK-STEP_START
if !CS! lss 0 set /a CS+=8640000
set /a SECS=CS/100
set "M=release-check: !STEP! ok (!SECS! s)"
call :say
exit /b 0

rem say: M on the console and in the log.
:say
echo(!M!
>> "!LOG!" echo(!M!
exit /b 0

rem show <file>: the file on the console and in the log.
:show
type "%~1"
type "%~1" >> "!LOG!"
exit /b 0

rem show_pair <name>: <name>-before.txt, then <name>-after.txt of the run's folder.
:show_pair
call :show "!RUN!\%1-before.txt"
set "M=--"
call :say
call :show "!RUN!\%1-after.txt"
exit /b 0

rem last_line <file>: LAST = its last non-empty line.
:last_line
set "LAST="
for /f "usebackq delims=" %%L in ("%~1") do set "LAST=%%L"
exit /b 0

rem clock: CLOCK = centiseconds since midnight (as verify.bat's).
:clock
set "CLOCK_T=%time: =0%"
set /a CLOCK=((1!CLOCK_T:~0,2!-100)*3600 + (1!CLOCK_T:~3,2!-100)*60 + (1!CLOCK_T:~6,2!-100))*100 + (1!CLOCK_T:~9,2!-100)
exit /b 0
