# Flora

The optional, deterministic content-time plant solver. Public headers are in `include/domain/flora`,
recipes in `schemas/flora.schema`, implementations in `src`, checks in `tests`.
[Subsystem contract](../../docs/subsystems/flora.md) describes the APIs, budgets, material roles,
motion and collision. [Shrub grounding](../../docs/experiments/shrub-growth-forms.md) records
the five named shrub forms, sources and authored numerical choices.

Define embedded recipe structs before `FloraFile` in the schema. The generated C++ header
follows declaration order; a later definition of an embedded type fails the build.
