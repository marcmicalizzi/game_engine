// A swaying mesh's form by ring, at the load (G204; sway_forms.h; renderer.md, "A swaying mesh's
// form by ring"): a streamed entry's resident mesh that carries the motion contract gets a swaying
// cut in a ring where its motion is seen and a still one where it is not, by what the mesh
// measures and never by who placed it. A placement generator of this test's own and a container
// the test writes — the procedural grid's DAG with a two-part motion contract — so it needs no
// capability and no device.
#include <core/math/world.h>
#include <domain/geometry/cluster_file.h>
#include <domain/scene_gen/scene_gen.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/flora_motion.h>
#include <systems/renderer/ring_forms.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/sway_forms.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

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

struct ScopedTunable {
  tunables::Tunable* tunable = nullptr;
  std::string previous;
  ScopedTunable(const char* name, const char* value) {
    tunable = tunables::find(name);
    REQUIRE_MESSAGE(tunable != nullptr, name);
    tunable->append_value(previous);
    REQUIRE(tunable->set_from_text(value));
  }
  ~ScopedTunable() {
    if (tunable != nullptr) (void)tunable->set_from_text(previous);
  }
};

// One resident mesh, one placement a tile: all the load's rule needs of an entry.
struct Plants {
  std::string path;
};
bool plants_open(const JsonValue& params, const scene_gen::Context&, void** state,
                 std::string* error) {
  const JsonValue* path = params.find("path");
  std::string_view text;
  if (path == nullptr || !path->get_string(text)) {
    if (error != nullptr) *error = "sway-forms-plants needs a path";
    return false;
  }
  auto* p = new Plants;
  p->path = std::string(text);
  *state = p;
  return true;
}
void plants_close(void* state) noexcept { delete static_cast<Plants*>(state); }
bool plants_expand(void* state, const scene_gen::Context&, scene_gen::Placements& out,
                   std::string*) {
  out.meshes.push_back(
      scene_gen::PlacementMesh{static_cast<const Plants*>(state)->path, "sway-forms-plant"});
  scene_gen::Placement p;
  p.mesh = 0;
  out.instances.push_back(p);
  return true;
}
bool plants_meshes(void* state, const scene_gen::Context&, Vector<scene_gen::PlacementMesh>& out,
                   std::string*) {
  out.push_back(
      scene_gen::PlacementMesh{static_cast<const Plants*>(state)->path, "sway-forms-plant"});
  return true;
}
bool plants_tile(void*, scene_gen::TileCoord tile, const scene_gen::Context& context,
                 scene_gen::Placements& out, std::string*) {
  const f64 t = static_cast<f64>(context.tile_size);
  scene_gen::Placement p;
  p.mesh = 0;
  p.position = WorldPos{t * (tile.x + 0.5), 0.0, t * (tile.z + 0.5)};
  out.instances.push_back(p);
  return true;
}
u8 plants_representation(const void*, const scene_gen::Context&) noexcept { return 0; }
constexpr scene_gen::PlacementGeneratorDesc k_plants{.name = "sway-forms-plants",
                                                     .open = &plants_open,
                                                     .close = &plants_close,
                                                     .expand = &plants_expand,
                                                     .meshes = &plants_meshes,
                                                     .tile = &plants_tile,
                                                     .representation = &plants_representation};
const scene_gen::Registrar k_plants_registrar{k_plants};

std::string json_path(std::string path) {
  for (char& c : path)
    if (c == '\\') c = '/';
  return path;
}

