// What a placement entry plans for the ground and the shapes it occupies, through a host that
// carries the tile set, the routes and the world (roadmap R144; docs/subsystems/scene_gen.md, "What
// an entry plans for the ground" and "The shapes a tile occupies"). No GPU: the scene reader, the
// `tiles` ground and the world's placements consumer on the CPU.
//
// The cases: a routes entry's treads planned at the read against the same treads pasted into the
// scene (one tile set, the same heights to the bit); two entries that edit the same ground (each
// plans on the ground as it was, the entries' order moves no height); a planned opening against the
// same polygon written as an authored hole; an edit refused where no tile set can take it; and the
// shapes — a tread's bands and a mouth's apron — keeping a later entry off them, the whole read and
// the per-tile build placing the same points.
//
// The generators of this file's own: `r144-mouth`, a cave mouth's stand-in (an opening, optionally
// a benched pad, and the opening's outline grown to its apron as the shape it occupies), and
// `r144-probe`, which stands on every point of a lattice the exclusion leaves free — the one
// question the sward, the scatter and the litter each ask of a candidate (`Context::excluded_mm`).
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/tileset/tileset.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>
#include <systems/world/placement_tiles.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <schemas/scene.h>
#include <string>
#include <utility>
#include <vector>

using namespace engine;

namespace {

std::string g_log;
u32 g_probe_shapes = 0;  // the bands and polygons the probe's last call was handed in `earlier`

f64 number(const JsonValue& params, const char* key, f64 otherwise) {
  f64 v = otherwise;
  if (const JsonValue* found = params.find(key)) found->get_f64(v);
  return v;
}

// ---- r144-mouth ---------------------------------------------------------------------------------

struct Mouth {
  std::string name;
  i64 x_mm = 0, z_mm = 0, half_mm = 0, apron_mm = 0;
  bool hole = true;
  bool pad = false;
  f64 pad_y = 0.0;
  f32 tile_size = 32.0f;
};
f64 metres(i64 mm) { return static_cast<f64>(mm) / 1000.0; }
// The square about the mouth's centre, `half_mm` either way, counter-clockwise from above.
void square(const Mouth& m, i64 half_mm, scene_gen::FootprintPoint out[4]) {
  out[0] = {m.x_mm - half_mm, m.z_mm - half_mm};
  out[1] = {m.x_mm - half_mm, m.z_mm + half_mm};
  out[2] = {m.x_mm + half_mm, m.z_mm + half_mm};
  out[3] = {m.x_mm + half_mm, m.z_mm - half_mm};
}
bool mouth_open(const JsonValue& params, const scene_gen::Context& context, void** state,
                std::string*) {
  auto* m = new Mouth;
  std::string_view name;
  if (const JsonValue* found = params.find("name")) found->get_string(name);
  m->name = std::string(name);
  m->x_mm = scene_gen::nearest_mm(number(params, "x", 0.0));
  m->z_mm = scene_gen::nearest_mm(number(params, "z", 0.0));
  m->half_mm = scene_gen::nearest_mm(number(params, "half", 2.0));
  m->apron_mm = scene_gen::nearest_mm(number(params, "apron", 1.5));
  if (const JsonValue* found = params.find("hole")) found->get_bool(m->hole);
  if (const JsonValue* found = params.find("pad")) m->pad = found->get_f64(m->pad_y);
  m->tile_size = context.tile_size;
  // What it would plan on: the ground before any edit at its centre, to the millimetre, and
  // whether anything has opened the ground there.
  const scene_gen::Ground before = context.uncut();
  g_log += "open " + m->name + " " +
           std::to_string(scene_gen::nearest_mm(before.surface_mm(m->x_mm, m->z_mm))) +
           (context.excluded_mm(m->x_mm, m->z_mm, scene_gen::k_exclude_holes) ? " in-hole;" : ";");
  *state = m;
  return true;
}
void mouth_close(void* state) noexcept { delete static_cast<Mouth*>(state); }
bool mouth_plan(void* state, const scene_gen::Context& context, scene_gen::GroundPlan& out,
                std::string*) {
  const Mouth& m = *static_cast<const Mouth*>(state);
  // The hook's ground is the unedited one itself.
  g_log += "plan " + m.name + " " +
           std::to_string(scene_gen::nearest_mm(context.ground.surface_mm(m.x_mm, m.z_mm))) + ";";
  scene_gen::FootprintPoint corner[4];
  if (m.hole) {
    square(m, m.half_mm, corner);
    scene::TerrainHole hole;
    hole.name = m.name;
    for (const scene_gen::FootprintPoint& p : corner)
      hole.polygon.push_back(WorldPos{metres(p.x_mm), 0.0, metres(p.z_mm)});
    out.holes.push_back(std::move(hole));
  }
  if (m.pad) {
    // A pad benched level over the square twice the mouth's, beside it.
    square(m, 2 * m.half_mm, corner);
    scene::TerrainEdit pad;
    pad.name = m.name + " pad";
    pad.mode = scene::TerrainEditMode::Bench;
    for (const scene_gen::FootprintPoint& p : corner)
      pad.outline.push_back(WorldPos{metres(p.x_mm), m.pad_y, metres(p.z_mm)});
    out.edits.push_back(std::move(pad));
  }
  return true;
}
// The apron: the opening's outline grown, as a polygon over this call's own scratch.
void mouth_shape(const Mouth& m, scene_gen::Placements& out) {
  scene_gen::FootprintPoint corner[4];
  square(m, m.half_mm + m.apron_mm, corner);
  out.shapes.add(scene_gen::Footprint::polygon(corner));
}
bool mouth_expand(void* state, const scene_gen::Context&, scene_gen::Placements& out,
                  std::string*) {
  mouth_shape(*static_cast<const Mouth*>(state), out);
  return true;
}
bool mouth_tile(void* state, scene_gen::TileCoord tile, const scene_gen::Context&,
                scene_gen::Placements& out, std::string*) {
  const Mouth& m = *static_cast<const Mouth*>(state);
  // Every tile the apron reaches, and four metres more.
  const i64 size = scene_gen::nearest_mm(static_cast<f64>(m.tile_size));
  const i64 reach = m.half_mm + m.apron_mm + 4000;
  const i64 x0 = tile.x * size, z0 = tile.z * size;
  if (m.x_mm + reach < x0 || m.x_mm - reach > x0 + size || m.z_mm + reach < z0 ||
      m.z_mm - reach > z0 + size)
    return true;
  mouth_shape(m, out);
  return true;
}
constexpr scene_gen::PlacementGeneratorDesc k_mouth{.name = "r144-mouth",
                                                    .open = &mouth_open,
                                                    .close = &mouth_close,
                                                    .expand = &mouth_expand,
                                                    .tile = &mouth_tile,
                                                    .plan_ground = &mouth_plan};
const scene_gen::Registrar k_mouth_registrar{k_mouth};

// ---- r144-probe ---------------------------------------------------------------------------------

struct Probe {
  i64 step_mm = 500;
  i64 lo_mm = -20000, hi_mm = 20000;  // the square it covers, both ways
};
bool probe_open(const JsonValue& params, const scene_gen::Context&, void** state, std::string*) {
  auto* p = new Probe;
  p->step_mm = scene_gen::nearest_mm(number(params, "step", 0.5));
  p->lo_mm = scene_gen::nearest_mm(number(params, "lo", -20.0));
  p->hi_mm = scene_gen::nearest_mm(number(params, "hi", 20.0));
  *state = p;
  return true;
}
void probe_close(void* state) noexcept { delete static_cast<Probe*>(state); }
void probe_window(const Probe& p, i64 x0, i64 x1, i64 z0, i64 z1, const scene_gen::Context& context,
                  scene_gen::Placements& out) {
  g_probe_shapes = 0;
  for (const scene_gen::Footprint& f : context.earlier)
    g_probe_shapes += f.shape != scene_gen::FootprintShape::disc ? 1u : 0u;
  // Lattice points in [x0, x1) x [z0, z1), the lattice counted from the world's origin.
  const auto first = [&](i64 from) {
    i64 k = from / p.step_mm;
    if (k * p.step_mm < from) ++k;
    return k * p.step_mm;
  };
  for (i64 z = first(z0); z < z1; z += p.step_mm)
    for (i64 x = first(x0); x < x1; x += p.step_mm) {
      if (context.excluded_mm(x, z)) continue;
      scene_gen::Placement at;
      at.mesh = 0;
      at.position = WorldPos{metres(x), context.ground.surface_mm(x, z), metres(z)};
      out.instances.push_back(at);
    }
  out.things = out.instances.size();
}
bool probe_expand(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
                  std::string*) {
  const Probe& p = *static_cast<const Probe*>(state);
  out.meshes.push_back(scene_gen::PlacementMesh{"r144/probe.glb", "r144/probe"});
  probe_window(p, p.lo_mm, p.hi_mm, p.lo_mm, p.hi_mm, context, out);
  return true;
}
bool probe_meshes(void*, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
                  std::string*) {
  out.push_back(scene_gen::PlacementMesh{"r144/probe.glb", "r144/probe"});
  return true;
}
bool probe_tile(void* state, scene_gen::TileCoord tile, const scene_gen::Context& context,
                scene_gen::Placements& out, std::string*) {
  const Probe& p = *static_cast<const Probe*>(state);
  const i64 size = scene_gen::nearest_mm(static_cast<f64>(context.tile_size));
  probe_window(p, std::max(p.lo_mm, tile.x * size), std::min(p.hi_mm, (tile.x + 1) * size),
               std::max(p.lo_mm, tile.z * size), std::min(p.hi_mm, (tile.z + 1) * size), context,
               out);
  return true;
}
constexpr scene_gen::PlacementGeneratorDesc k_probe{.name = "r144-probe",
                                                    .open = &probe_open,
                                                    .close = &probe_close,
                                                    .expand = &probe_expand,
                                                    .meshes = &probe_meshes,
                                                    .tile = &probe_tile};
const scene_gen::Registrar k_probe_registrar{k_probe};

// ---- the scene ----------------------------------------------------------------------------------

// A hillside 64 m square about the origin at 25 cm a point (finer than a tread is wide, or the
// tile set would close the tread's edit): 10 % across x, 4 % across z and a little twist, under
// the routes' grade cap everywhere.
void write_hill(const test::TempDir& tmp) {
  tileset::Heightfield field;
  field.width = 257;
  field.height = 257;
  field.spacing_mm = 250;
  field.origin_x_mm = -32000;
  field.origin_z_mm = -32000;
  for (u32 j = 0; j < field.height; ++j)
    for (u32 i = 0; i < field.width; ++i) {
      const f64 x = 0.25 * static_cast<f64>(i) - 32.0, z = 0.25 * static_cast<f64>(j) - 32.0;
      field.data.push_back(static_cast<f32>(8.0 + 0.10 * x + 0.04 * z + 0.0005 * x * z));
    }
  std::string error;
  REQUIRE_MESSAGE(tileset::write_heightfield(tmp.file("hill.json"), field, error), error);
}
std::string scene_text(const std::string& tiles_extra, const std::string& terrain_extra,
                       const std::string& placements, const std::string& world = std::string()) {
  return std::string(R"({"format":"engine.scene.v1","name":"r144","terrain":{"size":17,)") +
         R"("extent":32,"provider":"tiles","seed":3,"tiles":{"heightfield":"hill.json",)" +
         R"("ddc":"ddc","cells":32,"far_levels":0)" + tiles_extra + "}" + terrain_extra +
         R"(},"placements":[)" + placements + "]" + world + "}";
}
const char* k_routes =
    R"({"generator":"routes","params":{"name":"way","region_min":[-28,-28],)"
    R"("region_max":[28,28],"routes":[{"name":"across","waypoints":[[-24,0,-20],[22,0,18]]}]}})";
const char* k_probe_entry = R"({"generator":"r144-probe","params":{"step":0.5}})";

renderer::SceneDesc read(const test::TempDir& tmp, const char* file, const std::string& text) {
  const std::string path = tmp.file(file);
  REQUIRE(io::write_file_atomic(path, text) == io::Status::Ok);
  renderer::SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(renderer::read_scene_file(path, desc, error), error);
  return desc;
}
std::string compact(const JsonValue& json) {
  JsonWriteOptions options;
  options.pretty = false;
  return write_json(json, options);
}
u32 files_under(const std::string& dir) {
  u32 count = 0;
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec))
    count += entry.is_regular_file() ? 1u : 0u;
  return count;
}
// The ground's surface over the middle of the hill, every half metre, as bits.
std::vector<f32> heights(const renderer::TerrainDesc& terrain) {
  const renderer::TerrainSampler ground(terrain);
  REQUIRE_MESSAGE(ground.ok(), ground.error());
  std::vector<f32> out;
  for (i64 z = -28000; z <= 28000; z += 500)
    for (i64 x = -28000; x <= 28000; x += 500)
      out.push_back(ground.provider().surface_mm(x, z));
  return out;
}
using Points = std::vector<std::pair<i64, i64>>;
Points sorted(Points points) {
  std::sort(points.begin(), points.end());
  return points;
}
u32 mesh_named(const renderer::SceneDesc& desc, std::string_view name) {
  for (u32 m = 0; m < desc.mesh_info.size(); ++m)
    if (desc.mesh_info[m].name == name) return m;
  return ~0u;
}

}  // namespace

