// The profile solver against the closed forms (channels.md, "The profile", "The fixture"; roadmap
// W120 stage B): the fixture's ground solved by the standard step method and held to the
// fixture's own numbers, which come from direct evaluation and the direct step method; then what
// the fixture has none of — a bed too small for its water, a section that opens into a pool, a
// fall drowned by its tailwater, a line with no width on a ground it knows nothing of.
#include <core/hash/hash.h>
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/channels/flow_fixture.h>
#include <domain/channels/hydraulics.h>
#include <domain/channels/profile.h>
#include <domain/channels/stations.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::channels;

namespace {

const Trapezoid k_t{2.0, 1.0};

f64 fixture_height(const void*, i64 x_mm, i64 z_mm) noexcept {
  return flow_fixture_ground(static_cast<f64>(x_mm) / 1000.0, static_cast<f64>(z_mm) / 1000.0);
}
scene_gen::Ground fixture_ground() {
  scene_gen::Ground g;
  g.surface_fn = &fixture_height;
  return g;
}

FlowLine fixture_line(const char* name, f64 x0, f64 x1, f64 z, f64 q) {
  FlowLine line;
  line.name = name;
  line.points.push_back(WorldPos{x0, 0.0, z});
  line.points.push_back(WorldPos{x1, 0.0, z});
  line.discharge.push_back(q);
  return line;
}

ProfileOptions manning() {
  ProfileOptions o;
  o.resistance = Resistance::Manning;
  o.manning_n = 0.04;
  o.reach_m = 6.0;
  return o;
}

// The solved level at a place along a line running +x, between its stations (never across a
// pair).
f64 level_at(const Profile& p, f64 x) {
  for (u32 i = 0; i + 1 < p.stations.size(); ++i) {
    const ProfileStation& a = p.stations[i];
    const ProfileStation& b = p.stations[i + 1];
    if (x < a.x - 1e-9 || x > b.x + 1e-9) continue;
    return a.level + (b.level - a.level) * (b.x > a.x ? (x - a.x) / (b.x - a.x) : 0.0);
  }
  return 0.0;
}

// A trench in a plane: a flat bed `bed_half` either side of z = 0, banks rising 1:1 for `bank`
// metres, the plane beyond falling away from the banks' tops; the bed falls `slope` along +x.
struct Trench {
  f64 bed_half = 1.0;
  f64 bank = 0.25;
  f64 slope = 0.01;
  f64 wide_from_x = 1e9;  // from here on the flat bed is `wide_half` either side
  f64 wide_half = 1.0;
};
f64 trench_height(const void* context, i64 x_mm, i64 z_mm) noexcept {
  const Trench& t = *static_cast<const Trench*>(context);
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = std::fabs(static_cast<f64>(z_mm) / 1000.0);
  const f64 half = x >= t.wide_from_x ? t.wide_half : t.bed_half;
  const f64 bed = 20.0 - t.slope * x;
  const f64 rise = std::max(0.0, z - half);
  // Past the bank's top the ground falls away at one in ten.
  return rise <= t.bank ? bed + rise : bed + t.bank - 0.1 * (rise - t.bank);
}

// A trench whose bed is a polyline along +x: a flat bed `half` either side of z = 0 between banks
// rising 1:1 for 3 m. From `shelf_x0` to `shelf_x1` the left bank is instead a hump 12 cm high a
// lattice from the bed, a shelf 4 m wide behind it standing 8 cm over the bed, and then the bank:
// a level that passes the hump wets the shelf at once.
struct Long {
  f64 x[8]{};
  f64 y[8]{};
  u32 count = 0;
  f64 half = 1.0;
  f64 shelf_x0 = 1e9, shelf_x1 = -1e9;
};
f64 long_height(const void* context, i64 x_mm, i64 z_mm) noexcept {
  const Long& g = *static_cast<const Long*>(context);
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  f64 bed = x <= g.x[0] ? g.y[0] : g.y[g.count - 1];
  for (u32 i = 0; i + 1 < g.count; ++i)
    if (x >= g.x[i] && x <= g.x[i + 1])
      bed = g.y[i] + (g.y[i + 1] - g.y[i]) * (x - g.x[i]) / (g.x[i + 1] - g.x[i]);
  const f64 d = std::fabs(z) - g.half;
  if (d <= 0.0) return bed;
  if (z > 0.0 && x >= g.shelf_x0 && x <= g.shelf_x1) {
    if (d <= 0.25) return bed + 0.48 * d;
    if (d <= 0.5) return bed + 0.12 - 0.16 * (d - 0.25);
    if (d <= 4.5) return bed + 0.08;
    return bed + 0.08 + std::min(d - 4.5, 3.0);
  }
  return bed + std::min(d, 3.0);
}
scene_gen::Ground long_ground(const Long& g) {
  scene_gen::Ground ground;
  ground.surface_fn = &long_height;
  ground.context = &g;
  return ground;
}
f64 head_of(const ProfileStation& s) { return s.level + s.speed * s.speed / (2.0 * k_gravity); }
// The total head never rises from one station to the next, a fall's lip and foot included.
void check_head_falls(const Profile& p) {
  for (u32 i = 0; i + 1 < p.stations.size(); ++i) {
    const ProfileStation& a = p.stations[i];
    const ProfileStation& b = p.stations[i + 1];
    CHECK_MESSAGE(head_of(b) <= head_of(a) + 0.001,
                  "station " << i << " (flags " << a.flags << ", level " << a.level << ", head "
                             << head_of(a) << ") to " << i + 1 << " (flags " << b.flags
                             << ", level " << b.level << ", head " << head_of(b) << ")");
  }
}

}  // namespace

TEST_CASE("profile: a uniform reach stands at normal depth, to rounding") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  const f64 q = f.numbers.discharge_m3s;
  Profile p;
  REQUIRE_MESSAGE(
      solve_profile(fixture_line("uniform", 8, 48, -20, q), fixture_ground(), manning(), p, error),
      error);
  REQUIRE(p.stations.size() == 6);
  CHECK(p.stats.nodes == 161);
  f64 worst = 0.0;
  for (const ProfileStation& s : p.stations) {
    worst = std::max(worst, std::fabs(s.level - (10.0 - 0.01 * (s.x - 8.0) + 0.5)));
    CHECK(s.flags == 0);
    CHECK(s.friction == doctest::Approx(0.01).epsilon(1e-6));
    CHECK(s.speed == doctest::Approx(q / 1.25).epsilon(1e-6));
    CHECK(s.wet_left == doctest::Approx(1.5).epsilon(1e-6));
    CHECK(s.wet_right == doctest::Approx(1.5).epsilon(1e-6));
    CHECK(s.over_m == 0.0);
  }
  MESSAGE("uniform reach: the level's worst distance from bed + y_n is " << worst << " m");
  CHECK(worst < 1e-9);
  CHECK(p.stats.rises_other == 0);
  CHECK(p.stats.head_rises == 0);
  CHECK(p.stats.falls == 0);
  CHECK(p.stats.jumps == 0);
}

