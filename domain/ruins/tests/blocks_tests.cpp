// The block layer (docs/subsystems/ruins.md, "The block layer"): the section assembler's building
// laid block by block from a block kit. The kits are made here, in memory — the kit of boxes for
// the footprint and the synthetic block kit for the blocks — so nothing binary is in the tree. The
// cases are the page's invariants for the representation: the kit converts and a bad one is
// refused with a sentence; one seed and tile is one building on one thread or many, and it is the
// section assembler's building; an intact wall is every course laid in running bond, the quoins
// taking the corners in turn; what stands is under the ruin rule's line and carried by what is
// under it; an opening is a gap with its lintel over it; the fallen blocks lie beside the walls on
// the ground, and the far tier drops them and nothing else. The golden hash that says the same
// holds on every toolchain is in apps/engine_content/tests/determinism_tests.cpp.
#include <core/jobs/job_system.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/ruins/fragment.h>
#include <domain/ruins/ruins.h>
#include <domain/ruins/synthetic_kit.h>

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <schemas/scene.h>
#include <string>
#include <utility>
#include <vector>

using namespace engine;
using namespace engine::ruins;

namespace {

Kit section_kit(void (*edit)(scene::RuinKit&) = nullptr) {
  SyntheticKit synthetic;
  make_synthetic_kit(SyntheticKitOptions{}, synthetic);
  if (edit != nullptr) edit(synthetic.kit);
  Kit kit;
  std::string error;
  const bool ok = kit_from_schema(synthetic.kit, "", kit, error);
  REQUIRE_MESSAGE(ok, error);
  return kit;
}

BlockKit block_kit(const SyntheticBlockOptions& options = SyntheticBlockOptions{}) {
  SyntheticBlockKit synthetic;
  make_synthetic_block_kit(options, synthetic);
  BlockKit kit;
  std::string error;
  const bool ok = block_kit_from_schema(synthetic.kit, "", kit, error);
  REQUIRE_MESSAGE(ok, error);
  return kit;
}

// The assembler tests' ground: a tilted plane with a quadratic bump, no C library in it.
f32 test_ground_m(f64 x_m, f64 z_m) noexcept {
  const f32 x = static_cast<f32>(x_m);
  const f32 z = static_cast<f32>(z_m);
  const f32 u = x - 40.0f;
  const f32 v = z + 25.0f;
  return 1.5f + 0.02f * x - 0.015f * z + 0.0004f * (u * u + v * v) * 0.25f;
}
// The assembler asks it in whole millimetres (ADR-0053): the same heights at the same places.
f64 test_ground(const void*, i64 x_mm, i64 z_mm) noexcept {
  return static_cast<f64>(
      test_ground_m(static_cast<f64>(x_mm) / 1000.0, static_cast<f64>(z_mm) / 1000.0));
}

Placement placement_of(u64 seed, u8 wind = 0) {
  Placement p;
  p.world_seed = seed;
  p.tile_cm = 3200;
  p.wind_step = wind;
  p.ground = Ground{&test_ground, nullptr};
  return p;
}

bool same(const Block& a, const Block& b) {
  return std::memcmp(&a.position, &b.position, sizeof(WorldPos)) == 0 && a.block == b.block &&
         a.building == b.building && a.wall == b.wall && a.index == b.index &&
         a.course == b.course && a.role == b.role && a.yaw == b.yaw && a.flags == b.flags;
}

constexpr i32 k_dx[4] = {1, 0, -1, 0};
constexpr i32 k_dz[4] = {0, -1, 0, 1};

// A point of the world in the building's frame, centimetres, from the frame's integer origin and
// its sixteenth-turn yaw (the assembler's Q14 turn, inverted in doubles).
struct Local {
  f64 x;
  f64 z;
};
Local to_local(const Assembler::Frame& frame, f64 x, f64 z) {
  const f64 dx = static_cast<f64>(x) * 100.0 - static_cast<f64>(frame.origin_x_cm);
  const f64 dz = static_cast<f64>(z) * 100.0 - static_cast<f64>(frame.origin_z_cm);
  const f64 angle = static_cast<f64>(frame.yaw) * 22.5 * 3.14159265358979323846 / 180.0;
  const f64 c = std::cos(angle);
  const f64 s = std::sin(angle);
  return Local{c * dx - s * dz, s * dx + c * dz};
}

// Where a standing block lies along its wall, centimetres from the wall's start vertex.
struct Along {
  f64 u0;
  f64 u1;
  f64 across;
};
Along along_wall(const BlockAssembler& layer, const BlockKit& kit, const Block& b) {
  const Assembler::Side& s = layer.assembler().sides()[b.wall];
  const Local p = to_local(layer.assembler().frame(), b.position.x, b.position.z);
  const f64 u = (p.x - s.x0) * k_dx[s.dir] + (p.z - s.z0) * k_dz[s.dir];
  const f64 across = (p.x - s.x0) * k_dx[(s.dir + 1) & 3] + (p.z - s.z0) * k_dz[(s.dir + 1) & 3];
  return Along{u, u + kit.blocks[b.block].length_cm, across};
}

void intact_rules(scene::RuinKit& kit) {
  kit.rules.collapse = 0.0f;
  kit.rules.breach = 0.0f;
  kit.rules.windward = 0.0f;
  kit.rules.opening = 0.0f;
}

void open_rules(scene::RuinKit& kit) {
  kit.rules.collapse = 0.0f;
  kit.rules.breach = 0.0f;
  kit.rules.windward = 0.0f;
  kit.rules.opening = 1.0f;
}

}  // namespace

