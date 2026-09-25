# desert-overlook

The first scene of the benchmark corpus of [plan 09 §9.4](../../../docs/plan/09-testing-profiling.md#94-benchmark-scene-corpus): a flythrough with occlusion designed in. A 5.1 km procedural dune field with two rock ridges and an oasis basin, five landmarks from 100 m to 2.6 km, props along the approach, 64 instanced palms, and a 40-second camera path laid out so that each landmark sits fully occluded, partly occluded and in view at frames this page names. The measurements taken on it are in [docs/experiments/flythrough-desert-overlook.md](../../../docs/experiments/flythrough-desert-overlook.md); the format is `engine.scene.Scene` and `engine.scene.CameraPath` ([renderer](../../../docs/subsystems/renderer.md#scenes-camera-paths-and-flythroughs)).

| File | What |
|---|---|
| `scene.json` | the scene: 13 meshes, 25 placed instances, two palm scatters (64 palms), and the terrain |
| `camera-path.json` | 14 keys, cubic, 60 fps: 2,401 frames; and the markers below |

**It is not a reference scene.** `tools/ci/reference-compare.ps1` reads only the `*.json` files at the top of `content/test-scenes/`, and this one lives in a directory of its own because it is a different kind of fixture: a path to be measured along, not one frame compared with a path trace.

## Running it

```powershell
tools/fetch-samples.ps1        # the substitute set: seven Khronos samples, pinned
engine-view --scene content/test-scenes/desert-overlook/scene.json `
            --camera-path content/test-scenes/desert-overlook/camera-path.json `
            --benchmark run.jsonl --width 1920 --height 1080 --shadows off
# the owner's landmarks, where the local manifest exists:
#            --overlay D:/workspace/game_engine_local/scenes/desert-overlook/landmarks.json
tools/flythrough.ps1 -Preset msvc-release -Set substitute   # the whole measurement matrix
```

`--census` adds, per frame, the visible pairs by DAG level and by mesh; `--census-pixels` the pixels each mesh covers as well, which is how the states below were read; `--verify-occlusion` draws every frame with and without occlusion culling and compares them pixel for pixel; `--marker-captures <dir>` writes a picture of every marker. The terrain is built once per machine (about 45 s in `msvc-release`) and cached in `ddc/` under a key over its fields.

## Two asset sets, one set of placements

Every placement is made through a mesh's **fit** — scale to a height or an extent, bottom centre on the placement point — so whatever file stands in a slot occupies the same volume, and the path's occlusion holds for both sets.

| Slot | Height | Substitute (committed, `hash`) | Owner's landmark (overlay, `overlay` hash) |
|---|---|---|---|
| skyscraper | 270 m | Lantern `ffc0afdd1bc803ce` | skyscraper.glb `733773f40d248614` (1.94 M triangles) |
| office tower | 150 m | Corset `ee4c1c8be38b0e6d` | office-tower.glb `7a97e8a6628de329` (1.84 M) |
| cathedral | 95 m | FlightHelmet `84dd530fb67e4a9d` | baroque-cathedral.glb `b12f62f4bf62abf6` (1.90 M) |
| mosque | 55 m | Suzanne `74fef9db20b66d96` | mosque.glb `732623efe77bde02` (1.87 M) |
| airplane wreck | 48 m long | BoomBox `278559b810e63c72` | airplane-wreck.glb `fde41005d2c2fa04` (2.00 M) |
| palm (64 instances) | 12 m | SciFiHelmet `8fa4d013a805f5b5` | desert-palm-tree.glb `142d0a5d0c146395` (243 k) |

The hashes are `assets::source_mesh_hash` of the files. The load hashes every file it reads and refuses one whose bytes differ from what the scene names, so a number measured on this scene names exactly what it measured; the summary of a run carries the scene file's hash, the path file's hash, every mesh's hash and where it came from, and one `identity` over all of them. **The landmarks are the owner's paid-plan outputs and are never committed**: the scene names them only by content hash, and the local manifest (`engine.scene.Overlay`, at `D:/workspace/game_engine_local/scenes/desert-overlook/landmarks.json` on the development machine) says where the files with those bytes are. Without it the scene runs on the substitutes alone. The props along the approach and at the oasis are the same seven Khronos samples at human scale, in both sets.

## The layout

x is east, z is south, y is up, metres. The camera starts 900 m south of the origin and flies north.

- **Ridge A**, 70–75 m, runs east–west at z ≈ 330–460 with a **saddle** 36 m high at x = 0 — the only way through, and the path's.
- **Ridge B**, 55 m, runs from (−700, −260) to (520, −340), 350 m north of the oasis.
- **The oasis**: a basin 200 m across and 9 m deep centred on (0, 60), 40 palms within 95 m of its centre and 24 more at 110–175 m.
- **Landmarks**: the airplane wreck at (−260, −60), 90–300 m from the path in the basin; the mosque at (380, −150), in front of ridge B; the cathedral at (−520, −720) and the office tower at (900, −1,250), behind ridge B; the skyscraper at (−1,350, −1,950), 2.6 km from the saddle.
- **Path**: low through the dunes past the camp (0–6 s); up the saddle, whose face fills the frame just before the crest (6–16 s); rising 60 m over the saddle and panning from the skyscraper to the office tower (16–24 s); down into the basin (24–32 s); over the palms to the wreck (32–40 s).

## The frames to read the numbers against

States measured with `--census-pixels` at 640×360, occlusion culling on, and against the same frame with it off: *hidden* is inside the frustum (the frame with occlusion culling off draws pairs of it) and covering no pixel; *out of frame* is outside the frustum; otherwise the pixel count. The pairs are the frame's visible (instance, cluster) pairs with occlusion culling on / off, which is what occlusion culling saved. Overlay first; the substitute set where it differs.

| Frame | Marker | Pairs on / off (overlay) | What the frame holds |
|---|---|---|---|
| 0 | `behind-ridge-a` | 631 / 1,738 | All five landmarks and the palms inside the frustum and **fully occluded** by ridge A (0 px each). Occlusion culling removes 1,107 pairs, 64% of the frame. |
| 30 | `camp` | 628 / 1,749 | The camp's props 10–40 m ahead (the corset 343 px); every landmark still hidden. |
| 852 | `saddle-face` | 21 / 1,730 | The saddle's face fills the frame (every pixel is terrain); occlusion culling leaves 21 pairs of 1,730. |
| 859 | `disocclusion` | 1,750 / 1,750 | The first sky over the crest: the Hi-Z is built from the previous frame's 7 pairs, and **occlusion culling culls nothing** — the frame two-pass occlusion culling is weakest at. The first palms show (58 px). |
| 942 | `crest` | 1,181 / 1,612 | The camera crosses the saddle: skyscraper (247 px) and office tower (203 px) **come into view**; the cathedral is 10 px over ridge B (132 of its 133 pairs survive culling, so its bounds are in view); the mosque 1 px; the wreck still hidden. *Substitutes*: the wreck stand-in, a 48 m box, is already in view (111 px) and the mosque stand-in hidden. |
| 1080 | `west` | 1,357 / 1,527 | Looking west: skyscraper (189 px) and wreck (91 px) in view; the cathedral **partly occluded** by ridge B — 40 px, and 51 of its 133 pairs survive culling. Office tower and mosque out of frame. |
| 1260 | `overlook` | 1,662 / 1,736 | 60 m over the saddle: **all five landmarks in view** (mosque 1,719 px, office tower 565, skyscraper 403, wreck 183, cathedral 177 with its base still behind ridge B). Occlusion culling removes 4%. |
| 1440 | `east` | 1,143 / 1,172 | Office tower and mosque in view; the three west landmarks out of frame. |
| 1720 | `sinking` | 4,625 / 5,116 | Descending into the basin: the cathedral in frame and **fully occluded** by ridge B, the office tower too; the skyscraper partly (62 px). *Substitutes*: the cathedral stand-in keeps 9 px, and the office tower's stand-in is out of frame (its bounds are not the landmark's). |
| 1860 | `grove` | 5,906 / 6,655 | Over the palm grove at its densest: 48,644 px and 5,221 visible pairs of palm; skyscraper (22 px) and office tower (5 px) in slivers over ridge B. *Substitutes*: the office tower's stand-in is hidden. |
| 1980 | `oasis` | 4,150 / 4,850 | Skyscraper and cathedral in frame and **fully occluded** by ridge B; the wreck in view. *Substitutes*: the skyscraper stand-in, a lantern, keeps 3 px of its lamp over the ridge. |
| 2400 | `wreck` | 804 / 1,666 | The airplane wreck about 90 m away (2,535 px); skyscraper and cathedral in frame and hidden behind ridge B. |

The pixel counts scale with the resolution and the states hold at every one measured; a state within a pixel or two of changing (the mosque at 942, a sliver over ridge B) may read differently at another resolution or on another GPU's rasterization. The frames between the markers are in any run's `.jsonl`.

## Changing it

A change to the scene, the path or the terrain generator changes the hashes a run reports, which is the point: a number names what it measured. **The pictures changed on 2026-09-24**, and for the better: until then every cluster of the terrain and of the FlightHelmet was drawn with the material of whichever cluster had sat at its index before the scene's merge reordered them ([renderer](../../../docs/subsystems/renderer.md#invariants)), so any capture of this scene taken before then shows rock patches on the dunes, sand on the ridges and metal on the helmet's leather; the timings and the visible pairs were unaffected. Change the markers and this table in the same commit as the file they describe, re-reading the states with `--census-pixels` against a `--no-occlusion` run — `tools/flythrough.ps1 -Markers` does both.
