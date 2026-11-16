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
