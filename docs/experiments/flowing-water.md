# Flowing water: the grounding for channels and falls (S96)

**What this page is.** The sources and the numbers behind the water capability's channels ([water](../subsystems/water.md#channels), roadmap S96; the wider row is S95): how fast the water in a channel moves, where it turns white, where a channel stops flowing over its bed and falls, and what of that is a stated choice rather than a measured law. The owner's ask (2026-11-06): "running water — rivers, downhill rivers, waterfalls".

## The speed: Manning's equation

**The model.** For steady, uniform open-channel flow the mean velocity is Manning's (Chow 1959, *Open-Channel Hydraulics*, McGraw-Hill, ch. 5):

v = (1/n) · R^(2/3) · S^(1/2)  (SI units, m/s)

with R the hydraulic radius (flow area over wetted perimeter, m), S the slope of the energy line (taken as the surface's fall per metre, m/m) and n Manning's roughness. **For a wide channel R is about the depth**: a rectangle of width w and depth d has R = w d / (w + 2 d), within 10% of d once w ≥ 20 d, and for the parabolic bed the channel draws (area 2/3 w d) R tends to 2/3 d. The channel takes R = d at each point, the description's depth on the centreline: that over-states R by up to a half on a narrow, deep reach, which is v too fast by (3/2)^(2/3) = 1.31 at most — inside the spread of n itself, and a speed the eye reads only through the ripples' drift.

**n, the bed's roughness.** Chow's table (reproduced in the SDSU open-channel hydraulics notes, [ponce.sdsu.edu](https://ponce.sdsu.edu/openchannel/open_channel_hydraulics_05_manningsntable.html), and the UN-SPIDER copy, [un-spider.org](https://www.un-spider.org/sites/default/files/Table_Surface_roughness_Manning_n_Chow_1959.pdf)): natural minor streams, **mountain streams with no vegetation in the channel, steep banks, a bottom of gravels, cobbles and a few boulders: 0.030 / 0.040 / 0.050** (minimum / normal / maximum); cobbles with large boulders 0.040 / 0.050 / 0.070. The channel's `manning_n` defaults to **0.04**, the gravel bed's normal; TxDOT's design manual gives the same 0.030–0.050 range for that bed ([txdot.gov](https://www.txdot.gov/manuals/des/hyd/chapter-4--hydrology/section-11--time-of-concentration/manning-s-roughness-coefficient-values.html)).

**What it gives** (n 0.04): a lowland reach 0.6 m deep falling 1 in 100, **1.78 m/s**; a steep reach 0.4 m deep at 1 in 25, **2.71 m/s**; a run-out 0.8 m deep at 1 in 125, **1.93 m/s**. A slope of 0 would be still water and a phase that never advances, so the speed is floored at **0.05 m/s** (`k_channel_min_speed`), a pool's drift.

**Determinism.** d^(2/3) is the cube root of d², taken by Newton's iteration from above in + − × ÷ (64 steps, `water::cube_root`), not the C library's `cbrt`, which is not correctly rounded and differs between toolchains; the mesh's phases come from these speeds, so the GLB is the same bytes from every compiler (the geometry page's rule, [geometry](../subsystems/geometry.md#the-same-bytes-from-every-toolchain)).

## Where it turns white: the Froude number

**The model.** Fr = v / √(g d), the ratio of the flow's speed to a shallow-water wave's (Chow 1959, ch. 1 and 3). Below 1 the flow is **subcritical** (tranquil: a disturbance travels upstream); at 1 critical; above 1 **supercritical** (rapid: standing waves, hydraulic jumps where it slows again, the white water of rapids). The examples above: the lowland reach **0.73**, the run-out **0.69**, the steep reach **1.37**.

**What the shader does with it** (choices, stated): the ripples' slope scales by 0.4 + 0.6 · min(Fr, 2) — calm water is not glass, rapids are rough; the GGX roughness rises by 0.25 · min(Fr / 2, 1); and **foam starts at the critical 1 and covers most of the surface at 2** (`k_channel_foam_froude`, `k_channel_foam_full`): the white share of the ripples' crests grows from none to about four fifths across that range. The Froude number is the section's speed over the depth under the pixel, floored at 0.1 m, and the foam fades over the last 25 cm of depth (the waves' calm) — which leaves faint foam lines along a fast reach's banks, where the shallow margin is near critical; real streams collect foam along their banks too, from eddies rather than criticality, and this page says the picture does it for the wrong reason.

**The foam's colour**: white water as a diffuse surface of albedo **0.8**, a choice (whitecaps and fresh foam reflect most of the visible light; the value is in the range the ocean-colour literature uses for fresh foam, but this page did not pin a source and a later row may).

## Where it falls: cascade and waterfall

**What divides them.** There is no single classification of waterfalls (the literature says so: [Wikipedia, "Waterfall"](https://en.wikipedia.org/wiki/Waterfall)); the shape vocabulary in common use ([World of Waterfalls](https://www.world-of-waterfalls.com/what-types-of-waterfalls-are-there/), [Washington Trails Association](https://www.wta.org/go-outside/seasonal-hikes/spring-destinations/waterfall-field-guide-hike-4-types-of-falls)) is: a **plunge**, where the water leaves the rock and falls free; a **horsetail**, a near-vertical face the water keeps touching; a **cascade**, water stepping down a gently pitched series of ledges. **The channel draws a fall where the line between two points drops at least as far as it runs — 45 degrees and steeper** (`k_channel_fall_gradient` 1): a choice at the boundary between a steep cascade and a horsetail, since at 45° the water's own speed carries it off any ledge of a step's height, and below it the water stays on the bed and Manning's equation still describes a reach (a 1-in-1 cascade's Manning speed would be absurd — √S = 1 — which is the second reason to stop using it there).

**The fall's speed.** Free fall from the lip: v = √(v_lip² + 2 g h) at h below the lip (g 9.81 m/s²), the lip's speed Manning's. A 10 m fall from a 2.71 m/s lip lands at **14.3 m/s**; air drag on a coherent jet is negligible over tens of metres and the fall is not modelled breaking up.

**The plunge.** Below the fall the jet's speed decays back to the bed's over a length of **0.4 h, within 1 to 15 m** (`k_channel_plunge_share`, a choice): quadratically in the remaining share, so the Froude number — and the foam — is whole at the foot and gone at the end. Plunge-pool scour studies give the pool's size as growing with the drop and the discharge (the jet's diffusion through the pool; e.g. the scour-depth formulas of Veronese 1937 and their successors, depth ∝ q^0.54 H^0.225); the channel's length is not fitted to any of them and says so, and a 10 m fall's 4 m of white water is what the owner's picture is judged on.

## What a channel reads instead of a wave spectrum

The open sea's JONSWAP (the [ocean-surface page](ocean-surface.md)) is a wind sea grown over a fetch; a stream's surface is not wind waves but the flow's own turbulence carried along it — boils, ripples and the standing waves of supercritical reaches.

**S96 drew six sines carried by the flow phase** (the travel time from the head, the mesh's UV.x). Every pattern a function of τ − t is periodic in τ with the phase's 8 s wrap, so the whole surface repeated **exactly every 8 s** in time and every 8 s of travel in space (16 m of a 2 m/s reach) — the "tiled pattern" and the "regular ladder of crests" the owner and the forest POC's v35 review saw ("the water carries a lake's wind ripples, not flow; rapids are regular stripes; falls a flat strip"). **S98 replaces them** with what follows; the phase stays, for the speed and the direction (its gradient) and for the patterns that must be stationary or exactly carried (the standing waves, two weak long trains, the fall's streaks).

## What the surface does (S98)

### The flow map: detail carried downstream that never resets

**The technique** (Vlachos 2010, "Water flow in Portal 2", SIGGRAPH Advances in Real-Time Rendering): sample a world-anchored pattern at p − v·t_k, where t_k runs over a cycle and restarts; two copies half a cycle apart, each faded out by a triangle wave as it restarts, so the restart is never seen and the pattern never stretches. The channel's pattern is **gradient noise** (quintic, eight gradients, the shore's integer hash: no texture) in three octaves over the body-local point, the cycle **1 s** (`k_channel_flow_cycle_s`, a divisor of the clock's 1,024 s so the clock's wrap falls where both copies restart), and each copy **jumps to a new place every cycle** (an offset of up to 37 m drawn from the cycle's number), so the pattern a place shows does not come back until the clock wraps — the 1 s cycle's own repeat that a plain two-phase flow map has is gone too. The blend divides by √(w₀² + w₁²) so the pattern keeps its contrast through the fade. The velocity is the triangle's (the phase's gradient), so where it changes from one triangle to the next the copies' places differ by the cycle times the change: millimetres between two sections of a reach, a metre or more in a plunge's decay, where the water is white anyway.

**Its scale** — the regimes of Brocchini and Peregrine (2001, *J. Fluid Mech.* 449, 225–254, "The dynamics of strong turbulence at free surfaces. Part 1. Description"): the surface's state is set by turbulence of velocity scale q and length scale L against gravity (disrupted where q² > 2 g L) and surface tension (where q² > 2σ/(ρL)); the two lines cross at the capillary length √(σ/ρg) = **2.7 mm** and q = **0.23 m/s** (the test holds both; the paper's constants are of order one and, as its abstract says, heuristic). Below both lines the surface is smooth with deformations of height about **q²/2g**. In a stream the turbulence reaching the surface is the bed's: q of the order of the shear velocity u* = √(g d S) (Nezu and Nakagawa 1993, *Turbulence in Open-Channel Flows*: the surface's rms velocity is near u*), taken from Manning's as u* = v n √g / d^(1/6), and L of the order of the depth (the macro-turbulence's eddies span it). So:

- **Never broken by the bed's turbulence alone**: q² > 2 g L with L = d needs g d S > 2 g d, a slope of 2. What whitens a stream is the Froude number's (below), and the page says so rather than drawing breaking where the regime diagram puts none.
- **Glassy to knobbly**: the deformation q²/2g is 0.08 mm at 0.3 m/s (glassy: the eye sees a mirror), **3 mm** on the 0.6 m reach at 1.78 m/s and **8 mm** on the 0.4 m reach at 2.71 m/s (knobbly: the sky broken into boils). The shader draws exactly this: the largest octave **1.5 depths** across (within 0.2–4 m), the rms height q²/2g with q the pixel's u*, each smaller octave 0.47 of the one above with its height falling as l^(2/3) (Kolmogorov's: the velocity of an eddy of size l goes as l^(1/3), its head as the square). The surface turns from glassy to knobbly continuously: the slope a mirror loses its sharpness at, about 0.01, is reached near **0.9 m/s on half a metre of depth** (n 0.04), within 0.6–1.3 m/s over depths of 0.1–2 m.
- **Faded with the footprint**: an octave finer than twice the pixel's footprint is faded out and its slope's rms is added to the GGX roughness, so far water keeps its sheen and has no ladder.

