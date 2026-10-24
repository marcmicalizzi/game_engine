# ground-by-place

**Ground materials authored by place** (roadmap W72; [renderer](../../../docs/subsystems/renderer.md#ground-materials-by-place),
[tileset](../../../docs/subsystems/tileset.md#the-mask-level), the grounding in
[ground materials](../../../docs/experiments/ground-materials-grounding.md)): a 1 km authored
ground with a mountain, a forest plain with a trail and a clearing, and two ponds at nearly one
height with different beds, drawn from a palette of eleven substrates of which each 32 m mask tile
names at most four.

## Write the heightfield first

The grid is binary and never committed (`.gitignore`); `engine-content terrain-synth` writes it and
its header, the same floats on every toolchain:

```powershell
build/msvc-debug/bin/engine-content terrain-synth content/test-scenes/ground-by-place/ground.json `
  --kind mountain --size 1025 --spacing-mm 1000 --origin-mm -512000,-512000
build/msvc-debug/bin/engine-content terrain-masks content/test-scenes/ground-by-place/scene.json   # optional: the viewer builds it on a miss
build/msvc-debug/bin/engine-view --scene content/test-scenes/ground-by-place/scene.json --terrain-tiles --terrain-far 2 --time-rate 0
```

The set and the mask level go to the repository's `ddc/` (`tilesets/<key>/`, `tilemasks/<key>/`).
`terrain-masks --write-priors <dir>` writes the priors alone as greyscale PNGs, one per role.

## What each region was given, and what the priors did

The shape (`--kind mountain`): a 180 m mountain centred at (179, -179) whose profile `(1 - d²)³`
steepens to about 48° two-fifths of the way down from the summit and flattens at its foot 282 m
out, on a plain rising 8 m from south to north, with two bowls 2.5 m deep at (-256, 230) and
(51, 307), their rims 0.6 m apart in height.

| Input | Where | Gives |
|---|---|---|
| priors | everywhere | `soil` below 29°, `scree` from 29° (whole by 35°), `bedrock` from 39° (whole by 45°), `snow` above 150 m with a 20 m blend, 25 m lower facing north (-z) and higher facing south, none past 58° |
| region "alpine" | a 250 m circle on the mountain, 40 m feather | `base`: alpine meadow stands in for the priors' soil; scree, bedrock and snow stay where the priors put them |
| region "forest floor" | the whole map, after "alpine" | `base`: forest litter for what soil is left — the plain |
| region "clearing" | a 22 m circle at (-140, 120), 6 m feather | clearing grass outright |
| region "trail" | a 4 m strip from the sandy pond to the mountain's foot | trail outright, a 0.75 m feather |
| region "sandy pond" | the `sandy-pond` water body's circle | `bed` sand inside (1.5 m feather), `margin` gravel within 3 m outside, wet to 8 m: the exposed shore |
| region "boggy pond" | the `boggy-pond` water body's circle | `bed` and `margin` organic mud (5 m, 3 m feather), wet to 16 m: the sheltered, swampy one |

The two ponds stand at 1.9 m and 1.3 m: the priors give both the same soil, so their beds are
entirely the regions' — the case slope and elevation cannot decide. The mountain's snowline is the
scene's (a 185 m mountain is a test shape, not a range: the published aspect offset of a few
hundred metres is scaled to it). Layers are colour factors and porosity alone, so the scene needs
no texture pack and every region reads as its colour; a scene with a material pack names images
per layer as W68 does, and height maps (`height`) for the height blend.

Captures (2026-10-24, `msvc-debug`, RTX 5090, 1600 × 900, under
`D:/workspace/game_engine_local/ground-materials/captures/`): `overview` (the mountain's bands, the
trail, the clearing, the two ponds' margins), `mountain` and `mountain-north` (the snow lower on the
north face, the scree band below the bedrock, the meadow at the foot), `sandy-shore` and
`sandy-under` (gravel margin, the sand bed under the volume-tier water), `boggy-shore` (the dark wet
mud margin), `trail`, and `shore-night` (22:00, the shore fire on the gravel margin,
`--exposure-ev100 1`).

The camera path (`camera-path.json`) goes over the sandy shore, along the trail past the clearing,
to the mountain's foot and up it, and over the boggy pond; it is the path the resolve's costs were
measured on.
