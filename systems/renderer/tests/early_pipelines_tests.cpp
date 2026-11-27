// The resolve's pipeline started when the scene file has been read (roadmap R205, second leg;
// docs/subsystems/renderer.md, "The resolve compiles from the scene file"). What must hold, and is
// held here by counts, never by a clock: the pipeline a scene file names is compiled once, early,
// and the renderer's creation compiles none of it; a variant the file did not name costs one more
// compile and never a wrong pipeline; and the renderer that took the early pipeline draws the bytes
// one that made its own draws. Each device has a pipeline cache of the case's own, so the counts
// are the device's creations and not another run's cache.

#include "test_device.h"

#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/assets/gltf.h>
#include <domain/geometry/cluster_file.h>
#include <domain/gfx/device.h>
#include <domain/gfx/pipeline_batch.h>
#include <foundation/io/vfs.h>
#include <foundation/tunables/tunables.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/early_pipelines.h>
#include <systems/renderer/gpu_scene.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/settings.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <string>
#include <utility>

using namespace engine;
using namespace engine::renderer;

namespace {

constexpr u32 k_width = 256;
constexpr u32 k_height = 144;

struct Gpu {
  test::TempDir tmp{"renderer_early_pipelines"};
  gfx::Device device;
  std::string why;
  bool ok = false;
  Gpu() {
    gfx::DeviceOptions options = renderer_test::device_options();
    options.pipeline_cache_dir = tmp.file("pipeline-cache");
    std::string error;
    if (!device.create(options, &error)) {
      why = "no Vulkan device: " + error;
      return;
    }
    if (!device.features().buffer_int64_atomics) {
      why = std::string(device.adapter().name) + " has no 64-bit buffer atomics";
      return;
    }
    ok = true;
  }
};

// The renderer's own heightfield: a scene file's description that names no water, no layered
// ground, no generated mesh and no world, so every flag of its variant is the file's to say.
SceneDesc hills() {
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.heightfield_grid = 65;
  desc.cache = false;
  return desc;
}

FrameDesc looking() {
  FrameDesc frame;
  const Vec3 eye{0.0f, 3.0f, 0.0f};
  const Vec3 towards = normalize(Vec3{0.35f, -0.12f, -1.0f});
  frame.camera.position = absolute(WorldPos::origin(), eye);
  frame.camera.target = absolute(WorldPos::origin(), eye + towards * 100.0f);
  frame.camera.fov_y = 60.0f * 3.14159265f / 180.0f;
  frame.camera.znear = 0.05f;
  frame.dt_s = 1.0 / 60.0;
  return frame;
}

// The device's creations of the resolve's one-draw pipeline from `module`: every compile, early
// (on the worker, outside any batch) or at the renderer's creation (in its batch).
struct Compiles {
  u32 early = 0;
  u32 at_creation = 0;
};
Compiles resolve_compiles(const gfx::Device& device, const std::string& module) {
  Vector<gfx::PipelineCreation> all;
  gfx::pipeline_creations(device, all);
  Compiles out;
  for (const gfx::PipelineCreation& c : all) {
    if (c.shader != module || c.entry != "vs_fullscreen/fs_resolve") continue;
    ++(c.batched ? out.at_creation : out.early);
  }
  return out;
}

struct Rig {
  SceneData data;
  ResolvedSettings resolved;
  GpuScene scene;
  std::string error;

