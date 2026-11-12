# Landform, the ice: the grounding (W104 stage C, W106 stage D, W126 stage E, W127 stage F)

2026-11-10 to 2026-11-12. This page backs the ice age of `domain/landform`
([landform, "Ice"](../subsystems/landform.md#ice)): the published model the glaciers follow,
what was read of each source and how far, the rules that are this solver's own closures, and
what each stage measured. It was split from [the solver's grounding page](landform.md) by
W127, when the two together passed a page's ceiling: that page keeps the rivers, the slopes,
the flood, the constants, the measurements and the pictures; this one is the ice, stage by
stage, the newest last. **Read**, **abstract**, **secondary** and **not read** mean what they
mean there ([status of each source](landform.md#status-of-each-source)).

## Stage C (W104): the model

### Ice: the sources

| What | Form used | Source |
|---|---|---|
| The model | Valley glaciers in the stream-power form: the ice flux a point passes on is what its catchment made less what melted, routed down the steepest descent; erosion E = K_g A_i^m S^n with the rivers' m = 1/2 and n = 1, solved by the same implicit scheme; the slope is the ice surface's | Hergarten 2021, *Earth Surf. Dynam.* 9:937–952. **Read** (the publisher's full text, through a fetch that answers questions of it: the first 100,000 of its 121,691 characters, so not its conclusions): equations 13, 14, 19 to 28 and section 6 as that reading gave them. Its extraction garbled fractions, and the reading says where it inferred |
| Ice made and melted | p_i = p min((H − H_e) / (H_f − H_e), 1), H_e the equilibrium line and H_f the height at which all precipitation is ice; negative below the line | The same, equation 23. `full_m` 500 m is H_f − H_e of its example (8 and 10 units of 250 m). Its sum (equation 21) is over every donor; whether a negative flux is clipped the reading could not say. **This solver's**: bare ground below the line loses nothing, and a point the glacier covered last step passes its loss to the glacier; and a second slope below the line, `melt_m` 250 m. That ice melts about twice as fast with depth as it gathers with height is the published balance ratio of glaciers (Rea 2009's area-altitude balance ratios, about 1.75 worldwide), **not read**: quoted from memory of the figure, an authoring default until someone reads it |
| A glacier's width | w ∝ A_i^0.3: width to length 0.6 (Bahr 1997) times a catchment's length to area 0.52 (Prasicek et al. 2020) | Hergarten's equations 10 to 13, **read**; Bahr and Prasicek **secondary**, through him. He says the relation "is not directly derived from data but is consistent with them". The prefactor is his unity in grid spacings and has no physical value there: `width_m` 500 m at one square kilometre of flux is **authoring**, set so a 10 km² glacier is a kilometre wide |
| Thickness | A fixed part of the width | The same, section 6: he derives h from the flux and the slope (his equation 40) and adopts h ∝ w as simpler, 0.05 in his units, "about 0.25" in metres. `thickness_ratio` 0.25 |
| The swath | The erosion acts over a band about the line, each point of it with an effective flux A^ε A_i^(1−ε), ε = 0.25 "a safe choice"; "the U shape is introduced by the swath construction rather than predicted" | The same, section 3 and equation 22, **read**. His band is the donors within half a width of a point down the line. Stage C took every point within half a width whose bed is not above the ice, donor or not, which is what dug the trench at a held edge; stage E went back to his donors and kept the clip on the bed's height, which is this solver's, to keep a band from climbing a wall the ice does not reach ([the held edge](#the-held-edge-counted-and-mended)) |
| Hanging valleys and overdeepening | The slope is that of bed plus thickness, so a tributary meets a trunk at the trunk's ice and a bed may be cut below its receiver's where the ice thins; "overdeepenings, hanging valleys, and steps at confluences at least qualitatively" | The same, section 6 and the abstract, **read**. Here the thickness enters as a rise in the point's base level, weighted by the ice's share of its stream power beside the meltwater's (his equations 24 to 28, K_eff) |
| The ice's erodibility | K_g of 1 and 2 times the rivers' | The same, its figures 4 and 7. `erodibility` 2 |
| What the form cannot do | "The model will even fail completely, e.g., for piedmont glaciers"; width and thickness "rely on empirical scaling"; it "cannot predict under which conditions U-shaped valleys occur or how flat they become" | The same, section 8, **read**. Measured here: 0.73 by the stated measure, a parabola |
| A trough's section | A power law y = a x^b, b near 2; and the flow across a section that makes it | Svensson 1959 and Harbor 1992 (*GSA Bull.* 104:1364). **Not read**: named as what the measure is compared with and as what is missing |
| Till | What the ice carries is laid where the ice melts, in proportion to the melt, to no more than the ice's thickness | **This solver's**; no source. Hergarten's section 5 has a sediment law for ice (K_d,g, K_t,g) which he calls "rather based on intuition than on real-world data", not taken |

Leads that were not used, so nobody looks again: a shallow-ice solve (Kessler, Anderson and Briner
2008; Egholm et al. 2011; Braun, Zwartz and Tomkin 1999, none read) needs steps of hours at this
spacing or a coarser lattice of its own; a perfectly plastic profile (Nye 1951; Benn and Hulton
2010, none read) gives a thickness and no velocity, so its erosion would need the balance flux
anyway. Hergarten's form was taken because it is that balance flux with a published erosion law
on it.

## Stage D (W106): the sources, and the design that is not built

2026-11-11. Stage D set out to give the ice its basins, cirques and moraine ridges, and was cut
by the night's clock to what it could finish: the trough's section checked against the published
form, two checks restated from what the model does, the ground drawn by the renderer, and **the
rules below written as the design for the pass that builds them. None of the three is in the
solver.** The reading was by search extract and abstract for most of it (the publishers' PDFs
would not decode through the fetch, and two are scans), and the table says so line by line: a
number marked secondary is to be read at its source before a world pins it.

### A trough's section: the sources

| What | Form | Source |
|---|---|---|
| The form | Each side of a glacial valley's cross-section as y = a x^b, x from the centre line, fitted in logarithms by least squares | Svensson 1959, *J. Glaciol.* 3(25):362–363. **Read** (the note's text on the publisher's page; its equations are images, so its numbers were not): the exponent of his valley, Lapporten, "deviates only slightly" from 2; "The close agreement with a parabola is, however, evident"; and his own caution that one valley is no general conclusion. The fitted exponents, 2.045 and 2.177, are **secondary** (a search extract of a paper that quotes them) |
| Its range | b from 1.5 (a semicubic parabola) to 2.0, rising with the intensity of erosion | Graf 1970. **Secondary** (a search extract), not read |
| The fluvial end | An exponent of 1 is a V; mean exponents of 1.28 and 1.54 for fluvial and glacial end-member topography | Prasicek, Larsen and Montgomery 2015, *Nat. Commun.* 6:8028. **Secondary** (a search extract) |
| What makes it | The erosion's pattern across a section under a sliding glacier, starting from a V | Harbor 1992, *GSA Bull.* 104:1364–1375. **Not read**: the one copy found is a scan the fetch could not decode. Known only from a later paper's line that his model assumed a quadratic section |

**The measure is the exponent.** A section y = a x^b is 2 (y/a)^(1/b) wide at height y, so its
width half way up over its width at the top is (1/2)^(1/b): a V 0.5, b = 1.5 gives 0.63, a
parabola 0.71, Svensson's 2.18 gives 0.73. Stage C's glacier measured 0.73 and its river 0.56,
which is **b = 2.2 and b = 1.2**. So stage C's "a trough is a parabola, not a U", which it counted
as a shortfall, named the published description of a trough: Svensson's question was whether a
glacial valley is a parabola, and his answer was yes. The test's bounds are now the exponents'
(the river's under 1.5, the glacier's 1.5 or more), where they were "a tenth above the river's".
**That does not show the solver's ice makes a trough**: the test valley's 2.2 turned out to be a
trench against the region's held edge ([what a first attempt measured](#what-a-first-attempt-at-the-basins-measured)),
and with the edge mended on the glacier's line the same valley measures 1.45, under the range.
**What the reels show that the model lacks is a level floor, and in a real trough that floor is
mostly not rock**: it is the fill of a basin the ice dug below the river's grade, flooded when the
ice left and filled by the lake's sediment and the outwash. The solver has the flood and the
fill; it has no basin to fill, which is the next section.

### The basins the ice digs: the sources and the rule

| What | Statement | Source |
|---|---|---|
| What an overdeepening is, and what limits it | Glaciers carve their beds below the local base level; "water, ice and sediment" must then climb an adverse slope, and how well they do depends on that slope's gradient against the ice surface's; an adverse gradient over 11 times the surface's may hold a subglacial lake; the authors suppose the geometry tends to one that keeps the ice flowing efficiently | Cook and Swift 2012, *Earth-Sci. Rev.* 115:332–372. **Abstract only**, through a search extract; the repository copy is larger than the fetch takes. Its sections on where basins form (confluences, the equilibrium line) were **not read** |
| The limit as a number | Above an adverse bed slope of about 1.2 times the ice surface's slope for air-saturated water and 1.7 for pure water, water climbing the slope supercools, its channels freeze shut, sediment stays, and the bed is shielded; the "classic" ratio is 1.6; of 147 mapped overdeepenings few are deeper than the threshold allows | The 1.2 and 1.7 as a later paper gives them, attributing Röthlisberger and Lang 1987 and Alley et al. 1998; the 1.6 and the 147 from Werder 2016, *Geophys. Res. Lett.* 43 (its abstract). All **secondary** (search extracts). Hooke 1991 and Alley et al. 2003 **not read** |
| Where the bed is dug | Erosion in proportion to the ice discharge a unit of valley width carries digs "a parabolic divot" in the long profile, deepest under the equilibrium line; the upper step of real profiles appears only when the tributaries of the head are given as a width | Anderson, Molnar and Kessler 2006, *J. Geophys. Res.* 111:F01004. **Secondary** (search extracts of its summaries; no abstract was reached) |
| Steps and hanging valleys | Among the models that make U-shaped valleys and overdeepened long profiles from erosion by sliding | MacGregor, Anderson, Anderson and Waddington 2000, *Geology* 28:1031. **Not read**: named by a paper that cites it. MacGregor et al. 2009 (headwalls) and Rea 2009 **not read, not reached** |
| What the stream-power form says of it | "With this extension, overdeepenings, hanging valleys, and steps at confluences can be simulated in an at least qualitatively reasonable way"; nothing on what bounds a basin's depth, on cirques or on moraines; the model "is not able to predict ... under which conditions glacial erosion is more efficient than fluvial erosion" | Hergarten 2021, sections 8 and 9: the end stage C had not reached, **read** through the same fetch. With it the paper is read whole |

**Why the solver digs none today.** Under ice a point's base level is its receiver's bed raised
by the ice's share of the step in thickness to the receiver, so the update drives the ice's
surface level *between a point and its receiver* and never past: a bed goes below its receiver's
only where the ice is thinner there, by that much at most. Thickness goes with the flux to the
power 0.3, so it falls downstream only in the ablation zone, slowly, and there the erosion's power
(the square root of the flux) is falling with it. Stage C's one-metre hollow "in one build and
none in the next" is that margin. And a surface that follows the bed point by point reads the far
side of any hollow as an adverse *surface*, where a glacier's surface does not follow its bed:
it is smooth over several ice thicknesses, the ice flows down that surface, and it goes on
wearing a bed that rises under it. That is the missing piece, and the published limit above is
what bounds it.

**The rule, as designed** (closures marked: each is this solver's until someone reads the
source that would replace it):

1. **The slope the ice's erosion reads is the ice surface's over a coupling length**, not to
   the next point, **on a glacier's own line** (a point beside the line keeps the step to its
   receiver: [what a first attempt measured](#what-a-first-attempt-at-the-basins-measured)):
   from a point p, walk down the glacier's line a distance L = `coupling` × the
   ice's thickness at p to a point q; the surface slope is (b_p + h_p − b_q − h_q) / D, D the
   distance walked. That a glacier's flow answers to its thickness and surface slope averaged
   along its length is Kamb and Echelmeyer 1986 (*J. Glaciol.* 32(111):267–284; **abstract**,
   through a search extract of the authors' repository record): the coupling length ℓ is "one
   to three times the ice thickness for valley glaciers and four to ten times for ice sheets",
   and the weighting is well approximated by "a symmetrical triangular averaging window of
   length 4ℓ". So `coupling` is ℓ over the thickness, 2 by default (the middle of their range),
   and the window is 4ℓ, eight thicknesses, about the point. **The closure**: their window is
   symmetric and triangular, and the form below reads only its downstream half, 2ℓ, as one
   difference, because a sweep's point may read lower levels of the network and not higher
   ones; the upstream half would be read from the last step's ground. Which of the two the
   basins need is for the pass that builds them to measure.
2. **Implicit, like every other term**: the ice's term draws the bed toward the level at which
   that surface would be flat, T = b_q + h_q − h_p, at its own rate F_g = K_g Δt √(effective
   flux) over D in cells; the meltwater's term stays the rivers' law to the receiver's bed b_r.
   One division: z = (z + F_g T + F_w b_r) / (1 + F_g + F_w), with q's and r's heights the
   sweep's iterates (both are lower levels of the network, so a level still reads only levels
   below it and the bytes do not depend on the workers).
3. **The depth is bounded by the published limit, not a clamp**: the ice's term stops where the
   bed's adverse slope to its receiver reaches c times the surface slope, c = 1.5 (inside the
   1.2 to 1.7 above). That is a closed form too: (b_r − b_p) / d ≤ c (b_p + h_p − s_q) / D
   gives b_p ≥ (D b_r + c d (s_q − h_p)) / (D + c d), a mean of the receiver's bed and the
   flat-surface level, and the ice's target is the higher of T and that. A basin is then as
   deep as the surface's fall over a coupling length allows and no deeper, and its far side
   rises no steeper than 1.5 times the ice's surface falls.
4. **Where it digs follows from the flux**, with nothing added: below a confluence, where the
   flux jumps and the surface slope does not; under the equilibrium line, where the flux peaks
   (Anderson's divot); and at a valley's head when the line stands high enough that the glacier
   ends a little below it, which is a cirque (below).
5. **The cost** is one walk of a few points down the line for each point of ice, in the ice
   steps only, and one more place beside each point of the sweep.

**What fills them is already there.** Once the ice has gone the flood finds the basin
(`fill`), the rivers' load builds a delta from its head and levels its floor
([deposition](../subsystems/landform.md#deposition)), and a trough then reads, down valley, as a
rock step, a lake or a level flat, and a rock bar. The rock floor under the fill is the height
less `sediment` today. What the field file should gain when the basins stand is the flat's
extent as a field of its own (the sediment that settled in standing water, kept the way `till`
is), so a solver can tell a lake's fill from a fan on it.

**Cirques, as designed.** No rule of their own is proposed: with the rule above, a glacier that
ends a little below its valley's head (a high line: a cirque glaciation) has its flux peak and
fall within a kilometre, digs its divot there, and leaves a floor below a lip where its snout
stood, with a tarn in `fill`. The headwall is the slope limiter's, at the critical slope of its
rock above the ice. What that leaves out is the headwall's own retreat by frost at the ice's
edge (MacGregor et al. 2009, not read), and the published check on the floor's height against
the equilibrium line (Rea 2009's balance ratios, not read): a cirque built this way has to be
measured against both before the page calls it one. It needs the sketch to give the ice a
history (a list of stands, each a year and a line's height), which a moraine needs too.

**Moraines as ridges, as designed.** Today what the ice carries is laid where the ice melts,
spread over the glacier's width: a sheet. The rule proposed: **what melts out at a point of the
glacier's line is laid on the ice's margin within half the glacier's width of it** (the iced
points with a neighbour that is not), still in proportion to the melt and still no higher than
the ice is thick. In the ablation zone the margin within reach is the swath's two edges, so the
load makes laterals, which begin at the equilibrium line because nothing melts above it (that
lateral moraines mark the line is the premise of the published "maximum elevation of lateral
moraines" method, known here from a search extract of Hawkins 1985: **secondary**); at the
snout the edges close and the rest of the load makes the arc across the valley. A ridge's height
is then the volume delivered while the snout stood there over the margin's length, and a stand
that lasts builds a taller one; a later, higher stand leaves a recessional ridge up valley, and
the valley behind a ridge holds a lake by the flood's rule with no rule added. The tests it owes:
a ridge across the valley at each stand, standing above the ground up valley of it; the till's
volume equal to what the ice delivered less what the meltwater took; and laterals that begin at
the line.

### What a first attempt at the basins measured

The rule above was built once, on 2026-11-11, measured for half an hour, and **taken out again
whole**: it is not in the tree, and no pin moved. What the attempt found is what the next pass
should start from, in this order:

1. **The held edge comes first.** In the trough test (a trunk falling at 0.02 whose ice leaves
   through the held east edge) stage C's trunk ends at 1,041 m against an edge held at
   1,273 m, and its bed is below its outlet's level for its whole length. The held point on
   the line carries the glacier's 220 m of ice; the held points beside it carry 57 to 111 m,
   their own threads'; the swath's points that drain to those are worn below the edge by the
   difference, and the trench runs up the trunk from there. The ice's surface would have to
   climb 230 m in one point for that bed to be a glacier's. **Stage C's trough numbers (b = 2.2,
   1.7 times as wide, 196 m below the river's bed) are this trench.** Letting a held point under
   a swath carry the swath's ice grades the trunk to its outlet on the axis (37 m below the
   river's bed, 1.26 times as wide, b = 1.45) and leaves trenches of 50 to 200 m off the axis
   near the edge, whose cause was not found; it was not kept. With nothing laid down
   (`deposition` 0) the valley of the snout test ends 68 to 108 m below its base level beside
   the held edge under either rule, so a run with deposition hides the same defect under fill.
2. **The coupling is for a glacier's own line, not for its swath.** Read by every point of the
   swath, the slope over a reach takes the trough's section away (b from 2.2 to 1.47, the width
   a hundred metres up from 59 points to 32): a point beside the line has its receiver toward
   the line, the reach then runs down the valley, and the cross fall that Hergarten's swath
   wears away is divided by the reach's length. Read only where a point's own flux is the
   swath's (its line), with the step to the receiver kept beside it, the section and the
   hanging tributary stay (b = 1.59, a step of 39.9 m) while the edge's trench is still there
   to flatter them.
3. **A basin has to be told from a pond among till.** With deposition on, the snout valley's
   floor is till in mounds up to 92 m thick (the melt-out rule caps till at the ice's
   thickness and no lower), the glacier's line leaves the valley's axis among them, and the
   standing water counted after the ice is mostly behind till. The test that holds a rock
   basin should run with nothing laid down, or flood the rock's surface (the height less
   `sediment`); the one that holds the fill comes after.
4. **What the rule did where it could be read**: with nothing laid down and the line at
   1,400 m the valley held 508 points of standing water where the last ice stood, to 82 m,
   against 386 and 62 m reading the slope to the next point; the one point of the axis whose
   bed rose under the ice stood at 1.28 times the surface's slope, inside the 1.5. Both
   numbers include the edge's trench, so neither is evidence yet.
5. **The limit as built**: T′ = T + (b_r − T) D / (D + c d) wherever the receiver's bed stands
   above the flat-surface level T, in the sweep, one multiply and one add a point under ice;
   and the ice's power over the reach's length in spacings, so a uniform valley erodes as
   before. The sketch gained `ice.coupling` (2) and `ice.adverse_slope` (1.5).

### The two checks stage C restated, and what was found

- **The dammed valley's "level across within 8 m"** is kept as written and run for what it was
  written for (one thread, no banks: 7.6 m). What stage C measured as 12.9 m with its defaults
  is not a floor that is less level: it is the foot of the side slopes' aprons, which the bank
  rule cuts back to a scarp (the tallest step beside the river's own floor is 9.1 m with banks
  and 4.9 m without), on ground the test raises 30 m in its hundred thousand years while the
  river holds its grade to the dam. **The bank rule bounds no such scarp**: a bank of fill falls
  when the water's reach of it is worn, however tall it stands. So the second case holds what
  the rule does give (the river's own floor, the points within the water's depth of its bed, is
  wider than a thread and no narrower than with no bank taken; the step beside it is taller
  than with none) and bounds the scarp by the only thing that does, the uplift over the run.
  That bound is loose (30 m), and looser than stage C's "a quarter of the 87 m", which nothing
  derived.
- **The fan's "under a metre 1,700 m aside"** asked for a plain "as it was", and the plain there
  never was: by the shares (half straight on and a quarter to each diagonal down a plane) the
  water of the mouths facing the line spreads two points either way in the eight rows to it,
  and the far points stand sixteen beyond the massif's edge, so none of it arrives; what does
  is the massif's east and west flanks' rivers, which cross the plain beside it and turn
  south. The deposit along the line runs from 10.6 m opposite the massif to 1.0 m at the
  region's edges, 2.4 and 3.0 m at the far points. The bound is now what an even sheet of the
  same load along the line would stand at (5.5 m): the far points under it, the crown over it,
  since a point lays down G times the load a unit of its water carries and the far points'
  water has run farther over the plain, laying down and gathering clear water. Stage C's "under
  half the crown" was not derived either.

## Stage E (W126): the held edge mended, and the trough measured again

2026-11-11, the afternoon. Stage D left a defect found and not mended, a test that passed
through it, and a design. Stage E counted the defect, mended it at its cause, and measured the
trough again on the mended ice.

### The held edge, counted and mended

**The count** (the trough test's valley, each of its twenty ice steps: the points whose base
level the ice lowers by a metre or more, which is to say the points that read the ice as
thinning toward their receiver, sorted by why):

| Kind | Stage C, first step | Stage C, last step | With stage D's one-line mend, last step |
|---|---|---|---|
| A point of a swath beside its line whose receiver is a held point | 92 points, 113 m in the mean, 319 m at the worst | 78, 59 m, 159 m | 40, 29 m, 230 m |
| A point of a swath beside its line whose receiver is under less of the glacier (it drains out of the swath) | 112, 11 m, 70 m | 215, 13 m, 193 m | 240, 11 m, 57 m |
| A point of a line whose own ice thins | none | none | none |

Every point of this valley makes ice (the line is under its floor), so the ice thickens all the
way down and the third row is the only one the model means. **Both of the others are one
thing: a glacier's ice on a point that does not drain to the glacier's line.** Stage C stamped
a disc about each point of a line and gave the disc the line's ice, so the disc reached points
whose water and ice go elsewhere: at the region's edge, the points that drain straight out to
the held points beside the trunk's outlet; inside, the points at a disc's rim whose receiver
is one step outside it. Each of those has a receiver under some thinner thread's ice and reads
the difference as a snout. Stage D's mend gave the held points under a disc the disc's ice,
which moved the snout to the two held points just outside the last disc's rim: the trenches
"50 to 200 m deep off the axis", which ran up the swath's two margins from there (the bed at
the margin 914 m and 1,029 m, twenty points up from the edge, beside a floor at 1,271 m).

**The published swath has no such points.** Hergarten 2021, section 3, **read** again for
this (the publisher's text through the same fetch): "Let u be any donor of c0 ... The point u
is added to the swath and assigned to c0 if the distance between u and ci is not greater
than" half the width at ci "for any i ≥ 0 ... the same procedure is applied to all donors of
these points ... the largest value of A_i (resulting from different starting points c0) is
taken for each point." A swath is grown from a point of the line **through its donors**. He
does not say which distance (taken here as the straight one, in whole points, as stage C's
radius was), and he gives no boundary condition for the ice at a domain's edge (his example's
east and west edges are periodic).

**The mend**:

1. **A swath is the points that drain to its line.** Each point walks down its own line (96
   points at most, and no farther than the widest glacier's half width from where it started);
   a line point q stands for every line point between the receiver and q once the point is
   within half of q's width of q and its bed is not above q's ice (stage C's clip, kept);
   the point takes the largest flux among those.
2. **And never more than its receiver has.** That is the quoted recursion's consequence (a
   swath is entered through its own points), taken as a cap rather than by keeping every
   swath's membership: a point's swath flux is at most its receiver's unless its own flux is
   the larger. So a step down in the ice from a point to its receiver happens only where a
   line's own flux falls, which is melt. **This solver's closure**: the cap admits a line
   point whose own swath the receiver is not in when the receiver is in a larger one.
3. **A held point carries the thickest ice of the points that drain to it.** It is outside
   the model (no flux is counted there), so this is a boundary condition, and the one that
   adds nothing: the ice goes on as it came. **This solver's**; the paper has none.
4. **The step the ice comes finds the ice twice.** A glacier pays for its melt over the swath
   it had the step before; with none yet, the first step's glacier melted along its line alone
   and ran to the region's edge whatever the line's height (every line of the one-valley case
   reached column 159 of 160 on its first step and 82 to 155 on its second). That was a
   second way to bring ice to an edge it should not reach, found while a test wanted a snout
   that stays inside its region.

**What it measures**: the count's first two rows are empty at every step. In the trough test
no point stands under the edge's height (stage C: 2,407, the lowest by 274 m), the trunk's
axis falls to the edge without a step (1,400 m at its head, 1,290 m at the junction, 1,273 m
for its last kilometre and a half, the edge at 1,273 m), and the ice is 322 m on the last
point inside and on the edge. With the hillslope creep off these hold to the millimetre; with
it on, closed hollows of 9 to 14 cm stand on the floor, which is creep raising a level floor
unevenly where nothing is laid down, and not the ice.

**What it changes that was not asked for**: with less ice put where it does not belong, the
ice's surface is lower, less snow becomes ice, and glaciers are shorter and thinner than stage
C's. The one-valley case with the line at 1,600 m holds ice 32 m thick at the end where stage
C's reached 92 m; its valley glacier is gone from the floor by 65,000 years because the ground
has worn down under the line. And **a tributary's hanging step is 9 m where stage C measured
40 m** (30 m with the swath mended and before the first step was): the step stands where the
tributary's own ice meets the trunk's swath, and that margin moves by a point when the
trunk's thickness crosses a whole point of half width, after which the old step is inside the
swath and is worn away in a step or two. Nothing in the model sets a hanging step's height
from below; the page no longer claims one.

### The trough's section on the mended ice

| What | Statement | Source |
|---|---|---|
| The form and its range | y = a x^b "best" describes a glacial valley's cross-section; b from 1.5 to 2.0; b rises with the intensity of glacial erosion (deeper, narrower sections) | Graf 1970, *Arctic and Alpine Research* 2(4):303–312. **Abstract** (the author's repository record), where stage D had a search extract. The body was not opened |
| A wider range | b between 1.0 and 2.0 "in practice" (Hirano and Aniya 1988); in the Tian Shan 1.03 to 3.50 with most between 1.3 and 2.5 (Li, Liu and Cui 2001, as a later paper quotes them) | **Secondary** (search extracts of the two records and of the paper that quotes the third). Not read |
| What makes the section | A model in which "the erosion rate varies quadratically with sliding speed" forms U-shaped valleys "within 50 ka"; it "primarily simulates the formation of glacial valleys by deepening", where observed valleys "have formed mainly by widening" | Seddik, Greve, Sugiyama and Naruse 2009 (arXiv:0901.1177). **Abstract only**: the PDF would not decode. Harbor 1992 (*GSA Bull.* 104:1364), the model it follows, was looked for again and **not read**: the copies found are scans |

**The measure**, as stage D left it: a section's width half way up its lowest hundred metres
over its width at the hundred is (1/2)^(1/b). On the mended ice, the trough test's trunk at
five sections and five ages (the river's b beside it; widths in points of 50 m at the hundred
metres):

| Years of ice | b at columns 40, 52 (above the tributary) | b at 84, 96, 108 (below it) | Width at 96, ice against river |
|---|---|---|---|
| 10,000 | 1.9, 0.8 | 1.7, 1.5, 1.5 | 16 against 9 |
| 20,000 | 2.0, 1.7 | 2.2, 2.3, 1.7 | 23 against 8 |
| 40,000 | 4.3, 6.9 | 5.7, 4.3, 9.4 | 27 against 9 |
| 60,000 | 2.7, 5.0 | 4.5, 3.2, 6.8 | 31 against 11 |
| 100,000 | 3.1, 4.7 | 3.3, 6.4, 4.8 | 39 against 13 |

(The river's b is 0.85 to 1.5 throughout, 1.2 in the mean. The measure is a ratio of two
widths and is ill-conditioned past b = 3, where the two differ by a point or two.)

**So the mended ice does make a trough, and the answer to stage D's question is yes with a
limit.** A glacier's valley passes through the published range in its first 20,000 years
(b 1.5 to 2.3, twice the river's width) and then goes past it to a floor between walls
(b 3 and more). The section at 40,000 years, across the trunk above the tributary, in metres
every 100 m: 1,556, 1,489, 1,411 | 1,360, 1,351, 1,346, 1,342, **1,337**, 1,341, 1,346,
1,349, 1,353 | 1,400, 1,478, 1,558. The floor between the bars is the swath (the ice 250 m
thick over all of it): 23 m of cross fall over 400 m, a slope of 0.06 where the walls stand at
1.5. **Why**: every point of a swath has the line's flux in its stream power (to the 3/4)
and is worn toward its receiver, which is toward the line, so the swath's cross fall is
ground down to a few times the line's own fall; and outside the swath a slope's thinner ice
meets the trunk's at a step in the ice's surface, so the wall hangs above the floor by that
step and is steepened to its critical slope. That is the U of Hergarten's sentence,
"introduced by the swath construction rather than predicted". Once the trunk nears its base
level (the test's does after 40,000 years: the ice wears 2 mm a year) the floor is level.

**What is missing, said plainly**: nothing in the model makes the erosion fall off across a
glacier's width. The published section models (Harbor's, and Seddik's above) wear the bed in
proportion to the sliding speed squared, which is greatest under the thickest ice and falls
to nothing at the margin, and that is what holds a parabola while the valley deepens. A
closure that would do it inside this form is to weigh a swath point's power by the ice over
it under a level surface (the line's surface less the point's bed) against the line's
thickness. It is not built: the rock floor being too level is not what a person objects to in
the pictures (they show level floors, of fill), and the basins come first. The test holds the
published lower bound (b at 1.5 or more against the river's under 1.5), that the section a
hundred metres up is at least the swath wide (which is the model's own statement), and no
upper bound.

**The checks this restated** (each was stage C's number, taken on the trench): the trunk's
bed at the junction "50 m or more below the river's" is now "below the river's and not below
the edge" (25 m: the ice's erodibility is twice the rivers' on a flux the swath shares, and
the edge is 17 m below the junction); "the tributary's steepest step over five times the
river's" is now "taller than the river's, and where the ice thickens by 50 m or more"
(8.9 m against 2.7 m, at the swath's margin, the ice 224 m above it and 294 m below).

### The basins, as built, and what they are worth

Stage D's rule is in the solver, with one thing added that its design did not have and its
first attempt did not find.

**What was built** is the design above, point for point: on a glacier's own line the ice's
term reads its surface from the point to the point twice `coupling` thicknesses down the line
(`coupling` 2, the middle of Kamb and Echelmeyer's one to three; one walk of at most 64 points
for each point of ice, in the ice steps only), draws the bed toward the flat-surface level
T = b_q + h_q − h_p with its power over the reach's length, and stops at the adverse-slope
limit as the closed form T′ = T + (b_r − T) D / (D + c d), c = `adverse_slope` 1.5. A point of
a swath beside the line reads the step to its receiver, as before.

**What the design got wrong: the meltwater's term fills.** The design's update is
z = (z + F_g T + F_w b_r) / (1 + F_g + F_w), the ice toward its level and the water toward
the receiver's bed. Where the ice's level is below the receiver's bed, the water's term in
that formula is positive: it raises the bed toward the receiver, which is deposition by an
erosion law. So a basin came out as deep as the ice's share F_g / (F_g + F_w) of what the ice
alone would dig, and **reading the slope over a reach divides the ice's power by the reach's
length, eight points or so, and its share with it**. Measured in the firn valley (below) with
nothing laid down: with the design's update the valley held 71 points of standing water to
3.5 m, where reading the slope to the next point held 408 points to 13 m; the coupling dug
less than the rule it replaced. In a glacier's lower reach the meltwater is the whole
catchment's and its term is the larger of the two. **The mend**: where the joint update ends
below the receiver's bed, the water's term is left out and the ice works alone, with its own
part of the power, toward T′ (two evaluations of the same formula for such a point, and one
for every other). **This solver's closure**; that subglacial water climbs an adverse bed
rather than grading it is the premise of the published limit itself (it is the water that
supercools on the climb).

**The test valley.** A glacier fed from steep walls does not last: the walls' own ice wears
them 3 mm a year at the default erodibility, they sink under the line, and the one-valley
case's glacier is gone from its floor in 40,000 to 65,000 years. So the basins' valley has a
firn plateau above it (four kilometres falling at 0.025 from 2,000 m, then six kilometres of
valley at 0.03 between sides at 0.5, the line at 1,750 m). The plateau wears down too (its
axis stands at 1,740 m at the head and under 1,440 m half a kilometre in by the end), and the
glacier fills the valley to its edge for about 30,000 years and then draws back up it, so
every reach of the floor has stood under a snout.
That is a retreating glacier, not a steady one; no steady one was found in a hundred thousand
years of this solver's ice.

**What it measures**, nothing laid down and the creep off (so a hollow is rock):

| Run | Points holding a metre of water | Deepest |
|---|---|---|
| The valley under its rivers alone | 0 | 0 |
| Under ice, as built (`coupling` 2, limit 1.5) | 130, columns 132 to 197 of 200, all on the floor | 7.5 m |
| The limit at 0.5 | | 2.4 m |
| The limit at 6 | | 12.2 m |
| The slope read to the next point (`coupling` 0), water not filling | 505 | 34 m |

No point holds more water than the thickest ice that ever stood on it, and none holds any
where no ice stood. The axis' bed first rises after three steps, at five points, the steepest
at 0.89 of the ice surface's slope over its reach (0.26 with the limit at 0.5).

**Reading the slope to the next point digs a pit under every snout.** With `coupling` 0 the
only thing that lets a bed fall below its receiver is the ice thinning from one point to the
next, and this model's ice thins fastest at its snout (its thickness goes with the flux to
the power 0.3, and a glacier pays its swath's melt along its line, so the last half
kilometre loses about five metres of ice a point). So each step's snout leaves a pit a few
points long and up to 34 m deep, and a retreating glacier leaves a row of them, a saw-tooth
down the valley's axis (the bed at 1,312, 1,320, 1,314, 1,308, 1,302, 1,313, 1,309, 1,307,
1,303, 1,310 m every 250 m). Read over a reach of four thicknesses the same thinning is spread along the reach
and the pits become one long shallow basin. That, and not a deeper basin, is what the
coupling buys.

**What they are worth in a sketch, said plainly: little yet.** The basins are dug in the
glacier's lower reach, and that is where the till rule lays what the ice carries, to no more
than the ice's thickness. With deposition on, the firn valley when its ice leaves holds 819
points of water to 40 m, most of it ponded behind till and not in rock (stage D's solver:
287 points to 75 m), and 40,000 years of rivers fill all of it. The two-range sketch
(512 by 512 at 32 m) holds one point of a metre of water after its ice age with stage D's
solver and none with this, and with a colder ice age that ends one step before the run does
(the line at 600 m) it holds 11, where stage D's holds 2. **The pictures still show no trough lake.** What stands
between the rule and a lake a person can see is the till: laid as moraines on the ice's
margin (stage D's design, not built) it would dam the valley at the snout and leave the basin
open behind it, which is the order the photographs show: bar or moraine, lake, flat.

**Not built, and the state of each**:

- **The fields.** The rock floor under a basin's fill is the height less `sediment`, as it
  was; the flat's extent (the sediment that settled in standing water, kept apart the way
  `till` is) is not a field, and the field file is unchanged at version 3. To add it: the two
  places the books lay sediment in a lake (`spread` at a river's mouth and `fill_lake` on the
  floor) add to an array beside `till`, capped by the sediment each step as till is.
- **Cirques.** The rule is the basins' under a glacier that ends near its head; the sketch's
  ice needs a history of stands (years and line heights) to hold a glacier there, and the
  one-valley case shows why a single stand will not: the glacier is never still.
- **Moraines as ridges**, and till capped below the ice's thickness: the first thing to
  build, for the reason above. Stage E measured till to 58 m under ice 36 m thick at the last
  step (the cap is the thickness of the glacier that laid it, and earlier glaciers were
  thicker).
- **The section's fall across a swath** ([the trough's section](#the-troughs-section-on-the-mended-ice)).
- **A hanging valley worth the name**: a test valley whose tributary is much thinner than
  its trunk.

**What the solve costs.** The ice's new work is inside the ice steps: one walk down each
point's line for its swath (which replaced the stamp of a disc about every line point), one
more for its reach, and for a point whose joint update ends under its receiver's bed a
second evaluation of the update. The two-range sketch (512 by 512, 300 steps of which 76 are
ice), `engine-content landform --jobs 5` in `msvc-release`, the base's executable and this
one's run alternately on a machine at 76% to 84% CPU before each run: the subsystem page's
[performance notes](../subsystems/landform.md#performance-notes) have the pairs.
