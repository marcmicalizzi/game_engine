// The off-frustum shadow casters' volumes (caster_volume.h; docs/subsystems/renderer.md,
// "Shadows"), on the CPU against a brute-force membership:
//
// - a point `q` is in a directional light's sweep when some `q - t L`, `t` from 0 up to where it
//   reaches the cap, is inside the (closed) frustum and `q` is not past the cap. Sampled over `t`,
//   that is a subset of the sweep, and every point and every sphere it says is in must pass the
//   planes' test: the volume is **conservative**. The other way, exactly — the frustum's planes
//   give each `t` an interval — a point the planes accept must be in the unbounded sweep and short
//   of the cap, to a tolerance: the volume is **tight**, which a plane set that left out a
//   silhouette edge would not be.
// - a point light's hull the same way, over the segment from the light: `q` is in when some
//   `(q - s P) / (1 - s)` is in the frustum and `q` is within the light's reach.
// - the plane count never passes the array, the cap follows `shadow_distance` and the scene's
//   bounds, and a frustum with no far plane is closed at the scene's far side.
//
// The cameras are random — any direction, any field of view and aspect, symmetric or turned like a
// surround's side monitors — and so are the lights, with the awkward ones added by hand: a light
// along a frustum face, straight along the view, straight against it, overhead.
#include <core/math/math.h>
#include <domain/gfx/cluster_cull.h>
#include <systems/renderer/caster_volume.h>

#include <doctest/doctest.h>

#include <cmath>
#include <random>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

