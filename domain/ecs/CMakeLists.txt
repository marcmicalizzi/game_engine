# ecs: flecs as the runtime entity store (docs/plan/03-data-model.md §3.4), plus the tick
# pipeline of docs/plan/05-simulation.md §5.2, the job-system adapter, and the log sink. The
# library is exposed directly, not wrapped; see docs/subsystems/ecs.md for why.
#
# OPTIONAL (ADR-0027): ENGINE_WITH_ECS=OFF, and the minimal build, take the module, its tests,
# its bench, and the flecs fetch with it. The dependency pin lives in cmake/EngineEcs.cmake.
engine_module(NAME ecs LAYER domain
  OPTIONAL CAPABILITY ecs
  DEPS base containers jobs log platform time
  EXTERNAL_DEPS flecs::flecs_static
  SOURCES src/sim_world.cpp src/os_api.cpp)

# engine_module() has already recorded the capability as disabled and created nothing; the rest
# of this file configures targets, so it stops here rather than guarding each one.
if(NOT TARGET engine_ecs)
  return()
endif()

# flecs' headers are third-party: SYSTEM so their warnings never reach our -Werror, and PUBLIC
# because <flecs.h> is part of this module's public headers — that is the point of the module.
target_include_directories(engine_ecs SYSTEM PUBLIC
  $<TARGET_PROPERTY:flecs::flecs_static,INTERFACE_INCLUDE_DIRECTORIES>)

engine_module_tests(NAME ecs
  SOURCES tests/ecs_tests.cpp tests/size_table.cpp)

# The E6 half that measures flecs: 10^5 entities with relationships and a hierarchy, four
# systems a tick, worker scaling, query latency, and churn (docs/experiments/e6-ecs-store.md).
engine_module_bench(NAME ecs
  SOURCES bench/ecs_bench.cpp
  DEPS ids)
# The bench reports the process working set, which is psapi's on Windows and /proc/self/statm's
# on Linux.
if(WIN32 AND TARGET engine_ecs_bench)
  target_link_libraries(engine_ecs_bench PRIVATE psapi)
endif()
