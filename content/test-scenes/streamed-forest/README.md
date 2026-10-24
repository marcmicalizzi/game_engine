# streamed-forest

Three thousand of the flora capability's own trees on two kilometres of rolling ground, streamed
tile by tile (roadmap W69; [renderer](../../../docs/subsystems/renderer.md#authored-instances-per-tile),
[authored](../../../docs/subsystems/authored.md)). It is the engine's own stand-in for the forest
proof of concept's full v8 scene, which was refused before its first frame because a scene's own
instances all sat in its prefix.

- **The trees**: 900 oaks and 800 birches with their card levels and star (R89/R91), 700 tall and
  600 young spruces with a decimated chain only, scattered from seeds over a disc of 950 m, each a
  file instance of the scene — so with the `world` block the reader hands all of them to the
  `authored` placement generator, filed by the tile their origin is on.
- **The ancient** (`ancient.flora.json`, 80 m) reaches past a 32 m tile, so it stays in the prefix
  and is drawn from anywhere, as a landmark is.
- **The rings**: 32 m tiles in four rings of 1.5, 6, 16 and 34 tiles (48 m, 192 m, 512 m,
  1,088 m). Inside the first a tree is its full mesh; each ring past it draws the distance form cut
  at the nearest an origin on that ring's tiles can be to the camera, so the handover draws the
  same clusters; past the last a tile is not held and its trees are not drawn.
- **The ground** is the procedural `Waves` terrain, not an authored heightfield: the repository
  takes no binary assets, and a heightfield's raw grid is one. The streaming is the same over
  either; the v8 run below is over the proof of concept's authored region.
- **The path** (`camera-path.json`) crosses the forest above the crowns for forty seconds, past the
  ancient, and comes down among the trunks for ten.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/streamed-forest/scene.json --offscreen `
  --camera-path content/test-scenes/streamed-forest/camera-path.json --frames 600 --benchmark out.jsonl
```

`--world-instances whole` reads the same scene with every tree in the prefix, which is what a
streamed world did before 2026-10-24.

**Measured** (2026-10-24, RTX 5090, release, 600 frames, a shared machine — upper bounds; the
numbers with their `machine_state` are in [renderer](../../../docs/subsystems/renderer.md#authored-instances-per-tile)):
streamed, 30,925 prefix pairs and a tail of 253,597 at the end of the path (rings 0–3: 5, 102, 474
and 1,457 trees, 5,283, 92,876, 139,953 and 15,485 pairs), 0.62 ms of GPU at the median; read whole,
2,910,325 pairs and 2.81 ms. Its 1,532 handovers change no pixel with `--lod-fade 0`.
