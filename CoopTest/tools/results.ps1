# results.ps1 - summarizes the newest co-op logs (no clicks, read-only).
$game = Split-Path (Split-Path $PSScriptRoot)
Get-Process Xenus -EA SilentlyContinue | Select-Object Id, StartTime, Responding | Format-Table | Out-String
$c = Get-Item "$game\crash.txt" -EA SilentlyContinue; if ($c) { "crash.txt last written $($c.LastWriteTime)" }
Get-ChildItem "$game\CoopTest\bp_coop_*.log" | Sort-Object LastWriteTime | Select-Object -Last 2 | ForEach-Object {
  "=== $($_.Name)"
  Get-Content $_.FullName | Where-Object { $_ -match 'loaded, pid|command line used|lsmLoad|stMode set|mode byte|server replied|Can.t connect|NETREQ|joined|NETMSG_CLIENT|gameSvAttach|spawn fallback|F8:|netguard|vital export missing|process exit|pid sweep|start area|mapLoadSM|Invalid object|no free entity|too many entities|has duplicate|runs New Game|world ready|rejected entity|local pids|creates are rejected|own entities get|send filter|streaming|sync:|weapon:|guest: removed|guest: used|guest: door|guest: bullet|guest: hit|guest: vehicle|guest: own|guest: our|net in:|host: door|host: guest hit|host: our player|host: guest player|host: npcstime:|can now be hit|bullet traces|respawn|0.14:|guest: doors|probe|0.15:|save:|quests:|reputation:|profile:|teleport|admin:|npcs:|taxi|guest: use' }
}







