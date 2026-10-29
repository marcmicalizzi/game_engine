// The caster policy's untraced species under a traced picture (docs/subsystems/renderer.md, "The
// caster policy"; roadmap R110). `traced = false` — the sward's — was implemented as "not in the
// ray tracing chain", which under `--raster rt` meant "not drawn": the forest proof of concept's
// v20 grass was simply absent from the ray path's picture. The policy's intent is that no light's
// ray meets the grass, and that stands; the picture is the other contract. So:
//
// - a frame whose picture is rasterized, with traced shadows, keeps the instance out of the chain
//   exactly as before: nothing built for it, nothing wanted, no shadow;
// - the ray path builds its drawn entries, so its primary rays draw it where the policy-off run
//   draws it, word for word, rigid and deformed — and still no shadow ray meets it, which is the
//   top-level mask's doing (gfx.md, "The ray mask");
// - the reference path tracer's camera ray sees it and its light rays do not.
//
// The scene is the shadow fixture's floating cube over the ground (shadow_fixture.h), its mesh
// named the sward's species so the policy applies to it as it does to grass, against the same run
// with `renderer.flora.caster_policy=false`. A cube rather than the card: the card faces away from
// this camera, so its clusters are all casters and the picture has none of it to draw.
#include "shadow_fixture.h"

#include <domain/gfx/device.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace engine;
using namespace engine::renderer;
using namespace engine::renderer::shadow_fixture;

namespace {

constexpr u32 k_cube = 1;  // the caster's instance

// `renderer.flora.caster_policy` for a scope: read when the scene is made, so the other cases in
// the binary see the default again after it.
struct ScopedPolicy {
  tunables::Tunable* tunable = nullptr;
  std::string previous;
  explicit ScopedPolicy(bool on) {
    tunable = tunables::find("renderer.flora.caster_policy");
    REQUIRE(tunable != nullptr);
    tunable->append_value(previous);
    REQUIRE(tunable->set_from_text(on ? "true" : "false"));
  }
  ~ScopedPolicy() {
    if (tunable != nullptr) (void)tunable->set_from_text(previous);
  }
};

struct Picture {
  CapturedFrame frame;  // the shadow view: black where the sun's ray is blocked; and the ids
  u32 wanted = 0;       // RtStats::wanted + references_wanted: every structure it asked for
  u32 built_here = 0;   // RtStats::wanted alone: the frame's own records, the cache's aside
  u32 untraced = 0;     // FloraStats::untraced
  bool available = true;
  std::string why;
  ReferenceFrame reference;  // direct light only, when asked
  bool referenced = false;
};

struct Options {
  RasterMode raster = RasterMode::Hardware;
  bool policy = true;
  bool deform = false;
  bool reference = false;
};

Picture draw(const gfx::Device& device, const Options& options) {
  Picture out;
  const ScopedPolicy policy(options.policy);
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(make_scene(Caster::cube, data, error), error);
  data.sources[k_cube].species = "sward";  // the field species' policy: never met by a light's ray
  RenderSettings settings;
  settings.raster = options.raster;
  settings.shadows = ShadowMode::RayTraced;
  settings.lights = false;  // the sun alone
  if (options.deform) {
    settings.deform = true;
    settings.deform_kind = gfx::k_deform_wave;
    settings.deform_amplitude = 0.02f;
  }
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  const RenderAvailability availability = check_availability(resolved, device.features());
  if (availability != RenderAvailability::Ok ||
      (options.reference && !reference_available(resolved, device, &error))) {
    out.available = false;
    out.why =
        availability != RenderAvailability::Ok ? unavailable_reason(availability, device) : error;
    return out;
  }
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  out.untraced = scene.flora_stats().untraced;
  SceneRenderer renderer;
  SceneRenderer::Desc desc;
  desc.width = k_width;
  desc.height = k_height;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, desc, &error), error);
  const Mat4 rotation = fixture_frame();
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), to_world(rotation, k_eye_local));
  frame.camera.target = absolute(WorldPos::origin(), to_world(rotation, k_target_local));
  frame.view_mode = static_cast<u32>(gfx::ResolveMode::Shadow);
  frame.frame_index = 7;  // the wave's phase, when deformed
  CaptureChannels channels;
  channels.color = true;
  channels.ids = true;
  // Twice: the chain's counts are read back a frame late.
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.frame, &error), error);
  REQUIRE_MESSAGE(renderer.capture(frame, channels, out.frame, &error), error);
  // The frame's own records and, with the resident cluster cache, the references to its cached
  // structures: together every structure the frame's top level names.
  out.wanted = renderer.stats().rt.wanted + renderer.stats().rt.references_wanted;
  out.built_here = renderer.stats().rt.wanted;
  if (options.reference) {
    ReferenceRenderer reference;
    REQUIRE_MESSAGE(reference.create(device, scene, renderer, {}, &error), error);
    ReferenceSettings rs;
    rs.spp = 1;
    rs.batch = 1;
    rs.max_bounces = 0;  // direct light alone: the sun's shadow ray is the one light query
    rs.pixel_center = true;
    rs.frame_index = frame.frame_index;
    REQUIRE_MESSAGE(reference.render(frame.camera, rs, out.reference, &error), error);
    out.referenced = true;
    reference.destroy();
  }
  renderer.destroy();
  scene.destroy();
  return out;
}

