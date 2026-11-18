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
//   composite of the frame's own light and the blue's decoded light — on scRGB within a figure of
//   its light, on HDR10 every BT.2020 code inside the window such a composite may store
//   (`hdr10_blend_window`, docs/experiments/hdr10-code-rounding.md); outside the quads the frame
//   is the frame without them to a half's rounding and the dither's code.
// - **The frame after it without an overlay** is the frame before it, to the byte: the encode for
//   a picture with no HUD is unchanged, though the UI layer now exists.
// - **A debug view on HDR10** (no linear path) draws the overlay too, encoded by its own shader:
//   the white is paper white there as well.

#include "display_reference.h"
#include "test_device.h"

#include <domain/gfx/capture.h>
#include <domain/gfx/device.h>
#include <domain/gfx/display.h>
#include <domain/gfx/visibility_resolve.h>
#include <foundation/image/exr.h>
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
#include <cstring>
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
    if (!device.create(renderer_test::device_options(), &error)) {
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
constexpr u32 k_blue_codes[3] = {64, 128, 255};
constexpr u32 k_blue_alpha = 128;
f64 blue_linear(u32 c) { return std::pow(static_cast<f64>(k_blue_codes[c]) / 255.0, 2.2); }

// ---- HDR10's blend in BT.2020 codes (docs/experiments/hdr10-code-rounding.md; ADR-0075) -------
//
// **What the encode promises** for a pixel of a UI quad over an HDR10 picture, in each BT.2020
// channel by itself, with no matrix between the promise and the check: the stored code is the
// rounding of the composite's PQ value plus the dither, where the composite is the frame's own
// light times one minus the layer's coverage plus the layer's light, at paper white. A check in
// Rec. 709 light (the figure this replaced: 3 % of the light, a measurement and not a bound, which
// one colour in six of the space exceeds under either rounding) reads three BT.2020 channels
// through a matrix whose red row is about 1.66, -0.59, -0.07, so a code on a bright green lands on a
// dark red at the green's size. The case knows the under only as the frame-without-it's stored code
// `u` (the stand-in sky's undithered clear), the layer only as the colour and alpha it was given,
// so the window is an interval, and PQ being monotone it is exact at its ends:
//   - the under's true PQ value lies in [u - 1/2, u + 9/16]: rounded to nearest, or by a store that
//     cuts to four bits under a code first (a sixteenth below the edge) — the clear is still the
//     store's;
//   - the linear image holds the under as a half (relative error 2^-11; the matrix's entries are all
//     positive, so the BT.2020 nits keep that bound);
//   - the 8-bit sRGB layer holds the premultiplied colour at one of the two codes round it;
//   - the dither moves the value at most a code, and the rounding half a code (a sixteenth more for
//     a store's cut), so the code is in [v - 25/16, v + 3/2];
//   - and 1/32 of a code for the device's float32 against this double.
// It holds every correct composite by construction (the CPU case below simulates them) and still
// refuses a composite done in PQ's curve, which is the fault a blend in the wrong space would be.
constexpr f64 k_pq_steps = 1023.0;
f64 pq_code(f64 nits) { return gfx::reference::pq_encode(nits) * k_pq_steps; }
f64 pq_nits(f64 code) { return gfx::reference::pq_decode(code / k_pq_steps); }
f64 srgb_decode(f64 v) { return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4); }
f64 srgb_encode(f64 l) {
  return l <= 0.0031308 ? 12.92 * l : 1.055 * std::pow(l, 1.0 / 2.4) - 0.055;
}
f64 bt2020_of(const f64 rec709[3], u32 row) {
  f64 s = 0.0;
  for (u32 c = 0; c < 3; ++c)
    s += static_cast<f64>(gfx::k_bt709_to_bt2020[row][c]) * rec709[c];
  return s;
}

// The premultiplied colour the layer holds for a UI colour of 8-bit `codes` at 8-bit `alpha`, as
// 8-bit sRGB codes before the store rounds them (the hardware blends "over" in linear light into a
// layer cleared to zero).
void layer_codes(const u32 codes[3], u32 alpha, f64 out[3]) {
  for (u32 c = 0; c < 3; ++c) {
    const f64 lin = std::pow(static_cast<f64>(codes[c]) / 255.0, 2.2);
    out[c] = srgb_encode(lin * static_cast<f64>(alpha) / 255.0) * 255.0;
  }
}

