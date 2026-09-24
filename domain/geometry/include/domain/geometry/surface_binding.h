#pragma once

// The surface binding: a render vertex bound to a point of a refined surface, and the displacement
// transfer that moves it (docs/subsystems/geometry.md, "Limit surfaces and surface bindings";
// docs/plan/07-content-pipeline.md §7.11, the binding paragraph; docs/plan/05-simulation.md §5.16,
// the 2026-09-23 addendum's item 3).
//
// Why it is shaped this way. A tissue region's render surface is bound to its cage through the
// cage's limit surface (limit_surface.h), evaluated at a dense refinement, so the per-vertex record
// can stay small: a triangle of the *refined* surface, a point in it, and an offset along the
// footpoint normal. The record stores **no** absolute position of the region's surface. It moves
// the observed base by a weight times the surface's *change* from a paired reference observation:
//
//     out = base + w * ( D(f) + h * (N(state)(f) - N(ref)(f)) ),   D = L (x_state - x_ref)
//
// where `f` is the footpoint (triangle and barycentrics), `L` the limit operator, `x` the cage's
// nodes, `D` the refined surface's displacement, `N` the **footpoint normal** and `h` the signed
// normal offset. Three properties follow, and each is a validator row the plan names:
//
// - **The base is bitwise where the weight is zero.** A vertex with weight 0 is copied, never
//   recomputed, so the torso outside a region, and a seam two bilateral modules share and neither
//   owns, stays exactly the base whatever the cage does and whichever module runs first.
// - **At the reference state the output is the base, bitwise**, whatever the binding's error: the
//   transfer moves a vertex by a difference, so a footpoint a little off, or an offset that does
//   not reconstruct the vertex, costs nothing at rest and only its product with the deformation
//   elsewhere.
// - **Positions are affine in the nodes; the offset term is not.** With `h = 0` the transfer is
//   linear in the node state. The offset transports as `h` times the *change* of the normal, never
//   as an absolute offset along the current normal: it vanishes at the reference and moves rigidly
//   with a rigid motion, and its nonlinearity is second order in the rotation.
//
// **The footpoint normal has three definitions, and a record set names the one it uses**
// (`NormalMode`; the round-five closure of study019). They differ by up to 5.65 degrees on that
// study, which is 0.16 mm in its supine picture, so "the same surface to the float" needs the mode
// written down beside the records rather than assumed:
//
//   triangle                           the footpoint triangle's own unit normal: piecewise
//                                      constant, so two vertices either side of a refined edge
//                                      part by h times the change of that edge's dihedral
//   interpolated-vertex-area-weighted  normalize(sum of b_k n_k), n_k the corner's unit vertex
//                                      normal, itself the normalized sum of its incident refined
//                                      triangles' cross products (area weighted): study019's
//   limit-interpolated                 normalize(sum of b_k n_k), n_k the corner's unit **limit**
//                                      normal: the plan's rule (05 §5.16, item 3), exactly as
//                                      agreed with the authoring side on 2026-09-24 (below)
//
// **The limit-interpolated rule, step by step.** Evaluate the limit tangent operators Tu and Tv at
// the dense vertices; normalize each tangent; the vertex is **degenerate** when either tangent is
// zero or not finite, or when the cross product of the two unit tangents has a norm of at most
// `k_normal_degenerate_sine` (a dimensionless sine of the angle between them); a valid vertex's
// normal is that cross product normalized. Interpolate the corners' unit normals barycentrically
// at the footpoint and renormalize; the footpoint normal is degenerate when the interpolated length
// is at most `k_normal_interpolated_length`. Orientation comes from the winding alone — the side
// the triangles' counter-clockwise order faces — and is never flipped toward a camera, and every
// threshold is dimensionless, so a uniformly scaled cage binds and moves the same way.
//
// **Degeneracy is handled two ways, and they are not the same thing.** At the **reference**, a
// degenerate normal is *invalid*: it is not invented, it is reported (`BindingFrame::
// invalid_normals`, `SurfaceBindReport::invalid_normals`), and a tissue validator turns that into
// an error naming the vertex; its entry is the zero vector. At a **state**, a degenerate normal
// *falls back to its reference*: a vertex whose tangents collapse keeps its reference vertex
// normal, and a footpoint whose interpolation cancels keeps its reference footpoint normal, so the
// offset is carried rigidly rather than dropped. The same two rules, with a relative threshold
// (`k_normal_degenerate_ratio` of the reference length, since those vectors have units), cover the
// other modes: the facet in `triangle` mode — a footpoint triangle collapsed to a point keeps its
// reference normal — and each area-weighted vertex sum. A collapsed footpoint triangle changes
// nothing by itself in the interpolated modes, because its corners' vertex normals still have their
// other incident triangles, or their tangents; only a degenerate *vertex* or *interpolation* falls
// back. Nothing here substitutes one mode for another: every operation takes the mode, and a
// record set bound under one mode and transferred under another is a caller's explicit choice (the
// conformance comparison makes it on purpose).
//
// **Why the displacement form, and not S_state - S_ref.** "Bitwise at the reference" is only true
// if every change the transfer computes is *exactly* zero when the nodes have not moved, and a
// difference of two evaluations is not: once a compiler may fuse a multiply into the subtraction
// that follows it (FMA contraction — GCC's default for C++ at x86-64-v3 until ADR-0035 turned it
// off tree-wide, and always a possibility on a GPU), `a*b - a*b` becomes `fma(a, b, -round(a*b))`,
// the product's rounding error. That happened here: the first transfer subtracted two footpoint
// normals, GCC 13 fused one normal's final multiply into the subtraction, and one vertex in 300
// moved at rest (linux-gcc-release, 2026-09-24). So nothing is written as a difference of
// evaluations. The nodes are subtracted first, `surface_displacement` pushes that difference
// through the operator, and every normal's change — a triangle's, a vertex's, a footpoint's — is
// `unit_normal_change`'s, which is exactly zero by a select when the unnormalized vector's change
// is zero and otherwise expands it so that every term is a product with that change. At the
// reference every such product has an exact zero factor, and however a compiler fuses it, the
// result is zero. Do not "simplify" it back into two normals and a subtraction. **But never let
// the expansion measure the state**: its |c + dc|^2 = |c|^2 + dq keeps about 1e-7 of |c|^2 as
// rounding, so a state vector under about 3e-4 of its reference came out of it as length zero and
// was given no change — the reference normal, silently, with no fallback (the D8 comparison,
// 2026-09-24). The state's length is the directly evaluated vector's, and where the state is under
// a quarter of the reference's length its direction is too (docs/subsystems/geometry.md, "The
// expansion measured a short state as no state").
//
// The record is **10 bytes** (the plan's proposed packing, pinned by the size table): a `u16`
// refined triangle, two `u16` barycentric weights in 1/65535 (the third is what they leave), an
// IEEE half-float normal offset in the positions' units, and a `u16` blend weight in 1/65535. A
// `u16` triangle caps a region's refined surface at 65,536 triangles — 1,024 control triangles at
// level 3, 4,096 at level 2 — and `surface_binding_can_represent` is the check: the binders refuse
// a larger surface rather than wrapping, since the plan's no-hidden-limits rule has no wider record
// to fall back to yet. What the packing costs is measured, not assumed:
// `SurfaceBindReport::max_quantization_error` is the distance between the exact footpoint plus
// offset and what the record decodes to. The mode is not in the record: it is one per record set,
// and the set's owner (a tissue definition's binding, docs/subsystems/tissue.md) carries it.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/geometry/limit_surface.h>

