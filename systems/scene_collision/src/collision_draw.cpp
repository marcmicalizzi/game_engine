#include <core/log/log.h>
#include <core/time/time.h>
#include <domain/gfx/device.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/shader_library.h>
#include <domain/sim/tiers.h>
#include <systems/renderer/frame_pass.h>
#include <systems/renderer/scene_renderer.h>
#include <systems/renderer/view_set.h>
#include <systems/scene_collision/collision_draw.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <shaders/collision_lines.spv.h>

namespace engine::scene_collision {

namespace {

ENGINE_LOG_CATEGORY_DEFINE(log_collision_draw, "scene_collision.draw");

static_assert(sizeof(DrawLine) == 32, "mirrored by `Line` in collision_lines.slang");

// One view's block, mirrored by `ViewBlock` in collision_lines.slang.
struct ViewBlock {
  Mat4 view_proj;
  u32 rect[4] = {};
  u32 source[4] = {};  // the visibility region's width and height, the colour's slot, the lines
  u64 visibility = 0;
  u64 lines = 0;
  f32 depth_tolerance = 0.0f;
  u32 max_steps = 0;
  u32 pad[2] = {};
};
static_assert(sizeof(ViewBlock) == 128, "mirrored by `ViewBlock` in collision_lines.slang");

// The most pixels one line walks in a view: a line across a 4K view's diagonal is 4,406.
constexpr u32 k_max_steps = 8192;

constexpr gfx::BufferUsage k_address =
    gfx::BufferUsage::Storage | gfx::BufferUsage::ShaderDeviceAddress;

u32 groups(u32 n, u32 size) { return (n + size - 1) / size; }

u32 pack(f32 r, f32 g, f32 b) noexcept {
  const auto byte = [](f32 v) {
    return static_cast<u32>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f);
  };
  return byte(r) | (byte(g) << 8) | (byte(b) << 16) | (0xFFu << 24);
}

// Metres from the eye, the difference taken in f64 (ADR-0053).
Vec3 from_eye(WorldPos p, WorldPos eye) noexcept {
  return Vec3{static_cast<f32>(p.x - eye.x), static_cast<f32>(p.y - eye.y),
              static_cast<f32>(p.z - eye.z)};
}

void push_line(Vector<DrawLine>& out, Vec3 a, Vec3 b, u32 color) {
  DrawLine line;
  line.a = a;
  line.b = b;
  line.color = color;
  out.push_back(line);
}

// The lattice's segments that touch no hole, counted or emitted.
u32 ground_lines(const SceneCollision& collision, const HeldTile& tile, WorldPos eye, u32 color,
                 Vector<DrawLine>* out) {
  if (tile.n < 2) return 0;
  const u32 n = tile.n;
  u32 count = 0;
  const auto point = [&](u32 i, u32 j) {
    const WorldPos p{static_cast<f64>(tile.i0 + i) * tile.spacing_m,
                     static_cast<f64>(tile.heights[j * n + i]),
                     static_cast<f64>(tile.j0 + j) * tile.spacing_m};
    return from_eye(p, eye);
  };
  const auto hole = [&](u32 i, u32 j) {
    return collision.lattice_hole(tile.i0 + static_cast<i64>(i), tile.j0 + static_cast<i64>(j));
  };
  for (u32 j = 0; j < n; ++j) {
    for (u32 i = 0; i < n; ++i) {
      const bool here = hole(i, j);
      if (i + 1 < n && !here && !hole(i + 1, j)) {
        ++count;
        if (out != nullptr) push_line(*out, point(i, j), point(i + 1, j), color);
      }
      if (j + 1 < n && !here && !hole(i, j + 1)) {
        ++count;
        if (out != nullptr) push_line(*out, point(i, j), point(i, j + 1), color);
      }
    }
  }
  return count;
}

u32 piece_lines(const SceneCollision& collision, u32 tile_index, const HeldTile& tile) {
  u32 count = 0;
  for (u32 p = 0; p < tile.pieces; ++p) {
    HeldPiece piece;
    if (collision.held_piece(tile_index, p, piece))
      count += 3 * static_cast<u32>(piece.indices.size() / 3);
  }
  return count;
}

void emit_pieces(const SceneCollision& collision, u32 tile_index, const HeldTile& tile,
                 WorldPos eye, u32 color, Vector<DrawLine>& out) {
  const Vec3 corner = from_eye(tile.corner, eye);
  for (u32 p = 0; p < tile.pieces; ++p) {
    HeldPiece piece;
    if (!collision.held_piece(tile_index, p, piece)) continue;
    const Vec3 base = corner + piece.offset;
    for (u32 t = 0; t + 2 < piece.indices.size(); t += 3) {
      Vec3 c[3];
      for (u32 k = 0; k < 3; ++k)
        c[k] = base + rotate(piece.rotation, piece.vertices[piece.indices[t + k]]);
      push_line(out, c[0], c[1], color);
      push_line(out, c[1], c[2], color);
      push_line(out, c[2], c[0], color);
    }
  }
}

void emit_capsule(const DrawCapsule& capsule, WorldPos eye, Vector<DrawLine>& out) {
  const Vec3 feet = from_eye(capsule.feet, eye);
  const f32 r = capsule.radius;
  const f32 low = r;
  const f32 high = std::max(capsule.height - r, r);
  constexpr u32 k = k_capsule_ring_segments;
  const auto ring = [&](f32 y, u32 i) {
    const f32 a = k_two_pi * static_cast<f32>(i) / static_cast<f32>(k);
    return feet + Vec3{r * std::cos(a), y, r * std::sin(a)};
  };
  for (u32 i = 0; i < k; ++i) {
    push_line(out, ring(low, i), ring(low, (i + 1) % k), k_capsule_color);
    push_line(out, ring(high, i), ring(high, (i + 1) % k), k_capsule_color);
  }
  for (u32 i = 0; i < 4; ++i) {
    push_line(out, ring(low, i * k / 4), ring(high, i * k / 4), k_capsule_color);
  }
  // The round ends: a half circle under the bottom ring and over the top one, in two planes.
  for (u32 plane = 0; plane < 2; ++plane) {
    const Vec3 across = plane == 0 ? Vec3{1.0f, 0.0f, 0.0f} : Vec3{0.0f, 0.0f, 1.0f};
    for (u32 end = 0; end < 2; ++end) {
      const f32 y = end == 0 ? low : high;
      const f32 sign = end == 0 ? -1.0f : 1.0f;
      const auto arc = [&](u32 i) {
        const f32 a = 0.5f * k_two_pi * static_cast<f32>(i) / static_cast<f32>(k / 2);
        return feet + across * (r * std::cos(a)) + Vec3{0.0f, y + sign * r * std::sin(a), 0.0f};
      };
      for (u32 i = 0; i < k / 2; ++i)
        push_line(out, arc(i), arc(i + 1), k_capsule_color);
    }
  }
}

}  // namespace

