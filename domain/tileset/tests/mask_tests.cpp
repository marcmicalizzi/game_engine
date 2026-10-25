// The mask level (masks.h; docs/subsystems/tileset.md, "The mask level"): the build composites the
// inputs deterministically, a seam holds the same weights in both tiles, a tile naming more layers
// than its budget is refused, and the level reads back as the field it was built from.
#include <core/hash/hash.h>
#include <domain/scene_gen/ground_masks.h>
#include <domain/tileset/masks.h>
#include <domain/tileset/tileset.h>
#include <foundation/image/png.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

using namespace engine;
using namespace engine::tileset;

namespace {

// A mountain 80 m high in a 256 m square at 1 m: a paraboloid, so its slope rises with the radius
// through the soil (under 29 degrees), scree (29 to 35) and bedrock (past 39) bands to 58 degrees
// at its foot 100 m out, with flat ground round it.
Heightfield mountain() {
  Heightfield f;
  f.width = f.height = 257;
  f.spacing_mm = 1000;
  f.origin_x_mm = f.origin_z_mm = -128000;
  f.data.resize(257u * 257u);
  for (u32 z = 0; z < 257; ++z)
    for (u32 x = 0; x < 257; ++x) {
      const f64 dx = static_cast<f64>(x) - 128.0, dz = static_cast<f64>(z) - 128.0;
      const f64 q = (dx * dx + dz * dz) / 10000.0;
      f.data[z * 257 + x] = static_cast<f32>(q < 1.0 ? 80.0 * (1.0 - q) : 0.0);
    }
  return f;
}

scene::TerrainMaskRegion circle(const char* name, f64 x, f64 z, f32 r) {
  scene::TerrainMaskRegion region;
  region.name = name;
  region.center = WorldPos{x, 0, z};
  region.radius_m = r;
  return region;
}

struct Fixture {
  test::TempDir tmp{"engine_tileset_masks"};
  TileSet set;
  MaskInputs in;
  std::string error;

