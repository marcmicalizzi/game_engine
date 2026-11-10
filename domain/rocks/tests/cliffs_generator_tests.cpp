// The scene generator "cliffs" (docs/subsystems/rocks.md, "The cliffs generator"): the tracer on
// synthetic ground — a steep band found where it is and nowhere on flat or gentle ground — the
// faces' identity whichever blocks and tiles ask, `tile` over every tile being `expand`, an
// earlier footprint honoured, and the faces it writes being rocks that solve.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/gfx/formation.h>
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
  // One tier: these cases are about the tracer, and the tiers have their own below.
  layer.tier_m = 200.0f;
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
  f32 tallest = 0.0f;
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
    tallest = std::max(tallest, report.height_m);
    if (first) {
      MESSAGE("first face: " << report.length_m << " m long, " << report.height_m << " m high, "
                             << report.triangles << " triangles at " << report.cell_m << " m");
      first = false;
    }
  }
  // The flanks are 113 m of steep ground in tiers of about 12 m: no face is the whole flank.
  CHECK(tallest > 8.0f);
  CHECK(tallest < 40.0f);
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
    f64 entry = -1.0;
    for (f64 d = 0.0; d <= back; d += 0.25) {
      const f64 x =
          static_cast<f64>(face.x_mm) / 1000.0 + static_cast<f64>(p[i].x) - nx * (lean + d);
      const f64 z =
          static_cast<f64>(face.z_mm) / 1000.0 + static_cast<f64>(p[i].z) - nz * (lean + d);
      const f64 g = ground(nullptr, scene_gen::nearest_mm(x), scene_gen::nearest_mm(z));
      if (entry < 0.0 && g >= top) entry = d;
      // Past the entry itself, where the ground is the top's height by definition.
      if (entry >= 0.0 && d >= entry + 1.5) worst = std::max(worst, top - g);
    }
    if (entry < 0.0) return 1.0e9;
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
  // The field's beds are where a face is cut into tiers, so the line is not the plain one's: the
  // lowest tier comes first.
  REQUIRE(!faces.empty());
  const CliffFace& face = faces[0];
  CHECK(face.lowest);
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
  CHECK(layer.taper_m == 16.0f);  // the layer's own: ends go into the ground unless it says 0
  layer.taper_m = 0.0f;
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
  // The middle is the face it was, and the height rises along the taper (its 16 m of line, four
  // spacings) without a step; past it the height is what the ground under the foot gives.
  const u32 mid = after.size() / 2;
  CHECK(*after[mid].height == *before[mid].height);
  CHECK(after[mid].y == before[mid].y);
  REQUIRE(mid > 4);
  for (u32 i = 1; i <= 4; ++i)
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

// ---- a face in tiers (G142; rocks.md, "A face in tiers") ----------------------------------------

