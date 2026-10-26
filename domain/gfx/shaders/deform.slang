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
// **It is a chain of stages, not one exclusive mode.** `DeformDesc::stages` is a mask, and the
// pass runs what it names in a fixed order: static shape, pose morphs, skinning, then the
// procedural cage stand-in. A **terrain level** runs a stage of its own instead of the chain
// (`k_deform_stage_terrain`, below): its heights blended between two evaluated fields of the dune
// field, the renderer's time-lapse — and so does a **generated tree** (`k_deform_stage_flora`, the
// flora stage below): its parts bent by the frame's wind and the walker's push. The order is what
// the data means rather than a preference — a morph
// delta is authored in **bind space** against the rest mesh, so it has to be applied before the
// skin moves that mesh, and a cage acts on the **posed** surface because that is what a
// soft-tissue solver sees. gfx.md ("The deform chain") carries the argument and the crack rule
// stage by stage.
//
// The two morph stages are the same arithmetic over different weights, and the split is about
// *when* they run: the static stage's weights are a character's body, which changes when
// something authors it and not per frame, so its result is kept in a per-instance cache
// (`DeformDesc::cache`, filled by `deform_cache_main` over the whole mesh) and the per-frame
// chain starts from there. The pose stage's weights are an expression, which changes every frame
// and is paid for every frame — over the **cut**, which is what makes it affordable.
//
// Three procedural deformers, all stand-ins. §4.3 says skinning reads bone matrices and a
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

// Mirrors gfx::MorphParams (48 bytes): the scene's morph stream, one block behind one address
// because `DeformParams` is a 128-byte push block and six more addresses do not fit in it.
struct MorphParams {
  uint64_t channels;      // MorphChannel[]
  uint64_t directory;     // uint[cluster_count + 1]: the CSR into `slices`
  uint64_t slices;        // MorphSlice[]
  uint64_t indices;       // uint8 per delta, cluster-local, padded to a multiple of four
  uint64_t deltas;        // int16[3 * delta_count]
  uint64_t normal_deltas; // int16[3 * delta_count], or 0
};

// Mirrors gfx::DeformParams (128 bytes). `deform_cache_main` reads two of the fields with a
// different meaning, and says so where it uses them, because the block is at the 128-byte limit
// the renderer allows and the two it needs are the two the cut dispatch uses for the same
// purpose: where to start, and how many.
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
  uint64_t morph;         // MorphParams*, or 0 for a scene with no morph channels
  uint64_t normal_pool;   // uint[] octahedral, parallel to `pool`, or 0
  float time;             // animation phase in seconds
  float amplitude;        // displacement scale as a fraction of the mesh's grid box
  uint max_entries;       // the run's capacity; the device count is clamped to it
  uint visible_offset;    // this run's first entry in the whole visible list
  uint64_t terrain;       // TerrainLevelDesc[]: the frame's terrain levels, or 0
  uint64_t motion;        // uint per scene-wide vertex: part | bend weight << 16, or 0
  uint64_t flora;         // FloraPartPose[]: the frame's part poses, or 0
};

[[vk::push_constant]] ConstantBuffer<DeformParams> g_params;

static const float k_tau = 6.2831853071795864769;

float3 decode_oct(uint packed) { return decode_oct_normal(packed); }

// Three int16 at `3 * delta` of a delta stream, read out of the 32-bit words that contain them —
// the signed twin of `load_quantized_position`'s unpacking, and the reason both streams are
// padded to an even count on the host.
float3 load_morph_delta(uint64_t address, uint delta) {
  uint* words = (uint*)address;
  const uint i = delta * 3;
  const uint w0 = words[i >> 1];
  const uint w1 = words[(i + 1) >> 1];
  const uint w2 = words[(i + 2) >> 1];
  const uint x = (i & 1) != 0 ? (w0 >> 16) : (w0 & 0xffff);
  const uint y = ((i + 1) & 1) != 0 ? (w1 >> 16) : (w1 & 0xffff);
  const uint z = ((i + 2) & 1) != 0 ? (w2 >> 16) : (w2 & 0xffff);
  return float3(float(int(x << 16) >> 16), float(int(y << 16) >> 16), float(int(z << 16) >> 16));
}

