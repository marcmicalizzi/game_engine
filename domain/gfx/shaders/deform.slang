// The deformed-vertex pool (docs/plan/04-renderer.md §4.3, ADR-0026 decision 7, experiment E25):
// one compute pass per frame that writes deformed positions for **the clusters of the current LOD
// cut only**, so deformation costs the cut and not the source mesh. One 128-thread workgroup per
// entry of one run of the cull pass's visible list, dispatched indirectly from that run's own
// count word, so no CPU ever sees how big the cut is.
//
// A thread reads the cluster's rest-pose vertex off the mesh's 16-bit grid, applies the
// instance's deformer, and writes `pool[base + local]` — the block `deform_alloc.slang` gave
// **this visible entry** before this pass ran, as long as this cluster's vertex count and no
// longer. The pool is therefore sized by the frame's cut and a budget rather than by the
// population. Entries of rigid instances are skipped outright, and so is an entry the budget had
// no room for: its `deform_slots` word is `k_no_pool_slot`, and every reader takes its rest pose.
//
// Three deformers, one real and two stand-ins. §4.3 says skinning reads bone matrices and a
// deformable volume reads the tick's cage state; both land in the same pool through the same
// pass, and skinning is the one that exists:
//
//   skin     linear blend skinning (docs/plan/05-simulation.md §5.11). Reads the mesh's
//            `geometry::SkinBinding` stream — four u8 joint indices and four u8 weights summing
//            to 255 — and the *instance's* `anim::JointMatrix` array, and writes
//            sum_k (w_k / 255) * (M_k * rest). The CPU reference is `anim::skin_positions`, and
//            the gfx skinning test holds the two to 1e-5 on a two-bone cylinder.
//
//            **Skinning cannot tear**, and the reason is worth stating beside the code that
//            could: a skinned position is a function of the rest position and the binding, and
//            both are per-vertex data the cluster build hands every copy of a surface point
//            byte for byte — duplicates across clusters share them, and a simplified level
//            inherits them from the original vertex rather than recomputing them, because
//            clusterlod keeps original vertices. That is the same argument that makes
//            `lattice` safe and `wave` unsafe, and it is why skinning is position-only: it
//            never reads the normal.
//
//   wave     sinusoidal displacement along the **vertex normal**, animated by frame time. It is
//            here as the counter-example: a normal is *not* a quantity the cluster build
//            guarantees every copy of a surface point shares. Welded duplicates at a hard edge
//            carry different normals, and a simplified LOD level recomputes them even where the
//            DAG locks the positions, so displacing along one tears the surface open at hard
//            edges and at LOD-group boundaries (experiment E25 compares the two). Nothing but a
//            spike should displace along a normal.
//   lattice  a 3x3x3 trilinear free-form-deformation cage over the mesh's own quantization box
//            (which is a bounding cube of the mesh, so the cage needs no authored bounds), whose
//            27 control points move procedurally. This is the stand-in for a soft-body cage, and
//            it is **a function of the rest position alone**, which is the property that keeps it
//            crack-free: every copy of a surface point has the same position on every level, so
//            every copy moves the same way. A real cage transfer has to keep that property.

#include "scene.slang"

// Mirrors gfx::DeformParams (88 bytes).
struct DeformParams {
  uint64_t clusters;      // ClusterDesc[]
  uint64_t instances;     // InstanceDesc[]
  uint64_t meshes;        // MeshDesc[]
  uint64_t attributes;    // VertexAttributes[]; 0: displace radially instead of along the normal
  uint64_t visible;       // uint2[]: one run of the visible list, {instance, cluster} per entry
  uint64_t visible_count; // uint: that run's count word
  uint64_t pool;          // float[3 * pool_vertices] out
  uint64_t deform;        // DeformDesc[]
  uint64_t slots;         // uint[]: the whole list's per-entry pool bases
  float time;             // animation phase in seconds
  float amplitude;        // displacement scale as a fraction of the mesh's grid box
  uint max_entries;       // the run's capacity; the device count is clamped to it
  uint visible_offset;    // this run's first entry in the whole visible list
};

[[vk::push_constant]] ConstantBuffer<DeformParams> g_params;

static const float k_tau = 6.2831853071795864769;

