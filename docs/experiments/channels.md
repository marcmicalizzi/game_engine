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
  (the same paper's `bend_radius_widths`, pinned at 2.5 by W93 and at 3 by its continuation, below):
  the section is fully asymmetric at that radius and tighter.
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
  since the line's own bends are not regular); **6.6** over 149 spacings since the kinks were
  taken out of the line (below).
- **The pool's depth is the triangle's.** The hydraulic geometry gives a mean depth, an area over a
  width; a triangle of that width and area is twice the mean deep. So the thalweg at a bend's apex
  is `2 d̄` at the outer bank's toe, and between riffle and apex the inner bank's run is whatever
  keeps the area, `2 W (1 - d̄ / D) - D / m_outer`. **Read since** (below, "The bend's scour depth"): the published
  relations give 1.9 to 2.0 times the mean at a radius of two to three widths, which is this
  factor.
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

## The steep reaches, the pool's place, and three numbers read again

(W93, continued, 2026-11-10; [channels](../subsystems/channels.md#the-steep-reaches). What was
read, and how: each item says whether the paper itself was read or a secondary source that quotes
it. The reels looked at before the design: r03 and r49, and the motion sheets
`clear-riffle-cobbles` and `riffles-and-pools`.)

- **What a step-pool and a cascade reach are.** Montgomery & Buffington (1997, *GSA Bulletin*
  109(5): 596-611), **read in the paper** (a forest-service copy). Cascade: "narrowly confined by
  valley walls", a bed "longitudinally and laterally disorganized", cobbles and boulders, with
  small pools that span part of the channel "less than a channel width apart". Step-pool: large
  clasts in discrete channel-spanning accumulations (the steps) between pools of finer material,
  pools "roughly one to four channel widths" apart, steep gradients, small width-to-depth ratios
  and pronounced confinement by the valley's walls. Its table 1: pool spacing in widths 1 to 4
  (step-pool), under 1 (cascade), 5 to 7 (pool-riffle), none (plane-bed, which "lack rhythmic
  bedforms"); both steep types confined. Its field slopes are the layer's bounds (under 0.015,
  0.015 to 0.03, 0.03 to 0.065, over 0.065). **On banks the paper says nothing of their material**:
  only that confinement and immobile clasts make bank cutting unlikely. So "a confined channel's
  banks are boulders and bedrock, not a cut bank" is this page's reading of *confined by valley
  walls* with *channel-spanning accumulations*, not a sentence of the paper.
- **A step's height against its length and the slope**: Abrahams, Li & Atkinson (1995, *Water
  Resources Research* 31: 2593-2602), **abstract only**: `1 <= (H/L)/S <= 2`. The layer's
  `step_height_ratio` 1.5 is the middle of that range; **the paper's own mean was not found**.
- **Steps form where the channel narrows.** Saletti & Hassan (2020, *Earth Surface Dynamics* 8:
  855), a flume study, **read**: narrowing segments are "the predominant place where steps form
  and remain stable". Zimmermann, Church & Hassan (2010, *JGR Earth Surface*), **abstract only**:
  steps are stable where the channel's width over the step clasts' D84 (the jamming ratio) is
  under about six. So the cut is narrower at a step's crest and wider over its pool. **The
  amount, `step_widening` 0.3 (15 % either way), is a design choice**: no field ratio of a step's
  width to its pool's, and no coefficient of variation of width along a step-pool reach, was
  found. The two clasts laid at a step's ends, half in each bank (1 to 1.5 of the step's clast),
  are the jammed accumulation's ends, a reading and not a measured size.
- **A steep line is not a ruled one.** Rosgen (1994, below), **read**: type A streams (4 to 10 %,
  "cascading, step/pool bed features") and Aa+ (over 10 %); their sinuosity, 1.0 to 1.2, is from a
  secondary table (NRCS NEH 654, TS3E). No measured sinuosity of hillslope gullies was found. The
  wander (`steep_wander_widths` 0.75) gives 1.006 on the test's planar hillside, inside that
  range and at its low end; **its amplitude and its knots (5 widths) are design choices**, bounded
  by what the cut can hold: at 1.5 widths the test valley lost its downhill bank at 2.7 % of its
  points (2 % is the bound), at 0.75 at 1.8 %.
- **The pool lies downstream of the bend's apex.** NRCS, *National Engineering Handbook* part 654
  (2007), chapter 12, on the 1981 Red River survey, **read as a secondary source**: "the deepest
  pool is usually located downstream from the bend apex", the pool's offset from the apex over
  the apex-to-inflection channel distance averaging **0.36** (range -0.4 to 1.08), independent of
  sinuosity. A meander 10.9 widths long has 2.7 widths from apex to inflection, so the section is
  read **one width downstream** of the bend (`k_pool_lag_widths`). Dietrich & Smith (1983) and
  Johannesson & Parker (1989) on the bed's lag behind the curvature were **not read**.
