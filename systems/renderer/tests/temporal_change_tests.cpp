// The change between displayed frames (temporal_change.h; renderer.md, "The change between frames,
// as the eye sees it"), on the CPU: pictures of the test's own with a known change.

#include <systems/renderer/temporal_change.h>

#include <doctest/doctest.h>

#include <cmath>
#include <string>
#include <vector>

using namespace engine;
using engine::renderer::TemporalChange;

namespace {

std::vector<u8> picture(u32 w, u32 h, u8 grey) {
  std::vector<u8> p(static_cast<usize>(w) * h * 4u, grey);
  for (usize i = 3; i < p.size(); i += 4) p[i] = 255;
  return p;
}

}  // namespace

TEST_CASE("temporal change: a held picture changes nothing, and one step is one step") {
  constexpr u32 w = 64;
  constexpr u32 h = 32;
  TemporalChange t;
  std::vector<u8> a = picture(w, h, 100);
  CHECK(t.add(a.data(), w, h));
  CHECK(t.stats(TemporalChange::k_bands).pairs == 0);  // the first picture only sets the next's base
  CHECK(t.add(a.data(), w, h));
  TemporalChange::Stats s = t.stats(TemporalChange::k_bands);
  CHECK(s.pairs == w * h);
  CHECK(s.median_codes == 0.0);
  CHECK(s.p999_codes == 0.0);
  CHECK(s.at_least_one == 0.0);
  CHECK(s.mean_light == 0.0);
  // A quarter of the pixels one code brighter in every channel: one step of luma, and their light
  // by (101/100)^2.2 - 1 relative to their level, about 2.2 %.
  std::vector<u8> b = a;
  for (u32 i = 0; i < w * h / 4; ++i)
    for (u32 c = 0; c < 3; ++c) b[i * 4u + c] = 101;
  CHECK(t.add(b.data(), w, h));
  CHECK(t.frames() == 3);
  s = t.stats(TemporalChange::k_bands);
  CHECK(s.pairs == 2u * w * h);
  CHECK(s.at_least_one == doctest::Approx(0.125));
  CHECK(s.at_least_two == 0.0);
  CHECK(s.p95_codes == doctest::Approx(1.0));  // an eighth of the pairs moved a step
  CHECK(s.median_codes == 0.0);
  const f64 weber = 2.0 * (std::pow(101.0 / 255.0, 2.2) - std::pow(100.0 / 255.0, 2.2)) /
                    (std::pow(101.0 / 255.0, 2.2) + std::pow(100.0 / 255.0, 2.2));
  CHECK(s.p95_light <= weber);  // the lower edge of the tenth of a percent it falls in
  CHECK(s.p95_light > weber - 0.0011);
  // Every pixel is in the band of codes 64 to 127, and none elsewhere.
  CHECK(t.stats(4).pairs == s.pairs);
  for (u32 b2 = 0; b2 < TemporalChange::k_bands; ++b2)
    if (b2 != 4) CHECK(t.stats(b2).pairs == 0);
  // The series: one entry a picture, the level as displayed.
  CHECK(t.mean_luma().size() == 3);
  CHECK(t.mean_luma()[0] == doctest::Approx(100.0));
  CHECK(t.mean_luma()[2] == doctest::Approx(100.25));
  CHECK(t.centre_luma()[2] == doctest::Approx(100.0));  // the brightened rows are above the centre
  const std::string json = t.json();
  CHECK(json.find("\"format\":\"engine.temporal-change.v1\"") != std::string::npos);
  CHECK(json.find("\"bands\":[") != std::string::npos);
  CHECK(json.find("\"mean_luma\":[100.00000,100.00000,100.25000]") != std::string::npos);
}

TEST_CASE("temporal change: black stays out of the light's measure, and a new size starts again") {
  TemporalChange t;
  std::vector<u8> a = picture(8, 8, 0);
  std::vector<u8> b = picture(8, 8, 0);
  b[0] = 1;  // one red code on black: a luma change, and light against a floor of half a code
  t.add(a.data(), 8, 8);
  t.add(b.data(), 8, 8);
  const TemporalChange::Stats s = t.stats(0);
  CHECK(s.pairs == 64);
  CHECK(s.light_pairs == 0);  // under the floor
  CHECK(s.mean_codes > 0.0);
  std::vector<u8> c = picture(4, 4, 0);
  CHECK(!t.add(c.data(), 4, 4));  // another size: compared with nothing
  CHECK(t.stats(TemporalChange::k_bands).pairs == 64);
}
