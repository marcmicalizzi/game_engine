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

**What was read, and what was not.** Ferguson (2007) and Rickenmann and Recking (2011) themselves
were **not read**: both are open on the publisher's site, and the publisher and Durham's repository
refused every request (HTTP 403, twice, on 2026-11-16). Their constants are checked instead against
a **full text that restates both equations**, Mendicino and Colosimo (2019, *Water Resources
Research* 55, doi 10.1029/2019WR024819, its equations 13 and 14), so the VPE's 6.5 and 2.5 and
Rickenmann and Recking's modified form are verified against a careful reading of the sources, not
against the sources; **the ranges each of those two was fitted over remain unread.** Jarrett's law
was read in Jarrett's own words: his USGS report of the next year (WRI 85-4004, 1985, pp. 34–35),
which gives the same equation in feet with its limits. What each row rests on is said plainly.

| Source | What is taken | How far read |
|---|---|---|
| Jarrett (1984), "Hydraulics of high-gradient streams", *J. Hydraulic Engineering* 110(11); **Jarrett (1985), "Determination of roughness coefficients for streams in Colorado", USGS WRI 85-4004** | n = 0.39 S^0.38 R^-0.16 with R in feet (0.32 in metres: 0.39 x 0.3048^0.16 = 0.322), from current-meter measurements at **21 higher-gradient sites in Colorado**, standard error 28 %; "applicable within a range of slopes from 0.002 to 0.04 and for hydraulic radii from 0.5 to 7 ft" (**0.15 to 2.1 m**), with caution asked above 0.04 "because the applicability of the equations is questionable"; cobble and boulder beds | The 1984 paper not read; **the 1985 report read in its own text** (pubs.usgs.gov, pp. 34–35, the equation, its error and its four limitations). The engine's clamp of S to 0.09 is beyond the stated 0.04 |
| Ferguson (2007), "Flow resistance equations for gravel- and boulder-bed streams", *Water Resources Research* 43, W05427 | **The variable-power equation (VPE)**: V / u* = a1 a2 (R / D84) / sqrt(a1^2 + a2^2 (R / D84)^(5/3)), u* = sqrt(g R S), **a1 = 6.5, a2 = 2.5 with the roughness length D84**; asymptotic to a roughness-layer law, V / u* = a2 R / D84, in shallow flow and to a Manning–Strickler law, V / u* = a1 (R / D84)^(1/6), in deep flow | The paper refused (above). **The form and the constants checked in the full text of Mendicino and Colosimo (2019)**, equation 13: the friction factor's sqrt(8/f) = b1 b2 (R / d84) / sqrt(b1^2 + b2^2 (R / d84)^(5/3)), "with b1 = 6.5 and b2 = 2.5 (Ferguson, 2007), which is suitable for all relative submergence values", d84 the roughness length — the same as the code's. Ferguson's own data ranges were not read |
| Rickenmann and Recking (2011), "Evaluation of flow resistance in gravel-bed rivers through a large field data set", *Water Resources Research* 47, W07538 | 2,890 field measurements; of the conventional equations **the best overall performance is Ferguson's**, the two power laws for deep and shallow flow joined; the speed in steep shallow flow goes with about the 0.6 power of the unit discharge | The paper refused (above). **Their modified VPE read in Mendicino and Colosimo (2019)**, equation 14: V / u* = 4.416 (R / D84)^1.904 / (1 + (R / (1.283 D84))^1.618)^1.083. Computed here against Ferguson's at R / D84 of 0.15, 0.3, 0.5 and 1: 0.12, 0.40, 0.95, 2.54 against 0.37, 0.74, 1.22, 2.33 — **slower than Ferguson's by a third to two thirds over the forest's 0.15 to 0.5**, the same above 1. Not used (the speed it gives a steep shallow stream is lower still); it says the VPE is the faster of the two fits where the forest's streams are. Their discharge-based equation was not read |
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
and every bed over 9 % took the n of a 9 % bed — itself past the 4 % Jarrett's own report sets as
the law's upper limit, for want of data on steeper streams. At S over 0.09 that is n = 0.32 x 0.400 x 1.355 = 0.174,
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

## Measured: the old law against the new

