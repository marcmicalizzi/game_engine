# authored (domain, capability)

The placement generator `authored`: **a scene's own static instances, a tile at a time** (roadmap
W73, 2026-10-24). A streamed world used to leave a scene file's `instances` in the scene's fixed
prefix ([renderer](renderer.md#instances-that-come-and-go)) — every cluster of every tree a pair
whether it stood beside the camera or a kilometre away — while only a generator's placements came
and went with their tiles under the placements consumer's pair budget ([world](world.md#the-consumers)).
That is what refused the full v8 forest (8,819 region trees and 7,804 authored instances, every one
a file instance) before its first frame: its prefix alone was past the visibility id's 2^24 pairs.
[world](world.md#an-authored-world) named the missing piece in item 5 — *an authored world's
placements authored per tile is a placement generator reading its own set* — and this is it.

## Why this shape

**A generator, not a second consumer.** The placements consumer already streams every entry in the
scene's order, budgets them together and hands the renderer one tail of per-tile blocks; collision
asks the same entries for the inner ring. A scene's instances handed over as one more entry get all
of it, so the only new code is the filing. The reader writes the set
(`engine.scene.AuthoredSet`, a core schema type, so the renderer names no capability) and the
consumer opens the generator on it like any other; with this capability switched off the registry
has no `authored` and the reader keeps the instances in the prefix, exactly as before.

**Filed by the origin's tile, the form cut by the ring's distance.** An instance belongs to the tile
its origin is on (`anchor_tile`: the floor of x and z over the tile edge), however far its crown
reaches. What keeps a large crown right is not where it is filed but what each ring draws: the
reader cuts every form a ring hands out to what the mesh can draw from the nearest a camera can be
to an origin on a tile of that ring — `(radius[r − 1] − √2/2 − 1/4) × tile`, the quarter the camera's way between two ring updates — with every sphere's offset
from the origin counted ([geometry](geometry.md#distance-forms)). A crown that reaches into a nearer
tile is drawn in the form its own origin allows, which already holds every cluster its near side can
select, and no tile needs to know its neighbours'. That is the one rule; widening the rings or
anchoring by bounds would have made a tile's contents depend on its neighbours'.

**Stable ids.** Each instance's id is its place in the scene file's own list plus one
(`scene_gen::Placement::id`, which sat in that struct's padding), and a tile's placements come in
the order of their ids, so a tile's instances, their order and their names are a function of the
file and the tile — never of which tiles came in first, and the same on every reload
(`authored_tests.cpp`).

## Owned data

The opened set: the instances (`scene::AuthoredInstance`, 64 bytes, pinned in `tests/size_table.cpp`),
the form sets (per ring, the entry's resident mesh it draws), and the filing — every instance index
sorted by (tile, id), the tiles that hold one, and where each tile's run starts.

## Public API

- `authored::k_generator` — `"authored"`, the registered name.
- `authored::anchor_tile(origin, tile_size, out)` — the tile an origin is filed under.
- The generator through the registry (`scene_gen::GeneratorRegistry::find_placement`): `open` reads
  the set and refuses a tile edge that is not the world's, a form set with no ring, an instance
  naming a form set the set has not, and an origin no i32 tile reaches; `tile` answers the tile's
  instances in its ring's form (a ring past the form set's last draws the last; `k_no_ring` draws the
  innermost), with `kind` the ring; `occupies` says whether a tile holds any; `representation` is the
  ring up to the longest form set, so a move between two rings past it builds nothing again;
  `expand` is the whole set in its innermost form and `meshes` is empty, because the set names the
  entry's resident meshes, which only the scene reader holds.

## Invariants

- A tile's placements are a function of the set and the tile alone: the ground, the seed and the
  earlier entries are not read (the set stands where the file stood it).
- Every instance is on exactly one tile; a tile's are in id order.
- Positions are the file's `WorldPos` in f64, unchanged.

## Testing

`tests/authored_tests.cpp`: the filing (negative tiles by floor, a far tile in f64, id order over the
list's order), the form per ring and past the last, the representation, a reload in reversed order
giving the same tiles and ids, and the four refusals. The renderer's streamed-forest test holds the
whole chain — reader, forms, consumer, tail — against a whole read ([renderer](renderer.md#authored-instances-per-tile)).

## Capability contract (ADR-0027)

| Registration point | This capability | Status |
|---|---|---|
| Capabilities it requires | none | stands alone |
| Component and event types | none; its parameters are the core type `engine.scene.AuthoredSet` | the reader writes them without naming this module |
| Tick scheduler entry | none | no tick |
| Render-graph passes | none | its placements are instances like any other |
| Content-build derived step | none | |
| Scene generator | `authored`, a placement generator (`src/authored.cpp`) | above |
| Protocol methods | none | |
| Tunables | none | the reader's rules are settings of the host ([renderer](renderer.md#authored-instances-per-tile)) |
| LOD policy | the distance forms the reader cuts per ring | [geometry](geometry.md#distance-forms) |
| Determinism | `authored::k_determinism` = `derived` | a tile is a function of the set and the tile |
| Zero cost when unused | nothing runs unless a streamed world holds file instances | plan 11 §11.10 |
| Tests and size table | `tests/authored_tests.cpp`, `tests/size_table.cpp` (`AuthoredInstance`, 64 bytes) | written |
| Bench | none | a tile is a binary search and a run |
| Removal proof | `ENGINE_WITH_AUTHORED`, off in the minimal build | the reader keeps the instances in the prefix |

**Removing it.** `-DENGINE_WITH_AUTHORED=OFF` (or the minimal build) drops the module and its tests;
a streamed world's file instances then stay in the scene's prefix, which is the behaviour before
2026-10-24, and a scene whose prefix is past the pair cap is refused with the accounting's sentence.
