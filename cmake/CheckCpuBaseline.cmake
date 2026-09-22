# build.cpu_baseline: who carries the instruction-set baseline, read from what the build did
# (ADR-0031, ADR-0034; cmake/EngineCpuBaseline.cmake registers it).
#
#   cmake -DBINARY_DIR=<build/preset> -DBASELINE=v3 -DFLAGS=/arch:AVX2 \
#         -DHOST_TOOLS=engine_schemac -DFLOOR=engine_platform -P cmake/CheckCpuBaseline.cmake
#   cmake -DSELF_TEST=ON -P cmake/CheckCpuBaseline.cmake
#
# (list arguments are `|`-separated, because a `;` does not survive a CTest command line intact).
#
# Three rules, each of which a build can break without anybody noticing until it runs somewhere it
# was not tested:
#
#   1. Every translation unit of a **host tool** or of the **floor** (`core/platform`) carries no
#      instruction-set flag at all — not the baseline's, and not any other `/arch:` or `-march=`.
#   2. Every other translation unit carries **every** baseline flag, as a whole token: the engine,
#      its apps, tests and benches, and every third-party library built here. ADR-0031's proof was
#      this reading of `compile_commands.json` done once by hand; this is it done on every run.
#   3. Every executable of this build that a build step **runs** (a `COMMAND` in `build.ninja`) is
#      a declared host tool. That is what makes the classification a property of the build graph
#      rather than of somebody's memory: a second generator added without `engine_host_tool()`
#      would otherwise carry the baseline and pass rules 1 and 2, and be found — as schemac was —
#      six minutes into a build on the one machine below the baseline.
#
# The parse is of the two files as CMake's Ninja generator writes them: `compile_commands.json`
# one field per line, and `build.ninja`'s link statements and `COMMAND =` lines. Plain regular
# expressions rather than string(JSON), because string(JSON GET) re-parses the whole document per
# call and a ~2 MB file with a thousand entries would take minutes that way.

cmake_minimum_required(VERSION 3.28)

# Any instruction-set flag, not only this baseline's: a host tool may carry none of them.
set(_isa_regex "(^| )(/arch:[^ ]+|-march=[^ ]+|-mavx[^ ]*|-mfma|-mbmi2?|-mf16c|-mlzcnt|-mmovbe)( |$)")

# The classification itself, over plain lists, so the self-test can drive it with known input.
#   tu_targets / tu_commands  one entry per translation unit (commands with `;` already replaced)
# Every sentence it produces is free of `;`, because each one is a list element.
#   run_targets               the targets whose executables a build step runs
# Sets `violations` (a list of sentences) and `summary` in the caller's scope.
function(_engine_classify tu_targets tu_commands run_targets flags host_tools floor)
  set(_violations "")
  set(_baseline_tus 0)
  set(_host_tus 0)
  set(_floor_tus 0)
  set(_host_clean 0)
  set(_floor_clean 0)
  set(_seen_host "")
  list(LENGTH tu_targets _count)
  if(_count GREATER 0)
    math(EXPR _last "${_count} - 1")
    foreach(_i RANGE ${_last})
      list(GET tu_targets ${_i} _target)
      list(GET tu_commands ${_i} _command)
      if(_target IN_LIST host_tools OR _target IN_LIST floor)
        if(_target IN_LIST host_tools)
          math(EXPR _host_tus "${_host_tus} + 1")
          list(APPEND _seen_host ${_target})
          set(_kind "host tool")
        else()
          math(EXPR _floor_tus "${_floor_tus} + 1")
          set(_kind "floor target")
        endif()
        if(_command MATCHES "${_isa_regex}")
          string(STRIP "${CMAKE_MATCH_2}" _flag)
          list(APPEND _violations
            "${_kind} ${_target} compiles a translation unit with ${_flag}, and it may carry no instruction-set flag at all (ADR-0034)")
        elseif(_kind STREQUAL "host tool")
          math(EXPR _host_clean "${_host_clean} + 1")
        else()
          math(EXPR _floor_clean "${_floor_clean} + 1")
        endif()
      else()
        set(_missing "")
        foreach(_flag IN LISTS flags)
          string(REGEX REPLACE "([][+.*^$()|?\\\\])" "\\\\\\1" _escaped "${_flag}")
          if(NOT _command MATCHES "(^| )${_escaped}( |$)")
            list(APPEND _missing "${_flag}")
          endif()
        endforeach()
        if(_missing)
          string(REGEX MATCH "(^| )-c [^ ]+" _source "${_command}")
          list(APPEND _violations
            "${_target} compiles${_source} without the baseline's ${_missing} (ADR-0031: every shipped translation unit carries it)")
        else()
          math(EXPR _baseline_tus "${_baseline_tus} + 1")
        endif()
      endif()
    endforeach()
  endif()
  foreach(_tool IN LISTS host_tools)
    if(NOT _tool IN_LIST _seen_host)
      list(APPEND _violations
        "host tool ${_tool} has no translation unit in compile_commands.json, so nothing above checked it")
    endif()
  endforeach()
  list(REMOVE_DUPLICATES run_targets)
  foreach(_target IN LISTS run_targets)
    if(NOT _target IN_LIST host_tools)
      list(APPEND _violations
        "a build step runs ${_target}, which is not declared with engine_host_tool(): it carries the product's instruction set and will die with SIGILL on a build machine below the baseline (ADR-0034)")
    endif()
  endforeach()
  set(violations "${_violations}" PARENT_SCOPE)
  string(JOIN ", " _host_text ${host_tools})
  string(JOIN ", " _floor_text ${floor})
  string(JOIN ", " _run_text ${run_targets})
  string(JOIN " " _flag_text ${flags})
  if(NOT _flag_text)
    set(_flag_text "no flag (this compiler's default is the baseline)")
  endif()
  set(summary
    "${_count} translation units: ${_baseline_tus} carry ${_flag_text}, ${_host_clean} of ${_host_tus} host-tool [${_host_text}] and ${_floor_clean} of ${_floor_tus} floor [${_floor_text}] carry no instruction-set flag, and the build runs [${_run_text}]"
    PARENT_SCOPE)
