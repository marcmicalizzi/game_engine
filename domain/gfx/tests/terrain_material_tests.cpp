#include "terrain_hex_reference.h"
#include "brdf_reference.h"

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
