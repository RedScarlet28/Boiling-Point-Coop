# Boiling Point: Road to Hell — Co-op mod

> **⚠ Early alpha.** This is a work-in-progress experiment. Expect desyncs, crashes and broken saves.
> Back up your `Save` folder before playing. Tested mostly with two copies of the game on one PC.

Boiling Point: Road to Hell (2005, Deep Shadows) ships with dormant client/server network code from
its engine (the same family as Xenus / Bet On Soldier), but no usable multiplayer. This mod wakes that
code up and patches the many places where the single-player game assumes it is alone, so two or more
people can play **the campaign together**: one player hosts their world, the others join it.

The mod is a single proxy `dinput.dll` (written in C) placed in the game folder, configured by
`bp_coop.ini`. It does not change any game files.

## What works (so far)

| Area | State |
|---|---|
| Joining a host, seeing each other, movement | ✅ synced, low latency |
| NPCs, animals, bodies of the host's world on the guest | ✅ most of them (guest's own copies are removed) — some still unsynced |
| Enemy NPCs target the guest too | ✅ |
| Guest shooting / killing the host's NPCs | ✅ (some hits do not register) |
| Doors and buttons | ✅ |
| Time of day and weather | ✅ host → guest |
| Weapons (equip, gun visible in hands) | ✅ |
| Death and respawn (next to the other player) | ✅ fresh inventory after respawn |
| Saving in co-op (host) | 🧪 new — the save is a clean single-player world |
| Guest character profile (money, experience, inventory) | 🧪 new — saved to `coop_profile.ini` |
| Quests / missions host → guest (PDA, target marker) | 🧪 new |
| Faction reputation host → guest (+ HUD arrows) | 🧪 new |
| Taxi for the guest | 🧪 new |
| Admin console (teleport, bring, goto, money, time, scripts) | 🧪 new |
| Guest using the host's cars | ❌ guest walks through them, cannot drive them |
| Guest-only quest progress (dialogs, item pickups) reaching the host | ❌ |

## Installing

1. Use the English retail / GOG / Steam version of Boiling Point (`Xenus.exe`, `game.dll`, `vital.dll`).
2. Copy **`dinput.dll`** (from the [Releases](../../releases) page, or build it yourself, see below),
   **`bp_coop.ini`** and the **`CoopTest`** folder into the game folder (next to `Xenus.exe`).
3. Recommended in the game's `GAME.INI` when running two copies on one PC:
   `Windowed mode=1`, `ModeWidth=1280`, `ModeHeight=720`, `WRITECONSOLELOG=1`.
4. Playing over a network: press **Open firewall** in the launcher once on each PC (or run
   `CoopTest\tools\firewall.ps1` as administrator). It opens UDP 15011-15013.

To uninstall, delete `dinput.dll` from the game folder.

## Playing on two PCs — the launcher

Double-click **`Coop Launcher.cmd`** in the game folder (both PCs need the mod installed, same version).

1. **Host:** choose *Host a game*, type a name, press **START**. The launcher shows this PC's
   addresses — give one to your friend. In the game, Load Game (or New Game) and wait until you can walk.
2. **Guest:** choose *Join a friend*, type a name and the host's address (**Ping** checks it can be
   reached), then press **START** and click New Game. You join the host's world by yourself.

Which address to use:

* **Same house / LAN:** the host's `192.168.x.x` or `10.x.x.x` address.
* **Over the internet:** easiest is a VPN both players join (Tailscale, ZeroTier, Radmin VPN) — use the
  host's VPN address. Otherwise the host forwards **UDP 15011 and 15013** on the router to the host PC
  and gives the guest its public IP.

**Last session summary** shows what the logs recorded — useful for bug reports.

## Playing on one PC (testing)

```powershell
powershell -ExecutionPolicy Bypass -File CoopTest\start_both.ps1
```

1. **Host window:** Load Game (or New Game) and wait until you can walk around.
2. **Guest window:** New Game (or Load Game) — it joins the host by itself.

Without the launcher: the host runs `CoopTest\start_host.ps1`, the guest runs
`CoopTest\start_client.ps1 -hostIp <host address>`.

Always restart all copies together. After a session, `CoopTest\tools\results.ps1` summarizes the logs
(`CoopTest\bp_coop_<pid>.log`).

### Admin console

With `admin_console=1` a console window opens next to each game. Type `help`:

```
players | pos | tp X Y Z | goto [PID] | bring PID|all | mark | back | up [units]
rank | rank SIDE VALUE | money AMOUNT | exp INDEX VALUE | time +HOURS (host)
quests | cmd TEXT (raw game console command, e.g. "cmd GET TIME")
profile save | profile load | exec FILE (one command per line, "wait MS" pauses)
```

`coop_autoexec.txt` in the game folder runs automatically once the world is loaded
(see `coop_autoexec.example.txt`). **F8** on the host brings every guest to the host.

## Settings

Every feature can be switched off in `bp_coop.ini`; each key has a comment explaining it.
`start_host.ps1` / `start_client.ps1` set `mode`, `name` and `host_ip` for you.

## Building

Needs Visual Studio (MSVC x86 toolchain) and the Windows 10/11 SDK.

```powershell
powershell -ExecutionPolicy Bypass -File CoopTest\build.ps1
```

The result is `CoopTest\bin\dinput.dll`; copy it into the game folder.

## How it works (short)

* The host runs the game as a server (`stMode 3`) with campaign rules; guests connect with the
  engine's own `-CONNECT` client (`stMode 4`) and give their own objects local entity numbers.
* Guests remove their local copies of NPCs/animals that the host also sends, so everyone sees the
  host's world. Player movement uses the engine's own netcode.
* A small UDP side channel (port 15013) carries what the engine never synced: clock, weather,
  reputation, quest state, teleports, death reports.
* Many single-player routines (doors, vehicles, taxi, bullet hits, saving) are hooked so they run on
  the right machine.

Notes on addresses and structures are in comments throughout `CoopTest/src/bp_coop.c`.

## Status / disclaimer

Early alpha, made for fun and preservation. Not affiliated with Deep Shadows or the publishers.
You need your own copy of the game; no game files are included.

The mod's code is released under the [MIT License](LICENSE).
