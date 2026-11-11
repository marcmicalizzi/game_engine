// G143: what a face sheds (one supply rule: a cone under a gully, a sheet under a planar face),
// the blocks shed below faces, and the formation's lithology (docs/subsystems/rocks.md, "What a
// face sheds"; docs/experiments/rockfall-deposits.md, G143).
#include "rock_checks.h"

#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/rocks/deposits.h>
#include <foundation/io/vfs.h>

#include <doctest/doctest.h>
#include <test_temp_dir.h>

#include <algorithm>
#include <chrono>
#include <cmath>

using namespace engine;
using namespace engine::rocks;
namespace {
// A 24 m face at 63 degrees from x = 24 to 36 m above a meadow falling gently toward -x, a plateau
// above it, and three gullies cut into the face at z = 12, 24 and 36 m, each 6 m across and 5 m
// deep, their floors still steeper than 50 degrees. `data` pointing at a zero is the same face
// with no gullies: straight contours everywhere.
f64 gullied(const void* data, i64 x_mm, i64 z_mm) noexcept {
  const bool gullies = data == nullptr || *static_cast<const int*>(data) != 0;
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  f64 h = x < 24 ? .08 * (x - 24) : (x > 36 ? 24 : (x - 24) * 2);
  if (!gullies) return h;
  for (f64 c : {12.0, 24.0, 36.0}) {
    const f64 u = (z - c) / (3.0 + 3.0 * std::clamp((x - 24) / 12.0, 0.0, 1.0));
    if (u * u >= 1) continue;
    const f64 across = (1 - u * u) * (1 - u * u);
    const f64 along = std::clamp((x - 24) / 8.0, 0.0, 1.0) * std::clamp((40 - x) / 4.0, 0.0, 1.0);
    h -= 5.0 * across * along;
  }
  return h;
}
// The same meadow under a planar face whose height grows along z: 8 m at z = 0 to 20 m at z = 48.
f64 rising(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  const f64 crest = 8.0 + .25 * z;
  return x < 24 ? .08 * (x - 24) : std::min((x - 24) * 2, crest);
}
f64 flat(const void*, i64, i64) noexcept { return 0; }
scene_gen::Ground ground(const int* gullies = nullptr) {
  scene_gen::Ground g;
  g.surface_fn = &gullied;
  g.context = gullies;
  return g;
}
scene_gen::Ground ground_of(scene_gen::HeightFn fn) {
  scene_gen::Ground g;
  g.surface_fn = fn;
  return g;
}
DepositLayer layer() {
  DepositLayer p;
  p.region_min = {0, 0};
  p.region_max = {48, 48};
  p.spacing_m = .5f;
  p.patch.fragments = 8;
  p.patch.fragment_m = .2f;
  p.patch.triangle_budget = 20000;
  p.sediment_depth_m = 0;
  p.blocks_per_100m2 = 0;  // the deposit alone, unless a case asks for blocks
  return p;
}
f64 top(const DepositPlan& plan, u32 i) {
  return plan.ground[i] + static_cast<f64>(plan.thickness[i]);
}
u32 at(const DepositPlan& plan, f64 x, f64 z) {
  return static_cast<u32>(std::llround(z / .5)) * plan.width +
         static_cast<u32>(std::llround(x / .5));
}
const DepositPile* pile_at(const DepositPlan& plan, u32 foot) {
  for (const auto& pile : plan.piles)
    if (pile.foot == foot) return &pile;
  return nullptr;
}
const DepositPile& largest(const DepositPlan& plan) {
  const DepositPile* best = &plan.piles[0];
  for (const auto& pile : plan.piles)
    if (pile.volume_m3 > best->volume_m3) best = &pile;
  return *best;
}
constexpr f64 k_repose = 0.70020753820970977;  // tan 35 degrees
}  // namespace

