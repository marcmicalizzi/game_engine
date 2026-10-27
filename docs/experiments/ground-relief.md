# Ground relief

2026-10-26, R105. Research and implementation contract, written before implementation.
Status: in progress; no implementation, capture or performance acceptance is claimed yet.
Machine state: no measurement yet. The frozen forest report is shared-machine evidence,
not a timing baseline measured by this change.

## Sources read and limits

| Substrate | Evidence read | Numbers and proposed representation |
|---|---|---|
| Forest floor | [Uprooted Norway spruce study, 2017, Table 1](https://iforest.sisef.org/contents/?id=ifor1959-010), measurements of uprooted Norway spruce in southern Poland | Across the 1/3/5-year groups, mean pit lengths 2.74/2.26/2.18 m, widths 4.23/3.68/3.28 m, depths 0.95/0.65/0.52 m; organic mounds 0.33/0.32/0.38 m high. A paired pit and mound with a 0.30 m amplitude and metre-scale footprint is a deliberately shallow rendering approximation, not a fitted forest or an uprooting simulation. |
| Bedrock | [NPS joint description](https://www.nps.gov/places/geological-trail-station-4.htm), [Abe and Deckert 2021, section 4.2](https://se.copernicus.org/articles/12/2407/2021/), and [the existing rock contract](rock-structure.md) | Reuse the rock capability's authoring defaults: joint separation 0.65 m, width/depth 0.12/0.12 m; surface wavelength/amplitude 0.4/0.018 m. These dimensions are **authoring choices**, not measured geology. The paper's 0.1–1 cm scaling range and sandstone/limestone exponents 0.66/0.69 do not calibrate metre-scale terrain. |
| Talus | [Kirkby and Statham 1975, abstract](https://www.journals.uchicago.edu/doi/10.1086/628097), [Furbish et al. 2021, section 3.2](https://esurf.copernicus.org/articles/9/577/2021/), [existing slope priors](ground-materials-grounding.md) | Relative particle/bed roughness affects travel and sorting; the cited experiment uses a 35 degree slope. Retain the existing mask's 29–35 degree scree transition. Embedded angular caps are a bounded approximation, not settled stones or a prediction of downslope sorting. Size and exposed height remain author parameters. |
| Snow | [Fassnacht et al. 2009](https://sites.warnercnr.colostate.edu/wp-content/uploads/sites/74/2017/09/2009WRR45_W00D31Fassnacht_etal-Metrics_for_assessing_snow_surface_roughness_from_digital_imagery-2008WR006986.pdf) | Manual profiles at 1 cm and digital profiles at 0.5 mm show scale-dependent roughness; rough and smooth snow cannot share one universal amplitude. Declared only: no sastrugi dimensions or snow model pinned from these profiles. |
| Wind sand | [The existing ripple contract](../subsystems/terrain.md#how-real-ripples-behave-and-what-the-picture-takes-from-it) | Retain the existing 0.12 m wavelength / 0.008 m crest-to-trough defaults as shading. No geometric grain displacement. |
| Wave-washed sand | [Wiberg and Harris 1994, abstract](https://agupubs.onlinelibrary.wiley.com/doi/10.1029/93JC02726), [Lacy et al. 2007, section 3.3](https://agupubs.onlinelibrary.wiley.com/doi/full/10.1029/2006JC003942) | Oscillatory ripples depend on orbital diameter and grain size, unlike wind ripples. A beach must supply its own wavelength, height and more symmetric profile; wind celerity, lee exposure and grainflow are not a beach model. Beach parameter calibration remains open; no universal beach preset. |
| Soil, litter and duff | [Forest surface scope](forest-surface-quality-2026-10-26.md#game-side-authoring-that-can-proceed), forest measurements above | Declared only. A soil's material name does not imply tree throws everywhere. Litter/duff optical detail remains its authored maps; root plates, surface roots and woody debris need placed geometry with botanical identity and collision. No unverified thickness constant is pinned. |
| Gravel | Talus sources above | Declared only. Particle dimensions and exposure need a site or authored distribution; no universal gravel height. |

The requested Beatty and Stone (1986) and Šamonil, Král and Hort (2010) review were
located in bibliographies, but their full texts were not accessible in this read. No number
is attributed to them. The accessible primary spruce study supplies the forest dimensions;
they describe those plots, not all forest floors. The proposed feature density is an authoring
control, not inferred from mound diameter. Root plates and coarse woody debris are excluded
from a single-valued heightfield: it cannot represent their undercuts or spaces beneath logs.

## Sampling and budget trade

See [Relief](../subsystems/renderer.md#relief) for the contract. Feature width is distinct from
vertical amplitude. At 250 mm spacing a 1 m-wide feature can be fully represented by the
four-sample rule; a 0.12 m-wide joint cannot. Filtering must omit that joint, never widen it
to make it visible. Thus a 250/500 mm near option does **not** promise 2 cm lateral geometry.
Large uprooting pits already present in the source must not be added a second time.

For a square tile of side T and spacing s, source lattice vertices are `(T/s + 1)^2` and
triangles `2(T/s)^2`, before seam collapse or clustering. A 64 m tile is 16,641 vertices /
32,768 triangles at 500 mm, and 66,049 / 131,072 at 250 mm. Cluster-duplicated pool vertices,
reserved pairs, actual cut pairs and GPU milliseconds must be measured separately.

The frozen expanded-forest scene inspected for this work has 64 m world tiles, ring cells
128/64/32/16, hence spacings 500/1000/2000/4000 mm. Its source heightfield remains 1000 mm.
This differs from the night summary's statement that the finest drawn lattice is one metre.
The source scene and owner's views remain read-only; reproduction must rewrite the scene's
embedded `terrain.tiles.ddc` as well as the command-line cache to scratch.

## Acceptance still to run

CPU/GPU kernel agreement; negative and distant integer coordinates; spacing-filter bound;
same-level and mixed-level seams; collision lattice equality including prefetched builds;
absent-block hashes; raster/RT and CLAS/KHR equality; zero frame allocations; meadow and
ground-by-place captures; frozen forest views 5/1/8 and spawn at both requested resolutions.
Record per-pass timings, collision build time, pair/vertex counts and full machine state.
No result in this checklist is implied by the design.
