// The ground's detail on the GPU against its CPU mirror (ground_detail.slang against
// ground_detail_reference.h, docs/subsystems/gfx.md "The ground's detail"): sixty metres of sand
// with `k_material_ground_detail`, seen from a walker's eye height looking along it and looking
// down at the feet, at two places — by the origin, and 3.7 km out, where a float has a quarter of
// a millimetre under the point and the ripples a phase error to show for it — each drawn shaded
// and in the detail view. Every covered pixel is held to the reference: the point the pixel sees
// on its own triangle, its footprint by central differences, the detail by `ground_ref::shade`,
// and the shading by `brdf_ref::shade`. So the function the resolve draws is the function the CPU
// computes, the fades are the same functions of the same footprint, and the tolerance says by how
// much float and double disagree about all of it. Twice: the first pass (the defaults) on level
// sand, which is the proof the defaults did not move; and the second pass (the ergs' numbers,
// every term on) on level sand, on a windward slope and on three lees — the grain and its normal,
// the spacing, the exposure's band, the streaks' band and a slip face — with a third look at the
// feet at a millimetre a pixel, where the gradient grain's octaves draw.
//
// Also, without a device: the block the scene's numbers make, and the rule the filter moves
// variance by — that the slope variance it carries is the pattern's own, measured over the CPU
// function — and that the function is continuous (it has no seams of its own: no lattice edge, no
// kernel edge, no wrap of the phase shows up as a jump between neighbouring points).
#include "brdf_reference.h"
#include "ground_detail_reference.h"
#include "ground_detail_reference_v1.h"
#include "ground_detail_reference_v2.h"
#include "raster_path.h"
#include "scene_fixture.h"

#include <domain/geometry/cluster.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/device.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/ground_detail.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/visibility_resolve.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <shaders/visibility_resolve.spv.h>
#include <string>
#include <vector>

using namespace engine;
namespace ref = engine::brdf_ref;
namespace gref = engine::ground_ref;

namespace {

gfx::GroundDetailParams test_block() {
  gfx::GroundDetailDesc desc;  // the defaults a scene's `detail` block starts from
  return gfx::ground_detail_block(desc, Vec2{0.6f, -0.8f}, 7u);
}

}  // namespace

TEST_CASE("ground detail: the block is the scene's numbers, and the pattern is continuous") {
  const gfx::GroundDetailParams d = test_block();
  CHECK(d.flags == (gfx::k_ground_ripples | gfx::k_ground_grain));
  CHECK(std::abs(d.wind.x - 0.6f) < 1e-6f);
  CHECK(std::abs(d.wind.y + 0.8f) < 1e-6f);
  CHECK(d.amplitude == doctest::Approx(0.004f));
  CHECK(d.cell == doctest::Approx(0.12f * (6.0f - 4.5f * 0.35f)));
  // No ripples at a height of nothing, no grain at a strength of nothing.
  gfx::GroundDetailDesc none;
  none.ripple_height = 0.0f;
  none.grain_albedo = 0.0f;
  none.grain_roughness = 0.0f;
  CHECK(gfx::ground_detail_block(none, Vec2{1.0f, 0.0f}, 1u).flags == 0u);
  // A zero wind is +x rather than a NaN.
  const gfx::GroundDetailParams still =
      gfx::ground_detail_block(gfx::GroundDetailDesc{}, Vec2{}, 1u);
  CHECK(still.wind.x == 1.0f);
  CHECK(still.wind.y == 0.0f);

  // Continuity: along lines crossing many lattice cells, kernel edges and phase wraps, the height
  // never jumps by more than its steepest honest slope allows over the step, and the numeric
  // derivative agrees with the analytic gradient wherever the pattern is not at a defect's core —
  // at the scene's asymmetry and at the plain sinusoid the filter eases it to.
  constexpr double k_step = 0.0005;  // half a millimetre
  u32 samples = 0;
  u32 gradient_checked = 0;
  u32 gradient_off = 0;
  double largest_jump = 0.0;
  double largest_slope = 0.0;
  for (u32 line = 0; line < 6; ++line) {
    const double a = line < 4 ? static_cast<double>(d.asymmetry) : 0.5;
    const double z = 0.37 + 1.13 * line;
    const double x0 = -3.0 + 0.71 * line;
    gref::Ripple previous = gref::ripple(d, x0, z, a);
    for (u32 i = 1; i <= 12000; ++i) {
      const double x = x0 + k_step * i;
      const gref::Ripple r = gref::ripple(d, x, z, a);
      const double slope = std::max(std::fabs(r.gx), std::fabs(previous.gx));
      largest_slope = std::max(largest_slope, slope);
      const double jump = std::fabs(r.height - previous.height);
      largest_jump = std::max(largest_jump, jump);
      // A step's change is its mean slope over it; twice the larger end's slope plus a
      // micrometre covers the curvature within half a millimetre.
      CHECK_MESSAGE(jump <= 2.0 * slope * k_step + 1e-6,
                    "the height jumps " << jump << " m in " << k_step << " m at x " << x);
      if (slope < 0.5) {
        ++gradient_checked;
        const double numeric = (r.height - previous.height) / k_step;
        const double analytic = 0.5 * (r.gx + previous.gx);
        if (std::fabs(numeric - analytic) > 0.02 + 0.02 * std::fabs(analytic)) ++gradient_off;
      }
      previous = r;
      ++samples;
    }
  }
  CHECK(gradient_off * 1000 <= gradient_checked);
  MESSAGE("continuity over " << samples << " steps of 0.5 mm: largest jump " << largest_jump
                             << " m, largest slope " << largest_slope << ", analytic gradient off "
                             << gradient_off << " of " << gradient_checked);

  // The filter's transfer: the slope variance the block says the ripples have is the pattern's
  // own, E|grad h|^2 over a patch of many kernels — which is what `k_ground_slope_share` was
  // measured to be — at the scene's asymmetry, at the sinusoid the filter eases it to, and with
  // more defects and fewer.
  for (u32 variant = 0; variant < 4; ++variant) {
    gfx::GroundDetailDesc desc;
    if (variant == 2) desc.ripple_defects = 0.0f;
    if (variant == 3) desc.ripple_defects = 1.0f;
    const gfx::GroundDetailParams v = gfx::ground_detail_block(desc, Vec2{0.6f, -0.8f}, 7u);
    const f32 a = variant == 1 ? 0.5f : v.asymmetry;
    double sum = 0.0;
    u32 count = 0;
    for (u32 j = 0; j < 400; ++j) {
      for (u32 i = 0; i < 400; ++i) {
        const gref::Ripple r =
            gref::ripple(v, 0.0103 * i - 2.1, 0.0107 * j + 5.3, static_cast<double>(a));
        sum += r.gx * r.gx + r.gz * r.gz;
        ++count;
      }
    }
    const double measured = sum / count;
    const double block = static_cast<double>(gfx::ground_slope_variance(v, a));
    const double ratio = measured / block;
    MESSAGE("slope variance at asymmetry " << a << ", defects " << desc.ripple_defects
                                           << ": measured " << measured << ", block " << block
                                           << " (ratio " << ratio << ")");
    CHECK(ratio > 0.8);
    CHECK(ratio < 1.2);
  }
}

namespace {

// A few thousand shaded points for the CPU cases: positions by the origin and kilometres out,
// normals from flat to a slip face in every direction, footprints from none (the path tracer's) to
// past every fade, the full debug evaluation and a partial sand share.
struct Probe {
  ref::Dvec3 position, normal, dpdx, dpdy;
  double mask = 1.0;
  bool full = false;
};

Probe probe(u32 i) {
  u32 h = gref::pcg(i * 2654435761u + 12345u);
  const auto next = [&h] {
    h = gref::pcg(h);
    return gref::unit(h);
  };
  Probe p;
  const double far = (i % 3 == 0) ? 3700.0 : ((i % 3 == 1) ? 0.0 : 900.0);
  p.position = ref::Dvec3{far + next() * 40.0 - 20.0, next() * 5.0, -far * 0.5 + next() * 40.0};
  const double slope = next() * 40.0 * 3.14159265358979323846 / 180.0;
  const double azimuth = next() * 2.0 * 3.14159265358979323846;
  p.normal = ref::Dvec3{std::sin(slope) * std::cos(azimuth), std::cos(slope),
                        std::sin(slope) * std::sin(azimuth)};
  const double step = (i % 5 == 0) ? 0.0 : 0.0002 * std::pow(10.0, next() * 3.0);
  p.dpdx = ref::Dvec3{step, 0.0, step * (next() - 0.5)};
  p.dpdy = ref::Dvec3{step * (next() - 0.5), 0.0, step * (0.2 + 3.0 * next())};
  p.mask = (i % 7 == 0) ? next() : 1.0;
  p.full = (i % 11 == 0);
  return p;
}

}  // namespace

TEST_CASE("ground detail: the defaults are the first pass, to the bit") {
  // A scene that names none of the second pass's numbers draws what the first pass drew: the live
  // mirror with the defaults is the frozen one (ground_detail_reference_v1.h), exactly, for the
  // defaults and for the erg's numbers, at every probe.
  const ref::Dvec3 albedo{0.62, 0.47, 0.32};
  for (u32 variant = 0; variant < 2; ++variant) {
    gfx::GroundDetailDesc desc;
    if (variant == 1) {
      desc.ripple_defects = 0.8f;
      desc.grain_size = 0.03f;
    }
    const gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{0.6f, -0.8f}, 7u);
    u32 differ = 0;
    for (u32 i = 0; i < 4000; ++i) {
      const Probe p = probe(i);
      const gref::Shading a =
          gref::shade(d, p.position, p.normal, p.dpdx, p.dpdy, p.mask, albedo, 0.85, p.full);
      const engine::ground_ref_v1::Shading b = engine::ground_ref_v1::shade(
          d, p.position, p.normal, p.dpdx, p.dpdy, p.mask, albedo, 0.85, p.full);
      const bool same = a.normal.x == b.normal.x && a.normal.y == b.normal.y &&
                        a.normal.z == b.normal.z && a.albedo.x == b.albedo.x &&
                        a.albedo.y == b.albedo.y && a.albedo.z == b.albedo.z &&
                        a.roughness == b.roughness && a.weight == b.weight && a.fade == b.fade &&
                        a.ripple == b.ripple && a.grain == b.grain;
      if (!same) ++differ;
    }
    CHECK(differ == 0);
  }
}

TEST_CASE("ground detail: the ripples go by which way the ground faces the wind") {
  gfx::GroundDetailDesc desc;
  desc.lee_start_deg = 10.0f;
  desc.lee_end_deg = 18.0f;
  const gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  REQUIRE((d.flags & gfx::k_ground_exposure) != 0u);
  CHECK((gfx::ground_detail_block(gfx::GroundDetailDesc{}, Vec2{1.0f, 0.0f}, 7u).flags &
         gfx::k_ground_exposure) == 0u);
  const double deg = 3.14159265358979323846 / 180.0;
  // The ground leaning `slope` degrees, its normal turned `azimuth` from the wind (0: it falls
  // along the wind, the lee; 180: it climbs into it, the windward side).
  const auto normal = [deg](double slope, double azimuth) {
    return ref::Dvec3{std::sin(slope * deg) * std::cos(azimuth * deg), std::cos(slope * deg),
                      std::sin(slope * deg) * std::sin(azimuth * deg)};
  };
  CHECK(gref::exposure(d, normal(0.0, 0.0)) == 1.0);
  CHECK(gref::exposure(d, normal(25.0, 180.0)) == 1.0);  // windward, however steep
  CHECK(gref::exposure(d, normal(9.9, 0.0)) == 1.0);
  CHECK(gref::exposure(d, normal(18.0, 0.0)) == doctest::Approx(0.0).epsilon(1e-9));
  CHECK(gref::exposure(d, normal(25.0, 0.0)) == 0.0);
  // A slope facing across the wind falls along it by less: 15 degrees at 60 from the wind is
  // tan(15) cos(60) = 7.6 degrees of fall along it, fully exposed.
  CHECK(gref::exposure(d, normal(15.0, 60.0)) == 1.0);
  // Smooth in the normal: along a sweep of the lee slope through the band and of the azimuth round
  // the compass, a hundredth of a degree never moves it by more than its steepest honest slope.
  double previous = gref::exposure(d, normal(0.0, 0.0));
  double largest = 0.0;
  for (u32 i = 1; i <= 3000; ++i) {
    const double e = gref::exposure(d, normal(i * 0.01, 0.0));
    largest = std::max(largest, std::fabs(e - previous));
    previous = e;
  }
  CHECK(largest < 0.004);  // 1.5 / 8 degrees x 0.01 degrees, and the tangent's stretch
  // What the exposure takes is smooth sand, not unresolved sand: on a lee slope past the band,
  // under a footprint that fades the ripples entirely, the roughness is the material's own —
  // the ripples' lost variance, weighted by the exposure, is none.
  gfx::GroundDetailParams ripples_only = d;
  ripples_only.flags &= ~gfx::k_ground_grain;
  const ref::Dvec3 far_step{0.2, 0.0, 0.0};
  const ref::Dvec3 albedo{0.6, 0.5, 0.4};
  const gref::Shading lee = gref::shade(ripples_only, ref::Dvec3{1.0, 0.0, 2.0}, normal(20.0, 0.0),
                                        far_step, far_step, 1.0, albedo, 0.85);
  CHECK(lee.exposure == 0.0);
  CHECK(lee.weight == 0.0);
  CHECK(lee.roughness == 0.85);
  const gref::Shading windward =
      gref::shade(ripples_only, ref::Dvec3{1.0, 0.0, 2.0}, normal(15.0, 180.0), far_step, far_step,
                  1.0, albedo, 0.85);
  CHECK(windward.exposure == 1.0);
  CHECK(windward.roughness > 0.85);
}

