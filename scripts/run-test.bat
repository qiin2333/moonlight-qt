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
rem Capture and accumulate with delayed expansion DISABLED so argument content
rem (quotes, literal !, %VAR% references) survives verbatim; the invoke line
rem turns delayed expansion ON, which substitutes !TESTARGS! verbatim after
rem parsing, so the content never goes through batch syntax re-parsing.
rem Known limit: an UNQUOTED & in an extra arg can still split the accumulate
rem line (batch quoting has no way to fully shield it); no test needs that.
rem Capture %~2 BEFORE shifting: shift moves it to old %3's slot.
set "TARGET_EXE=%~2"
setlocal EnableExtensions DisableDelayedExpansion
set "TESTARGS="
shift
shift
:collect_args
set "CUR=%1"
if not defined CUR goto run_test
if defined TESTARGS (set "TESTARGS=%TESTARGS% %CUR%") else set "TESTARGS=%CUR%"
shift
goto collect_args
:run_test
rem The SDL2 compatibility DLL loads SDL3 dynamically before main(). Do not let
rem unrelated PATH entries hide missing runtime files in this regression.
if not "%TARGET_EXE%"=="clipboard_helper_lifecycle" goto run_binary
if not exist release\SDL2.dll goto missing_lifecycle_runtime
if not exist release\SDL3.dll goto missing_lifecycle_runtime
set "LIFECYCLE_QT_BIN="
for /f "delims=" %%q in ('qmake -query QT_INSTALL_BINS') do set "LIFECYCLE_QT_BIN=%%q"
if not defined LIFECYCLE_QT_BIN goto missing_lifecycle_runtime
set "PATH=%LIFECYCLE_QT_BIN%;%SystemRoot%\System32;%SystemRoot%"
goto run_binary
:missing_lifecycle_runtime
echo Missing lifecycle runtime: require local SDL2.dll, SDL3.dll and Qt bin directory
popd
exit /b 1
:run_binary
setlocal EnableDelayedExpansion
release\%TARGET_EXE%.exe !TESTARGS!
endlocal & set "RC=%ERRORLEVEL%"
popd
exit /b %RC%
