// **More lamps than the frame's room** (roadmap R239 task 5; renderer.md, "More lamps than the
// frame's room"; docs/experiments/lights-by-reach.md, "More lamps than the room"): what a frame's
// CPU pays for its lamps at 256 (the room, `k_max_lamps`), 1,024 and 4,096 — a city's street
// lights, measured and not changed. A street grid of 1.5 km with a lamp every 35 m on both sides
// at 9 m, the eye at street level in the middle, each lamp a spot of 1,910 cd pointed down (the
// Toronto proof of concept's cobra heads), at the fire's rule for its reach (195 m) and at a reach
// an author would name (50 m). The three costs a frame has per light: the array's fill
// (`add_lamp_lights`' arithmetic, into a vector the size of the count since the frame's own
// array holds 322), the light grid's build over it (`gfx::build_light_grid`, its 65,536 words of
// cells), and the copy of both into the frame slot's mapped buffer (here a host vector of the
// slot's size). No device: the GPU side is the resolve's walk, which the grid bounds by reach.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/world.h>
#include <domain/gfx/light_grid.h>
#include <domain/gfx/visibility_resolve.h>
#include <foundation/bench/bench.h>
#include <systems/renderer/fire.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

using namespace engine;
using namespace engine::renderer;

namespace {

// The lamps of a street grid: 15 streets each way, 105 m apart, over 1.5 km, a lamp every 35 m on
// each kerb (5,160 in all), the `count` nearest the middle in the order of their distance from it.
Vector<FrameFire> street_lamps(u32 count, f32 reach_m) {
  std::vector<WorldPos> at;
  for (i32 street = -7; street <= 7; ++street) {
    for (i32 step = -21; step <= 21; ++step) {
      for (const f64 kerb : {-7.0, 7.0}) {
        const f64 across = 105.0 * street + kerb;
        const f64 along = 35.0 * step;
        at.push_back(WorldPos{along, 9.0, across});  // the streets along x
        at.push_back(WorldPos{across, 9.0, along});  // and along z
      }
    }
  }
  std::stable_sort(at.begin(), at.end(), [](const WorldPos& a, const WorldPos& b) {
    return a.x * a.x + a.z * a.z < b.x * b.x + b.z * b.z;
  });
  Vector<FrameFire> lamps;
  lamps.reserve(count);
  for (u32 i = 0; i < count && i < at.size(); ++i) {
    FrameFire l;
    l.base = at[i];
    l.intensity = 1910.0f / 128000.0f;  // candela in a sky's units
    l.color = Vec3{1.0f, 0.93f, 0.82f};
    l.reach_m = reach_m;
    l.radius_m = 0.1f;
    l.direction = Vec3{0.0f, -1.0f, 0.0f};
    l.cos_inner = 0.5f;  // 60 degrees
    l.cos_outer = 0.0f;  // 90: a full cut-off head
    lamps.push_back(l);
  }
  return lamps;
}

// `add_lamp_lights`' arithmetic for one lamp, into an array of any size: a hand copy, because the
// renderer's writes into the frame's `FrameLighting`, whose room (322) is smaller than the counts
// measured here. **The two must move together** (fire.h / scene_fire.cpp, `add_lamp_lights`).
void fill(std::span<const FrameFire> lamps, const WorldPos& eye, gfx::ResolveLight* out) noexcept {
  for (usize i = 0; i < lamps.size(); ++i) {
    const FrameFire& l = lamps[i];
    gfx::ResolveLight& light = out[i];
    light = gfx::ResolveLight{};
    light.position_radius = Vec4{relative(l.base, eye), l.reach_m};
    light.color_intensity = Vec4{l.color, l.intensity};
    light.direction_type = Vec4{l.direction, gfx::k_light_spot};
    light.spot = Vec4{l.cos_inner, l.cos_outer, l.radius_m, 0.0f};
  }
}

struct Frame {
  Vector<FrameFire> lamps;
  Vector<gfx::ResolveLight> lights;
  Vector<u32> cells;
  Vector<u8> slot;  // the frame slot's lights and grid, as the mapped buffer holds them
  WorldPos eye{3.0, 1.7, 2.0};
  explicit Frame(u32 count, f32 reach) : lamps(street_lamps(count, reach)) {
    lights.resize(count);
    cells.resize(gfx::k_light_grid_words);
    slot.resize(sizeof(gfx::ResolveLight) * count + sizeof(gfx::LightGrid) +
                sizeof(u32) * gfx::k_light_grid_words);
  }
  // One frame's CPU for its lamps: the fill, the grid, the copy. Returns the grid's set bits.
  u32 run() noexcept {
    fill(lamps, eye, lights.data());
    gfx::LightGrid grid;
    const bool built =
        gfx::build_light_grid(std::span<const gfx::ResolveLight>(lights.data(), lights.size()),
                              std::span<u32>(cells.data(), cells.size()), grid);
    std::memcpy(slot.data(), lights.data(), sizeof(gfx::ResolveLight) * lights.size());
    if (built) {
      u8* at = slot.data() + sizeof(gfx::ResolveLight) * lights.size();
      std::memcpy(at, &grid, sizeof(grid));
      std::memcpy(at + sizeof(grid), cells.data(), gfx::light_grid_bytes(grid) - sizeof(grid));
    }
    return built ? grid.refs : 0;
  }
};

void report_grid(const Frame& f) {
  gfx::LightGrid grid;
  Vector<u32> cells(gfx::k_light_grid_words);
  if (!gfx::build_light_grid(std::span<const gfx::ResolveLight>(f.lights.data(), f.lights.size()),
                             std::span<u32>(cells.data(), cells.size()), grid))
    return;
  const f64 per_cell = grid.cells > 0 ? static_cast<f64>(grid.refs) / grid.cells : 0.0;
  // A tool's report, which AGENTS.md ("Conventions") lets a bench print.
  std::printf("  lamps %u: %u cells (%u x %u x %u), %u words a cell, %.1f lights a cell\n",
              static_cast<u32>(f.lights.size()), grid.cells, grid.dims_x, grid.dims_y, grid.dims_z,
              grid.words, per_cell);
}

void lamps_frame(bench::State& state, f32 reach) {
  const u32 count = static_cast<u32>(state.arg());
  Frame f(count, reach);
  f.run();
  // The runner calls this for every calibration, warm-up and repeat: the grid's line once an
  // argument of each reach.
  static std::vector<std::pair<f32, u32>> reported;
  if (std::find(reported.begin(), reported.end(), std::pair<f32, u32>{reach, count}) ==
      reported.end()) {
    reported.emplace_back(reach, count);
    report_grid(f);
  }
  while (state.keep_running())
    bench::keep(f.run());
  state.set_items(count);
  state.set_bytes(sizeof(gfx::ResolveLight) * count);
}

}  // namespace

ENGINE_BENCH_ARGS(lamps_frame_rule, "renderer.lamps.frame_cpu.reach_rule", 256, 1024, 4096) {
  lamps_frame(state, fire_light_reach(1910.0f));
}

ENGINE_BENCH_ARGS(lamps_frame_50m, "renderer.lamps.frame_cpu.reach_50m", 256, 1024, 4096) {
  lamps_frame(state, 50.0f);
}