Manning's n does not reach the shader (the material has no slot left that every other reader ignores); u* is taken with **n = 0.04**, the gravel bed's normal. A channel of n 0.07 (the S96 synthetic brook) draws its surface 1.75 times calmer than its own n says; a later row may carry n in the volume entry.

### Standing waves over riffles

Where the flow is near critical the surface carries waves that **stand still in the channel's frame**: the wave whose phase speed against the flow is the flow's own. Kennedy (1963, *J. Fluid Mech.* 16, 521–544, "The mechanics of dunes and antidunes in erodible-bed channels") gives their wavelength over antidunes as **λ = 2π v²/g** (2.56 m at 2 m/s, 4.7 m at 2.71 m/s; the test checks that √(g/k) is v) and finds them in flows of Froude number about **0.8 to 1.8**. The shader draws them in the Froude band **[0.8, 1.6]** (a smoothstep in from 0.8 to 1, out from 1.3 to 1.6), at a steepness ka of **0.12** at the band's peak (a choice: a quarter of the Stokes limit π/7, since a standing wave at that steepness breaks), never above a quarter of the depth, strongest in the thalweg (times the lateral share r below). They are a function of the phase alone: λ = 2πv²/g is g/(2πv) cycles a second of travel, and the two whole numbers of cycles per 8 s either side of it are cross-faded by where it lies between them, so the wrap stays seamless and nothing advects them. **In the normal only**: the ribbon stays flat. A displaced ribbon would need the mesh's rows a quarter of a wavelength apart where the waves stand (0.6–1.2 m — the spacing is already 0.25–1 m) and the heights written into the vertices, which the mesh's phase and depth UVs leave no room to say where: a third attribute, or a vertex shader the cluster path does not have.

