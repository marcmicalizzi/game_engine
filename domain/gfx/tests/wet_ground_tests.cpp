// Wet ground is a material (roadmap S99; shaders/wet_ground.slang; docs/subsystems/terrain.md,
// "Wet ground"): the shader's response against its double mirror (wet_reference.h) at 1 of 255
// over albedo x saturation x porosity x film; s = 0 the dry sample's bits; the roughness curve
// monotone and bounded; the fringe's curve.

#include "wet_reference.h"

#include <core/math/math.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/device.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/resources.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <shaders/terrain_material_probe.spv.h>
#include <string>

using namespace engine;
namespace wr = engine::gfx::wet_reference;

TEST_CASE("gfx: the wet ground's response agrees with its double reference") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("no device: " << error);
    return;
  }
  const auto module =
      gfx::create_shader_module(device, shaders::k_terrain_material_probe_spirv,
                                shaders::k_terrain_material_probe_spirv_size, &error);
  REQUIRE_MESSAGE(module.valid(), error);
  gfx::ComputePipeline pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "wet_main", {}, 16, pipeline, &error),
                  error);
  constexpr u32 k_count = 17 * 11 * 6 * 3;  // 3,366
  constexpr u32 k_groups = (k_count + 63) / 64;
  gfx::BufferResource output;
  REQUIRE_MESSAGE(
      gfx::create_buffer(device, k_groups * 64 * 3 * sizeof(Vec4),
                         gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress, true,
                         output, &error),
      error);
  struct Params {
    u64 output;
    u32 count;
    u32 pad;
  };
  const Params params{output.address, k_count, 0};
  REQUIRE_MESSAGE(gfx::submit_immediate(
                      device,
                      [&](gfx::CommandList cb) {
                        cb.bind_pipeline(gfx::BindPoint::Compute, pipeline.pipeline);
                        cb.push_constants(pipeline.layout, gfx::ShaderStage::Compute, 0,
                                          sizeof(params), &params);
                        cb.dispatch(k_groups, 1, 1);
                      },
                      &error),
                  error);
  const auto* v = static_cast<const Vec4*>(output.mapped);
  f64 worst_albedo = 0.0, worst_roughness = 0.0, worst_film = 0.0, worst_fringe = 0.0;
  u32 not_identical = 0, darker = 0;
  for (u32 i = 0; i < k_count; ++i) {
    const f64 a = static_cast<f64>(i % 17u) / 16.0;
    const f64 s = static_cast<f64>((i / 17u) % 11u) / 10.0;
    const f64 porosity = static_cast<f64>((i / 187u) % 6u) / 5.0;
    const f64 film = static_cast<f64>((i / 1122u) % 3u) / 2.0;
    // The dry sample as the probe made it, in float.
    const f32 af = static_cast<f32>(i % 17u) / 16.0f;
    const f64 dry[3] = {static_cast<f64>(af), static_cast<f64>(0.5f * af),
                        static_cast<f64>(0.1f + 0.8f * af * af)};
    const f64 r = static_cast<f64>(0.04f + static_cast<f32>(i % 13u) * 0.075f);
    const f64 got[3] = {static_cast<f64>(v[3 * i].x), static_cast<f64>(v[3 * i].y),
                        static_cast<f64>(v[3 * i].z)};
    for (u32 c = 0; c < 3; ++c) {
      const f64 want = s > 0.0 ? wr::albedo(dry[c], s, porosity) : dry[c];
      worst_albedo = std::max(worst_albedo, std::abs(got[c] - want));
      if (got[c] < dry[c]) ++darker;
    }
    const f64 want_r = s > 0.0 ? wr::roughness(r, s, film) : r;
    worst_roughness = std::max(worst_roughness, std::abs(static_cast<f64>(v[3 * i].w) - want_r));
    worst_film = std::max(worst_film, std::abs(static_cast<f64>(v[3 * i + 1].x) - wr::film(s, film)));
    const f64 h = static_cast<f64>((static_cast<f32>(i % 23u) - 3.0f) * 0.05f);
    const f64 fm = static_cast<f64>(0.025f + static_cast<f32>(i % 5u) * 0.25f);
    worst_fringe = std::max(worst_fringe, std::abs(static_cast<f64>(v[3 * i + 1].y) - wr::fringe(h, fm)));
    if (v[3 * i + 2].x != 1.0f) ++not_identical;
    (void)a;
  }
  MESSAGE("wet ground vs double: albedo " << worst_albedo << ", roughness " << worst_roughness
                                          << ", film " << worst_film << ", fringe " << worst_fringe
                                          << "; " << darker << " channels darkened");
  constexpr f64 k_one_of_255 = 1.0 / 255.0;
  CHECK(worst_albedo < k_one_of_255);
  CHECK(worst_roughness < k_one_of_255);
  CHECK(worst_film < k_one_of_255);
  CHECK(worst_fringe < k_one_of_255);
  CHECK(not_identical == 0u);  // s = 0 is the dry sample's bits
  CHECK(darker > 0u);
  gfx::destroy_buffer(device, output);
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  device.destroy();
}

TEST_CASE("gfx: the wet ground's model is monotone and bounded") {
  // Roughness: never above its own, never below the smaller of its own and the film's, and
  // non-increasing in the saturation and in the film.
  for (f64 r = 0.02; r <= 1.0; r += 0.07) {
    for (f64 f = 0.0; f <= 1.0; f += 0.25) {
      f64 last = r;
      for (f64 s = 0.0; s <= 1.0001; s += 0.01) {
        const f64 w = wr::roughness(r, s, f);
        CHECK(w <= r + 1e-12);
        CHECK(w >= std::min(r * wr::k_damp_roughness, wr::k_film_roughness) - 1e-12);
        CHECK(w <= last + 1e-12);
        last = w;
        CHECK(wr::roughness(r, s, std::min(f + 0.25, 1.0)) <= w + 1e-12);
      }
    }
  }
  // Lekner and Dorf: wet is never lighter, and a substrate of little porosity darkens less.
  for (f64 a = 0.0; a <= 1.0; a += 0.05) {
    CHECK(wr::darkening(a) <= 1.0);
    CHECK(wr::albedo(a, 1.0, 0.4) <= a + 1e-12);
    CHECK(wr::albedo(a, 1.0, 0.05) >= wr::albedo(a, 1.0, 0.4) - 1e-12);  // rock darkens less
  }
  // Sand of albedo 0.4 saturated keeps 0.61 of itself (shore.md's figure); dark soil of 0.15, 0.53.
  CHECK(wr::darkening(0.4) == doctest::Approx(0.6067).epsilon(0.001));
  CHECK(wr::darkening(0.15) == doctest::Approx(0.5286).epsilon(0.001));
  // The fringe: 1 at and under the waterline, 0 at its top, falling between.
  CHECK(wr::fringe(-0.1, 0.4) == 1.0);
  CHECK(wr::fringe(0.0, 0.4) == 1.0);
  CHECK(wr::fringe(0.2, 0.4) == doctest::Approx(0.5));
  CHECK(wr::fringe(0.4, 0.4) == 0.0);
  // No film below the free water: the damp band is matte.
  CHECK(wr::film(0.8, 0.0) == 0.0);
  CHECK(wr::film(1.0, 0.0) == 1.0);
}
