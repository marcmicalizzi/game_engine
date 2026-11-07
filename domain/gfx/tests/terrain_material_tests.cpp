#include "brdf_reference.h"
#include "terrain_hex_reference.h"

#include <core/math/math.h>
#include <domain/gfx/backend/vulkan/vulkan.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/device.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/visibility_resolve.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <numbers>
#include <shaders/terrain_mask_probe.spv.h>
#include <shaders/terrain_material_probe.spv.h>
#include <vector>

using namespace engine;

TEST_CASE("gfx: variance footprint is monotone and continuous between texture mips") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("no device: " << error);
    return;
  }
  if (!device.features().texture_compression_bc) return;
  gfx::BindlessSet bindless;
  REQUIRE(bindless.create(device, {}, &error));
  Vector<Vector<u8>> bytes;
  Vector<gfx::ImageLevelData> levels;
  bytes.resize(9);
  for (u32 level = 0; level < 9; ++level) {
    const u32 side = std::max(1u, 256u >> level);
    const u32 blocks = ((side + 3) / 4) * ((side + 3) / 4);
    bytes[level].resize(blocks * 8, 0);
    for (u32 b = 0; b < blocks; ++b) {
      bytes[level][b * 8] = static_cast<u8>(level * 8);
      bytes[level][b * 8 + 1] = static_cast<u8>(level * 8);
    }
    levels.push_back({bytes[level].data(), bytes[level].size()});
  }
  gfx::ImageResource image;
  gfx::ImageViewHandle view;
  REQUIRE(gfx::upload_image_2d_levels(
      device, 256, 256, gfx::Format::Bc4Unorm,
      std::span<const gfx::ImageLevelData>(levels.data(), levels.size()), image, &error));
  REQUIRE(gfx::create_image_view(device, image, view, &error));
  const u32 normal_slot = bindless.add_sampled_image(view, gfx::ImageLayout::ShaderReadOnly);
  REQUIRE(bindless.add_sampled_image(view, gfx::ImageLayout::ShaderReadOnly) == normal_slot + 1);
  gfx::SamplerHandle sampler;
  REQUIRE(gfx::create_mip_sampler(device, 1, sampler, &error));
  REQUIRE(bindless.add_sampler(sampler) == 0);
  const auto module =
      gfx::create_shader_module(device, shaders::k_terrain_material_probe_spirv,
                                shaders::k_terrain_material_probe_spirv_size, &error);
  REQUIRE(module.valid());
  const gfx::DescriptorSetLayoutHandle layout[] = {bindless.layout()};
  gfx::ComputePipeline pipeline;
  REQUIRE(
      gfx::create_compute_pipeline(device, module, "footprint_main", layout, 16, pipeline, &error));
  gfx::BufferResource output;
  constexpr u32 k_count = 2048;
  REQUIRE(gfx::create_buffer(device, k_count * sizeof(Vec4),
                             gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress,
                             true, output, &error));
  struct Params {
    u64 output;
    u32 count;
    u32 slot;
  };
  const Params params{output.address, k_count, normal_slot};
  REQUIRE(gfx::submit_immediate(
      device,
      [&](gfx::CommandList cb) {
        cb.bind_pipeline(gfx::BindPoint::Compute, pipeline.pipeline);
        const VkDescriptorSet set = gfx::vk::native(bindless.set());
        vkCmdBindDescriptorSets(gfx::vk::native(cb), VK_PIPELINE_BIND_POINT_COMPUTE,
                                gfx::vk::native(pipeline.layout), 0, 1, &set, 0, nullptr);
        cb.push_constants(pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(params), &params);
        cb.dispatch(k_count / 64, 1, 1);
      },
      &error));
  const auto* values = static_cast<const Vec4*>(output.mapped);
  float maximum_step = 0;
  for (u32 i = 0; i < k_count; ++i) {
    const double expected = brdf_ref::filter_roughness(
        0.5, brdf_ref::normal_variance(static_cast<double>(values[i].y), 1));
    CHECK(std::abs(static_cast<double>(values[i].x) - expected) < 2e-6);
    CHECK(values[i].z == 0.5f);  // absent section/flag is exactly the old path
    CHECK(std::abs(static_cast<double>(values[i].w) -
                   brdf_ref::terrain_filter_combination(0.0625, static_cast<double>(values[i].y),
                                                        {static_cast<double>(values[i].y), 0, 0})) <
          2e-6);
    if (i != 0) {
      const float step = values[i].x - values[i - 1].x;
      CHECK(step >= -2e-6f);
      maximum_step = std::max(maximum_step, std::abs(step));
    }
  }
  CHECK(maximum_step < 0.008f);
  MESSAGE("maximum adjacent roughness step: " << maximum_step);
  gfx::destroy_buffer(device, output);
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  bindless.destroy();
  gfx::destroy_sampler(device, sampler);
  gfx::destroy_image_view(device, view);
  gfx::destroy_image(device, image);
  device.destroy();
}

TEST_CASE("gfx: normal filtering agrees with the double reference") {
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
      gfx::create_compute_pipeline(device, module, "filter_main", {}, 16, pipeline, &error), error);
  gfx::BufferResource output;
  constexpr u32 k_count = 64;
  REQUIRE_MESSAGE(
      gfx::create_buffer(device, k_count * sizeof(Vec4),
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
    // Binary-exact inputs let the no-widening check test identity, not CPU/GPU division rounding.
    const double r = static_cast<double>(i % 8) / 8;
    const double loss = static_cast<double>(i / 8) / 8;
    const double scale = static_cast<double>(i % 3) * 0.5;
    const double v = brdf_ref::normal_variance(loss, scale);
    const double g = brdf_ref::geometric_alpha2({r * 0.1, 0, 0}, {0, loss * 0.2, 0});
    CHECK(std::abs(static_cast<double>(values[i].x) - v) < 2e-6);
    CHECK(std::abs(static_cast<double>(values[i].y) - g) < 2e-6);
    CHECK(std::abs(static_cast<double>(values[i].z) - brdf_ref::filter_roughness(r, v, g)) < 2e-6);
    CHECK(values[i].w == static_cast<float>(i % 8) / 8.0f);
  }
  gfx::destroy_buffer(device, output);
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  device.destroy();
}

