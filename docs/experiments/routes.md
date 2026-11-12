# Routes: the grounding of a footpath's cost, grade, tread and wear (W96)

The numbers the routes capability ([routes](../subsystems/routes.md)) is built on, each with the
source it came from, how it was read, and what the layer's default makes of it. The forest proof of
concept's prototype and its research note were leads to these publications, never a source: nothing
here is taken from them.

**How the sources were read.** A research pass on 2026-11-11 opened the Forest Service's design
parameters (a PDF read through its text layer), the HTML edition of the Forest Service's trail
notebook (read through a summarising fetcher, so its quotes are not checked against the raw page and
it has no printed page numbers), Cole (1995) and Ballantyne and Pickering (2015) in full, and Cole's
review as a scan. Tobler's report was opened, but its formula is a figure the pass could not read:
the constants below are therefore from a secondary page and are marked so. Everything the pass could
not open is said under "Not found".

## The cost: Tobler's hiking function

Tobler, W. (1993). *Three Presentations on Geographical Analysis and Modeling*. NCGIA Technical
Report 93-1, University of California, Santa Barbara; the first paper, "Non-isotropic geographic
modeling". The report gives the hiking function as its Figure II, "estimated from empirical data
given by Imhof (1950, pp 217-220)", and the text converts the terrain's slope to a walking velocity.

