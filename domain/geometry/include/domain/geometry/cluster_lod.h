#pragma once

// The cluster LOD DAG (docs/plan/04-renderer.md §4.3, ADR-0005): every level of detail of a
// mesh as clusters in the shared format, plus the two error bounds per cluster that make a
// view-dependent, crack-free cut possible in one pass with no hierarchy traversal:
//
//   render cluster c  <=>  screen_error(c.own) <= threshold  &&  screen_error(c.parent) > threshold
//
// `own` is the bound of the group c was simplified from (error 0 for original geometry),
// `parent` the bound of c's own group's simplified output (infinite when the group could not
// be simplified further). Errors are monotonic along every path of the DAG, so exactly one
// cluster on each path passes for any threshold. The GPU pass in domain/gfx/shaders/
// cluster_cull.slang evaluates the same test as `lod_selects` below.
//
// Built with meshoptimizer's clusterlod (demo/clusterlod.h, MIT): clusterize, partition into
// groups, simplify each group, repeat. The simplifier is **told where the atlas seams are** and
// given the UVs as a weighted attribute, because a textured mesh cannot be simplified correctly
// without the first and is measurably better for the second: see `ClusterLodOptions` below and
// docs/subsystems/geometry.md, "What the simplifier is given, and why".

#include <domain/geometry/cluster.h>

namespace engine::geometry {

// GPU-mirrored; keep in step with ClusterLodDesc in the shaders. 48 bytes.
struct ClusterLodDesc {
  Vec4 own;          // xyz center, w radius, in mesh space
  Vec4 parent;       // xyz center, w radius
  f32 own_error;     // mesh-space error of this cluster (0: original geometry)
  f32 parent_error;  // mesh-space error of the coarser version; k_lod_terminal_error when none
  u32 level;         // DAG depth; 0 is the original geometry
  u32 group;         // index of the group this cluster belongs to
};

inline constexpr f32 k_lod_terminal_error = 3.402823466e+38f;  // FLT_MAX: never coarse enough

// What a collapse may do at a vertex where an attribute is discontinuous — the edge of a UV
// atlas island, a hard shading edge, a skin-weight split. It matters because clusterlod runs
// meshoptimizer's **permissive** simplification by default, and permissive means exactly "a
// collapse may cross an attribute discontinuity unless the vertex is tagged"
// (`meshopt_SimplifyPermissive`). Untagged, a simplified triangle can therefore span two
// unrelated parts of an atlas and interpolate the texture across both.
enum class SeamRule : u8 {
  // Tag nothing. What the builder did before 2026-09-19; kept so the defect can be measured
  // against the fix rather than described.
  none,
  // `meshopt_SimplifyVertex_Protect`: the vertex stays a seam vertex under permissive mode, so
  // a collapse may still slide *along* the seam but never *across* it. This is the rule
  // meshoptimizer's own cluster-LOD sample uses, and the default here.
  protect,
  // `meshopt_SimplifyVertex_Lock`: the vertex does not move at all. Strictly stronger than
  // `protect` and strictly more expensive in triangles; measured in geometry.md.
  lock,
};

struct ClusterLodOptions {
  u32 max_triangles = 124;   // per cluster, 4..256
  u32 max_vertices = 64;     // per cluster, at most 255
  bool ray_tracing = false;  // clusterlod's RT-oriented defaults (smaller, spatially compact)
  bool normal_cones = true;  // false: k_cone_none on every cluster (see ClusterBuildOptions)

