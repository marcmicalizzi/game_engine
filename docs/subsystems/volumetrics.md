# volumetrics (systems)

**The participating medium's mid tier**: the dust the wind holds over the ground, the sand blowing off the dune crests, and the far-field clouds, drawn by one frame pass at the renderer's `AfterResolve` registration point ([renderer](renderer.md#frame-passes)). It is the first built piece of plan 04's "one field rendered three ways" ([04 §4.2](../plan/04-renderer.md#42-frame-architecture), the 2026-09-24 direction note under step 7; [13 §13.1](../plan/13-reference-consumer-games.md#day-night-and-weather), "three media at three scales, one field"): **froxels and a ray-marched far-field layer, no brick solve** — the tier between the baseline's flipbooks and the RT tier's near-field solve. Roadmap R47 (the medium), W45 (the plumes), R83 (the far clouds), P7.

## Owned data

- `MediumDesc`: the scene's `medium` block (`engine.scene.Medium`, `schemas/volumetrics.schema`), which the renderer carries uninterpreted (`SceneData::medium`, `engine.scene.Scene` version 8) and `read_medium` reads.
- `MediumPass`: the frame pass — its pipelines (`shaders/medium.slang`), one host-visible `MediumParams` block per view per frame slot, and every view's froxel grid (device-local, four halves a cell) — and a `renderer::TerrainSampler` of the scene's terrain for the dust's ground and the dunes' field for the wind record.

## The shape

One frame pass, three compute passes under one timer zone, `medium`, after the resolve has written the frame's linear radiance and its sky:

1. **Fill** the view-aligned froxels: a 3D grid of 160 × 90 × 64 per view (`volumetrics.froxels.x`, `.y`, `.z`), each cell its extinction and the light it scatters towards the eye per metre, four halves (8 bytes) a cell, 7.4 MB a view. **The slices are exponential in the distance along the pixel's ray** from `volumetrics.froxels.near_m` (0.5 m) to `volumetrics.froxels.far_m` (1.5 km): slice k ends at near · (far/near)^(k/64), 12.6% deeper than it began, so a plume ten metres away is sliced as finely as the haze a kilometre away is coarsely. Radial rather than view depth, because a pixel's distance is what the medium integrates over and what the apply looks up.
2. **Integrate** each column front to back: a cell of extinction σ and source S over its length l adds S (1 − e^(−σl)) / σ through the transmittance in front of it — the energy-conserving form, exact for a constant cell — and then holds the light and the transmittance from the eye to its far boundary.
3. **Apply** per pixel: the pixel's distance from its visibility word's depth (the high 32 bits, reversed-Z, through the view's inverse view-projection), the column at that distance — bilinear across the four nearest columns, linear between slice boundaries — and `colour · T + L_in · exposure` into the linear image, where `exposure` is the frame's (`gfx::SkyFrame`) so the medium's light is exposed as the resolve exposed the surface's. **Past the far boundary the dust is closed form**: its optical depth along the ray (below) and its unshadowed in-scatter, so a sky pixel or a dune 5 km out sees the whole column of haze, not 1.5 km of it; the horizon's haze is what the closed form gives a ray that never leaves the dust.

A pixel the pass leaves untouched (no medium in front of it) is not written. The renderer's encode quantizes the image afterwards ([renderer](renderer.md#frame-passes)).

## The scattering model

**Single scattering**, a source at each point of

    S(x, ω) = σ_s · [ E_sun(x) · (p(ω·sun) · V(x) + k_multiple / 4π) + L_ambient ]

- **The phase function** is Henyey–Greenstein: sand strongly forward, `k_dust_phase_g` = 0.75 (fine dust is 0.6–0.8 at visible wavelengths). Looking towards the sun through a storm is the bright forward lobe; looking away, the haze is the dust's colour lit from behind.
- **E_sun** is the sun's illuminance outside the air (`gfx::SkyParams::sun_illuminance`) through the air above the point (`sky_light_at`, the transmittance table at the point's altitude: the eye's altitude plus its height in the frame), so a medium at dusk is lit red and at night not at all; the stand-in's white intensity without a sky.
- **V** is the sun's visibility: the cascaded maps where the frame draws them (`--shadows csm`, or `auto` on a device without ray queries), one gather of the 2 × 2 texels round the point in the first cascade that holds it against its own depth plus two texels of bias (no receiver plane: a froxel is not a surface). Outside every cascade, and under traced shadows, the medium is unshadowed: shafts through a ray-traced frame are the RT tier's (R83).
- **k_multiple** (`k_dust_multiple`, 0.35) is the one constant term for light scattered more than once: an isotropic share of the unshadowed sun, which therefore reaches into a shadow. No multiple scattering beyond it.
- **L_ambient** is the sky's: the mean of the nine harmonics' ambient radiance facing up and facing down (`sky_ambient`), which for an isotropic share is the sky's mean radiance; the stand-in's 0.35 of its sky without one.
- **The albedo** `k_dust_albedo` (0.93, 0.83, 0.68): the sand's hue, lighter, since a fine grain scatters more of every wavelength than a dune's surface reflects.

## The dust haze

**A height fog of sand whose density at the ground is a function of the wind record at the world's clock** (`dust_state`; the wind is `terrain::WindRecord::wind_at`, the one wind every consumer reads, [terrain](terrain.md#storms)): with s the hour's strength over the record's mean,

    σ_ground = (k_dust_clear + k_dust_haze · s² + [storm hour] k_dust_wall · (s / 2.5)²) · scale

- `k_dust_clear_per_m` = 3.9 × 10⁻⁵: a calm's floor, 100 km of visibility at the ground.
- `k_dust_haze_per_m` = 4 × 10⁻⁴ times s²: the day's wind's haze (the square: the sand a wind lifts over the speed it carries it at). The record's mean day is a faint haze of about 10 km.
- `k_dust_wall_per_m` = 0.08 at a storm's 2.5 times the mean, by its square: **a storm hour is a wall**, the hour's half-sine strength of the storm (terrain.md, "Storms") taking visibility to tens of metres at its peak. A storm hour's flag comes from the record, so a strong afternoon wind on the storm erg (s ≈ 2 at its diurnal peak) is a haze of a couple of kilometres and not a wall.
- **The scale height** is `height_m` (the block's, 15 m by default) × (0.2 + 0.8 s): 3 m in a calm, 15 m on the mean day, 33 m in a storm's peak — saltation stays within a few metres of the ground, suspension rises.
- **The ground** is the terrain under the eye (`TerrainSampler::height`, the provider the renderer drew it with), so the haze is measured from where the viewer stands and settles into the interdunes below a crest; without a terrain, the world's y = 0. A single reference height for the frame rather than the ground under each froxel: a dune 60 m above the eye's floor sits in thinner dust than it should and a basin below it in thicker, which a per-froxel ground (a height map round the eye) would fix.
- **`visibility_m`** is the distance at which a horizontal ray's transmittance at the eye's height falls to 2%: −ln 0.02 / σ(h_eye) = 3.912 / σ, capped at 1,000 km. It is the field's number, computed on the CPU from the same function the GPU fills the froxels with, handed to the renderer's `Stats::visibility_m` and engine-view's summary, because **gameplay reads visibility from the field, never from the picture** (plan 04). It counts the dust; the air's own extinction is the sky's.

| Hour | s | σ at 1.7 m | Scale height | `visibility_m` |
|---|---|---|---|---|
| A calm day's noon | 0 | 2.2 × 10⁻⁵ /m | 3 m | 177 km |
| The record's mean day | 1.0 | 3.9 × 10⁻⁴ /m | 15 m | 10.0 km |
| A storm's peak (2.5) | 2.5 | 0.078 /m | 33 m | 50 m |
| The storm erg's strongest storm hour | 2.99 | 0.113 /m | 39 m | 35 m |
| The erg's walk at 16:00 (a storm hour, s 1.42) | 1.42 | 0.0245 /m | 20 m | 159 m |

The closed form along a ray (`dust_optical_depth`, the same in the shader) integrates σ₀ e^(−h/H) above the dust's ground and σ₀ below it: for a ray of vertical component dy from the eye, σ₀ e^(g/H) H/dy (e^(−a dy/H) − e^(−b dy/H)) to where it crosses the ground going down, and σ₀ per metre after. The test holds it to a sum along the ray within 0.2%.

## Determinism

**No history.** Every source is a function of the frame's state — the block, the wind record at the world's clock, the eye, the camera, the sun — and of no previous frame: no temporal reprojection, no accumulation, no jittered sample sequence over frames. The same frame is the same bytes, in any renderer and any number of times (`volumetrics_gpu_tests.cpp`). Not part of the simulation's replay: the medium is drawn, and the one number gameplay reads, `visibility_m`, is the CPU's function of the record and the eye.

## LOD policy

The plan's three tiers of the one field (04 §4.2): **baseline** — flipbook particles and a height fog, no froxels; **mid** — froxels and a ray-marched far-field layer (**this module**); **RT** — the near-field brick solve with the froxels around it and the far layer ray traced. Within the mid tier the grid is the tunables' and does not change with distance (the exponential slices are the distance LOD), and the dust past the grid is closed form. A view whose region the renderer resamples (a Panini view) is skipped: its region is not its picture.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way. The renderer gained a registration point for it (`frame_pass.h`) and carries the scene's block uninterpreted; it names nothing of this module.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | `engine_capability_requires(volumetrics terrain)`: the wind record and the crest lines | declared; off when the terrain is off. The sky capability is not linked: the medium reads the frame's `gfx::SkyParams`, which the renderer fills |
| Schema types | `engine.scene.Medium` and its blocks, `schemas/volumetrics.schema` | the scene file's `medium` block |
| Tick scheduler entry | none | the medium is drawn, not simulated; gameplay reads `visibility_m` |
| Render-graph passes | the renderer's frame pass at `AfterResolve` (`SceneRenderer::add_frame_pass`): `medium fill`, `medium integrate`, `medium apply` | every read and write declared; no barrier of its own |
| Content-build derived step | none | nothing is precomputed |
| Protocol methods | none | engine-host's `render.*` registers no medium yet |
| Tunables | `volumetrics.froxels.x`, `.y`, `.z`, `.near_m`, `.far_m` | read when a pass is made |
| LOD policy | above | the mid tier |
| Determinism | above | no history |
| Zero cost when unused | a scene with no `medium` block registers no pass, and the renderer then makes no linear image and draws no encode: the frame is the bytes it was | plan 11 §11.10's "not registered, not run" |
| Tests and size table | `tests/volumetrics_tests.cpp` (no device), `tests/volumetrics_gpu_tests.cpp`, `tests/size_table.cpp` (`MediumParams`, 512 bytes) | |
| Removal proof | `ENGINE_WITH_VOLUMETRICS`, off in the minimal build and with the terrain | engine-view carries a scene's block, logs that it cannot draw it, and refuses `--medium-debug` |

## Public API

`systems/volumetrics/volumetrics.h`: `MediumDesc`, `read_medium`, `MediumDebug`, `parse_medium_debug`, `medium_debug_name`, `MediumWind`, `wind_now`, the dust's constants (`k_dust_*`, `k_visibility_contrast`, `k_max_visibility_m`), `DustState`, `dust_state`, `dust_extinction`, `dust_optical_depth`, `visibility_m`, `MediumOptions`, `MediumStats`, `MediumParams` (GPU-mirrored), the `k_source_*` bits, and `MediumPass` (`create` registers the frame pass on a renderer, `destroy` removes it, `stats`).

## Testing

`volumetrics_tests.cpp`, with no device: the block reads with its defaults (dust and plumes on, no clouds) and refuses an unknown field and a cloud base above its top, each with the field's path; the dust is a function of the wind — calm over 100 km, the mean day 3–15 km, a storm's peak 10–100 m and under a tenth of the calm, scale heights a few metres, 15 and over 25, never clearer for a stronger wind, the block's scale multiplying it and `enabled: false` none; its closed-form optical depth against a sum along rays up, level and down across the ground within 0.2%; and the committed storm erg's strongest storm hour sees under a tenth as far as its calm noon, and the same hour twice is the same wind and the same number. `volumetrics_gpu_tests.cpp`, offscreen (the private desktop's rule): over the stand-in sky and under a sky with the cascaded maps, the medium changes most of the picture, the same frame is the same bytes twice, the frames after the first allocate no engine memory and no device buffer, and the frame after the pass is destroyed is the frame before it to the byte.

## Not yet

The crest plumes and the far-field clouds (the following stages of this change); a per-froxel ground; the brick field and the near solve (the RT tier); the baseline tier's flipbooks; the clouds' shadow on the ground (R83, it needs the resolve); engine-host's `render.*` registering a medium; E32's measurement.