namespace {

// What the GPU case draws: sixty metres of sand round each of two sites — by the origin, and 3.7 km
// out — on a plane through the site's point at y = 0 that climbs along the wind by `climb_deg`: a
// windward slope when positive, a lee when negative, level at 0. Its normal lies in the wind's
// vertical plane, so the shading normal the resolve hands the detail tilts the way the exposure,
// the spacing and the streaks read it.
struct GroundCase {
  const char* name = "";
  gfx::GroundDetailParams block;
  double climb_deg = 0.0;
  // The slope's line turned off the wind by this much: 0 climbs or falls along the wind, where
  // the steering has nothing to turn; anything else is ground oblique to it.
  double turn_deg = 0.0;
  // A third look at each site: the feet at a millimetre a pixel (6 degrees over 160 pixels from
  // 1.6 m), the owner's pixel at his feet at 11520 x 2160. At the other two looks a pixel is a
  // centimetre or more, where the gradient grain's coarsest octave, 2 cm, has faded into the
  // roughness; at a millimetre its octaves draw down to 2.5 mm, and its normal with them.
  bool millimetre = false;
  // Tolerances, of 255, by the origin [0] and 3.7 km out [1]: on the shaded picture, and on the
  // detail view's ripple height (red), the ripples' drawn share (green) and the grain (blue).
  int shaded[2] = {2, 4};
  int ripple[2] = {2, 16};
  int share[2] = {2, 16};
  int grain[2] = {2, 16};
  // Where it is drawn: by the origin and 3.7 km out unless a case names other places, each with the
  // tolerances it holds (0 the origin's, 1 the 3.7 km ones).
  struct Site {
    f32 x, z;
    const char* name;
    u32 tolerance;
  };
  u32 site_count = 2;
  Site sites[3] = {{0.0f, 0.0f, "by the origin", 0}, {2917.37f, -2403.71f, "3.7 km out", 1}, {}};
  // Each view's block carries the frame at its own eye (`gfx::ground_detail_frame`), as the
  // renderer's does; false leaves it at the world's origin, which is the arithmetic before the
  // frame — the point's world coordinate as a float — and is how the far case measures what that
  // drew (gfx.md, "Far from the origin").
  bool framed = true;
  // The detail view with the neighbouring cell's frame against the eye's own, of 255: the raw
  // channels at full contrast, where the neighbour's frame puts the point up to 1.5 km from its
  // origin and a float's step there is 0.12 mm against the eye's 0.06 (4 measured, RTX 5090).
  int neighbour = 4;
  // false: measure and report, and hold no tolerance (the far case's frameless run, which is there
  // to show what the instrument reads when the pattern is taken at a float32 world coordinate).
  bool hold = true;
};

// What one view of a case measured: the worst difference from the reference, of 255, on the shaded
// picture and on each channel of the detail view, and what the view holds.
struct GroundView {
  std::string name;
  u32 compared = 0;     // covered pixels held to the reference
  u32 missing = 0;      // pixels the reference sees the sand at and the GPU drew nothing at
  u32 rippled = 0;      // with ripples drawn: their share times their fade above 0
  u32 streaked = 0;     // with the streaks weighted in
  u32 flowed = 0;       // with the grainflow's lanes weighted in
  u32 over_one = 0;     // shaded pixels more than 1 of 255 off
  u32 over_shaded = 0;  // shaded pixels past the case's tolerance
  int shaded = 0;
  int ripple = 0;
  int share = 0;
  int grain = 0;
  int green_lo = 255;  // the shaded picture's green channel over its middle half
  int green_hi = 0;
  u32 clamped = 0;  // pixels whose point the resolve moves onto its triangle's edge
  // The detail view drawn again with the frame of the cell beside the eye's — what it draws after
  // the eye crosses into that cell — and its worst difference from the view with the eye's own
  // frame, of 255 on any channel (framed cases only).
  int neighbour = 0;
  // The instrument (gfx.md, "Far from the origin"): along the view's middle row, how many times the
  // ripple height the GPU drew changes between neighbouring covered pixels, how many times the
  // reference's changes, and the metres of sand the row spans. Where the pattern is evaluated at a
  // float32 world coordinate, the GPU's changes are capped at one a float step.
  u32 gpu_changes = 0;
  u32 ref_changes = 0;
  // And how many times the point's float32 world coordinate (x, z), rounded from the reference's
  // point, changes along it: the most the GPU's can when the pattern is taken there.
  u32 coordinate_changes = 0;
  double row_metres = 0.0;
};

// The plane's unit normal for a climb of `climb_deg` along the wind: `-dot(n.xz, w) / n.y`, the
// climb as ground_detail.slang reads it, is its tangent.
// With `turn_deg` the line it climbs along is the wind's turned by that much about +y, so the
// ground is oblique to the wind and the steering has a fall line to turn the ripples along.
ref::Dvec3 climb_normal(const gfx::GroundDetailParams& d, double climb_deg, double turn_deg = 0.0) {
  const double t = climb_deg * 3.14159265358979323846 / 180.0;
  const double a = turn_deg * 3.14159265358979323846 / 180.0;
  const double s = std::sin(t);
  const double wx = static_cast<double>(d.wind.x);
  const double wz = static_cast<double>(d.wind.y);
  const double lx = wx * std::cos(a) - wz * std::sin(a);
  const double lz = wx * std::sin(a) + wz * std::cos(a);
  return ref::Dvec3{-s * lx, std::cos(t), -s * lz};
}

// Draws the case on `device` from a walker's eyes 1.6 m over the sand looking along it and looking
// down at the feet (and at a millimetre, with `millimetre`), each shaded and in the detail view,
// and holds every covered pixel to the reference: the pixel's own triangle as the resolve fetched
// it — the visibility buffer names it, and its corners are read off the mesh's 16-bit grid as the
// shader reads them — the point the pixel's centre sees on it, the footprint by central
// differences there, and the triangle's normal turned to the camera, which is the shading normal a
// mesh with no attributes has; then the detail by `ground_ref::shade` and the shading by
// `brdf_ref::shade`. On a level plane every triangle is the plane; on a sloped one the grid moves a
// corner off it by up to half a step (0.46 mm on 60 m), which turns a metre's triangle by half a
// milliradian — enough, through the spacing, to slide a ripple's lee by several levels of the
// detail view — so the reference takes the triangle and not the plane it was cut from.
void draw_ground_case(gfx::Device& device, const GroundCase& c, Vector<GroundView>& out) {
  std::string error;
  constexpr u32 k_size = 160;
  constexpr u32 k_max_sites = 3;
  constexpr u32 k_max_views = 3 * k_max_sites;
  const u32 looks = c.millimetre ? 3u : 2u;
  const u32 site_count = c.site_count;
  REQUIRE(site_count >= 1u);
  REQUIRE(site_count <= k_max_sites);
  const u32 views = site_count * looks;
  using Site = GroundCase::Site;
  const Site* sites = c.sites;
  const ref::Dvec3 normal = climb_normal(c.block, c.climb_deg, c.turn_deg);
  const bool level = c.climb_deg == 0.0;
  // The plane's height at (x, z) over its point at site `s`.
  const auto height = [&](u32 s, f32 x, f32 z) {
    if (level) return 0.0f;
    const double dx = static_cast<double>(x) - static_cast<double>(sites[s].x);
    const double dz = static_cast<double>(z) - static_cast<double>(sites[s].z);
    return static_cast<f32>(-(normal.x * dx + normal.z * dz) / normal.y);
  };
  struct Look {
    Vec3 eye, target;
    f32 fov_y;
  };
  const auto look = [&](u32 v) {
    const u32 s = v / looks;
    const Site site = sites[s];
    const auto on = [&](f32 x, f32 z, f32 above) { return Vec3{x, height(s, x, z) + above, z}; };
    if (v % looks == 0) {
      return Look{on(site.x, site.z + 6.0f, 1.6f), on(site.x + 2.0f, site.z - 20.0f, 0.0f),
                  radians(60.0f)};
    }
    return Look{on(site.x, site.z + 0.4f, 1.6f), on(site.x + 0.1f, site.z - 0.1f, 0.0f),
                radians(v % looks == 1 ? 60.0f : 6.0f)};
  };
  const char* look_names[3] = {"along the sand", "at the feet", "at the feet at a millimetre"};
  const Vec3 up{0.0f, 1.0f, 0.0f};

  constexpr gfx::BufferUsage k_storage = gfx::BufferUsage::Storage;
  // Two blocks a view: the frame at its own eye (or none, for a case that is not framed), and the
  // frame of the cell beside it, a frame corner further along both axes — what the renderer hands
  // over once the eye has crossed into that cell — for the seam test.
  std::vector<gfx::GroundDetailParams> detail_blocks(2u * views, c.block);
  for (u32 v = 0; v < views; ++v) {
    if (!c.framed) continue;
    const Vec3 eye = look(v).eye;
    const double g = gfx::k_ground_frame_cell;
    gfx::ground_detail_frame(detail_blocks[2 * v], static_cast<double>(eye.x),
                             static_cast<double>(eye.z));
    gfx::ground_detail_frame(detail_blocks[2 * v + 1], static_cast<double>(eye.x) + g,
                             static_cast<double>(eye.z) + g);
  }
  gfx::BufferResource detail_buffer;
  REQUIRE(gfx::upload_buffer(device, detail_blocks.data(),
                             detail_blocks.size() * sizeof(gfx::GroundDetailParams), k_storage,
                             detail_buffer, &error));

  gfx::ResolveMaterial material;
  material.albedo = Vec4{0.84f, 0.69f, 0.47f, 0.92f};
  material.flags = gfx::k_material_ground_detail;
  gfx::BufferResource materials;
  REQUIRE(gfx::upload_buffer(device, &material, sizeof(material), k_storage, materials, &error));

  // Sixty metres of sand round each site, as one quad: two triangles that reach behind the walker
  // in every look, which the resolve rebuilds as rightly as the ones in front of him (gfx.md,
  // "Where a pixel meets its triangle"). Until 2026-10-03 this was sixty cells of a metre a side,
  // to stay clear of the resolve's old clamp of screen barycentrics, which put a pixel of a
  // triangle reaching behind the camera on an edge.
  constexpr u32 k_cells = 1;
  constexpr f32 k_cell = 60.0f / static_cast<f32>(k_cells);
  geometry::ClusterMesh meshes[k_max_sites];
  gfx_test::SingleInstance scenes[k_max_sites];
  gfx::BufferResource clusters[k_max_sites];
  gfx::BufferResource triangles[k_max_sites];
  gfx::BufferResource cluster_materials[k_max_sites];
  u32 cluster_count[k_max_sites] = {};
  for (u32 s = 0; s < site_count; ++s) {
    Vector<Vec3> positions;
    Vector<u32> indices;
    for (u32 j = 0; j <= k_cells; ++j) {
      for (u32 i = 0; i <= k_cells; ++i) {
        const f32 x = sites[s].x - 30.0f + k_cell * static_cast<f32>(i);
        const f32 z = sites[s].z - 30.0f + k_cell * static_cast<f32>(j);
        positions.push_back(Vec3{x, height(s, x, z), z});
      }
    }
    for (u32 j = 0; j < k_cells; ++j) {
      for (u32 i = 0; i < k_cells; ++i) {
        const u32 v00 = j * (k_cells + 1) + i;
        const u32 v10 = v00 + 1;
        const u32 v01 = v00 + k_cells + 1;
        const u32 v11 = v01 + 1;
        for (u32 index : {v00, v11, v10, v00, v01, v11})
          indices.push_back(index);
      }
    }
    REQUIRE(geometry::build_clusters(std::span<const Vec3>(positions.data(), positions.size()),
                                     std::span<const u32>(indices.data(), indices.size()),
                                     geometry::ClusterBuildOptions{}, meshes[s], &error));
    cluster_count[s] = meshes[s].clusters.size();
    const Vector<u32> zeros(cluster_count[s], 0u);
    REQUIRE(gfx::upload_buffer(device, meshes[s].clusters.data(),
                               cluster_count[s] * sizeof(geometry::ClusterDesc), k_storage,
                               clusters[s], &error));
    REQUIRE(gfx::upload_buffer(device, zeros.data(), cluster_count[s] * sizeof(u32), k_storage,
                               cluster_materials[s], &error));
    REQUIRE(scenes[s].create(device, meshes[s], cluster_count[s], &error));
    REQUIRE(gfx::upload_buffer(device, meshes[s].triangles.data(),
                               meshes[s].triangles.size() * sizeof(u32), k_storage, triangles[s],
                               &error));
  }

  const u64 vis_bytes = u64{k_size} * k_size * sizeof(u64);
  gfx::BufferResource vis[k_max_views];
  for (u32 v = 0; v < views; ++v) {
    REQUIRE(gfx::create_buffer(device, vis_bytes,
                               k_storage | gfx::BufferUsage::ShaderDeviceAddress |
                                   gfx::BufferUsage::TransferDst | gfx::BufferUsage::TransferSrc,
                               false, vis[v], &error));
  }
  // Shaded, the detail view, and the detail view with the neighbouring cell's frame.
  const u32 blocks_count = views * 3;
  gfx::BufferResource params;
  gfx::BufferResource host_color;
  gfx::BufferResource host_vis;
  REQUIRE(gfx::create_buffer(device, sizeof(gfx::ResolveParams) * blocks_count,
                             k_storage | gfx::BufferUsage::ShaderDeviceAddress, true, params,
                             &error));
  REQUIRE(gfx::create_buffer(device, u64{k_size} * k_size * 4 * blocks_count,
                             gfx::BufferUsage::TransferDst, true, host_color, &error));
  REQUIRE(gfx::create_buffer(device, vis_bytes * views, gfx::BufferUsage::TransferDst, true,
                             host_vis, &error));

  gfx::FrameContext frames;
  REQUIRE(frames.create(device, 2, &error));
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, gfx::BindlessConfig{}, &error));
  gfx::ShaderModuleHandle resolve_module =
      gfx::create_shader_module(device, shaders::k_visibility_resolve_spirv,
                                shaders::k_visibility_resolve_spirv_size, &error);
  REQUIRE(resolve_module.valid());
  gfx_test::ClusterRaster raster;
  REQUIRE_MESSAGE(raster.create(device, bindless.pipeline_layout(),
                                geometry::ClusterBuildOptions{}.max_triangles, &error),
                  error);
  gfx::GraphicsPipelineDesc resolve_desc;
  resolve_desc.vertex = resolve_module;
  resolve_desc.vertex_entry = "vs_fullscreen";
  resolve_desc.fragment = resolve_module;
  resolve_desc.fragment_entry = "fs_resolve";
  resolve_desc.layout = bindless.pipeline_layout();
  resolve_desc.color_format = gfx::Format::R8G8B8A8Unorm;
  gfx::PipelineHandle resolve_pipeline = {};
  REQUIRE_MESSAGE(gfx::create_graphics_pipeline(device, resolve_desc, resolve_pipeline, &error),
                  error);

  const Vec4 sky{0.45f, 0.62f, 0.80f, 1.0f};
  // 21 degrees up, from down the wind: across the crests, where the ripples show.
  const Vec3 sun_dir = normalize(Vec3{0.6f, 0.38f, -0.8f});
  const Vec4 ground{0.84f, 0.69f, 0.47f, 0.0f};
  gfx::ClusterDrawParams draws[k_max_views];
  auto* blocks = static_cast<gfx::ResolveParams*>(params.mapped);
  for (u32 v = 0; v < views; ++v) {
    const Look l = look(v);
    const u32 s = v / looks;
    const Mat4 view_proj =
        perspective_reversed_z(l.fov_y, 1.0f, 0.05f) * look_at(l.eye, l.target, up);
    gfx::ClusterDrawParams& draw = draws[v];
    draw = gfx::ClusterDrawParams{};
    draw.view_proj = view_proj;
    draw.clusters = clusters[s].address;
    draw.mesh = scenes[s].meshes.address;
    draw.instances = scenes[s].instances.address;
    draw.triangles = triangles[s].address;
    draw.visibility = vis[v].address;
    draw.width = k_size;
    draw.height = k_size;
    for (u32 mode = 0; mode < 3; ++mode) {
      gfx::ResolveParams& b = blocks[v * 3 + mode];
      b = gfx::ResolveParams{};
      b.sky = sky;
      b.sun = Vec4{sun_dir, 1.0f};
      b.ground = ground;
      b.camera = Vec4{l.eye, 0.0f};
      b.view_proj = view_proj;
      b.visibility = vis[v].address;
      b.clusters = clusters[s].address;
      b.mesh = scenes[s].meshes.address;
      b.instances = scenes[s].instances.address;
      b.triangles = triangles[s].address;
      b.materials = materials.address;
      b.cluster_materials = cluster_materials[s].address;
      b.width = k_size;
      b.height = k_size;
      b.ground_detail =
          detail_buffer.address + (2 * v + (mode == 2 ? 1 : 0)) * sizeof(gfx::GroundDetailParams);
      b.mode =
          static_cast<u32>(mode == 0 ? gfx::ResolveMode::Shaded : gfx::ResolveMode::GroundDetail);
    }
  }
  u64 block_address[k_max_views * 2];
  for (u32 i = 0; i < blocks_count; ++i)
    block_address[i] = params.address + i * sizeof(gfx::ResolveParams);

  gfx::RenderGraph graph(device);
  gfx::RgBuffer rg_vis[k_max_views];
  for (u32 v = 0; v < views; ++v)
    rg_vis[v] = graph.import_buffer("vis", vis[v]);
  const gfx::RgBuffer rg_host = graph.import_buffer("host", host_color);
  const gfx::RgBuffer rg_host_vis = graph.import_buffer("host vis", host_vis);
  gfx::RgImage targets[k_max_views * 2];
  for (u32 i = 0; i < blocks_count; ++i) {
    targets[i] = graph.create_image(
        "resolved", {k_size, k_size, gfx::Format::R8G8B8A8Unorm,
                     gfx::ImageUsage::ColorAttachment | gfx::ImageUsage::TransferSrc});
  }
  graph.add_pass(
      "clear", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (u32 v = 0; v < views; ++v)
          b.write(rg_vis[v], gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph&) {
        for (u32 v = 0; v < views; ++v)
          cb.fill_buffer(vis[v].buffer, 0, gfx::k_whole_size, 0);
      });
  for (u32 v = 0; v < views; ++v) {
    graph.add_pass(
        "visibility", gfx::PassKind::Raster,
        [&, v](gfx::PassBuilder& b) {
          b.render_area(k_size, k_size);
          b.write(rg_vis[v], gfx::Access::FragmentReadWrite);
        },
        [&, v](gfx::CommandList cb, gfx::RenderGraph&) {
          raster.draw(cb, bindless, draws[v], cluster_count[v / looks]);
        });
  }
  for (u32 i = 0; i < blocks_count; ++i) {
    graph.add_pass(
        "resolve", gfx::PassKind::Raster,
        [&, i](gfx::PassBuilder& b) {
          b.color_attachment(targets[i], gfx::LoadOp::Clear, gfx::ClearColor{});
          b.read(rg_vis[i / 3], gfx::Access::FragmentRead);
        },
        [&, i](gfx::CommandList cb, gfx::RenderGraph&) {
          cb.bind_pipeline(gfx::BindPoint::Graphics, resolve_pipeline);
          bindless.bind(cb, gfx::BindPoint::Graphics);
          cb.push_constants(bindless.pipeline_layout(), gfx::ShaderStage::All, 0, sizeof(u64),
                            &block_address[i]);
          cb.draw(3, 1, 0, 0);
        });
  }
  graph.add_pass(
      "readback", gfx::PassKind::Transfer,
      [&](gfx::PassBuilder& b) {
        for (u32 i = 0; i < blocks_count; ++i)
          b.read(targets[i], gfx::Access::TransferRead);
        for (u32 v = 0; v < views; ++v)
          b.read(rg_vis[v], gfx::Access::TransferRead);
        b.write(rg_host, gfx::Access::TransferWrite);
        b.write(rg_host_vis, gfx::Access::TransferWrite);
      },
      [&](gfx::CommandList cb, gfx::RenderGraph& g) {
        for (u32 i = 0; i < blocks_count; ++i) {
          VkBufferImageCopy region{};
          region.bufferOffset = u64{k_size} * k_size * 4 * i;
          region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          region.imageExtent = {k_size, k_size, 1};
          vkCmdCopyImageToBuffer(gfx::vk::native(cb), gfx::vk::native(g.image(targets[i]).image),
                                 gfx::vk::native(g.image_layout(targets[i])),
                                 gfx::vk::native(host_color.buffer), 1, &region);
        }
        for (u32 v = 0; v < views; ++v) {
          cb.copy_buffer(vis[v].buffer, host_vis.buffer,
                         gfx::BufferCopy{0, vis_bytes * v, vis_bytes});
        }
      });
  REQUIRE_MESSAGE(graph.compile(&error), error);
  gfx::CommandList commands = frames.begin_frame();
  graph.execute(commands);
  REQUIRE(frames.wait(frames.end_frame()));

  const auto pixel = [&](u32 image, u32 x, u32 y) {
    return static_cast<const u8*>(host_color.mapped) +
           (u64{k_size} * k_size * image + y * k_size + x) * 4;
  };
  const auto* words = static_cast<const u64*>(host_vis.mapped);
  const auto unorm = [](double v) {
    return static_cast<int>(std::lround(ref::clamp01(v) * 255.0));
  };

  // Every covered pixel of every view against the reference. A pixel is covered where the camera
  // ray through its centre meets the plane in front of the camera; the ones within half a metre of
  // the quad's edge or a pixel of the horizon are left out, since a point sample there may be
  // either side.
  const ref::Dvec3 sky_ref = ref::dvec3(sky);
  const ref::Dvec3 sun_ref = ref::dvec3(sun_dir);
  const ref::Dvec3 ground_ref = ref::dvec3(ground);
  const ref::Dvec3 up_ref = ref::dvec3(up);
  const ref::Dvec3 sand{0.84, 0.69, 0.47};
  out.clear();
  for (u32 v = 0; v < views; ++v) {
    const Look l = look(v);
    const u32 s = v / looks;
    const Site site = sites[s];
    const u32 far = site.tolerance;
    const gfx::GroundDetailParams& block = detail_blocks[2 * v];
    const ref::Dvec3 eye = ref::dvec3(l.eye);
    const ref::Dvec3 target = ref::dvec3(l.target);
    const double fov = static_cast<double>(l.fov_y);
    const ref::Dvec3 plane_point{static_cast<double>(site.x), 0.0, static_cast<double>(site.z)};
    const geometry::ClusterMesh& mesh = meshes[s];
    GroundView r;
    r.name = std::string(site.name) + ", " + look_names[v % looks];
    // Pixels within this of the quad's edge are left out: half a metre, or four float steps of the
    // site's coordinates where those are coarser — 10,000 km out a step is a metre, and the
    // rasterizer, which still puts world coordinates through the whole matrix, draws an edge that
    // far off (gfx.md, "Measured from the eye").
    const double site_far =
        std::max(std::fabs(static_cast<double>(site.x)), std::fabs(static_cast<double>(site.z)));
    const double edge = 29.5 - std::max(0.0, 4.0 * gref::float_step(site_far) - 0.5);
    // The instrument's row: the previous covered pixel's two ripple heights and where it was.
    int row_gpu = -1;
    int row_ref = -1;
    ref::Dvec3 row_at{};
    for (u32 y = 0; y < k_size; ++y) {
      for (u32 x = 0; x < k_size; ++x) {
        const double cx = static_cast<double>(x) + 0.5;
        const double cy = static_cast<double>(y) + 0.5;
        const ref::Dvec3 seen = gref::point_on_plane(eye, target, up_ref, fov, 1.0, k_size, k_size,
                                                     cx, cy, plane_point, normal);
        // At or above the horizon, the ray meets the plane behind the eye.
        if (ref::dot(seen - eye, target - eye) <= 0.0) continue;
        if (std::fabs(seen.x - static_cast<double>(site.x)) > edge ||
            std::fabs(seen.z - static_cast<double>(site.z)) > edge) {
          continue;
        }
        ref::Dvec3 dpdx;
        ref::Dvec3 dpdy;
        gref::plane_footprint(eye, target, up_ref, fov, 1.0, k_size, k_size, x, y, plane_point,
                              normal, dpdx, dpdy);
        const double reach =
            std::max(std::sqrt(ref::dot(dpdx, dpdx)), std::sqrt(ref::dot(dpdy, dpdy)));
        if (reach > 1.0) continue;  // a pixel a metre long: the horizon's own rounding
        const u64 word = words[u64{k_size} * k_size * v + y * k_size + x];
        if (word == 0) {
          ++r.missing;
          continue;
        }
        // The pixel's own triangle, its corners where the shader reads them.
        const u32 id = static_cast<u32>(word & 0xffffffffu);
        const geometry::ClusterDesc& cluster = mesh.clusters[id >> 8];
        const u32 packed = mesh.triangles[cluster.triangle_offset + (id & 0xffu)];
        ref::Dvec3 corner[3];
        for (u32 k = 0; k < 3; ++k) {
          corner[k] = ref::dvec3(geometry::dequantize_position(
              mesh, cluster.vertex_offset + geometry::ClusterMesh::unpack(packed, k)));
        }
        ref::Dvec3 n = ref::normalize(ref::cross(corner[1] - corner[0], corner[2] - corner[0]));
        if (ref::dot(n, eye - corner[0]) < 0.0) n = -n;
        // The point the resolve shades: where the pixel centre's ray meets the triangle, as the
        // resolve takes it (`ref::pixel_on_triangle`). A centre within the rasterizer's sub-pixel
        // snap of an edge can be drawn by the triangle across it, and the resolve then shades the
        // point on the edge — up to a millimetre from where the ray meets the surface under a
        // grazing footprint a metre long, ten levels of the grain's channel by the origin on the
        // windward slope — so the reference does too.
        const ref::TriangleHit hit = ref::pixel_on_triangle(
            eye, ref::pixel_direction(eye, target, up_ref, fov, 1.0, k_size, k_size, cx, cy),
            corner);
        if (hit.clamped) ++r.clamped;
        const ref::Dvec3 p = hit.position;
        gref::plane_footprint(eye, target, up_ref, fov, 1.0, k_size, k_size, x, y, corner[0], n,
                              dpdx, dpdy);
        const gref::Shading g = gref::shade(block, p, n, dpdx, dpdy, 1.0, sand, 0.92, false);
        const gref::Shading data = gref::shade(block, p, n, dpdx, dpdy, 1.0, sand, 0.92, true);
        ref::Surface surface;
        surface.position = p;
        surface.normal = g.normal;
        surface.view = ref::normalize(eye - p);
        surface.albedo = g.albedo;
        surface.roughness = g.roughness;
        surface.metallic = 0.0;
        const ref::Dvec3 linear =
            ref::shade(surface, sun_ref, 1.0, sky_ref, ground_ref, nullptr, 0, ref::Dvec3{});
        const int expect[3] = {ref::display(linear.x), ref::display(linear.y),
                               ref::display(linear.z)};
        const int expect_data[3] = {unorm(data.ripple * 0.5 + 0.5), unorm(data.weight * data.fade),
                                    unorm(data.grain * 0.5 + 0.5)};
        const u8* got = pixel(v * 3, x, y);
        const u8* got_data = pixel(v * 3 + 1, x, y);
        const u8* got_neighbour = pixel(v * 3 + 2, x, y);
        int here = 0;
        for (u32 k = 0; k < 3; ++k) {
          here = std::max(here, std::abs(int{got[k]} - expect[k]));
          r.neighbour = std::max(r.neighbour, std::abs(int{got_neighbour[k]} - int{got_data[k]}));
        }
        if (y == k_size / 2) {
          if (row_gpu >= 0) {
            if (int{got_data[0]} != row_gpu) ++r.gpu_changes;
            if (expect_data[0] != row_ref) ++r.ref_changes;
            if (static_cast<f32>(p.x) != static_cast<f32>(row_at.x) ||
                static_cast<f32>(p.z) != static_cast<f32>(row_at.z)) {
              ++r.coordinate_changes;
            }
            r.row_metres += std::sqrt(ref::dot(p - row_at, p - row_at));
          }
          row_gpu = got_data[0];
          row_ref = expect_data[0];
          row_at = p;
        }
        const int ripple = std::abs(int{got_data[0]} - expect_data[0]);
        const int share = std::abs(int{got_data[1]} - expect_data[1]);
        const int grain = std::abs(int{got_data[2]} - expect_data[2]);
        // The first pixel of the view past a tolerance, whole, to start from.
        if ((here > c.shaded[far] && r.shaded <= c.shaded[far]) ||
            (ripple > c.ripple[far] && r.ripple <= c.ripple[far]) ||
            (share > c.share[far] && r.share <= c.share[far]) ||
            (grain > c.grain[far] && r.grain <= c.grain[far])) {
          MESSAGE(std::string(c.name)
                  << ", " << r.name << ", pixel " << x << "," << y << " at " << p.x << "," << p.y
                  << "," << p.z << ": gpu data " << int{got_data[0]} << "," << int{got_data[1]}
                  << "," << int{got_data[2]} << " cpu " << expect_data[0] << "," << expect_data[1]
                  << "," << expect_data[2] << "; gpu shaded " << int{got[0]} << "," << int{got[1]}
                  << "," << int{got[2]} << " cpu " << expect[0] << "," << expect[1] << ","
                  << expect[2]);
        }
        r.shaded = std::max(r.shaded, here);
        r.ripple = std::max(r.ripple, ripple);
        r.share = std::max(r.share, share);
        r.grain = std::max(r.grain, grain);
        if (here > 1) ++r.over_one;
        if (here > c.shaded[far]) ++r.over_shaded;
        if (data.weight * data.fade > 0.0) ++r.rippled;
        if (g.streak > 0.0) ++r.streaked;
        if (g.flow > 0.0) ++r.flowed;
        ++r.compared;
      }
    }
    for (u32 y = k_size / 4; y < 3 * k_size / 4; ++y) {
      for (u32 x = k_size / 4; x < 3 * k_size / 4; ++x) {
        r.green_lo = std::min(r.green_lo, int{pixel(v * 3, x, y)[1]});
        r.green_hi = std::max(r.green_hi, int{pixel(v * 3, x, y)[1]});
      }
    }
    MESSAGE(std::string(c.name) << ", " << r.name << ": " << r.compared << " pixels compared, "
                                << r.rippled << " with ripples drawn, " << r.streaked
                                << " with streaks, " << r.flowed << " with grainflow; shaded worst "
                                << r.shaded << " of 255 (" << r.over_one << " over 1, "
                                << r.over_shaded << " past the tolerance); detail view worst "
                                << r.ripple << " on the ripple, " << r.share << " on its share, "
                                << r.grain << " on the grain; " << r.clamped
                                << " points clamped onto their triangle; the neighbouring cell's "
                                << "frame " << r.neighbour << " of 255 from the eye's; along the "
                                << "middle row the ripple height changes " << r.gpu_changes
                                << " times on the GPU and " << r.ref_changes << " in the reference "
                                << "over " << r.row_metres << " m, where the float32 world "
                                << "coordinate changes " << r.coordinate_changes << " times");
    out.push_back(r);
    if (!c.hold) continue;
    CHECK(r.compared > k_size * k_size / 4);
    CHECK(r.missing == 0u);
    // The frame the eye crosses into draws what the eye's own did: every lattice, hash and lane is
    // the world's from either corner, and the two differ by where they round, a few hundredths of
    // a millimetre (gfx.md, "Far from the origin").
    if (c.framed) CHECK(r.neighbour <= c.neighbour);
    // By the origin every pixel holds the tolerance. 3.7 km out the shaded picture's worst pixel
    // is the tail of the float's reach — the reconstruction's millimetre in the grain's finest
    // octaves and at a ripple's crest — and the tail is the GPU's own arithmetic: the third pass's
    // level sand at a millimetre is 4 on the RTX 5090 and 6 on the Titan Xp, with 880 and 884 of
    // its 25,600 pixels over 1; one distribution, two worst pixels. So out there a pixel in a
    // thousand may pass the tolerance, and none by more than twice it: a mismatch in the shader
    // moves a view, not a pixel.
    if (far == 0) {
      CHECK(r.shaded <= c.shaded[0]);
    } else {
      CHECK(r.over_shaded * 1000u <= r.compared);
      CHECK(r.shaded <= 2 * c.shaded[1]);
    }
    CHECK(r.ripple <= c.ripple[far]);
    CHECK(r.share <= c.share[far]);
    CHECK(r.grain <= c.grain[far]);
  }

  graph.reset();
  gfx::destroy_pipeline(device, resolve_pipeline);
  raster.destroy(device);
  gfx::destroy_shader_module(device, resolve_module);
  bindless.destroy();
  for (u32 s = 0; s < site_count; ++s) {
    scenes[s].destroy(device);
    gfx::destroy_buffer(device, clusters[s]);
    gfx::destroy_buffer(device, triangles[s]);
    gfx::destroy_buffer(device, cluster_materials[s]);
  }
  for (u32 v = 0; v < views; ++v)
    gfx::destroy_buffer(device, vis[v]);
  for (gfx::BufferResource* b : {&host_color, &host_vis, &params, &detail_buffer, &materials})
    gfx::destroy_buffer(device, *b);
  frames.destroy();
}

}  // namespace

