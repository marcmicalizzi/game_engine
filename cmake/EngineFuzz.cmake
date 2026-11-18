# The hostile-input harness's build half (roadmap T13; docs/ci/fuzzing.md;
# docs/experiments/fuzzing-design.md, "2. The harness: both, with different jobs").
#
# A target is one source file under a module's tests/ that defines `Verdict one(std::span<const u8>)`
# and names it with ENGINE_FUZZ_TARGET (tests/support/fuzz_target.h). engine_module_fuzz() compiles
# it two ways:
#   - into the module's test executable, as the doctest case "fuzz: <target>", in every preset; the
#     in-tree mutators of tests/support/fuzz_harness.h feed it a counted number of inputs, so
#     `-Affected` selects it with its module and nothing new is scheduled;
#   - under ENGINE_FUZZ (the linux-clang-fuzz preset), into one libFuzzer executable a target,
#     build/<preset>/fuzz/engine_fuzz_<target>, with ENGINE_FUZZ_LIBFUZZER defined so the same file
#     defines LLVMFuzzerTestOneInput. Not a CTest test: libFuzzer runs until it is told to stop.

option(ENGINE_FUZZ "Instrument the tree for libFuzzer and build one executable a hostile-input target (Clang only; the linux-clang-fuzz preset)" OFF)

if(ENGINE_FUZZ)
  if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    # MSVC's /fsanitize=fuzzer is experimental and declined (docs/experiments/fuzzing-design.md,
    # "2. The harness: both, with different jobs").
    message(FATAL_ERROR "ENGINE_FUZZ needs Clang with its fuzzer runtime; use the linux-clang-fuzz preset")
  endif()
  # Coverage instrumentation on every C++ target compiled from here on, engine and third party
  # alike; the sanitizers come from the preset (ENGINE_ASAN, ENGINE_UBSAN). C++ only: the C
  # dependencies are compiled by the system's `cc`, GCC on the Linux images, which has no such flag
  # (cmake/EngineOptions.cmake says the same of the ignore list). Only the libFuzzer executables
  # link libFuzzer itself (-fsanitize=fuzzer, below); every other executable links the sanitizers'
  # runtime, which carries the coverage callbacks' weak definitions.
  add_compile_options("$<$<COMPILE_LANG_AND_ID:CXX,Clang>:-fsanitize=fuzzer-no-link>")
  message(STATUS "ENGINE_FUZZ: -fsanitize=fuzzer-no-link on every target; engine_fuzz_<target> executables under ${CMAKE_BINARY_DIR}/fuzz")
endif()

#   engine_module_fuzz(NAME <module> TARGETS <target>=<source>... [SOURCES <file>...] [DEPS <module>...])
#
# TARGETS: one entry a target, its name (the doctest case is "fuzz: <name>", the libFuzzer
# executable engine_fuzz_<name>, its corpus content/fuzz/<name>/) and its source, relative to the
# module's directory. SOURCES: test-only files that use the harness without being a target (the
# harness's own tests). DEPS: modules the targets reach beyond the module itself.
#
# Call it after engine_module_tests(NAME <module> ...): it adds to that executable.
function(engine_module_fuzz)
  cmake_parse_arguments(EF "" "NAME" "TARGETS;SOURCES;DEPS" ${ARGN})
  if(NOT EF_NAME OR NOT EF_TARGETS)
    message(FATAL_ERROR "engine_module_fuzz: NAME and TARGETS are required")
  endif()
  get_property(_disabled GLOBAL PROPERTY ENGINE_DISABLED_MODULES)
  if("${EF_NAME}" IN_LIST _disabled)
    return()
  endif()
  if(NOT TARGET engine_${EF_NAME})
    message(FATAL_ERROR "engine_module_fuzz(${EF_NAME}): declare the module with engine_module() first")
  endif()

  # What the harness itself links: the JSON layer's parser and writer, the allocator's counters,
  # and the process API the harness's child runs use.
  set(_links engine::${EF_NAME} engine::json engine::memory engine::platform)
  foreach(_dep IN LISTS EF_DEPS)
    if(NOT TARGET engine_${_dep})
      message(FATAL_ERROR "engine_module_fuzz(${EF_NAME}): dependency '${_dep}' is not declared")
    endif()
    list(APPEND _links engine::${_dep})
  endforeach()

  set(_names "")
  set(_sources "")
  foreach(_entry IN LISTS EF_TARGETS)
    if(NOT _entry MATCHES "^([a-z0-9_]+)=(.+)$")
      message(FATAL_ERROR "engine_module_fuzz(${EF_NAME}): '${_entry}' is not <target>=<source>")
    endif()
    list(APPEND _names "${CMAKE_MATCH_1}")
    list(APPEND _sources "${CMAKE_MATCH_2}")
  endforeach()

  set(_test_target engine_${EF_NAME}_tests)
  if(ENGINE_BUILD_TESTS)
    if(NOT TARGET ${_test_target})
      message(FATAL_ERROR "engine_module_fuzz(${EF_NAME}): call engine_module_tests(NAME ${EF_NAME} ...) first")
    endif()
    target_sources(${_test_target} PRIVATE ${_sources} ${EF_SOURCES})
    target_link_libraries(${_test_target} PRIVATE ${_links})
    # Where the corpus is (tests/support/test_paths.h takes it over in a test bundle), and the
    # executable's own file name, which a case uses to run one input in a child of itself.
    target_compile_definitions(${_test_target} PRIVATE
      ENGINE_FUZZ_CONTENT_DIR="${CMAKE_SOURCE_DIR}/content/fuzz"
      ENGINE_FUZZ_TEST_EXE_NAME="$<TARGET_FILE_NAME:${_test_target}>")
  endif()

  if(ENGINE_FUZZ)
    list(LENGTH _names _count)
    math(EXPR _last "${_count} - 1")
    foreach(_i RANGE ${_last})
      list(GET _names ${_i} _name)
      list(GET _sources ${_i} _source)
      set(_exe engine_fuzz_${_name})
      if(TARGET ${_exe})
        message(FATAL_ERROR "engine_module_fuzz(${EF_NAME}): a target named '${_name}' already exists")
      endif()
      add_executable(${_exe} ${_source})
      target_link_libraries(${_exe} PRIVATE ${_links})
      target_include_directories(${_exe} PRIVATE "${CMAKE_SOURCE_DIR}/tests/support")
      target_compile_definitions(${_exe} PRIVATE ENGINE_FUZZ_LIBFUZZER=1)
      target_link_options(${_exe} PRIVATE -fsanitize=fuzzer)
      engine_apply_warnings(${_exe})
      set_target_properties(${_exe} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/fuzz")
    endforeach()
  endif()
endfunction()