TEST_CASE("profile: a mild reach into a lake is the backwater integral") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  const f64 q = f.numbers.discharge_m3s;
  FlowLine line = fixture_line("mouth", 8, 88, 20, q);
  line.tail_level = true;
  line.level = f.numbers.lake_level_m;
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, fixture_ground(), manning(), p, error), error);
  // The integral: dx = dE / (S_0 - S_f) from the mouth's depth, in steps of depth a hundredth of
  // the fixture's.
  DepthTable integral;
  direct_step(k_t, q, 0.04, 0.01, 1.2, 0.5 + 1e-4 * 0.7, 400000, integral);
  f64 worst = 0.0;
  for (const ProfileStation& s : p.stations) {
    const f64 depth = depth_at(integral, s.x - 88.0);
    worst = std::max(worst, std::fabs(s.depth - depth));
  }
  MESSAGE("M1: " << p.stations.size() << " stations, the depth's worst distance from the "
                 << "integral " << worst << " m");
  CHECK(worst < 0.0006);  // the description's millimetre
  const ProfileStation& mouth = p.stations[p.stations.size() - 1];
  CHECK(mouth.level == f.numbers.lake_level_m);
  CHECK(mouth.flags == scene_gen::channel_control_flags(scene_gen::ChannelControl::Mouth));
  CHECK(p.stations[0].depth == doctest::Approx(f.numbers.lake_head_m).epsilon(1e-3));
  CHECK(p.stats.rises_other == 0);
  CHECK(p.stats.rises_shooting == 0);
  CHECK(p.stats.head_rises == 0);
  // Its stations are the fixture's own, place for place.
  const scene::WaterChannel& fixture = f.channels[2];
  REQUIRE(p.stations.size() == fixture.points.size());
  for (u32 i = 0; i < p.stations.size(); ++i) {
    CHECK(p.stations[i].x == fixture.points[i].x);
    CHECK(std::fabs(p.stations[i].level - fixture.points[i].y) <= 0.001 + 1e-9);
  }
}

TEST_CASE("profile: a step is critical at its crest, falls, shoots, jumps and meets its pool") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  const FlowFixtureNumbers& n = f.numbers;
  const f64 q = n.discharge_m3s;
  FlowLine line = fixture_line("step", 8, 58, 0, q);
  line.tail_level = true;
  line.level = n.pool_level_m;
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, fixture_ground(), manning(), p, error), error);
  const scene::WaterChannel block = describe_profile("step", p, 12);
  REQUIRE_MESSAGE(scene_gen::water_channel_solved(block, &error), error);
  i32 lip = -1, head = -1;
  for (u32 i = 0; i < p.stations.size(); ++i) {
    if ((p.stations[i].flags & scene_gen::k_channel_fall_lip) != 0) lip = static_cast<i32>(i);
    if ((p.stations[i].flags & scene_gen::k_channel_jump_head) != 0) head = static_cast<i32>(i);
  }
  REQUIRE(lip > 0);
  REQUIRE(head > lip);
  CHECK(p.stats.falls == 1);
  CHECK(p.stats.jumps == 1);
  CHECK(p.stats.drowned_feet == 0);
  // The crest: critical depth, the closed form's, and a control.
  const ProfileStation& crest = p.stations[static_cast<u32>(lip)];
  CHECK(crest.x == 38.0);
  CHECK(crest.depth == doctest::Approx(n.critical_m).epsilon(2e-3));
  CHECK(crest.froude == doctest::Approx(1.0).epsilon(2e-3));
  CHECK(crest.flags == (scene_gen::channel_control_flags(scene_gen::ChannelControl::Crest) |
                        scene_gen::k_channel_fall_lip));
  MESSAGE("crest: depth " << crest.depth << " against the closed form's " << n.critical_m);
  // The approach: the M2 curve the direct step integrates, at the fixture's own stations.
  f64 worst = 0.0;
  const scene::WaterChannel& fixture = f.channels[1];
  for (u32 i = 0; i < fixture.points.size() && fixture.points[i].x <= 38.0; ++i)
    worst = std::max(worst, std::fabs(level_at(p, fixture.points[i].x) - fixture.points[i].y));
  MESSAGE("M2: the level's worst distance from the fixture's stations " << worst << " m");
  CHECK(worst < 0.0025);
  // The foot: Rand's drop, where the fixture has it.
  const ProfileStation& foot = p.stations[static_cast<u32>(lip) + 1];
  CHECK(foot.flags == scene_gen::k_channel_fall_foot);
  CHECK(foot.x == n.foot_x_m);
  CHECK(foot.depth == doctest::Approx(n.foot_m).epsilon(5e-3));
  CHECK(foot.froude == doctest::Approx(n.foot_froude).epsilon(1e-2));
  CHECK(p.stats.highest_fall_m == doctest::Approx(crest.level - foot.level));
  // The jump: where the fixture has it, its foot the head's sequent depth by the closed form.
  const ProfileStation& jump_head = p.stations[static_cast<u32>(head)];
  const ProfileStation& jump_foot = p.stations[static_cast<u32>(head) + 1];
  CHECK(jump_foot.flags == scene_gen::k_channel_jump_foot);
  MESSAGE("jump: head at x " << jump_head.x << " depth " << jump_head.depth << ", foot at x "
                             << jump_foot.x << " depth " << jump_foot.depth << "; the fixture's "
                             << n.jump_head_x_m << " " << n.jump_head_m << ", " << n.jump_foot_x_m
                             << " " << n.jump_foot_m);
  CHECK(std::fabs(jump_head.x - n.jump_head_x_m) < 0.05);
  CHECK(std::fabs(jump_head.depth - n.jump_head_m) < 0.002);
  CHECK(std::fabs(jump_foot.x - n.jump_foot_x_m) < 0.06);
  CHECK(jump_foot.depth ==
        doctest::Approx(trapezoid_sequent(k_t, jump_head.depth, q)).epsilon(2e-3));
  CHECK(trapezoid_force(k_t, jump_foot.depth, q) ==
        doctest::Approx(trapezoid_force(k_t, jump_head.depth, q)).epsilon(5e-3));
  CHECK(jump_foot.x - jump_head.x ==
        doctest::Approx(k_jump_length_rises * (jump_foot.depth - jump_head.depth)).epsilon(2e-2));
  // The mouth: the pool's level to the millimetre.
  const ProfileStation& mouth = p.stations[p.stations.size() - 1];
  CHECK(mouth.level == n.pool_level_m);
  CHECK(mouth.flags == scene_gen::channel_control_flags(scene_gen::ChannelControl::Mouth));
  // The surface rises only across the jump and along the shooting run above it; the head never.
  CHECK(p.stats.rises_jump == 1);
  CHECK(p.stats.rises_other == 0);
  CHECK(p.stats.head_rises == 0);
  CHECK(p.stats.rise_shooting_worst_m < 0.06);
  for (u32 i = 0; i + 1 < p.stations.size(); ++i) {
    const ProfileStation& a = p.stations[i];
    const ProfileStation& b = p.stations[i + 1];
    if (a.froude < 1.0 && b.froude < 1.0 && (a.flags & scene_gen::k_channel_fall_lip) == 0)
      CHECK(b.level <= a.level);
  }
  // The same bytes twice, and the bytes MSVC, GCC and Clang agree on.
  Profile again;
  REQUIRE(solve_profile(line, fixture_ground(), manning(), again, error));
  JsonWriteOptions flat;
  flat.pretty = false;
  const std::string text = write_json(schema::to_json(block), flat);
  CHECK(text == write_json(schema::to_json(describe_profile("step", again, 12)), flat));
  // Taken on MSVC (msvc-debug, 2026-11-13); linux-clang-debug, linux-gcc-release and
  // linux-gcc-release-v2 reproduce it. (Taken again the same night, from 8605109305383583938:
  // a jump's head carries its two nodes' flows blended — its speed 4.0757 to 4.0748 m/s, its
  // Froude number 3.1912 to 3.1902 — every station's speed is the solved level's and not the
  // level rounded to the millimetre's, and the block's text carries `derived_from`, empty.)
  // Taken again for W142 (msvc-debug, 2026-11-17), from 10561116533633154432: the drawdown above
  // the crest is stepped with the harmonic mean of the two sections' friction slopes, which moves
  // its stations' levels by millimetres; every check above against the fixture's own numbers
  // holds unchanged.
  CHECK(hash_bytes(text.data(), text.size()) == 8629341850483424636ull);
}