TEST_CASE("ground detail: the resolve draws the CPU's function of position") {
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;
  // The first pass: the defaults, on level sand. The tolerance — by the origin, 2 of 255 on the
  // shaded picture and on the detail view, the shading tests' own, and where both land (1, RTX
  // 5090, 2026-09-29); 3.7 km out, 4 on the shaded picture and 16 on the detail view: a float
  // there has a quarter of a millimetre under the point, the reconstruction interpolates corners
  // that far out and is off by about a millimetre, and a millimetre is a thirtieth of a ripple's
  // steep lee — which the raw height channel shows at full contrast (12–14 measured) and the
  // shaded picture at a few levels where the sun falls straight across the crests (3 measured, on
  // 485 of 25,600 pixels). It is the proof the defaults did not move with the second pass. Since
  // 2026-10-03 the reconstruction measures the corners from the eye and is off by the world
  // position's one rounding, an eighth of a millimetre: 6 on the detail view and 2 on the shaded
  // picture. The tolerances are the millimetre's still (gfx.md, "Measured from the eye").
  GroundCase first;
  first.name = "the first pass";
  first.block = test_block();
  Vector<GroundView> views;
  draw_ground_case(device, first, views);
  for (const GroundView& v : views)
    CHECK(v.rippled > 0u);

  // The ripples are in the picture: at the feet, with the sun low along the wind, the shaded sand
  // varies by more than the grain alone could make it (the grain moves the albedo by 8% either way
  // at most, a handful of levels here).
  REQUIRE(views.size() == 4u);
  MESSAGE("at the feet the green channel spans " << views[1].green_lo << " to "
                                                 << views[1].green_hi);
  CHECK(views[1].green_hi - views[1].green_lo > 20);
  device.destroy();
}