| Number | Value | Source, and how it was read |
|---|---|---|
| Speed | W = 6 exp(-3.5 \|S + 0.05\|) km/h, S = dh/dx | secondary (the encyclopaedic statement of Tobler's function); the report's own figure not read |
| Fastest | 6 km/h at S = -0.05 (about 2.9 degrees downhill) | the same |
| On the flat | 5.04 km/h | follows from the formula |
| Off a path | times 3/5 | secondary; **not used**: the search runs where a path will be |

**What the search makes of it.** A route is walked both ways, so a lattice step costs the mean of
its two directions' times, `0.6 (exp(3.5 |g + 0.05|) + exp(3.5 |g - 0.05|)) / 2` seconds a metre
(0.6 is 3600 s / 6000 m), rounded once to whole milliseconds; `det::exp`, never the C library's. A
symmetric cost also makes a leg the same path whichever end it is searched from, save for ties,
which the queue's total order settles.

## The grade cap and the tread: the Forest Service's trail classes

US Forest Service, *Forest Service Handbook* FSH 2309.18, §23.11, Exhibit 01, "Hiker/Pedestrian
Design Parameters" (10/16/2008), as printed in the Trail Fundamentals handout, pp. 49-50. Read in
full through the PDF's text layer.

| | Class 1 | Class 2 | Class 3 | Class 4 | Class 5 |
|---|---|---|---|---|---|
| Tread width, non-wilderness single lane | 0-12 in | 6-18 in | 18-36 in | 24-60 in | 36-72 in |
| Target grade | 5-25 % | 5-18 % | 3-12 % | 2-10 % | 2-5 % |
| Short-pitch maximum | 40 % | 35 % | 25 % | 15 % | 5 % |
| Maximum pitch density | 20-40 % | 20-30 % | 10-20 % | 5-20 % | 0-5 % |
| Target cross slope | natural | 5-20 % | 5-10 % | 3-7 % | 2-3 % |
| Maximum cross slope | natural | 25 % | 15 % | 10 % | 3 % |

**What the layer takes.** `grade_cap` 0.25 is class 3's short-pitch maximum: a developed footpath
that a walker reads as a made path, the reels' commonest (a worn dirt tread through turf, r01, r42).
The cap is a hard limit on every step, so the line's long grades fall well under it on the ground
the tests use; it does not hold the class's target grade or its pitch density, which a later
refinement could cost (below). `tread_width_m` 0.6 is inside class 3's 18-36 inches (0.46-0.91 m)
and the reels' measured 0.5 m worn path (r01); the range 0.3 to 1.5 m runs from class 2 to class 4.

**Cross slope: not held.** A tread benched by W98's `Bench` line is flat across its width, a cross
slope of 0; class 3's target is 5-10 % and the notebook's outslope "at least 5 percent". A line has
no cross-slope field (W98's report says the same), so this is a request, not a choice.

## The bench: the Forest Service's trail notebook

Hesselbarth, W., Vachowski, B., Davies, M. A. (2007). *Trail Construction and Maintenance
Notebook*, 2007 edition. USDA Forest Service, Missoula Technology and Development Center,
0723-2806-MTDC. Read in its HTML edition (pages 3, 4, 6, 8, 9 and 12); quotes through a summariser.

| Number | Value | Where | What the layer takes |
|---|---|---|---|
| Full bench | "Trail professionals almost always prefer full-bench construction"; a partial bench's fill fails | page 8 | the tread's top is the **lowest** ground across its width: the whole tread stands on cut ground |
| Backslope | "should match the angle of repose of the parent material"; 1:1 for most soils, 2:1 rock, 1:2 low-cohesion | page 8 | `backslope` 1, range 0.25 to 8 |
| Outslope | "at least 5 percent" | page 9 | not representable (above) |
| Half rule | "no more than half the sideslope grade" | page 4 (crediting IMBA's *Trail Solutions*, 2004) | **not held**: the cost has no term for the side slope; a row for it |
| Average grade | each section "less than 10 percent"; high-use trails 5-10 % | pages 3 and 4 | not held as a rule; the cap and Tobler's cost keep long grades low |
| Climbing turn vs switchback | climbing turns on side slopes of 15 % or less, switchbacks on 15-45 %, to 55 % with retaining walls | page 12 | what the search does (a turn's cost by angle) without a rule; counted, not checked |
| Switchback landing | Y-shaped platform at most 5 % grade; smooth-radius turn 1.5-3 m | page 12 | not built: a landing is a pad edit at the turn; a row for it |
| Steps | riser 150-200 mm, tread 254-305 mm, 2 riser + tread = 635-686 mm | page 12 | stage 4, not done |
| Grade reversals | about every 6-15 m, climbing 3-5 m | page 6 | not built |

The fill slope (`fill_slope` 0.5, 1 on 2) is W98's, grounded on its page
([ground-edits](ground-edits.md)); the notebook gives no fill ratio and the research pass found none
in a trail manual it could open.

## Wear: trampling beside the tread

| Source | Read | What it gives |
|---|---|---|
| Cole, D. N. (1995). Experimental trampling of vegetation. I. Relationship between trampling intensity and vegetation response. *Journal of Applied Ecology* 32, 203-214 | in full | relative cover against passes is best approximated by `Y = A - BX + CX^2` (curvilinear: the first passes do most of the harm); the most resistant type kept over 60 % of its cover after 500 passes, the least resistant 33 % after 25 and 2 % after 500 |
| Ballantyne, M., Pickering, C. M. (2015). The impacts of trail infrastructure on vegetation and soils: current literature and future directions. *Journal of Environmental Management* 164, 53-64 | in full | some differences in vegetation and soil "only apparent within 1 m of the edge of the trail"; others still apparent 20 m off, on wide trails with canopy gaps |
| Dale, D., Weaver, T. (1974). Trampling effects on vegetation of the trail corridors of north Rocky Mountain forests. *Journal of Applied Ecology* 11, 767-772 | **secondary only** | "Vegetation more than 2 m from the edge of a trail is often little affected" |
| Cole, D. N. (about 1987). Research on soil and vegetation in wilderness: a state-of-knowledge review | scan | use and impact on trails are "highly curvilinear"; citing Weaver and Dale (1978), half the vegetation lost after 1,000 passes in grassland and 300 in forest |

**What the layer takes.** `wear_m` 2: the reach over which trampling thins the cover past the
tread's edge, the secondary 2 m and inside the 1 m to 20 m the review spans (a narrow footpath with
no canopy gap is the near end). `tread_cover` 0: the tread itself is bare, as the reels' worn paths
are. **The shape between is a choice, not a fit**: a smoothstep in the distance, which is flat at
both ends. What the sources support is its direction and its curvature in use (the first passes do
the most), not a curve in distance; a walker steps off the tread less the further off it is, and the
smoothstep stands in for that product. A placement multiplies its density, or its height, by it.

## The lattice and the search: stated choices

| Number | Value | Why |
|---|---|---|
| `spacing_m` | 2 m | finer than a tread's turn radius at a switchback (1.5-3 m, above) would cost four times the points for a line the corner-cutting already rounds; 4 m (the prototype's) loses the bench across a gully |
| headings | 16 (the eight neighbours and the eight knight's moves) | a line on the lattice holds any heading within about 13 degrees, so a traverse under a cap is not forced into 45-degree steps |
| `turn_s`, `switchback_s` | 2 s, 30 s | a turn of 45 to 90 degrees costs about 3 m of walking and a switchback about 40 m: enough that a climb is a few long legs, not so much that a leg runs past a better line; no source prices a turn |
| `trodden` | 0.5 | a trodden step at half its time: the later route joins the earlier one wherever the detour costs less than half the shared stretch, which merges branches over a few tens of metres and not across a valley |
| `steep_penalty` | 20 | where no path under the cap exists, a step over it costs twenty times its time, so the steep stretch is as short as the ground allows; and it is counted |
| `sink_m` | 0.04 | "a few centimetres" (the solver-needs note); a worn path sits a little under the turf beside it in the reels (r01, r42) |
| `edge_round` | 0.15 | at W98's 0.75 a fill face at 1 on 2 under a 30 % side slope runs nearly parallel to the ground and the rounding lifts the ground 8 cm two metres off the tread (measured in the tread test); 0.15 keeps the edge a lip |
| `station_m` | 0.5 m | the tread's top is held under the cap station to station; half a metre is a pace |

## Not found

- Tobler's own figure, so the constants 6, 3.5 and 0.05 and the off-path 3/5 are from a secondary
  statement of the function.
- IMBA's *Trail Solutions* (2004): the half rule and the 10 percent average are through the notebook
  and secondary pages only.
- Cole (1978), Dale and Weaver (1974) and Hill and Pickering (2006) were not opened: no distance
  curve of cover from a tread's edge was read from a primary source.
- A fill slope or an angle of repose for loose fill in any trail manual opened.
- A measured width of a bare tread against its disturbed corridor.
