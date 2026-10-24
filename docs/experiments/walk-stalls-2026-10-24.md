# Walking after rapid travel (2026-10-24, roadmap W69)

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

The first walking frame's activation of (b) is 1.03 ms (0.4 ms of it ground heights, 0.8 ms bodies) where it was 637.7 ms; the frame thread's longest wait for the lock is 3 µs where it was 488 ms, and every thread's wait over the walk is 7 ms where it was 37 s — the rebuild's workers no longer serialize on each other either. **The slow frames left are not the walker's**: in (a), (c) and both cold runs every frame over 33 ms has ticks of 0.1–0.3 ms and no reader wait, and they fall in runs where another process held the CPU at 90–100% (an agent's build, at the start or end of the run); the harness's `machine_state` says so, and they are upper bounds of nothing this change touches. (d)'s 4.6 ms activation is a tile of the grove's placements (proxies and compound, 4.0 ms over the walk), the work the next stage moves ahead of the ring.
