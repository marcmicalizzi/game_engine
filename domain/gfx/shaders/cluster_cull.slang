// GPU cluster culling and LOD selection over a scene of instances (docs/plan/04-renderer.md
// §4.2, §4.3): one thread per (instance, cluster) **pair**, every test done in the instance's
// frame. The thread finds its instance by binary search over the instances' `first_pair` prefix
// sum, reads the cluster of that instance's mesh, moves the bounding sphere, the normal cone,
// and the two LOD spheres into world space, and runs the same frustum, backface, DAG-cut, and
// Hi-Z occlusion tests the single-mesh pass ran. Survivors append `{instance, cluster}` to a
// visible list, and the same atomic is the group count of the indirect draw that follows, so no
// CPU round trip sits between culling and drawing.
//
// Two-pass occlusion culling: pass 1 considers the pairs drawn last frame and tests them
// against last frame's Hi-Z; pass 2 considers everything pass 1 did not draw and tests it
// against the Hi-Z rebuilt from pass 1's depth. Both passes mark what they draw in `flags`,
// which becomes next frame's `prev_flags`; both arrays are indexed by pair. Pass 0 is a single
// pass without occlusion.

#include "scene.slang"

// Mirrors geometry::ClusterLodDesc (48 bytes).
struct ClusterLodDesc {
  float4 own;  // xyz center, w radius
  float4 parent;
  float own_error;
  float parent_error;
  uint level;
  uint group;
};

// Read through a device address; mirrors gfx::StreamParams (64 bytes). Null in `CullParams` for a
// scene that is uploaded whole.
struct StreamParams {
  uint64_t pages;            // ClusterPageDesc[]
  uint64_t page_of_cluster;  // uint per cluster
  uint64_t children;         // ClusterChildren per cluster
  uint64_t residency;        // uint per page
  uint64_t used;             // uint per page
  uint64_t requests;         // PageRequest[]
  uint64_t request_count;    // uint, the atomic
  uint64_t request_mask;     // uint per page
};

// The off-frustum casters' volumes: mirrors gfx::k_caster_volume_planes and k_max_caster_volumes.
static const uint k_caster_volume_planes = 13;
static const uint k_max_caster_volumes = 5;

