// Where the sky's colour bands come from, and what the dither leaves of them (display.h,
// display.slang; docs/subsystems/renderer.md, "The output encode"; ADR-0052; the numbers are in
// docs/experiments/sky-banding-2026-10-04.md).
//
// **The instrument.** The erg's sky (its calendar, over the built-in hills) at the hours whose
// gradient is slowest — a dusk, a dawn and the night under the moon — is drawn into a float target,
// which holds the encoded value the resolve computes before anything quantizes it, and into 8- and
// 10-bit UNORM targets with the dither off and on. Along every column of sky whose float is smooth
// (no star, no disc's edge, no slope of a code a pixel or more) it measures the widest run of one
// code between two one-code steps — a band — the one-code steps, any coarser step where the float
// moved less than a code, the RMS of each pixel's code against its float (the grain), and the RMS
// of the same difference averaged over 8 x 8 blocks (what is left of the bands once the eye has
// averaged the grain away).
//
// **What it established before the dither existed**, and still holds: the float under the bands is
// smooth — nearly every pixel along a line holds a float of its own, so nothing upstream (the sky's
// tables, the aerial table, the exposure) is quantized on the way — and the 8- and 10-bit pictures
// are exactly that float rounded, so every band is a step of one code at the final quantizer, which
// is what dither fixes. Then: the dithered pictures are the CPU mirror's noise rounded, at either
// depth, the bands are gone from the 8 x 8 average, the same frame twice is the same bytes, and a
// 10-bit target holds more than 256 levels along one line of sky.
//
// Compiled where the sky capability is (its "earth" provider); each GPU case skips with a message
// where there is no device.
#include <domain/gfx/capture.h>
#include <domain/gfx/device.h>
#include <domain/gfx/display.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/sky.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

// The erg's calendar, as content/test-scenes/desert-erg/scene.json names it.
scene::Sky erg_sky() {
  scene::Sky entry;
  entry.latitude_deg = 31.1f;
  entry.day_of_year = 100.0f;
  entry.moon_age_days = 12.39f;
  entry.turbidity = 1.6f;
  entry.ground_albedo = 0.38f;
  return entry;
}

scene_gen::SkyProvider make_earth(const scene::Sky& entry) {
  scene_gen::SkyProvider provider;
  const scene_gen::SkyProviderDesc* desc = scene_gen::GeneratorRegistry::global().find_sky("earth");
  REQUIRE(desc != nullptr);
  std::string error;
  REQUIRE_MESSAGE(desc->make(entry, provider, &error), error);
  return provider;
}

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

// The built-in heightfield (a 20 m square of low hills) under the erg's sky.
SceneDesc hills() {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.heightfield_grid = 65;
  desc.cache = false;
  desc.sky = erg_sky();
  return desc;
}

// One renderer of one target format, the dither on or off.
struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  SceneRenderer renderer;
  std::string error;

  bool build(const gfx::Device& device, gfx::Format format, bool dither, u32 width, u32 height) {
    if (!load_scene(hills(), data, error)) return false;
    RenderSettings settings;
    settings.shadows = ShadowMode::Off;
    settings.dither = dither;
    resolve_settings(settings, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    if (!scene.create(device, data, resolved, &error)) return false;
    SceneRenderer::Desc rd;
    rd.width = width;
    rd.height = height;
    rd.color_format = format;
    return renderer.create(device, scene, resolved, rd, &error);
  }
};

Camera aimed(Vec3 eye, Vec3 towards, f32 fov_deg) {
  Camera camera;
  camera.position = eye;
  camera.target = eye + towards * 100.0f;
  camera.fov_y = fov_deg * 3.14159265f / 180.0f;
  camera.znear = 0.05f;
  return camera;
}

// One frame read back raw: three channels a pixel, as the target's codes for a UNORM target and
// as the encoded value in [0, 1] for a float one, and which pixels nothing covers.
struct Picture {
  u32 width = 0;
  u32 height = 0;
  u32 steps = 0;  // the target's code steps: 255, 1023, or 0 for a float target
  Vector<f32> rgb;
  Vector<u8> sky;
};

