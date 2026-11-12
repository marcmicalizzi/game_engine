# Shore mechanics: a wave from deep water to the top of the beach (S102)

Roadmap S102 (the owner, 2026-11-12): "waves, streams, water in general needs to conform to the terrain it's on. the waves on the seashore rather than being an animation at the shore ... becomes something more representative of the real mechanics behind it: a wave moves over the water, the wave has energy, as it moves over the shore, it loses energy and height, until it's dissipated, and then because it's above the waterline, it recedes, and may be overtaken by another wave, depending on how periodic the waves are. whitecaps are a consequence of the wave breaking ... the ground wetness recession is a result of the water filtering into the ground above the waterline."

This page is the bodies' half of that (a sea, a lake, their shores): what main drew and what it read from the ground (the audit), the published models with the numbers the engine takes, one design in which every quantity at a place follows from the ground there, and what the first pass built. Running water at its obstacles is S103's page; the two name the same quantities the same way (below). The code is [water](../subsystems/water.md) ("The shore", "The swash", "The swell"); the page before this one, with S94's, S97's and S100's models, is [the shore](shore.md).

## The quantities, named once

| Symbol | What | Unit |
|---|---|---|
| d | the still depth at a place: the body's level less the ground | m |
| H | a wave's height, crest to trough; H₀ in deep water, H_b where it breaks | m |
| T, ω | its period and angular frequency 2π/T | s, rad/s |
| k, L | its wavenumber and wavelength 2π/k at the depth it is over; k₀, L₀ = gT²/2π in deep water | 1/m, m |
| c, c_g | its phase speed ω/k (the celerity) and its group speed, the speed its energy travels at | m/s |
| E | its energy per square metre of surface, ρ g H²/8 | J/m² |
| F | its **energy flux** per metre of crest, E c_g: what is kept until something takes it | W/m |
| θ | the angle between its crest and the depth contours (its direction to the shore's normal) | rad |
| β | the slope of the shore where the level meets it (rise over run) | – |
| ξ | the surf similarity (Iribarren) number, β / √(H₀/L₀) | – |
| R | the run-up: how high above the still level the swash's front climbs | m |
| u, Fr | a flow's speed and its **Froude number** u/√(g d): under 1 a wave can travel upstream, over 1 it cannot | m/s, – |

A wave in shallow water travels at √(g d), so a bore and a river's standing wave are the same object seen from two frames, and the Froude number is what S103's channels and this page's surf share: a bore is the moving hydraulic jump between a supercritical sheet and the slower water behind it.

## The audit: what main drew, and what it read from the ground

Main at 33ff2824, the scene `content/test-scenes/beach/` (a bay made for this: a long beach of about 1:30, a pocket of about 1:8 behind a rocky point with a stack off it, boulders in the swash; an 8 m/s wind over 60 km, which by the fetch laws is 1.1 m at 4.7 s, coming in 25° off the long beach's normal). Flown offscreen at 1920 × 1080, `--raster rt --shadows rt`, 15:00, four held cameras cut as the owner's reels are (sixteen frames 1/15 s apart, eight 0.25 s apart), under the GPU lock; the material root was the worktree's generated pack. By function:

| Mechanism | What main draws, and where | What it reads from the ground | Wave or clock | What the eye sees in the sheets |
|---|---|---|---|---|
| **1. A wave travels, and the ground sets its speed** | Near the shore one train, the wind's peak wave: its phase is the arrival field, an eikonal with the dispersion relation's celerity at each texel's depth (`gfx::water_arrival_field`); its wavenumber the local one (`water_height`, `water_waves.slang`). Offshore, sixteen deep-water bands that fade out where k₀d < 1 | **The depth map, per texel, for the phase.** But the eikonal's source was every deep texel at time zero: a pool, not a wave from somewhere | The wave's (the arrival) | **Crests are born parallel to the depth contour and stay so**: seen from 30 m up the surf is concentric bands along the whole bay, and every place on the long beach is reached at the same instant. The wind's 25° plays no part inshore of 5.5 m of depth. The beach pulses in unison, which is the "animation laid along the waterline" |
| the height it carries | H₀ K_s(d): linear shoaling at the pixel's own depth, a closed form (`water_train_shape`) | **The depth under the pixel and nothing else**: no account of where the wave came from, so no focusing on the point, no spreading in the pocket, no lee behind the stack | – | The stack and the point stand in the same surf as the open beach; the bores are the same height everywhere at a given depth |
| a swell | none: the train is the local wind sea's peak, 4.7 s here, 5.7 s at most for 7 m/s | – | – | Short, quick surf; no long sets |
| **2. It breaks where it is too tall for its depth** | The height is capped at 0.78 d, each wave of the group of seven at its own depth; foam is born there (`water_bore_foam`), a roller in the last tenth of the phase, a lace that decays behind | **The depth under the pixel** (so the break line does follow a bar) | The wave's | Foam in bands that start at a contour, wide and white. Inshore of a bar the cap lifts again and the wave is whole at once: a broken wave has no memory |
| how it breaks | Foam off where the wave surges (ξ > 3.3), by **one slope a body** | One number a body (the mean slope across the waterline) | – | The same breaker on the 1:30 beach, in the 1:8 pocket and on the rock |
| **3. The bore loses height to the depth** | The saturated bore 0.78 d to the shore | The depth under the pixel | – | A ridge of water that reads as a wave from the side (the point's camera), low from the beach's |
| setup | none | – | – | – |
| **4. The swash** | A front that climbs to R G(k) in 0.4 T and is back at −0.2 R at T, every wave (`water_swash_front`); its phase the arrival at the shoreline; R is Stockdon's at the body's one slope | **One slope and one run-up a body**; the bank's height at the pixel decides only whether the front covers it | **The clock's**: a sheet lasts exactly one period on any beach, and no bore ever meets the backwash of the one before | The gentle beach and the pocket run up the same 0.31 m at the same beat; the sheet is always just leaving as the next arrives; nothing overruns |
| **5. The sand's wetness** | The time since the front last stood at a height, a closed form, into exp(−age / `drain_s`); a film for 3 s | The height of the pixel | The wave's (through the age) | A dark band that pales upward between sheets; the same on every sand, no glassy lower beach |
| **6. A rock or a steep shore** | The same swash with the same R; a boulder that is an instance is not in the depth map | The point and the stack are in the map (the crests wrap them); **a boulder is nothing** | – | The film's mirror runs unbroken under and round the boulders; no wave meets them, nothing is thrown, nothing pours off |
| **7. A lake** | The same model with the lake's own fetch; the table's shortest band is 13 cm | as above | – | not flown here; the reels `lake-ripples` and `lake-wind-ripples` still show what [the shore page](shore.md#what-stage-b-costs-and-what-it-looks-like) lists: no catspaws, no gusts |
| the surface where the eye is low | The mesh is lifted (rings graded to 0.25 m at the outline) | – | – | **An open body's lift was faded to nothing over the outline's first 17 m by distance from the outline alone** — and a sea's outline is its coast, so the inner surf lost its height: half at 8.6 m off the waterline |

So the owner's reading was right, and for nameable reasons. The phase followed the ground; nearly nothing else did. The three that the eye pays for first: **the wave came from nowhere** (no direction, so the whole beach in step), **the swash was a clock** (one sheet a period on every slope, one run-up a body), and **no swell** (so "how periodic the waves are" had no lever). The first pass builds those three and removes the fade from the surf.

## The published models

Each with what the engine takes. How far each source was read is said in [Sources](#sources): several here were opened only as secondary pages, and two only as abstracts.

### Speed, direction and height before breaking

- **Dispersion.** ω² = g k tanh(k d); in the shallows c → √(g d), in deep water c = g/ω. The engine solves it per pixel (`water_wavenumber`, [the shore page](shore.md#the-models)).
- **Refraction: Snell's law.** Along a straight coast the phase's slowness along the shore is kept, sin θ / c = sin θ₀ / c₀: a crest slows where it is shallow and so turns to lie along the contours. In general the phase τ obeys the eikonal |∇τ| = 1/c(d), whose first-arrival solution is the refracted crest pattern; **a plane wave is its boundary condition**. Held: `water_tests.cpp`, "a train that comes in at an angle turns to the beach by Snell's law".
- **Shoaling.** The energy flux between two rays is kept while nothing takes it: H/H₀ = K_s K_r, K_s = √(c_g0/c_g) = (2 n tanh k d)^−½ with n = ½(1 + 2kd/sinh 2kd), and K_r = √(b₀/b) the rays' spacing, which on parallel contours is √(cos θ₀ / cos θ). In shallow water at normal incidence that is **Green's law**, H ∝ d^−¼ (valid under about d < L/20). The engine has K_s; **K_r is the next pass's** (the design, below).
- **The gravity–capillary minimum.** With surface tension ω² = g k + (σ/ρ) k³ and no wave on water travels slower than c_min = (4 g σ/ρ)^¼ = **0.23 m/s, at a wavelength of 1.7 cm**: shorter ripples are capillary waves and run faster again. A lake's catspaws and the rings of a raindrop live there. Not built; the lake's stage takes it.

### Breaking

- **The limit.** A solitary wave on a level bed breaks at H = 0.78 d (McCowan 1894); in general the steepness limit is Miche's (1944), H/L = 0.142 tanh(k d), which is 0.142 L in deep water and 0.89 d in shallow. The breaker index used for irregular seas is γ = H_b/d_b of 0.7 to 0.8. The engine keeps **0.78**.
- **How it breaks: Battjes (1974).** By the surf similarity number with the deep-water steepness, ξ₀ = β / √(H₀/L₀): **spilling under 0.5, plunging from 0.5 to 3.3, surging or collapsing past 3.3** (with the breaker height, 0.4 and 2.0). Spilling: the crest's water slides down the face as a roller; plunging: the crest outruns the base that the bed holds back, curls and falls; surging: the base runs up the beach before the crest can break. The beach scene's sea on its 1:30 beach has ξ₀ = 0.19 and in its pocket 0.70: one spills, the other plunges. Since S102 the surging test is made with each stretch of shore's own slope. The plunging shape is a later pass's.
- **A broken wave keeps losing energy as a bore.** Battjes and Janssen (1978): the dissipation of a random sea is the fraction of waves broken Q_b times a bore's, D = (α/4) ρ g f Q_b H_max² with H_max = γ d (in shallow water) and Q_b from 1 − Q_b + (H_rms/H_max)² ln Q_b = 0; Thornton and Guza (1983) give the same with a smooth weighting in place of the truncated distribution. With the flux equation d(E c_g cos θ)/dx = −D this is what makes a wave that broke on a bar stay lower across the trough behind it and re-form, instead of regaining its height when the depth does. The engine's saturated bore (H = 0.78 d) is the limit of this on a plane beach; the equation itself is the next pass's.
- **Setup: Longuet-Higgins and Stewart (1962, 1964).** The waves' momentum flux (radiation stress) falls through the surf and the mean level rises to balance it: a small set-down to the break point (about a fifth of the setup), then a rise to at most **(5/16) γ H_b** at the shoreline. For the beach scene (H_b about 1.2 m) that is 0.29 m: a quarter of the wave, and most of what Stockdon's first term measures. Not drawn as a level; it is inside R.

### The swash

- **The front is a projectile: Shen and Meyer (1963).** After the bore collapses at the shoreline the front moves up the slope as x_s(t) = u₀ t − ½ g β t² (for a small slope): launched at u₀, slowed by gravity along the slope, **its top u₀²/2g high whatever the slope, reached after u₀/(g β) seconds**. The sheet behind it is thin, its depth falling as the square of the distance behind the front (h = (x_s − x)²/(9 g t²) in Peregrine and Williams's 2001 form of the same solution). So the height a sheet reaches is the bore's (its energy's); **how long it takes is the slope's**. Friction and what sinks into the sand make the backwash the longer half on a real beach; the engine keeps S94's 0.4 for the uprush's share.
- **How high: Hunt (1959), Stockdon et al. (2006).** Hunt: R/H = ξ for ξ under about 2.5 (regular waves on a smooth slope). Stockdon et al., from ten field experiments on sandy beaches: R₂ = 1.1 (0.35 β (H₀L₀)^½ + ½ [H₀L₀ (0.563 β² + 0.004)]^½), the first term the setup, the second the swash, of which the 0.004 is the infragravity part that does not depend on the slope. The engine takes Stockdon's, **per stretch of shore**, and above it a ceiling for steep shores, where a sandy beach's fit grows without bound: Van der Meer's (1988) 2% run-up on a rock slope is at most **1.97 H** however long the wave (Hunt's limit on a smooth slope is 2.5; EurOtop 2018 says "around 3").
- **Whether the next wave meets the backwash.** The natural swash period T_s (the sheet's whole life) against the wave's T: under 1 the swash is free, up and back before the next bore; over 1 the incoming bore overruns the backwash, the shoreline never returns to its rundown, and the swash at the wave's own frequency **saturates**: more wave gives more setup and more infragravity motion, not a longer excursion each wave (Brocchini and Baldock 2008 review this and the literature behind it). With the projectile's time and S94's uprush share, T_s/T = √(2E/g) / (β q T) for a sheet that climbs E: **4.4 on the scene's 1:30 beach under its 4.7 s sea, 1.7 in its pocket, 0.9 on a 1:5 shingle bank under a 1 m, 10 s swell**. That ratio is what the owner called "depending on how periodic the waves are".

### The sand

- **Infiltration and the water table: Horn (2002), Turner and Masselink (1998).** Above the beach's water table the swash's sheet loses water into the sand (infiltration); below it water comes back out (exfiltration), and the lower beach face where the table meets the surface stays saturated and glassy (the seepage face) while the upper face goes matte within seconds of the sheet's leaving. The rate is the sand's hydraulic conductivity, which spans about three decades from fine sand to gravel, which is why a shingle beach shows no shine at all and a flat fine beach holds a mirror for the whole backwash. The engine has one `drain_s` a body and a 3 s film; the grain's own rate and the water table's line are a later pass's.

### A wall, a rock, a steep shore

- **Reflection: Miche (1951), Battjes (1974).** A slope reflects what it does not break: the reflection coefficient is about **0.1 ξ²** up to 1 (Battjes), so the 1:30 beach reflects nothing (ξ 0.19, r 0.004), the pocket 5%, and a rock face (ξ past 3) nearly everything: in front of it the incident and reflected trains stand, with crests twice the wave's height at the wall and nodes a quarter wavelength out.
- **Run-up, splash and overtopping.** As above for the run-up's ceiling; a breaking wave that meets a face throws water to several wave heights, and a rock lower than the run-up is overtopped and pours for the rest of the period. The engine draws none of these yet; an obstacle's effect is a function of the wave's height and period, the depth at its foot and its own size, and the design gives it a place.

## The design: one model, in which a place's wave follows from its ground

**What is made once a body, on the CPU, from its depth map** (the field, `gfx::WaterSurface::field`, read through the C1 spline that S100 gave it):

1. **The arrival τ of each train** — the eikonal with the local celerity and a **plane wave as its source**, so the crests come from a direction, bend and bunch. *Built for one train.*
2. **The energy flux's height** — along the eikonal's rays, d(E c_g)/ds = −D − E c_g ∇·ŝ: shoaling, the rays' convergence and spreading (K_r: the point focuses, the pocket spreads, the stack casts a lee), and Battjes and Janssen's dissipation, so a wave that broke on a bar is lower behind it. Solved by one sweep in the order of τ, stored as the ratio to the closed form H₀ K_s(d) so the texel's coarseness does not show at the waterline. With it the **fraction breaking** Q_b (where the foam is born, in place of a contour) and the **setup**. *Next pass.*
3. **Along the waterline, each stretch's own numbers**, carried onto the bank and out into the shallows by the nearest stretch: the **slope β** (`gfx::water_shore_slope_field`, *built*), and with (2) the bore's height there.
4. For a closed body, the fetch behind each place (S100's `grown`, kept).

**What a pixel (and a vertex) computes from the field and the clock, with nothing kept from frame to frame:**

- the crest's phase (t − τ)/T, its shape at the local depth and height;
- **the sheets on the beach**: every wave launches a sheet whose reach is its own (the group's shape times the place's R) and whose life is D = √(2E/g)/(β q T) periods; the waterline is the highest of the sheets still there (at most five), so on a gentle beach a new bore runs in over the last one's backwash and on a steep one the sand is bare between waves (`water_swash_front_at`; *built*);
- when the front last stood at a height — the wet sand's age — still a closed form: the least over the last twelve waves of the time since each one's sheet left (`water_swash_age`; *built*, held against a scan);
- the foam: sums over the last waves of what each left, as S100 has it.

**Why no history.** Everything above is a function of the place and the clock, because the train is periodic in its group of seven: a replay draws the same beach, the clock wraps with no jump, and nothing is stepped, stored or streamed. What would need a history: foam and sediment carried by a current that is not the train's (a rip, a longshore drift), water left standing in a hollow above the level (though a hollow's rim and the last sheet to overtop it are again a closed form), and a truly irregular sea. None is in the first passes; the cost of state would be a field a body updated every frame, and a replay that must be run from its start.

**An obstacle that is not in the depth map** (a boulder instance): a short list a body of discs (centre, radius, height above the level), made at load from the instances that stand in the surf and swash band, binned on the field's grid so a pixel meets at most two. From a disc and the place's wave a pixel gets: the front's delay and thinning in its lee, the rise on its seaward face (the sheet's speed head, u²/2g), a wedge of foam behind it on the uprush and a streak on the backwash, and for a rock in the surf the white water of a bore that meets it, by the wave's height against the depth at its foot. *A later pass; nothing of it is built.*

**What each piece costs, stated before it is built** (budget: +0.5 ms of resolve at 3840 × 2160, traced, the bounce on, on the beach's poses):

| Piece | Where it runs | Stated cost |
|---|---|---|
| The plane-wave source | load only | none a frame; the eikonal is the same sweeps |
| The slope field | load: two chamfer passes over the map | none a frame: the field's fourth float was spare, and the spline already read it |
| The sheets that overlap | ground pixels in a body's swash band, water pixels on the bank | a loop of five where there was one, and the age's search twelve where it was eight, each with a square root more; an early exit for ground above its stretch's reach. Expected under 0.1 ms on a frame half beach |
| The swell | water pixels | one branch; the train is evaluated offshore too where it was not, about 0.1 ms on a frame half sea |
| The energy-flux field (next) | load: one more sweep; frame: one more float from the same nine texels | under 0.05 ms |
| A second train (next: the wind's peak beside a swell) | a second arrival and height: the field grows from 16 to 32 bytes a texel (32 MB for a 1,024-texel map) | the train's cost again on water pixels in the shallows, about 0.1 ms |
| Obstacle discs (later) | pixels in the band, two discs each | about 0.1 ms |

**Staged, with what each returns to the eye:**

1. **This pass**: a direction (crests oblique to the beach, turning as they come in; the swash arriving along the beach, not all at once), the slope's own swash (the pocket's sheets higher and brisker than the long beach's; bores overrunning the backwash on the gentle beach), a swell, the surf's height near a coastal outline.
2. **The energy-flux field**: surf that is higher on the point and lower in the pocket, a lee behind the stack and the point, a wave that stays broken behind a bar, foam born where waves actually break, the setup as a level; the wind's own train beside a swell, so the shallows keep their chop.
3. **The rocky shore and the boulders**: reflection and the standing pattern at a steep face, the run up it, white water where a bore meets rock, the lee, the pour-off; the discs.
4. **The sand**: infiltration by the ground's own grain, the water table's line (glassy below, matte above), the backwash's rills.
5. **The large wave**: a plunging breaker's lip and tube (geometry the height field cannot carry: an overhang), spray. The owner has no references for these yet.
6. **The lake's own scale**: capillary ripples with their minimum speed, catspaws under gusts, lapping.

## What the first pass built

- **A wave has a direction** (`gfx::water_arrival_field`'s `direction`). The source texels (at least 1/k₀ deep, or the deepest fifth) carry a plane wave's arrival, x · n / c at the source's shallowest depth, instead of zero. On the scene's beach the crests now come in 25° off the normal at 5.5 m of depth and turn to 12° at 1 m and 7° at 0.3 m, as Snell's law says (the test holds the along-shore slowness to 2% and the angle to 0.7° on a plane beach; measured 9 × 10⁻⁶ and 0.19°). The direction is the wind's for a wind sea — a closed body's too — and the swell's own for a swell. **The bank no longer conducts.** S100 crossed the bank at 10 km/s so that it took its shoreline's arrival; with a direction that carried the earliest arrival on a coast along the whole of it, and the first run of the Snell case found every place on the waterline reached within 2 ms of every other where 20 m of beach should differ by 2.7 s — the whole beach's swash in step again, with the lead running back out into the shallows. The sweeps now treat the bank as a wall, and it takes the arrival of the nearest water the train reached (`gfx::water_carry_nearest`, the chamfer the slope uses). **What it does not do**: the source is taken as a plane wave over the whole deep region, so a headland's lee is reached by the phase (the first arrival creeps round it) with the height it had: the lee in height is stage 2's.
- **Every stretch of shore has its own slope** (`gfx::water_shore_slope_field`, the field's fourth float): the gradient of the signed depth at the waterline's texels, averaged over the waterline within three texels, carried to every other texel by its nearest stretch (a 3-4 chamfer). A pixel's run-up is Stockdon's on that slope (`WaterSurface::reach_scale`), no higher than 1.97 H₀ and than the depth map's bank range can show (`reach_cap`).
- **A sheet lasts as long as its slope keeps it, and sheets overlap** (`gfx::swash_span`, `water_swash_front_at`, `water_swash_age`): the model above. The uprush of a free sheet is the projectile's to 10⁻⁹ m (`swash_tests.cpp`).
- **A swell** (`swell_height_m`, `swell_period_s`, `swell_direction_deg` on an open body): the train that shoals, turns, breaks and runs up is the swell's; the wind's table stays the surface offshore and calms where its own peak feels the bottom; the swell's train is drawn over the whole body.
- **The surf keeps its height at a coastal outline** (`water_outline_fade`): an open body's lift fades in from its outline only where the outline stands in deep water.
- **The mesh's sheet ends inside every pixel's front** (`k_water_mesh_inside`, 0.85). A pixel's reach has lobes of a tenth and a twentieth either way that a vertex cannot carry; where the mesh's smooth front stood past a pixel's lobed one, the water pixels between were bared — the bed drawn through the water's branch, which is not the ground's own picture — in strips with the mesh's straight edges, up to 1.4 m wide on a 1:30 beach. S100 had the same strips; they passed in a quarter second. With sheets that overlap the edge stands still to be looked at, and the first flight of this stage showed them as pale wedges on the sand. The mesh now stops under the sheet everywhere, and what lies between its edge and the front is the ground's own pixels, filmed.

What it cost and what the sheets show are on [the water page](../subsystems/water.md#the-swell) with the day's numbers.

## Where the research and the budget were traded

One train still comes ashore (the swell's where there is one, else the wind's peak), so with a swell the shallows inshore of the wind sea's own depth have no chop. The height is still the closed form at the pixel's depth, with no refraction coefficient and no memory of breaking. The uprush's share of a sheet's life is one number (0.4), where it should follow the sand's friction and what sinks in; a sheet's life is kept to five periods, past which the beach's motion is infragravity's and no train of one period draws it. The slope is the waterline's own, not the mean over the swash's excursion that Stockdon's β is, so on a beach with a berm the sheet's time is the lower face's. The run-up's ceiling on a steep shore is a rock slope's from breakwater tests, used for a natural rock for want of a better one. Surging on a steep shore is still drawn as the same sheet with no foam, not as a wave that stands against the rock.

## Sources

How far each was read for this page, said plainly. "Secondary" means the statement was read on a reference page that cites the source, not in the source.

- Battjes, J. A. (1974). Surf similarity. *Proceedings of the 14th Conference on Coastal Engineering*, 466–480. **Secondary**: the regimes and their thresholds as the Iribarren number's reference page gives them, citing this paper; the reflection coefficient 0.1 ξ² from memory of the paper, not re-read.
- Battjes, J. A. and Janssen, J. P. F. M. (1978). Energy loss and set-up due to breaking of random waves. *Proceedings of the 16th Conference on Coastal Engineering*, 569–587. **Secondary**: the dissipation and the equation for Q_b as SWAN's technical manual states them (its section on depth-induced breaking), read whole.
- Brocchini, M. and Baldock, T. E. (2008). Recent advances in modeling swash zone dynamics: influence of surf-swash interaction on nearshore hydrodynamics and morphodynamics. *Reviews of Geophysics* 46, RG3003. **Abstract only.**
- EurOtop (2018). *Manual on wave overtopping of sea defences and related structures*, 2nd edition. **Secondary**: the run-up's form and its "maximum around 3" as the wave run-up reference page gives them.
- Horn, D. P. (2002). Beach groundwater dynamics. *Geomorphology* 48, 121–146. **Abstract only**; the seepage face and the matte upper beach are stated from the literature it reviews, not read there.
- Hunt, I. A. (1959). Design of seawalls and breakwaters. *Journal of the Waterways and Harbors Division* 85(3), 123–152. **Secondary** (the run-up page): R/H = ξ, limit 2.5.
- Longuet-Higgins, M. S. and Stewart, R. W. (1962). Radiation stress and mass transport in gravity waves, with application to "surf beats". *Journal of Fluid Mechanics* 13, 481–504; and (1964), Radiation stresses in water waves; a physical discussion, with applications. *Deep-Sea Research* 11, 529–562. **Secondary** (the wave setup page): the maximum setup (5/16) γ H_b and the set-down's size.
- McCowan, J. (1894); Miche, R. (1944), Mouvements ondulatoires de la mer en profondeur constante ou décroissante. *Annales des Ponts et Chaussées* 114. **Not opened**; the limits as the coastal engineering literature states them.
- Miche, R. (1951). Le pouvoir réfléchissant des ouvrages maritimes exposés à l'action de la houle. *Annales des Ponts et Chaussées* 121, 285–319. **Not opened.**
- Peregrine, D. H. and Williams, S. M. (2001). Swash overtopping a truncated plane beach. *Journal of Fluid Mechanics* 440, 391–399. **Not opened**; the sheet's depth is quoted from memory of its form of Shen and Meyer's solution and is used by nothing built.
- Shen, M. C. and Meyer, R. E. (1963). Climb of a bore on a beach, part 3: run-up. *Journal of Fluid Mechanics* 16, 113–125. **Not opened**; the projectile's law as [the shore page](shore.md#the-fronts-motion) already took it, and as the tests derive it from gravity on a slope.
- Stockdon, H. F., Holman, R. A., Howd, P. A. and Sallenger, A. H. (2006). Empirical parameterization of setup, swash, and runup. *Coastal Engineering* 53, 573–588. As [the shore page](shore.md#the-run-up) read it for S94; the formula's terms are re-derived in `swash_tests.cpp`.
- Thornton, E. B. and Guza, R. T. (1983). Transformation of wave height distribution. *Journal of Geophysical Research* 88(C10), 5925–5938. **Not opened** (a search result's summary only).
- Turner, I. L. and Masselink, G. (1998). Swash infiltration-exfiltration and sediment transport. *Journal of Geophysical Research* 103(C13), 30813–30824. As [the shore page](shore.md#how-long-the-sand-stays-wet) cites it; not re-read.
- Van der Meer, J. W. (1988). *Rock slopes and gravel beaches under wave attack*. Doctoral thesis, Delft University of Technology. **Secondary** (the run-up page): the 2% run-up's coefficients, its ceiling d = 1.97.
- Reference pages read whole for the statements above: the Iribarren number, capillary waves (the minimum 0.23 m/s at 1.7 cm), wave setup, wave run-up, swash; wave shoaling (its first 100,000 characters: the flux between rays, Green's law and its validity).
