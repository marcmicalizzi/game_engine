# Tree bases on ramps

The scene a tree's base on sloping ground is held to (roadmap G201;
[tree base on slopes](../../../docs/experiments/tree-base-on-slopes.md), "Stages";
[flora](../../../docs/subsystems/flora.md#the-trunks-skirt)). One species, the tall spruce of
[`spruce-tall.flora.json`](../../flora/spruce-tall.flora.json), on four planar ramps of 0, 15, 30
and 45 degrees, side by side along x at -45, -15, 15 and 45 m, each falling towards +z. Eight trees
a ramp stand down its fall line at z = -21 to 21 m, 6 m apart, at yaws 0 to 315 degrees, each with
its origin on the ramp (`translation.y` = -tan(a) z, not `ground`).

The ramps are one unit quad, [`ramp.gltf`](ramp.gltf) (text: its buffer is a data URI), turned about
x by the ramp's angle and scaled 24 by 70 m, so the ground under a tree is exactly the plane through
the ramp's centre line, with nothing of a terrain's sampling between the test and the geometry. The
scene has no terrain and no sky: the renderer's stand-in sun.

Cameras: [`view-30-2m.json`](view-30-2m.json) is one still view, the 30 degree ramp's spruce at yaw
180 from 2 m with the eye 0.6 m over the ramp, the pair the owner was shown (before and after the
skirt). [`camera-path.json`](camera-path.json) tours it: that view, the same ramp from 10 m
down-slope, every ramp from 50 m down-slope, and the 45 degree ramp across the slope at 10 m.

What holds it: `systems/renderer`'s "flora scene: no tree on a ramp of up to 45 degrees shows a gap
under its trunk" looks at every tree across the slope from 3 m and reads the ids: under each column
the tree draws low in the frame, nothing but the tree and its own ramp down to the ramp. Before the
skirt (main at 33ff2824) the 30 degree ramp failed it; with the skirt every ramp passes.

```
engine-view --scene content/test-scenes/tree-bases/scene.json --offscreen --frames 4 \
  --camera-path content/test-scenes/tree-bases/view-30-2m.json --capture out.png
```