  bool load(const gfx::Device& device) {
    if (!load_scene(hills(), data, error)) return false;
    resolve_settings(RenderSettings{}, device.features(), &data, resolved);
    if (check_availability(resolved, device.features()) != RenderAvailability::Ok) {
      error = "unavailable";
      return false;
    }
    return scene.create(device, data, resolved, &error);
  }
  bool render(const gfx::Device& device, EarlyPipelines* early, CapturedFrame& out) {
    SceneRenderer renderer;
    SceneRenderer::Desc rd;
    rd.width = k_width;
    rd.height = k_height;
    rd.early = early;
    if (!renderer.create(device, scene, resolved, rd, &error)) return false;
    CaptureChannels channels;
    channels.color = true;
    channels.ids = true;
    return renderer.capture(looking(), channels, out, &error);
  }
};

// The variant the file names, under the settings resolved before the scene was read.
ResolveVariant named_variant(const gfx::Device& device) {
  ResolvedSettings early;
  resolve_settings(RenderSettings{}, device.features(), nullptr, early);
  return predict_resolve_variant(hills(), early, gfx::Format::R8G8B8A8Unorm);
}

// A glTF's JSON with a plain material and one whose `extensions` are `extensions`.
std::string gltf_text(const char* extensions) {
  return std::string(R"({"asset": {"version": "2.0"}, "materials": [{}, )") + R"({"extensions": )" +
         extensions + "}]}";
}

// A `.glb` of one JSON chunk (padded to four bytes, as the format asks).
std::string glb_bytes(std::string json) {
  while (json.size() % 4 != 0)
    json.push_back(' ');
  const u32 head[5] = {0x46546C67u, 2u, static_cast<u32>(20 + json.size()),
                       static_cast<u32>(json.size()), 0x4E4F534Au};
  std::string out(reinterpret_cast<const char*>(head), sizeof(head));
  return out + json;
}

// A container of one section, 55, holding `record`: what the reader reads, and nothing else.
std::string clusters_bytes(const geometry::ClusterFileMaterialSpecular& record,
                           u32 version = geometry::k_cluster_file_version) {
  geometry::ClusterFileHeader header;
  header.version = version;
  header.section_count = 1;
  geometry::ClusterFileSection section;
  section.kind = static_cast<u32>(geometry::ClusterSection::MaterialSpecular);
  section.element_size = sizeof(record);
  section.element_count = 1;
  section.offset = 64;  // past the header and the table, on the format's alignment
  std::string out(64 + sizeof(record), '\0');
  std::memcpy(out.data(), &header, sizeof(header));
  std::memcpy(out.data() + sizeof(header), &section, sizeof(section));
  std::memcpy(out.data() + 64, &record, sizeof(record));
  header.total_bytes = out.size();
  std::memcpy(out.data(), &header, sizeof(header));
  return out;
}

// What the worker would read of a scene's mesh files: the same function, the same list.
MeshDeclarations declares(const SceneDesc& desc) {
  MeshDeclarations d;
  for (const std::string& path : mesh_files_to_read(desc))
    (void)read_mesh_declarations(path, d);
  return d;
}

}  // namespace

