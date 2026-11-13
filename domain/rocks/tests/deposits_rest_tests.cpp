// G190: where a pile comes to rest, and that it never ends in a cut (docs/subsystems/rocks.md,
// "Where a pile comes to rest"; docs/experiments/rockfall-deposits.md, G190).
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/rocks/deposits.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>

using namespace engine;
using namespace engine::rocks;
namespace {
// A face at 63 degrees (rise 2 over run 1) standing `face_m` over the top of a transit slope at
// `slope_deg` that falls `drop_m` to a meadow falling gently toward -x from x = 30 m: the forest's
// massif foot, a face whose foot grades through 35 to 50 degrees before the ground can hold debris.
// A shallow gully at z = 16 m makes the contours of the face bend, so a cone stands beside the
// sheet.
struct Graded {
  f64 slope_deg;
  f64 drop_m;
  f64 face_m;
  bool meadow;  // false: the transit slope runs on to the region's edge, with nowhere to rest
};
f64 tan_deg(f64 d) { return std::tan(d * 3.14159265358979323846 / 180.0); }
f64 graded(const void* data, i64 x_mm, i64 z_mm) noexcept {
  const auto& g = *static_cast<const Graded*>(data);
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  const f64 t = tan_deg(g.slope_deg);
  const f64 x0 = g.meadow ? 30.0 : -1000.0;
  const f64 x1 = g.meadow ? x0 + g.drop_m / t : 40.0;
  const f64 top = t * (x1 - x0);
  f64 h;
  if (x < x0)
    h = .08 * (x - x0);
  else if (x < x1)
    h = t * (x - x0);
  else
    h = top + std::min(2.0 * (x - x1), g.face_m);
  const f64 u = (z - 16.0) / 4.0;
  if (x >= x1 && u * u < 1)
    h -= 3.0 * (1 - u * u) * (1 - u * u) * std::clamp((x - x1) / 4.0, 0.0, 1.0);
  return h;
}
DepositLayer layer(u64 seed) {
  DepositLayer p;
  p.seed = seed;
  p.region_min = {0, 0};
  p.region_max = {80, 32};
  p.spacing_m = .5f;
  p.patch.fragments = 8;
  p.patch.fragment_m = .2f;
  p.patch.triangle_budget = 20000;
  p.sediment_depth_m = 0;
  p.blocks_per_100m2 = 0;
  return p;
}
// The step at the outline: for a talus sample beside a bare one, how far its top stands over the
// bare sample's ground beyond what a surface at the repose angle falls over the distance between
// them. A surface no steeper than the repose that meets the ground between the two stands at most
// that much over it, so the step is at most zero there; a surface cut by the run-out or a pile's
// height stands the cut's height over it.
f64 worst_step(const DepositPlan& plan, f64 repose) {
  f64 worst = -1e9;
  const i64 w = plan.width, h = plan.depth;
  const f64 s = static_cast<f64>(plan.spacing_mm) / 1000.0;
  for (i64 z = 0; z < h; ++z)
    for (i64 x = 0; x < w; ++x) {
      const u32 i = static_cast<u32>(z * w + x);
      if (plan.thickness[i] <= 0.0f || plan.coarse[i] <= 0.0f) continue;
      for (i64 dz = -1; dz <= 1; ++dz)
        for (i64 dx = -1; dx <= 1; ++dx) {
          const i64 xx = x + dx, zz = z + dz;
          if ((dx == 0 && dz == 0) || xx < 0 || zz < 0 || xx >= w || zz >= h) continue;
          const u32 j = static_cast<u32>(zz * w + xx);
          if (plan.thickness[j] > 0.0f) continue;
          const f64 d = s * std::sqrt(static_cast<f64>(dx * dx + dz * dz));
          const f64 top = plan.ground[i] + static_cast<f64>(plan.thickness[i]);
          worst = std::max(worst, top - plan.ground[j] - repose * d);
        }
    }
  return worst;
}
// The most a pile may stand over the bare ground past its outline (deposits.cpp, `k_cut_m`: the
// lowest stand of the bed's coarsest clast), plus the edge field's noise, which may lift the first
// bare ring by up to `k_edge_amp`.
constexpr f64 k_bound = 0.22 + k_edge_amp;
constexpr f64 k_repose = 0.70020753820970977;  // tan 35 degrees
}  // namespace

