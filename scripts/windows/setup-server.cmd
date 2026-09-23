@echo off
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-server.ps1" %*
pause
