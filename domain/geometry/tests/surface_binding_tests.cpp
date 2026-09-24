// The surface binding (docs/subsystems/geometry.md, "Limit surfaces and surface bindings"): the
// 10-byte record and its decoders, the three footpoint-normal modes and their degeneracy rules, the
// nearest-point binder and the authored-record binder and what they report, the image-orientation
// check, and the displacement transfer's contract — the base bitwise where the weight is zero, a
// rigid motion of the cage carried rigidly at weight one, a seam two modules share and neither owns
// left alone whichever runs first, linearity in the node state when the offsets are zero, and the
// base itself at the reference state — bit for bit, on every compiler, under every mode, which is
// why the transfer is written in the displacement form (surface_binding.h).
#include "limit_fixtures.h"

#include <domain/geometry/cluster.h>
#include <domain/geometry/limit_surface.h>
#include <domain/geometry/surface_binding.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

constexpr f32 k_a = 0.070f;  // the neutral fixture's footprint semi-axes, metres
constexpr f32 k_b = 0.086f;
constexpr f32 k_height = 0.080f;

constexpr NormalMode k_modes[] = {NormalMode::triangle,
                                  NormalMode::interpolated_vertex_area_weighted,
                                  NormalMode::limit_interpolated};

u32 index_of(NormalMode mode) { return static_cast<u32>(mode); }

// For MESSAGE and CAPTURE, which print a bare const char* as a pointer.
std::string name(NormalMode mode) { return normal_mode_name(mode); }

// A region: the fixture's 121-node dome cage at `centre`, its level-3 limit operator with the
// tangent operators, and the refined surface at the reference state under each mode.
struct Region {
  Vec3 centre{};
  Vector<u32> cage_faces;
  Vector<Vec3> nodes;
  LoopLimitSurface surface;
  BindingFrame rest[3];

  std::span<const u32> faces() const { return surface.faces; }
  const Vector<Vec3>& reference() const { return rest[0].reference; }
  BindingSurface at_rest(NormalMode mode) const { return rest[index_of(mode)].view(faces()); }
};

BindingFrame frame(const Region& r, NormalMode mode, std::span<const Vec3> state_nodes) {
  BindingFrame out;
  std::string error;
  const bool ok = evaluate_binding_frame(mode, r.surface, r.nodes, state_nodes, out, &error);
  INFO(error);
  REQUIRE(ok);
  return out;
}

void refresh(Region& r) {
  for (const NormalMode mode : k_modes) {
    r.rest[index_of(mode)] = frame(r, mode, r.nodes);
    REQUIRE(r.rest[index_of(mode)].invalid_normals.empty());
  }
}

Region make_region(Vec3 centre) {
  Region r;
  r.centre = centre;
  r.cage_faces = fixture::ringed_disk_faces(5, 24);
  r.nodes = fixture::ringed_disk_dome(5, 24, k_a, k_b, k_height);
  for (Vec3& p : r.nodes)
    p = p + centre;
  LoopSurfaceOptions options;
  options.level = 3;
  options.tangents = true;
  std::string error;
  const bool ok = build_loop_limit_surface(r.cage_faces, 121, options, r.surface, &error);
  INFO(error);
  REQUIRE(ok);
  refresh(r);
  return r;
}

struct D3 {
  f64 x, y, z;
};
D3 d3(Vec3 v) { return D3{static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)}; }

struct Placed {
  u32 triangle;
  f64 b0, b1, b2;
  f64 h;
};

// Points placed at a known footpoint and offset along `mode`'s footpoint normal: a random triangle
// of the refined surface whose corners all lie within normalized radius `max_radius` of the
// region's centre, barycentrics at least 0.15 each, and a normal offset in [h_lo, h_hi].
void place(const Region& r, NormalMode mode, fixture::Random& random, u32 count, f32 max_radius,
           f32 h_lo, f32 h_hi, Vector<Vec3>& points, Vector<Placed>* placed = nullptr) {
  const Vector<Vec3>& reference = r.reference();
  Vector<u32> candidates;
  for (u32 t = 0; t < r.surface.triangle_count(); ++t) {
    bool inside = true;
    for (u32 c = 0; c < 3; ++c) {
      const Vec3 p = reference[r.surface.faces[3 * t + c]] - r.centre;
      const f32 radius = std::sqrt((p.x / k_a) * (p.x / k_a) + (p.y / k_b) * (p.y / k_b));
      inside = inside && radius < max_radius;
    }
    if (inside) candidates.push_back(t);
  }
  REQUIRE(!candidates.empty());
  const BindingSurface surface = r.at_rest(mode);
  for (u32 i = 0; i < count; ++i) {
    const u32 t = candidates[static_cast<u32>(random.next() % candidates.size())];
    const f64 b0 = static_cast<f64>(random.uniform(0.15f, 0.7f));
    const f64 b1 = static_cast<f64>(random.uniform(0.15f, 0.85f - static_cast<f32>(b0)));
    const f64 b2 = 1.0 - b0 - b1;
    const D3 a = d3(reference[r.surface.faces[3 * t]]);
    const D3 b = d3(reference[r.surface.faces[3 * t + 1]]);
    const D3 c = d3(reference[r.surface.faces[3 * t + 2]]);
    const Vec3 nf = footpoint_normal(
        mode, surface, t, Vec3{static_cast<f32>(b0), static_cast<f32>(b1), static_cast<f32>(b2)});
    const D3 n = d3(nf);
    const f64 h = static_cast<f64>(random.uniform(h_lo, h_hi));
    points.push_back(Vec3{static_cast<f32>(b0 * a.x + b1 * b.x + b2 * c.x + h * n.x),
                          static_cast<f32>(b0 * a.y + b1 * b.y + b2 * c.y + h * n.y),
                          static_cast<f32>(b0 * a.z + b1 * b.z + b2 * c.z + h * n.z)});
    if (placed != nullptr) placed->push_back(Placed{t, b0, b1, b2, h});
  }
}

Vector<SurfaceBinding> bind(const Region& r, NormalMode mode, std::span<const Vec3> points,
                            std::span<const f32> weights = {}, SurfaceBindReport* report = nullptr,
                            f32 offset_limit = std::numeric_limits<f32>::infinity()) {
  SurfaceBindOptions options;
  options.weights = weights;
  options.offset_limit = offset_limit;
  Vector<SurfaceBinding> out;
  std::string error;
  const bool ok = bind_to_surface(mode, points, r.at_rest(mode), options, out, report, &error);
  INFO(error);
  REQUIRE(ok);
  REQUIRE(validate_surface_bindings(out, r.surface.triangle_count()));
  return out;
}

// A rigid motion in double: a rotation of `degrees` about `axis`, then a translation.
struct Rigid {
  f64 m[3][3];
  f64 t[3];
  Rigid(Vec3 axis, f64 degrees, Vec3 translation) {
    const Vec3 k = normalize(axis);
    const f64 x = static_cast<f64>(k.x);
    const f64 y = static_cast<f64>(k.y);
    const f64 z = static_cast<f64>(k.z);
    const f64 a = degrees * std::numbers::pi / 180.0;
    const f64 c = std::cos(a);
    const f64 s = std::sin(a);
    const f64 v = 1.0 - c;
    const f64 rotation[3][3] = {{c + x * x * v, x * y * v - z * s, x * z * v + y * s},
                                {y * x * v + z * s, c + y * y * v, y * z * v - x * s},
                                {z * x * v - y * s, z * y * v + x * s, c + z * z * v}};
    std::memcpy(m, rotation, sizeof(m));
    t[0] = static_cast<f64>(translation.x);
    t[1] = static_cast<f64>(translation.y);
    t[2] = static_cast<f64>(translation.z);
  }
  D3 operator()(Vec3 p) const {
    const D3 q = d3(p);
    return D3{m[0][0] * q.x + m[0][1] * q.y + m[0][2] * q.z + t[0],
              m[1][0] * q.x + m[1][1] * q.y + m[1][2] * q.z + t[1],
              m[2][0] * q.x + m[2][1] * q.y + m[2][2] * q.z + t[2]};
  }
  Vec3 to_f32(Vec3 p) const {
    const D3 q = (*this)(p);
    return Vec3{static_cast<f32>(q.x), static_cast<f32>(q.y), static_cast<f32>(q.z)};
  }
};

