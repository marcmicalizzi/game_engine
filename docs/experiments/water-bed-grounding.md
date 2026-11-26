# The bed as a state: what is published about a bed that water moves (W178, stage 0)

(2026-11-25, roadmap W178 stage 0, with W179 and W180 surveyed; pages only, nothing built or run.) The
owner, the same day: a walker or an animal crossing a river stirs its bed; an object placed in a river
or taken out of it diverts the flow and the sediment builds and scours to suit; "loose sediment
particles in the flowing water clouding it temporarily"; a tree knocked into a river, the river swelling
over it and turning a little, sand laid down round the trunk and the bank cut where the flow quickens;
ripples in sand that weather, tides and waves change; a storm that changes a shore. Rivers stay mostly
baked, the steady solve's bed the start and the far field, and the bed moves where something disturbs it
and the game's settings permit. The water's own state is W173's
([water-state-grounding](water-state-grounding.md)); this page is what the bed's design will be made
from: for each subject the published laws, their constants by grain size with their ranges, and where
each stops being right. **Section 10 is a reading for the design and is kept apart from the sources**;
nothing in it is a decision.

What the engine's pages already hold, which this page does not repeat: a gravel bed's median grain from
the bankfull Shields number and the threshold's rise with slope (Lamb, Dietrich and Venditti 2008;
[channels](channels.md#the-bed)); the shoreface's equilibrium profile, the depth of closure, the beach
face, the beach's state, cusps and the longshore drift ([coast](coast.md)); the horseshoe vortex and the
surface over an obstacle ([water-obstacles](water-obstacles.md)); breaking, setup and the swash
([shore-mechanics](shore-mechanics.md)); and the desert's aeolian ripples, a wind's and not a water's
([ripple-motion](ripple-motion-2026-10-04.md)). Where this page needs one of those it links it.

## How the sources were read

One pass over the web on the evening of 2026-11-25, through the fetch and search tools only, as text;
nothing was saved. **No PDF was read**, as on W173's page: a paper is read here only where an HTML page
carries it. Each source in [the list](#the-sources-how-far-each-was-read-and-under-what-licence) says
how far it was read — **read** (the page itself, for what is taken), **abstract** (the abstract or a
record page and nothing past it), **secondary** (a page that restates the source, the source not seen)
and **summary**, which is **not read**: a figure that came only from a search engine's summary is named
as such where it appears and is never a constant — and **under what licence its text stands**, read
before anything was taken from it. Two of the most used pages carry licences that limit reuse of their
*text*: the Coastal Wiki's footer is CC BY-NC-SA 4.0 by a search summary of it (its own pages fetched
tonight print no licence line), and the National Academies' and NALMS' pages are all rights reserved.
From those only the published equations and numbers they restate are taken, which are their cited
authors' and not the page's text, and nothing is quoted. No code was read for this page; XBeach's
manual (documentation, which states no licence for its text) is read for the published forms it
describes and nothing of the program. A number computed on this page from read numbers is marked
*derived*, with its inputs beside it; a statement written from the author's memory is marked
**memory** and is never pinned.

Common symbols: D (or d50) the grain's diameter, s = ρ_s/ρ the specific gravity (2.65 for quartz),
R = s − 1 = 1.65 the submerged one, ν the water's kinematic viscosity (10⁻⁶ m²/s at 20 °C), g = 9.81 m/s²,
h the depth, U the depth-averaged speed, τ_b the stress at the bed and u* = √(τ_b/ρ) its velocity,
p the bed's porosity, κ = 0.4.

## 1. The threshold of motion

**Shields' parameter** (the encyclopaedia's articles on sediment transport and on the Shields
parameter, read) is the stress at the bed made dimensionless by the grain's submerged weight,
θ = τ_b / ((ρ_s − ρ) g D); a grain moves where θ passes a critical θ_c, a property of the grain and not
of the flow. The classical constants the article gives: **0.06** for a uniform bed (Shields), **0.047**
(the value inside Meyer-Peter and Müller's law), **0.03** (Parker 1990, which the article applies in one
section to mixed beds and in another to well-sorted ones: inconsistent as written).

**The modern fit by grain size** is Soulsby's (1997), on the dimensionless grain size
D* = D (R g / ν²)^(1/3) (the Coastal Wiki's "Sand transport", read, eqs. 2 and 3):

    θ_c,motion = 0.30 / (1 + 1.2 D*) + 0.055 (1 − exp(−0.020 D*))
    θ_c,susp   = 0.30 / (1 + D*)     + 0.1   (1 − exp(−0.05 D*))

and two critical depth-averaged speeds for sand (eqs. 6, 7), U_cr = 0.19 D^0.1 log₁₀(2h/D) for
0.1 < D < 0.5 mm and 8.5 D^0.6 log₁₀(2h/D) for 0.5 < D < 2 mm (SI). *Derived*: 0.25 mm sand under 0.5 m
of water starts to move at about 0.30 m/s, 1 mm sand at about 0.40 m/s.

*Derived* from Soulsby's fit, with the settling speed of §4 beside it (Ferguson and Church, natural
grains) and the Rouse number P = w_s/(κ u*) at the threshold's own u*:

| D | D* | θ_c | τ_c (Pa) | u*_c (cm/s) | w_s (mm/s) | P at u*_c |
|---|---|---|---|---|---|---|
| 0.063 mm (silt/sand boundary) | 1.6 | 0.105 | 0.11 | 1.0 | 3.3 | 0.79 |
| 0.125 mm (fine sand) | 3.2 | 0.066 | 0.13 | 1.2 | 11 | 2.4 |
| 0.25 mm (medium sand) | 6.3 | 0.041 | 0.17 | 1.3 | 32 | 6.2 |
| 0.5 mm (coarse sand) | 12.7 | 0.031 | 0.25 | 1.6 | 71 | 11 |
| 1 mm | 25 | 0.031 | 0.51 | 2.3 | 126 | 14 |
| 2 mm (granule) | 51 | 0.040 | 1.3 | 3.6 | 196 | 14 |
| 8 mm (gravel) | 202 | 0.055 | 7.2 | 8.5 | 413 | 12 |
| 32 mm (coarse gravel) | 809 | 0.055 | 29 | 17 | 830 | 12 |

So **fine sand and finer go into suspension as soon as they move** (P near or under 2.5 at the
threshold), medium sand and coarser roll and hop first, and gravel needs a flood. Silt and clay finer
than 0.063 mm are left out of the table because they are cohesive and Shields' curve does not hold for
them: by the Coastal Wiki's bedform page as a search summary gives it, Shields is a reasonable
approximation for non-cohesive grains above about 0.1 mm (**summary**); a mud content past 20 to 40 %
raises sand's threshold as τ_c,sand = (1 + p_mud)^3 τ_c,Shields (the "Sand transport" page, read).

