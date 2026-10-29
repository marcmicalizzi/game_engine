# Wetland and waterside plants (G103)

Research read 2026-10-29 before the design. This is static content generation, not a
hydrological or plant-growth simulation. Dimensions below are representative points within
published envelopes, not population means. The same sedge has a low spreading and a raised
tussock form. No photographs or source assets are copied.

| Quantity | Source and observed range | Value used | Budget trade |
|---|---|---|---|
| Sedge leaf width and habit | [Native Plant Trust, Carex stricta](https://gobotany.nativeplanttrust.org/species/carex/stricta/): 4–6 mm; tussocks on seasonally flooded sites, spreading in drier sites | 5 mm; .7 m blade, .22 m tussock crown | Blade length/crown height are authored representatives; folded ribbons, three-sided culms |
| Rush culm | [Royal Botanic Gardens Sydney, Juncus effusus](https://plantnet.rbgsyd.nsw.gov.au/cgi-bin/NSWfl.pl?lvl=sp&name=Juncus~effusus&page=nswfl): 30–100 cm length, 1.4–4.3 mm diameter, pseudolateral inflorescence 2–7 cm | .75 m, 3 mm diameter; 4 cm lateral head | 3–6-sided tubes, clustered tiny flowers represented by branches |
| Reed culm, leaf, panicle | [Flora Zambesiaca account](https://www.botswanaflora.com/speciesdata/species.php?species_id=104020): culm .6–6 m, diameter to 1.5 cm; panicle 10–50 by 3.5–17 cm. [Native Plant Trust](https://gobotany.nativeplanttrust.org/species/phragmites/australis/): blades 15–40 cm by 20–40 mm | 2.4 m, 10 mm diameter; .3 m by .03 m leaves; .3 m panicle | Branched panicle, no individual hairs or florets |
| Cattail blade | [Native Plant Trust, Typha latifolia](https://gobotany.nativeplanttrust.org/species/typha/latifolia/): 5–29 mm width, terminal inflorescence | 18 mm blade; 1.8 m stem and 1.4 m leaf are authored reference dimensions | Strap leaves and cylindrical seed head; no individual seeds |
| Arrowhead blade and stalk | [Native Plant Trust, Sagittaria latifolia](https://gobotany.nativeplanttrust.org/species/sagittaria/latifolia/): blade to 305 mm, width 20–170 mm, petiole 65–510 mm; basal, sagittate | .22 by .12 m blade, .4 m petiole | Explicit basal lobes; flower omitted |
| White water-lily blade | [Native Plant Trust, Nymphaea odorata](https://gobotany.nativeplanttrust.org/species/nymphaea/odorata/): 50–400 mm, floating | .25 m blade with basal slit | Notched geometric fan; flowers omitted |
| Yellow pond-lily blade | [Native Plant Trust, Nuphar variegata](https://gobotany.nativeplanttrust.org/species/nuphar/variegata/): 70–350 by 50–250 mm, oblong/ovate floating leaves | .28 by .20 m | Elongated notched fan; flowers omitted |
| Zonation | [NRCS Technical Note 38](https://www.nrcs.usda.gov/plantmaterials/idpmctn380201.pdf), introduction: emergents occupy water-regime-specific shoreline zones | Explicit water elevation and depth band against supplied ground | No inferred hydrology; authors set site bands |

## Authored controls and missing evidence

No quantitative stand density, mortality distribution, patch autocorrelation or flexural
modulus dataset was established for these reference forms. Counts, crown radii, angles, seed
variation and dead/broken shares are explicit art controls, not claimed botanical constants.
Default dead fraction .18 and broken fraction .35 illustrate persistent dead material.
Stem heights vary by .8–1 of their representative; broken dead stems retain .35–.8 of it.
Leaf sizes vary by .85–1 of their representative, without scaling with patch extent.
The leaf taper shoulder is .65; curvature, leaf fold and arrangement approximate the
described habit. The .75–1.25 stature control is an author override, not a source range.

Root discs use deterministic angular/radial sampling; patches use a jittered lattice,
a smooth seeded field for recessed edges and gaps, and an explicit probability. These are
content distributions, not fitted colonisation models. Floating leaves are rigid at the
authored water surface: no wave following, buoyancy or seasonal water-level update.

Emergent motion reuses the field mechanism with a conservative .08-radian compliance
(.12 for narrow sedge leaves); these are authored bounded stand-ins, not measured stiffness.
Only the nearest cascade casts, in the rest pose, and traced shadows are omitted. Floating
leaves have no motion or collision. Every part has a flagless structural row.

## Engineering constants

Budgets are owner-tunable: sedge 6000, tussock 8000, rush 6000, reed/cattail 8000,
arrowhead 4000, water/pond lily 2000 source triangles. Counts default to 48/64/40/8/8/8/6/6.
The detail ladder reduces ribbon rows and tube sides without changing shoots or motion parts;
an insufficient floor is refused. A three-view baked far form uses the existing flora CPU
baker and cluster authored levels. Far forms and their transition error are measured separately.
Maximum 256 shoots and 100000 source triangles bound authoring memory and the u16 part contract.

Patch safety bounds (512 m radii, .125–4 m spacing, one million maximum candidates) are
work budgets, not ecological limits. The patch reports over-budget input. IDs enumerate
the finite candidate window and stay fixed across tiles, ring selection and request order.
Depth bands, slope, earlier footprints and an optional cooked palette weight gate candidates.
No mask means no inferred substrate; an explicitly requested missing mask is an error.

## Verification and measurements

The structural counts, capture review and shared-machine measurements below
record the implementation. Cross-toolchain verification is required before handover.

## Authored geometry constants, explicitly not measured

These are the bounded construction choices used in version 1. The botanical
sources above establish organs and dimension envelopes; they do not establish
these distribution or tessellation numbers.

| Construction | Values used | Why this bound |
|---|---|---|
| Root-disc radius | sedge/rush .13 m, tussock .24 m, reed .32 m, cattail .26 m, arrowhead .12 m, lilies .35 m | A readable clump with overlapping leaves; patch spacing is independent |
| Unmeasured stem radii | sedge .0015 m, arrowhead .003 m, lilies .003/.004 m; cattail .005 m | Small supporting stems within the leaf silhouette, not mechanical sections |
| Leaf fold / taper | ridge .16 of width, shoulder at .65 length; ribbon rows 6/4/2 | Retain a folded cross-section and visible taper under the small triangle budget |
| Sedge | three leaves at thirds of a turn; lean .25–.5, droop .26 live/.48 dead; terminal spike radius .002 m at .85–1 height | Radial tuft and triangular culm; seed spike size lies within Carex's key envelope |
| Rush | .12-height lean; head starts at .78 height, four branches .03 m radial/.04 m high, .0008 m radius | Pseudolateral silhouette without individual flowers |
| Reed | seven alternate leaves at .18–.83 of stem; lean .85, droop .32 live/.65 dead | Distichous spreading habit; bounded organ count |
| Reed plume | 12 branches, .3 m span, radial reach .006–.071 m, .0015 m radius; .035 by .012 m tip cards | Branching fits the cited panicle envelope without florets |
| Cattail | five basal leaves, golden-angle turn 2.399963229728653 rad, lean .28; head radius .018 m, .82–.97 stem height | Sparse readable straps and persistent seed head, dimensions authored |
| Arrowhead outline | six outline stations; total length .22 m and width .12 m before .85–1 variation; tilt .45 horizontal | Explicit sagittate lobes with six triangles rather than an alpha rectangle |
| Floating outline | 24/16/10 fan sectors; .32 rad basal gap; surface offset .002 m | Retain a true basal sinus and avoid coincident water surfaces |
| Tussock crown | radius 1.1 times root disc, starts .01 m below ground, roots anchored on an interior cone | Covers root bases with a simple organic mound |
| Stem taper | tip radius .7 of base; tip cap extends one tip radius | Reuses the companion tube kit; not a fitted taper model |
| Patch field | jitter .15–.85 cell; edge field 2 m scale, gap field 1 m; recession irregularity times .2–1 | Avoid regular rims and preserve local correlations across tiles |
| Slope / IDs | central difference at +/-250 mm; finite-window row IDs starting at 1 | Stable millimetre ground queries and no hash collisions |
| Far bake | three planes, 128 texels per view in 2 by 2 atlas; one-texel border; feature quarter of smallest extent, at least .02 m | Small resident images with actual coverage measured over the shared baker's views |
| Fallback material | live (.075,.22,.045), dead (.32,.22,.105), roughness .7; alpha cutoff .5 | Explicit placeholder colours until a pack supplies measured/textured material |

Engineering caps and author ranges are validation limits, not claims about
ecological maxima. The recipe's stature and spread may intentionally move a
plant outside the source's representative dimensions; defaults are the grounded
reference forms. No old flora geometry or identity pin is intentionally changed.


The version 3 MSVC debug run (2026-10-29) produced these deterministic sizes.
Triangles include the six authored far triangles; clusters count all DAG levels,
so they are the pair allocation per instance before distance-form extraction.

| Kind | Source + far triangles | All DAG clusters | Budget |
|---|---:|---:|---:|
| sedge | 4038 | 123 | 6000 |
| tussock | 5646 | 167 | 8000 |
| rush | 2130 | 68 | 6000 |
| reed | 2438 | 84 | 8000 |
| cattail | 1174 | 36 | 8000 |
| arrowhead | 198 | 7 | 4000 |
| water_lily | 258 | 9 | 2000 |
| pond_lily | 258 | 9 | 2000 |

Each whole-stand far cut is one cluster with six triangles. These are structural
counts, independent of machine load. They do not measure frame time or justify
filling the host's entire pair space with near containers. Recipes with other
counts, seeds or budgets must be measured independently.


### CPU authoring measurements

MSVC debug, Intel Core i9-10980XE (36 logical CPUs), 2026-10-29;
`engine_wetland_bench --wait-quiet=30 --repeats=3 --min-time=60`.
No previous wetland solver exists for a before/after timing comparison.

| Work | Median per solve/tile |
|---|---:|
| sedge | 3.295 ms |
| tussock | 4.326 ms |
| rush | 3.223 ms |
| reed | 2.493 ms |
| cattail | .849 ms |
| arrowhead | .290 ms |
| water_lily | .302 ms |
| pond_lily | .302 ms |
| tile (quarter of 32 m radius patch, .5 m spacing, flat ground) | 1.236 ms |

These are **upper bounds on a busy debug build**, not production costs.
Recorded `machine_state`: start CPU others/total 35.179%, own 0%; GPU 9%,
5333/32607 MiB; session unlocked; GPU lock held by another engine-view.
End CPU others/total 28.646%, own 0%; GPU 3%, 5257/32607 MiB; no GPU lock.
The quiet wait expired and the harness explicitly marked the result busy.
Raw JSONL and text are under
`D:/workspace/game_engine_local/wetland/2026-10-29/cpu-bench.*`.
The solve measurement excludes cluster construction and atlas baking.


### Tussock anchor correction (version 2)

The first pictures exposed root anchors above the faceted crown. Version 2 seats
roots on an interior cone, below every crown tessellation, and keeps the same
anchors on every budget rung. A vertical-ray test against the actual crown
triangles verifies all 64 roots at three budgets. The version bump invalidates
all wetland source/container keys; only tussock positions and motion pivots
change. Earlier flora species and their pins remain unchanged.


### Thin-organ LOD correction (version 3)

The first 20 m coarse-versus-finest picture lost reed blades and turned sedges
into compact blocks before the far cards. Whole-frame FLIP mean .01287 and
SSIM .97310 concealed that local loss because much of the frame was empty.
Generic decimation is now capped at 1 micrometre and followed by an authored
complete-source rung, then the measured six-triangle card rung. This retains
organ count/shape near and bounds the distant cut. A selector test counts every
source triangle before the card transition and exactly six afterwards. The
duplicate near rung costs storage; all-level cluster counts above include it.
Version 3 invalidates all wetland keys and container pins; geometry and motion
stay version 2. No existing flora pin changes.
### Conservative card threshold (version 4)

Version 3 still switched small sedge clumps to visibly blocky cards at 20 m.
Version 4 floors the coverage error at the stand's half-extent. At the default
five-pixel foliage threshold this delays the three-plane representation until
the stand is approximately ten pixels across its largest dimension. This is an
authored quality bound, not a botanical constant. It changes all wetland
source/container pins, but no source geometry or motion. The full-source and
six-triangle selector checks pass. Prelanding pictures are retained under
`prelanding-v1/` and `prelanding-v3/` in the local output directory.

The groundless pond study must use `--water-quality low`: high did not show
a usable water surface in this setup. The low tier hides submerged stalks and
shows the floating blades at .502 m. Arrowhead's anchor is raised to .35 m for
a shallow-water example. This does not change the water capability. Ground
fitting, wave following and artistic acceptance remain outside this implementation.


### Handoff status (2026-10-29)

Version 4 passes the MSVC debug wetland unit and benchmark tests. Documentation,
lint/license and formatting checks pass. The affected Windows run was stopped
after tests 1–85 passed, during flora, at the standing brief's call budget.
Linux validation and version 4 captures remain pending after shared-resource
queues; this is not a merge-ready result. The latest captures/paired timings
are version 3; final version 4 visual and performance acceptance is outstanding.
The local handoff records exact commands and remaining work. Roadmap G103 stays open.

