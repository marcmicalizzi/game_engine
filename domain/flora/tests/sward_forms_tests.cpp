// G137: the sward's growth forms (tall and short, sod and tussock), the stature rule that picks
// one per cell from the site, and the meadow's forbs (docs/subsystems/flora.md, "Growth forms by
// site"; docs/experiments/grass-sward.md, "Growth forms"). An entry that names no forms and no
// forbs places what it did before G137, to the bit.
#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <domain/flora/flora.h>
#include <domain/flora/sward.h>
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>
#include <test_paths.h>

#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>

using namespace engine;

namespace {

struct Form {
  const char* file;
  const char* species;
  bool tussock;
};
constexpr Form k_forms[] = {{"tall-sod.flora.json", "red_fescue", false},
                            {"tall-tussock.flora.json", "tufted_hairgrass", true},
                            {"short-sod.flora.json", "bent", false},
                            {"short-tussock.flora.json", "mat_grass", true}};

flora::Sward recipe(const char* file) {
  const std::string dir = test::data_path(ENGINE_SOURCE_DIR "/content/test-scenes/meadow",
                                          "content/test-scenes/meadow");
  const std::string path = dir + "/" + file;
  std::ifstream in(path, std::ios::binary);
  REQUIRE_MESSAGE(in.good(), path);
  std::stringstream text;
  text << in.rdbuf();
  flora::FloraFile parsed;
  std::string error;
  REQUIRE_MESSAGE(flora::read_flora_file(text.str(), parsed, error), error);
  REQUIRE(parsed.species == "sward");
  return parsed.sward;
}

struct Solved {
  assets::MeshData mesh;
  flora::Motion motion;
  flora::TreeReport report;
  flora::TreeLevels levels;
};
Solved solve(const flora::Sward& p) {
  Solved s;
  std::string error;
  REQUIRE_MESSAGE(flora::solve_sward(p, s.mesh, &s.report, &error, &s.motion, &s.levels), error);
  return s;
}

}  // namespace

TEST_CASE("sward forms: each recipe's blades stay inside its profile's source range") {
  for (const auto& form : k_forms) {
    INFO(form.file);
    flora::Sward p = recipe(form.file);
    CHECK(p.species == form.species);
    CHECK((p.tussock_spacing_m > 0) == form.tussock);
    p.fit_lods = false;  // the near blades' dimensions do not depend on the fit
    const auto* profile = flora::sward_profile(p.species);
    REQUIRE(profile != nullptr);
    const auto s = solve(p);
    CHECK(s.report.triangles <= p.triangle_budget);
    const u32 stride = 2 * p.segments + 1;  // uncut ribbons: two vertices a row and a tip
    for (u32 i = 0; i < s.report.sprays; ++i) {
      CHECK(s.motion.parts[i].length >= profile->length_min_m - 1e-6f);
      CHECK(s.motion.parts[i].length <= profile->length_max_m + 1e-6f);
      const f32 width = length(s.mesh.positions[i * stride] - s.mesh.positions[i * stride + 1]);
      CHECK(width >= profile->width_min_m - 1e-7f);
      CHECK(width <= profile->width_max_m + 1e-7f);
    }
  }
}

