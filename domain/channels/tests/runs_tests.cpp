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
