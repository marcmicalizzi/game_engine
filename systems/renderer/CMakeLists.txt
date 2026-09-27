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
#
# `schema` and `schemas` are the scene format's (schemas/scene.schema): a scene file, a camera
# path and a landmark overlay are read through the generated types, and a flythrough's records
# are written as them, so `engine-cli schema.describe` explains every one of them.
#
# `scene_gen` is the scene-generator registry (ADR-0046; docs/subsystems/scene_gen.md), and the
# renderer names no generator. A scene's terrain is drawn by the ground provider it names — the
# waves are the renderer's own, registered by `src/terrain.cpp`, and the terrain capability's dunes
# register themselves from their own module — and a scene's placement entries (its `ruins`, its
# `placements`) are expanded by the placement generators they name, the ruins capability's among
# them. A host links the capabilities its configuration has; without one the renderer builds, draws
# everything else as it always did, and the scene reader refuses a scene naming it with the
# registry's sentence, which the minimal presets prove. ADR-0037's conditional link to `ruins`, and
# the second one to `terrain` it grew, ended here.
set(_renderer_deps base containers hash math time jobs log platform io image geometry anim assets
                   gfx json texture tunables content_build schema schemas scene_gen)
engine_module(NAME renderer LAYER systems
  DEPS ${_renderer_deps}
  SOURCES src/settings.cpp src/view_set.cpp src/scene.cpp src/gpu_scene.cpp src/lighting.cpp
          src/page_source.cpp src/streaming.cpp src/scene_renderer.cpp src/reference.cpp
          src/capture.cpp src/terrain.cpp src/camera_path.cpp src/flythrough.cpp
          src/rt_capacity.cpp src/shadow_cascades.cpp src/tile_layout.cpp src/terrain_time.cpp
          src/terrain_rings.cpp)
target_include_directories(engine_renderer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

set(_renderer_tests tests/renderer_tests.cpp tests/reference_tests.cpp tests/streaming_tests.cpp
                    tests/atlas_lod_tests.cpp tests/vertex_id_tests.cpp
                    tests/shadow_caster_tests.cpp tests/flythrough_tests.cpp
                    tests/visibility_tie_tests.cpp tests/rt_capacity_tests.cpp
                    tests/shadow_map_tests.cpp tests/texture_tests.cpp
                    tests/dynamic_instances_tests.cpp tests/tile_layout_tests.cpp
                    tests/sun_arc_tests.cpp
                    # The reader through the registry with generators of the test's own, in every
                    # configuration: with no capability linked, the minimal one included.
                    tests/scene_gen_scene_tests.cpp)
if(TARGET engine::ruins)
  # A scene with a `ruins` entry read over a terrain: kits, meshes, instances and determinism; and,
  # where the terrain capability is too, the same buildings over the dunes a year apart.
  list(APPEND _renderer_tests tests/ruins_scene_tests.cpp)
endif()
if(TARGET engine::terrain)
  # A scene whose terrain names the dune generator: fields, hash, sampler against mesh, the ground,
  # and the time-lapse's cadence and blend; and on a GPU, the moving terrain's pool pass.
  list(APPEND _renderer_tests tests/terrain_generator_tests.cpp tests/terrain_motion_tests.cpp
       tests/terrain_clock_tests.cpp tests/terrain_crest_tests.cpp)
endif()
engine_module_tests(NAME renderer SOURCES ${_renderer_tests})
# The generators reach the scene reader through the registry, so the tests that read a scene naming
# one link its capability as a host does: its registrar is what makes the name one the reader knows.
foreach(_gen ruins terrain)
  if(TARGET engine_renderer_tests AND TARGET engine::${_gen})
    target_link_libraries(engine_renderer_tests PRIVATE engine::${_gen})
  endif()
endforeach()
# ENGINE_SOURCE_DIR is where the shadow caster measurement looks for the Khronos samples
# tools/fetch-samples.ps1 fetches into content/samples (through tests/support/test_paths.h). That
# case is skipped by default, and says so for a sample that is not there.
target_compile_definitions(engine_renderer_tests PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
