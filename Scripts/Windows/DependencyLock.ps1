Set-StrictMode -Version Latest

$HertaDependencyLockSchema = 'HERTA_DEPENDENCIES_V1'

function Read-HertaDependencyLock {
    param([Parameter(Mandatory)][string] $Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Dependency lock file not found: $Path"
    }

    $Dependencies = [System.Collections.Generic.List[object]]::new()
    $Keys = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
    $bFoundSchema = $false
    $LineNumber = 0

    foreach ($RawLine in [System.IO.File]::ReadAllLines($Path, [System.Text.Encoding]::UTF8)) {
        $LineNumber++
        $Line = $RawLine
        if ($Line.Length -eq 0 -or $Line.StartsWith('#', [System.StringComparison]::Ordinal)) {
            continue
        }

        if (-not $bFoundSchema) {
            if (-not $Line.Equals($HertaDependencyLockSchema, [System.StringComparison]::Ordinal)) {
                throw "Unsupported dependency lock schema '$Line' at line $LineNumber."
            }

            $bFoundSchema = $true
            continue
        }

        $Fields = $Line.Split([char]'|')
        if ($Fields.Count -ne 8) {
            throw "Dependency line $LineNumber must contain exactly eight pipe-delimited fields."
        }

        foreach ($Field in $Fields) {
            if ($Field.Length -eq 0) {
                throw "Dependency line $LineNumber contains an empty field."
            }
        }

        $Name, $Kind, $Platform, $Version, $License, $Url, $Sha256, $InstalledEntry = $Fields
        if (-not ($Kind.Equals('tool', [System.StringComparison]::Ordinal) -or
                $Kind.Equals('sdk', [System.StringComparison]::Ordinal))) {
            throw "Dependency line $LineNumber has unknown kind '$Kind'."
        }
        if ($Sha256 -cnotmatch '^[0-9a-fA-F]{64}$') {
            throw "Dependency line $LineNumber has an invalid SHA-256 value."
        }

        $ParsedUrl = $null
        if ($Url -cmatch '\s' -or
            -not $Url.StartsWith('https://', [System.StringComparison]::Ordinal) -or
            -not [System.Uri]::TryCreate($Url, [System.UriKind]::Absolute, [ref] $ParsedUrl) -or
            -not $ParsedUrl.Scheme.Equals('https', [System.StringComparison]::Ordinal)) {
            throw "Dependency line $LineNumber must use an absolute HTTPS URL."
        }

        $bRootedPath = $InstalledEntry -cmatch '^(?:[\\/]|[A-Za-z]:)'
        $bParentTraversal = $InstalledEntry -cmatch '(?:^|[\\/])\.\.(?:[\\/]|$)'
        if ($bRootedPath -or $bParentTraversal) {
            throw "Dependency line $LineNumber has an unsafe installed entry path."
        }

        $Key = "$Name|$Platform"
        if (-not $Keys.Add($Key)) {
            throw "Dependency line $LineNumber duplicates '$Key'."
        }

        $Dependencies.Add([pscustomobject]@{
            Name = $Name
            Kind = $Kind
            Platform = $Platform
            Version = $Version
            License = $License
            Url = $Url
            Sha256 = $Sha256.ToLowerInvariant()
            InstalledEntry = $InstalledEntry
        })
    }

    if (-not $bFoundSchema) {
        throw "Dependency lock file does not contain schema '$HertaDependencyLockSchema'."
    }

    return $Dependencies
}
