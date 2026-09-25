# engine-cli: thin scriptable client; spawns engine-host and sends one method call.
# render_tests.cpp drives engine-host directly as well, because a scene id lives in the host and
# the CLI spawns one host per invocation: the whole render.* round trip is one process.
# ops_tests.cpp does both for content.build, session.events, engine.budgets, session.run_headless
# and engine.run_tests, since a session's runtime world also lives in the host.
engine_app(NAME engine_cli OUTPUT engine-cli
  SOURCES main.cpp
  DEPS base containers json platform
  E2E_TESTS tests/cli_tests.cpp tests/render_tests.cpp tests/ops_tests.cpp tests/policy_tests.cpp)
if(TARGET engine_engine_cli_tests)
  target_compile_definitions(engine_engine_cli_tests PRIVATE ENGINE_HOST_PATH="$<TARGET_FILE:engine_engine_host>")
  add_dependencies(engine_engine_cli_tests engine_engine_host)
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
