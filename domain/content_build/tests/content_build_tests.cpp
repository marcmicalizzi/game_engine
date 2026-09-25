// The content build as a library. engine-content's end-to-end suite (apps/engine_content/tests)
// is this module's real test — it drives every rule, the cache, the manifest and the metrics
// through the command line, byte for byte — so these cases pin what a second host relies on: the
// build key's two moves, the spellings, a manifest's resolution, the identity skip and `force`,
// the metrics of a container, and the protocol's option defaults agreeing with the build's.
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/content_build/container_stats.h>
#include <domain/content_build/content_build.h>
#include <domain/geometry/cluster_file.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <fstream>
#include <schemas/protocol.h>
#include <string>
#include <vector>

using namespace engine;

namespace {

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((bits >> (8 * i)) & 0xffu));
}

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

// A flat n x n grid of quads as a .gltf and its .bin: positions, normals, UVs and 32-bit indices,
// one material. Enough triangles at n = 16 (512) for several clusters and a coarser level.
bool write_grid(const std::string& dir, const std::string& stem, u32 n, f32 height = 0.0f) {
  std::vector<u8> bin;
  const u32 side = n + 1;
  for (u32 j = 0; j < side; ++j) {
    for (u32 i = 0; i < side; ++i) {
      put_f32(bin, static_cast<f32>(i));
      put_f32(bin, (i + j) % 2 == 0 ? height : 0.0f);
      put_f32(bin, static_cast<f32>(j));
    }
  }
  const u32 normals = static_cast<u32>(bin.size());
  for (u32 v = 0; v < side * side; ++v) {
    put_f32(bin, 0.0f);
    put_f32(bin, 1.0f);
    put_f32(bin, 0.0f);
  }
  const u32 uvs = static_cast<u32>(bin.size());
  for (u32 j = 0; j < side; ++j) {
    for (u32 i = 0; i < side; ++i) {
      put_f32(bin, static_cast<f32>(i) / static_cast<f32>(n));
      put_f32(bin, static_cast<f32>(j) / static_cast<f32>(n));
    }
  }
  const u32 indices = static_cast<u32>(bin.size());
  for (u32 j = 0; j < n; ++j) {
    for (u32 i = 0; i < n; ++i) {
      const u32 a = j * side + i;
      const u32 b = (j + 1) * side + i;
      const u32 c = j * side + i + 1;
      const u32 d = (j + 1) * side + i + 1;
      for (const u32 v : {a, b, c, c, b, d})
        put_u32(bin, v);
    }
  }
  const u32 total = static_cast<u32>(bin.size());
  const u32 vertex_count = side * side;
  const u32 index_count = n * n * 6;
  const std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"" + stem +
      ".bin\",\"byteLength\":" + std::to_string(total) +
      "}],\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      std::to_string(normals) + "},{\"buffer\":0,\"byteOffset\":" + std::to_string(normals) +
      ",\"byteLength\":" + std::to_string(uvs - normals) +
      "},{\"buffer\":0,\"byteOffset\":" + std::to_string(uvs) +
      ",\"byteLength\":" + std::to_string(indices - uvs) +
      "},{\"buffer\":0,\"byteOffset\":" + std::to_string(indices) +
      ",\"byteLength\":" + std::to_string(total - indices) +
      "}],\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":" +
      std::to_string(vertex_count) + ",\"type\":\"VEC3\",\"min\":[0,0,0],\"max\":[" +
      std::to_string(n) + "," + std::to_string(height) + "," + std::to_string(n) +
      "]},{\"bufferView\":1,\"componentType\":5126,\"count\":" + std::to_string(vertex_count) +
      ",\"type\":\"VEC3\"},{\"bufferView\":2,\"componentType\":5126,\"count\":" +
      std::to_string(vertex_count) +
      ",\"type\":\"VEC2\"},{\"bufferView\":3,\"componentType\":5125,\"count\":" +
      std::to_string(index_count) +
      ",\"type\":\"SCALAR\"}],\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,"
      "1,1,1]}}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
      "\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}]}],\"nodes\":[{\"mesh\":0}],\"scenes\":[{"
      "\"nodes\":[0]}],\"scene\":0}";
  std::ofstream gltf(dir + "/" + stem + ".gltf", std::ios::binary);
  std::ofstream data(dir + "/" + stem + ".bin", std::ios::binary);
  if (!gltf.good() || !data.good()) return false;
  gltf << json;
  data.write(reinterpret_cast<const char*>(bin.data()), static_cast<std::streamsize>(bin.size()));
  return gltf.good() && data.good();
}

