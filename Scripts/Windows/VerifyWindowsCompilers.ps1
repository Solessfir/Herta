[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Development', 'Shipping')]
    [string] $Configuration = 'Development',

    [switch] $Rebuild
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

    & (Join-Path $RepositoryRoot 'GenerateProjectFiles.bat') $VisualStudioAction --clangcl
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $SolutionName = if ($VisualStudioVersion -eq '2026') { 'Herta.slnx' } else { 'Herta.sln' }
    $LogDirectory = Join-Path $RepositoryRoot 'Intermediate/CompilerVerification'
    [System.IO.Directory]::CreateDirectory($LogDirectory) | Out-Null

    # Each compiler has its own workspace and output folders, so both build incrementally. Warnings are errors, so an unchanged file that
    # built cleanly cannot hide one; -Rebuild forces a clean build anyway.
    $Target = if ($Rebuild) { '/t:Rebuild' } else { '/t:Build' }
    $Builds = @(
        # The Slow suite mostly waits on Blender and shader compiles, which do not depend on the engine's compiler, so it runs once.
        @{ Toolset = 'ClangCL'; Solution = "Intermediate/ProjectFiles/$VisualStudioAction-ClangCL/$SolutionName"; Binaries = "Binaries/windows/x86_64/$Configuration-ClangCL"; TestArguments = @('--test-suite-exclude=Slow') },
        @{ Toolset = $MSVCToolset; Solution = $SolutionName; Binaries = "Binaries/windows/x86_64/$Configuration"; TestArguments = @() }
    )
    foreach ($Build in $Builds) {
        $Toolset = $Build.Toolset
        Write-Host "Building all projects with $Toolset ($Configuration)..."
        Invoke-LoggedProcess -FilePath $MSBuildPath -Arguments @($Build.Solution, '/nologo', '/m', $Target, '/warnaserror', "/p:Configuration=$Configuration", '/p:Platform=x64') -LogPath (Join-Path $LogDirectory "$Toolset-$Configuration.build.log")
        if ($NativeExitCode -ne 0) {
            if ($VerificationExitCode -eq 0) { $VerificationExitCode = $NativeExitCode }
            continue
        }

        $TestsPath = Join-Path $RepositoryRoot "$($Build.Binaries)/HertaTests.exe"
        Invoke-LoggedProcess -FilePath $TestsPath -Arguments $Build.TestArguments -LogPath (Join-Path $LogDirectory "$Toolset-$Configuration.tests.log")
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
