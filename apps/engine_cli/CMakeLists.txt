# engine-cli: thin scriptable client; spawns engine-host and sends one method call.
# render_tests.cpp drives engine-host directly as well, because a scene id lives in the host and
# the CLI spawns one host per invocation: the whole render.* round trip is one process.
# ops_tests.cpp does both for content.build, session.events, engine.budgets, session.run_headless
# and engine.run_tests, since a session's runtime world also lives in the host.
engine_app(NAME engine_cli OUTPUT engine-cli
  SOURCES main.cpp
  DEPS base containers json platform
  E2E_TESTS tests/cli_tests.cpp tests/render_tests.cpp tests/ops_tests.cpp tests/policy_tests.cpp
            tests/save_tests.cpp tests/npc_replay_tests.cpp tests/console_tests.cpp)
if(TARGET engine_engine_cli_tests)
  target_compile_definitions(engine_engine_cli_tests PRIVATE ENGINE_HOST_PATH="$<TARGET_FILE:engine_engine_host>")
  add_dependencies(engine_engine_cli_tests engine_engine_host)
  # The engine-host these tests start opens a GPU device for render.*, and so can wait for the
  # machine-wide GPU lock (ADR-0049).
  engine_test_waits_for_gpu(engine_cli)
  # render_tests.cpp holds `render.capture` to engine-view's capture with the same flags, byte for
  # byte (docs/subsystems/renderer.md, "One request, two hosts"), so it runs engine-view too — after
  # the host it started has exited, since each takes the GPU lock for as long as it has a device. It
  # reads the pictures and the id buffer back with the image module, and checks a sky's reported sun
  # against the sky capability's own ephemeris where the build has it. engine-view is declared after
  # this file, which a generator expression and a dependency may name.
  target_compile_definitions(engine_engine_cli_tests PRIVATE
    ENGINE_VIEW_PATH="$<TARGET_FILE:engine_engine_view>")
  add_dependencies(engine_engine_cli_tests engine_engine_view)
  target_link_libraries(engine_engine_cli_tests PRIVATE engine::image)
  if(TARGET engine::sky)
    target_link_libraries(engine_engine_cli_tests PRIVATE engine::sky)
    target_compile_definitions(engine_engine_cli_tests PRIVATE ENGINE_CLI_TESTS_SKY=1)
  endif()
  # save_tests.cpp writes the player's action map and input log with the input module itself
  # (docs/subsystems/world.md, "The player"), and reads the save corpus under content/.
  target_link_libraries(engine_engine_cli_tests PRIVATE engine::input)
  target_compile_definitions(engine_engine_cli_tests PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
  # session.events reads a document's world.db when the host has the store capability; the case
  # that checks it writes one with the store itself, so it links it exactly when the host does.
  if(TARGET engine::store)
    target_link_libraries(engine_engine_cli_tests PRIVATE engine::store)
    target_compile_definitions(engine_engine_cli_tests PRIVATE ENGINE_CLI_TESTS_STORE=1)
  endif()
  # session.run_headless moves `Mover` records when the host has the kinematics capability (and so
  # the entity store); without it the materialization report says why nothing moved, and the case
  # that checks the report checks that instead.
  if(TARGET engine::kinematics)
    target_compile_definitions(engine_engine_cli_tests PRIVATE ENGINE_CLI_TESTS_KINEMATICS=1)
  endif()
  # session.run_headless streams a partitioned document tile by tile when the host has the world
  # capability (docs/subsystems/world.md); without it the `stream` field is 1006 Unavailable.
  if(TARGET engine::world)
    target_compile_definitions(engine_engine_cli_tests PRIVATE ENGINE_CLI_TESTS_WORLD=1)
  endif()
  # A world of 10^5 scheduled residents, streamed, saved, loaded and replayed (npc.md, "Held to the
  # clause"): the test writes its document with the generator itself, so it links the capability
  # exactly when the host has it and the world to stream it.
  if(TARGET engine::npc AND TARGET engine::world)
    target_link_libraries(engine_engine_cli_tests PRIVATE engine::npc)
    target_compile_definitions(engine_engine_cli_tests PRIVATE ENGINE_CLI_TESTS_NPC=1)
  endif()
  # console_tests.cpp runs Luau scripts through `engine-host --console` and `engine-cli --console`
  # (apps.md, "The console") when the host has the scripting capability; without it the case checks
  # that --console says so and exits 2. The scripts are content/console/'s.
  if(TARGET engine::scripting)
    target_compile_definitions(engine_engine_cli_tests PRIVATE ENGINE_CLI_TESTS_SCRIPTING=1)
  endif()
  # engine.run_tests runs the tissue validators when the host has the tissue capability; the case
  # makes its fixture with `engine-content tissue example`, which is built in the same
  # configurations. The target is declared after this file, which a generator expression and a
  # dependency may name.
  if(TARGET engine::tissue)
    target_compile_definitions(engine_engine_cli_tests PRIVATE ENGINE_CLI_TESTS_TISSUE=1
      ENGINE_CONTENT_PATH="$<TARGET_FILE:engine_engine_content>")
    add_dependencies(engine_engine_cli_tests engine_engine_content)
  endif()
endif()