TEST_CASE("profile: a line with no width on a ground it knows nothing of, by Jarrett's law") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  // The fixture's uniform trench, as an author would name it: two points and a discharge.
  FlowLine line = fixture_line("authored", 8, 48, -20, 0.8);
  ProfileOptions o;
  o.resistance = Resistance::Jarrett;  // W137: no longer the default
  o.reach_m = 6.0;
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, fixture_ground(), o, p, error), error);
  REQUIRE(p.stations.size() >= 2);
  for (const ProfileStation& s : p.stations) {
    CHECK(s.d84 == 0.0);  // Jarrett's law reads no grain
    // Uniform flow under Jarrett's n: V = (1 / n) R^(2/3) S^(1/2) with n = 0.32 S^0.38 R^-0.16.
    const f64 y = s.depth;
    const f64 radius = k_t.area(y) / k_t.perimeter(y);
    const f64 nn = 0.32 * std::pow(0.01, 0.38) * std::pow(radius, -0.16);
    CHECK(s.manning_n == doctest::Approx(nn).epsilon(1e-6));
    // (to the millimetre the level is written at).
    CHECK(s.speed == doctest::Approx(std::cbrt(radius * radius) * 0.1 / nn).epsilon(1e-3));
    // The speed is the solved level's and the depth is that level written to the millimetre: the
    // discharge through the written depth is Q to half a millimetre of the surface's width.
    CHECK(s.speed * k_t.area(y) == doctest::Approx(0.8).epsilon(0.0005 * k_t.top(y) / k_t.area(y)));
    // The width it was not given is the section's, crest to crest, to the lattice.
    CHECK(std::fabs(s.width - (8.0 + 0.02 * (s.x - 8.0))) <= 0.5);
    CHECK(s.manning_n > 0.05);  // a steep bed is rougher than 0.04
  }
  CHECK(p.stats.rises_other == 0);
  // Out of range, refused with the line's name.
  FlowLine bad = line;
  bad.discharge[0] = 0.0;
  CHECK_FALSE(solve_profile(bad, fixture_ground(), o, p, error));
  CHECK(error.find("\"authored\": its discharge must be positive") != std::string::npos);
  bad = line;
  bad.points.resize(1);
  CHECK_FALSE(solve_profile(bad, fixture_ground(), o, p, error));
  CHECK_FALSE(solve_profile(line, scene_gen::Ground{}, o, p, error));
}

// W137 (docs/experiments/stream-speed.md): uniform flow under Ferguson's variable-power law, the
// default, on the D84 a line names and on the one its section gives it; Jarrett's clamp gave every
// section under R = 0.15 m the n of 0.15 m, so a thin sheet and a deep one took one roughness.
namespace {
f64 vpe_speed(f64 radius, f64 slope, f64 d84) {
  const f64 x = radius / d84;
  const f64 ratio = 6.5 * 2.5 * x / std::sqrt(6.5 * 6.5 + 2.5 * 2.5 * std::pow(x, 5.0 / 3.0));
  return std::sqrt(9.81 * radius * slope) * ratio;
}
}  // namespace

