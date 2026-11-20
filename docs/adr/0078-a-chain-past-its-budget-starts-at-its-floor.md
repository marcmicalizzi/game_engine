# ADR-0078: A traced chain past its budget starts at its floor, not at the budget, and its references start at the floor too

- **Status:** Proposed
- **Date:** 2026-11-20
- **Supersedes:** the part of [ADR-0068](0068-the-traced-chain-holds-what-frames-asked-for.md)'s decision 1 that keeps a provisional first capacity made at the budget; the rest of ADR-0068 stands
- **Plan references:** docs/plan/04-renderer.md §4.4 (ray tracing: the budget manager), §4.9 (streaming and residency), [ADR-0063](0063-memory-is-held-under-one-budget-and-by-travel.md) (one budget, a governor over pools), [ADR-0068](0068-the-traced-chain-holds-what-frames-asked-for.md)
- **Docs touched:** [renderer](../subsystems/renderer.md) ("A guess past the budget is not made at it", "A guess is not a demand"), [apps](../subsystems/apps.md) (`--memory-report`'s `device_buffers.events`), [the measurement](../experiments/start-commit-by-owner-2026-11-18.md#w51-6-the-peak-at-the-first-frame), `docs/roadmap.md` (W51)

## Context

ADR-0068 decided that a first capacity no frame asked for is a guess and is given back once the
first frames have said what they build (`renderer.rt.first_seconds`, ten seconds of frames). It
left where that guess is made alone: a scene whose estimate of what a frame could build passes the
budget started at the budget, and the give-back undid it later.

Roadmap W51 measures what a launch commits, because a forest that needs 60 GB of commit to start
cannot ship (the owner, 2026-11-18). Its third stage (W51-6, 2026-11-19) read the device's large
blocks in order from `create` to the tenth second of frames on the forest's staging at 3840 × 2160
under traced primaries and shadows ([the measurement](../experiments/start-commit-by-owner-2026-11-18.md#w51-6-the-peak-at-the-first-frame)):

- **The load's maximum was the guess, not a block held twice.** The chain was made at the budget
  (330,906 clusters, the estimate 710,947 being past it: six chunks, records, a bottom-level set
  and scratch, 3.7 GiB) at `create`, held through the rest of the load, freed and made again at
  the budget when the world's first tiles arrived, and given back ten seconds into frames whose
  first built 6,306 structures. The commit's peak sat on the first frame, where that guess met the
  world's first commit: 22.71 GiB against 19.13 ready.
- **The cache's references did the same through their estimate**, every cached pair in every
  view: inside their ceiling, so the world's arrival took them from the floor to it before the
  first frame, a waited resize that made the whole chain again for a bottom-level set of 650 MiB
  where the floor's is 279, for frames that referenced 50,016.
- The budget is a share of the card (a sixteenth, scaled for micromaps), so a capacity made there
  says what the card is and nothing of the scene. ADR-0068 had already made that point about what
  is *held*; this is the same point about what is *made first*.

The alternatives: keep the budget and give it back sooner (the window cannot close before the first
frame, which is where the peak is); make the guess at a fraction of the budget (still the card's
size, only smaller); start at the floor every scene inside its budget already gets when its union
is larger (`renderer.rt.initial_clusters`, 65,536 clusters, one chunk), and let the frames' own
demand grow it as every chain after a give-back already does.

## Decision

1. **A chain whose estimate passes its budget, and whose union the budget does not hold whole,
   starts at `renderer.rt.initial_clusters`** (`first_rt_capacity`), clamped to the union and the
   budget. It grows by what its frames ask, a step made on the worker, as a chain does after
   ADR-0068's give-back. A scene inside its budget is untouched: allocated for every pair it could
   build, so it never drops anything. This replaces ADR-0068 decision 1's starting point; its
   window, its give-back and its rules for what a frame did ask for stand, and apply to the floor
   as they applied to the budget.
2. **The cache's references start at the same floor** (`first_rt_references`), clamped to the union
   and their limit, whatever their estimate. A union under the floor is held whole, as before.
3. **A frame before the first read-back that wants more keeps whole instances and counts what it
   dropped** ([ADR-0074](0074-a-frame-past-its-capacity-keeps-by-size-on-screen.md)'s order); a frame
   somebody waits for (`render_offscreen`, every capture and test) is drawn complete as ever.
4. `renderer.rt.initial_clusters=16777216` is the rule before, kept as the comparison.

## Consequences

- On the staging at the top setting the maximum falls from 22.71 to 19.66 GiB against batch 69's
  main the same hour, the device at it from 16.79 to 13.92 GiB, ready and standing unchanged, and
  no frame of the first hundred short of its demand or dropping an instance.
- A view that asks for much more than the floor in the frames before the first read-back is short
  for the frames the read-back and the step take, counted in `RtStats` (`rt_built`, `rt_wanted`).
  A walk's first frames are where that would show.
- `--memory-report`'s large-buffer log keeps its makes and frees in order with their times
  (`device_buffers.events`), so the next stage's peak is read off one list.
- ADR-0068 is marked superseded in its decision 1's starting point only, with a link here.

## Revisit when

- A walk or a flythrough shows holes or missing shadows in the first seconds after a load that a
  first capacity at the budget would have covered (`rt_built` under `rt_wanted` on frames nobody
  waited for).
- The governor of ADR-0063 is built: a first capacity becomes its question, with the card's free
  memory at load as an input.
- The floor itself shows in a launch's commit on a small card (it is one chunk on the RTX 5090).
