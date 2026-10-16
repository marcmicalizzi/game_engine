# game/ — the L5 layer: game modules

**A game module** is gameplay code compiled into a shared library (`<name>.dll`, `<name>.so`) that
a running host loads, calls, watches and replaces while it runs, carrying its state across the swap
([hotload](../docs/subsystems/hotload.md)). It links **no engine library**: its include path is the
engine's headers (`core/*/include` and `foundation/hotload`'s `module_abi.h`), and everything it
needs from the engine — the heap, the log, the clock, reads of host objects, and a game runner's
services — comes through the `HostApi` table it is handed. Its state is one struct in a `.schema`
file beside it, of fixed-layout values only, which the host migrates through the old and the new
build's descriptors when the module is rebuilt.

| Module | What |
|---|---|
| [`sandbox_npc/`](sandbox_npc/) | E7's C++ arm: the Luau fixtures as C++ and a 200-line body for the rebuild measurement ([E7](../docs/experiments/e7-luau-spike.md#the-c-hot-reload-arm)) |
| [`walk_poc/`](walk_poc/README.md) | the fixture of engine-view's game runner: thirst, water at the ruins, a day counter, a HUD and `end_run` ([apps](../docs/subsystems/apps.md#--game-running-a-game-module)) |

## Building and running one

```cmake
engine_game_module(NAME walk_poc SOURCES walk_poc.cpp SCHEMAS walk_poc.schema)
```

builds `build/<preset>/bin/game/walk_poc.dll` with the engine's compiler and flags (unity, hidden
visibility, warnings as errors, no exceptions, the CPU baseline). Run it from the walking session:

```powershell
tools/dev.ps1 build -Preset msvc-release
build/msvc-release/bin/engine-view --scene content/test-scenes/ruins-walk/scene.json --walk --start 16,20,40 0,0 --game build/msvc-release/bin/game/walk_poc.dll --report report.json
```

Rebuild the module while it runs (`cmake --build build/msvc-release --target engine_game_walk_poc`)
and the session swaps the new build in on the next frame with the state carried; a field added to
the state starts at its default. A module built outside the tree uses the exported build command
([hotload](../docs/subsystems/hotload.md#building-a-module)):

```
cmake -DCONFIG=build/<preset>/game_modules/build_config.cmake -DSOURCE_DIR=<dir> -DOUTPUT=<file> -P cmake/EngineGameModuleBuild.cmake
```

`tools/package-game.ps1 -Preset msvc-release -Module walk_poc -Scene content/test-scenes/desert-endless/scene.json`
makes the zip a tester double-clicks and sends one `report.json` back from
([apps](../docs/subsystems/apps.md#package-game-a-zip-somebody-else-double-clicks)).

## Where a real game lives

**Not here.** A game the project ships — Desert Survival first ([ADR-0022](../docs/adr/0022-consumer-game-order.md)) —
lives in its own repository, licensed apart from the engine and pinning an engine commit
([ADR-0047](../docs/adr/0047-reference-games-are-licensed-apart-from-the-engine.md), roadmap P18).
What it builds against is the contract: the module ABI (`foundation/hotload/include/foundation/hotload/module_abi.h`,
ABI 2), the tick it reads (`engine.game.GameTick`, `apps/engine_view/game_tick.schema`), the game
actions of engine-view's map (`content/input-maps/engine-view.json`, revision 5), and the build
command above. Everything in this directory is a fixture: small, owned by the engine's tests, and
changed with them.
