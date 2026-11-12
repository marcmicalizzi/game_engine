// G137: the sward's growth forms (tall and short, sod and tussock), the stature rule that picks
// one per cell from the site, and the meadow's forbs (docs/subsystems/flora.md, "Growth forms by
// site"; docs/experiments/grass-sward.md, "Growth forms"). An entry that names no forms and no
// forbs places what it did before G137, to the bit.
#include <core/hash/hash.h>
#include <core/schema/json_reflect.h>
#include <domain/flora/flora.h>
#include <domain/flora/meadow.h>
#include <domain/flora/sward.h>
#include <domain/flora/understory.h>
#include <domain/geometry/flora_species.h>
#include <domain/scene_gen/scene_gen.h>

#include <doctest/doctest.h>
#include <test_paths.h>
#include <test_temp_dir.h>

#include <bit>
#include <chrono>
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

namespace {
// Everything a solve hands on: the mesh, every level's indices and its card cutoff.
u64 solved_pin(const Solved& s) {
  u64 pin = hash_bytes(s.mesh.positions.data(), s.mesh.positions.size() * sizeof(Vec3));
  pin = hash_combine(pin, hash_bytes(s.mesh.indices.data(), s.mesh.indices.size() * sizeof(u32)));
  for (const auto& level : s.levels.levels) {
    pin = hash_combine(pin, hash_bytes(level.indices.data(), level.indices.size() * sizeof(u32)));
    pin = hash_combine(pin, hash_bytes(&level.alpha_cutoff, sizeof(level.alpha_cutoff)));
  }
  return pin;
}
}  // namespace

