// The far field at rest (docs/subsystems/renderer.md, "The far field at rest"; roadmap R152). Past
// `renderer.flora.field_rest_m` a field's blades take no wind, an instance wholly past it takes no
// pool block, and in the ray tracing chain it references its mesh's resident structures instead of
// having the frame build its own. These cases hold the rule's arithmetic on the CPU (the mirror
// the shaders follow operation for operation) and, where the flora capability is built, the rule
// on a row of sward patches from short of the fade band to well past the distance (6.4 to 20.6 m
// from the eye, the rule at 15 m over a band of 3 m): a patch short
// of the band is drawn exactly as with the rule off, one past the distance exactly as in a calm,
// fewer entries take a block, and on the ray path the frame builds fewer structures, references
// the rest, and draws the bytes the per-frame build of the same poses draws.

#include "shadow_fixture.h"

#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/flora_motion.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#if ENGINE_RENDERER_TESTS_FLORA
#include <foundation/tunables/tunables.h>
#endif

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;
using namespace engine::renderer::shadow_fixture;

namespace {

// A frame's block with the rule at 10 m over a 2.5 m band, two field records from entry 4, roots
// within 0.7 m of an instance's origin.
gfx::FloraFieldFrame rule() {
  gfx::FloraFieldFrame f;
  f.rest = Vec4{10.0f, 1.0f / 2.5f, 0.7f, 0.0f};
  f.field_first = 4;
  f.field_count = 2;
  return f;
}

gfx::InstanceDesc field_instance() {
  gfx::InstanceDesc i{};
  i.scale_max = 1.0f;
  i.deform = 5;
  return i;
}

}  // namespace

TEST_CASE("renderer: the far field's fade is whole short of its band and nothing from its margin") {
  const gfx::FloraFieldFrame f = rule();
  // The band ends the margin short of the distance and is 2.5 m wide: [7.25, 9.75).
  CHECK(gfx::field_fade(f, 0.0f) == 1.0f);
  CHECK(gfx::field_fade(f, 7.25f) == 1.0f);
  CHECK(gfx::field_fade(f, 8.5f) == doctest::Approx(0.5f));
  CHECK(gfx::field_fade(f, 10.0f - gfx::k_field_rest_margin_m) == 0.0f);
  CHECK(gfx::field_fade(f, 10.0f) == 0.0f);
  CHECK(gfx::field_fade(f, 1000.0f) == 0.0f);
  // It never rises with distance and never steps: a centimetre moves it a centimetre of the band.
  f32 last = 1.0f;
  for (f32 d = 0.0f; d < 12.0f; d += 0.01f) {
    const f32 share = gfx::field_fade(f, d);
    CHECK(share <= last);
    CHECK(last - share <= 0.01f / 2.5f + 1.0e-5f);
    last = share;
  }
  // No rule: every blade sways whole.
  gfx::FloraFieldFrame off = f;
  off.rest = Vec4{};
  CHECK(gfx::field_fade(off, 5000.0f) == 1.0f);
}

