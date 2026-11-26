# start_host.ps1 - starts a copy of the game that hosts the co-op session. No clicks: you click New Game yourself.
param([string]$name = 'HOST')
$game = Split-Path $PSScriptRoot
$ini = "$game\bp_coop.ini"
(Get-Content $ini) -replace '^mode=.*', 'mode=host' -replace '^name=.*', "name=$name" | Set-Content $ini
$p = Start-Process "$game\Xenus.exe" -WorkingDirectory $game -PassThru
Start-Sleep 3   # the mod reads bp_coop.ini when the game starts; wait before anything else changes it
"Host started (pid $($p.Id)). Log: CoopTest\bp_coop_$($p.Id).log"
