# A world formation field: the grounding (W85, 2026-11-05)

The owner's review of the forest proof of concept's v31 (engine-needs §2, 2026-11-05) asked for
"sedimentary or metamorphic bands across mountainsides — not straight, following the rock". This
page is what the field's shape and its numbers rest on: what a bed is and how thick beds are, how
a dipping and folded bed meets a slope, and what makes one bed stand out of a hillside and another
recede. The mechanism is `engine.scene.Formation` (scene.schema), drawn by the terrain resolve
([renderer](../subsystems/renderer.md#formation-bands)) and carved by a cliff
([rocks](../subsystems/rocks.md#geology-on-the-world-formation-w85)); roadmap W85.

## What a bed is, and how thick

- **A bed is a layer more than a centimetre thick**; thinner is a lamina. McKee and Weir (1953)
  set that limit and the names above it — very thin (1–5 cm), thin and medium, thick
  (60–120 cm), very thick (over 120 cm) — and Ingram (1954) gave the scheme most field descriptions
  use; Campbell (1967) dropped the 1 cm floor. The schema takes a bed from **1 cm to 1,000 m**
  (`thickness_m`, 0.01–1000): the floor is McKee and Weir's, the ceiling a formation's.
  (Sources: [Bed (geology)](https://en.wikipedia.org/wiki/Bed_(geology)), whose table reproduces
  McKee and Weir; [USGS, Terminology for stratification](https://pubs.usgs.gov/publication/70217840).)
- **Thicknesses are lognormal-like, not regular.** Talling (2001, *Sedimentology* 48:1297–1329)
  showed that turbidite beds of one basal grain size are lognormally distributed and the whole
  sequence a sum of lognormals; Carlson and Grotzinger (2001, *Sedimentology* 48:1331–1351)
  argued for power-law tails in some fans; Sylvester's re-analysis
  ([abstract](https://www.searchanddiscovery.com/documents/abstracts/2005pacific/abstracts/sylvester.htm),
  [notes](https://zsylvester.github.io/img/bed_thicknesses.pdf)) rejects the segmented power law.
  Drummond and Wilkinson (1996, *J. Geology* 104:1–18) found the same exponential-tailed,
  lognormal-type frequencies in lithofacies, named units and peritidal cycles, and argue that a
  hierarchy of "nth-order cycles" is mostly an arbitrary division of a continuous record
  ([record](https://acnpsearch.tweb-dev.unibo.it/singlejournalindex/14385635)).
  **What the field takes from this:** the beds are authored, not generated, but a sequence should be
  unequal — the test fixture and the captures draw four thicknesses from a lognormal about a
  0.9 m median with a log standard deviation near 0.7 (1.42, 0.37, 0.81, 2.23 m), and the macro
  sequence (members of a formation, the scale Drummond and Wilkinson's "named units" sit at) is
  three thicknesses of 34, 52.5 and 41 m. A sequence repeats exactly; with eight unequal beds and a
  second, incommensurate macro period the repeat is not something the eye finds on a hillside.
  A generator that draws the sequence from a seed and Talling's distribution is not built
  ("Next" in the W85 hand-off).
- **Why a macro sequence at all.** A 1.5 m bed is 1.4 pixels at a kilometre (G132,
  [v29-cliff-diagnosis](v29-cliff-diagnosis.md)), so by 2 km a sequence of metre beds is a mean
  colour under every pixel; members of tens of metres are what a distant mountainside's banding is.
  The box filter (below) makes the hand-over continuous: the bed sequence averages to its mean
  exactly as the footprint grows past its period, while the macro beds stay resolved.

## Dip, strike and the fold

- **A bed is a plane until it is folded**: its dip (the angle below horizontal, measured in the
  dip direction) and the strike perpendicular to it. The field uses rocks.md's existing convention
  (`detail::make_fabric`): the dip direction t = (cos az, 0, sin az), the normal leaning toward it
  by the dip, so the beds go down along t. A bed is never vertical here (dip within ±85°, as the
  rocks' geology already holds), because the coordinate's gradient along the ground must stay
  finite for the footprint.
- **The fold is cylindrical**: the folded surface is a line (the hinge) swept along the strike, and
  its profile across the hinge is, to first order, a sinusoid — Fourier analyses of natural fold
  profiles (Chapple 1968, Stabler 1968, Hudleston 1973, *Tectonophysics* 16:1–46) put most of a
  fold's shape in the first harmonic, the sinusoid being the shape whose higher terms are zero
  ([FOLD PROFILER](https://www.academia.edu/18188066/FOLD_PROFILER_A_MATLAB_based_program_for_fold_shape_classification)).
  So the field displaces the beds along their normal by A sin(2π u / L), u the distance along the
  dip direction, which puts the hinges along the strike. A second, longer fold is added for the
  regional bend under the local one.
- **The wavelength scales with the stiff layer**: Biot's (1961, *GSA Bull.* 72:1595–1620) dominant
  wavelength grows with the layer thickness and the cube root of the viscosity contrast, and
  measured wavelength-to-thickness ratios in natural single-layer folds average about 12 to 16 with
  a minimum near 6 (Hiroshima study of Biot's relation, [ir.lib.hiroshima-u.ac.jp/00053038](https://ir.lib.hiroshima-u.ac.jp/00053038);
  Sherwin and Chapple 1968, *Am. J. Sci.* 266:167–179). A fold that moves a whole sequence is
  controlled by its stiffest members, so the captures take the folded unit as the macro members
  (tens of metres): a 180–400 m wavelength and an amplitude of 5–15% of it, inside the gentle-fold
  range (an amplitude under a quarter of the wavelength). The schema allows 0–1,000 m of amplitude
  and 1 m to 100 km of wavelength: the field is a description, not a buckling model, and an author
  can draw a chevron's first harmonic or a basin.

## What stands out, and what recedes

- **Differential weathering**: resistant beds (sandstone, limestone, conglomerate in an arid
  climate) hold steep faces and ledges; shale and mudstone weather back into slopes and are
  undercut beneath the hard beds, which then break away — the Grand Canyon's stepped profile is
  the textbook case, Powell's explanation still the one given
  ([USGS, the South Kaibab Trail](https://www.usgs.gov/media/images/phantom-ranch-tour-a-block-sandstone-along-south-kaibab-trail),
  [Kaibab Trail notes](https://gotbooks.miracosta.edu/gonp/grca/phantom_ranch/html/grand048.htm)).
- **What the field takes from this:** each bed carries a `resistance`, 0 (recedes) to 1 (stands
  out). Nothing draws it tonight: it is the input the exposed-bedrock solver (engine-needs §1.1)
  takes to place ledges on resistant beds and soil on weak ones. The captures' fixture puts
  sandstone at 0.85, limestone 0.95, siltstone 0.45 and shale 0.15, which is the order the sources
  give and no more than that.
- **Colour**: the tints are multipliers on a layer's own texture, so a band reads as the rock it
  crosses, lighter or darker, warmer or greyer — the beds of one formation differ in tone far more
  than in hue. The fixture's are within ±30% of white.

## The edge

A contact between two beds is sharp at the scale of a centimetre to gradational over a few, so
`soft_m` (the narrowest an edge is drawn) defaults to 0.1 m: a design choice inside that range, and
the floor under the pixel's own footprint across the beds, which takes over beyond a few tens of
metres. The band is the sequence's value **box-filtered** over that width, computed from the
sequence's running integral (two lookups, whatever the width), so at any distance a pixel shows the
average of the beds it covers and a band never aliases.

## Numbers pinned

| Number | Value | From |
|---|---|---|
| bed thickness range | 0.01–1,000 m | McKee and Weir's floor; a formation's scale |
| beds a sequence | 1–8 (macro 0–8) | a GPU block of fixed size; incommensurate periods hide the repeat |
| dip | ±85° | rocks' existing range; a finite gradient |
| fold amplitude, wavelength | 0–1,000 m, 1–100,000 m | descriptive, not a buckling model |
| soft edge | 0–10 m, default 0.1 m | contacts sharp to gradational over centimetres |
| tint | 0–2 a channel | a multiplier on the layer's albedo |
| resistance | 0–1 | an ordering, sandstone/limestone high, shale low |
