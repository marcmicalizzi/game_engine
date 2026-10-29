// The scene generator "cliffs" (docs/subsystems/rocks.md, "The cliffs generator"): the tracer on
// synthetic ground — a steep band found where it is and nowhere on flat or gentle ground — the
// faces' identity whichever blocks and tiles ask, `tile` over every tile being `expand`, an
// earlier footprint honoured, and the faces it writes being rocks that solve.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/rocks/cliffs.h>
#include <domain/rocks/rocks.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace engine;
using namespace engine::rocks;

namespace {

// An escarpment along z: flat at 0 for x < 100, rising 30 m over 20 m (56°), flat at 30 beyond.
f64 escarpment(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0;
  (void)z_mm;
  const f64 t = x < 100.0 ? 0.0 : (x > 120.0 ? 1.0 : (x - 100.0) / 20.0);
  return 30.0 * t;
}
f64 flat(const void*, i64, i64) noexcept { return 5.0; }
// A 30° slope everywhere: steep for walking, not a cliff.
f64 gentle(const void*, i64 x_mm, i64) noexcept { return 0.577 * static_cast<f64>(x_mm) / 1000.0; }

scene_gen::Ground ground_of(scene_gen::HeightFn fn) {
  scene_gen::Ground g;
  g.surface_fn = fn;
  return g;
}

CliffLayer layer_over(f64 x0, f64 z0, f64 x1, f64 z1) {
  CliffLayer layer;
  layer.name = "test";
  layer.region_min[0] = x0;
  layer.region_min[1] = z0;
  layer.region_max[0] = x1;
  layer.region_max[1] = z1;
  layer.face.triangle_budget = 20000;
  layer.face.talus_blocks = 4;
  return layer;
}

JsonValue params_of(const CliffLayer& layer) { return schema::to_json(layer); }

struct Opened {
  const scene_gen::PlacementGeneratorDesc* desc = nullptr;
  void* state = nullptr;
  ~Opened() {
    if (desc != nullptr && state != nullptr) desc->close(state);
  }
};

}  // namespace

TEST_CASE("cliffs generator: the foot of a steep band is traced, and flat ground has none") {
  CliffLayer layer = layer_over(0, 0, 256, 256);
  Vector<CliffFace> faces;
  std::string error;
  REQUIRE_MESSAGE(trace_cliff_block(layer, ground_of(&escarpment), 0, 0, faces, &error), error);
  REQUIRE(faces.size() == 1);
  const CliffFace& face = faces[0];
  REQUIRE(face.cliff.ground.has_value());
  CHECK(face.cliff.ground->heights.size() == face.cliff.ground->width * face.cliff.ground->depth);
  const Vector<CliffPoint>& p = face.cliff.points;
  MESSAGE("one face at x " << static_cast<f64>(face.x_mm) / 1000.0 << ", " << face.length
                           << " m long, " << face.mean_height << " m high, batter "
                           << static_cast<f64>(*p[p.size() / 2].batter));
  // Along z, the full block, standing off downhill of the foot at x 100-104.
  CHECK(face.length > 240.0);
  f64 x_lo = 1e9, x_hi = -1e9;
  for (const CliffPoint& q : p) {
    x_lo = std::min(x_lo, static_cast<f64>(face.x_mm) / 1000.0 + static_cast<f64>(q.x));
    x_hi = std::max(x_hi, static_cast<f64>(face.x_mm) / 1000.0 + static_cast<f64>(q.x));
  }
  CHECK(x_lo > 94.0);
  CHECK(x_hi < 104.0);
  // Its height is the band's 30 m less the half metre under the brow, near enough; it leans back
  // by about the band's run over its rise.
  CHECK(face.mean_height > 24.0);
  CHECK(face.mean_height < 31.0);
  CHECK(*p[p.size() / 2].batter > 0.4f);
  CHECK(*p[p.size() / 2].batter < 1.1f);
  // It looks downhill, to -x: the line's right.
  const f32 tx = p[p.size() - 1].x - p[0].x;
  const f32 tz = p[p.size() - 1].z - p[0].z;
  CHECK(-tz < 0.0f);  // the normal (-tz, 0, tx)'s x
  CHECK(std::fabs(tx) < 0.05f * std::fabs(tz));
  // And it is a rock: its recipe solves, closed.
  assets::MeshData mesh;
  REQUIRE_MESSAGE(solve_cliff(face.cliff, mesh, nullptr, &error), error);

  Vector<CliffFace> none;
  REQUIRE(trace_cliff_block(layer, ground_of(&flat), 0, 0, none, &error));
  CHECK(none.empty());
  REQUIRE(trace_cliff_block(layer, ground_of(&gentle), 0, 0, none, &error));
  CHECK(none.empty());
  // A block with no steep ground in it, beside one with.
  REQUIRE(trace_cliff_block(layer, ground_of(&escarpment), 1, 0, none, &error));
  CHECK(none.empty());
}

