# Light within a conifer crown (G218, 2026-11-17)

**What prompted it.** The forest session walked 51,633 conifers on the engine's needle sprays
([conifer crowns](conifer-crown-coverage.md), [foliage, lit from both sides](foliage-two-sided-light.md))
and wrote: the pines went from thin olive strands to closed crowns and the spruces read bluer and
greyer, but at 1:1 the crowns are one even grey-green, with little light and dark between boughs.
A real spruce is tiers of lit bough tops over dark gaps, its new growth lighter at the tips. This
page is why the engine's crowns are even, what the literature says light does inside a conifer
crown, and what was changed.

(In progress: the measurement of the engine's crowns is below; the sources and the mend follow.)

## How even the engine's crowns are

The `tree-species` scene's spruce and pine with a sky (latitude 46.5, day 200), at 10, 30, 100 and
300 m and from under the spruce, the eye at 1.7 m, 1920 x 1080, `--raster rt --shadows rt`, the
bounce on (`renderer.gi.mode=bounce`), main's release build (a1eae86f) with main's generated pack.
Each view drawn shaded, as albedo and as the shadow view, read over the tree's needle pixels (albedo
green over 1.15 red). *Light* is the picture's linear luminance over the albedo's: what an albedo
of one returns. *Lit* and *blocked* are the shadow view's classes (the sun reaches the pixel, or a
ray to it is stopped). 2026-11-17 21:05 to 21:11, the session locked, other processes at 100 % of
the CPU (a GPU number here is a ratio of two pictures, which load does not move).

| sun | tree | lit share | light, lit | light, blocked | lit over blocked | the ground beside it, lit over blocked |
|---|---|---|---|---|---|---|
| 12:30 (63°) | spruce, 10 to 300 m | 0.47 to 0.49 | 1.12 to 1.34 | 0.71 to 0.89 | 1.50 to 1.64 | 3.2 to 5.0 |
| | spruce, from under | 0.34 | 1.43 | 0.96 | 1.50 | 4.5 |
| | pine, 10 to 300 m | 0.37 to 0.40 | 1.14 to 1.67 | 0.69 to 1.23 | 1.36 to 1.65 | 2.7 to 4.2 |
| 12:30, the bounce off | spruce | | 1.32 to 1.43 | 1.00 to 1.07 | 1.30 to 1.36 | 1.9 to 3.6 |
| 18:00 (15°) | spruce | | 0.97 to 1.02 | 0.55 to 0.63 | 1.58 to 1.81 | |

- **A needle in the sun returns only half again what one in the shade does**, at every distance
  and with the sun high or low, where the ground beside it returns three to five times. No needle
  pixel is ever turned away from the sun (the shadow view's third class is empty): with 0.8 of a
  card a sphere of normals, a bough's underside takes the sun as its top does.
- **The shade is mostly light that is there anyway**: with the bounce off a shaded needle returns
  1.0 to 1.07, and the bounce's crown prior takes 10 to 30 % of it away.
- Over half-metre blocks (a bough) the picture's 90th percentile is 1.45 to 1.70 times its 10th on
  the spruce and 1.95 to 2.5 on the pine.