// Read through a device address; mirrors gfx::CullParams (1728 bytes).
struct CullParams {
  float4 planes[6];  // inward-facing, normalized: dot(n, p) + w >= 0 inside
  float4 camera;     // xyz this view's eye in the frame's space, w = znear
  float4 lod;        // x = proj_scale, y = threshold_px, z = lod enabled, w = frustum enabled
  // x = projected cluster diameter in pixels below which a cluster goes to the software
  // rasterizer; y = mode: 0 everything hardware, 1 everything software, 2 split by x
  float4 raster;
  uint cluster_count;
  uint plane_count;
  uint count_index;  // u32 of draw_args that counts hardware survivors (0 mesh tasks, 1 vkCmdDrawIndirect)
  uint cone_cull;    // 1: reject clusters whose normal cone faces away from the camera
  uint64_t clusters;    // ClusterDesc[]
  uint64_t lods;        // ClusterLodDesc[]
  uint64_t visible;     // uint2[pair_count]: {instance, cluster} of the hardware survivors
  uint64_t draw_args;   // uint3: group counts of vkCmdDrawMeshTasksIndirectEXT; x counts survivors
  uint64_t sw_visible;  // uint2[pair_count]: the software-rasterized survivors
  uint64_t sw_args;     // uint3: group counts of vkCmdDispatchIndirect for the software pass
  // Occlusion culling.
  column_major float4x4 view_proj;
  uint64_t hiz;         // float[]: farthest reversed-Z depth per texel, mips back to back; 0 disables
  uint64_t prev_flags;  // uint[pair_count]: drawn last frame (pass 1 candidates)
  uint64_t flags;       // uint[pair_count]: drawn this frame (written by passes 1 and 2)
  uint hiz_width;       // mip 0 size
  uint hiz_height;
  uint hiz_mips;
  uint pass;            // 0 single pass, 1 or 2 as above
  uint hiz_offsets[16]; // element offset of each mip
  uint64_t instances;   // InstanceDesc[instance_count]
  uint64_t meshes;      // MeshDesc[]
  uint instance_count;
  uint pair_count;
  uint64_t streaming;   // StreamParams*, or 0: the scene is uploaded whole
  uint page_count;
  uint max_requests;
  // Shadow casters: the pairs the cone test alone rejected, which face away from the camera and
  // not necessarily from a light, and the pairs outside the frustum inside a caster volume (below).
  // 0 drops them; otherwise they are appended here (see cull_pair).
  uint64_t casters;       // uint2[pair_count]: {instance, cluster}, a run of its own
  uint64_t caster_count;  // uint: the atomic the run is appended under
  // The vertex path's indexed draw: this run's header and records (scene.slang). Read by
  // cull_vertex_main only.
  uint64_t vertex_draw;     // gfx::VertexDrawHeader, as sixteen words
  uint64_t vertex_records;  // uint4[pair_count]: {instance, cluster, first triangle, overflowed}
  // The visibility id names a pair, not an entry (scene.slang, `pair_of`), so the resolve needs a
  // way back from the pair to the entry this frame drew it as: this view's table, one word per
  // pair, written for every pair drawn. 0 writes none. The two bases say where `visible[0]` and
  // `sw_visible[0]` are in the whole list, which is what an entry index is counted from.
  uint64_t pair_entries;  // uint[pair_count]
  uint visible_base;
  uint sw_visible_base;
  // Terrain levels (scene.slang, TerrainLevelDesc): the frame's table, or 0. A cluster of a level
  // whose sphere is wholly inside the level's hole is dropped: the finer level draws that ground.
  uint64_t terrain;
  uint64_t coverage;  // uint per cluster: geometry::pack_lod_coverage, or 0 for none (see cull_pair)
  // The frame's origin (scene.slang): every instance is moved into its space before any test, and
  // the planes, `camera` and `view_proj` above are in that space.
  FrameEye eye;
  // Alpha-tested materials (scene.slang, MaskTables): set, and `eye.masks` 1, only in a frame with
  // one. A masked pair is never sent to the software rasterizer (see cull_pair).
  MaskTables masks;
  // Shadow casters outside the frustum (see cull_pair): convex volumes of inward planes in the
  // frame's space, each also inside the sphere `caster_reach[v]` when its w is not negative. Read
  // only with `casters` set and only by a pair the frustum test rejected.
  float4 caster_planes[k_max_caster_volumes][k_caster_volume_planes];
  float4 caster_reach[k_max_caster_volumes];
  uint caster_plane_counts[k_max_caster_volumes];
  uint caster_volume_count;
  float foliage_px;  // the coverage errors' threshold
  float fade;        // the cross-fade's half band, a fraction of the threshold; 0 switches at once
  float caster_horizon;  // gfx::CullParams::caster_horizon: no caster farther from the eye; 0 none
  uint horizon_pad0;
  uint horizon_pad1;
  uint horizon_pad2;
  uint cascade;      // 0 a camera run; c + 1 the sun's cascade c (the caster policy, below)
  uint pad0;
  uint pad1;
  uint pad2;
};

struct CullPush {
  uint64_t params;
};

[[vk::push_constant]] ConstantBuffer<CullPush> g_push;

static const float k_terminal_error = 3.402823466e+38;

float projected_error(float4 sphere, float error, float3 camera, float znear, float proj_scale) {
  if (error >= k_terminal_error) return k_terminal_error;
  const float distance = length(sphere.xyz - camera) - sphere.w;
  return error / max(distance, znear) * proj_scale;
}

// ---- the foliage error and the cross-fade (geometry.md, "Two errors"; renderer.md, "The
// cross-fade"; R91) ---------------------------------------------------------------------------------
//
// `geometry::lod_selects` with coverage, and `geometry::lod_fade_split`, the same formulas: a
// transition is acceptable when its geometric error projects to the threshold or under, or its
// coverage error to the foliage threshold or under; with a fade, the split is the smaller of the two
// projected errors over its threshold, mapped from (1 - fade, 1 + fade) to (0, 1). A cluster with no
// coverage reads +inf, which projects to the terminal error and is never acceptable by it, so it
// takes the one-threshold test bit for bit.
bool transition_acceptable(float geometric, float coverage, float threshold, float foliage) {
  return geometric <= threshold || coverage <= foliage;
}

float over_threshold(float projected, float threshold) {
  if (!(projected > 0.0)) return 0.0;
  if (!(threshold > 0.0)) return k_terminal_error;
  return projected / threshold;
}

float fade_split(float geometric, float coverage, bool fading, float threshold, float foliage,
                 float fade, bool step_shift) {
  if (!fading) return transition_acceptable(geometric, coverage, threshold, foliage) ? 0.0 : 1.0;
  const float ratio = min(over_threshold(geometric, threshold), over_threshold(coverage, foliage));
  if (step_shift) return ratio <= 1.0 - fade ? 0.0 : 1.0;
  return clamp((ratio - (1.0 - fade)) / (2.0 * fade), 0.0, 1.0);
}

