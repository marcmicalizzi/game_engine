// The sky on the GPU against its CPU mirror (sky.slang and sky_luts.slang against sky_reference.h;
// docs/subsystems/gfx.md, "The sky"). Four skies — noon, a sun two degrees up, a sun five degrees
// under the horizon, and a moonlit night with the sun forty degrees down — each built into every
// table on the GPU and again in double precision on the CPU, and held texel by texel: the
// transmittance, Psi_ms, the sky from the eye and the air to a surface; then the frame's sums (the
// ambient term's nine coefficients, the ground, the lights at the eye, the exposure); then the
// functions the resolve and the reference call, at the test's own directions — the sky an uncovered
// pixel shows (with the sun's disc, the moon's lit disc and a star's spot), the ambient radiance at
// a normal, the air to a surface, and the light that reaches a point. The tolerances say by how
// much float and double disagree about all of it.
//
// Also, with no device: the mirror's model against what the sky must do — the transmittance falls
// towards the horizon and reddens, the table's ends are its rays, the sky is blue overhead at noon
// and red on the sun's side at sunset, the exposure rule's stops.
#include "raster_path.h"
#include "sky_reference.h"

#include <domain/gfx/device.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/sky.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <ostream>
#include <shaders/sky_luts.spv.h>
#include <shaders/sky_probe.spv.h>
#include <string>
#include <vector>

using namespace engine;
namespace sref = engine::sky_ref;

