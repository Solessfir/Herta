@echo off
setlocal

set "HERTA_ROOT=%~dp0"
set "HERTA_ACTION=%~1"
if not defined HERTA_ACTION set "HERTA_ACTION=vs2022"

for /f "usebackq delims=" %%P in (`powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%HERTA_ROOT%Scripts\Setup.ps1" -PrintPremakePath`) do set "HERTA_PREMAKE=%%P"
if not defined HERTA_PREMAKE (
    echo Failed to resolve the project-local Premake executable. 1>&2
    exit /b 1
)

"%HERTA_PREMAKE%" --file="%HERTA_ROOT%premake5.lua" %HERTA_ACTION%
exit /b %ERRORLEVEL%
