#include <core/math/math.h>
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
#include <shaders/terrain_mask_probe.spv.h>
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
    const f64 blend = m0 == mips - 1 ? 0.0 : e.level - m0;
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
      CHECK(std::abs(have[ch] - want[ch]) < 2e-4);
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
