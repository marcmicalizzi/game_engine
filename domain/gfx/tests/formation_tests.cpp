// The world formation (W85; domain/gfx/formation.h, shaders/formation.slang): the schema's ranges,
// the twin's periodicity and fold, and the shader held to the twin over a lattice of points from
// the origin to 10,000 km out.
#include <core/math/math.h>
#include <domain/gfx/commands.h>
#include <domain/gfx/device.h>
#include <domain/gfx/formation.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/resources.h>

#include <doctest/doctest.h>

#include <cmath>
#include <cstring>
#include <shaders/formation_probe.spv.h>
#include <string>

using namespace engine;

namespace {

scene::FormationBed bed(const char* name, f32 thickness, Vec3 tint, f32 roughness, f32 resistance) {
  scene::FormationBed b;
  b.name = name;
  b.thickness_m = thickness;
  b.tint = tint;
  b.roughness = roughness;
  b.resistance = resistance;
  return b;
}

// Four beds of a sandstone-shale alternation (thicknesses drawn from a lognormal about 0.9 m, the
// grounding page's numbers) and three members at tens of metres.
scene::Formation fixture() {
  scene::Formation f;
  f.origin = WorldPos{123.4567, -20.0, -987.6543};
  f.dip_deg = 30.0f;
  f.azimuth_deg = 35.0f;
  f.fold_amplitude_m = 12.0f;
  f.fold_wavelength_m = 180.0f;
  f.fold2_amplitude_m = 40.0f;
  f.fold2_wavelength_m = 2400.0f;
  f.soft_m = 0.1f;
  f.beds.push_back(bed("sandstone", 1.42f, Vec3{1.10f, 1.02f, 0.90f}, -0.05f, 0.85f));
  f.beds.push_back(bed("shale", 0.37f, Vec3{0.70f, 0.72f, 0.78f}, 0.10f, 0.15f));
  f.beds.push_back(bed("siltstone", 0.81f, Vec3{0.92f, 0.90f, 0.88f}, 0.02f, 0.45f));
  f.beds.push_back(bed("limestone", 2.23f, Vec3{1.25f, 1.22f, 1.18f}, -0.08f, 0.95f));
  f.macro_beds.push_back(bed("lower", 34.0f, Vec3{1.0f, 0.95f, 0.9f}, 0.0f, 0.6f));
  f.macro_beds.push_back(bed("middle", 52.5f, Vec3{0.85f, 0.88f, 0.92f}, 0.03f, 0.3f));
  f.macro_beds.push_back(bed("upper", 41.0f, Vec3{1.1f, 1.08f, 1.0f}, -0.02f, 0.7f));
  return f;
}

f64 wrap(f64 a, f64 period) {
  const f64 r = a - period * std::floor(a / period);
  return r > 0.5 * period ? r - period : r;
}

}  // namespace

TEST_CASE("formation: the schema's ranges are refused with a sentence") {
  std::string error;
  gfx::FormationParams p;
  CHECK(gfx::pack_formation(fixture(), p, &error));
  CHECK(p.flags == gfx::k_formation_valid);
  CHECK(p.beds.period_mm == 1420u + 370u + 810u + 2230u);
  const auto refuse = [&](scene::Formation f, const char* fragment) {
    error.clear();
    CHECK_FALSE(gfx::pack_formation(f, p, &error));
    CHECK_MESSAGE(error.find(fragment) != std::string::npos, error);
    CHECK(error.rfind("formation: ", 0) == 0);
  };
  scene::Formation f = fixture();
  f.dip_deg = 86.0f;
  refuse(f, "dip_deg");
  f = fixture();
  f.dip_deg = -90.0f;
  refuse(f, "dip_deg");
  f = fixture();
  f.beds.clear();
  refuse(f, "at least one bed");
  f = fixture();
  f.beds[1].thickness_m = 0.0f;
  refuse(f, "thickness_m");
  f = fixture();
  f.macro_beds[0].thickness_m = -3.0f;
  refuse(f, "thickness_m");
  f = fixture();
  f.soft_m = 11.0f;
  refuse(f, "soft_m");
  f = fixture();
  f.soft_m = -0.1f;
  refuse(f, "soft_m");
  f = fixture();
  f.fold_wavelength_m = 0.5f;
  refuse(f, "wavelength");
  f = fixture();
  f.beds[0].tint = Vec3{2.5f, 1.0f, 1.0f};
  refuse(f, "tint");
  f = fixture();
  const scene::FormationBed extra = f.beds[0];
  for (u32 i = 0; i < 6; ++i)
    f.beds.push_back(extra);
  refuse(f, "at most 8");
}

