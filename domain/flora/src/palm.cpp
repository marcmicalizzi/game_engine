// The date palm's solver (palm.h; docs/subsystems/flora.md, "The date palm").
#include <core/math/deterministic.h>
#include <domain/flora/palm.h>

#include <cmath>
#include <string>

namespace engine::flora {

namespace {

constexpr f64 k_pi = 3.14159265358979323846;
constexpr f64 k_degree = k_pi / 180.0;

// ---- arithmetic
// -----------------------------------------------------------------------------------
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

// ---- the plan: how much detail the budget buys
// ----------------------------------------------------

struct Plan {
  u32 rings = 1;
  u32 rows_per_ring = 4;
  u32 segments = 16;
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

u32 count_triangles(const Plan& plan) { return trunk_triangles(plan); }

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
  steps = 0;
  const u32 budget = p.triangle_budget;
  auto fits = [&] { return count_triangles(plan) <= budget; };
  if (fits()) return true;
  const auto step = [&](auto&& change) {
    change();
    ++steps;
    return fits();
  };
  if (step([&] { plan.rows_per_ring = 2; })) return true;
  if (step([&] { plan.segments = 12; })) return true;
  if (step([&] { plan.segments = 8; })) return true;
  if (step([&] { plan.rows_per_ring = 1; })) return true;
  while (plan.rings > 1) {
    if (step([&] { plan.rings = (plan.rings + 1) / 2; })) return true;
  }
  return step([&] { plan.segments = 6; });
}

// ---- the trunk
// ------------------------------------------------------------------------------------
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

struct Emitter {
  assets::MeshData& mesh;
  u32 vertex(D3 position, D3 normal, f64 u, f64 v) {
    const u32 index = mesh.positions.size();
    mesh.positions.push_back(to_f32(position));
    mesh.normals.push_back(to_f32(normalized(normal)));
    mesh.uvs.push_back(Vec2{static_cast<f32>(u), static_cast<f32>(v)});
    return index;
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

  const u32 first = out.mesh.positions.size();
  for (u32 r = 0; r < rows; ++r) {
    const D3 centre = spine_point(spine, arc[r]);
    const D3 tangent = spine_tangent(spine, arc[r]);
    const D3 normal = spine_normal(spine, arc[r]);
    // The wall's slope where the profile cuts in or swells out tilts its normal along the spine.
    const u32 lo = r > 0 ? r - 1 : r;
    const u32 hi = r + 1 < rows ? r + 1 : r;
    const f64 slope = arc[hi] > arc[lo] ? (radius[hi] - radius[lo]) / (arc[hi] - arc[lo]) : 0.0;
    for (u32 i = 0; i <= segs; ++i) {
      const f64 angle = turn[r] + 2.0 * k_pi * static_cast<f64>(i) / static_cast<f64>(segs);
      const D3 radial = normal * det::cos(angle) + spine.side * det::sin(angle);
      out.vertex(centre + radial * radius[r], radial - tangent * slope,
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

bool solve_palm(const Palm& p, assets::MeshData& out, PalmReport* report, std::string* error) {
  out = assets::MeshData{};
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
               std::to_string(count_triangles(plan)) + " triangles";
    }
    return false;
  }

  out.materials.push_back(material("bark", Vec4{0.33f, 0.24f, 0.16f, 1.0f}, 0.9f, false));
  Emitter emit{out};
  build_trunk(p, spine, plan, trunk_rng, emit);
  assets::Primitive trunk;
  trunk.first_index = 0;
  trunk.index_count = out.indices.size();
  trunk.material = 0;
  out.primitives.push_back(trunk);

  if (report != nullptr) {
    *report = PalmReport{};
    report->trunk_triangles = trunk.index_count / 3;
    report->triangles = out.indices.size() / 3;
    report->vertices = out.positions.size();
    report->rings = plan.rings;
    report->rows_per_ring = plan.rows_per_ring;
    report->trunk_segments = plan.segments;
    report->steps = steps;
  }
  return true;
}

}  // namespace engine::flora
