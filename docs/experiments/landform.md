# Landform: the grounding of the solver (W95, stages A and B; W104, stage C)

2026-11-10. This page backs `domain/landform` ([landform](../subsystems/landform.md)): what the
solver models, where each process and constant comes from, what was read and what was not, the
decisions determinism forced, and the model's limits. The measurements (the bench, the regressions
the tests hold) are at the end.

**The want.** Mountains are the largest subject in the owner's reference reels (116 scenes), and a
range authored as ridge lines with a profile reads as smooth cones: no drainage, no spurs, no
couloirs, near-symmetric peaks. What makes a range read as a range is that it has been **eroded**:
the reels' walls (r03, r04) are cut into ribs and gullies that branch downward, their crests wander
and their summits sit off-centre, and their valley floors are far flatter than their sides. A
heightfield gets that from a drainage network, not from a profile function, so the solver is a
landscape evolution model in the published sense: uplift raises the ground, rivers cut it in
proportion to the water they collect and the slope they run down, and hillslopes shed what the
rivers undercut.

## Status of each source

**Read** means the text was read for this page (through a fetch of the publisher's or the archive's
page); **abstract** means only the abstract; **secondary** means the number is quoted by a source
that was read, or by the forest proof of concept's notes, and the original was not; **not read**
means nothing was taken from it but its name. The proof of concept's research notes
(`landform-v31-grounding.md`) were leads to these publications and are not cited as sources.

| Process | Form used | Source |
|---|---|---|
| Depression filling and routing | Priority-Flood: seed a priority queue with the outlets, pop the lowest, raise each unvisited neighbour to at least the popped height plus ε, push it. Every point then has a path that never rises to an outlet | Barnes, Lehman and Mulla 2014, *Computers & Geosciences* 62:117–127 (arXiv:1511.04463). **Read**: Algorithm 1 (the flood), Algorithm 3 (the ε variant: "requires a total-order queue"), Algorithm 4 (flow directions), and the section on orders: a total order gives "a predictable, reproducible result", a strict weak order does not. Complexity O(n log n) for floating point, O(n) for integers |
| Fluvial incision | The stream-power law, ∂z/∂t = U − K A^m S^n, solved implicitly in O(n) along the flow tree, each point after its receiver | Braun and Willett 2013, *Geomorphology* 180–181:170–179. **Not read** (paywalled). The scheme as described by Cordonnier, Bovy and Braun 2019, *Earth Surf. Dynam.* 7:549 (**read**: Eq. 3, "a single flow receiver", "an implicit scheme of linear complexity", drainage area treated explicitly), and by Barnes 2019 (arXiv:1803.02977, **read**: the steps receivers, donors, order, accumulation, uplift, erosion; Algorithm 6, the backward-Euler update with Newton-Raphson for a general n; cells of one level of the order may run concurrently) |
| Exponents | m = 1/2, n = 1 | Stock and Montgomery 1999, *J. Geophys. Res.* 104(B3):4983–4993, **abstract**: rivers with stable base levels follow K A^(0.3–0.5) S^1. m/n = 0.5 sits inside the 0.4–0.6 "often reported" for the concavity (Lague 2014, *ESPL* 39:38, **secondary**: the PDF would not decode here) |
| Hillslope transport | Linear diffusion, ∂z/∂t = D ∇²z, plus a critical slope no hillslope stands above: the two regimes of the nonlinear law q = K S / (1 − (S/Sc)²), which is linear diffusion at low gradient and whose flux grows without bound as S nears Sc | Roering, Kirchner and Dietrich 1999, *Water Resour. Res.* 35(3):853–870. **Abstract only** (the publisher refused the fetch): the law's form and that it "reduces to linear diffusion at low gradients"; its model runs use K = 0.003 m²/yr (seen in the figure text of an indexed copy); its Oregon Coast Range Sc = 1.25 is **secondary** |
| Flow direction on a plane | A diagonal neighbour's fall is weighed by 1 / (2 − r), r random in [0, 1), in place of 1 / √2 (Rho8) | Fairfield and Leymarie 1991, *Water Resour. Res.* 27(5):709–717. **Abstract only**: its problem ("unless the terrain is rugged, the derived channels tend to run in parallel lines") and that it answers with a random direction. The weight is the formula as the method is usually given and was **not** checked against the paper; what can be checked is its consequence, derived here: on a plane whose fall line stands an angle t off an axis, the diagonal wins where r > 1 − tan t, so a share tan t of the cells step diagonally and the mean direction is the plane's. Here r is hashed from the seed and the cell, so a cell's choice does not change from step to step and the network holds still |
| The slope limit as a gradient | The largest surface under the ground whose gradient is at most the critical slope: the eikonal equation's upwind step, (a + b + √(2f² − (a − b)²)) / 2, swept to convergence | The standard first-order (Godunov) scheme and the fast sweeping method (Zhao 2005, *Math. Comp.* 74:603–627; **not read**: the scheme is written here from its textbook statement). No number comes from it |
| Base level | An outlet is a point held at its level (a Dirichlet boundary); a river cuts no lower than the sea | The convention of every source above (Cordonnier et al. 2019 route to "boundary nodes"); no number comes from it |

## Deposition: the sources

2026-11-10, stage B. The same status words as above. Stage A's rivers only cut; the reels' valleys
(r03, r04) stand on flat floors under walls, which is sediment, so the law had to be the one that
lays down as well.

