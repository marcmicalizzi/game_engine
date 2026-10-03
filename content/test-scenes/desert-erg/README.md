# desert-erg

An **erg** drawn by the terrain capability's dune generator ([terrain](../../../docs/subsystems/terrain.md#the-erg-profile), [ADR-0043](../../../docs/adr/0043-dunes-as-a-function-of-time.md)): the owner's reference after his first flight over the generator ([third session](../../../docs/experiments/third-interactive-session-2026-09-25.md)) — Erg Chebbi's kind of dune sea, towering dunes at kilometre spacing with long sinuous crests and slip faces in shade, medium dunes on their flanks, waves and ripples on every surface, and flat interdune floors between. 6.1 km a side (`extent` 3,072 m) on a 4,097 grid (1.5 m a sample), seed 7, no ridges, no basin, no meshes (it needs no samples), three years into the world (`"time": 94608000`), with **12 sandstorms a year** (`storms_per_year`; [terrain](../../../docs/subsystems/terrain.md#storms)).

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-erg/scene.json --interactive
build/msvc-release/bin/engine-content terrain content/test-scenes/desert-erg/scene.json --tile 0,0 --stats --stats-side 6144
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-erg/scene.json --interactive --time-rate 86400
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-erg/scene.json --interactive --time-rate 86400 --terrain-rings
```

`--time-rate 86400` runs a game day a real second and moves the sand on screen, blended between evaluated fields every frame so no dune ever steps, at a speed that stays steady however late a field is: the picture runs about 1.5 s of real time behind game time at a day or a week a second, which is what keeps it from standing and sprinting ([renderer](../../../docs/subsystems/renderer.md#a-clock-that-never-stops)). The lights stand still (`--orbit-lights` brings back the old orbit, `--sun <azimuth,elevation>` moves the sun); the summary line's `time_lapse` block says how often the field was evaluated and what it cost. `--terrain-rings` draws the ground near the camera at 50 cm out to 250 m and at a metre out to a kilometre instead of the grid's 1.5 m, so a slip face near the camera is tens of samples wide rather than five, and moves it with the rest ([renderer](../../../docs/subsystems/renderer.md#the-rings-in-the-scene)).

## The band table

Tallest first, because a band is shaped by the ones before it (terrain.md, "The band table"). The numbers are chosen from the owner's description and the proportions of the photographs he sent, and every one is a field of `scene.json`: the owner decides them by flying.

| Band | Kind | Height | Cell | Share | Couples | Why |
|---|---|---|---|---|---|---|
| `mega-draa` | transverse, sinuosity 0.08, crests 1.2–2 cells long | 80–200 m | 2,400 m | 0.85 | — | "towering dunes of a hundred metres and more at kilometre spacing": the draa of a real erg are 100–300 m high and 1–3 km apart; crests longer than a cell overlap into long sinuous lines, and a sixth of the cells empty leave corridors. Their slip faces remember a year of wind and sharpen over four months, because a 150 m face does not reform in a week |
| `draa` | transverse, sinuosity 0.04 | 10–25 m | 360 m | 0.7 | on the flanks, across 400 m | "medium dunes on their flanks": they fade in across the first 400 m inside a mega-draa's footprint, so the floors stay open |
| `crest` | transverse | 2.5–6 m | 110 m | 0.6 | on the flanks, across 100 m | the scene's own wavelength, riding on everything taller |
| `barchan` | barchan | 1.5–5 m | 150 m | 0.35 | on the floors, across 80 m | crescents on the hardpan, fading out across the first 80 m inside a taller dune |
| `wave` | transverse, never a slip face | 0.3–0.8 m | 10 m | 0.6 | — | "waves on every surface", between ripples and dunes; they follow a few days of wind and are rounded, not sharp |

A coupling fades across a **width** inside the taller dunes' footprints, about fifteen times the band's height, and not across a height of their sand: their sand rises as steeply as their faces, and a fade across a few metres of it stood the draa's surface at 37–39° at the foot of a slip face. The fade's own slope is charged to the band's room ([terrain](../../../docs/subsystems/terrain.md#the-repose-limiter)), so a narrower width lowers the band where it fades rather than steepening it.

Below them the ripples (12 cm apart, 6 mm high, the day's wind) are the field's detail term, drawn by the renderer's near-field refinement rather than the mesh.

## The sand close up

Since 2026-09-29 the sand carries **wind ripples and grain** (`"detail"` in `scene.json`, `engine.scene.TerrainDetail`; [renderer](../../../docs/subsystems/renderer.md#the-sand-close-up)): not a texture but a function of the ground's world position, the seed and the wind, drawn per pixel in the resolve and faded by each pixel's footprint, so it has no tile, no seam — across the rings, their chunks or an LOD cut — and no repeat. Every number is the scene's, in the block, and the defaults are written out so they can be tuned by editing: ripples 12 cm apart and 8 mm high, three quarters of a wavelength their windward slope, a defect density of 0.35, faded out between 22° and 30° of slope, and a 2 cm grain moving the albedo by 8% and the roughness by 0.05 either way. The ripples lie across the ground's wind at the surface's time: the day's wind, turned in from yesterday's over the day's first two hours, which here blows towards the west (−x). Since the second pass (version 2 of the block) they also go by **which way the ground faces that wind**: on ground falling away from it they start to fade at a lee slope of 10° and are gone at 18° (`lee_start_deg`, `lee_end_deg`), so a crest's downwind side is smooth however gentle it is. And the grain is **gradient noise** in five octaves from 2 cm down to 1.25 mm (`grain_finest` 0.001) with a share of the normal (`grain_normal` 0.06), where it was two octaves of value noise. And a slip face carries **grainflow streaks** (`streak_*`): tongues 40 cm across and 2 m long down the fall line, from a lee slope of 22° and whole at 30°. And the ripples' **spacing follows the wind** (`spacing_gain` 2.5): 12 cm on the flat, longer climbing a windward slope, 18 cm at 12°. Removing the block draws the erg exactly as before; its terrain hash and cache entry are the same either way.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-erg/scene.json --interactive --walk --terrain-rings --start -1400,2,0 90,-10
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-erg/scene.json --camera-path content/test-scenes/desert-erg/walk-path.json --terrain-rings --offscreen --frames 1201 --capture walk.png --capture-every 240
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-erg/scene.json --interactive --walk --terrain-rings --start 462,2,-40 23,-8 --sun 15,6
```