TEST_CASE("renderer: a field instance rests when its every root is past the distance") {
  const gfx::FloraFieldFrame f = rule();
  const gfx::InstanceDesc patch = field_instance();
  // Its roots lie within 0.7 m of its origin, so it rests from 10.7 m.
  CHECK(gfx::field_rests(patch, Vec3{0.0f, 0.0f, -10.71f}, f));
  CHECK_FALSE(gfx::field_rests(patch, Vec3{0.0f, 0.0f, -10.69f}, f));
  CHECK(gfx::field_rests(patch, Vec3{6.2f, -6.2f, 6.2f}, f));  // 10.74 m, off every axis
  // A larger instance's roots reach farther.
  gfx::InstanceDesc scaled = patch;
  scaled.scale_max = 2.0f;
  CHECK_FALSE(gfx::field_rests(scaled, Vec3{0.0f, 0.0f, -11.0f}, f));
  CHECK(gfx::field_rests(scaled, Vec3{0.0f, 0.0f, -11.41f}, f));
  // Only a field's record: a tree's, a rigid instance, and the entries either side of the run.
  gfx::InstanceDesc other = patch;
  for (const u32 deform : {gfx::k_invalid_deform, 0u, 3u, 6u, 7u}) {
    other.deform = deform;
    CHECK_FALSE(gfx::field_rests(other, Vec3{0.0f, 0.0f, -50.0f}, f));
  }
  other.deform = 4;
  CHECK(gfx::field_rests(other, Vec3{0.0f, 0.0f, -50.0f}, f));
  // A hand-off's near form never (past its band it draws nothing, not its rest pose); its far form
  // only past its band's end, where it keeps every part.
  gfx::InstanceDesc near_form = patch;
  gfx::set_instance_handoff(near_form, gfx::k_instance_handoff_near,
                            gfx::pack_handoff_band(21.0f, 29.0f), 0);
  CHECK_FALSE(gfx::field_rests(near_form, Vec3{0.0f, 0.0f, -50.0f}, f));
  gfx::InstanceDesc far_form = patch;
  gfx::set_instance_handoff(far_form, gfx::k_instance_handoff_far,
                            gfx::pack_handoff_band(11.0f, 14.0f), 0);
  CHECK_FALSE(gfx::field_rests(far_form, Vec3{0.0f, 0.0f, -12.0f}, f));
  CHECK_FALSE(gfx::field_rests(far_form, Vec3{0.0f, 0.0f, -14.6f}, f));
  CHECK(gfx::field_rests(far_form, Vec3{0.0f, 0.0f, -14.8f}, f));
  // Never where the walker can reach a blade of it: its feet from the eye, and its reach.
  gfx::FloraFieldFrame walked = f;
  walked.walker = Vec4{0.0f, 0.0f, -20.0f, 0.3f};
  walked.rest.w = 0.3f + 0.05f + 0.4f;
  CHECK_FALSE(gfx::field_rests(patch, Vec3{0.0f, 0.0f, -20.0f}, walked));
  CHECK_FALSE(gfx::field_rests(patch, Vec3{1.4f, 0.0f, -20.0f}, walked));
  CHECK(gfx::field_rests(patch, Vec3{1.5f, 0.0f, -20.0f}, walked));
  // No rule, no rest.
  gfx::FloraFieldFrame off = f;
  off.rest = Vec4{};
  CHECK_FALSE(gfx::field_rests(patch, Vec3{0.0f, 0.0f, -5000.0f}, off));
}

#if ENGINE_RENDERER_TESTS_FLORA