**Slope.** On the bed's own slope β a grain is helped down and held up, θ_c' = θ_c cos β (1 ± tan β /
tan φ), φ the angle of repose, 30 to 40° (Fredsøe's form as XBeach's manual gives it, read). Down a
steep stream the threshold rises instead, τ*_c = 0.15 S^0.25 over S 0.001 to 0.1, because shallow flow
over clasts loses its stress to form drag (Lamb, Dietrich and Venditti 2008, read there:
[channels](channels.md#the-bed)).

**Where the threshold stops being right.** Shields' data were taken over flat beds and mean a state where
a small share of the bed moves, 1 to 10 % (the Coastal Wiki's sand page as a search summary gives it:
**summary**); a mixed bed hides its fines behind its coarse grains and armours (the reason Wilcock and
Crowe's law below is written per fraction); a cohesive bed needs an erosion law of its own (not read);
and past θ ≈ 1/s ≈ 0.4 the bed goes to **sheet flow**, a moving layer several grains thick with no
ripples (read, the sand page).

## 2. Bedload, suspended load, and what separates them

### Bedload

**Meyer-Peter and Müller (1948)**, as both pages read give it (the encyclopaedia; the Coastal Wiki's
sand page, which advises the constants and gives the spread of fits):

    q_b / √(R g D³) = m (θ − θ_c)^n,   m = 8, n = 1.5, θ_c = 0.047   (fits: 4 < m < 12, 1.4 < n < 1.65)

The encyclopaedia adds the transport-stage variants (a coefficient 5.7 near the threshold, 12.1 far
past it) and Wiberg and Smith's generalisation with m = 1.6 ln θ + 9.8 ≈ 9.64 θ^0.166. **Wong and Parker
(2006)** re-ran Meyer-Peter and Müller's own data and found volumes at most about half the original's
(the ASCE abstract as a search summary gives it: **summary**; their corrected coefficients were not read
and are not used here). For a **mixture**, Wilcock and Crowe (2003) per fraction i (the encyclopaedia,
read): W*_i = R g q_bi / (F_i u*³) = 0.002 φ^7.5 for φ < 1.35 and 14 (1 − 0.894 φ^0.5)^4.5 above, φ the
stress over the fraction's reference stress (whose hiding function the article does not give: not read).
**Total load without a threshold**: Engelund and Hansen, q / √(R g D³) = 0.05 c_f θ^2.5 (the
encyclopaedia; the sand page writes m = 0.04/c_D). **Van Rijn (2007)**, for sand under currents and
waves (the sand page, read, eq. 10): q_b = 0.015 ρ_s U h (D/h)^1.2 M_e^1.5, with the mobility
M_e = (U_e − U_cr)/√(R g D) and U_e = U + γ U_w (γ 0.4 for irregular waves), U_w the peak orbital
velocity at the bed.

*Derived*, to give the scale: 1 mm sand under 0.5 m of water at 0.38 m/s with Manning's n of 0.03 has
τ_b/ρ = g n² U² / h^(1/3) = 0.0016 m²/s², θ = 0.10, and moves by Meyer-Peter and Müller
**q_b ≈ 1.2 × 10⁻⁵ m²/s** a metre of width (43 litres of grains an hour across each metre of bed).

### Suspended load, and the criterion between the two

**The Rouse number** P = w_s / (κ u*) (the encyclopaedia, read; κ 0.4 there, 0.407 in another section):

| P | mode |
|---|---|
| > 7.5 | no motion |
| 2.5 – 7.5 | bedload |
| 1.2 – 2.5 | suspended, 50 % |
| 0.8 – 1.2 | suspended, 100 % |
| < 0.8 | wash load: carried through, barely settling |

The concentration's profile up the depth is Rouse's, c/c_a = [((h − z)/z)(a/(h − a))]^P (the sand page,
eq. 9; the encyclopaedia's form adds an exponent ratio α of 0.93 to 1.10 for sands and silts). The sand
page's other criteria: suspension where the bed's shear velocity passes the fall speed; or, the more
physical by the page, U > U_cr,susp = 2.8 (h/D)^0.1 √(R g D) for 0.1 to 2 mm. Van Rijn's suspended
transport (eq. 11): q_s = α_s ρ_s U D M_e^η D*^−0.6, α_s 0.008 and η 2.4 (the TR2004 fit).

### The exchange with the bed, in a depth-averaged model

A depth-averaged solver does not carry the profile; it carries one concentration C per class and
exchanges with the bed at a rate. Two published forms, read:

- **Erosion against deposition** (the coupled shallow-water and sediment model of arXiv 2505.22278,
  read, CC BY 4.0): ∂(h c)/∂t + ∇·(h c **u**) = E − D, with deposition D = w_s c_b, c_b the near-bed
  concentration, a profile factor times the mean (the paper's Bradford factor, c_b = c (0.4 (d/D_g)^1.64
  + 1.64)), and an entrainment E = w_s (1 − p) E_s with E_s = 1.3 × 10⁻⁷ Z⁵ / (1 + 4.3 × 10⁻⁷ Z⁵), Z a
  function of the shear velocity over the fall speed and the particle Reynolds number (its eqs. 69 and
  after; the form is Garcia and Parker 1991's by its constants, **memory**, the paper's attribution not
  read).
- **Relaxation to an equilibrium concentration** (XBeach's manual, read, eqs. 87 and 88):
  ∂(hC)/∂t + ∇·(hC **u**) − ∇·(D_h h ∇C) = (h C_eq − h C) / T_s with the adaptation time
  T_s = max(f_Ts h / w_s, T_s,min): the concentration relaxes towards the flow's carrying capacity at
  about the time a grain takes to settle through the depth. C_eq is Soulsby and van Rijn's,
  (A_s / h)(√(U² + 0.018 u_rms² / C_d) − U_cr)^2.4, with A_s the bed and suspended coefficients of the
  grain (eq. 100), or van Thiel and van Rijn's (eq. 105). **This is the form every graphics erosion
  method of §9 uses with a capacity of its own choosing**, and the form a game can afford.

The time the load lags its flow: 0 to 60 minutes over a tide, negligible for sand coarser than about
0.3 mm (the sand page, read). The water's own mass changes by (E − D)/(1 − p) where the exchange is fast
(the paper's mass equation): negligible for clouding, not for a bed scoured a metre in a flood.

## 3. The bed's conservation, and coupling it to the shallow-water step

**Exner** (the encyclopaedia, read): ∂η/∂t = −(1/ε₀) ∇·**q**_s, η the bed's height and ε₀ = 1 − λ_p the
packing, **0.45 to 0.75** in nature, about 0.64 random close packing as the practical ceiling (so a
porosity of 0.25 to 0.55). With the suspended exchange (arXiv 2505.22278, eq. 69, read):

    ∂η/∂t + ∇·(**q**_b / (1 − p)) = (D − E) / (1 − p)
    ∂h/∂t + ∇·(h **u**) = (E − D) / (1 − p)
    ∂(h **u**)/∂t + … = −g h ∇η − (g h² / 2ρ)(ρ_s − ρ) ∇c + (E − D) **u**_b / (1 − p) − friction

(the third line abridged; the density gradient term is the concentration's weight, which a clouding
load does not need and a turbidity current does). XBeach writes the bed's update with the acceleration
factor of §6 inside it, ∂z_b/∂t + (f_mor / (1 − p)) ∇·**q** = 0 (eq. 130, read). Where the bed's slope
passes a critical value it **avalanches**: XBeach's are 1 above the water and 0.3 below it (the Coastal
Wiki's dune-erosion page, read; the manual's own sentence is ambiguous on which is which), the same rule
graphics calls thermal erosion (§9). **Downslope transport**: q_slope = q (1 − α ∂z_b/∂s) (Soulsby's
form in the manual, eq. 124, read); α was not read.

**What the bed needs from the flow each step**: the stress at the bed τ_b as a vector, or the depth h,
the velocity **u** and the friction law that turns them into it (the water's own Manning term,
τ_b/ρ = g n² |**u**| **u** / h^(1/3), [water-state-grounding §1](water-state-grounding.md#1-depth-and-velocity-over-terrain));
under waves the orbital velocity at the bed U_w (or u_rms) and its excursion a, which the van Rijn and
Soulsby–van Rijn laws read and every ripple predictor reads (§7); the bed's own slope; and for the
suspended classes the concentration the flow carries. **What it gives back**: a changed bed height,
which the water step must then hold well-balanced (a lake over a changed bed stays at rest); the
exchange's mass; a concentration per class, which the renderer reads (§4); and a roughness that changes
with the bed's forms (§7).

**Stability, and the step.** The usual scheme steps the water, then the bed (splitting). Cordier, Le
and Morales de Luna (2011) show that splitting can make spurious oscillations because an eigenvalue of
the coupled system can differ in sign from the split one, and that with Meyer-Peter and Müller's law the
coupled system is hyperbolic, and so linearly stable, only under a constraint on the velocity (the
abstract as a search summary gives it, the HAL page refused the fetch: **summary**). **The bed's own
wave is slow.** The low-Froude linearisation (**memory**: the standard result, not read tonight) gives
the bed's celerity c_b ≈ (1/(1 − p)) (∂q/∂U)(U/h) / (1 − Fr²). *Derived* for the 1 mm example of §2
(q ∝ (θ − 0.047)^1.5, θ ∝ U², Fr 0.17): c_b ≈ **2.4 × 10⁻⁴ m/s, under a metre an hour**, against the
water's U + √(g h) = 2.6 m/s — four orders of magnitude, which is why a bed can step far less often than
its water and why the coastal models can quicken its clock (§6). The same form shows where that breaks:
the denominator vanishes near Fr = 1, where bed and surface interact strongly (antidunes, §7), which is
where splitting is weakest. Carraro et al. (2018; arXiv 1912.05245, abstract read) accelerate both mass
equations (MASSPEED) with factors chosen from the flux matrix's approximate eigenvalues, adaptively, "in
a way similar to the CFL condition"; Ranasinghe et al. (2011, abstract read) propose a critical
acceleration from the CFL condition of bedform migration, its number not read.

## 4. Settling, and what a suspended load does to the water's look

**Settling speed** of a natural grain (Ferguson and Church, the encyclopaedia, read):
w_s = R g D² / (C₁ ν + √(0.75 C₂ R g D³)), with (C₁, C₂) = (18, 1.0) for natural grains by sieve
diameter, (20, 1.1) by nominal diameter, (18, 0.4) smooth spheres, (24, 1.2) the angular limit; Stokes'
law is its small-grain end. *Derived* (sieve constants, quartz, 20 °C), with the time to settle a metre
and how far a stream 0.5 m deep at 0.5 m/s carries a cloud stirred to its full depth before it clears:

| D | w_s | a metre in | carried (0.5 m, 0.5 m/s) |
|---|---|---|---|
| 4 µm (clay/silt) | 0.014 mm/s | 19 h | 17 km |
| 16 µm (fine silt) | 0.23 mm/s | 73 min | 1.1 km |
| 63 µm (coarse silt) | 3.3 mm/s | 5 min | 77 m |
| 0.125 mm | 11 mm/s | 90 s | 23 m |
| 0.25 mm | 32 mm/s | 31 s | 8 m |
| 1 mm | 126 mm/s | 8 s | 2 m |

(The carried distance ignores turbulence, which keeps the fines up as long as P < 0.8, §2: in a moving
stream a clay cloud does not clear at all until the water slows.) Clay flocculates, settling as larger,
looser aggregates; hindered settling at high concentration (Richardson and Zaki) — **neither read**.

**Clarity against concentration.** A grain much larger than the wavelength removes light over twice its
cross-section, so a mass concentration C_k (g/m³) of grains of diameter d_k adds to the beam attenuation
c = c_w + Σ_k b_k C_k, with the mass-specific attenuation **b_k = 3 / (ρ_s d_k)** for solid spheres; a
black target is seen to the distance where ∫c ds = 4.8, so **y = 4.8 / c** in uniform water (Zaneveld and
Pegau 2003); clear water's c_w 0.16 m⁻¹ gives 30 m (arXiv 2611.12895, eqs. 22, 23, read, under arXiv's
non-exclusive licence; it cites Boss et al. 2009 for the size dependence and Davies-Colley and Smith 2001
for fine silt dominating attenuation). Natural waters measure **0.05 to 1.5 m²/g**, a decade's spread
driven mostly by the particles' apparent density (77 % of its variance), not their size (Neukermans et
al. 2012; **summary**), and aggregated marine particles sit at 0.2 to 0.6 m²/g, nearly insensitive to
size (Boss, Slade and Hill 2009; **summary**). Black-disc visibility converts to beam attenuation
directly, and both are better measures than nephelometric turbidity (Davies-Colley and Smith 2001,
abstract as a summary gives it); the three are only broadly correlated with the mass in suspension
(NALMS' page, read, all rights reserved). *Derived*, sighting range y = 4.8 / (0.16 + b C), solid quartz:

| C (mg/l) | 4 µm (b 0.28 m²/g) | 16 µm (0.071) | 63 µm (0.018) | 0.25 mm (0.0045) |
|---|---|---|---|---|
| 1 | 11 m | 21 m | 27 m | 29 m |
| 10 | 1.6 m | 5.5 m | 14 m | 23 m |
| 100 | 0.17 m | 0.66 m | 2.4 m | 7.8 m |
| 1,000 | 2 cm | 7 cm | 26 cm | 1.0 m |

So **what clouds water is silt and clay**: a gram of clay hides as much as sixty of medium sand, and a
footprint's sand falls out of sight within a stride while its silt drifts on. **Colour** is the
particles' absorption against their scattering: mineral-rich, small particles absorb more blue per
mass than larger, more organic ones in the St Lawrence estuary (Mohammadpour et al. 2017, abstract
read; its licence not read on the page, so its numbers are not taken); the absorption spectrum by mineral (iron-stained
clay's red-brown, a glacial flour's milky blue-grey) was **not read** and is the ground type's to carry
(W183). What the renderer would read is a concentration per class, a b per class from its diameter (or
a measured value inside the natural range), and an absorption colour per ground type.

## 5. Scour and deposition round an obstacle

**The mechanism.** The approach flow slows against the obstacle's face, its stagnation pressure drives a
downflow along the face that digs at its foot once it overcomes the grains' resistance, and the shear
layer rolls into a horseshoe vortex round the foot that carries the hole round the sides
([water-obstacles](water-obstacles.md); the encyclopaedia's bridge-scour article, read, names the
downflow). Behind the obstacle the wake's slower water lays the load down. **Clear-water scour**: the
approach bed is below its threshold, the hole deepens until the local stress falls back to it; **live-bed
scour**: sediment arrives from upstream and the hole stands where inflow and outflow balance. The
equilibrium clear-water depth is typically about 10 % deeper than the live-bed one (the same article),
and the deepest scour can come under the clear-water conditions before a flood's peak, not at it; a
cohesive or cemented bed scours no less deep, only more slowly (read, the same).

**Depth.** The HEC-18 (CSU) pier equation, HEC-18 5th edition 2012 (Ponce's calculator page, read):

    y_s / y₁ = 2 K₁ K₂ K₃ (a / y₁)^0.65 Fr₁^0.43,   Fr₁ = V₁ / √(g y₁)

a the obstacle's width, y₁ and V₁ the approach depth and speed, K₁ the nose's shape, K₂ the angle of
attack, K₃ the bed's condition. The factors' values (K₁ 1.0 for a round nose and 1.1 for a square one,
K₃ 1.1 for clear water) are a search summary's (**summary**); the caps on y_s / a recalled as 2.4 for
Fr ≤ 0.8 and 3.0 above are **memory**. The encyclopaedia says HEC-18 over-predicts under some
conditions. *Derived* (K₁K₂K₃ = 1.1): a trunk standing 0.4 m wide in 0.8 m of water at 0.6 m/s
(Fr 0.21) scours **0.58 m**, 1.4 widths. A log **lying** on the bed is a horizontal cylinder, and pier
equations do not describe it: the scour under a pipeline (Sumer and Fredsøe's, of the order of the
cylinder's diameter: **memory**) and a log jam's backwater and bank attack were **not read**; the
owner's tree, laid across a channel, also narrows it, and that narrowing is **contraction scour**, which
HEC-18 adds separately (the encyclopaedia's article names it, with debris that deflects flow and moves
the channel).

**Time.** Melville and Chiew (1999) define an equilibrium time t* that the same parameters govern as
the depth, take equilibrium as the hole deepening less than 5 % of the pier's width in 24 hours, and
find the hole at 50 to 80 % of its equilibrium depth after a tenth of t*, depending on the approach
speed (search summaries of the abstract and of a paper quoting it: **summary**). Their t* formula and
its hours or days were **not read** (the National Academies' report on wide piers, which carries both,
reached only its contents page). The shape of the published curve, steep and then slow, is what matters
first: a fresh obstacle's hole is mostly dug in the first part of its time.

## 6. The time scales, and the bed's quickened clock

**How fast the bed moves**, from §2 to §8 (*derived* unless marked):

| change | time | from |
|---|---|---|
| a footprint's cloud of fine sand settling out | seconds | §4 |
| the same cloud's silt clearing in a slow stream | minutes to an hour, carried hundreds of metres | §4 |
| a 5 cm dent refilled by 1 mm bedload at θ 0.1 (q 1.2 × 10⁻⁵ m²/s, over 0.3 m, p 0.4) | about 12 minutes | §2, Exner |
| sand laid against a log that stops that bedload, a metre of bed | about 7 cm an hour | the same |
| a scour hole at a fresh obstacle | most of it in the first tenth of t*; t* **not read** | §5 |
| a beach's storm cut | a storm: DUNERULE's reference is 5 hours | §8 |
| a bar's offshore move | about 90 m in one storm (Duck, 1994) | §8 |
| a dune foot's recovery | years: back to trend in about 15 at Walcheren after 1953 | §8 |
| a river bed's long change | the bed's celerity, under a metre an hour on a sand bed and nothing on a stone one; years for a reach | §3 |

**The morphological acceleration factor (MORFAC) is the published form of a calendar rate** — the
game's third clock (W187), set apart from the world clock that moves what is in front of the eye. The
coastal models multiply the bed's change in each hydrodynamic step by a factor f_mor and divide every
input time series by it, so a tide or a wave climate drives f_mor times its own bed change
(XBeach's manual, read: the time series are divided internally, and f_mor is described as O(1–10));
**only the sediment's equation is accelerated, not the water's** (Carraro et al. 2018, abstract read).
The values published: XBeach's manual describes its factor as O(1–10) (read; XBeach is a storm-impact
model); about 10 for long runs in another model, a
critical value of roughly 60, 90 and 130 for three tidal cases, O(100) "frequently used", and up to
1,000 in qualitative long-term studies (**summaries** of the TELEMAC paper, of a Coastal Engineering
study, of a river study and of a review: none read). **Where it stops being right**, as the authors
read here put it: no robust objective way to know the largest usable factor in advance (Ranasinghe et
al. 2011, abstract read, who propose a criterion from the bed-form migration CFL; Reyns et al. 2014,
abstract read, the same for tidal flow by geometry and time step); the scaling assumes the bed's change
is linear in the forcing over the accelerated span and that the water is much faster than the bed
(summaries of the review literature); results compare only at the ends of whole tidal cycles
(**summary**); and storms are run at low factors, a variable factor lower in strong episodic weather
(**summary** of a tidal-inlet study). This page's reading of what it distorts: anything whose flow
depends on the bed changing within the event (a scour hole that changes the flow that digs it, a bar
that changes where the waves break), and anything not linear in the forcing — a threshold law is not,
which is one reading of why storm runs keep their factors low. MASSPEED widens the linear range by accelerating the water's mass
equation too (Carraro et al., abstract).

**A calendar that jumps** ("it's winter now") needs a slow state at a date without the gap lived
through. What the literature offers, each read where marked:

- **Equilibrium forms by forcing**, the state the bed tends to under a held forcing: the shoreface's
  h = A x^(2/3) with A from the grain's fall speed ([coast](coast.md#the-equilibrium-profile), read
  there); the equilibrium ripple of §7 for a flow and a grain; the equilibrium scour depth of §5; a
  threshold channel's grain ([channels](channels.md#the-bed)).
- **A response that lags its forcing exponentially**: Kriebel and Dean (1993) write a profile's erosion
  as the convolution of a time-varying forcing with an exponential response, so the profile lags and
  reaches only part of its equilibrium in a short storm (**summary** of the abstract). A state at a date
  is then an integral over the weather's history, not a run.
- **An event's result as a formula of its strength**: van Rijn's DUNERULE gives a storm's eroded volume a
  metre of shore from its surge, wave height, period, duration, grain and slope (§8, read), and Larson et
  al. 2004's impact law gives the dune's retreat from the run-up over its toe (read): **a hurricane can
  carry a strength of its own**, within the ranges those fits were made on (the references of §8).
- **A family of baked sections chosen by the weather's history** is the cheapest form of the same: the
  section after a storm and after each span of calm, the history picking which, the equilibrium forms
  bounding what can be chosen.

## 7. Ripples and dunes in sand (W179)

**Under a current** (the Coastal Wiki's "Bedforms and roughness", read). Ripples form just above the
threshold, the first bedform to appear; mini ripples at speeds 10 to 20 % above the critical one on beds
finer than about 0.5 mm, asymmetric with a steep lee and a gentle stoss side, straight, lunate or
linguoid in plan. Fully developed current ripples (Soulsby et al. 2012):

    η_r = 202 D D*^(−0.554),   λ_r = D (500 + 1881 D*^(−1.5))

*Derived*: 0.2 mm sand, 1.6 cm high and 13 cm long. Flemming (1988)'s field fit η = 0.677 λ^0.809 (m);
Lapotre et al. (2017) put λ on the viscous scale, λ = 2504 D^(1/3) θ^(1/6) (ν/u*)^(2/3). On coarse sand
past about 0.7 mm current ripples give way to dunes (**memory**: not on the page read). **Dunes** scale
with the depth: λ = 3 to 15 h, 6 to 7 h usual; Venditti and Bradley (2022): log₁₀(Δ/h) = −0.397
[log₁₀(θ/θ_c) − 1.14]² − 0.503 and log₁₀(λ/h) = 0.098 [log₁₀(θ/θ_c) − 1.09]² + 0.791; high-angle lee faces
past 24° in shallow flow, low-angle in deep rivers; past them the upper flat bed and **antidunes**, under
ten depths long, near Fr 1 (same page). Ripples raise the bed's roughness: on a rippled bed
k_s ≈ 8 η²/λ + 170 D √(θ − θ_c), of the order of 25 η²/λ; a flat sand bed's is 2 to 3 D; a Mississippi
reach's falls from about 0.5 m among large dunes at 0.5 m/s to about 1 mm on a bed flattened at 2 m/s
(same page) — a moving bed's Manning's n is itself a state.

**Under waves** (same page), with a the orbital excursion at the bed and the mobility
Ψ = U_w² / (R g D):

- **Orbital** ripples, short periods: λ_r ≈ 1.3 to 1.4 a, steepness η/λ ≈ 0.1 (vortex ripples past
  about 0.1); **anorbital**, a > 1000 D: λ_r ≈ 400 to 600 D, η ≈ 0.1 to 0.12 λ, set by the grain
  instead; suborbital between.
- Predictors (each in the page's table): Nielsen (1981, with O'Donoghue et al. 2006),
  η = a (0.275 − 0.022 Ψ^0.5), λ = a (1.97 − 0.44 Ψ^0.21); Soulsby and Whitehouse (2005),
  η = 0.15 λ [1 − exp(−(5000 D/a)^3.5)], λ = a [1 + 0.00187 (a/D)(1 − exp(−(5000 D/a)^−1.5))]^−1;
  Goldstein et al. (2013); Ruessink et al. (2015) for D > 0.3 mm, η = 0.164 λ [1 − tanh(0.63 θ)],
  λ = 0.676 a θ^−0.163; Grasmeijer and Kleinhans (2004); van Rijn (2007), η = a 2.8 × 10⁻¹³ (250 − Ψ)^5,
  λ = 5 × 10⁶ η (250 − Ψ)^−2.5, which goes to **zero height at Ψ = 250** (the page does not call it the
  wash-out; the formula's form does); Chen et al. (2023).
- **Washed flat**: the page describes wash-out at high speeds through a longer, lower form without a
  number; sheet flow past θ ≈ 0.4 (§1) carries no ripples.

*Derived*, a 6 s wave over 3 m of water (kh 0.61) on 0.25 mm sand: at a height of 0.3 m, U_w = 0.24 m/s,
a = 0.23 m, Ψ = 14, and Nielsen gives ripples **28 cm long and 4.4 cm high** (λ ≈ 1.2 a, steepness 0.16);
at 1 m, U_w = 0.80 m/s, Ψ = 159, Nielsen's height is negative and van Rijn's 1.3 mm: **washed flat**. So
the same shore carries ripples in calm weather and none in a storm, and they re-form as it calms, which
is the owner's "different weather, tides, waves".

**Combined waves and current**: the page gives the stress, ⟨τ_cw⟩ = τ_c [1 + 1.2 (τ_w / (τ_c + τ_w))^3.2]
and the maximum with the angle between them (Soulsby et al. 1993), and no ripple predictor of its own.
**What W179 must still read**: ripple migration speed and the time a ripple field takes to re-form after
a change of forcing (neither on the page read), the current-ripple limit by grain, wave-current
ripples, and ripples on a beach the tide has left (the swash's own forms). A hard bed carries none: the
ground's type (W183) says where there is sand.

## 8. A shore's section under a storm (W180)

The section at rest, by grain, is [coast](coast.md)'s: Dean's h = A x^(2/3) with A = 0.5 w_s^0.44, the
closure depths of Hallermeier and Birkemeier, the face's slope by grain and exposure, the berm at high
water plus the fair-weather run-up, and the beach's state by Ω = H_b / (w_s T) (read there, with its
gaps). What the storm does to it, read tonight:

- **Cross-shore transport's two directions.** Undertow under breaking waves, plunging most, carries sand
  offshore; the nonlinear shoaling wave's skewness and acceleration asymmetry carry it onshore (the
  Coastal Wiki's "Nearshore sandbars", read; Hoefel and Elgar 2003 modelled onshore migration from the
  fluid's acceleration and both directions over 45 days at Duck: **summary** of the abstract). A storm
  moves sand off the beach and builds a bar; calm weather brings it back and the bar migrates onshore
  and welds to the beach.
- **When a bar moves which way**: offshore where Γ = H_s/h over its crest passes about 0.6, onshore under
  0.3 (Ruessink and Terwindt 2000, Dutch coast). Bars are rare on shorefaces steeper than about 1/30;
  dissipative coasts carry several, often three or four; bars lie 2 to 4° off the shoreline, the
  seaward end downdrift; Egmond's outer bar decays at the surf zone's seaward limit, 8 m deep; a bar can
  split weeks after high waves (all the same page, read). **How far**: a 1.8 m bar moved about 90 m
  seaward in one storm at Duck in 1994 (WHOI's article, read, all rights reserved; its time not
  stated); 280 m to 310–350 m offshore in about a day (a search summary of another Duck study:
  **summary**).
- **The storm's cut**: Larson et al. (2004)'s impact law, dV/dt = −4 C_s (R − z_D)² / T, with R the
  run-up level, z_D the dune's toe and T the period, C_s 10⁻³ to 2 × 10⁻³ fitted, and the toe's
  retreat with the time scale T / (4 C_s tan β); van Rijn's DUNERULE (2009): V(t) = 170 m³/m × (t / 5 h)^α_t
  (h_surge / 5 m)^α_h (H / 7.6 m)^0.5 (T / 12 s)^0.5 (D / 0.225 mm)^−1.3 (tan β / 0.022)^0.3 (1 + 0.01 θ)^0.5,
  α_t 0.5 before 5 hours and 0.2 after, α_h 1.3 under 5 m of surge and 0.5 above; the storm-profile
  method's post-storm section with a 1:1 scarp and a 1:12.5 slope at the deposit's toe (van Gent et al.
  2008); XBeach's avalanching, 1 dry and 0.3 wet (all the Coastal Wiki's "Dune erosion", read). Volumes:
  hundreds of cubic metres a metre in a severe surge, up to 800 m³/m and 80 to 100 m of dune foot in the
  Netherlands' 1-in-100,000-year storm; about 30 m at Walcheren in 1953, nearly 20 m at Penhale and
  Lacanau in the winter of 2014.
- **The calm's recovery**: the beach recovers much faster than the dune (no constant given); Walcheren's
  dune foot came back to its earlier trend about 15 years after 1953; Lacanau had not recovered after
  six; a dune's vegetation takes two to eight years (the same page, read).
- **Models of the whole section**: process-based (XBeach; Jacobsen, Fredsøe and Jensen 2014;
  Dubarbier et al. 2015, which needs tuning to local observations; Marchesiello et al. 2022 in 3D),
  semi-empirical (Splinter et al. 2018's multiple bars), equilibrium (Li et al. 2021's barred profile)
  — named on the sandbars page, none read. Kriebel and Dean's convolution (§6, **summary**).

**What W180 must still read**: Kriebel and Dean's time scale itself and Vellinga's erosion profile (both
named, not reached); a shoreline's equilibrium models against the wave climate (Miller and Dean 2004,
Yates et al. 2009, Splinter's ShoreFor: **memory**, not read); bar migration rates by forcing; the tide's
move of the surf zone up and down the section and what a range of tide does to the profile; and how a
gravel shore's storm ridge rolls over ([coast](coast.md) has the mechanism).

## 9. What interactive graphics has published on erosion over layered ground

| method | what it moves | stated cost | read |
|---|---|---|---|
| Musgrave, Kolb, Mace 1989 | hydraulic and thermal (talus) erosion on a height field | — | named only |
| Beneš, Forsbach 2001 | a layered representation of the ground for erosion | — | named by a summary |
| Neidhold et al. 2005 | fluid and erosion on a grid | 4 fps at 256² | as Mei et al.'s slides cite it |
| Beneš 2007 | the same | 5 fps at 300² | as Mei et al.'s slides cite it |
| Mei, Decaudin, Hu 2007 | the pipe model's water, a soil-science erosion model (Julien 1985), the load advected semi-Lagrangian, evaporation; GPU | 512² in demonstrations, interactive to 1,024²; times linear in cells; GeForce 8800 GTX | slides' text, read |
| Štava, Beneš, Brisbin, Křivánek 2008 | two erosions, dissolution where water is slow and force-based where fast, over four layers of material; tiled for large ground | at least 20 fps at 2048 × 1024 with four layers | summary of the Eurographics abstract |
| Krištof, Beneš, Křivánek, Štava 2009 | SPH water coupled to a grid's erosion model | interactive to 25,000 particles | summary of the abstract |
| Vanek et al. 2011; Skorkovska 2015 | adaptive GPU tiles; erosion on a triangle mesh | — | named only |
| Schott et al. 2023 | stream-power erosion in the uplift domain for authoring mountain ranges, a fast drainage-area approximation | interactive, no timing found | summary |

**The pattern.** Every real-time method read moves sediment by **relaxation towards a capacity**: the
water carries a load that rises towards what its speed and slope can carry by a dissolving rate and
falls towards it by a deposition rate (Mei et al.'s capacity is, from **memory**, C = K_c sin α |**v**|,
the paper not read), which is the form of XBeach's (C_eq − C)/T_s (§2) with a capacity chosen for looks
rather than from θ and the grain. Thermal erosion is the avalanching of §3. Layers enter as a stack of
materials of different erodibility at each cell (Beneš and Forsbach; Štava's four). The limits stated:
the water's time step, little erosion on flat ground (Mei et al., read), and the pipe model's missing
momentum ([water-state-grounding](water-state-grounding.md#height-field-solvers-in-graphics)). None
read gives a bed's cost separate from its water's, and none reaches the scale of a game's rivers at a
decimetre's cell. What the physical laws add is the capacity's dependence on the grain and the stress,
a threshold, and two modes; what graphics adds is the evidence that a layered stack and a capacity
update ride on a height-field step at its own rate.

## 10. A reading for the design, kept apart from the sources

Everything below is this page's reading of the above, for the design stage; it decides nothing.

### The smallest state a bed needs beside the water's

1. **A mobile layer's thickness δ over the built ground**, the bed the water sees z = z_built + δ, δ ≥ 0.
   Under δ is the ground's own stack (W183): where it is stone, nothing erodes below δ and transport is
   supply-limited, at most what arrives (Exner, §3, with a floor). The baked bed is δ₀; a disturbed bed
   is δ₀ plus a change.
2. **Grain classes: one for the bed, one or two for the cloud.** The bed's moving class from the ground's
   type (a gravel channel's D50 is already derived, [channels](channels.md#the-bed)); a fine class (silt,
   clay) is what clouds water and stays up (§1: fine sand and finer go straight to suspension; §4: silt
   and clay are what hide the bed). **Two classes is the smallest that gives both "the bed moves" and
   "the water clouds and clears"**; a third (a sand that suspends in floods) is the design's question.
3. **A depth-averaged concentration C per suspended class per cell**, carried by the flow as the foam's
   φ is ([water-state-grounding §5](water-state-grounding.md#5-foam-as-a-consequence)), with a source
   at the bed and a settling sink: either E − w_s c_b (§2's first form) or the relaxation
   (h C_eq − h C)/T_s (§2's second, the cheaper and the one graphics uses).
4. **Bedload as a flux, not a store**: q_b from θ − θ_c per §2, along the bed stress with a downslope
   term, and avalanching past a slope (§3).
5. **Ripples as a predictor, not a state, at first**: the equilibrium ripple for the cell's flow, waves
   and grain (§7) is a function of what the state already gives out; only a field that lags its forcing
   (re-forming after a storm) needs a stored height and phase, and its lag time is not yet read.

So, beside the water's 16 to 24 bytes a cell: **δ and one or two concentrations, 4 to 6 bytes at
16 bits each** (*derived*), with the ground's static data (its stack, its D50 by layer) W183's. A scene
with no mobile bed pays none of it.

### What the water's state must give out and take in

- **Out**: h, **u** with its direction, the stress at the bed τ_b (the state's own friction term already
  computes it), and under waves the orbital velocity and excursion at the bed — from the wave field's
  closed forms where no state runs ([shore-mechanics](shore-mechanics.md)), since ripples, wave-driven
  transport and the shore's section all read them.
- **In**: a bed height that changes, with the water step well-balanced over it (§3); **N carried
  scalars**, the foam one of them, each with a source, a decay or a settling sink — one generic carried
  tracer, not a foam special case; a roughness that changes with the bed's forms (§7: ripples raise Manning's
  n, a flattened bed lowers it); and the exchange's mass where it is not negligible.
- **The coupling**: splitting (water, then bed) is the common way, safe while the bed's wave is
  four orders slower than the water's (§3) and weakest near Fr 1 (Cordier et al., summary only). The
  slowness also means **the bed can step far less often than its water**: at the 1 mm example's
  celerity the bed's own CFL allows minutes on a 0.25 m cell (*derived*, 0.5 × 0.25 m / 2.4 × 10⁻⁴ m/s ≈
  9 min), though a fresh obstacle raises the local transport by the stress it concentrates.

### Which clock each part reads (W187)

| part | clock | why |
|---|---|---|
| a cloud stirred by a foot or a fall | world | sand settles in seconds, silt in minutes (§4) |
| a footprint's dent refilling | world | minutes under a moving bed (§6) |
| scour and the deposit round a fresh obstacle | world, while it is in front of the eye; the calendar takes over its slow tail | most of a hole is dug early (§5) |
| ripples re-forming as a flow changes | world near the eye; a predictor elsewhere | their size is an equilibrium function of the flow (§7); their lag not read |
| a bar's migration, a storm's cut, the calm's recovery | calendar | storms of hours, recoveries of years (§8) |
| a river bed's long change, a channel's slow migration | calendar | under a metre an hour on sand, years for a reach (§6) |

**The calendar's rate is the published MORFAC** (§6): the bed's change a hydrodynamic step multiplied,
the water's not, linear over the span, low in storms. **A jump of the calendar needs the slow state at a
date without the gap**: the equilibrium forms by forcing (the shore's profile, the ripple, the scour
depth, the threshold channel), the exponential lag (Kriebel and Dean, summary), an event's result as a
formula of its strength (DUNERULE, Larson et al.) so a hurricane carries its own within the fits'
ranges, and a family of baked sections the weather's history chooses among. The world-clock parts are
then a disturbance on top of whatever the calendar's state is at that date.

### What "rivers stay mostly baked, the bed moves where something disturbs it" could mean

A baked river bed is **in transport equilibrium, not at rest**: a threshold gravel channel moves its
median at bankfull and a sand bed moves at most flows, so sediment passes through a reach whose bed
does not change, ∇·**q** = 0. "Baked" is then that assumption, and a **live region** is one where the
state's τ_b departs from the baked one: an obstacle, a footprint, a changed discharge, a dam. Its
upstream edge must be fed the baked equilibrium transport or it scours from its inlet, and its
downstream edge hands back whatever it does not keep: **the bed's analogue of the water's LOD seam**,
where sediment as well as water must be counted. A region goes back to baked when its δ has settled
to a new equilibrium the calendar can carry.

### Questions the design must answer, with the evidence each way

- **One fine class or two**: clay and silt cloud water differently by a factor of four in b (§4) and
  settle at speeds a decade apart; one class can carry a ground type's mean.
- **The bed on the world clock or its own**: the bed's slowness (§3) argues a coarser step; a fresh
  obstacle's concentrated stress and the near-critical reaches argue that the step adapt.
- **Splitting or a coupled solve**: splitting is everyone's practice and cheap; its failure is near
  Fr 1 and was read only as a summary — W184's CPU reference is where to test it.
- **The exchange's form**: E − D is closer to the physics and needs the near-bed profile; relaxation to
  C_eq is cheaper, has a single time constant, and is what graphics ships.
- **What the renderer reads**: C per class times b from the class's diameter (or a measured value in
  0.05–1.5 m²/g) gives visibility; colour needs an absorption per ground type that was not read.
- **Where a lying log differs from a pier**: the scour under a horizontal cylinder, a jam's backwater
  and the bank's attack were not read; the pier law is the only depth law on this page.
- **How far the calendar may jump a live region**: equilibrium forms exist for the shore, ripples,
  scour and channels; a river's bed between equilibria (a bar half built) has no closed form read here.

### Three things the water's design most needs to hear

1. **The bed's height is state the step reads every step**, z = z_built + δ, so the scheme is
   well-balanced over a bed that changes, and a body, a foot or an author edits it (or a blocked share)
   as an edit on the world clock.
2. **The state gives out τ_b, h and u, and under waves the orbital velocity and excursion at the bed,
   and it carries generic scalars**, each with a source, a decay or a settling sink: the foam is one
   and the suspended classes are the next.
3. **The bed has its own clocks**: it may step far less often than its water; its slow forms run on the
   calendar at a published rate (MORFAC) and are obtainable at a date from equilibrium forms; and a
   baked river is an equilibrium of transport, so a live region's inlet must be fed the baked load.

## The sources, how far each was read, and under what licence

| source | what is taken | read | licence of the text read |
|---|---|---|---|
| The encyclopaedia, "Sediment transport" (to character 100,000 of 121,236) | Shields' parameter and its classical values; Ferguson and Church's settling law and constants; the Rouse number's ranges and profile; Meyer-Peter and Müller with its variants; Wiberg and Smith; Wilcock and Crowe; Wilcock and Kenworthy; Engelund and Hansen | read | CC BY-SA 4.0 (the site's footer, read on the Exner article) |
| The encyclopaedia, "Exner equation" | Exner's form; packing 0.45–0.75, about 0.64 | read | CC BY-SA 4.0 (footer read) |
| The encyclopaedia, "Shields parameter" | the definition | read | CC BY-SA 4.0 |
| The encyclopaedia, "Bridge scour" | clear-water and live-bed scour, 10 %, the downflow, contraction scour and debris, HEC-18's over-prediction | read | CC BY-SA 4.0 (footer read) |
| van Rijn, "Sand transport", Coastal Wiki (2026 revision) | Soulsby's thresholds (eqs. 2, 3), critical speeds (eqs. 4–7), MPM's spread, Engelund–Hansen, van Rijn's bed and suspended loads (eqs. 10, 11), the Rouse profile, sheet flow, mud's effect, the load's lag | read | CC BY-NC-SA 4.0 by a search summary of the site's footer (the page prints none); equations only, no text |
| van Rijn and Dronkers, "Bedforms and roughness", Coastal Wiki | current ripples (Soulsby et al. 2012, Flemming, Lapotre), dunes (Venditti and Bradley 2022), wave ripples (the predictors' table), roughness, combined stress | read | as above |
| "Dune erosion", Coastal Wiki | Larson et al. 2004; DUNERULE; the storm-profile method; XBeach's avalanching; volumes, retreats, recoveries | read | as above |
| "Nearshore sandbars", Coastal Wiki | the two transport directions; Ruessink and Terwindt's 0.6 and 0.3; bars' slope limit, number, angle, decay depth, splitting | read | as above |
| Lamb, Dietrich, Venditti (2008), *JGR* 113 F02008 | τ*_c = 0.15 S^0.25 | read through [channels](channels.md) (full text there) | not read tonight |
| "A moment-based shallow water model for coupled bedload–suspended load transport with variable density", arXiv 2505.22278 (2025) | the coupled shallow-water, bedload, suspended and Exner system; E and D; the settling and Bradford forms | read (the first 100,000 characters) | CC BY 4.0 (stated) |
| XBeach manual, "Processes and model formulation" (characters 100,000–200,000 for the sediment part) | the advection–diffusion with relaxation (eqs. 87, 88); Soulsby–van Rijn C_eq (eq. 100); bed update with f_mor (eq. 130), O(1–10); slope terms (eqs. 123, 124, 129) | read | documentation: no licence stated for its text; no code read |
| Carraro, Vanzo, Caleffi, Valiani, Siviglia (2018), *Adv. Water Res.* 117, arXiv 1912.05245 | MORFAC accelerates only the sediment equation; MASSPEED; a CFL-like criterion | abstract | arXiv non-exclusive licence |
| Ranasinghe et al. (2011), ICCE 32, "Morphodynamic upscaling with the MORFAC approach" | no robust a-priori limit; a bed-form CFL criterion proposed | abstract | CC BY (the proceedings' page) |
| Reyns et al. (2014), ICCE 34, the critical MORFAC in tidal conditions | the limit by geometry and time step; a morphological CFL | abstract (UNESCO's record) | none stated on the record |
| Cordier, Le, Morales de Luna (2011), *Adv. Water Res.* | splitting's oscillations; MPM's coupled hyperbolicity under a velocity constraint | summary only (HAL refused) | — |
| arXiv 2611.12895 (2026), silt release under cave ceilings | c = c_w + Σ b_k C_k, b_k = 3/(ρ_s d_k), y = 4.8/c, c_w 0.16 m⁻¹ | read (the optical part) | arXiv non-exclusive licence |
| Mohammadpour et al. (2017), *Biogeosciences* 14, 5297 | that mineral-rich small particles absorb more than organic ones (no number taken) | abstract | not read on the page |
| NALMS, "The black disk" | the three measures only broadly correlated; method precisions | read | © NALMS, all rights reserved; facts only |
| Ponce, HEC-18 pier-scour calculator page | the CSU equation's form and factors' meanings (HEC-18 5th ed., 2012) | read | no statement on the page; the equation is FHWA's |
| WHOI, "Where the surf meets the turf" | a 1.8 m bar about 90 m seaward in a storm, Duck 1994 | read | © WHOI, all rights reserved; facts only |
| Mei, Decaudin, Hu (2007), slides' text | the algorithm's steps, sizes, hardware, limits; Neidhold's and Beneš's rates | read (secondary to the paper) | no statement read |
| Melville, Chiew (1999); Wong, Parker (2006); Kriebel, Dean (1993); Hoefel, Elgar (2003); Davies-Colley, Smith (2001); Boss, Slade, Hill (2009); Neukermans et al. (2012); Štava et al. (2008); Krištof et al. (2009); Schott et al. (2023); the MORFAC values 60, 90, 130, O(100), 1,000 | as marked where each appears | summaries only | — |
| Not read at all | HEC-18 itself and its caps; the National Academies' wide-pier report (contents page only, all rights reserved); Melville and Chiew's t*; Wong and Parker's coefficients; Vellinga; Kriebel and Dean's T_s; Garcia and Parker 1991; Sumer and Fredsøe's pipeline scour; ripple migration and lag; mineral absorption spectra; Musgrave et al.; Beneš and Forsbach; Vanek et al.; Skorkovska; Miller and Dean; Yates et al.; ShoreFor | — | — |