TEST_CASE("deposits: a pile stands where rock comes to rest and never ends in a cut") {
  // The forest's foot (asks-audit gaps 41 and 42): below the face the ground falls at 35, 42 or
  // 50 degrees for 12 m before it can hold debris. Before G190 the foot was the first sample under
  // `source_deg`, the pile's surface stood over ground steeper than itself, and the flood cut it at
  // its height: a wall metres high along the bed's downslope edge.
  // 54 and 60 degrees: the forest's second world, whose median bed stood on 54-degree ground.
  for (f64 slope : {35.0, 42.0, 50.0, 54.0, 60.0})
    for (u64 seed : {1ull, 7ull, 23ull}) {
      CAPTURE(slope);
      CAPTURE(seed);
      Graded g{slope, 12.0, 20.0, true};
      scene_gen::Ground ground;
      ground.surface_fn = &graded;
      ground.context = &g;
      DepositPlan plan;
      std::string error;
      REQUIRE(plan_deposits(layer(seed), ground, plan, error));
      REQUIRE_FALSE(plan.piles.empty());
      const f64 step = worst_step(plan, k_repose);
      f64 held = 0, asked = 0, refused = 0, deepest = 0;
      for (const auto& pile : plan.piles) {
        held += pile.held_m3;
        asked += pile.volume_m3;
      }
      for (const auto& r : plan.refused)
        refused += r.volume_m3;
      // Every kept pile's foot is on ground that can hold it: gentler than the repose.
      u32 steep_feet = 0;
      for (const auto& pile : plan.piles) {
        const u32 f = pile.foot, x = f % plan.width;
        const f64 left = x > 0 ? plan.ground[f] - plan.ground[f - 1] : 0.0;
        if (left / .5 >= k_repose) ++steep_feet;
      }
      // The ground's slope under the bed, by area: the median talus sample's, central differences.
      Vector<f64> under;
      for (u32 i = 0; i < plan.thickness.size(); ++i) {
        deepest = std::max(deepest, static_cast<f64>(plan.thickness[i]));
        const u32 x = i % plan.width;
        if (plan.thickness[i] <= 0.0f || plan.coarse[i] <= 0.0f || x == 0 || x + 1 == plan.width)
          continue;
        under.push_back(std::atan(std::abs(plan.ground[i + 1] - plan.ground[i - 1]) / 1.0) * 180.0 /
                        3.14159265358979323846);
      }
      std::sort(under.begin(), under.end());
      const f64 median = under.empty() ? 0.0 : under[under.size() / 2];
      MESSAGE("slope ", slope, " seed ", seed, ": piles ", plan.piles.size(), ", moved ",
              plan.moved, ", refused ", plan.refused.size(), " (", refused, " m3), held ", held,
              " m3 of ", asked, " asked, deepest ", deepest, " m, median ground under the bed ",
              median, " degrees, worst outline step ", step, " m, worst cut ", plan.worst_cut_m,
              " m, rounds ", plan.rounds);
      CHECK(step <= k_bound);
      // The bed's median ground is the transit slope's below 54 degrees, where the wedge leans up
      // it and meets it, and the meadow's from 54 up, where the transit slope sheds itself and
      // the bed lies on the meadow below it; never ground steeper than the slope it leans on.
      CHECK(median <= slope + 0.5);
      CHECK(plan.worst_cut_m <= 0.22 + 1e-9);
      CHECK(steep_feet == 0);
      // The face's fall is stood, not refused: there is a meadow below to hold it, and what is
      // refused is only what fell off the region's two ends (no pile is refused for a cut). A pile
      // holds what it was asked to, not a wedge that fills the transit slope (the base held six
      // times it); G143's planar face holds 5% over, and the bound is a quarter, for the wedge on a
      // meadow that falls away from it. On a back slope at the repose itself the pile's surface
      // lies parallel to it and holds only its toe, a third of what it was asked (rocks.md, "Where
      // a pile comes to rest"), so the lower bound is a quarter.
      for (const auto& r : plan.refused)
        CHECK(r.cut_m == 0.0);
      CHECK(held <= 1.25 * asked);
      CHECK(held >= 0.25 * asked);
    }
}

