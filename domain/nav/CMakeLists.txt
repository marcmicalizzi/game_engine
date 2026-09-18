# nav: the engine's navigation mesh, with Recast/Detour 1.6 behind it (plan 05 §5.11, §5.8).
# An optional capability (ADR-0027): ENGINE_WITH_NAV=OFF, or the minimal preset, leaves the
# module, its tests, its bench, and the Recast download out of the configuration.
engine_capability_enabled(nav _engine_nav)
if(NOT _engine_nav)
  return()
endif()

include(EngineNav)
engine_fetch_recast()

engine_module(NAME nav LAYER domain OPTIONAL CAPABILITY NAV
  DEPS base containers math jobs log time
  EXTERNAL_DEPS Recast Detour DetourCrowd
  SOURCES src/tile_build.cpp src/tile_format.cpp src/nav_mesh.cpp src/crowd.cpp
          src/region_graph.cpp src/rebuild_queue.cpp)

# Recast's headers are third party, so the engine's warning policy applies to our code only.
# Nothing outside src/ includes them: the public headers speak core/math and core/containers.
# PRIVATE, not PUBLIC, so linking nav does not put <Recast.h> on anyone else's include path.
target_include_directories(engine_nav SYSTEM PRIVATE
  $<TARGET_PROPERTY:Recast,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:Detour,INTERFACE_INCLUDE_DIRECTORIES>
  $<TARGET_PROPERTY:DetourCrowd,INTERFACE_INCLUDE_DIRECTORIES>)

engine_module_tests(NAME nav
  SOURCES tests/tile_tests.cpp tests/path_tests.cpp tests/region_tests.cpp
          tests/queue_tests.cpp tests/crowd_tests.cpp tests/size_table.cpp
  DEPS jobs)

# E11's first data point: rebuild latency for one tile at three tile sizes, and the rate the
# queue sustains on eight workers (docs/subsystems/nav.md, "Performance notes").
engine_module_bench(NAME nav
  SOURCES bench/nav_bench.cpp
  DEPS jobs)