TEST_CASE("profile: a bed's resistance by Ferguson's law on its own D84") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  ProfileOptions o;
  CHECK(o.resistance == Resistance::Ferguson);
  o.reach_m = 6.0;
  // The fixture's uniform trench at S = 0.01, a cobble bed of D84 = 0.15 m named a point, at
  // two discharges: a deep flow (R / D84 about 2.5) and a sheet a clast deep (about 0.3).
  for (const f64 q : {0.8, 0.05}) {
    CAPTURE(q);
    FlowLine line = fixture_line("cobbles", 8, 48, -20, q);
    line.d84 = {0.15, 0.15};
    Profile p;
    REQUIRE_MESSAGE(solve_profile(line, fixture_ground(), o, p, error), error);
    REQUIRE(p.stations.size() >= 2);
    for (const ProfileStation& s : p.stations) {
      CHECK(s.d84 == 0.15);
      const f64 radius = k_t.area(s.depth) / k_t.perimeter(s.depth);
      // The level is written to the millimetre: the speed is held to the depth's share of that.
      CHECK(s.speed == doctest::Approx(vpe_speed(radius, 0.01, 0.15)).epsilon(0.02));
      // The n reported is the one Manning's form would need for the same friction slope.
      CHECK(s.manning_n ==
            doctest::Approx(std::pow(radius, 1.0 / 6.0) /
                            (std::sqrt(9.81) * s.speed / std::sqrt(9.81 * radius * 0.01)))
                .epsilon(0.03));
    }
    CHECK(p.stats.rises_other == 0);
  }
  // A larger grain is a rougher bed: the same water runs deeper and slower over boulders.
  FlowLine gravel = fixture_line("gravel", 8, 48, -20, 0.3);
  gravel.d84 = {0.04, 0.04};
  FlowLine boulders = gravel;
  boulders.d84 = {0.4, 0.4};
  Profile pg, pb;
  REQUIRE_MESSAGE(solve_profile(gravel, fixture_ground(), o, pg, error), error);
  REQUIRE_MESSAGE(solve_profile(boulders, fixture_ground(), o, pb, error), error);
  CHECK(pb.stations[1].depth > pg.stations[1].depth);
  CHECK(pb.stations[1].speed < 0.8 * pg.stations[1].speed);
  // A line that names no D84 takes the plan's sediment law's at its section's bank height and its
  // bed's slope: a gravel or cobble bed on the fixture's 1 % trench, the same law in uniform flow.
  FlowLine bare = fixture_line("bare", 8, 48, -20, 0.8);
  Profile pn;
  REQUIRE_MESSAGE(solve_profile(bare, fixture_ground(), o, pn, error), error);
  for (const ProfileStation& s : pn.stations) {
    CHECK(s.d84 > 0.004);
    CHECK(s.d84 < 0.5);
    const f64 radius = k_t.area(s.depth) / k_t.perimeter(s.depth);
    CHECK(s.speed == doctest::Approx(vpe_speed(radius, 0.01, s.d84)).epsilon(0.02));
  }
  // A D84 that is not one a point, or not positive, is refused with the line's name.
  FlowLine bad = gravel;
  bad.d84 = {0.04};
  CHECK_FALSE(solve_profile(bad, fixture_ground(), o, pg, error));
  CHECK(error.find("\"gravel\": its bed's D84 is absent or one a point") != std::string::npos);
  bad.d84 = {0.04, 0.0};
  CHECK_FALSE(solve_profile(bad, fixture_ground(), o, pg, error));
}

TEST_CASE("profile: the water above a lip on a steep rough bed is no pool") {
  // W142 (channels.md, "The profile", "The water above a lip"): a bed a metre wide falling one in
  // two, boulders of D84 = 0.3 m, 20 l/s, and a step of 0.8 m at x = 12. Under Ferguson's law the
  // water is tranquil on that slope (a clast deep, its friction slope the bed's), and at the
  // lip's critical section the friction slope is some twenty times the bed's. The standard step
  // charged half of that over the lattice step above the lip, so the water stood there as a pool
  // near two metres deep reaching metres back up the bed, ending in a face of water at the lip.
  Long g;
  g.count = 4;
  g.half = 0.5;
  const f64 xs[4] = {0.0, 12.0, 12.25, 30.0};
  const f64 ys[4] = {30.0, 24.0, 23.2, 14.325};
  for (u32 i = 0; i < 4; ++i) {
    g.x[i] = xs[i];
    g.y[i] = ys[i];
  }
  ProfileOptions o;
  CHECK(o.resistance == Resistance::Ferguson);
  o.reach_m = 3.0;
  FlowLine line = fixture_line("steep", 2, 28, 0, 0.02);
  line.d84 = {0.3, 0.3};
  Profile p;
  std::string error;
  REQUIRE_MESSAGE(solve_profile(line, long_ground(g), o, p, error), error);
  // The reach's own depth, well above the lip and below the line's head.
  f64 reach_depth = 0.0, nearest = 1e30;
  u32 lip = 0;
  bool has_lip = false;
  for (u32 i = 0; i < p.stations.size(); ++i) {
    const ProfileStation& s = p.stations[i];
    if (std::fabs(s.x - 6.0) < nearest) {
      nearest = std::fabs(s.x - 6.0);
      reach_depth = s.depth;
    }
    if (!has_lip && (s.flags & scene_gen::k_channel_fall_lip) != 0) {
      lip = i;
      has_lip = true;
    }
  }
  REQUIRE(has_lip);
  CHECK(p.stations[lip].x == doctest::Approx(12.0).epsilon(0.001));
  CHECK(reach_depth > 0.03);
  CHECK(reach_depth < 0.15);
  // Every station from there down to the lip is the reach's water quickening, never deeper than
  // half as much again, and the last of them stands over the lip by no more than the bed falls
  // between them and that depth: no pool, and no face of water at the lip.
  for (u32 i = 0; i < lip; ++i) {
    const ProfileStation& s = p.stations[i];
    if (s.x < 6.0) continue;
    CAPTURE(s.x);
    CHECK(s.depth <= 1.5 * reach_depth);
  }
  REQUIRE(lip > 0);
  const ProfileStation& above = p.stations[lip - 1];
  CHECK(above.level - p.stations[lip].level <= 0.5 * (p.stations[lip].x - above.x) + reach_depth);
  check_head_falls(p);
}

TEST_CASE("profile: a bed too small for its water is flagged, and no water is written outside it") {
  Trench trench;  // a bed 2 m wide with banks 0.25 m high
  scene_gen::Ground ground;
  ground.surface_fn = &trench_height;
  ground.context = &trench;
  FlowLine line;
  line.name = "small";
  line.points.push_back(WorldPos{0, 0, 0});
  line.points.push_back(WorldPos{60, 0, 0});
  std::string error;
  Profile held, over;
  line.discharge.push_back(0.3);
  REQUIRE_MESSAGE(solve_profile(line, ground, manning(), held, error), error);
  CHECK(held.stats.over_bank == 0);
  line.discharge[0] = 6.0;
  REQUIRE_MESSAGE(solve_profile(line, ground, manning(), over, error), error);
  CHECK(over.stats.over_bank == over.stats.stations);
  CHECK(over.stats.over_bank_worst_m > 0.3);
  for (const ProfileStation& s : over.stations) {
    CHECK((s.flags & scene_gen::k_channel_over_bank) != 0);
    // Written at the bank's top: 0.25 m of water, reaching the crests and no further.
    CHECK(s.depth == doctest::Approx(0.25).epsilon(4e-3));
    CHECK(s.wet_left <= 1.25 + 1e-9);
    CHECK(s.wet_right <= 1.25 + 1e-9);
    CHECK(s.over_m > 0.0);
  }
  const scene::WaterChannel block = describe_profile("small", over, 1);
  REQUIRE_MESSAGE(scene_gen::water_channel_solved(block, &error), error);
}