#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace engine::geometry {

struct SurfaceBinding {
  u16 triangle = 0;             // a triangle of the refined surface
  u16 barycentric[2] = {0, 0};  // corners 0 and 1, in 1/65535; corner 2 takes the rest
  u16 normal_offset = 0;        // IEEE half float, signed along the reference footpoint normal
  u16 weight = 0;               // blend weight in 1/65535; 0 leaves the base untouched
};
static_assert(sizeof(SurfaceBinding) == 10, "SurfaceBinding is the plan's 10-byte record");

inline constexpr u32 k_surface_binding_max_triangles = 65536;

// Whether a refined surface of this many triangles can be addressed by the record's `u16`.
constexpr bool surface_binding_can_represent(u32 surface_triangles) noexcept {
  return surface_triangles <= k_surface_binding_max_triangles;
}

// The record's decoders: exact at the ends (a weight of 65535 is 1.0f, of 0 is 0.0f), and what the
// transfer itself uses.
f32 binding_weight(const SurfaceBinding& binding) noexcept;
Vec3 binding_barycentrics(const SurfaceBinding& binding) noexcept;  // corners 0, 1, 2
f32 binding_normal_offset(const SurfaceBinding& binding) noexcept;

// ---- the footpoint normal
// ------------------------------------------------------------------------

