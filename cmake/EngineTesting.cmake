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

# The GPU lock's two numbers for a test (ADR-0049, docs/subsystems/gpu_lock.md). A test takes the
# machine-wide lock when it, or a process it starts, opens a GPU device, and waits for it when
# somebody else has it; the test main sets every wait in the test's tree to give up this many
# seconds after the test started, with exit code 75, which CTest reports as a skip. Half an hour
# is GPU-LOCK.md's rule 9 — a short job waits up to thirty minutes, because a measurement group's
# lease is ten to thirty — and so it outlasts anything short that holds the card, and not a
# diffusion batch or an hour of benchmarks, which a test should not sit behind.
set(ENGINE_TEST_GPU_LOCK_WAIT_S 1800 CACHE STRING
  "Seconds a test's processes may wait for the machine-wide GPU lock before giving up (exit 75)")
# And what a test that can wait has for its own work on top of that: the ten minutes the
# end-to-end tests have always had (cmake/EngineModule.cmake says why ten), which is also four
# times the slowest GPU module test (renderer, about 140 s in msvc-debug). A test that can wait
# gets TIMEOUT = work + queue, so a wait that ends at the deadline and then does all of the test's
# work still ends inside it. The linux-server presets raise it to 3,600 (CMakePresets.json): the
# Pascal server's renderer suite takes about 37 minutes (2,195 s and 2,207 s on gates b30 and b32,
# 2026-10-20), so its work alone was spending the queue budget too, and batch 33's heavier flora
# scenes (G73, R65) took it past 2,400 s and into a timeout.
set(ENGINE_TEST_WORK_TIMEOUT_S 600 CACHE STRING
  "Seconds of work a test that can wait for the GPU lock has on top of the wait")
target_compile_definitions(engine_test_main PRIVATE
  ENGINE_TEST_GPU_LOCK_WAIT_S=${ENGINE_TEST_GPU_LOCK_WAIT_S})

# Seconds per test source file, which a parted test's files are dealt by (_engine_test_parts in
# cmake/EngineModule.cmake; docs/ci/what-to-run.md, "Parts dealt by cost"). One line a file,
# `<test> <basename> <seconds>`; `#` starts a comment. tools/test-costs.ps1 writes it from a CTest
# log of a run with doctest's `--duration=true`, which every module and end-to-end test has. The
# reader and the deal are cmake/EngineTestDeal.cmake's, which needs no project, so the tool's
# prediction and its test run the configure's own deal in script mode.
include("${CMAKE_SOURCE_DIR}/cmake/EngineTestDeal.cmake")
set(_engine_cost_file "${CMAKE_SOURCE_DIR}/cmake/test-costs.txt")
if(EXISTS "${_engine_cost_file}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_engine_cost_file}")
  engine_read_test_costs("${_engine_cost_file}")
endif()