bool write_text(const std::string& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
  return file.good();
}

}  // namespace

TEST_CASE("content_build: the build key moves with --no-morph and --atlas repack, and only then") {
  const content_build::MeshOptions plain;
  const u64 source = 0x1234'5678'9abc'def0ull;
  const u64 key = content_build::build_key_of(source, plain);
  CHECK(key == geometry::cluster_cache_key(source, content_build::lod_options_of(plain), plain.weld,
                                           plain.page_bytes));
  content_build::MeshOptions no_morph = plain;
  no_morph.morph = false;
  content_build::MeshOptions repack = plain;
  repack.repack = true;
  CHECK(content_build::build_key_of(source, no_morph) != key);
  CHECK(content_build::build_key_of(source, repack) != key);
  CHECK(content_build::build_key_of(source, repack) !=
        content_build::build_key_of(source, no_morph));
  // A repack's own options move it again; a kept atlas ignores them.
  content_build::MeshOptions repack_two = repack;
  repack_two.atlas.supersample = 2;
  CHECK(content_build::build_key_of(source, repack_two) !=
        content_build::build_key_of(source, repack));
  content_build::MeshOptions keep_two = plain;
  keep_two.atlas.supersample = 2;
  CHECK(content_build::build_key_of(source, keep_two) == key);
}

TEST_CASE("content_build: the spellings the command line and the manifest share") {
  geometry::SeamRule rule = geometry::SeamRule::none;
  CHECK(content_build::read_seam_rule("lock", rule));
  CHECK(rule == geometry::SeamRule::lock);
  CHECK_FALSE(content_build::read_seam_rule("Lock", rule));
  CHECK(rule == geometry::SeamRule::lock);  // untouched on a refusal
  bool repack = false;
  CHECK(content_build::read_atlas_mode("repack", repack));
  CHECK(repack);
  CHECK_FALSE(content_build::read_atlas_mode("pack", repack));
  atlas::NormalMapMode mode = atlas::NormalMapMode::convert;
  CHECK(content_build::read_normal_map_mode("resample", mode));
  CHECK(mode == atlas::NormalMapMode::resample);
  content_build::MeshOptions options;
  CHECK(content_build::options_in_range(options));
  options.max_triangles = 257;
  CHECK_FALSE(content_build::options_in_range(options));
}

TEST_CASE("content_build: a manifest resolves against its own directory and refuses a bad entry") {
  const test::TempDir tmp("content_build_manifest");
  REQUIRE(tmp.ok());
  const std::string manifest = tmp.file("meshes.json");
  REQUIRE(write_text(manifest,
                     "{\"meshes\":[{\"source\":\"a.gltf\",\"output\":\"out/a.clusters\","
                     "\"options\":{\"max_triangles\":64,\"weld\":false,\"atlas\":\"repack\"}},"
                     "{\"source\":\"b.gltf\"}]}"));
  content_build::MeshOptions defaults;
  defaults.page_bytes = 0;
  Vector<content_build::ManifestEntry> entries;
  std::string error;
  REQUIRE_MESSAGE(content_build::read_manifest(manifest, defaults, entries, error), error);
  REQUIRE(entries.size() == 2);
  CHECK(entries[0].source == io::join_path(tmp.path(), "a.gltf"));
  CHECK(entries[0].output == io::join_path(tmp.path(), "out/a.clusters"));
  CHECK(entries[0].options.max_triangles == 64);
  CHECK_FALSE(entries[0].options.weld);
  CHECK(entries[0].options.repack);
  CHECK(entries[0].options.page_bytes == 0);  // what the entry did not say is the default's
  CHECK(entries[1].output.empty());           // the cache's, when the caller asks for one
  CHECK(entries[1].options.max_triangles == defaults.max_triangles);

  const char* bad[] = {
      "{\"mesh\":[]}",
      "{\"meshes\":[5]}",
      "{\"meshes\":[{\"output\":\"x\"}]}",
      "{\"meshes\":[{\"source\":\"a\",\"options\":{\"max_triangles\":1000}}]}",
      "{\"meshes\":[{\"source\":\"a\",\"options\":{\"atlas\":\"maybe\"}}]}",
      "not json",
  };
  for (const char* text : bad) {
    CAPTURE(text);
    REQUIRE(write_text(manifest, text));
    Vector<content_build::ManifestEntry> none;
    std::string message;
    CHECK_FALSE(content_build::read_manifest(manifest, defaults, none, message));
    CHECK(message.find("manifest") != std::string::npos);
  }
}

