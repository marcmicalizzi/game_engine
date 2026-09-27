# Third interactive session, 2026-09-25: the dune generator, flown

- **Question ([13 §13.1](../plan/13-reference-consumer-games.md#dynamic-sand-the-defining-system), [ADR-0043](../adr/0043-dunes-as-a-function-of-time.md) "Revisit when"):** does the terrain capability's dune field read as dunes when the owner flies it, and what does the first look ask of the generator and the renderer?
- **Machine:** the RTX 5090 host, NVIDIA Surround at 11520×2160 (one 82 Hz display), the release build of main `432b182`, `--shadows csm`, the display pacer on by default ([apps](../subsystems/apps.md#pacing)). Other processes during the flight: about 13% of the CPU and 7% of the GPU at the end of the run (the frame log's `machine_state`).
- **Scene:** `content/test-scenes/desert-dunes/scene.json` copied to the owner's scenes as `fly-dunes.json` — the desert overlook's 5.1 km, its two rock ridges, third ridge and oasis basin as fixed features, `dune_height` 8 m, `dune_wavelength` 110 m, `generator: Dunes`, `time` 94,608,000 s (three years). No meshes.
- **Recording:** `D:\workspace\game_engine_local\flythrough\dunes-2026-09-25T1745-{input,frames}.jsonl` (owner's machine, not committed): 12,138 frames, three marker presses (`M`, scancode 16, ticks 32,212, 33,929 and 34,538).
- **Decision:** three items for the generator (a band table with a scale spectrum, a time-lapse mode, finer sampling near the camera), one for the renderer (sand in shade needs the ground's bounce), recorded in the pages named below; nothing changes in the generator's representation yet. **Corrected the same evening:** the ground's bounce went in and did not move the seams, which are a shading normal turned from the camera and not shade ([below](#the-grounds-bounce-measured-2026-09-25-evening)).

## What the owner saw

1. **Grey seams on top of the dunes, in one direction.** Thin dark grey dashes along the crests when looking downwind; flat dark grey slabs with hard, angular edges when looking upwind at a slip face.
2. **The dunes feel small.** Ripples in the sand and small moving dune-like forms are there; towering dunes are not, and neither are the flat stretches between them. The reference he had in mind: ergs with dunes of a hundred metres and more, medium dunes on their flanks, ripples on everything, and open interdune floors.
3. **No visible movement.** The scene is evaluated at one time; nothing moves during a flight.

## What the captures say about the seams

The three marked views were rendered again offscreen from the recording (`--replay-input … --marker-captures`, 1920×1080), four ways:

| Render | Seams |
|---|---|
| `--shadows csm` (as flown) | present |
| `--shadows off` | present, pixel for pixel the same |
| `--view normals` | none: the normals are smooth across every seam |
| `--view albedo` | none: one sand colour everywhere |

So the seams are neither the shadow maps nor the geometry nor the material. They are **the slip faces, lit by the sky alone.** Every slip face is on the downwind side and stands at the angle of repose, so from upwind the top edge of each lee face shows over the crest as a thin dark line, broken where the crest undulates, and from downwind the whole face is one dark plane. Its shading is the sun at N·L ≤ 0 plus the sky's diffuse term, which is cool and weak next to the sun; a real slip face is nearly as warm as the lit side because the sand around it is bright and bounces light into it (compare the photographs the owner sent: the shaded faces are dark orange, never grey). **The renderer has no ground bounce**, so shade on sand reads as sky-grey. The hard edges are the mesh: 2,049 samples over 5.1 km is 2.5 m a sample, and a 34° face 12 m long is five samples. *(This paragraph's diagnosis is wrong, and the evening's measurement says why: the seam pixels face the sun and their normals point nearly straight up; the resolve drops the sun on them because their shading normal faces away from the camera — [below](#the-grounds-bounce-measured-2026-09-25-evening).)*

## What the scale question is

The scene gives the generator one number, `dune_height`, and the three bands derive their heights from it by fixed ratios ([terrain](../subsystems/terrain.md#the-time-function): draa 0.75–1.25 H, crests 0.3–0.6 H, barchans 0.2–0.45 H; ripples 6 mm). At H = 8 m the whole field is the small end of a real erg's spectrum. The physics of the representation already fits the large end — Bagnold's rule gives a 150 m draa a speed a twentieth of a 8 m dune's, so it stands still while the small forms crawl over it — but the bands, their heights, their spacings and how sparsely each is placed are constants in `dunes.h`. The fix is a **band table in the scene**: a mega-draa band (tall, kilometres apart, nearly stationary), the present three, a wave band between ripples and dunes, and a share of empty cells per band so flat interdune floors exist.

## What the time question is

`time` is a scene constant. At the default wind the draa move about 25 m a year and the barchans 77 m, so a real clock shows nothing in a flight, and the question the ADR asks the owner's eyes — do the migrating lattices read as dunes advancing or as a texture sliding — cannot be answered without a **time-lapse**: game time advancing at a day to a week per real second, the field re-evaluated on the job system every second or so and re-uploaded. The closed form makes that a re-evaluation, not a simulation. *(Built on 2026-09-26, blended in the deformed-vertex pool rather than re-uploaded, with the finer rings near the camera: [below](#the-dunes-in-time-lapse-looked-at-2026-09-26).)*

## The frame log, in passing

The display pacer's first live outing at 11520×2160 ([second session](second-interactive-session-2026-09-25.md#present-pacing)):

| | this flight | the 12:26 flight before the pacer, replayed |
|---|---:|---:|
| frames | 12,138 | 1,610 |
| frame ms median / p95 / p99 | 12.25 / 13.67 / 14.59 | 12.20 / 24.39 / 36.18 |
| frames over 20 ms | 8 | 243 |
| CPU ms median | 0.31 | — |
| clusters | 184,861 | — |

The eight long frames are the scene's first seconds. 1,674 MiB of device memory.

## What this decides, and what it does not

- The generator's next work is the band table, the time-lapse, and finer sampling near the camera (the crest's facets), with the repose limiter the terrain page already lists; all CPU, all closed-form, none of it changes ADR-0043's representation.
- The renderer's next work for the desert is an indirect term for the ground: a hemisphere ambient whose lower half carries the terrain's own albedo, so a slip face in shade is lit by the sand around it. It is the smallest thing that removes the grey; a real bounce is Phase 4's. *(It went in the same evening and did not remove the grey — the next section.)*
- It does not decide whether migration reads right, because nothing moved. That question waits for the time-lapse.

## The ground's bounce, measured (2026-09-25, evening)

The renderer item above went in: the resolve's indirect term is now a hemisphere of the sky above the horizon and **the ground below it** — the scene's ground albedo (the terrain's sand, (0.84, 0.69, 0.47) linear, for this scene) lit by the sun and the sky — blended by how much of a normal's cosine lobe each half fills, and the reference path tracer's escaped rays see the same two halves ([renderer](../subsystems/renderer.md#the-sky-above-the-ground-below)). The three markers were rendered again offscreen from the recording, `--shadows csm` at 1920×1080 as before, with the `msvc-release` binaries of the commit before the change and of the change (both on the RTX 5090, under the GPU lock), and compared with `render.compare`.

| Marker | FLIP mean / p95 / max, before → after | seam pixels before / after | the seams' mean colour before → after |
|---|---|---|---|
| tick 32,212 | 0.0031 / 0.025 / 0.061 | 13,093 / 13,093 | 112.0, 114.8, 108.8 → 112.9, 115.0, 109.0 |
| tick 33,929 | 0.0072 / 0.042 / 0.109 | 7,212 / 7,212 | 112.0, 114.7, 108.7 → 112.9, 115.0, 109.0 |
| tick 34,538 | 0.0061 / 0.037 / 0.154 | 6,653 / 6,653 | 111.9, 114.5, 108.5 → 113.1, 115.1, 108.9 |

A seam pixel here is one below the sky that is darker than the lit sand (red under 150) and nearly neutral (red within 12 of blue); the lit sand is 40 and more redder than it is blue, and nothing but the seams meets the rule. **The pictures, read side by side:** the lit sand is within a level of what it was; a slope in the cascades' shadow in the foreground of the second marker warms by two or three levels (182, 170, 150 → 185, 173, 151), and a steep face near the horizon by up to nine in red (109, 112, 106 → 118, 117, 108) — the ground's bounce doing what it should on a face that leans towards the ground. The seams do not change: the same pixels, the same flat grey-blue within a level, the same hard edges. The first marker's slabs are as dark and as angular as the owner saw them.

**Why: the seams are not shade.** Read against the normals captures the owner's run left beside the colour ones (`--capture-channels normals`), every seam pixel's shading normal points nearly straight up — n.y 0.987, 0.984 and 0.973 on average at the three markers, none under 0.77 — and every one faces the sun, N·L from 0.42 to 0.92 with a mean of 0.73. Shadows are not it either (the owner's `--shadows off` captures have the same seams). And they are all one colour: exactly the sky ambient of an up-facing normal with no sun in it, which the ground's bounce, weighted by (1 − n.y) / 2, changes by a level. The one way the resolve drops the sun from a pixel whose normal faces it is `brdf_eval`'s early return on **n·v ≤ 0**: the interpolated shading normal faces away from the camera while the triangle it lies on faces it. That is what a grazing view across a crest does to a smooth vertex normal leaning over the lee side, and the hard, angular edges are where n·v crosses zero inside a triangle, which is linear there.

**The experiment that settles it.** The same binary with one line of `brdf_eval` changed through a scratch shader manifest (`engine-view --shaders`): n·v clamped to 10⁻⁴ instead of rejected, as Filament's `clampNoV` does. **Every seam pixel is gone at all three markers** (0, 0 and 0 by the rule above), the slabs of the first marker and the crest dashes of the other two alike, and the rest of the picture barely moves (FLIP 0.008, 0.006 and 0.004 against the ground-bounce picture, p95 0.003 to 0.009, the maxima of 0.71 to 0.83 on the seams themselves). What is left on the lee sides is a gentle shading of the dunes' form rather than a hole in it.

**The corpus.** Against the reference path tracer the change is an improvement or noise on every scene: `heightfield` 0.0268 → 0.0235 FLIP, `shredded-atlas` 0.0424 → 0.0390, `thin-geometry` 0.0036 → 0.0035, `helmet-grid` and `wide-frustum` unchanged, `flighthelmet` 0.0128 → 0.0132, a third of that scene's own noise floor; the renderer's four agreement cases went from 0.0131, 0.0140, 0.0215 and 0.0034 to 0.0051, 0.0055, 0.0157 and 0.0015 ([renderer](../subsystems/renderer.md#the-sky-above-the-ground-below) has the table and why the reference's sky had to become uniform above the horizon for that to hold). The shading and attributes tests hold the GPU to the double-precision reference within 1 of 255 with the ground in it, and 0 of 255 on the underside of a quad that faces straight down at it.

**What this decides.** The ground term stays: it is right for every face turned towards the ground, it is what the reference sees, and it brought the two integrators closer. The grey seams are the n·v rejection.

**The clamp, committed.** `brdf_eval` now clamps n·v to 10⁻⁴ instead of rejecting it, in the shader and in its CPU reference, and the resolve turns a shading normal that is behind its triangle to the camera's side as the path tracer always did, so the clamp cannot let a light through thin two-sided geometry ([renderer](../subsystems/renderer.md#a-shading-normal-is-not-the-surface)). The three markers again, same recording and settings: **0, 0 and 0 seam pixels**, the picture within a pixel of the scratch experiment's (FLIP 0.0000 against it at all three), and FLIP 0.011, 0.012 and 0.010 against the pictures the owner flew, the maxima of 0.72 to 0.83 on the seams. Read side by side with the owner's: the first marker's slabs are gone and the dune behind them reads as one surface, lit on its near slope and soft over the crest; the other two markers' dashes along the crests are gone, and the crests show as a change of shade rather than a line. The corpus moved by no more than 0.0003 a scene and PSNR rose on every helmet scene.

What the lee faces look like once they keep their sun is the question for the owner's next flight, and with it one the pictures already ask: under the renderer's stand-in lights the sky lights an up-facing surface nearly as much as the sun does (0.35 of `k_sky`, 0.19 to 0.32 a channel, against the sun's 0.25 per unit of albedo at its elevation), so shade on sand is never far from light, and a real desert's is.

## The dunes in time-lapse, looked at (2026-09-26)

- **Question:** does a time-lapse move the erg on screen without a step anywhere — across the rings' borders and through their re-centres — and what does it cost? ([renderer](../subsystems/renderer.md#the-dunes-in-time-lapse), [the rings](../subsystems/renderer.md#the-rings-in-the-scene); the answer to "What the time question is", above.)
- **Machine:** the RTX 5090 host, `msvc-release` of the time-lapse branch at the commit that adds this section, offscreen and under the GPU lock (no window). The harness's `machine_state` at the runs' starts and ends: other processes' CPU 0–21%, GPU utilisation 0–40%, with one reading of 98% at the end of the still 1080p run — something outside the lock — so the table below is a quiet machine's except where that bites.
- **Scene:** `content/test-scenes/desert-erg` along its own path (600 frames of it, flown three times, 240 frames of warm-up), and three still cameras whose paths are the session's scratch and not committed: 4 m over the mega-draa's stoss near the path's `crest` marker looking east along it (`crest-hold`), 3 m over the floor near the `floor` marker looking east (`floor-hold`), and 120 m inside the inner ring's eastern border, 3 m over the sand, looking at it (`ring-edge`), the rings having been built round a first key 120 m behind.

### What the captures show

Offscreen, 30 frames at a game day and at a game week a real second, a capture every tenth frame; 240 frames at a week a second for the floor. Every run with `--no-lights` (below). The difference of two captures of one run, over the 2,073,600 pixels of 1920×1080:

| Run | Captures compared | Game time between | Mean \|difference\| | Pixels more than 8 of 255 apart |
|---|---|---|---:|---:|
| still (rate 0), rings | 9 → 29 | none | 0.000 | 0 |
| crest, a day a second | 9 → 29 | 8 h | 0.033 | 204 (0.010%) |
| crest, rings, a day a second | 9 → 29 | 8 h | 0.044 | 201 (0.010%) |
| crest, a week a second | 9 → 29 | 2.3 days | 0.063 | 272 (0.013%) |
| crest, rings, a week a second | 9 → 19, 19 → 29 | 1.2 days each | 0.073, 0.045 | 454, 162 |
| floor, rings, a week a second | 59 → 119 | 1 week | 0.285 | 1,333 (0.064%) |
| floor, rings, a week a second | 59 → 239 | 3 weeks | 0.354 | 3,682 (0.178%) |
| ring edge, rings, a week a second | 9 → 29 | 2.3 days | 0.102 | 482 (0.023%) |

**The dunes move, and nothing steps.** The pixels that change are the floor's barchans, the toe lines where the dunes meet the floor, the crests' silhouettes and the edges of the slip faces' shade; none is a block the shape of a field's tile, a ring's chunk or a ring's square, and a still run with the rings in the scene draws the same bytes on every frame. At the ring edge the inner ring's border, 130 m ahead at a grazing angle, shows no seam in a crop with its contrast stretched fourfold, frame after frame. **What the eye gets is slower than the numbers suggest:** the erg's tall dunes are slow by design (the mega-draa a metre a year, the draa 11 m), and the waves, which move most — about 2 m in the 2.3 days of the crest captures — are 0.3–0.8 m on 10 m cells, so from 3–4 m over the sand in a still frame they change the shading by a few levels and the motion that crosses the threshold is the barchans and the toes. Whether a flight at a week a second reads as dunes advancing is the owner's call in a window, which this session could not open.

**And the same run twice draws the same bytes.** The ring-edge run at a week a second, rings and all, run twice: its three captures identical to the byte and every level's last field the same hash (`ea2f082f517c2a75`, `2399944979535f70`, `80e19c5c066abcff`) — offscreen the re-centres, the fields and their times are a function of the frames.

**The first captures were misleading, and why.** The renderer's stand-in lighting (`frame_lighting`) orbits two coloured point lights round the scene with the frame index, so the sand's colour changes from frame to frame whatever the sand does: with them on, three weeks of the floor were 57.6% of pixels apart, and nearly all of it was the lights. A time-lapse is looked at with `--no-lights`.

**Dark specks on the far floor** — single dark pixels a kilometre and more away — are there with and without the rings and with and without shadows, so they are neither; they are older than this work and not chased here.

### Two defects the rings test found

The rings' GPU case (`terrain_motion_tests.cpp`, a 128 m terrain with the rings shrunk to fit, every pixel held to the level that draws it) failed twice before it passed, both times on something that was wrong before the rings:

- **A frame showed field slots its copies had not reached.** A frame recorded at most eight field copies and left the rest for the next, while the table it wrote already named every slot; an offscreen frame that catches the surface up installs several fields a level, each into the slot the last one freed, so a ring drew for a frame whatever its slot had held — 7–16 mm at 25 cm, a frame's worth of sand. The scene grid alone had never installed enough in one frame to be caught. Now a field goes over a budget a frame and is taken as b only once its last piece is in a frame, and a frame that must show a slot copies it whole.
- **The scene grid's hole folded at its corners.** The grid leaves the middle ring's square out by moving its vertices inside onto the nearest edge, and no such map is without a fold: a triangle whose corners went to two edges spanned the square's corner at the edges' heights and stood up to 1.3 m proud of the ring there. The vertices are now sunk by the distance they were moved, which puts those triangles under the ring's ground; with both fixed the case holds every level to its model within 0.2 mm over 2.4 million pixels and 9 re-centres.

### What it costs

The erg's path, 1,800 frames recorded a run, offscreen at a game day a second (the fields waited for, so every field is taken whole in the frame that needs it). GPU milliseconds from `gfx::GpuTimer`, median / 95th / 99th percentile:

| Run | Pairs drawn (median) | Pool pass | Pool allocator | Frame total |
|---|---:|---|---|---|
| still, 1920×1080 | 1,006 | — | — | 0.42 / 0.65 / 0.79 |
| time-lapse, 1920×1080 | 1,134 | 0.016 / 0.024 / 0.41 (max) | 0.008 / 0.010 | 0.48 / 7.60 / 8.15 |
| time-lapse and rings, 1920×1080 | 1,543 | 0.026 / 0.56 / 1.38 (max) | 0.012 / 0.031 | 1.17 / 12.1 / 38.3 |
| still, 11520×2160 over three views | 2,476 | — | — | 1.65 / 2.21 / 2.75 |
| time-lapse, 11520×2160 | 2,797 | 0.058 / 0.53 / 0.85 (max) | 0.018 / 0.030 | 2.03 / 9.50 / 21.1 |
| time-lapse and rings, 11520×2160 | 3,519 | 0.075 / 1.21 / 2.20 (max) | 0.024 / 0.047 | 3.28 / 24.6 / 50.4 |

- **The pool pass is not the cost.** It is hundredths of a millisecond at the median; the tail of the frame total is the copies.
- **Evaluation**, on the job pool: the erg's whole grid (4,097² samples) 750–775 ms, a middle ring's field (1 m over its 2 km square) 190 ms, an inner ring's (50 cm over 500 m) 72 ms. The worker keeps up at a day a second offscreen by waiting; in a window the lead rule times the grid's fields about 1.5 × 0.77 s × 86,400 = 28 game hours apart, where the displacement rule has them 7 game hours apart at most, so there the waves cross-fade rather than slide.
- **Copies:** 14.6 GB of fields in the time-lapse runs, 1.55 s of GPU — 7.1 ms for each of the grid's 67 MB fields (9.4 GB/s), which is the whole of the 95th percentile at 1080p. Hence `renderer.terrain.upload_mib` (8 MiB, under a millisecond a frame) in a window.
- **Rings:** 1.46 GB of slots and arenas on the device, and 3.3–3.9 s on a load (a two-frame run of the erg took 8.4 s with them and 4.5–5.2 s without: their build round the camera, their slots and their first upload). 107 re-centres over the three flights of each run (the path crosses about 3 km, flown in 600 frames); a re-centre's chunks 0.5 s on the ring worker at the median and 3.4 s at worst (a middle ring's), the pairs carried over 0.23–0.29 s (0.53 s at worst); 8,111 chunks built and 26,496 kept, 91 MB of chunks a re-centre, the arenas at most 69% full. Offscreen the chunks go over in the frame that asks, which is the rings' 99th percentile; in a window, `renderer.terrain.ring_upload_mib` (16 MiB) a frame.
- **Padding:** the grid's reached 15.6 m after six weeks of game time, the rings' stayed under 0.8 m (a re-centre re-bases the chunks it rebuilds).

### What this decides, and what it does not

- The time-lapse's mechanism stands: two fields and a blend in the pool, one surface time for every level, the cadence by displacement. What it costs a frame is the copies, now spread over frames, and what it costs the machine is the evaluations, which the lead rule stretches.
- The rings are worth drawing near the ground and cost their clusters and 1.5 GB; they stay off by default until the owner has flown them.
- The grid's padding grows with the time-lapse (15.6 m in six weeks) and its LOD errors with it: re-basing the grid in the background is the next piece ([renderer](../subsystems/renderer.md#not-yet)). The rings re-base on every re-centre; a still camera's rings age like the grid.
- It does not decide whether migration reads as dunes advancing: that needs the owner's eyes on a window.

## The owner's flight in time-lapse (2026-09-27)

The owner flew the erg in a window (RTX 5090, 11520×2160 surround, cascaded shadows) with `--time-rate` at the default week a second, at a day a second and at 8,640 (a tenth of a day). Three findings, none of them in the offscreen captures above:

- **Clear steps at a week and at a day a second; smooth at 8,640.** The design's promise is a per-frame *displacement* bound, and the tests hold it; what the eye reads is *velocity*. When every field is late — a whole-grid field costs 750–775 ms and at a week a second the cadence asks for one every quarter second — the surface holds at `b` and then catches up at the capped speed, a quarter of a spacing a frame, which on the 1.5 m grid is 22 m/s against the barchans' true 1.5 m/s: hold, burst, hold, about once a second. At 8,640 the fields keep up and the blend is continuous. The fix is a surface clock that follows the arrivals — render behind game time by an adapted latency, stretch when late, never catch up faster than about 1.5× the true rate — with the displacement bound kept as a safety cap, plus a faster evaluation; both are briefed to the desert-generator cloud session (`cloud-briefs/dune-motion.md`, outside the repository), because a storm sequence will run at a week a second and the volumetric will not hide hold-then-burst.
- **The sun moves too fast to see the sand move.** The stand-in lights orbit with the frame index (above: 57% of pixels changed from lighting alone). A fixed sun for time-lapse flights is in the same brief.
- **The largest dunes' crests never move.** The sand round a mega-draa's crest fills and empties, but the crest line stays. Bagnold's rule makes a 100–200 m dune nearly stationary, so some of this is the physics the generator was built on; whether it is *only* physics (the crest advancing by the band's travel, not anchored to its lattice cell) is to be measured, and a per-band celerity scale for cinematic rates proposed, in the same brief.

What this adds to the decision above: the time-lapse's mechanism stands, and its pacing rule does not — "a late field costs a moment of stillness and never a step" is true of displacement and false of what a viewer sees at the rates a sandstorm needs.
