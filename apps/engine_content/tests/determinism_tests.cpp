// The content build's output is a function of its input on every toolchain (ADR-0035,
// docs/subsystems/geometry.md "The same bytes from every toolchain"), end to end: a GLB whose every
// number is the same float on every compiler goes through `engine-content build` — the import with
// a rotated, scaled and translated node, so the node transform and the inverse-transpose normals
// are arithmetic the test sees, two primitives with a material each, the UV repair, the weld, the
// per-primitive DAGs and their merge, the pages — and `engine-content info` reports the hash of
// every section, which is compared with the table committed here. geometry's
// `determinism_tests.cpp` pins the builder alone; this pins what the import adds. Taken on MSVC;
// see that file's header for what a failure means and what it does not.
#include <core/json/json.h>
#include <core/platform/process.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace engine;

namespace {

void put_u32(std::vector<u8>& out, u32 v) {
  for (u32 i = 0; i < 4; ++i)
    out.push_back(static_cast<u8>((v >> (8 * i)) & 0xffu));
}

void put_f32(std::vector<u8>& out, f32 v) {
  u32 bits = 0;
  std::memcpy(&bits, &v, 4);
  put_u32(out, bits);
}

// A 129 x 129 heightfield of integer bumps (every coordinate a quarter unit or a multiple of 2^-16,
// so exact), a normal of its own at every vertex, UVs of 1/128, and two primitives — the cells left
// of column 64 and the rest — over one vertex buffer, each with its own material. The node puts it
// through a rotation about +X (the quaternion 0.6, 0, 0, 0.8, which a C library parses to the same
// floats everywhere), a non-uniform scale and a translation.
bool write_fixture(const std::string& path) {
  constexpr u32 n = 129;
  constexpr i64 half = (n - 1) / 2;
  std::vector<u8> bin;
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      const i64 u = static_cast<i64>(x) - half;
      const i64 v = static_cast<i64>(z) - half;
      const i64 d2 = (u - 4) * (u - 4) + (v + 3) * (v + 3);
      const i64 h = (d2 < 100 ? (100 - d2) * (100 - d2) : 0) + ((u * 5 + v * 11) % 7 + 7) % 7;
      put_f32(bin, static_cast<f32>(u) * 0.25f);
      put_f32(bin, static_cast<f32>(h) / 65536.0f);
      put_f32(bin, static_cast<f32>(v) * 0.25f);
    }
  }
  // A different normal at every vertex, none of them axis-aligned, so the inverse transpose
  // multiplies each by something other than zero and one and the import's renormalization rounds:
  // that is where a fused multiply-add shows, as a snorm16 step now and then. They are exact binary
  // fractions and not unit, which the import does not require: it normalizes after the transform.
  // (The narrow leak geometry.md describes — one fused copy of `dot` chosen by the linker — moved a
  // handful of normal components in three Khronos samples and none of this fixture's; what this
  // case guards is everything coarser, which is what contraction in the content build's own code
  // does: GCC's engine-content from before ADR-0035 fails it in thirteen sections.)
  const u32 normal_offset = static_cast<u32>(bin.size());
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      put_f32(bin, (static_cast<f32>(x) - 64.0f) / 64.0f + 0.1875f);
      put_f32(bin, 1.0f);
      put_f32(bin, (static_cast<f32>(z) - 64.0f) / 128.0f - 0.09375f);
    }
  }
  const u32 uv_offset = static_cast<u32>(bin.size());
  for (u32 z = 0; z < n; ++z) {
    for (u32 x = 0; x < n; ++x) {
      put_f32(bin, static_cast<f32>(x) / 128.0f);
      put_f32(bin, static_cast<f32>(z) / 128.0f);
    }
  }
  const u32 index_offset = static_cast<u32>(bin.size());
  u32 counts[2] = {0, 0};
  for (u32 part = 0; part < 2; ++part) {
    for (u32 z = 0; z + 1 < n; ++z) {
      for (u32 x = 0; x + 1 < n; ++x) {
        if ((x < 64 ? 0u : 1u) != part) continue;
        const u32 a = z * n + x;
        for (const u32 i : {a, a + n, a + 1, a + 1, a + n, a + n + 1})
          put_u32(bin, i);
        counts[part] += 6;
      }
    }
  }
  auto num = [](u32 v) { return std::to_string(v); };
  const u32 vertex_bytes = n * n * 12;
  std::string json =
      "{\"asset\":{\"version\":\"2.0\"},\"scene\":0,\"scenes\":[{\"nodes\":[0]}],"
      "\"nodes\":[{\"mesh\":0,\"translation\":[2.5,-1,0.25],\"rotation\":[0.6,0,0,0.8],"
      "\"scale\":[1.5,0.75,1.25]}],"
      "\"meshes\":[{\"primitives\":["
      "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":3,\"material\":0}"
      ","
      "{\"attributes\":{\"POSITION\":0,\"NORMAL\":1,\"TEXCOORD_0\":2},\"indices\":4,\"material\":1}"
      "]}],"
      "\"materials\":[{\"pbrMetallicRoughness\":{\"baseColorFactor\":[1,0.5,0.25,1]}},"
      "{\"pbrMetallicRoughness\":{\"baseColorFactor\":[0.25,0.5,1,1],\"roughnessFactor\":0.5}}],"
      "\"accessors\":["
      "{\"bufferView\":0,\"componentType\":5126,\"count\":" +
      num(n * n) +
      ",\"type\":\"VEC3\",\"min\":[-16,0,-16],\"max\":[16,1,16]},"
      "{\"bufferView\":1,\"componentType\":5126,\"count\":" +
      num(n * n) +
      ",\"type\":\"VEC3\"},"
      "{\"bufferView\":2,\"componentType\":5126,\"count\":" +
      num(n * n) +
      ",\"type\":\"VEC2\"},"
      "{\"bufferView\":3,\"byteOffset\":0,\"componentType\":5125,\"count\":" +
      num(counts[0]) +
      ",\"type\":\"SCALAR\"},"
      "{\"bufferView\":3,\"byteOffset\":" +
      num(counts[0] * 4) + ",\"componentType\":5125,\"count\":" + num(counts[1]) +
      ",\"type\":\"SCALAR\"}],"
      "\"bufferViews\":["
      "{\"buffer\":0,\"byteOffset\":0,\"byteLength\":" +
      num(vertex_bytes) + "},{\"buffer\":0,\"byteOffset\":" + num(normal_offset) +
      ",\"byteLength\":" + num(vertex_bytes) + "},{\"buffer\":0,\"byteOffset\":" + num(uv_offset) +
      ",\"byteLength\":" + num(n * n * 8) + "},{\"buffer\":0,\"byteOffset\":" + num(index_offset) +
      ",\"byteLength\":" + num((counts[0] + counts[1]) * 4) +
      "}],\"buffers\":[{\"byteLength\":" + num(static_cast<u32>(bin.size())) + "}]}";
  while (json.size() % 4 != 0)
    json += ' ';
  std::vector<u8> glb;
  put_u32(glb, 0x46546c67u);  // "glTF"
  put_u32(glb, 2u);
  put_u32(glb, static_cast<u32>(12 + 8 + json.size() + 8 + bin.size()));
  put_u32(glb, static_cast<u32>(json.size()));
  put_u32(glb, 0x4e4f534au);  // "JSON"
  glb.insert(glb.end(), json.begin(), json.end());
  put_u32(glb, static_cast<u32>(bin.size()));
  put_u32(glb, 0x004e4942u);  // "BIN\0"
  glb.insert(glb.end(), bin.begin(), bin.end());
  std::ofstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  f.write(reinterpret_cast<const char*>(glb.data()), static_cast<std::streamsize>(glb.size()));
  return f.good();
}

