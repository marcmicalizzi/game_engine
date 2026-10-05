// The world-eye test's probe (tests/world_eye_tests.cpp; ADR-0053): scene.slang's
// `instance_from_eye` and `instance_point` for one (instance, eye, point) case a thread, written
// out so the test can hold them to `engine::relative` in core/math/world.h to the bit.

#include "scene.slang"

struct WorldEyeProbeParams {
  uint64_t instances;  // InstanceDesc[count]
  uint64_t eyes;       // FrameEye[count]
  uint64_t points;     // float4[count]: a mesh-space point in xyz
  uint64_t out;        // float4[2 * count]: instance_from_eye, then instance_point
  uint count;
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<WorldEyeProbeParams> g_params;

[shader("compute")]
[numthreads(64, 1, 1)]
void world_eye_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint i = thread_id.x;
  if (i >= g_params.count) return;
  const InstanceDesc instance = ((InstanceDesc*)g_params.instances)[i];
  const FrameEye eye = ((FrameEye*)g_params.eyes)[i];
  const float3 p = ((float4*)g_params.points)[i].xyz;
  const float3 from_eye = instance_from_eye(instance, eye);
  float4* out = (float4*)g_params.out;
  out[2 * i] = float4(from_eye, 0.0);
  out[2 * i + 1] = float4(instance_point(instance, from_eye, p), 0.0);
}
