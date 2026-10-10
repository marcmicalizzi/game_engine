# engine-mcp: the MCP bridge (docs/plan/06-agent-tooling.md §6.3, docs/subsystems/apps.md). A
# Model Context Protocol server over stdio that spawns one engine-host and speaks the engine
# protocol to it, as engine-cli does. It links no protocol or schema library: every tool's input
# schema is generated at startup from the host's own `schema.describe`, so the host is the one
# source of truth and the bridge cannot drift from it.
engine_app(NAME mcp_bridge OUTPUT engine-mcp
  SOURCES main.cpp bridge.cpp host_client.cpp schema_gen.cpp tools.cpp
  DEPS base containers json ids log platform
  E2E_TESTS tests/mcp_tests.cpp)
target_compile_definitions(engine_mcp_bridge PRIVATE ENGINE_VERSION="${PROJECT_VERSION}")
# The bridge finds engine-host beside itself, so building one builds the other.
add_dependencies(engine_mcp_bridge engine_engine_host)
if(TARGET engine_mcp_bridge_tests)
  target_compile_definitions(engine_mcp_bridge_tests PRIVATE ENGINE_HOST_PATH="$<TARGET_FILE:engine_engine_host>")
  add_dependencies(engine_mcp_bridge_tests engine_engine_host)
  # The engine-host behind the bridge opens a GPU device for the render tools, and so can wait
  # for the machine-wide GPU lock (ADR-0049).
  engine_test_waits_for_gpu(mcp_bridge)
  # The MCP conformance client (tools/mcp-check.ps1) drives this engine-mcp as an agent's client
  # does and runs plan 06 §6.10's one-agent loop over it; its tests also prove it fails a stand-in
  # server that breaks the rules it checks. Named mcp_bridge.<part> so that `tools/dev.ps1 test
  # -Affected` runs it with the bridge's own tests; its capture and benchmark take the GPU lock.
  if(ENGINE_PWSH)
    add_test(NAME mcp_bridge.mcp_check
      COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
              -File "${CMAKE_SOURCE_DIR}/tools/mcp-check.Tests.ps1"
              -Mcp "$<TARGET_FILE:engine_mcp_bridge>"
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(mcp_bridge.mcp_check PROPERTIES LABELS "mcp_bridge;tools")
    engine_test_waits_for_gpu(mcp_bridge.mcp_check)
  endif()
endif()