`engine-content flow solve` at 1012756b (`msvc-debug`, one thread, CPU only; the machine shared with
other rows' builds and a test run), the same binary with `--resistance jarrett` (the old default,
unchanged) and with the new default. The speeds are `runs_by_slope`'s: drawn stretches by length.

**`content/test-scenes/torrent`** (the plan of 2026-11-15; 8 wet lines, 0.23 km over 30 %, one short
line of 15 to 30 %):

| Stage | Class | Law | Speed p10 / median / p90 m/s | Depth median m | Over a bank (stations, all lines) |
|---|---|---|---|---|---|
| 1 | over 30 % | Jarrett | 0.31 / 0.46 / 0.52 | 0.09 | 5 |
| 1 | over 30 % | **Ferguson** | 0.24 / **0.35** / 0.40 | 0.10 | 9 |
| 2 | over 30 % | Jarrett | 0.39 / 0.57 / 0.65 | 0.12 | 10 |
| 2 | over 30 % | **Ferguson** | 0.36 / **0.50** / 0.57 | 0.13 | 11 |

Every line's mean n went from Jarrett's clamped 0.17 to an equivalent 0.21 to 0.28: the plan's
clasts on a 40 % torrent bed stand as high as its water is deep. Falls, pools and runs per 100 m
are unchanged (4 falls, 3 pools, 3.04 runs at stage 1).

**The forest's world (v47), whole, at stage 1.** On main a root's first solve of v47 cooks the cut's
tile set once (78c0c9419087090a, about 37 GB of commit at its peak, the neighbouring row W131 found);
a root that already holds it solves in 166 to 256 s at 2.44 GB, as last night's did. Solved so,
the same binary (1012756b) with each law:

| Class | km | Law | Speed p10 / median / p90 m/s | Depth median m | Film share | Runs / 100 m | Pieces / 100 m | Falls (drawn) | Gaps |
|---|---|---|---|---|---|---|---|---|---|
| under 2 % | 4.3 | Jarrett | 0.06 / 0.12 / 0.26 | 0.19 | 0.01 | 1.11 | 1.11 | 0 | 7 |
| under 2 % | 4.3 | **Ferguson** | 0.03 / **0.13** / 0.23 | 0.19 | 0.01 | 1.20 | 1.20 | 0 | 8 |
| 2 to 5 % | 2.4 | Jarrett | 0.07 / 0.14 / 0.32 | 0.12 | 0.00 | 0.89 | 0.89 | 1 (1) | 3 |
| 2 to 5 % | 2.4 | **Ferguson** | 0.09 / **0.14** / 0.28 | 0.12 | 0.00 | 0.66 | 0.66 | 0 | 2 |
| 5 to 15 % | 5.1 | Jarrett | 0.13 / 0.25 / 0.37 | 0.07 | 0.00 | 0.72 | 0.86 | 7 (7) | 4 |
| 5 to 15 % | 5.2 | **Ferguson** | 0.11 / **0.19** / 0.28 | 0.09 | 0.00 | 0.74 | 0.82 | 6 (6) | 6 |
| 15 to 30 % | 8.3 | Jarrett | 0.10 / 0.29 / 0.40 | 0.07 | 0.00 | 0.98 | 1.41 | 43 (41) | 22 |
| 15 to 30 % | 8.5 | **Ferguson** | 0.10 / **0.22** / 0.29 | 0.08 | 0.00 | 1.12 | 1.60 | 54 (51) | 32 |
| over 30 % | 6.2 | Jarrett | 0.17 / 0.38 / 0.60 | 0.06 | 0.02 | 3.32 | 10.66 | 542 (527) | 136 |
| over 30 % | 6.1 | **Ferguson** | 0.07 / **0.26** / 0.39 | 0.08 | 0.02 | 3.52 | 9.86 | 459 (448) | 147 |

Over the whole world: 263 of 766 links wet and 26.6 km solved under both; falls 593 to 519, jumps
56 to 66; **stations over a bank 1,623 on 121 lines to 3,448 on 158 lines** (the worst 6.5 m to
8.1 m); pools 407 to 492 stations, open pools 695 to 1,202.

