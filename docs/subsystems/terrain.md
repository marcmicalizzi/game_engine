# terrain (domain, capability)

**Purpose.** The desert's procedural terrain generator ([ADR-0043](../adr/0043-dunes-as-a-function-of-time.md), proposed; [05 §5.13](../plan/05-simulation.md#513-deformable-surfaces-and-soft-bodies), [07 §7.6](../plan/07-content-pipeline.md#76-procedural-generation-volume-content), [13 §13.1](../plan/13-reference-consumer-games.md)). The endless desert's dunes are a **pure function of (world seed, tile, game time)**: parametric dune primitives placed from the seed in lattices that a seeded wind record carries downwind, evaluated at any time t in closed form, at the same cost for any t, with nothing about their evolution stored. Beside that function, and never read by it, is the only state the player adds: a per-tile **deformation overlay** of fixed size (footprints, digging, the sand a wall's drift is short of) that the wind fills back in, and a saturating per-tile **lag**, a few bytes, which is the one way large obstacles may feed back into where the dunes come to rest. A tile of the field at a time comes with the crest lines, the wind and the sand flux a renderer's blowing-sand pass needs, and a sampler that answers exactly what the tile's mesh has. It is CPU arithmetic, integer throughout, and the same bits on every toolchain; it draws nothing, owns no GPU work, and does not decide what a tile is to the world or the renderer.

**Why "terrain", and why `domain`.** What it owns is the ground's function of time and the ground's deformation; snow and mud will want the same overlay with their own rules and fields ([05 §5.13](../plan/05-simulation.md#513-deformable-surfaces-and-soft-bodies)), and the dunes are its first field, not its name. It is pure arithmetic over core modules that the renderer (L3), the world (L3) and the content tool (L4) all read, and must depend on none of them — the ruins capability is its nearest sibling and sits in `domain` for the same reason. It does not depend on the ruins capability either: a wall's drift arrives as this module's own `DriftDecl`, and the ruins' `Ground` query is matched by a function of the same signature (`DuneField::ground_height`).

## The time function

**The problem.** Wind shapes dunes over time: fields migrate downwind at a rate that falls with dune height (Bagnold: celerity is the sand flux over the dune's height), crests advance, slip faces steepen to the angle of repose and round off in a calm, and a reversing wind moves the slip face to the other side. The obvious way to get that is a simulation stepped day by day. Its state would have to be kept — per tile, forever, for every tile anyone ever saw — and a tile nobody visited for a year would have to be stepped a year to be seen. That is the storage the plan forbids, and a cost that grows with t.

**The representation.** Three **bands** of primitives, superposed ([dunes.h](../../domain/terrain/include/domain/terrain/dunes.h)):

| Band | Primitive | Lattice cell | Height (of `dune_height` H) | Share of cells | Moves as |
|---|---|---|---|---|---|
| `draa` | transverse crest: a segment 0.55–0.95 cells long, a gentle stoss 0.42 cells wide, a lee, a bend of up to 0.18 cells at its ends | 2 wavelengths | 0.75–1.25 H | all | H |
| `crest` | the same, at the scene's wavelength, riding on the draa | 1 wavelength | 0.3–0.6 H | 85% | 0.45 H |
| `barchan` | a crescent: a dome 4 H upwind and 5.5 H downwind and aside, and a scoop 3.5 H in radius whose rim is the brink; the dome's flanks either side of the scoop are the horns | 0.7 wavelength | 0.2–0.45 H | 40% | 0.325 H |

Each band's primitives are placed on a lattice, one or none per cell, every choice (presence, the centre jittered within the cell's middle half, height, length, the crest's angle within 20–25° of square to the prevailing wind, the bend) a draw from the engine's hash of the seed, the band, the cell and what the draw is for, in centimetres. **The lattice moves with the wind**: band b is displaced by

    D_b(t) = I(t) / H_b

where I(t) is the wind record's flux integral (below) and H_b the band's height — Bagnold's relation — and the height at a point p is the band's primitives evaluated at p − D_b(t). So barchans overtake the ridges and the draa hardly move: in the default wind, two years move the draa 125 m, the crests 279 m and the barchans 386 m, and the ratio of speeds is the inverse ratio of heights to a thousandth (the test asserts it).

**Why primitives belong to a lattice cell and not to a tile.** A primitive owned by the tile it was placed in migrates out of it; after a year its tile's neighbours are not the tiles it reaches, and the set a tile would have to consult grows with t. A lattice that moves with the wind makes the primitives that can reach any point at any time a fixed number of cells of each band, which is what makes the cost of t = 1000 years the cost of t = 0 (measured: 2.59 ms against 2.49, below).

**What t changes besides position.** A crest's slip-face **side** is the sign of the last 120 days' resultant flux across it, and its **sharpness** the last 30 days' flux across it against three quarters of a month of the mean: a sustained wind steepens the lee to a straight slip face at 34° (the angle of repose), a calm month rounds it (a smooth lee three times as wide, half as steep), and a reversal moves it to the other side. Both are blended continuously — the two sides' profiles weighted, the two lees mixed — so no crest flips between frames: across a year of a wind that reverses by season, the side's largest daily step is 12,030 of 65,536 (the test). A barchan points its horns down the last month's resultant, with a small bias toward the prevailing wind so its direction stays defined through a calm. Ripples (12 cm apart, 6 mm high at the mean wind, drifting a centimetre a minute) lie across the day's wind and realign to a new day's over its first two hours.

**Within a band the primitives meet by their maximum; the bands add.** A smooth maximum was the first draft and is wrong here: blending against a primitive's zero adds a bump wherever its reach begins, which made the surface jump there and depend on the order a gather held the primitives in. The maximum is continuous and order-free. The bands add because a crest riding on a draa is what a compound dune is — which has one cost, under "Not yet".

**Fixed features.** A scene's rock ridges and basins stay where the description put them. Over a ridge the sand thins to 40% of its height and in a basin it flattens (the renderer's heightfield rules, in integers), and near a ridge each band's lattice is **held back** upwind by up to `ridge_lag_q16` (0.3) of the ridge's width, falling off to nothing at twice the width — so every crest bows round the rock, at every t, as a function of the point. The interdune floor has a slow roll 11 wavelengths long, fixed in the world. The ridges' and basins' own heights stay the renderer's (`renderer::TerrainSampler` adds them); the generator knows only where they are.

## The band table

The bands are a **table** (`BandDesc`, `FieldDesc::bands`): per band its primitive kind, its lattice cell, its height range, the share of cells it occupies, a crest's length, stoss and bend as fractions of the cell and its spread from square to the wind — every seeded choice in centimetres and Q16, as before — and, new with the table, a crest's sinuosity, a cap on its lee's sharpness, the two wind windows its slip face remembers (120 and 30 days by default), how it couples to the bands before it (anywhere, only on their flanks, only on the floors between them), and whether the coarse detail keeps it. **The empty table is the default**: `default_bands(dune_height, wavelength)` is exactly the three bands above, built by the same centimetre arithmetic the constants used, so every field described before the table existed evaluates to the same bits — the golden hashes below did not move, and a test holds a table written out in full to the same tile as the empty one. The field's hash takes the table only when there is one. A band reads lag slot `min(b, 2)` of its tile's `TileLag` (three bytes, the record format saves already hold); every nudge moves all three slots alike, so which slot a band reads changes nothing today. `validate_bands` refuses what a field cannot be built from, with a sentence: an empty table (leave it out for the default), more than `k_max_bands` (8), a band with a zero share, heights not positive or out of order, a cell smaller than the band's own dune (a crest's stoss and rounded lee, a barchan's width), a band taller than the one before it (a band is shaped by the ones before it, so the table is tallest first), and a coupling on the first band.

**Coupling and sinuosity.** A band coupled to the **flanks** of the bands before it is multiplied by 1 − falloff(their sand / `couple_mm`): nothing on a floor, all of it once their sand has risen past the height — the owner's "medium dunes on their flanks". One coupled to the **floors** is multiplied by falloff(their sand / `couple_mm`): barchans on the hardpan, gone where the tall bands' sand passes a couple of metres. Both fades are smooth functions of a sum that is continuous, so no band ever steps, and a test holds a floor-coupled band to exactly zero on every point where the band before it stands higher than its height. A crest's **meander** moves its line `sinuosity` × cell either side, one wave a cell along it, from a seeded phase of its own, so a band without sinuosity draws nothing new. **Binning**: a gather now also bins each band's primitives by lattice cell, and a point visits only the cells whose primitives can reach it (a fixed number whatever the gather's size: the per-band evaluation stays bounded, and a band of ten-metre waves costs a point 25 cells, not the thousands a large gather holds); a band with at most 24 primitives in the gather is walked as a list, which is cheaper. The maximum is order-free, so either walk is the same bits.

## The erg profile

The owner's first flight ([third session](../experiments/third-interactive-session-2026-09-25.md)) found the default field — three bands scaled from one 8 m dune height — the small end of an erg's spectrum, and asked for Erg Chebbi's kind: towering dunes of a hundred metres and more at kilometre spacing, medium dunes on their flanks, waves and ripples on every surface, flat interdune floors between. [`content/test-scenes/desert-erg/`](../../content/test-scenes/desert-erg/README.md) is that table, and `desert-dunes` now carries it over the overlook's ridges and basin:

| Band | Kind | Height | Cell | Share | Couples | Moves (200 m² a year) |
|---|---|---|---|---|---|---|
| mega-draa | transverse, sinuosity 0.08, half length 0.6–1.0 cell, windows 365 / 120 days | 80–200 m | 2,400 m | 0.85 | — | 1.4 m a year |
| draa | transverse, sinuosity 0.04 | 10–25 m | 360 m | 0.7 | flanks, 8 m | 11 m |
| crest | transverse | 2.5–6 m | 110 m | 0.6 | flanks, 3 m | 47 m |
| barchan | barchan | 1.5–5 m | 150 m | 0.35 | floors, 2 m | 62 m |
| wave | transverse, sharpness 0, windows 3 / 1 days | 0.3–0.8 m | 10 m | 0.6 | — | 330 m |

**Why these numbers** (the plan does not settle them; each is a scene field and the owner decides by flying): real draa are 100–300 m high and 1–3 km apart, so 80–200 m on 2.4 km cells; crests longer than a cell overlap along their length into long lines, the meander makes them sinuous, and 15% empty cells leave corridors. A 150 m slip face does not reform in a week, so the mega-draa remember a year of wind for their side and four months for their sharpness. Medium dunes on the flanks only and barchans on the floors only are what leaves **open floors**: with no coupling every band covers every metre and nothing is flat. Waves are the megaripples between ripples and dunes — rounded (never a slip face), a few days' memory — and ride everything. No migration exponent was needed: Bagnold's rule alone leaves the mega-draa almost still while the waves cross a kilometre in three years, which is the "tall band nearly still while the small forms crawl over it" the owner described.

**What it costs**: a 32 m tile at 25 cm is 5.6 ms against the default's 2.4 (`terrain.tile.eval_erg`, below): five bands, a mega-draa reach of kilometres, and waves binned by cell.

## What the numbers say

The picture is the owner's to judge on his machine; `field_stats` (stats.h) measures what his eye would check, over any square of the field on a grid, and `engine-content terrain --stats` prints it ([apps](apps.md#engine-content-terrain-a-tile-of-the-dune-field)): the **height above the interdune floor** (10th, 50th, 90th, 99th percentile and the largest), the **slope histogram** of the sand (0–5, …, 30–34, 34–36 and over 36 degrees, by central differences), with the shares over the angle of repose and over 36°, the **flats** (the share within half a metre of the floor), **crest length per square kilometre** by band (counting only crest its coupling lets stand), and the **largest dune**'s height and its band's spacing (the mean distance from each of its dunes to the nearest). A slope's bin is decided by tan² against constants in doubles — exact for these magnitudes and IEEE everywhere, no `tan` — so every count is an integer the tests pin (`FieldStats::hash`), and a test holds the counts to a brute-force computation (point queries and `std::atan`) on a 48 × 48 window and to the same hash on three threads.

## The wind record

[wind.h](../../domain/terrain/include/domain/terrain/wind.h). **A table of days over a period of eight years, and its prefix sums.** Each day has a direction and a strength drawn from the seed with a seasonal structure: the direction swings about the prevailing one by ±30° over the year and strays ±15° a day; the strength peaks an eighth of a year after the swing does, varies ±35% a day, and about 16% of days are calm. A day's **sand flux** is the cube of its strength (Bagnold's cube law), normalized so the record's mean is `flux_cm2_per_day` — 5,479 cm² a day, 200 m² a year, a strong trade-wind desert. Then

    I(t) = cycles(t) × total + prefix[day of t in the period] + flux[that day] × (time into the day)

three lookups and a multiply, whatever t is (9.8 ns). **Why a period and not a telescoping or analytic record**: the integral has to be closed-form, and any daily sequence can be summed in closed form once it repeats; the alternatives (a noise whose sum telescopes, an analytic wind) force the weather to be something with a closed form of its own, which rules out storms and calms. Eight years is long enough that nobody notices a repeat of the daily weather and short enough that the table is 2,920 days × 44 bytes built in well under a millisecond. The day's flux is spread evenly over the day, so dunes move smoothly rather than once at midnight.

**The closed form is the simulation.** The test adds the days up one at a time for twice the period plus forty days, and back 400 days before the epoch, and the closed form equals the running sum at every day boundary — exactly, since both are integers. Evaluating t₂ directly equals evaluating t₁ and then t₂ (there is nothing an evaluation leaves behind), and going back to t₁ after t₂ gives t₁ again.

## Integer in every decision

[fixed.h](../../domain/terrain/include/domain/terrain/fixed.h). Positions are millimetres (`i64`), heights micrometres, fractions Q16, directions binary angles (65,536 to a turn) and unit vectors Q14. **The sine is a quarter-wave table built at compile time by a Taylor series in Q30 integer arithmetic** — a constant expression over integers has one value in C++, so no two toolchains can disagree about it (worst error 1 in Q15 against the true sine); square roots are an exact integer `isqrt`; divisions in the inner loop are multiplications by Q32 reciprocals a gather worked out once per primitive. Floats appear only in the output: `height_m` converts micrometres through a double, and a normal is three integer differences normalized with `std::sqrt`, which IEEE rounds correctly on every compiler. With floating-point contraction off tree-wide ([ADR-0035](../adr/0035-no-floating-point-contraction.md)) the tile's normals are the same bits everywhere too, so the golden hash covers them.

**The golden hashes** (`terrain_tests.cpp`): the reference field (seed 2026, 3 m dunes 90 m apart, one ridge and one basin near the tile) hashes to `e4ce66bd0e50f703`, and its tile (0, 0) at the 25 cm grid — every height, material and normal — to `cdb65fedad6a22ca` at t = 0, `fd311c603c266ab5` at 90¼ days and `c364843efab5ad9c` at 25 years and 200 days. Taken on Clang 18 (debug) and reproduced on GCC 13 (RelWithDebInfo) and on Clang at x86-64-v2 with `-O2`; MSVC is checked on the owner's machine. A mismatch on one toolchain is a toolchain dependence, which is the bug; re-pin only with a change that bumps `k_generator_version`, and say what moved.

## Tiles, seams and the sampler

[tile.h](../../domain/terrain/include/domain/terrain/tile.h). **A tile is where the field is sampled, not what it is made of.** `evaluate_tile` gathers every primitive that can reach the tile and a one-vertex apron round it — its neighbours' primitives and the upwind cells the wind has carried in included, since a gather covers the tile's rectangle moved back by each band's displacement and widened by the band's largest reach and the largest lag — and evaluates the grid: `(cells + 1)²` heights in µm, normals by central differences over the apron, and a material per vertex (sand, rock, basin sand, the basin's floor: the renderer's four, by the renderer's thresholds). So **a vertex two tiles share is one point evaluated against the same primitives**, with the same height to the micrometre, the same normal and the same material; the test compares shared east and north edges bit for bit for three tiles at three times. That holds because the combination is order-free (the maximum and the sum) and a primitive either reaches a point or contributes nothing, so any two gathers that cover a point give it the same height.

**The sampler contract** (`TileSampler`): a height and a normal at any point, which are the numbers the mesh of the tile under it has — the height is the field at the point, and the normal the same central difference over the mesh's spacing. It keeps the last tile's gather, so walking a tile costs one gather. `build_tile_mesh` writes world-metre positions, the normals and two counter-clockwise triangles a quad, the renderer's heightfield winding. The test samples every vertex of a mesh through the sampler and finds no difference.

**The ruins' ground** is `DuneField::ground_height`, which has the signature of `ruins::Ground::fn` without this module depending on the ruins capability, and answers the **interdune floor** (`Detail::floor`), which never moves. A ruin assembled on a tile at any time is therefore the same building, and the dunes migrate over it: a doorway buried by a moving dune, which [07 §7.6](../plan/07-content-pipeline.md#76-procedural-generation-volume-content) asked for, is the field passing over a building that stands on the floor.

## The overlay and its bound

[overlay.h](../../domain/terrain/include/domain/terrain/overlay.h). What the player changed, and nothing else. **Stamps** — a footprint (an ellipse to a depth, a flat floor to half its radius and a soft wall, a rim of the sand it pushed out), a dig, sand dumped — are applied to a grid of 128 × 128 `i16` millimetres a tile: at the world's 32 m tile a vertex every 25 cm, which is 05 §5.13's coarse deterministic CPU grid that gameplay reads (foot depth, movement penalty, tracking, audio). The value is the **deviation from the steady state** — the base dunes plus the drift every declared wall face asks for — so a ruin's drift is already there when its tile first comes in (the walls are older than the player) and costs nothing to store, and a wall the player builds declares its drift on the day it is built, which starts it empty (`declare_drift`).

**Decay toward the base.** Every vertex moves toward zero by `fill_um_per_cm2` × the sand flux that has blown since (the wind record's magnitude integral, closed form), at a rate that is full on loose sand and falls to nothing where a ridge is more than half the ground (rock takes no print and fills with nothing), and snaps to zero once within `bury_mm` of it. A 4 cm footprint is buried in 5 hours of the test's wind; a 0.5 m drift the player's new wall is short of reaches its steady state in 5 days; the deepest pit (`max_depth_mm`, 2 m) in about 33 days of the mean.

**Exact whatever the cadence.** The amount a vertex moves between t₀ and t₁ is `floor(F(t₁)) − floor(F(t₀))` of one absolute function F of time, so splitting an interval never changes the sum, and a decay followed by the snap composes. An overlay advanced every ten minutes and baking each stamp at once, one that only queues stamps and bakes when its queue is full, and one advanced once a day are the same bytes (the test runs a hundred stamps three ways). So a tile advanced every tick, one caught up when it comes back after a month, and a save taken anywhere between them are the same world.

**The bound, as a number.** The persistent record is a 56-byte header (tile, time, the rules' hash, the lag, the block mask), the 16 × 16 blocks of the grid that are not all zero, and the queue of at most 32 stamps not yet baked in: **at most 33,848 bytes a tile, whatever happened on it and for however long**. A tile whose grid is zero, whose lag is zero and whose queue is empty stores nothing. How long a record outlives the last stamp is bounded too — a vertex is never deeper than `max_depth_mm`, so every record is buried within that depth's worth of flux, and `burial_time_us` says when. The test walks a player back and forth across one tile for 30 days (12,010 footprints and a 0.9 m pit every third day): the largest record was 28,728 bytes, and 4 days after the last stamp the tile stored 56 bytes — its lag. A player who survives for weeks stores what the last few weeks' footprints left unburied, which is no more than a player who survived a day left after a day.

**Why lossless blocks and not 05 §5.13's compressed low-resolution copy.** The plan says an unloaded tile keeps a compressed low-resolution copy or a "trampled amount". A low-resolution copy loses exactly what gameplay reads the grid for — a 25 cm footprint trail becomes a metre-wide smear, and tracking dies the moment a tile unloads — while the sparse blocks of a trampled tile are a few kilobytes and those of a buried tile nothing. The bound is the same kind of number either way; this one does not change the world when a tile unloads.

**Seams.** A tile owns its vertices [0, 128) in both axes; its far edge is its neighbour's first row, read from the neighbour's overlay when that is loaded (`Deformation::overlays`). A stamp near an edge is applied to every loaded tile its footprint covers — the host's job. An unloaded neighbour reads as zero, which only matters beside a tile nobody is near.

## The feedback rule

[feedback.h](../../domain/terrain/include/domain/terrain/feedback.h). The owner would like footprints to change the dunes' future; a footprint that perturbed the base field would make every later evaluation depend on history, which is unbounded storage. So **the base field never reads the overlay**, and the only feedback is a **lag**: per tile and per band, a quantized distance (8 cm units, at most 96: 7.68 m) by which the band's lattice is held back upwind while it passes the tile — "your walls changed where the dune came to rest". It is four bytes of the tile's state (`TileLag`), it saturates, and `LagField` interpolates it bilinearly between tile centres, so the surface stays continuous (the largest step between neighbouring millimetres is 1 mm of lag) and a tile's own centre reads its own lag exactly. It is nudged by **large obstacles only**:

- a dug pit whose sand removed (over the vertices more than 10 cm down) is at least a cubic metre nudges its tile's stored lag one unit, **at most once a game day**, when the dig is baked;
- a declared drift of at least a cubic metre — a ruin's, or a wall the player built — holds its tile back one unit **for every day since it was declared**, a closed form of time that needs no storage (`derived_lag_units`).

A host puts the stored lag plus the derived one into the `LagField`, clamped to the ceiling. The history a tile carries is therefore at most 96 units per band, and the field reads (seed, lags, t): the same lags at the same time are the same dunes, however they came to be. The test digs a pit every day for a hundred days under a ceiling of 20 and the lag climbs a unit a day to 20 and stays there — and outlives the burial of the pits that made it, as the few bytes of history it is.

**Why per tile and not per primitive**, which is what the owner's note suggested: a primitive migrates through tiles, so an offset stored with it would have to be found wherever it had got to — a lookup into the state of whatever tile nudged it, arbitrarily far upwind after years — and written into tiles that are not loaded. A lag stored with the tile holds back every primitive of the band while it passes, which is what an obstacle does to sand, and costs a read of the tile and its eight neighbours. [ADR-0043](../adr/0043-dunes-as-a-function-of-time.md) records this and the alternatives.

## What the effect needs

The owner wants sparse volumetric sand blowing off the dune crests: renderer work for the owner's machine, which this module does not do. It exposes, per tile and per t, what that pass needs (`TileOutput`):

- **Crest lines** (`CrestLine`, 84 bytes): every gathered primitive whose crest crosses the tile, as five points on the surface in world metres (a transverse crest from −0.9 to 0.9 of its half length, bent at its ends; a barchan's brink, the upwind arc of its scoop from horn to horn), the unit vector down its slip face, its crest height, its sharpness (0 rounded, 1 at the angle of repose: a plume leaves a sharp brink) and its celerity in metres a day.
- **The local wind** (`TileWind`): the day's direction as a unit vector, its speed (`k_mean_wind_mps`, 8 m/s, at the record's mean strength), and the day's sand flux per metre of width.
- **A saltation flux estimate**: that flux times the share of the tile that is loose sand, m² a day. A plume's density per crest metre is a function of it and of the crest's sharpness; the function is the pass's to choose.

## Where it attaches

- **The renderer** serves its `TerrainSampler` from the generator when a scene's terrain says `"generator": "Dunes"` (`engine.scene.Terrain` version 2: `generator`, `time`, `sand_flux`), and builds the terrain's mesh from it a 64 × 64 block of vertices at a time; the waves stay the default, and the ruins stand on `TerrainSampler::ground`, the generator's floor ([renderer](renderer.md#scenes-camera-paths-and-flythroughs)). [`content/test-scenes/desert-dunes/`](../../content/test-scenes/desert-dunes/README.md) is the scene for the owner's fly-through.
- **The world ring** holds a tile of the field for every tile it holds (`world::TerrainTiles`): built on activation at the ring's resolution (128, 64, 32 cells), rebuilt when a lag round it moves, dropped on deactivation; the tile's overlay is a projection in the store (`engine.terrain.OverlayTile`), read and caught up on activation, written back — or erased once buried — on deactivation, and carried in the tile's snapshot ([world](world.md#the-consumers)).
- **`engine-content terrain <scene> --tile x,z --time t`** prints one tile's statistics, hashes, wind and crest lines as one JSON line, for tests and agents ([apps](apps.md#engine-content-terrain-a-tile-of-the-dune-field)).

## LOD policy

`Detail` (dunes.h) says which terms a surface computes, and the overlay is the coarse CPU grid:

| Detail | Terms | Where |
|---|---|---|
| `full` | floor, every band, ripples and grain | a sample spacing of a quarter of a ripple (3 cm) or finer: the renderer's deformation-map refinement, never a tile mesh (`detail_for_spacing`) |
| `dunes` | floor, every band | the tile meshes within 2 km (`detail_for_distance`) |
| `coarse` | floor, draa and crest | past 2 km, where a 1 m barchan is a pixel or two at 1080p |
| `floor` | the interdune floor | what ruins stand on |

A tile's cells a side is the host's choice: 128 (the overlay's 25 cm grid) near, 64 and 32 farther out — 2.5, 0.74 and 0.27 ms a tile (below).

## Invariants

Each is a test in `tests/terrain_tests.cpp` or `tests/overlay_tests.cpp`.

- The integer sine is within 3 of the sine in Q15 (1, measured); `isqrt` is exact.
- The wind record's closed-form integral equals the day-by-day sum at every day boundary, across its period and before its epoch; an interval's integral is the difference of its ends'; its mean is the flux asked for; some days are calm.
- The field is a function of time: t₂ directly, t₁ then t₂, and t₁ again after t₂ are the same tiles.
- Bands move downwind at the flux over their height, the lowest fastest, in the inverse ratio of their heights.
- Tiles meet without a seam — heights, normals and materials of shared edges bit for bit — at three times and three places.
- The sampler answers every mesh vertex's height and normal; the point query is the same number; the mesh is counter-clockwise from above.
- The floor never moves and is the ruins' ground; sand is never below it.
- The dunes have relief (−0.7 to 4.2 m over 49 tiles of 3 m dunes at three years), crest lines with unit lee vectors, barchans; slopes stand at the angle of repose, with the superposition's excess measured (below).
- A seasonally reversing wind moves a crest's slip face to both sides, never in one day.
- A ridge holds the sand back and thins it near, and changes nothing far; a basin's centre has no sand.
- A lag field is exact at tile centres, continuous, clamped to its ceiling, and moves the dunes near it and not far.
- A drift's steady state and volume; derived lag is zero before a drift, a unit a day after, and saturates; a small drift never lags.
- The LOD policy: `coarse` is `dunes` without the barchans (lower or equal everywhere, different somewhere); `floor` is the floor; ripples are millimetres.
- Many tiles are the same with no pool, one worker or three.
- The golden hashes of the reference field and its tile at three times.
- The erg profile is valid, meets its neighbours bit for bit at three times and three places, is a function of time, and its mega-draa move less than a hundredth of what its waves do; a floor-coupled band is exactly zero wherever the band before it stands above its coupling height.
- The statistics equal a brute-force count on a 48 × 48 window and are the same on three threads; their goldens (default 1 km at 4 m, the erg 6 km at 24 m, the erg at 0.5 m over a mega-draa's slip face) hold.
- The band table: the default written out is the empty table's tile bit for bit; an empty table, a zero share, a cell smaller than its dune, bands out of order, bad heights, a coupling on the first band and too many bands are refused.
- A footprint's residual only shrinks and is buried when `burial_time_us` said; the tile then stores nothing.
- Eager, queued and daily-advanced overlays are the same bytes.
- A record reads back to the same bytes and the same future; another tile's, other rules', a truncated one and junk are refused; a stamp earlier than the last is refused.
- Thirty days of footprints and pits never store more than the bound, and the tile keeps only its lag once they are buried.
- A dug-out drift refills toward its steady state every day; rock takes no print.
- Pits nudge the lag once a day, not per pit, and not at all when small; the lag saturates and outlives the pits.

## Public API

- `domain/terrain/terrain.h` — the capability: `k_determinism`, `overlay_rules_from_tunables`, and every header below.
- `domain/terrain/fixed.h` — `fx::floor_div`, `floor_mod`, `sin_q15`, `cos_q15`, `isqrt`, `length`, `falloff_q16`, `unit_q16`, `draw`.
- `domain/terrain/wind.h` — `WindParams`, `WindDay`, `FluxIntegral`, `WindRecord`, `day_of`, `k_record_days`, `k_us_per_day`.
- `domain/terrain/dunes.h` — `TileCoord`, `tile_key`, `k_max_bands`, `k_lag_slots`, `lag_slot`, `PrimitiveKind`, `BandCouple`, `BandMetres`, `band_from_metres`, `BandDesc`, `default_bands`, `validate_bands`, `Detail`, `detail_for_spacing`, `detail_for_distance`, `RidgeFeature`, `BasinFeature`, `FieldDesc`, `field_hash`, `k_generator_version`, `Primitive`, `Gather`, `Sample`, `DuneField` (with `ground_height`, `band_count`, `band`, `band_name`), `height_m`, `to_mm`.
- `domain/terrain/feedback.h` — `TileLag`, `FeedbackRules`, `DriftDecl`, `drift_volume_mm3`, `drift_height_um`, `derived_lag_units`, `LagField`.
- `domain/terrain/overlay.h` — `Stamp`, `StampKind`, `OverlayRules`, `Overlay`, `k_overlay_cells`, `k_overlay_record_max_bytes`.
- `domain/terrain/stats.h` — `FieldStats`, `BandStats`, `field_stats`, `evaluate_grid`, `slope_bin`, `k_slope_bins`, `k_slope_edges_deg`, `k_flat_um`.
- `domain/terrain/tile.h` — `TileOptions`, `CrestLine`, `TileWind`, `Deformation`, `TileOutput`, `evaluate_tile`, `evaluate_tiles`, `build_tile_mesh`, `TileSampler`, `grid_normal`, `material_at`.
- `domain/terrain/schemas/terrain.schema` — `engine.terrain.OverlayTile` (the store's projection kind), `CrestReport`, `TileReport` (version 2: `tile_stats`, `region`), `StatsReport`, `BandStatsReport`.

**Depends on.** `base`, `containers`, `math`, `hash`, `jobs` (`evaluate_tiles`'s pool), `tunables`, `schema`, `terrain_schemas`.

## Tunables

Read once by whoever holds tiles (`overlay_rules_from_tunables`). A record carries the hash of the rules it was written under and is refused under others, so a change to one is a change to the world, not a quiet reinterpretation of a save.

| Tunable | Default | Why |
|---|---|---|
| `terrain.overlay.fill_um_per_cm2` | 11 | fills a 3–4 cm footprint in about half a day of the mean wind and a 2 m pit in about 33 days: prints last an evening, a dug camp a month |
| `terrain.overlay.bury_mm` | 5 | below the depth a foot leaves in firm sand; a deviation this small is not worth a byte |
| `terrain.overlay.max_depth_mm` | 2,000 | a dug trench a person stands in; what bounds a record's life |
| `terrain.feedback.lag_unit_mm` | 80 | a unit is invisible on a dune tens of metres long; a day's nudge never pops |
| `terrain.feedback.lag_max_units` | 96 | 7.68 m: a dune held back by a camp by about a crest's width, never more |
| `terrain.feedback.pit_volume_l` | 1,000 | a cubic metre: a pit, not a footprint or a posthole |
| `terrain.feedback.drift_volume_l` | 1,000 | a wall 5 m long with a 40 cm drift is about one; a post is not |

## Testing

`tools/dev.ps1 test -Filter terrain` runs both files, with no device and no fixture on disk. `tools/dev.ps1 bench -Filter 'terrain.*'` runs the benches.

## Performance notes

Measured on the Linux container this capability was written in (4 vCPUs, `linux-gcc-release`, load average 0.6, no `--require-quiet`; [E36](../experiments/e36-desert-generator.md) has the quiet runs):

- **`terrain.tile.eval`**: a 32 m tile with normals and crest lines, 2.49 ms at 128 cells a side (16,641 vertices, an apron round them), 0.74 ms at 64 and 0.27 ms at 32. **`terrain.tile.eval_far`**, the same tile a thousand years in: 2.59 ms. The inner loop tests every gathered primitive (about 40) per point; moving its divisions into the gather as Q32 reciprocals took it from 3.56 ms to 2.49 (`Primitive` grew from 56 to 72 bytes for it).
- **`terrain.overlay.decay`**: an hour's advance of a settled tile (a few footprints) 6 µs, of a trampled one (every block holding a deviation) 228 µs. An empty grid costs nothing.
- **`terrain.wind.integral`**: 9.8 ns.

## Not yet

- **Superposed slip faces pass the angle of repose.** The bands add, so where a crest's slip face coincides with a draa's the slopes add too: 0.48% of the sand vertices of 49 reference tiles stand steeper than 36°, the worst 53.5°. A repose limiter in closed form — the lee of the sum taken as the envelope of the bands' lees — is the fix; a mask that removes the crest band on a draa's slip face was tried on paper and makes the transition steeper, since the mask's own gradient times a crest's height is a slope.
- **The renderer's pass.** The crest lines, wind and flux are there for a blowing-sand pass nobody has written; the GPU deformation map of 05 §5.13 that refines the overlay's grid is not written either.
- **Stamps come from nowhere yet.** Nothing in the tree makes footprints: a character's feet, a vehicle's wheels, a dig tool are a game's, and call the host's `deform`.
- **A registration point for scene generators.** The renderer links this capability where it is configured, exactly as it links the ruins, which makes it the second generator ADR-0037 said would trigger a registration point; [ADR-0043](../adr/0043-dunes-as-a-function-of-time.md) says why that design is left to the owner.
- **Barchans are sharp in every wind.** Their slip face does not round off in a calm the way a crest's does.
- **Nothing shows the dunes moving.** `time` is a scene constant and the field is built once, so a flight cannot answer [ADR-0043](../adr/0043-dunes-as-a-function-of-time.md)'s own revisit question (do the migrating lattices read as dunes advancing?). A time-lapse — game time at a day to a week per real second, the field re-evaluated on the job system every second or so and re-uploaded — is a re-evaluation of the closed form and belongs to the renderer's terrain path with a flag on `engine-view`.
- **The crest is five samples wide.** At 2.5 m a sample (2,049 over 5.1 km) a 34° slip face is a faceted plane with hard edges; near the camera the mesh wants the 25 cm grid the tile path already knows how to build, and far away the coarse one. Its dark grey colour in the same flight is the renderer's: not the missing ground bounce the session first took it for, but a smooth shading normal leaning away from a grazing camera across the crest, which lost the sun until the BSDF clamped n·v instead of rejecting it ([renderer](renderer.md#a-shading-normal-is-not-the-surface)); the facets remain.
- **Storms inside a day.** The record's resolution is the day, with a day's flux spread evenly over it, so the strongest thing it can say is a windy day. Desert Survival wants sandstorms ([13 §13.1](../plan/13-reference-consumer-games.md#survival-mechanics), direction note of 2026-09-25): a few hours of extreme wind with a direction the player shelters from, during which the dunes move by an order of magnitude more than a calm day moves them — which the flux integral gives for free once the record carries **storm hours** (a seeded set of storm intervals per period, each with a direction and a flux profile, summed into the same prefix sums so `I(t)` stays three lookups). The same record must then be what the dust density, the drift against walls and the gameplay read, so that one wind blows everywhere; `WindRecord::day` and `WindRecord::integral` are the functions to grow, and nothing else should invent a wind.

## Capability contract (ADR-0027)

This is a capability: it was added without editing `core/`, `foundation/`, the render graph, the scheduler, or another capability, and it can be removed from the build the same way.

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | stands alone |
| Component and event types | `domain/terrain/schemas/terrain.schema`: the overlay's projection kind and the tile report | done |
| Tick scheduler entry | none: the field is a function, evaluated where a tile is built; an overlay advances where its tile is held | not needed |
| Render-graph passes | none: the outputs are for a future pass (above) | not needed here |
| Content-build derived step | none: a tile is evaluated where it is needed, from its seed and t; `field_hash` names a field for a cache that wants one | not needed |
| Protocol methods | none yet | not needed |
| Tunables | `terrain.overlay.*`, `terrain.feedback.*` | done |
| LOD policy | `Detail`, `detail_for_spacing`, `detail_for_distance`; the overlay is 05 §5.13's CPU grid | done |
| Determinism | `terrain::k_determinism` = `derived`: a function of its description and t, integer, the same bits on every toolchain (golden hashes); the overlay is persistent state in the store, and its evolution is independent of when it is advanced | done |
| Zero cost when unused | no linked code (`ENGINE_WITH_TERRAIN=OFF`) | done |
| Tests and size table | `tests/terrain_tests.cpp`, `tests/overlay_tests.cpp`, `tests/size_table.cpp` (`Primitive` 72, `WindDay` 20, `Stamp` 32, `TileLag` 4, `CrestLine` 84) | done |
| Bench | `bench/terrain_bench.cpp`: `terrain.tile.eval`, `terrain.tile.eval_far`, `terrain.overlay.decay`, `terrain.wind.integral` | done |
| Removal proof | `ENGINE_WITH_TERRAIN`, off in the minimal build | works |

**Removing it.** `cmake --preset linux-clang-minimal` (or `-DENGINE_WITH_TERRAIN=OFF`) drops the module, its tests, and its bench; the module disappears from `build/<preset>/modules.json` and is listed there under `disabled_capabilities`. Everything else builds and passes.
