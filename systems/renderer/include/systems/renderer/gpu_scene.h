#pragma once

// The scene on the device (docs/subsystems/renderer.md). Everything whose size is a function of
// the *scene* lives here: the global geometry buffers behind device addresses, the material
// table and the bindless textures, the instance table, the deformed-vertex pool, the frame's
// visible list and the indirect argument blocks it is counted into, and — when the settings ask
// for ray tracing — the cluster acceleration structures, the per-instance bottom-level
// structures, and the top-level structure over them.
//
// The line between this and `SceneRenderer` is size: a `GpuScene` is sized by the scene and a
// `SceneRenderer` by the screen. That is also the order they have to be built in, because the
// `MeshDesc` array names the deformed-vertex pool and this scene's cluster templates, so it is
// uploaded last, after every address it carries exists.
//
// **The per-frame working set carries a slice per view** (04 §4.6). A `ViewSet` renders N views
// over one scene, and each view culls independently: it needs its own visible runs, its own
// indirect argument blocks, and its own drawn-last-frame flags. They are slices of the *same*
// buffers rather than a working set per renderer, for three reasons. The visible list has to be
// one array because a visibility id is an index into it and the resolve of any view must be able
// to look one up; the ray tracing chain builds one set of structures from the **union** of the
// views' cuts, and a union is a contiguous range of one list rather than a gather over N;
// and one allocation that scales with the view count keeps the residency budget one number, which
// is what §4.6 asks a `ViewSet` to share. A single view is exactly the layout it always was.

#include <core/base/macros.h>
#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/anim/skeleton.h>
#include <domain/gfx/acceleration.h>
#include <domain/gfx/bindless.h>
#include <domain/gfx/cluster_acceleration.h>
#include <domain/gfx/cluster_cull.h>
#include <domain/gfx/visibility_resolve.h>
#include <domain/gfx/vulkan.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>

#include <span>
#include <string>

namespace engine::renderer {

// The frame's visible list is one array in three runs — the hardware pass 1, the hardware pass
// 2, and the software rasterizer — so a visibility id names an entry of the whole list however
// many draws filled it. A frame that traces shadows never has a pass 2 or a software run, and
// puts its **shadow casters** in the third (`gfx::k_caster_run`): what the cone test kept out of
// the picture but not out of the shadows (renderer.md, "Shadows").
inline constexpr u32 k_visible_runs = 3;
static_assert(gfx::k_caster_run < k_visible_runs);

// How many frames' worth of bone matrices the joint buffer holds. The frame writes slot
// `FrameContext::slot()`, and a slot is not reused until the GPU has finished the frame that last
// used it, so N regions make the host write safe against N frames in flight with no barrier and
// no staging copy. Three rather than two so a renderer created with three frames in flight fits;
// `SceneRenderer::create` refuses more than this rather than overrunning the buffer.
inline constexpr u32 k_joint_slots = 3;

// How many frames' worth of page residency the streamed scene holds, for exactly the reason
// `k_joint_slots` exists: the cull pass of frame N reads the residency word array while the host
// is already writing frame N+1's, so each frame writes its own region and a region is not reused
// until the GPU has finished the frame that last had it.
inline constexpr u32 k_stream_slots = 3;

// How many page requests one frame's cull passes may write. A page a thousand clusters want costs
// one entry, because the pass deduplicates behind a per-page mask, so this bounds the *distinct*
// pages one frame can discover it is missing — which is a few dozen even on a camera jump. The
// counter still counts past it, so an overflow is reported rather than silently truncating.
inline constexpr u32 k_max_page_requests = 4096;

// A quarter of a megabyte of page payload a frame: about two 128 KB pages, which at 60 Hz is 15 MB
// a second — more than a camera moving at a sane speed asks for, and little enough that a jump
// converges over a handful of frames instead of in one stall.
inline constexpr u32 k_default_upload_budget = 256 * 1024;

class GpuScene {
 public:
  GpuScene() noexcept = default;
  ~GpuScene();
  ENGINE_NON_COPYABLE(GpuScene);

  // Uploads `data` with the buffers `resolved` calls for. The device must outlive the scene.
  // Under `resolved.stream` the vertex, attribute, triangle and float-position streams are a
  // **page pool** of fixed-size slots instead of the whole scene, and `GeometryStreamer` fills it.
  bool create(const gfx::Device& device, const SceneData& data, const ResolvedSettings& resolved,
              std::string* error = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return device_ != nullptr; }

