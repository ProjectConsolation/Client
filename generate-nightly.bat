@echo off
setlocal

REM Keep the legacy nightly entry point, but use the shared solution generator.
REM generate.bat accepts the same Premake arguments and performs dependency setup.
cd /d "%~dp0"
call "%~dp0generate.bat" %*
set "GENERATE_EXIT_CODE=%ERRORLEVEL%"

endlocal & exit /b %GENERATE_EXIT_CODE%
