<#
.SYNOPSIS
    Build and install the OpenXR Simulator runtime and companion.

.DESCRIPTION
    Installs the runtime, Vulkan companion, rig runner, relative manifest and
    activate/deactivate scripts into the target folder. Only build output lands
    there, never source.

    The dll and scripts are symlinked, so a rebuild is live in BetterVR without
    reinstalling. Creating symlinks needs Developer Mode (or an elevated shell);
    without it the script copies instead and says so.

.PARAMETER InstallTo
    Where to install. Defaults to out\install\OpenXRSimulator in this checkout.

.PARAMETER Copy
    Install real copies instead of symlinks, for a folder that has to stand on
    its own -- moved to another machine, or kept working after the build tree goes.

.PARAMETER Clean
    Delete the CMake build tree first.
#>
[CmdletBinding()]
param(
    [string]$InstallTo = (Join-Path $PSScriptRoot 'out\install\OpenXRSimulator'),
    [switch]$Copy,
    [switch]$Clean
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot
$build = Join-Path $root 'out\build\windows-local'
$output = Join-Path $build 'bin'

$vsRoot = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vsRoot) { throw 'No Visual Studio installation with the C++ toolchain was found.' }

$cmakeDir = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaDir = Join-Path $vsRoot 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
if (Test-Path -LiteralPath $cmakeDir) { $env:PATH = "$cmakeDir;$ninjaDir;$env:PATH" }
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) { throw 'cmake was not found on PATH or in the Visual Studio installation.' }

Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null

if ($Clean -and (Test-Path -LiteralPath $build)) { Remove-Item -LiteralPath $build -Recurse -Force }

Write-Host '==> Configuring' -ForegroundColor Cyan
cmake --preset release -S $root -B $build
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }

Write-Host '==> Building' -ForegroundColor Cyan
cmake --build $build
if ($LASTEXITCODE -ne 0) { throw "cmake build failed ($LASTEXITCODE)" }
ctest --test-dir $build --output-on-failure
if ($LASTEXITCODE -ne 0) { throw "ctest failed ($LASTEXITCODE)" }

if (-not (Test-Path -LiteralPath $InstallTo)) { New-Item -ItemType Directory -Force $InstallTo | Out-Null }
$InstallTo = (Resolve-Path -LiteralPath $InstallTo).Path

$linked = 0
$copied = 0
function Install-Artifact {
    param([string]$Source, [string]$DestDir)

    $source = (Resolve-Path -LiteralPath $Source).Path
    $dest = Join-Path $DestDir (Split-Path $source -Leaf)
    if ($source -eq [System.IO.Path]::GetFullPath($dest)) {
        throw 'The installation folder must differ from the build output folder.'
    }

    if (Test-Path -LiteralPath $dest) {
        try { Remove-Item -LiteralPath $dest -Force -ErrorAction Stop }
        catch { throw "Cannot replace $dest -- a running process still has it open. Close it and rebuild." }
    }

    if (-not $script:Copy) {
        try {
            New-Item -ItemType SymbolicLink -Path $dest -Target $source -ErrorAction Stop | Out-Null
            $script:linked++
            return
        } catch {
            # No SeCreateSymbolicLinkPrivilege: fall back rather than fail the build.
        }
    }
    Copy-Item -LiteralPath $source -Destination $dest -Force
    $script:copied++
}

foreach ($artifact in 'openxr_simulator.dll', 'simxr_companion.exe', 'simxr_runner.exe') {
    Install-Artifact (Join-Path $output $artifact) $InstallTo
}
$fonts = Join-Path $InstallTo 'fonts'
$fontLicense = Join-Path $InstallTo 'share\licenses\simxr_companion'
New-Item -ItemType Directory -Force $fonts, $fontLicense | Out-Null
Install-Artifact (Join-Path $root 'resources\fonts\AdwaitaSans-Regular.ttf') $fonts
Install-Artifact (Join-Path $root 'resources\fonts\ADWAITA-SANS-LICENSE.txt') $fontLicense
Install-Artifact (Join-Path $root 'activate_simulator.ps1') $InstallTo
Install-Artifact (Join-Path $root 'deactivate_simulator.ps1') $InstallTo

# Keep the installed manifest independent of the build directory.
$manifest = @{
    file_format_version = '1.0.0'
    runtime             = @{ library_path = '.\openxr_simulator.dll'; name = 'OpenXR Simulator' }
} | ConvertTo-Json -Depth 3
Set-Content -LiteralPath (Join-Path $InstallTo 'openxr_simulator.json') -Value $manifest -Encoding ascii

Write-Host ''
if ($linked -gt 0 -and $copied -eq 0) {
    Write-Host "Symlinked into $InstallTo -- rebuilds are picked up with no reinstall" -ForegroundColor Green
} elseif ($linked -gt 0) {
    Write-Host "Installed to $InstallTo ($linked symlinked, $copied copied)" -ForegroundColor Green
} elseif ($Copy) {
    Write-Host "Copied to $InstallTo" -ForegroundColor Green
} else {
    Write-Host "Copied to $InstallTo -- symlinks need Developer Mode (Settings > System >" -ForegroundColor Yellow
    Write-Host "For developers) or an elevated shell; without it every build needs a reinstall." -ForegroundColor Yellow
}
Write-Host ''
Write-Host "Start the companion: $(Join-Path $InstallTo 'simxr_companion.exe')"
Write-Host 'Use it per-process (leaves the machine-wide runtime alone):'
Write-Host "  `$env:XR_RUNTIME_JSON = '$(Join-Path $InstallTo 'openxr_simulator.json')'"
Write-Host 'or machine-wide with .\activate_simulator.ps1 in that folder.'
