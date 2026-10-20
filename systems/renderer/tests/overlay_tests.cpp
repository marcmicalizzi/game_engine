// The overlay's composite on each target (overlay.h; docs/subsystems/renderer.md, "The overlay";
// ADR-0054 decision 3 and its status note of 2026-10-19; roadmap P49).
//
// **What it holds.** One frame of the hills drawn without an overlay and then with two quads over
// it — opaque white, and a blue at half alpha — on an SDR, an HDR10 and an scRGB target:
//
// - **SDR, in the encoded picture as it always was**: outside the quads the frame is the frame
//   without them to the byte; the white is code 255; the half-alpha quad is a straight-alpha blend
//   of the *codes* (the space the UI is authored in on SDR) to a code — which a composite in linear
//   light would miss by a dozen codes, so this is the pin that SDR did not move.
// - **HDR10 and scRGB, in linear light at paper white**: the white is paper white (light 1.0, which
//   is PQ's code for 200 nits and scRGB's 2.5), never more; the half-alpha quad is the linear
//   composite of the frame's own light and the blue's decoded light; outside the quads the frame
//   is the frame without them to a half's rounding and the dither's code.
// - **The frame after it without an overlay** is the frame before it, to the byte: the encode for
//   a picture with no HUD is unchanged, though the UI layer now exists.
// - **A debug view on HDR10** (no linear path) draws the overlay too, encoded by its own shader:
//   the white is paper white there as well.

