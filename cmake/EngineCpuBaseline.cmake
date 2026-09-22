# The CPU this tree compiles for (ADR-0031, docs/plan/04-renderer.md §4.1).
#
#   -DENGINE_CPU_BASELINE=v3   (default)  x86-64-v3: AVX2, FMA, BMI1/2, F16C, LZCNT, MOVBE.
#                                         Haswell (2013) and Zen and newer.
#   -DENGINE_CPU_BASELINE=v2              x86-64-v2: SSE3..SSE4.2 and POPCNT. Nehalem/Westmere.
#
# **One place, on purpose.** An instruction-set baseline applied per target is one a new target
# quietly gets wrong, and the way it goes wrong — a binary that runs everywhere the author
# tested and traps on the one machine that matters — is not visible in a build log. So the flag
# is added to the top-level directory's COMPILE_OPTIONS before anything is fetched or added, and
# every target in the tree inherits it: the engine's modules, its apps, its tests and benches,
# and every third-party library built from source here (Jolt, flecs, SQLite, SDL3, meshoptimizer,
# Recast, Tracy, stb through our own translation unit). A dependency that publishes an arch flag
# of its own is made to follow this one rather than argue with it; Jolt is the only one, and
# cmake/EnginePhysics.cmake is where its USE_AVX*/USE_SSE4_* options are set from these
# variables. docs/plan/08-toolchain.md §8.9 has the table of what each dependency does. Two kinds
# of target give the flag back, and only two, both at the bottom of this file: `core/platform`,
# which has to run on the CPU it is refusing (ADR-0031), and the **host tools** the build itself
# runs, which never ship and have to run on whatever machine is building (ADR-0034).
#
# AVX-512 is never a baseline. It is dispatch-only (ADR-0031): the parts that have it are a
# minority, the frequency behaviour differs between them, and a kernel that wants it selects it
# at run time from platform::cpu_features() (plan 11 §11.4).
#
# **What is deliberately not here:** -march=native, -ffast-math, and anything else that makes a
# binary a property of the machine that built it. The baseline is a promise to users, so it is
# named, not detected.

include_guard(GLOBAL)

set(ENGINE_CPU_BASELINE "v3" CACHE STRING "CPU instruction-set baseline: v3 (x86-64-v3) or v2")
set_property(CACHE ENGINE_CPU_BASELINE PROPERTY STRINGS v3 v2)

if(NOT ENGINE_CPU_BASELINE STREQUAL "v3" AND NOT ENGINE_CPU_BASELINE STREQUAL "v2")
  message(FATAL_ERROR
    "ENGINE_CPU_BASELINE is '${ENGINE_CPU_BASELINE}'; it is 'v3' (the default, x86-64-v3) or "
    "'v2' (x86-64-v2, which exists for one 2010 machine — see ADR-0031).")
endif()

# The flags, and the one x86-64 assumption in the file. CMAKE_SYSTEM_PROCESSOR is AMD64 on
# Windows and x86_64 elsewhere; on anything else there is no x86 baseline to state and the
# build carries none, which is the right answer rather than a guess at an ARM equivalent.
set(ENGINE_CPU_BASELINE_FLAGS "")
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^([Aa][Mm][Dd]64|x86_64|x64)$")
  if(MSVC)
    # Covers cl and clang-cl. /arch:AVX2 is MSVC's whole x86-64-v3: it enables AVX, AVX2, FMA,
    # BMI1/2, LZCNT and F16C together. There is no /arch for v2 — the x64 default already
    # guarantees SSE2, and everything between SSE3 and SSE4.2 is reached through intrinsics
    # that MSVC compiles without an arch flag — so the v2 build carries no flag at all.
    if(ENGINE_CPU_BASELINE STREQUAL "v3")
      set(ENGINE_CPU_BASELINE_FLAGS /arch:AVX2)
    endif()
  else()
    # GCC and Clang have the microarchitecture levels by name, which is exactly what is wanted:
    # the promise is "x86-64-v3", not a list of -m flags that drifts from it.
    #
    # UNTESTED as this lands: hosted CI (the only GCC and Clang this project has) was
    # unavailable, so these two lines have never been compiled. They are written from the
    # documented spelling, which GCC has had since 11 and Clang since 12.
    if(ENGINE_CPU_BASELINE STREQUAL "v3")
      set(ENGINE_CPU_BASELINE_FLAGS -march=x86-64-v3)
    else()
      set(ENGINE_CPU_BASELINE_FLAGS -march=x86-64-v2)
    endif()
  endif()
