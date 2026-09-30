#pragma once

// The sky in the renderer (docs/subsystems/renderer.md, "The sky"; ADR-0048). A scene that names a
// sky (`engine.scene.Sky`) is drawn under a physical atmosphere — the sky from the eye, the air
// between the eye and every surface, the sun's and the moon's light through it, an ambient term
// from the sky's own hemisphere, twilight, a moon with its phase and its light, stars — and exposed
// by a declared rule; a scene that names none draws exactly what it drew before, the stand-in sky
// and sun (lighting.h).
//
// **The split.** The *model* — the air's numbers, where the sun and the moon are, which stars there
// are — is a sky provider's (scene_gen/sky.h), a capability's content the renderer names nowhere:
// `SkyPass::create` looks the scene's provider up in the registry by name. What is here is drawing:
// the tables' buffers and passes (domain/gfx/shaders/sky_luts.slang), the frame's block
// (`gfx::SkyParams`), the star table's cells, and the exposure's tunables.
//
// **One clock** (renderer.md, "One clock"): the sky is evaluated at the time the ground's surface
// stands at — the scene's `Terrain.time`, or a moving terrain's surface time as its time-lapse runs
// (`GpuScene::ground_time_s`) — plus the frame's `FrameDesc::sun_time_s`, the host's own offset
// (its
// `--sun-rate` and `--time-of-day`). The ground's wind reads the same time, and the moon's month
// runs on it, so the sun's hour, the wind's day and the moon's phase are one clock's.

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/gfx/pipeline.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/rhi.h>
#include <domain/gfx/shader_library.h>
#include <domain/gfx/sky.h>
#include <domain/scene_gen/sky.h>

#include <span>
#include <string>

namespace engine::renderer {

struct SceneData;
struct Camera;
class ViewSet;

// What a frame asks of the exposure beyond the scene's own rule (engine-view's `-`, `=` and `0`):
// stops added to the rule's, or a fixed exposure value that overrides it.
struct ExposureRequest {
  f32 compensation_ev = 0.0f;
  bool fixed = false;
  f32 ev100 = 15.0f;
};

// The frame's sky on the CPU: what the provider says at the frame's game time, and what the
// renderer decided from it.
struct FrameSky {
  bool active = false;
  f64 time_s = 0.0;  // the game time it was evaluated at
  scene_gen::SkyState state;
  bool moon = false;   // the scene's moon is on
  bool stars = false;  // and its stars
  // The directional light the cascaded shadow maps follow: the moon when the sun is below the
  // horizon and the moon above it, the sun otherwise.
  bool moon_key = false;
};

// The exposure's tunables (renderer.md, "Exposure"): stops added to every sky's exposure, and the
// rule's knee and slope below it. Read once a frame.
f64 sky_exposure_compensation_tunable();
f64 sky_exposure_knee_tunable();
f64 sky_exposure_slope_tunable();

// The star table's cell of a celestial direction: sky.slang's `sky_star_cell`, the same arithmetic.
u32 sky_star_cell(Vec3 celestial) noexcept;
// Each cell's list of the stars whose spot, out to `gfx::k_sky_star_reach`, can reach it:
// `cells` is `gfx::k_sky_star_cells + 1` offsets into `index`.
void build_star_cells(std::span<const scene_gen::Star> stars, Vector<u32>& cells,
                      Vector<u32>& index);
// A star's rgb irradiance in sun units: its magnitude's (the sun's is -26.74) times its colour.
Vec3 star_irradiance(const scene_gen::Star& star) noexcept;

class SkyPass {
 public:
  SkyPass() noexcept = default;
  ~SkyPass() { destroy(); }
  SkyPass(const SkyPass&) = delete;
  SkyPass& operator=(const SkyPass&) = delete;

  // Makes the scene's sky provider and the tables' buffers and pipelines, and uploads the star
  // table. Nothing for a scene with no sky (`active()` stays false). False, with `error`, when the
  // provider is not in this executable or refuses the entry.
  bool create(const gfx::Device& device, const SceneData& scene, gfx::ShaderLibrary& shaders,
              std::string* error);
  void destroy() noexcept;
  bool active() const noexcept { return device_ != nullptr; }

  // The provider at game time `time_s`.
  void evaluate(f64 time_s, FrameSky& out) const noexcept;
  // The frame's block: the air, the lights, the eye, the views, the exposure and the tables'
  // addresses.
  void fill(const FrameSky& sky, const ViewSet& views, const Camera& camera,
            const ExposureRequest& exposure, gfx::SkyParams& out) const noexcept;

  // Whether the scene's two tables (transmittance, Psi_ms) have been built; the frame that builds
  // them says so.
  bool tables_built() const noexcept { return tables_built_; }
  void set_tables_built() noexcept { tables_built_ = true; }

  // The buffers the passes write and the resolve and the reference read.
  gfx::BufferResource transmittance;
  gfx::BufferResource multiscatter;
  gfx::BufferResource sky_view;
  gfx::BufferResource aerial;
  gfx::BufferResource frame;  // gfx::SkyFrame
  gfx::BufferResource star_list;
  gfx::BufferResource star_cells;
  gfx::BufferResource star_index;
  // sky_luts.slang's five entry points.
  gfx::ComputePipeline transmittance_pipeline;
  gfx::ComputePipeline multiscatter_pipeline;
  gfx::ComputePipeline view_pipeline;
  gfx::ComputePipeline aerial_pipeline;
  gfx::ComputePipeline frame_pipeline;

  u32 star_count() const noexcept { return star_count_; }
  const scene::Sky& entry() const noexcept { return entry_; }

 private:
  const gfx::Device* device_ = nullptr;
  scene_gen::SkyProvider provider_;
  scene::Sky entry_;
  Vec3 ground_albedo_{};
  u32 star_count_ = 0;
  bool tables_built_ = false;
};

}  // namespace engine::renderer
