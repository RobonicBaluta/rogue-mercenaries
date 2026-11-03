@echo off
setlocal
title Rogue Mercenaries - uninstall
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0uninstall.ps1" -FromGameFolder %*
set rc=%errorlevel%
echo.
pause
if not "%rc%"=="0" exit /b %rc%
rem The rest of the mod is gone: this file deletes itself and its folder if that is now empty.
(goto) 2>nul & del "%~f0" & rd "%~dp0." 2>nul
