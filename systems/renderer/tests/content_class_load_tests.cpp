// Content classes decided when a scene is loaded (C64; renderer.md, "Content classes at load";
// docs/content-classes.md, "What decided at instantiation means"). CPU only, no device.
//
// Every fixture is neutral data of the test's own: a heightfield grid written as a container, a
// 32 x 32 stripe image, and a made-up game category `fixture_glow` (`none`, `dim`, `bright`). The
// classified mesh is a grid of another size than the neutral ones, so its vertices are counted
// apart from theirs and "not in the scene" is a count of bytes, not a picture.
#include <core/hash/hash.h>
#include <domain/geometry/cluster_file.h>
#include <domain/texture/material_textures.h>
#include <foundation/content_class/content_class.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/admission.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr const char* k_glow_categories =
    R"({"categories":[{"name":"fixture_glow","levels":["none","dim","bright"]}]})";
constexpr const char* k_policies =
    R"({"policies":[)"
    R"({"name":"glow-proxy","caps":[{"category":"fixture_glow","cap":"none","substitution":"proxy"}]},)"
    R"({"name":"glow-wide","caps":[{"category":"fixture_glow","cap":"bright","substitution":"omit"}]}]})";

content_class::NamedPolicy policy_named(const char* name) {
  content_class::ViewPolicies set;
  std::string error;
  REQUIRE_MESSAGE(set.read_json(content_class::Categories(), k_policies, error), error);
  for (u32 i = 0; i < set.size(); ++i)
    if (set.at(i).name == name) return set.at(i);
  FAIL("no such policy");
  return agent_default_view_policy();
}

// Section 54 for `fixture_glow` at `level`, its ladder `levels` (a categories file's JSON).
Vector<u8> glow_record(u8 level, const char* categories = k_glow_categories) {
  content_class::Categories table;
  std::string error;
  REQUIRE_MESSAGE(table.add_json(categories, error), error);
  content_class::ContentClasses classes;
  classes.levels[static_cast<u32>(table.find("fixture_glow"))] = level;
  Vector<u8> record;
  content_class::encode_classes(table, classes, nullptr, record);
  REQUIRE_FALSE(record.empty());
  return record;
}

void write_image(const std::string& path, u8 shade) {
  Vector<u8> pixels(32u * 32u * 4u);
  for (u32 i = 0; i < 32u * 32u; ++i) {
    pixels[i * 4 + 0] = static_cast<u8>((i / 4u) % 2u == 0 ? shade : 255u - shade);
    pixels[i * 4 + 1] = 128u;
    pixels[i * 4 + 2] = 64u;
    pixels[i * 4 + 3] = 255u;
  }
  Vector<u8> png;
  REQUIRE(image::encode_png(32, 32, 4, std::span<const u8>(pixels.data(), pixels.size()), png));
  REQUIRE(io::write_file(path, std::string_view(reinterpret_cast<const char*>(png.data()),
                                                png.size())) == io::Status::Ok);
}

struct Grid {
  std::string path;
  u32 clusters = 0;
  u32 vertices = 0;
};

// A grid of `side` vertices a side as a container at `name`, one material sampling `image` (a file
// written beside it) and carrying `record` as its section 54 (empty: unclassified).
Grid write_grid(const test::TempDir& tmp, const char* name, u32 side, const char* image,
                const Vector<u8>& record = {}) {
  SceneDesc grid;
  grid.meshes.push_back("");
  grid.heightfield_grid = side;
  grid.cache = false;
  SceneData made;
  std::string error;
  REQUIRE_MESSAGE(load_scene(grid, made, error), error);
  geometry::ClusterFileData file;
  file.mesh = std::move(made.lod);
  geometry::ClusterFileMaterial material;
  material.base_color_image = 0;
  file.materials.push_back(material);
  file.image_paths.push_back(image);
  file.cluster_material = Vector<u32>(file.mesh.mesh.clusters.size(), 0u);
  texture::fill_cluster_texture_records(file);
  file.content_classes = record;
  write_image(tmp.file(image), static_cast<u8>(side * 7u));
  Grid out;
  out.path = tmp.file(name);
  out.clusters = file.mesh.mesh.clusters.size();
  out.vertices = file.mesh.mesh.vertices.size();
  REQUIRE_MESSAGE(geometry::write_cluster_file(out.path, file, &error), error);
  return out;
}

SceneDesc scene_of(const test::TempDir& tmp, std::initializer_list<const Grid*> meshes) {
  SceneDesc desc;
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  for (const Grid* mesh : meshes) {
    SceneInstance instance{desc.meshes.size(), Transform3::identity()};
    instance.origin = WorldPos{30.0 * desc.meshes.size(), 0.0, 0.0};
    desc.instances.push_back(instance);
    desc.meshes.push_back(mesh->path);
  }
  return desc;
}