namespace {

CliffLayer tiered_layer(f64 x0, f64 z0, f64 x1, f64 z1) {
  CliffLayer layer = layer_over(x0, z0, x1, z1);
  layer.tier_m = 12.0f;  // the layer's own
  return layer;
}

// The downhill normal of a face's line at vertex i: the line's right.
void normal_at(const Vector<CliffPoint>& p, u32 i, f64& nx, f64& nz) {
  const CliffPoint& a = p[i > 0 ? i - 1 : 0];
  const CliffPoint& b = p[i + 1 < p.size() ? i + 1 : p.size() - 1];
  const f64 tx = static_cast<f64>(b.x - a.x), tz = static_cast<f64>(b.z - a.z);
  const f64 l = std::sqrt(tx * tx + tz * tz);
  nx = -tz / l;
  nz = tx / l;
}

// **What a face's mesh shows from behind its own top**: over every vertex of an upward triangle
// (the top, a ledge), how far it stands over the ground at its own place when the ground between
// it and the front has already risen over it — a point of the top behind where the top went into
// the ground, which only a viewer behind the face sees, and sees as a hollow shell. 0 when no
// vertex does.
f64 shell_behind(const CliffFace& face, const assets::MeshData& mesh, scene_gen::HeightFn ground) {
  const Vector<CliffPoint>& line = face.cliff.points;
  const f64 ox = static_cast<f64>(face.x_mm) / 1000.0, oz = static_cast<f64>(face.z_mm) / 1000.0;
  f64 worst = 0.0;
  for (u32 t = 0; t + 2 < mesh.indices.size(); t += 3) {
    f64 q[3][3];
    for (u32 k = 0; k < 3; ++k) {
      const Vec3& v = mesh.positions[mesh.indices[t + k]];
      q[k][0] = static_cast<f64>(v.x);
      q[k][1] = static_cast<f64>(v.y);
      q[k][2] = static_cast<f64>(v.z);
    }
    const f64 ux = q[1][0] - q[0][0], uy = q[1][1] - q[0][1], uz = q[1][2] - q[0][2];
    const f64 vx = q[2][0] - q[0][0], vy = q[2][1] - q[0][1], vz = q[2][2] - q[0][2];
    const f64 nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const f64 len = std::sqrt(nx * nx + ny * ny + nz * nz);
    if (!(len > 0.0) || ny < 0.9 * len) continue;
    for (u32 k = 0; k < 3; ++k) {
      const f64 x = ox + q[k][0], y = face.y + q[k][1], z = oz + q[k][2];
      const f64 here = ground(nullptr, scene_gen::nearest_mm(x), scene_gen::nearest_mm(z));
      if (y <= here + 0.05) continue;  // in the ground
      // The nearest vertex of the line gives the way to the front.
      u32 near = 0;
      f64 best = 1.0e30;
      for (u32 i = 0; i < line.size(); ++i) {
        const f64 dx = q[k][0] - static_cast<f64>(line[i].x);
        const f64 dz = q[k][2] - static_cast<f64>(line[i].z);
        if (dx * dx + dz * dz < best) {
          best = dx * dx + dz * dz;
          near = i;
        }
      }
      f64 fx = 0.0, fz = 0.0;
      normal_at(line, near, fx, fz);
      for (f64 d = 0.25; d <= 30.0; d += 0.25) {
        const f64 g =
            ground(nullptr, scene_gen::nearest_mm(x + fx * d), scene_gen::nearest_mm(z + fz * d));
        if (g >= y + 0.05) {
          worst = std::max(worst, y - here);
          break;
        }
      }
    }
  }
  return worst;
}

// The share of a block's steep ground that some face stands in front of: over a 2 m grid, the
// points steeper than the layer's threshold whose height lies in the span (foot to top) of a face's
// vertex within 16 m.
f64 covered_share(const Vector<CliffFace>& faces, scene_gen::HeightFn ground, f64 x0, f64 z0,
                  f64 x1, f64 z1, f64 tan_steep) {
  u32 steep = 0, covered = 0;
  const auto h = [&](f64 x, f64 z) {
    return ground(nullptr, scene_gen::nearest_mm(x), scene_gen::nearest_mm(z));
  };
  for (f64 z = z0; z <= z1; z += 2.0) {
    for (f64 x = x0; x <= x1; x += 2.0) {
      const f64 sx = (h(x + 4.0, z) - h(x - 4.0, z)) / 8.0,
                sz = (h(x, z + 4.0) - h(x, z - 4.0)) / 8.0;
      if (sx * sx + sz * sz < tan_steep * tan_steep) continue;
      ++steep;
      const f64 y = h(x, z);
      bool in = false;
      for (u32 f = 0; f < faces.size() && !in; ++f) {
        const CliffFace& face = faces[f];
        for (const CliffPoint& p : face.cliff.points) {
          const f64 dx = static_cast<f64>(face.x_mm) / 1000.0 + static_cast<f64>(p.x) - x;
          const f64 dz = static_cast<f64>(face.z_mm) / 1000.0 + static_cast<f64>(p.z) - z;
          if (dx * dx + dz * dz > 16.0 * 16.0) continue;
          const f64 foot = face.y + static_cast<f64>(p.y);
          if (y >= foot - 0.5 && y <= foot + static_cast<f64>(*p.height) + 0.5) {
            in = true;
            break;
          }
        }
      }
      covered += in ? 1u : 0u;
    }
  }
  return steep > 0 ? static_cast<f64>(covered) / static_cast<f64>(steep) : 1.0;
}

}  // namespace

