// A preview of the ground's detail without a GPU (docs/subsystems/gfx.md, "The ground's detail"):
// the CPU mirror (ground_detail_reference.h) evaluated over a few patches of sand and shaded by
// brdf_reference.h under a low sun, written as PNGs into the test's own scratch directory, which
// is kept and named. It is how the sand is looked at where no device runs — this is the mirror the
// GPU test holds the shader to, so the picture is the resolve's to its tolerance — and how a change
// to the function is judged before the owner walks it.
//
// It is **skipped unless asked for**, so the suite's time does not move:
//
//   engine_gfx_tests -tc="ground detail preview*" --no-skip
//
// Every picture is drawn twice: `before_*` by the first pass as it was merged
// (ground_detail_reference_v1.h, frozen) and `after_*` by the live mirror, each under a sun 20
// degrees up, once along the wind (from upwind, `_along`) and once across it (`_across`). The wind
// blows left to right in every picture (+x), the scene's defaults (`GroundDetailDesc`, or the
// erg's numbers where they differ) and seed 7:
//
// - `top_<ground>`: 2 m x 2 m from straight above at 2 mm a pixel, on flat ground, a 15-degree
//   slope climbing into the wind (`windward15`), a 15-degree slope falling away from it (`lee15`),
//   and a 32-degree slip face falling away from it (`slip32`);
// - `grain`: 25 cm x 25 cm from above at 0.25 mm a pixel on the flat, where the grain is judged;
// - `eye_<ground>`: 1280 x 720 in perspective from 1.65 m above the ground, looking downwind and
//   25 degrees down with a 60-degree vertical field, the footprint by central differences as the
//   GPU test measures it, on the flat and on the 15-degree lee slope;
// - `mask_<ground>`: the top patches' masks (`Shading::weight` in red, the second pass's exposure
//   and streak weight in green and blue where the mirror has them).
#include "brdf_reference.h"
#include "ground_detail_reference.h"
#include "ground_detail_reference_v1.h"

#include <core/containers/vector.h>
#include <domain/gfx/ground_detail.h>
#include <foundation/image/png.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <string>

using namespace engine;
namespace ref = engine::brdf_ref;
using ref::Dvec3;

