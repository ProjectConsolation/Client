@echo off
REM -----------------------------
REM generate.bat
REM Generates the VS solution
REM Uses local tools\premake5.exe and preserves extra arguments
REM -----------------------------

git submodule sync --recursive
git submodule update --init --recursive

if exist "deps\SDL\include\SDL.h" (
    echo Using SDL source from deps\SDL
) else if exist "deps\SDL2-src\SDL2-2.32.10\include\SDL.h" (
    echo Using SDL fallback source from deps\SDL2-src\SDL2-2.32.10
) else (
    echo WARNING: SDL source not found. SDL backend project will not be generated.
)

if not exist "tools\premake5.exe" (
    echo ERROR: premake5.exe not found in tools\
    exit /b 1
)

tools\premake5.exe %* vs2022

if not exist "build" (
    mkdir build
)

echo consolation-client.sln should now be in build\
