@echo off
setlocal

REM Keep the legacy nightly entry point, but use the shared solution generator.
REM generate.bat accepts the same Premake arguments and performs dependency setup.
cd /d "%~dp0"
call "%~dp0generate.bat" %*
set "GENERATE_EXIT_CODE=%ERRORLEVEL%"

if not "%GENERATE_EXIT_CODE%"=="0" goto finish
if not exist "build\xlive.vcxproj" (
    echo ERROR: The nightly solution is missing the xlive project. >&2
    set "GENERATE_EXIT_CODE=1"
    goto finish
)
echo Generated d3d9 and the optional xlive offline/debug shim projects.
echo Building d3d9 also builds xlive.dll; this script does not compile them.
echo Use xlive.dll only for offline/debug. See required_files\README_XLIVE.txt.

:finish
endlocal & exit /b %GENERATE_EXIT_CODE%
