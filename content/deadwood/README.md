# Deadwood recipes

Species-derived remains for [G101](../../docs/subsystems/deadwood.md). Every sample uses a
6 m source tree with a 60,000 triangle source cap and seed 17. The remains' separate caps are
the kind defaults. Version 2 supplies native exposed grain/normal/roughness maps without a pack;
bark and foliage keep their source fallback colours. `surface_resolution: 0` gives a flat debug
comparison. The [surface study](../../docs/experiments/deadwood-surfaces.md) documents the controls.

- `fresh-oak`, `fresh-spruce`: fresh horizontal falls, retaining leaves and upper branches.
- `bare-oak`: leafless fall with partial branch and bark loss.
- `snag`, `snapped`: standing dead crown and fractured trunk.
- `stump`: broken hollow stump.
- `hollow-log`, `collapsed-log`: a hollow bole section and a compressed shell with an open roof.
- `limb`: the first source branch with its descendants, detached and laid down.

Build with `engine-content build content/deadwood/<name>.deadwood.json <local-output>.clusters`.
Cooked meshes and captures belong outside the repository. The
[test scene](../test-scenes/deadwood/README.md) lays out these nine recipes. Recipes already bake
their yaw/pitch/roll; further tilting requires a new cook to fit bottom branches to the new site.
