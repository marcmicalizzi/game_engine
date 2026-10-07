// The ruin assembler (docs/subsystems/ruins.md). A kit of boxes is made here, in memory, from
// `make_synthetic_kit`, so nothing binary is in the tree; the kit converts once and every case
// assembles from it. The cases are the page's invariants: the kit is refused with a sentence when
// it breaks the module or its sockets lie; one seed and tile is one building, on one thread or
// many; every join has a corner; no two walls overlap; a rectangle closes; a footprint fits its
// tile; openings never go on a wall that came down; debris lands on the terrain and outside every
// wall; the wind brings the windward walls down more; and the LOD policy drops debris and nothing
// else. The golden hash that says the same holds on every toolchain is in
// apps/engine_content/tests/determinism_tests.cpp, beside the content build's.
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
#include <string_view>
#include <vector>

using namespace engine;
using namespace engine::ruins;

namespace {

Kit kit_of(const SyntheticKitOptions& options) {
  SyntheticKit synthetic;
  make_synthetic_kit(options, synthetic);
  Kit kit;
  std::string error;
  const bool ok = kit_from_schema(synthetic.kit, "", kit, error);
  REQUIRE_MESSAGE(ok, error);
  return kit;
}

// A ground with relief and no C library in it: a tilted plane with a quadratic bump, so a
// building's corners stand at different heights and a debris block's own height differs from the
// base's.
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

bool same(const Instance& a, const Instance& b) {
  return std::memcmp(&a.position, &b.position, sizeof(WorldPos)) == 0 && a.member == b.member &&
         a.building == b.building && a.wall == b.wall && a.slot == b.slot &&
         a.height_q == b.height_q && a.kind == b.kind && a.yaw == b.yaw;
}

bool same(const Output& a, const Output& b) {
  if (a.instances.size() != b.instances.size() || a.drifts.size() != b.drifts.size() ||
      a.sites.size() != b.sites.size()) {
    return false;
  }
  for (u32 i = 0; i < a.instances.size(); ++i)
    if (!same(a.instances[i], b.instances[i])) return false;
  for (u32 i = 0; i < a.sites.size(); ++i) {
    const Site& x = a.sites[i];
    const Site& y = b.sites[i];
    if (x.seed != y.seed || !(x.tile == y.tile) || x.first_instance != y.first_instance ||
        x.instance_count != y.instance_count || x.walls != y.walls || x.shape != y.shape ||
        x.yaw != y.yaw) {
      return false;
    }
  }
  return hash_output(a) == hash_output(b);
}

// The building's frame, from the world: undo the origin and the yaw. Centimetres, as doubles.
struct Local {
  f64 x;
  f64 z;
};
Local to_local(const Site& site, f64 x, f64 z) {
  const f64 dx = (static_cast<f64>(x) - static_cast<f64>(site.origin.x)) * 100.0;
  const f64 dz = (static_cast<f64>(z) - static_cast<f64>(site.origin.z)) * 100.0;
  const f64 angle = static_cast<f64>(site.yaw) * 22.5 * 3.14159265358979323846 / 180.0;
  const f64 c = std::cos(angle);
  const f64 s = std::sin(angle);
  return Local{c * dx - s * dz, s * dx + c * dz};
}

constexpr i32 k_dx[4] = {1, 0, -1, 0};
constexpr i32 k_dz[4] = {0, -1, 0, 1};

struct RectD {
  f64 x0, z0, x1, z1;
};
RectD wall_rect(const Assembler::Side& s, f64 half) {
  const f64 x1 = s.x0 + k_dx[s.dir] * s.length_cm;
  const f64 z1 = s.z0 + k_dz[s.dir] * s.length_cm;
  return RectD{std::min<f64>(s.x0, x1) - half, std::min<f64>(s.z0, z1) - half,
               std::max<f64>(s.x0, x1) + half, std::max<f64>(s.z0, z1) + half};
}

}  // namespace

