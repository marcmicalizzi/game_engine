# The chain's capacity step made ahead, measured (R133 and R121, 2026-11-15)

Roadmap rows R133 ("allocate the chain's next capacity step ahead, on a worker") and R121 ("a
resize that changes the cache's references swaps too", and the retired set's free off the frame's
thread) were written on 2026-11-02 and 11-04. Checked against the code on `main` at b95676a7, both
describe work that is done:

- **R121's references**: since R130 (6380a8ad, 2026-11-03) the cache's table has one copy a frame
  slot (`GpuScene::k_rt_cache_table_copies`), written when the slot's frame is recorded, and a step
  of the references makes its three buffers beside the old ones and hands those back with the
  chain's, so it swaps ([renderer](../subsystems/renderer.md#a-resize-is-swapped-in-not-waited-for),
  "The cache's references swap too").
- **R121's free**: since R128 (352868b5, 2026-11-04) the retired buffers are freed by a job on a
  one-thread pool of the renderer's own (`renderer.rt.free_on_worker`).
- **R133's step**: since R152 (524db0b4 and 25bc61fb, 2026-11-12) a step the policy asks for is
  made on that same worker from a copy of its inputs and taken by the first `begin_frame` that finds
  it made (`renderer.rt.step_on_worker`); only a frame whose read-back demand is already past the
  chain waits for it ([renderer](../subsystems/renderer.md#a-step-is-made-on-the-worker)). The
  slope rule R133 asked for ("a policy that knows the next step two frames early") is R128's
  `renderer.rt.slope_frames`. R152 measured it on the forest staging: main paid 5.9 to 11.1 ms for a
  step, the branch none ([the walk](rt-chain-walk-2026-11-12.md#the-walk-as-a-flight-and-what-the-staging-copy-does-not-reproduce)).

What this page adds is the step measured again on today's main, on a scene of the engine's own,
with the worker on and off in one binary, and what a step made ahead holds.

## The flight

`content/test-scenes/streamed-forest` (3,000 of the flora capability's trees streamed tile by tile;
its 3,000-frame path crosses above the crowns and comes down among the trunks), 3840 × 2160
offscreen, `--raster rt --shadows rt`, `renderer.gi.mode=bounce`, main's release build copied out
of the tree (b299cce2's source; the commits since are pages and ceilings). At the defaults the
chain never steps on this path: its first capacity is the budget (330,881 clusters) and the
two-minute shrink window outlasts the 50 s flight. So the policy is made to follow the demand,
which is how a step is made to happen on a scene this size:

- **follow**: `renderer.rt.peak_seconds=0`, `peak_frames=0`, `floor_pct=0`, `shrink_seconds=1`,
  `shrink_frames=30`; the headroom (25%) and the slope (4 frames) as they are, so a growth is asked
  for before the frames need it: the steps made ahead;
- **tight**: the same and `renderer.rt.headroom_pct=0`, `slope_frames=0`, so a growth is asked for
  only once the demand is past the capacity, and the frame that asks is already short and waits:
  the steps a frame pays for.

Each policy flown four times in one GPU lease, worker on and off alternating (on, off, on, off).
The flights' scripts and logs are outside the repository (the coordinator's scratch, `b66\R133`).

**The machine**: RTX 5090, 20:45 to 20:51 on 2026-11-15; other processes held the CPU at 100% for
every flight (a debug and a release build of this worktree and other agents' work), the GPU was
idle but for these flights, and free commit was 10 to 26 GB. Every run's harness line says its
milliseconds are upper bounds; the pairs are what compares.

## What a step cost the frame's thread

`sections.resize_ms` of every frame that took a step (the frame record's share of `begin_frame`
spent on the chain's capacity, R130), over the flight's 9 (follow) or 12 (tight) steps:

| policy | worker | steps | median | most | sum | frames over 33 ms (all causes) |
|---|---|---:|---:|---:|---:|---:|
| follow | on | 9, 9 | 0.38, 0.37 ms | 2.88, 0.88 ms | 6.3, 3.9 ms | 19, 51 |
| follow | off | 9, 9 | 2.69, 3.13 ms | 9.98, 10.16 ms | 31.7, 33.5 ms | 35, 29 |
| tight | on | 12, 12 | 2.10, 2.30 ms | 5.38, 3.69 ms | 30.1, 25.4 ms | 29, 38 |
| tight | off | 12, 12 | 2.83, 2.44 ms | 44.98, 119.82 ms | 78.8, 146.1 ms | 29, 43 |

- **A step made ahead costs its frame a third of a millisecond** where the frame's thread made it
  in about three: what is left is the taking (assignments), the policies' bookkeeping and the
  `ray tracing chain resized` log line at `--log info`, on a CPU other processes held whole.
- **A step a frame is short for costs that frame the allocation either way** (2.1 to 2.3 ms on the
  worker, 2.4 to 2.8 on the frame's thread at the median), as R152 designed: the frame waits rather
  than record into too little. Both binaries' records show 20 frames of each tight flight built
  against less than they wanted, the two frames before each growth lands, which no rule that hears
  of the demand two frames late can see.
- **The one slow step**: 28,672 → 32,768 clusters took the frame's thread 45 and 120 ms (`clas_ms`
  44.4 and 118.4: the driver's allocation of the new 299 MB cluster set) in both "off" flights,
  and the worker 2.1 and 3.7 ms for the same step in both "on" flights. Two of each is not enough
  to say why; it is the variance R128 recorded (1 to 349 ms for the same step), and on the worker
  it is not a frame's unless the frame is short.
- **No frame over 33 ms was a step's** but those two: the rest are the slot's wait on a CPU-starved
  machine and the world's tile hand-overs (up to 720 ms of `world` in one frame), the same on both
  sides.

**The branch beside main** (the counter below, one memory-budget query per step asked of the
worker): the follow policy, worker on, main's binary and this branch's release build alternating in
one lease at 21:21 to 21:23 (other processes on 83 to 98% of the CPU): 9 steps a flight on both,
`resize_ms` at the median 0.32 and 0.36 ms on main, 0.30 and 0.28 on the branch (most 1.11 and
0.95), the wall median 4.40 and 4.37 against 4.43 and 4.49 ms, frames over 33 ms 5 and 1 against 3
and 4 (none a step's), and no step asked past the device's budget.

## What a step made ahead holds, and on a smaller card

The step is made beside the chain it replaces and stands there from the moment the worker makes it
until the frames that read the old buffers have completed: taken by the next `begin_frame` that
finds it made, its replaced buffers wait on that slot's retired list until the slot comes round
(two frames in flight by default) and are then freed on the worker. So for three or four frames the
device holds `GpuScene::rt_step_bytes` more than the chain: the chunks the new capacity lacks, the
bottom-level set, and the cache's references when they move. Under 65,536 clusters the chain is one
chunk and a step makes it whole again (here 112 to 561 MB at 9,130 bytes a cluster with
micromaps); past that, the partial chunk and any chunks after it, so a growth by the headroom makes
one chunk, at most 598 MB, and a jump back to the last minute's peak the chunks between.

The capacity never passes the budget (a step's target is clamped to `renderer.rt.budget_mib` or the
device's rule before it is asked), so the transient is at most the budget again. On a 12 GB card
the device's rule is 768 MiB before micromaps and about 1.1 GiB with them (the RTX 5090's measured
ratio, 9,156 to 6,280 bytes a cluster), about 128,000 clusters in two chunks: a step holds one
chunk, up to 570 MiB, beside a chain of up to 1.1 GiB, for three or four frames.

**The device's room is not asked for a step made on the worker.** A step on the frame's thread asks
the device's memory budget first and, when the old and new sets will not fit beside each other,
waits for the device, frees and makes (R116); the worker's step makes beside the old set whatever
the budget says. R116 measured such a step at 524 ms in the driver on a card another process held
20 GB of, where the waited one took 37 ms; on the worker that time is the chain's and not a frame's
unless the frame is short, but whether the eviction the driver makes the room with costs the frames
after it was not measured, and could not be tonight (the forest staging does not fit the machine's
free commit). So the step is made as R152 made it, and since this page it is **counted**
(`RtStats::worker_steps_past_budget`) and **logged** at info (`the ray tracing chain's step is
made past the device's budget`, with the step's bytes and the budget), so that the next recording
on a full card says whether it happens and how often.

## What this does not cover

- The forest staging and the owner's walk: not flown (the machine had about 27 GB of free commit
  and a forest load needs 40).
- A full card: the streamed forest uses about 9 GB of the 32, so every step here had room.
- The tight flights' short frames drop structures for two frames a growth on both sides, which is
  the policy's lateness and not the step's; the defaults' headroom and slope exist for that.
