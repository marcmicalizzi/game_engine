// Hi-Z pyramid of the farthest depth (docs/plan/04-renderer.md §4.3, two-pass occlusion
// culling). Mip 0 copies the reversed-Z depth out of the 64-bit visibility buffer (empty
// pixels are 0, the far plane); every further mip takes the minimum, the farthest depth, of
// its 2x2 source texels. Mip sizes round up, so the last texel of an odd dimension covers one
// source texel and nothing is skipped. A cluster is occluded when its nearest point is farther
// than the Hi-Z value over its whole screen rectangle (cluster_cull.slang).

struct HizParams {
  uint64_t src;   // uint64_t[] visibility buffer (from_visibility) or float[] of the source mip
  uint64_t dst;   // float[] of the destination mip
  uint src_width;
  uint src_height;
  uint dst_width;
  uint dst_height;
  uint from_visibility;
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<HizParams> g_params;

[shader("compute")]
[numthreads(8, 8, 1)]
void hiz_build_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint x = thread_id.x;
  const uint y = thread_id.y;
  if (x >= g_params.dst_width || y >= g_params.dst_height) return;
  float* dst = (float*)g_params.dst;
  if (g_params.from_visibility != 0) {
    uint64_t* visibility = (uint64_t*)g_params.src;
    const uint64_t value = visibility[y * g_params.src_width + x];
    dst[y * g_params.dst_width + x] = asfloat(uint(value >> 32));
    return;
  }
  float* src = (float*)g_params.src;
  const uint x0 = x * 2;
  const uint y0 = y * 2;
  const uint x1 = min(x0 + 1, g_params.src_width - 1);
  const uint y1 = min(y0 + 1, g_params.src_height - 1);
  const float farthest = min(min(src[y0 * g_params.src_width + x0], src[y0 * g_params.src_width + x1]),
                             min(src[y1 * g_params.src_width + x0], src[y1 * g_params.src_width + x1]));
  dst[y * g_params.dst_width + x] = farthest;
}