void shoot(const gfx::Device& device, Rig& rig, const FrameDesc& frame, Picture& out) {
  CaptureChannels channels;
  channels.color = false;
  channels.depth = true;
  CapturedFrame shot;
  REQUIRE_MESSAGE(rig.renderer.capture(frame, channels, shot, &rig.error), rig.error);
  gfx::Capture raw;
  REQUIRE_MESSAGE(gfx::capture_image(device, rig.renderer.color_target(),
                                     gfx::ImageLayout::TransferSrc, raw, &rig.error),
                  rig.error);
  out.width = raw.width;
  out.height = raw.height;
  out.steps = gfx::display_steps(raw.format);
  const u32 n = raw.width * raw.height;
  out.rgb.resize(3 * n);
  out.sky.resize(n);
  for (u32 i = 0; i < n; ++i) {
    out.sky[i] = shot.depth[i] > 0.0f ? 0u : 1u;
    const u8* p = raw.bytes.data() + u64{i} * raw.bytes_per_pixel;
    switch (raw.format) {
      case gfx::Format::R32G32B32A32Sfloat: {
        f32 v[4];
        std::memcpy(v, p, sizeof(v));
        for (u32 c = 0; c < 3; ++c)
          out.rgb[3 * i + c] = v[c];
        break;
      }
      case gfx::Format::R8G8B8A8Unorm:
        for (u32 c = 0; c < 3; ++c)
          out.rgb[3 * i + c] = static_cast<f32>(p[c]);
        break;
      case gfx::Format::A2B10G10R10Unorm: {
        u32 word = 0;
        std::memcpy(&word, p, sizeof(word));
        for (u32 c = 0; c < 3; ++c)
          out.rgb[3 * i + c] = static_cast<f32>((word >> (10 * c)) & 1023u);
        break;
      }
      default: FAIL("a format the instrument does not read");
    }
  }
}

// ---- the dither's candidates, on the CPU
// -----------------------------------------------------------
//
// Chosen between by what each leaves of the bands and what grain it adds (the measurement case
// below and the experiment page): interleaved gradient noise remapped to a triangular distribution,
// which is display.h's and the shader's; white triangular noise, the sum of two uniforms from an
// integer hash (pcg2d); and rectangular white noise of half a code, the classic dither whose grain
// comes and goes with the signal.
enum class Noise : u8 { None, IgnTriangular, WhiteTriangular, WhiteRectangular };

const char* noise_name(Noise n) {
  switch (n) {
    case Noise::None: return "none";
    case Noise::IgnTriangular: return "ign-tpdf";
    case Noise::WhiteTriangular: return "white-tpdf";
    case Noise::WhiteRectangular: return "white-rpdf";
  }
  return "?";
}

// pcg2d (Jarzynski and Olano, "Hash Functions for GPU Rendering", 2020): two 32-bit words a pixel.
void pcg2d(u32& x, u32& y) {
  x = x * 1664525u + 1013904223u;
  y = y * 1664525u + 1013904223u;
  x += y * 1664525u;
  y += x * 1664525u;
  x ^= x >> 16;
  y ^= y >> 16;
  x += y * 1664525u;
  y += x * 1664525u;
  x ^= x >> 16;
  y ^= y >> 16;
}

// In codes, at most one either way.
f32 noise_at(Noise n, u32 x, u32 y) {
  u32 a = x;
  u32 b = y;
  switch (n) {
    case Noise::None: return 0.0f;
    case Noise::IgnTriangular: return gfx::display_dither(x, y);
    case Noise::WhiteTriangular: {
      pcg2d(a, b);
      const i32 sum = static_cast<i32>(a >> 9) + static_cast<i32>(b >> 9) - (1 << 23);
      return static_cast<f32>(sum) * (1.0f / 8388608.0f);
    }
    case Noise::WhiteRectangular:
      pcg2d(a, b);
      return static_cast<f32>(a >> 8) * (1.0f / 16777216.0f) - 0.5f;
  }
  return 0.0f;
}

// The float picture as a target of `steps` steps is handed it, with a candidate's noise and before
// the store rounds it, in codes.
Vector<f32> scaled(const Picture& f, u32 steps, Noise noise) {
  Vector<f32> out(f.rgb.size());
  for (u32 y = 0; y < f.height; ++y) {
    for (u32 x = 0; x < f.width; ++x) {
      const f32 n = noise_at(noise, x, y);
      for (u32 c = 0; c < 3; ++c) {
        const u32 i = 3 * (y * f.width + x) + c;
        const f32 v = std::clamp(f.rgb[i], 0.0f, 1.0f);
        f32 dithered = v;
        if (noise == Noise::IgnTriangular) {
          dithered = gfx::display_dithered(v, x, y, steps);
        } else if (noise != Noise::None) {
          const f32 s = static_cast<f32>(steps);
          const f32 room = std::min({1.0f, v * s, (1.0f - v) * s});
          dithered = v + n * room / s;
        }
        out[i] = std::clamp(dithered, 0.0f, 1.0f) * static_cast<f32>(steps);
      }
    }
  }
  return out;
}

