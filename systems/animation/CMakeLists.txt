# animation capability (ADR-0027). TODO(animation): one line saying what it is.
#
# OPTIONAL creates ENGINE_WITH_ANIMATION (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — the scheduler entry, tunables,
# schema types — from being dropped by the linker because nothing references them.

# The capability's own component and event types (ADR-0007). They live here, not in the shared
# schemas/ directory, so that adding them touches no file this capability does not own;
# CAPABILITY ties them to the same switch, so they leave the build with it.
engine_schema_library(NAME animation_schemas SCHEMAS schemas/animation.schema CAPABILITY animation)

engine_module(NAME animation LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math time log jobs ids json schema tunables geometry anim assets sim ecs animation_schemas
  SOURCES src/animation.cpp)

engine_module_tests(NAME animation
  SOURCES tests/animation_tests.cpp tests/size_table.cpp)

# TODO(animation): benchmark the hot path, not the skeleton (plan 11 §11.8).
engine_module_bench(NAME animation
  SOURCES bench/animation_bench.cpp)