struct CodeWindow {
  f64 lo[3];
  f64 hi[3];
};

// The window for one pixel: `under` the frame-without-it's stored BT.2020 codes there, the UI's
// 8-bit colour and alpha, paper white in nits.
CodeWindow hdr10_blend_window(const u32 under[3], const u32 ui[3], u32 alpha, f64 white) {
  const f64 a = static_cast<f64>(alpha) / 255.0;
  f64 code[3], layer_lo[3], layer_hi[3];
  layer_codes(ui, alpha, code);
  for (u32 c = 0; c < 3; ++c) {
    layer_lo[c] = srgb_decode(std::floor(code[c]) / 255.0);
    layer_hi[c] = srgb_decode(std::ceil(code[c]) / 255.0);
  }
  CodeWindow w{};
  constexpr f64 k_half = 0x1p-11;
  constexpr f64 k_float = 1.0 / 32.0;
  for (u32 i = 0; i < 3; ++i) {
    const f64 u = static_cast<f64>(under[i]);
    const f64 ui_lo = bt2020_of(layer_lo, i) * white;
    const f64 ui_hi = bt2020_of(layer_hi, i) * white;
    w.lo[i] = pq_code((1.0 - a) * pq_nits(u - 0.5) * (1.0 - k_half) + ui_lo) - 25.0 / 16.0 - k_float;
    w.hi[i] = pq_code((1.0 - a) * pq_nits(u + 9.0 / 16.0) * (1.0 + k_half) + ui_hi) + 1.5 + k_float;
  }
  return w;
}

// An HDR10 picture's stored BT.2020 codes, three a pixel, read from the target itself.
void stored_codes(const gfx::Device& device, Rig& rig, Vector<u32>& out) {
  gfx::Capture raw;
  REQUIRE_MESSAGE(gfx::capture_image(device, rig.renderer.color_target(),
                                     gfx::ImageLayout::TransferSrc, raw, &rig.error),
                  rig.error);
  REQUIRE(raw.format == gfx::Format::A2B10G10R10Unorm);
  REQUIRE(raw.width == k_width);
  REQUIRE(raw.height == k_height);
  out.resize(k_width * k_height * 3);
  for (u32 i = 0; i < k_width * k_height; ++i) {
    u32 word = 0;
    std::memcpy(&word, raw.bytes.data() + u64{i} * 4, sizeof(word));
    for (u32 c = 0; c < 3; ++c)
      out[3 * i + c] = (word >> (10 * c)) & 1023u;
  }
}

