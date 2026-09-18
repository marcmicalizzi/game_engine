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
// One renderer draws a whole `ViewSet` (systems/renderer/view_set.h, docs/plan/04-renderer.md
// §4.6): N views over one `GpuScene`, each with its own camera, projection, rectangle of the color
// target and quality tier, sharing the scene buffers, the acceleration structures and the
// residency budget. `Desc::views` picks the layout; a frame still names one camera, and the view
// set turns it into N.
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
#include <systems/renderer/view_set.h>

#include <span>
#include <string>

namespace engine::renderer {

// One view's share of a frame. The milliseconds are sums over `Stats::timed_frames`, like the
// totals beside them, and `Stats::view_ms` divides. The rectangle and the tier are copied out of
// the `ViewSet` so a summary can say which numbers belong to which monitor.
struct ViewStats {
  u32 x = 0;
  u32 y = 0;
  u32 width = 0;
  u32 height = 0;
  u32 source_width = 0;  // what the rasterizers ran at; wider than `width` for a Panini view
  u32 source_height = 0;
  f32 lod_scale = 1.0f;
  u32 shading_rate = 1;  // stubbed; see ViewQuality
  u32 visible_hw = 0;
  u32 visible_pass2 = 0;
  u32 visible_sw = 0;
  f64 gpu_cull = 0.0;
  f64 gpu_hw = 0.0;
  f64 gpu_sw = 0.0;
  f64 gpu_hiz = 0.0;
  f64 gpu_resolve = 0.0;
  f64 gpu_deform = 0.0;
  f64 gpu_trace = 0.0;

  u32 visible_pairs() const noexcept { return visible_hw + visible_pass2 + visible_sw; }
  // Everything this view cost on its own. The frame's acceleration structure chain is not in it:
  // it is built once from the union of the views' cuts and belongs to no single view.
  f64 gpu_sum() const noexcept {
    return gpu_cull + gpu_hw + gpu_sw + gpu_hiz + gpu_resolve + gpu_deform + gpu_trace;
  }
};

// Every number the JSON summary of a run reports, and every number a benchmark returns. The GPU
// milliseconds are sums over `timed_frames`; `*_ms()` divide.
// What the device's memory looked like when the run was measured, in mebibytes
// (`gfx::MemoryBudget`). `budget_mib` is what the Vulkan driver will let this process use, so
// `device_local_total_mib - budget_mib` is a **lower bound** on the share other processes hold —
// it under-reports a CUDA tenant, which is why the hosts print nvidia-smi's figure beside it in
// their machine_state block. Zero-valued and `valid == false` without VK_EXT_memory_budget.
struct GpuMemory {
  bool valid = false;
  u64 budget_mib = 0;
  u64 used_mib = 0;
  u64 device_local_total_mib = 0;
};

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
  // Sampled by sample_gpu_memory(), not by a frame: it is a driver query and the frame path
  // stays free of them.
  GpuMemory gpu_memory;
  // The per-view breakdown. `view_count` is 1 for a single view, and `views[0]` then holds the
  // same numbers the totals do.
  u32 view_count = 1;
  ViewStats views[k_max_views];

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
  // What one view cost a frame, everything but the shared acceleration structure chain.
  f64 view_ms(u32 view) const noexcept {
    return view < view_count ? views[view].gpu_sum() / timed() : 0.0;
  }
  f64 cpu_ms_per_frame() const noexcept {
    return frames > 0 ? cpu_ns / 1.0e6 / static_cast<f64>(frames) : 0.0;
  }
};

