# engine-cli: thin scriptable client; spawns engine-host and sends one method call.
# render_tests.cpp drives engine-host directly as well, because a scene id lives in the host and
# the CLI spawns one host per invocation: the whole render.* round trip is one process.
engine_app(NAME engine_cli OUTPUT engine-cli
  SOURCES main.cpp
  DEPS base containers json platform
  E2E_TESTS tests/cli_tests.cpp tests/render_tests.cpp)
if(TARGET engine_engine_cli_tests)
  target_compile_definitions(engine_engine_cli_tests PRIVATE ENGINE_HOST_PATH="$<TARGET_FILE:engine_engine_host>")
  add_dependencies(engine_engine_cli_tests engine_engine_host)
endif()
