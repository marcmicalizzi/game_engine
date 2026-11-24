# lantern-street

**A street of lamps among walls at midnight** (roadmap R87, [lights by reach](../../../docs/subsystems/renderer.md#lights-by-reach); the measurements on [the experiment page](../../../docs/experiments/lights-by-reach.md)): the case of a village's lanterns, each lighting its own surroundings, where the resolve should pay for the lamps that reach a pixel and not for every lamp of the frame.

- **The ground and the walls**: one unit cube (`block.gltf`, outward faces, a plaster grey), placed 65 times by the scene's instances with a scale — a 70 m square slab 0.2 m thick whose top is y = 0, and on an 8 × 8 grid of 7.5 m cells over 60 m × 60 m one wall 4 m long, 3 m high and 0.4 m thick in each cell, along x and along z in turn and 1.2 m off the cell's centre, so the lamps never stand in one.
- **The lamps** (`lamps`): 64 on an 8 × 8 grid over the same square, 2.6 m up, each 40 cd of a warm colour (1, 0.82, 0.58) with a reach of **8 m** (`reach_m`), no globe and no radius. With a reach shorter than the lamps' spacing allows, a point of the street is reached by a few lamps, never by most.
- **The sky**: the sky capability's at midnight on day 100 at 45° N with a three-day moon (the array's one directional light, walked at every pixel), its exposure fixed at EV100 3.5 as `lamp-room`'s.
- **The camera path** (`camera-path.json`): held at a walker's height in the south-west corner looking up the diagonal, eight seconds with a marker at frame 240, so a benchmark along it measures the lights alone.

The cube and the scene are text, written by a scratch script outside the tree from the numbers above (the cube's 912 bytes are the base64 buffer inline in `block.gltf`). **The measurement's variants are made from the committed `scene.json` by editing its `lamps` and `fires` arrays alone**, nothing else of the file changing:

- **0, 4, 16 and 256 lamps**: `lamps` replaced by a k × k grid (k = 0, 2, 4, 16), lamp (i, j) at x = (i + 0.5) · 60 / k − 30, z = (j + 0.5) · 60 / k − 30, y = 2.6, every other field the committed lamps' (colour (1, 0.82, 0.58), 40 cd, `reach_m` 8, names `lamp i j`).
- **N fires in place of the lamps** (0, 4, 8, 16, 32; R87's mend): `lamps` removed, and `fires` holding N campfires at the centres of the 8 × 8 wall cells, fire k in cell c = ⌊(k + 0.5) · 64 / N⌋ of the raster order (column c mod 8, row ⌊c / 8⌋, centre ((column + 0.5) · 7.5 − 30, 0.05, (row + 0.5) · 7.5 − 30)), each `{"radius_m": 0.3, "height_m": 0.8, "intensity": 250, "color_k": 1800, "smoke": {"density": 0}, "flicker": 0.6, "seed": k + 1}` — the campfire scene's fire without its smoke, so the medium's cost stays small beside the lights'.
- **The grid's worst case** (R87's mend): the 16 × 16 lamps above with `reach_m` 400 and the 64 fires of every cell, 320 lights of which every one covers the whole box, so no cell culls anything.

Written for this repository and under its licence (Apache-2.0, the root `LICENSE`), as everything in the tree is unless a `LICENSES.md` beside it says otherwise.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/lantern-street/scene.json --offscreen --width 3840 --height 2160 --raster rt --shadows rt --audio off --camera-path content/test-scenes/lantern-street/camera-path.json --benchmark lanterns.jsonl
```