What the deeper, slower sheets do to the breaks: the film share does not move (2 % over 30 %, the
films are W130's buried and thin stations, which a centimetre more does not lift), the runs a 100 m
are within a tenth either way, and over 30 % there are fewer pieces and fewer falls (83 fewer; the likely reading,
not traced station by station, is that a deeper sheet behind a lip drowns a small drop). The cost is
elsewhere: the 15 to 30 % class gains ten gaps and eleven falls, and **twice as many stations stand
over a bank** — a deeper sheet in the same cut reaches the cut's lip twice as often. Those are the
stations row W138 (the water walls and raised slabs) and W131 (the section under the line) work on;
a law that makes the water deeper makes their faults commoner until they are mended, and the
hand-back says so.

Before the whole world was solved, the forest session's own small scene of `region link 515` (`reports/quality-v47/parts/stream-scene`, the world's ground
there bit for bit, the plan 780a7a78621fb16f), solved in a copy whose tile root is this row's; only
the nine lines wholly on its ground are counted (0.53 km over 30 %, 0.23 km at 2 to 5 %):

| Line | Slope | Q l/s | Jarrett n, mean speed, mean depth | Ferguson n, mean speed, mean depth |
|---|---|---|---|---|
| region link 515 | 0.45 | 20.8 | 0.161, 0.46 m/s, 8.1 cm | 0.261, **0.28 m/s**, 14.9 cm |
| region link 483 | 0.44 | 22.0 | 0.148, 0.44, 9.4 cm | 0.210, 0.22, 18.6 cm |
| region link 480 | 0.56 | 24.6 | 0.167, 0.50, 8.0 cm | 0.262, 0.31, 14.1 cm |
| region link 543 | 0.029 | 4.3 | 0.065, 0.13, 9.8 cm | 0.059, 0.13, 8.7 cm |

| Class | Law | Speed p10 / median / p90 m/s | Depth median m |
|---|---|---|---|
| over 30 % | Jarrett | 0.11 / 0.42 / 0.61 | 0.069 |
| over 30 % | **Ferguson** | 0.02 / **0.25** / 0.37 | 0.092 |
| 2 to 5 % | Jarrett | 0.06 / 0.08 / 0.20 | 0.115 |
| 2 to 5 % | **Ferguson** | 0.08 / **0.10** / 0.18 | 0.100 |

(Counted by `inside.ps1` in the row's scratch from the solved blocks, the drawn rule approximated:
not a lip-to-foot pair and not both stations a 5 mm film.) The gentle reaches here are a pool's and
a mouth's, held by the level they meet, so the law moves them little.

**What this says about the owner's complaint.** The resistance law is a small part of the answer and
on the steep lines it points the other way: by the only law with data at these depths, 21 l/s on a
45 % bed of the plan's clasts runs at a mean of about 0.3 m/s, 15 cm deep, slower and deeper than
Jarrett's clamp had it. The forest session's 0.10 to 0.40 m/s medians are about what a small mountain
stream's reach-mean is. What makes a steep stream look fast is the water an eye follows over its
steps, and that is the draw's: the pattern travels at 1 to 2.2 m/s on a 10 to 30 % sheet (above),
whatever the mean. Deeper water helps the W130 count (a thicker sheet is less often a 5 mm film) and
costs the torrent four more stations over a bank at stage 1 (and the forest twice the stations over
a bank, above). The motion was measured and looked at next.

## Measured: the motion