TEST_CASE("gfx: filtered GGX is closer to an 8 by 8 normal-plane reference") {
  namespace ref = brdf_ref;
  // A unit-period sinusoidal normal map; centre sample is flat, 8x8 midpoint quadrature
  // integrates one unresolved period. Black dielectric isolates the sun's specular term.
  const ref::Dvec3 light{-std::cos(ref::k_pi / 12), std::sin(ref::k_pi / 12), 0};
  for (double r : {0.2, 0.5, 0.8}) {
    for (double amplitude : {0.1, 0.3, 0.6}) {
      ref::Dvec3 normals[64];
      double mean_y = 0;
      for (u32 y = 0; y < 8; ++y) {
        for (u32 x = 0; x < 8; ++x) {
          const auto n = ref::normalize({amplitude * std::sin(2 * ref::k_pi * (x + 0.5) / 8), 1,
                                         amplitude * std::sin(2 * ref::k_pi * (y + 0.5) / 8)});
          normals[y * 8 + x] = n;
          mean_y += n.y / 64;
        }
      }
      const double filtered = ref::filter_roughness(r, ref::normal_variance(1 - mean_y, 1));
      double before = 0, after = 0;
      for (int angle = 45; angle <= 89; ++angle) {
        for (int azimuth = -20; azimuth <= 20; ++azimuth) {
          const double theta = angle * ref::k_pi / 180;
          const double phi = azimuth * ref::k_pi / 180;
          ref::Surface s;
          s.view = {std::sin(theta) * std::cos(phi), std::cos(theta),
                    std::sin(theta) * std::sin(phi)};
          s.roughness = r;
          double truth = 0;
          for (const auto n : normals) {
            s.normal = n;
            truth += ref::direct(s, light, {1, 1, 1}).x / 64;
          }
          s.normal = {0, 1, 0};
          const double raw = ref::direct(s, light, {1, 1, 1}).x - truth;
          s.roughness = filtered;
          const double smooth = ref::direct(s, light, {1, 1, 1}).x - truth;
          before += raw * raw;
          after += smooth * smooth;
        }
      }
      MESSAGE("r=" << r << " amplitude=" << amplitude << " unfiltered_rmse="
                   << std::sqrt(before / 1845) << " filtered_rmse=" << std::sqrt(after / 1845)
                   << " ratio=" << std::sqrt(after / before));
      CHECK(after < before * 0.51);  // at least 28.5% lower RMSE in every fixture
    }
  }
}

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
      gfx::create_buffer(device, k_count * 8u * sizeof(Vec4),
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
  // The stochastic lattice at the same far eye (100,000 km out in z): the GPU's tiles, weights,
  // shifts and turns are the double reference's, and the blend of a smooth stand-in texture agrees
  // everywhere, edges included, since a tile leaving the triangle leaves with weight zero.
  u32 compared = 0;
  f64 worst_blend = 0, worst_field = 0;
  for (u32 i = 0; i < k_count; ++i) {
    const f32 px = static_cast<f32>(i) * 0.001f;
    const f32 py = -static_cast<f32>(i) * 0.25f;
    const f32 pz = static_cast<f32>(i) * 0.125f;
    const i32 cell[] = {(i & 1) == 0 ? 6548 : -6548, 0, 1562500};
    const f64 rest[] = {
        (static_cast<f64>(63.999f) + static_cast<f64>(0.00001f) + static_cast<f64>(px)) * 1000.0,
        (0.125 + static_cast<f64>(py)) * 1000.0, (-0.25 + static_cast<f64>(pz)) * 1000.0};
    const u32 axis = i % 3;
    const u32 a = axis == 0 ? 2 : 0;  // the plane's u: z for projection 0, x otherwise
    const u32 b = axis == 1 ? 2 : 1;  // its v: z for projection 1, y otherwise
    const f64 rotation = (i & 1) == 0 ? 0.0 : static_cast<f64>(0.7f);
    const auto hex =
        test::terrain_hex_reference(cell[a], cell[b], rest[a], rest[b], 1500.0, rotation);
    const f64 world[] = {cell[0] * 64.0 + rest[0] / 1000.0, cell[1] * 64.0 + rest[1] / 1000.0,
                         cell[2] * 64.0 + rest[2] / 1000.0};
    const f64 uv[] = {world[a] / 1.5 - std::floor(world[a] / 1.5),
                      world[b] / 1.5 - std::floor(world[b] / 1.5)};
    f64 x[3];
    for (u32 k = 0; k < 3; ++k) {
      const f64 p[] = {uv[0] + hex.shift[k][0], uv[1] + hex.shift[k][1]};
      x[k] = 0.5 + 0.25 * std::sin(2 * std::numbers::pi * (p[0] + 2 * p[1])) +
             0.15 * std::cos(2 * std::numbers::pi * (3 * p[0] - p[1]));
    }
    const Vec4* got = values + 3 * k_count + 5 * i;
    const f64 blend = test::terrain_hex_blend_reference(hex, 0.5, x);
    worst_blend = std::max(worst_blend, std::abs(blend - static_cast<f64>(got[4].x)));
    // Field by field only away from the triangle's edges, where float and double may pick
    // different (zero-weight) corners.
    const f64 edge = std::min({hex.frac[0], hex.frac[1], 1 - hex.frac[0], 1 - hex.frac[1],
                               std::abs(1 - hex.frac[0] - hex.frac[1])});
    if (edge < 1e-3) continue;
    ++compared;
    const f64 want[] = {hex.weight[0],   hex.weight[1],   hex.weight[2],   hex.gain,
                        hex.shift[0][0], hex.shift[0][1], hex.shift[1][0], hex.shift[1][1],
                        hex.shift[2][0], hex.shift[2][1], hex.turn[0][0],  hex.turn[0][1],
                        hex.turn[1][0],  hex.turn[1][1],  hex.turn[2][0],  hex.turn[2][1]};
    const auto* have = reinterpret_cast<const f32*>(got);  // four Vec4s: 16 floats in order
    f64 fields[16][2];
    for (u32 j = 0; j < 16; ++j) {
      fields[j][0] = want[j];
      fields[j][1] = static_cast<f64>(have[j]);
    }
    for (const auto& f : fields)
      worst_field = std::max(worst_field, std::abs(f[0] - f[1]));
  }
  MESSAGE("stochastic lattice, GPU against double: " << compared << " points field by field, worst "
                                                     << worst_field << "; worst blend "
                                                     << worst_blend);
  CHECK(compared > k_count / 2);
  CHECK(worst_field < 1e-4);
  CHECK(worst_blend < 1e-4);
  gfx::destroy_buffer(device, output);
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  device.destroy();
}

// ---- the mask level's sampler
// ---------------------------------------------------------------------

namespace {
// A synthetic level's point value: a function of the point's place in the level's own lattice at
// its mip (relative to the level's first tile), so every tile holding an edge point holds the same
// bytes, as the builder guarantees, and the reference is bilinear over the lattice itself.
u32 probe_byte(i64 gx, i64 gz, u32 mip, u32 channel) {
  return static_cast<u32>(((37 * gx + 11 * gz + 64 * channel + 29 * mip) % 256 + 256) % 256);
}
u32 probe_wet(i64 gx, i64 gz, u32 mip) {
  return static_cast<u32>(((5 * gx + 3 * gz + mip) % 256 + 256) % 256);
}
}  // namespace