TEST_CASE("deposits: a planar face's sheet follows the face's height and holds its supply") {
  DepositPlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_deposits(layer(), ground_of(&rising), plan, error), error);
  REQUIRE_FALSE(plan.piles.empty());

  // The foot of the face is x = 24 (the first sample whose steepest descent is the meadow's).
  f64 last_height = 0, last_depth = 0;
  f64 lo = 1e9, hi = 0;
  for (f64 z = 16; z <= 32; z += 4) {  // where the kernel's reach is inside the region
    const DepositPile* pile = pile_at(plan, at(plan, 24, z));
    REQUIRE_MESSAGE(pile != nullptr, "no pile at the foot, z = ", z);
    const f64 depth = static_cast<f64>(plan.thickness[at(plan, 23.5, z)]);
    MESSAGE("z ", z, ": face ", 8 + .25 * z, " m, pile ", pile->height_m,
            " m over the foot, depth ", depth, " m half a metre out, face ", pile->face_m2, " m2");
    // Higher face, more supply, a deeper sheet.
    CHECK(pile->height_m > last_height);
    CHECK(depth > last_depth);
    last_height = pile->height_m;
    last_depth = depth;
    // A sheet holds its supply as a wedge, whose volume goes as the square of its height: the
    // height goes as the root of the face's height (to 10% across a face 2.5 times as high).
    const f64 law = pile->height_m / std::sqrt(8 + .25 * z);
    lo = std::min(lo, law);
    hi = std::max(hi, law);
  }
  CHECK(hi / lo < 1.1);
  // What the deposit holds is what the face shed, to 10%, over the whole plan.
  f64 asked = 0, held = 0;
  for (const auto& pile : plan.piles) {
    asked += pile.volume_m3;
    held += pile.held_m3;
  }
  MESSAGE("supply ", asked, " m3, held ", held, " m3");
  CHECK(held > .9 * asked);
  CHECK(held < 1.1 * asked);
  // No deposit where no face stands above: the plateau, and the meadow past the sheet's reach.
  for (f64 z = 0; z <= 48; z += .5) {
    CHECK(plan.thickness[at(plan, 47.5, z)] == 0.0f);
    CHECK(plan.thickness[at(plan, 0, z)] == 0.0f);
  }
  REQUIRE(plan_deposits(layer(), ground_of(&flat), plan, error));
  CHECK(plan.piles.empty());
  for (f32 v : plan.thickness)
    CHECK(v == 0.0f);
  // A layer with no supply asks for no rockfall deposit.
  DepositLayer none = layer();
  none.supply_m = 0;
  REQUIRE(plan_deposits(none, ground_of(&rising), plan, error));
  CHECK(plan.piles.empty());
}

TEST_CASE("deposits: a cone stands at each gully's mouth and a planar face makes none") {
  DepositPlan plan;
  std::string error;
  // A metre of retreat: piles that reach less far than the gullies are apart (at the default, 3.5
  // m, this face's cones merge into an apron that bulges under each gully).
  DepositLayer p = layer();
  p.supply_m = 1;
  REQUIRE_MESSAGE(plan_deposits(p, ground(), plan, error), error);
  REQUIRE(plan.piles.size() >= 3);

  // The funnels route their face to the feet in their mouths: the largest supply of each gully is
  // several times a planar foot's, and the deposit's top along the foot bulges there into a cone
  // that stands over the apron between the gullies (the supply the spurs still shed).
  for (f64 c : {12.0, 24.0, 36.0}) {
    f64 most = 0, tallest = 0, between = 0;
    for (const auto& pile : plan.piles) {
      const f64 fz = static_cast<f64>(pile.foot / plan.width) * .5;
      if (std::abs(fz - c) <= 2.0) {
        most = std::max(most, pile.volume_m3);
        tallest = std::max(tallest, pile.height_m);
      }
      if (std::abs(fz - (c < 36 ? c + 6 : c - 6)) <= .25)
        between = std::max(between, pile.height_m);
    }
    MESSAGE("gully at z ", c, ": its largest foot sheds ", most, " m3; its tallest pile ", tallest,
            " m over the foot, between gullies ", between, " m");
    CHECK(most > 3.0 * 13.4);  // a planar foot of this face sheds 13.4 m2 x 1 m = 13.4 m3
    CHECK(tallest > between);
  }
  // A planar face: every foot gets the same column of face, so every pile is the same height
  // (away from the region's two ends): a sheet, no cone.
  const int none = 0;
  REQUIRE(plan_deposits(layer(), ground(&none), plan, error));
  f64 lo = 1e9, hi = 0;
  for (const auto& pile : plan.piles) {
    const f64 z = static_cast<f64>(pile.foot / plan.width) * .5;
    if (z < 20 || z > 28 || pile.face_m2 <= 0) continue;  // the kernel's reach inside the region
    lo = std::min(lo, pile.height_m);
    hi = std::max(hi, pile.height_m);
  }
  MESSAGE("planar face: piles from ", lo, " to ", hi, " m");
  CHECK(hi < 1.05 * lo);
  for (u32 i = 0; i < plan.ground.size(); ++i)
    CHECK(deposit_convergence(plan, i, 4) == 0.0);
}

