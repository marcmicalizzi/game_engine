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
  the spruce and 1.95 to 2.5 on the pine; per pixel 2.25 to 2.9 and 3.0 to 4.5. **The far form
  averages a little of it away**: at 300 m the spruce's are 1.45 and 2.25, against 1.70 and 2.6 at
  100 m.
- **The gaps between boughs show wood as bright as the needles.** The tree's own trunk and bare
  inner branches return 0.49 to 0.70 of an albedo of one (0.34 to 0.47 where the sun is blocked),
  and their flat bark colour (0.26, 0.18, 0.12, luminance 0.19, where no pack names a
  `conifer/bark`) is twice the needles' albedo, so a gap's pixels are 0.093 to 0.135 in luminance
  against the needles' 0.074 to 0.12. A gap that should be the crown's dark inside is a lit
  brown-orange line.

### Is it the cards?

The brief's two suspects in what a card states, each tried on main's own binary by a pack whose
needle roles declare other optics (a role's `transmission` and `normal_spread` replace the part's
own; materials.md), the same views, sun at 12:30, the bounce on (2026-11-17 21:43 to 21:52, the
session locked):

| needle card's `transmit`, `spread` | spruce: lit over blocked, 10 to 300 m | over half-metre blocks | pine |
|---|---|---|---|
| 0.47, 0.8 (main) | 1.50 to 1.64 | 1.45 to 1.70 | 1.36 to 1.65 |
| 0.27, 0.8 | 1.47 to 1.65 | 1.42 to 1.71 | 1.31 to 1.66 |
| 0.27, 0.4 | 1.51 to 1.78 | 1.48 to 1.72 | 1.34 to 1.67 |
| 0.47, 0.4 | 1.59 to 1.76 | 1.47 to 1.71 | 1.40 to 1.67 |
| 0.27, 0.13 | 1.57 to 1.87 | 1.53 to 1.73 | 1.37 to 1.67 |

**Neither is the cause.** A card a quarter as transmissive moves the shade 7 % and the sun as much;
a card nearly all sheet (spread 0.13, the sun answered by which way its plane faces) raises the
lit over the blocked by a tenth and leaves a bough's spread where it was. The 0.27 is what the
shoot's escape probability would make of a needle's 0.47 (0.47 × 4 STAR, 4 STAR about 0.6 for
Palmroth et al.'s middle shoot): grounded, but it moves nothing a person would see, so the
declared optics stand as R164 set them.

### Where the evenness comes from

The shade on a needle is ambient light, and there is a lot of it against the sun:

- **The sun is weak against the sky in the frame.** On level sand in the open the engine's lit
  over blocked is 2.1 with the bounce off (the sun at 63°, turbidity 2.5): the direct sun on a level
  surface is about as much as the sky's. A clear sky at that height gives several times more
  direct than diffuse light in the visible (*recalled*, not read tonight; the sky's own model is
  the place to check it). A needle, which takes the sun by the Lambert sphere's law (a fifth to two
  thirds of a sheet facing it) and the sky from both sides (1 + transmit of it), shows the same
  flatness more.
- **What lights the shade is the whole hemisphere, the sunlit ground below included**, scaled by
  the crown's prior where the bounce reads it: the bounce takes 10 to 30 % of a shaded needle's
  light on the crown's outside, where Stenberg et al. measured openness up to 0.9, and the wood
  inside keeps half of an outer needle's light where the openness falls to 0.2.
- **The gaps are bright wood** (above).

None of those is a card's: they are the sky's balance, the ambient's reach inside a crown and the
bark's colour, and the rows below are where each is answered.

## What changed

- **The needle's colour** (`matgen`, `k_matgen_version` 12; [matgen](../subsystems/matgen.md#spray-sets)):
  the spruce's and the bare conifer's sprays draw a one-year-old needle's measured colour and the
  year's growth 1.17 times paler; the pine follows at its own hue. Only the three sprays' images
  move.
- **Nothing in what a card declares**: the needle card's transmittance and its sphere's share stay
  R164's, since neither moves a crown's light and dark (the table above).

