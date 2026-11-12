# steep-dip

Bedrock on steeply dipping beds (roadmap G200;
[rocks](../../../docs/subsystems/rocks.md#beds-at-any-attitude), the grounding in
[steep-dip-bedrock](../../../docs/experiments/steep-dip-bedrock.md)): one mountain whose flanks
take every attitude of the beds to the slope.

- **The ground** is an authored heightfield under the `tiles` provider: `engine-content
  terrain-synth --kind mountain`, 1001 points a side at 1 m (a kilometre square centred on the
  origin), at a vertical scale of 1.25 — a mountain about 230 m high centred at (175, -175), its
  flanks steepening to about 54 degrees two-fifths of the way down and easing to the plain. The
  grid is binary and **never committed**: write it first (below).
- **The formation** is the forest proof of concept's (its v41 scene's): a gneiss sequence of eight
  beds between 0.25 and 7.8 m, dipping 57 degrees toward 128 degrees (from +x toward +z), folded
  over 470 m and 3.3 km, with a macro sequence of members from a 141 m paragneiss to a 6 m
  graphitic schist. So the flank facing 128 degrees has its beds dipping with the slope, the flank
  opposite has them dipping into it, and the flanks facing 38 and 218 degrees have their strike
  running up the slope.
- **The `ledges` layer** stands the resistant beds on the 30 to 50 degree ground of a 576 m square
  round the mountain (81 tiles, the near form in the first ring, the far form to the third), and
  **the `cliffs` layer** faces the ground past 50 degrees in gneiss with no talus, both carrying
  the formation (a placement generator is handed its entry, not the scene).
- **The camera path** has four markers: `far`, the whole mountain from a kilometre; `dip-slope`,
  the flank whose beds dip with it from 100 m; `strike`, a flank whose strike runs up it from
  100 m; `near`, a ledge on the flank whose beds dip into it from 10 m at eye height.

```powershell
build/msvc-release/bin/engine-content terrain-synth content/test-scenes/steep-dip/mountain.json `
  --kind mountain --size 1001 --spacing-mm 1000 --origin-mm -500000,-500000
build/msvc-release/bin/engine-view --offscreen --scene (Resolve-Path content/test-scenes/steep-dip/scene.json) `
  --camera-path content/test-scenes/steep-dip/camera-path.json --marker-captures <dir>
```

Name the scene by an absolute path (the `tiles` provider walks up from the heightfield to find
the repository's `ddc/`, as the escarpment's README says). What the markers showed is on the
grounding page.