TEST_CASE("ruins: the synthetic kit converts, and a kit that breaks the module is refused") {
  const Kit kit = kit_of(SyntheticKitOptions{});
  CHECK(kit.module_cm == 200);
  CHECK(kit.thickness_cm == 60);
  CHECK(kit.corner_in_cm == 200);
  CHECK(kit.corner_out_cm == 200);
  CHECK_FALSE(kit.has_inside_corner);
  CHECK(kit.reflex_in_cm == kit.corner_out_cm);  // the outside corner turned half round
  REQUIRE(kit.section_modules.size() == 2);
  CHECK(kit.section_modules[0] == 1);
  CHECK(kit.section_modules[1] == 5);
  for (u32 n = 0; n <= kit.max_fill_modules; ++n)
    CHECK(kit.fillable[n] == 1);  // a one-module section fills anything
  CHECK(kit.by_kind[static_cast<u32>(PieceKind::doorway)].size() == 1);
  CHECK(kit.by_kind[static_cast<u32>(PieceKind::debris)].size() == 2);
  CHECK(kit.margin_cm > kit.rules.debris_spread_cm);
  // The rubble rule's unit: the profile's block (0.6 m x 0.3 m x the 0.6 m wall), and a radius
  // that holds both it (half its diagonal, 43 cm) and the largest debris piece (42 cm).
  CHECK(kit.course_height_cm == 30);
  CHECK(kit.block_length_cm == 60);
  CHECK(kit.rubble_cm3 == 60 * 30 * 60);
  CHECK(kit.rubble_radius_cm == 43);
  CHECK(kit.margin_cm >= kit.thickness_cm / 2 + kit.rules.debris_spread_cm + 2 * 43);
  {
    // The synthetic debris says its volume (a box's, exactly); a kit that says none is estimated
    // as the square prism of its height inscribed in its radius.
    const Member& block = kit.members[kit.by_kind[static_cast<u32>(PieceKind::debris)][0]];
    CHECK(block.volume_cm3 == 50 * 30 * 35);
    SyntheticKit synthetic;
    make_synthetic_kit(SyntheticKitOptions{}, synthetic);
    for (scene::RuinMember& m : synthetic.kit.members)
      m.volume = 0.0f;
    Kit estimated;
    std::string why;
    REQUIRE(kit_from_schema(synthetic.kit, "", estimated, why));
    const Member& guess =
        estimated.members[estimated.by_kind[static_cast<u32>(PieceKind::debris)][0]];
    CHECK(guess.volume_cm3 == 2 * guess.radius_cm * guess.radius_cm * guess.height_cm);
  }

  SyntheticKit synthetic;
  make_synthetic_kit(SyntheticKitOptions{}, synthetic);
  std::string error;
  Kit refused;
  {
    // E33's doorway: 3.2 m between its sockets in a 2 m module.
    scene::RuinKit file = synthetic.kit;
    for (scene::RuinMember& m : file.members) {
      if (m.kind == scene::RuinPieceKind::Doorway) m.sockets[1].position.x = 3.2f;
    }
    CHECK_FALSE(kit_from_schema(file, "", refused, error));
    CHECK(error.find("doorway") != std::string::npos);
    CHECK(error.find("module") != std::string::npos);
  }
  {
    // A corner whose incoming socket says the wall comes in from the outside.
    scene::RuinKit file = synthetic.kit;
    for (scene::RuinMember& m : file.members) {
      if (m.kind == scene::RuinPieceKind::Corner) m.sockets[0].outside = Vec3{1, 0, 0};
    }
    CHECK_FALSE(kit_from_schema(file, "", refused, error));
    CHECK(error.find("outside corner") != std::string::npos);
  }
  {
    scene::RuinKit file = synthetic.kit;
    file.members[0].yaw_deg = 10.0f;
    CHECK_FALSE(kit_from_schema(file, "", refused, error));
    CHECK(error.find("22.5") != std::string::npos);
  }
  {
    scene::RuinKit file = synthetic.kit;
    Vector<scene::RuinMember> kept;
    for (const scene::RuinMember& m : file.members)
      if (m.kind != scene::RuinPieceKind::Corner) kept.push_back(m);
    file.members = kept;
    CHECK_FALSE(kit_from_schema(file, "", refused, error));
    CHECK(error.find("no corner") != std::string::npos);
  }
  {
    scene::RuinKit file = synthetic.kit;
    file.rules.yaw_steps = 3;
    CHECK_FALSE(kit_from_schema(file, "", refused, error));
  }
  {
    // The rubble rule counts in the profile's blocks: a block of no size cannot be counted in.
    scene::RuinKit file = synthetic.kit;
    file.course_height = 0.0f;
    CHECK_FALSE(kit_from_schema(file, "", refused, error));
    CHECK(error.find("rubble") != std::string::npos);
  }
  {
    scene::RuinKit file = synthetic.kit;
    file.rules.debris_per_module = 100.0f;
    CHECK_FALSE(kit_from_schema(file, "", refused, error));
    CHECK(error.find("debris_per_module") != std::string::npos);
  }
  {
    scene::RuinKit file = synthetic.kit;
    for (scene::RuinMember& m : file.members)
      if (m.kind == scene::RuinPieceKind::Debris) m.volume = -1.0f;
    CHECK_FALSE(kit_from_schema(file, "", refused, error));
    CHECK(error.find("volume") != std::string::npos);
  }
}