uint pack_splits(float own, float parent) {
  return uint(own * 65535.0 + 0.5) | (uint(parent * 65535.0 + 0.5) << 16);
}

// ---- geometry streaming (docs/plan/04-renderer.md §4.9, domain/geometry/cluster_pages.h) ------
//
// `geometry::select_lod_streaming`'s rule, one thread per pair instead of one loop over the mesh:
//
//   draw c  <=>  resident(page of c)
//                && projected parent error > threshold
//                && (projected own error <= threshold || !resident(pages of c's children))
//
// Both residency tests are over a whole *group* in the CPU model, and a group is always exactly
// one page: `build_cluster_pages` closes the open page before a group that would not fit, and
// gives a group that fits in no page at all one of its own, so `page_of_cluster` is constant
// across a group and the per-cluster test **is** the group test. `merge_paged_cluster_meshes`
// shifts a mesh's pages as a block and never links two meshes, so it stays true for a scene. The
// CPU side checks it before it uploads a page table (renderer::GeometryStreamer), because the rule
// is crack-free only while it holds.

bool page_resident(StreamParams* stream, uint page) {
  uint* resident = (uint*)stream->residency;
  return resident[page] != 0;
}

// geometry::screen_pixels: `projected_error`'s projection applied to the sphere's own size rather
// than to an error, so a request's priority and the LOD threshold are in the same units and the
// same view. And geometry::sphere_distance: observer to the near side, never below the near plane.
float projected_size(float3 center, float radius, float3 camera, float znear, float proj_scale) {
  const float distance = length(center - camera) - radius;
  return 2.0 * radius / max(distance, znear) * proj_scale;
}

float sphere_distance(float3 center, float radius, float3 camera, float znear) {
  const float distance = length(center - camera) - radius;
  return distance > znear ? distance : znear;
}

// One entry per page per frame. The mask word is **exchanged** rather than compared: the first
// lane to claim a page writes the entry and every other lane pays one atomic and leaves, so a page
// a thousand clusters want costs one twelve-byte entry instead of a thousand — which is what keeps
// `max_requests` a small constant rather than a function of the scene.
//
// The priority the entry carries is therefore the *winning* lane's and not the best of the
// requesters', where the CPU reference keeps the largest screen size and the smallest distance.
// That is a deliberate trade: a lock-free best-of would be two more atomics on every losing lane,
// and it buys nothing over a frame or two, because `PageResidencyManager::request` keeps the best
// of every request it sees and a queued page's key can only ever improve.
//
// A request past the buffer's capacity is dropped rather than wrapped. The counter still counts it,
// so the CPU can report that the frame overflowed, and the page is asked for again next frame
// because the mask is cleared with the rest of the feedback.
void request_page(CullParams* params, StreamParams* stream, uint page, float screen_px,
                  float distance) {
  Atomic<uint>* mask = (Atomic<uint>*)(stream->request_mask + uint64_t(page) * 4);
  if (mask->exchange(1u) != 0u) return;
  Atomic<uint>* count = (Atomic<uint>*)stream->request_count;
  const uint slot = count->add(1u);
  if (slot >= params->max_requests) return;
  PageRequest* requests = (PageRequest*)stream->requests;
  requests[slot].page = page;
  requests[slot].screen_px = screen_px;
  requests[slot].distance = distance;
}

// True when every page holding the run of clusters `first .. first + count` is resident, and —
// when `emit` is set — asking for the ones that are not. A run is a range of pages because the
// builder keeps a group in consecutive pages, so this is the CPU's `run_resident` exactly.
bool run_resident(CullParams* params, StreamParams* stream, uint first, uint count, bool emit,
                  float screen_px, float distance) {
  if (count == 0) return true;
  uint* page_of = (uint*)stream->page_of_cluster;
  const uint from = page_of[first];
  const uint to = page_of[first + count - 1];
  bool complete = true;
  for (uint page = from; page <= to; ++page) {
    if (page_resident(stream, page)) continue;
    complete = false;
    if (emit) request_page(params, stream, page, screen_px, distance);
  }
  return complete;
}