  Fixture() {
    REQUIRE_MESSAGE(write_heightfield(tmp.file("mountain.json"), mountain(), error), error);
    BuildOptions o;
    o.cells = 32;
    o.ring_spacing_mm = 2000;
    o.far_ratio = 2;
    o.far_levels = 1;
    BuildResult built;
    REQUIRE_MESSAGE(
        ensure_tileset(tmp.file("mountain.json"), o, tmp.file("set"), "", false, built, error),
        error);
    REQUIRE_MESSAGE(set.open(built.manifest, error), error);
    // A painted weight map: litter in the map's far corner, a ramp over 32 texels of 64.
    Vector<u8> pixels(64u * 64u * 4u, 255);
    for (u32 z = 0; z < 64; ++z)
      for (u32 x = 0; x < 64; ++x) {
        const u32 p = (z * 64 + x) * 4;
        const u8 v = x < 8 && z < 8 ? static_cast<u8>(255 - 30 * std::max(x, z)) : 0;
        pixels[p] = pixels[p + 1] = pixels[p + 2] = v;
      }
    REQUIRE(image::write_png(tmp.file("litter.png"), 64, 64, 4, {pixels.data(), pixels.size()}) ==
            io::Status::Ok);
    for (const char* name :
         {"soil", "scree", "rock", "snow", "sand", "gravel", "trail", "litter", "mud"})
      in.palette.push_back(name);
    in.budget = 4;
    in.map_x0_mm = in.map_z0_mm = -128000;
    in.map_size_mm = 256000;
    in.masks.spacing_mm = 1000;
    in.masks.tile_cells = 16;
    scene::TerrainMaskPriors p;
    p.soil = "soil";
    p.scree = "scree";
    p.bedrock = "rock";
    p.snow = "snow";
    p.snowline_m = 60;
    p.snow_blend_m = 8;
    p.snow_aspect_m = 6;
    in.masks.priors = p;
    scene::TerrainMaskPaint paint;
    paint.material = "litter";
    paint.image = tmp.file("litter.png");
    in.masks.paint.push_back(paint);
    // Two ponds at the same height with different beds, a trail across the flat.
    scene::TerrainMaskRegion sandy = circle("sandy pond", 110, 110, 8);
    sandy.bed = "sand";
    sandy.margin = "gravel";
    sandy.margin_m = 3;
    sandy.wet_m = 6;
    in.masks.regions.push_back(sandy);
    scene::TerrainMaskRegion boggy = circle("boggy pond", -110, 110, 8);
    boggy.bed = "mud";
    boggy.margin = "mud";
    boggy.wet_m = 10;
    boggy.feather_m = 4;
    in.masks.regions.push_back(boggy);
    scene::TerrainMaskRegion trail;
    trail.name = "trail";
    for (const WorldPos q : {WorldPos{115, 0, -120}, WorldPos{120, 0, -120}, WorldPos{120, 0, -20},
                             WorldPos{115, 0, -20}})
      trail.polygon.push_back(q);
    trail.material = "trail";
    trail.feather_m = 1;
    in.masks.regions.push_back(trail);
  }
};

// The palette weight a tile holds at one of its points of mip `m`, by layer.
u32 weight_at(const MaskLevel& level, std::span<const u32> words, u32 m, u32 a, u32 b, u32 layer) {
  const u32 c = level.info().cells;
  const u32 side = scene_gen::mask_mip_side(c, m);
  const u32 word = words[1 + scene_gen::mask_mip_offset(c, m) + b * side + a];
  for (u32 ch = 0; ch < 4; ++ch)
    if (((words[0] >> (8 * ch)) & 0xFFu) == layer) return (word >> (8 * ch)) & 0xFFu;
  return 0;
}
u32 wet_at(const MaskLevel& level, std::span<const u32> words, u32 m, u32 a, u32 b) {
  const u32 c = level.info().cells;
  const u32 side = scene_gen::mask_mip_side(c, m);
  const u32 p = scene_gen::mask_mip_offset(c, m) + b * side + a;
  return (words[1 + scene_gen::mask_points(c) + p / 4] >> (8 * (p % 4))) & 0xFFu;
}

}  // namespace

TEST_CASE("tileset masks: a seam holds the same palette weights in both tiles at every mip") {
  Fixture fx;
  MaskBuildResult built;
  REQUIRE_MESSAGE(
      ensure_mask_level(fx.set, fx.in, fx.tmp.file("masks"), "", false, built, fx.error), fx.error);
  MaskLevel level;
  REQUIRE_MESSAGE(level.open(built.manifest, fx.error), fx.error);
  const scene_gen::GroundMaskInfo& info = level.info();
  const u32 c = info.cells;
  const u32 n = scene_gen::mask_tile_words(c);
  Vector<u32> a(n), b(n);
  u32 seams = 0, points = 0, nonzero = 0;
  for (i32 tz = info.tile_min_z; tz <= info.tile_max_z; ++tz)
    for (i32 tx = info.tile_min_x; tx <= info.tile_max_x; ++tx) {
      if (!level.tile(tx, tz, {a.data(), n})) continue;
      for (u32 dir = 0; dir < 2; ++dir) {
        const i32 nx = tx + (dir == 0 ? 1 : 0), nz = tz + (dir == 1 ? 1 : 0);
        if (!level.tile(nx, nz, {b.data(), n})) continue;
        ++seams;
        for (u32 m = 0; m < scene_gen::mask_mips(c); ++m) {
          const u32 side = scene_gen::mask_mip_side(c, m);
          for (u32 k = 0; k < side; ++k) {
            // This tile's last row or column is the next one's first.
            const u32 ax = dir == 0 ? side - 1 : k, az = dir == 0 ? k : side - 1;
            const u32 bx = dir == 0 ? 0 : k, bz = dir == 0 ? k : 0;
            for (u32 layer = 0; layer < info.palette; ++layer) {
              const u32 wa = weight_at(level, {a.data(), n}, m, ax, az, layer);
              CHECK(wa == weight_at(level, {b.data(), n}, m, bx, bz, layer));
              nonzero += wa != 0 ? 1u : 0u;
            }
            CHECK(wet_at(level, {a.data(), n}, m, ax, az) ==
                  wet_at(level, {b.data(), n}, m, bx, bz));
            ++points;
          }
        }
      }
    }
  MESSAGE("seams " << seams << ", edge points " << points << ", nonzero weights " << nonzero);
  CHECK(seams > 400);
  CHECK(nonzero > 1000);
  // The CPU reading is continuous across a seam: just before it, from the tile before, and on it,
  // from the tile after, agree to a texel's rounding of the bilinear weights.
  Vector<f32> before, on;
  f32 wb = 0, wo = 0;
  const i64 seam_x = static_cast<i64>(c) * info.spacing_mm;  // tile 1's first point
  for (const i64 z : {i64{-37400}, i64{-3300}, i64{40250}, i64{99900}}) {
    REQUIRE(level.sample(seam_x - 1, 0.999999, z, 0.25, 0.0f, before, wb));
    REQUIRE(level.sample(seam_x, 0.0, z, 0.25, 0.0f, on, wo));
    for (u32 l = 0; l < info.palette; ++l)
      CHECK(std::abs(before[l] - on[l]) < 1e-4f);
    CHECK(std::abs(wb - wo) < 1e-4f);
  }
}

