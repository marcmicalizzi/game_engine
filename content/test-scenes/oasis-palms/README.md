# oasis-palms

A dozen **generated date palms** standing round a shallow basin on a small dune field: the flora
capability's solver ([flora](../../../docs/subsystems/flora.md)) makes every one of them from its
parameters, so the scene needs no sample asset and no generated image. The mesh table names five
flora files from [`content/flora/`](../../flora) — the default 9 m palm and four variants (a 14 m
one, a young 3.5 m one with a heavy skirt, one leaning 22°, and an old one with sparse drooping
fronds and no skirt) — and the twelve instances place them, each turned and some sized, between 3
and 25 m from the basin's middle. The terrain is the dune generator's (`"generator": "Dunes"`, 320 m
square at 1.25 m), so the scene needs the terrain capability too; the basin is 2.5 m deep, where an
oasis would be.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/oasis-palms/scene.json --interactive
build/msvc-release/bin/engine-view --scene content/test-scenes/oasis-palms/scene.json --offscreen --frames 1 --capture oasis.png
build/msvc-release/bin/engine-view --scene content/test-scenes/oasis-palms/scene.json --camera-path content/test-scenes/oasis-palms/camera-path.json --benchmark oasis.jsonl
```

The first load solves the five palms and clusters them into the derived-data cache (a second or two
each in a release build); every later load reads the containers. `engine-content flora
content/flora/date-palm.flora.json --cache` builds the same entry ahead of time.

**The camera path** (`camera-path.json`) goes once round the grove in 32 seconds at 38 m and 6 m
above the sand, looking at its middle, and then walks in under the crowns at eye height for twelve
more. Its keys hold heights above the ground.

**What to look at.** Green crowns against the sky and brown trunks below them; the leaf-scar rings
up each trunk; fronds that rise, arch and droop, their leaflets in a V; tan dead fronds hanging
under most crowns; and no two palms alike. Close in, the leaflets are geometry, seen from both
faces. Far out, the crowns coarsen through their cluster DAGs.

**The wind** (roadmap S66, [renderer](../../../docs/subsystems/renderer.md#the-flora-stage)). The
palms sway in the dune field's own wind — its record's direction and strength at the scene's
time — with a slight bend of the trunks and the fronds swinging most, no two in step. Walk in
(`--interactive --walk`) and the fronds you walk into bend away from you; the trunks still stop
you. `--tunable renderer.flora.wind_strength=0` does nothing here (the ground names a wind);
`--tunable renderer.flora.motion=false` draws the palms rigid.

**What pins it.** `systems/renderer/tests/flora_scene_tests.cpp` loads this file, checks every
`.flora.json` entry became a mesh with the three materials, and on a GPU captures the first frame of
the path offscreen and holds the pixels the crowns cover to green and the trunks' to brown, the
crowns above the trunks.
