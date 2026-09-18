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

// Mirrors gfx::MeshDesc (64 bytes): one per mesh of the scene. The last four addresses are
// where a deformed instance's positions come from instead of the grid.
struct MeshDesc {
  float4 quant;        // xyz grid origin, w grid step: this mesh's own grid
  uint64_t quantized;  // uint16_t[3 * vertex_count] of the whole scene, padded to an even count
  uint first_cluster;  // this mesh's range of the global cluster array
  uint cluster_count;
  uint64_t deform_pool;   // float[3 * pool_vertices]: the frame's deformed-vertex pool, or 0
  uint64_t deform_slots;  // uint[] per visible entry: its pool vertex base, or k_no_pool_slot
  uint64_t templates;     // uint64_t[]: a cluster template address per global cluster index, or 0
  uint64_t skin;          // SkinBinding[]: eight bytes per scene-wide vertex, or 0
};

// Mirrors gfx::InstanceDesc (96 bytes): one per instance, in order of first_pair.
struct InstanceDesc {
  column_major float4x4 world;  // mesh space to world
  uint mesh;
  uint material_base;  // added to the cluster's material index in the resolve
  uint first_pair;
  float scale_max;  // largest axis scale: radii and LOD errors multiply by it
  uint flags;       // bit 0: uniform scale
  uint deform;      // entry of the DeformDesc table; k_invalid_deform: rigid
  // Mesh-space slack the cull pass adds to every sphere of this instance, so a deformed instance
  // whose vertices leave their rest positions is still culled conservatively. Zero for a rigid
  // one, and a float plus zero is exact, so the rigid arithmetic is unchanged.
  float bounds_padding;
  uint pad;
};

// Mirrors gfx::DeformDesc (16 bytes): one per deformed instance, read by the pool pass alone.
// `joints` is per instance and `MeshDesc::skin` is per mesh, which is the split a crowd needs:
// one binding stream, one array of bone matrices per character. Where in the pool the instance
// writes is **not** here — the pool is suballocated per frame per visible entry, so that address
// is `MeshDesc::deform_slots[entry]`.
struct DeformDesc {
  uint flags;        // which deformer: k_deform_*
  uint joint_count;  // the bone-matrix array's length; 0 leaves a skinned instance at rest
  uint64_t joints;   // JointMatrix[joint_count], or 0
};

// Mirrors geometry::SkinBinding (8 bytes): four u8 joint indices then four u8 weights summing to
// 255. Read as two 32-bit words, because a byte array through a device address would be four
// loads where the hardware wants one.
struct SkinBinding {
  uint packed_joints;
  uint packed_weights;
};

// Mirrors anim::JointMatrix (48 bytes): the affine 3x4 of model * inverse_bind, three float4
// rows, so skinning a position is three dot products with float4(p, 1) and the fourth row that
// is always (0, 0, 0, 1) is never stored or loaded.
struct JointMatrix {
  float4 rows[3];
};

// ---- geometry streaming (docs/plan/04-renderer.md §4.3 step 3 and §4.9) -----------------------
//
// A scene's clusters are laid out in fixed-size **pages**, and a viewer may hold only some of
// them. Three tables travel with the scene and stay resident whatever the budget, because the
// cull pass tests every pair of every frame and has to be able to say "not that one" about a
// cluster whose payload is not here: the page table itself, the page each cluster is in, and the
// clusters each cluster refines into. They are 12 bytes a cluster plus 48 bytes a page against
// the ~1.5 KB a cluster the page holds, which is the ratio that makes the split worth making.

// Mirrors geometry::ClusterPageDesc (48 bytes). The cull pass reads none of it today — the
// drawing rule needs only `page_of_cluster` and the residency word — but the record is the
// streaming contract the way ClusterDesc is the geometry one, so it is written down here once,
// beside the tables that index it, rather than in whichever pass first needs `level_min`.
struct ClusterPageDesc {
  uint first_cluster;
  uint cluster_count;
  uint first_vertex;
  uint vertex_count;
  uint first_triangle;
  uint triangle_count;
  uint level_min;
  uint level_max;
  uint first_child_page;
  uint child_page_count;
  uint bytes;
  uint flags;
};

