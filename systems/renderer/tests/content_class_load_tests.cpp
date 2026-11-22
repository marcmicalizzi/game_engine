// Content classes decided when a scene is loaded (C64; renderer.md, "Content classes at load";
// docs/content-classes.md, "What decided at instantiation means"). CPU only, no device.
//
// The capture cases open a device (the merge's GPU run, or a run by their names) and skip without
// one; every other case is CPU only.
//
// Every fixture is neutral data of the test's own: a heightfield grid written as a container, a
// 32 x 32 stripe image, and a made-up game category `fixture_glow` (`none`, `dim`, `bright`). The
// classified mesh is a grid of another size than the neutral ones, so its vertices are counted
// apart from theirs and "not in the scene" is a count of bytes, not a picture.
#include "test_device.h"

#include <core/base/macros.h>
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <domain/assets/gltf.h>
#include <domain/assets/mesh_generator.h>
#include <domain/content_build/content_build.h>
#include <domain/content_build/generator_hosts.h>
#include <domain/geometry/cluster_file.h>
#include <domain/gfx/device.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/texture/material_textures.h>
#include <domain/texture/texture_build.h>
#include <foundation/content_class/content_class.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/admission.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/reference.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain.h>
#include <systems/renderer/view_set.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

#if ENGINE_PLATFORM_WINDOWS
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr const char* k_glow_categories =
    R"({"categories":[{"name":"fixture_glow","levels":["none","dim","bright"]}]})";
constexpr const char* k_policies =
    R"({"policies":[)"
    R"({"name":"glow-proxy","caps":[{"category":"fixture_glow","cap":"none","substitution":"proxy"}]},)"
    R"({"name":"glow-wide","caps":[{"category":"fixture_glow","cap":"bright","substitution":"omit"}]},)"
    // Every engine category at its top and the made-up one at `none`: what decides a class that
    // could not be read by the categories the scene knows (C64).
    R"({"name":"tops","caps":[{"category":"nudity","cap":"full","substitution":"omit"},)"
    R"({"category":"sexual","cap":"explicit","substitution":"omit"},)"
    R"({"category":"violence","cap":"graphic","substitution":"omit"},)"
    R"({"category":"fixture_glow","cap":"none","substitution":"omit"}]}]})";

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

std::filesystem::path fs_path(const std::string& p) {
  return std::filesystem::path(
      std::u8string_view(reinterpret_cast<const char8_t*>(p.data()), p.size()));
}

// A symbolic link to `target`, where the process may make one (Windows needs developer mode or
// elevation for it).
bool make_file_symlink(const std::string& target, const std::string& link) {
  std::error_code ec;
  std::filesystem::create_symlink(fs_path(target), fs_path(link), ec);
  return !ec;
}

#if ENGINE_PLATFORM_WINDOWS
std::wstring wide(const std::string& s) {
  std::wstring w(static_cast<usize>(MultiByteToWideChar(CP_UTF8, 0, s.data(),
                                                        static_cast<int>(s.size()), nullptr, 0)),
                 L'\0');
  MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(),
                      static_cast<int>(w.size()));
  for (wchar_t& c : w)
    if (c == L'/') c = L'\\';
  return w;
}

// A junction, which any user may make: an empty directory given a mount-point reparse point. Its
// target need not exist (the content build's tests and foundation/io's make them the same way).
bool make_junction(const std::string& target, const std::string& link) {
  if (!CreateDirectoryW(wide(link).c_str(), nullptr)) return false;
  const HANDLE h = CreateFileW(wide(link).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                               FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  const std::wstring substitute = L"\\??\\" + wide(target);
  const std::wstring print = wide(target);
  const usize names = (substitute.size() + 1 + print.size() + 1) * sizeof(wchar_t);
  std::string buf(8 + 8 + names, '\0');
  auto put16 = [&](usize at, usize v) {
    buf[at] = static_cast<char>(v & 0xFF);
    buf[at + 1] = static_cast<char>((v >> 8) & 0xFF);
  };
  const u32 tag = 0xA0000003u;  // IO_REPARSE_TAG_MOUNT_POINT
  std::memcpy(buf.data(), &tag, 4);
  put16(4, 8 + names);                                   // ReparseDataLength
  put16(8, 0);                                           // SubstituteNameOffset
  put16(10, substitute.size() * sizeof(wchar_t));        // SubstituteNameLength
  put16(12, (substitute.size() + 1) * sizeof(wchar_t));  // PrintNameOffset
  put16(14, print.size() * sizeof(wchar_t));             // PrintNameLength
  std::memcpy(buf.data() + 16, substitute.c_str(), (substitute.size() + 1) * sizeof(wchar_t));
  std::memcpy(buf.data() + 16 + (substitute.size() + 1) * sizeof(wchar_t), print.c_str(),
              (print.size() + 1) * sizeof(wchar_t));
  DWORD returned = 0;
  const BOOL ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(),
                                  static_cast<DWORD>(buf.size()), nullptr, 0, &returned, nullptr);
  CloseHandle(h);
  return ok != FALSE;
}
#endif

// The forms of a sidecar that is there and leads nowhere which this system lets a test make.
struct DanglingForm {
  const char* name;
  bool (*make)(const std::string& target, const std::string& link);
};
const DanglingForm k_dangling_forms[] = {
    {"a symbolic link to a file that is not there", make_file_symlink},
#if ENGINE_PLATFORM_WINDOWS
    {"a junction to a directory that is not there", make_junction},
#endif
};

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

// A sidecar written after the derived-data root was filled (texture.md, "Content classes"): the
// load reads it before it takes either key, so a glTF's mesh and an image named by path are decided
// by what it declares, each under a cache entry of its own keyed as the content build keys it, and
// never served the entry the root holds from before, which records no class. A sidecar that is
// there and leads nowhere is one that cannot be read, never `none`, as the content build reads it.
TEST_CASE(
    "content classes at load: a sidecar that comes after the cache decides, not the old entry") {
  std::string error;
  {
    const test::TempDir tmp("renderer_classes_later");
    REQUIRE(tmp.ok());
    REQUIRE(io::write_file(tmp.file("tri.gltf"), k_triangle_gltf) == io::Status::Ok);
    const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
    const Grid a = write_grid(tmp, "a.clusters", 9, "a_color.png");
    Grid tri;
    tri.path = tmp.file("tri.gltf");
    // `glow` teaches the scene the ladder; `glow-wide` admits all of it, so every asset is read.
    SceneDesc desc = scene_of(tmp, {&glow, &a, &tri});
    desc.view_policy = policy_named("glow-wide");
    const auto load = [&](SceneData& scene) {
      REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    };
    SceneData cold;
    load(cold);
    CHECK(cold.instances.size() == 3);
    CHECK(cold.load.built == 2);
    SceneData warm;
    load(warm);
    CHECK(warm.load.trusted == 2);
    CHECK(warm.load.built == 0);
    CHECK(std::string(warm.sources[2].cache) == "hit");
    REQUIRE(warm.sources[1].textures.size() == 1);
    const u64 plain_key = warm.sources[1].textures[0].key;
    CHECK(io::exists(texture::texture_cache_path(desc.ddc, plain_key)));

    // The image's sidecar declares its class on a material, the glTF's on the whole asset.
    REQUIRE(io::write_file(
                tmp.file("a_color.provenance.json"),
                R"({"content_classes":{"materials":{"m0":{"levels":{"fixture_glow":"dim"}}}}})") ==
            io::Status::Ok);
    REQUIRE(io::write_file(tmp.file("tri.provenance.json"),
                           R"({"content_classes":{"fixture_glow":"dim"}})") == io::Status::Ok);
    SceneData declared;
    load(declared);
    CHECK(declared.instances.size() == 3);
    CHECK(declared.load.unreadable_omitted == 0);
    CHECK(declared.held.levels[static_cast<u32>(declared.categories.find("fixture_glow"))] == 1);
    CHECK(std::string(declared.sources[2].cache) == "miss");  // not the classless entry
    REQUIRE(declared.sources[1].textures.size() == 1);
    CHECK(declared.sources[1].textures[0].key != plain_key);
    CHECK(declared.load.built == 1);
    // What the load built is what it asks for the next time: its keys are the content build's, so
    // the texture its records name is the one the step wrote.
    SceneData again;
    load(again);
    CHECK(std::string(again.sources[2].cache) == "hit");
    CHECK(again.sources[1].textures[0].key == declared.sources[1].textures[0].key);
    CHECK(io::exists(texture::texture_cache_path(desc.ddc, again.sources[1].textures[0].key)));
    CHECK(again.load.trusted == 2);
    CHECK(again.load.built == 0);
    // A level that changes is an entry of its own again, never the classified one before it.
    REQUIRE(
        io::write_file(
            tmp.file("a_color.provenance.json"),
            R"({"content_classes":{"materials":{"m0":{"levels":{"fixture_glow":"bright"}}}}})") ==
        io::Status::Ok);
    REQUIRE(io::write_file(tmp.file("tri.provenance.json"),
                           R"({"content_classes":{"fixture_glow":"bright"}})") == io::Status::Ok);
    SceneData brighter;
    load(brighter);
    CHECK(brighter.held.levels[static_cast<u32>(brighter.categories.find("fixture_glow"))] == 2);
    CHECK(std::string(brighter.sources[2].cache) == "miss");
    CHECK(brighter.sources[2].container != again.sources[2].container);
    CHECK(brighter.sources[1].textures[0].key != again.sources[1].textures[0].key);
    CHECK(brighter.sources[1].textures[0].key != plain_key);
    // And a sidecar taken away returns both to the plain entries the cold load built.
    std::error_code ec;
    REQUIRE(std::filesystem::remove(fs_path(tmp.file("a_color.provenance.json")), ec));
    REQUIRE(std::filesystem::remove(fs_path(tmp.file("tri.provenance.json")), ec));
    SceneData removed;
    load(removed);
    CHECK(removed.sources[1].textures[0].key == plain_key);
    CHECK(std::string(removed.sources[2].cache) == "hit");
    CHECK(removed.sources[2].container == warm.sources[2].container);
    CHECK(removed.load.built == 0);
  }
  u32 made = 0;
  for (const DanglingForm& form : k_dangling_forms) {
    const std::string form_name = form.name;
    CAPTURE(form_name);
    const test::TempDir tmp("renderer_classes_nowhere");
    REQUIRE(tmp.ok());
    REQUIRE(io::write_file(tmp.file("tri.gltf"), k_triangle_gltf) == io::Status::Ok);
    const Grid a = write_grid(tmp, "a.clusters", 9, "a_color.png");
    Grid tri;
    tri.path = tmp.file("tri.gltf");
    const SceneDesc desc = scene_of(tmp, {&a, &tri});
    SceneData plain;
    REQUIRE_MESSAGE(load_scene(desc, plain, error), error);
    CHECK(plain.instances.size() == 2);
    if (!form.make(tmp.file("gone"), tmp.file("tri.provenance.json"))) {
      MESSAGE("skipped: this process may not make " << form.name);
      continue;
    }
    REQUIRE(form.make(tmp.file("gone_too"), tmp.file("a_color.provenance.json")));
    ++made;
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    CHECK(scene.instances.empty());
    CHECK(scene.load.unreadable_omitted == 2);
    CHECK(std::string(scene.sources[1].cache) == "none");  // the glTF is never read
  }
  // At least the junction on Windows and the symbolic link elsewhere.
  CHECK(made >= 1u);
}