endfunction()

# --- the self-test -------------------------------------------------------------------------------

if(SELF_TEST)
  set(_failures 0)
  macro(_expect name expected)
    list(LENGTH violations _n)
    if(NOT _n EQUAL ${expected})
      message(SEND_ERROR "self-test '${name}': expected ${expected} violation(s), got ${_n}: ${violations}")
      math(EXPR _failures "${_failures} + 1")
    else()
      message(STATUS "self-test '${name}': ${_n} violation(s), as expected")
    endif()
  endmacro()
  set(_tool "engine_schemac")
  set(_floor "engine_platform")

  _engine_classify("engine_schemac;engine_platform;engine_math;Jolt"
    "cl /O2 -c a.cpp;cl /O2 -c b.cpp;cl /O2 /arch:AVX2 -c c.cpp;cl /arch:AVX2 /W3 -c d.cpp"
    "engine_schemac" "/arch:AVX2" "${_tool}" "${_floor}")
  _expect("a correct v3 build" 0)

  _engine_classify("engine_schemac;engine_math" "cl /O2 /arch:AVX2 -c a.cpp;cl /arch:AVX2 -c c.cpp"
    "engine_schemac" "/arch:AVX2" "${_tool}" "${_floor}")
  _expect("a host tool carrying the baseline" 1)

  _engine_classify("engine_schemac;engine_math;engine_anim" "cl -c a.cpp;cl /O2 -c c.cpp;cl /arch:AVX2 -c d.cpp"
    "" "/arch:AVX2" "${_tool}" "${_floor}")
  _expect("an engine translation unit without it" 1)

  _engine_classify("engine_schemac;engine_math" "g++ -c a.cpp;g++ -march=x86-64-v3 -c c.cpp"
    "engine_schemac;engine_content" "-march=x86-64-v3" "${_tool}" "${_floor}")
  _expect("a build step running a shipped executable" 1)

  _engine_classify("engine_schemac;engine_math" "g++ -march=x86-64-v2 -c a.cpp;g++ -march=x86-64-v2 -c c.cpp"
    "" "-march=x86-64-v2" "${_tool}" "${_floor}")
  _expect("a host tool with the v2 flag on GCC" 1)

  _engine_classify("engine_schemac;engine_math" "g++ -c a.cpp;g++ -march=x86-64-v2 -c c.cpp"
    "" "-march=x86-64-v3" "${_tool}" "${_floor}")
  _expect("a flag that is a prefix match but not the token" 1)

  _engine_classify("engine_platform;engine_math" "cl /O2 -c a.cpp;cl /O2 -c c.cpp"
    "" "" "${_tool}" "${_floor}")
  _expect("MSVC at v2, where the baseline is no flag, and a host tool never compiled" 1)

  if(_failures GREATER 0)
    message(FATAL_ERROR "build.cpu_baseline.self_test: ${_failures} case(s) did not classify as expected")
  endif()
  message(STATUS "build.cpu_baseline.self_test: every case classified as expected")
  return()
endif()

# --- the real build -----------------------------------------------------------------------------

if(NOT BINARY_DIR)
  message(FATAL_ERROR "CheckCpuBaseline.cmake: BINARY_DIR is required (or SELF_TEST=ON)")
