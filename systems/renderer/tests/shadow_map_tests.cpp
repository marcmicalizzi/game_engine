// The sun's cascaded shadow maps (docs/subsystems/renderer.md, "Shadows"; docs/plan/04-renderer.md
// §4.4, the 2026-09-23 direction note "shadows without ray queries").
//
// On the CPU, on every machine: the fit covers the camera's depth slices, keeps its texels the same
// size while the camera turns and on a whole-texel grid while it moves, and handles a surround; the
// settings resolve `csm` where the plan says (auto picks it without ray queries, explicit anywhere
// a raster path resolves the picture, never on direct, sw, auto or rt).
//
// On a GPU, over the shadow casters' fixture (shadow_fixture.h):
//
//   1. **The mapped shadow is the shadow a CPU ray finds**, on every caster, through the mesh path
//      and the vertex path, which must agree byte for byte: every ground pixel whose CPU answer is
//      the same for every pixel around it (the filter's reach) must be black where the CPU's sun
//      ray hits the caster and white where it does not. The edge band between is counted and
//      reported.
//   2. **A caster outside the camera's frustum casts**: the card above and beside a camera looking
//      straight down at its shadow, which the ray-traced shadows (built from the camera's cut)
//      cannot draw. A CPU witness first requires every cluster of the card to be outside the
//      frustum.
//   3. **The light-view cull never changes the shadow**: every cascade drawn with and without its
//      frustum test gives the same bytes, on the fixture and on the heightfield, deformed too.
//   4. **Mapped and traced shadows agree** where both are defined, away from the filter's edge.
//   5. **A shadow ray leaves its surface by that surface's own grid**, not by the scene's size:
//      the casters on a ground kilometres across still cast, pixel for pixel what a CPU ray fired
//      from the same lifted point finds (the ashlar ruins' missing shadows, 2026-09-26).
//
// A TITAN-Xp-like device's default request drawing maps is in renderer_tests.cpp beside the rest of
// that profile. The measurement on the Khronos samples is the last case, skipped by default.
#include "shadow_fixture.h"

#include <core/math/math.h>
#include <domain/gfx/device.h>
#include <foundation/image/png.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/caster_volume.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/lighting.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/shadow_cascades.h>
#include <systems/renderer/view_set.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::renderer;
using namespace engine::renderer::shadow_fixture;

namespace {

const Vec3 k_sun = normalize(Vec3{0.4f, 0.8f, 0.45f});  // frame_lighting's sun

// A frustum-slice corner unprojected through the view's own matrix: a second route to the corners
// `fit_shadow_cascades` builds from each view's frame, so the test does not share its arithmetic.
// Reversed-Z with no far plane puts view depth d at clip depth znear / d.
Vec3 unproject(const Mat4& inverse_view_proj, f32 x, f32 y, f32 depth) {
  const Vec4 p = inverse_view_proj * Vec4{x, y, depth, 1.0f};
  return Vec3{p.x / p.w, p.y / p.w, p.z / p.w};
}

// Every corner of every non-scene cascade's slice, in every view, is inside its sphere.
void check_corners(const ViewSet& views, const Camera& camera, const ShadowCascades& cascades) {
  u32 checked = 0;
  for (u32 c = 0; c < cascades.count; ++c) {
    if (cascades.scene_bounds[c]) continue;
    const f32 texel = 2.0f * cascades.radii[c] / static_cast<f32>(cascades.resolution);
    for (u32 v = 0; v < views.size(); ++v) {
      const Mat4 inv = inverse(views[v].view_proj);
      for (u32 end = 0; end < 2; ++end) {
        const f32 d = cascades.splits[c + end];
        for (u32 k = 0; k < 4; ++k) {
          const Vec3 p = unproject(inv, (k & 1) != 0 ? 1.0f : -1.0f, (k & 2) != 0 ? 1.0f : -1.0f,
                                   camera.znear / d);
          // The centre was snapped by up to half a texel along two axes.
          const f32 slack = cascades.radii[c] * 1.0e-3f + texel;
          CHECK_MESSAGE(length(p - cascades.centers[c]) <= cascades.radii[c] + slack,
                        "cascade " << c << " view " << v << " corner " << k << " at depth " << d
                                   << " is " << length(p - cascades.centers[c])
                                   << " from a sphere of radius " << cascades.radii[c]);
          ++checked;
        }
      }
    }
  }
  CHECK(checked > 0);
}

// Where cascade c's centre is in the world, across the light, in its own texels: the centres are in
// the frame's space, whose origin is the camera's eye (ADR-0053), so the centre is put back in the
// world in f64 and projected on the light's right and up. A texel's edges are the centre's
// coordinates plus whole texels (the radius is a whole number of them), so two fits whose centres
// differ here by whole numbers put every texel on the same piece of the world.
struct LightTexels {
  f64 x = 0.0;
  f64 y = 0.0;
};

LightTexels light_texels(const ShadowCascades& cascades, u32 c, const Camera& camera) {
  const f64 texel = 2.0 * static_cast<f64>(cascades.radii[c]) / cascades.resolution;
  const DVec3 from = (camera.position + DVec3{cascades.centers[c]}) - WorldPos::origin();
  return {dot(from, DVec3{cascades.light.right}) / texel,
          dot(from, DVec3{cascades.light.up}) / texel};
}

// How far, in texels, a and b are from being a whole number of texels apart.
f64 off_by(LightTexels a, LightTexels b) {
  const f64 x = a.x - b.x;
  const f64 y = a.y - b.y;
  return std::max(std::fabs(x - std::round(x)), std::fabs(y - std::round(y)));
}

// The texel grid across the light: a snapped centre's coordinates, from the world's origin, are
// whole texels (worked out here rather than read from the fit).
f32 off_grid(const ShadowCascades& cascades, u32 c, const Camera& camera) {
  return static_cast<f32>(off_by(light_texels(cascades, c, camera), LightTexels{}));
}

}  // namespace

TEST_CASE("renderer: the shadow cascades cover the camera's slices and hold still as it turns") {
  std::string error;
  ViewSet views;
  REQUIRE(views.build(ViewSetDesc{}, 1280, 720, &error));

  // The orbit engine-view frames a mesh with: the frustum at the scene's distance is wider than the
  // scene, so the first slice's sphere already holds all of it, and the one cascade there is is the
  // scene's own bounds — every texel on the scene, none on the empty frustum around it.
  {
    const WorldPos center = WorldPos::origin();
    const f32 radius = 10.0f;
    const Camera camera = orbit_camera_at(center, radius, 22.0f, 0.3f, k_orbit_pitch);
    views.update(camera);
    ShadowCascades cascades;
    fit_shadow_cascades(views, camera, k_sun, center, radius, ShadowFit{}, cascades);
    CHECK(cascades.count == 1);
    CHECK(cascades.scene_bounds[0]);
    CHECK(cascades.radii[0] == radius);
    // The centres are in the frame's space, measured from the eye (ADR-0053).
    CHECK(length(cascades.centers[0] - relative(center, camera.position)) <=
          cascades.cascades[0].texel_world);
    MESSAGE("orbit at 22 radius-tenths: one cascade over the scene's bounds, "
            << cascades.cascades[0].texel_world * 100.0f << " cm a texel at "
            << cascades.resolution);
  }

  // A camera outside a larger scene: the first slice starts at the scene's near side rather than
  // at the camera's near plane, the slices hold their corners, and the cascade whose sphere would
  // hold the whole scene is the scene's own bounds and the last.
  {
    const WorldPos center = WorldPos::origin();
    const f32 radius = 100.0f;
    Camera camera;
    camera.position = absolute(WorldPos::origin(), Vec3{0.0f, 50.0f, 150.0f});
    camera.target = center;
    camera.znear = 0.1f;
    views.update(camera);
    ShadowCascades cascades;
    fit_shadow_cascades(views, camera, k_sun, center, radius, ShadowFit{}, cascades);
    const f32 distance = static_cast<f32>(length(camera.position - center));
    CHECK(cascades.splits[0] == doctest::Approx(distance - radius).epsilon(1e-4));
    CHECK(cascades.splits[cascades.count] == doctest::Approx(distance + radius).epsilon(1e-4));
    for (u32 c = 0; c < cascades.count; ++c)
      CHECK(cascades.splits[c + 1] > cascades.splits[c]);
    REQUIRE(cascades.count >= 2);
    CHECK(cascades.scene_bounds[cascades.count - 1]);
    CHECK_FALSE(cascades.scene_bounds[0]);
    check_corners(views, camera, cascades);
    MESSAGE("outside a 100-unit scene: "
            << cascades.count << " cascades, radii " << cascades.radii[0] << " .. "
            << cascades.radii[cascades.count - 1] << ", the last the scene's bounds");
  }

  // A camera standing inside a 5 km terrain, as on the desert overlook: the first split is a
  // thousandth of the shadow distance, the cascades are four, and turning the camera on the spot
  // changes neither their count nor any radius — a sphere is the same whichever way it faces, and
  // the radius is quantized so the float noise of the turn cannot move a texel's size.
  {
    const WorldPos center = WorldPos::origin();
    const f32 radius = 3600.0f;
    Camera camera;
    camera.position = absolute(WorldPos::origin(), Vec3{10.0f, 3.0f, 900.0f});
    camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 40.0f, 300.0f});
    camera.znear = 0.1f;
    ShadowFit fit;
    fit.distance = 2000.0f;  // well inside the terrain, so no cascade is the scene's bounds
    views.update(camera);
    ShadowCascades first;
    fit_shadow_cascades(views, camera, k_sun, center, radius, fit, first);
    REQUIRE(first.count == 4);
    CHECK(first.splits[1] == doctest::Approx(2.0f * std::pow(1000.0f, 0.25f)).epsilon(1e-3));
    check_corners(views, camera, first);
    const Vec3 forward = narrow(camera.target - camera.position);
    for (const f32 yaw : {0.4f, 1.7f, -2.9f}) {
      Camera turned = camera;
      const f32 cs = std::cos(yaw);
      const f32 sn = std::sin(yaw);
      turned.target = camera.position + Vec3{forward.x * cs - forward.z * sn, forward.y,
                                             forward.x * sn + forward.z * cs};
      views.update(turned);
      ShadowCascades after;
      fit_shadow_cascades(views, turned, k_sun, center, radius, fit, after);
      REQUIRE(after.count == first.count);
      for (u32 c = 0; c < first.count; ++c)
        CHECK(after.radii[c] == first.radii[c]);
      check_corners(views, turned, after);
    }
    // Moving the camera moves every cascade by whole texels.
    for (const Vec3 step : {Vec3{0.37f, 0.0f, 0.21f}, Vec3{-1.9f, 0.4f, 3.3f}}) {
      Camera moved = camera;
      moved.position = camera.position + step;
      moved.target = camera.target + step;
      views.update(moved);
      ShadowCascades after;
      fit_shadow_cascades(views, moved, k_sun, center, radius, fit, after);
      for (u32 c = 0; c < after.count; ++c)
        CHECK_MESSAGE(off_grid(after, c, moved) < 2.0e-2f, "cascade " << c << " is "
                                                                      << off_grid(after, c, moved)
                                                                      << " texel off its grid");
    }
    MESSAGE("inside a 3.6 km scene, 2 km of shadow: splits "
            << first.splits[1] << ", " << first.splits[2] << ", " << first.splits[3] << ", "
            << first.splits[4] << " m; texels " << first.cascades[0].texel_world * 100.0f << " to "
            << first.cascades[3].texel_world * 100.0f << " cm");
  }

  // A turned three-monitor surround: every view's slice corners are inside the cascades.
  {
    ViewSetDesc desc;
    desc.layout = ViewLayout::Surround3;
    desc.surround.side_yaw = radians(30.0f);
    ViewSet surround;
    REQUIRE(surround.build(desc, 3 * 640, 360, &error));
    Camera camera;
    camera.position = absolute(WorldPos::origin(), Vec3{5.0f, 4.0f, 30.0f});
    camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 0.0f, 0.0f});
    camera.znear = 0.1f;
    surround.update(camera);
    ShadowFit fit;
    fit.distance = 80.0f;
    ShadowCascades cascades;
    fit_shadow_cascades(surround, camera, k_sun, WorldPos::origin(), 500.0f, fit, cascades);
    REQUIRE(cascades.count == 4);
    check_corners(surround, camera, cascades);
  }
}

