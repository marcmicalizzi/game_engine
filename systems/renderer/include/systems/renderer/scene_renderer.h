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
#include <systems/renderer/rt_capacity.h>
#include <systems/renderer/shadow_cascades.h>
#include <systems/renderer/streaming.h>
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
  u32 shadow_casters = 0;   // see Stats::shadow_casters
  u32 triangles_hw = 0;     // see Stats::triangles_hw
  u32 vertex_fallback = 0;  // see Stats::vertex_fallback
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

// One frame's numbers, as the renderer folds them in when that frame's slot comes around again
// (`Stats::last`). The sums beside it answer "what does this scene cost on average"; a flythrough
// asks "what did frame 1,412 cost", which a sum cannot answer and a CPU clock must not
// (docs/subsystems/renderer.md, "Scenes, camera paths and flythroughs").
//
// **Which frame it is** is carried rather than inferred, because the fold lags the submission by
// the frames in flight and a caller counting that lag itself would be wrong the first time the
// count changed: `frame` is the frame's own `FrameDesc::frame_index`, and `submission` is its
// place among the frames submitted since the last `reset_stats()`, from 0.
//
// The GPU milliseconds are that frame's zones from `gfx::GpuTimer`, summed over the views; the
// streaming numbers are what changed while it was the latest frame folded in: pages copied into
// the pool and their bytes, pages evicted, requests written, and the pages resident afterwards.
struct FrameStats {
  u64 frame = 0;
  u64 submission = 0;
  bool timed = false;  // the timestamps came back; the milliseconds are zero otherwise
  u32 visible_hw = 0;
  u32 visible_pass2 = 0;
  u32 visible_sw = 0;
  u32 shadow_casters = 0;     // Stats::shadow_casters: built into the structures, not drawn
  u32 shadow_pairs = 0;       // Stats::shadow_pairs: drawn into the cascaded maps
  f64 gpu_shadow = 0.0;       // the cascaded maps' passes, cull included
  f64 gpu_shadow_cull = 0.0;  // of which the light-view cull
  f64 gpu_cull = 0.0;
  f64 gpu_hw = 0.0;
  f64 gpu_sw = 0.0;
  f64 gpu_hiz = 0.0;
  f64 gpu_resolve = 0.0;
  f64 gpu_rt = 0.0;
  f64 gpu_clas = 0.0;
  f64 gpu_deform = 0.0;
  f64 gpu_deform_alloc = 0.0;
  f64 gpu_trace = 0.0;
  f64 gpu_total = 0.0;
  f64 gpu_blas = 0.0;  // the bottom-level build inside `gpu_rt`
  u32 uploads = 0;
  u64 upload_bytes = 0;
  u32 evictions = 0;
  u32 requests = 0;
  u32 pages_resident = 0;
  // Streaming's state after this frame, beside what it did in it: requests queued in the manager
  // and not admitted, page loads outstanding, and pages the **pool** holds, which lags the
  // manager's `pages_resident` by the upload budget and the read latency. A run whose pool stops
  // growing while its queue does not drain is the stall docs/subsystems/renderer.md
  // ("Admission never waits on a read it cannot start") describes.
  u32 pending = 0;
  u32 loads_in_flight = 0;
  u32 pool_pages = 0;
  // The ray tracing chain (Stats::rt_*): what this frame built, what it wanted, and the capacity
  // it was built into. `rt_wanted > rt_built` is a frame that dropped structures.
  u32 rt_built = 0;
  u32 rt_wanted = 0;
  u32 rt_capacity = 0;

  u32 visible_pairs() const noexcept { return visible_hw + visible_pass2 + visible_sw; }
};

// What the ray tracing chain held and did over a run (docs/subsystems/renderer.md, "The ray
// tracing chain's memory"). The per-frame structures are sized by the frames: `capacity` is what
// they hold room for now and `bytes` what the whole chain holds now, `peak_*` the most of either
// in the run, `limit` the budget in clusters. `built` and `wanted` are the last frame's, one frame
// late like the visible counts. `overflow_frames` counts frames that wanted more than the capacity
// held and so dropped whole instances' structures — `dropped_instances` of them lost their drawn
// clusters and `dropped_caster_instances` only their shadow casters, summed over those frames —
// which is zero on any run whose demand stays under the budget and moves by less than the headroom
// between two resizes.
struct RtStats {
  u32 capacity = 0;
  u32 limit = 0;
  u32 union_clusters = 0;  // what the chain would be sized for if it were sized by the scene
  u32 peak_capacity = 0;
  u64 bytes = 0;
  u64 peak_bytes = 0;
  u32 built = 0;
  u32 wanted = 0;
  u32 peak_wanted = 0;
  u32 grows = 0;
  u32 shrinks = 0;
  u64 overflow_frames = 0;
  u64 dropped_instances = 0;
  u64 dropped_caster_instances = 0;
};

