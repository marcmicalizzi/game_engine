# Compact traced records and one pair account (R100, 2026-10-26)

R94's fixed distance forms and R95's untraced grass exposed two older counts: the sparse
visible-list index range charged three runs, including pairs the chain never builds, and one
65,536-cluster memory query amortized fixed instance slots as if they were a per-cluster cost.
The placements consumer also omitted the terrain slots and block reservations from admission.

The implementation and formula are in [renderer](../subsystems/renderer.md#the-pair-accounting)
and [world](../subsystems/world.md#the-consumers). Compact records carry a record-to-visible-entry
map, preserving each hit's pair, caster classification and deformed pool entry. Static hits
retain mesh-local indices. The budget is the frame capacity's marginal bytes; scene-sized and
static storage is beside it and remains in the reported total. The default stays 1,024 MiB.

## Inputs and method

Read-only inputs: forest POC `reports/quality-v13/HANDOFF.md`, exact run
`reports/quality-v13/runs/grass-clearing-rt-wide-20261026-084719`, and v14's
`HANDOFF.md` streaming configuration. v13 has 32 m cells in rings 1.5/6/20/40 (48/192/640/1280 m),
4,000 mm terrain spacing and 115,629 grass instances. Its engine 816513e7 report has casters
disabled, wanted 118,597 versus limit 98,816, 80 overflow frames and 138,560 dropped-instance
events, 1,384,296,979 RT bytes including 154,052,888 static bytes. These are the owner's input
numbers, not the before run below. v14's 64 m cells halve the ring radii and keep their physical
reach; this task does not change that authoring decision or make an oversized immutable prefix fit.

Own baseline: c6527c71, `msvc-release`, engine-view copied to scratch before code changes.
Own after: this worktree's rebuilt executable. The v13 cook and sward cook were copied to
`D:/workspace/ge-codex-scratch/astra/poc-v13`; all reports and captures stay in scratch.
Both runs use hardware raster, traced shadows, 11520x2160, 180 measured frames, the same clearing
hold path, `--terrain-tiles --terrain-far 6 --time-rate 0 --world-rate 1 --no-lights
--time-of-day 15 --lod 1 --deform-pool-mib 64 --rt-budget-mib 1024`, flora motion at wind 0.7,
`--wait-quiet 120`, and the machine-wide GPU lock. RTX 5090, NVIDIA 617.42, Core i9-10980XE,
Windows 10.0.26200. Times are upper bounds when the recorded machine state is shared.

## Diagnostic baseline

| v13 run | casters | wanted | steady limit | overflow frames | dropped events | RT bytes | static bytes |
|---|---|---:|---:|---:|---:|---:|---:|
| own before | off | 118,597 | 98,816 | 198 | 342,936 | 1,384,296,979 | 154,052,888 |
| intermediate budget probe, casters still off | off | 118,597 | 170,977 | 0 | 0 | 1,839,454,435 | 154,052,888 |

Overflow totals include setup and capture frames as well as the 180 measured frames; they are
not interchangeable with the owner's 80-frame count. Both own runs have all four placement
rings, 3,760,093 live tail pairs, zero withheld tiles and zero renderer refusals.
The intermediate probe establishes the memory correction only: it still used the old caster
admission and is not evidence of a completed caster fix.

The old effective price was 10,866 bytes/cluster, the corrected marginal price 6,280:
`floor(1,073,741,824 / 6,280) = 170,977`. Static storage was never deducted from that 1 GiB;
the defect was fixed per-instance storage divided by the single probe. A no-caster complete
118,597-record frame needs 744,789,160 marginal bytes, or 711 MiB rounded up. That is below the
unchanged default. Total resident RT bytes are larger because the scene storage is reported too.

Baseline machine_state: other CPU 8.885% -> 4.196%, GPU 1% -> 100%, GPU memory
17,456 -> 18,314 MiB, GPU lock mine. Intermediate: other CPU 5.789% -> 6.435%, GPU 2% -> 1%,
GPU memory 17,428 -> 18,622 MiB, lock mine. No claim of quiet comparative timing follows from them.

## Caster-on v13 result

The corrected build keeps 15,283 caster entries and builds all 123,404 wanted records on every
measured frame. Static and untraced entries account for the difference between caster entries
and additional per-frame records. There are zero caster-index fallbacks, overflows, dropped
instances, caster drops, withheld tiles, refusals, growths or shrinks. The first capacity is
170,977, equal to the steady limit: R94's first-capacity rule holds with all four rings.

The compact set's maximum geometry index is 170,976 against 16,777,215. For the live scene,
`6,925,287 + 3,760,093 - 1,208,168 - 2,165,915 = 7,311,297` eligible pairs bound the frame.
Its allocated pair stride is 16,322,560; including its unused slots gives the conservative first
estimate `16,322,560 - 1,208,168 - 2,165,915 = 12,948,477`. Holes can reserve scratch and
influence this estimate but emit no records. The actual cut wants only 123,404.

A complete caster-on frame needs `123,404 * 6,280 = 774,977,120` marginal bytes, **740 MiB**
rounded up. The 1 GiB default is adequate; the old count was wrong. RT resident storage is
1,839,454,435 bytes, including unchanged static storage of 154,052,888, because fixed scene
storage is beside the marginal frame budget. The correction increases capacity and resident
bytes instead of silently hiding a drop. No default was raised.

| metric | own before | caster-on after |
|---|---:|---:|
| wanted / steady limit | 118,597 / 98,816 | 123,404 / 170,977 |
| caster entries | 0 | 15,283 |
| overflow frames / dropped instance events | 198 / 342,936 | 0 / 0 |
| RT bytes | 1,384,296,979 | 1,839,454,435 |
| cull median / p95 ms | 0.284 / 0.286 | 0.292 / 0.294 |
| deform median / p95 ms | 1.347 / 1.554 | 1.398 / 1.644 |
| hardware raster median / p95 ms | 8.379 / 8.556 | 8.427 / 8.445 |
| CLAS median / p95 ms | 1.879 / 2.062 | 2.598 / 2.721 |
| BLAS median / p95 ms | 1.567 / 1.585 | 1.561 / 2.217 |
| RT chain median / p95 ms | 100.095 / 100.852 | 5.512 / 6.353 |
| resolve median / p95 ms | 11.557 / 11.979 | 17.733 / 17.932 |
| sky median / p95 ms | 0.053 / 0.253 | 0.054 / 0.054 |
| total GPU median / p95 ms | 126.179 / 127.432 | 38.199 / 38.974 |

Trace, software raster, Hi-Z, cascaded shadows and median table upload times are zero in this
hardware/traced-shadow case. The complete chain avoids the serial overflow selection; the
resolve now traces the additional casters and costs more. The changed shadows are intended,
so this case is not a byte-identity comparison against the caster-disabled baseline.

After machine_state: other CPU 17.376% -> 24.232%, own CPU 0.341% -> 0%, GPU 4% -> 100%,
GPU memory 17,462 -> 18,656 MiB, lock mine, session unlocked. The 120-second quiet wait expired;
a debug configure/build and cook copy shared the machine. Timings are upper bounds, not isolated
costs. The baseline machine state is above.

## v8 crossing with casters

The full v8 `world/v8/base.json`, with 32 m cells and radii 1.5/6/40, uses copied v8 and v7 cooks.
Both own executables fly 1,800 frames at 30 fps over the same 60-second straight path from
(-950, -700) to (950, 2550), at endpoint heights 44.915 and 178.192 m (source `region.f32`
heights + 40 m), looking forward and 12 m down. Hardware raster and traced shadows, 1600x900,
the same flora/time/terrain flags as v13, no `--no-shadow-casters`. No POC script or binary ran.

| crossing metric | own before | after |
|---|---:|---:|
| measured frames / frames drawing only prefix | 1,800 / 1,213 | 1,800 / 0 |
| drawn pair range | 3,264,501–5,587,839 | 4,745,141–10,580,453 |
| placement refusal events | 5,384 | 0 |
| largest logged withheld count | 1,737 | 0 |
| fixed reservation / terrain slots | 3,264,501 / 621,840 | same |
| consumer pair ceiling | 5,592,405 | 16,777,216 |
| wanted max / mean | 21,616 / 10,108.3 | 40,072 / 17,470.9 |
| steady capacity ceiling | 150,447 | 166,937 |
| capacity end / peak | 24,576 / 150,447 | 24,576 / 166,937 |
| overflow frames / drawn drops / caster drops | 0 / 0 / 0 | 0 / 0 / 0 |
| RT bytes end / peak | 574,766,348 / 1,374,330,624 | 638,767,800 / 1,552,205,704 |
| static bytes | 176,117,408 | same |
| growths / shrinks | 4 / 2 | 4 / 2 |

The last camera has only 690,731 live tail pairs in both runs, so that final summary alone hides
the defect. The per-frame log shows it: before, most frames have exactly the fixed prefix;
after, every frame has a tail. The restored tail raises wanted records and raster/shadow work;
the baseline's missing forest is not a cheaper complete rendering. The after set's maximum
compact geometry index is 24,575 at the end, 166,936 at peak, against 16,777,215.
Its marginal charge is 6,432 bytes/cluster (the per-scene driver query, not a pinned GPU constant).

| GPU pass, median / p95 ms | before | after |
|---|---:|---:|
| cull | 0.492 / 0.736 | 0.211 / 0.780 |
| deform | 0.133 / 0.242 | 0.196 / 0.571 |
| hardware raster | 0.064 / 0.301 | 0.328 / 1.406 |
| CLAS | 0.278 / 0.494 | 0.371 / 0.759 |
| BLAS | 0.435 / 0.529 | 0.340 / 0.767 |
| RT chain | 1.168 / 1.593 | 1.432 / 1.970 |
| resolve | 0.610 / 1.233 | 1.241 / 2.038 |
| sky | 0.162 / 0.169 | 0.054 / 0.147 |
| table upload | 0.206 / 0.317 | 0.182 / 0.281 |
| total | 3.809 / 5.725 | 5.410 / 9.794 |

Trace, software raster, Hi-Z and cascaded-shadow passes are zero. Before machine_state:
other CPU 40.625% -> 33.507%, GPU 3% -> 16%, memory 16,350 -> 15,827 MiB. After:
other CPU 9.760% -> 10.018%, GPU 3% -> 19%, memory 16,447 -> 16,520 MiB. Both own CPU
samples are 0%, both locks mine and sessions unlocked. The after run passed its quiet wait;
the end sample still makes these upper bounds. The Windows build and Linux build overlapped
parts of the measurement period. Four growths during travel remain; this task does not change
capacity hysteresis or remove the cost of a finer cut after the shrink window.

## Validation

Release CPU checks: shared pair and compact-index cases, 2 cases / 30 assertions; placement
budget and layout agreement over five moves, 1 case / 242 assertions. The new four-ring
hardware/traced-shadow offscreen case passes 32 assertions: static forms present, casters kept,
zero withheld tiles/refusals/overflow at the normal budget, and counted drops without a scene
refusal at 1 MiB. These targeted checks precede the required debug affected and Linux runs.

The excluded-pair count also refreshes when the last tile is trimmed without writing surviving slots. A dedicated removal test checks that no stale untraced pairs remain in the next frame estimate. This does not change the captured forest runs.

The final Linux Clang debug filter (`renderer|world|gfx|authored|engine_view|docs|lint`) passes
14/14 CTest entries after the removal fix; its checkout volumes were pruned (3.66 GB).
This container verifies compilation and CPU paths, not desktop traced rendering.
The desktop debug gfx module passes 102 cases / 419,418 assertions, including CLAS/KHR/raster
comparisons; renderer passes 259 cases / 4,148,349 assertions, including the compact chain's
colour/id comparisons and the last-tile regression; world passes 39 cases / 72,108 assertions.
No image pin changed. The debug affected run passes 58/58 CTest entries in 3,386.79 seconds;
engine-view passes 139 cases / 46,161 assertions. The frame-loop gate passes 4 cases / 2,353
assertions: desert-erg and desert-endless have zero second-flight allocations and device buffers.
The existing streamed-tree comparison reports 308 CPU allocations with sway both on and off,
and zero device buffers; it checks equal allocation counts, not zero. That existing world
streaming allocation debt is not closed by R100. The desert-overlook input is unavailable
(`content/samples/Lantern/Lantern.glb`), so its flight is skipped. Early targeted sandbox runs
could not write the shared GPU lock; these results are superseded by this elevated, locked
affected run. A second affected run is required after the final documentation commit.