u32 body_color(world::TileCoord coord, bool walker) noexcept {
  // A hue from the tile's coordinates (a golden-ratio walk, so neighbours differ), at a saturation
  // that reads over foliage and bark, and full value for the walker's own tile.
  const u32 h = static_cast<u32>(coord.x) * 0x9E3779B1u ^ static_cast<u32>(coord.z) * 0x85EBCA77u;
  const f32 hue = static_cast<f32>(h >> 8) / 16777216.0f * 6.0f;
  const f32 v = walker ? 1.0f : 0.55f;
  const f32 s = 0.85f;
  const f32 c = v * s;
  const f32 x = c * (1.0f - std::fabs(std::fmod(hue, 2.0f) - 1.0f));
  const f32 m = v - c;
  f32 r = 0.0f, g = 0.0f, b = 0.0f;
  if (hue < 1.0f) {
    r = c;
    g = x;
  } else if (hue < 2.0f) {
    r = x;
    g = c;
  } else if (hue < 3.0f) {
    g = c;
    b = x;
  } else if (hue < 4.0f) {
    g = x;
    b = c;
  } else if (hue < 5.0f) {
    r = x;
    b = c;
  } else {
    r = c;
    b = x;
  }
  return pack(r + m, g + m, b + m);
}

void collision_lines(const SceneCollision& collision, WorldPos eye, WorldPos centre,
                     const DrawCapsule* capsule, const DrawOptions& options, Vector<DrawLine>& out,
                     DrawStats& stats) {
  const i64 start = time::monotonic_ns();
  stats = DrawStats{};
  out.clear();
  if (capsule != nullptr) {
    emit_capsule(*capsule, eye, out);
    stats.capsule_lines = out.size();
  }
  const f64 tile = static_cast<f64>(collision.config().tile_size);
  const u32 held = collision.held_tiles();
  stats.tiles_held = held;
  // The tiles within the radius, nearest centre first; a tile index and its distance, in a fixed
  // array: a ring holds a few dozen tiles at most.
  struct Near {
    f64 distance;
    u32 index;
  };
  constexpr u32 k_max_near = 1024;
  Near nearest[k_max_near];
  u32 count = 0;
  for (u32 t = 0; t < held && count < k_max_near; ++t) {
    HeldTile ht;
    if (!collision.held_tile(t, ht)) continue;
    const f64 x0 = ht.corner.x;
    const f64 z0 = ht.corner.z;
    const f64 dx = std::max({x0 - centre.x, 0.0, centre.x - (x0 + tile)});
    const f64 dz = std::max({z0 - centre.z, 0.0, centre.z - (z0 + tile)});
    if (options.radius_m > 0.0f &&
        std::sqrt(dx * dx + dz * dz) > static_cast<f64>(options.radius_m))
      continue;
    const f64 cx = x0 + 0.5 * tile - centre.x;
    const f64 cz = z0 + 0.5 * tile - centre.z;
    nearest[count++] = Near{std::sqrt(cx * cx + cz * cz), t};
  }
  std::sort(nearest, nearest + count, [](const Near& a, const Near& b) {
    return a.distance != b.distance ? a.distance < b.distance : a.index < b.index;
  });
  bool dropping = false;
  for (u32 k = 0; k < count; ++k) {
    HeldTile ht;
    (void)collision.held_tile(nearest[k].index, ht);
    const bool walker = centre.x >= ht.corner.x && centre.x < ht.corner.x + tile &&
                        centre.z >= ht.corner.z && centre.z < ht.corner.z + tile;
    const u32 ground = ground_lines(collision, ht, eye, 0, nullptr);
    const u32 pieces = piece_lines(collision, nearest[k].index, ht);
    if (dropping || static_cast<u64>(out.size()) + ground + pieces > options.max_lines) {
      dropping = true;
      ++stats.tiles_dropped;
      stats.dropped_lines += u64{ground} + pieces;
      continue;
    }
    (void)ground_lines(collision, ht, eye, walker ? k_ground_color_walker : k_ground_color, &out);
    emit_pieces(collision, nearest[k].index, ht, eye, body_color(ht.coord, walker), out);
    stats.ground_lines += ground;
    stats.proxy_lines += pieces;
    ++stats.tiles_drawn;
  }
  stats.lines = out.size();
  stats.build_ns = time::monotonic_ns() - start;
}