namespace {
// A face standing on a narrow bench at the brink of a 45-degree drop to the meadow: the bench is
// gentle enough to hold debris, but a pile on it would drape the drop below it.
f64 benched(const void* data, i64 x_mm, i64) noexcept {
  const f64 bench = *static_cast<const f64*>(data);
  const f64 x = static_cast<f64>(x_mm) / 1000.0;
  if (x < 30) return .08 * (x - 30);
  if (x < 40) return x - 30;
  if (x < 40 + bench) return 10 + .05 * (x - 40);
  return 10 + .05 * bench + std::min(2.0 * (x - 40 - bench), 20.0);
}

// A hillside at 20 degrees falling toward -x, before and after a stream's channel is cut down it
// along z = 16 m: a 4 m floor 3 m deep between banks at 56 degrees (the forest's 45-56).
f64 hillside(const void*, i64 x_mm, i64) noexcept { return .364 * static_cast<f64>(x_mm) / 1000.0; }
f64 channelled(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 d = std::abs(static_cast<f64>(z_mm) / 1000.0 - 16.0);
  const f64 cut = d < 2.0 ? 3.0 : std::max(0.0, 3.0 - (d - 2.0) * 1.4826);
  return .364 * static_cast<f64>(x_mm) / 1000.0 - cut;
}
bool hillside_before(void*, scene_gen::Ground& out) {
  out = {};
  out.surface_fn = &hillside;
  return true;
}
}  // namespace

TEST_CASE("deposits: a channel cut through a slope sheds nothing") {
  // The forest's second world: the channels' cut banks were read as rockfall sources and 649 piles
  // were shed into a stream's bed. A source is a face of the land, so a sample sheds only where the
  // ground before its edits is steep too.
  scene_gen::Ground cut;
  cut.surface_fn = &channelled;
  scene_gen::Ground before;
  before.surface_fn = &hillside;
  DepositPlan with_banks, without;
  std::string error;
  REQUIRE(plan_deposits(layer(1), cut, with_banks, error));
  REQUIRE(plan_deposits(layer(1), cut, without, error, &before));
  auto talus = [](const DepositPlan& plan) {
    u32 count = 0;
    for (u32 i = 0; i < plan.thickness.size(); ++i)
      count += plan.thickness[i] > 0.0f && plan.coarse[i] > 0.0f ? 1u : 0u;
    return count;
  };
  MESSAGE("banks as sources: ", with_banks.piles.size(), " piles, ", talus(with_banks),
          " talus samples; read against the ground before the cut: ", without.piles.size(), ", ",
          talus(without));
  CHECK_FALSE(with_banks.piles.empty());  // the defect, without the ground before the cut
  CHECK(without.piles.empty());
  CHECK(talus(without) == 0);
  // Through the generator: a host's `Context::uncut()` is what it reads.
  test::TempDir scratch;
  const std::string dir = scratch.path();
  scene_gen::Context c;
  c.dir = dir;
  c.ddc = dir;
  c.ground = cut;
  c.tile_size = 16;
  c.uncut_ground.fn = &hillside_before;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  void* state = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(layer(5)), c, &state, &error), error);
  scene_gen::Placements whole;
  REQUIRE(desc->expand(state, c, whole, &error));
  desc->close(state);
  CHECK(whole.instances.empty());
}

TEST_CASE("deposits: a pile on a bench at a brink is moved on down to the meadow below") {
  // The pile the bench's foot would stand ends in a cut over the drop (the flood stops it at its
  // own height): it is moved on over the brink to the meadow, and stands there whole.
  for (f64 bench : {2.0, 4.0}) {
    CAPTURE(bench);
    scene_gen::Ground ground;
    ground.surface_fn = &benched;
    ground.context = &bench;
    DepositPlan plan;
    std::string error;
    REQUIRE(plan_deposits(layer(1), ground, plan, error));
    f64 held = 0, asked = 0, refused = 0;
    for (const auto& pile : plan.piles) {
      held += pile.held_m3;
      asked += pile.volume_m3;
    }
    for (const auto& r : plan.refused)
      refused += r.volume_m3;
    const f64 step = worst_step(plan, k_repose);
    MESSAGE("bench ", bench, " m: piles ", plan.piles.size(), ", moved ", plan.moved, ", refused ",
            plan.refused.size(), " (", refused, " m3), held ", held, " m3 of ", asked,
            " asked, worst outline step ", step, " m, worst cut ", plan.worst_cut_m, " m, rounds ",
            plan.rounds);
    // Every pile the bench would have stood is moved, none refused; the moved pile against the
    // 45-degree drop holds less than it was asked (rocks.md, "Where a pile comes to rest": 40% on
    // a 2 m bench, 57% on a 4 m one), never more (it held five times it on the bench).
    CHECK(plan.moved > 0);
    CHECK(plan.refused.empty());
    CHECK(step <= k_bound);
    CHECK(plan.worst_cut_m <= 0.22 + 1e-9);
    CHECK(held >= 0.25 * asked);
    CHECK(held <= 1.25 * asked);
  }
}

