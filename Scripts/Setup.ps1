[CmdletBinding()]
param(
    [switch] $PrintPremakePath,
    [switch] $PrintVisualStudioAction,
    [switch] $ValidateOnly,
    [ValidateSet('2022', '2026')]
    [string] $VisualStudioVersion
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepositoryRoot = Split-Path -Parent $PSScriptRoot
$LockPath = Join-Path $RepositoryRoot 'Config/Dependencies.lock'
. (Join-Path $PSScriptRoot 'DependencyLock.ps1')

function Get-PremakeDependency {
    param([Parameter(Mandatory)] $Dependencies)

    $Matches = @($Dependencies | Where-Object { $_.Name -ceq 'premake' -and $_.Platform -ceq 'windows-x64' })
    if ($Matches.Count -ne 1) {
        throw "Dependencies.lock must contain exactly one premake entry for windows-x64."
    }

    return $Matches[0]
}

function Get-PremakePaths {
    param([Parameter(Mandatory)] $Dependency)

    $InstallDirectory = Join-Path $RepositoryRoot "External/Premake/Windows/$($Dependency.Version)"
    return [pscustomobject]@{
        InstallDirectory = $InstallDirectory
        Executable = Join-Path $InstallDirectory $Dependency.InstalledEntry
        Archive = Join-Path $RepositoryRoot "External/Premake/.Downloads/$([System.IO.Path]::GetFileName($Dependency.Url))"
    }
}

function Test-PremakeExecutable {
    param(
        [Parameter(Mandatory)][string] $Executable,
        [Parameter(Mandatory)][string] $Version
    )

    if (-not (Test-Path -LiteralPath $Executable -PathType Leaf)) {
        return $false
    }

    Push-Location ([System.IO.Path]::GetTempPath())
    try {
        $VersionOutput = (& $Executable --version 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0 -or $VersionOutput -notmatch [regex]::Escape($Version)) {
            throw "Premake at '$Executable' did not report expected version '$Version'. Output: $VersionOutput"
        }
    }
    finally {
        Pop-Location
    }

    return $true
}

function Get-Sha256 {
    param([Parameter(Mandatory)][string] $Path)

    $Stream = [System.IO.File]::OpenRead($Path)
    try {
        $Hasher = [System.Security.Cryptography.SHA256]::Create()
        try {
            return ([System.BitConverter]::ToString($Hasher.ComputeHash($Stream))).Replace('-', '').ToLowerInvariant()
        }
        finally {
            $Hasher.Dispose()
        }
    }
    finally {
        $Stream.Dispose()
    }
}

function Get-VerifiedArchive {
    param(
        [Parameter(Mandatory)] $Dependency,
        [Parameter(Mandatory)][string] $ArchivePath
    )

    $ArchiveDirectory = Split-Path -Parent $ArchivePath
    [System.IO.Directory]::CreateDirectory($ArchiveDirectory) | Out-Null

    if (Test-Path -LiteralPath $ArchivePath -PathType Leaf) {
        $ExistingHash = Get-Sha256 -Path $ArchivePath
        if ($ExistingHash -eq $Dependency.Sha256) {
            return
        }

        Remove-Item -LiteralPath $ArchivePath -Force
    }

    $TemporaryArchive = "$ArchivePath.$([System.Guid]::NewGuid().ToString('N')).tmp"
    try {
        Write-Host "Downloading Premake $($Dependency.Version)..."
        Invoke-WebRequest -Uri $Dependency.Url -OutFile $TemporaryArchive -UseBasicParsing
        $DownloadedHash = Get-Sha256 -Path $TemporaryArchive
        if ($DownloadedHash -ne $Dependency.Sha256) {
            throw "Premake archive SHA-256 mismatch. Expected $($Dependency.Sha256), received $DownloadedHash."
        }

        Move-Item -LiteralPath $TemporaryArchive -Destination $ArchivePath
    }
    finally {
        if (Test-Path -LiteralPath $TemporaryArchive) {
            Remove-Item -LiteralPath $TemporaryArchive -Force
        }
    }
}

function Install-Premake {
    param(
        [Parameter(Mandatory)] $Dependency,
        [Parameter(Mandatory)] $Paths
    )

    if (Test-PremakeExecutable -Executable $Paths.Executable -Version $Dependency.Version) {
        return
    }

    Get-VerifiedArchive -Dependency $Dependency -ArchivePath $Paths.Archive

    $InstallParent = Split-Path -Parent $Paths.InstallDirectory
    [System.IO.Directory]::CreateDirectory($InstallParent) | Out-Null
    $TemporaryDirectory = Join-Path $InstallParent ".$($Dependency.Version).$([System.Guid]::NewGuid().ToString('N')).tmp"
    try {
        Expand-Archive -LiteralPath $Paths.Archive -DestinationPath $TemporaryDirectory
        $TemporaryExecutable = Join-Path $TemporaryDirectory $Dependency.InstalledEntry
        if (-not (Test-PremakeExecutable -Executable $TemporaryExecutable -Version $Dependency.Version)) {
            throw "Premake archive did not contain '$($Dependency.InstalledEntry)'."
        }

        if (Test-Path -LiteralPath $Paths.InstallDirectory) {
            throw "Premake install directory exists but is invalid: $($Paths.InstallDirectory)"
        }

        Move-Item -LiteralPath $TemporaryDirectory -Destination $Paths.InstallDirectory
    }
    finally {
        if (Test-Path -LiteralPath $TemporaryDirectory) {
            Remove-Item -LiteralPath $TemporaryDirectory -Recurse -Force
        }
    }
}

function Remove-DownloadedArchive {
    param([Parameter(Mandatory)][string] $ArchivePath)

    if (Test-Path -LiteralPath $ArchivePath -PathType Leaf) {
        Remove-Item -LiteralPath $ArchivePath -Force
    }

    $ArchiveDirectory = Split-Path -Parent $ArchivePath
    if ((Test-Path -LiteralPath $ArchiveDirectory -PathType Container) -and
        @(Get-ChildItem -LiteralPath $ArchiveDirectory -Force).Count -eq 0) {
        Remove-Item -LiteralPath $ArchiveDirectory -Force
    }
}

function Write-BlenderStatus {
    $Blender = Get-Command blender -ErrorAction SilentlyContinue
    if ($null -eq $Blender) {
        Write-Host 'Optional Blender integration: not detected'
        return
    }

    $BlenderVersion = (& $Blender.Source --version 2>&1 | Select-Object -First 1)
    Write-Host "Optional Blender integration: $BlenderVersion"
}

function Initialize-GitSubmodules {
    $Git = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($null -eq $Git) {
        throw 'Git is required by Setup. Install Git for Windows and make git.exe available on PATH.'
    }

    & $Git.Source -C $RepositoryRoot submodule sync --recursive
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to synchronize Git submodule URLs.'
    }

    & $Git.Source -C $RepositoryRoot submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) {
        throw 'Failed to initialize Git submodules. Check network access and repository credentials.'
    }
}

