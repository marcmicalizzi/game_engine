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

#include "dispatch.slang"
#include "scene.slang"

// All three passes are folded dispatches (gfx::fold_dispatch, T160): a first upload lists every
// instance and a numbering may move a few hundred thousand slots, past the 65,535 workgroups a
// dimension Vulkan guarantees.

// Mirrors renderer::PairExpandParams (40 bytes).
struct PairExpandParams {
  uint64_t instances;  // InstanceDesc[]: the table set this frame reads, already uploaded
  uint64_t meshes;     // MeshDesc[]
  uint64_t list;       // uint[count]: the instances whose pairs to write; or uint4 runs, below
  uint64_t pairs;      // uint2[]: the same set's pair table
  uint count;
  uint slots;          // pair_expand_runs_main: the runs' slots in all
};

[[vk::push_constant]] ConstantBuffer<PairExpandParams> g_params;

static const uint k_pair_expand_workgroup = 64;

// One instance's pairs: `{index, first_cluster + c}` at `first_pair + c`. A null instance's mesh has
// no clusters and writes nothing.
void expand_instance(uint index, uint lane) {
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  const InstanceDesc instance = instances[index];
  MeshDesc* meshes = (MeshDesc*)g_params.meshes;
  const MeshDesc mesh = meshes[instance.mesh];
  uint2* pairs = (uint2*)g_params.pairs;
  for (uint c = lane; c < mesh.cluster_count; c += k_pair_expand_workgroup) {
    pairs[instance.first_pair + c] = uint2(index, mesh.first_cluster + c);
  }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void pair_expand_main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
  const uint index = folded_group(group);
  if (index >= g_params.count) return;
  uint* list = (uint*)g_params.list;
  expand_instance(list[index], thread.x);
}

// ---- the pairs numbered again where the slots stand (T154; renderer.md, "The pairs are numbered
// again, and no slot moves") ----------------------------------------------------------------------
//
// A numbering moves whole runs of slots: a block's by its distance, a free run's to its one number.
// The CPU says so as `renderer::PairShift` runs, {begin, end, value, set}, sorted by slot, which
// this set has not taken yet; `pair_shift_main` applies them to the set's instance table before
// the frame's copies (a slot the frame copies then takes its record whole, so a run over it does no
// harm), one workgroup a run, its lanes striding over the run's slots. Nothing reads the old number.
[shader("compute")]
[numthreads(64, 1, 1)]
void pair_shift_main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
  const uint index = folded_group(group);
  if (index >= g_params.count) return;
  uint4* runs = (uint4*)g_params.list;
  const uint4 run = runs[index];
  InstanceDesc* instances = (InstanceDesc*)g_params.instances;
  for (uint slot = run.x + thread.x; slot < run.y; slot += k_pair_expand_workgroup) {
    instances[slot].first_pair = run.w != 0 ? run.z : instances[slot].first_pair + run.z;
  }
}

// And the pairs of the slots numbered again that the frame did not copy, after the copies: runs
// {begin, end, prefix, 0} with `prefix` the runs' slots before this one, one workgroup a slot,
// which finds its run by a binary search over the prefixes (a few thousand runs: twelve steps).
[shader("compute")]
[numthreads(64, 1, 1)]
void pair_expand_runs_main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID) {
  const uint index = folded_group(group);
  if (index >= g_params.slots) return;
  uint4* runs = (uint4*)g_params.list;
  uint lo = 0;
  uint hi = g_params.count - 1;
  while (lo < hi) {
    const uint mid = (lo + hi + 1) / 2;
    if (runs[mid].z <= index) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  const uint4 run = runs[lo];
  expand_instance(run.x + (index - run.z), thread.x);
}