TEST_CASE("cliffs generator: a face is the same whichever blocks and tiles ask") {
  test::TempDir scratch("cliffs-identity");
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement(k_cliffs_generator);
  REQUIRE(desc != nullptr);
  scene_gen::Context context;
  context.ground = ground_of(&escarpment);
  context.tile_size = 64.0f;
  const std::string dir = scratch.path();
  context.dir = dir;
  context.ddc = dir;
  // The block traced alone, and the same block inside a region of four.
  Vector<CliffFace> alone;
  std::string error;
  REQUIRE(trace_cliff_block(layer_over(0, 0, 256, 256), context.ground, 0, 1, alone, &error));
  REQUIRE(alone.size() == 1);

  CliffLayer wide = layer_over(-256, 0, 256, 512);
  Opened whole;
  whole.desc = desc;
  REQUIRE_MESSAGE(desc->open(params_of(wide), context, &whole.state, &error), error);
  scene_gen::Placements expanded;
  REQUIRE(desc->expand(whole.state, context, expanded, &error));
  CHECK(expanded.things == 2);  // the band crosses two blocks along z, one face each
  bool found = false;
  for (const scene_gen::Placement& p : expanded.instances) {
    found = found || (std::fabs(p.position.x - static_cast<f64>(alone[0].x_mm) / 1000.0) < 1e-9 &&
                      std::fabs(p.position.z - static_cast<f64>(alone[0].z_mm) / 1000.0) < 1e-9);
  }
  CHECK(found);
  // Every tile, in reverse order: the same instances and the same recipes as the whole read.
  Vector<scene_gen::PlacementMesh> resident;
  REQUIRE(desc->meshes(whole.state, context, resident, &error));
  std::vector<std::string> from_tiles;
  for (i32 tz = 8; tz-- > 0;) {
    for (i32 tx = 4; tx-- > -4;) {
      scene_gen::Placements part;
      REQUIRE(desc->tile(whole.state, scene_gen::TileCoord{tx, tz}, context, part, &error));
      CHECK(desc->occupies(whole.state, scene_gen::TileCoord{tx, tz}) == !part.instances.empty());
      for (const scene_gen::Placement& p : part.instances)
        from_tiles.push_back(resident[p.mesh].path + "@" + std::to_string(p.position.x) + "," +
                             std::to_string(p.position.z));
    }
  }
  std::vector<std::string> from_whole;
  for (const scene_gen::Placement& p : expanded.instances)
    from_whole.push_back(expanded.meshes[p.mesh].path + "@" + std::to_string(p.position.x) + "," +
                         std::to_string(p.position.z));
  std::sort(from_tiles.begin(), from_tiles.end());
  std::sort(from_whole.begin(), from_whole.end());
  CHECK(from_tiles == from_whole);
  // Its recipes are on disk under the root it was handed, named by their parameters.
  for (const scene_gen::PlacementMesh& mesh : expanded.meshes) {
    CHECK(io::exists(mesh.path));
    CHECK(mesh.path.find("/cliffs/cliff-") != std::string::npos);
  }
}

TEST_CASE("cliffs generator: an earlier footprint on a face's line clears that face") {
  test::TempDir scratch("cliffs-footprint");
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement(k_cliffs_generator);
  REQUIRE(desc != nullptr);
  scene_gen::Context context;
  context.ground = ground_of(&escarpment);
  const std::string dir = scratch.path();
  context.dir = dir;
  context.ddc = dir;
  std::string error;
  Opened opened;
  opened.desc = desc;
  REQUIRE_MESSAGE(desc->open(params_of(layer_over(0, 0, 256, 512)), context, &opened.state, &error),
                  error);
  scene_gen::Placements before;
  REQUIRE(desc->expand(opened.state, context, before, &error));
  REQUIRE(before.things == 2);
  // A building's footprint standing on the second face's foot, at z 400.
  const scene_gen::Footprint footprint{99000, 400000, 5.0};
  context.earlier = std::span<const scene_gen::Footprint>(&footprint, 1);
  scene_gen::Placements after;
  REQUIRE(desc->expand(opened.state, context, after, &error));
  CHECK(after.things == 1);
  REQUIRE(after.instances.size() == 1);
  CHECK(after.instances[0].position.z < 256.0);
  // And one far from any face clears nothing.
  const scene_gen::Footprint far{180000, 400000, 5.0};
  context.earlier = std::span<const scene_gen::Footprint>(&far, 1);
  scene_gen::Placements kept;
  REQUIRE(desc->expand(opened.state, context, kept, &error));
  CHECK(kept.things == 2);
}

