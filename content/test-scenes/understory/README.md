# understory

A grove's floor (roadmap G75; [the understory](../../../docs/experiments/understory.md)): the four
understory species of the flora solver under trees from the existing recipes, placed the way a
temperate forest's layers stand — shrubs in the canopy's gaps, ferns and nettles in its shade,
grasses and flowers in the clearing's light.

- **The canopy**: two oaks and a third larger (`oak.flora.json`), a birch, a young spruce, and two
  oak saplings (`oak-sapling.flora.json`, a 3 m broadleaf: a sapling is a tree species at a small
  height since the allometric crown), round an octagonal clearing 14 m across.
- **Three scatter layers** ([scatter](../../../docs/subsystems/scatter.md)), each over a polygon
  and kept 1.2 m clear of every trunk:
  - `understory-shrubs`: hazel (`hazel.flora.json`, 4 m, sixteen stems that stop a walker) and
    dogwood (`dogwood.flora.json`, 2.2 m, thumb-thick stems a walker passes) in the ring between
    the clearing and the grove's edge, 0.022 a square metre — about twenty;
  - `understory-shade`: male fern, bracken and nettle in the same ring, a noise density of up to
    0.25 a square metre, the ferns' spacing a metre;
  - `understory-light`: hair-grass and fescue tufts and daisies in the clearing, 2 a square metre.
- **By hand**: three foxgloves at the clearing's edge and two brambles by the path — a spike of
  flowers and arching canes are a few plants placed where an author wants them, which a density
  field does not say.

415 instances (403 scattered), 35,387 pairs. `camera-path.json` walks it at eye height for 44 s:
round the ring past the shrubs and ferns, into the clearing, and out under the third oak.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/understory/scene.json --camera-path content/test-scenes/understory/camera-path.json --offscreen --benchmark understory.jsonl
```

**What it costs** (msvc-debug, RTX 5090, 1080p, traced shadows, 2026-10-21; a shared machine, so
upper bounds; [the understory](../../../docs/experiments/understory.md#the-understory-scene) has the
table): the GPU 0.88 ms at the median with the plants swaying and 0.74 rigid; the frame thread
14.1 ms swaying and 0.79 rigid, about 33 µs an instance composing every frond's and blade's pose —
the limit on how much of a forest floor sways, and what roadmap G77 is to lift.

**Walking it** (`--interactive --walk`): the hazels' stems stop you, everything else — the
dogwoods' stems, every leaf, frond, blade and flower — bends aside as you pass
([scene_collision](../../../docs/subsystems/scene_collision.md)). The wind is the renderer's
default; `--tunable renderer.flora.motion=false` draws everything rigid.

**What it is not yet.** The ground is the wave generator's sand, not a forest floor's litter (a
tiled set on the terrain); the shade and light layers are polygons drawn round the trees by hand,
where G77 will compute the light the canopy lets through; every card is an opaque polygon until
alpha masks (R65) and the understory's images (G60's roles, [content/materials](../../materials/README.md))
exist; and the ferns' `clear` of the shrubs is not applied: the scene reader measures an earlier
entry's flora meshes at a path it joins to the scene's directory twice, and warns that it could not
(the scatter layers still place, without that exclusion).
