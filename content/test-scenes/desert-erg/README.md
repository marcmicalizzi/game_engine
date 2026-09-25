# desert-erg

An **erg** drawn by the terrain capability's dune generator ([terrain](../../../docs/subsystems/terrain.md#the-erg-profile), [ADR-0043](../../../docs/adr/0043-dunes-as-a-function-of-time.md)): the owner's reference after his first flight over the generator ([third session](../../../docs/experiments/third-interactive-session-2026-09-25.md)) — Erg Chebbi's kind of dune sea, towering dunes at kilometre spacing with long sinuous crests and slip faces in shade, medium dunes on their flanks, waves and ripples on every surface, and flat interdune floors between. 6.1 km a side (`extent` 3,072 m) on a 4,097 grid (1.5 m a sample), seed 7, no ridges, no basin, no meshes (it needs no samples), three years into the world (`"time": 94608000`).

```powershell
build/msvc-release/bin/engine-view --scene content/test-scenes/desert-erg/scene.json --interactive
build/msvc-release/bin/engine-content terrain content/test-scenes/desert-erg/scene.json --tile 0,0 --stats --stats-side 6144
```

## The band table

Tallest first, because a band is shaped by the ones before it (terrain.md, "The band table"). The numbers are chosen from the owner's description and the proportions of the photographs he sent, and every one is a field of `scene.json`: the owner decides them by flying.

| Band | Kind | Height | Cell | Share | Couples | Why |
|---|---|---|---|---|---|---|
| `mega-draa` | transverse, sinuosity 0.08, crests 1.2–2 cells long | 80–200 m | 2,400 m | 0.85 | — | "towering dunes of a hundred metres and more at kilometre spacing": the draa of a real erg are 100–300 m high and 1–3 km apart; crests longer than a cell overlap into long sinuous lines, and a sixth of the cells empty leave corridors. Their slip faces remember a year of wind and sharpen over four months, because a 150 m face does not reform in a week |
| `draa` | transverse, sinuosity 0.04 | 10–25 m | 360 m | 0.7 | on the flanks, over 8 m | "medium dunes on their flanks": they fade in as the mega-draa's sand rises past 8 m, so the floors stay open |
| `crest` | transverse | 2.5–6 m | 110 m | 0.6 | on the flanks, over 3 m | the scene's own wavelength, riding on everything taller |
| `barchan` | barchan | 1.5–5 m | 150 m | 0.35 | on the floors, under 2 m | crescents on the hardpan, fading out where the tall bands' sand passes 2 m |
| `wave` | transverse, never a slip face | 0.3–0.8 m | 10 m | 0.6 | — | "waves on every surface", between ripples and dunes; they follow a few days of wind and are rounded, not sharp |

Below them the ripples (12 cm apart, 6 mm high, the day's wind) are the field's detail term, drawn by the renderer's near-field refinement rather than the mesh.

## What to look at

The camera path (`camera-path.json`, 60 s at 60 fps, west to east, which is upwind here) and its markers:

| Frame | Marker | What should be there |
|---|---|---|
| 0 | `floor` | an interdune floor: flat, with waves; a mega-draa's slip face in shade ahead |
| 1080 | `toe` | the foot of the slip face: 150–200 m of sand at the angle of repose |
| 2040 | `crest` | over the crest: a sharp, sinuous brink; draa and crest segments on the long stoss beyond |
| 3000 | `barchans` | the eastern floor: barchans, waves, flat ground to the next mega-draa |
| 3600 | `overview` | the erg from 350 m up: mega-draa about 2 km apart, open floors between |

**What `time` shows.** Bagnold's rule leaves the mega-draa almost still — a metre a year in this wind (200 m² a year of sand flux) — while the draa move about 11 m a year, the crests 48, the barchans 62 and the waves about 330: in three years the waves have crossed a kilometre of floor and the tall dunes have not moved their own width. Set `time` a month and a year apart to compare.
