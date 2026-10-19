// The date palm's solver (palm.h; docs/subsystems/flora.md, "The date palm").
#include <core/math/deterministic.h>
#include <domain/flora/palm.h>
#include <domain/geometry/cluster.h>

#include <cmath>
#include <string>
#include <utility>

namespace engine::flora {

namespace {

constexpr f64 k_pi = 3.14159265358979323846;
constexpr f64 k_degree = k_pi / 180.0;

// ---- arithmetic ---------------------------------------------------------------------------------
//
// f64 throughout, rounded to the f32 the mesh stores once, at the end: every operation is IEEE and
// contraction is off tree-wide, so a position is a function of the parameters' bits alone.

struct D3 {
  f64 x = 0.0;
  f64 y = 0.0;
  f64 z = 0.0;
};
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
f64 dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
D3 normalized(D3 a) {
  const f64 length = std::sqrt(dot(a, a));
  return length > 0.0 ? a * (1.0 / length) : D3{0.0, 1.0, 0.0};
}
Vec3 to_f32(D3 a) {
  return Vec3{static_cast<f32>(a.x), static_cast<f32>(a.y), static_cast<f32>(a.z)};
}
f64 lerp(f64 a, f64 b, f64 t) { return a + (b - a) * t; }
f64 clamp01(f64 t) { return t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t); }

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

// Independent streams for the parts, so adding a draw to one part never moves another's.
enum class Stream : u64 { trunk = 1, crown = 2, skirt = 3 };
Rng stream(const Palm& p, Stream s) {
  Rng mix(p.seed ^ (static_cast<u64>(s) * 0xD1B54A32D192ED03ull));
  return Rng(mix.next());
}

// ---- the plan: how much detail the budget buys --------------------------------------------------

struct Plan {
  u32 rings = 1;
  u32 rows_per_ring = 4;
  u32 segments = 16;
  u32 rachis_sides = 3;      // a trough (two edges and a keel), or a ribbon of 2
  u32 leaflet_segments = 3;  // along a leaflet
  u32 leaflets = 0;          // per side of a frond
  f64 leaflet_width = 0.0;   // a leaflet's base, metres
  u32 petiole_rows = 5;      // along the rachis below the first leaflet
};

// The rows each ring's profile is drawn with, at these phases of the ring, and how far below the
// trunk's radius the scar cuts at each (1: the full `ring_depth_m`). A ring is a leaf base's
// boot: the trunk steps in sharply at the scar and swells back out to the boot's lip, where the
// radius is the trunk's own — so the radius a caller asked for is the lip's.
struct RingProfile {
  u32 rows;
  f64 phase[4];
  f64 cut[4];
};
constexpr RingProfile k_profiles[] = {
    {4, {0.0, 0.35, 0.7, 0.92}, {0.9, 0.45, 0.12, 0.0}},
    {2, {0.0, 0.8, 0.0, 0.0}, {0.9, 0.0, 0.0, 0.0}},
    {1, {0.0, 0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 0.0}},
};
const RingProfile& profile_of(u32 rows_per_ring) {
  for (const RingProfile& p : k_profiles)
    if (p.rows == rows_per_ring) return p;
  return k_profiles[2];
}

u32 trunk_triangles(const Plan& plan) {
  const u32 rows = plan.rings * plan.rows_per_ring + 1;
  return (rows - 1) * plan.segments * 2 + plan.segments;  // the wall and the apex's cap
}

// A frond's rows: the petiole's, a pair round each leaflet's base, and the tip; between two rows
// the rachis is two triangles a side of its cross-section, and a leaflet of `leaflet_segments` is
// 2 * segments - 1 (a quad a segment, the last one a triangle to the point).
u32 frond_triangles(const Plan& plan) {
  const u32 rows = plan.petiole_rows + 2 * plan.leaflets + 1;
  return (rows - 1) * 2 * (plan.rachis_sides - 1) +
         2 * plan.leaflets * (2 * plan.leaflet_segments - 1);
}

u32 count_triangles(const Palm& p, const Plan& plan) {
  return trunk_triangles(plan) + (p.frond_count + p.dead_skirt_fronds) * frond_triangles(plan);
}

// The rings the trunk's length holds at the spacing asked: the spacing is kept to the nearest
// whole ring, so the scars are evenly spaced from the ground to the apex.
u32 rings_of(const Palm& p, f64 length) {
  const f64 n = std::floor(length / static_cast<f64>(p.ring_spacing_m) + 0.5);
  return n < 1.0 ? 1u : static_cast<u32>(n);
}

// The ladder (flora.md, "The budget"): the full detail first, then each rung in turn until the
// palm fits — fewer rows per ring before fewer segments round the trunk, because the silhouette is
// what a palm is recognized by, and a merged ring last of all.
bool plan_palm(const Palm& p, f64 length, Plan& plan, u32& steps) {
  plan = Plan{};
  plan.rings = rings_of(p, length);
  plan.leaflets = p.leaflet_count;
  plan.leaflet_width = static_cast<f64>(p.leaflet_width_m);
  steps = 0;
  const u32 budget = p.triangle_budget;
  auto fits = [&] { return count_triangles(p, plan) <= budget; };
  if (fits()) return true;
  const auto step = [&](auto&& change) {
    change();
    ++steps;
    return fits();
  };
  // Detail inside a leaflet and a scar ring first: neither changes the outline.
  if (step([&] { plan.leaflet_segments = 2; })) return true;
  if (step([&] { plan.rows_per_ring = 2; })) return true;
  if (step([&] { plan.leaflet_segments = 1; })) return true;
  if (step([&] { plan.segments = 12; })) return true;
  if (step([&] { plan.rachis_sides = 2; })) return true;
  if (step([&] { plan.petiole_rows = 2; })) return true;
  if (step([&] { plan.segments = 8; })) return true;
  if (step([&] { plan.rows_per_ring = 1; })) return true;
  // Then fewer, wider leaflets, keeping the frond's leaf area: the crown's outline is the fronds',
  // and a frond of half the leaflets twice as wide reads as the same frond from a distance.
  while (plan.leaflets > 1) {
    if (step([&] {
          plan.leaflets = (plan.leaflets + 1) / 2;
          plan.leaflet_width *= 2.0;
        })) {
      return true;
    }
  }
  while (plan.rings > 1) {
    if (step([&] { plan.rings = (plan.rings + 1) / 2; })) return true;
  }
  return step([&] { plan.segments = 6; });
}

// ---- the trunk ----------------------------------------------------------------------------------
//
// The spine leans by `lean_deg` towards an azimuth drawn from the seed and bends back towards the
// vertical as it rises: its angle from vertical falls linearly with arc length, from the lean at
// the ground to (1 - curve) of it at the apex. That has a closed form — the spine's height after an
// arc s is (sin a - sin(a - b s)) / b with a the lean and b = a * curve / L — so the arc length
// that puts the apex at exactly `height_m` is solved for rather than searched.

struct Spine {
  f64 lean = 0.0;    // a, radians
  f64 bend = 0.0;    // b, radians per metre
  f64 length = 0.0;  // L, metres of arc
  D3 heading;        // the lean's horizontal direction
  D3 side;           // horizontal and perpendicular to it: the axis the spine bends about
};

Spine make_spine(const Palm& p, Rng& rng) {
  Spine s;
  s.lean = static_cast<f64>(p.lean_deg) * k_degree;
  const f64 azimuth = rng.unit() * 2.0 * k_pi;
  s.heading = D3{det::cos(azimuth), 0.0, det::sin(azimuth)};
  s.side = D3{-det::sin(azimuth), 0.0, det::cos(azimuth)};
  const f64 height = static_cast<f64>(p.height_m);
  const f64 taken = s.lean * static_cast<f64>(p.curve);
  // The height per metre of arc; at a vanishing bend the integral is the straight line's cos a.
  const f64 rise =
      taken > 1e-9 ? (det::sin(s.lean) - det::sin(s.lean - taken)) / taken : det::cos(s.lean);
  s.length = height / rise;
  s.bend = taken / s.length;
  return s;
}

f64 spine_angle(const Spine& s, f64 arc) { return s.lean - s.bend * arc; }

D3 spine_point(const Spine& s, f64 arc) {
  f64 up = 0.0;
  f64 out = 0.0;
  if (s.bend * s.length > 1e-9) {
    const f64 a = spine_angle(s, arc);
    up = (det::sin(s.lean) - det::sin(a)) / s.bend;
    out = (det::cos(a) - det::cos(s.lean)) / s.bend;
  } else {
    up = arc * det::cos(s.lean);
    out = arc * det::sin(s.lean);
  }
  return s.heading * out + D3{0.0, up, 0.0};
}

D3 spine_tangent(const Spine& s, f64 arc) {
  const f64 a = spine_angle(s, arc);
  return s.heading * det::sin(a) + D3{0.0, det::cos(a), 0.0};
}

// The two directions across the spine: `normal` in the bend's plane, `side` out of it, so that
// cos(t) normal + sin(t) side goes round the spine (clockwise seen from above; the wall's winding
// below is chosen for it, and the tests hold every face to point outwards).
D3 spine_normal(const Spine& s, f64 arc) { return cross(s.side, spine_tangent(s, arc)) * -1.0; }

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
  // Opens a part: its record, and the part every vertex until the next one belongs to.
  void begin_part(D3 pivot, D3 direction, f64 length, u32 parent) {
    part = motion.parts.size();
    geometry::ClusterFileMotionPart record;
    record.pivot = to_f32(pivot);
    record.direction = to_f32(normalized(direction));
    record.length = static_cast<f32>(length);
    record.parent = parent;
    motion.parts.push_back(record);
    weight = 0.0;
  }
  void triangle(u32 a, u32 b, u32 c) {
    mesh.indices.push_back(a);
    mesh.indices.push_back(b);
    mesh.indices.push_back(c);
  }
};

