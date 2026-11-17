# White water and foam at the top tier: the research and the design (W139, 2026-11-16)

**Why this page exists.** The owner, shown the forest's streams and beaches on 2026-11-16: "the white
on the faster stream doesn't look realistic", and of foam everywhere, beaches included, "what we
have now would be fine on low graphics settings, but on max graphics (which to my knowledge is where
we test right now) it doesn't pass the realism grade". The forest session's needs 128, 139, 140,
144, 147 and 148 are the detail. This page is the research first and the design after it; **no
part of the top tier is built before it**, and the part that is built is listed at its end. The bar
is a photograph. Today's white (the Froude rule, W130's broken rule, the fall's strands, the swash's
foam line) is kept whole as the **low tier** ("The quality setting", below).

## What was read, and how far

| Source | What it gives | Read |
|---|---|---|
| Chanson (1996), *Air Bubble Entrainment in Free-Surface Turbulent Shear Flows*; Cummings & Chanson (1999) | entrainment at a plunging jet and at a free surface needs a speed of about a metre a second (inception 0.74 to 1.58 m/s); nappe flow on a stepped cascade strikes each step and is white past it | as [flowing-water](flowing-water.md) and [steep-streams](steep-streams.md) cite them: summaries, not reread tonight |
| Valle & Pasternack (2006), "Air concentrations of submerged and unsubmerged hydraulic jumps in a bedrock step-pool channel", *JGR* 111, F03016; Pasternack's field notes on the American River | natural jumps in a step-pool channel: air content skewed by the roller's submergence, varying by up to 20 % across a jump's width; the classical jump a poor analogue for surface air; air content in time over the whole 0 to 100 %, about 25 % on average | the abstract and the project's pages |
| Smetana's roller, L = 6 (y2 − y1) (as `domain/channels`' hydraulics.h already gives it; Hager, Bremen & Kawagoshi 1990 for the roller's length) | the aerated roller of a jump is a few of its downstream depths long | the engine's own; Hager et al.'s abstract |
| Clift, Grace & Weber (1978), *Bubbles, Drops and Particles* | the terminal rise of a millimetre to a few millimetres air bubble in water is about 0.2 to 0.3 m/s | recalled, not reread: labelled so where it is used |
| Craig, Ninham & Pashley (1993), "Effect of electrolytes on bubble coalescence", *Nature* 364 | salt above about 0.1 M stops bubbles coalescing; fresh water's bubbles merge and burst | recalled: why a stream's white clears in a metre or two and the sea's foam persists |
| Monahan & O'Muircheartaigh (1980), *J. Phys. Oceanogr.* 10, 2094 (as Andreas & Monahan 2000 cite it) | whitecap cover W = 3.84 × 10⁻⁶ U₁₀^3.41 (a fraction; U₁₀ in m/s), the sum of stage A (the active crests) and stage B (the decaying patches) | Andreas & Monahan (2000) and a conference summary |
| Callaghan, Deane & Stokes (2012), *JGR*; Callaghan (2024) | a whitecap's foam decays with e-folding times of 0.2 to 10.4 s an event, longer for larger patches | the abstract and search summaries |
| Koepke (1984), "Effective reflectance of oceanic whitecaps", *Applied Optics* 23, 1816; Frouin, Iacobellis & Deschamps (1996, 2000) | fresh foam reflects about 55 % of visible light, isotropically; a whitecap's reflectance averaged over its life is about 22 %, because it thins as it ages; bubbles under the surface brighten the water's own colour | search summaries of both; the papers not reached |
| Battjes (1974), Galvin (1968): the surf similarity parameter | ξ₀ = tan β / √(H₀/L₀): spilling under 0.5, plunging 0.5 to 3.3, surging and collapsing over 3.3 | recalled; the classification is textbook |
| Ihmsen, Akinci, Akinci & Teschner (2012), "Unified spray, foam and air bubbles for particle-based fluids", *The Visual Computer* | the graphics literature's split of white water into spray (in the air), foam (on the surface) and bubbles (under it), each with its own motion and life | the title page and the abstract |
| Vlachos (2010), Portal 2's flow maps; Gonzalez-Ochoa (2016), "Rendering rapids in Uncharted 4" (SIGGRAPH course) | games draw a river's foam as textures carried by a flow map, two phases cross-faded, and the white's place from a painted or simulated mask | the engine's own flow map is Vlachos's ([flowing-water](flowing-water.md)); the Uncharted talk from a summary |