  const SceneData& data() const noexcept { return *data_; }
  gfx::BindlessSet& bindless() noexcept { return bindless_; }
  const gfx::BindlessSet& bindless() const noexcept { return bindless_; }

  u32 cluster_count() const noexcept { return cluster_count_; }
  u32 leaf_count() const noexcept { return leaf_count_; }
  u32 instance_count() const noexcept { return instance_count_; }
  u32 pair_count() const noexcept { return pair_count_; }
  u32 material_count() const noexcept { return material_count_; }
  u32 triangles_per_cluster() const noexcept { return triangles_per_cluster_; }
  // The deformed-vertex pool's **budget**, not its occupancy: the pool is suballocated per frame
  // from the visible list, so what a frame actually uses is `Stats::deform_vertices`.
  u64 deform_pool_bytes() const noexcept { return deform_pool_bytes_; }
  u32 deform_pool_vertices() const noexcept { return deform_pool_vertices_; }
  // What the pool would have cost with a block per instance's whole mesh, which is what E25 built
  // and what the budget replaced. Reported so a summary can say what the suballocation saved.
  u64 deform_whole_mesh_bytes() const noexcept { return deform_whole_mesh_bytes_; }
  u64 template_bytes() const noexcept { return template_bytes_; }
  // Bytes of one view's run of the visible list: `pair_count` entries of eight.
  u64 visible_run_bytes() const noexcept { return visible_run_bytes_; }
  u32 view_count() const noexcept { return view_count_; }
  // Where run `run` of view `view` starts, as an entry index into the whole list. **Run-major**:
  // every view's first run is one contiguous range at the front, which is the range the ray
  // tracing chain builds the union from. With one view this is `run * pair_count`, the layout the
  // list has always had.
  u32 visible_base(u32 view, u32 run) const noexcept {
    return (run * view_count_ + view) * pair_count_;
  }
  // Bytes into `draw_args[pass]` / `sw_args` where this view's indirect block is.
  u64 args_offset(u32 view) const noexcept { return u64{view} * gfx::k_draw_args_bytes; }
  // Bytes into `deform_args` where this (view, run)'s indirect dispatch block is.
  u64 deform_args_offset(u32 view, u32 run) const noexcept {
    return (u64{view} * k_visible_runs + run) * gfx::k_draw_args_bytes;
  }
  // Entries of the whole visible list, which is what `deform_slots` has one word each of.
  u32 visible_entries() const noexcept { return k_visible_runs * view_count_ * pair_count_; }
  // The bytes of the CLAS records, structures and scratch the ray tracing chain holds, which is
  // what a summary reports as the multi-view memory cost.
  u64 rt_bytes() const noexcept { return rt_bytes_; }
  u32 tlas_slot() const noexcept { return tlas_slot_; }

