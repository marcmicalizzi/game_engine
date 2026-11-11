# Cave networks: the grounding (G146)

What the cave network solver ([caves](../subsystems/caves.md#the-network)) takes from the
literature, the number chosen for each rule and where it came from, and, for each source, whether
it was read again on the night the solver was written (2026-11-10) or is carried from memory of the
published work and still owes a reading. The single passage's grounding (profiles, the stalactite's
shape) is [caves.md](caves.md) and is not repeated.

**How to read the "checked" column.** *Read*: the statement was found in the source, or in a
paper quoting it, that night. *Derived*: it follows from first principles written out below.
*Owed*: the publisher's page refused an automated reader, so the number is the literature's as
remembered; it is flagged here so the next reader checks it before leaning on it. Nothing below
is tuned by eye and presented as a source's.

## What decides a cave's pattern

| Rule the solver follows | Source | Checked |
|---|---|---|
| The pattern is decided by how the water gets in. Sinkhole recharge makes a **branchwork**: tributaries that join downstream into passages of higher order. Mazes need many routes enlarging at once (steep gradients with strong undersaturation, or recharge spread evenly along many routes); ramiform caves rising thermal or sulphidic water. Structure decides where passages run, not which pattern forms. | Palmer 1991, abstract | Read |
| Branchworks are the common case: of Palmer's sample of about 500 caves, 17 % are network mazes, 14 % rudimentary single passages, 5 % ramiform or spongework, 3 % anastomotic, and the rest (about three fifths) branchwork. | Palmer 1991, as quoted by Dom and Wicks 2003 | Read (the four figures; the remainder is arithmetic) |
| Wall retreat approaches 0.01 to 0.1 cm a year, and a cave takes 10,000 to 100,000 years to reach a size a person passes. | Palmer 1991, abstract | Read |
| **Vadose passages follow the dip; shallow phreatic tubes follow the strike.** Water above the water table runs down the steepest opening it has, the dip of the bedding; at the water table it runs along the bed where the bed meets that level, which is the strike. | Palmer 1991 and Palmer 2007 (*Cave Geology*); Ford and Williams 2007, ch. 7 | Owed |
| **Levels are former base levels.** A trunk forms at the water table while the valley floor stands still; when the valley cuts down, the water finds a lower route and the old trunk is left dry above. | Palmer 1987 (*Cave levels and their interpretation*); Ford and Williams 2007 | Owed |
| A tube's long axis lies in the guiding bedding plane; a canyon is cut plumb, down from the tube it entrenches; a keyhole is the two together. | Lauritzen and Lundberg 2000; Ford and Williams 2007 | Owed (the proportions are [caves.md](caves.md)'s) |
| Phreatic tubes loop up and down across the beds, the more the fewer fissures the rock has (the four-state model). | Ford and Ewers 1978, in Ford and Williams 2007 | Owed |

**What the solver does with it.** One pattern, the common one: a branchwork. Per level a trunk is
laid along strike where the member's middle is at that level's height (so in dipping beds the
levels are offset down dip by `level_spacing_m / sin(dip)`, and in level beds they are stacked in
the member); feeders come down dip into each trunk as canyons, keyholes near the junction;
connectors run from each level down dip to the next, which is what makes the loops; the lowest
level is at the valley floor the solver finds from the ground. Mazes, spongework and hypogene
forms are not built.

## The numbers