TEST_CASE(
    "content_build: a task builds, skips when the container is the answer, and force rebuilds") {
  const test::TempDir tmp("content_build_task");
  REQUIRE(tmp.ok());
  REQUIRE(write_grid(tmp.path(), "grid", 16, 0.25f));

  content_build::ManifestEntry entry;
  entry.source = tmp.file("grid.gltf");
  entry.output = tmp.file("grid.clusters");
  const std::string ddc = tmp.file("ddc");
  jobs::JobSystem pool(content_build::job_config(2));

  content_build::MeshTask first;
  first.entry = &entry;
  first.ddc = &ddc;
  first.pool = &pool;
  content_build::run_mesh_task(&first);
  REQUIRE_MESSAGE(first.state == content_build::TaskState::Built, first.error.message);
  CHECK(first.result.triangles == 512);
  CHECK(first.result.clusters > first.result.leaf_clusters);
  CHECK(first.result.lod_levels > 1);
  CHECK(first.result.bytes > 0);
  CHECK(content_build::repairs_json(first.result).size() == 0);
  JsonValue keep = JsonValue::object();
  keep.set("mode", JsonValue("keep"));
  CHECK(content_build::atlas_json(first.result, entry.options) == keep);

  // The same bytes and options again: the container records them, so nothing is rebuilt.
  content_build::MeshTask second;
  second.entry = &entry;
  second.ddc = &ddc;
  content_build::run_mesh_task(&second);
  CHECK(second.state == content_build::TaskState::Skipped);
  CHECK(second.result.build_key == first.result.build_key);
  CHECK(second.result.bytes == first.result.bytes);

  // `force` builds anyway, and the bytes are the same: the build is a function of its inputs.
  content_build::MeshTask forced;
  forced.entry = &entry;
  forced.ddc = &ddc;
  forced.force = true;
  content_build::run_mesh_task(&forced);
  REQUIRE(forced.state == content_build::TaskState::Built);
  CHECK(forced.result.hash == first.result.hash);

  // The metrics engine-content stats prints, for the container just written.
  JsonValue summary;
  std::string human;
  std::string error;
  REQUIRE_MESSAGE(content_build::container_stats(entry.output, summary, human, error), error);
  u64 clusters = 0;
  REQUIRE(summary.find("clusters") != nullptr);
  CHECK(summary.find("clusters")->get_u64(clusters));
  CHECK(clusters == first.result.clusters);
  CHECK(summary.find("triangles_per_cluster") != nullptr);
  CHECK(summary.find("bytes") != nullptr);
  CHECK(human.find("streaming sweep") != std::string::npos);
  CHECK_FALSE(content_build::container_stats(tmp.file("nothing.clusters"), summary, human, error));

  // A source that is not there fails with the rule a script keys on, and writes nothing.
  content_build::ManifestEntry missing;
  missing.source = tmp.file("missing.gltf");
  missing.output = tmp.file("missing.clusters");
  content_build::MeshTask failed;
  failed.entry = &missing;
  failed.ddc = &ddc;
  content_build::run_mesh_task(&failed);
  CHECK(failed.state == content_build::TaskState::Failed);
  CHECK(std::string(failed.error.rule) == "source.unreadable");
}