TEST_CASE("tileset masks: the priors lay soil, scree, bedrock and snow where the ranges put them") {
  Fixture fx;
  Vector<f64> w;
  f64 wet = 0;
  const auto at = [&](f64 x, f64 z) {
    REQUIRE_MESSAGE(mask_point(fx.set, fx.in, static_cast<i64>(x * 1000),
                               static_cast<i64>(z * 1000), w, wet, fx.error),
                    fx.error);
  };
  at(0, -20);  // 20 m out: 18 degrees, below the snowline (80 - 3.2 = 76.8 m): snow
  CHECK(w[3] > 0.99);
  at(0, 70);  // 70 m out: 48 degrees, 40.8 m high: bedrock
  CHECK(w[2] > 0.99);
  at(-40, 0);  // 40 m out: 32.6 degrees, 67 m: the scree band at its blend, under snow? no: 67 > 64
  CHECK(w[3] > 0.99);
  at(0, 52);  // 52 m: 39.8 degrees, 58.4 m high: rock begins, scree mostly
  CHECK(w[1] > 0.5);
  at(70, -70);  // 99 m out, the foot, steep: rock
  CHECK(w[2] > 0.99);
  at(120, 0);  // the flat: soil
  CHECK(w[0] > 0.99);
  at(110, 110);  // the sandy pond's middle: its bed, wet
  CHECK(w[4] > 0.99);
  CHECK(wet == 1.0);
  at(-110, 110);  // the boggy pond: mud, at the same height as the sand
  CHECK(w[8] > 0.99);
  at(110, 119.5);  // 1.5 m outside the sandy pond: its gravel margin, still wet
  CHECK(w[5] > 0.3);
  CHECK(wet > 0.5);
  at(117.5, -60);  // the trail
  CHECK(w[6] > 0.99);
  at(-127, -127);  // the painted litter
  CHECK(w[7] > 0.9);
}