TEST_CASE("gfx: the mask sampler reads one tile's table and filters its mips at every place") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("no device: " << error);
    return;
  }
  const auto module = gfx::create_shader_module(device, shaders::k_terrain_mask_probe_spirv,
                                                shaders::k_terrain_mask_probe_spirv_size, &error);
  REQUIRE_MESSAGE(module.valid(), error);
  gfx::ComputePipeline pipeline;
  REQUIRE_MESSAGE(
      gfx::create_compute_pipeline(device, module, "mask_probe_main", {}, 32, pipeline, &error),
      error);
  // Four by three tiles of 8 cells at 50 cm, the level starting at tile (-2, -1); tile (1, 1) holds
  // nothing. Layout: scene_gen::GroundMasks (ground_masks.h), mirrored here by hand.
  constexpr u32 c = 8, mips = 4, tiles_x = 4, tiles_z = 3;
  constexpr i64 s = 500;
  u32 points = 0;
  for (u32 m = 0; m < mips; ++m)
    points += ((c >> m) + 1) * ((c >> m) + 1);
  const u32 words = 1 + points + (points + 3) / 4;
  const auto build = [&](Vector<u32>& buffer) {
    buffer.assign(tiles_x * tiles_z, 0u);
    u32 slot = 0;
    for (u32 tz = 0; tz < tiles_z; ++tz)
      for (u32 tx = 0; tx < tiles_x; ++tx) {
        if (tx == 3 && tz == 2) {
          buffer[tz * tiles_x + tx] = gfx::k_terrain_no_mask_tile;
          continue;
        }
        buffer[tz * tiles_x + tx] = slot++;
        const u32 base = buffer.size();
        for (u32 w = 0; w < words; ++w)
          buffer.push_back(0u);
        buffer[base] = (tx + 4 * tz) | 0xFFFF0000u | (7u << 8);
        u32 offset = 0;
        for (u32 m = 0; m < mips; ++m) {
          const u32 side = (c >> m) + 1;
          for (u32 b = 0; b < side; ++b)
            for (u32 a = 0; a < side; ++a) {
              const i64 gx = static_cast<i64>(tx) * (c >> m) + a;
              const i64 gz = static_cast<i64>(tz) * (c >> m) + b;
              const u32 p = offset + b * side + a;
              buffer[base + 1 + p] = probe_byte(gx, gz, m, 0) | (probe_byte(gx, gz, m, 1) << 8) |
                                     (probe_byte(gx, gz, m, 2) << 16) |
                                     (probe_byte(gx, gz, m, 3) << 24);
              buffer[base + 1 + points + p / 4] |= probe_wet(gx, gz, m) << (8 * (p % 4));
            }
          offset += side * side;
        }
      }
  };
  Vector<u32> level;
  build(level);
  gfx::BufferResource masks, record, input, output;
  struct Point {
    i32 cell[3];
    f32 level;
    f32 local[3];
    f32 pad;
  };
  // Near, round the origin, past it in negative cells, and 100,000 km out (the far record's level
  // starts there): seams approached to a millimetre from both sides, interiors, and four levels.
  Vector<Point> probe;
  struct Expect {
    u32 record;      // 0 near, 1 far
    i64 x_um, z_um;  // the place in micrometres from the level's first tile's corner
    f32 level;
  };
  Vector<Expect> expect;
  const i64 far_cell = 1562500;                     // 100,000 km
  const i64 far_tile = far_cell * 64000 / (c * s);  // the far level's first tile
  for (u32 r = 0; r < 2; ++r)
    for (const f32 lv : {0.0f, 0.5f, 1.75f, 3.0f})
      for (const i32 zi : {0, 3, 9}) {
        for (const i32 xi : {-1, 0, 1, 1999, 2000, 2001, 3999, 4000, 4001, 7999, 8000, 8001, 9001,
                             13999, 14000, 15990}) {
          Point p{};
          p.level = lv;
          // x millimetres from the level's start: the near level starts at -8 m, the far one at
          // the far cell's corner.
          const i64 x_mm = static_cast<i64>(xi);
          const i64 z_mm = static_cast<i64>(zi) * 1250 + 3;
          if (r == 0) {
            // Negative cells for the first half of the level: cell -1, local 56 m + x - 8 m.
            const i64 world_x = x_mm - 8000;
            const i64 cell = world_x < 0 ? -1 : 0;
            p.cell[0] = static_cast<i32>(cell);
            p.cell[2] = -1;
            p.local[0] = static_cast<f32>(static_cast<f64>(world_x - cell * 64000) / 1000.0);
            p.local[2] = static_cast<f32>(static_cast<f64>(z_mm - 4000 + 64000) / 1000.0);
          } else {
            p.cell[0] = static_cast<i32>(far_cell);
            p.cell[2] = static_cast<i32>(far_cell);
            p.local[0] = static_cast<f32>(static_cast<f64>(x_mm) / 1000.0);
            p.local[2] = static_cast<f32>(static_cast<f64>(z_mm) / 1000.0);
          }
          probe.push_back(p);
          // What the float32 local means, exactly, in micrometres (the reference's own reading).
          const f64 lx = static_cast<f64>(p.local[0]) * 1000.0,
                    lz = static_cast<f64>(p.local[2]) * 1000.0;
          Expect e;
          e.record = r;
          e.level = lv;
          if (r == 0) {
            e.x_um = static_cast<i64>(
                std::llround((static_cast<f64>(p.cell[0]) * 64000.0 + lx + 8000.0) * 1000.0));
            e.z_um = static_cast<i64>(std::llround((-64000.0 + lz + 4000.0) * 1000.0));
          } else {
            e.x_um = static_cast<i64>(std::llround(lx * 1000.0));
            e.z_um = static_cast<i64>(std::llround(lz * 1000.0));
          }
          expect.push_back(e);
        }
      }
  gfx::TerrainRecord near_rec;
  near_rec.flags = gfx::k_terrain_masked;
  near_rec.mask_tile_x0 = -2;
  near_rec.mask_tile_z0 = -1;
  near_rec.mask_tiles_x = tiles_x;
  near_rec.mask_tiles_z = tiles_z;
  near_rec.mask_spacing_mm = static_cast<u32>(s);
  near_rec.mask_cells = c;
  near_rec.mask_words = words;
  near_rec.mask_mips = mips;
  gfx::TerrainRecord far_rec = near_rec;
  far_rec.mask_tile_x0 = static_cast<i32>(far_tile);
  far_rec.mask_tile_z0 = static_cast<i32>(far_tile);
  const auto storage = gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
  REQUIRE(gfx::create_buffer(device, u64{level.size()} * 4, storage, true, masks, &error));
  std::memcpy(masks.mapped, level.data(), u64{level.size()} * 4);
  near_rec.mask = masks.address;
  far_rec.mask = masks.address;
  REQUIRE(
      gfx::create_buffer(device, 2 * sizeof(gfx::TerrainRecord), storage, true, record, &error));
  std::memcpy(record.mapped, &near_rec, sizeof(near_rec));
  std::memcpy(static_cast<u8*>(record.mapped) + sizeof(near_rec), &far_rec, sizeof(far_rec));
  const u32 half = probe.size() / 2;
  REQUIRE(
      gfx::create_buffer(device, u64{probe.size()} * sizeof(Point), storage, true, input, &error));
  std::memcpy(input.mapped, probe.data(), u64{probe.size()} * sizeof(Point));
  REQUIRE(gfx::create_buffer(device, u64{probe.size()} * 2 * sizeof(Vec4), storage, true, output,
                             &error));
  struct Params {
    u64 output;
    u64 record;
    u64 points;
    u32 count;
    u32 pad;
  };
  for (u32 r = 0; r < 2; ++r) {
    const Params params{output.address + u64{r} * half * 2 * sizeof(Vec4),
                        record.address + r * sizeof(gfx::TerrainRecord),
                        input.address + u64{r} * half * sizeof(Point), half, 0};
    REQUIRE_MESSAGE(gfx::submit_immediate(
                        device,
                        [&](gfx::CommandList cb) {
                          cb.bind_pipeline(gfx::BindPoint::Compute, pipeline.pipeline);
                          cb.push_constants(pipeline.layout, gfx::ShaderStage::Compute, 0,
                                            sizeof(params), &params);
                          cb.dispatch((half + 63) / 64, 1, 1);
                        },
                        &error),
                    error);
  }
  const auto* got = static_cast<const Vec4*>(output.mapped);
  u32 valid = 0, invalid = 0;
  for (u32 i = 0; i < probe.size(); ++i) {
    const Expect& e = expect[i];
    // The tile holding the place, and whether the level has it.
    const i64 point_um = s * 1000;
    const i64 gx0 = e.x_um >= 0 ? e.x_um / point_um : -((-e.x_um + point_um - 1) / point_um);
    const i64 gz0 = e.z_um >= 0 ? e.z_um / point_um : -((-e.z_um + point_um - 1) / point_um);
    const i64 tx = gx0 >= 0 ? gx0 / c : -((-gx0 + c - 1) / c);
    const i64 tz = gz0 >= 0 ? gz0 / c : -((-gz0 + c - 1) / c);
    const bool inside = tx >= 0 && tz >= 0 && tx < tiles_x && tz < tiles_z && !(tx == 3 && tz == 2);
    const bool ok = got[2 * i + 1].z == 1.0f;
    CHECK(ok == inside);
    if (!inside || !ok) {
      ++invalid;
      continue;
    }
    ++valid;
    CHECK(std::bit_cast<u32>(got[2 * i + 1].y) ==
          ((static_cast<u32>(tx) + 4 * static_cast<u32>(tz)) | 0xFFFF0000u | (7u << 8)));
    const u32 m0 = std::min(static_cast<u32>(e.level), mips - 1);
    const u32 m1 = std::min(m0 + 1, mips - 1);
    const f64 blend = m0 == mips - 1 ? 0.0 : static_cast<f64>(e.level) - m0;
    f64 want[5] = {};
    for (u32 k = 0; k < 2; ++k) {
      const u32 m = k == 0 ? m0 : m1;
      const f64 share = k == 0 ? 1.0 - blend : (m1 != m0 ? blend : 0.0);
      if (share == 0.0) continue;
      const f64 ux = static_cast<f64>(e.x_um) / static_cast<f64>(point_um << m);
      const f64 uz = static_cast<f64>(e.z_um) / static_cast<f64>(point_um << m);
      // Within the tile the cell never reaches its last point, so the global cell is the floor.
      const i64 cx = static_cast<i64>(std::floor(ux)), cz = static_cast<i64>(std::floor(uz));
      const f64 ax = ux - cx, az = uz - cz;
      const f64 q[4] = {(1 - ax) * (1 - az), ax * (1 - az), (1 - ax) * az, ax * az};
      const i64 px[4] = {cx, cx + 1, cx, cx + 1}, pz[4] = {cz, cz, cz + 1, cz + 1};
      for (u32 j = 0; j < 4; ++j) {
        for (u32 ch = 0; ch < 4; ++ch)
          want[ch] += share * q[j] * probe_byte(px[j], pz[j], m, ch) / 255.0;
        want[4] += share * q[j] * probe_wet(px[j], pz[j], m) / 255.0;
      }
    }
    const f32 have[5] = {got[2 * i].x, got[2 * i].y, got[2 * i].z, got[2 * i].w, got[2 * i + 1].x};
    for (u32 ch = 0; ch < 5; ++ch)
      CHECK(std::abs(static_cast<f64>(have[ch]) - want[ch]) < 2e-4);
  }
  MESSAGE(valid << " points read their tile, " << invalid << " past the level or its gap");
  CHECK(valid > 300);
  CHECK(invalid > 20);
  for (auto* b : {&masks, &record, &input, &output})
    gfx::destroy_buffer(device, *b);
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  device.destroy();
}

