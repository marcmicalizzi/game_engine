# A fast long flight over the world's tiles (2026-09-30)

- **Question.** With the ground drawn from the world's tiles ([renderer](../subsystems/renderer.md#the-ground-from-the-worlds-tiles), [ADR-0049](../adr/0049-the-ground-is-drawn-from-the-worlds-tiles.md), proposed), what does a fast, long flight cost — GPU and CPU milliseconds a frame, tiles made and let go a second, the largest hitch — and is what is resident bounded by the rings rather than by the distance flown? At 1920×1080 and at the owner's 11520×2160 across three views.
- **Date:** 2026-09-30. **Machine:** the owner's desktop, RTX 5090, `msvc-release`, `engine-view` offscreen. **Build:** the world-tiles branch with the three changes below.
- **Machine state:** shared, WARNING raised on every run. At 1080p other processes used 22–24% of the CPU (other agents compiling) and the GPU was 5–6% busy besides this run (the GPU lock held; `--wait-quiet 60` timed out and measured anyway); at 11520×2160, 7–12% and 4–5% (`--wait-quiet` found the machine quiet at the start, the end sample did not); the one run marked below had the GPU 36% busy at its end sample. Every number here is an upper bound; the ratios within a run stand. Measured on this branch before it was rebased onto the sky's pass (`09b295c`), so the sky's own cost is not in these frames.
- **Decision:** the ranges pass of the ray tracing chain became a workgroup ([gfx](../subsystems/gfx.md)), a tile slot rounds to four clusters rather than sixteen, and engine-view's drawn ground takes no world budget ([world](../subsystems/world.md#the-consumers)) — all three found by this flight.

## Setup

- **The scene**: [`content/test-scenes/desert-endless`](../../content/test-scenes/desert-endless/README.md) — the erg's five bands, its wind and storms and its sand close up, with a world block asking for ground tiles: 32 m tiles at 50 cm within 1.5 tiles of the camera, 1 m to 8 tiles, 2 m to 24, 4 m to 64 (2 km); the grid 3 samples and undrawn. Still sand (no `--time-rate`), shadows at the card's default (traced).
- **The flight**: the scene's own camera path, 120 s at 60 fps and 100 m/s, 90 m over the sand looking 700 m ahead: 6 km east across the erg and out of its old 6.1 km square at x = 3,072 (frame 3,043), then 6 km north-east beyond it — 12 km, 7,201 frames, every one timed (`--repeat 1 --warmup 30`).
- **Offscreen, a re-centre completes in the frame that asks for it**: the world's update, the tile set's rebuild on its worker, the uploads, the pairs and the swap are all inside the frame's wall time, so a frame's wall is the host's whole cost of that frame's tiles; the renderer's own CPU milliseconds (`cpu_ms`) are not. In a window the rebuild runs behind the frames instead.
- **Memory**: the process's private bytes and working set sampled every second by the script that started it; the device's terrain slots and arenas from the summary (`time_lapse.rings.device_bytes`), the renderer's own `gpu_memory_used_mib`.

```powershell
tools/gpu-lock.ps1 run -Purpose "..." -Exec "& '<scratch>\measure.ps1' -Label full-1080 -Path content\test-scenes\desert-endless\camera-path.json -Extra @('--width','1920','--height','1080')"
# measure.ps1: build\msvc-release\bin\engine-view --offscreen --scene content\test-scenes\desert-endless\scene.json
#   --camera-path <path> --repeat 1 --warmup 30 --wait-quiet 60 --benchmark bench-<label>.jsonl <extra>,
#   sampling (Get-Process).PrivateMemorySize64 and WorkingSet64 once a second.
```

## Results

| | 1920×1080 | 11520×2160, three views |
|---|---|---|
| GPU ms a frame, median / p95 / p99 / max | 1.45 / 1.90 / 2.38 / 5.93 | 3.25 / 4.14 / 4.73 / 6.04 |
| of which the ray tracing chain, median / p99 | 0.72 / 1.15 | 0.80 / 1.24 |
| renderer CPU ms, median / p95 / p99 / max | 2.78 / 5.38 / 7.70 / 82.0 | 2.77 / 5.53 / 8.37 / 457 |
| frame wall ms (host and renderer), median / p95 / p99 / max | 38.1 / 89.1 / 113 / 302 | 38.5 / 78.5 / 110 / 1,428 |
| frames over 100 ms | 169 of 7,201 | 120 of 7,201 |
| tiles built / let go, a second of flight | 1,964 / 1,951 | 1,964 / 1,951 (the same tiles) |
| rebuilds swapped (one a frame) / dropped | 7,198 / 0 | 7,198 / 0 |
| tiles held: first fill / at most / at the end | 12,844 / 14,729 / 14,475 | 12,844 / 14,729 / 14,475 |
| terrain slots and arenas on the device | 612.8 MB, the same throughout | 612.8 MB, the same throughout |
| process private bytes / working set, after load | 2.58 GB / 460 MB, flat for 12 km | 3.12 GB / 465 MB, flat for 12 km |
| the world ring's update, µs median / p99 / max | 573 / 1,430 / 33,502 | 585 / 1,818 / 138,832 |