TEST_CASE("deposits: the union holds the angle of repose and eases concave to its toe") {
  DepositPlan plan;
  std::string error;
  REQUIRE(plan_deposits(layer(), ground(), plan, error));
  REQUIRE_FALSE(plan.piles.empty());
  // No part of the deposit, cone, sheet or where they meet, is steeper than the repose angle
  // between any two neighbouring samples: the piles merge without a step.
  const u32 w = plan.width;
  u32 pairs = 0;
  // Past the outline the edge's noise lifts the first ring of bare samples by up to k_edge_amp
  // (rocks.md, "The edge and the seams"); the rule is held where both samples are deeper.
  const f32 deep = static_cast<f32>(2 * k_edge_amp);
  for (u32 i = 0; i < plan.ground.size(); ++i) {
    if (plan.thickness[i] <= deep) continue;
    for (u32 j : {i + 1, i + w, i + w + 1}) {
      if (j >= plan.ground.size() || (j % w) == 0 || plan.thickness[j] <= deep) continue;
      const f64 run = j == i + w + 1 ? .5 * std::sqrt(2.0) : .5;
      CHECK(std::abs(top(plan, i) - top(plan, j)) <= k_repose * run + 1e-4);
      ++pairs;
    }
  }
  CHECK(pairs > 1000);
  // Down the largest cone's axis from its foot: the upper part at 35 degrees (to 0.02 in slope),
  // the lower part gentler, the slope never rising again toward the toe by more than 0.01 (the
  // axis crosses from the cone's profile to its neighbours' where the piles meet; the first
  // agent's lone cone held 1e-4).
  const DepositPile& cone = largest(plan);
  const f64 cz = static_cast<f64>(cone.foot / plan.width) * .5;
  const f64 ax = static_cast<f64>(cone.x_mm) / 1000.0;
  const f64 cx = static_cast<f64>(cone.foot % plan.width) * .5;
  const f64 upper = .75 * cone.height_m / k_repose;
  f64 last = k_repose + 1;
  u32 upper_steps = 0, lower_steps = 0;
  for (f64 x = cx; x - .5 >= 0; x -= .5) {
    const u32 a = at(plan, x, cz), b = at(plan, x - .5, cz);
    if (plan.thickness[a] <= .001f || plan.thickness[b] <= .001f) break;
    const f64 slope = (top(plan, a) - top(plan, b)) / .5;
    CHECK(slope <= k_repose + 1e-4);
    CHECK(slope <= last + .01);
    last = slope;
    if (ax - x + .5 <= upper) {
      CHECK(slope >= k_repose - .02);
      ++upper_steps;
    } else {
      ++lower_steps;
    }
  }
  CHECK(upper_steps >= 2);
  CHECK(lower_steps >= 2);
  CHECK(last < k_repose - .02);
  // Fall sorting on the bed: the coarse share rises from the apex to the toe.
  CHECK(plan.coarse[at(plan, cx - 1, cz)] < plan.coarse[at(plan, cx - 6, cz)]);
}

TEST_CASE("deposits: shed blocks thin with distance, the largest farthest, none past the shadow") {
  DepositPlan plan;
  std::string error;
  DepositLayer p = layer();
  p.blocks_per_100m2 = 60;
  REQUIRE_MESSAGE(plan_deposits(p, ground(), plan, error), error);
  MESSAGE("shed blocks: ", plan.blocks.size());
  REQUIRE(plan.blocks.size() > 30);
  const f64 shadow = std::tan(27.5 * 3.14159265358979323846 / 180.0);
  f64 farthest = 0;
  for (const auto& b : plan.blocks) {
    const f64 dx = static_cast<f64>(b.x_mm - b.top_x_mm) / 1000.0;
    const f64 dz = static_cast<f64>(b.z_mm - b.top_z_mm) / 1000.0;
    const f64 d = std::sqrt(dx * dx + dz * dz);
    // Evans and Hungr's line from the top of its talus: never past it.
    CHECK(b.top_m - b.y_m >= shadow * d - 1e-9);
    // Rests: never on a top steeper than the repose angle.
    CHECK(b.slope[0] * b.slope[0] + b.slope[1] * b.slope[1] <= 1.0609 * k_repose * k_repose);
    // Partly buried by its age, in the stated range.
    CHECK(b.buried >= .15 - 1e-9);
    CHECK(b.buried <= .5 + 1e-9);
    CHECK(b.half[1] <= std::max(b.half[0], b.half[2]));
    farthest = std::max(farthest, b.distance_m);
  }
  u32 count[3] = {};
  f64 size[3] = {};
  for (const auto& b : plan.blocks) {
    const u32 bin = std::min(2u, static_cast<u32>(3 * b.distance_m / farthest));
    ++count[bin];
    size[bin] += b.half[0] + b.half[2];
  }
  for (u32 k = 0; k < 3; ++k)
    if (count[k]) size[k] /= count[k];
  MESSAGE("blocks ", plan.blocks.size(), " out to ", farthest, " m; by thirds: ", count[0], " ",
          count[1], " ", count[2], ", mean size ", size[0], " ", size[1], " ", size[2]);
  CHECK(count[0] > count[1]);
  CHECK(count[1] > count[2]);
  CHECK(size[0] < size[1]);
  CHECK(size[1] < size[2]);
  // Mean block size rises from the largest pile's foot out toward its toe.
  const DepositPile& cone = largest(plan);
  const i64 foot_x = static_cast<i64>(cone.foot % plan.width) * 500;
  const i64 foot_z = static_cast<i64>(cone.foot / plan.width) * 500;
  f64 inner = 0, outer = 0, far_r = 0;
  u32 inner_n = 0, outer_n = 0;
  for (const auto& b : plan.blocks)
    if (std::abs(b.z_mm - foot_z) <= 4000 && b.x_mm <= foot_x)
      far_r = std::max(far_r, static_cast<f64>(foot_x - b.x_mm) / 1000.0);
  for (const auto& b : plan.blocks) {
    if (std::abs(b.z_mm - foot_z) > 4000 || b.x_mm > foot_x) continue;
    const f64 r = static_cast<f64>(foot_x - b.x_mm) / 1000.0;
    if (r < .5 * far_r) {
      inner += b.half[0];
      ++inner_n;
    } else {
      outer += b.half[0];
      ++outer_n;
    }
  }
  REQUIRE(inner_n > 0);
  REQUIRE(outer_n > 0);
  CHECK(inner / inner_n < outer / outer_n);
}