| Process | Form used | Source |
|---|---|---|
| Erosion and deposition in one law | The bed loses e = K q^m S^n and gains d = q_s / ξ, the sediment flux over a transport length ξ = q / (d* v_s) that grows with the water discharge; along a stream dq_s/dx = e − q_s/ξ. Θ = d* v_s / r (settling velocity over runoff) says which limit a river is in: far below 1 detachment-limited, far above transport-limited, and "generally larger than 1" for natural rivers | Davy and Lague 2009, *J. Geophys. Res.* 114, F03007, doi:10.1029/2008JF001146. **Read** (equations 3 to 9 and 15 to 17). The paper's notation list defines d* the other way up from its equation 6; the equation is what is used |
| The same on drainage area, and its O(n) implicit solution | ∂h/∂t = U − K_f A^m S^n + (G / A) ∫_A (U − ∂h/∂t) dA, G = d* v_s / p (Davy and Lague's Θ). Discretely the deposit at a node is G / N_i times the sum over the nodes **upstream** of it of what each lost (the node itself is not in the sum). Solved by Gauss-Seidel sweeps in the order of the flow tree: h_i = [b_i − (G/N_i) Σ_ups h_j + F_i h_rec] / (1 + F_i), until the largest change is under 10⁻³ of U Δt; convergence is proven for G ≤ 1 and observed beyond; one sweep at G = 0, six at G = 1, eight or nine at G = 2, independent of the resolution | Yuan, Braun, Guerit, Rouby and Cordonnier 2019, *J. Geophys. Res. Earth Surface* 124(6):1346–1365, doi:10.1029/2018JF004867. **Read** (equations 9 to 24, figure 2's counts, the foreland runs). That the sum leaves out the node itself is read from its diagonal, 1 + F_i, and its convergence bound G < N_i / (N_i − 1) |
| The steady state with deposition | S = [(1 + G) U / K_f]^(1/n) A^(−m/n): the steepness (and the response time) scale by 1 + G, the concavity does not change | Yuan et al. 2019, equation 24. **Read**. The test holds it |
| What G is in nature | G = S_F^n / (S_R^n − S_F^n) from the slope of a fan and of the range river that feeds it at the apex. Experiments: 0.4 to 1.2, median 0.7. Natural systems (68): about 0 to 3.1, a third below 0.4 and two thirds above | Guerit, Yuan, Carretier, Bonnet, Rohais, Braun and Rouby 2019, *Geology* 47(9):853–856, doi:10.1130/G46356.1. **Read** (the accepted manuscript; figures and supplement not seen) |
| Sediment more erodible than rock | A second erodibility for the sediment: 1, 2 and 4 times the rock's in the foreland runs; at 4 almost no sediment is kept | Yuan et al. 2019. **Read**. They switch on height above a base surface and say they track no thickness in the uplifting area; here it is a thickness at every point |
| A fan's slope | For this law, what follows from G above: at the apex S_F = S_R G / (1 + G) at n = 1, half the feeding river's slope at G = 1. In the field: fans stand at 1.5° and more, depositional slopes of 0.5° to 1.5° being rare (Blair and McPherson 1994, **secondary**, from a lecture page; disputed for humid regions); a channel's slope falls down-fan from 0.10–0.04 to about 0.01 (Stock, Schmidt and Miller 2008, *GSA Bull.* 120:619, **abstract**) | Guerit et al. 2019 (**read**) for the relation; the field ranges are leads, not pins |
| A valley floor's width | W = k A^b with b about 0.3 to 0.4 in most studies: 0.30 ± 0.06 (Appalachian rivers) and 0.32 ± 0.15 (Clubb, Weir and Mudd 2022, *Earth Surf. Dynam.* 10:437, **read**), 0.3–0.4 (Brocard and van der Beek 2006), 0.34 (Schanz and Montgomery 2016), 0.76 (Tomkin et al. 2003), all **secondary** through Clubb 2022. In the Himalaya width falls as exhumation rises and widening is "sediment deposition, not lateral bedrock erosion" (Clubb et al. 2023, *Nature Geosci.* 16:739, **read**) | A check on what a floor should do, not an input: the model has no lateral erosion (Yuan et al. say so of theirs), so a floor here is as wide as its fill reaches up the valley's sides |
| What settles where in a lake | A river's bedload and sand stop at its mouth and its wash load goes on. The bedload share runs from about 1% of the load in large gravel-bed catchments to 30–50% in sand-bed rivers (Turowski, Rickenmann and Dadson 2010, *Sedimentology* 57:1126, **abstract**) | The split is `delta_share`, an authoring number: no source gives a share for what stops at a mouth, since suspended sand stops there too |

**What follows for the design.** Yuan's scheme, not a cut followed by a deposit: both were built,
and the subsystem page has what the second measured
([deposition](../subsystems/landform.md#deposition)). The sweeps end on a height tolerance, a
millimetre, since U Δt is zero in a sketch that raises nothing. **Lakes are not in
Yuan et al.** (their only remark is that local minima "artificially reduce the erosional
efficiency"), so what a lake and the sea do with the flux is this solver's: everything that
reaches standing water settles in it, a share at the mouth up to the water's level and the rest
from the deepest point up, to one level. The level fill is a statement that what stays in
suspension spreads over the whole floor; it is not a published scheme.

## Structure: the sources

2026-11-10, stage B. What is published about a river cutting through layers of unlike rock, and
what the solver takes from it.

| Finding | Source |
|---|---|
| Where a river crosses a contact between a weak and a strong rock, the two retreat at one rate measured along the contact ("erosional continuity"). With a contact much steeper than the channel that is the ordinary steady state, K_w S_w^n = K_s S_s^n, the strong rock steeper. With a contact much flatter than the channel it is K_w S_w^(n−1) = K_s S_s^(n−1): for n above 1 the strong rock is steeper still, for n below 1 the weak rock is the steeper, and **at n = 1 there is no fixed ratio**: the contacts stretch and stand as near-vertical steps. Its runs: K_w/K_s of 2 (1.2 at n = 1), layers of 10 and 50 m, m/n = 0.5 | Perne, Covington, Thaler and Myre 2017, *Earth Surf. Dynam.* 5:85–100, doi:10.5194/esurf-5-85-2017. **Read** |
| Layered rock with a strong contrast, flat to moderately tilted, does not reach a steady state across the landscape: erosion rates vary in space and time with the order of the layers, the way the contact dips against the outlet and its angle, and can be above or below the uplift in either rock | Forte, Yanites and Whipple 2016, *Earth Surf. Process. Landforms* 41:1736–1757, doi:10.1002/esp.3947. **Abstract only** (its erodibility ratios, thicknesses and dips were not read) |
| 100 m layers, the weak rock 2.1 to 2.8 times as erodible, dips to 10° upstream and 2.5° downstream: with n above 1 and beds dipping downstream, erosion locally exceeds ten times the uplift | Mitchell and Yanites 2021, *Earth Surf. Dynam.* 9:723–753, doi:10.5194/esurf-9-723-2021. **Read** (the first part of the page) |
| A cuesta's backslope follows the dip of its resistant bed and its front slope cuts across the bedding and is the steeper; cuesta, homoclinal ridge and hogback are one form at rising dip, and the angles that divide them are arbitrary | **Secondary** (an encyclopedia's summary of Thornbury 1954 and others). No measured range of scarp angles was found in a source that could be read |

**What follows for the design.** The solver is at n = 1, where Perne et al. say no ratio of
steepness is fixed and contacts stand as steps; so the test states the ratios its own sketch
gives and calls them that. The contrast between beds stays the sketch's (`rock.contrast`,
default 10, an authoring number: the published runs use 1.2 to 2.8, and a default nearer those is
something to weigh when the defaults are grounded). The frame is the point of the design: a
formation fixed in space under a surface that is being raised and cut is flat layers of
elevation; read in the rock's frame the beds come up through the surface, which is what the
three papers model.

## Slopes and steepness: the sources

2026-11-10, stage B. What real ranges measure, for the defaults and for the variation along a
ridge.

| Measure | Source |
|---|---|
| In the San Gabriel Mountains (50 basins eroding at 35 to 1,100 m/Myr) the mean hillslope angle rises with the erosion rate up to about 300 m/Myr and then stops responding, while the channels' normalized steepness keeps rising, nonlinearly | DiBiase, Whipple, Heimsath and Ouimet 2010, *Earth Planet. Sci. Lett.* 289:134–144, doi:10.1016/j.epsl.2009.10.036. **Abstract only** |
| The same range's normalized steepness (reference concavity 0.45) runs from 30 to 180 m^0.9, and rises as about the square root of the erosion rate; across studies the exponent is 0.25 to 1 | DiBiase and Whipple 2011, *J. Geophys. Res.* 116, F04036, doi:10.1029/2011JF002095. **Read** |
| On 1 m lidar, below a catchment mean slope of 30° the modal slope rises with the mean and little rock is exposed; above it rock exposure rises sharply and debris at its angle of repose holds the modal slope near 37° | DiBiase, Heimsath and Whipple 2012, *Earth Surf. Process. Landforms* 37:855–865, doi:10.1002/esp.3205. **Abstract only** |
| E = E₀ + K S / [1 − (S/S_c)²] with S_c = 40° for the Olympic Mountains; mean slope there varies by a few degrees across strong gradients of erosion rate | Montgomery and Brandon 2002, *Earth Planet. Sci. Lett.* 201:481, **secondary** through Montgomery 2003, *C. R. Geoscience* 335:1121 (read as a web page) |
| Hillslopes stop steepening above about 0.2 mm/yr; channel steepness keeps rising to at least 0.6 mm/yr (eastern Tibet) | Ouimet, Whipple and Granger 2009, *Geology* 37:579–582, doi:10.1130/G30013A.1. **Abstract only** |
| The nonlinear hillslope law's fitted S_c is 1.25 ± 0.10 (gradient) and its K 0.0032 ± 0.0009 m²/yr at one Oregon Coast Range basin; over five, S_c 1.2 to 1.35 and K 0.0031 to 0.0045 | Roering, Kirchner and Dietrich 1999, *Water Resour. Res.* 35:853–870. **Read** (the author's reprint; stage A had only the abstract). That S_c is the law's asymptote, which no slope reaches, not the angle slopes stand at: the solver's critical slope is the angle they stand at, which is the lidar's 37° and Montgomery's 40° |
| Power spectra of topography along linear transects are often power laws with an exponent near −2, S(k) ∝ k⁻²; a transect's fractal dimension is near 1.5, a Brown noise | Pelletier, "Why is topography fractal?", arXiv:physics/9705033, **abstract**; Huang and Turcotte 1989, *J. Geophys. Res.* 94:7491, **abstract** (Arizona, D = 1.52). That D = 1.5 is an exponent of 2 is the usual β = 5 − 2D, not read in either |

