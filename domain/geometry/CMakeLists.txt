# geometry: the cluster geometry format and its builder (docs/plan/04-renderer.md §4.3,
# ADR-0005). Clusters are the unit of rasterization, streaming, and acceleration structures.
# Also the Loop limit-surface operator and the surface binding a deformable region's render
# surface follows its cage with (docs/plan/05-simulation.md §5.16, 07 §7.11).
engine_module(NAME geometry LAYER domain
  DEPS base containers math hash io
  EXTERNAL_DEPS meshoptimizer
  SOURCES src/cluster.cpp src/cluster_lod.cpp src/cluster_pages.cpp src/cluster_file.cpp
          src/stress_mesh.cpp src/uv_repair.cpp src/limit_surface.cpp src/surface_binding.cpp)
target_include_directories(engine_geometry SYSTEM PUBLIC
  $<TARGET_PROPERTY:meshoptimizer,INTERFACE_INCLUDE_DIRECTORIES>)
# clusterlod.h (the cluster LOD builder) ships in meshoptimizer's demo directory.
target_include_directories(engine_geometry SYSTEM PRIVATE "${meshoptimizer_SOURCE_DIR}/demo")

engine_module_tests(NAME geometry
  SOURCES tests/cluster_tests.cpp tests/cluster_lod_tests.cpp tests/cluster_pages_tests.cpp
          tests/cluster_file_tests.cpp tests/atlas_seam_tests.cpp tests/morph_tests.cpp
          tests/uv_repair_tests.cpp tests/limit_surface_tests.cpp tests/surface_binding_tests.cpp
          tests/vertex_id_tests.cpp tests/size_table.cpp)

# The per-frame half of the limit surface: the operator's matvec at the fixture's size and at the
# cage-size limit, and the displacement transfer over a region's bound vertices.
engine_module_bench(NAME geometry SOURCES bench/geometry_bench.cpp)