TEST_CASE(
    "deposits: a deposit under a limestone face is limestone, its blocks no thicker than a bed") {
  DepositLayer p = layer();
  p.blocks_per_100m2 = 60;
  scene::Formation f;
  f.dip_deg = 0;
  scene::FormationBed cap;
  cap.name = "cap";
  cap.thickness_m = 1000;
  f.beds.push_back(cap);
  p.formation = f;
  DepositLithology row;
  row.bed = "cap";
  row.type = RockType::Limestone;
  p.lithology.push_back(row);
  DepositPlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_deposits(p, ground(), plan, error), error);
  REQUIRE(plan.rock.size() == plan.ground.size());
  for (u32 i = 0; i < plan.ground.size(); ++i)
    if (plan.thickness[i] > .001f) CHECK(plan.rock[i] == RockType::Limestone);
  for (const auto& b : plan.blocks)
    CHECK(b.type == RockType::Limestone);
  // Thin beds (0.4 m) of shale, named as the type: the blocks are slabs no thicker than a bed.
  f.beds[0].name = "shale";
  f.beds[0].thickness_m = .4f;
  p.formation = f;
  p.lithology.clear();
  REQUIRE(plan_deposits(p, ground(), plan, error));
  REQUIRE_FALSE(plan.blocks.empty());
  for (const auto& b : plan.blocks) {
    CHECK(b.type == RockType::Shale);
    CHECK(2 * b.half[1] <= .4 + 1e-6);
  }
  // Without a formation nothing is read: no rock per sample, and the patch's own type.
  p.formation.reset();
  REQUIRE(plan_deposits(p, ground(), plan, error));
  CHECK(plan.rock.empty());
  for (const auto& b : plan.blocks)
    CHECK(b.type == p.patch.type);
  // A field out of the scene's ranges is refused with its sentence.
  f.dip_deg = 120;
  p.formation = f;
  CHECK_FALSE(plan_deposits(p, ground(), plan, error));
  CHECK(error.find("formation") != std::string::npos);
}

TEST_CASE("deposits: the bed meets the meadow between its samples, thin, off the lattice") {
  DepositPlan plan;
  std::string error;
  REQUIRE_MESSAGE(plan_deposits(layer(), ground(), plan, error), error);
  REQUIRE(plan.sink.size() == plan.ground.size());
  // Every edge of the lattice from a deposit sample to a bare one on gentle ground (the meadow,
  // not the face it leans on): where the drawn top meets the ground along it, as a share of the
  // edge from the deposit sample. G128 and the first agent's beds put a bare sample's top 1 mm
  // under the ground, so the crossing was within 1% of the bare sample on nearly every edge: the
  // outline was the lattice's staircase.
  const u32 w = plan.width;
  u32 edges = 0, at_sample = 0;
  f64 thickest = 0, sum = 0;
  for (u32 i = 0; i < plan.ground.size(); ++i) {
    if (plan.thickness[i] <= .001f) continue;
    const u32 x = i % w, z = i / w;
    const u32 around[] = {x > 0 ? i - 1 : ~0u, x + 1 < w ? i + 1 : ~0u, z > 0 ? i - w : ~0u,
                          z + 1 < plan.depth ? i + w : ~0u};
    for (u32 j : around) {
      if (j == ~0u || plan.thickness[j] > .001f) continue;
      if (std::abs(plan.ground[j] - plan.ground[i]) > .5 * .5) continue;  // a face, not the meadow
      const f64 in = static_cast<f64>(plan.thickness[i]) - .001;
      const f64 out = -.001 - static_cast<f64>(plan.sink[j]);
      const f64 u = in / (in - out);
      at_sample += u > .97 ? 1 : 0;
      thickest = std::max(thickest, static_cast<f64>(plan.thickness[i]));
      sum += u;
      ++edges;
    }
  }
  MESSAGE("outline edges ", edges, ": ", at_sample,
          " cross within 3% of the bare sample, mean "
          "crossing at ",
          sum / edges, " of the edge; thickest sample at the outline ", thickest, " m");
  REQUIRE(edges > 100);
  // The stated measure: fewer than one crossing in ten at the bare sample, and the deposit at its
  // outline no thicker than the toe's slope over a diagonal step plus the noise (0.6 x 0.71 x
  // 0.5 + 0.1, rounded up: 0.35 m at 0.5 m samples).
  CHECK(at_sample * 10 < edges);
  CHECK(thickest < .35);
}

TEST_CASE("deposits: a version-1 layer is migrated: no cap, and a zero cap asks for no talus") {
  test::TempDir scratch;
  scene_gen::Context c;
  const std::string dir = scratch.path();
  c.dir = dir;
  c.ddc = dir;
  c.ground = ground();
  c.tile_size = 16;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  std::string error;
  for (f64 cap : {3.0, 0.0}) {
    JsonValue params = schema::to_json(layer());
    params.as_object().erase(std::string("supply_m"));
    params["talus_depth_m"] = JsonValue(cap);
    void* state = nullptr;
    REQUIRE_MESSAGE(desc->open(params, c, &state, &error), error);
    scene_gen::Placements whole;
    REQUIRE(desc->expand(state, c, whole, &error));
    desc->close(state);
    // A cap of 3 m was the apron's; the migrated layer takes the default supply. A cap of zero
    // asked for no talus, and with no channel sediment that is no deposit at all.
    if (cap > 0)
      CHECK_FALSE(whole.instances.empty());
    else
      CHECK(whole.instances.empty());
  }
}

