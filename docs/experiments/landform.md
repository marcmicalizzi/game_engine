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

What was measured rather than read: a level is narrow. The network has as many levels as its
longest river has points, 735 in the bench's 1,024 by 1,024 sketch twenty steps old, so the
published way to run a sweep in parallel (Barnes's, by level) hands a thread 1,400 points at a
time at that size, which is less work than the hand-off. The subsystem page says what would pay.

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
page ([Pictures](../subsystems/landform.md#pictures-and-what-still-reads-wrong)).