TEST_CASE("ground detail: the resolve draws the second pass's function on sloped sand") {
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;
  // The ergs' numbers as the second pass had them — its streaks on, 22 to 30 degrees, and none of
  // the third pass, which has a case of its own below — with the test's wind: every second-pass
  // term on, on ground that exercises each — level (the grain and its normal), climbing into the
  // wind at 12 degrees (the spacing, 1.53), falling away from it at 14 (inside the exposure's
  // band, 10 to 18), and at 26 and 32 (the streaks' band, and a whole slip face, where there are
  // no ripples). The ergs themselves no longer draw the streaks; a scene still may.
  gfx::GroundDetailDesc second = gref::erg_numbers();
  second.streak_start_deg = 22.0f;
  second.streak_full_deg = 30.0f;
  second.flow_start_deg = 0.0f;
  second.flow_full_deg = 0.0f;
  second.patch_size = 0.0f;
  second.steer_max_deg = 0.0f;
  second.ripple_celerity = 0.0f;
  second.flatten_start = 0.0f;
  second.flatten_end = 0.0f;
  const gfx::GroundDetailParams erg = gfx::ground_detail_block(second, Vec2{0.6f, -0.8f}, 7u);
  REQUIRE(erg.flags ==
          (gfx::k_ground_ripples | gfx::k_ground_grain | gfx::k_ground_exposure |
           gfx::k_ground_gradient_grain | gfx::k_ground_streaks | gfx::k_ground_spacing));
  REQUIRE(erg.grain_octaves == 5u);
  struct Slope {
    const char* name;
    double climb_deg;
    bool ripples;
    bool streaks;
  };
  const Slope slopes[5] = {{"level", 0.0, true, false},
                           {"climbing into the wind at 12 degrees", 12.0, true, false},
                           {"falling away from the wind at 14 degrees", -14.0, true, false},
                           {"falling away from the wind at 26 degrees", -26.0, false, true},
                           {"falling away from the wind at 32 degrees", -32.0, false, true}};
  for (const Slope& slope : slopes) {
    GroundCase c;
    c.name = slope.name;
    c.block = erg;
    c.climb_deg = slope.climb_deg;
    c.millimetre = true;
    // The tolerance, from what the RTX 5090 drew (2026-09-30), the same on every slope. By the
    // origin, 2 of 255 on the shaded picture and on each channel of the detail view: 1 measured
    // everywhere, at every look, once the reference clamps its point into the pixel's triangle as
    // the resolve does. 3.7 km out, 5 on the shaded picture, where a millimetre pixel resolves
    // the reconstruction's millimetre in the 5 mm and 2.5 mm octaves of the grain and its normal
    // (4 measured, on up to 898 of 25,600 pixels at the millimetre look, climbing into the wind;
    // the first pass's 3 at the feet is 3 here too). And 28 on the ripple height and the grain,
    // which the detail view shows raw at full contrast: 24 and 25 measured looking down the
    // 32-degree lee, whose upper part runs down the slope to grazing pixels 20 cm long, against the
    // first pass's 14 and 12 on level sand (the gradient grain's coarse octave, which the view
    // shows scaled to three of its root-mean-squares, changes faster with position than the first
    // pass's value noise did). The share is smooth: 1 measured.
    c.shaded[0] = 2;
    c.shaded[1] = 5;
    c.ripple[0] = 2;
    c.ripple[1] = 28;
    c.share[0] = 2;
    c.share[1] = 2;
    c.grain[0] = 2;
    c.grain[1] = 28;
    Vector<GroundView> views;
    draw_ground_case(device, c, views);
    u32 rippled = 0;
    u32 streaked = 0;
    for (const GroundView& v : views) {
      rippled += v.rippled;
      streaked += v.streaked;
    }
    // Each slope draws the terms it is there for, and not the ones it is not: the exposure takes
    // every ripple off the two steep lees, and the streaks start at 22 degrees.
    CHECK_MESSAGE((rippled > 0u) == slope.ripples, std::string(slope.name));
    CHECK_MESSAGE((streaked > 0u) == slope.streaks, std::string(slope.name));
  }
  device.destroy();
}

TEST_CASE("ground detail: the resolve draws the third pass's function on sloped sand") {
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;
  // The ergs' numbers as their scene has them, with the test's wind and a frame's motion: the
  // ripples 3.7 cm along the wind and travelling 2 mm a frame (a sixtieth of a wavelength: under
  // the shutter's fade, so the travel is a pure shift), the wind at its record's mean (nothing
  // flattened), and the avalanche clock a third of the way round a cycle, advancing a hundredth
  // of the lanes' share a frame. On ground that exercises each third-pass term:
  //   - level: the patches and the travel;
  //   - 12 degrees, its line 50 degrees off the wind: the steering, which turns the ripples along
  //     the contours only where the ground is oblique to the wind, with the patches and the travel;
  //   - the same slope with the storm's wind half way between the two flattening numbers: the
  //     ripples at half their share;
  //   - falling with the wind at 27 degrees, inside the grainflow's band (24 to 30), and at 32, a
  //     whole slip face: the lanes and their episodes, and no ripples.
  const gfx::GroundDetailDesc numbers = gref::erg_numbers();
  gfx::GroundDetailParams erg = gfx::ground_detail_block(numbers, Vec2{0.6f, -0.8f}, 7u);
  REQUIRE(erg.flags == (gfx::k_ground_ripples | gfx::k_ground_grain | gfx::k_ground_exposure |
                        gfx::k_ground_gradient_grain | gfx::k_ground_spacing |
                        gfx::k_ground_grainflow | gfx::k_ground_patches | gfx::k_ground_steering));
  const double turnover = static_cast<double>(numbers.flow_turnover);
  const double share = static_cast<double>(numbers.flow_share);
  const double moved = (17.0 + 1.0 / 3.0) / turnover;
  const double moved_step = 0.01 * share / turnover;
  gfx::GroundDetailParams storm = erg;
  gfx::ground_detail_motion(erg, numbers, 0.037, 0.002, 1.0f, moved, moved_step);
  gfx::ground_detail_motion(storm, numbers, 0.037, 0.002,
                            0.5f * (numbers.flatten_start + numbers.flatten_end), moved,
                            moved_step);
  REQUIRE((erg.flags & gfx::k_ground_motion) != 0u);
  REQUIRE(erg.ripple_live == 1.0f);
  REQUIRE(storm.ripple_live == doctest::Approx(0.5f));
  REQUIRE(erg.flow_clock == doctest::Approx(1.0 / 3.0).epsilon(1e-4));
  struct Slope {
    const char* name;
    const gfx::GroundDetailParams* block;
    double climb_deg;
    double turn_deg;
    bool ripples;
    bool lanes;
  };
  const Slope slopes[5] = {
      {"third pass, level", &erg, 0.0, 0.0, true, false},
      {"third pass, 12 degrees oblique to the wind", &erg, 12.0, 50.0, true, false},
      {"third pass, 12 degrees oblique to the wind in a storm", &storm, 12.0, 50.0, true, false},
      {"third pass, falling away from the wind at 27 degrees", &erg, -27.0, 0.0, false, true},
      {"third pass, falling away from the wind at 32 degrees", &erg, -32.0, 0.0, false, true}};
  for (const Slope& slope : slopes) {
    GroundCase c;
    c.name = slope.name;
    c.block = *slope.block;
    c.climb_deg = slope.climb_deg;
    c.turn_deg = slope.turn_deg;
    c.millimetre = true;
    // The second pass's tolerances, and what the RTX 5090 drew (2026-09-30): by the origin 1 on
    // the shaded picture at every look of every slope, the lanes included, and 2 at most on a
    // channel of the detail view; 3.7 km out 4 on the shaded picture (the millimetre look, as
    // the second pass) and 2 on a slip face. One number is wider, the raw ripple height 3.7 km
    // out: 40 for the second pass's 28. The patches shorten the wavelength to 0.7 of the scene's
    // at the least, so the reconstruction's millimetre is up to 1.43 times the phase it was, and
    // they spread the kernels further, which steepens the phase between them. Measured on the
    // 32-degree lee at the feet, where the view's top row is grazing pixels: 35 with the patches
    // (their scale 0.76 at that pixel) and 22 with the patches alone switched off, everything
    // else the same. No ripple is drawn on that slope (their share is 0 there, and the shaded
    // picture is within 1); the channel shows the height the kernels would have.
    c.shaded[0] = 2;
    c.shaded[1] = 5;
    c.ripple[0] = 2;
    c.ripple[1] = 40;
    c.share[0] = 2;
    c.share[1] = 2;
    c.grain[0] = 2;
    c.grain[1] = 28;
    Vector<GroundView> views;
    draw_ground_case(device, c, views);
    u32 rippled = 0;
    u32 flowed = 0;
    u32 streaked = 0;
    for (const GroundView& v : views) {
      rippled += v.rippled;
      flowed += v.flowed;
      streaked += v.streaked;
    }
    CHECK_MESSAGE((rippled > 0u) == slope.ripples, std::string(slope.name));
    CHECK_MESSAGE((flowed > 0u) == slope.lanes, std::string(slope.name));
    CHECK_MESSAGE(streaked == 0u, std::string(slope.name));
  }
  device.destroy();
}

TEST_CASE("ground detail: 420 km and 10,000 km out the resolve draws the origin's function") {
  gfx::Device device;
  if (!gfx_test::open_device(device)) return;
  if (!gfx_test::require(device, {gfx_test::Need::VisibilityBuffer})) return;
  // The owner flew the endless desert out to 420 km and found the ripples "very low resolution"
  // (gfx.md, "Far from the origin"): the pattern was taken at the point's float32 world coordinate,
  // whose step there is 3.1 cm, four to a ripple. Two places far out, both held to **the origin's
  // tolerances**, with the ergs' numbers as the third pass draws them (every term, a frame's
  // motion), level and on a 32-degree slip face for the lanes, at the three looks:
  //   - 420 km out, by the owner's spot, astride a boundary of the frame's grid (z = -66,048): the
  //     quad runs across it, and the walker's two eyes, at z + 0.4 and z + 6, are in the cells on
  //     either side of it, so two eyes in different frames look at the same sand;
  //   - 10,000 km out along x, where a float's step is a metre: the frame's origin and the eye are
  //     whole metres there, and the walker's "at the feet" targets round onto the metre too.
  // Each view is also drawn with the frame of the next cell along both axes, and held to its own.
  const gfx::GroundDetailDesc numbers = gref::erg_numbers();
  gfx::GroundDetailParams erg = gfx::ground_detail_block(numbers, Vec2{0.6f, -0.8f}, 7u);
  gfx::ground_detail_motion(
      erg, numbers, 0.037, 0.002, 1.0f,
      (17.0 + 1.0 / 3.0) / static_cast<double>(numbers.flow_turnover),
      0.01 * static_cast<double>(numbers.flow_share) / static_cast<double>(numbers.flow_turnover));
  const GroundCase::Site far_sites[2] = {
      {-419070.0f, -66051.0f, "420 km out, astride a frame boundary", 0},
      {10000000.0f, -2403.71f, "10,000 km out", 0}};
  // The frame's boundary lies between the two eyes of the 420 km site (frame corners are 1,024 m
  // apart, so their boundaries at odd multiples of 512 m).
  REQUIRE(std::floor((-66051.0 + 0.4) / 1024.0 + 0.5) !=
          std::floor((-66051.0 + 6.0) / 1024.0 + 0.5));
  struct Slope {
    const char* name;
    double climb_deg;
    bool ripples;
    bool lanes;
  };
  const Slope slopes[2] = {
      {"far out, level", 0.0, true, false},
      {"far out, falling away from the wind at 32 degrees", -32.0, false, true}};
  for (const Slope& slope : slopes) {
    GroundCase c;
    c.name = slope.name;
    c.block = erg;
    c.climb_deg = slope.climb_deg;
    c.millimetre = true;
    c.site_count = 2;
    c.sites[0] = far_sites[0];
    c.sites[1] = far_sites[1];
    c.shaded[0] = 2;
    c.ripple[0] = 2;
    c.share[0] = 2;
    c.grain[0] = 2;
    Vector<GroundView> views;
    draw_ground_case(device, c, views);
    u32 rippled = 0;
    u32 flowed = 0;
    for (const GroundView& v : views) {
      rippled += v.rippled;
      flowed += v.flowed;
    }
    CHECK_MESSAGE((rippled > 0u) == slope.ripples, std::string(slope.name));
    CHECK_MESSAGE((flowed > 0u) == slope.lanes, std::string(slope.name));
  }

  // The instrument, on what the resolve drew before the frame: the same level sand 420 km out with
  // every block left at the world's origin, which is the pattern at the point's float32 world
  // coordinate. Along the millimetre look's middle row (about 17 cm of sand at a millimetre a
  // pixel) the ripple height can change only where the coordinate does, once every 3.1 cm; the
  // reference's changes at almost every pixel. With the frame the two agree.
  GroundCase framed;
  framed.name = "420 km out with the frame";
  framed.block = erg;
  framed.millimetre = true;
  framed.site_count = 1;
  framed.sites[0] = far_sites[0];
  GroundCase bare = framed;
  bare.name = "420 km out without the frame (the pattern at float32 world coordinates)";
  bare.framed = false;
  bare.hold = false;
  Vector<GroundView> with;
  Vector<GroundView> without;
  draw_ground_case(device, framed, with);
  draw_ground_case(device, bare, without);
  REQUIRE(with.size() == 3u);
  REQUIRE(without.size() == 3u);
  const GroundView& a = with[2];
  const GroundView& b = without[2];
  MESSAGE("the millimetre look's middle row, "
          << a.row_metres << " m of sand: the reference's "
          << "ripple height changes " << a.ref_changes << " times; the GPU's " << a.gpu_changes
          << " with the frame and " << b.gpu_changes << " without it, where the float32 world "
          << "coordinate (x 3.1 cm a step, z 7.8 mm) changes " << b.coordinate_changes
          << " times; worst ripple height " << a.ripple << " of 255 with and " << b.ripple
          << " without");
  CHECK(gref::float_step(419070.0) == 0.03125);
  CHECK(gref::float_step(66051.0) == 0.0078125);
  // Without the frame the GPU's changes are bounded by the coordinate's.
  CHECK(b.gpu_changes <= b.coordinate_changes);
  CHECK(b.ref_changes > 4u * b.gpu_changes);
  // With it the GPU changes where the reference does, to the pixels where the two round apart.
  CHECK(a.gpu_changes * 10u >= a.ref_changes * 9u);
  device.destroy();
}

