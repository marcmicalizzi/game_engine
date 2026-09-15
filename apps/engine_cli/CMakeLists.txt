# engine-cli: thin scriptable client; spawns engine-host and sends one method call.
engine_app(NAME engine_cli OUTPUT engine-cli
  SOURCES main.cpp
  DEPS base containers json platform
  E2E_TESTS tests/cli_tests.cpp)
if(TARGET engine_engine_cli_tests)
  target_compile_definitions(engine_engine_cli_tests PRIVATE ENGINE_HOST_PATH="$<TARGET_FILE:engine_engine_host>")
  add_dependencies(engine_engine_cli_tests engine_engine_host)
endif()
