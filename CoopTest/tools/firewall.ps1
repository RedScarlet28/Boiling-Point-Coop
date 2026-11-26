# firewall.ps1 - lets another PC reach the host (UDP 15011 and 15013). Run once on the HOST PC, as administrator.
#   firewall.ps1          add the rule
#   firewall.ps1 -remove  remove it
param([switch]$remove)
$name = 'Boiling Point co-op (UDP 15011, 15013)'
if ($remove) { Remove-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue; "removed: $name"; return }
if (Get-NetFirewallRule -DisplayName $name -ErrorAction SilentlyContinue) { "already there: $name"; return }
New-NetFirewallRule -DisplayName $name -Direction Inbound -Protocol UDP -LocalPort 15011,15013 -Action Allow -Profile Any | Out-Null
"added: $name"
"Your local IP addresses (give one to the other player):"
Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
  ForEach-Object { "  $($_.IPAddress)  ($($_.InterfaceAlias))" }

