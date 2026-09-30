# desert-endless

The erg of [`desert-erg`](../desert-erg/README.md) — its five bands, its wind, its twelve storms a year and its sand close up, three years into the world — **with no edge**: the ground is drawn from the world's tiles streamed round the camera instead of from one grid ([renderer](../../../docs/subsystems/renderer.md#the-ground-from-the-worlds-tiles), [ADR-0049](../../../docs/adr/0049-the-ground-is-drawn-from-the-worlds-tiles.md), proposed), so the desert goes on in every direction for as far as anyone flies, and what is resident is what the rings hold, not how far the camera has come.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-endless/scene.json --interactive
```

## What the scene says

- **`world`**, with `"ground": true`: 32 m tiles in four rings — 50 cm a sample within a tile and a half of the camera (64 cells a tile), a metre out to 8 tiles (256 m), 2 m out to 24 (768 m) and 4 m out to 64 tiles, 2 km. A ring's `ground_cells` must cut a tile into whole millimetres and divide the ring inside it's. The world's ring hands the renderer the tiles it holds and the renderer rebuilds only the tiles whose key changed — its coordinates, its ring, or a neighbour's ring — on a worker, and swaps them in in one frame.
- **`terrain`**, the erg's own block with `size` 3: the grid is kept for what it carries (the material, the texture frame) and draws nothing, so a scene of tiles gives it a handful of samples. `extent` no longer bounds anything.
- **`camera_path`**: two minutes at 100 m/s, 90 m over the sand and looking 700 m ahead, 6 km east across the erg and out of its old 6.1 km square at x = 3,072, then 6 km north-east beyond it.

`--time-rate 86400` sets the sand going at a day a second, every level at one surface time and every tile of every level with it ([renderer](../../../docs/subsystems/renderer.md#the-ground-from-the-worlds-tiles), "Time-lapse across tiles"); `--walk` puts you on it, on a collision made from the same tiles ([scene_collision](../../../docs/subsystems/scene_collision.md)).

## What to look at

| Frame | Marker | What should be there |
|---|---|---|
| 0 | `start` | inside the erg, 2 km west of its centre: the mega-draa ahead |
| 3060 | `edge` | over x = 3,072, where the erg's grid used to end: nothing changes |
| 3600 | `turn` | turning north-east a kilometre past the old edge |
| 7200 | `far` | 12 km flown, 7 km past the old edge: the same desert, the same rings |

Nothing should show where two tiles meet — no crack, no line of light or shade, no step — whether the two are of one ring or of two, flying or with the sand moving; the tests that hold it are in [renderer](../../../docs/subsystems/renderer.md#the-ground-from-the-worlds-tiles). What a ring's handover does show is the LOD's own: a tile going from 1 m to 2 m changes its triangles at the distance the rings put that change.

## Measured

Along its 12 km path at 100 m/s, offscreen on the RTX 5090 (2026-09-30, a shared machine): 1.45 ms of GPU a frame at the median at 1920×1080 and 3.25 at 11,520×2160 across three views; about 1,960 tiles built and as many let go a second of flight, 33 a frame; 14,475–14,729 tiles held in flight; the device's 612.8 MB of terrain slots and arenas fixed at load, and the process's memory flat for the whole 12 km. Building the tiles is the cost — 38 ms of host wall a frame at the median offscreen, where a frame waits for its rebuild — and a windowed flight at this speed is not measured yet. [The experiment](../../../docs/experiments/world-tiles-flight-2026-09-30.md) has the tables, the hitches and what the flight changed.