TEST_CASE("deposits: a fall with nowhere to rest is refused and the plan says how much and where") {
  // The transit slope runs on past the region's edge: no ground in the region can hold the face's
  // fall, so nothing is stood and every pile and fall is recorded as refused.
  for (f64 slope : {42.0, 50.0}) {
    CAPTURE(slope);
    Graded g{slope, 0.0, 20.0, false};
    scene_gen::Ground ground;
    ground.surface_fn = &graded;
    ground.context = &g;
    DepositPlan plan;
    std::string error;
    REQUIRE(plan_deposits(layer(1), ground, plan, error));
    f64 refused = 0;
    for (const auto& r : plan.refused) {
      refused += r.volume_m3;
      CHECK(r.x_mm >= 0);
      CHECK(r.x_mm <= 80000);
    }
    MESSAGE("slope ", slope, ": refused ", plan.refused.size(), " (", refused, " m3), kept piles ",
            plan.piles.size(), ", worst outline step ", worst_step(plan, k_repose), " m");
    CHECK_FALSE(plan.refused.empty());
    CHECK(refused > 0.0);
    CHECK(worst_step(plan, k_repose) <= k_bound);
  }
}

TEST_CASE("deposits: a tile's patches arrive with it, none past far_rings, and one plan a load") {
  // G191 (the forest's gap 43): nothing is resident from the scene's read and nothing is written;
  // a tile hands its own patches' recipes, and a ring at or past `far_rings` draws none. T120 (gap
  // 44): the reader's open and the world's consumer's open of the same layer on the same ground
  // plan once; another seed plans again.
  test::TempDir scratch;
  const std::string dir = scratch.path();
  Graded g{42.0, 12.0, 20.0, true};
  scene_gen::Context c;
  c.dir = dir;
  c.ddc = dir;
  c.ground.surface_fn = &graded;
  c.ground.context = &g;
  c.tile_size = 16;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  CHECK(desc->meshes == nullptr);
  CHECK(desc->tile_meshes);
  DepositLayer p = layer(991);
  p.far_rings = 2;
  std::string error;
  const u64 made = deposit_plans_made();
  void* first = nullptr;
  void* second = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &first, &error), error);
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &second, &error), error);
  CHECK(deposit_plans_made() == made + 1);
  scene_gen::Placements whole;
  REQUIRE(desc->expand(first, c, whole, &error));
  REQUIRE_FALSE(whole.instances.empty());
  CHECK(whole.meshes.size() == whole.instances.size());
  // Nothing written: the recipes' directory under the root (T119) holds no file.
  auto files = [&] {
    std::error_code ec;
    u32 n = 0;
    if (std::filesystem::is_directory(dir, ec))
      for (const auto& f : std::filesystem::recursive_directory_iterator(dir, ec))
        n += f.is_regular_file() ? 1u : 0u;
    return n;
  };
  CHECK(files() == 0);
  // The tile under the transit slope's foot (x 30 m) in each ring.
  u32 near = 0, far = 0;
  for (u8 ring = 0; ring < 3; ++ring) {
    c.ring = ring;
    for (i32 tz = 0; tz < 2; ++tz)
      for (i32 tx = 0; tx < 5; ++tx) {
        scene_gen::Placements one;
        REQUIRE(desc->tile(second, {tx, tz}, c, one, &error));
        CHECK(one.meshes.size() == one.instances.size());
        for (const auto& instance : one.instances) {
          REQUIRE(instance.mesh < one.meshes.size());
          CHECK_FALSE(one.meshes[instance.mesh].recipe.empty());
        }
        if (ring < 2) near += one.instances.size();
        if (ring >= 2) far += one.instances.size();
        CHECK(one.kind == (ring < 2 ? 0 : 1));
      }
  }
  CHECK(near == 2 * whole.instances.size());
  CHECK(far == 0);
  CHECK(files() == 0);
  desc->close(first);
  desc->close(second);
  // A close and the next open of the same entry: the plan let go is kept until another is made.
  void* again = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &again, &error), error);
  CHECK(deposit_plans_made() == made + 1);
  desc->close(again);
  p.seed = 992;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &again, &error), error);
  CHECK(deposit_plans_made() == made + 2);
  desc->close(again);
}

