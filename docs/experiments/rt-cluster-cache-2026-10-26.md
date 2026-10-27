# The resident cluster structure cache (R101, 2026-10-26)

The ray tracing chain built a cluster acceleration structure for every visible (instance, cluster)
pair, every frame. The forest proof of concept's expanded forest at 11520×2160 under traced
shadows wanted 222,455 of them a frame against the 1 GiB budget's 166,937, dropped instances on
101 of 101 frames and spent 33.9 ms of its 98.5 ms frame in the chain (`reports/quality-v15/
engine-c1ee971d/HANDOFF.md`, run `expanded-forest-rt-wide-20261026-190424`, on a shared machine:
upper bounds). Those structures are copies: a forest is a few dozen meshes placed tens of
thousands of times. R101 builds a rigid mesh's cluster structures once, the first frame any
instance draws them, into a scene-sized cache every instance and frame references; the mechanism,
the budgets and the rule for a hit are in [renderer](../subsystems/renderer.md#the-resident-cluster-structure-cache).

## What the tests measured

On the rigid procedural grid (`rt_capacity_tests.cpp`, RTX 5090, `msvc-debug`): one mesh of 717
clusters, nine instances; a flight over four distances builds 288 structures into the cache (288
of 1,435 slots), referenced 56 times by the last frame, and **a second flight over the same cuts
builds none**; colour and ids are the per-frame build's bytes on the mesh path with traced shadows
and on the ray path. A cache of 199 slots — the near cut alone occupies 198, the far one 52,
both 214 — evicts 16 slots at each move between them, never overflows, and draws the per-frame
build's bytes. A rigid bar between two skinned ones: the chain builds the two skinned bars' 2
structures from the pool and references the rigid one's 1 cached structure, where the per-frame
chain built 3; the same bytes on the mesh and ray paths.