### Aeration: white water from the critical flow

Self-aeration — air drawn through the surface into the flow — starts where the turbulence's normal stress beats surface tension and gravity (Chanson 1996, *Air Bubble Entrainment in Free-Surface Turbulent Shear Flows*, Academic Press; the crude balance ½ρv'² > 2σ/r of Ervine and Falvey, USBR [PAP-0527](https://usbr.gov/tsc/techreferences/hydraulics_lab/pubs/PAP/PAP-0527.pdf)). In practice it needs a **speed of about a metre a second**: Cummings and Chanson's plunging-jet experiments measured inception at **0.74 to 1.58 m/s** ([core.ac.uk](https://core.ac.uk/download/14981111.pdf)), consistent with McKeogh (1978) and Ervine et al. (1980). In a stream it comes with the **critical Froude number**: standing waves and hydraulic jumps break the surface at Fr ≥ 1. The shader's foam share is the S96 Froude rule — none at 1, most of the surface at 2 — times a fade in over **0.8 to 1.2 m/s** (`k_channel_aeration_speed` 1), with the Froude number now the pixel's speed (the lateral profile's) over the **centreline's** depth, so the shallow banks no longer whiten for the wrong reason (S96's foam lines). Its pattern is the same noise, advected by the same two copies, **blurred along the flow over three taps** into streaks: cells of half a depth (0.1–1.5 m), stretched by the turbulence's turnover — a streak lasts about d/u* and travels v in that time, v/u* ≈ 7 depths for the test reaches, capped at three cells — and densest in the thalweg, where the speed is.

### The lateral profile

The speed across a section is not its mean: the bed holds the banks still and the thalweg runs fastest. The shader takes the **1/6 power law** — Manning's equation is the 1/6 power law's resistance law (Chen 1991, "Unified theory on power laws for flow resistance", *J. Hydraulic Eng.* 117(3)) — across the parabolic bed: **v(a) = C v̄ (1 − (2a/w)²)^(1/6)**, C = (4/3) / ∫₋₁¹ (1 − x²)^(7/6) dx = **1.045718** making the section's discharge v̄ times its area (2/3 w d; the test integrates it to 1e-5). The share r = 1 − (2a/w)² is the depth under the pixel over the centreline's, and the centreline's depth is recovered from the **depth map** (the bed is the parabola the description says): two more samples a probe of 0.4 m either side across the flow give the curvature K by the second difference and the centre by the first, d_c = d_mid + K (a_mid − a_c)². **At the banks** a probe falls dry and the three samples are taken inwards (one more fetch, three in all); where the bed is not a parabola (a plunge pool's bowl, where two stretches' beds overlap at a bend, the low tier with no map) the pixel takes the section's mean speed over its own depth, as S96 drew it. A vertex attribute is not needed, at the price of two or three bilinear fetches a channel pixel.

### The lip: Rouse's brink

A subcritical reach that ends at a free overfall accelerates into it: the depth falls to the critical depth d_c = (q²/g)^(1/3) some **three to four d_c** upstream and to **0.715 d_c** at the brink (Rouse 1936, "Discharge characteristics of the free overfall", *Civil Engineering* 6(4), 257–260; the end-depth ratio 0.715 is the classic one). The ribbon now speeds up over the last **4 d_c** of such a reach (`k_channel_brink_reach`), quadratically, to q / (0.715 d_c) at the lip (3.06 m/s from a 1.78 m/s reach 0.6 m deep, Froude 0.73), and the fall leaves the lip at that speed. A supercritical reach goes over as it is. The fall's sheet itself is **glassy at the lip** — a coherent tongue, the sky's bright roll on its curved face — and **white once the jet has fallen fast enough** to be aerated, a smoothstep over **4 to 6.5 m/s** of the fall's speed (a choice: a free jet's surface roughens and entrains air within its first metre or two of fall; the breakup of a waterfall's nappe is Ervine and Falvey's subject and this page does not pin a length from it).

