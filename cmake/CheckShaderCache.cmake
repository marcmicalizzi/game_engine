# tools.shader_cache: the shader compile cache's key, ingredient by ingredient (roadmap T76;
# docs/ci/what-to-run.md, "The shader cache"; tools/shader_meter/src/shader_cache.h).
#
#   cmake -DWORK=<directory> -DMETER=<shader-meter> -DSLANGC=<slangc> -P cmake/CheckShaderCache.cmake
#
# Two copies of a small shader tree (a source, an include found through -I) stand in for two
# checkouts, and a store of the test's own under WORK stands in for the machine's. Each case runs
# the meter as engine_shaders() does and reads the cost file's last line, which says whether the
# compile was served (`cache hit`), compiled and stored (`cache stored`), compiled and refused
# (`cache not stored: <why>`), or compiled with the cache off (no such line). The key must be
# complete, so every ingredient is changed once and each change must miss: the source, a file it
# includes, a flag, the compiler, a file that would now shadow an include. A hit must be the
# bytes slangc makes with no cache at all, from another checkout's path. And the rest: the switch,
# a failing compile, an output that names its own directory, two compiles storing one key at once,
# the limit, the depfile reader and SHA-256 itself.

cmake_minimum_required(VERSION 3.28)

foreach(_required IN ITEMS WORK METER SLANGC)
  if(NOT ${_required})
    message(FATAL_ERROR "tools.shader_cache: -D${_required}=... is required")
  endif()
endforeach()

# Forward slashes, since _expect's condition is parsed as code, where a backslash is an escape.
file(TO_CMAKE_PATH "${WORK}" WORK)
string(RANDOM LENGTH 12 ALPHABET "0123456789abcdef" _id)
set(_dir "${WORK}/shader-cache-${_id}")
set(_store "${_dir}/store")
file(MAKE_DIRECTORY "${_store}")
set(_failures 0)
macro(_expect what condition)
  cmake_language(EVAL CODE "if(${condition})\nset(_ok TRUE)\nelse()\nset(_ok FALSE)\nendif()")
  if(_ok)
    message(STATUS "  ok   ${what}")
  else()
    message(STATUS "  FAIL ${what}")
    math(EXPR _failures "${_failures} + 1")
  endif()
endmacro()

# --- SHA-256 and the depfile reader, before anything rests on them --------------------------------

function(_tool_sha256 file out_var)
  execute_process(COMMAND "${METER}" --sha256 "${file}" OUTPUT_VARIABLE _out RESULT_VARIABLE _code
                  OUTPUT_STRIP_TRAILING_WHITESPACE)
  set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()
