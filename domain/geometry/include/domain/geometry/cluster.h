#pragma once

// The cluster geometry format, runtime side (docs/plan/04-renderer.md §4.3, ADR-0005). A mesh
// is a list of clusters, each a small set of vertices (at most 64 today) and local-index
// triangles (at most 124), with a bounding sphere. Mesh shaders draw one cluster per workgroup,
// the software rasterizer takes clusters below a projected size, and acceleration structures
// are built from the same clusters (Phase 2). The layout below is what the GPU reads through
// device addresses, so it is fixed and pinned by the size table.
//
// v1 stores positions twice: as three floats per vertex in cluster order (what the acceleration
// structure builders read) and as three u16 on one mesh-wide grid (what the rasterizers and the
// resolve read, six bytes a vertex instead of twelve), plus packed normals and UVs per vertex
// and a normal cone per cluster for backface culling; the LOD DAG with error bounds is
// cluster_lod.h. Fixed-size pages for streaming follow.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>

#include <span>
#include <string>

namespace engine::geometry {

// A packed normal cone that never culls: zero axis, cutoff 1 (127/127).
inline constexpr u32 k_cone_none = 0x7f000000u;

// The slack, in radians, every cone keeps around every normal it holds, and in front of its apex
// (docs/subsystems/geometry.md, "Normal cones"). It is what makes the cone robust to a normal
// computed some other way than the builder computed it: another compiler or baseline (FMA
// contraction), the GPU, or a world transform the cull pass treats as uniform although its scales
// differ by up to 1e-4 (gfx::set_instance_transform), which can turn a normal by that much. The
// transform dominates, rounding is 1e-6 and below, and 1e-3 covers both ten times over for 0.06
// degrees of culling.
inline constexpr f32 k_cone_margin = 1.0e-3f;
// Its cosine and sine, of the double it widens to (0x1.0624dep-10), correctly rounded and written
// out, so that the cone refit adds the margin by the angle-sum identities and calls nothing from
// the C library: MSVC's CRT and glibc need not agree on std::cos in the last bit, and a build's
// output is a function of its input on every toolchain (geometry.md, "The same bytes from every
// toolchain"). Change the margin and these with it; the determinism test checks all three agree.
inline constexpr f64 k_cone_margin_cos = 0x1.ffffef39085c0p-1;   // 0.9999994999999942
inline constexpr f64 k_cone_margin_sin = 0x1.0624db22fdff9p-10;  // 0.0009999998808307693

// GPU-mirrored; keep in step with the ClusterDesc struct in shaders. 48 bytes.
struct ClusterDesc {
  u32 vertex_offset = 0;    // first vertex in ClusterMesh::vertices
  u32 triangle_offset = 0;  // first packed triangle in ClusterMesh::triangles
  u32 vertex_count = 0;
  u32 triangle_count = 0;
  Vec3 center{};  // bounding sphere
  f32 radius = 0.0f;
  // Normal cone for backface culling (meshoptimizer's form): the cluster is entirely
  // backfacing when dot(normalize(cone_apex - camera), axis) >= cutoff. `cone` packs the axis
  // as three snorm8 (bytes 0..2, x/127) and the cutoff as a snorm8 (byte 3); a cutoff of 1
  // (k_cone_none) marks a cluster that is never culled: two-sided, or normals too spread. The
  // cone is refit by quantize_positions to the triangles as floats and on the grid, with
  // k_cone_margin of slack.
  Vec3 cone_apex{};
  u32 cone = k_cone_none;
};

struct NormalCone {
  Vec3 axis{};        // decoded as stored: within a snorm8 step of unit length
  f32 cutoff = 1.0f;  // sin of the half-angle of the normal spread; 1 never culls
};

// Snorm8 packing of a cone; the cutoff rounds up so quantization never culls more.
u32 encode_cone(Vec3 axis, f32 cutoff) noexcept;
NormalCone decode_cone(u32 packed) noexcept;
// The CPU reference of the cull pass's cone test, on the packed values it reads.
bool cluster_backfacing(const ClusterDesc& cluster, Vec3 camera) noexcept;

// GPU-mirrored per-vertex attributes, 8 bytes, cluster-ordered like ClusterMesh::vertices: an
// octahedral normal in two snorm16 and a UV in two half floats. Keep in step with the shaders.
struct VertexAttributes {
  u32 normal_oct = 0;
  u32 uv_half2 = 0;
};

// GPU-mirrored per-vertex skin binding, 8 bytes, cluster-ordered like `vertices` and
// `attributes` (docs/plan/04-renderer.md §4.3, docs/plan/05-simulation.md §5.11): four joint
// indices and four weights, the weights summing to **exactly 255**. Keep in step with the
// SkinBinding struct in the shaders.
//
// Eight bytes is the whole budget: four influences is what every exporter writes and what a
// mesh-shader workgroup can afford to load, and a u8 joint index caps a skin's palette at
// `k_max_skin_joints`, which is more than a humanoid with fingers and toes needs. A mesh that
// wants more joints is split into several skinned meshes, which is also how it would be
// streamed. The weights sum to a fixed total rather than being renormalized on the GPU so that
// the shader's four multiply-adds need no divide and every copy of a surface point gets bitwise
// identical weights — the property the crack rule rests on (see gfx.md, "Deformed clusters").
struct SkinBinding {
  u8 joints[4] = {0, 0, 0, 0};
  u8 weights[4] = {255, 0, 0, 0};  // a default binding is rigid to joint 0, and already valid
};
static_assert(sizeof(SkinBinding) == 8, "SkinBinding is an 8-byte GPU-mirrored record");

// A u8 joint index: a skin binds to at most this many joints.
inline constexpr u32 k_max_skin_joints = 256;

// Quantizes up to four influences into a SkinBinding. The weights are normalized and rounded to
// sum to exactly 255 (largest remainder, so the rounding lands on the biggest influence), an
// influence with no weight gets joint 0 and weight 0, and a vertex with no weight at all binds
// rigidly to `joints[0]`. Joint indices at or above k_max_skin_joints are clamped: the callers
// that can tell (the glTF importer) refuse such a skin before reaching here.
SkinBinding make_skin_binding(const u32 joints[4], const f32 weights[4]) noexcept;

// ---- morph channels ----------------------------------------------------------------------------
//
// A **morph channel** is a named per-vertex displacement of the rest mesh, played at a weight: a
// facial expression, a corrective that fires with a joint angle, or one of the shape channels a
// parametric character is baked from. glTF calls them morph targets; the consumers are
// docs/plan/04-renderer.md §4.3 and the deform chain in gfx.md.
//
// Two properties decide the layout, and both come from what the GPU pass has to do.
//
// **Sparse.** A channel touches a region — a brow, a jaw, one seam — and a character carries
// dozens of them. Dense storage would be `channels x vertices` deltas of which nearly all are
// zero: fifty channels over a 300,000-vertex character is 1.8 GB dense and a few megabytes
// sparse. So a channel stores only the vertices it moves.
//
// **Cluster-ordered.** The deform pass runs one workgroup per *visible cluster*, so what it needs
// is "which channels touch this cluster, and where are their deltas" — not "where are channel
// k's deltas". The directory is therefore keyed by cluster (`ClusterMesh::morph_cluster_slices`,
// a CSR over `morph_slices`) and a cluster's deltas are one contiguous run per channel. A cluster
// no channel touches costs one `u32`.
//
// **Quantized to 16 bits with a per-channel scale.** The delta is `i16 * MorphChannel::scale`,
// and the scale is the channel's own largest absolute component over 32767 — its own extent, not
// the mesh's. That is what makes the quantization free rather than merely cheap: the position the
// delta is added to is itself on a 16-bit grid over the *whole mesh's* extent
// (`quantize_positions`), so a channel whose displacement is a fraction of the mesh is stored on
// a grid finer than the position it modifies by exactly that fraction. See geometry.md for the
// measured error.

// GPU-mirrored, 16 bytes, one per channel of a mesh. Keep in step with `MorphChannel` in the
// shaders. `position_scale` and `normal_scale` are what the deform pass reads; `max_displacement`
// is what a renderer computes an instance's `bounds_padding` from, and `default_weight` is the
// weight the source authored, which an app uses when nothing else says.
struct MorphChannel {
  f32 position_scale = 0.0f;    // position delta = i16 * this, in the mesh's own units
  f32 normal_scale = 0.0f;      // normal delta = i16 * this; 0: this channel has no normal deltas
  f32 max_displacement = 0.0f;  // the largest |position delta| the channel holds
  f32 default_weight = 0.0f;    // what the source said this channel plays at by default
};
static_assert(sizeof(MorphChannel) == 16, "MorphChannel is a 16-byte GPU-mirrored record");

// GPU-mirrored, 12 bytes: one channel's run of deltas inside one cluster. A cluster's slices are
// `morph_slices[morph_cluster_slices[c] .. morph_cluster_slices[c + 1])`, in ascending `channel`
// order with no repeats, and their delta runs are contiguous and in slice order — which is what
// makes a page's morph payload one range of each stream after `build_cluster_pages`.
struct MorphSlice {
  u32 channel = 0;
  u32 first_delta = 0;  // into ClusterMesh::morph_indices, morph_deltas, morph_normal_deltas
  u32 delta_count = 0;
};
static_assert(sizeof(MorphSlice) == 12, "MorphSlice is a 12-byte GPU-mirrored record");

// One channel as its source describes it, before clustering: the vertices it moves, indexed like
// the source positions, and their deltas. `vertices` is strictly increasing and the two delta
// arrays are parallel to it; `normal_deltas` may be empty, which means the channel moves
// positions only. A delta of exactly zero may be listed or left out — the builders keep what they
// are given, so an importer that drops near-zero deltas is what makes the stream sparse.
struct MorphChannelSource {
  std::string name;
  f32 default_weight = 0.0f;
  Vector<u32> vertices;
  Vector<Vec3> position_deltas;
  Vector<Vec3> normal_deltas;
};

// ---- canonical vertex identity ------------------------------------------------------------------
//
// A **vertex id** names a vertex of the *canonical base* a mesh was authored against, so that a
// file written beside the mesh — a binding, a region's membership, a corrective residual — can
// name vertices and still mean the same ones after the content build (docs/plan/07-content-
// pipeline.md §7.11, "identity has to survive the weld"). Without it, the only name a built vertex
// has is `vertex_source`, the index the weld gave it, and that changes whenever anything about the
// weld's input does.
//
// The id lives at the **control level**: every render vertex standing for one point of the base
// carries that point's id, so the two sides of a UV seam, a hard shading edge or a skin-weight
// split share it. A binding that names a *refined* point (a limit-surface vertex) is a different
// kind of name and is not this one; it waits for the limit-surface pass.
//
// Ids are **host-side only**: they are not in `VertexAttributes`, not uploaded, and not counted in
// a streaming page's bytes, because nothing on the GPU reads them. What they cost is four bytes a
// cluster vertex in the container and in a loaded mesh.

// Where a mesh's ids came from. Stored in the container (`ClusterSection::VertexIdScalars`), so a
// file written beside the mesh can check it is naming vertices in the same id space. The derivation
// rule is part of the value: a change to how `position_weld` ids are computed takes a **new**
// value, never this one, so a sidecar keyed on the old rule is told rather than silently wrong.
enum class VertexIdSource : u32 {
  none = 0,           // no ids: the stream is empty
  authored = 1,       // the source said, per vertex (glTF `_CANONICAL_ID`, assets/gltf.h)
  position_weld = 2,  // derived, rule 1: the rank of the vertex's position among the source's
                      // distinct positions (`position_weld_ids`)
};

// The id of a vertex the source gave none: a primitive without the authored attribute in a file
// whose other primitives carry it. It is reserved — an authored id may not be this value — and
// every such vertex carries it, so to the weld they are "no identity" and merge on their other
// attributes exactly as they did before ids existed.
inline constexpr u32 k_no_vertex_id = ~0u;

// "none", "authored", "position_weld", or "unknown" for a value a newer build wrote.
const char* vertex_id_source_name(VertexIdSource source) noexcept;

// The base's own rule for a mesh that carries no authored ids: a vertex's id is **the index of its
// position in a position weld of the source**, where the weld's output is ordered by the positions
// themselves — lexicographically by x, then y, then z, in numeric order, with -0 and +0 one
// position. So two vertices at the same position (the two sides of a UV seam, an exporter's
// duplicates) get the same id, every distinct position gets its own, the ids are dense (0 to the
// returned count minus one), and they are a function of the **set** of positions alone: the same
// mesh with its vertices in a different order gets the same id at every position. Sorted rather
// than hashed so that nothing depends on a table's iteration order. Fills `out` with one id per
// position and returns how many distinct positions there are.
//
// What it cannot do is survive an edit: moving, adding or removing a vertex changes the ranks of
// every position after it in that order. A base whose sidecars must outlive edits to its own mesh
// authors its ids (`VertexIdSource::authored`); this rule is what a mesh gets when nobody did.
u32 position_weld_ids(std::span<const Vec3> positions, Vector<u32>& out);

// Optional source attributes for the builders, indexed like the source positions. Empty normals
// are computed from the faces (area-weighted, smooth); empty UVs are zero.
struct AttributeSource {
  std::span<const Vec3> normals;
  std::span<const Vec2> uvs;
  // Optional per-vertex skin bindings. Empty leaves the built mesh unskinned; `joint_count` is
  // the width of the palette they index, which is what `validate_clusters` checks them against
  // and what a renderer sizes an instance's bone-matrix array by.
  std::span<const SkinBinding> skin;
  u32 joint_count = 0;
  // Optional morph channels over the same source vertices. Empty leaves the built mesh with no
  // morph stream at all, which is what every rigid mesh gets and what keeps its bytes identical.
  std::span<const MorphChannelSource> morph;
  // Optional canonical vertex ids over the same source vertices, and where they came from. Empty
  // leaves the built mesh with no id stream (`VertexIdSource::none`).
  std::span<const u32> vertex_ids;
  VertexIdSource vertex_id_source = VertexIdSource::none;
  // Optional per-vertex locks over the same source vertices: a non-zero entry is a vertex the LOD
  // simplifier never moves (`meshopt_SimplifyVertex_Lock`, and every vertex at its position with
  // it). For a mesh built in parts whose DAGs are merged (`merge_cluster_lod`): lock the vertices
  // two parts share and the parts meet exactly at every level. Empty locks nothing, and costs
  // nothing. Only `build_cluster_lod` reads it.
  std::span<const u8> locked;
};

u32 encode_normal_oct(Vec3 normal) noexcept;
Vec3 decode_normal_oct(u32 packed) noexcept;
u32 encode_half2(Vec2 v) noexcept;
Vec2 decode_half2(u32 packed) noexcept;
// One IEEE half float, round to nearest even: what `encode_half2` packs two of, and the surface
// binding's normal offset (surface_binding.h) stores one of.
u16 f32_to_f16(f32 value) noexcept;
f32 f16_to_f32(u16 half) noexcept;
// Area-weighted smooth normals; isolated vertices get +Y.
void compute_vertex_normals(std::span<const Vec3> positions, std::span<const u32> indices,
                            Vector<Vec3>& out);

// Merges vertices whose position, normal, UV, skin binding, morph deltas, **and canonical id** are
// bit-identical and drops unreferenced ones, rewriting `indices` in place and compacting the
// streams (`normals`, `uvs`, `skin` and `vertex_ids` take part when they are parallel to
// `positions`, and are left alone otherwise). Exporters often write unindexed or seam-split meshes;
// without welding, the 64-vertex cluster limit caps clusters at 21 triangles and the LOD builder
// has no connectivity to simplify across. Vertices that differ in any attribute stay distinct, so
// seams keep their normals and UVs — and **two duplicates with different skin weights or different
// morph deltas stay two vertices**, because merging them would silently pick one vertex's
// deformation for the other's surface. **Two duplicates with different ids stay two vertices** for
// the same reason one level up: the author said they are two points of the base, and a sidecar
// may move them apart. Equal ids never force a merge; they only permit one the other streams
// already agree on. Returns the new vertex count.
//
// **How the morph deltas reach the key without costing vertices x channels.** The obvious form —
// a dense per-vertex row of every channel's delta — is megabytes a vertex-pass on a character and
// is exactly what the sparse layout exists to avoid. Instead every source vertex is given a
// 32-bit **id**: the quantized deltas that touch it, in channel order, interned through a hash
// map that compares bytes on a collision, so equal ids mean bit-identical delta tuples and there
// is no false merge to reason about. The interning walks the sparse data, so it costs the deltas
// and not the vertices, and a vertex no channel touches gets id 0 without being visited at all.
// The id is then one more 4-byte stream of meshoptimizer's multi-stream remap, beside the others.
//
// `morph`, when given, is **also remapped in place**: the weld renumbers vertices, so each
// channel's `vertices` array is rewritten to the new indices, re-sorted, and deduplicated (two
// vertices that merged necessarily carried identical deltas, so keeping one is exact), and an
// entry for a vertex the weld dropped is removed.
u32 weld_vertices(Vector<Vec3>& positions, Vector<Vec3>& normals, Vector<Vec2>& uvs,
                  std::span<u32> indices, Vector<SkinBinding>* skin = nullptr,
                  Vector<MorphChannelSource>* morph = nullptr, Vector<u32>* vertex_ids = nullptr);

// The interning above, exposed because the weld is not the only place that wants to ask "do these
// two vertices carry the same morph deltas?". Fills `out` with one id per source vertex — 0 for a
// vertex no channel touches — and returns how many distinct ids it used. Deterministic: ids are
// handed out in order of first appearance while walking the channels in order and each channel's
// vertices in order, so two runs over the same source give the same ids.
u32 morph_vertex_keys(std::span<const MorphChannelSource> channels, u32 vertex_count,
                      Vector<u32>& out);

struct ClusterBuildOptions {
  u32 max_vertices = 64;    // at most 255 (local indices are bytes)
  u32 max_triangles = 124;  // at most 512 and a multiple of 4 (meshoptimizer)
  // 0 optimizes for reuse and locality; up to 1 trades that for tighter normal cones.
  f32 cone_weight = 0.0f;
  // False stores k_cone_none on every cluster, for two-sided meshes that must never be
  // backface culled.
  bool normal_cones = true;
};

struct ClusterMesh {
  Vector<ClusterDesc> clusters;
  Vector<Vec3> vertices;                // cluster-ordered copies of source positions
  Vector<u32> vertex_source;            // source vertex index per entry of `vertices`
  Vector<VertexAttributes> attributes;  // parallel to `vertices`
  // Per-vertex skin bindings, parallel to `vertices`, or empty for an unskinned mesh. It is an
  // optional stream rather than part of `VertexAttributes` because the overwhelming majority of
  // a scene's vertices are rigid, and eight bytes a vertex on all of them is a page budget the
  // streaming layout would pay for nothing.
  Vector<SkinBinding> skin;
  u32 skin_joint_count = 0;  // palette width `skin` indexes; 0 when unskinned
  // ---- morph channels, all empty for a mesh with none ----
  Vector<MorphChannel> morph_channels;  // GPU-mirrored, 16 bytes each
  Vector<std::string> morph_names;      // parallel to `morph_channels`; host side only
  // CSR over `morph_slices`: `clusters.size() + 1` entries when the mesh has channels, empty
  // otherwise. Cluster c's slices are [morph_cluster_slices[c], morph_cluster_slices[c + 1]).
  Vector<u32> morph_cluster_slices;
  Vector<MorphSlice> morph_slices;
  // One cluster-**local** vertex index per delta (a cluster holds at most 255 vertices), padded
  // with zeros to a multiple of four so a shader may read the last one out of a whole 32-bit word.
  Vector<u8> morph_indices;
  // Three i16 per delta, padded to an even count for the same reason `quantized` is. Scaled by
  // the owning channel's `position_scale` / `normal_scale`. `morph_normal_deltas` is either empty
  // — no channel of this mesh moves normals — or exactly parallel to `morph_deltas`, with the
  // channels that move no normals writing zeros: one array either way, because a per-slice second
  // offset would buy nothing on the content this exists for, where a source writes POSITION and
  // NORMAL for every target or for none.
  Vector<i16> morph_deltas;
  Vector<i16> morph_normal_deltas;
  // How many deltas the three arrays above really hold: they are padded, so their lengths cannot
  // say. It is also the sum of every slice's `delta_count`.
  u32 morph_delta_count = 0;
  // The canonical id of every cluster vertex, parallel to `vertices`, or empty when the source had
  // none. It reaches a cluster through `vertex_source` like every other per-vertex stream, so every
  // copy of a source vertex — in every cluster, on every LOD level — carries the same id. Host
  // side only: nothing uploads it (see "canonical vertex identity" above).
  Vector<u32> vertex_ids;
  VertexIdSource vertex_id_source = VertexIdSource::none;
  Vector<u32> triangles;  // per triangle: local i0 | i1 << 8 | i2 << 16
  // Positions on the mesh-wide 16-bit grid: three u16 per vertex, cluster-ordered like
  // `vertices`, padded with one zero to an even count so a shader may read the last triple as
  // two 32-bit words. `quantize_positions` fills all three fields; a mesh with no vertices keeps
  // the identity grid.
  Vector<u16> quantized;
  Vec3 quant_origin{};     // the AABB minimum of `vertices`
  f32 quant_scale = 1.0f;  // the largest AABB extent / 65535; 1 for a degenerate mesh
  u32 source_vertex_count = 0;
  u32 source_triangle_count = 0;

