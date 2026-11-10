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

namespace {

// A stadium-shaped pit: a flat floor within 5 m of the segment from (88, 128) to (168, 128), then
// walls at 60 degrees to a 40 m rim. Its foot is a closed loop, straight along the sides and a
// 5 m radius round each end.
f64 pit(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0;
  const f64 z = static_cast<f64>(z_mm) / 1000.0 - 128.0;
  const f64 along = std::min(168.0, std::max(88.0, x));
  const f64 d = std::sqrt((x - along) * (x - along) + z * z);
  return d < 5.0 ? 0.0 : std::min(40.0, (d - 5.0) * 1.73);
}

f64 gap_of(const CliffPoint& a, const CliffPoint& b) {
  const f64 dx = static_cast<f64>(b.x) - static_cast<f64>(a.x);
  const f64 dz = static_cast<f64>(b.z) - static_cast<f64>(a.z);
  return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

// The mechanism: a face is stood off downhill of the traced foot along the line's normal, so on the
// inside of a bend tighter than the standoff the offset line folds. At a standoff equal to the
// radius of the stadium pit's rounded ends the vertices of each end's foot land on its centre, and
// near that they come within centimetres of each other while the long sides keep the line well over
// the minimum length. The recipe's validator refuses vertices under 5 cm apart, and one refusal
// fails the whole scene. The sweep passes through that standoff (the foot's radius is only about
// the floor's, the lattice being 2 m), and in this build writes gaps under 5 cm at several of its
// steps without the merge.
TEST_CASE("cliffs generator: a line the stand-off folds is merged to the recipe's vertex gap") {
  u32 skipped_total = 0;
  u32 faces_total = 0;
  u32 most = 0;
  u32 least = 1000000;
  for (u32 step = 0; step <= 140; ++step) {
    CliffLayer layer = layer_over(0, 0, 256, 256);
    layer.spacing_m = 2.0f;
    layer.min_slope_deg = 48.0f;
    layer.standoff_m = 3.5f + 0.025f * static_cast<f32>(step);
    Vector<CliffFace> faces;
    std::string error;
    u32 skipped = 0;
    REQUIRE_MESSAGE(trace_cliff_block(layer, ground_of(&pit), 0, 0, faces, &error, &skipped),
                    error);
    skipped_total += skipped;
    for (const CliffFace& face : faces) {
      ++faces_total;
      std::string why;
      CHECK_MESSAGE(cliff_in_range(face.cliff, why),
                    "standoff " << *layer.standoff_m << ": " << why);
      for (u32 i = 1; i < face.cliff.points.size(); ++i)
        CHECK(gap_of(face.cliff.points[i - 1], face.cliff.points[i]) >= 0.05);
      most = std::max(most, face.cliff.points.size());
      least = std::min(least, face.cliff.points.size());
    }
  }
  MESSAGE("faces " << faces_total << ", skipped " << skipped_total << ", vertices " << least
                   << " to " << most);
  CHECK(faces_total > 0);
  // The resampled line has the same vertices at every standoff, so the standoffs that fold it are
  // the ones that wrote fewer: a vertex dropped for being within 5 cm of the one before it.
  CHECK(least < most);
  // A line that merely folds is merged, never left out.
  CHECK(skipped_total == 0);
}

// ---- faces in the slope (G142; rocks.md, "Faces in the slope") ----------------------------------

namespace {

// A steep band that ends inside the block: 30 m up over a run of 20 m (56 degrees) for z within
// 65 m of 125, the run widening by 3 m a metre past that, so the ground is under 50 degrees from
// about z = 58 down and z = 192 up.
f64 ending_band(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  const f64 past = std::fabs(z - 125.0) - 65.0;
  const f64 run = 20.0 + (past > 0.0 ? 3.0 * past : 0.0);
  const f64 t = x < 100.0 ? 0.0 : (x > 100.0 + run ? 1.0 : (x - 100.0) / run);
  return 30.0 * t;
}
// A fin: the same 30 m rise over 20 m, a top 2 m wide, and the ground falling away behind it at
// 37 degrees, which is not steep enough for a face of its own.
f64 fin(const void*, i64 x_mm, i64) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0;
  if (x < 100.0) return 0.0;
  if (x < 120.0) return 30.0 * (x - 100.0) / 20.0;
  if (x < 122.0) return 30.0;
  return x < 162.0 ? 30.0 - 30.0 * (x - 122.0) / 40.0 : 0.0;
}

scene::Formation level_formation() {
  scene::Formation f;
  f.dip_deg = 8.0f;
  f.azimuth_deg = 40.0f;
  scene::FormationBed sandstone;
  sandstone.name = "sandstone";
  sandstone.thickness_m = 2.0f;
  sandstone.resistance = 0.9f;
  scene::FormationBed shale;
  shale.name = "shale";
  shale.thickness_m = 1.0f;
  shale.resistance = 0.1f;
  f.beds.push_back(sandstone);
  f.beds.push_back(shale);
  return f;
}

// How far a face's top stands over the ground behind it: a top is flat from its crest to its
// back, in front of the slope at first and then in the ground; the worst, over every vertex, of
// the top less the ground from where it goes in to `back` metres behind the crest. 1e9 for a top
// that never goes into the ground at all.
f64 proud_of_ground(const CliffFace& face, scene_gen::HeightFn ground, f64 back) {
  const Vector<CliffPoint>& p = face.cliff.points;
  f64 worst = -1.0e9;
  for (u32 i = 0; i < p.size(); ++i) {
    const CliffPoint& a = p[i > 0 ? i - 1 : 0];
    const CliffPoint& b = p[i + 1 < p.size() ? i + 1 : p.size() - 1];
    const f64 tx = static_cast<f64>(b.x - a.x), tz = static_cast<f64>(b.z - a.z);
    const f64 l = std::sqrt(tx * tx + tz * tz);
    const f64 nx = -tz / l, nz = tx / l;  // the line's right: downhill
    const f64 top = face.y + static_cast<f64>(p[i].y) + static_cast<f64>(*p[i].height);
    const f64 lean = static_cast<f64>(*p[i].batter) * static_cast<f64>(*p[i].height);
    bool entered = false;
    for (f64 d = 0.0; d <= back; d += 0.5) {
      const f64 x =
          static_cast<f64>(face.x_mm) / 1000.0 + static_cast<f64>(p[i].x) - nx * (lean + d);
      const f64 z =
          static_cast<f64>(face.z_mm) / 1000.0 + static_cast<f64>(p[i].z) - nz * (lean + d);
      const f64 g = ground(nullptr, scene_gen::nearest_mm(x), scene_gen::nearest_mm(z));
      entered = entered || g >= top;
      if (entered) worst = std::max(worst, top - g);
    }
    if (!entered) return 1.0e9;
  }
  return worst;
}

}  // namespace

TEST_CASE("cliffs generator: a face carries the layer's formation at its own origin") {
  CliffLayer layer = layer_over(0, 0, 256, 256);
  Vector<CliffFace> plain;
  std::string error;
  REQUIRE(trace_cliff_block(layer, ground_of(&escarpment), 0, 0, plain, &error));
  REQUIRE(plain.size() == 1);
  CHECK_FALSE(plain[0].cliff.formation.has_value());

  layer.formation = level_formation();
  Vector<CliffFace> faces;
  REQUIRE_MESSAGE(trace_cliff_block(layer, ground_of(&escarpment), 0, 0, faces, &error), error);
  REQUIRE(faces.size() == 1);
  const CliffFace& face = faces[0];
  // The same line and the same identity: the field changes the rock, not where the face stands.
  CHECK(face.identity == plain[0].identity);
  REQUIRE(face.cliff.formation.has_value());
  CHECK(face.cliff.formation->dip_deg == 8.0f);
  CHECK(face.cliff.formation->beds.size() == 2);
  CHECK(face.cliff.world_origin.x == static_cast<f64>(face.x_mm) / 1000.0);
  CHECK(face.cliff.world_origin.y == face.y);
  CHECK(face.cliff.world_origin.z == static_cast<f64>(face.z_mm) / 1000.0);
  // The recipe is keyed by the field, and solves.
  RockFile with_field, without;
  with_field.shape = RockShape::Cliff;
  with_field.cliff = face.cliff;
  without.shape = RockShape::Cliff;
  without.cliff = plain[0].cliff;
  CHECK(canonical_rock_json(with_field).find("formation") != std::string::npos);
  CHECK(canonical_rock_json(without).find("formation") == std::string::npos);
  CHECK(rock_source_hash(with_field) != rock_source_hash(without));
  assets::MeshData mesh;
  REQUIRE_MESSAGE(solve_cliff(face.cliff, mesh, nullptr, &error), error);
  // A field out of range is refused by the layer, with the scene's sentence.
  layer.formation->dip_deg = 89.0f;
  CHECK_FALSE(cliff_layer_in_range(layer, error));
  CHECK(error.find("dip_deg") != std::string::npos);
}

TEST_CASE("cliffs generator: a face's ends go into the ground where the steep ground ends") {
  CliffLayer layer = layer_over(0, 0, 256, 256);
  Vector<CliffFace> capped, tapered;
  std::string error;
  REQUIRE(trace_cliff_block(layer, ground_of(&ending_band), 0, 0, capped, &error));
  REQUIRE(capped.size() == 1);
  layer.taper_m = 16.0f;
  REQUIRE_MESSAGE(trace_cliff_block(layer, ground_of(&ending_band), 0, 0, tapered, &error), error);
  REQUIRE(tapered.size() == 1);
  const Vector<CliffPoint>& before = capped[0].cliff.points;
  const Vector<CliffPoint>& after = tapered[0].cliff.points;
  REQUIRE(before.size() == after.size());
  // Without the taper both ends stand: metres of face over the ground at their feet.
  CHECK(*before[0].height > 5.0f);
  CHECK(*before[before.size() - 1].height > 5.0f);
  CHECK(std::fabs(before[0].y) < 0.01f);
  // With it each end is the least face, a metre, sunk so its top is under the ground at its foot.
  for (const u32 i : {0u, after.size() - 1}) {
    CHECK(*after[i].height == doctest::Approx(1.0f));
    const f64 foot =
        ending_band(nullptr, tapered[0].x_mm + scene_gen::nearest_mm(static_cast<f64>(after[i].x)),
                    tapered[0].z_mm + scene_gen::nearest_mm(static_cast<f64>(after[i].z)));
    const f64 top =
        tapered[0].y + static_cast<f64>(after[i].y) + static_cast<f64>(*after[i].height);
    CHECK(top < foot - 0.25);
  }
  // The middle is the face it was, and the height rises along the taper without a step.
  const u32 mid = after.size() / 2;
  CHECK(*after[mid].height == *before[mid].height);
  CHECK(after[mid].y == before[mid].y);
  for (u32 i = 1; i <= mid; ++i)
    CHECK(*after[i].height >= *after[i - 1].height - 0.75f);
  // A tapered face is another face: its identity moves with its ends, and it still solves.
  CHECK(tapered[0].identity != capped[0].identity);
  assets::MeshData mesh;
  REQUIRE_MESSAGE(solve_cliff(tapered[0].cliff, mesh, nullptr, &error), error);
  // The taper's range is the layer's.
  layer.taper_m = 500.0f;
  CHECK_FALSE(cliff_layer_in_range(layer, error));
  CHECK(error.find("taper_m") != std::string::npos);
}

TEST_CASE("cliffs generator: no face stands proud of the ground behind it") {
  CliffLayer layer = layer_over(0, 0, 256, 256);
  layer.face.depth = 24.0f;  // an authored back: the v29 faces' own (G132's diagnosis)
  Vector<CliffFace> wall, seated;
  std::string error;
  REQUIRE(trace_cliff_block(layer, ground_of(&fin), 0, 0, wall, &error));
  REQUIRE(wall.size() == 1);
  // On a fin the default face is a retaining wall: its flat top, from the crest to a back metres
  // behind, stands over the ground that falls away behind the brow.
  assets::MeshData wall_mesh;
  CliffReport wall_report;
  REQUIRE_MESSAGE(solve_cliff(wall[0].cliff, wall_mesh, &wall_report, &error), error);
  const f64 before = proud_of_ground(wall[0], &fin, static_cast<f64>(wall_report.depth_m));
  layer.back_m = 24.0f;
  REQUIRE_MESSAGE(trace_cliff_block(layer, ground_of(&fin), 0, 0, seated, &error), error);
  REQUIRE(seated.size() == 1);
  const f64 after = proud_of_ground(seated[0], &fin, 24.0);
  MESSAGE("a face's top over the ground behind it, on a fin: "
          << before << " m with its own back " << wall_report.depth_m << " m behind, " << after
          << " m with back_m 24");
  CHECK(before > 1.0);
  CHECK(after <= -0.2);
  CHECK(*seated[0].cliff.depth == 24.0f);
  // On a slope that keeps rising behind the brow the same option changes no height.
  Vector<CliffFace> rising, rising_seated;
  layer.back_m.reset();
  REQUIRE(trace_cliff_block(layer, ground_of(&escarpment), 0, 0, rising, &error));
  layer.back_m = 24.0f;
  REQUIRE(trace_cliff_block(layer, ground_of(&escarpment), 0, 0, rising_seated, &error));
  REQUIRE(rising.size() == 1);
  REQUIRE(rising_seated.size() == 1);
  CHECK(rising_seated[0].identity == rising[0].identity);

  // The triangles: a shallow back puts the box's cells on the front. A closed box cannot pass
  // a half (its back mirrors its front); what is reached is printed.
  layer.back_m = 3.0f;
  Vector<CliffFace> shallow;
  REQUIRE(trace_cliff_block(layer, ground_of(&escarpment), 0, 0, shallow, &error));
  REQUIRE(shallow.size() == 1);
  assets::MeshData mesh;
  CliffReport deep_report, shallow_report;
  REQUIRE_MESSAGE(solve_cliff(rising[0].cliff, mesh, &deep_report, &error), error);
  REQUIRE_MESSAGE(solve_cliff(shallow[0].cliff, mesh, &shallow_report, &error), error);
  const auto front_share = [](const CliffReport& r) {
    return 2.0 * static_cast<f64>(r.stations) * static_cast<f64>(r.rows) /
           static_cast<f64>(r.face_triangles);
  };
  MESSAGE("the front's share of the face's triangles: "
          << front_share(deep_report) * 100.0 << " % with the back " << deep_report.depth_m
          << " m behind (cell " << deep_report.cell_m << " m), "
          << front_share(shallow_report) * 100.0 << " % with it 3 m behind (cell "
          << shallow_report.cell_m << " m)");
  CHECK(front_share(shallow_report) > front_share(deep_report));
  CHECK(front_share(shallow_report) > 0.38);
  CHECK(shallow_report.cell_m <= deep_report.cell_m);
}
