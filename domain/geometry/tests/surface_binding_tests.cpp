// The surface binding (docs/subsystems/geometry.md, "Limit surfaces and surface bindings"): the
// 10-byte record and its decoders, the nearest-point binder and what it reports, and the
// displacement transfer's contract — the base bitwise where the weight is zero, a rigid motion of
// the cage carried rigidly at weight one, a seam two modules share and neither owns left alone
// whichever runs first, linearity in the node state when the offsets are zero, and the base itself
// at the reference state — bit for bit, on every compiler, which is why the transfer is written in
// the displacement form (surface_binding.h).
#include "limit_fixtures.h"

#include <domain/geometry/cluster.h>
#include <domain/geometry/limit_surface.h>
#include <domain/geometry/surface_binding.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>
#include <string>

using namespace engine;
using namespace engine::geometry;

namespace {

constexpr f32 k_a = 0.070f;  // the neutral fixture's footprint semi-axes, metres
constexpr f32 k_b = 0.086f;
constexpr f32 k_height = 0.080f;

// A region: the fixture's 121-node dome cage at `centre`, its level-3 limit operator, and the
// refined surface at the reference state.
struct Region {
  Vec3 centre{};
  Vector<u32> cage_faces;
  Vector<Vec3> nodes;
  LoopLimitSurface surface;
  Vector<Vec3> reference;

