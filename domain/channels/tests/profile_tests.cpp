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
  // linux-gcc-release-v2 reproduce it.
  CHECK(hash_bytes(text.data(), text.size()) == 8605109305383583938ull);
}

TEST_CASE("profile: a line with no width on a ground it knows nothing of, by Jarrett's law") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  // The fixture's uniform trench, as an author would name it: two points and a discharge.
  FlowLine line = fixture_line("authored", 8, 48, -20, 0.8);
  ProfileOptions o;
  o.reach_m = 6.0;
  Profile p;
  REQUIRE_MESSAGE(solve_profile(line, fixture_ground(), o, p, error), error);
  REQUIRE(p.stations.size() >= 2);
  for (const ProfileStation& s : p.stations) {
    // Uniform flow under Jarrett's n: V = (1 / n) R^(2/3) S^(1/2) with n = 0.32 S^0.38 R^-0.16.
    const f64 y = s.depth;
    const f64 radius = k_t.area(y) / k_t.perimeter(y);
    const f64 nn = 0.32 * std::pow(0.01, 0.38) * std::pow(radius, -0.16);
    CHECK(s.manning_n == doctest::Approx(nn).epsilon(1e-6));
    // (to the millimetre the level is written at).
    CHECK(s.speed == doctest::Approx(std::cbrt(radius * radius) * 0.1 / nn).epsilon(1e-3));
    CHECK(s.speed * k_t.area(y) == doctest::Approx(0.8).epsilon(1e-6));
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
