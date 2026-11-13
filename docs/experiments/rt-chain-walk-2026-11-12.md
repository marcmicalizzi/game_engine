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

**The pair budget withholds through most of the walk.** 1,586 lines (one each time the count of
withheld tiles changes), 1,518 naming ring 3 and 68 ring 2; up to 1,002 tiles withheld at once. In
the forest the scene holds up to 16.77 million pairs of the 16,777,216 the visibility id names; on
the open mountain, 2.4 to 9 million.
