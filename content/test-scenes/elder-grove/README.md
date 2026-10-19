# elder-grove

Two **ancient trees** of the flora capability's megaflora regime
([flora](../../../docs/subsystems/flora.md#megaflora), `engine.flora.Ancient`) 60 m apart, with
four willows, eight birches and an orchard row of seven around them, on a nearly flat ground of
the renderer's own waves (10 cm of relief, 256 m a side at 2 m):

- **elder A** (`content/flora/ancient.flora.json`) at x = −30: 100 m, a 10 m trunk flaring into six
  buttresses with a doorway-sized hollow at its foot, and nine major limbs from 30 to 62 m whose
  first runs are straight and near level — where platforms go;
- **elder B** (`ancient-rising.flora.json`) at x = 30: 90 m, two hollows, eight limbs from 30 to
  55 m whose runs rise at 20 degrees and more — ramps rather than floors;
- the willows (`willow.flora.json`), the birches (`birch.flora.json`, `birch-single.flora.json`)
  and the orchard row at z = 78 (`orchard-pruned.flora.json`, its middle tree
  `orchard-apple.flora.json`, free and fruiting).

It is the scene the elven village's structures will be laid into, and the one the walk along a
limb runs on (`apps/engine_view/tests/walk_limb_view_tests.cpp`: a walker on each tree's lowest
run, offscreen; flora.md, "Walking a limb"). `camera-path.json` walks in from the orchard row to
elder A's foot, looks up its trunk, rises beside it to its limbs at 30 m, into its canopy at 56 m,
and out over both trees, in 38 s.

It needs no sample asset: every tree is solved from its parameters and built through the
derived-data cache ([flora](../../../docs/subsystems/flora.md#in-a-scene)); the first load builds
seven containers, elder A's 196,301 triangles among them.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/elder-grove/scene.json --camera-path content/test-scenes/elder-grove/camera-path.json --offscreen --benchmark elder-grove.jsonl
build/msvc-release/bin/engine-view --scene content/test-scenes/elder-grove/scene.json --interactive --walk --start -40.72,1.87,-2.26 -102,0
```

The second line starts a walk six metres outside elder A's hollow, facing it (a live window: run
it yourself; the tests replay their walks offscreen).
