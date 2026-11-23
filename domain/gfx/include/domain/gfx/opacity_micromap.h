#pragma once

// **Opacity micromaps for the alpha-tested clusters** (VK_EXT_opacity_micromap; roadmap R120;
// docs/subsystems/renderer.md, "Opacity micromaps"). A micromap subdivides a triangle into 4^L
// micro-triangles and gives each a state; the traversal decides the known ones itself — a
// transparent micro-triangle is never a candidate, an opaque one commits as if the geometry were
// opaque — and hands the ray loop only an *unknown* one, which it decides by the mask as it
// decides every non-opaque candidate. So a leaf card's corners and interior cost the shadow ray
// nothing and only its edge reaches the mask loop.
//
// **The bake is conservative**, which is what keeps the picture the same bytes with micromaps and
// without: a micro-triangle is opaque only when every level-0 texel the ray's bilinear lookup could
// read anywhere on it (`mask_keeps_level0` in alpha_mask.slang: the base colour's alpha at level 0
// through the material's sampler and UV transform, times the factor) keeps the hit, transparent
// only when every one drops it, and unknown otherwise. A bilinear sample is a convex combination of
// its four texels, so it can keep a hit only if one of them does and drop it only if one of them
// does; a micro-triangle is given the union of every 2x2 footprint over its UV bounding box,
// widened by a margin, and `k_micromap_alpha_margin` either side of the cutoff is unknown too.
//
// **What is built.** One `VkMicromapEXT` for the scene, every masked triangle's entry in it, a
// 4-state entry at a level chosen from the triangle's UV area in its mask's level-0 texels
// (`k_micromap_target_texels` a micro-triangle, at most the cap), with a triangle that comes out
// wholly one state given the special index instead of data. A per-triangle index buffer (i32,
// special indices negative) and a per-cluster table of each masked cluster's first index are what
// the cluster records name (`ClusterBuildInput::omm_array`/`omm_indices`, clas_records.slang).
//
//     MicromapBakeInput in{...};          // the scene's tables and the masked clusters
//     OpacityMicromapSet set;
//     bake_opacity_micromaps(device, shaders, bindless, in, set);   // blocking, at load
//     ...set.tables.address is `ClusterRecordParams::omm`...
//     destroy_opacity_micromaps(device, set);

#include <core/base/types.h>
#include <core/containers/vector.h>
#include <domain/gfx/device.h>
#include <domain/gfx/resources.h>
#include <domain/gfx/rhi.h>

#include <span>
#include <string>

