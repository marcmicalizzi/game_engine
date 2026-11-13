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

**Four meshes hold most of it, and they are the sward's far form.** The `sward` generator places a
patch as a near form and a card (`distance.clusters`), and the cards of its dominant family at four
densities are the meshes of 245, 191, 102 and 55 clusters that head every ring's list. They are
placed as the same mesh in every ring, so a card 1 km away reserves as many pairs as one at the
eye:

| the four cards | instances | pairs | share of the scene's pairs |
|---|---:|---:|---:|
| v41, ring 2 (192 to 640 m) | 18,154 | 2,671,905 | 16.3% |
| v41, ring 3 (640 to 1,280 m) | 48,913 | 7,262,069 | 44.3% |
| v42, ring 2 | 30,291 | 4,448,644 | 27.7% |
| v42, ring 3 | 25,844 | 3,833,751 | 23.9% |

Beside them an authored tree costs its ring's distance form: a spruce or a shrub of ring 3 is a
form of 64 clusters (`mesh 8442 (source 3186)`, 5,475 instances, 350,400 pairs), cut from the top of
its source's DAG for the nearest eye its ring allows (`extract_lod_form`), and a generator's
placement has no such form. That is the whole of the withholding: with the far cards counted as
what a ring can draw of them the same tiles hold about 6.5 million pairs in v41 and 7.8 million in
v42 (the far two rings' cards at their drawn clusters, below), and nothing is withheld.

