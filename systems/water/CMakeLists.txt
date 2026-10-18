# water capability (ADR-0027; docs/subsystems/water.md; roadmap S63): a scene's water bodies
# (`engine.scene.WaterBody`) as surfaces — a mesh per body with the depth under every vertex,
# through the placement generator "water" the renderer's scene reader expands the block by — and
# the depth at a point a walker wades by.
#
# OPTIONAL creates ENGINE_WITH_WATER (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module and its tests from the build, and the rest of the tree
# still builds and passes — the renderer then refuses a scene with a `water` block with a sentence.
# WHOLE_ARCHIVE keeps the registrar (src/scene_generator.cpp), which nothing references, from being
# dropped by the linker.
#
# `scene_gen` for the registry and the ground; `schemas` for the block's type, which is the scene
# format's; `geometry` for the derived-data root the meshes are written under; `io` to write them.
# No renderer: the water material and the resolve's branch are the renderer's and gfx's.
engine_module(NAME water LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash json schema schemas io log geometry scene_gen
  SOURCES src/water.cpp src/scene_generator.cpp)

# water_tests.cpp: the shapes, the mesh and its depths over a synthetic ground, `depth_at`, the
# GLB, the generator through the registry, and the renderer's material packing; no device.
# water_scene_tests.cpp: the renderer's reader over a scene with a `water` block (meshes, instances,
# materials, the collision mark), and on a GPU the oasis pool's captures with and without it.
engine_module_tests(NAME water
  SOURCES tests/water_tests.cpp tests/water_scene_tests.cpp tests/size_table.cpp
  DEPS renderer gfx)
if(TARGET engine_water_tests)
  target_compile_definitions(engine_water_tests PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
# The oasis pool stands in the dunes under the earth's sky with scattered rocks at its shore, which
# are three capabilities': linked as a host links them (their registrars name the generators), and
# the cases over that scene compiled only where all three are.
if(TARGET engine_water_tests AND TARGET engine::terrain AND TARGET engine::sky
   AND TARGET engine::scatter)
  target_link_libraries(engine_water_tests PRIVATE engine::terrain engine::sky engine::scatter)
  target_compile_definitions(engine_water_tests PRIVATE ENGINE_WATER_TESTS_DUNES=1)
endif()
