@echo off
REM -----------------------------
REM generate-nightly.bat
REM Generates the VS solution for nightly builds
REM Uses local tools\premake5.exe and preserves extra arguments
REM -----------------------------

REM Update git submodules
git submodule update --init --recursive

REM Check premake exists
if not exist "tools\premake5.exe" (
    echo ERROR: premake5.exe not found in tools\
    exit /b 1
)

REM Run premake with any extra args passed in (%*) and generate VS2022 solution
tools\premake5.exe %* vs2022

REM Ensure build folder exists
if not exist "build" (
    mkdir build
)

echo consolation-client.sln should now be in build\