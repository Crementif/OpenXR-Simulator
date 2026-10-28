param(
    [Parameter(Mandatory = $true)][string]$WorkDirectory,
    [Parameter(Mandatory = $true)][string]$InnoCompiler
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$work = [System.IO.Path]::GetFullPath($WorkDirectory)
if (Test-Path -LiteralPath $work) { throw "Refusing to reuse a CI work directory: $work" }
New-Item -ItemType Directory -Path $work | Out-Null
$compiler = (Resolve-Path -LiteralPath $InnoCompiler).Path
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw 'Visual Studio C++ tools are required.' }
Import-Module (Join-Path $vs 'Common7\Tools\Microsoft.VisualStudio.DevShell.dll')
Enter-VsDevShell -VsInstallPath $vs -SkipAutomaticLocation -DevCmdArguments '-arch=x64 -host_arch=x64' | Out-Null
$cmakeBin = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'
$ninjaBin = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'
$env:PATH = "$cmakeBin;$ninjaBin;$env:PATH"
function Run-Native {
    param([string]$Program, [string[]]$Arguments)
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program failed ($LASTEXITCODE)." }
}

$deps = Join-Path $work 'deps'
Run-Native 'python' @((Join-Path $root 'packaging\ci\fetch-deps.py'), '--platform', 'windows', '--output', $deps)
$vulkan = Join-Path $work 'vulkan'
$headersBuild = Join-Path $work 'headers-build'
$loaderBuild = Join-Path $work 'vulkan-loader-build'
Run-Native 'cmake' @('-S', (Join-Path $deps 'Vulkan-Headers'), '-B', $headersBuild, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', '-DVULKAN_HEADERS_ENABLE_TESTS=OFF', "-DCMAKE_INSTALL_PREFIX=$vulkan")
Run-Native 'cmake' @('--install', $headersBuild, '--config', 'Release')
Run-Native 'cmake' @('-S', (Join-Path $deps 'Vulkan-Loader'), '-B', $loaderBuild, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTS=OFF', "-DCMAKE_PREFIX_PATH=$vulkan", "-DCMAKE_INSTALL_PREFIX=$vulkan")
Run-Native 'cmake' @('--build', $loaderBuild, '--parallel', '4')
Run-Native 'cmake' @('--install', $loaderBuild, '--config', 'Release')
$vulkanLibrary = Join-Path $vulkan 'lib\vulkan-1.lib'
if (-not (Test-Path -LiteralPath $vulkanLibrary -PathType Leaf)) { throw 'Vulkan import library was not installed.' }

$openxr = Join-Path $work 'openxr'
$openxrBuild = Join-Path $work 'openxr-build'
Run-Native 'cmake' @('-S', (Join-Path $deps 'OpenXR-SDK'), '-B', $openxrBuild, '-G', 'Ninja',
    '-DCMAKE_BUILD_TYPE=Release', '-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded', '-DBUILD_TESTS=OFF',
    '-DBUILD_CONFORMANCE_TESTS=OFF', '-DBUILD_API_LAYERS=OFF', '-DINSTALL_SYMBOLS=OFF', '-DDYNAMIC_LOADER=ON',
    "-DCMAKE_PREFIX_PATH=$vulkan", "-DCMAKE_INSTALL_PREFIX=$openxr")
Run-Native 'cmake' @('--build', $openxrBuild, '--parallel', '4')
Run-Native 'cmake' @('--install', $openxrBuild, '--config', 'Release')
$openxrLibrary = Join-Path $openxr 'lib\openxr_loader.lib'
if (-not (Test-Path -LiteralPath $openxrLibrary -PathType Leaf) -or
    -not (Test-Path -LiteralPath (Join-Path $openxr 'bin\openxr_loader.dll') -PathType Leaf)) {
    throw 'OpenXR loader and its import library were not installed.'
}
$license = Join-Path $work 'openxr-licenses.txt'
$licenseText = [System.IO.File]::ReadAllText((Join-Path $deps 'OpenXR-SDK\LICENSE')) + "`n`n" +
    [System.IO.File]::ReadAllText((Join-Path $deps 'OpenXR-SDK\src\external\jsoncpp\LICENSE'))
[System.IO.File]::WriteAllText($license, $licenseText, [System.Text.UTF8Encoding]::new($false))

$build = Join-Path $work 'simulator-build'
$packages = Join-Path $work 'packages'
Run-Native 'cmake' @('-S', $root, '-B', $build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
    '-DBUILD_TESTING=ON', '-DSIMXR_BUILD_RUNTIME=ON', '-DSIMXR_BUILD_COMPANION=ON',
    '-DSIMXR_BUILD_AUTOMATION=ON', '-DSIMXR_BUILD_PROBES=ON', '-DSIMXR_BUILD_HEADSET=ON',
    "-DCMAKE_PREFIX_PATH=$openxr", '-DCMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP=TRUE', "-DSIMXR_OUTPUT_DIRECTORY=$build\bin",
    "-DSIMXR_VULKAN_INCLUDE_DIR=$deps\Vulkan-Headers\include", "-DSIMXR_VULKAN_LIBRARY=$vulkanLibrary",
    "-DSIMXR_OPENGL_INCLUDE_DIR=$deps\gl", "-DSIMXR_OPENXR_LOADER_LICENSE_FILE=$license",
    "-DSIMXR_RELEASE_REPOSITORY=$env:SIMXR_RELEASE_REPOSITORY")
Run-Native 'cmake' @('--build', $build, '--parallel', '4', '--target', 'openxr_simulator', 'simxr_companion',
    'simxr_runner', 'simxr_headset_host', 'xr_probe_d3d11', 'xr_probe_d3d12', 'xr_probe_opengl', 'xr_probe_vk',
    'companion_settings_tests', 'update_checker_tests', 'mcp_simulation_command_tests', 'mcp_history_tests', 'startup_failure_tests',
    'stereo_surface_tests', 'stereo_consistency_tests', 'mcp_image_encode_tests', 'mcp_application_tests', 'package_documentation_tests', 'windows_profile_fixture')
Run-Native 'ctest' @('--test-dir', $build, '--output-on-failure', '-R', '^(companion_settings|update_checker|mcp_simulation_commands|mcp_history|mcp_connector_cancellation|mcp_connector_configuration|mcp_connector_protocol|startup_failure_format|stereo_surface|stereo_consistency|mcp_image_encode|mcp_application|package_documentation)$')
Run-Native 'python' @((Join-Path $root 'tests\app\test_update_timeout_windows.py'), (Join-Path $build 'update_checker_tests.exe'))
$stage = Join-Path $work 'verification-stage'
Run-Native 'cmake' @('--install', $build, '--config', 'Release', '--prefix', $stage)
foreach ($binary in Get-ChildItem -LiteralPath (Join-Path $stage 'bin') -File | Where-Object { $_.Extension -in '.exe', '.dll' }) {
    $dependencies = & dumpbin /dependents $binary.FullName
    if ($LASTEXITCODE -ne 0) { throw "Could not inspect dependencies of $($binary.Name)." }
    if ($dependencies -match '\b(?:MSVCP|VCRUNTIME|CONCRT)\d+[^\s]*\.dll\b') {
        throw "Unexpected Visual C++ redistributable dependency in $($binary.Name)."
    }
}
& (Join-Path $root 'packaging\windows\build-packages.ps1') -BuildDirectory $build -OutputDirectory $packages -InnoCompiler $compiler
& (Join-Path $root 'tests\packaging\test_windows_builder.ps1') -Stage $stage -InnoCompiler $compiler `
    -Cmake (Get-Command cmake).Source -Builder (Join-Path $root 'packaging\windows\build-packages.ps1') `
    -PackageDirectory $packages
$installers = @(Get-ChildItem -LiteralPath $packages -Filter '*-setup.exe' -File)
if ($installers.Count -ne 1) { throw 'Expected exactly one generated Windows installer.' }
Run-Native 'python' @((Join-Path $root 'tests\packaging\test_windows_install.py'), $installers[0].FullName, $stage)
$archives = @(Get-ChildItem -LiteralPath $packages -Filter '*.zip' -File)
if ($archives.Count -ne 1) { throw 'Expected exactly one portable Windows ZIP.' }
Run-Native 'python' @((Join-Path $root 'tests\packaging\test_windows_portable.py'), $archives[0].FullName, '--skip-xr')
if ($env:SIMXR_CI_PROFILE_UI_TEST -eq '1') {
    Run-Native 'python' @((Join-Path $root 'tests\app\test_profile_command_windows.py'),
        $archives[0].FullName, (Join-Path $build 'windows_profile_fixture.exe'))
}
Write-Output "Windows CI packages are in $packages"