TEST_CASE("tileset masks: a tile naming more layers than its budget is refused with a sentence") {
  Fixture fx;
  // A region of trail straddling the pond's margin and bed: soil, gravel, sand and trail meet, and
  // the litter region beside them makes five.
  scene::TerrainMaskRegion crowd = circle("crowd", 100, 110, 3);
  crowd.material = "trail";
  fx.in.masks.regions.push_back(crowd);
  scene::TerrainMaskRegion more = circle("more", 102, 106, 2);
  more.material = "litter";
  fx.in.masks.regions.push_back(more);
  MaskBuildResult built;
  CHECK_FALSE(build_mask_level(fx.set, fx.in, fx.tmp.file("over"), built, fx.error));
  MESSAGE(fx.error);
  CHECK(fx.error.find("tile (") != std::string::npos);
  CHECK(fx.error.find("layer_budget is 4") != std::string::npos);
  CHECK(fx.error.find("trail") != std::string::npos);
  // And the inputs a build cannot take.
  Fixture bad;
  bad.in.masks.regions[2].base = "litter";
  CHECK_FALSE(validate_mask_inputs(bad.in, bad.error));
  CHECK(bad.error.find("both material and base") != std::string::npos);
  bad.in.masks.regions[2].base.clear();
  bad.in.masks.regions[2].material = "basalt";
  CHECK_FALSE(validate_mask_inputs(bad.in, bad.error));
  CHECK(bad.error.find("\"basalt\"") != std::string::npos);
  bad.in.masks.regions[2].material = "trail";
  bad.in.budget = 8;
  CHECK_FALSE(validate_mask_inputs(bad.in, bad.error));
  CHECK(bad.error.find("per-tile budget") != std::string::npos);
}

TEST_CASE(
    "tileset masks: the level reads back as its field, is a hit the second time, and is "
    "keyed by its images' bytes") {
  Fixture fx;
  MaskBuildResult first;
  REQUIRE_MESSAGE(ensure_mask_level(fx.set, fx.in, "", fx.tmp.file("ddc"), false, first, fx.error),
                  fx.error);
  CHECK_FALSE(first.hit);
  CHECK(first.manifest.find("tilemasks") != std::string::npos);
  MaskBuildResult second;
  REQUIRE(ensure_mask_level(fx.set, fx.in, "", fx.tmp.file("ddc"), false, second, fx.error));
  CHECK(second.hit);
  CHECK(second.key == first.key);
  CHECK(second.hash == first.hash);
  MaskLevel level;
  REQUIRE(level.open(first.manifest, fx.error));
  Vector<f64> field;
  f64 wet = 0;
  Vector<f32> read;
  f32 read_wet = 0;
  u32 checked = 0;
  for (i64 z = -120; z <= 120; z += 7)
    for (i64 x = -120; x <= 120; x += 11) {
      REQUIRE(mask_point(fx.set, fx.in, x * 1000, z * 1000, field, wet, fx.error));
      REQUIRE(level.sample(x * 1000, 0.0, z * 1000, 0.0, 0.0f, read, read_wet));
      for (u32 l = 0; l < field.size(); ++l)
        CHECK(std::abs(read[l] - static_cast<f32>(field[l])) <= 0.5f / 255.0f + 1e-6f);
      CHECK(std::abs(read_wet - static_cast<f32>(wet)) <= 0.5f / 255.0f + 1e-6f);
      ++checked;
    }
  CHECK(checked > 700);
  // A changed image is another level; the same inputs elsewhere the same key.
  Vector<u8> pixel(4u, static_cast<u8>(200));
  REQUIRE(image::write_png(fx.in.masks.paint[0].image, 1, 1, 4, {pixel.data(), 4}) ==
          io::Status::Ok);
  u64 key = 0;
  REQUIRE(mask_key(fx.set.manifest(), fx.in, key, fx.error));
  CHECK(key != first.key);
}

