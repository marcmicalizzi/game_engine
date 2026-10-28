# Forest surface quality: content work and engine gaps

2026-10-26. Proposed research and acceptance scope, requested by the owner after a marked forest POC run. No engine implementation or rendering behavior changes accompany this page.

## Evidence and limits

The external project is `D:/workspace/forest_wildlife_poc`. The owner marked nine viewpoints in `reports/lab/20261026-105859-524`, using engine `816513e7`, hardware raster, CSM, HDR10 at 2560 x 1440, time 15. The scene, recording, report and hashes remain there. Controlled diagnostic scenes and cameras are under `reports/quality-v15/surface-audit`; the source scene is retained unchanged. These are shared-machine appearance checks, not quiet-certified performance measurements. The eventual performance acceptance includes the owner's real-window 11520 x 2160 target.

Three views (tags 1, 5 and 8) compare the original materials against terrain roughness maps removed with factor one, and separately terrain normal strength zero. Wetness remains unchanged: the roughness-one input does not guarantee an output of one on wet ground. The comparisons reduce broad ground sheen when roughness is raised; removing normals smooths the response rather than repairing it. This establishes material sensitivity, not a proof of a renderer defect or physically correct roughness. Do not ship these diagnostic overrides as a fix. EXRs retain HDR light; fixed-curve JPEG reviews do not reproduce the HDR display.