| Number | Value | Where it is from | Checked |
|---|---|---|---|
| Base level | the lowest ground within the network's reach | the definition of a base level; found from the ground | Derived |
| Level spacing | `level_spacing_m`, 9 m by default, 4 to 40 | the content's: a valley's history is not derivable from its present ground | not a constant |
| Trunk width downstream | area proportional to distance downstream, so width as its root, from 0.62 of the full width at the head | discharge adds at junctions and width goes as the root of discharge (hydraulic geometry: Leopold and Maddock 1953) | Owed |
| Meander wavelength | 11 widths, hashed within 15 % | Leopold and Wolman 1960 for rivers; Deike and White 1969 found solution conduits follow the same proportion | Owed |
| Meander amplitude | 0.08 to 0.15 of the wavelength | chosen within what a sinuosity of 1.1 to 1.3 gives | chosen, said so |
| Phreatic loop amplitude | at most 1.2 m, and never past the member's margin | Ford and Ewers's states 3 and 4 (a well-fissured rock); the member's margin rules | chosen within the model |
| Scallop length | `22,500 nu / v`, nu = 1.307e-6 m2/s (water at 10 C); 0.59 m at the default 0.05 m/s | Curl 1966: the scallop Reynolds number on the mean velocity and the scallop's length is about 22,500 (Blumberg and Curl 1974 give 2,200 on the friction velocity; Thomas 1979 about 1,000 across many materials) | Read (the constant, in a 2019 paper quoting it); Curl 1974's refinement by conduit size is owed |
| Scallop depth | 0.12 of its length | Blumberg and Curl 1974's flute profile, as remembered | Owed |
| Scallop asymmetry | steep on the upstream side, long on the downstream | Curl 1966 | Owed |
| Hall's roof, stable span | a fixed beam of one bed: `L = sqrt(2 S t / (rho g))` | beam theory, below; White and White 1969 apply it to cave roofs | Derived; the application Read (White and White 2003 restate it) |
| Hall falls in | cover under 0.33 of the span | Hatzor, Wainshtein and Bakun Mazor 2010: caverns to 18 m span stand at a cover of a third of the span; a 40 m span shows partial collapse | Read |
| Breakdown's shapes | slabs a bed thick, blocks, chips | Davies 1949's classes, in White and White 2003 | Read |
| Pile's slope | 0.3 of the hall's height over 0.9 of its radius, about 30 degrees at the default proportions | the angle of repose of angular rock, 30 to 40 degrees | Owed (a textbook range) |
| Sediment fill | 0.22 of a tube's height | chosen: enough for a level floor a walker's width wide in a tube twice as wide as high | chosen, said so |
| Drips in rows 2.5 m apart | along the joints the water comes down | speleothems line up under fractures (Hill and Forti 1997, *Cave Minerals of the World*) | Owed |

### The beam

A bed of thickness `t` spanning a width `L`, held at both walls, carrying its own weight: the load
a metre is `w = rho g b t` for a strip `b` wide, the largest moment is at the walls, `w L^2 / 12`,
and the stress there is `6 M / (b t^2) = rho g L^2 / (2 t)`. It fails where that reaches the
rock's strength in bending `S`: `L = sqrt(2 S t / (rho g))`. A bed cut through by a joint or
dissolved off one wall is a cantilever, with moment `w L^2 / 2`, and `L = sqrt(S t / (3 rho g))`.
For limestone at `rho` = 2,650 kg/m3 and `S` = 10 MPa (a mid figure; published values run from 5
to 15), a 0.9 m bed spans 26 m fixed and 10.7 m as a cantilever. The solver's halls are 9 to 24 m
across the short way, inside the fixed beam's span and past the cantilever's: a hall is where the
roof is failing bed by bed, which is what its stepped ceiling and its pile say.

## The rules, by what the code cites

**Sinuosity.** A trunk's meander is about eleven of its widths long (Leopold and Wolman's
proportion for rivers, which Deike and White found solution conduits keep), its amplitude 0.08 to
0.15 of that; a feeder wanders 1.5 to 3 m about a line within 26 degrees of the dip.

**Sumps.** The water table is base level. Where a passage's floor is under it there is water, and
where its roof is under it there is no air and no surface: a sump. The lowest trunk's head is taken
4 m under, across the beds where the member has the room and down dip where it has not, because a
branchwork's upstream end at base level is, as a rule, where the explorer stops.

**Halls.** A hall is where a roof is failing bed by bed (the beam, above): its span 13 to 18 m,
inside what Hatzor and others found standing; its ceiling stepped by the beds (2.2 times a wall's
ledge); its floor the pile, slabs a bed thick on a cone near the angle of repose. With less cover
than a third of its span it has stoped through to the ground, and the gap is found, not declared.
A hall is the one thing that leaves the soluble member: breakdown needs no solution.

**Speleothems.** Dripstone stands under the fractures the water comes down, so drips are placed in
rows across the passage at joints 2.5 m apart along it, a share of the joints wet; a stalagmite
under a drip where the floor is dry, a column where the two meet, a low boss of flowstone where the
floor under the drip slopes and the water runs before it gives up its calcite. Shapes are
[caves.md](caves.md)'s.