else()
  message(STATUS "engine CPU baseline: ${CMAKE_SYSTEM_PROCESSOR} is not x86-64; no arch flag")
endif()

if(ENGINE_CPU_BASELINE STREQUAL "v3")
  add_compile_definitions(ENGINE_CPU_BASELINE_V3=1)
else()
  add_compile_definitions(ENGINE_CPU_BASELINE_V3=0)
endif()

if(ENGINE_CPU_BASELINE_FLAGS)
  add_compile_options(${ENGINE_CPU_BASELINE_FLAGS})
endif()

message(STATUS "engine CPU baseline: ${ENGINE_CPU_BASELINE} (${ENGINE_CPU_BASELINE_FLAGS})")

# --- who carries the baseline, and who does not (ADR-0034) --------------------------------------
#
# **Shipped targets carry it; host tools do not.** A *shipped* target is anything that runs on a
# user's machine or stands in for one: every engine module, app, test and bench, and every
# third-party library they link. A *host tool* is anything the **build** runs on the build machine
# — today `tools/schemac`, which generates the schema headers — and it never ships.
#
# The two want opposite things. A shipped target's instruction set is a promise to users and has to
# be the baseline everywhere, which is what the top-level flag above does. A host tool's speed is
# irrelevant — schemac spends milliseconds a build — and its portability is the whole point: it has
# to run on whatever machine is building, including one below the product's baseline. Compiling it
# for x86-64-v3 is what made a v3 build on the Sandy Bridge GPU server die six minutes in with
# SIGILL and nothing else (`[126/976] schemac: schemas`, ninja's code 260), because a
# standard-library-only tool links no `core/platform` and has no `require_cpu_baseline()` to say
# so. So `engine_host_tool()` takes the flag back off, and a host tool is compiled for the
# compiler's own default — the build machine's platform promise — whatever ENGINE_CPU_BASELINE is.
# A v3 build on a v2 machine then builds to the end, and fails where it should: at the first engine
# binary that runs, with ADR-0031's one line and exit 78.
#
# That replaced a configure-time refusal (a native v3 configure on a machine whose /proc/cpuinfo
# lacked avx2 stopped with a message), which was the right stop-gap and the wrong rule: it forbade
# building v3 on a v2 machine at all, when the only thing that could not work was the host tool.
#
# **The classification is checked, not remembered.** `build.cpu_baseline`
# (cmake/CheckCpuBaseline.cmake, registered by engine_cpu_baseline_finalize() below) reads
# `compile_commands.json` and `build.ninja` after every build and fails if a host tool's
# translation unit carries any instruction-set flag, if any other translation unit lacks the
# baseline, or if the build runs an executable of its own that is not declared a host tool — so a
# second generator tool that forgets `engine_host_tool()` is caught by the graph, not by a server
# six minutes into a build.

# Removes the baseline flags from one target's COMPILE_OPTIONS. It removes rather than appending an
# opposite flag, because "the last /arch or -march wins" is true but is not the kind of thing a
# build should rest on — and MSVC has no /arch that means "x86-64-v2" to append in the first
# place. A target's COMPILE_OPTIONS property is initialized from the directory's when the target is
# created, so by the time either caller below runs, the flag is sitting in a list this can edit.
function(_engine_remove_cpu_baseline_flags target)
  if(NOT ENGINE_CPU_BASELINE_FLAGS)
    return()
  endif()
  get_target_property(_opts ${target} COMPILE_OPTIONS)
  if(NOT _opts)
    set(_opts "")
  endif()
  foreach(_flag IN LISTS ENGINE_CPU_BASELINE_FLAGS)
    list(REMOVE_ITEM _opts "${_flag}")
  endforeach()
  set_target_properties(${target} PROPERTIES COMPILE_OPTIONS "${_opts}")
