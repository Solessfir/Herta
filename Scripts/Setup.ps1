[CmdletBinding()]
param(
    [switch] $PrintPremakePath,
    [switch] $ValidateOnly
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

    $InstallDirectory = Join-Path $RepositoryRoot "SDK/Windows/Premake/$($Dependency.Version)"
    return [pscustomobject]@{
        InstallDirectory = $InstallDirectory
        Executable = Join-Path $InstallDirectory $Dependency.InstalledEntry
        Archive = Join-Path $RepositoryRoot "SDK/.Downloads/$([System.IO.Path]::GetFileName($Dependency.Url))"
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

function Find-SupportedMSBuild {
    $PathMSBuild = Get-Command MSBuild.exe -ErrorAction SilentlyContinue
    if ($null -ne $PathMSBuild) {
        return $PathMSBuild.Source
    }

    $VsWherePath = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    if (Test-Path -LiteralPath $VsWherePath -PathType Leaf) {
        $InstallationPath = (& $VsWherePath -latest -products '*' -version '[17.0,19.0)' -requires Microsoft.Component.MSBuild -property installationPath | Select-Object -First 1)
        if ($LASTEXITCODE -eq 0 -and -not [string]::IsNullOrWhiteSpace($InstallationPath)) {
            $MSBuildPath = Join-Path $InstallationPath.Trim() 'MSBuild/Current/Bin/MSBuild.exe'
            $CompilerPath = Get-ChildItem -LiteralPath (Join-Path $InstallationPath.Trim() 'VC/Tools/MSVC') -Filter cl.exe -File -Recurse -ErrorAction SilentlyContinue |
                Where-Object { $_.FullName -match '[\\/]bin[\\/]Hostx64[\\/]x64[\\/]cl\.exe$' } |
                Sort-Object FullName -Descending |
                Select-Object -First 1 -ExpandProperty FullName
            if ((Test-Path -LiteralPath $MSBuildPath -PathType Leaf) -and -not [string]::IsNullOrWhiteSpace($CompilerPath)) {
                return $MSBuildPath
            }
        }
    }

    throw 'Visual Studio Build Tools 2026 or 2022 with the Desktop development with C++ workload is required.'
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

$MSBuildPath = Find-SupportedMSBuild
Write-Host "MSBuild: $MSBuildPath"
Initialize-GitSubmodules
Install-Premake -Dependency $Premake -Paths $PremakePaths
Remove-DownloadedArchive -ArchivePath $PremakePaths.Archive

Write-Host "Premake $($Premake.Version): $($PremakePaths.Executable)"
Write-BlenderStatus
Write-Host 'Herta setup completed.'
