@echo off
setlocal

set "HERTA_ROOT=%~dp0"
set "HERTA_REQUESTED_ACTION=%~1"

if defined HERTA_REQUESTED_ACTION if /I not "%HERTA_REQUESTED_ACTION%"=="vs2026" if /I not "%HERTA_REQUESTED_ACTION%"=="vs2022" (
    echo Unsupported Premake action "%HERTA_REQUESTED_ACTION%". Use vs2026 or vs2022. 1>&2
    exit /b 2
)

set "HERTA_VISUAL_STUDIO_ARGUMENTS="
if /I "%HERTA_REQUESTED_ACTION%"=="vs2026" set "HERTA_VISUAL_STUDIO_ARGUMENTS=-VisualStudioVersion 2026"
if /I "%HERTA_REQUESTED_ACTION%"=="vs2022" set "HERTA_VISUAL_STUDIO_ARGUMENTS=-VisualStudioVersion 2022"

for /f "usebackq delims=" %%A in (`powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%HERTA_ROOT%Scripts\Setup.ps1" -PrintVisualStudioAction %HERTA_VISUAL_STUDIO_ARGUMENTS%`) do set "HERTA_ACTION=%%A"
if not defined HERTA_ACTION (
    echo Failed to resolve a supported Visual Studio action. Run Setup.bat first. 1>&2
    exit /b 1
)

for /f "usebackq delims=" %%P in (`powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%HERTA_ROOT%Scripts\Setup.ps1" -PrintPremakePath`) do set "HERTA_PREMAKE=%%P"
if not defined HERTA_PREMAKE (
    echo Failed to resolve the project-local Premake executable. 1>&2
    exit /b 1
)

set "HERTA_VULKAN_SDK="
for /f "usebackq delims=" %%V in (`powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%HERTA_ROOT%Scripts\Setup.ps1" -PrintVulkanSdkPath`) do set "HERTA_VULKAN_SDK=%%V"
if not defined HERTA_VULKAN_SDK (
    echo Failed to resolve the project-local Vulkan SDK. 1>&2
    exit /b 1
)

"%HERTA_PREMAKE%" --file="%HERTA_ROOT%premake5.lua" %HERTA_ACTION%
if %ERRORLEVEL% neq 0 exit /b %ERRORLEVEL%

if /I "%HERTA_ACTION%"=="vs2026" set "HERTA_SOLUTION=%HERTA_ROOT%Herta.slnx"
if /I "%HERTA_ACTION%"=="vs2022" set "HERTA_SOLUTION=%HERTA_ROOT%Herta.sln"

if not exist "%HERTA_SOLUTION%" (
    echo Premake completed without generating the expected solution: "%HERTA_SOLUTION%" 1>&2
    exit /b 1
)

echo Generated solution: "%HERTA_SOLUTION%"
exit /b 0
