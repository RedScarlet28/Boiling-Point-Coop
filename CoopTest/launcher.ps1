# launcher.ps1 - Boiling Point co-op launcher (host or join). Started by "Coop Launcher.cmd" in the game folder.
Add-Type -AssemblyName System.Windows.Forms, System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()
$game = Split-Path $PSScriptRoot
$ini  = Join-Path $game 'bp_coop.ini'
$exe  = Join-Path $game 'Xenus.exe'

function Get-IniValue([string]$key, [string]$def) {
    if (-not (Test-Path $ini)) { return $def }
    $m = Select-String -Path $ini -Pattern "^$key=(.*)$" | Select-Object -First 1
    if ($m) { return $m.Matches[0].Groups[1].Value.Trim() } else { return $def }
}
function Set-IniValues([hashtable]$vals) {
    $lines = [System.Collections.Generic.List[string]](Get-Content $ini)
    foreach ($k in $vals.Keys) {
        $i = $lines.FindIndex([Predicate[string]]{ param($l) $l -match "^$k=" })
        if ($i -ge 0) { $lines[$i] = "$k=$($vals[$k])" } else { $lines.Add("$k=$($vals[$k])") }
    }
    Set-Content -Path $ini -Value $lines -Encoding ASCII
}
function Get-LocalIPs {
    Get-NetIPAddress -AddressFamily IPv4 -ErrorAction SilentlyContinue |
        Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' } |
        ForEach-Object { "$($_.IPAddress)   ($($_.InterfaceAlias))" }
}
function Test-Admin { ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator) }
$fwName = 'Boiling Point co-op (UDP 15011-15013)'

# ---------------- window ----------------
$f = New-Object Windows.Forms.Form
$f.Text = 'Boiling Point Co-op Launcher (early alpha)'
$f.Size = New-Object Drawing.Size(520, 560); $f.StartPosition = 'CenterScreen'
$f.FormBorderStyle = 'FixedDialog'; $f.MaximizeBox = $false
$font = New-Object Drawing.Font('Segoe UI', 10); $f.Font = $font

function Add-Label($text, $x, $y, $w = 460, $h = 22) {
    $l = New-Object Windows.Forms.Label; $l.Text = $text; $l.Location = New-Object Drawing.Point($x, $y)
    $l.Size = New-Object Drawing.Size($w, $h); $f.Controls.Add($l); $l
}

$title = Add-Label 'Boiling Point: Road to Hell - Co-op' 20 12 460 28
$title.Font = New-Object Drawing.Font('Segoe UI', 14, [Drawing.FontStyle]::Bold)

$rbHost = New-Object Windows.Forms.RadioButton; $rbHost.Text = 'Host a game (my world)'; $rbHost.Location = New-Object Drawing.Point(24, 52); $rbHost.Size = New-Object Drawing.Size(220, 24)
$rbJoin = New-Object Windows.Forms.RadioButton; $rbJoin.Text = 'Join a friend'; $rbJoin.Location = New-Object Drawing.Point(260, 52); $rbJoin.Size = New-Object Drawing.Size(200, 24)
$f.Controls.AddRange(@($rbHost, $rbJoin))

Add-Label 'Player name:' 24 90 120 | Out-Null
$tbName = New-Object Windows.Forms.TextBox; $tbName.Location = New-Object Drawing.Point(150, 87); $tbName.Size = New-Object Drawing.Size(320, 24); $tbName.MaxLength = 15
$f.Controls.Add($tbName)

$lblIp = Add-Label "Host's address:" 24 124 120
$tbIp = New-Object Windows.Forms.TextBox; $tbIp.Location = New-Object Drawing.Point(150, 121); $tbIp.Size = New-Object Drawing.Size(220, 24)
$btTest = New-Object Windows.Forms.Button; $btTest.Text = 'Ping'; $btTest.Location = New-Object Drawing.Point(380, 119); $btTest.Size = New-Object Drawing.Size(90, 28)
$f.Controls.AddRange(@($tbIp, $btTest))

