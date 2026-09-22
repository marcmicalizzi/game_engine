# schemac: the schema compiler. A standalone tool (standard library only) so it builds before
# any engine module and on any host.
add_executable(engine_schemac
  src/main.cpp
  src/parser.cpp
  src/resolve.cpp
  src/emit_cpp.cpp
  src/emit_ecs.cpp
  src/emit_json_schema.cpp
  src/emit_docs.cpp)
target_compile_features(engine_schemac PRIVATE cxx_std_20)
target_include_directories(engine_schemac PRIVATE src)
engine_apply_warnings(engine_schemac)
set_target_properties(engine_schemac PROPERTIES OUTPUT_NAME schemac)
# A host tool (ADR-0034): the build runs it on the build machine and it never ships, so it is
# compiled for the compiler's default rather than for ENGINE_CPU_BASELINE. Compiled for x86-64-v3 it
# died with SIGILL six minutes into a v3 build on the Sandy Bridge GPU server, having no
# `require_cpu_baseline()` of its own to say why — it links no engine module, by design.
engine_host_tool(engine_schemac)

# Negative and positive tests drive the built compiler as a child process.
if(ENGINE_BUILD_TESTS)
  add_executable(engine_schemac_tests tests/schemac_tests.cpp)
  # engine::platform and engine::io are declared later (core, foundation); CMake resolves them
  # at generate time.
  target_link_libraries(engine_schemac_tests PRIVATE engine::platform engine::io engine_test_main)
  target_compile_definitions(engine_schemac_tests PRIVATE ENGINE_SCHEMAC_PATH="$<TARGET_FILE:engine_schemac>")
  add_dependencies(engine_schemac_tests engine_schemac)
  engine_apply_warnings(engine_schemac_tests)
  add_test(NAME schemac COMMAND engine_schemac_tests)
  set_tests_properties(schemac PROPERTIES LABELS "unit;schemac")
endif()
