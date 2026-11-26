# start_client.ps1 - starts a copy of the game that joins a host. No clicks: you click New Game yourself.
#   -hostIp: address of the host PC (127.0.0.1 = same PC)
param([string]$hostIp = '127.0.0.1', [string]$name = 'GUEST')
$game = Split-Path $PSScriptRoot
$ini = "$game\bp_coop.ini"
(Get-Content $ini) -replace '^mode=.*', 'mode=join' -replace '^name=.*', "name=$name" -replace '^host_ip=.*', "host_ip=$hostIp" | Set-Content $ini
$p = Start-Process "$game\Xenus.exe" -WorkingDirectory $game -PassThru
Start-Sleep 3
"Client started (pid $($p.Id)), joining $hostIp. Log: CoopTest\bp_coop_$($p.Id).log"