$info = New-Object Windows.Forms.TextBox; $info.Multiline = $true; $info.ReadOnly = $true; $info.ScrollBars = 'Vertical'
$info.Location = New-Object Drawing.Point(24, 160); $info.Size = New-Object Drawing.Size(446, 220)
$info.Font = New-Object Drawing.Font('Consolas', 9); $f.Controls.Add($info)

$btFw = New-Object Windows.Forms.Button; $btFw.Text = 'Open firewall (admin)'; $btFw.Location = New-Object Drawing.Point(24, 392); $btFw.Size = New-Object Drawing.Size(215, 32)
$btLog = New-Object Windows.Forms.Button; $btLog.Text = 'Last session summary'; $btLog.Location = New-Object Drawing.Point(255, 392); $btLog.Size = New-Object Drawing.Size(215, 32)
$f.Controls.AddRange(@($btFw, $btLog))

$btStart = New-Object Windows.Forms.Button; $btStart.Text = 'START'; $btStart.Location = New-Object Drawing.Point(24, 436); $btStart.Size = New-Object Drawing.Size(446, 44)
$btStart.Font = New-Object Drawing.Font('Segoe UI', 13, [Drawing.FontStyle]::Bold); $f.Controls.Add($btStart)
$status = Add-Label '' 24 488 460 24

function Show-Info {
    $t = @()
    if (-not (Test-Path (Join-Path $game 'dinput.dll'))) { $t += '!! dinput.dll (the mod) is missing from the game folder.'; $t += '' }
    if (-not (Test-Path $exe)) { $t += '!! Xenus.exe not found - put the launcher in the Boiling Point folder.'; $t += '' }
    if ($rbHost.Checked) {
        $t += 'HOSTING. Give your friend one of these addresses:'
        $ips = @(Get-LocalIPs); if ($ips) { $t += $ips | ForEach-Object { "   $_" } } else { $t += '   (no network address found)' }
        $t += ''
        $t += 'Same house / LAN: use the 192.168.x.x or 10.x.x.x address.'
        $t += 'Over the internet, either:'
        $t += ' - both join the same VPN (Tailscale, ZeroTier, Radmin VPN)'
        $t += '   and use the VPN address shown above (100.x / 25.x / ...), or'
        $t += ' - forward UDP ports 15011 and 15013 on your router to this PC'
        $t += '   and give your public IP (google "what is my ip").'
        $t += ''
        $t += 'Press "Open firewall" once, then START. In the game: Load Game'
        $t += '(or New Game) and wait until you can walk. Then your friend joins.'
    } else {
        $t += "JOINING. Type the host's address above (the host's launcher shows it)."
        $t += 'Press "Ping" to check the PC can be reached (some VPNs/routers'
        $t += 'block ping even when the game works).'
        $t += ''
        $t += 'Wait until the host is IN the game, then press START and click'
        $t += 'New Game. You join the host by yourself and appear in its world.'
    }
    $t += ''
    $t += 'An admin console window opens next to the game: type "help".'
    $t += 'goto = jump to the other player, bring all = pull them to you.'
    $t += 'Both players need the same mod version. Restart both games together.'
    $info.Lines = $t
    $lblIp.Enabled = $rbJoin.Checked; $tbIp.Enabled = $rbJoin.Checked; $btTest.Enabled = $rbJoin.Checked
}

# ---------------- load settings ----------------
if (-not (Test-Path $ini)) { [Windows.Forms.MessageBox]::Show("bp_coop.ini not found in $game", 'Co-op launcher') | Out-Null; return }
$mode = Get-IniValue 'mode' 'host'
if ($mode -eq 'join') { $rbJoin.Checked = $true } else { $rbHost.Checked = $true }
$tbName.Text = Get-IniValue 'name' $env:USERNAME
$ip = Get-IniValue 'host_ip' ''; if ($ip -eq '127.0.0.1') { $ip = '' }; $tbIp.Text = $ip
Show-Info
$rbHost.Add_CheckedChanged({ Show-Info }); $rbJoin.Add_CheckedChanged({ Show-Info })

