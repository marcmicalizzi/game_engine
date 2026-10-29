# Flora

Optional deterministic content-build solvers. Public headers are under `include/domain/flora`,
source recipes are in `schemas/flora.schema`, implementation is in `src`, and regression tests
are in `tests`. Generated schema code lives only under the build directory.

[Subsystem contract](../../docs/subsystems/flora.md),
[ground-plant grounding](../../docs/experiments/ground-plants.md),
[recipe corpus](../../content/flora/README.md).

`fern.cpp` and `herb.cpp` own the G103 extensions; `ground_plant.h` contains the local prescribed
arch, separate from the shared tree kit. `fern_lod.h` supplies the authored fern hierarchy, keeping
every pinna while simplifying the rachis. The solver runs at cook time and keeps no new
per-frame state. `tools/dev.ps1 test -Filter flora` exercises the morphology and determinism cases.
