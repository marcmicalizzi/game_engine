#pragma once

// **Seeing the collision** (docs/subsystems/scene_collision.md, "Seeing the collision"; roadmap
// P62): what a `SceneCollision` holds — every held tile's proxy triangles as wireframe, one colour
// a body and the walker's own tile brighter, the ground's heightfield lattice as lines in a second
// colour, and the walker's capsule — drawn over the shaded picture, depth-tested against the
// visibility buffer's depth, by a frame pass registered at the renderer's registration point
// (`renderer::SceneRenderer::add_frame_pass`, systems/renderer/frame_pass.h). It is how an
// invisible wall is named: the owner's v35 walk stopped twice in open ground (2026-11-07), and the
// lines show the body that held it where nothing was drawn.
//
// The lines are made on the CPU every frame the draw is on, from the consumer's own held tiles, in
// the frame's space — metres from the frame's eye, the difference taken in f64 (ADR-0053) — and
// uploaded to a host-visible buffer a frame slot, as the overlay uploads its lists. A budget of
// lines a frame keeps the nearest tiles whole and drops the farthest first, and says what it
// dropped. Off, nothing of this exists: the pass is not registered and the frame is the bytes it
// was (frame_pass.h's contract).

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <core/math/world.h>
#include <domain/physics/physics.h>
#include <systems/scene_collision/scene_collision.h>
#include <systems/world/world.h>

#include <memory>
#include <string>

namespace engine::gfx {
class Device;
}
namespace engine::renderer {
class SceneRenderer;
struct FramePassContext;
}  // namespace engine::renderer