**Not reached**: Koepke's and Callaghan's full texts; the decay time of stage B whitecaps as a
number from a paper (recalled as about 3.5 s, labelled so below); Andreas's spume threshold (recalled
as about 9 m/s of wind at 10 m). A row that pins one of these numbers reads its source first.

## What white water is

**Air drawn into water, seen from above.** Water turns white where air is entrained faster than it
leaves: under a plunging jet (a nappe's foot, a step, a breaking lip), in a jump's roller, and where
fast shallow water piles against a stone. Entrainment needs energy: the surface's turbulence must
beat surface tension and gravity, which on a free surface or at a jet's impact is a speed of about a
metre a second (Cummings & Chanson), and in a stream's reach-averaged terms a dissipation g S V over
about 0.02 m²/s³ (Ulseth et al. 2019's bubbly regime, as [steep-streams](steep-streams.md) has it).
Under that speed a step's water falls as glass and breaks nothing.

**It is a volume, not paint.** Bubbles of a millimetre scatter light and absorb almost none, so a
layer of bubbly water is white by multiple scattering, and how white is a matter of its optical
thickness. With a void fraction α of bubbles of radius r, the scattering coefficient is about
3α / 2r (each bubble's extinction twice its cross-section): at α = 0.1 and r = 1 mm, 150 per metre,
so a centimetre of it (optical thickness 1.5) is a translucent veil through which the bed shows and
5 cm (7.5) is opaque. The reflectance of a slab that scatters without absorbing, with an asymmetry g
(bubbles scatter forwards, g about 0.85), is in the two-stream approximation
R = τ(1 − g) / (2 + τ(1 − g)) (Kubelka–Munk with no absorption), and what it does not reflect it
passes on to the water and the bed under it. That is the edge every photograph shows: **thin white
is translucent and takes the colour of what is under it; thick white is opaque; and the brightest
fresh foam reflects about 55 %** (Koepke), not the 80 to 100 % a white paint does. In shade it is
lit by the sky alone and reads grey-blue (the reel `dusk-rough-shorebreak`), and a thin film of foam
over water full of bubbles is turquoise: the bubbles under the surface lift the water's own colour
(Frouin et al.).

**It clears as fast as the bubbles rise.** A bubble of a few millimetres rises at about 0.25 m/s
(Clift et al., recalled). Water carried at a speed V out of a plume d deep clears over about
L = V d / u_r: a step's plume 0.2 m deep at 1 m/s is clear in under a metre, a jump's 0.5 m roller
at 2 m/s in four. In fresh water the bubbles also merge and burst at the surface within a second
(Craig et al.: no salt to stop coalescence), so **a stream's white is local**: a short white fall at
each step, a band behind each stone and jump, streaked downstream and gone within a metre or two,
with dark clear water between even where the flow is supercritical (need 147; the reels
`step-pools-locked`, `boulder-step-pools`, `snowmelt-boulder-rapid`). Sea water keeps its foam:
a whitecap's patch decays over 0.2 to 10 s (Callaghan et al. 2012), and foam left on a beach lasts
until the film drains from under it.

**Where it stands and what moves.** A steady stream's white stands still: it is made at the same
step, stone and jump second after second, and what moves is its texture, carried through it at the
surface's speed. A sea's white is made where a wave breaks and is carried with the water after.

## The reels: shapes and sequences (never speeds)

No shot has a known playback rate or scale, so nothing is timed from them. What they fix is shape and
order. Looked at for this page: the forest session's `foam-engine-against-reels.jpg` (the engine
beside `foamy-swash`, `swash-coral-sand`, `step-pools-locked`, `boulder-step-pools`),
`step-pools-locked/slow.jpg`, `snowmelt-boulder-rapid/slow.jpg`, and the two `README.md` tables of
the second and third sets.

- **A steep stream** (`step-pools-locked`, `snowmelt-boulder-rapid`, `boulder-step-pools`,
  `low-ledge-drop`, `ledge-tongue`): a glassy dark tongue accelerating into each drop; a band of
  white **across** the flow at the drop's foot, brightest and most opaque there, lumpy, with fine
  streaks drawn out downstream; the band thins and tears into streaks and patches and the water is
  clear and dark (green or grey) again within a metre or two; dark wet stones stand in the white; a
  pillow (a smooth dome) over a sunk boulder breaking white just downstream; between frames the
  white's place holds while its texture streams through. In grey light the white is grey-blue, never
  paper white.
- **A beach** (`foamy-swash`, `swash-coral-sand`, `sunset-lace-shorebreak`,
  `afterglow-peeling-shorebreak`, `backwash-foam-spots`, `foam-spots-wet-sand`,
  `multi-bar-surf-mirror-band`): the lip whitens from its edge inward as air is drawn in; after a
  plunge a tumbling white band (bright clumps over a greyer base) runs up as a bore; the swash's
  white band **opens into a lace**: thin threads round larger dark cells, which stretch and tear as
  the sheet spreads and again on the backwash; several sheets overlap, each with its own scalloped
  front; separate few-centimetre spots stay on the sand while the film drains round them; a wet
  mirror band dries from its top edge.
- **Deep water after a break** (`foam-streaks-deep-water`): a blanket whose edge is round cells and
  holes, thin parallel streaks along the flow over black water thinning to dashes, turquoise where
  thin foam lies over aerated water.
- **Rock** (`cove-surge-drainage`, `surge-on-rock-shelf`): drainage as many white threads in the
  slab's grooves; foam marbled into swirls and loops over green water.

## What the engine draws today, and why it fails at the top tier

- **A stream** (water.md, "Shading"): the Froude rule (S96, S98: white from Froude 1 to most of the
  surface by 2, over a metre a second), W130's broken rule (a share of the surface by the sheet's
  slope, g S V and depth), the fall's strands (S108). Both stream rules are **a share of the whole
  surface by the section's numbers**, so at the owner's Manning's 0.04 (Froude 3) the Froude rule
  covers the whole ribbon (147): white is where the section is fast, not where air is drawn in; and
  it is drawn as a diffuse white surface of albedo 0.8 laid over the water by its share, opaque at
  90 %, with no thickness and no colour from under it.
- **A beach** (water.md, "The shore"): a foam line riding the swash front and short dashes: no lace,
  no bore, no spots left behind (148).
- **Open water**: no whitecaps, no spray.

## The design

### Streams first: white where the air goes in, gone where it comes out

**1. Sources from the solve** (CPU, at the mesh's build, deterministic like everything else there;
every input a station's own solved quantity, so an authored line gets it from its levels and speeds
and nothing names a scene or a generator):

| Source | Where | Strength |
|---|---|---|
| A fall's foot | the foot station of a lip-to-foot pair | the jet's speed √(2 g h) over the inception band 0.8 to 1.2 m/s; the plume's depth the smaller of the foot's depth and the jet's penetration |
| A jump | the head-to-foot pair (Froude falls through 1) | the roller, from the head over 6 (y₂ − y₁) |
| A step-pool or cascade reach | the reach over 3 % (Montgomery & Buffington), where the solve writes no steps yet | bands across the flow at the step spacing the literature gives (one to four widths on a step-pool reach, under a width on a cascade; Chin 1999's H/L of one to two slopes), placed from the plan (a station's distance along the line, seeded), each a source as a fall of H = S L |
| A stone in fast shallow water | a supercritical reach (Froude over 1) at small relative submergence | spots behind the stones the bed would have, hashed on the world's lattice; **not** the whole reach |

**2. The white carried and cleared downstream**: a(x) = Σ sources × e^(−(x − xₛ)/L) along the line,
L = V d / u_r with u_r = 0.25 m/s (Clift et al., recalled), so each source's white is a band a metre
or two long that fades: the stream between steps is dark water (need 147), and a supercritical
reach at Froude 3 is white only behind its stones and at its steps.

**3. Where it lives.** It is a field over the sheet, steady in the channel's frame (a steady stream's
white stands still). Tonight's leg carries the solve's numbers on the vertices (the section's Froude
number rides on UV.y: [steep-streams](steep-streams.md#the-whites-inputs-per-vertex)); the field
proper belongs **beside the depth map**: a second map at the depth map's 0.25 m texel (RG8: the
aeration and the plume's thickness), built in the same pass over the same texels
(`build_channel_depth_map`) and sampled bilinearly by the resolve, so it costs one fetch, has no
triangle in it, and is zero on every texel the depth map calls dry. Its upload is `gpu_scene.cpp`'s
(closed tonight), so it is the next leg's first step. Nothing is simulated per frame: the solve is
steady, and a simulation would only rediscover it.

**4. The structure at the pixel**, from the field's aeration A and the surface's speed: (a) at a
source, a lumpy band across the flow (noise of the plume's scale, a few decimetres, held in the
channel's frame); (b) downstream of it streaks drawn out along the flow by the distance the surface
travels in the bubbles' life, thinning to dashes as A falls (`foam-streaks-deep-water`); (c) the
threshold of the pattern falls as A rises, so dense white is a mass with holes and thin white is
threads; (d) the texture carried at the surface's speed through the standing field by the flow map
the channel already has.

**5. Lit as an aerated volume.** The white's optical thickness τ = A × (the plume's thickness) ×
3α/2r; its reflectance R(τ) from the two-stream formula above with g = 0.85, its single-scattering
albedo below 1 by a little (fresh foam 55 %); what it does not reflect passes to the water and the
bed, which the tier's own path has already shaded: colour = R × white_lit + (1 − R) × water. The
white is lit by the sun through the same shadow query (a soft wrap: multiple scattering lights it
past the terminator) and by the sky's ambient light, so in shade it is the sky's grey-blue; the
surface over it keeps its rough specular glint. Bubbles under a thin white lift the water's own
colour toward turquoise (the volume's single-scattering albedo up by A).

**6. Spray and mist** at a fall over about a metre and at a plunge: billboards in the air are the
first thing a budget cuts and are not in this leg; noted under the rows left.

### Beaches next (designed, not built tonight)

- **The breaker's type from the body's own wave** (its swash record's height, period and foreshore
  slope): the surf similarity ξ₀ (Battjes 1974): spilling white over the front face as a band
  (ξ₀ < 0.5), a plunging lip and an impact band (0.5 to 3.3), a surge with almost no white (over 3.3;
  the reel `sunset-backwash-sheet`).
- **The swash's foam as a net of cells**: a cellular pattern (a Voronoi of seeds fixed on the
  sand's lattice, hashed per swash cycle) whose borders are the threads; the cells open as the sheet
  spreads (the run-up's own strain, which the swash's analytic phase gives), so the band opens into
  lace and tears on the backwash, stretched along the downslope flow; spots where the film has
  drained stay put and fade over the drain time the record already carries (`drain_s`).
- **Whitecaps** on open water at Monahan & O'Muircheartaigh's cover W(U₁₀) = 3.84 × 10⁻⁶ U₁₀^3.41
  (1 % at 10 m/s, 0.1 % at 5), stage A on the crests the spectrum makes steepest (where the
  surface's Jacobian falls lowest, as the ocean literature does since Tessendorf), stage B patches
  carried with the water and decaying with an e-folding time of a few seconds (recalled about
  3.5 s; Callaghan et al.'s events 0.2 to 10.4 s).
- **A gale must read as a gale** (the wind row's pictures at 20 m/s, about 3 m of sea at 6.6 s,
  2026-11-16: no crest breaks, no whitecaps, no streaks, no spindrift, so a gale reads as a moderate
  breeze, and the swash floods the beach as one flat white sheet). What the sea looks like by wind
  is the Beaufort scale's own description (WMO): whitecaps begin at force 3 to 4 (about 4 to 7 m/s),
  are everywhere at force 6 (11 to 14 m/s) with some spray; at force 7 (14 to 17 m/s) foam from
  breaking waves is **blown in streaks along the wind**; at force 8 (17 to 21 m/s) crests break
  into **spindrift** and the streaks are well marked; past it dense streaks and spray cut the
  visibility. So the top tier draws, from the wind speed U and the sea's height: whitecap cover
  W(U) as above (3.84 × 10⁻⁶ U^3.41: 1 % at 10 m/s, 5 % at 16, 10 % at 20, capped where the
  literature's data end); **streaks along the wind** over about 10 m/s (stage B foam drawn out
  along the wind's direction, their share rising to dense bands by 20 m/s); **spindrift**, spray
  torn off the breaking crests and carried downwind over about 17 m/s; and on the shore, **a bore
  with a lace of foam behind it at any wave height**: the breaker's white is a band at the bore's
  front, opening behind it into the lace above, so a 3 m sea is a taller, faster, wider bore and
  more lace, **never a flat white sheet**.
- **The wind, read in one place.** Today's wind for a body is resolved once, at the scene's load, in
  `systems/renderer/src/scene_water.cpp` (`read_scene_water`'s `water.wind_m_s`: an open body
  takes the scene's wind, a closed one its own `wind_m_s`). The top tier's whitecap cover and spray
  read that value and nothing else; when the wind provider the other coordinator is building
  (`wind(place, time)`) lands, that one line is where it plugs in. (`gpu_scene.cpp`'s
  `swash_numbers` re-derives the same value for the swash; the next leg makes it read the same
  field.) **Where the two branches meet** (the wind row, `b67-W136`, handed back 2026-11-16 at
  f8dedacc): its `gfx::sea_state(desc, t, fetch)` (domain/gfx/wind.h) gives `wind_m_s`,
  `sea_height_m` and the swell, `gfx::wind_gust` the gusts, the scene's wind is
  `SceneData::wind` (a scene names it under `Sky.wind`; by hand `renderer.wind.speed_m_s` and
  `renderer.wind.to_deg`) and a body's on the GPU side `GpuScene::water_wind_frame`. When this
  row is stacked on it, the one line above becomes a read of `sea_state(...)` for the body (its
  `wind_m_s` for the whitecaps', the streaks' and the spindrift's speed, its `sea_height_m` for the
  breaker's type and the bore), and the GPU's whitecap pass reads the body's
  `water_wind_frame`; nothing else in the white reads a wind. Its test scene for the gale is
  `content/test-scenes/beach-wind` (on that branch).

### Cost, against the frame

The top tier adds at a stream pixel one bilinear fetch of the white field, about twelve noise
evaluations for the band and the streaks (W130's broken rule already spends ten), and the two-stream
reflectance (a few ALU); a pixel with no white pays the fetch alone. W130 measured its ten fetches
inside the runs' 0.3 ms spread of the resolve at 3840 × 2160 native, traced shadows, reconstruction
off (4.66 to 4.97 ms before, 4.78 to 4.79 after, machine not quiet). The top tier's target: **at most
0.5 ms more resolve at 3840 × 2160 native with traced shadows** on a frame where channels fill a
third of the picture, and nothing on a frame with no white. Spray, when it comes, is particles and
is measured on its own.

### The quality setting

**Today's white is the low tier and stays whole**: the tunable `renderer.water.white_tier`
(`low` or `high`, default `high`) reaches the resolve as one bit of the water's flags, and `low`
draws exactly the white W130 drew (with the three faults below mended in both, since they are
faults and not a tier). A scene, an asset or a hand-authored channel needs nothing new for either
tier; a scene with no water pays nothing.

## What this leg built (2026-11-16 night)

- **The three faults of last night's white** ([steep-streams](steep-streams.md#the-whites-inputs-per-vertex)):
  the section's Froude number per vertex (no white triangle on still water, no facet at a
  triangle's edge), the slope from the vertex normals, and no white on a film.
- **The streams' top tier, first cut** (`water_channel_white_top`; [water](../subsystems/water.md#shading),
  "Two tiers of white"): the steps' bands standing in the channel's frame and clearing at the
  bubbles' rise, the stones' wakes in fast shallow water, a carried texture, and the two-stream
  slab's reflectance over the water under it; the tier switch `renderer.water.white_tier`. What it
  stands in for, and where it falls short of the design: the steps are placed by the phase (one
  every 2 s of travel, so their spacing in metres is the speed's, a metre at 0.5 m/s and four at
  2 m/s) and not by the solve, which writes no steps yet (steep-streams.md, "The step-pool form,
  solved or drawn"); a fall's foot has no source of its own (the nappe's strands end at it); the
  stones are noise, not the bed's clasts; the field is computed per pixel from the vertices' numbers,
  not built beside the depth map.
- **Not built**: the white field beside the depth map (it needs `gpu_scene.cpp`, closed tonight),
  beaches (the breaker's type, the swash's lace, spots left on the sand), whitecaps, spray and mist.

## The second leg (2026-11-16 night)

### The cost

The resolve at 3840 × 2160 native, traced shadows, reconstruction off, on the torrent judging scene
(`engine-view --benchmark`, a held camera, 240 warm-up frames and three repeats of 60, the tier by
`--tunable renderer.water.white_tier`), runs alternated. **The machine was not quiet**: other
agents' builds held the CPU at 54 to 98 %, and other rows' renderer tests held the GPU lock around
every run (the harness's `machine_state`, in each run's summary line).

| Camera, build | tier 0 | tier 1 | tier 0 | tier 1 |
|---|---|---|---|---|
| a-up (the stream ~8 % of the frame), first cut d274eb9e | 4.789 | 7.466 (the GPU at 100 % from another process by its end) | 4.725 | 4.678 |
| e-close (the stream about a quarter of the frame), second cut 96eeb549 | 5.255 | 7.626 (the GPU at 99 to 100 % from another process throughout) | 5.256 | 5.534 |

Milliseconds of the resolve, medians over the path's frames (`cost.ps1` in the row's scratch; the
runs' summaries beside it). The two tier-1 runs that met another process on the GPU are outliers
of the machine, not of the shader: their p95 (8.2 and 9.9 ms) is twice their neighbours'. **The top
tier costs about 0.28 ms of resolve at 3840 × 2160 native on a frame a quarter stream** (5.534 against
5.255 and 5.256, whose spread is 0.001 ms), against the design's 0.5 ms; on the far camera, where
the stream is 8 % of the frame, nothing measurable. Where it goes: about seventeen gradient-noise
evaluations at a white pixel (the front's three octaves, the reach's variation, a lip's noise for
each band that reaches the pixel, the mass's four and the threads' six), and nothing at a pixel
whose bands and stones give no air, which returns before the texture. The second cut's constants
(96eeb549 to its successor) change no evaluation.

### The streams' white to the photograph

What the coordinator saw against a photograph of a small cascade, worst first, and what this leg
did about each:

- **Every band had the same scalloped, hard-edged front at an even spacing.** Two causes. The band
  was drawn per cell of the phase, so where the cell changed the band's place and strength jumped:
  a line across the flow at every cell's edge. And the front was one octave of noise at 0.6 m with
  the white starting whole at it. Now each pixel sums the three bands whose white can reach it
  (none ends at a line); a band's place in its cell (0 to 0.8 of it), strength (0.2 to 1) and
  drop (0.5 to 1.5 of the reach's) are drawn per band, so the spacing runs from a fifth of the
  mean to nearly twice it; the place a band strikes is moved along the flow by noise at 2.5 m (the
  lip is not straight), 30 cm (the boils) and 7 cm (the bubbles' clusters), and the white rises
  over 5 cm of travel; a slow variation over 7 m breaks the phase's 8 s repeat.
- **The white was flat**: the mass now stands up to 3 cm proud where it is thick, by the normal
  (the white's own, so its lumps are lit and shaded, with a wrap of 0.35 past the terminator for
  the light the bubbles scatter round, and the surface's, so the lumps glint), and its hollows are
  shaded down to 0.78.
- **No stone stands in it or parts it.** A lip's strength now varies along it, read where the
  pixel's water crossed the lip, so the white comes off a step as tongues with glassy water
  between, which is what a stone in a lip does to it. The bed's own clasts are not read: they are
  placed with the plan (`domain/channels`' `place_pieces`), which the water's generator does not
  read. What reads them is the white field below.
- **It thins to threads**: the zero lines of a pattern smeared along the flow (two taps, farther
  apart the older the white), kept only where they run along the flow, are what is left as the
  mass's threshold rises, so a band ends in lace and single streaks drawn out by the current. (The
  third cut's threads were the ridges of the smeared pattern everywhere, and drew loops like wire.)
- **The bands sit on a smooth plane**: the bed's steps are row W132's. **Where a band stands**: from
  the travel time, because the description carries no steps (`scene_gen/water_channel.h`'s flags
  name a fall's lip and foot and a jump's head and foot, nothing else). When W132 writes steps, the
  white takes each step's place and height from its station and the travel-time bands are what a
  reach with none keeps.

### What still fails against a photograph

Judged on the torrent (cameras `a-up` and `e-close`, 14:00, traced shadows; the row's scratch
`captures\before-day`, `v4-high`, `v2-low`) and on main's beach from eye level and from 30 m with
its 8 m/s wind and at 20 m/s (`beach-v4`, `gale-v4` against `beach-before`, `gale-low`), against
the reels' shapes:

- **Streams**: the white is now patches of lumpy foam across the flow at each band with clear water
  between and threads drawn out behind, which is the reels' order; but it still reads as foam lying
  on the water rather than water churning white: no boil lifts the surface, the relief is a normal
  and not a height (its silhouette against the light is flat), and nothing of the white is above
  the sheet where a step's jet strikes. The bands still stand on a smooth plane (W132), and no stone
  stands in them (the clasts are not read).
- **Beaches**: the bores are a white band at the crest opening into lace and the swash a thin bright
  front with lace and spots behind, never a sheet, at 1.1 m and at the gale's height; but the foam
  is flat-lit grey (no relief, no translucency to the water under it, as the streams' now have), the
  crest's band is a straight line along the crest where a photograph's breaks along it in sections,
  and a 20 m/s sea has no whitecaps or streaks offshore (the wind row's).
### The white field: the depth map's two spare channels

The design's field "beside the depth map" needs no new image: the depth map is uploaded as RGBA8
with red and green the 16-bit depth and **blue and alpha unused** (`gpu_scene.cpp`'s
`upload_water_depth`, which writes 0 and 255 there). So the field is two bytes a texel of the map
that already exists: blue the aeration A (the sum of the sources upstream with their clearing,
e^(−t u_r / d)), alpha the plume's thickness. The CPU side is `build_channel_depth_map`
(`systems/water/src/channel.cpp`), which already visits every texel at the place on the line it
stands for (`channel_at`): the sources are the description's fall feet and jump heads (flags) and,
once W132 writes them, its steps; the stones are the bed's clasts, which the generator would read
from the plan's pieces (`place_pieces`' log, `domain/channels`) — the one part that needs the
generator to read more than the channel's description. The map's file format (`DepthMap`,
`k_depth_map_version`) takes a byte plane; the upload is two lines in that lambda; the shader reads
`t.b` and `t.a` in the one fetch it already makes. Not built tonight: `gpu_scene.cpp` was closed to
other rows, and it is the next leg's first step (roadmap W139's "Not done").

### Beaches: the first piece

On main's beach (1.1 m at 4.7 s, a 1:30 shore) from above, the bores' foam lay as flat grey slabs
with straight edges along the crest lines, and the swash's foam as flat bands; on the wind row's
gale (3 m) the swash flooded the beach as one white sheet. One cause: the lace's density is
`water_foam_amount` = H / 0.5 m, whole from half a metre, and at a density of 1 the threshold
covers every texel of the pattern. The top tier now (`water_uprush_foam_top`; the bore's foam in
`water_bore_foam` with the tier):

- **the bore's front is a band** `0.35 H` wide (6 to 70 cm), densest at the front and ragged along
  the shore at its own scale and four times it, opening behind into
- **the lace, capped at a density of 0.7** (`k_beach_lace_max`), so its cells stay open whatever
  the sea: threads round dark cells, thinning as the sheet's bubbles burst;
- **spots stranded on the sand** where the backwash has bared it: the peaks of a 5 cm pattern,
  fewer and smaller as they age, fading over 2.5 times the foam's decay (the reels
  `backwash-foam-spots`, `foam-spots-wet-sand`);
- the bores' foam on the water: the bore's own white for a fifth of a period behind its crest (a tumbling band), then the lace capped the same way; the roller's front ragged along the crest.

Every height and period the beach's white reads, at both tiers, comes through **`water_white_sea`** (water.slang; `water_white_sea_of` for the bores' foam, which has the body's surface record in hand), which reads the
shore record (`WaterShore::height_m`, `period_s`) that `gpu_scene.cpp`'s `water_swash_numbers`
fills from the body's wind and fetch. Not built: the breaker's type by the surf similarity as a
picture of its own (today's `water_foam_strength` already turns a surging wave's foam off), the
lace's cells stretched by the sheet's spreading, and several sheets' fronts overlapping as separate
scallops.

### Whitecaps, streaks and spindrift: what the stacked leg fills in

They read the wind, which is the wind row's (`b67-W136`): nothing of them is built here. On the
stack:

1. **The one read of the sea** is `water_white_sea` (water.slang): it gains `wind_m_s` (and the
   wind's direction) from the body's record the wind row writes (`GpuScene::water_wind_frame`), and
   its height and period come from `sea_state(...)` through `water_swash_numbers`, which on the
   stack becomes a read of `sea_state` for the body. No other function of the white reads a wind
   or a height.
2. **`water_whitecap_cover(wind_m_s)`**: Monahan and O'Muircheartaigh's 3.84 × 10⁻⁶ U^3.41,
   capped at the data's end (written beside `water_white_sea`, unused until the stack).
3. **`water_whitecaps(surface, p, t, sea, footprint)`** (to write): stage A on the crests the
   spectrum makes steepest, the threshold on the surface's Jacobian set so the area above it is the
   cover; stage B the lace of `water_foam_lace` carried with the water and decaying by
   `water_foam_decay`; called beside `water_bore_foam` for an open body's pixels offshore of the
   break.
4. **`water_wind_streaks`** (to write): stage B drawn out along the wind's direction over 10 m/s,
   dense bands by 20 m/s (Beaufort 7 and 8).
5. **Spindrift** is spray, particles: a row of its own, measured on its own.