// Those values rounded to the nearest code: the CPU's quantization.
Vector<f32> rounded(const Vector<f32>& scaled_codes) {
  Vector<f32> out(scaled_codes.size());
  for (u32 i = 0; i < scaled_codes.size(); ++i)
    out[i] = std::floor(scaled_codes[i] + 0.5f);
  return out;
}

Vector<f32> quantize(const Picture& f, u32 steps, Noise noise) {
  return rounded(scaled(f, steps, noise));
}

// Pixels whose float is smooth sky: nothing covers it or its four neighbours, and in every channel
// it is under one 8-bit code a pixel from them (where a band can be a pixel wide or more at all)
// and its curvature is under a quarter of one (no star, no disc's edge, no table's seam).
Vector<u8> smooth_mask(const Picture& f) {
  constexpr f32 k_slope = 1.0f / 255.0f;
  constexpr f32 k_curve = 0.25f / 255.0f;
  Vector<u8> mask(f.width * f.height, 0);
  for (u32 y = 1; y + 1 < f.height; ++y) {
    for (u32 x = 1; x + 1 < f.width; ++x) {
      const u32 i = y * f.width + x;
      if (!f.sky[i] || !f.sky[i - 1] || !f.sky[i + 1] || !f.sky[i - f.width] ||
          !f.sky[i + f.width]) {
        continue;
      }
      bool ok = true;
      for (u32 c = 0; c < 3 && ok; ++c) {
        const f32 v = f.rgb[3 * i + c];
        const f32 up = f.rgb[3 * (i - f.width) + c];
        const f32 down = f.rgb[3 * (i + f.width) + c];
        const f32 left = f.rgb[3 * (i - 1) + c];
        const f32 right = f.rgb[3 * (i + 1) + c];
        ok = std::fabs(down - v) <= k_slope && std::fabs(v - up) <= k_slope &&
             std::fabs(right - v) <= k_slope && std::fabs(v - left) <= k_slope &&
             std::fabs(down - 2.0f * v + up) <= k_curve &&
             std::fabs(right - 2.0f * v + left) <= k_curve;
      }
      mask[i] = ok ? 1u : 0u;
    }
  }
  return mask;
}

struct Bands {
  u64 pixels = 0;  // smooth pixel-channels measured
  u32 widest = 0;  // the widest run of one code between two one-code steps, pixels
  // Where that run is, its code, and how far the float under it moves, codes.
  u32 widest_x = 0;
  u32 widest_y = 0;
  u32 widest_channel = 0;
  f32 widest_code = 0.0f;
  f32 widest_span = 0.0f;
  u64 steps = 0;       // one-code steps between neighbours along the lines
  u64 coarse = 0;      // steps of two codes or more where the float moved under one code
  f64 grain = 0.0;     // RMS of code less float, codes
  f64 residual = 0.0;  // RMS over 8 x 8 blocks of the mean of code less float, codes
  u64 blocks = 0;
  f64 distinct = 0.0;  // the share of neighbours along the lines whose floats differ
};

