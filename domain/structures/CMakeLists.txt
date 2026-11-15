# structures capability (ADR-0027; docs/plan/14-structures.md): the building description, the one
# mesher that turns it into cluster geometry by material role, the walk records beside the mesh,
# and the mesh generator `structure` (docs/subsystems/structures.md).
#
# OPTIONAL creates ENGINE_WITH_STRUCTURES (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the mesh generator's registrar from being dropped by the linker because
# nothing references it.
#
# Layer: domain, beside the other mesh generators (flora, rocks, bridges): it writes meshes
# through assets' generator registry and needs no ground, no document and nothing from systems/.

# The capability's own types (ADR-0007): the description, the recipe and the document record
# (`engine.structure.*`). They live here, not in the shared schemas/ directory, so adding them
# touches no file this capability does not own; CAPABILITY ties them to the same switch.
engine_schema_library(NAME structures_schemas SCHEMAS schemas/structures.schema CAPABILITY structures)

engine_module(NAME structures LAYER domain OPTIONAL WHOLE_ARCHIVE GENERATOR mesh:structure
  DEPS base containers math hash json schema log io assets geometry texture materials
       structures_schemas
  SOURCES src/description.cpp src/emit.cpp src/shell.cpp src/interior.cpp src/structures.cpp
          src/container.cpp)

engine_module_tests(NAME structures
  SOURCES tests/structures_tests.cpp tests/size_table.cpp)

# The conformance corpus in content/structures/ (plan 14 §14.1): the test finds it through
# test_paths.h, by this path or in a test bundle.
if(TARGET engine_structures_tests)
  target_compile_definitions(engine_structures_tests
    PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

engine_module_bench(NAME structures
  SOURCES bench/structures_bench.cpp)
