# Jolt Physics (MIT, docs/plan/08-toolchain.md, plan 05 §5.11). Fetched only when the physics
# capability is on, so a minimal build (ADR-0027) does not download or compile it at all.
#
# Jolt's CMake is written to be the top-level project: it rewrites CMAKE_CXX_FLAGS, picks a
# runtime library, turns on AVX2, LTO, a debug renderer, and a profiler, and disables RTTI and
# exceptions. Every option set below exists because the default breaks something here; the
# reasons are in docs/subsystems/physics.md so that the next person does not have to rediscover
# them. Options are cached FORCE because Jolt declares them with option() and a stale cache
# entry would silently change the ABI of a library whose JPH_* defines have to match ours.

include_guard(GLOBAL)
include(FetchContent)

set(ENGINE_JOLT_TAG "v5.6.0" CACHE STRING "Jolt Physics tag")

function(engine_fetch_jolt)
  if(TARGET Jolt)
    return()
  endif()

  # --- flags that would otherwise leak out of Jolt's directory ---------------------------
  # Jolt replaces CMAKE_CXX_FLAGS_DEBUG/RELEASE wholesale, which throws away the preset's
  # flags (and, in Debug, the iterator checking the rest of the tree is built with).
  set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
  # /MT vs the engine's /MD is an ABI mismatch: the two runtimes have separate heaps and the
  # linker reports it as LNK2038 at best.
  set(USE_STATIC_MSVC_RUNTIME_LIBRARY OFF CACHE BOOL "" FORCE)
  # Jolt's own -Wall -Werror judges Jolt's code with our policy; its headers are SYSTEM here.
  set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
  # CMake's own per-config flags already carry debug info for every preset we build; Jolt's
  # extra /Zi (and its /DEBUG exe-linker flag) only duplicate them.
  set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
  # LTO on one static library in a tree that does not use it produces object files the
  # archiver of the other half cannot read.
  set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
  # RTTI stays on everywhere (AGENTS.md): our JPH::JobSystemWithBarrier subclass is compiled
  # with RTTI, and under the Itanium ABI a polymorphic base compiled with -fno-rtti has no
  # typeinfo symbol for the derived class to reference.
  set(CPP_RTTI_ENABLED ON CACHE BOOL "" FORCE)
  set(CPP_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)

  # --- determinism ------------------------------------------------------------------------
  # Cross-platform determinism costs about 8% (plan 05 §5.10) and is a compile-time property:
  # /fp:precise, -ffp-contract=off, no FMA. Paying it now keeps replay and lockstep open
  # (ADR-0016) instead of leaving a switch that changes every recorded hash the day it flips.
  set(CROSS_PLATFORM_DETERMINISTIC ON CACHE BOOL "" FORCE)

  # --- instruction set --------------------------------------------------------------------
  # Jolt is the one dependency here that publishes an arch flag: it puts /arch:AVX2 (or the
  # -m flags) on the Jolt target as **PUBLIC**, so whatever it decides reaches every target
  # that links physics. So it does not decide. These options follow ENGINE_CPU_BASELINE
  # (cmake/EngineCpuBaseline.cmake, ADR-0031), which is also what the rest of the tree is
  # compiled with, and the JPH_USE_* defines Jolt's headers change shape with therefore agree
  # with the flags the library was built with — which is the thing physics.md's "Why nothing
  # from Jolt is public" says silently goes wrong when it does not.
  #
  # Until 2026-09-19 every one of these was OFF, because the minimum machine was an i7-980
  # (Westmere: SSE4.2, no AVX). The minimum is now x86-64-v3 and the v2 build is what that one
  # machine gets; ADR-0031 has the measurements, including what this change bought physics.
  set(USE_SSE4_1 ON CACHE BOOL "" FORCE)
  set(USE_SSE4_2 ON CACHE BOOL "" FORCE)
  if(ENGINE_CPU_BASELINE STREQUAL "v3")
    set(_engine_jolt_v3 ON)
  else()
    set(_engine_jolt_v3 OFF)
  endif()
  set(USE_AVX ${_engine_jolt_v3} CACHE BOOL "" FORCE)
  set(USE_AVX2 ${_engine_jolt_v3} CACHE BOOL "" FORCE)
  set(USE_LZCNT ${_engine_jolt_v3} CACHE BOOL "" FORCE)
  set(USE_F16C ${_engine_jolt_v3} CACHE BOOL "" FORCE)
  # TZCNT is BMI1, which x86-64-v3 has, so it follows the baseline like the rest.
  set(USE_TZCNT ${_engine_jolt_v3} CACHE BOOL "" FORCE)
  # AVX-512 is never a baseline (ADR-0031): dispatch-only, and Jolt has no dispatch.
  set(USE_AVX512 OFF CACHE BOOL "" FORCE)
  # FMADD stays off at every baseline. Jolt itself ignores it under CROSS_PLATFORM_DETERMINISTIC
  # — contraction is what determinism costs — so turning it on with the baseline would be a
  # switch that reads as if it did something and does not.
  set(USE_FMADD OFF CACHE BOOL "" FORCE)

  # --- things we do not use -----------------------------------------------------------------
  # Asserts in Debug only: Jolt's default is off everywhere, and an assert that never fires in
  # the configuration developers run is not worth its cost in the one they ship.
  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(USE_ASSERTS ON CACHE BOOL "" FORCE)
  else()
    set(USE_ASSERTS OFF CACHE BOOL "" FORCE)
  endif()
  # The debug renderer and the profiler cost time in every build that has them compiled in;
  # the engine profiles with Tracy (ENGINE_TRACY) and draws with its own renderer.
  set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
  set(DEBUG_RENDERER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
  set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
  set(PROFILER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
  # Floating-point exceptions also switch on Vec3's "keep W equal to Z" bookkeeping in every
  # vector operation; the engine validates inputs instead (AGENTS.md) and pays neither.
  set(FLOATING_POINT_EXCEPTIONS_ENABLED OFF CACHE BOOL "" FORCE)
  # Jolt 5.6 ships a GPU hair solver whose shaders are compiled with dxc at build time. The
  # engine has its own shader pipeline (ADR-0008) and does not use Jolt's, and requiring dxc or
  # the Vulkan SDK to build physics would break every machine that has neither.
  set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
  set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
  set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
  set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
  # Jolt's text/binary object stream is for its own sample assets; the engine serializes
  # through schemas (ADR-0007).
  set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
  set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

  # --- precision ------------------------------------------------------------------------------
  # A body's position, a query's origin and a character's feet are f64 (ADR-0053): at 420 km a
  # float steps by 3.1 cm and a walker built single-precision lost motion there
  # (docs/experiments/far-from-origin-2026-10-04.md). Jolt's double mode keeps positions in double
  # and does its collision arithmetic in float relative to a base near the bodies involved, which
  # is the engine's own rule for local frames. It is ON for every preset and both baselines, and
  # there is no option to turn it off: two precisions would be two simulations, and every hash a
  # replay records would depend on which one a build chose. src/jolt.h refuses to compile without
  # it. What it costs is measured in docs/experiments/world-positions-physics-2026-10-05.md.
  set(DOUBLE_PRECISION ON CACHE BOOL "" FORCE)
  set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
  set(TARGET_UNIT_TESTS OFF CACHE BOOL "" FORCE)
  set(TARGET_HELLO_WORLD OFF CACHE BOOL "" FORCE)
  set(TARGET_PERFORMANCE_TEST OFF CACHE BOOL "" FORCE)
  set(TARGET_SAMPLES OFF CACHE BOOL "" FORCE)
  set(TARGET_VIEWER OFF CACHE BOOL "" FORCE)

  # The CMake project lives in Build/, not at the repository root.
  FetchContent_Declare(jolt
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG        ${ENGINE_JOLT_TAG}
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  Build)
  FetchContent_MakeAvailable(jolt)

  # With OVERRIDE_CXX_FLAGS off, Jolt is compiled with the preset's flags, and CMake's
  # RelWithDebInfo uses /Ob1 — inline only what is marked inline. Jolt's hot loops are built out
  # of small unmarked helpers, so that is not a small loss: measured on msvc-release, a step of
  # the 1000-box bench with no job system cost 6.70 ms with /Ob1 and 2.43 ms with /Ob2. Restore
  # the inlining level on this target alone rather than changing the preset for the whole tree.
  if(MSVC)
    target_compile_options(Jolt PRIVATE $<$<CONFIG:RelWithDebInfo>:/Ob2>)
  endif()

  set_target_properties(Jolt PROPERTIES FOLDER "third_party")
endfunction()
