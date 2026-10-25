@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0de.ps1" %*
exit /b %ERRORLEVEL%