endif()
string(REPLACE "|" ";" _flags "${FLAGS}")
string(REPLACE "|" ";" _host_tools "${HOST_TOOLS}")
string(REPLACE "|" ";" _floor "${FLOOR}")

set(_cc "${BINARY_DIR}/compile_commands.json")
set(_ninja "${BINARY_DIR}/build.ninja")
foreach(_file IN ITEMS "${_cc}" "${_ninja}")
  if(NOT EXISTS "${_file}")
    message(FATAL_ERROR "build.cpu_baseline: ${_file} does not exist; configure and build first")
  endif()
endforeach()

# compile_commands.json: `"command"` then `"output"` per entry, one field per line. The target is
# the `CMakeFiles/<target>.dir/` directory its object file is written to.
file(READ "${_cc}" _text)
string(REPLACE ";" "<semicolon>" _text "${_text}")
# `[` and `]` change how CMake splits a list, and a command line may hold either.
string(REPLACE "[" "<lb>" _text "${_text}")
string(REPLACE "]" "<rb>" _text "${_text}")
string(REGEX MATCHALL "\n *\"(command|output)\": \"[^\n]*" _fields "${_text}")
set(_tu_targets "")
set(_tu_commands "")
set(_pending "")
foreach(_field IN LISTS _fields)
  if(_field MATCHES "^\n *\"command\": \"(.*)\",?$")
    set(_pending "${CMAKE_MATCH_1}")
  elseif(_field MATCHES "CMakeFiles[/\\\\]+([^/\\\\]+)\\.dir[/\\\\]")
    list(APPEND _tu_targets "${CMAKE_MATCH_1}")
    list(APPEND _tu_commands "${_pending}")
    set(_pending "")
  endif()
endforeach()

# build.ninja: which executable each target links, and which of them a build step runs.
file(READ "${_ninja}" _ninja_text)
string(REPLACE ";" "<semicolon>" _ninja_text "${_ninja_text}")
string(REPLACE "[" "<lb>" _ninja_text "${_ninja_text}")
string(REPLACE "]" "<rb>" _ninja_text "${_ninja_text}")
string(REGEX MATCHALL "\nbuild [^\n:]*(\\$:[^\n:]*)*: C(XX)?_EXECUTABLE_LINKER__[^ \n]+" _links "${_ninja_text}")
string(REGEX MATCHALL "\n  COMMAND = [^\n]*" _commands "${_ninja_text}")
string(JOIN "\n" _all_commands ${_commands})
string(REPLACE "\\" "/" _all_commands "${_all_commands}")
file(TO_CMAKE_PATH "${BINARY_DIR}" _root)
set(_run_targets "")
foreach(_link IN LISTS _links)
  if(NOT _link MATCHES "^\nbuild (.*): C(XX)?_EXECUTABLE_LINKER__(.+)_(Debug|Release|RelWithDebInfo|MinSizeRel)$")
    continue()
  endif()
  set(_exe "${CMAKE_MATCH_1}")
  set(_target "${CMAKE_MATCH_3}")
  string(REPLACE "$:" ":" _exe "${_exe}")
  string(REPLACE "$ " " " _exe "${_exe}")
  string(REPLACE "\\" "/" _exe "${_exe}")
  if(NOT IS_ABSOLUTE "${_exe}")
    set(_exe "${_root}/${_exe}")
  endif()
  # Followed by a space, a quote or the end of the line, so `bin/engine-cli` is not found inside
  # `bin/engine-cli-something`.
  foreach(_end IN ITEMS " " "\"" "\n")
    string(FIND "${_all_commands}\n" "${_exe}${_end}" _at)
    if(_at GREATER -1)
      list(APPEND _run_targets "${_target}")
      break()
    endif()
  endforeach()
endforeach()

_engine_classify("${_tu_targets}" "${_tu_commands}" "${_run_targets}" "${_flags}" "${_host_tools}"
                 "${_floor}")
message(STATUS "build.cpu_baseline (${BASELINE}): ${summary}")
if(violations)
  list(LENGTH violations _n)
  set(_shown 0)
  foreach(_v IN LISTS violations)
    if(_shown LESS 20)
      string(REPLACE "<semicolon>" "\\;" _v "${_v}")
      string(REPLACE "<lb>" "[" _v "${_v}")
      string(REPLACE "<rb>" "]" _v "${_v}")
      message(STATUS "  ${_v}")
    endif()
    math(EXPR _shown "${_shown} + 1")
  endforeach()
  message(FATAL_ERROR "build.cpu_baseline: ${_n} violation(s); cmake/EngineCpuBaseline.cmake and ADR-0034 say who carries the baseline")
endif()
