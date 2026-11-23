// The opacity micromaps' stored bake on the CPU (roadmap R207; micromap_cache.h; renderer.md, "The
// bake is read back"): the key and the file's bytes pinned (the engine's own arithmetic and layout,
// so every toolchain must give the same words), every input the key covers moving it, and a file
// refused by its header before a device is asked for anything. No device: the read back itself
// and the picture are alpha_mask_tests.cpp's.

#include <core/hash/hash.h>
#include <domain/gfx/device.h>
#include <systems/renderer/micromap_cache.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

struct KeyFixture {
  u32 items[6] = {3, 0, 4, 1, 9, 1};
  u32 counts[3] = {124, 2, 37};
  MicromapCacheKeyInput input() const {
    MicromapCacheKeyInput in;
    for (u32 i = 0; i < 28; ++i)
      in.device[i] = static_cast<u8>(i * 7 + 1);
    in.shader = 0x0123456789abcdefull;
    in.level_cap = 6;
    in.device_limit = 12;
    in.budget_bytes = u64{1024} << 20;
    in.target_texels = 4.0f;
    in.cluster_count = 10;
    in.items = items;
    in.triangle_counts = counts;
    in.materials = 0xfeedfacecafebeefull;
    in.geometry[0] = 0x1111222233334444ull;
    in.geometry[1] = 0x5555666677778888ull;
    return in;
  }
};

// A small bake's sections: three clusters, four triangles, two stored entries at levels 1 and 2.
struct SectionsFixture {
  u32 cluster_first[3] = {0, gfx::k_micromap_none, 2};
  i32 indices[4] = {0, gfx::k_micromap_fully_opaque, 1, 0};
  gfx::MicromapTriangle triangles[2] = {{0, 1, static_cast<u16>(gfx::k_micromap_format_4state)},
                                        {4, 2, static_cast<u16>(gfx::k_micromap_format_4state)}};
  u32 data[3] = {0x000000e4u, 0x5a5a5a5au, 0xa5a5ffffu};
  gfx::MicromapStats stats;
  gfx::MicromapSections sections() {
    stats.triangles = 4;
    stats.entries_per_level[1] = 2;
    stats.entries_per_level[2] = 1;
    stats.special_opaque = 1;
    stats.unique_entries = 2;
    stats.unique_per_level[1] = 1;
    stats.unique_per_level[2] = 1;
    stats.level_cap = 2;
    stats.data_bytes = sizeof(data);
    stats.index_bytes = sizeof(indices);
    stats.micro_opaque = 9;
    stats.micro_unknown = 7;
    gfx::MicromapSections s;
    s.cluster_first = cluster_first;
    s.indices = indices;
    s.triangles = triangles;
    s.data = data;
    s.stats = &stats;
    return s;
  }
};

std::string hex(u64 v) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(v));
  return text;
}

}  // namespace

TEST_CASE("micromap cache: the key is the engine's own arithmetic, the same on every toolchain") {
  const KeyFixture fx;
  const u64 key = micromap_cache_key(fx.input());
  MESSAGE("key " << hex(key));
  // Pinned on MSVC (R207); GCC and Clang must give the same word.
  CHECK(hex(key) == "0xca193a11cefcaffc");
  CHECK(micromap_cache_path("ddc", key) == "ddc/micromaps/" + hex(key).substr(2) + ".omm");
}

TEST_CASE("micromap cache: every input the key covers moves it") {
  const KeyFixture fx;
  const u64 base = micromap_cache_key(fx.input());
  auto moved = [&](auto change) {
    KeyFixture other;
    MicromapCacheKeyInput in = other.input();
    change(in, other);
    return micromap_cache_key(in) != base;
  };
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture&) { in.device[0] ^= 1; }));   // vendor
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture&) { in.device[8] ^= 1; }));   // driver
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture&) { in.device[27] ^= 1; }));  // UUID
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture&) { in.shader ^= 1; }));
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture&) { in.level_cap = 5; }));
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture&) { in.device_limit = 10; }));
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture&) { in.budget_bytes += 1; }));
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture&) { in.target_texels = 4.5f; }));
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture&) { in.cluster_count = 11; }));
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture& f) {
    f.items[1] = 2;  // another material for a cluster
    in.items = f.items;
  }));
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture& f) {
    f.counts[2] = 36;
    in.triangle_counts = f.counts;
  }));
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture&) { in.materials ^= 1; }));
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture&) { in.geometry[0] ^= 1; }));
  CHECK(moved([](MicromapCacheKeyInput& in, KeyFixture&) { in.geometry[1] ^= 1; }));
  CHECK_FALSE(moved([](MicromapCacheKeyInput&, KeyFixture&) {}));
}

