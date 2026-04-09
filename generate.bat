@echo off
REM -----------------------------
REM generate.bat
REM Generates the VS solution
REM Uses local tools\premake5.exe and preserves extra arguments
REM -----------------------------

git submodule update --init --recursive

if not exist "tools\premake5.exe" (
    echo ERROR: premake5.exe not found in tools\
    exit /b 1
)

tools\premake5.exe %* vs2022

if not exist "build" (
    mkdir build
)

echo consolation-client.sln should now be in build\