// One frame's inputs. `color` of a null image draws into the renderer's own offscreen target.
//
// **`joints` and `instance_joints` are the whole of the animation contract**, and they are plain
// spans on purpose: the renderer must not depend on `systems/animation`, on `domain/ecs`, or on
// flecs, so what crosses the boundary is data and not a system. The caller ticks its world at its
// own fixed step, takes `AnimationSystem::joint_matrices()` — one contiguous span of every
// animated instance's bone matrices — and says where each scene instance's run is. The renderer
// copies the span into this frame slot's region of the joint buffer with one memcpy and points
// each skinned instance's `gfx::DeformDesc::joints` at its run.
//
// An instance whose `InstanceJoints::count` is zero, or that the array does not reach, **draws
// its rest pose**: that is what an instance at animation LOD3 (no pool slot) looks like from
// here, and it is a deliberate choice over marking the instance rigid for the frame, which would
// mean rewriting the instance table — a per-instance upload in the middle of a frame — to save
// a pool write the cut already paid for.
struct FrameDesc {
  Camera camera;
  u64 frame_index = 0;      // drives the light orbit and the deformation phase, as engine-view does
  u32 view_mode = ~u32{0};  // override the settings' view mode; ~0 uses it
  // Override the settings' LOD pixel threshold for this frame; negative uses it. The reference
  // renderer passes 0 to make the frame's cut the *finest* clusters, so the acceleration
  // structures it then traces hold the source geometry rather than the frame's cut
  // (docs/plan/04-renderer.md §4.8). It is per frame and not per renderer because the same
  // renderer draws both pictures of a comparison.
  f32 lod_px = -1.0f;
  gfx::ImageResource color;  // a swapchain image, or null for the renderer's own target
  VkImageLayout final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkSemaphore wait = VK_NULL_HANDLE;          // the swapchain acquire, for a presented frame
  VkSemaphore signal = VK_NULL_HANDLE;        // the swapchain image's render-finished semaphore
  std::span<const anim::JointMatrix> joints;  // every animated instance's, in one span
  std::span<const InstanceJoints> instance_joints;  // parallel to the scene's instances
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
    // How the target is divided into views (04 §4.6). The default is one view over the whole of
    // it, which is what every caller before multi-view got. The layout has to agree with the one
    // `resolve_settings` saw, because the scene's per-frame working set is sized by the view
    // count; `create` checks and fails rather than overrunning a buffer.
    ViewSetDesc views;
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
  // The views the frame draws, laid out over the color target. Read for the rectangles, the
  // projections and the tiers; `submit_frame` updates the cameras from the frame's own.
  const ViewSet& views() const noexcept { return views_; }
  const Stats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept;
  // Reads the device's memory budget into `stats().gpu_memory`. A caller samples it around a
  // run — create() does it once so a summary always has a figure, and a host that measures
  // calls it again at the end, which is when the contention it reports actually matters.
  void sample_gpu_memory() noexcept;
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
  // One view's slice of the screen-sized buffers. Every view rasterizes into its own region of
  // one visibility buffer and builds its own Hi-Z pyramid in one pyramid buffer, both packed back
  // to back, so a view's passes address their region from element zero and every shader below the
  // resolve stays view-local — which is why the rasterizers and `hiz_build.slang` needed no
  // change at all for multi-view (04 §4.6).
  struct ViewTarget {
    u64 vis_offset = 0;  // first element of this view's region of the visibility buffer
    u32 width = 0;
    u32 height = 0;
    u32 hiz_mips = 0;
    u32 hiz_offsets[gfx::k_hiz_max_mips] = {};  // elements into the shared pyramid buffer
    u32 hiz_dispatches = 0;   // how many workgroup folds the pyramid takes, six mips at a time
    u32 level_base = 0;       // first of this view's blocks in `hiz_levels`
    u32 coverage_offset = 0;  // this view's per-tile mask, behind every view's pyramid
    u32 coverage_pitch = 0;   // tiles per row of it
  };
  // Screen-sized resources, recreated on resize.
  struct Targets {
    gfx::ImageResource depth;  // the direct path's depth buffer
    gfx::BufferResource vis;   // the visibility buffer: u64 per pixel, the views back to back
    gfx::BufferResource hiz;   // the Hi-Z pyramids of the farthest depth, the views back to back
    Vector<ViewTarget> views;
    u32 width = 0;  // the largest source rectangle: the raster passes' render area
    u32 height = 0;
    // 2 x dispatches per view: stable storage for the pass bodies, which capture by pointer.
    Vector<gfx::HizParams> hiz_levels;
    bool hiz_dirty = true;
    bool create(const gfx::Device& device, const ViewSet& views, std::string* error);
    void destroy(const gfx::Device& device) noexcept;
  };

  bool create_pipelines(std::string* error);
  bool create_color_target(std::string* error);
  // Declares every pass, compiles, and executes, all in one function: the render graph stores
  // pass bodies in an arena and requires them to capture by reference, so every parameter block
  // a body pushes has to still be alive when execute() records it.
  bool record_frame(const FrameDesc& frame, gfx::RgImage color_handle, std::string* error);
  void collect_slot(u32 slot);
  void fold_visible(u32 slot);
  void fill_view_layout() noexcept;
  bool read_visibility(CapturedFrame& out, const CaptureChannels& channels, std::string* error);
  u32 view_count() const noexcept { return views_.size(); }

  const gfx::Device* device_ = nullptr;
  GpuScene* scene_ = nullptr;
  ResolvedSettings resolved_;
  Desc desc_;
  ViewSet views_;
  gfx::ShaderLibrary shaders_;
  Pipelines pipelines_;
  Targets targets_;
  gfx::FrameContext frames_;
  gfx::GpuTimer timer_;
  gfx::RenderGraph* graph_ = nullptr;        // heap: RenderGraph is not default-constructible
  gfx::ImageResource color_;                 // the offscreen target, when the renderer owns one
  Vector<gfx::BufferResource> params_;       // two CullParams per view, per slot
  Vector<gfx::BufferResource> resolves_;     // one ResolveParams per view plus the lights, a slot
  Vector<gfx::BufferResource> stat_blocks_;  // host-visible copies of the argument blocks
  Vector<gfx::BufferResource> ray_params_;   // one RayVisibilityParams per view, per slot
  Stats stats_;
  VkCommandBuffer commands_ = VK_NULL_HANDLE;  // the frame between begin_frame and submit_frame
  u32 width_ = 0;
  u32 height_ = 0;
  u64 submitted_ = 0;  // frames submitted; also the slot-warmup counter
  u64 collected_ = 0;  // the timeline value whose statistics were last folded in
  bool flags_dirty_ = true;
  bool recording_ = false;
  bool joint_overflow_warned_ = false;  // a span longer than the scene was sized for, said once
};

}  // namespace engine::renderer