TEST_CASE("deposits: a cone across patch and tile borders meets itself to the millimetre") {
  test::TempDir scratch;
  scene_gen::Context c;
  const std::string dir = scratch.path();
  c.dir = dir;
  c.ddc = dir;
  c.ground = ground();
  c.tile_size = 16;
  DepositLayer p = layer();
  p.blocks_per_100m2 = 60;
  scene::Formation f;
  f.dip_deg = 0;
  scene::FormationBed cap;
  cap.name = "Limestone";
  cap.thickness_m = 1000;
  f.beds.push_back(cap);
  p.formation = f;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &state, &error), error);
  scene_gen::Placements whole;
  REQUIRE(desc->expand(state, c, whole, &error));
  desc->close(state);
  struct Read {
    DepositPatch patch;
    WorldPos at;
  };
  Vector<Read> patches;
  u32 blocks = 0;
  for (const auto& instance : whole.instances) {
    std::string text;
    REQUIRE(io::read_file(whole.meshes[instance.mesh].path, text) == io::Status::Ok);
    JsonValue json;
    REQUIRE(parse_json(text, json).ok);
    Read r;
    schema::ReadContext ctx;
    REQUIRE(schema::from_json(r.patch, json, ctx));
    r.at = instance.position;
    blocks += r.patch.blocks.size();
    CHECK(r.patch.type == RockType::Limestone);
    patches.push_back(r);
  }
  CHECK(blocks > 30);
  // Every pair of patches side by side along x: the shared column's top, in world metres, as the
  // mesh computes it (f32 local surface plus the f64 corner).
  u32 compared = 0;
  for (const auto& a : patches)
    for (const auto& b : patches) {
      const f64 span = .5 * static_cast<f64>(a.patch.width - 1);
      if (std::abs(b.at.x - (a.at.x + span)) > 1e-9 || b.at.z != a.at.z) continue;
      for (u32 z = 0; z < a.patch.depth && z < b.patch.depth; ++z) {
        const u32 ia = z * a.patch.width + a.patch.width - 1, ib = z * b.patch.width;
        if (a.patch.thickness[ia] <= .001f) continue;
        const f64 ya =
            a.at.y + static_cast<f64>(a.patch.ground[ia] + a.patch.thickness[ia] - .001f);
        const f64 yb =
            b.at.y + static_cast<f64>(b.patch.ground[ib] + b.patch.thickness[ib] - .001f);
        CHECK(std::abs(ya - yb) < 1e-3);
        // The top's gradient at the shared column is the plan's: the same on both sides.
        CHECK(a.patch.slope[2 * ia] == b.patch.slope[2 * ib]);
        CHECK(a.patch.slope[2 * ia + 1] == b.patch.slope[2 * ib + 1]);
        ++compared;
      }
    }
  CHECK(compared > 20);
  // And as drawn: the bed's top normal at each shared sample, solved by each patch, differs by
  // under a tenth of a degree (0.028 measured: f32 rounding of the one gradient both carry).
  f64 worst = 0;
  u32 normals = 0;
  auto top_normal = [&](const assets::MeshData& mesh, f32 x, f32 z, Vec3& out) {
    bool found = false;
    for (u32 v = 0; v < mesh.positions.size(); ++v)
      if (std::abs(mesh.positions[v].x - x) < 1e-4f && std::abs(mesh.positions[v].z - z) < 1e-4f &&
          (!found || mesh.normals[v].y > out.y)) {
        out = mesh.normals[v];
        found = true;
      }
    return found;
  };
  for (const auto& a : patches)
    for (const auto& b : patches) {
      const f64 span = .5 * static_cast<f64>(a.patch.width - 1);
      if (std::abs(b.at.x - (a.at.x + span)) > 1e-9 || b.at.z != a.at.z || normals > 40) continue;
      assets::MeshData ma, mb;
      REQUIRE(solve_deposit(a.patch, ma, error));
      REQUIRE(solve_deposit(b.patch, mb, error));
      for (u32 z = 0; z < a.patch.depth; ++z) {
        const u32 ia = z * a.patch.width + a.patch.width - 1;
        if (a.patch.thickness[ia] <= .001f) continue;
        Vec3 na{}, nb{};
        if (!top_normal(ma, static_cast<f32>(span), static_cast<f32>(z) * .5f, na) ||
            !top_normal(mb, 0.0f, static_cast<f32>(z) * .5f, nb))
          continue;
        const f64 cosine = std::clamp(static_cast<f64>(dot(na, nb)), -1.0, 1.0);
        worst = std::max(worst, std::acos(cosine) * 180.0 / 3.14159265358979323846);
        ++normals;
      }
    }
  MESSAGE("normals compared across patch borders: ", normals, ", worst ", worst, " degrees");
  CHECK(normals > 10);
  CHECK(worst < .1);
  // And a patch with blocks solves to a closed mesh: its bed and every fragment and block.
  for (const auto& r : patches)
    if (!r.patch.blocks.empty()) {
      assets::MeshData mesh;
      REQUIRE_MESSAGE(solve_deposit(r.patch, mesh, error), error);
      bool bed = false;
      for (f32 t : r.patch.thickness)
        bed = bed || t > .001f;
      if (bed) {
        checks::check_closed(mesh, 1 + r.patch.fragments + r.patch.blocks.size());
        break;
      }
    }
}

