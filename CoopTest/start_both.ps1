# start_both.ps1 - starts the host copy, then the client copy, on this PC. No clicks: you do those.
#   Host window:   Load Game (your save) or New Game, wait until you are in the game.
#   Client window: New Game (or Load Game) -> it joins the host by itself.
param([int]$delay = 8)
Get-Process Xenus -ErrorAction SilentlyContinue | Stop-Process -Force   # old copies would talk to the new ones
Start-Sleep 2
& "$PSScriptRoot\start_host.ps1"
Start-Sleep $delay
& "$PSScriptRoot\start_client.ps1"
"Both started. Host first: get in-game there, then start the game in the client window."