What to look at (the first two lines; `walk-path.json` is a 20 s walk at eye height whose markers are these views):

- **Along the wind** (yaw 90, west, from (−1400, 0)): the crests run across the view, sinuous, with a crest ending or two joining every few wavelengths; each has a gentle lit windward slope towards you and a short dark lee away from you. They fade into smooth sand some tens of metres ahead — first the lee's sharpness, then the ripple — and should never turn into bands or a moiré as you walk.
- **Across it** (yaw 180, south): the crests run along the view towards the horizon.
- **At your feet** (pitch −50 or so): 12 cm ripples, and the grain as fine texture down to a few millimetres, with no lattice and no blotches, and a low sun catching it between the crests; the finest octave is under a pixel even here. Before version 2 it was a soft mottle whose 5 mm cells read as an upscaled image.
- **A crest at a low sun** (the third line, a mega-draa's brink with the sun 6° up in the east): the stoss's ripples lit at grazing, the brink and the slip face beyond it smooth — ripples fade out towards the angle of repose, and a slip face is avalanched sand.
- **Over a crest, downwind** (any brink with the wind at your back): rippled sand up to the crest, smooth sand from the brink down the lee — on a gentle lee as much as on a steep one — and no line between them. Before version 2 a lee gentler than 22° kept its ripples.
- **Climbing a windward slope**: the ripples open out as the slope steepens toward the crest, from 12 cm on the floor to about 18 cm near the top, with no seam where they change.
- **A slip face** (walk down a mega-draa's lee, or stand twenty metres off one with the sun across it): long shallow lanes of avalanched sand running from near the brink most of the way to the toe, a hand or two high and half a metre across, each a straight chute with its levee running the face's length (fourth pass: from the brink and at your feet they are lanes, not the ovals and crossing dashes of the third), seen by their relief and barely by their colour. Only a quarter of them are there at once (`flow_share`), each coming as sand avalanches and fading as grainfall buries it, about three times a calm day and faster in wind (`flow_turnover` 800), so the face is mostly smooth with a few fresh flows; at `--time-rate 1` stand in the afternoon wind and watch one appear; they fade toward the brink and the toe as the slope leaves the band, and with the sun in front of the face they almost vanish, as real ones do. Version 2's short streaks ("rain on a window") are off (`streak_*` 0); the lanes are `flow_*`.
- **Ripples that are not all alike** (walk the floor): the spacing and the height change over tens of metres (0.7 to 1.4 times the 12 cm in patches about 30 m across, `patch_*`), some stretches of long straight crests beside stretches full of junctions, and on a slope the wind crosses obliquely the crests swing along its contours by up to 20° (`steer_*`).
- **Ripples that move** (`ripple_celerity` 2,000, `flatten_*` 1.6–2.2): at `--time-rate 1` watch a crest against a fixed point for a minute; it creeps downwind, about half a centimetre a minute on a calm day and faster in the afternoon wind. At `--time-rate 86400` the ripples are gone from a moving dune and the sand is a little rougher; bring the rate down and they come back.
- **`--view detail`** shows the function as data: red the ripple height, blue the grain, green where the ripples are drawn.

What does not look right yet: the ripples stand still (real ones migrate a centimetre a minute) and do not flatten in a storm; at a grazing sun they read less strongly than on real sand, because nothing shadows a ripple's lee from the next crest's; and there are no glints. The captures the change was judged by, and what they cost, are in [the experiment](../../../docs/experiments/sand-detail-2026-09-29.md).

## The sky

Since 2026-09-30 the erg names a **sky** (`"sky"` in `scene.json`, `engine.scene.Sky`; [renderer](../../../docs/subsystems/renderer.md#the-sky), [the experiment](../../../docs/experiments/sky-2026-09-30.md)): Earth's, at **31.1° N** (the Sahara's northern edge, Erg Chebbi's latitude), the world's epoch on **day 100** (10 April), air of **turbidity 1.6** (very clear), the region seen past the terrain's edge at an albedo of **0.38**, and the moon **12.39 days** old at the epoch — which is a full moon **three years in**, where the scene's `time` stands, since a thousand and ninety-five days is 37.08 months. The sun, the moon, the stars and the exposure are all functions of that one clock ([renderer](../../../docs/subsystems/renderer.md#one-clock)): the scene's `time` is **midnight**, so a plain run opens **under the full moon**, 57° up just west of south. `--time-of-day <h>` looks at another hour of the same day; `-`, `=` and `0` in a window step and hold the exposure.

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-erg/scene.json --camera-path content/test-scenes/desert-erg/walk-path.json --offscreen --frames 1 --time-of-day 16.5 --capture afternoon.png
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-erg/scene.json --interactive --walk --terrain-rings --start -1380,2,0 82,3 --time-of-day 18.2
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-erg/scene.json --interactive --walk --terrain-rings --start -1380,2,0 171,35
```

What to look at, from a walker's eyes at (−1380, 0):

- **Noon, and the afternoon**: a blue sky over the sand, deepest overhead and paling to a haze at the horizon; looking west towards the sun at 16:30, the white aureole round it; east, away from it, a deep, even blue. The far dunes fade into the air's blue with distance.
- **Sunset, looking at the sun** (`--time-of-day 18.2`, 8° north of west, the sun a degree up; the second line): a yellow-orange band on the horizon under a pink and then violet sky, the sand in the sun's last light warm and in shade mauve, and the air between you and the far dunes glowing.
- **Sunset, looking away** (east): **alpenglow** on the far mega-draa, a warm band low on the horizon, a blue-grey sky above it.
- **Civil twilight** (18.72, the sun 5° down): the orange band under a violet sky, the sand dim and purple-grey, no shadows.
- **The full moon** (the scene's own midnight, looking south 35° up; the third line): the moon's disc, lit whole, with its own aureole in a dark blue-grey sky, the brighter stars round it; turn north and the sand is lit dimly by moonlight, with the cascaded maps' shadows the moon's.
- **A half moon** (the scene with `moon_age_days` 5.03, at 21:00, looking west 35° up): half a disc, lit on the side facing where the sun set.
- **A moonless night** (`moon_age_days` 27.16, at midnight, looking north 20° up): a starfield of about 8,500 stars down to magnitude 6.5, a band of them denser where the Milky Way is, the brightest few coloured orange and blue-white; the sand is a dark silhouette.

What does not look right yet: noon's sky is slate rather than a desert's deep blue, because the exposure meters the sand (twice a real desert's albedo) to white; a moonlit scene is the moon's sunlight colour, not the blue an eye sees; the dark gradients band in 8 bits; the constellations are not the real ones.

## What to look at

The camera path (`camera-path.json`, 60 s at 60 fps, west to east, which is upwind here) and its markers:

| Frame | Marker | What should be there |
|---|---|---|
| 0 | `floor` | an interdune floor: flat, with waves; a mega-draa's slip face in shade ahead |
| 1080 | `toe` | the foot of the slip face: 150–200 m of sand at the angle of repose |
| 2040 | `crest` | over the crest: a sharp, sinuous brink; draa and crest segments on the long stoss beyond |
| 3000 | `barchans` | the eastern floor: barchans, waves, flat ground to the next mega-draa |
| 3600 | `overview` | the erg from 350 m up: mega-draa about 2 km apart, open floors between |

**How fast each band moves** (the net flux along its travel, 193 m² a game year with the storms, over its celerity height; [terrain](../../../docs/subsystems/terrain.md#how-far-the-big-dunes-move)): the mega-draa 1.38 m a game year, the draa 11.0, the crests 45.3, the barchans 59.3, the waves 350. At a week a real second a game year is 52 s of flight, so the mega-draa look still; that is the physics, measured on this scene (the draa's brink moves 11.05 m a year against its band's 11.00). A band's `celerity_scale` (1 by default) multiplies its travel for a cinematic time-lapse — `"celerity_scale": 20` on the mega-draa moves them 28 m a game year — and is a stylization; this scene does not set it.

**What `time` shows.** Bagnold's rule leaves the mega-draa almost still — a metre a year in this wind (200 m² a year of sand flux) — while the draa move about 11 m a year, the crests 48, the barchans 62 and the waves about 330: in three years the waves have crossed a kilometre of floor and the tall dunes have not moved their own width. Set `time` a month and a year apart to compare.

## What the numbers say

`engine-content terrain content/test-scenes/desert-erg/scene.json --tile 0,0 --stats --stats-side 6144 --stats-spacing 4`, the whole scene at 4 m, three years in ([terrain](../../../docs/subsystems/terrain.md#what-the-numbers-say) has what each number is):

| | |
|---|---|
| height above the floor, p10 / p50 / p90 / p99 / max | 0 / 3.2 / 113 / 184 / 193 m |
| flats (within half a metre of the floor) | 38.5% |
| sand steeper than 30° / 34° / 36° | 4.7% / 0.39% / **0** |
| crest per km² | mega-draa 372 m, draa 469, crest 1,963, barchan 92, wave 27,942 |
| the largest dune | 188 m, a mega-draa, the next about 2.1 km away |

No sand stands past the angle of repose: over the whole scene at a metre, at 0, 1, 3 and 7 years and with its storms, no vertex is steeper than 36° ([the repose limiter](../../../docs/subsystems/terrain.md#the-repose-limiter)). The storms move a year's dunes about 7% further than the same wind without them ([terrain](../../../docs/subsystems/terrain.md#storms)).