// How a record set's footpoint normal is produced (the header comment's table). The values are
// stable: a tissue container stores the mode by name, never by number, but a caller may keep one.
enum class NormalMode : u8 {
  triangle = 0,
  interpolated_vertex_area_weighted = 1,
  limit_interpolated = 2,
};
inline constexpr u32 k_normal_mode_count = 3;

// The interchange spelling — "triangle", "interpolated-vertex-area-weighted",
// "limit-interpolated" — which the authoring side writes and a tissue definition stores.
const char* normal_mode_name(NormalMode mode) noexcept;
// Parses that spelling; false for anything else, including another mode's C++ name.
bool parse_normal_mode(std::string_view name, NormalMode& out) noexcept;

// The agreed thresholds (the header comment's "The limit-interpolated rule"). All dimensionless.
// A vertex's limit normal is degenerate when the cross product of its two unit tangents has a norm
// at or below this: the sine of the angle between them.
inline constexpr f32 k_normal_degenerate_sine = 1.0e-6f;
// A footpoint's interpolation of unit vertex normals is degenerate at or below this length.
inline constexpr f32 k_normal_interpolated_length = 1.0e-6f;
// A vector with units in the historical modes — a facet's cross product, an area-weighted sum — is
// degenerate at a state when it is at most this fraction of its reference length (the agreed
// legacy extension); a reference vector is invalid only at exactly zero or not finite. A limit
// tangent has no such cutoff: the agreed rule (D10) calls it degenerate only when it is zero or
// not finite, at the reference and at a state alike, and a tangent that shrinks a millionfold but
// keeps a direction still has a normal. Every length behind these tests is taken scale-robustly (a
// squared length that would leave f32's range is taken after an exact power-of-two rescaling), so
// the decisions are the same under any uniform rescaling of the geometry.
inline constexpr f32 k_normal_degenerate_ratio = 1.0e-6f;

// c / |c|, or the zero vector when c is zero or not finite (an invalid reference normal is not
// invented). Scale-robust: the same direction at any scale, and at every scale an f32 can hold.
Vec3 unit_normal(Vec3 c) noexcept;

// n(c + dc) - n(c), n(v) = v / |v|. **Exactly zero when dc is zero**, by a select, on any compiler
// or GPU. Zero, too — the reference normal is kept — unless c is nonzero and |c + dc|^2, evaluated
// directly, exceeds `min_state_squared`, the caller's degeneracy threshold:
// `k_normal_degenerate_ratio^2 |c|^2` for a vector with units, the square of an absolute threshold
// for one built from unit vectors. Otherwise the change to c + dc's own direction, written so that
// **every term is a product with dc** while the state keeps at least a quarter of c's length:
//
//   dq   = dc . (2c + dc)                              (|c + dc|^2 - |c|^2)
//   dinv = -dq (1/|c + dc|) / (|c| (|c| + |c + dc|))   (1/|c + dc| - 1/|c|)
//   n(c + dc) - n(c) = dc / |c + dc| + c dinv
//
// exact in the reals, with rounding that grows as |c| / |c + dc|; and below a quarter, the direct
// n(c + dc) - n(c). **Both the test and the length are the state vector's own, never the
// expansion's |c|^2 + dq**, whose rounding (about 1e-7 of |c|^2) would call an exactly collapsed
// vector live, and — the defect the D8 comparison found on 2026-09-24 — measured a live state under
// about 3e-4 of c's length as length zero and returned no change for it: the reference normal,
// silently. The transfer's own callers hand the same function the state they evaluated from the
// state's corners, tangents or sums; this signature has only c + dc, summed in f32.
Vec3 unit_normal_change(Vec3 c, Vec3 dc, f32 min_state_squared) noexcept;

// ---- the per-vertex normal field of the two interpolated modes --------------------------------
//
// Each interpolated mode reads, per refined vertex, a unit normal at the reference and its change
// at the state. The change is a product with the displacement, so an unmoved cage gives exactly
// zero. The reference functions return how many vertices are invalid and list them when asked;
// an invalid vertex's normal is the zero vector and its change is zero.

