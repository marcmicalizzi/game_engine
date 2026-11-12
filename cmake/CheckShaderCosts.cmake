# build.shader_pool: the heavy shader compiles are in the pool, and the table that says which ones
# are heavy still says what the build measured (roadmap T118; docs/ci/what-to-run.md, "Heavy
# shaders share a pool"; cmake/EngineShaders.cmake registers both tests).
#
#   cmake -DBINARY_DIR=<build/preset> -P cmake/CheckShaderCosts.cmake
#   cmake -DSELF_TEST=ON -DWORK=<directory> -P cmake/CheckShaderCosts.cmake
#
# The real check reads build/<preset>/shaders/pool.txt (the configure's list of every slangc
# compile, its class, its SPIR-V and its measurement), build.ninja, and each `<stem>.cost` the
# meter (tools/shader_meter) wrote when the shader last compiled, and fails when
#   - a heavy shader's build statement does not carry the pool, or a light one's does;
#   - the pool's depth in build.ninja is not the configure's;
#   - a shader measured at or above the line is not heavy in the table (it grew, or is new): the
#     failure prints the row to add to cmake/shader-costs.txt;
#   - a heavy shader measured under half the line (it shrank, and holds a pool slot for nothing);
#   - a shader's SPIR-V exists with no measurement beside it (built by something other than the
#     meter, so the check cannot see it).
# A shader not built yet has neither and is counted, not failed. The half-line hysteresis keeps a
# shader near the line from flipping class between a Windows build (peak commit) and a Linux one
# (peak resident set).
#
# The self-test runs the reader, the depth and the classification over fixtures with known answers
# in a directory of its own under WORK, so a check that stopped matching anything fails here rather
# than passing everywhere quietly; and it runs the reader in a child `cmake -P` over a malformed row,
# a doubled row and an unknown name, each of which has to stop it.

cmake_minimum_required(VERSION 3.28)
include("${CMAKE_CURRENT_LIST_DIR}/EngineShaderCosts.cmake")

