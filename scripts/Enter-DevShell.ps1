<#
.SYNOPSIS
    Load the Visual Studio MSVC developer environment into the current PowerShell session.

.DESCRIPTION
    Locates the latest Visual Studio 2017+ install (Community, Professional, Enterprise,
    or Build Tools) using vswhere, then invokes Launch-VsDevShell.ps1 to import the MSVC
    toolchain environment (PATH, INCLUDE, LIB, LIBPATH, etc.) into the current
    PowerShell session.

    After running this script, cl.exe, link.exe, and other MSVC tools are on PATH,
    so the MSVC-pinned CMake presets (`local-msvc`, `local-msvc-debug`) will work with:

        cmake --workflow --preset local-msvc

    Environment changes only affect the current PowerShell process. Open a new
    shell and you'll need to run this again.

.PARAMETER Arch
    Target architecture (passed to Launch-VsDevShell.ps1). Default: amd64.

.PARAMETER HostArch
    Host architecture (passed to Launch-VsDevShell.ps1). Default: amd64.

.PARAMETER Prerelease
    Also consider Visual Studio Preview / prerelease installs.

.EXAMPLE
    PS C:\code\structured_log_viewer> .\scripts\Enter-DevShell.ps1
    PS C:\code\structured_log_viewer> cmake --workflow --preset local-msvc

.EXAMPLE
    PS> .\scripts\Enter-DevShell.ps1 -Arch x86 -HostArch amd64

.NOTES
    Requires Visual Studio 2017 or later (or the standalone "Build Tools for Visual
    Studio") with the "MSVC v14x - VS 20xx C++ x64/x86 build tools" component
    installed. vswhere.exe is shipped by the VS Installer at:

        %ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe
#>

[CmdletBinding()]
param(
    [ValidateSet('amd64', 'x86', 'arm64', 'arm')]
    [string] $Arch = 'amd64',

    [ValidateSet('amd64', 'x86')]
    [string] $HostArch = 'amd64',

    [switch] $Prerelease
)

$ErrorActionPreference = 'Stop'

if ($env:VSCMD_VER) {
    Write-Host "VS Dev Shell already active (VSCMD_VER=$($env:VSCMD_VER)). Skipping." -ForegroundColor Yellow
    return
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) {
    throw "vswhere.exe not found at '$vswhere'. Install Visual Studio 2017 or later, or the standalone Build Tools for Visual Studio."
}

$vswhereArgs = @(
    '-latest',
    '-products', '*',
    '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
    '-property', 'installationPath'
)
if ($Prerelease) { $vswhereArgs += '-prerelease' }

$installPath = (& $vswhere @vswhereArgs | Select-Object -First 1)
if (-not $installPath) {
    throw 'No Visual Studio installation with the "MSVC v14x - VS 20xx C++ x64/x86 build tools" component was found. Install it via the Visual Studio Installer.'
}
$installPath = $installPath.Trim()

$launcher = Join-Path $installPath 'Common7\Tools\Launch-VsDevShell.ps1'
if (-not (Test-Path -LiteralPath $launcher)) {
    throw "Launch-VsDevShell.ps1 not found under '$installPath'. The Visual Studio install appears incomplete."
}

Write-Host "Entering VS Dev Shell ($Arch / host $HostArch) from:" -ForegroundColor Cyan
Write-Host "  $installPath" -ForegroundColor Cyan

& $launcher -Arch $Arch -HostArch $HostArch -SkipAutomaticLocation

if (-not (Get-Command cl.exe -ErrorAction SilentlyContinue)) {
    Write-Warning 'cl.exe is still not on PATH after Launch-VsDevShell.ps1. Something went wrong; check the output above.'
} else {
    $cl = (Get-Command cl.exe).Source
    Write-Host "Ready. cl.exe -> $cl" -ForegroundColor Green
}