struct Stats {
  u64 frames = 0;         // frames submitted
  u64 timed_frames = 0;   // frames whose timestamps came back
  u32 visible_hw = 0;     // the hardware pass's survivors, one frame late
  u32 visible_pass2 = 0;  // occlusion pass 2's
  u32 visible_sw = 0;     // the software rasterizer's
  // The pairs the cone test kept out of the picture and the frame built into its acceleration
  // structures anyway, so that they cast shadows (ResolvedSettings::casters). Not visible pairs:
  // nothing draws them, and `visible_pairs()` does not count them. One frame late, like the rest.
  u32 shadow_casters = 0;
  // The vertex path's indexed draw, last frame, both hardware passes and every view: the triangles
  // the cut's clusters hold, which is what it draws (three indices each), and how many of those
  // clusters the index budget had no room for, which the fallback drew instead at one vertex
  // invocation per corner of each one's whole capacity (gfx::VertexDrawHeader). Zero on every
  // other draw. One frame late.
  u32 triangles_hw = 0;
  u32 vertex_fallback = 0;
  // The sun's cascaded shadow maps, last frame (ResolvedSettings::csm): the (instance, cluster)
  // pairs every cascade's light-view cull drew into the atlas, summed over the cascades — a pair
  // in two cascades counts twice — and, on the vertex path's indexed draw, the cascade survivors
  // its index budget sent to the capacity draw. Not visible pairs. One frame late.
  u32 shadow_pairs = 0;
  u32 shadow_fallback = 0;
  u32 visible_min = ~u32{0};
  u32 visible_max = 0;
  // The deformed-vertex pool's suballocation, one frame late like the visible counts and read the
  // same way — out of a host-visible copy of a device record, never from inside a frame.
  // `deform_overflow_*` is what the budget refused: those (instance, cluster) pairs drew their
  // **rest pose** this frame. Nonzero here is the one number that says the pool is too small.
  u32 deform_vertices = 0;           // pool vertices the frame's cut used
  u32 deform_entries = 0;            // visible entries given a block
  u32 deform_overflow_entries = 0;   // entries the budget had no room for
  u32 deform_overflow_vertices = 0;  // the vertices they would have needed
  u32 deform_peak_vertices = 0;      // the largest `deform_vertices` of the run
  f64 gpu_cull = 0.0;
  f64 gpu_hw = 0.0;
  f64 gpu_sw = 0.0;
  f64 gpu_hiz = 0.0;
  f64 gpu_resolve = 0.0;
  f64 gpu_rt = 0.0;  // records + ranges + emit + CLAS + cluster BLAS + TLAS
  f64 gpu_clas = 0.0;
  f64 gpu_blas = 0.0;  // the one bottom-level build of every instance's structure
  f64 gpu_deform = 0.0;
  // The pool's suballocator, kept apart from the pool pass it feeds: one dispatch covers every
  // view of a run, so it is the frame's cost and no view's, and E25's "9.4 µs plus 0.9 µs per
  // 1,000 visible clusters" is a statement about the pool pass alone.
  f64 gpu_deform_alloc = 0.0;
  f64 gpu_trace = 0.0;
  // Every pass of the cascaded shadow maps: the light-view culls, the pool and index passes of
  // the cascades' cuts, and the depth raster. `gpu_shadow_cull` is the cull's share of it. The
  // filtering is in `gpu_resolve`, where it runs.
  f64 gpu_shadow = 0.0;
  f64 gpu_shadow_cull = 0.0;
  f64 gpu_total = 0.0;
  f64 cpu_ns = 0.0;  // wall time inside submit_frame, summed
  // Sampled by sample_gpu_memory(), not by a frame: it is a driver query and the frame path
  // stays free of them.
  GpuMemory gpu_memory;
  // Geometry residency, folded in with the rest when a frame slot comes around. Zero for a scene
  // that is uploaded whole (`stream.pages_total == 0` is what says so).
  StreamStats stream;
  // The ray tracing chain's memory and what it built. Zero when the frame builds no structures.
  RtStats rt;
  // The per-view breakdown. `view_count` is 1 for a single view, and `views[0]` then holds the
  // same numbers the totals do.
  u32 view_count = 1;
  ViewStats views[k_max_views];
  // The frame folded in most recently, and how many have been. A caller that records a run frame
  // by frame reads `last` after every `begin_frame()` whose fold moved `folded` on.
  FrameStats last;
  u64 folded = 0;

