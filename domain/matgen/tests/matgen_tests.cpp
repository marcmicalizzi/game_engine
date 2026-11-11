// Tests for the matgen capability (docs/subsystems/matgen.md, "Testing"): a set tiles, a bed's
// grid count is the stated distribution, the parts' shares are the asked ones, the means are in
// their materials' ranges, a mip chain keeps the mean, and the bytes are pinned.
#include <core/hash/hash.h>
#include <domain/matgen/matgen.h>
#include <domain/texture/texture_build.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>

using namespace engine;

namespace {

matgen::Set recipe(std::string_view role, u32 texels, u64 seed = 0) {
  matgen::Set set;
  REQUIRE(matgen::default_set(role, set));
  set.texels = texels;
  if (seed != 0) set.seed = seed;
  return set;
}

struct Built {
  matgen::SetImages images;
  matgen::SetStats stats;
};

Built build(const matgen::Set& set, matgen::Window window = {}) {
  Built b;
  std::string error;
  REQUIRE_MESSAGE(matgen::build_set(set, b.images, &b.stats, error, window), error);
  return b;
}

// How many bytes of `moved` (built from a window `wx`, `wy` texels over) differ from `first`
// rotated by as much.
u32 rotation_mismatches(const Vector<u8>& first, const Vector<u8>& moved, u32 n, u32 channels,
                        u32 wx, u32 wy) {
  u32 wrong = 0;
  for (u32 y = 0; y < n; ++y) {
    for (u32 x = 0; x < n; ++x) {
      const u32 from = (((y + wy) % n) * n + (x + wx) % n) * channels;
      const u32 to = (y * n + x) * channels;
      for (u32 c = 0; c < channels; ++c)
        wrong += first[from + c] != moved[to + c] ? 1u : 0u;
    }
  }
  return wrong;
}

// The mean absolute step of a one-channel image between column `c` and the next (wrapped).
f64 column_step(const Vector<u8>& image, u32 n, u32 c) {
  f64 sum = 0;
  for (u32 y = 0; y < n; ++y)
    sum +=
        std::abs(static_cast<int>(image[y * n + c]) - static_cast<int>(image[y * n + (c + 1) % n]));
  return sum / n;
}
f64 row_step(const Vector<u8>& image, u32 n, u32 r) {
  f64 sum = 0;
  for (u32 x = 0; x < n; ++x)
    sum += std::abs(static_cast<int>(image[r * n + x]) -
                    static_cast<int>(image[((r + 1) % n) * n + x]));
  return sum / n;
}

u64 images_hash(const matgen::SetImages& s) {
  u64 h = hash_bytes(s.base_color.data(), s.base_color.size());
  h = hash_combine(h, hash_bytes(s.normal.data(), s.normal.size()));
  h = hash_combine(h, hash_bytes(s.roughness.data(), s.roughness.size()));
  return hash_combine(h, hash_bytes(s.height.data(), s.height.size()));
}

f64 luminance(const f64 c[3]) { return 0.2126 * c[0] + 0.7152 * c[1] + 0.0722 * c[2]; }

}  // namespace

TEST_CASE("matgen: a set is the same picture wherever the image starts") {
  // The set lives on the torus of its tile: built from a window half a tile and three texels
  // over, every image is the first build's bytes rotated, exactly. A noise that did not wrap its
  // lattice, a clast cut at the image's edge or a blur that stopped at it would each differ.
  for (const char* role : {"gravel/bed", "rock/scree", "sand/bar", "rock/granite", "rock/sandstone",
                           "rock/basalt", "ground/turf"}) {
    CAPTURE(role);
    const matgen::Set set = recipe(role, 128);
    const Built first = build(set);
    const u32 n = first.images.size, wx = n / 2, wy = n / 2 + 3;
    const Built moved = build(set, matgen::Window{wx, wy});
    CHECK(rotation_mismatches(first.images.base_color, moved.images.base_color, n, 3, wx, wy) == 0);
    CHECK(rotation_mismatches(first.images.normal, moved.images.normal, n, 3, wx, wy) == 0);
    CHECK(rotation_mismatches(first.images.roughness, moved.images.roughness, n, 1, wx, wy) == 0);
    CHECK(rotation_mismatches(first.images.height, moved.images.height, n, 1, wx, wy) == 0);
  }
}