namespace {

// The sward patch the hand-off's cases cook (handoff_tests.cpp).
constexpr const char* k_sward =
    R"({"species":"sward","sward":{"species":"perennial_ryegrass","blades_m2":192,"density":0.5,)"
    R"("tillers_per_clump":4,"leaves_per_tiller":3,"clumpiness":0.6,"seed":8801,)"
    R"("triangle_budget":2400,"card_px":32}})";

constexpr u32 k_w = 1280;
constexpr u32 k_h = 960;
constexpr u32 k_patches = 9;
constexpr f32 k_step = 2.0f;  // metres between patches, away from the eye
// The eye stands 5 m up and 4 m back from the first patch and looks down the row at 32 degrees,
// so every patch is in view, none hides another's blades, and the row runs from 6.4 m to 20.6 m
// from it.
const Vec3 k_eye{0.0f, 5.0f, 4.0f};
const Vec3 k_target{0.0f, 0.0f, -4.0f};
constexpr f32 k_rest = 15.0f;
constexpr f32 k_fade = 3.0f;
constexpr const char* k_rest_m = "15";
constexpr const char* k_fade_m = "3";

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

struct ScopedTunable {
  tunables::Tunable* tunable = nullptr;
  std::string previous;
  ScopedTunable(const char* name, const char* value) {
    tunable = tunables::find(name);
    REQUIRE_MESSAGE(tunable != nullptr, name);
    tunable->append_value(previous);
    REQUIRE(tunable->set_from_text(value));
  }
  ~ScopedTunable() {
    if (tunable != nullptr) (void)tunable->set_from_text(previous);
  }
};

struct RowShot {
  CapturedFrame frame;
  Stats stats;
  f32 root_reach = 0.0f;
  bool available = true;
  std::string why;
};

// The row drawn by `raster` in a wind of `strength`, two seconds into the sway.
bool draw_row(const gfx::Device& device, const test::TempDir& tmp, RasterMode raster,
              f32 strength, RowShot& out, std::string& error) {
  SceneDesc desc;
  desc.meshes.push_back(tmp.file("sward.flora.json"));
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  for (u32 i = 0; i < k_patches; ++i) {
    SceneInstance p;
    p.mesh = 0;
    p.transform.position = Vec3{0.0f, 0.0f, -k_step * static_cast<f32>(i)};
    desc.instances.push_back(p);
  }
  SceneData data;
  if (!load_scene(desc, data, error)) return false;
  RenderSettings settings;
  settings.raster = raster;
  settings.shadows = ShadowMode::Off;
  settings.lights = false;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  const RenderAvailability availability = check_availability(resolved, device.features());
  if (availability != RenderAvailability::Ok) {
    out.available = false;
    out.why = unavailable_reason(availability, device);
    return true;
  }
  if (!resolved.flora_motion) {
    out.available = false;
    out.why = "the field is not posed here";
    return true;
  }
  GpuScene scene;
  if (!scene.create(device, data, resolved, &error)) return false;
  out.root_reach = scene.field_rest_scene().root_reach;
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = k_w;
  rd.height = k_h;
  if (!renderer.create(device, scene, resolved, rd, &error)) return false;
  FrameDesc frame;
  frame.camera.position = absolute(WorldPos::origin(), k_eye);
  frame.camera.target = absolute(WorldPos::origin(), k_target);
  frame.world_time_s = 2.0;
  frame.flora.wind_set = true;
  frame.flora.wind_to = Vec2{1.0f, 0.0f};
  frame.flora.wind_strength = strength;
  CaptureChannels channels;
  channels.ids = true;
  channels.depth = true;
  // Three frames: the statistics are a frame behind, and the cache's first frame builds.
  bool ok = renderer.capture(frame, channels, out.frame, &error) &&
            renderer.capture(frame, channels, out.frame, &error) &&
            renderer.capture(frame, channels, out.frame, &error);
  out.stats = renderer.stats();
  renderer.destroy();
  scene.destroy();
  return ok;
}

f32 patch_distance(u32 i) {
  const Vec3 at{0.0f, 0.0f, -k_step * static_cast<f32>(i)};
  return length(at - k_eye);
}

// How instance `i` compares in two captures: the pixels either shows it at, and those of them
// where the two differ in id or depth.
struct PatchDiff {
  u32 pixels = 0;
  u32 differ = 0;
};
PatchDiff compare(const CapturedFrame& a, const CapturedFrame& b, u32 instance) {
  PatchDiff c;
  for (u32 px = 0; px < k_w * k_h; ++px) {
    const u32 ia = a.ids[u64{px} * k_id_words];
    const u32 ib = b.ids[u64{px} * k_id_words];
    const bool covered_a = a.depth[px] != 0.0f && ia == instance;
    const bool covered_b = b.depth[px] != 0.0f && ib == instance;
    if (!covered_a && !covered_b) continue;
    ++c.pixels;
    const bool same =
        covered_a && covered_b &&
        std::memcmp(&a.ids[u64{px} * k_id_words], &b.ids[u64{px} * k_id_words],
                    k_id_words * sizeof(u32)) == 0 &&
        std::memcmp(&a.depth[px], &b.depth[px], sizeof(f32)) == 0;
    c.differ += same ? 0u : 1u;
  }
  return c;
}

u32 differing(const CapturedFrame& a, const CapturedFrame& b) {
  u32 n = 0;
  for (u32 px = 0; px < k_w * k_h; ++px) {
    const bool same = std::memcmp(&a.ids[u64{px} * k_id_words], &b.ids[u64{px} * k_id_words],
                                  k_id_words * sizeof(u32)) == 0 &&
                      std::memcmp(&a.depth[px], &b.depth[px], sizeof(f32)) == 0 &&
                      std::memcmp(&a.color[u64{px} * 4], &b.color[u64{px} * 4], 4) == 0;
    n += same ? 0u : 1u;
  }
  return n;
}

}  // namespace

