# PatrolRepair (ASA)

An [AsaApi](https://github.com/ArkServerApi/AsaApi) plugin for ARK: Survival Ascended servers running Lost
Colony. It reports the state of the city patrols per patrol manager and, after each boot, removes what a
save load leaves broken, so the managers send out fresh mounted patrols.

## How it works

1. `PatrolRepair.Report` prints the last finished scan and starts a new one.
2. A scan looks at `ScanActorsPerTick` level actors per second, so a busy map takes a few seconds and no
   single tick does all the work.
3. Every living, unowned patrol creature is counted: **riders** (classes deriving from `RiderFamily`, the
   Thralls) and **mounts** (classes whose path contains `MountPathContains` or whose name ends with
   `MountClassEndsWith`). Each is grouped by the zone manager it is linked to. Garrison creatures of
   outposts (their `OwnerMission` is set) are counted on a separate line, and creatures without a manager
   link on another.
4. A rider counts as mounted when any of four links says so: its `MountedDino`, the actor it is attached
   to, a mount's passenger seats, or a mount's carried character. The line after each report says how many
   pairs each link found, and names any non-patrol classes riders are attached to.
5. Per manager the report shows riders (mounted / unmounted), unmounted riders that moved less than
   `StillDistance` since the previous scan, and mounts (ridden or not), next to the manager's own
   bookkeeping: `[linked count N, links M, reset version S/L]`. `linked count` is how many creatures the
   manager believes it has out, `links` how many it still holds a link to, and the reset version is the one
   stored in the save against the one this game build expects (the game resets patrols when they differ).
6. `PatrolRepair.Clean` plans from the last scan: everything that is not a mounted pair.
   - a manager's riders on foot and mounts nobody rides (`RemoveUnpaired`). A save load takes every rider
     off its mount; once a player is near only about a quarter get back on. The rest fill the manager's
     roster, so it never sends out new patrols.
   - riders and unridden mounts with no manager link (`RemoveUnlinked`). Every load leaves about 200 of
     these; nothing reclaims them.

   The managers refill with fresh mounted patrols within minutes, the same way they do after the game's
   own patrol reset. Mounted pairs and outpost garrisons are never removed.
7. `PatrolRepair.Clean confirm` queues the plan and removes `RemovalsPerTick` creatures per second. Each one
   is checked again just before: one that died, changed manager, got onto a mount or got a rider since the
   scan is left alone. `CleanAfterBootMinutes` runs one scan and clean that long after load;
   `AutoCleanMinutes` runs them on an interval.

Measured on a fresh save: before a restart, 388 mounted pairs and almost every mount ridden; after one
restart, with a player in the city, 95 pairs, about 280 empty mounts, about 1,200 riders on foot mostly
standing still, and 212 more riders without a manager link. The managers' `linked count` and `links` are 0
on a fresh save too, so the large negative counts on old saves do not matter. The game's own
`HandleResetPatrols`, called on a manager of an old save, changed nothing, so the plugin does not use it.

## Build

Requirements: Visual Studio 2026 with the "Desktop development with C++" workload (v145 toolset), and Git.

AsaApi is a git submodule at `extern\AsaApi`. After cloning the repository:

```
git submodule update --init --recursive
```

1. Build `extern\AsaApi\AsaApi.sln` (**Release / x64**) so `AsaApi.lib` lands in `extern\AsaApi\out_lib\`,
   or copy `AsaApi.lib` there from the `Lib` folder of the AsaApi release zip your servers run.
2. Open `PatrolRepair.sln`, select **Release / x64**, build.

`fmt` comes from vcpkg through the manifest in `vcpkg.json` (Visual Studio's bundled vcpkg; triplet
`x64-windows-static-md`).

The result is `out\PatrolRepair\`, containing `PatrolRepair.dll`, its `.pdb`, `PluginInfo.json`, `config.json`,
`config.commented.json` and `THIRD-PARTY-NOTICES.md`.

## Install

1. Copy the `out\PatrolRepair` folder to `ShooterGame\Binaries\Win64\ArkApi\Plugins\`. The folder and the DLL
   must keep the same name.
2. Start the server. It is only useful on Lost Colony; on other maps the report just finds no patrols.

## Using it

1. Run `PatrolRepair.Report` once to start the first scan, then again a few seconds later to see it.
2. Run it again later: from the second scan on, the report can tell which unmounted riders have stopped
   moving.
3. Compare a fresh save with one that has run for days. The full report, every manager included, is in the
   plugin log after each scan.
4. `PatrolRepair.Clean` to see what would go, `PatrolRepair.Clean confirm` to remove it,
   `PatrolRepair.Clean cancel` to stop a clean that is running. Try it on a copy of a save first.

## config.json

Comments are allowed. `configs\config.commented.json` has the same values with every setting explained.

| Setting | Default | Meaning |
|---|---|---|
| `General.ScanActorsPerTick` | 5000 | Level actors a scan looks at per second. |
| `General.StillDistance` | 300 | Unreal units (300 = one foundation). An unmounted rider that moved less than this between two scans counts as not moving. |
| `General.ReportManagers` | 10 | Managers shown in chat, most unmounted riders first. The log gets all of them. |
| `Patrols.RiderFamily` | `Thrall_Character_BP_Base_C` | Parent class of every patrol rider. |
| `Patrols.MountPathContains` | `/Thrall/Characters/RideableCreatures/` | Path part of patrol mount classes. |
| `Patrols.MountClassEndsWith` | `_Thrall_C` | Class name ending of the other patrol mounts (`Raptor_Character_BP_Thrall_C` and the like). |
| `Clean.RemovalsPerTick` | 10 | Creatures removed per second. |
| `Clean.RemoveUnlinked` | true | Remove riders and unridden mounts no patrol manager is linked to. |
| `Clean.RemoveUnpaired` | true | Remove patrol managers' riders on foot and mounts nobody rides. |
| `Clean.AutoCleanMinutes` | 0 | 0 = off; otherwise a scan and clean run on this interval, the first one interval after load. |
| `Clean.CleanAfterBootMinutes` | 10 | 0 = off; otherwise one scan and clean this many minutes after load. |
| `Clean.ResetNegativeLinkedCount` | true | After every scan, sets a patrol manager's linked count (`DirectLinkDinoCount`) back to 0 when it is below 0. A manager with a negative count sends out no patrols; this is what left the city empty. Counts of 0 or above are never touched. Each reset is logged. |

## Commands

Admin console commands (`cheat PatrolRepair.Report`); all of them also work over RCON.

| Command | What it does |
|---|---|
| `PatrolRepair.Report` | Shows the last finished scan and starts a new one. |
| `PatrolRepair.Clean [confirm\|cancel]` | Shows what a clean of the last scan would remove; `confirm` removes it (the scan must be at most 10 minutes old), `cancel` stops a running clean. |
| `PatrolRepair.ForceReset [confirm]` | Experimental. Shows how many patrol managers the last scan found; `confirm` sets each one's saved patrol reset version (`SaveForceResetPatrolsVersion`) to 0 so the game may reset their patrols on the next load. Takes effect only after `saveworld` and a restart. `count confirm` instead sets each manager's linked count (`DirectLinkDinoCount`) to 0 right away. Test on a copy of a save first. |
| `PatrolRepair.Reload` | Re-reads `config.json`. A broken file keeps the old settings and reports the error. |

## Tests

`tests\logic_tests.cpp` covers config parsing, the report and the clean plan. It needs no game and no Windows. From a Visual
Studio developer prompt in this folder:

```
cl /std:c++20 /EHsc /I extern\AsaApi\AsaApi\Core\Public /I src tests\logic_tests.cpp
logic_tests.exe configs\config.json --shipped
logic_tests.exe configs\config.commented.json --shipped
```

## Files

| File | Purpose |
|---|---|
| `src\Main.cpp` | Scan, manager bookkeeping, clean queue, commands. |
| `src\PatrolLogic.h` | Config parsing, report building and formatting, clean plan. No game dependencies. |
| `configs\config.json` | Default configuration. |
| `configs\config.commented.json` | The same configuration with every setting explained. Not read by the plugin. |
| `configs\PluginInfo.json` | Plugin metadata read by AsaApi. |
| `tests\logic_tests.cpp` | Stand-alone tests for `PatrolLogic.h` and the shipped config. |
| `vcpkg.json` | vcpkg manifest (fmt). |
| `extern\AsaApi` | AsaApi submodule the plugin builds against. |
