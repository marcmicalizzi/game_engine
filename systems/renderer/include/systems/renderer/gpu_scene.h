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
#include <domain/gfx/ground_detail.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/rhi.h>
#include <domain/gfx/visibility_resolve.h>
#include <systems/renderer/scene.h>
#include <systems/renderer/settings.h>
#include <systems/renderer/terrain_levels.h>
#include <systems/renderer/terrain_rings.h>
#include <systems/renderer/tile_layout.h>

#include <memory>
#include <span>
#include <string>

namespace engine::renderer {

// Mirrors PairExpandParams in pair_expand.slang: the push constants of the pass that writes a
// dynamic scene's changed pair-table entries from its instance table (docs/subsystems/renderer.md,
// "Instances that come and go"). The dynamic instances test holds it to the shader's reflection.
struct PairExpandParams {
  u64 instances = 0;  // gfx::InstanceDesc[]: the table set the frame reads, already uploaded
  u64 meshes = 0;     // gfx::MeshDesc[]
  u64 list = 0;       // u32[count]: the instances whose pairs to write
  u64 pairs = 0;      // u32x2[]: the same set's pair table
  u32 count = 0;
  u32 pad = 0;
};
static_assert(sizeof(PairExpandParams) == 40);
inline constexpr u32 k_pair_expand_workgroup = 64;  // numthreads in pair_expand.slang

// The frame's visible list is one array in three runs — the hardware pass 1, the hardware pass
// 2, and the software rasterizer — so a visibility id names an entry of the whole list however
// many draws filled it. A frame that traces shadows never has a pass 2 or a software run, and
// puts its **shadow casters** in the third (`gfx::k_caster_run`): what the cone test kept out of
// the picture but not out of the shadows (renderer.md, "Shadows").
inline constexpr u32 k_visible_runs = 3;

// The most (instance, cluster) pairs a scene may name: the visibility id is `pair << 8 | triangle`
// in 32 bits (gfx.md, "The tie rule"). Past it two pairs would share an id and the picture would be
// wrong without a word, so a scene — or a tail of instances — that would pass it is refused.
inline constexpr u32 k_max_pairs = 1u << 24;
static_assert(gfx::k_caster_run < k_visible_runs);

// The vertex path's index budget per region, in triangles of twelve bytes: how much of one hardware
// run of one view it draws indexed before later survivors go to the fallback draw, which draws
// their clusters' whole capacity at one vertex invocation per triangle corner
// (gfx::VertexDrawHeader). A region is sized to the scene's own bound instead when that is smaller
// — every triangle of every pair, which no cut can exceed — so a small scene can never fall back
// at all. 2^20 triangles is 12 MiB a region; the largest cut measured, 64 FlightHelmets at LOD
// 0.25 and 3840x2160, is 0.85 M.
inline constexpr u32 k_vertex_index_budget = 1u << 20;

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
  // With terrain levels, `rings` — the rings round the camera (TerrainRingSet) or the world's tiles
  // (TerrainTileSet), built before the scene is — adds each chunk level's slots and arenas beside
  // the scene's own meshes and uploads the chunks it holds into the first of them (renderer.md,
  // "The rings in the scene", "The ground from the world's tiles"); its chunks then know their
  // slots and give their DAGs up.
  bool create(const gfx::Device& device, const SceneData& data, const ResolvedSettings& resolved,
              std::string* error = nullptr, TerrainLevelSet* rings = nullptr);
  void destroy() noexcept;
  bool valid() const noexcept { return device_ != nullptr; }

  const SceneData& data() const noexcept { return *data_; }
  gfx::BindlessSet& bindless() noexcept { return bindless_; }
  const gfx::BindlessSet& bindless() const noexcept { return bindless_; }

  // ---- the sand's detail (renderer.md, "The sand close up") ------------------------------------
  //
  // Whether the scene draws it: a terrain whose description has a detail block, whose material
  // then carries `gfx::k_material_ground_detail` (and the rings' slots with it, which draw with the
  // terrain's material). The block a frame hands the resolve and the reference alike is the
  // scene's numbers with the wind the ground provider says blows at the time the ground's surface
  // stands at — the scene's own `time` until a moving terrain's motion says otherwise, every frame
  // (`TerrainMotion::frame`). The ground is asked through a sampler the scene holds for it, since a
  // dune field is too dear to make per frame; the renderer links no provider.
  bool ground_detail() const noexcept { return ground_ != nullptr; }
  gfx::GroundDetailParams ground_detail_params() const noexcept;
  // The surface's time this frame; the last frame's is kept, since the ripples fade by how far
  // they travelled between the two (`ground_detail_params`).
  void set_ground_time(f64 time_s) noexcept {
    ground_previous_s_ = ground_time_s_;
    ground_time_s_ = time_s;
  }
  f64 ground_time_s() const noexcept { return ground_time_s_; }

  u32 cluster_count() const noexcept { return cluster_count_; }
  u32 leaf_count() const noexcept { return leaf_count_; }
  u32 instance_count() const noexcept { return instance_count_; }
  u32 pair_count() const noexcept { return pair_count_; }
  // **The run length every per-pair buffer is laid out by**, which is the pair count for a scene
  // read whole and a capacity at or above it for one whose instances come and go: the visible
  // list's runs, the per-view pair-to-entry tables, the occlusion flags and the vertex path's
  // records are `pair_stride()` apart, so the tail can change length without moving any of them,
  // and only a tail past the stride reallocates them (`set_dynamic_instances`). The cull dispatch
  // and its bounds check use `pair_count()`, the pairs there are.
  u32 pair_stride() const noexcept { return pair_stride_; }