TEST_CASE("ruins blocks: the synthetic block kit converts at every fidelity") {
  const BlockKit low = block_kit();
  CHECK(low.course_cm == 30);
  CHECK(low.thickness_cm == 60);
  CHECK(low.blocks.size() == 14);
  CHECK(low.meshes.size() == 14);
  CHECK(low.stretcher_cm == 60);
  CHECK(low.quoin_cm == 90);  // the wall's thickness and half a stretcher: the bond's offset
  REQUIRE(low.straight.size() == 4);
  CHECK(low.groups[low.straight[0]].length_cm == 30);  // the half
  CHECK(low.groups[low.straight[1]].length_cm == 45);
  CHECK(low.groups[low.straight[2]].length_cm == 60);
  CHECK(low.groups[low.straight[3]].length_cm == 75);
  REQUIRE(low.lintels.size() == 1);
  CHECK(low.groups[low.lintels[0]].length_cm == 150);
  REQUIRE(low.sills.size() == 1);
  CHECK(low.groups[low.sills[0]].length_cm == 100);
  for (const KitBlock& b : low.blocks) {
    CHECK(b.height_cm == 30);
    CHECK(b.depth_cm == 60);
    CHECK(b.radius_cm * 2 >= b.length_cm);
  }
  // The fidelities bracket a block's pairs: one cluster, tens, hundreds.
  struct Case {
    BlockFidelity fidelity;
    u32 lo;
    u32 hi;
  };
  const Case cases[] = {{BlockFidelity::low, 44, 44},
                        {BlockFidelity::mid, 3500, 6500},
                        {BlockFidelity::high, 35000, 65000}};
  for (const Case& c : cases) {
    SyntheticBlockOptions options;
    options.fidelity = c.fidelity;
    SyntheticBlockKit kit;
    make_synthetic_block_kit(options, kit);
    CAPTURE(block_fidelity_name(c.fidelity));
    REQUIRE(kit.glb.size() == 14);
    REQUIRE(kit.triangles.size() == 14);
    for (u32 i = 0; i < kit.glb.size(); ++i) {
      CHECK(kit.glb[i][0] == 'g');
      CHECK(kit.triangles[i] >= c.lo);
      CHECK(kit.triangles[i] <= c.hi);
    }
    // Made again, the same bytes.
    SyntheticBlockKit again;
    make_synthetic_block_kit(options, again);
    CHECK(again.glb == kit.glb);
  }
  BlockFidelity parsed = BlockFidelity::low;
  CHECK(parse_block_fidelity("high", parsed));
  CHECK(parsed == BlockFidelity::high);
  CHECK_FALSE(parse_block_fidelity("ultra", parsed));
}

