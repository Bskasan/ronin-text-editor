@echo off
rem teal verify: the only gate before a commit.
rem   verify.bat        builds debug, release and bench (each with the macro check), runs --smoke on the
rem                     debug build and --test on the bench build
rem   verify.bat phase  the same, plus the full --test on the debug build (once per phase, before its last commit)
rem From Git Bash: cmd //c ".\verify.bat" [phase]
rem Every exit code is checked; the last line is "verify: PASS" or "verify: FAIL: <what failed>".
rem build\teal.log of each run is kept as build\verify\<step>.log.
setlocal EnableDelayedExpansion
set "ROOT=%~dp0"
cd /d "%ROOT%"

set FAILED=
set PHASE=0
if /i "%~1"=="phase" set PHASE=1
if not exist build mkdir build
if not exist build\verify mkdir build\verify
del /q build\verify\*.log 2>nul

call :build debug teal_debug
set DEBUG_OK=!BUILD_OK!
call :build release teal
call :build bench teal_bench
set BENCH_OK=!BUILD_OK!

if "!DEBUG_OK!"=="1" (
    call :run smoke build\teal_debug.exe --smoke
) else (
    call :fail "smoke skipped (no exe)"
)
if "!BENCH_OK!"=="1" (
    call :run test-bench build\teal_bench.exe --test
) else (
    call :fail "test on bench skipped (no exe)"
)
if "%PHASE%"=="1" (
    if "!DEBUG_OK!"=="1" (
        call :run test-debug build\teal_debug.exe --test
    ) else (
        call :fail "test on debug skipped (no exe)"
    )
)

if defined FAILED (
    echo verify: FAIL:!FAILED!
    exit /b 1
)
echo verify: PASS
exit /b 0

rem --- build <mode> <exe name>: a failed build removes its exe, so nothing runs a stale one -----------
:build
set BUILD_OK=0
echo verify: build %1
if exist "build\%2.exe" del /q "build\%2.exe"
call "%ROOT%build.bat" %1 > "build\verify\build-%1.log" 2>&1
set CODE=!errorlevel!
if not "!CODE!"=="0" (
    type "build\verify\build-%1.log"
    call :fail "build %1 (exit !CODE!)"
    exit /b 0
)
if not exist "build\%2.exe" (
    call :fail "build %1 (no exe)"
    exit /b 0
)
set BUILD_OK=1
exit /b 0

rem --- run <step> <exe> <flags...>: waits for the exe (a GUI program) and checks its exit code -------
:run
set STEP=%1
shift
echo verify: %STEP%
start "" /wait %1 %2 %3 %4 %5
set CODE=!errorlevel!
if exist build\teal.log copy /y build\teal.log "build\verify\%STEP%.log" >nul
if not "!CODE!"=="0" call :fail "%STEP% (exit !CODE!)"
exit /b 0

:fail
if defined FAILED (set "FAILED=!FAILED!, %~1") else (set "FAILED= %~1")
exit /b 0
