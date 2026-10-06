param(
    [ValidateSet('Debug', 'Development', 'Shipping')]
    [string]$Configuration = 'Shipping',
    [ValidateSet(1000, 5000, 10000)]
    [int[]]$Counts = @(1000, 5000, 10000),
    [ValidateSet('rendering', 'dynamic')]
    [string[]]$Workloads = @('rendering', 'dynamic')
)

$ErrorActionPreference = 'Stop'
$RepositoryRoot = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$Editor = Join-Path $RepositoryRoot "Binaries/windows/x86_64/$Configuration/HertaEditor.exe"
$Command = Join-Path $RepositoryRoot "Binaries/windows/x86_64/$Configuration/HertaEditorCmd.exe"

if (!(Test-Path -LiteralPath $Editor) -or !(Test-Path -LiteralPath $Command)) {
    throw "Build HertaEditor and HertaEditorCmd in $Configuration first."
}

$OutputDirectory = Join-Path $RepositoryRoot ('TestResults/Scaling/' + (Get-Date -Format 'yyyyMMdd-HHmmss-fff'))
New-Item -ItemType Directory -Path $OutputDirectory | Out-Null

foreach ($Workload in $Workloads) {
    foreach ($Count in $Counts) {
        $Name = "$Workload-$Count"
        $Level = Join-Path $OutputDirectory "$Name.hlevel"
        & $Command level.generate-scaling $Workload $Count $Level
        if ($LASTEXITCODE -ne 0) {
            throw "Could not generate $Name."
        }

        $EditorArguments = @('"--scaling-test=' + $Level + '"')
        if ($Workload -eq 'dynamic') {
            $EditorArguments += '--scaling-simulate'
        }

        $Process = Start-Process -FilePath $Editor -ArgumentList $EditorArguments -WorkingDirectory $RepositoryRoot -WindowStyle Hidden -RedirectStandardOutput (Join-Path $OutputDirectory "$Name.log") -RedirectStandardError (Join-Path $OutputDirectory "$Name.errors.log") -PassThru
        $null = $Process.Handle
        $PeakBytes = 0L
        while (!$Process.WaitForExit(250)) {
            $Process.Refresh()
            $PeakBytes = [Math]::Max($PeakBytes, $Process.PeakWorkingSet64)
        }

        $Process.WaitForExit()
        $ExitCode = $Process.ExitCode
        $Process.Dispose()
        if ($ExitCode -ne 0) {
            throw "Scaling capture $Name failed ($ExitCode). Inspect $OutputDirectory."
        }

        Write-Output "$Name peak_working_set_mib=$([Math]::Round($PeakBytes / 1MB, 2))"
        Select-String -LiteralPath (Join-Path $OutputDirectory "$Name.log") -Pattern 'Scaling phase=|Scaling metric=|Scaling GPU' | ForEach-Object { $_.Line }
    }
}

Write-Output "Scaling artifacts: $OutputDirectory"