// The trunk's radius at arc `s` before the scars: a straight taper from the base to the apex.
f64 taper(const Palm& p, const Spine& spine, f64 s) {
  return lerp(static_cast<f64>(p.trunk_base_radius_m), static_cast<f64>(p.trunk_top_radius_m),
              clamp01(s / spine.length));
}

// One island: u once round (a seam where it closes), v up the trunk at the same scale, so a bark
// texture is not stretched; and the cap a disc of its own beside it.
void build_trunk(const Palm& p, const Spine& spine, const Plan& plan, Rng& rng, Emitter& out) {
  const RingProfile& profile = profile_of(plan.rows_per_ring);
  const u32 rows = plan.rings * plan.rows_per_ring + 1;
  const u32 segs = plan.segments;
  const f64 ring_length = spine.length / static_cast<f64>(plan.rings);
  const f64 depth = static_cast<f64>(p.ring_depth_m);
  const f64 mean_radius =
      0.5 * (static_cast<f64>(p.trunk_base_radius_m) + static_cast<f64>(p.trunk_top_radius_m));
  const f64 v_scale = 1.0 / (2.0 * k_pi * mean_radius);

  // Each ring turns a little against the one below it, which is what makes a palm's scars read as
  // grown rather than lathed; the turn is interpolated through a ring so the wall never shears.
  Vector<f64> twist(plan.rings + 1, 0.0);
  for (u32 k = 1; k <= plan.rings; ++k)
    twist[k] = twist[k - 1] + rng.symmetric() * 4.0 * k_degree;

  Vector<f64> arc(rows);
  Vector<f64> radius(rows);
  Vector<f64> turn(rows);
  for (u32 r = 0; r < rows; ++r) {
    const u32 ring = r / plan.rows_per_ring;
    const u32 j = r % plan.rows_per_ring;
    const f64 phase = ring < plan.rings ? profile.phase[j] : 0.0;
    const f64 s = r + 1 == rows ? spine.length : (static_cast<f64>(ring) + phase) * ring_length;
    arc[r] = s;
    // The apex row is the lip of the last ring: the radius asked for at the top.
    const f64 cut = r + 1 == rows ? 0.0 : profile.cut[j];
    radius[r] = taper(p, spine, s) - depth * cut;
    turn[r] = ring < plan.rings ? lerp(twist[ring], twist[ring + 1], phase) : twist[plan.rings];
  }

  // The trunk is part 0: its pivot the spine's foot, its weight the arc up the spine.
  out.begin_part(spine_point(spine, 0.0), spine_tangent(spine, 0.0), spine.length,
                 geometry::k_motion_no_parent);
  const u32 first = out.mesh.positions.size();
  for (u32 r = 0; r < rows; ++r) {
    out.weight = arc[r] / spine.length;
    const D3 centre = spine_point(spine, arc[r]);
    const D3 tangent = spine_tangent(spine, arc[r]);
    const D3 normal = spine_normal(spine, arc[r]);
    // The wall's slope where the profile cuts in or swells out tilts its normal along the spine, at
    // half its geometric steepness: the scars still shade, and a coarse level that has merged a
    // ring's rows shades the trunk without the bands a full-strength normal left (flora.md, "What
    // the harness taught").
    const u32 lo = r > 0 ? r - 1 : r;
    const u32 hi = r + 1 < rows ? r + 1 : r;
    const f64 slope = arc[hi] > arc[lo] ? (radius[hi] - radius[lo]) / (arc[hi] - arc[lo]) : 0.0;
    for (u32 i = 0; i <= segs; ++i) {
      const f64 angle = turn[r] + 2.0 * k_pi * static_cast<f64>(i) / static_cast<f64>(segs);
      const D3 radial = normal * det::cos(angle) + spine.side * det::sin(angle);
      out.vertex(centre + radial * radius[r], radial - tangent * (0.5 * slope),
                 static_cast<f64>(i) / static_cast<f64>(segs), arc[r] * v_scale);
    }
  }
  for (u32 r = 0; r + 1 < rows; ++r) {
    for (u32 i = 0; i < segs; ++i) {
      const u32 a = first + r * (segs + 1) + i;
      const u32 b = a + 1;
      const u32 c = a + segs + 1;
      const u32 d = c + 1;
      out.triangle(a, d, b);
      out.triangle(a, c, d);
    }
  }

  // The apex's cap: a low cone over the last row, its own small disc of the atlas beside the wall.
  const f64 top = spine.length;
  const D3 tangent = spine_tangent(spine, top);
  const D3 normal = spine_normal(spine, top);
  const D3 apex = spine_point(spine, top) + tangent * (0.35 * radius[rows - 1]);
  const f64 cap_radius = 2.0 * k_pi * radius[rows - 1] * v_scale;  // the disc at the wall's scale
  out.weight = 1.0;
  const u32 centre = out.vertex(apex, tangent, 0.5, -0.5 - cap_radius);
  const u32 ring_first = out.mesh.positions.size();
  for (u32 i = 0; i < segs; ++i) {
    const f64 angle = turn[rows - 1] + 2.0 * k_pi * static_cast<f64>(i) / static_cast<f64>(segs);
    const f64 c = det::cos(angle);
    const f64 s = det::sin(angle);
    const D3 radial = normal * c + spine.side * s;
    const D3 position = spine_point(spine, top) + radial * radius[rows - 1];
    out.vertex(position, radial + tangent, 0.5 + c * cap_radius,
               -0.5 - cap_radius + s * cap_radius);
  }
  for (u32 i = 0; i < segs; ++i)
    out.triangle(ring_first + i, centre, ring_first + (i + 1) % segs);
}