TEST_CASE("ruins: the rubble field lies where the walls came down, clear of them and in the tile") {
  const Kit kit = kit_of(SyntheticKitOptions{});
  const f64 half = static_cast<f64>((kit.thickness_cm + 1) / 2);
  const f64 r = static_cast<f64>(kit.rubble_radius_cm);
  Assembler assembler(kit);
  Assembler far_assembler(kit);
  u32 sites = 0;
  u32 on_intact = 0;
  f64 removed_modules = 0.0;
  for (u32 b = 0; b < 250; ++b) {
    const TileCoord tile{static_cast<i32>(b % 13) - 6, static_cast<i32>(b / 13) - 9};
    const Placement placement = placement_of(4000 + b, static_cast<u8>(b & 15u));
    Output out;
    std::string error;
    REQUIRE(assembler.assemble(placement, tile, out, &error));
    const Vector<RubbleSite> field = assembler.rubble();
    // The field is the building's, whatever detail it was assembled at: the far tier asks for the
    // same sites.
    Placement far = placement;
    far.detail = Detail::walls;
    Output walls;
    REQUIRE(far_assembler.assemble(far, tile, walls, &error));
    const Vector<RubbleSite>& far_field = far_assembler.rubble();
    REQUIRE(far_field.size() == field.size());
    CHECK(std::memcmp(far_field.data(), field.data(), field.size() * sizeof(RubbleSite)) == 0);
    CAPTURE(b);
    const Vector<Assembler::Side>& sides = assembler.sides();
    const Assembler::Frame& frame = assembler.frame();
    // How much came down, in modules of wall fully brought down: the field's measure.
    for (u32 w = 0; w < sides.size(); ++w) {
      for (i32 u = 0; u < sides[w].length_cm; ++u)
        removed_modules += static_cast<f64>(k_q_one - assembler.height_q(w, u)) /
                           (static_cast<f64>(k_q_one) * kit.module_cm);
    }
    for (u32 i = 0; i < field.size(); ++i) {
      const RubbleSite& site = field[i];
      ++sites;
      REQUIRE(site.wall < sides.size());
      const Assembler::Side& s = sides[site.wall];
      on_intact += s.state == static_cast<u8>(WallState::intact) ? 1u : 0u;
      // Where its wall came down: the ruin line is below the intact wall there, within the eighth
      // of a module the field samples the line at.
      CHECK(site.along_cm >= 0);
      CHECK(site.along_cm <= s.length_cm);
      const i32 step = kit.module_cm / 8;
      CHECK(std::min({assembler.height_q(site.wall, site.along_cm),
                      assembler.height_q(site.wall, std::max(site.along_cm - step, 0)),
                      assembler.height_q(site.wall, std::min(site.along_cm + step, s.length_cm))}) <
            k_q_one);
      // Outside every wall by the site's radius, and inside the tile by it.
      for (const Assembler::Side& w : sides) {
        const RectD rc = wall_rect(w, half);
        const bool within = site.x_cm > rc.x0 - r && site.x_cm < rc.x1 + r &&
                            site.z_cm > rc.z0 - r && site.z_cm < rc.z1 + r;
        CHECK_FALSE(within);
      }
      i64 wx = 0, wz = 0;
      assembler.to_world_cm(site.x_cm, site.z_cm, wx, wz);
      CHECK(wx - kit.rubble_radius_cm >= frame.tile_x0_cm);
      CHECK(wx + kit.rubble_radius_cm <= frame.tile_x0_cm + frame.tile_cm);
      CHECK(wz - kit.rubble_radius_cm >= frame.tile_z0_cm);
      CHECK(wz + kit.rubble_radius_cm <= frame.tile_z0_cm + frame.tile_cm);
      // In draw order within its wall, walls in order.
      if (i > 0) {
        const RubbleSite& prev = field[i - 1];
        CHECK((prev.wall < site.wall || (prev.wall == site.wall && prev.index < site.index)));
      }
    }
  }
  // An intact wall drops nothing; and the sites are the rule's blocks per module brought down, less
  // the few that could not be put clear of the walls or inside the tile.
  CHECK(on_intact == 0);
  const f64 expected = removed_modules * kit.rules.debris_per_module_q / k_q_one;
  MESSAGE("rubble sites " << sites << " over 250 buildings; the rule asks for " << expected);
  CHECK(static_cast<f64>(sites) <= expected * 1.05 + 10.0);
  CHECK(static_cast<f64>(sites) >= expected * 0.85);
}

