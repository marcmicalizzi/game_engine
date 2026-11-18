# Light within a conifer crown (G218, 2026-11-17)

**What prompted it.** The forest session walked 51,633 conifers on the engine's needle sprays
([conifer crowns](conifer-crown-coverage.md), [foliage, lit from both sides](foliage-two-sided-light.md))
and wrote: the pines went from thin olive strands to closed crowns and the spruces read bluer and
greyer, but at 1:1 the crowns are one even grey-green, with little light and dark between boughs.
A real spruce is tiers of lit bough tops over dark gaps, its new growth lighter at the tips. This
page is why the engine's crowns are even, what the literature says light does inside a conifer
crown, and what was changed.

## What the literature says

Each row says whether its source was **read** for this row (its text, a table or an abstract, as
said), read on an earlier page (**read there**), or is **recalled**; nothing is pinned to a recalled
number.

| What | Source | What it says |
|---|---|---|
| A shoot's mutual shading, and what escapes it | Smolander and Stenberg 2003, *Remote Sensing of Environment* 88:363-373, sections 2.3, 2.4 and 3.3 (read, the authors' copy) | STAR, a shoot's spherically averaged silhouette over its total needle area, would be 1/4 for needles that shade each other not at all (Cauchy's theorem); **4 STAR is the shoot shading factor** (Stenberg et al. 1994; Stenberg 1996) and also **the mean probability that a photon leaving a needle's surface escapes the shoot without meeting another needle**; the shoot's scattering is the needle's times (1 - p) / (1 - p w), with p close to 1 - 4 STAR (their Fig. 6, nine Scots pine shoots); the shoot's phase function has a **peak in the backscattering direction** (the within-shoot hot spot) |
| A spruce shoot's STAR | Palmroth et al. 2002, *Tree Physiology* 22:1185 (read there: [conifer crowns](conifer-crown-coverage.md)) | 0.103 to 0.209 over 305 shoots, canopy openness 0.02 to 0.96 |
| Sun shoots and shade shoots, and the light down a spruce crown | Stenberg, Kangas, Smolander and Linder 1999, *Plant, Cell and Environment* 22:1133-1142 (abstract read) | canopy openness at the shoots **0.23 to 0.90** in control trees; needle width and thickness rise with openness; **STAR falls with openness**, so "shoot silhouette area per unit needle mass was up to three times larger in shade shoots than in sun shoots": the sun shoots at the crown's outside are bushy and shade themselves, the shade shoots inside are flat |
| A needle's colour by age | Hovi, Lukeš, Homolová, Juola and Rautiainen 2022, *Silva Fennica* 56(2) 10683, supplement S1 table S1.5 (read; top-of-canopy Norway spruce, every campaign's mean) | reflectance at 490, 560 and 665 nm: current-year needles **0.069, 0.165, 0.027**, one-year-old **0.062, 0.139, 0.026**; transmittance at 560 nm 0.085 and 0.059. The main text (read): "the c0 needles had higher albedo compared to c1 needles"; its bottom-of-canopy samples are not reported |
| Older needles darker still | Lhotáková et al. 2021, *Remote Sensing* 13(3):391, section 3.3 and Fig. 5 (read; sun-exposed branches of 123 trees, needles of the current year, the year before and four years and older) | reflectance and transmittance "decreased with needle age in the green part of the visible (VIS) spectrum"; no number in the text |
| A shoot scatters backward | Rautiainen et al. 2018, *Remote Sensing* 10:207 (read there: [conifer crowns](conifer-crown-coverage.md)) | Scots pine shoots "scatter most strongly in the backward directions with very little forward scattering in blue and red" |
| A spruce's needles above and below its twig | Gymnosperm Database, *Picea abies* (read there) | "above shoot directed forward, those below more or less pectinate": a bough is a plate, all round on top, parted below |

**What that asks of a crown.** A real crown is light and dark at the scale of its boughs for three
reasons, and none of them is the needle's colour: the sun reaches the outside of the crown and the
tops of the boughs and not what is under them; the sky reaches the inside through openness that
falls from about 0.9 at the outside to 0.2 and less inside (Stenberg et al. 1999, Palmroth et al.
2002); and light that enters a shoot leaves it only with the probability 4 STAR, about 0.4 to 0.8 for
a spruce, so a shoot is brightest seen from the sun's side (backward) and dull from its far side.
Within that, the year's growth at the end of each shoot is a fifth lighter than the needles behind
it, and those darken further with age.

## A needle's colour

The sprays' recipe drew a spruce needle at 0.050, 0.100, 0.040 linear (luminance 0.085), a colour
recalled on [conifer crowns](conifer-crown-coverage.md) and marked there to be checked. Hovi et
al.'s bands (table above) are reflectance at three wavelengths; between them the spectrum is not
in the text, so the linear sRGB colour was found with two shapes through the three bands (straight
lines between them, and a baseline under a Gaussian green bump at 555 nm fitted through them),
integrated against the CIE 1931 colour matching functions (Wyman, Sloan and Shirley 2013's
analytic fit) under an equal-energy light, each channel over a white reflector's
(`needle_rgb.py` in the row's scratch):

| needle | straight lines | green bump | luminance |
|---|---|---|---|
| current year (0.069, 0.165, 0.027) | 0.098, 0.136, 0.061 | 0.081, 0.152, 0.021 | 0.121, 0.125 |
| one year old (0.062, 0.139, 0.026) | 0.084, 0.116, 0.056 | 0.071, 0.129, 0.022 | 0.104, 0.107 |

The shapes agree on luminance to 3 % and differ in blue, where neither is pinned by a band. The
recipe takes **the one-year-old needle, 0.078, 0.122, 0.039**, the mean of the two (luminance
0.107, a quarter over the old colour): most of a spruce's needles are a year old or older, and the
year's growth is drawn paler on top of it, **1.17 times**, the two needles' luminance ratio (it was
1.15, chosen). Older needles darken further (Lhotáková et al. 2021) by an amount no text read
gives, so nothing darker is drawn. The pine's colour, 0.052, 0.092, 0.056, has no measured needle
behind it on any page; it keeps its grey-green hue and moves to the spruce's level, **0.065,
0.115, 0.070**. Brighter needles are a quarter brighter everywhere and do not by themselves make a
crown less even; the measurement below is why.

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