### The plunge

**A fall that ends a channel** gains an implicit plunge stretch: one more point, the plunge's 0.4 h on along its heading at the foot's height, the last width and depth, slope 0 — so the jet's speed decays over it as below a fall that continues (the Froude rule whitens it), and slows to the still pool's drift at its end (`with_plunge_stretch`).

**A fall that lands in a body** — its foot inside the body's depth map, within a metre of its level — is drawn by **the body**: the renderer's reader finds the foot, the body's GPU material carries it as a 16-byte record (body-local x and z, a radius of **the fall's height over four**, within 1–8 m, a choice: the white boil at a waterfall's foot is of the order of a fraction of its height; and the free-fall speed from rest, √(2 g h)), up to **seven** a body in one table entry (the highest falls kept, the rest dropped with a warning), and the body's branch draws a disc of white water there: whole to a quarter of the radius, fading to nothing at it, broken by the same noise carried **outwards** from the foot by the two-copy flow map at a tenth of the impact speed (at most 1.5 m/s). Such a channel is "pooled" and builds no plunge stretch over the pool, which would lie on the pool's own surface.

## Mist (a request to R47)

Not built. A fall's spray is a participating medium, which is the volumetrics capability's (R47). **The request** (S96's, restated by S98 with what the plunge now knows): a froxel density source at each plunge — the same records the body carries (the foot, body-local; the radius, h/4; the impact speed, √(2gh)) or the foot of a channel's own plunge stretch — a column of droplets rising from the foot to about the fall's height, densest within the plunge disc and drifting downwind, its density scaling with the impact speed's square (the spray's energy); one more source of that capability's, which this row does not edit.

## The numbers pinned

| Constant | Value | Source or choice |
|---|---|---|
| `manning_n` default | 0.04 | Chow 1959, mountain stream, gravel and cobbles, normal |
| hydraulic radius | the depth on the centreline | the wide-channel approximation (Chow 1959) |
| `k_channel_min_speed` | 0.05 m/s | choice: still water's drift |
| critical Froude | 1 | Chow 1959 |
| foam full | Fr 2 | choice |
| foam albedo | 0.8 | choice |
| `k_channel_fall_gradient` | 1 (45°) | choice, between cascade and horsetail |
| fall speed | √(v_lip² + 2 g h) | free fall |
| plunge length | 0.4 h, 1–15 m | choice |
| `k_channel_phase_period_s` | 8 s | the half-float UV's precision: 4 ms at the top of [4, 8) s |
| `k_channel_flow_cycle_s` (S98) | 1 s, a divisor of 1,024 s | choice: the flow map's cycle (Vlachos 2010) |
| capillary length, crossing q | 2.7 mm, 0.23 m/s | Brocchini and Peregrine 2001 (σ 0.0728 N/m, ρ 998.2 kg/m³) |
| surface height | q²/2g, q = u* = v n √g / d^(1/6) | Brocchini and Peregrine 2001; Nezu and Nakagawa 1993 for q ≈ u* |
| largest octave | 1.5 d, within 0.2–4 m | of the order of the depth (Nezu and Nakagawa 1993); 1.5 a choice |
| octave ratio, height law | 0.47, l^(2/3) | choice; Kolmogorov's l^(1/3) velocity squared |
| n for u* in the shader | 0.04 | Chow 1959; the description's own does not reach the shader |
| glassy to knobbly | about 0.9 m/s at 0.5 m (0.6–1.3 m/s over 0.1–2 m) | where q²/2g's slope reaches 0.01, a choice of what reads as a mirror |
| `k_channel_aeration_speed` | 1 m/s, faded over 0.8–1.2 | Cummings and Chanson: inception 0.74–1.58 m/s |
| standing wavelength | 2π v²/g | Kennedy 1963 |
| standing-wave Froude band | 0.8–1.6 | Kennedy 1963 (about 0.8–1.8) |
| standing-wave steepness | ka 0.12, ≤ d/4 | choice: a quarter of the Stokes limit |
| lateral power, factor C | 1/6, 1.045718 | Chen 1991 (Manning's exponent); C from the discharge |
| lateral probe | 0.4 m | choice |
| brink depth, reach | 0.715 d_c, 4 d_c | Rouse 1936 (critical depth 3–4 d_c upstream) |
| fall glassy to white | 4–6.5 m/s | choice |
| plunge disc radius | h/4, within 1–8 m | choice |
| plunge level tolerance | 1 m | the brief's: the foot within a metre of the body's level |
| `gfx::k_water_plunges` | 7 a body | one material entry of 16-byte records |