TEST_CASE("ruins: one seed and tile is one building, on one thread or many") {
  const Kit kit = kit_of(SyntheticKitOptions{});
  Vector<TileCoord> tiles;
  choose_tiles(7, TileCoord{-20, -20}, TileCoord{19, 19}, 300, 0.0f, tiles);
  REQUIRE(tiles.size() == 300);
  const Placement placement = placement_of(7, 3);
  std::string error;
  Output serial;
  REQUIRE(assemble_tiles(kit, placement, std::span<const TileCoord>(tiles.data(), tiles.size()),
                         nullptr, serial, &error));
  Output again;
  REQUIRE(assemble_tiles(kit, placement, std::span<const TileCoord>(tiles.data(), tiles.size()),
                         nullptr, again, &error));
  CHECK(same(serial, again));
  for (const u32 workers : {1u, 5u}) {
    jobs::JobSystem js(jobs::JobSystemConfig{
        .performance_workers = workers, .efficiency_workers = 1, .pin_threads = false});
    Output parallel;
    REQUIRE(assemble_tiles(kit, placement, std::span<const TileCoord>(tiles.data(), tiles.size()),
                           &js, parallel, &error));
    CHECK_MESSAGE(same(serial, parallel), "workers " << workers);
  }
  // One tile alone is the same building it is among three hundred.
  Output one;
  Assembler assembler(kit);
  REQUIRE(assembler.assemble(placement, tiles[123], one, &error));
  const Site& in_all = serial.sites[123];
  REQUIRE(one.sites.size() == 1);
  CHECK(one.sites[0].seed == in_all.seed);
  REQUIRE(one.sites[0].instance_count == in_all.instance_count);
  for (u32 i = 0; i < in_all.instance_count; ++i) {
    Instance a = one.instances[i];
    a.building = 123;
    CHECK(same(a, serial.instances[in_all.first_instance + i]));
  }
  // And a different world seed is a different desert.
  Output other;
  REQUIRE(assemble_tiles(kit, placement_of(8, 3),
                         std::span<const TileCoord>(tiles.data(), tiles.size()), nullptr, other,
                         &error));
  CHECK(hash_output(other) != hash_output(serial));
}

