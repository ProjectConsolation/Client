@echo off
setlocal

REM Edit this directory if you installed the game elsewhere.
set "GAME_DIR=C:\Program Files (x86)\Activision\Quantum of Solace(TM)"
REM Prefer the installation containing this launcher; retain GAME_DIR fallback.
if exist "%~dp0JB_Launcher_s.exe" set "GAME_DIR=%~dp0."

if not exist "%GAME_DIR%\JB_Launcher_s.exe" (
    echo Game launcher not found in "%GAME_DIR%".
    echo Right-click this file, choose Edit, and correct GAME_DIR.
    pause
    exit /b 1
)

REM Recommended for local GFWL emulation and LAN: install optional\offline\xlive.dll
REM beside the launcher. Consolation detects it automatically; no debug flags needed.
if exist "%~dp0Check Consolation Updates.ps1" (
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0Check Consolation Updates.ps1" -GameDirectory "%GAME_DIR%"
    if errorlevel 2 (
        echo Update rollback needs manual restoration. See the backup path above.
        pause
        exit /b 2
    )
)
start "" /D "%GAME_DIR%" "%GAME_DIR%\JB_Launcher_s.exe" -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1 %*
exit /b
