#pragma once

// Cluster-level acceleration structures (VK_NV_cluster_acceleration_structure), experiment E2
// of docs/plan/04-renderer.md §4.4: the ray tracing geometry is built from the same clusters
// the rasterizer selected, so the two never disagree on LOD. One cluster acceleration structure
// (CLAS) per cluster is built in bulk from a device array of per-cluster records whose count may
// itself live on the device (the frame's GPU-selected cut), a cluster bottom-level structure
// references them, and the standard top-level structure of acceleration.h instances that. Each
// cluster's base geometry index is its cluster id, so a hit's GeometryIndex names the cluster
// and PrimitiveIndex the triangle without the vendor cluster-id intrinsic, and
// ray_visibility.slang traces both paths unchanged. NVIDIA only:
// DeviceFeatures::cluster_acceleration_structure; acceleration.h is the fallback.
//
//     ClusterSetLimits limits{max_clusters, ...};
//     ClusterSet set;           create_cluster_set(device, limits, set);
//     ClusterBlas blas;         create_cluster_blas(device, max_clusters, blas);
//     ...write ClusterBuildInput records for the cut into a buffer with k_build_input_usage...
//     build_cluster_set(cb, set, infos_address, count_or_0, scratch);
//     acceleration_build_barrier(cb, ACCELERATION_STRUCTURE_BUILD, ACCELERATION_STRUCTURE_READ);
//     build_cluster_blas(cb, blas, set.addresses.address, cluster_count, scratch);
//     ...blas.address is the structure's device address for the TLAS instance record...
//
// Per frame from the GPU's own cut: clas_records.slang turns the cull pass's visible list into
// the CLAS records, the record count, and the bottom-level record, then build_cluster_set with
// the count's address and build_cluster_blas_indirect follow with no CPU round trip.

#include <core/base/types.h>
#include <domain/gfx/acceleration.h>

#include <span>
#include <string>

