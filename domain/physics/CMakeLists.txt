# physics: the engine's rigid- and soft-body world, with Jolt 5.6 behind it (plan 05 §5.11,
# ADR-0026). An optional capability (ADR-0027): ENGINE_WITH_PHYSICS=OFF, or the minimal preset,
# leaves the module, its tests, its bench, and the Jolt download out of the configuration.
engine_capability_enabled(physics _engine_physics)
if(NOT _engine_physics)
  return()
endif()

include(EnginePhysics)
engine_fetch_jolt()

engine_module(NAME physics LAYER domain OPTIONAL CAPABILITY PHYSICS
  DEPS base containers math hash time jobs log
  EXTERNAL_DEPS Jolt
  SOURCES src/world.cpp src/shapes.cpp src/soft_body.cpp src/character.cpp src/job_adapter.cpp
          src/backend.cpp)

# Jolt's headers are third party, so the engine's warning policy applies to our code only.
# Nothing outside src/ includes them: the public headers speak core/math and core/containers.
target_include_directories(engine_physics SYSTEM PRIVATE
  $<TARGET_PROPERTY:Jolt,INTERFACE_INCLUDE_DIRECTORIES>)

engine_module_tests(NAME physics
  SOURCES tests/rigid_body_tests.cpp tests/query_tests.cpp tests/soft_body_tests.cpp
          tests/soft_body_parallel_tests.cpp tests/deformable_tests.cpp
          tests/determinism_tests.cpp tests/character_tests.cpp tests/size_table.cpp
  DEPS jobs)

# Step cost for falling boxes at 1/4/8 workers and for soft cubes across worker, iteration and
# cage-size sweeps (docs/subsystems/physics.md, "Performance notes"), plus the E19 lattice-cage
# experiment: a press-hold-release fixture and a sustained-load one that print their own
# JSON lines so the run can be repeated on another machine
# (docs/experiments/e19-lattice-cage.md).
engine_module_bench(NAME physics
  SOURCES bench/physics_bench.cpp bench/e19_bench.cpp
  DEPS jobs)
