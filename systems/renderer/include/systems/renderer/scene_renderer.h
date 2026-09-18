#pragma once

// The renderer (docs/subsystems/renderer.md, docs/plan/04-renderer.md). One `SceneRenderer`
// owns everything sized by the *screen* and everything that submits work: the shader library
// and the pipelines built from it, the visibility buffer and the Hi-Z pyramid, the frame
// context, the GPU timers, the render graph, and the per-slot parameter blocks. It renders one
// frame at a time into a color image the caller names, which may be a swapchain image
// (engine-view) or an offscreen target the renderer owns (engine-host, the tests, CI).
//
// **There is no window anywhere below this line.** `foundation/window` is not a dependency of
// this module, and the renderer never touches a surface, a swapchain, or a present queue:
// engine-view keeps all four and hands in the acquired image and its two semaphores.
//
//     SceneData data; load_scene(desc, data, error);
//     ResolvedSettings resolved; resolve_settings(settings, device.features(), &data, resolved);
//     GpuScene scene; scene.create(device, data, resolved, &error);
//     SceneRenderer renderer; renderer.create(device, scene, resolved, {w, h}, &error);
//     CapturedFrame frame;
//     renderer.capture(orbit_camera(data.center, data.radius, 22.0f, 0), 0, {.ids = true},
//                      frame, &error);

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <core/math/math.h>
#include <domain/gfx/frame.h>
#include <domain/gfx/gpu_timer.h>
#include <domain/gfx/ray_visibility.h>
#include <domain/gfx/render_graph.h>
#include <domain/gfx/shader_library.h>
#include <systems/renderer/capture.h>
#include <systems/renderer/gpu_scene.h>

#include <string>

namespace engine::renderer {

struct Camera {
  Vec3 position{};
  Vec3 target{};
  f32 fov_y = 0.9599310886f;  // radians(55)
  f32 znear = 0.1f;           // reversed-Z: there is no far plane
};

// The elevation `orbit_camera` holds above its orbit circle: atan(0.45), engine-view's fixed
// height factor. It is the default pitch of an explicit orbit, so `orbit {distance: 22}` and
// `--orbit 22` are the same camera and the two hosts' captures of a scene compare.
inline constexpr f32 k_orbit_pitch = 0.4228539f;

// The camera engine-view has always orbited with, so a capture of a scene from engine-host and
// a capture of the same scene from engine-view are the same picture. `distance` of zero
// breathes between 8 and 36 units instead of holding still, and every distance scales with the
// scene's radius, so a 2 cm mesh and a 20 m one are framed alike.
Camera orbit_camera(const Vec3& center, f32 radius, f32 distance, u64 frame) noexcept;
// The same orbit at explicit angles, which is what a caller that wants one picture asks for.
// `distance` of zero is the breathing orbit's rest distance (22 radius-tenths).
Camera orbit_camera_at(const Vec3& center, f32 radius, f32 distance, f32 yaw, f32 pitch) noexcept;

// Every number the JSON summary of a run reports, and every number a benchmark returns. The GPU
// milliseconds are sums over `timed_frames`; `*_ms()` divide.
struct Stats {
  u64 frames = 0;         // frames submitted
  u64 timed_frames = 0;   // frames whose timestamps came back
  u32 visible_hw = 0;     // the hardware pass's survivors, one frame late
  u32 visible_pass2 = 0;  // occlusion pass 2's
  u32 visible_sw = 0;     // the software rasterizer's
  u32 visible_min = ~u32{0};
  u32 visible_max = 0;
  f64 gpu_cull = 0.0;
  f64 gpu_hw = 0.0;
  f64 gpu_sw = 0.0;
  f64 gpu_hiz = 0.0;
  f64 gpu_resolve = 0.0;
  f64 gpu_rt = 0.0;  // records + ranges + emit + CLAS + cluster BLAS + TLAS
  f64 gpu_clas = 0.0;
  f64 gpu_deform = 0.0;
  f64 gpu_trace = 0.0;
  f64 gpu_total = 0.0;
  f64 cpu_ns = 0.0;  // wall time inside submit_frame, summed