// interpolated-vertex-area-weighted: a_k is the sum over the refined triangles t around vertex k of
// (p1 - p0) x (p2 - p0), summed in triangle order; its change is the sum of each triangle's
// e1 x g2 + g1 x e2 + g1 x g2, e the reference edges and g their displacement. The unit normal is
// a_k / |a_k|, invalid at a zero or non-finite sum. A state is judged on its own sum,
// `state_vectors` (`area_weighted_normal_vectors` over the state's positions, reference plus
// displacement): one at most `k_normal_degenerate_ratio` of the reference's keeps the reference
// normal.
void area_weighted_normal_vectors(std::span<const u32> faces, std::span<const Vec3> positions,
                                  std::span<Vec3> out) noexcept;
void area_weighted_normal_vector_change(std::span<const u32> faces, std::span<const Vec3> reference,
                                        std::span<const Vec3> displacement,
                                        std::span<Vec3> out) noexcept;
u32 area_weighted_reference_normals(std::span<const Vec3> vectors, std::span<Vec3> out,
                                    Vector<u32>* invalid = nullptr) noexcept;
// `fallback`, when given, receives the vertices whose state was degenerate and kept its reference
// normal (not the invalid ones, which never had one).
void area_weighted_normal_changes(std::span<const Vec3> vectors,
                                  std::span<const Vec3> vector_change,
                                  std::span<const Vec3> state_vectors, std::span<Vec3> out,
                                  Vector<u32>* fallback = nullptr) noexcept;

// limit-interpolated, the agreed rule: `tangent_u`/`tangent_v` are the tangent operators applied to
// the reference nodes, `*_change` to the nodes' displacement. The unit tangents' changes, their
// cross product's change and the normal's change are each `unit_normal_change`'s, so the whole
// chain is a product with the displacement wherever the state is not short; the degeneracy tests
// are selects on the state's own tangents (zero or not finite, D10) and their unit cross product (a
// sine of at most `k_normal_degenerate_sine`), which at the reference are the reference's bit for
// bit.
u32 limit_reference_normals(std::span<const Vec3> tangent_u, std::span<const Vec3> tangent_v,
                            std::span<Vec3> out, Vector<u32>* invalid = nullptr) noexcept;
void limit_normal_changes(std::span<const Vec3> tangent_u, std::span<const Vec3> tangent_v,
                          std::span<const Vec3> tangent_u_change,
                          std::span<const Vec3> tangent_v_change, std::span<Vec3> out,
                          Vector<u32>* fallback = nullptr) noexcept;

// What the transfer and the binders read about the refined surface at one state. The two normal
// spans are the interpolated modes' (one entry per refined vertex) and are not read by `triangle`;
// the binders read only `faces`, `reference` and `normal_reference`.
struct BindingSurface {
  std::span<const u32> faces;              // the refined triangles, three indices each
  std::span<const Vec3> reference;         // S_ref: the refined vertices at the paired reference
  std::span<const Vec3> displacement;      // D = L (x_state - x_ref), `surface_displacement`
  std::span<const Vec3> normal_reference;  // unit vertex normals at the reference
  std::span<const Vec3> normal_change;     // their change at this state
};

// Everything one state needs besides the records, evaluated and owned: for tools, tests and the
// tissue validators, which run once per state rather than per frame (a per-frame pass keeps its
// own buffers and calls the functions above). `limit_interpolated` needs the surface's tangent
// operators (`LoopSurfaceOptions::tangents`); the call fails without them rather than falling back.
// Invalid reference vertex normals are listed, not refused: whether they matter depends on whether
// a footpoint uses them, which is the binders' and the validators' question.
struct BindingFrame {
  Vector<Vec3> reference;
  Vector<Vec3> displacement;
  Vector<Vec3> normal_reference;
  Vector<Vec3> normal_change;
  Vector<u32> invalid_normals;  // refined vertices whose reference normal is invalid under the mode
  Vector<u32>
      fallback_normals;  // refined vertices whose state normal was degenerate: reference kept

  BindingSurface view(std::span<const u32> faces) const noexcept {
    return BindingSurface{faces, reference, displacement, normal_reference, normal_change};
  }
};
bool evaluate_binding_frame(NormalMode mode, const LoopLimitSurface& surface,
                            std::span<const Vec3> reference_nodes,
                            std::span<const Vec3> state_nodes, BindingFrame& out,
                            std::string* error = nullptr);

// The unit footpoint normal at the reference, as `mode` defines it, or the zero vector when it is
// invalid: a degenerate facet in `triangle` mode; in the interpolated modes a corner of nonzero
// barycentric weight whose vertex normal is invalid, or an interpolation of length at most
// `k_normal_interpolated_length`. What the binders measure offsets along.
Vec3 footpoint_normal(NormalMode mode, const BindingSurface& surface, u32 triangle,
                      Vec3 barycentrics) noexcept;