**What follows.** (1) The critical slope's default is **0.8** (38.7°), between the 37° and the
40°; it was 0.9 (42°), above both, on the strength of a secondary quote. (2) K's default stays
5 × 10⁻⁶: with G = 1 a ridge at the default 1 mm/yr has a steepness of 400 m at the solver's
concavity of 0.5, and a steepness at 0.5 is one at 0.45 times A^0.05, 2.2 at 10 km², so about
180 m^0.9: where DiBiase and Whipple's range ends, at about that erosion rate. The law here is
linear in the uplift (n = 1) where their fit is a square root, so a sketch at 0.1 mm/yr is
gentler than the field (40 m against about 60). (3) The diffusivity's default, 0.005, is inside
Roering's 0.003 to 0.0045 times the factor of two their soil-to-rock density ratio puts between
a soil flux and a rock one; it was not changed. (4) A ridge's rate varies along its line as a
Brown noise, the spectrum the transects have; the amplitude (0.35) and the longest wavelength
(6 km) are authoring: no source gives the spectrum of uplift along a range, only of the
topography it leaves.

## The flood: the sources

2026-11-10, stage B. The published ways of making Priority-Flood cheaper, and which the solver
took ([the flood's cost](../subsystems/landform.md#the-floods-cost)).

| Variant | Source |
|---|---|
| A plain queue beside the priority queue: a neighbour no higher than the cell that reaches it is in a depression and goes to the plain queue, which is emptied first. 16.8% faster on average (12.5% median, 37.2% at most). The ε variant is built on the plain version, raises with `nextafter`, and needs a total order in the queue | Barnes, Lehman and Mulla 2014, *Comput. Geosci.* 62:117–127, arXiv:1511.04463, Algorithms 2 and 3. **Read** |
| Region-growing: most cells outside depressions and flats are processed without the priority queue; 44.6% faster on average than the fastest variant before it, over thirty 3 m DEMs | Zhou, Sun and Fu 2016, *Comput. Geosci.* 90:87–96, doi:10.1016/j.cageo.2016.02.021. **Abstract only** |
| The cells the priority queue processes cut by about 70%; 31 to 52% faster than the fastest before it | Wei, Zhou and Fu 2019, *Int. J. Digital Earth* 12:415–427, doi:10.1080/17538947.2018.1429503. **Abstract only** (the mechanism was not read) |
| No fill at all: basins labelled from the boundary and from local minima, linked by their lowest passes, a minimum spanning tree of that graph oriented from the boundary, and receivers updated along it; linear in the points | Cordonnier, Bovy and Braun 2019, *Earth Surf. Dynam.* 7:549–562. **Read**. No speed-up is stated as a number |

**What was taken.** The idea the middle two share, in the form the solver's total order allows:
ground with a strictly falling path to an outlet never needs the queue, because its filled
height is its own, and the order the queue would have given is the sort of the filled heights
(a push is never below the pop that made it). The bytes are the flood's, which the basin graph's
are not. Neither Zhou's nor Wei's algorithm was read, so this is not theirs; it is the same
observation, checked by the pins.

## Stage C (W104): the sources

2026-11-10 and 11. Stage C read these for what it added; the same four words say how far.
**The ice's sources, and stages D to F which are all ice, are on a page of their own**:
[the ice's grounding](landform-ice.md).

### The sweeps' layout, and the contrast's default

| What | Form used | Source |
|---|---|---|
| The order of a sweep | The network's points by generation from the outlets (a breadth-first stack), each point's receivers before it; the points of one generation independent of each other | Braun and Willett 2013 (**not read**, paywalled: their stack is by depth first, and its independence is by catchment) as described by Barnes 2019 (arXiv:1803.02977, **read** in stage A: "levels" of the order whose cells may run concurrently). The layout here is that level order with the levels' points sorted by their receiver's place; no number comes from either |
| `rock.contrast` 2 | Erodibility of the weakest rock over the strongest in one range | The contrasts stage B's page lists under [structure](#structure-the-sources) (1.2 to 2.8 across the lithologies of single ranges); stage B kept an authoring 10 and said so. 2 is the middle of the published range; a sketch that wants cliffs asks for more |

### Flow that divides: the sources

| What | Form used | Source |
|---|---|---|
| The rule | A point's flow to each lower neighbour in proportion to the slope toward it times the contour length it crosses, 0.5 of a spacing toward a cardinal neighbour and 0.354 toward a diagonal | Quinn, Beven, Chevallier and Planchon 1991, *Hydrol. Processes* 5:59–79. **Not read**; the rule and its two contour lengths are as the multiple-flow-direction literature states them (**secondary**: Esri's "How Flow Direction works", which gives the partition with a distance adjustment for cardinal and diagonal neighbours, and Qin et al. below) |
| Where it applies | The exponent on the slope rises with the steepest downslope gradient e: f(e) = 8.9 min(e, 1) + 1.1, from 1.1 on a plain to 10 on a slope of 1 | Qin, Zhu, Pei, Li, Zhou and Yang 2007, *Int. J. Geogr. Inf. Sci.* 21(4):443–458. **Secondary**: the formula as Esri's page quotes it and attributes it; the author's own copy of the paper was fetched and would not decode. The solver does not use the exponent: it divides at exponent 1 where f(e) < 2 (e < 0.1) and not at all above, a two-valued stand-in chosen so that no power is taken, and says so. `divide_below_slope` 0.1 is that crossing |
| The rivers' law over several receivers | Each receiver's implicit term is K Δt (A w_k)^m / l_k, the height (z + Σ F_k z_k) / (1 + Σ F_k), the sediment passed by the weights | FastScape's `StreamPowerLaw.f90` (the implementation of Yuan et al. 2019), **read** as source: `fact = kfint*dt*(a*mwrec)**m/mlrec`, `h = f/df`, `dh(ijr) = dh(ijr) + dh(ijk)*mwrec(k,ijk)`. Its own weights are slope to a power that may vary with slope (`p_mfd_exp = 0.5 + 0.6*slope`, `FlowRouting.f90`, **read**), on a surface filled by 10⁻⁸ a step; here the weights are Quinn's and the surface the flood's |

Authoring, marked so: the two-valued rule itself, and the shares kept as parts of 32,768.

### Lateral erosion: the sources

| What | Form used | Source |
|---|---|---|
| The model | A reach wears the neighbour on the outside of its bend at K_l A S / R; the volume, that rate over the water's depth and one spacing, accumulates at the neighbour until it equals what stands there above the reach's receiver, and the neighbour is then lowered to the receiver's height | Langston and Tucker 2018, *Earth Surf. Dynam.* 6:1–27. **Abstract only** of the article (its page gives no body and its PDF is past what the fetch takes): two bank models, one "the slow process of widening a bedrock canyon", one undercutting and slumping in softer rock; valleys "many times wider than the grid discretization scale". **The algorithm was read from the authors' implementation**, Landlab's `LateralEroder` (`lateral_erosion.py`, `node_finder.py`): `petlat = -Kl*da*max_slopes*inv_rad_curv`; `wd = 0.4*(da*runoffms)**0.35` with `runoffms = (Klr*F/kw)**2`, F = 0.02, kw = 10, so 4 × 10⁻⁶ m/s at a ratio of 1; `vol_lat_dt += abs(petlat)*dx*wd`; the two thresholds `(z[i] + wd - z[flowdirs[i]])*dx**2` (undercutting-slump) and `(z[lat_node] - z[flowdirs[i]])*dx**2` (total block); the lowering to `z[flowdirs[i]]`; the donor of the largest area as the inflow |
| The curvature | 0.23, 0.67 and 1.37 over the spacing for a straight reach, a bend of 45 or 135 degrees, a right angle; on a straight reach a random side | The same implementation (`node_finder.py`), **read** as source. Here the side on a straight reach is hashed from the seed, the step and the cell, and the bank is the neighbour square to the outflow on the bend's outside (the implementation uses lookup tables by donor and receiver, described in its comments and not transcribed: the rule here agrees with the one case its comments spell out, a right-angle bend eroding the cell straight ahead of the inflow) |
| K_l / K_v | `lateral_erosion` 1 | The implementation's default `Kl_ratio = 1.0`; the terrace test uses 3 for a weak rock, inside nothing published that was read |
| Whether it breaks a comb | Lateral erosion makes a network reorganise toward a dendritic one | Kwang, Langston and Parker 2021, *PNAS* 118(16). **Not read** (the publisher refused the fetch): known from its press summaries, which give 5 to 15 million years for the change. Measured here, 1.5 million years of it moved no divide ([valleys that are not a comb](../subsystems/landform.md#valleys-that-are-not-a-comb)) |

Authoring, marked so: joining the two bank models by what the bank is made of; the 64-cell floor
under which a reach is not asked; `channel_width_m` 5 m at one square kilometre with the exponent
of one half (Leopold and Maddock 1953, **not read**: the classical downstream exponent; Bieger et
al. 2015's regional curves were looked for and their coefficients not reached); the uplift's
unevenness, 0.7 over 3 km, which is the count's own result and has no source.

What was measured rather than read: a level is narrow. The network has as many levels as its
longest river has points, 735 in the bench's 1,024 by 1,024 sketch twenty steps old, so the
published way to run a sweep in parallel (Barnes's, by level) hands a thread 1,400 points at a
time at that size, which is less work than the hand-off. The subsystem page says what would pay.

## Constants, their ranges, and the defaults the schema pins

| Constant | Default | Range accepted | Status | Source or reason |
|---|---|---|---|---|
| m, n | 1/2, 1 | fixed | range | Stock and Montgomery 1999 (abstract): A^(0.3–0.5) S^1. Fixed because of determinism, below |
| Erodibility K | 5 × 10⁻⁶ yr⁻¹ | 10⁻⁸ to 10⁻² | authoring inside a range | Stock and Montgomery 1999 (abstract), for m = 0.4 in m^0.2/yr: granite and metamorphic rock 10⁻⁷–10⁻⁶, volcaniclastic rock 10⁻⁵–10⁻⁴, mudstone 10⁻⁴–10⁻². At m = 1/2 the unit is yr⁻¹ and a value converts by A^−0.1 (a fifth at 10 km²), so the default is a weak volcaniclastic or a strong sedimentary rock. What it fixes with the uplift is the steepness: at steady state S = (U/K) A^−1/2, so 1 mm/yr over the default is a steepness of 200 m |
| Hillslope diffusivity D | 0.005 m²/yr | 0 to 1 | range | Roering et al. 1999's runs, 0.003 (figure text); 10⁻³–10⁻² as the span of field values in a lecture summary of the literature (MIT OCW 12.163, **secondary**) |
| Critical slope Sc | 0.8 (38.7°) | 0.2 to 4 | range | Between the 37° modal slope of threshold hillslopes (DiBiase et al. 2012, abstract) and the 40° fitted to the Olympic Mountains (Montgomery and Brandon 2002, secondary). Stage A's 0.9 came from a secondary quote of Roering's 1.25, which is the nonlinear law's asymptote and not an angle slopes stand at ([slopes and steepness](#slopes-and-steepness-the-sources)) |
| Critical slope of the most resistant rock | 1.6 (58°) | Sc to 4 | authoring | Bare rock stands steeper than a soil-mantled slope; no source gives the figure |
| Resistance contrast | 10 | 1 to 1000 | authoring | A bed's `resistance` is the world formation's 0-to-1 ordering ([formation field](formation-field.md)); K falls from the sketch's value at 0 to a tenth of it at 1. Stock and Montgomery's lithologies span five orders, so ten between the beds of one range is modest |
| Seed roughness | 1 m | 0 to 100 | authoring | What breaks the symmetry of the starting plane so a network can form; it is gone from the result after the first few steps |
| Erodibility variation | ±25% over 16 cells | 0 to 0.9 | authoring | Rock is not uniform; without it a planar face at the critical slope sheds water along the lattice and its rills run parallel (the proof of concept's "couloir streaks") |
| Deposition coefficient G | 1 | 0 to 10 | range | Yuan et al. 2019's reference value ("typical"; G = 0.1 is close to none and the landscape changes from 1 up); inside Guerit et al. 2019's natural 0 to 3.1 and above their experiments' median of 0.7 |
| Sediment's erodibility over the rock's | 2 | 1 to 100 | range | Yuan et al. 2019 run 1, 2 and 4 |
| Share settling at a lake's mouth | 0.5 | 0 to 1 | authoring | No source; Turowski et al. 2010 (abstract) bound the bedload alone at 1% to 50% |
| Standing water | 10 mm | fixed | authoring | A point the flood raised by less is a flat a river crosses, not a lake: the flood's own ε over a wide flat is a few millimetres |
| The sweeps' tolerance | 1 mm | fixed | authoring | The millimetre the heights round to. Yuan et al. stop at 10⁻³ U Δt, 5 mm at 1 mm/yr and 5,000 years. The first build stopped at a tenth of a millimetre, finer than anything the output holds |
| A ridge's variation along its line | 0.35 over 6 km, four octaves, each 1/√2 the amplitude | 0 to 0.9; 10 m to 1,000 km | authoring, with the spectrum's form from a range | Transects of topography have spectra near k⁻² (Pelletier, Huang and Turcotte, abstracts); the amplitude and the wavelength have no source |
| Hack's exponent (a check, not an input) | — | 0.5 to 0.7 | secondary | Hack 1957 found 0.6; Gray 1961, 0.568; "empirically found to lie in the range from 0.5 to 0.7" (search extracts of Rigon et al. 1996 and its citers; none read in full) |

## What determinism decided

Every output byte has to be the same from MSVC, GCC and Clang, at either CPU baseline and at any
thread count. That decided four things.

1. **n = 1 and m = 1/2, fixed in stage A.** With n = 1 the backward-Euler update of a point is one
   division, z = (z + F z_r) / (1 + F), F = K Δt A^m / d: Barnes 2019's Newton form (Algorithm 6)
   reaches it in one exact step. With m = 1/2, A^m is one correctly rounded `sqrt`, and since
   A = N cells of spacing² and d is the spacing or its diagonal, F = K Δt √N (or that over √2): the
   spacing cancels and nothing but IEEE `* / +` and `sqrt` is in the incision. **A general m** costs
   `det::exp(m * det::log(A))` a point a step: `core/math/deterministic.h` has both, so it is
   covered and bit-identical across toolchains, at forty times the time of a square root:
   69 ns against 1.7 ns a point (the bench's `landform.area_power` rows, below), which would add
   a fifth to a step. **A general n** needs Newton's iteration, each
   round another such power of the height difference, and a convergence test whose count of rounds
   is itself a decision; `deterministic.h` covers the arithmetic, and it is what follows, not stage
   A, because field data often want n near 2 (Lague 2014, secondary) and that deserves its own tests.
2. **The flood runs on integers.** Heights are f64 metres between steps, and each step's flood
   works on the surface quantized to 1/1024 mm in a 64-bit key, the height above the index, so one
   integer comparison is the total order Barnes asks for and a tie in height goes to the lower cell
   index. ε is one unit, which is an exact increment and not `nextafter`.
3. **No C library in a decision.** `sqrt`, `floor` and IEEE arithmetic only; the ridge kernel and
   the noise are polynomials and integer hashes; contraction is off tree-wide (ADR-0035).
4. **Parallel only where the result cannot see it.** The uplift field, the quantization, the
   receivers (each point reads the filled surface, writes its own), the diffusion (a Jacobi step:
   read the old surface, write the new) and the output fields run on the job system. The flood, the
   accumulation, the incision and the slope limiter walk an order and are serial. Barnes 2019's
   levels of the order would run the last three in parallel with the same bytes; that follows.

## The model

A step of Δt years, over a lattice of points `spacing_mm` apart:

1. **Uplift.** z += U Δt at every point that is not held. U is the sketch's regional rate plus its
   ridges (the largest of the ridge kernels at the point, (1 − (r/reach)²)² of the line's rate) or
   its coarse raster, times the border taper (below).
2. **Flood and route.** Priority-Flood with ε from every held point; the receiver of a point is its
   steepest neighbour of eight on the filled surface, which always exists, so **every point drains
   to a held point**. The pop order is ascending in filled height, so it is also the order of the
   flow tree: a receiver is always popped before its donors.
3. **Drainage area.** In reverse pop order, a point adds its count of cells to its receiver's.
4. **Incision.** In pop order, a point above its receiver (and above the sea) moves to
   (z + F z_r) / (1 + F). A point in a depression is not cut (its receiver on the filled surface is
   not below it); its outlet is, at the whole basin's area, so a depression drains by its sill
   being cut, as a lake's does. A receiver below the sea counts as the sea's level: a river cuts to
   the sea and no lower.
5. **Hillslopes.** Explicit linear diffusion in as many substeps as stability needs
   (D Δt / spacing² at most 0.2 a substep), then the slope limiter, taken in pop order: a point
   is lowered to the largest height that keeps the gradient's length at the critical slope (the
   eikonal step from its lower neighbour along x and along z, and the same bound along each
   diagonal), so a slope a river has undercut fails back to the critical slope. What it removes
   leaves the model: there is no deposition in stage A.
6. After the last step the limiter runs to convergence (sweeps in four directions until nothing
   moves), so **no slope in the output exceeds its point's critical slope**, land or sea bed.

**What the first reliefs changed** (2026-11-10, a 384-point range at 32 m). The first limiter
bounded the slope to each of the eight neighbours, and the receivers were the plain steepest of
eight. The relief was facets: every failed slope an eight-sided pyramid creased along the lattice,
and on gentle ground rills in parallel along the diagonals, which is the proof of concept's
"couloir streaks" again. The eikonal step made the failed slopes cones, and Rho8's weight took the
rills off the lattice. The same run also showed how much of a range is planar when U/K is large
against the spacing ([landform](../subsystems/landform.md#the-sketch)): at a steepness of 400 m
on 32 m points nearly everything was at the critical slope.

**Resistant rock stands.** A point's resistance (uniform, or the bed of the sketch's formation its
surface cuts) divides its erodibility by up to the contrast and raises its critical slope toward
the resistant rock's. Stage A read a table of bands at the surface's present height: flat-lying
layers that never move. Stage B reads the world formation's dipping, folded beds (W85) in the
rock's own frame, so they rise with the rock ([structure](#structure-the-sources)).

**The borders take part.** The proof of concept's two worst landform defects were a ramp switched
on in one row at a region's edge and a fixed sea bed met as a 370 m underwater wall. Here:

- An **open** edge's points are held at their base level and are the outlets. Uplift is tapered to
  nothing toward every open or sea edge by a smoothstep over `border_reach_m`, so the ground meets
  the edge at the edge's level **and with no slope**: beyond the heightfield the tile set holds the
  edge flat ([tileset](../subsystems/tileset.md#the-far-mip-chain-and-its-filter)), and the solved
  ground joins that plain without a step.
- A **closed** edge is a wall nothing crosses: a divide another region shares.
- The **sea** is a level, and its bed is ground like any other: it is in the lattice, it is routed
  across to the sea edge's held points, it diffuses and it is held to the critical slope, so a
  land surface cut or raised beside it cannot leave a wall. Only the stream-power term stops at
  the waterline. A starting heightfield that has such a wall loses it.

**A starting heightfield is refined, not replaced.** With one, the surface starts as that ground
(sampled bilinearly, so a coarse solve can seed a fine one) and held points keep its heights; a
short run cuts drainage into it and the test holds the change of its low-pass under a bound.

## Why the proof of concept's pictures did not beat the version before, and what follows from it

Its hand-off says the slopes and drainage measured right and the pictures did not improve. From its
text, five causes, and what this design does about each:

| Cause | Here |
|---|---|
| Summits capped by the strip's width: a summit cannot stand higher than the critical slope times its distance to base level, and valleys dense enough to read as alpine put base level within 500–800 m of every summit | A solver cannot change this; the page says it so a sketch's author sees it. Relief comes from distance to base level: the summary the command prints carries the highest point and the mean slope, and a range that must be taller needs a wider region or a higher base level, never a steeper Sc |
| Basins filled flat (47 grey patches in its relief) | The filled surface is never written: it routes water and is kept as the `fill` field, for a later solver to make tarns of |
| Lattice-aligned streaks on planar faces | The diagonal's weight is hashed for each cell (Rho8), so the mean direction of flow on a plane is the plane's, and the erodibility varies. One receiver a point remains; see the limits |
| One smooth tower where uplift was a point with a bonus | Uplift is lines with a reach or a raster, and resistance is a band table, so a ridge is long and its crest is cut by the heads of the valleys on both sides |
| Smooth eroded slopes with a pale tiled material read as dunes; jagged rock is not a heightfield's to hold | Not a landform solver's: rock that reads as rock is the cliffs' and the terrain material's (G131, G132, R105). The fields are written so those solvers can find the steep, the wet and the fast-eroding ground |

## Limits of the model

- **Deposition without lateral erosion** (stage B). A floor is as wide as its fill reaches up the
  valley's sides: no river widens its valley by cutting its banks, so there are no strath
  terraces and a valley in ground that is rising everywhere is still a V. No scree: nothing
  stands at an angle of repose. One grain size.
- **A fan is laid along one path at a time.** With one receiver a point a mouth's deposit is a
  ridge a point wide that the next step's flood routes round; over many steps that averages to
  a fan, and a range's fans to one plain, but a single cone is not resolved, and with a long
  step the plain carries the last few steps' paths as low ridges.
- **No ice.** No U-shaped troughs, cirques, hanging valleys or overdeepened basins.
- **One receiver a point, one of eight neighbours.** Flow never divides (no braiding, no fan),
  and a single cell's choice is still a lattice direction: the hashed weight makes the mean
  direction right, and at the lattice's scale the ground is as rough as that choice. Divides
  cannot migrate by less than a cell.
- **Area for discharge, uniform rain.** No orographic precipitation, no thresholds for incision.
- **First order in time, drainage area explicit**: a large step is stable and smears a transient;
  steady state is exact (the slope-area test holds it to the fourth decimal).
- **Beds are sampled at a point**, the macro beds where the formation has them: a bed thinner
  than a point's fall is noise, and the uplift carries the beds straight up (no shear along a
  fault, no folding as the run goes on).
- **A heightfield**: no overhang, no pinnacle, no two-sided crest.

## What follows stage A

The glacial mode (an equilibrium-line altitude, ice flux, troughs and cirques); the rivers'
sweeps in the order's own sequence, and the order's levels in parallel; flow that divides (a fan as a
cone, a channel more than a point wide); lateral erosion, terraces and scree at an angle of
repose; soil depth; snow where erosion and wind put it; a general m and n; baking far-terrain maps
from near output; anything in the editor. Deposition, the formation's beds, uplift that varies
along a ridge and the flood's cost were the first of these and are stage B.

## A request to the channels generator

`channels` today routes its own lattice over the ground and begins a channel where its own D8
area and a local 4 m slope say so. With a landform beside the heightfield it should read
`drainage_area` (and `receiver` for the path) for the window its region covers, so a channel's head
and course are the ones the ground was cut by. The contract it would read is on the subsystem page
("The field file"). A request, not an edit: `domain/channels` is another row's.

## Measurements

2026-11-10. The bench's table and the machine's state are on the subsystem page
([Performance notes](../subsystems/landform.md#performance-notes)); the regressions are the tests'
(`domain/landform/tests/landform_tests.cpp`, which prints them).

| Measure | Result | Against |
|---|---|---|
| Concavity at steady state (96 points at 200 m, 1 mm/yr, K 2 × 10⁻⁵, two million years, stream power alone) | slope against area regresses to −0.4999 | the 0.5 chosen; published 0.4 to 0.6 |
| The same run's mean erosion rate | 1,001.1 µm/yr; 97% of points within 10% of the uplift, 80% within 1% | 1,000 µm/yr. The rest sit on divides whose receivers change from step to step: the explicit drainage area |
| Hack's exponent, the longest path above a point against its area, over the 1,535 points draining eight cells or more | 0.616 | Hack 1957, 0.6; published 0.5 to 0.7 |
| An open edge with the default taper (16 cells), a ridge of 3 mm/yr 3 km away, 100 m cells | the first row 1.45 m above the edge; the slope changes by at most 0.11 a row over the first four | without the taper 89 m and 1.14 |
| A coast (the golden sketch with one critical slope, 0.9) | no neighbours steeper than 0.9 by more than 0.8 mm, land, waterline or bed | the rounding to whole millimetres |
| A starting ground with a 370 m underwater wall, fifty thousand years | the largest step between neighbours 89,978 mm over a 100 m cell | 370,000 mm before; the critical slope allows 90,000 |
| A 407 m massif at 50 m refined for forty thousand years at the default erodibility, nothing raised | its 800 m low-pass moves at most 9.5 m (2.3% of the relief); single points move 29.7 m | the bound held is a twentieth of the relief |
| A 2,048-point sketch at 16 m to 100,000 years in twenty steps, four workers, busy machine | 27.1 s in stage A; 117.6 s in stage B (17.7 s at 1,024 a side, 0.58 s at 256) | the sweeps: about eight a step, two walks of the order each |
| Stage B. Steepness at steady state with G = 1 and sediment as erodible as rock (64 points at 200 m, 1 mm/yr, K 4 × 10⁻⁵, three million years) | S √A K / U = 1.955 over 572 channel points: 1.91 for 8 to 16 cells, 1.96 for 16 to 64, 2.00 above | 1 + G (N − 1) / N, Yuan et al. 2019's discrete form; 2 in the limit. The mean erosion rate is 1,000.4 µm/yr and the concavity 0.485 |
| The same with the cut first and the deposit laid after it (the first build) | 2.14: 1.92, 2.08 and 2.42 in the same three bins | the scheme that was dropped |
| The budget of the golden sketch (400,000 years) | eroded 9.061 × 10⁹ m³ = deposited 2.135 × 10⁹ + left 6.821 × 10⁹ + unrouted 1.06 × 10⁸; settled 5.6 × 10⁵ | both identities hold to nine digits |
| A V-shaped valley (sides 0.5, falling at 0.04) whose mouth is held 90 m above its bed, 100,000 years, 50 m points | the fill of 5 m or more is 3.6 points wide in its upper third and 6.7 in its lower, level across within 7.6 m, 88 m thick at most | the start's V falls 87 m over the lower floor's width |
| A massif (2 mm/yr, 900 m reach) beside a plain, 300,000 years | 400 m out the plain stands 4.6 m up opposite the massif, 2.9 and 2.8 m 700 m to either side, 0.9 m 1,700 m away; 10.0 m of sediment at the foot | a fan |
| A basin 40 m deep fed for 10,000 years | its floor level to the millimetre under 5.4 m of water with nothing settling at the mouths, within 3 mm under 8.5 m with half | — |
| A block rising at 1 mm/yr between two open edges in beds dipping 15° toward one (300 m soft under 200 m hard, a tenth as erodible), a million years, 100 m points | mean gradient 0.669 on hard rock and 0.305 on soft (2.2 to 1); 0.549 across the beds and 0.503 down the dip (1.09 to 1; 1.0002 in uniform rock); a hard bed's outcrop 9.0 points wide down the dip and 4.7 across (1.93 to 1); the divide 3.4 points off the middle | Perne et al. 2017: no fixed ratio at n = 1 |
| The default sketch: one default ridge (1 mm/yr, 4 km reach) across 8.2 km at 16 m, a million years, every default | mean slope 0.511, 27.5% of the land within a twentieth of the critical slope (0.8), the summit at 998 m. At a critical slope of 0.9: 0.546 and 24.5%. With K doubled and doubled again (0.8): 0.550 and 31.2%, 0.504 and 26.4% | DiBiase et al.: mean hillslope angles to about 35° in a range eroding this fast |
| A ridge raised for one step with no erosion, variation 0.5 over 3.2 km, read every 50 m over 25 km | the rate runs 1.483 to 2.454 mm/yr about a mean of 1.839; the mean squared difference is 0.0069 at a lag of 200 m and 0.0742 at 1,600 m | a spectrum that falls with wavenumber |
| The flood's profile at 2,048 a side (a young, rough ground; busy machine) | the route (flood, receivers, area) 812 ms; the lattice walked in the flood's order reading every neighbour, with no queue, 119 ms | stage A's reading was that the order of memory was the cost; it is the queue |
| The route of a ground with no depression at 2,048 a side, the queue kept off drained ground | 312 ms | 812 ms for the flood of every point on the young ground (no figure was taken for the old flood on the drained ground) |
| An open edge with deposition, the border test's sketch | the first row 1.41 m above the edge with G tapered toward it. With G uniform it stood 17.4 m up in the first build (the deposit laid after the cut); the uniform case was not measured again under the sweeps | the bound is 2 m |

What the first pictures showed, and what still reads wrong beside the reels, is on the subsystem
page ([Pictures](#pictures-and-what-still-reads-wrong)).

## Pictures, and what still reads wrong

**Stage E** (W126, 2026-11-11; the solver's own relief and drainage maps, outside the
repository; no `engine-view` run). The trough test's valley and the firn valley of the basins'
tests, each from stage D's solver and from this one, and the two-range sketch with its ice age
as it was and with a colder one (the line at 600 m, the ice gone one step before the end).
Beside the reels' glacial valleys (r26's level green floor with a river between walls that
rise from its edge to glaciers at the head; r08's sheer-walled valley with a lake on its
floor):

- **The trough's valley**: stage D's map shows the trench, a dark wedge that widens to the
  east edge; this one's is a band of even width, floor between walls, from the junction to the
  edge. At 128 by 96 points the map says no more than the numbers do.
- **The two ranges**: the glaciated valleys are crisper than stage D's, whose swaths smeared
  them into wide pale ribbons, and their floors read as narrow flats between steeper walls.
  **It still reads as a range cut by rivers.** No lake stands in any trough (11 points of a
  metre of water in a quarter of a million), no bowl sits at a valley's head, no ridge crosses
  a valley, and below the ice's reach the valleys are the V's they were. What r26 shows and
  this lacks, in the order a person would say it: the level floor the width of the valley
  (fill in a basin, which here the till buries before a lake can stand in it), the lake, the
  ice at the head, and everything under 32 m (rock, scree, the river's own banks).

**Stage D** (W106, 2026-11-11): **the first pictures of the ice-age ground as the renderer draws
it** (stage C's 1,024-point sketch with its ice age, read at 4 m by a run of no steps and built
as a tile set, both workarounds below; `engine-view --offscreen`, traced, 1,600 by 900, flat
tints and no material pack; eight views in two runs, outside the repository). What they show,
plainly, beside the reels (r03 and r04's valley floors under ribbed walls, r08's U-valley with
sheer granite walls, r22's cirque lake ringed by cliffs, r26 and r46's flat-floored troughs):

- **From a summit and from the air it reads as an eroded range**: branching valleys, sharp
  crests, spurs, a far range with snow on its crest, a green main valley. Those two views hold.
  The solver's relief of the same ground (stage C's `alpine-b`, written then and first read
  now) shows where the ice was as broad pale ribbons in the upper valleys that narrow to V's
  below its reach: wider valleys, and nothing else that says ice.
- **From inside a glaciated valley it does not read as one.** Looking down a side valley from
  its head (the best of the floor views), the section is an open, round-bottomed V between
  smooth walls at 35°: Svensson's parabola, and nothing a person would call a trough. No level
  floor, no step, no lake, no bowl at the head, no ridge across it. The solver's own map said
  the same in numbers: of 2,969 flooded points, 46 lie where the last ice stood.
- **Two of the four views from a valley floor are a blank slope**: at eye height in a valley a
  few hundred metres wide the frame is one wall of one tint, with nothing on it finer than the
  16 m lattice. r03's wall is ribs, ledges and scree; here it is a plane.
- **The main valley's floor is rolling fans**, smooth green domes with the range's front as
  rounded grey cones behind: desert hills, not r03's flat floor with a gravel river under a
  rock wall.
- **The beds are a staircase**: a hard bed's outcrop crosses a slope as a saw-toothed stripe a
  mask texel wide, and on the summit's near slope the 4 m masks are plainly visible as blocks.
- So, in the order a person objects: no rock and no scree at any scale under 16 m (the rock
  solvers' and the material's); no basin, lake, cirque or moraine (the design above, not
  built); no river with banks (the channels solver's); and the fans' domes.

**Stage C** (2026-11-11; the same sketch as stage B's, two ranges about a valley, 1,024 points at
16 m, 1.5 million years, solved by the command, its shaded relief and drainage map outside the
repository; no `engine-view` run was made, so nothing here is of the ground as the renderer
draws it). Beside stage B's map of the same sketch:

- **The valleys branch and bend.** They differ in size, leave the crest at angles, and join
  each other inside the range before they reach its foot; the divide wanders. Stage B's ran
  from crest to foot side by side.
- **The main valley has a river with a width**: a band several points wide where the flow
  divides over its floor, a lake where a fan has dammed it, and a floor of sediment the length
  of it. The piedmont's fans are cones with a few large rivers where there were many parallel
  threads.
- **With an ice age** (the same sketch at 512 points and 32 m, the line at 800 m for 380,000
  years, ended 20,000 years before the end) the upper valleys are wider and softer and their
  heads open; the lower valleys are unchanged.

Beside the reels (r03's valley floor under a ribbed wall with a side valley hanging above it),
what a person still objects to:

- **Most valleys still cross the range the short way.** The comb is reduced by a third by the
  count and is not gone. An uplift that is a line and a kernel will always have a short way;
  what would take it further is a sketch drawn as blocks and faults, or structure that steers
  the rivers (the strike valleys of dipping beds, which a contrast of 2 no longer cuts).
- **A trough has no level floor and no walls.** The ice's valleys are wide parabolas. r03's
  floor is flat for hundreds of metres and its wall stands up from it; that is the ice's flow
  across the section, and it is not here.
- **No cirque, no tarn, no moraine ridge.** Heads are open bowls with no lip; the till is a
  sheet where the ice melted, not a ridge across the valley.
- **The river on its floor is a band of lattice points, not a channel with banks**: at 16 m a
  river is one to five points, and what the picture needs is the channels solver cutting a bed
  of `channel_width` along it.
- **Everything is still smooth below 16 m**, and the hanging valleys have no waterfall: both
  are other solvers'.

**Stage B** (2026-11-10, the same 16 km region of two ranges about a valley, 1,024 points at
16 m, 1.5 million years, outside the repository). The sketch had to change with the model: its
ridges at 3 to 4 mm/yr over K = 3 × 10⁻⁵ solve, with deposition, to a comb of straight valleys
at a mean slope of 0.71 (a river that carries sediment stands twice as steep), and with hard
beds at a contrast of ten to 0.90, a land of cliffs. At 0.9 to 1.6 mm/yr over K = 1.2 × 10⁻⁵, no
regional uplift, a contrast of three and the default variation along the ridges, the relief
shows what stage B added: **a fan at every valley's mouth** on the plains north and south of the
ranges, each with its river's channels spreading over it, coalescing into an apron; **a floor
along the main valley**, on up to 23 m of sediment; crests that rise and fall along their length
and valleys no longer evenly spaced; and the beds as bands that cross the spurs, wider on the
side that falls with the dip. Drawn by `engine-view` (from the 16 m ground read at 4 m, as
stage A's were): from the main valley's floor the frame is a level floor, the smooth lobes of
the fans, banded slopes and a summit that stands off-centre, where stage A's same view was one
blank slope. Beside the reels (r03 and r04) a person still objects that:

- **Everything is smooth.** The fans are domes and the spurs dunes: nothing is finer than the
  lattice, the diffusion has rounded what the rivers cut, and there is one flat material. The
  reels' ground is rock, scree, turf and boulders at every scale below 16 m.
- **The floor has no river.** The channel is one point wide, and reading the ground at 4 m
  loses it; the reels' floors are a river with bars and cut banks. That is the channels
  solver's, from `drainage_area`, `receiver` and `sediment`.
- **A hard bed's outcrop is a saw-tooth**, a point wide where it crosses a slope, where the
  reels' beds are ledges and cliff bands with debris under them.
- **The faces between the gullies are still planes** at the critical slope, and the valleys of
  a range, though uneven now, still run straight down it side by side.
- **The floor is a trough, not a plain**: the two aprons meet along the river at their own
  slopes, and nothing has levelled it but one lake's fill.

**Stage A** (the same day, before deposition). A 16 km region of two ranges about a valley, 1,024 points at 16 m, 1.5 million years
(outside the repository, as every generated image is). The shaded relief is the first picture:
it reads as two eroded ranges, with branching valleys, spurs between them and crests that wander,
which a range of ridge lines with a profile does not. Drawn by `engine-view` from a tile set, from
2.6 km up it still reads as an eroded range; from a crest and from the valley floor it does not
hold. Beside the reference reels (r03 and r04, the alpine valleys), what a person would object to:

- **The faces are planes.** Nearly every slope stands at the critical angle with nothing on it
  finer than the 16 m lattice, and under one flat material that reads as dunes or folded paper,
  where the reels' walls are ribs, gullies, ledges and scree at every scale. The solver's part of
  that is the lattice (a finer one, or a refine of this ground at 4 m, which is minutes to tens of
  minutes) and the missing processes; the rest is the rock solvers' and the terrain material's.
- **No flat valley floor, no fans, no scree aprons.** Both reels stand on an alluvial floor under
  walls with pale debris cones at the foot of every gully. Nothing is deposited here, so every
  valley is a V to its mouth.
- **A channel is one point wide and follows the lattice**, and the bilinear surface between the
  points of a diagonal channel is a chain of pits (117,731 flooded points appeared when the 16 m
  ground was read at 4 m). A channel needs the channels solver's cut, not the heightfield's
  resolution.
- **The valleys of a straight range are evenly spaced and parallel**, because the uplift along a
  ridge line is uniform. A sketch with uneven rates, a raster or resistant bands breaks it; the
  model does not do so by itself.
- **Far peaks still read as cones**: seen from the valley floor the crest's summits are small
  symmetric pyramids, since a threshold slope is the same angle on every side. Asymmetry in the
  reels comes from structure (dipping beds) and ice, neither of which is here.
- **A depression's floor keeps straight lines**: where a rising ridge dams a valley the lake that
  forms is routed along the flood's order, and its floor shows it.

**The tile set could not build a heightfield whose spacing is one of its own far levels', and
now can.** Stages A to D met a 16 m ground refused at open (`16000.tiles` "is not this level's
archive": the finest level and the far chain's 16 m level were both named by their spacing), and
their pictures were taken from the same ground read at 4 m by a landform run of no steps. Since
2026-11-11 a far level's archive has a name of its own (the tile set's change, merged that
morning), so a solved ground builds at the spacing it was solved at and the 4 m read is no
longer needed; no picture has yet been taken that way.