TEST_CASE("profile: tranquil water never rises downstream where a bed opens into a pool") {
  Trench trench;
  trench.bank = 2.0;
  trench.slope = 0.002;
  trench.bed_half = 0.5;
  trench.wide_from_x = 30.0;
  trench.wide_half = 4.0;
  scene_gen::Ground ground;
  ground.surface_fn = &trench_height;
  ground.context = &trench;
  FlowLine line;
  line.name = "opens";
  line.points.push_back(WorldPos{0, 0, 0});
  line.points.push_back(WorldPos{60, 0, 0});
  line.discharge.push_back(1.2);
  line.tail_level = true;
  line.level = 20.0 - 0.002 * 60.0 + 0.9;
  std::string error;
  Profile p;
  ProfileOptions o = manning();
  o.reach_m = 8.0;
  REQUIRE_MESSAGE(solve_profile(line, ground, o, p, error), error);
  // The narrow reach runs faster than the pool below it: the energy equation alone would have the
  // level climb as the water slows. It does not.
  CHECK(p.stations[0].speed > 2.0 * p.stations[p.stations.size() - 1].speed);
  CHECK(p.stats.rises_other == 0);
  CHECK(p.stats.rises_shooting == 0);
  CHECK(p.stats.head_rises == 0);
  for (u32 i = 0; i + 1 < p.stations.size(); ++i)
    CHECK(p.stations[i + 1].level <= p.stations[i].level);
}

TEST_CASE("profile: a fall whose foot stands in deep water has no jump") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  FlowLine line = fixture_line("drowned", 8, 58, 0, f.numbers.discharge_m3s);
  line.tail_level = true;
  line.level = 11.2;  // 1.5 m over the apron: the pool stands at the fall's foot
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, fixture_ground(), manning(), p, error), error);
  CHECK(p.stats.falls == 1);
  CHECK(p.stats.jumps == 0);
  CHECK(p.stats.drowned_feet == 1);
  for (const ProfileStation& s : p.stations)
    if (s.x >= 40.0) {
      CHECK(s.froude < 1.0);
      CHECK(s.level >= 11.2);
    }
  CHECK(p.stats.rises_other == 0);
}

// ---- The head never rises (W120, the second pass: each kind seen on the forest's lines) --------

TEST_CASE("profile: a jump's head between two nodes of a falling bed is shooting water") {
  // A chute at 0.15 that steepens to 0.8 for a metre (under a face's gradient: no fall) into a
  // level pool: the jump stands on the steep metre, where the bed falls 0.2 m from node to node.
  Long g;
  g.count = 4;
  g.x[0] = 0, g.y[0] = 20.0;
  g.x[1] = 20, g.y[1] = 17.0;
  g.x[2] = 21, g.y[2] = 16.2;
  g.x[3] = 40, g.y[3] = 16.2;
  FlowLine line = fixture_line("falling", 0, 40, 0, 1.0);
  line.tail_level = true;
  line.level = 17.05;
  std::string error;
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, long_ground(g), manning(), p, error), error);
  CHECK(p.stats.jumps == 1);
  CHECK(p.stats.head_rises == 0);
  check_head_falls(p);
  for (u32 i = 0; i + 1 < p.stations.size(); ++i)
    if ((p.stations[i].flags & scene_gen::k_channel_jump_head) != 0) {
      MESSAGE("head at x " << p.stations[i].x << " depth " << p.stations[i].depth << " Froude "
                           << p.stations[i].froude << "; foot depth " << p.stations[i + 1].depth);
      CHECK(p.stations[i].froude > 1.0);
      CHECK(p.stations[i + 1].froude < 1.0);
    }
}

TEST_CASE("profile: shooting water into a pool behind a lip jumps in the pool, not at the lip") {
  // A chute into a pool three nodes long whose floor stands 0.2 m under the lip that holds it,
  // then a 2 m fall. The pool is tranquil and the lip critical: a jump's foot is in the pool.
  Long g;
  g.count = 7;
  g.x[0] = 0, g.y[0] = 20.0;
  g.x[1] = 19.75, g.y[1] = 17.0375;
  g.x[2] = 20, g.y[2] = 16.9;
  g.x[3] = 20.75, g.y[3] = 16.9;
  g.x[4] = 21, g.y[4] = 17.1;
  g.x[5] = 21.25, g.y[5] = 15.1;
  g.x[6] = 35, g.y[6] = 15.1;
  FlowLine line = fixture_line("pool", 0, 35, 0, 1.0);
  line.tail_level = true;
  line.level = 15.5;
  std::string error;
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, long_ground(g), manning(), p, error), error);
  CHECK(p.stats.falls == 1);
  CHECK(p.stats.head_rises == 0);
  check_head_falls(p);
  for (const ProfileStation& s : p.stations) {
    const bool foot = (s.flags & scene_gen::k_channel_jump_foot) != 0;
    // A jump's foot is tranquil water: never the critical lip itself.
    if (foot)
      CHECK((s.flags & (scene_gen::k_channel_control | scene_gen::k_channel_fall_lip)) == 0);
    if (foot) CHECK(s.froude < 1.0);
  }
}

TEST_CASE("profile: a fall whose tailwater stands over its lip is no fall") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  FlowLine line = fixture_line("over the lip", 8, 58, 0, f.numbers.discharge_m3s);
  line.tail_level = true;
  line.level = 12.5;  // the crest stands at 11.7 and passes critical at 12.077
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, fixture_ground(), manning(), p, error), error);
  CHECK(p.stats.falls == 0);
  CHECK(p.stats.jumps == 0);
  CHECK(p.stats.head_rises == 0);
  check_head_falls(p);
  for (u32 i = 0; i < p.stations.size(); ++i) {
    const ProfileStation& s = p.stations[i];
    CHECK((s.flags & (scene_gen::k_channel_fall_lip | scene_gen::k_channel_fall_foot)) == 0);
    CHECK(s.froude < 1.0);
    CHECK(s.level >= 12.5);
    if (i > 0) CHECK(s.level <= p.stations[i - 1].level);
  }
}