  std::span<const u32> faces() const { return surface.faces; }
};

Vector<Vec3> evaluate(const CsrMatrix& matrix, std::span<const Vec3> x) {
  Vector<Vec3> out(matrix.rows());
  apply(matrix, x, std::span<Vec3>(out.data(), out.size()));
  return out;
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
  options.tangents = false;
  std::string error;
  const bool ok = build_loop_limit_surface(r.cage_faces, 121, options, r.surface, &error);
  INFO(error);
  REQUIRE(ok);
  r.reference = evaluate(r.surface.limit, r.nodes);
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

// Points placed at a known footpoint and offset: a random triangle of the refined surface whose
// corners all lie within normalized radius `max_radius` of the region's centre, barycentrics at
// least 0.15 each, and a normal offset in [h_lo, h_hi].
void place(const Region& r, fixture::Random& random, u32 count, f32 max_radius, f32 h_lo, f32 h_hi,
           Vector<Vec3>& points, Vector<Placed>* placed = nullptr) {
  Vector<u32> candidates;
  for (u32 t = 0; t < r.surface.triangle_count(); ++t) {
    bool inside = true;
    for (u32 c = 0; c < 3; ++c) {
      const Vec3 p = r.reference[r.surface.faces[3 * t + c]] - r.centre;
      const f32 radius = std::sqrt((p.x / k_a) * (p.x / k_a) + (p.y / k_b) * (p.y / k_b));
      inside = inside && radius < max_radius;
    }
    if (inside) candidates.push_back(t);
  }
  REQUIRE(!candidates.empty());
  for (u32 i = 0; i < count; ++i) {
    const u32 t = candidates[static_cast<u32>(random.next() % candidates.size())];
    const f64 b0 = static_cast<f64>(random.uniform(0.15f, 0.7f));
    const f64 b1 = static_cast<f64>(random.uniform(0.15f, 0.85f - static_cast<f32>(b0)));
    const f64 b2 = 1.0 - b0 - b1;
    const D3 a = d3(r.reference[r.surface.faces[3 * t]]);
    const D3 b = d3(r.reference[r.surface.faces[3 * t + 1]]);
    const D3 c = d3(r.reference[r.surface.faces[3 * t + 2]]);
    const D3 e1{b.x - a.x, b.y - a.y, b.z - a.z};
    const D3 e2{c.x - a.x, c.y - a.y, c.z - a.z};
    D3 n{e1.y * e2.z - e1.z * e2.y, e1.z * e2.x - e1.x * e2.z, e1.x * e2.y - e1.y * e2.x};
    const f64 length = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
    n = D3{n.x / length, n.y / length, n.z / length};
    const f64 h = static_cast<f64>(random.uniform(h_lo, h_hi));
    points.push_back(Vec3{static_cast<f32>(b0 * a.x + b1 * b.x + b2 * c.x + h * n.x),
                          static_cast<f32>(b0 * a.y + b1 * b.y + b2 * c.y + h * n.y),
                          static_cast<f32>(b0 * a.z + b1 * b.z + b2 * c.z + h * n.z)});
    if (placed != nullptr) placed->push_back(Placed{t, b0, b1, b2, h});
  }
}

Vector<SurfaceBinding> bind(const Region& r, std::span<const Vec3> points,
                            std::span<const f32> weights = {}, SurfaceBindReport* report = nullptr,
                            f32 offset_limit = std::numeric_limits<f32>::infinity()) {
  SurfaceBindOptions options;
  options.weights = weights;
  options.offset_limit = offset_limit;
  Vector<SurfaceBinding> out;
  std::string error;
  const bool ok = bind_to_surface(points, r.reference, r.faces(), options, out, report, &error);
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

// The cage deformed in a way nothing is symmetric about: every node pushed by up to `amount`.
Vector<Vec3> deformed(const Vector<Vec3>& nodes, u64 seed, f32 amount) {
  fixture::Random random(seed);
  Vector<Vec3> out = nodes;
  for (Vec3& p : out)
    p = p + Vec3{random.uniform(-amount, amount), random.uniform(-amount, amount),
                 random.uniform(-amount, amount)};
  return out;
}

// The refined surface's displacement at a node state, the way a frame computes it.
Vector<Vec3> displacement(const Region& r, std::span<const Vec3> state_nodes) {
  Vector<Vec3> scratch(static_cast<u32>(state_nodes.size()));
  Vector<Vec3> out(r.surface.limit.rows());
  surface_displacement(r.surface.limit, r.nodes, state_nodes,
                       std::span<Vec3>(scratch.data(), scratch.size()),
                       std::span<Vec3>(out.data(), out.size()));
  return out;
}

Vector<Vec3> transfer(const Region& r, std::span<const SurfaceBinding> bindings,
                      std::span<const Vec3> base, std::span<const Vec3> state_nodes) {
  const Vector<Vec3> moved = displacement(r, state_nodes);
  Vector<Vec3> out(static_cast<u32>(base.size()));
  apply_binding(bindings, base, r.faces(), r.reference, moved,
                std::span<Vec3>(out.data(), out.size()));
  return out;
}

bool same_bits(Vec3 a, Vec3 b) { return std::memcmp(&a, &b, sizeof(Vec3)) == 0; }

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

TEST_CASE("binding: bind_to_surface finds the footpoint and offset a vertex was placed at") {
  const Region r = make_region(Vec3{});
  fixture::Random random(21);
  Vector<Vec3> points;
  Vector<Placed> placed;
  // Outside the convex top of the dome, where a point's nearest triangle is the one it was placed
  // over.
  place(r, random, 400, 0.5f, 0.0f, 0.003f, points, &placed);
  SurfaceBindReport report;
  const Vector<SurfaceBinding> bindings = bind(r, points, {}, &report);
  REQUIRE(bindings.size() == points.size());
  f64 worst_bary = 0.0;
  f64 worst_h = 0.0;
  f64 largest_h = 0.0;
  u32 wrong_triangle = 0;
  for (u32 i = 0; i < points.size(); ++i) {
    const SurfaceBinding& b = bindings[i];
    wrong_triangle += b.triangle == placed[i].triangle ? 0u : 1u;
    const Vec3 w = binding_barycentrics(b);
    worst_bary = std::max({worst_bary, std::fabs(static_cast<f64>(w.x) - placed[i].b0),
                           std::fabs(static_cast<f64>(w.y) - placed[i].b1)});
    worst_h =
        std::max(worst_h, std::fabs(static_cast<f64>(binding_normal_offset(b)) - placed[i].h));
    largest_h = std::max(largest_h, std::fabs(placed[i].h));
    CHECK(b.weight == 65535);
  }
  MESSAGE("400 vertices up to " << largest_h * 1000.0 << " mm off the surface: barycentrics within "
                                << worst_bary << ", offsets within " << worst_h * 1.0e6
                                << " um, the record's own quantization error at most "
                                << report.max_quantization_error * 1.0e6f << " um");
  CHECK(wrong_triangle == 0);
  CHECK(worst_bary <= 1.0e-4);  // 1/65535 of rounding plus the f32 placement of the point
  CHECK(worst_h <= std::ldexp(largest_h, -11) + 1.0e-8);
  CHECK(report.boundary_footpoints.empty());
  CHECK(report.offset_exceeded.empty());
  CHECK(static_cast<f64>(report.max_abs_offset) <= largest_h + 1.0e-8);
  CHECK(static_cast<f64>(report.max_quantization_error) <= std::ldexp(largest_h, -11) + 2.0e-7);
}

TEST_CASE("binding: a vertex of weight zero is the base, bit for bit") {
  const Region r = make_region(Vec3{});
  fixture::Random random(22);
  Vector<Vec3> points;
  place(r, random, 300, 0.95f, -0.004f, 0.004f, points);
  // Every other vertex has weight 0, the rest a weight in (0, 1].
  Vector<f32> weights;
  for (u32 i = 0; i < points.size(); ++i)
    weights.push_back(i % 2 == 0 ? 0.0f : random.uniform(0.05f, 1.0f));
  const Vector<SurfaceBinding> bindings = bind(r, points, weights);
  const Vector<Vec3> state = deformed(r.nodes, 5, 0.01f);
  const Vector<Vec3> out = transfer(r, bindings, points, state);
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
  const Vector<SurfaceBinding> unbound = bind(r, points, none);
  const Vector<Vec3> still = transfer(r, unbound, points, state);
  CHECK(std::memcmp(still.data(), points.data(), points.size() * sizeof(Vec3)) == 0);
}

TEST_CASE("binding: at weight one a rigid motion of the cage moves the bound vertices rigidly") {
  const Region r = make_region(Vec3{0.1f, -0.05f, 0.2f});
  fixture::Random random(23);
  Vector<Vec3> on_surface;
  place(r, random, 200, 0.5f, 0.0f, 0.0f, on_surface);
  Vector<Vec3> offset;
  place(r, random, 200, 0.5f, 0.0005f, 0.003f, offset);

  const Rigid motion(Vec3{1.0f, 2.0f, 3.0f}, 40.0, Vec3{0.2f, -0.1f, 0.3f});
  Vector<Vec3> state;
  for (const Vec3& p : r.nodes)
    state.push_back(motion.to_f32(p));

  for (const Vector<Vec3>* points : {&on_surface, &offset}) {
    SurfaceBindReport report;
    const Vector<SurfaceBinding> bindings = bind(r, *points, {}, &report);
    const Vector<Vec3> out = transfer(r, bindings, *points, state);
    f64 worst = 0.0;
    for (u32 i = 0; i < points->size(); ++i)
      worst = std::max(worst, distance(out[i], motion((*points)[i])));
    // Off rigid by (R - I) times the record's own reconstruction error, and f32 arithmetic.
    const f64 bound = 2.0 * static_cast<f64>(report.max_quantization_error) + 2.0e-6;
    MESSAGE(std::string(points == &on_surface ? "on the surface" : "0.5 to 3 mm off it")
            << ": a 40 degree turn and a 37 cm move carried to within " << worst * 1.0e6
            << " um of rigid (record quantization " << report.max_quantization_error * 1.0e6f
            << " um)");
    CHECK(worst <= bound);
  }
}

TEST_CASE("binding: a seam that belongs to neither module stays the base whichever runs first") {
  // Two modules, a left and a right region, over one render mesh laid out as
  // [left-only | seam | right-only]. Each module's span covers its own vertices and the seam, and
  // gives the seam weight 0: a bilateral region is two modules with a seam neither writes.
  const Region left = make_region(Vec3{-0.09f, 0.0f, 0.0f});
  const Region right = make_region(Vec3{0.09f, 0.0f, 0.0f});
  fixture::Random random(24);
  Vector<Vec3> base;
  place(left, random, 120, 0.9f, -0.003f, 0.003f, base);
  const u32 left_count = base.size();
  for (u32 i = 0; i < 17; ++i)  // the seam: the midline between the two, at the rims' height
    base.push_back(Vec3{0.0f, -0.08f + 0.01f * static_cast<f32>(i), random.uniform(0.0f, 0.004f)});
  const u32 seam_count = base.size() - left_count;
  place(right, random, 120, 0.9f, -0.003f, 0.003f, base);
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
  const Vector<SurfaceBinding> left_bindings = bind(left, left_span, left_weights);
  const Vector<SurfaceBinding> right_bindings = bind(right, right_span, right_weights);

  const Vector<Vec3> left_state = displacement(left, deformed(left.nodes, 31, 0.012f));
  const Vector<Vec3> right_state = displacement(right, deformed(right.nodes, 32, 0.012f));
  const auto run = [&](bool left_first) {
    Vector<Vec3> out = base;
    const std::span<Vec3> whole(out.data(), out.size());
    for (u32 pass = 0; pass < 2; ++pass) {
      if ((pass == 0) == left_first) {
        // In place: a module reads and writes its own span of the shared output.
        apply_binding(left_bindings, whole.subspan(0, left_count + seam_count), left.faces(),
                      left.reference, left_state, whole.subspan(0, left_count + seam_count));
      } else {
        apply_binding(right_bindings, whole.subspan(left_count, seam_count + right_count),
                      right.faces(), right.reference, right_state,
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
  u32 moved = 0;
  for (u32 i = 0; i < base.size(); ++i)
    moved += same_bits(a[i], base[i]) ? 0u : 1u;
  CHECK(moved == left_count + right_count);
}

TEST_CASE("binding: with zero offsets the transfer is linear in the node state") {
  const Region r = make_region(Vec3{});
  fixture::Random random(25);
  Vector<Vec3> on_surface;
  place(r, random, 300, 0.95f, 0.0f, 0.0f, on_surface);
  const Vector<SurfaceBinding> flat = bind(r, on_surface);
  for (const SurfaceBinding& b : flat)
    CHECK(std::fabs(binding_normal_offset(b)) <= 1.0e-7f);  // the f32 placement's residue only
  Vector<Vec3> offset;
  place(r, random, 300, 0.95f, 0.002f, 0.002f, offset);
  const Vector<SurfaceBinding> lifted = bind(r, offset);

  // The midpoint of two states against the average of their two transfers.
  const auto residual = [&](const Vector<SurfaceBinding>& bindings, const Vector<Vec3>& base,
                            const Vector<Vec3>& state_a, const Vector<Vec3>& state_b) {
    Vector<Vec3> middle;
    for (u32 i = 0; i < state_a.size(); ++i)
      middle.push_back((state_a[i] + state_b[i]) * 0.5f);
    const Vector<Vec3> a = transfer(r, bindings, base, state_a);
    const Vector<Vec3> b = transfer(r, bindings, base, state_b);
    const Vector<Vec3> m = transfer(r, bindings, base, middle);
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

  // Linearity holds for any states, so the hardest: every node pushed at random by up to 15 mm, a
  // crumpling no tissue would do.
  const Vector<Vec3> rough_a = deformed(r.nodes, 41, 0.015f);
  const Vector<Vec3> rough_b = deformed(r.nodes, 42, 0.015f);
  const auto [flat_p95, flat_max] = residual(flat, on_surface, rough_a, rough_b);
  CHECK(flat_max <= 1.0e-6);

  // The offset term's nonlinearity is second order in how far the normals turn, so it is measured
  // on smooth states like the fixture's: a sag of 30 mm down and 10 mm in at the centre, fading to
  // nothing at the fixed rim, against a 15 mm flattening.
  const auto smooth = [&](Vec3 at_centre) {
    Vector<Vec3> out = r.nodes;
    for (Vec3& p : out) {
      const f32 rr = (p.x / k_a) * (p.x / k_a) + (p.y / k_b) * (p.y / k_b);
      p = p + at_centre * std::max(0.0f, 1.0f - rr);
    }
    return out;
  };
  const auto [lifted_p95, lifted_max] = residual(lifted, offset, smooth(Vec3{0.0f, -0.03f, -0.01f}),
                                                 smooth(Vec3{0.0f, 0.0f, -0.015f}));
  const auto [rough_p95, rough_max] = residual(lifted, offset, rough_a, rough_b);
  MESSAGE(
      "midpoint state against the average of two transfers: zero offsets, nodes pushed at "
      "random by up to 15 mm, p95 "
      << flat_p95 * 1.0e6 << " um, max " << flat_max * 1.0e6
      << " um; 2 mm offsets on smooth sag states p95 " << lifted_p95 * 1.0e6 << " um, max "
      << lifted_max * 1.0e6 << " um (on the random states p95 " << rough_p95 * 1.0e6
      << " um: the normal term is second order in how far the normals turn)");
}

TEST_CASE("binding: at the reference state every vertex is its base") {
  const Region r = make_region(Vec3{});
  fixture::Random random(26);
  Vector<Vec3> points;
  place(r, random, 300, 0.95f, -0.004f, 0.004f, points);
  Vector<f32> weights;
  for (u32 i = 0; i < points.size(); ++i)
    weights.push_back(random.uniform(0.0f, 1.0f));
  const Vector<SurfaceBinding> bindings = bind(r, points, weights);
  const Vector<Vec3> out = transfer(r, bindings, points, r.nodes);
  u32 unchanged = 0;
  for (u32 i = 0; i < points.size(); ++i)
    unchanged += out[i] == points[i] ? 1u : 0u;
  CHECK(unchanged == points.size());
}

// The contract the displacement form exists for (surface_binding.h, "Why the displacement form"):
// with the nodes at the paired reference every output is its base, bit for bit, under any weight
// in (0, 1] and any offset, on every compiler. The first transfer computed two footpoint normals
// and subtracted them; GCC 13 at x86-64-v3 fused one normal's last multiply into the subtraction,
// and the test above failed there with 299 of 300. So this one is built to see any change at all:
// besides the placed vertices (some with a -0 coordinate, which x + 0 would turn into +0) it moves
// a base of signed zeros, denormals and tiny values, on which a change of 1e-40 would show, and it
// runs on the fixture and on a copy turned 73 degrees about a skew axis, so that no normal has a
// zero component to spare it rounding. Then, from the same reference, a rigid motion at weight one
// carries the vertices rigidly: the transfer is not simply the identity. Put back the old form and
// GCC 13 at x86-64-v3 changes 2,000 of the 2,000 sensitive vertices on both surfaces.
TEST_CASE("binding: the base bit for bit at the reference, and rigid under a motion of the cage") {
  const Rigid pose(Vec3{-2.0f, 1.0f, 0.5f}, 73.0, Vec3{0.31f, 1.12f, -0.47f});
  const Rigid motion(Vec3{0.3f, -1.0f, 2.0f}, 35.0, Vec3{-0.15f, 0.2f, 0.05f});
  const Region dome = make_region(Vec3{});
  Region turned = make_region(Vec3{});
  for (Vec3& p : turned.nodes)
    p = pose.to_f32(p);
  turned.reference = evaluate(turned.surface.limit, turned.nodes);

  fixture::Random random(30);
  // For the reference: the whole dome, inside it and out, and some vertices on the midline with
  // -0 as a mirrored mesh writes it.
  Vector<Vec3> placed;
  place(dome, random, 2000, 0.95f, -0.004f, 0.004f, placed);
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
  // placed over and the record reconstructs it (the rigid test above says why that matters).
  Vector<Vec3> outside;
  place(dome, random, 500, 0.5f, 0.0005f, 0.004f, outside);

  const Region* const regions[] = {&dome, &turned};
  for (const Region* r : regions) {
    const auto posed = [&](const Vector<Vec3>& points) {
      Vector<Vec3> out;
      for (const Vec3& p : points)
        out.push_back(r == &dome ? p : pose.to_f32(p));
      return out;
    };
    const Vector<Vec3> base = posed(placed);
    const Vector<SurfaceBinding> bindings = bind(*r, base, weights);
    u32 offset_bindings = 0;
    for (const SurfaceBinding& b : bindings)
      offset_bindings += binding_normal_offset(b) != 0.0f && b.weight != 0 ? 1u : 0u;
    CHECK(offset_bindings == bindings.size());  // nothing escapes through a zero weight or offset

    // The displacement of an unmoved cage is zero, exactly.
    const Vector<Vec3> still = displacement(*r, r->nodes);
    u32 nonzero = 0;
    for (const Vec3& d : still)
      nonzero += d == Vec3{} ? 0u : 1u;
    CHECK(nonzero == 0);

    const auto changed = [](const Vector<Vec3>& a, const Vector<Vec3>& b) {
      u32 count = 0;
      for (u32 i = 0; i < a.size(); ++i)
        count += same_bits(a[i], b[i]) ? 0u : 1u;
      return count;
    };
    CHECK(changed(transfer(*r, bindings, base, r->nodes), base) == 0);
    CHECK(changed(transfer(*r, bindings, sensitive, r->nodes), sensitive) == 0);
    Vector<Vec3> in_place = sensitive;  // `out` may be the base itself
    apply_binding(bindings, in_place, r->faces(), r->reference, still,
                  std::span<Vec3>(in_place.data(), in_place.size()));
    CHECK(changed(in_place, sensitive) == 0);

    // From the same reference, the cage moved rigidly: at weight one the vertices follow it.
    const Vector<Vec3> over = posed(outside);
    SurfaceBindReport report;
    const Vector<SurfaceBinding> whole = bind(*r, over, {}, &report);
    Vector<Vec3> state;
    for (const Vec3& p : r->nodes)
      state.push_back(motion.to_f32(p));
    const Vector<Vec3> carried = transfer(*r, whole, over, state);
    f64 worst = 0.0;
    for (u32 i = 0; i < over.size(); ++i)
      worst = std::max(worst, distance(carried[i], motion(over[i])));
    // Off rigid by (R - I) times the record's own reconstruction error, and f32 arithmetic.
    const f64 bound = 2.0 * static_cast<f64>(report.max_quantization_error) + 2.0e-6;
    MESSAGE(std::string(r == &dome ? "the dome" : "the turned copy")
            << ": 2000 vertices up to 4 mm off, the base bit for bit at the reference; a 35 degree "
               "turn of the cage carries 500 more to within "
            << worst * 1.0e6 << " um of rigid (record quantization "
            << report.max_quantization_error * 1.0e6f << " um)");
    CHECK(worst <= bound);
  }
}

TEST_CASE("binding: footpoints on the boundary and offsets over the limit are reported") {
  const Region r = make_region(Vec3{});
  fixture::Random random(27);
  Vector<Vec3> points;
  place(r, random, 50, 0.5f, 0.0f, 0.002f, points);  // 0 .. 49: ordinary
  for (u32 i = 0; i < 24; ++i) {                     // 50 .. 73: beyond the rim, on its plane
    const f64 phi = 2.0 * std::numbers::pi * (static_cast<f64>(i) + 0.5) / 24.0;
    points.push_back(Vec3{static_cast<f32>(1.3 * static_cast<f64>(k_a) * std::cos(phi)),
                          static_cast<f32>(1.3 * static_cast<f64>(k_b) * std::sin(phi)), 0.0f});
  }
  SurfaceBindReport report;
  const Vector<SurfaceBinding> bindings = bind(r, points, {}, &report);
  REQUIRE(bindings.size() == 74);
  REQUIRE(report.boundary_footpoints.size() == 24);
  for (u32 i = 0; i < 24; ++i)
    CHECK(report.boundary_footpoints[i] == 50 + i);
  CHECK(report.offset_exceeded.empty());

  // Offsets: the ordinary 50 again, and 10 placed 20 to 30 mm off the convex top.
  points.resize(50);
  place(r, random, 10, 0.5f, 0.02f, 0.03f, points);  // 50 .. 59: over a 10 mm limit
  SurfaceBindReport limited;
  bind(r, points, {}, &limited, 0.01f);
  CHECK(limited.boundary_footpoints.empty());
  REQUIRE(limited.offset_exceeded.size() == 10);
  for (u32 i = 0; i < 10; ++i)
    CHECK(limited.offset_exceeded[i] == 50 + i);
  CHECK(limited.max_abs_offset >= 0.02f);
}

TEST_CASE("binding: what the record cannot hold is refused, and bad records are named") {
  const Region r = make_region(Vec3{});
  const Vec3 one[] = {Vec3{0.0f, 0.0f, 0.1f}};
  SurfaceBindOptions options;
  Vector<SurfaceBinding> out;
  std::string error;

  // A surface a u16 cannot address: a 256 x 129 torus has 66,048 triangles.
  const Vector<u32> big = fixture::torus_faces(256, 129);
  const Vector<Vec3> big_positions = fixture::torus_positions(256, 129, 1.0f, 0.3f);
  CHECK_FALSE(bind_to_surface(one, big_positions, big, options, out, nullptr, &error));
  CHECK(error.find("65536") != std::string::npos);

  const f32 too_heavy[] = {1.5f};
  options.weights = too_heavy;
  CHECK_FALSE(bind_to_surface(one, r.reference, r.faces(), options, out, nullptr, &error));
  CHECK(error.find("weight") != std::string::npos);
  const f32 two[] = {1.0f, 1.0f};
  options.weights = two;
  CHECK_FALSE(bind_to_surface(one, r.reference, r.faces(), options, out, nullptr, &error));
  options.weights = {};

  const u32 dangling[] = {0, 1, 99999};
  CHECK_FALSE(bind_to_surface(one, r.reference, dangling, options, out, nullptr, &error));
  CHECK(error.find("out of range") != std::string::npos);

  const Vec3 far_away[] = {Vec3{0.0f, 0.0f, 1.0e6f}};
  CHECK_FALSE(bind_to_surface(far_away, r.reference, r.faces(), options, out, nullptr, &error));
  CHECK(error.find("half-float") != std::string::npos);

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
  fixture::Random random(28);
  Vector<Vec3> points;
  place(r, random, 500, 0.95f, -0.004f, 0.004f, points);
  const Vector<SurfaceBinding> a = bind(r, points);
  const Vector<SurfaceBinding> b = bind(r, points);
  REQUIRE(a.size() == b.size());
  CHECK(std::memcmp(a.data(), b.data(), a.size() * sizeof(SurfaceBinding)) == 0);
}
