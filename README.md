```
 ____            _                    _
|  _ \    __ _  | |_   _ __    ___   | |
| |_) |  / _` | | __| | '__|  / _ \  | |
|  __/  | (_| | | |_  | |    | (_) | | |                    |>
|_|      \__,_|  \__| |_|     \___/  |_|                o   |
 ____                           _                      /|\__|
|  _ \    ___   _ __     __ _  (_)  _ __          __   / |  |
| |_) |  / _ \ | '_ \   / _` | | | | '__|        (o \__/_\_____
|  _ <  |  __/ | |_) | | (_| | | | | |            `-.________  `--.
|_| \_\  \___| | .__/   \__,_| |_| |_|              /\     /\
               |_|
```
Server plugin for ARK: Survival Ascended's Lost Colony city patrols. Four things break them, and none of them
fix themselves:

- **Managers stop spawning.** Each patrol manager keeps a count of the creatures it has out
  (`DirectLinkDinoCount`). Over time it drifts below zero, and a manager with a negative count sends out no
  patrols at all. On one live save all 30 city managers were stuck at -956 to -9091 and the city was empty;
  setting the counts to 0 brought back over 800 riders within minutes.
- **Riders come off their mounts.** Every save load takes every rider off its mount, and only some get back on.
  The rest stand next to empty mounts and fill the manager's roster. On a fresh save one restart took the city
  from 388 mounted pairs to 95.
- **Riders lose their manager.** Every load cuts some riders off from their manager for good, and nothing
  reclaims them.
- **Managers fill up with empty groups (game 98.6).** While no player is online, Lost Colony's patrol spawn
  returns without spawning anything, but the manager still counts the group and saves it. A save made then
  (a shutdown save, say) holds a full set of empty groups. On the next load every manager starts at its group
  cap, no creature ever links back to those groups, and nothing in the game dissolves them, so the manager
  never sends out a patrol again. A fresh save worked only until its first restart.

PatrolRepair reports every patrol manager, including ones with no creatures. After every scan it sets any
negative count back to 0 and dissolves saved groups that have nothing left of them (`Spawning.DissolveOrphanGroups`,
through the manager's own `OnGroupDissolved`), and its clean removes everything that is not a mounted pair, so the
managers send out fresh mounted patrols. By default a report-only scan runs three minutes after each boot
(`Spawning.ScanAfterBootMinutes`), which frees the empty groups, and a scan and clean run ten minutes after boot;
`AutoCleanMinutes` adds them on an interval.

`PatrolRepair.Report` summarizes the last patrol scan and starts a new one. `PatrolRepair.Report verbose` shows manager details and logs the full scan.
(`Clean.ResetNegativeLinkedCount`, on by default). `PatrolRepair.Clean`
shows what a clean would remove; `PatrolRepair.Clean confirm` removes it a few per second.
`PatrolRepair.ForceReset count confirm` sets every manager's count to 0 by hand.

| Folder | Game | Server API (submodule) |
|---|---|---|
| [`ASA`](ASA/README.md) | ARK: Survival Ascended | [AsaApi](https://github.com/ArkServerApi/AsaApi) at `ASA/extern/AsaApi` |

There is no ASE version: Lost Colony and its patrols only exist on ASA.

Clone with submodules:

```
git clone --recurse-submodules https://github.com/bushome/PatrolRepair.git
```

or, in an existing clone:

```
git submodule update --init --recursive
```

## For Wildcard: Lost Colony patrol managers stop spawning after a restart (game 98.6)

Symptom: on a Lost Colony save, the city has no patrols after a server restart. Every `NPCZoneManager_Patrols_C`
reports its small/medium/large group counts at their caps, with no group objects and no creatures. A fresh save
spawns patrols until its first restart. Patrols worked on 97.18.

**Observed on 98.6 dedicated servers (log output from PatrolRepair's trace of `ANPCZoneManager::SpawnPatrolGroup`):**

- `SpawnPatrolGroup` returns before spawning anything unless its container is set, `bUsesManualSpawning` is true,
  the group id is above 0 and, when `GIsLostColonyMap` is set, the world settings' player controller actor list
  is not empty (read from the 98.6 server binary).
- With nobody connected, all 30 managers called it with `player controllers 0`: 0 creatures spawned, none of
  `IsValidNPCEntry`, `SpawnFromEntries`, `SpawnNPC` or `InjectLatentSpawn` reached, and `SpawningPatrolGroupID`
  left at its call value instead of being reset to -1.
- After a player joined, calls from the same managers on the same save reached `SpawnNPC`, spawned riders, and
  reset the id to -1.
- On the boot after a save made like that, every manager had its counts at the cap (for example small 7/7,
  medium 3/3, large 2/2), `SavedGroups` entries with 0 members and 0 latent spawns, no creature carrying their
  ids, and no group object, and none of them tried to spawn again for the whole session.

**From the Blueprint, read in the 97.16 DevKit (it has not been updated to 98.x, so this is how 97.16 works;
the 98.6 numbers above are consistent with it):** `DoSpawnPatrolGroup` increments the size's group count and adds
a `SavedGroups` entry (the only SaveGame variable on the manager) before it calls the native function.
`BeginPlay` rebuilds `NumSmallGroups`, `NumMediumGroups` and `NumLargeGroups` from `SavedGroups`. A group is only
counted down by `OnGroupDissolved`, which needs a group object, and group objects are only recreated from
members that load back in. A group that was counted, saved and never had members therefore holds its slot
permanently, and `ReceiveTick` stops calling `DoSpawnPatrolGroup` for that size at the cap.

**Sequence (two boots on the Lost Colony test server, one save):** after a boot where nobody connected (every
call `player controllers 0`), then a restart, all 30 managers came up at their caps with saved groups that had
no members and no group objects. After a boot where a player joined 11 minutes in and stayed 13 minutes, then
a restart, only 11 such groups were left across all managers. So while no player is connected the Blueprint
keeps creating groups that the native function refuses to populate, a save made then stores them, and after
the restart they fill every cap. Not a controlled test: the group counts were not read just before each
shutdown save, and PatrolRepair's `DissolveOrphanGroups` was not yet in the first build.

**Possible fixes on the game side:** only count and save a group when the native call spawned something, or skip
`DoSpawnPatrolGroup` while the player list is empty, or drop saved groups with no members and no latent spawns
when the counts are rebuilt at `BeginPlay`.

**Workaround:** PatrolRepair's `Spawning.DissolveOrphanGroups` (see above) calls the manager's own
`OnGroupDissolved` for those groups; with it, patrols came back on a loaded 98.6 save and across a restart.

## Development

Developed with AI assistance (Claude). Every change was reviewed and tested on a dedicated test server
before going live.