u32 instance_at(const Picture& p, u32 pixel) { return p.frame.ids[u64{pixel} * k_id_words]; }

u32 pixels_of(const Picture& p, u32 instance) {
  u32 n = 0;
  for (u32 px = 0; px < k_width * k_height; ++px)
    n += instance_at(p, px) == instance ? 1u : 0u;
  return n;
}

// The fixture's `is_ground` and `id_differences` take a Shot; the same on a Picture.
bool is_ground_px(const Picture& p, u32 pixel) { return instance_at(p, pixel) == k_ground; }

u32 id_differences_of(const Picture& a, const Picture& b) {
  u32 out = 0;
  for (u32 w = 0; w < a.frame.ids.size() && w < b.frame.ids.size(); ++w)
    out += a.frame.ids[w] != b.frame.ids[w] ? 1u : 0u;
  return out + (a.frame.ids.size() != b.frame.ids.size() ? 1u : 0u);
}

u32 shadowed_ground(const Picture& p) {
  u32 n = 0;
  for (u32 px = 0; px < k_width * k_height; ++px)
    n += is_ground_px(p, px) && p.frame.color[u64{px} * 4] == 0 ? 1u : 0u;
  return n;
}

}  // namespace

TEST_CASE("renderer: an untraced instance is drawn by the ray path and met by no light's ray") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const Picture hw = draw(gpu.device, Options{RasterMode::Hardware, true, false, false});
  if (!hw.available) {
    MESSAGE("ray-traced shadows unavailable here: " << hw.why);
    return;
  }
  const Picture hw_all = draw(gpu.device, Options{RasterMode::Hardware, false, false, false});
  CHECK(hw.untraced == 1);
  CHECK(hw_all.untraced == 0);
  // The witness: with the policy off the cube throws a real shadow onto the ground in view.
  const u32 cast = shadowed_ground(hw_all);
  CHECK(cast > 200);
  // A rasterized picture with traced shadows: nothing changes. The cube is drawn (by the
  // rasterizer), casts nothing, and the chain wants nothing for it — fewer structures than the
  // policy-off frame, which wants the cube's drawn clusters and its casters.
  CHECK(pixels_of(hw, k_cube) == pixels_of(hw_all, k_cube));
  CHECK(shadowed_ground(hw) == 0);
  CHECK(hw.wanted < hw_all.wanted);

  const Picture rt = draw(gpu.device, Options{RasterMode::RayTrace, true, false, false});
  if (!rt.available) {
    MESSAGE("rt path unavailable here: " << rt.why);
    return;
  }
  const Picture rt_all = draw(gpu.device, Options{RasterMode::RayTrace, false, false, false});
  u32 culled_clusters = 0;
  u32 cube_clusters = 0;
  {
    SceneData data;
    std::string error;
    REQUIRE_MESSAGE(make_scene(Caster::cube, data, error), error);
    culled_clusters = cone_culled(data, cube_clusters);
  }
  const u32 id_diff = id_differences_of(rt, rt_all);
  MESSAGE("cube " << pixels_of(rt, k_cube) << " px on the ray path with the policy, "
                  << pixels_of(rt_all, k_cube) << " without it, " << pixels_of(hw, k_cube)
                  << " rasterized; " << id_diff << " id words differ; shadowed ground "
                  << shadowed_ground(rt) << " / " << shadowed_ground(rt_all) << " / "
                  << shadowed_ground(hw) << " / " << cast << "; structures wanted hw " << hw.wanted
                  << " / " << hw_all.wanted << ", rt " << rt.wanted << " / " << rt_all.wanted
                  << " (built by the frame itself, rt: " << rt.built_here << " / "
                  << rt_all.built_here << "); the cube's clusters " << cube_clusters << ", "
                  << culled_clusters << " facing away");
  // The primary rays draw the cube where the policy-off run draws it, word for word: before R110
  // every one of its pixels held the ground behind it.
  CHECK(pixels_of(rt, k_cube) > 1000);
  CHECK(id_diff == 0);
  // And no shadow ray meets it: the ground is lit where the policy-off run's is shadowed.
  CHECK(shadowed_ground(rt_all) > 200);
  CHECK(shadowed_ground(rt) == 0);
  // The chain wants the cube's drawn clusters on the ray path and none of its casters: the
  // facing clusters above the hw frame's, the cube's back faces fewer than the policy-off frame's.
  CHECK(rt.wanted - hw.wanted == cube_clusters - culled_clusters);
  CHECK(rt_all.wanted - rt.wanted == culled_clusters);

  // Deformed: every instance read out of the pool, the cube's drawn entries built from their
  // blocks, and the resolve reading the same blocks through the pair table.
  const Picture rt_wave = draw(gpu.device, Options{RasterMode::RayTrace, true, true, false});
  const Picture rt_wave_all = draw(gpu.device, Options{RasterMode::RayTrace, false, true, false});
  MESSAGE("deformed: " << id_differences_of(rt_wave, rt_wave_all) << " id words differ, cube "
                       << pixels_of(rt_wave, k_cube) << " px, shadowed ground "
                       << shadowed_ground(rt_wave) << " / " << shadowed_ground(rt_wave_all));
  CHECK(pixels_of(rt_wave, k_cube) > 1000);
  CHECK(id_differences_of(rt_wave, rt_wave_all) == 0);
  CHECK(shadowed_ground(rt_wave) == 0);
  CHECK(shadowed_ground(rt_wave_all) > 200);
}

