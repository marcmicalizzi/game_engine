// What a site sample costs (site.h; docs/subsystems/scene_gen.md, "The site"; G144): the form
// (five ground queries and the arithmetic), the distances to a network of channels and bodies, and
// the two together, on a ground of products and sums that costs about what the renderer's waves do
// per point. Items are samples.
#include <domain/scene_gen/scene_gen.h>
#include <foundation/bench/bench.h>

using namespace engine;
using namespace engine::scene_gen;

namespace {

f64 rough_height(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0;
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  const f64 u = x / 100.0;
  const f64 v = z / 37.0;
  return 300.0 - 120.0 * u * u + 3.0 * u * v - 0.8 * v * v * v + 0.02 * x * z / (1.0 + u * u);
}

// A stream network across 2 km: 40 channels of 40 segments 5 m long, and 8 lakes.
const Site& network_site() {
  static const Site site = [] {
    Vector<scene::WaterChannel> channels;
    for (u32 c = 0; c < 40; ++c) {
      scene::WaterChannel ch;
      for (u32 i = 0; i <= 40; ++i) {
        const f64 x = -1000.0 + 50.0 * static_cast<f64>(c) + 0.3 * static_cast<f64>(i);
        const f64 z = -1000.0 + 5.0 * static_cast<f64>(i) + 25.0 * static_cast<f64>(c);
        ch.points.push_back(WorldPos{x, 0.0, z});
        ch.width_m.push_back(2.0f + 0.05f * static_cast<f32>(i));
      }
      channels.push_back(ch);
    }
    Vector<scene::WaterBody> bodies;
    for (u32 b = 0; b < 8; ++b) {
      scene::WaterBody body;
      scene::WaterCircle circle;
      circle.center = WorldPos{-800.0 + 200.0 * static_cast<f64>(b), 0.0, 300.0};
      circle.radius_m = 30.0f;
      body.shape.circle = circle;
      bodies.push_back(body);
    }
    Site s;
    s.build(46.5, {bodies.data(), bodies.size()}, {channels.data(), channels.size()});
    return s;
  }();
  return site;
}

void run(bench::State& state, u32 fields) {
  const Site& site = network_site();
  Ground ground;
  ground.surface_fn = &rough_height;
  i64 at = 0;
  state.set_items(1024);
  while (state.keep_running()) {
    f64 sum = 0.0;
    for (u32 i = 0; i < 1024; ++i) {
      at = (at + 7919 * 1000 + 13) % 2000000;
      const SiteSample s = site.sample_mm(ground, at - 1000000, (at * 3) % 2000000 - 1000000, fields);
      sum += s.slope + s.water_m() + s.heat_load;
    }
    bench::keep(sum);
  }
}

}  // namespace

ENGINE_BENCH(site_form, "scene_gen.site.form") { run(state, k_site_form); }
ENGINE_BENCH(site_water, "scene_gen.site.water") { run(state, k_site_water); }
ENGINE_BENCH(site_all, "scene_gen.site.all") { run(state, k_site_all); }
