// Each instance's bottom-level address, which this frame's cluster BLAS build decided (implicit
// destinations, gfx::ClusterBlasSet), into the reference field of its top-level instance record
// (docs/subsystems/renderer.md, "The ray tracing chain's memory"), and **the record's transform,
// measured from this frame's eye** (ADR-0053). One thread an instance.
//
// **Why a dispatch and not a copy.** Until 2026-10-04 this was one vkCmdCopyBuffer of
// `instance_count` eight-byte regions, and on the endless desert with far levels that is 47,849
// regions a frame: the driver encodes every region into the command buffer on the CPU, which made
// recording that one command 3 ms of a 3.5 ms frame at the median and every frame-thread spike
// past 8 ms (docs/experiments/frame-thread-spikes-2026-10-04.md). A dispatch is one command
// whatever the count, and writes the same 8 bytes into the same place.
//
// **Why the transform is written here, every frame.** The structures are traced in the frame's
// space, whose origin is the eye, so an instance's transform is its 3x4 with the translation
// replaced by `instance_from_eye` — the same function, the same operands and so the same bits as
// the rasterizers' (scene.slang). Written on the GPU from the instance's cell and the frame's eye,
// nothing is rewritten on the CPU when the eye moves, and the frame thread's cost does not depend
// on where the camera has been. It is 48 more bytes a record in the dispatch that was already
// writing eight.

#include "scene.slang"

// Mirrors gfx::TlasReferenceParams (120 bytes).
struct TlasReferenceParams {
  uint64_t addresses;  // uint64_t[count]: ClusterBlasSet::addresses, what the build wrote
  uint64_t records;    // VkAccelerationStructureInstanceKHR[count], 64 bytes each
  uint count;
  // Nonzero: the casters have records of their own, `count + i` for instance i (R140; renderer.md,
  // "The casters in instances of their own"), and both passes cover `2 * count`.
  uint caster_records;
  uint64_t instances;  // InstanceDesc[count]: the scene's, in record order
  FrameEye eye;        // the frame's origin
  // uint64_t per mesh: the static structure of a rigid distance form, built once (renderer.md,
  // "Static structures for rigid forms"), or 0; the whole word 0 when the scene has none.
  uint64_t static_addresses;
  // **The bottom-level build is over what the frame drew** (renderer.md, "The bottom-level build is
  // over what the frame drew"; R143): the ranges' per-instance records, the same records without
  // the empty ones, each instance's place among those (or `k_blas_none`), and their count, which
  // the build reads. All zero for a caller whose build is over every instance.
  uint64_t blas_records;  // BlasRecord[count], as clas_records.slang's ranges wrote them
  uint64_t blas_dense;    // BlasRecord[]: only the records that reference anything
  uint64_t blas_slots;    // uint[count]: an instance's place in `blas_dense`
  uint64_t blas_count;    // uint: the records in `blas_dense`; the ranges clear it every frame
};

// Mirrors VkClusterAccelerationStructureBuildClustersBottomLevelInfoNV (16 bytes).
struct BlasRecord {
  uint count;
  uint stride;
  uint64_t references;
};
static const uint k_blas_none = 0xffffffffu;  // gfx::k_cluster_blas_none

[[vk::push_constant]] ConstantBuffer<TlasReferenceParams> g_params;

// **Only the instances that drew something are built** (R143). The ranges leave one bottom-level
// record per instance slot, and the build costs the records it is handed whether they reference
// anything or not: on the forest proof of concept's 219,166 slots that was 2.17 ms a frame, flat,
// for a frame that draws a tenth of them — a streamed world's far rings are static forms, whose
// record is empty every frame, and so is every slot the view does not hold. One thread an instance,
// before the build: a record with a reference takes the next place of `blas_dense` and says so in
// `blas_slots`. The places follow the atomic, so the order of the structures in the build's buffer
// is not a function of the frame; the picture cannot tell, since a hit is named by its top-level
// record, which stays the instance's, and the ray path settles a tied distance by the pair.
// The bottom-level records and top-level records this frame has: one an instance, or with the
// casters' own records (R140) the casters' after the scene's slots, `caster_records + i`, and the
// records between the frame's instances and the slots (slots not live) inactive.
uint record_total() {
  return g_params.caster_records != 0 ? g_params.caster_records + g_params.count : g_params.count;
}
bool record_idle(uint i) { return i >= g_params.count && i < g_params.caster_records; }

[shader("compute")]
[numthreads(64, 1, 1)]
void blas_compact_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint i = thread_id.x;
  if (i >= record_total()) return;
  BlasRecord* records = (BlasRecord*)g_params.blas_records;
  uint* slots = (uint*)g_params.blas_slots;
  if (record_idle(i)) {
    slots[i] = k_blas_none;
    return;
  }
  const BlasRecord record = records[i];
  if (record.count == 0) {
    slots[i] = k_blas_none;
    return;
  }
  Atomic<uint>* built = (Atomic<uint>*)g_params.blas_count;
  const uint place = built[0].add(1);
  BlasRecord* dense = (BlasRecord*)g_params.blas_dense;
  dense[place] = record;
  slots[i] = place;
}

