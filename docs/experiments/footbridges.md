# Footbridges where a path fords a stream: the grounding (G147)

The sources behind [bridges](../subsystems/bridges.md): which crossing a footpath builds over a
stream one to ten metres wide, and the dimensions of each form's members. Read 2026-11-11 by a
research pass over published pages (each read as text through a fetch, which summarises the page
for a question: "full text" below means the numbers were in the text the pass got; two PDFs were
read as extracted text, the most direct reading it had). Every number the solver pins is in the
table with the source it was checked against, and the numbers that are choices say so.

## Sources

| Id | Source | How it was read |
|---|---|---|
| S1 | USDA Forest Service MTDC, *Trail Construction and Maintenance Notebook*, 2007 edition (0723-2806-MTDC), "Crossing streams and rivers" and "Trails in wet areas" (fs.usda.gov/t-d/pubs/htmlpubs/htm07232806/) | full text of the two HTML pages |
| S2 | USDA Forest Service, *Standard Specifications for Construction of Trails and Trail Bridges on Forest Service Projects*, 2014, §917 Fords, §934 Puncheon, §961 Native log stringer trail bridge | full text of the PDF |
| S3 | USDA Forest Service MTDC, *Locating Your Trail Bridge for Longevity* (1023-2808, 2010), pages 2 to 5, 12 and 13 (page 12 reproduces FSH 7709.56b ch. 7, the trail bridge design criteria) | full text of the pages |
| S4 | USDA Forest Service, *Trail Bridge Catalog* (MTDC 0823-2W01): deck girder, single span, rail systems, abutments | full text of the pages |
| S5 | USDA Forest Service, *Forest Trail Handbook*, section X, Bridges, §55 and table 5 (undated; a search dated it 1935, not confirmed) | the page, its table garbled in the fetch |
| S6 | The Conservation Volunteers (formerly BTCV), *Footpaths* handbook, online edition: types of bridge, footbridges, abutments and end seats, design of superstructure, stone slab paths | full text, except the superstructure tables (a summary) |
| S7 | Historic England list entries: Tarr Steps 1247822 (via the Exmoor HER), Postbridge clapper bridge 1105432 | full text; 1002508 (Postbridge's scheduling) a search summary only |
| S8 | Murray et al.'s gait values (step length on level ground), from a university gait page | a secondary page |
| S9 | USDA Forest Service, low-volume roads best practices, chapter 10, Bridges | full text of the extracted chapter; a road guide |

Not reached: the SCA's *Lightly on the Land*, the print edition of the TCV's handbook, FSH 7709.56b
itself. **No published gauge of a stepping stone's spacing, size or height over the water was
found** in any of them.

## The numbers

| Pinned in the solver | Value | Source and what it says | Kind |
|---|---|---|---|
| Stepping stones: deepest crossing, `k_stones_max_depth_mm` | 500 mm bankfull | S1: hikers ford where the water is 400 to 600 mm (16 to 24 in) deep most of the season | from the source, its middle |
| Stepping stones: widest, `k_stones_max_width_mm` | 4 m | none: no handbook gives a width for stones | **a choice** |
| A stone's pitch, `k_stone_pitch_mm` | 600 mm | S8: step length 0.66 m (women) to 0.79 m (men) on level ground; a step between stones is shorter | derived from a step length; **a choice** |
| A stone's top over the water, `k_stone_rise_mm` | 150 mm | none found | **a choice** |
| Clapper: longest slab, `k_clapper_max_slab_mm` | 2.4 m centre to centre | S7 and the page it cites: Tarr Steps' slabs mostly 6 to 8 ft (1.8 to 2.4 m), the largest about 2.4 m by 1.5 m, over gaps of 1.2 to 1.8 m | from the source |
| Clapper: deck width, `k_clapper_deck_mm` | 1.2 m | S7: Tarr Steps 1.5 m (5 ft); Postbridge about 2 m; S6: footbridge decks 750, 900 or 1200 mm | between the sources |
| Clapper: pier width along the path, `k_clapper_pier_mm` | 700 mm | S7 (secondary): Tarr Steps' piers 4 to 6 ft apart with slabs 6 to 8 ft long, so a pier of about 2 ft | derived |
| A pier founded below the bed, `k_pier_found_mm` | 200 mm | S4: footings at or below scour depth (no number); the report's "piers founded below the bed" | **a choice** |
| Clapper only where stone is to hand, `k_clapper_min_d84_mm` | D84 256 mm | S6: "only possible where large enough slabs are obtainable nearby"; 256 mm is the cobble-boulder break of the Wentworth scale | the rule from the source, the threshold a choice |
| Clapper slab thickness | span / 10, 150 to 250 mm | none found (no listing gives a thickness; S6's flags are 100 mm for a 1 m paving flag) | **a choice** |
| A clapper's deck at the banks' level, no freeboard | — | S7: Tarr Steps stands about 1 m over the river and was rebuilt after the floods of 1942, 1952 and 1979: a clapper is overtopped | from the source |
| Foot log diameter, `k_foot_log_mm` | 480 mm | S1: a single foot log at least 457 mm (18 in) through, its walking surface hewn at least 250 mm (10 in) wide | from the source |
| Foot log's longest span, `k_foot_log_max_span_mm` | 5 m | S4: a foot log spans 3 to 12 m (10 to 40 ft); the 18 in minimum makes the short end realistic | **a choice** within the source's range |
| Stringers' longest span, `k_stringers_max_span_mm` | 9 m | S6: its designs suit spans "under nine metres", longer want an engineer; S4: log deck girders 3 to 15 m | from the source |
| Stringer diameter by span | 250 mm at 5 m to 400 mm at 10 m, straight between | S6 (summary): round log mains of 250 mm at about 5 m to 400 mm at about 10 m; S5's 1930s table (4 logs of 12 in at 36 ft) agrees with a bending check at 1,200 psi | from the source; the straight line between is a choice |
| Two stringers under a 900 mm deck | — | S6 (summary): two logs for the narrower decks, three for 1200 mm | from the source |
| Deck width, `k_deck_mm` | 900 mm | S3 (FSH 7709.56b): hiker clear width at least 3 ft (914 mm); S6: 750 / 900 / 1200 mm | from the sources |
| Planks, `k_plank_mm`, `k_plank_thick_mm`, `k_plank_gap_mm` | 150 × 50 mm, 20 mm gaps | S6: deck boards 150 × 50 mm softwood; S1 and S2: puncheon decking with 3/4 in (20 mm) gaps | from the sources |
| Stringers hewn 50 mm for the planks | 50 mm | S2 §961: stringers hewn up to 2 in (50 mm) for plank bearing | from the source |
| A sill sunk a third of its diameter at most | 1/3 | S2 §961: sill logs hewn no more than one third of their diameter, mud sills buried | from the source |
| Handrail height, `k_rail_mm` | 1070 mm | S1, S4, the NPS: at least 42 in (1,070 mm) for pedestrians | from the source |
| When a rail is fitted, `k_rail_fall_mm` | deck over 1 m above the bed | S1 and S4: rails "unless an analysis (design warrant) shows they are not needed"; S6: simple low bridges have none "where there is no safety risk"; **no fall height in any source** | **a choice** |
| Sill or abutment set back from the bank's edge, `k_setback_mm` | 400 mm | S3: span at least the bankfull width with no piers in the stream; its worked example sets the bridge on stable ground 3 ft (0.9 m) back from the top of the bank (a site, not a rule) | the rule from the source, the distance a choice |
| Freeboard of a timber bridge | the bankfull depth over bankfull, at least `k_min_freeboard_mm` 300 mm | S3: the 50 to 100 year flood stands about twice the bankfull depth over the bed (its stated rule of thumb), so a deck over it clears bankfull by one bankfull depth; S9: 0.5 to 1.0 m over the design high water plus debris (roads); S3's example 0.9 m for wood debris | from the source; the 300 mm floor a choice |
| An approach keeps the path's grade cap | the route's `grade_cap` | none: no approach grade for a bridge that is not accessible was found; S1: approaches climb a short way above high water and drain away from the deck | **a choice** |

## What the sources say the forms are for

- **Stepping stones** belong to a ford: shallow water a hiker would otherwise wade (S1), and wet
  flushes (S6: "use the biggest stones available"). No source gives a width limit.
- **A clapper bridge** only where big slabs are to hand (S6); each span short (Tarr Steps: 17 spans
  over 55 m), slabs one to two tonnes, piers about a metre high, and floods go over it.
- **A single foot log**: an 18 in log, hewn flat on top; long spans by the catalogue, short in
  practice.
- **Log stringers under a plank deck** past that, to about 9 m without an engineer (S6), 15 m for
  log girders (S4).

## Where the sources disagree

Freeboard: 0.5 to 1.0 m over design high water (S9, roads) against 0.9 m in S3's one worked example;
the solver takes S3's flood-stage rule. Postbridge's slab sizes differ by source (4 m by 2 m, or 13 ft
by 6.5 ft; the listing gives none), so the slabs are pinned to Tarr Steps' listing. Tarr Steps' length
is 55 m in its listing and 50 m on a secondary page.

## Where structure was traded for triangles

A bridge is one mesh of a few hundred to two thousand triangles at its finest (bridges.md,
"Budgets"): every member is a convex solid of 12 to 44 triangles, with no bark, no split ends and no
chinking, and the deck's planks are boxes with a jittered width. The research says what the members
are; the budget says each is a solid, not a carving.
