// A placement's form by ring (G197; renderer/ring_forms.h; world.md, "A placement's form by ring"):
// the load's rule over what any mesh has — its DAG's cut for a ring's floor, its size, a form it
// offers — and the world's placements consumer drawing a streamed placement of a resident mesh as
// its ring's form. A placement generator of this test's own and the procedural grid's DAG as its
// mesh: no capability, so it runs in every configuration with the world.
#include <core/math/world.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/renderer/ring_forms.h>
#include <systems/renderer/scene.h>
#include <systems/world/placement_tiles.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <fstream>
#include <string>

using namespace engine;
using namespace engine::world;

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// A placement every `step` metres over the tile, one mesh, resident; every fourth at scale 2.
struct Field {
  std::string path;
  f64 step = 2.0;
};
bool field_open(const JsonValue& params, const scene_gen::Context&, void** state,
                std::string* error) {
  const JsonValue* path = params.find("path");
  std::string_view text;
  if (path == nullptr || !path->get_string(text)) {
    if (error != nullptr) *error = "ring-forms-field needs a path";
    return false;
  }
  auto* f = new Field;
  f->path = std::string(text);
  *state = f;
  return true;
}
void field_close(void* state) noexcept { delete static_cast<Field*>(state); }
void field_place(const Field& f, f64 x0, f64 z0, f64 size, scene_gen::Placements& out) {
  const i32 n = static_cast<i32>(size / f.step);
  for (i32 j = 0; j < n; ++j) {
    for (i32 i = 0; i < n; ++i) {
      scene_gen::Placement p;
      p.mesh = 0;
      p.position = WorldPos{x0 + f.step * (static_cast<f64>(i) + 0.5), 0.0,
                            z0 + f.step * (static_cast<f64>(j) + 0.5)};
      if ((i + j) % 4 == 0) p.scale = Vec3{2.0f, 2.0f, 2.0f};
      out.instances.push_back(p);
    }
  }
}
bool field_expand(void* state, const scene_gen::Context&, scene_gen::Placements& out,
                  std::string*) {
  const Field& f = *static_cast<const Field*>(state);
  out.meshes.push_back(scene_gen::PlacementMesh{f.path, "ring-forms-patch"});
  field_place(f, 0.0, 0.0, 8.0, out);
  return true;
}
bool field_meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
                  std::string*) {
  out.push_back(
      scene_gen::PlacementMesh{static_cast<const Field*>(state)->path, "ring-forms-patch"});
  return true;
}
bool field_tile(void* state, scene_gen::TileCoord tile, const scene_gen::Context& context,
                scene_gen::Placements& out, std::string*) {
  const f64 t = static_cast<f64>(context.tile_size);
  field_place(*static_cast<const Field*>(state), t * tile.x, t * tile.z, t, out);
  return true;
}
// One representation in every ring, as a generator that places one mesh says: only the forms
// make a ring change a rebuild.
u8 field_representation(const void*, const scene_gen::Context&) noexcept { return 0; }
constexpr scene_gen::PlacementGeneratorDesc k_field{.name = "ring-forms-field",
                                                    .open = &field_open,
                                                    .close = &field_close,
                                                    .expand = &field_expand,
                                                    .meshes = &field_meshes,
                                                    .tile = &field_tile,
                                                    .representation = &field_representation};
const scene_gen::Registrar k_field_registrar{k_field};

std::string json_path(std::string path) {
  for (char& c : path)
    if (c == '\\') c = '/';
  return path;
}

TileEvent event(TileEventKind kind, i32 x, i32 z, u8 from, u8 to) {
  TileEvent e;
  e.tile = TileCoord{x, z};
  e.kind = kind;
  e.from = from;
  e.to = to;
  return e;
}

}  // namespace

