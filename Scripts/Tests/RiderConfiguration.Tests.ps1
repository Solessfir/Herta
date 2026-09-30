$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepositoryRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$ProfilePath = Join-Path $RepositoryRoot '.run/HertaEditor.run.xml'
[xml]$Profile = Get-Content -LiteralPath $ProfilePath -Raw
$Configuration = $Profile.component.configuration
if ($Configuration.type -ne 'CppProject' -or $Configuration.name -ne 'HertaEditor') {
    throw 'Expected the shared HertaEditor C++ launch configuration.'
}

. (Join-Path $RepositoryRoot 'Scripts/DependencyLock.ps1')
$Dependencies = Read-HertaDependencyLock -Path (Join-Path $RepositoryRoot 'Config/Dependencies.lock')
$Vulkan = $Dependencies | Where-Object { $_.Name -eq 'vulkan-sdk' -and $_.Platform -eq 'windows-x64' }
$SdkRoot = Join-Path $RepositoryRoot "SDK/Windows/Vulkan/$($Vulkan.Version)"
$ExpectedConfigurations = @('Debug', 'Debug-ASan', 'Development', 'Shipping')
for ($Index = 0; $Index -lt $ExpectedConfigurations.Count; $Index++) {
    $Name = $ExpectedConfigurations[$Index]
    $Entry = $Configuration.SelectSingleNode("configuration_$($Index + 1)")
    if ($null -eq $Entry) {
        throw "Missing Rider configuration for $Name."
    }
    $Options = @{}
    foreach ($Option in $Entry.option) {
        $Options[$Option.name] = $Option.value
    }
    if ($Options.CONFIGURATION -ne $Name -or $Options.PLATFORM -ne 'x64' -or $Options.PASS_PARENT_ENVS -ne '1') {
        throw "Rider configuration $Name does not select $Name/x64 with inherited environment variables."
    }
    if ($Options.EXE_PATH -ne "`$PROJECT_DIR`$/Binaries/windows/x86_64/$Name/HertaEditor.exe") {
        throw "Rider configuration $Name launches the wrong executable."
    }
    $ProjectPath = $Options.PROJECT_FILE_PATH.Replace('$PROJECT_DIR$', $RepositoryRoot)
    if (-not (Test-Path -LiteralPath $ProjectPath)) {
        throw "Generated C++ project is missing: $ProjectPath"
    }
    $Environment = @{}
    foreach ($Variable in $Entry.SelectNodes('envs/env')) {
        $Environment[$Variable.name] = $Variable.value
    }
    if ($Name -eq 'Shipping') {
        if ($Environment.Count -ne 0) {
            throw 'Shipping must not include Vulkan validation environment variables.'
        }
        continue
    }
    if ([IO.Path]::GetFullPath($Environment.VULKAN_SDK) -ne [IO.Path]::GetFullPath($SdkRoot)) {
        throw "Rider configuration $Name does not use the pinned project-local Vulkan SDK."
    }
    if ([IO.Path]::GetFullPath($Environment.VK_ADD_LAYER_PATH) -ne [IO.Path]::GetFullPath((Join-Path $SdkRoot 'Bin'))) {
        throw "Rider configuration $Name does not use the SDK validation layer directory."
    }
}
foreach ($File in @('VkLayer_khronos_validation.json', 'VkLayer_khronos_validation.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $SdkRoot "Bin/$File"))) {
        throw "Validation layer file is missing: $File"
    }
}
Write-Output 'Rider configuration checks passed.'
