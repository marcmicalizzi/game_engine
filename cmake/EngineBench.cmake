# Micro-benchmark executables (docs/plan/02-architecture.md §2.5, ADR-0011).
#
#   engine_module_bench(NAME <module> SOURCES <file>... [DEPS <module>...])
#
# Builds engine_<module>_bench from the module's bench/ sources, linked against the module,
# foundation/bench, and the shared entry point, and registers a CTest smoke run (label
# "bench") so benchmarks cannot rot unnoticed. Full runs go through `tools/dev.ps1 bench`.

if(NOT DEFINED ENGINE_BUILD_BENCH)
  set(ENGINE_BUILD_BENCH ${ENGINE_BUILD_TESTS})
endif()
option(ENGINE_BUILD_BENCH "Build micro-benchmark executables and register smoke tests" ${ENGINE_BUILD_BENCH})

if(NOT ENGINE_BUILD_BENCH)
  return()
endif()

# engine::bench is declared later (foundation layer); CMake resolves the link at generate time.
add_library(engine_bench_main STATIC "${CMAKE_SOURCE_DIR}/tests/support/bench_main.cpp")
target_link_libraries(engine_bench_main PUBLIC engine::bench)
engine_apply_warnings(engine_bench_main)

function(engine_module_bench)
  set(_one NAME)
  set(_multi SOURCES DEPS)
  cmake_parse_arguments(EB "" "${_one}" "${_multi}" ${ARGN})
  if(NOT EB_NAME OR NOT EB_SOURCES)
    message(FATAL_ERROR "engine_module_bench: NAME and SOURCES are required")
  endif()
  if(NOT TARGET engine_${EB_NAME})
    message(FATAL_ERROR "engine_module_bench(${EB_NAME}): declare the module with engine_module() first")
  endif()
  set(_extra "")
  foreach(_dep IN LISTS EB_DEPS)
    list(APPEND _extra engine::${_dep})
  endforeach()

  set(_target engine_${EB_NAME}_bench)
  add_executable(${_target} ${EB_SOURCES})
  target_link_libraries(${_target} PRIVATE engine::${EB_NAME} ${_extra} engine::bench engine_bench_main)
  engine_apply_warnings(${_target})
  set_target_properties(${_target} PROPERTIES FOLDER "bench")

  add_test(NAME bench.${EB_NAME} COMMAND ${_target} --smoke --quiet --no-pin)
  set_tests_properties(bench.${EB_NAME} PROPERTIES LABELS "bench")
endfunction()