TEST_CASE("renderer: the resolve a scene file names compiles once, early, and is the one drawn") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  const ResolveVariant variant = named_variant(gpu.device);
  CHECK_FALSE(variant.water);
  CHECK_FALSE(variant.terrain);
  CHECK_FALSE(variant.stochastic);
  CHECK_FALSE(variant.foliage);

  // Two mesh files that declare nothing, handed to the worker: it reads both (only a declared one
  // stops the loop early) and the key stays the scene's.
  Vector<std::string> files;
  files.push_back(gpu.tmp.file("plain.gltf"));
  files.push_back(gpu.tmp.file("plain.glb"));
  REQUIRE(io::write_file(files[0], gltf_text("{}")) == io::Status::Ok);
  REQUIRE(io::write_file(files[1], glb_bytes(gltf_text("{}"))) == io::Status::Ok);

  EarlyPipelines early;
  std::string error;
  REQUIRE_MESSAGE(early.start(gpu.device, variant, std::string(), &error, files), error);
  const std::string module = early.stats().key.module;
  CHECK(module == (variant.shadows ? "visibility_resolve_rt" : "visibility_resolve"));

  Rig rig;
  REQUIRE_MESSAGE(rig.load(gpu.device), rig.error);
  CapturedFrame taken;
  REQUIRE_MESSAGE(rig.render(gpu.device, &early, taken), rig.error);
  const EarlyPipelines::Stats stats = early.stats();
  CHECK(stats.started);
  CHECK(stats.done);
  CHECK(stats.ok);
  CHECK(stats.taken);
  CHECK(stats.mismatched == 0);
  CHECK(stats.read);
  CHECK(stats.mesh_files == 2);
  CHECK(stats.mesh_files_read == 2);
  CHECK_FALSE(stats.key.specular);
  // One compile for one key: the worker's, and none at the renderer's creation.
  const Compiles with_early = resolve_compiles(gpu.device, module);
  CHECK(with_early.early == 1);
  CHECK(with_early.at_creation == 0);
  // The prediction is what the GPU scene decided, flag for flag.
  const GpuScene& scene = rig.scene;
  CHECK(variant.water == scene.has_water());
  CHECK(variant.terrain == scene.has_terrain_materials());
  CHECK(variant.stochastic == scene.has_stochastic_materials());
  CHECK(variant.foliage == scene.has_foliage_materials());
  CHECK(variant.specular == scene.has_specular_materials());
  CHECK_FALSE(scene.has_specular_materials());  // R234: the hills declare no reflectance
  // One shape of the bindless set (M1): the early layout's capacity is the scene's, and it is in
  // the key, so a set of another shape is another key and never takes this pipeline.
  CHECK(stats.key.layout == scene.bindless().capacity());
  CHECK(stats.key.layout.sampled_images <= GpuScene::bindless_config().sampled_images);
  ResolveKey other = stats.key;
  other.layout.sampled_images = stats.key.layout.sampled_images / 2;
  CHECK_FALSE(other == stats.key);

  // A second renderer handed the same object after the take (a shader reload's re-create) makes
  // its own, at creation, draws the same bytes, and is not counted as a wrong guess.
  CapturedFrame made;
  REQUIRE_MESSAGE(rig.render(gpu.device, &early, made), rig.error);
  CHECK(early.stats().mismatched == 0);
  const Compiles both = resolve_compiles(gpu.device, module);
  CHECK(both.early == 1);
  CHECK(both.at_creation == 1);
  REQUIRE(made.color.size() == taken.color.size());
  CHECK(std::memcmp(made.color.data(), taken.color.data(), made.color.size()) == 0);
  REQUIRE(made.ids.size() == taken.ids.size());
  CHECK(std::memcmp(made.ids.data(), taken.ids.data(), made.ids.size() * sizeof(u32)) == 0);
}

TEST_CASE("renderer: a variant the scene did not name costs one more compile, not a wrong one") {
  Gpu gpu;
  if (!gpu.ok) {
    MESSAGE("skipped: " << gpu.why);
    return;
  }
  // The guess says foliage where the scene has none: another key (the specialization), so the
  // renderer makes its own pipeline and the early one is destroyed untaken.
  ResolveVariant wrong = named_variant(gpu.device);
  wrong.foliage = !wrong.foliage;
  EarlyPipelines early;
  std::string error;
  REQUIRE_MESSAGE(early.start(gpu.device, wrong, std::string(), &error), error);
  const std::string module = early.stats().key.module;

  Rig rig;
  REQUIRE_MESSAGE(rig.load(gpu.device), rig.error);
  CapturedFrame drawn;
  REQUIRE_MESSAGE(rig.render(gpu.device, &early, drawn), rig.error);
  // An untaken compile runs to its end on its worker (a driver's compile cannot be called off):
  // `destroy` waits for it and destroys its pipeline, the device still open, before it is counted.
  early.destroy();
  const EarlyPipelines::Stats stats = early.stats();
  CHECK(stats.started);
  CHECK(stats.done);
  CHECK_FALSE(stats.taken);
  CHECK(stats.mismatched == 1);
  CHECK(stats.reason.find("foliage") != std::string::npos);
  CHECK(stats.reason.find("module") == std::string::npos);
  const Compiles compiles = resolve_compiles(gpu.device, module);
  CHECK(compiles.early == 1);
  CHECK(compiles.at_creation == 1);

  // A start is refused where the split's pipelines would be made instead: those are other keys.
  EarlyPipelines refused;
  tunables::Tunable* classes = tunables::find("renderer.resolve.classes");
  REQUIRE(classes != nullptr);
  std::string previous;
  classes->append_value(previous);
  REQUIRE(classes->set_from_text("draws"));
  CHECK_FALSE(refused.start(gpu.device, named_variant(gpu.device), std::string(), &error));
  CHECK_FALSE(refused.stats().started);
  CHECK(refused.stats().reason == error);
  (void)classes->set_from_text(previous);

  // A mesh file that declares a reflectance the scene's load does not have: the worker's read
  // joins the key (`specular`), the loop stops at the first declared file, and the renderer's
  // other key is one more compile, never a wrong pipeline.
  Vector<std::string> files;
  files.push_back(gpu.tmp.file("declared.gltf"));
  files.push_back(gpu.tmp.file("after.gltf"));
  REQUIRE(io::write_file(files[0], gltf_text(R"({"KHR_materials_ior": {"ior": 2.0}})")) ==
          io::Status::Ok);
  REQUIRE(io::write_file(files[1], gltf_text("{}")) == io::Status::Ok);
  EarlyPipelines read;
  REQUIRE_MESSAGE(read.start(gpu.device, named_variant(gpu.device), std::string(), &error, files),
                  error);
  Rig again;
  REQUIRE_MESSAGE(again.load(gpu.device), again.error);
  CapturedFrame redrawn;
  REQUIRE_MESSAGE(again.render(gpu.device, &read, redrawn), again.error);
  read.destroy();
  CHECK(read.stats().mesh_files_read == 1);
  CHECK(read.stats().key.specular);
  CHECK(read.stats().mismatched == 1);
  CHECK(read.stats().reason.find("specular") != std::string::npos);
  REQUIRE(redrawn.color.size() == drawn.color.size());
  CHECK(std::memcmp(redrawn.color.data(), drawn.color.data(), drawn.color.size()) == 0);
}