// ---- the stochastic sampler's statistics, direction and normals (R96), on its double reference
// ---------------------------------------------------------------------

namespace {
// A periodic image on the CPU, read bilinearly with wrap, as the layer's sampler reads its texture.
struct CpuImage {
  u32 size = 0;
  std::vector<f64> texels;
  f64 at(f64 u, f64 v) const {
    const f64 x = (u - std::floor(u)) * size - 0.5, y = (v - std::floor(v)) * size - 0.5;
    const f64 fx = std::floor(x), fy = std::floor(y);
    const f64 ax = x - fx, ay = y - fy;
    const i64 n = size;
    const auto texel = [&](i64 i, i64 j) {
      return texels[static_cast<size_t>(((j % n + n) % n) * n + ((i % n + n) % n))];
    };
    const i64 i = static_cast<i64>(fx), j = static_cast<i64>(fy);
    return (1 - ax) * (1 - ay) * texel(i, j) + ax * (1 - ay) * texel(i + 1, j) +
           (1 - ax) * ay * texel(i, j + 1) + ax * ay * texel(i + 1, j + 1);
  }
};

// A substrate-like image with a skewed, two-humped histogram: pebbles of 0.75 over a 0.2 floor,
// edges softened, from a few integer-frequency waves so it tiles.
CpuImage pebbles() {
  CpuImage image;
  image.size = 128;
  image.texels.resize(128 * 128);
  u32 seed = 7;
  f64 phase[6][3];
  for (auto& w : phase)
    for (f64& c : w)
      c = static_cast<f64>(test::terrain_hash(seed++) >> 8) / 16777216.0;
  for (u32 y = 0; y < 128; ++y)
    for (u32 x = 0; x < 128; ++x) {
      const f64 u = (x + 0.5) / 128, v = (y + 0.5) / 128;
      f64 n = 0;
      for (u32 k = 0; k < 6; ++k) {
        const f64 fu = 3 + std::floor(phase[k][1] * 6), fv = 3 + std::floor(phase[k][2] * 6);
        n += std::sin(2 * std::numbers::pi * (fu * u + (k % 2 == 0 ? fv : -fv) * v + phase[k][0]));
      }
      const f64 t = std::clamp((n / 6 + 0.12) * 6, 0.0, 1.0);
      image.texels[y * 128 + x] = 0.2 + 0.55 * t * t * (3 - 2 * t);
    }
  return image;
}

// One projection's read at a plane point (millimetres from the frame's cell 0), as
// `terrain_hex_sample` reads a colour channel: three tiles, the blend, the saturate.
f64 stochastic_read(const CpuImage& image, f64 u_mm, f64 v_mm, f64 pitch, f64 rotation, f64 mean,
                    f64 gamma = 7.0, bool variance = true) {
  const auto hex = test::terrain_hex_reference(0, 0, u_mm, v_mm, pitch, rotation, gamma);
  f64 x[3];
  for (u32 k = 0; k < 3; ++k)
    x[k] = image.at(u_mm / pitch + hex.shift[k][0], v_mm / pitch + hex.shift[k][1]);
  return std::clamp(test::terrain_hex_blend_reference(hex, mean, x, variance), 0.0, 1.0);
}

struct Moments {
  f64 mean = 0, variance = 0;
};
Moments moments(const std::vector<f64>& v) {
  Moments m;
  for (const f64 x : v)
    m.mean += x;
  m.mean /= static_cast<f64>(v.size());
  for (const f64 x : v)
    m.variance += (x - m.mean) * (x - m.mean);
  m.variance /= static_cast<f64>(v.size());
  return m;
}
// The 1-Wasserstein distance between two equal-sized samples: the mean gap of their sorted values.
f64 wasserstein(std::vector<f64> a, std::vector<f64> b) {
  std::sort(a.begin(), a.end());
  std::sort(b.begin(), b.end());
  f64 sum = 0;
  for (size_t i = 0; i < a.size(); ++i)
    sum += std::abs(a[i] - b[i]);
  return sum / static_cast<f64>(a.size());
}
}  // namespace

TEST_CASE("gfx: the stochastic terrain sampler keeps the texture's mean, variance and histogram") {
  // Over 260 by 260 repeats of the image, a point every 0.26 of a repeat: the tiled read of the
  // same points is the image's own statistics under the same filter. The stochastic read must
  // match its mean to 0.005 and its variance to 4%; the plain linear blends the method was chosen
  // over are measured beside it for the experiments page.
  const CpuImage image = pebbles();
  constexpr f64 k_pitch = 1500;
  constexpr u32 k_side = 1000;
  constexpr f64 k_step = 391.0;  // mm: no rational part of the pitch or the lattice
  std::vector<f64> tiled, ours, linear, sharp, burley;
  f64 image_mean = 0;
  for (const f64 t : image.texels)
    image_mean += t;
  image_mean /= static_cast<f64>(image.texels.size());
  for (u32 j = 0; j < k_side; ++j)
    for (u32 i = 0; i < k_side; ++i) {
      const f64 u = i * k_step + 0.37 * j, v = j * k_step;
      tiled.push_back(image.at(u / k_pitch, v / k_pitch));
      ours.push_back(stochastic_read(image, u, v, k_pitch, 0, image_mean));
      linear.push_back(stochastic_read(image, u, v, k_pitch, 0, image_mean, 1.0, false));
      sharp.push_back(stochastic_read(image, u, v, k_pitch, 0, image_mean, 7.0, false));
      burley.push_back(stochastic_read(image, u, v, k_pitch, 0, image_mean, 4.0));
    }
  const Moments m_tiled = moments(tiled), m_ours = moments(ours), m_linear = moments(linear),
                m_sharp = moments(sharp);
  const f64 sd = std::sqrt(m_tiled.variance);
  MESSAGE("tiled mean " << m_tiled.mean << " variance " << m_tiled.variance);
  MESSAGE("stochastic (gamma 7, variance-preserving): mean "
          << m_ours.mean << ", variance ratio " << m_ours.variance / m_tiled.variance << ", W1/sd "
          << wasserstein(tiled, ours) / sd);
  MESSAGE("linear blend (gamma 1): mean " << m_linear.mean << ", variance ratio "
                                          << m_linear.variance / m_tiled.variance << ", W1/sd "
                                          << wasserstein(tiled, linear) / sd);
  MESSAGE("linear blend (gamma 7, Mikkelsen's exponent): mean "
          << m_sharp.mean << ", variance ratio " << m_sharp.variance / m_tiled.variance
          << ", W1/sd " << wasserstein(tiled, sharp) / sd);
  MESSAGE("variance-preserving at gamma 4 (Burley's exponent): variance ratio "
          << moments(burley).variance / m_tiled.variance << ", W1/sd "
          << wasserstein(tiled, burley) / sd);
  CHECK(std::abs(m_ours.mean - m_tiled.mean) < 0.005);
  CHECK(std::abs(m_ours.variance / m_tiled.variance - 1.0) < 0.04);
  CHECK(wasserstein(tiled, ours) < wasserstein(tiled, sharp));
}

TEST_CASE("gfx: a stochastic layer without a turn keeps a directed texture's direction") {
  // Stripes across u. Offsets alone move them along u and keep them straight in v: the squared
  // gradient along v stays a small fraction of the one along u. Turns of up to 180 degrees
  // scatter them.
  CpuImage stripes;
  stripes.size = 128;
  stripes.texels.resize(128 * 128);
  for (u32 y = 0; y < 128; ++y)
    for (u32 x = 0; x < 128; ++x)
      stripes.texels[y * 128 + x] =
          0.5 + 0.4 * std::sin(2 * std::numbers::pi * 4 * (x + 0.5) / 128);
  constexpr f64 k_pitch = 2000, k_h = 2.0;  // mm
  const auto anisotropy = [&](f64 rotation) {
    f64 gu = 0, gv = 0;
    for (u32 j = 0; j < 300; ++j)
      for (u32 i = 0; i < 300; ++i) {
        const f64 u = i * 523.0 + 0.11 * j, v = j * 523.0;
        const f64 c = stochastic_read(stripes, u, v, k_pitch, rotation, 0.5);
        gu += std::pow(stochastic_read(stripes, u + k_h, v, k_pitch, rotation, 0.5) - c, 2);
        gv += std::pow(stochastic_read(stripes, u, v + k_h, k_pitch, rotation, 0.5) - c, 2);
      }
    return gv / gu;
  };
  const f64 straight = anisotropy(0), turned = anisotropy(std::numbers::pi);
  MESSAGE("gradient energy along the stripes over across them: no turn "
          << straight << ", turns to 180 degrees " << turned);
  CHECK(straight < 0.35);
  CHECK(turned > 0.75);
}

