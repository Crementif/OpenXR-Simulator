param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [Parameter(Mandatory = $true)][string]$InnoCompiler,
    [Parameter(Mandatory = $true)][string]$Cmake,
    [Parameter(Mandatory = $true)][string]$Builder,
    [string]$PackageDirectory
)

$ErrorActionPreference = 'Stop'
$sourcePath = (Resolve-Path -LiteralPath $Stage).Path
$source = $sourcePath.Replace('\', '/')
$readme = Get-Content -LiteralPath (Join-Path $Stage 'README.txt') -TotalCount 1
if ($readme -notmatch '^OpenXR Simulator ([0-9]+\.[0-9]+\.[0-9]+)$') { throw 'Staged version is missing.' }
$version = $Matches[1]
$manifest = Get-Content -Raw -LiteralPath (Join-Path $Stage 'bin\openxr_simulator.json') | ConvertFrom-Json
if ($manifest.runtime.library_path -notmatch '^\./((?:lib)?openxr_simulator\.dll)$') {
    throw 'Staged runtime path is invalid.'
}
$cmakePath = (Resolve-Path -LiteralPath $Cmake).Path
$env:PATH = (Split-Path -Parent $cmakePath) + ';' + $env:PATH
$root = Join-Path $env:TEMP ('simxr-package-builder-' + [guid]::NewGuid().ToString('N'))
$build = Join-Path $root 'build Ω'
$output = if ($PackageDirectory) { (Resolve-Path -LiteralPath $PackageDirectory).Path } else { Join-Path $root 'packages Ω' }
try {
    if (-not $PackageDirectory) {
        New-Item -ItemType Directory -Path (Join-Path $build 'generated\simxr') -Force | Out-Null
        Set-Content -LiteralPath (Join-Path $build 'generated\simxr\Version.h') -Value "inline constexpr char Version[] = `"$version`";" -Encoding Ascii
        $install = @'
if(NOT DEFINED CMAKE_INSTALL_PREFIX)
  message(FATAL_ERROR "Missing install prefix")
endif()
file(INSTALL DESTINATION "${CMAKE_INSTALL_PREFIX}" TYPE DIRECTORY FILES "__SOURCE__/")
'@
        Set-Content -LiteralPath (Join-Path $build 'cmake_install.cmake') -Value $install.Replace('__SOURCE__', $source) -Encoding UTF8
        & $Builder -BuildDirectory $build -OutputDirectory $output -InnoCompiler $InnoCompiler
        if ($LASTEXITCODE -ne 0) { throw "Package builder failed: $LASTEXITCODE" }
    }
    $package = "openxr-simulator-$version-windows-x64"
    $zip = Join-Path $output "$package.zip"
    $installer = Join-Path $output "$package-setup.exe"
    if (-not (Test-Path -LiteralPath $zip) -or -not (Test-Path -LiteralPath $installer)) {
        throw 'Package builder did not produce both artifacts.'
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($zip)
    try {
        $files = @(Get-ChildItem -LiteralPath $sourcePath -Recurse -File)
        $entries = @($archive.Entries | Where-Object { -not $_.FullName.EndsWith('/') })
        if ($entries.Count -ne $files.Count) { throw 'Portable package file count differs from stage.' }
        foreach ($file in $files) {
            $relative = $file.FullName.Substring($sourcePath.Length).TrimStart('\').Replace('\', '/')
            $entry = $archive.GetEntry("$package/$relative")
            if ($null -eq $entry) { throw "Portable package is missing $relative" }
            $stream = $entry.Open()
            $algorithm = [System.Security.Cryptography.SHA256]::Create()
            try {
                $actual = [BitConverter]::ToString($algorithm.ComputeHash($stream))
                $expected = (Get-FileHash -Algorithm SHA256 -LiteralPath $file.FullName).Hash
                if ($actual.Replace('-', '') -ne $expected) { throw "Portable package differs from stage: $relative" }
            } finally {
                $algorithm.Dispose()
                $stream.Dispose()
            }
        }
    } finally {
        $archive.Dispose()
    }
    Write-Output 'PASS Windows portable package matches all staged files and setup exists'
} finally {
    Remove-Item -LiteralPath $root -Recurse -Force -ErrorAction SilentlyContinue
}
