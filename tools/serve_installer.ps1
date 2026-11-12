<#
.SYNOPSIS
    Serve this repo's web installer on this PC at a fixed address.

.DESCRIPTION
    The GitHub Pages copy of the installer only changes once the CI run
    finishes (15-25 min after a push).  This script serves the same page
    straight out of web/dist, so a build that was just made is installable
    immediately:

        http://localhost:8000/

    Web Serial treats http://localhost as a secure context, so Chrome/Edge
    show the same install buttons as on the public site.  Everything the page
    needs (binaries, manifests, SD packs) is read from web/dist, so no network
    delay and no stale cache: the bytes on the card are the ones this PC just
    built.

.EXAMPLE
    .\tools\serve_installer.ps1
    # serve what is already in web/dist

.EXAMPLE
    .\tools\serve_installer.ps1 -Build
    # build all five apps + regenerate web/dist, then serve it

.EXAMPLE
    .\tools\serve_installer.ps1 -Build -Port 8080
#>
param(
    [int]$Port = 8000,
    [switch]$Build,
    [switch]$NoOpen
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
$Dist = Join-Path $Root "web\dist"

function Fail($message) {
    Write-Host "ERROR: $message" -ForegroundColor Red
    exit 1
}

function Find-Python {
    # The Microsoft Store python.exe in WindowsApps is only an alias that opens
    # the store, so every candidate is actually executed before use.
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
            return $candidate
        }
    }
    Fail "python 3 not found (the built-in http.server needs it)."
}

if ($Build) {
    Write-Host "=== building all apps + web/dist ==="
    & powershell -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $PSScriptRoot "build_all.ps1") -Installer
    if ($LASTEXITCODE -ne 0) { Fail "build_all.ps1 failed; not serving a stale installer." }
}

if (-not (Test-Path (Join-Path $Dist "index.html"))) {
    Fail "web/dist/index.html is missing. Run: .\tools\build_all.ps1 -Installer"
}

# A fixed address is the whole point: the user bookmarks it.  Refuse to move to
# another port silently, or the bookmark would keep serving the old build.
$busy = Get-NetTCPConnection -State Listen -LocalPort $Port -ErrorAction SilentlyContinue
if ($busy) {
    Fail "port $Port is already in use. Close the other server, or pass -Port <n>."
}

$py = Find-Python
$url = "http://localhost:$Port/"

Write-Host ""
Write-Host "  installer : $url" -ForegroundColor Green
Write-Host "  serving   : $Dist"
$stamp = Get-Item (Join-Path $Dist "tamaclock.bin")
Write-Host ("  tamaclock : {0:N0} bytes  {1}" -f $stamp.Length, $stamp.LastWriteTime)
Write-Host "  stop with : Ctrl+C"
Write-Host ""

# Give the server a moment to bind, then open the page.  The browser is started
# from a one-shot job so python can stay in the foreground and Ctrl+C works the
# way it does for any other command.
$opener = $null
if (-not $NoOpen) {
    $opener = Start-Job -ScriptBlock {
        param($u)
        Start-Sleep -Seconds 2
        Start-Process $u
    } -ArgumentList $url
}

try {
    & $py -m http.server $Port --bind 127.0.0.1 --directory $Dist
} finally {
    if ($opener) { Remove-Job $opener -Force -ErrorAction SilentlyContinue }
}
