# The traced chain on the owner's walk (R152, 2026-11-12)

Roadmap R152. The owner judges performance at 3840 × 2160 with traced primaries, traced shadows and
the bounce. On their walk of the forest proof of concept's v41 world that morning (the proof of
concept's run `reports/lab/20261112-111142-350`, engine 33ff2824, 596 s, 26,468 frames) the engine
logged `the ray tracing chain dropped instances' structures bound=budget unit="cluster structures"
wanted=376309 built=338072 capacity=338072 limit=338072`, fifteen `the ray tracing chain's resize
was slow` and 1,586 `the pair budget withholds tiles' placements`. This page is what that recording
says, the walk as a flight an agent can repeat, where the demand comes from, what each cut returns,
and what was changed.

Everything measured here is the RTX 5090 (32,404 MiB of heaps), `msvc-release`, `engine-view
--offscreen` under the GPU lock, with the proof of concept's flags (`--terrain-tiles --terrain-far 6
--time-of-day 15 --lod 1 --deform-pool-mib 64 --raster rt --shadows rt`, the flora's motion on at
wind 0.7, micromaps on, `renderer.gi.mode=bounce`, no budget named). The machine's state is beside
every table.

## What the recording says

Read as text: the run's `frames.jsonl` (one record a frame: the pose, the chain's counts, the
frame's CPU sections), its `stderr.log` and its `report.json`.

**It is a walk with jumps.** The owner walks at about 1.5 m/s and between walks flies or jumps
kilometres in a second or two (the log says `engine-view: flying`): from the lake to a ridge 1,100 m
up, to the southern slopes, to the stream marks. Every jump streams a new set of tiles in, and the
frames after it are the ones the chain is short on.

**The drop is one view, and its bound is the budget.** 36 frames of the 26,468 built less than they
wanted, all in three seconds at 5 min 41 s of the walk's own clock (16:21:42 by the log, nine and a
half minutes after the launch; the proof of concept's note calls it "the 7 to 8 minute mark"). The
eye had jumped to a slope at
(-1313, 583, 4097), 583 m up, and looked down it over the forest below (pitch -21 to -28 degrees):

| walk time | eye | looking | visible pairs | casters | wanted | built | capacity |
|---|---|---|---:|---:|---:|---:|---:|
| 340.30 s | -1313, 583, 4097 | along the slope | 128,336 | 39,086 | 127,225 | 127,225 | 338,072 |
| 340.71 s | the same | down the slope | 730,726 | 14,601 | **617,554** | 338,072 | 338,072 |
| 342.69 s | -1446, 489, 3902 | down the slope | 720,555 | 161,048 | 527,480 | 338,072 | 338,072 |
| 343.43 s | the same | along it | 156,394 | 20,478 | 107,290 | 107,290 | 338,072 |

The summary's `rt` block: `peak_wanted` 618,793, `overflow_frames` 36, `dropped_instances`
2,410,161 (instance events over those frames), `dropped_caster_instances` 193,263. The logged line
is the first of them (376,309, said once per bound). **338,072 is the budget and nothing else**:
with no budget named the device's rule gives this card 2,952 MiB (a sixteenth of its heaps, scaled
by what the micromaps add to a slot), and a cluster of capacity costs 9,156 bytes, 8,832 of them the
driver's reservation for a structure of 124 triangles and 64 vertices. So the view wanted 5.66 GB of
reservation; the capacity that holds it with the policy's headroom is 7.1 GB, on a card the forest
already fills to 26 to 27 of its 32 GB. A larger budget is not the answer; what the 618,793 are is
the question (below).

**Fifteen slow steps, and thirteen of them did not change the capacity.** The summary counts 18
growths and 18 shrinks of the capacity: it moves between the budget and half of it (172,032),
because a ten-second window that peaked under half shrinks the chain to the floor (half the last
minute's largest) and the next view of the forest grows it back. A replay of the recording's demand
through the policy's arithmetic (`RtCapacity`, on the CPU, without the read-back's two frames of
lag) gives 17 and 18. The slow ones, as the log names them:

| what stepped | count | whole step | of it: waiting for the device | freeing | making the cluster set |
|---|---:|---|---|---|---|
| the cache's references alone (`from=338072 to=338072`) | 13 | 66 to 130 ms | 0.1 to 71 ms | 42 to 81 ms | 6.6 to 14.6 ms |
| the first growth after the load (330,056 to 338,072) | 1 | 96 ms | 0.1 ms | 88 ms | 6.7 ms |
| a growth back to the budget (172,032 to 338,072) | 1 | 254 ms | 121 ms | 122 ms | 8.2 ms |

Every one of them says `swapped=false`: the device had no room for the new buffers beside the old,
so the step waited for the frames in flight, freed everything and made everything again. **For a
step of the references alone that was the wrong answer twice over**: the room was asked for the
whole capacity's 3.1 GB when the step makes 0.3 GB (the references and the bottom-level set), and
refused, the step then freed and remade the 3.1 GB cluster set it had no reason to touch. The time
is the free (the driver returning 3 GB on the frame's thread) and the wait, never the allocation.

**And the steps that were swapped still cost the frame their allocation.** The frame records carry
each step's share of `begin_frame` (`cpu.resize`): 61 frames of the walk paid for a step, 47 of them
more than 4 ms and 14 more than 50 ms, 1.74 s in all, against a median frame of 17.5 ms (p95 45,
p99 168). The 32 between 4 and 16 ms are steps the device did have room to swap: the new buffers
made on the frame's thread while the old ones wait for their frames.

### The policy, replayed

The recording's demand (`rt_wanted` and the wall clock of each of its 26,468 frames) fed to
`RtCapacity`'s arithmetic on the CPU, starting at the budget as the walk did, with the growth and
headroom rules as they are and only the shrink window changed. A replay has no read-back lag, so it
counts steps and not short frames:

| shrink window | growths | of them back to a capacity a shrink gave up | shrinks | capacity held, mean |
|---|---:|---:|---:|---:|
| 10 s (main; the run itself: 18 and 18) | 17 | 16 | 18 | 250,088 (2.13 GB) |
| 30 s | 7 | 7 | 7 | 294,026 (2.51 GB) |
| 60 s | 4 | 4 | 4 | 307,462 (2.62 GB) |
| 90 s | 2 | 2 | 2 | 334,062 (2.85 GB) |
| **120 s** | 0 | 0 | 1 | 335,596 (2.86 GB) |

(GB at 9,156 bytes a cluster of capacity.) A window that slides in six parts instead of tumbling was
tried and is worse at the same length (120 s sliding: 2, 2 and 2), because a tumbling window starts
again at every growth and so waits out a whole window of quiet from the last one, where a sliding
one shrinks as soon as any two minutes have been quiet; it was not built. What the longer window
costs is 0.73 GB of capacity held on this walk; what it saves is every one of the 16 growths a
shrink had caused. `renderer.rt.shrink_seconds` is 120 since.

**The pair budget withholds through most of the walk.** 1,586 lines (one each time the count of
withheld tiles changes), 1,518 naming ring 3 and 68 ring 2; up to 1,002 tiles withheld at once. In
the forest the scene holds up to 16.77 million pairs of the 16,777,216 the visibility id names; on
the open mountain, 2.4 to 9 million.

## The walk as a flight, and what the staging copy does not reproduce

The recording's poses (the walker's soles, so the eye is 1.65 m above them; yaw and pitch) become
camera paths with a key every 0.1 s at the recording's own clock, 60 frames a second. Three of
them, made by a script outside the repository: the walk (the recording's 322 to 382 s: the slope
the owner walked, the jump to the vantage, both looks down it, the walk on and the jump after;
3,600 frames), its worst place (338 to 345 s, 420 frames) and the stream marks (390 to 510 s, not
flown). They are flown on the engine side's staging copy of v41 (`scene-staging.json`, re-planned
for this main: the streams' cuts are W116's), main's binary (4f40bd49's source) and this row's back
to back in one lease.

**The staging copy does not hold what the owner's world held at the worst place, so the drop is
not reproduced.** At the vantage the flights want 130,264 structures at the most where the walk
wanted 618,793 (21%), and nothing is dropped by either binary; the scene there is 8.55 million
pairs and 145,702 instances where the recording has 16.5 million and 358,000 at the same pose; no
`pair budget` line is logged (the walk: 1,586). Holding the first pose for 1,500 frames before the
flight changes none of it, so it is not the streaming's pace. What differs was not run down. Two
things are known: the staging flights log `the mesh room withholds tiles' meshes` (169 to 172
tiles at `renderer.meshes.budget_mib`, 240 MiB resident), a line the owner's log does not have at
all; and the owner arrived at the vantage by flying 1.2 km in two seconds over forest whose tiles
the world still held, where a flight starts there. So the numbers below are the staging's, and the
owner's 618,793 is read from the recording alone.