// **The hybrid's cascades begin where its rays hand over** (R103; renderer.md, "Rays near, maps
// far"). With `ShadowFit::begin_distance` the first slice starts at the least view depth a point
// that far from the eye can have — the distance over the widest corner ray's secant — so every
// surface the hybrid reads the maps for (at least the ray distance from the eye) is inside a slice,
// none of the four is spent on the rays' side, and each is finer for
// the depth it reaches. A zero begin is the fit it always was.
TEST_CASE("renderer: the hybrid's cascades begin where its rays hand over") {
  std::string error;
  const WorldPos center = WorldPos::origin();
  const f32 radius = 3600.0f;
  Camera camera;
  camera.position = absolute(WorldPos::origin(), Vec3{10.0f, 3.0f, 900.0f});
  camera.target = absolute(WorldPos::origin(), Vec3{0.0f, 40.0f, 300.0f});
  camera.znear = 0.1f;
  ViewSetDesc wide;
  wide.layout = ViewLayout::Surround3;
  wide.surround.side_yaw = radians(30.0f);
  ViewSet layouts[2];
  REQUIRE(layouts[0].build(ViewSetDesc{}, 1280, 720, &error));
  REQUIRE(layouts[1].build(wide, 3 * 640, 360, &error));
  for (ViewSet& views : layouts) {
    views.update(camera);
    ShadowFit fit;
    fit.distance = 2000.0f;
    ShadowCascades all;
    fit_shadow_cascades(views, camera, k_sun, center, radius, fit, all);
    ShadowCascades same;
    ShadowFit zero = fit;
    zero.begin_distance = 0.0f;
    fit_shadow_cascades(views, camera, k_sun, center, radius, zero, same);
    REQUIRE(same.count == all.count);
    for (u32 c = 0; c <= all.count; ++c)
      CHECK(same.splits[c] == all.splits[c]);
    for (u32 c = 0; c < all.count; ++c) {
      CHECK(same.radii[c] == all.radii[c]);
      CHECK(same.centers[c].x == all.centers[c].x);
      CHECK(same.centers[c].z == all.centers[c].z);
    }
    for (const f32 begin : {50.0f, 100.0f, 400.0f}) {
      fit.begin_distance = begin;
      ShadowCascades hybrid;
      fit_shadow_cascades(views, camera, k_sun, center, radius, fit, hybrid);
      REQUIRE(hybrid.count == 4);
      // The least depth a point `begin` from the eye has in any view.
      f32 least = begin;
      for (u32 v = 0; v < views.size(); ++v) {
        const Mat4 inv = inverse(views[v].view_proj);
        for (u32 k = 0; k < 4; ++k) {
          const Vec3 unit = unproject(inv, (k & 1) != 0 ? 1.0f : -1.0f, (k & 2) != 0 ? 1.0f : -1.0f,
                                      camera.znear / 1.0f);
          least = std::min(least, begin / length(unit));
        }
      }
      CHECK(hybrid.splits[0] == doctest::Approx(least).epsilon(1e-3));
      CHECK(hybrid.splits[4] == all.splits[4]);
      check_corners(views, camera, hybrid);
      // Each cascade spans a smaller ratio of depths, so its texel is finer for the depth it ends
      // at (within the radius's quantization, a 64th of its power of two).
      for (u32 c = 0; c < 4; ++c) {
        CHECK(hybrid.cascades[c].texel_world / hybrid.splits[c + 1] <=
              all.cascades[c].texel_world / all.splits[c + 1] * 1.02f);
      }
      MESSAGE(views.size() << " view(s), maps from " << begin << " m: splits " << hybrid.splits[0]
                           << ", " << hybrid.splits[1] << ", " << hybrid.splits[2] << ", "
                           << hybrid.splits[3] << ", " << hybrid.splits[4] << " m; texels "
                           << hybrid.cascades[0].texel_world * 100.0f << " to "
                           << hybrid.cascades[3].texel_world * 100.0f << " cm (from "
                           << all.cascades[0].texel_world * 100.0f << " to "
                           << all.cascades[3].texel_world * 100.0f << ")");
    }
  }
}

// **A texel is a fixed piece of the world for as long as the sun stands still** (renderer.md,
// "Cascaded shadow maps"). The eye stepped a 1024th of a metre at a time across the corner of a
// 64 m cell — x = 64 and z = 128 at once — by the origin and 10,000 km out: every cascade's texel
// grid must stay where it is in the world, its centre moving by whole texels only. Until
// 2026-10-05 (picture-2) the grid was anchored at the eye's cell's corner, and every cascade took a
// sub-texel step at the crossing: a shimmer of every shadow edge every 64 m of travel.
TEST_CASE("renderer: a cascade's texels hold their place in the world as the eye crosses a cell") {
  std::string error;
  ViewSet views;
  REQUIRE(views.build(ViewSetDesc{}, 1280, 720, &error));
  ShadowFit fit;
  fit.distance = 2000.0f;  // inside the terrain below, so the splits are the eye's alone
  const f32 scene_radius = 3600.0f;
  for (const f64 cells : {0.0, 156250.0}) {
    const WorldPos base{cells * k_world_cell_m, 0.0, -cells * k_world_cell_m};
    Vector<Camera> cameras;
    for (i32 k = -6; k <= 6; ++k) {
      const f64 d = static_cast<f64>(k) / 1024.0;
      Camera camera;
      camera.position = base + DVec3{64.0 + d, 3.0, 128.0 + d};
      camera.target = camera.position + DVec3{-10.0, 37.0, -600.0};
      camera.znear = 0.1f;
      cameras.push_back(camera);
    }
    // The steps do cross the corner.
    REQUIRE(to_eye(cameras[0].position).cell.x + 1 == to_eye(cameras[12].position).cell.x);
    REQUIRE(to_eye(cameras[0].position).cell.z + 1 == to_eye(cameras[12].position).cell.z);
    ShadowCascades first;
    views.update(cameras[0]);
    fit_shadow_cascades(views, cameras[0], k_sun, base, scene_radius, fit, first);
    REQUIRE(first.count == 4);
    f64 worst[gfx::k_max_shadow_cascades] = {};
    for (u32 k = 1; k < cameras.size(); ++k) {
      views.update(cameras[k]);
      ShadowCascades after;
      fit_shadow_cascades(views, cameras[k], k_sun, base, scene_radius, fit, after);
      REQUIRE(after.count == first.count);
      for (u32 c = 0; c < first.count; ++c) {
        REQUIRE(after.radii[c] == first.radii[c]);
        const f64 off =
            off_by(light_texels(after, c, cameras[k]), light_texels(first, c, cameras[0]));
        worst[c] = off > worst[c] ? off : worst[c];
        // A centre in the frame's space is a float32 at the cascade's distance from the eye; its
        // rounding is a few ten-thousandths of a texel at the most.
        CHECK_MESSAGE(off < 1.0e-3, "cascade " << c << " at step " << k << " moved " << off
                                               << " of a texel against the world, " << cells
                                               << " cells out");
      }
    }
    MESSAGE(cells << " cells out, 12 steps across a cell's corner: texels "
                  << first.cascades[0].texel_world * 100.0f << " to "
                  << first.cascades[3].texel_world * 100.0f << " cm; worst sub-texel move "
                  << worst[0] << ", " << worst[1] << ", " << worst[2] << ", " << worst[3]);
  }
}

TEST_CASE("renderer: shadows resolve to maps without ray queries and to rays with them") {
  gfx::DeviceFeatures rtx;  // the RTX 5090's rows that matter here
  rtx.buffer_int64_atomics = true;
  rtx.mesh_shader = true;
  rtx.acceleration_structure = true;
  rtx.cluster_acceleration_structure = true;
  rtx.ray_query = true;
  rtx.geometry_shader = true;
  rtx.full_draw_index_uint32 = true;
  gfx::DeviceFeatures pascal = rtx;  // the TITAN Xp: no mesh shaders, no ray queries
  pascal.mesh_shader = false;
  pascal.cluster_acceleration_structure = false;
  pascal.ray_query = false;

  ResolvedSettings resolved;
  RenderSettings settings;  // auto
  resolve_settings(settings, rtx, nullptr, resolved);
  CHECK(resolved.shadows);  // traced where the device can: the 5090's default is unchanged
  CHECK_FALSE(resolved.csm);
  CHECK_FALSE(resolved.hybrid);  // asked for only (renderer.md, "Rays near, maps far")
  CHECK(std::string(resolved_shadow_name(resolved)) == "rt");
  {
    // The hybrid: rays and maps both, and the casters end where the rays do.
    RenderSettings asked;
    asked.shadows = ShadowMode::Hybrid;
    resolve_settings(asked, rtx, nullptr, resolved);
    CHECK(resolved.shadows);
    CHECK(resolved.csm);
    CHECK(resolved.hybrid);
    CHECK(resolved.rt_chain);
    CHECK_FALSE(resolved.occlusion);  // the rays' structures are built from one visible list
    CHECK(resolved.shadow_cascades == k_default_shadow_cascades);
    CHECK(std::string(resolved_shadow_name(resolved)) == "hybrid");
    CHECK(resolved.casters);
    CHECK(resolved.caster_horizon);
    CHECK(resolved.caster_horizon_m ==
          k_default_shadow_ray_distance_m + k_default_shadow_ray_band_m);
    // A shadow distance nearer than the rays' reach bounds the casters instead, with the horizon
    // asked for; without it the rays' reach does.
    RenderSettings nearer = asked;
    nearer.shadow_distance = 50.0f;
    nearer.rt_caster_horizon = true;
    resolve_settings(nearer, rtx, nullptr, resolved);
    CHECK(resolved.caster_horizon_m == 50.0f);
    nearer.rt_caster_horizon = false;
    resolve_settings(nearer, rtx, nullptr, resolved);
    CHECK(resolved.caster_horizon_m ==
          k_default_shadow_ray_distance_m + k_default_shadow_ray_band_m);
    // The explicit modes are what they were: rt has no maps and no horizon, csm no rays.
    RenderSettings rays;
    rays.shadows = ShadowMode::RayTraced;
    resolve_settings(rays, rtx, nullptr, resolved);
    CHECK(resolved.shadows);
    CHECK_FALSE(resolved.csm);
    CHECK_FALSE(resolved.hybrid);
    CHECK_FALSE(resolved.caster_horizon);
    CHECK(std::string(resolved_shadow_name(resolved)) == "rt");
    // The hybrid asked for on the ray path is its rays; on a device that cannot trace, its maps.
    RenderSettings hybrid;
    hybrid.shadows = ShadowMode::Hybrid;
    hybrid.raster = RasterMode::RayTrace;
    resolve_settings(hybrid, rtx, nullptr, resolved);
    CHECK(resolved.shadows);
    CHECK_FALSE(resolved.csm);
    CHECK_FALSE(resolved.hybrid);
    hybrid.raster = RasterMode::Hardware;
    resolve_settings(hybrid, rtx, nullptr, resolved);
    CHECK(resolved.hybrid);
  }
  resolve_settings(settings, rtx, nullptr, resolved);
  resolve_settings(settings, pascal, nullptr, resolved);
  CHECK_FALSE(resolved.shadows);
  CHECK(resolved.csm);  // maps where it cannot
  CHECK(resolved.vertex_path);
  CHECK(
      resolved.occlusion);  // the maps are culled from the light; the picture keeps its two passes
  CHECK(resolved.shadow_cascades == k_default_shadow_cascades);
  CHECK(std::string(resolved_shadow_name(resolved)) == "csm");
  CHECK(check_availability(resolved, pascal) == RenderAvailability::Ok);

  // An explicit csm draws maps on the mesh path of a device that could trace, and on the vertex
  // path, and turns nothing else off: occlusion culling stays, no acceleration structure is built.
  settings.shadows = ShadowMode::Cascaded;
  for (const RasterMode raster : {RasterMode::Hardware, RasterMode::Vertex}) {
    settings.raster = raster;
    resolve_settings(settings, rtx, nullptr, resolved);
    CHECK(resolved.csm);
    CHECK_FALSE(resolved.shadows);
    CHECK_FALSE(resolved.rt_chain);
    CHECK(resolved.occlusion);
    CHECK(check_availability(resolved, rtx) == RenderAvailability::Ok);
  }
  // Never on a path that does not rasterize the picture into one list per pass, or traces it.
  for (const RasterMode raster :
       {RasterMode::Direct, RasterMode::Software, RasterMode::Auto, RasterMode::RayTrace}) {
    settings.raster = raster;
    resolve_settings(settings, rtx, nullptr, resolved);
    CHECK_FALSE(resolved.csm);
    CHECK(resolved.shadow_cascades == 0);
  }
  // The knobs are clamped: at most four cascades, at least one, and a power-of-two map.
  settings.raster = RasterMode::Hardware;
  settings.shadow_cascades = 9;
  settings.shadow_map = 1000;
  resolve_settings(settings, rtx, nullptr, resolved);
  CHECK(resolved.shadow_cascades == 4);
  CHECK(resolved.settings.shadow_map == 1024);
  settings.shadow_cascades = 0;
  settings.shadow_map = 100000;
  resolve_settings(settings, rtx, nullptr, resolved);
  CHECK(resolved.shadow_cascades == 1);
  CHECK(resolved.settings.shadow_map == k_max_shadow_map);
  // Off is off everywhere.
  settings = RenderSettings{};
  settings.shadows = ShadowMode::Off;
  resolve_settings(settings, pascal, nullptr, resolved);
  CHECK_FALSE(resolved.csm);
  CHECK(std::string(resolved_shadow_name(resolved)) == "off");
  ShadowMode parsed = ShadowMode::Off;
  CHECK(parse_shadow_mode("csm", parsed));
  CHECK(parsed == ShadowMode::Cascaded);
  CHECK(std::string(shadow_name(ShadowMode::Cascaded)) == "csm");
  CHECK(parse_shadow_mode("hybrid", parsed));
  CHECK(parsed == ShadowMode::Hybrid);
  CHECK(std::string(shadow_name(ShadowMode::Hybrid)) == "hybrid");
  // The hybrid on the TITAN Xp is its maps.
  settings.shadows = ShadowMode::Hybrid;
  resolve_settings(settings, pascal, nullptr, resolved);
  CHECK(resolved.csm);
  CHECK_FALSE(resolved.hybrid);
  CHECK(check_availability(resolved, pascal) == RenderAvailability::Ok);
}