TEST_CASE("ruins blocks: a block kit that cannot lay a wall is refused with a sentence") {
  SyntheticBlockKit synthetic;
  make_synthetic_block_kit(SyntheticBlockOptions{}, synthetic);
  BlockKit refused;
  std::string error;
  {
    scene::RuinBlockKit file = synthetic.kit;
    Vector<scene::RuinBlock> kept;
    for (const scene::RuinBlock& b : file.blocks)
      if (b.role != scene::RuinBlockRole::Stretcher) kept.push_back(b);
    file.blocks = kept;
    CHECK_FALSE(block_kit_from_schema(file, "", refused, error));
    CHECK(error.find("stretcher") != std::string::npos);
  }
  {
    scene::RuinBlockKit file = synthetic.kit;
    for (scene::RuinBlock& b : file.blocks)
      if (b.role == scene::RuinBlockRole::Quoin) b.length = 0.6f;  // as long as the wall is thick
    CHECK_FALSE(block_kit_from_schema(file, "", refused, error));
    CHECK(error.find("stack bond") != std::string::npos);
  }
  {
    scene::RuinBlockKit file = synthetic.kit;
    for (scene::RuinBlock& b : file.blocks) {
      if (b.role == scene::RuinBlockRole::Quoin && b.eroded) b.length = 1.0f;
    }
    CHECK_FALSE(block_kit_from_schema(file, "", refused, error));
    CHECK(error.find("two lengths") != std::string::npos);
  }
  {
    scene::RuinBlockKit file = synthetic.kit;
    file.blocks[0].yaw_deg = 10.0f;
    CHECK_FALSE(block_kit_from_schema(file, "", refused, error));
    CHECK(error.find("22.5") != std::string::npos);
  }
  {
    scene::RuinBlockKit file = synthetic.kit;
    file.bond = "flemish";
    CHECK_FALSE(block_kit_from_schema(file, "", refused, error));
    CHECK(error.find("running") != std::string::npos);
  }
  {
    scene::RuinBlockKit file = synthetic.kit;
    file.rules.support = 1.5f;
    CHECK_FALSE(block_kit_from_schema(file, "", refused, error));
  }
  {
    scene::RuinBlockKit file = synthetic.kit;
    file.blocks[3].depth = 0.0f;
    CHECK_FALSE(block_kit_from_schema(file, "", refused, error));
    CHECK(error.find("positive") != std::string::npos);
  }
}

TEST_CASE("ruins blocks: one seed and tile is one building, the assembler's, on any thread count") {
  const Kit kit = section_kit();
  const BlockKit blocks = block_kit();
  Vector<TileCoord> tiles;
  choose_tiles(7, TileCoord{-20, -20}, TileCoord{19, 19}, 120, 0.0f, tiles);
  REQUIRE(tiles.size() == 120);
  const Placement placement = placement_of(7, 3);
  std::string error;
  BlockOutput serial;
  REQUIRE(assemble_block_tiles(kit, blocks, placement,
                               std::span<const TileCoord>(tiles.data(), tiles.size()), nullptr,
                               serial, &error));
  BlockOutput again;
  REQUIRE(assemble_block_tiles(kit, blocks, placement,
                               std::span<const TileCoord>(tiles.data(), tiles.size()), nullptr,
                               again, &error));
  CHECK(hash_blocks(serial) == hash_blocks(again));
  for (const u32 workers : {1u, 5u}) {
    jobs::JobSystem js(jobs::JobSystemConfig{
        .performance_workers = workers, .efficiency_workers = 1, .pin_threads = false});
    BlockOutput parallel;
    REQUIRE(assemble_block_tiles(kit, blocks, placement,
                                 std::span<const TileCoord>(tiles.data(), tiles.size()), &js,
                                 parallel, &error));
    REQUIRE(parallel.blocks.size() == serial.blocks.size());
    bool all_same = true;
    for (u32 i = 0; i < serial.blocks.size(); ++i)
      all_same = all_same && same(serial.blocks[i], parallel.blocks[i]);
    CHECK_MESSAGE(all_same, "workers " << workers);
    CHECK(hash_blocks(parallel) == hash_blocks(serial));
  }
  // The same buildings the section assembler makes: every site and every drift, field by field
  // (the site's instances are the blocks).
  Output sections;
  REQUIRE(assemble_tiles(kit, placement, std::span<const TileCoord>(tiles.data(), tiles.size()),
                         nullptr, sections, &error));
  REQUIRE(sections.sites.size() == serial.sites.size());
  REQUIRE(sections.drifts.size() == serial.drifts.size());
  for (u32 i = 0; i < sections.sites.size(); ++i) {
    const Site& a = sections.sites[i];
    const Site& b = serial.sites[i];
    CHECK(a.seed == b.seed);
    CHECK(a.tile == b.tile);
    CHECK(a.shape == b.shape);
    CHECK(a.yaw == b.yaw);
    CHECK(a.walls == b.walls);
    CHECK(std::memcmp(&a.origin, &b.origin, sizeof(WorldPos)) == 0);
  }
  for (u32 i = 0; i < sections.drifts.size(); ++i)
    CHECK(std::memcmp(&sections.drifts[i], &serial.drifts[i], sizeof(Drift)) == 0);
  // Several hundred blocks a building, as the plan guessed.
  const f64 mean = static_cast<f64>(serial.blocks.size()) / static_cast<f64>(serial.sites.size());
  MESSAGE("blocks per building: " << mean);
  CHECK(mean > 200.0);
  // One tile alone is the same building it is among the others.
  BlockAssembler layer(kit, blocks);
  BlockOutput one;
  REQUIRE(layer.assemble(placement, tiles[57], one, &error));
  const Site& in_all = serial.sites[57];
  REQUIRE(one.blocks.size() == in_all.instance_count);
  for (u32 i = 0; i < one.blocks.size(); ++i) {
    Block b = one.blocks[i];
    b.building = 57;
    CHECK(same(b, serial.blocks[in_all.first_instance + i]));
  }
}

