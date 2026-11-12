<#
.SYNOPSIS
    Rewrite the whole TamaHub flash layout over USB, even from a dead device.

.DESCRIPTION
    Recovery for the "black screen, nothing boots" state: bootloader, partition
    table, TamaHub (factory) and all five slots are rewritten from web/dist.

    Nothing has to be running on the badge for this to work -- esptool talks to
    the ESP32-S3 ROM loader, which is always there.  The script uses
    "usb-reset" (the USB-Serial/JTAG reset sequence) instead of the usual DTR/
    RTS lines, which is what makes it work when the device is stuck in a boot
    loop or sitting in download mode.

    NVS is NOT erased, so saves survive.  A device that was already erased
    keeps its blank NVS and starts fresh (the SD card is untouched).

.EXAMPLE
    .\tools\repair_device.ps1
    # port auto-detected, binaries taken from web/dist

.EXAMPLE
    .\tools\repair_device.ps1 -Build
    # rebuild all five apps + web/dist first, then repair

.EXAMPLE
    .\tools\repair_device.ps1 -Port COM7
#>
param(
    [string]$Port = "",
    [switch]$Build,
    [switch]$SkipApps
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Dist = Join-Path $Root "web\dist"

# Same table as partitions.csv / web/index.html.  Keep all three in step.
$LAYOUT = @(
    @{ bin = "bootloader.bin";  offset = 0x000000; name = "bootloader" },
    @{ bin = "partitions.bin";  offset = 0x008000; name = "partition table"; magic = 0xAA },
    @{ bin = "tamahub.bin";     offset = 0x020000; name = "TamaHub (factory)" },
    @{ bin = "tamaclock.bin";   offset = 0x220000; name = "Clock    (ota_0)" },
    @{ bin = "idlepoke.bin";    offset = 0x420000; name = "IdlePoke (ota_1)" },
    @{ bin = "idledigi.bin";    offset = 0x620000; name = "IdleDigi (ota_2)" },
    @{ bin = "tamapoke.bin";    offset = 0x820000; name = "TamaPoke (ota_3)" },
    @{ bin = "tamadigi.bin";    offset = 0xA20000; name = "TamaDigi (ota_4)" }
)

function Fail($message) {
    Write-Host ""
    Write-Host "ERROR: $message" -ForegroundColor Red
    exit 1
}

function Find-Esptool {
    $candidates = @()
    $candidates += Get-ChildItem "$env:LOCALAPPDATA\Arduino15\packages\esp32\tools\esptool_py" `
        -Recurse -Filter esptool.exe -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -ExpandProperty FullName
    $candidates += Get-ChildItem "C:\Users\Public\Arduino15\packages\esp32\tools\esptool_py" `
        -Recurse -Filter esptool.exe -ErrorAction SilentlyContinue |
        Sort-Object FullName | Select-Object -ExpandProperty FullName
    $candidates += Join-Path $Root "tools\bin\esptool.exe"
    foreach ($c in $candidates) {
        if (Test-Path $c) { return $c }
    }
    Fail "esptool.exe not found. Install the ESP32 core with the Arduino IDE, or pass one via tools\bin."
}

# Every ESP32-S3 that exposes USB-Serial/JTAG is VID_303A PID_1001.  That is
# also the badge's own CDC port, which is fine: the ROM loader answers on the
# same interface after a usb-reset.
function Find-Port {
    $ports = @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue |
        Where-Object { $_.PNPDeviceID -like "*VID_303A&PID_1001*" })
    if ($ports.Count -eq 1) { return $ports[0].DeviceID }
    $all = @(Get-CimInstance Win32_SerialPort -ErrorAction SilentlyContinue)
    if ($ports.Count -gt 1) {
        $names = ($ports | ForEach-Object { $_.DeviceID }) -join ", "
        Fail "several ESP32-S3 boards found ($names). Pass -Port COMx."
    }
    if ($all.Count -eq 1) { return $all[0].DeviceID }
    $names = if ($all.Count) { ($all | ForEach-Object { $_.DeviceID }) -join ", " } else { "none" }
    Fail "no badge found (serial ports: $names). Plug in USB, or press BOOT and reconnect."
}

if ($Build) {
    Write-Host "=== building all apps + web/dist ==="
    & powershell -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $PSScriptRoot "build_all.ps1") -Installer
    if ($LASTEXITCODE -ne 0) { Fail "build_all.ps1 failed." }
}

$parts = if ($SkipApps) { $LAYOUT[0..2] } else { $LAYOUT }
foreach ($part in $parts) {
    $path = Join-Path $Dist $part.bin
    if (-not (Test-Path $path)) {
        Fail "$($part.bin) is missing from web/dist. Run: .\tools\build_all.ps1 -Installer"
    }
}

$esp = Find-Esptool
if (-not $Port) { $Port = Find-Port }

Write-Host ""
Write-Host "  esptool : $esp"
Write-Host "  port    : $Port"
Write-Host "  source  : $Dist"
Write-Host ""

$cliArgs = @("--port", $Port, "--before", "usb-reset", "--after", "no-reset",
          "write-flash")
foreach ($part in $parts) {
    $path = Join-Path $Dist $part.bin
    $size = (Get-Item $path).Length
    Write-Host ("  {0,-22} 0x{1:X6}  {2,9:N0} bytes" -f $part.name, $part.offset, $size)
    $cliArgs += ("0x{0:X}" -f $part.offset)
    $cliArgs += $path
}
Write-Host ""

& $esp @cliArgs
if ($LASTEXITCODE -ne 0) {
    Fail @"
write-flash failed. Nothing on the badge was erased by this script, so simply
run it again.  If the port disappears, unplug USB, hold BOOT, reconnect, and
re-run.
"@
}

# The write is only useful if the ROM can actually load what we put there, so
# read the first bytes of every app back and check the ESP image magic (0xE9).
Write-Host ""
Write-Host "=== verifying ==="
$tmp = Join-Path $env:TEMP ("tamahub-repair-" + [guid]::NewGuid().ToString("N") + ".bin")
$bad = 0
foreach ($part in $parts) {
    & $esp --port $Port --before no-reset --after no-reset `
        read-flash ("0x{0:X}" -f $part.offset) 0x10 $tmp 2>&1 | Out-Null
    $magic = [IO.File]::ReadAllBytes($tmp)[0]
    $want = if ($part.ContainsKey('magic')) { $part.magic } else { 0xE9 }
    $ok = ($magic -eq $want)
    Write-Host ("  {0,-22} {1}" -f $part.name, $(if ($ok) { "OK" } else { "MISSING (0x{0:X2})" -f $magic })) `
        -ForegroundColor $(if ($ok) { "Green" } else { "Red" })
    if (-not $ok) { $bad++ }
}
Remove-Item $tmp -ErrorAction SilentlyContinue
if ($bad) { Fail "$bad part(s) did not read back as an app image." }

# Leave the chip running the app instead of the ROM loader.  A watchdog reset
# is the one reset that works over USB-Serial/JTAG without re-enumerating twice.
Write-Host ""
Write-Host "=== resetting into the app ==="
& $esp --port $Port --before no-reset --after watchdog-reset chip-id 2>&1 | Out-Null

Write-Host ""
Write-Host "Repair OK: TamaHub should be on screen now." -ForegroundColor Green
Write-Host "If it is still black, unplug USB, hold BOOT, reconnect and run this again." -ForegroundColor DarkGray