#   _engine_test_directories(<dir> <out>)
# <dir> and every directory under it that CMake processed, depth first.
function(_engine_test_directories dir out)
  set(_all "${dir}")
  get_property(_subs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(_s IN LISTS _subs)
    _engine_test_directories("${_s}" _more)
    list(APPEND _all ${_more})
  endforeach()
  set(${out} "${_all}" PARENT_SCOPE)
endfunction()

# Tests that assert an upper bound on wall-clock time carry the label `wall_clock`, set where each
# is registered (`engine_module_tests(... WALL_CLOCK)`, roadmap T87), and run with nothing beside
# them in their CTest invocation (RUN_SERIAL) when the run has jobs (`tools/dev.ps1 test -Jobs`, the
# Linux test presets' `execution.jobs`): `tileset` holds one height lookup beside a building window
# to 50 ms, and `scene_collision` one tile's activation and one prefetch wait to 120 ms. Every other
# timing check in the tree is a lower bound, a synthetic clock, or a ratio, which load cannot break
# (docs/ci/what-to-run.md, "Jobs"). Both say WALL_CLOCK where they are registered; nothing is
# labelled from here.

# How many GPU chains a CTest run with jobs has (roadmap T89; docs/ci/what-to-run.md, "Two GPU
# chains"): the tests labelled `gpu` are dealt over this many resource locks, `gpu_device.1` to
# `gpu_device.<n>`, so that many of them run at once in one invocation, and the machine-wide GPU
# lock orders their devices' holds as it orders two suites' (ADR-0049). Gate b66's `msvc-debug`
# chain was 4,480 s of its 4,496 s, and 2,096 s of that no test had a device open.
#
# `auto` (the default) derives the number at every configure, from two things:
#   - the machine-wide lock can order the chains' devices: its directory exists, so the protocol
#     is in use (gpu_lock.md, "Elsewhere, and the limits"). Where it cannot, one chain, since
#     nothing would stop two tests' devices being open at once;
#   - a test still fits its queue deadline beside the other chains (roadmap T171). Every wait in a
#     test's tree gives up at the test's start plus ENGINE_TEST_GPU_LOCK_WAIT_S, an absolute
#     deadline, and a test may itself run for ENGINE_TEST_WORK_TIMEOUT_S. Beside n - 1 other
#     chains on a card that bounds the suite, its holds take turns with theirs and its run
#     stretches up to n times; a test still running past the deadline gives up at its next wait
#     (exit 75, skipped), and beside another chain there is always a next wait. So
#     n <= WAIT_S / WORK_S: 1,800 / 600 gives the two chains on every preset with the default
#     work, and the linux-server presets' 7,200 s of work — the Titan Xp's renderer parts run
#     1,882 and 1,918 s alone (gate b66b) — give one, where two had renderer.2 to .4 and
#     engine_view.3 give up after their 1,800 s on gate b67 (docs/ci/what-to-run.md, "Two GPU
#     chains"). Two is the most it gives: more has not been measured.
# A whole number overrides it, and is told when it breaks the second rule.
set(_engine_gpu_lock_dir "")
if(DEFINED ENV{ENGINE_GPU_LOCK} AND NOT "$ENV{ENGINE_GPU_LOCK}" STREQUAL "")
  get_filename_component(_engine_gpu_lock_dir "$ENV{ENGINE_GPU_LOCK}" DIRECTORY)
elseif(WIN32)
  set(_engine_gpu_lock_dir "D:/workspace")
else()
  set(_engine_gpu_lock_dir "/tmp")
endif()
math(EXPR _engine_gpu_chains_fit "${ENGINE_TEST_GPU_LOCK_WAIT_S} / ${ENGINE_TEST_WORK_TIMEOUT_S}")
if(NOT IS_DIRECTORY "${_engine_gpu_lock_dir}" OR _engine_gpu_chains_fit LESS 2)
  set(_engine_gpu_chains_auto 1)
else()
  set(_engine_gpu_chains_auto 2)
endif()
# A build tree configured before T171 holds the number T89's configure derived, which nobody
# chose and which stays in the cache: read it as `auto` once. (A first configure has no cache
# file yet, so a -D given to it is kept.)
if(EXISTS "${CMAKE_BINARY_DIR}/CMakeCache.txt" AND NOT DEFINED CACHE{_ENGINE_TEST_GPU_CHAINS_AUTO_RULE})
  unset(ENGINE_TEST_GPU_CHAINS CACHE)
endif()
set(_ENGINE_TEST_GPU_CHAINS_AUTO_RULE 1 CACHE INTERNAL "ENGINE_TEST_GPU_CHAINS is auto unless set (T171)")
set(ENGINE_TEST_GPU_CHAINS auto CACHE STRING
  "GPU-labelled tests a CTest run with jobs runs at once (resource locks gpu_device.1..n), ordered by the machine-wide GPU lock: auto, or a whole number")
if(ENGINE_TEST_GPU_CHAINS STREQUAL "auto")
  set(_engine_gpu_chains ${_engine_gpu_chains_auto})
elseif(ENGINE_TEST_GPU_CHAINS MATCHES "^[1-9][0-9]*$")
  set(_engine_gpu_chains ${ENGINE_TEST_GPU_CHAINS})
  if(_engine_gpu_chains GREATER 1 AND _engine_gpu_chains GREATER _engine_gpu_chains_fit)
    message(WARNING "ENGINE_TEST_GPU_CHAINS is ${_engine_gpu_chains}, but a test with "
      "${ENGINE_TEST_WORK_TIMEOUT_S} s of work beside ${_engine_gpu_chains} chains can outrun its "
      "${ENGINE_TEST_GPU_LOCK_WAIT_S} s lock deadline and give up (exit 75) at its next wait")
  endif()
else()
  message(FATAL_ERROR "ENGINE_TEST_GPU_CHAINS is '${ENGINE_TEST_GPU_CHAINS}', not auto or a whole number of chains")
endif()

#   _engine_test_seconds(<test> <out>)
# A test's seconds as the cost table gives them (cmake/test-costs.txt): a part's, the files the
# deal gave it (_engine_test_parts); any other test's, its whole-test row (`<test> * <seconds>`).
# Empty when nobody measured it.
function(_engine_test_seconds test out)
  get_property(_part GLOBAL PROPERTY "ENGINE_TEST_PART_COST:${test}")
  get_property(_whole GLOBAL PROPERTY "ENGINE_TEST_COST:${test}:*")
  if(NOT "${_part}" STREQUAL "" AND _part GREATER 0)
    set(${out} "${_part}" PARENT_SCOPE)
  elseif(NOT "${_whole}" STREQUAL "")
    set(${out} "${_whole}" PARENT_SCOPE)
  else()
    set(${out} "" PARENT_SCOPE)
  endif()
endfunction()

#   _engine_finalize_tests()
# Deferred to the end of the top-level CMakeLists.txt, when every test of every directory exists.
# What a parallel CTest run needs, set by label and property so that a test registered anywhere —
# a module's, an app's, a tool's, one this file has never seen — is held to it:
#   - `gpu` (engine_test_waits_for_gpu: the test or a process it starts can open a device and so
#     wait for the machine-wide GPU lock) adds one GPU chain's RESOURCE_LOCK, `gpu_device.<k>`, so
#     no more than ENGINE_TEST_GPU_CHAINS such tests of one invocation run at once (above). The
#     chains are dealt by the cost table's seconds (engine_deal_gpu_chains, EngineTestDeal.cmake),
#     and tests that share another resource lock — the desktop's — are dealt as one, since they
#     run one at a time whichever chain holds them;
#   - `desktop` adds RESOURCE_LOCK `e2e_apps` (windows and input devices, one of each; engine_app):
#     an app that can open a window. A headless app's end-to-end test has `e2e` and not this;
#   - `wall_clock` (above) adds RUN_SERIAL;
#   - every test gets ENGINE_TEST_DEVICES in its environment, which it and every process it starts
#     inherit (roadmap T170; docs/ci/what-to-run.md, "Tests that open no device"): `forbidden`
#     without `gpu`, so a `gfx::Device::create` anywhere in its tree ends that process with a line
#     and a failure instead of opening a device outside the chains, and `report` with it, which the
#     test main turns into a marker file and reports, at the end of a run that passed, a test that
#     never tried to open one (foundation/gpu_lock/device_hold.h, tests/support/test_main.cpp).
# A RESOURCE_LOCK orders the tests of one `ctest` and nothing else: two invocations at once (the
# gate's suites side by side) still share the card through the lock itself (ADR-0049), and so do
# two chains of one invocation.
function(_engine_finalize_tests)
  _engine_test_directories("${CMAKE_SOURCE_DIR}" _dirs)
  set(_gpu_tests "")
  foreach(_dir IN LISTS _dirs)
    get_property(_tests DIRECTORY "${_dir}" PROPERTY TESTS)
    foreach(_test IN LISTS _tests)
      get_property(_labels TEST "${_test}" DIRECTORY "${_dir}" PROPERTY LABELS)
      get_property(_locks TEST "${_test}" DIRECTORY "${_dir}" PROPERTY RESOURCE_LOCK)
      if("desktop" IN_LIST _labels AND NOT "e2e_apps" IN_LIST _locks)
        set_property(TEST "${_test}" DIRECTORY "${_dir}" APPEND PROPERTY RESOURCE_LOCK e2e_apps)
      endif()
      if("wall_clock" IN_LIST _labels)
        set_property(TEST "${_test}" DIRECTORY "${_dir}" PROPERTY RUN_SERIAL TRUE)
      endif()
      if("gpu" IN_LIST _labels)
        list(APPEND _gpu_tests "${_test}")
        set_property(GLOBAL PROPERTY "_ENGINE_GPU_TEST_DIR:${_test}" "${_dir}")
        set_property(TEST "${_test}" DIRECTORY "${_dir}" APPEND PROPERTY ENVIRONMENT "ENGINE_TEST_DEVICES=report")
      else()
        set_property(TEST "${_test}" DIRECTORY "${_dir}" APPEND PROPERTY ENVIRONMENT "ENGINE_TEST_DEVICES=forbidden")
      endif()
    endforeach()
  endforeach()

  # One item a test, or a group of the tests that share a resource lock other than a chain's; its
  # seconds the sum of what was measured, and none when nothing of it was.
  set(_items "")
  foreach(_test IN LISTS _gpu_tests)
    get_property(_dir GLOBAL PROPERTY "_ENGINE_GPU_TEST_DIR:${_test}")
    get_property(_locks TEST "${_test}" DIRECTORY "${_dir}" PROPERTY RESOURCE_LOCK)
    list(FILTER _locks EXCLUDE REGEX "^gpu_device")
    if(_locks)
      list(SORT _locks)
      list(GET _locks 0 _group)
      set(_item "@${_group}")
    else()
      set(_item "${_test}")
    endif()
    get_property(_members GLOBAL PROPERTY "_ENGINE_GPU_ITEM:${_item}")
    if("${_members}" STREQUAL "")
      list(APPEND _items "${_item}")
      set_property(GLOBAL PROPERTY "_ENGINE_GPU_ITEM_SECONDS:${_item}" "")
    endif()
    set_property(GLOBAL APPEND PROPERTY "_ENGINE_GPU_ITEM:${_item}" "${_test}")
    _engine_test_seconds("${_test}" _s)
    if(NOT "${_s}" STREQUAL "")
      get_property(_sum GLOBAL PROPERTY "_ENGINE_GPU_ITEM_SECONDS:${_item}")
      if("${_sum}" STREQUAL "")
        set(_sum 0)
      endif()
      math(EXPR _sum "${_sum} + ${_s}")
      set_property(GLOBAL PROPERTY "_ENGINE_GPU_ITEM_SECONDS:${_item}" "${_sum}")
    endif()
  endforeach()
  if(NOT _items)
    return()
  endif()
  set(_deal_items "")
  set(_unmeasured 0)
  foreach(_item IN LISTS _items)
    get_property(_sum GLOBAL PROPERTY "_ENGINE_GPU_ITEM_SECONDS:${_item}")
    if("${_sum}" STREQUAL "")
      list(APPEND _deal_items "${_item}")
      math(EXPR _unmeasured "${_unmeasured} + 1")
    else()
      list(APPEND _deal_items "${_item}=${_sum}")
    endif()
  endforeach()
  engine_deal_gpu_chains(${_engine_gpu_chains} "${_deal_items}" _chain)   # EngineTestDeal.cmake

  math(EXPR _last "${_engine_gpu_chains} - 1")
  set(_summary "")
  foreach(_k RANGE 0 ${_last})
    math(EXPR _n "${_k} + 1")
    set(_count 0)
    foreach(_item IN LISTS _chain_${_k})
      get_property(_members GLOBAL PROPERTY "_ENGINE_GPU_ITEM:${_item}")
      foreach(_test IN LISTS _members)
        get_property(_dir GLOBAL PROPERTY "_ENGINE_GPU_TEST_DIR:${_test}")
        set_property(TEST "${_test}" DIRECTORY "${_dir}" APPEND PROPERTY RESOURCE_LOCK "gpu_device.${_n}")
        math(EXPR _count "${_count} + 1")
      endforeach()
    endforeach()
    list(APPEND _summary "gpu_device.${_n} ${_count} tests ${_chain_load_${_k}} s")
  endforeach()
  string(JOIN ", " _summary ${_summary})
  list(LENGTH _gpu_tests _total)
  message(STATUS "GPU chains: ${_engine_gpu_chains} (ENGINE_TEST_GPU_CHAINS ${ENGINE_TEST_GPU_CHAINS}) over ${_total} gpu-labelled tests (${_summary} by cmake/test-costs.txt; ${_unmeasured} unmeasured)")
endfunction()
cmake_language(DEFER CALL _engine_finalize_tests)

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

  # The slot pool script (T75) over a throwaway repository and two worktrees in a temporary directory.
  add_test(NAME tools.slot
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/slot.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.slot PROPERTIES LABELS "tools")

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

  # The Blender wrapper (docs/content-generation.md, "Blender"), through a PowerShell stand-in for
  # Blender: the refusals, the command line and environment Blender gets, and the provenance
  # sidecar a declared output earns. No Blender and no GPU.
  add_test(NAME tools.blender_run
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/blender-run.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.blender_run PROPERTIES LABELS "tools")

  # The material pack's two scripts (docs/subsystems/materials.md): packing a staging directory of
  # stand-in sets and its refusals, and fetching — the pinned hash, the manifest's file hashes, the
  # licence file, and a destination left untouched by a refused pack. No network and no image.
  add_test(NAME tools.materials_pack
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/materials-pack.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.materials_pack PROPERTIES LABELS "tools")

  # E39's measurement script (tools/hdr-measure.ps1, roadmap R80): its refusals, a dry run's plan
  # (every picture waits for a quiet machine, none lifts the present ceiling) and a run through
  # stand-ins for engine-cli and engine-view. No GPU and no window.
  add_test(NAME tools.hdr_measure
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/hdr-measure.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.hdr_measure PROPERTIES LABELS "tools")

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

  # Which tests a change can reach (tools/lib/Affected.psm1, `tools/dev.ps1 test -Affected`): the
  # rule an agent's pre-handover run rests on, tested over a module graph written in the test.
  add_test(NAME tools.affected
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/affected.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.affected PROPERTIES LABELS "tools")

  # Which Linux build volumes `tools/linux-build.ps1 -Prune -Stale` removes
  # (tools/lib/LinuxVolumes.psm1), over a machine written down in the test: the sweep deletes
  # build trees on a daemon every checkout shares, so its rule is tested as a rule. No daemon is
  # contacted.
  add_test(NAME tools.linux_build
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/linux-build.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.linux_build PROPERTIES LABELS "tools")
  # The licence check ADR-0014 promised (tools/license-check.ps1): the record in
  # third_party/LICENSES.md against the policy, against every FetchContent_Declare, and against the
  # licence files this preset's configure fetched into its _deps — so every CI job that configures
  # reads the code's own licences, not only the record's word for them.
  add_test(NAME lint.licenses
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/license-check.ps1" -Root "${CMAKE_SOURCE_DIR}"
            -BuildDir "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(lint.licenses PROPERTIES LABELS "lint")

  # And its rules, over fixture trees in a temporary directory.
  add_test(NAME tools.license_check
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/license-check.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.license_check PROPERTIES LABELS "tools")

  # The frame-budget gate's rules (tools/lib/FrameBudget.psm1, ADR-0018) over summaries written in
  # the test, and the committed budget file read: no GPU. The gate itself flies the scenes on the
  # budget's machine, at the merge (docs/ci/what-to-run.md, "Frame budgets").
  add_test(NAME tools.frame_budget
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/frame-budget.Tests.ps1"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.frame_budget PROPERTIES LABELS "tools")

  # The game author's kit (tools/export-game-sdk.ps1, tools/new-game-repo.ps1, templates/game-repo;
  # docs/game-authoring.md, roadmap P42), end to end from this build: an SDK exported into the
  # test's scratch directory, the template instantiated from it, its module built with the SDK's
  # relocatable command, run offscreen for 20 frames with a report, and packed with the SDK's
  # package-game.ps1. It runs engine-view, which takes the GPU lock itself, so it has the
  # end-to-end tests' lock, timeout and skip code; it skips by itself where the build has no
  # hotload capability (no module, no SDK) and off Windows.
  math(EXPR _game_sdk_timeout "${ENGINE_TEST_WORK_TIMEOUT_S} + ${ENGINE_TEST_GPU_LOCK_WAIT_S}")
  add_test(NAME tools.game_sdk
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/export-game-sdk.Tests.ps1" -BuildDir "${CMAKE_BINARY_DIR}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  # `gpu` too, since its engine-view takes the lock: the finalize below gives it a GPU chain's
  # resource lock with that, and `ctest -L gpu` lists it with the other tests that can wait; and
  # `desktop`, since that engine-view is engine-view, which gives it the desktop's (engine_app).
  set_tests_properties(tools.game_sdk PROPERTIES LABELS "tools;e2e;gpu;desktop"
    TIMEOUT ${_game_sdk_timeout} SKIP_RETURN_CODE 75)

  # The cost table's writer (tools/test-costs.ps1, cmake/test-costs.txt) over CTest logs and a
  # case-to-file map written in the test, the configure's deal (cmake/EngineTestDeal.cmake) over
  # fixture tables in script mode, and the prediction over a hand-written CTestTestfile: this
  # build's cmake, no build of its own and no GPU.
  add_test(NAME tools.test_costs
    COMMAND "${ENGINE_PWSH}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_SOURCE_DIR}/tools/test-costs.Tests.ps1" -Cmake "${CMAKE_COMMAND}"
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  set_tests_properties(tools.test_costs PROPERTIES LABELS "tools")
else()
  message(WARNING "pwsh not found; lint.banned_patterns, tools.lint, tools.new_capability, tools.generate, tools.e10_harness, tools.blender_run, docs_check, tools.docs_check, tools.machine_lock, tools.affected, tools.frame_budget, and tools.linux_build tests not registered")
  message(WARNING "pwsh not found; lint.banned_patterns, lint.licenses, tools.lint, tools.new_capability, tools.generate, tools.e10_harness, tools.blender_run, docs_check, tools.docs_check, tools.machine_lock, tools.affected, tools.license_check and tools.frame_budget tests not registered")
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
