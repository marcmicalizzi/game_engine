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
#include <domain/flora/motion.h>

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

}  // namespace engine::flora::detail