// `blend_tolerance` is scRGB's relative figure in light; HDR10's blend is held to its window.
void hold_hdr(gfx::DisplayEncoding encoding, f64 white_tolerance, f64 outside_tolerance,
              f64 blend_tolerance) {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const bool pq = encoding == gfx::DisplayEncoding::Pq;
  Rig rig;
  REQUIRE_MESSAGE(rig.build(gpu.device, encoding), rig.error);
  CapturedFrame before;
  REQUIRE_MESSAGE(shoot(rig, looking(), before, true), rig.error);
  Vector<u32> before_codes;
  if (pq) stored_codes(gpu.device, rig, before_codes);
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
  Vector<u32> with_codes;
  if (pq) stored_codes(gpu.device, rig, with_codes);

  f64 white_low = 1.0e9, white_high = 0.0, blend_worst = 0.0, outside_worst = 0.0;
  u32 outside_codes = 0;      // HDR10: the most a channel's code moved outside the quads
  u64 blend_out = 0;          // HDR10: channels of the blue outside their window
  f64 blend_margin = 1.0e9;   // HDR10: the least room any code had inside its window, in codes
  u32 blend_lo[3] = {1023, 1023, 1023}, blend_hi[3] = {0, 0, 0};
  bool one_under = true;      // HDR10: every pixel under the blue is one colour (the window's under)
  for (u32 y = 0; y < k_height; ++y) {
    for (u32 x = 0; x < k_width; ++x) {
      const u32 p = y * k_width + x;
      if (pq && in_blue(x, y)) {
        const u32* u = &before_codes[3 * p];
        for (u32 c = 0; c < 3; ++c)
          one_under = one_under && u[c] == before_codes[3 * (20 * k_width + 100) + c];
        const CodeWindow w = hdr10_blend_window(u, k_blue_codes, k_blue_alpha, k_paper_white);
        for (u32 c = 0; c < 3; ++c) {
          const u32 code = with_codes[3 * p + c];
          const f64 v = static_cast<f64>(code);
          if (v < w.lo[c] || v > w.hi[c]) ++blend_out;
          blend_margin = std::min(blend_margin, std::min(v - w.lo[c], w.hi[c] - v));
          blend_lo[c] = std::min(blend_lo[c], code);
          blend_hi[c] = std::max(blend_hi[c], code);
        }
      }
      for (u32 c = 0; c < 3; ++c) {
        const u32 i = p * 3 + c;
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
          if (pq) {
            const i64 d = static_cast<i64>(with_codes[i]) - static_cast<i64>(before_codes[i]);
            outside_codes = std::max(outside_codes, static_cast<u32>(d < 0 ? -d : d));
          }
        }
      }
    }
  }
  MESSAGE(std::string(gfx::display_encoding_name(encoding))
          << ": the white's light " << white_low << ".." << white_high
          << " of paper white, the blend within " << blend_worst * 100.0
          << " % of its light, outside the quads within " << outside_worst * 100.0 << " %");
  CHECK(white_low >= 1.0 - white_tolerance);
  CHECK(white_high <= 1.0 + white_tolerance);  // UI white is paper white, never past it
  CHECK(outside_worst <= outside_tolerance);
  if (pq) {
    const u32* u = &before_codes[3 * (20 * k_width + 100)];
    const CodeWindow w = hdr10_blend_window(u, k_blue_codes, k_blue_alpha, k_paper_white);
    MESSAGE("hdr10: under the blue (" << u[0] << ", " << u[1] << ", " << u[2]
                                      << "), the blend's codes red " << blend_lo[0] << ".."
                                      << blend_hi[0] << ", green " << blend_lo[1] << ".."
                                      << blend_hi[1] << ", blue " << blend_lo[2] << ".."
                                      << blend_hi[2] << " in windows " << w.lo[0] << ".."
                                      << w.hi[0] << ", " << w.lo[1] << ".." << w.hi[1] << ", "
                                      << w.lo[2] << ".." << w.hi[2] << " (" << blend_out
                                      << " outside, least room " << blend_margin
                                      << " codes); outside the quads a code moved at most "
                                      << outside_codes);
    CHECK(one_under);
    CHECK(blend_out == 0);
  } else {
    CHECK(blend_worst <= blend_tolerance);
  }

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
  // A PQ code at 200 nits is about 0.9 % of the light; the dither moves a channel a code. The
  // blend is held in BT.2020 codes to its window (`hdr10_blend_window`), not to a figure in light.
  hold_hdr(gfx::DisplayEncoding::Pq, 0.02, 0.03, 0.0);
}

TEST_CASE("overlay: on scRGB its white is paper white and its blend is in linear light") {
  // Half floats and an 8-bit sRGB layer: no dither, a few parts in a thousand.
  hold_hdr(gfx::DisplayEncoding::ScRgb, 0.005, 0.02, 0.02);
}

