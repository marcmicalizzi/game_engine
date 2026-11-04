# Juvenile regeneration specimens

The optional `regeneration` block selects established seedlings and saplings in the existing
flora loader. The seven species recipes hold seed 21 at 0.8 m reference height. Additional
seed-42 recipes show maple seedling groups, suppressed spruce, broken/recovering pine and
browsed/recovering hazel. `obstacle` uses seed 1 and a local sphere above a 40 cm oak's root.
The obstacle is a constraint, not rendered geometry.

[Specimen scene](../../test-scenes/regeneration/scene.json),
[model contract](../../../docs/subsystems/flora.md#regeneration-and-damaged-young-growth),
[research and numerical choices](../../../docs/experiments/flora-regeneration.md).

These are static authored forms. Heights are reference shoot lengths, not ages or measured
site productivity. Blades retain their species dimensions; group radius bounds plant centres,
not the crown. Existing recipes without the optional block keep their accepted geometry.
