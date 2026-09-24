# Visible order: what a deterministic visibility buffer costs

- **Question:** the desert-overlook flythrough ([results](flythrough-desert-overlook.md)) found that occlusion culling never lost a nearer surface but did change which of two surfaces at *exactly one depth* a pixel showed, and that two identical runs disagreed too, because the visibility word's id was the visible-list entry and the list was filled by atomic append. There are two ways out: make the **id** independent of the list (the scene's pair), or make the **list's order** a function of the frame's inputs (a pair-ordered compaction). What does each cost on the RTX 5090 and on the TITAN Xp, and which does the renderer need?
- **Date:** 2026-09-24. **Machines:** the development desktop — Intel i9-10980XE (18 cores / 36 threads), 64 GB, Windows 11 Pro, **RTX 5090** (32 GB, driver 610.88, Vulkan 1.4.341), `msvc-release`; and the headless server — Intel Xeon E5-2670 (Sandy Bridge-EP, 8 cores / 16 threads), Gentoo, **TITAN Xp** (Pascal, 12 GB, driver 580.178.04), `linux-server` (GCC 14.3.1, x86-64-v2) through `tools/remote-build.ps1`. **Builds** (every number below is a GPU timestamp, which the host compiler's floating-point flags do not reach, but the commit is stated anyway because [ADR-0035](../adr/0035-no-floating-point-contraction.md) changed the Linux builds' CPU code between the two rounds): *baseline* is main at `9d478b6` (round 1) or `2d0b906` (round 2); *pair ids, entry table* is this change's first form on `9d478b6`, which decoded every pixel through the view's pair-to-entry table; *pair ids* is the landed form, which decodes through the scene's pair table (on `9d478b6` for the RTX 5090, `2d0b906` for the TITAN Xp's round 2); *pair order* is the entry-table form plus the ordered compaction described below, on `9d478b6`, measured and not landed.
- **Machine state, RTX 5090:** every run under the machine-wide GPU lock (`agent-visible-order`). The harness printed its WARNING on every run: other processes used 10–33% of the CPU (other agents building; the owner's desktop), the GPU was 4–9% busy before each run and 9.9–17.8 GB of its memory belonged to other processes. The numbers are upper bounds by the rule; the comparisons are between builds run minutes apart in one session, whose baselines agree with each other to within 1–2 µs (the two rounds' heightfield baselines, 76.7 and 76.8 µs), and a difference under about 2 µs is not a difference here.
- **Machine state, TITAN Xp:** each group started only once the server's 1-minute load average was under 1.0 and the GPU under 5% (the E1 on Pascal driver's gate); load 0.93–0.99 at the gates, other processes at 1.5–14.3% of the CPU at the harness's samples, the card at 1,809–1,860 MHz and 55–70 °C as warm-ups ended. Each round started straight after that round's builds on the same machine, which is why the gates waited 20–70 s.
- **Decision:** the id is the pair, the tie rule is "the larger (pair, triangle) wins", and the list stays in atomic order ([gfx](../subsystems/gfx.md), "The tie rule" and "The visible list's order"). No ADR: the encoding is the gfx page's to state, and nothing about the choice should surprise the next reader once that page says why.

## Setup

**Pair ids** change what the visibility word carries: `pair << 8 | triangle` instead of `visible_index << 8 | triangle`, where `pair = instance.first_pair + (cluster − mesh.first_cluster)` is the pair's index in the scene (`pair_of` in `shaders/scene.slang`). Every rasterizer computes it from the entry it already reads. The cull pass writes one more word per drawn pair — the view's pair-to-entry table, since the deformed-vertex pool, the indexed draw's records and the CLAS records are all kept per *entry* — and the resolve decodes a pixel through the scene's pair table (`{instance, cluster}` per pair, written once at upload), reading the pair-to-entry table only for a deformed instance. The ray path turns its hit's entry into the pair through the visible list, the instance and the mesh, and has to settle a tie itself, because a ray query commits whichever of two equal-distance hits the traversal meets first (below).

**Pair order** leaves the id alone and sorts the list instead. Each 256-lane cull workgroup scans its block's survivors in shared memory (Hillis–Steele, the three survivor counts packed into one word, and the hardware survivors' triangles beside them on the vertex path) and publishes the block's counts; the workgroup that **finishes last** — a device-scope counter each block increments after a device memory barrier, the pattern of CUDA's `threadFenceReduction` sample, so no workgroup ever waits for another — scans every block's counts into offsets and adds the totals to the count words the draws are dispatched from; a second dispatch over the same workgroups scatters each survivor to `base + block offset + rank`, with its pair-to-entry word and, on the vertex path, its indexed-draw record. It passed every picture test, made the pool's overflow and the index budget's overflow reproducible, and was not kept; the design is described here so that nobody has to rebuild it to learn its cost.

