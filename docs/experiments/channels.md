# Channels: grounding (W86, 2026-11-07)

What the `channels` capability ([subsystem page](../subsystems/channels.md)) is built from: the
published models of where channels begin, how wide and deep they are, how they bend, what their beds
are made of, and the constants chosen inside each model's range. The sources were found and read
again for this row (2026-11-07), not copied from the proof of concept's v34 page
(`forest_wildlife_poc/research/trails-channels-v34.md`), which used the same literature for paint;
where a figure is an authoring choice inside a published range it says so. The schema's ranges
(`domain/channels/schemas/channels.schema`) are the ranges below.

## Routing the flow

- **Pits filled, then D8.** A lattice of a real ground has closed depressions a flow model must route
  through. The **priority-flood** fill (Barnes, Lehman & Mulla 2014, *Computers & Geosciences* 62:
  117–127) floods from the region's edge in order of height and raises every cell it reaches to at
  least the cell it came from **plus an epsilon**, so a filled flat still has a strictly falling path
  to the edge. Here the heights are integers in micrometres and the epsilon is one micrometre, so the
  fill is exact; the heap is ordered by (height, index), a total order, so the visit order is the same
  on every toolchain. Directions are **D8** (O'Callaghan & Mark 1984, *Computer Vision, Graphics and
  Image Processing* 28: 323–344): each cell drains to the neighbour of steepest drop over the filled
  heights, a diagonal's run √2 spacings, a tie to the first direction. Contributing area is summed with
  every cell after its donors (sorted by filled height). D8 lays 0°/45° lines on planar slopes, which
  is why the line is low-passed (below); D∞ (Tarboton 1997) would spread the area but not change where
  the channels are on this ground's cut troughs, and D8 is what the erosion model that cut v31's
  ground used.
- **Lattice: 4 m** (range 1–32 m). The proof of concept's landform ran at 2 m and its channel paint at
  4 m; 4 m keeps a 3.6 × 10.8 km world to 2.4 million points.

## Where a channel begins

**Montgomery & Dietrich 1988** (*Nature* 336: 232–234, "Where do channels begin?") measured channel
heads in Oregon and California and found the source area above a head falling as the local valley
gradient rises, over slopes of about 5° to 45°, with drier regions needing larger areas at a given
gradient; **Montgomery & Dietrich 1989** (*Water Resources Research* 25: 1907–1918) fitted the
inverse relation, an area–slope threshold `A·S^α ≥ T`, with α about 2 for their data; **Montgomery &
Foufoula-Georgiou 1993** (*WRR* 29: 3925–3934) made it the criterion for extracting networks from
elevation models and noted that the constant is the site's. The engine takes the threshold's form
with both numbers in the layer:

- `head_slope_exponent` α: **1** by default (range 0–3). 0 is the area-only rule the proof of
  concept's landform used; 2 is Montgomery & Dietrich's fit. 1 sits between them and keeps steep
  four-metre cells from starting a channel on a few hundred square metres.
- `head_threshold_m2` T: **1,500 m²** by default (range 10–10⁶). That is the landform's area threshold
  on the fells, 15,000 m² (`world/v31/terrain/landform-controls.json`), at a reference slope of 0.1,
  so a head on a 10 % slope sits where the erosion model started cutting. On the granite and massif the
  landform used 3,000 m²; a layer over that zone sets 300.
- `min_head_area_m2`: **3,000 m²** (range 0–10⁶), the landform's smaller threshold, as a floor: no
  channel on less area however steep.
- Once a cell is a channel, every cell downstream of it is: channels do not stop on the filled flats,
  where the slope is zero.

## How wide and how deep

**Downstream hydraulic geometry** (Leopold & Maddock 1953, USGS Professional Paper 252): along a river
system at a given frequency of flow, width grows about as discharge to the 0.5 and depth as discharge
to the 0.4. Bankfull discharge grows a little slower than drainage area, so on area the exponents fall
to about **0.3–0.5 for width** and **0.2–0.4 for depth**, which is what the regional curves of
bankfull dimensions on drainage area show (**Dunne & Leopold 1978**, *Water in Environmental
Planning*, the eastern United States curves: about 3–4 m wide and 0.3 m deep at 1–2.6 km²).