namespace {
// A 16 m patch at 0.5 m samples, its deposit everywhere but where `bare` says, at the forest's
// 128,000 triangles a patch, with `fragments` loose stones and `blocks` shed blocks.
DepositPatch dense_patch(u32 fragments, u32 blocks, bool bare_half = false) {
  DepositPatch p;
  p.width = 33;
  p.depth = 33;
  p.spacing_m = .5f;
  p.triangle_budget = 128000;
  p.fragments = fragments;
  p.fragment_m = .25f;
  p.origin_mm = {64000, 32000};
  p.seed = 77;
  for (u32 z = 0; z < p.depth; ++z)
    for (u32 x = 0; x < p.width; ++x) {
      p.ground.push_back(-.1f * static_cast<f32>(x) * .5f);
      const bool bare = bare_half && x < 20;
      p.thickness.push_back(bare ? 0.0f
                                 : 1.0f + .5f * static_cast<f32>((x * 7 + z * 3) % 5) / 5.0f);
      p.coarse.push_back(static_cast<f32>(x) / 32.0f);
    }
  for (u32 b = 0; b < blocks; ++b) {
    DepositBlock block;
    block.x_m = .5f + static_cast<f32>((b * 37) % 31) * .5f;
    block.z_m = .5f + static_cast<f32>((b * 53) % 31) * .5f;
    block.y_m = 1.2f;
    block.half[0] = .35f + .1f * static_cast<f32>(b % 3);
    block.half[1] = .2f;
    block.half[2] = .3f;
    block.seed = 1000 + b;
    p.blocks.push_back(block);
  }
  return p;
}
}  // namespace

TEST_CASE("deposits: a dense patch is handed as pieces that keep what it holds") {
  // G191 (rocks.md, "A patch under the cluster limit"): a patch whose finest form holds more than
  // k_deposit_piece_triangles is handed as pieces, each an ordinary patch over a rectangle of its
  // samples; together they hold every fragment and block of the whole, at the whole's steps and
  // rungs, so the pieces' finest forms hold the whole's triangles plus the walls at their cuts and
  // nothing less; each piece builds under a streamed host's cluster limit without building again.
  struct Case {
    u32 fragments;
    u32 blocks;
    bool bare_half;
  };
  for (const Case k : {Case{24, 0, false}, Case{96, 60, false}, Case{320, 60, false},
                       Case{600, 70, false}, Case{96, 60, true}}) {
    CAPTURE(k.fragments);
    CAPTURE(k.blocks);
    CAPTURE(k.bare_half);
    const DepositPatch whole = dense_patch(k.fragments, k.blocks, k.bare_half);
    std::string error;
    u64 triangles = 0;
    REQUIRE_MESSAGE(deposit_patch_triangles(whole, triangles, error), error);
    Vector<DepositPiece> pieces;
    deposit_patch_pieces(whole, pieces);
    REQUIRE_FALSE(pieces.empty());
    CHECK((pieces.size() > 1) == (triangles > k_deposit_piece_triangles));
    u64 fragments = 0, blocks = 0, sum = 0, cut_walls = 0;
    for (const DepositPiece& piece : pieces) {
      REQUIRE_MESSAGE(deposit_patch_fits(piece.patch, error), error);
      u64 t = 0;
      REQUIRE(deposit_patch_triangles(piece.patch, t, error));
      CHECK(t <= k_deposit_piece_triangles);
      sum += t;
      fragments += piece.patch.fragments;
      blocks += piece.patch.blocks.size();
      // Its corner is the whole's corner plus its samples, so its clasts are the whole's.
      CHECK(piece.patch.origin_mm[0] == whole.origin_mm[0] + piece.sample_x * 500);
      CHECK(piece.patch.origin_mm[1] == whole.origin_mm[1] + piece.sample_z * 500);
      // The walls a cut adds: one side of each of its interior borders, at most 11 triangles an
      // edge (five steps a spacing).
      const u32 inner = (piece.sample_x > 0) + (piece.sample_z > 0) +
                        (piece.sample_x + piece.patch.width < whole.width) +
                        (piece.sample_z + piece.patch.depth < whole.depth);
      cut_walls += static_cast<u64>(inner) * 32 * 6 * 2;
    }
    MESSAGE(pieces.size(), " pieces: the whole's finest form ", triangles, " triangles, the pieces' ",
            sum);
    CHECK(fragments == whole.fragments);
    CHECK(blocks == whole.blocks.size());
    CHECK(sum >= triangles);
    CHECK(sum <= triangles + cut_walls);
    // The pieces are a function of the patch.
    Vector<DepositPiece> again;
    deposit_patch_pieces(whole, again);
    REQUIRE(again.size() == pieces.size());
    for (u32 i = 0; i < pieces.size(); ++i)
      CHECK(deposit_source_hash(again[i].patch) == deposit_source_hash(pieces[i].patch));
  }
}