  u32 visible_pairs() const noexcept { return visible_hw + visible_pass2 + visible_sw; }
  f64 timed() const noexcept { return timed_frames > 0 ? static_cast<f64>(timed_frames) : 1.0; }
  f64 cull_ms() const noexcept { return gpu_cull / timed(); }
  f64 hw_ms() const noexcept { return gpu_hw / timed(); }
  f64 sw_ms() const noexcept { return gpu_sw / timed(); }
  f64 hiz_ms() const noexcept { return gpu_hiz / timed(); }
  f64 resolve_ms() const noexcept { return gpu_resolve / timed(); }
  f64 rt_ms() const noexcept { return gpu_rt / timed(); }
  f64 clas_ms() const noexcept { return gpu_clas / timed(); }
  f64 deform_ms() const noexcept { return gpu_deform / timed(); }
  f64 trace_ms() const noexcept { return gpu_trace / timed(); }
  f64 total_ms() const noexcept { return gpu_total / timed(); }
  f64 cpu_ms_per_frame() const noexcept {
    return frames > 0 ? cpu_ns / 1.0e6 / static_cast<f64>(frames) : 0.0;
  }
};

// One frame's inputs. `color` of a null image draws into the renderer's own offscreen target.
struct FrameDesc {
  Camera camera;
  u64 frame_index = 0;      // drives the light orbit and the deformation phase, as engine-view does
  u32 view_mode = ~u32{0};  // override the settings' view mode; ~0 uses it
  gfx::ImageResource color;  // a swapchain image, or null for the renderer's own target
  VkImageLayout final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkSemaphore wait = VK_NULL_HANDLE;    // the swapchain acquire, for a presented frame
  VkSemaphore signal = VK_NULL_HANDLE;  // the swapchain image's render-finished semaphore
};

class SceneRenderer {
 public:
  struct Desc {
    u32 width = 1280;
    u32 height = 720;
    // The pipelines' color format. engine-view passes the swapchain's so that the picture is
    // produced exactly as it is presented; offscreen it is the default, which differs from a
    // typical swapchain's only in channel order and so reads back byte for byte the same.
    VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
    u32 frames_in_flight = 2;
    // Create an offscreen color target the renderer owns, for frames that name no image.
    bool offscreen = true;
    // A shader manifest to prefer over the embedded SPIR-V; empty looks for
    // `<exe dir>/../shaders/manifest.json` and uses the embedded bytes when there is none.
    std::string shader_manifest;
  };

  SceneRenderer() noexcept = default;
  ~SceneRenderer();
  ENGINE_NON_COPYABLE(SceneRenderer);

  // `scene` and `device` must outlive the renderer. False with `error` on a problem; check
  // `check_availability` first, because a device that cannot render at all is not an error the
  // caller should report as one.
  bool create(const gfx::Device& device, GpuScene& scene, const ResolvedSettings& resolved,
              const Desc& desc, std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return device_ != nullptr; }

  u32 width() const noexcept { return width_; }
  u32 height() const noexcept { return height_; }
  const Stats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept;
  const ResolvedSettings& settings() const noexcept { return resolved_; }
  gfx::ShaderLibrary& shaders() noexcept { return shaders_; }
  // The renderer's own color target; null when it was created without one.
  const gfx::ImageResource& color_target() const noexcept { return color_; }

  // Screen-sized resources for a new size. The GPU must be idle (the renderer waits).
  bool resize(u32 width, u32 height, std::string* error = nullptr);

  // ---- one frame ------------------------------------------------------------------------------
  // Waits for the slot to come free, folds the statistics and timings of the frame that last
  // used it into `stats()`, and makes `acquire_semaphore()` safe to hand to a swapchain.
  void begin_frame();
  VkSemaphore acquire_semaphore() const noexcept { return frames_.acquire_semaphore(); }
  // Records and submits the frame begun by begin_frame(). Returns the timeline value it will
  // signal, or 0 when the frame could not be recorded (`error` says why, and nothing was
  // submitted beyond an empty command buffer).
  u64 submit_frame(const FrameDesc& frame, std::string* error = nullptr);
  // Submits the frame begun by begin_frame() with nothing in it, for a caller whose swapchain
  // could not hand over an image. The slot's semaphores have to be consumed either way.
  void abort_frame();
  // Folds the visible-pair counts of the frame just submitted in, for a caller that waited for
  // it rather than letting the slot come around (engine-view's last frame, before it captures).
  // The GPU timings of that frame are not in yet — a timestamp pool is read when its slot comes
  // around — so this reports what the cull pass counted and nothing else.
  void collect_visible();
  bool wait(u64 value) const noexcept { return frames_.wait(value); }
  void wait_idle() const noexcept { frames_.wait_idle(); }
  u64 completed() const noexcept { return frames_.completed(); }

