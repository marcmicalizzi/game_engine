# Shader compilation (ADR-0008): Slang prebuilt compiler fetched per host platform, `.slang`
# files compiled to SPIR-V at build time, and the bytes embedded in generated headers so tests
# and tools need no file lookup. Runtime loading through the VFS and a hash-keyed cache arrive
# with the renderer's shader library.
#
#   engine_shaders(NAME <name> SOURCES <file.slang>... [PROFILE spirv_1_6] [FLAGS ...]
#                  [DEPENDS <included.slang>...])
#
# Each source's `#include "..."` graph is walked at configure time and every file it reaches is
# added to that source's build dependencies, so editing a shared header (brdf.slang,
# scene.slang) rebuilds exactly the shaders that include it and nothing else. Reading a dozen
# small files costs nothing at configure time, and the scan is not registered as a configure
# dependency on purpose: re-running CMake on every shader save would cost far more than it
# saves, and while iterating on a shader it is gfx::ShaderLibrary's hot reload that recompiles,
# not the build. The consequence is that *adding or removing* an `#include` line needs a
# reconfigure to be seen; DEPENDS is the escape hatch for a file the scan cannot resolve (a
# generated header, or one reached through a macro) and stays supported.
#
# Produces the INTERFACE target engine_shaders_<name> whose include directory holds
# <stem>.spv.h for every source, each declaring
#   namespace engine::shaders { inline constexpr unsigned char k_<stem>_spirv[] = {...}; }
# aligned for VkShaderModuleCreateInfo. Every entry point in a file is compiled; entry point
# names are preserved in the SPIR-V.

include(FetchContent)

option(ENGINE_SHADERS "Fetch the Slang compiler and build shaders" ON)
set(ENGINE_SLANG_VERSION "2026.17.1" CACHE STRING "Slang release to fetch")

if(ENGINE_SHADERS)
  if(WIN32)
    set(_slang_archive "slang-${ENGINE_SLANG_VERSION}-windows-x86_64.zip")
    set(_slangc_name "slangc.exe")
  else()
    set(_slang_archive "slang-${ENGINE_SLANG_VERSION}-linux-x86_64.tar.gz")
    set(_slangc_name "slangc")
  endif()
  FetchContent_Declare(slang
    URL "https://github.com/shader-slang/slang/releases/download/v${ENGINE_SLANG_VERSION}/${_slang_archive}")
  FetchContent_MakeAvailable(slang)
  find_program(ENGINE_SLANGC NAMES ${_slangc_name}
    PATHS "${slang_SOURCE_DIR}/bin" NO_DEFAULT_PATH REQUIRED)
  message(STATUS "engine: slangc ${ENGINE_SLANG_VERSION} at ${ENGINE_SLANGC}")
endif()

# The heavy compiles share a pool (roadmap T118; docs/ci/what-to-run.md, "Heavy shaders share a
# pool"; cmake/EngineShaderCosts.cmake has the line, the share and why). A shader is heavy when
# cmake/shader-costs.txt gives it a peak at or above ENGINE_SHADER_HEAVY_MB; its slangc compile is
# given the Ninja job pool `engine_shader_heavy`, so no more than ENGINE_SHADER_HEAVY_JOBS of them
# run at once while every light shader and every C++ step runs as parallel as the build's jobs
# allow. Every compile runs under tools/shader_meter, which writes its peak memory and seconds
# beside the SPIR-V (`<stem>.cost`), and build.shader_pool (cmake/CheckShaderCosts.cmake) holds the
# table against those measurements and the pool against build.ninja, so a shader that grows past
# the line fails a test instead of joining the next build's peak unannounced. A generator with no
# pools (anything but Ninja) ignores JOB_POOL and configures as before.
include("${CMAKE_CURRENT_LIST_DIR}/EngineShaderCosts.cmake")
set(ENGINE_SHADER_HEAVY_JOBS "auto" CACHE STRING
  "How many heavy shader compiles run at once: `auto` (as many of the largest as fit in a third of this machine's physical memory, at least one) or a whole number")