TEST_CASE("profile: a last station brought down to the level it meets keeps the arriving flow") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  // The uniform reach, ending in a stem whose level stands 2 cm over the reach's last bed.
  FlowLine line = fixture_line("hangs", 8, 48, -20, f.numbers.discharge_m3s);
  line.tail_level = true;
  line.level = 9.62;
  line.tail = scene_gen::ChannelControl::Confluence;
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, fixture_ground(), manning(), p, error), error);
  const ProfileStation& last = p.stations[p.stations.size() - 1];
  CHECK(last.level == 9.62);
  CHECK(last.flags == scene_gen::channel_control_flags(scene_gen::ChannelControl::Confluence));
  // The water arrives at critical depth and drops to the stem: its speed is the arriving flow's,
  // not the discharge through 2 cm of the reach's own section (40 m/s, a Froude number of 90).
  MESSAGE("last station: speed " << last.speed << " Froude " << last.froude << " slope "
                                 << last.friction);
  CHECK(last.froude == doctest::Approx(1.0).epsilon(0.01));
  CHECK(p.stats.froude_max < 1.02);
  CHECK(p.stats.head_rises == 0);
  check_head_falls(p);
}

TEST_CASE("profile: a fall's foot keeps Rand's speed where the discharge grows across the fall") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  // The step, with a tributary's worth of water joining across the crest: 0.7 m3/s at the lip
  // (x = 38), 1.6 at the foot.
  FlowLine line;
  line.name = "grows";
  for (const f64 x : {8.0, 37.9, 38.3, 58.0})
    line.points.push_back(WorldPos{x, 0.0, 0.0});
  for (const f64 q : {0.4, 0.4, 1.6, 1.6})
    line.discharge.push_back(q);
  line.tail_level = true;
  line.level = f.numbers.pool_level_m;
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, fixture_ground(), manning(), p, error), error);
  REQUIRE(p.stats.falls == 1);
  for (u32 i = 0; i + 1 < p.stations.size(); ++i) {
    const ProfileStation& lip = p.stations[i];
    if ((lip.flags & scene_gen::k_channel_fall_lip) == 0) continue;
    const ProfileStation& foot = p.stations[i + 1];
    // Rand's ratio of speeds, from the lip's own critical flow: D_c = V_c^2 / g, h = 2 m.
    const f64 dc = lip.speed * lip.speed / k_gravity;
    const f64 expected =
        lip.speed / (k_drop_foot_share * std::exp(k_drop_foot_exponent * std::log(dc / 2.0)));
    MESSAGE("lip " << lip.discharge << " m3/s at " << lip.speed << " m/s; foot " << foot.discharge
                   << " m3/s at " << foot.speed << " m/s against Rand's " << expected);
    CHECK(foot.speed == doctest::Approx(expected).epsilon(0.03));
  }
  check_head_falls(p);
}

TEST_CASE("profile: a fall's foot beside a shelf has no more head than its lip") {
  // A 2 m fall in a trench half a metre wide onto a bed steep enough to shoot down; at the foot
  // the left bank is a hump with a shelf behind it, so the area under a level steps up by
  // 0.16 m2 as the level passes the hump, and Rand's area for the foot (0.15 m2) is one no level
  // has.
  Long g;
  g.count = 4;
  g.half = 0.25;
  g.x[0] = 0, g.y[0] = 12.3;
  g.x[1] = 30, g.y[1] = 12.0;
  g.x[2] = 30.25, g.y[2] = 10.0;
  g.x[3] = 50, g.y[3] = 9.0125;
  g.shelf_x0 = 31.0;
  g.shelf_x1 = 34.0;
  FlowLine line = fixture_line("shelf", 0, 50, 0, 0.835);
  std::string error;
  Profile p;
  ProfileOptions o = manning();
  o.reach_m = 9.0;
  REQUIRE_MESSAGE(solve_profile(line, long_ground(g), o, p, error), error);
  REQUIRE(p.stats.falls == 1);
  for (u32 i = 0; i + 1 < p.stations.size(); ++i)
    if ((p.stations[i].flags & scene_gen::k_channel_fall_lip) != 0)
      MESSAGE("lip head " << head_of(p.stations[i]) << "; foot at x " << p.stations[i + 1].x
                          << " level " << p.stations[i + 1].level << " speed "
                          << p.stations[i + 1].speed << " head " << head_of(p.stations[i + 1]));
  CHECK(p.stats.head_rises == 0);
  check_head_falls(p);
}

namespace {
// A ground with no bed across a line running +x (W129.1): a floor falling 1 % along x and level
// across, to `flat_half` either side of z = 0; past it the ground falls away at one in ten (a level
// spur), or, with a flat half past the section's reach, it is level from edge to edge (a lake's
// floor filled flat, which no cut crosses).
struct Level {
  f64 flat_half = 1.0;
};
f64 level_height(const void* context, i64 x_mm, i64 z_mm) noexcept {
  const Level& l = *static_cast<const Level*>(context);
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = std::fabs(static_cast<f64>(z_mm) / 1000.0);
  const f64 floor = 30.0 - 0.01 * x;
  return z <= l.flat_half ? floor : floor - 0.1 * (z - l.flat_half);
}
}  // namespace

TEST_CASE("profile: water on ground with no bank across it is a sheet, never refused") {
  // The forest's `region link 294` at a stage over 1 (W129.1): a line the plan left dry, so no
  // cut crossed the level floor it runs over, and every section was level at its thalweg. The
  // crest walk stopped at the thalweg, the section wet nothing at any level, and the line was
  // refused with "a station has no water in its section".
  for (const f64 flat_half : {1.0, 100.0}) {
    CAPTURE(flat_half);
    Level l;
    l.flat_half = flat_half;
    scene_gen::Ground ground;
    ground.surface_fn = &level_height;
    ground.context = &l;
    FlowLine line = fixture_line("level floor", 0, 40, 0, 0.0064);
    line.width.assign(2, 1.0);
    std::string error;
    Profile p;
    REQUIRE_MESSAGE(solve_profile(line, ground, ProfileOptions{}, p, error), error);
    REQUIRE(p.stations.size() >= 2);
    for (const ProfileStation& s : p.stations) {
      CHECK(s.depth >= 0.005 - 1e-9);
      // The water stands over the level ground to where it falls away (or the section's end).
      CHECK(s.wet_left + s.wet_right >= 2.0 * std::min(flat_half, 1.0) - 1e-6);
      CHECK(s.speed > 0.0);
    }
  }
}

