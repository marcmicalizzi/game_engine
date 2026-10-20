# escarpment

A ridge whose steep flanks are dressed with cliff faces by the `cliffs` generator
([rocks](../../../docs/subsystems/rocks.md#the-cliffs-generator), roadmap G71): the scene for
landscape-scale rock — faces found from the ground's heights rather than drawn by an author.

- **The ground** is an authored heightfield under the `tiles` provider
  ([tileset](../../../docs/subsystems/tileset.md)): `engine-content terrain-synth --kind ridge`,
  401 points a side at 1 m (400 m square, centred on the origin), at a vertical scale of 3 — a ridge
  120 m high along the diagonal x = −z / 4, its flanks steepening to 66° and rounding over at the
  top, on ground rising 18 m across. The grid is binary and **never committed**: write it first
  (below). The scene's grid draws it at 1 m.
- **The `cliffs` layer**, `escarpment`, traces the region on blocks of 400 m at 4 m: slopes over 50°,
  their feet traced into lines and stood with **sandstone faces** — undercut 0.5, a talus skirt of
  0.15 of the height, 24 fallen blocks of up to 2.5 m each, the default 250,000-triangle budget, the
  rock pack's `rock/sandstone` where the pack is installed and the flat colour otherwise — and
  **granite boulders** (`content/rocks/granite-boulder.rock.json`) at 4 per 100 m of face, 0.8–2.2
  scale, at their feet. Four faces, two a flank, about 215 m long and to 113 m high, overlapping where
  the blocks' grid at x = 0 and z = 0 divides them. The faces' recipes are written under the
  derived-data root's `cliffs/`.
- **The camera path** (`camera-path.json`) spends 34 seconds at the east flank: the whole face from
  the valley floor, along it at mid height, up over the crest looking down it, and a walk at eye
  height along its foot outside the skirt; three markers: `valley`, `along`, `foot`.

```powershell
build/msvc-debug/bin/engine-content terrain-synth content/test-scenes/escarpment/ridge.json `
  --kind ridge --size 401 --spacing-mm 1000 --origin-mm -200000,-200000
build/msvc-debug/bin/engine-view --offscreen --scene (Resolve-Path content/test-scenes/escarpment/scene.json) `
  --camera-path content/test-scenes/escarpment/camera-path.json --marker-captures <dir> `
  --shadows csm --sun 120,35
# a walk from the foot, live (the drop lands it on the ground outside the skirt; turn west to the face):
build/msvc-debug/bin/engine-view --scene (Resolve-Path content/test-scenes/escarpment/scene.json) `
  --walk --start 120,20,20 0,0
```

Name the scene by an **absolute path**: the `tiles` provider builds the set into the repository's
`ddc/`, which it finds by walking up from the heightfield's directory, and a relative path gives it
nowhere to walk from (a wart of the provider's, [tileset](../../../docs/subsystems/tileset.md)).
The first read builds the tile set and solves and clusters the four faces (seconds in a debug
build); later reads hit the cache. `domain/rocks/tests/cliffs_generator_tests.cpp` runs this scene's
layer over the same ridge and checks both flanks are dressed and every face solves; what the markers
show is in rocks.md ("The escarpment").
