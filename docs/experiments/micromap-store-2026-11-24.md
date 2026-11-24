# The opacity micromaps stored by part (R208, 2026-11-24)

Roadmap R208; the rules are on [renderer](../subsystems/renderer.md#the-bake-is-read-back). This page holds the figures the store's design and its floor rest on, and what they do not cover.

## What was asked, and what changed the question

R207 stored the whole scene's bake under one key, and its review (M3) asked for one file a scene digest, the old one replaced by a new bake, the dead stores' temporary files swept, and a floor under which a scene bakes rather than reads ("a tiny scene read in 55 ms against 13 ms baked"). Batch 73's merge then loaded the forest staging from six other cameras, and those loads wrote **a second whole file** beside the first (373 MB): R207's key named the masked clusters the load held, and in a streamed world what a load holds follows where it starts. One file a scene, replaced by each new bake, would rebake the whole scene at every new starting place. So the store keeps **parts** (a mesh's masked clusters, keyed by what the bake reads of them) in **packs** (what one load baked), and a load bakes only the parts no pack holds.

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

## The tree's own scenes (msvc-release, 1280×720, `--raster rt --shadows rt`, two frames)

The machine was not quiet: the CPU at 100 % from other agents' builds, 6.6 GB of memory and 26.8 GB of commit free, other rows' runs queued on the card's lock (each run here held it). The stage's times are an upper bound; the counts are the result.

**leaf-cards** (four oaks of one masked mesh: one part, 48,986 masked triangles):

| Load | Stage ms | Resident bytes | Stored |
|---|---|---|---|
| Bake, no store (`renderer.rt.micromap_store_min` past the scene), the binary's first | 241.5 | 331,388 | — |
| Bake, no store, again | 50.9 | 331,388 | — |
| Bake and store into an empty root | 68.2 | 323,196 | 317,496 bytes |
| Read | 7.7 | 323,196 | — |
| Read, again | 7.7 | 323,196 | — |

The read is a seventh of the bake. The resident micromap built from the store is 8,192 bytes smaller than the bake's own: entries shared by their bytes, where the bake shares by material and UV words.

**streamed-forest** (three thousand trees over two kilometres, streamed tile by tile): the load holds **1,822 masked parts, of 772 distinct keys** — its far-forest tiles are meshes of their own, and many of them are the same trees' cards with the same UVs and materials, so they key alike wherever they stand.

| Load | Parts read / baked | Stage ms | Pack bytes | Resident bytes |
|---|---|---|---|---|
| Bake, no store (as before R207: no key) | 0 / 1,822 | 198.0 | — | **92,870,128** |
| First store, the bake's own entries written (start A, the camera path) | 0 / 772 | 522.0 | 52,509,464 | 5,771,632 |
| A second start reading it (the fast flight's first point) | 1,822 / 0 | 94.0 | read 52,509,464 | 5,771,632 |
| Store into an empty root, entries compacted by their bytes | 0 / 772 | 403.0 | **5,325,008** | 5,771,632 |
| Read | 1,822 / 0 | **26.1** | read 5,325,008 | 5,771,632 |

Colour byte-equal (the PNG's bytes) between the no-store bake, the store and the read, here and on leaf-cards. **This scene's held parts do not depend on where it starts** (its tiles' meshes are the prefix's), so it shows a second start reading what the first stored, not a second start baking only what is new; that is the tree's case above, and the staging is the scene where it matters (below).

**Sharing by bytes is the larger win, on disk and resident.** The first store wrote the bake's own entries: 52.5 MB for a micromap of 5.8 MB. The bake shares an entry only between triangles of one material *index* and the same UV words, and here the tiles' copies of a tree's cards are other material indices with the same contents, so the bake of all 1,822 parts held 92.9 MB resident where the same triangles' states, shared by their bytes, are 5.8 MB — sixteen times less, the picture the same bytes. So a pack is written with its entries shared by their bytes (the assembly's own rule, run over the bake's parts before the write): 52.5 MB became 5.3 MB, and the read went from 94 ms to 26. A load with no derived-data root still bakes as before and holds the bake's own entries (row R212).

## A scene over its budget (the read's review, M1)

The budget is held to the assembled micromap, so a scene over it at the top cap drops a level and tries again. Its first load bakes and stores at each cap it tries (the top cap's pack is written before the assembly finds it over), and **every later load reads both caps' packs**: the top cap's whole, to assemble and find it over, then the lowered cap's, baking nothing (`alpha mask: a micromap over its budget is read back at the lowered cap, baking nothing`: on the quad, `packs_read` 2, `baked` 0, the lowered cap reported, the picture byte-equal to the first load's and to an unstored load at the same budget). As written first, the top cap's sections stayed in memory through the second round, so such a load's peak was both rounds' packs plus the assembly; they are now freed before the next cap is tried (a pack is one round's bake, and the cap is in every part's key, so nothing at a lower cap names them), and the peak is the larger round. Reading the top cap's states at all is the cost left: deciding the cap from the packs' headers, which could carry what the part assembles to, is row R211. The staging was over its 1 GiB budget at the top cap with the bake's own entries (cap 5); held to the entries shared by their bytes it may not be (the coordinator's flight says which cap, beside the resident bytes).

## The floor

R207's review saw a tiny scene read in 55 ms against 13 ms baked and asked for a size under which a scene bakes rather than reads. R208's read was timed beside its bake on the two sizes the tree has, `msvc-release`, the same busy machine as above:

| Scene | Masked triangles | Read ms | Bake ms |
|---|---|---|---|
| The alpha tests' quad (`alpha mask: a second load reads ...`, its two cases) | 4 | 2.9, 3.0 | 17.7, 17.2 (the load that bakes and stores) |
| leaf-cards | 48,986 | 7.7, 7.7, 7.3 | 50.9, 50.1 (a bake that stores nothing) |

The read is a sixth of the bake at four triangles and a seventh at 49 thousand: its fixed cost (the key pass's one dispatch, the folder's listing and each pack's header, the restore's uploads) is below the bake's (three passes and their uploads) at any size. So `renderer.rt.micromap_store_min` is **0**: every scene with a derived-data root stores and reads. R207's 55 ms was not reproduced and is not explained here; its read held the stored file's per-cluster table to the scene's word for word through a 4 MiB buffer, which R208's does not do (the table is the scene's own layout, never read). The switch stays for a device or a disk where a read costs more.
## What the staging should show

Not flown here: the store's key moved, and a row that moves a key does not load the staging with its tip. At the merge, the coordinator's flight: two warm loads of the staging from one start into an empty micromap folder (the second reads every part: `load.micromaps` `baked` 0), then a load from a second start (another camera path's first point; its `baked` is the parts the first start did not hold, its `read` the rest), then the first start again (`baked` 0). Beside it, `stored_bytes` against R207's 373 MB and `resident_bytes` against R207's 386,665,228 (naming the `level_cap` each ran at: R207's was 5, and R208's may be 6), and the stage against R207's 509 ms read.
