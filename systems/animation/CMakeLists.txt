# animation capability (ADR-0027): the system layer that plays clips — a playhead per entity, an
# SoA pose pool the capability owns, and the 3x4 skinning matrices a skinned instance uploads.
#
# OPTIONAL creates ENGINE_WITH_ANIMATION (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — the scheduler entry, tunables,
# schema types — from being dropped by the linker because nothing references them.

# The capability graph (ADR-0027, cmake/EngineModule.cmake). A capability that *ticks* attaches
# through `ecs::register_system` and registers its components with the entity store, so it links
# `domain/ecs` — which is itself an optional capability. Declaring the edge makes
# ENGINE_WITH_ECS=OFF switch this capability off too, with a line saying so, instead of failing
# the configure of a combination that is perfectly legitimate.
engine_capability_requires(animation ecs)

# The capability's own component and event types (ADR-0007). They live here, not in the shared
# schemas/ directory, so that adding them touches no file this capability does not own;
# CAPABILITY ties them to the same switch, so they leave the build with it.
engine_schema_library(NAME animation_schemas SCHEMAS schemas/animation.schema CAPABILITY animation)

engine_module(NAME animation LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math time log jobs ids json schema tunables geometry anim assets sim ecs animation_schemas
  SOURCES src/library.cpp src/pose_pool.cpp src/animation.cpp)

engine_module_tests(NAME animation
  SOURCES tests/animation_tests.cpp tests/lod_tests.cpp tests/size_table.cpp)

# 10^4 instances of a 23-joint skeleton through the real tick, by tier mix (plan 11 §11.8).
engine_module_bench(NAME animation
  SOURCES bench/animation_bench.cpp)
