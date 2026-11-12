# The parts' deal and the cost table it reads, as functions that need no project: the configure
# calls them (cmake/EngineTesting.cmake reads the table, _engine_test_parts in
# cmake/EngineModule.cmake deals), and so do `tools/test-costs.ps1 -Predict` and the tool's test,
# in script mode (`cmake -P`), so the deal a prediction or a fixture shows is the configure's own
# and not a second copy of it (roadmap T88; docs/ci/what-to-run.md, "Parts dealt by cost").

#   engine_read_test_costs(<file>)
# Reads cmake/test-costs.txt into the global properties ENGINE_TEST_COST:<test>:<basename>. One
# line a file, `<test> <basename> <whole seconds>`; `#` starts a comment, so a row may end in one
# (tools/test-costs.ps1 writes where the number came from there). A line of any other shape stops
# the configure. A missing file is an empty table.
function(engine_read_test_costs file)
  if(NOT EXISTS "${file}")
    return()
  endif()
  file(STRINGS "${file}" _lines)
  foreach(_line IN LISTS _lines)
    string(REGEX REPLACE "#.*$" "" _line "${_line}")
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "")
      continue()
    endif()
    if(NOT _line MATCHES "^([A-Za-z0-9_.]+)[ \t]+([A-Za-z0-9_.+-]+)[ \t]+([0-9]+)$")
      message(FATAL_ERROR "${file}: '${_line}' is not `<test> <basename> <whole seconds>`")
    endif()
    set_property(GLOBAL PROPERTY "ENGINE_TEST_COST:${CMAKE_MATCH_1}:${CMAKE_MATCH_2}" "${CMAKE_MATCH_3}")
  endforeach()
endfunction()

#   engine_deal_test_files(<test> <parts> <basenames> <out>)
# Deals <basenames> (a list) over <parts> parts and sets, in the caller's scope, <out>_<k> (the
# basenames of part k, from 0) and <out>_load_<k> (the table's seconds over them). The files the
# table gives seconds for go first, longest first (equal costs by name), each to the part with the
# least so far, the lowest-numbered of equal ones: longest-processing-time first, within a third of
# the best possible deal. The files it does not name follow in name order, round the parts from the
# first, adding nothing to a load; with an empty table that is the name-order deal every part had
# before the table.
function(engine_deal_test_files test parts names out)
  set(_names ${names})
  list(REMOVE_DUPLICATES _names)
  list(SORT _names)

  # The costed files as "<seconds, zero-padded>|<name>", so a string sort is a numeric one; sorted
  # ascending and then regrouped, highest cost first and each cost's names ascending.
  set(_costed "")
  set(_uncosted "")
  foreach(_base IN LISTS _names)
    get_property(_cost GLOBAL PROPERTY "ENGINE_TEST_COST:${test}:${_base}")
    if("${_cost}" STREQUAL "")
      list(APPEND _uncosted "${_base}")
    else()
      string(LENGTH "${_cost}" _digits)
      set(_padded "${_cost}")
      while(_digits LESS 9)
        string(PREPEND _padded "0")
        math(EXPR _digits "${_digits} + 1")
      endwhile()
      list(APPEND _costed "${_padded}|${_base}")
    endif()
  endforeach()
  list(SORT _costed)
  set(_by_cost "")
  set(_group "")
  set(_group_cost "")
  foreach(_entry IN LISTS _costed)
    string(REPLACE "|" ";" _pair "${_entry}")
    list(GET _pair 0 _c)
    if(NOT "${_c}" STREQUAL "${_group_cost}")
      list(PREPEND _by_cost ${_group})
      set(_group "")
      set(_group_cost "${_c}")
    endif()
    list(APPEND _group "${_entry}")
  endforeach()
  list(PREPEND _by_cost ${_group})

  math(EXPR _last "${parts} - 1")
  foreach(_k RANGE 0 ${_last})
    set(_part_${_k} "")
    set(_load_${_k} 0)
  endforeach()
  foreach(_entry IN LISTS _by_cost)
    string(REPLACE "|" ";" _pair "${_entry}")
    list(GET _pair 0 _cost)
    list(GET _pair 1 _base)
    math(EXPR _cost "${_cost} + 0")   # drops the padding
    set(_best 0)
    foreach(_k RANGE 1 ${_last})
      if(_load_${_k} LESS _load_${_best})
        set(_best ${_k})
      endif()
    endforeach()
    list(APPEND _part_${_best} "${_base}")
    math(EXPR _load_${_best} "${_load_${_best}} + ${_cost}")
  endforeach()
  set(_i 0)
  foreach(_base IN LISTS _uncosted)
    math(EXPR _k "${_i} % ${parts}")
    list(APPEND _part_${_k} "${_base}")
    math(EXPR _i "${_i} + 1")
  endforeach()

  foreach(_k RANGE 0 ${_last})
    set(${out}_${_k} "${_part_${_k}}" PARENT_SCOPE)
    set(${out}_load_${_k} "${_load_${_k}}" PARENT_SCOPE)
  endforeach()
endfunction()
