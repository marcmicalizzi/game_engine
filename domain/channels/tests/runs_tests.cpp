// The count of how whole a stream is drawn (channels.md, "How whole a stream is drawn"; W130).
#include <domain/channels/in_water.h>
#include <domain/channels/runs.h>
#include <domain/scene_gen/water_channel.h>

#include <doctest/doctest.h>

using namespace engine;
using namespace engine::channels;

namespace {
// A line down +x: one station a metre, each with its level, depth and flags.
scene::WaterChannel line(std::initializer_list<f64> levels, std::initializer_list<f32> depths,
                         std::initializer_list<u16> flags) {
  scene::WaterChannel c;
  f64 x = 0;
  for (const f64 y : levels) {
    c.points.push_back(WorldPos{x, y, 0.0});
    x += 1.0;
  }
  for (const f32 d : depths)
    c.depth_m.push_back(d);
  for (const u16 f : flags)
    c.flags.push_back(f);
  return c;
}
constexpr u16 k_lip = scene_gen::k_channel_fall_lip;
constexpr u16 k_foot = scene_gen::k_channel_fall_foot;
constexpr u16 k_over = scene_gen::k_channel_over_bank;
}  // namespace

TEST_CASE("a line's pieces and runs: a fall laid as a nappe joins the water, a film parts it") {
  // 0-1-2 surface, 2-3 a fall (the foot 1 m lower), 3-4-5 surface, 5-6-7 a climbing film over
  // its bank (not drawn), 7-8 surface again.
  const scene::WaterChannel c =
      line({10.0, 9.9, 9.8, 8.8, 8.7, 8.6, 8.7, 8.8, 8.7},
           {0.05f, 0.05f, 0.05f, 0.05f, 0.05f, 0.005f, 0.005f, 0.005f, 0.05f},
           {0, 0, k_lip, k_foot, 0, k_over, k_over, k_over, 0});
  const LineRuns r = measure_runs(c);
  CHECK(r.length_m == doctest::Approx(8.0));
  CHECK(r.fall_m == doctest::Approx(1.3));
  CHECK(r.falls == 1);
  CHECK(r.falls_drawn == 1);
  CHECK(r.pieces == 3);  // 0-2, 3-5, 7-8
  CHECK(r.runs == 2);    // 0-5 across the nappe, 7-8
  CHECK(r.gaps == 1);    // 5-7
  CHECK(r.undrawn_m == doctest::Approx(2.0));
  CHECK(r.film_m == doctest::Approx(2.0));
  // The forest's count: stations 5, 6 and 7 are thin, so water 0-4, film 5-7, water 8: three
  // runs, and the stretches leaving 5, 6 and 7 thin (the last station leaves none).
  CHECK(r.alternations == 3);
  CHECK(r.thin_m == doctest::Approx(3.0));
}

TEST_CASE("a fall whose foot does not stand lower is no nappe, and the classes are the forest's") {
  const scene::WaterChannel c =
      line({10.0, 9.0, 9.0, 8.0}, {0.05f, 0.05f, 0.05f, 0.05f}, {0, k_lip, k_foot, 0});
  const LineRuns r = measure_runs(c);
  CHECK(r.falls == 1);
  CHECK(r.falls_drawn == 0);
  CHECK(r.pieces == 2);
  CHECK(r.runs == 2);
  CHECK(run_class(0.0) == 0);
  CHECK(run_class(0.02) == 1);
  CHECK(run_class(0.149) == 2);
  CHECK(run_class(0.3) == 4);
  RunsBySlope by;
  by.add(r);  // 2 m over 3 m: over 30 %
  CHECK(by.classes[4].lines == 1);
  CHECK(by.classes[4].pieces == 2);
}

TEST_CASE("a line's staircase: the fall its steps take, and its level water (W132)") {
  // Two pools of two metres, level, each left over a 0.4 m step, then a metre of run falling
  // 0.2 m: a metre of fall over eight metres, 0.8 m of it at the two steps, four metres level.
  const scene::WaterChannel c = line({10.0, 10.0, 10.0, 9.6, 9.6, 9.6, 9.2, 9.2, 9.0},
                                     {0.2f, 0.2f, 0.05f, 0.2f, 0.2f, 0.05f, 0.05f, 0.05f, 0.05f},
                                     {0, 0, k_lip, k_foot, 0, k_lip, k_foot, 0, 0});
  const LineRuns r = measure_runs(c);
  CHECK(r.falls == 2);
  CHECK(r.fall_m == doctest::Approx(1.0));
  CHECK(r.step_fall_m == doctest::Approx(0.8));
  CHECK(r.level_m == doctest::Approx(5.0));  // 0-1-2, 3-4-5, 6-7; the steps and 7-8 are not
  RunsBySlope by;
  by.add(r);  // 12.5 %
  CHECK(by.classes[2].lines == 1);
  CHECK(by.classes[2].step_fall_m == doctest::Approx(0.8));
  const JsonValue json = runs_json(by);
  const JsonValue& one = json[2];
  const auto number = [&](const char* key) {
    f64 v = -1.0;
    REQUIRE(one.find(key) != nullptr);
    CHECK(one.find(key)->get_f64(v));
    return v;
  };
  CHECK(number("steps_per_100m") == doctest::Approx(25.0));
  CHECK(number("step_fall_share") == doctest::Approx(0.8));
  CHECK(number("step_height_mean_m") == doctest::Approx(0.4));
  CHECK(number("level_share") == doctest::Approx(0.625));
  // A plane at the reach's mean fall has no step and no level water.
  const scene::WaterChannel plane =
      line({10.0, 9.9, 9.8, 9.7}, {0.05f, 0.05f, 0.05f, 0.05f}, {0, 0, 0, 0});
  const LineRuns p = measure_runs(plane);
  CHECK(p.step_fall_m == 0.0);
  CHECK(p.level_m == 0.0);
}