## What is traded, and for what

- **Scallops shorter than 2.4 lattice spacings are not geometry.** At the default 0.25 m lattice
  that is 0.6 m, the scallop of a flow of 5 cm a second. A stream at 0.5 m/s cuts scallops 6 cm
  long, which a 0.25 m lattice cannot carry: those are the material's (a normal map on the
  limestone role), and the page says which role wants them.
- **One pattern.** A branchwork with loops from its levels and one bypass. Network mazes under a
  sandstone cap, anastomoses on a single parting and spongework are each a different layout rule.
- **The plan knows the ground only as heights.** A real cave's entrances are also where a stream
  sinks; the solver has no stream to ask (the channels generator is a neighbour's), so a feeder's
  head becomes an opening by cover alone.

## Surveys to lay the plan beside

The plan view and the long section `engine-content cave-plan` writes are drawn as a survey is:
passages at their width in plan, roof and floor in section with no vertical exaggeration. The
published ones to compare with, none of which could be opened that night: Blue Spring Cave,
Indiana, the standard branchwork (Palmer 1991, fig. 6); Crevice Cave, Missouri; the Flint
Ridge and Mammoth Cave levels in section (Palmer 1987); Ogof Ffynnon Ddu, Wales, for canyons down
dip into strike tubes. What to look for: tributaries meeting the trunk at acute angles pointing
downstream; trunks parallel to strike over hundreds of metres; levels at even heights in
section, joined by steep short passages.

## Sources

- Curl, R. L. (1966). Scallops and flutes. *Transactions of the Cave Research Group of Great
  Britain* 7, 121-160.
- Curl, R. L. (1974). Deducing flow velocity in cave conduits from scallops. *NSS Bulletin* 36(2),
  1-5.
- Blumberg, P. N., and Curl, R. L. (1974). Experimental and theoretical studies of dissolution
  roughness. *Journal of Fluid Mechanics* 65, 735-751.
- Thomas, R. M. (1979). Size of scallops and ripples formed by flowing water. *Nature* 277,
  281-283.
- Deike, G. H., and White, W. B. (1969). Sinuosity in limestone solution conduits. *American
  Journal of Science* 267, 230-241.
- Dom, J. E., and Wicks, C. M. (2003). Morphology of the caves of Missouri. *Journal of Cave and
  Karst Studies* 65(3), 155-159.
- Ford, D. C., and Ewers, R. O. (1978). The development of limestone cave systems in the
  dimensions of length and depth. *Canadian Journal of Earth Sciences* 15, 1783-1798.
- Ford, D., and Williams, P. (2007). *Karst Hydrogeology and Geomorphology*. Wiley.
- Hatzor, Y. H., Wainshtein, I., and Bakun Mazor, D. (2010). Stability of shallow karstic caverns
  in blocky rock masses. *International Journal of Rock Mechanics and Mining Sciences* 47,
  1289-1303.
- Hill, C., and Forti, P. (1997). *Cave Minerals of the World*, 2nd ed. National Speleological
  Society.
- Lauritzen, S.-E., and Lundberg, J. (2000). Solutional and erosional morphology. In Klimchouk,
  Ford, Palmer and Dreybrodt (eds), *Speleogenesis: Evolution of Karst Aquifers*, National
  Speleological Society, 408-426.
- Leopold, L. B., and Wolman, M. G. (1960). River meanders. *GSA Bulletin* 71, 769-794.
- Palmer, A. N. (1987). Cave levels and their interpretation. *NSS Bulletin* 49, 50-66.
- Palmer, A. N. (1991). Origin and morphology of limestone caves. *GSA Bulletin* 103, 1-21.
- Palmer, A. N. (2007). *Cave Geology*. Cave Books.
- White, E. L., and White, W. B. (1969). Processes of cavern breakdown. *NSS Bulletin* 31(4),
  83-96.
- White, E. L., and White, W. B. (2003). Gypsum wedging and cavern breakdown: studies in the
  Mammoth Cave System, Kentucky. *Journal of Cave and Karst Studies* 65(1), 43-52.
