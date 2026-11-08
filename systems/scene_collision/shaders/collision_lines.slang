// The collision debug draw (docs/subsystems/scene_collision.md, "Seeing the collision";
// collision_draw.h): the held tiles' proxy triangles, their ground's lattice and the walker's
// capsule as lines, drawn by a compute pass into the frame's linear colour image after the resolve
// (systems/renderer/frame_pass.h), depth-tested against the visibility buffer's depth.
//
// One thread a line and a view: the segment is clipped to the near side in clip space, projected,
// clipped to the view's rectangle, and walked a pixel at a time; a pixel is written where the line
// is at or in front of what the frame drew there, less a relative tolerance (`depth_tolerance`) so a
// line lying on the surface it outlines is not lost to the surface's own depth. Reversed-Z: a larger
// depth is nearer, and 0 is nothing drawn (the sky). No absolute world position: the lines are in the
// frame's space, metres from the eye (ADR-0053), made on the CPU from f64.

[[vk::binding(1, 0)]] RWTexture2D<float4> g_storage_images[];

// Mirrors scene_collision::DrawLine (32 bytes).
struct Line {
  float3 a;
  uint color;  // rgba8, r in the low byte
  float3 b;
  uint pad;
};

// Mirrors CollisionDraw's per-view block (collision_draw.cpp, `ViewBlock`, 128 bytes).
struct ViewBlock {
  column_major float4x4 view_proj;  // core/math's Mat4: clip = mul(view_proj, float4(p, 1))
  uint4 rect;          // the view's rectangle of the colour image
  uint4 source;        // x, y its region of the visibility buffer, z the colour's storage slot, w lines
  uint64_t visibility;
  uint64_t lines;
  float depth_tolerance;
  uint max_steps;
  uint pad[2];
};

struct Push {
  uint64_t block;
};
[[vk::push_constant]] ConstantBuffer<Push> g_push;

static const float k_near_w = 1.0e-3;

float4 unpack_color(uint c) {
  return float4(float(c & 0xFFu), float((c >> 8) & 0xFFu), float((c >> 16) & 0xFFu), 255.0) /
         255.0;
}

[numthreads(64, 1, 1)]
void collision_lines_main(uint3 id : SV_DispatchThreadID) {
  ViewBlock* v = (ViewBlock*)g_push.block;
  if (id.x >= v->source.w) return;
  const Line line = ((Line*)v->lines)[id.x];
  float4 ca = mul(v->view_proj, float4(line.a, 1.0));
  float4 cb = mul(v->view_proj, float4(line.b, 1.0));
  // The near side: w above a millimetre.
  if (ca.w < k_near_w && cb.w < k_near_w) return;
  if (ca.w < k_near_w) ca = lerp(ca, cb, (k_near_w - ca.w) / (cb.w - ca.w));
  if (cb.w < k_near_w) cb = lerp(cb, ca, (k_near_w - cb.w) / (ca.w - cb.w));
  const float2 size = float2(float(v->rect.z), float(v->rect.w));
  // Pixels, y down; depth z / w, which is affine along the segment on the screen.
  float2 pa = float2((ca.x / ca.w * 0.5 + 0.5) * size.x, (0.5 - ca.y / ca.w * 0.5) * size.y);
  float2 pb = float2((cb.x / cb.w * 0.5 + 0.5) * size.x, (0.5 - cb.y / cb.w * 0.5) * size.y);
  float za = ca.z / ca.w;
  float zb = cb.z / cb.w;
  // The rectangle: Liang-Barsky on the screen segment.
  float t0 = 0.0;
  float t1 = 1.0;
  const float2 d = pb - pa;
  const float p[4] = {-d.x, d.x, -d.y, d.y};
  const float q[4] = {pa.x, size.x - pa.x, pa.y, size.y - pa.y};
  [unroll]
  for (int k = 0; k < 4; ++k) {
    if (p[k] == 0.0) {
      if (q[k] < 0.0) return;
    } else {
      const float r = q[k] / p[k];
      if (p[k] < 0.0) t0 = max(t0, r);
      else t1 = min(t1, r);
    }
  }
  if (t0 > t1) return;
  const float2 qa = pa + d * t0;
  const float2 qb = pa + d * t1;
  const float zqa = lerp(za, zb, t0);
  const float zqb = lerp(za, zb, t1);
  const float steps_f = ceil(max(abs(qb.x - qa.x), abs(qb.y - qa.y)));
  const uint steps = min(uint(steps_f) + 1u, v->max_steps);
  const float4 color = unpack_color(line.color);
  uint64_t* vis = (uint64_t*)v->visibility;
  for (uint s = 0; s < steps; ++s) {
    const float t = steps > 1u ? float(s) / float(steps - 1u) : 0.0;
    const float2 at = lerp(qa, qb, t);
    const int2 px = int2(floor(at));
    if (px.x < 0 || px.y < 0 || px.x >= int(v->rect.z) || px.y >= int(v->rect.w)) continue;
    const float z = lerp(zqa, zqb, t);
    const uint64_t word = vis[uint(px.y) * v->source.x + uint(px.x)];
    const float drawn = asfloat(uint(word >> 32));
    if (z < drawn * (1.0 - v->depth_tolerance)) continue;  // behind what the frame drew
    g_storage_images[v->source.z][uint2(v->rect.x + uint(px.x), v->rect.y + uint(px.y))] = color;
  }
}