namespace {

constexpr f64 k_pi64 = 3.14159265358979323846;

Vec3 from_angles(f64 azimuth_deg, f64 elevation_deg) {
  const f64 az = azimuth_deg * k_pi64 / 180.0;
  const f64 el = elevation_deg * k_pi64 / 180.0;
  return Vec3{static_cast<f32>(std::cos(el) * std::cos(az)), static_cast<f32>(std::sin(el)),
              static_cast<f32>(std::cos(el) * std::sin(az))};
}

// Earth's air at a turbidity of 2.5 (the sky capability's numbers, sky.md), a sun and a moon where
// the case puts them, an eye 1.7 m over the sand at the origin, and a celestial frame tilted 30
// degrees.
gfx::SkyParams earth(f64 sun_az, f64 sun_el, f64 moon_az, f64 moon_el, u32 flags) {
  gfx::SkyParams p;
  p.rayleigh = Vec4{5.802e-3f, 13.558e-3f, 33.1e-3f, 8.0f};
  const f32 ext = static_cast<f32>(1.5 * 13.558e-3 * 8.0 / 1.2);
  const Vec3 mie_ext{ext * 0.8440f, ext, ext * 1.1954f};
  p.mie_extinction = Vec4{mie_ext.x, mie_ext.y, mie_ext.z, 0.76f};
  p.mie_scattering = Vec4{mie_ext.x * 0.9f, mie_ext.y * 0.9f, mie_ext.z * 0.9f, 1.2f};
  p.ozone = Vec4{0.650e-3f, 1.881e-3f, 0.085e-3f, 25.0f};
  p.radii = Vec4{6360.0f, 6460.0f, 15.0f, 0.0f};
  p.ground_albedo = Vec4{0.35f, 0.35f, 0.35f, 0.0f};
  p.local_albedo = Vec4{0.84f, 0.69f, 0.47f, 0.0f};
  p.night = Vec4{1.4e-9f, 1.6e-9f, 1.9e-9f, 0.0f};
  p.sun = Vec4{from_angles(sun_az, sun_el), 4.65e-3f};
  p.sun_illuminance = Vec4{1.0f, 1.0f, 1.0f, 0.0f};
  p.moon = Vec4{from_angles(moon_az, moon_el), 4.52e-3f};
  p.moon_illuminance = Vec4{1.6e-6f, 1.6e-6f, 1.6e-6f, 0.12f};
  p.camera = Vec4{0.0f, 1.7f, 0.0f, gfx::k_sky_min_altitude_km};
  const f32 lat = 30.0f * 3.14159265f / 180.0f;
  p.celestial_x = Vec4{-1.0f, 0.0f, 0.0f, 0.0f};
  p.celestial_y = Vec4{0.0f, std::cos(lat), std::sin(lat), 0.0f};
  p.celestial_z = Vec4{0.0f, std::sin(lat), -std::cos(lat), 0.0f};
  p.exposure = Vec4{gfx::k_sky_exposure_auto, 0.0f, 10.0f, 0.5f};
  p.stars = Vec4{3.0f, 1.0f, 0.0f, 0.0f};
  p.flags = flags;
  p.view_count = 1;
  p.views[0].pixel = Vec4{1.0e-3f, 0.0f, 0.0f, 0.0f};
  return p;
}

// Three stars of the case's own, each listed in the cell of its own direction and in no other,
// on the GPU and in the mirror alike.
struct TestStars {
  std::vector<Vec4> list;  // direction, then irradiance
  std::vector<u32> cells;
  std::vector<u32> index;
  sref::Stars mirror;
};

TestStars test_stars(const gfx::SkyParams& p) {
  TestStars t;
  // Celestial directions the frame above puts at three places in the sky: up high, low in the
  // south-east, and in the west.
  const Vec3 world[3] = {normalize(Vec3{0.1f, 0.95f, 0.2f}), normalize(Vec3{0.5f, 0.2f, 0.6f}),
                         normalize(Vec3{-0.8f, 0.4f, -0.1f})};
  const f64 irradiance[3] = {2.0e-11, 5.0e-12, 1.0e-13};
  const Vec3 cx = Vec3{p.celestial_x.x, p.celestial_x.y, p.celestial_x.z};
  const Vec3 cy = Vec3{p.celestial_y.x, p.celestial_y.y, p.celestial_y.z};
  const Vec3 cz = Vec3{p.celestial_z.x, p.celestial_z.y, p.celestial_z.z};
  std::vector<std::vector<u32>> per_cell(gfx::k_sky_star_cells);
  for (u32 s = 0; s < 3; ++s) {
    const Vec3 d{dot(world[s], cx), dot(world[s], cy), dot(world[s], cz)};
    const Vec3 color{1.1f, 1.0f, 0.8f};
    t.list.push_back(Vec4{d.x, d.y, d.z, 0.0f});
    t.list.push_back(Vec4{color.x * static_cast<f32>(irradiance[s]),
                          color.y * static_cast<f32>(irradiance[s]),
                          color.z * static_cast<f32>(irradiance[s]), 0.0f});
    t.mirror.direction.push_back(brdf_ref::dvec3(d));
    t.mirror.irradiance.push_back(brdf_ref::dvec3(Vec4{t.list.back()}));
    per_cell[sref::star_cell(brdf_ref::dvec3(d))].push_back(s);
  }
  t.cells.push_back(0);
  for (const auto& c : per_cell) {
    for (const u32 s : c)
      t.index.push_back(s);
    t.cells.push_back(static_cast<u32>(t.index.size()));
  }
  t.mirror.cells = t.cells;
  t.mirror.index = t.index;
  return t;
}

struct GpuSky {
  gfx::BufferResource params, transmittance, multiscatter, sky_view, aerial, frame;
  gfx::BufferResource star_list, star_cells, star_index, queries, out;
  gfx::ComputePipeline pipelines[5];
  gfx::ComputePipeline probe;
  gfx::ShaderModuleHandle luts_module, probe_module;

