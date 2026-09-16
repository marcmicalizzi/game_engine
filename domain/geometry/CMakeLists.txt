# geometry: the cluster geometry format and its builder (docs/plan/04-renderer.md §4.3,
# ADR-0005). Clusters are the unit of rasterization, streaming, and acceleration structures.
engine_module(NAME geometry LAYER domain
  DEPS base containers math
  EXTERNAL_DEPS meshoptimizer
  SOURCES src/cluster.cpp)
target_include_directories(engine_geometry SYSTEM PUBLIC
  $<TARGET_PROPERTY:meshoptimizer,INTERFACE_INCLUDE_DIRECTORIES>)

engine_module_tests(NAME geometry
  SOURCES tests/cluster_tests.cpp tests/size_table.cpp)