TEST_CASE("ruins: the grammar's properties hold over a thousand buildings") {
  struct Case {
    const char* name;
    SyntheticKitOptions options;
  };
  SyntheticKitOptions inside;
  inside.inside_corner = true;
  inside.corner_arm_in = 2.0f;
  inside.corner_arm_out = 2.0f;
  SyntheticKitOptions coarse;  // a 1 m module, no one-module section: fills must be searched
  coarse.module = 1.0f;
  coarse.short_section = 2;
  coarse.long_section = 3;
  coarse.corner_arm_in = 1.0f;
  coarse.corner_arm_out = 2.0f;
  const Case cases[] = {{"default", SyntheticKitOptions{}},
                        {"inside corners", inside},
                        {"no unit section", coarse},
                        {"e33 sizes", e33_sized_options()}};
  for (const Case& c : cases) {
    const std::string case_name = c.name;
    CAPTURE(case_name);
    const Kit kit = kit_of(c.options);
    const f64 half = static_cast<f64>((kit.thickness_cm + 1) / 2);
    Assembler assembler(kit);
    u32 shapes[4] = {0, 0, 0, 0};
    u32 openings = 0;
    u32 debris = 0;
    for (u32 b = 0; b < 250; ++b) {
      const TileCoord tile{static_cast<i32>(b % 17) - 8, static_cast<i32>(b / 17) - 7};
      Output out;
      std::string error;
      REQUIRE_MESSAGE(
          assembler.assemble(placement_of(1000 + b, static_cast<u8>(b & 15u)), tile, out, &error),
          error);
      REQUIRE(out.sites.size() == 1);
      const Site& site = out.sites[0];
      const Vector<Assembler::Side>& sides = assembler.sides();
      CAPTURE(b);
      CAPTURE(shape_name(assembler.shape()));
      ++shapes[site.shape];
      REQUIRE(site.walls == sides.size());

      // A ring closes: its walls, walked in order, come back to where they started.
      for (u8 ring = 0; ring < 2; ++ring) {
        i64 x = 0, z = 0;
        bool any = false;
        for (const Assembler::Side& s : sides) {
          if (s.ring != ring) continue;
          if (!any) {
            x = s.x0;
            z = s.z0;
            any = true;
          }
          CHECK(x == s.x0);
          CHECK(z == s.z0);
          x += k_dx[s.dir] * s.length_cm;
          z += k_dz[s.dir] * s.length_cm;
          CHECK(s.fill_cm % kit.module_cm == 0);
          CHECK(s.start_cm + s.fill_cm + s.end_cm == s.length_cm);
        }
        if (!any) continue;
        const Assembler::Side& first = *std::find_if(
            sides.begin(), sides.end(), [&](const Assembler::Side& s) { return s.ring == ring; });
        CHECK(x == first.x0);
        CHECK(z == first.z0);
      }

      // No two walls overlap: every pair but neighbours in one ring is disjoint.
      for (u32 i = 0; i < sides.size(); ++i) {
        for (u32 j = i + 1; j < sides.size(); ++j) {
          if (sides[i].ring == sides[j].ring) {
            u32 first = i, last = i;
            while (first > 0 && sides[first - 1].ring == sides[i].ring)
              --first;
            while (last + 1 < sides.size() && sides[last + 1].ring == sides[i].ring)
              ++last;
            if (j == i + 1 || (i == first && j == last)) continue;
          }
          const RectD a = wall_rect(sides[i], half);
          const RectD d = wall_rect(sides[j], half);
          const bool overlap = a.x0 < d.x1 && d.x0 < a.x1 && a.z0 < d.z1 && d.z0 < a.z1;
          CHECK_FALSE(overlap);
        }
      }

      // Every join has its corner: one corner piece per wall, at the wall's start vertex.
      Vector<u32> corners(sides.size(), 0u);
      const f64 tile_x0 = tile.x * 32.0;
      const f64 tile_z0 = tile.z * 32.0;
      for (u32 i = 0; i < site.instance_count; ++i) {
        const Instance& inst = out.instances[site.first_instance + i];
        const PieceKind kind = static_cast<PieceKind>(inst.kind);
        const Member& member = kit.members[inst.member];
        CHECK(static_cast<u8>(member.kind) == inst.kind);
        // The footprint, its debris and its drift fit the tile.
        CHECK(inst.position.x >= tile_x0);
        CHECK(inst.position.x <= tile_x0 + 32.0);
        CHECK(inst.position.z >= tile_z0);
        CHECK(inst.position.z <= tile_z0 + 32.0);
        const Local p = to_local(site, inst.position.x, inst.position.z);
        const Assembler::Side& s = sides[inst.wall];
        // Nothing that stands floats: a member's base is at or under the ground where it stands.
        if (kind != PieceKind::debris) {
          CHECK(inst.position.y <=
                static_cast<f64>(test_ground_m(inst.position.x, inst.position.z) + 1e-4f));
        }
        if (kind == PieceKind::corner || kind == PieceKind::inside_corner) {
          ++corners[inst.wall];
          CHECK(inst.slot == 0);
          CHECK(std::fabs(p.x - s.x0) <= 1.5);
          CHECK(std::fabs(p.z - s.z0) <= 1.5);
          CHECK(inst.position.y <= site.origin.y + 1e-4);  // on the base, or sunk into it
          continue;
        }
        if (kind == PieceKind::debris) {
          ++debris;
          // On the terrain where it lies, less the embed.
          const f32 ground = test_ground_m(inst.position.x, inst.position.z);
          const f32 expected =
              static_cast<f32>(std::floor(ground * 100.0f) - kit.rules.embed_cm) * 0.01f;
          CHECK(inst.position.y == doctest::Approx(expected).epsilon(1e-6));
          // Outside every wall, by its own radius, to within the centimetre the yaw rounds to.
          const f64 r = member.radius_cm - 2.0;
          for (const Assembler::Side& w : sides) {
            const RectD rc = wall_rect(w, half);
            const bool within =
                p.x > rc.x0 - r && p.x < rc.x1 + r && p.z > rc.z0 - r && p.z < rc.z1 + r;
            CHECK_FALSE(within);
          }
          continue;
        }
        // A section or an opening stands on its wall, inside the fill between the corners.
        const f64 along = (p.x - s.x0) * k_dx[s.dir] + (p.z - s.z0) * k_dz[s.dir];
        const f64 across =
            (p.x - s.x0) * k_dx[(s.dir + 1) & 3] + (p.z - s.z0) * k_dz[(s.dir + 1) & 3];
        CHECK(std::fabs(across) <= 1.5);
        CHECK(along >= s.start_cm - 1.5);
        CHECK(along + member.length_cm <= s.start_cm + s.fill_cm + 1.5);
        if (kind == PieceKind::doorway || kind == PieceKind::window) {
          ++openings;
          // Never on a wall that came down, and the wall stands intact over it.
          CHECK(s.state != static_cast<u8>(WallState::collapsed));
          CHECK(inst.height_q == k_q_one);
          CHECK(inst.position.y == doctest::Approx(site.origin.y));
        }
      }
      for (u32 w = 0; w < sides.size(); ++w)
        CHECK_MESSAGE(corners[w] == 1, "wall " << w);
      // Sections of one wall never overlap each other or an opening.
      for (u32 w = 0; w < sides.size(); ++w) {
        std::vector<std::pair<f64, f64>> spans;
        for (u32 i = 0; i < site.instance_count; ++i) {
          const Instance& inst = out.instances[site.first_instance + i];
          const PieceKind kind = static_cast<PieceKind>(inst.kind);
          if (inst.wall != w || kind == PieceKind::corner || kind == PieceKind::inside_corner ||
              kind == PieceKind::debris) {
            continue;
          }
          const Local p = to_local(site, inst.position.x, inst.position.z);
          const Assembler::Side& s = sides[w];
          const f64 along = (p.x - s.x0) * k_dx[s.dir] + (p.z - s.z0) * k_dz[s.dir];
          spans.push_back({along, along + kit.members[inst.member].length_cm});
        }
        std::sort(spans.begin(), spans.end());
        for (usize k = 1; k < spans.size(); ++k)
          CHECK(spans[k].first >= spans[k - 1].second - 1.5);
      }
      // One drift per wall face.
      CHECK(site.drift_count == sides.size());
    }
    MESSAGE(std::string(c.name) << ": rectangles " << shapes[0] << ", L " << shapes[1] << ", U "
                                << shapes[2] << ", courtyards " << shapes[3] << "; openings "
                                << openings << ", debris " << debris);
    CHECK(shapes[0] > 0);
    CHECK(debris > 0);
    if (std::string_view(c.name) == "e33 sizes") {
      // Its corner's arms (2.68 m and 3.68 m on the centre line) leave an L's and a U's closing
      // walls a fraction of the 2 m module, so those fall back to rectangles; a courtyard's inner
      // ring turns right at all four corners, where the two arms swap and cancel, so it closes.
      CHECK(shapes[1] == 0);
      CHECK(shapes[2] == 0);
      CHECK(shapes[3] > 0);
    } else {
      CHECK(shapes[1] > 0);
      CHECK(shapes[2] > 0);
      CHECK(shapes[3] > 0);
      CHECK(openings > 0);
    }
  }
}