// The instrument: `codes` against the float picture `f` they were quantized from, at `steps`.
Bands measure(const Picture& f, const Vector<f32>& codes, u32 steps, const Vector<u8>& mask) {
  Bands out;
  const f32 s = static_cast<f32>(steps);
  f64 grain = 0.0;
  u64 pairs = 0;
  u64 differ = 0;
  for (u32 x = 0; x < f.width; ++x) {
    u32 y = 0;
    while (y < f.height) {
      if (!mask[y * f.width + x]) {
        ++y;
        continue;
      }
      u32 end = y;
      while (end < f.height && mask[end * f.width + x])
        ++end;
      // [y, end) is one segment of smooth sky down this column.
      for (u32 c = 0; c < 3; ++c) {
        const auto q = [&](u32 row) { return codes[3 * (row * f.width + x) + c]; };
        const auto e = [&](u32 row) { return f.rgb[3 * (row * f.width + x) + c]; };
        for (u32 r = y; r < end; ++r) {
          const f64 d = static_cast<f64>(q(r)) - static_cast<f64>(e(r) * s);
          grain += d * d;
          ++out.pixels;
          if (r + 1 < end) {
            ++pairs;
            if (e(r + 1) != e(r)) ++differ;
            const f32 dq = std::fabs(q(r + 1) - q(r));
            if (dq == 1.0f) ++out.steps;
            if (dq >= 2.0f && std::fabs(e(r + 1) - e(r)) * s < 1.0f) ++out.coarse;
          }
        }
        // Runs of one code with a one-code step at each end, inside the segment. A run of the first
        // or the last code is the clip at black or white (the sky round the sun at its shoulder's
        // end), not a band, and is left out.
        u32 a = y;
        while (a < end) {
          u32 b = a;
          while (b + 1 < end && q(b + 1) == q(a))
            ++b;
          const bool bounded = a > y && b + 1 < end && std::fabs(q(a - 1) - q(a)) == 1.0f &&
                               std::fabs(q(b + 1) - q(b)) == 1.0f && q(a) > 0.0f && q(a) < s;
          if (bounded && b - a + 1 > out.widest) {
            out.widest = b - a + 1;
            out.widest_x = x;
            out.widest_y = a;
            out.widest_channel = c;
            out.widest_code = q(a);
            f32 lo = e(a);
            f32 hi = e(a);
            for (u32 r = a; r <= b; ++r) {
              lo = std::min(lo, e(r));
              hi = std::max(hi, e(r));
            }
            out.widest_span = (hi - lo) * s;
          }
          a = b + 1;
        }
      }
      y = end;
    }
  }
  // What survives an 8 x 8 average: the blocks every pixel of which is smooth sky.
  f64 residual = 0.0;
  for (u32 by = 0; by + 8 <= f.height; by += 8) {
    for (u32 bx = 0; bx + 8 <= f.width; bx += 8) {
      bool whole = true;
      for (u32 y = by; y < by + 8 && whole; ++y)
        for (u32 x = bx; x < bx + 8 && whole; ++x)
          whole = mask[y * f.width + x] != 0;
      if (!whole) continue;
      for (u32 c = 0; c < 3; ++c) {
        f64 sum = 0.0;
        for (u32 y = by; y < by + 8; ++y) {
          for (u32 x = bx; x < bx + 8; ++x) {
            const u32 i = 3 * (y * f.width + x) + c;
            sum += static_cast<f64>(codes[i]) - static_cast<f64>(f.rgb[i] * s);
          }
        }
        const f64 mean = sum / 64.0;
        residual += mean * mean;
        ++out.blocks;
      }
    }
  }
  out.grain = out.pixels > 0 ? std::sqrt(grain / static_cast<f64>(out.pixels)) : 0.0;
  out.residual = out.blocks > 0 ? std::sqrt(residual / static_cast<f64>(out.blocks)) : 0.0;
  out.distinct = pairs > 0 ? static_cast<f64>(differ) / static_cast<f64>(pairs) : 0.0;
  return out;
}

// How far a captured picture is from the CPU's quantization of the float one: pixel-channels that
// differ at all, by more than one code, and — for those that differ — how far the value the store
// was handed lay from the half-code edge between the two codes, in codes. The store resolves that
// edge to within a few hundredths of a code, not to the last bit (measured: 3% of pixel-channels
// land on the other code, every one of them within 0.03 of the edge).
struct Agreement {
  u64 compared = 0;
  u64 differ = 0;
  u64 over_one = 0;
  u64 above = 0;    // the captured code is the higher of the two
  f32 edge = 0.0f;  // the farthest from the edge any differing value lay
  f64 bias = 0.0;   // the mean of captured less rounded, codes
};

Agreement agree(const Picture& captured, const Vector<f32>& scaled_codes) {
  Agreement out;
  f64 sum = 0.0;
  for (u32 i = 0; i < captured.width * captured.height; ++i) {
    if (!captured.sky[i]) continue;
    for (u32 c = 0; c < 3; ++c) {
      const f32 x = scaled_codes[3 * i + c];
      const f32 signed_d = captured.rgb[3 * i + c] - std::floor(x + 0.5f);
      const f32 d = std::fabs(signed_d);
      ++out.compared;
      sum += static_cast<f64>(signed_d);
      if (d > 1.0f) ++out.over_one;
      if (d > 0.0f) {
        ++out.differ;
        if (signed_d > 0.0f) ++out.above;
        out.edge = std::max(out.edge, std::fabs(x - std::floor(x) - 0.5f));
      }
    }
  }
  out.bias = out.compared > 0 ? sum / static_cast<f64>(out.compared) : 0.0;
  return out;
}