  bool create(const gfx::Device& device, std::string* error) {
    luts_module = gfx::create_shader_module(device, shaders::k_sky_luts_spirv,
                                            shaders::k_sky_luts_spirv_size, error);
    probe_module = gfx::create_shader_module(device, shaders::k_sky_probe_spirv,
                                             shaders::k_sky_probe_spirv_size, error);
    if (!luts_module.valid() || !probe_module.valid()) return false;
    const char* entries[5] = {"transmittance_main", "multiscatter_main", "sky_view_main",
                              "aerial_main", "sky_frame_main"};
    for (u32 i = 0; i < 5; ++i) {
      if (!gfx::create_compute_pipeline(device, luts_module, entries[i], {}, sizeof(u64),
                                        pipelines[i], error))
        return false;
    }
    if (!gfx::create_compute_pipeline(device, probe_module, "probe_main", {}, 32, probe, error))
      return false;
    constexpr gfx::BufferUsage k_usage =
        gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;
    constexpr u64 k_texel = 16;
    const u64 sizes[] = {
        sizeof(gfx::SkyParams),
        k_texel * gfx::k_sky_transmittance_width * gfx::k_sky_transmittance_height,
        k_texel * gfx::k_sky_multiscatter_size * gfx::k_sky_multiscatter_size,
        k_texel * gfx::k_sky_view_width * gfx::k_sky_view_height,
        k_texel * gfx::k_sky_aerial_width * gfx::k_sky_aerial_height * gfx::k_sky_aerial_depth,
        sizeof(gfx::SkyFrame),
        k_texel * 64,
        4 * (gfx::k_sky_star_cells + 1),
        4 * 64,
        k_texel * 2 * 256,
        k_texel * 5 * 256};
    gfx::BufferResource* targets[] = {&params,     &transmittance, &multiscatter, &sky_view,
                                      &aerial,     &frame,         &star_list,    &star_cells,
                                      &star_index, &queries,       &out};
    for (u32 i = 0; i < 11; ++i) {
      if (!gfx::create_buffer(device, sizes[i], k_usage, true, *targets[i], error)) return false;
    }
    return true;
  }

  void destroy(const gfx::Device& device) {
    for (gfx::BufferResource* b : {&params, &transmittance, &multiscatter, &sky_view, &aerial,
                                   &frame, &star_list, &star_cells, &star_index, &queries, &out})
      gfx::destroy_buffer(device, *b);
    for (gfx::ComputePipeline& p : pipelines)
      gfx::destroy_compute_pipeline(device, p);
    gfx::destroy_compute_pipeline(device, probe);
    gfx::destroy_shader_module(device, luts_module);
    gfx::destroy_shader_module(device, probe_module);
  }

  bool dispatch(const gfx::Device& device, const gfx::ComputePipeline& p, u32 gx, u32 gy,
                const void* push, u32 push_bytes, std::string* error) {
    return gfx::submit_immediate(
        device,
        [&](gfx::CommandList cb) {
          cb.bind_pipeline(gfx::BindPoint::Compute, p.pipeline);
          cb.push_constants(p.layout, gfx::ShaderStage::Compute, 0, push_bytes, push);
          cb.dispatch(gx, gy, 1);
        },
        error);
  }