f64 distance(Vec3 a, D3 b) {
  const f64 dx = static_cast<f64>(a.x) - b.x;
  const f64 dy = static_cast<f64>(a.y) - b.y;
  const f64 dz = static_cast<f64>(a.z) - b.z;
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

f64 gap(Vec3 a, Vec3 b) { return distance(a, d3(b)); }

f64 angle_deg(Vec3 a, Vec3 b) {
  const D3 x = d3(a);
  const D3 y = d3(b);
  const f64 c =
      (x.x * y.x + x.y * y.y + x.z * y.z) /
      std::sqrt((x.x * x.x + x.y * x.y + x.z * x.z) * (y.x * y.x + y.y * y.y + y.z * y.z));
  return std::acos(std::clamp(c, -1.0, 1.0)) * 180.0 / std::numbers::pi;
}

// The cage deformed in a way nothing is symmetric about: every node pushed by up to `amount`.
Vector<Vec3> deformed(const Vector<Vec3>& nodes, u64 seed, f32 amount) {
  fixture::Random random(seed);
  Vector<Vec3> out = nodes;
  for (Vec3& p : out)
    p = p + Vec3{random.uniform(-amount, amount), random.uniform(-amount, amount),
                 random.uniform(-amount, amount)};
  return out;
}

// A smooth state like the fixture's: `at_centre` at the centre, fading to nothing at the fixed rim.
Vector<Vec3> smooth_state(const Region& r, Vec3 at_centre) {
  Vector<Vec3> out = r.nodes;
  for (Vec3& p : out) {
    const Vec3 q = p - r.centre;
    const f32 rr = (q.x / k_a) * (q.x / k_a) + (q.y / k_b) * (q.y / k_b);
    p = p + at_centre * std::max(0.0f, 1.0f - rr);
  }
  return out;
}

Vector<Vec3> transfer(const Region& r, NormalMode mode, std::span<const SurfaceBinding> bindings,
                      std::span<const Vec3> base, std::span<const Vec3> state_nodes,
                      BindingTerms terms = BindingTerms::full) {
  const BindingFrame f = frame(r, mode, state_nodes);
  Vector<Vec3> out(static_cast<u32>(base.size()));
  apply_binding(mode, bindings, base, f.view(r.faces()), std::span<Vec3>(out.data(), out.size()),
                terms);
  return out;
}

bool same_bits(Vec3 a, Vec3 b) { return std::memcmp(&a, &b, sizeof(Vec3)) == 0; }

u32 changed(const Vector<Vec3>& a, const Vector<Vec3>& b) {
  u32 count = 0;
  for (u32 i = 0; i < a.size(); ++i)
    count += same_bits(a[i], b[i]) ? 0u : 1u;
  return count;
}

// A small hand-built surface, for the degeneracy rules that a limit surface cannot be made to show
// on demand: a 5 x 5 sheet at z = 0, every refined vertex's reference normal +z.
struct Sheet {
  Vector<u32> faces = fixture::sheet_faces(5, 5);
  Vector<Vec3> reference;
  Vector<Vec3> displacement;
  Vector<Vec3> normal_reference;
  Vector<Vec3> normal_change;
  Sheet() {
    for (u32 j = 0; j < 5; ++j)
      for (u32 i = 0; i < 5; ++i)
        reference.push_back(Vec3{0.01f * static_cast<f32>(i), 0.01f * static_cast<f32>(j), 0.0f});
    displacement.assign(25, Vec3{});
    normal_reference.assign(25, Vec3{0.0f, 0.0f, 1.0f});
    normal_change.assign(25, Vec3{});
  }
  BindingSurface view() const {
    return BindingSurface{faces, reference, displacement, normal_reference, normal_change};
  }
  // The area-weighted normal field for the current displacement.
  void area_weighted() {
    Vector<Vec3> vectors(25);
    Vector<Vec3> change(25);
    Vector<Vec3> state(25);
    Vector<Vec3> state_vectors(25);
    area_weighted_normal_vectors(faces, reference, std::span<Vec3>(vectors.data(), 25));
    area_weighted_normal_vector_change(faces, reference, displacement,
                                       std::span<Vec3>(change.data(), 25));
    for (u32 i = 0; i < 25; ++i)
      state[i] = reference[i] + displacement[i];
    area_weighted_normal_vectors(faces, state, std::span<Vec3>(state_vectors.data(), 25));
    REQUIRE(area_weighted_reference_normals(vectors,
                                            std::span<Vec3>(normal_reference.data(), 25)) == 0);
    area_weighted_normal_changes(vectors, change, state_vectors,
                                 std::span<Vec3>(normal_change.data(), 25));
  }
};

SurfaceBinding record_on(u32 triangle, f32 b0, f32 b1, f32 offset, u16 weight = 65535) {
  SurfaceBinding b;
  b.triangle = static_cast<u16>(triangle);
  b.barycentric[0] = static_cast<u16>(std::lround(static_cast<f64>(b0) * 65535.0));
  b.barycentric[1] = static_cast<u16>(std::lround(static_cast<f64>(b1) * 65535.0));
  b.normal_offset = f32_to_f16(offset);
  b.weight = weight;
  return b;
}

}  // namespace

TEST_CASE("binding: the record is ten bytes and its decoders are exact where they must be") {
  CHECK(sizeof(SurfaceBinding) == 10);
  CHECK(alignof(SurfaceBinding) == 2);
  CHECK(surface_binding_can_represent(65536));
  CHECK_FALSE(surface_binding_can_represent(65537));

  SurfaceBinding b;
  b.weight = 0;
  CHECK(binding_weight(b) == 0.0f);
  b.weight = 65535;
  CHECK(binding_weight(b) == 1.0f);
  b.barycentric[0] = 65535;
  b.barycentric[1] = 0;
  CHECK(binding_barycentrics(b) == Vec3{1.0f, 0.0f, 0.0f});
  b.barycentric[0] = 0;
  b.barycentric[1] = 65535;
  CHECK(binding_barycentrics(b) == Vec3{0.0f, 1.0f, 0.0f});
  b.barycentric[1] = 0;
  CHECK(binding_barycentrics(b) == Vec3{0.0f, 0.0f, 1.0f});

  // The reference conversion itself: the smallest denormal is 2^-24, the largest 1023 * 2^-24, and
  // every finite half survives a round trip through f32.
  CHECK(f16_to_f32(0x0001) == 0x1.0p-24f);
  CHECK(f16_to_f32(0x03ff) == 1023.0f * 0x1.0p-24f);
  CHECK(f16_to_f32(0x0400) == 0x1.0p-14f);
  u32 round_trip_failures = 0;
  for (u32 bits = 0; bits <= 0xffffu; ++bits) {
    if ((bits & 0x7c00u) == 0x7c00u) continue;
    round_trip_failures += f32_to_f16(f16_to_f32(static_cast<u16>(bits))) == bits ? 0u : 1u;
  }
  CHECK(round_trip_failures == 0);

  // The transfer's branch-free half decoder against the reference conversion, every half there is.
  u32 mismatched = 0;
  for (u32 bits = 0; bits <= 0xffffu; ++bits) {
    b.normal_offset = static_cast<u16>(bits);
    const f32 fast = binding_normal_offset(b);
    const f32 exact = f16_to_f32(static_cast<u16>(bits));
    if ((bits & 0x7c00u) == 0x7c00u) {
      if ((bits & 0x3ffu) == 0) {
        mismatched += std::isinf(fast) && std::signbit(fast) == std::signbit(exact) ? 0u : 1u;
      } else {
        mismatched += std::isnan(fast) ? 0u : 1u;
      }
    } else {
      mismatched += std::memcmp(&fast, &exact, 4) == 0 ? 0u : 1u;
    }
  }
  CHECK(mismatched == 0);
}

TEST_CASE("binding: the three normal modes have one spelling each and nothing else parses") {
  CHECK(std::string(normal_mode_name(NormalMode::triangle)) == "triangle");
  CHECK(std::string(normal_mode_name(NormalMode::interpolated_vertex_area_weighted)) ==
        "interpolated-vertex-area-weighted");
  CHECK(std::string(normal_mode_name(NormalMode::limit_interpolated)) == "limit-interpolated");
  for (const NormalMode mode : k_modes) {
    NormalMode parsed = NormalMode::triangle;
    CHECK(parse_normal_mode(normal_mode_name(mode), parsed));
    CHECK(parsed == mode);
  }
  NormalMode parsed = NormalMode::triangle;
  CHECK_FALSE(parse_normal_mode("limit_interpolated", parsed));
  CHECK_FALSE(parse_normal_mode("Limit-Interpolated", parsed));
  CHECK_FALSE(parse_normal_mode("area-weighted", parsed));
  CHECK_FALSE(parse_normal_mode("", parsed));
}

