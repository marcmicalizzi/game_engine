# The shader cost table (cmake/shader-costs.txt) and the pool it puts the heavy compiles in
# (roadmap T118; docs/ci/what-to-run.md, "Heavy shaders share a pool"). Needs no project, so the
# configure (cmake/EngineShaders.cmake) and the check (cmake/CheckShaderCosts.cmake, in script mode)
# read the table and derive the pool's depth with the same functions.
#
# **Why a pool.** One slangc compile of the largest material resolve variant holds 12.7 GB of
# commit for two minutes, and six of the sixteen variants hold 2.5 GB or more; a build that
# recompiles them at four or six jobs asked for 38 to 40 GB at once, and on 2026-11-11, with
# thirteen agents building, the machine ran out of commit. A Ninja pool caps how many of those run
# together and leaves every other step of the build as parallel as it was.

# The line: a shader whose row in the table says it peaks at this many MiB or more is heavy. The
# measured shaders fall either side of a wide gap (the light ones at 1.0 GB and under, the heavy ones
# at 2.5 GB and over, cmake/shader-costs.txt), and 1.5 GiB sits in it with room for a light one to
# grow by half before the check asks for its row.
set(ENGINE_SHADER_HEAVY_MB 1536)
# The share of the build machine's physical memory the heavy compiles of one build may hold at once
# when ENGINE_SHADER_HEAVY_JOBS is `auto`: a third, leaving the rest to the same build's C++ jobs,
# the other builds a shared machine runs, and the system.
set(ENGINE_SHADER_HEAVY_SHARE_DIVISOR 3)
set(ENGINE_SHADER_HEAVY_POOL engine_shader_heavy)

#   engine_read_shader_costs(<file>)
# Reads the table into the global properties ENGINE_SHADER_COST_NAMES (the stems, in the table's
# order), ENGINE_SHADER_COST_MB:<stem> and ENGINE_SHADER_COST_SECONDS:<stem>. One row a shader,
# `<stem> <whole MiB> <seconds>`; `#` starts a comment. A line of any other shape, or a stem named
# twice, stops the configure with the file and line. A missing file is an empty table.
function(engine_read_shader_costs file)
  set_property(GLOBAL PROPERTY ENGINE_SHADER_COST_NAMES "")
  if(NOT EXISTS "${file}")
    return()
  endif()
  file(READ "${file}" _text)
  # A comment may hold a semicolon, which a CMake list would split on.
  string(REPLACE ";" "<semicolon>" _text "${_text}")
  string(REPLACE "\r" "" _text "${_text}")
  string(REPLACE "\n" ";" _lines "${_text}")
  set(_number 0)
  set(_names "")
  foreach(_line IN LISTS _lines)
    math(EXPR _number "${_number} + 1")
    string(REGEX REPLACE "#.*$" "" _body "${_line}")
    string(STRIP "${_body}" _body)
    if(_body STREQUAL "")
      continue()
    endif()
    if(NOT _body MATCHES "^([A-Za-z0-9_]+)[ \t]+([0-9]+)[ \t]+([0-9]+(\\.[0-9]+)?)$")
      string(REPLACE "<semicolon>" ";" _shown "${_line}")
      message(FATAL_ERROR "${file}:${_number}: not a row of the shader cost table: '${_shown}' "
        "(want `<shader stem> <peak MiB> <seconds>`, docs/ci/what-to-run.md, \"Heavy shaders share a pool\")")
    endif()
    set(_stem "${CMAKE_MATCH_1}")
    set(_mb "${CMAKE_MATCH_2}")
    set(_seconds "${CMAKE_MATCH_3}")
    if(_stem IN_LIST _names)
      message(FATAL_ERROR "${file}:${_number}: the shader '${_stem}' has a row already")
    endif()
    list(APPEND _names "${_stem}")
    set_property(GLOBAL PROPERTY ENGINE_SHADER_COST_MB:${_stem} "${_mb}")
    set_property(GLOBAL PROPERTY ENGINE_SHADER_COST_SECONDS:${_stem} "${_seconds}")
  endforeach()
  set_property(GLOBAL PROPERTY ENGINE_SHADER_COST_NAMES "${_names}")
endfunction()

#   engine_shader_is_heavy(<stem> <out var>)
# TRUE when the table gives <stem> a peak at or above the line, else FALSE.
function(engine_shader_is_heavy stem out_var)
  get_property(_mb GLOBAL PROPERTY ENGINE_SHADER_COST_MB:${stem})
  if(NOT "${_mb}" STREQUAL "" AND NOT _mb LESS ENGINE_SHADER_HEAVY_MB)
    set(${out_var} TRUE PARENT_SCOPE)
  else()
    set(${out_var} FALSE PARENT_SCOPE)
  endif()
endfunction()

#   engine_check_shader_cost_names(<file> <known stem>...)
# Stops the configure when a row of the table names a shader that is not among the known stems:
# a misspelt or removed shader's row would otherwise sit in the table and pool nothing.
function(engine_check_shader_cost_names file)
  get_property(_names GLOBAL PROPERTY ENGINE_SHADER_COST_NAMES)
  set(_unknown "")
  foreach(_stem IN LISTS _names)
    if(NOT _stem IN_LIST ARGN)
      list(APPEND _unknown "${_stem}")
    endif()
  endforeach()
  if(_unknown)
    string(JOIN ", " _shown ${_unknown})
    message(FATAL_ERROR "${file}: no .slang source in the tree is named ${_shown}; a row names a "
      "shader by its source's file name without `.slang` (docs/ci/what-to-run.md, \"Heavy shaders share a pool\")")
  endif()
endfunction()

#   engine_shader_pool_depth(<physical MiB> <out var> <heavy peak MiB>...)
# The `auto` depth: the most heavy compiles whose peaks, largest first, sum to no more than the
# share of <physical MiB>, and never fewer than one, since a heavy shader has to compile at all.
# The largest first because the pool does not choose which heavy compiles run together, so the
# depth has to fit the worst set it could pick.
function(engine_shader_pool_depth physical_mb out_var)
  math(EXPR _budget "${physical_mb} / ${ENGINE_SHADER_HEAVY_SHARE_DIVISOR}")
  set(_peaks ${ARGN})
  if(_peaks)
    list(SORT _peaks COMPARE NATURAL ORDER DESCENDING)
  endif()
  set(_sum 0)
  set(_depth 0)
  foreach(_peak IN LISTS _peaks)
    math(EXPR _sum "${_sum} + ${_peak}")
    if(_sum GREATER _budget)
      break()
    endif()
    math(EXPR _depth "${_depth} + 1")
  endforeach()
  if(_depth LESS 1)
    set(_depth 1)
  endif()
  set(${out_var} ${_depth} PARENT_SCOPE)
endfunction()