namespace {
// A bed falling 2 % along +x in a shallow vee (its sides rising 5 % across), and across it at
// x = 25 a bowl 8 m in radius and 0.6 m deep (W130.2): a closed hollow of the ground on the line,
// wider than the section's read, whose sill is its downstream rim near x = 33.
f64 bowl_height(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  const f64 r2 = (x - 25.0) * (x - 25.0) + z * z;
  return 20.0 - 0.02 * x + 0.05 * std::fabs(z) - 0.6 * std::max(0.0, 1.0 - r2 / 64.0);
}
}  // namespace

TEST_CASE("profile: a pit on a line stands as a pool at its sill and spills over it") {
  // The forest's lines down the planner's breached pits and the hollows finer than its lattice
  // (W130.2): the solved level stood over the section's crests, and the stations were written at
  // the crest and then as a 5 mm film climbing to the sill.
  scene_gen::Ground ground;
  ground.surface_fn = &bowl_height;
  FlowLine line = fixture_line("bowl", 0, 50, 0, 0.021);
  line.width.assign(2, 1.0);
  std::string error;
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, ground, ProfileOptions{}, p, error), error);
  CHECK(p.stats.pools > 0);
  CHECK(p.stats.pools_open == 0);
  f64 sill = 1e9, low = 1e9, high = -1e9, widest = 0.0;
  for (const ProfileStation& s : p.stations)
    if (s.x > 30.0 && s.x < 36.0) sill = std::min(sill, s.level);
  for (const ProfileStation& s : p.stations) {
    const f64 r2 = (s.x - 25.0) * (s.x - 25.0);
    if (r2 > 20.0) continue;  // the bowl's floor, 4.5 m either way of its centre
    CAPTURE(s.x);
    CHECK((s.flags & scene_gen::k_channel_over_bank) == 0);
    CHECK(s.depth > 0.1);
    low = std::min(low, s.level);
    high = std::max(high, s.level);
    widest = std::max(widest, s.wet_left + s.wet_right);
  }
  MESSAGE("the pool stands " << low << " to " << high << " m, " << widest << " m wide; the sill's "
                             << "water at " << sill << " m");
  CHECK(high - low < 0.01);  // level: a pool, not a film climbing
  CHECK(low >= 19.34);       // at its sill (the rim's ground at x = 33), which it spills over
  CHECK(sill < low);         // and spills: below the sill the water falls away
  CHECK(widest > 6.0);       // as wide as the level meets the ground, past the section's read
  check_head_falls(p);
}

namespace {
// The bowl, with its upstream half open on its -z side (W138): past z = -3 and short of x = 25
// the ground stands no higher than 19.0 m, under the level the bowl would hold, so water at that
// level leaves the bowl there.
f64 notched_bowl_height(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 h = bowl_height(nullptr, x_mm, z_mm);
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  return x < 25.0 && z < -3.0 ? std::min(h, 19.0) : h;
}
}  // namespace

TEST_CASE("profile: a pool stands no higher than the water above it spilled") {
  // The forest's walls (W138; 42 on v47 at stage 1): a pit's upstream stations open past the pool's
  // reach were written at their bank's top, its downstream ones as a pool at the sill's level, and
  // the sheet climbed from the one to the other. Water that leaves the bed at the lower level does
  // not stand higher below it: no written level climbs, and the pit's closed half is not written as
  // a pool over the spilled water.
  scene_gen::Ground ground;
  ground.surface_fn = &notched_bowl_height;
  FlowLine line = fixture_line("notched bowl", 0, 50, 0, 0.021);
  line.width.assign(2, 1.0);
  std::string error;
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, ground, ProfileOptions{}, p, error), error);
  MESSAGE("pools " << p.stats.pools << ", open " << p.stats.pools_open << ", spilled "
                   << p.stats.pools_spilled << ", reached back " << p.stats.pools_reached);
  CHECK(p.stats.pools_open > 0);
  CHECK(p.stats.pools_spilled > 0);
  f64 worst = 0.0;
  for (u32 k = 0; k + 1 < p.stations.size(); ++k) {
    const ProfileStation& a = p.stations[k];
    const ProfileStation& b = p.stations[k + 1];
    if ((a.flags & scene_gen::k_channel_jump_head) != 0) continue;
    // What the solve writes and the draw lays a sheet on: the 5 mm film over its bank climbing
    // the bowl's rim (W120's, left out by the draw) is not a face of water.
    if (!scene_gen::channel_stretch_drawn(a.flags, b.flags, static_cast<f32>(a.depth),
                                          static_cast<f32>(b.depth), a.level, b.level))
      continue;
    worst = std::max(worst, b.level - a.level);
  }
  CAPTURE(worst);
  CHECK(worst < 0.05);  // the forest's least climb
  // Before W138 the pit's closed half was a pool over the spilled water, and the climb to it drawn
  // (the draw's old rule left a stretch out only when both ends were over a bank). The draw's own
  // rule now refuses such a stretch whatever the solve writes, so the solve is held here by what it
  // writes: no pool stands over the spilled level.
  for (const ProfileStation& s : p.stations)
    if (s.x > 25.0 && s.x < 31.0) {
      CAPTURE(s.x);
      CHECK(s.depth < 0.2);
    }
  // The closed bowl (stand-alone, the test above) is still a pool.
  ground.surface_fn = &bowl_height;
  REQUIRE_MESSAGE(solve_profile(line, ground, ProfileOptions{}, p, error), error);
  CHECK(p.stats.pools > 0);
  CHECK(p.stats.pools_spilled == 0);
}

namespace {
// The bowl, with a scarp beside the line above it (W138 with W131; the forest's 24 gaps): from
// x = 13 to 18.5, where the line's own ground still stands over the level the bowl holds, the
// ground on its -z side past 0.2 m of the line falls to 19.1 m and stays there 30 m out, so the
// section there is read on a bed under the line's ground, open on that side.
f64 scarped_bowl_height(const void*, i64 x_mm, i64 z_mm) noexcept {
  const f64 h = bowl_height(nullptr, x_mm, z_mm);
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  return x >= 13.0 && x <= 18.5 && z < -0.2 ? std::min(h, 19.1) : h;
}
}  // namespace