// ---- a consumer of its own
// -----------------------------------------------------------------------

struct CollisionView::Impl {
  // The world first: the consumer's bodies and shapes are released before it goes.
  physics::World world;
  world::World ring;
  SceneCollision consumer;
  sim::ObserverSet observers;
  u64 tick = 0;
};

CollisionView::CollisionView() = default;
CollisionView::~CollisionView() = default;

bool CollisionView::create(const renderer::SceneData& scene,
                           const scene_gen::GroundProvider* ground, Config config, f32 radius_tiles,
                           std::string* error, const scene_gen::TileSource* tiles) {
  auto impl = std::make_unique<Impl>();
  physics::WorldOptions options;
  options.max_bodies = 4096;
  options.max_body_pairs = 16384;
  options.max_contact_constraints = 8192;
  if (impl->world.init(options) != physics::Status::Ok) {
    if (error != nullptr) *error = "the physics world could not be made";
    return false;
  }
  world::RingParams ring;
  ring.tile_size = config.tile_size;
  ring.ring_count = 1;
  for (f32& r : ring.radius)
    r = 0.0f;
  ring.radius[0] = radius_tiles;
  const char* ring_error = nullptr;
  if (!impl->ring.configure(ring, &ring_error)) {
    if (error != nullptr) *error = std::string("the collision ring: ") + ring_error;
    return false;
  }
  config.keep_shapes = true;
  config.prefetch_radius_tiles = 0.0f;
  if (!impl->consumer.create(impl->world, scene, ground, config, error, tiles)) return false;
  impl->ring.add_consumer(impl->consumer.consumer());
  impl_ = std::move(impl);
  return true;
}

void CollisionView::update(WorldPos at) {
  if (impl_ == nullptr) return;
  impl_->observers.clear();
  impl_->observers.add(at, 1.0f);
  impl_->ring.update(impl_->observers, impl_->tick++, true);
}