function Test-VisualStudioToolchain {
    param(
        [Parameter(Mandatory)][string] $MSBuildPath,
        [Parameter(Mandatory)][string] $PlatformToolset
    )

    $ProbeDirectory = Join-Path ([System.IO.Path]::GetTempPath()) "HertaToolchainProbe-$([System.Guid]::NewGuid().ToString('N'))"
    [System.IO.Directory]::CreateDirectory($ProbeDirectory) | Out-Null

    $SourcePath = Join-Path $ProbeDirectory 'Probe.cpp'
    $ProjectPath = Join-Path $ProbeDirectory 'Probe.vcxproj'
    $Source = @'
#include <expected>
#include <print>

int main()
{
    const std::expected<int, int> Value = 42;
    std::println("Herta C++23 probe");
    return Value.value() == 42 ? 0 : 1;
}
'@
    $Project = @"
<Project DefaultTargets="Build" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <ItemGroup Label="ProjectConfigurations">
    <ProjectConfiguration Include="Release|x64">
      <Configuration>Release</Configuration>
      <Platform>x64</Platform>
    </ProjectConfiguration>
  </ItemGroup>
  <PropertyGroup Label="Globals">
    <Keyword>Win32Proj</Keyword>
  </PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.Default.props" />
  <PropertyGroup Condition="'`$(Configuration)|`$(Platform)'=='Release|x64'" Label="Configuration">
    <ConfigurationType>Application</ConfigurationType>
    <PlatformToolset>$PlatformToolset</PlatformToolset>
    <UseDebugLibraries>false</UseDebugLibraries>
  </PropertyGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.props" />
  <ItemDefinitionGroup Condition="'`$(Configuration)|`$(Platform)'=='Release|x64'">
    <ClCompile>
      <LanguageStandard>stdcpp23</LanguageStandard>
      <WarningLevel>Level4</WarningLevel>
      <TreatWarningAsError>true</TreatWarningAsError>
    </ClCompile>
  </ItemDefinitionGroup>
  <ItemGroup>
    <ClCompile Include="Probe.cpp" />
  </ItemGroup>
  <Import Project="`$(VCTargetsPath)\Microsoft.Cpp.targets" />
</Project>
"@

    try {
        [System.IO.File]::WriteAllText($SourcePath, $Source, [System.Text.UTF8Encoding]::new($false))
        [System.IO.File]::WriteAllText($ProjectPath, $Project, [System.Text.UTF8Encoding]::new($false))
        $BuildOutput = (& $MSBuildPath $ProjectPath /nologo /verbosity:quiet /p:Configuration=Release /p:Platform=x64 2>&1 | Out-String).Trim()
        if ($LASTEXITCODE -ne 0) {
            Write-Verbose "Rejected MSBuild '$MSBuildPath' for $PlatformToolset. $BuildOutput"
            return $false
        }

        return $true
    }
    finally {
        Remove-Item -LiteralPath $ProbeDirectory -Recurse -Force
    }
}

