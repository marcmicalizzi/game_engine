// The HDR output encodes on a GPU (gfx/display.h, display.slang; docs/subsystems/renderer.md,
// "HDR output"; E39, docs/experiments/hdr-output-proposal.md).
//
// **What it holds.** The same frame drawn three ways — the SDR picture into a 10-bit target (what a
// 10-bit window presents today), HDR10 into the same format with the PQ encode, and scRGB into a
// half-float target — and read back. Decoded to light relative to paper white (the SDR codes
// through the 1/2.2 power, the PQ codes through ST 2084 and back from BT.2020 to Rec. 709, the
// half floats times 80 nits), the HDR pictures are the SDR picture wherever the SDR picture shows a
// channel below its shoulder's knee — 1 for the stand-in, which has none, and 0.6 under a sky —
// within one code of each encoding there (the dither's amplitude, which both pictures carry); and
// nothing in either goes past the display's peak. The stand-in's flat clear goes through the same
// conversion on the CPU and is held with the rest.
//
// Each case skips with a message where there is no device. The sky's case is compiled where the
// sky capability is (ENGINE_HDR_TESTS_SKY).
#include "display_reference.h"

#include <domain/gfx/capture.h>
#include <domain/gfx/device.h>
#include <domain/gfx/display.h>
#include <domain/gfx/sky.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#if ENGINE_HDR_TESTS_SKY
#include <domain/scene_gen/scene_gen.h>
#endif

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_width = 320;
constexpr u32 k_height = 180;
constexpr f64 k_paper_white = 200.0;
constexpr f64 k_peak = 1000.0;

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

