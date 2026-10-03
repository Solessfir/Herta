[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Development', 'Shipping')]
    [string] $Configuration = 'Development'
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-LoggedProcess {
    param([string] $FilePath, [string[]] $Arguments, [string] $LogPath)

    # Windows PowerShell turns native stderr into error records; retain diagnostics without aborting the other compiler.
    $ErrorActionPreference = 'Continue'
    & $FilePath @Arguments 2>&1 | Tee-Object -FilePath $LogPath -ErrorAction Stop
    $script:NativeExitCode = $LASTEXITCODE
}

trap {
    Write-Error $_ -ErrorAction Continue
    exit 1
}

if ($env:OS -ne 'Windows_NT') {
    throw 'Windows compiler verification requires Windows.'
}

$RepositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$SetupPath = Join-Path $PSScriptRoot 'Setup.ps1'
$VisualStudioAction = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $SetupPath -PrintVisualStudioAction
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$VisualStudioVersion = $VisualStudioAction.Substring(2)
$MSBuildPath = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $SetupPath -PrintMSBuildPath -VisualStudioVersion $VisualStudioVersion
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$VisualStudioRoot = $MSBuildPath -replace '[\\/]MSBuild[\\/].*$', ''
if ($VisualStudioRoot -eq $MSBuildPath) {
    throw "Cannot locate the Visual Studio installation for '$MSBuildPath'. Run from a normal terminal with Visual Studio installed."
}
$VCTargetsVersion = if ($VisualStudioVersion -eq '2026') { 'v180' } else { 'v170' }
$MSVCToolset = if ($VisualStudioVersion -eq '2026') { 'v145' } else { 'v143' }
$ClangExecutable = Join-Path $VisualStudioRoot 'VC/Tools/Llvm/x64/bin/clang-cl.exe'
$ClangTargets = Join-Path $VisualStudioRoot "MSBuild/Microsoft/VC/$VCTargetsVersion/Platforms/x64/PlatformToolsets/ClangCL/Toolset.targets"
if (-not (Test-Path -LiteralPath $ClangExecutable -PathType Leaf) -or -not (Test-Path -LiteralPath $ClangTargets -PathType Leaf)) {
    throw "ClangCL is missing from '$VisualStudioRoot'. In Visual Studio Installer, add C++ Clang tools for Windows and MSBuild support for LLVM (clang-cl) to this installation."
}

$PreviousNoPause = $env:HERTA_NO_PAUSE
$VerificationExitCode = 0
Push-Location $RepositoryRoot
try {
    $env:HERTA_NO_PAUSE = '1'
    & (Join-Path $RepositoryRoot 'GenerateProjectFiles.bat') $VisualStudioAction
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $Solution = if ($VisualStudioVersion -eq '2026') { 'Herta.slnx' } else { 'Herta.sln' }
    $TestsPath = Join-Path $RepositoryRoot "Binaries/windows/x86_64/$Configuration/HertaTests.exe"
    $LogDirectory = Join-Path $RepositoryRoot 'Intermediate/CompilerVerification'
    [System.IO.Directory]::CreateDirectory($LogDirectory) | Out-Null
    foreach ($Toolset in @('ClangCL', $MSVCToolset)) {
        Write-Host "Rebuilding all projects with $Toolset ($Configuration)..."
        # Compiler outputs share directories, so switching toolsets requires a full rebuild; MSVC runs last.
        Invoke-LoggedProcess -FilePath $MSBuildPath -Arguments @($Solution, '/nologo', '/m', '/t:Rebuild', '/warnaserror', "/p:Configuration=$Configuration", '/p:Platform=x64', "/p:PlatformToolset=$Toolset") -LogPath (Join-Path $LogDirectory "$Toolset-$Configuration.build.log")
        if ($NativeExitCode -ne 0) {
            if ($VerificationExitCode -eq 0) { $VerificationExitCode = $NativeExitCode }
            continue
        }

        Invoke-LoggedProcess -FilePath $TestsPath -Arguments @() -LogPath (Join-Path $LogDirectory "$Toolset-$Configuration.tests.log")
        if ($NativeExitCode -ne 0 -and $VerificationExitCode -eq 0) { $VerificationExitCode = $NativeExitCode }
    }
}
finally {
    $env:HERTA_NO_PAUSE = $PreviousNoPause
    Pop-Location
}

if ($VerificationExitCode -eq 0) {
    Write-Host "ClangCL and $MSVCToolset builds and tests passed ($Configuration)."
}
else {
    Write-Error "Compiler verification failed. See logs in '$LogDirectory'." -ErrorAction Continue
}
exit $VerificationExitCode
