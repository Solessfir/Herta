$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$PackageScript = Join-Path (Split-Path -Parent $PSScriptRoot) 'PackageEditor.ps1'
$TemporaryRoot = Join-Path ([IO.Path]::GetTempPath()) "HertaPackageTests-$([Guid]::NewGuid().ToString('N'))"
$FixtureRoot = Join-Path $TemporaryRoot 'Repository'
$TestCount = 0

function Write-FixtureFile {
    param([string]$Path, [string]$Content = 'fixture')

    [IO.Directory]::CreateDirectory((Split-Path -Parent $Path)) | Out-Null
    [IO.File]::WriteAllText($Path, $Content)
}

function Assert-PackageFails {
    param([hashtable]$Arguments, [string]$ExpectedMessage)

    $script:TestCount++
    try {
        & $PackageScript @Arguments | Out-Null
    }
    catch {
        if (-not $_.Exception.Message.Contains($ExpectedMessage)) {
            throw "Unexpected packaging failure: $($_.Exception.Message)"
        }
        return
    }
    throw "Packaging unexpectedly succeeded: $ExpectedMessage"
}

function Assert-ZipPayload {
    param([string]$OutputDirectory)

    $script:TestCount++
    $PackageName = 'Herta-windows-x86_64-Shipping'
    $Archive = [IO.Compression.ZipFile]::OpenRead((Join-Path $OutputDirectory "$PackageName.zip"))
    try {
        $Entries = @($Archive.Entries | ForEach-Object { $_.FullName.Replace('\', '/') })
        $Expected = @('LICENSE', 'README.txt') + $ContentFiles
        $Expected += $NoticeFiles | ForEach-Object { "ThirdPartyLicenses/$_" }
        $Expected += $WindowsPayload
        foreach ($Path in $Expected) {
            if ("$PackageName/$Path" -notin $Entries) {
                throw "Archive is missing $Path."
            }
        }
        foreach ($Path in $ExcludedFiles) {
            if ("$PackageName/$Path" -in $Entries) {
                throw "Archive includes excluded file $Path."
            }
        }
    }
    finally {
        $Archive.Dispose()
    }
}

try {
    [IO.Directory]::CreateDirectory($FixtureRoot) | Out-Null
    & git -C $FixtureRoot init --quiet
    if ($LASTEXITCODE -ne 0) { throw 'Could not initialize fixture repository.' }

    $ContentFiles = @(
        'Engine/Content/Editor/Fonts/Roboto/Roboto-Regular.ttf',
        'Engine/Content/Editor/Fonts/Roboto/Roboto-Medium.ttf',
        'Engine/Content/Editor/Fonts/Roboto/OFL.txt',
        'Engine/Content/Editor/Fonts/DroidSansMono/DroidSansMono.ttf',
        'Engine/Content/Editor/Fonts/DroidSansMono/LICENSE.txt',
        'Engine/Content/Shapes/Cube.gltf',
        'Engine/Content/Shapes/Cube.gltf.hmeta',
        'Games/Sandbox/Content/Tracked.gltf',
        'Games/Sandbox/Content/Tracked.gltf.hmeta'
    )
    $NoticeFiles = @(
        'External/example/LICENSE',
        'External/example/COPYING',
        'External/example/NOTICE.txt',
        'External/example/OFL.txt',
        'External/example/FTL.txt',
        'External/spdlog/include/spdlog/fmt/bundled/format.h',
        'External/freetype/src/gzip/zlib.h',
        'External/freetype/src/autofit/ft-hb-ft.c',
        'External/freetype/src/autofit/ft-hb-decls.h',
        'External/freetype/src/autofit/ft-hb-types.h',
        'External/freetype/src/autofit/hb-script-list.h'
    )
    $WindowsPayload = @('HertaEditor.exe', 'HertaEditorCmd.exe', 'HertaAssetWorker.exe') | ForEach-Object { "Binaries/windows/x86_64/Shipping/$_" }
    foreach ($Shader in @('TexturedMesh', 'DebugDraw', 'WorldGrid')) {
        foreach ($Stage in @('vert', 'frag')) {
            $WindowsPayload += "Binaries/windows/x86_64/Shipping/Shaders/$Shader.$Stage.hshader"
        }
    }
    $ExcludedFiles = @(
        'Binaries/windows/x86_64/Shipping/HertaShaderWorker.exe',
        'Binaries/windows/x86_64/Shipping/HertaTests.exe',
        'SDK/Windows/Vulkan/sdk.dll',
        'External/example/source.cpp',
        'Binaries/windows/x86_64/Shipping/msvcp140.dll',
        'Binaries/windows/x86_64/Shipping/vcruntime140.dll',
        'Engine/Content/Untracked.gltf',
        'Games/Sandbox/Content/Untracked.gltf'
    )
    foreach ($Path in (@('LICENSE') + $ContentFiles + $NoticeFiles + $WindowsPayload + $ExcludedFiles[0..5])) {
        Write-FixtureFile (Join-Path $FixtureRoot $Path)
    }
    & git -C $FixtureRoot add --all
    if ($LASTEXITCODE -ne 0) { throw 'Could not stage fixture repository.' }

    & git -C $FixtureRoot -c user.name=Fixture -c user.email=fixture@example.invalid -c commit.gpgsign=false commit --quiet -m Fixture
    if ($LASTEXITCODE -ne 0) { throw 'Could not commit fixture repository.' }
    foreach ($Path in $ExcludedFiles[6..7]) {
        Write-FixtureFile (Join-Path $FixtureRoot $Path)
    }
    $Arguments = @{ RepositoryRoot = $FixtureRoot; OutputDirectory = (Join-Path $TemporaryRoot 'Windows') }
    & $PackageScript @Arguments | Out-Null
    Assert-ZipPayload $Arguments.OutputDirectory
    Assert-PackageFails $Arguments 'Package output already exists'

    $Arguments.OutputDirectory = Join-Path $TemporaryRoot 'DirectoryExists'
    [IO.Directory]::CreateDirectory((Join-Path $Arguments.OutputDirectory 'Herta-windows-x86_64-Shipping')) | Out-Null
    Assert-PackageFails $Arguments 'Package output already exists'

    $Arguments.OutputDirectory = Join-Path $TemporaryRoot 'ArchiveExists'
    Write-FixtureFile (Join-Path $Arguments.OutputDirectory 'Herta-windows-x86_64-Shipping.zip') 'do not overwrite'
    Assert-PackageFails $Arguments 'Package output already exists'
    if ([IO.File]::ReadAllText((Join-Path $Arguments.OutputDirectory 'Herta-windows-x86_64-Shipping.zip')) -ne 'do not overwrite') {
        throw 'Existing archive was modified.'
    }

    $Arguments.OutputDirectory = Join-Path $TemporaryRoot 'MissingInput'
    foreach ($Path in @($WindowsPayload[-1], 'Engine/Content/Editor/Fonts/Roboto/OFL.txt', 'Games/Sandbox/Content/Tracked.gltf', 'External/example/LICENSE')) {
        $MissingPath = Join-Path $FixtureRoot $Path
        [IO.File]::Move($MissingPath, "$MissingPath.saved")
        Assert-PackageFails $Arguments 'Required package file is missing'
        [IO.File]::Move("$MissingPath.saved", $MissingPath)
        if (Test-Path -LiteralPath (Join-Path $Arguments.OutputDirectory 'Herta-windows-x86_64-Shipping')) {
            throw 'Missing input created a partial package.'
        }
    }

    Write-Output "Passed $TestCount editor packaging regression tests."
}
finally {
    $ResolvedTemporaryRoot = [IO.Path]::GetFullPath($TemporaryRoot)
    $TemporaryParent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd([IO.Path]::DirectorySeparatorChar)
    if ((Split-Path -Parent $ResolvedTemporaryRoot) -ne $TemporaryParent -or (Split-Path -Leaf $ResolvedTemporaryRoot) -notlike 'HertaPackageTests-*') {
        throw 'Refusing to remove an unexpected fixture directory.'
    }
    if (Test-Path -LiteralPath $ResolvedTemporaryRoot) {
        Remove-Item -LiteralPath $ResolvedTemporaryRoot -Recurse -Force
    }
}