| the worst place, 420 frames, 3840 × 2160 traced with the bounce | main | this row |
|---|---:|---:|
| frame, median (cold start; after 1,500 frames held) | 17.04; 16.29 ms | 17.08; 16.32 ms |
| the chain (`rt`), median | 2.80; 2.81 ms | 2.81; 2.81 ms |
| of it the cluster builds (`clas`) | 1.84; 1.85 ms | 1.85; 1.85 ms |
| structures wanted at the most, and built | 130,264, all | 130,264, all |
| capacity steps in the flight | 2 (338,072 to 172,032 and back) | 2, the same |
| frames that paid for a step, and the worst | 2: 5.9 and 9.4 ms; 1: 11.1 ms | none; none |

(RTX 5090, 22:32 to 22:56 on 2026-11-12, each pair one lease; other agents' builds and tests ran
beside them and each flight began with 58 to 69 GB of commit free, so the milliseconds are upper
bounds and the pairs are what compares.) The chain in chunks costs the frame nothing that shows:
the capacity is six chunks, a frame of 60,000 to 130,000 structures fills one or two, and the
empty builds are within the hundredth of a millisecond the medians agree to. Both steps of this
row's flights were made on the worker and taken, and no frame's `cpu.resize` is above zero. This
card had room for main's swaps tonight, so main paid its allocations (6 to 11 ms) and not the
owner's wait and free; the owner's 66 to 254 ms are on a card without the room, which these
flights did not have to reproduce to remove: the path that paid them (a step that cannot be
swapped) is no longer taken by a step of the policy's.