TEST_CASE("matgen: the seam is a column like any other") {
  // The step across the image's edge (the last column to the first, the last row to the first)
  // is inside the range of the steps between the columns and rows within it.
  for (const char* role : {"gravel/bed", "rock/granite", "ground/turf"}) {
    CAPTURE(role);
    const Built b = build(recipe(role, 256));
    const u32 n = b.images.size;
    for (const Vector<u8>* image : {&b.images.height, &b.images.roughness}) {
      f64 lo = 1e30, hi = 0;
      for (u32 c = 0; c + 1 < n; ++c) {
        lo = std::min({lo, column_step(*image, n, c), row_step(*image, n, c)});
        hi = std::max({hi, column_step(*image, n, c), row_step(*image, n, c)});
      }
      CHECK(column_step(*image, n, n - 1) >= lo);
      CHECK(column_step(*image, n, n - 1) <= hi);
      CHECK(row_step(*image, n, n - 1) >= lo);
      CHECK(row_step(*image, n, n - 1) <= hi);
    }
  }
}

TEST_CASE("matgen: a bed's grid count reads the stated distribution, over seeds") {
  // The recipe states the surface's grid-count distribution: normal in phi about D50, sorting
  // sigma, cut two phi either side. A normal cut at two sigma has its 16th and 84th percentiles
  // at 0.933 sigma, so D84 / D50 = D50 / D16 = 2^(0.933 sigma). The count here is over every
  // texel that shows a clast.
  matgen::Set set = recipe("gravel/bed", 512);
  const f64 d50 = set.granular.d50_um * 1e-3;
  const f64 ratio = std::exp2(0.933 * set.granular.sorting_milliphi * 1e-3);
  f64 sum50 = 0, sum84 = 0, sum16 = 0;
  const u64 seeds[] = {11, 22, 33, 44, 55};
  for (const u64 seed : seeds) {
    set.seed = seed;
    const Built b = build(set);
    MESSAGE("seed " << seed << ": D16 " << b.stats.d16_mm << " D50 " << b.stats.d50_mm << " D84 "
                    << b.stats.d84_mm << " mm, cover " << b.stats.clast_cover << ", matrix "
                    << b.stats.matrix_share << ", drawn " << b.stats.clasts_drawn << ", top "
                    << b.stats.clasts_top << ", sphericity " << b.stats.mean_sphericity);
    sum16 += b.stats.d16_mm;
    sum50 += b.stats.d50_mm;
    sum84 += b.stats.d84_mm;
    // One seed's count: a tile holds a few hundred clasts, and the coarse end a few dozen.
    CHECK(b.stats.d50_mm == doctest::Approx(d50).epsilon(0.25));
    // Sneed and Folk's maximum projection sphericity of the drawn clasts.
    CHECK(b.stats.mean_sphericity > 0.60);
    CHECK(b.stats.mean_sphericity < 0.75);
    CHECK(b.stats.clast_cover > 0.75);
  }
  const f64 count = static_cast<f64>(std::size(seeds));
  CHECK(sum50 / count == doctest::Approx(d50).epsilon(0.12));
  CHECK(sum84 / sum50 == doctest::Approx(ratio).epsilon(0.15));
  CHECK(sum50 / sum16 == doctest::Approx(ratio).epsilon(0.15));
}

TEST_CASE("matgen: scree is angular, openwork and coarser than the bed") {
  const Built scree = build(recipe("rock/scree", 512));
  const Built bed = build(recipe("gravel/bed", 512));
  MESSAGE("scree D50 " << scree.stats.d50_mm << " mm, under " << scree.stats.clasts_under
                       << ", matrix " << scree.stats.matrix_share << ", relief "
                       << scree.stats.height_max_m - scree.stats.height_min_m);
  CHECK(scree.stats.d50_mm == doctest::Approx(110.0).epsilon(0.25));
  // Openwork: the gaps go down a clast's thickness and more, where a bed's are filled.
  CHECK(scree.stats.height_max_m - scree.stats.height_min_m >
        2.0 * (bed.stats.height_max_m - bed.stats.height_min_m));
  CHECK(scree.stats.clasts_under > 0);
}