TEST_CASE("ruins blocks: an intact wall is every course in running bond, quoins interlocked") {
  const Kit kit = section_kit(&intact_rules);
  const BlockKit blocks = block_kit();
  BlockAssembler layer(kit, blocks);
  REQUIRE(layer.courses() == 8);  // 2.4 m of 0.3 m courses
  const i32 ht = blocks.thickness_cm / 2;
  u32 joints = 0;
  u32 bonded = 0;
  for (u32 b = 0; b < 40; ++b) {
    BlockOutput out;
    std::string error;
    REQUIRE(layer.assemble(placement_of(300 + b, static_cast<u8>(b & 15u)), TileCoord{1, -1}, out,
                           &error));
    const Vector<Assembler::Side>& sides = layer.assembler().sides();
    CAPTURE(b);
    // Nothing fell: every block stands, on the building's base, a course above the last.
    for (const Block& block : out.blocks) {
      CHECK((block.flags & k_block_fallen) == 0);
      const f32 expected =
          static_cast<f32>(layer.assembler().frame().base_cm + 30 * block.course) * 0.01f;
      CHECK(block.position.y == doctest::Approx(expected).epsilon(1e-6));
    }
    for (u32 w = 0; w < sides.size(); ++w) {
      const Assembler::Side& s = sides[w];
      std::vector<f64> previous;
      for (u32 c = 0; c < layer.courses(); ++c) {
        std::vector<std::pair<f64, f64>> spans;
        u32 quoins_at_start = 0;
        u32 quoins_at_end = 0;
        for (const Block& block : out.blocks) {
          if (block.wall != w || block.course != c) continue;
          const Along a = along_wall(layer, blocks, block);
          CHECK(std::fabs(a.across) <= 1.5);  // on the centre line
          spans.push_back({a.u0, a.u1});
          if (block.role == static_cast<u8>(BlockRole::quoin)) {
            if (a.u0 < 0.0) ++quoins_at_start;
            if (a.u1 > s.length_cm) ++quoins_at_end;
          }
        }
        std::sort(spans.begin(), spans.end());
        REQUIRE(!spans.empty());
        // The course runs from the far face of the corner it owns to the near face of the one it
        // does not, and owns its start on the even courses and its end on the odd ones.
        const bool even = (c & 1u) == 0;
        CHECK(quoins_at_start == (even ? 1u : 0u));
        CHECK(quoins_at_end == (even ? 0u : 1u));
        // Within the joint the slots leave (a block a few centimetres shorter than its slot is
        // centred in it), and never past the ends.
        const f64 start = even ? -ht : ht;
        const f64 end = even ? s.length_cm - ht : s.length_cm + ht;
        CHECK(spans.front().first >= start - 1.5);
        CHECK(spans.front().first <= start + 9.0);
        CHECK(spans.back().second <= end + 1.5);
        CHECK(spans.back().second >= end - 9.0);
        // Laid end to end: no joint wider or overlap deeper than the kit's lengths leave.
        std::vector<f64> here;
        for (usize k = 1; k < spans.size(); ++k) {
          const f64 joint = spans[k].first - spans[k - 1].second;
          CHECK(joint <= 9.0);
          CHECK(joint >= -9.0);
          here.push_back(0.5 * (spans[k].first + spans[k - 1].second));
        }
        // Running bond: a joint seldom sits over a joint of the course below.
        for (const f64 j : here) {
          if (previous.empty()) break;
          f64 nearest = 1e9;
          for (const f64 p : previous)
            nearest = std::min(nearest, std::fabs(p - j));
          ++joints;
          bonded += nearest > 10.0 ? 1u : 0u;
        }
        previous = here;
      }
    }
  }
  MESSAGE("joints over the course below: " << bonded << " of " << joints << " clear by 10 cm");
  CHECK(bonded * 10 >= joints * 8);
}

