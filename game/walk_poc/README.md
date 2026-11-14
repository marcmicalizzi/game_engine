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
- **The creator** (hotload ABI 9, roadmap P52), in its own build, **`walk_poc_creator`** (`--game build/<preset>/bin/game/walk_poc_creator.dll`; `walk_poc` itself declares its two actions and draws its HUD as it always did): **1** (`game_1`) opens and closes it. In it, **2**
  and **3** (`game_2`, `game_3`) select the previous and next parameter, and **Q** and **4** (`use`,
  `game_4`) take a step down and up — on the press, and while the key is held again ten times a
  second of the tick clock, so a replay steps the same (Q places no marker while it is open). The
  parameters: **height**, the actor's scale (`actor_scale`), 0.85 to 1.15 in hundredths; then one
  per morph channel of the actor (`actor_morph`, the static weights), 0 to 1 in tenths, named as the
  asset names it (`actor_morph_info`; glTF `extras.targetNames`) and `shape n` where it names none,
  at most 16. The HUD lists them down the top-right as elements, the selected one in amber, with its
  value as a meter in the top-left stack; closing removes them. It shapes the actor **`player`**
  when the scene names one, else **`bar`** (the tests' actor), looked up at a tick — not in
  `on_load`, where a scene with other actors would refuse the load — and does nothing where there is
  neither. **The shape is the game's state, not the engine's**: the appearance (`appearance_set`,
  `appearance_scale`, `appearance_count`, `appearance_weights`, `@since(6)`) is saved by
  `--game-state` and said to the actor again on the first tick after any load — a cold restore, a
  replay's restored header, a rebuild — because the host keeps no actor's shape.
- **The graphics** (ABI 4): on its first tick it logs the raster path and shadows the session
  resolved (`graphics_state`), and makes one request the host records (`shadows: csm`) and one it
  refuses (`raster: warp`), so a test sees both answers.

## How it is built

`walk_poc.cpp` is one translation unit; `walk_poc.schema` is its state (`engine.walk_poc.Survivor`:
thirst, seconds, metres, days, the run's start, the spring, the counters, `over`, and since version 2
the can's id and whether it was placed and taken, and the markers placed), so a rebuild carries the
run and `--game-state` saves it. It resolves the `GameTick` fields it reads, declares `interact` and
`use` and asks for the two props in `on_load`, exports `game_tick`, and allocates nothing there: the
HUD's text is formatted into a stack buffer the host copies. Six test builds of the same source
(`WALK_POC_TEST=1`, an action no map has; `2`, a failure at tick 30; `3`, no props and no walker
calls, so a scene with and without the game draws the same picture; `4`, named `walk_poc_teleport`,
a teleport 10 m ahead at tick 50; `5`, named `walk_poc_actor`, which finds the scene's actor `bar`
(hotload ABI 5) and walks it round a 3 m circle on the ground it asks for with `ground_height`,
playing `bend`, at twice the rate from tick 120, fading to rest from 240; `6`, named
`walk_poc_loading`, which exports ABI 7's `game_loading` — its own loading line, and the calls,
the last fraction and any fall counted in its state (`@since(4)`) — and `game_ready_milestone`,
asking for `ground_collision_ready`; `7`, named `walk_poc_aligned` (hotload ABI 8), which stands
`bar` at one place through eight one-second phases — `actor_place_aligned` with a level `up`, a 20
degree slope, a 60 degree wall held to the default 30 and then let through at 60, four refused
calls, the ground's own normal from `ground_sample`, a level `actor_place`, and two refused
footprints — with every call's answer in its state (`@since(5)`); `8`, named `walk_poc_abi7`, the
actor build declared as a module built against ABI 7 is, reading `actor_state` into ABI 5's
136-byte struct with a guard behind it; and `9`, `walk_poc_level`, build 7 making every placement
through `actor_place`, the picture build 7's level phases must reproduce; and `10`, named
`walk_poc_shape` (hotload ABI 9), the creator build with the actor's clip stopped at its first tick, five
refused shape calls' answers in its state (`shape_codes`: too many weights, a NaN weight, a scale of
5, of 0.1 and NaN), what `actor_state` reports of its scale and channels each tick, and none of the
run's HUD lines, so a capture after a restore is the picture of the shape alone) are what
engine-view's tests check the refusals, the tail, the actors, the loading hooks, the creator and the
replay rules with. Beside them, a module of its own source, **`walk_poc_world`**
(`world_probe.cpp`, hotload ABI 10, roadmap A43), stateless: once the ground under the feet has
collision it asks the world — the supports near a tree's crown, a post's declared perch and a
boulder's top, landing a prop `bird` on each; a ray at the trunk, sixteen down through the crown, a
sphere at the boulder and a ray into the pond that stops at the water; the water at five points of
the pond — and ends the run with every answer, which `world_queries_view_tests.cpp` holds.

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