// The most distinct codes of one channel down one column of uncovered pixels.
u32 levels_on_a_line(const Picture& p) {
  u32 best = 0;
  Vector<u8> seen(p.steps + 1);
  for (u32 c = 0; c < 3; ++c) {
    for (u32 x = 0; x < p.width; ++x) {
      std::fill(seen.begin(), seen.end(), u8{0});
      u32 n = 0;
      for (u32 y = 0; y < p.height; ++y) {
        if (!p.sky[y * p.width + x]) continue;
        const u32 code = static_cast<u32>(p.rgb[3 * (y * p.width + x) + c]);
        if (code <= p.steps && !seen[code]) {
          seen[code] = 1;
          ++n;
        }
      }
      best = std::max(best, n);
    }
  }
  return best;
}

// The hours whose sky gradient is slowest on the erg's calendar (the sweep below found them): the
// first quarter hour after noon with the sun three degrees under the horizon, the first after
// midnight with it there again, and the first night hour with the sun well down and the moon well
// up. Each is looked at towards its light's azimuth, the horizon low in the frame.
struct Hour {
  const char* name;
  f64 time_s;
  Vec3 light;
};

Vector<Hour> slow_hours(const scene_gen::SkyProvider& provider) {
  Vector<Hour> out;
  scene_gen::SkyState state;
  const f32 k_twilight = -std::sin(3.0f * 3.14159265f / 180.0f);
  for (f64 t = 12.0 * 3600.0; t < 24.0 * 3600.0; t += 900.0) {
    provider.state(t, state);
    if (state.sun.y < k_twilight) {
      out.push_back({"dusk", t, state.sun});
      break;
    }
  }
  for (f64 t = 0.0; t < 12.0 * 3600.0; t += 900.0) {
    provider.state(t, state);
    if (state.sun.y > k_twilight) {
      out.push_back({"dawn", t, state.sun});
      break;
    }
  }
  for (f64 t = 19.0 * 3600.0; t < 30.0 * 3600.0; t += 900.0) {
    provider.state(t, state);
    if (state.sun.y < -0.35f && state.moon.y > 0.4f) {
      out.push_back({"moonlit night", t, state.moon});
      break;
    }
  }
  return out;
}

// A frame looking towards `light`'s azimuth, or away from it, about 24 degrees up so the horizon is
// low in the frame.
FrameDesc frame_towards(Vec3 light, f64 time_s, f32 fov_deg, bool away = false) {
  const f32 sign = away ? -1.0f : 1.0f;
  Vec3 towards = normalize(Vec3{sign * light.x, 0.0f, sign * light.z});
  towards = normalize(towards + Vec3{0.0f, 0.45f, 0.0f});  // about 24 degrees up
  FrameDesc frame;
  frame.camera = aimed(Vec3{0.0f, 3.0f, 0.0f}, towards, fov_deg);
  frame.sun_time_s = time_s;
  return frame;
}

constexpr u32 k_width = 640;
constexpr u32 k_height = 360;

}  // namespace