endfunction()

# A **shipped** target that has to run on a CPU the rest of its binary cannot: `core/platform`,
# which decides whether the rest of the binary may run at all (core/platform/src/cpu_baseline.cpp).
# ADR-0031's one exemption inside the product, and it stays one: a second caller would be a module
# quietly opting out of the baseline, which ADR-0031 forbids. `build.cpu_baseline` names every
# target that calls this, so a second one shows up in a test log rather than in a code review.
function(engine_strip_cpu_baseline target)
  _engine_remove_cpu_baseline_flags(${target})
  set_property(TARGET ${target} PROPERTY ENGINE_CPU_ROLE floor)
  set_property(GLOBAL APPEND PROPERTY ENGINE_CPU_FLOOR_TARGETS ${target})
endfunction()

# A **host tool**: an executable the build runs on the build machine, which never ships (ADR-0034).
# It is compiled with no instruction-set flag from ENGINE_CPU_BASELINE, so it runs on any machine
# that can run the compiler that built it. Declare every executable an add_custom_command() runs
# this way, right after its add_executable(); `build.cpu_baseline` fails a build that runs one of
# its own executables without it.
function(engine_host_tool target)
  _engine_remove_cpu_baseline_flags(${target})
  set_property(TARGET ${target} PROPERTY ENGINE_CPU_ROLE host_tool)
  set_property(GLOBAL APPEND PROPERTY ENGINE_HOST_TOOL_TARGETS ${target})
endfunction()

# Registers `build.cpu_baseline` and its self-test. Called once, at the end of the top-level
# CMakeLists.txt, when every target that will ever call the two functions above has done so.
#
# Ninja only, because the check reads two files only a Ninja build writes in the form it parses
# (`compile_commands.json` is Makefile-and-Ninja; `build.ninja` is Ninja's own), and every preset
# in CMakePresets.json uses Ninja. Another generator configures fine and simply has no such test.
function(engine_cpu_baseline_finalize)
  if(NOT ENGINE_BUILD_TESTS)
    return()
  endif()
  get_property(_host GLOBAL PROPERTY ENGINE_HOST_TOOL_TARGETS)
  get_property(_floor GLOBAL PROPERTY ENGINE_CPU_FLOOR_TARGETS)
  string(JOIN "|" _host_arg ${_host})
  string(JOIN "|" _floor_arg ${_floor})
  string(JOIN "|" _flags_arg ${ENGINE_CPU_BASELINE_FLAGS})
  # The self-test runs the classification over synthetic input with a known answer, so a check
  # that stopped matching anything would fail here rather than pass everywhere quietly.
  add_test(NAME build.cpu_baseline.self_test
    COMMAND "${CMAKE_COMMAND}" -DSELF_TEST=ON -P "${CMAKE_SOURCE_DIR}/cmake/CheckCpuBaseline.cmake")
  set_tests_properties(build.cpu_baseline.self_test PROPERTIES LABELS "build")
  if(NOT CMAKE_GENERATOR MATCHES "Ninja" OR NOT CMAKE_EXPORT_COMPILE_COMMANDS)
    message(STATUS "engine CPU baseline: build.cpu_baseline needs Ninja and compile_commands.json; not registered")
    return()
  endif()
  add_test(NAME build.cpu_baseline
    COMMAND "${CMAKE_COMMAND}" "-DBINARY_DIR=${CMAKE_BINARY_DIR}" "-DBASELINE=${ENGINE_CPU_BASELINE}"
            "-DFLAGS=${_flags_arg}" "-DHOST_TOOLS=${_host_arg}" "-DFLOOR=${_floor_arg}"
            -P "${CMAKE_SOURCE_DIR}/cmake/CheckCpuBaseline.cmake")
  set_tests_properties(build.cpu_baseline PROPERTIES LABELS "build")
  message(STATUS "engine CPU baseline: host tools [${_host}], floor [${_floor}]")
endfunction()