// The procedural grid's DAG (20 m across) as a container with a motion contract of the kind
// `kind` names: a root at the grid's middle and one part from it that every vertex rides at a
// twentieth of its bend, so the mesh's reach is about half a metre in a mean wind.
void write_plant(const test::TempDir& tmp, const char* file_name, const char* kind) {
  std::string error;
  SceneDesc grid;
  grid.meshes.push_back("");
  grid.heightfield_grid = 33;
  grid.cache = false;
  SceneData made;
  REQUIRE_MESSAGE(load_scene(grid, made, error), error);
  geometry::ClusterFileData file;
  file.mesh = std::move(made.lod);
  Vec3 lo = file.mesh.mesh.vertices[0];
  Vec3 hi = lo;
  for (const Vec3& v : file.mesh.mesh.vertices) {
    lo = min(lo, v);
    hi = max(hi, v);
  }
  const Vec3 middle = (lo + hi) * 0.5f;
  geometry::ClusterFileMotionPart root;
  root.pivot = middle;
  root.direction = Vec3{0.0f, 1.0f, 0.0f};
  root.length = 1.0f;
  root.parent = geometry::k_motion_no_parent;
  geometry::ClusterFileMotionPart frond = root;
  frond.parent = 0;
  file.motion_parts.push_back(root);
  file.motion_parts.push_back(frond);
  file.motion_vertices.resize(file.mesh.mesh.vertices.size(),
                              geometry::ClusterFileMotionVertex{1, 3277});
  file.generator = "sway-forms-test";
  file.generator_kind = kind;
  REQUIRE_MESSAGE(geometry::write_cluster_file(tmp.file(file_name), file, &error), error);
}

// Rings of 1.5, 300 and 600 tiles of 8 m: floors of 4.3 m, 2.39 km and 4.79 km, the last two far
// enough for the grid to drop to its top levels (ring_forms_tests.cpp's own scene).
bool load_plants(const test::TempDir& tmp, const char* file_name, SceneData& scene,
                 std::string& error) {
  const std::string text = std::string(R"({"format":"engine.scene.v1","name":"sway-forms",)") +
                           R"("placements":[{"generator":"sway-forms-plants","params":{"path":")" +
                           json_path(tmp.file(file_name)) +
                           R"("}}],"world":{"tile_size":8,"rings":[{"radius":1.5},)"
                           R"({"radius":300},{"radius":600},{"radius":900}]},)" +
                           R"("terrain":{"size":17,"extent":40,"seed":3,"dune_height":0.0}})";
  if (!write_text(tmp.file("scene.json"), text)) return false;
  SceneDesc desc;
  if (!read_scene_file(tmp.file("scene.json"), desc, error)) return false;
  desc.cache = false;
  desc.authored.proj_scale = 1000.0f;  // a pixel is a millimetre a metre of distance
  return load_scene(desc, scene, error);
}

}  // namespace