TEST_CASE("ruins blocks: what stands is under the ruin line and carried, and nothing floats") {
  const Kit kit = section_kit();
  const BlockKit blocks = block_kit();
  BlockAssembler layer(kit, blocks);
  const i32 ht = blocks.thickness_cm / 2;
  const i32 top_cm = static_cast<i32>(layer.courses()) * blocks.course_cm;
  const i32 drop = blocks.rules.top_drop_q * blocks.course_cm / k_q_one;
  u32 standing = 0;
  u32 fallen = 0;
  for (u32 b = 0; b < 120; ++b) {
    BlockOutput out;
    std::string error;
    REQUIRE(layer.assemble(placement_of(700 + b, static_cast<u8>(b & 15u)), TileCoord{0, 2}, out,
                           &error));
    const Vector<Assembler::Side>& sides = layer.assembler().sides();
    CAPTURE(b);
    for (const Block& block : out.blocks) {
      if ((block.flags & k_block_fallen) != 0) {
        ++fallen;
        continue;
      }
      ++standing;
      const Assembler::Side& s = sides[block.wall];
      const Along a = along_wall(layer, blocks, block);
      const i32 centre = static_cast<i32>(std::floor(0.5 * (a.u0 + a.u1) + 0.5));
      const i64 line =
          i64{layer.assembler().height_q(block.wall, std::clamp(centre, 0, s.length_cm))} * top_cm /
          k_q_one;
      // Under the line, give or take the broken top's own draw and the centimetre the yaw rounds.
      CHECK(i64{(block.course + 1) * blocks.course_cm} <= line + drop + 2);
      if (block.course == 0) continue;
      // Something under it: a standing block of the course below in its wall, or the corner
      // square its neighbour laid.
      bool carried = a.u0 < ht || a.u1 > s.length_cm - ht;
      for (const Block& under : out.blocks) {
        if (carried) break;
        if (under.wall != block.wall || under.course + 1 != block.course ||
            (under.flags & k_block_fallen) != 0) {
          continue;
        }
        const Along u = along_wall(layer, blocks, under);
        carried = std::min(u.u1, a.u1) - std::max(u.u0, a.u0) > 1.5;
      }
      CHECK(carried);
    }
  }
  MESSAGE("standing " << standing << ", fallen and lying " << fallen);
  // The ruin rule brings walls down, and some of what fell lies beside them (the rubble rule's
  // share: most is under the sand).
  CHECK(fallen > 0);
  CHECK(standing > fallen);
}

