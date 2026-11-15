// The flow fixture and the description's check (channels.md, "The water's description", "The
// fixture"; roadmap W120 stage 0): every closed form the fixture is derived from, evaluated a
// second way; the ground's lattice against the function it samples; the stations against the
// contract's pins; and what the check refuses.
#include <core/json/json.h>
#include <core/schema/json_reflect.h>
#include <domain/channels/flow_fixture.h>
#include <domain/channels/hydraulics.h>
#include <domain/channels/stations.h>
#include <domain/scene_gen/water_channel.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>

using namespace engine;
using namespace engine::channels;

namespace {

const Trapezoid k_t{2.0, 1.0};

f64 run_of(const scene::WaterChannel& c, u32 i) {
  const f64 dx = c.points[i + 1].x - c.points[i].x, dz = c.points[i + 1].z - c.points[i].z;
  return std::sqrt(dx * dx + dz * dz);
}

}  // namespace

TEST_CASE("flow fixture: the closed forms hold, each evaluated a second way") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  const FlowFixtureNumbers& n = f.numbers;
  const f64 q = n.discharge_m3s;
  // Manning's equation at the chosen normal depth, by hand: A = 1.25, P = 2 + sqrt(2).
  {
    const f64 area = 1.25, radius = area / (2.0 + std::sqrt(2.0));
    CHECK(q == doctest::Approx(area * std::cbrt(radius * radius) * 0.1 / 0.04).epsilon(1e-12));
    CHECK(trapezoid_normal(k_t, q, n.manning_n, n.slope) == doctest::Approx(0.5).epsilon(1e-12));
    CHECK(trapezoid_friction(k_t, 0.5, q, n.manning_n) == doctest::Approx(0.01).epsilon(1e-12));
  }
  // Critical depth: Q^2 T = g A^3, and the Froude number there is 1.
  {
    const f64 y = n.critical_m;
    const f64 area = k_t.area(y);
    CHECK(q * q * k_t.top(y) == doctest::Approx(k_gravity * area * area * area).epsilon(1e-12));
    CHECK(trapezoid_froude(k_t, y, q) == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(n.normal_m > y);  // mild
    // The specific energy and the specific force are both least there.
    for (const f64 d : {-1e-3, 1e-3}) {
      CHECK(trapezoid_energy(k_t, y + d, q) > trapezoid_energy(k_t, y, q));
      CHECK(trapezoid_force(k_t, y + d, q) > trapezoid_force(k_t, y, q));
    }
  }
  // Rand's foot: the area's ratio to the critical area, and the depth that has that area.
  {
    const f64 critical_area = k_t.area(n.critical_m);
    const f64 hydraulic = critical_area / k_t.top(n.critical_m);
    const f64 area = 0.54 * std::pow(hydraulic / n.drop_m, 0.275) * critical_area;
    CHECK(k_t.area(n.foot_m) == doctest::Approx(area).epsilon(1e-9));
    CHECK(n.foot_m < n.critical_m);
    CHECK(n.foot_froude > 1.0);
    const f64 reach = 4.30 * n.drop_m * std::pow(hydraulic / n.drop_m, 0.81);
    CHECK(n.foot_x_m - n.crest_x_m >= reach);
    CHECK(n.foot_x_m - n.crest_x_m < reach + 0.25);
  }
  // The jump: the same specific force either side, its length six rises, the head shooting and
  // the foot tranquil.
  {
    CHECK(trapezoid_force(k_t, n.jump_head_m, q) ==
          doctest::Approx(trapezoid_force(k_t, n.jump_foot_m, q)).epsilon(1e-10));
    CHECK(n.jump_head_m < n.critical_m);
    CHECK(n.jump_foot_m > n.critical_m);
    CHECK(n.jump_length_m == doctest::Approx(6.0 * (n.jump_foot_m - n.jump_head_m)).epsilon(1e-12));
    CHECK(n.jump_foot_x_m == doctest::Approx(n.jump_head_x_m + n.jump_length_m).epsilon(1e-4));
    CHECK(n.jump_head_x_m > n.foot_x_m);
    CHECK(n.jump_foot_x_m < 58.0);
  }
  // The direct step against a finer one: the M1 curve's depth 40 m above the mouth.
  {
    DepthTable coarse, fine;
    direct_step(k_t, q, n.manning_n, n.slope, 1.2, 0.51, 4000, coarse);
    direct_step(k_t, q, n.manning_n, n.slope, 1.2, 0.51, 64000, fine);
    CHECK(depth_at(coarse, -40.0) == doctest::Approx(depth_at(fine, -40.0)).epsilon(1e-6));
    // The profile obeys dy/dx = (S_0 - S_f) / (1 - F^2) there.
    const f64 y = depth_at(fine, -40.0);
    const f64 slope = (depth_at(fine, -39.99) - depth_at(fine, -40.01)) / 0.02;
    const f64 froude = trapezoid_froude(k_t, y, q);
    CHECK(slope == doctest::Approx((n.slope - trapezoid_friction(k_t, y, q, n.manning_n)) /
                                   (1.0 - froude * froude))
                       .epsilon(1e-4));
  }
}