namespace {

// A ground of the scene file's own (`terrain`, 17 x 17 over 8 m), whose one layer samples
// `ground.png` beside the file; `glow.clusters` first when `with_glow` (it teaches the ladder).
std::string ground_scene(bool with_glow) {
  std::string json = R"({"format":"engine.scene.v1",)";
  if (with_glow) {
    json += R"("meshes":[{"path":"glow.clusters"}],)"
            R"("instances":[{"mesh":0,"translation":[0,0,0],"rotation":[0,0,0,1]}],)";
  }
  json += R"("terrain":{"size":17,"extent":8,)"
          R"("materials":{"layers":[{"base_color":"ground.png","projection":"Map"}]}}})";
  return json;
}

// The key the ground's cache entry has with no class at all: `load_terrain`'s own terms.
u64 classless_ground_key(const SceneDesc& desc) {
  geometry::ClusterLodOptions lod = desc.lod;
  lod.up = Vec3{0.0f, 1.0f, 0.0f};
  return geometry::cluster_cache_key(terrain_hash(desc.terrain), lod, false,
                                     geometry::ClusterPagesOptions{}.page_bytes);
}

u32 files_in(const std::string& dir) {
  std::error_code ec;
  u32 n = 0;
  for (std::filesystem::directory_iterator it(fs_path(dir), ec), end; !ec && it != end;
       it.increment(ec))
    ++n;
  return n;
}

}  // namespace

// **Nothing classified, nothing changed** (C64): the same tables under two policies, and every key
// the load takes, the mesh's, each image's and the ground's, the plain key function's value
// computed here without any class, with its entry under that name.
TEST_CASE("content classes at load: a scene with nothing classified loads what it loaded") {
  const test::TempDir tmp("renderer_classes_none");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  const Grid b = write_grid(tmp, "b.clusters", 11, "b.png");
  REQUIRE(io::write_file(tmp.file("tri.gltf"), k_triangle_gltf) == io::Status::Ok);
  Grid tri;
  tri.path = tmp.file("tri.gltf");
  std::string error;
  SceneData under_default;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {&a, &b, &tri}), under_default, error), error);
  SceneDesc wide_desc = scene_of(tmp, {&a, &b, &tri});
  wide_desc.view_policy = policy_named("glow-wide");
  SceneData wide;
  REQUIRE_MESSAGE(load_scene(wide_desc, wide, error), error);
  CHECK(tables_hash(under_default) == tables_hash(wide));
  u64 tri_hash = 0;
  REQUIRE_MESSAGE(assets::source_mesh_hash(tri.path, tri_hash, &error), error);
  const std::string tri_entry = geometry::cluster_cache_path(
      tmp.file("ddc"),
      geometry::cluster_cache_key(tri_hash, geometry::imported_lod_options(SceneDesc{}.lod), true,
                                  geometry::ClusterPagesOptions{}.page_bytes));
  for (const SceneData* scene : {&under_default, &wide}) {
    CHECK(scene->decisions.size() == 1);
    CHECK(scene->load.substituted.empty());
    CHECK(scene->load.unreadable_omitted == 0);
    CHECK(content_class::is_none(scene->held));
    CHECK(scene->loaded_of_read.empty());
    CHECK(scene->instances.size() == 3);
    CHECK(scene->load.image_sidecars_probed == 2);
    // Each image's: the texture key over the file's bytes and the record's options, and no class.
    for (u32 m = 0; m < 2; ++m) {
      CAPTURE(m);
      REQUIRE(scene->sources[m].textures.size() == 1);
      const geometry::ClusterFileTexture& record = scene->sources[m].textures[0];
      std::string bytes;
      REQUIRE(io::read_file(tmp.file(m == 0 ? "a.png" : "b.png"), bytes) == io::Status::Ok);
      texture::TextureBuildOptions options;
      REQUIRE(texture::unpack_texture_options(record.options, options));
      const u64 plain = texture::texture_cache_key(hash_bytes(bytes.data(), bytes.size()), options);
      CHECK(record.key == plain);
      CHECK(io::exists(texture::texture_cache_path(tmp.file("ddc"), plain)));
    }
    // The glTF's: the cluster key over its source hash, as the content build takes it.
    CHECK(scene->sources[2].container == tri_entry);
  }
  CHECK(io::exists(tri_entry));
  CHECK(under_default.instantiated_under.name == content_class::k_agent_default_name);
  CHECK(wide.instantiated_under.name == "glow-wide");
  // The ground's: `load_terrain`'s key with no class term.
  write_image(tmp.file("ground.png"), 40);
  REQUIRE(io::write_file(tmp.file("scene.json"), ground_scene(false)) == io::Status::Ok);
  SceneDesc ground_desc;
  REQUIRE_MESSAGE(read_scene_file(tmp.file("scene.json"), ground_desc, error), error);
  ground_desc.ddc = tmp.file("ddc");
  SceneData ground;
  REQUIRE_MESSAGE(load_scene(ground_desc, ground, error), error);
  REQUIRE(ground.terrain_mesh < ground.sources.size());
  const std::string ground_entry =
      geometry::cluster_cache_path(ground_desc.ddc, classless_ground_key(ground_desc));
  CHECK(ground.sources[ground.terrain_mesh].container == ground_entry);
  CHECK(io::exists(ground_entry));
}

// **The ground goes through the door** (C64): its class is the join of what the sidecars beside
// its heightfield source and every layer's images declare, read before any of them is; a ground
// the policy refuses is left out whole (never a box), and counted apart from the instances.
TEST_CASE(
    "content classes at load: a ground whose layer image is above the cap is left out whole") {
  const test::TempDir tmp("renderer_classes_ground");
  REQUIRE(tmp.ok());
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
  write_image(tmp.file("ground.png"), 40);
  REQUIRE(io::write_file(tmp.file("scene.json"), ground_scene(true)) == io::Status::Ok);
  std::string error;
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(tmp.file("scene.json"), desc, error), error);
  desc.ddc = tmp.file("ddc");
  const u64 classless = classless_ground_key(desc);
  REQUIRE(io::write_file(tmp.file("ground.provenance.json"),
                         R"({"content_classes":{"fixture_glow":"bright"}})") == io::Status::Ok);
  std::string image;
  REQUIRE(io::read_file(tmp.file("ground.png"), image) == io::Status::Ok);
  // The image is not one any more: a load that read it would fail.
  REQUIRE(io::write_file(tmp.file("ground.png"), "not an image") == io::Status::Ok);
  for (const std::string name : {"agent-default", "glow-proxy"}) {
    CAPTURE(name);
    SceneDesc under = desc;
    if (name != "agent-default") under.view_policy = policy_named(name.c_str());
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(under, scene, error), error);
    CHECK(scene.load.grounds_omitted == 1);
    CHECK(scene.terrain_mesh == ~0u);
    CHECK_FALSE(scene.terrain.enabled);
    // Not a box: under the proxy policy the dim container is one, and the ground nothing.
    CHECK(scene.instances.size() == (name == "glow-proxy" ? 1u : 0u));
    CHECK_FALSE(io::exists(geometry::cluster_cache_path(desc.ddc, classless)));
    CHECK(files_in(io::join_path(desc.ddc, "clusters")) == 0u);
  }
  // Under a policy that covers it, the ground is drawn, from an entry of its own carrying its
  // class.
  REQUIRE(io::write_file(tmp.file("ground.png"), image) == io::Status::Ok);
  desc.view_policy = policy_named("glow-wide");
  SceneData wide;
  REQUIRE_MESSAGE(load_scene(desc, wide, error), error);
  CHECK(wide.load.grounds_omitted == 0);
  CHECK(wide.instances.size() == 2);
  REQUIRE(wide.terrain_mesh < wide.sources.size());
  const SourceMesh& ground = wide.sources[wide.terrain_mesh];
  REQUIRE_FALSE(ground.container.empty());
  CHECK(ground.container != geometry::cluster_cache_path(desc.ddc, classless));
  geometry::ClusterFileData entry;
  REQUIRE_MESSAGE(geometry::read_cluster_file(ground.container, entry, &error), error);
  CHECK_FALSE(entry.content_classes.empty());
  CHECK(wide.held.levels[static_cast<u32>(wide.categories.find("fixture_glow"))] == 2);
}

// A ground with nothing beside its inputs keeps the key it had (the terrain's key gains a term only
// when there is a class); a sidecar beside a layer's image that leads nowhere refuses it, as it
// refuses a mesh.
TEST_CASE("content classes at load: a ground keeps its key with no sidecar, and fails closed") {
  std::string error;
  u32 made = 0;
  for (const DanglingForm& form : k_dangling_forms) {
    const std::string form_name = form.name;
    CAPTURE(form_name);
    const test::TempDir tmp("renderer_classes_ground_key");
    REQUIRE(tmp.ok());
    write_image(tmp.file("ground.png"), 40);
    REQUIRE(io::write_file(tmp.file("scene.json"), ground_scene(false)) == io::Status::Ok);
    SceneDesc desc;
    REQUIRE_MESSAGE(read_scene_file(tmp.file("scene.json"), desc, error), error);
    desc.ddc = tmp.file("ddc");
    SceneData plain;
    REQUIRE_MESSAGE(load_scene(desc, plain, error), error);
    CHECK(plain.instances.size() == 1);
    REQUIRE(plain.terrain_mesh < plain.sources.size());
    const std::string entry = geometry::cluster_cache_path(desc.ddc, classless_ground_key(desc));
    CHECK(plain.sources[plain.terrain_mesh].container == entry);
    CHECK(io::exists(entry));
    if (!form.make(tmp.file("gone"), tmp.file("ground.provenance.json"))) {
      MESSAGE("skipped: this process may not make " << form.name);
      continue;
    }
    ++made;
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    CHECK(scene.instances.empty());
    CHECK(scene.load.grounds_omitted == 1);
  }
  CHECK(made >= 1u);
}

// **An unreadable class is decided with what the scene knows at the decision** (C64): a glTF whose
// sidecar names a category nobody defines is unreadable, and the class it decides on is made at the
// decision, when the scene knows the category a container in it teaches too, before or after the
// glTF. So the order of the meshes does not change the outcome, under a policy that caps every
// engine category at its top and the game's at `none`.
TEST_CASE("content classes at load: an unreadable class decides the same in either mesh order") {
  const test::TempDir tmp("renderer_classes_order");
  REQUIRE(tmp.ok());
  REQUIRE(io::write_file(tmp.file("tri.gltf"), k_triangle_gltf) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("tri.provenance.json"),
                         R"({"content_classes":{"fixture_glow":"bright","fixture_nobody":"x"}})") ==
          io::Status::Ok);
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
  Grid tri;
  tri.path = tmp.file("tri.gltf");
  std::string error;
  for (const bool tri_first : {true, false}) {
    CAPTURE(tri_first);
    SceneDesc desc = tri_first ? scene_of(tmp, {&tri, &glow}) : scene_of(tmp, {&glow, &tri});
    desc.view_policy = policy_named("tops");
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    CHECK(loaded_instance(scene, tri_first ? 0u : 1u) == k_not_loaded);
    CHECK(scene.instances.empty());
    CHECK(scene.load.unreadable_omitted == 1);
  }
}

