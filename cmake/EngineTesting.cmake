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

# **clang 22 turns doctest's `__COUNTER__` into 1,797 errors, and it is doctest's, not ours.**
# `-Wc2y-extensions` is new in clang 22 and fires on `__COUNTER__`, which C standardized only in
# C2y; `DOCTEST_ANONYMOUS(x)` is `DOCTEST_CAT(x, __COUNTER__)`, so every `TEST_CASE` in the tree
# trips it and `-Werror` ends the build. Found on the Gentoo GPU server, which ships clang 22.1.8
# (docs/ci/remote-linux.md); the container's clang 18 has no such warning.
#
# Marking doctest SYSTEM above is not enough: clang suppresses diagnostics *inside* a system
# header, and this one is reported at the expansion site, which is our `.cpp`. So the suppression
# has to be named, and it is named as narrowly as it can be — one diagnostic, on Clang 22 and
# newer only, carried by `engine_test_main`'s INTERFACE so that it reaches test executables and
# **nothing else in the tree**. No engine translation unit uses `__COUNTER__`; if one ever does,
# it will still be warned about, because the flag is not on that target.
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 22)
  target_compile_options(engine_test_main INTERFACE -Wno-c2y-extensions)
  message(STATUS "clang ${CMAKE_CXX_COMPILER_VERSION}: -Wno-c2y-extensions on test targets (doctest's __COUNTER__)")
endif()
# The test main calls platform::require_cpu_baseline() first (ADR-0031), so every test
# executable carries the check without its module having to ask for it. engine::platform is
# declared later (core layer, and this file is included before add_subdirectory(core)); CMake
# resolves the link at generate time, the same way engine_bench_main links engine::bench.
target_link_libraries(engine_test_main PUBLIC engine::platform)

# Banned-pattern lint as a test so CI cannot forget it.
find_program(ENGINE_PWSH NAMES pwsh powershell)
if(ENGINE_PWSH)
  add_test(NAME lint.banned_patterns
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/lint.ps1" -Root "${CMAKE_SOURCE_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(lint.banned_patterns PROPERTIES LABELS "lint")

  # And the lint's own tests, over fixture trees in a temporary directory: the rules it enforces
  # include ADR-0028 seam 5's confinement of <flecs.h>, and a rule that silently stops matching
  # is worse than no rule.
  add_test(NAME tools.lint
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/lint.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.lint PROPERTIES LABELS "tools")

  # The capability scaffold (ADR-0027) generates into a temporary tree and checks the result.
  add_test(NAME tools.new_capability
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/new-capability.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.new_capability PROPERTIES LABELS "tools")

  # The generator-service tool (docs/content-generation.md), offline: the ComfyUI workflow
  # conversion against a synthetic /object_info, and the provenance sidecar through the Tripo
  # folder backend, which needs no service. Nothing here reaches a network or spends a credit.
  add_test(NAME tools.generate
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/generate.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.generate PROPERTIES LABELS "tools")

  # The E10 harness's judging and report (the STALE BINARIES caveat, the low-coverage mark) over a
  # synthetic report, and its staleness check against this tree's own binaries when they exist. No
  # GPU and no captures.
  add_test(NAME tools.e10_harness
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/e10-harness.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.e10_harness PROPERTIES LABELS "tools")

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

  # The machine-lock protocol (tools/lib/MachineLock.psm1, tools/gpu-lock.ps1) over lock files in
  # a scratch directory: the GPU lock is shared with tools outside this repository and the Linux
  # build lock with every checkout on the machine, so its rules are tested as rules.
  add_test(NAME tools.machine_lock
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/machine-lock.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.machine_lock PROPERTIES LABELS "tools")
else()
  message(WARNING "pwsh not found; lint.banned_patterns, tools.lint, tools.new_capability, tools.generate, tools.e10_harness, docs_check, tools.docs_check, and tools.machine_lock tests not registered")
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
