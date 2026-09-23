# renderer: the engine's renderer (docs/plan/02-architecture.md §2.3 L3, docs/plan/04-renderer.md).
# Not OPTIONAL: like gfx, the renderer is the engine's, not a capability a game adds — a build
# without it could not draw at all, and every consumer (engine-view, engine-host's render.*)
# would have to be switched off with it.
#
# No `window` dependency, on purpose and enforced here: engine-host renders offscreen with no
# display, and `systems/renderer/tests/renderer_tests.cpp` fails to compile if that header ever
# becomes reachable.
#
# `anim` is here for one type: `anim::JointMatrix`, the 48-byte 3x4 record `deform.slang` reads
# and the frame uploads a span of. It is a domain module and not a capability, so depending on it
# costs nothing and cannot be switched off — and copying the record here instead would be exactly
# the drift `domain/anim` took its own `geometry` dependency to avoid. What the renderer does not
# depend on is `systems/animation`: the capability that *produces* the matrices stays above it,
# and the contract between them is a span and a per-instance offset (docs/subsystems/animation.md).
engine_module(NAME renderer LAYER systems
  DEPS base containers math time jobs log platform io image geometry anim assets gfx json
  SOURCES src/settings.cpp src/view_set.cpp src/scene.cpp src/gpu_scene.cpp src/lighting.cpp
          src/page_source.cpp src/streaming.cpp src/scene_renderer.cpp src/reference.cpp
          src/capture.cpp)
target_include_directories(engine_renderer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

engine_module_tests(NAME renderer SOURCES tests/renderer_tests.cpp tests/reference_tests.cpp
                                          tests/streaming_tests.cpp tests/atlas_lod_tests.cpp
                                          tests/vertex_id_tests.cpp)
