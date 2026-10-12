# scatter capability (ADR-0027, ADR-0046; docs/subsystems/scatter.md): the scatter generator (plan
# 07 §7.6, roadmap G37), which places instances of a few meshes over the ground deterministically,
# by rule — a tiled, seeded Poisson-disc per kind, a density field, slope, height and region rules,
# exclusions and a budget — as the placement generator "scatter" in the scene-generator registry.
#
# OPTIONAL creates ENGINE_WITH_SCATTER (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its schema library and its tests from the build, and the
# rest of the tree still builds and passes — the renderer then refuses a scene naming "scatter"
# with the registry's sentence. That is the proof that the capability is additive. WHOLE_ARCHIVE
# keeps the registrar (src/scene_generator.cpp), which nothing references, from being dropped.

# The layer's types (ADR-0007): `engine.scatter.Layer` and what it holds, the capability's own so
# the switch takes them out with it.
engine_schema_library(NAME scatter_schemas SCHEMAS schemas/scatter.schema CAPABILITY scatter)

# `scene_gen` for the registry and the ground a placement stands on; `texture` for a `.tex` mask
# (density or exclusion); `io` for the mask, an excluded layer's file and the procedural rock's
# GLB; `jobs` for a whole read's tiles on the performance pool. No renderer, no document: what it
# makes is `scene_gen::Placements`, which the scene reader and the world's placements consumer turn
# into instances.
engine_module(NAME scatter LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash json schema schemas io log jobs time geometry scene_gen texture
       scatter_schemas
  SOURCES src/placer.cpp src/rock.cpp src/scene_generator.cpp)

# scatter_tests.cpp: the placer on the CPU over a synthetic ground (determinism, tiling, spacing,
# rules, density, exclusions, budget, the pinned hash) and through the registry.
# scatter_scene_tests.cpp: a scene naming a layer read by the renderer's reader, and on a GPU the
# placed rocks in the capture's ids channel standing on the drawn ground.
engine_module_tests(NAME scatter
  SOURCES tests/scatter_tests.cpp tests/scatter_scene_tests.cpp tests/size_table.cpp
  DEPS renderer gfx)

# The scattered variant of desert-endless is held field for field to the scene beside it.
if(TARGET engine_scatter_tests)
  target_compile_definitions(engine_scatter_tests PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
