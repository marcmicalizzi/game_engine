# Foliage, lit from both sides (R164, 2026-11-16)

What thin foliage does with light, what a card of needles stands for, the model the renderer took
from that, and what it does to the engine's own scenes. The renderer's rules are on
[renderer](../subsystems/renderer.md#foliage-lit-from-both-sides); flora's numbers on
[flora](../subsystems/flora.md#foliage-optics).

**Why.** The sprays' fourth leg ([conifer crowns](conifer-crown-coverage.md#where-the-light-went-the-fourth-leg))
found that the resolve turned a two-sided card's normal to the eye's side of its plane and lit it
by `n.l` alone, so 40 to 98 % of a conifer's needles seen from 1.7 m faced away from the sun, in
main's build as in the branch's. That, and not the image, was what kept a conifer dark from below.

## What the literature says

Each row says whether its source was **read** this row, seen as a search **snippet**, read on
[conifer crowns](conifer-crown-coverage.md#what-a-bough-looks-like-and-what-its-needles-do-with-light)
(**read there**), or **recalled**; nothing is pinned to a recalled number without saying so.

| What | Source | What it says |
|---|---|---|
| A leaf's reflectance in the green | PROSPECT-D and PROSPECT-PRO as run by the WUR RTM suite's leaf-model tutorial (read: N 1.5, Cab 40, Car 8) | 0.134 at 550 nm |
| A leaf's two spectra | Jacquemoud and Ustin, *Leaf optical properties* (read in the second leg, a copy with a text layer: Boston University's course files) | chlorophyll's absorption covers the visible "with a minimum at 550 nm"; reflectance and transmittance plotted with one shape. No number in the text: the 550 nm values the first leg recalled (transmittance 0.08 to 0.12, reflectance 0.10 to 0.15) are replaced by the three rows below |
| Leaves and needles over the visible, by plant type | Majasalmi and Bright 2019, *Geoscientific Model Development* 12:3923, §3.1.1 (read; integrating-sphere data compiled for land-surface models) | mean reflectance and transmittance over 400 to 700 nm: needleleaf evergreen temperate 0.08 and 0.04, boreal 0.09 and 0.05; broadleaf deciduous temperate 0.08 and 0.06, boreal 0.09 and 0.05; broadleaf evergreen 0.11 and 0.06. Transmittance over reflectance: needles 0.50 to 0.56, broadleaves 0.55 to 0.75 (band means, where blue and red, transmitting least, weigh as much as the green) |
| A needle's two numbers at 560 nm | Hovi, Lukeš, Homolová, Juola and Rautiainen 2022, *Silva Fennica* 56(2) 10683, supplement S1, table S1.5 (read; Norway spruce, top of canopy, every campaign's mean) | current-year needles reflect 0.165 and transmit 0.085, one-year-old 0.139 and 0.059 (blue 0.069 and 0.025, red 0.063 and 0.027): transmittance 0.52 and 0.42 of reflectance in the green, 0.36 in the blue, 0.43 in the red |
| A needle transmits less than it reflects; a birch leaf about as much | Lukeš, Stenberg, Rautiainen, Mõttus and Vanhatalo 2013, *Remote Sensing Letters* 4(7):667 (abstract read) | "the transmittance of pine and spruce needles is clearly lower than reflectance", while for birch "reflectance and transmittance were similar" |
| A leaf's transmittance at 550 nm | Noda et al. 2013, *Plant, Cell and Environment* (snippet; the full text refused the fetch) | 0.06 to 0.09 at 550 nm in the rows the snippet shows, species not shown |
| A leaf's angular response | Bousquet, Lachérade, Jacquemoud and Moya 2005, *Remote Sensing of Environment* 98 (snippet) | the BRDF is a specular part at the surface, wavelength independent and driven by the cuticle's roughness, plus a **Lambertian, wavelength-dependent diffuse part**, which dominates integrated over the hemisphere; canopy models take leaves as Lambertian in reflection and transmission |
| How a thin surface transmits, in a shipped format | Khronos, `KHR_materials_diffuse_transmission` (read) | thin-walled; `base = mix(diffuse_brdf(baseColor), diffuse_btdf(diffuseTransmissionColor), diffuseTransmission)`, the factor default 0, the colour default white; the specular layer unchanged; a leaf is its example |
| A shoot scatters backward | Rautiainen et al. 2018, *Remote Sensing* 10:207 (read there) | Scots pine shoots "scatter most strongly in the backward directions with very little forward scattering in blue and red" |
| A spruce's needles stand all round the twig above, parted below | Gymnosperm Database, *Picea abies* (read there) | "above shoot directed forward, those below more or less pectinate" |
| The Lambert sphere | Lambert's phase law (recalled; derived below) | a Lambertian sphere returns (sin a + (pi - a) cos a) / pi of its zero-phase light at phase angle a |
| Leaves of spherically distributed normals in canopy transport | Ross 1981; Shultis and Myneni 1988 (recalled) | the bi-Lambertian leaf (a reflectance and a transmittance, both Lambertian) and the area scattering phase function of a spherical leaf-angle distribution, which the sphere's share below is for one visible layer |
| How shipped foliage cards are shaded | Unreal's Two Sided Foliage, *GPU Gems 3* ch. 16, SpeedTree (read there) | a subsurface (transmission) colour; a back-face term on a wrapped `dot(-N, L)`; leaf normals turned out from the crown |

## The model

**A sheet that transmits.** A broadleaf is a sheet: its front reflects as the BSDF always did, and
light on its far side comes through Lambertian, `tau / pi |n.l|`, where `tau` is the
transmittance. The engine states it as a share of the reflectance, `transmit = tau / rho`, so the
albedo an asset already has stays its reflectance and the transmitted colour is the albedo's (the
two spectra have one shape). glTF's factor `t` splits one diffuse in two, `(1 - t)` reflected and
`t` transmitted, so an imported material's share is `t / (1 - t)`, at most 1.

