// A tile's mesh waits for its textures (docs/subsystems/world.md, "A tile's meshes: residency by
// reference"; texture.md, "Textures that arrive after the load"; roadmap R147): `TileMeshes`
// against a sink that keeps what a renderer's scene would — textures by key with a count of who
// holds each, and the meshes admitted — with the fixture's textured rocks. No device.
//
// What the cases hold: a budgeted commit stages no more texture bytes than its allowance, a
// texture larger than all of it going alone, and the mesh comes the commit after its last texture;
// a mesh whose textures are in the scene costs no upload; the table's own references go back when
// the mesh is in, and the mesh's when it is retired; a whole commit admits everything at once; a
// refused texture ends the wait, the mesh draws its constants and nothing of it stays held; and a
// mesh let go while it waited leaves no texture behind.
#include <systems/renderer/scene.h>
#include <systems/world/tile_meshes.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>
#include <tile_mesh_fixture.h>

using namespace engine;
using namespace engine::world;

namespace {

struct Scene {
  struct Texture {
    u64 key = 0;
    u32 refs = 0;
    u64 bytes = 0;
  };
  struct Mesh {
    u32 mesh = 0;
    Vector<u64> textures;
  };
  Vector<Texture> textures;
  Vector<Mesh> meshes;
  u32 uploads = 0;
  u64 uploaded_bytes = 0;  // since the test last zeroed it: one commit's
  u32 pairs = ~0u;         // textures at once
  u32 textured = 0;        // meshes admitted with their textures
  u32 plain = 0;           // and without
  u32 violations = 0;      // a mesh admitted naming a texture the scene does not hold

  Texture* find(u64 key) {
    for (Texture& t : textures)
      if (t.key == key) return &t;
    return nullptr;
  }
  void drop(u64 key) {
    for (u32 k = 0; k < textures.size(); ++k) {
      if (textures[k].key != key) continue;
      if (--textures[k].refs == 0) textures.erase(textures.begin() + k);
      return;
    }
    ++violations;
  }
  static bool fits(void*, u32, u32) { return true; }
  static bool admit(void* context, u32 mesh, const renderer::PlacementMeshData& data,
                    std::string*) {
    auto* self = static_cast<Scene*>(context);
    Mesh held;
    held.mesh = mesh;
    for (const renderer::PlacementTexture& texture : data.textures) {
      Texture* there = self->find(texture.key);
      if (there == nullptr) {
        ++self->violations;
        continue;
      }
      ++there->refs;  // the mesh's own, as `GpuScene::admit_mesh` takes
      held.textures.push_back(texture.key);
    }
    data.textures.empty() ? ++self->plain : ++self->textured;
    self->meshes.push_back(std::move(held));
    return true;
  }
  static void retire(void* context, u32 mesh) {
    auto* self = static_cast<Scene*>(context);
    for (u32 k = 0; k < self->meshes.size(); ++k) {
      if (self->meshes[k].mesh != mesh) continue;
      for (const u64 key : self->meshes[k].textures)
        self->drop(key);
      self->meshes.erase(self->meshes.begin() + k);
      return;
    }
    ++self->violations;
  }
  static TileMeshSink::Texture texture(void* context, const renderer::PlacementTexture& texture,
                                       std::string* why) {
    auto* self = static_cast<Scene*>(context);
    if (Texture* there = self->find(texture.key)) {
      ++there->refs;
      return TileMeshSink::Texture::held;
    }
    if (texture.data.levels.empty()) return TileMeshSink::Texture::need_bytes;
    if (self->textures.size() >= self->pairs) {
      if (why != nullptr) *why = "every texture pair holds a texture (renderer.textures.slots)";
      return TileMeshSink::Texture::refused;
    }
    Texture made;
    made.key = texture.key;
    made.refs = 1;
    made.bytes = texture.staged_bytes();
    self->textures.push_back(made);
    ++self->uploads;
    self->uploaded_bytes += made.bytes;
    return TileMeshSink::Texture::held;
  }
  static void release_texture(void* context, u64 key) { static_cast<Scene*>(context)->drop(key); }
  static bool texture_resident(void* context, u64 key) {
    return static_cast<Scene*>(context)->find(key) != nullptr;
  }
  TileMeshSink sink() {
    return TileMeshSink{this,
                        &Scene::fits,
                        &Scene::admit,
                        &Scene::retire,
                        &Scene::texture,
                        &Scene::release_texture,
                        &Scene::texture_resident};
  }
};

constexpr u32 k_look = test::k_tile_rock_textured;

scene_gen::PlacementMesh rock(i32 x, i32 z) {
  scene_gen::PlacementMesh mesh;
  mesh.path = test::tile_rock_path(x, z);
  mesh.name = "rock";
  mesh.recipe = test::tile_rock_recipe(x, z, 12, k_look);  // every rock the same two maps
  mesh.reach = test::k_tile_rock_reach;
  return mesh;
}

renderer::PlacementMeshOptions options_of(const test::TempDir& tmp) {
  test::register_tile_rocks();
  renderer::PlacementMeshOptions options;
  options.ddc = tmp.file("ddc");
  options.cache = true;
  return options;
}

TileMeshConfig room(u64 admission_bytes) {
  TileMeshConfig config;
  config.first_mesh = 3;
  config.slots = 8;
  config.clusters = 100000;
  config.max_clusters = 4096;
  config.cluster_bytes = 3000;
  config.linger = 0;
  config.admissions = 2;
  config.admission_bytes = admission_bytes;
  return config;
}

}  // namespace

