# Meadow sward (G88)

Eight source-bounded botanical recipes for the sward solver, separate from G75 tufts.
[Grounding](../../../docs/experiments/grass-sward.md) and
[contract](../../../docs/subsystems/flora.md#grass-sward-g88) state what is implemented and
what still needs acceptance. `camera-path.json` moves through 1, 5, 20 and 80 m.

`tools/sward-cook.ps1 -Out D:/workspace/game_engine_local/sward/2026-10-25/meadow` cooks a
family, the ground layer, density paint, heightfield and scene outside the checkout. No generated
PNGs, meshes, height grids or capture outputs belong here. It also writes recipes and provenance.

The fixture has a field, three existing birches at a shaded clearing edge, a slope, a wet pond margin and a trail. Its
layout is a deliberately small acceptance fixture, not a botanical/ecological simulation. Density
is authored once in the paint and read from W72's resulting palette by `sward` placement.
The script records the shade approximation and its controls. It never writes to the forest POC.
Wetness is present in the ground mask; wetness-driven blade height and colour, and slope-driven
stature, are still missing. The reference 1280/640/96 triangle ladder holds coarse coverage within
3.86% across 32 cooked patches, but sparse clump cards overshoot by up to 20.67%.
The fixture is an acceptance candidate, not an accepted visual baseline.

Use `engine-view --offscreen` for every capture. Current-main shadow measurements must use
`--shadows csm --shadow-distance 8` and be labelled as a stand-in for the pending kind policy.
No 0/3/8 m/s or gust acceptance is claimed from main's dimensionless CPU wind-strength knob.
The measured frame-budget row belongs to the separately briefed engine integration; no invented
baseline is entered here. The owner's captures and pair/shadow budgets remain the exit gate.