// **The window holds every linear composite and refuses a composite in PQ's curve**, with no
// device: random unders below paper white, any 8-bit UI colour and alpha, the layer at either of
// its two codes, every dither value on a grid of 129 from -1 to 1, the three channels, and both
// rounding rules for each of the two frames — to nearest (the encode's own, ADR-0075) and the
// store's measured cut to four bits under a code before it rounds (the clear's, still the store's).
// The simulation is the pipeline's arithmetic in double: the under's PQ code as the frame without
// the overlay stores it, the under as the linear image's half, the layer decoded from its sRGB
// code, the composite at paper white, BT.2020, PQ, the dither and the rounding. The same quad
// composited in PQ codes instead of light — the under's PQ value and the colour's, mixed by alpha —
// is the fault the window is there to see.
TEST_CASE("overlay: HDR10's blend window holds every linear composite, not one in PQ's curve") {
  u64 state = 0x9E3779B97F4A7C15ull;
  const auto next = [&]() {  // splitmix64
    state += 0x9E3779B97F4A7C15ull;
    u64 z = state;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
  };
  const auto uniform = [&]() { return static_cast<f64>(next() >> 11) * 0x1p-53; };
  const auto nearest = [](f64 x) { return std::floor(x + 0.5); };
  const auto cut = [](f64 x) {  // to 14 fraction bits of the unit value, then nearest
    return std::floor(std::floor(x / k_pq_steps * 16384.0) / 16384.0 * k_pq_steps + 0.5);
  };
  constexpr u32 k_cases = 4000;
  constexpr u32 k_dithers = 129;
  constexpr f64 k_white = static_cast<f64>(k_paper_white);
  u64 checked = 0, outside = 0, caught = 0, mixed = 0;
  f64 widest = 0.0;
  for (u32 n = 0; n < k_cases; ++n) {
    f64 under[3];
    u32 ui[3];
    for (u32 c = 0; c < 3; ++c) {
      under[c] = 1.0 / 1024.0 + uniform() * (1.0 - 1.0 / 1024.0);  // a normal half, below white
      ui[c] = static_cast<u32>(next() % 256);
    }
    const u32 alpha = static_cast<u32>(next() % 256);
    const f64 a = static_cast<f64>(alpha) / 255.0;
    f64 code[3], layer[3], half_under[3];
    layer_codes(ui, alpha, code);
    for (u32 c = 0; c < 3; ++c) {
      layer[c] = srgb_decode((next() & 1 ? std::ceil(code[c]) : std::floor(code[c])) / 255.0);
      half_under[c] =
          static_cast<f64>(image::f32_from_half(image::half_from_f32(static_cast<f32>(under[c]))));
    }
    f64 v_under[3], v_with[3];
    for (u32 i = 0; i < 3; ++i) {
      v_under[i] = pq_code(bt2020_of(under, i) * k_white);
      f64 composite[3];
      for (u32 c = 0; c < 3; ++c)
        composite[c] = (half_under[c] * (1.0 - a) + layer[c]) * k_white;
      v_with[i] = pq_code(bt2020_of(composite, i));
    }
    for (const bool before_cut : {false, true}) {
      u32 u[3];
      for (u32 i = 0; i < 3; ++i)
        u[i] = static_cast<u32>(before_cut ? cut(v_under[i]) : nearest(v_under[i]));
      const CodeWindow w = hdr10_blend_window(u, ui, alpha, k_white);
      for (u32 i = 0; i < 3; ++i) {
        widest = std::max(widest, w.hi[i] - w.lo[i]);
        const f64 room = std::min(std::min(v_with[i], k_pq_steps - v_with[i]), 1.0);
        for (u32 k = 0; k < k_dithers; ++k) {
          const f64 d = -1.0 + 2.0 * static_cast<f64>(k) / static_cast<f64>(k_dithers - 1);
          const f64 x = std::clamp(v_with[i] + d * room, 0.0, k_pq_steps);
          for (const f64 c : {nearest(x), cut(x)}) {
            ++checked;
            if (c < w.lo[i] || c > w.hi[i]) ++outside;
          }
        }
      }
      // The same quad composited in PQ codes: the under's code and the straight colour's, mixed by
      // alpha, rounded to nearest. Only a colour that covers something can be told apart.
      if (alpha == 0 || before_cut) continue;
      ++mixed;
      bool out = false;
      f64 straight[3];
      for (u32 c = 0; c < 3; ++c)
        straight[c] = layer[c] / a * k_white;
      for (u32 i = 0; i < 3; ++i) {
        const f64 bad = nearest((1.0 - a) * v_under[i] + a * pq_code(bt2020_of(straight, i)));
        out = out || bad < w.lo[i] || bad > w.hi[i];
      }
      if (out) ++caught;
    }
  }
  const f64 caught_share = static_cast<f64>(caught) / static_cast<f64>(mixed);
  MESSAGE("hdr10 blend window: " << outside << " of " << checked
                                 << " simulated codes outside (widest window " << widest
                                 << " codes); a composite in PQ codes caught in "
                                 << caught_share * 100.0 << " % of " << mixed << " colours");
  CHECK(outside == 0);
  CHECK(caught_share > 0.8);
}
