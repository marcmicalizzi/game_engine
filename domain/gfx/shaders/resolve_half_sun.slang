// **The half-rate sun pass** (docs/subsystems/renderer.md, "A shading rate by region"; roadmap
// R126): the sun's shadow ray once per 2x2 block of a view's visibility region in its half-rate
// columns, written for the resolve to take instead of tracing four.
//
// Why a pass and not a lane trick: the resolve is a fragment pass and a fragment quad is already a
// 2x2 block, but masking three lanes of a quad saves no warp time — the warp runs the ray for the
// lane that traces, and its traversal is the cost. The saving has to come from fewer warps, so the
// rays are asked by a quarter-size dispatch of their own, between the visibility buffer and the
// resolve, and the resolve reads one word a pixel where it traced one ray.
//
// **The block rule.** A block is taken only where its four visibility words hold one pair — one
// (instance, cluster), and so one material: a pair's material is its cluster's index plus its
// instance's base, so equal pairs are equal materials — and none of them is empty. A silhouette, a
// mask's edge (the pixel the mask cut shows the pair behind it) and a material's boundary are a
// second pair in the block, so they stay per pixel by construction. The representative is the
// **top-left pixel**: its point is reconstructed at its own centre exactly as the resolve
// reconstructs it, and its ray is the one `shadow_ray_blocked` asks for that pixel — the same
// function, the same flags and the same mask loop, because this file includes the resolve's — so
// the top-left pixel draws what it drew at full rate and the other three take its answer. The block
// centre was the alternative; it would centre the shadow's edge by half a pixel, but it lies on no
// pixel's triangle, so its point is the top-left triangle's plane extrapolated, which on a crease
// of the same pair is a point on no surface.
//
// A block is not taken (`k_half_sun_not_taken`, and its pixels trace as before) where the view
// asks no sun — a mode other than the shaded picture and the shadow view, no sun bit, no scene —
// where the surface faces away from the sun by both its interpolated and its geometric normal (the
// resolve does not ask there unless a normal map turns it back, and then that pixel traces), and,
// under rays near and maps far, where the maps answer the whole query.

#define RESOLVE_RAY_SHADOWS 1
#define RESOLVE_HALF_PASS 1
#include "visibility_resolve.slang"

// The block's answer: the pair << 1 | blocked, or k_half_sun_not_taken.
uint half_sun_block(ResolveParams* params, uint2 block) {
  const uint2 p = block * 2u;
  if (p.x + 1u >= params->width || p.y + 1u >= params->height) return k_half_sun_not_taken;
  if (params->mode != 3u && params->mode != 6u) return k_half_sun_not_taken;
  if ((params->shadow_flags & k_shadow_sun) == 0 || params->scene == k_no_scene) {
    return k_half_sun_not_taken;
  }
  // Under the cascaded maps alone the sun is not traced at all.
  if ((params->shadow_flags & (k_shadow_cascades | k_shadow_hybrid)) == k_shadow_cascades) {
    return k_half_sun_not_taken;
  }
  // A 32 x 32 tile the Hi-Z build found empty holds no surface (fs_resolve, the coverage mask).
  if (params->coverage != 0) {
    uint* tiles = (uint*)params->coverage;
    if (tiles[(p.y >> 5) * params->coverage_pitch + (p.x >> 5)] == 0) return k_half_sun_not_taken;
  }
  uint64_t* visibility = (uint64_t*)params->visibility;
  const uint row = p.y * params->width + p.x;
  const uint64_t v00 = visibility[row];
  const uint64_t v10 = visibility[row + 1u];
  const uint64_t v01 = visibility[row + params->width];
  const uint64_t v11 = visibility[row + params->width + 1u];
  if (v00 == 0 || v10 == 0 || v01 == 0 || v11 == 0) return k_half_sun_not_taken;
  const uint id = uint(v00 & 0xffffffffu);
  const uint pair = id >> 8;
  if ((uint(v10 & 0xffffffffu) >> 8) != pair || (uint(v01 & 0xffffffffu) >> 8) != pair ||
      (uint(v11 & 0xffffffffu) >> 8) != pair) {
    return k_half_sun_not_taken;
  }
  // The top-left pixel, decoded and reconstructed as fs_resolve does it.
  uint2 entry;
  if (params->pairs != 0) {
    entry = ((uint2*)params->pairs)[pair];
  } else {
    entry = visible_entry(params->visible, entry_of_pair(params->pair_entries, pair));
  }
  const SceneGeometry geo = geometry_of(params);
  InstanceDesc* instances = (InstanceDesc*)params->instances;
  const InstanceDesc instance = instances[entry.x];
  const uint visible_index =
      instance.deform == k_invalid_deform ? 0u : entry_of_pair(params->pair_entries, pair);
  float3 from_eye;
  const float2 source = float2(p) + 0.5;
  const Surface s = reconstruct_screen(geo, instance, visible_index, entry.y, id & 0xff, source,
                                       params->view_proj, params->camera.xyz, params->width,
                                       params->height, from_eye);
  const float3 sun = params->sun.xyz;
  if (dot(s.normal, sun) <= 0.0 && dot(s.geometric, sun) <= 0.0) return k_half_sun_not_taken;
  if ((params->shadow_flags & k_shadow_hybrid) != 0 && hybrid_weight(params, s.position) >= 1.0) {
    return k_half_sun_not_taken;
  }
  const float ray_bias = ray_offset(geo, instance, params->shadow_bias, params->shadow_bias_steps);
  g_fade_pixel = p;
  const float3 origin = s.position + s.geometric * ray_bias;
  const bool blocked = shadow_ray_blocked(params, k_shadow_sun, origin, sun, 1e30);
  return (pair << 1) | (blocked ? 1u : 0u);
}

// One thread a block of the half-rate columns: the view's left side, then its right, packed into
// one dispatch `gfx::HalfRateWindow::columns` blocks wide.
[shader("compute")]
[numthreads(8, 8, 1)]
void half_sun_main(uint3 id : SV_DispatchThreadID) {
  ResolveParams* params = (ResolveParams*)g_push.params;
  const uint blocks_x = (params->width + 1u) >> 1;
  const uint blocks_y = (params->height + 1u) >> 1;
  const uint b0 = params->half_x0 >> 1;
  const uint b1 = params->half_x1 >> 1;
  const uint columns = b0 + (blocks_x - b1);
  if (id.x >= columns || id.y >= blocks_y) return;
  const uint bx = id.x < b0 ? id.x : b1 + (id.x - b0);
  uint* answers = (uint*)params->half_sun;
  answers[id.y * blocks_x + bx] = half_sun_block(params, uint2(bx, id.y));
}