TEST_CASE(
    "formation: a bed repeats with its sequence's period, and the fold moves it by its "
    "amplitude") {
  std::string error;
  gfx::FormationParams p;
  REQUIRE(gfx::pack_formation(fixture(), p, &error));
  const f64 period = static_cast<f64>(p.beds.period_mm) / 1000.0;
  // Periodicity: the same bed, the same fraction, a whole number of periods away, either side.
  for (f64 c = -7.3; c < 7.3; c += 0.137) {
    const gfx::FormationBedSample a = gfx::formation_bed(p.beds, c);
    for (const f64 k : {-3.0, 1.0, 250.0}) {
      const gfx::FormationBedSample b = gfx::formation_bed(p.beds, c + k * period);
      CHECK(a.index == b.index);
      CHECK(std::fabs(a.fraction - b.fraction) < 1e-9 * (1.0 + std::fabs(k)) * 100.0);
    }
  }
  // The beds in order from the base: 1.42 m of the first, then the second.
  CHECK(gfx::formation_bed(p.beds, 0.01).index == 0u);
  CHECK(gfx::formation_bed(p.beds, 1.43).index == 1u);
  CHECK(gfx::formation_bed(p.beds, 1.80).index == 2u);
  CHECK(gfx::formation_bed(p.beds, 4.82).index == 3u);
  CHECK(gfx::formation_bed(p.beds, -0.01).index == 3u);
  // A box wider than many periods reads the sequence's mean.
  const Vec4 mean = gfx::formation_filtered(p.beds, 0.3, 40.0 * period);
  CHECK(std::fabs(static_cast<f64>(mean.x) - static_cast<f64>(p.beds.total.x) / period) < 1e-4);
  CHECK(std::fabs(static_cast<f64>(mean.w) - static_cast<f64>(p.beds.total.w) / period) < 1e-4);
  // The fold: horizontal beds (dip 0, azimuth 0, so the fold runs along x) with one 12 m fold of
  // 180 m. The bed surface through the origin stands at y = A sin(2 pi x / L): +A at a quarter
  // wavelength, -A at three quarters, zero at the hinge points between.
  scene::Formation flat = fixture();
  flat.origin = WorldPos{0.0, 0.0, 0.0};
  flat.dip_deg = 0.0f;
  flat.azimuth_deg = 0.0f;
  flat.fold2_amplitude_m = 0.0f;
  REQUIRE(gfx::pack_formation(flat, p, &error));
  const gfx::FormationFrame frame = gfx::formation_frame(p);
  CHECK(std::fabs(gfx::formation_coordinate(frame, 45.0, 12.0, 3.0).coordinate) < 1e-9);
  CHECK(std::fabs(gfx::formation_coordinate(frame, 135.0, -12.0, -7.0).coordinate) < 1e-9);
  CHECK(std::fabs(gfx::formation_coordinate(frame, 90.0, 0.0, 0.0).coordinate) < 1e-9);
  CHECK(std::fabs(gfx::formation_coordinate(frame, 45.0, 0.0, 0.0).coordinate + 12.0) < 1e-9);
  // A wavelength on, the same phase: a point 5 m up there is 7 m under the folded surface.
  CHECK(std::fabs(gfx::formation_coordinate(frame, 225.0, 5.0, 0.0).coordinate - 5.0 + 12.0) <
        1e-9);
}