TEST_CASE("micromap cache: a material's digest is of what the bake reads, never of a slot") {
  MicromapMaterialWords a;
  a.record.albedo_texture = 7;
  a.record.sampler = 3;
  a.record.uv_offset = Vec4{0.0f, 0.0f, 0.5f, 1.0f};
  a.albedo_identity = 0xabcdef;
  a.sampler_identity = (u64{1} << 32) | 5;
  const u64 base = micromap_materials_digest(std::span<const MicromapMaterialWords>(&a, 1));
  MicromapMaterialWords b = a;
  b.record.albedo_texture = 12;  // the same texture in another slot
  b.record.sampler = 9;
  b.record.normal_texture = 4;  // a slot the bake never reads
  CHECK(micromap_materials_digest(std::span<const MicromapMaterialWords>(&b, 1)) == base);
  auto moved = [&](auto change) {
    MicromapMaterialWords m = a;
    change(m);
    return micromap_materials_digest(std::span<const MicromapMaterialWords>(&m, 1)) != base;
  };
  CHECK(moved([](MicromapMaterialWords& m) { m.albedo_identity ^= 1; }));
  CHECK(moved([](MicromapMaterialWords& m) { m.sampler_identity ^= 1; }));
  CHECK(moved([](MicromapMaterialWords& m) { m.record.uv_offset.z = 0.4f; }));  // cutoff
  CHECK(moved([](MicromapMaterialWords& m) { m.record.uv_offset.w = 0.9f; }));  // factor
  CHECK(moved([](MicromapMaterialWords& m) { m.record.uv_transform.y = 0.1f; }));
  CHECK(moved([](MicromapMaterialWords& m) { m.record.flags ^= gfx::k_material_uv_transform; }));
  CHECK(moved([](MicromapMaterialWords& m) {
    m.record.albedo_texture = gfx::k_no_texture;
    m.albedo_identity = 0;
  }));
}

TEST_CASE("micromap cache: a stored bake's bytes are the engine's own layout") {
  SectionsFixture fx;
  const gfx::MicromapSections sections = fx.sections();
  const std::string header = micromap_file_header(0x00c0ffee00c0ffeeull, sections);
  // The prefix: magic, version, key, the content hash, the header's length.
  REQUIRE(header.size() > 28);
  CHECK(static_cast<u8>(header[0]) == 0x45);  // "EOMM"
  CHECK(static_cast<u8>(header[3]) == 0x4d);
  CHECK(static_cast<u8>(header[4]) == k_micromap_cache_version);
  const u64 hashed = hash_bytes(header.data(), header.size());
  MESSAGE("header " << header.size() << " bytes, hash " << hex(hashed));
  // Pinned on MSVC (R207): the layout and the content hash, which GCC and Clang must reproduce.
  CHECK(header.size() == 492);
  CHECK(hex(hashed) == "0x6b15b7b33480fa82");

  test::TempDir dir{"omm_file"};
  const std::string path = micromap_cache_path(dir.path(), 0x00c0ffee00c0ffeeull);
  std::string error;
  REQUIRE_MESSAGE(write_micromap_file(path, 0x00c0ffee00c0ffeeull, sections, &error), error);
  std::ifstream in(path, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  CHECK(bytes.size() == header.size() + sizeof(fx.cluster_first) + sizeof(fx.indices) +
                            sizeof(fx.triangles) + sizeof(fx.data));
  CHECK(bytes.compare(0, header.size(), header) == 0);
}

// A file is refused by its header before the restore asks the device for anything, so these need
// none: another key, another version, not a stored bake at all, a length its sections do not add
// up to, and no file.
TEST_CASE("micromap cache: a file is refused by its header for another key, version or length") {
  SectionsFixture fx;
  const gfx::MicromapSections sections = fx.sections();
  test::TempDir dir{"omm_refuse"};
  const u64 key = 0x1234;
  const std::string path = micromap_cache_path(dir.path(), key);
  std::string error;
  REQUIRE_MESSAGE(write_micromap_file(path, key, sections, &error), error);
  std::string good;
  {
    std::ifstream in(path, std::ios::binary);
    good.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  }
  const gfx::Device none;  // never created: a refusal must come before any device call
  auto refusal = [&](const std::string& bytes, u64 asked) {
    {
      std::ofstream out(path, std::ios::binary | std::ios::trunc);
      out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
    gfx::OpacityMicromapSet set;
    const MicromapFileRead read = read_micromap_file(none, path, asked, set);
    CHECK(read.found);
    CHECK_FALSE(read.used);
    CHECK(set.address == 0);
    return read.refused;
  };
  CHECK(refusal(good, key + 1).find("another key") != std::string::npos);
  std::string spoiled = good;
  spoiled[4] = static_cast<char>(spoiled[4] + 1);
  CHECK(refusal(spoiled, key).find("version") != std::string::npos);
  spoiled = good;
  spoiled[0] = 'X';
  CHECK(refusal(spoiled, key).find("not a stored bake") != std::string::npos);
  spoiled = good.substr(0, good.size() - 4);
  CHECK(refusal(spoiled, key).find("length") != std::string::npos);
  spoiled = good.substr(0, 10);
  CHECK(refusal(spoiled, key).find("prefix") != std::string::npos);
  gfx::OpacityMicromapSet set;
  const MicromapFileRead missing =
      read_micromap_file(none, micromap_cache_path(dir.path(), key + 7), key + 7, set);
  CHECK_FALSE(missing.found);
  CHECK(missing.refused.empty());
}