  static constexpr u32 unpack(u32 packed, u32 corner) noexcept {
    return (packed >> (8 * corner)) & 0xff;
  }
  static constexpr u32 pack(u32 i0, u32 i1, u32 i2) noexcept { return i0 | (i1 << 8) | (i2 << 16); }
};

// Splits an indexed triangle mesh into clusters. `indices` holds three entries per triangle.
// Fails (with `error`) on empty input, a non-multiple-of-three index count, an index out of
// range, or options outside the limits above.
bool build_clusters(std::span<const Vec3> positions, std::span<const u32> indices,
                    const ClusterBuildOptions& options, ClusterMesh& out,
                    std::string* error = nullptr, const AttributeSource& attributes = {});

// Fills `mesh.attributes` and, when the source carries them, `mesh.skin`, the morph stream and
// `mesh.vertex_ids` from the source attributes through `vertex_source`; the builders call it, and
// it is public so a mesh built elsewhere can be given attributes later.
void fill_cluster_attributes(ClusterMesh& mesh, std::span<const Vec3> positions,
                             std::span<const u32> indices, const AttributeSource& attributes);

// The morph half of `fill_cluster_attributes`, separately because the LOD builder fills the
// attributes of every level and has to do this per level too. Replaces every `mesh.morph_*`
// field. A channel's quantization scale is a function of **the whole source channel** and not of
// the vertices this mesh happens to hold, which is what lets `merge_cluster_lod` concatenate the
// parts of one mesh without requantizing anything: every part of a mesh, and every LOD level of
// every part, stores channel k on the same grid.
void fill_cluster_morph(ClusterMesh& mesh, std::span<const MorphChannelSource> channels);

// Pads `morph_indices` to a multiple of four and both delta arrays to an even `i16` count, so a
// shader may read the last of each out of whole 32-bit words. Every builder, merge and page
// layout that fills the streams itself calls it last, the way they all call `quantize_positions`.
void pad_morph_streams(ClusterMesh& mesh);

// Appends cluster `cluster` of `from`'s morph slices and their deltas to `to`, adding
// `channel_base` to each slice's channel index and renumbering `first_delta` onto `to`'s streams.
// `with_normals` says whether `to`'s normal-delta array exists, which it must if *any* mesh going
// into it has one; a cluster from a mesh without normal deltas then contributes zeros.
//
// It is public because all three places that put clusters in a different order — both merges and
// `build_cluster_pages` — need exactly this, called in the output's cluster order so that the
// delta runs come out in slice order. The caller pushes `to.morph_cluster_slices` itself, since
// only it knows when a cluster is finished.
void append_cluster_morph(ClusterMesh& to, const ClusterMesh& from, u32 cluster, u32 channel_base,
                          bool with_normals);

// Deltas of channel `channel` at cluster-local vertex `local` of cluster `cluster`, decoded. The
// CPU reference of what the deform chain's morph stages read, and false when that channel does
// not touch that vertex (in which case both deltas are left zero).
bool morph_delta_at(const ClusterMesh& mesh, u32 cluster, u32 channel, u32 local,
                    Vec3& position_delta, Vec3& normal_delta) noexcept;

// The morph half of `validate_clusters`, separately because a **LOD DAG** is not a cluster mesh
// that covers its source once — `validate_cluster_lod` is its validator, and it needs this check
// as much as `validate_clusters` does. Returns true for a mesh with no channels at all.
bool validate_morph_stream(const ClusterMesh& mesh, std::string* error = nullptr);

// The conservative bound a renderer pads an instance's culling spheres by: the sum over channels
// of |weight| x `max_displacement`. It is the triangle inequality and nothing cleverer — the
// channels of a face move the same region in the same direction as often as not, so a bound that
// assumed cancellation would be wrong on exactly the content this exists for. `weights` shorter
// than the channel array leaves the rest at zero.
f32 morph_bounds_padding(const ClusterMesh& mesh, std::span<const f32> weights) noexcept;

// Fills `quantized`, `quant_origin`, and `quant_scale` from `vertices`: one grid over the whole
// mesh, origin at the AABB minimum, step = the largest AABB extent / 65535 (1 when the mesh is a
// point), and q = round((p - origin) / step) clamped to 0..65535 per axis. Every builder calls
// it last, so a merged mesh gets one grid over all of its parts. Because every cluster's copy of
// a shared vertex quantizes the same source position on the same grid, the copies land on the
// same integer triple and welded meshes stay crack-free: quantization moves a seam's two copies
// by exactly the same amount. Public so a mesh built elsewhere can be quantized later; call it
// again after changing `vertices`.
//
// It also **refits every cluster's normal cone** to the new grid, because the rasterizers draw the
// grid and not the floats: over every triangle at least one grid step thick, both as floats and on
// the grid, in double precision, the axis becomes whichever of the one already packed in
// `ClusterDesc::cone` (meshoptimizer's) and the mean normal needs the narrower cone, and the
// cutoff and apex are recomputed around it with `k_cone_margin` of slack. A cluster whose cone
// would then be wider than meshoptimizer's own limit (cos 0.1) gets k_cone_none; k_cone_none
// stays k_cone_none.
void quantize_positions(ClusterMesh& mesh);
// The CPU reference of the shaders' load_position: the grid point of `vertex` as a float
// position. Out-of-range vertices read as the origin.
Vec3 dequantize_position(const ClusterMesh& mesh, u32 vertex) noexcept;

// Checks the invariants tests rely on: offsets and counts in range, counts within the limits,
// every source triangle present exactly once, every vertex inside its cluster's sphere, every
// triangle normal — as floats and on the grid — inside its cluster's normal cone, with the apex
// behind the triangle's plane, both with at least half of k_cone_margin to spare (when the cone is
// not k_cone_none; a triangle thinner than one grid step has no normal and is skipped), a
// quantized position stream that is present, padded to an even count, and within half a grid
// step of every float position, — when the mesh is skinned — a binding stream parallel to
// the vertices whose weights sum to 255 and whose joints are inside `skin_joint_count`, and —
// when the mesh has morph channels — a per-cluster directory that covers every cluster exactly
// once in ascending order, slices naming channels that exist in ascending order with no repeats,
// delta runs that are contiguous and in slice order, local vertex indices inside their cluster,
// and channel scales that are finite and not negative (a delta is an i16 and cannot itself be
// infinite, so the scale is where a non-finite displacement would have to come from), and — when
// the mesh carries ids — an id stream parallel to the vertices, a source that says where it came
// from, and **one id per source vertex**: every copy of a `vertex_source` entry carries the same.
bool validate_clusters(const ClusterMesh& mesh, std::span<const u32> source_indices,
                       const ClusterBuildOptions& options, std::string* error = nullptr);

}  // namespace engine::geometry
