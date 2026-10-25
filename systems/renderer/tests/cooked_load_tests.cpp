// A cooked load reads derived data only (renderer.md, "What the load reads"; roadmap W75): a scene
// whose textures are all built opens no source image — a material pack's files answered from the
// pack's index, every `.tex` trusted from its header — a missing `.tex` is rebuilt once and then
// trusted, two meshes naming one image hash it once, a recipe's own image is still hashed each
// load so a repainted one finds its new texture, and the per-mesh loads on the job system give the
// same tables at one worker as at every one.
#include <core/hash/hash.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>
#include <systems/renderer/scene.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <string>

using namespace engine;
using namespace engine::renderer;

#if ENGINE_RENDERER_TESTS_FLORA

namespace {

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream f(path, std::ios::binary);
  f << text;
  return f.good();
}

// A 32 x 32 map of the test's own, so every image has bytes of its own.
bool write_stripes(const std::string& path, u8 a, u8 b) {
  Vector<u8> pixels(32u * 32u * 4u);
  for (u32 y = 0; y < 32; ++y) {
    for (u32 x = 0; x < 32; ++x) {
      const u8 v = (x / 4u) % 2u == 0 ? a : b;
      u8* p = pixels.data() + (y * 32u + x) * 4u;
      p[0] = v;
      p[1] = static_cast<u8>(v / 2u + 64u);
      p[2] = 128u;
      p[3] = 255u;
    }
  }
  Vector<u8> png;
  if (!image::encode_png(32, 32, 4, std::span<const u8>(pixels.data(), pixels.size()), png))
    return false;
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
  return f.good();
}

// A pack with the palm's three roles, every map a file of its own.
void make_pack(const test::TempDir& tmp) {
  const char* roles[] = {"palm/bark", "palm/frond", "palm/skirt"};
  const char* maps[] = {"base_color", "normal", "roughness", "metallic", "height"};
  std::string pack = R"({"format":"engine.materials.pack/1","name":"t","version":"1","roles":{)";
  for (u32 r = 0; r < 3; ++r) {
    const std::string dir = std::string("pack/") + roles[r];
    std::filesystem::create_directories(tmp.native_file(dir));
    pack += std::string(r > 0 ? "," : "") + "\"" + roles[r] +
            R"(":{"tile_m":0.25,"normal_convention":"opengl","maps":{)";
    for (u32 i = 0; i < 5; ++i) {
      REQUIRE(write_stripes(tmp.file(dir + "/" + maps[i] + ".png"),
                            static_cast<u8>(10 + 30 * i + r), static_cast<u8>(20 + 30 * i)));
      pack += std::string(i > 0 ? "," : "") + "\"" + maps[i] + "\":{\"file\":\"" +
              std::string(roles[r]) + "/" + maps[i] + ".png\"}";
    }
    pack += "}}";
  }
  pack += "}}";
  REQUIRE(write_text(tmp.file("pack/pack.json"), pack));
}

std::string palm_recipe(u32 seed, const std::string& extra = "") {
  return R"({"species":"date_palm","palm":{"seed":)" + std::to_string(seed) +
         R"(,"height_m":5,"frond_count":8,"leaflet_count":12,"dead_skirt_fronds":2,)"
         R"("triangle_budget":6000})" +
         extra + "}";
}

SceneDesc scene_of(const test::TempDir& tmp, std::initializer_list<const char*> meshes) {
  SceneDesc desc;
  for (const char* mesh : meshes)
    desc.meshes.push_back(tmp.file(mesh));
  desc.ddc = tmp.file("ddc");
  desc.cache = true;
  desc.materials = tmp.file("pack");
  return desc;
}

// What the GPU upload reads from a loaded scene, hashed: the merged DAG, the parts, the instances
// and every mesh's material map and texture records.
u64 tables_hash(const SceneData& data) {
  auto bytes = [](const auto& v) { return hash_bytes(v.data(), v.size() * sizeof(*v.data())); };
  u64 h = bytes(data.lod.mesh.clusters);
  h = hash_combine(h, bytes(data.lod.mesh.vertices));
  h = hash_combine(h, bytes(data.lod.mesh.triangles));
  h = hash_combine(h, bytes(data.lod.lod));
  h = hash_combine(h, bytes(data.instances));
  for (const geometry::ClusterMeshPart& part : data.parts) {
    h = hash_combine(h, part.first_cluster);
    h = hash_combine(h, part.cluster_count);
    h = hash_combine(h, part.first_vertex);
  }
  for (const SourceMesh& source : data.sources) {
    h = hash_combine(h, bytes(source.part_of_cluster));
    h = hash_combine(h, bytes(source.part_material));
    h = hash_combine(h, bytes(source.textures));
    for (const assets::ImageRef& image : source.data.images)
      h = hash_combine(h, hash_bytes(image.uri.data(), image.uri.size()));
  }
  return h;
}

}  // namespace

