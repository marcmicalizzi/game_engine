// A shading rate by region, its CPU side (visibility_resolve.h; docs/subsystems/renderer.md, "A
// shading rate by region"; roadmap R126): the sides' default by the frame's aspect, and the
// full-rate columns of a view — the window the half pass dispatches around and the resolve tests
// a pixel against. No device; the pictures are systems/renderer's alpha_mask_tests.cpp.

#include <domain/gfx/visibility_resolve.h>

#include <doctest/doctest.h>

using namespace engine;

TEST_CASE("half rate: the sides' default is a third of a surround and nothing up to 2.5:1") {
  const f32 third = 1.0f / 3.0f;
  CHECK(gfx::half_rate_sides(-1.0f, 11520, 2160) == third);  // the owner's three monitors
  CHECK(gfx::half_rate_sides(-1.0f, 5120, 1440) == third);   // 32:9
  CHECK(gfx::half_rate_sides(-1.0f, 3840, 2160) == 0.0f);    // 4K
  CHECK(gfx::half_rate_sides(-1.0f, 3440, 1440) == 0.0f);    // 21:9
  CHECK(gfx::half_rate_sides(-1.0f, 5400, 2160) == 0.0f);    // exactly 2.5:1 is not wider
  CHECK(gfx::half_rate_sides(-1.0f, 5402, 2160) == third);
  // Asked for, the tunable is the answer whatever the aspect, up to half the frame at each side.
  CHECK(gfx::half_rate_sides(0.25f, 3840, 2160) == 0.25f);
  CHECK(gfx::half_rate_sides(0.0f, 11520, 2160) == 0.0f);
  CHECK(gfx::half_rate_sides(0.9f, 11520, 2160) == 0.5f);
}

TEST_CASE("half rate: one surround view keeps its centre third at full rate, in whole blocks") {
  const gfx::HalfRateWindow w = gfx::half_rate_window(1.0f / 3.0f, false, 11520, 0, 11520, 11520);
  CHECK(w.x0 == 3840);
  CHECK(w.x1 == 7680);
  CHECK(w.columns == 1920 + 1920);
  // Off: every column at full rate, nothing to dispatch.
  const gfx::HalfRateWindow off = gfx::half_rate_window(0.0f, false, 11520, 0, 11520, 11520);
  CHECK(off.x0 == 0);
  CHECK(off.x1 == 11520);
  CHECK(off.columns == 0);
  // The centre too: no full-rate column.
  const gfx::HalfRateWindow all = gfx::half_rate_window(0.0f, true, 11520, 0, 11520, 11520);
  CHECK(all.x0 == all.x1);
  CHECK(all.columns == 5760);
  // An odd width's last block is a column of its own and stays at the rate of its side.
  const gfx::HalfRateWindow odd = gfx::half_rate_window(0.0f, false, 129, 0, 129, 129);
  CHECK(odd.columns == 0);
  CHECK(odd.x1 == 130);
}

TEST_CASE("half rate: three surround views are the side monitors at half rate and the centre not") {
  const f32 third = 1.0f / 3.0f;
  const gfx::HalfRateWindow left = gfx::half_rate_window(third, false, 11520, 0, 3840, 3840);
  const gfx::HalfRateWindow centre = gfx::half_rate_window(third, false, 11520, 3840, 3840, 3840);
  const gfx::HalfRateWindow right = gfx::half_rate_window(third, false, 11520, 7680, 3840, 3840);
  CHECK(left.columns == 1920);
  CHECK(left.x0 == left.x1);  // no full-rate column: the whole view
  CHECK(centre.columns == 0);
  CHECK(centre.x0 == 0);
  CHECK(centre.x1 == 3840);
  CHECK(right.columns == 1920);
  CHECK(right.x0 == right.x1);
  // A Panini view's wider source: the window scales with it, and its edges stay even.
  const gfx::HalfRateWindow panini = gfx::half_rate_window(0.25f, false, 3840, 0, 3840, 4800);
  CHECK(panini.x0 == 1200);
  CHECK(panini.x1 == 3600);
  CHECK(panini.x0 % 2 == 0);
  CHECK(panini.columns == 600 + 600);
  CHECK(gfx::half_rate_blocks(11520, 2160) == u64{5760} * 1080);
  CHECK(gfx::half_rate_blocks(129, 7) == u64{65} * 4);
}
