param(
    [ValidateSet('Build', 'Upload', 'Monitor', 'Ports')]
    [string]$Action = 'Build',
    [string]$Port,
    [string]$BuildRoot
)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent

function Sync-SourceFile([string]$SourceFile, [string]$DestinationFile) {
    if ((Test-Path -LiteralPath $DestinationFile) -and
        ((Get-FileHash -LiteralPath $SourceFile).Hash -eq
         (Get-FileHash -LiteralPath $DestinationFile).Hash)) { return }
    New-Item -ItemType Directory -Path (Split-Path $DestinationFile -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $SourceFile -Destination $DestinationFile -Force
}
$pioExe = Join-Path $env:USERPROFILE '.platformio/penv/Scripts/platformio.exe'
if (-not (Test-Path -LiteralPath $pioExe)) {
    throw 'Install the VS Code PlatformIO IDE extension first.'
}

# Reuse the already downloaded toolchain on this machine if still available.
# After temporary-cache cleanup, PlatformIO will install it in the durable cache.
$legacyCore = Join-Path $env:TEMP 'pio-voice-core-20260917'
$legacyBuild = Join-Path $env:TEMP 'mine_voice_node_src_20260917_1730'
$previousCore = $env:PLATFORMIO_CORE_DIR
if (Test-Path -LiteralPath $legacyCore) {
    $env:PLATFORMIO_CORE_DIR = $legacyCore
} else {
    $env:PLATFORMIO_CORE_DIR = Join-Path $env:LOCALAPPDATA 'MineVoice/platformio'
}
if (-not $BuildRoot) {
    $BuildRoot = Join-Path $env:LOCALAPPDATA 'MineVoice/source'
    if (Test-Path -LiteralPath $legacyBuild) { $BuildRoot = $legacyBuild }
}
$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
if ($BuildRoot -match '[^\x00-\x7F]') {
    throw 'ESP-IDF 5.3 requires an ASCII build path. Pass -BuildRoot D:/esp32-build/voice.'
}
if (($Action -eq 'Upload' -or $Action -eq 'Monitor') -and -not $Port) {
    throw 'Specify the ESP32 USB port, for example -Port COM10. Do not select a Bluetooth port.'
}

try {
    if ($Action -eq 'Ports') {
        & $pioExe device list
        if ($LASTEXITCODE -ne 0) { throw 'Could not list ports.' }
        return
    }
    New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
    foreach ($item in @('main', 'scripts', 'platformio.ini', 'CMakeLists.txt',
                       'partitions.csv', 'sdkconfig.defaults', 'dependencies.lock')) {
        $sourceItem = Join-Path $projectRoot $item
        if (Test-Path -LiteralPath $sourceItem) {
            if (Test-Path -LiteralPath $sourceItem -PathType Container) {
                Get-ChildItem -LiteralPath $sourceItem -Recurse -File | ForEach-Object {
                    $relative = $_.FullName.Substring($projectRoot.Length + 1)
                    Sync-SourceFile $_.FullName (Join-Path $BuildRoot $relative)
                }
            } else {
                Sync-SourceFile $sourceItem (Join-Path $BuildRoot $item)
            }
        }
    }
    Write-Host "Build cache: $BuildRoot"
    Push-Location $BuildRoot
    try {
        if ($Action -eq 'Monitor') {
            & $pioExe device monitor --port $Port --baud 115200
            return
        }
        & $pioExe run
        if ($LASTEXITCODE -ne 0) { throw 'Build failed. See the compiler output above.' }
        $output = Join-Path $BuildRoot '.pio/build/esp32-s3-n16r8'
        $artifacts = Join-Path $projectRoot 'artifacts'
        New-Item -ItemType Directory -Path $artifacts -Force | Out-Null
        foreach ($image in @('firmware.bin', 'bootloader.bin', 'partitions.bin', 'srmodels/srmodels.bin')) {
            Copy-Item -LiteralPath (Join-Path $output $image) -Destination $artifacts -Force
        }
        Write-Host "Verified build images: $artifacts"
        if ($Action -eq 'Upload') {
            & $pioExe run --target upload --upload-port $Port
            if ($LASTEXITCODE -ne 0) { throw 'Upload failed.' }
        }
    } finally { Pop-Location }
} finally { $env:PLATFORMIO_CORE_DIR = $previousCore }