set(_engine_shader_cost_file "${CMAKE_SOURCE_DIR}/cmake/shader-costs.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_engine_shader_cost_file}")
engine_read_shader_costs("${_engine_shader_cost_file}")

# The quoted #include directives of one file, resolved against the including file's own
# directory first (which is how slangc resolves them) and then against the top-level source's
# directory. Angle-bracket includes are the compiler's own and are not tracked; a directive that
# resolves to nothing is left to slangc to complain about.
function(_engine_scan_shader_includes source root out_var)
  set(_found "")
  if(EXISTS "${source}")
    get_filename_component(_dir "${source}" DIRECTORY)
    file(STRINGS "${source}" _lines REGEX "^[ \t]*#[ \t]*include[ \t]*\"")
    foreach(_line IN LISTS _lines)
      string(REGEX REPLACE "^[ \t]*#[ \t]*include[ \t]*\"([^\"]+)\".*$" "\\1" _rel "${_line}")
      if(NOT "${_rel}" STREQUAL "${_line}")
        set(_candidate "${_dir}/${_rel}")
        if(NOT EXISTS "${_candidate}")
          set(_candidate "${root}/${_rel}")
        endif()
        if(EXISTS "${_candidate}")
          get_filename_component(_abs "${_candidate}" ABSOLUTE)
          list(APPEND _found "${_abs}")
        endif()
      endif()
    endforeach()
  endif()
  set(${out_var} "${_found}" PARENT_SCOPE)
endfunction()

# Every file `source` reaches through those directives, transitively. A file is visited once, so
# a cycle terminates; sixty-four files is a shader's include graph several times over and the
# guard is there so a configure can never hang on one.
function(_engine_shader_include_tree source out_var)
  get_filename_component(_root "${source}" DIRECTORY)
  set(_pending "${source}")
  set(_seen "${source}")
  set(_result "")
  set(_guard 0)
  while(_pending)
    list(POP_FRONT _pending _current)
    _engine_scan_shader_includes("${_current}" "${_root}" _includes)
    foreach(_inc IN LISTS _includes)
      if(NOT "${_inc}" IN_LIST _seen)
        list(APPEND _seen "${_inc}")
        list(APPEND _result "${_inc}")
        list(APPEND _pending "${_inc}")
      endif()
    endforeach()
    math(EXPR _guard "${_guard} + 1")
    if(_guard GREATER 64)
      break()
    endif()
  endwhile()
  set(${out_var} "${_result}" PARENT_SCOPE)
endfunction()

