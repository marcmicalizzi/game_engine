# water_state capability (ADR-0027): the shallow-water equations as a finite-volume step on the CPU,
# the reference every later form of the engine's simulated water is held to (roadmap W184).
#
# OPTIONAL creates ENGINE_WITH_WATER_STATE (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — the scheduler entry, tunables,
# schema types — from being dropped by the linker because nothing references them.

engine_module(NAME water_state LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math jobs
  SOURCES src/water_state.cpp)

engine_module_tests(NAME water_state
  SOURCES tests/water_state_tests.cpp tests/size_table.cpp)

# One step of a square grid at three sizes, in cells a second (plan 11 §11.8).
engine_module_bench(NAME water_state
  SOURCES bench/water_state_bench.cpp)
