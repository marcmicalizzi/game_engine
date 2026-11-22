// What the class read at a load reads, counted by kind (C72; renderer.md, "Content classes at
// load"). CPU only, no device.
//
// The scene is the shape of a forest's: thousands of distinct generated-mesh recipes over a
// material pack with tens of sets, some hundreds of distinct containers and glTFs, spread over a
// few directories, with no sidecar anywhere, and the same with a few sidecars. Every instance but
// mesh 0's raises a category nobody defines, so it is left out and its mesh is never read: what
// the load spends is the class read, its `content classes` stage, and the boxes the omitted meshes
// become. Every fixture is neutral data of the test's own.
#include <core/base/types.h>
#include <core/hash/hash.h>
#include <core/profiling/startup_trace.h>
#include <domain/assets/mesh_generator.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>

using namespace engine;
using namespace engine::renderer;
using content_build::ClassRead;

namespace {

void write_text(const std::string& path, const std::string& text) {
  REQUIRE(io::write_file(path, text) == io::Status::Ok);
}

// A pack of `sets` roles: the palm's three and the rest of names of their own, every map one
// stripe image of the test's own.
void make_pack(const test::TempDir& tmp, u32 sets) {
  std::filesystem::create_directories(tmp.native_file("pack"));
  Vector<u8> pixels(32u * 32u * 4u, u8{128});
  Vector<u8> png;
  REQUIRE(image::encode_png(32, 32, 4, std::span<const u8>(pixels.data(), pixels.size()), png));
  REQUIRE(io::write_file(tmp.file("pack/stripe.png"),
                         std::string_view(reinterpret_cast<const char*>(png.data()), png.size())) ==
          io::Status::Ok);
  std::string pack = R"({"format":"engine.materials.pack/1","name":"t","version":"1","roles":{)";
  for (u32 r = 0; r < sets; ++r) {
    const std::string role = r == 0   ? "palm/bark"
                             : r == 1 ? "palm/frond"
                             : r == 2 ? "palm/skirt"
                                      : "set" + std::to_string(r) + "/surface";
    pack +=
        std::string(r > 0 ? "," : "") + "\"" + role +
        R"(":{"tile_m":0.25,"normal_convention":"opengl","maps":{"base_color":{"file":"stripe.png"},)"
        R"("normal":{"file":"stripe.png"},"roughness":{"file":"stripe.png"},)"
        R"("metallic":{"file":"stripe.png"},"height":{"file":"stripe.png"}}})";
  }
  pack += "}}";
  write_text(tmp.file("pack/pack.json"), pack);
}

std::string palm_recipe(u32 seed) {
  return R"({"species":"date_palm","palm":{"seed":)" + std::to_string(seed) +
         R"(,"height_m":5,"frond_count":8,"leaflet_count":12,"dead_skirt_fronds":2,)"
         R"("triangle_budget":6000}})";
}

// A generator of the test's own with no host-aware hash, as deadwood's and the rocks' are: its
// hash never sees a host's hooks, so its recipes can name no host and the class read reads none of
// them (C72). It is never built here: every instance of its recipes is left out.
bool plain_hash(std::span<const u8> bytes, u64& out, std::string&) {
  out = hash_bytes(bytes.data(), bytes.size());
  return true;
}
bool plain_build(std::span<const u8>, const assets::MeshGeneratorInputs&,
                 const geometry::ClusterLodOptions&, u32, geometry::ClusterFileData&,
                 std::string& error) {
  error = "the cost case builds no plain recipe";
  return false;
}
constexpr assets::MeshGeneratorDesc k_plain_generator{
    .name = "costplain", .source_hash = plain_hash, .build = plain_build};
const assets::MeshGeneratorRegistrar k_plain_registrar{k_plain_generator};

// One small grid as a container, at `path`.
void write_grid(const std::string& path) {
  SceneDesc grid;
  grid.meshes.push_back("");
  grid.heightfield_grid = 5;
  grid.cache = false;
  SceneData made;
  std::string error;
  REQUIRE_MESSAGE(load_scene(grid, made, error), error);
  geometry::ClusterFileData file;
  file.mesh = std::move(made.lod);
  file.materials.push_back(geometry::ClusterFileMaterial{});
  file.cluster_material = Vector<u32>(file.mesh.mesh.clusters.size(), 0u);
  REQUIRE_MESSAGE(geometry::write_cluster_file(path, file, &error), error);
}

struct Shape {
  u32 recipes = 0;
  u32 plain = 0;  // recipes of `costplain`, whose hash takes no host
  u32 containers = 0;
  u32 gltfs = 0;
  u32 directories = 1;
  u32 sets = 0;
  u32 sidecar_every = 0;  // 0: no sidecar anywhere; else one beside every n-th source
};

// The raise every omitted instance carries: a category nobody defines, so it fails closed.
content::ContentClasses unknown_raise(const test::TempDir& tmp) {
  write_text(
      tmp.file("raise.json"),
      R"({"meshes":[{"path":"drawn.clusters"}],"instances":[{"mesh":0,"translation":[0,0,0],)"
      R"("rotation":[0,0,0,1],"content_classes":{"levels":{"fixture_hum":"loud"}}}]})");
  SceneDesc desc;
  std::string error;
  REQUIRE_MESSAGE(read_scene_file(tmp.file("raise.json"), desc, error), error);
  REQUIRE(desc.raises.size() == 1);
  return desc.raises[0];
}

// The scene: mesh 0 a plain container its one instance draws, then the recipes, the containers
// and the glTFs, dealt round `directories` directories, each its one instance left out.
SceneDesc cost_scene(const test::TempDir& tmp, const Shape& shape) {
  make_pack(tmp, shape.sets);
  SceneDesc desc;
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  desc.materials = tmp.file("pack");
  write_grid(tmp.file("drawn.clusters"));
  desc.raises.push_back(unknown_raise(tmp));
  desc.meshes.push_back(tmp.file("drawn.clusters"));
  desc.instances.push_back(SceneInstance{0, Transform3::identity()});
  for (u32 d = 0; d < shape.directories; ++d)
    std::filesystem::create_directories(tmp.native_file("d" + std::to_string(d)));
  u32 n = 0;
  const auto add = [&](const std::string& name, const std::string& text, bool copy_grid) {
    const std::string path = tmp.file("d" + std::to_string(n % shape.directories) + "/" + name);
    if (copy_grid)
      std::filesystem::copy_file(tmp.native_file("drawn.clusters"),
                                 std::filesystem::path(path));  // a distinct file, the same bytes
    else
      write_text(path, text);
    if (shape.sidecar_every != 0 && n % shape.sidecar_every == 0)
      write_text(content_class::sidecar_path_of(path), R"({"name":"fixture )" + name + "\"}");
    SceneInstance instance{desc.meshes.size(), Transform3::identity()};
    instance.content_classes = 1;
    desc.instances.push_back(instance);
    desc.meshes.push_back(path);
    ++n;
  };
  for (u32 i = 0; i < shape.recipes; ++i)
    add("tree" + std::to_string(i) + ".flora.json", palm_recipe(100 + i), false);
  for (u32 i = 0; i < shape.plain; ++i)
    add("log" + std::to_string(i) + ".costplain.json", R"({"seed":)" + std::to_string(i) + "}",
        false);
  for (u32 i = 0; i < shape.containers; ++i)
    add("rock" + std::to_string(i) + ".clusters", {}, true);
  for (u32 i = 0; i < shape.gltfs; ++i)
    add("prop" + std::to_string(i) + ".gltf", R"({"asset":{"version":"2.0"}})", false);
  return desc;
}

void report(const char* what, const SceneData& scene, f64 load_ms, f64 cpu_ms) {
  const SceneLoadReport& load = scene.load;
  std::string table = std::string(what) + ": " + std::to_string(load.class_paths) +
                      " distinct paths, the class read " + std::to_string(load.classes_ms) +
                      " ms of a " + std::to_string(load_ms) + " ms load, the thread's CPU " +
                      std::to_string(cpu_ms) + " ms of it\n";
  table += "kind               asked   found      bytes         ms\n";
  for (u32 k = 0; k < content_build::k_class_read_kinds; ++k) {
    const content_build::ClassReadCount& c = load.class_reads.kinds[k];
    char line[160];
    std::snprintf(line, sizeof(line), "%-17s %7u %7u %10llu %10.1f\n",
                  content_build::class_read_name(static_cast<ClassRead>(k)), c.asked, c.found,
                  static_cast<unsigned long long>(c.bytes), static_cast<f64>(c.ns) / 1.0e6);
    table += line;
  }
  MESSAGE(table);
}

// The load's wall milliseconds, and the calling thread's CPU milliseconds over it in `cpu_ms` (the
// class read runs on that thread).
f64 load_ms(const SceneDesc& desc, SceneData& out, f64& cpu_ms) {
  std::string error;
  const i64 cpu_start = profiling::thread_cpu_ns();
  const auto start = std::chrono::steady_clock::now();
  REQUIRE_MESSAGE(load_scene(desc, out, error), error);
  cpu_ms = static_cast<f64>(profiling::thread_cpu_ns() - cpu_start) / 1.0e6;
  return std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

TEST_CASE("content classes at load: the class read's cost on a forest's shape") {
  // The measurement (C72): the table is reported, never asserted on by time; the counts that do
  // not depend on how the reads are made are held.
  for (const u32 sidecar_every : {0u, 97u}) {
    const test::TempDir tmp("renderer_class_read_cost");
    REQUIRE(tmp.ok());
    // Thousands of recipes in a release build, where the table is taken; a fifth of the scene in a
    // debug one, which holds the same counts in the suite's time.
#if defined(NDEBUG)
    constexpr u32 k_scale = 5;
#else
    constexpr u32 k_scale = 1;
#endif
    Shape shape;
    shape.recipes = 400 * k_scale;
    shape.plain = 80 * k_scale;
    shape.containers = 40 * k_scale;
    shape.gltfs = 40 * k_scale;
    shape.directories = 8;
    shape.sets = 40;
    shape.sidecar_every = sidecar_every;
    const SceneDesc desc = cost_scene(tmp, shape);
    SceneData scene;
    f64 cpu_ms = 0.0;
    const f64 ms = load_ms(desc, scene, cpu_ms);
    report(sidecar_every == 0 ? "no sidecar anywhere" : "a sidecar beside every 97th source", scene,
           ms, cpu_ms);
    const SceneLoadReport& load = scene.load;
    CHECK(load.class_paths == 1 + shape.recipes + shape.plain + shape.containers + shape.gltfs);
    // A recipe whose generator has no host-aware hash is never read or probed for its hosts: the
    // answer, none, is known without it (C72).
    CHECK(load.class_reads[ClassRead::recipe_file].asked == shape.recipes);
    CHECK(load.class_reads[ClassRead::recipe_file].found == shape.recipes);
    // The pack's sets read once a load, not once a recipe (C72): every set once.
    CHECK(load.class_reads[ClassRead::pack_sets].asked == shape.sets);
    // Each recipe that can name a host probed once, on whichever thread.
    CHECK(load.class_reads[ClassRead::host_probe].asked == shape.recipes);
    CHECK(load.class_reads[ClassRead::container_section].asked == 1 + shape.containers);
    CHECK(load.class_reads[ClassRead::container_section].found == 1 + shape.containers);
    CHECK(load.unreadable_omitted == shape.recipes + shape.plain + shape.containers + shape.gltfs);
    CHECK(scene.instances.size() == 1);
    const u32 sidecars = sidecar_every == 0 ? 0u : (desc.meshes.size() - 2u) / sidecar_every + 1u;
    const u32 found = load.class_reads[ClassRead::recipe_sidecar].found +
                      load.class_reads[ClassRead::container_sidecar].found +
                      load.class_reads[ClassRead::gltf_sidecar].found;
    CHECK(found == sidecars);
  }
}
