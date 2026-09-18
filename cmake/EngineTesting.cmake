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

# The CI documentation gate is bash, because it runs on the hosted Linux runner (.github/workflows/ci.yml);
# its tests build throwaway git repositories, so they need git as well. Where either is missing the
# test is simply not registered — the gate itself still runs in CI, which is where it decides anything.
find_program(ENGINE_BASH NAMES bash)
find_program(ENGINE_GIT NAMES git)
if(ENGINE_BASH AND ENGINE_GIT)
  add_test(NAME tools.docs_gate
    COMMAND "${ENGINE_BASH}" "${CMAKE_SOURCE_DIR}/tools/docs-gate.test.sh"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.docs_gate PROPERTIES LABELS "tools")
else()
  message(STATUS "bash or git not found; tools.docs_gate test not registered")
endif()
