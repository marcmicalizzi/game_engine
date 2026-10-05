// The frame's origin on the GPU (ADR-0053; docs/subsystems/gfx.md, "The frame's origin"):
// shaders/scene.slang's `instance_from_eye` against `engine::relative(WorldCell, WorldEye)` in
// core/math/world.h, **to the bit**, for instances and eyes by the origin, at the owner's 419 km,
// at 10,000 km, at 1e8 m and out to 1e11 m, an eye a centimetre to 80 km from the instance, and an
// eye or an instance on either side of a cell's edge. The shader's function is the CPU's operation
// for operation and marked `precise`; a driver that fused or reordered it, or a mirror that
// drifted, fails here before any picture does.
//
// And the property the translation suite rests on, one level down: every case is drawn again moved
// by 6,548, 156,250 and 1,562,500 whole cells, eye and instance together, and `instance_from_eye`
// and `instance_point` give the same bits as the case by the origin.
#include "raster_path.h"

#include <core/math/world.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/device.h>
#include <domain/gfx/pipeline.h>

#include <doctest/doctest.h>

#include <cstring>
#include <random>
#include <shaders/world_eye_probe.spv.h>
#include <string>
#include <vector>

using namespace engine;

namespace {

struct ProbeParams {
  u64 instances = 0;
  u64 eyes = 0;
  u64 points = 0;
  u64 out = 0;
  u32 count = 0;
  u32 pad = 0;
};
static_assert(sizeof(ProbeParams) == 40);

u32 bits(f32 v) {
  u32 b = 0;
  std::memcpy(&b, &v, sizeof(b));
  return b;
}

struct Case {
  WorldPos at;     // the instance's translation
  WorldPos eye;    // the frame's eye
  Vec3 point;      // a mesh-space point of the instance
  f32 yaw = 0.0f;  // the instance's rotation about y
};

// Whole 1024ths of a metre, so f64 holds every position here exactly out to 1e11 m and a move by
// whole cells is exact too (math.md, "World positions").
f64 dyadic(f64 v) { return std::floor(v * 1024.0) / 1024.0; }

}  // namespace