namespace {

// ---- a CPU reference for the fixture's sun shadow ---------------------------------------------

struct Dvec {
  f64 x = 0.0, y = 0.0, z = 0.0;
};
Dvec operator-(Dvec a, Dvec b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Dvec operator+(Dvec a, Dvec b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Dvec operator*(Dvec a, f64 s) { return {a.x * s, a.y * s, a.z * s}; }
f64 ddot(Dvec a, Dvec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
Dvec dcross(Dvec a, Dvec b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
Dvec dnormalize(Dvec a) { return a * (1.0 / std::sqrt(ddot(a, a))); }
Dvec dv(Vec3 v) { return {static_cast<f64>(v.x), static_cast<f64>(v.y), static_cast<f64>(v.z)}; }

// The caster's triangles in world space and their box, which most shadow rays miss.
struct CasterTriangles {
  Vector<Dvec> v;  // three per triangle
  Dvec lo{1e30, 1e30, 1e30};
  Dvec hi{-1e30, -1e30, -1e30};
};

CasterTriangles caster_triangles(Caster caster) {
  const MeshSource source = caster_mesh(caster);
  const Mat4 frame = fixture_frame();
  CasterTriangles out;
  for (const u32 i : source.indices) {
    const Dvec p = dv(to_world(frame, source.positions[i]));
    out.v.push_back(p);
    out.lo = {std::min(out.lo.x, p.x), std::min(out.lo.y, p.y), std::min(out.lo.z, p.z)};
    out.hi = {std::max(out.hi.x, p.x), std::max(out.hi.y, p.y), std::max(out.hi.z, p.z)};
  }
  return out;
}

bool ray_hits_box(Dvec o, Dvec d, Dvec lo, Dvec hi) {
  f64 t0 = 0.0;
  f64 t1 = 1e30;
  const f64 os[3] = {o.x, o.y, o.z};
  const f64 ds[3] = {d.x, d.y, d.z};
  const f64 los[3] = {lo.x - 1e-6, lo.y - 1e-6, lo.z - 1e-6};
  const f64 his[3] = {hi.x + 1e-6, hi.y + 1e-6, hi.z + 1e-6};
  for (u32 a = 0; a < 3; ++a) {
    if (std::fabs(ds[a]) < 1e-15) {
      if (os[a] < los[a] || os[a] > his[a]) return false;
      continue;
    }
    f64 ta = (los[a] - os[a]) / ds[a];
    f64 tb = (his[a] - os[a]) / ds[a];
    if (ta > tb) std::swap(ta, tb);
    t0 = std::max(t0, ta);
    t1 = std::min(t1, tb);
    if (t0 > t1) return false;
  }
  return true;
}

// Does a ray from `o` along `d` hit any caster triangle (Möller-Trumbore, both faces)?
bool ray_blocked(const CasterTriangles& tris, Dvec o, Dvec d) {
  if (!ray_hits_box(o, d, tris.lo, tris.hi)) return false;
  for (u32 t = 0; t + 2 < tris.v.size(); t += 3) {
    const Dvec e1 = tris.v[t + 1] - tris.v[t];
    const Dvec e2 = tris.v[t + 2] - tris.v[t];
    const Dvec p = dcross(d, e2);
    const f64 det = ddot(e1, p);
    if (std::fabs(det) < 1e-18) continue;
    const f64 inv = 1.0 / det;
    const Dvec s = o - tris.v[t];
    const f64 u = ddot(s, p) * inv;
    if (u < 0.0 || u > 1.0) continue;
    const Dvec q = dcross(s, e1);
    const f64 w = ddot(d, q) * inv;
    if (w < 0.0 || u + w > 1.0) continue;
    if (ddot(e2, q) * inv > 1e-9) return true;
  }
  return false;
}

// Per pixel of a fixture picture: -1 not ground, 0 lit, 1 shadowed, by a CPU ray from the ground
// point under the pixel's centre to the sun. The ground is the plane y = 0 (the fixture's frame
// only turns about y), so the point is the camera ray's crossing of it, in double precision.
// `ground_y` moves the plane to where the ground's grid put it, and `lift` starts the ray that far
// up the ground's normal, as a traced shadow ray starts.
Vector<i8> cpu_shadow(Caster caster, Vec3 eye_local, Vec3 target_local, const Shot& shot,
                      f64 lift = 0.0, f64 ground_y = 0.0) {
  const Mat4 frame = fixture_frame();
  const Dvec eye = dv(to_world(frame, eye_local));
  const Dvec target = dv(to_world(frame, target_local));
  const Dvec f = dnormalize(target - eye);
  const Dvec s = dnormalize(dcross(f, Dvec{0.0, 1.0, 0.0}));
  const Dvec u = dcross(s, f);
  const f64 ty = std::tan(static_cast<f64>(Camera{}.fov_y) * 0.5);
  const f64 tx = ty * static_cast<f64>(k_width) / static_cast<f64>(k_height);
  const CasterTriangles tris = caster_triangles(caster);
  const Dvec sun = dv(k_sun);
  Vector<i8> out(k_width * k_height, static_cast<i8>(-1));
  for (u32 y = 0; y < k_height; ++y) {
    for (u32 x = 0; x < k_width; ++x) {
      const u32 p = y * k_width + x;
      if (!is_ground(shot, p)) continue;
      const f64 nx = (static_cast<f64>(x) + 0.5) / k_width * 2.0 - 1.0;
      const f64 ny = 1.0 - (static_cast<f64>(y) + 0.5) / k_height * 2.0;
      const Dvec d = f + s * (nx * tx) + u * (ny * ty);
      if (d.y >= 0.0) continue;
      const Dvec ground = eye + d * ((ground_y - eye.y) / d.y);
      out[p] = ray_blocked(tris, ground + Dvec{0.0, lift, 0.0}, sun) ? 1 : 0;
    }
  }
  return out;
}

// True when every ground pixel within `radius` pixels has the same CPU answer as `p`: the pixel is
// far enough from the shadow's edge that the filter cannot reach across it.
bool interior(const Vector<i8>& classes, u32 p, i32 radius) {
  const i32 px = static_cast<i32>(p % k_width);
  const i32 py = static_cast<i32>(p / k_width);
  for (i32 dy = -radius; dy <= radius; ++dy) {
    for (i32 dx = -radius; dx <= radius; ++dx) {
      const i32 x = px + dx;
      const i32 y = py + dy;
      if (x < 0 || y < 0 || x >= static_cast<i32>(k_width) || y >= static_cast<i32>(k_height))
        return false;
      if (classes[static_cast<u32>(y) * k_width + static_cast<u32>(x)] != classes[p]) return false;
    }
  }
  return true;
}

struct Agreement {
  u32 compared = 0;     // interior ground pixels
  u32 shadowed = 0;     // of those, the reference shadows
  u32 differ = 0;       // of those, the picture disagrees
  u32 band = 0;         // ground pixels near an edge, left out
  u32 penumbra = 0;     // of those, the picture is grey (the filter's)
  u32 band_differ = 0;  // of those, the picture is black or white against the reference's call
};

// A picture's shadow view against per-pixel classes (-1 skipped, 0 lit, 1 shadowed).
Agreement agree(const Shot& shot, const Vector<i8>& classes, i32 radius) {
  Agreement out;
  for (u32 p = 0; p < k_width * k_height; ++p) {
    if (classes[p] < 0 || !is_ground(shot, p)) continue;
    const u8 value = shot.frame.color[u64{p} * 4];
    const bool black = value == 0;
    const bool white = value == 255;
    if (!interior(classes, p, radius)) {
      ++out.band;
      if (!black && !white) ++out.penumbra;
      if ((classes[p] == 1 && white) || (classes[p] == 0 && black)) ++out.band_differ;
      continue;
    }
    ++out.compared;
    out.shadowed += classes[p] == 1 ? 1u : 0u;
    out.differ += (classes[p] == 1 ? !black : !white) ? 1u : 0u;
  }
  return out;
}

// The classes a picture's own shadow view gives: black shadowed, white lit, anything else (grey
// facing away, which the ground never is, or a penumbra) left out.
Vector<i8> picture_classes(const Shot& shot) {
  Vector<i8> out(k_width * k_height, static_cast<i8>(-1));
  for (u32 p = 0; p < k_width * k_height; ++p) {
    if (!is_ground(shot, p)) continue;
    const u8 value = shot.frame.color[u64{p} * 4];
    if (value == 0) out[p] = 1;
    if (value == 255) out[p] = 0;
  }
  return out;
}

u32 colour_differences(const Shot& a, const Shot& b) {
  u32 out = 0;
  for (u32 i = 0; i < a.frame.color.size() && i < b.frame.color.size(); ++i)
    out += a.frame.color[i] != b.frame.color[i] ? 1u : 0u;
  return out;
}

// How far the filter reaches, in this fixture's pixels: the cascades here are the scene's bounds
// or smaller, a few millimetres a texel against a pixel of about 2.6 cm, so the 4 x 4 footprint is
// well inside one pixel and two pixels covers it and the CPU-against-GPU disagreement on which side
// of an edge a pixel centre falls.
constexpr i32 k_edge_pixels = 2;

}  // namespace

TEST_CASE("renderer: the cascaded maps cast what a CPU ray finds, on every caster and path") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const RasterMode modes[2] = {RasterMode::Hardware, RasterMode::Vertex};
  for (const Caster caster : k_casters) {
    Shot first;
    for (const RasterMode mode : modes) {
      if (mode == RasterMode::Hardware && !gpu.device.features().mesh_shader) {
        MESSAGE("mesh path unavailable here: " << std::string(gpu.device.adapter().name)
                                               << " has no VK_EXT_mesh_shader");
        continue;
      }
      ShotOptions options;
      options.raster = mode;
      options.shadows = ShadowMode::Cascaded;
      const Shot shot = render(gpu.device, caster, options);
      REQUIRE(shot.available);
      REQUIRE(shot.csm);
      CHECK(shot.shadow_pairs > 0);
      const Vector<i8> reference = cpu_shadow(caster, k_eye_local, k_target_local, shot);
      const Agreement a = agree(shot, reference, k_edge_pixels);
      MESSAGE(std::string(raster_name(mode))
              << " " << std::string(caster_name(caster)) << ": " << shot.shadow_pairs
              << " pairs in the maps; " << a.compared << " interior ground pixels, " << a.shadowed
              << " shadowed, " << a.differ << " differ from a CPU ray; " << a.band
              << " edge pixels (" << a.penumbra << " grey, " << a.band_differ
              << " black or white against the CPU)");
      CHECK(a.shadowed > 200);
      CHECK(a.differ == 0);
      // On a failure, where: one character per 4 x 8 pixels, '#' both shadowed, '.' both lit,
      // 'L' the CPU shadowed and the map lit, 'S' the CPU lit and the map shadowed.
      if (a.differ != 0) {
        std::string art;
        for (u32 by = 0; by < k_height; by += 8) {
          for (u32 bx = 0; bx < k_width; bx += 4) {
            const u32 p = (by + 4) * k_width + bx + 2;
            const u8 v = shot.frame.color[u64{p} * 4];
            const i8 r = reference[p];
            char ch = '?';
            if (r < 0)
              ch = ' ';
            else if (r == 1 && v == 0)
              ch = '#';
            else if (r == 0 && v == 255)
              ch = '.';
            else if (r == 1)
              ch = 'L';  // CPU shadowed, map lit
            else
              ch = 'S';  // CPU lit, map shadowed
            art += ch;
          }
          art += '\n';
        }
        MESSAGE("\n" << art);
      }
      // Both paths draw the same triangles off the same grid into the maps.
      if (first.frame.color.empty()) {
        first = shot;
      } else {
        CHECK(colour_differences(first, shot) == 0);
        CHECK(id_differences(first, shot) == 0);
      }
    }
  }
}

// ---- casters outside the camera's frustum (renderer.md, "Shadows"; roadmap R54) ----------------
//
// Straight down at the card's shadow from 0.6 units, the card beside and above: every cluster of
// it is outside the frustum. The cascades, culled from the light, hold it; the ray-traced shadows
// hold it because the cull pass keeps a pair its frustum rejects when the pair is inside the view's
// frustum swept towards the sun (caster_volume.h). Until 2026-10-18 the traced shadow of this card
// was 0 pixels.
namespace {

const Vec3 k_off_eye{-0.85f, 0.6f, 0.0f};
const Vec3 k_off_target{-0.85f, 0.0f, 0.05f};

// On the CPU, with the renderer's own layout and fit: which of the scene's pairs are outside the
// camera's frustum, and of those which are inside the volume the cull pass keeps casters in.
struct OffFrustum {
  u32 caster_clusters = 0;   // the caster's
  u32 caster_outside = 0;    // of them, outside the frustum
  u32 caster_in_volume = 0;  // of those, inside the volume
  u32 in_volume = 0;         // every pair outside the frustum and inside the volume
};

OffFrustum off_frustum(Caster caster, Vec3 eye_local, Vec3 target_local, Vec3 caster_offset,
                       bool lights = false) {
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(make_scene(caster, data, error, 0.0f, caster_offset), error);
  const Mat4 frame = fixture_frame();
  Camera camera;
  camera.position = absolute(WorldPos::origin(), to_world(frame, eye_local));
  camera.target = absolute(WorldPos::origin(), to_world(frame, target_local));
  ViewSet views;
  REQUIRE(views.build(ViewSetDesc{}, k_width, k_height, &error));
  views.update(camera);
  const Mat4& view_proj = views[0].view_proj;  // the frame's space: the eye at the origin
  const Frustum frustum = frustum_from_view_proj(view_proj);
  const CasterBounds bounds{relative(data.center, camera.position), data.radius};
  CasterVolume volumes[gfx::k_max_caster_volumes];
  u32 volume_count = 0;
  REQUIRE(fit_directional_caster_volume(view_proj, k_sun, 0.0f, bounds, volumes[volume_count++]));
  if (lights) {
    // The renderer's stand-in point lights, where the frame puts them (lighting.h).
    RenderSettings settings;
    FrameLighting lighting;
    frame_lighting(data, 0, lighting_options(settings), lighting, camera.position);
    for (u32 l = 0; l < lighting.light_count; ++l) {
      const gfx::ResolveLight& light = lighting.lights[l];
      if (fit_point_caster_volume(view_proj, light.position_radius.xyz(), light.position_radius.w,
                                  bounds, volumes[volume_count])) {
        ++volume_count;
      }
    }
  }
  const Vec3 eye = relative(camera.position, WorldPos::origin());
  OffFrustum out;
  for (u32 i = 0; i < 2; ++i) {
    const geometry::ClusterMeshPart& part = data.parts[i];
    const Vec3 offset = i == 1 ? to_world(frame, caster_offset) : Vec3{};
    for (u32 c = 0; c < part.cluster_count; ++c) {
      const geometry::ClusterDesc& desc = data.lod.mesh.clusters[part.first_cluster + c];
      const Vec3 center = to_world(frame, desc.center) + offset - eye;
      const bool outside = !frustum_contains_sphere(frustum, center, desc.radius);
      bool kept = false;
      for (u32 v = 0; v < volume_count && outside && !kept; ++v)
        kept = caster_volume_contains_sphere(volumes[v], center, desc.radius);
      out.in_volume += kept ? 1u : 0u;
      if (i == 1) {
        ++out.caster_clusters;
        out.caster_outside += outside ? 1u : 0u;
        out.caster_in_volume += kept ? 1u : 0u;
      }
    }
  }
  return out;
}

u32 shadowed_ground(const Shot& shot) {
  u32 out = 0;
  for (u32 p = 0; p < k_width * k_height; ++p)
    out += is_ground(shot, p) && shadowed(shot, p) ? 1u : 0u;
  return out;
}

}  // namespace

TEST_CASE("renderer: a caster outside the camera's frustum casts into the maps and the rays") {
  const OffFrustum cpu = off_frustum(Caster::card, k_off_eye, k_off_target, Vec3{});
  MESSAGE("the card's " << cpu.caster_clusters << " clusters: " << cpu.caster_outside
                        << " outside the camera's frustum, " << cpu.caster_in_volume
                        << " of them inside its volume; " << cpu.in_volume
                        << " pairs of the scene outside the frustum and inside the volume");
  // The witnesses: without them the case below proves nothing.
  REQUIRE(cpu.caster_outside == cpu.caster_clusters);
  REQUIRE(cpu.caster_in_volume == cpu.caster_clusters);
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  ShotOptions options;
  options.eye = k_off_eye;
  options.target = k_off_target;
  options.shadows = ShadowMode::Cascaded;
  const Shot mapped = render(gpu.device, Caster::card, options);
  REQUIRE(mapped.available);
  const Vector<i8> reference = cpu_shadow(Caster::card, k_off_eye, k_off_target, mapped);
  const Agreement a = agree(mapped, reference, k_edge_pixels);
  MESSAGE("the card outside the frustum: maps shadow " << a.shadowed << " interior ground pixels ("
                                                       << a.differ << " differ from a CPU ray, "
                                                       << a.band << " edge pixels)");
  CHECK(a.shadowed > 1000);
  CHECK(a.differ == 0);

  options.shadows = ShadowMode::RayTraced;
  const Shot traced = render(gpu.device, Caster::card, options);
  if (!traced.available) {
    MESSAGE("part skipped: ray-traced shadows: " << traced.why);
    return;
  }
  // The rays now cast it: the maps' shadow within the filter's band, and a CPU ray's.
  CHECK(id_differences(traced, mapped) == 0);  // the same picture, shadowed two ways
  const Agreement rays = agree(mapped, picture_classes(traced), k_edge_pixels);
  const Agreement cpu_rays = agree(traced, reference, k_edge_pixels);
  options.off_frustum_casters = false;
  const Shot before = render(gpu.device, Caster::card, options);
  MESSAGE("ray-traced: " << shadowed_ground(traced) << " shadowed ground pixels, " << traced.casters
                         << " casters (" << cpu.in_volume
                         << " on the CPU); the maps differ from it on " << rays.differ << " of "
                         << rays.compared << " interior pixels (" << rays.shadowed
                         << " shadowed), a CPU ray on " << cpu_rays.differ
                         << "; without the volumes " << shadowed_ground(before)
                         << " shadowed ground pixels and " << before.casters << " casters");
  CHECK(rays.shadowed > 1000);
  CHECK(rays.differ == 0);
  CHECK(cpu_rays.shadowed > 1000);
  CHECK(cpu_rays.differ == 0);
  // Every caster is one the CPU's volume holds: the card's clusters and the ground's beyond the
  // view towards the sun, and nothing the cone test adds from this camera.
  CHECK(traced.casters == cpu.in_volume);
  // And without the volumes the camera's cut alone casts, which is nothing: the old answer.
  CHECK(shadowed_ground(before) == 0);
  CHECK(before.casters == 0);
  CHECK(id_differences(traced, before) == 0);
}

// ---- a deformed caster outside the frustum (renderer.md, "Shadows"; roadmap R84) ---------------
//
// The same card and camera under `--deform lattice`: every instance reads the pool, the card's
// cage swirls about its box's vertical axis and breathes, and the ground stays a plane (its cage's
// bottom face moves as one). Until 2026-10-18 a deformed instance outside the frustum was no
// caster — the caster run had no pool blocks, so its rest pose would have cast where its pose did
// not — and the card's traced shadow was 0 pixels here. Now the caster run is allocated and
// deformed like a drawn run, and the traced shadow is the maps' (whose cascades always had blocks)
// at both of two poses, which differ.
namespace {

constexpr f32 k_card_swirl = 0.1f;  // the lattice's amplitude: a corner of the card moves ~0.2

ShotOptions deformed_card(ShadowMode shadows, u64 frame) {
  ShotOptions options;
  options.eye = k_off_eye;
  options.target = k_off_target;
  options.shadows = shadows;
  options.deform = true;
  options.deform_kind = gfx::k_deform_lattice;
  options.deform_amplitude = k_card_swirl;
  options.frame_index = frame;
  return options;
}

}  // namespace

TEST_CASE("renderer: a deformed caster outside the frustum casts its pose into the rays") {
  const OffFrustum cpu = off_frustum(Caster::card, k_off_eye, k_off_target, Vec3{});
  REQUIRE(cpu.caster_outside == cpu.caster_clusters);
  REQUIRE(cpu.caster_in_volume == cpu.caster_clusters);
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  // Two poses: frame 0 and frame 55, where the swirl (sin 1.7 t) is near its peak.
  const u64 frames[2] = {0, 55};
  Shot traced[2];
  Shot mapped[2];
  for (u32 f = 0; f < 2; ++f) {
    mapped[f] = render(gpu.device, Caster::card, deformed_card(ShadowMode::Cascaded, frames[f]));
    REQUIRE(mapped[f].available);
    traced[f] = render(gpu.device, Caster::card, deformed_card(ShadowMode::RayTraced, frames[f]));
    if (!traced[f].available) {
      MESSAGE("skipped: ray-traced shadows: " << traced[f].why);
      return;
    }
    const Agreement rays = agree(mapped[f], picture_classes(traced[f]), k_edge_pixels);
    MESSAGE("frame " << frames[f] << ": the maps shadow " << shadowed_ground(mapped[f])
                     << " ground pixels, the rays " << shadowed_ground(traced[f]) << " ("
                     << traced[f].casters << " casters, " << traced[f].deform_entries
                     << " pool entries, " << traced[f].deform_overflow
                     << " refused); they differ on " << rays.differ << " of " << rays.compared
                     << " interior pixels (" << rays.shadowed << " shadowed), " << rays.band
                     << " edge pixels");
    CHECK(id_differences(traced[f], mapped[f]) == 0);  // the same picture, shadowed two ways
    CHECK(traced[f].deform_overflow == 0);
    CHECK(rays.shadowed > 1000);
    CHECK(rays.differ == 0);
  }
  // The shadow moves with the pose, under the rays as under the maps.
  const Compared moved = compare_ground(traced[0], traced[1]);
  const Compared moved_maps = compare_ground(mapped[0], mapped[1]);
  MESSAGE("the two poses' traced shadows differ on " << moved.differ << " ground pixels, the "
                                                     << "maps' on " << moved_maps.differ);
  CHECK(moved.differ > 100);
  CHECK(moved_maps.differ > 100);
  // Without the volumes nothing outside the frustum casts: the old answer, and the one a deformed
  // card got with them until the caster run had blocks.
  ShotOptions before = deformed_card(ShadowMode::RayTraced, frames[1]);
  before.off_frustum_casters = false;
  const Shot old = render(gpu.device, Caster::card, before);
  CHECK(shadowed_ground(old) == 0);
  CHECK(id_differences(traced[1], old) == 0);
}

TEST_CASE("renderer: a deformed caster the pool has no room for casts nothing, not its rest pose") {
  // **The overflow rule for casters** (renderer.md, "Shadows"). A drawn entry the pool's budget
  // refuses draws its rest pose — a character momentarily at rest beats a hole. A caster entry it
  // refuses is **not built at all**: its rest pose would cast a shadow where the pose has none (a
  // terrain level's rest heights can be metres from the field it draws), and a wrong shadow is
  // worse than a missing one. Under `--deform identity` the pose *is* the rest pose, so the two
  // rules can be told apart by what they cast: the rest-pose rule would cast the card's whole
  // shadow however small the pool, this one loses the shadow of every caster it refused. One KiB is
  // 85 vertices: the drawn run's first ground cluster takes 35 or more, and what is left cannot
  // hold every cluster of the card (the witness below), so at least one of them is refused — which
  // ones depends on the order the cull's atomics appended them in. `clas_records_tests.cpp` holds
  // the rule entry by entry.
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(make_scene(Caster::card, data, error), error);
  u32 ground_min = ~0u;
  u32 card_vertices = 0;
  for (u32 c = 0; c < data.parts[0].cluster_count; ++c) {
    ground_min =
        std::min(ground_min, data.lod.mesh.clusters[data.parts[0].first_cluster + c].vertex_count);
  }
  for (u32 c = 0; c < data.parts[1].cluster_count; ++c)
    card_vertices += data.lod.mesh.clusters[data.parts[1].first_cluster + c].vertex_count;
  const u32 budget = static_cast<u32>(1024 / (3 * sizeof(f32)));
  MESSAGE("smallest ground cluster " << ground_min << " vertices, the card's clusters "
                                     << card_vertices << ", a budget of " << budget);
  REQUIRE(budget - ground_min < card_vertices);
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  ShotOptions options = deformed_card(ShadowMode::RayTraced, 0);
  options.deform_kind = gfx::k_deform_identity;
  const Shot roomy = render(gpu.device, Caster::card, options);
  if (!roomy.available) {
    MESSAGE("skipped: " << roomy.why);
    return;
  }
  options.deform_pool_kib = 1;
  const Shot cramped = render(gpu.device, Caster::card, options);
  // Where the cramped picture is shadowed and the roomy one lit: a shadow the rule cast that the
  // pose has not. None, since it only ever leaves casters out.
  u32 extra = 0;
  for (u32 p = 0; p < k_width * k_height; ++p)
    extra += is_ground(cramped, p) && shadowed(cramped, p) && !shadowed(roomy, p) ? 1u : 0u;
  MESSAGE("with room: " << shadowed_ground(roomy) << " shadowed ground pixels, "
                        << roomy.deform_overflow << " entries refused; one KiB: "
                        << shadowed_ground(cramped) << " shadowed (" << extra
                        << " the roomy frame lights), " << cramped.deform_overflow
                        << " entries refused, " << cramped.casters << " casters culled");
  CHECK(roomy.deform_overflow == 0);
  CHECK(shadowed_ground(roomy) > 1000);
  CHECK(cramped.deform_overflow > 0);
  CHECK(cramped.casters == roomy.casters);  // the cull keeps them; the records pass leaves them out
  CHECK(id_differences(cramped, roomy) == 0);  // the rest pose is the pose: the same picture
  CHECK(shadowed_ground(cramped) < shadowed_ground(roomy));
  CHECK(extra == 0);
}

TEST_CASE("renderer: a caster outside the frustum on the side away from the light casts nothing") {
  // The same card moved 0.85 units the other way from the view, away from the sun: outside the
  // frustum, and behind the view's receivers as the sun sees them, so no ray from the view towards
  // the sun can meet it. It must not be a caster — which is what says the volume is tight and not
  // the whole scene — and it casts nothing, with the volumes or without them.
  const Vec3 away{-1.7f, 0.0f, 0.0f};
  const OffFrustum cpu = off_frustum(Caster::card, k_off_eye, k_off_target, away);
  MESSAGE("the card away from the light: "
          << cpu.caster_outside << " of its " << cpu.caster_clusters
          << " clusters outside the frustum, " << cpu.caster_in_volume << " inside the volume; "
          << cpu.in_volume << " pairs of the scene in the volume");
  REQUIRE(cpu.caster_outside == cpu.caster_clusters);
  CHECK(cpu.caster_in_volume == 0);
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  ShotOptions options;
  options.eye = k_off_eye;
  options.target = k_off_target;
  options.caster_offset = away;
  options.shadows = ShadowMode::RayTraced;
  const Shot traced = render(gpu.device, Caster::card, options);
  if (!traced.available) {
    MESSAGE("skipped: " << traced.why);
    return;
  }
  // The ground's pairs beyond the view towards the sun are casters; the card's are not.
  CHECK(traced.casters == cpu.in_volume);
  CHECK(shadowed_ground(traced) == 0);
}

TEST_CASE("renderer: a point light's casters outside the frustum are its hull's") {
  // With the stand-in point lights on, every light of the array casts a traced shadow, and each
  // one's volume — the hull of the frustum and the light, within its reach — keeps its own
  // off-frustum casters. The GPU's casters are the CPU's own fit over the sun's volume and the
  // lights', and the sun's shadow is the same bytes with the lights' volumes or without them:
  // a caster outside the sun's sweep cannot meet a ray from the view towards the sun.
  const OffFrustum sun = off_frustum(Caster::card, k_off_eye, k_off_target, Vec3{});
  const OffFrustum all = off_frustum(Caster::card, k_off_eye, k_off_target, Vec3{}, true);
  MESSAGE("outside the frustum and inside a volume: " << sun.in_volume << " pairs for the sun, "
                                                      << all.in_volume << " with the lights");
  CHECK(all.in_volume > sun.in_volume);
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  ShotOptions options;
  options.eye = k_off_eye;
  options.target = k_off_target;
  options.lights = true;
  const Shot lit = render(gpu.device, Caster::card, options);
  if (!lit.available) {
    MESSAGE("skipped: " << lit.why);
    return;
  }
  options.lights = false;
  const Shot alone = render(gpu.device, Caster::card, options);
  MESSAGE(lit.casters << " casters with the point lights, " << alone.casters << " without; "
                      << colour_differences(lit, alone)
                      << " colour bytes of the sun's shadow differ");
  CHECK(lit.casters == all.in_volume);
  CHECK(alone.casters == sun.in_volume);
  CHECK(colour_differences(lit, alone) == 0);
  CHECK(id_differences(lit, alone) == 0);
}

TEST_CASE("renderer: the off-frustum casters change no pixel's surface on any path") {
  // Casters are never drawn: with the volumes on and off the same pairs are under the same pixels
  // on the mesh path, the vertex path and the ray path, whose primary rays pass through a caster;
  // only the shadow differs, and in a view that does not ask for it nothing does. The off-frustum
  // card's camera, where the card and the ground beyond the view towards the sun are casters.
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  if (!gpu.device.features().cluster_acceleration_structure || !gpu.device.features().ray_query) {
    MESSAGE("skipped: " << unavailable_reason(RenderAvailability::NoAccelerationStructures,
                                              gpu.device));
    return;
  }
  const RasterMode modes[3] = {RasterMode::Hardware, RasterMode::Vertex, RasterMode::RayTrace};
  for (const RasterMode mode : modes) {
    if (mode == RasterMode::Hardware && !gpu.device.features().mesh_shader) continue;
    for (const u32 view : {static_cast<u32>(gfx::ResolveMode::Shadow),
                           static_cast<u32>(gfx::ResolveMode::Normals)}) {
      ShotOptions options;
      options.raster = mode;
      options.view_mode = view;
      options.eye = k_off_eye;
      options.target = k_off_target;
      const Shot on = render(gpu.device, Caster::card, options);
      options.off_frustum_casters = false;
      const Shot off = render(gpu.device, Caster::card, options);
      REQUIRE(on.available);
      const u32 ids = id_differences(on, off);
      const u32 colour = colour_differences(on, off);
      MESSAGE(std::string(raster_name(mode))
              << " " << std::string(view_mode_name(view)) << ": " << on.casters << " casters with "
              << "the volumes, " << off.casters << " without; " << on.visible << " and "
              << off.visible << " drawn pairs; " << ids << " id words and " << colour
              << " colour bytes differ");
      CHECK(on.casters > off.casters);
      CHECK(on.visible == off.visible);
      CHECK(ids == 0);
      if (view == static_cast<u32>(gfx::ResolveMode::Normals)) CHECK(colour == 0);
      if (view == static_cast<u32>(gfx::ResolveMode::Shadow)) CHECK(colour > 0);
    }
  }
}

TEST_CASE("renderer: texel LOD preserves the near fixture on both cascade raster paths") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  for (const Caster caster : {Caster::card, Caster::slab, Caster::cube, Caster::ell}) {
    ShotOptions options;
    options.shadows = ShadowMode::Cascaded;
    options.shadow_texel_lod = false;
    const Shot before = render(gpu.device, caster, options);
    options.shadow_texel_lod = true;
    const Shot texels = render(gpu.device, caster, options);
    options.raster = RasterMode::Vertex;
    const Shot vertex = render(gpu.device, caster, options);
    MESSAGE(std::string(caster_name(caster))
            << ": camera/texel " << colour_differences(before, texels)
            << " colour bytes, hw/vertex " << colour_differences(texels, vertex)
            << " colour bytes; pairs " << before.shadow_pairs << " / " << texels.shadow_pairs);
    CHECK(colour_differences(before, texels) == 0);
    CHECK(id_differences(before, texels) == 0);
    CHECK(colour_differences(texels, vertex) == 0);
    CHECK(id_differences(texels, vertex) == 0);
  }
}

// **The cascades cut by their own texels, on a receiver that is simplified** (R103; renderer.md,
// "Cascaded shadow maps"). The fixture's casters are their own roots, so the texel cut cannot move
// them; a heightfield's 129 x 129 grid has a real LOD DAG and shadows itself. Each map size is
// drawn three times — the picture's cut alone, the texel cut, the texel cut on the vertex path —
// and held to the rule the Khronos case holds the maps to against the rays: outside a band round
// every shadow edge as wide as the filter's footprint, the texel cut answers every pixel as the
// picture's cut does. The band is per pixel: the footprint (`k_footprint_texels` of the cascade
// that holds the pixel's depth) in the pixel's own size at its depth. The picture (ids) is the
// same, the two raster paths draw the same shadow to the byte, and the texel cut never draws more
// pairs into the maps than the picture's (it is the coarser of the two cuts, never a finer one).
TEST_CASE(
    "renderer: a cascade cut by its texels shadows a simplified receiver as the picture's cut "
    "does") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.heightfield_grid = 129;
  desc.cache = false;
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  constexpr u32 width = 480;
  constexpr u32 height = 270;
  // The 3 x 3 bilinear filter's reach: 4 x 4 texels (gfx.md, "The sun's cascaded shadow maps").
  constexpr f32 k_footprint_texels = 4.0f;
  struct OrbitView {
    f32 distance;
    f32 yaw;
    f32 pitch;
  };
  const OrbitView k_views[] = {{9.0f, 0.8f, 0.35f}, {4.0f, 2.6f, 0.2f}, {2.5f, 4.2f, 0.12f}};
  u64 opposite_total = 0;
  for (const u32 resolution : {64u, 256u, 2048u}) {
    for (const OrbitView& v : k_views) {
      CapturedFrame shots[3];
      u32 pairs[3] = {};
      ShadowCascades fit;
      FrameDesc frame;
      frame.camera = orbit_camera_at(data.center, data.radius, v.distance, v.yaw, v.pitch);
      frame.view_mode = static_cast<u32>(gfx::ResolveMode::Shadow);
      for (u32 run = 0; run < 3; ++run) {
        RenderSettings settings;
        settings.raster = run == 2 ? RasterMode::Vertex : RasterMode::Hardware;
        settings.shadows = ShadowMode::Cascaded;
        settings.shadow_map = resolution;
        settings.shadow_texel_lod = run != 0;
        settings.lights = false;
        ResolvedSettings resolved;
        resolve_settings(settings, gpu.device.features(), &data, resolved);
        REQUIRE(resolved.shadow_texel_lod == (run != 0));
        GpuScene scene;
        REQUIRE_MESSAGE(scene.create(gpu.device, data, resolved, &error), error);
        SceneRenderer renderer;
        SceneRenderer::Desc rdesc;
        rdesc.width = width;
        rdesc.height = height;
        REQUIRE_MESSAGE(renderer.create(gpu.device, scene, resolved, rdesc, &error), error);
        CaptureChannels channels;
        channels.ids = true;
        channels.depth = true;
        REQUIRE_MESSAGE(renderer.capture(frame, channels, shots[run], &error), error);
        pairs[run] = renderer.stats().shadow_pairs;
        if (run == 0) fit = renderer.shadow_cascades();
      }
      // The band's radius at a pixel: the footprint of the cascade whose slice holds its depth, in
      // the pixel's own size there (reversed-Z depth is znear over the view depth).
      const f32 pixel_at_unit =
          2.0f * std::tan(frame.camera.fov_y * 0.5f) / static_cast<f32>(height);
      auto band_at = [&](u32 p) -> i32 {
        const f32 depth = shots[0].depth[p];
        if (!(depth > 0.0f)) return 0;
        const f32 view_z = frame.camera.znear / depth;
        u32 c = 0;
        while (c + 1 < fit.count && view_z > fit.splits[c + 1])
          ++c;
        const f32 reach = k_footprint_texels * fit.cascades[c].texel_world;
        return static_cast<i32>(std::ceil(reach / (view_z * pixel_at_unit)));
      };
      auto answer = [&](const CapturedFrame& f, u32 p) -> i32 {
        if (f.ids[u64{p} * k_id_words] == k_no_id) return -1;
        const u8 c = f.color[u64{p} * 4];
        return c == 0 ? 0 : (c == 255 ? 1 : 2);  // black, white, grey (a penumbra or facing away)
      };
      u32 changed = 0;
      u32 opposite = 0;
      u32 interior = 0;
      u32 path_differ = 0;
      for (u32 y = 0; y < height; ++y) {
        for (u32 x = 0; x < width; ++x) {
          const u32 p = y * width + x;
          const i32 before = answer(shots[0], p);
          const i32 after = answer(shots[1], p);
          changed += shots[0].color[u64{p} * 4] != shots[1].color[u64{p} * 4] ? 1u : 0u;
          path_differ += shots[1].color[u64{p} * 4] != shots[2].color[u64{p} * 4] ? 1u : 0u;
          if (before < 0 || before == 2) continue;
          const i32 band = band_at(p);
          bool inside = true;
          for (i32 dy = -band; dy <= band && inside; ++dy) {
            for (i32 dx = -band; dx <= band && inside; ++dx) {
              const i32 nx = static_cast<i32>(x) + dx;
              const i32 ny = static_cast<i32>(y) + dy;
              inside =
                  nx >= 0 && ny >= 0 && nx < static_cast<i32>(width) &&
                  ny < static_cast<i32>(height) &&
                  answer(shots[0], static_cast<u32>(ny) * width + static_cast<u32>(nx)) == before;
            }
          }
          if (!inside) continue;
          ++interior;
          if (after != before && after != 2) ++opposite;
        }
      }
      opposite_total += opposite;
      MESSAGE("map " << resolution << ", view at " << v.distance << " radii: pairs " << pairs[0]
                     << " -> " << pairs[1] << ", " << fit.count << " cascade(s), texels "
                     << fit.cascades[0].texel_world << " to "
                     << fit.cascades[fit.count - 1].texel_world << " m; changed pixels " << changed
                     << ", interior " << interior << ", opposite there " << opposite
                     << ", hw/vertex differences " << path_differ);
      CHECK(pairs[1] <= pairs[0]);
      CHECK(opposite == 0);
      CHECK(shots[0].ids == shots[1].ids);
      CHECK(shots[1].color == shots[2].color);
      CHECK(shots[1].ids == shots[2].ids);
    }
  }
  MESSAGE("opposite answers inside the footprint's band, every map and view: " << opposite_total);
}

TEST_CASE("renderer: the cascades' light-view cull never changes the shadow") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  // The fixture: the cascades are small beside the ground, so their boxes cull.
  for (const Caster caster : {Caster::cube, Caster::ell}) {
    for (const u32 view :
         {static_cast<u32>(gfx::ResolveMode::Shadow), static_cast<u32>(gfx::ResolveMode::Shaded)}) {
      ShotOptions options;
      options.shadows = ShadowMode::Cascaded;
      options.view_mode = view;
      const Shot culled = render(gpu.device, caster, options);
      options.shadow_frustum = false;
      const Shot every = render(gpu.device, caster, options);
      const u32 differ = colour_differences(culled, every);
      MESSAGE(std::string(caster_name(caster))
              << " " << std::string(view_mode_name(view)) << ": " << culled.shadow_pairs
              << " pairs in the maps culled, " << every.shadow_pairs << " unculled; " << differ
              << " colour bytes differ");
      CHECK(differ == 0);
      CHECK(id_differences(culled, every) == 0);
    }
  }

