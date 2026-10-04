// Each instance's bottom-level address, which this frame's cluster BLAS build decided (implicit
// destinations, gfx::ClusterBlasSet), into the reference field of its top-level instance record
// (docs/subsystems/renderer.md, "The ray tracing chain's memory"). One thread an instance.
//
// **Why a dispatch and not a copy.** Until 2026-10-04 this was one vkCmdCopyBuffer of
// `instance_count` eight-byte regions, and on the endless desert with far levels that is 47,849
// regions a frame: the driver encodes every region into the command buffer on the CPU, which made
// recording that one command 3 ms of a 3.5 ms frame at the median and every frame-thread spike
// past 8 ms (docs/experiments/frame-thread-spikes-2026-10-04.md). A dispatch is one command
// whatever the count, and writes the same 8 bytes into the same place.

// Mirrors gfx::TlasReferenceParams (24 bytes).
struct TlasReferenceParams {
  uint64_t addresses;  // uint64_t[count]: ClusterBlasSet::addresses, what the build wrote
  uint64_t records;    // VkAccelerationStructureInstanceKHR[count], 64 bytes each
  uint count;
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<TlasReferenceParams> g_params;

// The reference is the record's last eight bytes (gfx::k_instance_record_bytes = 64).
static const uint k_record_words = 8;

[shader("compute")]
[numthreads(64, 1, 1)]
void tlas_references_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint i = thread_id.x;
  if (i >= g_params.count) return;
  uint64_t* addresses = (uint64_t*)g_params.addresses;
  uint64_t* records = (uint64_t*)g_params.records;
  records[uint64_t(i) * k_record_words + (k_record_words - 1)] = addresses[i];
}
