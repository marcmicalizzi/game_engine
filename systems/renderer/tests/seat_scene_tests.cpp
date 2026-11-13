// A tree seated by its footprint (G201 stage 2; renderer.md, "A tree seated by its footprint";
// scene_gen/seat.h), with a ground and a placement generator of this test's own, so it runs in
// every configuration: a bank a metre high on a gentle slope, and a mesh whose container records a
// seat (section 42). Grounded instances and a generator's placements whose rim stands over the
// bank's edge are lowered until the skirt is under the bank's foot; one standing clear, one placed
// at a height of its own and one whose mesh has no seat are not moved; the seat is read back from
// the container; the load at one thread and at many seats alike; and the bytes are pinned.
#include <core/hash/hash.h>
#include <core/math/world.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/seat.h>
#include <systems/renderer/ground_sample.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/terrain.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// A slope of 0.2 along z with a bank `dune_height` metres down past x = 0.
struct Bank {
  f32 height = 1.0f;
};
f32 bank_height(const void* state, f32 x, f32 z) noexcept {
  const f32 drop = x >= 0.0f ? static_cast<const Bank*>(state)->height : 0.0f;
  return 0.2f * z - drop;
}
void bank_destroy(void* state) noexcept { delete static_cast<Bank*>(state); }
constexpr scene_gen::GroundOps k_bank_ops{.destroy = &bank_destroy, .height = &bank_height};
bool bank_make(const scene::Terrain& entry, const scene_gen::Context&,
               scene_gen::GroundProvider& out, std::string*) {
  out = scene_gen::GroundProvider(&k_bank_ops, new Bank{entry.dune_height});
  return true;
}
constexpr scene_gen::GroundProviderDesc k_bank{.name = "seat-test-bank", .make = &bank_make};
const scene_gen::Registrar k_bank_registrar{k_bank};

// A row of one mesh at the x's its entry lists, z 1, standing on the ground.
struct Trees {
  std::string path;
  Vector<f64> xs;
};
bool trees_open(const JsonValue& params, const scene_gen::Context&, void** state,
                std::string* error) {
  auto* t = new Trees;
  const JsonValue* path = params.find("path");
  const JsonValue* xs = params.find("xs");
  std::string_view text;
  if (path == nullptr || !path->get_string(text) || xs == nullptr || !xs->is_array()) {
    delete t;
    if (error != nullptr) *error = "seat-test-trees needs a path and xs";
    return false;
  }
  t->path = std::string(text);
  for (u32 i = 0; i < xs->size(); ++i) {
    f64 x = 0.0;
    (void)(*xs)[i].get_f64(x);
    t->xs.push_back(x);
  }
  *state = t;
  return true;
}
void trees_close(void* state) noexcept { delete static_cast<Trees*>(state); }
bool trees_expand(void* state, const scene_gen::Context& context, scene_gen::Placements& out,
                  std::string*) {
  const Trees& t = *static_cast<const Trees*>(state);
  out.meshes.push_back(scene_gen::PlacementMesh{t.path, "seat-test-tree"});
  for (const f64 x : t.xs) {
    scene_gen::Placement p;
    p.position = WorldPos{x, 0.0, 1.0};
    p.position.y = context.ground.surface(p.position);
    out.instances.push_back(p);
  }
  return true;
}
constexpr scene_gen::PlacementGeneratorDesc k_trees{
    .name = "seat-test-trees", .open = &trees_open, .close = &trees_close, .expand = &trees_expand};
const scene_gen::Registrar k_trees_registrar{k_trees};

// A small mesh (the procedural grid's DAG) written as a container with a seat: a rim of 0.4 m, a
// collar 2 cm up and a skirt 0.55 m under the origin, the numbers of a young tree.
constexpr geometry::ClusterFileSeat k_seat{0.4f, 0.02f, -0.55f};
std::string write_tree(const test::TempDir& tmp, bool seated, const char* name) {
  SceneDesc grid;
  grid.meshes.push_back("");
  grid.heightfield_grid = 9;
  grid.cache = false;
  SceneData made;
  std::string error;
  REQUIRE_MESSAGE(load_scene(grid, made, error), error);
  geometry::ClusterFileData file;
  file.mesh = std::move(made.lod);
  if (seated) file.seat = k_seat;
  const std::string path = tmp.file(name);
  REQUIRE_MESSAGE(geometry::write_cluster_file(path, file, &error), error);
  return path;
}

// A path as a JSON string's body: forward slashes, which every host reads.
std::string json_path(std::string path) {
  for (char& c : path)
    if (c == '\\') c = '/';
  return path;
}

