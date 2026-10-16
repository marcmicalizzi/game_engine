# muscle_sim capability (ADR-0027, ADR-0055): the line tier's engine wiring — the scheduler entry
# that ticks every line actuator through `systems/muscle`'s supervisor, the components and the
# `LineActuator` records' materialization, and each tick's path from the posed skeleton
# (docs/subsystems/muscle_sim.md).
#
# OPTIONAL creates ENGINE_WITH_MUSCLE_SIM (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module and its tests from the build, and the rest of the tree
# still builds and passes. That is the proof that the capability is additive. WHOLE_ARCHIVE keeps
# the module's static registration objects from being dropped by the linker.
#
# Why a second capability rather than more of `systems/muscle`: the kernel and the supervisor are
# ECS-free, so their corpus and supervisor tests run in the no-ECS build; everything that needs the
# entity store, the scheduler or a skeleton is here.

# The capability graph (docs/plan/08-toolchain.md section 8.5). `muscle`, `ecs` and `animation`
# are optional capabilities and this one links all three — the kernel, the entity store, and the
# posed skeleton the path is routed through — so each edge is declared: with any of them switched
# off (the no-ECS build switches off `ecs` and `animation` with it), this capability is switched
# off too, with a line on the configure's status output, instead of failing the configure.
engine_capability_requires(muscle_sim muscle)
engine_capability_requires(muscle_sim ecs)
engine_capability_requires(muscle_sim animation)

engine_module(NAME muscle_sim LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math log ids sim ecs schema schemas muscle muscle_schemas animation anim
  SOURCES src/muscle_sim.cpp src/path.cpp)

# The world test materializes `LineActuator` records through the driver and the entity store's
# hook, so it reaches the document model too.
engine_module_tests(NAME muscle_sim
  SOURCES tests/path_tests.cpp tests/muscle_sim_tests.cpp tests/size_table.cpp
  DEPS doc json jobs assets)