  // ---- skinning ---------------------------------------------------------------------------
  //
  // The renderer's half of docs/subsystems/animation.md's contract. A frame hands over one
  // contiguous span of `anim::JointMatrix` and one `InstanceJoints` per instance; the frame
  // copies the span into this slot's region of `joints` with **one memcpy**, rewrites this
  // slot's copy of the deform table so each skinned instance's `DeformDesc::joints` points into
  // that region, and passes the slot's table address to the pool pass through
  // `gfx::DeformParams::deform`. Nothing is allocated and nothing is uploaded per instance.
  //
  // **Why there are two deform tables.** `MeshDesc::deform` is baked into an array uploaded once,
  // and `scene.slang`'s `load_position` reads it on every position read of every rasterizer — for
  // `pool_offset` alone, which never changes. Only `joints` and `joint_count` change per frame,
  // and only `deform.slang` reads them, through a push constant. So the static table stays
  // exactly what it was (device-local, written once, what a non-skinned `--deform` run uses) and
  // the per-frame one is a separate host-visible buffer with a region per slot. A skinned scene
  // therefore has no host write racing a device read, and a rigid one has no second table at all.
  bool skinned() const noexcept { return skinned_; }
  u32 max_joints() const noexcept { return max_joints_; }
  u32 skinned_instances() const noexcept { return skinned_instances_; }
  // Entries of the deform table: one per instance that reads the pool, which is every instance
  // under `--deform` and the skinned ones otherwise.
  u32 deform_count() const noexcept { return deform_descs_.size(); }
  // The instance each deform entry belongs to, so the frame can find an entry's `InstanceJoints`.
  std::span<const u32> deform_instances() const noexcept {
    return {deform_instance_.data(), deform_instance_.size()};
  }
  // The static table as it was uploaded, which is what each frame's copy starts from.
  std::span<const gfx::DeformDesc> deform_descs() const noexcept {
    return {deform_descs_.data(), deform_descs_.size()};
  }
  u64 joint_bytes() const noexcept { return u64{max_joints_} * sizeof(anim::JointMatrix); }
  anim::JointMatrix* joint_slot(u32 slot) noexcept {
    return static_cast<anim::JointMatrix*>(joints.mapped) + u64{slot} * max_joints_;
  }
  u64 joint_slot_address(u32 slot) const noexcept {
    return joints.address + u64{slot} * joint_bytes();
  }
  // Whether the scene has a **per-frame** copy of the deform table. Skinning forces one (the bone
  // matrices are per frame) and so do morph channels (the pose weights are), and a frame writes
  // it and passes its address instead of the scene's static one.
  bool has_frame_table() const noexcept { return deform_frames.mapped != nullptr; }
  gfx::DeformDesc* deform_frame(u32 slot) noexcept {
    return static_cast<gfx::DeformDesc*>(deform_frames.mapped) + u64{slot} * deform_count();
  }
  u64 deform_frame_address(u32 slot) const noexcept {
    return deform_frames.address + u64{slot} * deform_count() * sizeof(gfx::DeformDesc);
  }

  // ---- morph channels (geometry.md, "Morph channels"; gfx.md, "The deform chain") -------------
  //
  // The scene's channel array is every mesh's concatenated, and `geometry::ClusterMeshPart`
  // records each mesh's run. What lives here is the GPU side of it: the stream's six arrays
  // behind one `gfx::MorphParams` block, one weights region per deformed instance per frame slot
  // (the static half and the pose half, which is what the two stages read), the normal pool the
  // chain writes beside the positions, and the static shape caches.
  u32 morph_channel_count() const noexcept { return morph_channel_count_; }
  bool morphed() const noexcept { return morph_channel_count_ > 0; }
  u64 morph_params_address() const noexcept { return morphed() ? morph_params.address : 0; }
  u64 normal_pool_address() const noexcept { return morphed() ? deform_normals.address : 0; }
  // The bytes the static shape caches actually hold, and how many instances got one. An instance
  // that did not runs its static stage every frame, which is a cost and not a defect.
  u64 static_cache_bytes() const noexcept { return static_cache_bytes_; }
  u32 static_cached_instances() const noexcept { return static_cached_instances_; }
  // One frame slot's weights region: `2 * morph_channel_count()` floats per deform entry, the
  // static half first. The frame writes the pose half from `FrameDesc::morph_weights` and leaves
  // the static half alone, which is what makes a weights change a *scene* event.
  f32* morph_weight_slot(u32 slot) noexcept {
    return static_cast<f32*>(morph_weights.mapped) + u64{slot} * morph_weight_floats();
  }
  u64 morph_weight_floats() const noexcept {
    return u64{deform_count()} * 2 * morph_channel_count_;
  }
  u64 morph_weight_slot_address(u32 slot) const noexcept {
    return morph_weights.address + u64{slot} * morph_weight_floats() * sizeof(f32);
  }
  // True while a static weights change has not yet been written into every instance's cache. The
  // frame clears it by dispatching `deform_cache_main` once per cached instance.
  bool static_cache_dirty() const noexcept { return static_cache_dirty_; }
  void clear_static_cache_dirty() noexcept { static_cache_dirty_ = false; }
  // The mesh's cluster count for deform entry `d`, which is the cache dispatch's group count.
  u32 deform_mesh_clusters(u32 d) const noexcept {
    return d < deform_mesh_clusters_.size() ? deform_mesh_clusters_[d] : 0u;
  }
  // Rewrites the static half of every frame slot's weights and marks the caches dirty. The
  // caller's array is indexed by the **scene's** channel, and each instance takes its mesh's run
  // of it, which is what lets one array drive a scene of several rigs.
  void set_static_weights(std::span<const f32> weights);