struct Output {
  i32 exit_code = -1;
  std::string text;
};

Output run(std::vector<std::string> args) {
  static const std::string exe = test::app_path(ENGINE_APP_PATH);
  std::vector<std::string_view> argv;
  argv.push_back(exe);
  for (const std::string& a : args)
    argv.push_back(a);
  platform::Process p;
  std::string error;
  Output out;
  if (!p.spawn(std::span<const std::string_view>(argv.data(), argv.size()), &error)) {
    FAIL("cannot spawn engine-content: " << error);
    return out;
  }
  p.close_stdin();
  p.read_all(out.text);
  out.exit_code = p.wait();
  return out;
}

struct SectionHash {
  u32 kind;
  u64 hash;
};

// The section the container keeps its source path in: the path as the build was given it, which
// is this test's scratch directory and different on every run, so it is left out of the table.
constexpr u32 k_source_path_kind = 14;

// Taken on MSVC 14.51, 2026-09-24, at k_cluster_cache_version 13 — which section 15 (the source
// hash and the build key over it) carries, so a version bump moves that row and only that row —
// and moved to 14 the same day, which moved row 15 and added row 32 (`textures`, empty: the
// fixture has no images), and to 15 the same day again, which moved row 15 and added row 33
// (`material_sampling`: one default record per material). 723 clusters. The empty sections all
// hash to 0x9ca066f1a4ab2eea, `hash_bytes` of nothing.
constexpr SectionHash k_golden[] = {
    {1, 0xcf16170acf2289bdull},   // clusters
    {2, 0x490fb3784ad8b04eull},   // lod
    {3, 0x4078de21a7ff1cf2ull},   // vertices
    {4, 0xe91fb7caf7356d68ull},   // attributes
    {5, 0x3c84dda60b4858b4ull},   // triangles
    {6, 0x21aa9177d60a633bull},   // vertex_source
    {7, 0x12e205dad2935a73ull},   // level_cluster_counts
    {8, 0x919809e28cd5e273ull},   // cluster_material
    {9, 0xe1847fa60a0ef6c3ull},   // materials
    {10, 0x9ca066f1a4ab2eeaull},  // image_paths
    {11, 0x9ca066f1a4ab2eeaull},  // strings
    {12, 0xabbe4b9547eecfedull},  // scalars
    {13, 0x8de2d34a8650a3bcull},  // quantized
    {15, 0x4a013abb37af46a9ull},  // source_hash: the source's hash and the build key
    {16, 0x4e5e346448e74edeull},  // pages
    {17, 0xcac3ebbc6dcebd3aull},  // page_children
    {18, 0x1d46f3ca5f069ef3ull},  // page_scalars
    {19, 0x9ca066f1a4ab2eeaull},  // skin
    {20, 0x1e3a02855f6b4e6bull},  // skin_scalars
    {21, 0x9ca066f1a4ab2eeaull},  // images
    {22, 0x9ca066f1a4ab2eeaull},  // image_bytes
    {23, 0x9ca066f1a4ab2eeaull},  // morph_channels
    {24, 0x9ca066f1a4ab2eeaull},  // morph_names
    {25, 0x9ca066f1a4ab2eeaull},  // morph_cluster_slices
    {26, 0x9ca066f1a4ab2eeaull},  // morph_slices
    {27, 0x9ca066f1a4ab2eeaull},  // morph_indices
    {28, 0x9ca066f1a4ab2eeaull},  // morph_deltas
    {29, 0x0893bb8449506c8bull},  // morph_scalars
    {30, 0x73c95020f80e9f79ull},  // vertex_ids
    {31, 0x89af6b25f28e6045ull},  // vertex_id_scalars
    {32, 0x9ca066f1a4ab2eeaull},  // textures: none, since the fixture has no images
    {33, 0x58857a9ce8175a5eull},  // material_sampling: the fixture's materials, at the defaults
};

}  // namespace

