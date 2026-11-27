# The water's step on the card: what it costs, and whether it can be the same bytes everywhere (W175)

Roadmap W175, under W173 (the water's design and its decision record). The capability is [water_live](../subsystems/water_live.md): [water_state](../subsystems/water_state.md)'s reference scheme as a compute shader in single precision, held to the reference's cases. This page has what the step costs at three grids and three states, what it moves against the card's bandwidth, what a 60 Hz frame keeps, what single precision does to a lake at rest and to a slow exchange, and what it would take for the step to be the same bytes on every device.

**Machine.** This box: an RTX 5090 (driver 621.24, Vulkan 1.4.357), shared on 2026-11-25 with other rows, one of them flying the forest at 4K on the same card. Every number below carries the state the bench harness recorded around its run.

## The measurement

`engine_water_live_bench` (`msvc-release`): each variant runs batches of 16 steps of one state at one grid, every pass a `gfx::GpuTimer` zone, and prints the median and p95 of a step's GPU time over every step measured after a warm-up, the batch's own span over its 16 steps, and the mean step length the state allowed. Cells are 0.5 m. The states, put back every 1,024 steps so each stays the state it names:

- **lake**: a still lake a metre deep over a flat bed, closed edges, Manning's n 0.025;
- **stream**: a reach falling 2 % east with a valley across it, 10 cm of water at 1 m/s in the middle half of the cross-section and the rest dry, open at both ends, n 0.04;
- **beach**: a plane beach from −2 m to +1 m, still water at 0 with the offshore 30 % raised 0.5 m, so a bore runs up the slope and wets the upper beach, n 0.025.

Three forms: `step` (each cell computes its own four faces, the default), `faces` (every face computed once into a buffer, then the update), `fixed` (the default's faces with division, square root and cube root built from + − × alone).

**The three states are not uniform workloads.** In the lake every face takes the identical-state shortcut (both sides the same state to the bit), so its faces take no square root and no division; in the stream whole workgroups of dry cells skip the friction; no state runs a source or the compensated sum. **The beach is the honest budget case**: wet and dry cells, a moving bore, every face's full flux where the water is. And the card's step is not yet the reference's whole step: it carries no carried quantities and computes none of the reference's per-cell outputs (the speed, the bed stress, the dissipation), which would add 12 to 16 bytes a cell a step and their arithmetic; the bytes and times below are those of depth and momenta, not of the full state.

**Machine state** (the harness's `machine_state`, run of 2026-11-25 23:34 local): other processes at 45 % of the CPU at the start and 71 % at the end; the GPU 2 % busy at both samples (6.9 of 32.6 GB in use), its SM clock 480 MHz at the idle start and 2,617 of 3,090 MHz at the end; **the GPU lock held by others at both samples** — another row's water tests at the start, a 4K capture session at the end. The bench took the lock after 38 minutes in the queue, then waited the 30 minutes `--wait-quiet` allows for a quiet CPU that never came, and its lease ran out before its first benchmark refreshed it (the harness refreshes between benchmarks only), so the measured minutes were not under its own lock. Its numbers are **upper bounds**; the gather and the stored faces were measured in the same run, so their ratio stands better than either absolute. Its fixed-operation rows are discarded: the bench found that form producing NaN where friction meets a still cell (its square root of zero overflowed in the Newton steps), which the device cases then confirmed and the form's guards now prevent.

GPU time of one step (the step's length pass and the update; the faces pass too for `faces`), median and p95 over the steps measured, in ms:

| grid | state | step (gather) | p95 | faces (stored) | p95 | fixed ops | p95 | mean dt (s) |
|---|---|---|---|---|---|---|---|---|
| 512² | lake | 0.0084 | 0.0093 | 0.0118 | 0.0153 | 0.0104 | 0.0112 | 0.0359 |
| 512² | stream | 0.0074 | 0.0081 | 0.0108 | 0.0141 | 0.0118 | 0.0130 | 0.0359 |
| 512² | beach | 0.0075 | 0.0083 | 0.0109 | 0.0137 | 0.0101 | 0.0111 | 0.0229 |
| 1,024² | lake | 0.0220 | 0.0233 | 0.0336 | 0.0370 | 0.0268 | 0.0284 | 0.0359 |
| 1,024² | stream | 0.0205 | 0.0220 | 0.0321 | 0.0346 | 0.0306 | 0.0336 | 0.0362 |
| 1,024² | beach | 0.0200 | 0.0213 | 0.0317 | 0.0343 | 0.0267 | 0.0289 | 0.0227 |
| 2,048² | lake | 0.1062 | 0.1083 | 0.3260 | 0.3372 | 0.1248 | 0.1265 | 0.0359 |
| 2,048² | stream | 0.0978 | 0.0998 | 0.3021 | 0.3113 | 0.1369 | 0.1402 | 0.0373 |
| 2,048² | beach | 0.0982 | 0.0997 | 0.3102 | 0.3194 | 0.1151 | 0.1170 | 0.0227 |

The gather and the fixed columns come from a second run (2026-11-26 00:0x local), the two back to back under the run's own GPU lock: other processes at 23–27 % of the CPU (100 % at the fixed run's last sample), the GPU 0 % busy at every sample, its SM clock 2,895–2,910 MHz. The gather's numbers there agree with the first run's within 1.5 %. The stored faces are the first run's only.

Of each gather step, the step's length pass (one workgroup reducing a record per 256 cells) is 0.0007 ms at 512² and 1,024² and 0.0016 ms at 2,048²; the rest is the update. The batch's own span over its 16 steps, barriers and bubbles included, is within 0.001 ms of the zones' sum.

**Derived** (*derived* from the table):

| grid | cells | Mcells/s (gather) | ms a step per million cells | a 60 Hz frame's share, one step |
|---|---|---|---|---|
| 512² | 0.26 M | 31,200–35,400 | 0.028–0.032 | 0.05 % |
| 1,024² | 1.05 M | 47,700–52,400 | 0.019–0.021 | 0.13 % |
| 2,048² | 4.19 M | 39,500–42,900 | 0.023–0.025 | 0.6 % |

(From the run-2 medians of the gather.)

**Bytes against bandwidth.** The gather moves 32 bytes a cell a step from memory: its three planes and its bed and Manning's n read (20 bytes; the neighbours' reads come from cache), three planes written (12). At 2,048² that is 134.2 MB a step, 0.0749 ms at the RTX 5090's 1,792 GB/s; measured 0.098–0.106 ms (run 2), 70.5–76.6 % of the peak. So the step runs **near the bandwidth bound, with the arithmetic close behind**, not purely memory-bound: the fixed-operation form adds 17 to 40 % at 2,048², which a memory-bound kernel would hide; at 1,024² the step takes 0.019–0.021 ms per million cells, only about 15 % above that traffic's floor at DRAM bandwidth although its working set (two copies and the inputs, about 40 MB) fits in the L2; and the stored form reaches a higher fraction of peak than the gather. The grounding page's memory-bound estimate is a floor the step comes within about a third of, not the cost itself. At 512² the step is a few microseconds and launch and latency rule. The stored faces move about 128 bytes a cell (two faces of four floats written and four read a cell, plus the update's own 32), 537 MB a step at 2,048², 0.30 ms at peak against a measured 0.30–0.33 ms, and cost 3.1 times the gather at 2,048² and 1.5 times below it: the gather's second computation of every face costs less than the bytes the stored form adds, which is why the gather is the default.

**Sub-steps.** At 0.5 m cells every state's step is longer than a 60 Hz frame — 0.036 s for the lake, 0.036–0.044 s for the stream, 0.023 s for the beach's bore — so **one step a tick** at every grid of the three, by the reference's rule dt = 0.45 dx / (max(|u| + c) + max(|v| + c)). The bench reports the mean step; a budget's sub-step count should use the shortest step a state reaches, which for these states is within a few percent of the mean (the beach's 0.0227–0.0229 s) and for a flood or a fall can be much shorter. The count scales with the cell: at 0.25 m the beach would take two, and a fall's foot (the grounding page's 6.3 m/s on 5 cm) a few. A first budget is cells × sub-steps × about 0.025 ms per million cells on this card, the beach's rate, for depth and momenta only.

## Holding the card to the reference

The device cases (`tests/water_live_device_tests.cpp`) run the reference's own cases through both and compare. The bounds come from one argument: single precision's unit roundoff is 2²⁹ times double's, and a first-order monotone step under its CFL limit does not amplify a perturbation, so the card may stray from the reference by about one ulp of the water involved a step, S × 2⁻²³ after S steps; a reference bound that is a count of double's ulps is scaled by 2²⁹.

Measured on this card in `msvc-debug`, 2026-11-25 (the step's numbers do not depend on the build), both forms of the arithmetic (native / fixed):

| case | bound | native | fixed |
|---|---|---|---|
| exact lake, 20 s (557 steps), datum 0 and 2,000 m | nothing moves; the two the same bytes | 0 cells moved, hashes equal | the same, and the same hash as native |
| inexact lake, surface drift | 5.4e-5 m | 6.0e-8 m (one ulp of 0.9 m) | 1.2e-7 m |
| inexact lake, fastest | 5.4e-3 m/s | 1.0e-6 m/s | 1.0e-6 m/s |
| Stoker, L1 at 200 / 400 / 800 cells | orders ≥ 0.5, finest ≤ 0.0045 | 0.01197, 0.00693, 0.00394; orders 0.79, 0.81 | the same to four digits |
| Stoker, card against reference (L1) | S × 2⁻²³ (6.4e-5 at 539 steps) | 6.8e-8, 7.5e-8, 1.1e-7 | 7.1e-8, 7.3e-8, 1.2e-7 |
| Ritter, L1 | orders ≥ 0.5, finest ≤ 0.0058 | 0.01228, 0.00798, 0.00502; orders 0.62, 0.67 | the same |
| Ritter, card against reference | S × 2⁻²³ (7.5e-5 at 629 steps) | 5.9e-8, 9.7e-8, 1.6e-7 | 4.8e-8, 7.3e-8, 1.3e-7 |
| dam volume (800 cells) | S × 2⁻²⁴ | 4.0e-8 (Stoker), 1.1e-7 (Ritter) | 4.6e-8, 7.7e-8 |
| count of water: steps, volume, outflow | S × 2⁻²³ of the water against the reference | 824 steps (reference 824), 7.44407 m³ (7.44407), 27.9762 m³ out (27.9762) | the same |
| count of water: the card's own balance | S × 2⁻²⁴ of the water involved (1.8e-3 m³) | −6.4e-7 m³, no round-off clamped | 3.2e-6 m³, none clamped |
| closed basin, 1,000 steps, 140 m³ | S × cells × 2⁻²⁴ × h_max × area (1.8e-2 m³) | −2.0e-6 m³ | −1.4e-6 m³ |
| same bytes twice; gather and stored faces | equal hashes | equal | equal |

The card is the reference's scheme: the dam breaks' errors and orders are the reference's own to three digits (W184 measured 0.00394 and 0.00502 on the finest grids), and the card stays within about 10⁻⁷ of the reference in L1, a thousandth of the stated bound. The bounds are a worst case of steady accumulation; the measured differences are what rounding at random gives, about √S ulps. The count's 824 steps are the reference's 824: single precision did not move the step's length enough to add or drop a step in a minute of flow.

**Cell by cell.** The L1 bound above is heuristic: the argument that a monotone step does not amplify a perturbation is a result for a scalar law, and an L1 over all the water could hide a front placed a cell off. So the cases also hold the largest single-cell difference. The dam breaks' largest |h − h_ref| is 0.6–9.5e-9 m against eight √S ulps of the deepest water (8.7e-8 m at 539 steps). On the count's slope, compared field by field with the reference in L1: h 3.8e-7 to 7.1e-7, hu 4.5e-7 to 1.1e-6, hv 7.4e-7 to 1.2e-6 (held for h and hu to S × 2⁻²³); the largest cell differences are 4.8e-8 to 8.2e-8 m of depth and 1.0e-8 to 5.3e-8 m²/s of momentum, reported and not held, since a wet-dry front a cell apart would be a whole depth that no rounding bound covers, and none appeared. The slope runs twice, with the east edge open as the reference's case has it and with all four edges open, so every open edge's ghost and ledger term is compared.

**Conservation.** A face's flux is one value on both sides on this card: the gather (each cell computes its own four faces) and the stored faces (each face once) give the same hash after 400 steps of the count's flow, in both forms of the arithmetic. So every flux telescopes, and what moves a closed basin's volume is each cell's own update rounding its depth: 2.0e-6 m³ of 140 m³ in 1,000 steps (1.4e-8 of it), as conserved as a float depth can be. Exact conservation would need the depth in fixed point; nothing measured here asks for it.

## A lake at rest, and the datum

The reference's exact lake (every height a multiple of 1/64 under 2) is exact in single precision too: every sum and difference the reconstruction takes is exact, the two sides of every face are the same state to the bit, and nothing moves in 20 s. That holds at 2,000 m as well with the grid's datum there, and the two are the same bytes. The inexact lake is where single precision shows, and where the datum earns its place:

| inexact lake, 20 s | surface drift | fastest | cells moved |
|---|---|---|---|
| datum 0 (and 2,000 m with the datum there: the same bytes) | 6.0e-8 m (native), 1.2e-7 m (fixed) | 1.0e-6 m/s | — |
| 2,000 m in world heights (datum 0) | 1.2e-4 m | 1.4e-5 m/s (native), 3.2e-5 m/s (fixed) | 2,592 of 3,072 |

So **single precision holds a lake at rest** — to one or two ulps of the surface and a micrometre a second, a hundredth of the reference's bound scaled to single precision — **provided the bed is a height from the grid's own datum**. Handed world heights at 2,000 m the surface's ulp is 2⁻¹³ m = 1.2e-4 m, every face sees two sides that differ by that much, and every wet cell moves: two thousand times the drift, still slow (1.4e-5 m/s) but a lake that is not at rest. What it takes is what `set_bed` does: the datum subtracted in double, once, before the bed reaches the card; any datum within a few metres of the water works, and a grid's own lowest bed is the natural one.

## An exchange under the last bit

A loam's take, 3.7e-6 m/s (13.2 mm/h), from a still pond 1 m deep over 600 s at one step a 60 Hz frame must remove 3.7e-6 × 600 = 2.22 mm. One step's take is 6.17e-8 m; the depth's ulp just below 1 m is 2⁻²⁴ = 5.96e-8 m, so each subtraction rounds the take to one whole ulp, and the pond loses 3.4 % too little. The case (`an exchange under the depth's last bit is kept by the compensated sum`) measures four forms over 36,000 steps:

| form | lost | error | cost |
|---|---|---|---|
| plain step | 2.14577 mm | −3.34 % | none |
| on a clock 8 times coarser (8 × the take every 8th step) | 2.14577 mm | −3.34 % | none |
| on a clock 64 times coarser | 2.21086 mm | −0.41 % (of which about 0.09 % is the run ending half way through a period: 36,000 steps are 562.5 periods) | none; the take arrives in lumps of 3.9 µm a second |
| compensated: each cell's remainder kept (plane 3) | 2.21997 mm | −0.001 % (held to two ulps of the depth) | one more plane read and written a step, 8 bytes a cell |

The coarser clock does not cure it: it only changes how many ulps each application rounds to (8 × 6.17e-8 is 8.28 ulps, rounded to 8, the same 3.3 % short), and it helps only once an application is many ulps, at the price of a take that arrives in lumps. The compensated sum holds — what rounding leaves out of a cell's depth is kept to the bit (h' − h is exact by Sterbenz's lemma while the take is small against the depth) and added to the next step's take — and is the form the ground's store should use when it lands: the store's exchange is exactly this case, and its error would otherwise be a systematic loss, not noise. Its cost is the remainder's plane, 4 bytes read and 4 written a cell a step, a quarter of the step's traffic again, paid only by a grid that carries a slow exchange.

## The same bytes on every device

ADR-0027's "hashed" is a state that lives in the fixed-step tick, enters the sim hash and replays exactly, which the same bytes twice on one device is not enough for; W173's design asks whether the step can be the same bytes on every device. Three things decide it.

**Contraction.** A driver may fuse a multiply and an add into one rounding unless the SPIR-V forbids it with NoContraction. **Slang 2026.17.1's `precise` emits no NoContraction when it writes SPIR-V directly** (`-emit-spirv-directly`, which `engine_shaders()` uses): a two-line probe and this shader, every line `precise`, came out with 2,579 float add, subtract, multiply and divide instructions and not one decoration. The compile option `-fp-mode precise` puts it on every float operation (2,581 decorations; no GLSL.std.450 Fma), so `water_live` compiles with it (`engine_shaders(... FLAGS -fp-mode precise)`), and a case counts the decorations in the embedded module. The same holds for every shader in the tree that relies on `precise` today (ADR-0074's `rt_keep` key in `clas_records.slang` among them): as compiled, their `precise` lines forbid nothing, which is a finding for those rows, not one this row changes.

**Division, the square root, the cube root.** Vulkan's SPIR-V environment allows division 2.5 ulp and the square root its inverse-square-root's error, so two conformant devices may round them differently; addition, subtraction and multiplication are correctly rounded wherever contraction is forbidden. `Ops::fixed` builds the three from those alone: an estimate from the operand's bits (integer arithmetic, exact everywhere), then a fixed number of Newton steps — four for 1/x, three for 1/√x, four for ∛x — with the edges a bit trick does not cover answered explicitly (zero and the denormals, and reciprocals below the smallest normal). Its cost on this card, back to back with the native form: 17–35 % more a step for the lake and the beach (the beach +35 % at 512² and +34 % at 1,024²), 40–59 % more for the stream, whose dry half and wet-dry faces take the friction and the reconstruction's branches the native form answers with one instruction (0.137 against 0.098 ms at 2,048²); still about 0.03 ms per million cells. Its accuracy: the dam breaks and the lakes agree with the native form to the digits in the cases' table.

**Denormals.** The SPIR-V carries no DenormPreserve or DenormFlushToZero execution mode, so each device uses its default; on this card `shaderDenormPreserveFloat32 = false` and `shaderDenormFlushToZeroFloat32 = false`, read with `vulkaninfo` from `VkPhysicalDeviceVulkan12Properties` (core since Vulkan 1.2; the driver also lists `VK_KHR_shader_float_controls` revision 4 and `VK_KHR_shader_float_controls2`; RTX 5090, driver 621.24, Vulkan 1.4.357), so neither can be asked for, and the shader declares neither (it uses no FloatControls execution mode): a device that flushes and one that does not would disagree wherever a value passes 2⁻¹²⁶. The step's values stay far above it — the dry depth is 1e-9 m, and a depth below it has its momenta zeroed — and the fixed form answers zero and denormal operands itself; but nothing yet clamps every intermediate (a product of two small momenta can still underflow), so the same bytes across devices needs that clamp too, or a device that can be told its denormal mode.

**What was measured.** The same bytes twice on this card, in both forms (a case). The hash after 400 steps of the count's flow: native 3142517362989380888, fixed 18440459637061453408 — different, as they should be, since the two forms round their divisions differently; each is the same twice, and the same in the gather and the stored faces. **On the Titan Xp (2026-11-26) the same case gives native 10545315909033131114 and fixed 8796295464579050483**: the same twice and the same in both forms of the faces there too, but neither equals this card's, so the fixed-operation form as it stands is not yet the same bytes across devices ([water_live](../subsystems/water_live.md), "Two cards", has the table and what is not yet found).

## What it means for the design

- **The cost is not what limits the water.** One step of a 2,048² grid (4.2 million cells, a kilometre square at 0.5 m) is 0.1 ms on this card, 0.6 % of a 60 Hz frame, and one step a tick covers every state measured at 0.5 m cells. The grounding page's two examples — 1.5 million cells of stream corridor and 2.8 million of surf zone — would cost about 0.04 and 0.07 ms a tick. The budget the design can plan with is about 0.025 ms per million cells per sub-step on the RTX 5090 (an upper bound, measured beside other work), with the beach as the case to budget from; the memory traffic, 32 bytes a cell a step for depth and momenta, is the floor under it, and the arithmetic the full state adds (the carried quantities, the outputs the foam and the bed read) is not measured yet.
- **Single precision is enough for the state**, on two conditions this row found: the bed reaches the card as a height from the grid's own datum (world heights at 2,000 m lose the lake at rest), and a slow exchange under the depth's last bit is carried by a compensated sum (the plain step loses 3.3 % of a loam's take).
- **Gather, not stored faces.** Computing each face twice costs nothing the bytes do not; the stored form costs three times as much at the size that matters, and the two are the same bytes on this card.
- **Contraction must be forbidden by the compile option**, not by `precise` alone, for this shader and for any other the tree wants reproducible.
- **The older card** (the Titan Xp on the Linux server, at about 548 GB/s) passes the device cases and gives other bytes than this card in both forms (above); its step's time is not measured yet, since the bench's CTest run opens no device and the server is reached through CTest alone; its bandwidth floor for the 2,048² step is about 0.25 ms, and about 0.33 ms at the fraction of peak this card reached.