// The prediction's rules off any device: each flag the file can name, read as the GPU scene's own
// rule reads it (M2 of R205's second leg). Against the GPU scene's flags for the hills above, and
// for a water body in the water suite (water_scene_tests.cpp); the scenes that set a layered
// ground, stochastic and foliage by content are owed a case of their own (renderer.md, "The
// resolve compiles from the scene file").
TEST_CASE("renderer: the resolve's prediction reads the file by the renderer's own rules") {
  ResolvedSettings resolved;
  const auto predict = [&resolved](const SceneDesc& desc) {
    return predict_resolve_variant(desc, resolved, gfx::Format::R8G8B8A8Unorm);
  };
  const test::TempDir tmp{"renderer_early_prediction"};
  SceneDesc base = hills();
  base.materials = tmp.file("no-pack");  // no pack: the recipes' own words only
  CHECK(predict(base) == ResolveVariant{});

  // A streamed world whose tiles hand meshes: the mesh room, by `GpuScene::wants_mesh_room`.
  SceneDesc world = base;
  world.world.enabled = true;
  world.streamed.emplace_back();
  world.streamed.back().tile_meshes = true;
  CHECK(predict(world).foliage ==
        GpuScene::wants_mesh_room(world_is_dynamic(world.world, true), false, true));
  CHECK(predict(world).foliage);
  resolved.stream = true;  // paged streaming has no mesh room
  CHECK_FALSE(predict(world).foliage);
  resolved.stream = false;
  world.streamed.back().tile_meshes = false;
  CHECK_FALSE(predict(world).foliage);

  // A recipe's `material.sampling` is read as the field it is, never matched as text.
  SceneDesc rock = base;
  rock.recipes.push_back(MeshRecipe{"rock", R"({"material": {"sampling": "Stochastic"}})"});
  CHECK(predict(rock).stochastic);
  rock.recipes.back().bytes = R"({"material": {"sampling": "Tiled"}, "stochastic": false})";
  CHECK_FALSE(predict(rock).stochastic);
  rock.recipes.back().bytes = R"({"parts": [{"material": {"sampling": "Stochastic"}}]})";
  CHECK(predict(rock).stochastic);

  // A scene `Mesh.sampling` that asks (T150) is read; one that says `Tiled` is not a request.
  SceneDesc asked = base;
  asked.mesh_info.resize(asked.meshes.size());
  REQUIRE_FALSE(asked.mesh_info.empty());
  asked.mesh_info[0].sampling.push_back({-1, false});
  CHECK_FALSE(predict(asked).stochastic);
  asked.mesh_info[0].sampling.push_back({0, true});
  CHECK(predict(asked).stochastic);
  // In order, by the load's own rule (`apply_mesh_sampling`), not as a set: `[{}, {"sampling":
  // "Tiled"}]` asks and then takes it back, and loads plain, so it predicts plain.
  asked.mesh_info[0].sampling.clear();
  asked.mesh_info[0].sampling.push_back({-1, true});
  asked.mesh_info[0].sampling.push_back({-1, false});
  CHECK_FALSE(predict(asked).stochastic);
  asked.mesh_info[0].sampling.push_back({0, true});  // and one material asked again
  CHECK(predict(asked).stochastic);
  // An entry the load refuses predicts nothing.
  asked.mesh_info[0].sampling.clear();
  asked.mesh_info[0].sampling.push_back({-2, true});
  CHECK_FALSE(predict(asked).stochastic);
  // A declared reflectance (R234): a generator's `tile_meshes_specular` under the mesh room, as
  // the GPU scene reads it, and a mesh file whose material packs one — by the record's packer, so
  // the defaults stated are no declaration.
  SceneDesc told = world;
  told.streamed.back().tile_meshes = true;
  told.streamed.back().tile_meshes_specular = true;
  CHECK(predict(told).specular);
  CHECK_FALSE(predict(world).specular);
  resolved.stream = true;
  CHECK_FALSE(predict(told).specular);
  resolved.stream = false;
  // The mesh files are not the prediction's to read (the early compile's worker reads them): a
  // file that declares one leaves the prediction as it was, and the prediction opens nothing.
  const auto gltf = [&](const char* name, const char* extensions) {
    const std::string path = tmp.file(name);
    REQUIRE(io::write_file(path, gltf_text(extensions)) == io::Status::Ok);
    SceneDesc desc = base;
    desc.meshes.push_back(path);
    const u64 reads = mesh_declaration_reads();
    CHECK_FALSE(predict(desc).specular);
    CHECK(mesh_declaration_reads() == reads);
    return declares(desc).specular;
  };
  CHECK(gltf("ior.gltf", R"({"KHR_materials_ior": {"ior": 2.0}})"));
  CHECK(gltf("specular.gltf", R"({"KHR_materials_specular": {"specularFactor": 0.5}})"));
  CHECK_FALSE(gltf("defaults.gltf", R"({"KHR_materials_ior": {"ior": 1.5}, )"
                                    R"("KHR_materials_specular": {}})"));
  CHECK_FALSE(gltf("none.gltf", R"({})"));
  SceneDesc missing = base;
  missing.meshes.push_back(tmp.file("absent.gltf"));
  CHECK_FALSE(declares(missing).specular);
}