TEST_CASE("gfx: a turned stochastic tile turns its normal map's slopes with its colour") {
  // A height field and its OpenGL normal map (y up the image, so the stored slope pair is
  // (-dH/du, +dH/dv)). Inside a tile, where one weight is all, the slope the sampler reports is
  // the one the blended height itself has, at every turn: bump and colour stay on one feature.
  const f64 two_pi = 2 * std::numbers::pi;
  const auto height = [&](f64 u, f64 v) {
    return 0.5 + 0.2 * std::sin(two_pi * (u + 2 * v)) + 0.1 * std::cos(two_pi * (3 * u - v));
  };
  const auto stored = [&](f64 u, f64 v, f64 out[2]) {
    const f64 a = two_pi * (u + 2 * v), b = two_pi * (3 * u - v);
    const f64 hu = 0.2 * two_pi * std::cos(a) - 0.1 * 3 * two_pi * std::sin(b);
    const f64 hv = 0.2 * 2 * two_pi * std::cos(a) + 0.1 * two_pi * std::sin(b);
    out[0] = -hu;
    out[1] = hv;
  };
  constexpr f64 k_pitch = 1000, k_h = 0.01;  // mm
  u32 checked = 0;
  f64 worst = 0;
  for (const f64 rotation : {0.0, 0.9, 2.5}) {
    for (u32 i = 0; i < 4000; ++i) {
      const f64 u = 37.0 * i + 0.5, v = 61.0 * i + 0.25;
      const auto hex = test::terrain_hex_reference(0, 0, u, v, k_pitch, rotation);
      if (std::max({hex.weight[0], hex.weight[1], hex.weight[2]}) < 0.9999) continue;
      const auto blended = [&](f64 pu, f64 pv) {
        const auto h = test::terrain_hex_reference(0, 0, pu, pv, k_pitch, rotation);
        f64 x[3];
        for (u32 k = 0; k < 3; ++k)
          x[k] = height(pu / k_pitch + h.shift[k][0], pv / k_pitch + h.shift[k][1]);
        return test::terrain_hex_blend_reference(h, 0.5, x);
      };
      // terrain_hex_sample's slope: each tile's stored pair turned by its R, blended about zero.
      f64 slope[2] = {};
      for (u32 k = 0; k < 3; ++k) {
        f64 q[2];
        stored(u / k_pitch + hex.shift[k][0], v / k_pitch + hex.shift[k][1], q);
        const f64 c = hex.turn[k][0], s = hex.turn[k][1];
        slope[0] += hex.gain * hex.weight[k] * (c * q[0] - s * q[1]);
        slope[1] += hex.gain * hex.weight[k] * (s * q[0] + c * q[1]);
      }
      const f64 du = (blended(u + k_h, v) - blended(u - k_h, v)) / (2 * k_h / k_pitch);
      const f64 dv = (blended(u, v + k_h) - blended(u, v - k_h)) / (2 * k_h / k_pitch);
      worst = std::max({worst, std::abs(slope[0] + du), std::abs(slope[1] - dv)});
      ++checked;
    }
  }
  // The slopes are near 3; a turn the wrong way would be off by about that much.
  MESSAGE(checked << " points inside a tile; worst slope error " << worst);
  CHECK(checked > 100);
  CHECK(worst < 0.02);
}

namespace {
// R99's responses in double, from the sources' own numbers (docs/experiments/
// sand-and-snow-substrates-2026-11-02.md), so the shader's constants are held to the papers.
namespace r99 {
// Warren and Brandt (2008): ice's imaginary index at the three channels' wavelengths, the blue
// linear between its 460 and 470 nm rows.
constexpr double k_lambda[3] = {610e-9, 550e-9, 465e-9};
constexpr double k_chi[3] = {6.890e-9, 2.289e-9, 0.5 * (1.325e-10 + 1.956e-10)};
// Kokhanovsky (2021): the effective absorption length is 16 grain diameters.
constexpr double k_shape = 16.0;
double snow_albedo(int c, double grain_mm) {
  const double alpha = 4.0 * std::numbers::pi * k_chi[c] / k_lambda[c];
  return std::exp(-std::sqrt(alpha * k_shape * grain_mm * 1e-3));
}
// The lobe's weight w as the metallic word m with the diffuse kept: 0.04 (1 - m) + a m / (1 - m)
// = 0.04 w, solved here by bisection rather than by the shader's closed form.
double lobe_metallic(double a, double w) {
  if (w <= 1.0) return 0.0;
  double lo = 0.0, hi = 0.9;
  for (int k = 0; k < 200; ++k) {
    const double m = 0.5 * (lo + hi);
    (0.04 * (1.0 - m) + a * m / (1.0 - m) < 0.04 * w ? lo : hi) = m;
  }
  return 0.5 * (lo + hi);
}
// Lekner and Dorf (1988) over Angstrom: water's diffuse external reflectance 0.066 and n = 1.333.
double wet_darkening(double a) {
  const double re = 0.066, n = 1.333;
  const double ri = 1.0 - (1.0 - re) / (n * n);
  return (1.0 - re) * (1.0 - ri) / (1.0 - ri * a);
}
// Duff et al. (2017), as the shader builds it.
void basis(const double n[3], double t[3], double b[3]) {
  const double s = n[2] >= 0.0 ? 1.0 : -1.0;
  const double a = -1.0 / (s + n[2]);
  const double c = n[0] * n[1] * a;
  t[0] = 1.0 + s * n[0] * n[0] * a;
  t[1] = s * c;
  t[2] = -s * n[0];
  b[0] = c;
  b[1] = s + n[1] * n[1] * a;
  b[2] = -n[1];
}
f64 smoothstep(f64 e0, f64 e1, f64 x) {
  const f64 t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}
// The chance a pixel holds a facet at a footprint, density times min(1, 4 mm / footprint), none
// past 256 mm.
f64 glint_chance(f64 footprint, f64 density) {
  return density * std::min(1.0, 4.0 / std::max(footprint, 1.0)) *
         (1.0 - smoothstep(128.0, 256.0, footprint));
}
// A glint cell's hash: the cell's integer indices on the 2^level millimetre grid, the level, and
// the response's seed, as terrain_glint_cell builds it.
u32 glint_hash(i64 ix, i64 iz, u32 level, u32 seed) {
  return test::terrain_hash(static_cast<u32>(static_cast<u64>(ix)) ^
                            test::terrain_hash(static_cast<u32>(static_cast<u64>(iz)) ^
                                               test::terrain_hash(level ^ seed)));
}
i64 glint_axis(i32 cell, f32 local_mm, u32 level) {
  return test::terrain_floor_div(i64{cell} * 64000 + static_cast<i64>(std::floor(local_mm)),
                                 i64{1} << level);
}
f64 glint_unit(u32 h) { return static_cast<f64>(h >> 8) / 16777216.0; }
// R125's two-level glint: the finer cell floor(log2 footprint) and the coarser one above it, each
// level's chance x (1 - t) and x t of the fractional log t, x the root that makes the chance
// either draws p; the finer's facet wins where both draw.
struct Glint {
  f64 chance = 0, blend = 0, chance0 = 0, chance1 = 0, draw0 = 1, draw1 = 1;
  u32 level = 0, level_lit = 2, hash = 0;
};
Glint glint(i32 cell_x, i32 cell_z, f32 x_mm, f32 z_mm, f64 footprint, f64 density, u32 seed) {
  Glint g;
  if (density <= 0.0 || footprint >= 256.0) return g;
  const f64 lf = std::log2(std::max(footprint, 1.0));
  const f64 fl = std::floor(lf);
  g.level = static_cast<u32>(fl);
  g.blend = lf - fl;
  g.chance = glint_chance(footprint, density);
  const f64 t = g.blend;
  const f64 x = 2.0 * g.chance / (1.0 + std::sqrt(1.0 - 4.0 * t * (1.0 - t) * g.chance));
  g.chance0 = x * (1.0 - t);
  g.chance1 = x * t;
  const u32 h0 = glint_hash(glint_axis(cell_x, x_mm, g.level), glint_axis(cell_z, z_mm, g.level),
                            g.level, seed);
  const u32 hc = glint_hash(glint_axis(cell_x, x_mm, g.level + 1),
                            glint_axis(cell_z, z_mm, g.level + 1), g.level + 1, seed);
  g.draw0 = glint_unit(h0);
  g.draw1 = glint_unit(hc);
  if (g.draw0 < g.chance0) {
    g.level_lit = 0;
    g.hash = h0;
  } else if (g.draw1 < g.chance1) {
    g.level_lit = 1;
    g.hash = hc;
  }
  return g;
}
}  // namespace r99
}  // namespace

