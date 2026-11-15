// The motion pass (docs/subsystems/gfx.md, "Motion from the visibility buffer"; ADR-0064; R154):
// one thread a pixel over the visibility buffer, writing the pixels each pixel's surface point
// moved across the picture since the last frame, as two half floats, with neither frame's sub-pixel
// offset in it. `domain/gfx/motion.h` is the C++ side and says what each kind of pixel reads.
//
// No rasterizer writes a velocity: a pixel holds `depth << 32 | pair << 8 | triangle` and nothing
// else, the ray path has no fragment stage, and the resolve by pixel class would otherwise write it
// from sixteen modules. So motion is its own pass, between the visibility passes and the resolve.

#include "scene.slang"

// Mirrors gfx::MovedInstance (64 bytes).
struct MovedInstance {
  uint instance;
  int3 cell;
  float4 rows[3];
};

// Mirrors gfx::MotionParams (416 bytes).
struct MotionParams {
  column_major float4x4 reproject;
  column_major float4x4 unproject;
  column_major float4x4 prev_view_proj;
  FrameEye eye;
  FrameEye prev_eye;
  uint64_t visibility;
  uint64_t prev_visibility;
  uint64_t pairs;
  uint64_t pair_entries;
  uint64_t visible;
  uint64_t instances;
  uint64_t meshes;
  uint64_t clusters;
  uint64_t triangles;
  uint64_t output;
  uint64_t moved;
  uint64_t prev_pool;
  uint width;
  uint height;
  uint instance_count;
  uint moved_count;
  float2 offset_ndc;
  uint2 pad;
};

struct MotionPush {
  uint64_t params;  // MotionParams*: this view's block
};
[[vk::push_constant]] ConstantBuffer<MotionPush> g_push;

static const uint k_motion_invalid = 0x7c007c00u;  // gfx::k_motion_invalid

// The moved table's entry for `instance`, or -1: a binary search over the sorted table, run only in
// a frame whose table is not empty.
int find_moved(MotionParams* p, uint instance) {
  MovedInstance* table = (MovedInstance*)p->moved;
  uint lo = 0;
  uint hi = p->moved_count;
  while (lo < hi) {
    const uint mid = (lo + hi) >> 1;
    const uint at = table[mid].instance;
    if (at == instance) return int(mid);
    if (at < instance) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return -1;
}

// A point of the frame's space, less the instance's translation from the eye, back in mesh space:
// the inverse of the instance's linear part by its adjugate. False for a degenerate instance.
bool to_mesh(InstanceDesc instance, float3 v, out float3 local) {
  const float3 r0 = instance.rows[0].xyz;
  const float3 r1 = instance.rows[1].xyz;
  const float3 r2 = instance.rows[2].xyz;
  const float3 c0 = cross(r1, r2);
  const float3 c1 = cross(r2, r0);
  const float3 c2 = cross(r0, r1);
  const float det = dot(r0, c0);
  local = float3(0.0, 0.0, 0.0);
  if (abs(det) < 1e-30) return false;
  // The inverse's columns are the cofactor rows over the determinant.
  local = (c0 * v.x + c1 * v.y + c2 * v.z) / det;
  return true;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void motion_main(uint3 id : SV_DispatchThreadID) {
  MotionParams* p = (MotionParams*)g_push.params;
  if (id.x >= p->width || id.y >= p->height) return;
  const uint index = id.y * p->width + id.x;
  const uint64_t word = ((uint64_t*)p->visibility)[index];
  const float depth = asfloat(uint(word >> 32));
  const float2 ndc = float2((float(id.x) + 0.5) / float(p->width) * 2.0 - 1.0,
                            1.0 - (float(id.y) + 0.5) / float(p->height) * 2.0);
  // The pixel's own point in this frame's clip space, as it was drawn. Depth zero (an empty pixel)
  // is a point at infinity, which the eyes' difference inside `reproject` does not move: the sky
  // turns with the camera and nothing else.
  const float4 clip = float4(ndc, depth, 1.0);
  float4 prev = mul(p->reproject, clip);
  if (word != 0) {
    const uint id_word = uint(word & 0xffffffffu);
    const uint pair = id_word >> 8;
    uint2 entry;
    if (p->pairs != 0) {
      entry = ((uint2*)p->pairs)[pair];
    } else {
      entry = visible_entry(p->visible, entry_of_pair(p->pair_entries, pair));
    }
    const InstanceDesc instance = ((InstanceDesc*)p->instances)[entry.x];
    const MeshDesc mesh = ((MeshDesc*)p->meshes)[instance.mesh];
    const bool deformed = p->prev_pool != 0 && is_deformed(mesh, instance);
    int moved = -1;
    if (p->moved_count != 0) moved = find_moved(p, entry.x);
    if (deformed || moved >= 0) {
      const float4 here = mul(p->unproject, clip);
      float3 local;
      if (here.w != 0.0 &&
          to_mesh(instance, here.xyz / here.w - instance_from_eye(instance, p->eye), local)) {
        if (deformed) {
          // The triangle's corners as this frame deformed them and as last frame's inputs did, in
          // the same block of the two pools; the point keeps its barycentrics among them. A cluster
          // the pool had no room for drew its rest pose in both frames and keeps `local`.
          const uint base = deform_base(mesh, instance, entry_of_pair(p->pair_entries, pair));
          if (base != k_no_pool_slot) {
            const ClusterDesc cluster = ((ClusterDesc*)p->clusters)[entry.y];
            const uint packed = ((uint*)p->triangles)[cluster.triangle_offset + (id_word & 0xffu)];
            float* now = (float*)mesh.deform_pool;
            float* before = (float*)p->prev_pool;
            float3 a[3];
            float3 b[3];
            for (uint i = 0; i < 3; ++i) {
              const uint slot = (base + ((packed >> (8 * i)) & 0xffu)) * 3;
              a[i] = float3(now[slot], now[slot + 1], now[slot + 2]);
              b[i] = float3(before[slot], before[slot + 1], before[slot + 2]);
            }
            const float3 e1 = a[1] - a[0];
            const float3 e2 = a[2] - a[0];
            const float3 d = local - a[0];
            const float d11 = dot(e1, e1);
            const float d12 = dot(e1, e2);
            const float d22 = dot(e2, e2);
            const float denom = d11 * d22 - d12 * d12;
            if (denom > 0.0) {
              const float v = (d22 * dot(d, e1) - d12 * dot(d, e2)) / denom;
              const float w = (d11 * dot(d, e2) - d12 * dot(d, e1)) / denom;
              local = b[0] * (1.0 - v - w) + b[1] * v + b[2] * w;
            }
          }
        }
        InstanceDesc was = instance;
        if (moved >= 0) {
          const MovedInstance m = ((MovedInstance*)p->moved)[moved];
          was.cell = m.cell;
          was.rows[0] = m.rows[0];
          was.rows[1] = m.rows[1];
          was.rows[2] = m.rows[2];
        }
        const float3 point = instance_point(was, instance_from_eye(was, p->prev_eye), local);
        prev = mul(p->prev_view_proj, float4(point, 1.0));
      }
    }
  }
  uint packed_motion = k_motion_invalid;
  if (prev.w > 0.0) {
    const float2 motion = (ndc + p->offset_ndc - prev.xy / prev.w) *
                          float2(0.5 * float(p->width), -0.5 * float(p->height));
    packed_motion = f32tof16(motion.x) | (f32tof16(motion.y) << 16);
  }
  ((uint*)p->output)[index] = packed_motion;
}