namespace {

constexpr double k_deg = 3.14159265358979323846 / 180.0;
constexpr Dvec3 k_albedo{0.62, 0.47, 0.32};  // linear: a warm quartz sand
constexpr double k_roughness = 0.85;
constexpr double k_sun_elevation = 20.0 * k_deg;
constexpr double k_sun_intensity = 4.5;
constexpr Dvec3 k_sky{0.45, 0.55, 0.75};

// What a mirror hands the shading: the three things `ground_detail_shade` changes, and the masks.
struct Detail {
  Dvec3 normal;
  Dvec3 albedo;
  double roughness = 1.0;
  double weight = 0.0;
  double exposure = 0.0;
  double streak = 0.0;
};

Detail first_pass(const gfx::GroundDetailParams& d, Dvec3 p, Dvec3 n, Dvec3 dpdx, Dvec3 dpdy) {
  const ground_ref_v1::Shading s =
      ground_ref_v1::shade(d, p, n, dpdx, dpdy, 1.0, k_albedo, k_roughness);
  return {s.normal, s.albedo, s.roughness, s.weight, 0.0, 0.0};
}

Detail live(const gfx::GroundDetailParams& d, Dvec3 p, Dvec3 n, Dvec3 dpdx, Dvec3 dpdy) {
  const ground_ref::Shading s = ground_ref::shade(d, p, n, dpdx, dpdy, 1.0, k_albedo, k_roughness);
  return {s.normal, s.albedo, s.roughness, s.weight, 0.0, 0.0};
}

using DetailFn = Detail (*)(const gfx::GroundDetailParams&, Dvec3, Dvec3, Dvec3, Dvec3);

Dvec3 sun_direction(bool along) {
  const double c = std::cos(k_sun_elevation);
  const double s = std::sin(k_sun_elevation);
  // Along: the sun stands upwind, so the light travels with the wind and a windward face is lit.
  // Across: it stands to the wind's left (+z seen from above with +x to the right is down).
  return along ? Dvec3{-c, s, 0.0} : Dvec3{0.0, s, c};
}

void put(Vector<u8>& rgb, Dvec3 linear) {
  rgb.push_back(ref::display(linear.x));
  rgb.push_back(ref::display(linear.y));
  rgb.push_back(ref::display(linear.z));
}

Dvec3 lit(const Detail& g, Dvec3 position, Dvec3 view, Dvec3 sun) {
  ref::Surface s;
  s.position = position;
  s.normal = g.normal;
  s.view = view;
  s.albedo = g.albedo;
  s.roughness = g.roughness;
  return ref::shade(s, sun, k_sun_intensity, k_sky, k_albedo, nullptr, 0, Dvec3{});
}

bool write(const test::TempDir& dir, const std::string& name, u32 w, u32 h, const Vector<u8>& rgb) {
  const io::Status status = image::write_png(dir.file(name), w, h, 3, {rgb.data(), rgb.size()});
  return status == io::Status::Ok;
}

// A plane through the origin whose normal leans `slope_deg` along +x (`sign` +1 falls away from a
// wind blowing +x: the lee; -1 climbs into it: the windward side).
Dvec3 plane_normal(double slope_deg, double sign) {
  return Dvec3{sign * std::sin(slope_deg * k_deg), std::cos(slope_deg * k_deg), 0.0};
}

double plane_y(Dvec3 n, double x, double z) { return -(n.x * x + n.z * z) / n.y; }

// From straight above: pixel (i, j) is (x0 + (i + 0.5) step, z0 + (j + 0.5) step) on the plane.
void top_view(const test::TempDir& dir, const std::string& tag, const std::string& ground,
              DetailFn detail, const gfx::GroundDetailParams& d, Dvec3 n, double x0, double z0,
              double step, u32 size, bool masks) {
  Vector<u8> along, across, mask;
  along.reserve(static_cast<usize>(size) * size * 3);
  across.reserve(static_cast<usize>(size) * size * 3);
  if (masks) mask.reserve(static_cast<usize>(size) * size * 3);
  const Dvec3 dpdx{step, -n.x / n.y * step, 0.0};
  const Dvec3 dpdy{0.0, -n.z / n.y * step, step};
  const Dvec3 up{0.0, 1.0, 0.0};
  for (u32 j = 0; j < size; ++j) {
    for (u32 i = 0; i < size; ++i) {
      const double x = x0 + (i + 0.5) * step;
      const double z = z0 + (j + 0.5) * step;
      const Dvec3 p{x, plane_y(n, x, z), z};
      const Detail g = detail(d, p, n, dpdx, dpdy);
      put(along, lit(g, p, up, sun_direction(true)));
      put(across, lit(g, p, up, sun_direction(false)));
      if (masks) {
        mask.push_back(static_cast<u8>(std::lround(ref::clamp01(g.weight) * 255.0)));
        mask.push_back(static_cast<u8>(std::lround(ref::clamp01(g.exposure) * 255.0)));
        mask.push_back(static_cast<u8>(std::lround(ref::clamp01(g.streak) * 255.0)));
      }
    }
  }
  CHECK(write(dir, tag + "_" + ground + "_along.png", size, size, along));
  CHECK(write(dir, tag + "_" + ground + "_across.png", size, size, across));
  if (masks) CHECK(write(dir, tag + "_mask_" + ground + ".png", size, size, mask));
}

// The ray through screen position (sx, sy) meets the plane through the origin with normal n; the
// eye stands `eye_height` above the origin along the vertical. False where it looks at the sky.
struct Camera {
  Dvec3 eye, forward, right, up;
  double tan_half = 0.0, aspect = 1.0;
  u32 width = 0, height = 0;
};

bool hit(const Camera& c, Dvec3 n, double sx, double sy, Dvec3& out) {
  const double ndc_x = 2.0 * sx / c.width - 1.0;
  const double ndc_y = 1.0 - 2.0 * sy / c.height;
  const Dvec3 dir =
      c.forward + c.right * (ndc_x * c.aspect * c.tan_half) + c.up * (ndc_y * c.tan_half);
  const double denom = ref::dot(n, dir);
  if (!(denom < -1e-9)) return false;
  const double t = -ref::dot(n, c.eye) / denom;
  if (!(t > 0.0) || t > 5000.0) return false;
  out = c.eye + dir * t;
  return true;
}

void eye_view(const test::TempDir& dir, const std::string& tag, const std::string& ground,
              DetailFn detail, const gfx::GroundDetailParams& d, Dvec3 n) {
  Camera c;
  c.width = 1280;
  c.height = 720;
  c.aspect = static_cast<double>(c.width) / c.height;
  c.tan_half = std::tan(30.0 * k_deg);
  c.eye = Dvec3{0.0, plane_y(n, 0.0, 0.0) + 1.65, 0.0};
  const double pitch = 25.0 * k_deg;
  c.forward = Dvec3{std::cos(pitch), -std::sin(pitch), 0.0};
  c.right = ref::normalize(ref::cross(c.forward, Dvec3{0.0, 1.0, 0.0}));
  c.up = ref::cross(c.right, c.forward);
  Vector<u8> along, across;
  along.reserve(static_cast<usize>(c.width) * c.height * 3);
  across.reserve(static_cast<usize>(c.width) * c.height * 3);
  constexpr double h = 0.005;
  for (u32 py = 0; py < c.height; ++py) {
    for (u32 px = 0; px < c.width; ++px) {
      const double sx = px + 0.5;
      const double sy = py + 0.5;
      Dvec3 p, x0, x1, y0, y1;
      if (!hit(c, n, sx, sy, p) || !hit(c, n, sx - h, sy, x0) || !hit(c, n, sx + h, sy, x1) ||
          !hit(c, n, sx, sy - h, y0) || !hit(c, n, sx, sy + h, y1)) {
        put(along, ref::sky_radiance(k_sky));
        put(across, ref::sky_radiance(k_sky));
        continue;
      }
      const Dvec3 dpdx = (x1 - x0) * (1.0 / (2.0 * h));
      const Dvec3 dpdy = (y1 - y0) * (1.0 / (2.0 * h));
      const Detail g = detail(d, p, n, dpdx, dpdy);
      const Dvec3 view = ref::normalize(c.eye - p);
      put(along, lit(g, p, view, sun_direction(true)));
      put(across, lit(g, p, view, sun_direction(false)));
    }
  }
  CHECK(write(dir, tag + "_eye_" + ground + "_along.png", c.width, c.height, along));
  CHECK(write(dir, tag + "_eye_" + ground + "_across.png", c.width, c.height, across));
}

void draw_all(const test::TempDir& dir, const std::string& tag, DetailFn detail,
              const gfx::GroundDetailParams& d) {
  struct Ground {
    const char* name;
    double slope;
    double sign;
  };
  const Ground grounds[] = {
      {"flat", 0.0, 1.0}, {"windward15", 15.0, -1.0}, {"lee15", 15.0, 1.0}, {"slip32", 32.0, 1.0}};
  for (const Ground& g : grounds) {
    top_view(dir, tag, std::string("top_") + g.name, detail, d, plane_normal(g.slope, g.sign), -1.0,
             -1.0, 0.002, 1000, true);
  }
  top_view(dir, tag, "grain", detail, d, plane_normal(0.0, 1.0), 0.3, 0.3, 0.00025, 1000, false);
  eye_view(dir, tag, "flat", detail, d, plane_normal(0.0, 1.0));
  eye_view(dir, tag, "lee15", detail, d, plane_normal(15.0, 1.0));
}

}  // namespace

TEST_CASE("ground detail preview: the sand from the mirror, before and after" * doctest::skip()) {
  test::TempDir dir("ground-detail-preview");
  REQUIRE(dir.ok());
  dir.keep();
  // The erg's numbers (content/test-scenes/desert-erg, `terrain.detail`) with the wind along +x.
  const gfx::GroundDetailDesc desc;
  const gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  draw_all(dir, "before", first_pass, d);
  draw_all(dir, "after", live, d);
  MESSAGE("ground detail preview written to " << dir.path());
}