// ---- the crown and the skirt --------------------------------------------------------------------
//
// A frond is one connected surface: the rachis a strip of rows along a curve, and on each side of
// it a comb of leaflets whose bases are the rachis's own edge vertices. One surface rather than a
// rachis and a few hundred loose strips because the LOD builder can only collapse what is
// connected — a loose leaflet keeps its last triangle at every level, and a crown of them does not
// coarsen — and one UV island for the same reason the other way round: an island's edge is a seam
// the simplifier may not collapse across (geometry.md, "What the simplifier is given").

struct FrondShape {
  D3 base;           // where the rachis starts
  D3 axis;           // the crown's up: a frond's angle from vertical is measured from it
  D3 out;            // across `axis`, towards the frond's azimuth
  D3 across;         // the frond's lateral, so that cross(across, out) is `axis`
  f64 length = 0.0;  // of the rachis
  f64 start = 0.0;   // angle from `axis` at the base, radians
  f64 bend = 0.0;    // further angle by the tip; it grows with the square of the arc
  f64 leaflet_angle = 0.0;
  f64 vee = 0.0;  // how far a leaflet rises out of the rachis's plane, radians
  f64 leaflet_scale = 1.0;
  f64 sag = 0.0;  // a leaflet's droop under its own weight, in its lengths
};