  // The heightfield from close in, where the near cascades hold a small part of it, and the same
  // deformed, so the cascades' own pool blocks are exercised too.
  for (const bool deform : {false, true}) {
    SceneDesc desc;
    desc.meshes.push_back("");  // the procedural heightfield
    desc.heightfield_grid = 129;
    desc.cache = false;  // nothing here touches the tree's derived-data cache
    SceneData data;
    std::string error;
    REQUIRE_MESSAGE(load_scene(desc, data, error), error);
    CapturedFrame shots[2];
    u32 pairs[2] = {};
    for (u32 k = 0; k < 2; ++k) {
      RenderSettings settings;
      settings.shadows = ShadowMode::Cascaded;
      settings.shadow_frustum = k == 0;
      settings.deform = deform;
      settings.deform_kind = gfx::k_deform_wave;
      ResolvedSettings resolved;
      resolve_settings(settings, gpu.device.features(), &data, resolved);
      REQUIRE(check_availability(resolved, gpu.device.features()) == RenderAvailability::Ok);
      REQUIRE(resolved.csm);
      GpuScene scene;
      REQUIRE_MESSAGE(scene.create(gpu.device, data, resolved, &error), error);
      SceneRenderer renderer;
      SceneRenderer::Desc rdesc;
      rdesc.width = 320;
      rdesc.height = 180;
      REQUIRE_MESSAGE(renderer.create(gpu.device, scene, resolved, rdesc, &error), error);
      FrameDesc frame;
      frame.camera = orbit_camera_at(data.center, data.radius, 9.0f, 0.8f, 0.35f);
      frame.frame_index = 7;
      CaptureChannels channels;
      channels.ids = true;
      REQUIRE_MESSAGE(renderer.capture(frame, channels, shots[k], &error), error);
      pairs[k] = renderer.stats().shadow_pairs;
      CHECK(renderer.stats().deform_overflow_entries == 0);
    }
    u32 differ = 0;
    for (u32 i = 0; i < shots[0].color.size(); ++i)
      differ += shots[0].color[i] != shots[1].color[i] ? 1u : 0u;
    u32 id_differ = 0;
    for (u32 i = 0; i < shots[0].ids.size(); ++i)
      id_differ += shots[0].ids[i] != shots[1].ids[i] ? 1u : 0u;
    MESSAGE("heightfield" << std::string(deform ? " under --deform wave" : "") << ": " << pairs[0]
                          << " pairs in the maps culled, " << pairs[1] << " unculled; " << differ
                          << " colour bytes and " << id_differ << " id words differ");
    CHECK(pairs[0] < pairs[1]);  // the witness: the boxes did cull
    CHECK(differ == 0);
    CHECK(id_differ == 0);
  }
}

