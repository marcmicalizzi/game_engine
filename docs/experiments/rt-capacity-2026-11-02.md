# The ray tracing chain's capacity on the owner's v25 walk (2026-11-02)

Roadmap R116. **The question**: the owner's v25 walk of the forest proof of concept (`reports/lab/20261101-115716-655`, 2026-11-01, 3840×2160, `--raster rt --shadows rt`) dropped to about 5 fps for fractions of a second, early and less later. Its records said why: the chain's capacity (`renderer::RtCapacity`, R107) went from 336,348 clusters to 40,960 three seconds after the start, to 12,288 at a glance at the sea, and back up in runs of five to eight resizes a second — 48 resizes of 30–153 ms, 3.3 s in all, each a `wait_idle` and a new set of buffers on the frame's thread — while 754 frames built fewer structures than they wanted (frame 771: 69,632 of 206,170). What does a policy that remembers its peak, judges a shrink by wall time and keeps a floor change, and what does a resize cost when it is swapped in rather than waited for? The rules are on [the renderer's page](../subsystems/renderer.md#a-view-that-comes-back-regrows-once).

## The flight

The owner's recorded poses, frames 0–4,200 of the run (its first 85 s of pose time), one camera-path key a frame at 60 frames a second (W82's path maker, `pose_position`/`pose_yaw`/`pose_pitch`); the run's own scene and the proof of concept's derived-data cache (copied once, nothing cooked into it); `msvc-release` engine-view `--offscreen` at 1920×1080 with the owner's flags (`--terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 1 --time-of-day 15 --no-lights --lod 1 --deform-pool-mib 64 --rt-budget-mib 0 --raster rt --shadows rt`, the flora's motion and wind; no game module), `--warmup 30 --benchmark --wait-quiet 120`, three passes of the path, under the GPU lock. RTX 5090 (the device's budget: 338,115 clusters), i9-10980XE. Three builds of the same tree:

- **before**: `main` at 83af2e3c — R107's window of 240 frames, a growth to the demand plus 25%, every resize waited for;
- **seconds**: the first cut of R116 — the window ten seconds of wall time with no frame count, the peak a minute of wall time, the floor half of it — with the resize still waited for (`renderer.rt.swap_resize=false`), and the resize's cost split by part;
- **after**: R116 as committed — the window ten seconds *and* 240 frames, the peak a minute *or* 3,600 frames — with the resize swapped in.

**The machine.** Shared with four other agents' builds and GPU runs, and another process holding 19.7–21.2 GB of the card's 32.6 GB throughout. *before*: the wait for quiet gave up with others on 100% of the CPU and the GPU idle; at the end others 16.8%, the GPU 100% busy. *seconds*: quiet — others 8.8% at the start, 9.7% at the end, the GPU idle at both. *after*: others 72.1% at the start and 95.3% at the end, the GPU 2%. So every millisecond below is an upper bound; the counts are exact.

**What else is slow on this path.** Frames 2,800–3,970 of every pass take 0.6–2 s each in all three builds, all of it W82's world update (`world.ground_ms` about a second a frame: the ground's tiles), and so do scattered frames before; "frames over 100 ms" below therefore says how many carried a capacity change and how many had less than 50 ms of ground. That slow stretch also made a ten-second window out of 14 frames and forgot a minute's peak inside 90 frames in the *seconds* build, which is why the committed rules count frames as well.

## The numbers

Pass 0 is the first pass of the path, with the load; "all" is the three passes. The resize counts are the run's (`RtStats::grows`/`shrinks`, warm-ups included).

