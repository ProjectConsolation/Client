@echo off
setlocal

REM Edit this directory if you installed the game elsewhere.
set "GAME_DIR=C:\Program Files (x86)\Activision\Quantum of Solace(TM)"

if not exist "%GAME_DIR%\JB_Launcher_s.exe" (
    echo Game launcher not found in "%GAME_DIR%".
    echo Right-click this file, choose Edit, and correct GAME_DIR.
    pause
    exit /b 1
)

REM Recommended for local GFWL emulation and LAN: install optional\offline\xlive.dll
REM beside the launcher. Consolation detects it automatically; no debug flags needed.
start "" /D "%GAME_DIR%" "%GAME_DIR%\JB_Launcher_s.exe" -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1 %*
exit /b
