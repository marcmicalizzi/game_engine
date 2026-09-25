# Third interactive session, 2026-09-25: the dune generator, flown

- **Question ([13 §13.1](../plan/13-reference-consumer-games.md#dynamic-sand-the-defining-system), [ADR-0043](../adr/0043-dunes-as-a-function-of-time.md) "Revisit when"):** does the terrain capability's dune field read as dunes when the owner flies it, and what does the first look ask of the generator and the renderer?
- **Machine:** the RTX 5090 host, NVIDIA Surround at 11520×2160 (one 82 Hz display), the release build of main `432b182`, `--shadows csm`, the display pacer on by default ([apps](../subsystems/apps.md#pacing)). Other processes during the flight: about 13% of the CPU and 7% of the GPU at the end of the run (the frame log's `machine_state`).
- **Scene:** `content/test-scenes/desert-dunes/scene.json` copied to the owner's scenes as `fly-dunes.json` — the desert overlook's 5.1 km, its two rock ridges, third ridge and oasis basin as fixed features, `dune_height` 8 m, `dune_wavelength` 110 m, `generator: Dunes`, `time` 94,608,000 s (three years). No meshes.
- **Recording:** `D:\workspace\game_engine_local\flythrough\dunes-2026-09-25T1745-{input,frames}.jsonl` (owner's machine, not committed): 12,138 frames, three marker presses (`M`, scancode 16, ticks 32,212, 33,929 and 34,538).
- **Decision:** three items for the generator (a band table with a scale spectrum, a time-lapse mode, finer sampling near the camera), one for the renderer (sand in shade needs the ground's bounce), recorded in the pages named below; nothing changes in the generator's representation yet.

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

So the seams are neither the shadow maps nor the geometry nor the material. They are **the slip faces, lit by the sky alone.** Every slip face is on the downwind side and stands at the angle of repose, so from upwind the top edge of each lee face shows over the crest as a thin dark line, broken where the crest undulates, and from downwind the whole face is one dark plane. Its shading is the sun at N·L ≤ 0 plus the sky's diffuse term, which is cool and weak next to the sun; a real slip face is nearly as warm as the lit side because the sand around it is bright and bounces light into it (compare the photographs the owner sent: the shaded faces are dark orange, never grey). **The renderer has no ground bounce**, so shade on sand reads as sky-grey. The hard edges are the mesh: 2,049 samples over 5.1 km is 2.5 m a sample, and a 34° face 12 m long is five samples.

## What the scale question is

The scene gives the generator one number, `dune_height`, and the three bands derive their heights from it by fixed ratios ([terrain](../subsystems/terrain.md#the-time-function): draa 0.75–1.25 H, crests 0.3–0.6 H, barchans 0.2–0.45 H; ripples 6 mm). At H = 8 m the whole field is the small end of a real erg's spectrum. The physics of the representation already fits the large end — Bagnold's rule gives a 150 m draa a speed a twentieth of a 8 m dune's, so it stands still while the small forms crawl over it — but the bands, their heights, their spacings and how sparsely each is placed are constants in `dunes.h`. The fix is a **band table in the scene**: a mega-draa band (tall, kilometres apart, nearly stationary), the present three, a wave band between ripples and dunes, and a share of empty cells per band so flat interdune floors exist.

## What the time question is

`time` is a scene constant. At the default wind the draa move about 25 m a year and the barchans 77 m, so a real clock shows nothing in a flight, and the question the ADR asks the owner's eyes — do the migrating lattices read as dunes advancing or as a texture sliding — cannot be answered without a **time-lapse**: game time advancing at a day to a week per real second, the field re-evaluated on the job system every second or so and re-uploaded. The closed form makes that a re-evaluation, not a simulation.

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
- The renderer's next work for the desert is an indirect term for the ground: a hemisphere ambient whose lower half carries the terrain's own albedo, so a slip face in shade is lit by the sand around it. It is the smallest thing that removes the grey; a real bounce is Phase 4's.
- It does not decide whether migration reads right, because nothing moved. That question waits for the time-lapse.
