# engine-host: the headless engine server (ADR-0001). JSON-RPC over stdio, the document methods
# from domain/protocol, `render.*` over systems/renderer with no window in the process, and the
# rest of plan 06 §6.9's day-one operations — content.build, session.events, engine.budgets,
# session.run_headless and engine.run_tests (ops_methods.h) — over the modules they need
# (docs/subsystems/apps.md, protocol.md, renderer.md). It depends on `renderer` the way any app
# depends on a system: apps are L4 and systems are L3.
#
# Every capability a method reaches is optional (ADR-0027). An app is not a capability and cannot
# declare `engine_capability_requires`, so it does what engine-view does for animation: link the
# module when this configuration has it and define ENGINE_HOST_<NAME> so the code that uses it
# compiles. Without one, the method that needs it answers 1006 Unavailable naming it (the store
# for `session.events` with `source: "store"`, the tissue validators for `engine.run_tests` with a
# tissue file), a method that merely reports less says so in its result (`engine.budgets` without
# audio), and `audio.devices` is "method not found". The runtime world of `session.run_headless`
# and `session.materialize` is the engine's own scheduler, with a flecs world whose systems run
# inside its phases and the ticking capabilities installed when ECS is built, and the scheduler
# alone — which materializes nothing and says so — when it is not. `msvc-minimal` and the
# `*-no-ecs` presets are the proof.
set(_host_deps base containers hash json log io image memory tunables protocol renderer schema
               schemas bench time platform jobs geometry gfx sim content_build doc input)
set(_host_defs)
foreach(_cap audio ecs animation audio_system kinematics store tissue world)
  if(TARGET engine::${_cap})
    list(APPEND _host_deps ${_cap})
    string(TOUPPER ${_cap} _cap_upper)
    list(APPEND _host_defs ENGINE_HOST_${_cap_upper}=1)
  endif()
endforeach()
engine_app(NAME engine_host OUTPUT engine-host
  SOURCES main.cpp render_methods.cpp ops_methods.cpp
  DEPS ${_host_deps})
if(_host_defs)
  target_compile_definitions(engine_engine_host PRIVATE ${_host_defs})
endif()
# A save names the engine that wrote it (`SaveManifest::saved_by`), as engine.info does.
target_compile_definitions(engine_engine_host PRIVATE ENGINE_VERSION="${PROJECT_VERSION}")
# `bench` is here for foundation/bench/machine_state.h alone: render.benchmark reports what else
# the machine was doing, in the same shape and with the same thresholds the CPU benchmarks use,
# rather than growing a second copy of the sampler (docs/subsystems/bench.md).
