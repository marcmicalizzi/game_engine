# Walking after rapid travel (2026-10-24, roadmap W70)

**The question.** The owner flies to the forest POC's mountain summit (F, W, mouse), switches to walking (F), and mouse-look stays smooth while **W A S D** brings frames of 400–700 ms for a minute or two, then it clears; elsewhere too, and sometimes it clears by waiting. The POC's own replay on fa343854 (`reports/quality-v8/flight-walk`, a window, `--present-max-hz 120`) recorded a worst frame of **4,417.70 ms**, of which `SceneCollision::activate` was **4,388.34 ms**, thirteen more frames over 100 ms (3,677.74 ms at 19.5 s), GPU time 7 ms in those frames; the same walk started cold at the summit: worst 48.81 ms, activation 0.2008 ms. What on the frame's thread waits, and on what?

**The hypothesis it came with** (the POC's handoff, and a read-only review of 4b31ac77): commit 4b31ac77 (2026-10-23, W67) replaced the tile set reader's lock-free resident tiles with a bounded FIFO cache behind one `std::mutex`, and `TileSet::heights` held it over a whole window's evaluation, tile reads from disk included. The terrain's rebuild evaluates its windows on the job system, so after travel every worker queues on that lock — and the walker's collision tiles and its drawn-ground queries ask the same reader.

## How it was measured

- **The scene**: the POC's v7 staged forest (`flight-walk/scene.json`: 2.048 × 3.584 km of authored terrain at 0.5 m, `--terrain-tiles --terrain-far 6`, 23,361 instances), read as data; its tile set and derived data copied to a scratch directory so nothing of the POC's was written. No `--game` module (the POC's binary is not run): the animals are absent, and the walk is engine-view's own.
- **The runs**: this worktree's `msvc-release` engine-view, offscreen at 1600 × 900, `--raster hw --shadows csm`, the POC's other flags, `--wait-quiet 60`, replaying input logs: the POC's flight log (W + Shift at 200 m/s from (80, 12, 130) to the summit, F at tick 2313, W held 14 s) and three made from it — **(a)** F, then mouse-look only for 14 s; **(b)** F and W at once (the POC's log); **(c)** F, 30 s standing with the terrain's work recorded, then the same W; **cold**: the POC's summit log, started walking at the summit. And a second place: **(d)** a flight from the summit 2.3 km down to the elder grove (-556.7, 3.0, -201.3), F and W, with its own cold start there.
- **`--terrain-async`** (new, [apps](../subsystems/apps.md#walking)): an offscreen run normally completes each terrain rebuild inside the frame that asks for it, so its walker can never meet a rebuild in flight; with the flag the rebuilds run behind the frames as a window's do. Case (b) was run both ways.
- **The records' new `walk` object** ([apps](../subsystems/apps.md#walking), "The frame record's walk") splits every frame's ticks: activations by stage (ground heights, placements, bodies), the walker's ground queries, and the tile set reader's lock — the frame thread's own waits and holds, and every thread's — beside the terrain's re-centre stage. A record's `frame_ms` is start to start, so it is read beside the previous record's `walk`.

## Before (fa343854's reader, instrumented)

Frames from the switch to walking on (the cold runs from their start), `msvc-release`, i9-10980XE, RTX 5090. Machine state as the harness recorded it at start / end of each run: others' CPU 6–22% (one cold run ended at 99%, another agent's build), GPU 0–33%; upper bounds, though no case here is near its bound.

| Case | Worst frame | Frames > 100 ms | > 33 ms | Activation max | Frame thread's lock wait (most in one wait / sum) | Its lock hold, sum | All threads' lock wait, sum |
|---|---:|---:|---:|---:|---:|---:|---:|
| (a) look only, async | 57.1 ms | 0 | 1 | 43.0 ms | 7.8 / 47.4 ms | 0.47 ms | 3.5 s |
| **(b) F and W, async** | **649.0 ms** | 1 | 3 | **637.7 ms** | **488.0 / 814.7 ms** | 0.53 ms | **37.5 s** |
| (b) F and W, waited (offscreen default) | 44.9 ms | 0 | 3 | 1.0 ms | 0.0 / 0.01 ms | 0.51 ms | 0 |
| (c) 30 s standing, then W | 9.5 ms | 0 | 0 | 0.25 ms | 0.12 / 0.17 ms | 0.18 ms | 19.7 ms |
| cold at the summit | 19.8 ms | 0 | 0 | 0.21 ms | 0.31 / 0.52 ms | 0.16 ms | 23.6 ms |
| (d) to the grove, async | 38.6 ms | 0 | 1 | 6.4 ms | 29.5 / 39.9 ms | 0.69 ms | 2.2 s |
| cold at the grove | 17.9 ms | 0 | 0 | 1.0 ms | 0.04 / 0.06 ms | 0.22 ms | 0.3 ms |

**The 649 ms frame of (b)**, the first walking frame (t = 9.65 s, the terrain's re-centre pairing): its ticks took 638.2 ms; 8 tiles were activated (the drop's unlimited first fill) in 637.7 ms, of which **636.8 ms was the ground's heights** and 0.9 ms the bodies; the frame thread **waited 636.4 ms for the reader's lock and held it 0.4 ms**, while every thread together waited 26.1 s for it and held it 635 ms, reading 916 tiles from disk. The next two slow frames (87.5 and 83.3 ms) are each **one drawn-ground query** — the walker's two 2 × 2 windows of the renderer's tile source when its feet cross a cell, which only walking does — waiting 80.7 and 75.5 ms for the same lock.

**What that says.** The stall is the frame thread waiting for the reader's lock while the terrain's workers hold it in turn: its own work under the lock is under a millisecond, the wait is the workers' whole hold, and the workers wait on each other for tens of seconds. It is not the activation's physics (bodies 0.9 ms), not placements (none on the summit), not IO on the frame thread (0 of its own reads in the frame), and not the renderer (a waited run of the same path has no stall). It explains each of the owner's observations: **mouse-look** asks for no ground, so its frames are free; **W A S D** crosses cells and tiles, and every crossing asks the reader; the first walking frame activates a whole ring at once; and **waiting** lets the rebuild finish, after which the same walk is clean — (c) is as clean as the cold start. The offscreen magnitude (0.65 s) is below the window's (4.4 s) because the offscreen frames are not paced and fewer of the rebuild's jobs are queued when the walker asks; the mechanism and its attribution are the same.

## After the reader stopped holding its lock across work

The reader's lock now guards the cache's index alone: a request pins its tiles, evaluates from them unlocked, and a miss is read outside the lock with an in-flight marker ([tileset](../subsystems/tileset.md#the-readers-lock)). The same runs, same flags and logs:

| Case | Worst frame | Frames > 100 ms | > 33 ms | Activation max | Frame thread's lock wait (most / sum) | All threads' lock wait, sum | Machine (others' CPU start / end, GPU) |
|---|---:|---:|---:|---:|---:|---:|---|
| (a) look only, async | 54.6 ms | 0 | 15 | 1.1 ms | 0.000 / 0.00 ms | 0.0 ms | 100% / 100%, 0% |
| **(b) F and W, async** | **25.2 ms** | 0 | 0 | **1.03 ms** | **0.003 / 0.02 ms** | **7.1 ms** | 24% / 9%, 4–33% |
| (c) 30 s standing, then W | 106.7 ms | 1 | 31 | 0.19 ms | 0.001 / 0.01 ms | 6.8 ms | 7% / 100%, 0–1% |
| cold at the summit | 37.6 ms | 0 | 3 | 0.21 ms | 0.003 / 0.02 ms | 2.4 ms | 36% / 90%, 6–20% |
| (d) to the grove, async | 24.0 ms | 0 | 0 | 4.6 ms | 0.010 / 0.02 ms | 3.3 ms | 97% / 11%, 0–84% |
| cold at the grove | 44.3 ms | 0 | 1 | 0.94 ms | 0.001 / 0.02 ms | 0.0 ms | 10% / 7%, 0–6% |

The first walking frame's activation of (b) is 1.03 ms (0.4 ms of it ground heights, 0.8 ms bodies) where it was 637.7 ms; the frame thread's longest wait for the lock is 3 µs where it was 488 ms, and every thread's wait over the walk is 7 ms where it was 37 s — the rebuild's workers no longer serialize on each other either. **The slow frames left are not the walker's**: in (a), (c) and both cold runs every frame over 33 ms has ticks of 0.1–0.3 ms and no reader wait, and they fall in runs where another process held the CPU at 90–100% (an agent's build, at the start or end of the run); the harness's `machine_state` says so, and they are upper bounds of nothing this change touches. (d)'s 4.6 ms activation is a tile of the grove's placements (proxies and compound, 4.0 ms over the walk), which the next change leaves on the tick (below).

## After the ground was built ahead of the ring

The collision now builds a tile's ground heights on a worker of the walker's own a tile ahead of its ring, and the tick that activates the tile takes them, waits for that tile's build if it is running, or builds it if it has not started; bodies are made on the tick in the ring's order as before ([scene_collision](../subsystems/scene_collision.md#the-ground-built-ahead-of-the-ring)). The same runs:

| Case | Worst frame | Frames > 100 ms | > 33 ms | Activation max | Activations' ground heights, sum | Built ahead: ready / waited / on the tick | Frame thread's lock wait, most | Machine (others' CPU start / end, GPU) |
|---|---:|---:|---:|---:|---:|---|---:|---|
| (a) look only, async | 25.7 ms | 0 | 0 | 1.0 ms | 0.21 ms | 3 / 1 / 4 | 0.002 ms | 34% / 39%, 0–99% |
| **(b) F and W, async** | **24.3 ms** | 0 | 0 | 1.7 ms | **0.29 ms** | 6 / 0 / 3 | 0.001 ms | 38% / 44%, 5–9% |
| (b) F and W, waited | 61.5 ms | 0 | 4 | 0.86 ms | 0.19 ms | 5 / 0 / 4 | 0.001 ms | 30% / 72%, 0–34% |
| (c) 30 s standing, then W | 39.3 ms | 0 | 1 | 0.21 ms | 0.01 ms | 1 / 0 / 0 | 0.001 ms | 28% / 42%, 25–28% |
| cold at the summit | 14.7 ms | 0 | 0 | 0.16 ms | 0.00 ms | 1 / 0 / 0 | 0.000 ms | 26% / 28%, 0% |
| (d) to the grove, async | 25.5 ms | 0 | 0 | 7.8 ms | 0.07 ms | 7 / 0 / 1 | 0.001 ms | 56% / 62%, 19–30% |
| cold at the grove | 28.4 ms | 0 | 0 | 0.95 ms | 0.01 ms | 2 / 0 / 0 | 0.001 ms | 96% / 18%, 2–3% |

The activations' ground is a few tenths of a millisecond in every case (636.8 ms before the reader's fix, 0.4 after it); the activations on the tick at the switch to walking are the drop's first fill, which builds what the worker had not reached yet — its own cost, as before. What is left of an activation is its bodies (0.7–1.6 ms over a walk) and, at the grove, its placements (7.9 ms over the walk on a machine 56–62% busy), which are not built ahead (scene_collision.md says why). Every frame over 33 ms in these runs has ticks under 1.5 ms and no reader wait; they are the renderer's and the machine's, and every run was taken beside another agent's builds at 26–96% of the CPU, so the frame times are upper bounds.

**The replay holds.** The walker's state chain at the end of each walk is the same before and after both changes, async or waited: (b) `fb1e7776eea7a246` over 20.943 m, the grove `66d2d83db90d0523` over 15.057 m, the cold summit `fc50f6ba7c932b43` over 20.878 m, in every run of this page.

## After the placements were built ahead too (W71)

The build ahead now also asks the generators for the tile, cuts and builds the proxies' shapes and the tile's compound shape on the worker; the tick adopts the compound and makes the bodies in the ring's order ([scene_collision](../subsystems/scene_collision.md#the-placements-built-ahead)). A first run showed what was left: the walk's first fill is made on the tick F is pressed, so builds queued on that tick were mostly not started — at the grove three of seven, their placements on the tick — and the walker now also builds ahead round the camera while it flies (`view::Walker::look_ahead`). The same cases (b) and (d), the same flags, logs and scene, `msvc-release`, offscreen, `--terrain-async`; "before" is this change's base (d70d34bc, the gate's head), each binary run beside the other on the same machine; the activation's largest frame is split as the frame record splits it.

| Case | Worst frame | > 33 ms | Ticks, most in a frame | Activation, most in a frame | of it: ground / placements / bodies | Built ahead: ready / waited / on the tick | Machine (others' CPU start / end, GPU) |
|---|---:|---:|---:|---:|---|---|---|
| (b) F and W, before | 57.1 ms | 1 | 9.71 ms | 2.08 ms (8 tiles) | 0.30 / 0.00 / 1.75 ms | 6 / 0 / 3 | 6% / 15%, 2–5% |
| (b) placements ahead | 37.7 ms | 1 | 1.56 ms | 1.06 ms | 0.14 / 0.00 / 0.88 ms | 6 / 1 / 2 | 24% / 13%, 4–35% |
| **(b) and built while flying** | **24.0 ms** | 0 | **1.30 ms** | **0.90 ms** | 0.01 / 0.00 / 0.87 ms | **9 / 0 / 0** | 18% / 21%, 4–36% |
| (d) to the grove, before | 38.6 ms | 4 | 5.52 ms | 5.12 ms (7 tiles) | 0.87 / **3.34** / 0.89 ms | 7 / 0 / 1 | 20% / 17%, 79–93% |
| (d) before, again | 37.0 ms | 3 | 4.32 ms | 3.77 ms | 0.08 / **2.82** / 0.86 ms | 7 / 0 / 1 | 40% / 40%, 5–86% |
| (d) placements ahead | 171.9 ms | 174 | 39.5 ms | 11.4 ms | 0.23 / 5.56 / 1.20 ms | 5 / 0 / 3 | **91% / 100%**, 2–19% |
| **(d) and built while flying** | 60.2 ms | 13 | 5.81 ms | 2.68 ms | 0.02 / **0.00** / 2.64 ms | **8 / 0 / 0** | 22% / 35%, 3–83% |
| **(d) and built while flying, again** | 44.9 ms | 3 | **1.73 ms** | **1.11 ms** | 0.02 / **0.00** / 1.08 ms | **8 / 0 / 0** | 11% / 21%, 3–81% |

**What it says.** With the tiles built while flying and the placements built ahead, every activation after both flights took a finished build, and none of a tile's placements is paid on the tick: the grove's largest activation is its seven tiles' bodies, 1.1 ms where it was 3.8–5.1 ms (2.8–3.3 ms of it placements), and the frame's ticks at most 1.7 ms where they were 4.3–5.5. The worker spent 6.8 and 13.2 ms on placements over the two grove runs, the tiles the flight passed over included. The run of placements ahead without the flight's builds was taken while another process held 91–100% of the CPU, and its worst frame and its 11.4 ms activation are that machine's (the tick built three tiles itself, each on a starved core); it is kept for what it shows of the order, not its times. The second (d) "built while flying" run's bodies, 2.6 ms against 1.1 ms in its repeat, is the same kind of noise: the bodies are made the same way in both. Every frame over 33 ms left has ticks under 6 ms and no activation over 3 ms; they are the renderer's and the machine's.

**The replay still holds**: (b) `fb1e7776eea7a246` over 20.943 m and the grove `66d2d83db90d0523` over 15.057 m, in every run of this table — the chains of W70's runs above.

**What it costs**: the proxies the worker built for tiles the walker never took stay in the consumer's cache (one per mesh and scale, about 1.6 KB a tree's trunk at the grove): 16 proxies after the grove walk before, 238 with the tiles built under the flight, 504 after the flight to the summit (scene_collision.md, "Not yet").

## Four rings over the forest's authored tiles (W77)

**The failure reproduced**: the POC's full v8 scene streamed (W73's scratch copy, rings 1.5, 6 and 40 tiles of 32 m) with a fourth ring added (1.5, 6, 16, 40), `--terrain-tiles --terrain-far 6`, on the gate's head: `engine-view: terrain-tiles: world tiles: a tile's heights or its cluster LOD could not be made`, after 134 s. **The cause**: the first far level is `far_ratio` (4) times the outermost ring's spacing; the fourth ring at the default 8 cells is 4 m, so the six far levels ask 16 to 512 m, and the scene's set was built for the default `ring_spacing_mm` 2000 — 8 to 256 m — which refuses the 512 m filter. The engine's fault was the sentence; the build now refuses before it builds anything with "far level 6 of 6 asks the ground for heights every 512 m … build it for this world's outermost ring (`ring_spacing_mm` 4000, …)" ([renderer](../subsystems/renderer.md#ground-to-the-horizon)). **With the scene's `tiles.ring_spacing_mm` set to 4000** (a scratch copy; its set built into a scratch cache) the four-ring forest builds and draws.

The same binary (this change's), offscreen 1600 × 900, `--raster hw --shadows csm`, W73's flags, the hardwoods view (600 frames) and W73's 60 s crossing at 40 m over the ground (1,800 frames); `pairs` is the scene's whole count at its largest frame, prefix and slots and tail; GPU and frame milliseconds median / p95 / p99.

| Run | Rings | Tiles per ring | Trees per ring | Ring pairs | Pairs, largest frame | Withheld | GPU ms | Frame ms | GPU memory | Machine (others' CPU start / end, GPU) |
|---|---|---|---|---|---:|---|---|---|---:|---|
| hardwoods, three rings | 1.5 / 6 / 40 | 6 / 104 / 2,421 | 20 / 267 / 13,041 | 0.08 / 1.57 / 4.22 M | 11.45 M | none | 5.96 / 6.50 / 6.78 | 6.05 / 6.63 / 7.03 | 5,206 MiB | 22% / 16%, 5% |
| **hardwoods, four rings** | 1.5 / 6 / 16 / 40 | 6 / 104 / 533 / 1,888 | 20 / 267 / 2,601 / 10,440 | 0.08 / 1.57 / 1.02 / 0.28 M | **8.07 M** | none | 5.56 / 6.19 / 6.42 | 5.64 / 6.26 / 6.62 | 4,209 MiB | 54% / 85%, 3–99% |
| crossing, three rings | 1.5 / 6 / 40 | (at the end) 0 / 22 / 1,601 | 0 / 39 / 3,618 | 0 / 0.17 / 2.18 M | 15.76 M | none | 4.73 / 9.12 / 9.98 | 15.94 / 40.77 / 53.15 | 5,271 MiB | 21% / 14%, 4–17% |
| **crossing, four rings** | 1.5 / 6 / 16 / 40 | 0 / 22 / 256 / 1,345 | 0 / 39 / 406 / 3,212 | 0 / 0.17 / 0.28 / 0.16 M | **11.44 M** | none | 4.36 / 8.79 / 9.58 | 16.09 / 42.77 / 49.47 | 4,203 MiB | 21% / 20%, 4–9% |

**What it says.** The fourth ring carries the forest from 512 m to 1,280 m in its own, coarser forms, where the third had carried it in the 192–1,280 m ring's: at the hardwoods view the rings' pairs are 2.95 M against 5.88 M, the scene's largest count 8.07 M against 11.45 M, and along the crossing 11.44 M against 15.76 M — the three-ring crossing came within 1.0 M of the visibility id's 16.78 M, the four-ring one is 5.3 M under it. The GPU's median is 0.4 ms lower at both and its memory a gigabyte smaller; the frame's wall time along the crossing is the world's builds in both (W73). Both runs of a pair were taken minutes apart on the same machine, the four-ring hardwoods one with others at 54–85% of the CPU, so its times are upper bounds. The prefix in this gate's build is 5.0–5.3 M pairs where W73 measured 0.90 M (230 instances kept against 119): the scene's recipes, read live from the POC, changed since (G86, G87); it is the same in both rows of a pair.

## What was not done

- **The window.** Every run here is offscreen (`--terrain-async` reproduces the window's concurrency without one on the owner's desktop); the owner's 4.4 s window frame has not been replayed in a window after the fix. A `--windowed` replay of the POC's `flight-walk` log is the before and after the owner can run.
- ~~**The placements are not built ahead**~~: they are since W71 (above).
- **The owner's windowed replay** of the POC's `flight-walk` log after W70 and W71 is the owner's to run.
- **No `--game` module** was loaded (the POC's binary is not run here), so the 52 animals and their ground queries are absent; their queries meet the same reader.