  // Every table and the frame's sums, one dispatch at a time, each waited for.
  bool build(const gfx::Device& device, gfx::SkyParams p, const TestStars& stars,
             std::string* error) {
    std::memcpy(star_list.mapped, stars.list.data(), stars.list.size() * sizeof(Vec4));
    std::memcpy(star_cells.mapped, stars.cells.data(), stars.cells.size() * sizeof(u32));
    std::memcpy(star_index.mapped, stars.index.data(), stars.index.size() * sizeof(u32));
    p.transmittance = transmittance.address;
    p.multiscatter = multiscatter.address;
    p.sky_view = sky_view.address;
    p.aerial = aerial.address;
    p.frame = frame.address;
    p.star_list = star_list.address;
    p.star_cells = star_cells.address;
    p.star_index = star_index.address;
    std::memcpy(params.mapped, &p, sizeof(p));
    const u64 address = params.address;
    auto groups = [](u32 n) { return (n + 7) / 8; };
    return dispatch(device, pipelines[0], groups(gfx::k_sky_transmittance_width),
                    groups(gfx::k_sky_transmittance_height), &address, 8, error) &&
           dispatch(device, pipelines[1], groups(gfx::k_sky_multiscatter_size),
                    groups(gfx::k_sky_multiscatter_size), &address, 8, error) &&
           dispatch(device, pipelines[2], groups(gfx::k_sky_view_width),
                    groups(gfx::k_sky_view_height), &address, 8, error) &&
           dispatch(device, pipelines[3], groups(gfx::k_sky_aerial_width),
                    groups(gfx::k_sky_aerial_height), &address, 8, error) &&
           dispatch(device, pipelines[4], 1, 1, &address, 8, error);
  }
};

// The largest disagreement of a GPU table with the mirror's, relative to the larger of the texel's
// own magnitude and a floor a millionth of the table's largest value (where a texel is that much
// smaller than the table's brightest, a relative error means nothing on any display).
struct TableError {
  f64 worst = 0.0;  // the largest relative disagreement
  usize at = 0;     // the texel it is at
  f64 gpu = 0.0;    // and the two values there
  f64 cpu = 0.0;
  usize over = 0;  // texels past 1e-3
  usize count = 0;
};

TableError table_error(const gfx::BufferResource& gpu, const sref::Table& cpu, bool alpha = false) {
  const auto* texels = static_cast<const f32*>(gpu.mapped);
  f64 most = 0.0;
  for (const sref::Dvec4& t : cpu.texels)
    most = std::max({most, t.x, t.y, t.z});
  const f64 floor = std::max(most * 1e-6, 1e-30);
  TableError e;
  e.count = cpu.texels.size();
  for (usize i = 0; i < cpu.texels.size(); ++i) {
    const f64 c[4] = {cpu.texels[i].x, cpu.texels[i].y, cpu.texels[i].z, cpu.texels[i].w};
    f64 texel = 0.0;
    for (u32 k = 0; k < (alpha ? 4u : 3u); ++k) {
      const f64 g = static_cast<f64>(texels[i * 4 + k]);
      const f64 r = std::fabs(g - c[k]) / std::max(std::fabs(c[k]), floor);
      texel = std::max(texel, r);
      if (r > e.worst) {
        e.worst = r;
        e.at = i;
        e.gpu = g;
        e.cpu = c[k];
      }
    }
    if (texel > 1e-3) ++e.over;
  }
  return e;
}

std::ostream& operator<<(std::ostream& out, const TableError& e) {
  return out << e.worst << " (texel " << e.at << ": gpu " << e.gpu << ", cpu " << e.cpu << "; "
             << e.over << " of " << e.count << " past 1e-3)";
}

f64 rel(sref::Dvec3 cpu, const f32* gpu, f64 floor) {
  const f64 c[3] = {cpu.x, cpu.y, cpu.z};
  f64 worst = 0.0;
  for (u32 k = 0; k < 3; ++k) {
    worst = std::max(worst,
                     std::fabs(static_cast<f64>(gpu[k]) - c[k]) / std::max(std::fabs(c[k]), floor));
  }
  return worst;
}

}  // namespace