// **The scene file is read strictly** (C64): a key given twice anywhere refuses the file, naming
// the key, so a merge's appended `content_classes` or level cannot lose a raise.
TEST_CASE("content classes at load: a scene file that gives a key twice is refused, naming it") {
  const test::TempDir tmp("renderer_classes_twice");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  const auto refused = [&](const std::string& raise, const char* key) {
    const std::string scene_json = R"({"meshes":[{"path":"a.clusters"}],"instances":[)"
                                   R"({"mesh":0,"translation":[0,0,0],"rotation":[0,0,0,1],)" +
                                   raise + "}]}";
    REQUIRE(io::write_file(tmp.file("scene.json"), scene_json) == io::Status::Ok);
    SceneDesc desc;
    std::string error;
    CHECK_FALSE(read_scene_file(tmp.file("scene.json"), desc, error));
    CHECK_MESSAGE(error.find(key) != std::string::npos, error);
  };
  refused(
      R"("content_classes":{"levels":{"fixture_glow":"bright"}},"content_classes":{"levels":{}})",
      "content_classes");
  refused(R"("content_classes":{"levels":{"fixture_glow":"bright","fixture_glow":"none"}})",
          "fixture_glow");
}

// **Substitution is the placer's, not the slot's** (C64): an instance refused by its own raise is
// left out (or gets a box of its own) without touching the slot a prop, a tile or another instance
// still places; a mesh whose own class is refused is the box for every placer.
TEST_CASE("content classes at load: a raise refuses its instance, not the mesh others place") {
  const test::TempDir tmp("renderer_classes_placer");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
  const std::string scene_json =
      R"({"meshes":[{"path":"a.clusters"},{"path":"glow.clusters"}],"instances":[)"
      R"({"mesh":0,"translation":[0,0,0],"rotation":[0,0,0,1],)"
      R"("content_classes":{"levels":{"fixture_glow":"bright"}}},)"
      R"({"mesh":1,"translation":[30,0,0],"rotation":[0,0,0,1]}]})";
  REQUIRE(io::write_file(tmp.file("scene.json"), scene_json) == io::Status::Ok);
  std::string error;
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(tmp.file("scene.json"), desc, error), error);
  desc.ddc = tmp.file("ddc");
  // Both meshes are a prop's too: something after the load places each, unraised.
  desc.props.push_back(SceneProp{"post", 0});
  desc.props.push_back(SceneProp{"lamp", 1});
  const u32 box = box_vertices();
  {
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    CHECK(scene.instances.empty());
    CHECK(scene.parts[0].cluster_count == a.clusters);  // the post is still the mesh
    CHECK(scene.parts[1].cluster_count == 1);           // the lamp's own class: a box for all
    CHECK(scene.lod.mesh.vertices.size() == a.vertices + box);
  }
  desc.view_policy = policy_named("glow-proxy");
  SceneData scene;
  REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
  REQUIRE(scene.instances.size() == 2);
  CHECK(scene.parts[0].cluster_count == a.clusters);
  CHECK(scene.instances[0].mesh == 2);  // a box of its own, after the scene's meshes
  CHECK(scene.instances[1].mesh == 1);  // the lamp's slot is its box
}

// A substitution keeps a streamed scene on the streaming path (C64): the box has a page table of
// its own, so the scene's merged table is still built.
TEST_CASE("content classes at load: a streamed scene with a box in it still streams") {
  const test::TempDir tmp("renderer_classes_streamed");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(2));
  std::string error;
  for (const bool proxy : {false, true}) {
    CAPTURE(proxy);
    SceneDesc desc = scene_of(tmp, {&a, &glow});
    desc.stream = true;
    if (proxy) desc.view_policy = policy_named("glow-proxy");
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    CHECK(scene.instances.size() == (proxy ? 2u : 1u));
    CHECK_FALSE(scene.pages.pages.empty());
  }
}

// **A classes section the file cannot hold allocates nothing** (C64): the probe sizes its read
// from the section table, and `ClusterFileReader::open` refuses a table whose section runs past
// the file before anything is sized from it; the whole read refuses it the same way, so the load
// fails with its own sentence rather than reading a count of 4 GiB.
TEST_CASE("content classes at load: a classes section the file cannot hold allocates nothing") {
  const test::TempDir tmp("renderer_classes_huge");
  REQUIRE(tmp.ok());
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(2));
  std::string bytes;
  REQUIRE(io::read_file(glow.path, bytes) == io::Status::Ok);
  geometry::ClusterFileHeader header;
  REQUIRE(bytes.size() >= sizeof(header));
  std::memcpy(&header, bytes.data(), sizeof(header));
  bool patched = false;
  for (u32 s = 0; s < header.section_count; ++s) {
    const usize at = sizeof(header) + usize{s} * sizeof(geometry::ClusterFileSection);
    REQUIRE(at + sizeof(geometry::ClusterFileSection) <= bytes.size());
    geometry::ClusterFileSection section;
    std::memcpy(&section, bytes.data() + at, sizeof(section));
    if (section.kind != static_cast<u32>(geometry::ClusterSection::ContentClasses)) continue;
    section.element_count = 0xFFFFFFF0ull;  // a count the file's bytes cannot hold
    std::memcpy(bytes.data() + at, &section, sizeof(section));
    patched = true;
  }
  REQUIRE(patched);
  REQUIRE(io::write_file(glow.path, bytes) == io::Status::Ok);
  content_class::Categories categories;
  ContainerProbe probe;
  const ReadClass read = read_container_class(glow.path, categories, &probe);
  CHECK_FALSE(probe.opened);
  CHECK(probe.section.empty());
  CHECK(content_class::is_none(read.classes));  // and the whole read decides it, below
  std::string error;
  SceneData scene;
  CHECK_FALSE(load_scene(scene_of(tmp, {&glow}), scene, error));
  CHECK_MESSAGE(error.find("section") != std::string::npos, error);
}

// `substituted` counts instances (C64): a classified mesh no instance names, placed by a prop, is
// decided (its class in the table, its slot a box) and counted nowhere.
TEST_CASE("content classes at load: a mesh no instance names is decided, not counted") {
  const test::TempDir tmp("renderer_classes_unnamed");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
  SceneDesc desc = scene_of(tmp, {&a});
  desc.meshes.push_back(glow.path);
  desc.props.push_back(SceneProp{"lamp", 1});
  std::string error;
  SceneData scene;
  REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
  CHECK(scene.instances.size() == 1);
  CHECK(scene.parts[1].cluster_count == 1);  // the lamp's slot is its box
  CHECK(scene.decisions.size() == 2);
  CHECK(row(scene, "fixture_glow") == nullptr);
  CHECK(scene.load.unreadable_omitted == 0);
}

namespace {

// A generator of the test's own whose output grows over a host its recipe names (`{"host": ...}`),
// as a cover or a vine does: its key covers the host through the hosts' seam, and it builds
// nothing, so a load that reached its build would fail.
bool fixture_host_hash(std::span<const u8> bytes, u64& out, std::string&) {
  out = hash_bytes(bytes.data(), bytes.size());
  return true;
}
bool fixture_host_hash_with(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                            u64& out, std::string& error) {
  JsonValue recipe;
  const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
  const JsonValue* host = parse_json(text, recipe).ok ? recipe.find("host") : nullptr;
  if (host == nullptr || !host->is_string()) {
    error = "a fixture recipe names its host";
    return false;
  }
  u64 identity = 0;
  if (inputs.hosts == nullptr ||
      !inputs.hosts->identify(inputs.hosts->context, host->as_string(), identity, error))
    return false;
  out = hash_combine(hash_bytes(bytes.data(), bytes.size()), identity);
  return true;
}
bool fixture_host_build(std::span<const u8>, const assets::MeshGeneratorInputs&,
                        const geometry::ClusterLodOptions&, u32, geometry::ClusterFileData&,
                        std::string& error) {
  error = "the fixture generator builds nothing";
  return false;
}
constexpr assets::MeshGeneratorDesc k_fixture_host{.name = "fixturehost",
                                                   .source_hash = fixture_host_hash,
                                                   .build = fixture_host_build,
                                                   .source_hash_with = fixture_host_hash_with};

}  // namespace

// **A generated mesh carries its inputs' classes** (C64): a recipe that grows over a host is
// decided on the join of its own class and its host's, read from the host's sidecar before the
// host is read, so a host the policy refuses refuses the output and the host's file is never read.
TEST_CASE("content classes at load: a generated mesh over a refused host is left out, unread") {
  REQUIRE(assets::MeshGeneratorRegistry::global().add(k_fixture_host));
  const test::TempDir tmp("renderer_classes_host");
  REQUIRE(tmp.ok());
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
  REQUIRE(io::write_file(tmp.file("moss.fixturehost.json"), R"({"host":"statue.gltf"})") ==
          io::Status::Ok);
  // The host is not a glTF at all: reading it fails the load.
  REQUIRE(io::write_file(tmp.file("statue.gltf"), "not a glTF") == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("statue.provenance.json"),
                         R"({"content_classes":{"fixture_glow":"bright"}})") == io::Status::Ok);
  Grid moss;
  moss.path = tmp.file("moss.fixturehost.json");
  std::string error;
  SceneData scene;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {&glow, &moss}), scene, error), error);
  CHECK(scene.instances.empty());
  CHECK(loaded_instance(scene, 1) == k_not_loaded);
  CHECK(std::string(scene.sources[1].cache) == "none");  // never generated
  REQUIRE(row(scene, "fixture_glow") != nullptr);
  CHECK(row(scene, "fixture_glow")->omitted == 2);
}

// **The bytes that are loaded are the bytes that are decided** (C64): the probe of a container's
// classes section is a saving, so a container replaced or finished between the probe and the
// load's whole read is decided again by what the read found.
TEST_CASE("content classes at load: a container is decided by the bytes the load read") {
  const test::TempDir tmp("renderer_classes_swapped");
  REQUIRE(tmp.ok());
  const Grid plain = write_grid(tmp, "x.clusters", 9, "x.png");
  std::string error;
  content_class::Categories categories;
  ContainerProbe probe;
  const ReadClass probed = read_container_class(plain.path, categories, &probe);
  CHECK(probe.opened);
  CHECK(content_class::is_none(probed.classes));
  // Swapped between the probe and the read: the same name, classified now.
  (void)write_grid(tmp, "x.clusters", 9, "x.png", glow_record(2));
  geometry::ClusterFileData loaded;
  REQUIRE_MESSAGE(geometry::read_cluster_file(plain.path, loaded, &error), error);
  const ReadClass decided = container_class_as_loaded(
      probe, probed,
      std::span<const u8>(loaded.content_classes.data(), loaded.content_classes.size()),
      categories);
  REQUIRE(categories.find("fixture_glow") >= 0);
  CHECK(decided.classes.levels[static_cast<u32>(categories.find("fixture_glow"))] == 2);
  // One the probe could not open, whole when the load reads it.
  ContainerProbe missing;
  const ReadClass absent = read_container_class(tmp.file("later.clusters"), categories, &missing);
  CHECK_FALSE(missing.opened);
  const Grid later = write_grid(tmp, "later.clusters", 9, "later.png", glow_record(1));
  geometry::ClusterFileData finished;
  REQUIRE_MESSAGE(geometry::read_cluster_file(later.path, finished, &error), error);
  const ReadClass then = container_class_as_loaded(
      missing, absent,
      std::span<const u8>(finished.content_classes.data(), finished.content_classes.size()),
      categories);
  CHECK(then.classes.levels[static_cast<u32>(categories.find("fixture_glow"))] == 1);
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
  // Every triangle faces out of the box (counter-clockwise seen from outside, as every built mesh
  // is), and the twelve close it: their area-weighted normals sum to nothing.
  const Vec3 middle{(lo.x + hi.x) * 0.5f, (lo.y + hi.y) * 0.5f, (lo.z + hi.z) * 0.5f};
  const geometry::ClusterDesc& cluster = box.mesh.clusters[0];
  Vec3 closure{0.0f, 0.0f, 0.0f};
  u32 outward = 0;
  for (u32 t = 0; t < cluster.triangle_count; ++t) {
    const u32 packed = box.mesh.triangles[cluster.triangle_offset + t];
    Vec3 corner[3];
    for (u32 k = 0; k < 3; ++k)
      corner[k] =
          box.mesh.vertices[cluster.vertex_offset + geometry::ClusterMesh::unpack(packed, k)];
    const Vec3 n = cross(corner[1] - corner[0], corner[2] - corner[0]);
    const Vec3 centroid = (corner[0] + corner[1] + corner[2]) * (1.0f / 3.0f);
    outward += dot(n, centroid - middle) > 0.0f ? 1u : 0u;
    closure = closure + n;
  }
  CHECK(outward == 12u);
  CHECK(length(closure) < 1e-4f);
  const assets::Material material = proxy_material();
  CHECK(material.base_color_image == -1);
  CHECK(material.base_color.x == k_proxy_albedo);
}

