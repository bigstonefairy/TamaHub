<#
.SYNOPSIS
    Make the ESP32 toolchain usable when the Windows profile path is not ASCII.

.DESCRIPTION
    arduino-cli passes `-L` / `-I` paths straight to the linker.  On Windows a
    profile such as C:\Users\레오\... is mangled to C:\Users\????\... and the link
    fails with "cannot find -lpthread" (or the .elf cannot be created).

    The fix is to run the build from ASCII directories:

      * a junction for the repository      C:\Users\Public\tamahub
      * a REAL copy of the Arduino data    C:\Users\Public\Arduino15
      * a REAL copy of the user libraries  C:\Users\Public\ArduinoUser

    A junction is not enough for the Arduino data directory: arduino-cli
    resolves symlinks while scanning `packages`, and the resolved target still
    contains the Korean path, so it reports the platform as missing.  The
    hardware/toolchain files are therefore copied once (about 1.8 GB for the
    ESP32-S3 subset).

    Dot-source this file so it can export the environment variables:

        . .\tools\setup_windows_toolchain.ps1
        .\tools\build_all.ps1

    `build_all.ps1` does this automatically.

.EXAMPLE
    .\tools\setup_windows_toolchain.ps1
    # prints the paths and the environment variables to export
#>
param(
    [string]$StagingRoot = "C:\Users\Public",
    [string]$DataDir = "",
    [string]$UserDir = ""
)

$ErrorActionPreference = "Stop"
$RepoRoot = Split-Path -Parent $PSScriptRoot

function Test-Ascii([string]$Value) {
    foreach ($ch in $Value.ToCharArray()) {
        if ([int]$ch -gt 127) { return $false }
    }
    return $true
}

# Default toolchain locations when the caller does not pass anything.
if (-not $DataDir) {
    if ($env:ARDUINO_DIRECTORIES_DATA) { $DataDir = $env:ARDUINO_DIRECTORIES_DATA }
    else { $DataDir = Join-Path $env:LOCALAPPDATA "Arduino15" }
}
if (-not $UserDir) {
    if ($env:ARDUINO_DIRECTORIES_USER) { $UserDir = $env:ARDUINO_DIRECTORIES_USER }
    else { $UserDir = Join-Path $env:USERPROFILE "Documents\Arduino" }
}

$repoAscii = Test-Ascii $RepoRoot
$dataAscii = Test-Ascii $DataDir
$userAscii = Test-Ascii $UserDir

# Nothing to do: build in place.
if ($repoAscii -and $dataAscii -and $userAscii) {
    $env:ARDUINO_DIRECTORIES_DATA = $DataDir
    $env:ARDUINO_DIRECTORIES_USER = $UserDir
    $global:TamahubSketchRoot = $RepoRoot
    $global:TamahubBuildRoot = Join-Path $RepoRoot "build"
    Write-Host "toolchain paths are ASCII; building in place." -ForegroundColor Green
    return
}

$asciiRepo = Join-Path $StagingRoot "tamahub"
$asciiData = Join-Path $StagingRoot "Arduino15"
$asciiUser = Join-Path $StagingRoot "ArduinoUser"

Write-Host "non-ASCII path detected; staging the build under $StagingRoot" -ForegroundColor Yellow

# ---------------------------------------------------------------- repository --
# A junction's Target is the *real* path, so compare against the resolved repo
# root.  Running the script from inside the junction would otherwise look like a
# mismatch and "repoint" the junction onto itself, which deletes it.
$repoReal = @((Get-Item $RepoRoot -Force).Target)[0]
if (-not $repoReal) { $repoReal = (Resolve-Path -LiteralPath $RepoRoot).Path }

$existing = Get-Item $asciiRepo -Force -ErrorAction SilentlyContinue
if ($existing) {
    if (-not $existing.LinkType) {
        throw "$asciiRepo exists and is not a junction. Remove it or pass -StagingRoot."
    }
    if (@($existing.Target)[0] -ne $repoReal) {
        Write-Host "repointing $asciiRepo -> $repoReal"
        cmd /c rd $asciiRepo | Out-Null
        $existing = $null
    }
}
if (-not $existing) {
    if ($asciiRepo -eq $repoReal) {
        throw "cannot stage ${asciiRepo}: it is the repository itself (run from the real path)."
    }
    cmd /c mklink /J $asciiRepo $repoReal | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "mklink failed for $asciiRepo" }
}
Write-Host "  repo      $asciiRepo -> $repoReal"

# --------------------------------------------------------------- arduino data --
$needData = -not (Test-Path (Join-Path $asciiData "packages\esp32\hardware\esp32"))
if ($needData) {
    Write-Host "  copying the ESP32 toolchain to $asciiData (one time, ~2 GB)"
    New-Item -ItemType Directory -Force -Path $asciiData | Out-Null
    foreach ($file in "inventory.yaml", "package_index.json", "package_index.json.sig",
                      "package_esp32_index.json", "library_index.json", "library_index.json.sig") {
        $src = Join-Path $DataDir $file
        if (Test-Path $src) { Copy-Item $src (Join-Path $asciiData $file) -Force }
    }
    robocopy (Join-Path $DataDir "packages\esp32\hardware") `
             (Join-Path $asciiData "packages\esp32\hardware") /E /NFL /NDL /NJH /NJS | Out-Null
    # Only the toolchains this project uses.  The other chip families would add
    # another 4 GB for no benefit.
    foreach ($tool in "esp-x32", "esp32s3-libs", "esptool_py", "mkspiffs", "mklittlefs") {
        $src = Join-Path $DataDir "packages\esp32\tools\$tool"
        if (Test-Path $src) {
            robocopy $src (Join-Path $asciiData "packages\esp32\tools\$tool") /E /NFL /NDL /NJH /NJS | Out-Null
        }
    }
}

# ----------------------------------------------------------------- libraries --
$needLibs = -not (Test-Path (Join-Path $asciiUser "libraries\SensorLib"))
if ($needLibs) {
    Write-Host "  copying user libraries to $asciiUser\libraries"
    robocopy (Join-Path $UserDir "libraries") (Join-Path $asciiUser "libraries") /E /NFL /NDL /NJH /NJS | Out-Null
}

$env:ARDUINO_DIRECTORIES_DATA = $asciiData
$env:ARDUINO_DIRECTORIES_USER = $asciiUser
$global:TamahubSketchRoot = $asciiRepo
$global:TamahubBuildRoot = Join-Path $StagingRoot "tamahub-build"

Write-Host "  data      $asciiData" -ForegroundColor Green
Write-Host "  libraries $asciiUser" -ForegroundColor Green
Write-Host "  build     $global:TamahubBuildRoot" -ForegroundColor Green
