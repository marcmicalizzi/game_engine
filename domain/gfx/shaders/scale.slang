// Reads one storage buffer and writes another: the second stage of the render graph test, so a
// read-after-write dependency between two compute passes is exercised.

struct ScaleParams {
  uint count;
  uint factor;
};

[[vk::binding(0, 0)]] StructuredBuffer<uint> g_in;
[[vk::binding(1, 0)]] RWStructuredBuffer<uint> g_out;
[[vk::push_constant]] ConstantBuffer<ScaleParams> g_params;

[shader("compute")]
[numthreads(64, 1, 1)]
void scale(uint3 thread_id : SV_DispatchThreadID) {
  if (thread_id.x >= g_params.count) return;
  g_out[thread_id.x] = g_in[thread_id.x] * g_params.factor;
}
