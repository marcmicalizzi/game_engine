# desert-endless

The erg of [`desert-erg`](../desert-erg/README.md) — its five bands, its wind, its twelve storms a year and its sand close up, three years into the world — **with no edge**: the ground is drawn from the world's tiles streamed round the camera instead of from one grid ([renderer](../../../docs/subsystems/renderer.md#the-ground-from-the-worlds-tiles), [ADR-0050](../../../docs/adr/0050-the-ground-is-drawn-from-the-worlds-tiles.md), proposed), so the desert goes on in every direction for as far as anyone flies, and what is resident is what the rings hold, not how far the camera has come.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-endless/scene.json --interactive --time-of-day 10
```

The scene's time is the erg's, midnight three years in; `--time-of-day 10` flies it at ten in the morning, and without it the desert is under the full moon.

## What the scene says

- **`world`**, with `"ground": true`: 32 m tiles in four rings — 50 cm a sample within a tile and a half of the camera (64 cells a tile), a metre out to 8 tiles (256 m), 2 m out to 24 (768 m) and 4 m out to 64 tiles, 2 km. A ring's `ground_cells` must cut a tile into whole millimetres and divide the ring inside it's. The world's ring hands the renderer the tiles it holds and the renderer rebuilds only the tiles whose key changed — its coordinates, its ring, or a neighbour's ring — on a worker, and swaps them in in one frame.
- **`terrain`**, the erg's own block with `size` 3: the grid is kept for what it carries (the material, the texture frame) and draws nothing, so a scene of tiles gives it a handful of samples. `extent` no longer bounds anything. **Its `detail` is the erg's, field for field**, and the renderer's rings test holds the two files equal. Until 2026-10-04 it was a copy taken before the erg's third pass, so this desert drew the second pass's streaks on every slip face: short light and dark dashes edge to edge, which the owner flew into ([the fifth pass](../../../docs/experiments/sand-fifth-pass-2026-10-04.md)). Change the erg's block and this one together.
- **`sky`**, the erg's ([desert-erg](../desert-erg/README.md#the-sky)): Earth's at 31.1° N in early April, the moon two days short of full.
- **`camera_path`**: two minutes at 100 m/s, 90 m over the sand and looking 700 m ahead, 6 km east across the erg and out of its old 6.1 km square at x = 3,072, then 6 km north-east beyond it.

`--time-rate 86400` sets the sand going at a day a second, every level at one surface time and every tile of every level with it ([renderer](../../../docs/subsystems/renderer.md#the-ground-from-the-worlds-tiles), "Time-lapse across tiles"); `--walk` puts you on it, on a collision made from the same tiles ([scene_collision](../../../docs/subsystems/scene_collision.md)).

## What to look at

| Frame | Marker | What should be there |
|---|---|---|
| 0 | `start` | inside the erg, 2 km west of its centre: the mega-draa ahead |
| 3060 | `edge` | over x = 3,072, where the erg's grid used to end: nothing changes |
| 3600 | `turn` | turning north-east a kilometre past the old edge |
| 7200 | `far` | 12 km flown, 7 km past the old edge: the same desert, the same rings |

**From height** (2026-10-03, [renderer](../../../docs/subsystems/renderer.md#ground-to-the-horizon)): past the world's outermost ring the renderer draws six far levels of its own — 16 m to 512 m apart, each twice the reach of the one inside it — so the ground goes on to 78 km or more in every direction, with heights filtered to each level's spacing (the waves, crests and barchans stand in as their mean sand out there, and nothing shimmers as they move). Climb with `--interactive` to 100 m, 350 m and 2 km and look at the horizon. **What should be seen**: the ground running to the horizon from every height, meeting the sky in the haze; at 2 km a horizon a little brighter than the sky above it (the air table that hazes the ground ends at 32 km). **What should not**: a band of flat colour between the ground's last row and the horizon — the sky's planet, which is what five levels left from about 500 m up (`--terrain-far 5` shows it at 2 km; `--terrain-far 0` the old 2 km edge); a crack or a line where two tiles meet. **What still shows**: with traced shadows, dotted rows of dark one-to-three-pixel specks along far tiles' borders on distant ridges (a shadow ray's offset in the resolve, not the ground), and at a low sun the far shadows' stepped edges; with `--shadows csm`, no cast shadows past the cascades, a few kilometres out. [The experiment](../../../docs/experiments/far-ground-2026-10-03.md) has the captures and what is measured.

**A slip face close up** (2026-10-04; the owner's markers in two flights, at (−1783, 82, 419) looking 33° down at yaw −109° and at (−1759, 96, 439) looking 31° down at yaw −119°, on the same 33° face in its own shade at `--time-of-day 10`): smooth sand with a few long, faint grainflow tongues running down the fall line, each a soft darker scour under the brink opening into a lighter lobe — about two features per 100 m² where the face is in shade. **What should not be there**: a field of short light and dark dashes all pointing the same way (the second pass's streaks, which this scene drew until then), or, from a few hundred metres, one-pixel vertical lines (the fourth pass's tongues drawn under pixels they did not fit). What the sand close up should look like everywhere else is [desert-erg's](../desert-erg/README.md#the-sand-close-up).

Nothing should show where two tiles meet — no crack, no line of light or shade, no step — whether the two are of one ring or of two, flying or with the sand moving; the tests that hold it are in [renderer](../../../docs/subsystems/renderer.md#the-ground-from-the-worlds-tiles). What a ring's handover does show is the LOD's own: a tile going from 1 m to 2 m changes its triangles at the distance the rings put that change.

## Measured

Along its 12 km path at 100 m/s, offscreen on the RTX 5090 (2026-09-30, a shared machine, before the scene named a sky — the sky's own pass is measured in [its experiment](../../../docs/experiments/sky-2026-09-30.md)): 1.45 ms of GPU a frame at the median at 1920×1080 and 3.25 at 11,520×2160 across three views; about 1,960 tiles built and as many let go a second of flight, 33 a frame; 14,475–14,729 tiles held in flight; the device's 612.8 MB of terrain slots and arenas fixed at load, and the process's memory flat for the whole 12 km. Building the tiles is the cost — 38 ms of host wall a frame at the median offscreen, where a frame waits for its rebuild — and [the experiment](../../../docs/experiments/world-tiles-flight-2026-09-30.md) has the tables, the hitches and what the flight changed. **In a window** (2026-10-03, `--windowed --camera-path`, the path at its own speed in real time; [the measurement](../../../docs/experiments/world-tiles-window-2026-10-03.md)): no frame waits for a tile, the frame thread's share of them is 0.02 ms at the median and under 8 ms at worst, and the finest ring is 22 m (11 frames) behind the camera at the median at 100 m/s, 50 m at the most — the 1 m tiles drawn under it meanwhile — and 2.5 m at 30 m/s; offscreen, rebuilds that cost what changed halved the frame to 19 ms.