TEST_CASE("renderer: mapped and ray-traced shadows agree away from the filter's edge") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  if (!gpu.device.features().cluster_acceleration_structure || !gpu.device.features().ray_query) {
    MESSAGE("ray-traced shadows unavailable here: " << unavailable_reason(
                RenderAvailability::NoAccelerationStructures, gpu.device));
    return;
  }
  for (const Caster caster : k_casters) {
    ShotOptions options;
    options.shadows = ShadowMode::RayTraced;
    const Shot traced = render(gpu.device, caster, options);
    options.shadows = ShadowMode::Cascaded;
    const Shot mapped = render(gpu.device, caster, options);
    CHECK(id_differences(traced, mapped) == 0);  // the same picture, shadowed two ways
    const Agreement a = agree(mapped, picture_classes(traced), k_edge_pixels);
    MESSAGE(std::string(caster_name(caster))
            << ": " << a.compared << " interior ground pixels, " << a.shadowed
            << " shadowed by rays, " << a.differ << " differ in the maps; " << a.band
            << " edge pixels (" << a.penumbra << " grey, " << a.band_differ
            << " black or white against the rays), 3x3 bilinear PCF");
    CHECK(a.shadowed > 200);
    CHECK(a.differ == 0);
  }
}

// ---- rays near, maps far (renderer.md, "Rays near, maps far"; R102) ----------------------------
//
// The hybrid against the two it is made of, on every caster through the renderer's own chain (the
// cluster structures, the cascades, the casters' horizon): with the rays' distance at the median
// distance of the shadow the rays draw, every ground pixel nearer than it is the traced picture's
// byte, every one past it plus the band the mapped picture's, and every one in the band between the
// two; the same ids throughout. With the distance past the scene the hybrid is the traced picture
// whole, and with none the mapped one whole.
TEST_CASE("renderer: the hybrid draws the rays' shadow near the eye and the maps' far from it") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  if (!gpu.device.features().cluster_acceleration_structure || !gpu.device.features().ray_query) {
    MESSAGE("ray-traced shadows unavailable here: " << unavailable_reason(
                RenderAvailability::NoAccelerationStructures, gpu.device));
    return;
  }
  for (const Caster caster : k_casters) {
    ShotOptions options;
    options.depth = true;
    options.shadows = ShadowMode::RayTraced;
    const Shot traced = render(gpu.device, caster, options);
    options.shadows = ShadowMode::Cascaded;
    const Shot mapped = render(gpu.device, caster, options);
    REQUIRE(traced.frame.depth.size() == k_width * k_height);
    // Each pixel's distance from the eye, off its depth through the view's own matrix.
    const Mat4 inverse_view_proj = inverse(traced.view_proj);
    Vector<f32> distance(k_width * k_height, 0.0f);
    Vector<f32> shadowed_distances;
    for (u32 p = 0; p < k_width * k_height; ++p) {
      const f32 depth = traced.frame.depth[p];
      if (depth <= 0.0f) continue;
      const f32 x =
          (static_cast<f32>(p % k_width) + 0.5f) / static_cast<f32>(k_width) * 2.0f - 1.0f;
      const f32 y =
          1.0f - (static_cast<f32>(p / k_width) + 0.5f) / static_cast<f32>(k_height) * 2.0f;
      distance[p] = length(unproject(inverse_view_proj, x, y, depth));
      if (is_ground(traced, p) && shadowed(traced, p)) shadowed_distances.push_back(distance[p]);
    }
    REQUIRE(shadowed_distances.size() > 200);
    std::sort(shadowed_distances.begin(), shadowed_distances.end());
    const f32 ray_distance = shadowed_distances[shadowed_distances.size() / 2];
    constexpr f32 k_band = 0.25f;
    // A millimetre of slack: the matrix's inverse against the resolve's own reconstruction.
    constexpr f32 k_slack = 1.0e-3f;
    options.shadows = ShadowMode::Hybrid;
    options.shadow_ray_distance_m = ray_distance;
    options.shadow_ray_band_m = k_band;
    const Shot hybrid = render(gpu.device, caster, options);
    REQUIRE(hybrid.hybrid);
    CHECK(id_differences(hybrid, traced) == 0);
    // The rays with the same casters: the hybrid's end at its reach, so the traced picture it is
    // held to near the eye is the one with R94's horizon there (what the horizon itself costs a
    // near pixel is counted against the plain rays beside it).
    ShotOptions horizon = options;
    horizon.shadows = ShadowMode::RayTraced;
    horizon.rt_caster_horizon = true;
    horizon.shadow_distance = ray_distance + k_band;
    const Shot traced_horizon = render(gpu.device, caster, horizon);
    u32 n_near = 0, near_shadowed = 0, near_differ = 0, near_plain = 0;
    u32 n_far = 0, far_shadowed = 0, far_differ = 0;
    u32 n_band = 0, band_outside = 0;
    for (u32 p = 0; p < k_width * k_height; ++p) {
      if (distance[p] <= 0.0f) continue;
      const u8 got = hybrid.frame.color[u64{p} * 4];
      const u8 ray = traced_horizon.frame.color[u64{p} * 4];
      const u8 map = mapped.frame.color[u64{p} * 4];
      if (distance[p] < ray_distance - k_slack) {
        ++n_near;
        near_shadowed += ray == 0 ? 1u : 0u;
        near_differ += got != ray ? 1u : 0u;
        near_plain += got != traced.frame.color[u64{p} * 4] ? 1u : 0u;
      } else if (distance[p] > ray_distance + k_band + k_slack) {
        ++n_far;
        far_shadowed += map == 0 ? 1u : 0u;
        far_differ += got != map ? 1u : 0u;
      } else {
        ++n_band;
        band_outside += got + 1 < std::min(ray, map) || got > std::max(ray, map) + 1 ? 1u : 0u;
      }
    }
    MESSAGE(std::string(caster_name(caster))
            << ": rays within " << ray_distance << " m, maps past " << ray_distance + k_band
            << " m: " << n_near << " near pixels (" << near_shadowed << " shadowed), "
            << near_differ << " not the rays' (" << near_plain << " not the rays' without the "
            << "horizon); " << n_far << " far (" << far_shadowed << " shadowed), " << far_differ
            << " not the maps'; " << n_band << " in the band, " << band_outside
            << " outside the two; " << hybrid.horizon_casters << " casters past the horizon");
    CHECK(near_shadowed > 50);
    CHECK(far_shadowed > 50);
    CHECK(near_differ == 0);
    CHECK(far_differ == 0);
    CHECK(band_outside == 0);
    // The two ends.
    options.shadow_ray_distance_m = 1.0e6f;
    const Shot all_rays = render(gpu.device, caster, options);
    CHECK(colour_differences(all_rays, traced) == 0);
    options.shadow_ray_distance_m = 0.0f;
    options.shadow_ray_band_m = 0.0f;
    const Shot all_maps = render(gpu.device, caster, options);
    CHECK(colour_differences(all_maps, mapped) == 0);
  }
}