namespace {

constexpr u32 k_shot_width = 192;
constexpr u32 k_shot_height = 128;
// The meshes side by side (`scene_of`: 30 m apart), seen from above their middle. High enough that
// every triangle of the proxy fits the software rasterizer's box (below): from 40 m its lid's two
// triangles each had a box of 9,328 pixels, and that path drew the walls alone.
const Vec3 k_shot_eye{15.0f, 70.0f, -7.0f};
const Vec3 k_shot_target{15.0f, 0.0f, 0.0f};

// `k_max_bbox_pixels` in cluster_sw_raster.slang: `--raster sw` draws every pair itself and skips
// a triangle whose pixel bounding box is larger than this (renderer.md, "Content classes at load").
// A tessellated mesh's triangles stay under it from far less height than the proxy's twelve do.
constexpr u32 k_software_box_pixels = 64 * 64;

Camera shot_camera() {
  Camera camera;
  camera.position = absolute(WorldPos::origin(), k_shot_eye);
  camera.target = absolute(WorldPos::origin(), k_shot_target);
  return camera;
}

// The largest pixel bounding box any leaf triangle of `instance` has in the shot, measured as the
// software rasterizer measures it (clamped to the target; a triangle off the target or reaching
// behind the eye is not counted, because that rasterizer skips it for its own reason).
u32 largest_triangle_box(const SceneData& data, u32 instance) {
  ViewSet views;
  REQUIRE(views.build(ViewSetDesc{}, k_shot_width, k_shot_height));
  views.update(shot_camera());
  const Mat4& view_proj = views[0].view_proj;
  const gfx::InstanceDesc& desc = data.instances[instance];
  const geometry::ClusterMeshPart& part = data.parts[desc.mesh];
  // The frame's space: metres from the eye, the instance's cell moved there in f64.
  const Vec3 cell{static_cast<f32>(64.0 * desc.cell.x - static_cast<f64>(k_shot_eye.x)),
                  static_cast<f32>(64.0 * desc.cell.y - static_cast<f64>(k_shot_eye.y)),
                  static_cast<f32>(64.0 * desc.cell.z - static_cast<f64>(k_shot_eye.z))};
  const f32 w = static_cast<f32>(k_shot_width);
  const f32 h = static_cast<f32>(k_shot_height);
  u32 largest = 0;
  const u32 first = part.first_cluster + part.first_leaf_cluster;
  for (u32 c = first; c < first + part.leaf_cluster_count; ++c) {
    const geometry::ClusterDesc& cluster = data.lod.mesh.clusters[c];
    for (u32 t = 0; t < cluster.triangle_count; ++t) {
      const u32 packed = data.lod.mesh.triangles[cluster.triangle_offset + t];
      Vec2 lo{1e30f, 1e30f};
      Vec2 hi{-1e30f, -1e30f};
      bool in_front = true;
      for (u32 k = 0; k < 3; ++k) {
        const Vec3 p =
            data.lod.mesh
                .vertices[cluster.vertex_offset + geometry::ClusterMesh::unpack(packed, k)];
        const Vec3 q{dot(Vec3{desc.rows[0].x, desc.rows[0].y, desc.rows[0].z}, p) + desc.rows[0].w,
                     dot(Vec3{desc.rows[1].x, desc.rows[1].y, desc.rows[1].z}, p) + desc.rows[1].w,
                     dot(Vec3{desc.rows[2].x, desc.rows[2].y, desc.rows[2].z}, p) + desc.rows[2].w};
        const Vec4 clip = view_proj * Vec4{q.x + cell.x, q.y + cell.y, q.z + cell.z, 1.0f};
        in_front = in_front && clip.w > 1e-6f;
        if (!in_front) break;
        const f32 x = (clip.x / clip.w * 0.5f + 0.5f) * w;
        const f32 y = (0.5f - clip.y / clip.w * 0.5f) * h;
        lo = Vec2{std::min(lo.x, x), std::min(lo.y, y)};
        hi = Vec2{std::max(hi.x, x), std::max(hi.y, y)};
      }
      if (!in_front || hi.x < 0.0f || hi.y < 0.0f || lo.x >= w || lo.y >= h) continue;
      const i32 x0 = static_cast<i32>(std::max(std::floor(lo.x), 0.0f));
      const i32 y0 = static_cast<i32>(std::max(std::floor(lo.y), 0.0f));
      const i32 x1 = static_cast<i32>(std::min(std::ceil(hi.x), w - 1.0f));
      const i32 y1 = static_cast<i32>(std::min(std::ceil(hi.y), h - 1.0f));
      largest = std::max(largest, static_cast<u32>((x1 - x0 + 1) * (y1 - y0 + 1)));
    }
  }
  return largest;
}

struct Shot {
  CapturedFrame frame;
  Vector<u8> reference;  // the reference integrator's colour, when asked for and available
  bool available = true;
  std::string why;
};

// The scene in the shot (`k_shot_eye`), every channel captured, under `shadows`. `overhead` lifts
// the second mesh 6 m over the first, so that it would shadow it; `reference` also renders the
// reference integrator's picture where the device can.
Shot shoot(const gfx::Device& device, const test::TempDir& tmp,
           std::initializer_list<const Grid*> meshes, const content_class::NamedPolicy& policy,
           RasterMode raster, ShadowMode shadows = ShadowMode::Cascaded, bool overhead = false,
           bool reference = false);

// The same of a scene description the caller made.
Shot shoot_scene(const gfx::Device& device, const SceneDesc& desc, RasterMode raster,
                 ShadowMode shadows, bool reference) {
  Shot shot;
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  RenderSettings settings;
  settings.raster = raster;
  settings.shadows = shadows;
  settings.occlusion = false;
  settings.lights = false;
  ResolvedSettings resolved;
  resolve_settings(settings, device.features(), &data, resolved);
  const RenderAvailability availability = check_availability(resolved, device.features());
  if (availability != RenderAvailability::Ok) {
    shot.available = false;
    shot.why = unavailable_reason(availability, device);
    return shot;
  }
  GpuScene scene;
  REQUIRE_MESSAGE(scene.create(device, data, resolved, &error), error);
  SceneRenderer renderer;
  SceneRenderer::Desc rd;
  rd.width = k_shot_width;
  rd.height = k_shot_height;
  REQUIRE_MESSAGE(renderer.create(device, scene, resolved, rd, &error), error);
  FrameDesc frame;
  frame.camera = shot_camera();
  CaptureChannels channels;
  channels.color = true;
  channels.ids = true;
  channels.depth = true;
  channels.normals = true;
  REQUIRE_MESSAGE(renderer.capture(frame, channels, shot.frame, &error), error);
  if (reference && reference_available(resolved, device)) {
    ReferenceRenderer integrator;
    REQUIRE_MESSAGE(integrator.create(device, scene, renderer, ReferenceRenderer::Desc{}, &error),
                    error);
    ReferenceSettings rs;
    rs.spp = 8;
    rs.pixel_center = true;
    ReferenceFrame picture;
    REQUIRE_MESSAGE(integrator.render(shot_camera(), rs, picture, &error), error);
    shot.reference = std::move(picture.color);
  }
  return shot;
}

Shot shoot(const gfx::Device& device, const test::TempDir& tmp,
           std::initializer_list<const Grid*> meshes, const content_class::NamedPolicy& policy,
           RasterMode raster, ShadowMode shadows, bool overhead, bool reference) {
  SceneDesc desc = scene_of(tmp, meshes);
  if (overhead && desc.instances.size() > 1) desc.instances[1].origin = WorldPos{0.0, 6.0, 0.0};
  desc.view_policy = policy;
  return shoot_scene(device, desc, raster, shadows, reference);
}

template <typename T>
u64 bytes_of(const Vector<T>& v) {
  return hash_bytes(v.data(), v.size() * sizeof(T));
}

// Pixels whose first id word names `instance`.
u32 pixels_of(const Shot& s, u32 instance) {
  u32 n = 0;
  for (u32 p = 0; p < k_shot_width * k_shot_height; ++p)
    n += s.frame.ids[u64{p} * k_id_words] == instance ? 1u : 0u;
  return n;
}

}  // namespace

// **The shot keeps the proxy inside the software rasterizer's box.** The case below holds every
// path to the hardware's box, and `--raster sw` skips any triangle wider than 64 x 64 pixels of
// bounding box. A box's face is two triangles each as wide as the face, so from too near the
// software path draws the box's walls and not its lid, whatever the proxy is; this is the shot's
// half of that, held without a device.
TEST_CASE("content classes at load: the capture's shot keeps the proxy in the software box") {
  const test::TempDir tmp("renderer_classes_shot");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(2));
  SceneDesc desc = scene_of(tmp, {&a, &glow});
  desc.view_policy = policy_named("glow-proxy");
  SceneData data;
  std::string error;
  REQUIRE_MESSAGE(load_scene(desc, data, error), error);
  REQUIRE(data.instances.size() == 2);
  const u32 largest = largest_triangle_box(data, 1);
  MESSAGE("the proxy's largest triangle box in the shot: " << largest << " pixels");
  CHECK(largest > 0u);
  CHECK(largest <= k_software_box_pixels);
}

