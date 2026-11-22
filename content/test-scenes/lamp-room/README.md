# lamp-room

A closed room at midnight lit by two steady lamps ([renderer](../../../docs/subsystems/renderer.md#lamps), roadmap R86): the globe lanterns of [plan 14](../../../docs/plan/14-structures.md#globe-lanterns) in their smallest form, and the case of a furnished room with nothing to light it but the sky. The room is 6 m by 5 m and 3 m high with a table and a cupboard, every face turned inward and nothing open, under the sky capability's sky at midnight on day 100 at 45° N (no terrain, so the scene's epoch is midnight) with a three-day moon down, its exposure fixed at EV100 3.5 so that the lamplit walls read and nothing else does.

- **The lamps** (`lamps`): a warm one of 40 cd over the table, colour (1, 0.82, 0.58), and a blue one of 25 cd on the far wall, colour (0.45, 0.7, 1.0) — a colour, not a temperature. Neither names a reach, so each ends where it falls to 0.05 lux (28 m and 22 m: past the room's walls).
- **The globes** (`globe-warm.gltf`, `globe-blue.gltf`): icospheres 0.1 m in radius, 320 triangles, whose `glass/lantern` material gives off the lamp's own luminance, L = I / (π r²) in nits over 128,000 (renderer.md says why), through `emissiveFactor` and `KHR_materials_emissive_strength`; each stands at its lamp's centre as an instance.
- **The prop** `lantern`: the warm globe with the warm lamp at its origin, which nothing in the scene places: a game places it (`prop_place`, engine-view `--game`) and it lights where it stands.

The three meshes are text: each buffer is a data URI, written by a scratch script outside the tree (the repository takes no Python) from the numbers above.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/lamp-room/scene.json --offscreen --width 1920 --height 1080 --shadows rt --camera-path content/test-scenes/lamp-room/camera-path.json --marker-captures captures/
build/msvc-release/bin/engine-view --scene content/test-scenes/lamp-room/scene.json --offscreen --width 1920 --height 1080 --shadows csm --camera-path content/test-scenes/lamp-room/camera-path.json --marker-captures captures/
```

**What to look at.** The camera path holds in the corner by where a door would be, looking across the table at the cupboard and the blue lamp (marker `held`, frame 120). Under `--shadows rt` the table's top is bright under the warm globe and its legs' and edge's shadow falls on the floor beneath it, the cupboard's side facing the blue lamp is blue and its shadow is warm-lit only; under `--shadows csm` the lamps cast no shadow. The room's cost per lamp is measured by copies of this scene with 0, 8 and 64 lamps (renderer.md, "Lamps").
