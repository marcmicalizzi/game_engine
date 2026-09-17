// The GPU-resident scene, mirrored once for every shader that reads it (docs/plan/04-renderer.md
// §4.2). No bindings and no entry points: the cull pass, the three rasterizers, the resolve, and
// the cluster acceleration structure records all include this file, so the structs the CPU pins
// with size tables are written down here exactly once instead of in each shader. A shader that
// includes it names it in the `DEPENDS` of its `engine_shaders()`, the way brdf.slang is named.
//
// A scene is instances of meshes. Every mesh lives in one set of global buffers with the offsets
// baked into each ClusterDesc; a MeshDesc says which clusters a mesh owns and which 16-bit
// position grid it is on, and an InstanceDesc places one mesh in the world. The unit of work is
// the **pair** (instance, cluster): instance i owns its mesh's clusters starting at
// `first_pair`, a prefix sum over the instances in order. The visible list holds one
// `uint2 {instance, cluster}` per surviving pair, and the visibility buffer's id is
// `visible_index << 8 | triangle`.

#ifndef ENGINE_SCENE_SLANG
#define ENGINE_SCENE_SLANG

// Mirrors geometry::ClusterDesc (48 bytes).
struct ClusterDesc {
  uint vertex_offset;
  uint triangle_offset;
  uint vertex_count;
  uint triangle_count;
  float3 center;
  float radius;
  float3 cone_apex;  // normal cone for backface culling (cull pass only)
  uint cone;         // axis snorm8 x, y, z in bytes 0..2, cutoff snorm8 in byte 3
};

// Mirrors geometry::VertexAttributes (8 bytes).
struct VertexAttributes {
  uint normal_oct;
  uint uv_half2;
};

// Mirrors gfx::MeshDesc (32 bytes): one per mesh of the scene.
struct MeshDesc {
  float4 quant;        // xyz grid origin, w grid step: this mesh's own grid
  uint64_t quantized;  // uint16_t[3 * vertex_count] of the whole scene, padded to an even count
  uint first_cluster;  // this mesh's range of the global cluster array
  uint cluster_count;
};

// Mirrors gfx::InstanceDesc (96 bytes): one per instance, in order of first_pair.
struct InstanceDesc {
  column_major float4x4 world;  // mesh space to world
  uint mesh;
  uint material_base;  // added to the cluster's material index in the resolve
  uint first_pair;
  float scale_max;  // largest axis scale: radii and LOD errors multiply by it
  uint flags;       // bit 0: uniform scale
  uint3 pad;
};

static const uint k_instance_uniform_scale = 1u;

// Three u16 at 3 * vertex of the mesh's grid, read as the 32-bit words that contain them. The
// stream is padded to an even u16 count on the CPU, so the last word read is always inside the
// buffer. `vertex` is the scene-wide vertex index (ClusterDesc::vertex_offset + local).
float3 load_position(MeshDesc mesh, uint vertex) {
  uint* words = (uint*)mesh.quantized;
  const uint i = vertex * 3;
  const uint w0 = words[i >> 1];
  const uint w1 = words[(i + 1) >> 1];
  const uint w2 = words[(i + 2) >> 1];
  const uint x = (i & 1) != 0 ? (w0 >> 16) : (w0 & 0xffff);
  const uint y = ((i + 1) & 1) != 0 ? (w1 >> 16) : (w1 & 0xffff);
  const uint z = ((i + 2) & 1) != 0 ? (w2 >> 16) : (w2 & 0xffff);
  return mesh.quant.xyz + float3(float(x), float(y), float(z)) * mesh.quant.w;
}

// Entry `index` of the visible list. A null list is the identity scene the direct draws use:
// instance 0 and the cluster in index order, which is what a single-mesh draw without culling
// wants and what keeps a visibility id meaningful in both cases.
uint2 visible_entry(uint64_t visible, uint index) {
  if (visible == 0) return uint2(0, index);
  uint2* entries = (uint2*)visible;
  return entries[index];
}

// The instance that owns pair `pair`: the last one whose first_pair is at or below it. The
// instances are in order, so this is a plain binary search over the prefix sum.
uint instance_of_pair(InstanceDesc* instances, uint instance_count, uint pair) {
  uint lo = 0;
  uint hi = instance_count - 1;
  while (lo < hi) {
    const uint mid = (lo + hi + 1) >> 1;
    if (instances[mid].first_pair <= pair) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

// A direction in the instance's frame, for normals. The cofactor matrix of the upper-left 3x3
// (its columns are the cross products of the world's columns) is the adjugate transpose, which
// is the inverse transpose scaled by the determinant: the same direction for any positive scale
// and the correctly flipped one for a mirror, so normalizing afterwards makes the division
// unnecessary and the uniform-scale case needs no special path.
float3 transform_normal(column_major float4x4 world, float3 n) {
  const float3 c0 = float3(world[0][0], world[1][0], world[2][0]);
  const float3 c1 = float3(world[0][1], world[1][1], world[2][1]);
  const float3 c2 = float3(world[0][2], world[1][2], world[2][2]);
  return normalize(cross(c1, c2) * n.x + cross(c2, c0) * n.y + cross(c0, c1) * n.z);
}

#endif  // ENGINE_SCENE_SLANG
