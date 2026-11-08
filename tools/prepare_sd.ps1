<#
.SYNOPSIS
    Copy the upstream sprite packs onto a microSD card under the new
    per-game roots (/tamapoke/mons, /tamadigi/mons).

.DESCRIPTION
    TamaPoke: copies _repos\TamaPoke-main\tools\sdcard\mons.
    TamaDigi: looks for a ready mons folder; if there is none, prints how to
              unpack the version packs with the upstream tools.

.EXAMPLE
    .\tools\prepare_sd.ps1 -Drive E:\ -Game pokemon
    .\tools\prepare_sd.ps1 -Drive E:\ -Game both
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Drive,

    [ValidateSet("pokemon", "digimon", "both")]
    [string]$Game = "both",

    # Optional: folder containing the upstream checkouts (TamaPoke-main, TamaDigi-main).
    [string]$RepoRoot = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $PSScriptRoot
if (-not $RepoRoot) { $RepoRoot = Join-Path $Root "_repos" }

$driveRoot = $Drive.TrimEnd('\', '/') + "\"
if (-not (Test-Path $driveRoot)) { throw "drive not found: $driveRoot" }

function Copy-Mons([string]$source, [string]$dest) {
    if (-not (Test-Path $source)) {
        Write-Host "missing: $source" -ForegroundColor Yellow
        return $false
    }
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    Write-Host "copying $source -> $dest"
    Copy-Item (Join-Path $source "*") $dest -Recurse -Force
    return $true
}

$pokeSrc = Join-Path $RepoRoot "TamaPoke-main\tools\sdcard\mons"
$digiRepo = Join-Path $RepoRoot "TamaDigi-main"
$digiCandidates = @(
    (Join-Path $digiRepo "tools\sdcard\mons"),
    (Join-Path $digiRepo "assets\mons"),
    (Join-Path $Root "assets\digimon\mons")
)

function Find-Python {
    $candidates = @()
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
        if ($LASTEXITCODE -eq 0 -and "$major".Trim() -eq "3") { return $candidate }
    }
    return $null
}

$a = $false
$b = $false
if ($Game -in @("pokemon", "both")) {
    $a = Copy-Mons $pokeSrc (Join-Path $driveRoot "tamapoke\mons")
}
if ($Game -in @("digimon", "both")) {
    $digi = $digiCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1

    # TamaDigi ships sprites as web/sprites-ver*.pak; its own tool restores
    # tools/sdcard/mons from them.  Run it automatically when the checkout is
    # present so the user does not have to know the pack format.
    if (-not $digi) {
        $unpacker = Join-Path $digiRepo "tools\unpack_bundle.py"
        $packs = Get-ChildItem (Join-Path $digiRepo "web") -Filter "sprites-ver*.pak" `
            -ErrorAction SilentlyContinue
        if ((Test-Path $unpacker) -and $packs) {
            $python = Find-Python
            if ($python) {
                Write-Host "unpacking TamaDigi sprite packs with $unpacker"
                Push-Location $digiRepo
                try { & $python $unpacker } finally { Pop-Location }
                $digi = Join-Path $digiRepo "tools\sdcard\mons"
            } else {
                Write-Host "python 3 not found; cannot unpack the TamaDigi packs" -ForegroundColor Yellow
            }
        }
    }

    if ($digi -and (Test-Path $digi)) {
        $b = Copy-Mons $digi (Join-Path $driveRoot "tamadigi\mons")
    } else {
        Write-Host ""
        Write-Host "TamaDigi sprites are distributed as version packs, not as a raw folder." -ForegroundColor Yellow
        Write-Host "From an upstream TamaDigi checkout, run its own tool (no arguments):"
        Write-Host "  python tools\unpack_bundle.py"
        Write-Host "It restores tools\sdcard\mons; then copy that folder to $driveRoot\tamadigi\mons\"
        Write-Host "Alternatively use the game's serial uploader (PUT) with the /tamadigi/mons prefix."
    }
}

Write-Host ""
Write-Host "done. expected layout:" -ForegroundColor Green
Write-Host "  $driveRoot" + "tamapoke\mons\*.bin"
Write-Host "  $driveRoot" + "tamadigi\mons\*.bin"
if (-not $a -and -not $b) { exit 1 }