TEST_CASE("gfx: sand and snow responses agree with their double references") {
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
      gfx::create_compute_pipeline(device, module, "response_main", {}, 16, pipeline, &error),
      error);
  constexpr u32 k_count = 2048;
  gfx::BufferResource output;
  REQUIRE_MESSAGE(
      gfx::create_buffer(device, k_count * 5 * sizeof(Vec4),
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
                        cb.dispatch(k_count / 64, 1, 1);
                      },
                      &error),
                  error);
  const auto* v = static_cast<const Vec4*>(output.mapped);
  u32 lit = 0, lit_coarse = 0, unlit = 0, borderline = 0;
  f64 worst_albedo = 0, worst_lobe = 0, worst_wet = 0, worst_normal = 0, worst_chance = 0,
      worst_blend = 0;
  for (u32 i = 0; i < k_count; ++i) {
    // The snow's albedo (Kokhanovsky and Zege's spherical albedo) per channel.
    const f64 grain = static_cast<f64>(0.03f + static_cast<f32>(i % 64) * 0.078f);
    const f64 albedo[3] = {static_cast<f64>(v[5 * i].x), static_cast<f64>(v[5 * i].y),
                           static_cast<f64>(v[5 * i].z)};
    for (int c = 0; c < 3; ++c)
      worst_albedo = std::max(worst_albedo, std::abs(albedo[c] - r99::snow_albedo(c, grain)));
    // The lobe: its metallic word gives the weight's F0 and keeps the diffuse.
    const f64 a = static_cast<f64>(i % 17) / 16.0;
    const f64 w = 1.0 + static_cast<f64>(i % 7) * 0.5;
    if (a > 0.1)
      worst_lobe =
          std::max(worst_lobe, std::abs(static_cast<f64>(v[5 * i].w) - r99::lobe_metallic(a, w)));
    if (w == 1.0) CHECK(v[5 * i].w == 0.0f);  // weight 1: the dielectric exactly, no metallic
    // Wet darkening.
    const f64 wet[3] = {a, 0.5 * a, 0.1};
    const f64 got_wet[3] = {static_cast<f64>(v[5 * i + 1].x), static_cast<f64>(v[5 * i + 1].y),
                            static_cast<f64>(v[5 * i + 1].z)};
    for (int c = 0; c < 3; ++c)
      worst_wet = std::max(worst_wet, std::abs(got_wet[c] - r99::wet_darkening(wet[c])));
    // The glint: each level's draw is the integer hash of the frame's cell and millimetres, so it
    // is compared exactly; the chances and the blend to float precision (the GPU's log2 and sqrt);
    // the facet normal to the GPU's sin and cos.
    const i32 cell_x = (i & 1) == 0 ? 6548 : -6548;
    const i32 cell_z = (i & 2) == 0 ? 1562500 : -3;
    const f32 fi = static_cast<f32>(i);
    const f32 local_x = (63.999f + 0.00001f + fi * 0.0173f) * 1000.0f;
    const f32 local_z = (-0.25f + 0.0f + fi * -0.0291f) * 1000.0f;
    const f64 footprint = static_cast<f64>(0.3f + fi * 0.137f);
    const f64 density = (i % 4) == 0 ? 1.0 : static_cast<f64>(0.6f);
    const f64 alpha = static_cast<f64>(0.1f + static_cast<f32>(i % 9) * 0.07f);
    const u32 seed = (i % 2) == 0 ? 0x534E4F57u : 0x53414E44u;
    const Vec4 got_normal = v[5 * i + 2];
    const Vec4 got = v[5 * i + 3];
    const Vec4 got_levels = v[5 * i + 4];
    if (footprint >= 256.0) {
      CHECK(got_normal.w == 0.0f);
      continue;
    }
    // A point within a hundredth of a millimetre of a whole one may floor either way on a GPU
    // that fuses its multiply-add, and a footprint at a power of two may take either level on a
    // GPU's log2; those are not compared.
    const auto near_whole = [](f32 mm) {
      const f32 r = mm - std::floor(mm);
      return r < 0.01f || r > 0.99f;
    };
    const f64 lf = std::log2(std::max(footprint, 1.0));
    if (near_whole(local_x) || near_whole(local_z) ||
        (footprint > 1.0 && std::abs(lf - std::round(lf)) < 1e-5)) {
      ++borderline;
      continue;
    }
    const r99::Glint ref = r99::glint(cell_x, cell_z, local_x, local_z, footprint, density, seed);
    // The finer cell is the largest power of two at most the footprint (1 mm under a millimetre).
    const u32 size = static_cast<u32>(got.z);
    REQUIRE(std::has_single_bit(size));
    CHECK(static_cast<f64>(size) <= std::max(footprint, 1.0) * 1.001);
    CHECK(static_cast<f64>(size) > std::max(footprint, 1.0) * 0.499);
    CHECK(static_cast<u32>(std::countr_zero(size)) == ref.level);
    CHECK(static_cast<f64>(got_levels.y) == ref.draw0);
    CHECK(static_cast<f64>(got_levels.w) == ref.draw1);
    worst_chance = std::max({worst_chance, std::abs(static_cast<f64>(got.x) - ref.chance),
                             std::abs(static_cast<f64>(got_levels.x) - ref.chance0),
                             std::abs(static_cast<f64>(got_levels.z) - ref.chance1)});
    worst_blend = std::max(worst_blend, std::abs(static_cast<f64>(got.y) - ref.blend));
    if (std::abs(ref.draw0 - ref.chance0) < 1e-4 || std::abs(ref.draw1 - ref.chance1) < 1e-4) {
      ++borderline;
      continue;
    }
    CHECK(static_cast<u32>(got.w) == ref.level_lit);
    CHECK((got_normal.w == 1.0f) == (ref.level_lit != 2));
    if (ref.level_lit == 2) {
      ++unlit;
      continue;
    }
    ++lit;
    if (ref.level_lit == 1) ++lit_coarse;
    const u32 h0 = ref.hash;  // A GGX-distributed facet about the probe's normal (Walter et al.
                              // 2007's sampling).
    f64 n[3] = {static_cast<f64>(i % 3) - 1.0, 2.5, static_cast<f64>(i % 5) - 2.0};
    const f64 len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    for (f64& c : n)
      c /= len;
    const u32 h1 = test::terrain_hash(h0 ^ 0x68e31da4u);
    const u32 h2 = test::terrain_hash(h1);
    const f64 u1 = static_cast<f64>(h1 >> 8) / 16777216.0;
    const f64 u2 = static_cast<f64>(h2 >> 8) / 16777216.0;
    const f64 tan_t =
        std::min(alpha * std::sqrt(u1 / (1.0 - u1)), std::tan(25.0 * std::numbers::pi / 180.0));
    const f64 cos_t = 1.0 / std::sqrt(1.0 + tan_t * tan_t);
    const f64 sin_t = tan_t * cos_t;
    const f64 phi = (2.0 * u2 - 1.0) * std::numbers::pi;
    f64 t[3], b[3], f[3];
    r99::basis(n, t, b);
    for (int c = 0; c < 3; ++c)
      f[c] = t[c] * sin_t * std::cos(phi) + b[c] * sin_t * std::sin(phi) + n[c] * cos_t;
    const f64 fl = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    worst_normal = std::max({worst_normal, std::abs(static_cast<f64>(got_normal.x) - f[0] / fl),
                             std::abs(static_cast<f64>(got_normal.y) - f[1] / fl),
                             std::abs(static_cast<f64>(got_normal.z) - f[2] / fl)});
    CHECK(static_cast<f64>(got_normal.x) * n[0] + static_cast<f64>(got_normal.y) * n[1] +
              static_cast<f64>(got_normal.z) * n[2] >
          0.0);
  }
  MESSAGE("albedo " << worst_albedo << ", lobe " << worst_lobe << ", wet " << worst_wet
                    << ", facet normal " << worst_normal << ", glint chance " << worst_chance
                    << ", blend " << worst_blend << "; glints lit " << lit << " (" << lit_coarse
                    << " by the coarser level), unlit " << unlit << ", borderline " << borderline);
  CHECK(worst_albedo < 2e-6);
  CHECK(worst_lobe < 2e-5);
  CHECK(worst_wet < 2e-6);
  CHECK(worst_normal < 1e-3);
  CHECK(worst_chance < 1e-5);
  CHECK(worst_blend < 1e-5);
  // Every branch is exercised: lit by either level, and unlit.
  CHECK(lit > 50);
  CHECK(lit_coarse > 10);
  CHECK(unlit > 100);
  gfx::destroy_buffer(device, output);
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  device.destroy();
}

