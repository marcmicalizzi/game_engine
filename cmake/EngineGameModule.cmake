# Hot-reloadable C++ game modules (foundation/hotload, docs/subsystems/hotload.md; E7's C++ arm).
#
#   engine_game_module(NAME <name> SOURCES <file>... [SCHEMAS <file.schema>...]
#                      [OUTPUT_DIRECTORY <dir>])
#
# Builds engine_game_<name>, a shared library (`<name>.dll` / `<name>.so`, MODULE: loaded at run
# time, never linked) that a `hotload::ModuleHost` loads, watches and reloads. What makes it a game
# module rather than another engine library:
#
#   - **It links no engine library.** Its include path is the engine's headers (core/ and
#     foundation/hotload's module ABI) and nothing on its link line is ours: everything it needs
#     from the engine comes through the `HostApi` table the host hands it. A second copy of the log
#     categories, the tunables or the allocator registries inside a DLL would be a second set of
#     globals the engine does not know about. On Linux `-z defs` makes "it links nothing" a link
#     error rather than a promise, as an unresolved symbol already is for an MSVC DLL.
#   - **Unity build, always:** one translation unit, so the edit-to-running time is one compile
#     and one link (E7 measures it: `rebuild.200_lines`).
#   - **Hidden visibility:** the one exported symbol is `engine_hotload_describe`
#     (ENGINE_HOTLOAD_DESCRIBE in module_abi.h); nothing else can interpose on, or be interposed by,
#     another library.
#   - **SCHEMAS** are the module's state type: schemac runs over them and the generated tables are
#     compiled in with ENGINE_SCHEMA_NO_REGISTRAR, so the module carries its own descriptor (which
#     changes with every build that changes the schema) and registers nothing.
#   - It compiles with the engine's flags (the CPU baseline, floating-point contraction off,
#     warnings as errors, no exceptions), because it is the engine's code running in the engine.
#
# When the hotload capability is off (ENGINE_WITH_HOTLOAD=OFF, ENGINE_MINIMAL) this does nothing.
#
# The helper also exports the **command** that builds a game module from a directory of sources
# outside the build tree — the same compiler, the same flags, schemac — for the tests, which have
# to compile a second build of a module into their own scratch directory:
#
#   cmake -DCONFIG=<ENGINE_GAME_MODULE_BUILD_CONFIG> -DSOURCE_DIR=<dir> -DOUTPUT=<file>
#         -P <repo>/cmake/EngineGameModuleBuild.cmake
#
# `engine_game_module_build_command(<target>)` gives a target the three compile definitions that
# spell it (ENGINE_CMAKE_COMMAND, ENGINE_GAME_MODULE_BUILD_CONFIG, ENGINE_GAME_MODULE_BUILD_SCRIPT).