// True when every pixel the sphere's screen rectangle touches holds a surface nearer than the
// sphere's nearest point. Conservative: anything touching the near plane counts as visible.
bool occluded(CullParams* params, float3 center, float radius) {
  float2 lo = float2(1e30, 1e30);
  float2 hi = float2(-1e30, -1e30);
  for (uint i = 0; i < 8; ++i) {
    const float3 corner = center + radius * float3((i & 1) != 0 ? 1.0 : -1.0, (i & 2) != 0 ? 1.0 : -1.0,
                                                   (i & 4) != 0 ? 1.0 : -1.0);
    const float4 clip = mul(params->view_proj, float4(corner, 1.0));
    if (clip.w <= 1e-5) return false;
    const float2 ndc = clip.xy / clip.w;
    lo = min(lo, ndc);
    hi = max(hi, ndc);
  }
  const float3 to_camera = params->camera.xyz - center;
  const float distance = length(to_camera);
  if (distance <= radius) return false;
  const float4 nearest = mul(params->view_proj, float4(center + to_camera * (radius / distance), 1.0));
  if (nearest.w <= 1e-5) return false;
  const float depth = nearest.z / nearest.w;  // reversed Z: larger is nearer
  if (depth >= 1.0) return false;

  const float w = float(params->hiz_width);
  const float h = float(params->hiz_height);
  const int ix0 = int(clamp(floor((lo.x * 0.5 + 0.5) * w), 0.0, w - 1.0));
  const int ix1 = int(clamp(floor((hi.x * 0.5 + 0.5) * w), 0.0, w - 1.0));
  const int iy0 = int(clamp(floor((0.5 - hi.y * 0.5) * h), 0.0, h - 1.0));
  const int iy1 = int(clamp(floor((0.5 - lo.y * 0.5) * h), 0.0, h - 1.0));
  // Pick the mip whose texels are at least a quarter of the rectangle, so the footprint is at
  // most 5x5 texels and stays close to the rectangle (a 2x2 footprint at the next coarser
  // mips would cover up to four times the area and touch far-away depth, culling less).
  const uint size = uint(max(ix1 - ix0, iy1 - iy0)) + 1;
  const uint coarse = size <= 1 ? 0 : firstbithigh(size - 1) + 1;  // 2^coarse >= size
  uint mip = coarse >= 2 ? coarse - 2 : 0;
  mip = min(mip, params->hiz_mips - 1);
  const uint mip_w = (params->hiz_width + (1u << mip) - 1) >> mip;
  const uint mip_h = (params->hiz_height + (1u << mip) - 1) >> mip;
  const uint tx0 = uint(ix0) >> mip;
  const uint tx1 = min(uint(ix1) >> mip, mip_w - 1);
  const uint ty0 = uint(iy0) >> mip;
  const uint ty1 = min(uint(iy1) >> mip, mip_h - 1);
  float* hiz = (float*)params->hiz;
  const uint base = params->hiz_offsets[mip];
  float farthest = 1e30;
  for (uint ty = ty0; ty <= ty1; ++ty) {
    for (uint tx = tx0; tx <= tx1; ++tx) {
      farthest = min(farthest, hiz[base + ty * mip_w + tx]);
    }
  }
  return depth < farthest;
}

// meshoptimizer's cone test on the snorm8 packing (geometry::cluster_backfacing is the CPU
// reference): every triangle faces away when the direction from the camera to the apex lies
// within the cone. A cutoff of 1 marks a cluster without a usable cone. `apex` and `axis` are
// the cluster's, already moved into world space by the instance's transform.
bool backfacing(ClusterDesc cluster, float3 apex, float3 axis, float3 camera) {
  const float cutoff = float(int(cluster.cone) >> 24) / 127.0;
  if (cutoff >= 1.0) return false;
  const float3 to_apex = apex - camera;
  const float distance = length(to_apex);
  if (distance <= 1e-12) return false;
  return dot(to_apex * (1.0 / distance), axis) >= cutoff;
}

float3 cone_axis(ClusterDesc cluster) {
  return float3(int(cluster.cone << 24) >> 24, int(cluster.cone << 16) >> 24,
                int(cluster.cone << 8) >> 24) / 127.0;
}

// True when the sphere is inside one of the caster volumes, by the frustum test's rule: outside
// only when wholly outside one plane, or wholly outside the volume's reach (renderer's
// caster_volume.h, `caster_volume_contains_sphere`, is the same test on the CPU).
bool in_caster_volume(CullParams* params, float3 center, float radius) {
  for (uint v = 0; v < params->caster_volume_count; ++v) {
    bool inside = true;
    const uint count = params->caster_plane_counts[v];
    for (uint p = 0; p < count; ++p) {
      const float4 plane = params->caster_planes[v][p];
      if (dot(plane.xyz, center) + plane.w < -radius) {
        inside = false;
        break;
      }
    }
    const float4 reach = params->caster_reach[v];
    if (inside && reach.w >= 0.0 && length(center - reach.xyz) > reach.w + radius) inside = false;
    if (inside) return true;
  }
  return false;
}

