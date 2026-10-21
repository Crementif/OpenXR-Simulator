<#
.SYNOPSIS
    Build and run the D3D12 or Vulkan compatibility probe through CMake.
.PARAMETER RuntimeJson
    Runtime manifest to test. Defaults to this checkout's bin directory.
.PARAMETER OpenXRLoader
    Optional path to openxr_loader.lib when it is not installed through a CMake package.
#>
[CmdletBinding()]
param(
    [string]$RuntimeJson = (Join-Path $PSScriptRoot '..\bin\openxr_simulator.json'),
    [string]$OpenXRLoader,
    [switch]$Vulkan,
    [switch]$Enable1
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$build = Join-Path $root 'build'
$probeName = if ($Vulkan) { 'xr_probe_vk' } else { 'xr_probe' }
if (-not (Test-Path $RuntimeJson)) { throw "Runtime manifest not found: $RuntimeJson" }
if (-not (Get-Command cl -ErrorAction SilentlyContinue)) {
    $vsRoot = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vsRoot) { throw 'Run from a Visual Studio developer shell or install the C++ toolchain.' }
    Import-Module (Join-Path $vsRoot 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
    Enter-VsDevShell -VsInstallPath $vsRoot -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
}
$configureArgs = @('-S', $root, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DSIMXR_BUILD_PROBES=ON')
if ($OpenXRLoader) { $configureArgs += "-DSIMXR_OPENXR_LOADER=$OpenXRLoader" }
cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed ($LASTEXITCODE)" }
cmake --build $build --target $probeName
if ($LASTEXITCODE -ne 0) { throw "Probe build failed. Install the OpenXR loader or pass -OpenXRLoader. ($LASTEXITCODE)" }
$exe = Join-Path $root "bin\$probeName.exe"
$previousRuntime = $env:XR_RUNTIME_JSON
$previousEnable1 = $env:XR_PROBE_VK_ENABLE1
try {
    $env:XR_RUNTIME_JSON = (Resolve-Path $RuntimeJson).Path
    $env:XR_PROBE_VK_ENABLE1 = if ($Vulkan -and $Enable1) { '1' } else { '0' }
    & $exe
    $probeExitCode = $LASTEXITCODE
} finally {
    $env:XR_RUNTIME_JSON = $previousRuntime
    $env:XR_PROBE_VK_ENABLE1 = $previousEnable1
}
exit $probeExitCode
