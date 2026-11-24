// The opacity micromaps' store on the CPU (roadmap R207, R208; micromap_cache.h; renderer.md, "The
// bake is read back"): the generation and a part's key pinned (the engine's own arithmetic, so
// every toolchain must give the same words), every input a part's key covers moving it, a pack's
// bytes pinned, a pack refused by its header and by its tables before anything is copied, the
// assembly the same micromap whichever parts were stored, and the folder's pruning. No device: the
// read back itself and the picture are alpha_mask_tests.cpp's.

#include <core/hash/hash.h>
#include <systems/renderer/micromap_cache.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

using namespace engine;
using namespace engine::renderer;

namespace {

struct KeyFixture {
  u32 locals[3] = {0, 4, 9};
  u32 counts[3] = {124, 2, 37};
  u64 materials[3] = {0xfeedfacecafebeefull, 0xfeedfacecafebeefull, 0x0badf00d00000001ull};
  u64 digests[3] = {0x1111222233334444ull, 0x5555666677778888ull, 0x99990000aaaabbbbull};
  MicromapPartKeyInput input() const {
    MicromapPartKeyInput in;
    in.generation = 0x0123456789abcdefull;
    in.level_cap = 6;
    in.target_texels = 4.0f;
    in.locals = locals;
    in.triangle_counts = counts;
    in.materials = materials;
    in.digests = digests;
    return in;
  }
};

std::string hex(u64 v) {
  char text[24];
  std::snprintf(text, sizeof(text), "0x%016llx", static_cast<unsigned long long>(v));
  return text;
}

// Two parts' sections as one pack: part 0 has three triangles (an entry at level 1 twice, one
// special), part 1 two (the level-1 entry's words again under another entry, and a level-2 entry).
struct PackFixture {
  MicromapPackPart parts[2];
  i32 indices[5] = {0, gfx::k_micromap_fully_opaque, 0, 1, 2};
  gfx::MicromapTriangle triangles[3] = {{0, 1, static_cast<u16>(gfx::k_micromap_format_4state)},
                                        {4, 1, static_cast<u16>(gfx::k_micromap_format_4state)},
                                        {8, 2, static_cast<u16>(gfx::k_micromap_format_4state)}};
  u32 data[3] = {0x000000e4u, 0x000000e4u, 0xa5a5ffffu};
  PackFixture() {
    parts[0].key = 0xaaaa000000000001ull;
    parts[0].triangle_count = 3;
    parts[0].wanted[1] = 2;
    parts[0].wanted[3] = 1;
    parts[1].key = 0xbbbb000000000002ull;
    parts[1].triangle_count = 2;
    parts[1].wanted[2] = 2;
  }
  MicromapPackView view() const {
    MicromapPackView v;
    v.parts = parts;
    v.indices = indices;
    v.triangles = triangles;
    v.data = data;
    return v;
  }
};

std::string read_all(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

void write_all(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST_CASE("micromap cache: the keys are the engine's own arithmetic, the same on every toolchain") {
  u8 device[28];
  for (u32 i = 0; i < 28; ++i)
    device[i] = static_cast<u8>(i * 7 + 1);
  const u64 generation = micromap_generation(device, 0x0123456789abcdefull);
  const KeyFixture fx;
  const u64 part = micromap_part_key(fx.input());
  MESSAGE("generation " << hex(generation) << ", part " << hex(part));
  // Pinned on MSVC (R208); GCC and Clang must give the same words.
  CHECK(hex(generation) == "0xbe95f623e8515e87");
  CHECK(hex(part) == "0xf880eaf05d66dfb8");
  CHECK(micromap_generation_dir("ddc", generation) == "ddc/micromaps/" + hex(generation).substr(2));
}

TEST_CASE(
    "micromap cache: every input a part's key covers moves it, and nothing of the load does") {
  const KeyFixture fx;
  const u64 base = micromap_part_key(fx.input());
  auto moved = [&](auto change) {
    KeyFixture other;
    MicromapPartKeyInput in = other.input();
    change(in, other);
    return micromap_part_key(in) != base;
  };
  CHECK(moved([](MicromapPartKeyInput& in, KeyFixture&) { in.generation ^= 1; }));
  CHECK(moved([](MicromapPartKeyInput& in, KeyFixture&) { in.level_cap = 5; }));
  CHECK(moved([](MicromapPartKeyInput& in, KeyFixture&) { in.target_texels = 4.5f; }));
  CHECK(moved([](MicromapPartKeyInput& in, KeyFixture& f) {
    f.locals[1] = 5;  // another cluster of the part
    in.locals = f.locals;
  }));
  CHECK(moved([](MicromapPartKeyInput& in, KeyFixture& f) {
    f.counts[2] = 36;
    in.triangle_counts = f.counts;
  }));
  CHECK(moved([](MicromapPartKeyInput& in, KeyFixture& f) {
    f.materials[1] ^= 1;  // another material for a cluster
    in.materials = f.materials;
  }));
  CHECK(moved([](MicromapPartKeyInput& in, KeyFixture& f) {
    f.digests[0] ^= 1;  // another UV word
    in.digests = f.digests;
  }));
  CHECK(moved([](MicromapPartKeyInput& in, KeyFixture& f) {
    in.locals = std::span<const u32>(f.locals, 2);  // a cluster fewer
    in.triangle_counts = std::span<const u32>(f.counts, 2);
    in.materials = std::span<const u64>(f.materials, 2);
    in.digests = std::span<const u64>(f.digests, 2);
  }));
  // The generation moves with the device, the driver, the shader and nothing else.
  u8 device[28] = {};
  const u64 g = micromap_generation(device, 7);
  device[8] ^= 1;  // the driver's version
  CHECK(micromap_generation(device, 7) != g);
  device[8] ^= 1;
  device[27] ^= 1;  // the pipeline cache's UUID
  CHECK(micromap_generation(device, 7) != g);
  device[27] ^= 1;
  CHECK(micromap_generation(device, 8) != g);
  CHECK(micromap_generation(device, 7) == g);
  CHECK_FALSE(moved([](MicromapPartKeyInput&, KeyFixture&) {}));
}

TEST_CASE("micromap cache: a material's digest is of what the bake reads, never of a slot") {
  MicromapMaterialWords a;
  a.record.albedo_texture = 7;
  a.record.sampler = 3;
  a.record.uv_offset = Vec4{0.0f, 0.0f, 0.5f, 1.0f};
  a.albedo_identity = 0xabcdef;
  a.sampler_identity = (u64{1} << 32) | 5;
  const u64 base = micromap_material_digest(a);
  MicromapMaterialWords b = a;
  b.record.albedo_texture = 12;  // the same texture in another slot
  b.record.sampler = 9;
  b.record.normal_texture = 4;  // a slot the bake never reads
  CHECK(micromap_material_digest(b) == base);
  auto moved = [&](auto change) {
    MicromapMaterialWords m = a;
    change(m);
    return micromap_material_digest(m) != base;
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

TEST_CASE("micromap cache: a pack's bytes are the engine's own layout") {
  const PackFixture fx;
  const MicromapPackView view = fx.view();
  const u64 generation = 0x00c0ffee00c0ffeeull;
  const std::string header = micromap_pack_header(generation, view);
  CHECK(header.size() == k_micromap_pack_fixed_bytes + 2 * k_micromap_part_record_bytes);
  CHECK(static_cast<u8>(header[0]) == 0x45);  // "EOMM"
  CHECK(static_cast<u8>(header[3]) == 0x4d);
  CHECK(static_cast<u8>(header[4]) == k_micromap_cache_version);
  const u64 hashed = hash_bytes(header.data(), header.size());
  MESSAGE("header " << header.size() << " bytes, hash " << hex(hashed));
  // Pinned on MSVC (R208): the layout and the content hash, which GCC and Clang must reproduce.
  CHECK(hex(hashed) == "0x86b48845e0660e5b");

  test::TempDir dir{"omm_pack"};
  const std::string path = micromap_pack_path(dir.path(), generation, view.parts);
  std::string error;
  REQUIRE_MESSAGE(write_micromap_pack(path, generation, view, &error), error);
  const std::string bytes = read_all(path);
  CHECK(bytes.size() ==
        header.size() + sizeof(fx.indices) + sizeof(fx.triangles) + sizeof(fx.data));
  CHECK(bytes.compare(0, header.size(), header) == 0);
  // And it reads back as it was written.
  MicromapPackIndex index;
  REQUIRE_MESSAGE(read_micromap_pack_index(path, generation, index), index.refused);
  REQUIRE(index.parts.size() == 2);
  CHECK(index.parts[0].key == fx.parts[0].key);
  CHECK(index.parts[1].wanted[2] == 2);
  MicromapPackData data;
  std::string refused;
  REQUIRE_MESSAGE(read_micromap_pack(index, data, &refused), refused);
  CHECK(data.first_triangle[1] == 3);
  CHECK(std::memcmp(data.indices.data(), fx.indices, sizeof(fx.indices)) == 0);
  CHECK(std::memcmp(data.data.data(), fx.data, sizeof(fx.data)) == 0);
}

// A pack is refused by its header before any section is read, and by its content and its tables
// before anything of it is copied: another generation, another version, not a pack at all, a
// header or length its counts do not add up to, a changed byte, an index past its entries, a record
// past its states. Each refusal is named.
TEST_CASE("micromap cache: a pack is refused by its header, its bytes and its tables") {
  test::TempDir dir{"omm_refuse"};
  const u64 generation = 0x1234;
  const std::string path = dir.path() + "/pack.omm";
  auto refusal = [&](const PackFixture& fx, auto spoil, u64 asked) {
    std::string error;
    REQUIRE_MESSAGE(write_micromap_pack(path, generation, fx.view(), &error), error);
    std::string bytes = read_all(path);
    spoil(bytes);
    write_all(path, bytes);
    MicromapPackIndex index;
    if (!read_micromap_pack_index(path, asked, index)) return index.refused;
    MicromapPackData data;
    std::string refused;
    CHECK_FALSE(read_micromap_pack(index, data, &refused));
    CHECK(data.indices.empty());
    return refused;
  };
  const PackFixture good;
  auto none = [](std::string&) {};
  CHECK(refusal(good, none, generation + 1).find("another generation") != std::string::npos);
  CHECK(refusal(
            good, [](std::string& b) { b[4] = static_cast<char>(b[4] + 1); }, generation)
            .find("version") != std::string::npos);
  CHECK(refusal(
            good, [](std::string& b) { b[0] = 'X'; }, generation)
            .find("not a stored bake") != std::string::npos);
  CHECK(refusal(
            good, [](std::string& b) { b.resize(b.size() - 4); }, generation)
            .find("length") != std::string::npos);
  CHECK(refusal(
            good, [](std::string& b) { b.resize(20); }, generation)
            .find("shorter") != std::string::npos);
  CHECK(refusal(
            good, [](std::string& b) { b[28] = static_cast<char>(b[28] + 1); }, generation)
            .find("header's length") != std::string::npos);  // one part more than its records
  CHECK(refusal(
            good, [](std::string& b) { b[b.size() - 1] ^= 0x01; }, generation)
            .find("content hash") != std::string::npos);
  {
    PackFixture fx;
    fx.indices[3] = 3;  // past the three entries
    CHECK(refusal(fx, none, generation).find("an index names no stored entry (triangle 3") !=
          std::string::npos);
  }
  {
    PackFixture fx;
    fx.indices[1] = -5;  // below the special indices
    CHECK(refusal(fx, none, generation).find("an index names no stored entry (triangle 1") !=
          std::string::npos);
  }
  {
    PackFixture fx;
    fx.triangles[2].data_offset = 12;  // a level-2 entry's 4 bytes at 12 of 12
    CHECK(refusal(fx, none, generation).find("a triangle record names states") !=
          std::string::npos);
  }
  // A pack that is not there opens nothing.
  MicromapPackIndex missing;
  CHECK_FALSE(read_micromap_pack_index(dir.path() + "/none.omm", generation, missing));
}

// **The micromap is a function of the triangles' states, not of where they were stored** (R208):
// the scene's five triangles assembled from one pack, and from two packs that hold its parts
// apart (one of them storing the shared words under an entry of its own), give the same indices,
// records and states; the shared words are one entry, and a 64-bit proposal the words refuse
// costs an entry and never a wrong state.
TEST_CASE("micromap cache: the assembly is the same micromap whichever parts were stored") {
  const PackFixture whole;
  // Part 0 alone, and part 1 alone with its entries renumbered from zero.
  MicromapPackPart first_part[1] = {whole.parts[0]};
  const i32 first_indices[3] = {0, gfx::k_micromap_fully_opaque, 0};
  const gfx::MicromapTriangle first_records[1] = {whole.triangles[0]};
  const u32 first_data[1] = {whole.data[0]};
  MicromapPackPart second_part[1] = {whole.parts[1]};
  const i32 second_indices[2] = {0, 1};
  const gfx::MicromapTriangle second_records[2] = {
      {0, 1, static_cast<u16>(gfx::k_micromap_format_4state)},
      {4, 2, static_cast<u16>(gfx::k_micromap_format_4state)}};
  const u32 second_data[2] = {whole.data[1], whole.data[2]};
  MicromapPackView split[2];
  split[0] = MicromapPackView{first_part, first_indices, first_records, first_data};
  split[1] = MicromapPackView{second_part, second_indices, second_records, second_data};
  const MicromapPackView one[1] = {whole.view()};
  const MicromapRun one_runs[2] = {{0, 0, 3, whole.parts[0].wanted},
                                   {0, 3, 2, whole.parts[1].wanted}};
  const MicromapRun split_runs[2] = {{0, 0, 3, whole.parts[0].wanted},
                                     {1, 0, 2, whole.parts[1].wanted}};
  MicromapAssembly a;
  MicromapAssembly b;
  assemble_micromaps(one, one_runs, a);
  assemble_micromaps(split, split_runs, b);
  // Entries 0 and 1 of the pack are the same words at the same level: one entry.
  CHECK(a.triangles.size() == 2);
  CHECK(a.stats.unique_entries == 2);
  CHECK(a.stats.dedup_mismatches == 0);
  CHECK(a.stats.triangles == 5);
  CHECK(a.stats.special_opaque == 1);
  CHECK(a.stats.entries_per_level[1] == 3);
  CHECK(a.stats.wanted_per_level[2] == 2);
  // Level 1's words 0xe4 are its four states 0, 1, 2, 3: one transparent, one opaque, two unknown,
  // named three times; level 2's sixteen states of 0xa5a5ffff are 8 unknown-opaque, 4 unknown-
  // transparent and 4 opaque... counted by the states, once per triangle naming them.
  CHECK(a.stats.micro_transparent == 3 * 1 + 0);
  CHECK(a.stats.micro_opaque == 3 * 1 + 4);
  CHECK(a.stats.micro_unknown == 3 * 2 + 12);
  REQUIRE(a.indices.size() == b.indices.size());
  CHECK(std::memcmp(a.indices.data(), b.indices.data(), a.indices.size() * sizeof(i32)) == 0);
  REQUIRE(a.triangles.size() == b.triangles.size());
  CHECK(std::memcmp(a.triangles.data(), b.triangles.data(),
                    a.triangles.size() * sizeof(gfx::MicromapTriangle)) == 0);
  CHECK(a.total_bytes == b.total_bytes);
  for (u32 e = 0; e < a.triangles.size(); ++e) {
    const MicromapPackView& sa = one[a.entry_source[e]];
    const MicromapPackView& sb = split[b.entry_source[e]];
    CHECK(sa.data[sa.triangles[a.entry_index[e]].data_offset / 4] ==
          sb.data[sb.triangles[b.entry_index[e]].data_offset / 4]);
  }
}

// **What bounds the folder** (R208): a store keeps its own generation and the most recently used
// other, removes every pack of the rest and R207's whole-scene files, removes the packs a read
// refused, and sweeps a temporary file a store that died left once it is a minute old — never a
// younger one, which may be another process's store under way, and never a file a store did not
// write.
TEST_CASE("micromap cache: a store keeps two generations and leaves no dead store's temp") {
  test::TempDir dir{"omm_prune"};
  namespace fs = std::filesystem;
  const std::string root = dir.path();
  const u64 current = 0x1111;
  const u64 recent = 0x2222;
  const u64 old = 0x3333;
  const u64 older = 0x4444;
  auto make = [](const std::string& path) { write_all(path, "x"); };
  auto age = [](const std::string& path, std::chrono::minutes ago) {
    fs::last_write_time(fs::path(path), fs::file_time_type::clock::now() - ago);
  };
  for (const u64 g : {current, recent, old, older}) {
    const std::string gen = micromap_generation_dir(root, g);
    fs::create_directories(gen);
    make(gen + "/a.omm");
    make(gen + "/used");
  }
  age(micromap_generation_dir(root, recent) + "/used", std::chrono::minutes(10));
  age(micromap_generation_dir(root, old) + "/used", std::chrono::minutes(100));
  age(micromap_generation_dir(root, older) + "/used", std::chrono::minutes(1000));
  make(micromap_generation_dir(root, older) + "/notes.txt");  // not a store's: the folder stays
  const std::string here = micromap_generation_dir(root, current);
  make(here + "/b.omm");
  make(here + "/dead.omm.1.ff.tmp");
  age(here + "/dead.omm.1.ff.tmp", std::chrono::minutes(5));
  make(here + "/live.omm.2.ee.tmp");               // a store under way
  make(root + "/micromaps/00c0ffee00c0ffee.omm");  // R207's whole-scene file
  const std::string damaged[1] = {here + "/b.omm"};
  MicromapPruneReport report;
  const i64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
  prune_micromap_store(root, current, damaged, now, report);
  MESSAGE("swept " << report.swept << ", packs " << report.packs << ", generations "
                   << report.generations);
  CHECK(fs::exists(here + "/a.omm"));
  CHECK_FALSE(fs::exists(here + "/b.omm"));
  CHECK_FALSE(fs::exists(here + "/dead.omm.1.ff.tmp"));
  CHECK(fs::exists(here + "/live.omm.2.ee.tmp"));
  CHECK(fs::exists(micromap_generation_dir(root, recent) + "/a.omm"));
  CHECK_FALSE(fs::exists(micromap_generation_dir(root, old)));
  CHECK_FALSE(fs::exists(micromap_generation_dir(root, older) + "/a.omm"));
  CHECK(fs::exists(micromap_generation_dir(root, older) + "/notes.txt"));
  CHECK_FALSE(fs::exists(root + "/micromaps/00c0ffee00c0ffee.omm"));
  CHECK(report.swept == 1);
  CHECK(report.packs == 4);  // the damaged pack, old's, older's and R207's file
  CHECK(report.generations == 1);
}
