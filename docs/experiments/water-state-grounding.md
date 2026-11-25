# Water with a state: what is published about water that moves (W173, stage 0)

(2026-11-24, roadmap W173, stage 0; pages only, nothing built or run.) The owner, after walking v59: the
sea's white caps are "a flat animation on top of the water", not a consequence of a wave breaking; a
stream's source placed in the map should fill down, a dam should fill the area until it overflows, the
ground under moving water should absorb until it saturates "so long streams don't just dry up
suddenly", and white water should be "a consequence of turbulent water as opposed to a texture to be
drawn overtop of the water, the same on streams and on waves"; the first step is "getting the elements
of simulating it right, then cutting it down for performance and shortcuts". This page is what that
design will be made from: for each subject the published models, what each computes, its constants
with their ranges and units, what it is known to get wrong, and what a real-time implementation of it
has been reported to cost. The design is the next stage's. **Section 7 is a reading for that design and
is kept apart from the sources**; nothing in it is a decision.

What the engine has today, which this page does not repeat: the steady channel solve and its sources
([stream-profile](stream-profile.md), [channels](../subsystems/channels.md#the-profile)), the plan's
lakes and their balance ([channels](../subsystems/channels.md#hollows)), the shore's models — McCowan,
Miche, Battjes, Battjes and Janssen, Longuet-Higgins and Stewart, Shen and Meyer, Stockdon —
([shore-mechanics](shore-mechanics.md#breaking)), the wet sand's drain ([shore](shore.md#how-long-the-sand-stays-wet)),
white water's physics, the whitecap cover against the wind and the foam's decay
([white-water](white-water.md#what-white-water-is)), steep streams ([steep-streams](steep-streams.md)),
falls ([waterfalls](waterfalls.md)) and the open sea's spectrum ([ocean-surface](ocean-surface.md)). Where
this page needs one of those it links it, and the reading said there stands.

## How the sources were read

One pass over the web on the evening of 2026-11-24, through the fetch and search tools only. **No PDF
was read**: every PDF the fetch reached came back as compressed bytes it could not turn into text (Mei
et al.'s, Štava et al.'s, Jákó's and Chentanez and Müller's papers among them), so a paper is read here
only where an HTML page carries it — an abstract page, an arXiv rendering, a manual, an encyclopaedia
article, a patent, a project page. Each source in [the list](#the-sources-and-how-far-each-was-read)
says which of four it is: **read** (the page itself, whole for what is taken), **abstract** (the
abstract or a project page, and nothing past it), **secondary** (a page that quotes or restates the
source), and **search summary only**, which is **not read**: a figure that came only from a search
engine's summary is named as such where it appears and is never used as a constant. A number derived on
this page from read numbers is marked *derived*, with the arithmetic beside it.

## What has no state today

Every water the engine draws is a function of the clock and of fields built once at load. A channel is
the steady solve's level, wet reach, speed and Froude number at its stations
([water](../subsystems/water.md#channels)): nothing fills, nothing drains, and a discharge that changes
is a new cook. A body is a level over a flooded hollow, still or lifted by a wave train whose height,
breaking and setup are a load-time energy-flux field ([water](../subsystems/water.md#the-surf)). The
foam at a shore is a sum over the last waves of a periodic train
([water](../subsystems/water.md#the-swash)), the swash a ballistic closed form, the wet sand's age a
closed form of the train's group; a stream's white is the solve's Froude number and slope read per pixel.
So the four things the owner asks for are each missing for the same reason: **there is no depth or
velocity that anything can change**, no ground store that fills, and no record of where energy was lost.

## 1. Depth and velocity over terrain

### The shallow-water equations

The depth-averaged equations of an incompressible layer under gravity over a bed z(x, y): mass,
∂h/∂t + ∇·(h**u**) = r − i (rain, a source, less what infiltrates), and momentum,
∂(h**u**)/∂t + ∇·(h**u**⊗**u**) + ∇(g h²/2) = −g h ∇z − **τ**_b/ρ, with a bed friction that open-channel
work takes from Manning, **τ**_b/ρ = g n² |**u**| **u** / h^(1/3) (n the bed's Manning coefficient,
s m^(−1/3); the engine's channels read 0.03 to 0.05, [flowing-water](flowing-water.md)). The state is
three numbers a place: h, hu and hv. They assume a pressure that is hydrostatic down the vertical, which
holds when the flow runs along a bed of small slope; the information they carry travels at
u ± √(g h), which is the speed a stability limit is set by.

| depth h | 1 cm | 5 cm | 10 cm | 50 cm | 1 m | 3 m | 10 m | 30 m |
|---|---|---|---|---|---|---|---|---|
| √(g h), m/s (*derived*) | 0.31 | 0.70 | 0.99 | 2.21 | 3.13 | 5.42 | 9.90 | 17.2 |

**What they get wrong.** Anything the hydrostatic, depth-averaged layer cannot hold: an overturning
crest, a jet in the air, a plunge pool's three-dimensional roller, spray. In shallow water with no
dispersion every wave steepens until it is a bore (a shock), which is right inside the surf and wrong
offshore, where waves are dispersive; a solver that must carry waves over depth needs a dispersive
extension (Boussinesq-type equations, [§4](#4-breaking-as-something-simulated)). On steep ground the
small-slope assumption is the weak point: the search summaries of Denlinger and O'Connell (2008, a
nonhydrostatic model for steep terrain) and of Maranzoni and Tomirotti (2022, steep-slope shallow-water
equations) say the classical form is derived for slopes under about a tenth and departs from them
substantially past about 20° — **search summaries only, not read**, so neither number is used here.

**Roll waves.** A uniform flow down a steep bed is itself unstable when the Vedernikov number V passes 1;
V / F = β − 1, β the exponent of the discharge's growth with the area, so in a hydraulically wide
channel **the neutral Froude number is 2 under Chézy friction and 3/2 under Manning's**
([Ponce](https://ponce.sdsu.edu/the_vedernikov_number.html), read). Past it a smooth sheet breaks into
a train of steep surges travelling downstream — a real phenomenon, which the design of steep lined
canals must check for (the page's examples: a channelised river in La Paz at a slope of 0.06, a
photograph in the Swiss Alps); the instability is necessary and not sufficient. A solver that reproduces this on a steep shooting reach is not unstable:
it is right. One that is asked to hold such a reach still is asking the physics for something it does
not do.

### What hydrology uses instead: three cheaper waves

Hydrology drops terms from the momentum equation and keeps the mass equation whole:

- **The local inertial approximation** (Bates, Horritt and Fewtrell 2010, the LISFLOOD-FP scheme) drops
  the advection term and keeps the local acceleration, the pressure gradient, the bed slope and
  friction; flows along x and y are decoupled. Its time step follows the CFL condition and so scales with
  Δx, where the diffusive storage-cell model before it scaled with Δx², which made its stable step **one
  to three orders of magnitude larger** at typical cell sizes and its runs up to **1,120 times faster**
  in their tests; small instabilities and larger depth errors appeared at a Manning's n of 0.01
  ([abstract](https://www.fathom.global/academic-papers/a-simple-inertial-formulation-of-the-shallow-water-equations-for-efficient-two-dimensional-flood-inundation-modelling/)).
  It agrees with the full equations in the lower range of subcritical flow, **Fr under about 0.5**, and
  propagates floods more slowly than they do in unsteady problems (de Almeida and Bates 2013, as a
  search summary of its abstract gives it: **not read**). Cozzolino et al. (2019,
  [abstract](https://arxiv.org/abs/1904.12564v2), read) go further: under the approximation a shock
  always forms at a moving wet/dry front, **a wet bed cannot dry**, some Riemann problems have no
  solution even on a smooth bed, a flood cannot pass a bed drop of the order of its front's height, and
  mechanical energy is not conserved; they call its limits "discouragingly severe" and prefer the full
  equations for most practical uses.
- **The diffusion (diffusive, zero-inertia) wave** drops both acceleration terms: the water surface's
  slope balances friction, so the discharge follows the surface's gradient through Manning's law. It
  carries backwater (a downstream level is felt upstream), so a hollow fills to a level and a pool
  stands; it is HEC-RAS's default two-dimensional set, recommended for flood extents, levee breaches
  and dam-break inundation, and not for sharp contractions and expansions, detailed velocity fields,
  mixed regimes or hydraulic jumps
  ([CivilGEO's summary of HEC-RAS's options](https://knowledge.civilgeo.com/hec-ras-2d-computational-equations-comparison),
  secondary; the HEC-RAS manual itself was not reached). An explicit diffusive scheme's stable step
  falls with Δx² (above).
- **The kinematic wave** keeps only the balance of the bed's slope against friction, so the discharge is
  a function of the depth alone (Manning's law, q = h^(5/3) S^(1/2) / n per unit width) and the wave
  travels downstream at dq/dh with no attenuation. With one characteristic, pointing downstream, it
  **has no backwater: it cannot fill a hollow or stand a pool behind a dam** (*derived* from the
  equation's form, the textbook reading; the Ponce pages below say the same of its zero diffusion).

**Where each stops being right** (Ponce, Li and Simons 1978,
[online version](https://ponce.sdsu.edu/applicability_of_kinematic_and_diffusion_models.html), read; and
[Ponce's note on the diffusion wave](https://ponce.sdsu.edu/when_is_the_diffusion_wave_applicable.html), read).
For a disturbance of period T on a bed of slope S₀ under a flow of depth d₀ and speed u₀:

| model | criterion for 95 % accuracy after one period | what it means |
|---|---|---|
| kinematic | T S₀ u₀ / d₀ ≥ **171** (99 %: 873; 90 %: 83) | long slow waves on steep beds |
| diffusion | T S₀ (g / d₀)^½ ≥ **30** (16 for Fr 0.1–0.4, 45 for Fr 0.01–1) | a wider range, with physical attenuation |
| dynamic (full) | below both | short, fast changes: the only one that holds them |

The papers' own examples: overland flow on a slope of 0.01 at 1 ft deep and 4 ft/s is kinematic for
periods past 1.2 h and diffusive past 8.8 min; a river at a slope of 10⁻⁴, 10 ft deep at 3 ft/s, needs 66
days to be kinematic and 1.9 days to be diffusive. **The steeper the bed, the shorter the period each
holds for**: a mountain stream is the kinematic wave's best case and the full equations' worst
(*derived*, below), which is why hydrology routes steep channels kinematically and why a steady
solve on Manning's law (W120's) is already the kinematic answer for constant inflow. Neither cheap wave
holds a step's plunge, a jump or a pool's backwater on a steep stream: those are local and fast.

**Manning's law on a steep bed** is itself the weak point: a fixed n is far too smooth for shallow flow
over clasts, which is why [stream-profile](stream-profile.md#sources) took Jarrett's (1984) resistance
n = 0.32 S^0.38 R^−0.16 (R in metres; slopes 0.002 to 0.04 or 0.09 by the quotation), already read and
pinned there.

### Height-field solvers in graphics

- **Kass and Miller (1990)**: the linearised shallow-water (wave) equation on a height field with the
  speed √(g h) and an implicit integration — named by every later paper below; **not read** (its PDF did
  not parse).
- **The pipe model** (O'Brien and Hodgins 1995; Mei, Decaudin and Hu 2007; Štava et al. 2008). Columns
  of water on a grid exchange water through virtual pipes at their bottoms; a pipe's flux is accelerated
  by the difference of the two columns' surfaces (water plus ground), f ← f·damping + g A Δ(surface)
  Δt / l, the column's depth changes by the net flux over its area, and a column's total outflow is scaled
  by min(1, its water × area / (outflow × Δt)) so that **no column goes negative** — the wet/dry rule
  is that scaling. The fluxes persist from step to step, so the model has inertia, but **no advection
  of momentum**: a fast stream entering a lake does not carry into it but spreads in every direction,
  and two streams side by side flowing opposite ways do not interact if their levels agree; a wave
  forms where water first enters dry ground (the equations and these limits as
  [a WebGPU implementation's write-up](https://lisyarus.github.io/blog/posts/simulating-water-over-terrain.html)
  gives them, secondary; its time steps 0.001 to 0.01 s on cells of about a metre, the step bounded by
  the CFL condition, no timings). Mei et al.'s abstract ([hgpu](https://hgpu.org/?p=2256)) claims
  interactive rates and gives no numbers; their slides
  ([text](https://slidetodoc.com/fast-hydraulic-erosion-simulation-and-visualization-on-gpu/)) give
  512 × 512 grids for the demonstrations and interactive rates up to 1,024 × 1,024 on a GeForce 8800 GTX,
  name "the limitation for the time step" as the fluid solver's weakness, and show no equation.
- **Staggered grids with semi-Lagrangian advection** (Chentanez and Müller 2010; Thürey et al. 2007).
  The full nonlinear equations, depth at cell centres and velocities on faces, momentum advected
  semi-Lagrangian; Chentanez and Müller's abstract (a search summary of the Eurographics page; **the page
  itself refused the fetch and the PDF did not parse**) claims arbitrary terrain slopes, varying depth,
  wet and dry regions, and particles for what the height field cannot hold, in real time on the GPU, with
  no figure. Thürey et al.'s solver as its patent describes it
  ([US 8,204,725](https://patents.google.com/patent/US8204725), read) is the same discretisation,
  inviscid, on the CPU or GPU, with no performance figure.
- **Finite-volume, well-balanced, positivity-preserving schemes** (Audusse et al. 2004's hydrostatic
  reconstruction; Kurganov and Petrova 2007's central-upwind scheme). They conserve mass exactly, keep a
  **lake at rest** exactly at rest over any bed (well-balanced), keep depths non-negative, capture bores
  as shocks with the right jump conditions and allow a discontinuous bed. Brodtkorb, Sætra and Altinakar
  ([abstract](https://hgpu.org/?p=6665), read) ran such a scheme in single precision on a GeForce GTX 480:
  the Malpasset dam break's first 4,000 s on over 480,000 cells of 15 m in 27 s, **530 megacells a
  second** on average, single precision "sufficiently accurate for real-world simulations". Which
  scheme it is the abstract does not say. SERGHEI-SWE (Caviedes-Voullième et al. 2023,
  [GMD 16, 977](https://gmd.copernicus.org/articles/16/977/2023/gmd-16-977-2023.html), read for what is
  taken) is the same family at first order — augmented Roe-type Riemann solvers that keep the balance
  and the depth's positivity over rough topography with wetting and drying, forward Euler, CFL ≤ 0.5,
  double precision — and on the Malpasset case one A100 ran 6.5 times a 48-core CPU node (a V100 3.4,
  an RTX 3070 2.2); it gives no rate in cells a second in its text, and its authors note that a valley
  dry for long periods balances badly across devices. A 2025 study of the same code on A100, H100,
  MI250X and Intel Max devices ([arXiv 2511.01001](https://arxiv.org/html/2511.01001v1), read) puts
  most of its kernels in the **memory-bound** region of the roofline, the flux kernels about 40 % of
  the time: such a solver's cost is the bytes a cell reads and writes a step, which is the number a
  design can budget before it measures.
- **Boussinesq-type solvers** add dispersion for waves that cross depth; Celeris (Tavakkol and Lynett
  2017; [arXiv abstract](https://arxiv.org/abs/1611.05984), read) solves the extended equations by a
  hybrid finite-volume and finite-difference scheme in Direct3D shaders with a moving shoreline, faster
  than real time, validated on breaking and non-breaking benchmarks; a figure of about 50 times real time
  on about 10⁴ points is a GTC poster's as a search summary gives it, **not read**.

### Stability: the step, by the cell and the water

An explicit scheme's step obeys Δt ≤ C Δx / max(|u| + √(g h)); Bollermann, Chen, Kurganov and Noelle
([2014, ar5iv](https://ar5iv.labs.arxiv.org/html/1412.3580), read) run with C = 0.5 by their definition.
At that number (*derived*):

| case | Δx | u, h | Δt ≤ |
|---|---|---|---|
| a stream 5 cm deep at 1 m/s | 0.25 m | 1 m/s, 0.05 m | 0.074 s |
| a still pool 1 m deep | 0.5 m | 0, 1 m | 0.080 s |
| a surf zone 3 m deep at 2 m/s | 0.5 m | 2 m/s, 3 m | 0.034 s |
| the sea 10 m deep | 2 m | 0, 10 m | 0.10 s |
| a 2 m fall's foot (√(2 g h) = 6.3 m/s), 5 cm thick | 0.25 m | 6.3 m/s, 0.05 m | 0.018 s |

So at the world's own rate **one step a 60 Hz frame** (16.7 ms) is inside the limit in every case
above, the fall's foot only just — and a fall is a nappe a height field does not hold anyway
([waterfalls](waterfalls.md)); a world clock run faster than real time multiplies the steps by its rate,
and a finer cell divides the step in proportion. A diffusive explicit scheme has no such bound by speed
but one by Δx² over the diffusivity, which grows without bound on a flat water surface — the reason the
storage-cell model's steps were so small (Bates et al., above).

## 2. A source that fills downhill, a dam that fills and overflows

**What each family does at a wet/dry front.**

- **Pipe model**: the outflow scaling keeps every column non-negative, so a front advances cell by cell
  as water arrives and a hollow fills to its spill and spills; with no momentum advection the front is
  pushed by the surface's slope alone, so a sheet running down a steep slope reaches the bottom and
  spreads, and its run-out is not carried (read, secondary, above).
- **Finite-volume shallow water**: positivity is not free. Bollermann et al. (read) show that Kurganov
  and Petrova's correction for a negative reconstructed depth breaks the scheme's balance, and that with
  forward Euler a step small enough to guarantee positivity at a partly wet cell can be arbitrarily
  small; their remedy keeps the free surface level across a partly flooded cell (well-balanced at the
  front) and cuts only the outgoing fluxes of a cell that would run empty within the step (a local
  "draining time"), so the global step stays the CFL step, with velocity taken as zero where a cell's
  depth is under 10⁻⁹. Their tests are gentle slopes only (about 0.05 and ±3°), so **how such a front
  behaves on a mountain stream's slope is not in what was read**.
- **Local inertial**: cannot dry a wet bed and stands a shock at every moving front (Cozzolino et al.,
  above): a pool cannot drain away through it and a source's front is a step.
- **Diffusion wave**: fills hollows and stands pools (backwater), the way flood-inundation tools fill a
  floodplain; its explicit step collapses where the surface is flat.
- **Kinematic wave**: fills nothing; the steady channel solve's lakes are found by another algorithm.

**Pooling and spilling.** Two published ways: a dynamic solver that is well-balanced (a lake at rest
stays exactly at rest, so a full hollow neither leaks nor sloshes numerically), with the outflow over the
lip found by the equations themselves (over a broad crest the flow passes critical depth, the textbook
reading; the weir law Q = C L H^(3/2) is the steady consequence, C on average 3.33 in
[the encyclopaedia's form](https://en.wikipedia.org/wiki/Weir), read, which does not state its units); and a static one that the channels
capability already uses for the year's mean — Barnes et al.'s Fill-Spill-Merge, each hollow's balance of
inflow against evaporation and seepage deciding its level ([channels](../subsystems/channels.md#hollows)).
The second is the steady state the first reaches under constant inflow, which makes it the first's
initial condition and its check.

**How long filling takes** (*derived*, a hollow of vertical sides: volume over inflow): the plan's
smallest basin, 200 m² filled a metre, at 10 l/s is **5.6 hours** and at link 17's 0.29 m³/s 11 minutes;
a 40 m pond with a metre of mean depth (1,257 m³) at the 44 l/s of the stretch near mark 482 is **7.9
hours**. At the world's own rate a dam an author builds across a forest stream fills over a working
day; that is the physics, and it is a design question (section 7), not a solver's.

## 3. The ground takes water until it is saturated

**The laws** ([the encyclopaedia's infiltration article](https://en.wikipedia.org/wiki/Infiltration_(hydrology))
and [its sorptivity article](https://en.wikipedia.org/wiki/Sorptivity), read for the forms below):

- **Green and Ampt (1911)**: a sharp wetting front moving down into soil of uniform initial moisture;
  the rate f = K (1 + ψ Δθ / F) and the cumulative depth F = K t + ψ Δθ ln(1 + F / (ψ Δθ)), with K the
  saturated conductivity, ψ the suction at the front, Δθ the moisture deficit (saturated less initial)
  and the ponded depth taken as negligible. The rate starts high (capillarity dominates while F is
  small) and **falls towards K, not to zero**: over a deep unsaturated soil the front keeps moving down
  under gravity at K forever. The cumulative form is implicit and is solved numerically.
- **Horton**: empirical, f = f_c + (f₀ − f_c) e^(−k t), cumulative f_c t + (f₀ − f_c)(1 − e^(−k t)) / k;
  f_c the rate after saturation, which the SWMM guidance sets to the saturated conductivity.
- **Philip (1957)**: the series solution's first two terms, cumulative I = S t^½ + A₁ t and rate
  i = ½ S t^(−½) + A₁, with S the sorptivity (m s^(−½), the capacity to take water by capillarity);
  valid for short times only (how short is itself disputed, as a search summary of Vrugt and Hopmans
  says: not read).
- **Kostiakov**: f = a k t^(a−1), which goes to zero; the Kostiakov–Lewis form adds a final rate.
- **Richards' equation**: the general unsaturated-flow equation these approximate; nonlinear, expensive,
  not guaranteed to converge.

**Constants by soil** (Rawls et al. as tabulated by two manuals, read; SI by *derived* conversion, 1 in/h
= 25.4 mm/h). The two tables disagree on K by up to a factor of four (loam), and agree on silt loam;
that is the spread between the two publications as the manuals give them, not a transcription here:

| texture | porosity θ_s (range), 1993 | ψ, 1993 (mm) | K, 1993 (mm/h) | K, 1983 (mm/h) |
|---|---|---|---|---|
| sand | 0.437 (0.374–0.500) | 49.5 | 236 | 120 |
| loamy sand | 0.437 (0.363–0.506) | 61.2 | 59.7 | 30.0 |
| sandy loam | 0.453 (0.351–0.555) | 110 | 21.8 | 10.9 |
| loam | 0.463 (0.375–0.551) | 88.9 | 13.2 | 3.3 |
| silt loam | 0.501 (0.420–0.582) | 167 | 6.9 | 6.6 |
| sandy clay loam | 0.398 (0.332–0.464) | 218 | 3.0 | 1.5 |
| clay loam | 0.464 (0.409–0.519) | 209 | 2.0 | 1.0 |
| silty clay loam | 0.471 (0.418–0.524) | 273 | 2.0 | 1.0 |
| sandy clay | 0.430 (0.370–0.490) | 239 | 1.3 | 0.5 |
| silty clay | 0.479 (0.425–0.533) | 292 | 1.0 | 0.5 |
| clay | 0.475 (0.427–0.523) | 316 | 0.5 | 0.25 |

The 1993 columns are [TxDOT's hydraulic design manual's](https://www.txdot.gov/manuals/des/hyd/chapter-4--hydrology/section-13--hydrograph-method/green-and-ampt-loss-model.html)
table, citing Rawls et al. (1993); its second moisture column (0.417 for sand) is labelled the initial
content there and is not used. The 1983 K is [a SWMM help page's](https://tbn2net.com/help/SOLIDOSen/SwmmInfiltration.html)
minimum Horton rate, citing Rawls, Brakensiek and Miller (1983, *J. Hydraulic Engineering* 109). The
same page's Horton guidance: **f₀ for dry soils with little vegetation 127 mm/h on sand, 76 on loam, 25
on clay, twice that under dense vegetation; divided by 3 at field capacity and by 1.5 to 2.5 when partly
dry; the decay k 2 to 7 per hour; a saturated soil drying completely in 2 to 14 days**. Field
conductivities spread over decades more (gravel to clay); the encyclopaedia's table of them (Bear 1972,
via Freeze and Cherry) is an image that did not read.

**What a stream loses** (*derived*). A bed losing at K a square metre (Green and Ampt's late rate, the
unit-gradient limit) takes from a stream 1 m wide, per metre of channel: on the 1993 sand 65 ml/s, on loam
3.7 ml/s, on clay 0.14 ml/s. A 10 l/s stream would be gone in about **150 m of sand, 2.7 km of loam (10.9
km at the 1983 value), 70 km of clay**. While the bed is still taking water by capillarity the rate is
higher: with loam's ψ of 89 mm and a deficit of 0.3 (a choice for the example), f is 3.7 K at F = 10 mm
and nears K once F passes ψ Δθ ≈ 27 mm, an hour or two of loam's K. So a stream released onto dry ground
advances slowly over a bed that drinks hard, and once the bed is wet its loss settles to K; **it stops
losing only where the ground below has no more room** — a finite soil store full, or a water table at the
surface. A metre of loam with a deficit of 0.3 holds 300 mm, saturated in about 23 hours at loam's K
(*derived*).

**Losing and gaining reaches** ([USBR, Yuma](https://www.usbr.gov/lc/yuma/programs/YAWMS/GROUNDWATER_river.html),
read, after a USGS study). A stream gains where the water table beside it stands above its surface and
loses where it stands below; a **connected** losing reach touches the saturated zone and loses by the
difference between its stage and the water table, a **disconnected** one has an unsaturated zone under it
and loses at a rate independent of that difference, and if its seepage outpaces what the saturated zone
carries away a groundwater mound grows under it. A river can turn from gaining to losing during a high
flow until its stage recedes (the Colorado near Yuma after the 1983 flood). The USGS's own circular on
the subject refused the fetch; bank storage and the hyporheic exchange are therefore **not read**. Losses
along ephemeral channels are standardised per kilometre of reach in the transmission-loss literature
(Lane 1983 and the Walnut Gulch studies), as search summaries say: **not read**, no figure used.

**What a cell needs** (from the laws' forms): Green and Ampt needs, per place, the depth already taken
F (or the front's depth) against static K, ψ and Δθ; Horton the time since the soil was dry, or its
current capacity, against f₀, f_c and k with a recovery rate between wettings (SWMM's drying time); a
store that ends the loss needs its capacity (the soil's depth times its deficit) and what it holds; a
connected reach needs a water table's height. The smallest that gives "absorbs until saturated" is
**one number a cell, the water the ground there holds, against a capacity and a K that are static** —
section 7 weighs it.

## 4. Breaking as something simulated

### At a shore

**The criteria** are on [shore-mechanics](shore-mechanics.md#breaking) and stand: depth-limited breaking
at H = 0.78 d (McCowan), the steepness limit H/L = 0.142 tanh(k d) (Miche), the breaker's type by the
surf similarity number (Battjes 1974), a random sea's dissipation as a bore's times the fraction broken
(Battjes and Janssen 1978). Offshore, the probability that a dominant wave breaks is near zero below a
threshold of the significant steepness and grows near-quadratically past it, correlated at 0.78 over Lake
Washington, the Black Sea and the Southern Ocean for wavelengths of 3 to 300 m and winds of 5 to 20 m/s
(Banner, Babanin and Young 2000, [abstract](https://researchportalplus.anu.edu.au/en/publications/breaking-probability-for-dominant-waves-on-the-sea-surface/),
read; the threshold's value, 0.055 in a later paper's quotation, is a search summary's: **not read**).

**How a phase-resolving solver breaks a wave** ([FUNWAVE-TVD's documentation](https://fengyanshi.github.io/build/html/wavebreaking.html),
read):

- **By the shock itself** (Tonelli and Petti 2009): where the wave height over the total depth passes
  **0.8**, the Boussinesq equations are switched to the nonlinear shallow-water equations in those cells,
  and the shock-capturing scheme treats the broken front as a bore; the bore's jump conditions dissipate
  the energy a broken wave loses, with no breaking parameter beyond the switch.
- **By an eddy viscosity** (Kennedy et al. 2000): a viscosity ν = B δ_b² (h + η) η_t is added where the
  surface rises fast, δ_b = 1.2, B = 1 while breaking; breaking starts where η_t passes
  **C₁ √(g h)** and stops where it falls under **C₂ √(g h)**, Kennedy's C₁ = 0.65 and C₂ = 0.15 (0.45 and
  0.35 for the Vincent and Briggs benchmark; Choi et al. 2018 found C₁ should be smaller and C₂ larger to
  match laboratory data). FUNWAVE-TVD starts the viscosity at once, without Kennedy's ramp, having met no
  instability.
- **The roller** (Schäffer, Madsen and Deigaard 1993): a volume of water carried on the front face at
  the wave's speed, whose thickness comes from the face's geometry, with a 20° greatest breaking angle that
  caps the roller's thickness ratio at 0.1638 in FUNWAVE's form, and a roller speed of 1.3 √(g h) in the
  original, √(g d) in FUNWAVE's (read on the same page); the roller's excess momentum drives the undertow.
  The original's terminal angle and its decay time are **not read**.

**Graphics' height-field breaking** (Thürey et al. 2007 via [the patent](https://patents.google.com/patent/US8204725),
read): after each shallow-water step, grid points whose surface gradient exceeds t_H = ¼ g Δt / Δx and
whose velocity opposes the gradient (∇H · **u** < 0, the front of a wave and not its back) are
candidates; regions two cells round them are grouped, a wave line is traced along each crest a cell apart
and advected, and an overturning sheet of particles is launched from it at the line's speed raised by the
crest's height; where the sheet falls back it adds height to the grid and spawns spray and foam
particles. No lifetime or count is given. Chentanez and Müller (2010) turn what the height field cannot
hold into spray, splash and foam particles exchanging mass and momentum with the grid (abstract, as a
search summary gives it).

**Where the energy goes.** In a shock-capturing solver the loss is the bore's: per unit width, the jump's
head loss (h₂ − h₁)³ / (4 h₁ h₂) times the flux through it ([below](#in-a-stream)). A large share of it goes
into entraining air against buoyancy — 30 to 50 % in Melville, Loewen and Lamarre's 1992 review, as a
search summary gives it: **not read**. The whitecap's foam cover is proportional to the dissipation
([§5](#5-foam-as-a-consequence)).

### In a stream

**Hydraulic jumps against the Froude number** ([the encyclopaedia's article](https://en.wikipedia.org/wiki/Hydraulic_jump),
read): Bélanger's sequent depth h₂/h₁ = ½(√(1 + 8 Fr₁²) − 1) and the head loss (h₂ − h₁)³ / (4 h₁ h₂):

| Fr₁ | h₂/h₁ | jump | energy dissipated |
|---|---|---|---|
| ≤ 1 | 1 | none | 0 |
| 1–1.7 | 1–2 | undular | < 5 % |
| 1.7–2.5 | 2–3.1 | weak | 5–15 % |
| 2.5–4.5 | 3.1–5.9 | oscillating | 15–45 % |
| 4.5–9 | 5.9–12 | steady | 45–70 % |
| > 9 | > 12 | strong | 70–85 % |

A jump is about 4 (weak) to 6.2 (steady and strong) downstream depths long; the ranges overlap. The
engine's solve already places jumps and Smetana's roller length ([stream-profile](stream-profile.md#sources)),
and natural jumps in a step-pool bed are submerged and aerated unevenly
([white-water](white-water.md#what-white-water-is)). An undular jump (Fr₁ under 1.7) is standing waves
with no white: that is where the engine's standing-wave band already sits.

**Steps and plunges** are falls ([waterfalls](waterfalls.md)): a nappe's break-up length and its foot's
plunge are there, and a height field holds neither the nappe nor the pool's roller; a state on a grid can
take the foot as a source of momentum and air at the foot's cell.

**Self-aeration on a steep chute** ([Chanson's page](https://staff.civil.uq.edu.au/h.chanson/self_aer.html),
read): free-surface aeration starts at the **inception point, where the turbulent boundary layer growing
from the bed reaches the surface** (widely accepted for steep chutes, not on flat waterways); downstream
the mean air concentration tends, on a very long chute, to an equilibrium **C_e = 0.9 sin θ** of the slope
θ (the page's footnote); the entrained air lowers the friction (the drag reduction: a friction ratio of 0.92 at
a mean air content of 0.2, 0.52 at 0.5, 0.25 at 0.7). The speed a surface needs to entrain air, about a
metre a second, is [white-water's](white-water.md#what-white-water-is).

## 5. Foam as a consequence

**Whitecap cover against wind** stays as [white-water](white-water.md#what-white-water-is) read it:
Monahan and O'Muircheartaigh's W = 3.84 × 10⁻⁶ U₁₀^3.41, the sum of the active crests (stage A) and the
decaying patches (stage B). **Against dissipation** (Callaghan 2018, *J. Phys. Oceanogr.* 48,
[abstract](https://spiral.imperial.ac.uk/handle/10044/1/63522), read): from an energy balance, the whole
cover and the cover of the growing phase are each **linearly proportional to the breaking dissipation
rate** and **inversely proportional to the depth the bubble plume reaches** during active breaking; the
depth varies with the wave's scale and slope, so no single relation of dissipation to cover holds
everywhere; above about 12 m/s the dissipation is largely balanced by the wind's input, below it the ratio
falls quickly under one. Callaghan, Deane and Stokes (2024, a search summary of the abstract: **not
read**) found laboratory and ocean whitecaps' scaled foam areas evolving alike, with similar growth and
decay times.

**How long foam and a plume last.** A whitecap's foam decays with e-folding times of 0.2 to 10.4 s an event,
longer for larger patches (Callaghan et al. 2012, [white-water](white-water.md#what-white-water-is)'s
reading). The plume under it loses most of its air within the first wave period (Lamarre 1993's thesis, 95
% of the entrained volume as a search summary gives it: **not read**). Fresh water's bubbles merge and
burst within about a second and a stream's white clears over V d / u_r, a plume's depth d carried at the
speed V while its bubbles rise at u_r ≈ 0.25 m/s (white-water's reading, recalled there).

**How foam is carried.** A surface concentration φ (the share of the surface white, or foam's area density)
riding the surface's velocity **u**_s obeys ∂φ/∂t + ∇·(φ **u**_s) = S − φ / τ (*derived*: conservation
of a surface tracer with a source and an exponential decay, not a law read from a source): where the
surface diverges (behind a bore, where a jet surfaces, a boil) foam spreads and thins; where it converges
(eddy lines, the edge of a backwater, a pool's slack side) it gathers into lines and rafts, which is what
the engine's lace already draws as lines
([water](../subsystems/water.md#the-foams-pattern)). S is the breaking's: proportional to the dissipation
(Callaghan) and gated by the speed that entrains air (Chanson); τ is fresh water's second or the sea's
seconds. The same equation serves a stream's white downstream of a step and a beach's foam behind a bore.

## 6. What interactive graphics has shipped or published

Few of the published real-time methods give a cost with a grid, a step and a time; the table says what
each gives and how it was read.

| method | what it simulates | stated cost | read |
|---|---|---|---|
| Kass and Miller 1990 | linearised height field, implicit | — | not read |
| Mei, Decaudin, Hu 2007; Štava et al. 2008 (pipe model) | water and erosion on a terrain height field, GPU | 512² in demonstrations, interactive to 1,024², GeForce 8800 GTX; no times | abstract, slides' text |
| Planet Coaster 2 | wave pools and shore waves by the hydrostatic pipe model on 1 m cells, CPU SIMD eight cells at once, waves made by removing and adding water; ripples and flow round guests by a 2-D shallow-water solver (semi-Lagrangian, Crank–Nicolson, preconditioned conjugate gradient) on the GPU with a foam layer carried by the flow | no times; stability needed "trial and error" at up to five times real time | [Game Developer deep dive](https://www.gamedeveloper.com/programming/deep-dive-crafting-detailed-and-dynamic-water-in-planet-coaster-2), read |
| Timberborn | the pipe model, by a third party's account | — | not read (the developers' article was not found) |
| Yuksel, House, Keyser 2007, wave particles | surface waves as particles splatted to a warped height field; reflections off boundaries; no significant global flow | 1,681 boats and 295,856 faces at 4.8 fps, GeForce 7900 GTX, the rigid bodies the bottleneck | [project page](http://www.cemyuksel.com/research/waveparticles/), read |
| Thürey et al. 2007 | breaking fronts detected in a shallow-water grid, overturning sheets of particles | none | patent, read |
| Yu, Neyret, Bruneton, Holzschuch 2009 | a river's steady flow from its boundaries, detail advected in screen space | — | search summary only |
| Chentanez and Müller 2010 | full shallow water on terrain with wet/dry, particles for spray, splash, foam; small waves carried by the flow | "real time" on the GPU, no figure | search summary of the abstract |
| Brodtkorb, Sætra, Altinakar 2011 | well-balanced finite volume with dry states, single precision | 530 Mcells/s, GTX 480 | abstract |
| Tavakkol and Lynett 2017, Celeris | extended Boussinesq with moving shoreline, breaking, Direct3D | faster than real time | abstract |
| Jeschke et al. 2018, water surface wavelets | amplitudes over space, frequency and direction, which vary slowly and so relax the CFL and Nyquist limits; obstacles, two-way coupling | "very large" domains in real time, no figure | [abstract](https://research-explorer.ista.ac.at/record/134) |
| Jeschke et al. 2020, boundary-aware procedural waves | procedural ocean waves meeting terrain, with particles for breaking, foam and spray | interactive on a commodity PC | [abstract](https://research-explorer.ista.ac.at/record/8766) |
| Unreal Engine's Niagara Fluids | a 2-D shallow-water template for pools, wakes and simple interactions; 3-D FLIP for beaches and rivers, at a high cost | resolution against cost, no figure | search summary of Epic's documentation |

**The pattern across them**: a shallow-water or pipe-model grid where water must go somewhere (a pool, a
river, a flood), and phase-resolved waves on top by another representation (spectra, particles,
wavelets); breaking, spray and foam as particles or a carried layer born where the grid detects a front
it cannot hold. No method read couples a grid's breaking to a foam whose source is its dissipation with
a cost attached.

## 7. A reading for the design, kept apart from the sources

Everything below is this page's reading of the above, written for the design stage; it decides nothing.

### Which of today's closed forms become consequences, and which stay

| today | with a state | why |
|---|---|---|
| the steady channel solve (W120) | **the start and the far field**: the state's initial condition, and what is drawn where no state runs | it is the kinematic/gradually-varied answer the dynamic equations converge to under constant inflow, so it is the state's check too |
| a still body's level over a flooded hollow, the plan's balance | **the start**; a level that moves where the state runs | Fill-Spill-Merge is the steady state of a well-balanced solver under the same inflow |
| the open sea's spectrum, the whitecap cover by wind | **the far field**, and the boundary condition that drives a surf zone's state | no grid holds a sea; the spectrum is where the waves come from |
| the arrival and flux fields, Battjes and Janssen's kept share, the setup | **consequences** inside a surf zone's state: refraction, shoaling's steepening, breaking as bores and the setup come out of the equations | the shallow-water equations carry all three where kh is small; offshore they need dispersion (Boussinesq) or the spectrum's field |
| the ballistic swash (Shen and Meyer), Stockdon's run-up | **consequences**: a wet/dry front on a beach climbing and falling | Shen and Meyer's projectile is the shallow-water equations' own solution at the shore; Stockdon is a field fit to what such fronts do |
| the wet sand's drain (exp(−t / `drain_s`)) | **a consequence**: the ground's store filling and draining at its K | the shore page already names infiltration as the cause |
| the banks' capillary fringe | **stays**: a static height over whatever the water's edge now is | capillarity is the grain's, not the flow's |
| a stream's white by Froude number and slope, the plunge discs, `water_bore_foam` | **consequences**: a foam concentration sourced where the state loses energy (a bore, a jump, a step's foot) and carried by it | the owner's sentence |
| the lace, the threads, the streaks (W152, W157) | **stay**, as how a concentration under a pixel looks | a grid cell is decimetres; the lace is the sub-cell pattern |

### The smallest state for the owner's four things

1. **A source fills down**: depth and velocity per cell, h and h**u** (three numbers), with positivity at
   fronts (the pipe model's scaling, or a positivity-preserving finite volume's draining time).
2. **A dam fills and overflows**: the same three, **well-balanced** so a full pond stands still, and enough
   momentum for the spill over its lip to be the equations' (the diffusion wave does it too; the
   kinematic wave and the local inertial approximation do not drain or do not stand).
3. **The ground absorbs until saturated**: one number per cell, the water the ground there holds, with a
   static capacity (depth times deficit), a K and a capillary term from the bed's material (Green and
   Ampt's form, the loss falling from capillarity's to K and to nothing when the store is full). A
   water table, for gaining reaches, is a second number or a static field.
4. **White water from breaking and turbulence**: a surface concentration φ per cell, sourced by the energy
   the state loses there and carried and decayed by the equation of §5. The loss is what the scheme
   dissipates at a shock (a bore, a jump), or an eddy-viscosity onset of η_t > 0.65 √(g h), or a front
   test like Thürey's; gated by the entrainment speed.

That is **five numbers a cell that change** (h, hu, hv, the ground's store, φ) beside static ones the
terrain and the channels already hold (the bed's height, Manning's n, K, ψ Δθ, the store's capacity).
As 32-bit floats for the flow and 16-bit for the store and the foam, about 16 bytes a cell; with a bed
height of its own, 20 to 24 (*derived*, a starting estimate). For scale: a corridor ten metres wide along
the forest's 36.9 km of solved channel at 0.5 m is about 1.5 million cells, 35 MB at 24 bytes; a
3.5 km beach's 200 m surf zone at 0.5 m is 2.8 million cells, 67 MB (*derived*). Brodtkorb et al.'s
530 Mcells/s on a 2010 GTX 480 would take that surf zone about 5 ms a step; what the RTX 5090 does is
**not measured** and must be before the design leans on it. Since such kernels are memory-bound
(SERGHEI's study), the first estimate to test is the bytes a cell moves a step — its state read, its
neighbours' read, its new state written — times the cells, over the card's bandwidth.

### Questions the design must answer, with the evidence each way

- **Which equations, where.** The full equations are the only ones that hold a jump, a pool's backwater
  and a drying front together and keep a lake at rest; their cost is the CFL step, about one a frame at
  the world's rate on decimetre cells (§1). The diffusion wave is cheaper in a step but its explicit step collapses on flat
  water; the local inertial approximation is cheap and cannot dry; the kinematic wave holds a steep
  stream's routing for long disturbances and cannot pool. A **mix** — kinematic routing along steep
  links, the full equations in pools, at confluences, behind dams and at the shore — is how hydrology
  couples one-dimensional and two-dimensional reaches; the mix's seams are where mass must be counted.
- **Steep ground.** The equations' slope assumption weakens past about a tenth (not read), roll waves are
  physical past Fr 1.5 on Manning friction (read), and the read wet/dry schemes were tested on gentle
  slopes only. A grid over a 35 % step-pool stream at decimetre cells resolves neither the steps' jets
  nor the clasts; there the steady solve's reach-averaged answer plus the steps as sources may be the
  honest state, and the full equations the pools'.
- **The grid and its LOD.** A uniform grid near the eye with the steady solve beyond; nested grids with
  conservative transfer; or channel-aligned one-dimensional cells for long links and two-dimensional
  tiles where water spreads. What crosses a level's edge must conserve mass, or a stream gains or loses
  water at an LOD seam; a tile that leaves the eye's range must hand its water to the far field without a
  jump.
- **Time.** At the world's own rate a dam fills over hours (§2). Whether an author's dam may be filled
  faster (the world rate, which multiplies steps), whether a tile far from the eye steps at a coarser
  clock, and what a load starts from (the steady solve, so nothing needs a spin-up under constant
  inflow) are the design's.
- **The sea.** A grid cannot hold the sea; a surf corridor can, driven at its seaward edge by the
  spectrum's train. Non-dispersive equations break every wave as a bore by the time it reaches the surf,
  which is right in the inner surf and early offshore of it; Boussinesq-type equations break later and
  cost more a cell. Celeris and FUNWAVE show both run interactively, and neither gave a cost that was read.
- **The ground's state across a session.** A store that fills over hours and drains over days (SWMM's 2 to
  14) is state a save carries and a streamed tile keeps; the steady solve's year-mean seepage is the
  far field's.
- **Determinism and the frame.** An explicit finite-volume step in a fixed order is the same bytes twice
  on one device (no atomics in the flux sum); positions on the grid are cell-local (ADR-0053's
  [no absolute position on the GPU](../adr/0053-world-positions-are-f64-and-the-gpu-sees-none.md)); a
  replay needs the inflows and every author's edit on the world clock.
- **What drawing reads.** Today the mesh and the depth map are built at load; a state means a surface
  that moves, so the mesh's heights and the depth the resolve reads come from the grid each frame, and
  the foam's lace reads φ. Under [11 §11.10](../plan/11-performance-principles.md#1110-absent-capabilities-are-free)
  a scene with no state pays nothing, and a state's pools are made on first use.

## The sources, and how far each was read

| source | what is taken | read |
|---|---|---|
| Audusse, Bouchut, Bristeau, Klein, Perthame (2004), hydrostatic reconstruction | named as well-balanced and positive | as Bollermann et al. credit it |
| Banner, Babanin, Young (2000), *J. Phys. Oceanogr.* 30, 3145–3160 | breaking probability against significant steepness: a threshold, near-quadratic past it, r = 0.78 (0.81 with wind and shear); 3–300 m, 5–20 m/s; visual detection | abstract; the threshold's value not read |
| Bates, Horritt, Fewtrell (2010), *J. Hydrology* 387, 33–45 | the local inertial scheme; step ∝ Δx against Δx²; 1–3 orders larger steps; up to 1,120 times faster; instability at n = 0.01 | abstract (Fathom's page) |
| Bollermann, Chen, Kurganov, Noelle (2014), arXiv 1412.3580 | wet/dry fronts in central-upwind schemes; positivity against balance; draining time; CFL 0.5; ε = 10⁻⁹; gentle-slope tests only | read (ar5iv) |
| Brodtkorb, Sætra, Altinakar (2011/2012), *Computers & Fluids* 55, 1–12 | Malpasset, 480,000 cells of 15 m, 4,000 s in 27 s, 530 Mcells/s, GTX 480, single precision | abstract (hgpu) |
| Caviedes-Voullième et al. (2023), SERGHEI-SWE v1.0, *GMD* 16, 977 | first-order augmented Roe scheme, well-balanced and positive with wet/dry, CFL ≤ 0.5, double precision; Malpasset speed-ups A100 6.5, V100 3.4, RTX 3070 2.2 against a CPU node; dry valleys balance badly | read (the HTML article, the parts named) |
| Villalobos, Caviedes-Voullième, Rizzi, Meneses (2025), arXiv 2511.01001 | SERGHEI's kernels memory-bound on A100, H100, MI250X, Intel Max; flux kernels about 40 % of the time | read (the HTML rendering) |
| Callaghan (2018), *J. Phys. Oceanogr.* 48, 2609–2626 | whitecap cover ∝ dissipation, ∝ 1 / plume depth; dissipation ≈ wind input above 12 m/s | abstract |
| Callaghan, Deane, Stokes (2012, 2024) | foam decay 0.2–10.4 s (2012, white-water's reading); lab and field alike (2024) | 2012 as white-water read it; 2024 search summary only |
| Chanson, self-aeration on chute and stepped spillways (UQ page) | inception where the boundary layer meets the surface; C_e = 0.9 sin θ; drag reduction table | read |
| Chentanez, Müller (2010), SCA | full shallow water on terrain with wet/dry, particles for what it cannot hold | search summary of the abstract; the page refused the fetch, the PDF did not parse |
| CivilGEO, HEC-RAS 2D equation sets | diffusion wave, SWE-ELM, SWE-EM, local inertia: where each serves and fails | read (secondary to the HEC-RAS manual, which was not reached) |
| Cozzolino et al. (2019), *Adv. Water Res.*, arXiv 1904.12564 | local inertia: shock at moving fronts, no drying, no solution for some problems, energy not conserved | abstract |
| de Almeida, Bates (2013), *WRR* 49 | local inertia good for Fr < 0.5 | search summary only |
| Denlinger, O'Connell (2008); Maranzoni, Tomirotti (2022) | the slope limit of the hydrostatic equations | search summaries only |
| FUNWAVE-TVD documentation, "Wave breaking, roller and undertow" | Tonelli–Petti's 0.8 switch; Kennedy et al.'s eddy viscosity (δ_b 1.2, C 0.65/0.15, 0.45/0.35); Schäffer et al.'s roller (20°, 0.1638, 1.3 √(g h)) | read |
| Game Developer, Planet Coaster 2's water (2024) | pipe model on 1 m cells, CPU SIMD; GPU shallow water with a carried foam layer | read |
| Hydraulic jump, the encyclopaedia's article | Bélanger, head loss, the classes by Fr₁, lengths | read |
| Infiltration (hydrology) and Sorptivity, the encyclopaedia's articles | Green and Ampt, Horton, Kostiakov, Philip, Richards | read |
| Jeschke et al. (2018), *ACM TOG* 37(4); (2020), *CGF* 39(8) | wavelets; boundary-aware procedural waves with particles | abstracts |
| Kass, Miller (1990); O'Brien, Hodgins (1995); Mei, Decaudin, Hu (2007); Štava et al. (2008) | the height-field and pipe models | Mei et al.'s abstract and slides' text; the rest named only, their PDFs did not parse |
| Kurganov, Petrova (2007), *Comm. Math. Sci.* 5 | well-balanced, positivity-preserving central-upwind | search summary of the abstract; read through Bollermann et al. |
| lisyarus, "Simulating water over terrain" | the pipe model's equations and limits as implemented | read (secondary) |
| Ponce, Li, Simons (1978), online version; Ponce's diffusion-wave and Vedernikov pages | kinematic ≥ 171, diffusion ≥ 30; Vedernikov, F_ns 2 (Chézy) and 1.5 (Manning) | read |
| Rawls et al. (1983, 1993) via TxDOT's manual and a SWMM help page | Green–Ampt parameters by texture; Horton's guidance | read (secondary) |
| Tavakkol, Lynett (2017), Celeris | Boussinesq on the GPU, faster than real time | abstracts (arXiv, ICCE) |
| Thürey, Müller-Fischer, Schirm, Gross (2007) via US 8,204,725 | breaking-front detection and sheets | read (the patent) |
| USBR, "Groundwater basics: interaction with rivers" | gaining, losing, connected, disconnected; the mound | read |
| Weir, the encyclopaedia's article | Q = C L H^n, n 3/2 for a horizontal weir | read |
| Yuksel, House, Keyser (2007), wave particles | method, limits, the boat scene's cost | read (project page) |
| Not read at all | the USGS circular on ground and surface water (refused), Lamarre's thesis and Lamarre and Melville (1991), Melville, Loewen and Lamarre (1992), Schäffer et al.'s decay rule, Lane (1983) and the Walnut Gulch losses, Yu et al. (2009), From Dust, Timberborn's developers' article, Bear's conductivity table | — |
