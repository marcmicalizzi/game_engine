// Water depths beside static streamed terrain (W67): the same provider supplies the renderer's
// lattice, placement/wading ground view and the depth-map builder. No GPU is needed.
#include <domain/tileset/tileset.h>
#include <systems/renderer/terrain.h>
#include <systems/water/water.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace engine;

TEST_CASE("water: authored terrain depths agree with the native streamed surface") {
  const test::TempDir tmp("water_tileset");
  const auto field =
      tileset::synth_heightfield(tileset::SynthKind::basin, 129, 500, 5, -32000, -32000);
  std::string error;
  REQUIRE(tileset::write_heightfield(tmp.file("bowl.json"), field, error));
  tileset::BuildOptions options;
  options.cells = 16;
  options.far_levels = 0;
  tileset::BuildResult built;
  REQUIRE(tileset::ensure_tileset(tmp.file("bowl.json"), options, tmp.file("set"), "", false, built,
                                  error));
  renderer::TerrainDesc desc;
  desc.provider = "tiles";
  desc.tiles.emplace();
  desc.tiles->manifest = built.manifest;
  renderer::TerrainSampler sampler(desc);
  REQUIRE_MESSAGE(sampler.ok(), sampler.error());
  const auto ground = sampler.provider().view();
  const auto source = sampler.provider().tiles();
  water::Body body;
  body.radius_m = 16;
  body.shore_m = 2;
  const std::span<const water::Body> bodies(&body, 1);
  Vector<f32> heights(33 * 33);
  REQUIRE(
      source.heights(1e9, 500, -16, -16, 33, 33, std::span<f32>(heights.data(), heights.size())));
  for (i32 z = 0; z < 33; ++z) {
    for (i32 x = 0; x < 33; ++x) {
      const WorldPos at{static_cast<f64>(x - 16) * 0.5, 0, static_cast<f64>(z - 16) * 0.5};
      const f32 h = heights[static_cast<u32>(z * 33 + x)];
      CHECK(ground.surface(at) == static_cast<f64>(h));
      CHECK(water::depth_at(bodies, ground, at) == std::max(0.0f, -h));
    }
  }
  water::Mesh mesh;
  water::build_mesh(body, ground, mesh);
  REQUIRE_FALSE(mesh.positions.empty());
  for (u32 k = 0; k < mesh.positions.size(); ++k) {
    const Vec3 p = mesh.positions[k];
    const f32 expected = static_cast<f32>(
        std::max(0.0, -ground.surface(WorldPos{static_cast<f64>(p.x), 0, static_cast<f64>(p.z)})));
    CHECK(mesh.uvs[k].y == expected);
  }
  water::DepthMap map;
  water::build_depth_map(body, mesh, ground, map);
  REQUIRE(map.size > 0);
  const u32 c = map.size / 2;
  const WorldPos at{static_cast<f64>(map.origin.x + static_cast<f32>(c) * map.texel_m), 0,
                    static_cast<f64>(map.origin.y + static_cast<f32>(c) * map.texel_m)};
  const f32 stored = static_cast<f32>(map.texels[c * map.size + c]) / 65535.0f * map.depth_max;
  CHECK(std::fabs(stored - water::depth_at(bodies, ground, at)) <= map.depth_max / 65535.0f);
}
