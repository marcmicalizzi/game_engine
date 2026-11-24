# The opacity micromaps stored by part (R208, 2026-11-24)

Roadmap R208; the rules are on [renderer](../subsystems/renderer.md#the-bake-is-read-back). This page holds the figures the store's floor and its design rest on, and what they do not cover.

## What was asked, and what changed the question

R207 stored the whole scene's bake under one key, and its review (M3) asked for one file a scene digest, the old one replaced by a new bake, the dead stores' temporary files swept, and a floor under which a scene bakes rather than reads. Batch 73's merge then loaded the forest staging from six other cameras, and those loads wrote **a second whole file** beside the first (373 MB): R207's key named the masked clusters the load held, and in a streamed world what a load holds follows where it starts. One file a scene would rebake the whole scene at every new starting place. So the store keeps **parts** (a mesh's masked clusters, keyed by what the bake reads of them) in **packs** (what one load baked), and a load bakes only the parts no pack holds.

## The tree's own cases (msvc-debug, RTX 5090, the alpha tests' fixtures)

Each a count, the time printed beside it (a debug build's times are not the cost; they show the shape):

| Case | Parts | Read | Baked | Packs read | Stored | Stage ms |
|---|---|---|---|---|---|---|
| A first load into an empty root (the masked quad, the shadow rays) | 1 | 0 | 1 | 0 | 2,184 bytes | 251 (the bake pipeline's first compile in it) |
| The next load of it | 1 | 1 | 0 | 1 | none | 4.3 |
| A first load (the ray path's quad) | 1 | 0 | 1 | 0 | 2,184 bytes | 27.6 |
| The next load of it | 1 | 1 | 0 | 1 | none | 5.3 |
| A second start: the first's mesh and one more | 2 | 1 | 1 | 1 | one pack | |
| A third start holding both | 2 | 2 | 0 | 2 | none | 4.5 |

Colour and ids byte-equal between every read and its bake, and between the second start and a load of the same two meshes with no store.