**The walk's own flight was flown on main's side only.** Its 3,596 frames took main 1,213 s (a
frame's median is 21.2 ms, but the jumps stream tiles in and one frame in twenty is 0.8 s or more),
which outlasted the fourteen-minute lease, so this row's side was not started; the pair is ready to
fly (the hand-back has the command) and wants 25 minutes a side or the path resampled. What main's
side shows is the second symptom, on this card, with room to swap: 24 steps logged in the flight
(the capacity between 338,072 and 172,032 five times over, and the references), eight timed frames
that paid for one (six over 4 ms, 17.1 ms the worst, 80.6 ms in all), and three `resize was slow`
on steps that *were* swapped, 250 ms in making the bottom-level set and 315 ms in making a smaller
cluster set: the driver's allocation on the frame's thread, which is what the worker takes. The
flight wants 180,908 structures at the most, drops nothing, holds up to 15.4 million pairs and logs
no pair-budget line.

## Where the demand comes from

`engine-view --census` reads each frame's visible list back; with `renderer.rt.census_log` it also
counts every entry, and the shadow casters' run, by what the chain does with its instance and by
its ring (`VisibleCensus::count`, `GpuScene::rt_instance_kind`). The worst place's largest frame
(frame 351 of the flight, the second slope looked down; 130,264 wanted) and the frame with the
most casters (285):

| what the chain does with the entry | frame 351: drawn | casters | frame 285: drawn | casters |
|---|---:|---:|---:|---:|
| builds it this frame: deforming, traced (swaying trees and understory, the ground) | 9,601 | 2,679 | 5,569 | 29,187 |
| builds it this frame: deforming, **untraced** (the sward; built only because the picture is traced) | **117,024** | 0 | 46,520 | 0 |
| builds it this frame: rigid (untraced, or a mesh the cache does not keep) | 952 | 0 | 2,028 | 412 |
| references the resident cache | 7,656 | 84 | 48,920 | 12,276 |
| a rigid form's structure, built once | 33,850 | 7 | 111,731 | 101,159 |
| **the frame's own, by ring**: the prefix (the ground) | 2,667 | 413 | 3,472 | 5,223 |
| ring 0 (to 80 m) | 35,410 | 2,235 | 12,383 | 20,430 |
| ring 1 (80 to 192 m) | **89,030** | 31 | 36,847 | 3,561 |
| ring 2 (to 640 m) | 419 | 0 | 227 | 320 |
| ring 3 (to 1,280 m) | 51 | 0 | 1,188 | 65 |

**Nine tenths of what the frame builds is the sward, and two thirds of it stands 80 to 192 m
away.** The sward is untraced: no light's ray meets it, and under a rasterized picture it is not in
the structures at all. Under a traced picture it is the picture, so its drawn entries are built;
it sways (the field kind gives every instance its mesh's deform, in every ring), so they are built
from the pool every frame; and an untraced instance is never served by the cache (R110), so even
at rest they would be. The picture's own cut asks for 98% of the frame's structures at the first
view and 65% at the second; the casters are 2% and 35%, and they are the inner ring's trees. Who
asked for those casters (the cone test's back faces, the 24 m volume round the eye for the bounce,
the water's mirror and floor) was not split: the cull does not tell them apart and it takes a
flight with each off (R69 stage 8 measured the volume at 424 to 773 clusters a frame on this
world). A structure of the frame's own costs 9,156 bytes of capacity, 8,832 of them the driver's
reservation; what the builds occupy was not read back.

## The cuts

| cut | state | what it returns | what it costs |
|---|---|---|---|
| **The resize**: the set in chunks, a step made on the worker, room asked for what a step makes, a two-minute shrink window | built (this row) | no frame of the flights pays for a step (main: 6 to 11 ms on this card, 66 to 254 ms on the owner's); the walk's 36 capacity steps become 1 by the replay | 0.73 GB more capacity held on the walk on average; nothing measurable in the chain's time |
| **The sward past the inner ring stands still and is cached**: a field instance in a ring past the first takes no deform, and an untraced rigid instance under a traced picture references the cache | not built | at the worst place's largest frame the frame's own structures of rings 1 to 3, 89,500 of 130,264 (69%), become references; on the owner's view it is not known, since that view was not reproduced | the far sward's sway past 80 m (a card's tip moves a few centimetres, one to four pixels there at 3840 × 2160), and a change of state where a tile crosses the ring unless the sway fades to it; needs the ring in `write_slots`' choice to sway, the untraced instances in the cache's counts and `instance_cached`, and a look at how a tile that changes ring is rewritten |
| Casters the frame cannot use (a caster's level of detail by its shadow's size, casters past the light's reach) | not measured | at most the casters' share: 2% and 35% of the two frames above; `renderer.rt.caster_horizon` exists and was not flown | |
| The budget itself | not moved | 338,072 is the budget's (2,952 MiB over 9,156 bytes) and no second bound: the walk's view needs 5.66 GB of reservation, 7.1 GB with the headroom, on a card the forest fills; a MiB of budget buys 114 structures whatever they hold | |
| The reservation: structures at the size they are | not built | the forest's clusters are leaf cards of about 30 triangles reserved for 124; the rigid forms packed occupy a sixth of theirs (R143). The frame's set packed needs sizes before destinations, in the frame: a size pass or templates | a pass a frame, or templates for every deforming mesh |
| The pair budget: far placements counted as what their ring draws | not built (other rows' modules) | see below | |

## What the pair cap is spent on

The cap of 2^24 pairs is the owner's and does not move; what counts against it can. Each run's
summary has the scene's pairs by ring and by mesh at its last frame (`pair_breakdown`,
`pair_account.cpp`). Rings are tile rings of 1.25, 3, 10 and 20 tiles of 64 m: to 80 m, 192 m, 640 m
and 1,280 m.

| | the walk's last frame (v41, by the eastern shore) | v42 (`run-3-rt-sky`, its last frame) |
|---|---:|---:|
| pairs held, of 16,777,216 | 16,400,611 | 16,071,512 |
| the prefix and the ground rings' slots | 1,063,932 | 759,065 |
| ring 0: tiles, instances, pairs | 5, 1,803, 145,384 | 6, 1,254, 232,312 |
| ring 1 | 20, 18,612, 712,621 | 14, 4,698, 745,769 |
| ring 2 | 191, 38,178, 4,325,298 | 320, 132,110, 8,989,743 |
| ring 3 | 448, 125,087, 9,833,581 | 272, 98,695, 5,344,615 |
| holes in the tail | 319,795 | 0 |
| of the tail: rigid forms built once (`static_form_pairs`) | 4,062,966 | 4,800,370 |
| of the tail: untraced (`untraced_pairs`) | 10,372,531 | 9,808,411 |

**Four meshes hold most of it.** (Leg B named them by path: they are the litter's, not the
sward's cards as this paragraph read them; [below](#what-a-far-placement-draws).) Four meshes of 245, 191, 102 and 55 clusters head every ring's
list, each placed as itself (`mesh == source_mesh`) in all four rings, thousands of times. They are
read here as the `sward` generator's cards — it places a patch as a near form and a card
(`distance.clusters`) for each family at four densities, and the scene's untraced pairs, which are
the sward's and the scatter's, are 10.4 million, what these four hold — but no run of this row named
them by path; the summary's mesh table would. Whatever they are, one of them 1 km away reserves as
many pairs as one at the eye:

| the four cards | instances | pairs | share of the scene's pairs |
|---|---:|---:|---:|
| v41, ring 2 (192 to 640 m) | 18,154 | 2,671,905 | 16.3% |
| v41, ring 3 (640 to 1,280 m) | 48,913 | 7,262,069 | 44.3% |
| v42, ring 2 | 30,291 | 4,448,644 | 27.7% |
| v42, ring 3 | 25,844 | 3,833,751 | 23.9% |

Beside them an authored instance costs its ring's distance form: mesh 8442 is the ring-3 form of
mesh 3186, 64 clusters where its source has more, 5,475 instances and 350,400 pairs, cut from the
top of its source's DAG for the nearest eye its ring allows (`extract_lod_form`); a generator's
placement has no such form. **What a far placement should cost is what its ring can draw of it**:
the same cut, taken of a generator's mesh per ring, as an authored mesh has. How many clusters
that leaves a card of ring 2 or 3 was not measured (it needs the forms cut, or a census of a view
that holds those rings whole; the flights below held 8.5 million pairs and their far rings drew
470 of the frame's own structures). The bound is known: with the four meshes' pairs of rings 2
and 3 taken out altogether the same tiles hold 6.5 million pairs in v41 and 7.8 million in v42,
under half the cap, so every tile withheld on the walk (up to 1,002, about 22,000 pairs a ring-3
tile, 16,000 of them these four) would fit beside them; and each far instance left at even ten
pairs adds 0.67 million in v41 and 0.56 in v42. The work is in the placement path
(`systems/world`, `scene_gen`'s generators and the form cut in `scene.cpp`), which is other rows'
tonight; it is left as a row.


## Leg B (2026-11-13): the demand itself

The first leg removed the resize and counted the demand; this one is about what the frames build.
Three things came of it: why the staging copy cannot show the owner's drop, two rules that take
most of a traced frame's own structures out of it — [the far field at rest](#the-far-field-at-rest)
and [untraced rigid instances in the cache](#untraced-rigid-instances-reference-the-cache) — and
[what a far placement draws against the pairs it holds](#what-a-far-placement-draws).

### What the staging copy does not hold of the owner's demand

At the vantage the staging flights hold 8.55 million pairs and 145,702 instances where the
recording has 16.5 million and 358,330, and want 130,264 structures where the walk wanted 618,793.
What was tried, and what the files say (the proof of concept's run as text: `run.json`,
`scene.json`, `stderr.log`, `frames.jsonl`):

- **Not the mesh room.** The staging flights log `the mesh room withholds tiles' meshes` (a
  warning: the owner's log, which is warnings only, would have it). Flown on main with
  `renderer.meshes.budget_mib=2048` (a room of 815,913 clusters, no tile withheld): 9,079,737
  pairs at the most, 131,124 wanted. The room's 170 withheld tiles are half a million pairs.
- **Not how the eye got there.** The first leg held the pose 1,500 frames; the same.
- **Not the launcher's settings.** `run.json`'s arguments are the flights' (3840 × 2160, `--raster
  rt --shadows rt`, `--lod 1`, `--deform-pool-mib 64`, `--rt-budget-mib 0`, the wind at 0.7,
  micromaps, the bounce) but for what a session adds: the window (`hdr10`, uncapped), the game
  module with its 416 animals, the player, the input record.
- **Not the scenes' entries at that place.** The lab run's scene names the same sward entry (seed,
  extent, the sixteen families at four densities; cooked as `forest-v41-33ff2824` where the
  staging's is `forest-v41-b8eb339a`), the same two litter entries, eighteen wetland entries and
  the channels, and beside them `routes`, `walls`, `bridges`, `caves` and five `scatter` entries
  the staging does not have — the scatters are boulder fields in five polygons by the lake, none
  near the vantage.
- **What differs is what stands in the far rings.** Along the walk the staging holds as many
  instances as the recording (366,606 at the walk's first frames against 358,330 at the vantage)
  in half the pairs: 24 an instance against 46. At the worst path's last frame its second ring
  holds 4,447 instances and 242,999 pairs in 168 tiles and its third 10,712 and 534,395 in 395;
  the recording's last frame holds 38,178 and 4,325,298 in 191 tiles and 125,087 and 9,833,581 in
  448. The difference is the **litter**: the four meshes that head every ring of the recording
  ([below](#what-a-far-placement-draws)) stand 13,606 and 36,766 times in its second and third
  rings by their three largest, and 572 and 1,858 times in the staging's.

Why this main places a twentieth of the litter in the far rings there was not run down: the litter
generator reads the ground's masks through a `substrate` index (5 and 8 in the two entries), this
main's masks are version 4, and the staging's are the migration's. It is the masks' and the
litter's rows' to say, and until it is said **no flight on this main shows the owner's 618,793**:
the staging comes to 52 to 55% of the pairs, 41% of the instances, 34% of the visible pairs
(250,241 of 730,726) and 21% of the structures wanted. Every number below is the staging's.

### The far field at rest

The rule is on [the renderer's page](../subsystems/renderer.md#the-far-field-at-rest): past
`renderer.flora.field_rest_m` (120 m) a field's blades take no wind, the sway falling to nothing
over the 40 m before it; an instance wholly past it takes no pool block, so every reader draws it
from the grid; and the chain's records treat it as the rigid instance it is that frame, so its
entries reference the resident cache.

### Untraced rigid instances reference the cache

The same census that named the sward named a second builder the staging barely holds at the
vantage and the owner's world is full of: the **litter**, rigid and untraced. An untraced
instance's drawn entries are in the chain only under a traced picture (R110), and R110 left them
out of the cache, so every litter pair in view was built every frame. Nothing about the cache
needed that: a cached structure is every instance's of its mesh, and what keeps a light's ray off
an untraced instance is its own top-level record's mask. So `instance_cached` now serves an
untraced rigid instance in a traced-primary frame (`clas_records.slang`; `GpuScene::
rt_instance_cached` and the cache's `served` test on the CPU, by the scene's path), and the
census counts it as `untraced_cached`. In a rasterized frame nothing changes: none of its entries
reach the records.

### The pair at the worst place

`worst-path.json` (420 frames, the vantage and the slope below it), 300 frames held first, main's
binary (4f40bd49's source) and this row's back to back in one lease (2026-11-13 00:28 to 00:38;
RTX 5090; a debug build of this row finished three minutes before the lease and nothing of this
row ran beside it, other agents' work did: the milliseconds are upper bounds and the pair is what
compares). Medians over the flight:

| | main | this row |
|---|---:|---:|
| frame | 17.06 ms | 15.66 ms |
| GPU total | 17.09 ms | 15.70 ms |
| the chain (`rt`) | 2.79 ms | 1.88 ms |
| of it the cluster builds (`clas`) | 1.85 ms | 0.95 ms |
| the bottom-level build (`blas`) | 0.33 ms | 0.28 ms |
| the pool pass (`deform`) | 0.395 ms | 0.376 ms |
| the cull pass | 0.17 ms | 0.20 ms |
| the primary rays (`trace`) | 2.38 ms | 2.36 ms |
| the resolve | 8.45 ms | 8.40 ms |
| structures wanted, median; at the most | 88,348; 130,264 | 39,967; 77,152 |
| built | all | all |
| references to the cache, median; at the most | 7,772; 61,190 | 71,898; 99,455 |
| visible pairs, casters, the scene's pairs | 155,373; 11,643; 5,539,161 | the same, every frame |
| frames whose GPU total is over the other's by 0.5 ms | | 2 of 419 |

The cut is the same in every frame (the cull pass is untouched: an instance at rest keeps its
padding and is never cone-tested), so the visible list and the casters are main's to the entry.
What changed is who builds: at the flight's largest frame on main (frame 351, the second slope)
the frame's own structures go from 130,264 to 63,049 and the references from 7,740 to 74,955; at
frame 245, the row's own largest, from 119,206 to 77,152. By the census's kinds, this row, frame
378 (main's census of the same place is the first leg's table above):

| what the chain does with the entry | frame 378, drawn | casters |
|---|---:|---:|
| builds it this frame: deforming, traced | 9,418 | 2,535 |
| builds it this frame: deforming, untraced (the sward short of the distance) | 52,405 | 0 |
| builds it this frame: rigid | 296 | 1 |
| references the cache: traced | 7,495 | 85 |
| references the cache: untraced (**the sward at rest, and the litter**) | 64,700 | 0 |
| a rigid form's structure, built once | 33,306 | 7 |
| the frame's own by ring: prefix; ring 0; ring 1; ring 2; ring 3 | 2,653; 35,343; 23,857; 218; 48 | 428; 2,092; 15; 1; 0 |
| the references by ring: prefix; ring 0; ring 1; ring 2; ring 3 | 7,495; 383; 64,124; 190; 3 | |

So two thirds of the second ring's sward (64,124 of 87,981 entries) stands past 120 m and is no
longer built; what the frame still builds is the sward inside the distance (35,343 in the inner
ring, 23,857 in the second) and the swaying trees. **The memory**: the frame's set is sized by the
capacity, which the policy holds a step above the demand, so a demand of 77,152 where it was
130,264 is 53,112 structures, 486 MB of capacity at 9,156 bytes, the chain no longer has to hold;
on these flights the capacity was at the budget's 338,072 either way (it starts there and the
two-minute window had not passed). The references cost 99,455 where they cost 61,190, at tens of
bytes each. **The cache is full on both sides**: its 60,074 slots (506 MiB at 8,832 bytes a slot
with micromaps) are all occupied on main and on this row, and a frame's misses past them are
built in the frame's own set — 160,856 such builds over main's 720 frames and 282,808 over this
row's, 223 and 393 a frame, which are in the "wanted" figures above. The sward's cards and the
litter are a few hundred distinct clusters; what fills the cache is the inner rings' rigid meshes
(48,920 references at frame 285 on main). A cache that held the scene's distinct clusters would
give those builds back too; its slots at the size they are (R143's next step) is the row for it.

**The target, stated.** On the staging at the owner's vantage `built == wanted` already on main
(130,264 of a budget of 338,072), and with the two rules the frame wants 59% of that at the most.
At the owner's demand it cannot be flown (above). What the recording says of it: of 730,726
visible pairs the chain wanted 617,554 and referenced 14,441, so about 100,000 were static forms
and **six in seven of the visible entries were built that frame**: deforming or untraced. Every
untraced rigid entry of those (the litter: 10.4 million of that world's 16.4 million pairs) and
every sward entry past 120 m now references the cache; what stays in the frame's set is the
swaying trees of the inner ring, the ground, and the sward within 120 m, which on the staging is
63,049 at the worst frame. Whether that is under 338,072 on the owner's world is what a flight
there has to say; the bound from this side is that the staging's own demand fell by 52% and the
kinds that fell are the kinds the owner's world has 2.5 times more of.

### What a far placement draws

The cap of 2^24 pairs is the owner's and does not move. The first leg found four meshes of 245,
191, 102 and 55 clusters placed as themselves in every ring and read them as the sward's cards.
**They are the litter**: the first `litter` entry's four meshes, by path
`litter-v22-tangle.litter.json` (245 clusters in the owner's cook, 251 in the staging's),
`litter-v22-leaf` (191), `litter-v22-bark` (102) and `litter-v22-twig` (55; 58) of the proof of
concept's `world/v22/recipes`, each a 1.25 m patch of fallen leaves, bark, twigs or tangle laid
in 4 m cells, up to 256 cells a tile (the staging's load log names every mesh with its source
and cluster count, and the scene's entry lists the four in this order; the ids are consecutive,
3914 to 3917 in the recording and 5914 to 5917 in v42). The sward's cards are three and four
clusters and stand in the first two rings only. What each ring holds, from the two summaries'
`pair_breakdown`:

| | recording's last frame (v41) | | | v42 `run-3-rt-sky` | | |
|---|---:|---:|---:|---:|---:|---:|
| ring | litter instances | their pairs | of the ring's pairs | litter instances | their pairs | of the ring's pairs |
| 0 (to 80 m) | 513 | 75,085 | 52% | not in the six largest | | |
| 1 (to 192 m) | 1,697 | 249,037 | 35% | 1,195 | 169,715 | 23% |
| 2 (to 640 m) | 18,154 | 2,671,905 | 62% | 30,291 | 4,448,644 | 49% |
| 3 (to 1,280 m) | 48,913 | 7,262,069 | 74% | 25,844 | 3,833,751 | 72% |
| all four | 69,277 | 10,258,096 | 63% of the scene's 16,400,611 | 57,330 | 8,452,110 | 53% of 16,071,512 |

A litter patch 1 km away holds the 55 to 245 pairs it holds underfoot: 148 pairs an instance in
every ring. Beside it an authored tree in the third ring holds its distance form (32 pairs an
instance over that ring's 75,839 form instances in the recording, 20 in v42), and a sward card
three.

**What the cut draws of one.** Not measured where it matters: `renderer.rt.census_mesh_frame`
(new: on one census frame, per ring, the meshes that draw the most entries, with the instances
drawn and the most one drew) was flown on the held view below, and on the staging the litter is
in no ring's ten most-drawn meshes there (the section above). What the same frame shows of other
placements: a sward card draws its 3 of 3 clusters in the second ring (4,638 instances, 13,896
entries); a tree's ring form its 64 of 64 (a static form's frontier is drawn whole); the inner
ring's sward patches 75 of 268 an instance (9,252 entries over 123 instances, 82 at the most).
What the litter's containers say: a patch is 0.7 m across (`radius_m` 0.35), 7 pixels at the
second ring's inner edge and 2 at the third's at 3840 × 2160, and its DAG has 6 to 8 levels over
24 to 147 leaf clusters, so the cut that far is the top of the DAG: a few clusters where the
placement holds 55 to 245. The arithmetic below takes **four**, and says what each one more costs.

**What the cap would reach.** With a far litter patch held as four pairs, the same tiles hold:

| | recording's last frame (v41) | v42 `run-3-rt-sky` |
|---|---:|---:|
| pairs held today, of 16,777,216 | 16,400,611 | 16,071,512 |
| the litter of rings 2 and 3, today | 9,933,974 (67,067 instances) | 8,282,395 (56,135) |
| the same at four pairs an instance | 268,268 | 224,540 |
| the scene then | 6,734,905 | 8,013,657 |
| each pair more a far patch holds | 67,067 | 56,135 |
| a ring-2 tile, today; then | 22,646; 9,037 | 28,093; 14,570 |
| a ring-3 tile, today; then | 21,950; 6,177 | 19,649; 5,935 |

So the walk's world would hold 40% of the cap where it holds 98%, and v42's 48% where it holds
96%. The rings as they are set — every tile whose centre is within 20 tiles, 1,257 of them, 286
in the second ring's annulus and 942 in the third's — would cost the recording's world 1.06
million fixed, 0.86 million for the first two rings, 2.6 million for a full second ring and 5.8
million for a full third: 10.3 million, with 6.4 million of the cap unspent, where today a full
third ring alone would be 20.7 million. Every tile the walk withheld (up to 1,002 at once) fits, and so do the 905 the next world withheld on its island views (item 69 of
the forest's page); the unspent 6.4 million is a further 1,040 tiles at the third ring's density,
a disc of 27 tiles: **the cap reaches 1.7 km where it reaches short of 1.28 km today.**

**The design: a placement's ring forms.** An authored instance already costs what its ring draws:
past the first ring it is placed as a distance form, a mesh cut from the top of its source's DAG
for the nearest eye its ring allows (`extract_lod_form`, `stream_authored`), rigid, and under the
chain a static form whose structure is built once. A generator's placement names a mesh and the
world places that mesh in every ring. The same cut taken of a generator's meshes — per mesh and
ring, at load, since a generator's meshes are the scene's own (`SceneData::parts`, with
`form_source` and `form_rigid` beside them as for an authored form) — gives a placement its ring
forms: the placements consumer names `form(mesh, ring)` where it names `mesh`, the pair account's
`B(tile)` counts the form's clusters, and a rigid form being a static form, a far litter patch
then costs the frame's set and the cache nothing either. What it costs: a form a ring for each
placed mesh (eighteen for the litter's six), the hand-over at a ring's edge (a form cut for its
ring's inner edge draws there what the source draws there, R91's measure for trees), and R94's
price that a form is drawn whole wherever in its ring it stands. A field mesh's card needs none:
it is three clusters already. The generators' placement path (`systems/world`, `scene_gen`) is
other rows' tonight, so this is the count and the design, and a row.

### What the picture lost

Stopping the far field's sway is a change to the picture, not a cull, and this is its measure.
The worst place's largest frame held (`hold-path.json`: the eye at (-1445, 491, 3903) looking
along the slope, 300 frames of warm-up, then six captures a quarter second apart from the fourth
second, the id words read back beside each), this row's binary with the rule off and on, one
lease (2026-11-13 00:59 to 01:09). Every pixel of the 8,294,400 is put in one class by its id
words (instance, cluster, triangle) over the twelve captures:

| class | pixels | share | colour over 1.25 s, rule off | rule on | on against off, same frame |
|---|---:|---:|---:|---:|---:|
| still: the same in every capture of both runs | 6,478,408 | 78.11% | 0.62 | 0.62 | 0.24 |
| moving: changes over the on run (near grass, trees) | 1,673,871 | 20.18% | 23.60 | 23.91 | 2.07 |
| **stilled**: still with the rule, moving without it | 124,967 | 1.51% | 26.28 | 0.33 | 21.03 |
| leaned: still in both, not the same between them | 17,154 | 0.21% | 0.44 | 0.15 | 48.72 |

(Colour: the mean absolute difference of the three 8-bit channels, the first capture against the
last; the still class's 0.62 is the bounce's own noise.) **What the picture lost is the motion of
1.5% of its pixels**: the sward past 120 m, 12,000 instances in this view, whose pixels changed
as much over a second as the near grass's do (26 of 255) and now do not change. That far the
motion is a blade's or a card's edge crossing a pixel's centre — the tip's travel is about a
pixel — so what was there to lose reads as a shimmer on the far slopes rather than as grass
bending; it is gone, and the far slopes are as still as a rigid forest's far rings already are.
The 0.21% "leaned" are far blades whose mean lean in the wind is no longer drawn: a pose that
differs and does not move. **Where nothing moved the ids are identical**: the still class is the
same words in all twelve captures, and by rows the stilled and leaned pixels lie in the second to
seventh twelfths of the picture — the far slopes — with none in the lower five, the ground to
about 100 m, where the on run is the off run to the id in every capture.

**No step at the boundary, in space or in time.** By construction a blade's last posed frame is
its rest pose (the share is zero a quarter metre short of the distance, and a blade with no angle
returns the grid's position). On the captures: the stilled class thickens gradually up the far
slopes from the moving grass below it, with no line where the band ends (the class picture), and
the pixels whose ids change between consecutive captures are steady over the on run — 975,357,
982,398, 970,760, 960,702 and 943,285 — as they are over the off run (1,039,701 to 1,007,835),
about 64,000 fewer each quarter second: the far sway, and no capture apart from the others.

**The default** is the staging's evidence, and the owner's to overrule: 120 m with a band of 40 m
gives back 41 to 52% of the frame's own structures and a millisecond of the chain at the worst
place for the motion of 1.5% of the picture's pixels there. `renderer.flora.field_rest_m=0` keeps
every blade swaying; a larger distance trades the two (at 3840 × 2160 the strongest flutter's
travel is under a pixel from 139 m, the typical blade's from 100 m).

Pictures (opened): the on run's first capture,
`<scratch>/runs/b1-hold-on/markers/00240-hold-240.png`, and the classes,
`<scratch>/runs/classes.png` (still black, moving grey, stilled red, leaned yellow), `<scratch>`
being the coordinator's `scratchpad\b63\R152b`.
