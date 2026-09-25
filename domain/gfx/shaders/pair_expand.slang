// The pair table of a streamed world's changed blocks, written on the GPU from the instance table
// (docs/subsystems/renderer.md, "Instances that come and go"). The pair table is the inverse of
// `pair_of`: entry `first_pair + c` of an instance is `{instance, first_cluster + c}` of its mesh,
// which is what the resolve decodes a pixel's pair through (`ResolveParams::pairs`). It used to be
// built on the CPU for the whole tail and uploaded on every change — 66 MB for E33's ashlar kit,
// most of a 15 ms change — when the instance table the change had to upload anyway already says
// everything the entries hold. So the CPU uploads a changed block's instances and a list of the
// ones whose pairs to write, and this pass writes them: one workgroup per listed instance, its
// lanes striding over the mesh's clusters, so a 5,863-cluster ashlar member is 92 coalesced
// iterations and a one-cluster box one.
//
// Nothing here decides anything. The entries a hole covers — a block's slack past its instances,
// a freed block — are never listed and keep whatever they held: nothing reads the pair table but
// the resolve, and only for a pair the frame drew, which a hole never is.

#include "scene.slang"

// Mirrors renderer::PairExpandParams (40 bytes).
struct PairExpandParams {
  uint64_t instances;  // InstanceDesc[]: the table set this frame reads, already uploaded
  uint64_t meshes;     // MeshDesc[]
  uint64_t list;       // uint[count]: the instances whose pairs to write
  uint64_t pairs;      // uint2[]: the same set's pair table
  uint count;
  uint pad;
};

[[vk::push_constant]] ConstantBuffer<PairExpandParams> g_params;

static const uint k_pair_expand_workgroup = 64;

[shader("compute")]
[numthreads(64, 1, 1)]
void pair_expand_main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
  if (group.x >= g_params.count) return;
  uint* list = (uint*)g_params.list;
  const uint index = list[group.x];
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  const InstanceDesc instance = instances[index];
  MeshDesc* meshes = (MeshDesc*)g_params.meshes;
  const MeshDesc mesh = meshes[instance.mesh];
  uint2* pairs = (uint2*)g_params.pairs;
  for (uint c = thread.x; c < mesh.cluster_count; c += k_pair_expand_workgroup) {
    pairs[instance.first_pair + c] = uint2(index, mesh.first_cluster + c);
  }
}