// The generator's mesh is named by an absolute path: a generator's path is taken as it gives it.
std::string scene_text(const std::string& tree) {
  return R"({"format":"engine.scene.v1","name":"seats",)"
         R"("meshes":[{"name":"tree","path":"tree.clusters"},{"name":"prop","path":"prop.clusters"}],)"
         R"("instances":[)"
         // 0: its rim over the bank's edge; 1: clear of it; 2: placed at a height of its own over
         // the edge; 3: twice the size, its rim reaching the edge from farther; 4: no seat.
         R"({"mesh":0,"translation":[-0.2,0,0],"ground":true},)"
         R"({"mesh":0,"translation":[-3,0,0],"ground":true},)"
         R"({"mesh":0,"translation":[-0.2,0.5,0]},)"
         R"({"mesh":0,"translation":[-0.6,0,0],"ground":true,"scale":[2,2,2]},)"
         R"({"mesh":1,"translation":[-0.2,0,0],"ground":true}],)"
         R"("placements":[{"generator":"seat-test-trees",)"
         R"("params":{"path":")" +
         json_path(tree) +
         R"(","xs":[-0.25,-0.1,-4]}}],)"
         R"("terrain":{"size":17,"extent":20,"dune_height":1,"provider":"seat-test-bank"}})";
}

// The lowest ground under a rim of `r` round (x, z) on the bank, in whole millimetres, by brute
// force over the sixteen directions.
i64 lowest_mm(f64 x, f64 z, f64 r) {
  const Bank bank{1.0f};
  const auto h = [&](i64 xm, i64 zm) {
    return scene_gen::nearest_mm(
        static_cast<f64>(bank_height(&bank, static_cast<f32>(static_cast<f64>(xm) / 1000.0),
                                     static_cast<f32>(static_cast<f64>(zm) / 1000.0))));
  };
  const i64 cx = scene_gen::nearest_mm(x);
  const i64 cz = scene_gen::nearest_mm(z);
  i64 lowest = h(cx, cz);
  for (u32 k = 0; k < scene_gen::k_seat_rim_samples; ++k) {
    i64 dx = 0, dz = 0;
    scene_gen::seat_rim_offset(scene_gen::nearest_mm(r), k, dx, dz);
    lowest = std::min(lowest, h(cx + dx, cz + dz));
  }
  return lowest;
}

}  // namespace

TEST_CASE("seat: grounded trees over a bank's edge are lowered by their footprint, nothing else") {
  const test::TempDir tmp("renderer_seat_scene");
  REQUIRE(tmp.ok());
  const std::string tree = write_tree(tmp, true, "tree.clusters");
  write_tree(tmp, false, "prop.clusters");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_text(path, scene_text(tree)));
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(path, desc, error), error);
  // The file's five, the generator's three, the terrain.
  REQUIRE(desc.instances.size() == 5u + 3u + 1u);
  CHECK(desc.instances[0].grounded);
  CHECK_FALSE(desc.instances[2].grounded);
  CHECK(desc.instances[5].grounded);  // a generator's placement stands on the ground
  // The sources as the load reads them: the seat back from the container.
  SceneData data;
  desc.cache = false;
  desc.ddc = tmp.file("ddc");
  desc.load_jobs = 1;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  REQUIRE(data.sources.size() >= 2u);
  CHECK(data.sources[0].seat.radius_m == k_seat.radius_m);
  CHECK(data.sources[0].seat.skirt_y_m == k_seat.skirt_y_m);
  CHECK_FALSE(data.sources[1].seat.radius_m > 0.0f);
  // Which were seated and which lowered: 0, 1, 3, 5, 6, 7 seated; 0, 3, 5, 6 lowered.
  CHECK(data.load.seated == 6u);
  CHECK(data.load.lowered == 4u);

  // The rule on the read's instances, the sources' seats as loaded.
  Vector<SceneInstance> seated = desc.instances;
  SceneLoadReport report;
  seat_scene_instances(desc.terrain, {seated.data(), seated.size()},
                       {data.sources.data(), data.sources.size()},
                       {desc.mesh_info.data(), desc.mesh_info.size()}, report);
  const i64 skirt_mm = 550;
  // 0: the origin goes to the lowest ground under its rim plus the skirt less the margin.
  CHECK(scene_gen::nearest_mm(seated[0].origin.y) ==
        lowest_mm(-0.2, 0.0, 0.4) + skirt_mm - scene_gen::k_seat_margin_mm);
  CHECK(seated[0].origin.y < -0.4);
  // 1: clear of the bank, on the slope's plane, which the skirt reaches: unmoved, its slope 0.2.
  CHECK(seated[1].origin == desc.instances[1].origin);
  CHECK(seated[1].slope.z() == doctest::Approx(0.2).epsilon(0.002));
  CHECK(std::fabs(seated[1].slope.x()) < 0.002);
  // 2: at a height of its own; 4: no seat. Neither moves.
  CHECK(seated[2].origin == desc.instances[2].origin);
  CHECK(seated[4].origin == desc.instances[4].origin);
  CHECK(seated[4].slope == scene_gen::GroundSlope{});
  // 3: twice the size, so a rim of 0.8 m over the edge and a skirt of 1.1 m.
  CHECK(scene_gen::nearest_mm(seated[3].origin.y) ==
        std::min(i64{0}, lowest_mm(-0.6, 0.0, 0.8) + 1100 - scene_gen::k_seat_margin_mm));
  // 5, 6: the generator's over the edge; 7 clear.
  CHECK(seated[5].origin.y < 0.2 - 0.4);
  CHECK(seated[6].origin.y < 0.2 - 0.4);
  CHECK(seated[7].origin == desc.instances[7].origin);
  // Seated twice is seated once.
  Vector<SceneInstance> twice = seated;
  SceneLoadReport again;
  seat_scene_instances(desc.terrain, {twice.data(), twice.size()},
                       {data.sources.data(), data.sources.size()},
                       {desc.mesh_info.data(), desc.mesh_info.size()}, again);
  for (u32 i = 0; i < twice.size(); ++i)
    CHECK(twice[i].origin == seated[i].origin);
  CHECK(again.lowered == 0u);

  // The bytes: every origin's bits and every slope, pinned on MSVC (G201 stage 2), and the load
  // at every thread the same instance table and slopes as at one.
  u64 h = 0;
  for (const SceneInstance& instance : seated) {
    u64 bits[3];
    std::memcpy(&bits[0], &instance.origin.x, 8);
    std::memcpy(&bits[1], &instance.origin.y, 8);
    std::memcpy(&bits[2], &instance.origin.z, 8);
    h = hash_combine(h, hash_bytes(bits, sizeof(bits)));
    h = hash_combine(h, hash_bytes(&instance.slope, sizeof(instance.slope)));
  }
  MESSAGE("seated origins and slopes 0x" << std::hex << h);
  CHECK(h == 0x0ull);
  SceneDesc many = desc;
  many.load_jobs = 0;  // every thread the machine offers
  SceneData all;
  REQUIRE_MESSAGE(load_scene(many, all, error), error);
  REQUIRE(all.instances.size() == data.instances.size());
  CHECK(std::memcmp(all.instances.data(), data.instances.data(),
                    data.instances.size() * sizeof(data.instances[0])) == 0);
  REQUIRE(all.ground_slopes.size() == data.ground_slopes.size());
  for (u32 i = 0; i < all.ground_slopes.size(); ++i)
    CHECK(all.ground_slopes[i] == data.ground_slopes[i]);
}