TEST_CASE("binding: a unit normal's change is a product with the change, zero where it is zero") {
  fixture::Random random(40);
  u32 not_zero = 0;
  f64 worst = 0.0;
  for (u32 i = 0; i < 2000; ++i) {
    const Vec3 c{random.uniform(-1.0f, 1.0f), random.uniform(-1.0f, 1.0f),
                 random.uniform(-1.0f, 1.0f)};
    // No change, with either zero: exactly zero, bit for bit.
    const Vec3 zero_plus = unit_normal_change(c, Vec3{0.0f, 0.0f, 0.0f}, 0.0f);
    const Vec3 zero_minus = unit_normal_change(c, Vec3{-0.0f, -0.0f, -0.0f}, 0.0f);
    not_zero += zero_plus == Vec3{} && zero_minus == Vec3{} ? 0u : 1u;
    // A change: the difference of the two normals in double, to f32 rounding.
    const Vec3 dc{random.uniform(-0.3f, 0.3f), random.uniform(-0.3f, 0.3f),
                  random.uniform(-0.3f, 0.3f)};
    const Vec3 got = unit_normal_change(c, dc, 0.0f);
    const D3 a = d3(c);
    const D3 b{a.x + static_cast<f64>(dc.x), a.y + static_cast<f64>(dc.y),
               a.z + static_cast<f64>(dc.z)};
    const f64 la = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    const f64 lb = std::sqrt(b.x * b.x + b.y * b.y + b.z * b.z);
    if (lb < 0.2 * la) continue;  // near a collapse the expansion's relative accuracy falls
    const D3 want{b.x / lb - a.x / la, b.y / lb - a.y / la, b.z / lb - a.z / la};
    worst = std::max(worst, distance(got, want));
  }
  CHECK(not_zero == 0);
  CHECK(worst <= 2.0e-6);

  // A degenerate reference has no normal and no change; a state at or under the threshold keeps
  // the reference; one just over it turns.
  CHECK(unit_normal_change(Vec3{}, Vec3{1.0f, 0.0f, 0.0f}, 0.0f) == Vec3{});
  const Vec3 c{0.0f, 0.0f, 2.0f};
  CHECK(unit_normal_change(c, Vec3{0.0f, 0.0f, -2.0f}, 0.0f) == Vec3{});  // collapsed exactly
  const f32 floor = 1.0e-6f * 1.0e-6f * dot(c, c);
  CHECK(unit_normal_change(c, Vec3{1.0e-7f, 0.0f, -2.0f}, floor) == Vec3{});
  CHECK_FALSE(unit_normal_change(c, Vec3{1.0e-3f, 0.0f, -2.0f}, floor) == Vec3{});
  CHECK(unit_normal(Vec3{}) == Vec3{});
  CHECK(unit_normal(Vec3{std::numeric_limits<f32>::infinity(), 0.0f, 0.0f}) == Vec3{});
}

TEST_CASE("binding: bind_to_surface finds the footpoint and offset a vertex was placed at") {
  const Region r = make_region(Vec3{});
  for (const NormalMode mode : k_modes) {
    CAPTURE(name(mode));
    fixture::Random random(21);
    Vector<Vec3> points;
    Vector<Placed> placed;
    // Outside the convex top of the dome, where a point's nearest triangle is the one it was placed
    // over (for the interpolated modes, whose normal is not the facet's, the nearest point moves a
    // little from the footpoint a vertex was placed at, and the check below allows for it).
    place(r, mode, random, 400, 0.5f, 0.0f, 0.003f, points, &placed);
    SurfaceBindReport report;
    const Vector<SurfaceBinding> bindings = bind(r, mode, points, {}, &report);
    REQUIRE(bindings.size() == points.size());
    f64 largest_h = 0.0;
    f64 worst_position = 0.0;
    for (u32 i = 0; i < points.size(); ++i) {
      const SurfaceBinding& b = bindings[i];
      largest_h = std::max(largest_h, std::fabs(placed[i].h));
      CHECK(b.weight == 65535);
      // The record decodes to the vertex, within the tangential residual it cannot carry.
      const Vec3 w = binding_barycentrics(b);
      const u32* corner = r.surface.faces.data() + 3 * u32{b.triangle};
      const Vec3 q = r.reference()[corner[0]] * w.x + r.reference()[corner[1]] * w.y +
                     r.reference()[corner[2]] * w.z;
      const Vec3 n = footpoint_normal(mode, r.at_rest(mode), b.triangle, w);
      worst_position = std::max(worst_position, gap(q + n * binding_normal_offset(b), points[i]));
    }
    MESSAGE(name(mode) << ": 400 vertices up to " << largest_h * 1000.0
                       << " mm off the surface decode to within " << worst_position * 1.0e6
                       << " um (record quantization " << report.max_quantization_error * 1.0e6f
                       << " um, tangential residual " << report.max_tangential_residual * 1.0e6f
                       << " um)");
    CHECK(worst_position <= static_cast<f64>(report.max_quantization_error) +
                                static_cast<f64>(report.max_tangential_residual) + 1.0e-7);
    CHECK(report.boundary_footpoints.empty());
    CHECK(report.offset_exceeded.empty());
    CHECK(report.invalid_normals.empty());
    CHECK(static_cast<f64>(report.max_abs_offset) <= largest_h + 1.0e-7);
    if (mode == NormalMode::triangle) {
      // The facet normal: the nearest point is the placed footpoint, exactly as the first binder.
      u32 wrong_triangle = 0;
      for (u32 i = 0; i < points.size(); ++i)
        wrong_triangle += bindings[i].triangle == placed[i].triangle ? 0u : 1u;
      CHECK(wrong_triangle == 0);
      CHECK(report.max_tangential_residual <= 1.0e-7f);
    }
  }
}

TEST_CASE("binding: a vertex of weight zero is the base, bit for bit") {
  const Region r = make_region(Vec3{});
  for (const NormalMode mode : k_modes) {
    CAPTURE(name(mode));
    fixture::Random random(22);
    Vector<Vec3> points;
    place(r, mode, random, 300, 0.95f, -0.004f, 0.004f, points);
    // Every other vertex has weight 0, the rest a weight in (0, 1].
    Vector<f32> weights;
    for (u32 i = 0; i < points.size(); ++i)
      weights.push_back(i % 2 == 0 ? 0.0f : random.uniform(0.05f, 1.0f));
    const Vector<SurfaceBinding> bindings = bind(r, mode, points, weights);
    const Vector<Vec3> state = deformed(r.nodes, 5, 0.01f);
    const Vector<Vec3> out = transfer(r, mode, bindings, points, state);
    u32 kept = 0;
    u32 moved = 0;
    for (u32 i = 0; i < points.size(); ++i) {
      if (i % 2 == 0) kept += same_bits(out[i], points[i]) ? 1u : 0u;
      if (i % 2 == 1) moved += same_bits(out[i], points[i]) ? 0u : 1u;
    }
    CHECK(kept == 150);
    CHECK(moved == 150);

    // All zero: the whole output is the base.
    const Vector<f32> none(static_cast<u32>(points.size()), 0.0f);
    const Vector<SurfaceBinding> unbound = bind(r, mode, points, none);
    const Vector<Vec3> still = transfer(r, mode, unbound, points, state);
    CHECK(std::memcmp(still.data(), points.data(), points.size() * sizeof(Vec3)) == 0);
  }
}

TEST_CASE("binding: at weight one a rigid motion of the cage moves the bound vertices rigidly") {
  const Region r = make_region(Vec3{0.1f, -0.05f, 0.2f});
  const Rigid motion(Vec3{1.0f, 2.0f, 3.0f}, 40.0, Vec3{0.2f, -0.1f, 0.3f});
  Vector<Vec3> state;
  for (const Vec3& p : r.nodes)
    state.push_back(motion.to_f32(p));
  for (const NormalMode mode : k_modes) {
    CAPTURE(name(mode));
    fixture::Random random(23);
    Vector<Vec3> on_surface;
    place(r, mode, random, 200, 0.5f, 0.0f, 0.0f, on_surface);
    Vector<Vec3> offset;
    place(r, mode, random, 200, 0.5f, 0.0005f, 0.003f, offset);
    for (const Vector<Vec3>* points : {&on_surface, &offset}) {
      SurfaceBindReport report;
      const Vector<SurfaceBinding> bindings = bind(r, mode, *points, {}, &report);
      const Vector<Vec3> out = transfer(r, mode, bindings, *points, state);
      f64 worst = 0.0;
      for (u32 i = 0; i < points->size(); ++i)
        worst = std::max(worst, distance(out[i], motion((*points)[i])));
      // Off rigid by (R - I) times the record's own reconstruction error, and f32 arithmetic.
      const f64 bound = 2.0 * static_cast<f64>(report.max_quantization_error) +
                        2.0 * static_cast<f64>(report.max_tangential_residual) + 2.0e-6;
      MESSAGE(name(mode) << std::string(points == &on_surface ? ", on the surface"
                                                              : ", 0.5 to 3 mm off it")
                         << ": a 40 degree turn and a 37 cm move carried to within "
                         << worst * 1.0e6 << " um of rigid");
      CHECK(worst <= bound);
    }
  }
}