TEST_CASE("deposits: each piece of a patch at the forest's density builds under the cluster "
          "limit, and a container over it is refused by name") {
  // G191: the build is what holds the bound. Every piece of the densest case builds under 4,096
  // clusters at its first build (nothing is built again at half its budget), and a container that
  // stays over a limit is refused with a sentence naming the patch and its count.
  const DepositPatch whole = dense_patch(600, 70);
  Vector<DepositPiece> pieces;
  deposit_patch_pieces(whole, pieces);
  REQUIRE(pieces.size() > 1);
  const u64 reduced = deposit_builds_reduced();
  std::string error;
  u64 most = 0;
  for (const DepositPiece& piece : pieces) {
    geometry::ClusterFileData data;
    REQUIRE_MESSAGE(build_deposit_container(piece.patch, {}, 0, data, error), error);
    most = std::max<u64>(most, data.mesh.mesh.clusters.size());
    MESSAGE("a piece: ", data.mesh.mesh.clusters.size(), " clusters, ",
            data.mesh.leaf_triangle_count, " triangles at the finest form");
    CHECK(data.mesh.mesh.clusters.size() <= k_deposit_clusters_max);
  }
  CHECK(deposit_builds_reduced() == reduced);
  MESSAGE("the most clusters a piece: ", most);
  // A small patch against a limit no build can meet: built again, then refused, by name.
  DepositPatch small = dense_patch(8, 4);
  small.triangle_budget = 20000;
  geometry::ClusterFileData data;
  CHECK_FALSE(build_deposit_container(small, {}, 0, data, error, nullptr, 4));
  char name[24];
  std::snprintf(name, sizeof(name), "%016llx",
                static_cast<unsigned long long>(deposit_source_hash(small)));
  CHECK(error.find(std::string("patch deposit-") + name) != std::string::npos);
  CHECK(error.find("over the 4 a streamed host admits") != std::string::npos);
  MESSAGE(error);
}

namespace {
// A kilometre of massif foot (G191's measurement): a 60 m ridge 40 m across along z over a 40 m
// skirt 70 m across, so below a 60-67 degree face the ground grades through 50 to 35 degrees over
// about 15 m to a gentle floor, as the forest's does, its line wandering 12 m over 300 m.
f64 massif(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 z = static_cast<f64>(z_mm) / 1000.0;
  const f64 x = static_cast<f64>(x_mm) / 1000.0 - 12.0 * std::sin(z / 300.0 * 6.283185307179586);
  const f64 d = std::abs(x);
  auto bump = [&](f64 h, f64 w) {
    return d < w ? h * (0.5 + 0.5 * std::cos(3.14159265358979323846 * d / w)) : 0.0;
  };
  return bump(60, 40) + bump(40, 70) + .02 * x;
}
}  // namespace