TEST_CASE("engine-content: a build writes the committed bytes on every toolchain") {
  const test::TempDir tmp("engine_content_determinism");
  const std::string mesh = tmp.file("fixture.glb");
  const std::string out = tmp.file("fixture.clusters");
  REQUIRE(write_fixture(mesh));
  const Output built = run({"build", mesh, out, "--jobs", "2"});
  REQUIRE_MESSAGE(built.exit_code == 0, built.text);
  const Output info = run({"info", out});
  REQUIRE_MESSAGE(info.exit_code == 0, info.text);
  JsonValue summary;
  REQUIRE(parse_json(info.text, summary).ok);
  const JsonValue* sections = summary.find("sections");
  REQUIRE(sections != nullptr);
  REQUIRE(sections->is_array());

  std::vector<SectionHash> got;
  for (usize i = 0; i < sections->size(); ++i) {
    const JsonValue& s = (*sections)[i];
    u64 kind = 0;
    u64 hash = 0;
    const JsonValue* k = s.find("kind");
    const JsonValue* h = s.find("hash");
    REQUIRE(k != nullptr);
    REQUIRE(h != nullptr);
    REQUIRE(k->get_u64(kind));
    REQUIRE(h->get_u64(hash));
    if (kind == k_source_path_kind) continue;
    got.push_back(SectionHash{static_cast<u32>(kind), hash});
  }
  u64 clusters = 0;
  REQUIRE(summary.find("clusters") != nullptr);
  REQUIRE(summary.find("clusters")->get_u64(clusters));
  MESSAGE("fixture: " << clusters << " clusters");

  bool same = got.size() == std::size(k_golden);
  for (usize i = 0; same && i < got.size(); ++i)
    same = got[i].kind == k_golden[i].kind && got[i].hash == k_golden[i].hash;
  if (!same) {
    std::string table = "this build's table:\n";
    for (const SectionHash& s : got) {
      bool known = false;
      for (const SectionHash& g : k_golden)
        known = known || (g.kind == s.kind && g.hash == s.hash);
      char line[96];
      std::snprintf(line, sizeof(line), "    {%u, 0x%016llxull},%s\n", s.kind,
                    static_cast<unsigned long long>(s.hash), known ? "" : "  // <-- differs");
      table += line;
    }
    FAIL_CHECK("the container does not match the committed hashes; " << table);
  }
}

