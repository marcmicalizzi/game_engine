# tissue: the engine's tissue definition, v0 — the schema'd definition of a deformable tissue
# body (schemas/tissue.schema), its `.tissue` container and the interchange the authoring side
# writes, the validators and report the reviews specified, the layered volume model's records and
# rows, and the capability gate a file's requirements go through (docs/subsystems/tissue.md;
# docs/plan/05-simulation.md §5.14 and §5.16, 07 §7.10 and §7.11; ADR-0026, ADR-0029, ADR-0032).
# No solver and no GPU pass: a definition, checked.
#
# A capability (ADR-0027): ENGINE_WITH_TISSUE=OFF, or the minimal preset, leaves it out, and
# engine-content builds without its tissue subcommands. It links domain/physics for the cage-size
# verdict of ADR-0029 (deformable.h), which is the rule the plan says the content validator reads,
# and for the soft body a runtime region is built as (soft_body.h, the module's existing wrapping);
# physics is a capability of its own, so the edge is declared and tissue — the cage derivation
# with it — is off wherever physics is.
engine_capability_requires(tissue physics)

engine_module(NAME tissue LAYER domain OPTIONAL
  DEPS base containers math hash io json schema schemas geometry physics time
  SOURCES src/sha256.cpp src/tissue_file.cpp src/capabilities.cpp src/mesh_query.cpp
          src/cells.cpp src/energy.cpp src/validate.cpp src/layered.cpp src/expect.cpp
          src/synthetic.cpp src/layered_synthetic.cpp src/body.cpp src/cage.cpp
          src/cage_rows.cpp src/soft_body.cpp)

engine_module_tests(NAME tissue
  SOURCES tests/sha256_tests.cpp tests/tissue_file_tests.cpp tests/mesh_query_tests.cpp
          tests/cells_tests.cpp tests/energy_tests.cpp tests/validate_tests.cpp
          tests/quadratic_tests.cpp tests/expect_tests.cpp tests/p2_face_tests.cpp
          tests/layered_tests.cpp tests/capabilities_tests.cpp tests/cage_tests.cpp
          tests/soft_body_tests.cpp tests/size_table.cpp)