#include <domain/gfx/device.h>
#include <domain/gfx/display.h>
#include <domain/gfx/visibility_resolve.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/overlay.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_width = 320;
constexpr u32 k_height = 180;
constexpr f32 k_paper_white = 200.0f;

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

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, gfx::DisplayEncoding encoding) {
    SceneDesc desc;
    desc.meshes.push_back("");
    desc.heightfield_grid = 65;
    desc.cache = false;
    if (!load_scene(desc, data, error)) return false;
    RenderSettings settings;
    settings.shadows = ShadowMode::Off;
    settings.paper_white_nits = k_paper_white;
    settings.peak_nits = 1000.0f;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    rd.display = encoding;
    if (encoding != gfx::DisplayEncoding::Sdr)
      rd.color_format = gfx::display_encoding_format(encoding);
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

FrameDesc looking(u32 view_mode = ~u32{0}) {
  FrameDesc frame;
  const Vec3 eye{0.0f, 3.0f, 0.0f};
  const Vec3 towards = normalize(Vec3{0.35f, -0.12f, -1.0f});
  frame.camera.position = absolute(WorldPos::origin(), eye);
  frame.camera.target = absolute(WorldPos::origin(), eye + towards * 100.0f);
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.camera.znear = 0.05f;
  frame.view_mode = view_mode;
  return frame;
}

// Two quads: opaque white over [20, 80) x [20, 60), and (64, 128, 255) at alpha 128 over
// [100, 180) x [20, 60), textured by a one-texel white texture.
struct Quads {
  OverlayVertex vertices[8];
  u32 indices[12];
  OverlayCommand command;
  OverlayDrawData data;

  explicit Quads(u32 texture) {
    const auto quad = [&](u32 q, f32 x0, f32 y0, f32 x1, f32 y1, u32 rgba) {
      vertices[4 * q + 0] = OverlayVertex{x0, y0, 0.5f, 0.5f, rgba};
      vertices[4 * q + 1] = OverlayVertex{x1, y0, 0.5f, 0.5f, rgba};
      vertices[4 * q + 2] = OverlayVertex{x1, y1, 0.5f, 0.5f, rgba};
      vertices[4 * q + 3] = OverlayVertex{x0, y1, 0.5f, 0.5f, rgba};
      const u32 b = 4 * q;
      const u32 tri[6] = {b, b + 1, b + 2, b, b + 2, b + 3};
      for (u32 i = 0; i < 6; ++i)
        indices[6 * q + i] = tri[i];
    };
    quad(0, 20.0f, 20.0f, 80.0f, 60.0f, 0xFFFFFFFFu);
    quad(1, 100.0f, 20.0f, 180.0f, 60.0f, 0x80FF8040u);
    command.clip_x1 = static_cast<f32>(k_width);
    command.clip_y1 = static_cast<f32>(k_height);
    command.texture = texture;
    command.index_count = 12;
    data.vertices = std::span<const OverlayVertex>(vertices, 8);
    data.indices = std::span<const u32>(indices, 12);
    data.commands = std::span<const OverlayCommand>(&command, 1);
  }
};

bool in_white(u32 x, u32 y) { return x >= 20 && x < 80 && y >= 20 && y < 60; }
bool in_blue(u32 x, u32 y) { return x >= 100 && x < 180 && y >= 20 && y < 60; }

bool shoot(Rig& rig, const FrameDesc& frame, CapturedFrame& out, bool light) {
  CaptureChannels channels;
  channels.color = !light;
  channels.light = light;
  return rig.renderer.capture(frame, channels, out, &rig.error);
}

// The blue's straight alpha and its colour in linear light (the SDR transfer's inverse).
constexpr f64 k_alpha = 128.0 / 255.0;
f64 blue_linear(u32 c) {
  const f64 code[3] = {64.0, 128.0, 255.0};
  return std::pow(code[c] / 255.0, 2.2);
}

void hold_hdr(gfx::DisplayEncoding encoding, f64 white_tolerance, f64 blend_tolerance) {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, encoding), rig.error);
  CapturedFrame before;
  REQUIRE_MESSAGE(shoot(rig, looking(), before, true), rig.error);
  const u8 white[4] = {255, 255, 255, 255};
  std::string error;
  const u32 texture = rig.renderer.overlay().add_texture(1, 1, white, &error);
  REQUIRE_MESSAGE(texture != OverlayPass::k_no_texture, error);
  const Quads quads(texture);
  FrameDesc frame = looking();
  frame.overlay = &quads.data;
  CapturedFrame with;
  REQUIRE_MESSAGE(shoot(rig, frame, with, true), rig.error);
  REQUIRE(with.light.size() == before.light.size());
  REQUIRE(with.light.size() == k_width * k_height * 3);

  f64 white_low = 1.0e9, white_high = 0.0, blend_worst = 0.0, outside_worst = 0.0;
  for (u32 y = 0; y < k_height; ++y) {
    for (u32 x = 0; x < k_width; ++x) {
      for (u32 c = 0; c < 3; ++c) {
        const u32 i = (y * k_width + x) * 3 + c;
        const f64 under = static_cast<f64>(before.light[i]);
        const f64 seen = static_cast<f64>(with.light[i]);
        if (in_white(x, y)) {
          white_low = std::min(white_low, seen);
          white_high = std::max(white_high, seen);
        } else if (in_blue(x, y)) {
          const f64 expect = under * (1.0 - k_alpha) + blue_linear(c) * k_alpha;
          blend_worst = std::max(blend_worst, std::fabs(seen - expect) / std::max(expect, 0.05));
        } else {
          outside_worst = std::max(outside_worst, std::fabs(seen - under) / std::max(under, 0.05));
        }
      }
    }
  }
  MESSAGE(std::string(gfx::display_encoding_name(encoding))
          << ": the white's light " << white_low << ".." << white_high
          << " of paper white, the blend within " << blend_worst * 100.0
          << " %, outside the quads within " << outside_worst * 100.0 << " %");
  CHECK(white_low >= 1.0 - white_tolerance);
  CHECK(white_high <= 1.0 + white_tolerance);  // UI white is paper white, never past it
  CHECK(blend_worst <= blend_tolerance);
  CHECK(outside_worst <= blend_tolerance);

  // The next frame without an overlay is the frame before the first, to the byte: the layer
  // exists now, and a picture with no HUD does not go near it.
  CapturedFrame after;
  REQUIRE_MESSAGE(shoot(rig, looking(), after, true), rig.error);
  REQUIRE(after.light.size() == before.light.size());
  CHECK(std::equal(after.light.begin(), after.light.end(), before.light.begin()));

  // A debug view has no linear path: the overlay is drawn onto the target, encoded by its own
  // shader, and its white is paper white too.
  FrameDesc debug = looking(static_cast<u32>(gfx::ResolveMode::ClusterColors));
  debug.overlay = &quads.data;
  CapturedFrame view;
  REQUIRE_MESSAGE(shoot(rig, debug, view, true), rig.error);
  f64 debug_low = 1.0e9, debug_high = 0.0;
  for (u32 y = 20; y < 60; ++y) {
    for (u32 x = 20; x < 80; ++x) {
      for (u32 c = 0; c < 3; ++c) {
        const f64 v = static_cast<f64>(view.light[(y * k_width + x) * 3 + c]);
        debug_low = std::min(debug_low, v);
        debug_high = std::max(debug_high, v);
      }
    }
  }
  MESSAGE("a debug view's white: " << debug_low << ".." << debug_high);
  CHECK(debug_low >= 1.0 - white_tolerance);
  CHECK(debug_high <= 1.0 + white_tolerance);
}

}  // namespace