TEST_CASE(
    "world eye: instance_from_eye is engine::relative to the bit, and moving by cells "
    "changes no bit") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;

  // The cases, by the origin. Distances from 5 cm to 80 km, eyes on both sides of cell edges, an
  // instance exactly on an edge, a local that rounds up to 64.0f, negative coordinates.
  std::vector<Case> base;
  std::mt19937_64 rng(0xADA053ull);
  std::uniform_real_distribution<f64> unit(-1.0, 1.0);
  const f64 distances[] = {0.05, 0.75, 3.0, 61.0, 64.0, 500.0, 4096.0, 80000.0};
  const f64 places[] = {0.0, 1.0, 63.999, 64.0, 3700.0, 419072.0, -419070.0, 1.0e7, -1.0e7, 1.0e8};
  for (const f64 place : places) {
    for (const f64 distance : distances) {
      for (u32 k = 0; k < 6; ++k) {
        Case c;
        c.eye = WorldPos{dyadic(place + 31.0 * unit(rng)), dyadic(80.0 + 20.0 * unit(rng)),
                         dyadic(-place + 31.0 * unit(rng))};
        const DVec3 off{unit(rng), 0.3 * unit(rng), unit(rng)};
        c.at = c.eye + off * (distance / length(off));
        c.at = WorldPos{dyadic(c.at.x), dyadic(c.at.y), dyadic(c.at.z)};
        c.point = Vec3{static_cast<f32>(3.0 * unit(rng)), static_cast<f32>(2.0 * unit(rng)),
                       static_cast<f32>(3.0 * unit(rng))};
        c.yaw = static_cast<f32>(3.0 * unit(rng));
        base.push_back(c);
      }
    }
  }
  // An eye a millimetre either side of a cell's edge, with the instance 3 m away across it: the
  // case the shader's order of operations exists for (world.h, `relative_axis`).
  for (const f64 side : {-0.0009765625, 0.0009765625}) {
    Case c;
    c.eye = WorldPos{64.0 * 6548.0 + side, 82.0, -64.0 * 1043.0 - side};
    c.at = WorldPos{c.eye.x + 3.0, 81.5, c.eye.z - 3.0};
    c.point = Vec3{0.5f, 0.25f, -0.5f};
    base.push_back(c);
  }
  // An instance whose offset into its cell rounds to 64.0f: 2 um under the next cell's corner.
  {
    Case c;
    c.at = WorldPos{64.0 * 10.0 - 1.0 / 1048576.0, 0.0, 0.0};
    c.eye = WorldPos{640.5, 1.0, 0.25};
    base.push_back(c);
  }

  // Every case at the origin and moved by whole cells, eye and instance alike.
  const f64 shifts[] = {0.0, 6548.0, 156250.0, 1562500.0};
  constexpr u32 k_shifts = 4;
  const u32 per_shift = static_cast<u32>(base.size());
  const u32 count = per_shift * k_shifts;
  std::vector<gfx::InstanceDesc> instances(count);
  std::vector<gfx::FrameEye> eyes(count);
  std::vector<Vec4> points(count);
  std::vector<WorldEye> world_eyes(count);
  for (u32 s = 0; s < k_shifts; ++s) {
    const DVec3 shift{shifts[s] * k_world_cell_m, 0.0, -shifts[s] * k_world_cell_m};
    for (u32 i = 0; i < per_shift; ++i) {
      const Case& c = base[i];
      const u32 n = s * per_shift + i;
      const Mat4 linear = mat4_from_quat(quat_from_axis_angle(Vec3{0.0f, 1.0f, 0.0f}, c.yaw));
      gfx::set_instance_placement(instances[n], linear, c.at + shift);
      world_eyes[n] = to_eye(c.eye + shift);
      eyes[n] = gfx::frame_eye(world_eyes[n]);
      points[n] = Vec4{c.point, 0.0f};
    }
  }

  gfx::BufferResource b_instances, b_eyes, b_points, b_out;
  constexpr gfx::BufferUsage k_usage =
      gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
  REQUIRE_MESSAGE(gfx::create_buffer(device, sizeof(gfx::InstanceDesc) * count, k_usage, true,
                                     b_instances, &error),
                  error);
  REQUIRE_MESSAGE(
      gfx::create_buffer(device, sizeof(gfx::FrameEye) * count, k_usage, true, b_eyes, &error),
      error);
  REQUIRE_MESSAGE(gfx::create_buffer(device, sizeof(Vec4) * count, k_usage, true, b_points, &error),
                  error);
  REQUIRE_MESSAGE(
      gfx::create_buffer(device, sizeof(Vec4) * 2 * count, k_usage, true, b_out, &error), error);
  std::memcpy(b_instances.mapped, instances.data(), sizeof(gfx::InstanceDesc) * count);
  std::memcpy(b_eyes.mapped, eyes.data(), sizeof(gfx::FrameEye) * count);
  std::memcpy(b_points.mapped, points.data(), sizeof(Vec4) * count);

  gfx::ShaderModuleHandle module = gfx::create_shader_module(
      device, shaders::k_world_eye_probe_spirv, shaders::k_world_eye_probe_spirv_size, &error);
  REQUIRE_MESSAGE(module.valid(), error);
  gfx::ComputePipeline pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "world_eye_main", {},
                                               sizeof(ProbeParams), pipeline, &error),
                  error);
  ProbeParams params;
  params.instances = b_instances.address;
  params.eyes = b_eyes.address;
  params.points = b_points.address;
  params.out = b_out.address;
  params.count = count;
  REQUIRE_MESSAGE(gfx::submit_immediate(
                      device,
                      [&](gfx::CommandList cb) {
                        cb.bind_pipeline(gfx::BindPoint::Compute, pipeline.pipeline);
                        cb.push_constants(pipeline.layout, gfx::ShaderStage::Compute, 0,
                                          sizeof(params), &params);
                        cb.dispatch((count + 63) / 64, 1, 1);
                      },
                      &error),
                  error);

  const auto* out = static_cast<const Vec4*>(b_out.mapped);
  u32 mismatched = 0;
  u32 moved = 0;
  for (u32 n = 0; n < count; ++n) {
    // The shader against the CPU's function, every bit of every axis.
    const Vec3 cpu = relative(gfx::instance_cell(instances[n]), world_eyes[n]);
    const Vec3 gpu = out[2 * n].xyz();
    for (u32 a = 0; a < 3; ++a) {
      if (bits(gpu[a]) != bits(cpu[a])) {
        if (mismatched++ < 8) {
          MESSAGE("case " << n << " axis " << a << ": gpu " << gpu[a] << " cpu " << cpu[a]);
        }
      }
    }
    // The same case moved by whole cells: the same bits out of both functions.
    const u32 i = n % per_shift;
    for (u32 k = 0; k < 2; ++k) {
      const Vec3 here = out[2 * n + k].xyz();
      const Vec3 there = out[2 * i + k].xyz();
      for (u32 a = 0; a < 3; ++a) {
        if (bits(here[a]) != bits(there[a])) {
          if (moved++ < 8) MESSAGE("case " << i << " moved by " << n / per_shift << " differs");
        }
      }
    }
  }
  CHECK(mismatched == 0);
  CHECK(moved == 0);
  // And the arithmetic is what it claims: by the eye, a float step at the result's size.
  for (u32 i = 0; i < per_shift; ++i) {
    const DVec3 exact = to_world(gfx::instance_cell(instances[i])) - to_world(world_eyes[i]);
    const Vec3 gpu = out[2 * i].xyz();
    for (u32 a = 0; a < 3; ++a) {
      const f64 e = a == 0 ? exact.x : (a == 1 ? exact.y : exact.z);
      CHECK(std::abs(static_cast<f64>(gpu[a]) - e) <= 3.0 * 1.2e-7 * std::abs(e) + 1.0e-9);
    }
  }

  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  for (gfx::BufferResource* b : {&b_instances, &b_eyes, &b_points, &b_out})
    gfx::destroy_buffer(device, *b);
}