TEST_CASE("ground detail: the frame names the world's own lattices from any corner") {
  // `ground_detail_frame` without a device: for origins from the world's to 10,000 km and back,
  // each lattice's integers and offset put back together are the origin (to a nanometre), the
  // offset is inside its cell, and each lane direction's are the origin's own coordinates across
  // and along it. And the frame is a function of the cell the eye is in: two eyes in one cell get
  // the same block, and an eye half a cell over gets the next corner.
  gfx::GroundDetailParams d = gfx::ground_detail_block(gref::erg_numbers(), Vec2{0.6f, -0.8f}, 7u);
  const double eyes[][2] = {{0.0, 0.0},          {511.9, -511.9},     {512.1, -512.1},
                            {-1380.0, 0.0},      {2917.37, -2403.71}, {-419070.0, -66781.109375},
                            {-419070.0, -66051}, {1.0e7, -2403.71},   {-1.0e7, 1.0e7}};
  for (const auto& eye : eyes) {
    gfx::ground_detail_frame(d, eye[0], eye[1]);
    const double ox = static_cast<double>(d.origin_x);
    const double oz = static_cast<double>(d.origin_z);
    CHECK(std::fabs(ox - eye[0]) <= 512.0);
    CHECK(std::fabs(oz - eye[1]) <= 512.0);
    CHECK(std::fmod(ox, gfx::k_ground_frame_cell) == 0.0);
    CHECK(std::fmod(oz, gfx::k_ground_frame_cell) == 0.0);
    const auto lattice = [&](const gfx::GroundLattice& l, f32 size, const char* name) {
      const double s = static_cast<double>(size);
      // The integers are the low 32 bits of floor(origin / size); every lattice here has fewer
      // than 2^31 cells to 10,000 km, so they are the integer itself.
      const double x = static_cast<double>(l.base_x) * s + static_cast<double>(l.rem_x);
      const double z = static_cast<double>(l.base_z) * s + static_cast<double>(l.rem_z);
      CHECK_MESSAGE(std::fabs(x - ox) < 1e-6, name);
      CHECK_MESSAGE(std::fabs(z - oz) < 1e-6, name);
      CHECK_MESSAGE((l.rem_x >= 0.0f && l.rem_x < size), name);
      CHECK_MESSAGE((l.rem_z >= 0.0f && l.rem_z < size), name);
    };
    lattice(d.ripple_lattice, d.cell, "ripples");
    lattice(d.grain_lattice, d.grain_size, "grain");
    lattice(d.patch_lattice, d.patch_size, "patches");
    lattice(d.streak_lattice, d.streak_length, "streaks");
    for (u32 k = 0; k < gfx::k_ground_flow_directions; ++k) {
      const double angle = static_cast<double>(k) * (gref::k_two_pi / 16.0);
      const double across = ox * -std::sin(angle) + oz * std::cos(angle);
      const double along = ox * std::cos(angle) + oz * std::sin(angle);
      const double spacing = 2.0 * static_cast<double>(d.flow_width);
      const double seg = static_cast<double>(d.flow_length);
      const gfx::GroundLane& lane = d.lanes[k];
      CHECK(std::fabs(lane.lane * spacing + static_cast<double>(lane.across) - across) < 1e-6);
      CHECK(std::fabs(lane.segment * seg + static_cast<double>(lane.along) - along) < 1e-6);
    }
    // Every eye in the cell has this frame.
    gfx::GroundDetailParams same = d;
    gfx::ground_detail_frame(same, ox + 511.0, oz - 511.0);
    CHECK(std::memcmp(&same, &d, sizeof(d)) == 0);
  }
  // The world's origin, and every eye within half a cell of it, is the frame that changes nothing:
  // all zero, as a block nobody set a frame on.
  gfx::GroundDetailParams zero = d;
  gfx::ground_detail_frame(zero, -300.0, 200.0);
  const gfx::GroundDetailParams none =
      gfx::ground_detail_block(gref::erg_numbers(), Vec2{0.6f, -0.8f}, 7u);
  CHECK(std::memcmp(&zero, &none, sizeof(none)) == 0);
}

TEST_CASE("ground detail: the grain reads as sand") {
  // The constants the grain is scaled by are the noise's own, measured over the mirror; and the
  // height channel's analytic gradient is the numeric one.
  double v = 0.0, g = 0.0, gradient_off = 0.0;
  u32 n = 0;
  for (u32 j = 0; j < 400; ++j) {
    for (u32 i = 0; i < 400; ++i) {
      const double x = 0.0137 * i + 0.3;
      const double z = 0.0113 * j - 2.0;
      const gref::Grain3 a = gref::gradient_noise3(x, z, 0.1, 7u, 16u);
      for (const double c : a.value)
        v += c * c;
      g += a.gx * a.gx + a.gz * a.gz;
      ++n;
      const gref::Grain3 b = gref::gradient_noise3(x + 1e-6, z, 0.1, 7u, 16u);
      gradient_off = std::max(gradient_off, std::fabs((b.value[2] - a.value[2]) / 1e-5 - a.gx));
    }
  }
  const double rms = std::sqrt(v / (3.0 * n));
  const double slope_rms = std::sqrt(g / n);
  MESSAGE("gradient noise: rms " << rms << ", slope rms " << slope_rms << " per cell");
  CHECK(std::fabs(rms / static_cast<double>(gfx::k_ground_noise_rms) - 1.0) < 0.03);
  CHECK(std::fabs(slope_rms / static_cast<double>(gfx::k_ground_noise_slope_rms) - 1.0) < 0.03);
  CHECK(gradient_off < 1e-3);
  CHECK(gfx::ground_grain_octaves(0.02f, 0.001f) == 5u);  // 20, 10, 5, 2.5, 1.25 mm
  CHECK(gfx::ground_grain_octaves(0.02f, 0.0f) == 0u);
  CHECK(gfx::ground_grain_octaves(0.02f, 0.02f) == 1u);
  CHECK(gfx::ground_grain_octaves(0.2f, 0.0002f) == gfx::k_ground_max_grain_octaves);

  // Precision far from the origin: the resolve's position is a float, whose step 3.7 km out is a
  // quarter of a millimetre. Each octave evaluated at positions rounded to float against the
  // exact ones, as a share of its own root-mean-square; an octave the fade keeps at more than
  // half its weight there must hold to a sixth.
  gfx::GroundDetailDesc desc;
  desc.grain_finest = 0.001f;
  desc.grain_normal = 0.06f;
  const gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  REQUIRE(d.grain_octaves == 5u);
  for (const double far : {900.0, 3700.0}) {
    double size = static_cast<double>(d.grain_size);
    for (u32 o = 0; o < d.grain_octaves; ++o, size *= 0.5) {
      double err = 0.0;
      u32 count = 0;
      for (u32 i = 0; i < 20000; ++i) {
        const double x = far + 0.000037 * i;
        const double z = -0.5 * far + 0.000053 * i;
        const double fx = static_cast<double>(static_cast<f32>(x));
        const double fz = static_cast<double>(static_cast<f32>(z));
        const gref::Grain3 exact = gref::gradient_noise3(x, z, size, 7u, 16u + o);
        const gref::Grain3 rounded = gref::gradient_noise3(fx, fz, size, 7u, 16u + o);
        err += (exact.value[0] - rounded.value[0]) * (exact.value[0] - rounded.value[0]);
        ++count;
      }
      const double relative = std::sqrt(err / count) / static_cast<double>(gfx::k_ground_noise_rms);
      const double step = gref::float_step(far);
      const double weight = gref::fade(4.0 * step / size);
      MESSAGE(far << " m out, octave " << size * 1000.0 << " mm: error " << relative
                  << " of its rms, kept at " << weight);
      if (weight > 0.5) CHECK(relative < 1.0 / 6.0);
    }
  }
}

TEST_CASE("ground detail: the grain's filter keeps the mean and adds no sparkle") {
  // The pull-back's limits in a CPU form: sand seen from above at a pixel of 2 mm, 5 mm, 1 cm and
  // 3 cm, each pixel shaded once with its footprint against the mean of 8 x 8 samples inside it
  // with an eighth of it. The filtered picture's mean brightness is the supersampled one's (the
  // rougher-not-flatter rule), and its pixel-to-pixel variation is not above the supersampled
  // picture's (a term that should have faded and did not shows as sparkle).
  gfx::GroundDetailDesc desc;
  desc.ripple_height = 0.0f;  // the grain alone; the ripples' rule has its own test
  desc.grain_finest = 0.001f;
  desc.grain_normal = 0.06f;
  const gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  const ref::Dvec3 albedo{0.62, 0.47, 0.32};
  const ref::Dvec3 up{0.0, 1.0, 0.0};
  const ref::Dvec3 sun = ref::normalize(ref::Dvec3{-0.94, 0.342, 0.0});
  const auto luma = [&](ref::Dvec3 p, double step) {
    const ref::Dvec3 dx{step, 0.0, 0.0};
    const ref::Dvec3 dz{0.0, 0.0, step};
    const gref::Shading g = gref::shade(d, p, up, dx, dz, 1.0, albedo, 0.85);
    ref::Surface s;
    s.position = p;
    s.normal = g.normal;
    s.view = ref::normalize(ref::Dvec3{0.3, 1.0, 0.1});
    s.albedo = g.albedo;
    s.roughness = g.roughness;
    return ref::luminance(
        ref::shade(s, sun, 4.5, ref::Dvec3{0.45, 0.55, 0.75}, albedo, nullptr, 0, ref::Dvec3{}));
  };
  for (const double pixel : {0.002, 0.005, 0.01, 0.03}) {
    constexpr u32 k_side = 40;
    constexpr u32 k_sub = 8;
    double sum_f = 0.0, sum_s = 0.0, sq_f = 0.0, sq_s = 0.0;
    for (u32 j = 0; j < k_side; ++j) {
      for (u32 i = 0; i < k_side; ++i) {
        const double x0 = 11.0 + i * pixel;
        const double z0 = -3.0 + j * pixel;
        const double f = luma(ref::Dvec3{x0 + 0.5 * pixel, 0.0, z0 + 0.5 * pixel}, pixel);
        double s = 0.0;
        for (u32 b = 0; b < k_sub; ++b) {
          for (u32 a = 0; a < k_sub; ++a) {
            s += luma(
                ref::Dvec3{x0 + (a + 0.5) * pixel / k_sub, 0.0, z0 + (b + 0.5) * pixel / k_sub},
                pixel / k_sub);
          }
        }
        s /= k_sub * k_sub;
        sum_f += f;
        sum_s += s;
        sq_f += f * f;
        sq_s += s * s;
      }
    }
    const double n = k_side * k_side;
    const double mean_f = sum_f / n, mean_s = sum_s / n;
    const double sd_f = std::sqrt(std::max(sq_f / n - mean_f * mean_f, 0.0));
    const double sd_s = std::sqrt(std::max(sq_s / n - mean_s * mean_s, 0.0));
    MESSAGE("pixel " << pixel * 1000.0 << " mm: mean " << mean_f << " against " << mean_s
                     << " supersampled, variation " << sd_f << " against " << sd_s);
    CHECK(std::fabs(mean_f / mean_s - 1.0) < 0.01);
    CHECK(sd_f <= sd_s * 1.25 + 1e-4);
  }
}

TEST_CASE("ground detail: streaks run down a slip face and nowhere else") {
  gfx::GroundDetailDesc desc;
  desc.streak_start_deg = 22.0f;
  desc.streak_full_deg = 30.0f;
  const gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  REQUIRE((d.flags & gfx::k_ground_streaks) != 0u);
  const double deg = 3.14159265358979323846 / 180.0;
  const auto normal = [deg](double slope, double azimuth) {
    return ref::Dvec3{std::sin(slope * deg) * std::cos(azimuth * deg), std::cos(slope * deg),
                      std::sin(slope * deg) * std::sin(azimuth * deg)};
  };
  const ref::Dvec3 none{};
  const ref::Dvec3 albedo{0.6, 0.5, 0.4};
  const auto weight = [&](ref::Dvec3 n) {
    return gref::shade(d, ref::Dvec3{3.0, 0.0, 1.0}, n, none, none, 1.0, albedo, 0.85).streak;
  };
  CHECK(weight(normal(0.0, 0.0)) == 0.0);
  CHECK(weight(normal(33.0, 180.0)) == 0.0);  // windward, however steep
  CHECK(weight(normal(18.0, 0.0)) == 0.0);    // a gentle lee: smooth sand, neither
  CHECK(weight(normal(32.0, 0.0)) == 1.0);    // a slip face
  CHECK(weight(normal(26.0, 0.0)) > 0.0);
  CHECK(weight(normal(26.0, 0.0)) < 1.0);
  // No line anywhere: along a sweep of the lee slope from flat to past the angle of repose, every
  // weight — the ripples' and the streaks' — and the shaded albedo change smoothly.
  double largest_step = 0.0;
  gfx::GroundDetailDesc all = desc;
  all.lee_start_deg = 10.0f;
  all.lee_end_deg = 18.0f;
  const gfx::GroundDetailParams e = gfx::ground_detail_block(all, Vec2{1.0f, 0.0f}, 7u);
  gref::Shading previous =
      gref::shade(e, ref::Dvec3{3.0, 0.0, 1.0}, normal(0.0, 0.0), none, none, 1.0, albedo, 0.85);
  for (u32 i = 1; i <= 4000; ++i) {
    const gref::Shading g = gref::shade(e, ref::Dvec3{3.0, 0.0, 1.0}, normal(i * 0.01, 0.0), none,
                                        none, 1.0, albedo, 0.85);
    largest_step = std::max({largest_step, std::fabs(g.weight - previous.weight),
                             std::fabs(g.streak - previous.streak),
                             std::fabs(g.albedo.x - previous.albedo.x)});
    previous = g;
  }
  CHECK(largest_step < 0.005);

  // The block scales the tongues' sum to unit root-mean-square and its gradient to
  // `streak_normal`: measured over a slip face's worth of points.
  double v = 0.0, g2 = 0.0;
  u32 count = 0;
  for (u32 j = 0; j < 300; ++j) {
    for (u32 i = 0; i < 300; ++i) {
      const gref::Streak st = gref::streaks(d, 0.047 * i - 3.0, 0.053 * j + 1.0, 0.6, 0.8);
      v += st.value * st.value;
      g2 += st.gx * st.gx + st.gz * st.gz;
      ++count;
    }
  }
  const double rms = std::sqrt(v / count) * static_cast<double>(d.streak_albedo) /
                     static_cast<double>(desc.streak_albedo);
  const double slope_rms = std::sqrt(g2 / count) * static_cast<double>(d.streak_slope);
  MESSAGE("streaks: unit sum's rms " << rms << ", slope rms " << slope_rms << " against "
                                     << desc.streak_normal);
  CHECK(std::fabs(rms - 1.0) < 0.1);
  CHECK(std::fabs(slope_rms / static_cast<double>(desc.streak_normal) - 1.0) < 0.1);

  // The trap: the fall line depends on the normal, and only an offset from a kernel's centre is
  // projected on it, so turning the normal by a thousandth of a radian 3 km from the origin moves
  // the pattern by what a thousandth of a radian moves a point a metre from a kernel's centre —
  // not by what it would move a point 3 km from the origin.
  double moved = 0.0;
  const double turn = 0.001;
  for (u32 i = 0; i < 2000; ++i) {
    const double x = 3000.0 + 0.0137 * i;
    const double z = -1500.0 + 0.0091 * i;
    const gref::Streak a = gref::streaks(d, x, z, 1.0, 0.0);
    const gref::Streak b = gref::streaks(d, x, z, std::cos(turn), std::sin(turn));
    moved = std::max(moved, std::fabs(a.value - b.value));
  }
  const double sum_rms = std::sqrt(v / count);
  MESSAGE("a thousandth of a radian of normal 3 km out moves the tongues' sum by at most "
          << moved / sum_rms << " of its rms");
  CHECK(moved < 0.05 * sum_rms);
}

