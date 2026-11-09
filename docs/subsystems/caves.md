# caves (domain, capability)

**Purpose.** A cave passage from a hole in the ground in and down, and the speleothems that hang and stand in it (roadmap G134; the owner's request of 2026-11-06, the proof of concept's engine-needs page §6). A recipe (`engine.caves.Passage`, a scene's `<name>.cave.json`) and a seed in, one mesh out: the passage's shell — a closed surface wound to face inward, open only at the **collar**, the ring that meets the terrain's hole — and its stalactites, soda straws, stalagmites and columns. The hole itself is the terrain's (`holes`, W87, [terrain](terrain.md#holes)); the passage is what lies under it. What it does not do: breakdown, flowstone, draperies, halls and mazes, dissolution as a simulation, and the light inside (the renderer's: [Light](#light)).

**Why this shape.** A passage is content, not a system: it does not change in a tick, so it is a mesh generator like the rocks (`domain/rocks`, "The build") rather than a scheduler entry, and everything downstream — the cluster DAG as its LOD, pages, the derived-data cache, the ray tracing chain, traced shadows inside, `scene_collision`'s proxy cut from the DAG — is what every mesh already has. Nothing here is new to the renderer. The passage is **one mesh in the entrance's frame** (the origin the hole's centre at the ground), so a scene places it with one instance at the hole. The alternatives were a closed tube (the walker could not enter, and the terrain's opening would show a lid) and a signed-distance volume meshed by marching cubes (a voxel budget the triangle budget cannot pay at a passage's length, and a mesh that is not the same bytes on every toolchain without care the sweep does not need).

**Grounded.** [The grounding page](../experiments/caves.md) has the sources and the ranges: phreatic tubes twice as wide as high, vadose canyons three times as tall as wide, keyholes a tube over a narrow canyon (Ford and Williams 2007; Palmer 1991); the stalactite's radius as its length to the 3/4 (Short, Baygents and Goldstein 2005's ideal shape, far field); a soda straw's bore the drop's, 5 mm; a stalagmite about Curl's minimum diameter of 3 cm.

## The shell

- **The centreline**: the entrance, a vertical shaft of `shaft_m`, then a Catmull-Rom spline through the recipe's `points` (metres from the entrance), sampled every `ring_spacing_m` by arc length.
- **A ring a sample**, `ring_vertices` round, in the ring's frame: its tangent, and the profile's up — world up off the tangent, leaning to the passage's first heading where the tangent is near vertical, so the frame turns through the bend out of the shaft and never twists. The shaft's rings are level circles of the hole's radius; the passage's first two metres blend from that circle into the first segment's profile, vertex for vertex.
- **Profiles** (`profile_point`): a phreatic tube is an ellipse `width × width / 2`; a vadose canyon a superellipse of exponent 4, `width × 3 width`; a keyhole the tube over a canyon 0.35 of its width and 1.2 tube heights deep. `height_m` overrides the ratio within the grounding's range.
- **Bounded roughness**: a smooth integer-hash value noise over (ring, vertex), at most `roughness` (4 %) of the distance from the axis, faded in over the first metre so the collar is exact.
- **Wound to face inward**: every triangle's front is the side a camera in the passage sees. The cluster build keeps **normal cones on** (AGENTS.md, "Normal cones"): from inside, the cone test drops only walls facing away, as it does on any mesh; from outside, looking down through the hole, the far wall faces the camera and is drawn. The far end is a rounded cap. The only open edge is the collar ring, and it is **locked** in the DAG (`AttributeSource::locked`), so every cut keeps the ring that meets the hole.
- **The collar** lies on the hole's circle at the ground: radius `entrance_radius_m` to a millimetre, at y 0, or at `collar_heights_m` for a hole in sloping ground — the recipe's ring is **resampled by angle** (sample i at 2 pi i / count from +x towards +z, linear between the two either side, found by cross products rather than libm's `atan2` so the bytes stay the same on every toolchain), so a ring of 8 to 256 samples is accepted and each rim vertex takes the height at its own place whatever the passage's heading (G134, 2026-11-08: it was laid on the shaft's own ring, whose vertex 0 lies on the heading's side, and a cave not heading +z had its rim rotated, up to 1.1 m off the ground) — faded out down the shaft over twice the largest of them (2 m at least), so every vertex still falls as the shaft goes down and the wall never folds; a shaft shorter than that fade ends it abruptly. The terrain's opening is the cells wholly inside the hole ([terrain](terrain.md#holes)), so the ground's rim overhangs the collar by up to a cell's diagonal and no gap is left between the two.

## Speleothems

Drip points over the ceiling (the ring vertices whose profile points up more than 60 degrees), `drip_per_m2` per square metre, the count per ring an integer hash of the seed. Each drip is, by its own hash: a **soda straw** (`straw_share`: a tube of `straw_radius_m`, 2.5 mm, up to `straw_max_m`), or a **stalactite** of the ideal profile — `stalactite_radius`: the straw's radius at the tip growing as `(along / length)^(3/4)` to `slenderness × length` at the ceiling — with, seven times in ten, a **stalagmite** below on the floor under the drip (the lower half's vertex nearest it across, so over a keyhole's shoulder it stands on the shoulder): a column of `stalagmite_radius_m` (3–6 cm) with a rounded top, Dreybrodt's equilibrium shape. Where the two would come within `column_gap_m` they are drawn to meet halfway: a **column**. Each body is a closed surface of revolution, `sides` round, wound outward and sunk 8 cm into the rock and the floor so no seam shows. **The triangle budget** (`triangle_budget`, 20,000) is per passage: a drip that would pass it is counted (`drips_over_budget`) and placed nowhere. The test passage — a 6 m hole, a 4 m shaft and 40 m down with a bend, 64 m along its centreline — is 106 rings of 24 and 5,064 shell triangles, and its 81 drips 55 stalactites (12 of them columns), 26 straws and 30 stalagmites in 8,576 triangles, within the default budget; its DAG is 322 clusters.

## Light

Interior light is the renderer's. Today's resolve lights every surface with the unshadowed sky hemisphere as its ambient, so the walls of a passage tens of metres under the ground are lit as if open to the sky: **too bright**. Traced shadows hold the sun and a point light correctly inside (the shell is closed), and R69's irradiance cache (being built beside this) is what darkens the interior; under its `sky` mode the passage reads as it should, under its harmonics it does not yet. A test scene with a point light inside is the reference ([Testing](#testing)).

## Owned data

The recipe's type (`schemas/caves.schema`) and the solve. Nothing else: the mesh, once built, is a `.clusters` container like any other.

## Invariants

- The shell is two-manifold with exactly one open edge loop, the collar; the speleothems are closed; welded by exact position, no degenerate triangle.
- The shell's signed volume about the entrance is negative (wound inward).
- The collar is on the hole's circle within a millimetre, at the given heights.
- The profiles' proportions are the grounding's; a recipe outside a range is refused with its field.
- The same recipe is the same bytes on every toolchain: products, sums, square roots (correctly rounded), a circle from Taylor sums rather than libm's trigonometry, and an integer hash.

## Public API

`domain/caves/caves.h`: `read_passage`, `passage_in_range`, `solve_passage`, `passage_source_hash`, `build_passage_container`, `profile_point`, `segment_height`, `stalactite_radius`; the mesh generator "cave" registered for `<name>.cave.json`.

## Depends on

`assets` (the mesh, the generator registry), `geometry` (the cluster build), `materials` and `texture` (a pack's `rock/limestone` set, sampled in metres round and along the passage).

## LOD policy and determinism

LOD is the cluster DAG, as any mesh: a passage seen from its mouth draws its far reaches coarse. Determinism stance "derived": content, never read back into a tick.

## Testing

`domain/caves/tests/caves_tests.cpp`: the test passage (a 6 m hole, a 4 m shaft, 40 m down with a bend, tube, keyhole and canyon) is two-manifold with the collar its one open loop and wound inward (its signed volume); the collar on the rim within a millimetre; the collar on a sloping ground laid by the schema's angle convention for two headings (one off +z by 67 degrees) and two sample counts, each vertex within a millimetre of the ground; a ray straight down from the centreline at mid-passage meets the floor, as the walker's ground query does on the collision proxy; the speleothems exist and keep their budget; the profiles' proportions and the 3/4 law; the same bytes twice and another seed another passage; the refusals; and the container built with its DAG.

## Not yet

The generator reading the ground itself for the collar's heights (the recipe carries them, and a recipe fitted to a coarser ground than the hole cuts leaves a gap: engine-needs item 18, content's and this generator's); the bedding's control (a phreatic segment following the scene's formation bed, W85) — the grounding names it and the solver does not yet read the field; the speleothems as their own instances (they are part of the passage's mesh, so they share its LOD and its material); a cave's own material roles beyond `rock/limestone`; flowstone, draperies, breakdown.

## Hosts

engine-view and engine-host link the capability when it is built (their generator lists, beside the rocks), so a scene's `.cave.json` mesh loads in either.

## Capability contract (ADR-0027)

- **Switch**: `ENGINE_WITH_CAVES` (ON by default); OFF, or `ENGINE_MINIMAL`, removes the module, its schema types and its tests, and a scene's `.cave.json` is refused with the mesh-generator registry's sentence.
- **Registration points**: schema types (`schemas/caves.schema`), derived data (the mesh generator "cave"). No scheduler entry, render pass, protocol method or tunable.
- **Zero cost unused**: a scene that names no `.cave.json` never calls into it.
