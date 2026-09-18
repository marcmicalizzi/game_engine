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
set(_view_deps base containers math time log platform window image io gfx renderer json bench)
if(TARGET engine::animation)
  list(APPEND _view_deps ids animation)
endif()

engine_app(NAME engine_view OUTPUT engine-view
  SOURCES main.cpp
  DEPS ${_view_deps}
  E2E_TESTS tests/view_tests.cpp tests/mesh_view_tests.cpp)

if(TARGET engine::animation)
  # `engine_app(NAME engine_view)` builds the target `engine_engine_view`, and its end-to-end test
  # executable is that name plus `_tests`; both need the switch, because the test drives the flag.
  target_compile_definitions(engine_engine_view PRIVATE ENGINE_VIEW_ANIMATION=1)
  if(TARGET engine_engine_view_tests)
    target_compile_definitions(engine_engine_view_tests PRIVATE ENGINE_VIEW_ANIMATION=1)
  endif()
endif()
# The mesh end-to-end test also drives engine-content; apps/engine_content/CMakeLists.txt adds
# the build dependency, since that target is declared after this one.
