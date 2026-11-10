<#
.SYNOPSIS
    Build TamaHub + all games and verify the partition layout.

.DESCRIPTION
    Hub is built with PartitionScheme=custom, so Arduino copies
    hub/partitions.csv (the frozen 16 MB layout) into the build.
    Games are built with the upstream app3M_fat9M_16MB scheme; their app
    images are relocatable and are flashed into ota_0 / ota_1 by offset.

    Nothing here flashes a board. See README.md for the offsets / web installer.

.EXAMPLE
    .\tools\build_all.ps1
    .\tools\build_all.ps1 -Installer
    .\tools\build_all.ps1 -Clean -SkipGames
#>
param(
    [string]$ArduinoCli = "",
    [string]$BuildDir = "",
    [switch]$SkipHub,
    [switch]$SkipGames,
    [switch]$Installer,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot

# arduino-cli passes paths to the linker unquoted-ish; a non-ASCII profile path
# (C:\Users\레오\...) turns into C:\Users\????\... and the link fails.  Dot-source
# the helper so the build runs from ASCII directories when needed.
. (Join-Path $PSScriptRoot "setup_windows_toolchain.ps1")
$SketchRoot = $global:TamahubSketchRoot
if (-not $BuildDir) { $BuildDir = $global:TamahubBuildRoot }
Set-Location $Root

function Fail($message) {
    Write-Host "ERROR: $message" -ForegroundColor Red
    exit 1
}

function Find-Cli {
    if ($ArduinoCli) {
        if (-not (Test-Path $ArduinoCli)) { Fail "arduino-cli not found: $ArduinoCli" }
        return (Resolve-Path $ArduinoCli).Path
    }
    $local = Join-Path $Root "tools\bin\arduino-cli.exe"
    if (Test-Path $local) { return $local }
    $cmd = Get-Command arduino-cli -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    Fail "arduino-cli not found. Put it in tools\bin or pass -ArduinoCli."
}

function Find-Python {
    # The Microsoft Store 'python.exe' in WindowsApps is only an alias that
    # opens the store, so every candidate is actually executed before use.
    $candidates = @()
    if ($env:TAMAHUB_PYTHON) { $candidates += $env:TAMAHUB_PYTHON }
    $candidates += Get-ChildItem (Join-Path $env:LOCALAPPDATA "Programs\Python") `
        -Filter python.exe -Recurse -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty FullName
    foreach ($name in @("python", "py")) {
        $cmd = Get-Command $name -ErrorAction SilentlyContinue
        if ($cmd) { $candidates += $cmd.Source }
    }
    foreach ($candidate in $candidates) {
        if ($candidate -like "*\WindowsApps\*") { continue }
        if (-not (Test-Path $candidate)) { continue }
        $major = & $candidate -c "import sys; print(sys.version_info[0])" 2>$null
        if ($LASTEXITCODE -eq 0 -and "$major".Trim() -eq "3") {
            return @($candidate)
        }
    }
    Fail "python 3 not found (needed for the layout / installer scripts)."
}

function Invoke-Python {
    param([string[]]$Arguments)
    $py = @(Find-Python)
    if ($py.Count -gt 1) {
        & $py[0] $py[1] @Arguments
    } else {
        & $py[0] @Arguments
    }
    if ($LASTEXITCODE -ne 0) { Fail "python failed with exit code $LASTEXITCODE" }
}

$cli = Find-Cli
Write-Host "arduino-cli: $cli"

$coreList = (& $cli core list | Out-String)
if ($coreList -notmatch "esp32:esp32") {
    Fail @"
ESP32 core is not installed. Run:
  $cli core update-index --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
  $cli core install esp32:esp32@3.3.11 --additional-urls https://espressif.github.io/arduino-esp32/package_esp32_index.json
  $cli lib install "GFX Library for Arduino@1.6.7" "SensorLib@0.4.1" "XPowersLib@0.3.3" "U8g2@2.36.19"
"@
}

if ($Clean -and (Test-Path $BuildDir)) {
    Write-Host "cleaning $BuildDir"
    Remove-Item $BuildDir -Recurse -Force
}
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

# Keep the sketch-local copy of the table in sync with the source of truth.
Copy-Item (Join-Path $Root "partitions.csv") (Join-Path $Root "hub\partitions.csv") -Force

$fqbnCommon = "CDCOnBoot=cdc,FlashSize=16M,PSRAM=opi"
$fqbnHub = "esp32:esp32:esp32s3:$fqbnCommon,PartitionScheme=custom"
$fqbnGame = "esp32:esp32:esp32s3:$fqbnCommon,PartitionScheme=app3M_fat9M_16MB"

if (-not $SkipHub) {
    Write-Host "`n=== TamaHub (factory, custom partitions) ===" -ForegroundColor Cyan
    & $cli compile --fqbn $fqbnHub --build-path (Join-Path $BuildDir "hub") (Join-Path $SketchRoot "hub")
    if ($LASTEXITCODE -ne 0) { Fail "hub build failed" }
}

if (-not $SkipGames) {
    foreach ($game in @("TamaPoke", "TamaDigi", "TamaNaruto")) {
        Write-Host "`n=== $game (ota slot, app3M scheme) ===" -ForegroundColor Cyan
        & $cli compile --fqbn $fqbnGame --build-path (Join-Path $BuildDir $game) (Join-Path $SketchRoot "games\$game")
        if ($LASTEXITCODE -ne 0) { Fail "$game build failed" }
    }

    # Apps live under apps/ so they are not confused with the three games.
    Write-Host "`n=== TamaClock (ota slot, app3M scheme) ===" -ForegroundColor Cyan
    & $cli compile --fqbn $fqbnGame --build-path (Join-Path $BuildDir "TamaClock") (Join-Path $SketchRoot "apps\TamaClock")
    if ($LASTEXITCODE -ne 0) { Fail "TamaClock build failed" }

    Write-Host "`n=== TamaNav (ota slot, app3M scheme) ===" -ForegroundColor Cyan
    & $cli compile --fqbn $fqbnGame --build-path (Join-Path $BuildDir "TamaNav") (Join-Path $SketchRoot "apps\TamaNav")
    if ($LASTEXITCODE -ne 0) { Fail "TamaNav build failed" }
}

$binArgs = @()
if (Test-Path (Join-Path $BuildDir "hub\hub.ino.bin")) {
    $binArgs += "--bin", "factory=$(Join-Path $BuildDir 'hub\hub.ino.bin')"
}
foreach ($pair in @(@("TamaPoke", "ota_0"), @("TamaDigi", "ota_1"), @("TamaNaruto", "ota_2"), @("TamaClock", "ota_3"), @("TamaNav", "ota_4"))) {
    $bin = Join-Path $BuildDir "$($pair[0])\$($pair[0]).ino.bin"
    if (Test-Path $bin) { $binArgs += "--bin", "$($pair[1])=$bin" }
}

Write-Host "`n=== layout / size check ===" -ForegroundColor Cyan
Invoke-Python (@((Join-Path $Root "tools\check_layout.py")) + $binArgs)

if ($Installer) {
    Write-Host "`n=== installer ===" -ForegroundColor Cyan
    Invoke-Python @((Join-Path $Root "tools\make_installer.py"), "--build-dir", $BuildDir)
    Invoke-Python @((Join-Path $Root "tools\check_installer.py"), "--dist", (Join-Path $Root "web\dist"))
}

Write-Host "`nBuild OK." -ForegroundColor Green
