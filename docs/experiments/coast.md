# The coast as a landform: the grounding, and the design of a coast process (W128)

Roadmap W128, 2026-11-12. **Pages only: nothing on this page is built.** It grounds what a coast is
in the published models, and designs the process the [landform solver](../subsystems/landform.md)
will run to make one. The build is its own row and starts from here.

**Why.** The solver ends at the sea's level. Its sea takes everything its rivers bring, up to that
level ([deposition](../subsystems/landform.md#deposition)), so an island with deposition on builds a
plain of its own sediment round itself: the forest proof of concept's v42 island grew 2.6 km² of
land on a 1.0 km² footprint, and its authors switched deposition off, which also removed every fan
and valley floor on land. Nothing erodes at the waterline, and the slope limiter holds every slope
at its critical angle ([the slope limiter](../subsystems/landform.md#the-slope-limiter)), so no
output can hold a sea cliff, a platform or a stack. There is no shoreface and no beach. The
island's cliffs, its beach and its sea bed were shaped by hand. Of the owner's 80 new reference
scenes 48 are ocean or beach, and the owner's direction for water (2026-11-12: water "needs to
conform to the terrain it's on") makes the coast's shape the input to every wave: a wave shoals,
breaks and runs up by the bathymetry and the beach face it meets, and a flat hand-made beach gives
the wave model (roadmap S102, building today) nothing to follow.

## How the sources were read

Said once, because it qualifies every number below. No paper was read from its own pages in this
session. A source marked **opened** was fetched and questioned through the session's page-reading
tool, which answers against the page's text: the equations and constants are as it returned them,
and where the page was longer than the tool reads, the part read is stated. A source marked
**summary** was seen only as a search result's summary. A source marked **memory** was not opened at
all: the statement is what the author of this page recalls, it is given so the build row knows what
to look for, and **no number so marked may be pinned until the build row has opened its source**.
The classical papers (Bruun, Dean 1977, Moore, Hallermeier, Bascom, Sunamura 1992, Hsu and Evans,
Komar and Inman) are behind publishers and were reached only through the pages that restate them.
[What could not be found or opened](#what-could-not-be-found-or-opened) lists every gap.

## The shoreface and the beach

### The equilibrium profile

Below the waterline a sandy coast worked by waves tends to one concave curve,

    h(x) = A x^(2/3)

h the depth and x the distance seaward of the waterline, both in metres, and A in m^(1/3) (Bruun
1954 from Danish and Californian profiles; Dean 1977 from the United States' Atlantic and Gulf
coasts: both through the Coastal Wiki's shoreface page and Bosboom and Stive's textbook, opened).
Dean's derivation: if the surf dissipates the same wave power in every unit volume of water, with
the wave height a fixed fraction γ of the depth and the shallow-water celerity √(g h), the energy
balance integrates to that power of two thirds, and A depends on the sediment alone, not on the
waves offshore (Bosboom and Stive, opened). Dean's fits ran from **0.079 to 0.398 m^(1/3)**; Bruun's
North Sea beaches gave 0.135 and Florida's 0.10 (the same page).

**A from the grain.** Moore 1982 related A to the grain's diameter (not opened; one point of his
through a search summary: 0.07 for 0.25 mm sand). Dean 1987 put it on the fall speed:

    A = 0.5 w^0.44        (w in m/s; 0.067 w^0.44 with w in cm/s)

(both forms on Bosboom and Stive's page, opened; the Coastal Wiki gives the first). The fall speed
of a natural grain is Ferguson and Church 2004's w = R g D² / (C₁ ν + √(0.75 C₂ R g D³)), with
C₁ = 18 and C₂ = 1 for natural sand, R the submerged specific gravity (1.65 for quartz) and ν the
water's kinematic viscosity (the form and the two constants confirmed by a search summary of
implementations; the paper not opened). **Computed here** from the two, at ν = 10⁻⁶ m²/s:

| D50 | w | A | Depth at 100 m | Where it reaches 6 m |
|---|---|---|---|---|
| 0.2 mm | 2.3 cm/s | 0.096 | 2.1 m | 494 m |
| 0.3 mm | 4.0 cm/s | 0.122 | 2.6 m | 345 m |
| 0.5 mm | 7.1 cm/s | 0.156 | 3.4 m | 238 m |
| 1 mm | 12.6 cm/s | 0.20 | 4.3 m | 164 m |

These sit inside Dean's range and within a few percent of the table the engineering manuals print
(memory: 0.100, 0.125, 0.161 and 0.210 for the same four sizes; the manual was not opened). So a
shoreface is 150 to 500 m wide, and coarser sand makes deeper water closer in.

**What the form does not say.** Its slope is infinite at the waterline, so it cannot be the beach
face, which has a slope of its own (below); it is monotone, where the upper shoreface usually
carries one or more bars; and it describes the long-term mean of micro- to mesotidal sandy coasts
well on the upper shoreface and less well below (the Coastal Wiki, opened; it also gives Bodge
1992's exponential form and Inman's exponents of 0.3 to 0.45 as alternatives). The proof of
concept's hand-made beach used exactly this curve with A = 0.10.

### The depth of closure, and the wave base

Waves reshape the profile only down to a depth. Hallermeier's **inner** limit, the seaward end of
the profile that storms rework within a year, restated by Birkemeier and by Houston (the Coastal
Wiki's closure-depth page, opened; Hallermeier 1978, 1981 and 1983 themselves not):

    h_in = 2.28 H − 68.5 H² / (g T²)        (Hallermeier)
    h_in = 1.75 H − 57.9 H² / (g T²)        (Birkemeier 1985)
    h_in = 6.75 H̄                           (Houston 1995, from the mean annual significant height)

H is the significant height exceeded twelve hours a year, T its period. Typical values 3 to 10 m.
A year's storm of 3 m at 9 s closes at 6.1 m; a lake's 0.5 m at 2.5 s at 0.86 m (computed here).
Hallermeier's **outer** limit, past which waves on average move no sand at all:

    h_out = 0.018 H_s T_s √( g / (d50 (s − 1)) )

with the ordinary significant height and period and s = 2.65 (the same page; the shoreface page
writes the same law as (H_s − 0.3 σ) T_s √(g / (5000 d50)), and 0.018 / √1.65 is 1 / √5000).
Typical values 15 to 40 m. A sea of 1 m at 6 s over 0.3 mm sand gives 15 m by the formula; the page's
own worked example says about 11 m for those inputs, a discrepancy the build row must settle
against Hallermeier 1983 before the constant is pinned. The textbook **wave base**, half a
wavelength, is the depth at which orbital motion is felt at all: 50 m for an 8 s swell.

**What follows for the solver.** A sea does not fill to its level. Between the waterline and h_in
the bed is the waves' profile; between h_in and h_out sand is moved in storms and comes to rest
gently; below h_out the sea keeps what it is given. So the level to which a sea's bed may be built
by its rivers is the level less a wave base, and what arrives above it is the waves' to move.

### The beach face

The face the swash runs on is a plane, steeper for coarser grains and flatter for a more exposed
shore (Bascom 1951: not opened; the trend is restated by every later page that cites it, and by a
2022 data paper and a 2021 study of gravelly New England beaches through search summaries). The
physics is percolation: the uprush loses water into a coarse bed, the backwash returns weaker, and
the face steepens until gravity makes up the difference (the Coastal Wiki's gravel page, opened,
describes the mechanism for shingle).

**The numbers that were opened**: a gravel face stands at about 1 in 8 (tan β 0.125), with grains
resting at 35° in air and about 30° in water (the Coastal Wiki's gravel page). **The numbers that
were not**: the compilation the brief names, Bujan, Cox and Masselink 2019, was reached as its abstract
only (an index's record of it, opened): 2,144 measurements from very fine sand to boulders; the
face steepens with the grain up to cobbles, steeply among fine sediments and gently among coarse,
with a plateau near very coarse sand, where a beach becomes one that infiltration rules; boulders
may fall off again (preliminary); and the fit has the form tan β = a (D50 − 0.125)^b + c. **The
abstract does not give a, b and c.** From memory they are

    tan β = 0.268 − 0.154 (D50 − 0.125)^(−0.145)        (D50 in mm; the coefficients from memory)

which gives 0.044 at 0.2 mm (2.5°), 0.070 at 0.3 mm (4.0°), 0.091 at 0.5 mm, 0.11 at 1 mm (6.3°),
0.16 at 10 mm (9°) and 0.18 at 50 mm (10°). Those agree with what could be checked beside them: the
gravel page's 1 in 8, the proof of concept's 0.075 for 0.3 mm in a sheltered bay, and the reels'
catalogue (5 to 8° on coral sand, 3 to 5° on a long fine-sand beach, 10 to 15° at the step of a
steep one). Sunamura 1984's form, in which the slope also falls with the breaker's height over
√(g D) T (so exposure flattens a beach of a given grain), was not opened.

### The berm, the step, bars, and the beach's state

- **The berm** is the flat the constructive swash builds at the top of its reach: its crest stands
  at high water plus the run-up of the fair-weather wave. The engine already carries a run-up law,
  Stockdon et al. 2006's R₂ from the wave's height and length and the face's slope
  ([the shore page](shore.md#the-run-up)), and that is what sets the crest here. Takeda and
  Sunamura's berm-height formula (memory: 0.125 H_b^(5/8) (g T²)^(3/8), 1.4 m for a 1 m breaker at
  8 s) was searched for and not found; it is not used. The reels' berms are about 1 m on a coral
  islet; the proof of concept's hand berm was 2 m.
- **The beach's state** follows the dimensionless fall speed Ω = H_b / (w T) (Wright and Short
  1984, through search summaries of several reviews and a textbook page: **summary**): under about
  1 a beach is **reflective** (a steep face, a step at its foot, cusps, no bar, waves that surge or
  plunge on the face); over about 6 **dissipative** (a wide flat surf zone, several bars, spilling
  waves); between them **intermediate**, with bars and rips in four named arrangements. The model
  was fit to micro-tidal sandy beaches of New South Wales.
- **The step** is the coarse-grained drop at the foot of a reflective beach's swash, where the
  backwash meets the breaker (the Coastal Wiki's gravel page names "a (submerged) step at the wave
  break point", opened, with no height). Its height is of the order of the breaker's (memory).
  No opened source gave a formula.
- **Bars**: where and how tall. No opened source gave either beyond "the upper shoreface is often
  undulated by one or more bars" (the Coastal Wiki). A bar stands near the break point of the waves
  that built it (general knowledge). The design leaves bars to a later row for that reason.

### Cusps

Evenly spaced horns and bays along the face, **less than a metre to 50 to 100 m apart**, usually on
steeper, coarser beaches under waves arriving square-on, and wiped out by oblique waves' longshore
current (the Coastal Wiki's cusps page, opened). Two accounts: standing edge waves, whose length
sets the spacing (subharmonic, at half the incident frequency, or synchronous; from memory the
spacings are g T² tan β / π and half that), and self-organisation by the swash itself (Werner and
Fink 1993; Coco and others), in which **the spacing is proportional to the swash's horizontal
excursion**. The page gives the proportionality without its factor; a search summary of a 2021
paper gives about 1.6, and the factor recalled from Werner and Fink is about 1.7. The page's
appendix gives Dean and Maurmeyer 1980's geometry, spacing over excursion 4√ε with ε from 0.07 to
0.29, so **1.06 to 2.15**. The design takes spacing = 1.6 times the excursion, the excursion the
run-up over the face's slope: a 6 s wave running 0.8 m up a face of 0.12 goes 6.7 m and spaces its
cusps 11 m apart.

### Shingle, boulders and storm ridges

A shingle beach (4 to 256 mm) is a reflective beach taken further: a face near 1 in 8, a single
plunging breaker on the step, a swash zone as wide as the surf zone, reflection coefficients up to
about 0.2, porosity 0.25 to 0.4, half a metre of profile change in one tide under moderate waves,
and a near-vertical berm toward high water (the Coastal Wiki's gravel page, opened). Its crest is
built by storms to the storm's run-up and moved landward by overwash (rollover); the extreme case
it cites, Chesil Beach, stands 11 m above mean sea level and 160 m wide. Four kinds are told
apart: pure gravel, mixed sand and gravel, a gravel upper beach over a sand terrace, and sand.
**Boulder beaches** (over 256 mm): no source on their slopes or on the wave that moves a block was
opened. What the design needs of them is weaker, and is general knowledge: a block larger than the
waves of the climate can move stays where the cliff or the weathered mantle left it, so a boulder
shore is a lag, placed by where its source was and not by transport.

## Where sediment goes

### Along the shore

Waves that break at an angle drive sand along the beach. The CERC relation, with the breaker's
significant height H_b, its angle α_b to the shore, and the breaker index γ:

    Q = (K / (16 (s − 1))) √(g / γ) H_b^2.5 sin 2α_b

K = 0.39 for significant heights (the Coastal Engineering Manual's value, through the Coastal
Wiki's littoral-drift page, opened; Komar and Inman 1970's field data are its origin), s = 2.65,
and the bed's porosity 0.4 enters when a volume of sand is turned into a shoreline's movement. The
same page gives Kamphuis 1991 and van Rijn 2014, which add the grain and the slope, and the form
this design uses, **Ashton and Murray 2006's**, in which refraction and shoaling over parallel
contours are already carried from deep water to the break:

    Q = Q₁ cos^1.2(α) sin(α),    Q₁ = a₁ 2^−0.4 π^−0.2 γ^−0.4 g^0.6 T^0.2 H_s^2.4,    a₁ = K / 26

α the deep-water angle between the crests and the shore. Computed here: Q₁ is 0.059 m³/s for a 1 m
sea at 8 s, 1.9 million cubic metres a year, and the transport peaks near 42° at 0.47 of it. So a
metre of wave moves **hundreds of thousands of cubic metres a year** along an open beach.

**What follows.**

- Sand leaves a headland, whose two flanks face the waves at opposite angles, and gathers where the
  shore turns to face them squarely: a bay.
- A shoreline whose transport rises with its angle (under about 45° at the break, 42° in deep
  water) smooths itself out, and **a beach reaches its equilibrium plan in years**: the shoreline's
  diffusivity is of the order of Q₁ over the active profile's height, 10⁵ m²/yr, so a 1 km bay
  relaxes in about ten years. Against a stand of the sea that lasts thousands of years, the sand is
  always at equilibrium. The design leans on this.
- Above that angle the shoreline is unstable, and bumps grow into flying spits and capes (Ashton,
  Murray and Arnoult 2001, through the same page).
- The one-line model (Pelnard-Considère 1956, same page): the whole profile between the berm and
  the closure depth moves in and out as one, Δy/Δt = −(1 / ((D_B + D_C)(1 − p))) ΔQ/Δx.

### The plan of a pocket beach

Between two headlands with nothing passing them, a beach comes to rest where the net transport is
nothing at every point: the shoreline is square to the waves that reach it, so it is straight
where the waves arrive unbent and curls in behind the headland the waves bend round. Hsu and Evans
1989 fit the curve from 27 beaches, model and real (the form through the IH Cantabria model
documentation, opened; the count through a search summary):

    R / R₀ = C₀ + C₁ (β / θ) + C₂ (β / θ)²

R₀ the control line from the headland's tip, where the waves diffract, to the down-coast end of
the bay; β the angle between that line and the crests arriving at the tip; R the distance from the
tip to the shore at angle θ from the crest line; and the three coefficients tabulated against β,
so that R = R₀ at θ = β, which makes the three sum to one. **The table was not found**: it is
printed for β from 20° to 80° in steps of 2° in Silvester and Hsu 1993 and as a figure in the
Coastal Engineering Manual's Part III, chapter 2 (search summaries); the technical note and the
manual's pages refused the fetch, and the paper is not open. So the design's check of a pocket beach is stated first by the
condition the parabola is the fit of, zero net transport, and by the parabola once the build row
has the table. González and Medina 2001 add where the straight part begins (the same
documentation).

### Spits, salients and tombolos

An island or a reef shelters the shore behind it; the waves bend in from both sides, the
transport from both sides converges, and the shore grows a **salient** toward the island, or
reaches it as a **tombolo**. The rule is the island's length L along the shore over its distance D
offshore:

| Source | Tombolo | Salient | Little or nothing |
|---|---|---|---|
| Sunamura and Mizuno 1987, 23 natural islands in Japan (a page that annotates the paper, opened; the paper's own file held no text) | L/D > 0.67 | 0.3 to 0.67 | under 0.3 |
| Single breakwaters, the Coastal Wiki (opened), "rules of thumb" | L/D > 0.9 to 1.0 | under 0.6 to 0.7 | — |
| van Rijn 2013, several breakwaters (the annotating page) | L/D > 2 | 0.5 to 2 | under 0.2 |

The spread is the rule's honesty: supply, wave direction and tide move it. A **spit** is the same
transport running past the end of the land into deeper or sheltered water; none of the opened
pages gives a rule for its length beyond the high-angle instability above.

### Deltas

A river mouth on a wave-worked coast is a contest between what the river brings and what the waves
can carry away (Nienhuis, Ashton and Giosan 2015, through the Coastal Wiki's page on wave-dominated
deltas, opened):

    R = Q_river / Q_wave,    Q_wave = 90 H_s^(12/5) T_p^(1/5) kg/s

Q_river the river's sand. R over 1 and the river wins: a delta that projects, with several
channels. Under 1 the waves win: one channel, a smooth cuspate bulge whose flanks steepen toward
45° as R nears 1, beach ridges behind an advancing shore, and at R near 0 no bulge at all. Computed
here: Q_wave is 136 kg/s for a 1 m sea at 8 s, 1.6 million cubic metres of grains a year, and still
74,000 m³/yr for a sheltered bay's 0.3 m at 3 s; a stream draining half a square kilometre at a
millimetre a year brings 500. **Every stream of a small island is wave-dominated wherever there
are waves.** A lake is the other case: a 0.1 m wavelet at 1.2 s carries 4,400 m³/yr, so its streams
build deltas with a top at the water's level, a front at the angle of repose and a bottom of fines
(Gilbert's form, general knowledge), which is what the solver's lake already does.

## Rock coasts

### Retreat as force against resistance

Sunamura's account (his 2015 review in the Proceedings of the Japan Academy, opened: the first
100,000 of its 164,000 characters, through section 7; the 1992 book it summarises is not open): a
cliff's foot retreats at a rate set by the waves' assailing force F_W against the rock's resisting
force F_R, and not at all below a threshold,

    dX/dt = K ( ρ g H_f / S_c − ε )    where it is positive, else 0

with F_W = A ρ g H_f, H_f the height of the waves just in front of the cliff, and F_R = B S_c, S_c
the rock's uniaxial compressive strength; K = A C / B and ε = B / A. (An earlier form took the
logarithm of F_W / F_R; the review uses the linear one. Its laboratory constants, C about
3.9 × 10⁻² cm/h for breaking and for broken waves, are a flume's and do not transfer.) Two things
make it a model and not a rate:

- **The wave at the foot is not the wave offshore.** Waves break where the depth is 1.25 times
  their height (H_b / h_b = 0.8) and decay across whatever lies between the break and the cliff:
  H_f / H_b = exp(−35.2 l tan β / (T √(g h_b))), l the distance and tan β the slope (the review's
  equation 13). Computed here: a 100 m platform at 1 in 50 cuts a 2 m, 8 s breaker to a sixth. So
  **retreat widens the platform, and the platform slows the retreat**, toward a width at which
  the wave at the foot falls to the threshold.
- **Rates span four orders of magnitude.** Soft-rock cliffs go back 10⁻² to 10⁰ m a year (the
  review: 0.5 m/yr on Long Island over 80 years, 0.2 at Santa Cruz, 1.0 at Dunwich; and 12 m in a
  night in a surge). The newest compilation, Prémaillon and others 2018's GlobR2C2 of 1,530 cliffs
  (opened: the first 100,000 of 114,000 characters), gives **medians of 2.9 cm/yr for hard rock
  (granite, gneiss, limestone), about 10 cm/yr for medium and 23 cm/yr for weak (chalk, poorly
  consolidated rock)**, by Hoek and Brown's rock-mass classes, with scatter as large as the median
  in every class; rock resistance is the strongest signal in it, and waves show no clear effect in
  the compiled data (a weak peak at tidal ranges of 1 to 3 m). Measured cliffs are the ones that
  move, so these medians are an upper reading of a hard coast (this page's inference, not the
  paper's).

### The beach at the cliff's foot: a tool, then a cover

A little sediment at the foot is ammunition; a lot is armour (Sunamura's review, opened). His fit
to a Lake Erie bluff (Carter and Guy's data) has the erosion coefficient rising from a bare foot
to a peak at a beach 1.25 m wide and falling to nothing at 6 m; on the open Pacific at Oceanside
erosion fell markedly past 20 m of beach and ceased at 60 m; at Newhaven 70 m were needed to keep
storm waves off the chalk. Those widths scale with the waves: a cliff is safe when the beach in
front of it is wide enough that the storm's run-up ends on sand. Limber and Murray 2011 built a
plan-view model on this one nonlinearity and found that **headlands and pocket beaches arise and
persist on a coast of uniform rock**: a bay that gathers sand stops retreating, a bare headland
goes on until the sand reaches it (the abstract through search summaries; the paper not opened).
Their 2014 pair adds the second feedback, wave energy converging on headlands and spreading in
bays, and finds a headland's seaward reach inversely proportional to the sand supply (abstracts,
**summary**).

### Platforms, of two kinds, and plunging cliffs

What retreat leaves is a rock floor, the shore platform (the Coastal Wiki's rocky-shore page and
Sunamura's review, both opened):

- **Sloping** (Sunamura's Type A): a surface descending from the cliff's foot under the sea at
  about **1° to 5°**, tens to many hundreds of metres wide; with large tides and strong waves, and
  in soft rock (compressive strength up to about 5 MPa).
- **Level, with a seaward drop** (Type B): a horizontal or nearly horizontal surface a few metres
  to hundreds wide ending in a scarp; on small tides and in more resistant rock. Its cliffs
  retreat one to two orders of magnitude more slowly than Type A's. It may stand at low tide,
  across the tide, or metres above high tide: high platforms on hard rock with low waves, low
  ones in limestone, where the surface also hardens.
- **Plunging cliffs**: a face that goes straight down into water deep enough that the waves
  reflect from it unbroken, and no appreciable retreat (the review); found where a steep inherited
  slope was drowned.
- **What decides.** The gradient rises with the tidal range, because a larger tide spreads the
  waves' work over a taller band; it falls with wave height and with weaker rock; the tide is the
  strongest single predictor in a LiDAR survey of 700 km of south-west England (search summaries of
  Trenhaile's work and of that survey: **summary**; no regression was opened). Sunamura's own
  condition between A and B is a comparison of the wave's force with the rock's strength at the
  platform's seaward edge; its equation was not reached (the thesis that prints it is garbled).
  Platforms also wear down, hundredths of a millimetre to a millimetre a year, by wetting and
  drying, salt and grazing (the Coastal Wiki).

### Notches, stacks, arches and geos

- **The notch** is cut at the waterline, where the waves work longest, and the cliff above falls
  when it is deep enough. Kogure and others measured notches up to 8.8 m deep under limestone
  cliffs 3 to 22 m tall in Okinawa and modelled the fall as a cantilever's; a follow-up found
  collapse when the notch reaches about 0.5 to 0.9 of the block's seaward length (search
  summaries: **summary**). Its height follows exposure, tide and structure (Sunamura's review,
  opened, with no formula).
- **Stacks, arches, caves and geos**: none of the opened pages treats them. What they say that
  bears on them: waves quarry rock along its joints, faults and weak beds, and gullies are scoured
  along them (the Coastal Wiki). The rest is general knowledge: the sea enters a weak line as a
  narrow inlet (a geo) or a cave, two caves meeting through a headland are an arch, an arch whose
  roof has fallen leaves a stack, and a stack is a piece of harder or less jointed rock the
  retreat went round. **No density, spacing or size rule was found**; the design derives the ones
  large enough to see on its lattice from the rock's own field and says which numbers are
  authored.

### Refraction: headlands and bays

A wave slows as the water shallows, so its crests turn toward the contours of the bed (Snell's
law on the celerity), and the energy between two rays is conserved: where rays converge, over the
shoal in front of a headland, the wave grows, and where they spread, into a bay, it shrinks. S102's
grounding page states the relations; the engine's dispersion and spectra are on
[the ocean-surface page](ocean-surface.md#dispersion). On a rocky coast it is the second negative
feedback: the further a headland stands out, the more energy it draws (Limber and others 2014,
summary; the Coastal Wiki, opened, says refraction toward headlands limits a coast's indentation).

## A lake's shore, at its own scale

A lake's waves are its own wind's over its own fetch ([the wave a wind raises](shore.md#the-wave-a-wind-raises)):
5 m/s over 2 km raises 0.13 m at 1.3 s; a gale of 15 m/s over 4 km about 0.5 m at 2.4 s (computed
here from the same fetch laws). Everything above then applies with those numbers. What a lake
shows:

- **A wave-cut bench and a wave-built terrace**: a shelf round the exposed shores, cut in the bank
  at the landward side and built of what was cut at the lakeward side, whose **outer edge stands
  at the depth where storm waves stop stirring the bed**. An early survey of south-eastern
  Wisconsin's lakes says so in those terms and gives Lake Mendota's shelf as 15 to 20 ft deep at
  its edge and from a few rods to nearly half a mile wide (a search summary of the digitised
  survey: **summary**, its title not confirmed). Half the gale's wavelength above is 4.6 m, which
  is 15 ft: the rule and the lake agree.
- **Mud below, sand and stones above.** The depth below which a lake keeps mud is predictable
  from wave theory, and it is the few largest storms of a year that set it, not rare extremes
  (Rowan, Kalff and Rasmussen 1992, abstract through search; a later paper quotes its fit as
  2.685 E^0.305 metres with E an exposure in km²: **summary**, unverified). Fine sediment also
  slides off slopes over 4 to 5%. So a lake's exposed shore is a lag of cobbles on a bench, its
  lee shore is mud to the waterline, and that is where the reeds are.
- **No tide**, so everything happens in a band a wave's height tall; **deltas** at every inflow.

## What the reels' five shores are

From the second set's catalogue and sheets (`inspiration2/_frames/motion`, opened), in the terms
above. The numbers are what a sketch would set to make each; none is measured from the footage
beyond the slopes the catalogue estimates.

| Shot | What it is | The numbers that make it |
|---|---|---|
| `swash-coral-sand` | A low reef island's lagoon beach. The reef takes the swell; what reaches the sand is 0.2 to 0.4 m. Coarse carbonate sand with coral gravel, so a steep reflective face on which a small wave surges as a thin sheet: the lace | Waves 0.3 m at 6 to 10 s; grain 0.5 to 2 mm, kind carbonate, supplied from the reef and not by rivers; face tan β 0.09 to 0.14 (5 to 8°); Ω about 0.4; berm about 1 m; tide under 1 m; A about 0.2, so the lagoon's floor of 2 to 3 m is 30 to 60 m out |
| `shorebreak-plunging` | A reflective beach of coarse sand open to a short-period sea: deep water close in, a step at the swash's foot, and the wave breaking once, on the step | Waves 1 m at 6 s; grain 0.5 to 1 mm; face 0.10 to 0.20 at the step (catalogue: 10 to 15°); Iribarren number about 1.1, plunging; Ω about 1.7; a step 0.3 to 1 m; no bar |
| `surge-on-rock-shelf` | A low level bench (Type B) in limestone or cemented beach sand on a coast of almost no tide: a shelf 0.5 to 1 m above the water, pitted, broken into slabs at its edge, with sand behind. The surf sheets over it because its top is inside the run-up | Tide 0.2 to 0.4 m; resistance in the middle of the scale; platform level 0.3 to 1 m above the sea, 5 to 30 m wide, a seaward drop of 1 to 2 m; waves 0.5 to 1 m at 6 to 8 s; a sand beach landward of it |
| `swash-round-boulder` | A pocket beach of medium sand between headlands that are piles of granite corestones: boulders 2 to 5 m across, rounded by weathering in the ground and not by the sea, far larger than these waves move. The sea stripped the weathered mantle and left them | Grain 0.3 to 0.5 mm, face 0.07 to 0.09 (catalogue: about 5°); headlands 100 to 300 m apart with zero net transport between them; a boulder lag where a deep weathered mantle met the sea; waves 0.5 m |
| `chop-on-riprap` | A gravel pocket at the foot of a wooded bank on fetch-limited water: the bank's own stones as a steep lag beach, short chop reflecting from it, drift logs at high water | Wind sea 0.2 to 0.4 m at 2 to 3 s over 5 to 20 km; grain 10 to 30 mm, face 0.13 to 0.18 (catalogue: 10 to 15°); the source a bank of weak, stony ground retreating; a tide of metres, so a sand terrace below the gravel |

Corestones as the origin of the granite shore's boulders is the author's reading of the frames
(fluted, rounded blocks piled in place, as on the granitic Seychelles) and of general
geomorphology; no source was opened for it.

## The rules, as one table

| What | Rule | Constants | Source, and how far read |
|---|---|---|---|
| Shoreface | h = A x^(2/3) | A 0.079 to 0.398 m^(1/3) | Bruun 1954, Dean 1977 via the Coastal Wiki and Bosboom and Stive: opened |
| A from the grain | A = 0.5 w^0.44, w in m/s | 0.096, 0.122, 0.156, 0.20 at 0.2, 0.3, 0.5, 1 mm | Dean 1987 via the same: opened. Moore 1982: summary |
| Fall speed | w = R g D² / (18 ν + √(0.75 R g D³)) | C₁ 18, C₂ 1 | Ferguson and Church 2004: summary |
| Closure, inner | 2.28 H − 68.5 H²/(g T²); 1.75, 57.9; 6.75 H̄ | H exceeded 12 h a year | Hallermeier, Birkemeier, Houston via the Coastal Wiki: opened |
| Closure, outer | 0.018 H_s T_s √(g / (d50 (s − 1))) | 15 to 40 m | Hallermeier 1983 via the same: opened; its example disagrees with the formula |
| Beach face | steeper with grain, flatter with exposure; gravel 1 in 8 | tan β = 0.268 − 0.154 (D − 0.125)^−0.145 | Gravel: Coastal Wiki, opened. The fit: Bujan and others 2019, its form from the abstract (opened), its coefficients **memory**. Bascom 1951, Sunamura 1984: not opened |
| Berm | high water + run-up of the fair-weather wave | Stockdon's R₂ | [shore.md](shore.md#the-run-up) |
| Beach state | Ω = H_b / (w T) | reflective under 1, dissipative over 6 | Wright and Short 1984: summary |
| Cusps | spacing = f × swash excursion | f 1.6 (1.06 to 2.15) | Coastal Wiki (Dean and Maurmeyer; Werner and Fink): opened; f: summary |
| Longshore transport | Q = Q₁ cos^1.2 α sin α | K 0.39; peak near 42° | CERC, Ashton and Murray 2006 via the Coastal Wiki: opened |
| One-line model | Δy/Δt = −ΔQ/Δx / ((D_B + D_C)(1 − p)) | p 0.4 | Pelnard-Considère 1956 via the same: opened |
| Pocket beach | zero net transport; R/R₀ = C₀ + C₁(β/θ) + C₂(β/θ)² | C's tabulated against β: **not found** | Hsu and Evans 1989 via IH Cantabria's documentation: opened |
| Tombolo | island length over distance offshore | over 0.67 (to 1.0); salient 0.3 to 0.67 | Sunamura and Mizuno 1987 via an annotating page; Coastal Wiki: opened |
| Delta | R = Q_river / Q_wave, Q_wave = 90 H^(12/5) T^(1/5) kg/s | wave-dominated under 1 | Nienhuis and others 2015 via the Coastal Wiki: opened |
| Cliff retreat | dX/dt = K (ρ g H_f / S_c − ε), 0 below the threshold | medians 2.9, 10, 23 cm/yr by rock class | Sunamura 2015 (opened, to section 7); Prémaillon and others 2018 (opened, most) |
| Wave at the foot | breaks at h = 1.25 H; H_f/H_b = exp(−35.2 l tan β / (T √(g h_b))) | — | Sunamura 2015: opened |
| Beach as tool and cover | rate up to a narrow beach, then down to nothing at a wide one | 1.25 m and 6 m (a lake); 20 to 70 m (open coasts) | Sunamura 2015: opened. Limber and Murray 2011: summary |
| Platforms | sloping 1° to 5° (large tide, soft rock); level with a drop (small tide, hard rock); plunging: no retreat | soft is S_c up to about 5 MPa | Coastal Wiki, Sunamura 2015: opened. Gradient against tide: summary |
| Notch | at the waterline; the cliff falls at a depth | up to 8.8 m measured; 0.5 to 0.9 of the block | Kogure and others: summary |
| Lake bench | outer edge where storm waves stop stirring the bed | half the storm wavelength; Mendota 15 to 20 ft | an early Wisconsin survey; Rowan and others 1992: summary |

## The design

One sentence: **the solver's sea fills only to its wave base during the long run, and after the
last step the solver runs a stand of the sea, a few thousand years in steps of its own, in which
each shore cell's exposure to a wave climate drives a cliff line back through the rock as a level
set, leaves a platform at the tide's level, and drapes the sand the cliffs and rivers supplied at
its equilibrium plan and profile, written as bathymetry to the closure depth and as fields the
water, the masks, the scatter and the cliffs read.**

### A stage of its own, and one rule inside the step: why both

**The sea filling is a fault of the step, so its cure is in the step.** The fill happens over the
million years of the run, one step's load at a time. One rule changes: a point under the sea has
room for sediment only up to the sea's level less the wave base
([the long run](#the-long-run-the-sea-fills-to-its-wave-base-and-no-higher)).

**Everything else is a stand, after the last step.** Three reasons, each sufficient:

1. **The times do not meet.** A beach reaches its plan in years, a cliff moves metres to hundreds
   of metres in a thousand years, and the solver's step is thousands of years: a weak cliff at the
   median 23 cm a year would cross 290 cells of a 4 m lattice in one 5,000-year step. A process three orders faster than the
   step cannot be a term of the step.
2. **A coast is young.** The sea has stood near its present level for about six thousand years,
   and through the ice ages it stood far lower (general knowledge; no source opened). What a
   coast shows is that one stand's work on ground the rivers and the ice made at other levels.
   Running retreat for the whole million years at the median rate would remove 29 km of hard rock,
   every island in any sketch.
3. **It keeps the landscape's solve whole.** A sketch with no sea solves to the bytes it solved to;
   a sketch with a sea changes by the wave-base rule alone until its `coast` block asks for a stand.

So the solver gains a **stand**: `coast.stand_yr` years (default 6,000) in steps of
`coast.step_yr` (default 100) after the last landscape step and its settling, before the fields
are routed from the final millimetres. The rivers do not move during it; their loads at their
mouths are the last step's.

### Inputs

**What the sketch gains.**

| Field | What | Range, default |
|---|---|---|
| `sea.wave_base_m` | The depth below the level to which the sea's bed may be built | 0 to 200 m; absent: Hallermeier's outer depth of the climate's mean sea, or 20 m with no climate |
| `sea.tide_m` | The spring tidal range (a water body's own `tide_m` is an amplitude, half of this; the two must be set together) | 0 to 15 m, default 0 |
| `sea.stands` | The level's history, a year and a level each, in order, as the ice's `stands` are; the wave-base rule follows it, and the coast's stand runs at the last | none |
| `coast.stand_yr`, `coast.step_yr` | The stand and its step | 0 (no coast) to 20,000; 100 |
| `coast.waves` | The rose of what arrives from the open sea: entries of `from_deg`, `height_m`, `period_s` and `share` of the year. It enters through `Sea` edges only | none |
| `coast.wind` | The rose of local wind: `from_deg`, `speed_m_s`, `share`. It raises a sea over the lattice's own fetch, on the sea and on every lake | none |
| `coast.storm` | The height exceeded twelve hours a year over the mean height, for the closure depth and the berm | 1 to 6, default 3 |
| `coast.sand` | The default sediment: `grain_mm`, `kind` (quartz, carbonate), and `river_share`, the part of a river's load that is sand or coarser | 0.3 mm; quartz; 0.3 |
| `coast.sources` | Supplies that are not a river or a cliff: a polyline or a stretch of an edge, a `rate_m3_per_yr`, a `grain_mm` and a `kind` (a reef's carbonate sand; drift entering at the lattice's edge; sand the rising sea swept onshore) | none |
| `coast.cliff` | `rate_m_per_yr` at resistance 0, 0.5 and 1 under the reference wave of 1 m at the foot; `yield`, the share of what a cliff loses that stays on the shore as sand or coarser; `grain_mm` of that yield at resistance 0 and 1 | 0.25, 0.03 and 0.003: authored inside the published span (GlobR2C2's medians, 23 cm for weak rock and 2.9 cm for hard; hard rock typically under a centimetre a year, the Coastal Wiki after Sunamura 1992); 0.3; 0.3 mm and 200 mm |
| `coast.outline` | Closed polylines of land, a plan: the uplift's footprint and the starting sea bed are made from them | none |

**What is already there**: the rock's resistance at every point, uniform or from the formation
([structure](../subsystems/landform.md#structure)); the rivers' sediment flux at their mouths; the
sediment layer and the rock floor beneath it; every lake, by its fill.

**The outline** answers the authors' fifth gap (a plan is only an uplift raster). An outline
becomes two things the authors made by hand: the uplift's footprint, rising from nothing at the
line to the sketch's rate a reach inside it, and the starting bed outside it, the equilibrium
shoreface for the sketch's grain out to the closure depth and a shelf beyond, falling to
`sea.depth_m` over `sea.shelf_m`. It needs no stand and is stage A's.

### The long run: the sea fills to its wave base and no higher

In [the step](../subsystems/landform.md#the-step)'s rivers, the sea's rule becomes:

- A point under the sea has room up to `level − wave_base_m`. What it has no room for goes on
  along the bed to deeper water, as it does today, and what reaches a `Sea` edge leaves. The
  budget gains one line, `shelf_m3`, what settled under the wave base; `left` is as before.
- Above the wave base nothing settles from a river in the long run, **except** at a mouth where
  the river beats the waves: R = Q_river / Q_wave over 1, with the sketch's climate at that mouth
  (no climate: every mouth is the river's, and the sea behaves as a lake). There the mouth builds
  a delta to the level exactly as a lake's does (`delta_share`). A lake is unchanged.
- **Deposition stays on for the land**, at its default: fans, valley floors and lake fills are as
  they are today. The island that grew a plain keeps its footprint.

Why the wave base and not the closure depth: the long run's step cannot see a beach, only whether
the bed is still the waves' to move. The beach is the stand's.

### The stand

Once, at its start: the cells of the sea are those under the level that connect to a `Sea` edge
(a hollow inland of the coast below the level is not sea); each lake is a body of its own with the
wind's sea and no swell. Then each step of `coast.step_yr`:

**1. Exposure.** For every shore cell and each of 32 azimuths, a walk across the lattice's water
until land or an edge. An azimuth that reaches a `Sea` edge is open to the rose's swell from that
direction; one that ends on land has a fetch, and the wind's entries raise their sea over it by
the fetch laws the water already uses. Each open azimuth's wave is carried to its break in closed
form (Snell's law and the flux over contours taken parallel to the local shore, which is what
Ashton and Murray's form contains), and the cell keeps the energy-weighted breaker height, its
direction, and the storm's height. The shore's own direction at a cell is the gradient of the
distance field below, not the lattice's staircase. **Sheltering is exact; focusing is not**: this
gives a headland its square-on waves and a bay its oblique and shadowed ones, and misses the
growth of a wave over a shoal. A later stage replaces the closed form by a wave field marched over
the lattice's bathymetry ([what is shared with S102](#the-same-physics-as-the-waters-wave-field-what-is-shared)).

**2. The cliff line retreats.** The line between rock that stands and rock the sea has cut is the
zero of a distance function kept on the lattice. Each step it moves landward at the local speed

    v = rate(r) × g(W) × max(0, H_f / H_ref − ε)

`rate(r)` the sketch's three rates read log-linearly in the resistance r at the surface (the
formation's bed there: a hard bed is a headland), H_f the storm wave at the foot (the smaller of
the depth-limited height at the foot at high tide and the breaker decayed across the platform by
Sunamura's exponential), ε the threshold, and g(W) the beach's tool-and-cover factor for the
width W of sand in front: rising from 1 on bare rock to a peak at a narrow beach, falling to 0
where the beach is wide enough that the storm's run-up ends on it. The form is Sunamura's linear
law; its scale is authored through the three rates because his constants are a flume's. The
advance is the upwind step of the eikonal equation, the scheme
[the slope limiter](../subsystems/landform.md#the-slope-limiter) already uses, sub-stepped so the
line moves at most half a cell at a time. So the line is known to a fraction of a cell, moves the
same distance at any spacing, and has no eight-sided bias.

- **Where the foot is too deep for the storm wave to break** (deeper than 1.25 H), H_f is the
  unbroken wave's and falls under the threshold: a **plunging cliff**, and no retreat.
- **What the line has passed is platform.** Its rock floor is lowered to the platform's surface:
  from `tide_m / 2` above the level at the foot, seaward at a gradient that rises with the tide
  (an authored line through the published 1° to 5°: 0.01 + 0.006 per metre of tide, held to
  0.09), down to where the surface meets the old bed. With a tide under 2 m in rock of resistance
  over 0.5 it is **level with a seaward drop**: flat at mid-tide to its edge, then a scarp to the
  storm wave's breaking depth. The two numbers that pick the kind are authored inside what the
  sources say (small tide and hard rock: level); the regression was not opened.
- **The cliff is as tall as the land the line retreated into**, which is the hand rule the
  authors found. Its cells are marked, and **the limiter leaves a marked cliff standing**: the
  solver's invariant becomes "no pair of neighbours stands steeper than the critical slope except
  across a marked sea cliff or a platform's seaward drop".
- **What the cliff lost** (its height times its retreat, per metre of shore) goes to the budget:
  `yield` of it as beach sediment at the grain its resistance gives, the rest as fines, offshore.
- **Stacks**: a cell of harder rock (the formation's bed, or the seeded variation the sketch
  already has) that the line passed on both sides stands on the platform. Those of a cell or
  more are in the heightfield; smaller ones are sites in the features file.

**3. The budget.** Each stretch of shore between two points sand cannot pass (a headland whose
foot is deeper than the inner closure depth, a lattice edge that is `Closed`) is a littoral cell
with a volume of sand: what it held, plus the step's supply from its cliffs, its rivers' mouths
(`river_share` of each mouth's load) and the sketch's sources, less what leaves past a headland
that is not deep enough to stop it or through an open edge. Sand of different grains in one cell
mixes to a volume-weighted grain.

**4. The sand comes to rest.** Because a beach reaches its plan in years and the step is a
century, the sand is not stepped: it is placed at equilibrium.

- **In plan**: within a cell with nothing passing, the waterline of the sand is the curve that is
  square, at every point, to the energy-weighted direction of the waves that reach that point
  (zero net transport by the rose and the shadows of step 1). Within a cell with sand passing
  through, it is the curve along which the transport is the throughput. The curve is found by
  integrating that direction field from the cell's down-drift end, and its offset from the rock by
  the one number that makes the volume under the profile equal the cell's volume, found by
  bisection.
- **In section**: from the waterline seaward Dean's profile for the cell's grain, to the inner
  closure depth of the cell's own storm wave; landward the face at the slope for the grain and
  the exposure, to the berm at high water plus the run-up; then the berm, level, to the rock. The
  sand's surface at a point is that section read at the point's distance from the waterline; where
  the rock stands higher the rock shows, as a platform through the beach, a boulder, a headland.
- A cell with too little sand to reach its waterline has a beach only at its head; one with none
  is rock. A mouth whose river beats the waves keeps its delta.

**5. A lake's shores** are done once, at the stand's end, by the same steps with the wind's sea
alone: a bench cut in the exposed shores to the storm wave's closure depth and built out to half
its wavelength, what was cut laid at its outer edge; nothing on a lee shore.

**After the last step** the sand is written into the sediment layer, the marks and the fields are
taken, and the fields are routed from the final millimetres as today.

**What stops it.** The sea does not fill because nothing may settle above the wave base, and the
beach's volume is its budget and no more. A cliff stops at its own platform (the wave at its
foot falls to the threshold), behind its own beach, at deep water, or at the stand's end. The
budget closes: cliffs + rivers' sand + sources = beaches + offshore + out, to the nine digits the
landscape's budget holds.

### The same physics as the water's wave field: what is shared

S102 is building, over one body's depth map, a phase field from the eikonal with the depth's
celerity and a height field from the energy flux with shoaling, refraction and breaking. The
coast's exposure is that physics at another scale and for another purpose. **They share formulas
first, and one header of closed forms once S102 has merged; they do not share the field solver.**

- **Why not the solver**: the water's is built at scene load on a map of at most 1,024 texels a
  side at decimetres to metres, for one body, in the renderer's modules, to be drawn; the coast's
  runs offline on the solver's lattice at metres to tens of metres, sixty times in a stand as the
  shore moves, over a rose of directions, and every decision in it must be the same bytes on every
  toolchain. The landform is a capability and may not depend on the water, another capability; and
  it should not link the GPU's module for a formula.
- **What is shared**: the dispersion relation at a depth, the celerities, the shoaling coefficient,
  Snell's law, the breaker index, the fetch laws and Stockdon's run-up, as pure functions with no
  C library call in a decision (the dispersion relation solved by a rational
  approximation in place of a hyperbolic tangent: Hunt 1979's explicit solution, good to about
  0.2% at any depth by a later discussion's account, a search summary; its coefficients are known
  to this page only from memory and are to be taken from the paper). Until S102 merges the landform
  carries its own and `engine-content`, which links both, holds the two to each other in a test,
  as it holds the heightfield's two readers.
- **What must agree is the climate.** The coast is the equilibrium of one wave climate, and the
  water must draw that climate on it: the field file records the stand's rose, and a water body
  over a solved coast should take its swell's height, period and direction and its wind from
  there. That is a request to S102's next pass. A beach built for 1 m at 8 s under a sea drawn at
  3 m is the mismatch the owner's sentence rules out.
- **Later**: when the lattice's own wave field replaces the closed-form exposure (stage G), it is
  the same eikonal and flux march as the water's, written for the lattice, and the page of that
  stage says whether one implementation can then serve both.

### Determinism

The stand is held to [the solver's rule](../subsystems/landform.md#the-same-bytes-everywhere): the
same bytes from MSVC, GCC and Clang, at either baseline and any thread count.

- The cells of the shore are visited in the order of a contour trace started at the lowest cell
  index; the littoral cells are numbered along it.
- The azimuth walks are integer; the eikonal advance is the limiter's arithmetic; the bisection
  ends on a comparison of volumes summed in one order.
- The fractional powers (H^2.4, T^0.2, cos^1.2, w^0.44, x^(2/3)) are the only new arithmetic. Per
  rose entry and per grain they are constants computed once through `core/math/deterministic.h`'s
  exponential and logarithm; per cell they are tables over a quantized angle and a quantized
  distance, so no decision calls the C library. x^(2/3) for the profile is the cube root of a
  square: if `deterministic.h` has no correctly rounded cube root, the build row adds one or uses
  the table, and says which.
- The jobs run only what reads no neighbour's result: the azimuth walks of different shore cells,
  the profile's evaluation over a littoral cell. The line's advance and the budget are serial.

### Outputs, and who reads each

**The heightfield** holds the cliff as a step, the platform, the beach to its berm and the bed to
the closure depth and on down the shelf. **The field file** becomes version 5 with these, all zero
or none away from a shore, and the stand's climate, tide, closure depths and budget in its JSON:

| Field | Type | What | Who reads it |
|---|---|---|---|
| `shore_distance` | i32, mm | Distance from the still waterline, positive seaward, to 2 km | The refinement of the profile at a finer spacing (below); the masks (bands by distance and height); the wetland's and the aquatic plants' zones |
| `substrate` | u8 | none, sand, shingle, boulders, rock platform, sea cliff, mud | **The masks** (wet sand, dry sand, shingle, splash-zone rock: a request to the row that holds the terrain masks); **the scatter** (boulders in the swash, a stack's debris); **the water** (the bed's colour under shallow water; how fast the sand drains, S102's infiltration) |
| `grain` | u32, µm | D50 of the beach's sediment | **The water** (drain time and the face the swash meets); the masks' choice of sand set; ripples on a sandy bed |
| `exposure` | u16, mm | The energy-weighted breaker height at the shore; `exposure_storm` the storm's | **The water** (S102: the local bore's height along a shore, to check its own field against); the masks (how high the splash zone stands); the cliffs (clean rock against vegetated); the wetland (reeds only where it is small) |
| `wave_direction` | u8 | The energy-weighted direction the waves come from, in 256ths of a turn | The water; the scatter (which way drift and wrack are swept) |
| `beach_slope` | i32, ppm | The face's tan β | **The water** (S102's run-up and swash width); the refinement |
| `berm` | u32, mm | The berm's crest above the level, and `berm_line` (u8) marking the crest's cells | **The masks** (the wrack line is the last high water's line below it; dry sand above it); **the scatter** (wrack, driftwood and shells on the line; the storm ridge on shingle); the water (where the swash can ever reach) |
| `cliff_height` | u32, mm | On the cliff line's cells: the top less the foot | **The cliffs and ledges** (a sea cliff's face: its height, its foot at the platform's level, fresh rock); the deposits (no talus at a wave-washed foot; a boulder apron where the yield's grain is large) |
| `cliff_retreat` | u32, mm | How far the line came | The cliffs (a long retreat: a clean vertical face; none: the inherited slope) |
| `notch` | u16, mm | The undercut at the foot, by the rate and the rock | The cliffs' face mesh (the heightfield cannot hold an overhang) |
| `platform` | u8 | 0, sloping, level; with the level's height in the JSON by body | The masks (splash-zone and intertidal rock); the water (S102's rocky shore: a shelf the surf sheets over); the scatter (pools, weed) |

**A features file**, `<name>.coast.json`, for what is not a field: per littoral cell the beach's
record (volume, grain, A, face slope, berm, state Ω, and its cusps: spacing, relief and the point
their phase is counted from); per platform its kind, level, gradient and drop; stacks under a
cell's size (a point, a height, a width); geos (a line along a weak bed or a joint set of the
formation, a width, a depth), for the ground's cuts; arch and cave sites for whatever row builds
them; mouths with their R; salients and tombolos. Everything in it is a function of the sketch.

### Resolution: what the lattice holds, and what it cannot

A notch is a metre, a beach step a metre or two, a cusp ten, a beach face tens, a shoreface
hundreds. The lattice is 4 to 16 m.

- **The lattice holds** the waterline's place to a fraction of a cell (the distance field), the
  cliff line and the cliff's height as a step between two cells, the platform, the shoreface, the
  berm and the pocket beach's plan, stacks of a cell or more, the substrate and the exposure.
- **The lattice does not hold the profile's shape near the waterline**: a face 25 m wide is three
  cells at 8 m, and bilinear ground between them makes the waterline wander. So **the profile is
  re-evaluated, not interpolated**: the fields carry the section's parameters and the distance,
  and anything that builds ground at a finer spacing (a refine window, the tile set's build at
  0.25 to 1 m) computes the section there from those, exactly, wherever the substrate is sand or
  shingle. This is an edit of the ground in the pattern of the channels' cuts
  ([tileset](../subsystems/tileset.md)), and it must be in the ground the water samples for its
  depth map, or the wave follows one beach and the eye sees another. A request to the tile set's
  row.
- **The same edit adds** what is under the lattice and has a rule: cusps (the record's spacing and
  relief along the berm's line), the step at the swash's foot on a reflective beach, the berm's
  crest, the storm ridge on shingle, the platform's drop. **The cliffs' generator adds** the face,
  the notch, sub-cell stacks; **the cuts add** geos; arches and caves are meshes and another row's.
- **How steepness is kept off the lattice.** Every form here is set analytically from a distance
  and a physical parameter (a rate in metres a year along a line known to a fraction of a cell; a
  slope from a grain; a level from a tide), never from a difference between neighbours, so the
  same sketch at 4 m and at 8 m gives the same cliff line, the same cliff heights where the land
  behind is the same, and the same section. Transport and retreat are counted per metre of the
  smoothed shore, not per cell face (a staircase is up to √2 too long).
- **The land behind is not the same at two spacings**, and that is the solver's, not the
  coast's: the authors measured a summit at 212 m on 8 m and 235 m on 4 m. The likely cause, said
  here so its row can test it: on an unchannelled slope a cell collects its own width times the
  slope's length, so the area at which the rivers' term overtakes the critical slope is reached
  at a hillslope length that doubles when the spacing halves, and a finer lattice has more ground
  at the critical angle. The usual cure is to give the stream-power term the area per unit width
  times a fixed physical width on unchannelled ground (Pelletier 2010, from memory, not opened).
  It is a change to the landscape's step and a row of its own; this page's stages are checked at
  two spacings on a land that does not move (a starting ground and a stand alone).

### What it costs

Estimated, not measured, from [the solver's own tables](../subsystems/landform.md#performance-notes):
the solve holds about 250 bytes a point and spends about 1.4 µs a point a step, so 1,024 points a
side for 300 steps is about 460 s and the authors' island (1,025 by 1,281, 400 steps) took 200 s.

| Part | Work | On 1,024 a side with 8,000 shore cells |
|---|---|---|
| The wave-base rule, every step | one comparison a sea point in the step's books | under 1% of the books; nothing measurable |
| Exposure | shore cells × 32 azimuths × the walk (to an edge, or 256 cells over a coarse land mask) | 10 to 60 million reads a stand step; 0.05 to 0.2 s; 3 to 12 s a stand of 60 |
| The cliff line | the eikonal advance over a band a few cells wide, sub-stepped | under 1 s a stand |
| The distance field | fast sweeps over the coastal band (the shoreface's width each side: about a million points) | 0.05 to 0.1 s a step; 3 to 6 s a stand |
| The sand | per littoral cell a bisection of 20 volumes over its band | 0.05 to 0.1 s a step; 3 to 6 s a stand |
| A lake | the same once | under 1 s |
| **A stand** | | **10 to 25 s, 2 to 5% of a 460 s solve**, on one thread but the walks |

While it runs the stand holds about 30 bytes a point (the distance, the speed, the sand's surface,
the marks, the exposure), after the step's network (140 bytes a point) has been freed, so the
solve's peak does not rise. On disk the thirteen grids add 34 bytes a point, 45 MB for the
authors' island beside the 77 MB it writes today; nearly all of it zeros inland, which a later
packer could drop. Nothing is resident at run time and nothing scales with the world.

### Stages, in build order

Each stage is one row's work, ends usable, and names the sketch that shows it and the checks that
hold it. The three sketches: **an island with a headland and a bay** (the authors' island reduced:
a kilometre, banded rock, one exposed side), **a straight exposed coast** (a plain meeting the sea,
uniform), **a lake** (4 km long, wind along it).

| Stage | Builds | Returns to the eye | Sketch | Held by | Cost |
|---|---|---|---|---|---|
| **A** | The wave-base rule and its budget line; `coast.outline` (footprint and starting bed); `sea.tide_m` carried | An island with deposition on: fans and valley floors on land and no plain round it; a sea bed that is a shoreface and a shelf without a hand-made start | Island | **The sea not filled**: with G = 1 the land's area is within 5% of G = 0's, where today it is 2.6 times; no sediment above the wave base off a wave-dominated mouth; the budget closes with `shelf_m3`; a lake's delta is byte for byte what it was; the pins of sea-less sketches do not move | Nothing in seconds; one field of 4 bytes a point while running |
| **B** | The stand's frame; exposure; the section (Dean's shoreface, the face, the berm) for a given supply; `shore_distance`, `substrate`, `grain`, `exposure`, `wave_direction`, `beach_slope`, `berm`; the climate recorded | A beach that is a beach in section, with deep water where the grain is coarse; **the bathymetry S102's waves shoal over** | Straight coast | **The profile against Dean's form**: at every lattice point from two cells out to the closure depth, the depth within 0.1 m of A x^(2/3) for the sketch's grain, x from the distance field; the face's slope within 2% of the grain's; the same at 4 m and 8 m within 0.1 m; exposure uniform to 1% on the straight coast, and a shadow behind a test island as wide as geometry says | 5 to 15 s a stand at 1,024 a side |
| **C** | The cliff line as a level set; the platform's two kinds; plunging; the limiter's exemption; yields to the budget; `cliff_height`, `cliff_retreat`, `notch`, `platform`; lattice-scale stacks and the features file's rock entries | Sea cliffs as tall as the land behind them, a platform at their foot, a hard bed standing out as a headland and a stack, streams left hanging: what the authors cut by hand | Island | A straight uniform cliff's retreat against the rate law integrated with the platform's decay (closed form), within 3% or a cell; no retreat where the foot is deeper than 1.25 H; the platform's gradient the one asked for; headland relief rises with the rock's contrast; the slope invariant holds everywhere but marked cells; the line at 4 m and at 8 m within one coarse cell | 1 to 3 s more |
| **D** | Littoral cells, the budget, the plan at zero net transport, tool and cover on stage C's rate | Pocket beaches between headlands, curled in behind the headland the waves bend round; bare headlands; a bay that gathered sand and stopped | Island | **A pocket beach's plan**: under one wave direction the shoreline is square to the arriving crests within 2° along its straight part, and its curve behind one headland is within a stated bound of Hsu and Evans' parabola **once the build row has their table**; **sediment conserved** to nine digits; a cell given twice the sand is wider and not otherwise shaped | 3 to 6 s more |
| **E** | A lake's bench and terrace | A shelf round a lake's windward-facing shores, cobbles on it, none in the lee | Lake | The bench's outer edge at half the storm wavelength for the cell's fetch, within a lattice step in depth; no bench where the fetch is under a stated length; the lake's deltas unchanged; what was cut equals what was built | under 1 s |
| **F** | Sand that moves in time where equilibrium is not the answer: the one-line model solved implicitly along the shore chain; salients and tombolos; a wave-worked mouth's cuspate bulge; a spit past a headland | A tombolo behind the skerry, a bulge at the largest river, a spit across a bay's mouth | Island with an islet; straight coast with a river | An islet's shore response by its length over its distance, against the published thresholds (a tombolo over 1.0, none under 0.3); a mouth's bulge only where R nears 1; the implicit solve's result independent of its step | 5 to 10 s more |
| **G** | The level's history in the long run (drowned valleys, raised shorelines where the land rises); the lattice's own wave field in place of the closed-form exposure (focusing over shoals); bars | Rias and fjord mouths; energy gathered on a headland's shoal; a bar where the beach's state has one | Island | A valley cut at a low stand is flooded to its head at the high one; the wave field against Snell's law on a plane bed and against S102's on the same bathymetry | to be estimated from S102's measured field |

The order is by what unblocks most: A returns deposition to every island the day it merges; B is
the ground S102's waves follow; C and D are the 19 rocky-shore and island scenes and the forest's
own island; E is cheap once B stands and the forest has a lake.

### What the build row needs from other rows

- **S102** (water): the closed forms as shared functions once it merges; a body that takes its
  swell and wind from the coast's recorded climate; its swash and infiltration reading
  `beach_slope`, `grain` and `substrate` where a coast provides them.
- **The tile set and the terrain masks** (the other coordinator's): the profile's re-evaluation at
  tile spacing as a ground edit, with the cusps and the step; the masks painted from `substrate`,
  `shore_distance` and `berm` (wet sand, dry sand, shingle, the wrack line, splash-zone rock).
- **The cliffs and the ledges**: a sea cliff's face from `cliff_height`, `cliff_retreat` and
  `notch`; sub-cell stacks from the features file.
- **The scatter**: kinds placed by `substrate` (boulders in the swash) and along `berm_line`
  (wrack, driftwood, shells: a small static kit, as the proof of concept's table says).
- **The channels**: a channel that crosses a beach to the sea, and one left hanging at a cliff.
- **The material sets**: wet sand, dry sand of two grains, shingle, wrack; the generated pack has
  none (the proof of concept's engine need 64).
- **The landform itself**: resistance over the plan outside a formation (the authors' gap 7: the
  skerries needed a solve of their own), which the cliff's rate reads; and the lattice dependence
  above, as its own row.
- **`core/math/deterministic.h`**: a cube root, if it has none.

### What every solver holds, and where this design stands

1. **The same bytes everywhere**: designed for, [above](#determinism); every stage pins hashes on
   MSVC that GCC and Clang reproduce, as the solver's stages do.
2. **Tile-stable**: the output is on the world's lattice, as the solver's is; the profile's
   re-evaluation at a tile is a function of the fields at the tile's points, so two tiles agree
   along their edge.
3. **Nothing exists before its tile**: not held, by kind, as for the solver: a littoral cell's
   sand is everything along its shore. It is the plan above the tile, built once, offline.
4. **Merged, with a far form**: the heightfield's far forms are the tile set's; a cliff's face,
   a stack and a boulder field are the cliffs' and the scatter's meshes, with their LOD.
5. **It writes to the ground, and the others read it**: it writes the ground, and the fields and
   the features file are how it is read; who reads each is [above](#outputs-and-who-reads-each).
6. **Grounded in published research**: this page, with what was and was not opened said for
   every number; the build row opens what is marked memory before it pins.
7. **Budgets stated**: [estimated above](#what-it-costs), to be measured by each stage as a pair
   against the stage before on the same sketch.

## What to ask of the next reels

The second set is rich in swash and thin in the shape of the coast itself. Five of its 80 scenes
are rocky shore, none shows a cliff's foot, a platform at low tide or a beach in section, and
none is from above. For the stages above, in order of what would decide most:

1. **A beach in section, from the side and from above.** One frame that holds the berm, the face,
   the waterline and the break line, taken along the beach from the berm's height and again from a
   headland or a drone; at low tide if there is a tide. A person standing at the waterline and one
   on the berm for scale. This is the picture stage B is judged against, and none exists.
2. **A pocket beach's plan from above**, the wave crests visible coming in, both headlands in
   frame. It is the only way to see whether the shoreline lies along the crests.
3. **A cliffed coast with its foot showing**: from the sea, the beach or a cliff top looking
   along, at low tide: the notch, the platform (its surface, its seaward edge), fallen blocks,
   stacks and an arch if the coast has them. A person or a boat at the foot. The cliff's rock and
   its bedding in a closer frame.
4. **A shore platform at low tide and again at high**, the same camera: which kind it is, and how
   the surf crosses it.
5. **Cusps**, from the berm looking along and from above, with the swash running, so the spacing
   can be set against the swash's reach in the same frame.
6. **A shingle or cobble beach**: its section, its storm ridge, the step, a close frame of the
   stones with a hand or a coin; and a **boulder shore** with the same.
7. **A river or a stream meeting a beach**, from above or from a height: the bar across its mouth,
   which way it is deflected.
8. **A spit or a tombolo** from above or from a hill.
9. **A lake's exposed shore and its lee shore on the same day**: cobbles and a bench seen through
   clear water on one, mud and reeds on the other, and the wind's direction said.
10. **For every shore**: the place and the date (so the tide's range and state can be looked up), a
    locked camera for thirty seconds (so the period can be counted), a close frame of the sediment
    with something of known size, and one frame that holds the horizon, for the camera's height.
    Clear water seen from above grades by depth and is the nearest thing to a bathymetry a reel
    can give.

Large waves on any of these are not in either set yet; a cliff or a steep beach under a real
swell would be the first reference for the storm that the design's closure depth, berm and
retreat are all written in terms of.

## What could not be found or opened

- **Not opened, restated by a page that was**: Bruun 1954; Dean 1977 and 1987; Hallermeier 1978,
  1981, 1983; Birkemeier 1985; Komar and Inman 1970 and the CERC manual; Pelnard-Considère 1956;
  Ashton and Murray 2001 and 2006; Hsu and Evans 1989; Nienhuis and others 2015; Sunamura 1992
  (through his own 2015 review, of which the last third, on slope failure, was not read).
- **Summaries only**: Moore 1982; Ferguson and Church 2004; Wright and Short 1984; Limber and
  Murray 2011 and the 2014 pair; Matsumoto, Dickson and Kench 2016 (an exploratory profile model
  that makes plunging cliffs, benches and both platform kinds from wave type and tide, with a 2018
  sequel on waves against weathering across tidal ranges: the nearest published thing to stage C,
  and the build row should open the papers. An implementation of it is public under the GPL; its
  code is not to be read into the engine, which takes permissive licences only, ADR-0014); Trenhaile on gradient against
  tide; Kogure and others on notches; Rowan, Kalff and Rasmussen 1992; the Wisconsin lake survey;
  Sunamura and Mizuno 1987 (its file was a scan with no text; the thresholds are an annotating
  page's).
- **From memory, unverified**: the three coefficients of Bujan, Cox and Masselink 2019's fit (its
  abstract opened, which gives the form and not the numbers; the publisher's page redirected and
  returned nothing); Bascom 1951 and Sunamura 1984 on
  the beach face; Takeda and Sunamura's berm height; the edge-wave cusp spacings; Hunt 1979's
  dispersion approximation; Pelletier 2010 on the lattice and the contributing area; the
  engineering manual's table of A; that the sea reached its level about six thousand years ago.
- **Not found at all**: the table of Hsu and Evans' coefficients; Sunamura's condition between the
  two platform kinds and his notch formulas; any rule for the number, spacing or size of stacks,
  arches and geos; a bar's height and place; a beach step's height; the slope of a boulder beach
  and the wave that moves a block; a lake terrace's width against fetch as a formula; the share
  of a river's load that is sand (the default 0.3 is a placeholder), and of a cliff's loss that
  stays on the shore (the same).
- **Disagreements left standing**: Hallermeier's outer depth by the formula (15 m) and by the
  page's worked example (about 11 m) for the same inputs; the tombolo's threshold between
  0.67 and 2 by source.

## Sources

Opened through the page-reading tool, 2026-11-12:

- Coastal Wiki (Flanders Marine Institute): "Shoreface profile"; "Closure depth"; "Littoral drift
  and shoreline modelling"; "Wave-dominated river deltas"; "Gravel Beaches"; "Beach Cusps"; "Rocky
  shore morphology"; "Detached breakwaters". coastalwiki.org.
- J. Bosboom and M. J. F. Stive, *Coastal Dynamics*, section 7.2.2, "(Semi-)empirical derivations"
  (LibreTexts edition).
- T. Sunamura, "Rocky coast processes: with special reference to the recession of soft rock
  cliffs", *Proceedings of the Japan Academy, Series B* 91(9), 2015 (J-STAGE; sections 1 to 7).
- M. Prémaillon, V. Regard, T. J. B. Dewez and Y. Auda, "GlobR2C2 (Global Recession Rates of
  Coastal Cliffs)", *Earth Surface Dynamics* 6, 651–668, 2018 (all but the last eighth).
- IH Cantabria, IH-SET documentation, "Hsu and Evans (1989)" (the parabolic bay's form and
  González and Medina 2001's addition).
- A page annotating Sunamura and Mizuno 1987 and later breakwater rules, at
  ancientportsantiques.com.
- The abstract of D. Bujan, R. Cox and G. Masselink, "From fine sand to boulders: examining the
  relationship between beach-face slope and sediment size", *Marine Geology* 417, 2019, through
  Semantic Scholar's record.
- The owner's second set of reels: `inspiration2/_frames/catalog.json` (the coastal scenes'
  landform notes) and five shots' sheets under `_frames/motion`.
- The forest proof of concept's account of building a coast by hand, as text: its solver-needs
  page ("Added by v42", sections 3.13 and 3.14) and its islands part's README. Leads and the
  statement of the need, not sources.

Named above and not opened: see the section before this one.
