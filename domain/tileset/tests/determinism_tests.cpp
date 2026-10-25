// **A set is the same bytes on every toolchain** (docs/subsystems/tileset.md, "The same bytes
// everywhere"): a synthetic heightfield's set, built here, hashes level by level to what was pinned
// on MSVC, which GCC and Clang (v3) reproduce. The source is polynomials and integer hashes, the
// quantization one f64 multiply-add and a floor, the filter integer sums, so nothing a toolchain
// chooses reaches the bytes. A mismatch on one toolchain is a toolchain dependence, which is the
// bug; re-pin only with a change that bumps `k_tileset_version`, and say what moved.
#include <core/hash/hash.h>
#include <domain/tileset/tileset.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <cstdio>
#include <string>

using namespace engine;
using namespace engine::tileset;

namespace {

struct Golden {
  i64 spacing_mm;
  const char* hash;     // over the level's tile records (manifest `hash`)
  const char* archive;  // over the level's archive file, every byte of it
};

// The records' hashes pinned 2026-10-17 on MSVC (msvc-debug), k_tileset_version 1, and unchanged
// by version 2 (2026-10-24, W76): a record is the bytes its tile file held. The set's hash moved
// with the build key, which names the version (it was "d3d1ae3493a1a152"); the archives' hashes
// are version 2's, pinned on MSVC and reproduced by GCC and Clang in the Linux containers.
constexpr Golden k_levels[] = {
    {250, "fd82e80e4bd15f80", "60560ee72a1596b4"},
    {1000, "3a1e7be49e9262b8", "49b1a32cfb0a8baa"},
    {2000, "e4664bf93ee87160", "279816856e9d9afe"},
};
constexpr const char* k_set = "9d8c5f04c967e143";
constexpr const char* k_grid = "4695685c9bf29577";

std::string hex16(u64 v) {
  char text[17];
  std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
  return text;
}

}  // namespace

TEST_CASE("tileset determinism: a synthetic set's level hashes are the pinned ones") {
  test::TempDir tmp{"engine_tileset_determinism"};
  // A seeded ridge 129 points a side at 25 cm, its corner off the origin, a vertical scale and an
  // offset; tiles of 32 cells; a chain at 1 and 2 m (ratios 4 and 8).
  const Heightfield field = synth_heightfield(SynthKind::ridge, 129, 250, 2026, -8000, 4000);
  std::string error;
  REQUIRE_MESSAGE(write_heightfield(tmp.file("ridge.json"), field, error), error);
  BuildOptions o;
  o.cells = 32;
  o.ring_spacing_mm = 500;
  o.far_ratio = 2;
  o.far_levels = 2;
  o.vertical_scale = 1.5;
  o.offset_y_mm = -1234;
  o.offset_x_mm = 250;
  BuildResult built;
  REQUIRE_MESSAGE(
      ensure_tileset(tmp.file("ridge.json"), o, tmp.file("set"), "", false, built, error), error);
  TileSet set;
  REQUIRE_MESSAGE(set.open(built.manifest, error), error);
  const TileSetManifest& m = set.manifest();
  REQUIRE(m.levels.size() == 3);
  bool same = m.hash == k_set && m.source_hash.size() == 16;
  std::string archives[3];
  for (u32 l = 0; l < 3; ++l) {
    std::string bytes;
    REQUIRE(io::read_file(io::join_path(tmp.file("set"), m.levels[l].archive), bytes) ==
            io::Status::Ok);
    archives[l] = hex16(hash_bytes(bytes.data(), bytes.size()));
    same = same && m.levels[l].spacing_mm == k_levels[l].spacing_mm &&
           m.levels[l].hash == k_levels[l].hash && archives[l] == k_levels[l].archive;
  }
  // The source hash is the header's bytes, which record the grid's hash: the synthetic source is
  // the same floats on every toolchain too.
  const std::string grid_hash = m.source_hash;
  if (!same) {
    std::string table = "this build's hashes:\n";
    for (u32 l = 0; l < 3; ++l)
      table += "    {" + std::to_string(m.levels[l].spacing_mm) + ", \"" + m.levels[l].hash +
               "\", \"" + archives[l] + "\"},\n";
    table += "  set \"" + m.hash + "\", source \"" + grid_hash + "\"";
    MESSAGE(table);
  }
  CHECK(same);
  CHECK(m.source_hash == std::string(k_grid));
}
