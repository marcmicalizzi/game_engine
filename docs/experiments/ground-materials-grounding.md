# Ground materials by place: grounding

Status, 2026-10-24: the sources and numbers behind the mask level's generated priors and the
resolve's palette, height blend and wetness (roadmap W72; [renderer](../subsystems/renderer.md#ground-materials-by-place),
[tileset](../subsystems/tileset.md#the-mask-level)). The priors are **defaults an author starts
from**, never a geology the engine imposes: every number below is a field of
`engine.scene.TerrainMaskPriors` or `TerrainMasks`, and a region overrides any of them where it lies.

## Where the substrates lie

**Scree and talus at the angle of repose.** Talus below rock walls stands at about the angle of
repose of coarse angular debris: an Alpine survey of talus profiles found a break at a fairly
constant 33–34° and describes about 35° as typical of natural talus
([IRD, talus slopes](https://horizon.documentation.ird.fr/exl-doc/pleins_textes/divers19-08/010048761.pdf));
only actively fed or undercut screes are at the full angle, and most are shallower and concave
([scree](https://en.wikipedia.org/wiki/Scree)), the repose angle rising with angularity and grain
interlock ([angle of repose](https://en.wikipedia.org/wiki/Angle_of_repose)). **Chosen**:
`scree_deg` 32 with `slope_blend_deg` 3 — scree fading in from 29° and whole by 35°, the band's
lower edge at the concave foot rather than the repose angle itself.

**Exposed bedrock past a threshold slope.** In the San Gabriel Mountains (500 km² of 1 m lidar,
¹⁰Be erosion rates), bedrock exposure is limited below catchment-mean slopes of 30° and rises
strongly above it, with angle-of-repose debris alongside
([DiBiase, Heimsath and Whipple 2012](https://www.public.asu.edu/~aheimsat/publications/DiBiase%20et%20al%20ESPL%2012.pdf));
a 2023 compilation of 120 rates there couples morphology to erosion up to slopes near 50° through
progressive exposure of bare bedrock ([DiBiase et al. 2023](https://doaj.org/article/776cfb7a8ec44334bfaf34c8094a35c9)).
A catchment *mean* of 30° holds local slopes steeper than it, so the prior places the local
threshold above it. **Chosen**: `bedrock_deg` 42 (rock from 39°, whole by 45°), above the scree
band so the order down a slope is bedrock, scree, soil.

**Soil thinning with slope.** Soil depth falls with slope on hillslopes, which is why TOPMODEL
extensions make effective soil depth vary with topography
([Saulnier, Beven and Obled 1997](https://csdms.colorado.edu/wiki/Reference:Reference-001065)). The
prior does not carry a depth; it carries the consequence the eye sees — soil gives way to scree and
rock as the slope rises — through the two bands above. A region's `base` replaces the soil
(forest litter, alpine meadow) without moving the bands.

**Snow by elevation and aspect.** Shaded slopes hold snow lower: a northern-Alps report puts the
snowline about 300 m higher on sunny aspects than on shaded ones in the same week
([pistehors, January 2019](https://pistehors.com/bwEBN2gBGq0XVzdpaT4f/french-snow-conditions-10th-january-2019)),
the mechanism being the irradiance a slope's aspect receives ([snow line](https://en.wikipedia.org/wiki/Snow_line);
the Swiss observers' daily north/south snowline estimates in EnviDat are the systematic source to
take a seasonal figure from, not read here). Above about 60° new snow sluffs continuously and does
not build a pack ([Avalanche Canada, slope angle](https://avalanche.ca/glossary/terms/slope-angle);
*Snow Sense*, quoted by the [Utah Avalanche Center](https://utahavalanchecenter.org/blog/16386), with
the caveat that slabs do occur on steeper ground). **Chosen**: `snowline_m` (the scene's; the
default 100 km is no snow), `snow_aspect_m` 150 — half the reported difference either side of
the east/west line, a slope facing the pole by its share of facing it (`pole_x`, `pole_z`), fading
to nothing on the flat — `snow_blend_m` 60, and `snow_max_deg` 55 with the slope blend, so no
snow from 58°. The field report is one week of one season: it sets the order of magnitude, and a
scene sets its own.

**Shores and beds by energy.** Coarse deposits (sand, gravel) dominate the shallow, wave-exposed
zones where fine material is eroded and transported; fine cohesive mud accumulates in deeper and
sheltered water ([Håkanson 1977, as reviewed](https://kirj.ee/wp-content/plugins/kirj/pub/biol.ecol-3-2005-171-189_20230307164158.pdf);
Håkanson and Jansson, *Principles of Lake Sedimentology*, 1983, for the erosion, transport and
accumulation zones; the mud deposition boundary depth from wave theory is
[Rowan, Kalff and Rasmussen 1992](https://discovery-pp.ucl.ac.uk/id/eprint/10039393/1/Morales-Marin_et_al-2017-Limnology_and_Oceanography.pdf),
cited there). That is a function of fetch, depth and wind the authored world does not yet model,
so **it is the author's, per water body**: a region names the body and gives it a `bed` and a
`margin` — sand and gravel for an exposed pond, organic mud for a sheltered or swampy one — which
is exactly why two ponds at one height may differ. The prior the engine offers is wetness by
distance from the outline (`wet_m`).

## Terrain material practice

- **A palette, a few per tile, weights per texel.** Unreal's landscape keeps, per component, the
  list of layers painted there and their weights packed four to an RGBA weightmap, and a component's
  cost follows the layers active on it; layers a component never painted are skipped
  ([Unreal Engine landscape technical guide](https://docs.unrealengine.com/4.26/BuildingWorlds/Landscape/TechnicalGuide);
  [weightmap allocation](https://docs.unrealengine.com/4.26/API/Runtime/Landscape/FWeightmapLayerAllocationInfo/index.html)).
  A 5.8 regression where components inherited their neighbours' layers and overflowed the
  64-resource limit ([forum report](https://forums.unrealengine.com/t/landscape-edit-layers-5-8-regression-neighbor-input-area-widening-for-seam-fix-leaks-into-persisted-weightmaplayerallocations-inflating-srv-usage/2743253))
  is the failure the per-tile refusal here avoids: a tile names what its own points use, and its
  edge points are its neighbours'.
- **Height blend.** Opacity-weighted sums look smeared; blending by each layer's height plus its
  weight, keeping what lies within a depth of the highest, puts sand in the cracks and leaves stone
  tops clean (Mishkinis, "Advanced Terrain Texture Splatting", 2013,
  [gamedev.net](https://www.gamedev.net/tutorials/_/technical/graphics-programming-and-theory/advanced-terrain-texture-splatting-r3287/);
  [texture splatting](https://en.wikipedia.org/wiki/Texture_splatting)), with a depth of 0.2 the
  common default. **Chosen**: `height_blend` 0.2, applied only where a palette layer gives a height
  map; with none the weights are linear, as W68 drew.
- **Wetness as a modifier.** Wet surfaces are darker because light totally internally reflected
  at the water film is absorbed, strongly for porous, rough, mid-albedo ground such as sand (dry
  albedo 0.2–0.4) and little for very dark or bright ones
  ([MIT, wet materials, EGWR 1999](https://groups.csail.mit.edu/graphics/pubs/wet_materials_egwr99.pdf)).
  Lagarde's game model darkens the diffuse by `lerp(1, 0.2, porosity)` at full wetness — 0.2 from
  measured databases and Nakamae's 0.1–0.3 — and raises gloss towards water's by at most half
  that factor ([Lagarde, "Water drop 3b", 2013](https://seblagarde.wordpress.com/2013/04/14/water-drop-3b-physically-based-wet-surfaces/)).
  **Chosen**: exactly that, with roughness multiplied by `lerp(1, factor, 0.5 * wet)` — so a fully
  wet, fully porous substrate keeps 0.6 of its roughness and rock (porosity near 0) barely changes;
  wetness is the larger of the scene's (`wetness`) and the mask's, never a material of its own.
- **Texture arrays and virtual texturing** are the industry's answer to a palette of many maps;
  this engine's bindless set already names every layer's textures by slot, so a palette costs table
  entries and no array.

## Where the research and the budgets traded

The research says scree, rock and snow follow slope, height and aspect continuously; the per-tile
budget of four says at most four of them meet in a 32 m tile. The default bands are 6° wide and
the snow blend 60 m so the four roles rarely all meet with a region's material besides; when they
would, the build refuses the tile and names it rather than dropping one (W68's rule, kept). The
texel spacing (1 m) is the trail's and the shore's width, not the substrate's grain, which the
layers' textures carry at their own `tile_m`.
