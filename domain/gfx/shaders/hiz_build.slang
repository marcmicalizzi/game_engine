// Hi-Z pyramid of the farthest depth (docs/plan/04-renderer.md §4.3, two-pass occlusion
// culling). Mip 0 is the reversed-Z depth copied out of the 64-bit visibility buffer (empty
// pixels are 0, the far plane); every further mip takes the minimum, the farthest depth, of
// its 2x2 source texels. Mip sizes round up, so the last texel of an odd dimension covers one
// source texel and nothing is skipped. A cluster is occluded when its nearest point is farther
// than the Hi-Z value over its whole screen rectangle (cluster_cull.slang).
//
// **One workgroup reduces a 32 x 32 tile into up to six mips**, rather than one dispatch per
// mip. The pyramid is bandwidth-bound — at 11520 x 2160 one build moves about 460 MB and the
// two builds a frame measured 87% of the card's peak — and a dispatch per mip re-reads the
// whole of each mip out of memory to produce the next one. Reducing inside a workgroup pays
// that read once: the tile's 16 x 16 threads each fold a 2 x 2 quad, and the four further
// halvings (16 -> 8 -> 4 -> 2 -> 1) run through shared memory. What is left in memory is the
// source read, which is the visibility buffer and cannot be avoided, and one write per texel of
// every mip, which is what the cull pass reads. A pyramid deeper than six mips is a second
// dispatch reading the sixth, and so on; 11520 x 2160 is 15 mips, so three dispatches and two
// barriers instead of fifteen and fourteen.
//
// **The values are bit-identical to the mip-at-a-time build**, which is what makes this safe:
// `min` is exact in floating point and the tile is a power of two aligned at a power-of-two
// multiple, so a workgroup's texels at every level are exactly the 2x2 subtrees the per-mip
// pass would have reduced, in the same partition. The edge rule is reproduced too: the per-mip
// pass clamps an out-of-range source to the last texel, and `min(a, a) = a`, so a shared-memory
// slot outside the mip holds +infinity's stand-in (every depth is in [0, 1]) and folds away.
// The occlusion test recomputes the whole pyramid on the CPU and requires equality.

struct HizParams {
  uint64_t src;      // uint64_t[] visibility buffer (from_visibility) or float[] of mip src_mip
  uint64_t pyramid;  // float[]: this view's pyramid, its mips back to back
  uint width;        // mip 0's extent, which every mip's extent is derived from
  uint height;
  uint src_mip;     // which mip `src` holds
  uint src_offset;  // element offset of mip `src_mip` in `pyramid`
  uint levels;      // reductions this dispatch writes: mips src_mip + 1 .. src_mip + levels
  uint from_visibility;  // 1: `src` is the visibility buffer and mip `src_mip` is written too
};

[[vk::push_constant]] ConstantBuffer<HizParams> g_params;

static const uint k_hiz_threads = 16;  // 16 x 16 threads fold a 32 x 32 tile
// The identity of `min` over this data: a reversed-Z depth is in [0, 1], so any larger value
// leaves the result of a fold untouched and stands in for a texel outside the mip.
static const float k_hiz_far = 1.0e30;

groupshared float g_tile[k_hiz_threads * k_hiz_threads];

uint hiz_extent(uint extent, uint mip) { return (extent + (1u << mip) - 1u) >> mip; }

[shader("compute")]
[numthreads(16, 16, 1)]
void hiz_build_main(uint3 group_id : SV_GroupID, uint3 local_id : SV_GroupThreadID) {
  const uint lx = local_id.x;
  const uint ly = local_id.y;
  float* pyramid = (float*)g_params.pyramid;

  uint src_w = hiz_extent(g_params.width, g_params.src_mip);
  uint src_h = hiz_extent(g_params.height, g_params.src_mip);
  uint src_off = g_params.src_offset;
  uint dst_w = (src_w + 1u) >> 1;
  uint dst_h = (src_h + 1u) >> 1;
  uint dst_off = src_off + src_w * src_h;

  // The first fold: this thread's 2x2 of the source mip, at the destination texel it produces.
  const uint dx = group_id.x * k_hiz_threads + lx;
  const uint dy = group_id.y * k_hiz_threads + ly;
  float value = k_hiz_far;
  if (dx < dst_w && dy < dst_h) {
    const uint x0 = dx * 2;
    const uint y0 = dy * 2;
    const uint x1 = min(x0 + 1, src_w - 1);
    const uint y1 = min(y0 + 1, src_h - 1);
    float a;
    float b;
    float c;
    float d;
    if (g_params.from_visibility != 0) {
      uint64_t* visibility = (uint64_t*)g_params.src;
      a = asfloat(uint(visibility[y0 * src_w + x0] >> 32));
      b = asfloat(uint(visibility[y0 * src_w + x1] >> 32));
      c = asfloat(uint(visibility[y1 * src_w + x0] >> 32));
      d = asfloat(uint(visibility[y1 * src_w + x1] >> 32));
      // Mip `src_mip` is that depth word. Every texel of it is the x0 or the x1 of exactly one
      // destination texel, so these four writes cover the mip; a clamped duplicate writes the
      // same value to the same place twice.
      pyramid[src_off + y0 * src_w + x0] = a;
      pyramid[src_off + y0 * src_w + x1] = b;
      pyramid[src_off + y1 * src_w + x0] = c;
      pyramid[src_off + y1 * src_w + x1] = d;
    } else {
      float* src = (float*)g_params.src;
      a = src[y0 * src_w + x0];
      b = src[y0 * src_w + x1];
      c = src[y1 * src_w + x0];
      d = src[y1 * src_w + x1];
    }
    value = min(min(a, b), min(c, d));
    if (g_params.levels != 0) pyramid[dst_off + dy * dst_w + dx] = value;
  }

  g_tile[ly * k_hiz_threads + lx] = value;
  GroupMemoryBarrierWithGroupSync();
  // The remaining folds, entirely in shared memory: the tile halves each time and so does the
  // number of threads that have anything to do.
  uint side = k_hiz_threads / 2;
  for (uint level = 2; level <= g_params.levels; ++level) {
    src_w = dst_w;
    src_h = dst_h;
    src_off = dst_off;
    dst_w = (src_w + 1u) >> 1;
    dst_h = (src_h + 1u) >> 1;
    dst_off = src_off + src_w * src_h;
    const bool active = lx < side && ly < side;
    float folded = k_hiz_far;
    if (active) {
      const uint base = (2 * ly) * k_hiz_threads + 2 * lx;
      folded = min(min(g_tile[base], g_tile[base + 1]),
                   min(g_tile[base + k_hiz_threads], g_tile[base + k_hiz_threads + 1]));
      const uint ex = group_id.x * side + lx;
      const uint ey = group_id.y * side + ly;
      if (ex < dst_w && ey < dst_h) pyramid[dst_off + ey * dst_w + ex] = folded;
    }
    // Read before write: a thread's own slot is another thread's source at this level.
    GroupMemoryBarrierWithGroupSync();
    if (active) g_tile[ly * k_hiz_threads + lx] = folded;
    GroupMemoryBarrierWithGroupSync();
    side >>= 1;
  }
}