constexpr f64 k_max_frond_angle = 178.0 * k_degree;
constexpr f64 k_petiole = 0.22;  // the share of the rachis below the first leaflet
constexpr f64 k_golden_angle = 2.39996322972865332;  // pi (3 - sqrt 5): phyllotaxis

f64 frond_angle(const FrondShape& f, f64 arc) {
  const f64 t = arc / f.length;
  const f64 a = f.start + f.bend * t * t;
  return a < k_max_frond_angle ? a : k_max_frond_angle;
}

D3 frond_direction(const FrondShape& f, f64 angle) {
  return f.out * det::sin(angle) + f.axis * det::cos(angle);
}

// The rachis's half-width: a petiole's at the base, a few millimetres at the tip, in proportion to
// the frond's length.
f64 rachis_half_width(const FrondShape& f, f64 arc) {
  const f64 scale = f.length / 3.8;
  return lerp(0.035 * scale, 0.004 * scale, clamp01(arc / f.length));
}

// A leaflet's length at `x` along the leaflet-bearing part: longest a little past the middle,
// shorter towards the base and the tip, as a date palm's pinnae are; at most a ninth of the frond.
f64 leaflet_length(const FrondShape& f, f64 x) {
  return f.length * 0.11 * f.leaflet_scale * (0.3 + 0.7 * det::sin(k_pi * (0.12 + 0.76 * x)));
}

