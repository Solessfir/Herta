@echo off
setlocal

set "HERTA_ROOT=%~dp0"
set "HERTA_REQUESTED_ACTION=%~1"

if defined HERTA_REQUESTED_ACTION if /I not "%HERTA_REQUESTED_ACTION%"=="vs2026" if /I not "%HERTA_REQUESTED_ACTION%"=="vs2022" (
    echo Unsupported Premake action "%HERTA_REQUESTED_ACTION%". Use vs2026 or vs2022. 1>&2
    set "HERTA_EXIT_CODE=2"
    goto :error
)

set "HERTA_VISUAL_STUDIO_ARGUMENTS="
if /I "%HERTA_REQUESTED_ACTION%"=="vs2026" set "HERTA_VISUAL_STUDIO_ARGUMENTS=-VisualStudioVersion 2026"
if /I "%HERTA_REQUESTED_ACTION%"=="vs2022" set "HERTA_VISUAL_STUDIO_ARGUMENTS=-VisualStudioVersion 2022"

for /f "usebackq delims=" %%A in (`powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%HERTA_ROOT%Scripts\Setup.ps1" -PrintVisualStudioAction %HERTA_VISUAL_STUDIO_ARGUMENTS% 2^>nul`) do set "HERTA_ACTION=%%A"
if not defined HERTA_ACTION (
    echo Could not find a supported Visual Studio toolchain. Run Setup.bat first. 1>&2
    set "HERTA_EXIT_CODE=1"
    goto :error
)

for /f "usebackq delims=" %%P in (`powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%HERTA_ROOT%Scripts\Setup.ps1" -PrintPremakePath 2^>nul`) do set "HERTA_PREMAKE=%%P"
if not defined HERTA_PREMAKE (
    echo Could not find project-local Premake. Run Setup.bat first. 1>&2
    set "HERTA_EXIT_CODE=1"
    goto :error
)

set "HERTA_VULKAN_SDK="
for /f "usebackq delims=" %%V in (`powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%HERTA_ROOT%Scripts\Setup.ps1" -PrintVulkanSdkPath 2^>nul`) do set "HERTA_VULKAN_SDK=%%V"
if not defined HERTA_VULKAN_SDK (
    echo Could not find the project-local Vulkan SDK. Run Setup.bat first. 1>&2
    set "HERTA_EXIT_CODE=1"
    goto :error
)

"%HERTA_PREMAKE%" --file="%HERTA_ROOT%premake5.lua" %HERTA_ACTION%
set "HERTA_EXIT_CODE=%ERRORLEVEL%"
if not "%HERTA_EXIT_CODE%"=="0" goto :error

if /I "%HERTA_ACTION%"=="vs2026" set "HERTA_SOLUTION=%HERTA_ROOT%Herta.slnx"
if /I "%HERTA_ACTION%"=="vs2022" set "HERTA_SOLUTION=%HERTA_ROOT%Herta.sln"

if not exist "%HERTA_SOLUTION%" (
    echo Premake completed without generating the expected solution: "%HERTA_SOLUTION%" 1>&2
    set "HERTA_EXIT_CODE=1"
    goto :error
)

echo Generated solution: "%HERTA_SOLUTION%"
exit /b 0

:error
if not defined CI if not defined HERTA_NO_PAUSE pause
exit /b %HERTA_EXIT_CODE%
