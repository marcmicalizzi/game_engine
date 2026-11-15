#pragma once

// **The water at a point** (docs/subsystems/water.md, "The water at a point"; roadmap A43): what a
// game module asks through `HostApi::water_at` (apps.md, "What a module can ask the world") —
// whether a point is under water, the surface's level there, the depth, and the water's velocity in
// the ground plane where it has one.
//
// **Two kinds of water, one question, each behind one function.** A body's answer is its cooked
// depth map (`body_water_at`: the map the content's reader read beside the body's mesh, the one the
// resolve shades the bottom from), not the ground asked again, so what a module reads is what is
// drawn; its velocity is zero. A channel's answer is `channel_water_at`, **the solve's**: the
// stations the hydraulic solve wrote (W120: a level, the wet reach either side, the section's mean
// speed and Froude number, the flags) read through `water::channel_sample` (channel.h, S106), the
// same evaluation the sheet and its depth map are drawn from. Nothing derives a speed from a slope
// or a depth from a parabola; a channel that was never solved is refused at load
// (`read_channel`) and answers nothing.
//
// **Determinism.** Every number is a function of the point and the scene's water, in + - * / and
// sqrt: the same bits from every toolchain, and asking changes nothing. **Cost.** A body is a
// bounding test and four texels; a channel is asked only where the index (below) says one is near,
// and then only along the stretches near the point. No allocation.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/scene_gen/scene_gen.h>
#include <systems/water/channel.h>
#include <systems/water/water.h>

#include <span>

namespace engine::water {

inline constexpr u32 k_no_water = 0xFFFFFFFFu;

// What one water says about a point. `source` 1 a body, 2 a channel (module_abi.h's
// `k_water_body`, `k_water_channel`), `index` which, in the order the query was given them.
struct WaterPoint {
  bool wet = false;
  f64 level = 0.0;   // world metres: the surface's height
  f32 depth = 0.0f;  // the surface less the bed, metres, > 0 where wet
  Vec2 velocity{};   // (x, z), m/s
  u8 source = 0;
  u32 index = k_no_water;
};

// **A body's answer**: wet where the point is inside the body (its outline, its flooded region, or
// anywhere for an open body) and its depth map says the bed is below the level there — the map
// sampled bilinearly in its texels' signed depths, the level the body's. Past the map's square, an
// open body answers from `ground` (its level less the ground's surface) where it has one, and dry
// otherwise. A body with no map (`map.size` 0) answers from `ground` as `depth_at` does.
bool body_water_at(const Body& body, const DepthMap& map, const scene_gen::Ground* ground,
                   WorldPos at, WaterPoint& out) noexcept;

// **A channel's answer — the solve's, through `channel_sample`** (channel.h): over the stretches
// [first, last] of the line (all of them by default), the surface's level where the point's
// nearest place on the line is, the depth as that level less the ground under the point (the
// solved greatest depth where there is no ground), and the surface's velocity in the ground plane
// (the section's solved mean speed, times the surface factor and the lateral share of the point's
// depth; on a fall the nappe's own). Dry outside the wet reach, on a stretch drawn over its bank,
// or where the ground stands at or above the level. Nothing here reads `slope` or `manning_n`.
bool channel_water_at(const Channel& channel, const ChannelLine& line,
                      const scene_gen::Ground& ground, WorldPos at, WaterPoint& out, u32 first = 0,
                      u32 last = 0xffffffffu) noexcept;

// **The channels' index**: a grid of `k_channel_cell_m` cells over every stretch's bounds grown by
// its channel's reach (the widest wet reach and the margin under the bank), holding for each cell
// one span of stretches a channel, from the first stretch whose grown bounds touch the cell to the
// last. A point asks only its cell's spans: a channel whose reach does not come near it is never
// looked at, and a long one only along the stretches that do. **The same answer as asking every
// stretch**: wet needs the nearest stretch within the channel's reach, and every stretch within
// that reach of the point, the nearest and every one tying with it, is in the point's span. The
// cell doubles until the grid is at most `k_channel_cells_max` a side.
inline constexpr f64 k_channel_cell_m = 16.0;
inline constexpr u32 k_channel_cells_max = 512;

// **The scene's water, held for the questions.** Bodies with their depth maps, and channels with
// their lines and index; a point asks each body and its cell's channels, and the deepest wet
// answer wins.
class WaterQuery {
 public:
  void add_body(Body body, DepthMap map);
  void add_channel(Channel channel);
  // **Once, after the last `add_channel`**: the ground (may be null; it must outlive the query) —
  // what an open body is asked past its map and a channel's depth is measured to — and the
  // channels' index built. A query never prepared asks every channel's every stretch.
  void prepare(const scene_gen::Ground* ground);
  u32 bodies() const noexcept { return bodies_.size(); }
  u32 channels() const noexcept { return channels_.size(); }
  bool empty() const noexcept { return bodies_.empty() && channels_.empty(); }
  // Dry is an answer: `out.wet` false. The body's `index` counts bodies and the channel's channels.
  // Allocates nothing.
  void sample(WorldPos at, WaterPoint& out) const noexcept;

 private:
  struct HeldBody {
    Body body;
    DepthMap map;
  };
  struct ChannelSpan {
    u32 channel;
    u32 first;
    u32 last;
  };
  u32 cell_of(f64 v, u32 cells, f64 origin) const noexcept;
  Vector<HeldBody> bodies_;
  Vector<Channel> channels_;
  Vector<ChannelLine> lines_;
  const scene_gen::Ground* ground_ = nullptr;
  // The index: `cell_start_[k]` to `cell_start_[k + 1]` are cell k's spans (row-major in x).
  Vector<u32> cell_start_;
  Vector<ChannelSpan> spans_;
  f64 grid_x_ = 0.0;
  f64 grid_z_ = 0.0;
  f64 cell_m_ = k_channel_cell_m;
  u32 cells_x_ = 0;
  u32 cells_z_ = 0;
};

}  // namespace engine::water