  // ---- geometry streaming (04 §4.3 step 3, §4.9; docs/subsystems/renderer.md) -----------------
  //
  // **What is paged and what is not.** The descriptors stay: `clusters`, `lods`,
  // `cluster_materials`, the page table, and the two per-cluster tables the drawing rule reads are
  // scene-sized and always resident, because the cull pass tests every pair of every frame and has
  // to be able to say "not that one" about a cluster whose payload is not here. That is 112 bytes
  // a cluster plus 48 a page against the ~1.5 KB a cluster a page carries — 7% held to stream the
  // other 93%, which is the ratio that makes the split worth making.
  //
  // **Slot-relative offsets are patched, not indirected.** When a page lands in slot *s* the host
  // rewrites that page's `geometry::ClusterDesc::vertex_offset` and `triangle_offset` to point
  // into slot *s* and uploads the 48-byte records with the payload. Nothing in any shader changes,
  // which matters more than it sounds: `gfx::ClusterDrawParams` is **full at its 128-byte push
  // limit** and `gfx::MeshDesc` at 64, so an indirection — "add the slot base of
  // `page_of_cluster[c]`" — has nowhere to put the two addresses it needs in the rasterizers, the
  // resolve and the CLAS records without restructuring every one of those blocks. See
  // docs/subsystems/gfx.md for what the indirection was measured to cost.
  bool streamed() const noexcept { return streamed_; }
  u32 page_count() const noexcept { return page_count_; }
  u32 page_slots() const noexcept { return page_slots_; }
  u32 slot_vertices() const noexcept { return slot_vertices_; }
  u32 slot_triangles() const noexcept { return slot_triangles_; }
  u32 max_requests() const noexcept { return max_requests_; }
  u64 page_budget_bytes() const noexcept { return page_budget_bytes_; }
  u32 upload_budget_bytes() const noexcept { return upload_budget_bytes_; }
  // What the pool, the staging ring and the always-resident tables cost, which is what a summary
  // reports against the whole scene's geometry bytes (`geometry_bytes()`).
  u64 stream_bytes() const noexcept { return stream_bytes_; }
  u64 geometry_bytes() const noexcept { return geometry_bytes_; }
  // Where each stream begins inside a staged page, and what the page costs in total.
  //
  // **There is one formula, and this is it.** Four things read this layout and two of them being
  // a byte apart is a page that never loads or a mesh drawn out of another's bytes: the staging
  // ring's allocation, the upload budget's floor (a budget under the largest page never
  // converges), the copies `GeometryStreamer::record_uploads` records, and the byte offsets a
  // file-backed page source reads its ranges into. Every sub-block is 16-byte aligned and the
  // padding is counted, because a run of `3 * triangle_count` bytes of 8-bit indices does not end
  // on a word and a ring whose blocks drift off alignment costs a split write on every stream
  // behind it.
  struct PageStage {
    u64 clusters = 0;    // the patched `geometry::ClusterDesc` records; always at 0
    u64 quantized = 0;   // three u16 a vertex
    u64 attributes = 0;  // geometry::VertexAttributes
    u64 triangles = 0;   // one packed u32 a triangle
    u64 vertices = 0;    // float positions, only when the frame builds acceleration structures
    u64 indices8 = 0;    // and their 8-bit form, packed on the host from `triangles`
    u64 total = 0;
    bool ray_tracing = false;
  };
  PageStage page_stage_layout(u32 page) const noexcept;
  // Every page's payload bytes for each stream, which is what an upload copies and a budget counts.
  u64 page_payload_bytes(u32 page) const noexcept { return page_stage_layout(page).total; }
  // This frame slot's residency word array, host-visible and mapped: the host writes it, the cull
  // pass reads it, and a slot is not reused until the GPU has finished the frame that had it.
  u32* residency_slot(u32 slot) noexcept;
  u64 residency_slot_address(u32 slot) const noexcept;
  // The block `gfx::CullParams::streaming` points at for this frame slot; the addresses are
  // constant except for the residency array, so there is one block per slot, written at create.
  u64 stream_params_address(u32 slot) const noexcept;