TEST_CASE("banding: the sky's bands are the final quantizer's, and the dither breaks them") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const scene_gen::SkyProvider provider = make_earth(erg_sky());
  const Vector<Hour> hours = slow_hours(provider);
  REQUIRE(hours.size() == 3);

  // The float target holds the encoded value before any quantizer; it has no code steps, so it is
  // never dithered, whatever the setting says.
  Rig exact;
  REQUIRE_MESSAGE(exact.build(gpu.device, gfx::Format::R32G32B32A32Sfloat, true, k_width, k_height),
                  exact.error);
  struct Target {
    const char* name;
    gfx::Format format;
    bool dither;
    Rig rig;
  };
  Target targets[] = {{"8-bit", gfx::Format::R8G8B8A8Unorm, false, {}},
                      {"8-bit dithered", gfx::Format::R8G8B8A8Unorm, true, {}},
                      {"10-bit", gfx::Format::A2B10G10R10Unorm, false, {}},
                      {"10-bit dithered", gfx::Format::A2B10G10R10Unorm, true, {}}};
  for (Target& t : targets) {
    REQUIRE_MESSAGE(t.rig.build(gpu.device, t.format, t.dither, k_width, k_height), t.rig.error);
  }

  for (const Hour& hour : hours) {
    for (const bool away : {false, true}) {
      const std::string name =
          std::string(hour.name) + (away ? ", away from its light" : ", towards its light");
      CAPTURE(name);
      const FrameDesc frame = frame_towards(hour.light, hour.time_s, 60.0f, away);
      Picture f;
      shoot(gpu.device, exact, frame, f);
      REQUIRE(f.steps == 0);
      const Vector<u8> mask = smooth_mask(f);
      u64 smooth = 0;
      for (const u8 m : mask)
        smooth += m;
      REQUIRE(smooth > k_width * k_height / 4);

      Bands plain[2];  // undithered, at 8 and at 10 bits
      for (Target& t : targets) {
        const std::string target = t.name;
        CAPTURE(target);
        Picture p;
        shoot(gpu.device, t.rig, frame, p);
        const u32 steps = p.steps;
        REQUIRE((steps == 255 || steps == 1023));
        const u32 depth = steps == 255 ? 0 : 1;
        // The picture is the float rounded — with the CPU mirror's noise where the dither is on —
        // except where the value lay within a few hundredths of a code of the edge between two,
        // which the store resolves its own way.
        const Agreement a =
            agree(p, scaled(f, steps, t.dither ? Noise::IgnTriangular : Noise::None));
        const Bands b = measure(f, p.rgb, steps, mask);
        MESSAGE(name << ", " << target << ": " << b.pixels << " smooth pixel-channels, widest band "
                     << b.widest << " px (its float spans " << b.widest_span << " codes), "
                     << b.steps << " one-code steps, " << b.coarse << " coarser, grain " << b.grain
                     << " codes RMS, 8x8 residual " << b.residual << " codes RMS over " << b.blocks
                     << " blocks; floats distinct " << b.distinct << "; against the float "
                     << "rounded: " << a.differ << " of " << a.compared << " differ (" << a.above
                     << " above), " << a.over_one << " by more than one, all within " << a.edge
                     << " of a code's edge; mean " << a.bias << " codes");
        CHECK(a.over_one == 0);
        CHECK(a.edge < 0.08f);
        // Nothing upstream is quantized: the float under the bands holds a value of its own at
        // nearly every pixel.
        CHECK(b.distinct > 0.99);
        if (!t.dither) {
          plain[depth] = b;
          // No step is coarser than one code where the float moved less than one, and the float
          // under the widest band moves no more than the one code the band is (and the store's
          // slop at each end): every band is a step of one code at the final quantizer.
          CHECK(b.coarse == 0);
          CHECK(b.widest_span <= 1.15f);
          // The defect the instrument exists to see: bands a dozen pixels wide and more at 8 bits.
          if (steps == 255) CHECK(b.widest >= 12);
        } else {
          // Dithered: the grain is the triangular dither's half a code; what an 8 x 8 average
          // leaves is under 0.06 of a code, below the undithered picture's wherever its bands are
          // wider than the average; and where the bands were wide no run of one code is half as
          // wide.
          CHECK(b.grain > 0.4);
          CHECK(b.grain < 0.6);
          CHECK(b.residual < 0.06);
          if (plain[depth].widest >= 32) CHECK(b.residual < plain[depth].residual);
          if (plain[depth].widest >= 48) CHECK(2 * b.widest <= plain[depth].widest);
        }
      }
    }
  }
}