TEST_CASE("ground detail: the streaks' filter keeps the mean and adds no sparkle") {
  gfx::GroundDetailDesc desc;
  desc.ripple_height = 0.0f;
  desc.grain_albedo = 0.0f;
  desc.grain_roughness = 0.0f;
  desc.streak_start_deg = 22.0f;
  desc.streak_full_deg = 30.0f;
  const gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  const double slope = 32.0 * 3.14159265358979323846 / 180.0;
  const ref::Dvec3 n{std::sin(slope), std::cos(slope), 0.0};
  const ref::Dvec3 albedo{0.62, 0.47, 0.32};
  const ref::Dvec3 sun = ref::normalize(ref::Dvec3{0.2, 0.5, 0.8});
  const auto luma = [&](double x, double z, double step) {
    const double y = -(n.x * x + n.z * z) / n.y;
    const ref::Dvec3 p{x, y, z};
    const ref::Dvec3 dx{step, -n.x / n.y * step, 0.0};
    const ref::Dvec3 dz{0.0, -n.z / n.y * step, step};
    const gref::Shading g = gref::shade(d, p, n, dx, dz, 1.0, albedo, 0.85);
    ref::Surface s;
    s.position = p;
    s.normal = g.normal;
    s.view = ref::normalize(ref::Dvec3{0.8, 0.6, 0.1});
    s.albedo = g.albedo;
    s.roughness = g.roughness;
    return ref::luminance(
        ref::shade(s, sun, 4.5, ref::Dvec3{0.45, 0.55, 0.75}, albedo, nullptr, 0, ref::Dvec3{}));
  };
  for (const double pixel : {0.05, 0.1, 0.2, 0.4}) {
    constexpr u32 k_side = 32;
    constexpr u32 k_sub = 8;
    double sum_f = 0.0, sum_s = 0.0, sq_f = 0.0, sq_s = 0.0;
    for (u32 j = 0; j < k_side; ++j) {
      for (u32 i = 0; i < k_side; ++i) {
        const double x0 = 40.0 + i * pixel;
        const double z0 = 7.0 + j * pixel;
        const double f = luma(x0 + 0.5 * pixel, z0 + 0.5 * pixel, pixel);
        double s = 0.0;
        for (u32 b = 0; b < k_sub; ++b)
          for (u32 a = 0; a < k_sub; ++a)
            s +=
                luma(x0 + (a + 0.5) * pixel / k_sub, z0 + (b + 0.5) * pixel / k_sub, pixel / k_sub);
        s /= k_sub * k_sub;
        sum_f += f;
        sum_s += s;
        sq_f += f * f;
        sq_s += s * s;
      }
    }
    const double count = k_side * k_side;
    const double mean_f = sum_f / count, mean_s = sum_s / count;
    const double sd_f = std::sqrt(std::max(sq_f / count - mean_f * mean_f, 0.0));
    const double sd_s = std::sqrt(std::max(sq_s / count - mean_s * mean_s, 0.0));
    MESSAGE("slip face, pixel " << pixel * 100.0 << " cm: mean " << mean_f << " against " << mean_s
                                << ", variation " << sd_f << " against " << sd_s);
    CHECK(std::fabs(mean_f / mean_s - 1.0) < 0.01);
    CHECK(sd_f <= sd_s * 1.25 + 1e-4);
  }
}

TEST_CASE("ground detail: the ripples' spacing follows the wind, without a jump or a slide") {
  gfx::GroundDetailDesc desc;
  desc.spacing_gain = 2.5f;
  const gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  REQUIRE((d.flags & gfx::k_ground_spacing) != 0u);
  const double deg = 3.14159265358979323846 / 180.0;
  const auto windward = [deg](double slope) {
    return ref::Dvec3{-std::sin(slope * deg), std::cos(slope * deg), 0.0};
  };
  const ref::Dvec3 none{};
  const ref::Dvec3 albedo{0.6, 0.5, 0.4};
  // The wavelength the pattern has: zero crossings of the height along the wind over 40 m, two a
  // wavelength, at the flat's scale and at a 12-degree windward slope's (1 + 2.5 tan 12 = 1.53).
  const auto crossings = [&](double scale) {
    u32 count = 0;
    double previous = gref::ripple(d, 0.0, 0.7, 0.5, scale).height;
    for (u32 i = 1; i <= 80000; ++i) {
      const double h = gref::ripple(d, 0.0005 * i, 0.7, 0.5, scale).height;
      if ((h > 0.0) != (previous > 0.0)) ++count;
      previous = h;
    }
    return count;
  };
  const double s12 = std::min(1.0 + 2.5 * std::tan(12.0 * deg), 1.6);
  const u32 at_flat = crossings(1.0);
  const u32 at_slope = crossings(s12);
  MESSAGE("zero crossings over 40 m: " << at_flat << " on the flat, " << at_slope << " at scale "
                                       << s12);
  CHECK(static_cast<double>(at_flat) / at_slope == doctest::Approx(s12).epsilon(0.12));

  // Scale on the windward slope, read back from shade(): the ripple's height over its own
  // amplitude stays in [-1, 1] however the spacing scales it.
  double largest = 0.0;
  for (u32 i = 0; i < 2000; ++i) {
    const gref::Shading g = gref::shade(d, ref::Dvec3{0.013 * i, 0.0, 0.3}, windward(12.0), none,
                                        none, 1.0, albedo, 0.85, true);
    largest = std::max(largest, std::fabs(g.ripple));
  }
  CHECK(largest <= 1.0 + 1e-9);

  // No jump where the wavelength varies: over ground whose windward slope rises from 0 to 14
  // degrees across 12 m — so the scale climbs from 1 to its clamp across dozens of lattice cells —
  // the ripple's height at the scene's asymmetry never jumps between half-millimetre steps by
  // more than its slope allows, as on the flat.
  double previous_h = 0.0;
  u32 jumps = 0;
  for (u32 i = 0; i <= 24000; ++i) {
    const double x = -2.0 + 0.0005 * i;
    const double angle = std::clamp((x + 2.0) / 12.0, 0.0, 1.0) * 14.0;
    const gref::Shading g = gref::shade(d, ref::Dvec3{x, 0.0, 0.43}, windward(angle), none, none,
                                        1.0, albedo, 0.85, true);
    const double h = g.ripple * static_cast<double>(d.amplitude);
    // The steepest honest change over a step: the ripple's slope bound, 2 pi a / (w l) at the
    // scene's asymmetry, with the scale's own change riding along.
    const double bound = 2.0 * 3.14159265358979323846 * static_cast<double>(d.amplitude) /
                         (0.25 * static_cast<double>(d.wavelength)) * 0.0005;
    if (i > 0 && std::fabs(h - previous_h) > 2.0 * bound + 1e-6) ++jumps;
    previous_h = h;
  }
  CHECK(jumps == 0);

  // The slide: a tenth of a degree of normal on a 12-degree windward slope moves the phase at a
  // kernel's edge by R dk; bounded analytically (renderer.md) at 27.8 gain sec^2 / s radians per
  // radian, and measured on the mirror as the crests' displacement over 4,000 points where the sum
  // stands above the taper: under a twentieth of a wavelength everywhere.
  const double tenth = 0.1;
  const double s0 = std::min(1.0 + 2.5 * std::tan(12.0 * deg), 1.6);
  const double s1 = std::min(1.0 + 2.5 * std::tan((12.0 + tenth) * deg), 1.6);
  const double k0 = 2.0 * 3.14159265358979323846 / (static_cast<double>(d.wavelength) * s0);
  const double k1 = 2.0 * 3.14159265358979323846 / (static_cast<double>(d.wavelength) * s1);
  const double bound =
      static_cast<double>(d.cell) * std::fabs(k1 - k0) / (2.0 * 3.14159265358979323846);
  double worst = 0.0;
  for (u32 i = 0; i < 4000; ++i) {
    const double x = 0.0371 * i;
    const double z = 0.0213 * i - 3.0;
    const gref::Ripple a = gref::ripple(d, x, z, 0.5, s0);
    const gref::Ripple b = gref::ripple(d, x, z, 0.5, s1);
    // The crest's displacement is the change of the sum's phase, in turns, which is wavelengths;
    // read where the sum stands clear of the taper's floor (a defect's core has no crest to move).
    if (a.taper < 0.5 || b.taper < 0.5) continue;
    double shift = std::fabs(b.turn - a.turn);
    if (shift > 0.5) shift = 1.0 - shift;
    worst = std::max(worst, shift);
  }
  MESSAGE("a tenth of a degree of normal on a 12-degree windward slope: bound "
          << bound << " wavelengths at a kernel's edge, measured " << worst);
  CHECK(bound < 0.05);
  CHECK(worst < 0.05);
}

TEST_CASE("ground detail: the third pass's defaults are the second pass, to the bit") {
  // A scene that names none of the third pass's numbers draws what the second pass drew: the live
  // mirror against the frozen one (ground_detail_reference_v2.h), with the ergs' numbers as the
  // second pass shipped them and with the defaults, at every probe.
  const ref::Dvec3 albedo{0.62, 0.47, 0.32};
  for (u32 variant = 0; variant < 2; ++variant) {
    gfx::GroundDetailDesc desc;
    if (variant == 1) {
      desc.lee_start_deg = 10.0f;
      desc.lee_end_deg = 18.0f;
      desc.grain_finest = 0.001f;
      desc.grain_normal = 0.06f;
      desc.streak_start_deg = 22.0f;
      desc.streak_full_deg = 30.0f;
      desc.spacing_gain = 2.5f;
    }
    const gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{0.6f, -0.8f}, 7u);
    u32 differ = 0;
    for (u32 i = 0; i < 4000; ++i) {
      const Probe p = probe(i);
      const gref::Shading a =
          gref::shade(d, p.position, p.normal, p.dpdx, p.dpdy, p.mask, albedo, 0.85, p.full);
      const engine::ground_ref_v2::Shading b = engine::ground_ref_v2::shade(
          d, p.position, p.normal, p.dpdx, p.dpdy, p.mask, albedo, 0.85, p.full);
      const bool same = a.normal.x == b.normal.x && a.normal.y == b.normal.y &&
                        a.normal.z == b.normal.z && a.albedo.x == b.albedo.x &&
                        a.albedo.y == b.albedo.y && a.albedo.z == b.albedo.z &&
                        a.roughness == b.roughness && a.weight == b.weight && a.fade == b.fade &&
                        a.ripple == b.ripple && a.grain == b.grain && a.streak == b.streak;
      if (!same) ++differ;
    }
    CHECK(differ == 0);
  }
}

TEST_CASE("ground detail: grainflow lanes are long, run down the fall line, and are scaled") {
  gfx::GroundDetailDesc desc;
  desc.flow_start_deg = 24.0f;
  desc.flow_full_deg = 30.0f;
  const gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  REQUIRE((d.flags & gfx::k_ground_grainflow) != 0u);
  // The constants the block scales by are the lanes' own, over a slip face's worth of points.
  double v = 0.0, g2 = 0.0;
  u32 count = 0;
  for (u32 j = 0; j < 300; ++j) {
    for (u32 i = 0; i < 300; ++i) {
      const gref::Flow f = gref::grainflow(d, 0.37 * i - 50.0, 0.29 * j + 3.0, 0.6, 0.8);
      v += f.value * f.value;
      g2 += (f.gx * f.gx + f.gz * f.gz) * static_cast<double>(d.flow_width * d.flow_width);
      ++count;
    }
  }
  const double rms = std::sqrt(v / count);
  const double slope_rms = std::sqrt(g2 / count);
  MESSAGE("grainflow: value rms " << rms << ", slope rms " << slope_rms << " per lane width");
  CHECK(std::fabs(rms / static_cast<double>(gfx::k_ground_flow_rms) - 1.0) < 0.1);
  CHECK(std::fabs(slope_rms / static_cast<double>(gfx::k_ground_flow_slope_rms) - 1.0) < 0.1);

  // Long: along the fall line the lanes change slowly and across it fast — the value's gradient
  // along the fall line is under a fifth of its gradient across, on average.
  double along = 0.0, across = 0.0;
  for (u32 i = 0; i < 4000; ++i) {
    const gref::Flow f = gref::grainflow(d, 0.113 * i - 200.0, 0.071 * i + 3.0, 0.6, 0.8);
    along += std::fabs(f.gx * 0.6 + f.gz * 0.8);
    across += std::fabs(-f.gx * 0.8 + f.gz * 0.6);
  }
  MESSAGE("grainflow: mean gradient along the fall line " << along / 4000.0 << ", across "
                                                          << across / 4000.0);
  CHECK(along < 0.2 * across);

  // No seam: along lines crossing many cells, lanes and windows, the value never jumps between
  // millimetre steps by more than its gradient allows.
  u32 jumps = 0;
  for (u32 line = 0; line < 4; ++line) {
    gref::Flow before = gref::grainflow(d, -30.0, 1.7 + 3.1 * line, 0.6, 0.8);
    for (u32 i = 1; i <= 60000; ++i) {
      const double x = -30.0 + 0.001 * i;
      const gref::Flow f = gref::grainflow(d, x, 1.7 + 3.1 * line + 0.0003 * i, 0.6, 0.8);
      // A step's change is its mean slope over it: twice the larger end's slope bounds it, as the
      // ripples' continuity test has it.
      const double slope = std::max(std::hypot(f.gx, f.gz), std::hypot(before.gx, before.gz));
      if (std::fabs(f.value - before.value) > 2.0 * slope * 0.0011 + 1e-4) ++jumps;
      before = f;
    }
  }
  CHECK(jumps == 0);

  // The trap: the fall line depends on the normal and only offsets from a cell's centre are
  // projected on it, so a thousandth of a radian of normal 3 km out moves the lanes by what it
  // moves a point a cell from a centre.
  double moved = 0.0;
  const double turn = 0.001;
  for (u32 i = 0; i < 2000; ++i) {
    const double x = 3000.0 + 0.0137 * i;
    const double z = -1500.0 + 0.0091 * i;
    // Mid-sector (11.25 degrees), where the blend's weight turns fastest with the normal.
    const double mid = 11.25 * 3.14159265358979323846 / 180.0;
    const gref::Flow a = gref::grainflow(d, x, z, std::cos(mid), std::sin(mid));
    const gref::Flow b = gref::grainflow(d, x, z, std::cos(mid + turn), std::sin(mid + turn));
    moved = std::max(moved, std::fabs(a.value - b.value));
  }
  MESSAGE("a thousandth of a radian of normal 3 km out moves the lanes by at most "
          << moved / static_cast<double>(gfx::k_ground_flow_rms) << " of their rms");
  CHECK(moved < 0.35 * static_cast<double>(gfx::k_ground_flow_rms));

  // The filter keeps the mean: a 32-degree face shaded with the lanes against 8 x 8 supersampling
  // at pixels of 5 cm to 40 cm.
  gfx::GroundDetailDesc only = desc;
  only.ripple_height = 0.0f;
  only.grain_albedo = 0.0f;
  only.grain_roughness = 0.0f;
  const gfx::GroundDetailParams e = gfx::ground_detail_block(only, Vec2{1.0f, 0.0f}, 7u);
  const double slope = 32.0 * 3.14159265358979323846 / 180.0;
  const ref::Dvec3 n{std::sin(slope), std::cos(slope), 0.0};
  const ref::Dvec3 albedo{0.62, 0.47, 0.32};
  const ref::Dvec3 sun = ref::normalize(ref::Dvec3{0.1, 0.35, 0.93});
  const auto luma = [&](double x, double z, double step) {
    const ref::Dvec3 p{x, -(n.x * x) / n.y, z};
    const ref::Dvec3 dx{step, -n.x / n.y * step, 0.0};
    const ref::Dvec3 dz{0.0, 0.0, step};
    const gref::Shading g = gref::shade(e, p, n, dx, dz, 1.0, albedo, 0.85);
    ref::Surface sf;
    sf.position = p;
    sf.normal = g.normal;
    sf.view = ref::normalize(ref::Dvec3{0.8, 0.6, 0.1});
    sf.albedo = g.albedo;
    sf.roughness = g.roughness;
    return ref::luminance(
        ref::shade(sf, sun, 4.5, ref::Dvec3{0.45, 0.55, 0.75}, albedo, nullptr, 0, ref::Dvec3{}));
  };
  for (const double pixel : {0.05, 0.15, 0.4}) {
    double sum_f = 0.0, sum_s = 0.0;
    for (u32 j = 0; j < 24; ++j) {
      for (u32 i = 0; i < 24; ++i) {
        const double x0 = 40.0 + i * pixel;
        const double z0 = 7.0 + j * pixel;
        sum_f += luma(x0 + 0.5 * pixel, z0 + 0.5 * pixel, pixel);
        double s = 0.0;
        for (u32 b = 0; b < 8; ++b)
          for (u32 a = 0; a < 8; ++a)
            s += luma(x0 + (a + 0.5) * pixel / 8.0, z0 + (b + 0.5) * pixel / 8.0, pixel / 8.0);
        sum_s += s / 64.0;
      }
    }
    MESSAGE("grainflow at a pixel of " << pixel * 100.0 << " cm: mean " << sum_f / 576.0
                                       << " against " << sum_s / 576.0 << " supersampled");
    CHECK(std::fabs(sum_f / sum_s - 1.0) < 0.01);
  }
}

