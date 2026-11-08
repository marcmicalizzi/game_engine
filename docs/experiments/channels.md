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

## Against the proof of concept

The proof of concept's erosion model carved v31 with **688 channel heads** and **5–7 km of channel per
km²**; its v34 paint drew from that drainage. The capability's own count on the same ground is in
[the subsystem page](../subsystems/channels.md#against-the-proof-of-concept) with the region it was
taken over.