TEST_CASE("a line's walls: a level climbing from a film over its bank is counted, not drawn") {
  // W138: 0-1 surface, 1 a film over its bank, 2 a pool 0.8 m over it (the forest's wall), 2-3
  // the pool, 3-4 a 6 cm climb between two plain stations (drawn: the description says so), 4-5
  // a jump (its foot over its head, not counted).
  constexpr u16 k_head = scene_gen::k_channel_jump_head;
  constexpr u16 k_jfoot = scene_gen::k_channel_jump_foot;
  const scene::WaterChannel c =
      line({10.0, 9.9, 10.7, 10.7, 10.76, 10.0, 10.6},
           {0.05f, 0.005f, 0.6f, 0.6f, 0.6f, 0.02f, 0.3f}, {0, k_over, 0, 0, 0, k_head, k_jfoot});
  const LineRuns r = measure_runs(c);
  CHECK(r.climbs == 2);
  CHECK(r.walls == 1);
  CHECK(r.walls_drawn == 0);
  CHECK(r.climb_worst_m == doctest::Approx(0.8));
  CHECK(r.undrawn_m == doctest::Approx(1.0));
  RunsBySlope by;
  by.add(r);
  const JsonValue j = runs_json(by);
  u32 walls = 0;
  f64 v = 0;
  for (u32 k = 0; k < k_run_classes; ++k)
    if (const JsonValue* w = j[k].find("walls"); w != nullptr && w->get_f64(v))
      walls += static_cast<u32>(v);
  CHECK(walls == 1);
}

TEST_CASE("what stands in a line's water: within its wet reach, each side by its own") {
  // W138: a line down +x, 2 m wet to its left (+z, the draw's left for a line down +x is the
  // side where heading.x * z > 0) and 0.5 m to its right, level 10 falling to 9.
  scene::WaterChannel c = line({10.0, 9.5, 9.0}, {0.3f, 0.3f, 0.3f}, {0, 0, 0});
  c.points[1].x = 10.0;
  c.points[2].x = 20.0;
  c.wet_left_m.assign(3, 2.0f);
  c.wet_right_m.assign(3, 0.5f);
  const scene::WaterChannel blocks[1] = {c};
  const ChannelWater water(blocks);
  u32 block = 99;
  f64 level = 0;
  REQUIRE(water.level_at(5.0, 1.5, block, level));
  CHECK(block == 0);
  CHECK(level == doctest::Approx(9.75));
  CHECK_FALSE(water.level_at(5.0, -1.0, block, level));  // past the right's half metre
  CHECK(water.level_at(5.0, -0.4, block, level));
  CHECK_FALSE(water.level_at(5.0, 2.5, block, level));
  CHECK_FALSE(water.level_at(30.0, 0.0, block, level));
  // A stretch the draw leaves out holds no water: a film over its bank climbing.
  scene::WaterChannel film = c;
  film.flags[0] = scene_gen::k_channel_over_bank;
  film.flags[1] = scene_gen::k_channel_over_bank;
  film.points[1].y = 10.2;
  const scene::WaterChannel films[1] = {film};
  const ChannelWater none(films);
  CHECK_FALSE(none.level_at(5.0, 0.0, block, level));
  CHECK(none.level_at(15.0, 0.0, block, level));
}

TEST_CASE("how fast a class's drawn water runs: the speed and the depth by length (W137)") {
  // 0-1 and 1-2 drawn, 2-3 a fall (never counted), 3-4 drawn: three drawn stretches, their speeds
  // the means of their stations' (0.15, 0.25, 0.9) and their depths (0.05, 0.05, 0.03).
  scene::WaterChannel c = line({10.0, 9.9, 9.8, 8.8, 8.7}, {0.05f, 0.05f, 0.05f, 0.03f, 0.03f},
                               {0, 0, k_lip, k_foot, 0});
  for (const f32 v : {0.1f, 0.2f, 0.3f, 0.8f, 1.0f})
    c.speed_mps.push_back(v);
  const LineRuns r = measure_runs(c);
  REQUIRE(r.speed_mps.size() == 3);
  CHECK(r.speed_mps[0] == doctest::Approx(0.15));
  CHECK(r.speed_mps[2] == doctest::Approx(0.9));
  CHECK(r.depth_m[2] == doctest::Approx(0.03));
  // By length, sorted by value: half the drawn length is under 0.25 m/s.
  CHECK(weighted_quantile(r.speed_mps, r.stretch_m, 0.5) == doctest::Approx(0.25));
  CHECK(weighted_quantile(r.speed_mps, r.stretch_m, 0.9) == doctest::Approx(0.9));
  CHECK(weighted_quantile(r.speed_mps, r.stretch_m, 0.1) == doctest::Approx(0.15));
  CHECK(weighted_quantile(Vector<f64>{}, Vector<f64>{}, 0.5) == 0.0);
  RunsBySlope by;
  by.add(r);
  const JsonValue json = runs_json(by);
  const JsonValue& cls = json[run_class(r.slope())];
  f64 median = 0;
  REQUIRE(cls.find("speed_median_mps") != nullptr);
  CHECK(cls.find("speed_median_mps")->get_f64(median));
  CHECK(median == doctest::Approx(0.25));
  // A description with no speeds (laid before W120) counts none.
  c.speed_mps.clear();
  CHECK(measure_runs(c).speed_mps.empty());
}