TEST_CASE("ground plan: a routes entry's treads planned at the read are the treads pasted") {
  const test::TempDir tmp("view_ground_plan_routes");
  write_hill(tmp);
  // Planned: the scene names the entry and pastes nothing.
  const renderer::SceneDesc planned = read(tmp, "planned.json", scene_text("", "", k_routes));
  REQUIRE(planned.terrain.tiles.has_value());
  const auto& edits = planned.terrain.tiles->edits;
  REQUIRE_FALSE(edits.empty());
  JsonValue pasted_edits = JsonValue::array();
  for (const scene::TerrainEdit& edit : edits) {
    CHECK(edit.mode == scene::TerrainEditMode::Bench);
    pasted_edits.as_array().push_back(schema::to_json(edit));
  }
  const std::vector<f32> planned_heights = heights(planned.terrain);
  const u32 files = files_under(tmp.file("ddc"));
  // Pasted: the same edits written into the tile set's block, and no entry.
  const renderer::SceneDesc pasted =
      read(tmp, "pasted.json", scene_text(R"(,"edits":)" + compact(pasted_edits), "", ""));
  // One block, one hash, and one tile set: the pasted read and its ground built nothing the
  // planned one had not.
  CHECK(compact(schema::to_json(*planned.terrain.tiles)) ==
        compact(schema::to_json(*pasted.terrain.tiles)));
  CHECK(renderer::terrain_hash(planned.terrain) == renderer::terrain_hash(pasted.terrain));
  const std::vector<f32> pasted_heights = heights(pasted.terrain);
  CHECK(files_under(tmp.file("ddc")) == files);
  REQUIRE(planned_heights.size() == pasted_heights.size());
  u32 different = 0;
  for (usize k = 0; k < planned_heights.size(); ++k)
    different += std::memcmp(&planned_heights[k], &pasted_heights[k], sizeof(f32)) != 0 ? 1u : 0u;
  CHECK(different == 0);
  // And the treads are in the ground: it is not the hill it was.
  renderer::TerrainDesc bare = pasted.terrain;
  bare.tiles->edits.clear();
  const std::vector<f32> bare_heights = heights(bare);
  u32 benched = 0;
  for (usize k = 0; k < bare_heights.size(); ++k)
    benched += bare_heights[k] != planned_heights[k] ? 1u : 0u;
  CHECK(benched > 100);
  // Read again, the plan is read from its file and the edits are the same bytes.
  const renderer::SceneDesc again = read(tmp, "planned.json", scene_text("", "", k_routes));
  CHECK(compact(schema::to_json(*again.terrain.tiles)) ==
        compact(schema::to_json(*planned.terrain.tiles)));
}

