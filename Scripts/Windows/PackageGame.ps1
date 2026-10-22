param(
    [string]$RepositoryRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string]$Project = 'Games/Sandbox/Sandbox.hertaproject',
    [ValidateSet('Development', 'Shipping')]
    [string]$Configuration = 'Shipping',
    [string]$OutputDirectory = '',
    # Copies the package outside the checkout and plays a scripted run there, proving it needs nothing from the source tree.
    [switch]$Verify
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $RepositoryRoot 'Intermediate/Packages'
}

$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$ProjectPath = (Resolve-Path -LiteralPath (Join-Path $RepositoryRoot $Project)).Path
$ProjectName = [IO.Path]::GetFileNameWithoutExtension($ProjectPath)
$PackageName = "$ProjectName-windows-x86_64-$Configuration"
$PackageRoot = Join-Path $OutputDirectory $PackageName
$ArchivePath = Join-Path $OutputDirectory "$PackageName.zip"
$BinaryDirectory = Join-Path $RepositoryRoot "Binaries/windows/x86_64/$Configuration"

if ((Test-Path -LiteralPath $PackageRoot) -or (Test-Path -LiteralPath $ArchivePath)) {
    throw "Package output already exists. Choose an empty output directory: $OutputDirectory"
}

foreach ($Program in @('HertaGame.exe', 'HertaEditorCmd.exe', 'HertaAssetWorker.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $BinaryDirectory $Program) -PathType Leaf)) {
        throw "Build $Configuration before packaging; missing $Program in $BinaryDirectory"
    }
}

$Shaders = @(Get-ChildItem -LiteralPath (Join-Path $BinaryDirectory 'Shaders') -Filter '*.hshader' -File -ErrorAction SilentlyContinue)
if ($Shaders.Count -eq 0) {
    throw "Cooked shaders are missing from $BinaryDirectory/Shaders"
}

# Staging cooks the starting level's dependency closure through the asset worker and copies the project and level verbatim.
& (Join-Path $BinaryDirectory 'HertaEditorCmd.exe') game.stage $ProjectPath $PackageRoot
if ($LASTEXITCODE -ne 0) {
    throw 'Staging the game content failed.'
}

Copy-Item -LiteralPath (Join-Path $BinaryDirectory 'HertaGame.exe') -Destination (Join-Path $PackageRoot "$ProjectName.exe")
New-Item -ItemType Directory -Force -Path (Join-Path $PackageRoot 'Shaders') | Out-Null
foreach ($Shader in $Shaders) {
    Copy-Item -LiteralPath $Shader.FullName -Destination (Join-Path $PackageRoot "Shaders/$($Shader.Name)")
}

$TrackedFiles = & git -c core.quotepath=false -C $RepositoryRoot ls-files --recurse-submodules
if ($LASTEXITCODE -ne 0) {
    throw 'Could not enumerate third-party license files.'
}

$LicenseFiles = @($TrackedFiles | Where-Object {
    $_ -like 'External/*' -and [IO.Path]::GetFileName($_) -match '(?i)(license|copying|notice|^ofl\.|^ftl\.)'
})
# These dependencies embed their notices in source files rather than separate license files.
$LicenseFiles += @(
    'External/spdlog/include/spdlog/fmt/bundled/format.h',
    'External/freetype/src/gzip/zlib.h'
)

Copy-Item -LiteralPath (Join-Path $RepositoryRoot 'LICENSE') -Destination (Join-Path $PackageRoot 'LICENSE')
foreach ($RelativePath in ($LicenseFiles | Sort-Object -Unique)) {
    $Destination = Join-Path $PackageRoot "ThirdPartyLicenses/$RelativePath"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    Copy-Item -LiteralPath (Join-Path $RepositoryRoot $RelativePath) -Destination $Destination
}

$Revision = & git -C $RepositoryRoot rev-parse HEAD
@"
$ProjectName - Herta game, Windows x86_64 $Configuration
Revision: $Revision

Run $ProjectName.exe. Settings and logs are kept per user in %LOCALAPPDATA%\Herta\$ProjectName.
A Vulkan-capable GPU, driver, and system Vulkan loader are required.

WASD moves, Space jumps, Shift sprints, R restarts the course, Esc opens the menu, and F11 or Alt+Enter toggles fullscreen.

Herta is licensed under MIT. See LICENSE and ThirdPartyLicenses; the HUD font license is in Engine/Content/Fonts.
This software is based in part on the work of the FreeType Team (https://freetype.org).
"@ | Set-Content -LiteralPath (Join-Path $PackageRoot 'README.txt') -Encoding UTF8

if ($Verify) {
    $Relocated = Join-Path ([IO.Path]::GetTempPath()) "HertaGameVerify-$([guid]::NewGuid())"
    try {
        Copy-Item -LiteralPath $PackageRoot -Destination $Relocated -Recurse
        $Process = Start-Process -FilePath (Join-Path $Relocated "$ProjectName.exe") -ArgumentList '--smoke-test=600' -WorkingDirectory $Relocated -PassThru -Wait -NoNewWindow
        if ($Process.ExitCode -ne 0) {
            throw "The relocated game failed its scripted run with exit code $($Process.ExitCode); see %LOCALAPPDATA%\Herta\$ProjectName\Logs."
        }

        Write-Output "Relocated game passed its scripted run from $Relocated"
    }
    finally {
        Remove-Item -LiteralPath $Relocated -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Compress-Archive -LiteralPath $PackageRoot -DestinationPath $ArchivePath
Write-Output "Packaged game: $ArchivePath"