TEST_CASE("matgen: a rock's fresh breaks and lichen are the asked shares, over seeds") {
  for (const u64 seed : {3ull, 5ull, 7ull}) {
    matgen::Set set = recipe("rock/granite", 256, seed);
    set.rock.rind_permille = 700;
    set.rock.lichen_permille = 200;
    const Built b = build(set);
    MESSAGE("seed " << seed << ": fresh " << b.stats.fresh_share << " lichen "
                    << b.stats.lichen_cover);
    CHECK(b.stats.fresh_share == doctest::Approx(0.30).epsilon(0.1));
    // Lichen grows on the rind alone: its cover of the whole is its share of the rind's.
    CHECK(b.stats.lichen_cover == doctest::Approx(0.20 * 0.70).epsilon(0.25));
    set.rock.lichen_permille = 0;
    set.rock.rind_permille = 1000;
    const Built bare = build(set);
    CHECK(bare.stats.lichen_cover == 0.0);
    CHECK(bare.stats.fresh_share == 0.0);
  }
}

TEST_CASE("matgen: a floor's covers follow the Poisson law, over seeds") {
  for (const u64 seed : {3ull, 5ull, 7ull}) {
    const Built turf = build(recipe("ground/turf", 256, seed));
    MESSAGE("seed " << seed << ": moss " << turf.stats.moss_share << " thatch "
                    << turf.stats.thatch_cover << " live " << turf.stats.live_cover);
    CHECK(turf.stats.moss_share == doctest::Approx(0.25).epsilon(0.12));
    CHECK(turf.stats.thatch_cover == doctest::Approx(0.60).epsilon(0.08));
    CHECK(turf.stats.live_cover == doctest::Approx(0.80).epsilon(0.08));
  }
}

TEST_CASE("matgen: every default's colour and roughness means are in its material's range") {
  // Linear albedo (luminance) and roughness ranges by material (generated-material-sets.md, "The
  // ranges the defaults are held to").
  struct Range {
    const char* role;
    f64 albedo_lo, albedo_hi, rough_lo, rough_hi;
  };
  const Range ranges[] = {
      {"gravel/bed", 0.06, 0.20, 0.30, 0.60},    // a wet mixed bed: half the dry 0.2 to 0.4
      {"sand/bar", 0.25, 0.42, 0.80, 0.95},      // dry quartz sand
      {"rock/scree", 0.12, 0.35, 0.80, 0.95},    // dry grey rock, its voids in shade
      {"rock/granite", 0.15, 0.35, 0.70, 0.95},  // weathered granite
      {"rock/granite_wet", 0.06, 0.18, 0.30, 0.60}, {"rock/basalt", 0.03, 0.12, 0.75, 0.95},
      {"rock/limestone", 0.20, 0.50, 0.75, 0.95},   {"rock/sandstone", 0.18, 0.45, 0.75, 0.95},
      {"ground/soil", 0.03, 0.10, 0.85, 0.98},      {"ground/moss", 0.03, 0.10, 0.85, 0.98},
      {"ground/thatch", 0.08, 0.20, 0.70, 0.90},    {"ground/turf", 0.05, 0.14, 0.50, 0.85},
  };
  CHECK(matgen::default_sets().size() == std::size(ranges));
  for (const Range& r : ranges) {
    CAPTURE(r.role);
    const Built b = build(recipe(r.role, 256));
    const f64 albedo = luminance(b.stats.mean_color);
    MESSAGE(std::string(r.role) << ": albedo " << albedo << " (" << b.stats.mean_color[0] << ", "
                                << b.stats.mean_color[1] << ", " << b.stats.mean_color[2]
                                << ") roughness " << b.stats.mean_roughness << " relief mm "
                                << (b.stats.height_max_m - b.stats.height_min_m) * 1000);
    CHECK(albedo >= r.albedo_lo);
    CHECK(albedo <= r.albedo_hi);
    CHECK(b.stats.mean_roughness >= r.rough_lo);
    CHECK(b.stats.mean_roughness <= r.rough_hi);
  }
}

