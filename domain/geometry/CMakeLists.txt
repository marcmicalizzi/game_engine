# geometry: the cluster geometry format and its builder (docs/plan/04-renderer.md §4.3,
# ADR-0005). Clusters are the unit of rasterization, streaming, and acceleration structures.
engine_module(NAME geometry LAYER domain
  DEPS base containers math hash io
  EXTERNAL_DEPS meshoptimizer
  SOURCES src/cluster.cpp src/cluster_lod.cpp src/cluster_pages.cpp src/cluster_file.cpp
          src/stress_mesh.cpp)
target_include_directories(engine_geometry SYSTEM PUBLIC
  $<TARGET_PROPERTY:meshoptimizer,INTERFACE_INCLUDE_DIRECTORIES>)
# clusterlod.h (the cluster LOD builder) ships in meshoptimizer's demo directory.
target_include_directories(engine_geometry SYSTEM PRIVATE "${meshoptimizer_SOURCE_DIR}/demo")

engine_module_tests(NAME geometry
  SOURCES tests/cluster_tests.cpp tests/cluster_lod_tests.cpp tests/cluster_pages_tests.cpp
          tests/cluster_file_tests.cpp tests/atlas_seam_tests.cpp tests/morph_tests.cpp
          tests/size_table.cpp)