TEST_CASE("sward forms: the same form from the same seed twice, another from another seed") {
  // The LOD fit is the whole cost of a solve (four coverage views rasterized about eleven times;
  // a solve with it off takes milliseconds), and three fitted solves of each form took 212 s of a
  // debug suite (T88). So each form's fitted solve runs once and its bytes are pinned, which holds
  // more than a second run in the same process did — every run, every toolchain — and the repeat
  // and the seed's effect are held on the near form, with the fit off. A quarter of each recipe's
  // density (the families' lightest class: the same species, layout and crowns) keeps the fitted
  // solve to a third of the cost again. Pins taken on MSVC (x64, v3); the Linux containers
  // reproduce them, as they do sward_tests.cpp's eight-variant family pin at full density.
  constexpr u64 k_pins[] = {7626587603361884875ull, 8745277581405426160ull, 2444956417515254200ull,
                            6988461357104796185ull};
  for (u32 i = 0; i < 4; ++i) {
    const auto& form = k_forms[i];
    INFO(form.file);
    flora::Sward p = recipe(form.file);
    p.density = .25f;
    const auto fitted = solve(p);
    CHECK(solved_pin(fitted) == k_pins[i]);
    p.fit_lods = false;
    const auto a = solve(p);
    const auto b = solve(p);
    REQUIRE(a.mesh.positions.size() == b.mesh.positions.size());
    CHECK(solved_pin(a) == solved_pin(b));
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
  flora::MeadowSpecies daisy;
  daisy.species = "leucanthemum_vulgare";
  daisy.mesh = "daisy.clusters";
  placements.community.push_back(daisy);
  placements.meadow.wet_reach_m = 6;
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
  // G140: a community member naming no mesh has its stand-in recipe written into the derived-data
  // root, which is this test's own scratch directory.
  test::TempDir scratch;
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
    ctx.ddc = scratch.path();
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
  // The whole read by default (k_no_ring): the near form alone, as before. The streamed inner ring
  // (0) places G136's card twin after every patch, and the forbs after those.
  scene_gen::Placements tile(u8 ring = scene_gen::k_no_ring) {
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

// The ID, mesh and position of every placement: the arithmetic the pick and the jitter do is the
// same bits on every toolchain, where the turn's sines are the C library's (the tilt tests in
// sward_tests.cpp hold the turns; the tilt's arctangent is the sward's own since G138).
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
  // all participate, read whole (the inner ring would add G136's card twin to every patch). The pin
  // was taken from eeb8077c's placement (before G137) on this fixture.
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
  const auto a = first.tile();
  const auto b1 = second.tile(1);
  const auto b0 = second.tile();
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

flora::MeadowSpecies member(const char* species) {
  flora::MeadowSpecies m;
  m.species = species;
  m.mesh = std::string(species) + ".clusters";
  return m;
}

TEST_CASE("sward forms: the community's plants take their covers' share, and the cap a tile's") {
  // G140: one species on a level, dry, open meadow, its niche F 5 and L 7.5: red clover's cover
  // is 0.18 exp(-(0.5 / 1.5)^2 / 2), 0.170 of the cells.
  auto p = four_forms();
  p.community.push_back(member("trifolium_pratense"));
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
  flora::MeadowNiche niche;
  niche.moisture = 5;
  niche.light = 7.5;
  const auto* clover = flora::meadow_species("trifolium_pratense");
  REQUIRE(clover != nullptr);
  u32 cover = 0;
  flora::meadow_cover({&clover, 1}, niche, {&cover, 1});
  CHECK(std::fabs(4096.0 * cover / flora::k_meadow_cover_one - 697) < 3);
  // G167: a cell holds a drift of eight clovers on a disc of r sqrt(8), so the drift is drawn in
  // the cover over the drift's share of the cell (0.251): 0.677 of the cells, and the clover still
  // covers its 0.170 of the ground.
  const u32 scale = flora::meadow_drift_scale(*clover, 1);
  u32 odds = 0;
  flora::meadow_drift_odds({&clover, 1}, {&scale, 1}, niche, {&odds, 1});
  const f64 expected = 4096.0 * odds / flora::k_meadow_cover_one;
  CHECK(flora::meadow_drift_count(*clover, 1) == 8);
  const f64 covered = expected * 8 * 3.14159265358979 * clover->crown_m * clover->crown_m;
  CHECK(std::fabs(covered / (4096.0 * cover / flora::k_meadow_cover_one) - 1) < .01);
  // Expectation, as the density classes are: sigma 30 at this share.
  CHECK(std::fabs(static_cast<f64>(forbs) - expected) < 120);
  // The card ring carries the far form after its cards: red clover's stands for four plants, one in
  // four of the near ring's cells by its own stream, its mesh after the near one.
  u32 far = 0;
  for (const auto& placed : meadow.tile(1).instances)
    if ((placed.id & 0x80000000u) != 0) {
      ++far;
      CHECK(placed.mesh == 9);
      CHECK(placed.handoff == 0);
    }
  CHECK(std::fabs(static_cast<f64>(far) - static_cast<f64>(forbs) / 4) < 50);
  // The streamed inner ring (G136's hand-off by distance): every patch's pair first, then the
  // forbs with no role, so the hand-off never sees a forb and a herb draws at every distance.
  const auto ring0 = meadow.tile(0);
  REQUIRE(ring0.instances.size() == 2 * 4096 + forbs);
  for (u32 i = 0; i < ring0.instances.size(); ++i) {
    const bool forb = (ring0.instances[i].id & 0x80000000u) != 0;
    CHECK(forb == (i >= 2 * 4096));
    if (forb) CHECK(ring0.instances[i].handoff == 0);
  }

  auto capped = p;
  capped.forb_tile_cap = 100;
  Meadow a(&flat, capped), b(&flat, capped);
  b.tile(1);
  const auto ka = a.tile(), kb = b.tile();
  REQUIRE(ka.instances.size() == 4096 + 100);
  REQUIRE(kb.instances.size() == ka.instances.size());
  for (u32 i = 0; i < ka.instances.size(); ++i)
    CHECK(ka.instances[i].id == kb.instances[i].id);

  auto none = p;
  none.community.clear();
  Meadow bare(&flat, none);
  CHECK(bare.tile().instances.size() == 4096);

  // G137's one-mesh forbs are refused by name, and so is a species the table does not hold.
  const auto* gen = scene_gen::GeneratorRegistry::global().find_placement("sward");
  REQUIRE(gen != nullptr);
  for (const bool old : {true, false}) {
    auto bad = p;
    if (old)
      bad.forbs.push_back("oxeye.clusters");
    else
      bad.community.push_back(member("bellis_perennis"));
    Meadow probe(&flat, four_forms());
    void* state = nullptr;
    std::string error;
    CHECK_FALSE(gen->open(schema::to_json(bad), probe.ctx, &state, &error));
    CHECK(error.find(old ? "replaced by community" : "unknown community species") !=
          std::string::npos);
  }
}

TEST_CASE("sward forms: the slope's arctangent is plain arithmetic within 2 ulp of std::atan") {
  // G138: the slope that decides a form is the sward's own arctangent (fdlibm's, sward.cpp), not
  // the C library's. Over the slopes a ground has (gradient 0..16, every 1/4096), the reduction's
  // breakpoints and far out, both signs; the cross-compiler pin is in determinism_tests.cpp.
  const auto ulps = [](f64 a, f64 b) {
    const i64 ia = std::bit_cast<i64>(a), ib = std::bit_cast<i64>(b);
    return static_cast<u64>(ia > ib ? ia - ib : ib - ia);
  };
  u64 worst = 0;
  f64 worst_at = 0;
  const auto check = [&](f64 x) {
    for (const f64 v : {x, -x}) {
      const u64 d = ulps(flora::sward_atan(v), std::atan(v));
      if (d > worst) {
        worst = d;
        worst_at = v;
      }
    }
  };
  for (u32 i = 0; i <= 65536; ++i)
    check(static_cast<f64>(i) * (1.0 / 4096.0));
  for (const f64 edge : {7.0 / 16, 11.0 / 16, 19.0 / 16, 39.0 / 16})
    for (i32 k = -64; k <= 64; ++k)
      check(edge + static_cast<f64>(k) * 1e-15);
  for (f64 x = 1e-30; x < 1e30; x *= 1.37)
    check(x);
  MESSAGE("largest difference from std::atan: ", worst, " ulp at ", worst_at);
  CHECK(worst <= 2u);
  CHECK(flora::sward_atan(0.0) == 0.0);
  CHECK(flora::sward_atan(1.0) == std::atan(1.0));
  CHECK(flora::sward_atan(1e300) == std::atan(1e300));
  CHECK(flora::sward_atan(-1e300) == std::atan(-1e300));
  const f64 nan = std::bit_cast<f64>(0x7ff8000000000000ull);
  CHECK(flora::sward_atan(nan) != flora::sward_atan(nan));
}

// ---- the transition band (G138)
// ------------------------------------------------------------------

namespace {

// The 95 % to 5 % width of each score's boundary on a level, dry, open meadow, where the mosaic
// noise is the only term that varies: `seeds` worlds of one 64 m tile, every patch put at its
// signed distance from the boundary along its transect (the line through it along the score's
// gradient, numerical here and not the placement's own, the zero found by bisection), the share of
// the positive form binned at 0.25 m, and the distances where it crosses 5 % and 95 % read off.
struct Width {
  f64 tall = 0, tussock = 0;
};
Width band_width(f32 band_m, u32 seeds) {
  auto p = four_forms();
  p.stature.band_m = band_m;
  Meadow meadow(&flat, p);
  constexpr f64 k_reach = 6, k_bin = .25;
  constexpr u32 k_bins = 48;  // -6 m .. 6 m
  u32 count[2][k_bins] = {}, positive[2][k_bins] = {};
  const f64 scale = static_cast<f64>(p.stature.mosaic_m);
  for (u32 world = 0; world < seeds; ++world) {
    meadow.ctx.world_seed = 1000 + world;
    const auto out = meadow.tile();
    REQUIRE(out.instances.size() == 4096);
    const u64 base = hash_combine(meadow.ctx.world_seed, p.seed);
    const u64 noise_seed[2] = {hash_combine(base, 0x74616c6cull),
                               hash_combine(base, 0x7475737365ull)};
    const f64 bias[2] = {static_cast<f64>(p.stature.tall_bias),
                         static_cast<f64>(p.stature.tussock_bias)};
    for (const auto& placed : out.instances) {
      const u32 form = form_of(placed);
      const bool is[2] = {form <= 2, form == 2 || form == 4};
      for (u32 axis = 0; axis < 2; ++axis) {
        const auto score = [&](f64 x, f64 z) {
          return bias[axis] +
                 static_cast<f64>(p.stature.noise) *
                     (2 * flora::sward_mosaic_noise(noise_seed[axis], x, z, scale) - 1);
        };
        const f64 x = placed.position.x, z = placed.position.z, s0 = score(x, z);
        const f64 h = 1e-3;
        const f64 gx = (score(x + h, z) - score(x - h, z)) / (2 * h);
        const f64 gz = (score(x, z + h) - score(x, z - h)) / (2 * h);
        const f64 g = std::sqrt(gx * gx + gz * gz);
        if (!(g > 1e-6) || std::fabs(s0) / g > 2 * k_reach) continue;
        const f64 toward = s0 > 0 ? -1 : 1;  // down the gradient where the score is positive
        const f64 ux = toward * gx / g, uz = toward * gz / g;
        const auto same_side = [&](f64 t) {
          return (score(x + t * ux, z + t * uz) > 0) == (s0 > 0);
        };
        f64 inside = 0, outside = -1;
        for (u32 step = 1; step <= static_cast<u32>(k_reach / k_bin); ++step) {
          const f64 t = step * k_bin;
          if (!same_side(t)) {
            outside = t;
            break;
          }
          inside = t;
        }
        if (outside < 0) continue;
        for (u32 i = 0; i < 30; ++i) {
          const f64 mid = .5 * (inside + outside);
          if (same_side(mid))
            inside = mid;
          else
            outside = mid;
        }
        const f64 d = (s0 > 0 ? 1 : -1) * .5 * (inside + outside);
        const i64 bin = static_cast<i64>(std::floor((d + k_reach) / k_bin));
        if (bin < 0 || bin >= static_cast<i64>(k_bins)) continue;
        ++count[axis][bin];
        positive[axis][bin] += is[axis] ? 1u : 0u;
      }
    }
  }
  // The first crossing of a share, interpolated between bin centres, scanning up the distance.
  const auto crossing = [&](u32 axis, f64 share) {
    f64 last_d = -k_reach, last_s = 0;
    for (u32 b = 0; b < k_bins; ++b) {
      if (count[axis][b] < 50) continue;
      const f64 d = -k_reach + (b + .5) * k_bin;
      const f64 s = static_cast<f64>(positive[axis][b]) / count[axis][b];
      if (s >= share)
        return s > last_s ? last_d + (d - last_d) * (share - last_s) / (s - last_s) : d;
      last_d = d;
      last_s = s;
    }
    return k_reach;
  };
  Width w;
  w.tall = crossing(0, .95) - crossing(0, .05);
  w.tussock = crossing(1, .95) - crossing(1, .05);
  return w;
}

}  // namespace

TEST_CASE("sward forms: two forms grade into each other over the band's width, 95 % to 5 %") {
  // smoothstep(1/2 +- u) is 95 % and 5 % at the constant's u: the band's metres map onto the ramp.
  const f64 u = .5 * flora::k_sward_band_span;
  CHECK(std::fabs((.5 + u) * (.5 + u) * (3 - 2 * (.5 + u)) - .95) < 1e-9);
  // The default band, 3 m, measured along transects over twelve worlds' boundaries of both scores,
  // and held to a fifth either side (the band reads distance as score over gradient, which a curved
  // boundary bends; a 0.25 m bin and the interpolation between bins add the rest).
  const auto three = band_width(3, 12);
  MESSAGE("band 3 m: tall against short ", three.tall, " m, tussock against sod ", three.tussock,
          " m");
  CHECK(three.tall > .8 * 3);
  CHECK(three.tall < 1.2 * 3);
  CHECK(three.tussock > .8 * 3);
  CHECK(three.tussock < 1.2 * 3);
  // A band of zero is G137's line: the share jumps within a bin of the boundary.
  const auto zero = band_width(0, 4);
  MESSAGE("band 0 m: ", zero.tall, " m, ", zero.tussock, " m");
  CHECK(zero.tall < .5);
  CHECK(zero.tussock < .5);
}

TEST_CASE("sward forms: a band of zero places G137's forms and a band out of range is refused") {
  // The same tile with and without the band: the band moves only cells near a boundary, and a band
  // of zero moves none (determinism_tests.cpp pins it against G138's first commit, to the bit).
  auto hard = four_forms();
  hard.stature.band_m = 0;
  Meadow a(&flat, hard), b(&flat, four_forms());
  const auto line = a.tile(), band = b.tile();
  REQUIRE(line.instances.size() == band.instances.size());
  u32 moved = 0;
  for (u32 i = 0; i < line.instances.size(); ++i) {
    CHECK(line.instances[i].id == band.instances[i].id);
    moved += line.instances[i].mesh != band.instances[i].mesh ? 1u : 0u;
  }
  CHECK(moved > 0);
  CHECK(moved < line.instances.size() / 4);
  for (const f32 bad : {-1.0f, 21.0f}) {
    auto p = four_forms();
    p.stature.band_m = bad;  // mosaic_m is 20
    MaskGround ground;
    ground.info.cells = 4;
    ground.info.spacing_mm = 1000;
    ground.info.palette = 2;
    scene_gen::Context ctx;
    ctx.tile_size = 4;
    ctx.ground.context = &ground;
    ctx.ground.masks_fn = [](const void* s, scene_gen::GroundMasks& out, std::string*) {
      out = {&k_masks, s};
      return true;
    };
    const auto* gen = scene_gen::GeneratorRegistry::global().find_placement("sward");
    void* state = nullptr;
    std::string error;
    CHECK_FALSE(gen->open(schema::to_json(p), ctx, &state, &error));
    CHECK(error.find("band_m") != std::string::npos);
  }
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

// ---- G140, the meadow community, and G139, the band where a site field draws the boundary -------

namespace {
Vector<const flora::MeadowSpeciesTraits*> whole_table() {
  Vector<const flora::MeadowSpeciesTraits*> all;
  for (const auto& species : flora::meadow_species())
    all.push_back(&species);
  return all;
}
// A transect from a stream's bank (d = 0) up a slope facing the sun: the slope rises to 0.6 and the
// heat load to 1.4 at 80 m.
scene_gen::SiteSample transect(f64 d) {
  scene_gen::SiteSample s;
  s.channel_m = d;
  s.slope = .6 * d / 80;
  s.heat_load = 1 + .4 * d / 80;
  return s;
}
// A valley floor rising along x, z = a x^2, its slope tan(22.5 degrees) at x = 32 m: the stature
// rule's steepness is one half there, so with `tall_bias` 1 the tall forms meet the short ones in
// the middle of the tile, on a boundary the slope draws.
constexpr f64 k_valley_a = .41421356237309503 / 64;
f64 valley(const void*, i64 x_mm, i64) noexcept {
  const f64 x = static_cast<f64>(x_mm) * .001;
  return k_valley_a * x * x;
}
}  // namespace

TEST_CASE("meadow: the community's covers never sum past one, in integers") {
  const auto all = whole_table();
  REQUIRE(all.size() >= 12);
  u32 counts[4] = {};
  for (const auto* species : all)
    ++counts[static_cast<u32>(species->role)];
  CHECK(counts[0] >= 6);  // six to ten forbs, a sedge, a rush and a dwarf shrub
  CHECK(counts[0] <= 10);
  CHECK(counts[1] >= 1);
  CHECK(counts[2] >= 1);
  CHECK(counts[3] >= 1);
  Vector<u32> covers(all.size());
  u32 passed = 0;
  for (u32 f = 4; f <= 48; ++f)
    for (u32 l = 4; l <= 36; ++l) {
      flora::MeadowNiche niche;
      niche.moisture = f * .25;
      niche.light = l * .25;
      const u32 sum =
          flora::meadow_cover({all.data(), all.size()}, niche, {covers.data(), all.size()});
      u64 check = 0;
      for (u32 i = 0; i < all.size(); ++i) {
        check += covers[i];
        CHECK(covers[i] <= static_cast<u32>(all[i]->cover * flora::k_meadow_cover_one) + 1);
      }
      CHECK(check == sum);
      CHECK(sum <= flora::k_meadow_cover_one);
      passed += sum > 0 ? 1u : 0u;
    }
  CHECK(passed > 1000);
}

TEST_CASE("meadow: from a stream's edge up a dry slope the species keep their niches' order") {
  const flora::MeadowRule rule;
  const auto all = whole_table();
  Vector<u32> covers(all.size());
  Vector<f64> moment(all.size(), 0.0), mass(all.size(), 0.0);
  for (u32 step = 0; step <= 160; ++step) {
    const f64 d = step * .5;
    const auto niche = flora::meadow_niche(rule, transect(d));
    CHECK(niche.moisture <= flora::k_meadow_wet_margin);
    flora::meadow_cover({all.data(), all.size()}, niche, {covers.data(), all.size()});
    for (u32 i = 0; i < all.size(); ++i) {
      moment[i] += d * covers[i];
      mass[i] += covers[i];
      // The wet margin's species are absent from the slope; the dry slope's from the bank.
      if (all[i]->moisture >= 8 && d >= 50) CHECK(covers[i] == 0);
      if (all[i]->moisture <= 4 && d == 0) CHECK(covers[i] == 0);
    }
  }
  // Each species' cover-weighted place along the transect: a wetter optimum stands nearer the
  // stream.
  for (u32 a = 0; a < all.size(); ++a) {
    REQUIRE(mass[a] > 0);
    for (u32 b = 0; b < all.size(); ++b)
      if (all[a]->moisture > all[b]->moisture) {
        INFO(all[a]->name, " before ", all[b]->name);
        CHECK(moment[a] / mass[a] < moment[b] / mass[b]);
      }
  }
  // The ends of the gradient as read: the bank is Ellenberg's wet site, the sunny slope's top dry.
  CHECK(flora::meadow_niche(rule, transect(0)).moisture == flora::k_meadow_wet_margin);
  CHECK(flora::meadow_niche(rule, transect(80)).moisture < 3.5);
  // Shade lowers the light, and the shade edge's species gain on the open meadow's.
  const auto* bilberry = flora::meadow_species("vaccinium_myrtillus");
  const auto* daisy = flora::meadow_species("leucanthemum_vulgare");
  REQUIRE(bilberry != nullptr);
  REQUIRE(daisy != nullptr);
  const auto open = flora::meadow_niche(rule, transect(30), 0, 0);
  const auto shaded = flora::meadow_niche(rule, transect(30), 0, 1);
  CHECK(shaded.light < open.light);
  CHECK(flora::meadow_response(*bilberry, shaded) > flora::meadow_response(*bilberry, open));
  CHECK(flora::meadow_response(*daisy, shaded) < flora::meadow_response(*daisy, open));
}

TEST_CASE("meadow: a cell's plant is its cell's from any tile order, and a tile keeps its budget") {
  auto p = four_forms();
  for (const auto& species : flora::meadow_species())
    p.community.push_back(member(std::string(species.name).c_str()));
  REQUIRE(p.community.size() <= 16);
  p.forb_tile_cap = 4096;
  Meadow first(&rising, p, 120), second(&rising, p, 120);
  second.tile(1);
  const auto a = first.tile(0), b = second.tile(0);
  REQUIRE(a.instances.size() == b.instances.size());
  u32 plants = 0, used = 0;
  for (u32 i = 0; i < a.instances.size(); ++i) {
    CHECK(a.instances[i].id == b.instances[i].id);
    CHECK(a.instances[i].mesh == b.instances[i].mesh);
    if ((a.instances[i].id & 0x80000000u) != 0) {
      ++plants;
      used |= 1u << (a.instances[i].mesh - 8);
    }
  }
  CHECK(plants > 400);
  CHECK(std::popcount(used) >= 4);
  // The default budget: 256 plants a tile in the nearest ring; in the next the far forms, each
  // counted as the plants it stands for, up to the same 256; none past it.
  p.forb_tile_cap = 256;
  Meadow capped(&rising, p, 120);
  Vector<u32> thin_of_mesh;  // the far meshes follow the 13 near ones, flowering species only
  for (const auto& species : flora::meadow_species())
    if (flora::meadow_far_thin(species) > 0)
      thin_of_mesh.push_back(flora::meadow_far_thin(species));
  const u32 far_base = 8 + static_cast<u32>(flora::meadow_species().size());
  u32 near = 0, far = 0, stands_for = 0;
  for (const auto& placed : capped.tile(0).instances)
    near += (placed.id & 0x80000000u) != 0 ? 1u : 0u;
  for (const auto& placed : capped.tile(1).instances)
    if ((placed.id & 0x80000000u) != 0) {
      REQUIRE(placed.mesh >= far_base);
      REQUIRE(placed.mesh < far_base + thin_of_mesh.size());
      ++far;
      stands_for += thin_of_mesh[placed.mesh - far_base];
    }
  CHECK(near == 256);
  CHECK(stands_for <= 256);
  CHECK(stands_for > 256 - 4);
  CHECK(far < near);
  MESSAGE("a capped tile: ", near, " plants near, ", far, " far forms standing for ", stands_for);
  CHECK(capped.tile(2).instances.empty());
}

TEST_CASE("meadow: the far form keeps the flowers' colour and coloured area within a tenth") {
  // Each head's coloured area is its petal cards' (n l^2 at the herb's width ratio); a tile's
  // flowers, near against far, per colour, summed over four worlds with no cap.
  // G167: a near drift is `count` plants; its far form one stem carrying `thin` drifts' heads.
  const auto area = [](const flora::MeadowPlant& d) {
    return static_cast<f64>(d.count) * d.herb.flowers * d.herb.petals *
           static_cast<f64>(d.herb.petal_length_m) * static_cast<f64>(d.herb.petal_length_m);
  };
  for (const auto& species : flora::meadow_species()) {
    const auto near = flora::meadow_drift(species, 1), far = flora::meadow_far_drift(species, 1);
    CHECK(far.herb.flower_red == near.herb.flower_red);
    CHECK(far.herb.flower_green == near.herb.flower_green);
    CHECK(far.herb.flower_blue == near.herb.flower_blue);
    if (flora::meadow_far_thin(species) == 0) continue;
    INFO(species.name);
    // The table's own heads: a near drift's petal cards carry them whole (at most 8 a head).
    const f64 table = static_cast<f64>(near.count) * species.flowers * species.petals *
                      species.petal_m * species.petal_m;
    CHECK(std::fabs(area(near) / table - 1) < 1e-3);
    CHECK(near.herb.petals <= flora::k_meadow_drift_petals);
    CHECK(std::fabs(area(far) / (area(near) * flora::meadow_far_thin(species)) - 1) < 1e-3);
  }
  auto p = four_forms();
  for (const auto& species : flora::meadow_species())
    p.community.push_back(member(std::string(species.name).c_str()));
  p.forb_tile_cap = 4096;
  const u32 count = static_cast<u32>(flora::meadow_species().size());
  Vector<f64> area_of_mesh;  // coloured area by mesh index past the patches'
  for (const auto& species : flora::meadow_species())
    area_of_mesh.push_back(area(flora::meadow_drift(species, 1)));
  for (const auto& species : flora::meadow_species())
    if (flora::meadow_far_thin(species) > 0)
      area_of_mesh.push_back(area(flora::meadow_far_drift(species, 1)));
  f64 near_area = 0, far_area = 0;
  u32 near_count = 0, far_count = 0;
  for (u32 w = 0; w < 4; ++w) {
    Meadow meadow(&rising, p, 120);
    meadow.ctx.world_seed = 500 + w;
    for (u8 ring : {u8{0}, u8{1}})
      for (const auto& placed : meadow.tile(ring).instances) {
        if ((placed.id & 0x80000000u) == 0) continue;
        REQUIRE(placed.mesh >= 8);
        const u32 m = placed.mesh - 8;
        REQUIRE(m < area_of_mesh.size());
        (ring == 0 ? near_area : far_area) += area_of_mesh[m];
        if (area_of_mesh[m] > 0) ++(ring == 0 ? near_count : far_count);
        if (ring == 1) CHECK(m >= count);
      }
  }
  MESSAGE("four worlds: near ", near_count, " flowering plants, ", near_area,
          " m2 of petal card; far ", far_count, " far forms, ", far_area, " m2");
  CHECK(far_area > .9 * near_area);
  CHECK(far_area < 1.1 * near_area);
}

TEST_CASE("meadow: the covers and a tile's community are the same bits on every toolchain") {
  // Pins taken on MSVC (x64, v3); the Linux containers reproduce them on GCC and Clang at v3 and
  // GCC at v2. The response's exponential is det::exp, so no C library decides a cover.
  const auto all = whole_table();
  Vector<u32> covers(all.size());
  u64 pin = k_hash_seed;
  for (u32 f = 0; f <= 40; ++f)
    for (u32 l = 0; l <= 20; ++l) {
      flora::MeadowNiche niche;
      niche.moisture = 2 + f * .1875;
      niche.light = 4.5 + l * .2;
      pin = hash_combine(
          pin, flora::meadow_cover({all.data(), all.size()}, niche, {covers.data(), all.size()}));
      pin = hash_combine(pin, hash_bytes(covers.data(), covers.size() * sizeof(u32)));
    }
  CHECK(pin == 2798555847098717520ull);
  auto p = four_forms();
  for (const auto& species : flora::meadow_species())
    p.community.push_back(member(std::string(species.name).c_str()));
  p.forb_tile_cap = 4096;
  // Re-taken for G167 (MSVC, 2026-11-11): a cell draws a drift at its cover over the drift's share
  // of the cell, and the out-of-bloom species have no far form, so both rings' plants moved; the
  // covers' pin above did not.
  Meadow sloped(&rising, p, 120, true);
  CHECK(pin_of(sloped.tile(0)) == 11273859908338696005ull);
  CHECK(pin_of(sloped.tile(1)) ==
        18266861944754905536ull);  // the far ring: the cards and the far forms
  Meadow valley_floor(&valley, p);
  CHECK(pin_of(valley_floor.tile(0)) == 11264372552321339524ull);
}

TEST_CASE("meadow: what a tile's placement costs, with and without the community") {
  // Reported, not held: a 64 m tile of 1 m cells on a slope, the four forms with the default band,
  // and the thirteen species, at activation (never per frame). The flora page quotes the
  // msvc-release figures with the machine's state.
  auto bare = four_forms();
  auto full = bare;
  for (const auto& species : flora::meadow_species())
    full.community.push_back(member(std::string(species.name).c_str()));
  for (const auto* entry : {&bare, &full}) {
    Meadow meadow(&rising, *entry, 120, true);
    for (u8 ring : {u8{0}, u8{1}}) {
      f64 best = 1e30;
      for (u32 run = 0; run < 5; ++run) {
        const auto start = std::chrono::steady_clock::now();
        const auto out = meadow.tile(ring);
        const f64 ms =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        best = std::min(best, ms);
        CHECK(out.instances.size() > 1000);
      }
      const std::string what = entry == &bare ? "forms alone" : "forms and community";
      MESSAGE(what, ", ring ", u32{ring}, ": ", best, " ms a tile (best of 5)");
    }
  }
}

TEST_CASE("meadow: each species' stand-in herb is in range and inside its budget") {
  u32 total = 0;
  for (const auto& species : flora::meadow_species()) {
    const auto herb = flora::meadow_herb(species, 3);
    std::string error;
    CHECK_MESSAGE(flora::herb_in_range(herb, error), species.name, ": ", error);
    assets::MeshData mesh;
    flora::TreeReport report;
    REQUIRE_MESSAGE(flora::solve_herb(herb, mesh, &report, &error), species.name, ": ", error);
    CHECK(report.triangles <= herb.triangle_budget);
    CHECK(report.triangles > 0);
    total += report.triangles;
    u32 far_triangles = 0;
    if (flora::meadow_far_thin(species) > 0) {
      const auto far = flora::meadow_far_herb(species, 3);
      CHECK_MESSAGE(flora::herb_in_range(far, error), species.name, " far: ", error);
      assets::MeshData far_mesh;
      flora::TreeReport far_report;
      REQUIRE_MESSAGE(flora::solve_herb(far, far_mesh, &far_report, &error), species.name, ": ",
                      error);
      far_triangles = far_report.triangles;
      // Fewer triangles a plant it stands for than the near form's.
      CHECK(far_triangles < report.triangles * flora::meadow_far_thin(species));
    }
    MESSAGE(species.name, ": ", report.triangles, " triangles near, ", far_triangles, " far for ",
            flora::meadow_far_thin(species), " plants");
  }
  MESSAGE("the table's stand-ins: ", total, " triangles");
}

TEST_CASE("meadow: a drift is the field kind, every plant one parentless part from its base") {
  // G167: the catalogue's `meadow` is posed as a field, and each drift's plants are blades of it:
  // a part a plant, no parent, its pivot on the ground, every weight in [0, 1] rising with height.
  const auto* kind = geometry::flora_species_info("meadow");
  REQUIRE(kind != nullptr);
  CHECK(kind->motion == geometry::FloraMotionKind::field);
  CHECK_FALSE(kind->casters.traced);
  CHECK(kind->casters.cascades == 1);
  u32 near_total = 0, far_total = 0;
  for (const auto& species : flora::meadow_species()) {
    INFO(species.name);
    for (const bool far : {false, true}) {
      if (far && flora::meadow_far_thin(species) == 0) continue;
      const auto drift =
          far ? flora::meadow_far_drift(species, 1, 3) : flora::meadow_drift(species, 1, 3);
      std::string error;
      REQUIRE_MESSAGE(flora::meadow_plant_in_range(drift, error), error);
      assets::MeshData mesh;
      flora::TreeReport report;
      flora::Motion motion;
      REQUIRE_MESSAGE(flora::solve_meadow_plant(drift, mesh, &report, &error, &motion), error);
      REQUIRE(motion.parts.size() == drift.count);
      REQUIRE(motion.vertex_part.size() == mesh.positions.size());
      REQUIRE(motion.vertex_weight.size() == mesh.positions.size());
      CHECK(motion.structure.empty());
      for (const auto& part : motion.parts) {
        CHECK(part.parent == geometry::k_motion_no_parent);
        CHECK(part.pivot.y == 0.0f);
        CHECK(part.direction.y == 1.0f);
        CHECK(part.length > 0.0f);
        // On the drift's disc, with the quarter-spacing jitter.
        CHECK(std::sqrt(part.pivot.x * part.pivot.x + part.pivot.z * part.pivot.z) <=
              drift.radius_m * 1.25f + 1e-4f);
      }
      for (u32 v = 0; v < mesh.positions.size(); ++v) {
        CHECK(motion.vertex_weight[v] >= 0.0f);
        CHECK(motion.vertex_weight[v] <= 1.0f);
        if (mesh.positions[v].y > 0.05f) CHECK(motion.vertex_weight[v] > 0.0f);
      }
      // A flowering drift keeps its heads in their own primitive, at their colour.
      if (drift.herb.flowers > 0) {
        REQUIRE(mesh.primitives.size() >= 2);
        CHECK(mesh.primitives[1].material == 1);
        CHECK(mesh.materials[1].base_color.x == drift.herb.flower_red);
      }
      if (far)
        far_total += report.triangles;
      else
        near_total += report.triangles;
      const std::string which = std::string(species.name) + (far ? " far: " : " near: ");
      MESSAGE(which, drift.count, " plants: ", report.triangles, " triangles, ",
              drift.herb.flowers * drift.count, " heads");
    }
  }
  MESSAGE("the table's drifts: ", near_total, " triangles near, ", far_total, " far");
  // The container: the flora generator builds `meadow` and names the field kind's species.
  flora::FloraFile file;
  file.species = flora::k_species_meadow;
  file.meadow = flora::meadow_drift(*flora::meadow_species("trifolium_pratense"), 1);
  geometry::ClusterFileData out;
  std::string error;
  REQUIRE_MESSAGE(flora::build_flora_container(file, {}, 0, out, nullptr, error), error);
  CHECK(out.generator_kind == "meadow");
  CHECK(out.motion_parts.size() == file.meadow.count);
  CHECK_FALSE(flora::flora_collides(file));
  CHECK(flora::flora_role(flora::k_species_meadow, 1) == "herb/flower");
}

TEST_CASE("meadow: a forb carries its heads only within its flowering months") {
  // G167: the table's months, first day of the first to last day of the last.
  const auto* caltha = flora::meadow_species("caltha_palustris");
  const auto* clover = flora::meadow_species("trifolium_pratense");
  const auto* rush = flora::meadow_species("juncus_effusus");
  REQUIRE(caltha != nullptr);
  REQUIRE(clover != nullptr);
  REQUIRE(rush != nullptr);
  CHECK(flora::meadow_in_bloom(*caltha, 60));   // 1 March
  CHECK(flora::meadow_in_bloom(*caltha, 151));  // 31 May
  CHECK_FALSE(flora::meadow_in_bloom(*caltha, 152));
  CHECK_FALSE(flora::meadow_in_bloom(*caltha, 59));
  CHECK(flora::meadow_in_bloom(*clover, 200));
  CHECK_FALSE(flora::meadow_in_bloom(*rush, 200));  // no heads drawn at all
  CHECK(flora::meadow_herb(*caltha, 1, 200).flowers == 0);
  CHECK(flora::meadow_herb(*caltha, 1, 100).flowers == caltha->flowers);
  CHECK(flora::meadow_far_thin(*caltha, 200) == 0);
  CHECK(flora::meadow_far_thin(*caltha, 100) > 0);
  // Out of bloom the entry writes no far form for it, and the near drift stands in leaf.
  auto p = four_forms();
  p.community.push_back(member("caltha_palustris"));
  p.community.back().mesh.clear();
  p.meadow.day_of_year = 200;
  Meadow summer(&flat, p, 255);
  u32 far = 0;
  for (const auto& placed : summer.tile(1).instances)
    far += (placed.id & 0x80000000u) != 0 ? 1u : 0u;
  CHECK(far == 0);
  auto bad = p;
  bad.meadow.day_of_year = 367;
  void* state = nullptr;
  std::string error;
  CHECK_FALSE(summer.gen->open(schema::to_json(bad), summer.ctx, &state, &error));
  CHECK(error.find("day_of_year") != std::string::npos);
}

TEST_CASE("sward forms: where the slope draws the boundary the band keeps its width (G139)") {
  // G138's band turned metres into score by the mosaic noise's gradient alone; here the noise is
  // nearly still (0.02) and the slope draws the boundary, so G138 placed a line (its band 3 m times
  // the noise's gradient over the score's, about 0.1 m). The band now reads the whole score's
  // gradient. Measured as the width a smoothstep ramp of the share of tall patches across the
  // columns would have: the sum over 1 m columns of s (1 - s) is 9/70 of the ramp's whole length,
  // and its 5 % to 95 % width `k_sward_band_span` of that.
  auto p = four_forms();
  p.stature.noise = .02f;
  p.stature.tall_bias = 1;
  p.stature.band_m = 3;
  const auto width_of = [&](const flora::SwardPlacements& entry, u32 worlds) {
    f64 sum = 0;
    for (u32 w = 0; w < worlds; ++w) {
      Meadow meadow(&valley, entry);
      meadow.ctx.world_seed = 1000 + w;
      u32 tall[64] = {}, all[64] = {};
      for (const auto& placed : meadow.tile().instances) {
        const i64 column = static_cast<i64>(std::floor(placed.position.x));
        if (column < 0 || column > 63) continue;
        ++all[column];
        tall[column] += form_of(placed) <= 2 ? 1u : 0u;
      }
      for (u32 c = 0; c < 64; ++c)
        if (all[c] > 0) {
          const f64 s = static_cast<f64>(tall[c]) / all[c];
          sum += s * (1 - s);
        }
    }
    return sum / worlds / (9.0 / 70.0) * flora::k_sward_band_span;
  };
  const f64 band = width_of(p, 6);
  MESSAGE("a band of 3 m on a slope's boundary: ", band, " m");
  CHECK(band > .8 * 3);
  CHECK(band < 1.2 * 3);
  auto line = p;
  line.stature.band_m = 0;
  const f64 none = width_of(line, 2);
  MESSAGE("a band of 0 m there: ", none, " m");
  CHECK(none < .5);
}