namespace engine::gfx {

// What the device allows per cluster and how big the arrays may get. `instantiate` sizes a
// ClusterSet for instantiating cluster templates rather than building clusters from scratch;
// everything downstream (the addresses array, the cluster bottom-level build, the top-level
// structure) is the same either way.
struct ClusterSetLimits {
  u32 max_clusters = 0;                 // records per build
  u32 max_triangles_per_cluster = 124;  // at most ClusterAsProperties::max_triangles_per_cluster
  u32 max_vertices_per_cluster = 64;
  // The largest cluster id used as a geometry index: at most ClusterAsProperties::
  // max_geometry_index (the same number as DeviceFeatures::cluster_max_geometry_index), which a
  // set's size query and creation refuse to pass. It bounds what a set can *name*, not how many
  // structures it holds, so a scene can exceed it where no frame's cut is large.
  u32 max_geometry_index = 0;
  bool instantiate = false;  // the set holds instantiated templates, not rebuilt clusters
};

struct ClusterAsProperties {
  u32 max_vertices_per_cluster = 0;
  u32 max_triangles_per_cluster = 0;
  u32 max_geometry_index = 0;
  u32 cluster_alignment = 0;
  u32 scratch_alignment = 0;
  u32 bottom_level_alignment = 0;
  u32 template_alignment = 0;
};
bool cluster_as_properties(const Device& device, ClusterAsProperties& out) noexcept;

// One cluster for the CLAS build: float3 positions (stride 12) and tightly packed 8-bit local
// indices, three per triangle (pack_cluster_indices() produces them from the cluster format).
struct ClusterBuildInput {
  u32 cluster_id = 0;      // reported as the cluster's ClusterID and used as its geometry index
  u32 triangle_count = 0;  // at most 256
  u32 vertex_count = 0;    // at most 256
  DeviceAddress vertices = 0;
  DeviceAddress indices = 0;  // u8[3 * triangle_count]
  // Every cluster the picture draws is opaque. A **shadow caster** — a cluster the cone test kept
  // out of the picture but not out of the shadows (ClusterRecordParams) — is not, so a primary ray
  // that culls non-opaque geometry passes through it and a shadow ray that forces opacity stops.
  bool opaque = true;
};
inline constexpr u64 k_cluster_build_record_bytes = 64;  // one triangle-cluster build record
// Appends three bytes per packed cluster triangle (i0 | i1 << 8 | i2 << 16).
void pack_cluster_indices(std::span<const u32> packed, Vector<u8>& out);
// Writes the device records the build consumes, k_cluster_build_record_bytes each.
void write_cluster_build_records(std::span<const ClusterBuildInput> clusters, void* out) noexcept;

// What a set of `limits` costs before anything is allocated: the driver's worst-case storage for
// `limits.max_clusters` structures (with the alignment slack `create_cluster_set` adds), and the
// scratch one build of them needs. The storage is linear in the cluster count — a cluster of
// `max_triangles_per_cluster` triangles and `max_vertices_per_cluster` vertices is what every
// structure is reserved for, 6,144 bytes for 124 and 64 on the RTX 5090 at driver 610 — which is
// what lets a caller turn a byte budget into a cluster count (docs/subsystems/renderer.md, "The
// ray tracing chain's memory"). False — and nothing to budget with — for limits the device refuses
// (`limits_fit`) and for a driver answer of zero bytes, which is how a driver refuses a size query.
struct ClusterBuildSizes {
  u64 data_bytes = 0;
  u64 scratch_bytes = 0;
};
bool cluster_set_build_sizes(const Device& device, const ClusterSetLimits& limits,
                             ClusterBuildSizes& out) noexcept;

// Storage for up to max_clusters CLAS built in one command (implicit destinations): the packed
// structures, the resulting address of each, and its size.
struct ClusterSet {
  BufferResource data;       // every CLAS, packed by the driver
  BufferResource addresses;  // u64[max_clusters], written by the build; host visible
  BufferResource sizes;      // u32[max_clusters], written by the build; host visible
  u64 build_scratch_bytes = 0;
  u32 max_clusters = 0;
  ClusterSetLimits limits;
  u32 alignment = 0;
};
bool create_cluster_set(const Device& device, const ClusterSetLimits& limits, ClusterSet& out,
                        std::string* error = nullptr);
// Builds one CLAS per record: `records` holds ClusterBuildInput records (see
// write_cluster_build_records) and `count` is a device address of a u32 count, or 0 to build
// limits.max_clusters records.
void build_cluster_set(CommandList commands, const ClusterSet& set, DeviceAddress records,
                       DeviceAddress count, const BufferResource& scratch);
// The same command for a set created with `limits.instantiate`: `records` holds instantiate
// records (k_cluster_instantiate_record_bytes each, see write_cluster_instantiate_records or
// clas_records.slang's `instantiate` variant), each naming a template and the positions to
// instantiate it with. The addresses and sizes the build writes are read exactly as a rebuild's.
void instantiate_cluster_templates(CommandList commands, const ClusterSet& set,
                                   DeviceAddress records, DeviceAddress count,
                                   const BufferResource& scratch);
void destroy_cluster_set(const Device& device, ClusterSet& set) noexcept;

// Cluster templates (docs/plan/04-renderer.md §4.3, experiment E25): a template is built once
// per cluster from the bind-pose topology and instantiated per frame from deformed positions,
// which is what makes a deforming mesh's ray tracing geometry cheaper than rebuilding it. The
// positions given at build time are a reference pose the builder lays the structure out from,
// not the ones traced against; an instantiation supplies those, so the rest pose is what to hand
// a template. Build it with cluster id and base geometry index **zero**, because an instantiate
// record's offsets are added to the template's own.
struct ClusterTemplateSet {
  BufferResource data;       // every template, packed by the driver
  BufferResource addresses;  // u64[max_clusters], written by the build; host visible
  BufferResource sizes;      // u32[max_clusters], written by the build; host visible
  u64 build_scratch_bytes = 0;
  u32 max_clusters = 0;
  ClusterSetLimits limits;
  u32 alignment = 0;
};
bool create_cluster_templates(const Device& device, const ClusterSetLimits& limits,
                              ClusterTemplateSet& out, std::string* error = nullptr);
// Builds one template per record; `records` holds k_cluster_template_record_bytes each (see
// write_cluster_template_records) and `count` is a device address of a u32 count, or 0 for
// limits.max_clusters.
void build_cluster_templates(CommandList commands, const ClusterTemplateSet& set,
                             DeviceAddress records, DeviceAddress count,
                             const BufferResource& scratch);
// **Templates at the size they are, not the size they could be.** `create_cluster_templates`
// reserves the driver's worst case for every template, which for a scene is one worst-case
// template per cluster of every mesh — about 2.2 KB each on the RTX 5090 at driver 610, where the
// desert overlook's templates occupy 504–532 bytes once built — so a quarter of the storage is used
// and the rest is reserved for triangles no cluster has. This does the whole job at load, in
// three blocking submissions: the build in COMPUTE_SIZES mode writes each template's size, the
// host packs them back to back at the device's template alignment, and the build runs again in
// EXPLICIT_DESTINATIONS mode into one buffer of exactly that sum. `records` are
// `limits.max_clusters` template build records in a buffer the build can read; `out.addresses`
// then holds each template's address and `out.sizes` its size, as after `build_cluster_templates`.
// The scratch it needs is allocated and freed inside, because nothing per frame shares it.
bool create_packed_cluster_templates(const Device& device, const ClusterSetLimits& limits,
                                     DeviceAddress records, ClusterTemplateSet& out,
                                     std::string* error = nullptr);
void destroy_cluster_templates(const Device& device, ClusterTemplateSet& set) noexcept;

// The 72-byte template build record: the 64-byte cluster build record followed by one more
// address, the instantiation bounding-box limit (0 for none).
inline constexpr u64 k_cluster_template_record_bytes = 72;
void write_cluster_template_records(std::span<const ClusterBuildInput> clusters,
                                    void* out) noexcept;

// One instantiation of a template: the offsets are added to the template's own cluster id and
// base geometry index, so a template built with zeros reports exactly these.
struct ClusterInstantiateInput {
  u32 cluster_id = 0;
  u32 geometry_index = 0;
  DeviceAddress cluster_template = 0;
  DeviceAddress vertices = 0;  // float3, stride 12
};
// The 32-byte instantiate record as clas_records.slang writes it, in order: u32 clusterIdOffset;
// u32 geometryIndexOffset in 24 bits; u64 clusterTemplateAddress; then the vertex buffer as a
// u64 address and a u64 stride.
inline constexpr u64 k_cluster_instantiate_record_bytes = 32;
void write_cluster_instantiate_records(std::span<const ClusterInstantiateInput> clusters,
                                       void* out) noexcept;

// The 64-byte CLAS build record as a shader writes it (clas_records.slang mirrors this), in
// order: u32 cluster_id; u32 cluster_flags; u32 counts, which packs triangle_count, then
// vertex_count << 9, position_truncate << 18, and index_type << 24; u32 geometry, the base
// geometry index in 24 bits with the geometry flags << 29 (4 = opaque); u32 strides0, the index
// stride (1) with the vertex stride (12) << 16; u32 strides1, the geometry-index stride with the
// opacity-micromap index stride << 16 (both 0); then five u64 addresses: index_buffer,
// vertex_buffer, geometry_buffer, omm_array, omm_indices.
inline constexpr u32 k_cluster_index_type_8bit = 1;  // ..._INDEX_FORMAT_8BIT_NV
inline constexpr u32 k_cluster_geometry_opaque = 4;  // ..._GEOMETRY_OPAQUE_BIT_NV

// The 16-byte record of a cluster bottom-level build: u32 reference count, u32 stride (8), u64
// address of the CLAS address array. A GPU-driven cut writes it from a shader.
inline constexpr u64 k_cluster_blas_record_bytes = 16;

// Push constants of clas_records.slang, shared by its three entry points (64 threads a group).
// A scene builds one cluster bottom-level structure per instance, so the CLAS records of an
// instance have to end up next to each other, while the cull pass's visible list is in whatever
// order its atomics produced. Three small passes sort that out without a CPU round trip:
//
//   `records_main`  one thread per visible entry: bucket the entry into its instance's slice of
//                   `slots` (a pair-indexed scratch array) and count it into `instance_counts`.
//                   A drawn entry takes the next slot from the **bottom** of the slice
//                   (instance.first_pair * views + a per-instance atomic), a shadow caster the next
//                   from the **top** (the slice's last slot minus a second atomic), so the two
//                   never mix and the emit pass can tell them apart. Sparse: an instance's slice is
//                   as long as its mesh, times the number of views whose cuts accumulate here.
//   `ranges_main`   one thread: decide what the set's `capacity` can hold (below), the prefix sum
//                   of what it holds into each instance's dense record base, the total into
//                   `record_count`, and one 16-byte bottom-level record per instance pointing at
//                   that instance's run of CLAS addresses.
//   `emit_main`     one thread per pair slot: move the kept entries down to the dense `records`
//                   array — an instance's drawn clusters first, then its casters — and write the
//                   64-byte CLAS build record there. A record's base geometry index is the entry's
//                   **visible index**, so a hit's GeometryIndex leads to the same pair the
//                   rasterizer drew.
//
// **The set is sized by the frame, not by the scene** (docs/subsystems/renderer.md, "The ray
// tracing chain's memory"). `capacity` is how many structures the set was created for, which the
// renderer keeps a step above what recent frames built; a frame that wants more than it holds
// keeps what fits and drops the rest **whole instances at a time, casters before drawn clusters**:
// every instance's drawn clusters are taken in instance order while they fit, an instance whose
// drawn clusters do not fit is skipped entirely (its casters with them), and the other instances'
// casters are then taken the same way from what is left. Whole instances because a partial one
// would keep whichever clusters the atomics happened to bucket first, which is not the same twice;
// casters last because a caster only adds a shadow of a surface facing away from the camera,
// while a drawn cluster that loses its structure loses its shadow and, under the ray path, its
// place in the picture. `record_count` then holds four words: what was built (the count the CLAS
// build reads), what the frame wanted, how many instances lost their drawn clusters (and with
// them everything), and how many kept those but lost their casters.
//
// `instance_counts` is three words an instance: the drawn entries, the casters (both counted by
// `records_main`, and must be zeroed before it; `ranges_main` rewrites each to what was kept, 0 or
// all of it), and the dense record base. A deformed instance's record points at the frame's
// deformed-vertex pool (`MeshDesc::deform_pool`) rather than `vertices`, and with the instantiate
// mode the emit pass writes instantiate records naming each cluster's template
// (`MeshDesc::templates`) instead of build records.
//
// **More than one view** ([04 §4.6](docs/plan/04-renderer.md)): the RT geometry of a
// `renderer::ViewSet` is the union of the views' cuts under one top-level structure, so
// `records_main` covers every view's run of the visible list and the counts accumulate. The view
// count is what makes the bucketing fit: every instance's slice of `slots` is that many times its
// mesh's cluster count, because in the worst case every view draws every cluster of it. Mirrors
// RecordParams in the shader.
//
// **Shadow casters** (docs/subsystems/geometry.md, "Normal cones"). With `caster_count` set, the
// cull pass has also appended the pairs its cone test alone rejected to the visible list's run
// `k_caster_run`, and `records_main` covers that run of every view as well: a caster is built into
// its instance's structure like any drawn cluster, with its visible index as its geometry index,
// but **without the opaque flag**. The shadow rays trace with `RAY_FLAG_FORCE_OPAQUE` and so hit
// it; the primary rays of the ray path and the reference trace with `RAY_FLAG_CULL_NON_OPAQUE`
// and so do not, which keeps the traced picture the rasterized one word for word. A drawn pair
// and a caster are the same pair only in different views, so the drawn entries and the casters of
// one instance together never outgrow its slice. The templates variant cannot mark a record (the
// flags are the template's), so the renderer never asks for casters in that mode.
//
// 128 bytes, the largest push block the renderer allows — which is why `instance_first` is not a
// field of its own (it is the last third of `instance_counts`), and why the view count and the
// instantiate flag share `mode`: the word that freed is `capacity`.
struct ClusterRecordParams {
  u64 clusters = 0;       // geometry::ClusterDesc[]
  u64 vertices = 0;       // float3[]: cluster-ordered rest positions
  u64 indices8 = 0;       // u8[]: pack_cluster_indices of every cluster, in triangle order
  u64 instances = 0;      // gfx::InstanceDesc[instance_count]
  u64 meshes = 0;         // gfx::MeshDesc[]: cluster counts, the deformed-vertex pool, templates
  u64 visible = 0;        // u32x2[]: the visible list, {instance, cluster} per entry
  u64 visible_count = 0;  // u32: the cull pass's count word
  u64 slots = 0;          // u32[views * pair_count]: visible index per pair slot (records_main out)
  // u32[3 * instance_count]: drawn and caster counts per instance (records_main out; zeroed
  // first; ranges_main rewrites them to what was kept), then each instance's dense record base.
  u64 instance_counts = 0;
  // u32: view 0's count of shadow casters, view v's `k_draw_args_bytes` further on; the casters
  // themselves are run `k_caster_run` of the visible list. 0: there is no caster run.
  u64 caster_count = 0;
  u64 records = 0;  // build or instantiate records out, the matching stride each
  // u32[4] out: built (what the build reads as `count`), wanted, instances that lost their drawn
  // clusters, instances that lost their casters (k_cluster_record_count_words).
  u64 record_count = 0;
  u64 blas_records = 0;    // k_cluster_blas_record_bytes per instance (ranges_main out)
  u64 clas_addresses = 0;  // ClusterSet::addresses.address
  u32 instance_count = 0;
  // The scene's pair count: `emit_main` covers `views * pair_count` slots, and one visible run's
  // count is clamped to it. The clamp used to be the CLAS set's capacity, which is the same bound
  // for one view and the wrong one for several: a run is filled by one atomic over a dispatch of
  // `pair_count` threads and cannot exceed it, while the set holds `views` runs.
  u32 pair_count = 0;
  // The low 16 bits: how many views' cuts accumulate into this set (see above). Bit 16: write
  // k_cluster_instantiate_record_bytes records from templates instead of build records.
  u32 mode = 1;
  u32 capacity = 0;  // the structures the set holds: ranges_main keeps no more (see above)
};
static_assert(sizeof(ClusterRecordParams) == 128);
// `tlas_references.slang`'s push block: each instance's bottom-level address, which the frame's
// implicit-destination build of a `ClusterBlasSet` decided (`ClusterBlasSet::addresses`), written
// into the reference field — the last eight bytes — of its `k_instance_record_bytes` top-level
// record, one thread an instance. A dispatch rather than a copy of `count` eight-byte regions,
// which the driver encodes region by region on the CPU: 47,849 of them a frame took 3 ms to record
// on the endless desert, and every frame-thread spike past 8 ms with them
// (docs/experiments/frame-thread-spikes-2026-10-04.md).
struct TlasReferenceParams {
  u64 addresses = 0;  // u64[count]
  u64 records = 0;    // top-level instance records, k_instance_record_bytes each
  u32 count = 0;
  u32 pad = 0;
};
static_assert(sizeof(TlasReferenceParams) == 24);
inline constexpr u32 k_tlas_references_workgroup = 64;
inline constexpr u32 k_cluster_records_workgroup = 64;
inline constexpr u32 k_cluster_records_instantiate = 1u << 16;  // ClusterRecordParams::mode
inline constexpr u32 k_cluster_record_count_words = 4;
constexpr u32 cluster_records_mode(u32 views, bool instantiate) noexcept {
  return (views & 0xffffu) | (instantiate ? k_cluster_records_instantiate : 0u);
}

// Every instance's cluster bottom-level structure, built by **one** command into one buffer
// (implicit destinations), with each structure's address written to `addresses` by the build.
//
// It replaces a `ClusterBlas` per instance, and for two reasons. The memory: a structure with an
// explicit destination has to be reserved for everything it could ever hold, which for a scene is
// every cluster of its mesh in every view — the sum over the instances is `views * pairs`, the
// scene again — while one implicit build over `max_clusters` references in total is reserved for
// what a frame builds. And the time: one command in place of one per instance, each waiting on the
// last because they shared a scratch buffer, which was most of the ray tracing chain's
// milliseconds on a scene of 90 instances. The price is that an address is known only on the
// device after the build, so the top-level instance records take their `blas` field from
// `addresses` by a copy on the GPU rather than being written once on the host.
struct ClusterBlasSet {
  BufferResource data;       // every structure, packed by the driver
  BufferResource addresses;  // u64[max_structures], written by the build; storage, transfer source
  u64 build_scratch_bytes = 0;
  u32 max_structures = 0;  // bottom-level structures per build (one per scene instance)
  u32 max_clusters = 0;    // CLAS references over all of them
  u32 max_clusters_per_structure = 0;
  u32 alignment = 0;
};
bool cluster_blas_set_build_sizes(const Device& device, u32 max_structures, u32 max_clusters,
                                  u32 max_clusters_per_structure, ClusterBuildSizes& out) noexcept;
bool create_cluster_blas_set(const Device& device, u32 max_structures, u32 max_clusters,
                             u32 max_clusters_per_structure, ClusterBlasSet& out,
                             std::string* error = nullptr);
// Builds one structure per 16-byte record at `records` (k_cluster_blas_record_bytes each, as
// clas_records.slang's `ranges_main` writes them); `count` is a device address of a u32 count, or
// 0 for `max_structures`. The records' reference counts must sum to at most `max_clusters` and
// none may exceed `max_clusters_per_structure`.
void build_cluster_blas_set(CommandList commands, const ClusterBlasSet& set, DeviceAddress records,
                            DeviceAddress count, const BufferResource& scratch);
void destroy_cluster_blas_set(const Device& device, ClusterBlasSet& set) noexcept;

// A bottom-level structure over CLAS references (the addresses a ClusterSet build wrote). Built
// to an explicit destination, so its address is known when it is created and a top-level
// instance record can be written once.
struct ClusterBlas {
  BufferResource data;
  BufferResource record;       // k_cluster_blas_record_bytes; host visible
  BufferResource destination;  // u64: the explicit destination the build is told to use
  DeviceAddress address = 0;   // where the structure lives after any build (TlasInstance::blas)
  u64 build_scratch_bytes = 0;
  u32 max_clusters = 0;
  u32 alignment = 0;
};
bool create_cluster_blas(const Device& device, u32 max_clusters, ClusterBlas& out,
                         std::string* error = nullptr);
// Writes the record on the host (`references`: a u64 array of CLAS addresses; `count` how
// many) and records the build.
void build_cluster_blas(CommandList commands, const ClusterBlas& blas, DeviceAddress references,
                        u32 count, const BufferResource& scratch);
// Records the build with whatever the record holds: for a record a shader wrote on the GPU.
// `record_address` overrides `blas.record`, which is how a scene keeps one contiguous array of
// bottom-level records for its instances (one shader dispatch writes them all) while every
// instance still has its own structure.
void build_cluster_blas_indirect(CommandList commands, const ClusterBlas& blas,
                                 const BufferResource& scratch, DeviceAddress record_address = 0);
void destroy_cluster_blas(const Device& device, ClusterBlas& blas) noexcept;

}  // namespace engine::gfx
