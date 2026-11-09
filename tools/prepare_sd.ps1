<#
.SYNOPSIS
    Copy the upstream sprite packs onto a microSD card under the per-game roots
    (/tamapoke/mons, /tamadigi/mons, /tamaruto/mons).

.DESCRIPTION
    TamaPoke: copies _repos\TamaPoke-expanded\tools\sdcard\mons.
    TamaDigi: unpacks web/sprites-ver*.pak into tools\sdcard\mons.
    TamaNaruto: unpacks web/sprites-<village>.pak into tools\sdcard\mons.

.EXAMPLE
    .\tools\prepare_sd.ps1 -Drive E:\ -Game pokemon
    .\tools\prepare_sd.ps1 -Drive E:\ -Game all
#>
param(
    [Parameter(Mandatory = $true)]
    [string]$Drive,

    [ValidateSet("pokemon", "digimon", "naruto", "both", "all")]
    [string]$Game = "all",

    # Optional: folder containing the upstream checkouts.
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

$pokeCandidates = @(
    (Join-Path $RepoRoot "TamaPoke-expanded\tools\sdcard\mons"),
    (Join-Path $RepoRoot "TamaPoke-main\tools\sdcard\mons")
)
$pokeSrc = $pokeCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
$digiRepo = Join-Path $RepoRoot "TamaDigi-main"
$narutoRepo = Join-Path $RepoRoot "TamaRuto-main"

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

function Unpack-Mons([string]$Repo, [string]$PackPattern) {
    $ready = Join-Path $Repo "tools\sdcard\mons"
    if (Test-Path $ready) { return $ready }
    $unpacker = Join-Path $Repo "tools\unpack_bundle.py"
    $packs = Get-ChildItem (Join-Path $Repo "web") -Filter $PackPattern -ErrorAction SilentlyContinue
    if (-not (Test-Path $unpacker) -or -not $packs) { return $null }
    $python = Find-Python
    if (-not $python) {
        Write-Host "python 3 not found; cannot unpack $Repo" -ForegroundColor Yellow
        return $null
    }
    Write-Host "unpacking $Repo sprite packs with $unpacker"
    Push-Location $Repo
    try { & $python $unpacker } finally { Pop-Location }
    if (Test-Path $ready) { return $ready }
    return $null
}

$a = $false
$b = $false
$n = $false
if ($Game -in @("pokemon", "both", "all")) {
    $a = Copy-Mons $pokeSrc (Join-Path $driveRoot "tamapoke\mons")
}
if ($Game -in @("digimon", "both", "all")) {
    $digi = Unpack-Mons $digiRepo "sprites-ver*.pak"
    if ($digi) {
        $b = Copy-Mons $digi (Join-Path $driveRoot "tamadigi\mons")
    } else {
        Write-Host "TamaDigi sprites missing: run python tools\unpack_bundle.py in $digiRepo" -ForegroundColor Yellow
    }
}
if ($Game -in @("naruto", "all")) {
    $naruto = Unpack-Mons $narutoRepo "sprites-*.pak"
    if ($naruto) {
        $n = Copy-Mons $naruto (Join-Path $driveRoot "tamaruto\mons")
    } else {
        Write-Host "TamaNaruto sprites missing: run python tools\unpack_bundle.py in $narutoRepo" -ForegroundColor Yellow
    }
}

Write-Host ""
Write-Host "done. expected layout:" -ForegroundColor Green
Write-Host "  $driveRoot" + "tamapoke\mons\*.bin"
Write-Host "  $driveRoot" + "tamadigi\mons\*.bin"
Write-Host "  $driveRoot" + "tamaruto\mons\*.bin"
if (-not $a -and -not $b -and -not $n) { exit 1 }