TEST_CASE("ground detail: ripples in patches, steered by the ground, without a jump or a slide") {
  gfx::GroundDetailDesc desc;
  desc.patch_size = 30.0f;
  desc.steer_max_deg = 20.0f;
  const gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  REQUIRE((d.flags & gfx::k_ground_patches) != 0u);
  REQUIRE((d.flags & gfx::k_ground_steering) != 0u);
  // The patches span their range: over a kilometre of samples the wavelength's scale reaches near
  // both ends of [0.7, 1.4], and its mean sits near the middle.
  double lo = 10.0, hi = 0.0, sum = 0.0;
  for (u32 i = 0; i < 20000; ++i) {
    double v0 = 0.0, v1 = 0.0;
    gref::patch(d, 0.37 * i - 3000.0, 0.113 * i + 11.0, v0, v1);
    const double scale =
        static_cast<double>(d.patch_mid) * std::exp2(v0 * static_cast<double>(d.patch_half_log2));
    lo = std::min(lo, scale);
    hi = std::max(hi, scale);
    sum += scale;
  }
  MESSAGE("patches: the wavelength's scale from " << lo << " to " << hi << ", mean "
                                                  << sum / 20000.0);
  CHECK(lo < 0.8);
  CHECK(hi > 1.25);
  CHECK(std::fabs(sum / 20000.0 - 1.0) < 0.1);

  // Steering: nothing on the flat or straight up the wind's own slope; along the contours on a
  // slope across it, at most the clamp.
  const double deg = 3.14159265358979323846 / 180.0;
  const auto normal = [deg](double slope, double azimuth) {
    return ref::Dvec3{std::sin(slope * deg) * std::cos(azimuth * deg), std::cos(slope * deg),
                      std::sin(slope * deg) * std::sin(azimuth * deg)};
  };
  const auto turn = [&](ref::Dvec3 n) {
    double wx = 1.0, wz = 0.0;
    gref::steer(d, wx, wz, n);
    return std::atan2(wz, wx) / deg;
  };
  CHECK(turn(normal(0.0, 0.0)) == 0.0);
  CHECK(std::fabs(turn(normal(15.0, 180.0))) < 1e-9);
  CHECK(std::fabs(turn(normal(15.0, 135.0))) > 3.0);
  double largest = 0.0, previous = turn(normal(25.0, 0.0)), step = 0.0;
  for (u32 i = 1; i <= 3600; ++i) {
    const double t = turn(normal(25.0, i * 0.1));
    largest = std::max(largest, std::fabs(t));
    step = std::max(step, std::fabs(t - previous));
    previous = t;
  }
  MESSAGE("steering at 25 degrees round the compass: at most " << largest << " degrees, " << step
                                                               << " per 0.1 degree");
  CHECK(largest <= 20.0 + 1e-6);
  CHECK(step < 1.0);

  // No jump: the shaded ripple's height along lines crossing patches, over ground whose slope
  // swings across the wind, never changes between half-millimetre steps by more than its slope
  // allows.
  const ref::Dvec3 none{};
  const ref::Dvec3 albedo{0.6, 0.5, 0.4};
  u32 jumps = 0;
  for (u32 line = 0; line < 3; ++line) {
    double previous_h = 0.0;
    for (u32 i = 0; i <= 40000; ++i) {
      const double x = -5.0 + 0.0005 * i;
      const double az = 90.0 + 60.0 * std::sin(x * 0.3);
      const gref::Shading g = gref::shade(d, ref::Dvec3{x, 0.0, 17.0 * line + 0.4},
                                          normal(12.0, az), none, none, 1.0, albedo, 0.85, true);
      const double h = g.ripple * static_cast<double>(d.amplitude) * 1.4;
      const double bound = 2.0 * 3.14159265358979323846 * static_cast<double>(d.amplitude) * 1.4 /
                           (0.25 * static_cast<double>(d.wavelength) * 0.7) * 0.0005;
      if (i > 0 && std::fabs(h - previous_h) > 2.0 * bound + 1e-6) ++jumps;
      previous_h = h;
    }
  }
  CHECK(jumps == 0);

  // The slide steering costs: a tenth of a degree of turn moves each kernel's phase at its edge by
  // R k dtheta; measured as the crests' shift where the sum stands clear of its taper.
  double worst = 0.0;
  const double dtheta = 0.1 * deg;
  for (u32 i = 0; i < 4000; ++i) {
    const double x = 0.0371 * i;
    const double z = 0.0213 * i - 3.0;
    const gref::Ripple a =
        gref::ripple(d, x, z, 0.5, 1.0, 1.0, 0.0, static_cast<double>(d.wind.z), 0.0);
    const gref::Ripple b = gref::ripple(d, x, z, 0.5, 1.0, std::cos(dtheta), std::sin(dtheta),
                                        static_cast<double>(d.wind.z), 0.0);
    if (a.taper < 0.5 || b.taper < 0.5) continue;
    double shift = std::fabs(b.turn - a.turn);
    if (shift > 0.5) shift = 1.0 - shift;
    worst = std::max(worst, shift);
  }
  MESSAGE("a tenth of a degree of steering moves the crests at most " << worst << " wavelengths");
  CHECK(worst < 0.05);
}

TEST_CASE("ground detail: ripples travel, blur under a time-lapse and flatten in a storm") {
  gfx::GroundDetailDesc desc;
  desc.ripple_defects = 0.0f;
  desc.ripple_celerity = 10.0f;
  desc.flatten_start = 1.8f;
  desc.flatten_end = 2.4f;
  gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  d.wind.z = 0.0f;  // kernels exactly along the wind, so a travel is exactly a shift
  const double wavelength = static_cast<double>(d.wavelength);
  // Travel: the pattern at travel s is the pattern at rest moved s along the wind.
  double worst = 0.0;
  for (u32 i = 0; i < 2000; ++i) {
    const double x = 0.0137 * i - 5.0;
    const double z = 0.0071 * i + 1.0;
    const gref::Ripple a = gref::ripple(d, x, z, 0.75, 1.0, 1.0, 0.0, 0.0, 0.037);
    // The kernels stay where the lattice put them, so only their phases move: compare phases.
    const gref::Ripple c = gref::ripple(d, x, z, 0.75, 1.0, 1.0, 0.0, 0.0, 0.0);
    double shift = a.turn - c.turn;
    shift -= std::floor(shift + 0.5);
    if (a.taper > 0.5) worst = std::max(worst, std::fabs(std::fabs(shift) - 0.037 / wavelength));
  }
  MESSAGE("a travel of 3.7 cm moves the phase by 3.7 cm to within " << worst << " wavelengths");
  CHECK(worst < 1e-9);

  // The block: the travel reduced modulo 256 wavelengths in double, and the per-frame step.
  gfx::GroundDetailParams m = d;
  gfx::ground_detail_motion(m, desc, 1.0e6 + 0.05, 0.01, 1.0f);
  CHECK((m.flags & gfx::k_ground_motion) != 0u);
  const double period = gfx::k_ground_travel_period * wavelength;
  CHECK(static_cast<double>(m.travel) ==
        doctest::Approx(std::fmod(1.0e6 + 0.05, period)).epsilon(1e-6));
  CHECK(m.ripple_live == 1.0f);
  gfx::GroundDetailParams storm = d;
  gfx::ground_detail_motion(storm, desc, 0.0, 0.0, 2.5f);
  CHECK(storm.ripple_live == 0.0f);
  gfx::GroundDetailParams gust = d;
  gfx::ground_detail_motion(gust, desc, 0.0, 0.0, 2.1f);
  CHECK(gust.ripple_live > 0.0f);
  CHECK(gust.ripple_live < 1.0f);

  // A time-lapse blurs them: at a quarter of a wavelength a frame whole, at a half gone, the lost
  // slope variance into the roughness; a storm flattens them, and adds no roughness.
  gfx::GroundDetailParams r = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  r.flags &= ~gfx::k_ground_grain;
  const ref::Dvec3 up{0.0, 1.0, 0.0};
  const ref::Dvec3 none{};
  const ref::Dvec3 albedo{0.6, 0.5, 0.4};
  const ref::Dvec3 p{1.3, 0.0, 2.1};
  gfx::GroundDetailParams slow = r, fast = r, flat = r;
  gfx::ground_detail_motion(slow, desc, 3.0, 0.2 * wavelength, 1.0f);
  gfx::ground_detail_motion(fast, desc, 3.0, 0.6 * wavelength, 1.0f);
  gfx::ground_detail_motion(flat, desc, 3.0, 0.0, 3.0f);
  const gref::Shading a = gref::shade(slow, p, up, none, none, 1.0, albedo, 0.85);
  const gref::Shading b = gref::shade(fast, p, up, none, none, 1.0, albedo, 0.85);
  const gref::Shading c = gref::shade(flat, p, up, none, none, 1.0, albedo, 0.85);
  CHECK(a.fade == 1.0);
  CHECK(b.fade == 0.0);
  CHECK(b.roughness > 0.85);
  CHECK(b.normal.y == 1.0);
  CHECK(c.weight == 0.0);
  CHECK(c.roughness == 0.85);
  CHECK(c.normal.y == 1.0);
}

TEST_CASE("ground detail: grainflow lanes come and go with the wind") {
  // An episode's shape: present for the share of its cycle, sudden up and slow down, no step at
  // the cycle's wrap; over every phase a lane is present for the share of the time.
  CHECK(gref::episode(0.0, 0.25) == 0.0);
  CHECK(gref::episode(0.999999, 0.25) == 0.0);
  CHECK(gref::episode(0.25 * 0.2, 0.25) == 1.0);
  u32 present = 0;
  double largest_step = 0.0, previous = gref::episode(0.0, 0.25);
  for (u32 i = 1; i <= 100000; ++i) {
    const double e = gref::episode(i * 1e-5, 0.25);
    if (e > 0.0) ++present;
    largest_step = std::max(largest_step, std::fabs(e - previous));
    previous = e;
  }
  CHECK(present / 100000.0 == doctest::Approx(0.25).epsilon(0.01));
  CHECK(largest_step < 0.005);

  gfx::GroundDetailDesc desc;
  desc.ripple_height = 0.0f;
  desc.grain_albedo = 0.0f;
  desc.grain_roughness = 0.0f;
  desc.flow_start_deg = 24.0f;
  desc.flow_full_deg = 30.0f;
  desc.flow_share = 0.25f;
  desc.flow_turnover = 800.0f;
  const gfx::GroundDetailParams base = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  // The clock is the transport times the turnover, a fraction of a cycle; no wind, no change.
  gfx::GroundDetailParams a = base, b = base;
  gfx::ground_detail_motion(a, desc, 0.0, 0.0, 1.0f, 12.3456, 0.0);
  gfx::ground_detail_motion(b, desc, 0.0, 0.0, 1.0f, 12.3456, 0.0);
  CHECK(a.flow_clock == b.flow_clock);
  CHECK(static_cast<double>(a.flow_clock) ==
        doctest::Approx(12.3456 * 800.0 - std::floor(12.3456 * 800.0)).epsilon(1e-5));

  // Sparse: on a slip face, with a quarter of the lanes present, most of the face is smooth; and
  // the lanes present at one reading are not those at a reading half a cycle on.
  const double slope = 32.0 * 3.14159265358979323846 / 180.0;
  u32 lit = 0, changed = 0;
  gfx::GroundDetailParams later = base;
  later.flow_clock = 0.5f;
  for (u32 i = 0; i < 4000; ++i) {
    const double x = 0.047 * i - 90.0;
    const double z = 0.031 * i + 3.0;
    const gref::Flow f = gref::grainflow(base, x, z, 1.0, 0.0);
    const gref::Flow g = gref::grainflow(later, x, z, 1.0, 0.0);
    if (std::fabs(f.gx) + std::fabs(f.gz) > 0.3) ++lit;
    if ((std::fabs(f.gx) + std::fabs(f.gz) > 0.3) != (std::fabs(g.gx) + std::fabs(g.gz) > 0.3))
      ++changed;
  }
  MESSAGE("grainflow episodes: " << lit / 40.0 << "% of a face in a lane, " << changed / 40.0
                                 << "% changed half a cycle on");
  CHECK(lit < 4000u / 2u);
  CHECK(changed > 4000u / 10u);

  // Faster than a frame can show, the lanes fade: at half the share a frame there are none.
  const ref::Dvec3 n{std::sin(slope), std::cos(slope), 0.0};
  const ref::Dvec3 none{};
  const ref::Dvec3 albedo{0.6, 0.5, 0.4};
  gfx::GroundDetailParams storm = base;
  storm.flow_clock_step = 0.125f;
  u32 drawn = 0;
  for (u32 i = 0; i < 200; ++i) {
    const gref::Shading g =
        gref::shade(storm, ref::Dvec3{0.37 * i, 0.0, 1.0}, n, none, none, 1.0, albedo, 0.85);
    if (g.normal.x != n.x || g.albedo.x != albedo.x) ++drawn;
  }
  CHECK(drawn == 0);
}

