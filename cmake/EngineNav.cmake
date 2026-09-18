# Recast/Detour (zlib, docs/plan/08-toolchain.md §8.6, plan 05 §5.11). Fetched only when the nav
# capability is on, so a minimal build (ADR-0027) does not download or compile it at all.
#
# Recast's CMake is written to be a standalone project with a demo application: the defaults
# build an SDL2/OpenGL demo, a Catch2 test binary that registers itself with *our* CTest, and
# install/export rules for a package nobody installs. Every option below exists because the
# default breaks something here; the reasons are in docs/subsystems/nav.md so that the next
# person does not have to rediscover them. Options are cached FORCE because Recast declares them
# with option(), and DT_POLYREF64 in particular is a PUBLIC compile definition that changes the
# width of every path element — a stale cache entry would silently change an ABI.

include_guard(GLOBAL)
include(FetchContent)

set(ENGINE_RECAST_TAG "v1.6.0" CACHE STRING "recastnavigation tag")

function(engine_fetch_recast)
  if(TARGET Recast)
    return()
  endif()

  # --- targets we do not want built at all --------------------------------------------------
  # The demo links SDL2 and OpenGL and would make the nav capability unbuildable on any machine
  # without them; the engine has its own window layer (foundation/window).
  set(RECASTNAVIGATION_DEMO OFF CACHE BOOL "" FORCE)
  set(RECASTNAVIGATION_EXAMPLES OFF CACHE BOOL "" FORCE)
  # Recast's own tests call enable_testing() and add_test(Tests Tests) in a subdirectory of our
  # build, so a `ctest` run of the engine would try to run them; they also compile a vendored
  # Catch2 amalgamation beside our doctest. Their upstream coverage is not ours to run.
  set(RECASTNAVIGATION_TESTS OFF CACHE BOOL "" FORCE)

  # --- Detour's two ABI switches ------------------------------------------------------------
  # DT_POLYREF64 widens dtPolyRef to 64 bits. Left off: a 32-bit ref is salt|tile|poly, and with
  # NavMeshOptions' defaults (1024 tiles, 4096 polys a tile) that is 10 + 12 bits with 10 bits of
  # salt left, which Detour requires. Paths and the A* node pool are half the size this way. The
  # limit is visible in NavMeshOptions (ADR-0017) and this is the switch to flip when a world
  # needs more resident tiles than the budget allows.
  set(RECASTNAVIGATION_DT_POLYREF64 OFF CACHE BOOL "" FORCE)
  # DT_VIRTUAL_QUERYFILTER makes dtQueryFilter's passFilter/getCost virtual. That is an indirect
  # call per visited polygon inside A* (plan 11 §11.4); the engine's filter is the stock one with
  # different flags and costs, which needs no subclass.
  set(RECASTNAVIGATION_DT_VIRTUAL_QUERYFILTER OFF CACHE BOOL "" FORCE)

  # The next two are *CMake's* variables, not Recast's, so they are set as plain variables in this
  # function's scope — which add_subdirectory() inherits — and never forced into the cache. A
  # capability may not change how the rest of the tree configures (ADR-0027 decision 3), and a
  # cached BUILD_SHARED_LIBS would outlive this call and apply to every later configure.
  #
  # Recast has no static/shared switch of its own; it follows BUILD_SHARED_LIBS. Static, like
  # everything else here, so there is no DLL to place beside the test binaries.
  set(BUILD_SHARED_LIBS OFF)

  # Recast 1.6.0 declares cmake_minimum_required(VERSION 3.1). CMake 3.31 deprecates anything
  # below 3.5 and CMake 4 refuses it outright; this variable is how a consumer says "read it as
  # 3.5" without patching the dependency. Ignored by older CMake, which accepts 3.1 anyway.
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

  # EXCLUDE_FROM_ALL keeps DebugUtils and DetourTileCache — which nothing here links — out of the
  # build, and keeps Recast's install() rules out of ours.
  FetchContent_Declare(recastnavigation
    GIT_REPOSITORY https://github.com/recastnavigation/recastnavigation.git
    GIT_TAG        ${ENGINE_RECAST_TAG}
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(recastnavigation)

  foreach(_target Recast Detour DetourCrowd)
    if(TARGET ${_target})
      set_target_properties(${_target} PROPERTIES FOLDER "third_party")
      # Recast is C++98-era code compiled with our flags. Its warnings are not ours to fix and
      # -Werror is on for the engine, so the one thing we must not do is judge it by our policy.
      if(MSVC)
        target_compile_options(${_target} PRIVATE /W0)
        # Deliberately *not* the /Ob2 that domain/physics has to give Jolt in RelWithDebInfo.
        # The same suspicion applied — CMake's RelWithDebInfo uses /Ob1, "inline only what is
        # marked inline", and a voxel pipeline is small helpers all the way down — but measured
        # both ways on the bench scene it is worth nothing: 7.35 / 32.9 / 135.8 ms with /Ob1
        # against 7.39 / 32.7 / 135.8 ms with /Ob2 for 32, 64 and 128 m tiles. Recast marks its
        # own hot helpers `inline` in the headers, so /Ob1 already has them. The flag is left off
        # rather than kept "just in case", because an option nobody can justify is one the next
        # person has to re-measure.
      else()
        target_compile_options(${_target} PRIVATE -w)
      endif()
    endif()
  endforeach()
endfunction()
