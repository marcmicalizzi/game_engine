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

// Mirrors gfx::TlasReferenceParams (88 bytes).
struct TlasReferenceParams {
  uint64_t addresses;  // uint64_t[count]: ClusterBlasSet::addresses, what the build wrote
  uint64_t records;    // VkAccelerationStructureInstanceKHR[count], 64 bytes each
  uint count;
  uint pad;
  uint64_t instances;  // InstanceDesc[count]: the scene's, in record order
  FrameEye eye;        // the frame's origin
  // uint64_t per mesh: the static structure of a rigid distance form, built once (renderer.md,
  // "Static structures for rigid forms"), or 0; the whole word 0 when the scene has none.
  uint64_t static_addresses;
};

[[vk::push_constant]] ConstantBuffer<TlasReferenceParams> g_params;

// A record is 64 bytes: the 3x4 transform in its first twelve words, the reference in its last
// eight bytes (gfx::k_instance_record_bytes = 64).
static const uint k_record_words = 8;
static const uint k_record_floats = 16;
static const uint k_record_mask_word = 12;  // the 32-bit word holding the custom index and mask

[shader("compute")]
[numthreads(64, 1, 1)]
void tlas_references_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint i = thread_id.x;
  if (i >= g_params.count) return;
  uint64_t* addresses = (uint64_t*)g_params.addresses;
  uint64_t* records = (uint64_t*)g_params.records;
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  const InstanceDesc instance = instances[i];
  // A rigid form's structure was built once, at load: this frame's build left the instance's own
  // empty (clas_records.slang leaves its entries out), and its mesh's static one stands in.
  uint64_t reference = addresses[i];
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