TEST_CASE("ground plan: two entries that edit the ground each plan on the ground as it was") {
  const test::TempDir tmp("view_ground_plan_order");
  write_hill(tmp);
  // Two pads that overlap by two metres (a's over x in [4, 16], b's over [14, 26]), one raised
  // over the hill and one sunk into it, each with its opening; and an authored hole.
  const std::string a =
      R"({"generator":"r144-mouth","params":{"name":"a","x":10,"z":6,"half":3,"pad":14}})";
  const std::string b =
      R"({"generator":"r144-mouth","params":{"name":"b","x":20,"z":6,"half":3,"pad":5}})";
  const std::string authored = R"(,"holes":[{"name":"shaft","center":[-30,0,-30],"radius_m":2}])";
  g_log.clear();
  const renderer::SceneDesc ab = read(tmp, "ab.json", scene_text("", authored, a + "," + b));
  // The hill at each centre before any edit, 8 + 0.1 x + 0.04 z + 0.0005 x z, in millimetres:
  // every plan reads it, in the scene's order and before the ground is made, with nothing opened
  // there yet; and every open after reads it too, through `uncut()`, though the ground it is
  // handed now carries both pads and its own opening.
  CHECK(g_log ==
        "open a 9270;plan a 9270;open b 10300;plan b 10300;"
        "open a 9270 in-hole;open b 10300 in-hole;");
  REQUIRE(ab.terrain.tiles->edits.size() == 2);
  CHECK(ab.terrain.tiles->edits[0].name == "a pad");
  CHECK(ab.terrain.tiles->edits[1].name == "b pad");
  // The openings after the scene's own hole, and nothing is placed in any of the three.
  CHECK(ab.terrain.holes.count() == 3);
  CHECK(ab.terrain.holes.contains_mm(-30000.0, -30000.0));
  CHECK(ab.terrain.holes.contains_mm(10000.0, 6000.0));
  CHECK(ab.terrain.holes.contains_mm(20000.0, 6000.0));
  CHECK(ab.terrain.exclusion.hole_count() == 3);
  // Swapped in the file: the plans swap, the edits and the openings swap, and no height moves,
  // because edits compose by a highest fill and then a lowest cut (tileset.md).
  g_log.clear();
  const renderer::SceneDesc ba = read(tmp, "ba.json", scene_text("", authored, b + "," + a));
  CHECK(g_log ==
        "open b 10300;plan b 10300;open a 9270;plan a 9270;"
        "open b 10300 in-hole;open a 9270 in-hole;");
  CHECK(ba.terrain.tiles->edits[0].name == "b pad");
  const std::vector<f32> first = heights(ab.terrain), second = heights(ba.terrain);
  REQUIRE(first.size() == second.size());
  CHECK(std::memcmp(first.data(), second.data(), first.size() * sizeof(f32)) == 0);
  // Both pads are in it: a's stands at 14 m where the hill was near 9, b's at 5 m where it was
  // over 10, and where the two overlap the cut wins.
  const renderer::TerrainSampler ground(ab.terrain);
  REQUIRE(ground.ok());
  CHECK(ground.provider().surface_mm(5000, 6000) == doctest::Approx(14.0).epsilon(0.001));
  CHECK(ground.provider().surface_mm(24000, 6000) == doctest::Approx(5.0).epsilon(0.001));
  CHECK(ground.provider().surface_mm(15000, 6000) == doctest::Approx(5.0).epsilon(0.001));
}

