param(
    [string]$RepositoryRoot = (Split-Path -Parent (Split-Path -Parent $PSScriptRoot)),
    # The .hertaproject to package, relative to the repository or absolute, such as Games/Sandbox/Sandbox.hertaproject.
    [Parameter(Mandatory = $true)]
    [string]$Project,
    [ValidateSet('Development', 'Shipping')]
    [string]$Configuration = 'Shipping',
    [string]$OutputDirectory = '',
    # Loose stages the content as files, Package puts it in <Project>.hpak beside the executables, and Embedded builds that package into them.
    [ValidateSet('Loose', 'Package', 'Embedded')]
    [string]$ContentMode = 'Package',
    # A private package signing key from HertaCooker keygen, kept outside the checkout; its public half belongs in Config/PackagePublicKeys.txt.
    [string]$SigningKey = '',
    # Ships each translation as its own <Project>-<culture>.hpak beside the base package instead of inside it.
    [switch]$LanguagePacks,
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

foreach ($Program in @('HertaGame.exe', 'HertaServer.exe', 'HertaCooker.exe', 'HertaAssetWorker.exe', 'HertaShaderWorker.exe')) {
    if (-not (Test-Path -LiteralPath (Join-Path $BinaryDirectory $Program) -PathType Leaf)) {
        throw "Build $Configuration before packaging; missing $Program in $BinaryDirectory"
    }
}

# The project's game modules, built for this configuration beside the project, ship beside the executables, where the game loads them.
$Descriptor = Get-Content -LiteralPath $ProjectPath -Raw | ConvertFrom-Json
$ModuleFiles = @(foreach ($Module in $Descriptor.modules) {
    $Library = Join-Path (Split-Path -Parent $ProjectPath) "Binaries/windows/x86_64/$Configuration/$($Module.name).dll"
    if (-not (Test-Path -LiteralPath $Library -PathType Leaf)) {
        throw "Build the project's $($Module.name) module for $Configuration before packaging: generate with --project=$Project and build $($Module.name)"
    }
    $Library
})

$Shaders = @(Get-ChildItem -LiteralPath (Join-Path $BinaryDirectory 'Shaders') -Filter '*.hshader' -File -ErrorAction SilentlyContinue)
if ($Shaders.Count -eq 0) {
    throw "Cooked shaders are missing from $BinaryDirectory/Shaders"
}

$GameExecutable = Join-Path $PackageRoot "$ProjectName.exe"
$ServerExecutable = Join-Path $PackageRoot "$ProjectName-Server.exe"
$Cooker = Join-Path $BinaryDirectory 'HertaCooker.exe'
$SigningArguments = @()
if (-not [string]::IsNullOrWhiteSpace($SigningKey)) {
    $SigningArguments = @("--signing-key=$((Resolve-Path -LiteralPath $SigningKey).Path)")
}

# Cooking stages the starting level's dependency closure through the asset worker, then writes it loose, as a package with the engine
# shaders, or as that package embedded in both executables.
function Invoke-Cook {
    param([string]$Output, [string]$Mode, [string[]]$Extra)
    & $Cooker cook "--project=$ProjectPath" "--output=$Output" "--mode=$Mode" "--name=$ProjectName" "--shaders=$(Join-Path $BinaryDirectory 'Shaders')" @Extra @SigningArguments
    if ($LASTEXITCODE -ne 0) {
        throw 'Cooking the game content failed.'
    }
}

# Get-FileHash is missing from some PowerShell sessions, so hash through .NET as Setup does.
function Get-Sha256 {
    param([string]$Path)
    $Stream = [System.IO.File]::OpenRead($Path)
    $Hasher = [System.Security.Cryptography.SHA256]::Create()
    try {
        return [BitConverter]::ToString($Hasher.ComputeHash($Stream)).Replace('-', '')
    }
    finally {
        $Hasher.Dispose()
        $Stream.Dispose()
    }
}

New-Item -ItemType Directory -Force -Path $PackageRoot | Out-Null
Copy-Item -LiteralPath (Join-Path $BinaryDirectory 'HertaGame.exe') -Destination $GameExecutable
Copy-Item -LiteralPath (Join-Path $BinaryDirectory 'HertaServer.exe') -Destination $ServerExecutable
foreach ($Library in $ModuleFiles) {
    Copy-Item -LiteralPath $Library -Destination $PackageRoot
}
$PackExtra = @(if ($LanguagePacks) { '--language-packs' })
switch ($ContentMode) {
    'Loose' {
        Invoke-Cook -Output $PackageRoot -Mode 'loose' -Extra @()
        New-Item -ItemType Directory -Force -Path (Join-Path $PackageRoot 'Shaders') | Out-Null
        foreach ($Shader in $Shaders) {
            Copy-Item -LiteralPath $Shader.FullName -Destination (Join-Path $PackageRoot "Shaders/$($Shader.Name)")
        }
    }
    'Package' { Invoke-Cook -Output $PackageRoot -Mode 'package' -Extra $PackExtra }
    'Embedded' { Invoke-Cook -Output $PackageRoot -Mode 'embedded' -Extra (@("--embed-into=$GameExecutable,$ServerExecutable") + $PackExtra) }
}

$TrackedFiles = & git -c core.quotepath=false -C $RepositoryRoot ls-files --recurse-submodules
if ($LASTEXITCODE -ne 0) {
    throw 'Could not enumerate third-party license files.'
}

$LicenseFiles = @($TrackedFiles | Where-Object {
    $_ -like 'External/*' -and [IO.Path]::GetFileName($_) -match '(?i)(license|licence|copying|notice|^ofl\.|^ftl\.)'
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

WASD moves, Space jumps and wall jumps, Shift sprints, C or Ctrl crouches and slides, R restarts the course, Esc opens the menu, and F11 or Alt+Enter toggles fullscreen.

Multiplayer: $ProjectName.exe --host[=port] hosts a game and plays it; others join with $ProjectName.exe --connect=address:port and an optional --name=Name.
$ProjectName-Server.exe runs a dedicated server without a window, GPU, or audio device; it takes --port=, which defaults to 27015, and --max-players=.

Herta is licensed under MIT. See LICENSE and ThirdPartyLicenses; the HUD font licenses are in Engine/Content/Fonts.
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

        # The dedicated server must start from the package alone, and a hosted game must play through its own loopback client.
        $Server = Start-Process -FilePath (Join-Path $Relocated "$ProjectName-Server.exe") -ArgumentList '--port=0', '--ticks=120' -WorkingDirectory $Relocated -PassThru -Wait -NoNewWindow
        if ($Server.ExitCode -ne 0) {
            throw "The relocated dedicated server failed with exit code $($Server.ExitCode); see $Relocated\Saved\Logs\Server.log."
        }

        $Hosted = Start-Process -FilePath (Join-Path $Relocated "$ProjectName.exe") -ArgumentList '--smoke-test=300', '--host=27315' -WorkingDirectory $Relocated -PassThru -Wait -NoNewWindow
        if ($Hosted.ExitCode -ne 0) {
            throw "The relocated game failed its hosted run with exit code $($Hosted.ExitCode); see %LOCALAPPDATA%\Herta\$ProjectName\Logs."
        }

        Write-Output "Relocated dedicated server started and a hosted game played from $Relocated"

        # The same sources must package to the same bytes, which signed packages keep too: Ed25519 signatures are deterministic.
        if ($ContentMode -eq 'Package') {
            $Rebuild = Join-Path $Relocated 'Rebuild'
            Invoke-Cook -Output $Rebuild -Mode 'package' -Extra $PackExtra
            $First = Get-Sha256 (Join-Path $PackageRoot "$ProjectName.hpak")
            $Second = Get-Sha256 (Join-Path $Rebuild "$ProjectName.hpak")
            if ($First -ne $Second) {
                throw "Packaging the same content twice gave different bytes: $First and $Second"
            }

            Write-Output "The package rebuilt byte for byte: $First"
        }
    }
    finally {
        Remove-Item -LiteralPath $Relocated -Recurse -Force -ErrorAction SilentlyContinue
    }
}

Compress-Archive -LiteralPath $PackageRoot -DestinationPath $ArchivePath
Write-Output "Packaged game: $ArchivePath"