TEST_CASE("binding: a seam that belongs to neither module stays the base whichever runs first") {
  // Two modules, a left and a right region, over one render mesh laid out as
  // [left-only | seam | right-only]. Each module's span covers its own vertices and the seam, and
  // gives the seam weight 0: a bilateral region is two modules with a seam neither writes.
  const Region left = make_region(Vec3{-0.09f, 0.0f, 0.0f});
  const Region right = make_region(Vec3{0.09f, 0.0f, 0.0f});
  for (const NormalMode mode : k_modes) {
    CAPTURE(name(mode));
    fixture::Random random(24);
    Vector<Vec3> base;
    place(left, mode, random, 120, 0.9f, -0.003f, 0.003f, base);
    const u32 left_count = base.size();
    for (u32 i = 0; i < 17; ++i)  // the seam: the midline between the two, at the rims' height
      base.push_back(
          Vec3{0.0f, -0.08f + 0.01f * static_cast<f32>(i), random.uniform(0.0f, 0.004f)});
    const u32 seam_count = base.size() - left_count;
    place(right, mode, random, 120, 0.9f, -0.003f, 0.003f, base);
    const u32 right_count = base.size() - left_count - seam_count;

    const std::span<const Vec3> all(base.data(), base.size());
    const std::span<const Vec3> left_span = all.subspan(0, left_count + seam_count);
    const std::span<const Vec3> right_span = all.subspan(left_count, seam_count + right_count);
    Vector<f32> left_weights;
    for (u32 i = 0; i < left_span.size(); ++i)
      left_weights.push_back(i < left_count ? random.uniform(0.2f, 1.0f) : 0.0f);
    Vector<f32> right_weights;
    for (u32 i = 0; i < right_span.size(); ++i)
      right_weights.push_back(i < seam_count ? 0.0f : random.uniform(0.2f, 1.0f));
    const Vector<SurfaceBinding> left_bindings = bind(left, mode, left_span, left_weights);
    const Vector<SurfaceBinding> right_bindings = bind(right, mode, right_span, right_weights);

    const BindingFrame left_state = frame(left, mode, deformed(left.nodes, 31, 0.012f));
    const BindingFrame right_state = frame(right, mode, deformed(right.nodes, 32, 0.012f));
    const auto run = [&](bool left_first) {
      Vector<Vec3> out = base;
      const std::span<Vec3> whole(out.data(), out.size());
      for (u32 pass = 0; pass < 2; ++pass) {
        if ((pass == 0) == left_first) {
          // In place: a module reads and writes its own span of the shared output.
          apply_binding(mode, left_bindings, whole.subspan(0, left_count + seam_count),
                        left_state.view(left.faces()), whole.subspan(0, left_count + seam_count));
        } else {
          apply_binding(mode, right_bindings, whole.subspan(left_count, seam_count + right_count),
                        right_state.view(right.faces()),
                        whole.subspan(left_count, seam_count + right_count));
        }
      }
      return out;
    };
    const Vector<Vec3> a = run(true);
    const Vector<Vec3> b = run(false);
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(Vec3)) == 0);
    u32 seam_kept = 0;
    for (u32 i = left_count; i < left_count + seam_count; ++i)
      seam_kept += same_bits(a[i], base[i]) ? 1u : 0u;
    CHECK(seam_kept == seam_count);
    CHECK(changed(a, base) == left_count + right_count);
  }
}

TEST_CASE("binding: with zero offsets the transfer is linear in the node state") {
  const Region r = make_region(Vec3{});
  for (const NormalMode mode : k_modes) {
    CAPTURE(name(mode));
    fixture::Random random(25);
    Vector<Vec3> on_surface;
    place(r, NormalMode::triangle, random, 300, 0.95f, 0.0f, 0.0f, on_surface);
    const Vector<SurfaceBinding> flat = bind(r, mode, on_surface);
    Vector<Vec3> offset;
    place(r, mode, random, 300, 0.95f, 0.002f, 0.002f, offset);
    const Vector<SurfaceBinding> lifted = bind(r, mode, offset);

    // The midpoint of two states against the average of their two transfers.
    const auto residual = [&](const Vector<SurfaceBinding>& bindings, const Vector<Vec3>& base,
                              const Vector<Vec3>& state_a, const Vector<Vec3>& state_b) {
      Vector<Vec3> middle;
      for (u32 i = 0; i < state_a.size(); ++i)
        middle.push_back((state_a[i] + state_b[i]) * 0.5f);
      const Vector<Vec3> a = transfer(r, mode, bindings, base, state_a);
      const Vector<Vec3> b = transfer(r, mode, bindings, base, state_b);
      const Vector<Vec3> m = transfer(r, mode, bindings, base, middle);
      Vector<f64> errors;
      for (u32 i = 0; i < base.size(); ++i) {
        const D3 average{(static_cast<f64>(a[i].x) + static_cast<f64>(b[i].x)) * 0.5,
                         (static_cast<f64>(a[i].y) + static_cast<f64>(b[i].y)) * 0.5,
                         (static_cast<f64>(a[i].z) + static_cast<f64>(b[i].z)) * 0.5};
        errors.push_back(distance(m[i], average));
      }
      std::sort(errors.begin(), errors.end());
      return std::pair{errors[errors.size() * 95 / 100], errors.back()};
    };

    // Linearity holds for any states once the offsets are gone, so the hardest: every node pushed
    // at random by up to 15 mm. The records carry the f32 placement's residue as an offset in the
    // interpolated modes (their normal is not the facet's), so they are rebuilt with none at all.
    Vector<SurfaceBinding> zero_offsets = flat;
    for (SurfaceBinding& b : zero_offsets)
      b.normal_offset = 0;
    const Vector<Vec3> rough_a = deformed(r.nodes, 41, 0.015f);
    const Vector<Vec3> rough_b = deformed(r.nodes, 42, 0.015f);
    const auto [flat_p95, flat_max] = residual(zero_offsets, on_surface, rough_a, rough_b);
    CHECK(flat_max <= 1.0e-6);

    // The offset term's nonlinearity is second order in how far the normals turn, so it is
    // measured on smooth states like the fixture's: a sag of 30 mm down and 10 mm in at the centre,
    // fading to nothing at the fixed rim, against a 15 mm flattening.
    const auto [lifted_p95, lifted_max] =
        residual(lifted, offset, smooth_state(r, Vec3{0.0f, -0.03f, -0.01f}),
                 smooth_state(r, Vec3{0.0f, 0.0f, -0.015f}));
    MESSAGE(name(mode)
            << ": midpoint state against the average of two transfers: zero offsets, nodes pushed "
               "at random by up to 15 mm, p95 "
            << flat_p95 * 1.0e6 << " um, max " << flat_max * 1.0e6
            << " um; 2 mm offsets on smooth sag states p95 " << lifted_p95 * 1.0e6 << " um, max "
            << lifted_max * 1.0e6 << " um");
    CHECK(lifted_max <= 1.0e-3);
  }
}

