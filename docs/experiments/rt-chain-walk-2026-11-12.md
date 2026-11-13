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

**Four meshes hold most of it.** Four meshes of 245, 191, 102 and 55 clusters head every ring's
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

