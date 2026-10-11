# hotload: hot-reloadable C++ game modules (docs/subsystems/hotload.md) — the host that loads a
# game module built by engine_game_module() (cmake/EngineGameModule.cmake), watches its file,
# migrates its state through the schema descriptors of the old and new builds, and swaps it; and
# the module ABI (include/foundation/hotload/module_abi.h) a module builds against without linking
# any engine library. Experiment E7's C++ arm (docs/experiments/e7-luau-spike.md).
#
# OPTIONAL (ADR-0027): ENGINE_WITH_HOTLOAD=OFF, and the minimal build, take the module, its tests,
# its bench, every game module and the game layer's targets with it.

engine_module(NAME hotload LAYER foundation OPTIONAL
  DEPS base containers memory log time json schema io
  SOURCES src/module_host.cpp src/migrate.cpp src/dynamic_library.cpp)
if(TARGET engine_hotload AND NOT WIN32)
  target_link_libraries(engine_hotload PRIVATE ${CMAKE_DL_LIBS})
endif()

# The host objects the tests and the bench bind for modules to read (tests/hotload_test.schema).
engine_schema_library(NAME hotload_test_types
  SCHEMAS tests/hotload_test.schema
  CAPABILITY hotload)

engine_module_tests(NAME hotload
  SOURCES tests/hotload_tests.cpp tests/seeded_mistakes_tests.cpp tests/size_table.cpp
  DEPS hotload_test_types platform io)

if(TARGET engine_hotload_tests)
  # The test modules, built by the same helper a game uses. Each is a separate library because
  # each is a separate build of "the module" the tests swap between.
  set(_modules "${CMAKE_CURRENT_BINARY_DIR}/modules")
  engine_game_module(NAME hotload_counter_v1 OUTPUT_DIRECTORY "${_modules}"
    SOURCES tests/modules/counter_v1/counter.cpp SCHEMAS tests/modules/counter_v1/counter.schema)
  engine_game_module(NAME hotload_counter_v2 OUTPUT_DIRECTORY "${_modules}"
    SOURCES tests/modules/counter_v2/counter.cpp SCHEMAS tests/modules/counter_v2/counter.schema)
  engine_game_module(NAME hotload_counter_retyped OUTPUT_DIRECTORY "${_modules}"
    SOURCES tests/modules/counter_retyped/counter.cpp
    SCHEMAS tests/modules/counter_retyped/counter.schema)
  set(_refusals no_entry describe abi paths drops)
  set(_index 1)
  foreach(_refusal IN LISTS _refusals)
    engine_game_module(NAME hotload_refusal_${_refusal} OUTPUT_DIRECTORY "${_modules}"
      SOURCES tests/modules/refusals/refusals.cpp)
    target_compile_definitions(engine_game_hotload_refusal_${_refusal} PRIVATE HOTLOAD_REFUSAL=${_index})
    math(EXPR _index "${_index} + 1")
  endforeach()

  set(_defs "")
  foreach(_m IN ITEMS counter_v1 counter_v2 counter_retyped refusal_no_entry refusal_describe
                      refusal_abi refusal_paths refusal_drops)
    add_dependencies(engine_hotload_tests engine_game_hotload_${_m})
    string(TOUPPER "${_m}" _upper)
    list(APPEND _defs "HOTLOAD_MODULE_${_upper}=\"$<TARGET_FILE:engine_game_hotload_${_m}>\"")
  endforeach()
  target_compile_definitions(engine_hotload_tests PRIVATE ${_defs})
  # The command that compiles a module from sources in the test's own scratch directory.
  engine_game_module_build_command(engine_hotload_tests)
  # The seeded-mistake table compiles about fifteen small modules; give it room on a loaded box.
  set_tests_properties(hotload PROPERTIES TIMEOUT ${ENGINE_TEST_WORK_TIMEOUT_S})
endif()

# E7's C++ arm, mirrored row for row on the Luau arm's bench (foundation/scripting/bench): calls,
# view reads, on_tick over 1,000 and 10,000 instances, reload, migration, and the module's size.
# game/sandbox_npc adds the module it loads and its path.
engine_module_bench(NAME hotload
  SOURCES bench/hotload_bench.cpp
  DEPS hotload_test_types io)
