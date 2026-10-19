# rock-field

A rock field on a small dune erg, made entirely by the rock solver
([rocks](../../../docs/subsystems/rocks.md), roadmap G62) and the scatter
([scatter](../../../docs/subsystems/scatter.md)): a scene to fly that needs no sample asset.

- **Three basalt outcrops** (`content/rocks/basalt-outcrop.rock.json`) are the scene's own
  instances, grounded, at the middle, to the north-west (scaled 1.3) and the south-east (0.8).
- **A scatter layer**, `rock-field`, read whole over 6 × 6 tiles of 32 m (192 m square) at 0.02 a
  square metre and at most 40 a tile, places three kinds whose `mesh` names a recipe: the granite
  boulder (6 m apart, slopes under 22°, 0.5–1.5 scale), the sandstone boulder (6 m apart, 3 m clear
  of a granite one, 0.5–1.3) and the granite scree (4 m apart, 1.5 m clear of a boulder, 0.8–1.6),
  each aligned to the sand. Exclusion circles of 6, 7.5 and 5 m keep the outcrops clear.
- **The terrain** is the dunes at 2.5 m and a 60 m wavelength, 320 m across.
- **The camera path** (`camera-path.json`) circles the field at 45 m and 5 m above the sand for 24
  seconds looking at the middle outcrop, then walks in from the east between the outcrops at eye
  height for 10; three markers: `east`, `approach`, `between`.

```powershell
build/msvc-debug/bin/engine-view --offscreen --scene content/test-scenes/rock-field/scene.json `
  --camera-path content/test-scenes/rock-field/camera-path.json --marker-captures <dir> --shadows csm
```

The first read solves and clusters the four recipes into the derived-data cache (a few seconds in a
debug build); later reads hit it. `systems/scatter/tests/scatter_scene_tests.cpp` reads this scene
through the renderer's scene reader and checks the recipes, the count and the exclusion circles.
What the markers show is in rocks.md ("What the capture showed").