  // ---- the global buffers, read through device addresses -------------------------------------
  gfx::BufferResource clusters;           // geometry::ClusterDesc[]
  gfx::BufferResource quantized;          // three u16 per vertex on each mesh's own grid
  gfx::BufferResource vertices;           // float positions; only the acceleration builds read them
  gfx::BufferResource triangles;          // packed local indices
  gfx::BufferResource lods;               // geometry::ClusterLodDesc[]
  gfx::BufferResource attributes;         // geometry::VertexAttributes[]
  gfx::BufferResource skin;               // geometry::SkinBinding[]: 8 bytes per scene vertex
  gfx::BufferResource meshes;             // gfx::MeshDesc[], uploaded last
  gfx::BufferResource instances;          // gfx::InstanceDesc[]
  gfx::BufferResource materials;          // gfx::ResolveMaterial[]
  gfx::BufferResource cluster_materials;  // u32 per cluster

  // ---- the frame's working set, sized by the scene and the view count --------------------------
  gfx::BufferResource visible;       // u32x2[3 * views * pair_count]: {instance, cluster} per entry
  gfx::BufferResource draw_args[2];  // occlusion pass 1 and pass 2 indirect blocks, one per view
  gfx::BufferResource sw_args;       // the software rasterizer's indirect dispatch block, per view
  gfx::BufferResource flags[2];      // drawn last frame / this frame, ping-pong, by pair, per view

  // ---- the deformed-vertex pool ----------------------------------------------------------------
  //
  // **The pool holds what the frame deforms, not every instance's whole mesh.** E25 gave each
  // deformed instance a block as long as its mesh's cluster-ordered vertex range — 12.2 MB for
  // 1,024 foxes, 1.7 GB if they had been FlightHelmets — to write the ~1,300 clusters a frame
  // actually touches. `deform_pool` is now a **budget** (`RenderSettings::deform_pool_mib`,
  // clamped down to what the whole-mesh layout would have taken so a small scene allocates no
  // more than it did), suballocated per frame by `deform_alloc.slang`: one block per entry of the
  // visible list, as long as that cluster's vertex count. `deform_slots` is the result — one word
  // per entry of the whole list, its block's first pool vertex or `gfx::k_no_pool_slot` — and it
  // is the address every position reader now goes through (`MeshDesc::deform_slots`).
  gfx::BufferResource deform_pool;   // f32[3 * deform_pool_vertices]; shared by every view
  gfx::BufferResource deform_slots;  // u32 per entry of the visible list: its pool vertex base
  gfx::BufferResource deform_alloc;  // gfx::DeformAlloc: the frame's cursor and overflow counters
  gfx::BufferResource deform_table;  // gfx::DeformDesc[] indexed by InstanceDesc::deform
  gfx::BufferResource deform_args;   // one indirect dispatch block per (view, run)
  // Skinning: the frame's bone matrices and the frame's copy of the deform table, both
  // host-visible with `k_joint_slots` regions, both absent unless an instance is skinned.
  gfx::BufferResource joints;         // anim::JointMatrix[k_joint_slots * max_joints]
  gfx::BufferResource deform_frames;  // gfx::DeformDesc[k_joint_slots * deform_count]
  // The morph stream and the chain's two morph stages.
  gfx::BufferResource morph_channels;   // geometry::MorphChannel[]
  gfx::BufferResource morph_directory;  // u32[cluster_count + 1]
  gfx::BufferResource morph_slices;     // geometry::MorphSlice[]
  gfx::BufferResource morph_indices;    // u8 per delta
  gfx::BufferResource morph_deltas;     // i16[3 * delta_count]
  gfx::BufferResource morph_normals;    // i16[3 * delta_count], or none
  gfx::BufferResource morph_params;     // one gfx::MorphParams, the block the pass reads
  gfx::BufferResource morph_weights;    // f32[k_joint_slots * deform_count * 2 * channels]
  gfx::BufferResource deform_normals;   // u32 octahedral per pool vertex
  gfx::BufferResource static_cache;     // gfx::DeformCacheVertex[], handed out per instance