// **The level is one archive** (tileset.md, "The archive"): every present tile's record, in the
// order the manifest lists them, hashes to the manifest's level hash, so each is byte for byte what
// the build wrote; the reader finds each through the table, refuses a tile the level lacks, and
// reads the whole level, asked in the renderer's order, in a few windows rather than a read a tile.
TEST_CASE("tileset masks: the level's archive holds every tile and reads it in windows") {
  Fixture fx;
  MaskBuildResult built;
  REQUIRE_MESSAGE(
      ensure_mask_level(fx.set, fx.in, fx.tmp.file("masks"), "", false, built, fx.error), fx.error);
  MaskLevel level;
  REQUIRE_MESSAGE(level.open(built.manifest, fx.error), fx.error);
  const MaskLevelManifest& m = level.manifest();
  CHECK(m.archive == mask_archive(m.spacing_mm));
  std::string file;
  REQUIRE(io::read_file(io::join_path(fx.tmp.file("masks"), m.archive), file) == io::Status::Ok);
  CHECK(file.size() == m.archive_bytes);
  // The layout: a 64-byte header (count at 12, the table's offset at 24), the records, the table.
  u32 count = 0;
  u64 toc = 0;
  std::memcpy(&count, file.data() + 12, 4);
  std::memcpy(&toc, file.data() + 24, 8);
  REQUIRE(count == built.tiles);
  const u32 words = scene_gen::mask_tile_words(m.cells);
  const u64 record = 32 + u64{words} * 4;
  CHECK(toc == 64 + u64{count} * record);
  u64 h = hash_combine(k_hash_seed, std::stoull(m.key, nullptr, 16));
  for (u32 i = 0; i < count; ++i) {
    i32 tx = 0, tz = 0;
    u64 offset = 0;
    std::memcpy(&tx, file.data() + toc + u64{i} * 24, 4);
    std::memcpy(&tz, file.data() + toc + u64{i} * 24 + 4, 4);
    std::memcpy(&offset, file.data() + toc + u64{i} * 24 + 8, 8);
    CHECK(tx == m.present[2 * i]);
    CHECK(tz == m.present[2 * i + 1]);
    h = hash_combine(h, hash_bytes(file.data() + offset, record));
  }
  char hex[17];
  std::snprintf(hex, sizeof(hex), "%016llx", static_cast<unsigned long long>(h));
  CHECK(std::string(hex) == m.hash);
  // Every present tile through the reader, in the renderer's order (z, then x), twice.
  Vector<u32> got(words);
  u32 read = 0;
  for (u32 pass = 0; pass < 2; ++pass)
    for (u32 i = 0; i < count; ++i) {
      REQUIRE(level.tile(m.present[2 * i], m.present[2 * i + 1], {got.data(), words}));
      const u64 offset = 64 + u64{i} * record + 32;
      CHECK(std::memcmp(got.data(), file.data() + offset, words * 4) == 0);
      ++read;
    }
  CHECK_FALSE(level.tile(m.tile_max[0] + 1, m.tile_max[1] + 1, {got.data(), words}));
  const MaskLevel::ReadStats stats = level.read_stats();
  MESSAGE(read << " tiles read in " << stats.windows << " windows, " << stats.bytes << " bytes");
  // 289 tiles of 7.5 KB are 2.2 MB: two windows a pass.
  CHECK(stats.windows <= 6);
}

TEST_CASE("tileset masks determinism: the fixture's level hashes to the pinned value") {
  // Pinned 2026-10-24 on MSVC (msvc-debug), k_mask_version 2, k_tileset_version 2; the same bytes
  // from GCC and Clang at v3 (the containers' run of this case). Version 1's was
  // "8127dd57bf0cee9c": the records are the bytes they were, and the hash, seeded with the key,
  // moved with the two versions the key names (W76's archive). The archive's own hash is every
  // byte of the file.
  constexpr const char* k_level = "558a6a74fd7c454a";
  constexpr const char* k_archive = "75eef7ec443e0e7e";
  Fixture fx;
  MaskBuildResult built;
  REQUIRE_MESSAGE(build_mask_level(fx.set, fx.in, fx.tmp.file("pinned"), built, fx.error),
                  fx.error);
  MaskLevel level;
  REQUIRE(level.open(built.manifest, fx.error));
  std::string bytes;
  REQUIRE(io::read_file(io::join_path(fx.tmp.file("pinned"), level.manifest().archive), bytes) ==
          io::Status::Ok);
  char archive[17];
  std::snprintf(archive, sizeof(archive), "%016llx",
                static_cast<unsigned long long>(hash_bytes(bytes.data(), bytes.size())));
  MESSAGE("level hash " << level.manifest().hash << ", archive " << archive << ", " << built.tiles
                        << " tiles, " << built.bytes << " bytes, key " << level.manifest().key);
  CHECK(level.manifest().hash == std::string(k_level));
  CHECK(std::string(archive) == std::string(k_archive));
  CHECK(level.manifest().archive_bytes == bytes.size());
}