- **The bend's scour depth** (the first of the three numbers W93 could not read). The same
  handbook's chapter 12 quotes Thorne & Soar's (2001) fits for the greatest depth in a bend over
  the mean depth at the crossing, for a radius under ten widths: `2.14 - 0.19 ln(Rc/W)` for a
  width-to-depth ratio under 60, which is **2.01 at two widths and 1.93 at three**; and Maynord
  (1996, *J. Hydraulic Engineering* 122: 460-464) as the handbook's technical supplement 14B
  quotes it, `1.8 - 0.051 Rc/W + 0.0084 W/y`, 2.03 and 1.98 at a width-to-depth of 40. **Secondary,
  and one caution**: the extraction of the handbook's page was garbled across its columns, so
  which of the two fits goes with which width-to-depth class is a reading. Either way the factor
  this page took from geometry, **twice the mean at the apex, is what the published relations
  give at a radius of two to three widths**; nothing changed, and it is no longer only geometry.
- **The riffle's width** (the second). Richards' (1976) own percentage was **not found**; a 2017
  review (Brown & Pasternack, *Earth Surface Dynamics* 5) says only that his riffles were wider
  than his pools on average. The handbook's chapter 12 gives Hey & Thorne's (1986) riffle width
  as 1.034 of the reach's, ranging 0.75 to 1.5. **`riffle_widening` 0.12 stays a design choice**,
  inside that range and above its mean. **And a finding that was not applied**: the same chapter's
  Red River widths, relative to the width at the inflection, are 1.35 at the apex and 1.15 at
  the pool for bends with point bars (1.05 and 0.95 for bends of even width). A bend with a point
  bar is *wider* than its crossings there, where this section narrows it by 6 %. It was found
  after the section was built and tested, it is one survey read at second hand, and applying it
  changes every bend's cut; it is left as a row.
- **Rosgen's bounds** (the third). Rosgen (1994, *Catena* 22: 169-199), **read in a text copy of
  the paper**: the entrenchment ratio is "the ratio of the width of the flood-prone area to the
  bankfull surface width", the flood-prone width taken at "twice the maximum bankfull depth";
  1 to 1.4 is entrenched, 1.41 to 2.2 moderately entrenched, over 2.2 slightly entrenched; "the
  continuum allows for a change of ±0.2 units". So the 2.2 the section above gave "as its readers
  cite it" is the paper's own, and `flood_entrenchment_min` 1.4 is its lower bound.
- **`bend_radius_widths` is 3**, the upper end of Leopold & Wolman's 2 to 3 (it was 2.5). With
  the kinks low-passed out of the line (below) fewer points reached full asymmetry: at 2.5 the
  12-seed test valley's pools stood 7.4 widths apart, outside Keller & Melhorn's 5 to 7, and no
  point of the area test's valley reached a full apex. At 3, with the stretch that counts as
  straight widened (the mean bend over a pool-to-pool length under 0.2 to 0.5, from 0.1 to 0.3),
  the mean is **6.6 widths** (149 spacings, 2.7 to 24) and the outer bank is the deeper side at
  327 of 341 bends (95.9 %; 99.6 % before, when the pool sat at the apex and the test's bend is
  still defined there).
- **The kinks.** The line is low-passed at half a width (a Gaussian's sigma; two steps at
  least) after the trough test. A meander 10.9 widths long keeps 96 % of its amplitude
  (`exp(-(2 pi sigma / lambda)^2 / 2)`). Measured over seven test valleys and four seeds: the
  tightest radius between consecutive points is 1.02 widths (of a metre at least), the bound held
  is half a width; the test valley's points with less than half a downhill bank went from 32 of
  1,969 to 36 with the wander and 20 without it, and those with none from 12 to 0.

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
## Bed sediment