# FIPS 180-4's examples, and a file of several blocks against CMake's own SHA-256.
file(WRITE "${_dir}/empty.txt" "")
file(WRITE "${_dir}/abc.txt" "abc")
file(WRITE "${_dir}/two-blocks.txt" "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
string(REPEAT "the shader cache's key; " 300 _long)
file(WRITE "${_dir}/long.txt" "${_long}")
_tool_sha256("${_dir}/empty.txt" _h0)
_tool_sha256("${_dir}/abc.txt" _h1)
_tool_sha256("${_dir}/two-blocks.txt" _h2)
_tool_sha256("${_dir}/long.txt" _h3)
file(SHA256 "${_dir}/long.txt" _h3_cmake)
_expect("SHA-256 of nothing" "\"${_h0}\" STREQUAL \"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\"")
_expect("SHA-256 of 'abc'" "\"${_h1}\" STREQUAL \"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad\"")
_expect("SHA-256 of the 448-bit message" "\"${_h2}\" STREQUAL \"248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1\"")
_expect("SHA-256 of 7,200 bytes is CMake's" "\"${_h3}\" STREQUAL \"${_h3_cmake}\"")

# slangc's own form on Windows (every backslash and drive colon escaped) and the plain one.
file(WRITE "${_dir}/windows.d" "C\\:\\\\out\\\\x.spv: D\\:\\\\a\\ b\\\\main.slang D\\:\\\\a\\ b\\\\inc\\\\common.slang\n")
file(WRITE "${_dir}/posix.d" "/out/x.spv: /src/a/main.slang \\\n  /src/a/inc/common.slang\n")
execute_process(COMMAND "${METER}" --parse-depfile "${_dir}/windows.d" OUTPUT_VARIABLE _w)
execute_process(COMMAND "${METER}" --parse-depfile "${_dir}/posix.d" OUTPUT_VARIABLE _p)
# Compared here rather than in _expect, whose condition is parsed again as code, where a backslash
# in the value would be read as an escape.
string(REPLACE "\\" "/" _w "${_w}")
string(REPLACE "\r" "" _w "${_w}")
string(REPLACE "\r" "" _p "${_p}")
set(_w_ok FALSE)
if(_w STREQUAL "D:/a b/main.slang\nD:/a b/inc/common.slang\n")
  set(_w_ok TRUE)
endif()
set(_p_ok FALSE)
if(_p STREQUAL "/src/a/main.slang\n/src/a/inc/common.slang\n")
  set(_p_ok TRUE)
endif()
_expect("a Windows depfile reads as two paths, a space in one, its escapes undone" "_w_ok")
_expect("a POSIX depfile with a continued line reads as two paths" "_p_ok")

# --- two checkouts of one small tree --------------------------------------------------------------

function(_write_tree root value)
  file(WRITE "${root}/x/shaders/main.slang"
    "// A source that includes a file found through -I.\n#include \"common.slang\"\nRWStructuredBuffer<uint> g_out;\n[shader(\"compute\")]\n[numthreads(1, 1, 1)]\nvoid main_cs(uint3 id : SV_DispatchThreadID) { g_out[id.x] = value(); }\n")
  file(WRITE "${root}/x/inc/common.slang" "uint value() { return ${value}u; }\n")
endfunction()
_write_tree("${_dir}/a" 1)
_write_tree("${_dir}/b" 1)

# One compile as engine_shaders() runs it. MODE: on (the cache), off (ENGINE_SHADER_CACHE=0), none
# (no --cache at all: the build with the cache unset). Sets <prefix>_code, _line (the cost file's
# third line, or "" when it has none), _hash (the SPIR-V's) and _err.
function(_compile prefix checkout out mode)
  cmake_parse_arguments(C "" "COMPILER;MAX_MB" "FLAGS" ${ARGN})
  if(NOT C_COMPILER)
    set(C_COMPILER "compiler-one")
  endif()
  if(NOT DEFINED C_MAX_MB)   # 0 is a limit, and false to if()
    set(C_MAX_MB 4096)
  endif()
  set(_src "${checkout}/x/shaders/main.slang")
  set(_spv "${_dir}/out/${out}.spv")
  set(_cost "${_dir}/out/${out}.cost")
  file(MAKE_DIRECTORY "${_dir}/out")
  file(REMOVE "${_spv}" "${_cost}")
  set(_cache --cache "${_store}" --compiler "${C_COMPILER}" --source "${_src}" --output "${_spv}")
  set(_switch 1)
  if(mode STREQUAL "none")
    set(_cache "")
  elseif(mode STREQUAL "off")
    set(_switch 0)
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env "ENGINE_SHADER_CACHE=${_switch}" "ENGINE_SHADER_CACHE_MAX_MB=${C_MAX_MB}"
            "${METER}" ${_cache} "${_cost}" "${SLANGC}" "${_src}" -target spirv -profile spirv_1_6
            -emit-spirv-directly -fvk-use-entrypoint-name -O2 -warnings-disable 41012
            -I "${checkout}/x/inc" ${C_FLAGS} -o "${_spv}"
    RESULT_VARIABLE _code OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
  set(_line "")
  if(EXISTS "${_cost}")
    file(STRINGS "${_cost}" _lines)
    list(LENGTH _lines _n)
    if(_n GREATER 2)
      list(GET _lines 2 _line)
    endif()
  endif()
  set(_hash "")
  if(EXISTS "${_spv}")
    file(SHA256 "${_spv}" _hash)
  endif()
  set(${prefix}_code "${_code}" PARENT_SCOPE)
  set(${prefix}_line "${_line}" PARENT_SCOPE)
  set(${prefix}_hash "${_hash}" PARENT_SCOPE)
  set(${prefix}_err "${_err}" PARENT_SCOPE)
endfunction()

function(_count_files dir out_var)
  file(GLOB_RECURSE _all LIST_DIRECTORIES false "${dir}/*")
  list(LENGTH _all _n)
  set(${out_var} "${_n}" PARENT_SCOPE)
endfunction()

_compile(first "${_dir}/a" a1 on)
_expect("a first compile succeeds and is stored (${first_code}, '${first_line}')"
        "first_code EQUAL 0 AND \"${first_line}\" STREQUAL \"cache stored\"")
if(NOT first_code EQUAL 0)
  message(STATUS "  the first compile said: ${first_err}")
endif()
_compile(plain "${_dir}/b" plain none)
_compile(other "${_dir}/b" b1 on)
_expect("the same compile in another checkout's path is served (${other_code}, '${other_line}')"
        "other_code EQUAL 0 AND \"${other_line}\" STREQUAL \"cache hit\"")
_expect("what it serves is the bytes slangc makes with no cache, and what it stored"
        "NOT \"${plain_hash}\" STREQUAL \"\" AND \"${other_hash}\" STREQUAL \"${plain_hash}\" AND \"${first_hash}\" STREQUAL \"${plain_hash}\"")

# Every ingredient of the key, changed once: each must miss and compile.
file(WRITE "${_dir}/b/x/inc/common.slang" "uint value() { return 2u; }\n")
_compile(include "${_dir}/b" b2 on)
_compile(include_plain "${_dir}/b" b2plain none)
_expect("an edit to an included file misses (${include_line})" "\"${include_line}\" STREQUAL \"cache stored\"")
_expect("and its SPIR-V is the edited compile's" "\"${include_hash}\" STREQUAL \"${include_plain_hash}\" AND NOT \"${include_hash}\" STREQUAL \"${plain_hash}\"")
_compile(back "${_dir}/a" a2 on)
_expect("the first checkout, unchanged, is still served its own (${back_line})"
        "\"${back_line}\" STREQUAL \"cache hit\" AND \"${back_hash}\" STREQUAL \"${first_hash}\"")
file(APPEND "${_dir}/b/x/shaders/main.slang" "// an edit to the source\n")
_compile(source "${_dir}/b" b3 on)
_expect("an edit to the source misses (${source_line})" "\"${source_line}\" STREQUAL \"cache stored\"")
_compile(flag "${_dir}/a" a3 on FLAGS -DENGINE_PROBE=1)
_expect("a changed flag misses (${flag_line})" "\"${flag_line}\" STREQUAL \"cache stored\"")
_compile(compiler "${_dir}/a" a4 on COMPILER compiler-two)
_expect("a changed compiler misses (${compiler_line})" "\"${compiler_line}\" STREQUAL \"cache stored\"")
file(WRITE "${_dir}/a/x/shaders/common.slang" "uint value() { return 3u; }\n")
_compile(shadow "${_dir}/a" a5 on)
_compile(shadow_plain "${_dir}/a" a5plain none)
_expect("a file that now shadows an include misses (${shadow_line})" "\"${shadow_line}\" STREQUAL \"cache stored\"")
_expect("and slangc read the shadow, so a hit there would have been a wrong picture"
        "\"${shadow_hash}\" STREQUAL \"${shadow_plain_hash}\" AND NOT \"${shadow_hash}\" STREQUAL \"${first_hash}\"")
file(REMOVE "${_dir}/a/x/shaders/common.slang")
_compile(unshadowed "${_dir}/a" a6 on)
_expect("the shadow gone, the first compile is served again (${unshadowed_line})"
        "\"${unshadowed_line}\" STREQUAL \"cache hit\" AND \"${unshadowed_hash}\" STREQUAL \"${first_hash}\"")

# The switch: ENGINE_SHADER_CACHE=0 compiles, serves nothing and stores nothing.
_count_files("${_store}" _before)
_compile(off "${_dir}/a" a7 off FLAGS -DENGINE_PROBE=7)
_count_files("${_store}" _after)
_expect("ENGINE_SHADER_CACHE=0 compiles (${off_code}) with no cache line ('${off_line}') and stores nothing (${_before} files, then ${_after})"
        "off_code EQUAL 0 AND \"${off_line}\" STREQUAL \"\" AND _before EQUAL _after")
_compile(off_again "${_dir}/a" a8 on FLAGS -DENGINE_PROBE=7)
_expect("so the same compile with the cache on is a miss (${off_again_line})" "\"${off_again_line}\" STREQUAL \"cache stored\"")

# A failing compile: its exit code, no measurement, nothing stored, and never served afterwards.
file(WRITE "${_dir}/b/x/shaders/main.slang" "this is not slang\n")
_compile(broken "${_dir}/b" b4 on)
_expect("a failing compile fails (${broken_code}) with no cost file and no SPIR-V"
        "NOT broken_code EQUAL 0 AND \"${broken_hash}\" STREQUAL \"\" AND NOT EXISTS \"${_dir}/out/b4.cost\"")
_compile(broken_again "${_dir}/b" b5 on)
_expect("and fails again rather than being served (${broken_again_code})" "NOT broken_again_code EQUAL 0")

# An output that names the directory it was compiled in (-g writes the source's path) differs
# between checkouts, so it is compiled and never stored.
_compile(debug "${_dir}/a" a9 on FLAGS -g)
_compile(debug_again "${_dir}/a" a10 on FLAGS -g)
_expect("a SPIR-V naming its own directory is not stored ('${debug_line}', then '${debug_again_line}')"
        "debug_code EQUAL 0 AND \"${debug_line}\" MATCHES \"^cache not stored: the SPIR-V names\" AND \"${debug_again_line}\" MATCHES \"^cache not stored\"")

# Two builds storing one key at once (execute_process runs its commands together): both succeed
# with the same bytes, and the next is served.
set(_src "${_dir}/a/x/shaders/main.slang")
foreach(_n IN ITEMS 1 2)
  set(_c${_n} "${METER}" --cache "${_store}" --compiler compiler-one --source "${_src}"
      --output "${_dir}/out/race${_n}.spv" "${_dir}/out/race${_n}.cost" "${SLANGC}" "${_src}" -target spirv
      -profile spirv_1_6 -emit-spirv-directly -fvk-use-entrypoint-name -O2 -warnings-disable 41012
      -I "${_dir}/a/x/inc" -DENGINE_PROBE=race -o "${_dir}/out/race${_n}.spv")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E env ENGINE_SHADER_CACHE=1 ${_c1}
                COMMAND "${CMAKE_COMMAND}" -E env ENGINE_SHADER_CACHE=1 ${_c2}
                RESULTS_VARIABLE _codes OUTPUT_QUIET ERROR_QUIET)
