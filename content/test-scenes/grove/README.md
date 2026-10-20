# grove

Twelve generated trees of the flora capability's three species on a nearly flat ground: four oaks
(`content/flora/oak.flora.json`, `engine.flora.Broadleaf`), four tall and three young spruces
(`spruce-tall.flora.json`, `spruce-young.flora.json`, `engine.flora.Conifer`) and one date palm,
each instance turned and some sized. `camera-path.json` circles the grove for 32 s at 34 m and 7 m
above the ground, looking at its middle, then walks in among the trunks at eye height for 18 s.

It needs no sample asset: every tree is solved from its parameters and built through the
derived-data cache like any glTF ([flora](../../../docs/subsystems/flora.md#in-a-scene)), so the
first load builds four containers and every later one reads them.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/grove/scene.json --camera-path content/test-scenes/grove/camera-path.json --offscreen --benchmark grove.jsonl
```

**The ground is the wave generator's sand at 15 cm of relief**, not a meadow: a scene's terrain
has no colour of its own, and a grass-coloured ground is a tile set with a material
([tileset](../../../docs/subsystems/tileset.md)), which this scene does not carry yet. It is the
trees that the scene is for: the three species side by side at one scale, their canopies'
coverage, their LOD from 34 m in to under the crowns, and the motion contract's parts swaying.

**The wind** (roadmap S66, [renderer](../../../docs/subsystems/renderer.md#the-flora-stage)). The
waves say no wind, so the trees sway in the renderer's default — a mean day towards +x —
which `--tunable renderer.flora.wind_strength=2,renderer.flora.wind_deg=90` turns into a gale
towards +z and `renderer.flora.wind_strength=0` calms: in a calm the grove is its rest pose.
`--tunable renderer.flora.motion=false` draws every tree rigid, which is how the costs in
renderer.md were measured against. Walking (`--interactive --walk`) into a tree's leaves bends
them aside; its trunk and, on the trees with a structural table, its load-bearing limbs stop you.