// Emits one frond. Every triangle is wound to face the frond's upper side at its station, so the
// area-weighted normals the crown gets are a surface's and not a cancellation.
void build_frond(const FrondShape& f, const Plan& plan, Emitter& out) {
  const u32 n = plan.leaflets;
  const f64 first_leaflet = f.length * k_petiole;
  const f64 spacing = (f.length - first_leaflet) / static_cast<f64>(n);
  const f64 half = 0.5 * (plan.leaflet_width < 0.9 * spacing ? plan.leaflet_width : 0.9 * spacing);

  // The rows along the rachis: the petiole's, then a pair round each leaflet's base, then the tip.
  Vector<f64> arcs;
  for (u32 i = 0; i < plan.petiole_rows; ++i)
    arcs.push_back(first_leaflet * static_cast<f64>(i) / static_cast<f64>(plan.petiole_rows));
  for (u32 i = 0; i < n; ++i) {
    const f64 centre = first_leaflet + (static_cast<f64>(i) + 0.5) * spacing;
    arcs.push_back(centre - half);
    arcs.push_back(centre + half);
  }
  arcs.push_back(f.length);
  const u32 rows = arcs.size();

  // The centre line, integrated row to row at each step's middle angle, and each row's frame.
  Vector<D3> centre(rows);
  Vector<D3> tangent(rows);
  Vector<D3> up(rows);
  D3 at = f.base;
  for (u32 r = 0; r < rows; ++r) {
    if (r > 0) {
      const f64 mid = 0.5 * (arcs[r - 1] + arcs[r]);
      at = at + frond_direction(f, frond_angle(f, mid)) * (arcs[r] - arcs[r - 1]);
    }
    centre[r] = at;
    tangent[r] = frond_direction(f, frond_angle(f, arcs[r]));
    up[r] = cross(f.across, tangent[r]);
  }

  const auto face = [&](u32 a, u32 b, u32 c, D3 hint) {
    const Vec3 pa = out.mesh.positions[a];
    const Vec3 pb = out.mesh.positions[b];
    const Vec3 pc = out.mesh.positions[c];
    const Vec3 nf = cross(pb - pa, pc - pa);
    const f64 facing = static_cast<f64>(nf.x) * hint.x + static_cast<f64>(nf.y) * hint.y +
                       static_cast<f64>(nf.z) * hint.z;
    if (facing >= 0.0) {
      out.triangle(a, b, c);
    } else {
      out.triangle(a, c, b);
    }
  };
  // The chart: metres along the rachis and across it at one scale, so a frond texture is not
  // stretched; every frond shares it. The normal is a placeholder the crown's pass replaces.
  const f64 to_uv = 1.0 / f.length;
  const auto vertex = [&](D3 p, D3 normal, f64 along, f64 lateral) {
    return out.vertex(p, normal, along * to_uv, 0.5 + lateral * to_uv);
  };

  // The rachis: a trough of three vertices across (two edges and a keel below), or a ribbon of two.
  Vector<u32> left(rows);
  Vector<u32> right(rows);
  Vector<u32> keel(rows);
  // A frond is a part of its own hanging from the trunk; its weight is the arc along the rachis.
  out.begin_part(f.base, tangent[0], f.length, k_trunk_part);
  for (u32 r = 0; r < rows; ++r) {
    out.weight = arcs[r] / f.length;
    const f64 w = rachis_half_width(f, arcs[r]);
    left[r] = vertex(centre[r] + f.across * w, up[r], arcs[r], w);
    right[r] = vertex(centre[r] - f.across * w, up[r], arcs[r], -w);
    if (plan.rachis_sides == 3)
      keel[r] = vertex(centre[r] - up[r] * (0.6 * w), up[r], arcs[r], 0.0);
  }
  for (u32 r = 0; r + 1 < rows; ++r) {
    if (plan.rachis_sides == 3) {
      face(left[r], keel[r], keel[r + 1], up[r]);
      face(left[r], keel[r + 1], left[r + 1], up[r]);
      face(keel[r], right[r], right[r + 1], up[r]);
      face(keel[r], right[r + 1], keel[r + 1], up[r]);
    } else {
      face(left[r], right[r], right[r + 1], up[r]);
      face(left[r], right[r + 1], left[r + 1], up[r]);
    }
  }

  // The leaflets: each from its two base vertices on the rachis's edge, out at the leaflet angle
  // and up out of the rachis's plane by the vee, tapering to a point and sagging a little.
  const f64 ca = det::cos(f.leaflet_angle);
  const f64 sa = det::sin(f.leaflet_angle);
  const f64 cv = det::cos(f.vee);
  const f64 sv = det::sin(f.vee);
  for (u32 i = 0; i < n; ++i) {
    const u32 r0 = plan.petiole_rows + 2 * i;
    const u32 r1 = r0 + 1;
    const f64 length = leaflet_length(f, (static_cast<f64>(i) + 0.5) / static_cast<f64>(n));
    const f64 mid_arc = 0.5 * (arcs[r0] + arcs[r1]);
    out.weight = mid_arc / f.length;  // a leaflet is carried by the rachis where it stands
    const f64 base_half = 0.5 * (arcs[r1] - arcs[r0]);
    const f64 w = rachis_half_width(f, mid_arc);
    const D3 t = normalized(tangent[r0] + tangent[r1]);
    const D3 u = normalized(up[r0] + up[r1]);
    for (const f64 side : {1.0, -1.0}) {
      const u32 b0 = side > 0.0 ? left[r0] : right[r0];
      const u32 b1 = side > 0.0 ? left[r1] : right[r1];
      const D3 p0 = centre[r0] + f.across * (side * rachis_half_width(f, arcs[r0]));
      const D3 p1 = centre[r1] + f.across * (side * rachis_half_width(f, arcs[r1]));
      const D3 mid = (p0 + p1) * 0.5;
      const D3 dir = normalized(t * ca + (f.across * (side * cv) + u * sv) * sa);
      const D3 edge = normalized(p1 - p0);
      D3 hint = normalized(cross(edge, dir));
      if (dot(hint, u) < 0.0) hint = hint * -1.0;
      u32 prev0 = b0;
      u32 prev1 = b1;
      for (u32 j = 1; j <= plan.leaflet_segments; ++j) {
        const f64 s = static_cast<f64>(j) / static_cast<f64>(plan.leaflet_segments);
        const D3 along = mid + dir * (length * s) - D3{0.0, 1.0, 0.0} * (f.sag * length * s * s);
        const f64 chart_along = mid_arc + length * s * ca;
        const f64 chart_across = side * (w + length * s * sa);
        if (j == plan.leaflet_segments) {
          const u32 tip = vertex(along, hint, chart_along, chart_across);
          face(prev0, prev1, tip, hint);
        } else {
          // A pinna is linear: nearly its full width for most of its length, narrowing only
          // towards the tip, which is what keeps a crown from reading as sticks at the distance
          // where a leaflet is a pixel wide. Its edges are straight — a long, slightly tapering
          // quad to the last row, then a point — so the rows in between are collinear with it
          // and the simplifier drops them first at no cost in area (flora.md, "What the
          // harness taught"); a curved taper lost a quarter of every leaflet at the first level.
          const f64 h = base_half * (1.0 - 0.27 * s);
          const u32 v0 = vertex(along - edge * h, hint, chart_along - h, chart_across);
          const u32 v1 = vertex(along + edge * h, hint, chart_along + h, chart_across);
          face(prev0, prev1, v1, hint);
          face(prev0, v1, v0, hint);
          prev0 = v0;
          prev1 = v1;
        }
      }
    }
  }
}