TEST_CASE("matgen: a mip chain keeps the set's mean colour") {
  // The set's far form is its mean: every level of the texture build's chain (a repeating
  // sampler's) has the level-0 mean, in linear light, to a code or so.
  for (const char* role : {"gravel/bed", "rock/granite", "ground/turf"}) {
    CAPTURE(role);
    const Built b = build(recipe(role, 256));
    const u32 n = b.images.size;
    Vector<u8> rgba;
    rgba.resize(n * n * 4);
    for (u32 i = 0; i < n * n; ++i) {
      for (u32 c = 0; c < 3; ++c)
        rgba[i * 4 + c] = b.images.base_color[i * 3 + c];
      rgba[i * 4 + 3] = 255;
    }
    Vector<texture::MipLevel> chain;
    texture::build_mip_chain(rgba, n, n, texture::MipMode::srgb, true, chain, nullptr,
                             texture::EdgeMode::repeat, texture::EdgeMode::repeat);
    REQUIRE(chain.size() == 9);
    f64 base[3] = {0, 0, 0};
    for (u32 level = 0; level < chain.size(); ++level) {
      const texture::MipLevel& m = chain[level];
      f64 mean[3] = {0, 0, 0};
      const u32 texels = m.width * m.height;
      for (u32 i = 0; i < texels; ++i)
        for (u32 c = 0; c < 3; ++c)
          mean[c] += static_cast<f64>(texture::srgb8_to_linear(m.rgba[i * 4 + c]));
      for (u32 c = 0; c < 3; ++c) {
        mean[c] /= texels;
        if (level == 0) base[c] = mean[c];
        CAPTURE(level);
        CHECK(mean[c] == doctest::Approx(base[c]).epsilon(0.03));
      }
    }
  }
}

TEST_CASE("matgen: a recipe reads, refuses and keys") {
  matgen::Set set = recipe("gravel/bed", 256);
  const std::string text = matgen::canonical_json(set);
  matgen::Set back;
  std::string error;
  REQUIRE_MESSAGE(matgen::read_set(text, back, error), error);
  CHECK(matgen::canonical_json(back) == text);
  CHECK(matgen::set_key(back) == matgen::set_key(set));
  // The key moves with any field.
  matgen::Set other = set;
  other.granular.d50_um += 1;
  CHECK(matgen::set_key(other) != matgen::set_key(set));
  other = set;
  other.seed += 1;
  CHECK(matgen::set_key(other) != matgen::set_key(set));
  // Out of range, by field.
  other = set;
  other.texels = 1000;
  CHECK_FALSE(matgen::validate_set(other, error));
  CHECK(error.find("texels") != std::string::npos);
  other = set;
  other.role = "gravel";
  CHECK_FALSE(matgen::validate_set(other, error));
  other = set;
  other.granular.lithologies.clear();
  CHECK_FALSE(matgen::validate_set(other, error));
  other = set;
  other.tile_mm = 10;
  matgen::SetImages images;
  CHECK_FALSE(matgen::build_set(other, images, nullptr, error));
  CHECK_FALSE(matgen::read_set("{\"role\":\"a/b\",\"nonsense\":1}", back, error));
  // The defaults are sorted by role and each is in range.
  const Vector<matgen::Set> sets = matgen::default_sets();
  for (u32 i = 0; i < sets.size(); ++i) {
    CHECK_MESSAGE(matgen::validate_set(sets[i], error), sets[i].role << ": " << error);
    if (i > 0) CHECK(sets[i - 1].role < sets[i].role);
  }
  CHECK_FALSE(matgen::default_set("rock/unobtainium", back));
}

TEST_CASE("matgen: the same bytes from every toolchain") {
  // Taken on MSVC (x86-64-v3, debug); Clang 18 and GCC 13 at v3 and GCC at v2 reproduce them
  // (matgen.md, "Same bytes everywhere"). A change to the generator moves them and bumps
  // k_matgen_version.
  struct Pin {
    const char* role;
    u64 hash;
  };
  const Pin pins[] = {
      {"gravel/bed", 13289521710736930817ull},    {"rock/scree", 1990142896200779824ull},
      {"sand/bar", 6269199880936299696ull},       {"rock/granite", 10653580526231192418ull},
      {"rock/sandstone", 1534237874609981072ull}, {"ground/turf", 4583399645411956302ull},
  };
  for (const Pin& pin : pins) {
    CAPTURE(pin.role);
    const Built b = build(recipe(pin.role, 256));
    const u64 hash = images_hash(b.images);
    MESSAGE(std::string(pin.role) << " " << hash);
    CHECK(hash == pin.hash);
  }
}
