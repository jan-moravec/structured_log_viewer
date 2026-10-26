#Requires -Version 7.4
<#
.SYNOPSIS
    Runs the slp scenarios from README.md against generated fixtures.

.DESCRIPTION
    Expects the fixtures from New-TestFixtures.ps1. Each check prints PASS,
    FAIL, or INFO; INFO rows report measurements and known limitations.
    Expected values are derived from the fixtures themselves (for example,
    the --min-level error count must equal the Error + Fatal counts from
    `slp stats`), so the checks do not depend on exact generated data.

    The tail checks open listeners on 127.0.0.1:<TcpPort> and <UdpPort>. Pick
    ports outside the Windows excluded ranges shown by
    `netsh int ipv4 show excludedportrange protocol=udp`.

.PARAMETER Slp
    Path to slp.exe. Defaults to the downloaded build in build\StructuredLogViewer.

.PARAMETER Generator
    Path to log_generator.exe. Defaults to the release preset build.

.PARAMETER FixtureDir
    Directory written by New-TestFixtures.ps1.

.PARAMETER SkipTail
    Skips the slower streaming checks.

.OUTPUTS
    Exit code 0 when every check passes, otherwise 1.
#>
[CmdletBinding()]
param(
    [string]$Slp = (Join-Path $PSScriptRoot '..\..\build\StructuredLogViewer\slp.exe'),
    [string]$Generator = (Join-Path $PSScriptRoot '..\..\build\release\bin\Release\log_generator.exe'),
    [string]$FixtureDir = (Join-Path $PSScriptRoot '..\..\build\manual-test'),
    [int]$TcpPort = 5141,
    [int]$UdpPort = 5142,
    [switch]$SkipTail
)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $false

foreach ($tool in $Slp, $Generator) {
    if (-not (Test-Path $tool)) { throw "Not found: $tool" }
}
$FixtureDir = [System.IO.Path]::GetFullPath($FixtureDir)
if (-not (Test-Path (Join-Path $FixtureDir 'app.jsonl'))) {
    throw "No fixtures in $FixtureDir. Run New-TestFixtures.ps1 first."
}
$work = Join-Path $FixtureDir 'work'
New-Item -ItemType Directory -Force -Path $work | Out-Null

$results = [System.Collections.Generic.List[object]]::new()

function Add-Result([string]$Area, [string]$Check, $Expected, $Actual, [switch]$Info) {
    $status = if ($Info) { 'INFO' } elseif ("$Actual" -eq "$Expected") { 'PASS' } else { 'FAIL' }
    $results.Add([pscustomobject]@{ Status = $status; Area = $Area; Check = $Check; Expected = "$Expected"; Actual = "$Actual" })
    $color = @{ PASS = 'Green'; FAIL = 'Red'; INFO = 'Cyan' }[$status]
    Write-Host ("[{0}] {1,-9} {2}  (expected: {3}; actual: {4})" -f $status, $Area, $Check, $Expected, $Actual) -ForegroundColor $color
}

# Runs slp and returns its exit code and stdout; stderr is kept apart.
function Invoke-Slp {
    $stderr = Join-Path $work 'slp.err'
    $out = & $Slp @args 2> $stderr
    [pscustomobject]@{ Code = $LASTEXITCODE; Out = ($out -join "`n"); Err = (Get-Content -Raw $stderr) }
}

function Get-Count {
    [int64](Invoke-Slp query --count --allow-empty -q @args).Out
}

function Get-Fixture([string]$Name) { Join-Path $FixtureDir $Name }

function Get-LineNumbers([string[]]$JsonLines) {
    foreach ($line in $JsonLines) {
        if ($line -match '"line_number":(\d+)') { [int64]$Matches[1] }
    }
}

function Get-GapCount([int64[]]$Numbers) {
    $gaps = 0
    for ($i = 1; $i -lt $Numbers.Count; ++$i) {
        if ($Numbers[$i] -ne $Numbers[$i - 1] + 1) { ++$gaps }
    }
    $gaps
}