TEST_CASE(
    "renderer: the reference path tracer sees an untraced instance and is not shadowed by it") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const Picture on = draw(gpu.device, Options{RasterMode::Hardware, true, false, true});
  if (!on.available || !on.referenced) {
    MESSAGE("no reference here: " << on.why);
    return;
  }
  const Picture off = draw(gpu.device, Options{RasterMode::Hardware, false, false, true});
  REQUIRE(off.referenced);
  REQUIRE(on.reference.width == k_width);
  // The region the policy-off cube shadows (the resolve's rays say where), and the ground the sun
  // reaches in both.
  f64 shade_on = 0.0;
  f64 shade_off = 0.0;
  f64 lit_on = 0.0;
  u32 shade = 0;
  u32 lit = 0;
  u32 cube = 0;
  u32 cube_differ = 0;
  for (u32 px = 0; px < k_width * k_height; ++px) {
    const f32* a = &on.reference.hdr[u64{px} * 4];
    const f32* b = &off.reference.hdr[u64{px} * 4];
    if (instance_at(on, px) == k_cube && instance_at(off, px) == k_cube) {
      // The camera ray meets the cube in both: the same surface, lit the same.
      ++cube;
      const f32 scale = std::max(std::fabs(b[1]), 1e-4f);
      cube_differ += std::fabs(a[1] - b[1]) > 1e-3f * scale ? 1u : 0u;
      continue;
    }
    if (!is_ground_px(on, px) || !is_ground_px(off, px)) continue;
    if (off.frame.color[u64{px} * 4] == 0) {
      ++shade;
      shade_on += static_cast<f64>(a[1]);
      shade_off += static_cast<f64>(b[1]);
    } else {
      ++lit;
      lit_on += static_cast<f64>(a[1]);
    }
  }
  REQUIRE(shade > 200);
  REQUIRE(lit > 200);
  REQUIRE(cube > 1000);
  shade_on /= shade;
  shade_off /= shade;
  lit_on /= lit;
  MESSAGE("reference, direct light: ground the cube shadows "
          << shade_on << " with the policy, " << shade_off << " without; lit " << lit_on
          << "; cube " << cube << " px, " << cube_differ << " differ");
  // The policy-off reference shadows the ground (direct light only: black), the policy's does not:
  // there the shadowed region is as bright as the rest of the lit ground.
  CHECK(shade_off < 0.05 * lit_on);
  CHECK(shade_on > 0.8 * lit_on);
  // Its camera ray sees the cube (the frame built its drawn entries, `rt_complete`): every cube
  // pixel the same as the policy-off reference's, where before R110 it held the ground behind.
  CHECK(cube_differ <= cube / 100);
}
