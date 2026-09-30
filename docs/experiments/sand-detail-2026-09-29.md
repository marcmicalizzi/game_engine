# The sand close up (2026-09-29)

**Question.** The owner walked the erg for the first time on 2026-09-28 and asked for "a sand texture and normals for the sand", with the condition that it look natural and create no seams. Can wind ripples and grain drawn as a *function of position* — no image, no tiles — read as sand from a walker's eyes, fade without moiré or pop as the camera pulls away, stay seamless across the terrain's rings and chunks, and cost the resolve less than a third of a millisecond at the owner's 11520×2160?

**Answer.** Yes on every count but one soft one: the ripples read as ripples — sinuous crests across the wind with ends and junctions, a lit stoss and a dark lee — and fade smoothly into sand that is as bright as the supersampled ripples it stands for; the detail is the same function on both sides of every ring and chunk border to within a byte or four; and it costs the resolve 0.09 ms at the median at 11520×2160 (0.19 at the 95th percentile, 0.23 at the 99th, one frame of 3,601 at 0.42), 0.01–0.03 ms at 1080p. The soft one: at a grazing sun they read less strongly than real ripples do, because nothing shadows one ripple's lee from the next crest.

## Setup

- **The function** (docs/subsystems/gfx.md, "The ground's detail"; renderer.md, "The sand close up"): phasor-noise ripples (Tricard et al. 2019) in the shading normal — complex Gabor kernels along the wind on a jittered lattice, an asymmetric profile laid on the sum's phase, the height tapered at the phase's singularities — and two octaves of value-noise grain in albedo and roughness, all a function of the shaded point's world (x, z), the scene's seed and the ground provider's wind. The filter fades the ripples by the pixel's footprint along the wind in two stages (the profile's harmonics between 8 and 4 pixels a wavelength, the sinusoid between 4 and 2) and moves the lost slope variance into the GGX roughness.
- **The scene**: `content/test-scenes/desert-erg` with the `detail` block at its defaults (12 cm, 8 mm, asymmetry 0.75, defects 0.35, slope fade 22–30°, 2 cm grain at 8% albedo and 0.05 roughness); "off" is the same file without the block, whose terrain hash and cache entry are the same. The ripples lie across the day's wind, towards −x (west).
- **Machine**: the owner's desktop, RTX 5090, `msvc-release`, `engine-view` offscreen; `--terrain-rings` for every picture (the ground near the camera at 50 cm and 1 m), shadows traced (the default on this card).

## Captures

Taken with this commit, 1920×1080 unless named otherwise, in the session's scratch directory `sand-detail/captures/` (outside the repository; the owner looks at them before he walks it). The camera paths are one-pose paths at eye height (1.65 m over the ground, `"ground": true`); the walk path is committed as `content/test-scenes/desert-erg/walk-path.json`.

| File | What it is |
|---|---|
| `sand-along.png`, `sand-along-off.png` | eye height on the western floor at (−1400, 0), looking along the wind (west) |
| `sand-along-surround.png`, `sand-along-surround-off.png` | the same at 11520×2160, `--views surround3` |
| `sand-along-detailview.png` | the same in `--view detail`: red the ripple height, blue the grain, green the share drawn |
| `sand-across.png` | the same place, looking across the wind (south) |
| `sand-feet.png`, `sand-feet-off.png` | looking down a pace ahead |
| `sand-crest-lowsun.png`, `sand-crest-lowsun-off.png` | a mega-draa's brink at (462, −40), looking along it, the sun 6° up in the east (`--sun 15,6`) |
| `sand-slipface.png` | on the slip face at (330, −20): no ripples at 34° |
| `pullback-NNN.png`, `pullback-NNN-off.png` | frames 0, 40, 80, 120, 160 and 239 of the pull-back below, 960×540 (1.65, 4.2, 10.3, 24, 62 and 350 m up) |
| `pullback-060-crop-twostage.png`, `pullback-060-crop-onestage.png` | frame 60 (6 m up), a crop at 2×, with the filter as committed and with the one-stage fade it replaced |

**How they read, honestly.** Along the wind, the near ground is a field of sinuous crests about a hand's width apart, with a crest ending or forking every few wavelengths, a lit windward face and a thin dark lee — it reads as wind-rippled sand, and it fades into smooth sand ahead without bands; the far dunes are unchanged from the picture without the detail. Across the wind the crests run away towards the horizon and read as well. At the feet the ripples are right and the grain is a soft centimetre mottle rather than grains, which is honest at this resolution: a millimetre grain is under a pixel even there. On the crest at a low sun the stoss's ripples are the most striking picture of the set, and the brink and slip face beyond are smooth, as they should be; but the contrast is milder than a photograph at dawn would have, because a real ripple's lee lies in the shadow the next crest casts and here it is only turned from the sun. The slip face is smooth. What does not read as sand: a lee is a slightly too regular dark line where the ripples are resolved at four to eight pixels a wavelength (the one-stage fade made it a stair-stepped 1-pixel line; the two-stage fade softens it into a sinusoid); and in `sand-along-surround.png` the third monitor shows **dark grey shards on the ground far off, which are not the detail's**: they are identical in `-off`, absent without `--terrain-rings`, and absent with the rings but no shadows, so they are traced shadows from the rings' geometry near the middle ring's border (the grid's sunken hole, or a skirt) — worth its own look before the owner walks with rings and traced shadows.

## The filter: the pull-back

240 frames from 1.65 m to 350 m over the western floor (heights doubling every 30 frames, the camera backing off as it rises and looking at a fixed point of the floor), 960×540, detail on and off, and every eighth frame again at 3840×2160, box-filtered 4×4 down to 960×540: the picture the pixel should average to. Luma of the display's bytes.

| | detail off | detail on (committed) | detail on, one-stage fade |
|---|---|---|---|
| frame-to-frame mean \|ΔL\|, mean over the path / worst frame | 0.57 / 1.44 | 1.17 / 2.22 | 1.18 / 2.18 |
| pixels changing more than 4 levels a frame, mean / worst frame | 2.8% / 8.7% | 8.4% / 17.6% | 7.5% / 15.5% |
| mean \|error\| against the supersampled picture, mean / worst of 30 frames | 0.32 / 0.64 | 0.89 / 1.37 | 0.76 / 1.09 |
| the same after a box blur of radius 3, twice (bands and shifts only) | 0.17 / 0.33 | **0.20 / 0.33** | 0.21 / 0.33 |
| mean signed error against it (what the filter leaves brighter or darker) | −0.09 … 0.04 | **−0.09 … 0.05** | −0.09 … 0.05 |

Read by height: the frame-to-frame change with the detail is 1.6–2.0 levels while the ripples are resolved (below 12 m), falls through 1.0 at 16 m, and is the change without the detail, within a hundredth, from 30 m up — no frame spikes on the way down, which is the no-pop half. Near the ground the extra change is the ripples moving across the screen as the camera moves over them, not flicker. The no-moiré half is the blurred row: the error a ripple's structure cannot explain rises by 0.03 of a level with the detail, and the brightness never moves by more than 0.09 of a level from the supersampled picture — distant sand is rougher, not flatter. The unblurred error is larger with the detail because the resolve point-samples resolved ripples that the supersampled picture averages; the two-stage fade makes it larger still (the supersampled picture keeps the asymmetric lee the eased profile no longer draws), and was kept anyway for what the crops show: the one-stage fade's lee was a hard 1-pixel stair at six to twelve metres, the two-stage's is a soft line.

The renderer's own test (`systems/renderer/tests/ground_detail_tests.cpp`) holds the rule on the waves at 192×108 over 48 frames: at most 0.12 of a level added to the frame-to-frame change, 0.18 to the error against a 4×4 supersampled picture, and 0.09 of mean bias.

## Seams

`systems/renderer/tests/ground_detail_rings_tests.cpp`, the rings shrunk to a 128 m terrain (an inner ring 6 m either side at 25 cm, a middle one 20 m at 50 cm), a view 5 m up across the inner ring's border and the chunk borders at x = 0 and z = 0, in the detail view; each pixel's ripple height and grain against the CPU function at the world point its own depth gives (RTX 5090, `msvc-debug`):

| pixels | count | ripple, worst of 255 | grain, worst | over 3 | share drawn, worst step between neighbours |
|---|---|---|---|---|---|
| inside a chunk | 46,867 | 13 | 11 | 30 | 12 |
| at a ring's border | 512 | 1 | 4 | 1 | 9 |
| at a chunk's border | 881 | 1 | 1 | 0 | 7 |

The borders are no worse than the inside; the inside's worst pixels are at a grazing angle, where a float depth is millimetres along the ground and a lee or a 5 mm grain cell changes fastest. Over the rings with the detail on, occlusion culling and the cone test change 0 pixels of 15,059, and two runs 0; on the waves likewise, and a block asking for nothing draws exactly what no block draws.

## GPU against CPU

`domain/gfx/tests/ground_detail_tests.cpp`: sixty metres of sand in one-metre cells, a walker's view along it and at the feet, by the origin and 3.7 km out, shaded and in the detail view, every covered pixel against `ground_detail_reference.h` shaded by `brdf_reference.h`. By the origin **1 of 255** on both (tolerance 2); 3.7 km out **3** shaded (tolerance 4, 469 of 25,600 pixels over 1) and **14** on the raw height (tolerance 16), which is float positions a quarter of a millimetre coarse, interpolated, about a millimetre on a 30 mm lee.

## What it costs

`tools/flythrough.ps1` under the GPU lock (`tools/gpu-lock.ps1 run`), `--shadows auto` (traced), occlusion on, 3 repeats after 240 warm-up frames, the erg's own camera path (3,601 frames) and `walk-path.json` (1,201 frames at eye height); resolve GPU milliseconds from `gfx::GpuTimer`, each frame the median of its repeats:

| path, resolution | resolve off | resolve on | on − off, frame by frame: median / p95 / p99 / max | frame median off → on |
|---|---|---|---|---|
| erg, 1920×1080 | 0.135 | 0.147 | 0.010 / 0.022 / 0.028 / 0.042 | 0.451 → 0.465 |
| erg, 11520×2160 surround3 | 1.721 | 1.849 | **0.091 / 0.193 / 0.228 / 0.425** | 2.355 → 2.481 |
| walk, 1920×1080 | 0.131 | 0.160 | 0.028 / 0.032 / 0.037 / 0.041 | 0.435 → 0.463 |
| walk, 11520×2160 surround3 | 2.324 | 2.419 | **0.090 / 0.105 / 0.116 / 0.129** | 2.974 → 3.070 |

Machine state: the lock was ours throughout; before each run the GPU was 0–2% busy and other processes used 0.9–7.0% of the CPU; the harness raised a WARNING on five of the eight runs because its sample *after* the run read the GPU 76–100% busy — the run's own work, caught as it ended — and one end sample saw others at 9.6% of the CPU. Treat the absolute numbers as upper bounds; the on − off differences were taken the same way minutes apart.

**Corrected 2026-09-30:** the numbers below were taken with traced shadows and hold for them alone. With the cascaded maps or with no shadows the same walk costs the resolve 0.53 ms at the surround size, over the budget: [the second pass's page](sand-detail-second-pass-2026-09-29.md#the-cost-on-the-gpu-measured-at-the-merge-2026-09-30) has the table and the likely reason.

**What it costs a frame**: at the owner's surround, about 0.09 ms at the median and 0.2 ms at the 95th percentile of the erg's path, under a third of a millisecond except for one frame in 3,601 (0.42 ms, which the 99th percentile, 0.23, says is an outlier rather than a kind of frame). The cost is where the ripples are drawn: they are skipped wherever their fade is zero, which past a few tens of metres is everywhere, so the walk at eye height, whose lower half is all ripples, costs the same 0.09 at the median as the erg's path and far less spread. **What would go first** if it had to be cut: the kernels — a point sums eighteen (two a cell over 3 × 3 cells) of which about six reach it; one a cell halves the work at the cost of a field that leans on its lattice, and a 2 × 2 search with kernels half a cell wide keeps the density for four cells' worth of hashing. Then the grain's fine octave, which is under a pixel almost everywhere at 1080p.

## What surprised me

- **A quad is the wrong test surface for a resolve.** The first GPU comparison failed on half of every picture: the resolve rebuilds a pixel from its triangle's corners projected to the screen, which is meaningless for a triangle reaching behind the camera — and a quad under a walker's feet does. Sixty metres of one-metre cells passes to 1 of 255. The terrain's own triangles are small enough near the camera that this does not arise, but a 1.5 m grid triangle looked at from eye height at a shallow pitch could reach behind the eye; nothing has shown it yet.
- **The slope variance of a phasor pattern is 0.65 of its profile's** (measured over the CPU function, 160,000 points), not 1: the taper lowers every defect's neighbourhood. The transfer moves the pattern's own variance, and the share holds within 8% from no defects to the most.
- **The frame-to-frame change is not a flicker meter.** Resolved detail under a moving camera changes the picture by itself; only the blurred error and the change's convergence to the no-detail change at height separate aliasing from motion.

## Caveats

One machine, one sun (the default and one low one), one erg. The pull-back is one path; a camera sweeping sideways at a grazing angle is where a ripple field aliases worst and was not measured on its own. The seams were tested on shrunk rings, not the erg's own.
