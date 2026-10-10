# scripting: the Luau host for content logic (docs/plan/08-toolchain.md §8.2, experiment E7) — a
# sandboxed VM with a memory limit and a deterministic step budget per call, read-only bindings
# generated from the schema IDL, hot reload, and a Luau type-definition file emitted from the same
# descriptions (docs/subsystems/scripting.md).
#
# OPTIONAL (ADR-0027): ENGINE_WITH_SCRIPTING=OFF, and the minimal build, take the module, its
# tests, its bench, and the Luau download with it. The pin lives in cmake/EngineScripting.cmake.
engine_capability_enabled(scripting _engine_scripting)
if(NOT _engine_scripting)
  return()
endif()

include(EngineScripting)
engine_fetch_luau()

engine_module(NAME scripting LAYER foundation OPTIONAL
  DEPS base containers memory hash ids json log time schema tunables io
  SOURCES src/script_context.cpp src/bindings.cpp src/values.cpp src/type_definitions.cpp
          src/scripting.cpp src/program.cpp)

# PRIVATE: Luau is an implementation detail. For a static library CMake still carries the link
# dependency to whoever links scripting ($<LINK_ONLY:...>), but not the include directories or the
# LUA_* definitions, so no public header can include lua.h by accident and nothing outside src/
# compiles against it.
target_link_libraries(engine_scripting PRIVATE Luau.VM Luau.Compiler)

# The object types the tests and benches read and type-check (tests/scripting_test.schema).
engine_schema_library(NAME scripting_test_types
  SCHEMAS tests/scripting_test.schema
  CAPABILITY scripting)

engine_module_tests(NAME scripting
  SOURCES tests/scripting_tests.cpp tests/binding_tests.cpp tests/reload_tests.cpp
          tests/typed_api_tests.cpp tests/luau_check.cpp tests/size_table.cpp
          tests/program_tests.cpp
  DEPS scripting_test_types)
# The type checker is test-only: the typed API test drives Luau's analysis library with the
# emitted definition file, which is what `luau-analyze` would do if it could load one. Nothing the
# engine ships links it.
if(TARGET engine_scripting_tests)
  target_link_libraries(engine_scripting_tests PRIVATE Luau.Analysis)
endif()

# The E7 measurements (docs/experiments/e7-luau-spike.md): the call round trip, property reads,
# on_tick over shared and separate VMs, compile time, hot-reload latency, GC pauses under a steady
# allocation load, and the VM's memory floor.
engine_module_bench(NAME scripting
  SOURCES bench/scripting_bench.cpp
  DEPS scripting_test_types io)

# E7's binary-size row: the same small program with and without a script call, linking the same
# engine modules otherwise, so the difference in executable size is the scripting library plus
# Luau's VM and compiler. Built with the benches, never run by the build or the tests.
if(TARGET engine_scripting_bench)
  foreach(_variant IN ITEMS without with)
    set(_probe engine_scripting_size_probe_${_variant})
    add_executable(${_probe} bench/size_probe.cpp)
    target_link_libraries(${_probe} PRIVATE
      engine::platform engine::log engine::hash engine::json engine::schema engine::tunables
      engine::io)
    if(_variant STREQUAL "with")
      target_link_libraries(${_probe} PRIVATE engine::scripting)
      target_compile_definitions(${_probe} PRIVATE ENGINE_SIZE_PROBE_WITH_SCRIPTING=1)
    else()
      target_compile_definitions(${_probe} PRIVATE ENGINE_SIZE_PROBE_WITH_SCRIPTING=0)
    endif()
    target_compile_options(${_probe} PRIVATE ${ENGINE_NO_EXCEPTIONS_FLAGS})
    engine_apply_warnings(${_probe})
    set_target_properties(${_probe} PROPERTIES FOLDER "bench")
  endforeach()
endif()