function Find-SupportedMSBuild {
    param([string] $Version)

    $Versions = if ([string]::IsNullOrWhiteSpace($Version)) { @('2026', '2022') } else { @($Version) }
    $CandidatePaths = [System.Collections.Generic.List[string]]::new()

    $PathMSBuild = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($null -ne $PathMSBuild) {
        $CandidatePaths.Add($PathMSBuild.Source)
    }

    $VsWherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $VsWherePath -PathType Leaf) {
        $InstallationPaths = @(& $VsWherePath -all -products '*' -version '[17.0,19.0)' -requires Microsoft.Component.MSBuild -property installationPath)
        if ($LASTEXITCODE -eq 0) {
            foreach ($InstallationPath in $InstallationPaths) {
                if (-not [string]::IsNullOrWhiteSpace($InstallationPath)) {
                    $CandidatePaths.Add((Join-Path $InstallationPath.Trim() 'MSBuild/Current/Bin/MSBuild.exe'))
                }
            }
        }
    }

    foreach ($CandidateVersion in $Versions) {
        $PlatformToolset = if ($CandidateVersion -ceq '2026') { 'v145' } else { 'v143' }
        foreach ($CandidatePath in $CandidatePaths | Select-Object -Unique) {
            if ((Test-Path -LiteralPath $CandidatePath -PathType Leaf) -and
                (Test-VisualStudioToolchain -MSBuildPath $CandidatePath -PlatformToolset $PlatformToolset)) {
                return [pscustomobject]@{
                    Path = $CandidatePath
                    Version = $CandidateVersion
                    PlatformToolset = $PlatformToolset
                }
            }
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($Version)) {
        $PlatformToolset = if ($Version -ceq '2026') { 'v145' } else { 'v143' }
        $PackageId = if ($Version -ceq '2026') { 'Microsoft.VisualStudio.BuildTools' } else { 'Microsoft.VisualStudio.2022.BuildTools' }
        $InstallCommand = "winget install --id $PackageId --exact --override `"--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`""
        throw "Visual Studio $Version cannot build Herta with C++23 x64. Add the Desktop development with C++ workload and the $PlatformToolset MSVC toolset in Visual Studio Installer. For a new Build Tools installation: $InstallCommand"
    }

    $InstallCommand = 'winget install --id Microsoft.VisualStudio.BuildTools --exact --override "--wait --passive --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"'
    throw "Neither Visual Studio 2026 v145 nor Visual Studio 2022 v143 can build Herta with C++23 x64. Add the Desktop development with C++ workload in Visual Studio Installer. For a new Visual Studio 2026 Build Tools installation: $InstallCommand"
}

$Dependencies = Read-HertaDependencyLock -Path $LockPath
$Premake = Get-PremakeDependency -Dependencies $Dependencies
$PremakePaths = Get-PremakePaths -Dependency $Premake

if ($ValidateOnly) {
    Write-Host "Validated $($Dependencies.Count) dependency lock entries."
    exit 0
}

if ($PrintPremakePath) {
    if (-not (Test-PremakeExecutable -Executable $PremakePaths.Executable -Version $Premake.Version)) {
        throw "Premake is not installed at '$($PremakePaths.Executable)'. Run Setup.bat first."
    }

    [Console]::Out.WriteLine($PremakePaths.Executable)
    exit 0
}

if ($PrintVisualStudioAction) {
    $Toolchain = Find-SupportedMSBuild -Version $VisualStudioVersion
    [Console]::Out.WriteLine("vs$($Toolchain.Version)")
    exit 0
}

$Toolchain = Find-SupportedMSBuild -Version $VisualStudioVersion
if ([string]::IsNullOrWhiteSpace($VisualStudioVersion) -and $Toolchain.Version -ceq '2022') {
    Write-Warning 'Visual Studio 2026 v145 is unavailable. Falling back to Visual Studio 2022 v143.'
}
Write-Host "Visual Studio action: vs$($Toolchain.Version) ($($Toolchain.PlatformToolset))"
Write-Host "MSBuild: $($Toolchain.Path)"
Initialize-GitSubmodules
Install-Premake -Dependency $Premake -Paths $PremakePaths
Remove-DownloadedArchive -ArchivePath $PremakePaths.Archive

Write-Host "Premake $($Premake.Version): $($PremakePaths.Executable)"
Write-BlenderStatus
Write-Host 'Herta setup completed.'