// The contract the displacement form exists for (surface_binding.h, "Why the displacement form"):
// with the nodes at the paired reference every output is its base, bit for bit, under any weight
// in (0, 1], any offset and every normal mode, on every compiler. The first transfer computed two
// footpoint normals and subtracted them; GCC 13 at x86-64-v3 fused one normal's last multiply into
// the subtraction, and the check failed there with 299 of 300. So this one is built to see any
// change at all: besides the placed vertices (some with a -0 coordinate, which x + 0 would turn
// into +0) it moves a base of signed zeros, denormals and tiny values, on which a change of 1e-40
// would show, and it runs on the fixture and on a copy turned 73 degrees about a skew axis, so that
// no normal has a zero component to spare it rounding. Then, from the same reference, a rigid
// motion at weight one carries the vertices rigidly: the transfer is not simply the identity. Run
// it on `linux-gcc-release` after any change to the transfer (geometry.md says how).
TEST_CASE("binding: the base bit for bit at the reference, and rigid under a motion of the cage") {
  const Rigid pose(Vec3{-2.0f, 1.0f, 0.5f}, 73.0, Vec3{0.31f, 1.12f, -0.47f});
  const Rigid motion(Vec3{0.3f, -1.0f, 2.0f}, 35.0, Vec3{-0.15f, 0.2f, 0.05f});
  const Region dome = make_region(Vec3{});
  Region turned = make_region(Vec3{});
  for (Vec3& p : turned.nodes)
    p = pose.to_f32(p);
  refresh(turned);

  for (const NormalMode mode : k_modes) {
    CAPTURE(name(mode));
    fixture::Random random(30);
    // For the reference: the whole dome, inside it and out, and some vertices on the midline with
    // -0 as a mirrored mesh writes it.
    Vector<Vec3> placed;
    place(dome, mode, random, 2000, 0.95f, -0.004f, 0.004f, placed);
    for (u32 i = 0; i < placed.size(); i += 97)
      placed[i].x = -0.0f;
    Vector<f32> weights;
    for (u32 i = 0; i < placed.size(); ++i)
      weights.push_back(i % 7 == 0 ? 1.0f : random.uniform(1.0e-4f, 1.0f));
    // Values on which any nonzero step, however small, changes the bits.
    const f32 detectors[] = {0.0f, -0.0f, 0x1.0p-149f, -0x1.0p-140f, 1.0e-30f, -3.0e-38f};
    Vector<Vec3> sensitive;
    for (u32 i = 0; i < placed.size(); ++i)
      sensitive.push_back(Vec3{detectors[i % 6], detectors[(i + 2) % 6], detectors[(i + 4) % 6]});
    // For the motion: outside the convex top, where a vertex's footpoint is the triangle it was
    // placed over and the record reconstructs it.
    Vector<Vec3> outside;
    place(dome, mode, random, 500, 0.5f, 0.0005f, 0.004f, outside);

    const Region* const regions[] = {&dome, &turned};
    for (const Region* r : regions) {
      const auto posed = [&](const Vector<Vec3>& points) {
        Vector<Vec3> out;
        for (const Vec3& p : points)
          out.push_back(r == &dome ? p : pose.to_f32(p));
        return out;
      };
      const Vector<Vec3> base = posed(placed);
      const Vector<SurfaceBinding> bindings = bind(*r, mode, base, weights);
      u32 offset_bindings = 0;
      for (const SurfaceBinding& b : bindings)
        offset_bindings += binding_normal_offset(b) != 0.0f && b.weight != 0 ? 1u : 0u;
      CHECK(offset_bindings == bindings.size());  // nothing escapes through a zero weight or offset

      // The displacement and every normal change of an unmoved cage are zero, exactly.
      const BindingFrame still = frame(*r, mode, r->nodes);
      u32 nonzero = 0;
      for (const Vec3& d : still.displacement)
        nonzero += d == Vec3{} ? 0u : 1u;
      for (const Vec3& d : still.normal_change)
        nonzero += d == Vec3{} ? 0u : 1u;
      CHECK(nonzero == 0);

      CHECK(changed(transfer(*r, mode, bindings, base, r->nodes), base) == 0);
      CHECK(changed(transfer(*r, mode, bindings, sensitive, r->nodes), sensitive) == 0);
      Vector<Vec3> in_place = sensitive;  // `out` may be the base itself
      apply_binding(mode, bindings, in_place, still.view(r->faces()),
                    std::span<Vec3>(in_place.data(), in_place.size()));
      CHECK(changed(in_place, sensitive) == 0);

      // From the same reference, the cage moved rigidly: at weight one the vertices follow it.
      const Vector<Vec3> over = posed(outside);
      SurfaceBindReport report;
      const Vector<SurfaceBinding> whole = bind(*r, mode, over, {}, &report);
      Vector<Vec3> state;
      for (const Vec3& p : r->nodes)
        state.push_back(motion.to_f32(p));
      const Vector<Vec3> carried = transfer(*r, mode, whole, over, state);
      f64 worst = 0.0;
      for (u32 i = 0; i < over.size(); ++i)
        worst = std::max(worst, distance(carried[i], motion(over[i])));
      const f64 bound = 2.0 * static_cast<f64>(report.max_quantization_error) +
                        2.0 * static_cast<f64>(report.max_tangential_residual) + 2.0e-6;
      MESSAGE(name(mode)
              << std::string(r == &dome ? ", the dome" : ", the turned copy")
              << ": 2000 vertices up to 4 mm off, the base bit for bit at the reference; a 35 "
                 "degree turn of the cage carries 500 more to within "
              << worst * 1.0e6 << " um of rigid");
      CHECK(worst <= bound);
    }
  }
}

