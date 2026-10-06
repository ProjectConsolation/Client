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

REM Offline/debug only: install the optional xlive.dll, then append
REM -offline -seta name "CSL_User00" to the START command below.
REM Replace CSL_User00 with your own player name. Keep its quotation marks.
start "" /D "%GAME_DIR%" "%GAME_DIR%\JB_Launcher_s.exe" -multiplayer -seta g_gametype dm -set cin_firstRunDone 1 -set cin_skipAllMovies 1 %*
exit /b
