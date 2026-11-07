# Caves: what the passage and the speleothems are grounded in (G134)

The owner asked for caves and interiors on 2026-11-06 (the proof of concept's engine-needs page, §6). This page is the research the `caves` capability ([caves](../subsystems/caves.md)) starts from, and the numbers its recipe pins with their ranges. The terrain's half — a hole in the ground the passage opens from — is W87 ([terrain](../subsystems/terrain.md#holes)).

## Sources

- **Ford, D. C. and Williams, P. W. (2007).** *Karst Hydrogeology and Geomorphology*, Wiley, chapter 7 (speleogenesis: the origin of caves) and chapter 8 (cave interior deposits). The standard text.
- **Palmer, A. N. (1991).** Origin and morphology of limestone caves. *GSA Bulletin* 103(1), 1–21, doi:10.1130/0016-7606(1991)103<0001:OAMOLC>2.3.CO;2. From its abstract: caves form along the ground-water paths of greatest discharge and solutional aggressiveness, and most take 10⁴–10⁵ years to reach a traversable size; of the ~500 caves Palmer classified, about 57 % are branchwork, 17 % network, 14 % a single passage (as summarized in *Journal of Cave and Karst Studies* 65(3), 2003).
- **Short, M. B., Baygents, J. C., Beck, J. W., Stone, D. A., Toomey, R. S. and Goldstein, R. E. (2005).** Stalactite growth as a free-boundary problem: a geometric law and its Platonic ideal. *Physical Review Letters* 94, 018501; and Short, Baygents and Goldstein (2005), Stalactite growth as a free-boundary problem, *Physics of Fluids* 17, 083101. The growth rate is proportional to the local thickness of the film flowing down the surface (thin-film Stokes flow), and a broad class of initial shapes is attracted to one ideal shape, which matched the mean of photographed stalactites in Kartchner Caverns (Arizona) to within 5 %. The follow-up on icicles (Short, Baygents and Goldstein 2006, *Physics of Fluids* 18, 083101) states that away from the tip its solution has the same power law.
- **Romanov, D., Kaufmann, G. and Dreybrodt, W. (2008).** Modeling stalagmite growth by first principles of chemistry and physics of calcite precipitation. *Geochimica et Cosmochimica Acta* 72, 423–437; **Kaufmann, G. (2003)**, Stalagmite growth and palaeo-climate: the numerical perspective, *Earth and Planetary Science Letters* 214, 251–266; and Dreybrodt and Romanov's "Regular stalagmites: the theory behind their shape" (*Acta Carsologica* 37/2-3, 2008): a stalagmite under a steady drip grows to an equilibrium shape — a column of constant diameter with a rounded top — whose radius is set by the drop's volume and the drip interval by mass balance. Curl's earlier analysis gave a minimum diameter of about 3 cm from the drop's volume and the film at the apex, close to what is observed; the small regular stalagmite of the Acta Carsologica paper is 4.5 cm across.
- **Hill, C. A. and Forti, P. (1997).** *Cave Minerals of the World*, NSS: a soda straw is a hollow tube whose bore is the drop's — about 5 mm across — growing at its rim, and becomes a carrot-shaped stalactite when its bore is blocked and the water runs down the outside.

## What the passage takes

| Quantity | Pinned | Range the recipe accepts | Source and reasoning |
|---|---|---|---|
| Phreatic tube: width to height | 2.0 | 1.2 – 3.5 | Ford and Williams ch. 7: a tube formed under water full of the flow is round in massive rock and elliptical along a bedding plane, wider than high, its long axis in the bed. |
| Vadose canyon: height to width | 3.0 | 1.5 – 8 | Ford and Williams ch. 7, Palmer 1991: a free-surface stream cuts down, so a canyon is tall and narrow, its width the stream's. |
| Keyhole | a tube over a canyon, the canyon's width 0.35 of the tube's and its depth 1.2 tube heights | 0.2 – 0.6 of the tube; 0.5 – 3 tube heights | A phreatic tube later entrenched by a vadose stream (Ford and Williams ch. 7). |
| Passage width | 3 m | 0.8 – 12 m | Traversable passages (Palmer 1991's "traversable size"), within the triangle budget; larger halls are not this solver's. |
| Wall roughness | 4 % of the width | 0 – 15 % | Bounded value noise along and round the passage: scallops and breakdown at the picture's scale, never a fold through the shell. |
| Bedding control | a phreatic tube's floor follows the scene's formation bed it starts in (W85), within its dip | — | Palmer 1991: phreatic passages follow the beds and the fractures; the formation field is the engine's own beds. |

## What the speleothems take

| Quantity | Pinned | Range | Source |
|---|---|---|---|
| Stalactite profile | radius ∝ (distance from the tip)^(3/4), from a tip of the soda straw's radius | exponent fixed | Short et al. 2005: the ideal shape's far field is height ∝ radius^(4/3), so radius ∝ length^(3/4). Its rounded tip region, a few soda-straw radii long, is drawn as the straw's bore radius; the exact analytic form near the tip was not re-read for this page (the PRL's text was not reachable from here), so only the far-field law is used. |
| Soda straw radius | 2.5 mm | 2 – 3.5 mm | Hill and Forti: the bore is the drop's, about 5 mm across. |
| Stalactite length | 0.2 – 1.2 m | 0.05 – 3 m | A passage of a few metres; the recipe's range. |
| Stalactite base radius to length | 0.12 | 0.06 – 0.25 | The ideal shape's slenderness at those lengths (carrot-shaped, Kartchner's photographs). |
| Stalagmite radius | 3 cm | 1.5 – 15 cm | Curl's minimum diameter of about 3 cm; Romanov et al.'s equilibrium radius set by the drip. |
| Stalagmite height | 0.1 – 0.8 m | 0 – 3 m | The recipe's range. |
| Column | where a stalactite's tip comes within 10 cm of its stalagmite's top | 0 – 0.5 m | Ford and Williams ch. 8: a column is the two joined. |
| Drip density | 0.6 per m² of ceiling | 0 – 5 | A decorated passage; 0 is a bare one. |

## What is not taken

Breakdown (fallen blocks), flowstone and draperies, cave pearls, helictites, pools and rimstone dams; maze (network) and spongework patterns, halls; the dissolution itself (the passage is a recipe's shape, not a simulated speleogenesis). Light inside a cave is the renderer's: under the sky ambient of today's resolve the interior is too bright (the unshadowed hemisphere reaches every wall), and R69's irradiance cache is what darkens it ([caves](../subsystems/caves.md#light)).
