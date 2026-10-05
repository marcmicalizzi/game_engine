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

// Mirrors gfx::TlasReferenceParams (80 bytes).
struct TlasReferenceParams {
  uint64_t addresses;  // uint64_t[count]: ClusterBlasSet::addresses, what the build wrote
  uint64_t records;    // VkAccelerationStructureInstanceKHR[count], 64 bytes each
  uint count;
  uint pad;
  uint64_t instances;  // InstanceDesc[count]: the scene's, in record order
  FrameEye eye;        // the frame's origin
};

[[vk::push_constant]] ConstantBuffer<TlasReferenceParams> g_params;

// A record is 64 bytes: the 3x4 transform in its first twelve words, the reference in its last
// eight bytes (gfx::k_instance_record_bytes = 64).
static const uint k_record_words = 8;
static const uint k_record_floats = 16;

[shader("compute")]
[numthreads(64, 1, 1)]
void tlas_references_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint i = thread_id.x;
  if (i >= g_params.count) return;
  uint64_t* addresses = (uint64_t*)g_params.addresses;
  uint64_t* records = (uint64_t*)g_params.records;
  records[uint64_t(i) * k_record_words + (k_record_words - 1)] = addresses[i];
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  const InstanceDesc instance = instances[i];
  const float3 from_eye = instance_from_eye(instance, g_params.eye);
  float4* rows = (float4*)(g_params.records + uint64_t(i) * k_record_floats * 4);
  rows[0] = float4(instance.rows[0].xyz, from_eye.x);
  rows[1] = float4(instance.rows[1].xyz, from_eye.y);
  rows[2] = float4(instance.rows[2].xyz, from_eye.z);
}
