# Tree bases on ramps

The scene a tree's base on sloping ground is held to (roadmap G201;
[tree base on slopes](../../../docs/experiments/tree-base-on-slopes.md), "Stages";
[flora](../../../docs/subsystems/flora.md#the-trunks-skirt)). One species, the tall spruce of
[`spruce-tall.flora.json`](../../flora/spruce-tall.flora.json), on four planar ramps of 0, 15, 30
and 45 degrees, side by side along x at -45, -15, 15 and 45 m, each falling towards +z. Eight trees
a ramp stand down its fall line at z = -21 to 21 m, 6 m apart, at yaws 0 to 315 degrees, each with
its origin on the ramp (`translation.y` = -tan(a) z, not `ground`).

The ramps are one unit slab, [`ramp.gltf`](ramp.gltf) (text: its buffer is a data URI), its top the
unit square and its bottom 2 m under it, turned about x by the ramp's angle and scaled 24 by 70 m, so
the ground under a tree is exactly the plane through the ramp's centre line, with nothing of a
terrain's sampling between the test and the geometry, and a skirt under a tree by a ramp's edge is
inside the slab, not hanging below a sheet (G201 stage 2; until then the ramp was a quad). The
scene has no terrain and no sky: the renderer's stand-in sun.

**[`seat.json`](seat.json): trees seated by their footprints** (G201 stage 2;
[renderer](../../../docs/subsystems/renderer.md#a-tree-seated-by-its-footprint)). The same spruce
at eight yaws 5 cm inside the edge of a tread raised 1.2 m over flat ground (a `Bench` edit, its fill
faces at 3 in 1) and eight 5 cm off the lip of a cutting 1.5 m deep (a `Cut` edit, its banks at 4 in
1), every one `ground: true` with its rim over a face steeper than its skirt reaches; the load lowers
each until its skirt is under the face's foot. The ground is a `tiles` heightfield, flat, 257 points
at 0.25 m, which is binary and **never committed**: write it first. [`seat-views.json`](seat-views.json)
holds one still view a tree, from 3 m along its edge at eye height, a marker each.

```
engine-content terrain-synth content/test-scenes/tree-bases/seat-ground.json --kind flat --size 257 --spacing-mm 250 --origin-mm -32000,-32000
engine-view --scene <absolute path>/content/test-scenes/tree-bases/seat.json --offscreen \
  --camera-path content/test-scenes/tree-bases/seat-views.json --marker-captures <dir>
engine-content ground sample content/test-scenes/tree-bases/seat.json --point -4.6,-21,0.33 --skirt 0.48
```

What holds it: `systems/renderer`'s "a tree on a tread's edge or a cutting's lip is seated with no
gap", the same ground and trees written into its scratch, read from the ids and the depth.

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