TEST_CASE("profile: the stream that runs into a pool is written at the pool's level") {
  // The stations above the bowl were written at their open edge, 0.2 m under the pool's level and
  // under the line's own ground, and the pool's surface stood over them: the sheet climbed from the
  // one to the other, which the draw lays nothing on (a gap). The water that meets the pool
  // stands at its level; written there it is still under the line's ground, and the sheet is laid.
  scene_gen::Ground ground;
  ground.surface_fn = &scarped_bowl_height;
  FlowLine line = fixture_line("scarped bowl", 0, 50, 0, 0.021);
  line.width.assign(2, 1.0);
  std::string error;
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, ground, ProfileOptions{}, p, error), error);
  REQUIRE(p.stats.pools > 0);
  f64 pool = 1e9;
  for (const ProfileStation& s : p.stations)
    if (std::fabs(s.x - 25.0) < 3.0) pool = std::min(pool, s.level);
  u32 undrawn = 0, raised = 0;
  for (u32 k = 0; k + 1 < p.stations.size(); ++k) {
    const ProfileStation& a = p.stations[k];
    const ProfileStation& b = p.stations[k + 1];
    if (a.x < 12.0 || b.x > 25.0) continue;
    CAPTURE(a.x);
    if ((a.flags & scene_gen::k_channel_fall_lip) != 0 &&
        (b.flags & scene_gen::k_channel_fall_foot) != 0)
      continue;
    if (!scene_gen::channel_stretch_drawn(a.flags, b.flags, static_cast<f32>(a.depth),
                                          static_cast<f32>(b.depth), a.level, b.level))
      ++undrawn;
    if (a.x >= 13.0 && a.x <= 18.5 && a.level >= pool - 1e-3) {
      ++raised;
      // Written no higher than the line's own ground there: hidden, never a sheet in the air.
      CHECK(a.level <= scarped_bowl_height(nullptr, scene_gen::nearest_mm(a.x), 0) + 1e-3);
    }
  }
  MESSAGE("the pool at " << pool << " m; " << raised << " stations above it at its level, "
                         << undrawn << " stretches from x = 12 to the pool not drawn");
  CHECK(raised > 0);
  CHECK(undrawn == 0);
  // Not check_head_falls: the station on the bowl's rim at x = 18.75, written at the pool's level
  // 1.5 cm over its bed, takes the line's discharge through that sliver at Froude 20 with or
  // without this rule (a pool's rim station's flow; HANDOFF, W138).
}

namespace {
// **A bench cut across a hillside** (W131; the forest's `region link 515`, stations 374 to 407 and
// 712 to 724): the hillside rises 0.65 across a line running +x (to its left, +z) and falls 0.1
// along it. The cut is a floor a metre wide at the line, 0.4 m under the hillside, falling 4 %
// toward the downhill side, with an uphill face at 1:1 out to the hillside. On the downhill side
// either the ground falls away from the floor's edge at the hillside's own slope (no bank: where
// the seat was not read, at a knickpoint or beside another cut's face), or a lip 0.2 m high holds
// it first. Two metres downhill lies another trench a metre deep (the line's own next leg below a
// switchback).
struct Bench {
  bool lip = false;
};
f64 bench_height(const void* context, i64 x_mm, i64 z_mm) noexcept {
  const Bench& b = *static_cast<const Bench*>(context);
  const f64 x = static_cast<f64>(x_mm) / 1000.0, z = static_cast<f64>(z_mm) / 1000.0;
  const f64 hill0 = 100.0 - 0.1 * x;  // the hillside at the line
  const auto floor_at = [&](f64 zz) { return hill0 - 0.4 + 0.04 * zz; };
  if (z >= -0.5 && z <= 0.5) return floor_at(z);
  if (z > 0.5) return std::min(floor_at(0.5) + (z - 0.5), hill0 + 0.65 * z);
  f64 g = 0.0;
  if (b.lip) {
    g = z >= -0.75 ? floor_at(-0.5) + 0.8 * (-0.5 - z) : floor_at(-0.5) + 0.2 + 0.65 * (z + 0.75);
  } else {
    g = floor_at(-0.5) + 0.65 * (z + 0.5);
  }
  return g - std::max(0.0, 1.0 - 2.0 * std::fabs(z + 2.0));
}
}  // namespace

TEST_CASE(
    "profile: the section is read in the bed under the line, never on the hillside beside it") {
  // Before W131 the thalweg, found within the bed's half width, was walked downhill with no bound:
  // across a bench with no downhill bank it ran down the hillside to the section's end or into the
  // trench below, and the station's level stood there, a metre or more under the ground at the
  // line, where nothing of it is seen.
  for (const bool lip : {false, true}) {
    CAPTURE(lip);
    Bench b;
    b.lip = lip;
    scene_gen::Ground ground;
    ground.surface_fn = &bench_height;
    ground.context = &b;
    FlowLine line = fixture_line("bench", 0, 40, 0, 0.021);
    line.width.assign(2, 1.0);
    std::string error;
    Profile p;
    REQUIRE_MESSAGE(solve_profile(line, ground, ProfileOptions{}, p, error), error);
    REQUIRE(p.stations.size() >= 2);
    CHECK(p.stats.under_ground == 0);
    CHECK(p.stats.falls == 0);
    for (u32 i = 0; i < p.stations.size(); ++i) {
      const ProfileStation& s = p.stations[i];
      CAPTURE(s.x);
      const f64 at_line = 100.0 - 0.1 * s.x - 0.4;
      // The bed is the floor's, within the bed's half width of the line, and the water on it.
      CHECK(s.bed >= at_line - 0.03);
      CHECK(s.level >= at_line - k_under_ground_m);
      CHECK(s.depth >= 0.02);                           // a stream the draw lays, not a film
      CHECK(s.wet_right <= (lip ? 0.75 : 0.5) + 1e-6);  // to the floor's edge, or up the lip
      if (lip) CHECK((s.flags & scene_gen::k_channel_over_bank) == 0);
      if (i + 1 < p.stations.size()) {
        const ProfileStation& t = p.stations[i + 1];
        CHECK(scene_gen::channel_stretch_drawn(s.flags, t.flags, static_cast<f32>(s.depth),
                                               static_cast<f32>(t.depth), s.level, t.level));
      }
    }
    check_head_falls(p);
  }
}