#if ENGINE_CONTENT_RUINS
// The ruin assembler's row (docs/subsystems/ruins.md, "The same ruin from every toolchain"): the
// synthetic kit of boxes, written by `ruins-kit`, and the 64 buildings world seed 2026 puts on the
// 16 x 16 tiles around the origin, assembled on two thread counts, must hash to the number pinned
// here. Every decision the assembler makes is integer arithmetic on centimetres and Q10 fractions
// and every draw is the engine's hash, so the only floats in the hash are the kit's metres parsed
// once and the positions written from integer centimetres — which is why the same number holds on
// MSVC, GCC and Clang, at v2 and v3. The ground is flat: a terrain's height is the caller's, and
// no decision reads it. A change to the grammar, the ruin rule or a draw's purpose moves it; say
// so in the commit and take the new number from a failing run's message.
//
// Taken on MSVC 14.51, 2026-09-24: 64 buildings, 3,272 instances.
constexpr u64 k_ruins_golden = 0x3667b0bbaa0d803cull;

TEST_CASE("engine-content: ruins assembles the committed buildings on every toolchain") {
  const test::TempDir tmp("engine_content_ruins_determinism");
  const Output kit = run({"ruins-kit", tmp.file("kit")});
  REQUIRE_MESSAGE(kit.exit_code == 0, kit.text);
  const std::string kit_path = tmp.file("kit") + "/kit.json";
  std::string hashes[2];
  u64 buildings = 0;
  u64 instances = 0;
  const char* jobs[2] = {"1", "3"};
  for (u32 t = 0; t < 2; ++t) {
    const Output made = run({"ruins", kit_path, "2026", "-", "--region", "-8,-8,7,7", "--count",
                             "64", "--wind", "30", "--no-write", "--jobs", jobs[t]});
    REQUIRE_MESSAGE(made.exit_code == 0, made.text);
    JsonValue summary;
    REQUIRE(parse_json(made.text, summary).ok);
    REQUIRE(summary.find("hash") != nullptr);
    hashes[t] = std::string(summary.find("hash")->as_string());
    REQUIRE(summary.find("buildings")->get_u64(buildings));
    REQUIRE(summary.find("instances")->get_u64(instances));
  }
  CHECK(buildings == 64);
  MESSAGE("ruins: 64 buildings, " << instances << " instances, hash " << hashes[0]);
  // One thread or three: the same buildings in the same order.
  CHECK(hashes[0] == hashes[1]);
  char golden[17];
  std::snprintf(golden, sizeof(golden), "%016llx", static_cast<unsigned long long>(k_ruins_golden));
  CHECK_MESSAGE(hashes[0] == std::string(golden),
                "the assembled ruins do not match the committed hash; this build's is 0x"
                    << hashes[0] << "ull");
}

