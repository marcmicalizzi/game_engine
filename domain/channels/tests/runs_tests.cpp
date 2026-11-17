// The count of how whole a stream is drawn (channels.md, "How whole a stream is drawn"; W130).
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