// **The capture holds the box or nothing** (for the merge's GPU run, and a run by name on both
// tiers): under `agent-default` the classified grid is not in any channel (colour, ids, depth and
// normals are the bytes of the scene without it); under the proxy policy a box stands where it
// stood; under a policy that covers it the grid itself; on every rasterizer and the ray path.
TEST_CASE("content classes at load: a capture holds the box or nothing, on every path") {
  gfx::Device device;
  std::string error;
  if (!device.create(renderer_test::device_options(), &error) ||
      !device.features().buffer_int64_atomics) {
    MESSAGE("skipped: no Vulkan device with 64-bit buffer atomics: " << error);
    return;
  }
  const test::TempDir tmp("renderer_classes_capture");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(2));
  const content_class::NamedPolicy omit = agent_default_view_policy();
  const content_class::NamedPolicy proxy = policy_named("glow-proxy");
  const content_class::NamedPolicy wide = policy_named("glow-wide");
  u32 hardware_box = 0;
  for (const RasterMode mode :
       {RasterMode::Hardware, RasterMode::Vertex, RasterMode::Software, RasterMode::RayTrace}) {
    const Shot omitted = shoot(device, tmp, {&a, &glow}, omit, mode);
    if (!omitted.available) {
      MESSAGE("raster mode " << static_cast<u32>(mode) << " unavailable: " << omitted.why);
      continue;
    }
    const Shot alone = shoot(device, tmp, {&a}, omit, mode);
    const Shot boxed = shoot(device, tmp, {&a, &glow}, proxy, mode);
    const Shot whole = shoot(device, tmp, {&a, &glow}, wide, mode);
    INFO("raster mode " << static_cast<u32>(mode));
    // Omitted: every channel the bytes of the scene that never had it.
    CHECK(bytes_of(omitted.frame.ids) == bytes_of(alone.frame.ids));
    CHECK(bytes_of(omitted.frame.depth) == bytes_of(alone.frame.depth));
    CHECK(bytes_of(omitted.frame.normals) == bytes_of(alone.frame.normals));
    CHECK(bytes_of(omitted.frame.color) == bytes_of(alone.frame.color));
    CHECK(pixels_of(omitted, 1) == 0u);
    // Proxied: a box where it stood, and not the grid.
    const u32 box = pixels_of(boxed, 1);
    const u32 grid = pixels_of(whole, 1);
    MESSAGE("raster mode " << static_cast<u32>(mode) << ": box " << box << " pixels, grid " << grid
                           << " pixels");
    CHECK(box > 1000u);
    CHECK(grid > 1000u);
    // Every channel holds the box, not the grid and not nothing.
    CHECK(bytes_of(boxed.frame.ids) != bytes_of(whole.frame.ids));
    CHECK(bytes_of(boxed.frame.color) != bytes_of(whole.frame.color));
    CHECK(bytes_of(boxed.frame.depth) != bytes_of(whole.frame.depth));
    CHECK(bytes_of(boxed.frame.normals) != bytes_of(whole.frame.normals));
    CHECK(bytes_of(boxed.frame.color) != bytes_of(alone.frame.color));
    CHECK(bytes_of(boxed.frame.depth) != bytes_of(alone.frame.depth));
    CHECK(bytes_of(boxed.frame.normals) != bytes_of(alone.frame.normals));
    // Every path draws the box alike: the vertex path to the pixel, the others within its rim.
    if (mode == RasterMode::Hardware) hardware_box = box;
    if (mode == RasterMode::Vertex) CHECK(box == hardware_box);
    if (hardware_box != 0u) {
      CHECK(box + k_shot_width >= hardware_box);
      CHECK(box <= hardware_box + k_shot_width);
    }
  }
  // **Nor its shadow**: the classified grid 6 m over the other, so that drawn it would shadow it,
  // under the maps and under traced shadows, and through the reference integrator: omitted, every
  // channel is the bytes of the scene that never had it; covered, it is there and casts.
  for (const ShadowMode shadows : {ShadowMode::Cascaded, ShadowMode::RayTraced}) {
    const RasterMode mode = RasterMode::Hardware;
    const bool reference = shadows == ShadowMode::RayTraced;
    const Shot omitted = shoot(device, tmp, {&a, &glow}, omit, mode, shadows, true, reference);
    if (!omitted.available) {
      MESSAGE("shadow mode " << static_cast<u32>(shadows) << " unavailable: " << omitted.why);
      continue;
    }
    INFO("shadow mode " << static_cast<u32>(shadows));
    const Shot alone = shoot(device, tmp, {&a}, omit, mode, shadows, true, reference);
    const Shot whole = shoot(device, tmp, {&a, &glow}, wide, mode, shadows, true, reference);
    CHECK(bytes_of(omitted.frame.color) == bytes_of(alone.frame.color));
    CHECK(bytes_of(omitted.frame.ids) == bytes_of(alone.frame.ids));
    CHECK(bytes_of(omitted.frame.depth) == bytes_of(alone.frame.depth));
    CHECK(bytes_of(omitted.frame.normals) == bytes_of(alone.frame.normals));
    CHECK(bytes_of(whole.frame.color) != bytes_of(alone.frame.color));
    // The control is the shadow on the receiver itself: of the pixels the neutral grid covers in
    // both shots (its instance, 0), some are darker with the classified grid over it, so the
    // equalities above cannot hold only because the shadow fell outside the frame.
    u32 receiver = 0;
    u32 darker = 0;
    for (u32 p = 0; p < k_shot_width * k_shot_height; ++p) {
      if (whole.frame.ids[u64{p} * k_id_words] != 0u ||
          alone.frame.ids[u64{p} * k_id_words] != 0u || whole.frame.depth[p] == 0.0f ||
          alone.frame.depth[p] == 0.0f)
        continue;
      ++receiver;
      u32 lit = 0;
      u32 shaded = 0;
      for (u32 c = 0; c < 3; ++c) {
        lit += alone.frame.color[u64{p} * 4u + c];
        shaded += whole.frame.color[u64{p} * 4u + c];
      }
      darker += shaded + 24u <= lit ? 1u : 0u;
    }
    MESSAGE("shadow mode " << static_cast<u32>(shadows) << ": " << darker << " of " << receiver
                           << " receiver pixels darker under the caster");
    CHECK(receiver > 1000u);
    CHECK(darker > 100u);
    if (!omitted.reference.empty()) {
      CHECK(bytes_of(omitted.reference) == bytes_of(alone.reference));
      CHECK(bytes_of(whole.reference) != bytes_of(alone.reference));
    } else {
      MESSAGE("the reference integrator is unavailable on this device");
    }
  }
}

// **The ground holds nothing of a refused layer in any channel** (C64; for the device leg): a
// ground whose one layer image declares a class above the cap, beside a neutral grid. Under
// `agent-default` every channel is the bytes of the same scene with no ground at all; under a
// policy that covers it, the ground is drawn.
TEST_CASE("content classes at load: a capture holds nothing of a refused ground") {
  gfx::Device device;
  std::string error;
  if (!device.create(renderer_test::device_options(), &error) ||
      !device.features().buffer_int64_atomics) {
    MESSAGE("skipped: no Vulkan device with 64-bit buffer atomics: " << error);
    return;
  }
  const test::TempDir tmp("renderer_classes_ground_capture");
  REQUIRE(tmp.ok());
  (void)write_grid(tmp, "a.clusters", 9, "a.png");
  (void)write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
  write_image(tmp.file("ground.png"), 40);
  REQUIRE(io::write_file(tmp.file("ground.provenance.json"),
                         R"({"content_classes":{"fixture_glow":"bright"}})") == io::Status::Ok);
  // The neutral grid in the shot, the dim one that teaches the ladder far out of it.
  const std::string placed =
      R"({"format":"engine.scene.v1","meshes":[{"path":"a.clusters"},{"path":"glow.clusters"}],)"
      R"("instances":[{"mesh":0,"translation":[15,0,0],"rotation":[0,0,0,1]},)"
      R"({"mesh":1,"translation":[400,0,0],"rotation":[0,0,0,1]}])";
  const std::string with_ground =
      placed + R"(,"terrain":{"size":17,"extent":8,)"
               R"("materials":{"layers":[{"base_color":"ground.png","projection":"Map"}]}}})";
  REQUIRE(io::write_file(tmp.file("ground.json"), with_ground) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("bare.json"), placed + "}") == io::Status::Ok);
  const auto desc_of = [&](const char* file, const content_class::NamedPolicy& policy) {
    SceneDesc desc;
    REQUIRE_MESSAGE(read_scene_file(tmp.file(file), desc, error), error);
    desc.ddc = tmp.file("ddc");
    desc.view_policy = policy;
    return desc;
  };
  for (const RasterMode mode : {RasterMode::Hardware, RasterMode::RayTrace}) {
    INFO("raster mode " << static_cast<u32>(mode));
    const Shot refused = shoot_scene(device, desc_of("ground.json", agent_default_view_policy()),
                                     mode, ShadowMode::Cascaded, false);
    if (!refused.available) {
      MESSAGE("raster mode " << static_cast<u32>(mode) << " unavailable: " << refused.why);
      continue;
    }
    const Shot bare = shoot_scene(device, desc_of("bare.json", agent_default_view_policy()), mode,
                                  ShadowMode::Cascaded, false);
    const Shot covered = shoot_scene(device, desc_of("ground.json", policy_named("glow-wide")),
                                     mode, ShadowMode::Cascaded, false);
    CHECK(bytes_of(refused.frame.color) == bytes_of(bare.frame.color));
    CHECK(bytes_of(refused.frame.ids) == bytes_of(bare.frame.ids));
    CHECK(bytes_of(refused.frame.depth) == bytes_of(bare.frame.depth));
    CHECK(bytes_of(refused.frame.normals) == bytes_of(bare.frame.normals));
    CHECK(bytes_of(covered.frame.ids) != bytes_of(bare.frame.ids));
  }
}

namespace {

// A fixture generator of this file's, registered once whichever case reaches it first.
void need_generator(const assets::MeshGeneratorDesc& generator) {
  if (assets::MeshGeneratorRegistry::global().find(generator.name) == nullptr)
    REQUIRE(assets::MeshGeneratorRegistry::global().add(generator));
}

}  // namespace

// **A container host the probe cannot open makes its recipe unreadable** (C64): the host's own read
// has to succeed for the recipe to be built at all, so refusing it at the probe loses nothing,
// where reading it as `none` would build over whatever the finished file's section 54 declares.
TEST_CASE("content classes at load: a recipe over a host missing at the probe is unreadable") {
  need_generator(k_fixture_host);
  const test::TempDir tmp("renderer_classes_host_missing");
  REQUIRE(tmp.ok());
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  REQUIRE(io::write_file(tmp.file("moss.fixturehost.json"), R"({"host":"statue.clusters"})") ==
          io::Status::Ok);
  content_build::MeshOptions options;
  content_build::SourceClasses classes;
  content_build::Diagnostic why;
  CHECK_FALSE(
      content_build::read_source_classes(tmp.file("moss.fixturehost.json"), options, classes, why));
  CHECK_MESSAGE(why.message.find("statue.clusters") != std::string::npos, why.message);
  Grid moss;
  moss.path = tmp.file("moss.fixturehost.json");
  std::string error;
  SceneData scene;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {&a, &moss}), scene, error), error);
  CHECK(scene.instances.size() == 1);
  CHECK(loaded_instance(scene, 1) == k_not_loaded);
  CHECK(std::string(scene.sources[1].cache) == "none");  // never generated
  CHECK(scene.load.unreadable_omitted == 1);
}

namespace {

// `ground_scene`'s ground, with `glow.clusters` teaching the ladder, given a mask level painted
// from `mark.png` beside the file and its palette held to four layers a tile, as masks require. The
// tile set the scene read asks masks for is not the load's: it takes the description as given.
SceneDesc painted_ground(const test::TempDir& tmp) {
  write_image(tmp.file("ground.png"), 40);
  write_image(tmp.file("mark.png"), 90);
  REQUIRE(io::write_file(tmp.file("scene.json"), ground_scene(true)) == io::Status::Ok);
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(tmp.file("scene.json"), desc, error), error);
  desc.ddc = tmp.file("ddc");
  REQUIRE(desc.terrain.materials.has_value());
  desc.terrain.materials->layer_budget = 4;
  scene::TerrainMasks masks;
  scene::TerrainMaskPaint paint;
  paint.image = tmp.file("mark.png");
  masks.paint.push_back(paint);
  desc.terrain.masks = masks;
  return desc;
}

}  // namespace