TEST_CASE("ruins: the windward walls come down more than the lee") {
  const Kit kit = kit_of(SyntheticKitOptions{});
  Assembler assembler(kit);
  u32 windward = 0, windward_down = 0, lee = 0, lee_down = 0;
  for (u32 b = 0; b < 600; ++b) {
    Output out;
    std::string error;
    REQUIRE(assembler.assemble(placement_of(55 + b, static_cast<u8>(b % 16)), TileCoord{0, 0}, out,
                               &error));
    for (const Assembler::Side& s : assembler.sides()) {
      const bool down = s.state == static_cast<u8>(WallState::collapsed);
      if (s.facing == 0) {
        ++windward;
        windward_down += down ? 1u : 0u;
      } else if (s.facing == 2 && s.ring == 0) {
        ++lee;
        lee_down += down ? 1u : 0u;
      }
    }
  }
  const f64 w = static_cast<f64>(windward_down) / windward;
  const f64 l = static_cast<f64>(lee_down) / lee;
  MESSAGE("collapsed: windward " << w << " of " << windward << ", lee " << l << " of " << lee);
  CHECK(w > l + 0.1);
}

TEST_CASE("ruins: the far tier keeps every wall and drops only the debris") {
  const Kit kit = kit_of(SyntheticKitOptions{});
  Assembler assembler(kit);
  CHECK(detail_for_distance(100.0f, 32.0f) == Detail::full);
  CHECK(detail_for_distance(300.0f, 32.0f) == Detail::walls);
  for (u32 b = 0; b < 40; ++b) {
    Placement near = placement_of(900 + b);
    Placement far = near;
    far.detail = Detail::walls;
    Output full, walls;
    std::string error;
    REQUIRE(assembler.assemble(near, TileCoord{3, -2}, full, &error));
    REQUIRE(assembler.assemble(far, TileCoord{3, -2}, walls, &error));
    Vector<Instance> kept;
    for (const Instance& i : full.instances)
      if (i.kind != static_cast<u8>(PieceKind::debris)) kept.push_back(i);
    REQUIRE(kept.size() == walls.instances.size());
    for (u32 i = 0; i < kept.size(); ++i)
      CHECK(same(kept[i], walls.instances[i]));
  }
}

