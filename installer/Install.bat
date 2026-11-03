@echo off
setlocal
title Rogue Mercenaries - install
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\install.ps1" %*
echo.
pause