TEST_CASE("ground detail: grainflow lanes run the face's length and never cross") {
  // The fourth pass (docs/experiments/sand-fourth-pass-2026-10-03.md): the lanes are lines in
  // sixteen fixed directions, a function of position alone, one family for most fall lines and two
  // blended only within 2.25 degrees of a sector's middle.
  gfx::GroundDetailDesc desc;
  desc.flow_start_deg = 24.0f;
  desc.flow_full_deg = 30.0f;
  const gfx::GroundDetailParams d = gfx::ground_detail_block(desc, Vec2{1.0f, 0.0f}, 7u);
  const double deg = 3.14159265358979323846 / 180.0;
  // Tongues: along a lane's own centre line, the runs where a tongue stands (its height not zero)
  // are its length — 45 to 95% of `flow_length` (24 m) less the soft ends — and the gaps between
  // them are where some stop part-way. Measured along the centres of 400 lanes of direction 0.
  {
    std::vector<double> runs;
    const double spacing = 2.0 * static_cast<double>(d.flow_width);
    for (u32 lane = 0; lane < 400; ++lane) {
      const double z = (static_cast<double>(lane) - 200.0) * spacing;
      double run = 0.0;
      for (u32 i = 0; i < 2000; ++i) {
        // Direction 0 is +x; its lanes lie along x at z = m spacing + jitter, so look across the
        // lane's width for its crest.
        double best = 0.0;
        for (const double dz : {-0.6, -0.45, -0.3, -0.15, 0.0, 0.15, 0.3, 0.45, 0.6})
          best = std::max(best, std::fabs(gref::lane_set(d, 0.05 * i, z + dz * spacing, 0u).value));
        if (best > 1e-6) {
          run += 0.05;
        } else if (run > 0.0) {
          runs.push_back(run);
          run = 0.0;
        }
      }
    }
    std::sort(runs.begin(), runs.end());
    REQUIRE(runs.size() > 50u);
    const double median = runs[runs.size() / 2];
    MESSAGE("tongues: " << runs.size() << " runs along lanes' centres, median " << median
                        << " m, 10th percentile " << runs[runs.size() / 10] << " m, 90th "
                        << runs[runs.size() * 9 / 10] << " m");
    CHECK(median > 8.0);
  }
  (void)deg;
  // No crossing: the gradient's share along the fall line at the 99th percentile of points where
  // the lanes stand clear of zero, outside the blend (one family: at most sin 9 degrees = 0.156)
  // and at the blend's middle, where two families cross (reported, the price of the blend).
  for (const double angle : {0.0, 8.0, 11.25}) {
    const double fx = std::cos(angle * deg);
    const double fz = std::sin(angle * deg);
    std::vector<double> ratio;
    for (u32 i = 0; i < 20000; ++i) {
      const gref::Flow f = gref::grainflow(d, 0.0731 * i - 600.0, 0.0517 * i + 9.0, fx, fz);
      const double along = std::fabs(f.gx * fx + f.gz * fz);
      const double across = std::fabs(-f.gx * fz + f.gz * fx);
      const double total = std::hypot(along, across);
      if (total > 0.2 / static_cast<double>(d.flow_width)) ratio.push_back(along / total);
    }
    std::sort(ratio.begin(), ratio.end());
    const double p90 = ratio[ratio.size() * 90 / 100];
    const double p99 = ratio[ratio.size() * 99 / 100];
    MESSAGE("fall line " << angle << " degrees: the gradient's share along it, 90th percentile "
                         << p90 << ", 99th " << p99 << " (the tongues' heads and toes)");
    // A tongue's head, its chute easing into its lobe, its rounded toes and its meander (a
    // centreline leaning up to about 11 degrees) slope along the fall line by design; lanes of one
    // direction never cross. Outside a blend nine points in ten lean less than 0.4 along it.
    if (angle < 11.0) CHECK(p90 < 0.4);
  }
  // Seams: across the angle where the blend starts and where it ends, a micro-degree either side
  // gives the same value; and across a direction's own angle, where one family hands to the next.
  double jump = 0.0;
  for (const double edge : {9.0, 13.5, 22.5}) {
    for (u32 i = 0; i < 2000; ++i) {
      const double x = 0.413 * i - 300.0;
      const double z = 0.271 * i + 2.0;
      const double lo = (edge - 1e-6) * deg;
      const double hi = (edge + 1e-6) * deg;
      const double a = gref::grainflow(d, x, z, std::cos(lo), std::sin(lo)).value;
      const double b = gref::grainflow(d, x, z, std::cos(hi), std::sin(hi)).value;
      jump = std::max(jump, std::fabs(a - b));
    }
  }
  CHECK(jump < 1e-4);
}

// ---- fifth pass: what a slip face's lee term draws, read as an eye reads it
// ----------------------
//
// docs/experiments/sand-fifth-pass-2026-10-04.md. The owner saw a slip face in the endless desert
// covered edge to edge in short light and dark dashes; the fourth pass's numbers (variance, the
// gradient's share along the fall line, the filter's mean) could not have caught it, because they
// say nothing of what the picture is made of. Two readings do:
//
// - **Features.** A face 40 m down its fall line and 16 m across, in plan at 5 cm a sample (the
//   owner's pixels were 1.3 to 3.3 cm), each sample shaded with that footprint and with every term
//   of the block, lit by the sky alone (the face in its own shade, as he saw it at 10:00) and by a
//   sun raking across the fall line 20 degrees up. Where the luminance stands more than 2% off the
//   face's mean is a feature, light and dark apart, eight-connected; each is measured by its second
//   moments along the surface (a uniform ellipse's full axes, `4 sqrt(lambda)`, each sample
//   counting its own width). Speckle is many, short, crisp; grainflow is few, long, faint.
// - **Speckle at a distance.** Pixels of 5 to 40 cm over the same face: the variation the pixels
//   draw against the variation 8 x 8 samples of each pixel hold. A feature finer than its pixel
//   that is still drawn shows as a pixel-wide line or dot, the supersampled picture averages it
//   away, and the ratio says so.
namespace {

struct FaceReading {
  u32 features = 0;
  double per_100m2 = 0.0;      // features per 100 m^2 of the face's surface
  double median_length = 0.0;  // metres along the surface
  double median_aspect = 0.0;  // long axis over short
  double p99_contrast = 0.0;   // |luminance / mean - 1|, 99th percentile
  double covered = 0.0;        // the share of the face past the threshold
};

constexpr double k_face_slope = 32.0 * 3.14159265358979323846 / 180.0;

double face_luma(const gfx::GroundDetailParams& d, double x, double z, double step, bool sun) {
  const ref::Dvec3 n{std::sin(k_face_slope), std::cos(k_face_slope), 0.0};
  const ref::Dvec3 albedo{0.62, 0.47, 0.32};
  const ref::Dvec3 p{x, -(n.x * x) / n.y, z};
  const ref::Dvec3 dx{step, -n.x / n.y * step, 0.0};
  const ref::Dvec3 dz{0.0, 0.0, step};
  const gref::Shading g = gref::shade(d, p, n, dx, dz, 1.0, albedo, 0.85);
  ref::Surface sf;
  sf.position = p;
  sf.normal = g.normal;
  sf.view = ref::normalize(ref::Dvec3{0.8, 0.6, 0.1});
  sf.albedo = g.albedo;
  sf.roughness = g.roughness;
  const ref::Dvec3 light = ref::normalize(ref::Dvec3{0.1, 0.35, 0.93});
  return ref::luminance(ref::shade(sf, light, 4.5, ref::Dvec3{0.45, 0.55, 0.75}, albedo, nullptr, 0,
                                   ref::Dvec3{}, !sun));
}

FaceReading read_face(const std::vector<double>& luma, u32 w, u32 h, double pixel,
                      double threshold) {
  FaceReading out;
  double mean = 0.0;
  for (const double v : luma)
    mean += v;
  mean /= static_cast<double>(luma.size());
  std::vector<double> contrast(luma.size());
  for (size_t i = 0; i < luma.size(); ++i)
    contrast[i] = luma[i] / mean - 1.0;
  std::vector<double> sorted(contrast.size());
  u32 over = 0;
  for (size_t i = 0; i < contrast.size(); ++i) {
    sorted[i] = std::fabs(contrast[i]);
    if (sorted[i] > threshold) ++over;
  }
  std::sort(sorted.begin(), sorted.end());
  out.p99_contrast = sorted[sorted.size() * 99 / 100];
  out.covered = static_cast<double>(over) / static_cast<double>(contrast.size());
  // Light and dark apart, eight-connected. Rows run down the fall line (x in plan), which the
  // surface stretches by 1 / cos(slope).
  const double along = pixel / std::cos(k_face_slope);
  std::vector<u8> seen(contrast.size(), 0u);
  std::vector<u32> stack;
  std::vector<double> lengths, aspects;
  for (u32 start = 0; start < w * h; ++start) {
    if (seen[start] || std::fabs(contrast[start]) <= threshold) continue;
    const bool light = contrast[start] > 0.0;
    double n = 0.0, sx = 0.0, sz = 0.0, sxx = 0.0, szz = 0.0, sxz = 0.0;
    stack.clear();
    stack.push_back(start);
    seen[start] = 1u;
    while (!stack.empty()) {
      const u32 at = stack.back();
      stack.pop_back();
      const u32 i = at % w;
      const u32 j = at / w;
      const double x = i * along;
      const double z = j * pixel;
      n += 1.0;
      sx += x;
      sz += z;
      sxx += x * x;
      szz += z * z;
      sxz += x * z;
      for (i32 b = -1; b <= 1; ++b) {
        for (i32 a = -1; a <= 1; ++a) {
          const i32 ni = static_cast<i32>(i) + a;
          const i32 nj = static_cast<i32>(j) + b;
          if (ni < 0 || nj < 0 || ni >= static_cast<i32>(w) || nj >= static_cast<i32>(h)) continue;
          const u32 k = static_cast<u32>(nj) * w + static_cast<u32>(ni);
          if (seen[k]) continue;
          const double c = contrast[k];
          if (light ? c > threshold : c < -threshold) {
            seen[k] = 1u;
            stack.push_back(k);
          }
        }
      }
    }
    if (n < 3.0) continue;  // a speck of one or two samples is under any pixel that sees it
    const double mx = sx / n, mz = sz / n;
    const double cxx = sxx / n - mx * mx + along * along / 12.0;
    const double czz = szz / n - mz * mz + pixel * pixel / 12.0;
    const double cxz = sxz / n - mx * mz;
    const double tr = 0.5 * (cxx + czz);
    const double det = std::sqrt(std::max(0.0, 0.25 * (cxx - czz) * (cxx - czz) + cxz * cxz));
    const double major = 4.0 * std::sqrt(tr + det);
    const double minor = 4.0 * std::sqrt(std::max(tr - det, 1e-12));
    lengths.push_back(major);
    aspects.push_back(major / minor);
  }
  out.features = static_cast<u32>(lengths.size());
  const double area = (w * along) * (h * pixel);
  out.per_100m2 = 100.0 * static_cast<double>(lengths.size()) / area;
  if (!lengths.empty()) {
    std::sort(lengths.begin(), lengths.end());
    std::sort(aspects.begin(), aspects.end());
    out.median_length = lengths[lengths.size() / 2];
    out.median_aspect = aspects[aspects.size() / 2];
  }
  return out;
}

// The face in plan, both lights from one evaluation of the detail per sample.
void read_face_both(const gfx::GroundDetailParams& d, FaceReading& shade, FaceReading& sun,
                    double pixel = 0.05, u32 w = 680, u32 h = 320) {
  std::vector<double> sky(w * h), lit(w * h);
  for (u32 j = 0; j < h; ++j) {
    for (u32 i = 0; i < w; ++i) {
      const double x = 40.0 + (i + 0.5) * pixel;
      const double z = 7.0 + (j + 0.5) * pixel;
      sky[j * w + i] = face_luma(d, x, z, pixel, false);
      lit[j * w + i] = face_luma(d, x, z, pixel, true);
    }
  }
  shade = read_face(sky, w, h, pixel, 0.02);
  sun = read_face(lit, w, h, pixel, 0.02);
}

// Speckle: over 1,500 pixels of `pixel` metres scattered across the face, each drawn at its
// footprint and held as the mean of 8 x 8 samples of it, the root-mean-square of the difference
// (what a pixel draws that it cannot hold: a feature finer than it, drawn) and of the held picture
// about its mean (what there is to see at that size), both as shares of the mean; under the raking
// sun, which shows relief the most.
void read_speckle(const gfx::GroundDetailParams& d, double pixel, double& error, double& held,
                  double* drawn_rms = nullptr) {
  const u32 count = 1500;
  std::vector<double> drawn(count), kept(count);
  double mean = 0.0;
  for (u32 k = 0; k < count; ++k) {
    // A low-discrepancy scatter over 34 m by 16 m (the golden ratio's sequence and its square).
    const double a = k * 0.6180339887498949;
    const double b = k * 0.7548776662466927;
    const double x0 = 40.0 + 34.0 * (a - std::floor(a));
    const double z0 = 7.0 + 16.0 * (b - std::floor(b));
    drawn[k] = face_luma(d, x0 + 0.5 * pixel, z0 + 0.5 * pixel, pixel, true);
    double u = 0.0;
    for (u32 j = 0; j < 8; ++j)
      for (u32 i = 0; i < 8; ++i)
        u += face_luma(d, x0 + (i + 0.5) * pixel / 8.0, z0 + (j + 0.5) * pixel / 8.0, pixel / 8.0,
                       true);
    kept[k] = u / 64.0;
    mean += kept[k];
  }
  mean /= count;
  double e2 = 0.0, h2 = 0.0;
  for (u32 k = 0; k < count; ++k) {
    e2 += (drawn[k] - kept[k]) * (drawn[k] - kept[k]);
    h2 += (kept[k] - mean) * (kept[k] - mean);
  }
  error = std::sqrt(e2 / count) / mean;
  held = std::sqrt(h2 / count) / mean;
  if (drawn_rms) {
    double dm = 0.0, d2 = 0.0;
    for (u32 k = 0; k < count; ++k)
      dm += drawn[k];
    dm /= count;
    for (u32 k = 0; k < count; ++k)
      d2 += (drawn[k] - dm) * (drawn[k] - dm);
    *drawn_rms = std::sqrt(d2 / count) / mean;
  }
}
}  // namespace

TEST_CASE("ground detail: a slip face draws a few long faint tongues, not speckle") {
  // The ergs' numbers, which both erg scenes now carry (the endless desert carried the second
  // pass's streaks until the fifth pass), and those streaks, which are what the owner saw.
  const gfx::GroundDetailParams ergs =
      gfx::ground_detail_block(gref::erg_numbers(), Vec2{1.0f, 0.0f}, 7u);
  gfx::GroundDetailDesc second = gref::erg_numbers();
  second.streak_start_deg = 22.0f;
  second.streak_full_deg = 30.0f;
  second.flow_start_deg = 0.0f;
  second.flow_full_deg = 0.0f;
  const gfx::GroundDetailParams streaks = gfx::ground_detail_block(second, Vec2{1.0f, 0.0f}, 7u);
  REQUIRE((ergs.flags & gfx::k_ground_grainflow) != 0u);
  REQUIRE((streaks.flags & gfx::k_ground_streaks) != 0u);

  FaceReading ergs_sky, ergs_sun, streaks_sky, streaks_sun;
  read_face_both(ergs, ergs_sky, ergs_sun);
  read_face_both(streaks, streaks_sky, streaks_sun);
  const auto say = [](const std::string& what, const FaceReading& r) {
    MESSAGE(what << ": " << r.features << " features, " << r.per_100m2 << " per 100 m^2, median "
                 << r.median_length << " m long at an aspect of " << r.median_aspect
                 << "; contrast " << r.p99_contrast << " at the 99th percentile, " << r.covered
                 << " of the face past 2%");
  };
  say("tongues in shade", ergs_sky);
  say("tongues under a raking sun", ergs_sun);
  say("streaks in shade", streaks_sky);
  say("streaks under a raking sun", streaks_sun);
  // Few, long and faint where the owner looked, a face in its own shade: 2.0 features per 100 m^2,
  // a median 3.9 m long at an aspect of 15, 3.9% of contrast at the 99th percentile (the fourth
  // pass read 2.7, 3.8 m, 3.7%).
  CHECK(ergs_sky.per_100m2 < 5.0);
  CHECK(ergs_sky.median_length > 3.0);
  CHECK(ergs_sky.median_aspect > 8.0);
  CHECK(ergs_sky.p99_contrast < 0.05);
  // Under a low raking sun the relief shows, and stays long: 7.2 per 100 m^2, a median 8.1 m. The
  // fourth pass's levees read 19.2 per 100 m^2 a median 1.8 m long here — dashes.
  CHECK(ergs_sun.per_100m2 < 12.0);
  CHECK(ergs_sun.median_length > 5.0);
  CHECK(ergs_sun.median_aspect > 12.0);
  // The instrument sees the dashes the owner saw: the second pass's streaks are 49 features per
  // 100 m^2 a median 1.7 m long at 26% of contrast.
  CHECK(streaks_sky.per_100m2 > 25.0);
  CHECK(streaks_sky.median_length < 2.5);
  CHECK(streaks_sky.p99_contrast > 0.15);
  for (const double pixel : {0.03, 0.05, 0.07, 0.1, 0.15, 0.2, 0.3, 0.4}) {
    double error = 0.0, held = 0.0, drawn = 0.0, serror = 0.0, sheld = 0.0, sdrawn = 0.0;
    read_speckle(ergs, pixel, error, held, &drawn);
    read_speckle(streaks, pixel, serror, sheld, &sdrawn);
    MESSAGE("pixels of " << pixel * 100.0 << " cm: tongues vary by " << drawn << " drawn, " << held
                         << " held, " << error << " apart; streaks " << sdrawn << ", " << sheld
                         << ", " << serror);
    // Never more variation drawn than the pixels hold, but for a tenth: at most 1.09 times, at
    // 10 cm. The fourth pass drew 1.17 times at 10 cm, 1.36 at 15 and 1.19 at 20.
    CHECK(drawn < 1.15 * held + 0.002);
  }
}