TEST_CASE("cliffs generator: a steep band is cut into tiers at the layer's breaks") {
  CliffLayer layer = tiered_layer(0, 0, 256, 256);
  CHECK(CliffLayer{}.tier_m == 12.0f);
  Vector<CliffFace> faces;
  std::string error;
  REQUIRE_MESSAGE(trace_cliff_block(layer, ground_of(&escarpment), 0, 0, faces, &error), error);
  // 30 m of band, steep to 27 m by the slope's 4 m differences, in tiers of 12: a break at 12 m,
  // and none at 24, which is within a third of a tier of the top. Two faces, lowest first.
  for (const CliffFace& face : faces)
    MESSAGE("tier " << face.tier << " from " << face.y << " m, " << face.mean_height << " m high");
  REQUIRE(faces.size() == 2);
  for (u32 f = 0; f < 2; ++f) {
    const CliffFace& face = faces[f];
    const CliffPoint& mid = face.cliff.points[face.cliff.points.size() / 2];
    const f64 foot = face.y + static_cast<f64>(mid.y);
    const f64 top = foot + static_cast<f64>(*mid.height);
    MESSAGE("tier " << face.tier << ": foot " << foot << " m, top " << top << " m, batter "
                    << static_cast<f64>(*mid.batter) << ", " << face.cliff.points.size()
                    << " vertices, budget " << face.cliff.triangle_budget);
    CHECK(face.tier == static_cast<i32>(f));
    CHECK(face.lowest == (f == 0));
    CHECK(face.cliff.open);
    // Only the lowest tier has the band's skirt and blocks, and a ground patch under them.
    CHECK((face.cliff.talus > 0.0f) == (f == 0));
    CHECK((face.cliff.talus_blocks > 0) == (f == 0));
    CHECK(face.cliff.ground.has_value() == (f == 0));
    // A tier's top is the break (the top one's, half a metre under the brow), and the next tier's
    // foot is at most that: its bottom edge is under the top it stands on.
    if (f == 0) CHECK(top == doctest::Approx(12.0).epsilon(0.02));
    if (f == 1) CHECK(top == doctest::Approx(26.5).epsilon(0.02));
    if (f > 0) {
      CHECK(foot <= 12.0 * f + 0.01);
      CHECK(foot > 12.0 * f - 2.0);
    }
    // A share of the layer's budget by its height.
    CHECK(face.cliff.triangle_budget < layer.face.triangle_budget);
    assets::MeshData mesh;
    REQUIRE_MESSAGE(solve_cliff(face.cliff, mesh, nullptr, &error), error);
  }
  // With the formation the breaks are its resistant beds' tops: at the top of a tier that has
  // another over it, the profile's own point is on a sandstone bed's top (2 m of sandstone on
  // 1 m of shale, so the coordinate there is 2 past a whole period).
  layer.formation = level_formation();
  Vector<CliffFace> bedded;
  REQUIRE_MESSAGE(trace_cliff_block(layer, ground_of(&escarpment), 0, 0, bedded, &error), error);
  REQUIRE(bedded.size() >= 2);
  gfx::FormationParams field;
  REQUIRE(gfx::pack_formation(*layer.formation, field, &error));
  const gfx::FormationFrame frame = gfx::formation_frame(field);
  // Counted over every vertex of every face that has a tier over it (a tier's own end is tapered
  // under the break, and its top tier's top is the brow's): most tops are on a sandstone bed's top.
  u32 on_plane = 0, tops = 0;
  for (u32 f = 0; f < bedded.size(); ++f) {
    const CliffFace& face = bedded[f];
    bool has_upper = false;
    for (const CliffFace& other : bedded)
      has_upper = has_upper || other.tier == face.tier + 1;
    if (!has_upper) continue;
    for (const CliffPoint& p : face.cliff.points) {
      const f64 top = face.y + static_cast<f64>(p.y) + static_cast<f64>(*p.height);
      // The profile's point at that height: the band rises 30 m over x 100 to 120.
      const f64 x = 100.0 + top * 20.0 / 30.0;
      const f64 z = static_cast<f64>(face.z_mm) / 1000.0 + static_cast<f64>(p.z);
      const f64 c = gfx::formation_coordinate(frame, x, top, z).coordinate;
      const f64 in_period = c - std::floor(c / 3.0) * 3.0;
      ++tops;
      on_plane += std::fabs(in_period - 2.0) < 0.2 ? 1u : 0u;
    }
  }
  MESSAGE(bedded.size() << " faces with the formation; " << on_plane << " of " << tops
                        << " tops under another tier are on a sandstone bed's top");
  CHECK(tops > 0);
  CHECK(2 * on_plane > tops);  // The tier's range is the layer's.
  layer.tier_m = 1.0f;
  CHECK_FALSE(cliff_layer_in_range(layer, error));
  CHECK(error.find("tier_m") != std::string::npos);
}

