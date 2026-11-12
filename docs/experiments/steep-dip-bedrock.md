# Bedrock on steeply dipping beds (G200)

What exposed bedrock looks like when its beds dip steeply, and why, as the rules the `ledges`
generator and the `cliffs` faces take for each attitude of the beds to the slope
([rocks](../subsystems/rocks.md#beds-at-any-attitude)). It continues
[exposed-bedrock.md](exposed-bedrock.md), whose rules were written for beds dipping 20 degrees or
less. The question came from the forest proof of concept, whose formation dips 57 degrees: on a
steep dip the generators either did nothing useful or drew the wrong mountain.

**How the sources were read.** Several of the primary texts could not be opened from here (the
publishers' pages refused an automated reader, and a PDF could not be rendered). Each source below
says how far it was read: *opened* (the page or the article read), *abstract*, *snippet* (a search
result's summary only), or *not opened* (known by citation, not read). Anything without a source
is marked as the engine's design.

## The landforms, as the dip rises

**Cuesta, homoclinal ridge, hogback, flatiron.** A resistant bed over a weak one makes an
asymmetric ridge: a long **dip slope** on the resistant bed's top and a short, steep **scarp** cut
across the bed edges. As the dip rises the two flanks come to the same steepness and the ridge is
a **hogback**; where streams cross a hogback at intervals the resistant bed's dip slope is cut into
**flatirons**, triangular facets wide at the base and pointed above, the bedding plane their face.

| Landform | Dip | Source | How far read |
|---|---|---|---|
| cuesta | under 20 degrees ("escarpment slopes much steeper than the dip slopes") | MIT courseware, PGT 7.4.1 "Hogback" | opened |
| homoclinal ridge | 10 to 30 degrees | Wikipedia "Homoclinal ridge", citing Simonett 1968, Huggett 2011, Thornbury 1954 | opened (a lead; the textbooks not opened) |
| hogback | over 20 degrees; "the dip slope is approximately equal to the escarpment slope" | MIT PGT 7.4.1 | opened |
| hogback | over 30 to 40 degrees, "steep slopes of nearly equal inclination on both flanks" | Wikipedia "Hogback", citing Huggett 2011, Cotton 1952, Fairbridge 1968 | opened (a lead) |
| flatiron | caprock about 2 m thick, rarely 5 m; on a dip slope cut by regularly spaced streams | Wikipedia "Flatiron", citing Schmidt 1989, Ollier 1985 | opened (a lead; Schmidt not opened) |
| Boulder's Flatirons | 50 to 60 degrees east | secondary pages | snippet |

The thresholds disagree by 20 degrees between sources, and one says outright that they are
arbitrary: the forms grade into one another. **So the engine has no case boundaries at these
dips.** It blends by angle (below).

**The hogback's weak flank is a ramp of blocks.** Glade and Anderson (2018, *JGR Earth Surface*,
doi:10.1002/2017JF004466; abstract) and Glade, Anderson and Tucker (2017, *Geology*; secondary, a
university magazine's account, opened): the slope in the weak rock under a resistant layer is a
steep, linear-to-concave ramp mantled with blocks shed by the resistant layer; bigger blocks make
steeper ground. No numbers were reached.

## The dip slope: slabs and their overlaps

Where erosion strips the weak bed off a resistant one, the resistant bed's top is the ground: a
**bedding-plane slab** (University of Wisconsin-Green Bay geology pages on flatirons, opened;
University of North Texas course review, opened). Where several resistant beds are stacked, each
slab ends down-dip at its own edge and the next bed's top continues below it: the slope is
**stepped**, each step a bed thick. *No source opened gives the step; it is the geometry of the
beds, below.*

**The edge of a slab is a joint normal to its bedding.** Joints in a bedded rock are commonly
normal to the bedding and spaced about as far apart as the bed is thick (below), so a slab's
down-dip edge is a face square to the slab: on beds dipping `d` out of the slope it **overhangs
by `d`** from the vertical, its top edge out past its foot. That is the "stepped overlap" of a dip
slope, a shingle's edge. *Geometry, from the joints' attitude.*

**A slab steeper than the soil holds is bare.** The soil mantle is whole only under about 30
degrees and none is held past 45 ([exposed-bedrock.md](exposed-bedrock.md#where-rock-shows),
DiBiase, Heimsath and Whipple 2012). A tread is in its bedding plane, so on beds dipping past 30
degrees a tread is a plane no soil stays on: **rock, not turf.**

**Plane sliding.** Where the beds dip out of a slope less steeply than the slope (they
*daylight*) and more steeply than the joints' friction angle (about 30 degrees), with the dip
direction within 20 to 30 degrees of the slope's, slabs slide on their bedding planes (Rocscience
*Dips* documentation, "Planar Sliding", after Goodman 1980 and Hudson and Harrison 1997, opened;
Hoek and Bray's 20 degrees, snippet only).

## The scarp side: stacked bed edges, and toppling

**A scarp is the bed edges, stacked.** A resistant member stands as a cliff, the weak member under
it lies back as a ramp, and the weak member's retreat undercuts the resistant one, which sheds
blocks (the university magazine account above; the UNT review, opened; Duszynski, Migon and
Strzelecki 2019, *Earth-Science Reviews*, "Escarpment retreat in sedimentary tablelands and cuesta
landscapes", snippet). The engine's ledges and benches already do this for shallow dips
([exposed-bedrock.md](exposed-bedrock.md#how-layered-rock-makes-steps)).

**On a gentle dip into the slope, a bed's edge leans back.** A riser cut by the joints normal to
the bedding, on beds dipping `d` into the face, is battered back by `d`: a cuesta's scarp of beds
dipping 25 degrees into the hill stands at about 65 degrees. *Geometry.*

**Past a threshold the beds topple.** Goodman and Bray (1976, "Toppling of rock slopes"; not
opened, its condition as the Rocscience *Dips* documentation implements it, "Flexural Toppling",
opened): beds dipping steeply into a face topple when the normal to the beds is inclined less
steeply than a line the friction angle above the face, that is when

`(90 - dip) + friction <= face angle`,

with their strike within about 10 to 30 degrees of the face's. Checked against Zheng et al.
(2021, *Frontiers in Earth Science*, doi:10.3389/feart.2021.773088, opened, text sections): beds at
78 degrees in a 63 degree face have a critical friction angle of 51 degrees, which is
`63 - (90 - 78)`. **For a ledge's riser, whose face is near vertical, and a friction angle of 30
degrees, beds dipping more than 30 degrees into it topple.** What toppling makes: the beds lean
out over their feet, bent or rotated about their bases (flexural and block toppling, Goodman and
Bray's two primary classes; snippet), with tension cracks behind and blocks at the foot.

**A block on its base topples when it is narrower than the tangent of its base's dip is tall.**
Hoek and Bray's chart, as Wyllie and Mah give it (the Itasca *3DEC* example "Sliding and Toppling
Blocks", opened; its four cases agree with the rule): a block of width `b` and height `h` on a base
dipping `psi` topples when `b / h < tan psi` and slides when `psi` passes the friction angle. A
bed's joint block on a joint normal to its bedding has its base dipping `90 - dip`: at 57 degrees
into the slope that is 33 degrees, past the friction angle, so the block also slides.

**Wedges.** Two joint sets whose line of intersection daylights in the face and plunges past the
friction angle release wedges (Maptek *PointStudio* help, opened; no numbers). The engine does not
model a second joint set's wedges; see "What is not done".

## Along the strike: ribs and gullies

Where the strike runs up a slope, the resistant members stand as **ribs** along it and the weak
members between them are cut into **strike valleys and gullies**: the main streams follow the weak
beds and short tributaries drain the ribs at about right angles, a trellis (the UNT review,
opened; Wikipedia "Flatiron", a lead). *Not reached in an opened source*: on near-vertical beds a
rib narrows to the resistant member's outcrop width, `thickness / sin(dip)` across the strike, and
its crest is serrated where cross-joints and gullies cut it. That is geometry and the engine takes
it as such.

## Joints normal to bedding, and their spacing

| Rule | Numbers | Source | How far read |
|---|---|---|---|
| spacing proportional to the bed's thickness | thickness over spacing about 1.3 (spacing 0.77 of the thickness) in the Monterey Formation; log-normal about the median | Narr and Suppe 1991, *J. Struct. Geol.* 13 | snippet (the abstract via an aggregator) |
| saturation | spacing to thickness 0.8 to 1.2, critical about 1.0 | Bai and Pollard 2000, *J. Struct. Geol.* 22 | abstract |
| proportional; Gamma-distributed | no ratio | Huang and Angelier 1989, *Geol. Mag.* 126 | abstract |
| the field's spread | under 0.1 to over 10; past about 1.2 unsaturated | Tang et al. 2008 and others | snippet |

The ledges keep `joint_ratio` 1.3 ([exposed-bedrock.md](exposed-bedrock.md#how-layered-rock-makes-steps)).

## What the engine takes, by attitude

The beds' attitude is measured **against the face a ledge stands as**, not against a fixed dip:
the apparent dip `a` of the beds along the outward normal of the bed's own edge on the ground (the
direction in which the bed coordinate falls along the surface), positive where the beds dip out of
that face and negative where they dip into it. One rule covers every aspect of a spur, because the
normal turns with the ground and the blends are functions of angles.

| Attitude | `a` | What the ledge's front is | Source |
|---|---|---|---|
| any, gentle | `|a|` under 20 | plumb, as G142 built it (a cuesta's scarp; G142's pins) | the cuesta range above |
| beds dipping out of the face (a dip slope's slab edge) | 20 to 90 | the joint normal to bedding: overhangs by `a`, a shingle's edge, ramped in over 20 to 30 | geometry of the joints |
| beds dipping into the face, under the toppling threshold | -20 to -30 | the joint normal to bedding: battered back by `|a|`, ramped in over 20 to 30 | geometry of the joints |
| beds dipping into the face, past it | -30 to -90 | toppled: leans out along its own bedding by `90 - |a|`, never more than 25 degrees, crossing over 30 to 40 | Goodman and Bray 1976 with a vertical face and a friction angle of 30 |
| strike up the slope | the edge's normal is along the contour | the same rules on the edge's own normal: ribs on the resistant members, their faces across the slope | the trellis above |

Every number in the table is one of these: the 20 degrees is the cuesta's upper dip in the
opened source; the 30 is the friction angle every source above uses for rock joints; 25 degrees
of lean is the engine's (a toppled bed leaning further reads as a diving board, and the blocks
that would lean further are the ones that have fallen). Where a front's foot would leave the
ground because the face leans back over falling ground, the lean is held to what keeps its foot
on the ground (`batter * fall <= 0.6`, the engine's).

**A tread's rock.** A tread steeper than the rule's soil slope (30 degrees) is drawn in its unit's
rock, not the soil: a dip slope's slabs are bare.

**A face's members.** On a cliff face cut through beds dipping past 20 degrees, the formation's
weak members stand back from its resistant ones by their contrast in resistance, so a face across
the strike shows its members as ribs and gullies and a face along a dip shows them as banded
recesses (ramped in over 20 to 35 degrees of dip; under it the benches do this, G172).

## Measured

The tables are on the subsystem page, where the rules are: what the generators did on a steep dip
before and after ([rocks](../subsystems/rocks.md#beds-at-any-attitude)) and what a massif costs
([rocks](../subsystems/rocks.md#a-massif-inside-a-budget)). The pictures and the frame cost are here.

**The frame's cost** (`content/test-scenes/steep-dip/`, its camera path flown once, 3840 x 2160,
`--raster rt --shadows rt`, `renderer.gi.mode=bounce`, RTX 5090, 2026-11-12; main's release
binaries copied out of the tree against this branch's msvc-release, in one lease, alternating;
the machine was not quiet, 78 to 93 % of the processors busy with other work, so every figure is
an upper bound, GPU times from the engine's own timers, medians over the path):

| | main | this branch | main again | this branch again |
|---|---|---|---|---|
| frame | 4.60 ms (p95 8.80) | 4.47 (6.39) | 4.81 (13.66) | 4.50 (6.36) |
| resolve | 1.89 | 1.96 | 1.90 | 1.98 |
| traced primaries | 1.34 | 1.26 | 1.34 | 1.27 |
| the bounce's rays | 0.42 | 0.40 | 0.41 | 0.40 |
| bottom- and top-level builds | 0.090, 0.077 | 0.090, 0.077 | 0.089, 0.077 | 0.089, 0.079 |

`--raster hw`, the same flags: 3.21 ms on main against 3.16 here (resolve 2.05 against 1.99). The
scene's clusters are 52,862 on main and 52,808 here. The change is content, not a pass: the
treads past the soil slope change material (rock for turf, about 0.07 ms more resolve on this
path, within the pairs' spread) and the faces' members move vertices, so there is no cost to
claim back and none was found.

**The pictures** (the same scene and settings at 1920 x 1080, `load.materials` the worktree's
`content/materials` with the generated sets only, so the gneiss draws its flat colour; main's
binaries and this branch's, one checkout's caches each). The reels that show steeply bedded
mountains are r21 (a jagged grey limestone ridge whose beds stand as near-vertical slabby faces
and ribs over a 25 to 35 degree slope) and r22's tiles 9 and 12 (a tilted slab peak, strata
dipping about 30 degrees, its dip slope one bedding plane and its scarp stacked bed edges).
Beside them, plainly:

- **From a kilometre** the mountain is still the forest's pale banded cone: the faces are a grey
  hood in horizontal 12 m tiers over the steep band, and the crags are dark patches at its foot.
  Nothing at this distance says the beds dip 57 degrees; r21's ridge is all diagonal ribs and
  slabs from the crest down. The tiers' horizontal seams are what reads, and they are levels by
  design on a steep dip (the faces carve the members, but within a tier).
- **From 100 m** a member's gully shows in the lowest tier where the schist crosses it (the face
  stands back and the ground shows in the notch), and on the flank whose strike runs up the slope
  the crags carry thin ribs running up it. The crags themselves read as flat grey skins on the
  ground with holes, not as rock standing out of it.
- **From 10 to 35 m** at eye height the views that were aimed at the ledges' outer band found the
  faces' feet and the ground, and one crag with a single leaning fin: the ledges cover 12 to 30 %
  of the 35 to 48 degree ground in patches, and from below a step of 0.5 to 2 m on a 45 degree
  slope is hidden by the slope in front of it. r22's slabs are tens of metres wide; ours are a
  metre or two.
- What a person would still object to: the horizontal tiers; flat-coloured rock; crags as skins;
  the steps too small for the mountain (a 57 degree gneiss stands in ribs metres to tens of metres
  high, the ledges cap a step at 4 m and the exposure rule stands it to a fraction of that).

## What the forest's authors found on their cirque (v42), and what this row took

Their part (read as text) tried both generators on the cirque's foot and on a 31 to 37 degree
stream slope. Of what they wrote down, this row took the dip (the faces read as horizontal
shelving because the tiers' breaks and the benches need 20 degrees or less: the members now carve
the faces, and the ledges' fronts, treads and leans follow the beds) and the cost (every recipe of
a region built at load: the far form by ring, and the case for faces arriving with their tiles).
It left, as rows: the profile walk's 200 m limit (a 900 m wall is dressed for its lowest 250 m);
tiers cut shorter than their two tapers on a rippled wall, which stand as triangular blades (27 %
of faces at a 4 m trace); a tier's top sampling the set stretched along the ledge; the talus skirt
on a 35 to 50 degree foot; and the ledges' little rock on 30 to 40 degree ground, which is the
cited rule's answer and not a defect of the attitude.

## What is not done

- **Toppled blocks are not drawn at the foot.** A block that topples is a gap in its ledge, handed
  over in `ledge_gaps` with the others; a deposit lays it (G143's).
- **No second joint set, so no wedges**, and no gully cut into the ground on a weak member: the
  ledges cannot lower the terrain. A weak member between two ribs is the ground's own surface.
- **A flexural topple's bend** (beds curving over near the crest) is a lean, not a curve.
