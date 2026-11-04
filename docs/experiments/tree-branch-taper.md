# Branch-aware broadleaf taper and junctions (G129)

Status: current refinement committed for requested integration without gates, 2026-11-04; validation follow-ups remain open.
Machine state: shared development machine; geometry counts only, no timing claim.

## Grounding

[Minamino and Tateno (2014)](https://journals.plos.org/plosone/article?id=10.1371/journal.pone.0093535)
compare area-preserving branching with biomechanical models and beech/fir measurements.
Area balance is useful, with departures depending on angle and lateral loading; it is neither
linear subtraction of radii nor an exact universal equality.
[Sone, Noguchi and Terashima (2005)](https://pubmed.ncbi.nlm.nih.gov/15519984/)
measured young Acer: daughter area approximately matched parent area, and branch area tracked
distal foliage, but annual area increments did not follow that simple equality.
The [allometry grounding](tree-allometry.md) explains the configurable exponent.

[Chomicki et al. (2017)](https://doi.org/10.1093/aob/mcx113) distinguish continued apical growth
from replacement by one or several lateral axes. A single dominant continuation is therefore
valid; two substantial outgoing branches are not mandatory. A strong continuation with a tiny
residual shoot should carry nearly all the incoming section. Species development and branch
history matter, so this is not a claim that all broadleaf crowns follow one architectural model.

[Liu et al. (2023)](https://doi.org/10.3390/plants12234060) distinguish a curved attachment,
collar, bark ridge and thickening below a junction. Their modeled Acer unions support treating
the whole transition as a shape, rather than inserting a cylinder. Their example dimensions
are not universal species constants. The owner's annotated mature-tree reference likewise
traces a substantial trunk through successive branching zones with broad, smooth limb bases.

## Growth and taper

The broadleaf and birch solvers share the correction, including profiles routed through them.
Willow, orchard, conifers and ancient-tree architectures retain their own rules.
The growth graph, bud count and leaf allocation remain independent of mesh budget.

Outgoing radius powers are removed above each lateral attachment, with simultaneous outlets
accounted for together. A five-percent radius reduction along an axis supplies the gentle taper
between junctions. This rate and the finite blend spans are authored bounds, not measurements.
A thick trunk is not forced down to twig size after its last scaffold. Its remaining support
passes to the existing upper scaffold(s), with the strongest one providing a continuous visible
path. The former unused leader reserve is apportioned to scaffolds, leaving the small terminal
shoot at species twig scale. Stems below 3 cm basal radius retain their fine axial tip; this
threshold is an authoring choice for compact young geometry, not a botanical age boundary.
No second recursive crown is added at the trunk top. Because twig classes were selected before
this redistribution, the terminal-radius upper bound includes the maximum scaffold gain
(1 / (1 - leader_share))^(1 / area_exponent); recursion and leaf counts are not increased.

The dominant continuation turns gradually from the incoming stem heading before its laterals
grow. Its distal crown follows the resulting offset; forcing a short connector back onto the old
branch ray caused an S-shaped correction and a visible kink. Thirty-two fixed definition steps
integrate the heading blend over up to eight initial branch radii (at most 40 percent of its
length). These are authored bounds, independent of the mesh budget.

The radius profile and attachment offsets are reconciled before meshing. The stem shoulder and
continuation sample that same grown centerline. The visible overlap also blends toward the
child's actual polygonal section, including bark relief: two independently shaped tubes on the
same centerline still produced a neck and subsequent swelling. Only the internal cap is inset,
with ten percent edge clearance, including coarse cross-sections. That cap shares the matching child ring's wind transform; the preceding fixed stem ring
bounds the
parent bend. This separates visible gradual taper from an invisible closure inside another tube.

## Large and small junctions

Branch bases start inside supporting wood and initially follow the parent's direction before
curving onto their own axes. Their frames are transported through that curve to avoid a sudden
cross-section/UV twist. The emergence has no added radius hump: the branch reaches its full
section while still buried, then follows its taper as it becomes exposed.

For reproducibility, the authored size blend is smoothstep(child radius / 8 cm). Across that
blend the buried offset grows from 0.75 to five times the smaller parent/child radius, and the
sweep length from two to eight child radii, bounded by 40 percent of branch length. The opening
radius is 0.35 to 0.6 times the smaller radius; it reaches the nominal section within the first
quarter of the sweep. These bounds express the requested larger sweeps on large limbs and
compact ones on twigs; they are not published botanical constants.

Up to four existing ring intervals sample a swept junction or terminal shoulder. The mesh
budget gains no reserved shoulder rings. Fine folded shoots keep their two-ring approximation.
Surfaces overlap inside supporting wood: this is not a welded manifold union. Bark ridges,
scars and actual damage are separate from this correction. Coarse rings approximate curvature.

## Root binding, caches and dependent content

Folded terminal wood can share the trunk's motion part and restart its UVs high in the crown.
Root collar extraction therefore finds the basal UV edge only among lower-trunk faces.
Swept branch motion pivots and weights use their actual curved paths. A fixed 64-step arc-length
map remaps wood, folded shoots, leaves and the shared cap consistently, independently of mesh
resolution. The existing motion-distance checks retain their tolerances.
This is cold generation: runtime vertex, part and GPU layouts are unchanged.
A scoped source-version salt invalidates affected trees and their baked LODs, while unrelated
architectures retain their keys. Deadwood and wood litter derived from these trees also recook.
The authored [tree-to-tree lean](../../content/test-scenes/deadwood/README.md#tree-to-tree-lean)
must be refitted against the final supporting mesh without changing its 1 cm contact tolerance.

## Review history and validation

The first area-debit prototype hid an open branch rim but still left a pointed trunk tail and
inserted-looking limbs. Native review rejected that form. The next curved-seat prototype added
a radius hump and a visible continuation collar; those were removed. The owner also rejected a
visible neck before the continuing branch. The revision keeps that section substantial and
fits its visible overlap to the child section while insetting only the hidden closure. A later
review found a short S-bend; growing the turn before its laterals replaces that connector.

The earlier prototype passed 127 Linux Clang debug CTest checks; its volumes were pruned.
Its Windows affected run was stopped when the owner requested the structural revision. Those
results do not validate the revised geometry. Current focused growth, taper, budget and
motion-distance validation: eight cases and 1,130,732 assertions passed. The first revised
wind run caught a coarse birch cap protrusion; the actual
polygon fit replaces the nominal circular inset without relaxing containment tolerances.
The wind containment ray targets the named scaffold shell: the cap itself and folded shoots
sharing its motion part are excluded, because those surfaces do not define that shell's parity.
A maple wind sample also hit one shared triangle edge twice (both at 2.91732 m); the ray now
counts coincident hits once, within sixteen float epsilons scaled by hit distance. Actual
section-clearance, wind-strength and bridge-shape tolerances are unchanged.
Both focused strong-wind cases pass on the current geometry (121,090 assertions combined;
the named-cap case rerun after the shared-edge ray fix). Native comparison: external
`branch-taper/review-continuation.html`. Final visual acceptance, remaining deterministic pins,
dependent contact fixture, Windows affected selection and matching Linux validation remain pending.
### Requested integration checkpoint (2026-11-04)

The owner requested merging the current solver work through the Forest POC integration chat,
skipping gates. This checkpoint preserves the current visual refinement without starting a new
refinement or full validation run. The integrating chat owns the shared merge and release build.
The author worktree must fast-forward to the resulting main before subsequent solver work.

Known incomplete baselines are explicit: only the default broadleaf geometry golden has been
updated to the final current mesh (`0x765d7bb485f8687d`). The named-profile aggregate, birch
single/clump geometry, impostor atlas, engine-content, deadwood and wood-litter golden values
still represent an earlier G129 prototype and require recapturing and cross-toolchain checks.
These are expected test failures, not passing current baselines. The authored deadwood lean
placement/capsule was fitted to that earlier prototype; its current contact needs rechecking
against the final mesh with the existing 1 cm tolerance. No tolerance was relaxed to hide this.
Full affected Windows and matching Linux checks have not run on this current geometry.
G129 remains in progress until these follow-ups are completed.