TEST_CASE("sward forms: a tussock's crowns stand at their spacing with ground between them") {
  for (const auto& form : k_forms) {
    if (!form.tussock) continue;
    INFO(form.file);
    flora::Sward p = recipe(form.file);
    p.fit_lods = false;
    const auto s = solve(p);
    const u32 count = s.report.sprays;
    const u32 between =
        static_cast<u32>(static_cast<f64>(count) * static_cast<f64>(p.between_share));
    const f64 cell = static_cast<f64>(p.cell_m), spacing = static_cast<f64>(p.tussock_spacing_m);
    const u32 side = static_cast<u32>(cell / spacing + .5);
    const f64 square = cell / side, radius = static_cast<f64>(p.clump_radius_m);
    // Every crown root lies in its own lattice square; neighbours' roots keep a fifth of what the
    // spacing leaves beyond two radii between them.
    Vector<u32> square_of(count - between);
    Vector<u32> in_square(side * side);
    f64 base_top = 0;
    for (u32 i = 0; i + between < count; ++i) {
      const auto& r = s.motion.parts[i].pivot;
      const u32 sx = static_cast<u32>((static_cast<f64>(r.x) + cell * .5) / square);
      const u32 sz = static_cast<u32>((static_cast<f64>(r.z) + cell * .5) / square);
      REQUIRE(sx < side);
      REQUIRE(sz < side);
      square_of[i] = sz * side + sx;
      ++in_square[sz * side + sx];
      CHECK(r.y >= 0);
      CHECK(r.y <= p.crown_base_m + 1e-6f);
      base_top = std::max(base_top, static_cast<f64>(r.y));
    }
    for (u32 n : in_square)
      CHECK(n > 0);
    if (p.crown_base_m > 0) CHECK(base_top > .8 * static_cast<f64>(p.crown_base_m));
    const f64 gap = .2 * (square - 2 * radius);
    f64 closest = 1e30;
    for (u32 i = 0; i + between < count; ++i)
      for (u32 j = i + 1; j + between < count; ++j) {
        if (square_of[i] == square_of[j]) continue;
        const auto& a = s.motion.parts[i].pivot;
        const auto& b = s.motion.parts[j].pivot;
        const f64 dx = static_cast<f64>(a.x - b.x), dz = static_cast<f64>(a.z - b.z);
        closest = std::min(closest, std::sqrt(dx * dx + dz * dz));
      }
    CHECK(closest >= gap - 1e-6);
    // The turf between the crowns is short: its leaves come from the bottom of the range.
    const auto* profile = flora::sward_profile(p.species);
    for (u32 i = count - between; i < count; ++i)
      CHECK(s.motion.parts[i].length <=
            profile->length_min_m +
                (profile->length_max_m - profile->length_min_m) * p.between_growth + 1e-6f);
  }
}

TEST_CASE("sward forms: the same form from the same seed twice, another from another seed") {
  for (const auto& form : k_forms) {
    INFO(form.file);
    flora::Sward p = recipe(form.file);
    const auto a = solve(p);
    const auto b = solve(p);
    REQUIRE(a.mesh.positions.size() == b.mesh.positions.size());
    CHECK(std::memcmp(a.mesh.positions.data(), b.mesh.positions.data(),
                      a.mesh.positions.size() * sizeof(Vec3)) == 0);
    CHECK(a.mesh.indices == b.mesh.indices);
    CHECK(a.levels.levels[1].alpha_cutoff == b.levels.levels[1].alpha_cutoff);
    p.seed += 1;
    const auto c = solve(p);
    CHECK((c.mesh.positions.size() != a.mesh.positions.size() ||
           std::memcmp(a.mesh.positions.data(), c.mesh.positions.data(),
                       a.mesh.positions.size() * sizeof(Vec3)) != 0));
  }
}

TEST_CASE("sward forms: the new fields roundtrip and out-of-range ones are refused by name") {
  flora::Sward p;
  p.species = "tufted_hairgrass";
  p.tussock_spacing_m = .5f;
  p.crown_base_m = .05f;
  p.between_share = .2f;
  p.between_growth = .3f;
  flora::Sward back;
  schema::ReadContext read;
  REQUIRE(schema::from_json(back, schema::to_json(p), read));
  CHECK(back == p);
  flora::SwardPlacements placements;
  flora::SwardFamily family;
  family.form = flora::SwardForm::short_tussock;
  family.near.push_back("n");
  family.cards.push_back("c");
  placements.families.push_back(family);
  placements.stature.mosaic_m = 12;
  placements.forbs.push_back("daisy.clusters");
  placements.forb_share = .1f;
  flora::SwardPlacements placements_back;
  REQUIRE(schema::from_json(placements_back, schema::to_json(placements), read));
  CHECK(placements_back == placements);

  std::string error;
  CHECK(flora::sward_in_range(p, error));
  const auto refused = [&](flora::Sward bad, const char* field) {
    std::string why;
    CHECK_FALSE(flora::sward_in_range(bad, why));
    CHECK(why.find(field) != std::string::npos);
  };
  flora::Sward bad = p;
  bad.tussock_spacing_m = .1f;
  refused(bad, "tussock_spacing_m");
  bad = p;
  bad.tussock_spacing_m = .4f;
  bad.clump_radius_m = .25f;  // two radii past the spacing
  refused(bad, "tussock_spacing_m");
  bad = {};
  bad.crown_base_m = .05f;  // a sod has no crown to raise
  refused(bad, "crown_base_m");
  bad = {};
  bad.between_share = .2f;
  refused(bad, "between_share");
  bad = p;
  bad.between_share = .95f;
  refused(bad, "between_share");
}

