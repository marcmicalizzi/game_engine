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
# variables. docs/plan/08-toolchain.md §8.9 has the table of what each dependency does.
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

# Takes the baseline flag back off one target, for code that has to be able to run on a CPU the
# rest of the binary cannot: `core/platform`, which is what decides whether the rest of the
# binary may run at all (core/platform/src/cpu_baseline.cpp).
#
# It removes the flag rather than appending an opposite one, because "the last /arch or -march
# wins" is true but is not the kind of thing a build should rest on — and MSVC has no /arch that
# means "x86-64-v2" to append in the first place. A target's COMPILE_OPTIONS property is
# initialized from the directory's when the target is created, so by the time this is called the
# flag is sitting in a list this function can edit. Verify it in
# `build/<preset>/compile_commands.json`, which is the artefact the claim rests on.
function(engine_strip_cpu_baseline target)
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
