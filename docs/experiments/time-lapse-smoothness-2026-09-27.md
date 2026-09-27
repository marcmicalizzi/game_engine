# The dunes at high time-lapse rates (2026-09-27)

**The question.** The owner flew the erg (`content/test-scenes/desert-erg`) at a game day and a game week a real second ([third session](third-interactive-session-2026-09-25.md#the-owners-flight-in-time-lapse-2026-09-27)) and reported three things: the sand moved in steps about once a second at the two high rates while a tenth of a day a second (8,640) was smooth; the sun seemed to race across the sky; and the towering dunes' crests stood still while the waves raced. This page is what each turned out to be and what was done, all of it measured on the CPU (the session had no GPU): the smoothness in a model of the time-lapse driven through the same functions `TerrainMotion` runs, the evaluation on the 4-vCPU container, the crests on the committed scene.

**Where.** The 4-vCPU Linux container (Intel Xeon at 2.8 GHz), `linux-clang-debug` for the model and the crest measurement (both are counts, not times), `linux-gcc-release` for the benches, run with `--wait-quiet`. The owner's machine — 16 cores, an RTX 5090 — is where the 45/11/4-frame evaluation times come from (750 ms for the erg's grid, 190 ms and 72 ms for the middle and inner rings' fields, [renderer](../subsystems/renderer.md#the-dunes-in-time-lapse)).

## A. The model, and the metric

`systems/renderer/tests/terrain_clock_tests.cpp` models the interactive time-lapse with the erg's three levels — the scene's grid at 1.5 m, the middle ring at 1 m, the inner at 50 cm, each a line of 192 samples of a travelling dune profile (2 m tall, 12 m wavelength, a sharp brink, 0.9 m a game day: the erg's waves, its fastest band) — on **one field worker** that evaluates them in turn, **every** field taking its level's evaluation time (45, 11 and 4 frames at 60 fps). It picks the next level the way `TerrainMotion::schedule_next` does, times the field with the cadence rule and the keep-up rule, and moves the surface with `terrain_surface_frame` (and, after, `TerrainClock`) a frame at a time, drawing every level's samples as the pool pass would (`a (1 − t) + b t` in floats). Nothing in it is timed: two runs are the same run.

**The metric.** The sand drawn is a function of the surface time, so its motion is the surface time's advance per frame over the true one (`rate / 60`), the **speed ratio**, and a time-lapse is smooth when that changes slowly. Two numbers, both taken after the first three seconds (before the first fields exist the surface can only stand):

- **stop-go**: the share of frames on which the surface stood still while game time ran *and* which a frame above twice the true speed follows within a second — the hold-then-burst;
- **window ratio**: over every one-second window, the largest speed ratio over the smallest (a stand-still counted as 1% of the true speed), the worst window of the run. 1.0 is a constant speed; a stop inside a moving second reads 100 or more.

**The code before, at the three rates** (900 frames, `Mode::before`: the surface chasing game time, the keep-up rule timing a field by the evaluation's wall time alone, one field ahead a level):

| Rate | stop-go | window ratio | frames stood still | speed ratio, min–max | largest move / bound | lag at the end |
|---|---|---|---|---|---|---|
| 8,640 | 0 | 1.0 | 0 | 1.00–1.00 | 0.61 | 0 |
| 86,400 | 0.874 | 833 | 92.5% | 0–8.33 | 0.83 | 7.1 s of real time |
| 604,800 | 0 | 143 | 59.9% | 0–1.43 | 1.00 | 7.9 s |

That is the owner's report exactly: smooth at 8,640, and at a day a second the surface stands nine frames in ten and bursts at up to eight times the true speed. At a week a second the burst is capped by the per-frame bound (1.43, which is why stop-go reads 0 there and the window ratio does not), so it reads as stand-crawl-stand, and the lag behind game time grows without end. The cause is the worker: three levels whose fields are each timed 1.5 evaluations ahead ask for more than one worker gives — the load is about 2 — and the keep-up rule, timing by the evaluation alone, never sees the wait behind the other levels.

## B. The clock

**What was built** (`TerrainClock`, `terrain_keep_up_time` in `terrain_time.h`; [renderer](../subsystems/renderer.md#a-clock-that-never-stops)). In a window the surface runs at game time minus a latency L at a speed that changes by at most the rate every half second, closes a gap over two seconds, never exceeds 1.5 times the rate, and brakes for the newest field every level has at the same deceleration. L is 1.2 times the worst of each level's last eight **turnarounds** — real seconds from the level wanting a field to its being ready — at the rate, the largest of the levels'. A ring holds two fields after b (its four slots), the scene's grid one (its three, which `gpu_scene.cpp` gives it and which this change does not touch) and so times its fields with twice the lead. Fields are timed by the worst recent turnaround, and no shorter than the span a pair crosses within the per-frame bound.

**What the model found on the way.** Each step was tried in the model before the next:

1. **L from the arrival deficit** — how far game time had run past a level's newest field when the next arrived, 1.2 times the worst, floored at the interval between arrivals, as the brief suggested — **diverged**: fields are asked for when the surface reaches b, so a lagging surface asks later, the newest field falls further behind game time, and L grew to 8 s of real time at a week a second. The interval floor also cost 8,640 its smoothness: there the interval is the displacement cadence, four seconds between fields that were ready in 45 frames. L is the turnaround instead, which a lagging surface does not change.
2. **Timing fields by the turnaround's EMA** left dips to 0.3 of the true speed whenever a ring's field waited behind the grid's 45 frames; the **worst** recent turnaround removed most of them.
3. **Two fields after b**, so a level asks for the one after next while the next waits to be drawn: the window ratio went from 4.5 to 1.1–1.2 at the high rates.
4. **The grid has only three slots**, so one field after b: with the model holding it to that, the ratio came back to 1.4–1.6 (speeds 0.6–1.24); **twice the lead for a level with room for one** brought it to 1.0.

**After, at the three rates** (1,200 frames, `Mode::after`):

| Rate | stop-go | window ratio | frames stood still | speed ratio, min–max | largest move / bound | L | fields (late) |
|---|---|---|---|---|---|---|---|
| 8,640 | 0 | 1.0 | 0 | 0.99–1.00 | 0.01 | 11,059 s (77 frames) | 30 (8) |
| 86,400 | 0 | 1.0 | 0 | 0.91–1.00 | 0.10 | 129,600 s (90 frames) | 40 (38) |
| 604,800 | 0 | 1.0 | 0 | 0.91–1.00 | 0.42 | 907,200 s (90 frames) | 39 (39) |

The picture lags game time by L: 1.5 s of real time at the two high rates, 1.3 s at 8,640. A worker that stops for five seconds at a week a second (`a stalled worker…`) brakes the sand to a stop over half a second and sets it off again at the rate, never above it. Every invariant the blend had holds in every run: no sample of any level moves more than its bound in a frame, every handover draws b to the bit, every level has one surface time. **Offscreen nothing changed**: `wait` has no clock, keeps game time and waits for its fields, and the second field ahead is timed by displacement alone like the first, so two offscreen runs capture the same bytes.

**What it needs from the GPU side, and does not have.** A fourth field slot for the scene's grid (`GpuScene`'s `terrain_[0].slots`, 3 today, in `gpu_scene.cpp`, which this change was not to touch) would let the grid hold two fields after b like the rings and drop its doubled lead — its spans half as long, so the waves on the grid cross-fade over half the game time. It is 67 MB more device memory on the erg.

## C. Evaluation

**The bench was measuring one core.** `terrain.field.reevaluate` ran its pool with `pin_threads = false`, and the bench runner pins its own thread to one CPU before any bench runs, so every worker inherited that one CPU: user time equal to wall time, 13.4 s for the 4,097 grid on the 4-vCPU container. Pinned workers (the pool's default): **1.0 s at 2,049 and 4.4 s at 4,097**, 3.8 million samples a second; the documented 2.58 s and 8.81 s came from the same bench, so from one core too ([bench](../subsystems/bench.md) now says so, and the terrain bench pins its pool). The owner's 750 ms for 4,097 on 16 cores is 22 million samples a second.

**Where a sample goes.** A new bench, `terrain.field.window` — a 257² window at the grid's 1.5 m on one thread, stepping 400 m along the erg's diagonal each iteration — costs 51 ms, **0.78 µs a sample**. Callgrind over it: the primitives' own profiles (`primitive_value`, the lee zone, the side slope, the cross profile) about 30%; finding and visiting the primitives near a point (`band_value`, the per-band binned cells) about 30%; stacking the bands and the repose bound about 10%; the integer sine about 7%; the gather itself under 3% at this density (at the whole grid's 129² smoke size it is 60%, which is why the window bench exists).

**The cheapest wins, tried in order.**

- **Work distribution.** The renderer's field worker cut the grid into jobs of 16 blocks of 64 × 64. The erg's blocks cost 2.0 to 11.6 ms on one core (median 3.0, 90th percentile 3.9; the mega-draa's reach is the dear end), and a list schedule of the measured blocks puts the last wave at **2.4% idle on 16 workers and 10.7% on 32** at 16 blocks a job, **0.6% and 1.9% at 4**. The owner's 16 cores are likely 32 threads, so `k_blocks_per_job` is now 4: about 8% off the grid's evaluation there, a 41-frame grid instead of 45. The merge is by block index, as before; the bytes do not depend on the split.
- **Inlining the integer sine** (7% of the profile, an out-of-line call per sample for a sinuous crest): 52.1 ms against 50.9, inside the noise. Not kept.
- **Evaluating only the window the cut needs.** The only part of the grid nothing draws is the middle ring's square (the pool pass moves the grid's vertices inside it onto its edge and reads one sample inside the edge for the normal): 11% of the erg's grid. But the square moves at every re-centre, and the grid's pair is not re-evaluated then, so skipping it needs the grid's pair carried over at a re-centre as a ring's is. Not cheap; not done.
- **SIMD in the per-sample loop.** The per-sample work is a loop over a data-dependent handful of primitives per band in 64-bit fixed point, and AVX2 has no 64-bit multiply; a vector form would evaluate one primitive against a run of samples (structure of arrays over the block's samples), a rewrite of `band_value` and `primitive_value` that must reproduce the golden hashes bit for bit. Perhaps 2–4×; not the cheap win the brief meant, and not enough (below).

**The highest rate the CPU sustains.** "Sustained" meaning every level's fields timed by the displacement rule alone — the waves sliding a quarter of a sample between fields rather than cross-fading — the model's ladder of rates a tenth apart (`the highest rate one field worker keeps…`) gives **about 7,200 game seconds a real second** at the owner's times (45/11/4 frames) and **about 7,900** with the grid at 41 frames: two game hours a second. The clock keeps the sand smooth far above that — the fields are spaced further apart and the waves cross-fade — but it does not make them slide. **A week a second is not reachable on the CPU**: sliding the waves a quarter of a sample between fields needs a field every 12,000 game seconds for the inner ring (20 ms of real time at a week a second, for a million samples), every 24,000 for the middle ring (40 ms, four million) and every 36,000 for the grid (60 ms, 16.8 million) — about 430 million samples a second against the owner's 22 million, twenty times short. No SIMD rewrite of this loop closes that.

**A GPU path (a design note; nothing of it is started).** Evaluate the field **per vertex of the frame's cut, in the pool pass, every frame**, at the surface time itself:

- *What moves to the GPU*: `DuneField::sample`'s dunes detail — the bands' primitive values, their maximum, the stacking with its couplings, the repose bound, the ridge and basin masks — as a Slang port of the integer arithmetic (`int64_t` is core in SPIR-V with `shaderInt64`, which every device the engine runs on has; the sine table is 1,025 constants). A per-frame **primitive table** built on the CPU: for each band, the cells within reach of the cut's bounds, each cell's primitive (`make_primitive`, a hash of the cell and the band's time shape, the same function the CPU gather calls), binned by cell as `Gather` bins them; a few hundred kilobytes for the erg's view, rebuilt only when the band lattices' displacement or the view's cell range changes.
- *What it costs*: the cut is 1,100–2,800 pairs, 150,000–360,000 vertices; each is about 2,200 cycles of 64-bit integer work on the CPU (0.78 µs at 2.8 GHz), perhaps 4 times that in emulated 64-bit multiplies on the GPU, and the normal wants the gradient (the profile's `k_slope` path gives it analytically) rather than four more samples. On the order of 10¹⁰ operations a frame: a millisecond or less on an RTX 5090, 5–20 ms on the Maxwell and Pascal baseline runners — so it is a capability with the CPU fields as its fallback, not a replacement.
- *What it removes*: the fields, their slots and uploads, the cadence and keep-up rules, the worker and its turnaround — and L: the surface is the game time, every frame, at any rate. The rings and the grid read one function, so their borders meet by construction.
- *What it keeps*: the cluster spheres and LOD errors are still the rest pose's, padded by how far the sand is from rest, which must then be bounded without evaluating the field on the CPU (the sum of the bands' heights over each cluster's footprint is a loose bound; a coarse CPU field at the cadence a tight one). The determinism test becomes "the GPU's heights are the CPU's integers, converted", held on a device.

## D. The sun

The sun never moved: `frame_lighting` has always put it at `normalize(0.4, 0.8, 0.45)`. What moved were the **two stand-in point lights**, which orbited the scene with the frame index (0.013 radians a frame), a warm one 1.35 scene radii out and 0.7 up whose reach is four radii — over the erg a second sun going round every eight seconds. They now stand where frame 0 put them unless `--orbit-lights` (`RenderSettings::orbit_lights`, protocol `RenderSettings` version 9) asks; the sun stands where `--sun <azimuth,elevation>` (degrees; `RenderSettings::sun_*`) or the `renderer.sun.azimuth_deg`/`elevation_deg` tunables put it, by default 48.37° and 53.03°, which `sun_direction` returns as the old vector to the bit. No test depended on the orbit: the reference scenes render frame 0, the resolve and the path tracer are compared at one frame index and both call `frame_lighting`, and the rest turn the lights off. There is no time of day.

## E. The big dunes' crests

**What Bagnold says.** A dune's celerity is the sand flux over its height. The erg's net flux along the mega-draa's travel is 193 m² a game year (storms included), so a 188 m dune on its own would move 1.02 m a year and an 80 m one 2.4 m. The generator moves a band as one lattice at the flux over its **celerity height**, the middle of its range: the mega-draa (80–200 m) at 140 m, **1.38 m a game year**; the draa (10–25 m) at 17.5 m, **11.0 m**; then the crests 45.3 m, the barchans 59.3, the waves 350.

**What the field does** (`terrain_crest_tests.cpp`, on the committed scene three years in, a transect down each band's travel through its tallest primitive):

| | band travel, 1 y / 3 y | measured, 1 y / 3 y |
|---|---|---|
| a draa at full weight (24 m, on a mega-draa's flank), its brink | 11.00 / 33.77 m | 11.05 / 33.75 m |
| the tallest mega-draa (188 m), its whole profile | 1.37 / 4.22 m | −7.50 / +5.75 m |
| the same, its slip face at half height | | −9.25 / +5.67 m |
| the same, over 10 and 30 years, its whole profile | 14.05 / 42.24 m | 19.50 / 29.25 m |

**Not anchored; physics.** The draa's brink goes exactly where its band goes. The mega-draa's lattice does too (it is the same code), but its slip face **re-forms** with each year's wind — its side follows 365 days of flux and its sharpness 120 — which moves its face and brink by about ten metres a year either way while the translation is a metre and a half; over decades the profile follows the band within that swing. At a week a second a game year is 52 real seconds: 1.4 m in 52 s over a 1.5 m grid is the stillness the owner saw, and it is what an erg does.

**So `celerity_scale`** (a per-band stylization in the scene's band table, (0, 1000], 1 by default and never on by default; [terrain](../subsystems/terrain.md#how-far-the-big-dunes-move)): the band's celerity height over the scale, so its lattice travels that many times Bagnold's distance and the cadence follows. 20 on the mega-draa is 28 m a game year. The field's and the scene's hashes take it only when it is not 1, so no golden and no cache entry moved.

## What is not done

- The GPU path above, and the fourth grid slot (B).
- Nothing here ran on a GPU or in a window: the owner's next flight is the check, with `--interactive --benchmark` for the per-frame `terrain` records.
- `render.*` over the protocol still does not run the motion (as before).