TEST_CASE("ruins blocks: both forms lay their rubble on the same sites, in the same amount") {
  // The kit of boxes and the synthetic block kit: the section kit's profile block (0.6 m x 0.3 m x
  // the 0.6 m wall) is the block kit's stretcher, so a site is one stretcher's worth either way.
  const Kit kit = section_kit();
  const BlockKit blocks = block_kit();
  BlockAssembler layer(kit, blocks);
  Assembler assembler(kit);
  Assembler field_of(kit);
  u32 sites = 0;
  u32 laid = 0;
  u32 pieces = 0;
  u32 oversize = 0;
  f64 site_cm3 = 0.0;
  f64 block_cm3 = 0.0;
  f64 stone_cm3 = 0.0;
  std::vector<f64> wall_ratio;  // the sections' rubble over the blocks', per wall with rubble
  for (u32 b = 0; b < 80; ++b) {
    const Placement placement = placement_of(2100 + b, static_cast<u8>(b & 15u));
    const TileCoord tile{1, 2};
    BlockOutput laid_out;
    Output sections;
    Output scratch;
    std::string error;
    REQUIRE(layer.assemble(placement, tile, laid_out, &error));
    REQUIRE(assembler.assemble(placement, tile, sections, &error));
    Placement walls = placement;
    walls.detail = Detail::walls;
    REQUIRE(field_of.assemble(walls, tile, scratch, &error));
    const Vector<RubbleSite>& field = field_of.rubble();
    const u32 wall_count = field_of.sides().size();
    CAPTURE(b);
    sites += field.size();
    site_cm3 += static_cast<f64>(field.size()) * kit.rubble_cm3;
    auto site_of = [&](u32 wall, u32 index) -> const RubbleSite* {
      for (const RubbleSite& s : field)
        if (s.wall == wall && s.index == index) return &s;
      return nullptr;
    };
    auto world_of = [&](const RubbleSite& s, f64& x, f64& z) {
      i64 wx = 0, wz = 0;
      field_of.to_world_cm(s.x_cm, s.z_cm, wx, wz);
      x = static_cast<f64>(wx);
      z = static_cast<f64>(wz);
    };
    std::vector<f64> wall_stones(wall_count, 0.0);
    std::vector<f64> wall_blocks(wall_count, 0.0);
    // The sections' debris: every piece on a site of its wall, within the site's radius.
    for (const Instance& inst : sections.instances) {
      if (inst.kind != static_cast<u8>(PieceKind::debris)) continue;
      ++pieces;
      const RubbleSite* site = site_of(inst.wall, inst.slot);
      REQUIRE(site != nullptr);
      const Member& mb = kit.members[inst.member];
      f64 x = 0.0, z = 0.0;
      world_of(*site, x, z);
      const f64 d = std::hypot(static_cast<f64>(inst.position.x) * 100.0 - x,
                               static_cast<f64>(inst.position.z) * 100.0 - z);
      CHECK(d <= kit.rubble_radius_cm - mb.radius_cm + 2.0);
      stone_cm3 += mb.volume_cm3;
      wall_stones[inst.wall] += mb.volume_cm3;
    }
    // The blocks: one fallen block a site, at its centre and turned by its yaw — pushed off it
    // only when it is longer than the site holds.
    std::vector<u32> taken(field.size(), 0u);
    for (const Block& block : laid_out.blocks) {
      if ((block.flags & k_block_fallen) == 0) continue;
      ++laid;
      const RubbleSite* site = site_of(block.wall, block.index);
      REQUIRE(site != nullptr);
      ++taken[static_cast<usize>(site - field.data())];
      const KitBlock& kb = blocks.blocks[block.block];
      CHECK(block.yaw == site->yaw);
      const f64 yaw = static_cast<f64>(block.yaw) * 22.5 * 3.14159265358979323846 / 180.0;
      const f64 cx =
          static_cast<f64>(block.position.x) * 100.0 + 0.5 * kb.length_cm * std::cos(yaw);
      const f64 cz =
          static_cast<f64>(block.position.z) * 100.0 - 0.5 * kb.length_cm * std::sin(yaw);
      f64 x = 0.0, z = 0.0;
      world_of(*site, x, z);
      if (kb.radius_cm <= kit.rubble_radius_cm) {
        CHECK(std::hypot(cx - x, cz - z) <= 2.0);
      } else {
        ++oversize;
      }
      const f64 v = static_cast<f64>(kb.length_cm) * kb.height_cm * kb.depth_cm;
      block_cm3 += v;
      wall_blocks[block.wall] += v;
    }
    for (usize i = 0; i < field.size(); ++i)
      CHECK(taken[i] <= 1u);
    for (u32 w = 0; w < wall_count; ++w)
      if (wall_blocks[w] > 0.0) wall_ratio.push_back(wall_stones[w] / wall_blocks[w]);
  }
  std::sort(wall_ratio.begin(), wall_ratio.end());
  const f64 ratio = stone_cm3 / block_cm3;
  MESSAGE("rubble over 80 buildings: "
          << sites << " sites; " << laid << " fallen blocks (" << oversize
          << " longer than a site) and " << pieces << " debris pieces; volume, blocks "
          << block_cm3 * 1e-6 << " m3, debris " << stone_cm3 * 1e-6 << " m3, sites "
          << site_cm3 * 1e-6 << " m3; debris over blocks " << ratio << ", per wall p10 "
          << wall_ratio[wall_ratio.size() / 10] << " median " << wall_ratio[wall_ratio.size() / 2]
          << " p90 " << wall_ratio[wall_ratio.size() * 9 / 10]);
  // Nearly every site takes its block (a long one may not fit), and the two forms put down the
  // same volume to within the pieces' own sizes.
  CHECK(sites > 200);
  CHECK(laid * 100 >= sites * 97);
  CHECK(laid <= sites);
  CHECK(ratio > 0.85);
  CHECK(ratio < 1.15);
}

