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
| S10 | Devon County Council, *Bridges Guidance* (land drainage consent; devon.gov.uk/floodriskmanagement/land-drainage-consent/bridges-guidance/, undated) | full text of the page (G186) |
| S11 | USDA Forest Service, *Sustainable Trail Bridge Design* (pub. 2023-2805P), "Siting", "User Barriers", "Abutments" | full text of the PDF, extracted (G186) |
| S12 | Exmoor Historic Environment Record, Tarr Steps scheduled monument DEM5398 | full text (G186) |

Not reached: the SCA's *Lightly on the Land*, the print edition of the TCV's handbook, FSH 7709.56b
itself (403 again in G186's pass), Paths for All's *Path Bridges* (2006, the successor of the
Countryside Commission for Scotland's *Footbridges in the Countryside*: its sheets on flood levels
and abutments would be the best source; only a copy behind a reader was found), Standards New
Zealand's SNZ HB 8630:2004 (an outline only), Historic England's Postbridge entry (403). **No
published gauge of a stepping stone's spacing, size or height over the water was found** in any of
them.

## The deck's height, by form and span

G186. G147 took one rule for every timber form — the underside over bankfull by a bankfull depth, at
least 300 mm — from S3's figure 13, and a foot log over a 0.6 to 0.8 m brook stood about a metre
over its banks on earth humps (G147's pictures; reel r19's crossings sit almost at the bank). A
second reading (G186, one research pass over published pages) found that the figure is not a
clearance rule:

- **S3, page 03**: the maximum riffle bankfull depth, doubled, *estimates* the 50 to 100 year flood
  stage over the bed. It is a way to find a flood level, and putting a deck's underside on it is
  that flood with no freeboard at all — stricter than any rule the pass found for a small crossing.
- **S11, "Siting"**: a trail bridge in general is sited to pass the 100-year flood with debris
  clearance "as necessary" (no height given; it points to FSH 7709.56b §62.2, not reached), and a
  critical one a foot more than a road bridge. **"User Barriers"**: single-log bridges and minor
  bridges "close to the ground" need no rails — the source's own picture of a small crossing is one
  close to the ground.
- **S10**: for a path footbridge, the soffit **about 300 mm above the bank tops**; the 600 mm over
  the design flood that road and rail keep is relaxed on small watercourses where the flood level
  is unknown, **provided the soffit stays above bank level**; and a structure raised above flood
  level on ramps "could not be reached during a flood in any case", while long embankments across
  a floodplain "may cause more problems than the bridge itself".
- **S12**: Tarr Steps' piers stand about 1 m over the bed (and the slabs at about the banks'
  level, as G147 had): a clapper is overtopped.
- **S11, "minor trail bridge"**: a wood bridge of 20 ft (6.1 m) clear span or less — a class for
  inspection, not for a clearance, but the one size threshold the sources draw for a small timber
  bridge.

So the rule (`plan_timber`, `k_bank_clearance_mm`, `k_minor_span_mm`):

| Form | Where the underside stands | Source |
|---|---|---|
| Stepping stones | each top 150 mm over the water at the scene's stage (unchanged) | a choice (above) |
| Clapper | the slabs on the banks' level, overtopped in a flood (unchanged) | S7, S12 |
| Foot log, and stringers of a clear span to 6.1 m | **300 mm over the lower bank's top**, and on or over the ground at both sills | S10 (the number), S11 (the class and its span) |
| Stringers past 6.1 m | over the flood stage S3 estimates: bankfull plus a bankfull depth, at least 300 mm (G147's rule, kept where it belongs) | S3, S11 |

The span is the clear span between the sills' inner faces (`BridgePlan::span`). The threshold is
the sources' one span for a minor timber bridge, used as the line between "sits at the bank" and
"keeps a flood clearance": **a choice**, said so, since no source draws that line by span.

## The approaches: steps, then a short fill

G186. S10 against embankments, S11's deck grade (at most 5 %, about 2 % for drainage, straight approaches
of 25 ft) and its backwalls that hold an approach's fill at the stringers' ends, and S1's fords
approached "a short distance above the typical high water line" are what the pass found; **no
source gives a footbridge's approach ramp, its length or a step's rise**. What the solver builds:

- From the deck's end out along the path, **stone steps on the ground** until the deck is reached:
  each riser at most `k_riser_mm` (200 mm) and the risers equal, each step `k_step_going_mm`
  (300 mm) along the path, bedded 100 mm, at most `k_max_steps` (4) steps an end. Choices: the
  numbers of an outdoor step (a riser near the 150 to 220 mm of a stair, a going a foot long), not
  a footbridge source's.
- Past four steps (a deck still more than 800 mm over the ground there: a stringer bridge past the
  minor span, or a bank that falls away), the rest is a `Fill` from the last step's outer edge at
  the route's grade, as G147's approaches were, a metre lower and 1.2 m further out.

So a foot log over a 3 m brook is reached by two to four stones laid at the bank, and no earthwork,
and the channel's cut cannot notch an approach that is not there (bridges.md, "The ground and the
neighbours").

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
| Freeboard of a timber bridge **past the minor span** (G186: only there) | the bankfull depth over bankfull, at least `k_min_freeboard_mm` 300 mm | S3: the 50 to 100 year flood stands about twice the bankfull depth over the bed (its stated rule of thumb: an estimate of the flood, not a clearance), so a deck over it clears bankfull by one bankfull depth; S9: 0.5 to 1.0 m over the design high water plus debris (roads); S3's example 0.9 m for wood debris | from the source; the 300 mm floor a choice |
| A small timber bridge's underside over the bank, `k_bank_clearance_mm` (G186) | 300 mm over the lower bank's top | S10: a path footbridge's soffit about 300 mm above the bank tops; on a small watercourse the flood clearance is relaxed so long as the soffit is above bank level | from the source |
| The minor span, `k_minor_span_mm` (G186) | 6.1 m clear | S11: a minor trail bridge is a wood bridge of 20 ft clear span or less (an inspection class) | the number from the source; **its use as the line a choice** |
| An approach's steps: `k_riser_mm`, `k_step_going_mm`, `k_max_steps` (G186) | 200 mm, 300 mm, 4 an end | none: no footbridge source gives a step | **choices** |
| An approach keeps the path's grade cap | the route's `grade_cap` | none: no approach grade for a bridge that is not accessible was found; S1: approaches climb a short way above high water and drain away from the deck; S11: a deck's grade at most 5 % | **a choice** |

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
the solver takes S3's flood-stage rule past the minor span only (G186). For a small crossing the
sources pull apart: S11 asks every trail bridge to pass the 100-year flood with debris clearance,
yet exempts single logs and minor bridges "close to the ground" from rails, and S1 has foot logs
close to the ground; S10's path footbridge stands 300 mm over the banks and gives up the flood
clearance on small watercourses. The solver follows S10 below 6.1 m. Postbridge's slab sizes differ by source (4 m by 2 m, or 13 ft
by 6.5 ft; the listing gives none), so the slabs are pinned to Tarr Steps' listing. Tarr Steps' length
is 55 m in its listing and 50 m on a secondary page.

## Where structure was traded for triangles

A bridge is one mesh of a few hundred to two thousand triangles at its finest (bridges.md,
"Budgets"): every member is a convex solid of 12 to 44 triangles, with no bark, no split ends and no
chinking, and the deck's planks are boxes with a jittered width. The research says what the members
are; the budget says each is a solid, not a carving.
