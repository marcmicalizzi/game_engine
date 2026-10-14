# muscle capability (ADR-0027, ADR-0055): the line tier of the engine's muscles, a Hill-type
# actuator on a routed path integrated at the scheduler's tick (docs/subsystems/muscle.md).
#
# OPTIONAL creates ENGINE_WITH_MUSCLE (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — the schema types — from being
# dropped by the linker because nothing references them.
#
# Stage 1 is a batch kernel over the capability's own store, with no ECS and no scheduler entry,
# so it requires no other capability (no engine_capability_requires).

# The capability's own state, event and work types (ADR-0007). They live here, not in the shared
# schemas/ directory, so that adding them touches no file this capability does not own;
# CAPABILITY ties them to the same switch, so they leave the build with it.
engine_schema_library(NAME muscle_schemas SCHEMAS schemas/muscle.schema CAPABILITY muscle)

engine_module(NAME muscle LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math log ids hash json schema muscle_schemas
  SOURCES src/muscle.cpp src/store.cpp)

engine_module_tests(NAME muscle
  SOURCES tests/law_tests.cpp tests/scheme_tests.cpp tests/schedule_tests.cpp
          tests/lifecycle_tests.cpp tests/corpus_tests.cpp tests/size_table.cpp
  DEPS json schema)

# The law's identity test re-reads tests/model.txt and the kernel's sources from the checkout.
if(TARGET engine_muscle_tests)
  target_compile_definitions(engine_muscle_tests PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()

# The cost of a probe (plan 11 §11.8).
engine_module_bench(NAME muscle
  SOURCES bench/muscle_bench.cpp)
