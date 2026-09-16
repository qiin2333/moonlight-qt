@echo off
rem Usage: scripts\run-test.bat <test_dir> <target_exe> [args passed to the test...]
rem Builds and runs one test under tests\<test_dir>: qmake -> jom release -> run.
rem Exits with the failing step's error code. Shared by CI (build.yml) and local runs:
rem   - qmake must already be on PATH (Qt bin dir);
rem   - the MSVC environment is set up here via vswhere + vcvarsall;
rem   - jom parallelism: set JOM_JOBS=-j8 locally; leave empty in CI so
rem     JOM_MAX_CPUS (set by build.yml) governs it;
rem   - tests needing a specific Qt platform (e.g. QTest UI tests) should
rem     export QT_QPA_PLATFORM before calling this script.
rem Keep this file ASCII-only: cmd parses it under the OEM codepage, and
rem non-ASCII comments get misparsed into bogus commands.
setlocal
if "%~1"=="" echo usage: run-test.bat ^<test_dir^> ^<target_exe^> & exit /b 2
if "%~2"=="" echo usage: run-test.bat ^<test_dir^> ^<target_exe^> & exit /b 2

for /f "usebackq delims=" %%i in (`%~dp0vswhere.exe -latest -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvarsall.bat" AMD64
if errorlevel 1 exit /b %ERRORLEVEL%

pushd tests\%~1
if errorlevel 1 exit /b %ERRORLEVEL%
qmake %~1.pro
if errorlevel 1 exit /b %ERRORLEVEL%
..\..\scripts\jom.exe %JOM_JOBS% release
if errorlevel 1 exit /b %ERRORLEVEL%

rem Forward every extra arg (%3 and beyond) to the test binary. Batch tops out
rem at %9, so collect them with a shift loop instead of listing %3..%9.
rem Delayed expansion keeps argument content (quotes included) out of the parser.
rem Capture %~2 BEFORE shifting: shift moves it to old %3's slot.
set "TARGET_EXE=%~2"
setlocal EnableExtensions EnableDelayedExpansion
set "TESTARGS="
shift
shift
:collect_args
set "CUR=%~1"
if not defined CUR goto run_test
if defined TESTARGS (set "TESTARGS=!TESTARGS! !CUR!") else set "TESTARGS=!CUR!"
shift
goto collect_args
:run_test
release\!TARGET_EXE!.exe !TESTARGS!
endlocal & set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
