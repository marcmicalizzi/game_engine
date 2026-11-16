# Steep streams: what a small flow on a steep bed looks like (W130, 2026-11-15)

The owner walked the forest's streams on 2026-11-15 and found them "incredibly disjointed on any
slopes, with dry patches and wet patches". The forest session's count (its own script, closed to
the engine's agents): on lines steeper than 30 %, 7.4 % of the length a 5 mm film and 6.0 pieces of
water every 100 m, against under 1.6 on gentler lines; the line the owner stood at falls 37 % over
418 m with 21 l/s, a metre wide and 6 cm deep, in 34 pieces. This page grounds what such a stream
is in the published work, before anything in the solve or the draw changes, and says which sources
were read whole, which only as a summary or a snippet, and which could not be reached. The rules
that follow are on [channels](../subsystems/channels.md#how-whole-a-stream-is-drawn) and
[water](../subsystems/water.md).

[The channels grounding](channels.md#the-steep-reaches-the-pools-place-and-three-numbers-read-again)
already holds Montgomery & Buffington's reach types and Abrahams, Li & Atkinson's step geometry,
read for W91 and W93; what is new here is what they say about **the water** at a small flow, and
the stepped-chute hydraulics that say what form a drop takes.

## What was read, and how far

| Source | What it gives | Read |
|---|---|---|
| Montgomery & Buffington (1997), "Channel-reach morphology in mountain drainage basins", *GSA Bulletin* 109, 596–611 | the reach types by slope: pool-riffle under about 2 %, plane-bed 1–3 %, step-pool 3–8 %, cascade above about 8 % (to about 30 %), colluvial and bedrock beyond; a cascade's pools under a channel width apart, a step-pool's one to four | full text (for W86 and W91; [channels](channels.md)) |
| Abrahams, Li & Atkinson (1995), "Step-pool streams: adjustment to maximum flow resistance", *Water Resources Research* 31, 2593–2602 | a step's height H over its length L against the slope S: **S ≤ H/L ≤ 2S** on flume slopes of 0.05 to 0.20, the bed's form adjusted to maximum resistance | as cited (for W91) and again tonight as a summary of a search; the paper itself not reached |
| Chin (1999), "The morphologic structure of step-pools in mountain streams", *Geomorphology* 27, 191–204 | 464 step-pools in 13 reaches of the Santa Monica Mountains: the clast size sets a step's height, the discharge its wavelength; the slope relation may be correlation and not cause | abstract only |
| A Swiss steep-stream reach (IAHS proceedings; Molnar and others) | **steps take about 60 % of the reach's total drop** (57.4 m of step height in its elevation change) | a search snippet of its table; the PDF could not be decoded here |
| Chanson, stepped chutes (University of Queensland notes after Chanson 1994, *J. Hydraulic Research* 32; 1996, 34) | the flow regimes down a stepped bed: **nappe flow** at low discharge and large steps, the water a free nappe from step to step over an air cavity, the head lost at each step its height; a transition; **skimming** flow over the step edges at high discharge; the onset correlations hold for flat steps with h/l 0.2 to 1.4, ±30 % | the notes read whole (their equations are images and were not legible) |
| Wüthrich & Chanson (2014), gabion stepped weir, *J. Hydraulic Engineering* | on a smooth stepped spillway: **nappe flow for d_c/h under about 0.5**, a transition to about 0.9, skimming above | a summary of the paper by a search; the numbers as quoted there |
| Comiti, Mao and others on velocity in step-pool reaches; Grant, Swanson & Wolman (1990), *GSA Bulletin* 102 (stepped beds in the western Cascades) | the spacing of steps in widths and a mean speed at low flow | not reached: the Oregon archive's scan has no text layer, and nothing else was found tonight |

## What it says about the forest's line, in numbers

The owner's line: q = 21 l/s over about a metre, so the critical depth is
d_c = (q² / g)^(1/3) = (0.021² / 9.81)^(1/3) = **0.0356 m**. Its drops, 0.3 to 4 m, give
**d_c / h = 0.009 to 0.12**: deep in the nappe regime, a factor of four under the onset of the
transition on any of the correlations above. So:

- **Every drop of 0.3 m or more at this flow is a free nappe** that falls onto the pool below and
  breaks there; none is a sheet sliding down a face, and none is a skimming chute. A drop under
  about three critical depths (10 cm here) is not a fall at all: the water stays on the bed (the
  falls' own test, [waterfalls](waterfalls.md)). The solve's lip-to-foot pairs are therefore the
  right description, and **what a pair needs is water drawn as a fall**, never a gap.
- **Steps take most of the fall, and the water between them stands.** With S ≤ H/L ≤ 2S the steps'
  heights add up to between all of the reach's drop and twice it (a step's pool lies under the lip
  of the step below it), and a measured Swiss reach put 60 % of its drop at its steps. Between two
  steps the water is a pool: level, or nearly, over its length. A 6 cm ribbon laid parallel to a
  37 % bed is the wrong form; a flight of small pools joined by nappes is the right one.
- **A pit on a line is a pool.** A pool's surface stands at its outlet's sill and is as wide as that
  level meets the ground; the water leaves over the sill. A 5 mm film down into a pit and up its far
  side is water going uphill, which no source describes.
- **A cascade (above about 8 %) has its pools under a width apart**, a step-pool reach one to four
  widths. On the forest's 37 % line, a metre wide, that is a step every metre or two where the bed
  has the clasts to build them; the ground's lattice there (the solve's 25 cm, the heightfield's
  0.33 to 3 m) holds few of them, and a bed's steps finer than the lattice are the clasts', not the
  ground's.

## The numbers chosen

| Number | Value | From |
|---|---|---|
| A fall's least drop | three critical depths, the critical depth at least 1 cm | the falls' test (S104), unchanged; consistent with nappe flow at d_c/h ≤ 0.33 |
| The regime of a drop | a nappe wherever d_c/h < 0.5 | Wüthrich & Chanson (2014) as summarised; a chute down a face is not drawn below it |
| Reach type by slope | step-pool 3–8 %, cascade 8–30 %, steeper colluvial or bedrock | Montgomery & Buffington (1997), as the reaches already use |

**Labelled a choice**: where a pool's level stands between two neighbouring stations, and the
drawn width of a nappe at a drop the lattice resolves as a single cell.

## What a small flow on a steep bed looks like

(W130's third leg, 2026-11-16.) The question here is the draw's, not the solve's: what a person
standing beside 5 to 50 l/s on a 30 to 40 % bed sees, so that the sheet the solve hands the
renderer reads as that.

**What was read tonight, and how far.**

| Source | What it gives | Read |
|---|---|---|
| Montgomery & Buffington (1997), as above | a cascade (about 8 to 30 %): "tumbling flow", the water over and around individual large clasts, with jet-and-wake flow and pools under a channel width apart; a step-pool reach (3 to 8 %) its water dropping over the steps into the pools | full text (for W86 and W91) |
| Comiti, Mao, Wilcox, Wohl & Lenzi (2007), "Field-derived relationships for flow velocity and resistance in high-gradient streams", *J. Hydrology* 340, 48–62 | ten step-pool reaches of the Rio Cordon at bed slopes **0.08 to 0.21** and 3 to 80 % of bankfull: the dimensionless unit discharge, then the slope, predicts the speed; at-a-station speed rises with discharge to the power 0.48 to 0.6 | the abstract and a search summary; the PDF was fetched but its text could not be extracted here, so the equations' coefficients and the measured speeds were **not** read |
| Toombes & Chanson; Chanson (1993, 1994) on stepped cascades | stepped cascades are known for a high volume of entrained air; at low discharge and a large step, nappe flow, each step's jet striking the step below; past the inception point the surface is white | a search summary (the notes behind the table above read whole) |
| Ulseth, Hall, Boix Canadell, Madinger, Niayifar & Battin (2019), "Distinct air–water gas exchange regimes in low- and high-energy streams", *Nature Geoscience* 12, 259–263 | two regimes of gas exchange by the energy dissipation rate e_D = g S V: below a break, exchange across a surface with no bubbles; above it, turbulence draws air into bubbles in proportion to e_D. **The break, 0.020 m²/s³, is recalled, not read**: the summaries found tonight state the two regimes without the number | a summary of the paper and two reviews of it; the paper itself not reached (its open copy asked for a browser check) |
| Cummings & Chanson, as the channel's aeration already cites | self-aeration's inception at 0.74 to 1.58 m/s | as cited in [flowing-water](flowing-water.md) |

**What it says, at the owner's line's numbers.** 21 l/s down a 37 % bed: a reach-averaged solve
gives a mean speed of 0.4 to 0.7 m/s at 4 to 10 cm (the torrent's stations, W120's section
resistance), so a mean Froude number of 0.3 to 0.9, subcritical. That is consistent with the step-pool
literature, whose reach-mean speeds at low flow are small because most of the head is spent at the
steps; but the water is not slow anywhere a person looks at it: over each step it falls freely
(nappe flow, above) and strikes the pool or the step below at √(2 g h), 1.4 m/s for 10 cm and
2.6 m/s for 35 cm, past the 1 m/s that self-aeration needs, so **every step's foot is white water,
and on a cascade, whose steps are under a width apart, that is most of the surface**. In e_D terms
the line dissipates g × 0.37 × 0.5 ≈ 1.8 m²/s³, two orders over the bubbly regime's break, where a
gentle reach of 1 % at 0.3 m/s (0.03) sits at it and a pool (0) far under. So what is seen beside
such a stream is a white, aerated thread broken over the stones with dark water between, never a
transparent film and never a level ribbon; a pool between two steps is the one place it is clear.

**What the draw did with it before this leg** (`content/test-scenes/torrent`, link 11, the 22
stations below its 2.4 m fall; printed from the solved description and the shader's own rules): the
pixel's Froude number (the section's times the vertical's share) 0.22 to 0.90 at every station, so
the Froude rule's white was **zero everywhere**; the speed's fade (0.8 to 1.2 m/s of the surface's
speed, which was 0.23 to 0.71) zero again; and the body's calm quarter metre, which multiplies the
ripples' height and the white, 0.17 to 0.38, which left a ripple of 0.02 to 0.15 mm: **a clear,
glassy sheet**, which from eye height looking down at a bed of the same colour is the bed. The
shore's fade over 3 cm of depth, which the coordinator suspected, took nothing off the thalweg
(4 to 9 cm deep there), only off the edges; on a stream of 1 to 3 cm across its section it would
have taken the sky off the whole of it, so it is mended too.

**The rule chosen** (`water::channel_broken_share`, mirrored by `water.slang`'s
`water_channel_broken_share`; [water](../subsystems/water.md#shading)): the share of the surface
drawn white is `0.6 × s(0.03, 0.30, S) × s(0.02, 0.2, g S V) × s(1.5 mm, 1 cm, d)`, with s the
smooth step, S the **sheet's own fall along the flow** (the solved levels' slope, which a pool does
not have), V the solved section speed and d the water under the pixel; in the draw, times the section's Froude number over
[0.25, 0.6] so a pool stays clear, densest in the thalweg (times the vertical's share of the flow
to the power 1.5, so the margins are dark water), in threads of a clast's size, 25 cm, and a third
of it, drawn out along the flow by the distance the surface travels in half a second and carried at
the surface's speed;
the larger of it and the Froude rule's white wins. At the 22 stations of link 11 printed above (their
levels falling 18 to 67 %) it gives 0.34 to 0.60 of the surface, 0.57 on the mean, where the Froude
rule gave none. Every input is the station's own solved quantity,
which an authored line carries as well (its levels, its speed, the ground under it): nothing names
a scene, a generator or the forest.

| Number | Value | From |
|---|---|---|
| No white below | S = 3 % | the step-pool reach's onset, Montgomery & Buffington (1997) |
| The most white from | S = 30 % | the cascade's upper end, the same |
| The most white | 60 % of the surface | **a choice**: a cascade at low flow is mostly white with dark water between the clasts; 85 % was tried first and drew the torrent's reach as an unbroken white band, paint rather than water |
| The energy to entrain air | g S V over 0.02 to 0.2 m²/s³ | the lower end Ulseth et al. (2019)'s break, recalled; the upper a choice, ten times it |
| The water it needs | 1.5 mm to 1 cm | the veil's two depths (S108), a choice there |
| The cells | 25 cm | **a choice**: a clast's size in a steep small stream; Chin (1999) has the clasts set the steps' height |

**What it still is not.** It is white drawn on the solve's sheet, not the form: the sheet stays a
plane at the reach's mean fall, and the steps and pools the literature says carry the drop are not
in its geometry ("The step-pool form, solved or drawn", below). Nor is the white lit as the aerated
volume it is: it is the channel's foam, a diffuse surface of albedo 0.8.

## The step-pool form, solved or drawn

(The next row's start; written by W130's third leg, 2026-11-16, and not built.) A steep reach's form
is a flight of pools at their sills, each spilling as a nappe into the next, the steps one to four
widths apart on a step-pool reach and under a width on a cascade. Two ways to put it in front of
the eye:

- **Solved stations.** The solve writes, along a reach steeper than about 3 %, a lip and a foot at
  every step and a level pool between, as it already writes a fall and (W130.2) a pit's pool: the
  stations' levels are a staircase, and the sheet, the nappe, the depth map, the water query and a
  walker's wading all read the same staircase. Its spacing comes from Montgomery & Buffington's
  widths and Abrahams, Li & Atkinson's S ≤ H/L ≤ 2S, seeded from the plan so it is tile-stable; its
  heights must sum to the reach's drop (60 % of it at the steps, the Swiss reach). The cost: the
  stations are three to ten times as many on a steep reach (a step every 1 to 2 m of a metre-wide
  stream, where the solve now writes a station every 0.5 to 1 m anyway), a lip-to-foot pair per step
  for the falls' cook, and the ground must agree, so the cut has to carve the steps the stations
  stand on, or the pools stand in the air over a smooth bed.
- **Drawn detail.** The solve stays a mean plane, and the draw makes the steps: the sheet's level
  is given a periodic staircase about the solved one inside the mesh builder, with the white and
  the nappes at each riser. It is cheaper and needs no re-plan, but nothing else reads it — the
  water query, a body's wading and the ground's cut stay on the plane — and a staircase that the
  ground under it does not have shows the plane's bed through every pool.

**The first is the right shape**, and the second only its stand-in: a step is where the head is
lost, which is the solve's business, every reader of the water should see the same steps, and the
cut can carve what the plan names. The leg that builds it starts from the falls' cook (S104), which
already turns a lip-to-foot pair into a nappe, and from W130.2's pool, which already stands water
level at a sill; what it adds is the spacing and the carving.