TEST_CASE("ruins blocks: an opening is a gap in its courses with a lintel over it") {
  const Kit kit = section_kit(&open_rules);
  const BlockKit blocks = block_kit();
  BlockAssembler layer(kit, blocks);
  u32 openings = 0;
  u32 lintels = 0;
  u32 sills = 0;
  for (u32 b = 0; b < 60; ++b) {
    BlockOutput out;
    std::string error;
    REQUIRE(layer.assemble(placement_of(40 + b, static_cast<u8>(b & 15u)), TileCoord{-2, 0}, out,
                           &error));
    const Vector<Assembler::Side>& sides = layer.assembler().sides();
    CAPTURE(b);
    for (u32 w = 0; w < sides.size(); ++w) {
      const Assembler::Side& s = sides[w];
      if (s.opening == ~0u) continue;
      ++openings;
      const Member& mb = kit.members[s.opening];
      const f64 o0 = s.opening_at_cm + mb.opening_start_cm;
      const f64 o1 = s.opening_at_cm + mb.opening_end_cm;
      u32 lintel_here = 0;
      for (const Block& block : out.blocks) {
        if (block.wall != w || (block.flags & k_block_fallen) != 0) continue;
        const Along a = along_wall(layer, blocks, block);
        const i32 bottom = static_cast<i32>(block.course) * blocks.course_cm;
        const i32 top = bottom + blocks.course_cm;
        // Nothing in the gap across the courses the opening crosses.
        if (bottom < mb.head_cm && top > mb.sill_cm) {
          CHECK_FALSE_MESSAGE((a.u0 < o1 - 1.5 && a.u1 > o0 + 1.5),
                              "course " << static_cast<u32>(block.course) << " role "
                                        << static_cast<u32>(block.role) << " u " << a.u0 << ".."
                                        << a.u1 << " gap " << o0 << ".." << o1 << " wall length "
                                        << s.length_cm << " start " << s.start_cm);
        }
        if (block.role == static_cast<u8>(BlockRole::lintel)) {
          ++lintel_here;
          CHECK(a.u0 <= o0 - 5.0);  // bearing on both jambs
          CHECK(a.u1 >= o1 + 5.0);
          CHECK(bottom >= mb.head_cm);
        }
        if (block.role == static_cast<u8>(BlockRole::sill)) {
          ++sills;
          CHECK(top <= mb.sill_cm);
          CHECK(mb.sill_cm > 0);
        }
      }
      CHECK(lintel_here <= 1);
      lintels += lintel_here;
    }
  }
  MESSAGE("openings " << openings << ", lintels " << lintels << ", sills " << sills);
  CHECK(openings > 20);
  CHECK(lintels * 10 >= openings * 9);  // intact walls: the lintel stands over nearly every one
  CHECK(sills > 0);
}

