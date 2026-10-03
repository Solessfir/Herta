$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

. (Join-Path (Split-Path -Parent $PSScriptRoot) 'DependencyLock.ps1')

$TemporaryRoot = Join-Path ([System.IO.Path]::GetTempPath()) "HertaDependencyLockTests-$([System.Guid]::NewGuid().ToString('N'))"
[System.IO.Directory]::CreateDirectory($TemporaryRoot) | Out-Null
$TestCount = 0

function Write-TestLock {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string] $Content,
        [bool] $WithBom = $false
    )

    $Path = Join-Path $TemporaryRoot "$Name.lock"
    $Encoding = [System.Text.UTF8Encoding]::new($WithBom)
    [System.IO.File]::WriteAllText($Path, $Content, $Encoding)
    return $Path
}

function Assert-ParseSucceeds {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string] $Content,
        [int] $ExpectedCount,
        [bool] $WithBom = $false
    )

    $script:TestCount++
    $Path = Write-TestLock -Name $Name -Content $Content -WithBom $WithBom
    $Dependencies = @(Read-HertaDependencyLock -Path $Path)
    if ($Dependencies.Count -ne $ExpectedCount) {
        throw "$Name expected $ExpectedCount dependencies, received $($Dependencies.Count)."
    }
}

function Assert-ParseFails {
    param(
        [Parameter(Mandatory)][string] $Name,
        [Parameter(Mandatory)][string] $Content,
        [Parameter(Mandatory)][string] $ExpectedMessage
    )

    $script:TestCount++
    $Path = Write-TestLock -Name $Name -Content $Content
    try {
        Read-HertaDependencyLock -Path $Path | Out-Null
    }
    catch {
        if ($_.Exception.Message.IndexOf($ExpectedMessage, [System.StringComparison]::Ordinal) -lt 0) {
            throw "$Name failed with an unexpected message: $($_.Exception.Message)"
        }
        return
    }

    throw "$Name unexpectedly parsed successfully."
}

$Sha = '0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef'
$Header = "HERTA_DEPENDENCIES_V1`n"
$Entry = "premake|tool|windows-x64|1.0|BSD-3-Clause|https://example.com/premake.zip|$Sha|premake5.exe"

try {
    $CrLfValid = ($Header + "# comment`n`n" + $Entry).Replace("`r`n", "`n").Replace("`n", "`r`n")
    Assert-ParseSucceeds -Name 'valid-bom-crlf' -Content $CrLfValid -ExpectedCount 1 -WithBom $true
    Assert-ParseSucceeds -Name 'case-sensitive-keys' -Content ($Header + $Entry + "`nPremake|tool|windows-x64|1.0|BSD-3-Clause|https://example.com/other.zip|$Sha|other.exe") -ExpectedCount 2

    Assert-ParseFails -Name 'missing-schema' -Content '# comment' -ExpectedMessage 'does not contain schema'
    Assert-ParseFails -Name 'unsupported-schema' -Content "HERTA_DEPENDENCIES_V2`n$Entry" -ExpectedMessage 'Unsupported dependency lock schema'
    Assert-ParseFails -Name 'wrong-field-count' -Content ($Header + "$Entry|extra") -ExpectedMessage 'exactly eight'
    Assert-ParseFails -Name 'empty-field' -Content ($Header + "premake|tool||1.0|BSD-3-Clause|https://example.com/premake.zip|$Sha|premake5.exe") -ExpectedMessage 'empty field'
    Assert-ParseFails -Name 'invalid-kind' -Content ($Header + "premake|library|windows-x64|1.0|BSD-3-Clause|https://example.com/premake.zip|$Sha|premake5.exe") -ExpectedMessage 'unknown kind'
    Assert-ParseFails -Name 'invalid-sha' -Content ($Header + "premake|tool|windows-x64|1.0|BSD-3-Clause|https://example.com/premake.zip|invalid|premake5.exe") -ExpectedMessage 'invalid SHA-256'
    Assert-ParseFails -Name 'non-https-url' -Content ($Header + "premake|tool|windows-x64|1.0|BSD-3-Clause|http://example.com/premake.zip|$Sha|premake5.exe") -ExpectedMessage 'absolute HTTPS URL'
    Assert-ParseFails -Name 'uppercase-https-url' -Content ($Header + "premake|tool|windows-x64|1.0|BSD-3-Clause|HTTPS://example.com/premake.zip|$Sha|premake5.exe") -ExpectedMessage 'absolute HTTPS URL'
    Assert-ParseFails -Name 'whitespace-url' -Content ($Header + "premake|tool|windows-x64|1.0|BSD-3-Clause|https://example.com/premake file.zip|$Sha|premake5.exe") -ExpectedMessage 'absolute HTTPS URL'
    Assert-ParseFails -Name 'rooted-entry' -Content ($Header + "premake|tool|windows-x64|1.0|BSD-3-Clause|https://example.com/premake.zip|$Sha|C:\premake5.exe") -ExpectedMessage 'unsafe installed entry'
    Assert-ParseFails -Name 'parent-entry' -Content ($Header + "premake|tool|windows-x64|1.0|BSD-3-Clause|https://example.com/premake.zip|$Sha|bin/../premake5.exe") -ExpectedMessage 'unsafe installed entry'
    Assert-ParseFails -Name 'duplicate-key' -Content ($Header + $Entry + "`n$Entry") -ExpectedMessage 'duplicates'

    Write-Host "Passed $TestCount dependency lock parser tests."
}
finally {
    if (Test-Path -LiteralPath $TemporaryRoot) {
        Remove-Item -LiteralPath $TemporaryRoot -Recurse -Force
    }
}