// ---- a shadow ray leaves its surface by that surface's own grid ---------------------------------
//
// The regression case for the ashlar ruins' missing shadows (docs/subsystems/renderer.md,
// "Shadows"): the fixture's casters, a metre and a half tall, on a ground that is one mesh five
// kilometres across (`ground_mesh`'s reach) — a 7.6 cm grid, drawn 3 cm below its floats, and a
// scene 3.9 km in radius, as the ruins' walls stand on a 5 km terrain with a 7.8 cm grid. Until
// 2026-09-26 every shadow ray left its surface by a thousandth of the *scene's* radius, 3.9 m here,
// and started above every caster: the sand beside a wall was lit. The fixture's other cases could
// not see it, because their scene is only as big as its caster. Now a ray leaves by `ray_offset`:
// one step of the receiver's own grid plus the float term the scene's reach sets. The CPU
// reference fires the same ray, from the same lifted point on the plane the grid drew, at the
// caster's triangles in double precision, so every interior ground pixel must agree with it.
// Measured when it was written: with the old offset every shadowed pixel differs (297, 334, 899
// and 903 of them, card to ell); with the float term alone and no grid term the ray starts inside
// the ground's float plane and 32,513 to 34,370 of about 34,000 lit pixels are black; with the grid
// term alone it passes, 15 to 20 edge pixels moving. What the float term is for — a small mesh far
// from the origin, whose own grid is finer than the float arithmetic — is measured on the ruins.
TEST_CASE("renderer: a shadow ray leaves the ground by the ground's own grid, not the scene's") {
  constexpr f32 k_reach = 2500.0f;
  // The offset the rule says a ray from this ground leaves by (lighting.h, `k_shadow_bias_*`):
  // one step of the ground's own grid and 2^-18 of the scene's reach. It is worked out here from
  // the rule and the scene, not read back from the renderer, so a renderer that offsets by
  // anything else draws a different shadow from the CPU's.
  f64 lift = 0.0;
  f64 ground_y = 0.0;  // where the grid put the plane y = 0
  {
    SceneData data;
    std::string error;
    REQUIRE_MESSAGE(make_scene(Caster::cube, data, error, k_reach), error);
    const geometry::ClusterMeshPart& part = data.parts[k_ground];
    const f32 grid = part.quant_scale * data.instances[k_ground].scale_max;
    // The scene's reach is measured from the frame's eye (ADR-0053; lighting.cpp): the camera
    // `render` draws the fixture from.
    const WorldPos eye = absolute(WorldPos::origin(), to_world(fixture_frame(), k_eye_local));
    const f32 reach = length(relative(data.center, eye)) + data.radius;
    lift = static_cast<f64>(k_shadow_bias_steps * grid + k_shadow_bias_relative * reach);
    const f64 origin = static_cast<f64>(part.quant_origin.y);
    const f64 step = static_cast<f64>(part.quant_scale);
    ground_y = origin + std::round(-origin / step) * step;
    MESSAGE("the scene's radius " << data.radius << ", the ground's grid step " << grid
                                  << ", the ground drawn " << ground_y
                                  << " off its floats: a ray leaves the ground " << lift
                                  << " up, where a thousandth of the radius was "
                                  << 1.0e-3f * data.radius);
    CHECK(data.radius > 3000.0f);   // the witnesses: the scene is as big as the ruins',
    CHECK(ground_y < -0.3 * step);  // and the surface drawn is below the one traced
    CHECK(ground_y + lift > 0.0);
    // The casters' lowest point is 0.4 up (the ell's foot) and their tops 1.5 to 1.6: an offset
    // that is a small part of that is one the shadow survives; 3.5 was none of it.
    CHECK(lift < 0.1);
  }
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  if (!gpu.device.features().cluster_acceleration_structure || !gpu.device.features().ray_query) {
    MESSAGE("ray-traced shadows unavailable here: " << unavailable_reason(
                RenderAvailability::NoAccelerationStructures, gpu.device));
    return;
  }
  for (const Caster caster : k_casters) {
    ShotOptions options;
    options.shadows = ShadowMode::RayTraced;
    options.ground_reach = k_reach;
    const Shot traced = render(gpu.device, caster, options);
    REQUIRE(traced.available);
    const Vector<i8> reference =
        cpu_shadow(caster, k_eye_local, k_target_local, traced, lift, ground_y);
    const Agreement a = agree(traced, reference, k_edge_pixels);
    MESSAGE(std::string(caster_name(caster))
            << " on a 5 km ground: " << a.compared << " interior ground pixels, " << a.shadowed
            << " shadowed by a CPU ray, " << a.differ << " differ in the traced picture; " << a.band
            << " edge pixels (" << a.band_differ << " against the CPU)");
    CHECK(a.shadowed > 200);  // the witness: the caster does cast, as on the small ground
    CHECK(a.differ == 0);
  }
}

