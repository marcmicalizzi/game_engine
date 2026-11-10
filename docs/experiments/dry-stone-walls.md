# Dry-stone walls: the grounding (roadmap G145)

**Status, 2026-11-10.** The sources a dry-stone wall in the engine is built from, the numbers chosen from them, and where a number is a design choice with no source behind it. The rules that follow from these numbers, and what the capability does with them, are on [walls](../subsystems/walls.md). The bar is the owner's: walls judged against photographs, and the proof of concept's stand-in (824 m of wall as 15,866 stone instances, "smooth lumps of one grey" at two metres) is what this has to beat.

## Sources

| Id | Source | Read as |
|---|---|---|
| **TCV** | A. Brooks and S. Adcock, *Dry Stone Walling: A Practical Conservation Handbook*, The Conservation Volunteers (formerly BTCV); the online edition at conservationhandbooks.com, chapters "Rebuilding a free standing wall" (General features, The foundation, Building, Throughs, Coping, Gapping), "Technical walling" (Walls on slopes, Coping on slopes), "Standard wall furniture" (Wall heads) and "Variations in walling" (Dimensions, Throughstones). Read 2026-11-10. | The handbook every UK walling course teaches from; the primary source here |
| **Post** | B. Post (DSWA-GB certified Master Craftsman, The Stone Trust), "Dry Stone Walls: Principles of structurally sound construction", *Masonry Magazine*, November 2018. | A second, independent statement of the section's numbers |
| DSWA | The Dry Stone Walling Association of Great Britain's own technical specifications (single-skin, double-skin and retaining walls), named by the North York Moors National Park's specification as the standard. | **Not read**: the two public copies found (the North York Moors and Peak District national parks' leaflets) refused an automated read (HTTP 403). A search summary of the Peak District leaflet gives throughs "at about 0.5 m and 1 m above ground, roughly 90 cm apart"; it is quoted here only as agreeing with TCV, and no number is pinned on it. Reading the DSWA specification itself is the first thing a later pass should do. |

The reference images are the owner's reels **r07**, **r53** and **r57** (Ticino gneiss hamlets in autumn: field and house walls of thin, flat-bedded grey slabs, tight courses, dark joints, moss on the tops) and **r01** (a pale dry-stone terrace wall at the top of a grazed slope). They are what the stone's shape and colour are judged against; no number is taken from them.

## How a wall is built (what the solver has to reproduce)

- **Two faces and hearting.** A field wall is two battered faces of stones laid with their length into the wall, the space between them packed by hand with small stones (hearting); the faces are tied by **through-stones** that cross the whole width, and the top is closed by a course across the full width and the **cope** (TCV, General features; Post). Laying a stone with its length along the face ("tracing") is "one of the primary reasons walls fail" (Post).
- **Courses graded by size.** "Stones are set level in roughly graded layers with the biggest at the bottom and the smallest under the coping" (TCV, General features); the biggest stones go in the foundation (TCV, The foundation). In the lower layers, steps between stones are "3″ (75 mm) or more", not small ones (TCV, Building).
- **Joints crossed.** The waller's rule is "one on two and two on one"; a **running joint** is a seam of "two, three or even four uncrossed joints" (TCV, Building). So a vertical joint may meet the bed of one course and stop: it crosses at most two courses.
- **Through-stones.** At "a maximum of 39″ (1 m) centres between the throughs in any row", rows "a maximum of 30″ (750 mm)" apart and staggered (TCV, Throughs); in a cattle wall the lower row is "at about 2′ (600 mm) height" (TCV, General features); throughs project from both faces "from 1″ (25 mm) up to 6″ (150 mm)" (TCV, Throughstones). One or two rows are normal, by the wall's height (ibid.).
- **The cope.** "Usually 10-12″ (250-300 mm)" high; each stone "on its thickest edge, centrally on the wall", vertical, packed tight with wedges (TCV, Coping).
- **Heads.** A wall head at a gateway is built of alternating **ties** (long stones across the whole wall) and **runners** (stones along it), is vertical "because they often form the side of a gateway", and is finished "with a large square topstone" (TCV, Wall heads).
- **On a slope.** "Lay the stones on the true horizontal, whatever the slope"; each footing is stepped "level both across and with the slope"; on a slope "each layer starts with a footing stone at one end and finishes with a topstone at the other" — the courses run out into the ground uphill and into the cope downhill, so the top follows the ground while every bed is level. Some wallers set their lines along the slope instead (TCV, Walls on slopes). Topstones on a slope are tilted uphill in most written sources, downhill in most practice, and vertical is "the most stable" but rare (TCV, Coping on slopes). No source found gives a gradient at which practice changes.
- **How one fails.** "Often the cause of a collapse is movement of the foundations"; "most collapses higher in a wall are caused by movement lower in the wall", announced by a bulge or loose face stones (TCV, Gapping). A gap is rebuilt from its footing with its fallen stone gathered "from alongside the gap" — the stone lies at the foot on one or both sides.

## The numbers chosen

The style modelled is a **double-faced field wall of flat-bedded metamorphic slab** (gneiss or slate): the reels' stone, and the one the brief asks for (flat-bedded, angular, split along bedding). TCV's slate walls are built "with few stones exceeding 6″ (150 mm) in thickness" (Dimensions), which bounds the course heights below.

| Quantity | Pinned | Range a test holds | Source |
|---|---|---|---|
| Height, ground to the cope's top | 1.30 m | 1.0 to 1.8 m | TCV: cattle wall "about 4′ (1.2 m)", sheep "at least 5′3″ (1.6 m)"; Post: 3 to 6 ft |
| Base width at the ground | 0.70 m | 0.60 to 0.90 m | Post: "28″ to 34″ is fairly typical" (0.71 to 0.86 m); TCV: base about half the height |
| Width under the cover | 0.40 m | 0.30 to 0.50 m | Post: "14″ to 18″" (0.36 to 0.46 m); TCV: the top about half the base |
| Batter, each face (run : rise) | 1 : 6.4 (from the two widths and the body's height) | 1 : 6 to 1 : 10 | Post: "the typical range for batter is from 1:6 to 1:10", 1:6 for irregular stone |
| Cope height | 0.27 m | 0.25 to 0.30 m | TCV: "usually 10-12″ (250-300 mm)" |
| Cope stone thickness along the wall | 50 to 120 mm | — | design: slab stone on edge; TCV gives none |
| Cope set | vertical | — | TCV, Coping ("ensure that the stone is vertical"); on slopes the stable choice of three (Coping on slopes) |
| Cover course (across the full width under the cope) | 60 to 90 mm thick, 20 to 40 mm proud of each face | — | design: the brief's "cover band"; no source read gives its numbers |
| Course height at the foot / under the cover | 150 mm / 60 mm, linear in height, each stone ±25 % | a course stone 30 to 190 mm; a jumper (a stone brought up level with a step beside it, two courses) to 280 mm | TCV graded layers; slate's 150 mm |
| Face length of a stone | 1.5 to 4 times its height, at most 0.60 m | — | design: slab proportions; length into the wall at least the face length (TCV, Post: no tracing) |
| Running joints | none crossing more than two courses | measured: 1.6 % of joints cross three, 0.6 % four (the solver's residue, [walls](../subsystems/walls.md#the-module)) | TCV, Building |
| Through-stones | one row, bed 0.52 to 0.60 m above the ground where the faces allow (to 0.75 m where they already stand higher), at most 0.80 m apart before a run's stretch (0.75 m in a 3 m module, under 0.89 m stretched), projecting 30 to 100 mm | bed 0.45 to 0.75 m, centres ≤ 1.0 m | TCV (≤ 1 m centres, lower row at about 0.6 m, 25 to 150 mm projection) |
| Footing | the foot course set 100 mm below the ground, 50 to 75 mm proud of each face | — | TCV, Dimensions: a Scottish scarcement "usually 2-3″ (50-75 mm)"; the depth a design choice (TCV gives none; Post's 6-18″ is a crushed-stone bed, not a footing) |
| Slope | courses level and stepped up to a grade of 0.5 (1 in 2); the top follows the ground | grades 0 to 0.5 | TCV, Walls on slopes; the limit a design choice (no source gives one) |
| Gateway | two heads, vertical, ties and runners alternating, a large square topstone; 3.0 m clear by default | two heads | TCV, Wall heads; the width a design choice |
| Ruined stretch | fallen to the footing, its stone lying at the foot on both sides, within 1.5 m of the face | the asked fraction of the line's stations | TCV, Gapping |
| Stone per metre | 1 to 2 t | the solved module's stone volume, as a check | TCV, The foundation: "between one and two tonnes of stone in every metre length of wall" |

## Where structure was traded for triangles

- **The hearting is not geometry.** It is a dark closed core behind the faces, so a gap between face stones shows shadow and not the field behind. A hearting stone is never seen in a standing wall.
- **A stone is a split block, not a sculpted rock**: eight corners jittered within its beds (flat to 5 mm) and its face (proud or sunk up to 15 mm), and a face split into four planes about a proud point, 14 triangles a stone. Pinnings (the small stones a waller knocks into a face's gaps) are the same at a tenth of the size. What a stone does not have: chipped arrises, lichen, moss and a wet foot (the cover solver's, roadmap G76).
- **The courses are the module's, not the world's.** A module is solved with level beds at its own grade class, and the next module along the line is solved independently, so the beds of two modules do not continue into each other across their joint; the stones of the two overlap by up to 120 mm there, so the joint is not a straight seam. A wall laid by one waller from one end has courses that run on.
- **Stone colour is a palette, not a photograph**: every stone takes a swatch of a small generated texture (grey, blue-grey, brown-grey and weathered light slabs, with a texel's mottling), so the stones of one wall differ from each other as the reels' do. A kit of photographed stones is an asset set (the solver-needs report's section 4); what it would need is listed on [walls](../subsystems/walls.md#a-photographed-kit).

## What the proof of concept measured, and what this must beat

| | Proof of concept (v40) | This row's target |
|---|---|---|
| A hundred metres of wall | about 1,900 stone instances | tens of instances (a module a few metres long) |
| Traced structures | one a stone: the chain dropped 1,491 drawn instances at 1920 × 1080 with a 1 GiB budget | one a module |
| At ten metres | the closest thing in the world to the references | as good |
| At two metres | "smooth lumps of one grey" | flat-bedded, angular slabs of differing greys, with pinnings |

The measured numbers are on [walls](../subsystems/walls.md#budgets).

## What the first pictures show (2026-11-10)

A scratch scene (a 80 × 50 m field on the renderer's waves, grades to about 1 in 12, a gateway, a lane wall crossing a tile border, and a second layer ruined by half), drawn by `engine-view` in a streamed world, beside the reels r07, r53, r57 and r01. What reads: flat-bedded slabs in level courses with crossed joints, stones of differing greys, a ragged cope on edge, a vertical head at the gateway, a broken stretch stepping down to a fallen one with its stone spread at both feet, and no seam where the lane wall crosses a tile's border. What a person would still object to beside the photographs: every stone is a split block with straight arrises, not a weathered slab with chipped edges, so at two metres the face reads as a stack of boxes; the faces are too even in depth (the reels' gneiss faces are flush and tight, with thin dark joints, where these stones sit proud by up to 1.5 cm each); the cope reads a little like books on a shelf; a head's end shows the dark hearting in places where a course has neither tie nor runners; nothing is wet, mossed or lichened and the grass does not grow to the wall's foot; and at a hundred metres the outline form reads darker than the near wall.