TEST_CASE("ruins: tiles are chosen by rank, and count and density agree") {
  Vector<TileCoord> by_count;
  Vector<TileCoord> by_density;
  choose_tiles(42, TileCoord{-10, -10}, TileCoord{9, 9}, 25, 0.0f, by_count);
  choose_tiles(42, TileCoord{-10, -10}, TileCoord{9, 9}, 0, 0.1f, by_density);
  REQUIRE(by_count.size() == 25);
  CHECK(by_density.size() > 10);
  CHECK(by_density.size() < 80);
  for (u32 i = 1; i < by_count.size(); ++i)
    CHECK(tile_rank(42, by_count[i - 1]) <= tile_rank(42, by_count[i]));
  // The density's tiles are exactly the lowest-ranked ones: a count asks for a prefix of them.
  const u32 n = std::min<u32>(by_count.size(), by_density.size());
  for (u32 i = 0; i < n; ++i)
    CHECK(by_count[i] == by_density[i]);
  for (const TileCoord& t : by_density)
    CHECK(tile_has_building(42, t, 0.1f));
  Vector<TileCoord> again;
  choose_tiles(42, TileCoord{-10, -10}, TileCoord{9, 9}, 25, 0.0f, again);
  for (u32 i = 0; i < again.size(); ++i)
    CHECK(again[i] == by_count[i]);
}

TEST_CASE("ruins: a fragment is a scene with every piece, its tag, its site and its drift") {
  const Kit kit = kit_of(SyntheticKitOptions{});
  Output out;
  std::string error;
  Assembler assembler(kit);
  REQUIRE(assembler.assemble(placement_of(3), TileCoord{0, 0}, out, &error));
  REQUIRE(assembler.assemble(placement_of(3), TileCoord{1, 0}, out, &error));
  scene::Scene scene;
  make_fragment(kit, out, "", "two ruins", scene);
  CHECK(scene.format == "engine.scene.v1");
  CHECK(scene.meshes.size() == kit.meshes.size());
  REQUIRE(scene.instances.size() == out.instances.size());
  CHECK(scene.ruin_sites.size() == 2);
  CHECK(scene.sand_drifts.size() == out.drifts.size());
  for (u32 i = 0; i < out.instances.size(); ++i) {
    REQUIRE(scene.instances[i].ruin.has_value());
    CHECK(scene.instances[i].ruin->building == out.instances[i].building);
    CHECK(static_cast<u8>(scene.instances[i].ruin->kind) == out.instances[i].kind);
    CHECK(scene.instances[i].mesh == kit.members[out.instances[i].member].mesh_index);
  }
  // It round-trips through its schema, which is what the renderer's scene reader reads it with.
  const JsonValue json = schema::to_json(scene);
  scene::Scene back;
  schema::ReadContext ctx;
  REQUIRE(schema::from_json(back, json, ctx));
  CHECK(ctx.ok());
  CHECK(back == scene);
}

namespace {

f64 flat_ground(const void*, i64, i64) noexcept { return 0.75; }

}  // namespace

