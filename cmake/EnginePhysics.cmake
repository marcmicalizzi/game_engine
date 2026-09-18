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
  # Jolt puts /arch:AVX2 on the Jolt target as PUBLIC, so it would reach every target that
  # links physics. The baseline machine (docs/ci/self-hosted-runners.md: i7-980, Westmere) has
  # SSE4.2 and no AVX, LZCNT, TZCNT or F16C, and the rest of the tree compiles for the same
  # baseline. FMADD is off anyway under CROSS_PLATFORM_DETERMINISTIC.
  set(USE_SSE4_1 ON CACHE BOOL "" FORCE)
  set(USE_SSE4_2 ON CACHE BOOL "" FORCE)
  set(USE_AVX OFF CACHE BOOL "" FORCE)
  set(USE_AVX2 OFF CACHE BOOL "" FORCE)
  set(USE_AVX512 OFF CACHE BOOL "" FORCE)
  set(USE_LZCNT OFF CACHE BOOL "" FORCE)
  set(USE_TZCNT OFF CACHE BOOL "" FORCE)
  set(USE_F16C OFF CACHE BOOL "" FORCE)
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
  set(DOUBLE_PRECISION OFF CACHE BOOL "" FORCE)
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
