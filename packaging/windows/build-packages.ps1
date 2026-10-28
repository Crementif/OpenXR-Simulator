param(
    [Parameter(Mandatory = $true)][string]$BuildDirectory,
    [Parameter(Mandatory = $true)][string]$OutputDirectory,
    [Parameter(Mandatory = $true)][string]$InnoCompiler,
    [string]$Configuration = 'Release'
)

$ErrorActionPreference = 'Stop'
$build = (Resolve-Path -LiteralPath $BuildDirectory).Path
$compiler = (Resolve-Path -LiteralPath $InnoCompiler).Path
$output = [System.IO.Path]::GetFullPath($OutputDirectory)
New-Item -ItemType Directory -Force -Path $output | Out-Null
$header = Get-Content -Raw -LiteralPath (Join-Path $build 'generated\simxr\Version.h')
if ($header -notmatch 'Version\[\]\s*=\s*"([0-9]+\.[0-9]+\.[0-9]+)"') {
    throw 'Could not read the configured simulator version.'
}
$version = $Matches[1]
$packageName = "openxr-simulator-$version-windows-x64"
$archiveName = "$packageName.zip"
$setupName = "$packageName-setup.exe"
foreach ($name in @($archiveName, $setupName)) {
    if (Test-Path -LiteralPath (Join-Path $output $name)) {
        throw "Refusing to replace an existing package: $name"
    }
}
$temporary = Join-Path $output ('.simxr-build-' + [guid]::NewGuid().ToString('N'))
$stage = Join-Path $temporary $packageName
New-Item -ItemType Directory -Path $temporary | Out-Null
function Assert-X64([string]$File) {
    $reader = [System.IO.BinaryReader]::new([System.IO.File]::OpenRead($File))
    try {
        if ($reader.ReadUInt16() -ne 0x5a4d) { throw "Invalid PE file: $File" }
        $reader.BaseStream.Position = 0x3c
        $offset = $reader.ReadUInt32()
        $reader.BaseStream.Position = $offset
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw "Expected a Windows x64 binary: $File"
        }
    } finally {
        $reader.Dispose()
    }
}
try {
    & cmake --install $build --config $Configuration --prefix $stage
    if ($LASTEXITCODE -ne 0) { throw "CMake staging failed ($LASTEXITCODE)." }
    foreach ($relative in @('bin\simxr_companion.exe', 'bin\simxr_mcp_connector.exe',
                            'bin\simxr_runner.exe', 'bin\simxr_install_check.exe',
                            'bin\openxr_simulator.json',
                            'share\doc\simxr\BUILDING.md',
                            'share\simxr\branding\openxr-simulator.ico', 'share\licenses\simxr\LICENSE')) {
        if (-not (Test-Path -LiteralPath (Join-Path $stage $relative) -PathType Leaf)) {
            throw "The staged installation is incomplete: $relative"
        }
    }
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $stage 'bin\openxr_simulator.json') | ConvertFrom-Json
    $runtime = [string]$manifest.runtime.library_path
    if ($runtime -notmatch '^\./(?:lib)?openxr_simulator\.dll$' -or
        -not (Test-Path -LiteralPath (Join-Path (Join-Path $stage 'bin') $runtime) -PathType Leaf)) {
        throw 'The staged runtime manifest does not point to its adjacent runtime DLL.'
    }
    foreach ($binary in Get-ChildItem -LiteralPath (Join-Path $stage 'bin') -File) {
        if ($binary.Extension -in @('.exe', '.dll')) {
            Assert-X64 $binary.FullName
        }
    }
    Push-Location $temporary
    try {
        & cmake -E tar cf $archiveName --format=zip $packageName
        if ($LASTEXITCODE -ne 0) { throw "Portable ZIP creation failed ($LASTEXITCODE)." }
    } finally {
        Pop-Location
    }
    $script = Join-Path $PSScriptRoot 'OpenXRSimulator.iss'
    & $compiler "/DStageDir=$stage" "/DProductVersion=$version" "/O$temporary" $script
    if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed ($LASTEXITCODE)." }
    Move-Item -LiteralPath (Join-Path $temporary $archiveName) -Destination (Join-Path $output $archiveName)
    Move-Item -LiteralPath (Join-Path $temporary $setupName) -Destination (Join-Path $output $setupName)
    Write-Output "Created $archiveName and $setupName in $output"
} finally {
    Remove-Item -LiteralPath $temporary -Recurse -Force -ErrorAction SilentlyContinue
}