namespace {
// The cut a renderer draws at mesh-space error `t`: every cluster whose own error is at most `t`
// and whose parent's is past it (geometry.md, the cluster LOD's one formula).
bool in_cut(const geometry::ClusterLodMesh& lod, u32 k, f32 t) {
  return lod.lod[k].own_error <= t && t < lod.lod[k].parent_error;
}
// The highest triangle of the cut over (x, z), in the mesh's own frame; false where none covers it.
bool cut_top(const geometry::ClusterLodMesh& lod, f32 t, f64 x, f64 z, f64& y) {
  bool found = false;
  const auto& m = lod.mesh;
  for (u32 k = 0; k < m.clusters.size(); ++k) {
    if (!in_cut(lod, k, t)) continue;
    const auto& c = m.clusters[k];
    for (u32 j = 0; j < c.triangle_count; ++j) {
      const u32 packed = m.triangles[c.triangle_offset + j];
      const Vec3 a = m.vertices[c.vertex_offset + (packed & 0xffu)];
      const Vec3 b = m.vertices[c.vertex_offset + ((packed >> 8) & 0xffu)];
      const Vec3 d = m.vertices[c.vertex_offset + ((packed >> 16) & 0xffu)];
      const f64 ax = static_cast<f64>(a.x), az = static_cast<f64>(a.z);
      const f64 e1x = static_cast<f64>(b.x) - ax, e1z = static_cast<f64>(b.z) - az;
      const f64 e2x = static_cast<f64>(d.x) - ax, e2z = static_cast<f64>(d.z) - az;
      const f64 det = e1x * e2z - e2x * e1z;
      if (std::abs(det) < 1e-12) continue;
      const f64 u = ((x - ax) * e2z - e2x * (z - az)) / det;
      const f64 v = (e1x * (z - az) - (x - ax) * e1z) / det;
      if (u < -1e-6 || v < -1e-6 || u + v > 1 + 1e-6) continue;
      const f64 h = static_cast<f64>(a.y) + u * (static_cast<f64>(b.y) - static_cast<f64>(a.y)) +
                    v * (static_cast<f64>(d.y) - static_cast<f64>(a.y));
      if (!found || h > y) y = h;
      found = true;
    }
  }
  return found;
}
}  // namespace