# ---------------- actions ----------------
$btTest.Add_Click({
    $a = $tbIp.Text.Trim(); if (-not $a) { $status.Text = "Type the host's address first."; return }
    $status.Text = "Pinging $a ..."; $f.Refresh()
    $ok = Test-Connection -ComputerName $a -Count 2 -Quiet -ErrorAction SilentlyContinue
    $status.Text = if ($ok) { "$a answers ping - looks reachable." } else { "$a does not answer ping (may still work if ping is blocked)." }
})

$btFw.Add_Click({
    $cmd = "if (-not (Get-NetFirewallRule -DisplayName '$fwName' -EA SilentlyContinue)) { New-NetFirewallRule -DisplayName '$fwName' -Direction Inbound -Protocol UDP -LocalPort 15011,15012,15013 -Action Allow -Profile Any | Out-Null }; " +
           "if (-not (Get-NetFirewallRule -DisplayName 'Boiling Point co-op (Xenus.exe)' -EA SilentlyContinue)) { New-NetFirewallRule -DisplayName 'Boiling Point co-op (Xenus.exe)' -Direction Inbound -Program '$exe' -Action Allow -Profile Any | Out-Null }"
    try {
        if (Test-Admin) { powershell -NoProfile -Command $cmd } else {
            Start-Process powershell -Verb RunAs -Wait -ArgumentList '-NoProfile', '-Command', $cmd
        }
        $have = Get-NetFirewallRule -DisplayName $fwName -ErrorAction SilentlyContinue
        $status.Text = if ($have) { 'Firewall open for the game (UDP 15011-15013).' } else { 'Firewall rule not added (cancelled?).' }
    } catch { $status.Text = 'Firewall: cancelled.' }
})

$btLog.Add_Click({
    $r = Join-Path $PSScriptRoot 'tools\results.ps1'
    $out = & $r 2>&1 | Out-String
    $tmp = Join-Path $env:TEMP 'bp_coop_summary.txt'; Set-Content $tmp $out; Start-Process notepad $tmp
})

$btStart.Add_Click({
    $name = ($tbName.Text.Trim() -replace '[^A-Za-z0-9_\-]', '')
    if (-not $name) { $status.Text = 'Type a player name (letters and numbers).'; return }
    if ($rbJoin.Checked) {
        $h = $tbIp.Text.Trim()
        if (-not $h) { $status.Text = "Type the host's address."; return }
        if ($h -notmatch '^\d{1,3}(\.\d{1,3}){3}$') {
            try { $h = ([Net.Dns]::GetHostAddresses($h) | Where-Object AddressFamily -eq 'InterNetwork' | Select-Object -First 1).IPAddressToString } catch { $h = $null }
            if (-not $h) { $status.Text = 'That address could not be found.'; return }
        }
        Set-IniValues @{ mode = 'join'; name = $name; host_ip = $h }
    } else {
        Set-IniValues @{ mode = 'host'; name = $name }
    }
    if (Get-Process Xenus -ErrorAction SilentlyContinue) {
        $r = [Windows.Forms.MessageBox]::Show('The game is already running. Close it and start again?', 'Co-op launcher', 'YesNo')
        if ($r -ne 'Yes') { return }
        Get-Process Xenus -ErrorAction SilentlyContinue | Stop-Process -Force; Start-Sleep 2
    }
    $p = Start-Process $exe -WorkingDirectory $game -PassThru
    $status.Text = "Started ($(if ($rbHost.Checked) {'host'} else {'joining ' + $h})). Log: CoopTest\bp_coop_$($p.Id).log"
})

[void]$f.ShowDialog()