TEST_CASE(
    "sway forms: a swaying mesh takes a swaying cut where it is seen to move, a still one "
    "past it") {
  const test::TempDir tmp("renderer_sway_forms");
  REQUIRE(tmp.ok());
  write_plant(tmp, "fern.clusters", "fern");
  const ScopedTunable motion("renderer.flora.motion", "true");
  const ScopedTunable wind("renderer.forms.still_wind", "1");
  // The plant's reach is about 0.54 m: 0.22 px from ring 2's floor and 0.11 px from ring 3's at a
  // projection of 1000. At 0.225 px a placement up to its own size stands still in ring 2, short
  // of the 1.5 a ring's cut serves, so ring 2 keeps a swaying cut; in ring 3 one up to twice its
  // size does, and the ring's form stands still.
  const ScopedTunable px("renderer.forms.still_px", "0.225");
  std::string error;
  SceneData scene;
  REQUIRE_MESSAGE(load_plants(tmp, "fern.clusters", scene, error), error);
  REQUIRE(scene.streamed.size() == 1u);
  const u32 source = scene.streamed[0].meshes[0];
  const RingFormTable& table = scene.ring_forms;
  REQUIRE(table.rings == 4u);
  REQUIRE(source < table.meshes.size());
  const MeshRingForms& forms = table.meshes[source];
  REQUIRE(forms.active);
  CHECK(table.swaying == 1u);
  REQUIRE(scene.sources[source].has_motion());
  // Ring 1's floor is inside the mesh: its cut keeps everything, and is the mesh itself.
  CHECK(forms.ring[1].mesh == source);
  // Ring 2: a cut of fewer clusters that still carries the contract, its parts' source ids kept.
  const u32 swaying = forms.ring[2].mesh;
  REQUIRE(swaying != source);
  REQUIRE(swaying < scene.parts.size());
  CHECK(scene.parts[swaying].cluster_count < scene.parts[source].cluster_count);
  CHECK(scene.sources[swaying].has_motion());
  CHECK(scene.sources[swaying].motion_part_ids.size() ==
        scene.sources[swaying].motion_parts.size());
  CHECK(scene.form_source[swaying] == source);
  CHECK(scene.form_rigid[swaying] == 0u);
  CHECK(forms.ring[2].max_scale == doctest::Approx(1.5f));
  // Ring 3: the still cut, with no contract, never a static form, padded as the mesh's own
  // instances are, and serving the scales that stand still there.
  const u32 still = forms.ring[3].mesh;
  REQUIRE(still != swaying);
  REQUIRE(still != source);
  REQUIRE(still < scene.parts.size());
  CHECK(forms.still_from == 3u);
  CHECK(table.stills == 1u);
  CHECK_FALSE(scene.sources[still].has_motion());
  CHECK(scene.form_source[still] == source);
  CHECK(scene.form_rigid[still] == 0u);
  CHECK(scene.parts[still].cluster_count <= scene.parts[swaying].cluster_count);
  REQUIRE(still < scene.still_padding.size());
  FloraMesh pose;
  const SourceMesh& own = scene.sources[source];
  REQUIRE(flora_mesh({own.motion_parts.data(), own.motion_parts.size()},
                     {own.structural_parts.data(), own.structural_parts.size()}, own.species,
                     pose));
  const geometry::ClusterMeshPart& part = scene.parts[source];
  const f32 bound =
      flora_bounds_padding(pose,
                           std::span<const Vec3>(scene.lod.mesh.vertices.data() + part.first_vertex,
                                                 own.motion_vertices.size()),
                           {own.motion_vertices.data(), own.motion_vertices.size()});
  CHECK(bound > 0.5f);
  CHECK(scene.still_padding[still] == bound);
  CHECK(scene.still_padding[source] == 0.0f);
  CHECK(scene.still_padding[swaying] == 0.0f);
  // Its largest still scale there is past what a cut serves, so the entry serves a cut's scales.
  const f32 reach_px = 0.5f * bound * 1000.0f / table.floor_m[3];
  CHECK(0.225f / reach_px >= 1.5f);
  CHECK(0.225f / (0.5f * bound * 1000.0f / table.floor_m[2]) < 1.5f);
  CHECK(forms.ring[3].max_scale == doctest::Approx(1.5f));

  // The read: each ring's form for every placement a cut serves; past that, the mesh itself.
  const Vec3 one = Vec3::one();
  const Vec3 large{1.4f, 1.4f, 1.4f};
  CHECK(pick_ring_form(table, source, 0, one) == source);
  CHECK(pick_ring_form(table, source, 2, one) == swaying);
  CHECK(pick_ring_form(table, source, 2, large) == swaying);
  CHECK(pick_ring_form(table, source, 3, one) == still);
  CHECK(pick_ring_form(table, source, 3, large) == still);
  CHECK(pick_ring_form(table, source, 3, Vec3{2.0f, 2.0f, 2.0f}) == source);
  const u32 subject[1] = {source};
  CHECK_FALSE(same_ring_forms(table, subject, 2, 3));

  // A still form's instance takes the padding; the mesh's and the swaying cut's take theirs where
  // the flora stage makes their records.
  SceneInstance placed;
  placed.mesh = still;
  gfx::InstanceDesc desc;
  REQUIRE(make_instance(scene, placed, 0, desc, &error));
  CHECK(desc.bounds_padding == bound);
  placed.mesh = swaying;
  REQUIRE(make_instance(scene, placed, 0, desc, &error));
  CHECK(desc.bounds_padding == 0.0f);

  // Never by its size: every ring past the first a swaying cut. And the rule off: the mesh draws
  // itself in every ring, as before.
  {
    const ScopedTunable never("renderer.forms.still_px", "0");
    SceneData cuts;
    REQUIRE_MESSAGE(load_plants(tmp, "fern.clusters", cuts, error), error);
    const MeshRingForms& f = cuts.ring_forms.meshes[cuts.streamed[0].meshes[0]];
    REQUIRE(f.active);
    CHECK(f.still_from == 0u);
    CHECK(cuts.ring_forms.stills == 0u);
    CHECK(cuts.sources[f.ring[3].mesh].has_motion());
    CHECK(f.ring[3].mesh == f.ring[2].mesh);
  }
  {
    const ScopedTunable off("renderer.forms.sway", "false");
    SceneData whole;
    REQUIRE_MESSAGE(load_plants(tmp, "fern.clusters", whole, error), error);
    const u32 m = whole.streamed[0].meshes[0];
    CHECK(pick_ring_form(whole.ring_forms, m, 3, one) == m);
    CHECK(whole.ring_forms.swaying == 0u);
  }
}