// **A ground's mask images are its inputs too** (C64): the paint maps and the index map the mask
// level is composited from are read for their sidecars through the same reader as the layers'
// images, so a paint map above the cap leaves the ground out, a policy that covers it draws the
// ground from an entry carrying the class, and nothing beside a map keeps the ground's key.
TEST_CASE("content classes at load: a ground's painted mask above the cap leaves it out") {
  const test::TempDir tmp("renderer_classes_ground_mask");
  REQUIRE(tmp.ok());
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
  SceneDesc desc = painted_ground(tmp);
  const std::string classless = geometry::cluster_cache_path(desc.ddc, classless_ground_key(desc));
  std::string error;
  {
    SceneData plain;
    REQUIRE_MESSAGE(load_scene(desc, plain, error), error);
    CHECK(plain.load.grounds_omitted == 0);
    REQUIRE(plain.terrain_mesh < plain.sources.size());
    CHECK(plain.sources[plain.terrain_mesh].container == classless);
  }
  REQUIRE(io::write_file(tmp.file("mark.provenance.json"),
                         R"({"content_classes":{"fixture_glow":"bright"}})") == io::Status::Ok);
  SceneData refused;
  REQUIRE_MESSAGE(load_scene(desc, refused, error), error);
  CHECK(refused.load.grounds_omitted == 1);
  CHECK(refused.terrain_mesh == ~0u);
  CHECK_FALSE(refused.terrain.enabled);
  CHECK(refused.instances.empty());  // the dim container is refused too under `agent-default`
  desc.view_policy = policy_named("glow-wide");
  SceneData wide;
  REQUIRE_MESSAGE(load_scene(desc, wide, error), error);
  CHECK(wide.load.grounds_omitted == 0);
  REQUIRE(wide.terrain_mesh < wide.sources.size());
  const SourceMesh& ground = wide.sources[wide.terrain_mesh];
  REQUIRE_FALSE(ground.container.empty());
  CHECK(ground.container != classless);
  geometry::ClusterFileData entry;
  REQUIRE_MESSAGE(geometry::read_cluster_file(ground.container, entry, &error), error);
  CHECK_FALSE(entry.content_classes.empty());
  CHECK(wide.held.levels[static_cast<u32>(wide.categories.find("fixture_glow"))] == 2);
}

// A sidecar beside a paint map that leads nowhere refuses the ground, as one beside a layer's image
// does.
TEST_CASE("content classes at load: a dangling sidecar beside a ground's paint map refuses it") {
  u32 made = 0;
  for (const DanglingForm& form : k_dangling_forms) {
    const std::string form_name = form.name;
    CAPTURE(form_name);
    const test::TempDir tmp("renderer_classes_mask_nowhere");
    REQUIRE(tmp.ok());
    const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
    const SceneDesc desc = painted_ground(tmp);
    if (!form.make(tmp.file("gone"), tmp.file("mark.provenance.json"))) {
      MESSAGE("skipped: this process may not make " << form.name);
      continue;
    }
    ++made;
    std::string error;
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    CHECK(scene.load.grounds_omitted == 1);
    CHECK(scene.terrain_mesh == ~0u);
  }
  CHECK(made >= 1u);
}

namespace {

// A generator of the test's own that grows over its recipe's host as `fixturehost` does and builds
// `g_cover_mesh`, a grid with no image, so a recipe the door admits is drawn.
const geometry::ClusterFileData* g_cover_mesh = nullptr;
bool fixture_cover_build(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                         const geometry::ClusterLodOptions& lod, u32 page_bytes,
                         geometry::ClusterFileData& out, std::string& error) {
  u64 identity = 0;
  if (!fixture_host_hash_with(bytes, inputs, identity, error)) return false;
  if (g_cover_mesh == nullptr) {
    error = "the fixture cover has no mesh to build";
    return false;
  }
  out = *g_cover_mesh;
  // The identity a real cover records (cover.cpp), so a reload finds the entry it wrote.
  out.source_hash = identity;
  out.build_key = assets::generated_mesh_build_key(
      identity, assets::MeshGeneratorInputs{inputs.materials}, lod, page_bytes);
  return true;
}
constexpr assets::MeshGeneratorDesc k_fixture_cover{.name = "fixturecover",
                                                    .source_hash = fixture_host_hash,
                                                    .build = fixture_cover_build,
                                                    .source_hash_with = fixture_host_hash_with};

// A grid of `side` vertices a side with one material that samples no image.
geometry::ClusterFileData plain_grid(u32 side) {
  SceneDesc grid;
  grid.meshes.push_back("");
  grid.heightfield_grid = side;
  grid.cache = false;
  SceneData made;
  std::string error;
  REQUIRE_MESSAGE(load_scene(grid, made, error), error);
  geometry::ClusterFileData file;
  file.mesh = std::move(made.lod);
  file.materials.push_back(geometry::ClusterFileMaterial{});
  file.cluster_material = Vector<u32>(file.mesh.mesh.clusters.size(), 0u);
  return file;
}

content_class::NamedPolicy policy_in(const char* json, const char* name) {
  content_class::ViewPolicies set;
  std::string error;
  REQUIRE_MESSAGE(set.read_json(content_class::Categories(), json, error), error);
  for (u32 i = 0; i < set.size(); ++i)
    if (set.at(i).name == name) return set.at(i);
  FAIL("no such policy");
  return agent_default_view_policy();
}

// The game's category capped at `dim` and the engine's violence at `mild`: what tells a host's real
// level from the top a class that cannot be read is decided at.
constexpr const char* k_dim_policies =
    R"({"policies":[{"name":"glow-dim","caps":[)"
    R"({"category":"fixture_glow","cap":"dim","substitution":"omit"},)"
    R"({"category":"violence","cap":"mild","substitution":"omit"}]}]})";

}  // namespace

// **A recipe is decided the same in either mesh order, and at its host's real level** (C64): a
// container host's record teaches the scene's table its ladders as a listed container's does,
// whole or not at all, so a cover listed before its host, or over a host the scene does not list,
// reads the host's level rather than meeting a category the table does not know yet.
TEST_CASE("content classes at load: a recipe over a game-classified host decides in any order") {
  need_generator(k_fixture_cover);
  const geometry::ClusterFileData cover = plain_grid(5);
  g_cover_mesh = &cover;
  const content_class::NamedPolicy dim = policy_in(k_dim_policies, "glow-dim");
  std::string error;
  {
    const test::TempDir tmp("renderer_classes_host_order");
    REQUIRE(tmp.ok());
    const Grid statue = write_grid(tmp, "statue.clusters", 17, "statue.png", glow_record(1));
    REQUIRE(io::write_file(tmp.file("moss.fixturecover.json"), R"({"host":"statue.clusters"})") ==
            io::Status::Ok);
    Grid moss;
    moss.path = tmp.file("moss.fixturecover.json");
    // The cover's entry is classified (its host's class), and the second order hits the one the
    // first wrote, under the same key.
    std::string entry;
    for (const bool moss_first : {true, false}) {
      CAPTURE(moss_first);
      SceneDesc desc =
          moss_first ? scene_of(tmp, {&moss, &statue}) : scene_of(tmp, {&statue, &moss});
      desc.view_policy = dim;
      SceneData scene;
      REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
      CHECK(scene.instances.size() == 2);
      CHECK(loaded_instance(scene, moss_first ? 0u : 1u) != k_not_loaded);
      CHECK(scene.load.unreadable_omitted == 0);
      const SourceMesh& made = scene.sources[moss_first ? 0u : 1u];
      REQUIRE_FALSE(made.container.empty());
      CHECK(std::string(made.cache) == std::string(moss_first ? "miss" : "hit"));
      if (moss_first) {
        entry = made.container;
        geometry::ClusterFileData file;
        REQUIRE_MESSAGE(geometry::read_cluster_file(entry, file, &error), error);
        CHECK_FALSE(file.content_classes.empty());
      }
      CHECK(made.container == entry);
    }
  }
  // Over a host the scene does not list: the host's own level, admitted at `dim`, refused above it.
  for (const u8 level : {u8{1}, u8{2}}) {
    CAPTURE(static_cast<u32>(level));
    const test::TempDir tmp("renderer_classes_host_unlisted");
    REQUIRE(tmp.ok());
    (void)write_grid(tmp, "statue.clusters", 17, "statue.png", glow_record(level));
    REQUIRE(io::write_file(tmp.file("moss.fixturecover.json"), R"({"host":"statue.clusters"})") ==
            io::Status::Ok);
    Grid moss;
    moss.path = tmp.file("moss.fixturecover.json");
    SceneDesc desc = scene_of(tmp, {&moss});
    desc.view_policy = dim;
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    CHECK(scene.load.unreadable_omitted == 0);
    if (level == 1) {
      CHECK(scene.instances.size() == 1);
    } else {
      CHECK(scene.instances.empty());
      REQUIRE(row(scene, "fixture_glow") != nullptr);
      CHECK(row(scene, "fixture_glow")->omitted == 1);
    }
  }
  g_cover_mesh = nullptr;
}

namespace {

// Section 54 for the made-up category `name` at `level` of the ladder `categories` gives it.
Vector<u8> record_of(const char* categories, const char* name, u8 level) {
  content_class::Categories table;
  std::string error;
  REQUIRE_MESSAGE(table.add_json(categories, error), error);
  content_class::ContentClasses classes;
  classes.levels[static_cast<u32>(table.find(name))] = level;
  Vector<u8> record;
  content_class::encode_classes(table, classes, nullptr, record);
  REQUIRE_FALSE(record.empty());
  return record;
}

// A second made-up category, and two policies over both: everything admitted, or the glow held to
// `dim` and the hum to `none`.
constexpr const char* k_hum_categories =
    R"({"categories":[{"name":"fixture_hum","levels":["none","low","loud"]}]})";
constexpr const char* k_two_policies =
    R"({"policies":[)"
    R"({"name":"two-wide","caps":[{"category":"fixture_glow","cap":"bright","substitution":"omit"},)"
    R"({"category":"fixture_hum","cap":"loud","substitution":"omit"}]},)"
    R"({"name":"hum-off","caps":[{"category":"fixture_glow","cap":"dim","substitution":"omit"},)"
    R"({"category":"fixture_hum","cap":"none","substitution":"omit"}]}]})";

// `scene_of` over `count` meshes in the order given.
SceneDesc scene_in(const test::TempDir& tmp, const Grid* const* meshes, u32 count) {
  SceneDesc desc;
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  for (u32 m = 0; m < count; ++m) {
    SceneInstance instance{m, Transform3::identity()};
    instance.origin = WorldPos{30.0 * m, 0.0, 0.0};
    desc.instances.push_back(instance);
    desc.meshes.push_back(meshes[m]->path);
  }
  return desc;
}

// What a load decided, as text that does not depend on the order of the scene's meshes: each
// instance's mesh (by path, the ground by name), whether it was loaded and the container its bytes
// came from, whose name is its key; the counts by row; the unreadable and the grounds left out.
std::string outcome_of(const SceneDesc& desc, const SceneData& scene) {
  Vector<std::string> lines;
  for (u32 i = 0; i < desc.instances.size(); ++i) {
    const u32 m = desc.instances[i].mesh;
    const bool loaded = loaded_instance(scene, i) != k_not_loaded;
    lines.push_back((desc.meshes[m].empty() ? std::string("(ground)") : desc.meshes[m]) +
                    (loaded ? " loaded from " : " left out, ") +
                    (m < scene.sources.size() ? scene.sources[m].container : std::string()));
  }
  for (const SubstitutedCount& r : scene.load.substituted)
    lines.push_back("row " + r.category + ": " + std::to_string(r.omitted) + " omitted, " +
                    std::to_string(r.proxied) + " proxied");
  std::sort(lines.begin(), lines.end());
  std::string out = "unreadable " + std::to_string(scene.load.unreadable_omitted) + " omitted, " +
                    std::to_string(scene.load.unreadable_proxied) + " proxied; grounds " +
                    std::to_string(scene.load.grounds_omitted) + " omitted\n";
  for (const std::string& line : lines)
    out += line + "\n";
  return out;
}

}  // namespace