TEST_CASE("deposits: what a kilometre of massif foot costs at load and a tile (measured)" *
          doctest::skip()) {
  // Run by hand in msvc-release (`--no-skip --test-case=*kilometre*`): the numbers rocks.md quotes
  // for G191. "Before" is what a host did at load until G191, every patch built and held; "after"
  // is what it does now, nothing built until a tile asks.
  test::TempDir scratch;
  const std::string dir = scratch.path();
  scene_gen::Context c;
  c.dir = dir;
  c.ddc = dir;
  c.ground.surface_fn = &massif;
  c.tile_size = 64;
  DepositLayer p;
  p.region_min = {-96, -512};
  p.region_max = {96, 512};
  p.spacing_m = 1;
  p.patch_m = 16;
  u32 budget = 128000, fragments = 96;  // the forest's
  SUBCASE("the forest's budget, 128,000 triangles a 16 m patch") {}
  SUBCASE("the default budget's density, 32,000 triangles a 16 m patch") {
    budget = 32000;
    fragments = 24;
  }
  p.patch.triangle_budget = budget;
  p.patch.fragments = fragments;
  p.sediment_depth_m = 0;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  std::string error;
  const auto t0 = std::chrono::steady_clock::now();
  void* state = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &state, &error), error);
  const f64 open_ms =
      std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t0).count();
  const auto t1 = std::chrono::steady_clock::now();
  void* second = nullptr;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &second, &error), error);
  const f64 reopen_ms =
      std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - t1).count();
  desc->close(second);
  scene_gen::Placements whole;
  REQUIRE(desc->expand(state, c, whole, &error));
  u64 recipe_bytes = 0;
  for (const auto& m : whole.meshes)
    recipe_bytes += m.recipe.size();
  // Every tile's patches built, as the host builds a tile's meshes when it activates.
  Vector<f64> tile_ms, mesh_ms;
  u64 triangles = 0, bytes = 0, clusters_max = 0, built = 0;
  const u64 reduced = deposit_builds_reduced();
  f64 all_ms = 0;
  for (i32 tz = -8; tz < 8; ++tz)
    for (i32 tx = -2; tx < 2; ++tx) {
      scene_gen::Placements one;
      c.ring = 0;
      REQUIRE(desc->tile(state, {tx, tz}, c, one, &error));
      if (one.instances.empty()) continue;
      f64 ms = 0;
      for (const auto& m : one.meshes) {
        JsonValue json;
        REQUIRE(parse_json(m.recipe, json).ok);
        DepositPatch patch;
        schema::ReadContext ctx;
        REQUIRE(schema::from_json(patch, json, ctx));
        geometry::ClusterFileData data;
        const auto b = std::chrono::steady_clock::now();
        REQUIRE_MESSAGE(build_deposit_container(patch, {}, 0, data, error), error);
        const f64 one_ms =
            std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - b).count();
        ms += one_ms;
        mesh_ms.push_back(one_ms);
        triangles += data.mesh.leaf_triangle_count;
        clusters_max = std::max<u64>(clusters_max, data.mesh.mesh.clusters.size());
        const std::string path = dir + "/p.clusters";
        REQUIRE(geometry::write_cluster_file(path, data, &error));
        std::string read;
        REQUIRE(io::read_file(path, read) == io::Status::Ok);
        bytes += read.size();
        ++built;
      }
      tile_ms.push_back(ms);
      all_ms += ms;
    }
  desc->close(state);
  std::sort(tile_ms.begin(), tile_ms.end());
  std::sort(mesh_ms.begin(), mesh_ms.end());
  // G191's pieces: how many meshes the patches were handed as, the longest one mesh's build (a
  // tile's meshes build on the host's pool, so the longest is what a tile waits at least), and how
  // many containers were built again at half their budget (none is the pieces' purpose).
  MESSAGE("meshes ", whole.meshes.size(), " for ", whole.things, " patches; one mesh's build median ",
          mesh_ms.empty() ? 0.0 : mesh_ms[mesh_ms.size() / 2], " ms, max ",
          mesh_ms.empty() ? 0.0 : mesh_ms.back(), " ms; built again at half the budget ",
          deposit_builds_reduced() - reduced);
  CHECK(deposit_builds_reduced() == reduced);
  MESSAGE("a kilometre of massif foot at ", budget, " a patch: ", whole.instances.size(),
          " meshes over ", tile_ms.size(), " tiles; open ", open_ms, " ms, the second open ",
          reopen_ms, " ms; recipes ", recipe_bytes,
          " bytes; every patch built (the load before G191) ", all_ms, " ms, ", triangles,
          " triangles, ", bytes, " bytes of containers, the largest patch ", clusters_max,
          " clusters; a tile's activation median ",
          tile_ms.empty() ? 0.0 : tile_ms[tile_ms.size() / 2], " ms, max ",
          tile_ms.empty() ? 0.0 : tile_ms.back(), " ms; built ", built);
  CHECK(clusters_max <= 4096);
}
