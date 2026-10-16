# walk_poc — the game runner's fixture

**A fixture and a demonstration of the contract, not Desert Survival.** It exists so that
engine-view's game runner ([apps](../../docs/subsystems/apps.md#--game-running-a-game-module),
roadmap P41) has a module to load, tick, hot-swap, save, restore and end, and so that a game's
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
- **Days** are the midnights the world's clock (`world_time_s`) has passed since the run began; the
  score is seconds survived, days and metres walked.
- **`interact`** (E, a pad's West) says a line in the log and counts on the HUD.
- **The end**: thirst at zero calls `end_run({"days","seconds","distance"})`, and the session ends.
- **The HUD**: the thirst meter, "day N · seconds survived · metres walked", where the water is
  (the nearest ruin within 400 m, refreshed once a second, and the spring), and the E count.

## How it is built

`walk_poc.cpp` is one translation unit; `walk_poc.schema` is its state (`engine.walk_poc.Survivor`:
thirst, seconds, metres, days, the run's start, the spring, the counters, `over`), so a rebuild
carries the run and `--game-state` saves it. It resolves the `GameTick` fields it reads and
declares `interact` and `use` in `on_load`, exports `game_tick`, and allocates nothing there: the
HUD's text is formatted into a stack buffer the host copies. Two test builds of the same source
(`WALK_POC_TEST=1`, an action no map has; `2`, a failure at tick 30) are what engine-view's tests
check the refusals with.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/ruins-walk/scene.json --walk --start 16,20,40 0,0 --game build/msvc-release/bin/game/walk_poc.dll --game-state walk_poc.state.json
```