const SceneCollision& CollisionView::collision() const noexcept { return impl_->consumer; }

// ---- the draw
// ------------------------------------------------------------------------------------

struct CollisionDraw::Gpu {
  const gfx::Device* device = nullptr;
  gfx::ComputePipeline pipeline;
  Vector<gfx::BufferResource> lines;   // one a frame slot, host-visible
  Vector<gfx::BufferResource> blocks;  // one a frame slot: a `ViewBlock` a view
  u64 capacity = 0;                    // lines each of `lines` holds
  u64 push[renderer::k_max_views] = {};
};

CollisionDraw::CollisionDraw() = default;
CollisionDraw::~CollisionDraw() { destroy(); }

bool CollisionDraw::create(const gfx::Device& device, renderer::SceneRenderer& renderer,
                           const DrawOptions& options, std::string* error) {
  destroy();
  options_ = options;
  gpu_ = std::make_unique<Gpu>();
  gpu_->device = &device;
  gfx::ShaderLibrary& shaders = renderer.shaders();
  shaders.add_embedded("collision_lines", shaders::k_collision_lines_spirv,
                       shaders::k_collision_lines_spirv_size);
  const gfx::Shader* shader = shaders.get("collision_lines", error);
  if (shader == nullptr) {
    destroy();
    return false;
  }
  const gfx::DescriptorSetLayoutHandle set_layout = renderer.scene().bindless().layout();
  const std::span<const gfx::DescriptorSetLayoutHandle> sets(&set_layout, 1);
  if (!gfx::create_compute_pipeline(device, shader->module, "collision_lines_main", sets,
                                    sizeof(u64), gpu_->pipeline, error)) {
    destroy();
    return false;
  }
  if (!renderer.add_frame_pass(renderer::FramePassStage::AfterResolve, &CollisionDraw::record_thunk,
                               this, "collision", error)) {
    destroy();
    return false;
  }
  renderer_ = &renderer;
  return true;
}

void CollisionDraw::destroy() noexcept {
  if (frames_ > 0) {
    // What the draw cost over its life, on the CPU: the host's log has it (scene_collision.md,
    // "Seeing the collision").
    ENGINE_LOG_INFO(log_collision_draw, "collision draw", log::field("frames", frames_),
                    log::field("lines_last", stats_.lines), log::field("lines_max", lines_max_),
                    log::field("tiles_last", stats_.tiles_drawn),
                    log::field("dropped_last", stats_.tiles_dropped),
                    log::field("build_ms_mean",
                               static_cast<f64>(build_ns_) / 1.0e6 / static_cast<f64>(frames_)));
    frames_ = 0;
  }
  if (renderer_ != nullptr) {
    renderer_->remove_frame_pass(&CollisionDraw::record_thunk, this);  // waits for the device
    renderer_ = nullptr;
  }
  if (gpu_ != nullptr && gpu_->device != nullptr) {
    gfx::destroy_compute_pipeline(*gpu_->device, gpu_->pipeline);
    for (gfx::BufferResource& b : gpu_->lines)
      gfx::destroy_buffer(*gpu_->device, b);
    for (gfx::BufferResource& b : gpu_->blocks)
      gfx::destroy_buffer(*gpu_->device, b);
  }
  gpu_.reset();
  collision_ = nullptr;
  view_ = nullptr;
}

void CollisionDraw::set(const SceneCollision* collision, WorldPos centre,
                        const DrawCapsule* capsule) {
  collision_ = collision;
  centre_ = centre;
  has_capsule_ = capsule != nullptr;
  if (capsule != nullptr) capsule_ = *capsule;
}

bool CollisionDraw::record_thunk(const renderer::FramePassContext& context, void* user,
                                 std::string* error) {
  return static_cast<CollisionDraw*>(user)->record(context, error);
}

