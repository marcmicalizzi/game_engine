# A streamed world's tile work off the frame's thread, and no trading at the pair cap (T131, W118)

2026-11-13. What the owner's walk over the forest showed, the engine scenes it was reproduced on, and the pair of binaries that measures the change. The rules are on the [world](../subsystems/world.md) page: [Builds off the frame's thread](../subsystems/world.md#builds-off-the-frames-thread), [No trading at the pair cap](../subsystems/world.md#no-trading-at-the-pair-cap) and the mesh room's yield ([A tile's meshes](../subsystems/world.md#a-tiles-meshes-residency-by-reference)).

## What the walk's frame records say

The owner's walk of 2026-11-13 (the forest proof of concept's lab run `20261113-121148-576`, 3840×2160, `--raster rt --shadows rt`, `--walk --interactive`; its `frames.jsonl` read as text, 49,698 frames over 13.5 minutes): 337 s of it at a walking or running speed (2 to 6 m/s over a second), 161 frames over 50 ms and 38 over 100 ms in those seconds. Beyond the summary the owner was given:

- **The spikes come in trains, and a train is a backlog draining.** At 242 to 251 s, running at 2.2 to 5 m/s, every frame for nine seconds is 50 to 165 ms with `world.tiles_ms` 18 to 40 ms and `tiles_built` 4 — the build budget's four a commit — while `pending` falls from 753 to 11. The backlog was left by a move of 1.1 km at about 300 m/s at 237 to 240 s (a game's jump or a flight), which made 955 tiles due; the run that followed paid for it at four tiles a frame on the frame's thread.
- **Built tiles were thrown straight to the pair budget.** Through the same drain `withheld` climbs from 230 to 850: tiles were built on the frame's thread and then not drawn, and the budget's ranking let others go each commit as the walker moved.
- **Many spike frames do no tile work at all** (frame 19632, 58 ms, nothing built; the session's other parts), and `terrain.rebuild_ms` of 700 ms on a frame is the terrain worker's, not the frame's.
- The scene's tables were written whole (`cpu.tables` 14 to 26 ms, `table_pairs` 15 M) on the frames a compaction ran.

## The engine scenes

Three scenes in the agent's scratch, each a streamed world of 64 m tiles in rings of 1.5, 4 and 8 tiles round a camera path at eye height along the foot of a ridge, 360 m at 4.5 m/s over 80 s, flown in a window in real time (`--windowed --camera-path`, 1600×900, one derived-data root warmed by a first run):

- **cap-walk**: G191's massif foot (a deposits layer at the forest's density, `far_rings` 3) on a 2.5 km ground carpeted with the litter generator's six meshes at 2 m cells and a background chance of 0.7: 16.1 to 16.7 M pairs, at the 2^24 cap, up to 43 tiles withheld.
- **cap-walk-dense**: the same at 1 m cells (four times the candidates a tile), up to 171 withheld.
- **deposits**: G191's scene alone (deposits only, `far_rings` 3, the default 256 MiB mesh room), the one W118 was found on.

No generator here is the forest's; every tile's work is the deposits' and the litter's `tile` calls, so a tile builds in a few milliseconds (a forest tile's 15 to 60 ms is many more entries), and these scenes show what the rule changes rather than the forest's sizes.

## The pair

`msvc-release`, the base `61ccce67` (`engine-view.exe` SHA-256 `FAAFCFEC…`) against the branch (`C3EDAC8B…`), on a shared machine: other processes held 22 to 100% of the CPU across the runs (each run's state before and after is in its `machine.txt`), and **the desktop session was locked**, which put a 52 to 65 ms frame every 1.5 s into every run with 1 ms of CPU and 2 ms of GPU in it — the window's presentation, not the engine — so frames over 50 ms are the same count in every run (105 to 107) and say nothing here. What does: the frames' own world turn (`world.tiles_ms + admit_ms + commit_ms + handover_ms`).

| | cap-walk base | cap-walk branch | dense base | dense branch |
|---|---:|---:|---:|---:|
| The first frame (the fill round the start) | 1,946 ms | 55 ms | 1,883 ms | 55 ms |
| Tile work on the frame's thread a frame, p99 | 1.59 ms | 0.50 ms | 5.32 ms | 0.86 ms |
| Frames whose own tile builds passed 10 ms | 1 | 0 | 1 | 0 |
| A commit's hand-over of a job's tiles, the most | — | 1.36 ms | — | 2.79 ms |
| The generators on the worker, over the walk | — | 1,414 ms | — | 3,147 ms |
| Tiles let go by the pair budget (`refused`) | 223 | 68 | 297 | 39 |
| Of those drawn again while still held (trades) | not counted | 0 | not counted | 0 |
| Tight compactions (a whole write of 105,000 slots, 15 to 26 ms, then 6 to 9 ms of tables) | 5 | 4 | 5 | 6 |
| A relayout (the device idle, the stride grown) | in the first frame | 51 ms at 11 s | in the first frame | 53 ms at 4 s |

| | deposits base | deposits branch |
|---|---:|---:|
| Deposit instances drawn, every 300 updates | 88 80 48 8 0 0 … 0 | 0 96 88 88 88 96 88 … 88 |
| Updates after the first fill that drew more than the last | 0 | 15 |
| Tiles that gave their meshes up to a nearer one | — | 79 |
| The room at the end | 103 meshes, 250.5 MiB, 32 tiles withheld | 95 meshes, 228.5 MiB, 25 withheld |

**What the branch leaves on the frame's thread** is the renderer's half of an arrival: the tight compactions at the cap (the layout's holes run out of slack within 4% of the ceiling and every slot is written again) and one relayout when a fill that arrives over frames grows the stride after the first frame rather than inside it. Neither is the world's; both are the renderer's tile layout ([renderer](../subsystems/renderer.md#instances-that-come-and-go)).

**For the forest** the command is the owner's (`run.json` of the walk) with the branch's binaries; the numbers that decide are the frames whose `world.tiles_ms` passes 10 ms (226 frames over 50 ms in two minutes on the base), the trades (`returned` in the `--world-log` summary), and the first frame.
