// The vertex path's index expansion (docs/subsystems/gfx.md, "Baseline tier"): between the cull
// pass and the vertex path's indexed draw, one workgroup per hardware survivor of the run writes
// its triangles' indices into the run's region (gfx::VertexDrawHeader), three u32 a triangle,
// each `slot << 8 | local vertex`. The slot in the high bits is what makes an index name one
// cluster's vertex in one visible entry, so the post-transform cache shares a cluster's vertices
// between its triangles and never between two entries; the local byte is the cluster's own.
//
// It is a pass of its own, and not a loop in the cull pass's survivor tail, because there the
// writes are one thread per survivor each striding over its own cluster: on the TITAN Xp that loop
// cost the cull pass 0.08 ms at 6,000 survivors, and a workgroup per survivor writes the same
// indices in consecutive lanes. Dispatched indirectly from the count the cull pass kept beside the
// cursor, so workgroup g is slot g and the pass costs the cut, not the scene.
//
// Workgroup 0 also finishes the draw's arguments: the index count is three times the triangles
// that fit, which is the cursor unless a survivor overflowed, and then the start of the first one
// that did (the cull pass's `fit_end`).

#include "scene.slang"

// Mirrors gfx::VertexExpandParams (40 bytes).
struct ExpandParams {
  uint64_t header;     // gfx::VertexDrawHeader of the run, as sixteen words
  uint64_t records;    // uint4 per slot: {instance, cluster, first triangle, overflowed}
  uint64_t indices;    // uint[3 * capacity]: the run's index buffer
  uint64_t clusters;   // ClusterDesc[]
  uint64_t triangles;  // three local indices packed in a uint
};

[[vk::push_constant]] ConstantBuffer<ExpandParams> g_params;

static const uint k_expand_workgroup = 64;

[shader("compute")]
[numthreads(64, 1, 1)]
void expand_main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
  const uint slot = group.x;
  uint* header = (uint*)g_params.header;
  if (slot == 0 && thread.x == 0) {
    header[k_vertex_draw_index_count] =
        3 * min(header[k_vertex_draw_cursor], header[k_vertex_draw_fit_end]);
  }
  uint4* records = (uint4*)g_params.records;
  const uint4 record = records[slot];  // {instance, cluster, first triangle, overflowed}
  if (record.w != 0) return;           // the fallback draws it
  ClusterDesc* clusters = (ClusterDesc*)g_params.clusters;
  const ClusterDesc cluster = clusters[record.y];
  uint* triangles = (uint*)g_params.triangles;
  uint* indices = (uint*)g_params.indices;
  const uint high = slot << 8;
  for (uint k = thread.x; k < cluster.triangle_count; k += k_expand_workgroup) {
    const uint packed = triangles[cluster.triangle_offset + k];
    const uint at = (record.z + k) * 3;
    indices[at] = high | (packed & 0xff);
    indices[at + 1] = high | ((packed >> 8) & 0xff);
    indices[at + 2] = high | ((packed >> 16) & 0xff);
  }
}
