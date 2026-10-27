<#
.SYNOPSIS
    Generates the log fixtures used by the manual test plan in README.md.

.DESCRIPTION
    Every fixture is seeded and uses a simulated clock, so the files are the
    same on every run. Timestamps have no zone suffix and start on
    2026-03-01; slp and the GUI read them as UTC.

.PARAMETER Generator
    Path to log_generator.exe. Defaults to the release preset build.

.PARAMETER OutDir
    Directory for the fixtures. Existing fixtures there are overwritten.

.PARAMETER SkipLarge
    Skips the 1,000,000-line big.jsonl (about 200 MB).
#>
[CmdletBinding()]
param(
    [string]$Generator = (Join-Path $PSScriptRoot '..\..\build\release\bin\Release\log_generator.exe'),
    [string]$OutDir = (Join-Path $PSScriptRoot '..\..\build\manual-test'),
    [switch]$SkipLarge
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path $Generator)) {
    throw "log_generator not found at '$Generator'. Build it with: cmake --build --preset release --target log_generator"
}

function Invoke-Generator {
    & $Generator -q @args
    if ($LASTEXITCODE -ne 0) {
        throw "log_generator $($args -join ' ') failed with exit code $LASTEXITCODE"
    }
}

$OutDir = [System.IO.Path]::GetFullPath($OutDir)
New-Item -ItemType Directory -Force -Path $OutDir, (Join-Path $OutDir 'rot') | Out-Null
Remove-Item -Path (Join-Path $OutDir 'rot\app.jsonl*') -ErrorAction SilentlyContinue

$start = '2026-03-01T00:00:00Z'

Write-Host 'app.jsonl         1,000 JSON lines'
Invoke-Generator --start-time $start -n 1000 --seed 1 -o (Join-Path $OutDir 'app.jsonl')

$formats = [ordered]@{
    'logfmt'          = 'fmt_logfmt.logfmt'
    'csv'             = 'fmt_csv.csv'
    'syslog'          = 'fmt_syslog.log'
    'apache-combined' = 'fmt_apache-combined.log'
    'apache-common'   = 'fmt_apache-common.log'
    'apache-error'    = 'fmt_apache-error.log'
    'java'            = 'fmt_java.log'
}
foreach ($format in $formats.Keys) {
    Write-Host ("{0,-17} 2,000 lines" -f $formats[$format])
    Invoke-Generator -f $format --start-time $start -n 2000 --seed 2 -o (Join-Path $OutDir $formats[$format])
}

Write-Host 'day.jsonl         one simulated day, daily profile and bursts (~220k lines)'
Invoke-Generator --start-time $start -d 1d --rate 2 --bursts --seed 3 -o (Join-Path $OutDir 'day.jsonl')

Write-Host 'rot\app.jsonl     5,000 lines rotated every 1,000; 3 backups kept (lines 1000-4999 remain)'
Invoke-Generator --start-time $start -n 5000 --rate 50 --roll-lines 1000 --roll-keep 3 --seed 4 -o (Join-Path $OutDir 'rot\app.jsonl')

if (-not $SkipLarge) {
    Write-Host 'big.jsonl         1,000,000 lines (~200 MB)'
    Invoke-Generator --start-time $start -n 1M --rate 200 --seed 5 -o (Join-Path $OutDir 'big.jsonl')
}

Write-Host "Fixtures written to $OutDir"