**A card of needles is a cloud of normals.** A needle is an opaque cylinder; a twig holds them all
round. Whatever way the card's plane faces, the eye sees the half of every needle that faces it,
so the card's normal stands for a distribution. For the share `spread` of the card that is a
sphere of normals, the light a covered pixel returns is the n.v-weighted mean, over the normals
the eye sees, of a Lambertian facet's cosine to the light:

    mean over visible n of max(n.l, 0), weighted by n.v  =  (2/3) phi(alpha)
    phi(a) = (sin a + (pi - a) cos a) / pi,   cos alpha = l.v

which is the Lambert sphere: two thirds of a sheet lit square on at zero phase, 1/pi of that at a
right angle, nothing straight against the light. The transmitted half, light on a facet's far
side, is the same law at `pi - alpha`. `foliage_reference_tests.cpp` holds both against a sum over
400,000 normals of a Fibonacci sphere, to 2e-4.

**The hemisphere.** For an ambient linear in the normal (the sky's harmonics to first order, the
stand-in's sky and ground halves), the mean over the visible half of a sphere of normals, weighted
by n.v, is the ambient at that mean normal, (2/3) v: (5/6) A(v) + (1/6) A(-v). A sheet that
transmits takes A(n) + transmit A(-n). The resolve evaluates the sky's ambient at the extra
directions and scales them by what the scene's cache let through at the front.

**The numbers chosen.**

| | transmit | spread | why |
|---|---|---|---|
| broadleaf leaf, frond | 0.8 | 0 | between the visible band's means (0.55 to 0.75, Majasalmi and Bright) and a birch's "similar" in the green (Lukeš et al.); the green, which carries most of a leaf's luminance, is where transmittance is nearest reflectance, and the band means are pulled down by blue and red |
| needle card | 0.47 | 0.8 | a spruce needle at 560 nm, 0.52 of its reflectance in its first year and 0.42 after (Hovi et al. 2022), the visible band's 0.50 to 0.56 (Majasalmi and Bright); a spruce's twig holds them all round above and parted below, so most of a card, not all, is a sphere of normals |

The four-bit codes put both at the nearest fifteenth (0.8 is 12, 0.47 is 7, 0.467). **What the
second leg moved**: the needle's transmittance, 0.2 (code 3) to 0.47 (code 7). The first leg had
it from a recalled "near zero"; the measured numbers are half the reflectance. The leaf's 0.8
stands, now on read sources. One finding beside it, not this row's to act on: the spruce recipe's
needle colour (green 0.100, linear, [conifer crowns](conifer-crown-coverage.md)) is under the
measured needle reflectance at 560 nm (0.139 to 0.165), and that page's recalled 0.08 to 0.12 is
too; a card's texels are needles only, so it is the needle's own number that applies. (G218
moved the recipe to it: [light within a conifer crown](conifer-crown-light.md#a-needles-colour).)

**What it costs.** One byte of the material's flags word, no word of any hot record. A foliage
pixel asks the sun in every direction but straight against it, so more of a crown traces its sun
ray (an underside used to skip it); its ambient evaluates the sky three more times.

## What it does to the engine's trees

**How it was measured.** The grove with a sky (latitude 46.5, day 200, 15:30) and `tree-species`
with the same sky, 1920 x 1080, `--raster rt --shadows rt`, bounce off, the eye at 1.7 m: views
at 10, 30, 100 and 300 m and from under a crown, an oak from three sides and from under it, a
beech. Before is the sprays' final build (98f62783, G203's fourth leg), after is this row's; each
view drawn shaded, as albedo and as the shadow view, and read over the tree's foliage pixels
(albedo green over 1.15 red) as G203's fourth leg read them: the picture's luminance, the
albedo's, and the shadow view's shares of sunlit, blocked and turned away. 2026-11-16 23:00 to
23:11, the session locked (both sides alike), other processes at 51 to 96 % of the CPU. These
first pictures drew the flat card colours (no generated pack in the materials root); the sprays'
pictures follow.

| view | foliage | picture Y before | after | change | turned away before | after |
|---|---|---|---|---|---|---|
| grove a, 10 m | tall spruce | 0.077 | 0.094 | +21 % | 0.66 | 0.02 |
| grove, under the spruce | tall spruce | 0.085 | 0.097 | +14 % | 0.73 | 0.00 |
| tree-species, 10 m | spruce | 0.090 | 0.099 | +10 % | 0.51 | 0.00 |
| tree-species, under | spruce | 0.089 | 0.097 | +9 % | 0.73 | 0.00 |
| tree-species, 10 m | pine | 0.103 | 0.101 | -1 % | 0.79 | 0.00 |
| tree-species, under | pine | 0.103 | 0.102 | 0 % | 0.84 | 0.00 |
| grove, three sides | oak | 0.151 to 0.160 | 0.226 to 0.231 | +45 to +51 % | 0.73 to 0.97 | 0.00 |
| grove, under | oak | 0.154 | 0.227 | +48 % | 0.91 | 0.00 |
| tree-species, 10 m, under | oak | 0.149, 0.150 | 0.208, 0.205 | +40 %, +37 % | 0.88 | 0.00 |
| tree-species, 10 m | beech | 0.150 | 0.224 | +49 % | 0.86 | 0.00 |

- **No foliage pixel faces away from the sun any more** where it faced it 51 to 97 % of the time:
  the sun is asked everywhere, and what answers it is the leaf through itself or the needles'
  sphere.
- **The broadleaves take the most**: a backlit leaf passes 0.8 of what a sunlit one reflects, and
  from under a crown most leaves are lit from behind. They read as backlit leaves do, a
  yellow-green glow against the light; the sunlit ones are unchanged.
- **The conifers take less, and the pine none on the mean**: with 0.8 of a card a sphere of
  normals, a card facing the sun gives up a third of its square-on light (the sphere returns two
  thirds of a sheet at zero phase) and a card facing away gains the sphere's share. The pine's
  mean is where it was; its needles are lit by where they are in the crown and the sun's
  direction, no longer by which way each card's plane faces. A spruce from under its crown stays
  dark against the light, which a shoot's backward scattering says it should be.
- **The far forms had none of it** in the first build (a crown at 30 to 300 m still had 50 to 67 %
  turned away): a tree's impostor and branch-card levels made a material of their own. They now
  take their foliage's optics (`flora.cpp`).

**The cost, flat cards** (the grove's own camera path, 3001 frames, 3840 x 2160, `--raster rt
--shadows rt`, the bounce on, before and after alternating in one lease, 2026-11-16 23:08 to
23:10, the session locked on both sides, other processes at 62 to 79 % of the CPU, free commit 27
to 36 GB): GPU total 4.591 and 4.592 ms before, 4.880 and 4.941 after, **+0.29 to +0.35 ms**, all
of it the resolve (2.290 and 2.299 against 2.583 and 2.571, +0.28): the sun ray an underside now
traces, through masked cards, and three more evaluations of the sky's ambient. The primary rays,
the bounce's trace and every other pass are unchanged to 0.01 ms.

**With the generated sprays** (the same views and pair, the materials root a pack this row's
`engine-content matgen` wrote, the far forms with their optics; 2026-11-16 23:2x to 23:35, locked,
other processes at 41 to 100 % of the CPU): the tall spruce at 10 m 0.083 to 0.093 (+11 %),
under it 0.086 to 0.094 (+9 %), the species' spruce +4 % at 10 m and +8 % under it, the pine
unchanged (0.094, 0.095), the oaks +37 to +51 % as on flat cards, and the oak at 300 m, now a far
form with its optics, 0.121 to 0.213 (+76 %); turned away 0.56 to 0.97 before, none after. The
pair: GPU total 4.990 and 5.024 ms before, 5.257 and 5.193 after, **+0.17 to +0.27 ms** (resolve
2.546 and 2.610 against 2.857 and 2.752); the second flight of the branch ran with the CPU at 100 %
and has a p99 of 115 ms, so this half is an upper bound. The sprays cost 0.41 ms over main's flat
cards on G203's pair; this row adds about half that again.

## Where the cost is, and what the second leg took back

**The split** (R164's second leg, 2026-11-17 00:14 to 00:58, the session locked on every flight,
other processes at 10 to 74 % of the CPU, free commit 49 to 69 GB; the grove's own path, 3001
frames, 3840 x 2160, `--raster rt --shadows rt`, the bounce on, the generated sprays pack; G is
G203's fourth leg (98f62783), B the first leg's tip, M main's build at 86b931f7; means of GPU
milliseconds, each side flown two or three times in one lease):

| | G203 (G) | first leg (B) | change |
|---|---:|---:|---:|
| grove, total | 4.858, 4.873, 4.857, 4.861 | 5.113, 5.109, 5.114, 5.128 | +0.25 |
| grove, resolve | 2.506, 2.517, 2.503, 2.508 | 2.762, 2.757, 2.762, 2.775 | +0.255 |
| grove, resolve without its rays (`renderer.resolve.stop_after=lighting`) | 1.722 | 1.772, 1.773 | +0.05 |
| rock-field (no foliage material), resolve, M / G / B | 1.462 to 1.464 / 1.463 | 1.488 to 1.489 | +0.026 |

So four fifths of the lighting's cost (0.20 of 0.25) is the sun rays a foliage pixel now asks
from the light's side of its card, which before it never asked: on a crown's shaded side, through
the rest of the crown. A twentieth is the shading itself; and a scene with no foliage material
paid 0.026 ms of resolve (1.8 %) for code it never runs.

**What was tried, each measured as a pair in the same lease:**

- **The sky's ambient in two evaluations instead of three** (`foliage_raw_ambient_pair`: the
  harmonics' even part is the same along `x` and `-x` and the odd part changes sign, so one
  evaluation answers both; the same values but for the order of the sums). No change past the
  noise (resolve 2.761 against 2.762).
- **Both of Lambert's phases from one `acos`** (`brdf_foliage_eval_cos`; phi(pi - a) is
  (sin a - a cos a) / pi), and none of it at a spread of zero. No change past the noise.
- **Statements for the BSDF choice**, where a conditional expression held a BSDF on each side. No
  change: the compiler had not evaluated both.
- **The half-rate sun pass for foliage** (`resolve_half_sun.slang`): it now asks a foliage block
  from the light's side and takes it whichever side the sun is on, where the first leg left every
  lit-through pixel to its own ray. At 11520 x 2160 on the grove (its sides at half rate by the
  default rule) the resolve was 4.585 and 4.632 before, 4.575 and 4.639 after: the sprays' cards
  rarely fill a 2 x 2 block with one pair, so few blocks are taken. It stays, because it is the
  rule R126 states and costs nothing, not because it saves.
- **The shares read where they are used, the front's ambient not held across `scene_ambient`.**
  No change (rock-field 1.488 and 1.487).

**Where the no-foliage scene's 0.026 goes** (2026-11-17 01:35 to 01:41, locked): a build of the
same tip with `foliage` forced false in `fs_resolve`, so the compiler drops every foliage line,
drew the rock field in 1.466 and 1.466 ms of resolve against main's 1.463 and 1.462 and the tip's
1.488 and 1.487. The foliage code costs a scene that never runs it by being there: registers, the
same mechanism the water and the stochastic materials met (renderer.md, "Stochastic mesh
materials"), which those answered with a resolve module of their own chosen per scene.

- **One GGX lobe for a foliage pixel** (`brdf_direct` for every pixel, foliage's diffuse added after
  it, `brdf_foliage_diffuse_cos`; the ambient's two halves one after the other). Rock field 1.493
  and 1.493 against main's 1.463 and 1.463; the grove +0.25 and +0.31 on G203 (02:08 to 02:11,
  locked, the CPU at 1 to 7 %). No change. The shape is kept: it is the same model with one copy
  of the lobe.

**Where this lands**: the lighting costs the grove +0.25 ms at 3840 x 2160 (0.20 of it rays) and a
scene with no foliage +0.026 to +0.030 ms. Nothing tried inside the kernel moved either.

**What would give it back** (not done, each a row of its own):
- *A scene with no foliage at main's cost*: a resolve module without the foliage lines, chosen per
  scene as `_stochastic` is (`GpuScene` marks a material row with the foliage byte at upload,
  `resolve_module` takes a fifth bit). The forced-off build above is that module's number. It
  doubles the resolve's modules from 16 to 32, a build-time cost the coordinator weighs.
- *The rays*: they are the light that is now there, and the shading's own cost is a twentieth.
  Fewer of them changes the picture, so it is the owner's lever to state, as R126 stated the
  sides': a foliage pixel lit through its card taking its 2 x 2 quad's answer at full-rate
  columns, or the faint-light rule (R115) given a tolerance a backlit needle actually falls under
  (the stated 2^-12 takes none of them). The half pass shows the one-pair block rule takes few
  spray blocks; a rule by material rather than by pair would take more.
- G203's sprays (0.41 ms over flat cards on its own pair, not part of this row's number): a
  needle set whose roughness is one value could carry it as a factor.
