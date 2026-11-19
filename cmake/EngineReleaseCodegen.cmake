# How MSVC's RelWithDebInfo (the msvc-release presets) compiles and links the engine's own code
# (roadmap T193; docs/ci/what-to-run.md, "The release preset's code generation").
#
# CMake's RelWithDebInfo is `/O2 /Ob1` and links `/debug /INCREMENTAL`: inline only what is marked
# inline or defined in its class, and reach every function through an incremental-link thunk. A
# plain Release is `/O2 /Ob2` and `/INCREMENTAL:NO`, which with no `/debug` means `/OPT:REF,ICF`.
#
#   cmake   CMake's RelWithDebInfo as it is.
#   inline  `/Ob2` on the engine's own targets.
#   full    `/Ob2`, and every executable and module of the engine's linked `/INCREMENTAL:NO
#           /OPT:REF /OPT:ICF` (the default: measured, T193).
#
# Debug information stays in every setting (`/Z7` and `/debug`, and `/Zo`, which MSVC turns on
# with them in an optimized build): the sampler and Tracy need the PDBs. Only MSVC's
# RelWithDebInfo reads this: Debug, the sanitizer presets and the GCC and Clang presets are
# untouched, and so are the host tools (ADR-0034), told apart by the role engine_host_tool() gives
# them, and the third-party targets, which never pass through engine_apply_warnings() and keep
# `/Ob1` (Jolt has its own `/Ob2`, cmake/EnginePhysics.cmake; Recast measured none,
# cmake/EngineNav.cmake).
set(ENGINE_MSVC_RELEASE_CODEGEN "full" CACHE STRING
  "MSVC RelWithDebInfo code generation for the engine's own targets: cmake, inline or full")
set_property(CACHE ENGINE_MSVC_RELEASE_CODEGEN PROPERTY STRINGS cmake inline full)
if(NOT ENGINE_MSVC_RELEASE_CODEGEN MATCHES "^(cmake|inline|full)$")
  message(FATAL_ERROR "ENGINE_MSVC_RELEASE_CODEGEN is '${ENGINE_MSVC_RELEASE_CODEGEN}': cmake, inline or full")
endif()

# A target is the engine's own when engine_apply_release_codegen() marked it and it is not a host
# tool. Evaluated when the build is generated, so engine_host_tool() may come after the marking.
set(_engine_own_code
  "$<AND:$<CONFIG:RelWithDebInfo>,$<BOOL:$<TARGET_PROPERTY:ENGINE_OWN_CODE>>,$<NOT:$<STREQUAL:$<TARGET_PROPERTY:ENGINE_CPU_ROLE>,host_tool>>>")

if(MSVC AND NOT ENGINE_MSVC_RELEASE_CODEGEN STREQUAL "cmake")
  # The inlining level leaves the configuration's flags and is given per target instead, `/Ob2`
  # to the engine's own and the `/Ob1` they had to everything else, so no command line carries
  # both (cl's D9025 on every file). Before any target exists: a directory's options reach only
  # the targets created after them, and every target here, fetched ones too, is created below.
  # C++ only: C is enabled later, by the third-party projects that use it, and every C target is
  # third party, so its flags keep their `/Ob1` (the second one added below changes nothing).
  string(REPLACE "/Ob1" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
  add_compile_options("$<$<CONFIG:RelWithDebInfo>:$<IF:${_engine_own_code},/Ob2,/Ob1>>")
endif()

# Called for every engine target by engine_apply_warnings(), the one function every module, test,
# bench, app and game module passes through, so no module opts in on its own.
function(engine_apply_release_codegen target)
  if(NOT MSVC OR ENGINE_MSVC_RELEASE_CODEGEN STREQUAL "cmake")
    return()
  endif()
  set_property(TARGET ${target} PROPERTY ENGINE_OWN_CODE ON)
  if(ENGINE_MSVC_RELEASE_CODEGEN STREQUAL "full")
    get_target_property(_type ${target} TYPE)
    if(_type MATCHES "^(EXECUTABLE|SHARED_LIBRARY|MODULE_LIBRARY)$")
      # After CMake's `/debug /INCREMENTAL` on the link line, so these win; `/debug` alone would
      # leave `/OPT:NOREF,NOICF`, which is why both are named.
      target_link_options(${target} PRIVATE "$<${_engine_own_code}:/INCREMENTAL:NO;/OPT:REF;/OPT:ICF>")
    endif()
  endif()
endfunction()