// ---- the Khronos samples: mapped against traced, and what a cut other than the picture's costs --
//
// The measurement the design rests on (docs/subsystems/renderer.md, "Shadows"), kept as code so it
// can be re-taken rather than re-argued. Skipped by default: it needs the samples
// (`tools/fetch-samples.ps1`) and a device that traces, and it renders a few hundred frames. Run
// it, in a release build and under the GPU lock, with
//
//     engine_renderer_tests -tc='renderer: cascaded shadow maps on the Khronos samples' -ns -s
//
// Every sample stands on a ground six radii wide and is drawn from sixteen cameras (eight yaws, at
// engine-view's pitch and low over the horizon), in the shadow view, ray-traced and mapped, and the
// two are compared the way the fixture's case compares them: a pixel is interior when every pixel
// within `k_sample_edge` of it has the rays' own answer, and there the maps must give the same
// answer; the band around an edge is counted. The same frames are drawn again with the cascades'
// cut coarser than the picture's (`RenderSettings::shadow_lod_scale` 2 and 4), which is what a
// light-view LOD choice would do to the maps, and each configuration is timed.
namespace {

constexpr u32 k_sample_w = 1280;
constexpr u32 k_sample_h = 720;
// A cascade here is the scene's bounds, 8.5 sample radii across 2048 texels, against a pixel of
// about 3.3 thousandths of a radius at the orbit's distance: the filter's two-texel reach is about
// two and a half pixels.
constexpr i32 k_sample_edge = 3;

struct SampleCounts {
  u64 interior = 0;
  u64 shadowed = 0;
  u64 acne = 0;  // interior: lit by the rays, black in the maps
  u64 leak = 0;  // interior: shadowed by the rays, white in the maps
  u64 grey = 0;  // interior: a penumbra where the rays say there is no edge
  u64 band = 0;
  u64 band_grey = 0;
  u64 band_differ = 0;  // band: black or white against the rays' answer
};

void count_against(const CapturedFrame& traced, const CapturedFrame& mapped, SampleCounts& out) {
  const u32 w = k_sample_w;
  const u32 h = k_sample_h;
  auto cls = [&](u32 p) -> i32 {
    if (traced.ids[u64{p} * k_id_words] == k_no_id) return -1;
    const u8 v = traced.color[u64{p} * 4];
    return v == 0 ? 1 : (v == 255 ? 0 : -1);  // grey: facing away, no answer
  };
  for (u32 y = 0; y < h; ++y) {
    for (u32 x = 0; x < w; ++x) {
      const u32 p = y * w + x;
      const i32 c = cls(p);
      if (c < 0) continue;
      bool inside = true;
      for (i32 dy = -k_sample_edge; dy <= k_sample_edge && inside; ++dy) {
        for (i32 dx = -k_sample_edge; dx <= k_sample_edge && inside; ++dx) {
          const i32 nx = static_cast<i32>(x) + dx;
          const i32 ny = static_cast<i32>(y) + dy;
          if (nx < 0 || ny < 0 || nx >= static_cast<i32>(w) || ny >= static_cast<i32>(h)) {
            inside = false;
          } else if (cls(static_cast<u32>(ny) * w + static_cast<u32>(nx)) != c) {
            inside = false;
          }
        }
      }
      const u8 v = mapped.color[u64{p} * 4];
      const bool black = v == 0;
      const bool white = v == 255;
      if (!inside) {
        ++out.band;
        if (!black && !white) ++out.band_grey;
        if ((c == 1 && white) || (c == 0 && black)) ++out.band_differ;
        continue;
      }
      ++out.interior;
      out.shadowed += c == 1 ? 1u : 0u;
      if (c == 0 && black) ++out.acne;
      if (c == 1 && white) ++out.leak;
      if (!black && !white) ++out.grey;
    }
  }
}

struct MapRig {
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  bool open(const gfx::Device& device, const SceneData& data, ShadowMode shadows, f32 lod_scale,
            u32 cascades, std::string& error) {
    RenderSettings settings;
    settings.raster = RasterMode::Hardware;
    settings.shadows = shadows;
    settings.shadow_lod_scale = lod_scale;
    settings.shadow_cascades = cascades;
    // ENGINE_SHADOW_NORMAL_OFFSET=<texels> overrides k_shadow_normal_offset_texels.
    const std::string offset = test::detail::environment("ENGINE_SHADOW_NORMAL_OFFSET");
    if (!offset.empty()) settings.shadow_normal_offset = std::strtof(offset.c_str(), nullptr);
    // ENGINE_SHADOW_TEXEL_LOD=0 draws the cascades from the picture's cut alone, as before R103.
    settings.shadow_texel_lod = test::detail::environment("ENGINE_SHADOW_TEXEL_LOD") != "0";
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc desc;
    desc.width = k_sample_w;
    desc.height = k_sample_h;
    return renderer.create(device, scene, resolved, desc, &error);
  }
};

}  // namespace