TEST_CASE("cooked load: a scene whose textures are built reads no source image") {
  const test::TempDir tmp("renderer_cooked_load");
  REQUIRE(tmp.ok());
  make_pack(tmp);
  REQUIRE(write_text(tmp.file("a.flora.json"), palm_recipe(9)));
  REQUIRE(write_text(tmp.file("b.flora.json"), palm_recipe(10)));
  std::string error;

  // Cold: everything built, every pack file the palms sample read and hashed once — however many
  // meshes name it.
  SceneData cold;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {"a.flora.json", "b.flora.json"}), cold, error), error);
  const SceneLoadReport& c = cold.load;
  REQUIRE(c.references > 0);
  CHECK(c.textures * 2 == c.references);  // the two palms sample the same images
  CHECK(c.built == c.textures);
  CHECK(c.trusted == 0);
  CHECK(c.images_hashed == c.textures);  // one hash per distinct image, not one per mesh
  CHECK(c.unbuilt == 0);

  // Warm: no source image opened, every texture trusted, nothing built.
  SceneData warm;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {"a.flora.json", "b.flora.json"}), warm, error), error);
  const SceneLoadReport& w = warm.load;
  CHECK(w.source_image_reads == 0);
  CHECK(w.source_image_bytes == 0);
  CHECK(w.images_hashed == 0);
  CHECK(w.images_from_index == c.textures);
  CHECK(w.trusted == c.textures);
  CHECK(w.built == 0);
  // The same keys the cold load built, in every mesh's records: what the upload looks up.
  CHECK(tables_hash(warm) == tables_hash(cold));

  // A missing `.tex` is built once, from the one file it needs, and then trusted.
  const std::string textures = tmp.file("ddc/textures");
  std::string removed;
  for (const auto& entry : std::filesystem::directory_iterator(textures)) {
    if (entry.path().extension() == ".tex") {
      removed = entry.path().string();
      break;
    }
  }
  REQUIRE_FALSE(removed.empty());
  std::filesystem::remove(removed);
  SceneData repaired;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {"a.flora.json", "b.flora.json"}), repaired, error),
                  error);
  CHECK(repaired.load.built == 1);
  CHECK(repaired.load.trusted == c.textures - 1);
  CHECK(repaired.load.source_image_reads == 1);
  CHECK(std::filesystem::exists(removed));
  SceneData after;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {"a.flora.json", "b.flora.json"}), after, error), error);
  CHECK(after.load.built == 0);
  CHECK(after.load.source_image_reads == 0);

  // A torn `.tex` (one byte short of what its header says) is not trusted: rebuilt the same way.
  {
    std::string bytes;
    REQUIRE(io::read_file(removed, bytes) == io::Status::Ok);
    bytes.pop_back();
    REQUIRE(io::write_file(removed, bytes) == io::Status::Ok);
  }
  SceneData torn;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {"a.flora.json", "b.flora.json"}), torn, error), error);
  CHECK(torn.load.built == 1);
}

TEST_CASE("cooked load: a recipe's own image is hashed each load, so a repainted one is rebuilt") {
  const test::TempDir tmp("renderer_cooked_recipe");
  REQUIRE(tmp.ok());
  make_pack(tmp);
  std::filesystem::create_directories(tmp.native_file("sets"));
  REQUIRE(write_stripes(tmp.file("sets/bark_c.png"), 90, 140));
  REQUIRE(write_text(tmp.file("own.flora.json"),
                     palm_recipe(9, R"(,"materials":{"bark":{"base_color":"sets/bark_c.png",)"
                                    R"("tile_m":0.5}})")));
  std::string error;
  SceneData first;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {"own.flora.json"}), first, error), error);
  SceneData second;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {"own.flora.json"}), second, error), error);
  // The recipe's file is covered by nothing the load reads, so it is read (once) every load; the
  // pack's files are not.
  CHECK(second.load.source_image_reads == 1);
  CHECK(second.load.images_hashed == 1);
  CHECK(second.load.built == 0);
  REQUIRE(write_stripes(tmp.file("sets/bark_c.png"), 30, 220));  // repainted
  SceneData third;
  REQUIRE_MESSAGE(load_scene(scene_of(tmp, {"own.flora.json"}), third, error), error);
  CHECK(third.load.built == 1);
  CHECK(tables_hash(third) != tables_hash(second));  // a new key in the bark's record
}

TEST_CASE("cooked load: the meshes load on the job system into the tables one worker gives") {
  const test::TempDir tmp("renderer_cooked_jobs");
  REQUIRE(tmp.ok());
  make_pack(tmp);
  REQUIRE(write_text(tmp.file("a.flora.json"), palm_recipe(9)));
  REQUIRE(write_text(tmp.file("b.flora.json"), palm_recipe(10)));
  REQUIRE(write_text(tmp.file("c.flora.json"), palm_recipe(11)));
  std::string error;
  // Built cold at every worker, read at one, read at every one, and a repeated path: one table.
  SceneDesc desc = scene_of(tmp, {"a.flora.json", "b.flora.json", "c.flora.json", "a.flora.json"});
  SceneData cold;
  REQUIRE_MESSAGE(load_scene(desc, cold, error), error);
  CHECK(cold.load.meshes == 4);
  CHECK(std::string(cold.sources[3].cache) == "hit");  // the repeat waited for the first's entry
  desc.load_jobs = 1;
  SceneData one;
  REQUIRE_MESSAGE(load_scene(desc, one, error), error);
  desc.load_jobs = 0;
  SceneData every;
  REQUIRE_MESSAGE(load_scene(desc, every, error), error);
  CHECK(every.load.threads >= one.load.threads);
  CHECK(tables_hash(one) == tables_hash(every));
  CHECK(tables_hash(one) == tables_hash(cold));
  // The first failure is the lowest mesh's, whatever finished first.
  REQUIRE(write_text(tmp.file("bad1.flora.json"),
                     R"({"species":"date_palm","palm":{"frond_count":0}})"));
  REQUIRE(write_text(tmp.file("bad2.flora.json"), R"({"species":"nothing"})"));
  SceneData refused;
  CHECK_FALSE(load_scene(scene_of(tmp, {"a.flora.json", "bad1.flora.json", "bad2.flora.json"}),
                         refused, error));
  CHECK(error.find("frond_count") != std::string::npos);
}

#endif  // ENGINE_RENDERER_TESTS_FLORA