TEST_CASE("sward forms: a recipe that names no tussock solves the bytes it always did") {
  // The default family's pins in sward_tests.cpp hold the sod layout; here a sod recipe with every
  // G137 field spelled out at its default solves the same mesh as one that names none.
  flora::Sward plain;
  plain.species = "bent";
  flora::Sward spelled = plain;
  spelled.tussock_spacing_m = 0;
  spelled.crown_base_m = 0;
  spelled.between_share = 0;
  spelled.between_growth = .9f;  // read only by a tussock's turf
  const auto a = solve(plain);
  const auto b = solve(spelled);
  REQUIRE(a.mesh.positions.size() == b.mesh.positions.size());
  CHECK(std::memcmp(a.mesh.positions.data(), b.mesh.positions.data(),
                    a.mesh.positions.size() * sizeof(Vec3)) == 0);
}

// ---- the placement ------------------------------------------------------------------------------

namespace {

struct MaskGround {
  scene_gen::GroundMaskInfo info;
  Vector<u32> words;
};
const scene_gen::GroundMaskOps k_masks{
    .present = [](const void*, i32, i32) noexcept { return true; },
    .info =
        [](const void* state, scene_gen::GroundMaskInfo& out) noexcept {
          out = static_cast<const MaskGround*>(state)->info;
          return true;
        },
    .tile =
        [](const void* state, i32, i32, std::span<u32> out) noexcept {
          const auto& g = *static_cast<const MaskGround*>(state);
          for (u32 i = 0; i < g.words.size(); ++i)
            out[i] = g.words[i];
          return true;
        }};

constexpr f64 k_tan40 = 0.83909963117728;
constexpr f64 k_tan20 = 0.36397023426620234;
f64 flat(const void*, i64, i64) noexcept { return 1.0; }
f64 steep(const void*, i64 x_mm, i64) noexcept { return k_tan40 * static_cast<f64>(x_mm) * .001; }
f64 rising(const void*, i64 x_mm, i64) noexcept { return k_tan20 * static_cast<f64>(x_mm) * .001; }

// One 64 m tile of 1 m cells over `surface`; palette 1 is the grass, its weight `weight(i)` at mip
// point i, and the wetness byte `wet` everywhere.
struct Meadow {
  MaskGround ground;
  scene_gen::Context ctx;
  void* state = nullptr;
  const scene_gen::PlacementGeneratorDesc* gen = nullptr;
  Meadow(scene_gen::HeightFn surface, const flora::SwardPlacements& p, u8 wet = 0,
         bool graded = false) {
    ground.info.cells = 64;
    ground.info.spacing_mm = 1000;
    ground.info.palette = 3;
    ground.info.tile_max_x = 0;
    ground.info.tile_max_z = 0;
    ground.words.resize(scene_gen::mask_tile_words(64), 0);
    ground.words[0] = 0xffff0201u;  // grass palette 1 in channel 0, shade palette 2 in channel 1
    const u32 points = scene_gen::mask_points(64);
    for (u32 i = 0; i < points; ++i)
      ground.words[i + 1] = graded ? (i * 37u + i / 65u * 11u) % 256u : 255u;
    for (u32 i = 1 + points; i < ground.words.size(); ++i)
      ground.words[i] = wet * 0x01010101u;
    ctx.tile_size = 64;
    ctx.world_seed = 77;
    ctx.ground.context = &ground;
    ctx.ground.surface_fn = surface;
    ctx.ground.masks_fn = [](const void* s, scene_gen::GroundMasks& out, std::string*) {
      out = {&k_masks, s};
      return true;
    };
    gen = scene_gen::GeneratorRegistry::global().find_placement("sward");
    REQUIRE(gen != nullptr);
    std::string error;
    REQUIRE_MESSAGE(gen->open(schema::to_json(p), ctx, &state, &error), error);
  }
  ~Meadow() {
    if (gen != nullptr && state != nullptr) gen->close(state);
  }
  scene_gen::Placements tile(u8 ring = 0) {
    ctx.ring = ring;
    scene_gen::Placements out;
    std::string error;
    REQUIRE_MESSAGE(gen->tile(state, {0, 0}, ctx, out, &error), error);
    return out;
  }
};

flora::SwardFamily family_of(flora::SwardForm form, f32 density, const char* name) {
  flora::SwardFamily f;
  f.form = form;
  f.density = density;
  f.near.push_back(std::string(name) + ".near.clusters");
  f.cards.push_back(std::string(name) + ".cards.clusters");
  return f;
}
// The four forms, one family each at weight 1, in the enum's order: a patch's ring-0 mesh is
// 2 * (form - 1).
flora::SwardPlacements four_forms() {
  flora::SwardPlacements p;
  p.layer = 1;
  p.families.push_back(family_of(flora::SwardForm::tall_sod, 1, "ts"));
  p.families.push_back(family_of(flora::SwardForm::tall_tussock, 1, "tt"));
  p.families.push_back(family_of(flora::SwardForm::short_sod, 1, "ss"));
  p.families.push_back(family_of(flora::SwardForm::short_tussock, 1, "st"));
  return p;
}
u32 form_of(const scene_gen::Placement& placed) { return placed.mesh / 2 + 1; }

// The ID, mesh and position of every placement: the arithmetic the pick and the jitter do is the same
// bits on every toolchain, where the turn's sines and arctangents are the C library's (the tilt tests
// in sward_tests.cpp hold the turns).
u64 pin_of(const scene_gen::Placements& out) {
  u64 pin = k_hash_seed;
  for (const auto& placed : out.instances) {
    pin = hash_combine(pin, placed.id);
    pin = hash_combine(pin, placed.mesh);
    pin = hash_combine(pin, hash_bytes(&placed.position, sizeof(placed.position)));
  }
  return hash_combine(pin, out.instances.size());
}

}  // namespace

