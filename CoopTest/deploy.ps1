# deploy.ps1 - copies the current mod build to every game folder listed in CoopTest\sync_targets.txt
# (one folder per line, # = comment). build.ps1 runs this after a successful build.
# Copied: dinput.dll, Coop Launcher.cmd, CoopTest scripts/source. bp_coop.ini: only MISSING keys are added,
# so each folder keeps its own mode / name / host_ip.
$ErrorActionPreference = 'Continue'
$src  = Split-Path $PSScriptRoot                # this game folder
$list = Join-Path $PSScriptRoot 'sync_targets.txt'
if (-not (Test-Path $list)) { return }
$dll = Join-Path $PSScriptRoot 'bin\dinput.dll'

function Merge-Ini([string]$from, [string]$to) {
    if (-not (Test-Path $to)) { Copy-Item $from $to; return 'copied' }
    $have = @{}; foreach ($l in Get-Content $to) { if ($l -match '^([A-Za-z0-9_]+)=') { $have[$Matches[1]] = 1 } }
    $add = @(); $pending = @()
    foreach ($l in Get-Content $from) {
        if ($l -match '^;') { $pending += $l; continue }
        if ($l -match '^([A-Za-z0-9_]+)=' -and -not $have[$Matches[1]]) { $add += $pending; $add += $l }
        $pending = @()
    }
    if ($add) { Add-Content -Path $to -Value $add -Encoding ASCII; return "added $(@($add | Where-Object { $_ -notmatch '^;' }).Count) keys" }
    return 'up to date'
}

foreach ($t in Get-Content $list) {
    $t = $t.Trim(); if (-not $t -or $t.StartsWith('#')) { continue }
    if ((Resolve-Path $t -ErrorAction SilentlyContinue).Path -eq (Resolve-Path $src).Path) { continue }
    if (-not (Test-Path (Join-Path $t 'Xenus.exe'))) { "deploy: skipped $t (no Xenus.exe there / folder not found)"; continue }
    $dst = Join-Path $t 'dinput.dll'
    try { Copy-Item $dll $dst -Force -ErrorAction Stop }
    catch { Rename-Item $dst ("dinput_old" + (Get-Random) + ".dll") -ErrorAction SilentlyContinue; Copy-Item $dll $dst -Force }
    New-Item -ItemType Directory -Force (Join-Path $t 'CoopTest\src'), (Join-Path $t 'CoopTest\tools') | Out-Null
    foreach ($f in 'launcher.ps1', 'start_both.ps1', 'start_host.ps1', 'start_client.ps1', 'build.ps1', 'deploy.ps1') {
        Copy-Item (Join-Path $PSScriptRoot $f) (Join-Path $t "CoopTest\$f") -Force -ErrorAction SilentlyContinue }
    foreach ($f in 'results.ps1', 'firewall.ps1') { Copy-Item (Join-Path $PSScriptRoot "tools\$f") (Join-Path $t "CoopTest\tools\$f") -Force }
    Copy-Item (Join-Path $PSScriptRoot 'src\*') (Join-Path $t 'CoopTest\src') -Force
    foreach ($f in 'Coop Launcher.cmd', 'coop_autoexec.example.txt') { Copy-Item (Join-Path $src $f) (Join-Path $t $f) -Force -ErrorAction SilentlyContinue }
    $r = Merge-Ini (Join-Path $src 'bp_coop.ini') (Join-Path $t 'bp_coop.ini')
    "deploy: $t updated (bp_coop.ini $r)"
}
