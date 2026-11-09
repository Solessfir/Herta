[CmdletBinding()]
param(
    [ValidateSet('Debug', 'Development', 'Shipping')]
    [string] $Configuration = 'Development',

    [switch] $Rebuild
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Start-LoggedProcess {
    param([string] $FilePath, [string[]] $Arguments, [string] $LogPath)

    $Process = Start-Process -FilePath $FilePath -ArgumentList $Arguments -NoNewWindow -PassThru -RedirectStandardOutput $LogPath -RedirectStandardError "$LogPath.stderr"
    # Windows PowerShell only reports ExitCode for a process whose handle was opened before it exited.
    $null = $Process.Handle
    return @{ Process = $Process; LogPath = $LogPath }
}

function Wait-LoggedProcess {
    param($Started)

    $Started.Process.WaitForExit()
    $Errors = Get-Content -LiteralPath "$($Started.LogPath).stderr" -Raw -ErrorAction SilentlyContinue
    if ($Errors) {
        Add-Content -LiteralPath $Started.LogPath -Value $Errors
    }

    Remove-Item -LiteralPath "$($Started.LogPath).stderr" -ErrorAction SilentlyContinue
    return $Started.Process.ExitCode
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

$PremakePath = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $SetupPath -PrintPremakePath
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
$env:HERTA_VULKAN_SDK = & powershell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File $SetupPath -PrintVulkanSdkPath
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$VerificationExitCode = 0
Push-Location $RepositoryRoot
try {
    # Premake runs directly because GenerateProjectFiles.bat repeats the toolchain checks done above, which take seconds each.
    foreach ($Options in @(@(), @('--clangcl'))) {
        & $PremakePath --file=premake5.lua @Options $VisualStudioAction | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "Premake failed to generate $VisualStudioAction $Options project files." }
    }

    $SolutionName = if ($VisualStudioVersion -eq '2026') { 'Herta.slnx' } else { 'Herta.sln' }
    $LogDirectory = Join-Path $RepositoryRoot 'Intermediate/CompilerVerification'
    [System.IO.Directory]::CreateDirectory($LogDirectory) | Out-Null

    # Each compiler has its own workspace and output folders, so both build incrementally. Warnings are errors, so an unchanged file that
    # built cleanly cannot hide one; -Rebuild forces a clean build anyway.
    $Target = if ($Rebuild) { '/t:Rebuild' } else { '/t:Build' }
    $Builds = @(
        # The Slow suite mostly waits on Blender and shader compiles, which do not depend on the engine's compiler, so it runs once.
        @{ Toolset = 'ClangCL'; Solution = "Intermediate/ProjectFiles/$VisualStudioAction-ClangCL/$SolutionName"; Binaries = "Binaries/windows/x86_64/$Configuration-ClangCL"; TestSuites = @(@{ Name = 'Fast'; Arguments = @('--test-suite-exclude=Slow') }) },
        @{ Toolset = $MSVCToolset; Solution = $SolutionName; Binaries = "Binaries/windows/x86_64/$Configuration"; TestSuites = @(@{ Name = 'Fast'; Arguments = @('--test-suite-exclude=Slow') }, @{ Name = 'Slow'; Arguments = @('--test-suite=Slow') }) }
    )

    # The compilers share no outputs, so both build at once; the slower link-bound tail of one overlaps the other's compiles.
    Write-Host "Building all projects with ClangCL and $MSVCToolset ($Configuration)..."
    foreach ($Build in $Builds) {
        $Build.Started = Start-LoggedProcess -FilePath $MSBuildPath -Arguments @($Build.Solution, '/nologo', '/m', $Target, '/warnaserror', "/p:Configuration=$Configuration", '/p:Platform=x64') -LogPath (Join-Path $LogDirectory "$($Build.Toolset)-$Configuration.build.log")
    }

    $Tests = @()
    foreach ($Build in $Builds) {
        $ExitCode = Wait-LoggedProcess $Build.Started
        if ($ExitCode -ne 0) {
            Write-Host "$($Build.Toolset) build failed; see $($Build.Started.LogPath)"
            if ($VerificationExitCode -eq 0) { $VerificationExitCode = $ExitCode }
            continue
        }

        # Each test process uses its own scratch directories, and the slow suite mostly waits on other processes, so the suites run side by side.
        $TestsPath = Join-Path $RepositoryRoot "$($Build.Binaries)/HertaTests.exe"
        foreach ($Suite in $Build.TestSuites) {
            $Tests += Start-LoggedProcess -FilePath $TestsPath -Arguments $Suite.Arguments -LogPath (Join-Path $LogDirectory "$($Build.Toolset)-$Configuration.$($Suite.Name).tests.log")
        }
    }

    foreach ($Test in $Tests) {
        $ExitCode = Wait-LoggedProcess $Test
        if ($ExitCode -ne 0) {
            Write-Host "Tests failed; see $($Test.LogPath)"
            Select-String -LiteralPath $Test.LogPath -Pattern 'ERROR:' -Context 6, 3 | Select-Object -First 3 | Out-String | Write-Host
            if ($VerificationExitCode -eq 0) { $VerificationExitCode = $ExitCode }
        }
    }
}
finally {
    Pop-Location
}

if ($VerificationExitCode -eq 0) {
    Write-Host "ClangCL and $MSVCToolset builds and tests passed ($Configuration)."
}
else {
    Write-Error "Compiler verification failed. See logs in '$LogDirectory'." -ErrorAction Continue
}
exit $VerificationExitCode
