#pragma once

// **The flow fixture, as the water's tests hold it** (docs/subsystems/channels.md, "The fixture";
// roadmap W120, S106): the three solved channels `engine-content flow fixture` writes and the
// ground they were solved on, here as numbers and a function so the water capability's tests need
// neither the channels capability nor a file. The stations are the fixture's `scene.json`'s, to
// the four digits it prints (written from W120's stage 0, c8c6f9f6); the ground is the fixture's
// own closed form, `min(13, bed(x) + max(0, |z - z_line| - 1))`: three trenches along +x at
// z = -20, 0 and 20, a flat bed 2 m wide, banks rising a metre a metre.

#include <core/base/types.h>
#include <core/math/world.h>
#include <domain/scene_gen/scene_gen.h>
#include <domain/scene_gen/water_channel.h>

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <schemas/scene.h>

namespace engine::channel_fixture {

struct Station {
  f64 x;
  f64 level;
  f32 depth;
  f32 speed;
  f32 froude;
  f32 wet;
  u16 flags;
};

inline scene::WaterChannel solved(const char* name, f64 z,
                                  std::initializer_list<Station> stations) {
  scene::WaterChannel c;
  c.name = name;
  for (const Station& s : stations) {
    c.points.push_back(WorldPos{s.x, s.level, z});
    c.width_m.push_back(4.0f);
    c.depth_m.push_back(s.depth);
    c.slope.push_back(0.01f);
    c.discharge_m3s.push_back(1.5993f);
    c.speed_mps.push_back(s.speed);
    c.froude.push_back(s.froude);
    c.wet_left_m.push_back(s.wet);
    c.wet_right_m.push_back(s.wet);
    c.flags.push_back(s.flags);
  }
  return c;
}

// "uniform": normal depth 0.5 m on a slope of 0.01, six stations 8 m apart.
inline scene::WaterChannel uniform() {
  return solved("uniform", -20.0,
                {{8.0, 10.5, 0.5f, 1.2794f, 0.6328f, 1.5f, 0},
                 {16.0, 10.42, 0.5f, 1.2794f, 0.6328f, 1.5f, 0},
                 {24.0, 10.34, 0.5f, 1.2794f, 0.6328f, 1.5f, 0},
                 {32.0, 10.26, 0.5f, 1.2794f, 0.6328f, 1.5f, 0},
                 {40.0, 10.18, 0.5f, 1.2794f, 0.6328f, 1.5f, 0},
                 {48.0, 10.1, 0.5f, 1.2794f, 0.6328f, 1.5f, 0}});
}

// "step": an M2 drawdown to a critical crest at x = 38 (33: control, crest, a fall's lip), a 2 m
// fall to Rand's foot at x = 40 (64), a shooting run whose level rises, a jump from x = 41.246 (8)
// to x = 44.201 (16), and the pool's backwater to the mouth at x = 58 (3).
inline scene::WaterChannel step() {
  return solved("step", 0.0,
                {{8.0, 12.499, 0.4986f, 1.2837f, 0.6357f, 1.499f, 0},
                 {16.0, 12.416, 0.4964f, 1.2905f, 0.6403f, 1.496f, 0},
                 {24.0, 12.331, 0.4906f, 1.3090f, 0.6528f, 1.491f, 0},
                 {32.0, 12.232, 0.4722f, 1.3699f, 0.6946f, 1.472f, 0},
                 {37.0, 12.138, 0.4281f, 1.5384f, 0.8141f, 1.428f, 0},
                 {37.75, 12.107, 0.4050f, 1.6421f, 0.8905f, 1.405f, 0},
                 {38.0, 12.077, 0.3767f, 1.7861f, 1.0f, 1.377f, 33},
                 {40.0, 9.837, 0.1373f, 5.4507f, 4.8454f, 1.137f, 64},
                 {41.246, 9.88, 0.18f, 4.0748f, 3.1902f, 1.18f, 8},
                 {44.201, 10.372, 0.6725f, 0.8899f, 0.3876f, 1.672f, 16},
                 {52.0, 10.336, 0.6359f, 0.9542f, 0.4256f, 1.636f, 0},
                 {58.0, 10.3, 0.6f, 1.0252f, 0.4688f, 1.6f, 3}});
}

// "mouth": an M1 backwater into a lake at 10.4.
inline scene::WaterChannel mouth() {
  return solved("mouth", 20.0,
                {{8.0, 10.544, 0.5438f, 1.1562f, 0.5515f, 1.544f, 0},
                 {16.0, 10.5, 0.5796f, 1.0697f, 0.4965f, 1.58f, 0},
                 {24.0, 10.468, 0.6281f, 0.9689f, 0.4345f, 1.628f, 0},
                 {32.0, 10.446, 0.6864f, 0.8673f, 0.3745f, 1.686f, 0},
                 {40.0, 10.432, 0.7516f, 0.7733f, 0.3213f, 1.752f, 0},
                 {48.0, 10.421, 0.8214f, 0.6901f, 0.2763f, 1.821f, 0},
                 {56.0, 10.414, 0.8941f, 0.6180f, 0.2388f, 1.894f, 0},
                 {64.0, 10.409, 0.9689f, 0.5560f, 0.2077f, 1.969f, 0},
                 {72.0, 10.405, 1.0451f, 0.5026f, 0.1819f, 2.045f, 0},
                 {80.0, 10.402, 1.1222f, 0.4565f, 0.1604f, 2.122f, 0},
                 {88.0, 10.4, 1.2f, 0.4165f, 0.1423f, 2.2f, 3}});
}

// The fixture's ground, metres, at a place in metres.
inline f64 ground_m(f64 x, f64 z) noexcept {
  f64 line = 0.0;
  f64 bed = 0.0;
  if (z < -10.0) {
    line = -20.0;
    bed = 10.0 - 0.01 * (x - 8.0);
  } else if (z < 10.0) {
    line = 0.0;
    // The crest at x = 38, a face dropping 2 m over one lattice cell, a level apron at 9.7.
    if (x <= 38.0)
      bed = 12.0 - 0.01 * (x - 8.0);
    else if (x < 38.25)
      bed = 11.7 - 2.0 * (x - 38.0) / 0.25;
    else
      bed = 9.7;
  } else {
    line = 20.0;
    bed = 10.0 - 0.01 * (x - 8.0);
  }
  return std::min(13.0, bed + std::max(0.0, std::abs(z - line) - 1.0));
}

inline f64 ground_fn(const void*, i64 x_mm, i64 z_mm) noexcept {
  return ground_m(static_cast<f64>(x_mm) * 0.001, static_cast<f64>(z_mm) * 0.001);
}

inline scene_gen::Ground ground() {
  scene_gen::Ground g;
  g.surface_fn = &ground_fn;
  return g;
}

}  // namespace engine::channel_fixture
