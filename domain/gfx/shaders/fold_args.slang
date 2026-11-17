// Folded indirect arguments from counts the GPU wrote (T165; gfx.md, "A dispatch that grows with
// the scene"; domain/gfx/dispatch.h, `gfx::FoldArgsParams`). A dispatch whose workgroup count is a
// run's survivors reads its arguments from here rather than from the count word itself, which is
// `{count, 1, 1}` and would ask for more than Vulkan's guaranteed 65,535 workgroups on x past that
// many survivors: thread i reads count word i and writes `folded_args(count)` (dispatch.slang).
// It took the place of the transfer pass that copied the deformed-vertex pool's count words, and
// writes the software rasterizer's arguments beside its count.

#include "dispatch.slang"

// Mirrors gfx::FoldArgsParams (32 bytes).
struct FoldParams {
  uint64_t counts;    // uint: count i at counts + i * count_stride
  uint64_t args;      // uint3: the arguments of count i at args + i * args_stride
  uint count_stride;  // bytes
  uint args_stride;   // bytes
  uint n;             // counts to fold
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<FoldParams> g_params;

[shader("compute")]
[numthreads(32, 1, 1)]
void fold_args_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint i = thread_id.x;
  if (i >= g_params.n) return;
  const uint count = *(uint*)(g_params.counts + uint64_t(i) * g_params.count_stride);
  uint* args = (uint*)(g_params.args + uint64_t(i) * g_params.args_stride);
  const uint3 folded = folded_args(count);
  args[0] = folded.x;
  args[1] = folded.y;
  args[2] = folded.z;
}