W97, 2026-11-10. What lies on a bed, read before the design ([channels](../subsystems/channels.md#bed-sediment)).
A research sub-agent did the reading for this row; each number below says how it was read: **full
text**, **abstract**, **secondary** (quoted by another source, named), or **not read** (a design
choice, or from memory, said so).

| Number | Value here | Source | How read |
|---|---|---|---|
| Bed material by reach | dune-ripple sand; pool-riffle gravel; plane-bed gravel-cobble; step-pool cobble-boulder; cascade boulder; bedrock rock; colluvial variable | Montgomery & Buffington 1997, *GSA Bulletin* 109: 596–611, Table 1 | full text |
| A surface pebble count's median by reach | Finney Creek composite: D50 17 mm (pool-riffle) to 80 mm (cascade), D84 57 to 250 mm | Montgomery & Buffington 1997, Fig. 8 | full text |
| Relative roughness d90/h, the most | pool-riffle 0.3, plane-bed and step-pool 0.8, cascade, bedrock and colluvial 1 | Montgomery & Buffington 1997 (pool-riffle under ~0.3; plane-bed 0.2–0.8; step-pool 0.3–0.8; a cascade's largest clasts near the bankfull depth) | full text |
| The surface's size distribution | lognormal in size, so D84/D50 = D50/D16 = 2^sigma | Bunte & Abt 2001, USDA RMRS-GTR-74, p. 17 | full text |
| Sorting sigma (phi units) | dune-ripple 0.6, pool-riffle 1.2, plane-bed 1.5, steeper 1.75 | Bunte & Abt 2001 p. 68: ~0.5 after long transport, 1.5–2 in mountain gravel-bed streams; Finney Creek's D84/D50 3.1–3.4 is sigma 1.6–1.8 | full text; **the value a reach takes inside that range is this row's choice** |
| The median | D50 = h S / (1.65 tau*), tau* = 0.15 S^0.25 | Lamb, Dietrich & Venditti 2008, *JGR* 113 F02008, eq. 3 (the critical Shields number rising with slope over S 0.001–0.1, r² 0.41) | full text; taking the bankfull flow as just moving the median (a threshold channel) is Parker et al. 2007's quasi-universal gravel river, **secondary** (its 0.049 not confirmed in the paper) |
| The armour | the surface median is about twice the subsurface's (1 with high supply, 3+ in high-energy mountain streams) | Bunte & Abt 2001 §3.3.1.1 p. 131 | full text; the surface is what is drawn, so the ratio sets nothing here: it says the drawn surface is coarser than what a cut face would show |
| A bar's sorting | the surface median fines by a third from head to tail: 1.2 of the reach's at the head, 0.8 at the tail | Rice & Church 2010, *Sedimentology* 57: 232–251 (Fraser River: 33 % surface, 17 % subsurface) | abstract |
| The flood bed's surface | the bar tail's, 0.8 | none | **design choice** |
| A step's clasts | the reach's D84 to D97.7; a step's keystone about its D84, the step's height of its order | a 2021 synthesis in *Earth-Science Reviews* and a thesis | secondary (Chin 1999 and Grant et al. 1990 not read) |
| Boulder cover on steep reaches | not pinned: the lognormal's own cover above the drawn size | Yager et al. 2007, Nitsche et al. 2011 | **not found** |
| Imbrication | the a-b plane dips upstream 10–30° (tan 0.176–0.577), the a axis across the flow within ~20° | often quoted; one study measures 12–55°, mean 34.5° | secondary (a search snippet); the primary papers (Johansson 1963, Rust 1972, Laronne & Carson 1976) **not read** |
| Protrusion | 0.36 of the b axis on average, drawn 0.15–0.57; never over 0.9 of the clast's height | Vázquez-Tarrío et al. 2024, *JGR Earth Surface*: 0.04–1.0, means 0.35–0.38 | secondary (a search snippet); Kirchner et al. 1990 read in its abstract only (the spread within one size exceeds the difference between surfaces) |
| Shape | b/a 0.7 ± 0.15, c/b 0.6 ± 0.2 | Zingg 1935, Sneed & Folk 1958 | **not read: from memory**, a design choice inside the usual river-gravel range |
| Roundness by transport | `1 - exp(-x / 1 km)` of x metres of channel upstream, 0.05 to 0.95 | Domokos et al. 2014, *PLOS ONE* 9: e88657: edges round with almost no loss of axis length first (phase I), about 1 km of transport; "within the first few kilometres" in the field | full text (the arXiv preprint) |
| The normal's tail | `1 / (1 + e^(-1.702 x))` | a logistic approximation, from memory | **checked by test instead**: the size test compares the drawn bands against `erfc` within 0.05 |

**What the numbers give** on the tests' valleys (`channels: the bed's sediment by reach, in the
plan`): mean D50 13.6 mm on pool-riffle, 31.5 mm on plane-bed, 23.6 mm on step-pool, 27.9 mm on the
cascade — the order of Finney Creek's 17 to 80 mm, smaller because the valleys' channels are
shallow (0.1 to 0.3 m), and the relative roughness caps the steep reaches. The cover of clasts of
64 mm or more on the step-pool valley's bed is 0.18 to 0.20 of its area over four seeds, three
quarters of it in clasts above D84.

**The drawn size**, 64 mm (`clast_min_mm`, 16 to 1,024): the Wentworth cobble's lower bound. A 64 mm
clast is about 9 pixels across at 7 m in a 1080-line, 60-degree view (W93's `stream` camera
distance), and a 32 mm one 4; what is finer is the bed's material, whose texels are the grains
(the roles `gravel/bed` and `sand/bar`, which no pack here has). Halving it to 32 mm multiplies the
count by about four on a pool-riffle bed; it is the knob a far-seeing scene turns up.

**Not applied**, found while reading: none of the sources gives a pool's surface sediment; the
row leaves the deepest of a bend's pool (more than 1.6 mean depths) bare of drawn clasts, a
design choice that reads as the finer sediment a pool collects and the depth hiding the bed.

## Against the proof of concept

The proof of concept's erosion model carved v31 with **688 channel heads** and **5–7 km of channel per
km²**; its v34 paint drew from that drainage. The capability's own count on the same ground is in
[the subsystem page](../subsystems/channels.md#against-the-proof-of-concept) with the region it was
taken over.
