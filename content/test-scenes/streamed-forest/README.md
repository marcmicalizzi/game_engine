# streamed-forest

Three thousand of the flora capability's own trees on two kilometres of rolling ground, streamed
tile by tile (roadmap W73; [renderer](../../../docs/subsystems/renderer.md#authored-instances-per-tile),
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

## The turnover variant

**`turnover.json`: the walk's stalls without the forest** (roadmap T168, 2026-11-17). The owner's
walks of the forest proof of concept flew at 300 to 1,000 m/s, 400 to 800 m up, and dropped a
walker from that flight; what that does — the tail emptying and filling by hundreds of thousands
of instances, its tables growing, a walker's ring of collision tiles wanted at once — the grounded
walk never does, and until this variant only the forest's staging, which no checkout has,
reproduced it. This is the same scene with:

- **ten times the trees**, 30,000 on a disc of 2 km (9,000 oaks, 8,000 birches, 7,000 tall and
  6,000 young spruces), and under them a streamed scatter layer of solved boulders and stones
  (desert-endless's, at up to 120 a tile, to the last ring), so the rings hold about a hundred
  thousand instances and a flight across the disc turns the tail over as the forest's does. A
  hundred times the trees was tried first: 300,000 swaying trees were still loading after six
  minutes at 46 GB of private commit, which no flight should need;
- **the ground drawn from the world's tiles** (`"world": {"ground": true}`, the rings' cells as
  `desert-endless` has them), so the walker stands on the renderer's tile source and its collision
  builds tiles ahead on its workers (scene_collision.md, "Ahead of a running walker"): over the
  procedural `Waves` ground read as one grid there is no tile source, nothing is built ahead, and
  a drop costs nothing to reproduce;
- **two flights the engine owns**: `fast-flight.json`, six seconds walking, a climb, then about
  13 km across the disc at 450 m/s, 400 to 700 m up, and down to walk again (56 s, 1,680 frames at
  30 fps), after T164's fast flight over the staging; and `run-drop.jsonl`, an input log replayed
  offscreen (`--replay-input`): a sprint, a flight at 800 m/s and a drop, twice, after T166's.

**What it reproduces so far** (2026-11-17, batch 67's build with T167, 3840 × 2160, `--raster rt
--shadows rt`, the bounce on, `--terrain-async`, the session locked and other agents' builds beside
it: upper bounds): it loads in about 100 s at 35 GB of private commit; the fast flight turns the
tail over between 17,000 and 58,000 instances (2.6 M pairs at most) and grows the trees' tables
three times (relayouts of up to 17.5 ms), at a median of 10.7 ms and a worst frame of 35.8 ms. It
does **not** yet reproduce the forest's growths of a 500,000-slot table, and the replayed drop
built nothing ahead (27 activations, none ready, waited for or built inline, 52 ms in all): the
walker in this host path does not stand on the tiles' source here, which is the next thing to
find (roadmap T168).

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/streamed-forest/turnover.json --offscreen `
  --width 3840 --height 2160 --raster rt --shadows rt --terrain-async `
  --camera-path content/test-scenes/streamed-forest/fast-flight.json --warmup 0 --repeat 1 --benchmark out.jsonl
build/msvc-release/bin/engine-view --scene content/test-scenes/streamed-forest/turnover.json --offscreen `
  --width 3840 --height 2160 --raster rt --shadows rt --terrain-async `
  --replay-input content/test-scenes/streamed-forest/run-drop.jsonl --benchmark out.jsonl
```
