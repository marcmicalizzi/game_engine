// The content build as a library. engine-content's end-to-end suite (apps/engine_content/tests)
// is this module's real test — it drives every rule, the cache, the manifest and the metrics
// through the command line, byte for byte — so these cases pin what a second host relies on: the
// build key's two moves, the spellings, a manifest's resolution, the identity skip and `force`,
// the metrics of a container, and the protocol's option defaults agreeing with the build's.
#include <core/base/macros.h>
#include <core/hash/hash.h>
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/content_build/container_stats.h>
#include <domain/content_build/content_build.h>
#include <domain/content_build/generator_hosts.h>
#include <domain/geometry/cluster_file.h>
#include <domain/texture/texture_build.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <schemas/protocol.h>
#include <string>
#include <system_error>
#include <vector>

#if ENGINE_PLATFORM_WINDOWS
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>
#endif

using namespace engine;

TEST_CASE("content_build: generator images resolve beside their recipe and propagate read errors") {
  test::TempDir tmp("generator_image");
  const std::string pixels("image\0bytes", 11);
  {
    std::ofstream file(tmp.file("leaf.png"), std::ios::binary);
    file.write(pixels.data(), static_cast<std::streamsize>(pixels.size()));
  }
  content_build::GeneratorHosts host(tmp.file("tree.flora.json"), {}, 0, nullptr);
  const auto* hooks = host.hooks();
  REQUIRE(hooks->read_image != nullptr);
  Vector<u8> bytes;
  std::string error;
  REQUIRE(hooks->read_image(hooks->context, "leaf.png", bytes, error));
  REQUIRE(bytes.size() == pixels.size());
  CHECK(std::memcmp(bytes.data(), pixels.data(), pixels.size()) == 0);
  CHECK_FALSE(hooks->read_image(hooks->context, "missing.png", bytes, error));
  CHECK(!error.empty());
}

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
bool write_grid(const std::string& dir, const std::string& stem, u32 n, f32 height = 0.0f,
                const std::string& material_extras = "") {
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
      "1,1,1]}" +
      material_extras +
      "}],\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,"
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
  // An import's key is over its own options, the loose-island rule among them.
  CHECK(key == geometry::cluster_cache_key(source, content_build::import_lod_options_of(plain),
                                           plain.weld, plain.page_bytes));
  CHECK(content_build::import_lod_options_of(plain).loose_islands);
  CHECK(!content_build::lod_options_of(plain).loose_islands);
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

// ---- C62: the source's class (docs/content-classes.md, "How it travels through derived data") --
//
// Every fixture is neutral: a flat grid, a made-up game category `fixture_glow` (`none`, `dim`,
// `bright`), and the engine's categories as names only.

namespace {

const char* const k_glow_categories =
    R"({"categories":[{"name":"fixture_glow","levels":["none","dim","bright"]}]})";

// One task over `entry`, run to its end.
content_build::MeshTask run_entry(const content_build::ManifestEntry& entry, const std::string& ddc,
                                  bool force = false) {
  content_build::MeshTask task;
  task.entry = &entry;
  task.ddc = &ddc;
  task.force = force;
  content_build::run_mesh_task(&task);
  return task;
}

bool read_bytes(const std::string& path, std::string& out) {
  return io::read_file(path, out) == io::Status::Ok;
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
// target need not exist (foundation/io's roots tests make them the same way).
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

// The forms of a sidecar that is there and leads nowhere which this system lets a test make,
// each made at `link` by `make(target, link)` and named for the case's output.
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

TEST_CASE("content_build: an unclassified source builds yesterday's bytes under yesterday's key") {
  const test::TempDir tmp("content_build_classes");
  REQUIRE(tmp.ok());
  REQUIRE(write_grid(tmp.path(), "grid", 16, 0.25f));
  const std::string ddc = tmp.file("ddc");
  content_build::ManifestEntry entry;
  entry.source = tmp.file("grid.gltf");
  entry.output = tmp.file("plain.clusters");
  const content_build::MeshTask plain = run_entry(entry, ddc);
  REQUIRE_MESSAGE(plain.state == content_build::TaskState::Built, plain.error.message);
  CHECK(plain.result.content_classes.is_null());
  u64 source_hash = 0;
  REQUIRE(content_build::source_hash_of(entry.source, source_hash, nullptr));
  CHECK(plain.result.build_key == content_build::mesh_build_key(entry.source, source_hash, {}));

  // The generation tools' sidecar, its flat map at `none`: the same key, the same bytes.
  REQUIRE(write_text(tmp.file("grid.provenance.json"),
                     R"({"schema":"engine.generation.provenance/1","content_class":"general",
                         "content_classes":{"nudity":"none","sexual":"none","violence":"none"}})"));
  entry.output = tmp.file("flat.clusters");
  const content_build::MeshTask flat = run_entry(entry, ddc);
  REQUIRE_MESSAGE(flat.state == content_build::TaskState::Built, flat.error.message);
  CHECK(flat.result.build_key == plain.result.build_key);
  CHECK(flat.result.hash == plain.result.hash);
  std::string a;
  std::string b;
  REQUIRE(read_bytes(tmp.file("plain.clusters"), a));
  REQUIRE(read_bytes(tmp.file("flat.clusters"), b));
  CHECK(a == b);
  geometry::ClusterFileData read;
  std::string error;
  REQUIRE(geometry::read_cluster_file(tmp.file("flat.clusters"), read, &error));
  CHECK(read.content_classes.empty());  // no section 54
}

// T150 (renderer.md, "Stochastic mesh materials"): a glTF material's `extras.sampling` reaches the
// container it builds as section 33's stochastic flag, which is what the renderer's upload reads;
// one that asks for nothing, or for "tiled", writes the zero every container before it wrote.
TEST_CASE("content_build: a material's extras.sampling reaches the container's sampling record") {
  const test::TempDir tmp("content_build_sampling");
  REQUIRE(tmp.ok());
  const std::string ddc = tmp.file("ddc");
  const auto built_flags = [&](const std::string& stem, const std::string& extras) {
    REQUIRE(write_grid(tmp.path(), stem, 8, 0.0f, extras));
    content_build::ManifestEntry entry;
    entry.source = tmp.file(stem + ".gltf");
    entry.output = tmp.file(stem + ".clusters");
    const content_build::MeshTask task = run_entry(entry, ddc);
    REQUIRE_MESSAGE(task.state == content_build::TaskState::Built, task.error.message);
    geometry::ClusterFileData read;
    std::string error;
    REQUIRE_MESSAGE(geometry::read_cluster_file(entry.output, read, &error), error);
    REQUIRE(read.material_sampling.size() == 1);
    return read.material_sampling[0].flags;
  };
  CHECK(built_flags("plain", "") == 0u);
  CHECK(built_flags("tiled", ",\"extras\":{\"sampling\":\"tiled\"}") == 0u);
  CHECK(built_flags("asked", ",\"extras\":{\"sampling\":\"stochastic\"}") ==
        geometry::k_material_sampling_stochastic);
}

TEST_CASE("content_build: a classified source gets its own key and section 54 with its ladder") {
  const test::TempDir tmp("content_build_classes");
  REQUIRE(tmp.ok());
  REQUIRE(write_grid(tmp.path(), "grid", 16, 0.25f));
  const std::string ddc = tmp.file("ddc");
  content_class::Categories categories;
  std::string error;
  REQUIRE(categories.add_json(k_glow_categories, error));
  content_build::ManifestEntry entry;
  entry.source = tmp.file("grid.gltf");  // to the cache: the key addresses the entry
  REQUIRE(io::make_directories(io::join_path(ddc, "clusters")) == io::Status::Ok);
  entry.options.categories = &categories;

  // Built neutral first, then given a sidecar: a new key, so a new entry, never the old one.
  const content_build::MeshTask neutral = run_entry(entry, ddc);
  REQUIRE_MESSAGE(neutral.state == content_build::TaskState::Built, neutral.error.message);
  REQUIRE(write_text(tmp.file("grid.provenance.json"),
                     R"({"name":"grid","content_classes":{"levels":{"fixture_glow":"bright"}}})"));
  const content_build::MeshTask bright = run_entry(entry, ddc);
  REQUIRE_MESSAGE(bright.state == content_build::TaskState::Built, bright.error.message);
  CHECK(bright.result.build_key != neutral.result.build_key);
  CHECK(bright.result.path != neutral.result.path);
  CHECK(io::exists(neutral.result.path));  // the neutral entry is left as it was
  REQUIRE_FALSE(bright.result.content_classes.is_null());
  CHECK(bright.result.content_classes.find("levels")->find("fixture_glow")->as_string() ==
        "bright");

  // Section 54 holds the class with its ladder, and a host with no categories file learns it.
  geometry::ClusterFileData read;
  REQUIRE_MESSAGE(geometry::read_cluster_file(bright.result.path, read, &error), error);
  REQUIRE_FALSE(read.content_classes.empty());
  CHECK(read.build_key == bright.result.build_key);
  content_class::Categories host;
  content_class::ContentClasses classes;
  REQUIRE_MESSAGE(
      content_class::decode_classes(
          host, std::span<const u8>(read.content_classes.data(), read.content_classes.size()),
          classes, error),
      error);
  const i32 glow = host.find("fixture_glow");
  REQUIRE(glow >= 0);
  CHECK(classes.levels[static_cast<u32>(glow)] == 2);
  CHECK(host.level_name(static_cast<u32>(glow), 1) == "dim");

  // Again: the classified container is the answer to the classified source.
  const content_build::MeshTask again = run_entry(entry, ddc);
  CHECK(again.state == content_build::TaskState::Skipped);
  CHECK(again.result.build_key == bright.result.build_key);
}

TEST_CASE("content_build: a sidecar that cannot be read refuses the source and names it") {
  const test::TempDir tmp("content_build_classes");
  REQUIRE(tmp.ok());
  REQUIRE(write_grid(tmp.path(), "grid", 8));
  const std::string ddc = tmp.file("ddc");
  content_class::Categories categories;
  std::string error;
  REQUIRE(categories.add_json(k_glow_categories, error));
  content_build::ManifestEntry entry;
  entry.source = tmp.file("grid.gltf");
  entry.output = tmp.file("grid.clusters");
  entry.options.categories = &categories;
  for (const char* sidecar : {
           "{ not json",                                                           // not JSON
           R"({"content_classes":["fixture_glow"]})",                              // wrong shape
           R"({"content_classes":{"fixture_glow":2}})",                            // wrong type
           R"({"content_classes":{"fixture_glow":"dim","fixture_glow":"none"}})",  // a key twice
           R"({"content_classes":{"fixture_hum":"dim"}})",                         // undeclared
       }) {
    CAPTURE(sidecar);
    REQUIRE(write_text(tmp.file("grid.provenance.json"), sidecar));
    const content_build::MeshTask task = run_entry(entry, ddc);
    CHECK(task.state == content_build::TaskState::Failed);
    CHECK(std::string(task.error.rule) == content_build::k_rule_content_class);
    CHECK(task.error.message.find("grid.provenance.json") != std::string::npos);
    CHECK_FALSE(io::exists(entry.output));  // nothing written
  }
  // A game's category with no categories file given: refused, never read as `none`.
  REQUIRE(write_text(tmp.file("grid.provenance.json"),
                     R"({"content_classes":{"fixture_glow":"dim"}})"));
  content_build::ManifestEntry bare = entry;
  bare.options.categories = nullptr;
  const content_build::MeshTask task = run_entry(bare, ddc);
  CHECK(task.state == content_build::TaskState::Failed);
}

// A sidecar that is there and cannot be followed is a sidecar the build failed to read, never "no
// sidecar": `io::exists` follows a link and answers false for one that leads nowhere, which the
// build once read as `none` (ADR-0073: what cannot be read is never `none`). The mesh's sidecar
// and an image's, each through every form of such a link the system lets a test make.
TEST_CASE(
    "content_build: a sidecar that leads nowhere refuses the source, a mesh's or an image's") {
  u32 made = 0;
  for (const DanglingForm& form : k_dangling_forms) {
    const std::string form_name = form.name;
    CAPTURE(form_name);
    const test::TempDir tmp("content_build_classes");
    REQUIRE(tmp.ok());
    if (!form.make(tmp.file("gone"), tmp.file("grid.provenance.json"))) {
      MESSAGE("skipped: this process may not make " << form.name);
      continue;
    }
    ++made;
    REQUIRE(write_grid(tmp.path(), "grid", 8));
    content_build::ManifestEntry entry;
    entry.source = tmp.file("grid.gltf");
    entry.output = tmp.file("grid.clusters");
    const content_build::MeshTask task = run_entry(entry, tmp.file("ddc"));
    CHECK(task.state == content_build::TaskState::Failed);
    CHECK(std::string(task.error.rule) == content_build::k_rule_content_class);
    CHECK(task.error.message.find("grid.provenance.json") != std::string::npos);
    CHECK_FALSE(io::exists(entry.output));

    // An image named by path, its sidecar the same kind of link.
    const u8 pixels[16] = {200, 40, 40, 255, 40, 200, 40, 255, 40, 40, 200, 255, 9, 9, 9, 255};
    REQUIRE(image::write_png(tmp.file("tile.png"), 2, 2, 4, pixels) == io::Status::Ok);
    REQUIRE(form.make(tmp.file("gone_too"), tmp.file("tile.provenance.json")));
    content_build::TextureSource source;
    source.file = tmp.file("tile.png");
    source.options = texture::pack_texture_options(texture::TextureBuildOptions{});
    const content_build::TextureSource* sources[] = {&source};
    const u32 mesh_of[] = {0};
    jobs::JobSystem pool(content_build::job_config(1));
    Vector<content_build::TextureResult> results;
    content_build::run_texture_step(sources, mesh_of, tmp.file("ddc"), pool, results, nullptr);
    REQUIRE(results.size() == 1u);
    CHECK(results[0].state == content_build::TextureState::Failed);
    CHECK(results[0].error.find(content_build::k_rule_content_class) == 0u);
    CHECK(results[0].error.find("tile.provenance.json") != std::string::npos);
  }
  // At least the junction on Windows and the symbolic link elsewhere, so the mend is tested on
  // every system the suite runs on.
  CHECK(made >= 1u);
}

TEST_CASE(
    "content_build: the key holds names, so a reordered file keeps it and a renamed level not") {
  const test::TempDir tmp("content_build_classes");
  REQUIRE(tmp.ok());
  REQUIRE(write_grid(tmp.path(), "grid", 8));
  REQUIRE(write_text(tmp.file("grid.provenance.json"),
                     R"({"content_classes":{"fixture_glow":"dim"}})"));
  const auto key_under = [&](const char* categories_json) {
    content_class::Categories categories;
    std::string error;
    REQUIRE_MESSAGE(categories.add_json(categories_json, error), error);
    content_build::MeshOptions options;
    options.categories = &categories;
    content_build::SourceClasses classes;
    content_build::Diagnostic diagnostic;
    REQUIRE_MESSAGE(
        content_build::read_source_classes(tmp.file("grid.gltf"), options, classes, diagnostic),
        diagnostic.message);
    return content_build::classified_build_key(tmp.file("grid.gltf"), 1234u, options, classes);
  };
  const u64 key = key_under(k_glow_categories);
  CHECK(key != content_build::mesh_build_key(tmp.file("grid.gltf"), 1234u, {}));
  // Another game category listed first: `fixture_glow` has another index, and the same key.
  CHECK(key_under(R"({"categories":[{"name":"fixture_hum","levels":["none","low"]},
                     {"name":"fixture_glow","levels":["none","dim","bright"]}]})") == key);
  // A level renamed: another key.
  CHECK(
      key_under(R"({"categories":[{"name":"fixture_glow","levels":["none","dim","blazing"]}]})") !=
      key);
}

TEST_CASE("content_build: a manifest names its categories file, and refuses one it cannot read") {
  const test::TempDir tmp("content_build_classes");
  REQUIRE(tmp.ok());
  REQUIRE(write_text(tmp.file("glow.categories.json"), k_glow_categories));
  REQUIRE(write_text(tmp.file("m.json"),
                     R"({"categories":"glow.categories.json","meshes":[{"source":"grid.gltf"}]})"));
  content_class::Categories categories;
  Vector<content_build::ManifestEntry> entries;
  std::string error;
  REQUIRE_MESSAGE(
      content_build::read_manifest(tmp.file("m.json"), {}, entries, error, {}, &categories), error);
  REQUIRE(entries.size() == 1u);
  CHECK(entries[0].options.categories == &categories);
  CHECK(categories.find("fixture_glow") >= 0);
  // A host that reads no categories file refuses the manifest rather than read it as `none`.
  entries.clear();
  CHECK_FALSE(content_build::read_manifest(tmp.file("m.json"), {}, entries, error));
  REQUIRE(write_text(tmp.file("n.json"),
                     R"({"categories":"missing.json","meshes":[{"source":"grid.gltf"}]})"));
  content_class::Categories other;
  CHECK_FALSE(content_build::read_manifest(tmp.file("n.json"), {}, entries, error, {}, &other));
}

// The manifest names the file every entry's sidecar is read against, so it is parsed strictly, as
// every file the classes module reads is (C69): with `"categories"` given twice the parser's rule
// (the last one wins) let a text merge or an append choose the table without a word.
TEST_CASE("content_build: a manifest that gives a key twice is refused, naming it") {
  const test::TempDir tmp("content_build_classes");
  REQUIRE(tmp.ok());
  REQUIRE(write_text(tmp.file("glow.categories.json"), k_glow_categories));
  REQUIRE(write_text(tmp.file("hum.categories.json"),
                     R"({"categories":[{"name":"fixture_hum","levels":["none","low"]}]})"));
  REQUIRE(write_text(tmp.file("m.json"), R"({"categories":"glow.categories.json",
                                             "categories":"hum.categories.json",
                                             "meshes":[{"source":"grid.gltf"}]})"));
  content_class::Categories categories;
  Vector<content_build::ManifestEntry> entries;
  std::string error;
  CHECK_FALSE(
      content_build::read_manifest(tmp.file("m.json"), {}, entries, error, {}, &categories));
  CHECK_MESSAGE(error.find("'categories'") != std::string::npos, error);
  CHECK(entries.empty());
  CHECK(categories.find("fixture_hum") < 0);  // neither file was read
  CHECK(categories.find("fixture_glow") < 0);
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