namespace engine::gfx {

class ShaderLibrary;
class BindlessSet;

// The four special indices (VK_OPACITY_MICROMAP_SPECIAL_INDEX_*_EXT): a triangle that is wholly one
// state costs no data.
inline constexpr i32 k_micromap_fully_transparent = -1;
inline constexpr i32 k_micromap_fully_opaque = -2;
inline constexpr i32 k_micromap_fully_unknown_transparent = -3;
inline constexpr i32 k_micromap_fully_unknown_opaque = -4;
// A micro-triangle's 4-state value, two bits each, least significant first within a byte.
inline constexpr u32 k_micromap_transparent = 0;
inline constexpr u32 k_micromap_opaque = 1;
inline constexpr u32 k_micromap_unknown_transparent = 2;
inline constexpr u32 k_micromap_unknown_opaque = 3;
inline constexpr u32 k_micromap_format_4state = 2;   // VK_OPACITY_MICROMAP_FORMAT_4_STATE_EXT
inline constexpr u32 k_micromap_max_level = 12;      // the format's own ceiling
inline constexpr u32 k_micromap_triangle_bytes = 8;  // VkMicromapTriangleEXT
// The bake's margin either side of the material's cutoff: alpha within it is unknown, so a
// filter's rounding cannot turn a known state into a wrong one.
inline constexpr f32 k_micromap_alpha_margin = 1.0f / 512.0f;
// A micro-triangle's UV box is widened by this many texels before its footprint is taken: the
// sampler's sub-texel precision and the traversal's choice of micro-triangle on a shared edge.
inline constexpr f32 k_micromap_texel_margin = 1.0f / 64.0f;
// The level a triangle asks for: the smallest whose micro-triangles cover at most this many
// level-0 texels each (then the cap).
inline constexpr f32 k_micromap_target_texels = 4.0f;
// A micro-triangle whose footprint is more 2x2 texel quads than this is unknown without a look
// (a capped level on a very large triangle).
inline constexpr u32 k_micromap_max_quads = 1024;

// The bytes of one 4-state entry at `level` as this engine lays it out: 4^level two-bit states,
// padded to whole 32-bit words so that the bake writes a word a lane with no atomics.
constexpr u64 micromap_entry_bytes(u32 level) noexcept {
  const u64 bits = (u64{1} << (2 * level)) * 2;
  const u64 bytes = (bits + 7) / 8;
  return (bytes + 3) / 4 * 4;
}
// The format's own size before any padding (the arithmetic on renderer.md's page): 4^L x 2 bits.
constexpr u64 micromap_state_bytes(u32 level) noexcept {
  return ((u64{1} << (2 * level)) * 2 + 7) / 8;
}

// The micro-triangle `index` of a triangle subdivided `level` times, as barycentric corners
// (weights of the triangle's second and third vertices, the ray query's convention): the format's
// recursive "bird" order — the child nearest vertex 0, the middle one (flipped), the one nearest
// vertex 1, the one nearest vertex 2 — with the base-4 digits of `index` most significant first.
// meshoptimizer's experimental generator (opacitymap.cpp, `rasterizeOpacityRec`) is the reference
// for the order; opacity_micromap_bake.slang mirrors this function.
void micromap_micro_triangle(u32 index, u32 level, f32 out[6]) noexcept;

// The engine's rule for one micro-triangle on the CPU, for tests: `alpha` is a `width` x `height`
// level-0 mask (unorm8, row-major, wrapped), `factor` and `cutoff` the material's, and `t0..t2` the
// micro-triangle's corners in UV. Returns one of the four `k_micromap_*` states; the shader's
// unknown sub-state follows the centre sample, this returns `k_micromap_unknown_opaque` or
// `_transparent` by the same rule with a bilinear sample of its own.
u32 micromap_reference_state(std::span<const u8> alpha, u32 width, u32 height, f32 factor,
                             f32 cutoff, const f32 t0[2], const f32 t1[2],
                             const f32 t2[2]) noexcept;

// What the bake reads. Every address is the scene's, as the ray loops read them; `items` lists
// the alpha-tested clusters as `{cluster, material}` pairs (the opaque shadow proxy's list,
// renderer.md), and `triangle_counts` each item's cluster's triangle count.
struct MicromapBakeInput {
  DeviceAddress clusters = 0;    // geometry::ClusterDesc[]
  DeviceAddress triangles = 0;   // u32[]: packed local indices
  DeviceAddress attributes = 0;  // VertexAttributes[]: the UVs
  DeviceAddress materials = 0;   // ResolveMaterial[]
  std::span<const u32> items;    // {cluster, material} per masked cluster
  std::span<const u32> triangle_counts;
  u32 cluster_count = 0;  // the scene's: the per-cluster table's length
  u32 max_level = 4;      // the cap, at most the device's maxOpacity4StateSubdivisionLevel
  // The level a triangle asks for: the smallest whose micro-triangles cover at most this many of
  // its mask's level-0 texels (`renderer.rt.micromap_texels`).
  f32 target_texels = k_micromap_target_texels;
  // Data, indices and the triangle array together, before the driver's own storage; the cap
  // drops a level at a time until the bake fits (0: no budget).
  u64 budget_bytes = 0;
};

// What the bake measured: per level, the triangles that kept data; the special-indexed
// triangles by state; the micro-triangles by state; and the cap a budget left.
struct MicromapStats {
  u32 triangles = 0;
  u32 entries_per_level[k_micromap_max_level + 1] = {};
  u32 special_transparent = 0;
  u32 special_opaque = 0;
  u32 special_unknown = 0;
  u64 micro_transparent = 0;  // micro-triangles of the triangles with data, by state
  u64 micro_opaque = 0;
  u64 micro_unknown = 0;
  // The unknown ones whose three corners and centre, sampled as the ray loop samples, all give
  // one answer: unknown for the bilinear footprint's sake rather than an edge seen to cross them.
  u64 micro_unknown_filter = 0;
  // Per level: the micro-triangles of the triangles with data, the unknown ones, and the triangles
  // that wanted the level (the texel target's, before the cap), with data or not.
  u64 micro_per_level[k_micromap_max_level + 1] = {};
  u64 unknown_per_level[k_micromap_max_level + 1] = {};
  u32 wanted_per_level[k_micromap_max_level + 1] = {};
  // **Deduplication** (R120 stage 3): entries with data are shared by every triangle of the same
  // material and UVs, word for word; `unique_entries` is what is stored and `entries_per_level`
  // counts triangles, as before. `dedup_mismatches` are triangles whose key matched another's
  // and whose words did not (a hash collision), given entries of their own.
  u32 unique_entries = 0;
  u32 unique_per_level[k_micromap_max_level + 1] = {};
  u32 dedup_mismatches = 0;
  u32 level_cap = 0;       // the cap the bake used
  u32 levels_dropped = 0;  // how far a budget lowered it
  u64 data_bytes = 0;      // the states, as uploaded
  u64 index_bytes = 0;     // 4 a masked triangle
  u64 storage_bytes = 0;   // the micromap the driver built: what stays resident with the two tables
  f64 bake_ms = 0.0;       // the whole bake, on the CPU's clock, uploads and waits included
};

// One entry of a micromap's triangle array (VkMicromapTriangleEXT, 8 bytes): where its states
// start in the data, its level, and its format (`k_micromap_format_4state`).
struct MicromapTriangle {
  u32 data_offset = 0;
  u16 level = 0;
  u16 format = static_cast<u16>(k_micromap_format_4state);
};
static_assert(sizeof(MicromapTriangle) == k_micromap_triangle_bytes);

// `ClusterRecordParams::omm` points at this block (24 bytes), which clas_records.slang reads:
// the micromap's address, the scene's index buffer, and per cluster its first index (or ~0).
struct MicromapTables {
  u64 array =
      0;  // the micromap's device address: VkClusterAccelerationStructure...::opacityMicromapArray
  u64 indices = 0;        // i32 per masked triangle
  u64 cluster_first = 0;  // u32 per cluster of the scene: its first index, or k_micromap_none
};
static_assert(sizeof(MicromapTables) == 24);
inline constexpr u32 k_micromap_none = 0xffffffffu;

struct OpacityMicromapSet {
  MicromapHandle handle;
  BufferResource storage;        // the micromap
  BufferResource indices;        // i32 per masked triangle
  BufferResource cluster_first;  // u32 per cluster
  BufferResource tables;         // MicromapTables
  // The host's copy of `cluster_first`, for the records a CPU writes (static forms, templates).
  Vector<u32> cluster_first_host;
  MicromapStats stats;
  DeviceAddress address = 0;  // the micromap's: what a cluster record names
  u64 resident_bytes() const noexcept {
    return storage.size + indices.size + cluster_first.size + tables.size;
  }
};

// The two subdivision limits (VkPhysicalDeviceOpacityMicromapPropertiesEXT); false without the
// extension.
bool opacity_micromap_limits(const Device& device, u32& max_2state_level,
                             u32& max_4state_level) noexcept;

// The bake and the build, once, blocking: a classify pass over every masked triangle at its level,
// the budget, the special indices, the data pass, and `vkBuildMicromapsEXT`. Every buffer but the
// set's is freed before it returns. False with nothing allocated when the device has no
// micromaps, and with `out` empty (and true) when there is nothing to bake.
//
// **A change to what the bake writes bumps `renderer::k_micromap_cache_version`**
// (systems/renderer/micromap_cache.h; renderer.md, "The bake is read back"): the shader's SPIR-V is
// in the stored bake's key, but the host's rules here — the sort, the dedup, the budget's level
// drop, the special indices, the per-cluster table's layout — are not, so a stored bake made by
// the old rules would otherwise be read as the new one's.
bool bake_opacity_micromaps(const Device& device, ShaderLibrary& shaders,
                            const BindlessSet& bindless, const MicromapBakeInput& in,
                            OpacityMicromapSet& out, std::string* error = nullptr);
void destroy_opacity_micromaps(const Device& device, OpacityMicromapSet& set) noexcept;

// ---- the bake as a derived node (roadmap R207; renderer.md, "The bake is read back") ----------
//
// **Why a cached bake is keyed by the device.** The bake's bytes are not the same on every card:
// its corners and UV boxes are float arithmetic a driver may contract into fused multiply-adds,
// the box's divide is not correctly rounded in Vulkan, the unknown sub-state is a bilinear sample
// at the device's own sub-texel precision (and the shadow rays read that sub-state under the
// two-state answer, so it is picture, not padding), and the cap is the device's subdivision limit.
// So a stored bake is one device's answer: its key names the device as the driver states it
// (`micromap_device_identity`, the pipeline cache's identity), the bake shader's bytes, and what
// the bake read — and within one device the bake is a pure function of those (one thread writes
// each word, the host's sort and dedup are deterministic).

// The physical device as the pipeline cache names it: vendor, device, driver version and
// `pipelineCacheUUID`. False without a device.
bool micromap_device_identity(const Device& device, u8 out[28]) noexcept;

// **What the bake reads of the geometry, digested on the device** (the key pass, mode 3 of
// opacity_micromap_bake.slang): every masked cluster's triangles, each triangle's three
// `uv_half2` words — the only geometry the bake reads, through its packed indices — hashed in the
// cluster's order and mixed with the item's place in `in.items`, two 64-bit lanes summed with
// atomics, so the answer is integer arithmetic only and the same on any device and any schedule.
// `in.items`, the triangle counts and the materials are the host's to key. False with `out` zero
// when the device has no micromaps or a dispatch fails.
bool micromap_geometry_digest(const Device& device, ShaderLibrary& shaders,
                              const BindlessSet& bindless, const MicromapBakeInput& in, u64 out[2],
                              std::string* error = nullptr);

// The sections a bake leaves, in the order a stored bake holds them: per cluster of the scene its
// first index or `k_micromap_none`, per masked triangle its index (special or an entry), per
// stored entry its triangle record, and the states. `level_cap` is the cap the bake used.
struct MicromapSections {
  std::span<const u32> cluster_first;
  std::span<const i32> indices;
  std::span<const MicromapTriangle> triangles;
  std::span<const u32> data;  // the states, `stats.data_bytes / 4` words
  const MicromapStats* stats = nullptr;
};
// Handed the sections before the bake frees its buffers (the states are read from a host-visible
// copy only when a sink is given). A function and a context, called once; its answer is the
// sink's own (the bake has succeeded either way).
struct MicromapSink {
  void (*take)(void* context, const MicromapSections& sections) = nullptr;
  void* context = nullptr;
};
bool bake_opacity_micromaps(const Device& device, ShaderLibrary& shaders,
                            const BindlessSet& bindless, const MicromapBakeInput& in,
                            const MicromapSink& sink, OpacityMicromapSet& out,
                            std::string* error = nullptr);

// **A stored bake made resident again**, with no dispatch: the per-cluster table (also kept on the
// host), the indices and the tables uploaded, and the micromap built from its states and triangle
// records. Each section is written by `fill` straight into the memory it is uploaded from (no host
// copy of the states or the indices is held beside the upload's), in the order of
// `MicromapSection`; `fill` answers false for a short or torn read, and the restore then fails
// with nothing kept. `stats` is the stored bake's, given back as the set's.
enum class MicromapSection : u8 { cluster_first, indices, triangles, data };
struct MicromapRestore {
  u32 cluster_count = 0;
  u32 triangle_count = 0;
  u32 entry_count = 0;
  u64 data_bytes = 0;
  MicromapStats stats;
  bool (*fill)(void* context, MicromapSection section, void* dst, u64 bytes) = nullptr;
  void* context = nullptr;
};
// The restore refuses a per-cluster first index that is neither `k_micromap_none` nor below
// `triangle_count` before it uploads the table; the indices it uploads as `fill` wrote them, so a
// caller reading them from anywhere it does not trust checks them first (`micromap_index_valid`;
// the renderer's stored-bake reader does, before the restore allocates anything).
bool restore_opacity_micromaps(const Device& device, const MicromapRestore& in,
                               OpacityMicromapSet& out, std::string* error = nullptr);

// **The per-cluster table a bake of `items` lays out** (the bake's own rule, the one function both
// it and a stored bake's reader call): per cluster of the scene its first index into the masked
// triangles' indices, the items' triangle counts summed in the items' order, or `k_micromap_none`.
void micromap_cluster_first(u32 cluster_count, std::span<const u32> items,
                            std::span<const u32> triangle_counts, Vector<u32>& out);
// An index a stored bake may hold: one of the four special indices, or an entry below
// `entry_count`.
inline bool micromap_index_valid(i32 index, u32 entry_count) noexcept {
  return index < 0 ? index >= k_micromap_fully_unknown_opaque
                   : static_cast<u32>(index) < entry_count;
}

// Builds a micromap from states written on the host (`data`, two bits a micro-triangle, least
// significant first) and its triangle array, blocking: the same build the bake ends with, for a
// test or a tool that knows its states. Sets `out.handle`, `out.storage`, `out.address` and
// `out.stats.storage_bytes`; the index tables stay empty (the caller's).
bool build_opacity_micromap(const Device& device, std::span<const u32> data,
                            std::span<const MicromapTriangle> triangles, OpacityMicromapSet& out,
                            std::string* error = nullptr);

// The bake shader's push block (opacity_micromap_bake.slang mirrors it, 120 bytes). `mode` 0
// classifies — each triangle's level, the level the texel target wanted, the states its
// micro-triangles took, into `codes`, and its dedup key into `keys`; 2 verifies that every
// triangle sharing an entry is its writer's material and UVs word for word; 1 writes each
// entry's states, one thread an entry, at the offset its triangle-array entry names; and 3, the
// key pass, one thread an item, adds its triangles' UV words' two hashes into `counters[0..1]`.
struct MicromapBakeParams {
  u64 clusters = 0;
  u64 triangles = 0;
  u64 attributes = 0;
  u64 materials = 0;
  u64 items = 0;    // uint2 per item: {cluster, material}
  u64 refs = 0;     // uint2 per masked triangle: {item, triangle within the cluster}
  u64 codes = 0;    // uint per triangle (classify out): level | states seen << 4
  u64 indices = 0;  // int per triangle (write in): the special index, or the entry
  u64 entries = 0;  // VkMicromapTriangleEXT per entry (write in)
  u64 data = 0;     // the states (write out)
  // u64 [13][4] (write out): per level, the transparent, opaque and unknown micro-triangles and
  // the unknown ones whose corners and centre agree, each counted once per triangle using it
  u64 counters = 0;
  u64 keys = 0;     // uint2 per triangle (classify out): the dedup key
  u64 writers = 0;  // uint2 per entry: {the triangle that writes it, its uses} (write, verify in)
  u32 count = 0;    // masked triangles (classify, verify), entries (write)
  u32 mode = 0;     // bits 0..7: 0 classify, 1 write, 2 verify; bits 8..11: the level cap
  f32 target_texels = k_micromap_target_texels;  // a micro-triangle's most level-0 texels
  u32 pad = 0;
};
static_assert(sizeof(MicromapBakeParams) == 120);
inline constexpr u32 k_micromap_counter_columns = 4;
inline constexpr u32 k_micromap_bake_workgroup = 64;
inline constexpr u32 k_micromap_bake_row = 65535;  // groups per row of the 2D dispatch

}  // namespace engine::gfx