**What is resident is the rings'.** The device's terrain slots and arenas are reserved at load and never grow — 612.8 MB of them for four levels, most of it the 4 m level's 34,700 slots. The process's private bytes rise to 2.98 GB while the first fill is built and uploaded, settle at 2.58 GB within two minutes of wall time and stay there, within 25 MB, for the remaining 10 km; the working set holds at 455–495 MB. The tiles held follow the rings: 12,844 in the first fill (a camera standing still holds only the tiles within each radius), 14,475–14,729 in flight, because a moving ring also holds the band its tiles leave by (they go at the radius times 1.15). Nothing grows with distance: the per-level chunk lists, the world's held tiles and the renderer's lists all stay at their flight size.

**The ground costs the GPU little; building it costs the CPU a lot.** At 1080p the frame's GPU time is 1.45 ms at the median, of which 0.72 ms is the ray tracing chain for the traced shadows and 0.17 the resolve; the visible cut is 3,624 pairs at the median, because the tiles' LODs are coarse at a distance. The host's side is a frame's tiles: 32.7 tiles built a frame on the job pool (most of them the outer ring's 8-cell tiles entering the ring, the rest a ring's boundary and the neighbours whose key names the moved tile's level), uploaded, and swapped — 38 ms of wall a frame at the median offscreen, 113 at the 99th percentile, 302 at the most. In a window that work is behind the frames, not in them; the offscreen wall is what it costs the machine, and at 100 m/s it is more than a 60 Hz frame. A window would therefore draw the rings some frames behind the camera at this speed, not stall; that was not measured here (below).

**At 11520×2160 the tiles are the same work and the pixels are the GPU's.** The two runs built, kept and let go exactly the same tiles — 235,731 built and 234,100 let go, the same held counts — since what the rings hold depends on the camera and not on the views. The GPU's frame is 3.25 ms at the median, 1.58 of it the resolve over 25 million pixels and 0.80 the ray tracing chain over the three views' union (6,077 visible pairs at the median). The largest hitches are one stretch of the path, frames 3,297–3,446 just past the old edge, where the renderer's own CPU time rose to 230–457 ms on five frames and one rebuild took 1.4 s of worker wall where the rest took under 160 ms — 1,428 ms of frame wall at frame 3,334; the same frames at 1080p, the same tiles, did not stall (that run's largest was 302 ms, at frame 5,882), so it reads as the shared machine rather than the path, and one run cannot say more.

## Before the three changes

| run | GPU ms median | ray tracing chain ms | tiles held at the end |
|---|---|---|---|
| first 60 s, the ranges pass on one thread (GPU 36% busy at its end sample) | 8.11 | 7.50 | 9,652 |
| whole flight, the ranges pass a workgroup, the world budgeted | 1.36 | 0.67 | 8,971 |
| whole flight, the drawn ground unbudgeted (the table above) | 1.45 | 0.72 | 14,475 |

- **The ray tracing chain's ranges pass was one thread over every instance.** A tile slot is an instance, and the rings' slots make 40,000 of them; `ranges_main` walked them four times on one GPU thread, and the chain took 7.50 ms. As a workgroup that scans runs of the instances the chain takes 0.67–0.72 ms ([gfx](../subsystems/gfx.md), `ClusterRecordParams::capacity`).
- **The world's budget of eight promotions an update fell behind.** The rings want about ten a frame at 100 m/s — six tiles entering the outer ring, four moving inwards — and the ring lost the difference: after 12 km it held 8,971 tiles against the 14,475 the unbudgeted ring holds, the horizon ahead thinning. The drawn ground's events cost a hash map's writes; engine-view now lifts the budget when the world streams nothing else ([world](../subsystems/world.md#the-consumers)).
- **A slot rounded to sixteen clusters** made the 4 m level's 34,700 slots 555,000 pairs, by the capacity rule, for tiles of one or two clusters; rounded to four, 139,000. The flight's GPU time does not show it at these numbers — the cull is 0.02 ms — but the device's slot tables shrink with it.

## What surprised me

- The first smoke run drew sky: it spread the 12 km path over 41 frames, 300 m a frame, and every tile was withheld from the third frame — the windows follow the camera and the budgeted ring could not bring the tiles to them. It is the correct behaviour of a ring that falls behind, now written down ([renderer](../subsystems/renderer.md#the-ground-from-the-worlds-tiles), "Which tiles").
- The tiles built a frame (33) are three times the tiles that cross the outer ring's front: a tile's key names its neighbours' levels, so every tile that moves a ring rebuilds up to eight neighbours whose borders change.
- The renderer's own CPU time spikes to 46–82 ms on five frames, all after frame 4,500, with nothing else in its frame out of the ordinary; not explained here.

## What it decides

That the tiles can carry an endless desert with the device's memory fixed at load and the host's flat, and that the renderer's GPU cost of the ground is small. It does not decide that 100 m/s is sustainable in a window: the offscreen wall says the tiles cost more than a frame at that speed, and a windowed flight — how far behind the rings fall, and whether the swap's frame hitches — is the next measurement.

## Caveats

One machine, a shared one; still sand (a time-lapse adds the fields' evaluation on the field worker, and a frozen level's pair carried over at every window move); traced shadows only (the maps would replace the 0.72 ms chain with a cascade pass); one path, flown once.