TEST_CASE("flow fixture: the lattice is the ground's function, and the function is the trapezoid") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  REQUIRE(f.width == 385);
  REQUIRE(f.height == 241);
  REQUIRE(f.heights.size() == f.width * f.height);
  u32 off = 0;
  for (u32 j = 0; j < f.height; ++j)
    for (u32 i = 0; i < f.width; ++i) {
      const f64 x = 0.25 * i, z = 0.25 * j - 30.0;
      if (f.heights[j * f.width + i] != static_cast<f32>(flow_fixture_ground(x, z))) ++off;
    }
  CHECK(off == 0);
  // Between lattice points, on the uniform reach and on the step's approach, the bilinear surface
  // of the four corners is the function: the bed plus what the bank has risen.
  const auto bilinear = [&](f64 x, f64 z) {
    const f64 fx = x / 0.25, fz = (z + 30.0) / 0.25;
    const u32 i = static_cast<u32>(fx), j = static_cast<u32>(fz);
    const f64 u = fx - i, v = fz - j;
    const auto h = [&](u32 a, u32 b) { return static_cast<f64>(f.heights[b * f.width + a]); };
    return (1 - v) * ((1 - u) * h(i, j) + u * h(i + 1, j)) +
           v * ((1 - u) * h(i, j + 1) + u * h(i + 1, j + 1));
  };
  for (const f64 x : {9.1, 20.37, 33.9})
    for (const f64 dz : {-1.83, -1.1, -0.4, 0.0, 0.62, 1.47}) {
      const f64 rise = std::max(0.0, std::fabs(dz) - 1.0);
      CHECK(bilinear(x, -20.0 + dz) ==
            doctest::Approx(10.0 - 0.01 * (x - 8.0) + rise).epsilon(1e-6));
      CHECK(bilinear(x, dz) == doctest::Approx(12.0 - 0.01 * (x - 8.0) + rise).epsilon(1e-6));
    }
}