// Inverse of geometry::encode_normal_oct: two snorm16 in a uint. Kept here rather than shared
// with the resolve so that the pool pass depends on nothing but scene.slang.
float3 decode_oct(uint packed) {
  const float x = float(int(packed << 16) >> 16) / 32767.0;
  const float y = float(int(packed) >> 16) / 32767.0;
  float3 n = float3(x, y, 1.0 - abs(x) - abs(y));
  if (n.z < 0.0) {
    n.xy = float2((1.0 - abs(y)) * (x < 0.0 ? -1.0 : 1.0), (1.0 - abs(x)) * (y < 0.0 ? -1.0 : 1.0));
  }
  return normalize(n);
}

// The mesh's quantization box: origin and side, in the mesh's own space. geometry's
// quantize_positions anchors the grid at the bounding box's low corner and gives it the largest
// axis extent, so this is a bounding cube and costs nothing to carry.
float3 grid_origin(MeshDesc mesh) { return mesh.quant.xyz; }
float grid_side(MeshDesc mesh) { return mesh.quant.w * 65535.0; }

// A sine travelling along the box diagonal, displacing the surface along its own normal.
float3 deform_wave(MeshDesc mesh, float3 p, float3 normal, float time, float amplitude) {
  const float side = max(grid_side(mesh), 1.0e-9);
  const float3 u = (p - grid_origin(mesh)) / side;  // 0..1 across the box
  const float phase = (u.x * 6.0 + u.y * 2.5 + u.z * 4.0) * k_tau + time * 3.0;
  return p + normal * (sin(phase) * amplitude * side);
}

// The 27 control points of the cage, at the corners, edge midpoints, face centres, and centre of
// the mesh's box. Point (i, j, k) sits at the box corner it names and then moves: a swirl about
// the box's y axis whose strength grows away from the centre, plus a breathing scale, both
// functions of time alone. Deterministic, so two instances with the same phase agree.
float3 control_point(float3 origin, float side, uint3 ijk, float time, float amplitude) {
  const float3 t = float3(ijk) * 0.5;  // 0, 0.5, 1 per axis
  const float3 centred = t - 0.5;      // -0.5 .. 0.5
  const float radius = length(centred.xz);
  const float angle = sin(time * 1.7) * amplitude * 4.0 * radius;
  const float s = sin(angle);
  const float c = cos(angle);
  const float3 swirl =
      float3(centred.x * c - centred.z * s, centred.y, centred.x * s + centred.z * c);
  const float breathe = 1.0 + sin(time * 2.3 + centred.y * 4.0) * amplitude * 0.5;
  return origin + (swirl * breathe + 0.5) * side;
}

// The three knot weights of one axis coordinate in 0..1.
float3 axis_weights(float a) {
  if (a < 0.5) {
    const float t = a * 2.0;
    return float3(1.0 - t, t, 0.0);
  }
  const float t = (a - 0.5) * 2.0;
  return float3(0.0, 1.0 - t, t);
}

// Linear blend skinning of one vertex: four influences, weights out of 255, a zero weight
// skipped outright — which is also what makes a stale joint index on a dead influence harmless.
// A non-zero influence naming a joint past the end of the array is skipped too, so bad data
// deforms a vertex short rather than reading somewhere it should not. Three dot products per
// influence against the 3x4: the fourth row of a skinning matrix is always (0, 0, 0, 1), so it
// is neither stored nor loaded.
float3 deform_skin(MeshDesc mesh, DeformDesc deform, uint vertex, float3 rest) {
  if (mesh.skin == 0 || deform.joints == 0 || deform.joint_count == 0) return rest;
  SkinBinding* bindings = (SkinBinding*)mesh.skin;
  const SkinBinding binding = bindings[vertex];
  JointMatrix* matrices = (JointMatrix*)deform.joints;
  const float4 point = float4(rest, 1.0);
  float3 moved = float3(0.0, 0.0, 0.0);
  for (uint k = 0; k < 4; ++k) {
    const uint weight = (binding.packed_weights >> (8 * k)) & 0xffu;
    if (weight == 0) continue;
    const uint joint = (binding.packed_joints >> (8 * k)) & 0xffu;
    if (joint >= deform.joint_count) continue;
    const JointMatrix m = matrices[joint];
    const float3 skinned = float3(dot(m.rows[0], point), dot(m.rows[1], point),
                                  dot(m.rows[2], point));
    moved += skinned * (float(weight) * (1.0 / 255.0));
  }
  return moved;
}