TEST_CASE("cliffs generator: a layer out of range is refused with its field's name") {
  CliffLayer layer = layer_over(0, 0, 256, 256);
  std::string error;
  layer.min_slope_deg = 10.0f;
  CHECK_FALSE(cliff_layer_in_range(layer, error));
  CHECK(error.find("engine.rocks.CliffLayer.min_slope_deg") != std::string::npos);
  layer = layer_over(0, 0, 256, 256);
  layer.block_m = 250.0f;
  layer.spacing_m = 3.0f;
  CHECK_FALSE(cliff_layer_in_range(layer, error));
  CHECK(error.find("engine.rocks.CliffLayer.block_m") != std::string::npos);
  layer = layer_over(0, 0, 256, 256);
  layer.face.undercut = 3.0f;
  CHECK_FALSE(cliff_layer_in_range(layer, error));
  CHECK(error.find("engine.rocks.Cliff.undercut") != std::string::npos);
  layer = layer_over(10, 0, 0, 256);
  CHECK_FALSE(cliff_layer_in_range(layer, error));
  CHECK(error.find("region_min") != std::string::npos);
}

namespace {

// content/test-scenes/escarpment's ground: `engine-content terrain-synth --kind ridge --size 401
// --spacing-mm 1000 --origin-mm -200000,-200000` under `vertical_scale` 3 — the tile set's
// analytic ridge (domain/tileset, `synth_heightfield`), written out here so the test needs neither
// the capability nor the grid on disk: a ridge 40 m high along a diagonal, falling to nothing 0.4
// of the half-side from its line, on ground rising 6 m across, all three times.
f64 escarpment_ridge(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 u = std::clamp(static_cast<f64>(x_mm) / 200000.0, -1.0, 1.0);
  const f64 v = std::clamp(static_cast<f64>(z_mm) / 200000.0, -1.0, 1.0);
  const f64 d = std::fabs(u + 0.25 * v) / 0.4;
  const f64 t = d < 1.0 ? (1.0 - d * d) : 0.0;
  return 3.0 * (40.0 * t * t + 3.0 * (v + 1.0));
}

}  // namespace

TEST_CASE("cliffs generator: the escarpment scene's layer dresses both flanks of its ridge") {
  const std::string dir = test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/escarpment",
                                          "content/test-scenes/escarpment");
  std::string text;
  if (io::read_file(dir + "/scene.json", text) != io::Status::Ok) {
    MESSAGE("content/test-scenes/escarpment is not here (a bundle without it): skipped");
    return;
  }
  JsonValue scene;
  REQUIRE(parse_json(text, scene).ok);
  const JsonValue* placements = scene.find("placements");
  REQUIRE(placements != nullptr);
  const JsonValue& entry = (*placements)[0];
  REQUIRE(entry.find("params") != nullptr);
  test::TempDir scratch("cliffs-escarpment");
  const scene_gen::PlacementGeneratorDesc* desc =
      scene_gen::GeneratorRegistry::global().find_placement(k_cliffs_generator);
  REQUIRE(desc != nullptr);
  scene_gen::Context context;
  context.ground = ground_of(&escarpment_ridge);
  context.dir = dir;
  context.ddc = scratch.path();
  std::string error;
  Opened opened;
  opened.desc = desc;
  REQUIRE_MESSAGE(desc->open(*entry.find("params"), context, &opened.state, &error), error);
  scene_gen::Placements out;
  REQUIRE(desc->expand(opened.state, context, out, &error));
  u32 east = 0;
  u32 west = 0;
  u32 boulders = 0;
  for (const scene_gen::Placement& p : out.instances) {
    if (p.tag == 1) {
      ++boulders;
      continue;
    }
    // The ridge's line is x = -z / 4: a face east of it looks east, one west of it west.
    (p.position.x > -0.25 * p.position.z ? east : west) += 1;
  }
  MESSAGE(out.things << " faces (" << east << " east, " << west << " west), " << boulders
                     << " boulders at their feet, " << out.meshes.size() << " meshes");
  CHECK(east >= 2);
  CHECK(west >= 2);
  CHECK(boulders > 0);
  // Each face is a recipe that reads back and solves, the first one closed.
  bool first = true;
  for (const scene_gen::PlacementMesh& mesh : out.meshes) {
    if (mesh.path.find("/cliffs/cliff-") == std::string::npos) continue;
    std::string recipe;
    REQUIRE(io::read_file(mesh.path, recipe) == io::Status::Ok);
    RockFile file;
    REQUIRE_MESSAGE(read_rock_file(recipe, file, error), error);
    REQUIRE(file.shape == RockShape::Cliff);
    CliffReport report;
    assets::MeshData solved;
    REQUIRE_MESSAGE(solve_cliff(*file.cliff, solved, &report, &error), error);
    CHECK(report.triangles <= file.cliff->triangle_budget);
    CHECK(report.height_m > 20.0f);
    if (first) {
      MESSAGE("first face: " << report.length_m << " m long, " << report.height_m << " m high, "
                             << report.triangles << " triangles at " << report.cell_m << " m");
      first = false;
    }
  }
}