TEST_CASE("sky: the mirror's model does what a sky does") {
  const gfx::SkyParams noon_params = earth(90.0, 60.0, 270.0, -30.0, gfx::k_sky_sun_disc);
  const sref::Sky noon = sref::from_params(noon_params);
  // The transmittance table's rays round-trip, and its ends are the ray straight up and the ray
  // grazing the planet.
  for (const f64 mu : {1.0, 0.5, 0.1, 0.0, -0.05}) {
    const f64 h = 0.3;
    const f64 r = noon.bottom + h;
    if (sref::hits_ground(noon.bottom, r, h, mu)) continue;
    f64 u, v, r2, h2, mu2;
    sref::transmittance_uv(noon, r, h, mu, u, v);
    sref::transmittance_ray(noon, u, v, r2, h2, mu2);
    CHECK(std::fabs(mu2 - mu) < 1e-6);
    CHECK(std::fabs(h2 - h) < 1e-6);
  }
  // Straight up from the ground the air takes a few percent of red and a quarter of blue; at the
  // horizon it takes nearly everything, blue first.
  const sref::Dvec3 up = sref::transmittance_integral(noon, noon.bottom, 0.0, 1.0, 500);
  CHECK(up.x > 0.8);
  CHECK(up.z < up.y);
  CHECK(up.y < up.x);
  const sref::Dvec3 low = sref::transmittance_integral(noon, noon.bottom + 0.001, 0.001, 0.02, 500);
  CHECK(low.z < 0.02);
  CHECK(low.x > low.z * 10.0);
  // The table's 256 steps are within a tenth of a percent of 4,000 straight up, where a haze's
  // 1.2 km scale height is steepest along the ray. Hillaire's 40 were 3% short in blue at a
  // turbidity of 2.5: the midpoint of a 2.5 km step reads the Mie layer at a third of its mean.
  const sref::Dvec3 fine = sref::transmittance_integral(noon, noon.bottom, 0.0, 1.0, 4000);
  const sref::Dvec3 table = sref::transmittance_integral(noon, noon.bottom, 0.0, 1.0, 256);
  CHECK(std::fabs(table.z - fine.z) / fine.z < 0.001);
  const sref::Dvec3 hillaire = sref::transmittance_integral(noon, noon.bottom, 0.0, 1.0, 40);
  CHECK(std::fabs(hillaire.z - fine.z) / fine.z > 0.02);
  // The exposure rule: 2.5 * 2^15 lux is EV100 15; below the knee half a stop a stop.
  CHECK(gfx::sky_exposure_ev100(2.5 * 32768.0, 0.0, 10.0, 0.5) == doctest::Approx(15.0));
  CHECK(gfx::sky_exposure_ev100(10.0 / 16.0, 0.0, 10.0, 0.5) ==
        doctest::Approx(std::log2(4.0) - 2.0));
  CHECK(gfx::sky_exposure_ev100(2.5 * 32768.0, 1.0, 10.0, 0.5) == doctest::Approx(14.0));
  // A white Lambertian surface lit by any E above the knee is exposed to 1, and an 18% card to the
  // display's middle grey; below the knee, a stop of darkness darkens it by half a stop.
  for (const f64 lux : {100000.0, 1000.0}) {
    const f64 scale = gfx::sky_exposure_scale(gfx::sky_exposure_ev100(lux, 0.0, 10.0, 0.5));
    CHECK(scale * (lux / static_cast<f64>(gfx::k_sky_sun_lux)) / k_pi64 == doctest::Approx(1.0));
  }
  const f64 dim = 10.0 / 4.0;  // two stops under the knee
  const f64 scale = gfx::sky_exposure_scale(gfx::sky_exposure_ev100(dim, 0.0, 10.0, 0.5));
  CHECK(scale * (dim / static_cast<f64>(gfx::k_sky_sun_lux)) / k_pi64 == doctest::Approx(0.5));
}

