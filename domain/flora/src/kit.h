#pragma once

// The species solvers' shared kit (docs/subsystems/flora.md, "Determinism"): f64 vectors rounded to
// the mesh's f32 once, the splitmix64 streams, the emitter that writes a vertex with its motion
// attributes, and the two shapes the branching species are made of — a tube along a grown axis
// (trunk and branches) and a spray of cards on a twig (leaves and needles). Internal to the module.
//
// Everything here is IEEE `+ - * /` and `sqrt` in f64 with the deterministic sines, so a mesh built
// from it is a function of its parameters' bits on every toolchain.

#include <core/math/deterministic.h>
#include <domain/assets/gltf.h>
#include <domain/flora/impostor.h>
#include <domain/flora/motion.h>
#include <domain/flora/trees.h>

#include <cmath>

namespace engine::flora::detail {

inline constexpr f64 k_pi = 3.14159265358979323846;
inline constexpr f64 k_degree = k_pi / 180.0;
inline constexpr f64 k_golden_angle = 2.39996322972865332;  // pi (3 - sqrt 5): 137.5 degrees

struct D3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};
inline D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline D3 operator*(D3 a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
inline f64 dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline D3 cross(D3 a, D3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline f64 length_of(D3 a) { return std::sqrt(dot(a, a)); }
inline D3 normalized(D3 a) {
  const f64 length = std::sqrt(dot(a, a));
  return length > 0.0 ? a * (1.0 / length) : D3{0.0, 1.0, 0.0};
}
inline Vec3 to_f32(D3 a) {
  return Vec3{static_cast<f32>(a.x), static_cast<f32>(a.y), static_cast<f32>(a.z)};
}
inline D3 from_f32(Vec3 a) {
  return D3{static_cast<f64>(a.x), static_cast<f64>(a.y), static_cast<f64>(a.z)};
}
inline f64 lerp(f64 a, f64 b, f64 t) { return a + (b - a) * t; }
inline f64 clamp01(f64 t) { return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t); }
inline f64 smoothstep01(f64 t) {
  const f64 c = clamp01(t);
  return c * c * (3.0 - 2.0 * c);
}
inline constexpr D3 k_up{0.0, 1.0, 0.0};

// A unit vector perpendicular to `d`: the one nearest to `hint` (which must not be parallel).
inline D3 perpendicular(D3 d, D3 hint) { return normalized(hint - d * dot(hint, d)); }
// `v` turned by `angle` about the unit axis `axis` (Rodrigues).
inline D3 rotate(D3 v, D3 axis, f64 angle) {
  const f64 c = det::cos(angle);
  const f64 s = det::sin(angle);
  return v * c + cross(axis, v) * s + axis * (dot(axis, v) * (1.0 - c));
}

// splitmix64 (Steele, Lea and Flood 2014): every draw is integer arithmetic on the seed, and a
// draw's float is its top 53 bits scaled by a power of two, so it is exact on every target.
struct Rng {
  u64 state;
  explicit Rng(u64 seed) : state(seed) {}
  u64 next() {
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  }
  f64 unit() { return static_cast<f64>(next() >> 11) * (1.0 / 9007199254740992.0); }  // [0, 1)
  f64 symmetric() { return unit() * 2.0 - 1.0; }                                      // [-1, 1)
};
// An independent stream: a part's draws never move another part's.
inline Rng stream_of(u64 seed, u64 stream) {
  Rng mix(seed ^ (stream * 0xD1B54A32D192ED03ull));
  return Rng(mix.next());
}

// Every vertex is emitted with the part it belongs to and its bend weight (motion.h): the part is
// set once per part, the weight per station along it.
struct Emitter {
  assets::MeshData& mesh;
  Motion& motion;
  u32 part = k_trunk_part;
  f64 weight = 0.0;
  u32 vertex(D3 position, D3 normal, f64 u, f64 v) {
    const u32 index = mesh.positions.size();
    mesh.positions.push_back(to_f32(position));
    mesh.normals.push_back(to_f32(normalized(normal)));
    mesh.uvs.push_back(Vec2{static_cast<f32>(u), static_cast<f32>(v)});
    motion.vertex_part.push_back(part);
    motion.vertex_weight.push_back(static_cast<f32>(clamp01(weight)));
    return index;
  }
  void triangle(u32 a, u32 b, u32 c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
  }
  // A triangle wound so that its face normal looks along `hint`.
  void face(u32 a, u32 b, u32 c, D3 hint) {
    const Vec3 nf =
        cross(mesh.positions[b] - mesh.positions[a], mesh.positions[c] - mesh.positions[a]);
    const f64 facing = static_cast<f64>(nf.x) * hint.x + static_cast<f64>(nf.y) * hint.y +
                       static_cast<f64>(nf.z) * hint.z;
    if (facing >= 0.0) {
      triangle(a, b, c);
    } else {
      triangle(a, c, b);
    }
  }
  // Opens a part: its record, and the part every vertex until the next one belongs to. Returns
  // its id.
  u32 begin_part(D3 pivot, D3 direction, f64 length, u32 parent) {
    part = motion.parts.size();
    geometry::ClusterFileMotionPart record;
    record.pivot = to_f32(pivot);
    record.direction = to_f32(normalized(direction));
    record.length = static_cast<f32>(length);
    record.parent = parent;
    motion.parts.push_back(record);
    weight = 0.0;
    return part;
  }
};

inline assets::Material material(const char* name, Vec4 colour, f32 roughness, bool two_sided) {
  assets::Material m;
  m.name = name;
  m.base_color = colour;
  m.metallic = 0.0f;
  m.roughness = roughness;
  m.double_sided = two_sided;
  return m;
}

// ---- an axis grown from a bud -------------------------------------------------------------------
//
// A branch's centre line: `rows + 1` points from its base, each step `length / rows` along a
// direction that turns towards `pull` (the light, or gravity) by `pull` radians a metre — through
// the plane of the two, so the turn is a rotation and never a renormalized blend — and wanders by
// up to `wiggle` radians a step from the seed. Parallel-transported frames go with it, so a tube
// swept along it never twists.

struct Axis {
  Vector<D3> point;
  Vector<D3> tangent;
  Vector<D3> normal;  // a frame across the tangent, transported along it
  Vector<f64> arc;
  f64 length = 0.0;
};

Axis grow_axis(D3 base, D3 direction, f64 length, u32 rows, D3 pull, f64 pull_per_m, f64 wiggle,
               Rng& rng);

// The point and tangent at `arc` along the axis, interpolated between its rows.
D3 axis_point(const Axis& axis, f64 arc);
D3 axis_tangent(const Axis& axis, f64 arc);

// The same axis at `rows` even steps, its frame transported anew: how a shoot grown at the
// structure's resolution is meshed at the budget's, so the budget never moves a shoot.
Axis resample_axis(const Axis& axis, u32 rows);

// ---- a tube -------------------------------------------------------------------------------------
//
// A tapered closed tube along an axis: `sides` round (one seam column), the radius linear from `r0`
// at the base to `r1` at the tip, closed by a cone to the tip point. The base is open: a branch's
// base is inside its parent. UVs in metres — u round the circumference, v along the axis, plus
// `uv_offset` — so a tiled bark set (a `tile_m` repeat) lays on it unstretched (flora.md,
// "Materials"). Every vertex's weight is its row's arc over the axis's length. The caller opens the
// part.

inline u32 tube_triangles(u32 rows, u32 sides) { return rows * sides * 2 + sides; }
void build_tube(const Axis& axis, f64 r0, f64 r1, u32 sides, Vec2 uv_offset, Emitter& out);

// ---- a spray of cards ---------------------------------------------------------------------------
//
// Leaves or needle bundles on a twig: the twig's centre line from `from` to the axis's tip divided
// into `cards` equal steps, and on each step two cards — one either side — whose base is that step
// of the centre line and which stand out from it at `angle` towards the tip, `lift` out of the
// spray's plane, `card_length` long. Consecutive cards share their centre-line vertices, so a
// spray is **one connected surface** the LOD builder can collapse along the twig (flora.md, "What
// the harness taught"), two-sided, and its UVs give every card the **full card** — card i spans
// u in [i, i + 1], one side v in [0, 1] and the other [0, -1] — so a one-leaf image (sampled with
// repeat) lies on each card whole. `side` is the spray's plane's lateral, perpendicular to the
// axis. Weights are each card's station on the axis.

inline u32 spray_triangles(u32 cards) { return cards * 4; }
void build_spray(const Axis& axis, f64 from, u32 cards, f64 card_length, f64 angle, f64 lift,
                 f64 droop, D3 side, Emitter& out);

// ---- the second batch of species: the birch, the willow, the orchard tree, the ancient tree ----
//
// Added after everything above and changing none of it, so the palm's, the broadleaf's and the
// conifer's bytes stay pinned (flora.md, "Determinism").

// `grow_axis` whose turn towards `pull` starts only at arc `pull_from`: a shoot that runs straight
// for a stretch and then bends (the megaflora's limbs, a willow's arch). The wander applies along
// the whole axis, and the draws are the same two a step as `grow_axis`'s. With `pull_from` 0 it is
// `grow_axis`.
Axis grow_axis_after(D3 base, D3 direction, f64 length, u32 rows, D3 pull, f64 pull_per_m,
                     f64 pull_from, f64 wiggle, Rng& rng);

// The first `rows + 1` points of an axis, its length theirs: a hanging curtain cut where it would
// reach the ground. The steps stay even, so `axis_point` reads it as it reads any axis.
Axis truncate_axis(const Axis& axis, u32 rows);

// A child's direction: `angle` from the parent's tangent at `arc`, towards `azimuth` round it, and
// the unit radial it leaves along (the broadleaf's rule, for the species after it).
D3 branch_direction(const Axis& parent, f64 arc, f64 azimuth, f64 angle, D3* radial_out);

// The pipe model's radius: the root's times (tips over total)^(1/2.5).
f64 pipe_radius_of(f64 root_radius, f64 tips, f64 total);

// Opens a part as `Emitter::begin_part` does and records its structural row beside it
// (structure.h): its radius at the base and at the tip, and its flags.
u32 begin_structural_part(Emitter& out, D3 pivot, D3 direction, f64 length, u32 parent, f64 r0,
                          f64 r1, u32 flags);

// A tube along a shoot's grown axis, resampled at `rows`, as its own structural part. Returns the
// part.
u32 emit_tube_part(Emitter& out, const Axis& axis, f64 r0, f64 r1, u32 rows, u32 sides, u32 parent,
                   u32 flags);

// A spray of cards along a shoot's grown axis as its own part, with no wood (radii zero), its plane
// laid level. Returns the part.
u32 emit_spray_part(Emitter& out, const Axis& axis, u32 parent, u32 cards, f64 card_length,
                    f64 angle, f64 lift, f64 droop);

// Replaces the normals of the vertices from `first_vertex` on with the area-weighted normals of
// the triangles from `first_index` on — how a spray's cards get a surface's normals.
void smooth_normals(assets::MeshData& mesh, u32 first_vertex, u32 first_index);

// ---- the allometric crown (G72; docs/experiments/tree-allometry.md, flora.md "Allometry") ------
//
// The recursion every converted species grows its crown with, from the shoots of its named orders
// (the broadleaf's trunk and limbs, the ancient tree's limbs past their runs and its trunk's top).
// A shoot of base radius R carries laterals along its distal part and a **leader** at its tip, and
// the children's cross-sections sum to the parent's with exponent `area_exponent` (the pipe model,
// da Vinci's rule): R^D = sum r_child^D, the leader taking `leader_share` of the area and the
// laterals the rest equally. The recursion stops where a shoot's thinnest child would fall below
// `twig_radius`: that shoot is a **terminal twig**, `twig_length` long whatever the tree's size
// (Corner's rules: the terminal unit is the species', not the tree's), and its leaves are cards of
// the species' own size. So the order count and the twig count follow from the sizes — about
// (R / r_twig)^D twigs — and a bigger tree is more tree, not a scaled-up one.
//
// The structure never depends on the budget: every shoot is grown once, from a stream of its own
// (its index in the depth-first order), and a plan only says how much of it is meshed. Shoots of a
// radius class at or above the part class are motion parts with grown axes (their count capped, so
// a tree has thousands of parts, not millions); every finer shoot is **folded** into its nearest
// part — a straight segment that rides the part at the station it leaves it, meshed as a tube or
// not at all by the plan, its leaves always.

inline constexpr u32 k_crown_none = 0xFFFFFFFFu;
inline constexpr u32 k_crown_terminal = 1u;  // a terminal twig: it carries leaves
inline constexpr u32 k_crown_part = 2u;      // a motion part with a grown axis
inline constexpr u32 k_crown_named = 4u;     // a shoot of the species' named orders

struct CrownSpec {
  f64 area_exponent = 2.0;  // D in R^D = sum r^D
  f64 leader_share = 0.3;   // the leader's share of a node's cross-section
  u32 laterals = 3;         // laterals on a shoot of the recursion (Rb - 1, capped)
  f64 length_ratio = 0.63;  // a child's length over its parent's
  f64 twig_radius = 0.004;
  f64 twig_length = 0.8;
  f64 branch_angle = 0.7;  // a lateral's angle from its parent, radians
  f64 leader_angle = 0.2;  // the leader's
  f64 upturn = 0.35;       // radians a shoot turns towards the light over its length
  f64 wander = 0.08;       // radians a grown step wanders
  f64 from = 0.3;          // laterals leave from this share of their parent's length on
  u32 part_cap = 2000;     // the most recursion shoots that are motion parts
  u64 seed = 1;
  u64 stream = 1000;  // the streams of the recursion's shoots start here
};

struct CrownNode {
  D3 base;
  D3 tip;
  D3 dir;  // the tangent at the base
  f64 r0 = 0.0;
  f64 r1 = 0.0;
  f64 length = 0.0;
  u32 parent = k_crown_none;   // index into the nodes
  u32 generation = 0;          // below the named orders
  i32 cls = 0;                 // radius class: twigs are 0, each class a pipe step thicker
  u32 axis = k_crown_none;     // index into the axes, for a named shoot or a part
  u32 part = k_crown_none;     // the part it is, or rides, set by the emission
  u32 carrier = k_crown_none;  // the part node a folded shoot rides (itself for a part)
  f64 weight = 0.0;            // where a folded shoot rides its part
  u32 cards = 0;               // a twig's leaf cards a side
  u32 flags = 0;
};

// A named shoot the recursion grows from: the cross-section its children share (its radius where
// they leave it), its laterals' count and length, and where they start.
struct CrownRoot {
  u32 node = 0;
  f64 radius = 0.0;
  u32 laterals = 0;
  f64 lateral_length = 0.0;  // the first lateral's; later ones shorter, the leader the last's
  f64 from = 0.25;
  f64 to = 0.95;
  f64 spread_scale = 1.0;
};

struct Crown {
  Vector<CrownNode> nodes;  // the named shoots first, then the recursion depth-first
  Vector<Axis> axes;
  Vector<u32> twigs;  // node indices of the terminal twigs, depth-first
  u32 first_recursion = 0;
  i32 part_class = 0;  // recursion shoots of this class and up are parts
  u32 recursion_parts = 0;
  u32 laterals =
      3;  // a recursion shoot's laterals: each merged level stands for laterals + 1 times the twigs
  u32 orders = 0;       // the most orders from the first named shoot to a twig, both counted
  f64 footprint = 0.0;  // m^2: the convex hull of every shoot tip, seen from above
  f64 card_step = 0.0;  // along a twig, one card's base
};

// The leader's radius under a node of base radius `radius`: what a root's own tube tapers to.
f64 crown_leader_radius(const CrownSpec& spec, f64 radius);

// Adds a named shoot (with its grown axis) and returns its node.
u32 crown_add_named(Crown& crown, const Axis& axis, f64 r0, f64 r1, u32 parent);

// Grows the recursion from the roots, assigns the part class and fills the footprint.
void crown_grow(Crown& crown, const CrownSpec& spec, const Vector<CrownRoot>& roots,
                u32 named_generations);

// Distributes `cards` one-sided leaf cards over the twigs (as evenly as integers go, depth-first,
// each twig at most `cap` a side) and sets each twig's count a side.
void crown_distribute_leaves(Crown& crown, f64 cards, u32 cap);

// How much of the structure a mesh carries (the ladder's rungs move these and nothing else).
struct CrownPlan {
  f64 edge_m = 0.12;  // a tube's circumference step: sides = 2 pi r / edge, 3 to `max_sides`
  u32 max_sides = 12;
  f64 row_m = 1.0;     // a part tube's rows: one per this many metres, 1 to 8
  i32 wood_class = 0;  // folded shoots of this class and up are tubes
  u32 merge = 0;       // leaf cards merged this many generations up their twigs
  u32 halvings = 0;    // merged sprays' cards halved this many times
};

u32 crown_tube_sides(const CrownPlan& plan, f64 radius);
u32 crown_part_rows(const CrownPlan& plan, f64 length);

// The triangles a plan makes of the recursion (the named shoots are the species').
u32 crown_triangles(const Crown& crown, const CrownPlan& plan);

// The bark of the recursion: every part a tube and a part of its own, every folded shoot of the
// plan's classes a tube in its part. Parents first, so the part ids are dense; `structural` also
// records each part's structural row.
void crown_emit_wood(Crown& crown, const CrownPlan& plan, Emitter& out, bool structural);

// Where one spray of the leaves landed: the node it grows on (a twig, or with the plan's merge the
// sub-tree root), and its runs of the mesh's indices and vertices.
struct SprayRange {
  u32 node = 0;
  u32 first_index = 0;
  u32 index_count = 0;
  u32 first_vertex = 0;
  u32 vertex_count = 0;
};

// The leaves: a spray of the twig's cards on every leafy twig, `card_length` long with the step
// `crown.card_step` along it — or, with the plan's merge, one spray per sub-tree root standing for
// its twigs' leaves, its cards sized to the sub-tree's extent. Returns the longest card built.
// `sprays`, when given, receives every spray's ranges in emission order (the impostor chain's).
f64 crown_emit_leaves(const Crown& crown, const CrownPlan& plan, Emitter& out, f64 card_length,
                      f64 angle, f64 lift, f64 droop, Vector<SprayRange>* sprays = nullptr);

// ---- the impostor chain (R89; impostor.h, flora.md "The impostor chain") ----------------------
//
// Appended after everything above and changing none of it: a solver that is not asked for levels
// emits the bytes it did.

// The branch-group levels above the finest leaves: at step k every spray's leaves gather on their
// sub-tree root k orders further up (never past a part), and each sub-tree becomes **one card** in
// the plane its leaves lie nearest (the least-variance plane of their vertices), the size of their
// extent in it, sampling one of `spec.representatives` baked tiles. The cards are appended to the
// mesh past everything (with their motion: the root's carrier part), their triangles kept in the
// level's own indices. Stops where a step merges nothing more.
void crown_levels(const Crown& crown, const CrownPlan& plan, const Vector<SprayRange>& sprays,
                  const ImpostorSpec& spec, Emitter& out, TreeLevels& levels);

// The whole-tree impostor as the last level: four vertical planes through the trunk's axis 45
// degrees apart and one level plane at the crown's centre, each the size of the tree in it, each
// with a tile of its own view of the bark and the finest leaves (`mesh.indices` up to
// `levels.leaf_index_end`). Riding the trunk (`part`).
void tree_star(const ImpostorSpec& spec, u32 part, Emitter& out, TreeLevels& levels);

// The crown as `CrownShoot` rows (trees.h), in node order.
void crown_table(const Crown& crown, Vector<CrownShoot>& out);

// The one-sided area of the triangles from `first_index` on.
f64 triangles_area(const assets::MeshData& mesh, u32 first_index);

// ---- the understory (G75; flora.md "The understory") -------------------------------------------
//
// Added after everything above and changing none of it, so every tree's bytes stay pinned.

// `build_spray` with a card length per station: `lengths` holds `cards + 1` lengths, station k's
// cards that long — a fern's pinnae, longest below the middle and shortening to the tip, and a
// stipe of cards a few millimetres long that is the bare stalk drawn as a ribbon of the same
// surface. Every other rule is `build_spray`'s: one connected surface, the full card per image.
void build_spray_profile(const Axis& axis, f64 from, u32 cards, const Vector<f64>& lengths,
                         f64 angle, f64 lift, f64 droop, D3 side, Emitter& out);

// A blade: a strip along the axis's rows, `width` across at the base, its edges straight to 0.65
// of its length and closing to a point at the tip (so the rows between collapse first), its plane
// across `side`. UVs the full card: u across in [0, 1] at the base's width, v along in [0, 1].
// Weights are each row's arc over the length. `segments` quads, the last a triangle.
inline u32 blade_triangles(u32 segments) { return 2 * segments - 1; }
void build_blade(const Axis& axis, f64 width, D3 side, Emitter& out);

// A leaf card: a diamond from `base` out along `direction` for `length`, widest (`width`) at 0.35
// of it, its tip sunk by `droop` of the length, its plane across `side`. Two triangles, the UVs
// the full card. Every vertex's weight is the caller's.
inline constexpr u32 k_leaf_card_triangles = 2;
void build_leaf_card(D3 base, D3 direction, D3 side, f64 length, f64 width, f64 droop,
                     Emitter& out);

}  // namespace engine::flora::detail