// The apex's frame: where the crown stands and which way is its up.
struct Apex {
  D3 point;
  D3 axis;
  D3 normal;
  D3 side;
  f64 radius = 0.0;
};

Apex apex_of(const Palm& p, const Spine& spine) {
  Apex a;
  a.point = spine_point(spine, spine.length);
  a.axis = spine_tangent(spine, spine.length);
  a.normal = spine_normal(spine, spine.length);
  a.side = spine.side;
  a.radius = static_cast<f64>(p.trunk_top_radius_m);
  return a;
}

void orient(FrondShape& f, const Apex& apex, f64 azimuth) {
  const f64 c = det::cos(azimuth);
  const f64 s = det::sin(azimuth);
  f.axis = apex.axis;
  f.out = apex.normal * c + apex.side * s;
  f.across = apex.side * c - apex.normal * s;
}

// The green fronds, youngest (k = 0) up the middle and the oldest at the cone's edge, round the
// apex on the golden angle as a palm's leaves come, and the older ones springing a little lower.
void build_crown(const Palm& p, const Apex& apex, const Plan& plan, Rng& rng, Emitter& out) {
  const f64 turn = rng.unit() * 2.0 * k_pi;
  const u32 count = p.frond_count;
  for (u32 k = 0; k < count; ++k) {
    const f64 x = std::sqrt((static_cast<f64>(k) + 0.5) / static_cast<f64>(count));
    FrondShape f;
    orient(f, apex, turn + static_cast<f64>(k) * k_golden_angle + rng.symmetric() * 0.1);
    f.start = static_cast<f64>(p.frond_spread_deg) * k_degree * x * (1.0 + 0.08 * rng.symmetric());
    f.bend = static_cast<f64>(p.frond_droop) * 100.0 * k_degree * (0.5 + 0.5 * x);
    f.length =
        static_cast<f64>(p.frond_length_m) * (0.8 + 0.2 * x) * (1.0 + 0.05 * rng.symmetric());
    f.base = apex.point + f.out * (0.5 * apex.radius * x) - f.axis * (1.5 * apex.radius * x);
    f.leaflet_angle = static_cast<f64>(p.leaflet_angle_deg) * k_degree;
    f.vee = 28.0 * k_degree;
    f.sag = 0.12;
    build_frond(f, plan, out);
  }
}

