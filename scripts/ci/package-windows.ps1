# Builds the portable Windows zip from the `release` build.
# CMake already copies the runtime DLLs (TBB, MSVC runtime, OpenSSL) and license files next to the .exe.
# windeployqt skips the vc_redist installer; the zip ships the runtime DLLs instead.
#
# Environment: APP_NAME (default StructuredLogViewer).

$ErrorActionPreference = "Stop"

$appName = if ($env:APP_NAME) { $env:APP_NAME } else { "StructuredLogViewer" }
$binDir = Resolve-Path "build\release\bin\Release"

New-Item -ItemType Directory -Force $appName | Out-Null
Push-Location $appName
try {
    Move-Item "$binDir\$appName.exe" .
    Move-Item "$binDir\slp.exe" .
    Move-Item "$binDir\tzdata" .
    Move-Item "$binDir\tbb*.dll" .
    Move-Item "$binDir\msvcp*.dll" .
    Move-Item "$binDir\vcruntime*.dll" .
    if (Test-Path "$binDir\concrt*.dll") {
        Move-Item "$binDir\concrt*.dll" .
    }
    Move-Item "$binDir\libssl-*.dll" .
    Move-Item "$binDir\libcrypto-*.dll" .
    Move-Item "$binDir\THIRD_PARTY_LICENSES.txt" .
    Move-Item "$binDir\LICENSE" .
    windeployqt --release --no-compiler-runtime "$appName.exe"
    if ($LASTEXITCODE -ne 0) { throw "windeployqt failed with code $LASTEXITCODE" }
    Compress-Archive -Path . -DestinationPath "..\$appName.zip"
} finally {
    Pop-Location
}