  // ---- instances added and removed between frames ---------------------------------------------
  // (docs/subsystems/renderer.md, "Instances that come and go")
  //
  // A scene whose `SceneData::dynamic` is set — a streamed world's — is its load's instances, a
  // **fixed prefix**, and a **tail** the caller replaces between frames, named block by block: a
  // streamed world's tiles (`DynamicBlock`). The meshes are resident from the load (the kits' few
  // dozen), so a tail is instances and nothing else. Each block has **its own run of instance slots
  // and a reserved run of pairs** (`TileLayout`), so a tile that arrives, leaves or changes writes
  // its own slots and nothing else, and every other tile keeps its instances and its pairs — and
  // with them its ids and its occlusion history, as the prefix always did. What no block uses is a
  // hole, which the cull pass rejects after its binary search (a null instance, of a mesh with no
  // clusters); holes past `k_default_compact_pct` of the dispatch compact the layout.
  //
  // **Nothing here waits for the device.** A change is written into `instance_table_` on the CPU
  // and marked for each **table set** — the instance table and the pair table, one set per frame in
  // flight (`reserve_table_sets`) — and the next frame to be recorded flips to the next set and
  // brings it up to date on the GPU before it reads it (`prepare_tables`): it copies the changed
  // slots out of that frame slot's staging and expands their pairs (pair_expand.slang). A set is
  // written only by the frame that flips to it, and a set comes round again only after every
  // other set has been current, so the frames in flight never see one change. Per-pair buffers
  // outgrown by a change are handed to the caller in `retired` rather than destroyed, for the same
  // reason. Refused, with a sentence and nothing changed: a scene that is not dynamic, blocks that
  // do not cover the tail in order or repeat a key, a tail past the 2^24 pairs the visibility id
  // names, a mesh the scene has not, and a skinned tail instance. A dynamic scene has no ray
  // tracing chain and no deformed-vertex pool (`create` refuses both): the chain's top-level
  // records and the pool's table are laid out once per instance, and nothing rewrites them.
  bool dynamic() const noexcept { return dynamic_; }
  u32 static_instance_count() const noexcept { return static_instances_; }
  u32 static_pair_count() const noexcept { return static_pairs_; }
  // The tail's instances and pairs — what the blocks hold, not the slots and pairs they reserve.
  u32 dynamic_instance_count() const noexcept { return layout_.live_instances(); }
  u32 dynamic_pair_count() const noexcept { return layout_.live_pairs(); }
  // What one change did: the layout's blocks, whether the per-pair buffers were made again (their
  // contents — the occlusion flags among them — are then undefined until a frame writes them), and
  // how many instance slots it wrote, which is what each table set will copy.
  struct TileChange {
    TileLayoutChange layout;
    bool grew = false;
    u32 slots_written = 0;
  };
  // Replaces the tail. `blocks` cover it in order; none is the whole tail as one block (key 0).
  // `compact` lays every block out again in the tail's order with no free blocks, which a change
  // does on its own when the holes pass the declared share; either way it is counted. Buffers a
  // frame in flight may still read — per-pair buffers and table sets that were outgrown — go to
  // `retired` for the caller to destroy when those frames are done, or are destroyed at once when
  // it is null (the device must then be idle).
  bool set_dynamic_instances(std::span<const SceneInstance> tail,
                             std::span<const DynamicBlock> blocks, TileChange& change,
                             Vector<gfx::BufferResource>* retired, std::string* error = nullptr,
                             bool compact = false);
  const TileLayout& tile_layout() const noexcept { return layout_; }
  u64 compactions() const noexcept { return compactions_; }
  // Where instance slot `slot` came from in the tail last handed over; ~0 for the load's own, a
  // hole and past the end. What a test compares a streamed scene's ids with a loaded one's through,
  // since a tile's instances sit wherever its block is.
  u32 tail_index(u32 slot) const noexcept;
  // The same for every slot at once (`instance_count()` entries), for a caller turning a whole
  // capture's ids: one pass over the blocks rather than a search per pixel.
  void tail_indices(Vector<u32>& out) const;
  // How many table sets a dynamic scene keeps: at least two, and one per frame in flight. Called
  // by `SceneRenderer::create`; a set added here is written whole by the first frame that flips to
  // it. Nothing for a scene read whole.
  bool reserve_table_sets(u32 frames_in_flight, std::string* error = nullptr);
  u32 table_sets() const noexcept { return sets_.size(); }
  // What the frame about to be recorded in frame slot `slot` has to write into the tables it reads
  // (a dynamic scene only; `write` is false for one read whole and on every frame after the first
  // since the last change). Flips `instances` and `pair_table` to the next set when anything
  // changed, stages that set's changed slots and the list of instances whose pairs the GPU writes
  // into the slot's staging, and says what to copy. The slot's previous frame must be complete (the
  // caller has been through `begin_frame`).
  struct TableUpdate {
    bool write = false;
    u32 set = 0;
    gfx::BufferResource staging;     // the instances, then the expansion list (the scene owns it)
    u64 list_address = 0;            // the expansion list's device address
    u32 slots = 0;                   // instance slots copied
    u32 expand = 0;                  // instances whose pairs the GPU writes
    u64 pairs = 0;                   // the pairs they hold
    Vector<gfx::BufferCopy> copies;  // staging -> `instances`, one per run of changed slots
  };
  bool prepare_tables(u32 slot, TableUpdate& out, std::string* error = nullptr);
  // The mesh of instance `index`, prefix or tail; ~0 past the end and for a hole's null instance
  // (whose mesh is `null_mesh()`, one past the scene's). What a census or an id capture turns an
  // instance into, since `SceneData::instances` holds only the prefix.
  u32 instance_mesh(u32 index) const noexcept {
    if (index >= instance_count_ || index >= instance_table_.size()) return ~0u;
    const u32 mesh = instance_table_[index].mesh;
    return mesh < data_->parts.size() ? mesh : ~0u;
  }
  // The mesh a hole's null instance names: one `gfx::MeshDesc` past the scene's, with no clusters.
  u32 null_mesh() const noexcept { return null_mesh_; }
  // True when every instance slot's `first_pair` is at or above the one before it and every live
  // slot is where the layout says: what the cull pass's binary search needs. For the tests.
  bool validate_tables(std::string* why = nullptr) const;
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
    return (run * view_count_ + view) * pair_stride_;
  }
  // Bytes into `draw_args[pass]` / `sw_args` where this view's indirect block is.
  u64 args_offset(u32 view) const noexcept { return u64{view} * gfx::k_draw_args_bytes; }
  // Bytes into `deform_args` where this (view, run)'s indirect dispatch block is.
  u64 deform_args_offset(u32 view, u32 run) const noexcept {
    return (u64{view} * k_visible_runs + run) * gfx::k_draw_args_bytes;
  }
  // Entries of the whole visible list, which is what `deform_slots` has one word each of: every
  // view's three runs, then one run per shadow cascade.
  u32 visible_entries() const noexcept {
    return (k_visible_runs * view_count_ + cascade_count_) * pair_stride_;
  }

  // ---- the sun's cascaded shadow maps (renderer.md, "Shadows") --------------------------------
  //
  // Each cascade is one more run of the cull pass's output, **behind** every view's three, so the
  // camera's runs and everything indexed by them — the ray tracing chain's union, the census, the
  // pool's run-major allocation of the camera's runs — are exactly where they were, and a cascade's
  // entries share the one index space everything per entry is kept in (a deformed instance's pool
  // block above all). A cascade has its own indirect argument block (`shadow_args`, one per
  // cascade, `k_draw_args_bytes` apart like the views') and, on the vertex path's indexed draw,
  // its own header, records and index array behind the views' two runs.
  u32 shadow_cascades() const noexcept { return cascade_count_; }
  u32 cascade_base(u32 cascade) const noexcept {
    return (k_visible_runs * view_count_ + cascade) * pair_stride_;
  }
  u64 shadow_args_offset(u32 cascade) const noexcept {
    return u64{cascade} * gfx::k_draw_args_bytes;
  }
  // The pool pass's indirect dispatch block for a cascade's run, behind the views' (view, run)
  // ones.
  u64 shadow_deform_args_offset(u32 cascade) const noexcept {
    return (u64{k_visible_runs} * view_count_ + cascade) * gfx::k_draw_args_bytes;
  }
  // The vertex path's indexed-draw region of a cascade: region `2 * views + cascade` of the three
  // buffers, behind the views' two hardware runs.
  u64 shadow_vertex_header_offset(u32 cascade) const noexcept {
    return (u64{2} * view_count_ + cascade) * sizeof(gfx::VertexDrawHeader);
  }
  u64 shadow_vertex_records_offset(u32 cascade) const noexcept {
    return (u64{2} * view_count_ + cascade) * pair_stride_ * sizeof(gfx::VertexDrawRecord);
  }
  u64 shadow_vertex_indices_offset(u32 cascade) const noexcept {
    return (u64{2} * view_count_ + cascade) * vertex_index_capacity_ *
           gfx::k_vertex_draw_index_bytes;
  }
  // This view's pair-to-entry table (`pair_entries`), as the address the cull and the resolve take.
  u64 pair_entries_address(u32 view) const noexcept {
    return pair_entries.address + u64{view} * pair_stride_ * sizeof(u32);
  }
  // The (instance, cluster) a visibility id's pair stands for: the inverse of the cull pass's
  // `first_cluster + (pair - first_pair)`, by binary search over the instances' prefix sum. It is a
  // function of the scene alone, like the id, so a capture decodes an id without reading anything
  // back from the frame that wrote it. False for a pair past the scene's.
  bool pair_cluster(u32 pair, u32& instance, u32& cluster) const noexcept;
  // ---- the ray tracing chain's memory (docs/subsystems/renderer.md) ----------------------------
  //
  // **Sized by the frame.** The per-frame structures — the CLAS records, the cluster structures,
  // the one bottom-level set over them and the scratch the builds share — hold `rt_capacity()`
  // clusters, which `SceneRenderer` keeps a step above what recent frames built
  // (`systems/renderer/rt_capacity.h`), and never more than `rt_capacity_limit()`, the budget in
  // clusters. `rt_union_clusters()` is what they used to be sized for — every pair in every view —
  // and a scene whose union fits under both the budget and `k_initial_rt_clusters` is still given
  // exactly that, so it can never drop anything. What stays sized by the scene is small and says
  // so: the bucketing scratch (four bytes a pair a view), three words and a record an instance,
  // the top-level structure, and the 8-bit indices and float positions the builds read.
  //
  // Everything the chain holds right now, in bytes: the per-frame part at the current capacity,
  // the scene-sized part, and the templates' storage. What a summary reports as the chain's cost.
  u64 rt_bytes() const noexcept { return rt_bytes_; }
  u32 rt_capacity() const noexcept { return rt_capacity_; }
  u32 rt_capacity_limit() const noexcept { return rt_capacity_limit_; }
  u32 rt_union_clusters() const noexcept { return rt_union_clusters_; }
  // What one cluster of capacity costs in the per-frame part, from the driver's own sizes: the
  // structure, its address and size words, its record, its share of the bottom-level set and of
  // the scratch. It is what turns the byte budget into `rt_capacity_limit()`.
  u64 rt_bytes_per_cluster() const noexcept { return rt_bytes_per_cluster_; }
  // Reallocates the per-frame part for `clusters` structures (clamped to the limit). The device
  // must be idle with respect to every buffer of the chain: the caller waits first. Nothing else
  // changes — the top-level structure, its bindless slot and the scene-sized buffers stay — so a
  // frame recorded afterwards simply imports the new buffers.
  // `beyond_budget` lets the capacity pass `rt_capacity_limit()`, up to `rt_union_clusters()`: a
  // frame that asked to be complete (`FrameDesc::rt_complete`, the reference renderer's) and
  // nothing else.
  bool resize_ray_tracing(u32 capacity, std::string* error = nullptr, bool beyond_budget = false);
  // The vertex path's indexed draw (gfx::VertexDrawHeader): for each view and each of the two
  // hardware runs, run-major like the visible list, a header, a record per pair and an index array
  // of `vertex_index_capacity()` triangles, in three buffers. Empty unless the resolved settings
  // draw indexed. These are byte offsets into `vertex_headers`, `vertex_records`, `vertex_indices`.
  u64 vertex_header_offset(u32 view, u32 run) const noexcept {
    return (u64{run} * view_count_ + view) * sizeof(gfx::VertexDrawHeader);
  }
  u64 vertex_records_offset(u32 view, u32 run) const noexcept {
    return (u64{run} * view_count_ + view) * pair_stride_ * sizeof(gfx::VertexDrawRecord);
  }
  u64 vertex_indices_offset(u32 view, u32 run) const noexcept {
    return (u64{run} * view_count_ + view) * vertex_index_capacity_ *
           gfx::k_vertex_draw_index_bytes;
  }
  u32 vertex_index_capacity() const noexcept { return vertex_index_capacity_; }
  u64 vertex_draw_bytes() const noexcept {
    return vertex_headers.size + vertex_records.size + vertex_indices.size;
  }
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
  // The deformed normal pool: a scene with morph channels carries one, and so does one with terrain
  // levels, whose pool pass writes the moved field's normals (below).
  u64 normal_pool_address() const noexcept {
    return morphed() || !terrain_.empty() ? deform_normals.address : 0;
  }

  // ---- terrain levels (docs/subsystems/renderer.md, "The dunes in time-lapse") ----------------
  //
  // A scene whose terrain is the dune generator's, drawn with `ResolvedSettings::terrain_levels`,
  // draws it as **deformed instances**: the pool pass writes each vertex of the cut at its lattice
  // point with the height blended between two evaluated fields (deform.slang's terrain stage, a
  // `gfx::TerrainLevelDesc` per level). What lives here is the device side and nothing else: per
  // level, field buffers big enough for the level's largest window — three for the scene's grid
  // (the pair drawn and the next), four for a ring (the pair drawn and the pair a re-centre draws
  // next) — and a host-visible table of level descriptions per frame slot. The fields arrive from
  // the host (`TerrainMotion` evaluates them off the frame) in host-visible staging buffers that
  // the frame copies from and then retires, so a field's upload waits for nothing and blocks
  // nothing.
  static constexpr u32 k_terrain_field_slots = 4;
  u32 terrain_level_count() const noexcept { return terrain_.size(); }
  u32 terrain_field_slots(u32 level) const noexcept {
    return level < terrain_.size() ? terrain_[level].slots : 0u;
  }
  // Whether nothing handed to `terrain_upload` for `level` is still waiting for a frame to copy it.
  bool terrain_uploaded(u32 level) const noexcept {
    return level < terrain_.size() && terrain_[level].pending.empty();
  }
  const TerrainLattice& terrain_lattice(u32 level) const noexcept {
    return terrain_[level].lattice;
  }
  // Samples one field buffer of `level` holds.
  u64 terrain_field_capacity(u32 level) const noexcept { return terrain_[level].capacity; }
  // The scene instance that draws `level`.
  u32 terrain_instance(u32 level) const noexcept { return terrain_[level].instance; }
  // A host-visible, persistently mapped buffer for `samples` heights of `level`, which the caller
  // fills from any thread and hands to `terrain_upload` (or back, `terrain_retire`). **Kept and
  // handed out again**: a staging buffer comes back to the scene once the frame that copied its
  // last piece is done, and the next field takes the smallest one it fits in; a new one is made,
  // `level`'s field capacity long, only when none is free — a time-lapse's first fields and a
  // re-centre's first pairs — so the frames that follow make none (renderer.md, "What a frame waits
  // for"). Until 2026-10-04 every field made a buffer and the frame freed it.
  bool terrain_staging(u32 level, u64 samples, gfx::BufferResource& out,
                       std::string* error = nullptr);
  // How many field staging buffers the scene has made, and holds free now.
  u32 terrain_field_stagings() const noexcept { return field_staging_made_; }
  u32 terrain_field_stagings_free() const noexcept { return field_pool_.size(); }
  // Queues `staging` to be copied into field slot `slot` of `level`, in pieces over as many frames
  // as the upload budget takes (`set_terrain_upload_budget`), before anything in those frames reads
  // a field; the staging buffer comes back once the frame that records its last piece is done
  // (`terrain_staging`). An `urgent` field —
  // one the next frame shows — goes over whole in that frame. `window` says which lattice window
  // the heights cover (its `heights` is ignored). A slot may be shown from the frame that records
  // its last piece (`terrain_slot_uploaded`); a second copy into the same slot replaces the first.
  // The copies are ordered after every frame already submitted that read the slot.
  bool terrain_upload(u32 level, u32 slot, const gfx::TerrainField& window,
                      const gfx::BufferResource& staging, std::string* error = nullptr,
                      bool urgent = false);
  // Whether no piece of a copy into field slot `slot` of `level` is still to be recorded — or
  // would be left over by the next frame, which records every piece when the budget is 0.
  bool terrain_slot_uploaded(u32 level, u32 slot) const noexcept;
  // Bytes of fields a frame copies at most; 0 is all of them. `renderer.terrain.upload_mib` when
  // the scene is made; an offscreen time-lapse, which waits for its fields, sets 0.
  void set_terrain_upload_budget(u64 bytes) noexcept { terrain_upload_budget_ = bytes; }
  u64 terrain_upload_budget() const noexcept { return terrain_upload_budget_; }
  // A staging buffer from `terrain_staging` that nothing will copy after all (a field a re-centre
  // made stale): it comes back with the next frame's.
  void terrain_retire(const gfx::BufferResource& staging) {
    if (staging.buffer.valid()) field_retired_.push_back(staging);
  }
  // What `level` draws from the next frame on: field slot `slot_a`, and `slot_b` blended in by
  // `blend` (`~0u` for none), its spheres padded by `padding` metres, and nothing of it inside
  // `hole` (x0, z0, x1, z1; empty when x1 <= x0).
  void terrain_show(u32 level, u32 slot_a, u32 slot_b, f32 blend, f32 padding, Vec4 hole) noexcept;
  // **The rings' chunks** (renderer.md, "The rings in the scene"). Each ring level has
  // `terrain_slots(level)` slots — a mesh and an identity instance each, `terrain_slot_clusters`
  // clusters and pairs apiece — and arenas of vertices and triangles. A chunk goes into a free
  // slot (`terrain_chunk_upload`: its clusters, LOD records, positions, attributes and triangles,
  // and with ray tracing its float positions and 8-bit indices, staged for the next frame to copy,
  // with its offsets moved to the slot's and the arenas'), and is drawn from the frame in which
  // `terrain_chunk_show` turns its slot on, which is also where its predecessor's is turned off
  // and freed. False, with a sentence, when no slot or no arena room is left, and nothing changes.
  u32 terrain_slots(u32 level) const noexcept;
  u32 terrain_slot_clusters(u32 level) const noexcept;
  u32 terrain_free_slots(u32 level) const noexcept;
  // A ring's arenas: what is free in all, and the largest free run, in vertices and triangles.
  struct ArenaFree {
    u64 vertices = 0;
    u64 largest_vertices = 0;
    u64 triangles = 0;
    u64 largest_triangles = 0;
    u64 vertex_capacity = 0;
    u64 triangle_capacity = 0;
  };
  ArenaFree terrain_arena_free(u32 level) const noexcept;
  bool terrain_chunk_upload(u32 level, TerrainChunk& chunk, std::string* error = nullptr);
  // **The terrain's staging ring** (renderer.md, "What a frame waits for"): one host-visible,
  // persistently mapped buffer, made and touched once when the scene is, that every chunk staged
  // for upload and every frame's slot records are written into, a frame's share released when its
  // slot comes round again. Until 2026-10-04 each chunk and each frame's records made a buffer of
  // their own — an allocation on the frame's thread, and first-touch page faults on every byte
  // staged. A staging that does not fit (an offscreen frame that uploads a whole rebuild, or the
  // first fill) gets a buffer of its own, counted in `terrain_staging_overflows`; a window's frame
  // stages no more than fits (`terrain_chunk_fits`) and leaves the rest for the next.
  bool terrain_chunk_fits(const TerrainChunk& chunk) const noexcept;
  u64 terrain_staging_bytes() const noexcept { return staging_ring_.size; }
  u64 terrain_staging_peak() const noexcept { return ring_peak_; }
  u64 terrain_staging_overflows() const noexcept { return ring_overflows_; }
  u64 terrain_staging_overflow_bytes() const noexcept { return ring_overflow_bytes_; }
  // On: the slot draws its chunk from the next frame. Off: it draws nothing, and its slot and arena
  // ranges are free for the next upload (whose copy the frame orders after every frame in flight).
  void terrain_chunk_show(u32 level, u32 slot, bool on) noexcept;
  // What the rings' slots and arenas hold on the device, and what the uploads have moved.
  u64 terrain_ring_bytes() const noexcept { return terrain_ring_bytes_; }
  u64 terrain_chunk_uploads() const noexcept { return terrain_chunk_uploads_; }
  u64 terrain_chunk_upload_bytes() const noexcept { return terrain_chunk_upload_bytes_; }
  // The frame's half, called by the frame about to be recorded in frame slot `slot`: writes that
  // slot's level table, and hands over the copies to record, the field buffers the frame reads, and
  // the staging buffers to retire once it is done.
  struct TerrainUpdate {
    u64 table = 0;  // this frame slot's `gfx::TerrainLevelDesc[]`; 0 without terrain levels
    struct Copy {
      gfx::BufferResource staging;
      gfx::BufferResource field;
      u64 bytes = 0;     // this piece's
      u64 offset = 0;    // into the staging buffer and the field alike
      bool last = true;  // the field's last piece: the scene takes the staging back once it is done
    };
    Vector<Copy> copies;
    Vector<gfx::BufferResource> fields;  // every field buffer the table names
    // The rings' chunk uploads and their slots' mesh records: copies into the scene's geometry
    // buffers, which every pass that reads geometry then has to be ordered after.
    struct GeometryCopy {
      gfx::BufferHandle src;
      gfx::BufferHandle dst;
      gfx::BufferCopy region;
    };
    Vector<GeometryCopy> geometry;
    u64 geometry_bytes = 0;
    // Buffers of their own that stagings the ring had no room for made: the frame frees them once
    // it is done (the ring's own share, and the fields' staging, come back to the scene instead).
    Vector<gfx::BufferResource> retire;
  };
  void terrain_prepare(u32 slot, TerrainUpdate& out);
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
  // What the materials' textures take on the device, as the allocator reports it, and how many
  // were the content build's built textures against how many were decoded from their source images
  // (docs/subsystems/texture.md, "In the renderer"). Both counts are **distinct** textures: one
  // upload per distinct content per scene (docs/subsystems/renderer.md, "One upload per distinct
  // image"), however many meshes sample it.
  u64 texture_bytes() const noexcept { return texture_bytes_; }
  u32 textures_built() const noexcept { return textures_built_; }
  u32 textures_decoded() const noexcept { return textures_decoded_; }
  // Every (mesh, image) a material samples and that reached a texture, counted once per mesh; of
  // those, how many took a texture the scene already held rather than uploading their own, and the
  // device bytes those uploads would have taken. With `RenderSettings::share_textures` off the last
  // two are zero and the references are the uploads.
  u32 texture_references() const noexcept { return texture_references_; }
  u32 textures_shared() const noexcept { return textures_shared_; }
  u64 texture_bytes_saved() const noexcept { return texture_bytes_saved_; }
  bool share_textures() const noexcept { return share_textures_; }  // the setting it was built with
  // The bindless samplers the materials read through: one per distinct (wrap s, wrap t, filters,
  // mipmapped) combination the scene's slots asked for, however many materials share it
  // (docs/subsystems/renderer.md, "Materials"). The heightfield's own sampler is not counted.
  u32 material_samplers() const noexcept { return material_samplers_.size(); }
  // Materials whose textured slots asked for different UV transforms, of which the table can
  // carry one: they draw every slot with the base colour's (or the first textured slot's).
  u32 transform_conflicts() const noexcept { return transform_conflicts_; }
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
  gfx::BufferResource visible;  // u32x2[3 * views * pair_count]: {instance, cluster} per entry
  // u32[views * pair_count]: the visible entry each pair this frame drew is, per view. The id a
  // pixel holds is the pair (docs/subsystems/gfx.md, "The tie rule"); this is the way back to
  // what is kept per entry, which the resolve needs only for a deformed instance's pool block.
  gfx::BufferResource pair_entries;
  // u32x2[pair_count]: the {instance, cluster} of every pair, written once at upload — the inverse
  // of the rasterizers' `pair_of`, and how the resolve decodes an id (`ResolveParams::pairs`).
  gfx::BufferResource pair_table;
  gfx::BufferResource draw_args[2];  // occlusion pass 1 and pass 2 indirect blocks, one per view
  gfx::BufferResource sw_args;       // the software rasterizer's indirect dispatch block, per view
  gfx::BufferResource shadow_args;   // one indirect block per shadow cascade; none without maps
  gfx::BufferResource flags[2];      // drawn last frame / this frame, ping-pong, by pair, per view
  // The vertex path's indexed draw, two hardware runs a view (gfx::VertexDrawHeader).
  gfx::BufferResource vertex_headers;  // VertexDrawHeader: arguments, cursor, fallback, dispatch
  gfx::BufferResource vertex_records;  // VertexDrawRecord per pair
  gfx::BufferResource vertex_indices;  // the index buffers, three u32 a triangle

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
  // Sized by the scene:
  gfx::BufferResource indices8;  // 8-bit packed cluster indices for the CLAS builds
  // u32[k_cluster_record_count_words]: built, wanted, instances that lost their drawn clusters,
  // instances that lost their casters (gfx::ClusterRecordParams). A transfer source, because the
  // frame's statistics copy it out for the capacity policy.
  gfx::BufferResource record_count;
  gfx::BufferResource slots;  // u32 per pair per view: the records pass's bucketing scratch
  // Three u32 per instance: its drawn clusters, its casters, then its dense record base.
  gfx::BufferResource instance_counts;
  gfx::BufferResource blas_records;  // one 16-byte bottom-level record per instance
  // One top-level instance record per instance, device-local and written once: the transforms
  // never change, and the one field that does — the bottom-level address, which the implicit
  // build of `blas_set` decides each frame — is written in on the GPU, a thread an instance
  // (`gfx::TlasReferenceParams`, tlas_references.slang).
  gfx::BufferResource rt_instances;
  gfx::AccelerationStructure tlas;
  gfx::ClusterTemplateSet clas_templates;  // --rt-templates: one per cluster, packed to size
  // Sized by the frame (`rt_capacity()` clusters), and replaced by `resize_ray_tracing`:
  gfx::BufferResource records;  // CLAS build or instantiate records written from the cull output
  gfx::ClusterSet clas_set;
  gfx::ClusterBlasSet blas_set;  // every instance's cluster bottom-level structure, one build
  gfx::BufferResource rt_scratch;

 private:
  bool upload_geometry(const ResolvedSettings& resolved, std::string* error);
  bool upload_materials(const ResolvedSettings& resolved, std::string* error);
  bool create_working_set(const ResolvedSettings& resolved, std::string* error);
  bool upload_pair_table(std::string* error);
  // ---- a dynamic scene's tables ----
  // One set of the tables a frame reads: the instance table and the pair table, and the instance
  // slots changed since this set was last written (`all`: every one).
  struct InstanceRange {
    u32 begin = 0;
    u32 end = 0;
  };
  struct TableSet {
    gfx::BufferResource instances;
    gfx::BufferResource pairs;
    Vector<InstanceRange> pending;
    bool all = true;
  };
  // What a live tail slot was made from, to tell a block that came back unchanged.
  struct InstanceKey {
    u32 mesh = 0;
    f32 bounds_padding = 0.0f;
    Transform3 transform;
    bool operator==(const InstanceKey&) const = default;
  };
  static InstanceKey key_of(const SceneInstance& instance) noexcept {
    return InstanceKey{instance.mesh, instance.bounds_padding, instance.transform};
  }
  // Makes a set's two buffers at the current capacities; the set starts wholly pending.
  bool create_table_set(TableSet& set, std::string* error);
  // Gives a buffer back: to `retired` when a frame may still read it, at once otherwise.
  void retire(gfx::BufferResource& buffer, Vector<gfx::BufferResource>* retired) noexcept;
  // The layout's writes into `instance_table_`, and every slot they touched marked for every set.
  bool write_slots(std::span<const SceneInstance> tail, std::span<const DynamicBlock> blocks,
                   std::span<const SlotWrite> writes, u32& slots, std::string* error);
  // After a change: the per-pair working set and the table sets grown to fit it.
  bool fit_tables(TileChange& change, Vector<gfx::BufferResource>* retired, std::string* error);
  gfx::InstanceDesc null_instance(u32 first_pair) const noexcept;
  ResolvedSettings resolved_;  // what `create` was given, for a working set made again
  bool create_ray_tracing(const ResolvedSettings& resolved, std::string* error);
  bool create_streaming(const ResolvedSettings& resolved, std::string* error);
  bool create_morph(const ResolvedSettings& resolved, std::string* error);
  bool create_terrain(const ResolvedSettings& resolved, std::string* error);
  // One terrain level's device side (above).
  struct TerrainLevel {
    TerrainLattice lattice;
    u32 instance = ~0u;
    u64 capacity = 0;  // samples a field buffer holds
    u32 slots = 0;     // field buffers made: 3 for the scene's grid, 4 for a ring
    gfx::BufferResource fields[k_terrain_field_slots];
    gfx::TerrainField windows[k_terrain_field_slots];  // what each holds, its address filled in
    u32 shown_a = ~0u;
    u32 shown_b = ~0u;
    f32 blend = 0.0f;
    f32 padding = 0.0f;
    Vec4 hole{};
    f32 skirt = 0.0f;  // metres a ring's skirt hangs below its border
    u32 flags = 0;     // gfx::TerrainLevelDesc::flags
    // Copies handed over and not yet wholly recorded, in order: `done` bytes of each went already.
    struct Pending {
      gfx::BufferResource staging;
      gfx::BufferResource field;
      u32 slot = 0;
      u64 bytes = 0;
      u64 done = 0;
      bool urgent = false;
    };
    Vector<Pending> pending;
  };
  Vector<TerrainLevel> terrain_;
  gfx::BufferResource terrain_table_;  // k_joint_slots regions of gfx::TerrainLevelDesc, mapped
  // ---- the rings' slots ----
  struct Range {
    u64 offset = 0;
    u64 count = 0;
  };
  struct TerrainSlot {
    Range vertices;
    Range triangles;
    gfx::MeshDesc desc{};  // the template: the scene's addresses and the slot's clusters
    Vec4 quant{};          // the chunk's own 16-bit grid
    u32 clusters = 0;      // the chunk's clusters
    bool loaded = false;
    bool on = false;
  };
  struct RingSlots {
    u32 level = 0;
    u32 first_mesh = 0;  // mesh of slot 0; slot s is mesh first_mesh + s
    u32 first_instance = 0;
    u32 first_cluster = 0;
    u32 clusters_per_slot = 0;
    u64 vertex_base = 0;
    u64 triangle_base = 0;
    u64 vertex_capacity = 0;
    u64 triangle_capacity = 0;
    Vector<TerrainSlot> slots;
    Vector<u32> free_slots;
    Vector<Range> free_vertices;  // offsets absolute (scene-wide vertex index)
    Vector<Range> free_triangles;
  };
  // The part of mesh `mesh`: the scene's, or a ring slot's (capacity, not its chunk).
  const geometry::ClusterMeshPart& part_of(u32 mesh) const noexcept;
  bool lay_out_rings(const ResolvedSettings& resolved, TerrainLevelSet& rings, std::string* error);
  // Stages a chunk into slot `s` of `ring` (arena ranges already taken) as copies for the next
  // frame, or for `create`'s one-shot upload.
  bool stage_chunk(RingSlots& ring, u32 s, TerrainChunk& chunk, std::string* error);
  // The staging ring (above): `bytes` of it for the next frame to copy from, 16-byte aligned, at
  // `offset`; false when it has no room (or is not made yet), and the caller makes a buffer of its
  // own. `ring_release` is the frame recorded in `region`'s: what the frame that last had it took
  // is free again, and what was staged since the last frame is this frame's.
  bool create_staging_ring(std::string* error);
  bool ring_take(u64 bytes, u64& offset) noexcept;
  bool ring_fits(u64 bytes) const noexcept;
  void ring_release(u32 region) noexcept;
  // A chunk's staged bytes: its streams one after another at 16-byte offsets (`stage_chunk`).
  u64 chunk_staging_bytes(const TerrainChunk& chunk) const noexcept;
  static bool take_range(Vector<Range>& free, u64 count, Range& out) noexcept;
  static void give_range(Vector<Range>& free, Range range) noexcept;
  Vector<RingSlots> ring_slots_;                  // index level - 1
  Vector<geometry::ClusterMeshPart> slot_parts_;  // mesh data.parts.size() + k
  u64 scene_vertex_count_ = 0;                    // the scene's own, where the arenas start
  u64 scene_triangle_count_ = 0;
  u64 vertex_capacity_ = 0;  // scene + arenas
  u64 triangle_capacity_ = 0;
  u64 terrain_ring_bytes_ = 0;
  u64 terrain_upload_budget_ = 0;  // bytes of fields a frame copies; 0 all
  u64 terrain_chunk_uploads_ = 0;
  u64 terrain_chunk_upload_bytes_ = 0;
  Vector<TerrainUpdate::GeometryCopy> pending_geometry_;
  Vector<gfx::BufferResource> pending_staging_;
  u64 pending_geometry_bytes_ = 0;
  gfx::BufferResource staging_ring_;       // the terrain's staging ring (`terrain_chunk_fits`)
  u64 ring_head_ = 0;                      // bytes ever taken from it, a running count
  u64 ring_tail_ = 0;                      // bytes ever released
  u64 ring_slot_end_[k_joint_slots] = {};  // where each frame slot's share ends, as a running count
  u64 ring_peak_ = 0;                      // the most it held at once
  u64 ring_overflows_ = 0;                 // stagings that did not fit and made a buffer
  u64 ring_overflow_bytes_ = 0;
  // The fields' staging buffers (`terrain_staging`): free ones, the ones each frame slot's last
  // frame copied the last piece of (free once that slot comes round), and the ones handed back
  // uncopied since the last frame.
  Vector<gfx::BufferResource> field_pool_;
  Vector<gfx::BufferResource> field_returning_[k_joint_slots];
  Vector<gfx::BufferResource> field_retired_;
  u32 field_staging_made_ = 0;
  Vector<u32> pending_mesh_writes_;   // mesh indices whose MeshDesc the next frame writes
  Vector<gfx::MeshDesc> mesh_descs_;  // what `meshes` holds, for a slot's record to be rewritten
  TerrainLevelSet* rings_ = nullptr;  // during `create` only
  // The sand's detail: the terrain's ground, for its wind, and the time its surface stands at.
  std::unique_ptr<TerrainSampler> ground_;
  f64 ground_time_s_ = 0.0;
  f64 ground_previous_s_ = 0.0;

  const gfx::Device* device_ = nullptr;
  const SceneData* data_ = nullptr;
  gfx::BindlessSet bindless_;
  gfx::ImageResource procedural_texture_;
  gfx::ImageViewHandle procedural_view_;
  gfx::SamplerHandle sampler_;  // the heightfield's procedural texture: linear, clamped
  // One per distinct sampler a material slot asked for: its key (the packed glTF sampler word,
  // and bit 31 for a mipmapped one), the sampler, and its bindless slot.
  struct MaterialSampler {
    u32 key = 0;
    gfx::SamplerHandle sampler;
    u32 slot = 0;
  };
  Vector<MaterialSampler> material_samplers_;
  u32 transform_conflicts_ = 0;
  // One per distinct texture of the scene, built or decoded from the meshes' images: the scene owns
  // them, every mesh that samples one shares it, and a streamed tail of instances never touches
  // them (renderer.md, "One upload per distinct image").
  Vector<gfx::ImageResource> textures_;
  Vector<gfx::ImageViewHandle> texture_views_;
  u64 texture_bytes_ = 0;
  u32 textures_built_ = 0;
  u32 textures_decoded_ = 0;
  u32 texture_references_ = 0;
  u32 textures_shared_ = 0;
  u64 texture_bytes_saved_ = 0;
  bool share_textures_ = true;
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
  u32 pair_stride_ = 0;
  bool dynamic_ = false;
  u32 static_instances_ = 0;
  u32 static_pairs_ = 0;
  Vector<u32> mesh_material_base_;        // each mesh's first material, for an instance added later
  u32 instance_capacity_ = 0;             // a dynamic scene's instance buffers, in slots
  u32 null_mesh_ = 0;                     // the mesh a hole's null instance names: no clusters
  TileLayout layout_;                     // where the tail's blocks are
  Vector<InstanceKey> tail_keys_;         // per instance slot: what a live tail slot was made from
  Vector<TileLayout::Request> requests_;  // a change's scratch
  Vector<SlotWrite> writes_;              // and the layout's answer
  Vector<TableSet> sets_;                 // the table sets; `instances`/`pair_table` alias one
  u32 current_set_ = 0;                   // the set the last frame read
  bool tables_changed_ = false;           // a change since the last frame flipped
  Vector<gfx::BufferResource> table_staging_;  // per frame slot: host visible, kept
  u32 compact_pct_ = k_default_compact_pct;    // `renderer.tiles.compact_pct`, read at create
  u64 compactions_ = 0;
  u32 material_count_ = 0;
  u32 triangles_per_cluster_ = 0;
  u32 view_count_ = 1;
  u32 cascade_count_ = 0;  // shadow cascades whose runs follow the views'
  u64 visible_run_bytes_ = 0;
  u32 vertex_index_capacity_ = 0;
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
  u64 rt_scene_bytes_ = 0;  // the chain's scene-sized part, which a resize leaves alone
  u64 rt_bytes_per_cluster_ = 0;
  u32 rt_capacity_ = 0;
  u32 rt_capacity_limit_ = 0;
  u32 rt_union_clusters_ = 0;
  u32 rt_max_per_instance_ = 0;      // the most references one instance's structure can hold
  gfx::ClusterSetLimits rt_limits_;  // what every per-frame set is created with, but the count
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