# Starts `slp tail`, waits, runs the generator, and returns the emitted JSON lines.
function Invoke-Tail([string[]]$TailArgs, [string[]]$GeneratorArgs, [int]$DelayMs = 1500) {
    $outFile = Join-Path $work 'tail.out'
    $errFile = Join-Path $work 'tail.err'
    $quoted = @('tail', '--output-format', 'jsonl') + $TailArgs | ForEach-Object { if ($_ -match '\s') { "`"$_`"" } else { $_ } }
    $process = Start-Process -FilePath $Slp -ArgumentList $quoted -RedirectStandardOutput $outFile `
        -RedirectStandardError $errFile -NoNewWindow -PassThru
    $null = $process.Handle
    Start-Sleep -Milliseconds $DelayMs
    & $Generator -q @GeneratorArgs
    $process.WaitForExit()
    $errors = Get-Content -Raw $errFile
    if ($errors -match 'cannot listen.*') {
        Write-Warning "slp tail: $($Matches[0])"
    }
    [pscustomobject]@{
        Code  = $process.ExitCode
        Lines = @(Get-Content $outFile | Where-Object { $_ -like '{*' })
        Err   = $errors
    }
}

$app = Get-Fixture 'app.jsonl'
$day = Get-Fixture 'day.jsonl'

# --- Format detection and parse health ---------------------------------------
$formatFixtures = [ordered]@{
    'app.jsonl'               = 'json'
    'fmt_logfmt.logfmt'       = 'logfmt'
    'fmt_csv.csv'             = 'csv'
    'fmt_syslog.log'          = 'regex'
    'fmt_apache-combined.log' = 'regex'
    'fmt_apache-common.log'   = 'regex'
    'fmt_apache-error.log'    = 'regex'
    'fmt_java.log'            = 'regex'
}
foreach ($name in $formatFixtures.Keys) {
    $path = Get-Fixture $name
    $physicalLines = @(Get-Content $path).Count
    $records = if ($name -like '*.csv') { $physicalLines - 1 } else { $physicalLines }
    $run = Invoke-Slp validate --output-format json $path
    $report = $run.Out | ConvertFrom-Json
    Add-Result 'validate' "$name exit code" 0 $run.Code
    Add-Result 'validate' "$name detected format" $formatFixtures[$name] $report.format
    Add-Result 'validate' "$name rows" $records $report.rowsProduced
    Add-Result 'validate' "$name parse errors" 0 $report.parseErrorCount
    Add-Result 'validate' "$name linesConsumed equals physical lines" $physicalLines $report.linesConsumed
    $schema = (Invoke-Slp schema --sample 0 --output-format json $path).Out | ConvertFrom-Json
    $timeColumns = @($schema.columns | Where-Object type -eq 'time').Count
    Add-Result 'schema' "$name has one time column" 1 $timeColumns
}

foreach ($name in 'app.jsonl', 'fmt_logfmt.logfmt', 'fmt_csv.csv', 'fmt_java.log', 'fmt_apache-error.log') {
    $schema = (Invoke-Slp schema --sample 0 --output-format json (Get-Fixture $name)).Out | ConvertFrom-Json
    $levelType = ($schema.columns | Where-Object header -eq 'level').type
    Add-Result 'schema' "$name level column has type level" 'level' $levelType
}

# --- Filters agree with each other -------------------------------------------
$stats = (Invoke-Slp stats --output-format json $app).Out | ConvertFrom-Json
$levelCount = @{}
$stats.levels | ForEach-Object { $levelCount[$_.level] = [int64]$_.count }
Add-Result 'stats' 'level counts add up to total' $stats.total ($levelCount.Values | Measure-Object -Sum).Sum
$errorPlus = $levelCount['Error'] + $levelCount['Fatal']
Add-Result 'filter' '--min-level error = Error + Fatal' $errorPlus (Get-Count --min-level error $app)
Add-Result 'filter' "'level IN [error, fatal]' = --min-level error" $errorPlus (Get-Count --filter 'level IN [error, fatal]' $app)
$database = Get-Count --filter 'component:database' $app
Add-Result 'filter' '--invert complements the filter' ($stats.total - $database) (Get-Count -x --filter 'component:database' $app)
Add-Result 'filter' 'numeric range line_number IN [100..199]' 100 (Get-Count --filter 'line_number IN [100..199]' $app)
Add-Result 'filter' 'numeric comparison line_number >= 990' 10 (Get-Count --filter 'line_number >= 990' $app)
$sorted = (Invoke-Slp query --sort line_number:desc --limit 3 --columns line_number -t csv --no-header $app).Out -split "`n"
Add-Result 'query' '--sort desc --limit 3' '999,998,997' ($sorted -join ',')
$last = (Invoke-Slp query --last 2 --columns line_number -t csv --no-header $app).Out -split "`n"
Add-Result 'query' '--last 2' '998,999' ($last -join ',')

# --- Time handling ------------------------------------------------------------
$window = @('--since', '2026-03-01T13:00:00+00:00', '--until', '2026-03-01T14:00:00+00:00')
$inWindow = Get-Count @window $day
Add-Result 'time' '--since/--until match a timestamp IN [..] filter' `
    (Get-Count --filter 'timestamp IN [2026-03-01T13:00:00Z..2026-03-01T14:00:00Z]' $day) $inWindow
Add-Result 'time' "--since/--until with 'Z' equal '+00:00'" $inWindow `
    (Get-Count --since 2026-03-01T13:00:00Z --until 2026-03-01T14:00:00Z $day)
$rawFirst = ((Get-Content $app -TotalCount 1) -replace '.*"timestamp":"([^"]+)".*', '$1') -replace 'T', ' '
$shown = (Invoke-Slp query --tz UTC --limit 1 --columns timestamp -t csv --no-header $app).Out
Add-Result 'time' 'zone-less log timestamp is read as UTC' $rawFirst $shown
$shownNewYork = (Invoke-Slp query --tz America/New_York --limit 1 --columns timestamp -t csv --no-header $app).Out
Add-Result 'time' '--tz only shifts rendering (UTC-5 in New York)' ($rawFirst -replace '^2026-03-01 00:', '2026-02-28 19:') $shownNewYork
Add-Result 'time' 'zone-less --since/--until are read in --tz' $inWindow `
    (Get-Count --tz America/New_York --since 2026-03-01T08:00:00 --until 2026-03-01T09:00:00 $day)
$hourly = (Invoke-Slp stats --by time --bucket 1h --tz UTC --output-format json $day).Out | ConvertFrom-Json
$buckets = @($hourly.buckets)
if ($buckets.Count -ge 24) {
    $night = $buckets[2].total
    $afternoon = $buckets[14].total
    Add-Result 'time' 'daily profile: 14:00 UTC busier than 02:00 UTC (x3)' 'True' ($afternoon -gt 3 * $night)
}
else {
    Add-Result 'time' 'stats --by time returns 24 hourly buckets' 24 $buckets.Count
}

# --- Rotation history ---------------------------------------------------------
$rotated = Get-Fixture 'rot\app.jsonl'
Add-Result 'rotation' 'rotated siblings are loaded' 4000 (Get-Count $rotated)
Add-Result 'rotation' '--no-rotation-history loads only the active file' 1000 (Get-Count --no-rotation-history $rotated)
$numbers = @(Get-LineNumbers ((Invoke-Slp query --columns line_number $rotated).Out -split "`n"))
Add-Result 'rotation' 'line_number runs 1000..4999 without gaps' '1000..4999, 0 gaps' `
    ("{0}..{1}, {2} gaps" -f $numbers[0], $numbers[-1], (Get-GapCount $numbers))

# --- Standard input -----------------------------------------------------------
Add-Result 'stdin' 'JSON lines piped to slp' 5000 ([int64](& $Generator -q -n 5000 --seed 9 -o - | & $Slp query --count -q -))
$stdinFormat = ((& $Generator -q -f logfmt -n 300 -o - | & $Slp validate --output-format json -q -) -join "`n" | ConvertFrom-Json).format
Add-Result 'stdin' 'logfmt piped to slp is detected' 'logfmt' $stdinFormat

# --- Exit codes ---------------------------------------------------------------
Add-Result 'exit' 'missing source exits 3' 3 (Invoke-Slp query (Join-Path $work 'missing.jsonl')).Code
Add-Result 'exit' 'empty result exits 1' 1 (Invoke-Slp query --filter 'level:nope' $app).Code
Add-Result 'exit' 'empty result with --allow-empty exits 0' 0 (Invoke-Slp query --filter 'level:nope' --allow-empty $app).Code
Add-Result 'exit' 'malformed filter exits 2' 2 (Invoke-Slp query --filter 'level IN [' $app).Code
Add-Result 'exit' 'unknown column exits 2' 2 (Invoke-Slp query --columns nope $app).Code
$existing = Join-Path $work 'exists.jsonl'
Set-Content -Path $existing -Value ''
Add-Result 'exit' 'existing output without --force exits 5' 5 (Invoke-Slp query -o $existing $app).Code

# --- Bundles ------------------------------------------------------------------
$warnPlus = Get-Count --min-level warn $day
$incident = Join-Path $work 'incident'
$errorsOnly = Join-Path $work 'errors_only'
Remove-Item "$incident.slvbundle", "$errorsOnly.slvbundle" -ErrorAction SilentlyContinue
Add-Result 'bundle' 'bundle with stored filter exits 0' 0 (Invoke-Slp bundle --min-level warn --sort timestamp:desc -o $incident $day).Code
Add-Result 'bundle' 'reading it back applies the stored filter' $warnPlus (Get-Count "$incident.slvbundle")
Add-Result 'bundle' '--filtered-rows bundle exits 0' 0 (Invoke-Slp bundle --filtered-rows --min-level error -o $errorsOnly $day).Code
Add-Result 'bundle' '--filtered-rows keeps only matching rows' (Get-Count --min-level error $day) (Get-Count "$errorsOnly.slvbundle")

# --- Large file ---------------------------------------------------------------
$big = Get-Fixture 'big.jsonl'
if (Test-Path $big) {
    $elapsed = Measure-Command { $null = Get-Count --min-level error $big }
    Add-Result 'perf' 'big.jsonl --min-level error --count (seconds)' '' ([math]::Round($elapsed.TotalSeconds, 2)) -Info
}

# --- Streaming ----------------------------------------------------------------
if (-not $SkipTail) {
    $live = Join-Path $work 'live.jsonl'
    & $Generator -q -n 10 -o $live
    $run = Invoke-Tail @('--for', '5s', $live) @('--rate', '100', '--profile', 'flat', '-n', '200', '--append', '-o', $live)
    Add-Result 'tail' 'file: initial 10 lines plus 200 appended' 210 $run.Lines.Count

    $tcp = "tcp://127.0.0.1:$TcpPort"
    $udp = "udp://127.0.0.1:$UdpPort"
    $run = Invoke-Tail @('--max-rows', '500', '--for', '15s', $tcp) @('-n', '500', '--rate', '500', '-o', $tcp)
    Add-Result 'tail' 'TCP JSON lines' 500 $run.Lines.Count
    $run = Invoke-Tail @('--for', '5s', $udp) @('-n', '300', '--rate', '300', '-o', $udp)
    Add-Result 'tail' 'UDP JSON lines' 300 $run.Lines.Count
    $run = Invoke-Tail @('--for', '4s', $tcp) @('-f', 'logfmt', '-n', '300', '-o', $tcp) -DelayMs 300
    Add-Result 'tail' 'TCP logfmt auto-detected (sender within 0.3 s)' 300 $run.Lines.Count
    $run = Invoke-Tail @('--for', '5s', $tcp) @('-f', 'logfmt', '-n', '300', '--rate', '300', '-o', $tcp) -DelayMs 1500
    Add-Result 'tail' 'TCP logfmt auto-detected (sender after 1.5 s)' 300 $run.Lines.Count
    $run = Invoke-Tail @('--regex-template', 'syslog_rfc3164', '--for', '5s', $udp) @('-f', 'syslog', '-n', '300', '--rate', '300', '-o', $udp)
    Add-Result 'tail' 'UDP syslog with --regex-template' 300 $run.Lines.Count

    $rotDir = Join-Path $work 'rotlive'
    foreach ($strategy in 'rename', 'copytruncate', 'truncate') {
        New-Item -ItemType Directory -Force -Path $rotDir | Out-Null
        Remove-Item (Join-Path $rotDir '*') -ErrorAction SilentlyContinue
        $file = Join-Path $rotDir 'app.jsonl'
        New-Item -ItemType File -Path $file | Out-Null
        $run = Invoke-Tail @('--no-rotation-history', '--for', '8s', $file) `
            @('--rate', '400', '--profile', 'flat', '-n', '2000', '--roll-lines', '300', '--roll-strategy', $strategy, '-o', $file) -DelayMs 1000
        $numbers = @(Get-LineNumbers $run.Lines)
        $summary = "{0} rows, {1} gaps" -f $numbers.Count, (Get-GapCount $numbers)
        if ($strategy -eq 'rename') {
            Add-Result 'tail' 'rotating file (rename) loses no lines' '2000 rows, 0 gaps' $summary
        }
        else {
            # Lines written after the last poll and before the truncate are gone from
            # the live file; this is the known limitation of truncating rotation.
            Add-Result 'tail' "rotating file ($strategy), lines lost at truncation" '' $summary -Info
        }
    }
}

# --- Summary ------------------------------------------------------------------
$failed = @($results | Where-Object Status -eq 'FAIL')
$passed = @($results | Where-Object Status -eq 'PASS')
Write-Host ''
Write-Host ("{0} passed, {1} failed, {2} info" -f $passed.Count, $failed.Count, @($results | Where-Object Status -eq 'INFO').Count)
if ($failed.Count -gt 0) {
    $failed | Format-Table Area, Check, Expected, Actual -AutoSize -Wrap | Out-Host
    exit 1
}
exit 0