// Trilinear free-form deformation: the position's coordinates in the box weight the eight cells'
// worth of control points. A quadratic Bezier would be smoother; trilinear is what a cage solver
// hands a renderer, and it is what the transfer pass will read from a real one.
float3 deform_lattice(MeshDesc mesh, float3 p, float time, float amplitude) {
  const float side = max(grid_side(mesh), 1.0e-9);
  const float3 origin = grid_origin(mesh);
  const float3 u = clamp((p - origin) / side, 0.0, 1.0);
  // Weights over the three knots 0, 0.5, 1 of one axis: linear within whichever of the two cells
  // the coordinate falls in, zero on the knot the other cell owns.
  const float3 wx = axis_weights(u.x);
  const float3 wy = axis_weights(u.y);
  const float3 wz = axis_weights(u.z);
  float3 moved = float3(0.0, 0.0, 0.0);
  for (uint i = 0; i < 3; ++i) {
    const float ax = i == 0 ? wx.x : (i == 1 ? wx.y : wx.z);
    if (ax == 0.0) continue;
    for (uint j = 0; j < 3; ++j) {
      const float ay = j == 0 ? wy.x : (j == 1 ? wy.y : wy.z);
      if (ay == 0.0) continue;
      for (uint k = 0; k < 3; ++k) {
        const float az = k == 0 ? wz.x : (k == 1 ? wz.y : wz.z);
        if (az == 0.0) continue;
        moved += control_point(origin, side, uint3(i, j, k), time, amplitude) * (ax * ay * az);
      }
    }
  }
  return moved;
}

[shader("compute")]
[numthreads(128, 1, 1)]
void deform_main(uint3 group : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  uint* count_word = (uint*)g_params.visible_count;
  const uint count = min(*count_word, g_params.max_entries);
  const uint entry = group.x;
  if (entry >= count) return;

  uint2* visible = (uint2*)g_params.visible;
  const uint2 pair = visible[entry];
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  const InstanceDesc instance = instances[pair.x];
  if (instance.deform == k_invalid_deform) return;  // rigid: its positions stay on the grid
  // Where this frame put this entry's vertices. `k_no_pool_slot` means the budget had no room:
  // write nothing, and every reader draws the rest pose for this cluster.
  uint* slots = (uint*)g_params.slots;
  const uint base = slots[g_params.visible_offset + entry];
  if (base == k_no_pool_slot) return;
  DeformDesc* table = (DeformDesc*)g_params.deform;
  const DeformDesc deform = table[instance.deform];
  MeshDesc* meshes = (MeshDesc*)g_params.meshes;
  const MeshDesc mesh = meshes[instance.mesh];
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[pair.y];
  const uint kind = deform.flags & k_deform_kind_mask;

  VertexAttributes* attributes = (VertexAttributes*)g_params.attributes;
  float* pool = (float*)g_params.pool;
  for (uint i = group_thread.x; i < cluster.vertex_count; i += 128) {
    const uint vertex = cluster.vertex_offset + i;
    const float3 rest = load_quantized_position(mesh, vertex);
    float3 moved = rest;
    if (kind == k_deform_wave) {
      float3 normal;
      if (g_params.attributes != 0) {
        normal = decode_oct(attributes[vertex].normal_oct);
      } else {
        const float3 centre = grid_origin(mesh) + float3(0.5 * grid_side(mesh));
        const float3 radial = rest - centre;
        const float len = length(radial);
        normal = len > 1.0e-9 ? radial / len : float3(0.0, 1.0, 0.0);
      }
      moved = deform_wave(mesh, rest, normal, g_params.time, g_params.amplitude);
    } else if (kind == k_deform_lattice) {
      moved = deform_lattice(mesh, rest, g_params.time, g_params.amplitude);
    } else if (kind == k_deform_skin) {
      moved = deform_skin(mesh, deform, vertex, rest);
    }
    // k_deform_identity writes the rest pose, which is what makes a deformed instance and a
    // rigid one draw the same picture.
    const uint slot = (base + i) * 3;
    pool[slot] = moved.x;
    pool[slot + 1] = moved.y;
    pool[slot + 2] = moved.z;
  }
}