// A record is 64 bytes: the 3x4 transform in its first twelve words, the reference in its last
// eight bytes (gfx::k_instance_record_bytes = 64).
static const uint k_record_words = 8;
static const uint k_record_floats = 16;
static const uint k_record_mask_word = 12;  // the 32-bit word holding the custom index and mask

[shader("compute")]
[numthreads(64, 1, 1)]
void tlas_references_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint i = thread_id.x;
  if (i >= record_total()) return;
  uint64_t* addresses = (uint64_t*)g_params.addresses;
  uint64_t* records = (uint64_t*)g_params.records;
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  if (record_idle(i)) {
    // A slot not live, inside the top-level build only because the casters' half starts past
    // every slot: a zero reference is an inactive instance whatever its other words hold.
    records[uint64_t(i) * k_record_words + (k_record_words - 1)] = 0;
    return;
  }
  if (i >= g_params.count) {
    // **A caster's own record** (R140; renderer.md, "The casters in instances of their own"): the
    // drawn record's transform, custom index and flags, the casters' structure, and the caster bit
    // alone — never met by a camera ray (`k_ray_mask_camera`), met by every light's — or 0 where the
    // drawn record is a hole or its instance is untraced. Word 12's custom index never changes and
    // its mask's being zero is the CPU's, so reading it beside the drawn thread's write of the mask
    // reads the same index and the same zeroness either way.
    const uint j = i - g_params.caster_records;
    const InstanceDesc owner = instances[j];
    uint64_t caster_reference = 0;
    if (g_params.blas_slots != 0) {
      const uint place = ((uint*)g_params.blas_slots)[i];
      if (place != k_blas_none) caster_reference = addresses[place];
    } else {
      caster_reference = addresses[i];
    }
    records[uint64_t(i) * k_record_words + (k_record_words - 1)] = caster_reference;
    uint* drawn_words = (uint*)(g_params.records + uint64_t(j) * k_record_floats * 4);
    uint* caster_words = (uint*)(g_params.records + uint64_t(i) * k_record_floats * 4);
    const uint drawn_index_mask = drawn_words[k_record_mask_word];
    const uint caster_mask = (drawn_index_mask >> 24) != 0u
                                 ? (instance_ray_mask(owner.flags) & k_ray_mask_caster)
                                 : 0u;
    caster_words[k_record_mask_word] = (drawn_index_mask & 0xffffffu) | (caster_mask << 24);
    caster_words[k_record_mask_word + 1] = drawn_words[k_record_mask_word + 1];
    const float3 owner_from_eye = instance_from_eye(owner, g_params.eye);
    float4* caster_rows = (float4*)(g_params.records + uint64_t(i) * k_record_floats * 4);
    caster_rows[0] = float4(owner.rows[0].xyz, owner_from_eye.x);
    caster_rows[1] = float4(owner.rows[1].xyz, owner_from_eye.y);
    caster_rows[2] = float4(owner.rows[2].xyz, owner_from_eye.z);
    return;
  }
  const InstanceDesc instance = instances[i];
  // A rigid form's structure was built once, at load: this frame's build left the instance's own
  // empty (clas_records.slang leaves its entries out), and its mesh's static one stands in.
  uint64_t reference = 0;
  if (g_params.blas_slots != 0) {
    // The build was over the instances that drew something (`blas_compact_main`): this one's
    // structure is at its place among them, and an instance with none stays inactive.
    const uint place = ((uint*)g_params.blas_slots)[i];
    if (place != k_blas_none) reference = addresses[place];
  } else {
    reference = addresses[i];
  }
  if (g_params.static_addresses != 0 && (instance.flags & k_instance_static_rt) != 0) {
    reference = ((uint64_t*)g_params.static_addresses)[instance.mesh];
  }
  records[uint64_t(i) * k_record_words + (k_record_words - 1)] = reference;
  // **The record's mask, from the instance's flags** (R110; gfx.md, "The ray mask"): every bit but
  // the caster bit for an instance no light's ray may meet, 0xff for every other. Written here
  // every frame, beside the transform, so a flag that changes reaches the top level without the CPU
  // rewriting the record; a record whose mask is 0 — a dynamic scene's hole, which the CPU writes
  // so (`GpuScene::tlas_record`) — stays inactive. Word 12 is instanceCustomIndex | mask << 24.
  uint* words = (uint*)(g_params.records + uint64_t(i) * k_record_floats * 4);
  const uint index_mask = words[k_record_mask_word];
  if ((index_mask >> 24) != 0u) {
    words[k_record_mask_word] = (index_mask & 0xffffffu) | (instance_ray_mask(instance.flags) << 24);
  }
  const float3 from_eye = instance_from_eye(instance, g_params.eye);
  float4* rows = (float4*)(g_params.records + uint64_t(i) * k_record_floats * 4);
  rows[0] = float4(instance.rows[0].xyz, from_eye.x);
  rows[1] = float4(instance.rows[1].xyz, from_eye.y);
  rows[2] = float4(instance.rows[2].xyz, from_eye.z);
}