TEST_CASE("seat: the drawn ground can be asked, as the load stands on it") {
  const test::TempDir tmp("renderer_seat_ground");
  REQUIRE(tmp.ok());
  const std::string tree = write_tree(tmp, true, "tree.clusters");
  const std::string path = tmp.file("scene.json");
  REQUIRE(write_text(path, scene_text(tree)));
  SceneGround ground;
  std::string error;
  REQUIRE_MESSAGE(ground.open(path, std::string(), false, false, error), error);
  const JsonValue head = ground.head();
  const JsonValue* provider = head.find("provider");
  std::string_view name;
  REQUIRE(provider != nullptr);
  REQUIRE(provider->get_string(name));
  CHECK(name == "seat-test-bank");
  // At the first instance: the disc's drop is the bank's, and the seat is the load's.
  const JsonValue answer = ground.sample(GroundQuestion{-0.2, 0.0, 0.4}, 0.55);
  const JsonValue* disc = answer.find("disc");
  REQUIRE(disc != nullptr);
  f64 seat = 0.0;
  REQUIRE(disc->find("seat_m") != nullptr);
  REQUIRE(disc->find("seat_m")->get_f64(seat));
  CHECK(scene_gen::nearest_mm(seat) ==
        lowest_mm(-0.2, 0.0, 0.4) + 550 - scene_gen::k_seat_margin_mm);
  f64 drop = 0.0;
  REQUIRE(disc->find("drop_m")->get_f64(drop));
  CHECK(drop > 0.9);
  CHECK(ground.not_carried().empty());
  // A generator this executable does not carry: the ground is still asked, through the plan alone,
  // and the head names it, since whatever it would have planned is missing. Read whole, refused.
  REQUIRE(write_text(path, R"({"format":"engine.scene.v1","name":"x","placements":[)"
                           R"({"generator":"no-such-generator","params":{}}],)"
                           R"("terrain":{"size":17,"extent":20,"provider":"seat-test-bank"}})"));
  SceneGround partial;
  REQUIRE_MESSAGE(partial.open(path, std::string(), false, false, error), error);
  REQUIRE(partial.not_carried().size() == 1u);
  CHECK(partial.not_carried()[0] == "no-such-generator");
  const JsonValue* missing = partial.head().find("not_carried");
  REQUIRE(missing != nullptr);
  CHECK(missing->size() == 1u);
  SceneGround whole;
  CHECK_FALSE(whole.open(path, std::string(), false, true, error));
  CHECK(error.find("no-such-generator") != std::string::npos);
}
