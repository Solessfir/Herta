@echo off
setlocal

powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Scripts\Windows\Setup.ps1" %*
exit /b %ERRORLEVEL%