# The problems one build shows, as a list of sentences in <out var>, and a line of counts in
# <summary var>. <pool file> is pool.txt's path, <ninja> the Ninja files to read (rules.ninja,
# where CMake declares the pools, and build.ninja). Separate from the real run so the self-test
# drives it.
function(_engine_shader_pool_problems pool_file ninja out_var summary_var)
  set(_problems "")
  file(STRINGS "${pool_file}" _pool_lines)
  set(_line_mb "")
  set(_pool "")
  set(_depth "")
  set(_entries "")
  foreach(_l IN LISTS _pool_lines)
    if(_l MATCHES "^line_mb ([0-9]+)$")
      set(_line_mb "${CMAKE_MATCH_1}")
    elseif(_l MATCHES "^pool (.+)$")
      set(_pool "${CMAKE_MATCH_1}")
    elseif(_l MATCHES "^depth ([0-9]+)$")
      set(_depth "${CMAKE_MATCH_1}")
    elseif(_l MATCHES "^shader (.+)$")
      list(APPEND _entries "${CMAKE_MATCH_1}")
    endif()
  endforeach()
  if(_line_mb STREQUAL "" OR _pool STREQUAL "" OR _depth STREQUAL "")
    list(APPEND _problems "${pool_file} has no line, pool or depth; configure again")
    set(${out_var} "${_problems}" PARENT_SCOPE)
    set(${summary_var} "" PARENT_SCOPE)
    return()
  endif()
  math(EXPR _half "${_line_mb} / 2")

  # build.ninja: the build statements and the pool lines under them, in order, so a pool line
  # belongs to the build line before it; and the pool's declaration with its depth.
  set(_ninja_lines "")
  foreach(_ninja_file IN LISTS ninja)
    file(STRINGS "${_ninja_file}" _file_lines REGEX "^(build |pool |  pool = |  depth = )")
    list(APPEND _ninja_lines ${_file_lines})
  endforeach()
  set(_declared_depth "")
  set(_in_declaration FALSE)
  set(_statement "")
  set(_pooled "")
  foreach(_l IN LISTS _ninja_lines)
    if(_l MATCHES "^pool (.+)$")
      set(_in_declaration FALSE)
      if("${CMAKE_MATCH_1}" STREQUAL "${_pool}")
        set(_in_declaration TRUE)
      endif()
    elseif(_l MATCHES "^  depth = ([0-9]+)$")
      if(_in_declaration)
        set(_declared_depth "${CMAKE_MATCH_1}")
      endif()
    elseif(_l MATCHES "^build ")
      set(_in_declaration FALSE)
      string(REPLACE "\\" "/" _statement "${_l}")
    elseif(_l MATCHES "^  pool = (.+)$")
      if("${CMAKE_MATCH_1}" STREQUAL "${_pool}" AND NOT _statement STREQUAL "")
        list(APPEND _pooled "${_statement}")
      endif()
    endif()
  endforeach()
  if(NOT _declared_depth STREQUAL _depth)
    list(APPEND _problems "build.ninja declares the pool ${_pool} with depth '${_declared_depth}', the configure chose ${_depth}")
  endif()

  set(_heavy_count 0)
  set(_measured 0)
  set(_unbuilt 0)
  set(_rows "")
  foreach(_entry IN LISTS _entries)
    string(REPLACE "|" ";" _fields "${_entry}")
    list(GET _fields 0 _stem)
    list(GET _fields 1 _heavy)
    list(GET _fields 2 _spv_rel)
    list(GET _fields 3 _cost)
    if(_heavy)
      math(EXPR _heavy_count "${_heavy_count} + 1")
    endif()
    # Ninja writes the outputs relative to the build directory, with the platform's separators.
    string(REGEX REPLACE "([][+.*?^$()|{}])" "\\\\\\1" _spv_pattern "${_spv_rel}")
    set(_in_pool FALSE)
    foreach(_s IN LISTS _pooled)
      if(_s MATCHES "^build ${_spv_pattern}[ :|]")
        set(_in_pool TRUE)
        break()
      endif()
    endforeach()
    if(_heavy AND NOT _in_pool)
      list(APPEND _problems "${_stem} is heavy in the table and its compile is not in the pool ${_pool} in build.ninja")
    elseif(NOT _heavy AND _in_pool)
      list(APPEND _problems "${_stem} is light in the table and its compile is in the pool ${_pool} in build.ninja")
    endif()

    get_filename_component(_cost_dir "${_cost}" DIRECTORY)
    if(NOT EXISTS "${_cost}")
      if(EXISTS "${_cost_dir}/${_stem}.spv")
        list(APPEND _problems "${_stem}.spv exists with no ${_stem}.cost beside it: it was not compiled through tools/shader_meter, so its cost is unseen")
      else()
        math(EXPR _unbuilt "${_unbuilt} + 1")
      endif()
      continue()
    endif()
    file(STRINGS "${_cost}" _cost_lines)
    set(_peak "")
    set(_seconds "")
    foreach(_c IN LISTS _cost_lines)
      if(_c MATCHES "^peak_mb ([0-9]+)$")
        set(_peak "${CMAKE_MATCH_1}")
      elseif(_c MATCHES "^seconds ([0-9.]+)$")
        set(_seconds "${CMAKE_MATCH_1}")
      endif()
    endforeach()
    if(_peak STREQUAL "")
      list(APPEND _problems "${_cost} holds no peak_mb line")
      continue()
    endif()
    math(EXPR _measured "${_measured} + 1")
    if(NOT _peak LESS _line_mb)
      list(APPEND _rows "${_stem} ${_peak} ${_seconds}")
    endif()
    if(NOT _heavy AND NOT _peak LESS _line_mb)
      list(APPEND _problems "${_stem} measured ${_peak} MiB, at or above the line of ${_line_mb}, and the table does not make it heavy: put `${_stem} ${_peak} ${_seconds}  # <where>` in cmake/shader-costs.txt")
    elseif(_heavy AND _peak LESS _half)
      list(APPEND _problems "${_stem} is heavy in the table and measured ${_peak} MiB, under half the line (${_half}): write its row `${_stem} ${_peak} ${_seconds}  # <where>` in cmake/shader-costs.txt")
    endif()
  endforeach()
  list(LENGTH _entries _count)
  string(JOIN ", " _rows_shown ${_rows})
  set(${summary_var} "${_count} shader compiles, ${_heavy_count} heavy in ${_pool} at depth ${_depth}, ${_measured} measured, ${_unbuilt} not built; at or above the line: ${_rows_shown}" PARENT_SCOPE)
  set(${out_var} "${_problems}" PARENT_SCOPE)
