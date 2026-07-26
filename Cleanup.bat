@echo off
setlocal

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\Cleanup.ps1" %*
exit /b %ERRORLEVEL%