// The dead fronds: below the crown, round the trunk, hanging down along it with their leaflets
// folded in.
void build_skirt(const Palm& p, const Spine& spine, const Apex& apex, const Plan& plan, Rng& rng,
                 Emitter& out) {
  const f64 turn = rng.unit() * 2.0 * k_pi;
  const u32 count = p.dead_skirt_fronds;
  const f64 frond = static_cast<f64>(p.frond_length_m);
  for (u32 j = 0; j < count; ++j) {
    const f64 x = (static_cast<f64>(j) + 0.5) / static_cast<f64>(count);
    FrondShape f;
    orient(f, apex, turn + static_cast<f64>(j) * k_golden_angle + rng.symmetric() * 0.1);
    const f64 drop = 1.5 * apex.radius + (0.15 + 0.85 * x) * 0.25 * frond;
    f64 arc = spine.length - drop;
    if (arc < 0.3 * spine.length) arc = 0.3 * spine.length;
    f.base = spine_point(spine, arc) + f.out * (0.9 * taper(p, spine, arc));
    f.start = (120.0 + 25.0 * rng.unit()) * k_degree;
    f.bend = 40.0 * k_degree;
    f.length = 0.85 * frond * (1.0 + 0.05 * rng.symmetric());
    f.leaflet_angle = 0.75 * static_cast<f64>(p.leaflet_angle_deg) * k_degree;
    f.vee = -5.0 * k_degree;
    f.leaflet_scale = 0.95;
    f.sag = 0.3;
    build_frond(f, plan, out);
  }
}

assets::Material material(const char* name, Vec4 colour, f32 roughness, bool two_sided) {
  assets::Material m;
  m.name = name;
  m.base_color = colour;
  m.metallic = 0.0f;
  m.roughness = roughness;
  m.double_sided = two_sided;
  return m;
}

}  // namespace

bool palm_in_range(const Palm& p, std::string& error) {
  const auto bad = [&](const char* field, const char* range) {
    error = std::string("engine.flora.Palm.") + field + " is out of range (" + range + ")";
    return false;
  };
  // Written so that a NaN fails every test.
  if (!(p.height_m >= 1.0f && p.height_m <= 30.0f)) return bad("height_m", "1 to 30 m");
  if (!(p.trunk_base_radius_m >= 0.05f && p.trunk_base_radius_m <= 1.0f))
    return bad("trunk_base_radius_m", "0.05 to 1 m");
  if (!(p.trunk_top_radius_m >= 0.03f && p.trunk_top_radius_m <= p.trunk_base_radius_m))
    return bad("trunk_top_radius_m", "0.03 m to trunk_base_radius_m");
  if (!(p.lean_deg >= 0.0f && p.lean_deg <= 40.0f)) return bad("lean_deg", "0 to 40 degrees");
  if (!(p.curve >= 0.0f && p.curve <= 1.0f)) return bad("curve", "0 to 1");
  if (!(p.ring_spacing_m >= 0.04f && p.ring_spacing_m <= 1.0f))
    return bad("ring_spacing_m", "0.04 to 1 m");
  if (!(p.ring_depth_m >= 0.0f && p.ring_depth_m <= 0.1f &&
        p.ring_depth_m <= 0.25f * p.trunk_top_radius_m))
    return bad("ring_depth_m", "0 to 0.1 m and a quarter of trunk_top_radius_m");
  if (!(p.frond_count >= 1u && p.frond_count <= 64u)) return bad("frond_count", "1 to 64");
  if (!(p.frond_length_m >= 0.5f && p.frond_length_m <= 8.0f))
    return bad("frond_length_m", "0.5 to 8 m");
  if (!(p.frond_droop >= 0.0f && p.frond_droop <= 1.0f)) return bad("frond_droop", "0 to 1");
  if (!(p.frond_spread_deg >= 5.0f && p.frond_spread_deg <= 120.0f))
    return bad("frond_spread_deg", "5 to 120 degrees");
  if (!(p.leaflet_count >= 2u && p.leaflet_count <= 160u)) return bad("leaflet_count", "2 to 160");
  if (!(p.leaflet_width_m >= 0.005f && p.leaflet_width_m <= 0.12f))
    return bad("leaflet_width_m", "0.005 to 0.12 m");
  if (!(p.leaflet_angle_deg >= 10.0f && p.leaflet_angle_deg <= 85.0f))
    return bad("leaflet_angle_deg", "10 to 85 degrees");
  if (p.dead_skirt_fronds > 64u) return bad("dead_skirt_fronds", "0 to 64");
  if (!(p.triangle_budget >= 2000u && p.triangle_budget <= 2000000u))
    return bad("triangle_budget", "2,000 to 2,000,000");
  return true;
}