**How.** `engine-view` at 1012756b (`msvc-debug`, `--shadows rt`, 1920x1080, `--time-of-day 13`,
offscreen, so each frame is a sixtieth of a second of the world clock), 120 frames with
`--capture-every 2`: 60 frames over 2 s. The torrent as the last batch textured it (b66's judging
variant) with each solve's channels: **before**, the Jarrett solve drawn by this binary given
d6534963's `water.slang` compiled into a manifest of its own (`--shaders`; nothing else in the
shaders differs between the two commits), so the pattern travels at the surface's speed as on main;
**after**, the Ferguson solve and this branch's shader. The camera stands 3.5 m beside `torrent link
11` at its station 15 (a 30 % sheet; the solve's mean there 0.40 to 0.62 m/s by Jarrett, 0.34 to
0.42 by Ferguson, about 10 cm deep), 1.7 m over the ground, looking across the flow (45 degrees of
field; the flow 0.99 perpendicular to the line of sight), 4.3 m from the water: 3.3 mm a pixel.
The tracker (the row's scratch, `motion\track.ps1`) removes what does not move (the mean of the 60
frames: the bed seen through the water, the banks) and correlates a 400 x 120 window of each frame
with the next over shifts of 40 pixels, summing the 59 surfaces.

| | Shift a frame pair (1/30 s) | **Apparent speed** | Solve's mean there | Correlation at the peak / at zero |
|---|---|---|---|---|
| Before (main's draw, Jarrett) | (5, 2) px, every pair within a pixel | **0.54 m/s** (0.49 to 0.59 at a pixel's error) | 0.40 to 0.62 m/s | 0.72 / 0.56 |
| After (the travel, Ferguson) | (21, 8) px | **2.25 m/s** (2.20 to 2.30) | 0.34 to 0.42 m/s | 0.53 / 0.15 |

The after figure is the formula's to within the measurement: sqrt(0.4^2 + 2 x 9.81 x 0.25 x 1) =
2.25 m/s on a sheet whose own slope gives the full weight. A second window, lower on the reach,
gives (4, 1) and (17, 5) px (0.42 and 1.77 m/s at the centre's scale, which is not quite its own):
the same fourfold step.

**Looked at** (six frames of each, the first three consecutive, the middle and the last two; the
frames and the contact sheets are in the row's scratch, `motion\`): **it reads as fast water now**,
where before it read as a slow glide: between two frames a thirtieth of a second apart the white
moves a blob's width downstream, and the bed under it stays put, so the eye separates the water from
the stones at once. What a person would still object to, in the order they would see it:

1. **The whole sheet slides as one.** Every part of the section, edge and middle, carries its
   pattern at the same 2.25 m/s; the white is the same soft round blobs, rigid, sliding like a
   texture on a belt. A real torrent's fast water is threads and tongues (the jets over the steps),
   streaked along the flow, beside slower water at the edges and in the lee of stones; the travel
   here has the jets' speed but not their shape or their place. The weight w is the sheet's slope's,
   not a step's, so on a uniformly steep reach there is no slow water at all.
2. **The white is faceted**: hard-edged triangles, one share a triangle (row W139's first fault,
   not this row's); the Ferguson solve, slower and deeper, draws less of it than Jarrett's did.
3. **No relief.** The surface is a flat film over the pebbles; nothing stands over a stone and
   nothing tumbles at a step. The pattern's speed is the only cue that the water is fast, and on a
   real stream it is the least of them.
4. **The speed is a lie of continuity**: 10 cm of water at a mean of 0.35 m/s cannot have its whole
   surface moving at 2.25 m/s. Over a step's jet it can; across the sheet it cannot, and a person who
   watches a pool-less reach for long enough may read it as a film sliding over water.

So the draw now answers "not fast enough" for the eye at a glance, and the next step for realism is
the jets' place and shape (threads that start at a step or stone and fade in its pool, the step-pool
form of W132), not a faster mean.

## Numbers pinned, each against its source

| Number | Value | Source |
|---|---|---|
| VPE a1 | 6.5 | Ferguson (2007), as restated in the full text of Mendicino and Colosimo (2019), eq. 13; Ferguson's text itself unread |
| VPE a2 | 2.5 | the same |
| Jarrett's range | S 0.002 to 0.04, R 0.15 to 2.1 m | Jarrett (1985), USGS WRI 85-4004, read; the engine's clamps (S 0.09, R 0.15) are what the old law used and are kept as they were |
| VPE roughness length | D84 of the bed's surface | the same |
| u* | sqrt(g R S_f), the friction slope in place of S in a varied profile | the textbook's reading of a resistance law in the standard step method ([stream-profile](stream-profile.md)) |
| A line with no plan's bed | D84 from the plan's own sediment law at the section's own bank height and slope | `ChannelSediment`'s rules, [channels](../subsystems/channels.md#bed-sediment) |
| Jet height H | 0.25 m at a full cascade's weight | a choice, the clasts' scale (`k_channel_clast_m`) |
| Jet weight | smoothstep(0.03, 0.30, slope) | Montgomery and Buffington (1997)'s step-pool and cascade slopes, as the broken share takes them |
