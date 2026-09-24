# engine-host: the headless engine server (ADR-0001). JSON-RPC over stdio, the document methods
# from domain/protocol, and `render.*` over systems/renderer with no window in the process
# (docs/subsystems/apps.md, renderer.md). It depends on `renderer` the way any app depends on a
# system: apps are L4 and systems are L3.
#
# `audio.devices` comes from the audio capability, which is optional (ADR-0027). An app is not a
# capability and cannot declare `engine_capability_requires`, so it does what engine-view does for
# animation: link the module when this configuration has it and define ENGINE_HOST_AUDIO so the one
# registration line compiles. Without it the host has no audio.* methods and says so the usual way,
# "method not found".
set(_host_deps base containers hash json log io image tunables protocol renderer schema schemas bench)
if(TARGET engine::audio)
  list(APPEND _host_deps audio)
endif()
engine_app(NAME engine_host OUTPUT engine-host
  SOURCES main.cpp render_methods.cpp
  DEPS ${_host_deps})
if(TARGET engine::audio)
  target_compile_definitions(engine_engine_host PRIVATE ENGINE_HOST_AUDIO=1)
endif()
# `bench` is here for foundation/bench/machine_state.h alone: render.benchmark reports what else
# the machine was doing, in the same shape and with the same thresholds the CPU benchmarks use,
# rather than growing a second copy of the sampler (docs/subsystems/bench.md).