The current contracts in [renderer](../subsystems/renderer.md#native-authored-terrain-materials) and [ground materials by place](../subsystems/renderer.md#ground-materials-by-place) establish:

- Palette layers already provide colour, normal, roughness, height blending and wetness, selected by painted masks, regions, slope, elevation and aspect. Four layers per mask tile is an explicit budget, not four for the world.
- The forest scene already carries sand, snow, soil, gravel, litter, rock and pine-duff materials. Their presence does not establish that their masks, scales or appearance are good.
- Terrain height maps control substrate blending; they do not displace geometry or collision. A normal map changes shading, not silhouette, occlusion or contact.
- Fine colour, normal and roughness fade to layer means. The POC sets 32 to 128 m. Native layers and legacy procedural sand detail are mutually exclusive, so the desert's existing detail cannot simply be switched on for the forest beach.
- The current indirect term is a sky/ground hemisphere, rather than the scene-resolved transport sought by R69. Improving content does not supply canopy occlusion or reflected local surroundings.

## Grounding

[USFS Grass Lake](https://research.fs.usda.gov/psw/rnas/locations/grass-lake) describes strong habitat contrasts near Tahoe: heavily shaded red-fir stands with sparse understory, open rocky pine slopes, and dense shrub patches in hollows. [NRCS Loamy Moist Outwash](https://edit.sc.egov.usda.gov/catalogs/esd/022A/F022AE025CA) describes greater understory diversity on moist, fertile valley soils. These support habitat-specific patches, not a uniform dense fern or grass carpet. They do not prescribe polygon counts or a universal cover percentage for this fantasy region.

[Heitz and Neyret, HPG 2018](https://eheitzresearch.wordpress.com/722-2/) describe example-based stochastic textures with preserved appearance. [Deliot and Heitz, GPU Zen 2](https://eheitzresearch.wordpress.com/738-2/) specifically address coarse-mip colour drift and compressed textures. These motivate R96's research; no algorithm or constants are selected yet. Multi-map PBR correspondence and normal orientation must be established separately, not assumed from a colour-only example.

[Olano and Baker, LEAN Mapping](https://userpages.cs.umbc.edu/olano/papers/lean/) demonstrates why unresolved bump distributions affect specular response. It motivates R97's acceptance requirement, not adoption of its Ward-based model into this engine's GGX path. A GGX-compatible method must be evaluated against the reference renderer and measured on supported hardware.

## Proposed order and acceptance

### First: diagnose shine, then improve scale continuity

Keep the owner-marked baseline. Validate source roughness convention, normal orientation and physical texture scale, wetness masks, material blending, sun/sky contribution and SDR/HDR output on isolated dry/wet substrate fixtures. A broad highlight by itself is not proof of an error. R97 should preserve unresolved normal variation in the material response rather than merely increasing detail distances or globally raising roughness. Walk from close range through and beyond the existing fade distances, at grazing and overhead angles, in daylight and campfire lighting. Require stable highlight shape, no distance bands or shimmer, and quantified reference error and pass cost.

**R97's finding (2026-10-27; [normal filtering](normal-filtering.md)).** Built normal maps now carry
a normal-variance chain that widens the GGX roughness by what the pixel's footprint no longer
resolves, and a tiled layer with one keeps its normal and roughness past the detail distances; on
by default. On the v18 forest at 2560×1440 the ground's mean sun-specular energy falls 15.2% at tag 1
(beach, 0.01837 → 0.01558), 8.1% at tag 8 (backshore, 0.01027 → 0.00944) and **2.2% at tag 5**
(mountain, 0.02949 → 0.02884, the marked camera lifted 3 m because v17's relief buried it), for
+0.10 to +0.26 ms of resolve and 83 MB of textures. The mountain's broad sheen is therefore not
unresolved normal-map variation: it is the rock layer's own roughness on ground smooth between the
heightfield's vertices. What would change it is relief at the decimetre-to-metre scale (R98),
substrate detail within the layered path (R99) and the masks' choice of layer and roughness
(content), not more filtering.

### R96: nonrepeating coordinated terrain materials

Keep authored large-scale substrate patches distinct from fine texture sampling. Evaluate a bounded, opt-in stochastic or equivalent nonperiodic sampler for suitable substrates, with identical spatial choices for colour, normal, roughness and height; rotate/reorient normals correctly. Preserve physical grain/leaf scale, mip stability, regional colour identity, integer-world anchoring and tile/ring seams. A road, strata or directed needle bed must be able to retain intentional direction. No random RGB tint or unbounded layer count as the acceptance substitute. Report sample count, texture residency and resolve cost at ordinary and surround resolutions.

### R99: substrate detail inside the layered terrain path

Allow a sand layer to carry appropriate grain and ripple detail while adjacent rock, litter and snow retain their own behavior, rather than choosing between all terrain layers and the legacy sand-detail block. Research wave-washed beach detail separately from wind-driven desert ripples. Snow needs its own grounded surface model; a white colour alone is insufficient. Blend by material coverage and pixel footprint, preserve shared shading across water-bottom, raster, ray-shadow and reference paths, and make absent detail cost absent. Static appearance is independent of W39/W41's tracks and deformation.

### R98: surface relief, where shading alone cannot suffice

Establish a budgeted relief contract for centimetre-to-decimetre substrate shape: choose which scales use mesh/heightfield geometry, which use a view-dependent shading approximation if justified, and which remain normal detail. State silhouette, self-occlusion, shadow, RT and collision behavior for every tier. A shading-only technique must not claim to move the walker's support. Research before choosing displacement/parallax machinery or constants. Validate rocks embedded in soil, litter over roots, sand transitions and snow edges at eye level, including LOD/ring crossings. Preserve repros of capacity failures rather than silently removing relief or content.

## Existing roadmap work to refine, not duplicate

| Existing item | Forest acceptance case / missing scope |
|---|---|
| R21, R90 | Coherent layered materials and host cover: moss/soil on rocks and roots, without coplanar stand-in sheets or independent incompatible PBR patterns |
| R22 | Backlit and shaded leaves with plausible transmission; do not compensate by painting brighter leaf colour |
| R69, R71, R72 | Canopy-restricted sky contribution, local bounce and reflections, stable under movement; shaded ponds reflect their surroundings. Stage baseline-tier visibility/transport explicitly during design rather than assuming RT is available |
| R92, G87 | Distant and elevated-view crowns maintain coverage and species shape. First separate a coarse LOD from a tree outside the game's 1280 m instance residency radius |
| G77, G90 | Habitat-based understory and coverage ladders; no persistent grass trails are implied by existing contact bending. Confirm noncolliding grass with diagonal walking |
| W25, W47 | Bounded material-page residency and neighbourhood-first loading as the authored region grows |
| W39, W41 | Persistent footprints/trails and snow deformation after static material appearance; keep physics support and rendering consistent |
| S25, S63, S86 | Dynamic deadwood interaction, water movement/swimming/buoyancy where their APIs are still absent; independent from static ground realism |

## Game-side authoring that can proceed

Use existing native masks to distribute dull mineral soil, decomposed litter, localized intact leaf accumulations, conifer duff, gravel and sand. Derive patches from actual stands, routes, slope and moisture; do not stack every substrate on every tile. Audit the four-layer budget before adding a new paint channel. Preserve coordinated maps and source provenance; albedo must not carry fabricated shadows or highlights.

Add grounded roots, broken twigs, branch fragments, small stones and patchy plants at sizes requiring silhouettes. Root form belongs to tree/asset authoring; merely placing a trunk deeper does not create roots. Existing shrubs, ferns, herbs, cover and deadwood assets allow an initial pass, with quality and solver gaps recorded individually. Keep paths and dry exposed slopes readable, preserve patch boundaries across world cells, and measure density/LOD costs. More objects cannot fix an incorrect BRDF, and a richer texture cannot supply missing silhouettes.

The first authoring comparison should cover one forest-floor patch, one beach transition and one mountain patch with fixed near/mid/far cameras before applying the recipes throughout the region. This page adds roadmap scope; it does not certify a new playable POC version.