// The worker's reads, off any device (R234's mend): each kind of mesh file by its suffix in any
// case, a truncated file of each, a `.glb` whose lengths claim more than it holds, a container of
// another version, and a recipe's path never touched — every refusal declaring nothing.
TEST_CASE("renderer: the mesh files' declarations are read by suffix and within their size") {
  const test::TempDir tmp{"renderer_early_mesh_reads"};
  const std::string declared_json = gltf_text(R"({"KHR_materials_ior": {"ior": 2.0}})");
  const std::string plain_json = gltf_text(R"({})");
  const auto read = [](const std::string& path) {
    MeshDeclarations d;
    const u64 before = mesh_declaration_reads();
    const bool opened = read_mesh_declarations(path, d);
    CHECK(mesh_declaration_reads() - before == (opened ? 1u : 0u));
    return std::pair<bool, bool>{opened, d.specular};
  };
  const auto write = [&tmp](const char* name, const std::string& bytes) {
    const std::string path = tmp.file(name);
    REQUIRE(io::write_file(path, bytes) == io::Status::Ok);
    return path;
  };

  // A `.glb`: its JSON chunk, by an upper-case suffix as well.
  CHECK(read(write("declared.glb", glb_bytes(declared_json))) == std::pair{true, true});
  CHECK(read(write("upper.GLB", glb_bytes(declared_json))) == std::pair{true, true});
  CHECK(read(write("plain.glb", glb_bytes(plain_json))) == std::pair{true, false});
  CHECK(read(write("Upper.GlTf", declared_json)) == std::pair{true, true});
  // Truncated: the chunk is cut short, and a 30-byte file claims a 64 MiB chunk; neither is read
  // past its size (nothing is allocated for a length the file does not hold).
  const std::string whole = glb_bytes(declared_json);
  CHECK(read(write("cut.glb", whole.substr(0, whole.size() - 7))) == std::pair{true, false});
  std::string claims = whole.substr(0, 30);
  const u32 huge = 64u << 20;
  std::memcpy(claims.data() + 12, &huge, sizeof(huge));
  CHECK(read(write("claims.glb", claims)) == std::pair{true, false});

  // A container's section 55, at this build's version only.
  geometry::ClusterFileMaterialSpecular stated;
  stated.extensions = assets::k_ext_ior;
  stated.ior = 2.0f;
  CHECK(read(write("declared.clusters", clusters_bytes(stated))) == std::pair{true, true});
  CHECK(read(write("upper.CLUSTERS", clusters_bytes(stated))) == std::pair{true, true});
  geometry::ClusterFileMaterialSpecular defaults;
  defaults.extensions = assets::k_ext_ior;  // stated, at the default: no declaration
  CHECK(read(write("defaults.clusters", clusters_bytes(defaults))) == std::pair{true, false});
  const std::string container = clusters_bytes(stated);
  CHECK(read(write("cut.clusters", container.substr(0, container.size() - 4))) ==
        std::pair{true, false});
  CHECK(read(write("headless.clusters", container.substr(0, 20))) == std::pair{true, false});
  CHECK(read(write("other.clusters", clusters_bytes(stated, geometry::k_cluster_file_version +
                                                                1))) == std::pair{true, false});

  // Neither a suffix it does not know nor a missing file is opened.
  CHECK(read(write("declared.json", declared_json)) == std::pair{false, false});
  CHECK(read(tmp.file("absent.glb")) == std::pair{false, false});

  // The files the worker is handed: by suffix, less the procedural path and every recipe's path,
  // decided without the disk (the recipe's path need not exist).
  SceneDesc desc;
  desc.meshes.push_back("");
  desc.meshes.push_back(tmp.file("declared.glb"));
  desc.meshes.push_back(tmp.file("handed.glb"));
  desc.meshes.push_back(tmp.file("declared.json"));
  desc.meshes.push_back(tmp.file("upper.CLUSTERS"));
  desc.recipes.push_back(MeshRecipe{tmp.file("handed.glb"), "{}"});
  const u64 before = mesh_declaration_reads();
  const Vector<std::string> files = mesh_files_to_read(desc);
  CHECK(mesh_declaration_reads() == before);
  REQUIRE(files.size() == 2);
  CHECK(files[0] == tmp.file("declared.glb"));
  CHECK(files[1] == tmp.file("upper.CLUSTERS"));
}