| Quantity | Default | Range | Why |
|---|---|---|---|
| width `a (A / 1 km²)^b` | a = 3.0 m, b = 0.4 | a 0.1–100 m, b 0.1–0.8 | Dunne & Leopold's curves at 1 km²; b inside Leopold & Maddock's range. A 0.03 km² rill is 0.75 m wide, a 6 km² stream 6.2 m |
| depth `c (A / 1 km²)^f` | c = 0.3 m, f = 0.3 | c 0.02–10 m, f 0.1–0.8 | the same curves; width-to-depth 10 at 1 km², 12 at 6 km², gravel-bed values |

## The line

- **Low-pass.** The D8 line is smoothed by a Gaussian along its length, σ = max(6 m, 2 w) (ranges
  0–100 m, 0–10 widths), eased to nothing at the link's ends so it meets its junctions where the
  lattice put them. The proof of concept's paint used the same σ; it removes the lattice's 45° steps.
- **Meanders.** Leopold, Wolman & Miller (1964, *Fluvial Processes in Geomorphology*, ch. 7), with the
  definitions of **Leopold & Wolman 1960** (*GSA Bulletin* 71: 769–794): wavelength **λ = 10.9 w^1.01**
  and amplitude **2.7 w^1.1**, and a bend's radius of curvature 2–3 widths. The line is offset along its
  normal by half that amplitude times `sin` of the phase run at that wavelength (a link's wavelength
  jittered ±30 %, its phase its seed's), eased in over half a wavelength at each end. The amplitude
  of a sine of half-amplitude 1.35 w at λ = 10.9 w gives a tightest radius of λ² / (4π² · 1.35 w) =
  2.2 w, inside the observed 2–3. Ranges: wavelength 4–30 widths, amplitude coefficient 0–10.
- **No meanders on steep beds.** **Montgomery & Buffington 1997** (*GSA Bulletin* 109: 596–611): pool–
  riffle channels below about 1–2 % slope, plane-bed about 1–3 %, step-pool about 3–8 %, cascades
  steeper; the steep forms are confined and do not meander. The amplitude fades by a smoothstep from
  full below **1 %** to none above **4 %** (`meander_slope_min/max`, 0–0.5), on the link's mean slope.
- **In the trough.** The ground already holds the channel's trough, so a meander offset that would
  climb more than half the bankfull depth out of it is halved, up to three times, then dropped, and the
  offsets are smoothed again (three binomial passes). Authoring: the erosion model's trough is the
  better evidence of where the channel is than the free sine.

## The bed

- **Pool–riffle rhythm.** Pools and riffles alternate every **5–7 widths** (Leopold, Wolman & Miller
  1964; Keller & Melhorn 1978, *GSA Bulletin* 89: 723–730): **6 widths** (range 2–20). Above the
  step-pool slope the rhythm is step to step, **1–4 widths** in Montgomery & Buffington's survey:
  **2.5** (range 1–10). The riffle stands **0.2** of the depth over the pool (range 0–1, authoring
  inside the riffle–pool depth differences the 1964 text shows).
- **The grain.** A gravel bed's median grain is what bankfull flow just moves: the **bankfull Shields
  number** τ*bf = h S / (R D50) is nearly constant across single-thread gravel-bed rivers, about
  **0.049** (Parker 2004; **Parker, Wilcock, Paola, Dietrich & Pitlick 2007**, *JGR Earth Surface* 112,
  F04005, "Physical basis for quasi-universal relations describing bankfull hydraulic geometry of
  single-thread gravel bed rivers"; Dong et al. 2019 quote 0.049 and note later data show it varies),
  with R = 1.65 the submerged specific gravity of quartz. So **D50 = h S / (1.65 · 0.049)**: a 0.3 m
  deep bed on 3 % has D50 ≈ 11 cm, cobble; a 0.1 m rill on 10 % about 12 cm, coarse; a 0.5 m stream on
  0.6 % about 4 cm, gravel. The skin's relief is up to one median grain, **clamped to 0.5–3 cm** (the mesh
  is the lumps; the material's texels are the grains), as a hash of each vertex's world place so the
  joins between pieces agree. Range of τ*: 0.01–0.2.
- **Bars.** Point bars build on the inside of bends and alternate bars along straighter reaches, one
  to each riffle, two to a meander wavelength (Leopold, Wolman & Miller 1964); a bar's top builds to
  near the bankfull stage. A bar here every **half a pool-to-pool length**, on alternating sides along
  a straight reach and on the inside where the line bends (mean curvature times width above 0.05);
  **0.35 widths** wide (range 0.05–0.5), **3 widths** long (0.5–20), its crown **0.4 of the depth**
  above the bed (0–1); none on a bed narrower than **1 m** (0–100). The width, length and spacing are
  the proof of concept's paint's; the rise is authoring, below the bankfull top.
- **Banks.** A cut bank of soil each side where the bed is a **stream's (1.5 m or wider)** and not a
  rill's (0–100 m): **0.6 of the depth** high (0–2), **0.25 w + 0.4 m** wide (the paint's, 0–2
  widths and 0–10 m), with its face over the first fifth of that width and its back falling to the
  ground; **half again higher on the outside of a bend** and half lower on the inside, where the flow
  cuts and the bar builds. Authoring inside the form the sources describe: the ground's trough on a
  2 m lattice cannot hold a metre-scale bank, so the berm stands for it.

## The cut

(W89, 2026-11-07; [channels](../subsystems/channels.md#the-cut), [tileset](../subsystems/tileset.md#channels-cut-into-the-ground).)

- **The depth is a mean.** Leopold & Maddock's (1953) depth, and the regional curves' bankfull depth
  (Dunne & Leopold 1978) the coefficients above come from, is the **mean** depth, the section's area
  over its width. The cut keeps that area: a trapezoid of the bankfull width at the top whose thalweg
  `D` solves `D (W − D / m) = W d̄`. A 3 m channel of mean depth 0.30 m with banks at 45° has a thalweg
  of 0.34 m and a flat bed 2.3 m wide; one whose banks are too gentle for its area (`m W < 4 d̄`) is
  a V of twice the mean depth. Why a trapezoid and not a parabola: a gravel bed is close to flat across
  between steep banks, and the flat bed is where the gravel piece lies; the parabola is S96's for the
  water's optics, which reads the depth at the line.
- **The bank slope, 1 (45°) by default, 0.25 to 8.** A cohesionless bank stands at no more than its
  material's angle of repose, about 30–40° for sand and gravel (Knighton 1998, *Fluvial Forms and
  Processes*, on bank failure by material; the bank-stability literature models a non-cohesive bank
  by its friction angle, Simon & Collison 2002); a cohesive or root-bound bank stands much steeper,
  and a gravel-bed stream's bank is typically a cohesionless lower bank under a cohesive, vegetated
  upper bank near vertical (Eaton 2006, "Bank stability analysis for regime models of vegetated
  gravel bed rivers", *ESPL* 31). 45° sits between the two; an author sets a layer's `bank_slope` per
  material (0.7 for a sandy bank at 35°, 3 or more for a vegetated one). **Read for this**: the
  sources' abstracts and the secondary summaries of them; the ranges are those, and no constant here
  was fitted.
- **The stage, 0.5 of the thalweg (W86's).** Bankfull is a flood (a recurrence of one to two years);
  the water an explorer sees most days is a fraction of it. At a station depth grows about as
  discharge to the 0.4 (Leopold & Maddock 1953), so a flow a tenth to a third of bankfull stands
  `0.1^0.4` to `0.33^0.4`, 0.40 to 0.64, of the bankfull depth. W86's 0.5 lies inside that range and is
  kept; the flow fraction itself is an assumption, not a measurement.
- **The face past the top**, up to a depth more at the same slope, is a channel cut into a trough's
  side: the erosion model's troughs are wider than the channels in them, and where the line runs
  higher than the trough's floor the bank on the uphill side is cut back rather than left as a step.
  Past it nothing, so a cliff beside a channel is not cut back.

## The section

(W93, 2026-11-10; [channels](../subsystems/channels.md#the-section). The owner's complaint through
three updates of the proof of concept: a lowland stream reads as a canal, one trapezoid along a
reach. Looked at before the design: the reference reels r03 and r49 and the motion sheet
`clear-riffle-cobbles`, a clear cobble river whose outer bank is a low turf edge with a rounded,
slumping top and whose inner side is a pale gravel bar rising out of the water.)

- **A bend's section is asymmetric.** Leopold, Wolman & Miller (1964, ch. 7) and Leopold & Wolman
  (1960): at a bend the deep water lies against the concave (outer) bank, which is cut and steep,
  and a point bar builds from the convex (inner) bank toward it; at the crossing between two bends
  the section is symmetric and shallow, a riffle. A bend's radius of curvature is **2 to 3 widths**
  (the same paper's `bend_radius_widths`, pinned at 2.5): the section is fully asymmetric at that
  radius and tighter.
- **Straight reaches have the rhythm too.** Leopold & Wolman (1957, USGS Professional Paper 282-B,
  "River channel patterns: braided, meandering and straight"): a straight channel's thalweg wanders
  from bank to bank and its bed has pools and riffles at about the spacing a meander's would have,
  with bars alternating sides. So where the line does not bend the asymmetry alternates sides, to
  `alternate_asymmetry` (0.6) of a bend's. **The 0.6 is a design choice**: a straight reach's
  pools are shallower than a bend's, and no ratio was read.
- **Pool to pool, 5 to 7 widths.** Keller & Melhorn (1978, *GSA Bulletin* 89: 723-730, "Rhythmic
  spacing and origin of pools and riffles"), as its readers cite it (re-checked 2026-11-10 against
  two reviews that quote the range; the paper's own mean was not found): `pool_riffle_widths`, 6,
  is W86's and unchanged. A meander's bend is half of 10.9 widths long, so pools at its apexes
  stand 5.45 apart, inside the range. **Measured**: over 12 seeds on the test valley, 172 spacings,
  mean **6.0 widths** (the test holds the mean to 5 to 7; single spacings run from 1.7 to 22,
  since the line's own bends are not regular).
- **The pool's depth is the triangle's.** The hydraulic geometry gives a mean depth, an area over a
  width; a triangle of that width and area is twice the mean deep. So the thalweg at a bend's apex
  is `2 d̄` at the outer bank's toe, and between riffle and apex the inner bank's run is whatever
  keeps the area, `2 W (1 - d̄ / D) - D / m_outer`. **Not read**: the published bend-scour relations
  (the ratio of a bend's greatest depth to its mean as a function of radius over width); the factor
  2 here is geometry, and it is the upper end of what those relations are cited for.
- **A riffle is wider than a pool.** Richards (1976, *GSA Bulletin* 87, "Channel width and the
  riffle-pool sequence"), as cited (re-checked 2026-11-10: riffles are on average wider than the
  pools beside them, the flow over a riffle being turned toward the banks). **The share, 0.12
  (`riffle_widening`), is a design choice**: the figure was not found in a source read, and the
  field's range is 0 to 0.5. Half of it is taken each way about the hydraulic geometry's width,
  and the depth changes with it, so the area is the hydraulic geometry's at every point (the test
  integrates the section and holds it to 4 %).
- **The outer bank, 3 (72 degrees).** A cohesive or root-bound upper bank stands near vertical
  (Eaton 2006, above); the tile set's lattice draws a face no steeper than its depth over one
  spacing, so on 25 cm a 0.5 m bank at 3 is two points. `cut_bank_slope`, 0.25 to 8.
- **A rounded top, 0.75 of the depth.** A bank's top is convex: turf rolls over it and its lip
  slumps (the reels; no profile was read). `bank_round` is the height of the band over which the
  cut's face and the ground are joined by a smooth minimum; 0.75 rounds the upper three quarters
  of a 45-degree bank on flat ground. **A design choice.**
- **Which bends count: a window of four widths.** The asymmetry reads the line's mean curvature
  over four widths about a point. A bar and its pool are built by the flow along a bend, over a
  length of several widths (the linear theories of flow in bends give the bed an adaptation
  length of that order; **not read for this row**), and the line W91's trough test draws back
  turns left and right again inside two or three widths. Measured on the test valley over 12
  seeds: read over 1.5 widths those kinks put pools 2.7 widths apart; over a window of 3 widths,
  4.7; of 4, **6.0**; of 6, 7.9, with the bends' asymmetry fading. Four is the window that keeps
  the published spacing. The same test's bends are defined at that scale (a turn of 30 degrees
  between the line's heading two widths above a point and two below), and 551 of 553 have the
  deeper side on the outside; a bend as a chord of two widths each way sees it (323 on the same
  seeds) has it there on 90 %, the rest being kinks the window averages out.

## The flood bed

(W93, 2026-11-10; [channels](../subsystems/channels.md#the-flood-bed). The proof of concept's
solver needs, 2.1 item 3; the reels r03 and r49: a clear river whose inner side is a pale gravel
bar and whose outer bank is a turf edge half a metre high, and a turquoise one threading a cobble
bed several times its width under a raw scarp of eroded earth.)

- **Entrenchment and the flood-prone width.** Rosgen (1994, "A classification of natural rivers",
  *Catena* 22: 169-199): the **flood-prone width** is the valley's width at an elevation of **twice
  the greatest bankfull depth** above the thalweg, and the **entrenchment ratio** is that width over
  the bankfull width; a ratio under **1.4** (give or take 0.2) is an entrenched channel, one that
  fills its valley's floor (types A, F and G), 1.4 to 2.2 moderately entrenched (B), and over 2.2
  slightly entrenched, with a floodplain (C, D, E). **Read for this row** (2026-11-10): the
  definition and the 1.4 bound from an extension summary of the classification (North Carolina
  State, "Application of the Rosgen stream classification to North Carolina"); the paper's own
  PDF was fetched and has no text layer, so **the 2.2 bound and the type letters are from the
  paper's key as its readers cite it** and nothing here is pinned to them. Pinned:
  `flood_height_depths` 2 (the elevation, in thalwegs over the bed) and
  `flood_entrenchment_min` 1.4 (no flood bed below it).
- **The bed's width, three widths at the most** (`flood_bed_widths`). **A design choice** inside
  Rosgen's slightly entrenched class (over 2.2): a single-thread gravel-bed river's active bed with
  its bars is a few channel widths wide, and the proof of concept's own paint was 3.2 (1.1 widths
  either side). A wandering or braided river's bed is many more, and braided reaches are not this
  row's.
- **Wider on the inside of a bend** (0.6 of the half bed, `k_flood_bend`). The point bar accretes
  on the inside and the outer bank is cut into the terrace (Leopold, Wolman & Miller 1964), so the
  bed is the bar's side's. **The share is a design choice.**
- **The bed's level, a quarter of a thalweg under the top** (`flood_bed_depth`). A bar's top builds
  to near the bankfull stage (the same text), which is the top of the bank here, and the bed must
  stand over the water's stage (half the thalweg, "The cut" above) or it floods. **A design choice
  between the two.**
- **The scarp, 1.5 (56 degrees)** (`scarp_slope`). A freshly cut face of alluvium stands steeper
  than its angle of repose (30 to 40 degrees, above) while its fines and roots hold it, and
  ravels back to it; r49's scarp is near vertical at its top over a ravelled foot. **A design
  choice** between the two; the lattice at 25 cm draws a 0.5 m scarp as two points.
- **What was not grounded**: bar forms on the bed (unit bars, chutes, the riffle where the flow
  crosses a bar), which are not built.

**Measured**: three valleys with floors of 32, 9 and 2 m between 45-degree walls and a 4.6 to
5.3 m stream: the entrenchment ratio read from the ground is the geometry's to 3 %, and the bed is
three widths, the floor less the scarps' runs, and none
(`domain/channels/tests/channels_tests.cpp`).

## Reaches

(W90, 2026-11-08.) **Montgomery & Buffington (1997)**, "Channel-reach morphology in mountain
drainage basins", *GSA Bulletin* 109(5), 596–611: seven reach types, colluvial and bedrock and five
alluvial ones (cascade, step-pool, plane-bed, pool-riffle, dune-ripple), ordered by falling slope
and rising drainage area, with overlapping slope distributions in their field data. The bounds as the
paper's synthesis is cited by its readers (the paper's own table could not be fetched tonight; the
bounds below are the ones its citations agree on, and each is a layer field the author may move):

| bound | value pinned | range reported | source |
|---|---|---|---|
| cascade above | 0.065 | > 0.065 (typically 0.08–0.30) | M&B 1997 as cited |
| step-pool | 0.03–0.065 | 0.03–0.065; 3–7 % in later citations | M&B 1997 as cited |
| plane-bed | 0.015–0.03 | 0.01–0.03 | M&B 1997 as cited |
| pool-riffle below | 0.015 | < 0.015; 0.001–0.02 | M&B 1997 as cited |
| dune-ripple below | 0.001 | < 0.001, sand beds | M&B 1997 (low-gradient sand) |
| bedrock / colluvial above | 0.2 | colluvial and bedrock reaches occupy the steepest, the colluvial at the network's tips | **a design choice**: the cascade's upper tail, not a bound the paper gives |

Which of bedrock and colluvial a steep point is: colluvial on a link that begins at a channel head
(Montgomery & Buffington: colluvial channels are the headwater tips, filled from the hillslopes),
bedrock otherwise. A design choice, stated; their area bound was not read. The window (20 widths, at
least 20 m) follows their definition of a reach as a stretch of many channel widths. Bars: on
dune-ripple and pool-riffle reaches only; Montgomery & Buffington's plane-bed reach has no bars, so
the brief's "bars on plane-bed and pool-riffle" was narrowed to the paper's.

**Step spacing**: 1 to 4 channel widths (the step-pool literature as Montgomery & Buffington
summarize it; Chin 1999, *Geomorphology* 27, 191–204; Chartrand & Whiting 2000, *ESPL* 25, 583–600:
steeper beds closer). The rule `L/w = clamp(2.5 sqrt(0.03 / S), 1, 4)` is a fit to that range, not a
published regression: 2.5 widths at S = 0.03, 1.7 at 0.065, 1 from about 0.19. **Step height**:
Abrahams, Li & Atkinson (1995), "Step-pool streams: adjustment to maximum flow resistance", *Water
Resources Research* 31(10): steps satisfy 1 ≤ (H / L) / S ≤ 2, the maximum of flow resistance near
the upper half; pinned at 1.5 (`step_height_ratio`, range 1–2). A step-forming clast is of the step's
height (Chin 1999), which sizes the boulders. The 1 m fall height (`fall_min_m`) is a design choice
for what the water draws as a fall rather than a ramp.

### W91, re-grounded

(2026-11-08, roadmap W91; the owner's v36 walk, items 20 and 21.) W90 took the step relations onto
every steep reach. Re-read for this row:

- **Montgomery & Buffington (1997)** (above): **cascade** beds are disorganised longitudinally and
  laterally — tumbling flow over and around individual large clasts that protrude through it at most
  stages, with no regular sequence of steps; **step-pool** beds alone have the regular alternation of
  channel-spanning steps and pools; **bedrock** channels lack a continuous alluvial bed (transport
  capacity exceeds supply) and **colluvial** channels are the headwater hollows filled from the
  hillslopes, with no fluvial bedforms. So: steps on step-pool reaches alone; clusters on cascades;
  neither on bedrock nor colluvial reaches. Read from the paper's synthesis as its readers cite it (the
  full text was not fetched, as for W90).
- **The step relations' range.** Abrahams, Li & Atkinson (1995) and Chin (1989, 1999) measured
  step-pool channels of slope about 0.03–0.2; the relations are not extended past it. The pinned rule
  on step-pool reaches (0.03–0.065) gives H/(L S) = 1.5 by construction, inside Abrahams et al.'s 1–2
  (the test holds every step to it), L/w 2.5 to 1.7.
- **Knickpoints.** A knickpoint is a convexity of the long profile, an abrupt steepening that the
  rest of a bedrock river's profile adjusts to (Whipple 2004, "Bedrock rivers and the geomorphology of
  active orogens", *Annu. Rev. Earth Planet. Sci.* 32, 151–185, as summarized; not read in full).
  Hayakawa & Oguchi (2006), "DEM-based identification of fluvial knickzones and its application to
  Japanese mountain rivers", *Geomorphology* 78, find knickzones on a DEM by comparing a river's
  gradient over a short window with its trend over longer ones (read from their conference abstracts:
  50 m DEMs, windows of hundreds of metres, a threshold on the rate the gradient falls with the
  window's length). W91 takes the idea, a short window's gradient against the reach's, at the scale a
  channel a few metres wide is drawn at: a window of 3 widths (at least 2 m) and the threshold the
  water's own, a fall of at least 1 per metre (45°) over it, dropping at least `fall_min_m` (1 m).
  **The window, the 1 m and the chute's 0.9 are design choices**, not the papers' numbers: their DEMs
  are fifty times coarser than the 0.5 m line.
- **The riser's angle, 38°.** A boulder step's face stands at about its clasts' angle of repose: about
  35° on natural talus (Carson 1977, "Angles of repose, angles of shearing resistance and angles of
  talus slopes", *Earth Surface Processes* 2, 363–380, as cited), steeper for coarse angular clasts
  (the mass angle of repose of riprap rises with angularity, size and non-uniformity: "Mass angle of
  repose of open-graded rock riprap", *J. Irrig. Drain. Eng.* 2011, abstract). 38° sits in the 35–40°
  the brief named and below the 45° at which the water leaves its bed; range 20–44.
- **Cascade clusters.** The spacing's exponential (mean 1.5 widths) and the clasts' one to two
  bankfull depths are **design choices** for "no regular sequence" and "clasts protruding through the
  flow"; no published spacing distribution was read.
- **The ribbon on the ground.** Water stands level across its section and never over the ground
  beside it; a surface over lower ground would spill toward the fall line. The 3 cm tolerance and the
  2 cm margin are design choices under the lattice's height step on a slope.

## Mouths and confluences

**The fan's length** (`fan_widths`, 3 widths, and its spread to twice the width at the shore) is a
**design choice**, not a pinned source: the brief asked for "a few widths", and a river-mouth deposit's
length in channel widths was not found in a source read tonight. The bed grading to half a thalweg
under the level makes the water at the shore its upstream depth at the default stage. **Confluences**:
the tributary meets the trunk's water surface (the junction's water levels are equal, the classic
confluence condition; Best 1987 on confluence morphology, not read tonight), and its bed is graded to
the trunk's over 3 of its own widths, also a design choice.

**The planform**: Leopold & Wolman (1960), "River meanders", *GSA Bulletin* 71, 769–794: the
meander wavelength 10–14 widths about a mean, the source of `meander_wavelength_variation` = ±25 %
(10.9 ± 2.7 covers 8–14); the amplitude's ±50 % is a design choice for bends that differ visibly.
## Against the proof of concept

The proof of concept's erosion model carved v31 with **688 channel heads** and **5–7 km of channel per
km²**; its v34 paint drew from that drainage. The capability's own count on the same ground is in
[the subsystem page](../subsystems/channels.md#against-the-proof-of-concept) with the region it was
taken over.