namespace engine::scene_collision {

// One line, in the frame's space (metres from the eye). Mirrored by `Line` in
// shaders/collision_lines.slang.
struct DrawLine {
  Vec3 a{};
  u32 color = 0;  // rgba8, r in the low byte
  Vec3 b{};
  u32 pad = 0;
};

// The walker's capsule, drawn as two rings and four sides.
struct DrawCapsule {
  WorldPos feet{};
  f32 radius = 0.3f;
  f32 height = 1.8f;
};
// Segments a ring of the capsule has; the capsule is 2 rings, 4 sides and 2 half-circle arcs a
// meridian plane (2 planes), every arc `k_capsule_ring_segments / 2` segments.
inline constexpr u32 k_capsule_ring_segments = 24;
inline constexpr u32 k_capsule_lines =
    2 * k_capsule_ring_segments + 4 + 2 * 2 * (k_capsule_ring_segments / 2);

// The colours (linear, in the exposed picture the pass draws into).
inline constexpr u32 k_ground_color = 0xFF806020u;         // the lattice: a dim teal
inline constexpr u32 k_ground_color_walker = 0xFFFFC040u;  // under the walker: bright teal
inline constexpr u32 k_capsule_color = 0xFF00FFFFu;        // yellow

struct DrawOptions {
  // Tiles whose nearest point is within this of the centre are drawn; 0 draws every held tile.
  f32 radius_m = 0.0f;
  // Lines a frame, the budget: past it the farthest tiles are dropped whole, first.
  u32 max_lines = 1u << 20;
  // A line's pixel is drawn where its depth is at least the frame's depth less this fraction of it
  // (reversed-Z): a line lying on the surface it outlines shows through the surface's own depth.
  f32 depth_tolerance = 0.01f;
};

struct DrawStats {
  u32 tiles_held = 0;     // what the consumer holds
  u32 tiles_drawn = 0;    // within the radius and the budget
  u32 tiles_dropped = 0;  // within the radius, dropped by the budget (the farthest first)
  u32 lines = 0;          // this frame's, every kind
  u32 proxy_lines = 0;    // three a proxy triangle
  u32 ground_lines = 0;   // the lattice's: 2 n (n - 1) a tile, less those touching a hole
  u32 capsule_lines = 0;  // `k_capsule_lines` with a capsule
  u64 dropped_lines = 0;  // what the dropped tiles would have drawn
  i64 build_ns = 0;       // the CPU's time making this frame's lines
};

// A body's colour: the tile's coordinates hashed to a hue, the walker's own tile brighter.
u32 body_color(world::TileCoord coord, bool walker) noexcept;

// **The lines of what `collision` holds**, in the frame of `eye`: the capsule first when given,
// then the tiles within `options.radius_m` of `centre` nearest first — each tile's ground lattice
// and its pieces' proxy triangles (three edges a triangle, `Config::keep_shapes`) — until the next
// tile would pass `options.max_lines`, which is dropped with every farther one. `out` is cleared
// first. Allocates nothing once `out` has grown to the frame's count.
void collision_lines(const SceneCollision& collision, WorldPos eye, WorldPos centre,
                     const DrawCapsule* capsule, const DrawOptions& options, Vector<DrawLine>& out,
                     DrawStats& stats);

// **A consumer of its own round a point**, for a host with no walker to draw the collision of
// (engine-view's flight, `--collision-draw`): a physics world, a ring of the radius round the point
// with no budget, and a `SceneCollision` that keeps its shapes — the same bodies a walker standing
// there would meet, since a consumer's tiles are a function of the scene and the numbers.
class CollisionView {
 public:
  CollisionView();
  ~CollisionView();
  ENGINE_NON_COPYABLE(CollisionView);
  // `config.keep_shapes` is set here. `radius_tiles` is the ring's.
  bool create(const renderer::SceneData& scene, const scene_gen::GroundProvider* ground,
              Config config, f32 radius_tiles, std::string* error = nullptr,
              const scene_gen::TileSource* tiles = nullptr);
  // The ring round `at`: tiles made and let go, unlimited.
  void update(WorldPos at);
  const SceneCollision& collision() const noexcept;
  bool valid() const noexcept { return impl_ != nullptr; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// **The draw**: registers its frame pass on `renderer` at `create` and removes it at `destroy`.
// Between frames the host names what to draw (`set`); the pass makes the lines at record time, in
// the frame's own eye.
class CollisionDraw {
 public:
  CollisionDraw();
  ~CollisionDraw();
  ENGINE_NON_COPYABLE(CollisionDraw);
  bool create(const gfx::Device& device, renderer::SceneRenderer& renderer,
              const DrawOptions& options, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return renderer_ != nullptr; }
  // What the next frames draw: the consumer (null: nothing), the centre the radius and the budget's
  // order are measured from, and the walker's capsule (null: none). The consumer must stay alive
  // while it is set.
  void set(const SceneCollision* collision, WorldPos centre, const DrawCapsule* capsule);
  // Or a consumer of the draw's host's own that follows the frame: at every frame it is updated
  // round the capsule's feet when one is set and the frame's eye otherwise, and drawn round that
  // point.
  void follow_eye(CollisionView* view) noexcept { view_ = view; }
  // The walker's capsule (null: none), kept until the next call.
  void set_capsule(const DrawCapsule* capsule) noexcept {
    has_capsule_ = capsule != nullptr;
    if (capsule != nullptr) capsule_ = *capsule;
  }
  const DrawStats& stats() const noexcept { return stats_; }
  const DrawOptions& options() const noexcept { return options_; }

 private:
  struct Gpu;
  static bool record_thunk(const renderer::FramePassContext& context, void* user,
                           std::string* error);
  bool record(const renderer::FramePassContext& context, std::string* error);

  renderer::SceneRenderer* renderer_ = nullptr;
  std::unique_ptr<Gpu> gpu_;
  DrawOptions options_;
  const SceneCollision* collision_ = nullptr;
  CollisionView* view_ = nullptr;
  WorldPos centre_{};
  bool has_capsule_ = false;
  DrawCapsule capsule_{};
  Vector<DrawLine> lines_;
  DrawStats stats_;
  // Over the draw's life, logged at destroy.
  u64 frames_ = 0;
  i64 build_ns_ = 0;
  u32 lines_max_ = 0;
};

}  // namespace engine::scene_collision
