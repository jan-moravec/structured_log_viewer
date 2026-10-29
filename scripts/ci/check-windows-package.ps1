# Starts the packaged app without a display and runs `slp.exe --version`, so a missing DLL fails CI.
#
# Environment: APP_NAME (default StructuredLogViewer).

$ErrorActionPreference = "Stop"

$appName = if ($env:APP_NAME) { $env:APP_NAME } else { "StructuredLogViewer" }

$env:QT_QPA_PLATFORM = "offscreen"
$exe = "$appName\$appName.exe"
$p = Start-Process -FilePath $exe -PassThru -NoNewWindow
Start-Sleep -Seconds 5
if ($p.HasExited) {
    # A missing DLL shows up as STATUS_DLL_NOT_FOUND: -1073741515 (0xc0000135).
    Write-Error "Packaged binary exited prematurely with code $($p.ExitCode)"
    exit 1
}
Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
Write-Host "Packaged binary started successfully and was terminated after 5s."

& "$appName\slp.exe" --version
if ($LASTEXITCODE -ne 0) {
    Write-Error "Packaged slp.exe --version exited with code $LASTEXITCODE"
    exit 1
}
if (-not (Test-Path "$appName\THIRD_PARTY_LICENSES.txt")) {
    Write-Error "THIRD_PARTY_LICENSES.txt missing next to slp.exe"
    exit 1
}