TEST_CASE("deposits: neighbouring patches' coarse forms meet along their border") {
  // Stage 1 of G143's third pass (rocks.md, "The coarse forms keep the patch's border"): from
  // across a valley the apron drew as square panels of different shade with dark wedges between
  // them. Each 8 m patch is its own closed mesh (top, buried underside, walls), so the cluster
  // DAG's simplifier had no open border to hold: it pulled each patch's top away from the shared
  // line its own way, and the two coarse forms no longer met. Every cut of two neighbours must draw
  // the same top along the line they share, and no coarser than its error below the plan's top.
  test::TempDir scratch;
  scene_gen::Context c;
  const std::string dir = scratch.path();
  c.dir = dir;
  c.ddc = dir;
  c.ground = ground();
  c.tile_size = 16;
  DepositLayer p = layer();
  p.patch.fragments = 0;  // the bed alone: a fragment by the line is one patch's, not both
  p.patch.triangle_budget = 40000;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  REQUIRE(desc != nullptr);
  void* state = nullptr;
  std::string error;
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &state, &error), error);
  scene_gen::Placements whole;
  REQUIRE(desc->expand(state, c, whole, &error));
  desc->close(state);
  struct Read {
    DepositPatch patch;
    WorldPos at;
  };
  Vector<Read> patches;
  for (const auto& instance : whole.instances) {
    std::string text;
    REQUIRE(io::read_file(whole.meshes[instance.mesh].path, text) == io::Status::Ok);
    JsonValue json;
    REQUIRE(parse_json(text, json).ok);
    Read r;
    schema::ReadContext ctx;
    REQUIRE(schema::from_json(r.patch, json, ctx));
    r.at = instance.position;
    patches.push_back(r);
  }
  // The deepest pair side by side along x: the most bed along their shared column.
  i32 best_a = -1, best_b = -1;
  f64 best = 0;
  for (u32 i = 0; i < patches.size(); ++i)
    for (u32 j = 0; j < patches.size(); ++j) {
      const auto &a = patches[i], &b = patches[j];
      const f64 span = .5 * static_cast<f64>(a.patch.width - 1);
      if (std::abs(b.at.x - (a.at.x + span)) > 1e-9 || b.at.z != a.at.z) continue;
      f64 depth = 0;
      for (u32 z = 0; z < a.patch.depth; ++z)
        depth += static_cast<f64>(a.patch.thickness[z * a.patch.width + a.patch.width - 1]);
      if (depth > best) {
        best = depth;
        best_a = static_cast<i32>(i);
        best_b = static_cast<i32>(j);
      }
    }
  REQUIRE(best_a >= 0);
  const Read& a = patches[static_cast<u32>(best_a)];
  const Read& b = patches[static_cast<u32>(best_b)];
  geometry::ClusterFileData da, db;
  REQUIRE_MESSAGE(build_deposit_container(a.patch, {}, 0, da, error), error);
  REQUIRE_MESSAGE(build_deposit_container(b.patch, {}, 0, db, error), error);
  const f64 span = .5 * static_cast<f64>(a.patch.width - 1);
  u32 cuts = 0, gaps = 0;
  f64 worst_gap = 0, worst_sag = 0, worst_sag_t = 0;
  for (f32 t : {0.0f, .005f, .02f, .05f, .1f, .25f, .5f, 1.0f}) {
    ++cuts;
    // Along the shared line, a little in from each side, at every sample and between them.
    for (u32 s = 0; s + 1 < 2 * a.patch.depth - 1; ++s) {
      const f64 z = .25 * static_cast<f64>(s);
      f64 ya = 0, yb = 0;
      const bool ha = cut_top(da.mesh, t, span - 1e-4, z, ya);
      const bool hb = cut_top(db.mesh, t, 1e-4, z, yb);
      if (ha != hb) {
        ++gaps;
        continue;
      }
      if (!ha) continue;
      const f64 gap = std::abs((a.at.y + ya) - (b.at.y + yb));
      worst_gap = std::max(worst_gap, gap);
      gaps += gap > 2e-3;
    }
    // Inside the patch: the cut's top against the plan's at every sample with a bed.
    for (u32 i = 0; i < a.patch.width * a.patch.depth; ++i) {
      if (a.patch.thickness[i] <= .05f) continue;
      const f64 x = .5 * static_cast<f64>(i % a.patch.width),
                z = .5 * static_cast<f64>(i / a.patch.width);
      f64 y = 0;
      if (!cut_top(da.mesh, t, std::clamp(x, 1e-4, span - 1e-4),
                   std::clamp(z, 1e-4, .5 * static_cast<f64>(a.patch.depth - 1) - 1e-4), y))
        continue;
      const f64 plan = static_cast<f64>(a.patch.ground[i]) + static_cast<f64>(a.patch.thickness[i]);
      const f64 sag = plan - y - static_cast<f64>(t);
      if (sag > worst_sag) {
        worst_sag = sag;
        worst_sag_t = static_cast<f64>(t);
      }
    }
  }
  MESSAGE("patches ", best_a, " and ", best_b, ": ", cuts, " cuts, ", gaps,
          " places along the border where the two tops part by over 2 mm or one is missing (worst ",
          worst_gap, " m); worst sag of a cut below the plan's top past its own error ", worst_sag,
          " m (at error ", worst_sag_t, ")");
  CHECK(gaps == 0u);
  CHECK(worst_sag < .05);
}

TEST_CASE("deposits: G143's plan and a patch with blocks are their pinned bytes") {
  DepositLayer p = layer();
  p.blocks_per_100m2 = 60;
  DepositPlan plan;
  std::string error;
  REQUIRE(plan_deposits(p, ground(), plan, error));
  u64 h = hash_bytes(plan.thickness.data(), plan.thickness.size() * sizeof(f32));
  h = hash_combine(h, hash_bytes(plan.coarse.data(), plan.coarse.size() * sizeof(f32)));
  for (const auto& b : plan.blocks) {
    const f64 v[] = {static_cast<f64>(b.x_mm),
                     static_cast<f64>(b.z_mm),
                     b.y_m,
                     b.half[0],
                     b.half[1],
                     b.half[2],
                     b.slope[0],
                     b.slope[1],
                     b.yaw,
                     b.buried};
    h = hash_combine(h, hash_bytes(v, sizeof(v)));
  }
  for (const auto& pile : plan.piles)
    h = hash_combine(h, hash_bytes(&pile.height_m, sizeof(f64)));
  MESSAGE("G143 plan pin: ", h);
  // Re-taken by G143's second agent (2026-11-10): one supply rule replaced the capped apron and
  // the opt-in cones (was 11798949088187083648).
  // Re-taken for the edge field (2026-11-10, stage 2): the outline's ring moved by world noise and
  // the bare samples' sink (was 8162223296658278579).
  CHECK(h == 4874663829597876097ull);
  DepositPatch patch;
  patch.width = 5;
  patch.depth = 5;
  patch.fragments = 4;
  patch.triangle_budget = 4000;
  for (u32 i = 0; i < 25; ++i) {
    patch.ground.push_back(static_cast<f32>(i % 5) * .125f);
    patch.thickness.push_back(.1f);
    patch.coarse.push_back(.5f);
  }
  DepositBlock block;
  block.x_m = 2;
  block.z_m = 2;
  block.y_m = .35f;
  block.half = {.6f, .3f, .45f};
  block.slope = {.125f, 0};
  block.yaw = .7f;
  block.buried = .3f;
  block.seed = 143;
  patch.blocks.push_back(block);
  assets::MeshData mesh;
  REQUIRE_MESSAGE(solve_deposit(patch, mesh, error), error);
  checks::check_closed(mesh, 1 + patch.fragments + 1);
  MESSAGE("G143 patch mesh pin: ", checks::mesh_hash(mesh));
  // Re-taken for the bed's normals from the top's gradient, not its triangles (stage 2; was
  // 6681331591728775090), then for fragments built at the rung their size needs (stage 3; was
  // 2756680837895180932).
  CHECK(checks::mesh_hash(mesh) == 11144000825443644342ull);
  // A patch with no blocks keeps its version-1 key: the canonical bytes say nothing of them.
  DepositPatch plain = patch;
  plain.blocks.clear();
  plain.block_triangles = 999;
  DepositPatch other = plain;
  other.block_triangles = 432;
  CHECK(deposit_source_hash(plain) == deposit_source_hash(other));
}

