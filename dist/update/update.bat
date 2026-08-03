@echo off
REM SessionOpenMP updater -- double-click this.
REM
REM It runs update.ps1, which sits next to this file. Open that in Notepad if you want to see exactly
REM what it does before running it; it is plain text and commented.
REM
REM About "-ExecutionPolicy Bypass": Windows blocks PowerShell scripts by default, including ones you
REM downloaded yourself. This applies Bypass to THIS ONE run only -- it does not change any setting on
REM your machine, and the next script you run is still blocked as normal.

setlocal
cd /d "%~dp0"

if not exist "%~dp0update.ps1" (
    echo.
    echo   update.ps1 is missing. It must sit in the same folder as this file.
    echo.
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0update.ps1" %*

echo.
pause