static const uint k_page_root = 1u;      // nothing coarser can replace it: pinned, never evicted
static const uint k_page_oversized = 2u;  // one group did not fit in a page of its own

// Mirrors geometry::ClusterChildren (two uint): the run of clusters one cluster refines into,
// which is the group it was simplified from and which the page builder keeps contiguous. A
// level-0 cluster is original geometry and has `cluster_count` zero.
struct ClusterChildren {
  uint first_cluster;
  uint cluster_count;
};

// Mirrors geometry::PageRequest (12 bytes): what one missing page is worth to the cluster that
// could not refine into it. The CPU heap reads these words without a conversion.
struct PageRequest {
  uint page;
  float screen_px;  // the projected diameter of the requesting cluster's sphere, in pixels
  float distance;   // observer to the near side of that sphere
};

static const uint k_instance_uniform_scale = 1u;
static const uint k_invalid_deform = 0xffffffffu;
// gfx::k_no_pool_slot: this visible entry has no block in the frame's pool, so every reader takes
// its rest pose. That is the *only* overflow behaviour there is — never garbage, never a crash.
static const uint k_no_pool_slot = 0xffffffffu;
static const uint k_deform_identity = 0u;
static const uint k_deform_wave = 1u;
static const uint k_deform_lattice = 2u;
static const uint k_deform_skin = 3u;
static const uint k_deform_kind_mask = 3u;

// The rest pose: three u16 at 3 * vertex of the mesh's grid, read as the 32-bit words that
// contain them. The stream is padded to an even u16 count on the CPU, so the last word read is
// always inside the buffer. `vertex` is the scene-wide vertex index (vertex_offset + local).
float3 load_quantized_position(MeshDesc mesh, uint vertex) {
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

// Where this frame put the deformed vertices of visible entry `entry`, as a **pool vertex
// index**, or k_no_pool_slot when the instance is rigid, the frame has no pool, or the entry did
// not fit the budget. Resolved once per cluster by every reader — a workgroup, a draw instance,
// a resolve pixel's triangle — and then handed to `load_position` for each of its vertices.
//
// `entry` is the index into the **whole** visible list (the visibility id's high bits), not into
// one run of it, because the table is one array over the frame's three runs and every view.
uint deform_base(MeshDesc mesh, InstanceDesc instance, uint entry) {
  if (instance.deform == k_invalid_deform || mesh.deform_slots == 0) return k_no_pool_slot;
  uint* slots = (uint*)mesh.deform_slots;
  return slots[entry];
}

// True when this instance's positions could come from the pool. It says nothing about whether
// this frame found room for a particular cluster; `deform_base` answers that.
bool is_deformed(MeshDesc mesh, InstanceDesc instance) {
  return instance.deform != k_invalid_deform && mesh.deform_slots != 0;
}

// **Every** position read of a scene goes through this (docs/plan/04-renderer.md §4.3). `base` is
// what `deform_base` answered for the entry being drawn and `first` is the cluster's
// `vertex_offset`, so the pool slot is the vertex's position *within its cluster* — which is what
// a per-frame suballocation makes it. `k_no_pool_slot` is the rest pose off the mesh's 16-bit
// grid, and it covers three cases with one branch: a rigid instance, a frame with no pool, and a
// deformed cluster the pool budget had no room for.
float3 load_position(MeshDesc mesh, uint base, uint first, uint local) {
  if (base != k_no_pool_slot) {
    float* pool = (float*)mesh.deform_pool;
    const uint slot = (base + local) * 3;
    return float3(pool[slot], pool[slot + 1], pool[slot + 2]);
  }
  return load_quantized_position(mesh, first + local);
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
