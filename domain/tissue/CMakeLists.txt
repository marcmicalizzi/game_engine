# tissue: the engine's tissue definition, v0 — the schema'd definition of a deformable tissue
# body (schemas/tissue.schema), its `.tissue` container and the interchange the authoring side
# writes, and the validators and report the reviews specified (docs/subsystems/tissue.md;
# docs/plan/05-simulation.md §5.14 and §5.16, 07 §7.10 and §7.11; ADR-0026, ADR-0029, ADR-0032).
# No solver and no GPU pass: a definition, checked.
#
# A capability (ADR-0027): ENGINE_WITH_TISSUE=OFF, or the minimal preset, leaves it out, and
# engine-content builds without its tissue subcommands. It links domain/physics for one header,
# the cage-size verdict of ADR-0029 (deformable.h), which is the rule the plan says the content
# validator reads; physics is a capability of its own, so the edge is declared and tissue is off
# wherever physics is.
engine_capability_requires(tissue physics)

engine_module(NAME tissue LAYER domain OPTIONAL
  DEPS base containers math hash io json schema schemas geometry physics
  SOURCES src/sha256.cpp src/tissue_file.cpp src/mesh_query.cpp src/cells.cpp src/energy.cpp
          src/validate.cpp src/expect.cpp src/synthetic.cpp)

engine_module_tests(NAME tissue
  SOURCES tests/sha256_tests.cpp tests/tissue_file_tests.cpp tests/mesh_query_tests.cpp
          tests/cells_tests.cpp tests/energy_tests.cpp tests/validate_tests.cpp
          tests/quadratic_tests.cpp tests/expect_tests.cpp tests/size_table.cpp)