// **The caster horizon** (renderer.md, "Shadows"; R94): a caster whose sphere is farther from the
// eye — the frame's origin — than `caster_horizon` casts nothing, and is counted in the second word
// of the view's software argument block, beside the casters' own count, which a frame with casters
// never dispatches.
bool beyond_caster_horizon(CullParams* params, float3 center, float radius) {
  if (!(params->caster_horizon > 0.0) || length(center) - radius <= params->caster_horizon) {
    return false;
  }
  Atomic<uint>* dropped = (Atomic<uint>*)(params->caster_count + 4);
  dropped->add(1);
  return true;
}

// A hardware survivor's share of its run's indexed draw (gfx::VertexDrawHeader): its triangles are
// allocated under the cursor, its record says where they start, and vertex_expand.slang is given
// one workgroup for it. A survivor that does not fit is flagged instead, pulls the indexed draw's
// end back to where it would have started, and is drawn by the fallback — a region that runs out
// costs time, never a hole. Allocation is in atomic order, so everything allocated after the first
// survivor that did not fit did not fit either, and the survivors that did are one prefix.
void allocate_vertex_draw(uint64_t header_address, uint64_t records_address, uint slot,
                          uint instance, uint cluster, uint triangles) {
  uint* header = (uint*)header_address;
  Atomic<uint>* words = (Atomic<uint>*)header_address;
  const uint first = words[k_vertex_draw_cursor].add(triangles);
  const uint capacity = header[k_vertex_draw_capacity];
  const bool fits = first <= capacity && triangles <= capacity - first;
  uint4* records = (uint4*)records_address;
  records[slot] = uint4(instance, cluster, first, fits ? 0u : 1u);
  words[k_vertex_draw_expand_groups].add(1u);
  if (!fits) {
    words[k_vertex_draw_fit_end].min(first);
    words[k_vertex_draw_fallback_instances].max(slot + 1u);
    words[k_vertex_draw_overflow].add(1u);
  }
}

