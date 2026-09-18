# Test infrastructure: doctest (MIT, single header) fetched at configure time,
# one shared main, and the banned-pattern lint registered as a CTest test.

if(NOT ENGINE_BUILD_TESTS)
  return()
endif()

include(CTest)
include(FetchContent)

# Only the single header is used. SOURCE_SUBDIR points at a directory that does not exist so
# FetchContent skips doctest's own CMakeLists.txt (whose minimum version predates CMake 4).
FetchContent_Declare(doctest
  GIT_REPOSITORY https://github.com/doctest/doctest.git
  GIT_TAG        v2.4.11
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  cmake-not-used)
FetchContent_MakeAvailable(doctest)

add_library(engine_test_main STATIC "${CMAKE_SOURCE_DIR}/tests/support/test_main.cpp")
# Treat doctest as a system header so our warning policy applies only to our code, and let it
# include the real standard headers so string_view and friends stringify in assertions.
target_include_directories(engine_test_main SYSTEM PUBLIC "${doctest_SOURCE_DIR}")
target_compile_definitions(engine_test_main PUBLIC DOCTEST_CONFIG_USE_STD_HEADERS)
target_include_directories(engine_test_main PUBLIC "${CMAKE_SOURCE_DIR}/tests/support")

# Banned-pattern lint as a test so CI cannot forget it.
find_program(ENGINE_PWSH NAMES pwsh powershell)
if(ENGINE_PWSH)
  add_test(NAME lint.banned_patterns
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/lint.ps1" -Root "${CMAKE_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(lint.banned_patterns PROPERTIES LABELS "lint")

  # The capability scaffold (ADR-0027) generates into a temporary tree and checks the result.
  add_test(NAME tools.new_capability
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/new-capability.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.new_capability PROPERTIES LABELS "tools")

  # "Documentation moves with the code" (AGENTS.md), over this tree: a module has a page, an ADR
  # is numbered and indexed, and every link resolves. It reads files, so it runs under every
  # preset including the minimal ones, where the documentation is the same documentation.
  add_test(NAME docs_check
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/docs-check.ps1" -Root "${CMAKE_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(docs_check PROPERTIES LABELS "docs")

  # And the check's own tests, over fixture trees in a temporary directory.
  add_test(NAME tools.docs_check
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/docs-check.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.docs_check PROPERTIES LABELS "tools")
else()
  message(WARNING "pwsh not found; lint.banned_patterns, tools.new_capability, docs_check, and tools.docs_check tests not registered")
endif()

# The CI documentation gate is bash, because it runs on the hosted Linux runner
# (.github/workflows/ci.yml); its tests build throwaway git repositories, so they need git too.
#
# Finding bash on Windows takes care: C:/Windows/System32/bash.exe is the WSL launcher, comes
# first on PATH, and on a machine with no distribution installed it fails instead of running a
# script. Git for Windows ships a real bash beside git, so that one is tried first and whichever
# candidate is picked has to prove it can run a command before a test is registered on it.
find_program(ENGINE_GIT NAMES git)

set(_engine_bash_candidates "")
if(WIN32 AND ENGINE_GIT)
  get_filename_component(_engine_git_bin "${ENGINE_GIT}" DIRECTORY)
  cmake_path(SET _engine_git_bash NORMALIZE "${_engine_git_bin}/../bin/bash.exe")
  cmake_path(SET _engine_usr_bash NORMALIZE "${_engine_git_bin}/../usr/bin/bash.exe")
  list(APPEND _engine_bash_candidates "${_engine_git_bash}" "${_engine_usr_bash}")
endif()
find_program(ENGINE_BASH_ON_PATH NAMES bash)
if(ENGINE_BASH_ON_PATH)
  list(APPEND _engine_bash_candidates "${ENGINE_BASH_ON_PATH}")
endif()

set(ENGINE_BASH "")
foreach(_candidate IN LISTS _engine_bash_candidates)
  if(NOT EXISTS "${_candidate}")
    continue()
  endif()
  execute_process(COMMAND "${_candidate}" -c "command -v git"
    RESULT_VARIABLE _engine_bash_status OUTPUT_QUIET ERROR_QUIET)
  if(_engine_bash_status EQUAL 0)
    set(ENGINE_BASH "${_candidate}")
    break()
  endif()
endforeach()

if(ENGINE_BASH)
  add_test(NAME tools.docs_gate
    COMMAND "${ENGINE_BASH}" "${CMAKE_SOURCE_DIR}/tools/docs-gate.test.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.docs_gate PROPERTIES LABELS "tools")
  message(STATUS "docs gate tests: ${ENGINE_BASH}")
else()
  message(STATUS "no bash that can run git; tools.docs_gate test not registered")
endif()
