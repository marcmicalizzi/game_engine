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
| A leaf's two spectra | Jacquemoud and Ustin, *Leaf optical properties* (recalled; the PDF reached had no text layer) | reflectance and transmittance have one shape in the visible, a green peak between chlorophyll's blue and red absorption; a green leaf's transmittance at 550 nm about 0.08 to 0.12 against a reflectance of 0.10 to 0.15 |
| A leaf's angular response | Bousquet, Lachérade, Jacquemoud and Moya 2005, *Remote Sensing of Environment* 98 (snippet) | the BRDF is a specular part at the surface, wavelength independent and driven by the cuticle's roughness, plus a **Lambertian, wavelength-dependent diffuse part**, which dominates integrated over the hemisphere; canopy models take leaves as Lambertian in reflection and transmission |
| How a thin surface transmits, in a shipped format | Khronos, `KHR_materials_diffuse_transmission` (read) | thin-walled; `base = mix(diffuse_brdf(baseColor), diffuse_btdf(diffuseTransmissionColor), diffuseTransmission)`, the factor default 0, the colour default white; the specular layer unchanged; a leaf is its example |
| A needle transmits less than it reflects | Lukeš et al. 2013, Hovi et al. 2017 (read there) | pine and spruce needle transmittance "clearly lower than reflectance"; a spruce needle's visible transmittance near zero |
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
| broadleaf leaf, frond | 0.8 | 0 | a green leaf's transmittance near its reflectance at 550 nm (recalled 0.08 to 0.12 against 0.10 to 0.15), less in red and blue, which the albedo's colour already carries |
| needle card | 0.2 | 0.8 | needles transmit a fraction of what they reflect; a spruce's twig holds them all round above and parted below, so most of a card, not all, is a sphere of normals |

The four-bit codes put both at the nearest fifteenth (0.8 is 12, 0.2 is 3).

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
