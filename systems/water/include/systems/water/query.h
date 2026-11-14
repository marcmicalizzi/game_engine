#pragma once

// **The water at a point** (docs/subsystems/water.md, "The water at a point"; roadmap A43): what a
// game module asks through `HostApi::water_at` (apps.md, "What a module can ask the world") —
// whether a point is under water, the surface's level there, the depth, and the water's velocity in
// the ground plane where it has one.
//
// **Two kinds of water, one question, each behind one function.** A body's answer is its cooked
// depth map (`body_water_at`: the map the content's reader read beside the body's mesh, the one the
// resolve shades the bottom from), not the ground asked again, so what a module reads is what is
// drawn; its velocity is zero. A channel's answer is `channel_water_at`, **the one function a
// channel's cooked field stands behind**: today it answers from the description as the scene gives
// it (`WaterChannel`: the centreline's surface heights, the widths and depths, the slope, Manning's
// speed with the 1/6-power lateral profile, as the mesher draws it), and when the channels are
// solved from their discharge (a level, a depth, a speed and the wet reaches at every station) the
// solved stations go behind the same function and nothing a module calls changes.
//
// **Determinism.** Every number is a function of the point and the scene's water, in + - * /, sqrt
// and `cube_root` (channel.h): the same bits from every toolchain, and asking changes nothing.
// **Cost.** A body is a bounding test and four texels; a channel a pass over its stretches, about a
// hundred for a stream across a 1 km world. No allocation.

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

// **A channel's answer — the one function its solved stations go behind.** The stretch of the
// centreline whose (x, z) is nearest the point and holds it within half its width there: the
// surface's height along the stretch, the depth of the parabolic bed at that offset, and the
// velocity along the stretch's heading at Manning's mean speed for the centre's depth and the
// slope, times the 1/6-power lateral profile (`lateral_speed_share`, here as `cube_root(sqrt(r))`).
bool channel_water_at(const Channel& channel, WorldPos at, WaterPoint& out) noexcept;

// **The scene's water, held for the questions.** Bodies with their depth maps, and channels; a
// point asks each and the deepest wet answer wins.
class WaterQuery {
 public:
  // `ground` (may be null) is what an open body is asked past its map; it must outlive the query.
  void set_ground(const scene_gen::Ground* ground) noexcept { ground_ = ground; }
  void add_body(Body body, DepthMap map);
  void add_channel(Channel channel);
  u32 bodies() const noexcept { return bodies_.size(); }
  u32 channels() const noexcept { return channels_.size(); }
  bool empty() const noexcept { return bodies_.empty() && channels_.empty(); }
  // Dry is an answer: `out.wet` false. The body's `index` counts bodies and the channel's channels.
  void sample(WorldPos at, WaterPoint& out) const noexcept;

 private:
  struct HeldBody {
    Body body;
    DepthMap map;
  };
  Vector<HeldBody> bodies_;
  Vector<Channel> channels_;
  const scene_gen::Ground* ground_ = nullptr;
};

}  // namespace engine::water