u32 box_vertices() {
  geometry::ClusterLodMesh box;
  REQUIRE(build_proxy_box(Vec3{0, 0, 0}, Vec3{1, 2, 3}, geometry::ClusterLodOptions{}, box));
  return box.mesh.vertices.size();
}

const SubstitutedCount* row(const SceneData& scene, const char* category) {
  for (const SubstitutedCount& r : scene.load.substituted)
    if (r.category == category) return &r;
  return nullptr;
}

u64 tables_hash(const SceneData& data) {
  auto bytes = [](const auto& v) { return hash_bytes(v.data(), v.size() * sizeof(*v.data())); };
  u64 h = bytes(data.lod.mesh.clusters);
  h = hash_combine(h, bytes(data.lod.mesh.vertices));
  h = hash_combine(h, bytes(data.lod.mesh.triangles));
  h = hash_combine(h, bytes(data.instances));
  for (const SourceMesh& source : data.sources)
    h = hash_combine(h, bytes(source.textures));
  return h;
}

}  // namespace

TEST_CASE("content classes at load: omit leaves a mesh's bytes out, proxy puts one box in") {
  const test::TempDir tmp("renderer_classes_three");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  const Grid b = write_grid(tmp, "b.clusters", 9, "b.png");
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(2));
  const u32 box = box_vertices();
  std::string error;

  SUBCASE("agent-default omits a game category: nothing of it is read or kept") {
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(scene_of(tmp, {&a, &b, &glow}), scene, error), error);
    CHECK(scene.instances.size() == 2);
    CHECK(scene.lod.mesh.vertices.size() == a.vertices + b.vertices + box);  // glow's are absent
    CHECK(scene.parts[2].cluster_count == 1);
    CHECK(scene.sources[2].data.images.empty());
    CHECK(scene.sources[2].container.empty());
    CHECK(scene.load.references == 2);  // its image was never a texture of the scene
    CHECK(scene.load.images_hashed == 2);
    CHECK(scene.decisions.size() == 2);
    REQUIRE(row(scene, "fixture_glow") != nullptr);
    CHECK(row(scene, "fixture_glow")->omitted == 1);
    CHECK(content_class::is_none(scene.held));
    CHECK(loaded_instance(scene, 2) == k_not_loaded);
    CHECK(loaded_instance(scene, 1) == 1);
  }
  SUBCASE("proxy: one box at its bounds, and nothing else of it") {
    SceneDesc desc = scene_of(tmp, {&a, &b, &glow});
    desc.view_policy = policy_named("glow-proxy");
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    REQUIRE(scene.instances.size() == 3);
    CHECK(scene.instances[2].mesh == 2);
    CHECK(scene.lod.mesh.vertices.size() == a.vertices + b.vertices + box);
    CHECK(scene.sources[2].data.images.empty());
    CHECK(scene.sources[2].data.materials.size() == 1);
    CHECK(scene.load.references == 2);
    REQUIRE(row(scene, "fixture_glow") != nullptr);
    CHECK(row(scene, "fixture_glow")->proxied == 1);
    CHECK(scene.decisions.size() == 2);
  }
  SUBCASE("a policy that covers it loads it whole, and the scene says it holds it") {
    SceneDesc desc = scene_of(tmp, {&a, &b, &glow});
    desc.view_policy = policy_named("glow-wide");
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    CHECK(scene.instances.size() == 3);
    CHECK(scene.lod.mesh.vertices.size() == a.vertices + b.vertices + glow.vertices);
    CHECK(scene.load.substituted.empty());
    CHECK(scene.decisions.size() == 2);
    CHECK(scene.held.levels[static_cast<u32>(scene.categories.find("fixture_glow"))] == 2);
  }
}

