# @@NAME@@

A game on the engine, as a hot-reloadable game module: C++ the engine's viewer loads into its
walking session, ticks 240 times a second, draws a HUD for, rebuilds under while it runs, and
packages for a tester. This repository was made by the engine's `tools/new-game-repo.ps1` from
`templates/game-repo/`; everything in it is the author's to change, this file first.

## Where the engine is

**`engine-sdk.json`** is the one answer to "which engine is this game built against": the path of
the engine SDK (`sdk`, absolute or relative to this directory) and the fields of that SDK's
`sdk.json` it pins — the engine `commit`, whether it was `dirty`, the `preset`, the CPU baseline and
the hotload ABI version. The SDK is a folder the engine's `tools/export-game-sdk.ps1` writes: the
headers a module compiles against, the schema compiler, the build command, `engine-view`, two scenes
and the package tool. It is not committed here; whoever clones this repository gets the SDK of the
pinned commit (an export of it, or the zip the exporter makes) and points `engine-sdk.json` at it.
Moving to a newer engine is an export of the new build and a change to the pinned fields, made
deliberately and by itself.

## Build, run, package

```powershell
./build.ps1                         # src/ -> build/@@NAME@@.dll, with the SDK's compiler and flags
./run.ps1 [-Scene <scene.json>]     # engine-view on the SDK's desert with the module; -Report report.json
./package.ps1 [-Scene <scene.json>] # the zip a tester double-clicks and sends report.json back from
```

The loop: `./run.ps1` opens a window on the desert with the game loaded; edit `src/`, run
`./build.ps1` in another shell, and the running session swaps the new build in on the next frame
with the state carried (a field added to `src/@@NAME@@.schema` starts at its default). W A S D
walk, the mouse looks, E interacts, Escape twice ends. `./run.ps1 -Report report.json` writes the
report engine-view's `--report` writes, which is what a tester sends back; `./run.ps1 -Offscreen
-Frames 60 -Report report.json` replays the SDK's recorded walk with no window, the same ticks every
time, which is how the module is tested. `./run.ps1 -EngineArgs "--game-state save.json"` saves the
state at exit and restores it next time.

**The action map is pinned by a file.** The keys a session reads are the SDK's
`content/input-map.json` (its engine-view's map, written by `engine-view --dump-action-map`, with
its `hash`), which `./run.ps1` passes as `--input-map` and which the SDK's recorded walk was
recorded against. A recording of this game names that hash; a test that needs it reads the file's
`hash`, never a number typed into the test, so moving to a newer SDK moves the map with it and a
recording made against the old one is refused naming both hashes. A game that wants its own
bindings copies the file into this repository, edits it, and passes `-InputMap` to `./run.ps1`.

Requirements: Windows x64, PowerShell 7, CMake 3.28 or later, Visual Studio 2022 or later with the
C++ x64 tools, a Vulkan driver. The SDK's `README.md` has the detail, and says that Linux is not
covered by an exported SDK yet.

## What is where

| Path | What |
|---|---|
| [`src/@@NAME@@.schema`](src/@@NAME@@.schema) | the game's state: one struct of fixed-layout values the engine carries across rebuilds and saves |
| [`src/@@NAME@@.cpp`](src/@@NAME@@.cpp) | the module: `on_load` resolves the tick's fields and the actions it reads, `game_tick` runs the game and says what the HUD shows |
| `build/` | the built module and its intermediate files (ignored) |
| `engine-sdk.json` | the engine pin, above |
| [`LICENSE.md`](LICENSE.md) | the licence is the author's choice; what the engine's own reference game chose, and why, is recorded there |

Every `.cpp` in `src/` is one translation unit and every `.schema` there is compiled by the SDK's
schemac; add files freely. The contract a module is written against — the fields of
`engine.game.GameTick`, the six services, the determinism rules, the HUD's limits — is the engine's
documentation at the pinned commit: `docs/game-authoring.md` (the author's guide) and
`docs/subsystems/apps.md`, "The game contract". The SDK's `include/foundation/hotload/module_abi.h`
is the contract's source.

## Not done here

`git init` and the first commit are the author's; the licence is the author's (`LICENSE.md`); a
scene of the game's own goes in a directory of this repository (`./run.ps1 -Scene scenes/<name>/scene.json`,
and `./package.ps1 -Scene` carries that directory's content), starting from a copy of the SDK's
`content/test-scenes/`.
