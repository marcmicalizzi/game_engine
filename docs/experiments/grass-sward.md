# Grass sward: research and acceptance (G90)

Reserved on 2026-10-25 for the sward solver: grass as ground coverage, with blade
patches near the eye, clump cards beyond them, and the ground material far away.
This is new work; G75's isolated tufts remain complete and unchanged.

The solver builds on W72's ground masks and W73's streamed placements. Field sway
and caster policies are separately briefed engine work. The owner confirmed on
2026-10-25 that existing cooked paint and distance forms cover the other seams.

Research, implementation and acceptance are in progress. Measurements below distinguish
the passing reference patch from the unfinished family. The owner's visual acceptance remains required.

## Dense cover and management states (continuation, 2026-10-25)

The owner prioritizes dense grass to walk through, tall stands, short trodden/mown/grazed cover,
and sparse clumps, with AAA-quality assets and distance representations independently of future
AO, atmosphere and bloom. The continuation changes the unit of construction from an isolated
leaf to a basal shoot with several leaves, then gathers shoots into crowns. Oregon State's
[Bunch and Sod-forming grasses](https://forages.oregonstate.edu/regrowth/how-does-grass-grow/grass-types/bunch-and-sod-forming-grasses)
distinguishes crown-bound tillers from lateral sod growth; its examples include ryegrass and
cocksfoot as bunch grasses and bentgrasses as stoloniferous. Blair, Nippert and Briggs,
[Grassland Ecology (2014), morphology, pp. 398–401](https://www.k-state.edu/ecophyslab/pdf%27s/Grassland%20Ecology_Springer_Reference_series.pdf),
describe shoots as repeated sheath/blade units and explain protected basal growth after defoliation.
These sources justify grouping and clipping, not the authored sample counts or crown radius.

Implementation v5 uses 1–8 rendered leaves per shoot, 1–32 shoots per crown, a 0–1 gathering
parameter and a 0–0.25 m crown radius. Those ranges are authoring controls, not botanical constants.
Alternating leaf directions form a fan around a common root. Boundary correction moves whole crowns, preserving shared roots. Roots stay inside their cells; foliage overhang is included in actual mesh bounds. The near/coarse rungs share every shoot and taper shoulder; the
coarse rung reduces curve tessellation instead of replacing unrelated leaves with wider blades.
Cutting truncates the axis at a horizontal height and keeps the blade's remaining width, rather
than shortening a pointed leaf. Variable cutting height represents uneven grazing; it is an
appearance model, not a grazing ecology simulation. Species dimensions still bound uncut blades.

The dense candidate uses 1024 tall-fescue blades/m², growth range 0.45–1, and an explicit 8500
triangle/m² all-rung cap (5120 near + 3072 coarse + at most 96 cards = 8288). This is a high-quality
candidate, not a new silently enlarged default or a measured frame-budget allowance. The 2400
kind default remains; the low-cost candidate uses 384 blades and two segments (2400 all-rung).
Mown/grazed candidates budget the extra triangles at cut ends explicitly. Rendered leaf area
and projected coverage must be reported separately from biological shoot density.

Persistent walked tracks need runtime state beyond wind: a spatial bend-direction/flattening
field sampled at world roots, stamped by swept contacts, with decay/recovery and tile lifetime
rules. High graphics quality may retain that visual field; low quality must allocate none.
Long-term wear (shorter/sparser grass) is a separate authoring/gameplay mask, not the same as a
recovering bend. Existing sections 35/36 already supply roots, lengths and bend weights; section
41 remains exactly AO/thickness/hue/age. No extra section or CPU object per blade is proposed.
The engine-side field contract needs contact/history support before a persistent-track demo can
be claimed. The current branch's runtime does not implement that history.

The ComfyUI material lab remains available for appearance comparisons. First establish geometric
coverage and management silhouettes; then feed any selected leaf appearance consistently into
near material, cards and ground with provenance. A photographed/generated whole-sward tile must
not be pasted onto each individual blade. No new generated material is claimed in this step.
## Sources and chosen bounds

Leaf dimensions are leaf dimensions, **not culm height**. Growth interpolates within them;
it is an authored growth state, not calendar phenology. Species-specific seasonal colour,
straw fraction and seed production were not established by these sources.

| Quantity | Source and location | Value used | Budget trade |
|---|---|---|---|
| Tall fescue blade | [USFS FEIS, botanical description](https://research.fs.usda.gov/feis/species-reviews/scharu) | 5–70 cm long, 3–13 mm wide | Sample leaves, not whole plants |
| Perennial ryegrass blade | [Kew GrassBase description](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:407493-1/general-information) | 3–20 cm, 2–6 mm | GrassBase paragraph; the Flora Zambesiaca width in the same page conflicts |
| Timothy blade | [Kew GrassBase](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:415868-1/general-information) | 4–20 cm, 3–9 mm; spiciform panicle | Never use culm height as leaf length |
| Cocksfoot blade | [Kew GrassBase](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:397309-1/general-information) | 10–45 cm, 2–14 mm; fascicled panicle | Folded leaf approximated as ribbon |
| Wood melick blade | [Kew GrassBase](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:408184-1/general-information) | 5–20 cm, 3–7 mm; open panicle | Woodland profile; density unknown |
| Wavy hairgrass blade | [Kew GrassBase, Avenella flexuosa](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:391947-1/general-information) | 5–20 cm, 0.3–0.8 mm; filiform involute | Rolled leaf approximated as ribbon; the old tuft's 4 mm is not this species' width |
| Bent blade | [Flora of China, Agrostis capillaris](https://www.efloras.org/florataxon.aspx?flora_id=2&taxon_id=242301747) | 2–15 cm, 1–4 mm; open panicle | Short growth state, not simulated mowing |
| Red fescue blade | [Kew GrassBase](https://powo.science.kew.org/taxon/urn:lsid:ipni.org:names:30010036-2/general-information) | 3–40 cm, 0.5–1 mm (exceptionally 2 mm) | Common width range |
| Tufted hair-grass blade (G137) | [Kew GrassBase, *Deschampsia cespitosa*](https://static1.kew.org/data/grasses-db/www/imp02819.htm) | 10–60 cm, 2–5 mm; "caespitose; clumped densely", culms 20–200 cm | The tall tussock's profile, `tufted_hairgrass` |
| Mat-grass blade (G137) | [Kew GrassBase, *Nardus stricta*](https://static1.kew.org/data/grasses-db/www/imp06577.htm) | 4–30 cm, 0.5–1 mm, filiform; caespitose, culms 25–60 cm | The short tussock's profile, `mat_grass`; a filiform leaf approximated as a ribbon |
| Growth forms (G137) | Briske and Derner 1998, "Clonal biology of caespitose grasses", in Cheplick (ed.), *Population Biology of Grasses*, Cambridge UP | Caespitose (bunch, tussock) grasses hold their tillers in discrete crowns; rhizomatous and stoloniferous ones spread them as a sod | The two habits as layouts; not read in full here, cited for the classification |
| Forb share (G137) | [Natural England lowland grassland SSSI condition sheet, 2024 draft](https://www.contractsfinder.service.gov.uk/Notice/Attachment/04c2b009-14eb-4859-9e4f-2eee2bf491e7); UKHab's "other neutral grassland" criterion as summarised by a [habitat guide](https://intercom.help/joes-blooms/en/articles/9138437-habitat-type-other-neutral-grassland); [Natural England, MG5](https://publications.naturalengland.org.uk/file/5017566310039552) | Herbs 40–90 % of the sward for a species-rich calcareous grassland (CG2); more than 20 % cover of broadleaved herbs and sedges for "other neutral grassland"; MG5's grasses usually a substantial share, exceptionally 95 % | 0.33 of a hay meadow's cells, 0.1 an improved pasture's: a count of cells standing for cover |
| Field tiller density | [Praat 1995 thesis pp.150–151, PDF 167–168](https://mro.massey.ac.nz/server/api/core/bitstreams/0fbaf226-da07-47c6-9511-ecc60c49fc9e/content) | Ryegrass 5815–7414/m²; tall fescue 3080–3093/m² at 217 days after sowing | Treatment estimates, not universal defaults; other profiles use zero for unknown |
| Grassland LAI | [Scurlock, Asner & Gower 2001, Table 6 p.19](https://info.ornl.gov/sites/publications/Files/Pub57077.pdf), precursor to [Asner et al. 2003](https://doi.org/10.1046/j.1466-822X.2003.00026.x) | Filtered mean 1.71, range 0.29–5.0 | Report mesh one-sided area separately; full 2003 table not retrieved |
| Wind loading | [de Langre 2008 pp.145,159](https://yakari.polytechnique.fr/Django-pub/documents/delangre2008rp-1pp.pdf) | Small-deflection drag scales with speed squared; reconfiguration reduces it | No species EI found; CPU compliance is compatibility, not measured stiffness |
| Canopy frequency | [Py et al. 2006 Table 1 p.430](https://yakari.polytechnique.fr/Django-pub/documents/py2006rp-1pp.pdf) | Wheat 2.50 Hz, alfalfa 1.05 Hz at about 0.68–0.69 m | Neither is a universal grass frequency |
| Coherent gusts | [Finnigan 2000 abstract](https://www.annualreviews.org/content/journals/10.1146/annurev.fluid.32.1.519) | Canopy-scale coherent sweeps | Spatial wind field; full text unavailable |

Watson & Dallwitz's site could not be retrieved. Species-level Kew and Flora of China descriptions
replace it. Praat's field estimates are distinct from controlled-box ryegrass experiments with
much higher tiller counts. Seed-head habits are recorded; reproductive geometry is not yet built.

## Growth forms, the stature rule and the meadow's forbs (G137, 2026-11-08)

The owner (2026-11-08): not all grass is thick and tall — some is clumpy and tall, some short and
thick, some clumpy and thick — and a natural field carries other meadow plants besides grass. The
sources above give the two habits (Briske and Derner 1998): **sod** formers (red fescue, bent,
smooth meadow-grass) spread their tillers by rhizomes and stolons into a continuous turf, and
**caespitose** grasses (tufted hair-grass, mat-grass, cocksfoot, purple moor-grass) hold them in
discrete crowns with ground between. Crossed with stature that is four forms, each a recipe in
[`content/test-scenes/meadow/`](../../content/test-scenes/meadow/README.md):

| Form | Recipe | Profile (blade, source) | Leaves used | Habit as built |
|---|---|---|---|---|
| tall sod | `tall-sod` | red fescue, 3–40 cm × 0.5–1 mm (Kew) | 22–40 cm (`growth_min` 0.55) | 768 blades/m², shoots of 3 in crowns of 4, dispersed (`clumpiness` 0.1) |
| tall tussock | `tall-tussock` | tufted hair-grass, 10–60 cm × 2–5 mm (Kew) | 30–60 cm (`growth_min` 0.4) | crowns 0.5 m apart, 0.12 m radius, a 5 cm raised base; 30 % short turf between (10–17.5 cm) |
| short sod | `short-sod` | bent, 2–15 cm × 1–4 mm (Flora of China) | 4–8.5 cm (`growth` 0.15–0.5) | 1,024 blades/m², two-segment ribbons |
| short tussock | `short-tussock` | mat-grass, 4–30 cm × 0.5–1 mm (Kew) | 12–27 cm (`growth` 0.3–0.9) | crowns 0.25 m apart, 0.05 m radius; 35 % turf between (4–9 cm) |

Every blade is inside its profile's source range (the forms tests hold it); nothing is scaled.
**What is authored and not measured:** the crown spacings (0.5 m and 0.25 m), radii and the 5 cm
raised base. No field measurement of tussock spacing or base height was found for either species
(Davy 1980's *Biological Flora* account of *D. cespitosa*, J. Ecol. 68:1075–1096, likely has one
and was not retrievable); the values are chosen so a tussock reads as a crown with ground around it
at 1 m cells, and are the first numbers to replace when a source turns up.

**The stature rule** (`sward_site_form`, `SwardStature`): per cell, two scores — tall against
short, tussock against sod — each a bias plus weighted site terms plus a value noise; positive
chooses tall (tussock). The site terms and why:

| Term | Field | Default weight (tall, tussock) | Why |
|---|---|---|---|
| wetness | the mask's wet band (`wet_m` around a water body), or a stream's nearness over the riparian reach | +1.5, +1.5 | wet margins carry tall tussocks (tufted hair-grass, purple moor-grass, rushes) |
| steepness | smoothstep of the slope from 15° to 30° | −2.0, +0.3 | steep ground has thin, dry soil and carries short turf, mat-grass tussocks among it |
| shade | a palette layer's weight (`shade_layer`, none by default) | +0.5, −1.0 | woodland grasses are lax and loose, not tussocks; the woodland profile stays its own |
| mosaic | value noise on a 20 m lattice, −1..1 | 1, 1 | meadows are mosaics at 10–30 m (a stated choice, not a fit) |
| bias | — | 0, −0.35 | a level, dry, open meadow is half tall and half short, and mostly sod |
| band (G138) | `band_m`, metres | 3 m, both | the forms grade over a few metres rather than meeting on a line ("Where two forms meet", below) |

The weights and thresholds are design choices with stated defaults, not fits: the direction of each
term is the ecology's, the size is chosen so that a fully wet cell (1.5) outvotes the mosaic (at
most 1) and a fully steep one (−2) outvotes both.

**The meadow's forbs.** A share of the cells that hold a patch also stand a herb from the
understory's forms — the oxeye daisy (*Leucanthemum vulgare*, an MG5 hay-meadow forb; stems 20–90 cm
across the floras, 0.5 m here, `meadow/oxeye-daisy.flora.json`). The share defaults to **0.33**, a
species-rich hay meadow's; **0.1** is an improved pasture's. The sources give cover, not counts: the
condition sheets put herbs at 40–90 % of a species-rich sward and "other neutral grassland" above
20 %, and improved grassland below it; one forb a cell is a count standing for cover, and a 0.6 m
daisy covers far less than its cell, so the share understates the forbs' cover on purpose until a
forb of meadow cover exists. The herb's motion kind is `parts`: every forb is a run of the frame's
part-pose table and a CPU pose per part each frame, so a tile keeps at most `forb_tile_cap` (256 by
default, the ones of lowest hash) and the forbs stand in the nearest ring only. A `field` herb, posed
on the GPU like the sward, is the request that lifts both limits (flora.md, "Not yet").
**On the forest proof of concept's v36 (2026-11-09, `msvc-release`, `--raster hw --shadows rt`, the
batch's engine-only flags, four held views of 12 frames each, the GPU median of the last six).** The
before scene is v36's own sward entry (one form, tall fescue at 1,024 blades/m² in four density
classes); the after scene gives the entry the four forms at the same four classes (two variants each,
cooked by `engine-content flora`), the oxeye daisy at a 0.33 share and `forb_tile_cap` 1,024. Both on
one binary (the no-forms placement is eeb8077c's to the bit, so the binary is the pairing); every row
is an upper bound: the harness recorded other processes at 14–19 % of the CPU at 1080p, and at 4K
56 % of the CPU and **82 % of the GPU busy before** (another client outside the lock), 24 % and 0 %
after, so the 4K pair is not a cost.

| View | 1080p GPU ms before → after | 4K GPU ms before → after | Instances before → after (1080p) | Visible pairs before → after (1080p) |
|---|---|---|---|---|
| grass-clearing | 10.36 → 10.38 | 16.50 → 17.24 | 181,813 → 184,181 | 943,737 → 934,326 |
| forest-ground | 10.77 → 11.20 | 24.71 → 23.75 | 142,783 → 144,573 | 292,736 → 292,709 |
| open-ground | 12.38 → 13.37 | 22.01 → 22.57 | 122,618 → 127,777 | 757,905 → 713,734 |
| shore-fringe | 13.41 → 13.69 | 23.03 → 24.20 | 160,559 → 166,841 | 793,222 → 861,405 |

The scene's pairs stay at the placements' pair budget (16.7 million, 33 tile withholdings in both
runs), so the forms' fewer triangles per patch (3,840, 3,200, 3,072 and 2,560 near triangles at full
density against tall fescue's 5,120) do not show as fewer pairs here; the instances grow by the forbs
(1,800 to 6,300 in the near ring). Where the meadow is in view (open-ground, 1080p) the rasterizer
falls 0.26 ms (2.29 → 1.94 and 1.88 → 1.62: the short forms), and the resolve (+0.3 ms), the
deformed-vertex pool (+0.08 to +0.18 ms: the forbs' parts) and the ray tracing chain (+0.05 to
+0.4 ms) rise. A forb is visible at 1080p as a white speck: the daisy's 2 cm rays on a 0.5 m stem read
as a meadow only close up, which is the forb's shape, not the share.
## Where two forms meet: the transition band (G138, 2026-11-09)

The forest proof of concept's v37 (engine-needs item 27): the stature rule picked one form per cell
by the sign of a score, so where the 20 m mosaic crossed zero a tall-tussock patch met short sod along
a sharp curve, and "real meadows do grade at that scale over a few metres"; the request was a band in
which the score's magnitude near zero mixes the two forms by the cell's hash, "so the boundary dithers
over 2–4 m instead of being a contour".

**What the literature gives, read on 2026-11-09.** No source reached gives the width of the boundary
between a tall patch and a short one directly; what they give is the scale of the height mosaic itself,
which bounds it, and the shape a boundary is described by.

| Quantity | Source and location | Value taken | How it is used |
|---|---|---|---|
| Short and tall patches as one mosaic | Bakker, de Leeuw and van Wieren 1984, "Micro-patterns in grassland vegetation created and sustained by sheep-grazing", *Vegetatio* 55:153–161 ([doi](https://doi.org/10.1007/BF00045017)); the abstract as a web search quoted it (the indexing page, [Conservation Evidence](https://www.conservationevidence.com/individual-study/708), refused a direct read, 403) | "short, heavily grazed areas alternated with taller, lightly grazed patches, both having the same species composition", the micro-pattern "more or less stable"; recorded in permanent 2 × 2 m quadrats. No patch size or boundary width | The two statures are one sward's states side by side, not two communities with a gap: the boundary mixes them rather than separating them |
| The height mosaic's scale | Olofsson, de Mazancourt and Crawley 2008, "Spatial heterogeneity and plant species richness at different spatial scales under rabbit grazing", *Oecologia* 156:825–834 ([doi](https://doi.org/10.1007/s00442-008-1038-6)); abstract, as indexed by [AGRIS](https://agris.fao.org/search/fr/records/65dec49b0f3e94b9e5d11598) | mean patch size (range) of vegetation height in grazed grassland 2.1 ± 0.3 m; of grazing 2.4 ± 0.5 m, of droppings 3.7 ± 0.6 m | Height decorrelates within about 2 m, so a tall patch cannot fade into a short one over much more than one or two such ranges: 2–4 m |
| Boundaries as sigmoids | Hufkens, Scheunders and Ceulemans 2009, "Ecotones in vegetation ecology: methodologies and definitions revisited", *Ecological Research* 24:977–986, and the same group's sigmoid-wave method (*Ecological Informatics* 3:97–104, 2008); known only from a web search's summary of their abstracts, not read | a transect across a boundary is described by a fitted sigmoid, its width by the fit | The band's shape: the share of one form falls along a transect as a sigmoid (here a smoothstep, a sigmoid with compact support) |

The default is **3 m** from 95 % of one form to 5 % of it along a transect: the middle of the request's
2–4 m, and one and a half of Olofsson's 2.1 m height ranges. It is a stated choice consistent with
those sources, **not a measured boundary width**. Still to be read, with what each might give: Cid and
Brizuela 1998, *J. Range Manage.* 51:644–649 (tall-fescue patches under cattle; the full text was
refused, 403); the 2020 *Agronomy* 10:716 study of vegetation-height semivariograms under cattle and
sheep (ranges of 1–2.5 m by a search's summary; the page was refused, 403); the 1991 *Grass and Forage
Science* sward-stick study (variance at scales under 1–2 m by a search's summary; refused, 403);
McNaughton 1984, *Am. Nat.* 124 (grazing lawns), Adler, Raff and Lauenroth 2001, *Oecologia* 128,
van der Maarel 1990, *J. Veg. Sci.* 1, Fortin and others 2000, *Landscape Ecology* 15, Cadenasso and
others 2003, *BioScience* 53, and Herben and others 1993, *J. Veg. Sci.* 4, none reached in full.

**The shape is a dither of individuals.** Bakker's patches share their species, so the transition is
which state a patch of turf is in, not a mixture of densities or an intermediate height: each cell
keeps one whole form (its own cooked families), and near the boundary the form is drawn with the
probability the sigmoid gives at the cell's distance from the boundary. A gradient of height would
need forms between the four, which are not built.

**What the band does, measured (2026-11-09, `msvc-debug`, the forms tests).** `SwardStature::band_m`
3 m by default; the rule reads a cell's distance from the boundary as score over the mosaic noise's
gradient and draws the form by the cell's hash on a smoothstep that reaches 5 % and 95 % at half the
band to each side (flora.md, "Where two forms meet"). Along transects through 12 worlds of one 64 m
tile each, every patch put at its distance found by bisection on the score itself:

| Band asked | Tall against short, 95 % to 5 % | Tussock against sod, 95 % to 5 % |
|---|---|---|
| 3 m (default) | 3.04 m | 3.02 m |
| 0 (G137's line) | 0.225 m (one 0.25 m bin) | 0.225 m |

The band is the asked width where the mosaic draws the boundary; where a site field moves the score
too, it is narrower by the ratio of the noise's gradient to the whole (flora.md has the reason).

## The floor under a sward (G141, 2026-11-10)

The owner (2026-11-10): between sparse crowns the meadow's ground "reads low-poly and too shiny".
The cause was the `--sward-assets` bake: blade lines on one flat green, `0.35 + 0.65 cover` times the
blade's own colour, at a constant roughness of 0.70 — a floor that was the blade, drawn larger than the
blades above it. This section grounds what the bake now composes the blades over
([flora](../subsystems/flora.md#the-ground-under-the-sward-g141) has the rules).

**What the drawn sward covers.** Measured on the meadow's own recipes at 256 px a metre, with the bake's
16 samples a texel: the short-tussock recipe (mat-grass, 512 blades/m²) covers **2.3 %** of the ground
from straight above and the short-sod recipe (bent, 1,024 blades/m²) **6.2 %**; their drawn leaf area
index (one-sided leaf area over the cell, `TreeReport::leaf_area_index`) is a few hundredths. A
measured grassland's is about a hundred times that: **1.7 ± 1.2** across grassland sites (Scurlock,
Asner and Gower 2001, *Worldwide historical estimates of leaf area index, 1932–2000*, ORNL/TM-2001/268,
the grassland biome's mean). So from above the drawn sward is mostly its floor, and the floor is what
a person looks at between crowns: it has to be a floor.

**What a floor is made of, by form.** Ground-cover surveys of grassland classify each point of a
pin-point transect as live vegetation, litter or bare ground (the Canterbury and Waimakariri tussock
resurveys, Landcare Research, in the *New Zealand Journal of Ecology*'s tussock-grassland series). In a
short-tussock exclosure bare ground fell to about **5 %** within five years while the grazed ground
outside stayed near **15 %**, litter abundant inside and sparse outside, and the same grassland held
about **17 %** bare ground in 1948 (*New Zealand Journal of Ecology*, 1999, the exclosure study, as its
abstract reports it). Managed European grasslands show the same direction: heavily grazed short turf
opens bare soil and moss, lightly grazed tall stands accumulate litter (the grazed-and-cut bryophyte
comparison found with the same search). No survey found gives the three shares of the *floor* (the
ground that is not a live leaf) for each of the sward's four forms, so the shares below are **design
choices in the surveys' direction**: a tussock's floor shows soil between crowns and more of it where
grazed; a sod is a closed mat of thatch and moss with little soil; wet and shaded ground favours moss.

| Form | Thatch | Moss | Soil | Canopy LAI (design) |
|---|---|---|---|---|
| tall sod (red fescue, rank) | 0.70 | 0.25 | 0.05 | 4.0 |
| tall tussock (tufted hair-grass, lightly grazed) | 0.72 | 0.10 | 0.18 | 3.0 |
| short sod (bent, grazed lawn) | 0.40 | 0.52 | 0.08 | 2.0 |
| short tussock (mat-grass, grazed) | 0.50 | 0.20 | 0.30 | 1.5 |

Wetness moves half its value of the soil's share and three tenths of the thatch's to moss; shade three
tenths of the soil's and two tenths of the thatch's (`sward_floor_mix`). The canopy LAIs sit inside the
grassland mean plus two deviations, taller and closed swards higher.

**Their reflectance against the blade.** In the visible, plant litter and soil cannot be told apart by
reflectance alone — only the 2.1 µm cellulose-lignin absorption separates them, and it fades as litter
ages (Nagler, Daughtry and Goward 2000, *Remote Sensing of Environment* 71: 207–215). Standing litter
has a disproportionate effect on a grassland canopy's reflectance, and leaf area and leaf angle are its
main controls (Asner 1998, *Remote Sensing of Environment* 64: 234–253). So the floor's parts take
linear albedos inside the visible ranges those papers' spectra span, as **stated design values**:
thatch (decaying, on the ground, not standing straw) **(0.20, 0.165, 0.10)**, rising to the red;
moss **(0.045, 0.075, 0.025)**; a moist organic loam **(0.085, 0.065, 0.045)**. The blade keeps its
material's (0.09, 0.22, 0.06). Roughness: thatch 0.80 (± 0.03 grain), moss 0.95, soil 0.92, the blade
its 0.70 — the floor is the matte part of a meadow, which is what "too shiny" asked for.

**Why the floor is darker than the blades near the eye.** In the reference reels the ground between
grass crowns is dark: r01's grazed turf on the ridge and its tall flowering meadow, r24's tussock bank
and turf mounds, and the `grass-in-wind` frames' foreground clump (the reels under
`game_engine_local\inspiration\_frames\`) show shadowed litter and soil between thin lit blades, not a
lit floor. The floor under a canopy receives the light the canopy lets through, Beer and Lambert's
`exp(−k LAI)` (Monsi and Saeki 1953), with `k = 0.5` for a spherical leaf-angle distribution's
extinction (Campbell and Norman 1998, *An Introduction to Environmental Biophysics*, ch. 15). The
drawn blades cast almost none of it (they are never traced, and the cascades shadow only the nearest
metres), so the bake carries it: the near colour is the floor and the low leaves times
`exp(−0.5 · canopy LAI)`, floored at 0.25 — 0.47 for a short tussock, 0.37 for a short sod.

**The far colour is the drawn sward seen at a low elevation.** Past the sward's reach the layer stands
in for the canopy, so it is what the near sward looks like there: the drawn leaf area's cover along a
10° view, `1 − exp(−0.5 LAI_drawn / sin 10°)` (12.7 % for the short tussock, 30 % for the short sod),
over the top view of blades and shaded floor. Measured on the bake (the ground tests):

| Recipe | Cover from above | Cover at 10° | Transmission | Near mean (linear) | Far mean (linear) |
|---|---|---|---|---|---|
| short tussock (`d100-v0`) | 0.023 | 0.127 | 0.47 | (0.047, 0.055, 0.025) | (0.063, 0.075, 0.034) |
| short sod | 0.062 | 0.302 | 0.37 | darker than its far (test) | greener than red (test) |

**The match, pinned in one capture pair** (2026-11-10, `msvc-debug` engine-view, RTX 5090, 1280 × 720,
`--shadows csm --shadow-distance 8 --time-of-day 11`, the short-tussock cook with two variants; the
machine shared with other agents' builds, which does not move a picture). One camera at 3.5 m looking
at the ground 20 m ahead (about 10° of elevation): the meadow as drawn, its ground at its near colour
(within `detail_near_m`), against the same scene with no sward placed and the layer at its far colour
everywhere (`detail_near_m` 0, `detail_far_m` 0.01). Mean linear colour of the ground below the
centre, left of the trail (columns 160–560, rows 420–720): drawn **(0.0733, 0.0830, 0.0475)**, luma
**0.0784**; far layer **(0.0864, 0.0937, 0.0539)**, luma **0.0893**. The far colour is **14 % brighter**
than the sward it stands for and the same hue within 5 % (green over red 1.08 against 1.13); the
bound pinned is **15 %** in luma. The far is brighter because it takes the blade at its albedo where
the drawn blade is shaded by its own lean and the cascade, and because it assumes 10° where most of
the measured rows are seen steeper, under less blade.

**Floor, as drawn.** The first floor (a 3 cm Worley mosaic of the three parts with dark crevices) read
as camouflage polygons from a walker's eye; it was replaced before this commit by dead-leaf fragments
over a soft moss-and-soil underlay (flora.md has the layout), which reads as a litter mat.

**What this does not settle.** The far colour is the *drawn* sward's, so a field drawn at a hundredth of
a measured leaf area reads as its floor from afar too — browner and darker than the reels' far meadows,
which are green because the real canopy covers the floor. That is the blade count's to fix (solver
needs 2.3 item 5), not the floor's. The floor shares and albedos are stated design values, not fits to
a plot survey of these forms; a photographed or scanned floor (a pack's `ground/thatch`, `ground/moss`
and `ground/soil` roles, which the bake already reads where a pack has them) would replace the
procedural parts.

## Rendering model and explicit approximations

[Jahrmann & Wimmer 2017 §§3–5](https://www.cg.tuwien.ac.at/research/publications/2017/JAHRMANN-2017-RRTG/JAHRMANN-2017-RRTG-draft.pdf)
uses curved blades and spatially grouped patches. [Wohllaib 2021 slides 24,28–30](https://archive.thedatadungeon.com/ghost_of_tsushima_2020/documents/gdc_2021/gdc_2021_procedural_grass_in_got.pdf)
uses Bézier curves and their derivatives, with fewer vertices at distance. Our 2–3 segments and
256 blades/m² are budget choices, not their counts or botanical densities. The rest axis turns
with authored horizontal curvature coefficient 0.15–0.55 and ±0.2 radians twist; arc normalization preserves leaf
length. This is not an EI beam solution. A whole blade translates inward to stay in its cell.

[Sanders, The Vegetation of Horizon Zero Dawn, GDC **2018**, slides 23–30,58,83](https://media.gdcvault.com/gdc2018/presentations/gilbert_sanders_between_tech_and.pdf)
documents distance animation, terrain colour data and distinct visual/shadow geometry. The 2017
procedural-placement talk is different. These motivate colour continuity and reduced shadow
work, not this engine's performance or its nearest-cascade policy.

Historical v4 ladder: 256 blades/m² × 5 triangles near, half as many × 5 coarse with width scaled by
paired nominal leaf area, sixteen clumps/m² × three quads (96 triangles). Default 2400
triangles/m² covers **all levels** (2016 at full density). Up to sixteen variants use independent
deterministic streams. Pairing adjacent headings, keeping the longer axis, avoids the directional
bias of discarding even lattice indices. Card tiles are 128 pixels with one-pixel margins;
the existing baker supersamples 4×4. The cutoff is explicit, default 0.5. These are budget knobs.

AO (base 0.35 to tip 1), thickness (0.5), hue (0.4–0.6), green RGB (0.09,0.22,0.06), roughness
(0.7), and dry share (0.1) are uncalibrated appearance inputs. A gradient is not measured patch
sky occlusion. Main's resolve does not yet read the hue bytes. The ground uses the same colour.

## Coverage against elevation (G136)

2026-11-08, `measure_sward_coverage_at` (the four-azimuth probe of `measure_sward_coverage` at
another elevation; 256-pixel orthographic frames fit to the patch, the card at its fitted cutoff),
the full-density reference patch (`solve_sward` defaults, seed 8801), the mean of the four azimuths,
`sward_tests.cpp`, "the card covers what the near form does at 10, 30 and 60 degrees":

| Elevation | Near form | Card | Card / near |
|---|---|---|---|
| 10° | 0.0797 | 0.0723 | 0.907 |
| 30° (the cook's fit) | 0.0493 | 0.0489 | 0.992 |
| 60° | 0.0389 | 0.0372 | 0.957 |

**What it decides.** The card the cook fits at 30° under-covers the near form by 9% at a grazing
10° and by 4% at 60°: its two vertical cards a bake group are sparse across a grazing view and its
top card edge-on. That is a tenth of the patch's own coverage, not bare ground, so the proposed
view-angle term in the shared mask function (a cutoff lowered as the view grazes) is **not built**:
it would buy at most those 9%, change the alpha test every path and the opacity micromaps share, and
the hand-off by distance (renderer.md, "The hand-off by distance") already puts the card past 21 m
alone, where a patch is a few pixels. The test holds the ratio within 10% at the three elevations.
What the measurement does not cover: the v36 scene's sparse families (`d25`, `d50`), whose cards
the "sparse-family card coverage" gap above names, and the card's look — flat, with no parallax —
which a coverage fraction does not see. If the owner's "flat paint" past the edge persists with the
hand-off in place, those are the next things to measure.

## Confirmed wire contract (owner, 2026-10-25)

One parentless part per blade in section 35. Section 36 stores **linear arc fraction**, 0..65535;
the engine applies the bend shape. Phase derives from part index and instance seed on the GPU.
Section **41**, no other new section, stores u8 unorm ao, thickness, hue, age. Section 37 has no
collider or structural flags. One `sward` catalogue entry covers all botanical profiles.

Caster policy is engine-side: no traced structures, one nearest cascade, rest pose. Current-main
measurements must label `--shadows csm --shadow-distance` as a stand-in. Density is a linear R
paint image through `TerrainMaskPaint`; placements read the same cooked mask. The entry's far
ends at ring 1's floor, with no placements in outer rings. Runtime paint and far-ground wind
sheen remain future work.

## Historical v4 measurements and unresolved acceptance, 2026-10-25

The CPU coverage probe rasterizes near geometry, coarse geometry and masked cards into the same
256² orthographic frame, at four cardinal azimuths and 30° elevation, with 4×4 samples per pixel.
These are geometric measurements, not timing claims. Ryegrass seed 8801, variant 0:

| Density | Coarse relative difference, four views (%) | Cards relative difference, four views (%) |
|---|---|---|
| 0.25 | -1.25, +0.98, +2.03, -0.53 | +9.16, +10.35, +5.95, +6.39 |
| 0.50 | -3.56, +0.47, -0.78, -0.88 | +6.72, +6.23, +8.03, +6.49 |
| 0.75 | -0.45, +2.05, -0.11, -0.69 | +8.85, +2.51, +8.35, +6.42 |
| 1.00 | -0.78, +0.09, -0.63, -0.23 | +3.79, +3.13, +4.74, +4.77 |

The first implementation used two-segment, double-width coarse ribbons and 64-pixel cards:
full-density coverage fell 4–13% and 31–33% respectively. Retaining taper resolution, pairing
headings, and fixing the card UV inset improved this. Lowering the 128-pixel card cutoff to 0.35
overshot by about 40%; it is not the fix. Sparse cards and all other species still need acceptance.
The full-density reference's near coverage is about 7.3% of this projected frame; it is not LAI.
The first eye-level capture looks sparse and repetitive, and is not accepted as a convincing sward.

The complete eight-variant family (32 patches, four densities) then measured these worst absolute
differences across its four views. This broader result supersedes any inference from variant 0:

| Density | Coarse worst (%) | Cards worst (%) | Full container clusters | Card-only form clusters |
|---|---|---|---|---|
| 0.25 | 3.85 | 20.67 | 26 | 3 |
| 0.50 | 3.56 | 13.65 | 46 | 3 |
| 0.75 | 2.95 | 9.27 | 64–67 | 3 |
| 1.00 | 2.01 | 9.79 | 86–87 | 3 |

At a completely occupied 16×16 m tile this is at most 22272 resident source pairs in ring 0,
768 in ring 1 and zero in later rings. These are container-pair bounds, not visible draw counts.
The card forms are now explicitly selected and validated as cards only; the ordinary one-pixel
20 m cut retained 19 clusters at full density and did not meet the representation contract.
Per-frame deformed bytes and the shadow delta still need the frame measurements, not these bounds.

A read-only POC check of `world/v10/understory.json` in the 64 m square x=[-250,-186),
z=[-480,-416), around its grass-clearing camera, found 3624 grass tufts, all marked clearing,
2707 occupied 1 m cells and 976 occupied 2 m cells. Light proxy min/mean/max was
0.0672 / 0.985495 / 1. This counts occupied source cells; it does not claim the new fixture
recreates the POC's density at every texel. No POC file was changed.

The initial 1600×900 offscreen diagnostic used main's CPU motion, CSM and shadow distance 8 m,
while compilation was active. It reported 227161 tail pairs of 16777216 (1.35%), 3486 flora
instances, 1703424 posed parts and 654365808 tail bytes. Its CPU pose mean was 8.33 ms, but
`machine_state` was null and GPU timings were zero: this is **not a quiet performance result**,
nor does tail storage equal visible deformed-pool bytes. It demonstrates why field sway remains
necessary. This v1 diagnostic predates the heading-paired ladder and guaranteed card-only form.

Development pins: section 41 moved from 11938167440745581753 (v1) to 660343445336747083
(v2, three-segment coarse ribbons) to 17839441404926445449 (v3, heading pairing). The eight-variant
geometry/attribute family pin was 3702055350956951526. Version 4 also fixes card roughness to
match blades/ground; the geometry pins remain v3's. Existing kinds retain their old bake defaults.

## Historical v4 static diagnostics and verification

The completed 32-patch meadow was flown at sky hours 12 and 18, wind strength zero, with
CSM and shadow distance 8 m. The sky record reports sun elevations 64.42° and 14.98°;
use `sun.sky`, not the legacy non-sky orbit fields. Eight marker images near 1/5/20/80 m
are in the external `sward/2026-10-25/meadow/captures-sun12` and `captures-sun18` folders.
The near ribbons remain sparse and the far handover is visibly discontinuous; these are
acceptance candidates, not accepted pictures. No 3/8 m/s or travelling-gust sequence is claimed.

Diagnostic stationary 0.5 s path, two repeats, 120 warmup frames and the harness's minimum warmup,
Windows `msvc-debug`, 60 s quiet wait, GPU lock held. GPU medians in milliseconds:

| Resolution | Shadows | Total | Shadow pass | Deform pass | Harness quiet | Other CPU start/end (%) |
|---|---|---|---|---|---|---|
| 1600×900 | off | 0.927 | 0 | 0.489 | false | 8.28 / 11.46 |
| 1600×900 | CSM, 8 m | 1.236 | 0.330 | 0.423 | false | 8.85 / 6.27 |
| 11520×2160 | off | 5.588 | 0 | 0.727 | true | 8.16 / 8.00 |
| 11520×2160 | CSM, 8 m | 8.261 | 2.199 | 0.819 | false | 9.72 / 12.17 |

These include the fixture's trees and terrain. They compare shadows off/on, not grass absent/present,
and the busy CSM runs are upper bounds, not isolated sward costs. In particular, 2.199 ms does not
establish the requested <1 ms shadow increment. The engine's nearest-cascade/rest-pose policy
is still absent. No frame-budget baseline is set from these debug diagnostics.

All four runs had 229125 tail pairs plus 5841 prefix pairs (1.40% of 16777216); visible pairs were
11034 at 1600×900 and 18062 at 11520×2160. CSM shadow pairs were 6114 and 39835 respectively.
CPU motion reported 1703601 parts for 3489 flora instances, and 654388384 tail bytes. The public
flythrough record does not expose actual deformed-pool bytes; tail storage is not that measurement.
Its `frame_loop.last_frame=-1` also means this stationary diagnostic is not the requested meadow
allocation proof with moving tiles and blowing wind. Full machine-state records and raw frames
are retained with each external `bench-*.jsonl`.

Validation: `msvc-debug test -Affected` passed all 122 selected tests (4968.58 s on the shared
machine), including lint/docs, renderer and the existing frame-loop paths. Linux Clang passed
all 25 selected tests (522.59 s), including both new deterministic pins; its two worktree volumes
were pruned. Clang required explicit f32-to-f64 promotions; those preserve the original arithmetic.
The final Windows follow-up rebuilt successfully and passed flora (247.70 s) after that portability-only correction; both deterministic pins remain unchanged.

The walking-height review also calls for layered fans: the authored outer-leaf fraction now
raises horizontal curvature from the inner leaf's 0.2–0.55 toward 0.9–1.25 and vertical droop
from 0 to 0.65 in the normalized quadratic axis. Arc length is still normalized to the source
range. These are appearance choices, not measured species mechanics. Cutting retains the first
upward crossing so an arched tip beyond the cut is removed. Coarse width fitting is limited to
0.75–1.25 of natural width and never modifies near blades. Near/card captures remain necessary:
four-view covered area alone did not reveal the earlier cell seams.

## Engine hand-back for walked tracks

The CPU compatibility fix makes structurally non-woody `sward` roots pushable, including part
zero. It changes no tree-root policy and allocates nothing new per frame. Its existing 12-tick
recovery is useful for contact verification but too short for the requested visible walked path.

The field-kind implementation should treat these as three independent concerns:

- Immediate contact: stamp swept actor capsules/feet in world coordinates and sample the result
  at each blade root. Bend by the section-36 arc weight, keep roots fixed, and apply the same
  deformation to every position reader through the deformed-vertex pool. No per-blade CPU state.
- High-quality visual history: retain direction and flattening in optional tiled storage, with
  authored recovery time and bounded spatial/temporal lifetime. Clear/reinitialize newly mapped
  tiles, handle origin movement and teleport explicitly, and test departure/re-entry. Low quality
  allocates no history field and schedules no history update pass. This is a proposed interface,
  not an implemented allocation or a sourced biological recovery constant.
- Lasting wear: authored/gameplay management state selects shorter/sparser grass and matching
  ground paint. It must not disappear when graphics quality changes. Mown, grazed and worn recipes
  now provide distinct asset states; the transient bend field must not silently rewrite them.

The root cell may have leaf overhang; use actual mesh bounds and do not shrink those bounds to
the placement footprint. Section 41 and the motion sections are unchanged. No new container
section is requested. Final field/caster integration and a terrain walking replay remain open.

The v5 isolated captures deliberately omit terrain, wind and shadows. Dense cover now fills
most of the walking-height picture; the old cell-edge seams are absent. Card forms still
flatten the canopy from elevated angles, even where covered area matches. Mown/grazed outputs
correctly distinguish clipped and uneven tips but remain sparse as standalone lawn geometry
(modeled LAI approximately 0.27/0.30 in these authored samples). A dense short-grass surface
needs further representation work within the draw/streaming budgets; simply claiming the
covered-area fit as visual acceptance would hide both defects. The external comparison page
switches identical near/card cameras among close, walking height and overview.

## Engine hand-back: field sway (R95)

**Done on the engine side, 2026-10-25** (roadmap R95; [renderer: the field kind](../subsystems/renderer.md#the-field-kind),
[the caster policy](../subsystems/renderer.md#the-caster-policy)). The `sward` catalogue row is
`FloraMotionKind::field` with the caster policy `{traced: false, cascades: 1, rest_pose: true}`.
The pool pass bends each blade about its root from a per-frame block (wind, gust field, flutter
clock, walker) and the blade table; no CPU state, no pose table run, no tail record. The walker's
push is the immediate-contact term of the list above, in the same pass with no history: walked-track
history and lasting wear stay open.

**The travelling gust, grounded.** The mixing-layer analogy for plant canopies puts the coherent
canopy-top eddies' streamwise spacing at about eight shear lengths, `Λx ≈ 8 Ls` with
`Ls = U(h)/U'(h)`, and their convection at about `1.8 U(h)`
([Raupach, Finnigan & Brunet 1996](https://doi.org/10.1007/BF00120941); [Finnigan 2000](https://www.annualreviews.org/content/journals/10.1146/annurev.fluid.32.1.519);
the constants as applied in [a study of wind over emergent wetland vegetation, arXiv:1601.07960](https://arxiv.org/abs/1601.07960),
which also derives `Ls ≈ h/α` with `α ≈ 2.8` for an exponential canopy profile and measures
`Ls = 0.58 m` at `h = 1.75 m`). Chosen numbers, and where they were traded:

| Quantity | Value | From |
|---|---|---|
| honami wavelength | 1.4 m | `8 h/2.8` at the owner's knee height, `h = 0.5 m` |
| honami speed | 1.8 m/s | `1.8 U(h)` with `U(h) = 1 m/s` on a mean day: an assumption, not a measurement |
| sweep wavelength | 57 m | the same rule for a 20 m forest edge upwind of a clearing |
| sweep speed | 4.5 m/s | `1.8 U(h)` with `U(h) = 2.5 m/s` at the forest top: an assumption |
| flutter | 1.26 Hz | the honami's own `1.8 / 1.4`, between Py et al.'s alfalfa 1.05 Hz and wheat 2.50 Hz |
| wave pairs | ±20° of the wind, shares 0.2, 0.2, 0.3, 0.3 | authored, so fronts break into patches rather than run as lines |

Lean and flutter amplitudes are the trees' (`0.6 + 0.8 g`, `0.25 + 0.35 g` of the compliance) and
the compliance is Tuft's 0.20: still compatibility values, not a measured blade EI (de Langre 2008,
above). The speeds do not scale with the wind's strength, which would make the phase jump by the
change times the clock; a turning wind re-phases the gust far from the world's origin (the mean lean
does not jump).

**Measured** (renderer.md, "The field kind", has the table and the machine's state): the cooked
dense meadow's flight at 1920×1080 went from 35.7 ms of GPU (13.0 ms cascades, 22.3 ms pool pass,
15.3 ms CPU posing, 2.6 GB of tail room for 4,514 card records) to 1.42 ms (0.11, 0.09, 0.14 ms, no
grass record), and at 11520×2160 from 60.1 to 6.9 ms; cascade pairs fell from 370,918 to 3,526 at
1080p. The POC's bounded 64 × 64 m dense sward, 173 ms of GPU on 51ef062a, draws in 7.6 ms
offscreen at 1920×1080 (4.6 ms cascades, 0.44 ms pool) and 23.9 ms at 11520×2160. A full-density
card container keeps 16 of its 1,040 blades once loaded (1,040 parts before). None of these is a
quiet run.

**Still open on the engine side**: section 41 in the resolve (`ao` on the sky's ambient, `hue` and
`age` on the base colour; `thickness` waits for R22) — not reached in R95's budget; walked-track
history; a gust field that follows a turning wind without re-phasing.

## V5 asset measurements, 2026-10-25

Independent 256² four-view coverage measurements of seed 8801, variant 0, after fitting with
128² probes. These are geometry/coverage results, not frame timings. Each isolated stand has
64 copies of its patch, no terrain masking gaps, wind zero and shadows off; raw 1600×900
captures use matching near/card close, walking-height and overview cameras. Camera overview
is at 10 m for inspection, closer than the intended outer-ring card use. The table is these
seven authored recipes, not a claim that every species/density/seed has been accepted.

| State | Rendered blades/m² | Modeled LAI | All-rung triangles | Coarse max difference (%) | Card max difference (%) | Full/card clusters |
|---|---:|---:|---:|---:|---:|---:|
| Dense | 1024 | 3.531 | 8288 | 0.71 | 1.79 | 348 / 3 |
| Dense low | 384 | 1.303 | 2400 | 0.00 | 3.29 | 103 / 3 |
| Mown | 1024 | 0.265 | 10096 | 0.91 | 1.08 | 411 / 3 |
| Grazed | 768 | 0.304 | 7186 | 0.48 | 2.24 | 296 / 3 |
| Trodden | 192 | 0.034 | 1976 | 2.15 | 2.90 | 83 / 3 |
| Clumps | 192 | 0.454 | 1578 | 0.61 | 1.55 | 69 / 2 |
| Tall | 768 | 3.202 | 6240 | 0.50 | 2.14 | 262 / 3 |

Counts include geometric near/coarse rungs and actual nonempty card groups. Budgets conservatively
reserve every possible group and a cut-end triangle for every blade; a blade already below the
cut height stays pointed. The lower actual mown/grazed triangle counts follow from those uncut
leaves, not dropped cuts. Reference raw data and 42 PNGs live in the external
`sward/2026-10-25-v5/final-showcase`, with `comparison.html`, `measurements.json`, individual
recipes and provenance. The low recipe gives up coverage; it is a candidate, not a final quality
policy. The full patch library and ground transitions remain to be visually accepted.

V5 deterministic pins: section41 **8707601967473198806** and eight-variant geometry/attributes/
fitted-cutoff family **2792296357357641801**. Linux Clang reproduced both and passed all 25 selected
tests (649.46 s, 11m43s including build); its two worktree volumes were pruned. The focused CPU
contact regression passed 12 assertions. Windows `msvc-debug test -Affected -Base 84e83f84`
passed all 122 selected tests (3861.44 s), including renderer, content, viewport and frame-loop
checks. Committed content makes the current affected selector choose the whole suite.

Build notes: after the Sward schema grew, MSVC incremental links reported LNK1236 for existing
fern/companion object members in several consumers. Recompiling those objects was not sufficient
for every consumer; clearing only this worktree's `.ilk` files completed the links. No shared
compiler cache was purged and no compiler-cache launcher was enabled here. In the recipe-driven
PowerShell cook, the parameter `$Recipe` and local `$recipe` collide case-insensitively; the
local must use `$swardPatch` so a hashtable is not coerced to the parameter's string type.

The dense terrain family also cooked successfully: two variants at each of 25%, 50%, 75% and
100% density. Across these eight patches, the independent four-view maximum is 1.21% for the
coarse ribbons and 2.82% for cards. Full-density modeled LAI is 3.531/3.422; exact source values
and per-patch counts are retained in external `dense-meadow/family-measurements.json`.

The dense terrain replay completed with raw captures inside the stand, at the 5 m walking view,
20 m and 80 m. Near cover is continuous and the authored trail stays clear. The 80 m image has
an obvious card-to-ground boundary; the shared colour source alone does not make the transition
invisible. This remains a failed visual acceptance item. Captures use the RTX 5090, `msvc-debug`,
1600x900, wind zero, sky hour 12 (sun elevation 64.42 degrees), CSM with 8 m shadow distance as
the temporary stand-in for the absent caster policy. They are not quiet benchmark runs:
`machine_state=null`, `quiet=false`, uncollected GPU timings are zero. Do not interpret those
zeros as measured costs.

The final replay record retains 7,861,937 motion parts and 2,616,732,832 motion-tail bytes;
tail storage is not deformed-pool bytes. Final ring instances are `[0,1310,0]` and pairs
`[0,3930,0]`, with 5841 prefix pairs. These are final-camera values, not peak near-view counts.
They do not establish the pair/shadow budget. `frame_loop.last_frame=-1` likewise supplies no
allocation proof. The dense path still needs the engine's GPU field motion, explicit pool-byte
statistics, quality ladder and quiet frame-budget run. All camera, scene, density, provenance,
PNGs and the complete record are in external `sward/2026-10-25-v5/dense-meadow`.

The owner selected **mostly knee height** for the first tall-grass walkthrough on 2026-10-25.
Use that visual target for the dense/tall variants and subsequent stem/seed-head work, keeping
botanical dimensions within the cited species ranges. Waist/chest-height stands are not the
default acceptance target. This is an art-direction reference, not a new botanical constant.

### Next representation experiment

AMD's [procedural grass example](https://gpuopen.com/learn/mesh_shaders/mesh_shaders-procedural_grass_rendering/)
uses curved blades grouped into patches, reduces blade count while increasing width with distance,
and applies root darkening and spatial colour variation. Its later
[procedural generation sample](https://gpuopen.com/learn/work_graphs_mesh_nodes/work_graphs_mesh_nodes-procedural_generation/)
replaces dense patches with quads for its sparse LOD. These are concrete examples of multiple
representations, not evidence that one technique is universal across AAA games or a performance
prediction for this renderer.

For short turf, the next proposed comparison is a baked dense basal layer plus a smaller set of
geometric leaves, against a higher-count geometric patch, with identical footprint and cameras.
The source lawn used for baking can be denser than the runtime mesh. This is a proposal prompted
by the sparse clipped captures, not an implemented new tier. For tall cards, test vertical fans
and additional depth samples against the current three orthogonal planes: covered area must be
accompanied by elevated-view silhouette and parallax review. Both experiments must retain one
appearance source for near geometry, cards and ground. ComfyUI leaf-material variants can then
be compared through that same bake route; unrelated whole-grass textures would not establish
cross-tier consistency.

## Local-main integration, 2026-10-25

The owner authorized a targeted merge check, with the full matrix deferred to tonight's run.
The combined tree includes main `4a48e749` (tree maturity and rock detail). Main's tree catalogue
IDs remain unchanged and sward is appended after alder; the resolved maturity validation path
also validates sward. Both new test families and registrations remain present.

Independent branches had reserved G88 three times. Tree maturity keeps G88, rock detail is G89,
and sward is G90. Historical external capture labels and commit subjects retain their original
G88 label; the checked-in references now use the unique rows.

The scoped checks use existing controls: debug flora, geometry, scene generation, world,
scene collision, lint and docs; release counterparts plus the frame-loop allocation test,
renderer flora/motion/streamed-tail cases, authored streaming viewport cases, flora/content
canonical determinism cases and a live sward cook; one filtered Linux Clang run. No global gate
policy is changed. The unrelated Windows suites, remote GPU matrix and full frame-budget set
are deferred at the owner's request. Results are recorded when these integration checks finish.

Integrated debug checks: flora, geometry, scene_gen, world, scene_collision and both lint checks
passed (the eight-entry run took 470.92 s). Its docs check found three old G88 heading anchors;
those were corrected and `dev.ps1 docs` then passed (256 pages, 5753 links). Both sward pins
remain unchanged on the combined tree. No code fix was needed after conflict resolution.

Integrated release checks passed: 9/9 CTest entries including frame-loop allocations (169.81 s),
20 focused renderer cases / 1999 assertions, 4 authored-streaming viewport cases / 82 assertions,
and 9 flora/content determinism cases / 578 assertions. Cases outside those source-file filters
were deliberately not selected. The live release dense cook passed and retained source hash
2250113272344848541, 348 full clusters and 8288 all-rung triangles.

Linux Clang passed all 9 selected integration entries (358.75 s; 6m29s including build).
Both deterministic sward pins remain unchanged. The persistent gate's volumes remain warm.
The tested code is integration commit `51ef062a`; the follow-up changes only this record and
roadmap status. Raw results and the reproducible release harness are retained under external
`sward/2026-10-25-v5/integration`. This focused gate does not close the open grass visual or
performance acceptance items, nor claim the deferred full matrix has run on this tip.

Grass shadows under traced shadows, what other pipelines do and what the clearing costs: [grass-shadows-2026-10-31.md](grass-shadows-2026-10-31.md) (G119).
