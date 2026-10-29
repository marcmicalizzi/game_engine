# Flora

The optional, deterministic content-time plant solver. Public headers are in `include/domain/flora`,
recipes in `schemas/flora.schema`, implementations in `src`, checks in `tests`.
[Subsystem contract](../../docs/subsystems/flora.md) describes the APIs, budgets, material roles,
motion and collision. [Shrub grounding](../../docs/experiments/shrub-growth-forms.md) records
the five named shrub forms, sources and authored numerical choices.

Define embedded recipe structs before `FloraFile` in the schema. The generated C++ header
follows declaration order; a later definition of an embedded type fails the build.
Optional deterministic content-build solvers. Public headers are under `include/domain/flora`,
source recipes are in `schemas/flora.schema`, implementation is in `src`, and regression tests
are in `tests`. Generated schema code lives only under the build directory.

[Subsystem contract](../../docs/subsystems/flora.md),
[ground-plant grounding](../../docs/experiments/ground-plants.md),
[recipe corpus](../../content/flora/README.md).

`fern.cpp` and `herb.cpp` own the G115 extensions; `ground_plant.h` contains the local prescribed
arch, separate from the shared tree kit. `fern_lod.h` supplies the authored fern hierarchy, keeping
every pinna while simplifying the rachis. The solver runs at cook time and keeps no new
per-frame state. `tools/dev.ps1 test -Filter flora` exercises the morphology and determinism cases.