TEST_CASE("flow fixture: its stations are the contract's, and its channels pass the check") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  REQUIRE(f.channels.size() == 3);
  REQUIRE(f.bodies.size() == 2);
  for (const scene::WaterChannel& c : f.channels) {
    CAPTURE(c.name);
    REQUIRE_MESSAGE(scene_gen::water_channel_solved(c, &error), error);
    const u32 n = c.points.size();
    for (u32 i = 0; i + 1 < n; ++i) {
      const f64 run = run_of(c, i);
      CHECK(run >= 0.25);
      const bool pair =
          (c.flags[i] & (scene_gen::k_channel_jump_head | scene_gen::k_channel_fall_lip)) != 0;
      if (!pair) CHECK(run <= k_station_max_m + 1e-9);
    }
    // Level less depth is the trench's bed at every station, and the wet reach is the trapezoid's
    // half top width at that depth.
    const f64 bed0 = c.name == "step" ? 12.0 : 10.0;
    for (u32 i = 0; i < n; ++i) {
      const f64 x = c.points[i].x;
      const f64 bed = c.name == "step" && x > 38.0 ? 9.7 : bed0 - 0.01 * (x - 8.0);
      CHECK(c.points[i].y - static_cast<f64>(c.depth_m[i]) == doctest::Approx(bed).epsilon(1e-4));
      CHECK(static_cast<f64>(c.wet_left_m[i]) ==
            doctest::Approx(1.0 + static_cast<f64>(c.depth_m[i])).epsilon(1e-3));
      CHECK(c.wet_left_m[i] == c.wet_right_m[i]);
      // The speed is the discharge over the area, and the Froude number is that speed's.
      const f64 y = static_cast<f64>(c.depth_m[i]);
      CHECK(static_cast<f64>(c.speed_mps[i]) * k_t.area(y) ==
            doctest::Approx(static_cast<f64>(c.discharge_m3s[i])).epsilon(2e-3));
    }
  }
  // "uniform": six stations at normal depth, no flag.
  const scene::WaterChannel& uniform = f.channels[0];
  REQUIRE(uniform.points.size() == 6);
  for (u32 i = 0; i < 6; ++i) {
    CHECK(uniform.depth_m[i] == 0.5f);
    CHECK(uniform.flags[i] == 0);
    CHECK(uniform.slope[i] == doctest::Approx(0.01f));
  }
  // "step": one lip with its foot, one jump with its foot, in that order; the lip critical and a
  // crest's control; the surface never rising in tranquil flow; the mouth at the pool's level to
  // the millimetre.
  const scene::WaterChannel& step = f.channels[1];
  u32 lips = 0, heads = 0;
  for (u32 i = 0; i < step.points.size(); ++i) {
    const u16 flags = step.flags[i];
    if ((flags & scene_gen::k_channel_fall_lip) != 0) {
      ++lips;
      CHECK(scene_gen::channel_is_control(flags));
      CHECK(scene_gen::channel_control_kind(flags) == scene_gen::ChannelControl::Crest);
      CHECK(step.froude[i] == doctest::Approx(1.0f).epsilon(1e-3));
      CHECK(step.froude[i + 1] > 1.0f);
      CHECK(step.points[i].y - step.points[i + 1].y > 2.0);
    }
    if ((flags & scene_gen::k_channel_jump_head) != 0) {
      ++heads;
      CHECK(step.froude[i] > 1.0f);
      CHECK(step.froude[i + 1] < 1.0f);
      CHECK(step.points[i + 1].y > step.points[i].y);
    }
    if (i + 1 < step.points.size() && step.froude[i] < 1.0f && step.froude[i + 1] < 1.0f)
      CHECK(step.points[i + 1].y <= step.points[i].y);
  }
  CHECK(lips == 1);
  CHECK(heads == 1);
  const u32 last = step.points.size() - 1;
  CHECK(step.flags[last] == scene_gen::channel_control_flags(scene_gen::ChannelControl::Mouth));
  CHECK(step.points[last].y == f.bodies[0].level_m);
  const scene::WaterChannel& mouth = f.channels[2];
  CHECK(mouth.points[mouth.points.size() - 1].y == f.bodies[1].level_m);
  CHECK(mouth.flags[mouth.points.size() - 1] ==
        scene_gen::channel_control_flags(scene_gen::ChannelControl::Mouth));
}

TEST_CASE("water channel: a description that is not solved is refused by name") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  const scene::WaterChannel solved = f.channels[1];
  const auto refused = [&](const scene::WaterChannel& c, const char* says) {
    std::string why;
    CHECK_FALSE(scene_gen::water_channel_solved(c, &why));
    CHECK_MESSAGE(why.find(says) != std::string::npos, why);
  };
  // What every writer before W120 wrote: a level, a width, a depth and a slope, and nothing solved.
  scene::WaterChannel old = solved;
  old.discharge_m3s.clear();
  old.speed_mps.clear();
  old.froude.clear();
  old.wet_left_m.clear();
  old.wet_right_m.clear();
  old.flags.clear();
  refused(old, "cooked before W120: cook again");
  // An author's line: points and a discharge.
  scene::WaterChannel authored = old;
  authored.discharge_m3s.push_back(0.4f);
  refused(authored, "names a discharge and is not solved");
  scene::WaterChannel shorter = solved;
  shorter.froude.resize(shorter.froude.size() - 1);
  refused(shorter, "its froude has");
  scene::WaterChannel dry = solved;
  dry.discharge_m3s[2] = 0.0f;
  refused(dry, "discharge_m3s at point 2 must be positive");
  scene::WaterChannel unknown = solved;
  unknown.flags[0] = 0x100;
  refused(unknown, "a bit this build does not know");
  // A discontinuity is a pair: a lip without its foot, a foot without its head.
  scene::WaterChannel lip = solved;
  for (u32 i = 0; i < lip.flags.size(); ++i)
    lip.flags[i] = static_cast<u16>(lip.flags[i] & ~scene_gen::k_channel_fall_foot);
  refused(lip, "is not followed by the fall's foot");
  scene::WaterChannel foot = solved;
  for (u32 i = 0; i < foot.flags.size(); ++i)
    foot.flags[i] = static_cast<u16>(foot.flags[i] & ~scene_gen::k_channel_jump_head);
  refused(foot, "does not follow a jump's head");
}