TEST_CASE("tile mesh textures: a commit stages its allowance, and the mesh follows its textures") {
  test::TempDir tmp{"engine_world_tile_mesh_textures"};
  Scene scene;
  TileMeshes meshes;
  // An allowance one map fits in and two do not: the rock's base colour map is 1,392 staged bytes
  // and its normal map, with its variance chain, 2,112.
  constexpr u64 k_allowance = 1500;
  meshes.create(options_of(tmp), room(k_allowance), scene.sink(), nullptr);
  std::string error;
  const u32 a = meshes.acquire(rock(0, 0), "", &error);
  REQUIRE_MESSAGE(a != TileMeshes::k_none, error);

  // Commit 1: the base colour map, and no more.
  meshes.begin_commit(false);
  CHECK(meshes.admit(a) == TileMeshes::Admit::deferred);
  meshes.end_commit();
  CHECK(scene.uploads == 1);
  const u64 first = scene.uploaded_bytes;
  CHECK(first > 0);
  CHECK(first <= k_allowance);
  CHECK(scene.meshes.empty());
  CHECK_FALSE(meshes.resident(a));
  // Commit 2: the normal map, larger than the whole allowance, alone — and so no mesh behind it.
  scene.uploaded_bytes = 0;
  meshes.begin_commit(false);
  CHECK(meshes.admit(a) == TileMeshes::Admit::deferred);
  meshes.end_commit();
  CHECK(scene.uploads == 2);
  const u64 second = scene.uploaded_bytes;
  CHECK(second > k_allowance);
  CHECK(scene.meshes.empty());
  MESSAGE("staged a commit under an allowance of " << k_allowance << ": " << first << ", then "
                                                   << second << " (one texture alone)");
  // Commit 3: the mesh, with its textures; the table's own references are back.
  scene.uploaded_bytes = 0;
  meshes.begin_commit(false);
  CHECK(meshes.admit(a) == TileMeshes::Admit::done);
  meshes.end_commit();
  CHECK(scene.uploaded_bytes == 0);
  CHECK(meshes.resident(a));
  CHECK(scene.textured == 1);
  CHECK(scene.plain == 0);
  REQUIRE(scene.textures.size() == 2);
  CHECK(scene.textures[0].refs == 1);
  CHECK(scene.textures[1].refs == 1);
  CHECK(meshes.stats().textures_uploaded == 2);
  CHECK(meshes.stats().texture_bytes == first + second);

  // A second rock of the same two maps: nothing to upload, so it arrives in one commit, and its
  // build read neither file (the scene said it held them).
  const u32 b = meshes.acquire(rock(1, 0), "", &error);
  REQUIRE_MESSAGE(b != TileMeshes::k_none, error);
  meshes.begin_commit(false);
  CHECK(meshes.admit(b) == TileMeshes::Admit::done);
  meshes.end_commit();
  CHECK(scene.uploads == 2);
  CHECK(scene.uploaded_bytes == 0);
  CHECK(scene.textured == 2);
  CHECK(meshes.stats().textures_shared == 2);
  CHECK(scene.textures[0].refs == 2);

  // The first goes (no linger): its references with it. Then the second, and the textures.
  meshes.release(a);
  meshes.begin_commit(false);
  meshes.end_commit();
  REQUIRE(scene.textures.size() == 2);
  CHECK(scene.textures[0].refs == 1);
  meshes.release(b);
  meshes.begin_commit(false);
  meshes.end_commit();
  CHECK(scene.textures.empty());
  CHECK(scene.meshes.empty());
  CHECK(scene.violations == 0);

  // A whole commit (a restart, a capture's fill) admits a mesh and its textures at once.
  const u32 c = meshes.acquire(rock(2, 0), "", &error);
  REQUIRE_MESSAGE(c != TileMeshes::k_none, error);
  meshes.begin_commit(true);
  CHECK(meshes.admit(c) == TileMeshes::Admit::done);
  meshes.end_commit();
  CHECK(scene.uploads == 4);
  CHECK(scene.textures.size() == 2);
  CHECK(scene.violations == 0);
}

TEST_CASE("tile mesh textures: a refused texture ends the wait and the mesh draws its constants") {
  test::TempDir tmp{"engine_world_tile_mesh_textures_refused"};
  Scene scene;
  scene.pairs = 1;
  TileMeshes meshes;
  meshes.create(options_of(tmp), room(0), scene.sink(), nullptr);
  std::string error;
  const u32 a = meshes.acquire(rock(0, 0), "", &error);
  REQUIRE_MESSAGE(a != TileMeshes::k_none, error);
  meshes.begin_commit(false);
  CHECK(meshes.admit(a) == TileMeshes::Admit::done);
  meshes.end_commit();
  CHECK(meshes.resident(a));
  CHECK(scene.plain == 1);
  CHECK(scene.textured == 0);
  CHECK(meshes.stats().texture_refusals == 1);
  // The map that did get in went back with the refusal: nothing is held for a mesh that draws none.
  CHECK(scene.textures.empty());
  CHECK(scene.violations == 0);
}

TEST_CASE("tile mesh textures: a mesh let go while it waits leaves no texture behind") {
  test::TempDir tmp{"engine_world_tile_mesh_textures_dropped"};
  Scene scene;
  TileMeshes meshes;
  meshes.create(options_of(tmp), room(1500), scene.sink(), nullptr);
  std::string error;
  const u32 a = meshes.acquire(rock(0, 0), "", &error);
  REQUIRE_MESSAGE(a != TileMeshes::k_none, error);
  meshes.begin_commit(false);
  CHECK(meshes.admit(a) == TileMeshes::Admit::deferred);
  meshes.end_commit();
  REQUIRE(scene.textures.size() == 1);  // held by the table for the mesh to come
  meshes.release(a);
  meshes.begin_commit(false);
  meshes.end_commit();
  CHECK(scene.textures.empty());
  CHECK(scene.meshes.empty());
  CHECK(scene.violations == 0);
}
