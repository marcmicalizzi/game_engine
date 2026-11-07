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

## Mist

Not built. A fall's spray is a participating medium, which is the volumetrics capability's (R47): a froxel density source at the foot, sized by the fall's height and the impact speed, would be one more source of that capability's; the report of S96 asks for it as a request rather than editing another capability.

## What a channel reads instead of a wave spectrum

The open sea's JONSWAP (the [ocean-surface page](ocean-surface.md)) is a wind sea grown over a fetch; a stream's surface is not wind waves but the flow's own turbulence carried along it — boils, ripples and the standing waves of supercritical reaches. The channel draws six sines whose phase is **carried by the flow phase** (the travel time from the head, the mesh's UV.x) and bent by a static phase over the surface, so every crest moves downstream at the local speed exactly (a pattern f(τ − t, p) with p fixed in the world is carried at v wherever v is), steeper with the Froude number; on a fall, streaks across the face carried down at the fall's speed. A flow map advected in two phases (Vlachos 2010, S95's proposal) is the alternative; it needs the velocity as a field over the surface and blends two copies of the pattern, where the phase needs nothing but the UV and never stretches, since the phase is a function of place along the channel and not an accumulated displacement.

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