  // begin_frame, submit_frame into the renderer's own target, and wait. The whole offscreen
  // path in one call, which is what engine-host and the tests use.
  bool render_offscreen(const FrameDesc& frame, std::string* error = nullptr);

  // Renders and reads back every channel of `channels`. `normals` costs a second frame, because
  // the normal is what the resolve reconstructs and only the resolve knows it; `ids` and
  // `depth` come out of the visibility buffer of the first, which `--raster direct` does not
  // write — asking for either there is an error rather than a buffer of nothing.
  bool capture(const FrameDesc& frame, const CaptureChannels& channels, CapturedFrame& out,
               std::string* error = nullptr);

  // Recompiles every shader whose source changed and rebuilds the pipelines when any did,
  // naming the ones that changed. False only when the pipelines could not be rebuilt, which is
  // fatal for the caller; a shader that does not compile is not a failure — it keeps its last
  // good version and its diagnostics land in `error`.
  bool poll_shaders(Vector<std::string>& changed, std::string* error = nullptr);

 private:
  struct Pipelines {
    VkPipeline direct = VK_NULL_HANDLE;
    VkPipeline hardware = VK_NULL_HANDLE;
    VkPipeline vertex = VK_NULL_HANDLE;
    VkPipeline resolve = VK_NULL_HANDLE;
    gfx::ComputePipeline software;
    gfx::ComputePipeline cull;
    gfx::ComputePipeline deform;
    gfx::ComputePipeline hiz;
    gfx::ComputePipeline records;
    gfx::ComputePipeline record_ranges;
    gfx::ComputePipeline record_emit;
    gfx::ComputePipeline trace;
    void destroy(const gfx::Device& device) noexcept;
  };
  // Screen-sized resources, recreated on resize.
  struct Targets {
    gfx::ImageResource depth;  // the direct path's depth buffer
    gfx::BufferResource vis;   // the visibility buffer: u64 per pixel
    gfx::BufferResource hiz;   // the Hi-Z pyramid of the farthest depth
    u32 width = 0;
    u32 height = 0;
    u32 hiz_mips = 0;
    u32 hiz_offsets[gfx::k_hiz_max_mips] = {};
    Vector<gfx::HizParams> hiz_levels;  // 2 x hiz_mips: stable storage for the pass bodies
    bool hiz_dirty = true;
    bool create(const gfx::Device& device, u32 w, u32 h, std::string* error);
    void destroy(const gfx::Device& device) noexcept;
  };

  bool create_pipelines(std::string* error);
  bool create_color_target(std::string* error);
  // Declares every pass, compiles, and executes, all in one function: the render graph stores
  // pass bodies in an arena and requires them to capture by reference, so every parameter block
  // a body pushes has to still be alive when execute() records it.
  bool record_frame(const FrameDesc& frame, gfx::RgImage color_handle, std::string* error);
  void collect_slot(u32 slot);
  bool read_visibility(CapturedFrame& out, const CaptureChannels& channels, std::string* error);

  const gfx::Device* device_ = nullptr;
  GpuScene* scene_ = nullptr;
  ResolvedSettings resolved_;
  Desc desc_;
  gfx::ShaderLibrary shaders_;
  Pipelines pipelines_;
  Targets targets_;
  gfx::FrameContext frames_;
  gfx::GpuTimer timer_;
  gfx::RenderGraph* graph_ = nullptr;        // heap: RenderGraph is not default-constructible
  gfx::ImageResource color_;                 // the offscreen target, when the renderer owns one
  Vector<gfx::BufferResource> params_;       // two CullParams per slot
  Vector<gfx::BufferResource> resolves_;     // ResolveParams plus the lights, per slot
  Vector<gfx::BufferResource> stat_blocks_;  // host-visible copies of the argument blocks
  Vector<gfx::BufferResource> ray_params_;   // RayVisibilityParams per slot
  Stats stats_;
  VkCommandBuffer commands_ = VK_NULL_HANDLE;  // the frame between begin_frame and submit_frame
  u32 width_ = 0;
  u32 height_ = 0;
  u64 submitted_ = 0;  // frames submitted; also the slot-warmup counter
  u64 collected_ = 0;  // the timeline value whose statistics were last folded in
  bool flags_dirty_ = true;
  bool recording_ = false;
};

}  // namespace engine::renderer
