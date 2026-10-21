// A tree's impostor chain drawn (docs/subsystems/flora.md, "The impostor chain"; renderer.md,
// "Impostors are masked clusters"; R89): from a plane's baked view, the whole-tree star drawn by
// the masked variants and lit through its baked normals matches the full geometry's picture within
// a stated error — the proof that the bake, the container, the texture step and the draw agree.

#include <domain/gfx/device.h>
#include <systems/renderer/camera_path.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cmath>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

#if ENGINE_RENDERER_TESTS_FLORA

namespace {

struct Gpu {
  gfx::Device device;
  std::string why;
  bool ok = false;
  Gpu() {
    std::string error;
    if (!device.create(gfx::DeviceOptions{}, &error)) {
      why = "no Vulkan device: " + error;
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    ok = true;
  }
};

// The impostor tests' 8 m oak (domain/flora/tests/impostor_tests.cpp).
constexpr const char* k_small_oak =
    R"({"species":"broadleaf","broadleaf":{"height_m":8,"triangle_budget":30000}})";

}  // namespace

TEST_CASE("impostor view: the tree's star from its baked view matches the full tree") {
  const test::TempDir tmp("renderer_impostor_view");
  REQUIRE(tmp.ok());
  const std::string oak = tmp.file("oak.flora.json");
  {
    std::ofstream f(oak, std::ios::binary);
    f << k_small_oak;
  }
  SceneDesc desc;
  desc.meshes.push_back(oak);
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  REQUIRE(data.sources.size() == 1);
  REQUIRE(data.sources[0].data.materials.size() >= 4);  // bark, leaf, a card level, the star

  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  RenderSettings settings;
  ResolvedSettings resolved;
  resolve_settings(settings, gpu.device.features(), &data, resolved);
  REQUIRE(check_availability(resolved, gpu.device.features()) == RenderAvailability::Ok);
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(gpu.device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = 480;
  rd.height = 480;
  REQUIRE_MESSAGE(renderer.create(gpu.device, scene, resolved, rd, &error), error);

  // The star's third plane faces +z through the trunk: look at the tree from far along +z, where a
  // perspective view is nearly the bake's orthographic one, the tree about 200 pixels tall.
  CameraPath path;
  CameraPathKey key;
  key.position = WorldPos{0.0, 4.0, 120.0};
  key.target = WorldPos{0.0, 4.0, 0.0};
  key.fov_y = 0.08f;
  path.keys.push_back(key);
  key.time = 1.0;
  path.keys.push_back(key);
  FrameDesc frame;
  frame.camera = sample_camera_path(path, 0.0);
  CaptureChannels channels;
  channels.ids = true;
  CapturedFrame full;
  CapturedFrame star;
  frame.lod_px = 0.0f;  // the finest clusters
  REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
  REQUIRE_MESSAGE(renderer.capture(frame, channels, full, &error), error);
  frame.lod_px = 1.0e6f;  // the chain's root
  REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
  REQUIRE_MESSAGE(renderer.capture(frame, channels, star, &error), error);
  if (!test::detail::environment("ENGINE_FLORA_KEEP_CAPTURE").empty()) {
    CaptureFiles files;
    write_capture(tmp.path(), "full", full, files, error);
    write_capture(tmp.path(), "star", star, files, error);
    tmp.keep();
  }

  // Coverage: a pixel is the tree's when an instance drew it. Colour: over the pixels both drew.
  const u32 instances = data.instances.size();
  u32 both = 0, either = 0, full_only = 0, star_only = 0;
  f64 colour_error = 0.0;
  for (u32 p = 0; p < full.width * full.height; ++p) {
    const bool a = full.ids[p * k_id_words] < instances;
    const bool b = star.ids[p * k_id_words] < instances;
    either += (a || b) ? 1u : 0u;
    full_only += (a && !b) ? 1u : 0u;
    star_only += (b && !a) ? 1u : 0u;
    if (!(a && b)) continue;
    ++both;
    for (u32 k = 0; k < 3; ++k)
      colour_error += std::abs(static_cast<f64>(full.color[p * 4 + k]) -
                               static_cast<f64>(star.color[p * 4 + k]));
  }
  REQUIRE(either > 2000u);
  const f64 overlap = static_cast<f64>(both) / static_cast<f64>(either);
  const f64 mean = colour_error / (3.0 * static_cast<f64>(both > 0 ? both : 1u));
  MESSAGE("star against full tree from the baked view: "
          << either << " tree pixels, overlap " << overlap << " (" << full_only
          << " only the tree's, " << star_only << " only the star's), mean colour error " << mean
          << " of 255");
  // The stated bounds (flora.md, "The impostor chain"): the silhouettes share most of their
  // pixels — the oblique planes and the level one add a fringe the tree does not have — and where
  // both draw, the colour is the tree's to within a fifth of the range.
  CHECK(overlap > 0.5);
  CHECK(mean < 50.0);
}

namespace {

// A pixel's form, by its cluster's material: 0 nothing of the tree, 1 bark, 2 the finest leaves,
// 3 the first card level, 4 anything coarser.
u32 form_of(const SceneData& data, const CapturedFrame& frame, u32 p) {
  if (frame.ids[p * k_id_words] >= data.instances.size()) return 0u;
  const u32 cluster = frame.ids[p * k_id_words + 1];
  const SourceMesh& source = data.sources[0];
  const u32 local = cluster - data.parts[0].first_cluster;
  if (local >= source.part_of_cluster.size()) return 0u;
  const u32 material = source.part_of_cluster[local];
  return material == 0u ? 1u : material == 1u ? 2u : material == 2u ? 3u : 4u;
}

}  // namespace

TEST_CASE("impostor view: across a switch the levels mix by the dither, the paths agree, no pop") {
  const test::TempDir tmp("renderer_impostor_fade");
  REQUIRE(tmp.ok());
  const std::string oak = tmp.file("oak.flora.json");
  {
    std::ofstream f(oak, std::ios::binary);
    f << k_small_oak;
  }
  SceneDesc desc;
  desc.meshes.push_back(oak);
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  REQUIRE(data.sources[0].lod_coverage.size() == data.sources[0].part_of_cluster.size());

  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  // The camera holds 15 m off the trunk while the threshold sweeps from 0.8 to 1.25 times the
  // settings' over 20 frames (`FrameDesc::lod_px`, which the foliage threshold follows), so the
  // first card level's groups cross their bands with nothing else moving: what changes is the
  // switch.
  CameraPath path;
  CameraPathKey key;
  key.position = WorldPos{0.0, 4.0, 15.0};
  key.target = WorldPos{0.0, 4.0, 0.0};
  key.fov_y = radians(60.0f);
  path.keys.push_back(key);
  key.time = 1.0;
  key.position = WorldPos{0.0, 4.0, 15.0};
  path.keys.push_back(key);
  struct Run {
    Vector<CapturedFrame> frames;
    bool ok = false;
  };
  const auto fly = [&](RasterMode raster, f32 fade, Run& run) {
    RenderSettings settings;
    settings.raster = raster;
    settings.shadows = ShadowMode::Off;
    settings.lod_fade = fade;
    ResolvedSettings resolved;
    resolve_settings(settings, gpu.device.features(), &data, resolved);
    if (check_availability(resolved, gpu.device.features()) != RenderAvailability::Ok) return;
    GpuScene scene;
    REQUIRE_MESSAGE(scene.create(gpu.device, data, resolved, &error), error);
    SceneRenderer renderer;
    SceneRenderer::Desc rd;
    rd.width = 480;
    rd.height = 480;
    REQUIRE_MESSAGE(renderer.create(gpu.device, scene, resolved, rd, &error), error);
    CaptureChannels channels;
    channels.ids = true;
    for (u32 f = 0; f < 20; ++f) {
      FrameDesc frame;
      frame.frame_index = f;
      frame.camera = sample_camera_path(path, 0.0);
      frame.lod_px = 0.8f * std::pow(1.5625f, static_cast<f32>(f) / 19.0f);
      CapturedFrame captured;
      REQUIRE_MESSAGE(renderer.render_offscreen(frame, &error), error);
      REQUIRE_MESSAGE(renderer.capture(frame, channels, captured, &error), error);
      run.frames.push_back(std::move(captured));
    }
    run.ok = true;
  };
  Run faded, sudden, vertex, traced, vertex_sudden, traced_sudden;
  fly(RasterMode::Hardware, 0.1f, faded);
  REQUIRE(faded.ok);
  fly(RasterMode::Hardware, 0.0f, sudden);
  fly(RasterMode::Vertex, 0.1f, vertex);
  fly(RasterMode::RayTrace, 0.1f, traced);
  fly(RasterMode::Vertex, 0.0f, vertex_sudden);
  fly(RasterMode::RayTrace, 0.0f, traced_sudden);

  // The dither: 2 x 2 blocks holding both the leaves and the first cards, faded against sudden —
  // a hard switch mixes them only along a group's edge, the band's screen door everywhere inside.
  u32 frame_cards = 0;
  u64 mixed_faded = 0, mixed_sudden = 0;
  f64 step_faded = 0.0, step_sudden = 0.0;  // the largest frame-to-frame change of the cards' share
  f64 previous_faded = -1.0, previous_sudden = -1.0;
  f64 pop_faded = 0.0, pop_sudden = 0.0;  // the largest mean 8x8-block change of light, of 255
  const u32 w = faded.frames[0].width;
  for (u32 f = 0; f < 20; ++f) {
    const CapturedFrame& a = faded.frames[f];
    const auto share = [&](const CapturedFrame& frame, u64& mixed) {
      u32 tree = 0, cards = 0;
      for (u32 p = 0; p < frame.width * frame.height; ++p) {
        const u32 form = form_of(data, frame, p);
        tree += form != 0u ? 1u : 0u;
        cards += form == 3u ? 1u : 0u;
      }
      for (u32 y = 0; y + 1 < frame.height; y += 2)
        for (u32 x = 0; x + 1 < w; x += 2) {
          bool leaf = false, card = false;
          for (const u32 p : {y * w + x, y * w + x + 1, (y + 1) * w + x, (y + 1) * w + x + 1}) {
            const u32 form = form_of(data, frame, p);
            leaf = leaf || form == 2u;
            card = card || form == 3u;
          }
          mixed += leaf && card ? 1u : 0u;
        }
      return tree > 0 ? static_cast<f64>(cards) / static_cast<f64>(tree) : 0.0;
    };
    const f64 s_faded = share(a, mixed_faded);
    frame_cards += s_faded > 0.0 ? 1u : 0u;
    if (previous_faded >= 0.0) {
      const f64 d = std::abs(s_faded - previous_faded);
      step_faded = d > step_faded ? d : step_faded;
    }
    previous_faded = s_faded;
    if (!sudden.frames.empty()) {
      const f64 s_sudden = share(sudden.frames[f], mixed_sudden);
      if (previous_sudden >= 0.0) {
        const f64 d = std::abs(s_sudden - previous_sudden);
        step_sudden = d > step_sudden ? d : step_sudden;
      }
      previous_sudden = s_sudden;
    }
    // **Popping**: the picture low-passed to 8 x 8 blocks (so a screen door's noise averages out),
    // and the mean over the blocks of how much a block's light changes from the frame before — the
    // camera's own motion moves both runs alike; a group switching whole moves its blocks at once,
    // where a faded one spreads the change over the band's frames.
    if (f > 0) {
      for (u32 r = 0; r < 2; ++r) {
        const Run& run = r == 0 ? faded : sudden;
        if (run.frames.empty()) continue;
        const CapturedFrame& now = run.frames[f];
        const CapturedFrame& was = run.frames[f - 1];
        f64 total = 0.0;
        u32 blocks = 0;
        for (u32 by = 0; by + 8 <= now.height; by += 8)
          for (u32 bx = 0; bx + 8 <= w; bx += 8) {
            f64 a_sum = 0.0, b_sum = 0.0;
            for (u32 y = by; y < by + 8; ++y)
              for (u32 x = bx; x < bx + 8; ++x)
                for (u32 k = 0; k < 3; ++k) {
                  a_sum += now.color[(y * w + x) * 4u + k];
                  b_sum += was.color[(y * w + x) * 4u + k];
                }
            total += std::abs(a_sum - b_sum) / (64.0 * 3.0);
            ++blocks;
          }
        const f64 moved = blocks > 0 ? total / blocks : 0.0;
        f64& worst = r == 0 ? pop_faded : pop_sudden;
        worst = moved > worst ? moved : worst;
      }
    }
  }
  MESSAGE(
      "cross-fade over 20 frames of a threshold sweep at 15 m: the first cards' share of the "
      "tree's pixels "
      "moves at most "
      << step_faded << " a frame faded, " << step_sudden << " switched at once; 2x2 blocks "
      << "holding leaves and cards " << mixed_faded << " faded, " << mixed_sudden
      << " at once; the largest frame-to-frame mean 8x8-block change " << pop_faded
      << " of 255 faded, " << pop_sudden << " at once");
  CHECK(frame_cards > 0u);
  CHECK(mixed_faded > 2u * mixed_sudden);  // the screen door inside the band
  CHECK(step_faded < step_sudden);         // the switch spread over the band's frames
  CHECK(pop_faded < pop_sudden);

  // Inside the band every path draws the same ids: the mesh shaders, the vertex path and the ray
  // path compute the splits alike and hash the same pixels (alpha_mask.slang, `fade_keeps`).
  const struct {
    const Run* run;
    const Run* against;
    const char* name;
  } pairs[] = {{&vertex, &faded, "vertex path, faded"},
               {&traced, &faded, "ray path, faded"},
               {&vertex_sudden, &sudden, "vertex path, at once"},
               {&traced_sudden, &sudden, "ray path, at once"}};
  for (const auto& pair : pairs) {
    const Run* other = pair.run;
    if (!other->ok || !pair.against->ok) continue;
    u64 mismatched = 0, compared = 0;
    for (u32 f = 0; f < 20; ++f) {
      const CapturedFrame& a = pair.against->frames[f];
      const CapturedFrame& b = other->frames[f];
      for (u32 p = 0; p < a.width * a.height; ++p) {
        ++compared;
        if (a.ids[p * k_id_words] != b.ids[p * k_id_words] ||
            a.ids[p * k_id_words + 1] != b.ids[p * k_id_words + 1])
          ++mismatched;
      }
    }
    MESSAGE(std::string(pair.name) << ": " << mismatched << " of " << compared
                                   << " pixels' ids differ from the mesh path's");
    CHECK(mismatched * 1000u <= compared);
  }
}
#endif  // ENGINE_RENDERER_TESTS_FLORA