struct D3 {
  double x = 0, y = 0, z = 0;
};
D3 d3(Vec3 v) { return {v.x, v.y, v.z}; }
D3 operator+(D3 a, D3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
D3 operator-(D3 a, D3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
D3 operator*(D3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double dotd(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 crossd(D3 a, D3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
double lengthd(D3 a) { return std::sqrt(dotd(a, a)); }
Vec3 f3(D3 a) { return Vec3{static_cast<f32>(a.x), static_cast<f32>(a.y), static_cast<f32>(a.z)}; }

// A view-projection with its eye at the origin, as the renderer builds every view's: `forward`, a
// frustum given by its tangents at unit depth (a surround's side view is asymmetric), reversed Z
// with no far plane unless `far` is given.
Mat4 view_projection(Vec3 forward, f32 left, f32 right, f32 bottom, f32 top, f32 znear,
                     f32 far = 0.0f) {
  const Vec3 up = std::fabs(forward.y) > 0.95f ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 1.0f, 0.0f};
  const Mat4 view = look_at(Vec3{}, forward, up);
  Mat4 p;
  p.at(0, 0) = 2.0f / (right - left);
  p.at(0, 2) = (right + left) / (right - left);
  p.at(1, 1) = 2.0f / (top - bottom);
  p.at(1, 2) = (top + bottom) / (top - bottom);
  if (far <= 0.0f) {
    p.at(2, 2) = 0.0f;
    p.at(2, 3) = znear;
  } else {
    p.at(2, 2) = znear / (far - znear);
    p.at(2, 3) = far * znear / (far - znear);
  }
  p.at(3, 2) = -1.0f;
  p.at(3, 3) = 0.0f;
  return p * view;
}

struct Frame {
  Plane planes[6];
  D3 corners[8];
};

bool meet(const Plane& a, const Plane& b, const Plane& c, D3& out) {
  const D3 na = d3(a.normal), nb = d3(b.normal), nc = d3(c.normal);
  const D3 bc = crossd(nb, nc);
  const double det = dotd(na, bc);
  if (std::fabs(det) < 1e-12) return false;
  out = (bc * -double(a.d) + crossd(nc, na) * -double(b.d) + crossd(na, nb) * -double(c.d)) *
        (1.0 / det);
  return true;
}

bool frame_of(const Mat4& view_proj, const CasterBounds& bounds, Frame& out) {
  if (!closed_frustum(view_proj, bounds, out.planes)) return false;
  for (u32 i = 0; i < 8; ++i) {
    if (!meet(out.planes[i & 1], out.planes[2 + ((i >> 1) & 1)], out.planes[4 + ((i >> 2) & 1)],
              out.corners[i]))
      return false;
  }
  return true;
}

bool in_frustum(const Frame& f, D3 q, double tol = 0.0) {
  for (const Plane& p : f.planes)
    if (dotd(d3(p.normal), q) + double(p.d) < -tol) return false;
  return true;
}

// The cap the volume must reach and may not pass: the fit's rule, written again.
double cap_of(const Frame& f, D3 light, f32 distance, const CasterBounds& bounds, double& lowest) {
  double highest = -1e300;
  lowest = 1e300;
  for (const D3& c : f.corners) {
    highest = std::max(highest, dotd(light, c));
    lowest = std::min(lowest, dotd(light, c));
  }
  double cap = dotd(light, d3(bounds.scene_center)) + double(bounds.scene_radius);
  if (distance > 0.0f) cap = std::min(cap, highest + double(distance));
  return std::max(cap, highest);
}

// Sampled: some q - t L inside the frustum, t on a grid from 0 to `sweep`, and q short of the cap.
bool in_sweep_sampled(const Frame& f, D3 q, D3 light, double sweep, double cap) {
  if (dotd(light, q) > cap) return false;
  for (u32 k = 0; k <= 512; ++k) {
    if (in_frustum(f, q - light * (sweep * k / 512.0))) return true;
  }
  return false;
}

// Exact over t >= 0 (each plane bounds t on one side), widened by `tol`; and short of the cap.
bool in_sweep_exact(const Frame& f, D3 q, D3 light, double cap, double tol) {
  if (dotd(light, q) > cap + tol) return false;
  double lo = 0.0, hi = 1e300;
  for (const Plane& p : f.planes) {
    const double a = dotd(d3(p.normal), q) + double(p.d) + tol;
    const double b = dotd(d3(p.normal), light);
    if (std::fabs(b) < 1e-15) {
      if (a < 0.0) return false;
    } else if (b > 0.0) {
      hi = std::min(hi, a / b);
    } else {
      lo = std::max(lo, a / b);
    }
  }
  return lo <= hi;
}

// A point light's hull, sampled over the segment's parameter: some (q - s P) / (1 - s) in the
// frustum.
bool in_hull_sampled(const Frame& f, D3 q, D3 light) {
  for (u32 k = 0; k < 512; ++k) {
    const double s = k / 512.0;
    if (in_frustum(f, (q - light * s) * (1.0 / (1.0 - s)))) return true;
  }
  return false;
}

// Exact: the ray from the light through q meets the frustum at or beyond q (P + mu (q - P), mu >=
// 1). The tolerance is a distance at q, so near the light, where the frustum is many times farther,
// it is that many times wider at the frustum (mu times).
bool in_hull_exact(const Frame& f, D3 q, D3 light, double tol) {
  const D3 d = q - light;
  double lo = 1.0, hi = 1e300;
  for (const Plane& p : f.planes) {
    const double a = dotd(d3(p.normal), light) + double(p.d);
    const double b = dotd(d3(p.normal), d) + tol;
    if (std::fabs(b) < 1e-15) {
      if (a < 0.0) return false;
    } else if (b > 0.0) {
      lo = std::max(lo, -a / b);
    } else {
      hi = std::min(hi, -a / b);
    }
  }
  return lo <= hi;
}

struct Rng {
  std::mt19937 gen;
  explicit Rng(u32 seed) : gen(seed) {}
  double uniform(double a, double b) { return std::uniform_real_distribution<double>(a, b)(gen); }
  D3 direction() {
    for (;;) {
      const D3 v{uniform(-1, 1), uniform(-1, 1), uniform(-1, 1)};
      const double l = lengthd(v);
      if (l > 0.1 && l <= 1.0) return v * (1.0 / l);
    }
  }
  // A point of the frustum: a random convex combination of its corners, leaning on a few of them
  // so that faces and edges are reached and not only the middle.
  D3 in(const Frame& f) {
    double w[8];
    double total = 0.0;
    for (double& x : w) {
      x = std::pow(uniform(0.0, 1.0), 6.0);
      total += x;
    }
    D3 p{};
    for (u32 i = 0; i < 8; ++i)
      p = p + f.corners[i] * (w[i] / total);
    return p;
  }
  D3 on_edge(const Frame& f) {
    const u32 a = static_cast<u32>(uniform(0.0, 8.0)) & 7u;
    const u32 axis = static_cast<u32>(uniform(0.0, 3.0)) % 3u;
    const u32 b = a ^ (1u << axis);
    const double s = uniform(0.0, 1.0);
    return f.corners[a] * (1.0 - s) + f.corners[b] * s;
  }
};

struct Camera3 {
  Mat4 view_proj;
  CasterBounds bounds;
};

Camera3 random_camera(Rng& rng) {
  Camera3 c;
  const Vec3 forward = f3(rng.direction());
  const f32 ty = static_cast<f32>(std::tan(rng.uniform(0.25, 0.9)));
  const f32 aspect = static_cast<f32>(rng.uniform(0.5, 4.0));
  // One in three asymmetric, as a surround's side views are.
  f32 left = -ty * aspect, right = ty * aspect;
  if (rng.uniform(0.0, 1.0) < 0.33) {
    const f32 shift = static_cast<f32>(rng.uniform(-0.8, 0.8)) * ty * aspect;
    left += shift;
    right += shift;
  }
  const f32 znear = static_cast<f32>(rng.uniform(0.05, 1.0));
  const f32 far = rng.uniform(0.0, 1.0) < 0.2 ? static_cast<f32>(rng.uniform(5.0, 80.0)) : 0.0f;
  c.view_proj = view_projection(forward, left, right, -ty, ty, znear, far);
  c.bounds.scene_center = f3(rng.direction() * rng.uniform(0.0, 150.0));
  c.bounds.scene_radius = static_cast<f32>(rng.uniform(5.0, 200.0));
  return c;
}

}  // namespace

TEST_CASE("caster volume: a frustum with no far plane is closed at the scene's far side") {
  const Mat4 vp = view_projection(Vec3{0, 0, -1}, -0.5f, 0.5f, -0.5f, 0.5f, 0.1f);
  CasterBounds bounds;
  bounds.scene_center = Vec3{0, 0, -50};
  bounds.scene_radius = 100.0f;
  Plane planes[6];
  REQUIRE(closed_frustum(vp, bounds, planes));
  CHECK(planes[5].distance(Vec3{0, 0, -149.0f}) >= 0.0f);
  CHECK(planes[5].distance(Vec3{0, 0, -151.0f}) < 0.0f);
  // A projection with a nearer far plane of its own keeps it.
  const Mat4 near_far = view_projection(Vec3{0, 0, -1}, -0.5f, 0.5f, -0.5f, 0.5f, 0.1f, 40.0f);
  REQUIRE(closed_frustum(near_far, bounds, planes));
  CHECK(planes[5].distance(Vec3{0, 0, -39.0f}) >= 0.0f);
  CHECK(planes[5].distance(Vec3{0, 0, -41.0f}) < 0.0f);
}

TEST_CASE("caster volume: the directional sweep against a brute-force membership") {
  Rng rng(0x5eed1u);
  u32 cases = 0, accepted = 0, swept_points = 0, max_planes = 0, spheres = 0;
  for (u32 trial = 0; trial < 400; ++trial) {
    const Camera3 cam = random_camera(rng);
    // The awkward lights by hand on the first trials, then random ones.
    D3 light = rng.direction();
    Frame frame;
    REQUIRE(frame_of(cam.view_proj, cam.bounds, frame));
    if (trial % 8 == 1) light = d3(frame.planes[4].normal);         // along the view
    if (trial % 8 == 2) light = d3(frame.planes[4].normal) * -1.0;  // against it
    if (trial % 8 == 3) light = D3{0, 1, 0};                        // overhead
    if (trial % 8 == 4) {                                           // along a face
      const D3 n = d3(frame.planes[static_cast<u32>(rng.uniform(0, 4)) & 3u].normal);
      const D3 t = crossd(n, rng.direction());
      light = t * (1.0 / lengthd(t));
    }
    const f32 distance = trial % 3 == 0 ? static_cast<f32>(rng.uniform(1.0, 300.0)) : 0.0f;
    CasterVolume volume;
    REQUIRE(fit_directional_caster_volume(cam.view_proj, f3(light), distance, cam.bounds, volume));
    light = d3(normalize(f3(light)));
    CHECK(volume.plane_count <= gfx::k_caster_volume_planes);
    CHECK(volume.plane_count >= 5);
    max_planes = std::max(max_planes, volume.plane_count);
    double lowest = 0.0;
    const double cap = cap_of(frame, light, distance, cam.bounds, lowest);
    const double sweep = cap - lowest;
    double scale = lengthd(d3(cam.bounds.scene_center)) + cam.bounds.scene_radius;
    for (const D3& c : frame.corners)
      scale = std::max(scale, lengthd(c));
    scale = std::max(scale, std::fabs(cap));
    const double tol = 2e-5 * (scale + sweep);
    ++cases;

    for (u32 i = 0; i < 400; ++i) {
      // Near a face, near an edge, or anywhere in a box round the sweep.
      D3 q;
      const u32 kind = i % 3;
      const double t = rng.uniform(-0.1, 1.2) * sweep;
      if (kind == 0) {
        q = rng.in(frame) + light * t + rng.direction() * (rng.uniform(0.0, 0.03) * scale);
      } else if (kind == 1) {
        q = rng.on_edge(frame) + light * t + rng.direction() * (rng.uniform(0.0, 0.01) * scale);
      } else {
        q = rng.direction() * (rng.uniform(0.0, 1.5) * (scale + sweep));
      }
      const bool planes = caster_volume_contains_sphere(volume, f3(q), 0.0f);
      // Conservative: every point the brute force finds is accepted.
      if (in_sweep_sampled(frame, q, light, sweep, cap)) {
        ++swept_points;
        CHECK_MESSAGE(planes, "trial " << trial << " point " << i << " is swept and refused");
      }
      // Tight: every accepted point is in the unbounded sweep and short of the cap.
      if (planes) {
        ++accepted;
        CHECK_MESSAGE(in_sweep_exact(frame, q, light, cap, tol),
                      "trial " << trial << " point " << i << " is accepted and not swept");
      }
    }
    // Spheres: one any of whose sampled points is swept must be accepted.
    for (u32 i = 0; i < 60; ++i) {
      const D3 c = rng.in(frame) + light * (rng.uniform(-0.2, 1.3) * sweep) +
                   rng.direction() * (rng.uniform(0.0, 0.1) * scale);
      const double r = rng.uniform(0.0, 0.05) * scale;
      bool any = false;
      for (u32 k = 0; k < 48 && !any; ++k)
        any = in_sweep_sampled(frame, c + rng.direction() * (r * rng.uniform(0.0, 1.0)), light,
                               sweep, cap);
      if (any) {
        ++spheres;
        CHECK(caster_volume_contains_sphere(volume, f3(c), static_cast<f32>(r)));
      }
    }
  }
  MESSAGE(cases << " cameras and lights, at most " << max_planes << " planes; " << swept_points
                << " swept points and " << spheres << " swept spheres accepted, " << accepted
                << " accepted points all swept");
  CHECK(swept_points > 10000);
}

TEST_CASE("caster volume: the cap follows shadow_distance and the scene's bounds") {
  // Looking along -z with a 60 degree field, the light straight up: the frustum, closed at depth
  // 150 (the scene's far side), has its highest corners 86.6 up; the scene's far side towards the
  // light is 100 up.
  const f32 ty = std::tan(0.5236f);
  const Mat4 vp = view_projection(Vec3{0, 0, -1}, -ty, ty, -ty, ty, 0.1f);
  CasterBounds bounds;
  bounds.scene_center = Vec3{0, 0, -50};
  bounds.scene_radius = 100.0f;
  const Vec3 up{0, 1, 0};
  const Vec3 below_far{0, 0, -150};  // straight above a point of the frustum at every height
  CasterVolume volume;
  // The scene's bounds.
  REQUIRE(fit_directional_caster_volume(vp, up, 0.0f, bounds, volume));
  CHECK(caster_volume_contains_sphere(volume, below_far + up * 99.0f, 0.0f));
  CHECK(!caster_volume_contains_sphere(volume, below_far + up * 101.0f, 0.0f));
  // The scene's bounds move it.
  bounds.scene_radius = 120.0f;
  REQUIRE(fit_directional_caster_volume(vp, up, 0.0f, bounds, volume));
  CHECK(caster_volume_contains_sphere(volume, below_far + up * 119.0f, 0.0f));
  CHECK(!caster_volume_contains_sphere(volume, below_far + up * 121.0f, 0.0f));
  bounds.scene_radius = 100.0f;
  // A shadow distance: that far past the highest corner, 86.6 + 5.
  const f32 highest = 150.0f * ty;
  REQUIRE(fit_directional_caster_volume(vp, up, 5.0f, bounds, volume));
  CHECK(caster_volume_contains_sphere(volume, below_far + up * (highest + 4.5f), 0.0f));
  CHECK(!caster_volume_contains_sphere(volume, below_far + up * (highest + 5.5f), 0.0f));
  // ... and never past the scene's far side.
  REQUIRE(fit_directional_caster_volume(vp, up, 1000.0f, bounds, volume));
  CHECK(caster_volume_contains_sphere(volume, below_far + up * 99.0f, 0.0f));
  CHECK(!caster_volume_contains_sphere(volume, below_far + up * 101.0f, 0.0f));
  // A scene wholly below the frustum's top still never cuts the frustum.
  bounds.scene_center = Vec3{0, -500, -50};
  REQUIRE(fit_directional_caster_volume(vp, up, 0.0f, bounds, volume));
  CHECK(caster_volume_contains_sphere(volume, Vec3{0.0f, highest * 0.99f, -150.0f}, 0.0f));
  // Below the frustum, away from the light, nothing is a caster: the sweep goes up only.
  bounds.scene_center = Vec3{0, 0, -50};
  REQUIRE(fit_directional_caster_volume(vp, up, 0.0f, bounds, volume));
  CHECK(!caster_volume_contains_sphere(volume, below_far - up * (highest + 1.0f), 0.0f));
}

TEST_CASE("caster volume: a point light's hull against a brute-force membership") {
  Rng rng(0xb0bu);
  u32 cases = 0, inside_lights = 0, hull_points = 0, accepted = 0, max_planes = 0;
  for (u32 trial = 0; trial < 300; ++trial) {
    const Camera3 cam = random_camera(rng);
    Frame frame;
    REQUIRE(frame_of(cam.view_proj, cam.bounds, frame));
    double scale = lengthd(d3(cam.bounds.scene_center)) + cam.bounds.scene_radius;
    const D3 light = rng.direction() * (rng.uniform(0.0, 2.0) * scale);
    const f32 reach =
        trial % 2 == 0 ? static_cast<f32>(rng.uniform(0.2, 3.0) * scale) : 0.0f;  // 0: unbounded
    CasterVolume volume;
    const bool fit = fit_point_caster_volume(cam.view_proj, f3(light), reach, cam.bounds, volume);
    if (in_frustum(frame, light)) {
      ++inside_lights;
      CHECK(!fit);  // a light inside the frustum needs no volume
      continue;
    }
    if (!fit) {
      // Only a light on a face, within rounding, may be taken for one inside.
      CHECK(in_frustum(frame, light, 1e-4 * scale));
      continue;
    }
    ++cases;
    CHECK(volume.plane_count <= gfx::k_caster_volume_planes);
    max_planes = std::max(max_planes, volume.plane_count);
    scale = std::max(scale, lengthd(light));
    const double tol = 2e-5 * scale;
    for (u32 i = 0; i < 400; ++i) {
      D3 q;
      const double s = rng.uniform(-0.1, 1.1);
      const D3 f = i % 2 == 0 ? rng.in(frame) : rng.on_edge(frame);
      if (i % 4 < 3) {
        q = f * (1.0 - s) + light * s + rng.direction() * (rng.uniform(0.0, 0.02) * scale);
      } else {
        q = rng.direction() * (rng.uniform(0.0, 2.5) * scale);
      }
      const bool within = reach <= 0.0f || lengthd(q - light) <= double(reach);
      const bool planes = caster_volume_contains_sphere(volume, f3(q), 0.0f);
      if (within && in_hull_sampled(frame, q, light)) {
        ++hull_points;
        CHECK_MESSAGE(planes, "trial " << trial << " point " << i << " is in the hull and refused");
      }
      if (planes) {
        ++accepted;
        CHECK_MESSAGE(in_hull_exact(frame, q, light, tol),
                      "trial " << trial << " point " << i << " is accepted and not in the hull");
        if (reach > 0.0f) CHECK(lengthd(q - light) <= double(reach) + tol);
      }
    }
  }
  MESSAGE(cases << " lights outside the frustum (" << inside_lights << " inside), at most "
                << max_planes << " planes; " << hull_points << " hull points accepted, " << accepted
                << " accepted points all in the hull");
  CHECK(hull_points > 10000);
  CHECK(max_planes <= 11);
}