namespace {
// R125's band measure: over a square of side `side_mm` from the frame's origin, the facets per
// square metre (every facet that lights some point) and the share of the finer cells lit, by the
// two-level rule, or with `one_level` by the rule before it (one cell of the smallest power of two
// at least the footprint, its own chance p, its own hash).
struct GlintField {
  f64 facets_per_m2 = 0;
  f64 lit_share = 0;
};
GlintField glint_field(f64 footprint, f64 density, u32 seed, i64 side_mm, bool one_level) {
  GlintField out;
  const f64 area_m2 = static_cast<f64>(side_mm) * static_cast<f64>(side_mm) * 1e-6;
  const f64 p = r99::glint_chance(footprint, density);
  const f64 lf = std::log2(std::max(footprint, 1.0));
  if (one_level) {
    const u32 level = static_cast<u32>(std::ceil(lf));
    const i64 n = side_mm >> level;
    u64 count = 0;
    for (i64 iz = 0; iz < n; ++iz)
      for (i64 ix = 0; ix < n; ++ix)
        count += r99::glint_unit(r99::glint_hash(ix, iz, level, seed)) < p ? 1 : 0;
    out.facets_per_m2 = static_cast<f64>(count) / area_m2;
    out.lit_share = static_cast<f64>(count) / static_cast<f64>(n * n);
    return out;
  }
  const u32 level = static_cast<u32>(std::floor(lf));
  const f64 t = lf - std::floor(lf);
  const f64 x = 2.0 * p / (1.0 + std::sqrt(1.0 - 4.0 * t * (1.0 - t) * p));
  const i64 n0 = side_mm >> level;
  const i64 n1 = n0 / 2;
  std::vector<u8> lit0(static_cast<usize>(n0 * n0));
  u64 facets = 0, lit_points = 0;
  for (i64 iz = 0; iz < n0; ++iz)
    for (i64 ix = 0; ix < n0; ++ix) {
      const bool on = r99::glint_unit(r99::glint_hash(ix, iz, level, seed)) < x * (1.0 - t);
      lit0[static_cast<usize>(iz * n0 + ix)] = on ? 1 : 0;
      facets += on ? 1 : 0;
    }
  for (i64 iz = 0; iz < n1; ++iz)
    for (i64 ix = 0; ix < n1; ++ix) {
      u32 under = 0;
      for (i64 k = 0; k < 4; ++k)
        under += lit0[static_cast<usize>((2 * iz + k / 2) * n0 + 2 * ix + k % 2)];
      const bool on = r99::glint_unit(r99::glint_hash(ix, iz, level + 1, seed)) < x * t;
      // The finer facet wins where both draw: the coarser one lights the finer cells left dark.
      lit_points += on ? 4 : under;
      facets += on && under < 4 ? 1 : 0;
    }
  out.facets_per_m2 = static_cast<f64>(facets) / area_m2;
  out.lit_share = static_cast<f64>(lit_points) / static_cast<f64>(n0 * n0);
  return out;
}
}  // namespace

TEST_CASE("gfx: glint facets per area are continuous across a cell size's step") {
  // A footprint crossing a power of two is a curve on the ground (a screen row on flat ground, a
  // contour on a slope). Before R125 the cell doubled there and its facets re-drew: the facets per
  // area fell fourfold across the curve, and the eye read the step as a band (the owner's v26
  // walk, 2026-11-02). Two levels blended by the fractional log hand the coarser level the whole
  // chance by the step, whose cells and hash are the next octave's finer level.
  constexpr f64 k_density = 0.5;  // the snow's
  constexpr u32 k_seed = 0x534E4F57u;
  for (const f64 step : {4.0, 64.0}) {
    const i64 side = 512 * static_cast<i64>(step);
    const f64 below = step * 0.998, above = step * 1.002;
    const GlintField b = glint_field(below, k_density, k_seed, side, false);
    const GlintField a = glint_field(above, k_density, k_seed, side, false);
    const GlintField ob = glint_field(below, k_density, k_seed, side, true);
    const GlintField oa = glint_field(above, k_density, k_seed, side, true);
    MESSAGE("step " << step << " mm: facets per m^2 " << b.facets_per_m2 << " -> "
                    << a.facets_per_m2 << " (one level: " << ob.facets_per_m2 << " -> "
                    << oa.facets_per_m2 << "); lit share " << b.lit_share << " -> " << a.lit_share);
    CHECK(std::abs(a.facets_per_m2 / b.facets_per_m2 - 1.0) < 0.05);
    CHECK(std::abs(a.lit_share / b.lit_share - 1.0) < 0.05);
    // The measure sees the old rule's step: a fourfold fall.
    CHECK(ob.facets_per_m2 / oa.facets_per_m2 > 3.0);
  }
  // Midway between two levels each draws half the chance; the share of the ground lit stays p,
  // where two independent draws of p / 2 would light p - p^2 / 4.
  for (const f64 footprint : {2.0 * std::numbers::sqrt2, 32.0 * std::numbers::sqrt2}) {
    const GlintField g = glint_field(footprint, k_density, k_seed, 512 * 64, false);
    const f64 p = r99::glint_chance(footprint, k_density);
    MESSAGE("footprint " << footprint << " mm: lit share " << g.lit_share << ", p " << p);
    CHECK(std::abs(g.lit_share / p - 1.0) < 0.02);
  }
}

TEST_CASE("gfx: a glint's facet does not move with the eye") {
  // The same ground point seen from two eyes a world cell apart (and at different places inside
  // their cells) draws the same cells and the same facet at every footprint: the cells are
  // anchored to the world's integer cell and millimetres, never to the eye. Points on a 1/2048 m
  // lattice keep every sum exact in f32, as the shader's are for these eyes.
  constexpr u32 k_seed = 0x53414E44u;
  u32 compared = 0, lit = 0;
  for (u32 k = 0; k < 4096; ++k) {
    const f32 px = static_cast<f32>((k * 37u) % 8192u) / 2048.0f;
    const f32 pz = static_cast<f32>((k * 101u) % 8192u) / 2048.0f;
    // Eye A: cell (5, -2), local (60, 3.5); eye B: cell (6, -1), local (0.25, 1.0).
    const f32 ax = (60.0f + 0.0f + px) * 1000.0f, az = (3.5f + 0.0f + pz) * 1000.0f;
    const f32 bpx = px + 60.0f - 64.0f - 0.25f, bpz = pz + 3.5f - 64.0f - 1.0f;
    const f32 bx = (0.25f + 0.0f + bpx) * 1000.0f, bz = (1.0f + 0.0f + bpz) * 1000.0f;
    for (const f64 footprint : {0.7, 1.5, 3.9, 4.1, 11.0, 63.0, 65.0, 200.0}) {
      const r99::Glint a = r99::glint(5, -2, ax, az, footprint, 0.5, k_seed);
      const r99::Glint b = r99::glint(6, -1, bx, bz, footprint, 0.5, k_seed);
      CHECK(a.draw0 == b.draw0);
      CHECK(a.draw1 == b.draw1);
      CHECK(a.level_lit == b.level_lit);
      CHECK(a.hash == b.hash);
      ++compared;
      lit += a.level_lit != 2 ? 1 : 0;
    }
  }
  MESSAGE(compared << " points compared, " << lit << " lit");
  CHECK(lit > 100);
}

