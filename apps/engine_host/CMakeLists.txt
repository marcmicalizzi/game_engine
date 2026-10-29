# engine-host: the headless engine server (ADR-0001). JSON-RPC over stdio, the document methods
# from domain/protocol, `render.*` over systems/renderer with no window in the process, and the
# rest of plan 06 §6.9's day-one operations — content.build, session.events, engine.budgets,
# session.run_headless and engine.run_tests (ops_methods.h) — over the modules they need
# (docs/subsystems/apps.md, protocol.md, renderer.md). It depends on `renderer` the way any app
# depends on a system: apps are L4 and systems are L3.
#
# `--console` (console.cpp) runs a Luau script against the dispatcher over the scripting capability
# (docs/subsystems/apps.md, "The console"); linked like the others below, and without it the flag
# says so and exits 2.
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
               schemas bench time platform jobs geometry gfx sim content_build doc input net doc_scene
               profiling)
set(_host_defs)
foreach(_cap audio ecs animation audio_system kinematics npc scripting store tissue world)
  if(TARGET engine::${_cap})
    list(APPEND _host_deps ${_cap})
    string(TOUPPER ${_cap} _cap_upper)
    list(APPEND _host_defs ENGINE_HOST_${_cap_upper}=1)
  endif()
endforeach()
# The scene generators this configuration has (ADR-0046; docs/subsystems/scene_gen.md): linked for
# their registrars alone, which is what makes `render.load` read a scene whose terrain names the
# dunes or whose entries name the ruins or the city, and the flora capability for its mesh generator,
# which makes a scene's `.flora.json` mesh a palm, and the tile set for its `tiles` ground provider,
# which makes an authored terrain draw (docs/subsystems/tileset.md). Nothing here calls one: the
# scene reader finds
# each by the name a scene gives.
foreach(_gen authored city cover deadwood flora rocks ruins scatter sky soil terrain tileset water wetland)
  if(TARGET engine::${_gen})
    list(APPEND _host_deps ${_gen})
  endif()
endforeach()
engine_app(NAME engine_host OUTPUT engine-host
  SOURCES main.cpp render_methods.cpp ops_methods.cpp console.cpp listen.cpp
  DEPS ${_host_deps}
  E2E_TESTS tests/transport_tests.cpp)
if(_host_defs)
  target_compile_definitions(engine_engine_host PRIVATE ${_host_defs})
endif()
# A save names the engine that wrote it (`SaveManifest::saved_by`), as engine.info does.
target_compile_definitions(engine_engine_host PRIVATE ENGINE_VERSION="${PROJECT_VERSION}")
# `bench` is here for foundation/bench/machine_state.h alone: render.benchmark reports what else
# the machine was doing, in the same shape and with the same thresholds the CPU benchmarks use,
# rather than growing a second copy of the sampler (docs/subsystems/bench.md).

# `--listen` (listen.cpp, docs/subsystems/apps.md): the protocol over a loopback WebSocket through
# foundation/net, and `net` and `doc_scene` above are what it and `render.load {session}` need.
# The listener serves the browser shell (apps/engine_shell, docs/subsystems/engine_shell.md) on
# GET /, from build/<preset>/shell/ beside shaders/manifest.json: the files are copied there at
# build, one rule per file so an edited page is copied again and nothing else is.
file(GLOB_RECURSE _shell_sources CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/apps/engine_shell/*")
set(_shell_outputs)
foreach(_source IN LISTS _shell_sources)
  file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}/apps/engine_shell" "${_source}")
  set(_output "${CMAKE_BINARY_DIR}/shell/${_rel}")
  get_filename_component(_output_dir "${_output}" DIRECTORY)
  add_custom_command(OUTPUT "${_output}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_output_dir}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_source}" "${_output}"
    DEPENDS "${_source}" VERBATIM)
  list(APPEND _shell_outputs "${_output}")
endforeach()
add_custom_target(engine_shell_files DEPENDS ${_shell_outputs})
add_dependencies(engine_engine_host engine_shell_files)

if(TARGET engine_engine_host_tests)
  if(TARGET engine::tileset)
    target_compile_definitions(engine_engine_host_tests PRIVATE ENGINE_HOST_TEST_TILESET=1)
  endif()
  # transport_tests.cpp starts engine-host with --listen and drives it through foundation/net's own
  # client (engine_app above links the host's modules into it and marks it as one that can wait for
  # the GPU lock, since render.load {session} and A32's camera open a device).
  # And a second, independent client: .NET's ClientWebSocket against the same host
  # (tools/host-ws.Tests.ps1), because a framing bug the in-tree server and client shared would be
  # invisible to the test above. Named engine_host.<part> so `test -Affected` runs it with the rest.
  if(ENGINE_PWSH)
    add_test(NAME engine_host.host_ws
      COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
              -File "${CMAKE_SOURCE_DIR}/tools/host-ws.Tests.ps1"
              -HostExe "$<TARGET_FILE:engine_engine_host>"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(engine_host.host_ws PROPERTIES LABELS "engine_host;tools")
  endif()
endif()