**What it does to the crowns** (the same views and runs as the table above, the branch's build
with a pack its own `engine-content matgen` wrote, 2026-11-17 23:09 to 23:13, the session locked,
the CPU at 100 %): the spruce's needles are 0.096 to 0.126 in luminance where they were 0.080 to
0.103 (+19 to +23 %), the pine's 0.087 to 0.147 (+18 to +20 %); lit over blocked 1.49 to 1.65
on the spruce and 1.36 to 1.68 on the pine, half-metre blocks 1.43 to 1.67, all where they were.
The agent's reading of its own pictures: the crowns are lighter and more olive than main's
grey-green, as a needle's measured colour is under a sky's blue ambient, and **as even as they
were**: the colour was wrong and is right, and the evenness is the light's.

## What it costs

Nothing a frame can measure: the three images keep their size, format and mask (the spruce's
opaque share is 0.2533 before and after). The `tree-species` scene's own held views at 3840 x
2160, `--raster rt --shadows rt`, the bounce on, main's build and pack against the branch's,
alternating in one lease (2026-11-18 00:10, the session locked, other processes at 100 % of the
CPU, so these are upper bounds): GPU total 4.477 and 4.478 ms on main, 4.478 and 4.493 on the
branch; the resolve 1.81 and 1.84 against 1.84 and 1.85. Where the conifers' 0.66 ms could be given
back is the renderer's: the R164 page's foliage-free resolve module and the sun rays a shaded
foliage pixel asks.

**A grove that draws no trees.** The engine's own `grove` scene, and the same scene with a sky,
drew almost none of its twelve trees on main's build (a1eae86f) in these runs, a fresh derived-data
root as much as a warmed one: views at 10 to 300 m show sand and sky, and from under the spruce a
few branches stand in the air. Its frame at 4K was 2.2 ms against R164's 4.9 on the same path. So
the grove is not a measure of a conifer's cost on this main, and its cause was not looked for here.

## What an imported asset gets, and a scene with none

A spray is the engine's own image for a role a pack may leave to it, so an imported conifer and a
pack that names its own needles draw exactly what they drew; a scene with no conifer, or with
conifers on a pack's own needle images, draws the same bytes. What recooks: every generated set's
key moves with `k_matgen_version`, so a materials root's `generated` folder is stale and is built
again (`engine-content matgen --out <dir>`), and every generated mesh keyed against a pack with a
`generated` folder recooks: **every forest recooks once**. No flora, renderer or shader byte moved.

## Not done, and the rows it leaves

- **The sky's direct against its diffuse light**, measured here at about one to one on level sand
  at 63°; a renderer row to hold the sky model's sun and sky against a published clear-sky model
  in the visible, since every shadow in every scene is as flat as this one.
- **The ambient inside a crown**: the crown prior takes 10 to 30 % from a shaded needle on the
  crown's outside and keeps half of an outer needle's light on the wood inside, where the
  literature's openness falls to 0.2; and the sunlit ground below is in every needle's
  hemisphere. The renderer's (the prior, the cache, the resolve's foliage ambient).
- **The bark in the gaps**: the conifer's flat bark (luminance 0.19) is twice the needles' albedo;
  a measured spruce and pine bark (Juola, Hovi and Rautiainen 2020, *Silva Fennica* 54(4) 10331:
  spruce bark 0.07 to 0.53 over 400 to 1000 nm, its visible numbers in figures only, read
  tonight) for `conifer/bark` and the flat colour.
- **The year's growth at a bough's tips as a material of its own**: the solver knows each card's
  shoot age along its axis (`needle_years`, `shoot_growth_m`); the outer card of every axis is
  the current year's. A second needle material with the current-year colour carries it, any asset
  can state the same with two materials, and the impostor chain's spray ranges have to be split
  for it, which is why it was not done here.
- **Sun and shade shoots**: shade shoots are flat and sun shoots bushy (Stenberg et al. 1999), which
  the cards' lift already follows; a shade needle's own optics were not found in text.
