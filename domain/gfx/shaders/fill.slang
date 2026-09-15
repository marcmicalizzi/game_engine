// The first shader through the toolchain: fills a storage buffer with a known pattern so the
// compute test can prove slangc, embedding, module creation, descriptors, dispatch, and
// synchronization end to end.

struct FillParams {
  uint count;
  uint multiplier;
  uint offset;
};

[[vk::binding(0, 0)]] RWStructuredBuffer<uint> g_data;
[[vk::push_constant]] ConstantBuffer<FillParams> g_params;

[shader("compute")]
[numthreads(64, 1, 1)]
void fill(uint3 thread_id : SV_DispatchThreadID) {
  if (thread_id.x >= g_params.count) return;
  g_data[thread_id.x] = thread_id.x * g_params.multiplier + g_params.offset;
}