file(SHA256 "${_dir}/out/race1.spv" _r1)
file(SHA256 "${_dir}/out/race2.spv" _r2)
_compile(after_race "${_dir}/a" race3 on FLAGS -DENGINE_PROBE=race)
_expect("two compiles of one key at once both succeed (${_codes}) with one SPIR-V, and the next is served (${after_race_line})"
        "\"${_codes}\" STREQUAL \"0;0\" AND \"${_r1}\" STREQUAL \"${_r2}\" AND \"${after_race_line}\" STREQUAL \"cache hit\" AND \"${after_race_hash}\" STREQUAL \"${_r1}\"")

# The limit: a store over it drops its least recently used files at the next store's walk.
_compile(limited "${_dir}/a" a11 on FLAGS -DENGINE_PROBE=limit MAX_MB 0)
file(GLOB_RECURSE _results LIST_DIRECTORIES false "${_store}/v1/r/*")
list(LENGTH _results _kept)
_expect("a store over its limit is emptied down to it (${limited_line}, ${_kept} results left)"
        "limited_code EQUAL 0 AND _kept EQUAL 0")
_compile(refilled "${_dir}/a" a12 on FLAGS -DENGINE_PROBE=limit)
_expect("and what it dropped is compiled again (${refilled_line})" "\"${refilled_line}\" STREQUAL \"cache stored\"")

file(REMOVE_RECURSE "${_dir}")
if(_failures GREATER 0)
  message(FATAL_ERROR "tools.shader_cache: ${_failures} case(s) did not come out as expected")
endif()
message(STATUS "tools.shader_cache: every case came out as expected")