// The block layer's row (docs/subsystems/ruins.md, "The block layer"): the same 64 buildings laid
// block by block from the synthetic block kit, written by `ruins-block-kit`, on two thread counts.
// The layer decides on the same integers the assembler does — centimetres along a wall, Q10
// fractions, the engine's hash, the Q14 turn — so the same number holds on every toolchain. A
// change to the layer's rules or draws moves it; say so in the commit and take the new number
// from a failing run's message.
//
// Taken on MSVC 14.51, 2026-09-24: 64 buildings, 44,876 blocks.
constexpr u64 k_ruin_blocks_golden = 0x68ed0c002d78242full;

TEST_CASE("engine-content: ruins laid block by block are the committed blocks on every toolchain") {
  const test::TempDir tmp("engine_content_ruin_blocks_determinism");
  const Output kit = run({"ruins-kit", tmp.file("kit")});
  REQUIRE_MESSAGE(kit.exit_code == 0, kit.text);
  const Output blocks = run({"ruins-block-kit", tmp.file("blocks")});
  REQUIRE_MESSAGE(blocks.exit_code == 0, blocks.text);
  const std::string kit_path = tmp.file("kit") + "/kit.json";
  const std::string blocks_path = tmp.file("blocks") + "/block-kit.json";
  std::string hashes[2];
  u64 buildings = 0;
  u64 instances = 0;
  const char* jobs[2] = {"1", "3"};
  for (u32 t = 0; t < 2; ++t) {
    const Output made =
        run({"ruins", kit_path, "2026", "-", "--region", "-8,-8,7,7", "--count", "64", "--wind",
             "30", "--blocks", blocks_path, "--no-write", "--jobs", jobs[t]});
    REQUIRE_MESSAGE(made.exit_code == 0, made.text);
    JsonValue summary;
    REQUIRE(parse_json(made.text, summary).ok);
    REQUIRE(summary.find("hash") != nullptr);
    hashes[t] = std::string(summary.find("hash")->as_string());
    REQUIRE(summary.find("buildings")->get_u64(buildings));
    REQUIRE(summary.find("instances")->get_u64(instances));
  }
  CHECK(buildings == 64);
  MESSAGE("ruin blocks: 64 buildings, " << instances << " blocks, hash " << hashes[0]);
  CHECK(hashes[0] == hashes[1]);
  char golden[17];
  std::snprintf(golden, sizeof(golden), "%016llx",
                static_cast<unsigned long long>(k_ruin_blocks_golden));
  CHECK_MESSAGE(
      hashes[0] == std::string(golden),
      "the laid blocks do not match the committed hash; this build's is 0x" << hashes[0] << "ull");
}
#endif
