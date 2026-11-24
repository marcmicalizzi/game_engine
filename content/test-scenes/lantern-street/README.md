# lantern-street

**A street of lamps among walls at midnight** (roadmap R87, [lights by reach](../../../docs/subsystems/renderer.md#lights-by-reach); the measurements on [the experiment page](../../../docs/experiments/lights-by-reach.md)): the case of a village's lanterns, each lighting its own surroundings, where the resolve should pay for the lamps that reach a pixel and not for every lamp of the frame.

- **The ground and the walls**: one unit cube (`block.gltf`, outward faces, a plaster grey), placed 65 times by the scene's instances with a scale — a 70 m square slab 0.2 m thick whose top is y = 0, and on an 8 × 8 grid of 7.5 m cells over 60 m × 60 m one wall 4 m long, 3 m high and 0.4 m thick in each cell, along x and along z in turn and 1.2 m off the cell's centre, so the lamps never stand in one.
- **The lamps** (`lamps`): 64 on an 8 × 8 grid over the same square, 2.6 m up, each 40 cd of a warm colour (1, 0.82, 0.58) with a reach of **8 m** (`reach_m`), no globe and no radius. With a reach shorter than the lamps' spacing allows, a point of the street is reached by a few lamps, never by most.
- **The sky**: the sky capability's at midnight on day 100 at 45° N with a three-day moon (the array's one directional light, walked at every pixel), its exposure fixed at EV100 3.5 as `lamp-room`'s.
- **The camera path** (`camera-path.json`): held at a walker's height in the south-west corner looking up the diagonal, eight seconds with a marker at frame 240, so a benchmark along it measures the lights alone.

The cube and the scene are text, written by a scratch script outside the tree from the numbers above; the measurement's copies with 0, 4, 16 and 256 lamps (2 × 2, 4 × 4 and 16 × 16 grids over the same square) are the same file with another grid of lamps.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/lantern-street/scene.json --offscreen --width 3840 --height 2160 --raster rt --shadows rt --audio off --camera-path content/test-scenes/lantern-street/camera-path.json --benchmark lanterns.jsonl
```