function(engine_shaders)
  set(_one NAME PROFILE)
  set(_multi SOURCES FLAGS DEPENDS)
  cmake_parse_arguments(SH "" "${_one}" "${_multi}" ${ARGN})
  if(NOT SH_NAME OR NOT SH_SOURCES)
    message(FATAL_ERROR "engine_shaders: NAME and SOURCES are required")
  endif()
  if(NOT SH_PROFILE)
    set(SH_PROFILE spirv_1_6)
  endif()
  set(_target engine_shaders_${SH_NAME})
  add_library(${_target} INTERFACE)
  if(NOT ENGINE_SHADERS)
    message(STATUS "engine shaders: ${SH_NAME} skipped (ENGINE_SHADERS is OFF)")
    return()
  endif()

  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/shaders/${SH_NAME}")
  set(_headers "")
  set(_included "")
  foreach(_inc IN LISTS SH_DEPENDS)
    get_filename_component(_inc_abs "${_inc}" ABSOLUTE)
    list(APPEND _included "${_inc_abs}")
  endforeach()
  foreach(_src IN LISTS SH_SOURCES)
    get_filename_component(_abs "${_src}" ABSOLUTE)
    get_filename_component(_stem "${_src}" NAME_WE)
    set(_spv "${_gen}/${_stem}.spv")
    set(_header "${_gen}/include/shaders/${_stem}.spv.h")
    _engine_shader_include_tree("${_abs}" _scanned)
    # The meter's measurement is an output beside the SPIR-V, so a build that lost it compiles
    # the shader again rather than leave the check without its number.
    set(_cost "${_gen}/${_stem}.cost")
    engine_shader_is_heavy("${_stem}" _heavy)
    set(_pool "")
    if(_heavy)
      set(_pool JOB_POOL ${ENGINE_SHADER_HEAVY_POOL})
    endif()
    file(RELATIVE_PATH _spv_rel "${CMAKE_BINARY_DIR}" "${_spv}")
    set_property(GLOBAL APPEND PROPERTY ENGINE_SHADER_POOL_ENTRIES
      "${_stem}|${_heavy}|${_spv_rel}|${_cost}")
    add_custom_command(
      OUTPUT "${_spv}" "${_cost}"
      COMMAND engine_shader_meter "${_cost}" "${ENGINE_SLANGC}" "${_abs}"
              -target spirv -profile ${SH_PROFILE} -emit-spirv-directly
              -fvk-use-entrypoint-name -O2 -warnings-disable 41012 ${SH_FLAGS}
              -o "${_spv}"
      DEPENDS "${_abs}" ${_included} ${_scanned}
      ${_pool}
      COMMENT "slangc: ${_src}"
      VERBATIM)
    add_custom_command(
      OUTPUT "${_header}"
      COMMAND "${CMAKE_COMMAND}"
              "-DINPUT=${_spv}" "-DOUTPUT=${_header}" "-DSYMBOL=k_${_stem}_spirv"
              -P "${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake"
      DEPENDS "${_spv}" "${CMAKE_SOURCE_DIR}/cmake/EmbedFile.cmake"
      COMMENT "embed: ${_stem}.spv"
      VERBATIM)
    list(APPEND _headers "${_header}")
    # Manifest entry for the runtime shader library (gfx::ShaderLibrary): where the source and
    # the SPIR-V live and how slangc was invoked, so a running app can recompile on edit.
    set(_args "\"-target\", \"spirv\", \"-profile\", \"${SH_PROFILE}\", \"-emit-spirv-directly\", \"-fvk-use-entrypoint-name\", \"-O2\", \"-warnings-disable\", \"41012\"")
    foreach(_flag IN LISTS SH_FLAGS)
      string(APPEND _args ", \"${_flag}\"")
    endforeach()
    set_property(GLOBAL APPEND PROPERTY ENGINE_SHADER_MANIFEST_ENTRIES
      "{\"name\": \"${_stem}\", \"source\": \"${_abs}\", \"spirv\": \"${_spv}\", \"args\": [${_args}]}")
  endforeach()
  add_custom_target(${_target}_build DEPENDS ${_headers})
  add_dependencies(${_target} ${_target}_build)
  target_include_directories(${_target} INTERFACE "${_gen}/include")
  message(STATUS "engine shaders: ${SH_NAME} (${SH_PROFILE})")
endfunction()

# Writes build/<preset>/shaders/manifest.json listing every shader declared with
# engine_shaders(): name, source, SPIR-V output, slangc arguments, and the compiler path.
# gfx::ShaderLibrary::load_manifest() reads it to load shaders from files and to recompile
# them when their sources change while an app runs.
function(engine_write_shader_manifest)
  if(NOT ENGINE_SHADERS)
    return()
  endif()
  get_property(_entries GLOBAL PROPERTY ENGINE_SHADER_MANIFEST_ENTRIES)
  string(JOIN ",\n    " _joined ${_entries})
  file(WRITE "${CMAKE_BINARY_DIR}/shaders/manifest.json"
    "{\n  \"slangc\": \"${ENGINE_SLANGC}\",\n  \"shaders\": [\n    ${_joined}\n  ]\n}\n")
  list(LENGTH _entries _count)
  message(STATUS "engine shaders: manifest with ${_count} shaders at ${CMAKE_BINARY_DIR}/shaders/manifest.json")
  _engine_shader_pool_finalize()
endfunction()

