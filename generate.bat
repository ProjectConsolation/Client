@echo off
REM -----------------------------
REM generate.bat
REM Generates the VS solution
REM Uses premake5.exe and preserves extra arguments
REM -----------------------------

set "PREMAKE_URL=https://github.com/premake/premake-core/releases/download/v5.0.0-beta8/premake-5.0.0-beta8-windows.zip"
set "PREMAKE_HASH=2301e3e23ff3074cb83a5ea6103d68c7ea81dad56b786807c84b0643cddea31b"

REM The following variables can be set:
REM    PREMAKE_NO_GLOBAL - Ignore premake5 executable from PATH
REM    PREMAKE_NO_PROMPT - Download premake5 without prompting

cd /d "%~dp0"

git submodule sync --recursive
git submodule update --init --recursive

if exist "deps\SDL\include\SDL.h" (
    echo Using SDL source from deps\SDL
) else if exist "deps\SDL2-src\SDL2-2.32.10\include\SDL.h" (
    echo Using SDL fallback source from deps\SDL2-src\SDL2-2.32.10
) else (
    echo WARNING: SDL source not found. SDL backend project will not be generated.
)

if "%PREMAKE_NO_GLOBAL%"=="" (
    where /q "premake5.exe"
    if not errorlevel 1 (
        set "PREMAKE_BIN=premake5.exe"
        goto runpremake
    )
)

if exist "tools\premake5.exe" (
    tools\premake5.exe --version >NUL 2>&1
    if not errorlevel 1 (
        set "PREMAKE_BIN=tools\premake5.exe"
        goto runpremake
    )
)

if not "%PREMAKE_NO_PROMPT%"=="" (
    call :downloadpremake
    if errorlevel 1 exit /b 1
    set "PREMAKE_BIN=tools\premake5.exe"
    goto runpremake
)

echo Could not find premake5. You can either install it yourself or this script can download it for you.
set /p choice="Do you wish to download it automatically? [y/N]> "
if /i "%choice%"=="y" (
    call :downloadpremake
    if errorlevel 1 exit /b 1
    set "PREMAKE_BIN=tools\premake5.exe"
    goto runpremake
)

echo Please install premake5 and try again.
exit /b 1

:downloadpremake
if not exist "tools" mkdir "tools"

where /q "pwsh"
if not errorlevel 1 (
    set "POWERSHELL_BIN=pwsh"
) else (
    set "POWERSHELL_BIN=powershell"
)

echo Downloading premake5...
%POWERSHELL_BIN% -NoProfile -NonInteractive -Command "Invoke-WebRequest '%PREMAKE_URL%' -OutFile 'tools\premake.zip'"
if errorlevel 1 (
    echo Download failed. >&2
    exit /b 2
)

echo Extracting premake5...
%POWERSHELL_BIN% -NoProfile -NonInteractive -Command "Expand-Archive -LiteralPath 'tools\premake.zip' -DestinationPath 'tools' -Force"
if errorlevel 1 (
    echo Extraction failed. >&2
    del /q "tools\premake.zip" >NUL 2>&1
    exit /b 2
)

del /q "tools\premake.zip" >NUL 2>&1

echo Verifying premake5 hash...
%POWERSHELL_BIN% -NoProfile -NonInteractive -Command "if ((Get-FileHash -LiteralPath 'tools\premake5.exe' -Algorithm SHA256).Hash -eq '%PREMAKE_HASH%') { exit 0 } else { exit 1 }"
if errorlevel 1 (
    echo Hash verification failed. >&2
    del /q "tools\premake5.exe" >NUL 2>&1
    exit /b 2
)

exit /b 0

:runpremake
%PREMAKE_BIN% %* vs2022

if not exist "build" (
    mkdir build
)

echo consolation-client.sln should now be in build\
