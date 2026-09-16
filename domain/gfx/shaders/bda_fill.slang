// Buffers by device address (ADR-0023): the shader receives a raw address in push constants
// and writes through a pointer. No descriptor is involved.

struct BdaParams {
  uint64_t address;
  uint count;
  uint stride;
};

[[vk::push_constant]] ConstantBuffer<BdaParams> g_params;

[shader("compute")]
[numthreads(64, 1, 1)]
void bda_fill(uint3 thread_id : SV_DispatchThreadID) {
  if (thread_id.x >= g_params.count) return;
  uint* data = (uint*)g_params.address;
  data[thread_id.x] = thread_id.x * g_params.stride;
}
