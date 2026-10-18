# volumetrics capability (ADR-0027; docs/subsystems/volumetrics.md): the participating medium's mid
# tier — the dust haze, the crest plumes and the far-field clouds — drawn by one frame pass at the
# renderer's AfterResolve registration point (systems/renderer/frame_pass.h).
#
# OPTIONAL creates ENGINE_WITH_VOLUMETRICS (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its schema types, its shader and its tests from the build,
# and the rest of the tree still builds and passes: the renderer carries a scene's `medium` block
# uninterpreted and engine-view says the build has no medium. That is the proof that the capability
# is additive. WHOLE_ARCHIVE keeps the tunables' static registration objects.

# The capability graph (docs/plan/08-toolchain.md section 8.5). `terrain` is itself an optional
# capability and this one links it (the wind record and the crest lines), so the edge is declared:
# with the terrain switched off, this capability is switched off too. The renderer is the engine's,
# not a capability; the sky capability is not linked (the medium reads the frame's `gfx::SkyParams`,
# which the renderer fills), so neither needs an edge.
engine_capability_requires(volumetrics terrain)

# The scene file's `medium` block (`engine.scene.Medium`), read by this capability alone.
engine_schema_library(NAME volumetrics_schemas SCHEMAS schemas/volumetrics.schema CAPABILITY volumetrics)

# The medium's shader (shaders/medium.slang): it includes domain/gfx's sky.slang and view_ray.slang
# by their paths, as the renderer's encode includes display.slang.
engine_shaders(NAME volumetrics SOURCES shaders/medium.slang)

engine_module(NAME volumetrics LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash log time tunables json schema schemas gfx renderer scene_gen
       terrain volumetrics_schemas
  SOURCES src/volumetrics.cpp)
# A WHOLE_ARCHIVE module is an interface over its `_impl` library, which the shader header is for.
if(TARGET engine_volumetrics_impl AND TARGET engine_shaders_volumetrics)
  target_link_libraries(engine_volumetrics_impl PRIVATE engine_shaders_volumetrics)
endif()

engine_module_tests(NAME volumetrics
  SOURCES tests/volumetrics_tests.cpp tests/size_table.cpp tests/volumetrics_gpu_tests.cpp)
# The GPU cases draw a scene naming a sky where the sky capability is linked, as the renderer's own
# sky tests do: its registrar is what makes "earth" a sky the reader knows.
if(TARGET engine_volumetrics_tests AND TARGET engine::sky)
  target_link_libraries(engine_volumetrics_tests PRIVATE engine::sky)
  target_compile_definitions(engine_volumetrics_tests PRIVATE ENGINE_VOLUMETRICS_TESTS_SKY=1)
endif()
if(TARGET engine_volumetrics_tests)
  target_compile_definitions(engine_volumetrics_tests PRIVATE
                             ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