| | before (R107) | seconds (waited) | **after (R116)** |
|---|---:|---:|---:|
| capacity resizes: grows + shrinks | 41 + 10 | 25 + 15 | **4 + 5** |
| the cache's reference resizes: grows + shrinks | 32 + 7 | 12 + 11 | **2 + 4** |
| capacity changes in pass 0's records | 15 | 8 | **5** |
| resizes over 25 ms (the warning), their total | 65, 76.7 s (27 ms – 8.4 s) | 34, 3.6 s (27 ms – 1.3 s) | **7, 1.75 s (27 – 927 ms)** |
| frames short of what they wanted, pass 0 (all) | 15 (47) | 10 (40) | **7 (21)** |
| of them short under the budget, pass 0 (all) | 10 (32) | 3 (23) | **0 (0)** |
| the worst of those | frame 2035: 53,248 of 153,805 (35%) | frame 772 of pass 2: 61,440 of 171,137 (36%) | **none** |
| frames over 100 ms, pass 0: all / with a capacity change / under 50 ms of ground | 684 / 4 / 210 | 483 / 6 / 45 | 617 / **0** / 86 |

The seven short frames of every *after* pass are frames 2,836–2,842, which want 372,296–373,289 structures against the budget's 338,115: past the budget, which the drop rule handles ([the renderer](../subsystems/renderer.md#the-ray-tracing-chains-memory)) and which no capacity policy can. Under the budget no frame of the three passes built less than it wanted.

**Where the capacity went** (pass 0). *before*: 338,115 → 40,960 at frame 210 (the load's frames: 240 of them in a moment), → 53,248 → 61,440 → 217,088 at frames 769–772 as the forest came in, → 36,864 at 1,732, → 53,248 → 69,632 → 192,512 → 217,088 at 2,033–2,063, then 241,664, 274,432, 307,200, 338,115 as the demand climbed, → 73,728 → 16,384 at the sea. *after*: 338,115 → 172,032 at frame 210 (a whole window of the load: ten seconds and 240 frames, to half the budget), → 338,115 at 772 in one step, → 172,032 at 1,959, → 338,115 at 2,036 in one step, → 172,032 at 3,552 at the sea; passes 1 and 2 grew once each at 772, back to the peak, and shrank once each at the sea. No growth went anywhere but back to the peak, so the replay has nothing to say against the 25% headroom or the 4,096 step, and both stay.

## What a resize costs, part by part

The *seconds* build split every slow resize (34 of them, all waited): **the wait for the device 0.05–154 ms, freeing the old set 6–51 ms (once 1,110 ms), making the CLAS set 1.5–10 ms (once 178), the bottom-level set 1–13 ms, the records, the record-to-entry table and the scratch under 1 ms.** So the driver's CLAS set creation is not the slow part; the wait and the free are. A swapped resize takes the wait away and defers the free to the slot's deferred frees, which run on the frame's thread when the slot comes round two frames later — the free's milliseconds move, they do not vanish, and moving them off the frame's thread is not built (it is `gfx::FrameContext`'s, outside this change).

In the *after* flight six of the seven slow resizes were waited, because the cache's references moved in the same resize and the cache's one host-visible table names them (R114's; [the renderer](../subsystems/renderer.md#a-resize-is-swapped-in-not-waited-for)). The one swapped slow resize was pass 0's growth 172,032 → 338,115: **524 ms, 343 of it making the CLAS set and 179 the bottom-level set**, where the same growth waited for was 37 ms in the *seconds* flight (14 waiting, 16 freeing, 5 and 1 making). Made beside the old set on a card another process held 20 GB of, the new 2 GB set waited on the driver making room; passes 1 and 2 made the same growth swapped in under 25 ms. That is why the committed swap first asks the device's memory budget (`VK_EXT_memory_budget`) whether both sets fit what the process may hold, and waits and frees first when they do not.

## What holds it

`systems/renderer/tests/rt_capacity_tests.cpp`: `rt capacity: a demand that falls to a tenth and returns regrows once, to the peak` (R107's rule shrinks to 28,672 after 240 frames at a tenth and grows back in four steps), `rt capacity: a window of fast frames does not shrink` (R107's shrinks after 0.24 s of 1 ms frames; and a handful of one-second frames is not a window either), `rt capacity: the floor holds half the last minute's peak, and halves once a minute` (R107's goes straight to 4,096), all on the CPU; `rt capacity: a resize swapped in while frames are in flight draws what a whole chain draws` on the device — three resizes in flight under each raster mode, the same colour and ids as a chain that never resizes, no stop for the device swapped and one a resize waited. R107's own case keeps its arithmetic with the seconds, the peak and the floor set to nothing.