  // ---- attribute-aware simplification ----------------------------------------------------
  //
  // The weights convert an attribute unit into a length. meshoptimizer normalizes the positions
  // of the subset it is simplifying by that subset's own extent and multiplies each attribute by
  // its weight, so a weight of w makes one unit of attribute error worth w times the *group's*
  // size of geometric error — a scale-free number, and the one knob that says how much of the
  // picture a triangle is allowed to cost. A weight of 0 drops the term and its cost (the
  // simplifier skips attributes with no weight); both zero and `SeamRule::none` on every seam
  // reproduces the position-only build exactly.
  //
  // The defaults are measured rather than inherited, and the measurement is worth knowing before
  // changing them (geometry.md has the table). A **UV** weight of 0.5 costs nothing on any mesh
  // measured — on a sane parametrization the UV is locally affine in the position, so the term is
  // nearly redundant with the position term and only bites where a collapse *shears* the texture,
  // which is what it is there for. A **normal** weight is a different animal: at 0.5 it makes a
  // 129x129 heightfield draw at full detail where it used to draw half, and four times the
  // triangles at four times the distance, because a normal delta is an absolute number that does
  // not shrink as the group does. It is therefore **off by default**: it buys shading accuracy
  // that the LOD threshold already buys more cheaply, and it does nothing at all for the atlas
  // defect the seam rules below exist for.
  f32 normal_weight = 0.0f;  // three floats, the vertex normal; 0.1 costs ~28%, 0.5 costs 2-4x
  f32 uv_weight = 0.5f;      // two floats, the first UV set
  // Where an atlas island ends. This is the one that fixes the defect: without it a group of
  // clusters spanning twenty islands simplifies as though the atlas were continuous.
  SeamRule uv_seams = SeamRule::protect;
  // Where a hard shading edge is. Off by default: it costs triangles on every mechanical model
  // and the normal *weight* already prices the shading error, while a UV discontinuity is not a
  // price but a discontinuity — see geometry.md for the measurement behind the difference.
  SeamRule normal_seams = SeamRule::none;
  // A skin binding is per vertex too, and `weld_vertices` deliberately keeps two coincident
  // vertices with different weights apart; merging their wedges here would undo that and hand
  // one surface the other's deformation. Protecting them costs nothing on a rigid mesh, which
  // has no such pairs at all.
  SeamRule skin_seams = SeamRule::protect;
  // A morph delta is per vertex for the third time, and the argument is the skin one word for
  // word: `weld_vertices` keeps two coincident vertices with different deltas apart, and merging
  // their wedges here would undo it and hand one surface the other's displacement — which, unlike
  // a shading change, opens a hole the moment the channel plays. Free on a mesh with no channels
  // and on one whose coincident vertices agree, which is every mesh a single exporter wrote for
  // a channel that does not stop at a seam.
  SeamRule morph_seams = SeamRule::protect;
  // A canonical vertex id is the fourth, and the argument is the same one more time:
  // `weld_vertices` keeps two coincident vertices the author gave different ids apart (two points
  // of the base that happen to touch — closed lips, the two sides of a module's boundary loop), and
  // merging their wedges here would give a coarse triangle one side's vertex on the other side's
  // surface, which a sidecar that moves the two apart turns into a crack. Free on every mesh
  // without authored ids: a derived id is a function of the position, so no two coincident vertices
  // ever disagree.
  SeamRule id_seams = SeamRule::protect;