TEST_CASE("content_build: the protocol's ContentBuildOptions states the build's own defaults") {
  // `content.build` reads its options from this schema type; if a builder default moves and the
  // IDL does not, the protocol and engine-content would build different containers from the
  // same request.
  const protocol::ContentBuildOptions wire;
  const content_build::MeshOptions build;
  CHECK(wire.max_triangles == build.max_triangles);
  CHECK(wire.max_vertices == build.max_vertices);
  CHECK(wire.page_bytes == build.page_bytes);
  CHECK(wire.weld == build.weld);
  CHECK(wire.morph == build.morph);
  CHECK(wire.uv_weight == build.uv_weight);
  CHECK(wire.normal_weight == build.normal_weight);
  geometry::SeamRule uv = geometry::SeamRule::none;
  geometry::SeamRule normal = geometry::SeamRule::lock;
  REQUIRE(content_build::read_seam_rule(wire.uv_seams, uv));
  REQUIRE(content_build::read_seam_rule(wire.normal_seams, normal));
  CHECK(uv == build.uv_seams);
  CHECK(normal == build.normal_seams);
  bool repack = true;
  REQUIRE(content_build::read_atlas_mode(wire.atlas, repack));
  CHECK(repack == build.repack);
  atlas::NormalMapMode maps = atlas::NormalMapMode::resample;
  REQUIRE(content_build::read_normal_map_mode(wire.atlas_normal_maps, maps));
  CHECK(maps == build.atlas.normal_maps);
  CHECK(wire.atlas_proxy == build.atlas.proxy_triangles);
  CHECK(wire.atlas_chart_cost == build.atlas.max_chart_cost_milli);
  CHECK(wire.atlas_supersample == build.atlas.supersample);
}

// ---- derived steps ------------------------------------------------------------------------------
//
// The table a capability hands the build (the audio capability's `.clip` is the one in the tree,
// held to its rules end to end by apps/engine_content/tests/clip_tests.cpp). A stand-in step here
// holds the orchestration to the rules without any capability: the source's identity, the skip,
// `force`, the manifest's array and its resolution, and the two failures.

namespace {

// "Builds" a source by writing its bytes behind the 16-byte identity it was given.
bool fake_accepts(std::string_view path) { return path.ends_with(".txt"); }

u64 fake_key(u64 source_hash) { return source_hash ^ 0x5a5a5a5a5a5a5a5aull; }

std::string fake_path(std::string_view ddc, u64 key) {
  return io::join_path(io::join_path(ddc, "fake"), std::to_string(key) + ".out");
}

bool fake_identity(const std::string& output, u64& source_hash, u64& build_key) {
  std::string bytes;
  if (io::read_file(output, bytes) != io::Status::Ok || bytes.size() < 16) return false;
  std::memcpy(&source_hash, bytes.data(), 8);
  std::memcpy(&build_key, bytes.data() + 8, 8);
  return true;
}

bool fake_build(std::span<const u8> source, u64 source_hash, u64 build_key,
                const std::string& output, JsonValue& report, std::string& error) {
  if (source.size() >= 3 && std::memcmp(source.data(), "bad", 3) == 0) {
    error = "not a source this step takes";
    return false;
  }
  std::string bytes(16 + source.size(), '\0');
  std::memcpy(bytes.data(), &source_hash, 8);
  std::memcpy(bytes.data() + 8, &build_key, 8);
  std::memcpy(bytes.data() + 16, source.data(), source.size());
  if (io::write_file_atomic(output, bytes) != io::Status::Ok) {
    error = "cannot write";
    return false;
  }
  report.set("length", JsonValue(static_cast<u64>(source.size())));
  return true;
}

const content_build::DerivedStep k_fake_step{"fake",     "fakes",        &fake_accepts, &fake_key,
                                             &fake_path, &fake_identity, &fake_build};

}  // namespace