// The footpoint normal's change at the state `surface` describes, N(state) - N(ref), exactly as the
// transfer computes it for a record with these barycentrics (pass `binding_barycentrics` to match a
// record bit for bit): zero at the reference, and zero with `*fell_back` set when the state's
// footpoint normal is degenerate and the reference one is kept. What the volume report's projection
// diagnostic and `engine-content limit-dump` read the state normal from.
Vec3 footpoint_normal_change(NormalMode mode, const BindingSurface& surface, u32 triangle,
                             Vec3 barycentrics, bool* fell_back = nullptr) noexcept;

// ---- binding ------------------------------------------------------------------------------------

struct SurfaceBindOptions {
  // The blend weight of each render vertex, in [0, 1]; empty binds every vertex at weight 1. A
  // vertex of weight 0 is not searched for (it is never moved) and gets a record naming corner 2
  // of triangle 0. `bind_from_records` takes its weights from the footpoints instead.
  std::span<const f32> weights;
  // A vertex whose |normal offset| exceeds this is reported (it is still bound).
  f32 offset_limit = std::numeric_limits<f32>::infinity();
};

struct SurfaceBindReport {
  // Render vertices (of nonzero weight) whose footpoint is on the refined surface's boundary: on a
  // boundary edge or at a boundary vertex. Such a vertex lies beyond the surface, and its offset
  // is not along the line to its footpoint.
  Vector<u32> boundary_footpoints;
  // Render vertices whose |normal offset| exceeds `SurfaceBindOptions::offset_limit`.
  Vector<u32> offset_exceeded;
  // Render vertices (of nonzero weight) whose footpoint normal is invalid at the reference
  // (`footpoint_normal` is zero): the offset is recorded, but a record set with any of these is a
  // validator error, since the normal it would transport along was never defined.
  Vector<u32> invalid_normals;
  f32 max_abs_offset = 0.0f;
  // The largest distance between a vertex's exact footpoint plus offset and what its 10-byte record
  // decodes to: the barycentrics' 1/65535 and the half float's 2^-11 relative step, together.
  f32 max_quantization_error = 0.0f;
  // The largest distance from a vertex to its footpoint plus offset along the mode's normal: the
  // part of the vertex's position the record does not carry (the tangential residual), which is
  // zero for a nearest point on a triangle's interior in `triangle` mode and not otherwise.
  f32 max_tangential_residual = 0.0f;
  // `bind_from_records` only: the largest |authored offset - the offset measured along the mode's
  // normal|, over footpoints that carried an authored offset. How far the authored records are
  // from reconstructing their vertices under the mode they are transferred with.
  f32 max_offset_disagreement = 0.0f;
};

// Binds every render vertex to its nearest point of the refined surface at the reference: the
// triangle, the barycentrics of the nearest point, and the signed offset of the vertex along the
// footpoint normal `mode` defines (`surface.reference`, `surface.faces` and, for the interpolated
// modes, `surface.normal_reference`). Ties between equally near triangles go to the lower triangle
// index, so the result is a function of the input alone. Refuses a surface of more than
// `k_surface_binding_max_triangles`, an index out of range, a weight outside [0, 1] and an offset a
// half float cannot hold. **Not** what an authored binding uses: a nearest point has no chart
// regularization, and on study019 it reversed 8 image triangles the authoring side's footpoints did
// not — `bind_from_records` consumes those instead.
bool bind_to_surface(NormalMode mode, std::span<const Vec3> render_vertices,
                     const BindingSurface& surface, const SurfaceBindOptions& options,
                     Vector<SurfaceBinding>& out, SurfaceBindReport* report = nullptr,
                     std::string* error = nullptr);

// A footpoint as the authoring side wrote it, per render vertex: a refined triangle, barycentrics
// in its corner order, the blend weight, and optionally the normal offset it measured.
struct AuthoredFootpoint {
  u32 triangle = 0;
  Vec3 barycentric{1.0f, 0.0f, 0.0f};
  // The authored offset, kept as the record's; NaN measures it along the mode's footpoint normal.
  f32 offset = std::numeric_limits<f32>::quiet_NaN();
  f32 weight = 0.0f;
};