  // ---- geometry streaming ----------------------------------------------------------------------
  gfx::BufferResource page_table;       // geometry::ClusterPageDesc[page_count]
  gfx::BufferResource page_of_cluster;  // u32 per cluster
  gfx::BufferResource page_children;    // geometry::ClusterChildren per cluster
  gfx::BufferResource residency;        // u32 per page, k_stream_slots regions, host-visible
  gfx::BufferResource page_used;        // u32 per page, written by the cut
  gfx::BufferResource page_requests;    // geometry::PageRequest[max_requests]
  gfx::BufferResource request_count;    // u32, the cull pass's atomic
  gfx::BufferResource request_mask;     // u32 per page: one request a page a frame
  gfx::BufferResource stream_params;    // gfx::StreamParams[k_stream_slots]
  gfx::BufferResource page_stage;       // the staging ring: one upload budget per frame slot

  // ---- ray tracing ------------------------------------------------------------------------------
  gfx::BufferResource indices8;      // 8-bit packed cluster indices for the CLAS builds
  gfx::BufferResource records;       // CLAS build records written from the cull output
  gfx::BufferResource record_count;  // u32: how many
  gfx::BufferResource slots;         // u32 per pair: the records pass's bucketing scratch
  // Two u32 per instance: its surviving clusters, then (behind all of those) its dense record base.
  gfx::BufferResource instance_counts;
  gfx::BufferResource blas_records;  // one 16-byte bottom-level record per instance
  gfx::BufferResource rt_instances;  // one top-level instance record per instance
  gfx::BufferResource rt_scratch;
  gfx::BufferResource template_records;
  gfx::ClusterSet clas_set;
  gfx::ClusterTemplateSet clas_templates;
  Vector<gfx::ClusterBlas> cluster_blas;  // one per scene instance
  gfx::AccelerationStructure tlas;

 private:
  bool upload_geometry(const ResolvedSettings& resolved, std::string* error);
  bool upload_materials(const ResolvedSettings& resolved, std::string* error);
  bool create_working_set(const ResolvedSettings& resolved, std::string* error);
  bool create_ray_tracing(const ResolvedSettings& resolved, std::string* error);
  bool create_streaming(const ResolvedSettings& resolved, std::string* error);
  bool create_morph(const ResolvedSettings& resolved, std::string* error);

  const gfx::Device* device_ = nullptr;
  const SceneData* data_ = nullptr;
  gfx::BindlessSet bindless_;
  gfx::ImageResource procedural_texture_;
  VkImageView procedural_view_ = VK_NULL_HANDLE;
  VkSampler sampler_ = VK_NULL_HANDLE;
  Vector<gfx::ImageResource> textures_;  // decoded from the meshes' images
  Vector<VkImageView> texture_views_;
  Vector<gfx::InstanceDesc> instance_table_;  // the scene's, with material_base and deform filled
  Vector<gfx::DeformDesc> deform_descs_;      // the static table, kept for each frame's copy
  Vector<u32> deform_instance_;               // the instance of each deform entry
  u32 max_joints_ = 0;
  u32 skinned_instances_ = 0;
  bool skinned_ = false;
  u32 cluster_count_ = 0;
  u32 leaf_count_ = 0;
  u32 instance_count_ = 0;
  u32 pair_count_ = 0;
  u32 material_count_ = 0;
  u32 triangles_per_cluster_ = 0;
  u32 view_count_ = 1;
  u64 visible_run_bytes_ = 0;
  Vector<u32> deform_mesh_clusters_;  // the cache dispatch's group count per deform entry
  u32 morph_channel_count_ = 0;
  u64 static_cache_bytes_ = 0;
  u32 static_cached_instances_ = 0;
  bool static_cache_dirty_ = false;
  u32 deform_pool_vertices_ = 0;
  u64 deform_pool_bytes_ = 0;
  u64 deform_whole_mesh_bytes_ = 0;
  u64 template_bytes_ = 0;
  u64 rt_bytes_ = 0;
  u32 tlas_slot_ = gfx::BindlessSet::k_invalid_slot;
  bool ray_tracing_ = false;
  bool deform_ = false;
  bool streamed_ = false;
  u32 page_count_ = 0;
  u32 page_slots_ = 0;
  u32 slot_vertices_ = 0;   // the largest page's vertex count: every slot is sized for it
  u32 slot_triangles_ = 0;  // and its triangle count
  u32 max_requests_ = 0;
  u32 upload_budget_bytes_ = 0;
  u64 page_budget_bytes_ = 0;
  u64 stream_bytes_ = 0;
  u64 geometry_bytes_ = 0;
};

}  // namespace engine::renderer