TEST_CASE("content_build: a derived step builds a manifest's entries once and skips them after") {
  const test::TempDir tmp("content_build_derived");
  REQUIRE(tmp.ok());
  REQUIRE(write_text(tmp.file("a.txt"), "alpha"));
  REQUIRE(write_text(tmp.file("b.txt"), "bravo"));
  REQUIRE(write_text(tmp.file("bad.txt"), "bad input"));
  REQUIRE(io::make_directories(tmp.file("out")) == io::Status::Ok);
  const std::string manifest = tmp.file("manifest.json");
  REQUIRE(write_text(manifest,
                     "{\"fakes\":[{\"source\":\"a.txt\"},"
                     "{\"source\":\"b.txt\",\"output\":\"out/b.out\"},"
                     "{\"source\":\"bad.txt\",\"output\":\"out/bad.out\"},"
                     "{\"source\":\"missing.txt\",\"output\":\"out/m.out\"}]}"));

  // A manifest of a step's entries alone has no meshes, when the host names the step's array.
  const content_build::MeshOptions defaults;
  Vector<content_build::ManifestEntry> meshes;
  std::string message;
  CHECK_FALSE(content_build::read_manifest(manifest, defaults, meshes, message));
  const char* const arrays[] = {"fakes"};
  REQUIRE(content_build::read_manifest(manifest, defaults, meshes, message, arrays));
  CHECK(meshes.empty());

  Vector<content_build::DerivedEntry> entries;
  REQUIRE(content_build::read_derived_entries(manifest, k_fake_step, entries, message));
  REQUIRE(entries.size() == 4u);
  CHECK(entries[0].source == io::join_path(tmp.path(), "a.txt"));
  CHECK(entries[0].output.empty());
  CHECK(entries[1].output == io::join_path(tmp.path(), "out/b.out"));

  const std::string ddc = tmp.file("ddc");
  jobs::JobSystem pool(content_build::job_config(2));
  auto run = [&](bool force) {
    Vector<content_build::DerivedTask> tasks;
    for (const content_build::DerivedEntry& entry : entries) {
      content_build::DerivedTask task;
      task.step = &k_fake_step;
      task.entry = &entry;
      task.ddc = &ddc;
      task.force = force;
      tasks.push_back(std::move(task));
    }
    content_build::run_derived_tasks(
        std::span<content_build::DerivedTask>(tasks.data(), tasks.size()), pool);
    return tasks;
  };

  const Vector<content_build::DerivedTask> first = run(false);
  CHECK(first[0].state == content_build::TaskState::Built);
  CHECK(first[0].source_hash == hash_bytes("alpha", 5));
  CHECK(first[0].build_key == fake_key(first[0].source_hash));
  CHECK(first[0].path == fake_path(ddc, first[0].build_key));  // the cache's, its directory made
  CHECK(first[0].bytes == 21u);
  u64 length = 0;
  REQUIRE(first[0].report.find("length") != nullptr);
  CHECK(first[0].report.find("length")->get_u64(length));
  CHECK(length == 5u);
  CHECK(first[1].state == content_build::TaskState::Built);
  CHECK(first[2].state == content_build::TaskState::Failed);
  CHECK(std::string(first[2].error.rule) == content_build::k_rule_derived_build);
  CHECK(first[3].state == content_build::TaskState::Failed);
  CHECK(std::string(first[3].error.rule) == content_build::k_rule_source_unreadable);

  const Vector<content_build::DerivedTask> second = run(false);
  CHECK(second[0].state == content_build::TaskState::Skipped);
  CHECK(second[1].state == content_build::TaskState::Skipped);
  CHECK(second[1].bytes == 21u);

  const Vector<content_build::DerivedTask> forced = run(true);
  CHECK(forced[0].state == content_build::TaskState::Built);
  CHECK(forced[1].state == content_build::TaskState::Built);

  // A changed source is a new key: built, into a new cache entry.
  REQUIRE(write_text(tmp.file("a.txt"), "alpha, again"));
  const Vector<content_build::DerivedTask> changed = run(false);
  CHECK(changed[0].state == content_build::TaskState::Built);
  CHECK(changed[0].path != first[0].path);
  CHECK(changed[1].state == content_build::TaskState::Skipped);

  // An array that is not one is refused, naming the manifest.
  REQUIRE(write_text(manifest, "{\"fakes\":5}"));
  CHECK_FALSE(content_build::read_derived_entries(manifest, k_fake_step, entries, message));
  CHECK(message.find("manifest") != std::string::npos);
}