TEST_CASE(
    "ruins blocks: fallen blocks lie beside the walls on the ground; the far tier drops them") {
  const Kit kit = section_kit();
  const BlockKit blocks = block_kit();
  BlockAssembler layer(kit, blocks);
  const i32 ht = blocks.thickness_cm / 2;
  u32 fallen = 0;
  for (u32 b = 0; b < 60; ++b) {
    const Placement near = placement_of(1300 + b, static_cast<u8>(b & 15u));
    Placement far = near;
    far.detail = Detail::walls;
    BlockOutput full;
    BlockOutput walls;
    std::string error;
    const TileCoord tile{2, 3};
    REQUIRE(layer.assemble(near, tile, full, &error));
    const Vector<Assembler::Side>& sides = layer.assembler().sides();
    const Assembler::Frame frame = layer.assembler().frame();
    CAPTURE(b);
    Vector<Block> kept;
    for (const Block& block : full.blocks) {
      if ((block.flags & k_block_fallen) == 0) {
        kept.push_back(block);
        continue;
      }
      ++fallen;
      const KitBlock& kb = blocks.blocks[block.block];
      // On the ground under where it lies, less the embed.
      const f32 ground = test_ground_m(block.position.x, block.position.z);
      CHECK(std::fabs(block.position.y - static_cast<f64>(ground)) <= 0.2);
      // Its centre, half its length along its yaw from its origin, is outside every wall by its
      // own radius and inside the tile.
      const f64 yaw = static_cast<f64>(block.yaw) * 22.5 * 3.14159265358979323846 / 180.0;
      const f64 cx = static_cast<f64>(block.position.x) + 0.005 * kb.length_cm * std::cos(yaw);
      const f64 cz = static_cast<f64>(block.position.z) - 0.005 * kb.length_cm * std::sin(yaw);
      CHECK(cx >= tile.x * 32.0);
      CHECK(cx <= tile.x * 32.0 + 32.0);
      CHECK(cz >= tile.z * 32.0);
      CHECK(cz <= tile.z * 32.0 + 32.0);
      const Local p = to_local(frame, cx, cz);
      const f64 r = kb.radius_cm - 2.0;
      for (const Assembler::Side& w : sides) {
        const f64 x1 = w.x0 + k_dx[w.dir] * w.length_cm;
        const f64 z1 = w.z0 + k_dz[w.dir] * w.length_cm;
        const bool within =
            p.x > std::min<f64>(w.x0, x1) - ht - r && p.x < std::max<f64>(w.x0, x1) + ht + r &&
            p.z > std::min<f64>(w.z0, z1) - ht - r && p.z < std::max<f64>(w.z0, z1) + ht + r;
        CHECK_FALSE(within);
      }
    }
    REQUIRE(layer.assemble(far, tile, walls, &error));
    REQUIRE(walls.blocks.size() == kept.size());
    for (u32 i = 0; i < kept.size(); ++i)
      CHECK(same(kept[i], walls.blocks[i]));
  }
  CHECK(fallen > 0);
}

TEST_CASE("ruins blocks: a block fragment is a scene of every block with its tag") {
  const Kit kit = section_kit();
  const BlockKit blocks = block_kit();
  BlockAssembler layer(kit, blocks);
  BlockOutput out;
  std::string error;
  REQUIRE(layer.assemble(placement_of(3), TileCoord{0, 0}, out, &error));
  REQUIRE(layer.assemble(placement_of(3), TileCoord{1, 0}, out, &error));
  scene::Scene scene;
  make_block_fragment(kit, blocks, out, "", "two ruins", scene);
  CHECK(scene.meshes.size() == blocks.meshes.size());
  REQUIRE(scene.instances.size() == out.blocks.size());
  CHECK(scene.ruin_sites.size() == 2);
  CHECK(scene.sand_drifts.size() == out.drifts.size());
  for (u32 i = 0; i < out.blocks.size(); ++i) {
    const Block& b = out.blocks[i];
    REQUIRE(scene.instances[i].ruin.has_value());
    REQUIRE(scene.instances[i].ruin->block.has_value());
    const scene::RuinBlockTag& tag = *scene.instances[i].ruin->block;
    CHECK(static_cast<u8>(tag.role) == b.role);
    CHECK(tag.course == b.course);
    CHECK(tag.fallen == ((b.flags & k_block_fallen) != 0));
    CHECK(scene.instances[i].mesh == blocks.blocks[b.block].mesh_index);
  }
  const JsonValue json = schema::to_json(scene);
  scene::Scene back;
  schema::ReadContext ctx;
  REQUIRE(schema::from_json(back, json, ctx));
  CHECK(ctx.ok());
  CHECK(back == scene);
}