  // ---- heightfield levels ------------------------------------------------------------------
  //
  // The direction a heightfield's ground faces, for a mesh that is one. Every level of such a
  // mesh is a single-valued surface over the plane across `up`, so no triangle of any level may
  // face away from it; a group whose simplified triangles do has folded the ground over itself,
  // and a ray leaving the lower sheet meets the fold (docs/experiments/shadow-shards-2026-10-09.md,
  // F17). With `up` set the builder refuses such a result (geometry.md, "Heightfield levels never
  // fold"). Zero, the default, for every other mesh, which is built exactly as before, byte for
  // byte. The terrain builders set it and nothing else does: a mesh with overhangs is not a
  // heightfield, and only a group whose input faces `up` everywhere is held to it.
  Vec3 up{};
  // With `up` set, false keeps every result the simplifier gives — the build without `up`, byte for
  // byte — and only counts, in `ClusterLodMesh::fold_guard`, the groups whose result turned a
  // triangle over. It is the "before" the guard's test and the fold census are taken against;
  // nothing that draws sets it.
  bool refuse_folds = true;
};

// What the heightfield guard saw during one build (`ClusterLodOptions::up`), counted by the
// builder and **not stored in a container**: a mesh read from a file reports zeros.
struct FoldGuardStats {
  u32 groups = 0;          // groups simplified with `up` set
  u32 regular_folded = 0;  // groups whose regular result first turned a triangle over
  u32 refused = 0;         // of those, refused and simplified again (`refuse_folds`)
  u32 retries = 0;         // the simplifications those took, all rounds
  u32 unsimplified = 0;    // groups that still folded after every round, left as they were
  u32 sloppy = 0;          // groups whose regular simplification got stuck short of the target
  u32 sloppy_folded = 0;   // of those, the ones whose sloppy result turned a triangle over
  u32 sloppy_refused = 0;  // of those, dropped for the regular result (`refuse_folds`)
};

struct ClusterLodMesh {
  ClusterMesh mesh;                  // every cluster of every level; level 0 comes first
  Vector<ClusterLodDesc> lod;        // parallel to mesh.clusters
  Vector<u32> level_cluster_counts;  // clusters per DAG level
  u32 group_count = 0;
  u32 leaf_triangle_count = 0;  // level 0 total, equal to the source triangle count
  FoldGuardStats fold_guard;    // the build's own; zero for a mesh read from a container
};

bool build_cluster_lod(std::span<const Vec3> positions, std::span<const u32> indices,
                       const ClusterLodOptions& options, ClusterLodMesh& out,
                       std::string* error = nullptr, const AttributeSource& attributes = {});

// ---- authored coarser levels (geometry.md, "Authored levels")
// ------------------------------------
//
// A caller that knows a better coarse form of its mesh than decimation does — a tree whose leaf
// cards merge into baked branch-group cards and then into a whole-tree impostor (flora.md, "The
// impostor chain") — hands those forms over as **levels above the finest**, each with the
// mesh-space error it stands at. The finest is decimated exactly as `build_cluster_lod` does, and
// the decimated chain is **cut where its error reaches the first level's**: a cluster whose own
// error is at or past it is dropped, and a survivor whose parent error is at or past it (or
// terminal) is re-parented to the first level. Level k's clusters then carry own error
// `levels[k].error` and parent error `levels[k + 1].error` (terminal for the last), and every one
// of those transitions carries **one sphere**, enclosing every vertex of every level and every
// sphere the decimated survivors carry, so a level switches as a whole and the cut stays exactly
// one cluster on every path for any view (the projected error is monotonic because the sphere
// encloses the spheres below it and the errors grow). An empty level is allowed last only: nothing
// draws past its error, which is how a part hands over to a level that another part of the same
// mesh carries (a tree's bark to its impostor; `widen_lod_spheres` then gives both parts one
// sphere). With no levels this is `build_cluster_lod`, byte for byte.
struct ClusterLodLevel {
  std::span<const u32> indices;  // triangles over the same `positions`; empty: nothing past here
  f32 error = 0.0f;              // mesh-space, greater than the level below's
};

bool build_cluster_lod_levels(std::span<const Vec3> positions, std::span<const u32> indices,
                              std::span<const ClusterLodLevel> levels,
                              const ClusterLodOptions& options, ClusterLodMesh& out,
                              std::string* error = nullptr, const AttributeSource& attributes = {});

// The sphere every authored transition at or past `from_error` carries in `mesh` (the own sphere of
// a cluster whose own error is at least it, the parent sphere of one whose finite parent error is),
// grown to enclose `with` when its radius is positive: what two parts of one mesh that share a
// transition are both widened to. Zero radius when the mesh has no such transition.
Vec4 authored_lod_sphere(const ClusterLodMesh& mesh, f32 from_error, Vec4 with = {}) noexcept;

// Replaces every sphere `authored_lod_sphere` reads with `sphere`. Fails, changing nothing, when
// `sphere` does not enclose one of them (a sphere that did not would let the projected error fall
// along a path, and the cut would then draw two levels or none).
bool widen_lod_spheres(ClusterLodMesh& mesh, f32 from_error, Vec4 sphere,
                       std::string* error = nullptr);

// ---- an authored hierarchy: one sphere a sub-tree (geometry.md, "Authored levels") -------------
//
// `build_cluster_lod_levels` gives every authored transition one sphere, so a 115 m crown switches
// whole from its near side. A caller that knows how its coarse forms **nest** — a tree's leaves
// each belong to one card of the first branch-group level, each card to one card of the next, the
// last to the whole-tree star — hands that nesting over instead, and every transition then carries
// the sphere of exactly what it replaces: the far side of a crown is cards while the near side is
// leaves. Level k's triangles are partitioned into **elements** (a card, a plane star), each with
// its parent element in level k + 1; the finest triangles each name their element of level 0.
//
// The builder groups top-down: the last level's elements in their order, consecutive runs of at
// most one cluster's worth (`max_vertices`, `max_triangles`); then each group's children in the
// order of their parents, runs again, never crossing their parent group; the finest triangles of
// each first-level group are clustered on their own. A group's clusters share one transition — its
// sphere, enclosing every vertex it stands for and every child group's sphere, and the level's
// error — and its children's parent transition is that same one, so the projected error rises along
// every path and the cut is one cluster on every path for any view (no decimation runs: a leaf
// cluster's own error is 0 and its parent is its first card group). The groups, in the DAG's sense
// (the clusters that share a parent), are the runs under one parent group.
struct ClusterLodHierarchyLevel {
  std::span<const u32> indices;              // triangles over the same positions
  std::span<const u32> element_of_triangle;  // per triangle of `indices`
  std::span<const u32> parent_of_element;    // per element: its element of the next level
  u32 elements = 0;
  f32 error = 0.0f;  // mesh-space, rising
};

bool build_cluster_lod_hierarchy(std::span<const Vec3> positions, std::span<const u32> indices,
                                 std::span<const u32> element_of_triangle,
                                 std::span<const ClusterLodHierarchyLevel> levels,
                                 const ClusterLodOptions& options, ClusterLodMesh& out,
                                 std::string* error = nullptr,
                                 const AttributeSource& attributes = {});

// ---- the foliage error (geometry.md, "Two errors") ---------------------------------------------
//
// A transition may carry, beside its geometric error, a **coverage error**: what drawing the
// coarser form changes at the pixel for foliage, where a card is judged by its coverage and colour
// and not by the parallax of a point off its plane (flora.md, "The impostor chain"). It is a
// length, so it projects like the geometric one, and it is held to its own threshold
// (`LodView::foliage_px`). A transition is **acceptable** when either projected error is at or
// under its threshold:
//
//   acceptable(t) = proj(t.sphere, t.error) <= threshold_px || proj(t.sphere, t.coverage) <=
//   foliage_px draw c        = acceptable(c.own) && !acceptable(c.parent)
//
// which needs no division and, with no coverage (+inf), is the one-threshold rule bit for bit. One
// u32 per cluster carries the own and the parent coverage as two halves (`pack_lod_coverage`, low
// half own); a mesh with none has none. The builder of a mesh with coverage keeps both errors
// rising along every path, so the cut is still one cluster on every path.
inline constexpr u32 k_lod_no_coverage = 0x7C007C00u;  // both halves +inf: the geometric rule
// The foliage threshold's default, `renderer.lod.foliage_px` (renderer.md, "The foliage threshold"
// has where the number comes from).
inline constexpr f32 k_default_foliage_px = 5.0f;
u32 pack_lod_coverage(f32 own, f32 parent) noexcept;  // rounded to half floats
f32 lod_coverage_own(u32 packed) noexcept;
f32 lod_coverage_parent(u32 packed) noexcept;

// View-dependent selection, the reference for the GPU pass. Errors are projected to screen
// pixels the way clusterlod documents: error / max(distance - radius, znear) * proj_scale.
struct LodView {
  Vec3 camera{};
  f32 znear = 0.1f;
  // cot(fov_y / 2) * viewport_height / 2: mesh-space size at distance 1 to pixels.
  f32 proj_scale = 0.0f;
  f32 threshold_px = 1.0f;
  // The coverage errors' threshold (`renderer.lod.foliage_px`): read only for a cluster with one.
  f32 foliage_px = 0.0f;
  // The cross-fade's half band, as a fraction of the threshold (renderer.md, "The cross-fade"): 0
  // switches a transition at once; f fades it over projected errors from (1 - f) to (1 + f) times
  // the threshold. Read only for a transition with a coverage error.
  f32 fade = 0.0f;
};
f32 projected_error(Vec4 sphere, f32 error, const LodView& view) noexcept;
bool lod_selects(const ClusterLodDesc& lod, const LodView& view) noexcept;
bool lod_selects(const ClusterLodDesc& lod, const LodView& view, u32 coverage) noexcept;
// The cross-fade's split of one transition: 0 when its coarser side is acceptable outright, 1 when
// it is not at all, and between them across the band, by the smaller of the two projected errors
// over its threshold. A cluster draws the pixels whose stable hash h (in [0, 1)) has
// split(own) <= h < split(parent), so the two sides of a transition share every pixel between them
// and exactly one cluster of every path keeps each pixel. A transition with no coverage, or a fade
// of 0, splits at 0 or 1 by `lod_selects`' rule; `step_shift` moves that rule's threshold by
// (1 - fade), which is how an opaque cluster of a fading mesh stays until its masked replacement is
// whole (renderer.md, "The cross-fade").
f32 lod_fade_split(Vec4 sphere, f32 error, f32 coverage, const LodView& view,
                   bool step_shift = false) noexcept;
// Appends the indices of the selected clusters; returns how many.
u32 select_lod(const ClusterLodMesh& mesh, const LodView& view, Vector<u32>& out);
// The same with a coverage word per cluster (empty: none).
u32 select_lod(const ClusterLodMesh& mesh, const LodView& view, std::span<const u32> coverage,
               Vector<u32>& out);
// The same with a threshold on raw mesh-space error (no camera): own <= t < parent.
u32 select_lod_raw(const ClusterLodMesh& mesh, f32 threshold, Vector<u32>& out);

// Merges DAGs built over parts of one mesh (a glTF primitive per material, for example) into
// one: the vertex, attribute, and triangle streams are concatenated with the offsets shifted,
// source vertex indices and group indices shift by the parts before, level counts add up, and
// the clusters are reordered so every level-0 cluster comes first (the direct-draw convention;
// the cut test is per cluster and does not care about order). `part_of_cluster`, when given,
// receives the part index of every merged cluster in output order, which is how a caller maps
// clusters back to materials. Fails on an empty list or an empty part.
bool merge_cluster_lod(std::span<const ClusterLodMesh> parts, ClusterLodMesh& out,
                       Vector<u32>* part_of_cluster = nullptr, std::string* error = nullptr);

// Where one mesh landed in the buffers merge_cluster_meshes built, and the 16-bit grid its
// positions stayed on. A renderer uploads one of these per mesh (`gfx::MeshDesc` mirrors the
// first two fields and the grid) and indexes it by an instance's mesh id.
struct ClusterMeshPart {
  u32 first_cluster = 0;  // in the merged `mesh.clusters`
  u32 cluster_count = 0;
  u32 leaf_cluster_count = 0;  // the level-0 clusters of this mesh
  // Where they start, relative to `first_cluster`. Zero for a mesh merged the default way, whose
  // leaves come first; `cluster_count - leaf_cluster_count` for a mesh that was laid out in pages
  // before the merge, because a paged mesh is ordered coarse to fine and its leaves are last.
  u32 first_leaf_cluster = 0;
  u32 first_vertex = 0;  // in the merged vertex, attribute, and quantized streams
  // This mesh's run of the scene's page table (`merge_paged_cluster_meshes`), zero and empty when
  // the scene carries none.
  u32 first_page = 0;
  u32 page_count = 0;
  Vec3 quant_origin{};  // this mesh's own grid, unchanged by the merge
  f32 quant_scale = 1.0f;
  // This mesh's run of the scene's morph channel array. Separate meshes have separate channel
  // sets — one character's "smile" is not another's — so the merge concatenates them and a slice
  // of this mesh names `first_morph_channel + k`. Zero and zero for a mesh with no channels.
  u32 first_morph_channel = 0;
  u32 morph_channel_count = 0;
  // Where this mesh's canonical vertex ids came from. Each mesh of a scene has its own id space —
  // two meshes' id 7 are unrelated — so the merge concatenates the streams and says per mesh what
  // the ids at [first_vertex, next mesh's first_vertex) mean; `none` for a mesh that carried none,
  // whose run of the merged stream is `k_no_vertex_id` when another mesh's ids made it exist.
  VertexIdSource vertex_id_source = VertexIdSource::none;
};

// Whether a merge reorders each mesh's clusters so its level-0 clusters come first.
//
// `leaves_first` is the scene convention: a direct draw of the leaves is then the first
// `leaf_cluster_count` clusters of the mesh. `keep` leaves a mesh's clusters in the order it
// arrived in, which is what a mesh laid out in **pages** needs: `build_cluster_pages` orders the
// clusters coarse to fine so that each page is a contiguous run of them, and pulling the leaves
// to the front would leave every page naming clusters that are no longer beside each other.
enum class ClusterOrder : u8 { leaves_first, keep };

// Merges DAGs built over *separate meshes* into the one set of buffers a scene draws from: the
// vertex, attribute, triangle, and quantized streams are concatenated with the offsets shifted,
// exactly as merge_cluster_lod does, with two differences that matter for a scene.
//
//   - Each mesh keeps its **own 16-bit position grid**. `quantized` is concatenated untouched
//     (three u16 per vertex at the merged vertex index, the whole stream padded to an even
//     count), and `parts_out[m]` reports the origin and step to decode a vertex of mesh m with.
//     Quantizing the scene on one grid instead would spend the 16 bits on the distance between
//     meshes rather than on each mesh's own detail.
//   - Every mesh's clusters stay **contiguous**, so `{first_cluster, cluster_count}` names a
//     mesh's clusters and an instance reaches cluster `first_cluster + local`. Within a mesh the
//     level-0 clusters still come first, so a direct draw of the leaves is the first
//     `leaf_cluster_count` of them.
//
// `out.quant_origin`/`quant_scale` are the first mesh's, so a single-mesh merge behaves as
// before; every other mesh's grid is in `parts_out`, and `dequantize_position` on the merged
// mesh is therefore only correct for the first. Fails on an empty list or an empty part.
//
// `order` is `ClusterOrder::keep` when the meshes have been laid out in pages; see the enum, and
// `merge_paged_cluster_meshes` in cluster_pages.h, which is what a caller with page tables uses.
//
// **A merge may reorder a mesh's clusters**, and a caller that keeps anything per cluster of a
// mesh — a material per cluster above all — has to reorder it the same way. `source_of_cluster`,
// when given, receives for every merged cluster the index it had in its own mesh
// (`parts[m].mesh.clusters`), so mesh m's run of it, `[first_cluster, first_cluster +
// cluster_count)`, is exactly what `permute_cluster_array` (cluster_pages.h) takes. With `keep` it
// is the identity; with `leaves_first` it is too for a mesh built by `build_cluster_lod` (leaves
// already first) and is not for one laid out in pages — which is every mesh read from a
// `.clusters` container. Until 2026-09-24 the renderer did not take it, and every scene of more
// than one mesh drew its multi-material meshes' clusters with other clusters' materials
// (docs/experiments/first-interactive-session-2026-09-24.md).
bool merge_cluster_meshes(std::span<const ClusterLodMesh> parts, ClusterLodMesh& out,
                          Vector<ClusterMeshPart>& parts_out, std::string* error = nullptr,
                          ClusterOrder order = ClusterOrder::leaves_first,
                          Vector<u32>* source_of_cluster = nullptr);

// Level 0 covers every source triangle exactly once; every cluster's own error is at most its
// parent error; the raw cut is non-empty for every threshold; the triangle count of the raw
// cut never grows with the threshold. For a merged mesh, `source_indices` is the parts' index
// lists concatenated with each shifted by the source vertex counts of the parts before it.
bool validate_cluster_lod(const ClusterLodMesh& mesh, std::span<const u32> source_indices,
                          std::string* error = nullptr);

// ---- folds in a heightfield's levels -------------------------------------------------------
//
// A heightfield's every level is a single-valued surface over the plane across its `up`, so no
// triangle of it faces away from `up` and no two triangles of it overlap seen along `up`. This is
// the census of both, per DAG level: the check the heightfield guard (`ClusterLodOptions::up`) is
// held to, and the diagnostic that found where the terrain's folds were made (geometry.md,
// "Heightfield levels never fold"; `engine-content folds`).
//
// A triangle is **upside down** when its geometric normal (counter-clockwise front) is more than
// `k_fold_facing` past square to `up` — 90.06 degrees: a skirt hung straight down from a border
// faces exactly sideways and is not counted, and the census's folds were 105 to 166 degrees from
// the ground they covered. Two triangles **overlap** when their shadows on the ground plane share
// area (a separating-axis test in double, edges and corners shared exactly do not count); only
// pairs within one cluster are tested, which is what a ray leaving one triangle meets first, and a
// triangle standing on edge (its shadow a segment) overlaps nothing.
inline constexpr f64 k_fold_facing = 1.0 / 1024.0;  // cosine past square: 90.056 degrees

struct LodFoldLevel {
  u32 clusters = 0;
  u32 triangles = 0;
  u32 upside_down = 0;      // triangles facing away from `up`
  u32 overlapping = 0;      // triangles whose shadow overlaps another's of the same cluster
  u32 folded_clusters = 0;  // clusters holding either
};

// One entry per DAG level. `on_grid` reads the positions the rasterizers draw, off the 16-bit grid
// (`dequantize_position`), rather than the floats the builder produced, so a fold quantization
// made is told apart from one the simplifier made. Fails on an empty mesh, a zero `up`, or
// `on_grid` on a mesh with no grid.
bool count_lod_folds(const ClusterLodMesh& mesh, Vec3 up, bool on_grid, Vector<LodFoldLevel>& out,
                     std::string* error = nullptr);

// ---- how much of the *picture* a cut costs ------------------------------------------------
//
// `ClusterLodDesc`'s two errors bound how far a cut has moved the **surface**. They say nothing
// about how far it has moved the surface's **attributes**, and on a textured mesh that is the
// term a viewer sees first: a simplified triangle that interpolates its UVs across two unrelated
// atlas islands paints one part of the model with another part's texture while its silhouette is
// still within a pixel of the truth. This is the CPU measurement that catches that, needs no GPU,
// and is what the seam rules above are judged by.
//
// The method: sample the **finest** surface (the level-0 clusters), find the closest point on the
// triangles of `cut`, interpolate both meshes' stored attributes at those two points, and report
// how far apart they are. Closest point rather than a ray along the normal because it is
// unconditionally defined and needs no tolerance of its own; the cut approximates the fine
// surface, so the two agree wherever the simplification was honest.
//
// UV distance is reported in **texels of an atlas of `uv_texels` a side**, because that is the
// unit the damage is visible in and the unit a content author reasons in. The floor of the
// measurement is the format's own: `VertexAttributes` stores a UV as two half floats, whose
// spacing near 1.0 is about 0.0005 — two texels of 4096 — so a tolerance under about four texels
// measures the encoding rather than the simplifier.
struct AttributeErrorOptions {
  f32 uv_texels = 4096.0f;           // the atlas size a UV delta is reported in texels of
  f32 uv_tolerance_texels = 8.0f;    // over this, a sample counts as an outlier
  f32 normal_tolerance_deg = 20.0f;  // over this, a sample counts as an outlier
  // Cap on the samples taken, so a 540 k-triangle model is measurable in a test's worth of time.
  // Level-0 triangles are taken at a fixed stride, so the result is a function of the mesh alone
  // and two runs agree to the bit. 0 takes every triangle.
  u32 max_samples = 20000;
};

struct AttributeError {
  u32 samples = 0;
  u32 uv_outliers = 0;      // samples whose UV moved more than the tolerance
  u32 normal_outliers = 0;  // samples whose normal turned more than the tolerance
  u32 unmatched = 0;        // samples with no triangle of the cut anywhere near them
  f32 uv_mean_texels = 0.0f;
  f32 uv_p99_texels = 0.0f;
  f32 uv_max_texels = 0.0f;
  f32 normal_mean_deg = 0.0f;
  f32 normal_p99_deg = 0.0f;
  f32 normal_max_deg = 0.0f;
  // The headline number: the share of the finest surface a coarse cut paints with the wrong part
  // of the atlas. On a clean simplification it is a fraction of a percent; a build that collapses
  // across islands puts it in the tens of percent.
  f32 uv_outlier_fraction() const noexcept {
    return samples == 0 ? 0.0f : static_cast<f32>(uv_outliers) / static_cast<f32>(samples);
  }
  f32 normal_outlier_fraction() const noexcept {
    return samples == 0 ? 0.0f : static_cast<f32>(normal_outliers) / static_cast<f32>(samples);
  }
};

// `cut` is a list of cluster indices — what `select_lod`, `select_lod_raw` or a level's own range
// produced. Fails on a mesh with no attributes, an empty cut, or an index out of range.
bool measure_lod_attribute_error(const ClusterLodMesh& mesh, std::span<const u32> cut,
                                 const AttributeErrorOptions& options, AttributeError& out,
                                 std::string* error = nullptr);

}  // namespace engine::geometry