endfunction()

if(CASE STREQUAL "read")
  # A child of the self-test: read TABLE and check its names against KNOWN; any refusal is this
  # process's FATAL_ERROR and its exit code.
  engine_read_shader_costs("${TABLE}")
  string(REPLACE "," ";" _known "${KNOWN}")
  engine_check_shader_cost_names("${TABLE}" ${_known})
  return()
endif()

if(SELF_TEST)
  if(NOT WORK)
    message(FATAL_ERROR "tools.shader_costs: -DWORK=<directory> is required")
  endif()
  string(RANDOM LENGTH 12 ALPHABET "0123456789abcdef" _id)
  set(_dir "${WORK}/shader-costs-${_id}")
  file(MAKE_DIRECTORY "${_dir}")
  set(_failures 0)
  # The condition is if()'s own text, evaluated as code so its spaces and quotes are if()'s.
  macro(_expect what condition)
    cmake_language(EVAL CODE "if(${condition})\nset(_ok TRUE)\nelse()\nset(_ok FALSE)\nendif()")
    if(_ok)
      message(STATUS "  ok   ${what}")
    else()
      message(STATUS "  FAIL ${what}")
      math(EXPR _failures "${_failures} + 1")
    endif()
  endmacro()

  # The reader: rows, comments, a comment holding a semicolon, and the class each row gives.
  file(WRITE "${_dir}/table.txt" "# a comment; with a semicolon\n\nbig 12699 139.4  # R148; one at a time\nmid 2504 22.8\nsmall 997 10.7\nedge 1536 20\n")
  engine_read_shader_costs("${_dir}/table.txt")
  get_property(_names GLOBAL PROPERTY ENGINE_SHADER_COST_NAMES)
  _expect("the reader keeps the four rows in order" "\"${_names}\" STREQUAL \"big;mid;small;edge\"")
  get_property(_s GLOBAL PROPERTY ENGINE_SHADER_COST_SECONDS:big)
  _expect("a row's seconds are read past its comment" "\"${_s}\" STREQUAL \"139.4\"")
  engine_shader_is_heavy(big _h1)
  engine_shader_is_heavy(edge _h2)
  engine_shader_is_heavy(small _h3)
  engine_shader_is_heavy(absent _h4)
  _expect("a peak above the line is heavy" "_h1")
  _expect("a peak at the line is heavy" "_h2")
  _expect("a peak under the line is light" "NOT _h3")
  _expect("a shader with no row is light" "NOT _h4")

  # The refusals, each in a child process because each is a FATAL_ERROR.
  foreach(_case IN ITEMS "malformed|big 12699\n|not a row" "doubled|big 1 1\nbig 2 2\n|has a row already"
                         "unknown|big 1 1\nbgi 2 2\n|no .slang source")
    string(REPLACE "|" ";" _parts "${_case}")
    list(GET _parts 0 _label)
    list(GET _parts 1 _content)
    list(GET _parts 2 _says)
    file(WRITE "${_dir}/${_label}.txt" "${_content}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -DCASE=read "-DTABLE=${_dir}/${_label}.txt" "-DKNOWN=big,mid"
                            -P "${CMAKE_CURRENT_LIST_FILE}"
                    RESULT_VARIABLE _code OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
    string(FIND "${_err}" "${_says}" _at)
    _expect("the table '${_label}' stops the configure and says '${_says}'" "NOT _code EQUAL 0 AND _at GREATER -1")
  endforeach()
  file(WRITE "${_dir}/fine.txt" "big 1 1\nmid 2 2\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" -DCASE=read "-DTABLE=${_dir}/fine.txt" "-DKNOWN=big,mid"
                          -P "${CMAKE_CURRENT_LIST_FILE}" RESULT_VARIABLE _code OUTPUT_QUIET ERROR_QUIET)
  _expect("a table naming only known shaders reads" "_code EQUAL 0")

  # The depth: R148's six heavy peaks (cmake/shader-costs.txt, be3826d9) against this machine's
  # 64 GB, a 32 GB developer machine, a 128 GB one, and none heavy at all.
  set(_six 12699 7185 3939 2838 2536 2504)
  engine_shader_pool_depth(65229 _d64 ${_six})
  engine_shader_pool_depth(32768 _d32 ${_six})
  engine_shader_pool_depth(131072 _d128 ${_six})
  engine_shader_pool_depth(32768 _d0)
  engine_shader_pool_depth(65229 _dorder 2504 12699 2536 7185 2838 3939)
  _expect("64 GB: a third is 21,743 MiB, the two largest are 19,884, three 23,823: depth 2 (got ${_d64})" "_d64 EQUAL 2")
  _expect("32 GB: a third is 10,922 MiB, under the largest alone: depth 1, never 0 (got ${_d32})" "_d32 EQUAL 1")
  _expect("128 GB: a third is 43,690 MiB, all six are 31,701: depth 6 (got ${_d128})" "_d128 EQUAL 6")
  _expect("no heavy shader: depth 1 (got ${_d0})" "_d0 EQUAL 1")
  _expect("the depth takes the largest first, whatever the table's order (got ${_dorder})" "_dorder EQUAL 2")

  # The classification over one fixture build: a heavy shader in the pool, a light one out of it,
  # and then each way the build can disagree with the table.
  set(_b "${_dir}/build")
  file(MAKE_DIRECTORY "${_b}/sh")
  function(_fixture pool_body heavy_measured light_measured)
    file(WRITE "${_b}/pool.txt" "line_mb 1536\npool engine_shader_heavy\ndepth 2\nshader big|TRUE|sh/big.spv|${_b}/sh/big.cost\nshader small|FALSE|sh/small.spv|${_b}/sh/small.cost\nshader later|FALSE|sh/later.spv|${_b}/sh/later.cost\n")
    file(WRITE "${_b}/build.ninja" "${pool_body}")
    file(REMOVE "${_b}/sh/big.cost" "${_b}/sh/small.cost" "${_b}/sh/small.spv")
    if(NOT heavy_measured STREQUAL "")
      file(WRITE "${_b}/sh/big.cost" "peak_mb ${heavy_measured}\nseconds 139.4\n")
    endif()
    if(light_measured STREQUAL "none")
      file(WRITE "${_b}/sh/small.spv" "x")
    elseif(NOT light_measured STREQUAL "")
      file(WRITE "${_b}/sh/small.cost" "peak_mb ${light_measured}\nseconds 10.7\n")
    endif()
  endfunction()
  set(_good "pool engine_shader_heavy\n  depth = 2\n\nbuild sh\\big.spv sh\\big.cost <pipe> \${cmake_ninja_workdir}sh\\big.spv: CUSTOM_COMMAND a.slang\n  COMMAND = x\n  pool = engine_shader_heavy\n  restat = 1\n\nbuild sh\\small.spv sh\\small.cost: CUSTOM_COMMAND b.slang\n  COMMAND = y\n  restat = 1\n")
  set(_cases
    "as the table says|${_good}|12699|997|0|-"
    "the heavy compile is not in the pool|pool engine_shader_heavy\n  depth = 2\nbuild sh\\big.spv: CUSTOM_COMMAND a\nbuild sh\\small.spv: CUSTOM_COMMAND b\n|12699|997|1|not in the pool"
    "the light compile is in the pool|pool engine_shader_heavy\n  depth = 2\nbuild sh\\big.spv: CUSTOM_COMMAND a\n  pool = engine_shader_heavy\nbuild sh\\small.spv: CUSTOM_COMMAND b\n  pool = engine_shader_heavy\n|12699|997|1|is in the pool"
    "the depth differs from the configure's|pool engine_shader_heavy\n  depth = 3\nbuild sh\\big.spv: CUSTOM_COMMAND a\n  pool = engine_shader_heavy\nbuild sh\\small.spv: CUSTOM_COMMAND b\n|12699|997|1|with depth '3'"
    "a light shader grew past the line|${_good}|12699|1600|1|put `small 1600 10.7"
    "a light shader grew to just under the line|${_good}|12699|1535|0|-"
    "a heavy shader shrank under half the line|${_good}|700|997|1|under half the line"
    "a heavy shader shrank, not under half|${_good}|800|997|0|-"
    "a SPIR-V with no measurement|${_good}|12699|none|1|not compiled through tools/shader_meter")
  foreach(_case IN LISTS _cases)
    string(REPLACE "|" ";" _parts "${_case}")
    list(GET _parts 0 _label)
    list(GET _parts 1 _body)
    list(GET _parts 2 _hm)
    list(GET _parts 3 _lm)
    list(GET _parts 4 _want)
    list(GET _parts 5 _says)
    string(REPLACE "<pipe>" "|" _body "${_body}")
    _fixture("${_body}" "${_hm}" "${_lm}")
    _engine_shader_pool_problems("${_b}/pool.txt" "${_b}/build.ninja" _found _summary)
    list(LENGTH _found _n)
    set(_matches TRUE)
    if(NOT _says STREQUAL "-")
      string(FIND "${_found}" "${_says}" _at)
      if(_at LESS 0)
        set(_matches FALSE)
      endif()
    endif()
    _expect("${_label}: ${_want} problem(s) (found ${_n}: ${_found})" "_n EQUAL _want AND _matches")
  endforeach()

  file(REMOVE_RECURSE "${_dir}")
  if(_failures GREATER 0)
    message(FATAL_ERROR "tools.shader_costs: ${_failures} case(s) did not come out as expected")
  endif()
  message(STATUS "tools.shader_costs: every case came out as expected")
  return()
endif()

# --- the real build -----------------------------------------------------------------------------

if(NOT BINARY_DIR)
  message(FATAL_ERROR "build.shader_pool: -DBINARY_DIR=<build/preset> is required")
endif()
foreach(_file IN ITEMS "${BINARY_DIR}/shaders/pool.txt" "${BINARY_DIR}/build.ninja")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "build.shader_pool: ${_file} does not exist; configure and build first")
  endif()
endforeach()
# CMake declares the pools in CMakeFiles/rules.ninja, which build.ninja includes.
set(_ninja_files "${BINARY_DIR}/build.ninja")
if(EXISTS "${BINARY_DIR}/CMakeFiles/rules.ninja")
  list(PREPEND _ninja_files "${BINARY_DIR}/CMakeFiles/rules.ninja")
endif()
_engine_shader_pool_problems("${BINARY_DIR}/shaders/pool.txt" "${_ninja_files}" _problems _summary)
message(STATUS "build.shader_pool: ${_summary}")
if(_problems)
  list(LENGTH _problems _n)
  foreach(_p IN LISTS _problems)
    message(STATUS "  ${_p}")
  endforeach()
  message(FATAL_ERROR "build.shader_pool: ${_n} problem(s); docs/ci/what-to-run.md, \"Heavy shaders share a pool\", says what each means")
endif()
