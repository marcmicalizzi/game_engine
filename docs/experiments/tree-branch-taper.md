# Branch-aware broadleaf taper and junctions (G129)

Status: reviewed upper-crown follow-up prepared for approved integration, 2026-11-04; broad validation remains with the coordinator.
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

## Integrated checkpoint: growth and taper

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
## Upper-crown allocation follow-up (2026-11-04, in progress)

The Forest specimen `habitat-v14-tree-017888` uses `beech-old`, seed 10408,
260 years, 26 m height and 0.66 m basal radius. The first refinement promoted the
highest lateral to a continuation even when its orientation and crown were lateral.
Equal initial scaffold radii also made short upper branches disproportionately thick.

The follow-up gives the remaining original stem path an explicit leader role, before
recursion grows. Only that role can receive the stem's curved mesh continuation; being
last or thickest is insufficient. Lateral and leader support share the existing named
crown allocation, weighted by squared axis length as a projected-crown-area proxy.
Species reach, layering, angle and maturity rules still supply those axes. This proxy
is authored, not a measured universal length/diameter law: Sone et al. support a link
between branch section and distal foliage, while Minamino and Tateno show why loading
and angle prevent exact universal pipe equality. Neither establishes a height-only
percentage cap. This implementation does not simulate light competition or cambial growth.

The first follow-up weighted the upper leader only by its remaining length. The owner identified
the resulting rapid shoulder taper in a native close-up: the requested junction is an asymmetric
V, with a substantial continuing stem beside the lateral. The allocator therefore retains at
least the existing species `leader_share` in radius-power units at the final fork. For paired
outlets this considers their combined demand. It redistributes the same budget; it does not
inflate the trunk or claim a universal botanical diameter cap. This architectural floor is an
authored use of the species' existing apical-allocation control.

The shoulder also used a concentric reduction, giving the exposed outline a bottle-neck shape.
The continuing axis now shifts away from the final lateral(s) across its existing smooth sweep.
The offset is the difference between the incoming fork radius and continuing radius, directed
opposite the radius-power-weighted lateral directions; opposing outlets cancel. This preserves
the outer bark line approximately while the inside forms the crotch of an asymmetric V. The
smoothstep displacement has zero derivative at both ends and is applied before descendant
shoots grow, so descendants follow the resulting axis. This is an authored geometric model of
the curved union, not a measured cambial-growth law. It adds no mesh rings or growth nodes.

The upper leader's recursive shoots use its own remaining length and allocated radius,
not the full trunk's dimensions. It is funded from the same initial radius-power sum
as the lateral crowns, so this does not add a second full-budget crown at the top.
Existing fine sapling axes (below the existing 3 cm radius boundary) retain their paths.
One named upper-leader tube per mature stem is accounted for by the existing budget
ladder. Both normal recursion and progressive area debit continue down to twig scale.
Shared sweep, visible cross-section matching and strong-wind binding remain in use.
A lateral at the trimmed stem endpoint retains its radial seat; endpoint position alone no
longer makes a named lateral concentric with the continuing trunk.
The scoped branch-taper cache version advances; unrelated architectures keep their keys.

Pending: final current golden values, dependent contact validation, reduced-LOD visual review
and full affected Windows / matching Linux validation.

### Upper-crown review evidence

Reviewed integration revision: Windows affected build passed; focused flora taper,
maturity and allometry checks passed 22 cases / 572,880 assertions. The two renderer wind
attachment cases passed 130,882 assertions, including coarser budget rungs. Lint and docs checks
passed. Native beech close-ups from two sides, whole foliage, maple/birch wood and a maple
sapling are under `D:/workspace/game_engine_local/tree-growth/2026-11-04/upper-crown/review.html`.
The 26 m beech uses 421,432 triangles at its million-triangle cap, versus 453,316 at the prior
checkpoint; its 8,000 budget solve uses 4,264 triangles. These are geometry counts on the shared
machine, not performance measurements. The full affected selection, Linux and the previously
listed goldens/contact follow-ups remain pending; these focused results do not replace them.
### Approved upper-crown integration checkpoint (2026-11-04)

The owner approved this reviewed offset-V revision for the Forest coordinator to merge and
rebuild. The author branch was verified against current main `e67b2906` before packaging; no
fast-forward was needed and no shared checkout or release build was changed by the author.
Broad suites and merge gates remain deferred to the coordinator, as requested.

The focused checks above are the evidence for this revision. Full affected Windows and matching
Linux runs have not been performed for it. This new geometry also supersedes the previous
checkpoint's default broadleaf golden and part-count expectation; current broadleaf/birch and
named-profile geometry, baked LOD and dependent content/deadwood/litter baselines need updating
and cross-toolchain verification. The earlier authored deadwood contact fixture still needs
validation at its existing 1 cm tolerance. No golden or contact tolerance was relaxed here.
G129 remains in progress until those recorded follow-ups close.
### Baselines recaptured (G133, 2026-11-05)