TEST_CASE("formation: the shader's coordinate, bed and filter are its f64 twin's") {
  gfx::Device device;
  std::string error;
  if (!device.create(gfx::DeviceOptions{}, &error)) {
    MESSAGE("no device: " << error);
    return;
  }
  const auto module = gfx::create_shader_module(device, shaders::k_formation_probe_spirv,
                                                shaders::k_formation_probe_spirv_size, &error);
  REQUIRE_MESSAGE(module.valid(), error);
  gfx::ComputePipeline pipeline;
  REQUIRE_MESSAGE(gfx::create_compute_pipeline(device, module, "formation_probe_main", {}, 32,
                                               pipeline, &error),
                  error);
  gfx::FormationParams params;
  REQUIRE(gfx::pack_formation(fixture(), params, &error));
  const gfx::FormationFrame frame = gfx::formation_frame(params);
  struct Point {
    i32 cell[3];
    f32 width;
    f32 local[3];
    f32 pad;
  };
  Vector<Point> points;
  // A lattice round the origin and round four far places, 10 km and 10,000 km out either way: the
  // reduction is what keeps the far ones exact.
  const i32 centres[][3] = {
      {0, 0, 0}, {156, 1, -156}, {-157, -2, 155}, {156250, 3, 156250}, {-156250, -1, -156250}};
  for (const auto& centre : centres)
    for (i32 a = -3; a <= 3; ++a)
      for (i32 b = -2; b <= 2; ++b)
        for (i32 c = -3; c <= 3; ++c) {
          Point q{};
          q.cell[0] = centre[0] + (a < 0 ? -1 : 0);
          q.cell[1] = centre[1] + (b < 0 ? -1 : 0);
          q.cell[2] = centre[2] + (c < 0 ? -1 : 0);
          q.local[0] = static_cast<f32>(std::fmod(17.3 * a + 64.0 + 0.001 * c, 64.0));
          q.local[1] = static_cast<f32>(std::fmod(9.71 * b + 64.0, 64.0));
          q.local[2] = static_cast<f32>(std::fmod(13.9 * c + 64.0 + 0.37 * a, 64.0));
          q.width = static_cast<f32>((a + 3) * 0.25);
          points.push_back(q);
        }
  const auto storage = gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
  gfx::BufferResource block, input, output;
  REQUIRE(gfx::create_buffer(device, sizeof(params), storage, true, block, &error));
  std::memcpy(block.mapped, &params, sizeof(params));
  REQUIRE(
      gfx::create_buffer(device, u64{points.size()} * sizeof(Point), storage, true, input, &error));
  std::memcpy(input.mapped, points.data(), u64{points.size()} * sizeof(Point));
  REQUIRE(gfx::create_buffer(device, u64{points.size()} * 3 * sizeof(Vec4), storage, true, output,
                             &error));
  struct Push {
    u64 output;
    u64 formation;
    u64 points;
    u32 count;
    u32 pad;
  };
  const Push push{output.address, block.address, input.address, points.size(), 0};
  REQUIRE_MESSAGE(gfx::submit_immediate(
                      device,
                      [&](gfx::CommandList cb) {
                        cb.bind_pipeline(gfx::BindPoint::Compute, pipeline.pipeline);
                        cb.push_constants(pipeline.layout, gfx::ShaderStage::Compute, 0,
                                          sizeof(push), &push);
                        cb.dispatch((points.size() + 63) / 64, 1, 1);
                      },
                      &error),
                  error);
  const auto* got = static_cast<const Vec4*>(output.mapped);
  const f64 period = static_cast<f64>(params.beds.period_mm) / 1000.0;
  const f64 macro_period = static_cast<f64>(params.macro.period_mm) / 1000.0;
  f64 worst = 0.0, worst_macro = 0.0, worst_gradient = 0.0, worst_filter = 0.0;
  u32 compared = 0, edges = 0;
  for (u32 i = 0; i < points.size(); ++i) {
    const Point& q = points[i];
    // What the float local means, exactly: the twin's own reading of the probe's point.
    const f64 x = static_cast<f64>(q.cell[0]) * 64.0 + static_cast<f64>(q.local[0]);
    const f64 y = static_cast<f64>(q.cell[1]) * 64.0 + static_cast<f64>(q.local[1]);
    const f64 z = static_cast<f64>(q.cell[2]) * 64.0 + static_cast<f64>(q.local[2]);
    const gfx::FormationCoordinate want = gfx::formation_coordinate(frame, x, y, z);
    const Vec4 a = got[3 * i], g = got[3 * i + 1], filtered = got[3 * i + 2];
    const f64 e = std::fabs(wrap(static_cast<f64>(a.x) - want.coordinate, period));
    const f64 em = std::fabs(wrap(static_cast<f64>(a.y) - want.coordinate, macro_period));
    worst = e > worst ? e : worst;
    worst_macro = em > worst_macro ? em : worst_macro;
    CHECK_MESSAGE(e < 1e-3, "point " << i << ": " << a.x << " against " << want.coordinate);
    CHECK_MESSAGE(em < 1e-3, "point " << i);
    const f64 grad[3] = {static_cast<f64>(g.x), static_cast<f64>(g.y), static_cast<f64>(g.z)};
    for (u32 k = 0; k < 3; ++k) {
      const f64 d = std::fabs(grad[k] - want.gradient[k]);
      worst_gradient = d > worst_gradient ? d : worst_gradient;
    }
    // The bed, where the point is not within a millimetre of a boundary.
    const gfx::FormationBedSample b = gfx::formation_bed(params.beds, want.coordinate);
    const gfx::FormationBedSample m = gfx::formation_bed(params.macro, want.coordinate);
    const f64 thick = static_cast<f64>(params.beds.beds[b.index].thickness_m);
    if (b.fraction * thick < 1e-3 || (1.0 - b.fraction) * thick < 1e-3) {
      ++edges;
      continue;
    }
    ++compared;
    CHECK(static_cast<u32>(a.z) == b.index);
    CHECK(static_cast<u32>(a.w) == m.index);
    CHECK(std::fabs(static_cast<f64>(g.w) - b.fraction) < 2e-3);
    const Vec4 wf = gfx::formation_filtered(params.beds, want.coordinate, static_cast<f64>(q.width));
    const f64 df = std::fabs(static_cast<f64>(filtered.x) - static_cast<f64>(wf.x)) +
                   std::fabs(static_cast<f64>(filtered.w) - static_cast<f64>(wf.w));
    worst_filter = df > worst_filter ? df : worst_filter;
  }
  MESSAGE("coordinate error " << worst << " m, macro " << worst_macro << " m, gradient "
                              << worst_gradient << ", filter " << worst_filter << "; " << compared
                              << " beds compared, " << edges << " within a millimetre of an edge");
  CHECK(worst_gradient < 1e-4);
  // The box's edges move with the coordinate, by the coordinate's error, a few micrometres.
  CHECK(worst_filter < 1e-3);
  CHECK(compared > points.size() / 2);
  for (gfx::BufferResource* b : {&block, &input, &output})
    gfx::destroy_buffer(device, *b);
  gfx::destroy_compute_pipeline(device, pipeline);
  gfx::destroy_shader_module(device, module);
  device.destroy();
}