// A refused start reads none of its files (R234's mend): the reads are the worker's, and a start
// refused before any device call never makes one. Off any device: the refusal comes first.
TEST_CASE("renderer: a refused early compile reads no mesh file") {
  const test::TempDir tmp{"renderer_early_refused_reads"};
  const std::string path = tmp.file("declared.gltf");
  REQUIRE(io::write_file(path, gltf_text(R"({"KHR_materials_ior": {"ior": 2.0}})")) ==
          io::Status::Ok);
  SceneDesc desc = hills();
  desc.meshes.push_back(path);
  tunables::Tunable* classes = tunables::find("renderer.resolve.classes");
  REQUIRE(classes != nullptr);
  std::string previous;
  classes->append_value(previous);
  REQUIRE(classes->set_from_text("draws"));
  const gfx::Device never;  // never created: the refusal comes before any use of it
  EarlyPipelines refused;
  std::string error;
  const u64 before = mesh_declaration_reads();
  CHECK_FALSE(
      refused.start(never, ResolveVariant{}, std::string(), &error, mesh_files_to_read(desc)));
  CHECK(mesh_declaration_reads() == before);
  CHECK_FALSE(refused.stats().started);
  CHECK(refused.stats().mesh_files_read == 0);
  (void)classes->set_from_text(previous);
}