TEST_CASE(
    "water channel: a block of version 5 reads through its migration, and is refused by name") {
  FlowFixture f;
  std::string error;
  REQUIRE_MESSAGE(make_flow_fixture(f, error), error);
  // A block as versions 3 to 5 wrote it: laid, with `fit` a point.
  scene::WaterChannel old = f.channels[0];
  old.discharge_m3s.clear();
  old.speed_mps.clear();
  old.froude.clear();
  old.wet_left_m.clear();
  old.wet_right_m.clear();
  old.flags.clear();
  JsonValue block = schema::to_json(old);
  JsonValue fit = JsonValue::array();
  for (u32 i = 0; i < old.points.size(); ++i)
    fit.push_back(JsonValue(u64{0}));
  block.set("fit", std::move(fit));
  JsonValue root = JsonValue::object();
  JsonValue list = JsonValue::array();
  list.push_back(block);
  root.set("water_channels", std::move(list));
  // Unmigrated, the schema refuses the field by its name.
  {
    scene::WaterChannel read;
    schema::ReadContext ctx;
    CHECK_FALSE((schema::from_json(read, block, ctx) && ctx.ok()));
  }
  // Through the migration the scene's reader applies, it reads, and the check says what it is.
  schema::ReadContext ctx;
  REQUIRE(scene_gen::migrate_scene_water_channels(root, ctx));
  const JsonValue& migrated = (*root.find("water_channels"))[0];
  CHECK_FALSE(migrated.contains("fit"));
  scene::WaterChannel read;
  REQUIRE((schema::from_json(read, migrated, ctx) && ctx.ok()));
  CHECK(read.points.size() == old.points.size());
  std::string why;
  CHECK_FALSE(scene_gen::water_channel_solved(read, &why));
  CHECK_MESSAGE(why.find("cooked before W120: cook again") != std::string::npos, why);
  // A solved block names no `fit` and is not touched.
  JsonValue solved_root = JsonValue::object();
  JsonValue solved_list = JsonValue::array();
  solved_list.push_back(schema::to_json(f.channels[1]));
  solved_root.set("water_channels", std::move(solved_list));
  const std::string before = write_json(solved_root, JsonWriteOptions{});
  REQUIRE(scene_gen::migrate_scene_water_channels(solved_root, ctx));
  CHECK(write_json(solved_root, JsonWriteOptions{}) == before);
}

TEST_CASE("stations: the fewest nodes that hold the pins") {
  // A level falling as a parabola, 1 cm of sag over 4 m: the chord from the first node reaches as
  // far as its sag stays within the pin less the rounding, and no farther.
  Vector<StationNode> nodes;
  for (u32 k = 0; k <= 160; ++k) {
    StationNode node;
    node.s = 0.25 * k;
    node.level = 10.0 - 0.0025 * node.s * node.s;  // sag over a chord of length L: 0.0025 L^2 / 4
    node.wet_left = node.wet_right = 1.0;
    nodes.push_back(node);
  }
  nodes[100].keep = true;
  const Vector<u32> kept = select_stations(nodes);
  REQUIRE(kept.size() > 2);
  CHECK(kept[0] == 0);
  CHECK(kept[kept.size() - 1] == 160);
  bool has_kept = false;
  for (u32 a = 0; a + 1 < kept.size(); ++a) {
    const StationNode& p = nodes[kept[a]];
    const StationNode& r = nodes[kept[a + 1]];
    has_kept = has_kept || kept[a] == 100;
    CHECK(r.s - p.s <= k_station_max_m);
    for (u32 k = kept[a] + 1; k < kept[a + 1]; ++k) {
      const f64 u = (nodes[k].s - p.s) / (r.s - p.s);
      CHECK(std::fabs(p.level + u * (r.level - p.level) - nodes[k].level) <= k_station_level_m);
    }
    // One node farther would have broken a pin (or passed a kept node, or the line's end).
    if (kept[a + 1] < 160 && kept[a + 1] != 100) {
      const f64 length = nodes[kept[a + 1] + 1].s - p.s;
      CHECK((0.0025 * length * length / 4.0 > k_station_level_m - k_station_round_m - 1e-9 ||
             length > k_station_max_m));
    }
  }
  CHECK(has_kept);
  // A pair is kept whole and nothing is tested across it.
  Vector<StationNode> step;
  for (u32 k = 0; k < 8; ++k) {
    StationNode node;
    node.s = 0.25 * k;
    node.level = k < 4 ? 5.0 : 2.0;
    step.push_back(node);
  }
  step[3].keep = step[3].pair = true;
  step[4].keep = true;
  const Vector<u32> pair = select_stations(step);
  REQUIRE(pair.size() == 4);
  CHECK(pair[1] == 3);
  CHECK(pair[2] == 4);
}