TEST_CASE("banding: the dithered picture is the same bytes twice, and 10 bits hold more levels") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const scene_gen::SkyProvider provider = make_earth(erg_sky());
  const Vector<Hour> hours = slow_hours(provider);
  REQUIRE(!hours.empty());
  const FrameDesc frame = frame_towards(hours[0].light, hours[0].time_s, 60.0f);
  Rig eight;
  REQUIRE_MESSAGE(eight.build(gpu.device, gfx::Format::R8G8B8A8Unorm, true, k_width, k_height),
                  eight.error);
  Rig ten;
  REQUIRE_MESSAGE(ten.build(gpu.device, gfx::Format::A2B10G10R10Unorm, true, k_width, k_height),
                  ten.error);
  Picture a;
  Picture b;
  shoot(gpu.device, eight, frame, a);
  shoot(gpu.device, eight, frame, b);
  CHECK(a.rgb == b.rgb);
  Picture c;
  Picture d;
  shoot(gpu.device, ten, frame, c);
  shoot(gpu.device, ten, frame, d);
  CHECK(c.rgb == d.rgb);
  const u32 levels8 = levels_on_a_line(a);
  const u32 levels10 = levels_on_a_line(c);
  MESSAGE("the most codes of one channel down one column of sky: " << levels8 << " at 8 bits, "
                                                                   << levels10 << " at 10");
  CHECK(levels8 <= 256);
  CHECK(levels10 > 256);
}

// The measurement behind the choices (docs/experiments/sky-banding-2026-10-04.md): which hours have
// the slowest gradient, and what each candidate noise leaves of the bands and adds as grain, at 8
// and 10 bits, all from the float target quantized on the CPU. Skipped by default; run it with
// `engine_renderer_tests -tc="banding: the sweep*" --no-skip`.
TEST_CASE("banding: the sweep of the day and of the candidate noises" * doctest::skip()) {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const scene_gen::SkyProvider provider = make_earth(erg_sky());
  Rig exact;
  REQUIRE_MESSAGE(
      exact.build(gpu.device, gfx::Format::R32G32B32A32Sfloat, false, k_width, k_height),
      exact.error);
  // The day, a quarter hour at a time, towards the sun and towards the moon: the widest 8-bit band.
  scene_gen::SkyState state;
  for (f64 t = 0.0; t < 24.0 * 3600.0; t += 900.0) {
    provider.state(t, state);
    for (u32 towards_moon = 0; towards_moon < 2; ++towards_moon) {
      const Vec3 light = towards_moon != 0 ? state.moon : state.sun;
      const FrameDesc frame = frame_towards(light, t, 60.0f);
      Picture f;
      shoot(gpu.device, exact, frame, f);
      const Vector<u8> mask = smooth_mask(f);
      const Bands b8 = measure(f, quantize(f, 255, Noise::None), 255, mask);
      const Bands b10 = measure(f, quantize(f, 1023, Noise::None), 1023, mask);
      const std::string towards = towards_moon != 0 ? "moon" : "sun";
      MESSAGE("sweep " << t / 3600.0 << " h, towards the " << towards << ", sun elevation "
                       << std::asin(std::clamp(state.sun.y, -1.0f, 1.0f)) * 57.29578f
                       << " deg: widest band " << b8.widest << " px at 8 bits (code "
                       << b8.widest_code << " ch " << b8.widest_channel << " at " << b8.widest_x
                       << "," << b8.widest_y << ", float spans " << b8.widest_span << "), "
                       << b10.widest << " at 10 (code " << b10.widest_code << " ch "
                       << b10.widest_channel << " at " << b10.widest_x << "," << b10.widest_y
                       << ", float spans " << b10.widest_span << "); 8x8 residual " << b8.residual
                       << " / " << b10.residual << " codes; floats distinct " << b8.distinct << "; "
                       << b8.pixels / 3 << " smooth pixels");
    }
  }
  // The candidates at the slow hours.
  for (const Hour& hour : slow_hours(provider)) {
    for (const bool away : {false, true}) {
      const FrameDesc frame = frame_towards(hour.light, hour.time_s, 60.0f, away);
      Picture f;
      shoot(gpu.device, exact, frame, f);
      const Vector<u8> mask = smooth_mask(f);
      for (const u32 steps : {255u, 1023u}) {
        for (const Noise n :
             {Noise::None, Noise::IgnTriangular, Noise::WhiteTriangular, Noise::WhiteRectangular}) {
          const Bands b = measure(f, quantize(f, steps, n), steps, mask);
          const std::string hour_name = std::string(hour.name) + (away ? " away" : " towards");
          const std::string noise = noise_name(n);
          MESSAGE("candidate " << hour_name << " at " << hour.time_s / 3600.0 << " h, "
                               << (steps == 255 ? 8 : 10) << " bits, " << noise << ": widest band "
                               << b.widest << " px, " << b.steps << " one-code steps, grain "
                               << b.grain << " codes RMS, 8x8 residual " << b.residual
                               << " codes RMS, " << b.blocks << " blocks");
        }
      }
    }
  }
}