TEST_CASE("ring forms: the rule picks the cheapest form that serves a ring, or nothing") {
  using namespace engine::renderer;
  const f32 floors[4] = {0.0f, 10.0f, 100.0f, 1000.0f};
  // A 1 m mesh of 100 clusters (scene mesh 5) whose cuts keep 20, then 4, then the same 4.
  const RingCut cuts[4] = {{5, 100}, {6, 20}, {7, 4}, {7, 4}};
  RingFormRules rules;
  rules.proj_scale = 1000.0f;
  rules.cut_scale = 1.5f;
  rules.min_px = 1.0f;
  RingFormTable table;
  table.rings = 4;
  table.meshes.resize(8);
  table.meshes[5] = choose_ring_forms(5, 100, 1.0f, floors, cuts, {}, rules);
  const MeshRingForms& f = table.meshes[5];
  REQUIRE(f.active);
  CHECK(f.ring[0].mesh == 5u);
  CHECK(f.ring[1].mesh == 6u);
  CHECK(f.ring[1].kind == RingFormKind::cut);
  CHECK(f.ring[2].mesh == 7u);
  CHECK(f.ring[3].mesh == 7u);
  // 1 m at 1000 m and a projection of 1000 is 1 px: a placement any smaller is under `min_px`.
  CHECK(f.ring[3].none_below_scale == doctest::Approx(1.0f));
  const Vec3 one = Vec3::one();
  CHECK(pick_ring_form(table, 5, 0, one) == 5u);
  CHECK(pick_ring_form(table, 5, 1, one) == 6u);
  CHECK(pick_ring_form(table, 5, 2, one) == 7u);
  CHECK(pick_ring_form(table, 5, 3, Vec3{0.9f, 0.9f, 0.9f}) == k_no_ring_form);
  // Past what a cut serves (1.5), the ring before's; past every cut's, the mesh itself.
  CHECK(pick_ring_form(table, 5, 2, Vec3{1.2f, 1.0f, 1.0f}) == 7u);
  CHECK(pick_ring_form(table, 5, 2, Vec3{1.0f, 2.0f, 1.0f}) == 5u);
  // A ring past the table's draws its last ring's entry; a mesh past the table, an inactive one
  // and a tile mesh's `~0u` are themselves.
  CHECK(pick_ring_form(table, 5, 6, one) == 7u);
  CHECK(pick_ring_form(table, 4, 2, one) == 4u);
  CHECK(pick_ring_form(table, 40, 2, one) == 40u);
  CHECK(pick_ring_form(table, ~0u, 2, one) == ~0u);
  // Two rings differ where `min_px` differs, and are one where it is off and the cut is the same.
  const u32 subject[1] = {5};
  CHECK_FALSE(same_ring_forms(table, subject, 2, 3));
  CHECK_FALSE(same_ring_forms(table, subject, 0, 1));
  CHECK(same_ring_forms(table, subject, 2, 2));
  rules.min_px = 0.0f;
  table.meshes[5] = choose_ring_forms(5, 100, 1.0f, floors, cuts, {}, rules);
  CHECK(same_ring_forms(table, subject, 2, 3));
  CHECK(pick_ring_form(table, 5, 3, Vec3{0.01f, 0.01f, 0.01f}) == 7u);

  // **A form the mesh offers** (a far form its container carries): drawn where it serves the
  // mesh's projected diameter and costs fewer pairs, with the scale it serves as its bound.
  const OfferedForm offered[2] = {{5, 9, 2, 20.0f}, {3, 10, 1, 1000.0f}};
  rules.min_px = 1.0f;
  table.meshes[5] = choose_ring_forms(5, 100, 1.0f, floors, cuts, offered, rules);
  CHECK(f.ring[1].mesh == 6u);  // 100 px: over the 20 it serves
  CHECK(f.ring[2].mesh == 9u);  // 10 px: it serves, at 2 pairs against the cut's 4
  CHECK(f.ring[2].kind == RingFormKind::offered);
  CHECK(f.ring[2].max_scale == doctest::Approx(2.0f));
  CHECK(f.ring[3].mesh == 9u);
  CHECK(pick_ring_form(table, 5, 2, Vec3{3.0f, 3.0f, 3.0f}) == 5u);
  // Off: every ring the mesh itself.
  rules.enabled = false;
  CHECK_FALSE(choose_ring_forms(5, 100, 1.0f, floors, cuts, offered, rules).active);
}

