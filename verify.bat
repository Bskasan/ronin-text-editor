@echo off
rem ronin verify: the only gate before a commit.
rem   verify.bat        builds debug, release and bench (each with the macro check), checks build.bat clean on a
rem                     scratch tree (clean-check), runs the source check (tools\check_source.c: every source file's
rem                     header against docs\ARCHITECTURE.md's module map, the include rules), --smoke on the debug and
rem                     the bench build, --test on the bench build, and checks the release exe's imports and --version
rem   verify.bat phase  the same, plus the full --test on the debug build, the seed sweep (--fuzz-seeds 1 256) on the
rem                     bench build, the soak (--soak 1000000) on both and the leak check (--leak-check all) on the
rem                     debug build (once per phase, before its last commit)
rem From Git Bash: cmd //c ".\verify.bat" [phase]
rem Every exit code is checked; the last line is "verify: PASS" or "verify: FAIL: <what failed>".
rem build\ronin.log of each run is kept as build\verify\<step>.log.
rem Every process it starts runs at below-normal priority (Phase 24), so the machine stays usable meanwhile: it starts
rem itself again once that way (every child inherits the class), and each exe step is started with /belownormal too.
rem The seed sweep uses (logical processors - 2) workers, at least 1; VERIFY_WORKERS=N (1 to 64) sets another count.
setlocal
if defined RONIN_VERIFY_LOW goto verify_low
set RONIN_VERIFY_LOW=1
start "" /belownormal /b /wait cmd /d /c call "%~f0" %*
exit /b %errorlevel%
:verify_low
setlocal EnableDelayedExpansion
set "ROOT=%~dp0"
cd /d "%ROOT%"

set FAILED=
set PHASE=0
if /i "%~1"=="phase" set PHASE=1
set /a WORKERS=NUMBER_OF_PROCESSORS-2
if !WORKERS! lss 1 set WORKERS=1
if defined VERIFY_WORKERS (
    set WORKERS=0
    set /a WORKERS=VERIFY_WORKERS 2>nul
    if !WORKERS! lss 1 set WORKERS=0
    if !WORKERS! gtr 64 set WORKERS=0
    if "!WORKERS!"=="0" (
        echo verify: FAIL: VERIFY_WORKERS must be a number from 1 to 64, not "%VERIFY_WORKERS%"
        exit /b 1
    )
)
if not exist build mkdir build
if not exist build\verify mkdir build\verify
del /q build\verify\*.log 2>nul
rem build.bat compiles the source check on every build: none of an earlier run may stand in for it.
if exist build\check_source.exe del /q build\check_source.exe

call :build debug ronin_debug
set DEBUG_OK=!BUILD_OK!
call :build release ronin
call :build bench ronin_bench
set BENCH_OK=!BUILD_OK!
call :clean_check
call :source_check

if "!DEBUG_OK!"=="1" (
    call :run smoke build\ronin_debug.exe --smoke
    rem The release exe imports exactly the DLLs of res\imports.txt, and no CRT DLL.
    if exist build\ronin.exe (
        call :run check-imports build\ronin_debug.exe --check-imports build\ronin.exe res\imports.txt
        call :version
    ) else (
        call :fail "import check skipped (no release exe)"
    )
) else (
    call :fail "smoke skipped (no exe)"
)
if "!BENCH_OK!"=="1" (
    rem The bench build is CRT-free as the release one is: the smoke runs on it too (without the debug layer).
    call :run smoke-bench build\ronin_bench.exe --smoke
    call :run test-bench build\ronin_bench.exe --test
) else (
    call :fail "test on bench skipped (no exe)"
)
if "%PHASE%"=="1" (
    if "!DEBUG_OK!"=="1" (
        call :run test-debug build\ronin_debug.exe --test
    ) else (
        call :fail "test on debug skipped (no exe)"
    )
    if "!BENCH_OK!"=="1" (
        call :run fuzz-seeds build\ronin_bench.exe --fuzz-seeds 1 256 --workers !WORKERS!
        call :run soak-bench build\ronin_bench.exe --soak 1000000
    ) else (
        call :fail "seed sweep and soak skipped (no exe)"
    )
    if "!DEBUG_OK!"=="1" (
        call :run soak-debug build\ronin_debug.exe --soak 1000000
        call :run leak-check build\ronin_debug.exe --leak-check all
    ) else (
        call :fail "soak and leak check on debug skipped (no exe)"
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

rem --- run <step> <exe> <flags...>: waits for the exe (a GUI program, below normal priority) and checks its exit code -
:run
set STEP=%1
shift
echo verify: %STEP%
start "" /belownormal /wait %1 %2 %3 %4 %5 %6 %7
set CODE=!errorlevel!
if exist build\ronin.log copy /y build\ronin.log "build\verify\%STEP%.log" >nul
if not "!CODE!"=="0" call :fail "%STEP% (exit !CODE!)"
exit /b 0

rem --- version: the release exe's --version prints "ronin X.Y.Z" through a redirected standard output, exit 0 ---
:version
echo verify: version
build\ronin.exe --version > build\verify\version.log 2>&1
set CODE=!errorlevel!
if not "!CODE!"=="0" (
    call :fail "version (exit !CODE!)"
    exit /b 0
)
findstr /r /x /c:"ronin [0-9][0-9]*\.[0-9][0-9]*\.[0-9][0-9]*" build\verify\version.log >nul
if errorlevel 1 call :fail "version (printed something else)"
exit /b 0

rem --- source-check: tools\check_source.c (built by build.bat) on the repository; its output, kept as
rem build\verify\source-check.log, is shown when it fails ------------------------------------------------------------
:source_check
echo verify: source-check
if not exist build\check_source.exe (
    call :fail "source-check skipped (no build\check_source.exe)"
    exit /b 0
)
build\check_source.exe . > build\verify\source-check.log 2>&1
set CODE=!errorlevel!
if not "!CODE!"=="0" (
    type build\verify\source-check.log
    call :fail "source-check (exit !CODE!)"
)
exit /b 0

rem --- clean-check: build.bat clean on a scratch tree, run from another directory that looks like a repository:
rem build\ present (holding a junction to a directory outside it, which must survive), missing, a junction, a file;
rem a directory that is not the repository root. The tree is removed afterwards. Log: build\verify\clean-check.log.
:clean_check
echo verify: clean-check
call :clock
set CC_START=!CLOCK!
set "CC=%ROOT%build\tmp\clean_check"
set "CCLOG=%ROOT%build\verify\clean-check.log"
set CC_FAILED=
if exist "!CC!" rd /s /q "!CC!"
mkdir "!CC!\repo\src" "!CC!\repo\build\x" "!CC!\repo\dist" "!CC!\outside" "!CC!\notrepo\build" "!CC!\cwd\src" "!CC!\cwd\build"
copy /y "%ROOT%build.bat" "!CC!\repo\" >nul
copy /y "%ROOT%build.bat" "!CC!\notrepo\" >nul
for %%F in (repo\src\ronin.c repo\verify.bat repo\build\x\f.txt repo\dist\z.zip outside\sentinel.txt notrepo\build\keep.txt cwd\src\ronin.c cwd\verify.bat cwd\build\keep.txt) do (
    echo clean-check> "!CC!\%%F"
)
mklink /J "!CC!\repo\build\link" "!CC!\outside" >nul
> "!CCLOG!" echo clean-check in !CC! (current directory !CC!\cwd)
pushd "!CC!\cwd"
call :cc_run present 0 repo
if exist "!CC!\repo\build" call :cc_fail "present: build\ left"
if exist "!CC!\repo\dist" call :cc_fail "present: dist\ left"
if not exist "!CC!\outside\sentinel.txt" call :cc_fail "present: the junction was followed"
call :cc_run missing 0 repo
mklink /J "!CC!\repo\build" "!CC!\outside" >nul
call :cc_run junction 10 repo
if not exist "!CC!\repo\build" call :cc_fail "junction: removed"
if not exist "!CC!\outside\sentinel.txt" call :cc_fail "junction: its target touched"
rd "!CC!\repo\build"
echo clean-check> "!CC!\repo\dist"
call :cc_run file 10 repo
del /q "!CC!\repo\dist"
call :cc_run not-root 10 notrepo
if not exist "!CC!\notrepo\build\keep.txt" call :cc_fail "not-root: build\ touched"
if not exist "!CC!\cwd\build\keep.txt" call :cc_fail "the current directory's build\ touched"
popd
rd /s /q "!CC!"
if exist "!CC!" call :cc_fail "the scratch tree left"
call :clock
set /a CC_CS=CLOCK-CC_START
if !CC_CS! lss 0 set /a CC_CS+=8640000
>> "!CCLOG!" echo time: !CC_CS!0 ms
if defined CC_FAILED call :fail "clean-check:!CC_FAILED!"
exit /b 0

rem --- cc_run <case> <expected exit code> <directory>: build.bat clean in the scratch tree, its output logged --------
:cc_run
call "!CC!\%3\build.bat" clean >> "!CCLOG!" 2>&1
set CODE=!errorlevel!
>> "!CCLOG!" echo %1: exit !CODE!
if not "!CODE!"=="%2" call :cc_fail "%1 (exit !CODE!)"
exit /b 0

:cc_fail
>> "!CCLOG!" echo FAIL: %~1
if defined CC_FAILED (set "CC_FAILED=!CC_FAILED!; %~1") else (set "CC_FAILED= %~1")
exit /b 0

rem --- clock: CLOCK = centiseconds since midnight -------------------------------------------------------------
:clock
set "CLOCK_T=%time: =0%"
set /a CLOCK=((1!CLOCK_T:~0,2!-100)*3600 + (1!CLOCK_T:~3,2!-100)*60 + (1!CLOCK_T:~6,2!-100))*100 + (1!CLOCK_T:~9,2!-100)
exit /b 0

:fail
if defined FAILED (set "FAILED=!FAILED!, %~1") else (set "FAILED= %~1")
exit /b 0
