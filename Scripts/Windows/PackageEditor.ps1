param(
    [string]$RepositoryRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $RepositoryRoot 'Intermediate/Packages'
}

$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$PackageName = 'Herta-windows-x86_64-Shipping'
$PackageRoot = Join-Path $OutputDirectory $PackageName
$BinaryDirectory = 'Binaries/windows/x86_64/Shipping'
$ArchivePath = Join-Path $OutputDirectory "$PackageName.zip"

if ((Test-Path -LiteralPath $PackageRoot) -or (Test-Path -LiteralPath $ArchivePath)) {
    throw "Package output already exists. Choose an empty output directory: $OutputDirectory"
}

$RequiredFiles = @(
    'LICENSE',
    "$BinaryDirectory/HertaEditor.exe",
    "$BinaryDirectory/HertaEditorCmd.exe",
    "$BinaryDirectory/HertaAssetWorker.exe",
    'Engine/Content/Editor/Fonts/Roboto/Roboto-Regular.ttf',
    'Engine/Content/Editor/Fonts/Roboto/Roboto-Medium.ttf',
    'Engine/Content/Editor/Fonts/Roboto/OFL.txt',
    'Engine/Content/Editor/Fonts/DroidSansMono/DroidSansMono.ttf',
    'Engine/Content/Editor/Fonts/DroidSansMono/LICENSE.txt',
    'Engine/Content/Shapes/Cube.gltf',
    'Engine/Content/Shapes/Cube.gltf.hmeta'
)
foreach ($Shader in @('TexturedMesh', 'DebugDraw', 'WorldGrid')) {
    foreach ($Stage in @('vert', 'frag')) {
        $RequiredFiles += "$BinaryDirectory/Shaders/$Shader.$Stage.hshader"
    }
}

foreach ($RelativePath in $RequiredFiles) {
    if (-not (Test-Path -LiteralPath (Join-Path $RepositoryRoot $RelativePath) -PathType Leaf)) {
        throw "Required package file is missing: $RelativePath"
    }
}

$TrackedFiles = & git -c core.quotepath=false -C $RepositoryRoot ls-files --recurse-submodules
if ($LASTEXITCODE -ne 0) {
    throw 'Could not enumerate tracked content and third-party license files.'
}

$LicenseFiles = @($TrackedFiles | Where-Object {
    $_ -like 'External/*' -and [IO.Path]::GetFileName($_) -match '(?i)(license|copying|notice|^ofl\.|^ftl\.)'
})
# These dependencies embed their notices in source files rather than separate license files.
$LicenseFiles += @(
    'External/spdlog/include/spdlog/fmt/bundled/format.h',
    'External/freetype/src/gzip/zlib.h',
    'External/freetype/src/autofit/ft-hb-ft.c',
    'External/freetype/src/autofit/ft-hb-decls.h',
    'External/freetype/src/autofit/ft-hb-types.h',
    'External/freetype/src/autofit/hb-script-list.h'
)

$PackageFiles = $RequiredFiles + @($TrackedFiles | Where-Object {
    $_ -like 'Engine/Content/*' -or $_ -like 'Games/Sandbox/Content/*' -or $_ -like 'Games/Sandbox/Scenes/*'
})
foreach ($RelativePath in ($PackageFiles + $LicenseFiles | Sort-Object -Unique)) {
    if (-not (Test-Path -LiteralPath (Join-Path $RepositoryRoot $RelativePath) -PathType Leaf)) {
        throw "Required package file is missing: $RelativePath"
    }
}

$Revision = & git -C $RepositoryRoot rev-parse HEAD
if ($LASTEXITCODE -ne 0) {
    throw 'Could not record the package revision.'
}

foreach ($RelativePath in ($PackageFiles | Sort-Object -Unique)) {
    $Destination = Join-Path $PackageRoot $RelativePath
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    Copy-Item -LiteralPath (Join-Path $RepositoryRoot $RelativePath) -Destination $Destination
}

foreach ($RelativePath in ($LicenseFiles | Sort-Object -Unique)) {
    $Destination = Join-Path $PackageRoot "ThirdPartyLicenses/$RelativePath"
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Destination) | Out-Null
    Copy-Item -LiteralPath (Join-Path $RepositoryRoot $RelativePath) -Destination $Destination
}

New-Item -ItemType Directory -Force -Path (Join-Path $PackageRoot 'Games/Sandbox/Content') | Out-Null

@"
Herta experimental editor - Windows x86_64 Shipping
Revision: $Revision

Extract the complete archive into a writable directory.
Run $BinaryDirectory/HertaEditor.exe.
The game runtime is not implemented. Save scene edits with Ctrl+S.
A Vulkan-capable GPU, driver, and system Vulkan loader are required.
The MSVC runtime is statically linked; no separate CRT DLLs are required.
Blender is optional and only required to import .blend files.

Herta is licensed under MIT. See LICENSE and ThirdPartyLicenses.
Font licenses are included beside the fonts in Engine/Content.
This software is based in part on the work of the FreeType Team (https://freetype.org).
"@ | Set-Content -LiteralPath (Join-Path $PackageRoot 'README.txt') -Encoding UTF8

Compress-Archive -LiteralPath $PackageRoot -DestinationPath $ArchivePath
Write-Output "Packaged experimental editor: $ArchivePath"
