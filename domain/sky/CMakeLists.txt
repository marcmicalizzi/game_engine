# sky capability (ADR-0027, ADR-0048; docs/subsystems/sky.md): the model of an Earth sky — the
# ephemeris that places the sun and the moon on a world's calendar at its latitude, Earth's air at a
# turbidity, and a procedural table of stars — registered as the sky provider "earth" in the scene
# generators' registry (scene_gen/sky.h), which is how the renderer reaches it without naming it.
#
# OPTIONAL creates ENGINE_WITH_SKY (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module and its tests, and the rest of the tree still builds and
# passes — the renderer then draws every scene that names no sky exactly as before, and refuses a
# scene that names one with the registry's sentence. WHOLE_ARCHIVE keeps the registrar, which nothing
# references, from being dropped by the linker.
#
# Pure f64 arithmetic on the CPU: no GPU, no ECS. `schemas` for the scene's sky entry
# (`engine.scene.Sky`), `scene_gen` for the registry and the provider's types.
engine_module(NAME sky LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math schema schemas scene_gen
  SOURCES src/ephemeris.cpp src/earth.cpp src/sky_provider.cpp)

engine_module_tests(NAME sky
  SOURCES tests/sky_tests.cpp tests/size_table.cpp)
