# Luau (MIT, docs/plan/08-toolchain.md §8.2 and §8.6, experiment E7): the typed Lua the scripting
# capability embeds (foundation/scripting). Fetched only when that capability is on, so a minimal
# build (ADR-0027) neither downloads nor compiles it.
#
# Luau's CMake declares every library it has (the VM, the compiler, the type checker, the native
# code generator, the CLI helpers) whether anyone links it or not. EXCLUDE_FROM_ALL keeps what
# nothing here links out of the build: the engine library links the VM and the compiler, and only
# the scripting tests link the analysis library, because that is where the type checker runs
# (docs/subsystems/scripting.md, "The typed API"). The native code generator (Luau.CodeGen) is
# never linked: it writes executable memory, it is x64/arm64-specific, and interpreting is fast
# enough for content logic — E7 measures by how much.
#
# SYSTEM makes every Luau target's include directories system directories for whoever links it,
# so the engine's -Werror never judges Luau's headers, and the per-target flags below keep our
# warning policy off Luau's own sources. Neither reaches the engine's code.
#
# The one definition that changes behaviour is LUA_USE_LONGJMP=1. By default Luau raises a script
# error by throwing a C++ exception through whatever C++ frames are between the raise and the
# protected call, and the engine's own frames there — every host function a script calls, the
# interrupt that enforces the step budget, the allocator that enforces the memory limit — are
# compiled with -fno-exceptions on GCC and Clang (cmake/EngineOptions.cmake). longjmp is what Luau
# itself uses when its API is extern "C" (LUAU_EXTERN_C), so it is a configuration upstream
# builds and tests. The cost is a rule for host code: a function a script can call holds nothing
# with a destructor at any point where it can raise, because longjmp runs none. The compiler and
# the analysis library keep their exceptions: they throw and catch internally and never across
# an engine frame.

include_guard(GLOBAL)
include(FetchContent)

# 0.739 (2026-09-18) is commit a62362a53ddc9c629b0e29378a84abb4534d8b64.
set(ENGINE_LUAU_TAG "0.739" CACHE STRING "Luau release tag")

function(engine_fetch_luau)
  if(TARGET Luau.VM)
    return()
  endif()

  # Luau's options are declared with option(), so a stale cache entry would win without FORCE.
  # None of the CLI tools, tests or the web build are wanted: the type checker the engine needs is
  # a library, and the upstream `luau-analyze` cannot load a definition file anyway.
  set(LUAU_BUILD_CLI OFF CACHE BOOL "" FORCE)
  set(LUAU_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LUAU_BUILD_WEB OFF CACHE BOOL "" FORCE)
  set(LUAU_WERROR OFF CACHE BOOL "" FORCE)
  # /MT against the engine's /MD is two heaps and LNK2038 at best.
  set(LUAU_STATIC_CRT OFF CACHE BOOL "" FORCE)
  # extern "C" would also switch longjmp on, but only on the VM's and compiler's *declarations*;
  # the engine compiles lua.h as C++ and wants the C++ names. LUA_USE_LONGJMP is set below instead.
  set(LUAU_EXTERN_C OFF CACHE BOOL "" FORCE)
  set(LUAU_BUILD_SHARED OFF CACHE BOOL "" FORCE)

  FetchContent_Declare(luau
    GIT_REPOSITORY https://github.com/luau-lang/luau.git
    GIT_TAG        ${ENGINE_LUAU_TAG}
    GIT_SHALLOW    TRUE
    EXCLUDE_FROM_ALL
    SYSTEM)
  FetchContent_MakeAvailable(luau)

  target_compile_definitions(Luau.VM PUBLIC LUA_USE_LONGJMP=1)

  foreach(_target Luau.Common Luau.Ast Luau.Bytecode Luau.Compiler Luau.Config Luau.Analysis
                  Luau.VM Luau.CodeGen Luau.Inliner Luau.Require Luau.CLI.lib isocline)
    if(TARGET ${_target})
      set_target_properties(${_target} PROPERTIES FOLDER "third_party")
      get_target_property(_type ${_target} TYPE)
      if(NOT _type STREQUAL "INTERFACE_LIBRARY")
        # Third-party code is judged by its own policy, not ours.
        if(MSVC)
          target_compile_options(${_target} PRIVATE /W0)
        else()
          target_compile_options(${_target} PRIVATE -w)
        endif()
      endif()
    endif()
  endforeach()
endfunction()