TEST_CASE("sward forms: an entry naming no forms and no forbs places what it did, to the bit") {
  // Four density classes over a graded mask on a 20 degree slope: the pick, the jitter and the tilt
  // all participate. The pin was taken from eeb8077c's placement (before G137) on this fixture.
  flora::SwardPlacements p;
  p.layer = 1;
  p.families.push_back(family_of(flora::SwardForm::any, .25f, "a"));
  p.families.push_back(family_of(flora::SwardForm::any, .5f, "b"));
  p.families.push_back(family_of(flora::SwardForm::any, .75f, "c"));
  p.families.push_back(family_of(flora::SwardForm::any, 1, "d"));
  Meadow meadow(&rising, p, 200, true);
  const auto out = meadow.tile();
  CHECK(out.instances.size() > 1000);
  u32 used = 0;
  for (const auto& placed : out.instances)
    used |= 1u << placed.mesh;
  CHECK((used & 0x15u) == 0x15u);  // three classes' near meshes at least
  CHECK(pin_of(out) == 10093430337790593354ull);
}

TEST_CASE("sward forms: a wet cell grows a tall tussock and a steep dry one a short form") {
  {
    Meadow wet(&flat, four_forms(), 255);
    const auto out = wet.tile();
    REQUIRE(out.instances.size() == 4096);
    for (const auto& placed : out.instances)
      CHECK(form_of(placed) == 2);
  }
  {
    Meadow dry(&steep, four_forms(), 0);
    const auto out = dry.tile();
    REQUIRE(out.instances.size() == 4096);
    u32 tussock = 0;
    for (const auto& placed : out.instances) {
      CHECK(form_of(placed) >= 3);
      tussock += form_of(placed) == 4 ? 1u : 0u;
    }
    CHECK(tussock > 0);  // the habit is still the mosaic's
    CHECK(tussock < 4096);
  }
  {
    // A form the entry has no family for falls to the nearest it has: wet ground with no tall
    // tussock family grows the tall sod.
    auto p = four_forms();
    p.families.erase(p.families.begin() + 1);
    Meadow wet(&flat, p, 255);
    for (const auto& placed : wet.tile().instances)
      CHECK(placed.mesh == 0);
  }
}

