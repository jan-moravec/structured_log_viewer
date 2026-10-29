# Finds a full OpenSSL install (headers, import libraries, DLLs) and sets OPENSSL_ROOT_DIR for CMake.
# The runner image has one. If it is missing, installs OpenSSL from Chocolatey, unpinned because old versions
# disappear from the mirror. Strawberry Perl's OpenSSL has no import libraries, so it is skipped.

$ErrorActionPreference = "Stop"

function Find-OpenSsl {
    foreach ($c in @("C:\Program Files\OpenSSL-Win64", "C:\Program Files\OpenSSL")) {
        if ((Test-Path "$c\bin\openssl.exe") -and
            (Test-Path "$c\include\openssl") -and
            (Test-Path "$c\bin\libssl*.dll") -and
            (Test-Path "$c\bin\libcrypto*.dll")) {
            return $c
        }
    }
    return $null
}

$found = Find-OpenSsl
if ($found) {
    Write-Host "Using the OpenSSL preinstalled on the runner at $found"
} else {
    Write-Host "No usable preinstalled OpenSSL; installing from Chocolatey."
    for ($attempt = 1; $attempt -le 3 -and -not $found; $attempt++) {
        choco install openssl --yes --no-progress --force
        $found = Find-OpenSsl
        if (-not $found) {
            Write-Host "Attempt $attempt did not produce a usable OpenSSL."
            Start-Sleep -Seconds (10 * $attempt)
        }
    }
}
if (-not $found) {
    Write-Host "OpenSSL not found at any known install root. Listing C:\Program Files\:"
    Get-ChildItem "C:\Program Files" -Directory | Where-Object { $_.Name -like "OpenSSL*" } | Format-Table -AutoSize
    throw "OpenSSL install missing required headers / openssl.exe; cannot continue."
}
$version = & "$found\bin\openssl.exe" version
Write-Host "Located OpenSSL: $version at $found"
Get-ChildItem "$found\bin" -Filter "libssl*.dll"
Get-ChildItem "$found\bin" -Filter "libcrypto*.dll"
if ($env:GITHUB_ENV) {
    "OPENSSL_ROOT_DIR=$found" | Out-File -FilePath $env:GITHUB_ENV -Append -Encoding utf8
}
