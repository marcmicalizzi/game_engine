# How fast a small stream runs, and what the eye reads of it (W137)

The owner, walking the forest's streams at twice the flow (2026-11-16): "the water doesn't feel like
it's flowing fast enough". The forest session's numbers (its need 146): solved runs move at a median
0.10 m/s under 2 %, 0.2 at 2 to 5 %, 0.3 at 5 to 30 % and 0.40 m/s over 30 %, at stage 1 and 2 alike,
because Jarrett's law gives n = 0.17 on most of these small sections; Manning's 0.04 gives 0.42 to
1.15 m/s at half the depth and a stream that is white from end to end.

This page asks two questions and keeps them apart, because they have different answers:

1. **What is the mean speed of 5 to 200 l/s on a 2, 10 and 40 % bed?** That is the resistance law's,
   the solve's (`domain/channels`, [channels](../subsystems/channels.md#the-profile)).
2. **What speed does an eye read off such a stream?** That is the draw's: the speed the surface's
   pattern travels at ([water](../subsystems/water.md#the-surface-from-the-solve)).

## Sources

Tonight's web access reached abstracts and search summaries, not papers: every full text tried
(Durham's repository, Wiley, the authors' PDFs) was refused or came back as a PDF this machine could
not turn into text. What each row rests on is said plainly.

| Source | What is taken | How far read |
|---|---|---|
| Jarrett (1984), "Hydraulics of high-gradient streams", *J. Hydraulic Engineering* 110(11) | n = 0.32 S^0.38 R^-0.16 (metres), fitted on 75 measurements in 21 Colorado streams, **R 0.15 to 2.1 m**, S 0.002 to 0.09 (or 0.002 to 0.04 and R to 1.68 m, by the other quotation) | As on [stream-profile](stream-profile.md): the abstract and two secondary quotations. Not opened again tonight |
| Ferguson (2007), "Flow resistance equations for gravel- and boulder-bed streams", *Water Resources Research* 43, W05427 | **The variable-power equation (VPE)**: V / u* = a1 a2 (R / D84) / sqrt(a1^2 + a2^2 (R / D84)^(5/3)), u* = sqrt(g R S), **a1 = 6.5, a2 = 2.5 with the roughness length D84**; asymptotic to a roughness-layer law, V / u* = a2 R / D84, in shallow flow and to a Manning–Strickler law, V / u* = a1 (R / D84)^(1/6), in deep flow | The record and the abstract by search; **the two constants are confirmed by a search summary of a text by the same author** ("suggested coefficient values a1 = 6.5 and a2 = 2.5 when k is equated with D84"), where [water-obstacles](water-obstacles.md) had them from memory. The paper's figures and its own data ranges were not read |
| Rickenmann and Recking (2011), "Evaluation of flow resistance in gravel-bed rivers through a large field data set", *Water Resources Research* 47, W07538 | 2,890 field measurements; of the conventional equations **the best overall performance is Ferguson's**, the two power laws for deep and shallow flow joined; the speed in steep shallow flow goes with about the 0.6 power of the unit discharge | Abstract by search. Their own discharge-based equation's constants could not be confirmed against any page and are **not used** |
| Bathurst, Li and Simons (1981); Bathurst (1985) | The flow classes by relative submergence: large-scale roughness at d / D84 under about 1.2, intermediate 1.2 to 4, small-scale over 4 where the boundary-layer laws hold; below about 10 the friction factor rises above the logarithmic law's | A search summary quoting the classes. Not opened |
| Aberle and Smart (2003), "The influence of roughness structure on flow resistance on steep slopes", *J. Hydraulic Research* 41 | At low submergence on steep slopes the bed's datum is hard to define, the resistance depends on the roughness's arrangement (the standard deviation of the bed's heights) more than on a grain size, and no standard equation exists | A search summary. Not opened. Nothing pinned |
| Comiti, Mao, Wilcox, Wohl and Lenzi (2007), "Field-derived relationships for flow velocity and resistance in high-gradient streams", *J. Hydrology* 340 | Ten step-pool reaches of the Rio Cordon at slopes **0.08 to 0.21**, 3 to 80 % of bankfull, speeds by salt dilution: the dimensionless unit discharge predicts the speed best, then the slope, then the steps' steepness; at a station the speed rises with discharge to the power 0.48 to 0.6 | The abstract (the university's page, tonight). The measured speeds were not read |
| Lee and Ferguson (2002), "Velocity and flow resistance in step-pool streams", *Geomorphology* 46 | Speed and resistance vary strongly with discharge in step-pool reaches, field and flume | The abstract by search only. Nothing pinned |
| Wilcox and Wohl (2006); Comiti et al. (2007), as [water-obstacles](water-obstacles.md) quotes them | **On a step-pool most of the resistance is spill**, the tumbling over steps and the jumps behind them, not grain friction | Abstracts by search (an earlier row's) |
| Montgomery and Buffington (1997); Lamb, Dietrich and Venditti (2008); Bunte and Abt (2001) | The bed's D84 a point of the plan carries (`ChannelSediment`, [channels](../subsystems/channels.md#bed-sediment)), which this row reads and does not change | Read in full by W97 |
| Free fall, Torricelli | Water leaving a lip at v0 and falling a height H reaches sqrt(v0^2 + 2 g H) | Textbook mechanics |

**Not found tonight**: a published ratio of the surface's speed to the mean in shallow, rough,
steep flow (the float coefficient), measured mean speeds tabulated by slope at a few litres a second,
and the step height of small steep streams as a number (Chin 1999 says it is set by the clasts; the
page that took it, [steep-streams](steep-streams.md), did not read a value either).

## The mean speed: Jarrett's law was outside its data on every small stream

Jarrett fitted his n on hydraulic radii of 0.15 m and more. The engine held R to that range **inside
the power and nowhere else**: n = 0.32 S^0.38 max(R, 0.15)^-0.16, and S to at most 0.09. A forest
stream of 5 to 50 l/s has R of 2 to 8 cm, so every one of its sections took the n of a 15 cm radius;
and every bed over 9 % took the n of a 9 % bed. At S over 0.09 that is n = 0.32 x 0.400 x 1.355 = 0.174,
the forest session's 0.17. Below that the slope's power still acts, so a 2 % bed took n = 0.098 at any
depth under 15 cm — the roughness of a boulder bed, on a gravel brook. That is why the speeds were
low on the gentle reaches, where the law's error is largest: Jarrett's n stands for the steep
streams of his data, and the clamp gave a 2 % brook a third of a 9 % torrent's n regardless of what
the brook's bed is made of.

**The law that has data at these depths is Ferguson's.** Its variable is the relative submergence
R / D84, a property of the section itself (its depth) and of its bed (the clasts the plan lays), and
both are the section's own: nothing is keyed on a scene, a slope class or a generator. In shallow
flow it is the roughness layer's law, V = 2.5 (R / D84) sqrt(g R S): the speed rises as R^1.5, falls
as the clasts grow, and goes with the square root of the friction slope. In deep flow it is
Manning–Strickler's: V / u* = 6.5 (R / D84)^(1/6) is Manning's law with n = D84^(1/6) / (6.5
sqrt(g)) = 0.049 D84^(1/6), 0.030 on a 5 cm gravel and 0.036 on a 15 cm cobble bed — Chow's gravel
and cobble rows. It needs no clamp: it holds from R / D84 of about 0.1 to the deep law.

What it says for the speeds asked about (a rectangle of the width a small stream has, D84 as the
plan's sediment gives a bed of that slope; Ferguson's law against the old clamped Jarrett, both in
uniform flow):

| Bed | D84 | Q l/s | Width m | VPE depth m | **VPE speed m/s** | Froude | R / D84 | Jarrett depth m | Jarrett speed m/s |
|---|---|---|---|---|---|---|---|---|---|
| 2 % | 0.05 | 5 | 0.4 | 0.06 | **0.21** | 0.27 | 0.93 | 0.07 | 0.19 |
| 2 % | 0.05 | 20 | 0.7 | 0.08 | **0.34** | 0.38 | 1.34 | 0.11 | 0.27 |
| 2 % | 0.05 | 50 | 1.0 | 0.10 | **0.48** | 0.47 | 1.72 | 0.15 | 0.34 |
| 2 % | 0.05 | 200 | 1.8 | 0.15 | **0.76** | 0.63 | 2.51 | 0.23 | 0.48 |
| 10 % | 0.15 | 5 | 0.4 | 0.07 | **0.19** | 0.23 | 0.34 | 0.06 | 0.23 |
| 10 % | 0.15 | 20 | 0.7 | 0.09 | **0.31** | 0.33 | 0.48 | 0.09 | 0.32 |
| 10 % | 0.15 | 50 | 1.0 | 0.11 | **0.44** | 0.42 | 0.61 | 0.13 | 0.40 |
| 10 % | 0.15 | 200 | 1.8 | 0.15 | **0.73** | 0.60 | 0.87 | 0.20 | 0.55 |
| 40 % | 0.30 | 5 | 0.4 | 0.07 | **0.19** | 0.23 | 0.17 | 0.04 | 0.35 |
| 40 % | 0.30 | 20 | 0.7 | 0.09 | **0.32** | 0.34 | 0.24 | 0.06 | 0.49 |
| 40 % | 0.30 | 50 | 1.0 | 0.11 | **0.45** | 0.43 | 0.30 | 0.08 | 0.62 |
| 40 % | 0.30 | 200 | 1.8 | 0.15 | **0.74** | 0.61 | 0.43 | 0.13 | 0.85 |

(Computed tonight; the D84 per slope is illustrative, the plan's own is measured below.)

Two things follow, and both agree with the field work quoted above:

- **The mean speed of a small stream is nearly the same at every slope**, 0.2 to 0.75 m/s from 5
  to 200 l/s, because a steeper bed carries larger clasts and in the roughness layer the clasts take
  back what the slope gives (V goes as sqrt(S) / D84). That is Rickenmann and Recking's and
  Comiti's finding in words: the dimensionless discharge, not the slope, sets the speed, and it goes
  with about the 0.5 to 0.6 power of the discharge (the table's 5 to 200 l/s: 0.6 power at 2 %,
  0.36 at 40 %, since the width grows too).
- **A gentle reach runs faster than it did** (up to 1.6 times at 2 %), and **a steep reach slower**
  (0.55 to 0.85 times over 30 %), deeper by half. The law is not what makes a torrent look fast;
  Manning's 0.04 did, and only by drawing a bed of sand under it.

What a reach-averaged law cannot say: on a step-pool or a cascade most of the resistance is the
steps' spill, and the VPE's data took the spill in. Where the engine's bed draws a step and the
solve makes it a fall (a lip and a foot), the fall's drop is taken out of the friction there, and
the VPE between the steps counts the spill again. How much depends on how many of a reach's steps
the lattice resolves (the forest's 0.25 m lattice resolves the large ones: 593 falls over 26.6 km),
and no source read tonight separates grain from spill resistance at these submergences; the law is
used as the whole, and this page says the steep reaches it gives are, if anything, a little slow.

## What the eye reads: the threads, not the mean

A mean speed of 0.3 m/s on a 40 % bed is a mean over a flight of steps and pools: water that falls
H over each step reaches sqrt(v^2 + 2 g H) at its foot — 1.4 m/s over 10 cm, 2.2 m/s over 25 cm,
3.1 m/s over 50 cm — and is all but still in the pool below. **What a person follows on a steep
stream is the jets and the white threads they carry**, born at each step and stone and travelling
at the jet's speed until the pool takes them (the reels' `snowmelt-boulder-rapid`: tongues into
white holes that clear within a metre or two; shapes, never speeds, since no shot has a scale or a
known rate). The pool between them reads still because it is.

So the draw carries the surface's pattern — the advected detail and the white threads, which ride
the same two flow-map copies — at the **travel speed** (`water::channel_travel_speed`, mirrored by
`water.slang`'s `water_channel_travel_speed`):

    travel = sqrt(surface^2 + 2 g H w),   w = smoothstep(0.03, 0.30, the sheet's own slope)

with `surface` the solve's speed at the pixel's surface (the section's mean, the vertical's share of
it, times 7/6) and H the height a thread falls between its stones, `k_channel_jet_m` = 0.25 m — the
clasts' scale the white threads are already drawn on (`k_channel_clast_m`; Chin 1999 via
[steep-streams](steep-streams.md): the steps' height is set by the clasts, a few decimetres; **a
choice** where no number was read). `w` is the step-pool's weight the broken water already uses
(Montgomery and Buffington's 3 % step-pool to 30 % cascade), so a level pool (w = 0) travels at its
own speed and a gentle reach at its surface's, unchanged. On a 10 % sheet w = 0.17 and a 0.3 m/s
surface travels at 0.95 m/s; on a 30 % sheet 2.2 m/s.

What does not move: the depth, the Froude number and every rule that reads them (the standing waves,
the Froude rule's foam, the broken share's gates) keep reading the solve's own speed. Only what the
pattern is carried at changes, so the stream "keeps its solved depth and looks as fast as it is
where the eye follows it" (need 146 b).

## Numbers pinned, each against its source

| Number | Value | Source |
|---|---|---|
| VPE a1 | 6.5 | Ferguson (2007), confirmed by a search summary of his own later text |
| VPE a2 | 2.5 | the same |
| VPE roughness length | D84 of the bed's surface | the same |
| u* | sqrt(g R S_f), the friction slope in place of S in a varied profile | the textbook's reading of a resistance law in the standard step method ([stream-profile](stream-profile.md)) |
| A line with no plan's bed | D84 from the plan's own sediment law at the section's own bank height and slope | `ChannelSediment`'s rules, [channels](../subsystems/channels.md#bed-sediment) |
| Jet height H | 0.25 m at a full cascade's weight | a choice, the clasts' scale (`k_channel_clast_m`) |
| Jet weight | smoothstep(0.03, 0.30, slope) | Montgomery and Buffington (1997)'s step-pool and cascade slopes, as the broken share takes them |