TEST_CASE("content classes at load: a raise in the scene file decides one instance of a mesh") {
  const test::TempDir tmp("renderer_classes_raise");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
  // Four instances: a plain one of `a`, one of `a` raised to bright, one of the dim container
  // (which teaches the scene the ladder), and one of `a` raising a category nobody defines.
  const std::string scene_json =
      R"({"meshes":[{"path":"a.clusters"},{"path":"glow.clusters"}],"instances":[)"
      R"({"mesh":0,"translation":[0,0,0],"rotation":[0,0,0,1]},)"
      R"({"mesh":0,"translation":[30,0,0],"rotation":[0,0,0,1],)"
      R"("content_classes":{"levels":{"fixture_glow":"bright"}}},)"
      R"({"mesh":1,"translation":[60,0,0],"rotation":[0,0,0,1]},)"
      R"({"mesh":0,"translation":[90,0,0],"rotation":[0,0,0,1],)"
      R"("content_classes":{"levels":{"fixture_hum":"loud"}}}]})";
  REQUIRE(io::write_file(tmp.file("scene.json"), scene_json) == io::Status::Ok);
  std::string error;
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(tmp.file("scene.json"), desc, error), error);
  REQUIRE(desc.raises.size() == 2);
  CHECK(desc.instances[1].content_classes == 1);
  desc.ddc = tmp.file("ddc");

  SceneData under_default;
  REQUIRE_MESSAGE(load_scene(desc, under_default, error), error);
  CHECK(under_default.instances.size() == 1);  // the plain one alone
  REQUIRE(row(under_default, "fixture_glow") != nullptr);
  CHECK(row(under_default, "fixture_glow")->omitted == 2);
  CHECK(under_default.load.unreadable_omitted == 1);
  CHECK(loaded_instance(under_default, 0) == 0);
  CHECK(loaded_instance(under_default, 1) == k_not_loaded);
  CHECK(read_instance_count(under_default) == 4);

  desc.view_policy = policy_named("glow-wide");
  SceneData wide;
  REQUIRE_MESSAGE(load_scene(desc, wide, error), error);
  CHECK(wide.instances.size() == 3);  // the unknown category still fails closed
  CHECK(wide.load.unreadable_omitted == 1);
  CHECK(wide.lod.mesh.vertices.size() == a.vertices + glow.vertices);  // `a` is read once
}

TEST_CASE("content classes at load: an image's sidecar added after the build decides its mesh") {
  const test::TempDir tmp("renderer_classes_image");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a_color.png");
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
  // The image's own stem, so its sidecar is the image's and not the container's beside it.
  std::string before;
  REQUIRE(io::read_file(a.path, before) == io::Status::Ok);
  std::string error;
  SceneData plain;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {&a}), plain, error), error);
  CHECK(plain.instances.size() == 1);
  CHECK(plain.load.image_sidecars_probed == 1);
  CHECK(plain.load.image_sidecars_found == 0);

  REQUIRE(io::write_file(tmp.file("a_color.provenance.json"),
                         R"({"content_classes":{"fixture_glow":"bright"}})") == io::Status::Ok);
  // Alone, nothing in the scene gives `fixture_glow` a ladder: the class cannot be read, and fails
  // closed.
  SceneData alone;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {&a}), alone, error), error);
  CHECK(alone.instances.empty());
  CHECK(alone.load.unreadable_omitted == 1);
  CHECK(alone.load.image_sidecars_found == 1);
  // Beside a container that brings the ladder, it reads as what it says.
  SceneData taught;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {&glow, &a}), taught, error), error);
  CHECK(taught.instances.empty());
  REQUIRE(row(taught, "fixture_glow") != nullptr);
  CHECK(row(taught, "fixture_glow")->omitted == 2);
  CHECK(taught.load.unreadable_omitted == 0);
  // The container was not rebuilt: its bytes are what the build wrote.
  std::string after;
  REQUIRE(io::read_file(a.path, after) == io::Status::Ok);
  CHECK(after == before);
}

TEST_CASE("content classes at load: what cannot be read is omitted and counted apart") {
  const test::TempDir tmp("renderer_classes_unreadable");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
  // A second container naming `fixture_glow` with another ladder than the first one met.
  const Grid other = write_grid(
      tmp, "other.clusters", 13, "other.png",
      glow_record(1, R"({"categories":[{"name":"fixture_glow","levels":["none","bright"]}]})"));
  std::string error;
  {
    SceneDesc desc = scene_of(tmp, {&glow, &other});
    desc.view_policy = policy_named("glow-wide");
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    CHECK(scene.instances.size() == 1);  // the first is covered; the contradiction is not
    CHECK(scene.load.unreadable_omitted == 1);
  }
  // A sidecar beside a container that is not the reader's.
  REQUIRE(io::write_file(tmp.file("a.provenance.json"), "{\"content_classes\": [1, 2]}") ==
          io::Status::Ok);
  SceneData scene;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {&a}), scene, error), error);
  CHECK(scene.instances.empty());
  CHECK(scene.load.unreadable_omitted == 1);
}

// One triangle as a glTF of the test's own, its buffer a `data:` URI (positions, then indices).
constexpr const char* k_triangle_gltf =
    R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)"
    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}],)"
    R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3",)"
    R"("min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],)"
    R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},)"
    R"({"buffer":0,"byteOffset":36,"byteLength":6}],)"
    R"("buffers":[{"byteLength":44,"uri":"data:application/octet-stream;base64,)"
    R"(AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAABAAIAAAA="}]})";

