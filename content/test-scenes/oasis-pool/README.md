# oasis-pool

The first water ([water](../../../docs/subsystems/water.md), roadmap S63): a pool of 20 m in a hollow of small dunes, its level below the sand all round its edge, so the surface meets the sand at a shore inside the circle. The terrain is the terrain capability's dunes (seed 5, 3 m dunes 40 m apart, 256 m square at 1 m) with a basin 15 m across and 3.5 m deep at the origin; the pool's level is −0.45 m, its bottom 2.27 m deep at the centre and dry all round its 10 m circle (`water_scene_tests.cpp` holds both). A ring of the scatter capability's procedural rocks stands round it, kept 10.6 m from the centre, and the earth's sky is at 10:00 on day 100 at 31° N (the terrain's time, three years and ten hours into the world), the sun 53° up towards +x and +z.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/oasis-pool/scene.json --interactive
build/msvc-release/bin/engine-view --scene content/test-scenes/oasis-pool/scene.json --camera-path content/test-scenes/oasis-pool/camera-path.json --offscreen --width 1920 --height 1080 --marker-captures captures/
build/msvc-release/bin/engine-view --scene content/test-scenes/oasis-pool/scene.json --interactive --walk --start 13,1,0 90,-10
```

**What to look at.** The camera path circles the pool two metres over the sand 22 m out and then crosses it a metre and a half over the water; its markers: `east` (frame 0, from the east shore), `sun` (frame 900, from the south-west, looking towards the morning sun) and `across` (frame 1170, low over the middle). **The walk** starts on the east shore three metres from the water, looking at the pool.

**The water's optics** (2026-10-18, [Translucent by volume](../../../docs/subsystems/water.md#translucent-by-volume)): absorption (0.6, 0.12, 0.06) and scattering (0.04, 0.07, 0.085) a metre with a little turbidity (0.15), chosen so that the shore's sand shows through the shallows, the first metre turns sea-green and the 2.3 m middle is the water's own deep blue-green. `--water-quality low` draws the surface alone, the bottom's colour faded by the depth map rather than painted through (2026-10-19: the depth map, not the mesh's vertex depths, so neither tier shows the mesh's rings).

Nothing here is a benchmark; the scene exists to be looked at and walked into.
