#include <core/math/math.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/device.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/resources.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <shaders/terrain_material_probe.spv.h>

using namespace engine;

TEST_CASE(
    "gfx: terrain material phases retain millimetres far away and projected normals preserve "
    "slopes") {
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
  REQUIRE_MESSAGE(
      gfx::create_compute_pipeline(device, module, "probe_main", {}, 16, pipeline, &error), error);
  gfx::BufferResource output;
  constexpr u32 k_count = 64;
  REQUIRE_MESSAGE(
      gfx::create_buffer(device, k_count * 3u * sizeof(Vec4),
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
                        cb.dispatch(1, 1, 1);
                      },
                      &error),
                  error);
  const auto* values = static_cast<const Vec4*>(output.mapped);
  for (u32 i = 0; i < k_count; ++i) {
    const f32 px = static_cast<f32>(i) * 0.001f;
    const f32 py = -static_cast<f32>(i) * 0.25f;
    const f32 pz = static_cast<f32>(i) * 0.125f;
    const f64 world[] = {((i & 1) == 0 ? 6548.0 : -6548.0) * 64.0 + static_cast<f64>(63.999f) +
                             static_cast<f64>(0.00001f) + static_cast<f64>(px),
                         0.125 + static_cast<f64>(py),
                         1562500.0 * 64.0 - 0.25 + static_cast<f64>(pz)};
    const f32 got[] = {values[3 * i].x, values[3 * i].y, values[3 * i].z};
    for (u32 axis = 0; axis < 3; ++axis) {
      const f64 cycles = world[axis] / 1.5;
      const f64 expected = cycles - std::floor(cycles);
      const f64 diff = std::abs(static_cast<f64>(got[axis]) - expected);
      CHECK(std::min(diff, 1.0 - diff) < 1e-5);
    }
    CHECK(std::abs(values[3 * i + 1].w) < 1e-6f);
    CHECK(values[3 * i + 2].x == 0);
    CHECK(values[3 * i + 2].y == 0);
    CHECK(values[3 * i + 2].z == 0);
    CHECK(values[3 * i + 2].w == doctest::Approx(1.0));
  }
  gfx::destroy_buffer(device, output);
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  device.destroy();
}