bool solve_palm(const Palm& p, assets::MeshData& out, PalmReport* report, std::string* error,
                Motion* motion) {
  out = assets::MeshData{};
  Motion parts;
  std::string message;
  if (!palm_in_range(p, message)) {
    if (error != nullptr) *error = message;
    return false;
  }
  Rng trunk_rng = stream(p, Stream::trunk);
  const Spine spine = make_spine(p, trunk_rng);
  Plan plan;
  u32 steps = 0;
  if (!plan_palm(p, spine.length, plan, steps)) {
    if (error != nullptr) {
      *error = "engine.flora.Palm.triangle_budget " + std::to_string(p.triangle_budget) +
               " cannot hold this palm: its coarsest form needs " +
               std::to_string(count_triangles(p, plan)) + " triangles";
    }
    return false;
  }

  // Flat PBR constants, in linear colour, until the textures (roadmap G60): bark brown, the
  // fronds' grey green, and the dead fronds' tan.
  out.materials.push_back(material("bark", Vec4{0.33f, 0.24f, 0.16f, 1.0f}, 0.9f, false));
  out.materials.push_back(material("frond", Vec4{0.13f, 0.21f, 0.07f, 1.0f}, 0.55f, true));
  out.materials.push_back(material("dead frond", Vec4{0.45f, 0.33f, 0.18f, 1.0f}, 0.85f, true));
  Emitter emit{out, parts};
  const auto primitive = [&](u32 first, i32 material_index) {
    assets::Primitive part;
    part.first_index = first;
    part.index_count = out.indices.size() - first;
    part.material = material_index;
    if (part.index_count > 0) out.primitives.push_back(part);
    return part.index_count / 3;
  };
  build_trunk(p, spine, plan, trunk_rng, emit);
  const u32 trunk_tris = primitive(0, 0);

  const Apex apex = apex_of(p, spine);
  const u32 fronds_vertex = out.positions.size();
  const u32 crown_first = out.indices.size();
  Rng crown_rng = stream(p, Stream::crown);
  build_crown(p, apex, plan, crown_rng, emit);
  const u32 crown_tris = primitive(crown_first, 1);
  const u32 skirt_first = out.indices.size();
  Rng skirt_rng = stream(p, Stream::skirt);
  build_skirt(p, spine, apex, plan, skirt_rng, emit);
  const u32 skirt_tris = primitive(skirt_first, 2);

  // The fronds' normals: area-weighted over their own faces (the trunk's are analytic, and its
  // seam column would not survive the average).
  {
    Vector<Vec3> normals;
    const std::span<const u32> fronds(out.indices.data() + crown_first,
                                      out.indices.size() - crown_first);
    geometry::compute_vertex_normals(out.positions, fronds, normals);
    for (u32 v = fronds_vertex; v < out.positions.size(); ++v)
      out.normals[v] = normals[v];
  }
  if (motion != nullptr) *motion = std::move(parts);

  if (report != nullptr) {
    *report = PalmReport{};
    report->trunk_triangles = trunk_tris;
    report->crown_triangles = crown_tris;
    report->skirt_triangles = skirt_tris;
    report->triangles = out.indices.size() / 3;
    report->vertices = out.positions.size();
    report->rings = plan.rings;
    report->rows_per_ring = plan.rows_per_ring;
    report->trunk_segments = plan.segments;
    report->rachis_sides = plan.rachis_sides;
    report->leaflet_segments = plan.leaflet_segments;
    report->leaflets_per_side = plan.leaflets;
    report->leaflet_width_m = static_cast<f32>(plan.leaflet_width);
    report->steps = steps;
  }
  return true;
}

}  // namespace engine::flora