TEST_CASE(
    "sky: every table, the frame's sums and the functions the resolve calls, on the GPU "
    "against the mirror") {
  gfx::Device device;
  std::string error;
  if (!gfx_test::open_device(device)) return;
  GpuSky gpu;
  REQUIRE_MESSAGE(gpu.create(device, &error), error);

  struct Case {
    const char* name;
    gfx::SkyParams params;
  };
  const u32 all = gfx::k_sky_sun_disc | gfx::k_sky_moon | gfx::k_sky_stars;
  const Case cases[] = {
      {"noon", earth(100.0, 62.0, 300.0, -20.0, all)},
      {"sunset", earth(265.0, 2.0, 80.0, 10.0, all)},
      {"twilight", earth(275.0, -5.0, 60.0, 25.0, all)},
      {"moonlit night", earth(10.0, -40.0, 190.0, 45.0, all)},
  };
  for (const Case& c : cases) {
    const std::string name = c.name;
    CAPTURE(name);
    const TestStars stars = test_stars(c.params);
    REQUIRE_MESSAGE(gpu.build(device, c.params, stars, &error), error);
    const sref::Sky sky = sref::from_params(c.params);
    const sref::Tables tables = sref::build_tables(sky);
    // The tables, texel by texel.
    const TableError e_t = table_error(gpu.transmittance, tables.transmittance);
    const TableError e_ms = table_error(gpu.multiscatter, tables.multiscatter);
    const TableError e_view = table_error(gpu.sky_view, tables.sky_view);
    const TableError e_air = table_error(gpu.aerial, tables.aerial, true);
    MESSAGE(name << " tables: transmittance " << e_t << "\n  Psi_ms " << e_ms << "\n  sky view "
                 << e_view << "\n  aerial " << e_air);
    CHECK(e_t.worst < 1e-3);
    CHECK(e_ms.worst < 2e-3);
    CHECK(e_view.worst < 2e-3);
    CHECK(e_air.worst < 2e-3);
    // The frame's sums.
    const sref::Frame frame = sref::frame(sky, tables);
    const auto* f = static_cast<const f32*>(gpu.frame.mapped);
    f64 sh_floor = 0.0;
    for (const sref::Dvec3& s : frame.sh)
      sh_floor = std::max({sh_floor, s.x, s.y, s.z});
    f64 e_sh = 0.0;
    for (u32 k = 0; k < 9; ++k)
      e_sh = std::max(e_sh, rel(frame.sh[k], f + 4 * k, sh_floor * 1e-3));
    const f64 e_ground = rel(frame.ground, f + 36, 1e-30);
    const f64 e_sun = rel(frame.sun_ground, f + 40, 1e-12);
    const f64 e_exposure = std::fabs(static_cast<f64>(f[48]) - frame.exposure) / frame.exposure;
    CHECK(e_sh < 2e-3);
    CHECK(e_ground < 2e-3);
    CHECK(e_sun < 1e-3);
    CHECK(e_exposure < 2e-3);
    CHECK(std::fabs(static_cast<f64>(f[49]) - frame.ev) < 2e-3);
    // The functions the resolve and the reference call, at directions of every kind: the zenith,
    // round the horizon, below it, at the sun's disc and its limb, at the moon's, and at each star.
    std::vector<Vec4> q;
    std::vector<sref::Dvec3> dirs;
    std::vector<sref::Dvec3> points;
    std::vector<f64> distances;
    auto add = [&](Vec3 d, Vec3 point, f32 distance) {
      d = normalize(d);
      q.push_back(Vec4{d.x, d.y, d.z, 1.0e-3f});
      q.push_back(Vec4{point.x, point.y, point.z, distance});
      dirs.push_back(brdf_ref::dvec3(d));
      points.push_back(brdf_ref::dvec3(point));
      distances.push_back(static_cast<f64>(distance));
    };
    for (u32 i = 0; i < 12; ++i) {
      const f32 a = 0.5236f * static_cast<f32>(i);
      for (const f32 el : {-0.3f, -0.02f, 0.01f, 0.05f, 0.3f, 1.2f}) {
        add(Vec3{std::cos(a) * std::cos(el), std::sin(el), std::sin(a) * std::cos(el)},
            Vec3{50.0f * static_cast<f32>(i), 0.5f + 20.0f * static_cast<f32>(i % 3), 0.0f},
            10.0f + 400.0f * static_cast<f32>(i));
      }
    }
    const Vec3 sun{c.params.sun.x, c.params.sun.y, c.params.sun.z};
    const Vec3 moon{c.params.moon.x, c.params.moon.y, c.params.moon.z};
    add(sun, Vec3{0.0f, 2.0f, 0.0f}, 3000.0f);
    add(normalize(sun + normalize(cross(sun, Vec3{0.0f, 1.0f, 0.0f})) * 0.004f), Vec3{}, 12000.0f);
    add(moon, Vec3{0.0f, 150.0f, 0.0f}, 800.0f);
    add(normalize(moon + normalize(cross(moon, Vec3{0.0f, 1.0f, 0.0f})) * 0.003f),
        Vec3{0.0f, 0.0f, 0.0f}, 31000.0f);
    const Vec3 cx{c.params.celestial_x.x, c.params.celestial_x.y, c.params.celestial_x.z};
    const Vec3 cy{c.params.celestial_y.x, c.params.celestial_y.y, c.params.celestial_y.z};
    const Vec3 cz{c.params.celestial_z.x, c.params.celestial_z.y, c.params.celestial_z.z};
    for (u32 s = 0; s < 3; ++s) {
      const Vec4 d = stars.list[2 * s];
      const Vec3 world = cx * d.x + cy * d.y + cz * d.z;
      add(world, Vec3{}, 100.0f);
      add(normalize(world + Vec3{0.0f, 7.0e-4f, 0.0f}), Vec3{}, 100.0f);  // a pixel off it
    }
    const u32 count = static_cast<u32>(dirs.size());
    std::memcpy(gpu.queries.mapped, q.data(), q.size() * sizeof(Vec4));
    struct Probe {
      u64 sky, queries, out;
      u32 count, pad;
    } probe{gpu.params.address, gpu.queries.address, gpu.out.address, count, 0};
    REQUIRE_MESSAGE(
        gpu.dispatch(device, gpu.probe, (count + 63) / 64, 1, &probe, sizeof(probe), &error),
        error);
    const auto* out = static_cast<const f32*>(gpu.out.mapped);
    f64 e_bg = 0.0, e_amb = 0.0, e_ins = 0.0, e_tr = 0.0, e_light = 0.0;
    f64 disc_seen = 0.0;
    u32 worst_bg = 0;
    // What a display shows of a radiance at the frame's exposure, in bytes: the resolve's own
    // transfer, unrounded — the scale every picture the sky makes is judged on.
    auto shown = [&](f64 radiance) {
      const f64 exposed = sref::tone(radiance * frame.exposure, sky.shoulder);
      const f64 v = std::pow(std::max(exposed, 0.0), 1.0 / 2.2);
      return std::min(v, 1.0) * 255.0;
    };
    auto byte_error = [&](sref::Dvec3 cpu, const f32* g) {
      return std::max({std::fabs(shown(cpu.x) - shown(static_cast<f64>(g[0]))),
                       std::fabs(shown(cpu.y) - shown(static_cast<f64>(g[1]))),
                       std::fabs(shown(cpu.z) - shown(static_cast<f64>(g[2])))});
    };
    for (u32 i = 0; i < count; ++i) {
      const sref::Dvec3 bg = sref::background(sky, tables, &stars.mirror, dirs[i], 1.0e-3);
      const sref::Dvec3 amb = sref::ambient(frame, dirs[i]);
      sref::Dvec3 ins;
      f64 tr;
      sref::aerial(sky, tables, dirs[i], distances[i], ins, tr);
      const sref::Dvec3 light =
          sref::light_at(sky, tables, points[i], dirs[i], sky.sun_illuminance);
      const f64 b = byte_error(bg, out + 20 * i);
      if (b > e_bg) {
        e_bg = b;
        worst_bg = i;
      }
      e_amb = std::max(e_amb, byte_error(amb, out + 20 * i + 4));
      e_ins = std::max(e_ins, byte_error(ins, out + 20 * i + 8));
      e_tr = std::max(e_tr, std::fabs(static_cast<f64>(out[20 * i + 11]) - tr));
      e_light = std::max(e_light, rel(light, out + 20 * i + 12, 1e-9));
      disc_seen = std::max(disc_seen, bg.x);
    }
    CHECK(e_bg < 0.05);
    CHECK(e_amb < 0.05);
    CHECK(e_ins < 0.05);
    CHECK(e_tr < 1e-4);
    CHECK(e_light < 1e-3);
    MESSAGE(name << " worst background, of a byte " << e_bg << ": direction (" << dirs[worst_bg].x
                 << ", " << dirs[worst_bg].y << ", " << dirs[worst_bg].z << ") gpu "
                 << out[20 * worst_bg] << " cpu "
                 << sref::background(sky, tables, &stars.mirror, dirs[worst_bg], 1.0e-3).x
                 << "; the table alone gpu " << out[20 * worst_bg + 16] << " cpu "
                 << sref::view_radiance(sky, tables, dirs[worst_bg]).x << " at gpu uv ("
                 << out[20 * worst_bg + 17] << ", " << out[20 * worst_bg + 18] << "); stars gpu "
                 << out[20 * worst_bg + 19]);
    MESSAGE(name << ": sums " << e_sh << " " << e_ground << " " << e_sun << " exposure "
                 << e_exposure << " (EV100 " << frame.ev << ", " << frame.lux << " lux); functions "
                 << e_bg << " " << e_amb << " " << e_ins << " " << e_tr << " " << e_light
                 << "; brightest background " << disc_seen);
  }
  gpu.destroy(device);
}
