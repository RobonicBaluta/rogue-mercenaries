@echo off
setlocal
title Rogue Mercenaries - uninstall
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0installer\uninstall.ps1" %*
echo.
pause