TEST_CASE("content classes at load: an unreadable sidecar beside a glTF omits it, unread") {
  const test::TempDir tmp("renderer_classes_gltf");
  REQUIRE(tmp.ok());
  REQUIRE(io::write_file(tmp.file("tri.gltf"), k_triangle_gltf) == io::Status::Ok);
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  Grid tri;
  tri.path = tmp.file("tri.gltf");
  std::string error;
  {
    // Neutral, it loads as any glTF does.
    SceneData plain;
    REQUIRE_MESSAGE(load_scene(scene_of(tmp, {&a, &tri}), plain, error), error);
    CHECK(plain.instances.size() == 2);
    CHECK(plain.decisions.size() == 1);
  }
  REQUIRE(io::write_file(tmp.file("tri.provenance.json"), R"({"content_classes":"general"})") ==
          io::Status::Ok);
  SceneData scene;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {&a, &tri}), scene, error), error);
  CHECK(scene.instances.size() == 1);
  CHECK(scene.load.unreadable_omitted == 1);
  CHECK(scene.sources[1].data.images.empty());
  CHECK(std::string(scene.sources[1].cache) == "none");  // never read: no cache lookup at all
}

TEST_CASE("content classes at load: a scene with nothing classified loads what it loaded") {
  const test::TempDir tmp("renderer_classes_none");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  const Grid b = write_grid(tmp, "b.clusters", 11, "b.png");
  std::string error;
  SceneData under_default;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {&a, &b}), under_default, error), error);
  SceneDesc wide_desc = scene_of(tmp, {&a, &b});
  wide_desc.view_policy = policy_named("glow-wide");
  SceneData wide;
  REQUIRE_MESSAGE(load_scene(wide_desc, wide, error), error);
  CHECK(tables_hash(under_default) == tables_hash(wide));
  for (const SceneData* scene : {&under_default, &wide}) {
    CHECK(scene->decisions.size() == 1);
    CHECK(scene->load.substituted.empty());
    CHECK(scene->load.unreadable_omitted == 0);
    CHECK(content_class::is_none(scene->held));
    CHECK(scene->loaded_of_read.empty());
    CHECK(scene->instances.size() == 2);
    CHECK(scene->load.image_sidecars_probed == 2);
  }
  CHECK(under_default.instantiated_under.name == content_class::k_agent_default_name);
  CHECK(wide.instantiated_under.name == "glow-wide");
}

// The raise is two bytes in what was `SceneInstance`'s padding: the struct without it, laid out the
// same way, is as large, so an instance that raises nothing pays nothing.
TEST_CASE("content classes at load: the raise costs an instance no bytes") {
  // Offsets taken from an object, not `offsetof`, which a type holding a `std::string` may not ask.
  const SceneInstance instance{};
  const auto at = [&](const void* member) {
    return static_cast<usize>(static_cast<const char*>(member) -
                              reinterpret_cast<const char*>(&instance));
  };
  const auto round_up = [](usize n, usize a) { return (n + a - 1) / a * a; };
  const usize slope_without =
      round_up(at(&instance.grounded) + sizeof(bool), alignof(scene_gen::GroundSlope));
  const usize size_without =
      round_up(slope_without + sizeof(scene_gen::GroundSlope), alignof(SceneInstance));
  CHECK(at(&instance.content_classes) > at(&instance.grounded));
  CHECK(at(&instance.content_classes) < at(&instance.slope));
  CHECK(sizeof(SceneInstance) == size_without);
}

TEST_CASE("content classes at load: the proxy box is a closed box at the bounds it is given") {
  geometry::ClusterLodMesh box;
  REQUIRE(build_proxy_box(Vec3{-1, 0, 2}, Vec3{1, 3, 2}, geometry::ClusterLodOptions{}, box));
  CHECK(box.mesh.clusters.size() == 1);
  CHECK(box.leaf_triangle_count == 12);
  Vec3 lo;
  Vec3 hi;
  for (u32 i = 0; i < box.mesh.vertices.size(); ++i) {
    const Vec3 v = box.mesh.vertices[i];
    lo = i == 0 ? v : Vec3{std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)};
    hi = i == 0 ? v : Vec3{std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z)};
  }
  CHECK(lo.x == doctest::Approx(-1.0f));
  CHECK(hi.y == doctest::Approx(3.0f));
  CHECK(hi.z - lo.z == doctest::Approx(0.001f));  // a flat extent widened to a millimetre
  const assets::Material material = proxy_material();
  CHECK(material.base_color_image == -1);
  CHECK(material.base_color.x == k_proxy_albedo);
}