TEST_CASE("binding: a collapsed footpoint triangle, with valid neighbours") {
  // One interior triangle of the sheet collapses to a point 2 mm above its centroid while every
  // other vertex stays: its facet has no normal. The authoring side's small fixture for the same
  // case: the facet normal is zero while the smooth footpoint normal is still +z, because the
  // corners' other triangles still contribute.
  Sheet sheet;
  const u32 t = 2 * (1 * 4 + 1);  // the first triangle of quad (1, 1): an interior triangle
  const u32 corners[3] = {sheet.faces[3 * t], sheet.faces[3 * t + 1], sheet.faces[3 * t + 2]};
  const Vec3 centroid =
      (sheet.reference[corners[0]] + sheet.reference[corners[1]] + sheet.reference[corners[2]]) *
      (1.0f / 3.0f);
  for (const u32 c : corners)
    sheet.displacement[c] = centroid + Vec3{0.0f, 0.0f, 0.002f} - sheet.reference[c];
  sheet.area_weighted();

  const Vec3 base_position{0.013f, 0.012f, 0.003f};
  const SurfaceBinding records[] = {record_on(t, 0.3f, 0.3f, 0.003f)};
  const Vec3 base[] = {base_position};
  const auto run = [&](NormalMode mode, BindingTerms terms) {
    Vec3 out{};
    apply_binding(mode, records, base, sheet.view(), std::span<Vec3>(&out, 1), terms);
    return out;
  };

  // The facet: collapsed, so in `triangle` mode the offset is carried along the reference normal
  // and the transfer is the displacement alone.
  const Vec3 state_facet =
      cross(sheet.reference[corners[1]] + sheet.displacement[corners[1]] -
                (sheet.reference[corners[0]] + sheet.displacement[corners[0]]),
            sheet.reference[corners[2]] + sheet.displacement[corners[2]] -
                (sheet.reference[corners[0]] + sheet.displacement[corners[0]]));
  CHECK(state_facet == Vec3{});
  CHECK(run(NormalMode::triangle, BindingTerms::full) ==
        run(NormalMode::triangle, BindingTerms::displacement));

  // The corners' area-weighted normals are still defined, so the interpolated normal turns: the
  // transfer matches a double-precision evaluation of the state's normals.
  const Vec3 area_full = run(NormalMode::interpolated_vertex_area_weighted, BindingTerms::full);
  const Vec3 area_moved =
      run(NormalMode::interpolated_vertex_area_weighted, BindingTerms::displacement);
  CHECK_FALSE(area_full == area_moved);
  Vector<D3> state_normals(25, D3{0.0, 0.0, 0.0});
  for (u32 f = 0; f < sheet.faces.size(); f += 3) {
    D3 p[3];
    for (u32 k = 0; k < 3; ++k)
      p[k] = d3(sheet.reference[sheet.faces[f + k]] + sheet.displacement[sheet.faces[f + k]]);
    const D3 e1{p[1].x - p[0].x, p[1].y - p[0].y, p[1].z - p[0].z};
    const D3 e2{p[2].x - p[0].x, p[2].y - p[0].y, p[2].z - p[0].z};
    const D3 c{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
    for (u32 k = 0; k < 3; ++k) {
      D3& n = state_normals[sheet.faces[f + k]];
      n = D3{n.x + c.x, n.y + c.y, n.z + c.z};
    }
  }
  const Vec3 w = binding_barycentrics(records[0]);
  D3 m{0.0, 0.0, 0.0};
  const f64 bw[3] = {static_cast<f64>(w.x), static_cast<f64>(w.y), static_cast<f64>(w.z)};
  for (u32 k = 0; k < 3; ++k) {
    const D3 n = state_normals[corners[k]];
    const f64 length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    m = D3{m.x + bw[k] * n.x / length, m.y + bw[k] * n.y / length, m.z + bw[k] * n.z / length};
  }
  const f64 ml = std::sqrt(m.x * m.x + m.y * m.y + m.z * m.z);
  const f64 h = static_cast<f64>(binding_normal_offset(records[0]));
  D3 moved{0.0, 0.0, 0.0};
  for (u32 k = 0; k < 3; ++k) {
    const D3 d = d3(sheet.displacement[corners[k]]);
    moved = D3{moved.x + bw[k] * d.x, moved.y + bw[k] * d.y, moved.z + bw[k] * d.z};
  }
  const D3 want{static_cast<f64>(base_position.x) + moved.x + h * (m.x / ml - 0.0),
                static_cast<f64>(base_position.y) + moved.y + h * (m.y / ml - 0.0),
                static_cast<f64>(base_position.z) + moved.z + h * (m.z / ml - 1.0)};
  MESSAGE(
      "a collapsed footpoint triangle: triangle mode keeps the reference normal; the "
      "area-weighted footpoint normal still turns, "
      << angle_deg(Vec3{static_cast<f32>(m.x), static_cast<f32>(m.y), static_cast<f32>(m.z)},
                   Vec3{0.0f, 0.0f, 1.0f})
      << " degrees, and the transfer is within " << distance(area_full, want) * 1.0e9
      << " nm of double precision");
  CHECK(distance(area_full, want) <= 1.0e-8);
}

TEST_CASE("binding: a vertex whose tangents collapse keeps its reference limit normal") {
  // The limit rule's vertex stage on tangents given directly: four vertices, each tangent frame
  // tilted, and at the state one whose v tangent turns parallel to u, one whose u tangent shrinks
  // to nothing, one whose tangents are merely rotated, and one left alone.
  const Vec3 tu[] = {Vec3{1.0f, 0.0f, 0.1f}, Vec3{0.5f, 0.1f, 0.0f}, Vec3{0.02f, 0.0f, 0.0f},
                     Vec3{3.0f, 0.2f, 0.1f}};
  const Vec3 tv[] = {Vec3{0.0f, 1.0f, 0.0f}, Vec3{0.0f, 0.4f, 0.2f}, Vec3{0.0f, 0.03f, 0.001f},
                     Vec3{-0.1f, 2.0f, 0.3f}};
  Vec3 normals[4];
  Vector<u32> invalid;
  CHECK(limit_reference_normals(tu, tv, normals, &invalid) == 0);
  for (const Vec3& n : normals)
    CHECK(std::fabs(length(n) - 1.0f) <= 1.0e-6f);
  // Orientation is the winding's: u x v, never flipped.
  CHECK(dot(normals[0], cross(tu[0], tv[0])) > 0.0f);

  const Vec3 dtu[] = {Vec3{}, Vec3{-0.5f, -0.1f, 0.0f}, Vec3{0.0f, 0.0f, 0.02f}, Vec3{}};
  const Vec3 dtv[] = {tu[0] * 0.5f - tv[0], Vec3{}, Vec3{0.0f, -0.03f, 0.03f}, Vec3{}};
  Vec3 change[4];
  limit_normal_changes(tu, tv, dtu, dtv, change);
  CHECK(change[0] == Vec3{});  // v turned parallel to u: the sine is zero, the reference is kept
  CHECK(change[1] == Vec3{});  // u fell to zero length: the reference is kept
  CHECK(change[3] == Vec3{});  // no change at all: exactly zero
  // The rotated frame: its change is the difference of the two limit normals, to f32 rounding.
  const Vec3 state_normal = normalize(cross(normalize(tu[2] + dtu[2]), normalize(tv[2] + dtv[2])));
  CHECK(gap(normals[2] + change[2], state_normal) <= 1.0e-6);
  CHECK(length(change[2]) > 0.1f);

  // At the reference, a tangent pair that is degenerate is invalid, not invented: zero, listed.
  const Vec3 bad_u[] = {Vec3{}, Vec3{1.0f, 0.0f, 0.0f},
                        Vec3{std::numeric_limits<f32>::quiet_NaN(), 0.0f, 0.0f},
                        Vec3{1.0f, 0.0f, 0.0f}};
  const Vec3 bad_v[] = {Vec3{0.0f, 1.0f, 0.0f}, Vec3{2.0f, 1.0e-7f, 0.0f}, Vec3{0.0f, 1.0f, 0.0f},
                        Vec3{0.0f, 1.0f, 0.0f}};
  invalid.clear();
  CHECK(limit_reference_normals(bad_u, bad_v, normals, &invalid) == 3);
  REQUIRE(invalid.size() == 3);
  CHECK(invalid[0] == 0);  // a zero tangent
  CHECK(invalid[1] == 1);  // parallel within the sine threshold (5e-8)
  CHECK(invalid[2] == 2);  // not finite
  CHECK(normals[0] == Vec3{});
  CHECK(normals[3] == Vec3{0.0f, 0.0f, 1.0f});
}

TEST_CASE("binding: an interpolation that cancels keeps the reference footpoint normal") {
  // A footpoint on the midpoint of an edge whose two vertex normals turn to face each other: at the
  // state the interpolation of the corners' normals has no length, and the offset is carried along
  // the reference footpoint normal rather than dropped.
  Sheet sheet;
  const u32 t = 2 * (1 * 4 + 1);
  const u32 c0 = sheet.faces[3 * t];
  const u32 c1 = sheet.faces[3 * t + 1];
  const u32 c2 = sheet.faces[3 * t + 2];
  for (u32 k = 0; k < 25; ++k)
    sheet.displacement[k] = Vec3{0.0f, 0.0f, 0.001f};
  // The corners' state normals chosen so that the record's own weights sum them to nothing: three
  // unit vectors in the xy plane closing a triangle of sides b0, b1, b2.
  const SurfaceBinding cancelling[] = {record_on(t, 0.4f, 0.3f, 0.004f)};
  const Vec3 w = binding_barycentrics(cancelling[0]);
  const f64 b0 = static_cast<f64>(w.x);
  const f64 b1 = static_cast<f64>(w.y);
  const f64 b2 = static_cast<f64>(w.z);
  const f64 theta = std::acos((b2 * b2 - b0 * b0 - b1 * b1) / (2.0 * b0 * b1));
  const D3 n0{1.0, 0.0, 0.0};
  const D3 n1{std::cos(theta), std::sin(theta), 0.0};
  const D3 n2{(-b0 * n0.x - b1 * n1.x) / b2, (-b0 * n0.y - b1 * n1.y) / b2, 0.0};
  const auto to_change = [](D3 n) {
    return Vec3{static_cast<f32>(n.x), static_cast<f32>(n.y), static_cast<f32>(n.z) - 1.0f};
  };
  sheet.normal_change[c0] = to_change(n0);
  sheet.normal_change[c1] = to_change(n1);
  sheet.normal_change[c2] = to_change(n2);
  const Vec3 base[] = {Vec3{0.1f, 0.2f, 0.3f}};
  const auto run = [&](std::span<const SurfaceBinding> records, BindingTerms terms) {
    Vec3 out{};
    apply_binding(NormalMode::limit_interpolated, records, base, sheet.view(),
                  std::span<Vec3>(&out, 1), terms);
    return out;
  };
  CHECK(run(cancelling, BindingTerms::full) == run(cancelling, BindingTerms::displacement));
  // Elsewhere in the triangle the interpolation has length and turns.
  const SurfaceBinding turning[] = {record_on(t, 0.6f, 0.2f, 0.004f)};
  CHECK_FALSE(run(turning, BindingTerms::full) == run(turning, BindingTerms::displacement));

  // At the reference a cancelling interpolation is invalid: no normal is invented for it.
  sheet.normal_reference[c0] = Vec3{1.0f, 0.0f, 0.0f};
  sheet.normal_reference[c1] = Vec3{-1.0f, 0.0f, 0.0f};
  CHECK(footpoint_normal(NormalMode::limit_interpolated, sheet.view(), t, Vec3{0.5f, 0.5f, 0.0f}) ==
        Vec3{});
  CHECK_FALSE(footpoint_normal(NormalMode::limit_interpolated, sheet.view(), t,
                               Vec3{0.6f, 0.4f, 0.0f}) == Vec3{});
  // And a corner whose own normal is invalid invalidates a footpoint that uses it.
  sheet.normal_reference[c0] = Vec3{};
  CHECK(footpoint_normal(NormalMode::limit_interpolated, sheet.view(), t, Vec3{0.2f, 0.4f, 0.4f}) ==
        Vec3{});
  CHECK_FALSE(footpoint_normal(NormalMode::limit_interpolated, sheet.view(), t,
                               Vec3{0.0f, 0.5f, 0.5f}) == Vec3{});
}

TEST_CASE("binding: a uniformly scaled cage binds and moves the same way") {
  // Every threshold is dimensionless, so a region scaled by a power of two — which scales every
  // float exactly — binds to the same footpoints, bit for bit, records its offsets scaled by the
  // same factor, and moves exactly the scaled amount. The offsets are kept between 0.5 and 3 mm so
  // that at both scales they are normal half floats: below 6.1e-5 of the positions' unit a half is
  // denormal and rounds to an absolute step, a property of the record's packing and not of the
  // normal (and a reason the offset should be stored in a unit near the region's size).
  const Region unit = make_region(Vec3{});
  for (const f32 factor : {1024.0f, 0.25f}) {
    CAPTURE(factor);
    Region scaled = make_region(Vec3{});
    for (Vec3& p : scaled.nodes)
      p = p * factor;
    refresh(scaled);
    for (const NormalMode mode : k_modes) {
      CAPTURE(name(mode));
      fixture::Random random(50);
      Vector<Vec3> points;
      place(unit, mode, random, 300, 0.5f, 0.0005f, 0.003f, points);
      Vector<Vec3> scaled_points;
      for (const Vec3& p : points)
        scaled_points.push_back(p * factor);
      const Vector<SurfaceBinding> a = bind(unit, mode, points);
      const Vector<SurfaceBinding> b = bind(scaled, mode, scaled_points);
      u32 same = 0;
      for (u32 i = 0; i < a.size(); ++i)
        same += a[i].triangle == b[i].triangle && a[i].barycentric[0] == b[i].barycentric[0] &&
                        a[i].barycentric[1] == b[i].barycentric[1] &&
                        binding_normal_offset(b[i]) == binding_normal_offset(a[i]) * factor
                    ? 1u
                    : 0u;
      CHECK(same == a.size());
      const Vector<Vec3> state = smooth_state(unit, Vec3{0.0f, -0.02f, -0.01f});
      Vector<Vec3> scaled_state;
      for (const Vec3& p : state)
        scaled_state.push_back(p * factor);
      const Vector<Vec3> moved = transfer(unit, mode, a, points, state);
      const Vector<Vec3> scaled_moved = transfer(scaled, mode, b, scaled_points, scaled_state);
      u32 exact = 0;
      for (u32 i = 0; i < moved.size(); ++i)
        exact += scaled_moved[i] == moved[i] * factor ? 1u : 0u;
      CHECK(exact == moved.size());
    }
  }
}

TEST_CASE("binding: the same authored footpoints and offsets under the three modes") {
  // The conformance comparison the round-five closure asks for: one record set — footpoints and
  // offsets fixed, as an authoring tool would have written them — transferred under each normal
  // mode. The records are identical; only the normal the offsets are carried along differs, so the
  // differences below are the modes' and nothing else's (on study019 the authoring side measured
  // 5.65 degrees and 0.163 mm between triangle and area weighted).
  const Region r = make_region(Vec3{});
  fixture::Random random(60);
  Vector<Vec3> points;
  place(r, NormalMode::interpolated_vertex_area_weighted, random, 600, 0.9f, 0.001f, 0.012f,
        points);
  const Vector<SurfaceBinding> authored =
      bind(r, NormalMode::interpolated_vertex_area_weighted, points);
  Vector<AuthoredFootpoint> footpoints;
  for (const SurfaceBinding& b : authored)
    footpoints.push_back(AuthoredFootpoint{b.triangle, binding_barycentrics(b),
                                           binding_normal_offset(b), binding_weight(b)});
  Vector<SurfaceBinding> by_mode[3];
  for (const NormalMode mode : k_modes) {
    std::string error;
    REQUIRE(bind_from_records(mode, points, footpoints, r.at_rest(mode), SurfaceBindOptions{},
                              by_mode[index_of(mode)], nullptr, &error));
    CHECK(std::memcmp(by_mode[index_of(mode)].data(), authored.data(),
                      authored.size() * sizeof(SurfaceBinding)) == 0);
  }
  const Vector<Vec3> state = smooth_state(r, Vec3{0.0f, 0.012f, 0.013f});
  Vector<Vec3> moved[3];
  for (const NormalMode mode : k_modes)
    moved[index_of(mode)] = transfer(r, mode, authored, points, state);
  const auto compare = [&](NormalMode x, NormalMode y) {
    f64 worst_angle = 0.0;
    f64 worst_position = 0.0;
    for (u32 i = 0; i < authored.size(); ++i) {
      const Vec3 w = binding_barycentrics(authored[i]);
      worst_angle = std::max(worst_angle,
                             angle_deg(footpoint_normal(x, r.at_rest(x), authored[i].triangle, w),
                                       footpoint_normal(y, r.at_rest(y), authored[i].triangle, w)));
      worst_position = std::max(worst_position, gap(moved[index_of(x)][i], moved[index_of(y)][i]));
    }
    MESSAGE(name(x) << " against " << name(y) << ": reference footpoint normals up to "
                    << worst_angle << " degrees apart, transferred positions up to "
                    << worst_position * 1.0e3 << " mm");
    return std::pair{worst_angle, worst_position};
  };
  const auto [tri_area_angle, tri_area_position] =
      compare(NormalMode::triangle, NormalMode::interpolated_vertex_area_weighted);
  const auto [limit_area_angle, limit_area_position] =
      compare(NormalMode::limit_interpolated, NormalMode::interpolated_vertex_area_weighted);
  const auto [limit_tri_angle, limit_tri_position] =
      compare(NormalMode::limit_interpolated, NormalMode::triangle);
  // On a smooth dome at level 3 the dense area-weighted normal is a close estimate of the limit
  // normal and the facet normal a coarser one; neither difference is zero, which is the point.
  CHECK(limit_area_angle > 0.0);
  CHECK(limit_area_angle < tri_area_angle);
  CHECK(tri_area_angle < 10.0);
  CHECK(limit_tri_angle < 10.0);
  CHECK(tri_area_position < 1.0e-3);
  CHECK(limit_area_position < tri_area_position);
  CHECK(limit_tri_position < 1.0e-3);
}

TEST_CASE("binding: bind_from_records keeps the authored footpoints and says how far they are") {
  const Region r = make_region(Vec3{});
  for (const NormalMode mode : k_modes) {
    CAPTURE(name(mode));
    fixture::Random random(70);
    Vector<Vec3> points;
    place(r, mode, random, 400, 0.9f, -0.004f, 0.004f, points);
    Vector<f32> weights;
    for (u32 i = 0; i < points.size(); ++i)
      weights.push_back(i % 5 == 0 ? 0.0f : random.uniform(0.1f, 1.0f));
    SurfaceBindReport nearest_report;
    const Vector<SurfaceBinding> nearest = bind(r, mode, points, weights, &nearest_report);

    // The nearest binder's own footpoints, fed back without offsets: the same records, the offsets
    // measured again along the mode's normal from footpoints rounded to 1/65535, so within a unit
    // of the half float's last place.
    Vector<AuthoredFootpoint> footpoints;
    for (u32 i = 0; i < nearest.size(); ++i)
      footpoints.push_back(AuthoredFootpoint{nearest[i].triangle, binding_barycentrics(nearest[i]),
                                             std::numeric_limits<f32>::quiet_NaN(), weights[i]});
    SurfaceBindReport report;
    Vector<SurfaceBinding> packed;
    std::string error;
    REQUIRE(bind_from_records(mode, points, footpoints, r.at_rest(mode), SurfaceBindOptions{},
                              packed, &report, &error));
    REQUIRE(packed.size() == nearest.size());
    u32 different = 0;
    for (u32 i = 0; i < packed.size(); ++i) {
      if (weights[i] == 0.0f) continue;
      different +=
          packed[i].triangle == nearest[i].triangle &&
                  packed[i].barycentric[0] == nearest[i].barycentric[0] &&
                  packed[i].barycentric[1] == nearest[i].barycentric[1] &&
                  packed[i].weight == nearest[i].weight &&
                  std::abs(i32{packed[i].normal_offset} - i32{nearest[i].normal_offset}) <= 1
              ? 0u
              : 1u;
    }
    CHECK(different == 0);
    CHECK(report.max_offset_disagreement == 0.0f);  // nothing was authored
    CHECK(report.invalid_normals.empty());

    // Authored offsets are kept, and how far they are from the measured ones is reported.
    for (u32 i = 0; i < footpoints.size(); ++i)
      footpoints[i].offset = binding_normal_offset(nearest[i]) + (i % 2 == 0 ? 0.0005f : 0.0f);
    REQUIRE(bind_from_records(mode, points, footpoints, r.at_rest(mode), SurfaceBindOptions{},
                              packed, &report, &error));
    u32 kept = 0;
    for (u32 i = 0; i < packed.size(); ++i)
      kept += packed[i].normal_offset == f32_to_f16(footpoints[i].offset) ? 1u : 0u;
    CHECK(kept == packed.size());
    CHECK(report.max_offset_disagreement >= 0.00049f);
    CHECK(report.max_offset_disagreement <= 0.00052f);

    // What it refuses, naming the vertex.
    Vector<AuthoredFootpoint> bad = footpoints;
    bad[7].triangle = r.surface.triangle_count();
    CHECK_FALSE(bind_from_records(mode, points, bad, r.at_rest(mode), SurfaceBindOptions{}, packed,
                                  nullptr, &error));
    CHECK(error.find("render vertex 7") != std::string::npos);
    bad = footpoints;
    bad[3].barycentric = Vec3{0.5f, 0.5f, 0.1f};
    CHECK_FALSE(bind_from_records(mode, points, bad, r.at_rest(mode), SurfaceBindOptions{}, packed,
                                  nullptr, &error));
    CHECK(error.find("render vertex 3") != std::string::npos);
    bad = footpoints;
    bad[4].barycentric = Vec3{1.2f, -0.2f, 0.0f};
    CHECK_FALSE(bind_from_records(mode, points, bad, r.at_rest(mode), SurfaceBindOptions{}, packed,
                                  nullptr, &error));
    bad = footpoints;
    bad[5].weight = 1.5f;
    CHECK_FALSE(bind_from_records(mode, points, bad, r.at_rest(mode), SurfaceBindOptions{}, packed,
                                  nullptr, &error));
    CHECK_FALSE(bind_from_records(mode, points, std::span(footpoints.data(), 10), r.at_rest(mode),
                                  SurfaceBindOptions{}, packed, nullptr, &error));
  }
}

TEST_CASE("binding: a reversed image is found and named") {
  // A render grid laid over the dome and bound to it: every image faces the way its triangle does.
  const Region r = make_region(Vec3{});
  const u32 n = 21;
  Vector<Vec3> grid;
  for (u32 j = 0; j < n; ++j) {
    for (u32 i = 0; i < n; ++i) {
      const f32 x = (static_cast<f32>(i) / static_cast<f32>(n - 1) - 0.5f) * 1.2f * k_a;
      const f32 y = (static_cast<f32>(j) / static_cast<f32>(n - 1) - 0.5f) * 1.2f * k_b;
      const f32 rr = (x / k_a) * (x / k_a) + (y / k_b) * (y / k_b);
      grid.push_back(Vec3{x, y, k_height * (1.0f - rr) * (1.0f - rr) * 0.8f + 0.004f});
    }
  }
  const Vector<u32> triangles = fixture::sheet_faces(n, n);
  const Vector<SurfaceBinding> bindings = bind(r, NormalMode::limit_interpolated, grid);
  BindingImageReport report;
  check_binding_images(bindings, grid, triangles, r.reference(), r.faces(), false, report);
  CHECK(report.triangles == triangles.size() / 3);
  CHECK(report.reversed.empty());
  MESSAGE("a 21 x 21 grid over the dome: image deviation p50 "
          << report.deviation_p50_deg << ", p95 " << report.deviation_p95_deg << ", max "
          << report.deviation_max_deg << " degrees");

  // Swap two corners' footpoints of one triangle: its image turns over, and only its neighbours
  // that share those two vertices can follow.
  Vector<SurfaceBinding> swapped = bindings;
  const u32 t = 2 * (10 * (n - 1) + 10);
  std::swap(swapped[triangles[3 * t + 1]], swapped[triangles[3 * t + 2]]);
  check_binding_images(swapped, grid, triangles, r.reference(), r.faces(), false, report);
  REQUIRE(!report.reversed.empty());
  CHECK(std::find(report.reversed.begin(), report.reversed.end(), t) != report.reversed.end());

  // Fully weighted only: a triangle with a corner below weight one is not checked.
  Vector<SurfaceBinding> partial = bindings;
  partial[triangles[0]].weight = 30000;
  check_binding_images(partial, grid, triangles, r.reference(), r.faces(), true, report);
  CHECK(report.triangles < triangles.size() / 3);
}

TEST_CASE("binding: the displacement and offset terms add up to the whole transfer") {
  const Region r = make_region(Vec3{});
  for (const NormalMode mode : k_modes) {
    CAPTURE(name(mode));
    fixture::Random random(80);
    Vector<Vec3> points;
    place(r, mode, random, 300, 0.9f, -0.004f, 0.004f, points);
    const Vector<SurfaceBinding> bindings = bind(r, mode, points);
    const Vector<Vec3> state = smooth_state(r, Vec3{0.0f, -0.02f, -0.015f});
    const Vector<Vec3> full = transfer(r, mode, bindings, points, state, BindingTerms::full);
    const Vector<Vec3> moved =
        transfer(r, mode, bindings, points, state, BindingTerms::displacement);
    const Vector<Vec3> offset = transfer(r, mode, bindings, points, state, BindingTerms::offset);
    f64 worst = 0.0;
    f64 largest_offset_term = 0.0;
    for (u32 i = 0; i < points.size(); ++i) {
      const D3 sum{static_cast<f64>(moved[i].x) + static_cast<f64>(offset[i].x) -
                       static_cast<f64>(points[i].x),
                   static_cast<f64>(moved[i].y) + static_cast<f64>(offset[i].y) -
                       static_cast<f64>(points[i].y),
                   static_cast<f64>(moved[i].z) + static_cast<f64>(offset[i].z) -
                       static_cast<f64>(points[i].z)};
      worst = std::max(worst, distance(full[i], sum));
      largest_offset_term = std::max(largest_offset_term, gap(offset[i], points[i]));
    }
    CHECK(worst <= 1.0e-7);
    CHECK(largest_offset_term > 1.0e-5);  // the offset term is not trivially zero
  }
}

TEST_CASE("binding: footpoints on the boundary and offsets over the limit are reported") {
  const Region r = make_region(Vec3{});
  for (const NormalMode mode : k_modes) {
    CAPTURE(name(mode));
    fixture::Random random(27);
    Vector<Vec3> points;
    place(r, mode, random, 50, 0.5f, 0.0f, 0.002f, points);  // 0 .. 49: ordinary
    for (u32 i = 0; i < 24; ++i) {                           // 50 .. 73: beyond the rim
      const f64 phi = 2.0 * std::numbers::pi * (static_cast<f64>(i) + 0.5) / 24.0;
      points.push_back(Vec3{static_cast<f32>(1.3 * static_cast<f64>(k_a) * std::cos(phi)),
                            static_cast<f32>(1.3 * static_cast<f64>(k_b) * std::sin(phi)), 0.0f});
    }
    SurfaceBindReport report;
    const Vector<SurfaceBinding> bindings = bind(r, mode, points, {}, &report);
    REQUIRE(bindings.size() == 74);
    REQUIRE(report.boundary_footpoints.size() == 24);
    for (u32 i = 0; i < 24; ++i)
      CHECK(report.boundary_footpoints[i] == 50 + i);
    CHECK(report.offset_exceeded.empty());

    // Offsets: the ordinary 50 again, and 10 placed 20 to 30 mm off the convex top.
    points.resize(50);
    place(r, mode, random, 10, 0.5f, 0.02f, 0.03f, points);  // 50 .. 59: over a 10 mm limit
    SurfaceBindReport limited;
    bind(r, mode, points, {}, &limited, 0.01f);
    CHECK(limited.boundary_footpoints.empty());
    REQUIRE(limited.offset_exceeded.size() == 10);
    for (u32 i = 0; i < 10; ++i)
      CHECK(limited.offset_exceeded[i] == 50 + i);
    CHECK(limited.max_abs_offset >= 0.019f);
  }
}

TEST_CASE("binding: what the record cannot hold is refused, and bad records are named") {
  const Region r = make_region(Vec3{});
  const NormalMode mode = NormalMode::limit_interpolated;
  const Vec3 one[] = {Vec3{0.0f, 0.0f, 0.1f}};
  SurfaceBindOptions options;
  Vector<SurfaceBinding> out;
  std::string error;

  // A surface a u16 cannot address: a 256 x 129 torus has 66,048 triangles.
  const Vector<u32> big = fixture::torus_faces(256, 129);
  const Vector<Vec3> big_positions = fixture::torus_positions(256, 129, 1.0f, 0.3f);
  const BindingSurface big_surface{big, big_positions, {}, {}, {}};
  CHECK_FALSE(
      bind_to_surface(NormalMode::triangle, one, big_surface, options, out, nullptr, &error));
  CHECK(error.find("65536") != std::string::npos);

  const f32 too_heavy[] = {1.5f};
  options.weights = too_heavy;
  CHECK_FALSE(bind_to_surface(mode, one, r.at_rest(mode), options, out, nullptr, &error));
  CHECK(error.find("weight") != std::string::npos);
  const f32 two[] = {1.0f, 1.0f};
  options.weights = two;
  CHECK_FALSE(bind_to_surface(mode, one, r.at_rest(mode), options, out, nullptr, &error));
  options.weights = {};

  const u32 dangling[] = {0, 1, 99999};
  BindingSurface broken = r.at_rest(mode);
  broken.faces = dangling;
  CHECK_FALSE(bind_to_surface(mode, one, broken, options, out, nullptr, &error));
  CHECK(error.find("out of range") != std::string::npos);

  // An interpolated mode needs its normals.
  BindingSurface no_normals = r.at_rest(mode);
  no_normals.normal_reference = {};
  CHECK_FALSE(bind_to_surface(mode, one, no_normals, options, out, nullptr, &error));
  CHECK(error.find("limit-interpolated") != std::string::npos);

  const Vec3 far_away[] = {Vec3{0.0f, 0.0f, 1.0e6f}};
  CHECK_FALSE(bind_to_surface(mode, far_away, r.at_rest(mode), options, out, nullptr, &error));
  CHECK(error.find("half-float") != std::string::npos);

  // The limit mode without the tangent operators is refused, not quietly another mode.
  LoopLimitSurface bare;
  LoopSurfaceOptions no_tangents;
  no_tangents.tangents = false;
  REQUIRE(build_loop_limit_surface(r.cage_faces, 121, no_tangents, bare));
  BindingFrame f;
  CHECK_FALSE(evaluate_binding_frame(mode, bare, r.nodes, r.nodes, f, &error));
  CHECK(error.find("tangent") != std::string::npos);
  CHECK(evaluate_binding_frame(NormalMode::interpolated_vertex_area_weighted, bare, r.nodes,
                               r.nodes, f, &error));

  SurfaceBinding bad;
  bad.triangle = static_cast<u16>(r.surface.triangle_count());
  CHECK_FALSE(validate_surface_bindings({&bad, 1}, r.surface.triangle_count(), &error));
  bad.triangle = 0;
  bad.barycentric[0] = 40000;
  bad.barycentric[1] = 30000;
  CHECK_FALSE(validate_surface_bindings({&bad, 1}, r.surface.triangle_count(), &error));
  bad.barycentric[1] = 0;
  bad.normal_offset = 0x7c00;
  CHECK_FALSE(validate_surface_bindings({&bad, 1}, r.surface.triangle_count(), &error));
  bad.normal_offset = 0;
  CHECK(validate_surface_bindings({&bad, 1}, r.surface.triangle_count(), &error));
}

TEST_CASE("binding: two binds agree to the bit") {
  const Region r = make_region(Vec3{});
  for (const NormalMode mode : k_modes) {
    CAPTURE(name(mode));
    fixture::Random random(28);
    Vector<Vec3> points;
    place(r, mode, random, 500, 0.95f, -0.004f, 0.004f, points);
    const Vector<SurfaceBinding> a = bind(r, mode, points);
    const Vector<SurfaceBinding> b = bind(r, mode, points);
    REQUIRE(a.size() == b.size());
    CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(SurfaceBinding)) == 0);
  }
}
