# engine-view: the window on systems/renderer (docs/plan/02-architecture.md §2.2,
# docs/subsystems/renderer.md). The flags, the window and its swapchain, the event loop, the
# capture, and the JSON summary; every pass that draws lives in the renderer module, which
# engine-host is the second consumer of. --frames N --capture out.png is how agents and CI look
# at it.
# `json` builds the one block of the summary that has nulls in it, and `bench` is there for
# foundation/bench/machine_state.h: the summary says what else the machine was doing, in the
# same shape and with the same thresholds the CPU benchmarks use (docs/subsystems/bench.md).
engine_app(NAME engine_view OUTPUT engine-view
  SOURCES main.cpp
  DEPS base containers math time log platform window image io gfx renderer json bench
  E2E_TESTS tests/view_tests.cpp tests/mesh_view_tests.cpp)
# The mesh end-to-end test also drives engine-content; apps/engine_content/CMakeLists.txt adds
# the build dependency, since that target is declared after this one.