// **A terrain tile's corner comes back to the millimetre** (cluster_cull.h, `set_terrain_corner`;
// renderer.md, "The ground's tiles are placed at their corners"). The pool pass and the cull pass
// find every lattice point of a tile from the corner in whole millimetres, which they read back off
// the instance's cell and float32 local; a millimetre lost there is a tile drawn a millimetre off.
// Corners on lattices of 25 cm to 64 m, by the origin, astride a cell's edge, at the owner's 420 km
// and at 10,000 km and 1e8 m, on both sides of zero: every one comes back exactly, and the local is
// in [0, 64).
TEST_CASE("terrain corner: a tile's corner placed from millimetres reads back to the millimetre") {
  const i64 sites_mm[] = {0,
                          gfx::k_world_cell_mm,
                          gfx::k_world_cell_mm - 1,
                          419'070'000,
                          -419'070'000,
                          -66'781'000,
                          10'000'000'000,
                          -10'000'000'000,
                          100'000'000'000,
                          -100'000'000'000};
  const i64 spacings_mm[] = {250, 500, 1'000, 4'000, 16'000, 64'000};
  u32 checked = 0;
  for (const i64 site : sites_mm) {
    for (const i64 spacing : spacings_mm) {
      for (i64 k = -3; k <= 3; ++k) {
        // A lattice point near the site, and one a millimetre either side of it.
        const i64 base = gfx::terrain_floor_div(site, spacing) * spacing + k * spacing;
        for (const i64 x : {base, base + 1, base - 1}) {
          const i64 z = -x / 3;
          gfx::InstanceDesc instance{};
          gfx::set_terrain_corner(instance, x, z);
          const gfx::TerrainCornerMm back = gfx::terrain_corner_mm(instance);
          REQUIRE_MESSAGE(back.x == x, "x " << x << " came back as " << back.x);
          REQUIRE_MESSAGE(back.z == z, "z " << z << " came back as " << back.z);
          CHECK(instance.rows[0].w >= 0.0f);
          CHECK(instance.rows[0].w < 64.0f);
          CHECK(instance.cell.y == 0);
          ++checked;
        }
      }
    }
  }
  MESSAGE(checked << " corners, from the origin to 1e8 m: every one back to the millimetre");
}
