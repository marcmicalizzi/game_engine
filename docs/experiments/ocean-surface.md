# The ocean surface: what is known about wind waves, and what the engine builds from it

Roadmap **S93** (2026-10-31). The forest proof of concept's v23 scene put a sea to the east of its
map: one `engine.scene.WaterBody` named `eastern-sea`, a rectangle 928 m by 3,584 m (x 1,120 to
2,048, z −768 to 2,816) at a level of −6 m, with waves of 0.12 m and 8 m. It shows three things the
water lacks: the sea stops at its polygon, so past it the far ground and the sky; its surface is the
pond's four sines; and nothing under it is drawn. This page is the grounding for all three — the
published wave models, their constants checked against a source, what one high-detail open
implementation chose — and then the design, stage by stage. Stage 1, the sea to the horizon, is
built ([water](../subsystems/water.md#to-the-horizon)); the rest is designed here and not built.

## What the engine's waves are today, and what they cannot do

The water branch (`domain/gfx/shaders/water.slang`, [water](../subsystems/water.md#shading)) sums
**four directional sines** over the body-local point: the body's wavelength and amplitude times
{1, 0.62, 0.38, 0.24} and {1, 0.55, 0.32, 0.18}, turned from one main direction by the chop, each
travelling at the deep-water speed √(g k) rounded to whole cycles per 1,024 s. Only the slope is
evaluated; the mesh stays flat. For a pond under a breeze that is enough. For a sea it is not:

- **No spectrum.** Four wavelengths at fixed ratios from one authored length, with authored
  amplitudes. A real sea's energy is a continuous function of frequency set by the wind and how far
  it has blown (below); four lines of it are a visible pattern, which the pond's page already
  admits overhead ("four sines make a visibly regular pattern").
- **Dispersion only per wave, and in deep water only.** Each sine moves at its own deep-water speed,
  which is right; but nothing knows the depth, so a wave runs onto a beach at its open-sea speed
  and length.
- **No directional spread.** Four directions at fixed turns. A wind sea spreads its energy over
  angle, narrowest at the spectral peak and widening away from it.
- **A flat silhouette.** The normal moves; the surface does not. Up close a sea's crests stand
  against the sky, and a trochoidal crest is sharper than its trough.
- **No foam**, no whitecaps where the surface folds, no spray.
- **It stops at the polygon.** The body's mesh is its outline; there is nothing past it.

## The published models

### Dispersion

Linear (Airy) wave theory gives the relation between a wave's angular frequency ω and its
wavenumber k = 2π/λ in water of depth h:

  ω² = g k tanh(k h),

which in deep water (k h ≫ 1; in practice h > λ/2) is **ω² = g k**: phase speed √(g/k), so long
waves outrun short ones, and group speed half the phase speed. In shallow water (k h ≪ 1)
ω → k √(g h): every length moves at √(g h), which is why waves slow, shorten and turn to meet a
beach. With surface tension, ω² = g k + (σ/ρ) k³ (capillary–gravity; σ/ρ ≈ 7.4 × 10⁻⁵ m³/s²),
which matters only below a few centimetres. g = 9.81 m/s² here as in `water.slang`.

A worked number: the pond's main wave of 2.5 m has k = 2.513 m⁻¹ and ω = √(9.81 × 2.513) =
4.965 rad/s, a period of 1.27 s; the eastern sea's 8 m wave a period of 2.26 s.

### Frequency spectra

A spectrum S(ω) is the variance of the surface's height per unit of angular frequency; the
significant wave height is H_s = 4 √m₀ with m₀ = ∫ S dω.

**Phillips (1958)** argued that the high-frequency tail of a wind sea is saturated — limited by
breaking — so that S(ω) = α g² ω⁻⁵ there, with the "Phillips constant" α ≈ 8.1 × 10⁻³. This is the
tail every later form keeps.

**Pierson–Moskowitz (1964)**, the fully developed sea (the wind has blown long enough over enough
water that the waves no longer grow):

  S(ω) = α g² ω⁻⁵ exp(−β (ω₀/ω)⁴), α = 8.1 × 10⁻³, β = 0.74, ω₀ = g / U₁₉.₅,

with U₁₉.₅ the wind 19.5 m above the sea; its peak is at **ω_p = 0.877 g / U₁₉.₅**. Checked against
[wikiwaves, "Ocean-Wave Spectra"](https://www.wikiwaves.org/index.php/Ocean-Wave_Spectra) (after
Stewart, *Introduction to Physical Oceanography*). At 7 m/s that is ω_p = 1.229 rad/s: a period of
5.1 s and a peak wavelength of 40.8 m.

**JONSWAP (Hasselmann et al. 1973)**, from the Joint North Sea Wave Project's measurements of a sea
still growing with fetch F (the distance the wind has blown over water):

  S(ω) = α g² ω⁻⁵ exp(−(5/4)(ω_p/ω)⁴) γ^r, r = exp(−(ω − ω_p)² / (2 σ² ω_p²)),

  α = 0.076 (U₁₀² / (F g))^0.22, ω_p = 22 (g² / (U₁₀ F))^(1/3), γ = 3.3,
  σ = 0.07 for ω ≤ ω_p and 0.09 above.

γ^r is the **peak enhancement**: a growing sea is peakier than a developed one, and γ = 1 is
Pierson–Moskowitz in the JONSWAP shape. Checked against wikiwaves (above) and against the
[wavespectra library's construction page](https://wavespectra.readthedocs.io/en/v4.8.0/construction.html),
which cites Hasselmann et al. (1973) and states that γ = 1 reduces it to Pierson–Moskowitz. The
report itself ([UCSD's copy](https://airsea.ucsd.edu/wp-content/uploads/sites/10/2019/10/Jonswap-Hasselmann1973.pdf))
is a scan this session could not read as text, so the numbers are the secondary sources' (they
agree with each other and with the two implementations below). One reader of the literature
(Arc Blanc, below) prints ω_p without the cube root; the exponent 1/3 is right — without it ω_p
would not be a frequency.

Worked at web-ocean's defaults, U₁₀ = 7 m/s and F = 90 km: α = 0.076 × (5.55 × 10⁻⁵)^0.22 = 0.0088,
ω_p = 22 × (1.528 × 10⁻⁴)^(1/3) = 1.176 rad/s — a 5.3 s period and a 44.6 m peak wavelength. At
U₁₀ = 10 m/s and 10 km of fetch: α = 0.0144, ω_p = 1.98 rad/s, a 15.7 m peak — the short, steep sea
of an enclosed bay.

**TMA (Bouws et al. 1985)**: JONSWAP in water of finite depth, multiplied by a depth function

  Φ(ω, h) = tanh²(k h) / (1 + 2 k h / sinh(2 k h)),

with k from the finite-depth dispersion relation; Φ → 1 in deep water. Checked against wavespectra
(above), which cites Bouws, Günther, Rosenthal and Vincent (1985), *J. Geophys. Res.* 90(C1),
975–986. Horvath (below) and others use Kitaigorodskii's piecewise approximation in
ω_h = ω √(h/g) (½ ω_h² up to 1, 1 − ½ (2 − ω_h)² up to 2, then 1); the piecewise form was not
found in a source this session could read, so the engine would evaluate the exact form above,
which costs a tanh and a sinh per spectral sample at build time and nothing per pixel.

### Directional spreading

The directional spectrum is S(ω) D(ω, θ), with D integrating to 1 over θ and θ measured from the
wind.

- **Mitsuyasu et al. (1975)**: D ∝ cos^{2s}(θ/2), with s largest at the peak and falling away on
  both sides (s ∝ (ω/ω_p)⁵ below, (ω/ω_p)^−2.5 above). The secondary sources this session reached
  agree on the shape and put s at the peak between 5 and 30 for a wind sea (about 10 typical) and
  25–75 for swell; the coefficient that ties it to the wave age (11.5 (U₁₀/c_p)^−2.5, as it is
  usually quoted) was not checked against the paper, and the engine does not need it (below).
- **Hasselmann et al. (1980)**: the same cos^{2s} with its own fits of s to ω/ω_p and the wave age;
  not checked here either.
- **Donelan, Hamilton and Hui (1985), with Banner's (1990) high-frequency branch**, the form both
  implementations below use:

  D(ω, θ) = β / (2 tanh(β π)) · sech²(β θ), with r = ω/ω_p and
  β = 2.61 r^1.3 for r < 0.95, 2.28 r^−1.3 for 0.95 ≤ r < 1.6,
  β = 10^(−0.4 + 0.8393 exp(−0.567 ln r²)) above.

  Checked twice: the Arc Blanc paper (Algis, Bramas, Darles and Aveneau 2025,
  [arXiv:2503.03326](https://arxiv.org/html/2503.03326)), which attributes the first two branches to
  Donelan et al. and the third to Banner, and web-ocean's `sim.js` (below), which computes exactly
  these three branches. It is the narrowest at the peak (β = 2.28 there, and since its
  cumulative share within ±θ is tanh(βθ) / tanh(βπ), half the energy lies within ±14°) and widens
  both ways, which is what the sea looks like: long crests at the peak, a
  criss-cross of short waves.

### Synthesis: Tessendorf, and Horvath

**Tessendorf (2001)**, *Simulating Ocean Water* (SIGGRAPH course notes), is the method every
production ocean since has started from: draw a complex amplitude per wavenumber on a grid,
h̃₀(k) = (ξ_r + i ξ_i) √(P(k)/2) with ξ standard normal; advance it by the dispersion relation,
h̃(k, t) = h̃₀(k) e^{iωt} + h̃₀*(−k) e^{−iωt} (so the height stays real); and take the inverse FFT to
get the height field h(x, t) on a tile that repeats. The **horizontal displacement**
D(x, t) = Σ −i (k/|k|) h̃(k, t) e^{ik·x}, scaled by a "choppiness" λ, pulls points towards the
crests — the trochoid's sharp crest and broad trough — and the surface **folds** where the
Jacobian J = (1 + λ ∂D_x/∂x)(1 + λ ∂D_z/∂z) − λ² (∂D_x/∂z)² goes below zero, which is where
whitecaps go. Time loops if every ω is rounded to a multiple of 2π/T — the same trick the engine's
four sines already use with T = 1,024 s. The notes' spectrum is the Phillips form
P(k) = A exp(−1/(k L)²) / k⁴ · |k̂·ŵ|², L = V²/g the largest wave a wind V raises, with an
exp(−k² l²) suppression of the shortest; L = V²/g and the exp(−1/(kL)²) term are as the survey of
[Darles et al. (arXiv:1109.6494)](https://arxiv.org/pdf/1109.6494) quotes them.

**Horvath (2015)**, *Empirical directional wave spectra for computer graphics* (DigiPro '15,
DOI 10.1145/2791261.2791267; the abstract and the Arc Blanc paper's reading of it, the paper itself
being behind ACM's wall here): replace the Phillips spectrum with the oceanographic ones — TMA for
the frequency part, a choice of empirical spreading functions (Mitsuyasu, Hasselmann,
Donelan–Banner) for the direction — and add a **swell** parameter that elongates the spreading into
long parallel trains. Converting a frequency–direction spectrum to the FFT's wavenumber grid takes
the Jacobian of the dispersion relation: S(k_x, k_z) = S(ω) D(ω, θ) (dω/dk) / k, which is what
makes the oceanographic constants mean what they say on a graphics grid.

### Production

- **Sea of Thieves** ([Ang, *The Technical Art of Sea of Thieves*, SIGGRAPH 2018 Talks](https://history.siggraph.org/wp-content/uploads/2022/09/2018-Talks-Ang_The-Technical-Art-of-Sea-of-Thieves.pdf)):
  the ocean is an implementation of Tessendorf's FFT.
- **Atlas** ([Mihelich and Tcheblokov, *Wakes, Explosions and Lighting: Interactive Water Simulation
  in Atlas*, GDC 2019](https://gpuopen.com/gdc-presentations/2019/gdc-2019-agtd6-interactive-water-simulation-in-atlas.pdf)):
  sea states from a spectrum, buoyancy against the same surface, wakes and explosions added on top.
- **Far Cry 5** (Grujic and Cutocheras, *Water Rendering in Far Cry 5*, GDC 2018, as
  [80.lv summarised it](https://80.lv/articles/the-gdc-talk-water-rendering-in-far-cry-5)): lakes
  and rivers rather than an ocean, with half-precision maths to afford the shading.

### web-ocean, read as a reference

[Noniv/web-ocean](https://github.com/Noniv/web-ocean) is **MIT-licensed** ("MIT License, Copyright (c)
2026 Maksymilian Dendura", its `LICENSE`), so ADR-0014 would allow its code with the notice; **none
is copied here** — what follows is what it chose, read from its README, `js/main.js` and
`js/wgsl/sim.js`, and nothing of it is in the tree.

- **Spectrum**: fetch-limited JONSWAP for the wind sea (α and ω_p by the fetch laws above, σ 0.07 /
  0.09, γ 3.3) with **Donelan–Banner spreading**, plus a **narrow long-crested swell** (its own
  JONSWAP with γ = 6, scaled to a significant height, spread by cos^{2s}(θ/2) with s = 48). Deep
  water only: no TMA. Capillary–gravity dispersion, ω² = g k + 7.4 × 10⁻⁵ k³.
- **Defaults**: wind 7 m/s at 118°, fetch 90 km; swell of 1.8 m significant height, 120 m long, from
  94°; choppiness 1.25; a short-wave cut of 1.2 mm; the loop 600 s.
- **Cascades**: six of 256 × 256, patches of 1,103, 211.3, 39.7, 7.43, 1.393 and 0.2617 m, each
  owning a band of wavenumbers (a cascade's upper edge six of the next patch's waves per metre,
  12π/L), the finer ones toned down.
- **Foam**: injected where the total Jacobian folds, held in two camera-centred world-space maps
  (128 m and 1 km), decaying at 0.22 /s and drifting downwind — **foam with a history**, a
  simulation stepped every frame.
- **The horizon**: the surface is a **CDLOD quadtree of 17 levels to 160 km**, vertices morphing
  continuously between levels — geometry to the horizon, displaced.
- **Shading**: the sky from an environment cube rebuilt each frame; the sun's glitter from an
  anisotropic Gaussian slope distribution with Smith masking and LEAN-mapped slope moments, so the
  sub-pixel waves widen the highlight instead of aliasing; a subsurface glow on backlit crests.

**What matters here.** The spectrum, the spreading and the cascade bands are the literature's,
and the engine should take them from the literature too (it does not need web-ocean's code for
any of them). Two of its choices do not fit the engine as they stand: foam with a history (a
picture here is a function of the frame's time, and a capture at a time must not depend on the
frames drawn before it — ADR-0056's one clock, the water's "no history" rule), and a displaced
quadtree to 160 km (the engine's surfaces are cluster geometry in a visibility buffer with the GPU
seeing no world position, ADR-0053; a camera-relative mesh 160 km across is a second terrain
system). The LEAN-style filtering of the glitter is the right answer to what the pond's footprint
fade does crudely, and belongs to the FFT stage.

## The design

### Stage 1, built: the sea to the horizon (an analytic plane in the resolve)

**The question** (the brief's): a body may be **open** — its polygon is its shore, and past the
polygon the same water goes on to the horizon wherever the ground is below the level, without a
crack or a double surface where the two meet, the waves continuing across the seam, and a body
that is not open drawing exactly what it drew.

**Two answers were weighed.**

(a) **A far water level of geometry**, laid out from the camera like the far ground (ADR-0051):
nested squares of flat tiles, the margin rule, a tile placed only where the far lattice says the
ground is below the level, the body's own mesh cut out of the first square. It would put the sea in
the visibility buffer (ids, depth, picking), but it costs a layout that moves with the camera,
instances that come and go every time a square moves (a dynamic scene's pool), and a seam with
the body's mesh on two different 16-bit grids: the eastern sea's mesh quantizes at 5.5 cm a step
(3,584 m over 65,535) and a far tile at its own, so their shared edge would not be one set of
vertices, and a flat coplanar seam that is not watertight shows the sea floor through its cracks.
It also stops wherever its last square stops, as the far ground does at 78 km — and a tile set
"only where the ground is below the level" is decided per tile, so a coast is drawn in 256 m to
8 km steps at the far levels' spacing.

(b) **An analytic plane in the resolve**, which is what was built. The sea's surface is flat at the
level; the resolve already has, for every pixel, the ray through it and the distance to whatever
the visibility buffer holds there (or nothing). So a pixel whose ray meets the plane y = level
**before** its surface — or that has no surface and looks down — is the sea, shaded by the same
water branch at the plane's point. That gives:

- **To the horizon, exactly**: the plane has no edge; its last pixel is the eye's horizontal.
- **No seam**: the body's own mesh is a surface in the buffer and is shaded by the branch as
  before; the plane is drawn only on pixels whose surface is *not* water, so it never draws over
  the mesh, and wherever the mesh does not cover (its outline, or a crack) the plane is the same
  water at the same level. The waves are a function of the **body-local point**, the point less the
  body's instance's place in the frame, and the plane takes that same instance's place
  (`instance_from_eye`), so the two meet on the same phase to the bit.
- **The shore per pixel**: where the ground stands above the level the ground is nearer than the
  plane and wins; where it is below, the plane wins. The coast is where the far ground's own
  triangles cross the level, at whatever level of detail they are drawn, not at a tile's step.
- **No geometry, no layout, nothing per camera move**: a few dozen ALU operations a pixel, and only
  in the resolve's water modules, which only a scene with water draws with (the plain modules are
  the bytes they were; the frame budgets' scenes draw with them).
- **Every visibility path**: the mesh shaders, the vertex path and `--raster rt` all end in the
  same resolve, so the sea is the same under each; an "untraced" far level of geometry would have
  been missing from the ray path's picture.

What it costs, said plainly: **the plane is not in the visibility buffer** — the ids and depth
channels, picking and the reference path tracer see what is under it (the far ground, or nothing);
it is drawn **from above only** (an eye below the level sees no plane: underwater is below); it
floods **every** hollow below the level that the eye can see, inland ones too, because a pixel
cannot know whether its ground is connected to the sea (a mask is the fix if a scene needs one);
and it is **flat to infinity**, where the sky's planet curves: the sea's last row is the eye's
horizontal, a few pixels above the planet's own horizon from a hill (√(2h/R): 0.32° from 100 m),
and past the aerial-perspective table's 32 km it is no hazier than at 32 km, as the far ground is
(ADR-0051's consequence, unchanged).

**The bottom of the open sea.** Inside the body's depth map's square the plane reads the map, as
the mesh does, so the shading is continuous across the outline. Outside it, the depth under a plane
pixel is the level less the height of the surface the pixel would otherwise show — the ground the
ray meets behind the water, at its own place — and with no surface behind it, deep (the bottom
gone). The volume tier marches nothing there: the refracted view runs to a level bottom at that
depth.

**The shore of an open body** is where its depth is zero, not its outline: an open polygon's sides
that face the sea are not a shore, and the pond's fade from the edge (the bottom alone at the edge,
the reflection coming in over `shore_m`) would draw a band of wet sand across the open water at the
polygon's side. An open body's fade is by depth, over the depth the waves already calm in (25 cm),
on the mesh and the plane alike.

### Stage 1b, built: the spectrum in the shader (Gerstner waves)

Built on 2026-10-31 as below with N = 16 and the table made on the CPU, with one change from the
design: the waves' directions are drawn by inverting the spreading's cumulative distribution and
the steepness is each wave's own k a scaled down only where the sum passes 0.6
([water](../subsystems/water.md#the-spectrum) has what holds it and what it costs). The design as
it was written:

Replace an open body's four sines with **N = 16–32 Gerstner (trochoidal) waves** drawn from
JONSWAP at the scene's wind (`GroundProvider::wind`) and a fetch that becomes a body field, with
Donelan–Banner spreading:

- **Sampling, deterministic by the seed.** Divide [0.5 ω_p, 4 ω_p] (where JONSWAP keeps nearly all
  of its variance) into N bands of equal log width; in band i take ω_i at the band's centre, its
  amplitude a_i = √(2 S(ω_i) Δω_i) (the variance of the band as one sine), a direction θ_i drawn
  from D(ω_i, θ) by inverting its cumulative distribution at a hashed uniform (the sech² has a
  closed-form inverse: θ = atanh(tanh(βπ)(2u − 1)) / β), and a phase from the seed's hash — the
  same hash the four sines use. k_i from the dispersion relation (deep: ω²/g; finite: TMA's depth
  as a constant per body, its mean). ω_i rounded to whole cycles per 1,024 s, so the clock still
  wraps.
- **Steepness**: Gerstner's Q_i = q / (k_i a_i N), q ≤ 1 so no crest loops; the normal is the
  Gerstner normal; the silhouette stays flat (stage 1 has no displacement).
- **Where it is computed**: the CPU (`renderer::SceneWater` at load, in f64) writes the N waves
  into a table the water material names — an address in a material entry, a few hundred bytes a
  body — because a spectrum evaluated per pixel would cost the transcendentals of every sample on
  every pixel. The shader sums them with the footprint fade it has. The double-precision reference
  (`water_reference.h`) reads the same table.
- **Cost estimate** (not measured): 32 waves × a cos, a sin and a dozen ALU operations per pixel
  — at the measured 32 ps a water pixel for the volume tier's whole branch at 1080p, perhaps
  +40–80 ps a pixel, about +0.1 ms with the sea filling a 1080p frame, +0.6 ms at 3840 × 2160 and
  +1.7 ms at 11,520 × 2,160 in the worst case of a frame that is all sea. Measured on the
  `east-shore` camera when built.

### Stage 2, designed: FFT cascades

What an FFT ocean costs **here**:

- **The compute pass**, once a frame, before the cull: per cascade, a 256² spectrum update
  (h̃(k, t) from h̃₀ and ω), then inverse FFTs of the height, the two horizontal displacements and the
  slopes packed two to a complex transform (three transforms), then a pass that writes a
  displacement map (RGBA16F) and a normal-and-Jacobian map (RGBA16F). Four cascades are
  4 × 3 × (2 × 256 × log₂ 256 = 4,096 butterflies a row, 256 rows, both directions) ≈ 25 million
  butterflies, a few hundred MFLOP: well under 0.1 ms on the RTX 5090 and a few tenths on the
  Titan X; the maps are 4 × 256² × 16 bytes = 4 MiB with mips. Its spectrum h̃₀ is built once at
  load from the seed. It goes in through `RenderGraph::add_pass` from the water capability, so a
  scene with no open water pays nothing.
- **The surface**: near, the body's mesh displaced through the deformed-vertex pool (a rigid
  instance whose vertices move: the pool's stage reads the displacement maps; the cull pads its
  bounds by the cascades' largest displacement); far, the stage 1 plane with the cascades' normals
  and a flat silhouette — at 1 km a 1 m wave is 0.6 px at 1080p, so the silhouette is lost there
  anyway. The hand-over is where the cascades' largest displacement is under a pixel.
- **Per pixel**: four cascades × two filtered reads (normal and Jacobian) at the footprint's mip,
  in place of the sines; the glitter's roughness widened by the slope variance the cascades'
  coarser mips hold (LEAN's idea, as web-ocean does). At the volume tier's measured 32 ps a water
  pixel plus perhaps 20 ps for eight reads: about 0.4 ms at 3840 × 2160 and 1.3 ms at 11,520 × 2,160
  with the frame all sea; a sea that is a third of the frame, a third of that.
- **Foam without history**: the Jacobian at the frame's time, plus the same at t − 0.5 s and
  t − 1 s from the same spectrum (three more transforms, or a cheaper lower cascade alone), decayed
  analytically — a function of time and not of the frames drawn, so a capture at a time is the same
  bytes whatever came before. A foam map stepped every frame is what web-ocean does, and is refused
  here for that reason.
- **Determinism**: h̃₀ from the seed's hash (Box–Muller from two hashed uniforms per wavenumber, the
  same bits on every toolchain, as the four sines' phases are); every ω a whole number of cycles per
  period; the time the world clock reduced modulo the period in double (`gfx::water_clock`); no
  history. A replay draws the same sea.

### Underwater, designed

What each piece needs and from whom:

- **The camera below the level** (the eye's frame y below the open body's level, or inside a closed
  body's outline and below its level): the resolve draws the plane **from below** — the same
  analytic plane, hit when the eye looks up — with **Snell's window**: within the critical angle
  arcsin(1/1.33) = 48.8° of straight up the sky is seen refracted (the transmitted sky through the
  wave normal), outside it **total internal reflection** returns the water's own light. Needs: the
  water branch (mine), the sky (`sky_environment`, unchanged).
- **The water-tinted view**: everything the eye sees under water is attenuated and in-scattered by
  the body's extinction and scattering along the view — Beer–Lambert by the same σₜ, σₛ the volume
  tier uses — which is the medium's job: the volumetrics capability's froxels
  (`systems/volumetrics`) given a second medium below the level with water's coefficients (a
  hundred times air's), lit by the sun refracted at the surface. Needs: a "water medium" source in
  volumetrics (another agent's module, R112's) fed the body's optics and level from the scene.
- **Swimming**: the walker below the chest depth floats instead of stopping (S86's "swimming"):
  a buoyancy state in `apps/engine_view`'s walker, the depth from `depth_at` (which already answers
  the open sea past the polygon).
- **Bubbles**: a volumetric source in the same medium (spawned at the walker's head when below the
  level), not particles — the engine has no particle system, and the medium already advects and
  lights.
- **Plants**: flora placed below the level (kelp, seagrass) drawn as **untraced** flora so the
  water's surface is not their caster; they sway by the flora stage's wind with the body's current
  as the wind.

## What is measured

Stage 1's own cost and the pond's bytes are on [water](../subsystems/water.md#to-the-horizon).