TEST_CASE("gfx: the far terrain's read rules are bands, and exactly off under a cell") {
  // R136 (renderer.md, "What a far pixel reads"). A plane tilted from flat to vertical a hundredth
  // of a degree at a time: under a cell the projections keep the 1/128 step they had (a share
  // jumps by about 1/128 where it crosses it); past two cells the floor is a band from 1/32 to 1/16
  // and no share jumps. The ramps that pick one tile, the maps' means and a layer's weight are
  // their double mirrors, exactly 0 (or the weight) on the near side.
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
      gfx::create_compute_pipeline(device, module, "reads_main", {}, 16, pipeline, &error), error);
  constexpr u32 k_tilts = 9001;  // 0 to 90 degrees in 0.01 degree steps
  constexpr u32 k_total = k_tilts + 512;
  gfx::BufferResource output;
  REQUIRE_MESSAGE(
      gfx::create_buffer(device, k_total * sizeof(Vec4),
                         gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress, true,
                         output, &error),
      error);
  struct Params {
    u64 output;
    u32 count;
    u32 pad;
  };
  const Params params{output.address, k_tilts, 0};
  REQUIRE_MESSAGE(gfx::submit_immediate(
                      device,
                      [&](gfx::CommandList cb) {
                        cb.bind_pipeline(gfx::BindPoint::Compute, pipeline.pipeline);
                        cb.push_constants(pipeline.layout, gfx::ShaderStage::Compute, 0,
                                          sizeof(params), &params);
                        cb.dispatch((k_total + 63) / 64, 1, 1);
                      },
                      &error),
                  error);
  const auto* v = static_cast<const Vec4*>(output.mapped);
  const auto smooth = [](f64 a, f64 b, f64 x) {
    const f64 t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
  };
  // The far rule's double mirror: a share under 1/32 is dropped, one over 1/16 kept, a band
  // between, then renormalized.
  const auto band = [](f64 a) { return a * std::clamp((a - 1.0 / 32) / (1.0 / 32), 0.0, 1.0); };
  f64 far_jump = 0, near_jump = 0, worst_far = 0;
  u32 dropped_far = 0, dropped_near = 0;
  for (u32 i = 0; i < k_tilts; ++i) {
    const f64 theta = std::numbers::pi / 2 * i / (k_tilts - 1);
    const f64 x4 = std::pow(std::sin(theta), 4), y4 = std::pow(std::cos(theta), 4);
    const f64 ax = x4 / (x4 + y4), ay = y4 / (x4 + y4);
    const f64 kx = band(ax), ky = band(ay);
    worst_far = std::max({worst_far, std::abs(static_cast<f64>(v[i].x) - kx / (kx + ky)),
                          std::abs(static_cast<f64>(v[i].y) - ky / (kx + ky))});
    CHECK(std::abs(static_cast<f64>(v[i].x) + v[i].y - 1.0) < 1e-6);
    CHECK(std::abs(static_cast<f64>(v[i].z) + v[i].w - 1.0) < 1e-6);
    dropped_far += v[i].x == 0.0f || v[i].y == 0.0f ? 1u : 0u;
    dropped_near += v[i].z == 0.0f || v[i].w == 0.0f ? 1u : 0u;
    if (i != 0) {
      far_jump = std::max({far_jump, std::abs(static_cast<f64>(v[i].x) - v[i - 1].x),
                           std::abs(static_cast<f64>(v[i].y) - v[i - 1].y)});
      near_jump = std::max({near_jump, std::abs(static_cast<f64>(v[i].z) - v[i - 1].z),
                            std::abs(static_cast<f64>(v[i].w) - v[i - 1].w)});
    }
  }
  MESSAGE("largest share change in 0.01 degree: far " << far_jump << ", near " << near_jump
                                                      << "; tilts reading one projection: far "
                                                      << dropped_far << ", near " << dropped_near);
  CHECK(worst_far < 1e-5);
  CHECK(far_jump < 1e-3);             // a band: no step anywhere
  CHECK(near_jump > 0.9 / 128.0);     // the near rule is the step it was
  CHECK(dropped_far > dropped_near);  // and the far one reads one projection on more slopes
  for (u32 j = 0; j < 512; ++j) {
    const Vec4 r = v[k_tilts + j];
    const f64 t = j / 128.0;
    CHECK(std::abs(static_cast<f64>(r.x) - smooth(1.0, 2.0, t)) < 1e-6);
    CHECK(std::abs(static_cast<f64>(r.y) - smooth(0.5, 1.0, 0.25 * t)) < 1e-6);
    const f64 w = t / 128.0;
    CHECK(std::abs(static_cast<f64>(r.z) -
                   w * std::clamp((w - 1.0 / 128) / (1.0 / 128), 0.0, 1.0)) < 1e-6);
    CHECK(r.w == static_cast<f32>(w));
    if (t <= 1.0) CHECK(r.x == 0.0f);         // under a cell: three tiles, exactly
    if (0.25 * t <= 0.5) CHECK(r.y == 0.0f);  // under half a repeat: the maps, exactly
  }
  gfx::destroy_buffer(device, output);
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  device.destroy();
}

TEST_CASE("gfx: a stochastic tile under 1/128 of the blend is ramped out, the rest untouched") {
  // R136's tiles by weight over one cell of the lattice: where every weight is 1/128 or more the
  // hex is the lattice's bit for bit; under 1/256 a tile's weight is 0 (it is not read); between,
  // the double mirror's band, renormalized, with the operator's gain from the kept weights.
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
      gfx::create_compute_pipeline(device, module, "kept_main", {}, 16, pipeline, &error), error);
  constexpr u32 k_count = 64 * 64;
  gfx::BufferResource output;
  REQUIRE_MESSAGE(
      gfx::create_buffer(device, k_count * 2 * sizeof(Vec4),
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
                        cb.dispatch(k_count / 64, 1, 1);
                      },
                      &error),
                  error);
  const auto* v = static_cast<const Vec4*>(output.mapped);
  constexpr f64 k_lo = 1.0 / 256, k_hi = 1.0 / 128;
  u32 untouched = 0, one_tile = 0, two_tiles = 0;
  f64 worst = 0;
  for (u32 i = 0; i < k_count; ++i) {
    const Vec4 lattice = v[2 * i], kept = v[2 * i + 1];
    const f64 w[3] = {lattice.x, lattice.y, lattice.z};
    const f32 k[3] = {kept.x, kept.y, kept.z};
    if (std::min({w[0], w[1], w[2]}) >= k_hi) {
      CHECK(std::bit_cast<u32>(kept.x) == std::bit_cast<u32>(lattice.x));
      CHECK(std::bit_cast<u32>(kept.y) == std::bit_cast<u32>(lattice.y));
      CHECK(std::bit_cast<u32>(kept.z) == std::bit_cast<u32>(lattice.z));
      CHECK(std::bit_cast<u32>(kept.w) == std::bit_cast<u32>(lattice.w));
      ++untouched;
      continue;
    }
    f64 band[3], sum = 0;
    for (u32 j = 0; j < 3; ++j) {
      band[j] = w[j] * std::clamp((w[j] - k_lo) / (k_hi - k_lo), 0.0, 1.0);
      sum += band[j];
    }
    f64 squares = 0;
    u32 read = 0;
    for (u32 j = 0; j < 3; ++j) {
      worst = std::max(worst, std::abs(k[j] - band[j] / sum));
      squares += (band[j] / sum) * (band[j] / sum);
      if (w[j] < k_lo) CHECK(k[j] == 0.0f);  // not read
      read += k[j] > 0.0f ? 1u : 0u;
    }
    CHECK(std::abs(kept.w - 1.0 / std::sqrt(squares)) < 1e-5);
    one_tile += read == 1 ? 1u : 0u;
    two_tiles += read == 2 ? 1u : 0u;
  }
  MESSAGE("of " << k_count << " points: " << untouched << " untouched, " << two_tiles
                << " read two tiles, " << one_tile << " one; worst weight against the mirror "
                << worst);
  CHECK(worst < 1e-6);
  CHECK(untouched > 0);
  CHECK(one_tile + two_tiles > k_count / 3);  // the rule reads fewer tiles on much of the cell
  gfx::destroy_buffer(device, output);
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  device.destroy();
}
