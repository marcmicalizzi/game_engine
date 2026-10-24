# Species on shared growth structures

The ten named species use the `tree` block and a shared family solver. Their reference habits and
parameter ranges are described in [flora](../../../docs/subsystems/flora.md#shared-growth-rules-and-named-species-g85)
and grounded in [growth rules](../../../docs/experiments/tree-growth-rules.md). These fixtures use
seed 17 and no baked impostors so captures compare the actual solver geometry. They omit the
budget to use each species' default, listed in flora.md and the
[G86 audit](../../../docs/experiments/tree-profile-audit.md). Evergreen cards represent foliated
shoots or scale fans, not individual needles. These are source budgets, not per-frame
triangle targets. Production deciduous recipes can omit the two zero bake fields to use the
broadleaf chain. Inspect unbaked geometry with `--lod 0.01`: the view's `--finest` switch applies
only to the reference tracer, and a normal simplified mesh cut can lose fine leaves.

Maple's unmerged source retains terminal twig wood and places independent, opposing leaf pairs
around each shoot. Use a single-leaf cutout with the stalk at the image's bottom centre; the full
rectangle is mapped once per leaf. The default 200,000 budget is unchanged (seed 17: 193,114
triangles). Node spacing limits foliage to what the twig can carry without squeezing the images;
small explicit budgets can still select merged sprays. See the
[maple correction](../../../docs/experiments/tree-profile-audit.md#maple-terminal-shoot-correction)
for the geometry/material tradeoff and captures.

`ancient-dwelling.flora.json` is the wide, full village canopy: spreading habit, twelve major limbs
between 22 and 28 m, a 6 m core radius, LAI 5.5 and a 1.5-million-triangle source budget. Its crown
is continuous above the main supports. `ancient-dwelling-tall.flora.json` offers a layered mixed
form with fourteen limbs between 22 and 56 m, a 4.5 m core radius and LAI 4.5. Both leave straight
run length to the habit and mechanical cap, instead of requesting the original anchor A's 20 m
straight run. Both intentionally change the scaffold: existing platforms must be re-anchored
against the structural table. The original game's recipes are not overwritten. These numbers are
authored fantasy-tree choices, not engineering certification for a dwelling; the natural habit
defaults retain their original limb counts and bands.

Build any recipe with `engine-content flora <recipe> <output.clusters>` or use it in a scene mesh
table. Each material remains independently addressable by species: `maple/bark`, `maple/leaf`,
`cedar/bark`, `cedar/needles`, and similarly for the other names. An explicit recipe material still
wins over a pack. Missing textures leave the diagnostic flat colours.