  u32 visible_pairs() const noexcept { return visible_hw + visible_pass2 + visible_sw; }
  f64 timed() const noexcept { return timed_frames > 0 ? static_cast<f64>(timed_frames) : 1.0; }
  f64 cull_ms() const noexcept { return gpu_cull / timed(); }
  f64 hw_ms() const noexcept { return gpu_hw / timed(); }
  f64 sw_ms() const noexcept { return gpu_sw / timed(); }
  f64 hiz_ms() const noexcept { return gpu_hiz / timed(); }
  f64 resolve_ms() const noexcept { return gpu_resolve / timed(); }
  f64 rt_ms() const noexcept { return gpu_rt / timed(); }
  f64 clas_ms() const noexcept { return gpu_clas / timed(); }
  f64 blas_ms() const noexcept { return gpu_blas / timed(); }
  f64 deform_ms() const noexcept { return gpu_deform / timed(); }
  f64 deform_alloc_ms() const noexcept { return gpu_deform_alloc / timed(); }
  f64 trace_ms() const noexcept { return gpu_trace / timed(); }
  f64 shadow_ms() const noexcept { return gpu_shadow / timed(); }
  f64 shadow_cull_ms() const noexcept { return gpu_shadow_cull / timed(); }
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
  // Under `render_offscreen`, build every ray tracing structure this frame wants even past the
  // budget, up to the scene's whole union (docs/subsystems/renderer.md, "The ray tracing chain's
  // memory"). The reference renderer sets it: its one real-time frame is the geometry every ray of
  // a converged picture traces, and at `lod_px = 0` that is every leaf in view, which no budget
  // sized for real-time cuts holds. The next ordinary frame shrinks the chain back to the budget.
  bool rt_complete = false;
  gfx::ImageResource color;  // a swapchain image, or null for the renderer's own target
  VkImageLayout final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  VkSemaphore wait = VK_NULL_HANDLE;          // the swapchain acquire, for a presented frame
  VkSemaphore signal = VK_NULL_HANDLE;        // the swapchain image's render-finished semaphore
  std::span<const anim::JointMatrix> joints;  // every animated instance's, in one span
  std::span<const InstanceJoints> instance_joints;  // parallel to the scene's instances
  // The **pose** morph weights, one per channel of the scene's morph channel array, on exactly
  // the same plain-span contract as the joint matrices: the caller samples a clip's weight tracks
  // (`anim::Clip::sample(time, pose, weights)`) and hands the array over; the renderer copies it
  // into this frame slot and points each deformed instance's pose weights at its mesh's run of
  // it. A span shorter than the scene's channels leaves the rest at zero, and an empty one leaves
  // every pose weight at zero — which is the rest shape, and what a frame that animates nothing
  // passes. The **static** weights are not here: they change rarely, they live on the scene, and
  // that is the whole difference between the two stages.
  std::span<const f32> morph_weights;
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
    // Where a streamed scene's page payloads come from. Null is the in-memory source: the pages
    // are copied out of the `SceneData`, which must still hold its streams. A source built by
    // `attach_page_source` reads them out of the meshes' `.clusters` containers instead, and must
    // outlive the renderer. Ignored for a scene that is not streamed.
    PageSource* page_source = nullptr;
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
  // The same set, with every projection brought up to `camera` first. A host that has to decide
  // something from **this** frame's frusta before the frame is recorded — which characters are
  // worth animating finely, say — needs them ahead of `submit_frame`, and reading them afterwards
  // would make that decision lag the camera by a frame, which is exactly what shows on a cut.
  // `submit_frame` updates the set again from its own camera, and `update` is a pure function of
  // the camera, so calling this with the camera the frame will carry costs one matrix build per
  // view and changes nothing about the frame.
  const ViewSet& update_views(const Camera& camera) noexcept {
    views_.update(camera);
    return views_;
  }
  const Stats& stats() const noexcept { return stats_; }
  void reset_stats() noexcept;
  // Reads the device's memory budget into `stats().gpu_memory`. A caller samples it around a
  // run — create() does it once so a summary always has a figure, and a host that measures
  // calls it again at the end, which is when the contention it reports actually matters.
  void sample_gpu_memory() noexcept;
  const ResolvedSettings& settings() const noexcept { return resolved_; }
  // The geometry residency this renderer drives; inactive for a scene uploaded whole. A caller
  // that wants to measure convergence from cold calls `reset_residency()` on it.
  GeometryStreamer& streamer() noexcept { return streamer_; }
  gfx::ShaderLibrary& shaders() noexcept { return shaders_; }
  // The renderer's own color target; null when it was created without one.
  const gfx::ImageResource& color_target() const noexcept { return color_; }
  // The sun's cascades the last frame was drawn with (`ResolvedSettings::csm`); `count` 0 without
  // maps. The fit is the CPU's (`fit_shadow_cascades`), so this is what the frame used, not a
  // readback.
  const ShadowCascades& shadow_cascades() const noexcept { return cascades_; }

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
    VkPipeline vertex_fallback = VK_NULL_HANDLE;  // the indexed draw's overflow: capacity-drawn
    // The cascaded shadow maps' depth-only draws: the picture's own rasterizer — the mesh path's
    // mesh stage, or the vertex path's indexed (and fallback) or capacity vertex stage — with no
    // fragment stage and the atlas as its depth attachment.
    VkPipeline shadow = VK_NULL_HANDLE;
    VkPipeline shadow_fallback = VK_NULL_HANDLE;
    VkPipeline resolve = VK_NULL_HANDLE;
    gfx::ComputePipeline software;
    gfx::ComputePipeline cull;
    gfx::ComputePipeline expand;  // vertex_expand.slang: the indexed draw's indices
    gfx::ComputePipeline deform;
    // The static shape stage over one instance's **whole mesh**, not over the cut: a cache the
    // next frame's cut can start from has to cover every cluster the cut might name.
    gfx::ComputePipeline deform_cache;
    gfx::ComputePipeline deform_alloc;
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
  bool create_shadow_maps(std::string* error);
  void destroy_shadow_maps() noexcept;
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
  GeometryStreamer streamer_;
  gfx::RenderGraph* graph_ = nullptr;  // heap: RenderGraph is not default-constructible
  gfx::ImageResource color_;           // the offscreen target, when the renderer owns one
  // The sun's depth atlas: one `shadow_map`-texel square per cascade, side by side, D32, drawn as
  // a depth attachment and sampled by the resolve through the scene's bindless set. It is sized
  // by the settings, not the screen, so a resize leaves it alone.
  gfx::ImageResource shadow_atlas_;
  VkImageView shadow_view_ = VK_NULL_HANDLE;
  VkSampler shadow_sampler_ = VK_NULL_HANDLE;
  u32 shadow_texture_slot_ = gfx::BindlessSet::k_invalid_slot;
  u32 shadow_sampler_slot_ = gfx::BindlessSet::k_invalid_slot;
  ShadowCascades cascades_;
  Vector<gfx::BufferResource> params_;       // two CullParams per view, per slot
  Vector<gfx::BufferResource> resolves_;     // one ResolveParams per view plus the lights, a slot
  Vector<gfx::BufferResource> stat_blocks_;  // host-visible copies of the argument blocks
  Vector<gfx::BufferResource> ray_params_;   // one RayVisibilityParams per view, per slot
  // Which frame each slot last carried, for `Stats::last`: its FrameDesc::frame_index and its
  // submission since the last reset.
  Vector<u64> slot_frame_;
  Vector<u64> slot_submission_;
  Stats stats_;
  VkCommandBuffer commands_ = VK_NULL_HANDLE;  // the frame between begin_frame and submit_frame
  u32 width_ = 0;
  u32 height_ = 0;
  u64 submitted_ = 0;  // frames submitted; also the slot-warmup counter
  // Frames recorded since create, never reset: the occlusion flags' ping-pong parity, which has
  // to alternate every frame whatever frame number the caller passes and whatever reset_stats did.
  u64 recorded_ = 0;
  u64 collected_ = 0;  // the timeline value whose statistics were last folded in
  bool flags_dirty_ = true;
  bool recording_ = false;
  bool joint_overflow_warned_ = false;   // a span longer than the scene was sized for, said once
  bool deform_overflow_warned_ = false;  // the pool budget refused a pair, said once
  bool vertex_fallback_warned_ = false;  // the index budget sent a cluster to the fallback, once
  bool shadow_fallback_warned_ = false;  // the same, for a shadow cascade's cut
  bool rt_overflow_warned_ = false;      // a frame dropped instances' structures, said once
  bool rt_hold_ = false;  // render_offscreen is redrawing a frame it grew the chain for
  // The ray tracing chain's capacity policy (rt_capacity.h): fed every folded frame's demand, and
  // the reason `GpuScene::resize_ray_tracing` is ever called.
  RtCapacity rt_capacity_;
  // Resizes the chain to `capacity` when it differs, waiting for the device first. False only when
  // the allocation failed, in which case the chain keeps nothing and the frame must not be drawn.
  bool apply_rt_capacity(u32 capacity, std::string* error, bool beyond_budget = false);
  void note_rt_bytes() noexcept;
};

}  // namespace engine::renderer