TEST_CASE("renderer: past its rest distance a field stands as in a calm, short of the band it "
          "sways as before") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("renderer unavailable here: " << gpu.why);
    return;
  }
  const test::TempDir tmp("renderer_field_rest");
  REQUIRE(tmp.ok());
  REQUIRE(write_text(tmp.file("sward.flora.json"), k_sward));
  const f32 rest_m = k_rest;
  const f32 band_from = rest_m - gfx::k_field_rest_margin_m - k_fade;
  for (const RasterMode raster : {RasterMode::Hardware, RasterMode::RayTrace}) {
    CAPTURE(static_cast<u32>(raster));
    std::string error;
    RowShot off;   // the wind, no rule
    RowShot calm;  // no wind
    RowShot on;    // the wind, the rule
    {
      const ScopedTunable none("renderer.flora.field_rest_m", "0");
      REQUIRE_MESSAGE(draw_row(gpu.device, tmp, raster, 1.0f, off, error), error);
      if (!off.available) {
        MESSAGE("path " << static_cast<u32>(raster) << " unavailable here: " << off.why);
        continue;
      }
      REQUIRE_MESSAGE(draw_row(gpu.device, tmp, raster, 0.0f, calm, error), error);
    }
    {
      const ScopedTunable rest("renderer.flora.field_rest_m", k_rest_m);
      const ScopedTunable fade("renderer.flora.field_rest_fade_m", k_fade_m);
      REQUIRE_MESSAGE(draw_row(gpu.device, tmp, raster, 1.0f, on, error), error);
    }
    REQUIRE(on.root_reach > 0.0f);
    u32 near_patches = 0;
    u32 rest_patches = 0;
    u32 moved_off = 0;
    for (u32 i = 0; i < k_patches; ++i) {
      const f32 d = patch_distance(i);
      const PatchDiff swayed = compare(off.frame, calm.frame, i);
      const PatchDiff to_off = compare(on.frame, off.frame, i);
      const PatchDiff to_calm = compare(on.frame, calm.frame, i);
      MESSAGE("patch " << i << " at " << d << " m: " << swayed.pixels << " px, the wind moves "
                       << swayed.differ << "; with the rule " << to_off.differ
                       << " differ from the wind's, " << to_calm.differ << " from the calm's");
      REQUIRE(swayed.pixels > 50);
      if (d + on.root_reach < band_from - 0.05f) {
        // Wholly short of the band: the rule has not touched it.
        ++near_patches;
        CHECK(to_off.differ == 0);
        CHECK(swayed.differ > 0);  // and the wind does move it, so the case can tell
      } else if (d - on.root_reach >= rest_m + 0.05f) {
        // Wholly past the distance: its rest pose, to the id and the depth's bits.
        ++rest_patches;
        CHECK(to_calm.differ == 0);
        moved_off += swayed.differ;
      }
    }
    CHECK(near_patches >= 2);
    CHECK(rest_patches >= 2);
    CHECK(moved_off > 0);  // the wind moved what the rule put at rest
    // Fewer entries take a block, and the pool pass writes fewer vertices.
    MESSAGE("path " << static_cast<u32>(raster) << ": pool entries " << off.stats.deform_entries
                    << " -> " << on.stats.deform_entries << ", vertices "
                    << off.stats.deform_vertices << " -> " << on.stats.deform_vertices
                    << "; structures built " << off.stats.rt.built << " -> " << on.stats.rt.built
                    << ", references " << off.stats.rt.references << " -> "
                    << on.stats.rt.references);
    CHECK(on.stats.deform_entries < off.stats.deform_entries);
    CHECK(on.stats.deform_vertices < off.stats.deform_vertices);
    CHECK(on.stats.deform_overflow_entries == 0);
    if (raster != RasterMode::RayTrace) continue;
    // The chain: what rests is referenced, not built, and nothing is lost between the two.
    CHECK(off.stats.rt.references == 0);
    CHECK(on.stats.rt.references > 0);
    CHECK(on.stats.rt.built < off.stats.rt.built);
    CHECK(on.stats.rt.built + on.stats.rt.references == off.stats.rt.built);
    CHECK(on.stats.rt.built == on.stats.rt.wanted);
    // And the picture is the per-frame build's of the same poses, byte for byte.
    RowShot built;
    {
      const ScopedTunable rest("renderer.flora.field_rest_m", k_rest_m);
      const ScopedTunable fade("renderer.flora.field_rest_fade_m", k_fade_m);
      const ScopedTunable no_cache("renderer.rt.cache", "false");
      REQUIRE_MESSAGE(draw_row(gpu.device, tmp, raster, 1.0f, built, error), error);
    }
    CHECK(built.stats.rt.references == 0);
    CHECK(built.stats.rt.built == off.stats.rt.built);
    CHECK(differing(on.frame, built.frame) == 0);
  }
}

#endif  // ENGINE_RENDERER_TESTS_FLORA