TEST_CASE("ground plan: a planned opening is the same polygon written as a hole") {
  const test::TempDir tmp("view_ground_plan_hole");
  write_hill(tmp);
  const renderer::SceneDesc planned = read(
      tmp, "planned.json",
      scene_text(
          "", "",
          R"({"generator":"r144-mouth","params":{"name":"m","x":10.5,"z":-4.25,"half":2.5}})"));
  const renderer::SceneDesc pasted = read(
      tmp, "pasted.json",
      scene_text(
          "",
          R"(,"holes":[{"name":"m","polygon":[[8,0,-6.75],[8,0,-1.75],[13,0,-1.75],[13,0,-6.75]]}])",
          R"({"generator":"r144-mouth","params":{"name":"m","x":10.5,"z":-4.25,"half":2.5,"hole":false}})"));
  REQUIRE(planned.terrain.holes.count() == 1);
  // The compiled holes hash every region's numbers, and the terrain's hash is what its meshes are
  // cached under: equal, the two scenes draw one terrain from one cache entry.
  CHECK(planned.terrain.holes.hash() == pasted.terrain.holes.hash());
  CHECK(planned.terrain.holes.hash() != 0);
  CHECK(renderer::terrain_hash(planned.terrain) == renderer::terrain_hash(pasted.terrain));
  CHECK(planned.terrain.exclusion.hole_count() == 1);
  for (i64 z = -9000; z <= 1000; z += 250)
    for (i64 x = 6000; x <= 15000; x += 250) {
      const bool in = planned.terrain.holes.contains_mm(static_cast<f64>(x), static_cast<f64>(z));
      CHECK(in == pasted.terrain.holes.contains_mm(static_cast<f64>(x), static_cast<f64>(z)));
      CHECK(in == (x > 8000 && x < 13000 && z > -6750 && z < -1750));
    }
}

