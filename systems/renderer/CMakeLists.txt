# renderer: the engine's renderer (docs/plan/02-architecture.md §2.3 L3, docs/plan/04-renderer.md).
# Not OPTIONAL: like gfx, the renderer is the engine's, not a capability a game adds — a build
# without it could not draw at all, and every consumer (engine-view, engine-host's render.*)
# would have to be switched off with it.
#
# No `window` dependency, on purpose and enforced here: engine-host renders offscreen with no
# display, and `systems/renderer/tests/renderer_tests.cpp` fails to compile if that header ever
# becomes reachable.
engine_module(NAME renderer LAYER systems
  DEPS base containers math time log platform io image geometry assets gfx json
  SOURCES src/settings.cpp src/scene.cpp src/gpu_scene.cpp src/scene_renderer.cpp src/capture.cpp)
target_include_directories(engine_renderer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

engine_module_tests(NAME renderer SOURCES tests/renderer_tests.cpp)