TEST_CASE("cliffs generator: no generated face shows from behind its own top, and tiers cover") {
  struct Case {
    const char* name;
    scene_gen::HeightFn ground;
    f64 x0, z0, x1, z1;
    i32 bx, bz;
  };
  // A fin (a top 2 m wide, the ground falling away behind it), the escarpment scene's ridge (a
  // crest rounded over between two steep flanks) and a planar slope under a flat top.
  const Case cases[] = {{"fin", &fin, 0, 0, 256, 256, 0, 0},
                        {"rounded crest", &escarpment_ridge, -256, -256, 0, 0, -1, -1},
                        {"planar slope", &escarpment, 0, 0, 256, 256, 0, 0}};
  const f64 tan_steep = std::tan(50.0 * 3.14159265358979323846 / 180.0);
  for (const Case& c : cases) {
    const std::string name = c.name;
    CAPTURE(name);
    std::string error;
    // The single box a line, as the generator stood before the tiers, with the same seated top.
    CliffLayer one = layer_over(c.x0, c.z0, c.x1, c.z1);
    Vector<CliffFace> single;
    REQUIRE_MESSAGE(trace_cliff_block(one, ground_of(c.ground), c.bx, c.bz, single, &error), error);
    CliffLayer layer = tiered_layer(c.x0, c.z0, c.x1, c.z1);
    Vector<CliffFace> faces;
    REQUIRE_MESSAGE(trace_cliff_block(layer, ground_of(c.ground), c.bx, c.bz, faces, &error),
                    error);
    REQUIRE(!faces.empty());
    f64 worst_line = -1.0e9, worst_mesh = 0.0;
    u32 triangles = 0;
    for (const CliffFace& face : faces) {
      f64 tallest = 1.0;
      for (const CliffPoint& p : face.cliff.points)
        tallest = std::max(tallest, static_cast<f64>(*p.height));
      // The line's own statement: the top, from where it goes into the ground to the back, is
      // under the ground; it goes in within the back.
      REQUIRE(face.cliff.depth.has_value());
      CHECK(static_cast<f64>(*face.cliff.depth) >= cliff_back_m(layer.face, tallest) - 1e-3);
      const f64 proud = proud_of_ground(face, c.ground, static_cast<f64>(*face.cliff.depth));
      if (proud > 0.0)
        MESSAGE("tier " << face.tier << " at " << face.y << " m, " << face.length << " m long, "
                        << face.mean_height << " m high, back " << *face.cliff.depth << ": "
                        << proud);
      worst_line = std::max(worst_line, proud);
      // And the mesh's: no vertex of the top over the ground behind where the top went in.
      assets::MeshData mesh;
      CliffReport report;
      REQUIRE_MESSAGE(solve_cliff(face.cliff, mesh, &report, &error), error);
      triangles += report.triangles;
      worst_mesh = std::max(worst_mesh, shell_behind(face, mesh, c.ground));
    }
    const f64 with_tiers = covered_share(faces, c.ground, c.x0, c.z0, c.x1, c.z1, tan_steep);
    const f64 with_one = covered_share(single, c.ground, c.x0, c.z0, c.x1, c.z1, tan_steep);
    MESSAGE(name << ": " << faces.size() << " faces in tiers (" << triangles
                 << " triangles), a top at most " << worst_line
                 << " m over the ground behind it, a mesh vertex at most " << worst_mesh
                 << " m proud behind; the steep ground covered " << with_tiers * 100.0
                 << " % in tiers, " << with_one * 100.0 << " % by one seated box a line ("
                 << single.size() << " faces)");
    CHECK(worst_line <= 0.0);
    CHECK(worst_mesh == 0.0);
    CHECK(with_tiers >= 0.9);
    CHECK(with_tiers >= with_one - 0.02);
  }
}

TEST_CASE("cliffs generator: an open face spends its triangles on its front") {
  // The triangles: a generated face has no back and no bottom, so its front is most of it. The
  // same face as a closed box, for the measure: under a half, since a box's back mirrors its front.
  CliffLayer layer = layer_over(0, 0, 256, 256);
  Vector<CliffFace> standard;
  std::string error;
  REQUIRE(trace_cliff_block(layer, ground_of(&escarpment), 0, 0, standard, &error));
  REQUIRE(standard.size() == 1);
  const auto front_share = [](const CliffReport& r) {
    return 2.0 * static_cast<f64>(r.stations) * static_cast<f64>(r.rows) /
           static_cast<f64>(r.face_triangles);
  };
  assets::MeshData mesh;
  CliffReport standard_report;
  REQUIRE_MESSAGE(solve_cliff(standard[0].cliff, mesh, &standard_report, &error), error);
  MESSAGE("a layer's own face: " << front_share(standard_report) * 100.0 << " % with the back "
                                 << standard_report.depth_m << " m behind (cell "
                                 << standard_report.cell_m << " m), " << standard_report.triangles
                                 << " triangles");
  CHECK(standard[0].cliff.open);
  CHECK(front_share(standard_report) >= 0.6);
  // The generator asked the solver how far back the face goes, and got what the solver stands.
  CHECK(
      static_cast<f64>(standard_report.depth_m) ==
      doctest::Approx(cliff_back_m(standard[0].cliff, static_cast<f64>(standard_report.height_m))));
  Cliff boxed = standard[0].cliff;
  boxed.open = false;
  CliffReport boxed_report;
  REQUIRE_MESSAGE(solve_cliff(boxed, mesh, &boxed_report, &error), error);
  MESSAGE("the same face as a closed box: " << front_share(boxed_report) * 100.0 << " % (cell "
                                            << boxed_report.cell_m << " m)");
  CHECK(front_share(boxed_report) < 0.5);
  // An open face is the box's front, top and ends: 2ab + 2ac + 4bc of its cells.
  CHECK(standard_report.face_triangles ==
        2u * standard_report.stations * standard_report.rows +
            2u * standard_report.stations * standard_report.depth_rows +
            4u * standard_report.rows * standard_report.depth_rows);
}
