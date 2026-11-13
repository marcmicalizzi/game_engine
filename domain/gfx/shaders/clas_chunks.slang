// What each chunk of the frame's cluster set builds (docs/subsystems/renderer.md, "The frame's set
// stands in chunks"; R152). The frame's cluster structures are built by one command a chunk, each
// into a buffer of its own, so that the set grows by the chunks it lacks instead of being made
// again whole; a build reads its count from the device, and the frame has one count — the records
// pass's word 0, which the cache's assign pass may still raise — so this writes each chunk's share
// of it after every pass that moves it and before the first build. Chunk k holds the records
// [k * chunk_clusters, (k + 1) * chunk_clusters) of the first `capacity`; a thread a chunk, one
// workgroup.

struct ClusterChunkParams {  // gfx::ClusterChunkParams
  uint64_t record_count;
  uint64_t counts;
  uint chunks;
  uint chunk_clusters;
  uint capacity;
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<ClusterChunkParams> g_params;

[shader("compute")]
[numthreads(64, 1, 1)]
void clas_chunks_main(uint3 thread_id : SV_DispatchThreadID) {
  const uint k = thread_id.x;
  if (k >= g_params.chunks) return;
  // Never more than the chunks hold: the records pass keeps to the capacity it was given, and this
  // holds the builds to it whatever a later pass wrote.
  const uint built = min(((uint*)g_params.record_count)[0], g_params.capacity);
  const uint first = k * g_params.chunk_clusters;
  const uint room = min(g_params.chunk_clusters, g_params.capacity - min(first, g_params.capacity));
  ((uint*)g_params.counts)[k] = built > first ? min(built - first, room) : 0u;
}
