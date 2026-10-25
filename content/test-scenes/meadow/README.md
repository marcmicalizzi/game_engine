# Meadow sward (G88)

Eight source-bounded botanical recipes for the sward solver, separate from G75 tufts.
[Grounding](../../../docs/experiments/grass-sward.md) and
[contract](../../../docs/subsystems/flora.md#grass-sward-g88) state what is implemented and
what still needs acceptance. `camera-path.json` moves through 1, 5, 20 and 80 m.

`tools/sward-cook.ps1 -Out D:/workspace/game_engine_local/sward/2026-10-25/meadow` cooks a
dense tall-fescue family by default, the ground layer, density paint, heightfield and scene outside the checkout. `-Recipe <file>` selects another management state; `-Species` optionally overrides its species. No generated
PNGs, meshes, height grids or capture outputs belong here. It also writes recipes and provenance.

The fixture has a field, three existing birches at a shaded clearing edge, a slope, a wet pond margin and a trail. Its
layout is a deliberately small acceptance fixture, not a botanical/ecological simulation. Density
is authored once in the paint and read from W72's resulting palette by `sward` placement.
The script records the shade approximation and its controls. It never writes to the forest POC.
Wetness is present in the ground mask; wetness-driven blade height and colour, and slope-driven
stature, are still missing. The original v4 family had up to 20.67% card error. V5 changes shoot structure, allows leaf overhang to avoid cell seams and fits its coarse width/card cutoff at cook time.
The fixture is an acceptance candidate, not an accepted visual baseline.

Use `engine-view --offscreen` for every capture. Current-main shadow measurements must use
`--shadows csm --shadow-distance 8` and be labelled as a stand-in for the pending kind policy.
No 0/3/8 m/s or gust acceptance is claimed from main's dimensionless CPU wind-strength knob.
The measured frame-budget row belongs to the separately briefed engine integration; no invented
baseline is entered here. The owner's captures and pair/shadow budgets remain the exit gate.

## Dense cover and managed grass candidates

The v5 continuation adds `dense`, `dense-low`, `mown`, `grazed`, `trodden`, `clumps` and `tall`
recipes. Dense/tall use grouped tall-fescue leaves, not uniformly scattered isolated blades.
Mown leaves are clipped at 5 cm; grazing varies the cut height. Trodden is an authored short,
sparse state, not a simulation of a footstep. Clumps gather shoots into distinct crowns.
Sample counts, crown radii and cutting heights are authored examples, not species constants.
The high candidate has an explicit 8500 all-rung triangle cap; dense-low stays within 2400.
Neither is a claim about runtime performance or accepted visual quality.

```powershell
tools/sward-showcase.ps1 -Out D:/workspace/game_engine_local/sward/showcase -Capture
```

The showcase cooks seven 8x8 m stands, near/card scenes and matching close, walking-height and
overview captures. It uses no terrain to hide holes and disables wind/shadows for asset comparison.
Full scene integration still uses `sward-cook.ps1`. Persistent walked tracks require an optional
runtime contact/recovery field; low graphics quality must pay none of its allocation/pass costs.
The solver's roots and arc weights are ready for that consumer, but this branch does not add it.
ComfyUI PBR comparisons remain possible after silhouette/coverage is sound; every accepted leaf
appearance must also feed the card and ground bake consistently, with provenance.