bool CollisionDraw::record(const renderer::FramePassContext& c, std::string* error) {
  if (view_ != nullptr && view_->valid()) {
    const WorldPos at = has_capsule_ ? capsule_.feet : c.eye_world;
    view_->update(at);
    collision_ = &view_->collision();
    centre_ = at;
  }
  if (collision_ == nullptr || gpu_ == nullptr) return true;
  const u32 dropped_before = stats_.tiles_dropped;
  collision_lines(*collision_, c.eye_world, centre_, has_capsule_ ? &capsule_ : nullptr, options_,
                  lines_, stats_);
  if (stats_.tiles_dropped != dropped_before) {
    ENGINE_LOG_INFO(log_collision_draw, "the collision draw's budget dropped tiles",
                    log::field("dropped", stats_.tiles_dropped),
                    log::field("dropped_lines", stats_.dropped_lines),
                    log::field("max_lines", options_.max_lines));
  }
  ++frames_;
  build_ns_ += stats_.build_ns;
  lines_max_ = std::max(lines_max_, stats_.lines);
  if (lines_.empty() || c.view_count == 0) return true;
  Gpu& g = *gpu_;
  const gfx::Device& device = *g.device;
  // A frame slot's buffers: grown, never shrunk, to the next power of two of the lines a frame
  // made, so a steady frame makes none.
  const u64 want = lines_.size();
  if (g.lines.size() != c.frames_in_flight || g.capacity < want) {
    u64 capacity = 1024;
    while (capacity < want)
      capacity *= 2;
    for (gfx::BufferResource& b : g.lines)
      gfx::destroy_buffer(device, b);
    g.lines.clear();
    g.lines.resize(c.frames_in_flight);
    for (gfx::BufferResource& b : g.lines) {
      if (!gfx::create_buffer(device, capacity * sizeof(DrawLine), k_address, true, b, error))
        return false;
    }
    g.capacity = capacity;
  }
  if (g.blocks.size() != c.frames_in_flight) {
    for (gfx::BufferResource& b : g.blocks)
      gfx::destroy_buffer(device, b);
    g.blocks.clear();
    g.blocks.resize(c.frames_in_flight);
    for (gfx::BufferResource& b : g.blocks) {
      if (!gfx::create_buffer(device, sizeof(ViewBlock) * renderer::k_max_views, k_address, true, b,
                              error))
        return false;
    }
  }
  std::memcpy(g.lines[c.slot].mapped, lines_.data(), lines_.size() * sizeof(DrawLine));
  u8* blocks = static_cast<u8*>(g.blocks[c.slot].mapped);
  bool skip[renderer::k_max_views] = {};
  const u32 views = std::min(c.view_count, renderer::k_max_views);
  for (u32 v = 0; v < views; ++v) {
    const renderer::FramePassView& view = c.views[v];
    // A resampled view's region is not its picture (frame_pass.h, `panini`).
    skip[v] = view.panini || view.source_width != view.width || view.source_height != view.height;
    ViewBlock b;
    b.view_proj = view.view_proj;
    b.rect[0] = view.x;
    b.rect[1] = view.y;
    b.rect[2] = view.width;
    b.rect[3] = view.height;
    b.source[0] = view.source_width;
    b.source[1] = view.source_height;
    b.source[2] = c.color_storage;
    b.source[3] = lines_.size();
    b.visibility = view.visibility;
    b.lines = g.lines[c.slot].address;
    b.depth_tolerance = options_.depth_tolerance;
    b.max_steps = k_max_steps;
    std::memcpy(blocks + sizeof(ViewBlock) * v, &b, sizeof(b));
    g.push[v] = g.blocks[c.slot].address + sizeof(ViewBlock) * v;
  }
  gfx::RenderGraph& graph = *c.graph;
  const gfx::RgBuffer visibility = c.visibility;
  const gfx::RgImage color = c.color;
  gfx::GpuTimer* timer = c.timer;
  gfx::BindlessSet* bindless = c.bindless;
  Gpu* gpu = gpu_.get();
  const u32 line_groups = groups(lines_.size(), 64);
  graph.add_pass(
      "collision draw", gfx::PassKind::Compute,
      [&](gfx::PassBuilder& b) {
        b.read(visibility, gfx::Access::ComputeRead);
        b.write(color, gfx::Access::ComputeReadWrite);
      },
      [gpu, timer, bindless, views, skip, line_groups](gfx::CommandList cb, gfx::RenderGraph&) {
        timer->begin(cb, "collision");
        cb.bind_pipeline(gfx::BindPoint::Compute, gpu->pipeline.pipeline);
        bindless->bind(cb, gfx::BindPoint::Compute);
        for (u32 v = 0; v < views; ++v) {
          if (skip[v]) continue;
          cb.push_constants(gpu->pipeline.layout, gfx::ShaderStage::Compute, 0, sizeof(u64),
                            &gpu->push[v]);
          cb.dispatch(line_groups, 1, 1);
        }
        timer->end(cb);
      });
  return true;
}

}  // namespace engine::scene_collision
