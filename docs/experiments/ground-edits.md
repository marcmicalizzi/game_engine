# Ground edits: the slopes a fill and a bench take (W98)

**What this page is.** The sources of the default numbers of `engine.scene.TerrainEdit` (roadmap W98; [tileset](../subsystems/tileset.md#ground-edits-that-raise-as-well-as-lower)), and where each number was chosen inside the range a source gives. The cut's own channel numbers are [channels](channels.md#the-cut)'. Read 2026-11-10; each source below was opened and its passage read, except where a line says otherwise.

## The sources

1. **Washington State DOT, *Geotechnical Design Manual* M 46-03, Chapter 9, "Embankments"** (2022), p. 9-7, "Stability Assessment": an embankment of 10 ft or less with side slopes of **2H:1V or flatter** may be designed on precedent; steeper needs a stability analysis, and face stabilization is required steeper than 1.7H:1V.
2. **Washington State DOT, *Geotechnical Design Manual* M 46-03, Chapter 10, "Soil Cut Design"**: cuts of 2H:1V or flatter in granular soil need little analysis (§10.2.1, p. 10-4); sand and gravel stand at 1.5H:1V to 2H:1V, dense glacial till at 1.75H:1V to **1H:1V** (p. 10-6).
3. **Iowa DOT, *Design Manual* 200F-2 "Embankments"** and **200F-8 "Rock Cut Slopes"** (2026): highway foreslopes since about 2010 at 3.5H:1V, the oldest as steep as 1.5H:1V to 2H:1V; rock cuts at 0.5H:1V in limestone and cemented sandstone.
4. **AASHTO, *Roadside Design Guide*, 4th ed.** (2011), through a secondary page (the guide itself was not opened): 4H:1V or flatter recoverable, 3H:1V to 4H:1V traversable.
5. **USDA Forest Service, *Trail Construction and Maintenance Notebook*** (Hesselbarth et al., 2007, 0723-2806-MTDC; the HTML edition, "Rolling Contour Trails", "Full-Bench Construction" and "Partial-Bench Construction"): trail builders "almost always prefer full-bench construction"; the tread is outsloped **about 5 percent**; the backslope "should match the angle of repose of the parent material", **most soils stable at 1:1**, solid rock at 2:1 (V:H), less cohesive soils needing 1:2; a fill steeper than its angle of repose needs a retaining or crib wall.
6. **FAO, *Manual on Small Earth Dams*** (Stephens, 2010, Irrigation and Drainage Paper 64; title from memory, its §3.2 p. 15 opened): homogeneous earth dams at 1:3 upstream and **1:2 downstream**.

## The numbers chosen

| Field | Default | Range | Source and where in it |
|---|---|---|---|
| `fill_slope` | 0.5 (1V:2H) | [0.1, 8] | 1: the steepest earth fill designed on precedent; 6: a homogeneous dam's downstream face. A road's recoverable 1V:4H (0.25, 4) and a rock fill's face are inside the range. |
| `bank_slope` on a bench's or a patch's cut face | 1 (1V:1H) | [0.25, 8] | 5: "most soils are stable with a 1:1 backslope"; 2: dense till at 1H:1V. Rock (2:1, 0.5H:1V in 3) is inside the range. Shared with the channel's bank, whose default is the same number from [channels](channels.md#the-cut). |
| `face_m`, `fill_m` | 2 m | (0, 100] m | Not from a source: the height at which a face stops holding its slope and runs out (the cut's rule since W93). 1 sets 10 ft (3 m) as the height designed on precedent; 2 m keeps a trail's or a pad's faces at their slope and lets a tall fill steepen into the ground rather than reach tens of metres out. |
| tread outslope | none | — | 5's 5 percent is under a centimetre across a 1 m tread at the lattices the set is built at (25 cm and coarser), so a bench is flat across its width; a route that wants it names a tilted line (each point's y) or a patch with a grid. |

**Not grounded, and why.** The angle of repose of loose gravel, rock fill and talus (30 to 40 degrees in the summaries found) was not found in a source that could be opened tonight; a deposit's faces (G143's) should be grounded on Carson and Kirkby (*Hillslope Form and Process*) or Selby (*Hillslope Materials and Processes*) before its row pins `fill_slope` for scree. A fill's slope above 1 (steeper than 45 degrees) is a dry-stone or crib face, which the sources say needs a wall: the range allows it so a wall's footing can say so, and the wall is the wall's row.

**Where structure was traded.** A face is a plane at its slope; real cuts and fills bench, slump and ravel. At the lattice spacings the set holds (25 cm finest), a face's own relief under a metre is the material's and the cover's, not the heights'.