TEST_CASE("renderer: cascaded shadow maps on the Khronos samples" * doctest::skip()) {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  if (!gpu.device.features().cluster_acceleration_structure || !gpu.device.features().ray_query) {
    MESSAGE("ray-traced shadows unavailable here: nothing to compare the maps against");
    return;
  }
  test::TempDir dir("shadow-maps");
  const std::string ground = dir.file("ground.glb");
  REQUIRE(write_ground_glb(ground));
  const char* k_samples[] = {"FlightHelmet/FlightHelmet.gltf",
                             "SciFiHelmet/SciFiHelmet.gltf",
                             "BoomBox/BoomBox.glb",
                             "Lantern/Lantern.glb",
                             "Corset/Corset.glb",
                             "Avocado/Avocado.glb",
                             "Suzanne/Suzanne.gltf"};
  constexpr u32 k_yaws = 8;
  constexpr u32 k_timed_frames = 240;
  const f32 k_lod_scales[3] = {1.0f, 2.0f, 4.0f};
  // ENGINE_SHADOW_SAMPLES=FlightHelmet,BoomBox measures only those; unset measures all.
  const std::string only = test::detail::environment("ENGINE_SHADOW_SAMPLES");
  const bool timing = test::detail::environment("ENGINE_SHADOW_NO_TIMING").empty();
  // ENGINE_SHADOW_PICTURE_LOD_ONLY=1 leaves out the coarser cuts (x2, x4).
  const u32 lod_runs =
      test::detail::environment("ENGINE_SHADOW_PICTURE_LOD_ONLY").empty() ? 3u : 1u;
  // ENGINE_SHADOW_CASCADES=1 draws every map with that many cascades instead of the default.
  const std::string cascades_env = test::detail::environment("ENGINE_SHADOW_CASCADES");
  const u32 cascades = cascades_env.empty()
                           ? k_default_shadow_cascades
                           : static_cast<u32>(std::strtoul(cascades_env.c_str(), nullptr, 10));
  // ENGINE_SHADOW_TEXEL_ERROR=<texels> sets `renderer.shadow.texel_error` for the run (R103).
  const std::string texel_error_env = test::detail::environment("ENGINE_SHADOW_TEXEL_ERROR");
  if (!texel_error_env.empty()) {
    REQUIRE(tunables::apply_overrides("renderer.shadow.texel_error=" + texel_error_env));
  }
  for (const char* relative : k_samples) {
    const std::string name = std::string(relative).substr(0, std::string(relative).find('/'));
    if (!only.empty() && only.find(name) == std::string::npos) continue;
    const std::string path =
        test::data_path(std::string(ENGINE_SOURCE_DIR "/content/samples/") + relative,
                        std::string("content/samples/") + relative);
    if (!test::path_exists(path)) {
      MESSAGE("skipped, not fetched (tools/fetch-samples.ps1): " << path);
      continue;
    }
    std::string error;
    SceneDesc probe_desc;
    probe_desc.meshes.push_back(path);
    probe_desc.ddc = dir.path() + "/ddc";
    SceneData probe;
    REQUIRE_MESSAGE(load_scene(probe_desc, probe, error), error);
    f32 lowest = 1e30f;
    for (const Vec3& v : probe.lod.mesh.vertices)
      lowest = std::min(lowest, v.y);
    SceneDesc desc = probe_desc;
    desc.meshes.push_back(ground);
    SceneInstance model;
    model.mesh = 0;
    desc.instances.push_back(model);
    SceneInstance floor;
    floor.mesh = 1;
    // A sample by the origin. (`relative` is the sample's path in this loop.)
    const Vec3 probe_center = engine::relative(probe.center, WorldPos::origin());
    floor.transform.position = Vec3{probe_center.x, lowest, probe_center.z};
    // Uniformly: the quad is flat, so its height scale changes nothing it draws, but a bounding
    // sphere scales by the largest axis, and a height scale of 1 under a 2 cm sample made the
    // scene's radius the unscaled quad's — and the ray-traced shadows' bias, a thousandth of
    // that radius, nine times what it is for every other sample. The shadow on the ground moved
    // by it, which is what the maps were first measured against.
    floor.transform.scale = Vec3{6.0f * probe.radius, 6.0f * probe.radius, 6.0f * probe.radius};
    desc.instances.push_back(floor);
    SceneData data;
    REQUIRE_MESSAGE(load_scene(desc, data, error), error);

    MapRig traced;
    REQUIRE_MESSAGE(traced.open(gpu.device, data, ShadowMode::RayTraced, 1.0f, cascades, error),
                    error);
    MapRig mapped[3];
    for (u32 k = 0; k < lod_runs; ++k) {
      REQUIRE_MESSAGE(
          mapped[k].open(gpu.device, data, ShadowMode::Cascaded, k_lod_scales[k], cascades, error),
          error);
    }
    const f32 pitches[2] = {k_orbit_pitch, 0.12f};
    SampleCounts counts[3];
    u64 shadow_pairs[3] = {};
    u64 picture_pairs = 0;
    // The view whose interior disagrees most at the picture's own LOD, kept for the pictures below.
    u64 worst_disagreement = 0;
    u32 worst_view = 0;
    CapturedFrame worst_rays;
    CapturedFrame worst_maps;
    for (u32 view = 0; view < 2 * k_yaws; ++view) {
      FrameDesc frame;
      frame.camera =
          orbit_camera_at(probe.center, probe.radius, 0.0f,
                          k_two_pi * static_cast<f32>(view % k_yaws) / static_cast<f32>(k_yaws),
                          pitches[view / k_yaws]);
      frame.view_mode = static_cast<u32>(gfx::ResolveMode::Shadow);
      CaptureChannels channels;
      channels.ids = true;
      CapturedFrame rays;
      REQUIRE_MESSAGE(traced.renderer.capture(frame, channels, rays, &error), error);
      picture_pairs += traced.renderer.stats().visible_pairs();
      for (u32 k = 0; k < lod_runs; ++k) {
        CapturedFrame maps;
        REQUIRE_MESSAGE(mapped[k].renderer.capture(frame, channels, maps, &error), error);
        const u64 before = counts[k].acne + counts[k].leak;
        count_against(rays, maps, counts[k]);
        shadow_pairs[k] += mapped[k].renderer.stats().shadow_pairs;
        if (k == 0 && counts[k].acne + counts[k].leak - before >= worst_disagreement) {
          worst_disagreement = counts[k].acne + counts[k].leak - before;
          worst_view = view;
          worst_rays = rays;
          worst_maps = maps;
        }
      }
    }
    // The worst view as three pictures a person can read, kept in the case's scratch directory:
    // the rays' shadow view, the maps', and where they disagree inside the band — red where the
    // maps are black and the rays white, blue the reverse, yellow grey in the maps — over the maps'
    // own picture dimmed.
    if (!worst_maps.color.empty()) {
      Vector<u8> diff(k_sample_w * k_sample_h * 4, 0);
      for (u32 p = 0; p < k_sample_w * k_sample_h; ++p) {
        const u8 r = worst_rays.color[u64{p} * 4];
        const u8 m = worst_maps.color[u64{p} * 4];
        u8 rgb[3] = {static_cast<u8>(m / 3), static_cast<u8>(m / 3), static_cast<u8>(m / 3)};
        if (r == 255 && m == 0) rgb[0] = 255, rgb[1] = 0, rgb[2] = 0;
        if (r == 0 && m == 255) rgb[0] = 0, rgb[1] = 64, rgb[2] = 255;
        if ((r == 0 || r == 255) && m != 0 && m != 255 && m != r) {
          rgb[0] = 255, rgb[1] = 220, rgb[2] = 0;
        }
        diff[p * 4 + 0] = rgb[0];
        diff[p * 4 + 1] = rgb[1];
        diff[p * 4 + 2] = rgb[2];
        diff[p * 4 + 3] = 255;
      }
      std::string sample(relative);
      sample = sample.substr(0, sample.find('/'));
      const auto write = [&](const char* suffix, const Vector<u8>& pixels) {
        (void)image::write_png(dir.file(sample + suffix), k_sample_w, k_sample_h, 4,
                               std::span<const u8>(pixels.data(), pixels.size()));
      };
      write("-rays.png", worst_rays.color);
      write("-maps.png", worst_maps.color);
      write("-diff.png", diff);
      dir.keep();
      MESSAGE(std::string(relative)
              << ": view " << worst_view << " disagrees most (" << worst_disagreement
              << " interior px); pictures in " << dir.path());
    }
    const ShadowCascades& fit = mapped[0].renderer.shadow_cascades();
    {
      // The last view's fit, cascade by cascade, in sample radii.
      std::string text;
      for (u32 c = 0; c < fit.count; ++c) {
        text += " [" + std::to_string(fit.splits[c] / probe.radius) + ", " +
                std::to_string(fit.splits[c + 1] / probe.radius) + "] r " +
                std::to_string(fit.radii[c] / probe.radius) + " texel " +
                std::to_string(fit.cascades[c].texel_world / probe.radius) +
                (fit.scene_bounds[c] ? " (scene)" : "");
      }
      MESSAGE(std::string(relative) << ": sample radius " << probe.radius << ", scene radius "
                                    << data.radius << "; last view's cascades:" << text);
    }
    for (u32 k = 0; k < lod_runs; ++k) {
      const SampleCounts& c = counts[k];
      MESSAGE(std::string(relative)
              << " on a ground, 16 views, cascades' LOD x" << k_lod_scales[k] << ": " << fit.count
              << " cascade(s), " << fit.cascades[0].texel_world / probe.radius * 1000.0f
              << " thousandths of a radius a texel; pairs in the maps " << shadow_pairs[k]
              << " (picture " << picture_pairs << "); interior " << c.interior << " px, "
              << c.shadowed << " shadowed by rays: acne " << c.acne << ", leak " << c.leak
              << ", grey " << c.grey << "; band " << c.band << " px (" << c.band_grey << " grey, "
              << c.band_differ << " against the rays)");
    }
    // At the picture's own cut the two implementations never give opposite answers inside the
    // band (0 on all seven samples on the RTX 5090, 2026-09-24). The coarser cuts are measured,
    // not asserted: they are the option that was not taken.
    CHECK(counts[0].acne == 0);
    CHECK(counts[0].leak == 0);

    // The cost at engine-view's own camera: off, rays, and the maps at each LOD scale. Each is
    // warmed up and timed twice, forwards and backwards, because a GPU that is still raising its
    // clocks makes whichever runs first look slowest. ENGINE_SHADOW_NO_TIMING leaves it out.
    if (!timing) continue;
    FrameDesc timed;
    timed.camera = orbit_camera_at(probe.center, probe.radius, 0.0f, 0.0f, k_orbit_pitch);
    MapRig off;
    REQUIRE_MESSAGE(off.open(gpu.device, data, ShadowMode::Off, 1.0f, cascades, error), error);
    SceneRenderer* rigs[5] = {&off.renderer, &traced.renderer, &mapped[0].renderer,
                              &mapped[1].renderer, &mapped[2].renderer};
    const char* names[5] = {"off", "rt", "csm", "csm lod x2", "csm lod x4"};
    const u32 rig_count = 2 + lod_runs;
    for (u32 pass = 0; pass < 2 * rig_count; ++pass) {
      const u32 k = pass < rig_count ? pass : 2 * rig_count - 1 - pass;
      SceneRenderer& r = *rigs[k];
      for (u32 f = 0; f < 60; ++f) {
        timed.frame_index = f;
        REQUIRE_MESSAGE(r.render_offscreen(timed, &error), error);
      }
      r.reset_stats();
      for (u32 f = 0; f < k_timed_frames; ++f) {
        timed.frame_index = f;
        REQUIRE_MESSAGE(r.render_offscreen(timed, &error), error);
      }
      const Stats& s = r.stats();
      MESSAGE(std::string(relative)
              << " " << std::string(names[k]) << ": ms cull " << s.cull_ms() << " hw " << s.hw_ms()
              << " hiz " << s.hiz_ms() << " rt " << s.rt_ms() << " shadow " << s.shadow_ms()
              << " (cull " << s.shadow_cull_ms() << ") resolve " << s.resolve_ms() << " total "
              << s.total_ms() << "; pairs " << s.visible_pairs() << " + " << s.shadow_pairs
              << " in the maps");
    }
  }
}