**The method is E1 on Pascal's** ([e1-pascal-rerun](e1-pascal-rerun.md)): one `engine-host --stdio` process per scene and build, `render.load` once, then per configuration a `render.benchmark` with 2 s of warm-up and three measured repeats (3,000 frames at 1080p, 1,500 at 4K), offscreen, the median of the three reported; GPU milliseconds per pass from `gfx::GpuTimer`. Two scenes: the **heightfield** (one 1025 × 1025 grid, 2.1 M triangles, 46,628 pairs, orbit distance 9, LOD threshold 1 px, 722 pairs drawn at 1080p) and the **helmet grid** (64 FlightHelmets, 150,784 pairs, orbit 22, 1,766 drawn). The RTX 5090 draws them on the mesh path (`hw`), the vertex path (the indexed draw) and the ray path (`rt`); the TITAN Xp on the vertex path. The **desert-overlook path** (2,401 frames, three repeats, per-frame medians) was flown with the owner's landmarks on the RTX 5090, whose 876,537 pairs are the largest scene measured here, and with the substitutes on the TITAN Xp; frames 852 (the saddle's face fills the frame) and 1260 (the overlook, every landmark in view) are the scene README's markers.

## Results

### RTX 5090, the E1 grid (frame total, µs)

| scene, path, occlusion, resolution | baseline | pair ids | pair order |
|---|---|---|---|
| heightfield, mesh, off, 1080p | 76.8 | 77.9 | 83.9 |
| heightfield, mesh, off, 4K | 224.4 | 225.0 | 237.0 |
| heightfield, mesh, on, 1080p | 119.1 | 119.0 | 130.4 |
| heightfield, mesh, on, 4K | 354.4 | 356.2 | 364.4 |
| heightfield, vertex, off, 1080p | 79.2 | 80.3 | 86.5 |
| heightfield, vertex, off, 4K | 227.1 | 229.0 | 236.9 |
| heightfield, vertex, on, 1080p | 119.5 | 119.7 | 135.7 |
| heightfield, vertex, on, 4K | 353.4 | 351.7 | 369.5 |
| helmet grid, mesh, off, 1080p | 38.6 | 40.3 | 47.1 |
| helmet grid, mesh, off, 4K | 90.9 | 92.6 | 100.8 |
| helmet grid, mesh, on, 1080p | 73.6 | 73.8 | 87.9 |
| helmet grid, mesh, on, 4K | 212.4 | 204.1 | 231.1 |
| helmet grid, vertex, off, 1080p | 48.5 | 48.5 | 57.7 |
| helmet grid, vertex, off, 4K | 110.8 | 110.2 | 119.8 |
| helmet grid, vertex, on, 1080p | 83.5 | 85.0 | 101.3 |
| helmet grid, vertex, on, 4K | 233.2 | 229.0 | 243.4 |

Pair ids cost 0–2 µs a frame, inside this session's noise on half the rows: about 1 µs of the resolve at 1080p and up to 1.2 µs of the cull pass (the pair-to-entry word) on the helmet grid. Pair order costs **6–12 µs a frame with occlusion off and 8–19 µs with it on**, because every cull dispatch pays it: the heightfield's cull and scatter together went from 10.2 to 16.5 µs at 1080p, against the cull pass alone it was added to.

### RTX 5090, the ray path (trace pass, µs)

| scene, resolution | baseline | pair ids: second look (landed) | one traversal, loop commits | one traversal, no commit |
|---|---|---|---|---|
| heightfield, 1080p | 127 / 127 | 217 / 217 | 148 (wrong) | 193 |
| heightfield, 4K | 463 / 471 | 817 / 821 | 538 (wrong) | 735 |
| helmet grid, 1080p | 38 / 39 | 42 / 42 | 45 (wrong) | 58 |
| helmet grid, 4K | 132 / 130 | 138 / 139 | 136 (wrong) | 164 |

Two sessions of three builds each, baseline and the second look in both (the pairs of numbers); the one-traversal builds are on `2d0b906`, the rest on `9d478b6`. The frame is the trace plus the cluster acceleration structures' per-frame build, which is 1.9–2.8 ms on the helmet grid and moves by more between runs than the trace does, so the trace pass is the column to read. The heightfield's traced frame goes from 404 to 490 µs at 1080p and from 918 to 1,273 µs at 4K with the second look. "Wrong" is measured, not guessed: that build failed the tie case on the ray path, the last copy holding none of the 2,857 pixels.

### RTX 5090, the desert-overlook path with the owner's landmarks (µs)

| occlusion, resolution | baseline: path median / frame 852 / frame 1260 | pair ids | pair order |
|---|---|---|---|
| off, 1080p | 96.2 / 126.8 / 105.4 | 97.2 / 129.2 / 107.0 | 125.5 / 158.2 / 136.1 |
| on, 1080p | 134.0 / 136.4 / 150.7 | 134.1 / 138.1 / 151.6 | 191.5 / 194.7 / 210.0 |
| off, 4K | 280.6 / 403.5 / 297.9 | 282.1 / 411.9 / 298.4 | — |
| on, 4K | 375.9 / 422.4 / 396.7 | 376.5 / 427.4 / 402.4 | — |

The pair-order column is from the first round, whose baseline agrees with this one's to 0.2–1 µs. **At 876,537 pairs the ordered compaction costs 29 µs a frame without occlusion and 58 µs with it** — the cull pass goes from 20 to 43 µs, and from 39 to 79 µs over its two dispatches — because the last block's scan is over every block of the *scene*, 3,424 of them, not over the cut. The pair ids' cost stays at 1–2 µs of path median; the 8 µs at frame 852 in 4K is the resolve (1.7 µs), the cull (2.1) and the raster (2.2) of a frame whose every pixel is terrain, and is the largest single frame found.

### TITAN Xp, the vertex path (frame total, µs)

Round 1, on `9d478b6`:

| scene, occlusion, resolution | baseline | pair ids, entry table | pair order |
|---|---|---|---|
| heightfield, off, 1080p | 651.8 | 661.2 | 659.5 |
| heightfield, off, 4K | 2,577.7 | 2,628.0 | 2,547.4 |
| heightfield, on, 1080p | 792.8 | 805.9 | 817.8 |
| heightfield, on, 4K | 2,978.2 | 3,038.2 | 3,077.9 |
| helmet grid, off, 1080p | 302.6 | 303.6 | 319.9 |
| helmet grid, off, 4K | 1,030.0 | 1,028.8 | 1,051.4 |
| helmet grid, on, 1080p | 375.6 | 378.2 | 410.7 |
| helmet grid, on, 4K | 1,096.9 | 1,102.2 | 1,134.3 |

On the TITAN Xp the ordering cost the cull pass 10 µs a dispatch (25.5 to 35.2 µs on the heightfield at 1080p) and the helmet grid's frame 17–37 µs — and on the heightfield with occlusion off it paid for itself, because the vertex path's raster ran 10–13% faster (154.5 to 138.4 µs at 1080p, 704.6 to 610.3 µs at 4K) with the indexed draw's records in pair order, which on one heightfield is spatial order. That is a finding about draw order and Pascal's raster, not about determinism, and is a follow-up (below). The heightfield's occlusion-on rows moved the other way by more than the ordering explains (the resolve 120 µs slower at 4K), which was not chased.
Round 2, on `2d0b906`, with the resolve's pixels decoded through the scene's pair table (frame total, and the resolve pass beside it):

| scene, occlusion, resolution | baseline | pair ids | resolve, baseline → pair ids |
|---|---|---|---|
| heightfield, off, 1080p | 652.2 | 652.7 | 472.0 → 477.8 |
| heightfield, off, 4K | 2,571.0 | 2,576.9 | 1,843.3 → 1,867.8 |
| heightfield, on, 1080p | 790.6 | 802.2 | 464.8 → 474.8 |
| heightfield, on, 4K | 2,976.3 | 2,996.4 | 1,797.2 → 1,814.5 |
| helmet grid, off, 1080p | 299.1 | 300.7 | 227.5 → 228.8 |
| helmet grid, off, 4K | 1,024.0 | 1,025.5 | 918.2 → 919.4 |
| helmet grid, on, 1080p | 373.7 | 378.3 | 141.9 → 143.9 |
| helmet grid, on, 4K | 1,090.3 | 1,099.8 | 479.9 → 486.8 |

The desert-overlook path with the substitutes, round 2, 1080p (µs; the harness called all four runs quiet):

| occlusion | baseline: path median / frame 852 / frame 1260 | pair ids |
|---|---|---|
| off | 643.2 / 934.9 / 637.9 | 649.2 / 920.8 / 642.9 |
| on | 782.3 / 974.8 / 787.2 | 789.3 / 974.6 / 791.5 |
The entry table cost the heightfield's resolve 2.2–3.1% (10–55 µs) in round 1; the pair table costs it 1.0–2.2% (6–25 µs), the frame 0–1.5%, and the desert path 0.9% of its median (6–7 µs). What is left is locality rather than a dependent load: the table is indexed by pair, so the few hundred pairs a frame draws are spread over the whole scene's table where the visible list held them in one dense run.

## What surprised me

- **The list's order was never what the picture needed.** The tie was decided by the id's bits, so changing what the id carries fixed the picture for a table load, while sorting the list — the fix the flythrough's write-up pointed at — costs the cull pass more than itself and fixes the picture only incidentally. What the order still decides (which entries a pool or index budget refuses, the order acceleration structures are built in) is not in any picture the defaults draw.
- **The ordered compaction's cost is the scene's, not the cut's.** The scan over blocks is over every pair of the scene whether or not it is drawn, so the 47 k-pair heightfield pays 6 µs and the 877 k-pair overlay 29 µs a dispatch. A design that wanted it would have to scan survivors only — per-block counts compacted before they are scanned, or blocks of 1,024 — and would still pay the second dispatch and its barrier, which on the RTX 5090 is about 4 µs of the 6.
- **The first pair-id resolve cost the TITAN Xp 2–3% and the RTX 5090 almost nothing.** Going through the view's pair-to-entry table and then the visible list is two dependent loads per pixel where the entry id had one; Pascal's resolve is latency-bound at 4K (1.85 ms on the heightfield) and paid 55 µs for it. The scene's pair table gives the {instance, cluster} in one load again and halves that (1.0–2.2%); the rest is the table's locality.
- **A ray query cannot be asked for "the larger key at equal distance".** It commits the first of two equal hits the traversal meets, which is the structure's order and nothing the rasterizers know. The first way round — a second traversal over exactly the committed distance — was correct and nearly doubled the heightfield's trace pass (127 to 217 µs at 1080p), because a full-screen surface pays a whole second descent per pixel. The obvious improvement — one traversal with every candidate reported to the loop, which commits the nearest itself and breaks a tie by key — cost 16% and was **wrong**: on this driver a commit shrinks the ray so that its own distance is *excluded*, so a coincident candidate the traversal meets after the first is never reported and the structure's order decides again. Not committing is right, but then everything behind the surface is reported too, which cost 26–56% and grows with the depth complexity of the scene; the second look is bounded by one descent to one point, so it stays. The ray path is the parity and E2 path, not the shipping primary visibility.
- **On Pascal, drawing the cut in pair order made the raster faster.** The indexed draw walks its records in list order, and in pair order a heightfield's clusters arrive in spatial order: 10–13% off the TITAN Xp's vertex raster, enough to hide the ordering's cost there. The RTX 5090's mesh and vertex paths showed nothing of the kind. A per-workgroup ordered append — one atomic per block after an in-block scan, no second dispatch — keeps runs of 256 pairs in order, which may be most of that locality for a fraction of the cost; it is not determinism and is not built.
- **The old id was run-to-run stable at small scale.** The tie cases' 24 coincident boxes gave the same wrong answer on every repeat, because a handful of workgroups appends in the same order each time; only the flythrough's hundreds of thousands of pairs showed two identical runs disagreeing. The cases therefore test the *rule* (the last copy wins, one pass equals two) rather than repeatability.

## What it decides

The id is the scene's pair, a depth tie goes to the larger (pair, triangle) on every rasterizer and the ray path, and the invariant the flythrough had to weaken is restored: the path shows 0 differing pixels with and without occlusion culling on both asset sets, the mesh path and the vertex path, on both cards ([flythrough](flythrough-desert-overlook.md)). The visible list is **not** sorted: it would cost 6–58 µs a frame on the RTX 5090 to make reproducible two budgets the defaults are sized never to reach. Considered and not built: a **per-workgroup ordered append** (one atomic per block after an in-block scan), which orders entries within a block but leaves the blocks racing for the counter, so nothing the list feeds becomes reproducible — as a determinism mechanism it would pay the in-block scan for nothing, though the draw-order locality it keeps is worth measuring on Pascal for its own sake (above); and a **single-pass decoupled look-back**, which saves the second dispatch but chains every block behind its predecessor when all blocks are resident at once, as they are at these pair counts, and rests on forward progress between workgroups, which a GPU shared with the owner's desktop is not the place to lean on.

**What it does not decide:** whether the tie rule picks the *right* surface. For coincident copies it does not matter; for back-to-back faces (the palms) the rule picks by index, not by facing, so a two-sided leaf can resolve to the sheet facing away. A facing bit above the pair would settle that and costs one of the id's 24 bits; it is a follow-up.

## Caveats

One RTX 5090 on a shared desktop, every run flagged by the harness; the frame-level differences under 2 µs are noise here, and the pair-id rows with occlusion on at 4K moved by up to 8 µs *down*, which is the Hi-Z pass's own variance. One TITAN Xp, whose rounds were built on different commits. The heightfield is the worst case for anything per pixel (every pixel is one surface) and the helmet grid for anything per pair; neither has deformed instances, whose pixels do read the pair-to-entry table, so a skinned crowd filling the screen pays the second load back — unmeasured.
