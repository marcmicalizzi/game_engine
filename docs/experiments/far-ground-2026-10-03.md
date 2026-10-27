# Ground to the horizon: far levels past the world's tiles (2026-10-03)

## 2026-10-26 F20: the forest mountain's error

The owner sees the opposite mountain as rounded and blotchy. This measures the geometric
part before choosing a finer far lattice. The original far-ground flight below remains the
historical baseline; these are different data, cameras and machine state.

**Sources and definitions.** Read ADR-0050/0051, the renderer's complete "Ground to the horizon"
section, and the tile set's exact trapezoidal box filter. [Losasso and Hoppe (2004), §9 error
analysis](https://hhoppe.com/geomclipmap.pdf) measures error against the reconstructed piecewise
linear surface and reports percentiles separately from maxima; that distinction matters here.
Its numerical terrain results are not transferred to this authored mountain.

Read-only inputs: the POC's `reports/quality-v15/engine-c1ee971d/runs/ridge-csm-wide-20261026-191449/scene.json`,
`run.json` and `Reproduce.cmd`, `surface-audit/markers.json` (entry 5, one-based), and the cook
`world/v14/ddc/tilesets/0642f2422dcb4e1a/manifest.json`. The manifest and source descriptor say
**0.5 m**, 4097×7169 samples, origin (-1024,-768) m, data hash `bd4d4be5d83f1bd6`, far filters
16,32,64,128,256,512 m. The scene has **64 m** world tiles and ring cells 128,64,32,16 (4 m at
the outside), radius 20 tiles. These correct the brief's 1 m source/32 m tiles. The actual ridge
camera is (590,259.1289978027344,1430), looking at (150,560,2000); tag 5 is
(5.2993751246767715,262.3149009019297,1552.9847968086178), yaw 2.5136375427246094,
pitch 0.18588769435882568 radians. The scratch camera uses the engine's
`(-cos(pitch) sin(yaw), sin(pitch), -cos(pitch) cos(yaw))`, FOV 55°, held for two seconds.

**A bound that is true.** For cell min `m`, max `M` and box mean `mu`,
`max|h-mu| = max(M-mu,mu-m) <= M-m`. Half the range bounds distance to the **midrange**, not
generally to the mean. A single narrow peak disproves a universal half-range claim. A mesh
interpolates neighbouring box means, so its conservative support includes their boxes too;
one box's range is not a global triangle-error certificate. Likewise p95 leaves a 5% tail.
A plane's range grows with spacing although its filtered triangulation is exact: ranges are
conservative proxies, not direct evidence that a mountain has lost that many metres of shape.

**Census method.** Scratch `measure.ps1` reads the frozen float32 source, quantizes once to
integer millimetres as the set does, enumerates world-aligned square box supports at each
spacing, includes those whose centre has height >=200 m and z>=1024 m, and requires the whole
support inside the authored source. Min/max are over all source vertices in a box (extrema of
the bilinear surface); the mean is the exact separable trapezoidal integral. Nearest-rank
percentiles, one value per box. CPU arithmetic is independent of concurrent machine load.
Raw counts and histograms: `D:/workspace/ge-codex-scratch/sol-far/mountain-errors.json`.

| spacing, m | boxes | range median, m | range p95, m | mean-deviation p95, m |
|---:|---:|---:|---:|---:|
| 2 | 384765 | 1.577 | 2.557 | 1.288 |
| 4 | 96138 | 3.153 | 5.107 | 2.585 |
| 8 | 24002 | 6.296 | 10.206 | 5.198 |
| 16 | 5995 | 12.611 | 20.393 | 10.473 |
| 32 | 1492 | 25.440 | 40.789 | 21.182 |
| 64 | 374 | 49.761 | 82.483 | 42.263 |
| 128 | 89 | 102.183 | 169.411 | 92.170 |
| 256 | 20 | 196.191 | 292.252 | 166.215 |
| 512 | 6 | 316.339 | 426.865 | 270.040 |

`0.75 m/m` rounds above every measured p95 mean-deviation/spacing, including 128 m's 0.7201.
It is a **scene-specific percentile proxy**, not a proven terrain-wide bound, especially with
only six 512 m supports. At a 55° vertical FOV, `P=H/(2 tan(FOV/2))`; one pixel at 2 km is
1.446 m at 2560×1440 and 0.964 m at 11520×2160, and two pixels twice that. At 5 km the
one-pixel thresholds are 3.615/2.410 m. At the conservative first far start (1040 m), one pixel
allows 0.752/0.501 m. Even the smallest permitted first spacing, 4 m, carries 3 m under the
chosen envelope: **requests of 1 and 2 pixels must be refused**, not silently clamped or labelled
as satisfying their error. Refining near rings is outside F20.

**Reconstruction cross-check.** Scratch `reconstruction.ps1` integrates the same quantized
bilinear source for the four coarse vertices, uses the engine's two triangles per cell, and
measures the largest residual at source vertices in each interior coarse cell. This excludes
seam fans and cluster simplification, and uses unrounded box means, so it is a diagnostic,
not a rendered-error certificate. It selects the same height/z region by cell centre, with all
four box supports inside the data. Results (`reconstruction-errors.json`):

| spacing, m | cells | per-cell maximum residual median / p95 / maximum, m |
|---:|---:|---:|
| 4 | 96062 | 0.043 / 0.152 / 1.557 |
| 8 | 23968 | 0.172 / 0.407 / 4.912 |
| 16 | 5958 | 0.538 / 1.271 / 12.521 |
| 32 | 1481 | 1.474 / 4.975 / 26.276 |
| 64 | 359 | 3.675 / 20.086 / 38.471 |
| 128 | 89 | 13.434 / 40.876 / 49.915 |
| 256 | 20 | 39.250 / 90.246 / 93.992 |

The residual grows faster than linearly over several levels; doubling distance does not in
general justify doubling spacing without checking the error table. The CPU rule checks all
levels. A spatial residual mip would avoid assigning the mountain's worst percentile to empty
or flat distant terrain; this change adds no source-format or provider-interface data.

**Implementation and limits.** The optional settings rule, explicit measured-table API,
override precedence, preserved reach and the eight-far/sixteen-total refusal
are specified in [renderer](../subsystems/renderer.md#ground-to-the-horizon). Default zero is the
old layout. The rule runs once at load, with no frame-loop work. A finer chain requires a new
cook in scratch (the original cook only answers 16–512 m); the POC and its cache stay read-only.
The blotchy distant colour belongs to R96/R97, and authored ribs/gullies remain game work.

- **Question.** From a few hundred metres up the endless desert ended at the world's outermost ring, 2 km out, in a straight edge against the sky model's planet. How far must the ground reach at 2 m, 100 m, 350 m and 2,000 m over the erg's sky, how many coarser levels does that take, what do heights filtered to a level's spacing buy over point samples, and what do the levels cost? ([renderer](../subsystems/renderer.md#ground-to-the-horizon), [ADR-0051](../adr/0051-the-far-ground-is-the-renderers.md), proposed.)
- **Date:** 2026-10-03. **Machine:** the owner's desktop (RTX 5090, 36 logical CPUs). The arithmetic, the filter and the tests on `msvc-debug`; the captures and the cost on `msvc-release`, each run alone under the GPU lock with `--wait-quiet 60`. **Machine state** (the harness's `machine_state`): other agents' CPU at 1.7–10.8% at the start and end of each measured run, the GPU idle (0–1%) at each start, 9.3–10.0 GB of device memory held by other processes, and the owner's session **locked** throughout; the two runs with six far levels did not reach the quiet threshold in 60 s and ran anyway (`quiet: false`), the others did.
- **Status:** the arithmetic, the filter, the tests, the captures from 2 m to 2,000 m at 10:00 and 18:00, the hand-over at the far edge (one more far level: six by default) and the offscreen cost are here. **The windowed lag is not**: the owner's session was locked, and a locked session presents at 4 Hz (below, "In a window"). The third pass (on `main` after the shared request) measured what a far square's move costs and made its pairs and its scan cheaper (below, "A far square's move"), corrected the reading of the allocations and of the far tiles built, and captured the owner's 350 m start (below, "The captures"); the session was locked again.

## How far the ground has to reach

The erg's sky is turbidity 1.6: aerosol of vertical depth 0.065 at 550 nm over a 1.2 km scale height (`sky::earth_atmosphere`) and Rayleigh's 0.0136 /km over 8 km. Along a ray from height *h* down to the ground *d* away (a flat path), the green transmittance is `exp(-d (β_R f(h, 8) + β_M f(h, 1.2)))` with `f(h, H) = (H / h)(1 - e^{-h/H})`:

| eye height | extinction along the way, /km | air down to 10% | the drawn table's 32 km leaves | true horizon √(2Rh) | where flat ground meets the planet's horizon line √(Rh/2) |
|---|---|---|---|---|---|
| 2 m | 0.0678 | 34 km | 11% | 5.0 km | 2.5 km |
| 100 m | 0.0655 | 35 km | 12% | 35.7 km | 17.9 km |
| 350 m | 0.0603 | 38 km | 15% | 66.8 km | 33.4 km |
| 2,000 m | 0.0384 | 60 km | 29% | 160 km | 79.8 km |

- **The reach the brief asks for** — the nearer of the haze and the horizon — is about 34 km at 2 m (the horizon of the eye is 5 km, but the mega-draa's 80–200 m crests stand above it to 45 km, so the haze decides), 35 km at 100 m, 38 km at 350 m and 60 km at 2,000 m.
- **The drawn air ends at 32 km** (`k_sky_aerial_distance_km`): ground past it keeps the 32 km transmittance in the third column, so from high up nothing past 32 km is taken any further into the haze, however far the ground goes. That is `sky.slang`'s, which this change does not touch.
- **Levels.** With the defaults (the first far level at 16 m, four times the outermost ring's 4 m; 16 cells a far tile), far level *k* is 16·2^(k−1) m apart in a square 7.2·2^(k−1) km across, and the ground reaches, from a camera at the worst place the windows let it stand (`terrain_far_reach_m`: three quarters of the last half-side less half its snap): 2.0–2.4 km with no far level (the rings' radius and its hysteresis), then 2.4, 4.9, 9.7, 19.5 and **38.9 km with five** (57 km to the last square's sides from its centre, 81 to its corners), 77.8 km with six. Five reach the haze from up to 350 m; 2,000 m wants six, and its last 28 km would be drawn at the 32 km table's transmittance. The default was five (`renderer.terrain.far_levels`) and is six since the captures below ("The hand-over at the far edge"): what decides the edge from high up is not the haze but whether the ground reaches the planet's horizon line.

| far level | spacing | tile | square | slots (two a tile) | field |
|---|---|---|---|---|---|
| 5 (finest, level 5) | 16 m | 256 m | 7.2 km | 1,568 | 451² |
| 4 | 32 m | 512 m | 14.3 km | 1,176 | 451² |
| 3 | 64 m | 1,024 m | 28.7 km | 1,176 | 451² |
| 2 | 128 m | 2,048 m | 57.3 km | 1,176 | 451² |
| 1 (coarsest) | 256 m | 4,096 m | 114.7 km | 1,176 | 451² |

6,272 slots against the rings' 41,000; a far slot holds at least sixteen clusters (a 16-cell tile's 512 triangles unsimplified), so about 100,000 pairs.

## Heights filtered to the spacing

`domain/terrain`'s `terrain_tests.cpp`, on the erg's five bands three years and 17 days in, ten points a side at each spacing against the field box-filtered over each point's cell (sampled every 2 m), and 24 points a side from the time the waves have moved a metre:

| spacing | bands kept | RMS against the box, filtered / points | largest | moved, waves a metre on: filtered / points |
|---|---|---|---|---|
| 16 m | mega, draa, crest, barchan | 0.23 / 0.23 m | 1.03 m | 1.3 / 8.4 cm |
| 32 m | the same | 0.45 / 0.45 m | 1.76 m | 4.7 / 23.5 cm |
| 64 m | mega, draa, barchan | 0.84 / 0.82 m | 3.25 m | 1.7 / 27.3 cm |
| 128 m | mega, draa | 1.61 / 1.68 m | 7.57 m | 1.5 / 28.5 cm |

- **Against the box the filter is no closer than point samples, and was not meant to be**: what a point sample gets wrong against the box is mostly the bands it keeps — a crest's brink is a kink the box rounds off — and the largest error is the same pixel either way. What it changes is what moves: a far lattice's points move by the slow bands they keep (the dropped bands' means do not move at all), a fifth to a twentieth of what the ground at those points does as the waves cross them. That is the shimmer, and it is gone.
- **Four points a cell was tried first** and dropped: it dropped bands a box of the spacing keeps four fifths of (the draa at 128 m, the barchans at 64 m), and the filtered lattice was then *further* from the box than its points (2.09 against 1.68 m at 128 m). Two points a cell — the sampling theorem's — is the rule (`DuneField::carries`).
- **What a far level costs the cadence**: its fastest kept band. At 256 m only the mega-draa, a metre and a half a year: the next field would be decades away and the thirty-day ceiling times it. In the time-lapse test (a week a second, the small world's far levels at 4, 8 and 16 m over dunes 20 m apart) the coarsest far level took 2 fields in 90 frames where the finest ring took 143, every level at the one surface time and none past its own bound.

## What the tests found

- **engine-view's silent stop after the first marker was an access violation**, not an exit 1 (the code was 0xC0000005; the lock's wrapper reported it as 1, and a fault prints nothing). Under `cdb`: `TerrainMotion::frame` read past the end of the finest ring's two fields after b, whose count stood at 0xFFFFFFFF. The surface's frame (`terrain_surface_frame`) shared the surface among at most `k_max_surface_levels` levels, still eight when the far levels raised the level bound to sixteen; the endless desert draws nine (five far levels and four rings), so its finest ring was left out, its `installed` word was never written, the debug stack's 0xCCCCCCCC took a field it did not have, and the count wrapped. The bound is now the level bound itself, and every output of the frame is written whatever the count (`terrain_generator_tests.cpp`, "every level a set can have shares the surface": sixteen levels at their b with a field ready, each must take it — without the fix the last eight keep the stack's word and stand where they were). The small world of the GPU tests has six drawn levels, under the old eight, which is why nothing there found it. In a release build the ninth level's word is whatever the stack held, so the finest ring's sand could stand still or take fields it did not have.
- **A T-junction at the corner of two collapsed edges**, older than the far levels: a tile with coarser neighbours on two adjacent sides drew its corner cell as a triangle of no area plus one whose long edge had the cell's corner vertex on it. The far levels' tiling test found it at every far square's corners (9–11 such triangles a layout); a ring's tile at a corner of coarser rings had it too. Fixed by taking the cell's other diagonal ([renderer](../subsystems/renderer.md#ground-to-the-horizon)).
- **The rings' picture does not move**: 49,152 of 49,152 pixels covered, 0 differ in depth, 0 colour bytes differ, with and without the far levels, at the origin and 50 km out.
- **No hole** in any column of the far views (from 90 m up across the far borders, from 6 m out to the last level's edge, finest cut and a pixel's, origin and 50 km). The normal's largest jump across two levels' borders: 0.008–0.14 radians against 0.09–0.24 inside a tile. Across two far tiles of one level, 0.06–0.22 against the same, except one grazing view at the origin: 0.137 against 0.086 — **the census, not a seam**: its pairs out there are a 4 m cell apart, so a border pair samples the surface's own creases (this one at z = −64, x = 35). Held by the picture instead: the same far levels cut into six-cell tiles (24, 48, 96 m) draw the same depth, normal and colour as the four-cell ones on 31,536 and 4,850 pixels of far ground at the origin (from 90 m and grazing; 30,762 and 1,871 at 50 km), 40% of them on a border of the four-cell tiles, and in the six-cell layout, where z = −64 is inside a tile, the inside's largest jump is 0.157 and the borders' 0.077. The first comparison counted 1 and 6 pixels different, every one a pixel where a crest in front was drawn at another level in the other layout (their squares differ with their tiles), so both visible points are now required to be clear far ground of one level in both layouts.
- **A far tile was built from the ground at each point, not its level's filtered answer**, while its fields were filtered: its padding was the gap between the two (0.97, 0.57 and 0 m on the test's three far levels) and its DAG simplified heights it never draws. A set built ahead answered zeros past its finest tiles, so its far tiles were flat at rest with 3.1 m of padding, and the grazing view's rings drew 10,733 of 31,000 pixels differently from the procedural source's; with no far levels, the same three cameras and the same wider set matched exactly, which is what pointed at the far levels. Both sources match on every pixel since (125,963 compared), with every level's padding zero while the sand stands still. How the far tiles' rest meshes reached the rings' pixels was not traced once the cause was gone.


## The captures, 2 m to 2,000 m

Offscreen, 1920×1080, `msvc-release`, the scene's own sky and shadows (traced on this card), a path of held views east from x = 1,000, z = 2,000 at 2, 100, 350 and 2,000 m over the sand looking at the ground 5, 8, 15 and 40 km ahead, each view twice a metre apart along x (`--marker-captures`, `--time-of-day 10` and `18`). Read by eye, and the edge and the pairs by numbers:

- **2 m.** The near dune and its ripples, the next dunes behind it, and to the right the ground going on in ridges of haze to the horizon, where it meets the sky with nothing to see between them. At 18:00 the near slope is lit low across its ripples and the far ridges are bands of lit crest and shadow into the haze.
- **100 m and 350 m.** Draa and mega-draa in rows to the horizon; the ground's last row stands above the planet's horizon line (flat ground from 100 m meets it 17.9 km out, from 350 m 33.4 km; the last level reaches further), so the ground meets the sky, and the step there is a horizon's (10:00, 350 m: the sky's row above the ground 161 against the ground's 167 of 255). No edge, no band.
- **2,000 m.** With five far levels a band of the planet's flat colour lay between the ground's last row (row 522, about 60 km out along the view) and the planet's horizon (row 514): at 10:00 the band was 162 and the ground under it 176, a ruled line across the picture; at 18:00 the band's flat yellow over the ground's shadowed peach. That is the hand-over below; with six far levels the ground runs to the horizon line and the band is gone.
- **No crack**: no column of any of the eight pictures has an uncovered pixel under covered ground (the ids, counted per column), the endless desert's own configuration — far tiles of sixteen cells at four times the outermost ring's spacing — which the tests do not draw.
- **No shimmer between two frames a metre apart.** Over the rows of far ground (rows 528–640 at 100 m, 511–640 at 350 m, 520–700 at 2,000 m) the mean luma difference between the two pictures is 0.02–0.10 of 255 and 0.01–0.13% of pixels differ by more than 8; at 2 m, whose rows hold the near dune's crest, 0.35 and 1.33 with 0.8% and 3%, which is the crest's parallax. The filtered far lattices do not move under a metre's move, as the CPU test said.
- **The dark specks** of one to three pixels on distant ridges (the first look saw them at 2 m; at 350 m they run in dotted rows along tile borders, every few pixels) **are traced shadows**, not holes: a speck's pixel is covered by the same triangle as its neighbours (the ids), with the same normal (the normal channel), and is lit by the sky alone; with `--shadows csm` they are gone. A shadow ray leaves a point reconstructed on a far triangle's plane, lifted by `ray_offset`'s bias and a few steps of the receiver's 16-bit grid — millimetres — while a pixel out there is metres across: at a crease (a far tile's border fan, a crest) the neighbouring triangle stands above the plane the ray left from and shadows it. The offset belongs to the resolve (`material.slang`, another agent's this week); one that grows with the pixel's footprint would remove them. Not changed here. **Since corrected** ([the shadow shards, 2026-10-10](shadow-shards-2026-10-09.md#2026-10-10-the-census-again-f17-and-the-surround-at-the-cameras-field-of-view-f18)): the surround's many specks were its views' different cuts in one structure (fixed 2026-10-09), and the census of the few left in one view found no ray leaving from an edge — all but one meet a triangle folded over their receiver in the terrain's coarse clusters, half a metre up at most, which no offset that keeps real shadows clears. The fix is in the geometry, and **was made there on 2026-10-11** ([the shards page](shadow-shards-2026-10-09.md#2026-10-11-where-the-folds-are-made-f17), [geometry](../subsystems/geometry.md#heightfield-levels-never-fold)): the LOD builder's regular simplifier made the folds, and it now refuses a heightfield simplification that turns a triangle over; the census's blocked pixels went from 3 to 1 at 5760×1080 and from 10 to 0 at 11520×2160.
- **A low sun on the far dunes.** With traced shadows (this card's default) every far dune casts its own shadow, however far: at 18:00 from 2,000 m the mega-draa throw long shadows across the interdune floors to the horizon, their edges stepped by the far levels' 128–512 m triangles. With `--shadows csm` (a card with no ray queries) the cascades end within a few kilometres and past them the far dunes are lit by their own slopes alone — sunlit stoss faces and sky-lit lee faces, no cast shadow across the floors — so from 350 m and 2,000 m the evening desert loses its long shadows a few kilometres out. The horizon-angle term (renderer.md, "Ground to the horizon") is still the cheap option and is still not built.

- **From the owner's start** (third pass: `fly-endless.ps1`'s `--start -2000,350,0 90,-10`, held as a two-key camera path since `--start` is a live session's, offscreen at 1920×1080, `--time-of-day 10 --shadows csm`, 4 frames, six far levels by default): dunes in rows from under the camera to the horizon, the near ones with their slip faces in soft shade and the far rows fading into a pale band of haze, and the sky's gradient from blue overhead to grey at the horizon. **No edge**: the sky's rows come down smoothly to row 358 (146, 145, 145 averaged over every column) and the sand begins at row 359 (160, 158, 156), the horizon's step and no band of the planet's flat colour between them; the planet's horizon line (a dip of 0.6° from 350 m) would be near row 368, below the ground's last row, as the arithmetic above says for six levels from 350 m.
## The hand-over at the far edge

The two cheap options, tried from 2,000 m at both hours (rows of the picture averaged over every column, 0–255):

| | the sky's band above the ground | the ground's first rows | what shows |
|---|---|---|---|
| five far levels, the planet at the scene's albedo 0.38 (as built) | 10:00: 162 / 18:00: 205,185,163 | 176 / 209,183,167 | a ruled line at 10:00, the band's flat yellow over shadowed ground at 18:00 |
| the planet at the sand's albedo, 0.71 (`sky.ground_albedo` in the scene) | 191,183,176 / 208,185,163 | 187,185,184 / 211,184,167 | the luminance step gone at 10:00 but the band warm over a neutral ground: still a line; the whole sky lifted by the brighter ground's bounce (row 500: 164 → 190) |
| the far levels ending where the aerial table does (32 km) | not drawn | | from 2,000 m a band of flat planet 2° tall instead of 0.4°; from 350 m the ground's edge would drop under the horizon line (33.4 km) and show a band where none shows now |
| **six far levels** (chosen) | 10:00: 164 / 18:00: 199,180,169 (sky) | 183 / 201,180,167 | **no band**: the ground's last row is above the planet's horizon line and meets the sky as a horizon |

Neither cheap option removes the edge, because the edge is not the albedo: past the aerial table's 32 km the ground keeps the 32 km transmittance (29% from 2,000 m) while the planet beyond it is drawn at its own distance (10% or less), so the ground is clearer than the planet next to it whatever its albedo. What removes it is the ground reaching the planet's horizon line, so that no planet shows below the horizon at all: flat ground meets it at √(Rh/2) — 79.8 km from 2,000 m — and six far levels reach 77.8 km from the worst place the windows let the camera stand (115 km along the last square's axes), which covers every height up to 1.9 km and leaves at most a pixel's sliver at 2,000 m in the worst direction. **`renderer.terrain.far_levels` is six** since. What remains is the horizon itself: the ground at 60–115 km is drawn at the table's 32 km haze, so it meets the sky a little brighter than real air would leave it (183 against the sky's 164 at 10:00). Removing that is the sky's: an aerial table that reaches the last level's distance, or past its end the sky's own ground-distance transmittance (`sky.slang`, untouched here).

## The cost

The scene's own 12 km path at 100 m/s, offscreen, 7,201 frames, `--shadows csm` (the baseline's shadows; the traced ones cost the same with far levels or without, as the chain is sized by the frame), median / p99 GPU milliseconds by pass:

| | 1080p, none | 1080p, five | 1080p, six | 11520×2160 surround3, none | surround3, six |
|---|---|---|---|---|---|
| cull | 0.229 / 0.484 | 0.287 / 0.368 | 0.306 / 0.377 | 0.138 / 0.145 | 0.152 / 0.164 |
| raster (hw) | 0.040 / 0.094 | 0.048 / 0.060 | 0.051 / 0.070 | 0.342 / 0.405 | 0.339 / 0.486 |
| deform (the terrain stage) | 0.074 / 0.190 | 0.094 / 0.139 | 0.099 / 0.140 | 0.053 / 0.058 | 0.053 / 0.065 |
| shadow maps | 0.917 / 2.522 | 0.823 / 2.385 | 0.831 / 2.390 | 0.407 / 0.597 | 0.473 / 0.719 |
| resolve | 0.442 / 0.988 | 0.485 / 0.662 | 0.488 / 0.656 | 1.491 / 1.737 | 1.578 / 1.917 |
| **total** | **2.544 / 5.542** | **2.937 / 5.046** | **2.980 / 5.103** | **3.544 / 4.286** | **4.015 / 4.930** |
| pairs drawn (visible), median / p99 | 3,491 / 3,703 | 4,727 / 5,011 | 5,061 / 5,343 | 5,678 / 6,173 | 8,861 / 9,805 |
| shadow-map pairs, median / p99 | 21,059 / 67,372 | 22,984 / 75,777 | 22,984 / 75,777 | 35,448 / 58,445 | 38,363 / 65,789 |
| terrain chunks resident (most) | 14,475 (14,729) | 17,414 (17,663) | 18,002 (18,251) | 14,475 | 18,002 |
| terrain slots and arenas on the device | 339.6 MiB | 477.6 MiB | 503.4 MiB | 339.6 MiB | 503.4 MiB |
| tiles built over the flight | 235,731 | 597,044 | 597,044 | 235,731 | 597,044 |
| a rebuild's worker wall, ms median / p99 / max | 11.5 / 55.6 / 560 | 12.9 / 42.3 / 62.3 | 12.8 / 49.8 / 77.8 | 12.6 / 55.9 / 85.8 | 13.0 / 51.4 / 71.3 |
| the frame thread's terrain time offscreen (waits included), median / p99 | 12.2 / 64.9 | 13.8 / 55.8 | 13.9 / 60.1 | 13.4 / 59.4 | 14.1 / 59.2 |

- **The far levels cost about 0.4 ms of GPU at 1080p and 0.5 ms at the surround size** at the median, mostly the cull (it visits every far slot's pairs every frame), the resolve's and the shadow maps' extra ground, and 138–164 MiB of device memory (two slots for every tile each square can hold, reserved at load; the sixth level adds 26 MiB). The p99 does not grow: without far levels the p99 frames are the ones whose view runs out of ground past the outermost ring, which the cull and resolve then pay for in other ways (0.48 against 0.37 ms of cull).
- **The rebuilds**: the far levels add little to a rebuild's median (11.5 → 12.8 ms) and nothing to its tail, but they build two and a half times the tiles over the flight. That was read as 361,000 far tiles, 50 a rebuild, rebuilt for their holes; **it is the rings** (corrected below, "A far square's move"): of the 597,044 tiles six far levels build over the flight, 87,765 are far tiles (12 a rebuild at the median) and 509,279 are the rings' (71 a rebuild, against 32 with no far levels). The frame thread's time offscreen is the wait for that rebuild; in a window it is the staging and the swap (renderer.md, "What a frame waits for").
- **Allocations in steady state: none, since a correction.** `terrain_tiles_gpu_tests.cpp`'s long flight (520 m, 260 frames) carries three far levels for this: over its second half, with seven moves of a far square, the frame thread first made 5 allocations — 4 on the frames a far square moved and 1 on another — and the first reading of them (the upload lists reaching a new high) was wrong. A stack taken at each counted allocation (a hook in `mem::try_allocate`, for the one run) put all five in `GpuScene::give_range`: an arena's free list growing as a swap let tiles go — a far square's move lets a strip of them go at once and fragments the arena, which took the list to new lengths; the fifth was the same list on an ordinary swap. The upload list was already sized at `start` for every slot. The free lists are now reserved for the most they can hold (free ranges are kept apart and merged, so there is a slot's range between any two: at most the slots and two), and the flight counts 0 with the far levels, as the rings alone did. A list grown by every chunk staged with the ray tracing chain on went too (`stage_chunk` packed the cluster indices into a fresh list; they go straight into the staging buffer now), though the flight, with shadows off, never counted it.

## A far square's move

The third pass, 2026-10-03. Measured on the same path offscreen (`msvc-release`, `--shadows csm`, six far levels, `--log debug,*=warn,renderer=debug`): each rebuild's phases on the worker (`TerrainTileSet::Phases`, now with `far_built` and `far_moved`, in its debug line) paired with its swap's line (`terrain rings swapped`, now with the frame thread's `stage_ms`, the swap's `swap_ms` and the pair carry-over's `pairs_samples`). **Machine state**: the owner's session locked throughout, other processes at 2–6% of the CPU and the GPU idle at the start and end samples, `quiet: true`; the no-far-level flight below ran beside a Linux container build (28.6% of the CPU at its start, `quiet: false`) and is quoted for its counts, which no load changes. Medians with the 99th percentile after the slash; a far square moved in 19 of the 7,198 rebuilds (the finest level 11 times, then 5, 2 and 1 for the next three).

| | ordinary rebuild, before / after | a far square's move, before / after |
|---|---|---|
| tiles built (of them far tiles) | 81 (12) | 310 (211) |
| the worker's scan (keys, what to build), ms | 0.20 / 0.18 | **2.44 / 1.20** (the finest level's: 2.59 / 1.23) |
| the worker's build, wall ms (heights and mesh CPU ms) | 12.6 / 12.5 (45 + 177) | 35.5 / 31.5 (191 + 894) |
| the rebuild, worker wall ms | 13.0 / 12.9 | 36.8 / 33.1 |
| the pairs carried over to moved windows, ms (samples evaluated) | 0.12 / 0.12 (none) | **38.6 / 17.4** (226,336) |
| the frame thread: staging the tiles, MiB and ms; the swap, ms | 0.87, 0.61; 0.09 | 4.30, 2.4; 0.18 |

- **What a far square's move costs**: on the workers, the tiles that changed — the moved level's new strip, the row whose border flag it cleared, and the coarser level's tiles the strip uncovered, 211 far tiles of 16 × 16 cells and their DAGs, 20 ms of wall over the ordinary rebuild — and the pair carry-over; on the frame thread 2.4 ms of staging (4.3 MiB, over one or two frames in a window at `renderer.terrain.tile_upload_mib`) and 0.18 ms of swap. Offscreen, where a frame waits for both, that frame took about 50 ms against 13.
- **Was it the pairs step over the moved strip** (the first agent's guess)? Partly, and not the way it looked. The pair evaluates only the moved strip: a far level's is 28,864 samples (64 of its lattice's columns over 451 rows; the coarser levels' moves, which happened alone, show it exactly). The rest of a finest-level move's 226,336 samples is a ring's window moving in the same rebuild (197,472 samples; a ring window's move costs 15–111 ms of pairs on its own, 137 times over the flight). What made the strip dear was the split: 8 blocks of 64 × 64 at four a job is two jobs, 13 ms for 28,864 samples. Windows are now split a block a job when they have fewer than four blocks a worker (`blocks_per_job`): the far strip alone 13.4 → 4.1 ms, a ring window's 197,472 samples 23 → 13 ms, a far move's pairs 38.6 → 17.4 ms at the median and 100 → 32 at the 99th percentile. The heights are a function of the lattice point, so no byte changes.
- **The scan**: a move of the finest far square asked every held tile of every ring again (`inside_far` might have changed for any of them) — 13,000 tiles hashed and decided — and a ring window's move every held tile of its level. Now only a held tile whose window coverage or `inside_far` the move changes is asked (one pass of arithmetic over the held tiles), which is none on this path: 2.6 → 1.2 ms. What is left of it is the moved level laid out whole (`far_tiles` over the square, with the finest level's hole masks), a millisecond.
- **The tiles built are what changed, but there are more of them than the far tiles**: with far levels the rings build 71 tiles a rebuild against 32 without. In the small world of the CPU tests, flown 90 steps of a third of a tile, the outermost ring adds 2,160 tiles with three far levels against 540 with none, the two inner rings the same with or without. The likely reason, not traced: an outer ring's tile collapses its edges *and corners* onto the far level beside it, so every world tile entering or leaving the rings changes the keys of the outer tiles round it, diagonally too — where with nothing beyond the rings a corner took the coarsest *drawn* neighbour and did not change. That is the seam rule working, and it is what a kilometre-sized rebuild in a slow window pays for most: every outer-ring tile the moving boundary passes, rebuilt for its edges.

## In a window

One 1920×1080 windowed flight of the path at 100 m/s, as [the window's page](world-tiles-window-2026-10-03.md) flew it, could not be measured: **the owner's session was locked**, and a locked session presents at about 4 Hz (`present_ms` 245 ms at the median, 471 frames in 120 s, with far levels and without alike). At 25 m a frame the rebuilds took what changed in seconds of travel: with six far levels 12 rebuilds, 3.4 s of worker wall at the median and 6.6 s at most, the finest ring 1.6 km behind the camera at the median; with none 19 rebuilds, 0.9 and 3.0 s, 1.0 km behind. So **a rebuild that covers kilometres costs about four times as much with the far levels** — every far tile whose hole the rings' band swept over, and every far tile a moved square brings in, is built, each 16 × 16 cells and its DAG where an outer ring's tile is 8 × 8 — and a window whose frames come that slowly falls further behind. At 52 frames a second each rebuild covers one or two metres of travel, as offscreen, and costs what the table above says; that is still to be measured with the session unlocked, with the same command (`engine-view --windowed --camera-path ... --benchmark`). The third pass found the session locked again (every run's `machine_state.session_locked`), so it has the offscreen numbers above and no windowed ones; with the session locked the lag is the present rate's, not the rebuilds'.

## What is open

- The windowed lag with the session unlocked (above): the finest ring's and the far levels' — one layout, swapped whole, so the same lag — at 100 m/s.
- The outer ring's tiles rebuilt for their edges and corners beside the far level (above, "A far square's move"): the rings build twice the tiles with far levels. An outer tile whose edges were on the far lattice whatever its neighbours would be built once, but the ring inside it would then have to meet the far lattice too: a change of the seam rule, not attempted.
- The finest far level laid out whole on its square's move (a millisecond of the scan).
- Far tiles rebuilt for their holes: 12 a rebuild at the median, not the 50 first read (above, "The cost"); a far tile whose mesh did not depend on its hole would be built once per square's move.
- The traced shadows' specks on distant ridges: an offset that grows with the pixel's footprint, in the resolve. (Not so, 2026-10-10: they are rays under fold-overs in the terrain's coarse clusters, [the shards page](shadow-shards-2026-10-09.md#2026-10-10-the-census-again-f17-and-the-surround-at-the-cameras-field-of-view-f18); the folds are refused where the simplifier made them since 2026-10-11, [the shards page](shadow-shards-2026-10-09.md#2026-10-11-where-the-folds-are-made-f17).)
- The far ground's haze past 32 km: the sky's aerial table.
- Far dunes past the cascades at a low sun: the horizon-angle term.

## 2026-10-26 F20 qualification: captures, capacity and shared-machine costs

**Frozen-data verification.** The measured source and frozen `region.f32` both have SHA-256
`b4cd3ecf8e6d7a0cbea86fd41ad2fd6463f4487cd060c392bf6dae8a3f2ef75b`. The descriptor differs
only in its rewritten data path. Scratch `verify-cook.ps1` compared every finest archive record
against the frozen float32 data quantized to millimetres: **7,345 records, 32,971,705 samples,
zero mismatches**, including aprons and clamped edges. The copied cook has 64 cells per tile,
also correcting the initial brief's 32. The measured ranges therefore describe the frozen cook.

**Final chains.** The default remains six levels, 16,32,64,128,256,512 m. With the measured
percentile proxy `terrain_far_error_per_m=0.75`, error 1 and 2 pixels are refused at both
resolutions, on both requested cameras; their `report.json` records exit 1 and the sentence
"no legal far spacing meets terrain_far_error_px with the supplied measured bounds". There
is no capture from a refused layout. To measure feasible finer layouts as well, error **8**
selects seven levels, **4,8,16,32,64,128,256 m**, and error **16** selects seven levels,
**8,16,32,64,128,256,512 m**, at both resolutions. These are relaxed statistical proxies, not
one/two-pixel guarantees. Starts, finest first, in metres:

| mode | starts | worst-camera reach |
|---|---|---:|
| default | 1040,2048,4096,8192,16384,32768 | 65536 m |
| 8 px | 1040,1664,3328,6656,13312,26624,53248 | 106496 m |
| 16 px | 1040,1600,3200,6400,12800,25600,51200 | 102400 m |

An initial prototype also preserved the coarsest spacing; that unnecessarily reserved an eighth
level for 8 px, about 167 MiB of extra arenas plus fields and slot metadata. The final rule keeps
reach alone. Raw `*-px8` runs are that superseded prototype and are excluded below; **use
`*-px8-final`** for the final seven-level layout. `*-px16` is unchanged by that correction.

**Reproduction.** All renders used this worktree's release engine-view, offscreen, hardware
raster, cascaded shadows, lod 1, terrain time rate 0, world rate 1, hour 15, no point lights,
64 MiB pool, 1024 MiB RT budget, flora wind strength 0.7, 120 measured frames after 30 warmup,
`--wait-quiet 10`. The exact argument arrays, startup trace, frames, report and capture are in
each scratch run directory. The scene's embedded terrain DDC and the CLI DDC both point to the
scratch copy; the finer chain was cooked there from identical source data. Nothing wrote to the
POC. The final measured executable SHA-256 is
`452f6e529936665fe3df2f358d9a8953974cb78264e0444c120cfdccc2e36221` (built while the F20 code
was uncommitted; its stamp records `ec17b831` dirty, the C++ subsequently committed as `7039c2ff`).

**Machine state: all timings are upper bounds, not isolated costs.** RTX 5090 (32607 MiB),
Core i9-10980XE, NVIDIA 617.42. The session was unlocked and the GPU lock was ours in every
sample, but other work remained active and every completed run reports `quiet:false`.
The per-run start/end samples are below and preserved in `report.json`; several wide frames
were heavily disturbed. Do not infer speedups or slowdown ratios from their total GPU times.
There is no far-only hardware-pass timestamp: the HW column is the whole primary hardware
pass; delta is its difference from the matching no-far control, including changed border
geometry and run noise, **not a separately measured far draw**.

Capacity columns sum only the far levels: DAG vertex arena capacity and reserved cluster-pair
space (two slots per tile), not source-grid vertices or visible pairs. Arena bytes use the actual
CSM reservation formula: `100 * pairs + 14 * vertices + 4 * packed_triangles`. This excludes
field pairs, per-slot/per-instance tables and common scene buffers. Device MiB is the engine's
whole-scene device allocation report. All successful runs counted zero frame-loop allocations
and zero buffer creations.

| run (under scratch/runs) | far | vertex capacity | pair capacity | far arenas MiB | HW / delta ms | total GPU ms | whole-device MiB |
|---|---:|---:|---:|---:|---:|---:|---:|
| beach-11520x2160-baseline-final | 6 | 5789760 | 114048 | 118.26 | 3.441 / 0.028 | 50.955 | 5810 |
| beach-11520x2160-no-far-final | 0 | 0 | 0 | 0.00 | 3.413 / 0.000 | 129.817 | 5692 |
| beach-11520x2160-px16-final | 7 | 15106864 | 303264 | 308.96 | 3.428 / 0.015 | 51.298 | 6000 |
| beach-11520x2160-px8-final | 7 | 60621088 | 1213056 | 1238.45 | 3.480 / 0.067 | 109.974 | 6926 |
| beach-2560x1440-baseline-final | 6 | 5789760 | 114048 | 118.26 | 0.473 / 0.001 | 16.017 | 4804 |
| beach-2560x1440-no-far-final | 0 | 0 | 0 | 0.00 | 0.472 / 0.000 | 38.916 | 4665 |
| beach-2560x1440-px16-final | 7 | 15106864 | 303264 | 308.96 | 0.474 / 0.002 | 16.167 | 5028 |
| beach-2560x1440-px8-final | 7 | 60621088 | 1213056 | 1238.45 | 0.483 / 0.011 | 17.078 | 6377 |
| ridge-11520x2160-baseline | 6 | 5770176 | 109440 | 117.57 | 0.561 / 0.003 | 25.081 | 5743 |
| ridge-11520x2160-no-far | 0 | 0 | 0 | 0.00 | 0.559 / 0.000 | 24.675 | 5606 |
| ridge-11520x2160-px16 | 7 | 15071224 | 295488 | 307.77 | 0.560 / 0.001 | 36.546 | 5967 |
| ridge-11520x2160-px8-final | 7 | 60496672 | 1213056 | 1236.79 | 0.561 / 0.003 | 26.314 | 6925 |
| ridge-2560x1440-baseline | 6 | 5770176 | 109440 | 117.57 | 0.111 / 0.001 | 24.503 | 3974 |
| ridge-2560x1440-no-far | 0 | 0 | 0 | 0.00 | 0.110 / 0.000 | 27.796 | 3839 |
| ridge-2560x1440-px16 | 7 | 15071224 | 295488 | 307.77 | 0.113 / 0.003 | 27.460 | 4195 |
| ridge-2560x1440-px8-final | 7 | 60496672 | 1213056 | 1236.79 | 0.116 / 0.005 | 8.122 | 5639 |
| tag5-11520x2160-baseline | 6 | 5772480 | 114048 | 118.04 | 0.497 / 0.037 | 53.221 | 5810 |
| tag5-11520x2160-no-far | 0 | 0 | 0 | 0.00 | 0.460 / 0.000 | 78.555 | 5693 |
| tag5-11520x2160-px16 | 7 | 15024568 | 295488 | 307.15 | 0.496 / 0.036 | 82.521 | 5999 |
| tag5-11520x2160-px8-final | 7 | 60154528 | 1213056 | 1232.22 | 0.498 / 0.038 | 37.182 | 6920 |
| tag5-2560x1440-baseline | 6 | 5772480 | 114048 | 118.04 | 0.083 / 0.002 | 6.654 | 3843 |
| tag5-2560x1440-no-far | 0 | 0 | 0 | 0.00 | 0.082 / 0.000 | 6.458 | 3707 |
| tag5-2560x1440-px16 | 7 | 15024568 | 295488 | 307.15 | 0.085 / 0.003 | 6.734 | 4062 |
| tag5-2560x1440-px8-final | 7 | 60154528 | 1213056 | 1232.22 | 0.089 / 0.007 | 6.884 | 5489 |

| run | other CPU %, start/end | GPU %, start/end | global GPU MiB, start/end |
|---|---:|---:|---:|
| beach-11520x2160-baseline-final | 9.4 / 10.6 | 1 / 30 | 17706 / 19749 |
| beach-11520x2160-no-far-final | 32.7 / 25.5 | 100 / 99 | 19067 / 21250 |
| beach-11520x2160-px16-final | 13.5 / 16.5 | 1 / 1 | 17917 / 19959 |
| beach-11520x2160-px8-final | 25.2 / 15.6 | 100 / 100 | 17265 / 23972 |
| beach-2560x1440-baseline-final | 15.1 / 21.1 | 2 / 100 | 17033 / 18748 |
| beach-2560x1440-no-far-final | 13.2 / 16.5 | 100 / 100 | 18378 / 20073 |
| beach-2560x1440-px16-final | 8.9 / 9.7 | 2 / 48 | 17223 / 18973 |
| beach-2560x1440-px8-final | 30.0 / 41.2 | 1 / 100 | 15217 / 19515 |
| ridge-11520x2160-baseline | 35.1 / 27.8 | 1 / 1 | 20009 / 21990 |
| ridge-11520x2160-no-far | 18.5 / 22.6 | 2 / 100 | 19907 / 21864 |
| ridge-11520x2160-px16 | 17.5 / 25.0 | 100 / 100 | 22440 / 16361 |
| ridge-11520x2160-px8-final | 34.4 / 40.5 | 2 / 1 | 15600 / 17564 |
| ridge-2560x1440-baseline | 12.3 / 13.7 | 100 / 100 | 23950 / 24833 |
| ridge-2560x1440-no-far | 24.4 / 27.8 | 100 / 100 | 21448 / 22322 |
| ridge-2560x1440-px16 | 26.9 / 25.4 | 100 / 100 | 21755 / 22672 |
| ridge-2560x1440-px8-final | 24.8 / 26.3 | 2 / 1 | 13164 / 14225 |
| tag5-11520x2160-baseline | 19.0 / 97.4 | 1 / 92 | 18826 / 22922 |
| tag5-11520x2160-no-far | 23.6 / 18.8 | 100 / 100 | 24508 / 26561 |
| tag5-11520x2160-px16 | 16.4 / 26.1 | 100 / 100 | 24814 / 26862 |
| tag5-11520x2160-px8-final | 7.6 / 13.4 | 1 / 100 | 20393 / 23665 |
| tag5-2560x1440-baseline | 41.8 / 25.8 | 2 / 1 | 14402 / 16625 |
| tag5-2560x1440-no-far | 9.2 / 16.3 | 1 / 100 | 18036 / 18766 |
| tag5-2560x1440-px16 | 9.5 / 16.5 | 1 / 99 | 18337 / 19121 |
| tag5-2560x1440-px8-final | 27.3 / 24.8 | 4 / 100 | 14997 / 15912 |

Per-level reservations, representative tag-5 layout (identical at both pixel sizes):

| mode | spacing m | held tiles | DAG vertices capacity | pairs capacity | arenas MiB | two fields MiB |
|---|---:|---:|---:|---:|---:|---:|
| baseline | 16 | 510 | 1220992 | 27648 | 25.28 | 1.14 |
| baseline | 32 | 432 | 911680 | 17280 | 18.57 | 1.14 |
| baseline | 64 | 432 | 906496 | 17280 | 18.50 | 1.14 |
| baseline | 128 | 432 | 909088 | 17280 | 18.54 | 1.14 |
| baseline | 256 | 432 | 916000 | 17280 | 18.63 | 1.14 |
| baseline | 512 | 432 | 908224 | 17280 | 18.52 | 1.14 |
| px16 | 8 | 1000 | 2745952 | 62208 | 56.87 | 2.56 |
| px16 | 16 | 972 | 2044168 | 38880 | 41.68 | 2.56 |
| px16 | 32 | 972 | 2050000 | 38880 | 41.76 | 2.56 |
| px16 | 64 | 972 | 2038336 | 38880 | 41.61 | 2.56 |
| px16 | 128 | 972 | 2044168 | 38880 | 41.68 | 2.56 |
| px16 | 256 | 972 | 2059720 | 38880 | 41.89 | 2.56 |
| px16 | 512 | 972 | 2042224 | 38880 | 41.66 | 2.56 |
| px8-final | 4 | 3926 | 11011840 | 248832 | 227.83 | 10.18 |
| px8-final | 8 | 3888 | 8212480 | 186624 | 170.17 | 10.18 |
| px8-final | 16 | 3888 | 8173600 | 155520 | 166.68 | 10.18 |
| px8-final | 32 | 3888 | 8196928 | 155520 | 166.99 | 10.18 |
| px8-final | 64 | 3888 | 8150272 | 155520 | 166.37 | 10.18 |
| px8-final | 128 | 3888 | 8173600 | 155520 | 166.68 | 10.18 |
| px8-final | 256 | 3888 | 8235808 | 155520 | 167.51 | 10.18 |

**What the captures show.** Tag 5 primarily sees a near slope and rocks; the ridge camera sees
near trees and the smooth authored ridge. Their broad shape remains similar with finer far
levels. A supplemental recorded **beach/tag-1** camera sees the opposite shore and mountain:
the broad rounded profile and distant colour patches remain. Finer samples preserve detail the
heightfield contains; they do not author ribs/gullies. No claim that these captures repair the
mountain's authored shape, or R96/R97's colour/material problem, is supported by this experiment.

Baseline-to-final-8-px full-image RGB mean absolute differences (0..255), 1440p / 2160p:
tag 5 **0.0355 / 0.0627**, ridge **0.0201 / 0.00063**, beach **0.1624 / 0.2615**. This is an
image comparison including changed shadow fits/casters and foliage, not a geometry-error
measurement. The controlled no-far-visible byte equality and exact seams remain the existing
GPU tests' responsibility. Scratch `pixel-differences.json` carries the pixel counts and tails.

**Capture paths**, rooted at `D:/workspace/ge-codex-scratch/sol-far/runs/`:

- `tag5-{2560x1440,11520x2160}-{baseline,px16,px8-final}/view.png`.
- `ridge-{2560x1440,11520x2160}-{baseline,px16,px8-final}/view.png`.
- `beach-{2560x1440,11520x2160}-{baseline-final,px16-final,px8-final}/view.png`.
- Matching `no-far` (beach: `no-far-final`) directories are the marginal-HW controls.

The report artifacts are `costs.json`, `levels.json`, `mountain-errors.json`,
`reconstruction-errors.json`, `verify-cook.log`, and the exact `arguments.json` in each run.
All are scratch artifacts, never committed. The initial 8-level prototype is retained for audit.

**Tests.** Three new CPU cases: 154,689 assertions; arithmetic at both actual resolutions,
starts/spacings, override precedence, missing/error tables, failure immutability, the forest's
one/two-pixel refusal, reach preservation without an extra coarsest level, and the sixteen-level
refusal. The unchanged moving-square CPU tiling proof passes 1,338,928 assertions. The Clang
container passed all 13 selected CTest tests in 171.73 s after two double-promotion errors found
on its first build were corrected. Its two worktree volumes were pruned (3.65 GB). Windows
affected qualification is recorded in the scratch REPORT after the final documentation commit.

Range histogram (box counts; upper bin edges in metres):

| spacing | <=1 | (1,2] | (2,4] | (4,8] | (8,16] | (16,32] | (32,64] | (64,128] | >128 |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 16 | 2 | 15 | 94 | 788 | 3610 | 1422 | 41 | 23 | 0 |
| 32 | 0 | 0 | 1 | 20 | 193 | 912 | 346 | 20 | 0 |
| 64 | 0 | 0 | 0 | 0 | 4 | 49 | 228 | 87 | 6 |

At the **actual baseline starts**, before cluster simplification or seam fans, the diagnostics project as follows (single-view 55-degree FOV; two pixels allow twice the metre thresholds):

| spacing m | start m | one pixel m, 1440p / 2160p | box-deviation p95 in pixels, 1440p / 2160p | reconstructed-cell p95 in pixels, 1440p / 2160p |
|---:|---:|---:|---:|---:|
| 16 | 1040 | 0.752 / 0.501 | 13.93 / 20.89 | 1.69 / 2.54 |
| 32 | 2048 | 1.481 / 0.987 | 14.30 / 21.46 | 3.36 / 5.04 |
| 64 | 4096 | 2.961 / 1.974 | 14.27 / 21.41 | 6.78 / 10.17 |
