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