# The pool's half of the manifest, when every engine_shaders() call has been made: the table's rows
# checked against the tree's sources, the depth settled, the pool declared, and the list of every
# compile with its class written to build/<preset>/shaders/pool.txt for build.shader_pool.
function(_engine_shader_pool_finalize)
  # A row is known when a .slang source of that name exists anywhere in the tree's layers, not
  # only among this configuration's shaders: a capability switched off (ENGINE_MINIMAL) drops its
  # shaders from the build but its row is still right for the builds that carry it.
  set(_known "")
  foreach(_layer IN ITEMS core foundation domain systems apps game tools)
    file(GLOB_RECURSE _sources LIST_DIRECTORIES false "${CMAKE_SOURCE_DIR}/${_layer}/*.slang")
    foreach(_source IN LISTS _sources)
      get_filename_component(_stem "${_source}" NAME_WE)
      list(APPEND _known "${_stem}")
    endforeach()
  endforeach()
  engine_check_shader_cost_names("${_engine_shader_cost_file}" ${_known})

  get_property(_pool_entries GLOBAL PROPERTY ENGINE_SHADER_POOL_ENTRIES)
  set(_heavy_peaks "")
  set(_heavy_names "")
  foreach(_entry IN LISTS _pool_entries)
    string(REPLACE "|" ";" _fields "${_entry}")
    list(GET _fields 0 _stem)
    list(GET _fields 1 _heavy)
    if(_heavy)
      get_property(_mb GLOBAL PROPERTY ENGINE_SHADER_COST_MB:${_stem})
      list(APPEND _heavy_peaks "${_mb}")
      list(APPEND _heavy_names "${_stem}")
    endif()
  endforeach()

  cmake_host_system_information(RESULT _physical_mb QUERY TOTAL_PHYSICAL_MEMORY)
  if(ENGINE_SHADER_HEAVY_JOBS STREQUAL "auto")
    engine_shader_pool_depth("${_physical_mb}" _depth ${_heavy_peaks})
    set(_how "auto: a third of ${_physical_mb} MiB")
  elseif(ENGINE_SHADER_HEAVY_JOBS MATCHES "^[1-9][0-9]*$")
    set(_depth "${ENGINE_SHADER_HEAVY_JOBS}")
    set(_how "ENGINE_SHADER_HEAVY_JOBS")
  else()
    message(FATAL_ERROR "ENGINE_SHADER_HEAVY_JOBS is '${ENGINE_SHADER_HEAVY_JOBS}': `auto` or a whole number of at least 1")
  endif()
  set_property(GLOBAL APPEND PROPERTY JOB_POOLS "${ENGINE_SHADER_HEAVY_POOL}=${_depth}")

  set(_text "line_mb ${ENGINE_SHADER_HEAVY_MB}\npool ${ENGINE_SHADER_HEAVY_POOL}\ndepth ${_depth}\n")
  foreach(_entry IN LISTS _pool_entries)
    string(APPEND _text "shader ${_entry}\n")
  endforeach()
  file(WRITE "${CMAKE_BINARY_DIR}/shaders/pool.txt" "${_text}")
  list(LENGTH _heavy_names _heavy_count)
  string(JOIN " " _heavy_shown ${_heavy_names})
  message(STATUS "engine shaders: ${_heavy_count} heavy (at or above ${ENGINE_SHADER_HEAVY_MB} MiB), at most ${_depth} at once (${_how}): ${_heavy_shown}")

  if(ENGINE_BUILD_TESTS)
    add_test(NAME tools.shader_costs
      COMMAND "${CMAKE_COMMAND}" -DSELF_TEST=ON "-DWORK=${CMAKE_BINARY_DIR}/Testing/Temporary"
              "-DMETER=$<TARGET_FILE:engine_shader_meter>"
              -P "${CMAKE_SOURCE_DIR}/cmake/CheckShaderCosts.cmake")
    set_tests_properties(tools.shader_costs PROPERTIES LABELS "tools")
    if(CMAKE_GENERATOR MATCHES "Ninja")
      add_test(NAME build.shader_pool
        COMMAND "${CMAKE_COMMAND}" "-DBINARY_DIR=${CMAKE_BINARY_DIR}"
                -P "${CMAKE_SOURCE_DIR}/cmake/CheckShaderCosts.cmake")
      set_tests_properties(build.shader_pool PROPERTIES LABELS "build")
    else()
      message(STATUS "engine shaders: build.shader_pool reads build.ninja; not registered for ${CMAKE_GENERATOR}")
    endif()
  endif()
endfunction()