The gate on `main` at 4adc7aaa failed eleven tests on this geometry; G133 compared each against
the same test built at 86d76acd (before G124–G129) on MSVC debug. Every golden that moved names a
broadleaf-architecture tree or what is cut from one, and nothing else moved: the six conifer
profiles, the spruce impostor chain, the spruce deadwood container, the ancient and pine content
containers and the four deadwood-free litter kinds hash as they did. Recaptured: the default
broadleaf (220 parts, 49,136 triangles; [flora](../subsystems/flora.md#the-broadleaf) has why the
parts rose and what they cost), the named-profile aggregate (maple, oak, elm, walnut, beech, ash,
aspen and alder moved), the small oak's four bake hashes, the birch single and clump, the oak
deadwood container, the twig and tangle litter (the 0.35 m oak the twig cuts its log from has the
axial terminal twig and a 0.245 m trunk part where it had 0.225 m: 2,625 → 1,351 triangles), and
the content build's broadleaf container. The birch's full-detail LAI floor moved from the old
solver's 2.9 to the measured stands' 2.5 (it reads 2.79). The authored lean was refitted (below).
Not resolved: the content build's young broadleaf (6 m, LAI 4 asked, 30,000) realises LAI 2.85
where it realised 3.22, under its "within a fifth" bound and under the 3–7 of closed broadleaf
forest; the leader fix's length-weighted radii leave it 312 twigs (320 before) under a hull 9%
wider, and the spray's cards, already more than its twigs hold at the leaf's spacing, share the
shorter twig length. That is a solver question for this row, not a bound to move (answered
below, "The young broadleaf's leaf area").

The authored lean's contact was 50 mm off the final support trunk (and the solver refused the
pose). Its placement and capsule were refitted to the final mesh by the same method, the 1 cm
tolerance and the damaged tree unchanged: the standing oak is translated to z = 5.11449 m (from
5.22519) so the nearest leaning-wood sample sits 3.0 mm from the trunk's actual section at
y = 3.459 m, and the capsule is that section's centre (0.40466, 4.91408) with radius 0.29473 m,
the section's reach towards the contact less the 3 mm.

### The young broadleaf's leaf area (G129 follow-up, 2026-11-07)

G133's diagnosis held on measurement. The default broadleaf's twig is 0.7 m (0.595–0.805 with the
recursion's jitter) and its card is a 0.24 m leaf 0.12 m wide at 50°, laid
`card_step` = 0.12 / sin 50° = 0.157 m apart, so a twig holds four cards a side (three on the
shortest). The leaf area index asks for 3,794 cards on the 6 m tree's 27.33 m² footprint — 1,897 a
side over 312 twigs, six each — where the twigs hold 1,230 a side; the spray spread its six over
the whole twig, every card came out about two-thirds of its leaf, and the 3,794 cards made 78.0 m²
of leaf where 109 were asked. Before the leader fix the tree had 321 twigs under a 24.89 m² hull
and realised 3.24; the fix's wider hull asked for 10 % more cards on 3 % fewer twigs. The leader's
share was not what starved the twigs (it moved them by nine), so the answer is not in the
allocation but in the spray: a twig takes no more cards than its length holds at the leaf's spacing,
and the rest of the leaf area is borne by more twigs. Their count comes from the pipe model
(Shinozaki, Yoda, Hozumi and Kira 1964, in [tree allometry](tree-allometry.md)): a terminal twig of
radius r is (r / r_twig)^Δ unit pipes, each serving one twig's leaves, and the recursion's terminal
criterion stops a shoot anywhere under 2.07 r_twig (where its thinnest child, at a 0.233 share,
would fall under r_twig), which is why the recursion grows about a third of the pipe rule's twigs
(312 of 957 at 6 m, 2,925 of 7,656 at 12 m). The spare pipes leave the twig as leafy twigs of the
species' size, as many as the deficit divided by the shortest twig's room, spread by spare pipes;
never more than the pipes allow ([flora](../subsystems/flora.md#allometry) has the rule).

| Default broadleaf | Budget | Twigs, before the leader fix / main / now | LAI, before / main / now | Leaf m², main → now | Cards (all three) |
|---|---|---|---|---|---|
| 4 m | 30,000 | 81 / 96 / 196 | 1.93 / 2.38 / 3.89 | 24.05 → 39.36 | 1,404 (1,448 before) |
| 6 m (the content build's) | 30,000 | 321 / 312 / 535 | 3.24 / 2.85 / 3.89 | 78.02 → 106.22 | 3,794 (3,456 before) |
| 8 m (the impostor tests' small oak) | 30,000 | 921 / 855 / 873 | 3.89 / 3.87 / 3.90 | 192.77 → 194.34 | 6,914 |
| 12 m (the default) | 60,000 | 2,313 / 2,925 / 2,925 | 3.89 / 3.90 / 3.90 | 480.09, unchanged | 17,092 |
| 24 m | 60,000 | 20,625 / 20,307 / 20,307 | 7.37 / 7.20 / 7.20 (merged cards, 1.04 m) | 3,719.44, unchanged | 20,816 |
| 32 m | 60,000 | 66,369 / 53,664 / 53,664 | 7.44 / 6.41 / 6.41 (merged, 2.33 m) | 6,057.76, unchanged | 14,374 |
| 32 m | 2,000,000 | 66,369 / 53,664 / 53,664 | 3.90 / 3.90 / 3.90 | 3,683.64, unchanged | 131,194 |

msvc-debug `engine-content flora`, 2026-11-07; "before the leader fix" is efe7e9c5's parent's
flora sources on this tree, "main" 8be9c6d4. Triangles and parts do not move on any row (the 6 m
tree's 16,088 and 406; a spray's twig wood is folded below `wood_class` on every rung, and a leafy
twig rides its twig's part). What the owner approved does not move: the 12 m and taller trees'
twigs already hold their cards, so they are the same bytes; the 6 m tree keeps its limbs, its
scaffolds and its every recursion shoot, and gains 223 leafy twigs from three to nine tenths along
twigs, whose cards are whole leaves again — a fuller, not a different, young crown, and one order
deeper (5 → 6). The 4 m sapling doubles its twigs. The cards' realised area is 0.97 of a leaf on
every height (the spray's lift and droop fold the rest out of the card's plane); the solver's LAI
of 3.89 against the 4 asked is that 3 %. The impostor tests' small oak (8 m) moves its four bake
hashes for its 18 new twigs.