TEST_CASE(
    "sway forms: a field stands still in the rings past its rest distance, and a session "
    "with no motion forms a plant as a rigid mesh") {
  const test::TempDir tmp("renderer_sway_forms_field");
  REQUIRE(tmp.ok());
  write_plant(tmp, "grass.clusters", "sward");
  const ScopedTunable motion("renderer.flora.motion", "true");
  const ScopedTunable wind("renderer.forms.still_wind", "1");
  const ScopedTunable px("renderer.forms.still_px", "0");  // never by its size
  std::string error;
  {
    // The far field's rest at 120 m: ring 1's floor (4.3 m) is short of it, ring 2's (2.39 km)
    // past it by far more than the roots' reach.
    const ScopedTunable rest("renderer.flora.field_rest_m", "120");
    SceneData scene;
    REQUIRE_MESSAGE(load_plants(tmp, "grass.clusters", scene, error), error);
    const u32 source = scene.streamed[0].meshes[0];
    const MeshRingForms& forms = scene.ring_forms.meshes[source];
    REQUIRE(forms.active);
    CHECK(forms.ring[1].mesh == source);
    CHECK(forms.still_from == 2u);
    const u32 still = forms.ring[2].mesh;
    REQUIRE(still != source);
    CHECK_FALSE(scene.sources[still].has_motion());
    CHECK(scene.form_rigid[still] == 0u);
    CHECK(scene.still_padding[still] > 0.0f);
    CHECK(forms.ring[2].max_scale == doctest::Approx(1.5f));  // at rest at every scale a cut serves
  }
  {
    // No rest distance and no size rule: the field sways in every ring, as cuts.
    const ScopedTunable rest("renderer.flora.field_rest_m", "0");
    SceneData scene;
    REQUIRE_MESSAGE(load_plants(tmp, "grass.clusters", scene, error), error);
    const MeshRingForms& forms = scene.ring_forms.meshes[scene.streamed[0].meshes[0]];
    REQUIRE(forms.active);
    CHECK(forms.still_from == 0u);
    CHECK(scene.sources[forms.ring[2].mesh].has_motion());
  }
  {
    // A session that draws no motion: the contract is not posed, so the mesh is rigid to the rule
    // — a cut with no contract and no padding in every ring that drops a cluster.
    const ScopedTunable still("renderer.flora.motion", "false");
    SceneData scene;
    REQUIRE_MESSAGE(load_plants(tmp, "grass.clusters", scene, error), error);
    const u32 source = scene.streamed[0].meshes[0];
    const MeshRingForms& forms = scene.ring_forms.meshes[source];
    REQUIRE(forms.active);
    CHECK(scene.ring_forms.swaying == 0u);
    const u32 far = forms.ring[2].mesh;
    REQUIRE(far != source);
    CHECK_FALSE(scene.sources[far].has_motion());
    CHECK((scene.still_padding.empty() || scene.still_padding[far] == 0.0f));
  }
}