SceneDesc hills(bool sky) {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.heightfield_grid = 65;
  desc.cache = false;
  if (sky) {
    scene::Sky entry;
    entry.latitude_deg = 31.1f;
    entry.day_of_year = 100.0f;
    entry.turbidity = 1.6f;
    entry.ground_albedo = 0.38f;
    desc.sky = entry;
  }
  return desc;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, bool sky, gfx::Format format,
             gfx::DisplayEncoding encoding) {
    if (!load_scene(hills(sky), data, error)) return false;
    RenderSettings settings;
    settings.shadows = ShadowMode::Off;
    settings.peak_nits = static_cast<f32>(k_peak);
    settings.paper_white_nits = static_cast<f32>(k_paper_white);
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    rd.color_format = format;
    rd.display = encoding;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

f32 half_to_float(u16 h) {
  const u32 sign = (h >> 15) & 1u;
  const u32 exponent = (h >> 10) & 31u;
  const u32 mantissa = h & 1023u;
  f32 v = 0.0f;
  if (exponent == 0) {
    v = std::ldexp(static_cast<f32>(mantissa), -24);
  } else if (exponent == 31) {
    v = mantissa == 0 ? 65504.0f : 0.0f;  // never written: the encode is finite
  } else {
    v = std::ldexp(static_cast<f32>(mantissa | 1024u), static_cast<i32>(exponent) - 25);
  }
  return sign != 0 ? -v : v;
}

// One frame read back as light relative to paper white, three channels a pixel, in Rec. 709, and
// for each channel the size of one code of its encoding there (what the comparison allows).
struct Light {
  Vector<f64> rel;
  Vector<f64> step;
};

void shoot(const gfx::Device& device, Rig& rig, const FrameDesc& frame, Light& out) {
  CaptureChannels channels;
  channels.color = false;
  CapturedFrame shot;
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
  gfx::Capture raw;
  REQUIRE_MESSAGE(gfx::capture_image(device, rig.renderer.color_target(),
                                     gfx::ImageLayout::TransferSrc, raw, &rig.error),
                  rig.error);
  REQUIRE(raw.width == k_width);
  const u32 n = raw.width * raw.height;
  out.rel.resize(3 * n);
  out.step.resize(3 * n);
  f64 m_inv[3][3];
  {
    f64 m[3][3];
    gfx::reference::bt709_to_bt2020(m);
    gfx::reference::invert3(m, m_inv);
  }
  const gfx::DisplayEncoding encoding = rig.renderer.display();
  for (u32 i = 0; i < n; ++i) {
    const u8* p = raw.bytes.data() + u64{i} * raw.bytes_per_pixel;
    if (raw.format == gfx::Format::A2B10G10R10Unorm) {
      u32 word = 0;
      std::memcpy(&word, p, sizeof(word));
      f64 code[3];
      for (u32 c = 0; c < 3; ++c)
        code[c] = static_cast<f64>((word >> (10 * c)) & 1023u);
      if (encoding == gfx::DisplayEncoding::Sdr) {
        for (u32 c = 0; c < 3; ++c) {
          const f64 v = code[c] / 1023.0;
          out.rel[3 * i + c] = std::pow(v, 2.2);
          out.step[3 * i + c] = std::pow(std::min(1.0, v + 1.0 / 1023.0), 2.2) - std::pow(v, 2.2);
        }
      } else {
        // PQ codes are BT.2020 nits; back to Rec. 709 through the inverse, then to paper white.
        f64 nits[3];
        f64 widest = 0.0;
        for (u32 c = 0; c < 3; ++c) {
          nits[c] = gfx::reference::pq_decode(code[c] / 1023.0);
          widest = std::max(
              widest, gfx::reference::pq_decode(std::min(1.0, (code[c] + 1.0) / 1023.0)) - nits[c]);
        }
        for (u32 r = 0; r < 3; ++r) {
          f64 s = 0.0;
          f64 spread = 0.0;
          for (u32 c = 0; c < 3; ++c) {
            s += m_inv[r][c] * nits[c];
            spread += std::fabs(m_inv[r][c]) * widest;
          }
          out.rel[3 * i + r] = s / k_paper_white;
          out.step[3 * i + r] = spread / k_paper_white;
        }
      }
    } else if (raw.format == gfx::Format::R16G16B16A16Sfloat) {
      for (u32 c = 0; c < 3; ++c) {
        u16 h = 0;
        std::memcpy(&h, p + 2 * c, sizeof(h));
        const f64 v = static_cast<f64>(half_to_float(h)) * static_cast<f64>(gfx::k_scrgb_unit_nits) / k_paper_white;
        out.rel[3 * i + c] = v;
        out.step[3 * i + c] = std::fabs(v) / 1024.0 + 1e-6;  // a half's last bit
      }
    } else {
      FAIL("a format this test does not read");
    }
  }
}

// The HDR picture against the SDR one wherever the SDR picture shows a channel below `knee`.
struct Agreement {
  u64 compared = 0;
  u64 outside = 0;
  f64 worst = 0.0;  // in steps allowed
  f64 brightest_nits = 0.0;
  u32 at = 0;
  f64 want = 0.0;
  f64 got = 0.0;
};

Agreement agree(const Light& sdr, const Light& hdr, f64 knee) {
  Agreement a;
  for (u32 i = 0; i < sdr.rel.size(); ++i) {
    a.brightest_nits = std::max(a.brightest_nits, hdr.rel[i] * k_paper_white);
    if (sdr.rel[i] >= knee * 0.98) continue;  // at the knee a code either side can be past it
    ++a.compared;
    const f64 allowed = 1.5 * (sdr.step[i] + hdr.step[i]) + 1e-5;
    const f64 d = std::fabs(hdr.rel[i] - sdr.rel[i]);
    if (d / allowed > a.worst) {
      a.worst = d / allowed;
      a.at = i;
      a.want = sdr.rel[i];
      a.got = hdr.rel[i];
    }
    if (d > allowed) ++a.outside;
  }
  return a;
}

FrameDesc looking(f64 time_s) {
  FrameDesc frame;
  const Vec3 eye{0.0f, 3.0f, 0.0f};
  const Vec3 towards = normalize(Vec3{0.35f, -0.12f, -1.0f});
  frame.camera.position = absolute(WorldPos::origin(), eye);
  frame.camera.target = absolute(WorldPos::origin(), eye + towards * 100.0f);
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.camera.znear = 0.05f;
  frame.sun_time_s = time_s;
  return frame;
}

void hold(bool sky, f64 time_s, f64 knee) {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const FrameDesc frame = looking(time_s);
  Rig sdr;
  REQUIRE_MESSAGE(
      sdr.build(gpu.device, sky, gfx::Format::A2B10G10R10Unorm, gfx::DisplayEncoding::Sdr),
      sdr.error);
  Rig pq;
  REQUIRE_MESSAGE(
      pq.build(gpu.device, sky, gfx::Format::A2B10G10R10Unorm, gfx::DisplayEncoding::Pq), pq.error);
  Rig scrgb;
  REQUIRE_MESSAGE(
      scrgb.build(gpu.device, sky, gfx::Format::R16G16B16A16Sfloat, gfx::DisplayEncoding::ScRgb),
      scrgb.error);
  Light a;
  Light b;
  Light c;
  shoot(gpu.device, sdr, frame, a);
  shoot(gpu.device, pq, frame, b);
  shoot(gpu.device, scrgb, frame, c);
  for (const auto& [name, light] : {std::pair<const char*, const Light*>{"hdr10", &b},
                                    std::pair<const char*, const Light*>{"scrgb", &c}}) {
    CAPTURE(name);
    const Agreement got = agree(a, *light, knee);
    MESSAGE(name << ": " << got.compared << " channels below the knee compared, " << got.outside
                 << " outside one code of each encoding (worst " << got.worst
                 << " of the allowance, at channel " << got.at << " (pixel " << got.at / 3 % k_width
                 << ", " << got.at / 3 / k_width << "): sdr " << got.want << " hdr " << got.got
                 << "); brightest " << got.brightest_nits << " nits");
    CHECK(got.compared > static_cast<u64>(k_width) * k_height);  // most of the picture
    CHECK(got.outside == 0);
    CHECK(got.brightest_nits <= k_peak * 1.002 + 1.0);  // never past the peak (a code's slack)
  }
  // The HDR10 picture and the scRGB one are the same light, to the coarser of the two codes.
  u64 differ = 0;
  for (u32 i = 0; i < b.rel.size(); ++i) {
    if (std::fabs(b.rel[i] - c.rel[i]) > 1.5 * (b.step[i] + c.step[i]) + 1e-5) ++differ;
  }
  CHECK(differ == 0);
  // The same frame twice is the same bytes in HDR10 too: the noise is the pixel's alone.
  Light again;
  shoot(gpu.device, pq, frame, again);
  CHECK(again.rel == b.rel);
}

}  // namespace

TEST_CASE(
    "hdr output: HDR10 and scRGB are the SDR picture below paper white, and stop at the "
    "peak (the stand-in, no sky)") {
  hold(false, 0.0, 1.0);
}

#if ENGINE_HDR_TESTS_SKY
TEST_CASE("hdr output: under a sky, HDR10 and scRGB are the SDR picture below the shoulder") {
  REQUIRE(scene_gen::GeneratorRegistry::global().find_sky("earth") != nullptr);
  hold(true, 16.0 * 3600.0, static_cast<f64>(gfx::k_sky_shoulder));
}
#endif

TEST_CASE("hdr output: an HDR encoding refuses a target that cannot hold it") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  Rig rig;
  CHECK_FALSE(rig.build(gpu.device, false, gfx::Format::R8G8B8A8Unorm, gfx::DisplayEncoding::Pq));
  CHECK(rig.error.find("10-bit packed") != std::string::npos);
}
