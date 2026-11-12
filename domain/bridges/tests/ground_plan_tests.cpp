// A bridges entry's approaches planned through the host's hook against the same approaches pasted
// into the scene (roadmap G186; docs/subsystems/bridges.md, "The ground and the neighbours";
// scene_gen.md, "What an entry plans for the ground"): the edits the hook hands are the edits the
// command's JSON reads back as, field for field, and the two tile sets they build are the same
// ground to the bit. Compiled where the tile set is (a test-only dependency).

#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/bridges/bridges.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/tileset.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <schemas/scene.h>
#include <string>

using namespace engine;
using namespace engine::bridges;

namespace {

// A stream across a level valley floor at 2 m, `width` wide and `depth` deep, at (x, z): the
// crossing's profile is that floor with the channel in it (`bridges_tests.cpp`'s, at a height).
Crossing crossing_at(i32 width, i32 depth, i64 x, i64 z) {
  Crossing c;
  c.station = 12;
  c.x_mm = x;
  c.z_mm = z;
  c.dir_x = 0;
  c.dir_z = 10000;
  c.width_mm = static_cast<u32>(width);
  c.bed_mm = 2000 - depth;
  c.water_mm = c.bed_mm + depth / 2;
  c.tread_mm = 600;
  c.profile_step_mm = 250;
  const i32 n = (width / 2 + 6000) / 250 * 2 + 1;
  for (i32 i = 0; i < n; ++i) {
    const i64 px = static_cast<i64>(i - n / 2) * 250, ax = px < 0 ? -px : px;
    c.profile_mm.push_back(2 * ax < width ? c.bed_mm + static_cast<i64>(depth) * 2 * ax / width
                                          : 2000);
  }
  return c;
}

scene::Terrain terrain_with(const Vector<scene::TerrainEdit>& edits) {
  scene::Terrain t;
  t.provider = "tiles";
  t.extent = 32;
  t.tiles = scene::TerrainTileSet{};
  t.tiles->heightfield = "floor.json";
  t.tiles->ddc = "ddc";
  t.tiles->cells = 16;
  t.tiles->edits = edits;
  return t;
}

bool make_ground(const scene::Terrain& t, const std::string& dir, scene_gen::GroundProvider& out,
                 std::string& error) {
  scene_gen::Context context;
  context.dir = dir;
  const scene_gen::GroundProviderDesc* desc =
      scene_gen::GeneratorRegistry::global().find_ground("tiles");
  REQUIRE(desc != nullptr);
  return desc->make(t, context, out, &error) && out.valid();
}

}  // namespace

TEST_CASE("bridges ground plan: the approaches planned through the hook are the ones pasted") {
  test::TempDir tmp("bridges_ground_plan");
  std::string error;
  // A valley floor at 2 m: 161 points at 250 mm, 40 m a side from the origin.
  tileset::Heightfield field = tileset::synth_heightfield(tileset::SynthKind::flat, 161, 250);
  for (f32& h : field.data)
    h = 2.0f;
  REQUIRE_MESSAGE(tileset::write_heightfield(tmp.file("floor.json"), field, error), error);

  // A stringer bridge over an 8 m stream: past the minor span, so its deck keeps the flood
  // clearance and its approaches are fills (bridges.md, "The deck's height").
  Layer layer;
  layer.seed = 3;
  layer.approach_grade_permille = 200;
  layer.crossings.push_back(crossing_at(8000, 900, 20000, 20000));
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement("bridges");
  REQUIRE(desc != nullptr);
  const std::string root = tmp.path();
  scene_gen::Context context;
  context.ddc = root;
  void* state = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(layer), context, &state, &error), error);
  scene_gen::GroundPlan planned;
  REQUIRE_MESSAGE(desc->plan_ground(state, context, planned, &error), error);
  desc->close(state);
  REQUIRE(planned.edits.size() == 2);

  // What a person pastes: the command's `edits`, written as text and read back as the scene's.
  const std::string text = write_json(approach_edits_json(layer, layer.approach_grade_permille));
  JsonValue parsed;
  REQUIRE(parse_json(text, parsed).ok);
  Vector<scene::TerrainEdit> pasted;
  for (const JsonValue& json : parsed.as_array()) {
    scene::TerrainEdit edit;
    schema::ReadContext reader;
    REQUIRE(schema::from_json(edit, json, reader));
    REQUIRE(reader.ok());
    pasted.push_back(std::move(edit));
  }
  REQUIRE(pasted.size() == planned.edits.size());
  for (u32 k = 0; k < pasted.size(); ++k)
    CHECK(write_json(schema::to_json(pasted[k])) == write_json(schema::to_json(planned.edits[k])));

  // The two grounds, and the floor with no approach.
  scene_gen::GroundProvider a, b, bare;
  REQUIRE_MESSAGE(make_ground(terrain_with(planned.edits), tmp.path(), a, error), error);
  REQUIRE_MESSAGE(make_ground(terrain_with(pasted), tmp.path(), b, error), error);
  REQUIRE_MESSAGE(make_ground(terrain_with({}), tmp.path(), bare, error), error);
  u32 raised = 0, points = 0;
  for (i64 z = 8000; z <= 32000; z += 125)
    for (i64 x = 16000; x <= 24000; x += 125) {
      const f64 ha = a.view().surface_mm(x, z), hb = b.view().surface_mm(x, z);
      CHECK(std::memcmp(&ha, &hb, sizeof(f64)) == 0);
      raised += ha > bare.view().surface_mm(x, z) + 0.05 ? 1u : 0u;  // metres, at mm points
      ++points;
    }
  INFO("points raised by the approaches: " << raised << " of " << points);
  CHECK(raised > 200);
}
