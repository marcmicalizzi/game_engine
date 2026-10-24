# elder-grove

Two **ancient trees** of the flora capability's megaflora regime
([flora](../../../docs/subsystems/flora.md#megaflora), `engine.flora.Ancient`) 60 m apart, with
four willows, eight birches and an orchard row of seven around them, on a nearly flat ground of
the renderer's own waves (10 cm of relief, 256 m a side at 2 m):

- **elder A** (`content/flora/ancient.flora.json`) at x = −30: 100 m, a 10 m trunk flaring into six
  buttresses with a doorway-sized hollow at its foot, and nine major limbs from 30 to 62 m whose
  first runs are straight and near level — where platforms go — and which then fork, meander and
  turn up; the recipe asks for a 24 m run, shortened by the wood's mechanical cap. G85 increases
  its limb radius from 1.6 to 1.8 m to carry the fuller crown while retaining the walk test's 8 m
  clear span. Renewal on an explicit run stays beside the trunk;
- **elder B** (`ancient-rising.flora.json`) at x = 30: 90 m, two hollows, eight limbs from 30 to
  55 m whose 24 m runs rise at 20 degrees and more — ramps rather than floors;
- the willows (`willow.flora.json`), the birches (`birch.flora.json`, `birch-single.flora.json`)
  and the orchard row at z = 78 (`orchard-pruned.flora.json`, its middle tree
  `orchard-apple.flora.json`, free and fruiting).

It is the scene the elven village's structures will be laid into, and the one the walk along a
limb runs on (`apps/engine_view/tests/walk_limb_view_tests.cpp`: a walker on each tree's lowest
run, offscreen; flora.md, "Walking a limb"). `camera-path.json` walks in from the orchard row to
elder A's foot, looks up its trunk, rises beside it to its limbs at 30 m, into its canopy at 56 m,
and out over both trees, in 38 s.

**G85 (2026-10-23)** adds inner crown growth, supported collars and irregular wood; the older
counts and timing measurements below describe their dated versions, not the revised trees.
See [shared growth rules](../../../docs/experiments/tree-growth-rules.md) for the new geometry
comparisons. Platform anchors must be revalidated after rebuilding.

It needs no sample asset: every tree is solved from its parameters and built through the
derived-data cache ([flora](../../../docs/subsystems/flora.md#in-a-scene)); the first load builds
seven containers. **The two elders' crowns are the allometric recursion** (G73, 2026-10-20;
[flora](../../../docs/subsystems/flora.md#allometry)): leaves 0.3 m long on 0.8 m twigs, as on a young
tree of the species, and many of them — elder A 1,419,335 triangles, 214,834 twigs over 12 orders and
564,920 leaf cards over a 57 m crown (a leaf area index of 2.9), elder B 1,050,182 triangles and
147,180 twigs — on the default budget of 1,500,000, which holds them at the leaf's own size. Until then
elder A was 196,301 triangles with 5 m twigs of 1.4 m cards. **Since the habits** (G79, 2026-10-22;
[flora](../../../docs/subsystems/flora.md#megaflora)) their limbs fork, meander and are no longer
than their wood could carry, and their crowns are narrower: elder A 606,450 triangles, 195,576 twigs
and 177,812 cards over a 3,201 m² footprint, elder B 386,241 and 138,421 twigs; msvc-release builds
elder A's container in 4.2 s and elder B's in 2.8 s. The GPU and pool numbers below are from before
the habits, on the larger trees. In msvc-debug the first load builds elder
A's container in about 49 s and elder B's in 30 s; at 1080p the camera path costs the GPU 2.0 ms a
frame (median; 0.78 before), and the deformed-vertex pool takes the scene's 7.8 million tree vertices,
93.5 MB ([tree allometry](../../../docs/experiments/tree-allometry.md#what-it-costs) has the table).

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/elder-grove/scene.json --camera-path content/test-scenes/elder-grove/camera-path.json --offscreen --benchmark elder-grove.jsonl
build/msvc-release/bin/engine-view --scene content/test-scenes/elder-grove/scene.json --interactive --walk --start -40.72,1.87,-2.26 -102,0
```

The second line starts a walk six metres outside elder A's hollow, facing it (a live window: run
it yourself; the tests replay their walks offscreen).

**Foliage LOD (R91, 2026-10-21).** The trees' impostor chains now take over along the path: every card group switches on its own sub-tree's sphere and by its coverage error under `renderer.lod.foliage_px` (5 px; `--foliage-lod`), with a dithered cross-fade over 10 % of the switch (`--lod-fade`, 0 switches at once). The default ancient tree's first cards come in about 37 m from a group and its star at about 690 m; the oak's at 28 m and 434 m ([renderer](../../../docs/subsystems/renderer.md#the-foliage-threshold) has the costs before and after).