// **A wall's drift stands where its centimetres say, anywhere** (ADR-0053; ruins.md, "Far from the
// origin"; 2026-10-07). The tile grid's corner moved to the owner's 419 km, to 10,000 km and to
// 1e8 m under the same seeds (`Placement::origin_x_cm`), and once more 48 m past 10,000 km so a
// tile straddles a cell's edge there: every building's drifts are the origin's, each end moved by
// exactly the corner's centimetres and each the integer centimetres divided once in f64 at the
// building's base; and a fragment written of them reads back as the same doubles. Until
// 2026-10-07 a drift's ends were float32 metres of the world, a 3.1 cm grid at 419 km and a
// metre's at 10,000 km.
TEST_CASE("ruins: a wall's drift far out is its twin by the origin, moved by whole centimetres") {
  const Kit kit = kit_of(SyntheticKitOptions{});
  const TileCoord tiles[] = {{0, 0}, {1, 0}, {0, 1}, {-1, -1}};
  const auto build = [&](i64 ox_cm, i64 oz_cm, Output& out) {
    Placement p = placement_of(3);
    p.ground = Ground{&flat_ground, nullptr};
    p.origin_x_cm = ox_cm;
    p.origin_z_cm = oz_cm;
    Assembler assembler(kit);
    std::string error;
    for (const TileCoord t : tiles)
      REQUIRE_MESSAGE(assembler.assemble(p, t, out, &error), error);
  };
  Output home;
  build(0, 0, home);
  REQUIRE(home.drifts.size() > 8u);
  const auto cm = [](f64 metres) { return std::llround(metres * 100.0); };
  const auto exact = [](f64 metres, i64 centimetres) {
    return metres == static_cast<f64>(centimetres) / 100.0;  // divided once
  };
  const i64 corners_cm[] = {41'907'200, 1'000'000'000, 10'000'000'000, 1'000'004'800};
  for (const i64 c : corners_cm) {
    Output far;
    build(c, -c, far);
    REQUIRE(far.drifts.size() == home.drifts.size());
    u32 moved_wrong = 0, not_exact = 0, other_wrong = 0;
    for (u32 i = 0; i < far.drifts.size(); ++i) {
      const Drift& a = home.drifts[i];
      const Drift& b = far.drifts[i];
      for (const bool to : {false, true}) {
        const WorldPos pa = to ? a.to : a.from;
        const WorldPos pb = to ? b.to : b.from;
        moved_wrong +=
            cm(pb.x) == cm(pa.x) + c && cm(pb.z) == cm(pa.z) - c && pb.y == pa.y ? 0u : 1u;
        not_exact +=
            exact(pb.x, cm(pb.x)) && exact(pb.z, cm(pb.z)) && exact(pb.y, cm(pb.y)) ? 0u : 1u;
      }
      other_wrong += std::memcmp(&a.normal, &b.normal, sizeof(Vec2)) == 0 && a.height == b.height &&
                             a.reach == b.reach && a.building == b.building && a.wall == b.wall &&
                             a.windward == b.windward
                         ? 0u
                         : 1u;
    }
    // The site's base is the drift's height on the ground.
    for (const Site& site : far.sites) {
      for (u32 i = site.first_drift; i < site.first_drift + site.drift_count; ++i)
        other_wrong += far.drifts[i].from.y == site.origin.y ? 0u : 1u;
    }
    // Written into a fragment and read back through the schema, the ends are the same doubles.
    scene::Scene scene;
    make_fragment(kit, far, "", "far ruins", scene);
    scene::Scene back;
    schema::ReadContext ctx;
    REQUIRE(schema::from_json(back, schema::to_json(scene), ctx));
    REQUIRE(back.sand_drifts.size() == far.drifts.size());
    u32 read_wrong = 0;
    for (u32 i = 0; i < far.drifts.size(); ++i) {
      read_wrong += back.sand_drifts[i].from == far.drifts[i].from &&
                            back.sand_drifts[i].to == far.drifts[i].to
                        ? 0u
                        : 1u;
    }
    MESSAGE("the grid's corner " << c << " cm out: " << far.drifts.size() << " drifts, "
                                 << moved_wrong << " ends not moved by the corner, " << not_exact
                                 << " not their centimetres divided once, " << read_wrong
                                 << " read back otherwise");
    CHECK(moved_wrong == 0u);
    CHECK(not_exact == 0u);
    CHECK(other_wrong == 0u);
    CHECK(read_wrong == 0u);
  }
}

TEST_CASE("ruins: the synthetic kit's meshes are GLBs") {
  SyntheticKit kit;
  make_synthetic_kit(SyntheticKitOptions{}, kit);
  REQUIRE(kit.glb.size() == kit.mesh_names.size());
  REQUIRE(kit.glb.size() == kit.kit.members.size());
  for (const Vector<u8>& glb : kit.glb) {
    REQUIRE(glb.size() > 20);
    CHECK(glb[0] == 'g');
    CHECK(glb[1] == 'l');
    CHECK(glb[2] == 'T');
    CHECK(glb[3] == 'F');
    const u32 length = u32{glb[8]} | u32{glb[9]} << 8 | u32{glb[10]} << 16 | u32{glb[11]} << 24;
    CHECK(length == glb.size());
  }
}
