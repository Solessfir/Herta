[CmdletBinding(SupportsShouldProcess)]
param()

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepositoryRoot = [System.IO.Path]::GetFullPath((Split-Path -Parent $PSScriptRoot))
$RequiredMarkers = @('premake5.lua', 'Config/Dependencies.lock')

foreach ($Marker in $RequiredMarkers) {
    if (-not (Test-Path -LiteralPath (Join-Path $RepositoryRoot $Marker))) {
        throw "Cleanup could not validate the Herta repository root: $RepositoryRoot"
    }
}

function Remove-HertaPath {
    [CmdletBinding(SupportsShouldProcess)]
    param([Parameter(Mandatory)][string] $Path)

    $ResolvedPath = [System.IO.Path]::GetFullPath($Path)
    $RepositoryPrefix = $RepositoryRoot.TrimEnd([System.IO.Path]::DirectorySeparatorChar) + [System.IO.Path]::DirectorySeparatorChar
    if (-not $ResolvedPath.StartsWith($RepositoryPrefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Cleanup target is outside the Herta repository: $ResolvedPath"
    }

    if (-not (Test-Path -LiteralPath $ResolvedPath)) {
        return
    }

    $RelativePath = $ResolvedPath.Substring($RepositoryPrefix.Length)
    if ($PSCmdlet.ShouldProcess($RelativePath, 'Remove generated Herta state')) {
        Remove-Item -LiteralPath $ResolvedPath -Recurse -Force
        Write-Host "Removed $RelativePath"
    }
}

$GeneratedDirectories = @(
    'Binaries',
    'DerivedDataCache',
    'Intermediate',
    'Saved',
    'SDK',
    'External/Premake',
    'TestResults',
    '.idea',
    '.vs'
)

foreach ($Directory in $GeneratedDirectories) {
    Remove-HertaPath -Path (Join-Path $RepositoryRoot $Directory)
}

$GeneratedRootFiles = @(
    '.run/HertaEditor.run.xml',
    'Makefile',
    'compile_commands.json',
    '.DS_Store',
    'Desktop.ini',
    'Thumbs.db'
)
foreach ($File in $GeneratedRootFiles) {
    Remove-HertaPath -Path (Join-Path $RepositoryRoot $File)
}

$GeneratedRootPatterns = @(
    '*.code-workspace',
    '*.make',
    '*.sln',
    '*.slnx',
    '*.suo',
    '*.user',
    '*.vcxproj',
    '*.vcxproj.filters',
    '*.vcxproj.user',
    '*.workspace'
)
foreach ($Pattern in $GeneratedRootPatterns) {
    foreach ($File in Get-ChildItem -LiteralPath $RepositoryRoot -File -Filter $Pattern) {
        Remove-HertaPath -Path $File.FullName
    }
}

if ($WhatIfPreference) {
    Write-Host 'Cleanup dry run completed.'
}
else {
    Write-Host 'Herta generated state is clean.'
}