TEST_CASE("overlay: on SDR it blends in the encoded picture, outside its quads the frame's bytes") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, gfx::DisplayEncoding::Sdr), rig.error);
  CapturedFrame before;
  REQUIRE_MESSAGE(shoot(rig, looking(), before, false), rig.error);
  const u8 white[4] = {255, 255, 255, 255};
  std::string error;
  const u32 texture = rig.renderer.overlay().add_texture(1, 1, white, &error);
  REQUIRE_MESSAGE(texture != OverlayPass::k_no_texture, error);
  const Quads quads(texture);
  FrameDesc frame = looking();
  frame.overlay = &quads.data;
  CapturedFrame with;
  REQUIRE_MESSAGE(shoot(rig, frame, with, false), rig.error);
  REQUIRE(with.color.size() == before.color.size());
  u32 outside = 0, white_off = 0;
  i32 blend_worst = 0;
  const f64 code[3] = {64.0, 128.0, 255.0};
  for (u32 y = 0; y < k_height; ++y) {
    for (u32 x = 0; x < k_width; ++x) {
      for (u32 c = 0; c < 3; ++c) {
        const u32 i = (y * k_width + x) * 4 + c;
        const i32 seen = with.color[i];
        if (in_white(x, y)) {
          if (seen != 255) ++white_off;
        } else if (in_blue(x, y)) {
          const f64 expect =
              static_cast<f64>(before.color[i]) * (1.0 - k_alpha) + code[c] * k_alpha;
          blend_worst =
              std::max(blend_worst, std::abs(seen - static_cast<i32>(std::lround(expect))));
        } else if (seen != before.color[i]) {
          ++outside;
        }
      }
    }
  }
  MESSAGE("sdr: " << outside << " channels changed outside the quads, " << white_off
                  << " of the white's not 255, the blend within " << blend_worst << " codes");
  CHECK(outside == 0);
  CHECK(white_off == 0);
  CHECK(blend_worst <= 1);
}

TEST_CASE("overlay: on HDR10 its white is paper white and its blend is in linear light") {
  // A PQ code at 200 nits is about 0.8 % of the light; the dither moves a channel a code.
  hold_hdr(gfx::DisplayEncoding::Pq, 0.02, 0.03);
}

TEST_CASE("overlay: on scRGB its white is paper white and its blend is in linear light") {
  // Half floats and an 8-bit sRGB layer: no dither, a few parts in a thousand.
  hold_hdr(gfx::DisplayEncoding::ScRgb, 0.005, 0.02);
}