TEST_CASE("ring forms: a streamed placement draws its ring's form, and stands where it stood") {
  const test::TempDir tmp("world_ring_forms");
  REQUIRE(tmp.ok());
  std::string error;
  // The patch: the procedural grid's DAG (20 m across), written as a container.
  {
    renderer::SceneDesc grid;
    grid.meshes.push_back("");
    grid.heightfield_grid = 33;
    grid.cache = false;
    renderer::SceneData made;
    REQUIRE_MESSAGE(renderer::load_scene(grid, made, error), error);
    geometry::ClusterFileData file;
    file.mesh = std::move(made.lod);
    REQUIRE_MESSAGE(geometry::write_cluster_file(tmp.file("patch.clusters"), file, &error), error);
  }
  const std::string patch = json_path(tmp.file("patch.clusters"));
  // Rings of 1.5, 300 and 600 tiles of 8 m: the floors are 4.3 m and 2.39 km, far enough for the
  // grid's rough relief (its coarse levels' errors are decimetres) to drop to its top levels.
  const std::string text = std::string(R"({"format":"engine.scene.v1","name":"ring-forms",)") +
                           R"("placements":[{"generator":"ring-forms-field","params":{"path":")" +
                           patch +
                           R"("}}],"world":{"tile_size":8,"rings":[{"radius":1.5},)"
                           R"({"radius":300},{"radius":600}]},)" +
                           R"("terrain":{"size":17,"extent":40,"seed":3,"dune_height":0.0}})";
  REQUIRE(write_text(tmp.file("scene.json"), text));
  renderer::SceneDesc desc;
  REQUIRE_MESSAGE(renderer::read_scene_file(tmp.file("scene.json"), desc, error), error);
  desc.cache = false;
  renderer::SceneData scene;
  REQUIRE_MESSAGE(renderer::load_scene(desc, scene, error), error);
  REQUIRE(scene.streamed.size() == 1u);
  const u32 source = scene.streamed[0].meshes[0];
  const renderer::RingFormTable& table = scene.ring_forms;
  REQUIRE(table.subjects == 1u);
  REQUIRE(table.rings == 3u);
  REQUIRE(source < table.meshes.size());
  const renderer::MeshRingForms& forms = table.meshes[source];
  REQUIRE(forms.active);
  // Ring 1's floor is inside the patch's reach: it keeps everything, and is the mesh itself.
  CHECK(forms.ring[1].mesh == source);
  // Ring 2's is a cut of fewer clusters, a mesh of its own placed as its source is.
  const u32 far = forms.ring[2].mesh;
  REQUIRE(far != source);
  REQUIRE(far < scene.parts.size());
  CHECK(forms.ring[2].kind == renderer::RingFormKind::cut);
  CHECK(scene.form_source[far] == source);
  CHECK(scene.form_rigid[far] == 1u);
  CHECK(scene.parts[far].cluster_count < scene.parts[source].cluster_count);
  CHECK(table.cuts >= 1u);

  PlacementTilesConfig config;
  config.tile_size = 8.0f;
  PlacementTiles tiles;
  REQUIRE_MESSAGE(tiles.create(scene, config, &error), error);
  const TileConsumer consumer = tiles.consumer();
  // The same tile drawn in ring 0, then in ring 2: the same placements in the same places, each
  // the mesh near and its ring's form far, and a scale-2 one the mesh itself (over the 1.5 a cut
  // serves).
  REQUIRE(consumer.activate(consumer.context, event(TileEventKind::Activate, 0, 0, k_inactive, 0)));
  consumer.commit(consumer.context);
  const std::span<const renderer::SceneInstance> drawn = tiles.tile_instances(TileCoord{0, 0});
  Vector<renderer::SceneInstance> near;
  near.assign(drawn.begin(), drawn.end());
  REQUIRE(near.size() == 16u);
  const u32 near_pairs = tiles.stats().ring_pairs[0];
  for (const renderer::SceneInstance& s : near)
    CHECK(s.mesh == source);
  // One representation, so only the forms say a move from ring 0 or 1 to ring 2 is a rebuild.
  CHECK(tiles.redraws(TileCoord{0, 0}, 0, 2));
  CHECK(tiles.redraws(TileCoord{0, 0}, 1, 2));
  CHECK_FALSE(tiles.redraws(TileCoord{0, 0}, 2, 2));
  REQUIRE(consumer.change_ring(consumer.context, event(TileEventKind::ChangeRing, 0, 0, 0, 2)));
  consumer.commit(consumer.context);
  const std::span<const renderer::SceneInstance> farther = tiles.tile_instances(TileCoord{0, 0});
  REQUIRE(farther.size() == near.size());
  u32 formed = 0;
  for (u32 i = 0; i < farther.size(); ++i) {
    CHECK(farther[i].origin == near[i].origin);
    const bool large = farther[i].transform.scale.x > 1.5f;
    CHECK(farther[i].mesh == (large ? source : far));
    formed += farther[i].mesh == far ? 1u : 0u;
  }
  CHECK(formed == 12u);
  const u32 far_pairs = tiles.stats().ring_pairs[2];
  CHECK(far_pairs < near_pairs);
  MESSAGE("16 placements of a " << scene.parts[source].cluster_count
                                << "-cluster patch: " << near_pairs << " pairs in ring 0, "
                                << far_pairs << " in ring 2 (its cut "
                                << scene.parts[far].cluster_count << " clusters)");
}