TEST_CASE("ground plan: an edit is refused where no tile set can take it") {
  const test::TempDir tmp("view_ground_plan_refused");
  const std::string path = tmp.file("waves.json");
  REQUIRE(io::write_file_atomic(
              path,
              R"({"format":"engine.scene.v1","name":"waves","terrain":{"size":17,"extent":20},)"
              R"("placements":[{"generator":"r144-mouth","params":{"name":"m","pad":1}}]})") ==
          io::Status::Ok);
  renderer::SceneDesc desc;
  std::string error;
  CHECK_FALSE(renderer::read_scene_file(path, desc, error));
  CHECK(error.find("placements 0") != std::string::npos);
  CHECK(error.find("r144-mouth") != std::string::npos);
  CHECK(error.find("\"tiles\"") != std::string::npos);
}

TEST_CASE(
    "ground plan: the shapes an entry occupies keep the next off them, read whole or by tile") {
  const test::TempDir tmp("view_ground_plan_shapes");
  write_hill(tmp);
  const std::string mouth =
      R"({"generator":"r144-mouth","params":{"name":"m","x":-10,"z":7.5,"half":2,"apron":1.5}})";
  const std::string entries = mouth + "," + k_routes + "," + k_probe_entry;
  // Read whole.
  const renderer::SceneDesc whole = read(tmp, "whole.json", scene_text("", "", entries));
  // The probe was handed the apron and the tread's bands after it, and stands on neither.
  CHECK(g_probe_shapes > 1);
  CHECK(whole.terrain.exclusion.footprint_count() == g_probe_shapes);
  const u32 probe_mesh = mesh_named(whole, "r144/probe");
  REQUIRE(probe_mesh != ~0u);
  Points stood;
  for (const renderer::SceneInstance& instance : whole.instances)
    if (instance.mesh == probe_mesh)
      stood.push_back(
          {scene_gen::nearest_mm(instance.origin.x), scene_gen::nearest_mm(instance.origin.z)});
  stood = sorted(std::move(stood));
  const u32 lattice = 80 * 80;
  CHECK(stood.size() < lattice);
  const scene_gen::Ground none;
  u32 on_shape = 0, in_hole = 0;
  for (i64 z = -20000; z < 20000; z += 500)
    for (i64 x = -20000; x < 20000; x += 500) {
      const bool shape =
          whole.terrain.exclusion.excluded_mm(none, x, z, scene_gen::k_exclude_footprints);
      const bool hole = whole.terrain.exclusion.excluded_mm(none, x, z, scene_gen::k_exclude_holes);
      on_shape += shape ? 1u : 0u;
      in_hole += hole ? 1u : 0u;
      CHECK(std::binary_search(stood.begin(), stood.end(), std::pair<i64, i64>{x, z}) ==
            !(shape || hole));
    }
  CHECK(in_hole == 7 * 7);    // the opening, 4 m square: the lattice points strictly inside
  CHECK(on_shape > 15 * 15);  // the apron's 7 m square, edge included, and the tread's bands
  // The apron keeps the probe off the mouth's rim: a point outside the opening, inside the apron.
  CHECK_FALSE(std::binary_search(stood.begin(), stood.end(), std::pair<i64, i64>{-13000, 7500}));
  CHECK(std::binary_search(stood.begin(), stood.end(), std::pair<i64, i64>{-14000, 7500}));
  // A tread's station is bare: the first edit's own points.
  const WorldPos station = whole.terrain.tiles->edits[0].points[1];
  CHECK(whole.terrain.exclusion.excluded_mm(none, scene_gen::nearest_mm(station.x),
                                            scene_gen::nearest_mm(station.z),
                                            scene_gen::k_exclude_footprints));

  // By tile: the same scene streamed, every tile the probe covers built by the world's consumer.
  const renderer::SceneDesc streamed_desc =
      read(tmp, "streamed.json", scene_text("", "", entries, R"(,"world":{"tile_size":16})"));
  REQUIRE(streamed_desc.streamed.size() == 3);
  // Nothing was expanded, so the scene's exclusion holds no shape: a tile's are its own.
  CHECK(streamed_desc.terrain.exclusion.footprint_count() == 0);
  renderer::SceneData scene;
  scene.terrain = streamed_desc.terrain;
  scene.streamed = streamed_desc.streamed;
  scene.parts.resize(streamed_desc.meshes.size());
  scene.world.tile_size = 16.0f;
  scene.dynamic = true;
  world::PlacementTiles placements;
  world::PlacementTilesConfig config;
  config.tile_size = 16.0f;
  config.ring_count = 1;
  std::string error;
  REQUIRE_MESSAGE(placements.create(scene, config, &error), error);
  const world::TileConsumer consumer = placements.consumer();
  Points by_tile;
  u32 tiles_with_shapes = 0;
  for (i32 tz = -2; tz <= 1; ++tz)
    for (i32 tx = -2; tx <= 1; ++tx) {
      world::TileEvent event;
      event.tile = {tx, tz};
      event.to = 0;
      CHECK(consumer.activate(consumer.context, event));
      consumer.commit(consumer.context);
      tiles_with_shapes += g_probe_shapes > 0 ? 1u : 0u;
      for (const renderer::SceneInstance& instance : placements.tile_instances(event.tile))
        by_tile.push_back(
            {scene_gen::nearest_mm(instance.origin.x), scene_gen::nearest_mm(instance.origin.z)});
    }
  CHECK(tiles_with_shapes > 2);
  CHECK(tiles_with_shapes < 16);  // a tile no shape reaches is handed none
  CHECK(sorted(std::move(by_tile)) == stood);
}