TEST_CASE("sward forms: the mosaic is patches of the noise's scale, the same in any order") {
  Meadow first(&flat, four_forms()), second(&flat, four_forms());
  const auto a = first.tile(0);
  const auto b1 = second.tile(1);
  const auto b0 = second.tile(0);
  REQUIRE(a.instances.size() == 4096);
  REQUIRE(b0.instances.size() == 4096);
  u32 count[5] = {}, alike = 0, pairs = 0;
  for (u32 i = 0; i < 4096; ++i) {
    CHECK(a.instances[i].mesh == b0.instances[i].mesh);
    CHECK(a.instances[i].mesh + 1 == b1.instances[i].mesh);  // ring 1: the same form's cards
    ++count[form_of(a.instances[i])];
    if (i % 64 != 63) {
      ++pairs;
      alike += form_of(a.instances[i]) == form_of(a.instances[i + 1]) ? 1u : 0u;
    }
  }
  // A level, dry, open meadow: tall and short both, sod and tussock both, and neighbours mostly
  // alike, since the noise's lattice is 20 m and a cell 1 m.
  CHECK(count[1] + count[2] > 400);
  CHECK(count[3] + count[4] > 400);
  CHECK(count[2] + count[4] > 100);
  CHECK(count[1] + count[3] > 400);
  CHECK(alike > pairs * 8 / 10);
}

TEST_CASE("sward forms: the forbs take their share of the patches, and the cap a tile's") {
  auto p = four_forms();
  p.forbs.push_back("oxeye.clusters");
  p.forb_share = .33f;
  p.forb_tile_cap = 4096;
  Meadow meadow(&flat, p);
  const auto out = meadow.tile();
  u32 patches = 0, forbs = 0;
  for (const auto& placed : out.instances) {
    if ((placed.id & 0x80000000u) != 0) {
      ++forbs;
      CHECK(placed.mesh == 8);  // after the four families' near and card lists
      CHECK(std::fabs(static_cast<f64>(rotate(placed.rotation, Vec3::unit_y()).y) - 1) < 1e-6);
      CHECK(placed.position.y == 1.0);
    } else {
      ++patches;
    }
  }
  REQUIRE(patches == 4096);
  // Expectation, as the density classes are: 0.33 of 4096 is 1352, sigma 30.
  CHECK(forbs > 1352 - 120);
  CHECK(forbs < 1352 + 120);
  // The nearest ring alone: the card ring carries none.
  CHECK(meadow.tile(1).instances.size() == 4096);

  auto capped = p;
  capped.forb_tile_cap = 100;
  Meadow a(&flat, capped), b(&flat, capped);
  b.tile(1);
  const auto ka = a.tile(0), kb = b.tile(0);
  REQUIRE(ka.instances.size() == 4096 + 100);
  REQUIRE(kb.instances.size() == ka.instances.size());
  for (u32 i = 0; i < ka.instances.size(); ++i)
    CHECK(ka.instances[i].id == kb.instances[i].id);

  auto none = p;
  none.forb_share = 0;
  Meadow bare(&flat, none);
  CHECK(bare.tile().instances.size() == 4096);
}

TEST_CASE("sward forms: an entry mixing named and unnamed forms is refused") {
  auto p = four_forms();
  p.families.push_back(family_of(flora::SwardForm::any, 1, "x"));
  scene_gen::Context ctx;
  ctx.tile_size = 4;
  MaskGround ground;
  ground.info.cells = 4;
  ground.info.spacing_mm = 1000;
  ground.info.palette = 2;
  ctx.ground.context = &ground;
  ctx.ground.masks_fn = [](const void* s, scene_gen::GroundMasks& out, std::string*) {
    out = {&k_masks, s};
    return true;
  };
  const auto* gen = scene_gen::GeneratorRegistry::global().find_placement("sward");
  void* state = nullptr;
  std::string error;
  CHECK_FALSE(gen->open(schema::to_json(p), ctx, &state, &error));
  CHECK(error.find("every family names a form") != std::string::npos);
}