TEST_CASE("deposits: what a tile of cones and shed blocks costs (measured, printed)") {
  // The numbers rocks.md and the grounding page quote: instances and merged meshes a 16 m tile,
  // triangles a square metre of bed at the finest and coarsest forms, and the milliseconds a
  // tile's patches take to build (the activation's cost: `tile` itself is a lookup).
  test::TempDir scratch;
  scene_gen::Context c;
  const std::string dir = scratch.path();
  c.dir = dir;
  c.ddc = dir;
  c.ground = ground();
  c.tile_size = 16;
  DepositLayer p = layer();
  p.blocks_per_100m2 = 30;
  p.patch.fragments = 64;
  p.patch.triangle_budget = 40000;
  const auto* desc = scene_gen::GeneratorRegistry::global().find_placement("deposits");
  void* state = nullptr;
  std::string error;
  DepositPlan alone;
  const auto planned = std::chrono::steady_clock::now();
  REQUIRE(plan_deposits(p, c.ground, alone, error));
  const f64 plan_ms =
      std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - planned).count();
  // `open` plans and checks every patch, and solves none (G143, stage 4): a patch is solved
  // when its tile's mesh is built.
  const u64 solves = deposit_solve_count();
  const auto opened = std::chrono::steady_clock::now();
  REQUIRE_MESSAGE(desc->open(schema::to_json(p), c, &state, &error), error);
  const f64 open_ms =
      std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - opened).count();
  CHECK(deposit_solve_count() == solves);
  scene_gen::Placements tile;
  REQUIRE(desc->tile(state, {1, 1}, c, tile, &error));  // the tile holding the middle cone's foot
  Vector<scene_gen::PlacementMesh> meshes;
  REQUIRE(desc->meshes(state, c, meshes, &error));
  desc->close(state);
  REQUIRE_FALSE(tile.instances.empty());
  u64 leaf = 0, root = 0, blocks = 0;
  f64 area = 0, build_ms = 0;
  for (const auto& instance : tile.instances) {
    std::string text;
    REQUIRE(io::read_file(meshes[instance.mesh].path, text) == io::Status::Ok);
    JsonValue json;
    REQUIRE(parse_json(text, json).ok);
    DepositPatch patch;
    schema::ReadContext ctx;
    REQUIRE(schema::from_json(patch, json, ctx));
    blocks += patch.blocks.size();
    for (u32 z = 0; z + 1 < patch.depth; ++z)
      for (u32 x = 0; x + 1 < patch.width; ++x) {
        const u32 a = z * patch.width + x;
        if (std::max({patch.thickness[a], patch.thickness[a + 1], patch.thickness[a + patch.width],
                      patch.thickness[a + patch.width + 1]}) > .001f)
          area += static_cast<f64>(patch.spacing_m) * static_cast<f64>(patch.spacing_m);
      }
    geometry::ClusterFileData data;
    const auto built = std::chrono::steady_clock::now();
    REQUIRE_MESSAGE(build_deposit_container(patch, {}, 0, data, error), error);
    build_ms +=
        std::chrono::duration<f64, std::milli>(std::chrono::steady_clock::now() - built).count();
    leaf += data.mesh.leaf_triangle_count;
    for (u32 k = 0; k < data.mesh.lod.size(); ++k)
      if (data.mesh.lod[k].parent_error == geometry::k_lod_terminal_error)
        root += data.mesh.mesh.clusters[k].triangle_count;
  }
  MESSAGE("tile (1,1): ", tile.instances.size(), " instances (merged meshes), ", blocks,
          " blocks as geometry, ", area, " m2 of bed; finest ", leaf, " triangles (",
          static_cast<f64>(leaf) / area, " a m2), coarsest ", root, " (",
          static_cast<f64>(root) / area, " a m2); plan ", plan_ms, " ms, region open ", open_ms,
          " ms, the tile's patches ", build_ms, " ms");
  CHECK(tile.instances.size() <= 4);  // a 16 m tile of 8 m patches: four merged meshes at most
  CHECK(root < leaf);
}
