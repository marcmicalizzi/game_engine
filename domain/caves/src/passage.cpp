// The passage solver (caves.h; docs/subsystems/caves.md, "The shell", "Speleothems"). Products,
// sums and square roots only — correctly rounded on every toolchain — and an integer hash for the
// noise and the drips, so a recipe is the same mesh everywhere.
#include <core/hash/hash.h>
#include <domain/caves/caves.h>

#include <algorithm>
#include <cmath>

namespace engine::caves {

namespace {

constexpr f64 k_pi = 3.14159265358979323846;

// cos and sin of 2 pi k / n without libm's trigonometry, which is not correctly rounded and
// differs between C libraries: the Taylor sums to x^13 on [0, pi/4] (an error under 1e-16 there),
// reduced by octant, so a circle is the same bits on every toolchain and exactly symmetric.
f64 sin_poly(f64 x) noexcept {
  const f64 x2 = x * x;
  return x *
         (1.0 -
          x2 / 6.0 *
              (1.0 - x2 / 20.0 *
                         (1.0 - x2 / 42.0 *
                                    (1.0 - x2 / 72.0 * (1.0 - x2 / 110.0 * (1.0 - x2 / 156.0))))));
}
f64 cos_poly(f64 x) noexcept {
  const f64 x2 = x * x;
  return 1.0 -
         x2 / 2.0 *
             (1.0 -
              x2 / 12.0 *
                  (1.0 - x2 / 30.0 * (1.0 - x2 / 56.0 * (1.0 - x2 / 90.0 * (1.0 - x2 / 132.0)))));
}
// cos and sin of 2 pi k / n.
void unit_circle(u32 k, u32 n, f64& c, f64& s) noexcept {
  // In eighths of a turn: the octant and the remainder in [0, 1/8) of a turn, as exact rationals.
  const u64 num = static_cast<u64>(k % n) * 8u;
  const u32 octant = static_cast<u32>(num / n);
  const f64 frac = static_cast<f64>(num % n) / static_cast<f64>(n);  // [0, 1)
  const f64 a = frac * (3.14159265358979323846 / 4.0);               // [0, pi/4)
  const f64 ca = cos_poly(a), sa = sin_poly(a);
  const f64 cb = cos_poly(3.14159265358979323846 / 4.0 - a);
  const f64 sb = sin_poly(3.14159265358979323846 / 4.0 - a);
  switch (octant) {
    case 0:
      c = ca;
      s = sa;
      break;
    case 1:
      c = sb;
      s = cb;
      break;
    case 2:
      c = -sa;
      s = ca;
      break;
    case 3:
      c = -cb;
      s = sb;
      break;
    case 4:
      c = -ca;
      s = -sa;
      break;
    case 5:
      c = -sb;
      s = -cb;
      break;
    case 6:
      c = sa;
      s = -ca;
      break;
    default:
      c = cb;
      s = -sb;
      break;
  }
}

f64 sgn(f64 v) noexcept { return v < 0.0 ? -1.0 : 1.0; }

// A hash in [0, 1).
f64 unit_hash(u64 seed, u64 a, u64 b, u64 c) noexcept {
  const u64 h = hash_combine(hash_combine(hash_combine(mix64(seed), a), b), c);
  return static_cast<f64>(h >> 11) * (1.0 / 9007199254740992.0);
}

// Smooth value noise in [-1, 1] over a lattice of (ring, vertex) cells `cell` apart, periodic round
// the ring (`n` vertices).
f64 wall_noise(u64 seed, f64 ring, u32 k, u32 n) noexcept {
  constexpr f64 cell = 3.0;
  const f64 r = ring / cell;
  const f64 kk = static_cast<f64>(k) / cell;
  const u32 cells_round = std::max<u32>(1u, static_cast<u32>(static_cast<f64>(n) / cell));
  const i64 r0 = static_cast<i64>(std::floor(r));
  const i64 k0 = static_cast<i64>(std::floor(kk));
  const f64 fr = r - static_cast<f64>(r0);
  const f64 fk = kk - static_cast<f64>(k0);
  const auto v = [&](i64 a, i64 b) {
    const u64 bb = static_cast<u64>(((b % cells_round) + cells_round) % cells_round);
    return unit_hash(seed, 0x57414C4Cull, static_cast<u64>(a), bb) * 2.0 - 1.0;
  };
  const f64 sr = fr * fr * (3.0 - 2.0 * fr);
  const f64 sk = fk * fk * (3.0 - 2.0 * fk);
  const f64 a = v(r0, k0) + (v(r0, k0 + 1) - v(r0, k0)) * sk;
  const f64 b = v(r0 + 1, k0) + (v(r0 + 1, k0 + 1) - v(r0 + 1, k0)) * sk;
  return a + (b - a) * sr;
}

Vec3 v3(f64 x, f64 y, f64 z) noexcept {
  return Vec3{static_cast<f32>(x), static_cast<f32>(y), static_cast<f32>(z)};
}

struct D3 {
  f64 x = 0.0, y = 0.0, z = 0.0;
  D3 operator+(D3 o) const noexcept { return {x + o.x, y + o.y, z + o.z}; }
  D3 operator-(D3 o) const noexcept { return {x - o.x, y - o.y, z - o.z}; }
  D3 operator*(f64 s) const noexcept { return {x * s, y * s, z * s}; }
};
D3 d3(Vec3 v) noexcept {
  return D3{static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)};
}
f64 dot(D3 a, D3 b) noexcept { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) noexcept {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
f64 length(D3 a) noexcept { return std::sqrt(dot(a, a)); }
D3 normalized(D3 a) noexcept {
  const f64 l = length(a);
  return l > 0.0 ? a * (1.0 / l) : D3{0.0, 1.0, 0.0};
}

// Catmull-Rom through `p` (centripetal-free, uniform), at parameter t in [0, n - 1].
D3 spline(const Vector<D3>& p, f64 t) noexcept {
  const u32 n = p.size();
  const u32 i = std::min<u32>(static_cast<u32>(t), n - 2);
  const f64 u = t - static_cast<f64>(i);
  const D3 p0 = p[i == 0 ? 0 : i - 1];
  const D3 p1 = p[i];
  const D3 p2 = p[i + 1];
  const D3 p3 = p[std::min<u32>(i + 2, n - 1)];
  const f64 u2 = u * u, u3 = u2 * u;
  return (p1 * 2.0 + (p2 - p0) * u + (p0 * 2.0 - p1 * 5.0 + p2 * 4.0 - p3) * u2 +
          (p1 * 3.0 - p0 - p2 * 3.0 + p3) * u3) *
         0.5;
}

}  // namespace

f32 segment_height(const PassageSegment& segment) noexcept {
  if (segment.height_m > 0.0f) return segment.height_m;
  switch (segment.profile) {
    case PassageProfile::Vadose: return segment.width_m * 3.0f;
    case PassageProfile::Keyhole: return segment.width_m * 0.5f * 2.2f;  // tube + 1.2 tube heights
    case PassageProfile::Phreatic:
    default: return segment.width_m * 0.5f;
  }
}

Vec2 profile_point(PassageProfile profile, f32 width_m, f32 height_m, u32 k, u32 n) noexcept {
  f64 c = 0.0, s = 0.0;
  unit_circle(k, n, c, s);
  const f64 a = static_cast<f64>(width_m) * 0.5;
  const f64 h = static_cast<f64>(height_m);
  switch (profile) {
    case PassageProfile::Vadose: {
      // A superellipse of exponent 4, tall: flat walls, a rounded roof and floor.
      const f64 x = a * sgn(c) * std::sqrt(std::fabs(c));
      const f64 y = h * 0.5 * sgn(s) * std::sqrt(std::fabs(s));
      return Vec2{static_cast<f32>(x), static_cast<f32>(y)};
    }
    case PassageProfile::Keyhole: {
      // The tube (its height h / 2.2) over a canyon 0.35 of its width and 1.2 tube heights deep;
      // the centreline at the tube's centre, so the canyon hangs below it.
      const f64 tube = h / 2.2;
      if (s >= 0.0) return Vec2{static_cast<f32>(a * c), static_cast<f32>(tube * 0.5 * s)};
      const f64 x = a * 0.35 * sgn(c) * std::sqrt(std::fabs(c));
      const f64 y = -tube * 1.2 * std::sqrt(-s);
      return Vec2{static_cast<f32>(x), static_cast<f32>(y)};
    }
    case PassageProfile::Phreatic:
    default: return Vec2{static_cast<f32>(a * c), static_cast<f32>(h * 0.5 * s)};
  }
}

f32 stalactite_radius(f32 along, f32 length, f32 base, f32 tip) noexcept {
  if (!(length > 0.0f) || along <= 0.0f) return tip;
  const f64 t = std::min<f64>(1.0, static_cast<f64>(along) / static_cast<f64>(length));
  const f64 q = std::sqrt(t);
  const f64 three_quarters = q * std::sqrt(q);  // t^(3/4)
  return static_cast<f32>(static_cast<f64>(tip) +
                          (static_cast<f64>(base) - static_cast<f64>(tip)) * three_quarters);
}

namespace {

// A body of revolution about a vertical axis through `foot`, `levels` rings from `y0` to `y1`
// (either order), radius `radius(i)` at ring i, closed at both ends by a point; wound outward.
template <class Radius>
void revolve(assets::MeshData& m, D3 foot, f64 y0, f64 y1, u32 levels, u32 sides, Radius radius,
             f64 v_scale) {
  const u32 base = m.positions.size();
  // The two poles.
  m.positions.push_back(v3(foot.x, foot.y + y0, foot.z));
  m.normals.push_back(Vec3{0.0f, y0 < y1 ? -1.0f : 1.0f, 0.0f});
  m.uvs.push_back(Vec2{0.0f, 0.0f});
  for (u32 i = 0; i < levels; ++i) {
    const f64 t = levels == 1 ? 0.5 : static_cast<f64>(i) / static_cast<f64>(levels - 1);
    const f64 y = y0 + (y1 - y0) * t;
    const f64 r = radius(i);
    for (u32 k = 0; k < sides; ++k) {
      f64 c = 0.0, s = 0.0;
      unit_circle(k, sides, c, s);
      m.positions.push_back(v3(foot.x + r * c, foot.y + y, foot.z + r * s));
      m.normals.push_back(v3(c, 0.0, s));
      m.uvs.push_back(
          Vec2{static_cast<f32>(static_cast<f64>(k) / sides), static_cast<f32>(y * v_scale)});
    }
  }
  m.positions.push_back(v3(foot.x, foot.y + y1, foot.z));
  m.normals.push_back(Vec3{0.0f, y1 > y0 ? 1.0f : -1.0f, 0.0f});
  m.uvs.push_back(Vec2{0.0f, 1.0f});
  const u32 first = base + 1;
  const u32 last_pole = base + 1 + levels * sides;
  // Outward: seen from outside, counter-clockwise. With y rising from y0 to y1 and the ring
  // counter-clockwise seen from +y (x towards z is clockwise seen from +y), the quads' winding
  // flips with the direction of y.
  const bool up = y1 > y0;
  const auto tri = [&](u32 a, u32 b, u32 c) {
    if (up) {
      m.indices.push_back(a);
      m.indices.push_back(c);
      m.indices.push_back(b);
    } else {
      m.indices.push_back(a);
      m.indices.push_back(b);
      m.indices.push_back(c);
    }
  };
  for (u32 k = 0; k < sides; ++k) {
    const u32 k1 = (k + 1) % sides;
    tri(base, first + k1, first + k);
    for (u32 i = 0; i + 1 < levels; ++i) {
      const u32 a = first + i * sides + k;
      const u32 b = first + i * sides + k1;
      const u32 c = first + (i + 1) * sides + k;
      const u32 d = first + (i + 1) * sides + k1;
      tri(a, b, d);
      tri(a, d, c);
    }
    const u32 top = first + (levels - 1) * sides;
    tri(last_pole, top + k, top + k1);
  }
}

}  // namespace

bool solve_passage(const Passage& passage, assets::MeshData& out, PassageReport* report_out,
                   std::string* error) {
  out = assets::MeshData{};
  PassageReport report;
  std::string why;
  if (!passage_in_range(passage, why)) {
    if (error != nullptr) *error = why;
    return false;
  }
  const u32 n = passage.ring_vertices;
  const f64 radius = static_cast<f64>(passage.entrance_radius_m);
  const f64 spacing = static_cast<f64>(passage.ring_spacing_m);
  // The centreline: the entrance, the shaft's foot, then the points.
  Vector<D3> control;
  control.push_back(D3{0.0, 0.0, 0.0});
  control.push_back(D3{0.0, -static_cast<f64>(passage.shaft_m), 0.0});
  for (const Vec3& p : passage.points)
    control.push_back(d3(p));
  const u32 spans = control.size() - 1;
  const auto segment_of = [&](u32 span) -> PassageSegment {
    // Span 0 is the shaft: round, the entrance's radius. Span k >= 1 is segment k - 1.
    if (span == 0) return PassageSegment{};
    if (passage.segments.empty()) return PassageSegment{};
    return passage.segments[std::min<u32>(span - 1, passage.segments.size() - 1)];
  };
  // The passage's first horizontal direction: what "up" leans to in the shaft (see below).
  D3 heading = control.size() > 2 ? control[2] - control[1] : D3{1.0, 0.0, 0.0};
  heading.y = 0.0;
  heading = length(heading) > 0.0 ? normalized(heading) : D3{1.0, 0.0, 0.0};
  // Samples by arc length: a fine walk of the spline, rings every `spacing` metres.
  constexpr u32 k_walk = 64;
  struct Sample {
    D3 p;
    f64 t = 0.0;
  };
  Vector<Sample> walk;
  for (u32 i = 0; i <= spans * k_walk; ++i) {
    const f64 t = static_cast<f64>(i) / k_walk;
    walk.push_back(Sample{spline(control, t), t});
  }
  // The shaft is straight: the spline's first span, between two equal-x points with the next
  // point's pull, bends; its rings are placed on the straight line instead (the collar round and
  // level, the shaft vertical) down to its foot, and the spline after.
  Vector<Sample> rings;
  {
    const f64 shaft = static_cast<f64>(passage.shaft_m);
    const u32 shaft_rings = std::max<u32>(1u, static_cast<u32>(std::ceil(shaft / spacing)));
    for (u32 r = 0; r < shaft_rings; ++r) {
      const f64 d = shaft * static_cast<f64>(r) / static_cast<f64>(shaft_rings);
      rings.push_back(Sample{D3{0.0, -d, 0.0}, d / std::max(shaft, 1e-9)});
    }
    f64 since = spacing;
    for (u32 i = k_walk; i < walk.size(); ++i) {
      since += length(walk[i].p - walk[i - 1].p);
      report.length_m += static_cast<f32>(length(walk[i].p - walk[i - 1].p));
      if (since >= spacing || i + 1 == walk.size()) {
        rings.push_back(walk[i]);
        since = 0.0;
      }
    }
    report.length_m += static_cast<f32>(shaft);
  }
  const u32 ring_count = rings.size();
  if (ring_count < 3) {
    if (error != nullptr) *error = "engine.caves.Passage is shorter than three rings";
    return false;
  }
  // Each ring's frame: its tangent, and the profile's up — world up off the tangent, leaning to
  // the passage's heading where the tangent is near vertical (the shaft, the bend out of it), so
  // the frame turns with the bend and never twists.
  const D3 world_up{0.0, 1.0, 0.0};
  Vector<D3> tangents(ring_count), ups(ring_count), sides(ring_count);
  for (u32 r = 0; r < ring_count; ++r) {
    D3 t;
    if (r == 0) {
      t = D3{0.0, -1.0, 0.0};
    } else if (r + 1 < ring_count) {
      t = normalized(rings[r + 1].p - rings[r - 1].p);
    } else {
      t = normalized(rings[r].p - rings[r - 1].p);
    }
    if (rings[r].p.y > -static_cast<f64>(passage.shaft_m) + 1e-9 && r > 0 && rings[r].t <= 1.0)
      t = D3{0.0, -1.0, 0.0};
    tangents[r] = t;
    const f64 vertical = std::fabs(dot(t, world_up));
    const f64 w = std::clamp((vertical - 0.7) / 0.3, 0.0, 1.0);
    const D3 lean = world_up + heading * w;
    D3 u = lean - t * dot(t, lean);
    u = length(u) > 1e-9 ? normalized(u) : heading;
    ups[r] = u;
    sides[r] = normalized(cross(u, t));  // x across, so (side, up) is counter-clockwise seen down t
  }
  // The profile at each ring: the shaft round; a segment's own; blended over a ring's spacing
  // either side of a change, and from the shaft's circle into the first segment over its first
  // two metres.
  const f64 shaft = static_cast<f64>(passage.shaft_m);
  const f64 rough = static_cast<f64>(passage.roughness);
  Vector<D3> ring_centre(ring_count);
  const u32 first = 0;
  out.positions.reserve(static_cast<usize>(ring_count) * (n + 1) + 2);
  f64 collar_fade = 2.0;
  for (const f32 h : passage.collar_heights_m)
    collar_fade = std::max(collar_fade, 2.0 * std::fabs(static_cast<f64>(h)));
  f64 along = 0.0;
  for (u32 r = 0; r < ring_count; ++r) {
    if (r > 0) along += length(rings[r].p - rings[r - 1].p);
    const bool in_shaft = r == 0 || (rings[r].t <= 1.0 && rings[r].p.y > -shaft + 1e-9);
    const u32 span = in_shaft ? 0u : std::min<u32>(static_cast<u32>(rings[r].t), spans - 1);
    const PassageSegment seg = segment_of(span == 0 ? 1u : span);
    const f32 sh = segment_height(seg);
    // How far into the passage proper: the shaft's circle blends into the first profile over 2 m.
    const f64 past = in_shaft ? 0.0 : std::min(1.0, (along - shaft) / 2.0);
    // The noise fades in from the collar over the shaft's first metre, so the collar is exact.
    const f64 depth = std::min(1.0, along / 1.0);
    D3 centre = rings[r].p;
    ring_centre[r] = centre;
    for (u32 k = 0; k <= n; ++k) {
      const u32 kk = k % n;
      f64 c = 0.0, s = 0.0;
      unit_circle(kk, n, c, s);
      D3 p;
      if (in_shaft) {
        // A level circle of the entrance's radius in the ring's frame (side and up lie level in
        // the shaft), so the passage's first rings continue it vertex for vertex.
        p = centre + sides[r] * (radius * c) + ups[r] * (radius * s);
        // The collar's heights, faded out down the shaft over `fade` metres — at least twice the
        // largest, so every vertex still falls as it goes down and the wall never folds.
        if (!passage.collar_heights_m.empty() && along < collar_fade)
          p.y += static_cast<f64>(passage.collar_heights_m[kk]) * (1.0 - along / collar_fade);
      } else {
        const Vec2 q = profile_point(seg.profile, seg.width_m, sh, kk, n);
        // The circle in the ring's own frame, for the blend out of the shaft: same angle index.
        const f64 cx = radius * c, cy = radius * s;
        const f64 x = cx + (static_cast<f64>(q.x) - cx) * past;
        const f64 y = cy + (static_cast<f64>(q.y) - cy) * past;
        p = centre + sides[r] * x + ups[r] * y;
      }
      if (rough > 0.0 && depth > 0.0) {
        const f64 bump = 1.0 + rough * depth * wall_noise(passage.seed, static_cast<f64>(r), kk, n);
        p = centre + (p - centre) * bump;
      }
      out.positions.push_back(v3(p.x, p.y, p.z));
      out.uvs.push_back(Vec2{0.0f, static_cast<f32>(along)});
    }
  }
  // UVs: metres round the ring (cumulative chord) and along the passage.
  for (u32 r = 0; r < ring_count; ++r) {
    f64 round = 0.0;
    for (u32 k = 1; k <= n; ++k) {
      round += length(d3(out.positions[r * (n + 1) + k]) - d3(out.positions[r * (n + 1) + k - 1]));
      out.uvs[r * (n + 1) + k].x = static_cast<f32>(round);
    }
  }
  // The shell's quads, wound to face the axis: seen from inside, counter-clockwise.
  const auto at = [&](u32 r, u32 k) { return first + r * (n + 1) + k; };
  for (u32 r = 0; r + 1 < ring_count; ++r) {
    for (u32 k = 0; k < n; ++k) {
      const u32 a = at(r, k), b = at(r, k + 1), c = at(r + 1, k), d = at(r + 1, k + 1);
      out.indices.push_back(a);
      out.indices.push_back(b);
      out.indices.push_back(d);
      out.indices.push_back(a);
      out.indices.push_back(d);
      out.indices.push_back(c);
    }
  }
  // The far end: a rounded cap, its pole a third of the last width beyond the last ring.
  {
    const u32 last = ring_count - 1;
    const PassageSegment seg = segment_of(spans);
    const D3 pole = ring_centre[last] + tangents[last] * (static_cast<f64>(seg.width_m) / 3.0);
    const u32 p = out.positions.size();
    out.positions.push_back(v3(pole.x, pole.y, pole.z));
    out.uvs.push_back(Vec2{0.0f, static_cast<f32>(along + static_cast<f64>(seg.width_m) / 3.0)});
    for (u32 k = 0; k < n; ++k) {
      out.indices.push_back(at(last, k));
      out.indices.push_back(at(last, k + 1));
      out.indices.push_back(p);
    }
  }
  // Check the winding faces the axis: a quad's normal against the direction to its ring's centre.
  {
    const D3 a = d3(out.positions[at(2, 0)]), b = d3(out.positions[at(2, 1)]),
             d = d3(out.positions[at(3, 1)]);
    const D3 normal = cross(b - a, d - a);
    const D3 inward = ring_centre[2] - a;
    if (dot(normal, inward) < 0.0) {
      for (u32 i = 0; i < out.indices.size(); i += 3)
        std::swap(out.indices[i + 1], out.indices[i + 2]);
    }
  }
  report.rings = ring_count;
  report.ring_vertices = n;
  report.shell_triangles = out.indices.size() / 3;
  report.collar_first = at(0, 0);
  report.collar_count = n + 1;
  const u32 shell_vertices = out.positions.size();

  // ---- speleothems ------------------------------------------------------------------------------
  if (passage.speleothems.has_value()) {
    const Speleothems& sp = *passage.speleothems;
    const u32 sides_n = sp.sides;
    const u32 per_body = sides_n * 2 * 6;  // up to six levels a body
    for (u32 r = 2; r + 1 < ring_count; ++r) {
      if (rings[r].t <= 1.0 && rings[r].p.y > -shaft + 1e-9) continue;  // the shaft has no ceiling
      // The ceiling: the ring's vertices whose profile points up more than 60 degrees, and the
      // floor: its lowest vertex.
      u32 lo = 0, ceiling_count = 0;
      f64 low_y = 1e300;
      for (u32 k = 0; k < n; ++k) {
        const D3 p = d3(out.positions[at(r, k)]);
        if (p.y < low_y) {
          low_y = p.y;
          lo = k;
        }
        f64 c = 0.0, s = 0.0;
        unit_circle(k, n, c, s);
        ceiling_count += s > 0.86 ? 1u : 0u;
      }
      const PassageSegment here = segment_of(std::max<u32>(1u, static_cast<u32>(rings[r].t)));
      const f64 ceiling_width =
          std::max(0.5, static_cast<f64>(ceiling_count) / static_cast<f64>(n) * k_pi *
                            static_cast<f64>(here.width_m));
      const f64 expected =
          static_cast<f64>(sp.drip_per_m2) * ceiling_width * length(rings[r + 1].p - rings[r].p);
      const u32 count = static_cast<u32>(expected + unit_hash(passage.seed, 0x44524950ull, r, 0));
      for (u32 d = 0; d < count; ++d) {
        ++report.drips;
        if (report.speleothem_triangles + 2 * per_body > sp.triangle_budget) {
          ++report.drips_over_budget;
          continue;
        }
        // A drip point between two ceiling vertices of this ring and the next.
        const f64 h0 = unit_hash(passage.seed, 0x5354414Cull, r, d);
        const f64 h1 = unit_hash(passage.seed, 0x414C4F4Eull, r, d);
        const f64 h2 = unit_hash(passage.seed, 0x4C454E47ull, r, d);
        const f64 h3 = unit_hash(passage.seed, 0x4D495445ull, r, d);
        u32 k = n / 4;  // the top
        const u32 spread = std::max<u32>(1u, ceiling_count / 2);
        k = (k + n - spread + static_cast<u32>(h0 * (2.0 * spread))) % n;
        const D3 c0 = d3(out.positions[at(r, k)]);
        const D3 c1 = d3(out.positions[at(r + 1, k)]);
        const D3 top = c0 + (c1 - c0) * h1;
        const f64 floor_y =
            std::min(d3(out.positions[at(r, lo)]).y, d3(out.positions[at(r + 1, lo)]).y);
        const f64 gap = top.y - floor_y;
        if (gap < 0.3) continue;
        const D3 foot{top.x, floor_y, top.z};
        const f64 embed = 0.08;  // into the rock above, so no seam shows at the ceiling
        if (h2 < static_cast<f64>(sp.straw_share)) {
          const f64 len = std::min(gap * 0.5, 0.02 + h3 * static_cast<f64>(sp.straw_max_m));
          const f64 rs = static_cast<f64>(sp.straw_radius_m);
          revolve(
              out, foot, gap + embed, gap - len, 2, std::max<u32>(4u, sides_n / 2),
              [&](u32) { return rs; }, 1.0);
          ++report.straws;
          continue;
        }
        f64 len = static_cast<f64>(sp.stalactite_min_m) +
                  h3 * static_cast<f64>(sp.stalactite_max_m - sp.stalactite_min_m);
        len = std::min(len, gap * 0.6);
        const f64 base_r = static_cast<f64>(sp.slenderness) * len;
        const f64 tip = static_cast<f64>(sp.straw_radius_m);
        f64 mite =
            static_cast<f64>(sp.stalagmite_max_m) * unit_hash(passage.seed, 0x4D4954ull, r, d);
        const f64 mite_r = static_cast<f64>(sp.stalagmite_radius_m) *
                           (1.0 + unit_hash(passage.seed, 0x524144ull, r, d));
        const bool has_mite = unit_hash(passage.seed, 0x484153ull, r, d) < 0.7;
        bool column = false;
        if (has_mite) {
          mite = std::min(mite, gap - len);
          if (gap - len - mite < static_cast<f64>(sp.column_gap_m)) {
            // A column: the two meet halfway across the gap that was left.
            column = true;
            const f64 meet = (gap - len - mite) * 0.5;
            len += meet + 0.01;
            mite += meet + 0.01;
          }
        }
        constexpr u32 k_levels = 6;
        const f32 tip_r = column ? static_cast<f32>(mite_r) : static_cast<f32>(tip);
        revolve(
            out, foot, gap + embed, gap - len, k_levels, sides_n,
            [&](u32 i) {
              // Ring i from the ceiling (i = 0) to the tip: its distance from the tip.
              const f64 from_tip = len * (1.0 - static_cast<f64>(i) / (k_levels - 1));
              return static_cast<f64>(stalactite_radius(static_cast<f32>(from_tip),
                                                        static_cast<f32>(len),
                                                        static_cast<f32>(base_r), tip_r));
            },
            1.0);
        ++report.stalactites;
        if (has_mite && mite > 0.02) {
          // A column of constant radius with a rounded top (Dreybrodt's equilibrium stalagmite),
          // sunk into the floor.
          revolve(
              out, foot, -embed, mite, k_levels, sides_n,
              [&](u32 i) {
                const f64 t = static_cast<f64>(i) / (k_levels - 1);
                const f64 cap =
                    t > 0.75
                        ? std::sqrt(std::max(0.0, 1.0 - ((t - 0.75) / 0.25) * ((t - 0.75) / 0.25)))
                        : 1.0;
                return mite_r * std::max(0.25, cap);
              },
              1.0);
          ++report.stalagmites;
        }
        report.columns += column ? 1u : 0u;
        report.speleothem_triangles = (out.indices.size() / 3) - report.shell_triangles;
      }
    }
    report.speleothem_triangles = (out.indices.size() / 3) - report.shell_triangles;
  }

  // Normals: the shell's from its faces (the seam's two copies summed), the speleothems' as built.
  out.normals.resize(out.positions.size(), Vec3{0.0f, 0.0f, 0.0f});
  {
    Vector<D3> acc(shell_vertices);
    for (u32 i = 0; i < report.shell_triangles * 3; i += 3) {
      const D3 a = d3(out.positions[out.indices[i]]), b = d3(out.positions[out.indices[i + 1]]),
               c = d3(out.positions[out.indices[i + 2]]);
      const D3 nrm = cross(b - a, c - a);
      for (u32 e = 0; e < 3; ++e)
        acc[out.indices[i + e]] = acc[out.indices[i + e]] + nrm;
    }
    for (u32 r = 0; r < ring_count; ++r) {
      const D3 seam = acc[at(r, 0)] + acc[at(r, n)];
      acc[at(r, 0)] = seam;
      acc[at(r, n)] = seam;
    }
    for (u32 v = 0; v < shell_vertices; ++v) {
      const D3 nn = normalized(acc[v]);
      out.normals[v] = v3(nn.x, nn.y, nn.z);
    }
  }
  assets::Material material;
  material.name = "limestone";
  const Vec3 color = passage.base_color;
  material.base_color = color.x > 0.0f || color.y > 0.0f || color.z > 0.0f
                            ? Vec4{color.x, color.y, color.z, 1.0f}
                            : Vec4{0.55f, 0.50f, 0.42f, 1.0f};
  material.metallic = 0.0f;
  material.roughness = passage.roughness_factor;
  out.materials.push_back(material);
  assets::Primitive part;
  part.first_index = 0;
  part.index_count = out.indices.size();
  part.material = 0;
  out.primitives.push_back(part);
  if (report_out != nullptr) *report_out = report;
  return true;
}

}  // namespace engine::caves