// One morph stage over one vertex: sum over the channels that touch this cluster of
// weight x delta, added to the position and the normal in place.
//
// The scan is per **slice** and the lookup inside a slice is a binary search, because a slice's
// local vertex indices are built in ascending order. That is the shape the cluster-ordered
// directory was chosen for: a cluster no channel touches reads one word of the directory and
// stops, and a cluster three channels touch costs three searches of about eight loads, not a
// walk of the mesh's whole delta stream.
//
// `weight_base` is 0 for the static stage's half of `DeformDesc::weights` and `channel_count` for
// the pose stage's, which is the only difference between the two.
void morph_stage(uint cluster, uint local, DeformDesc deform, uint weight_base,
                 inout float3 position, inout float3 normal) {
  if (g_params.morph == 0 || deform.weights == 0 || deform.channel_count == 0) return;
  MorphParams* morph = (MorphParams*)g_params.morph;
  if (morph->directory == 0) return;
  uint* directory = (uint*)morph->directory;
  MorphSlice* slices = (MorphSlice*)morph->slices;
  MorphChannel* channels = (MorphChannel*)morph->channels;
  uint* indices = (uint*)morph->indices;
  float* weights = (float*)deform.weights;
  const uint lo = directory[cluster];
  const uint hi = directory[cluster + 1];
  for (uint s = lo; s < hi; ++s) {
    const MorphSlice slice = slices[s];
    if (slice.channel < deform.first_channel) continue;
    const uint index = slice.channel - deform.first_channel;
    if (index >= deform.channel_count) continue;
    const float weight = weights[weight_base + index];
    if (weight == 0.0) continue;
    // Where `local` sits in this slice's ascending run of cluster-local vertex indices.
    uint left = 0;
    uint right = slice.delta_count;
    while (left < right) {
      const uint mid = (left + right) >> 1;
      const uint at = slice.first_delta + mid;
      const uint value = (indices[at >> 2] >> (8 * (at & 3))) & 0xffu;
      if (value < local) {
        left = mid + 1;
      } else {
        right = mid;
      }
    }
    if (left >= slice.delta_count) continue;
    const uint at = slice.first_delta + left;
    if (((indices[at >> 2] >> (8 * (at & 3))) & 0xffu) != local) continue;
    const MorphChannel channel = channels[slice.channel];
    position += load_morph_delta(morph->deltas, at) * (channel.position_scale * weight);
    if (morph->normal_deltas != 0 && channel.normal_scale > 0.0) {
      normal += load_morph_delta(morph->normal_deltas, at) * (channel.normal_scale * weight);
    }
  }
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

// The same blend applied to a **direction**: the matrices' upper-left 3x3 with no translation.
// It is the rotation part of the skin rather than the inverse transpose, which is exact for the
// rigid joints every skeleton has and differs only where a joint carries a non-uniform scale.
// Nothing calls it unless the frame carries a normal pool, so no existing picture sees it.
float3 skin_normal(MeshDesc mesh, DeformDesc deform, uint vertex, float3 rest) {
  if (mesh.skin == 0 || deform.joints == 0 || deform.joint_count == 0) return rest;
  SkinBinding* bindings = (SkinBinding*)mesh.skin;
  const SkinBinding binding = bindings[vertex];
  JointMatrix* matrices = (JointMatrix*)deform.joints;
  const float4 direction = float4(rest, 0.0);
  float3 moved = float3(0.0, 0.0, 0.0);
  for (uint k = 0; k < 4; ++k) {
    const uint weight = (binding.packed_weights >> (8 * k)) & 0xffu;
    if (weight == 0) continue;
    const uint joint = (binding.packed_joints >> (8 * k)) & 0xffu;
    if (joint >= deform.joint_count) continue;
    const JointMatrix m = matrices[joint];
    const float3 turned = float3(dot(m.rows[0], direction), dot(m.rows[1], direction),
                                 dot(m.rows[2], direction));
    moved += turned * (float(weight) * (1.0 / 255.0));
  }
  return dot(moved, moved) > 1.0e-20 ? normalize(moved) : rest;
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

// The rest pose of one vertex and the normal a stage may need with it. `have_normal` is false
// when the frame carries no attribute stream, and the caller then falls back to the radial
// direction the `wave` stand-in used before attributes were passed.
float3 rest_normal(MeshDesc mesh, uint vertex, float3 rest) {
  if (g_params.attributes != 0) {
    VertexAttributes* attributes = (VertexAttributes*)g_params.attributes;
    return decode_oct(attributes[vertex].normal_oct);
  }
  const float3 centre = grid_origin(mesh) + float3(0.5 * grid_side(mesh));
  const float3 radial = rest - centre;
  const float len = length(radial);
  return len > 1.0e-9 ? radial / len : float3(0.0, 1.0, 0.0);
}

// The chain over one vertex. Every stage is optional and the order is fixed; `stages` says which
// ran, and the whole of the ordering argument is in this file's header and in gfx.md.
void deform_vertex(MeshDesc mesh, DeformDesc deform, ClusterDesc cluster, uint cluster_index,
                   uint local, out float3 position, out float3 normal) {
  const uint stages = deform.stages;
  const uint vertex = cluster.vertex_offset + local;
  const bool cached = (stages & k_deform_stage_static) != 0 && deform.cache != 0 &&
                      (stages & k_deform_stage_rebuild_cache) == 0;
  if (cached) {
    DeformCacheVertex* cache = (DeformCacheVertex*)deform.cache;
    const DeformCacheVertex entry = cache[vertex - deform.first_vertex];
    position = entry.position;
    normal = decode_oct(entry.normal);
  } else {
    position = load_quantized_position(mesh, vertex);
    normal = rest_normal(mesh, vertex, position);
    // Stage 1. Skipped when the cache above already holds its result, which is the whole point
    // of the cache: a body shape costs a dispatch when it is authored and nothing after that.
    if ((stages & k_deform_stage_static) != 0) {
      morph_stage(cluster_index, local, deform, 0, position, normal);
      normal = normalize(normal);
    }
  }
  // Stage 2.
  if ((stages & k_deform_stage_pose) != 0) {
    morph_stage(cluster_index, local, deform, deform.channel_count, position, normal);
    normal = normalize(normal);
  }
  // Stage 3. The normal is turned by the same blend, which is exact for the rigid joints every
  // skeleton has; it is computed only when the frame carries a normal pool to put it in.
  if ((stages & k_deform_stage_skin) != 0) {
    if (g_params.normal_pool != 0) normal = skin_normal(mesh, deform, vertex, normal);
    position = deform_skin(mesh, deform, vertex, position);
  }
  // Stage 4, on the posed surface. `k_deform_identity` writes what it was handed, which is what
  // proves a deformed instance and a rigid one draw the same picture.
  //
  // **A note the next person will want.** The two procedural stand-ins produce a picture that is
  // *not* byte-identical to the one the exclusive-mode pass produced, on a handful of pixels, by
  // one level of 255 — `--deform wave` on the FlightHelmet measures PSNR 106.5 dB, SSIM 1.000000,
  // FLIP mean 0.000000 with a worst pixel of 0.0088. Nothing about the arithmetic changed: the
  // identity deformer, which writes the rest position, is byte-identical, and so is every picture
  // with no deform pass at all and the skinned Fox through `--animate`. What changed is that the
  // same expressions are now compiled in a module that also holds the morph stages, and the
  // driver contracts their multiply-adds differently — the same effect E25 recorded for the pool's
  // depths ("the two compilations of the same expression round the multiply and the add
  // differently in the last bit"). Moving the stage back into the caller's loop body was tried and
  // changed nothing, which is what says the cause is the module and not the call.
  if ((stages & k_deform_stage_procedural) != 0) {
    const uint kind = stages & k_deform_kind_mask;
    if (kind == k_deform_wave) {
      position = deform_wave(mesh, position, normal, g_params.time, g_params.amplitude);
    } else if (kind == k_deform_lattice) {
      position = deform_lattice(mesh, position, g_params.time, g_params.amplitude);
    }
  }
}

// ---- the terrain stage (docs/subsystems/renderer.md, "The dunes in time-lapse") -----------------
//
// A terrain level is a heightfield on a lattice, and the dune field moves only its heights. So a
// vertex's position is a function of **its lattice point alone**: (i, j) from the rest position,
// which every copy of the vertex on every LOD level shares bit for bit (the crack rule, gfx.md's
// deform chain), then `origin + (i, j) * spacing` across and the blend of the level's two fields
// up. The fields are the same function of time evaluated at two game times; the blend is a
// continuous function of game time the host advances a little every frame, so the surface moves
// every frame by a fraction of the difference and reaches the second field exactly when the next
// one is ready to start (renderer::TerrainMotion). The normal is the blended field's central
// difference at the lattice point — the arithmetic `load_terrain` gives the rest mesh its normals
// with, one-sided at a field's edge like it — so the light moves with the sand.

// Height at lattice point (i, j) of one field, clamped into its window.
float terrain_sample(TerrainField field, int i, int j) {
  const int x = clamp(i - field.i0, 0, int(field.nx) - 1);
  const int z = clamp(j - field.j0, 0, int(field.nz) - 1);
  float* heights = (float*)field.heights;
  return heights[uint(z) * field.nx + uint(x)];
}

// The blended field. `a (1 - t) + b t` rather than `a + (b - a) t`: it is exactly `a` at t = 0 and
// exactly `b` at t = 1, so a level that has reached its next field draws that field's heights to
// the bit, and a level with one field draws it.
float terrain_height(TerrainLevelDesc level, int i, int j) {
  const float a = terrain_sample(level.a, i, j);
  if (level.b.heights == 0) return a;
  const float b = terrain_sample(level.b, i, j);
  return a * (1.0 - level.blend) + b * level.blend;
}

// The lattice range both fields cover along one axis, which a neighbour for the normal is clamped
// into — the one-sided difference at a field's edge that the rest mesh's normals take at the grid's.
int2 terrain_window(TerrainLevelDesc level, bool along_z) {
  const int lo_a = along_z ? level.a.j0 : level.a.i0;
  const int hi_a = lo_a + int(along_z ? level.a.nz : level.a.nx) - 1;
  if (level.b.heights == 0) return int2(lo_a, hi_a);
  const int lo_b = along_z ? level.b.j0 : level.b.i0;
  const int hi_b = lo_b + int(along_z ? level.b.nz : level.b.nx) - 1;
  return int2(max(lo_a, lo_b), min(hi_a, hi_b));
}

// The blended field's normal at lattice point (i, j): central differences, one-sided at the
// fields' window, as `load_terrain` gives the rest mesh its normals. A function of the lattice
// alone, so a world-lattice tile and the scene's grid take the same one.
float3 terrain_normal(TerrainLevelDesc level, int i, int j) {
  const int2 wx = terrain_window(level, false);
  const int2 wz = terrain_window(level, true);
  const int x0 = clamp(i - 1, wx.x, wx.y);
  const int x1 = clamp(i + 1, wx.x, wx.y);
  const int z0 = clamp(j - 1, wz.x, wz.y);
  const int z1 = clamp(j + 1, wz.x, wz.y);
  const float dx = float(x1 - x0) * level.spacing;
  const float dz = float(z1 - z0) * level.spacing;
  const float hx = terrain_height(level, x1, j) - terrain_height(level, x0, j);
  const float hz = terrain_height(level, i, z1) - terrain_height(level, i, z0);
  // cross((0, hz, dz), (dx, hx, 0)), as `load_terrain` takes it: +y for a flat field.
  const float3 n = float3(-dz * hx, dz * dx, -dx * hz);
  const float len = length(n);
  return len > 0.0 ? n / len : float3(0.0, 1.0, 0.0);
}

void terrain_vertex(MeshDesc mesh, InstanceDesc instance, TerrainLevelDesc level, uint vertex,
                    out float3 position, out float3 normal) {
  // A vertex another level draws: a world tile's border along a coarser tile names that tile's
  // level in its rest normal (down 45 degrees, at its number of sixteenths of a turn), and is
  // placed, raised and lit from that level's description, as the coarser tile's own copy is — so
  // the two tiles draw one vertex whatever pair and blend each level is at (renderer.md, "The ground
  // from the world's tiles"). No other level sets the flag, and a heightfield normal never points
  // down. Horizontal markers would also match a steep authored cliff (W67).
  if ((level.flags & k_terrain_level_named) != 0 && g_params.attributes != 0) {
    VertexAttributes* attributes = (VertexAttributes*)g_params.attributes;
    const float3 named = decode_oct(attributes[vertex].normal_oct);
    if (named.y < -0.5) {
      const int sixteenths = int(round(atan2(named.z, named.x) * (8.0 / 3.14159265358979)));
      level = ((TerrainLevelDesc*)g_params.terrain)[uint((sixteenths + 16) % 16)];
    }
  }
  const float3 rest = load_quantized_position(mesh, vertex);
  // **A world-lattice level's tile is placed at its corner** (scene.slang, `terrain_corner_mm`;
  // renderer.md, "The ground's tiles are placed at their corners"): the vertex's lattice point is
  // found from the corner in whole millimetres and its rest position from the corner, and it is
  // written relative to the corner, so nothing here is the size of the distance from the world's
  // origin. A world-lattice level has no hole.
  if (level.spacing_mm > 0) {
    const int64_t s = int64_t(level.spacing_mm);
    const int64_t cx = terrain_corner_axis_mm(instance.cell.x, instance.rows[0].w);
    const int64_t cz = terrain_corner_axis_mm(instance.cell.z, instance.rows[2].w);
    const int ti = terrain_lattice_index(cx, rest.x, s);
    const int tj = terrain_lattice_index(cz, rest.z, s);
    bool down = false;
    if (level.skirt > 0.0 && g_params.attributes != 0) {
      VertexAttributes* attributes = (VertexAttributes*)g_params.attributes;
      down = decode_oct(attributes[vertex].normal_oct).y < -0.5;
    }
    position = float3(terrain_from_corner(ti, s, cx),
                      terrain_height(level, ti, tj) - (down ? level.skirt : 0.0),
                      terrain_from_corner(tj, s, cz));
    normal = terrain_normal(level, ti, tj);
    return;
  }
  const float inv = 1.0 / level.spacing;
  int i = int(round((rest.x - level.origin.x) * inv));
  int j = int(round((rest.z - level.origin.y) * inv));
  // Strictly inside the square the next finer level draws: onto its nearest edge. The edges are
  // lines of this level's lattice (renderer::terrain_levels snaps them there), so the vertex lands
  // on a lattice point and takes the field's own height there. A cluster that straddles the edge
  // keeps its part outside, exact at the edge; a vertex on the edge is not moved. **And down by as
  // far as it moved**: no map of a square's inside onto its edge is without a fold (the diagonals,
  // where the nearest edge changes), so a triangle whose corners went to two different edges spans
  // the square's corner at the edges' heights, over ground the finer level draws, and stood up to
  // a metre proud of it on the test's dunes. Sunk by the distance its corners were pulled in, it
  // lies under that ground unless the sand across it rises faster than that; the price is that a
  // triangle stretched from the last vertex outside to the edge dips there by at most the length
  // it was stretched, a notch the finer level's skirt stands in.
  const float4 hole = level.hole;
  const float x = level.origin.x + float(i) * level.spacing;
  const float z = level.origin.y + float(j) * level.spacing;
  float sunk = 0.0;
  if (hole.z > hole.x && x > hole.x && x < hole.z && z > hole.y && z < hole.w) {
    const float left = x - hole.x;
    const float right = hole.z - x;
    const float low = z - hole.y;
    const float high = hole.w - z;
    const float nearest = min(min(left, right), min(low, high));
    if (nearest == left) {
      i = int(round((hole.x - level.origin.x) * inv));
    } else if (nearest == right) {
      i = int(round((hole.z - level.origin.x) * inv));
    } else if (nearest == low) {
      j = int(round((hole.y - level.origin.y) * inv));
    } else {
      j = int(round((hole.w - level.origin.y) * inv));
    }
    sunk = nearest;
  }
  // A ring's skirt hangs from its border: same lattice point as the border vertex above it, and
  // `skirt` metres under the field, so it follows the sand down the whole way. The GPU scene marks
  // a skirt vertex by a rest normal pointing straight down, which no heightfield vertex has (the
  // pool writes the normals the resolve reads, so the rest one is free to say so).
  bool skirt = false;
  if (level.skirt > 0.0 && g_params.attributes != 0) {
    VertexAttributes* attributes = (VertexAttributes*)g_params.attributes;
    skirt = decode_oct(attributes[vertex].normal_oct).y < -0.5;
  }
  position = float3(level.origin.x + float(i) * level.spacing,
                    terrain_height(level, i, j) - (skirt ? level.skirt : 0.0) - sunk,
                    level.origin.y + float(j) * level.spacing);
  normal = terrain_normal(level, i, j);
}

// ---- the flora stage (docs/subsystems/renderer.md, "The flora stage"; flora.md, "Motion") -------
//
// A generated tree sways by its parts. A vertex's part and bend weight are the mesh's (the motion
// contract, `DeformParams::motion`), and the part's pose is the frame's (`DeformParams::flora`, from
// `DeformDesc::first_part`): a bend of `pivot_angle.w * weight` radians about `axis` through the
// part's pivot, then the rigid frame its ancestors' bends carried the part to — which the CPU
// composed parent-first, so nothing here walks a chain. The weight is 0 at the part's root and 1 at
// its tip, so the root stays where its parent put it and the tip travels.
//
// **Crack-free for the same reason skinning is**: a vertex's position is a function of its rest
// position, its part and its weight, all three per-vertex data every copy of a surface point shares
// on every LOD level (clusterlod keeps original vertices, and section 36 is one record per cluster
// vertex). The normal turns with the same rotation; it is never displaced along.
//
// **A part at rest writes the rest pose itself**: its position off the grid and its normal's packed
// word copied through, so a tree with no wind and no push is the rigid picture to the bit — the
// pool reader decodes the same word the rigid path decodes.

// Mirrors gfx::FloraPartPose (80 bytes).
struct FloraPartPose {
  float4 rows[3];       // the frame the part's ancestors carried it to: rotation | translation
  float4 pivot_angle;   // the pivot, and the part's own bend at its tip in radians
  float4 axis;          // the bend's unit axis; w 0 when the part is at rest
};

// Rodrigues, in the order gfx::flora_rotate writes it.
float3 flora_rotate(float3 v, float3 k, float c, float s) {
  const float3 kxv = cross(k, v);
  const float kv = dot(k, v) * (1.0 - c);
  return float3(v.x * c + kxv.x * s + k.x * kv, v.y * c + kxv.y * s + k.y * kv,
                v.z * c + kxv.z * s + k.z * kv);
}

void flora_vertex(MeshDesc mesh, DeformDesc deform, uint vertex, out float3 position,
                  out uint normal_word) {
  position = load_quantized_position(mesh, vertex);
  if (g_params.attributes != 0) {
    VertexAttributes* attributes = (VertexAttributes*)g_params.attributes;
    normal_word = attributes[vertex].normal_oct;
  } else {
    normal_word = encode_oct_normal(rest_normal(mesh, vertex, position));
  }
  if (g_params.motion == 0 || g_params.flora == 0) return;
  const uint record = ((uint*)g_params.motion)[vertex];
  const uint part = record & 0xffffu;
  if (part >= deform.joint_count) return;  // a part the table does not have: at rest
  const FloraPartPose pose = ((FloraPartPose*)g_params.flora)[deform.first_part + part];
  if (pose.axis.w == 0.0) return;
  const float angle = pose.pivot_angle.w * (float(record >> 16) * (1.0 / 65535.0));
  const float c = cos(angle);
  const float s = sin(angle);
  const float3 k = pose.axis.xyz;
  const float3 pivot = pose.pivot_angle.xyz;
  const float3 q = flora_rotate(position - pivot, k, c, s) + pivot;
  position = float3(dot(pose.rows[0].xyz, q) + pose.rows[0].w,
                    dot(pose.rows[1].xyz, q) + pose.rows[1].w,
                    dot(pose.rows[2].xyz, q) + pose.rows[2].w);
  if (g_params.normal_pool == 0) return;
  const float3 n = flora_rotate(decode_oct(normal_word), k, c, s);
  const float3 m = float3(dot(pose.rows[0].xyz, n), dot(pose.rows[1].xyz, n),
                          dot(pose.rows[2].xyz, n));
  const float len = length(m);
  if (len > 0.0) normal_word = encode_oct_normal(m / len);
}

// ---- the field kind (renderer.md, "The field kind"; gfx/flora_motion.h; R95) ---------------------
//
// Grass, posed here and nowhere else: a field mesh's record is its mesh's, its blades' roots are
// the scene's static field part table (`DeformDesc::joints`), and the frame's wind, gust field,
// flutter clock and walker are one `FloraFieldFrame` just before the trees' poses. A blade bends
// about its root, across the wind, by the trees' lean and flutter with the gust read off a field of
// four travelling plane waves at the root, plus the walker's push away from its capsule; the arc
// fraction scales the angle, so a straight blade's tip travels as the square of the fraction (the
// cantilever) and the root stays. Crack-free as the trees are: everything a vertex reads is its
// rest position, its part and its fraction, and its instance, which every copy shares.

// Mirrors gfx::FloraFieldFrame (160 bytes).
struct FloraFieldFrame {
  FrameEye eye;
  float4 wind;     // to.x, to.z, strength, the flutter's phase
  float4 waves[2]; // the four waves' (x, z) vectors, rad/m
  float4 phases;   // their phases at the eye
  float4 weights;  // their shares of the gust
  float4 walker;   // the walker's feet from the eye, radius (<= 0: none)
  float4 push;     // height, the push's most radians, margin, 0
};

static const float k_field_lean = 0.6;     // renderer::flora_part_poses' constants, the trees'
static const float k_field_lean_gust = 0.8;
static const float k_field_flutter = 0.25;
static const float k_field_flutter_gust = 0.35;

uint field_hash(uint x) {  // lowbias32, as flora_motion.cpp's hash32
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

// The instance's seed from its placement alone — its cell and its offset into it — so a whole read
// and a streamed one, which place it at the same bits, give each blade the same phase.
uint field_seed(InstanceDesc instance) {
  uint h = field_hash(uint(instance.cell.x) * 0x9e3779b9u ^ asuint(instance.rows[0].w));
  h = field_hash(h ^ (uint(instance.cell.z) * 0x85ebca6bu) ^ asuint(instance.rows[2].w));
  return field_hash(h ^ asuint(instance.rows[1].w));
}

void field_vertex(MeshDesc mesh, InstanceDesc instance, DeformDesc deform, uint vertex,
                  out float3 position, out uint normal_word) {
  position = load_quantized_position(mesh, vertex);
  if (g_params.attributes != 0) {
    VertexAttributes* attributes = (VertexAttributes*)g_params.attributes;
    normal_word = attributes[vertex].normal_oct;
  } else {
    normal_word = encode_oct_normal(rest_normal(mesh, vertex, position));
  }
  if (g_params.motion == 0 || g_params.flora == 0 || deform.joints == 0) return;
  const uint record = ((uint*)g_params.motion)[vertex];
  const uint part = record & 0xffffu;
  const uint fraction = record >> 16;
  if (part >= deform.joint_count || fraction == 0) return;  // the root stays where it is
  const FloraFieldFrame field = ((FloraFieldFrame*)(g_params.flora - 160))[0];
  const float4 blade = ((float4*)deform.joints)[part];  // root xyz, length
  const float strength = field.wind.z;
  const bool walker = field.walker.w > 0.0;
  if (strength <= 0.0 && !walker) return;  // calm and untouched: the rest pose, to the bit
  const float3 from_eye = instance_from_eye(instance, field.eye);
  const float3 root = instance_point(instance, from_eye, blade.xyz);  // in the frame's space
  // ---- the wind: lean and flutter about the axis across it, the gust from the field at the root
  float3 omega = float3(0.0, 0.0, 0.0);
  if (strength > 0.0) {
    const float g4 = field.weights.x * sin(field.phases.x + dot(field.waves[0].xy, root.xz)) +
                     field.weights.y * sin(field.phases.y + dot(field.waves[0].zw, root.xz)) +
                     field.weights.z * sin(field.phases.z + dot(field.waves[1].xy, root.xz)) +
                     field.weights.w * sin(field.phases.w + dot(field.waves[1].zw, root.xz));
    const float gust = saturate(0.5 + 0.5 * g4);
    // The blade's phase from its root's bits rather than its part index: a distance form prunes
    // and renumbers its parts (renderer.md, "A form carries only the parts it uses") but copies
    // the roots, so a card sways in its patch's phases.
    const uint h = field_hash(field_hash(asuint(blade.x) * 0x9e3779b9u ^ asuint(blade.z)) ^
                              asuint(blade.y) ^ field_seed(instance));
    const float phase = float(h >> 8) * (k_tau / 16777216.0);
    const float compliance = asfloat(deform.first_channel);
    const float angle =
        compliance * strength *
        ((k_field_lean + k_field_lean_gust * gust) +
         (k_field_flutter + k_field_flutter_gust * gust) * sin(field.wind.w + phase));
    // cross(up, to): the axis that turns up towards where the wind blows.
    omega = float3(field.wind.y, 0.0, -field.wind.x) * angle;
  }
  // ---- the walker: a blade whose root is within its radius plus the blade's length of the
  // capsule's axis, and within its height, is turned away from it, most at the capsule's skin.
  if (walker) {
    const float2 away = root.xz - field.walker.xz;
    const float d = length(away);
    const float length_m = blade.w;
    const float reach = field.walker.w + field.push.z + length_m;
    if (length_m > 0.0 && d < reach && d > 1.0e-4 && root.y > field.walker.y - length_m &&
        root.y < field.walker.y + field.push.x) {
      const float t = saturate((reach - d) / length_m);
      const float2 a = away / d;
      omega += float3(a.y, 0.0, -a.x) * (field.push.y * t);
    }
  }
  const float angle_world = length(omega);
  if (angle_world <= 0.0) return;
  // The rotation's axis into the instance's mesh frame: the 3x3's inverse by its cofactors.
  const float3 c0 = float3(instance.rows[0].x, instance.rows[1].x, instance.rows[2].x);
  const float3 c1 = float3(instance.rows[0].y, instance.rows[1].y, instance.rows[2].y);
  const float3 c2 = float3(instance.rows[0].z, instance.rows[1].z, instance.rows[2].z);
  const float3 w = omega / angle_world;
  const float3 k_mesh = float3(dot(cross(c1, c2), w), dot(cross(c2, c0), w), dot(cross(c0, c1), w));
  const float det = dot(c0, cross(c1, c2));
  const float k_length = length(k_mesh);
  if (k_length <= 0.0 || det == 0.0) return;
  const float3 k = k_mesh * (sign(det) / k_length);
  const float a = angle_world * (float(fraction) * (1.0 / 65535.0));
  const float c = cos(a);
  const float s = sin(a);
  position = flora_rotate(position - blade.xyz, k, c, s) + blade.xyz;
  if (g_params.normal_pool == 0) return;
  const float3 n = flora_rotate(decode_oct(normal_word), k, c, s);
  const float len = length(n);
  if (len > 0.0) normal_word = encode_oct_normal(n / len);
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

  float* pool = (float*)g_params.pool;
  uint* normals = (uint*)g_params.normal_pool;
  if ((deform.stages & k_deform_stage_terrain) != 0) {
    // A terrain level with no table this frame, or no field in it yet (a host that made the level
    // and runs no motion), writes its rest pose into the block it was given, so a reader never
    // meets an unwritten slot.
    bool have_level = g_params.terrain != 0 && instance.terrain != 0;
    TerrainLevelDesc level = (TerrainLevelDesc)0;
    if (have_level) {
      level = ((TerrainLevelDesc*)g_params.terrain)[instance.terrain - 1];
      have_level = level.a.heights != 0;
    }
    for (uint i = group_thread.x; i < cluster.vertex_count; i += 128) {
      float3 moved;
      float3 normal;
      const uint vertex = cluster.vertex_offset + i;
      if (have_level) {
        terrain_vertex(mesh, instance, level, vertex, moved, normal);
      } else {
        moved = load_quantized_position(mesh, vertex);
        normal = rest_normal(mesh, vertex, moved);
      }
      const uint slot = (base + i) * 3;
      pool[slot] = moved.x;
      pool[slot + 1] = moved.y;
      pool[slot + 2] = moved.z;
      if (g_params.normal_pool != 0) normals[base + i] = encode_oct_normal(normal);
    }
    return;
  }
  if ((deform.stages & k_deform_stage_flora) != 0) {
    const bool field = (deform.stages & k_deform_flora_field) != 0;
    for (uint i = group_thread.x; i < cluster.vertex_count; i += 128) {
      float3 moved;
      uint normal_word;
      if (field) {
        field_vertex(mesh, instance, deform, cluster.vertex_offset + i, moved, normal_word);
      } else {
        flora_vertex(mesh, deform, cluster.vertex_offset + i, moved, normal_word);
      }
      const uint slot = (base + i) * 3;
      pool[slot] = moved.x;
      pool[slot + 1] = moved.y;
      pool[slot + 2] = moved.z;
      if (g_params.normal_pool != 0) normals[base + i] = normal_word;
    }
    return;
  }
  for (uint i = group_thread.x; i < cluster.vertex_count; i += 128) {
    float3 moved;
    float3 normal;
    deform_vertex(mesh, deform, cluster, pair.y, i, moved, normal);
    const uint slot = (base + i) * 3;
    pool[slot] = moved.x;
    pool[slot + 1] = moved.y;
    pool[slot + 2] = moved.z;
    if (g_params.normal_pool != 0) normals[base + i] = encode_oct_normal(normal);
  }
}

// **The static shape stage's cache, filled over the whole mesh.** One workgroup per cluster of
// one instance's mesh — not per entry of the cut, because a cache the next frame's cut can start
// from has to cover every cluster the cut might name.
//
// It reuses two fields of `DeformParams` with a different meaning, which is a thing to know
// before reading the dispatch: `visible_offset` is the **instance** to rebuild and `max_entries`
// is how many clusters its mesh has. The block is at the 128-byte limit the renderer allows, and
// those two are the fields the cut dispatch uses for exactly the same two purposes.
[shader("compute")]
[numthreads(128, 1, 1)]
void deform_cache_main(uint3 group : SV_GroupID, uint3 group_thread : SV_GroupThreadID) {
  const uint local_cluster = group.x;
  if (local_cluster >= g_params.max_entries) return;
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  const InstanceDesc instance = instances[g_params.visible_offset];
  if (instance.deform == k_invalid_deform) return;
  DeformDesc* table = (DeformDesc*)g_params.deform;
  const DeformDesc deform = table[instance.deform];
  if (deform.cache == 0 || (deform.stages & k_deform_stage_static) == 0) return;
  MeshDesc* meshes = (MeshDesc*)g_params.meshes;
  const MeshDesc mesh = meshes[instance.mesh];
  const uint cluster_index = mesh.first_cluster + local_cluster;
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[cluster_index];

  DeformCacheVertex* cache = (DeformCacheVertex*)deform.cache;
  for (uint i = group_thread.x; i < cluster.vertex_count; i += 128) {
    const uint vertex = cluster.vertex_offset + i;
    float3 position = load_quantized_position(mesh, vertex);
    float3 normal = rest_normal(mesh, vertex, position);
    morph_stage(cluster_index, i, deform, 0, position, normal);
    DeformCacheVertex entry;
    entry.position = position;
    entry.normal = encode_oct_normal(normalize(normal));
    cache[vertex - deform.first_vertex] = entry;
  }
}
