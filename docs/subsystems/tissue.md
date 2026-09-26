# tissue (domain)

**Purpose.** The engine's first tissue definition, v0: the schema'd description of a deformable
tissue body as the authoring side produces it — regions of tetrahedra with their materials in SI
units, the closed frames they rest on, the attachments between them, the skin binding as a
footprint and a transition band over a declared set of canonical vertex ids, the observation the
region reproduces and the contract it was accepted under, and the states the authoring side solved
— together with its `.tissue` container, the plain interchange the authoring side writes without
the engine, and the **validators and report** the reviews of the neutral torso studies specified
([05 §5.14 and §5.16](../plan/05-simulation.md#516-characters-at-run-time) with their addenda,
[07 §7.10 and §7.11](../plan/07-content-pipeline.md#711-characters),
[ADR-0026](../adr/0026-deformable-volumes-first-class.md),
[ADR-0029](../adr/0029-deformable-volume-budgets.md),
[ADR-0032](../adr/0032-characters-are-parameter-vectors.md)). **Nothing here simulates**: the
states are node positions the authoring side solved, and the engine binds, transfers, measures and
checks them. An optional capability ([ADR-0027](../adr/0027-additive-capabilities.md)):
`ENGINE_WITH_TISSUE=OFF`, `ENGINE_WITH_PHYSICS=OFF` (it requires physics, below) or a `*-minimal`
preset leaves the module and its tests out, and `engine-content` then refuses `tissue` with a
sentence.

**Why a module of its own, in `domain/`.** The definition is data about one kind of asset with its
own container, validators and schema, which is a module's worth of owned data; it reads
`domain/geometry` (the Loop limit surface and the surface binding a region's skin follows) and
`domain/physics` (the cage-size verdict of ADR-0029, `deformable.h`), both domain modules, so it
cannot be lower; and nothing in it ticks, renders or allocates per frame, so it has no business in
`systems/`. The link to physics is for one header — the plan says the content validator reads
`cage_size_verdict()` there — and physics is itself a capability, so the edge is declared
(`engine_capability_requires(tissue physics)`) and tissue is off wherever physics is.

**Owned data.** A `TissueFile` owns a definition and its blocks' bytes. A `TissueReport` owns the
rows and numbers one validation produced; an `ExpectationResult` what a fixture's declaration made
of one. `SyntheticTissue` owns the generated example. Nothing else holds a tissue definition; the
render path, the solver and the content build's derived steps that will read one do not exist yet
(below, "Not yet").

**Invariants.**
- A block's bytes are the ones its SHA-256 names, checked at import and again at every container
  read; a container's content hash covers every byte after its header.
- A container's section kinds are append-only (`BlockKind` in `schemas/tissue.schema`); a reader
  **skips** a kind it does not know, drops a table entry of an unknown kind and ignores an unknown
  definition field, each with a warning, and never refuses a file for being newer.
- Writing what was read gives the same bytes: the definition is stored as canonical JSON.
- A binding's normal mode is the string it was authored with; nothing substitutes another mode.
- Every validator row states its threshold and names a witness when it fails.
- The equilibrium gap and the reference's volumetric strain are diagnostics: they are reported and
  never fail, because the reference is the runtime solver's fixed point and a loaded state's strain
  does not name its cause.
- A ten-node cell's volume and its Jacobian's sign are exact, from its ten nodes; a row that walks
  the cells' linear subdivision instead says so in its value, and nothing subdivides silently.
- A reference body is never held to a runtime-only limit, and never excused a geometric or material
  one.
- A fixture is accepted when its outcome is the declared one: the rows that fail or are skipped,
  by id, subject and severity, exactly.

**Public API.** `domain/tissue/tissue_file.h`: `TissueFileHeader`, `TissueFileSection`,
`block_element_size`, `block_kind_name`, `TissueBlock`, `TissueFile`, `encode_tissue_file`,
`write_tissue_file`, `read_tissue_file`, `read_tissue_file_memory`, `TissueFileInfo`,
`read_tissue_file_info`, `import_interchange`, `export_interchange`, `seal_block`, `add_block`,
`topology_sha256`. `domain/tissue/validate.h`: `Severity`, `Verdict`, `ValidationRow`,
`ValidateOptions`, `TissueReport`, `validate_tissue`, `rows_json`, `report_json`.
`domain/tissue/expect.h`: `k_expect_format`, `k_expect_rule`, `parse_expectation`,
`read_expectation`, `ExpectationDifference`, `ExpectationResult`, `compare_expectation`,
`difference_kind_name`, `expectation_json`, `expectation_text`, `expectation_from_report`,
`write_expectation`. `domain/tissue/synthetic.h`: `SyntheticOptions`, `make_synthetic_tissue`.
`domain/tissue/sha256.h`: `Sha256`, `sha256`, `sha256_hex`. The types themselves are
`engine::tissue::*` in `<schemas/tissue.h>`, generated from `schemas/tissue.schema`.

**Depends on.** `base`, `containers`, `math`, `hash`, `io`, `json`, `schema`, `schemas`,
`geometry`, `physics`.

---

## The definition

`schemas/tissue.schema` is the source of truth; this is its outline. Physical quantities only —
moduli in pascals, membrane stiffness as E·t in N/m, densities in kg/m³, lengths in metres — and
**no solver compliance anywhere**, because a compliance authored at one cell size is wrong at every
other ([ADR-0029](../adr/0029-deformable-volume-budgets.md) decision 3: the build derives it). Kinds
name mechanics, never anatomy.

| Object | What it holds |
|---|---|
| `Region` | name; `kind` (`Volume`, `Cavity`); `cage` (`Tetrahedral`, four-node cells, the runtime's kind, and since version 2 `TetrahedralQuadratic`, ten-node cells in Gmsh's order, the kind a certified reference body is solved on — both imported from the authoring side's mesher, never generated here; below, "The ten-node cell"); `role` (version 2: `Runtime`, a cage the runtime solves, or `Reference`, a certified body a runtime cage is derived from; below, "Reference bodies"); nodes and cells (`tetrahedra`, a Tetrahedra or a QuadraticTetrahedra block as the cage says); `phases` (a material each — bulk and shear modulus, density — the first taking what the others' per-tetrahedron fractions leave); `membranes` (triangles, E·t per triangle or one value, ν, tension-only); `cables` (edges, stiffness, **slack** and **recruitment**); named `node_sets`; `sheets` (open control surfaces over its nodes, each with a Loop level); the `shell_stitch` that closes the sheets' limit surfaces into one shell; which sheet is the `top` (the skin follows it) and which the `support`; `hero` (ADR-0029's allowance) |
| `Frame` | a closed, outward-wound rigid proxy: vertices and triangles; **declared cover per vertex** with its `CoverProvenance` (`Undeclared`, `Fitted`, `AuthoredTransform`, `Authored`) and a provenance note, because a frame contracted to fit is a size changed, not discovered; `proxy`; the frames it lies `inside` |
| `Surface` | an attachment target that is not a frame, flagged `proxy` |
| `Attachment` | a region's node set, `kind` (`Fixed` — a stiff spring, not a weld —, `SlidingBilateral`, `Unilateral`), and a target: the world, a frame, a surface, another region's sheet (`region/sheet`) or a bone |
| `SkinBinding` | the region and sheet it follows; the Loop level and **the refinement rule** (`loop-hoppe-1994-v1`, geometry.md's numbering and masks); the **normal mode** (below); the **domain**, an explicit set of canonical ids the binding may write; the **footprint** (ids at weight one) and the **transition band** (ids and weights in (0, 1), its width and profile) as two authored objects; and the per-domain-id **records**: refined triangle, barycentrics, optional authored offset, optional authored reference normal |
| `Observation` | the declared base (`base_id`, vertex count, faces, `topology_sha256`), the accepted observation's positions, the **declared domain** (explicit ids), the base's positions at every id outside it, the signed deviation inside it, landmarks by canonical id, the **load** (gravity vector in body coordinates, medium and its density, pose, fill), and **acceptance** as a separate record (who, when, which sheet) |
| `RegionState` | a region's nodes at a state, its `role` (`Reference` — what the binding is paired with —, `Rest`, `Response`, `Construction`), its `provenance` (`Authored`, `ForwardFromAuthoredRest`, `InverseStatics`, `ForwardFromReference`), its load, and — required for `InverseStatics` — an `InverseFit`: the fit's `method` (`surface-targeted`, `legacy-all-nodes`), how it extended the correction to the interior (`harmonic`, `elastic`, `none`), its step, the node sets it held, its residual, its smallest cell ratio, and the **`solver`** the rest was recovered against (`InverseSolver`: `kind` `xpbd` with `iterations`, `step_s` and optionally `duration_s` and `damping_per_s`, or `static-minimization` with `gradient_tolerance_n`; and the `implementation`); optionally the authoring side's own transferred visible surface, for the agreement row |
| `DepthBudget` | the layer order along the local normal — frame, clearance, support, tissue, roof, cover, exterior — with the clearance's minimum, the width above which a clearance is a named **slot** or an undeclared depth, the slot's name, the cover layer's range, target and **purpose**, the area fraction of the roof the range must hold on, the absolute visible-minus-top-surface volume target, and the **skin clearance floor**, the least distance the simulated tissue surface keeps inside the transferred skin (0, bare containment, by default) |

**Why the inverse fit declares its interior.** One observed exterior fixes where the skin is and
says nothing about where the interior nodes rest; two inverse fits of one observation that placed
the interior differently are two tissues with the same picture at the reference and different
responses. So a state that claims inverse statics must say how it placed what it could not see, and
the validator refuses one that does not. Study019's fixture is `legacy-all-nodes` with `none`: it
targeted every node, which is the historical behaviour, not a prior.

**Why a rest names its solver.** An XPBD relaxation converges to the fixed point of its constraint
projections, and that is not the stationary point of the energy the constraints stand for: a rest
recovered by exact minimization drifts under an XPBD runtime, and a rest recovered against one
solver's fixed point reproduces the observation only under that solver, its iteration count and its
step. So **the engine-facing reference state is the fixed point of the runtime solver**, not the
energy's stationary point, and a rest carries the identity of the solver it is a rest for. The
validator warns when a rest names none and refuses an identity that pins nothing (an `xpbd` one
without its iterations and step, a `static-minimization` one without the gradient it stopped at);
the equilibrium gap (below) is how far the two points are apart.

**Why the records are footpoints and not packed 10-byte records.** The binding's per-vertex data is
stored as the authoring side wrote it — a refined triangle, barycentrics in f32, the offset in f32 —
and packed into `geometry::SurfaceBinding` records when it is read (`bind_from_records`, which
reports what the packing costs: 3.69 µm on study019). The packed record is what a runtime reads,
and it is a function of the footpoints alone once the offsets are authored, so it is derived data:
it belongs in the content-build step that will write a binding stream into the cluster pages, keyed
like every other derived entry, and a definition that stored it would carry the same information
twice at two precisions.

**Why the normal mode is a string.** The footpoint normal the offsets transport along has three
definitions (geometry.md, "The footpoint normal"), and the authoring side and the engine agreed their
spellings — `triangle`, `interpolated-vertex-area-weighted`, `limit-interpolated` — in the round-five
closure. An enumeration's identifier cannot carry the hyphen, and a cross-tool contract's spelling
is not something to translate, so the field is a string that `geometry::parse_normal_mode` accepts
exactly and nothing else.

## The interchange: what an authoring tool writes

One JSON file, a `TissueDefinition`, plus one binary file per block. Everything bulky is a block,
referenced by name from the JSON; the JSON's `blocks` table lists each one:

```json
{"name": "left.nodes", "kind": "RegionNodes", "file": "s019.left.nodes.bin",
 "count": 275, "bytes": 3300, "sha256": "…64 hex digits…"}
```

`count` is elements, not bytes; `bytes` is `count` times the kind's element size, and the file is
exactly that long; `sha256` is the SHA-256 of the file's bytes. The kinds and their layouts, all
little-endian, f32 IEEE binary32, u32 unsigned, u8 a byte:

| Kind | Element | Referenced from |
|---|---|---|
| `RegionNodes` 10, `FrameVertices` 20, `SurfaceVertices` 24, `ObservedPositions` 28, `BaseOutsidePositions` 29, `FootpointBarycentrics` 33, `AuthoredNormals` 35, `StateNodes` 36, `ExpectedVisible` 37 | f32 × 3 | positions, barycentrics, normals |
| `Tetrahedra` 11, `BaseQuads` 27 | u32 × 4 | a region's four-node cells; a base's quads (split (0, 1, 2), (0, 2, 3) wherever a triangle is needed) |
| `QuadraticTetrahedra` 38 | u32 × 10 | a region's ten-node cells: the four corners, then the edge nodes of (0, 1), (1, 2), (0, 2), (0, 3), (2, 3), (1, 3) — Gmsh's order (below) |
| `MembraneTriangles` 14, `SheetFaces` 18, `ShellStitch` 19, `FrameTriangles` 21, `SurfaceTriangles` 25, `BaseTriangles` 26 | u32 × 3 | triangles |
| `CableEdges` 16 | u32 × 2 | cables |
| `NodeSet` 12, `CanonicalIds` 30, `FootpointTriangles` 32 | u32 | node sets; id sets, **strictly ascending**; one refined triangle per domain id |
| `PhaseFraction` 13, `MembraneStiffness` 15, `CableStiffness` 17, `FrameCover` 22, `IdValues` 31, `FootpointOffsets` 34 | f32 | one value per element of what it annotates (NaN where a cover or an offset is not declared) |
| `FrameCoverProvenance` 23 | u8 | one `CoverProvenance` per frame vertex |

Kinds 1 and 2 are the container's own and never an interchange block. In a Blender script a block is

```python
data = np.ascontiguousarray(array.astype('<f4')).tobytes()   # '<u4' for indices, '<u1' for u8
open(path, 'wb').write(data)
entry = {"name": name, "kind": kind, "file": os.path.basename(path), "count": len(array),
         "bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
```

and the rest is `json.dump`. **Canonical ids** are the base's vertex indices, dense from 0 to
`vertex_count - 1`, and the observation's positions are in id order; they are the ids a glTF export
carries in `_CANONICAL_ID` ([geometry](geometry.md#canonical-vertex-identity)), so a definition can
later be resolved against a built mesh. **The topology hash** is SHA-256 over the base's face
indices widened to little-endian u64, face by face and corner by corner — NumPy's default integer,
the spelling study019 recorded, so the hash the authoring side already published verifies.
Enumerations are spelled by their schema names (`"Tetrahedral"`, `"SlidingBilateral"`); a field
left out takes its schema default. `engine-content tissue example <dir>` writes the synthetic
definition as a complete interchange, the worked example of every field, and beside it
`synthetic-quadratic.json`, the worked example of a ten-node reference body (`"cage":
"TetrahedralQuadratic"`, `"role": "Reference"`, a `QuadraticTetrahedra` block).

`import_interchange` refuses what does not add up — an unknown kind, a byte count that is not
count × element, a file of another length, a hash that differs, a duplicate name, a `format` other
than `engine.tissue.v0`, and any field the schema does not have — because an interchange is written
for this format. A container only warns about the last kind of thing.

## The container: `.tissue`

Laid out like `.clusters` ([geometry](geometry.md), "Pages and streaming"): a 32-byte header
(`TISS`, version 1, flags, section count, total bytes, content hash), a table of 32-byte section
records `{kind, element_size, element_count, offset, name, 0}`, and payloads at 16-byte aligned
offsets. Section 1 is the definition's canonical JSON with each block's `file` cleared, section 2 the
block names, and every other section one block, its kind its `BlockKind`. **The kinds are
append-only**: never renumbered or reused. A reader skips a section of a kind it does not know,
drops a table entry of a kind it does not know, ignores an unknown definition field, and says so
for each in `TissueFile::warnings`, which the validators turn into a `file.read` warning row — the
plan's rule for element kinds ([07 §7.11](../plan/07-content-pipeline.md#711-characters)): a region
whose kind a build lacks falls back rather than failing the file. What a read refuses is damage: a
wrong magic or version, a section past the end, a content hash that differs, and a block whose bytes
are not the SHA-256 the table records.

## The ten-node cell

A certified reference body is solved on quadratic tetrahedra: 1,635 nodes and 846 ten-node cells in
the authoring side's supine pair, study019's 275-node cage with a node on each of its 1,360 edges.
`cage: "TetrahedralQuadratic"` reads them from a `QuadraticTetrahedra` block, u32 × 10 a cell.

**The node order is Gmsh's** (`MSH` element type 11), because Gmsh is the authoring side's mesher and
its order is the one documented where a mesh comes from. Three orders are in use, and they differ
only in the six edge nodes:

| Order | nodes 4 to 9 lie on the edges |
|---|---|
| Gmsh (this interchange) | (0, 1) (1, 2) (0, 2) (0, 3) (2, 3) (1, 3) |
| VTK `VTK_QUADRATIC_TETRA`, meshio `tetra10` | (0, 1) (1, 2) (0, 2) (0, 3) (1, 3) (2, 3) |
| lexicographic: the authoring side's `reference-p2.json` sidecar | (0, 1) (0, 2) (0, 3) (1, 2) (1, 3) (2, 3) |

A writer holding another order permutes into Gmsh's: from VTK's, swap nodes 8 and 9; from the
lexicographic, Gmsh's node k is the lexicographic node (0, 1, 2, 3, 4, 7, 5, 6, 9, 8)[k]. Nothing in
the numbers says which order a file was written in, so the geometry is read instead:
`region.cell_edge_nodes` refuses a cell whose edge node lies a quarter of its edge or more from the
straight edge's midpoint at the construction, and when the cell fits one of the other two orders
its witness says which (tested for both).

**Volume and orientation are exact, from the ten nodes** (`src/cells.h`). A ten-node cell is a
quadratic map from the reference tetrahedron; its Jacobian's columns are linear, so det J is a cubic,
which on a tetrahedron is exactly twenty Bernstein coefficients (Johnen, Remacle and Geuzaine 2013,
the validity test Gmsh itself runs on curved elements). The cell's volume, the integral of det J, is
the coefficients' sum over 120, exactly; det J lies between the least and greatest coefficient and
equals the corner coefficient at a corner. So a least coefficient above zero proves the cell valid, a
corner at or below zero proves it inverted, and between the two the cell is split into eight and the
pieces' coefficients recomputed, to depth 4, where a cell still undecided fails. The test checks the
volume against det J from the shape functions' gradients integrated by a Gauss rule exact for the
cubic, to 1e-13, and the bounds against det J sampled on a lattice, including cells that are
positive at every corner and inverted inside.

**The same-node linear subdivision** is what a row whose meaning is linear walks: each cell's four
corner tetrahedra and its inner octahedron split along the octahedron's shortest diagonal at the
construction (the first of (4, 8), (6, 9), (7, 5) on a tie), eight four-node cells over the cell's
own ten nodes, chosen once so that every state walks the same ones. It is conforming across cells
whatever diagonal each chose — every face of a cell splits into the same four triangles — so its
boundary is the piecewise-linear surface through every boundary node. The authoring side's own
diagnostic subdivision (6,768 cells for 846) need not have chosen the same diagonals; only the energy
rows see the difference, the boundary does not. Row by row:

| Row | On a ten-node cage |
|---|---|
| `region.cage_size` | natively: every node counts |
| `region.cell_quality` | natively: each cell's corner tetrahedron's SICN (its straight-sided shape; Gmsh's own high-order SICN bounds the curved cell and can differ where it is curved), with each cell's Jacobian ratio (least det J over greatest, from its Bernstein bounds) reported beside it as its curvature |
| `region.cell_edge_nodes` | ten-node cages only (above) |
| `region.cell_orientation` | natively: det J positive over the whole cell, proven as above, every state |
| `region.materials` | natively: one fraction per ten-node cell, the mass from the exact volumes |
| `region.through_thickness` | natively: a cell spans the sheets when any of its ten nodes is on each |
| `region.volumetric_strain` | natively: each cell's exact volume at the reference over the rest, averaged over its ten nodes |
| `region.affine_patch`, `state.equilibrium_gap` | **on the subdivision**, named in the row's `representation`: `bulk-edge-v0` is the runtime's network of linear cells, which a ten-node cell does not have; its weight is lumped from the subdivision |
| `frame.intersections`, `region.skin_containment`, the depth budget's boundary clearance, `volume.omitted_volume`'s exposed boundary | **on the subdivision's boundary**, named in the row's `piecewise_linear_boundary` or the volume report's representations: the curved faces are tested by their four chords each, which is what an exact triangle-pair test can take |
| `frame.containment`, the node clearances | natively: nodes are nodes |
| `volume.report` | `cage_ml` natively, the cells' exact volume; `cage_linear_ml` beside it is the subdivision's, and the difference is what the cells' curvature holds |

## Reference bodies

`role: "Reference"` says a region is a certified reference body — what a runtime cage will be derived
from ([07 §7.10](../plan/07-content-pipeline.md#710-deformable-volume-assets): cage generation is a
derived step) and what a regression fixture holds — and not a cage the runtime solves. The rows that
encode a **runtime-only** limit report it and never fail:

- `region.cage_size` becomes an info row with the same numbers, the verdict ADR-0029 would give the
  body as a runtime cage (`verdict_as_runtime`, and `verdict_as_hero`), and how far over the ambient
  default and the 800-node limit it is: the budget the derived cage has to meet.
- `region.sheets` reports a sheet refining past the binding record's u16 instead of escalating: the
  packed record is what a runtime reads, and a reference body's binding is not the one shipped. The
  engine's transfer still reads the packed record, so a binding over such a sheet cannot be checked
  here, and `blocks.resolve` says so; the five-faces rule is Hoppe's geometry and still applies.

Everything else applies in full, because it is geometry, material or declaration and not the
runtime's: cell quality, orientation and edge nodes, materials, containment and intersections, the
depth budget and the cover, the binding rows, the volumes, and the inverse-statics declarations (a
certified state still names what it was solved against). The diagnostics stay diagnostics.

## Validators

`validate_tissue` runs every row; `engine-content tissue validate` prints them as one JSON line and
exits 1 when an **error** row fails (or, in the fixture mode below, when the outcome is not the
declared one). Rows with a threshold the plan states but calls a quality gate are **warnings**; rows
the reviews ask to see and never gate are **info**. On a ten-node cage each row that walks cells
works as "The ten-node cell" says, and on a reference body the two runtime-only limits report
("Reference bodies"). The ids:

| Row | Severity | Threshold | Why |
|---|---|---|---|
| `definition.format`, `blocks.resolve` | error | the format and unit; every reference resolves to a block of the right kind and count, every index in range | nothing below can be checked on data that does not resolve |
| `file.read` | warning | nothing skipped | an unknown kind falls back to its rest shape |
| `observation.topology` | error | the computed topology hash is the declared one | an observation is accepted against one declared base |
| `observation.outside_bitwise` | error | the observation is the base **bit for bit** at every id outside the declared domain | "bitwise equal outside an explicitly declared id set" (05 §5.16, item 2) |
| `observation.landmarks` | error | each landmark's id exists and its stated position is the observation's there, bit for bit | landmarks are named by canonical id |
| `observation.load`, `observation.deviation`, `observation.acceptance` | warning, info, info | a declared load; the deviation map reported; acceptance reported, never gated | a sheet inspected is not an acceptance |
| `region.cage_size` | warning (error past 800); **info under `Reference`** | `physics::cage_size_verdict`: 256 nodes unless `hero`, 800 for any; a reference body's reported as a budget with `verdict_as_runtime` | one solve group; ADR-0029 decision 2 |
| `region.cell_quality` | warning (error at or below 0) | every cell's (corner) tetrahedron's SICN above 0.1; a ten-node cell's Jacobian ratio reported beside it | the imported tetrahedral kinds need a quality row: no engine tetrahedralizer stands behind them |
| `region.cell_edge_nodes` | error, ten-node cages only | at the construction every edge node within a quarter of its edge of the straight edge's midpoint (Gmsh's order); every edge's node the same in every cell sharing the edge, no node both corner and edge node | the node order is read from the geometry, and a mesh whose cells disagree on an edge's node has a crack |
| `region.cell_orientation` | error | every tetrahedron positive in every state; a ten-node cell's det J positive over the whole cell, proven by its Bernstein coefficients to depth 4 | a cell that turns inside out has a volume constraint pushing it further out |
| `region.materials` | error | positive moduli and densities, fractions in [0, 1] summing to at most one per cell, −1 < ν < 0.5, nonnegative cables | SI quantities that mean what they say |
| `region.sheets` | warning (error past the u16 cap, **reported under `Reference`**) | at most five faces at any control boundary vertex; refined triangles within 65,536 | Hoppe's rules are G1 only there; the record's triangle is a `u16` |
| `state.inverse` | error | every inverse-statics state declares its method and interior extension; the node sets it held exist | one exterior observation does not identify the interior rest |
| `state.inverse_solver` | warning (error for an identity that pins no fixed point) | every inverse-statics state names the solver its rest was recovered against; `xpbd` with its iterations and step, `static-minimization` with its gradient tolerance | the engine-facing reference is the runtime solver's fixed point |
| `region.through_thickness` | info | the cells that span both sheets; the nodes inside the cage (off its boundary) and on neither sheet | a slab one cell thick has no through-thickness freedom, and refining it changes its response for that reason alone |
| `region.affine_patch` | info | ten affine shears and a dilation of the whole region at its rest, under the declared energy: each shear's effective modulus against the declared one, globally and in the region's frame, the isotropic mean, the bulk | an edge network carries its mesh's fabric; the declared energy calibrates the isotropic part, so what the row shows is the mesh's |
| `region.volumetric_strain` | info | the reference's volumetric strain \|V / V0 − 1\| against the rest **per cell, node-averaged** (each node's rest-volume-weighted mean of its cells) **and the deviation between them** (a cell against the mean of its corners), p50, p95, max, the pressure it stands for, how many of the ten worst cells touch a held node, **beside the declared load, the declared interior prior and the reference's equilibrium gap**: read with them; never a verdict, no threshold | stress at a loaded reference is physical, so a strain alone does not name its cause; a fit can also put a pre-stress an order of magnitude above its load into the interior, and a checkerboard averages away at the nodes and shows in the deviation |
| `state.equilibrium_gap` | info, never a failure | the norm of the declared energy's exact gradient at the free nodes of the rest, the reference and every response, max and rms in newtons, **naming the solver** whose fixed point the reference is, with what it does not include and the nodes where it is complete | the static-equilibrium residual of every stored state, and the distance between the solver's fixed point and the energy's stationary point |
| `region.shell_closed` | error | the sheets' refined triangles and the stitch close a consistently wound 2-manifold of positive volume | the closed smooth shell is one of the four volumes |
| `attachments.resolve` | error | every attachment names a region, one of its node sets and a target that exists | an attachment to nothing is a region that falls |
| `frame.closed_manifold` | error | every edge used twice in opposite directions, no triangle without area, positive volume | the winding number needs a closed, consistently wound frame |
| `frame.cover`, `frame.layering` | info, error | declared cover and provenance beside the measured nearest skin distance over **all** frame vertices, with the minimum's location; every vertex inside the frame it declares it lies in | the frame declares its cover per vertex with provenance |
| `frame.containment` | error | **no region node inside the frame by generalized winding number** (> 0.5), all nodes, every state; the minimum node distance reported per state | a node inside its frame is tissue inside bone, and a winding number does not care what a ray would have hit |
| `frame.intersections` | error | no triangle of the region's piecewise-linear boundary **or** of its closed smooth shell crosses the frame, every state | surfaces can cross between vertices that all pass |
| `region.skin_containment` | error | at **every** state (construction, reference, rest, responses), against the skin as transferred to it: an **exact triangle-pair intersection test** of the tetrahedra's boundary and of the smooth shell against the skin, none crossing; and the **clearance distribution** against the declared floor on the simulated surface — every node, the boundary's nodes, and every skin vertex of the declared domain against the boundary (the other direction of the vertex-to-face distance) — and on the dense cap, never on the cap alone | the region lies inside the base's skin; the dense cap is the surface the skin follows and the tetrahedral boundary the one that is simulated, and the two clear the skin by different amounts |
| `depth.layer_order` | error | along the roof's **local outward normal** at every dense point: skin outside, support inside, frame beyond the support | a sloping region inverts any world-axis order; 18 of study019's 121 control nodes invert the y order and none the normal order |
| `depth.clearance` | warning | support to frame at least the declared minimum on both representations; above the slot width at the median, a named slot or an undeclared depth | a clearance wider than a few millimetres is a region to be, not a margin |
| `cover.range`, `cover.report` | warning, info | the cover inside the declared layer's range on at least the declared fraction of the roof's area; and the report below | the cover is a thickness distribution, never a percentage of the shell |
| `binding.seam_identity` | error | no vertex written by two bindings; every vertex a binding does not write is the observation bit for bit in every state | a bilateral region is two modules with a seam neither writes |
| `binding.domain` | error | footprint and band disjoint subsets of the domain, band weights in (0, 1) | two authored objects over an explicit id set |
| `binding.written_in_observation_domain` | warning | every written vertex lies in the observation's declared domain | outside it the contract calls a vertex base |
| `binding.records` | error | footpoints within the sheet's refined surface and the u16 cap, finite offsets, a **valid reference footpoint normal** under the record set's mode | invalid reference normals are rejected, never invented |
| `binding.reference_identity` | error | at the reference every domain vertex is the observation, bit for bit | the displacement form (geometry.md) |
| `binding.image_orientation` | error | no bound render triangle whose footpoint image is reversed, and none that turns over in a response | study019's first nearest-point binding reversed eight |
| `binding.footpoint_normals`, `binding.mode_conformance` | info | the engine's footpoint normal under each mode against the authored one; the same records transferred under all three modes, positions and visible volumes | one definition of the normal, measured |
| `binding.transfer_agreement` | warning | the engine's transfer within 0.01 mm of the authoring side's at every domain vertex | two implementations of one transfer, compared |
| `volume.report`, `volume.visible_vs_top` | info, warning | the four volumes and the decomposition, reported together, each labelled with the representation it is measured on, and **the posterior sweep** (the support sheet's, on its limit surface and on its control triangles) as its own line beside them; \|visible − top swept\| within the budget's absolute target | a response is compared with the top surface's sweep, never with the cage |
| `volume.omitted_volume` | warning (error when containment could not run) | the sheets span every simulated surface node that can move; otherwise the volume those nodes sweep beyond what the bound surface carries, per state, the smooth shell, top sweep and visible body labelled `restricted`, and **containment mandatory** | a binding over a subset of a cage's surface nodes is not a representation of that cage, and containment is then the only check on what it omits |

**The volume report** is the plan's four quantities at every state, always together: the cage's
piecewise-linear volume (the tetrahedra's sum), the closed smooth shell (the sheets' limit surfaces
and the stitch, by the divergence theorem), the top surface's **swept** volume from the reference
and the visible body's — "swept" being the volume a triangle sweeps when its corners move linearly,
exact for linear motion and, over a closed surface, exactly its change of volume
(`((d0 + d1 + d2) / 6) · (e1 × e2 + (e1 × g2 + g1 × e2) / 2 + g1 × g2 / 3)`). At every response the
**exact transfer decomposition** — operator only (`BindingTerms::displacement`), offset only
(`BindingTerms::offset`), full, and their interaction (full minus the two) — because the marginals
cancel and are never reported alone; the smooth shell's bias against the cage at the reference;
and the **projection diagnostic** Σ w h (cos θ − 1) A, θ the footpoint normal's turn, which is
**reported and never subtracted** (a rigidly rotated offset shell keeps its volume while the sum
reads −14.8 ml). The visible body's triangles are the base's quads split (0, 1, 2), (0, 2, 3), the
split study019's volumes were computed with; the other split reads +0.678 ml for its +0.748.

**The cover report** is on **one sampling measure, the roof's own dense area** (each dense vertex
of the top sheet's limit surface carries a third of its triangles' area): the thickness
distribution (nearest distance to the skin, positive inside), the area fractions inside, above and
below the declared range and above 5 mm, and the volume split **total = target × area + excess −
deficit**, which is an identity on one measure and is checked to hold (`split_residual_ml`); the
excess over the range's maximum beside it; cover over the local tissue thickness (the roof-to-support
distance along the inward normal); and the roof-fit residual, cover minus target, with where it is
worst. The bound-offset estimate Σ w h A that study019 reported is in the volume report under its
own name, on the **weighted skin area** — a different measure, never added to the cover split. The
round-five closure asked for exactly this: an additive split on one measure, never a baseline and a
positive excess summed as a partition, never the skin's weighted area mixed with the roof's.

**Coverage and the omitted volume.** The smooth shell, the top sweep and the visible body see only
the sheets' control nodes, so they represent the cage only when the sheets span every simulated
surface node that can move: every node of the cage's boundary on its exposed side (each boundary
face not wholly the support sheet's) that is not held by a fixed attachment and not the support's
own. A node on no sheet is one the bound surface does not carry. Its footpoint is its nearest point
on the top sheet's limit surface at the reference, the bound surface would have carried it as that
point moves, and the **omitted volume** of a state is what the exposed boundary sweeps with the
nodes where they are, minus what it sweeps with them where the bound surface would have put them.
The review of the authoring side's refinement pair is why: a 121-control binding over a 457-node
anterior surface omitted +2.1 ml at standing and −1.6 ml of the response, against a whole visible
response of +0.75 ml. The decomposition is unchanged and stays exact either way.

## The fixture mode

A regression fixture is a definition the engine is **expected to fail in known ways**: a certified
reference body whose upstream audit rows are still open, a historical binding kept on purpose. Exit 0
would hide both a fix upstream and a regression here behind one status, so the plan's contract
([05 §5.16](../plan/05-simulation.md#516-characters-at-run-time), 2026-09-26) is that a fixture is
accepted when "the file parses, every row is evaluated and the failures are the ones the packet
declares". `engine-content tissue validate <file> --expect <expected.json>` is that contract
(`domain/tissue/expect.h`):

- **The declaration** is a `TissueExpectation` (`schemas/tissue.schema`): the format the authoring
  side published with its first fixture packet, `astra.tissue.expected-failures.v1`, adopted as
  published — the cross-tool spelling is kept, as the normal modes' are — so a packet's
  `EXPECTED-FAILURES.json` is read as it stands. Each failure is an `id`, a `subject` and a
  `severity` exactly as the report prints them, and the `witness` recorded with it. The engine adds
  four optional fields a v1 file does not carry: `verdict` (`Skipped` declares a row that cannot
  run), `value` (a pattern the row's value must match — every key named present, numbers within
  `tolerance`, everything else equal), `tolerance` (absolute, in the number's own units; 0 is the
  printed shortest round-trip form), and `note`.
- **The match is exact on the multiset of (id, subject, severity)**, and on the outcome: every row
  that fails or is skipped, whatever its severity, must be declared, and every declared failure must
  fail (or be skipped) with the declared severity. A declared row that passes, runs as info or is
  absent is a *missing* difference (fixed upstream, a role that now reports it, or a declaration out
  of date); a failure nobody declared is an *unexpected* one (a regression); a declared value the row
  no longer has is a *value* difference. The witness is never matched: it names a node or a number
  and moves with any change in how the engine walks the data; it is printed beside a difference.
- **Skipped rows count.** "Every row is evaluated" is the contract, and a row that could not run
  says nothing, so it must be declared (`"verdict": "Skipped"`) or it is a difference. The v1 rule
  names failures only; a fixture whose rows all run is unaffected.
- **Exit 0 exactly when nothing differs**, whatever the error count; 1 otherwise, with every
  difference printed to stderr in a sentence and in the JSON line's `"expect"` (`matched`,
  `declared`, `outcomes`, `differences`); 1 as well when the declaration cannot be read — a file
  that is not a v1 declaration is refused before anything is validated, never read as empty.
- **`--write-expect <path>`** writes the declaration this run would match — every failing or skipped
  row with its severity and witness, no value — for review before it is committed as a fixture's; the
  engine's additions are written only where they differ from their defaults, so a declaration of
  failures alone is exactly what a v1 writer produces. `report` already writes the report as data (one
  JSON line), so it gained no flag.

## The declared energy, the patch test and the equilibrium gap

Three rows read the declared materials as mechanics, and they need an energy to do it. A definition
carries moduli, never compliances, so the energy is the engine's, named `bulk-edge-v0`
(`src/energy.h`) because a gap is comparable only between two implementations that evaluate the same
energy. Per tetrahedron of rest volume V0, bulk modulus K and shear modulus μ (the phases mixed by
fraction, as for the mass):

    bulk:   (K − 5μ/3) (V − V0)² / (2 V0)
    edges:  for each of its six edges, (5/4) μ V0 ((l − l0) / l0)²

These are the energies of an XPBD volume constraint and of a distance constraint per edge — the
network Jolt's soft body and the authoring side's kernel both are — and the constants are what make
the declared (K, μ) mean what they say at small strain, **exactly and for any cell shape**: a
dilation strains every edge alike, so the edges add (5/3)μ to the bulk term's K − 5μ/3; and averaged
over the five independent pure shears the edges store μ|ε|²V0, because each edge's strain is d·ε·d
and |dev(d dᵀ)|² = 2/3 for any unit d. Two things it is not. A network of central forces is
anisotropic cell by cell — a regular tetrahedron carries 1.25μ in the three plane shears and 0.625μ
in the two normal-stress differences — so a mesh's response carries its edges' fabric; and it cannot
represent Poisson's ratio below 1/4 (Cauchy's relation), which the law refuses. Tissue is near 1/2.

**The authoring kernel's G is an edge coefficient.** Its edges store 3 G Vₑ ε² with Vₑ a sixth of
the incident rest volume, which is this law with μ = 2G/5: an edge network's isotropic shear is 2/5
of its per-edge coefficient (the spatial-sensitivity review's §3), and it adds 2G/3 to the bulk. So
the study019 conversion declares the material the kernel realized, μ = 2G/5 and K = K_kernel +
2G/3, under which `bulk-edge-v0` is the kernel's bulk and edge energy. That reading is an **explicitly approximate isotropic interpretation** of the legacy anisotropic finite-strain edge law, never an exact reproduction of it and never a canonical conversion (the authoring side's ruling of 2026-09-24; its B2a and B2b comparators declare physical cell moduli, μ = 350 (1 − f) + 1000 f Pa over the gland fraction f and K = 100 kPa, and are deliberately not a fit to the network). A replay of historical019 keeps that law's own averaging: the kernel takes
an edge's coefficient from a nodal average of the gland fraction and the engine from the cells that
share the edge, which moves the per-plane fabric by a few percent (below). Declaring G as μ instead
makes the energy 2.5 times stiffer in shear than the kernel that produced the states.

**`region.affine_patch`** applies ten pure shears (the three coordinate planes and two normal-stress
differences, globally and in the region's frame, whose normal is the top sheet's mean normal at the
rest and whose first in-plane axis is the one closest to x) and a dilation to the whole region at
its rest, and reports each shear's effective modulus against the declared volume-weighted μ, the
isotropic mean and the bulk against K. The isotropic mean and the bulk read 1 by construction; the
spread of the ten is the mesh's fabric. Both sets are needed, because a cubic fabric (a lattice, a
slab of Kuhn cells) is soft in the three plane shears and stiff in the other two, and a row of plane
shears alone reads it as uniformly soft.

**`region.volumetric_strain`** is the reference's volume change against the rest per cell,
node-averaged (each node's rest-volume-weighted mean of its cells) and as the deviation between the
two (a cell against the mean of its corners' averages: a smooth field reads the same both ways, a
checkerboard averages away at the nodes and shows here), with the pressure it stands for at the
cell's K, and **three companions: the reference's declared load, the declared interior prior** of its
inverse fit, **and its equilibrium gap**. It is read with them; never a verdict, and it has no
threshold. A loaded reference is stressed because gravity and the support demand it, so a strain
alone does not say whether the load or the fit's interior prior put it there — but a fit can place a
pre-stress an order of magnitude above its load while the surface fits to a fraction of a millimetre,
and this is where that shows.

**`state.equilibrium_gap`** is the static-equilibrium residual of every stored state: the norm of
the declared energy's exact gradient, plus gravity less the medium's buoyancy, at the free nodes of
the rest, the reference and every response, max and rms in newtons, **naming the solver** whose fixed
point the reference is. It is **a diagnostic and never a failure**, because the reference is the
runtime solver's fixed point and not the energy's stationary point, and this is the distance between
them. It is a force; the displacement form the authoring side reports (a Newton correction, p95 and
max, with the active support features in the projection) is owed (below, "Not yet"). Weight is lumped a quarter of each cell to its corners
from the construction's cells, where the densities are declared. Nodes are held (a fixed attachment,
or a target the gap has no geometry for), sliding (the component along the target's angle-weighted
pseudonormal at the nearest point is the reaction and is removed), in contact (a unilateral
attachment within 0.1 mm of its target, treated as sliding; v0 does not check the reaction's sign),
or free. Membranes and cables are **not included yet**, and the row lists them and reports the gap
separately at the nodes none of them reaches, where it is complete. The gradient is the energy's own
to 4 × 10⁻¹¹ of its largest component against central differences.

## Study019, converted and validated

The neutral packet `fable-observation-refit-2026-09-23` was converted once, by a Blender-Python
script in a scratchpad (never a repository tool; nothing of the packet is committed), into the
interchange above — the left side's region (its materials as the kernel realized them, above),
frame, support, binding (in its historical `interpolated-vertex-area-weighted` mode), observation
contract and its standing, rest and supine states with the solver the rest was fitted against —
imported, and validated. **0 errors and 3 warnings**, and the numbers against the review
(`FABLE-REVIEW.md`, `ASTRA-RESULTS.md`, `ASTRA-ROUND5-CLOSURE.md`, `reports/*.json`):

| | Engine | Review or authoring side |
|---|---|---|
| topology hash; outside the domain | `73b5b579…` computed = declared; 0 of 5,962 differ | the same hash; bitwise |
| cage | 275 nodes, `Wide` (19 over), `Ok` as hero; SICN min 0.119319, volume ratio 316× | the same; Gmsh's 0.119319 |
| nodes inside the frame | 0 of 275 in every state | 0 of 275 |
| minimum node-to-frame distance | 2.2887 / 2.3986 / 2.8718 mm (construction, standing, supine) | 2.289 / 2.399 / 2.872 mm |
| closed shell; its bias at the reference | 318.493 / 319.553 / 319.402 ml; −10.03 ml | the same; −10.03 ml |
| cage | 328.603 / 329.588 / 329.699 ml | the same |
| top surface swept (construction, supine) | +0.0528, +0.1392 ml | +0.0528, +0.1392 ml |
| visible response; decomposition | +0.7484 ml; operator +0.7053, offset −0.9774, interaction +1.0206 | +0.748; +0.705, −0.977, +1.021 |
| visible minus top swept | 0.609 ml, inside 2 ml | "0.6 ml, the binding's" |
| projection diagnostic; footpoint turn | −0.5373 ml; p50 6.7°, p95 12.7°, max 13.5° | −0.537 ml; p50 6.7°, p95 12.7°, max 13.5° |
| bound-offset estimate | 86.22 ml on 0.02230 m² of weighted skin | 86.2 ml on 0.02230 m² |
| cover, on the roof's 0.02548 m² | p50 2.08, p95 6.69, max 11.53, min 1.10 mm; 60.7% in [1, 3] mm, 39.3% above 3, 16.4% above 5; total 82.45 ml = layer 50.97 + excess 34.83 − deficit 3.35 | p50 2.08, p95 6.70, max 11.53, min 1.10; 61%, 39%, 16%; 82.4 ml, 51.0 layer, 34.7 excess (no deficit term) |
| skin clearance, supine | 0.587 mm at a node, 0.919 mm on the dense cap | 0.587 and 0.92 mm |
| skin clearance on the simulated surface, every state (floor 0) | no triangle pair of the tetrahedra's boundary or the smooth shell crosses the skin in any of the four states; boundary nodes min 1.100 / 0.976 / 0.393 / 0.587 mm and p50 6.3 mm (construction, standing, rest, supine); the skin's domain vertices outside the boundary by at least 1.078 / 1.080 / 0.988 / 0.844 mm | — |
| the posterior sweep | the support sheet from the standing reference: −1.113 ml (smooth) and −1.044 ml (its control net) at the construction and the rest, −0.290 and −0.297 ml at supine | — |
| dense support to frame | 4.53 mm minimum | 4.5 mm |
| records | 13,801 of 13,824 refined triangles, under 65,536; offsets 0.994–12.43 mm, p50 3.97; quantization 3.69 µm; tangential residual 3.93 mm; 2 boundary footpoints | 13,801 of 13,824; 0.99–12.43, 3.97; 3.7 µm; 3.93 mm; 2 |
| image orientation | 0 reversed (986 fully weighted triangles, 1,253 active), 0 turned over at supine | 0 reversed (985 a side) |
| transfer against the authoring side's supine | 0.68 µm at most | — |
| footpoint normal, limit rule against study019's | p50 0.0788°, p95 0.300°, max 1.180° | p50 0.0788°, p95 0.300°, max 1.180° |
| the three modes at supine | limit vs area-weighted 0.0220 mm, triangle vs area-weighted 0.1628 mm; visible +0.7484 (area), +0.7654 (limit), +0.7473 (triangle) ml | 0.02205 mm, 0.1628 mm; +0.7484, +0.7653, +0.7473 ml |
| solver | `xpbd`, 96 iterations, 1/480 s, 1.5 s | `reports/mechanics-trial.json` |
| through-thickness | 659 of 846 cells span both sheets; 33 interior nodes | 659 of 846; 33 of 275 |
| coverage | complete: the 97 moving surface nodes are all top-sheet controls; no omitted volume, nothing restricted | — (the refinement pair's fine level: 121 of 457, +2.1 ml omitted at standing) |
| affine patch, at the rest (μ̄ = 198.3 Pa declared, the kernel's 2/5 of Ḡ = 495.7) | isotropic 1.000, bulk 1.000; plane shears xy 0.837, xz 1.002, yz 1.175; in the slab's frame (normal (0.326, −0.909, 0.259)) in-plane 0.956, transverse 0.685 and 1.068; normal-stress differences 0.843 and 1.143 globally, 1.025 and 1.267 in the frame; range 0.68 to 1.27 | the kernel: 196 Pa effective against Ḡ = 492, 0.40 of G; xy 0.80, xz 1.07, yz 1.14; in-plane 1.12, transverse 0.91 and 0.62 |
| the reference's volumetric strain (standing, 1 g, prior `legacy-all-nodes`/`none`, gap 0.59 N) | per cell p99 5.55%, max 12.18% (cell 293), 8 of the 10 worst touching the rim, 5.6 kPa at p99 and 12.2 kPa at the max; node-averaged p50 0.12%, p95 1.13%, max 3.38%; cell against its corners' average p50 0.43%, p95 2.22%, max 11.0% — the strain is cell-to-cell far more than it is a field | p99 5.9%, max 12.2%, 8 of 10 at the rim (at the standing equilibrium after a Newton correction) |
| equilibrium gap, bulk and edges and gravity | standing 0.592 N max (interior node 272), 0.053 N rms over 227 free and sliding nodes; complete at the 33 interior nodes: 0.592 max, 0.117 rms; supine 0.211 max, 0.023 rms (complete: 0.211, 0.046); the bulk term alone reaches 0.768 N and the edges 0.035 N, against at most 0.060 N of weight on a node and 3.13 N in all; membranes (432) and cables (136) not included | the authoring side, all terms: 0.61 N max and 0.08 N rms at an XPBD fixed point whose tail motion was 4 × 10⁻¹² mm. Both are **force residuals**, not the projected equilibrium displacement the plan's gap row asks for, which the authoring side closed at 0.067 mm standing and 0.026 mm supine (coarse static equilibrium minus the XPBD endpoint, active support features projected) and which this page still owes (Not yet) |

**The warnings, and the disagreements.** The cage is `Wide` (an authoring fixture, a hero volume if
it ran). The cover fails its range on 39% of the roof — the review's "failure of the construction,
at the cage's resolution, along the fold", now a warning with a witness at the inferior fold.
**Two vertices the binding writes (ids 6237 and 8311, band weights) lie outside the observation's
declared domain** — the domain is 016's weighted set plus 018's zone and 019's weights were
recomputed; nothing the review reported, and the first thing to settle before the next packet. Four
numbers differ by definition, not by error: the cover over tissue thickness is p50 0.18, p95 0.42,
max 1.43 here (the roof's cover over the tissue along the roof's normal) against the review's 0.13 /
0.37 / 0.81 (the skin's bound offset over a chart-node thickness); the roof-fit residual is cover
minus target along the normal, p95 4.69 and max 9.53 mm, against the review's 6.84 and 17.0 mm
distance between a roof point and its chart-mapped target; the image deviation p50 7.32°, p95
14.5°, max 45.2° over one side's fully weighted triangles against Astra's 7.30° / 15.3° / 43.2° over
both sides; and the cover split carries a deficit term (3.35 ml) the review's partition did not.

**The mechanics rows.** The equilibrium gap is the bulk term's: the largest gradient is at an
interior node, where no membrane or cable reaches and the gap is complete, and it agrees with the
authoring side's own measurement (0.59 against 0.61 N), so study019's standing reference is a fixed
point of its kernel about 0.6 N — ten times a node's weight — away from the energy's stationary
point. Declaring the packet's G as μ (the nominal reading) moves it only to 0.588 N, since the edges
carry little of it. The plane-shear fabric differs from the review's by up to 7% (xz 1.002 against
1.07): the kernel weights an edge by a nodal average of the gland fraction and the engine by the
cells that share it, and the slab-frame numbers also depend on the in-plane axis, which the review
does not state. The volumetric strain's p99 is 5.55% on the declared standing state and 5.9% on the
review's re-solved equilibrium; the maximum and the rim's share agree.

## The supine fixtures, imported as ten-node reference bodies

The authoring side's step-one packet `astra-supine-negative-controls-2026-09-26` (manifest
`e390ff7d…`) publishes the certified supine pair, B2a-supine and B2b-supine: per case the
authoritative ten-node identity as a sidecar, `reference-p2.json` (`astra.tissue.reference-p2.v1`:
1,635 float64 nodes, 846 ten-node cells in the lexicographic order), a same-node linear view in the
v0 interchange (`diagnostic-v0.json`, 6,768 four-node cells, float32 blocks), and the declared
failures of that view (`EXPECTED-FAILURES.json`, v1). It was imported on 2026-09-26 by a scratch
adapter (a PowerShell script outside the repository; nothing of the packet is committed) that copies
the v0 view and replaces only its cells: the sidecar's cells permuted into Gmsh's order as a
`QuadraticTetrahedra` block, one gland fraction per ten-node cell, `cage: TetrahedralQuadratic`,
`role: Reference`; every other block is the packet's. Its checks, all as the packet says: every
child of the diagnostic uses only its parent's ten nodes, eight children a parent, the gland
fraction is one value per parent, and the supine state block is exactly the float32 of the sidecar's
float64 endpoint.

The adapter no longer stands between the packet and the validator. The authoring side exported the
same fixtures natively on 2026-09-26 (`astra-supine-native-reference-2026-09-26`, manifest
`ceeedd4a…`; per case `reference-native.tissue`, `56c034f7…` for B2a and `afb39fc7…` for B2b): the
ten-node cells built from the original corners and canonical edge ids in Gmsh's order, the original
per-cell gland fractions, `cage: TetrahedralQuadratic`, `role: Reference`, all thirty-nine original
blocks kept. The release `engine-content` (`0fd48ede…`, built from `main` at `5e7c8d1`) imports both,
exits 0 in fixture mode against the four-row declaration written down before the run
(`EXPECTED-FAILURES.predeclared.json`), and its 43 rows — 27 pass, 12 info, 4 fail — equal the adapter
route's row for row to the witness in both cases, which is the check that the permutation and the
native construction describe one body. Those two files, by hash, are what the engine's regression
fixture pins from here; the step-one packet stays the record of where they came from.


**The packet's own view, as published, matches its declaration**: `validate diagnostic.tissue
--expect EXPECTED-FAILURES.json` exits 0 for both cases, and every row's id, subject, severity,
verdict and witness is the pinned release engine's (only the labels this change adds to the values
are new; every number is the same). **As ten-node reference bodies** (43 rows: the 42 and
`region.cell_edge_nodes`), both cases say, row for row against the linear view:

| Row | Linear view (runtime role) | Ten-node reference body |
|---|---|---|
| `region.cage_size` | fails, error: 1,635 nodes past 800 | **reported**, info: 1,635 nodes and 846 cells, `Refused` as a runtime cage, 1,379 over the ambient default and 835 over the limit |
| `region.cell_quality` | passes: SICN min 0.1063 over the 6,768 children | passes: SICN min 0.119319 over the 846 corner tetrahedra (study019's, Gmsh's), Jacobian ratio min 0.99973 at the construction |
| `region.cell_edge_nodes` | — | passes: 1,360 edges agreed on by every cell, edge nodes within 1.4e-5 of an edge of the chords at the construction (float32), up to 0.20 (A) and 0.24 (B) of an edge in the failed standing reference |
| `region.cell_orientation` | fails, error: the failed standing reference's children 3738 and 3974 (A; 3739 too in B) inverted | **passes**: every ten-node cell's det J proven positive in every state; the failed standing reference is the most curved, least Jacobian ratio 0.0457 (A) and 0.0459 (B), both at cell 496, the parent of child 3974 |
| `region.skin_containment` | fails, error: construction node 929 at 0.847 mm | fails the same: nodes and boundary are the same (106 and 120 boundary pairs crossing the skin at supine, 6 and 26 shell pairs) |
| `cover.range`, `volume.visible_vs_top`, `volume.omitted_volume` | fail, warnings | fail the same, same witnesses (3.186 and 3.617 ml; 624 moving surface nodes on no sheet) |
| `region.through_thickness` (info) | none of the 6,768 children spans both sheets | 659 of the 846 cells do, study019's count; 673 interior nodes either way |
| `region.volumetric_strain` (info) | p99 35.7% (A), max 213% at child 3972 | per ten-node cell, exact volumes: p99 20.0%, max 36.5% at cell 639 (A); 20.9% and 39.0% (B) |
| `volume.report` (info) | cage 329.198 ml at supine | cage 329.766 ml (A), the linear view's 329.198 beside it; 329.993 against 328.405 ml in the failed standing reference, where the cells are most curved |
| `state.equilibrium_gap`, `region.affine_patch` (info) | 7.25 N (A), 8.45 N (B) at the failed standing reference; isotropic 1.000, planes 0.70 to 1.27 | the same numbers to every digit: on the subdivision, whose diagonals are the packet's own |

The other 30 rows have the same verdicts, witnesses and numbers (the mass is 0.3194 kg either way:
the construction's cells are straight). **So the fixture mode says**: against the published declaration
with `region.cage_size` removed (it now reports), `validate --expect` exits **1** with one
difference: `region.cell_orientation` is declared failing and passes — the inversion the linear view
reports is its subdivision's, of cells whose quadratic map is valid, which the packet's README says in
its own words ("different interpolated geometries"). Against the declaration with both removed it
exits 0: the ten-node bodies' failures are exactly `region.skin_containment`, `cover.range`,
`volume.visible_vs_top` and `volume.omitted_volume`, and `--write-expect` writes that declaration.
An independent sampling of det J on a lattice (a scratch script, outside the engine) agrees: no cell at
or below zero in the failed standing reference, least ratio 0.045723 at cell 496, the engine's bound
to every digit printed. The validation takes about 21 s a case in `msvc-debug`.

## The synthetic definition

`make_synthetic_tissue()` (`synthetic.h`) generates, from formulas, a slab cut from a torus — tube
radii 45 and 60 mm about a 300 mm major circle, 7 × 7 nodes on three layers, 432 tetrahedra — 2 mm
under a flat 41 × 41 skin whose observation rises 1 mm inside its domain, on a box frame 4 mm below
its lowest node, with a skin binding from the nearest-point binder under the limit rule, four states
(construction, a standing reference by inverse statics, a rest, a pressed response), a rim that is
the slab's whole side wall on every layer (so no moving surface node is off a sheet), a declared
solver (the runtime's 8 iterations at 1/120 s, and an implementation string that says the rest is
constructed, not solved — its equilibrium gap is correspondingly large) and a depth budget. It
passes every error row and fails `cover.range` (the slab curves away from a flat skin), which is
what it is for: the tests break one thing at a time and look for the row that says so. Its plane
shears read 0.49 to 0.78 of μ and its normal-stress differences 1.24 and 1.82: a slab of stretched
Kuhn cells has the fabric of a lattice.
`engine-content tissue example` writes it as an interchange and a container.

**The quadratic variant** (`SyntheticOptions::quadratic`, written beside it as
`synthetic-quadratic.*`) is the same slab as a ten-node reference body: a node on each of its 698
edges (845 nodes, which no runtime cage is, so `role` is `Reference`), placed on the torus at the
mean of its ends' parameters — so the cells curve a little, as a mesher's boundary-fitted ones do,
by at most 3.6% of an edge — and moved in each state by the bump at its own place; each sheet's
control triangle split in four over its edges' nodes at one Loop level less, so the dense surface is
the four-node slab's and the sheets still carry every moving surface node. It passes every error row
and fails only `cover.range`, and its exact volume is the solid torus section's to 4 parts in a
million (99.1515 against 99.1519 ml; the four-node slab's chords give 98.4045 and the same nodes'
subdivision 98.9647). It is the body the fixture-mode test declares two failures for.

## Testing

`tools/dev.ps1 test -Filter tissue`: `sha256_tests.cpp` (FIPS 180-4's examples, streaming in pieces
of every size, and hashes `hashlib` computed on the interchange's spellings: a topology and an f32
block); `tissue_file_tests.cpp` (a round trip through the container and back to the same bytes; a
section relabelled to an unknown kind skipped with a warning; an unknown definition field and an
unknown block kind name warned about and the rest read; a flipped byte, a block altered under a
repaired content hash, a truncation, a wrong magic and a wrong version refused; the interchange
round-tripped, and a short file, a changed byte, a wrong format, a count that does not match, a
duplicate name, an unknown field and a container-only kind refused); `mesh_query_tests.cpp` (the
winding number of a box, signed distances off faces, edges and corners, rays in and out, triangle
pairs crossing, missing and coplanar, two boxes' surfaces, SICN of a regular, flat, inverted and
sliver tetrahedron, and swept volumes that are exactly a translation's and a rigid motion's zero);
`validate_tests.cpp` (the synthetic definition passes every error row and every row the plan names
is present; the volumes, the decomposition identity, the reference's zero sweep and the cage against
the solid torus section's volume within 2%; and each of these fails with its witness: a node pushed
into the frame, a frame vertex lifted through the slab, a frame triangle wound backwards, one ulp
outside the domain, a wrong topology hash, a landmark on the wrong id, an inverted tetrahedron, a
node through the skin, a zero shear modulus, two footpoints swapped into a reversed image, a second
binding writing the same skin, a band id in the footprint, an inverse-statics state without its
prior, and a normal mode spelled any way but the three; a rest without a solver warned about and
one whose identity pins nothing refused; the equilibrium gap never failed, exactly zero at a rest
and under no gravity, and exactly a free node's weight less the air's when the reference is its
own rest; the slab's through-thickness counts, its patch test's isotropic and bulk ratios of one,
and a rest shrunk 3% read as 9.56% at every cell, with the load and the prior beside it; and the side
wall's middle layer released and pushed out 1 mm, 24 uncovered nodes and 1.78 ml omitted, with the
smooth rows labelled restricted, containment evaluated; a declared skin clearance floor of 20 mm
refused on a slab 2 mm under the skin, with the clearance on the tetrahedra's boundary and from the
skin to it reported beside the cap's; the support sheet's sweep exactly zero at the reference and its
own line at the response); `energy_tests.cpp` (the declared energy and its gradient zero at
rest; the gradient against central differences; a dilation reading K and the five shears' mean
reading μ on irregular cells; a regular cell's 1.25 and 0.625; a Poisson's ratio below 1/4 and an
inverted rest refused); `cells_tests.cpp` (a straight ten-node cell is its corner tetrahedron
exactly; a curved cell's volume against det J from the shape functions integrated by an exact Gauss
rule, to 1e-13; the Bernstein bounds against det J sampled on a lattice over 400 random cells,
including cells positive at every corner and inverted inside; the subdivision tiling the cell along
each of the three diagonals); `quadratic_tests.cpp` (the ten-node slab runs every row, fails only its
cover, reports its size as a budget, names the subdivision in the linear rows and holds the torus
section's volume; the same body as a runtime cage refused; a file in VTK's order and in the
lexicographic order each named; a crack between cells counted; a cell folded in a response inverted,
with the state in the witness; the container and the interchange carrying the kind);
`expect_tests.cpp` (the fixture regression test: the ten-node slab with its apex landmark declared
one ulp off, failing `cover.range` and `observation.landmarks` on purpose, matched by the published
v1 declaration of those two; a failure left undeclared, a declared row that passes, a severity moved,
a value outside its tolerance and a skipped row nobody declared each a difference; a first run's
written declaration matching that run; a declaration of failures alone written exactly as v1; a
declaration that is not one refused); the size table pins the two container records.
`apps/engine_content`'s end-to-end `tissue_tests.cpp` drives `example`, `import`, `info`,
`validate`, `report` and the fixture mode's `--write-expect` and `--expect`.

**Performance notes.** Content-build code, CPU, double precision in the geometric queries, not a
hot path: study019 validates and reports in about 10 s in `msvc-debug`, most of it the winding
numbers (275 nodes against 4,096 frame triangles, in four states) and the shell-against-skin
intersection pass; a supine fixture's ten-node body in about 21 s, its 1,635 nodes costing what
study019's 275 did six times over. A ten-node cell's Jacobian is 64 determinants and, only where
they do not decide, eight pieces a level to depth 4. Nothing here runs per frame.

## LOD policy and determinism stance

ADR-0027 asks both of a capability. **LOD**: none yet — the definition is authoring data, and the
tier policy it will feed is §5.14's (full tissue at the hero and near tiers, a spring on the
attachment points at mid, rest shape beyond), which belongs to the runtime that does not exist.
**Determinism**: a definition is authored, persistent data; its container's bytes are a function of
its content (canonical JSON, fixed section order), and the validators' numbers are a function of the
file (every traversal breaks ties by index). Nothing here enters the sim hash, because nothing here
simulates.

## Not yet

- **Resolved 2026-09-26: the interchange carries the certified reference body.** The ten-node cell
  kind, the reference role and the fixture mode above; the supine pair imported through them and
  then exported natively, the two files the regression fixture pins (above).
  What the three leave open: a curved face is tested against the frame and the skin by its four
  chords (the subdivision's boundary), not by a curved-triangle intersection; the declared energy
  (`bulk-edge-v0`) is the runtime's linear network, so the gap and the patch test on a ten-node body
  measure its subdivision, not the P2 potential it was certified under, and a P2 gap needs that
  potential and the D13 consistent load in the definition; positions are f32 in every block, so a
  float64 endpoint's identity is the sidecar's to keep (the supine block is exactly the f32 of it);
  a packet's `reference-p2.json` is the authoring side's sidecar and is converted, not read — step 2
  writes the kind natively; and a build older than this one refuses a container naming
  `TetrahedralQuadratic` outright rather than falling back, because an unknown enumerator is an
  error in `core/schema` where an unknown field or block kind is skipped.
No runtime solver: no element kind beyond the definition — the tetrahedra, membranes, cables and
attachments are described and checked, not simulated, and a cable's slack and recruitment are carried
without a law that reads them. No GPU pass: the transfer is the CPU reference in `domain/geometry`.
No content-build step: a `.tissue` is imported and validated, not derived into a cage, compliances
or a binding stream in the cluster pages. No resolution of canonical ids against a built mesh's
identity stream. No packed binding-record section (above). No transition-continuity row (the
dihedral turning at the band against the base's own, gated per sector on the maximum), no rim step
per sector, no ripple row on the thickness field, and no refinement-convergence row; the first two
need a sector definition (a chart and its angle origin) the schema does not carry yet, the last a
second fixture. No tetrahedralizer: the
tetrahedral cage kind is imported (the authoring side's Gmsh), never generated. The equilibrium gap
does not include membranes (a tension field on principal stress, Pipkin's relaxed energy) or cables
(the schema's slack and linear recruitment ramp is not the authoring kernel's cubic recruitment), so
it is complete only at the nodes neither reaches; a unilateral contact's reaction is removed without
checking its sign. **Owed from the round-seven review:** the gap as a **displacement** — the static
correction a Newton step or a static solve would make, p95 and max, with the **active support
features** (the face, edge or vertex of the support each sliding node sits on) in the projection
rather than one pseudonormal — which needs the declared energy's Hessian, a constrained solve, and
membrane and cable laws in the energy to be comparable; the authoring side's first measured row is
study019 at 0.067 mm standing and 0.026 mm supine. `bulk-edge-v0` is the network the runtime has, not a patch-test-exact continuum
shear element (a constant-strain tetrahedron with a deviatoric law), which a converged comparator
needs; and physics has `volume_compliance_for` but no edge counterpart, which this law's constant
(an edge coefficient of 5μ/2) is the candidate for once a lattice patch test measures it on Jolt.