// **Every ladder is learned before any class is read** (C64): a cover's own sidecar naming the
// category its host teaches, above the host's level, reads at its level whether the cover is listed
// before its host, after it, or alone: admitted under a cap at that level, and refused under one
// below it by the category's row, never as a class that could not be read.
TEST_CASE("content classes at load: a cover's own sidecar reads before its host, after, alone") {
  need_generator(k_fixture_cover);
  const geometry::ClusterFileData cover = plain_grid(5);
  g_cover_mesh = &cover;
  const test::TempDir tmp("renderer_classes_cover_sidecar");
  REQUIRE(tmp.ok());
  const Grid statue = write_grid(tmp, "statue.clusters", 17, "statue.png", glow_record(1));
  REQUIRE(io::write_file(tmp.file("moss.fixturecover.json"), R"({"host":"statue.clusters"})") ==
          io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("moss.fixturecover.provenance.json"),
                         R"({"content_classes":{"fixture_glow":"bright"}})") == io::Status::Ok);
  Grid moss;
  moss.path = tmp.file("moss.fixturecover.json");
  std::string error;
  for (const char* name : {"glow-wide", "glow-dim"}) {
    const std::string policy_name = name;
    CAPTURE(policy_name);
    const bool admits = policy_name == "glow-wide";
    for (const u32 form : {0u, 1u, 2u}) {
      CAPTURE(form);  // 0: the cover first, 1: its host first, 2: the cover alone
      SceneDesc desc = form == 0   ? scene_of(tmp, {&moss, &statue})
                       : form == 1 ? scene_of(tmp, {&statue, &moss})
                                   : scene_of(tmp, {&moss});
      desc.view_policy = admits ? policy_named(name) : policy_in(k_dim_policies, name);
      SceneData scene;
      REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
      CHECK(scene.load.unreadable_omitted == 0);
      CHECK((loaded_instance(scene, form == 1 ? 1u : 0u) != k_not_loaded) == admits);
      // The dim statue is admitted under both.
      CHECK(scene.instances.size() == desc.meshes.size() - (admits ? 0u : 1u));
      if (!admits) {
        REQUIRE(row(scene, "fixture_glow") != nullptr);
        CHECK(row(scene, "fixture_glow")->omitted == 1);
      }
    }
  }
  g_cover_mesh = nullptr;
}

// **A host the scene does not list teaches in the second round** (C64), before any class is read,
// so a glTF whose sidecar names the category only that host carries reads at its level in either
// order: both are admitted under a cap at that level.
TEST_CASE("content classes at load: an unlisted host's ladder reaches a glTF in either order") {
  need_generator(k_fixture_cover);
  const geometry::ClusterFileData cover = plain_grid(5);
  g_cover_mesh = &cover;
  const test::TempDir tmp("renderer_classes_unlisted_teacher");
  REQUIRE(tmp.ok());
  (void)write_grid(tmp, "statue.clusters", 17, "statue.png", glow_record(1));
  REQUIRE(io::write_file(tmp.file("moss.fixturecover.json"), R"({"host":"statue.clusters"})") ==
          io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("tri.gltf"), k_triangle_gltf) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("tri.provenance.json"),
                         R"({"content_classes":{"fixture_glow":"dim"}})") == io::Status::Ok);
  Grid moss;
  moss.path = tmp.file("moss.fixturecover.json");
  Grid tri;
  tri.path = tmp.file("tri.gltf");
  std::string error;
  for (const bool cover_first : {true, false}) {
    CAPTURE(cover_first);
    SceneDesc desc = cover_first ? scene_of(tmp, {&moss, &tri}) : scene_of(tmp, {&tri, &moss});
    desc.view_policy = policy_in(k_dim_policies, "glow-dim");
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    CHECK(scene.load.unreadable_omitted == 0);
    CHECK(scene.instances.size() == 2);
    CHECK(loaded_instance(scene, cover_first ? 1u : 0u) != k_not_loaded);
  }
  g_cover_mesh = nullptr;
}

// **A listed container out-ranks a host the scene does not list** (C64): the unlisted host teaches
// in the second round, so whichever order the scene lists the two, the listed container's ladder
// stands, the listed container is read at its level, and the host whose ladder differs is refused
// as a second listed container's would be, which leaves its recipe unreadable.
TEST_CASE("content classes at load: a listed container's ladder stands over an unlisted host's") {
  need_generator(k_fixture_cover);
  const geometry::ClusterFileData cover = plain_grid(5);
  g_cover_mesh = &cover;
  const test::TempDir tmp("renderer_classes_listed_ranks");
  REQUIRE(tmp.ok());
  const Grid lamp = write_grid(tmp, "lamp.clusters", 17, "lamp.png", glow_record(1));
  (void)write_grid(
      tmp, "odd.clusters", 13, "odd.png",
      glow_record(1, R"({"categories":[{"name":"fixture_glow","levels":["none","bright"]}]})"));
  REQUIRE(io::write_file(tmp.file("vine.fixturecover.json"), R"({"host":"odd.clusters"})") ==
          io::Status::Ok);
  Grid vine;
  vine.path = tmp.file("vine.fixturecover.json");
  std::string error;
  for (const bool lamp_first : {true, false}) {
    CAPTURE(lamp_first);
    SceneDesc desc = lamp_first ? scene_of(tmp, {&lamp, &vine}) : scene_of(tmp, {&vine, &lamp});
    desc.view_policy = policy_in(k_dim_policies, "glow-dim");
    SceneData scene;
    REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
    CHECK(loaded_instance(scene, lamp_first ? 0u : 1u) != k_not_loaded);
    CHECK(loaded_instance(scene, lamp_first ? 1u : 0u) == k_not_loaded);
    CHECK(scene.instances.size() == 1);
    CHECK(scene.load.unreadable_omitted == 1);
    CHECK(std::string(scene.sources[lamp_first ? 1u : 0u].cache) == "none");  // never generated
  }
  g_cover_mesh = nullptr;
}

// **The outcome does not depend on the order of the meshes** (C64): a listed classified container,
// a cover over it with its own sidecar, a cover over a host the scene does not list carrying a
// second category, a glTF whose sidecar names both and a ground whose sidecar names the second,
// loaded in every one of the 120 orders of the mesh list under two policies, decide the same
// instances, the same counts by row and the same unreadable count, and draw each asset from the
// same entry, whose name is its key.
TEST_CASE("content classes at load: every order of a scene's meshes decides it alike") {
  need_generator(k_fixture_cover);
  const geometry::ClusterFileData cover = plain_grid(5);
  g_cover_mesh = &cover;
  const test::TempDir tmp("renderer_classes_every_order");
  REQUIRE(tmp.ok());
  const Grid lamp = write_grid(tmp, "lamp.clusters", 17, "lamp.png", glow_record(1));
  (void)write_grid(tmp, "bell.clusters", 13, "bell.png",
                   record_of(k_hum_categories, "fixture_hum", 1));
  REQUIRE(io::write_file(tmp.file("moss.fixturecover.json"), R"({"host":"lamp.clusters"})") ==
          io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("moss.fixturecover.provenance.json"),
                         R"({"content_classes":{"fixture_glow":"bright"}})") == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("vine.fixturecover.json"), R"({"host":"bell.clusters"})") ==
          io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("tri.gltf"), k_triangle_gltf) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("tri.provenance.json"),
                         R"({"content_classes":{"fixture_glow":"dim","fixture_hum":"low"}})") ==
          io::Status::Ok);
  write_image(tmp.file("ground.png"), 40);
  REQUIRE(io::write_file(tmp.file("ground.provenance.json"),
                         R"({"content_classes":{"fixture_hum":"low"}})") == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("scene.json"), ground_scene(false)) == io::Status::Ok);
  std::string error;
  SceneDesc ground;
  REQUIRE_MESSAGE(read_scene_file(tmp.file("scene.json"), ground, error), error);
  Grid moss;
  moss.path = tmp.file("moss.fixturecover.json");
  Grid vine;
  vine.path = tmp.file("vine.fixturecover.json");
  Grid tri;
  tri.path = tmp.file("tri.gltf");
  const Grid field{};  // the ground: the empty path
  const Grid* const all[5] = {&lamp, &moss, &vine, &tri, &field};
  for (const char* name : {"two-wide", "hum-off"}) {
    const std::string policy_name = name;
    CAPTURE(policy_name);
    const bool wide = policy_name == "two-wide";
    u32 at[5] = {0, 1, 2, 3, 4};
    std::string first;
    u32 orders = 0;
    do {
      const Grid* order[5];
      std::string listed;
      for (u32 i = 0; i < 5; ++i) {
        order[i] = all[at[i]];
        listed += std::to_string(at[i]);
      }
      CAPTURE(listed);
      SceneDesc desc = scene_in(tmp, order, 5);
      desc.terrain = ground.terrain;
      desc.view_policy = policy_in(k_two_policies, name);
      SceneData scene;
      REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
      CHECK(scene.load.unreadable_omitted == 0);
      CHECK(scene.load.grounds_omitted == (wide ? 0u : 1u));
      // Wide, all five; with the glow at `dim` and the hum at `none`, the lamp alone.
      CHECK(scene.instances.size() == (wide ? 5u : 1u));
      const std::string outcome = outcome_of(desc, scene);
      if (orders == 0) first = outcome;
      CHECK(outcome == first);
      ++orders;
    } while (std::next_permutation(at, at + 5));
    CHECK(orders == 120u);
  }
  g_cover_mesh = nullptr;
}

// **The class read follows a host chain exactly as deep as the hosts' seam does** (C64):
// `GeneratorHosts` identifies hosts down to `k_host_depth` levels and refuses a chain one deeper,
// and the class read takes the same chain, joining its last host's class, and refuses the same one.
TEST_CASE("content classes at load: a recipe's host chain reads as deep as it builds") {
  need_generator(k_fixture_host);
  const test::TempDir tmp("renderer_classes_host_depth");
  REQUIRE(tmp.ok());
  (void)write_grid(tmp, "leaf.clusters", 9, "leaf.png", glow_record(2));
  content_class::Categories table;
  std::string error;
  REQUIRE_MESSAGE(table.add_json(k_glow_categories, error), error);
  content_build::MeshOptions options;
  options.categories = &table;
  // `r0` grows over `r1`, and so on; the chain's last recipe grows over the leaf.
  const auto chain = [&](u32 recipes) {
    for (u32 i = 0; i < recipes; ++i) {
      const std::string host = i + 1 < recipes ? "r" + std::to_string(i + 1) + ".fixturehost.json"
                                               : std::string("leaf.clusters");
      REQUIRE(io::write_file(tmp.file("r" + std::to_string(i) + ".fixturehost.json"),
                             R"({"host":")" + host + R"("})") == io::Status::Ok);
    }
    return tmp.file("r0.fixturehost.json");
  };
  for (const u32 recipes : {content_build::k_host_depth, content_build::k_host_depth + 1}) {
    CAPTURE(recipes);
    const std::string top = chain(recipes);
    std::string text;
    REQUIRE(io::read_file(top, text) == io::Status::Ok);
    const content_build::GeneratorHosts hosts(top, geometry::ClusterLodOptions{},
                                              geometry::ClusterPagesOptions{}.page_bytes, nullptr);
    u64 identity = 0;
    std::string why;
    const bool identified = assets::generated_source_hash(
        k_fixture_host, std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()),
        hosts.inputs(), identity, why);
    CHECK_MESSAGE(identified == (recipes == content_build::k_host_depth), why);
    content_build::SourceClasses classes;
    content_build::Diagnostic diagnostic;
    const bool read = content_build::read_source_classes(top, options, classes, diagnostic);
    CHECK_MESSAGE(read == identified, diagnostic.message);
    if (read) CHECK(classes.classes.levels[static_cast<u32>(table.find("fixture_glow"))] == 2);
  }
}