set(ENGINE_GAME_MODULE_BUILD_CONFIG "${CMAKE_BINARY_DIR}/game_modules/build_config.cmake")
set(ENGINE_GAME_MODULE_BUILD_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/EngineGameModuleBuild.cmake")
set_property(GLOBAL PROPERTY ENGINE_GAME_MODULE_CONFIG_WRITTEN FALSE)

# The include path of a game module: every core module's public headers (header-only use only: a
# module that calls a function defined in a core library fails to link), and the module ABI.
function(_engine_game_module_includes out_var)
  file(GLOB _core_includes LIST_DIRECTORIES TRUE "${CMAKE_SOURCE_DIR}/core/*/include")
  set(_dirs "")
  foreach(_d IN LISTS _core_includes)
    if(IS_DIRECTORY "${_d}")
      list(APPEND _dirs "${_d}")
    endif()
  endforeach()
  list(APPEND _dirs "${CMAKE_SOURCE_DIR}/foundation/hotload/include")
  set(${out_var} "${_dirs}" PARENT_SCOPE)
endfunction()

# Writes the exported build command's configuration from the first game module target, whose
# compile options are the directory's (the CPU baseline, sanitizers) plus the warnings.
function(_engine_game_module_write_config target)
  get_property(_written GLOBAL PROPERTY ENGINE_GAME_MODULE_CONFIG_WRITTEN)
  if(_written)
    return()
  endif()
  set_property(GLOBAL PROPERTY ENGINE_GAME_MODULE_CONFIG_WRITTEN TRUE)

  string(TOUPPER "${CMAKE_BUILD_TYPE}" _cfg)
  separate_arguments(_flags NATIVE_COMMAND "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${_cfg}}")
  set(_msvc FALSE)
  if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC" OR MSVC)
    set(_msvc TRUE)
    # Debug information in the object (/Z7) rather than through mspdbsrv (/Zi): a test's compile
    # should not leave a PDB server behind it.
    list(TRANSFORM _flags REPLACE "^[/-]Zi$" "/Z7")
    if(_cfg STREQUAL "DEBUG")
      list(APPEND _flags /MDd)
    else()
      list(APPEND _flags /MD)
    endif()
  else()
    list(APPEND _flags -fPIC -fvisibility=hidden -fvisibility-inlines-hidden -ffp-contract=off)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
      list(APPEND _flags -fno-gnu-unique)
    endif()
  endif()
  list(APPEND _flags ${CMAKE_CXX20_STANDARD_COMPILE_OPTION})
  get_target_property(_opts ${target} COMPILE_OPTIONS)
  foreach(_o IN LISTS _opts)
    if(NOT _o MATCHES "^\\$<")
      list(APPEND _flags "${_o}")
    endif()
  endforeach()
  get_directory_property(_dir_defs COMPILE_DEFINITIONS)
  set(_defs ENGINE_SCHEMA_NO_REGISTRAR=1 ENGINE_GAME_MODULE=1)
  foreach(_d IN LISTS _dir_defs)
    if(NOT _d MATCHES "^\\$<")
      list(APPEND _defs "${_d}")
    endif()
  endforeach()
  _engine_game_module_includes(_includes)
  set(_link "")
  if(NOT _msvc)
    list(APPEND _link -shared)
    if(NOT ENGINE_ASAN AND NOT ENGINE_UBSAN)
      list(APPEND _link -Wl,-z,defs)
    endif()
    if(ENGINE_ASAN)
      list(APPEND _link -fsanitize=address)
    endif()
    if(ENGINE_UBSAN)
      list(APPEND _link -fsanitize=undefined)
    endif()
  endif()

  set(_content "# Generated by cmake/EngineGameModule.cmake. Read by cmake/EngineGameModuleBuild.cmake.\n")
  string(APPEND _content "set(GM_CXX [==[${CMAKE_CXX_COMPILER}]==])\n")
  string(APPEND _content "set(GM_MSVC ${_msvc})\n")
  string(APPEND _content "set(GM_FLAGS [==[${_flags}]==])\n")
  string(APPEND _content "set(GM_DEFINES [==[${_defs}]==])\n")
  string(APPEND _content "set(GM_INCLUDES [==[${_includes}]==])\n")
  string(APPEND _content "set(GM_LINK_FLAGS [==[${_link}]==])\n")
  string(APPEND _content "set(GM_SCHEMAC [==[$<TARGET_FILE:engine_schemac>]==])\n")
  string(APPEND _content "set(GM_SCHEMA_ROOT [==[${CMAKE_SOURCE_DIR}/schemas]==])\n")
  string(APPEND _content "set(GM_SUFFIX [==[${CMAKE_SHARED_MODULE_SUFFIX}]==])\n")
  # The MSVC environment the configure ran in (tools/dev.ps1 imports VsDevCmd): a test that runs
  # outside that environment still finds the compiler's headers and libraries.
  string(APPEND _content "set(GM_ENV_INCLUDE [==[$ENV{INCLUDE}]==])\n")
  string(APPEND _content "set(GM_ENV_LIB [==[$ENV{LIB}]==])\n")
  string(APPEND _content "set(GM_ENV_PATH [==[$ENV{PATH}]==])\n")
  file(GENERATE OUTPUT "${ENGINE_GAME_MODULE_BUILD_CONFIG}" CONTENT "${_content}")
endfunction()

function(engine_game_module)
  set(_one NAME OUTPUT_DIRECTORY)
  set(_multi SOURCES SCHEMAS)
  cmake_parse_arguments(GM "" "${_one}" "${_multi}" ${ARGN})
  if(NOT GM_NAME OR NOT GM_SOURCES)
    message(FATAL_ERROR "engine_game_module: NAME and SOURCES are required")
  endif()
  engine_capability_enabled(hotload _hotload)
  if(NOT _hotload)
    return()
  endif()
  set(_target engine_game_${GM_NAME})
  if(NOT GM_OUTPUT_DIRECTORY)
    set(GM_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/game")
  endif()

  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated/game_${GM_NAME}")
  set(_generated "")
  set(_outputs "")
  set(_abs_schemas "")
  foreach(_schema IN LISTS GM_SCHEMAS)
    get_filename_component(_abs "${_schema}" ABSOLUTE)
    get_filename_component(_stem "${_schema}" NAME_WE)
    list(APPEND _abs_schemas "${_abs}")
    list(APPEND _generated "${_gen}/src/${_stem}.cpp")
    list(APPEND _outputs "${_gen}/include/schemas/${_stem}.h" "${_gen}/include/schemas/${_stem}_ecs.h"
      "${_gen}/src/${_stem}.cpp" "${_gen}/json/${_stem}.schema.json" "${_gen}/docs/${_stem}.md")
  endforeach()
  if(_abs_schemas)
    add_custom_command(
      OUTPUT ${_outputs}
      COMMAND engine_schemac --out "${_gen}" --schema-root "${CMAKE_SOURCE_DIR}/schemas" ${_abs_schemas}
      DEPENDS ${_abs_schemas} engine_schemac
      COMMENT "schemac: game module ${GM_NAME}"
      VERBATIM)
  endif()

  add_library(${_target} MODULE ${GM_SOURCES} ${_generated})
  _engine_game_module_includes(_includes)
  target_include_directories(${_target} PRIVATE ${_includes} "${_gen}/include")
  target_compile_definitions(${_target} PRIVATE ENGINE_SCHEMA_NO_REGISTRAR=1 ENGINE_GAME_MODULE=1)
  target_compile_options(${_target} PRIVATE ${ENGINE_NO_EXCEPTIONS_FLAGS})
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(${_target} PRIVATE -fno-gnu-unique)
  endif()
  engine_apply_warnings(${_target})
  if(NOT MSVC AND NOT ENGINE_ASAN AND NOT ENGINE_UBSAN)
    target_link_options(${_target} PRIVATE -Wl,-z,defs)
  endif()
  set_target_properties(${_target} PROPERTIES
    OUTPUT_NAME "${GM_NAME}"
    PREFIX ""
    UNITY_BUILD ON
    UNITY_BUILD_BATCH_SIZE 0
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    LIBRARY_OUTPUT_DIRECTORY "${GM_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY "${GM_OUTPUT_DIRECTORY}"
    FOLDER "game")
  _engine_game_module_write_config(${_target})
  message(STATUS "engine game module: ${GM_NAME}")
endfunction()

function(engine_game_module_build_command target)
  if(NOT TARGET ${target})
    return()
  endif()
  target_compile_definitions(${target} PRIVATE
    ENGINE_CMAKE_COMMAND="${CMAKE_COMMAND}"
    ENGINE_GAME_MODULE_BUILD_CONFIG="${ENGINE_GAME_MODULE_BUILD_CONFIG}"
    ENGINE_GAME_MODULE_BUILD_SCRIPT="${ENGINE_GAME_MODULE_BUILD_SCRIPT}")
endfunction()
