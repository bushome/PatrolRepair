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
Server plugin for ARK: Survival Ascended's Lost Colony city patrols. Three things break them, and none of them
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

PatrolRepair reports every patrol manager, including ones with no creatures. After every scan it sets any
negative count back to 0, and its clean removes everything that is not a mounted pair, so the managers send out
fresh mounted patrols. By default a scan and clean run ten minutes after each boot; `AutoCleanMinutes` adds them
on an interval.

`PatrolRepair.Report` generates a report of the status of the patrol managers and attached NPC counts. 
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

## Development

Developed with AI assistance (Claude). Every change was reviewed and tested on a dedicated test server
before going live.
