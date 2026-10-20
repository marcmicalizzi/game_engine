# walk_poc — the game runner's fixture

**A fixture and a demonstration of the contract, not Desert Survival.** It exists so that
engine-view's game runner ([apps](../../docs/subsystems/apps.md#--game-running-a-game-module),
roadmap P41, P43) has a module to load, tick, hot-swap, save, restore and end, and so that a game's
author has a complete small example of every part of the contract. Desert Survival is written in
its own repository against the same contract ([ADR-0047](../../docs/adr/0047-reference-games-are-licensed-apart-from-the-engine.md),
roadmap P18); nothing here is its design, and its survival meters are its own (roadmap P1).

## The rules

- **Thirst** is a meter from 100. It drains at **0.25 a second of walking** (400 s from full) and
  **0.75 at a sprint**, and refills at **10 a second** while the walker stands within **6 m of
  water**.
- **Water** is the nearest `ruins` placement (`nearest_placement("ruins", 6 m)`: the proof of
  concept's "water at the ruins"), or, where none is that near, **a spring 25 m ahead** of where the
  walker stood on the run's first tick — so the fixture works on any scene, streamed or not.
- **The water can** (a prop, on a scene that declares the props `can` and `marker`): placed at the
  spring on the first tick, at the height of the walker's feet. Within **2 m** of it the HUD says so,
  and `interact` picks it up — `prop_remove`, and the meter is full. On a scene with no props
  (`prop_mesh` answers `Unsupported`) nothing is placed and the fixture runs as it always did; a
  scene with props and not these two refuses the load, naming the prop.
- **A marker**: `use` (Q) places the `marker` prop where the walker stands, turned the way it looks.
- **Slowing down**: below half thirst the walker slows — `walker_set` with a scale from 1 at 50
  down to 0.5 at 0 — told only when the scale moves, so a fresh walker's speed is the recording's
  own to the bit until thirst falls that far.
- **Days** are the midnights the world's clock (`world_time_s`) has passed since the run began; the
  score is seconds survived, days and metres walked.
- **`interact`** (E, a pad's West) with nothing in reach says a line in the log and counts on the HUD.
- **The end**: thirst at zero calls `end_run({"days","seconds","distance"})`, and the session ends.
- **The HUD**: the thirst meter, "day N · seconds survived · metres walked", where the water is
  (the can when in reach; else the nearest ruin within 400 m, refreshed once a second, and the
  spring), the E count, and the can and markers taken and placed. And, once the can stands, a
  structured element (hotload ABI 4, `hud_element`): `[E] Pick up`, outlined text with no panel
  centred below the picture's middle, shown while the can is in reach and hidden otherwise.
- **The graphics** (ABI 4): on its first tick it logs the raster path and shadows the session
  resolved (`graphics_state`), and makes one request the host records (`shadows: csm`) and one it
  refuses (`raster: warp`), so a test sees both answers.

## How it is built

`walk_poc.cpp` is one translation unit; `walk_poc.schema` is its state (`engine.walk_poc.Survivor`:
thirst, seconds, metres, days, the run's start, the spring, the counters, `over`, and since version 2
the can's id and whether it was placed and taken, and the markers placed), so a rebuild carries the
run and `--game-state` saves it. It resolves the `GameTick` fields it reads, declares `interact` and
`use` and asks for the two props in `on_load`, exports `game_tick`, and allocates nothing there: the
HUD's text is formatted into a stack buffer the host copies. Five test builds of the same source
(`WALK_POC_TEST=1`, an action no map has; `2`, a failure at tick 30; `3`, no props and no walker
calls, so a scene with and without the game draws the same picture; `4`, named `walk_poc_teleport`,
a teleport 10 m ahead at tick 50; `5`, named `walk_poc_actor`, which finds the scene's actor `bar`
(hotload ABI 5) and walks it round a 3 m circle on the ground it asks for with `ground_height`,
playing `bend`, at twice the rate from tick 120, fading to rest from 240) are what engine-view's
tests check the refusals, the tail, the actors and the replay rules with.

**Its own art** (hotload ABI 6): with `hud/panel.png` beside the scene file (or the module), every
build draws that image nine-sliced as a panel behind the `[E] Pick up` prompt, and with
`hud/font.ttf`, a `FIND THE WATER` title in that font at the top of the picture, letter-spaced with a
drop shadow; without the files the HUD is the one it always drew. engine-view's HDR HUD case writes
both into its scratch directory and captures them in `sdr8`, `hdr10` and `scrgb`.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/ruins-walk/scene.json --walk --start 16,20,40 0,0 --game build/msvc-release/bin/game/walk_poc.dll --game-state walk_poc.state.json
```

A scene that wants the can declares it, beside a mesh of its own:

```json
"props": [{"name": "can", "mesh": "can.glb", "fit": {"height": 0.4}},
          {"name": "marker", "mesh": "stake.glb"}]
```