namespace {

// A generator whose hash reads an image beside its recipe and refuses an empty one, as a generator
// that parses an image it hashes would; it grows over a host as `fixturehost` does.
bool fixture_image_hash_with(std::span<const u8> bytes, const assets::MeshGeneratorInputs& inputs,
                             u64& out, std::string& error) {
  Vector<u8> image;
  if (inputs.hosts == nullptr ||
      !inputs.hosts->read_image(inputs.hosts->context, "skin.png", image, error))
    return false;
  if (image.empty()) {
    error = "an empty image is not one";
    return false;
  }
  return fixture_host_hash_with(bytes, inputs, out, error);
}
constexpr assets::MeshGeneratorDesc k_fixture_image{.name = "fixtureimage",
                                                    .source_hash = fixture_host_hash,
                                                    .build = fixture_host_build,
                                                    .source_hash_with = fixture_image_hash_with};

}  // namespace

// **A recipe whose hash refuses the class probe is unreadable** (C64): the probe answers every
// image with no bytes, so a generator that hashes an image's content can refuse it; its hosts are
// then unknown, and the recipe is refused rather than decided without them.
TEST_CASE("content classes at load: a recipe whose hash refuses the probe is unreadable") {
  need_generator(k_fixture_image);
  const test::TempDir tmp("renderer_classes_probe_hash");
  REQUIRE(tmp.ok());
  write_image(tmp.file("skin.png"), 60);
  (void)write_grid(tmp, "statue.clusters", 9, "statue.png", glow_record(2));
  const std::string recipe = tmp.file("moss.fixtureimage.json");
  REQUIRE(io::write_file(recipe, R"({"host":"statue.clusters"})") == io::Status::Ok);
  content_class::Categories table;
  std::string error;
  REQUIRE_MESSAGE(table.add_json(k_glow_categories, error), error);
  content_build::MeshOptions options;
  options.categories = &table;
  content_build::SourceClasses classes;
  content_build::Diagnostic diagnostic;
  CHECK_FALSE(content_build::read_source_classes(recipe, options, classes, diagnostic));
  CHECK_MESSAGE(diagnostic.message.find("moss.fixtureimage.json") != std::string::npos,
                diagnostic.message);
  // Under the hosts' own hooks the image is read and the recipe's hash is taken.
  std::string text;
  REQUIRE(io::read_file(recipe, text) == io::Status::Ok);
  const content_build::GeneratorHosts hosts(recipe, geometry::ClusterLodOptions{},
                                            geometry::ClusterPagesOptions{}.page_bytes, nullptr);
  u64 identity = 0;
  CHECK_MESSAGE(assets::generated_source_hash(
                    k_fixture_image,
                    std::span<const u8>(reinterpret_cast<const u8*>(text.data()), text.size()),
                    hosts.inputs(), identity, error),
                error);
}

namespace {

// Every engine category at its top and the game's too: a policy that admits the class a source that
// cannot be read is decided at.
constexpr const char* k_top_policies =
    R"({"policies":[{"name":"every-top","caps":[)"
    R"({"category":"nudity","cap":"full","substitution":"omit"},)"
    R"({"category":"sexual","cap":"explicit","substitution":"omit"},)"
    R"({"category":"violence","cap":"graphic","substitution":"omit"},)"
    R"({"category":"fixture_glow","cap":"bright","substitution":"omit"}]}]})";

}  // namespace

// **An entry records the class its source was decided on** (C64): a glTF whose sidecar names a
// category no data in the scene defines cannot be read, and is decided at the top of every category
// the scene knows at the decision, the game's one a container later in the scene teaches included;
// admitted, its key and the section 54 of the entry the load writes are taken from that class, so
// the entry names the game's category. (Until every ladder was learned before any class was read,
// a sidecar naming only the container's category was unreadable here too.)
TEST_CASE(
    "content classes at load: an unreadable glTF's entry records the class it was decided on") {
  const test::TempDir tmp("renderer_classes_unreadable_record");
  REQUIRE(tmp.ok());
  REQUIRE(io::write_file(tmp.file("tri.gltf"), k_triangle_gltf) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("tri.provenance.json"),
                         R"({"content_classes":{"fixture_glow":"bright","fixture_nobody":"x"}})") ==
          io::Status::Ok);
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
  Grid tri;
  tri.path = tmp.file("tri.gltf");
  SceneDesc desc = scene_of(tmp, {&tri, &glow});
  desc.view_policy = policy_in(k_top_policies, "every-top");
  std::string error;
  SceneData scene;
  REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
  CHECK(scene.instances.size() == 2);
  REQUIRE_FALSE(scene.sources[0].container.empty());
  geometry::ClusterFileData entry;
  REQUIRE_MESSAGE(geometry::read_cluster_file(scene.sources[0].container, entry, &error), error);
  REQUIRE_FALSE(entry.content_classes.empty());
  content_class::Categories table;
  content_class::ContentClasses recorded;
  REQUIRE_MESSAGE(
      content_class::decode_classes(
          table, std::span<const u8>(entry.content_classes.data(), entry.content_classes.size()),
          recorded, error),
      error);
  REQUIRE(table.find("fixture_glow") >= 0);
  CHECK(recorded.levels[static_cast<u32>(table.find("fixture_glow"))] == 2);
  CHECK(recorded.levels[static_cast<u32>(table.find("violence"))] == 2);
  CHECK(scene.held.levels[static_cast<u32>(scene.categories.find("fixture_glow"))] == 2);
}

// **The instance a load makes after a streamed tail goes through the door** (C64): a streamed world
// whose tail takes every file instance leaves the load to make one instance of mesh 0, decided by
// mesh 0's own class as any instance the load makes, so a refused mesh 0 is left out and counted.
TEST_CASE("content classes at load: the mesh-0 instance made after a streamed tail is decided") {
  if (scene_gen::GeneratorRegistry::global().find_placement("authored") == nullptr) {
    MESSAGE("no authored generator in this build");
    return;
  }
  const test::TempDir tmp("renderer_classes_tail");
  REQUIRE(tmp.ok());
  const Grid glow = write_grid(tmp, "glow.clusters", 17, "glow.png", glow_record(1));
  const Grid a = write_grid(tmp, "a.clusters", 9, "a.png");
  const std::string scene_json =
      R"({"format":"engine.scene.v1","meshes":[{"path":"glow.clusters"},{"path":"a.clusters"}],)"
      R"("instances":[{"mesh":1,"translation":[3,0,4],"rotation":[0,0,0,1]},)"
      R"({"mesh":1,"translation":[9,0,2],"rotation":[0,0,0,1]},)"
      R"({"mesh":1,"translation":[-6,0,5],"rotation":[0,0,0,1]}],)"
      R"("world":{"tile_size":32,"rings":[{"radius":1.5},{"radius":4},{"radius":9}]}})";
  REQUIRE(io::write_file(tmp.file("scene.json"), scene_json) == io::Status::Ok);
  std::string error;
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(tmp.file("scene.json"), desc, error), error);
  desc.ddc = tmp.file("ddc");
  REQUIRE_FALSE(desc.stream);
  SceneData scene;
  REQUIRE_MESSAGE(load_scene(desc, scene, error), error);
  CHECK(scene.authored.streamed == 3);
  CHECK(std::none_of(scene.instances.begin(), scene.instances.end(),
                     [](const auto& instance) { return instance.mesh == 0; }));
  REQUIRE(row(scene, "fixture_glow") != nullptr);
  CHECK(row(scene, "fixture_glow")->omitted == 1);
}

// **The props read refuses a key given twice, naming it** (C64), as the scene read does.
TEST_CASE("content classes at load: the props read names a key given twice") {
  const test::TempDir tmp("renderer_classes_props_twice");
  REQUIRE(tmp.ok());
  REQUIRE(io::write_file(tmp.file("scene.json"),
                         R"({"format":"engine.scene.v1","format":"engine.scene.v1"})") ==
          io::Status::Ok);
  Vector<SceneProp> props;
  std::string error;
  CHECK_FALSE(read_scene_props(tmp.file("scene.json"), props, error));
  CHECK_MESSAGE(error.find("'format' is given twice") != std::string::npos, error);
}

namespace {

// A generator whose hash writes `g_swap`'s bytes over its path the first time it is taken: in a
// load, after the containers listed before its recipe were probed and before any is read whole.
struct Swap {
  std::string path;
  std::string bytes;
  bool done = false;
};
Swap* g_swap = nullptr;
bool fixture_swap_hash_with(std::span<const u8> bytes, const assets::MeshGeneratorInputs&, u64& out,
                            std::string& error) {
  if (g_swap != nullptr && !g_swap->done) {
    g_swap->done = true;
    if (io::write_file(g_swap->path, g_swap->bytes) != io::Status::Ok) {
      error = "the fixture could not write its swap";
      return false;
    }
  }
  out = hash_bytes(bytes.data(), bytes.size());
  return true;
}
constexpr assets::MeshGeneratorDesc k_fixture_swap{.name = "fixtureswap",
                                                   .source_hash = fixture_host_hash,
                                                   .build = fixture_host_build,
                                                   .source_hash_with = fixture_swap_hash_with};

}  // namespace

// **A container is decided by the bytes the load read, through the load** (C64): the container is
// probed unclassified, replaced by a classified one while a later recipe's class is read, and read
// whole after; the load decides it by what the whole read found and leaves its instance out. The
// recipe's own instance raises a category nobody defines, so it is refused unread.
TEST_CASE("content classes at load: a container replaced after its probe is decided as read") {
  need_generator(k_fixture_swap);
  const test::TempDir tmp("renderer_classes_swapped_load");
  REQUIRE(tmp.ok());
  const Grid x = write_grid(tmp, "x.clusters", 9, "x.png");
  const Grid next = write_grid(tmp, "x_next.clusters", 9, "x.png", glow_record(2));
  Swap swap{x.path, {}, false};
  REQUIRE(io::read_file(next.path, swap.bytes) == io::Status::Ok);
  REQUIRE(io::write_file(tmp.file("turn.fixtureswap.json"), "{}") == io::Status::Ok);
  const std::string scene_json =
      R"({"meshes":[{"path":"x.clusters"},{"path":"turn.fixtureswap.json"}],"instances":[)"
      R"({"mesh":0,"translation":[0,0,0],"rotation":[0,0,0,1]},)"
      R"({"mesh":1,"translation":[30,0,0],"rotation":[0,0,0,1],)"
      R"("content_classes":{"levels":{"fixture_hum":"loud"}}}]})";
  REQUIRE(io::write_file(tmp.file("scene.json"), scene_json) == io::Status::Ok);
  std::string error;
  SceneDesc desc;
  REQUIRE_MESSAGE(read_scene_file(tmp.file("scene.json"), desc, error), error);
  desc.ddc = tmp.file("ddc");
  g_swap = &swap;
  SceneData scene;
  const bool loaded = load_scene(desc, scene, error);
  g_swap = nullptr;
  REQUIRE_MESSAGE(loaded, error);
  CHECK(swap.done);
  CHECK(scene.instances.empty());
  CHECK(loaded_instance(scene, 0) == k_not_loaded);
  REQUIRE(row(scene, "fixture_glow") != nullptr);
  CHECK(row(scene, "fixture_glow")->omitted == 1);
  CHECK(scene.load.unreadable_omitted == 1);
}