// The pass for one pair. `vertex_draw` is a literal at both entry points below, so each is its own
// program: the vertex path's bookkeeping is compiled out of `cull_main` rather than branched
// around, because code in the survivor tail costs every thread of the dispatch and not only the
// survivors (measured on the RTX 5090: 2 µs on a 10 µs pass whatever the survivor count).
[ForceInline]
void cull_pair(uint pair, bool vertex_draw) {
  CullParams* params = (CullParams*)g_push.params;
  if (pair >= params->pair_count) return;

  if (params->pass == 1) {
    uint* prev = (uint*)params->prev_flags;
    if (prev[pair] == 0) return;
  } else if (params->pass == 2) {
    uint* flags = (uint*)params->flags;
    if (flags[pair] != 0) return;  // drawn in pass 1
  }

  InstanceDesc* instances = (InstanceDesc*)params->instances;
  const uint instance_index = instance_of_pair(instances, params->instance_count, pair);
  const InstanceDesc instance = instances[instance_index];
  // **The caster policy** (renderer.md, "The caster policy"; R95): an instance whose species casts
  // into the `n` nearest cascades alone (flags bits 2-4 hold n + 1; 0 is every cascade) is no
  // pair of a farther cascade's run, before anything about its cluster is read. Grass in the
  // nearest cascade only is what keeps a meadow's shadow from costing every cascade its blades.
  if (params->cascade != 0) {
    const uint nearest = (instance.flags & k_instance_cascade_mask) >> k_instance_cascade_shift;
    if (nearest != 0 && params->cascade >= nearest) return;
  }
  MeshDesc* meshes = (MeshDesc*)params->meshes;
  const MeshDesc mesh = meshes[instance.mesh];
  // A **hole** of a streamed world's pair space (docs/subsystems/renderer.md, "Instances that come
  // and go"): the slack a tile's block keeps past its instances' pairs, or a freed block, whose
  // instance slots are null instances of a mesh with no clusters. The search lands on the last
  // instance at or below the pair, and the pair is past that instance's clusters, so it is
  // rejected here, before anything about a cluster is read. A scene read whole has no holes and
  // never takes this branch.
  const uint local = pair - instance.first_pair;
  if (local >= mesh.cluster_count) return;
  const uint index = mesh.first_cluster + local;

  ClusterDesc* clusters = (ClusterDesc*)params->clusters;
  const ClusterDesc cluster = clusters[index];
  // Everything below is in the frame's space, whose origin is the eye (scene.slang): the instance's
  // translation from the eye once, and every point of it through `instance_point`.
  const float3 from_eye = instance_from_eye(instance, params->eye);
  const float3 center = instance_point(instance, from_eye, cluster.center);
  // A deformed instance's vertices leave their rest positions, so every sphere this pass tests is
  // inflated by the caller's bound on that displacement, in world units. It is zero for a rigid
  // instance, and adding zero to a float is exact, so nothing about the rigid path changes.
  // The same constant goes on the LOD spheres below, which is what keeps the cut crack-free: a
  // cluster's parent sphere is its children's own sphere, so one constant per instance still
  // leaves exactly one cluster per DAG path passing (docs/subsystems/renderer.md).
  float padding = instance.bounds_padding * instance.scale_max;
  // A **terrain level** (scene.slang, TerrainLevelDesc) takes its padding from the frame's table,
  // where it follows the fields as they move away from the rest heights the spheres were fit to,
  // and a cluster of it wholly inside the square the next finer level covers draws nothing the
  // picture keeps: every vertex of it would be moved onto the square's edge by the pool pass
  // (deform.slang's terrain stage). A level moves its vertices up and down only, so the test is on
  // the rest sphere's footprint, and it is the same test for every run and view.
  if (instance.terrain != 0 && params->terrain != 0) {
    TerrainLevelDesc* levels = (TerrainLevelDesc*)params->terrain;
    const TerrainLevelDesc level = levels[instance.terrain - 1];
    padding = max(padding, level.padding);
    // The hole is the scene grid's alone (`spacing_mm` 0; a world-lattice level's tiles leave out
    // what they leave out by their own geometry), in the grid's mesh space, which the grid at cell
    // zero shares with its cell's frame; the test takes the cluster there and not in the frame's
    // space, so it is the same test wherever the eye is.
    const float4 hole = level.hole;
    const float3 at = instance_cell_point(instance, cluster.center);
    const float footprint = cluster.radius * instance.scale_max;
    if (level.spacing_mm == 0 && hole.z > hole.x && at.x - footprint >= hole.x &&
        at.x + footprint <= hole.z && at.z - footprint >= hole.y && at.z + footprint <= hole.w) {
      return;
    }
  }
  const float radius = cluster.radius * instance.scale_max + padding;

  // **A caster outside the frustum** (renderer.md, "Shadows"). The shadow rays meet only what the
  // frame builds, so a pair the frustum drops would stop casting into the view the moment its
  // cluster left it: a tree behind the camera threw no shadow forward, and an object walking out
  // of the picture had its shadow cut by the frustum's edge, cluster by cluster. With `casters`
  // set, such a pair is tested against the view's caster volumes — every point from which a ray
  // towards a shadowed light can reach the view — and inside one it is a caster, which goes on
  // through the cut and the page table below like any caster. A deformed instance is one too since
  // 2026-10-18 — a skinned character, a `--deform` mesh, and every terrain level: the caster run is
  // allocated pool blocks and deformed like a drawn run, and its CLAS records read them (an entry
  // the budget refused is built not at all, `clas_records.slang`). The padded sphere bounds it
  // here as it does a drawn pair.
  bool caster = false;
  if (params->lod.w > 0.0) {
    bool outside = false;
    for (uint p = 0; p < params->plane_count; ++p) {
      const float4 plane = params->planes[p];
      if (dot(plane.xyz, center) + plane.w < -radius) {
        outside = true;
        break;
      }
    }
    if (outside) {
      if (params->casters == 0 || params->caster_volume_count == 0 ||
          !in_caster_volume(params, center, radius)) {
        return;
      }
      caster = true;
    }
  }
  // The cone is a promise about the **rest** triangles, moved by a transform that keeps angles, so
  // it is tested only where both hold. A non-uniform scale shears the cone open. A deformed
  // instance — skinned, morphed, or under `--deform` — draws every position out of the frame's
  // pool, and a turned joint can bring round a cluster whose rest-pose cone still says it faces
  // away: trusting that cone cuts a hole in the character where it faces the viewer. Its clusters
  // are therefore never cone-tested (docs/subsystems/geometry.md, "Normal cones"). It is checked
  // on the instance rather than on whether this frame gave the entry a pool block, because the
  // pool is allocated from this pass's own output.
  //
  // **Facing away from the camera is not facing away from a light.** The shadow rays trace the
  // structures this frame builds from the cull output, so a cluster dropped here also stops
  // casting: a card or a leaf seen from behind throws no shadow at all, and a floating box lit
  // from one side and seen from the other loses every shadow ray that enters by its bottom and
  // leaves by its lit side (docs/subsystems/geometry.md, "Normal cones"). With `casters` set the
  // pair is kept as a **shadow caster** instead: it takes every test below exactly as a drawn pair
  // does, so it is in the same cut at the same LOD, and it ends in a run of its own that no
  // rasterizer reads. A pair already kept as a caster outside the frustum needs no cone.
  if (!caster && params->cone_cull != 0 && (instance.flags & k_instance_uniform_scale) != 0 &&
      instance.deform == k_invalid_deform) {
    const float3 apex = instance_point(instance, from_eye, cluster.cone_apex);
    const float3 axis = normalize(instance_direction(instance, cone_axis(cluster)));
    if (backfacing(cluster, apex, axis, params->camera.xyz)) {
      if (params->casters == 0) return;
      caster = true;
    }
  }
  // The page this cluster's payload is in. Streaming is only ever on together with LOD selection
  // (renderer::resolve_settings forces the cut on with it), because the fallback clause of the
  // drawing rule *is* the LOD test: without a cut there is nothing for a missing page to fall back
  // to.
  StreamParams* stream = (StreamParams*)params->streaming;
  uint page = 0;
  if (params->streaming != 0) {
    uint* page_of = (uint*)stream->page_of_cluster;
    page = page_of[index];
  }
  // The splits this pair keeps between (the cross-fade's; 0 and 1 outside every band).
  float split_own = 0.0;
  float split_parent = 1.0;
  if (params->lod.z > 0.0) {
    ClusterLodDesc* lods = (ClusterLodDesc*)params->lods;
    const ClusterLodDesc lod = lods[index];
    // The coverage word (geometry::pack_lod_coverage), +inf in both halves for none.
    const uint cover = params->coverage != 0 ? ((uint*)params->coverage)[index] : 0x7C007C00u;
    const float4 own_sphere = float4(instance_point(instance, from_eye, lod.own.xyz),
                                     lod.own.w * instance.scale_max + padding);
    const float4 parent_sphere = float4(instance_point(instance, from_eye, lod.parent.xyz),
                                        lod.parent.w * instance.scale_max + padding);
    const float own = projected_error(own_sphere, lod.own_error * instance.scale_max,
                                      params->camera.xyz, params->camera.w, params->lod.x);
    const float parent =
        projected_error(parent_sphere, lod.parent_error * instance.scale_max, params->camera.xyz,
                        params->camera.w, params->lod.x);
    const float own_cover_error = f16tof32(cover & 0xffffu);
    const float parent_cover_error = f16tof32(cover >> 16);
    const float own_cover = projected_error(own_sphere, own_cover_error * instance.scale_max,
                                            params->camera.xyz, params->camera.w, params->lod.x);
    const float parent_cover =
        projected_error(parent_sphere, parent_cover_error * instance.scale_max, params->camera.xyz,
                        params->camera.w, params->lod.x);
    const bool own_ok =
        transition_acceptable(own, own_cover, params->lod.y, params->foliage_px);
    const bool parent_ok =
        transition_acceptable(parent, parent_cover, params->lod.y, params->foliage_px);
    if (params->streaming == 0) {
      // The cross-fade, for a transition with a coverage error: both sides draw across the band and
      // each keeps the pixels between its splits (alpha_mask.slang, ade_keeps).
      const bool own_fading = params->fade > 0.0 && own_cover_error < k_terminal_error;
      const bool parent_fading = params->fade > 0.0 && parent_cover_error < k_terminal_error;
      if (own_fading || parent_fading) {
        const bool opaque = !pair_masked(params->masks, instance, index);
        split_own = fade_split(own, own_cover, own_fading, params->lod.y, params->foliage_px,
                               params->fade, opaque);
        split_parent = fade_split(parent, parent_cover, parent_fading, params->lod.y,
                                  params->foliage_px, params->fade, opaque);
        if (!(split_own < split_parent)) return;
      } else if (!(own_ok && !parent_ok)) {
        return;
      }
    } else {
      const float screen_px =
          projected_size(center, radius, params->camera.xyz, params->camera.w, params->lod.x);
      const float distance = sphere_distance(center, radius, params->camera.xyz, params->camera.w);
      if (!page_resident(stream, page)) {
        // A group with no coarser version has no parent to ask on its behalf, so it asks for
        // itself: without the coarsest level there is nothing to draw at all. In a scene whose
        // root pages are pinned this never fires — it is here because the CPU reference does it,
        // and the two have to agree on a residency a test made up.
        if (lod.parent_error >= k_terminal_error)
          request_page(params, stream, page, screen_px, distance);
        return;
      }
      if (parent_ok) return;  // the coarser version is good enough
      if (!own_ok) {
        // Too coarse for this view: refine into the children, unless their pages are not all
        // here, in which case this cluster is the best there is and the missing pages are asked
        // for. A level-0 cluster has no children and is simply not drawn, exactly as an
        // unstreamed cut would not draw it.
        ClusterChildren* children = (ClusterChildren*)stream->children;
        const ClusterChildren kids = children[index];
        if (run_resident(params, stream, kids.first_cluster, kids.cluster_count, true, screen_px,
                         distance)) {
          return;
        }
      }
    }
  }
  if (params->hiz != 0 && occluded(params, center, radius)) return;
  // The cut read this page: the CPU manager's LRU is only as good as this bit, because a page
  // nothing asks for and nothing draws out of is exactly the page that should go first.
  if (params->streaming != 0) {
    uint* used = (uint*)stream->used;
    used[page] = 1u;
  }
  // The cross-fade's table (alpha_mask.slang, `fade_keeps`): every pair this view draws or casts
  // with, its two splits and its instance, read by the masked readers of this frame only.
  if (params->masks.fade != 0) {
    uint2* fade = (uint2*)params->masks.fade;
    fade[pair] = uint2(pack_splits(split_own, split_parent), instance_index);
  }
  // A caster is in the frame's ray tracing geometry and nowhere else. It is not drawn, so it is
  // not in the drawn-this-frame flags either — and a frame with casters has no second occlusion
  // pass to read them, because the ray tracing chain turns occlusion culling off.
  if (caster) {
    // Past the horizon a caster that passed every other test is counted instead of kept, so the
    // casters kept and the casters past it are the casters there would have been.
    if (beyond_caster_horizon(params, center, radius)) return;
    // An instance the policy keeps out of the ray tracing chain (`k_instance_untraced`) casts no
    // traced shadow, so it is no caster: the chain would build it only to have nothing trace it.
    if ((instance.flags & k_instance_untraced) != 0) return;
    Atomic<uint>* count = (Atomic<uint>*)params->caster_count;
    const uint slot = count->add(1);
    uint2* casters = (uint2*)params->casters;
    casters[slot] = uint2(instance_index, index);
    return;
  }
  if (params->pass != 0) {
    uint* flags = (uint*)params->flags;
    flags[pair] = 1;
  }

  // Hardware or software rasterization: by projected size unless the mode forces one path.
  bool software = params->raster.y == 1.0;
  if (params->raster.y == 2.0) {
    const float distance = max(length(center - params->camera.xyz) - radius, params->camera.w);
    const float diameter_px = 2.0 * radius / distance * params->lod.x;
    software = diameter_px < params->raster.x;
    // **An alpha-tested pair is the hardware rasterizers' alone** (renderer.md, "Alpha-tested
    // geometry"): the software rasterizer writes the buffer from compute, a thread a triangle
    // walking its box, and testing a mask there is a UV interpolation and a texture fetch per
    // covered pixel with no quad to take a mip from. Only a pair that would have gone software pays
    // the two reads, and a frame with no masked material reads nothing (`masks.materials` 0).
    if (software && pair_masked(params->masks, instance, index)) software = false;
  }
  uint entry;
  if (software) {
    Atomic<uint>* count = (Atomic<uint>*)params->sw_args;
    const uint slot = count->add(1);
    uint2* visible = (uint2*)params->sw_visible;
    visible[slot] = uint2(instance_index, index);
    entry = params->sw_visible_base + slot;
  } else {
    Atomic<uint>* count = (Atomic<uint>*)(params->draw_args + uint64_t(params->count_index) * 4);
    const uint slot = count->add(1);
    uint2* visible = (uint2*)params->visible;
    visible[slot] = uint2(instance_index, index);
    entry = params->visible_base + slot;
    if (vertex_draw) {
      allocate_vertex_draw(params->vertex_draw, params->vertex_records, slot, instance_index,
                           index, cluster.triangle_count);
    }
  }
  // The slot is whichever the append handed out, so the entry's index is not a function of the
  // frame; the visibility id is the pair, which is, and this is the way back from it to the entry
  // (docs/subsystems/gfx.md, "The tie rule"). A pair is drawn at most once per view per frame —
  // pass 2 skips what pass 1 drew, the software and hardware runs split the survivors, a caster is
  // not drawn — so one word per pair per view is enough, and a word left over from an earlier
  // frame is never read: only a pair this frame drew can be under a pixel.
  if (params->pair_entries != 0) {
    uint* pair_entries = (uint*)params->pair_entries;
    pair_entries[pair] = entry;
  }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void cull_main(uint3 thread_id : SV_DispatchThreadID) {
  cull_pair(thread_id.x, false);
}

// The vertex path's cull: the same pass, and every hardware survivor also gets its triangles in
// this run's indexed draw (gfx::VertexDrawHeader). Needs `count_index` = 1.
[shader("compute")]
[numthreads(64, 1, 1)]
void cull_vertex_main(uint3 thread_id : SV_DispatchThreadID) {
  cull_pair(thread_id.x, true);
}