// Packs authored footpoints into records, one per render vertex, rather than searching for nearest
// points: the footpoint is the authoring side's (chart-regularized, on study019), and the engine's
// job is to carry it, not to recompute it. The barycentrics are renormalized to sum to one (they
// must be finite, none below -1e-6 and their sum within 1e-5 of one); an authored offset is kept
// and compared with the one measured along `mode`'s normal (`max_offset_disagreement`), a missing
// one is measured. Every footpoint is packed, a zero-weight one too (it stays inert and keeps its
// authored location), and the reports cover the ones of nonzero weight. Refuses what
// `bind_to_surface` refuses, plus a triangle out of range and barycentrics that are not a point of
// the triangle, naming the render vertex.
bool bind_from_records(NormalMode mode, std::span<const Vec3> render_vertices,
                       std::span<const AuthoredFootpoint> footpoints, const BindingSurface& surface,
                       const SurfaceBindOptions& options, Vector<SurfaceBinding>& out,
                       SurfaceBindReport* report = nullptr, std::string* error = nullptr);

// Load-time check of records that came from outside: every triangle in range, the two stored
// barycentrics summing to at most 65535, and a finite offset.
bool validate_surface_bindings(std::span<const SurfaceBinding> bindings, u32 surface_triangles,
                               std::string* error = nullptr);

// **Image orientation** (the plan's validator row "no reversed triangle in the bound render
// surface"): a render triangle whose three corners are bound maps to the triangle of their
// footpoints on the refined surface at the reference, and that image is reversed when its normal
// faces away from the render triangle's (their dot product is not positive). A nearest-point
// binding on a fold folds the image before any deformation, which is what reversed study019's first
// eight; a reversed image turns inside out the first time the cage moves. `bindings` is one record
// per render vertex (parallel to `render_positions`); `fully_weighted` checks only triangles whose
// corners all have weight one, otherwise every triangle whose corners all have a nonzero weight.
// The deviation is the angle between each checked render triangle's normal and its image's.
struct BindingImageReport {
  u32 triangles = 0;     // render triangles checked
  Vector<u32> reversed;  // those whose image is reversed or degenerate, as render triangle indices
  f32 deviation_p50_deg = 0.0f;
  f32 deviation_p95_deg = 0.0f;
  f32 deviation_max_deg = 0.0f;
  u32 deviation_max_triangle = 0;  // the render triangle of the largest deviation
};
void check_binding_images(std::span<const SurfaceBinding> bindings,
                          std::span<const Vec3> render_positions,
                          std::span<const u32> render_triangles,
                          std::span<const Vec3> surface_positions,
                          std::span<const u32> surface_faces, bool fully_weighted,
                          BindingImageReport& out);

// The refined surface's displacement from the paired reference observation, `L (x_state - x_ref)`:
// the nodes are subtracted into `node_displacement` (scratch, one per node) and the operator is
// applied to that, never to the two states separately, so it is exactly zero wherever the nodes
// have not moved (the header comment says why that matters). `out` has one entry per refined
// vertex (`limit.rows()`). Once per region per frame, the same cost as evaluating the state. The
// tangent operators take the same `node_displacement` for the limit mode's normal change.
void surface_displacement(const CsrMatrix& limit, std::span<const Vec3> reference_nodes,
                          std::span<const Vec3> state_nodes, std::span<Vec3> node_displacement,
                          std::span<Vec3> out) noexcept;

// Which parts of the transfer to apply: all of it, the surface displacement alone (every offset
// taken as zero), or the offset term alone (the displacement taken as zero, the normal change
// kept). The last two are the volume report's decomposition (docs/subsystems/tissue.md: operator
// only, offset only, full, and their interaction), which is why they are the transfer's own code
// and not a caller's imitation of it.
enum class BindingTerms : u8 { full = 0, displacement = 1, offset = 2 };

// The per-frame displacement transfer (the header comment's formula), `bindings[i]` moving
// `base_positions[i]` into `out[i]`, the footpoint normal as `mode` defines it. At the reference,
// where the displacement and the normal change are zero, every output is its base bit for bit,
// whichever compiler built this and wherever `surface.reference` was evaluated. `out` may be
// `base_positions` itself (the same span), never a partial overlap. The bindings must be valid for
// the surface (`validate_surface_bindings`); this does not check them again. The mode and the
// terms are selected once, outside the vertex loop.
void apply_binding(NormalMode mode, std::span<const SurfaceBinding> bindings,
                   std::span<const Vec3> base_positions, const BindingSurface& surface,
                   std::span<Vec3> out, BindingTerms terms = BindingTerms::full) noexcept;

}  // namespace engine::geometry
