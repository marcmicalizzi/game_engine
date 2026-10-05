# engine-view: the window on systems/renderer (docs/plan/02-architecture.md §2.2,
# docs/subsystems/renderer.md). The flags, the window and its swapchain, the event loop, the
# capture, and the JSON summary; every pass that draws lives in the renderer module, which
# engine-host is the second consumer of. --frames N --capture out.png is how agents and CI look
# at it.
# `json` builds the one block of the summary that has nulls in it, and `bench` is there for
# foundation/bench/machine_state.h: the summary says what else the machine was doing, in the
# same shape and with the same thresholds the CPU benchmarks use (docs/subsystems/bench.md).
#
# `--animate` needs the animation capability, which needs `domain/ecs`, and both are optional
# (ADR-0027). An app is not a capability and cannot declare `engine_capability_requires`, so it
# does the same thing by hand: link the module when this configuration has it, define
# ENGINE_VIEW_ANIMATION so the glue compiles, and otherwise build an engine-view that refuses
# `--animate` with a sentence. `msvc-minimal` is the proof that the second half works.
#
# `--interactive` is the first owner of a window that turns its events into `input::RawEvent`s
# (window_input.cpp) for something to fly (fly_camera.cpp), and the first to step the time-lapse's
# rates on keys (time_controls.cpp): `input` for the bindings, the tick state and the log,
# `tunables` for the camera's speeds and tick rate.
set(_view_deps base containers hash math time jobs log platform window image io gfx renderer json
               schema schemas bench input tunables)
if(TARGET engine::animation)
  list(APPEND _view_deps ids animation)
endif()

# The camera and the conversion are compiled into the tests as well as the app: the trajectory of
# the committed session fixture is checked with no GPU and no window (tests/fly_tests.cpp), which
# is the half of the replay test that runs on every machine CI has. The walk mode (walk.cpp) is the
# camera's too, and walks on the scene's ground provider in every configuration.
set(_view_interactive fly_camera.cpp window_input.cpp time_controls.cpp walk.cpp)
if(TARGET engine::scene_collision)
  # The walker's collision (walk.h, docs/subsystems/scene_collision.md): a `physics::CharacterBody`
  # on the static bodies the scene-collision capability keeps round it. Both capabilities are
  # optional (ADR-0027), so this is `--animate`'s hand-made switch again: linked where they are
  # configured, and without them the walker follows the ground, which the minimal build proves.
  list(APPEND _view_deps physics scene_collision world sim)
endif()
set(_view_tests tests/view_tests.cpp tests/mesh_view_tests.cpp tests/fly_tests.cpp
                tests/interactive_tests.cpp tests/time_controls_tests.cpp tests/walk_tests.cpp
                tests/walk_view_tests.cpp tests/far_tests.cpp ${_view_interactive})
if(TARGET engine::ruins)
  # A scene's `ruins` entry drawn end to end (docs/subsystems/ruins.md): the renderer's scene reader
  # expands it through the registry, and the test writes the kit of boxes and counts the pieces with
  # the assembler. The walk into one of a ruin's walls writes the same kit beside the committed
  # ruins-walk scene (docs/subsystems/apps.md, "Walking").
  list(APPEND _view_tests tests/ruins_view_tests.cpp tests/walk_ruins_view_tests.cpp)
endif()
# The scene generators this configuration has (ADR-0046; docs/subsystems/scene_gen.md): linked for
# their registrars, which is what makes `--scene` read a terrain that names the dunes and a scene's
# ruins and city, and `--world` stream them. Nothing here calls one: the renderer's scene reader and
# the world's placements consumer find each by the name a scene gives.
foreach(_gen city ruins sky terrain)
  if(TARGET engine::${_gen})
    list(APPEND _view_deps ${_gen})
  endif()
endforeach()
if(TARGET engine::world)
  # `--world`: a streamed scene's tile ring with the camera as its observer (world_view.cpp,
  # docs/subsystems/world.md). The world capability is optional (ADR-0027), so this is the same
  # hand-made switch as `--animate`'s: linked where it is configured, `--world` refused elsewhere.
  list(APPEND _view_deps world sim)
  set(_view_world world_view.cpp)
endif()
if(TARGET engine::animation)
  # The animation-LOD glue is a header beside main.cpp (`anim_lod.h`) rather than a block inside
  # it, because it is the pattern a game copies and because it is worth testing on its own: the
  # observer set, the per-view weights, the frustum gate and the band scale are arithmetic, and
  # these cases drive them with no GPU and no world. Without the capability the header is not
  # compiled at all, which is what the minimal build proves.
  list(APPEND _view_tests tests/anim_lod_tests.cpp)
endif()

engine_app(NAME engine_view OUTPUT engine-view
  SOURCES main.cpp ${_view_interactive} ${_view_world}
  DEPS ${_view_deps}
  E2E_TESTS ${_view_tests})
# engine-view presents, so it is one of the few targets that see the Vulkan backend set
# (domain/gfx/backend/vulkan/: the swapchain, the device's instance for the surface); the renderer
# it drives does not (docs/subsystems/gfx.md, "The RHI surface and the backend surface").
target_link_libraries(engine_engine_view PRIVATE engine::gfx_vulkan)

if(TARGET engine_engine_view_tests)
  # The session fixture, its trajectory and the default map live under content/, found relative to
  # the source tree (or the test bundle's copy of it: tests/support/test_paths.h).
  target_compile_definitions(engine_engine_view_tests PRIVATE
    ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  if(TARGET engine::terrain)
    # The cases over a dune scene skip only in a build whose engine-view has no dunes to draw it.
    target_compile_definitions(engine_engine_view_tests PRIVATE ENGINE_VIEW_TESTS_DUNES=1)
  endif()
endif()

if(TARGET engine::animation)
  # `engine_app(NAME engine_view)` builds the target `engine_engine_view`, and its end-to-end test
  # executable is that name plus `_tests`; both need the switch, because the test drives the flag.
  target_compile_definitions(engine_engine_view PRIVATE ENGINE_VIEW_ANIMATION=1)
  if(TARGET engine_engine_view_tests)
    target_compile_definitions(engine_engine_view_tests PRIVATE ENGINE_VIEW_ANIMATION=1)
  endif()
endif()
if(TARGET engine::world)
  target_compile_definitions(engine_engine_view PRIVATE ENGINE_VIEW_WORLD=1)
  if(TARGET engine_engine_view_tests)
    target_compile_definitions(engine_engine_view_tests PRIVATE ENGINE_VIEW_WORLD=1)
  endif()
endif()
if(TARGET engine::scene_collision)
  target_compile_definitions(engine_engine_view PRIVATE ENGINE_VIEW_WALK_PHYSICS=1)
  if(TARGET engine_engine_view_tests)
    target_compile_definitions(engine_engine_view_tests PRIVATE ENGINE_VIEW_WALK_PHYSICS=1)
  endif()
endif()
# The mesh end-to-end test also drives engine-content; apps/engine_content/CMakeLists.txt adds
# the build dependency, since that target is declared after this one.
