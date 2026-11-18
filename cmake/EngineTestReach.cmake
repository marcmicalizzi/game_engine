# What a module's tests can reach (roadmap T31; docs/ci/what-to-run.md, "What affected means").
#
#   _engine_test_reach_prepare()                 # once, before the first call below
#   _engine_test_reach(<module> <out_json>)      # the module's `test_reach` object for modules.json
#
# `tools/dev.ps1 test -Affected` selects a module's tests when a change can reach them. The module
# graph alone (ENGINE_MODULE_DEPS) does not say what a test target links: a test links the
# capabilities it exercises without its module depending on them (renderer's tests link sky and
# terrain), and starts the executables it drives (engine_cli's tests start engine-host, engine-view
# and engine-content; engine_view's start the game modules). Before this, the affected rule made
# up for it by selecting everything above a changed capability's layer. This walks the targets
# themselves instead, after every directory has declared them, so the answer is what the build
# links and builds, not what a list here remembers:
#
#   - the start: the module's test executable (engine_<m>_tests), its bench (engine_<m>_bench),
#     and, for an app, the app itself (its `<app>.<part>` tests and scripted tests run it);
#   - the edges: LINK_LIBRARIES and INTERFACE_LINK_LIBRARIES (generator expressions read for the
#     targets they name, so a whole-archive module's `$<LINK_LIBRARY:WHOLE_ARCHIVE,engine_x_impl>`
#     counts), MANUALLY_ADDED_DEPENDENCIES (add_dependencies: the executables a test starts, which
#     is how this tree says so) and COMPILE_DEFINITIONS (a `$<TARGET_FILE:...>` a test is compiled
#     with is an executable it runs), followed to the end; an imported target is a leaf.
#
# What it writes, for each module:
#   modules  every engine module (or app) a target of that closure is, or is the _impl of, and
#            every module whose directory holds a source it compiles, an include directory it
#            reads, or (for a target that is no module's, a custom target) the CMakeLists that
#            declared it: engine-editor compiles engine-view's camera, water's tests include the
#            renderer's test headers, engine-host's shell copy is engine-host's;
#   files    every source file of the closure's targets outside its own target's directory and
#            outside every module's (tests/support/test_main.cpp, apps/tests/...): a change to one
#            reaches these tests and no module owns it;
#   dirs     the source directory of every target of the closure that is no module's, under the
#            source tree and outside the build tree (a game module: game/walk_poc).
# What a test reads at run time from the source tree (content/, tools/ scripts) is not a build
# edge; tools/lib/Affected.psm1 finds those from the sources' text when it runs, so an edit that
# adds a reference is seen without a configure.
#
# Each target is read once (_engine_test_reach_target, kept in global properties) and each
# module's closure is a walk over those: the tree has about a thousand targets and a library sits
# in a hundred closures, so reading its sources once per closure cost minutes of configure.

function(_engine_test_reach_prepare)
  get_property(_modules GLOBAL PROPERTY ENGINE_MODULES)
  set(_dirs "")
  set(_names "")
  foreach(_m IN LISTS _modules)
    get_property(_dir TARGET engine_${_m} PROPERTY ENGINE_MODULE_DIR)
    list(APPEND _dirs "${_dir}")
    list(APPEND _names "${_m}")
  endforeach()
  set_property(GLOBAL PROPERTY _ENGINE_TEST_REACH_MODULE_DIRS "${_dirs}")
  set_property(GLOBAL PROPERTY _ENGINE_TEST_REACH_MODULE_NAMES "${_names}")
endfunction()

# In <out>, the modules whose directory holds <path> (absolute): the deepest, and every module that
# shares it (a capability and its schema types). Empty when no module's does.
function(_engine_test_reach_in_module out path)
  get_property(_dirs GLOBAL PROPERTY _ENGINE_TEST_REACH_MODULE_DIRS)
  get_property(_names GLOBAL PROPERTY _ENGINE_TEST_REACH_MODULE_NAMES)
  set(_owners "")
  set(_best -1)
  set(_i 0)
  foreach(_dir IN LISTS _dirs)
    string(FIND "${path}/" "${_dir}/" _at)
    if(_at EQUAL 0)
      string(LENGTH "${_dir}" _n)
      list(GET _names ${_i} _name)
      if(_n GREATER _best)
        set(_best ${_n})
        set(_owners "${_name}")
      elseif(_n EQUAL _best)
        list(APPEND _owners "${_name}")
      endif()
    endif()
    math(EXPR _i "${_i} + 1")
  endforeach()
  set(${out} "${_owners}" PARENT_SCOPE)
endfunction()

# TRUE when <path> (absolute) is inside the source tree and outside the build tree.
function(_engine_test_reach_in_tree out path)
  string(FIND "${path}/" "${CMAKE_SOURCE_DIR}/" _in_source)
  string(FIND "${path}/" "${CMAKE_BINARY_DIR}/" _in_build)
  if(_in_source EQUAL 0 AND NOT _in_build EQUAL 0)
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()

# Reads one target once: the module it is, the files and directory it adds, and its edges.
function(_engine_test_reach_target t)
  get_property(_done GLOBAL PROPERTY "_ENGINE_TR_DONE:${t}")
  if(_done)
    return()
  endif()
  set_property(GLOBAL PROPERTY "_ENGINE_TR_DONE:${t}" TRUE)
  set(_module "")
  set(_files "")
  set(_dir "")
  set(_edges "")
  get_property(_imported TARGET ${t} PROPERTY IMPORTED)
  if(NOT _imported)
    # Which module the target is: a module's own target, or the archive behind a whole-archive one.
    get_property(_module TARGET ${t} PROPERTY ENGINE_MODULE_NAME)
    if(NOT _module AND t MATCHES "^engine_(.+)_impl$" AND TARGET engine_${CMAKE_MATCH_1})
      get_property(_module TARGET engine_${CMAKE_MATCH_1} PROPERTY ENGINE_MODULE_NAME)
    endif()
    # A target that is no module's own belongs to the module whose CMakeLists declared it (a
    # custom target: engine-host's copy of the shell's files), or to no module (a game module).
    get_property(_src_dir TARGET ${t} PROPERTY SOURCE_DIR)
    if(NOT _module AND NOT _src_dir STREQUAL CMAKE_SOURCE_DIR)
      _engine_test_reach_in_tree(_inside "${_src_dir}")
      if(_inside)
        _engine_test_reach_in_module(_owned "${_src_dir}")
        if(_owned)
          list(APPEND _module ${_owned})
        else()
          file(RELATIVE_PATH _dir "${CMAKE_SOURCE_DIR}" "${_src_dir}")
        endif()
      endif()
    endif()

    # Include directories in another module's directory (water's tests read the renderer's and
    # gfx's tests/): that module is read. tests/support is every test's and its headers are
    # placed by who includes them (tools/lib/Affected.psm1), the top of the tree by nothing.
    foreach(_prop IN ITEMS INCLUDE_DIRECTORIES INTERFACE_INCLUDE_DIRECTORIES)
      get_property(_incs TARGET ${t} PROPERTY ${_prop})
      foreach(_inc IN LISTS _incs)
        string(REGEX REPLACE "^\\$<BUILD_INTERFACE:(.*)>$" "\\1" _inc "${_inc}")
        if(_inc MATCHES "\\$<" OR NOT IS_ABSOLUTE "${_inc}" OR _inc STREQUAL CMAKE_SOURCE_DIR)
          continue()
        endif()
        _engine_test_reach_in_tree(_inside "${_inc}")
        if(_inside)
          _engine_test_reach_in_module(_owned "${_inc}")
          list(APPEND _module ${_owned})
        endif()
      endforeach()
    endforeach()

    # Sources from outside the target's own directory: the test main, a test file several hosts
    # share, another module's source compiled in again (engine-editor builds engine-view's camera).
    # Those inside it belong to whoever owns that directory already — unless that directory is the
    # top of the tree, which nothing owns (engine_test_main is declared there).
    set(_own "${_src_dir}")
    if(_src_dir STREQUAL CMAKE_SOURCE_DIR)
      set(_own "")
    endif()
    get_property(_sources TARGET ${t} PROPERTY SOURCES)
    foreach(_s IN LISTS _sources)
      if(_s MATCHES "\\$<")
        continue()
      endif()
      if(NOT IS_ABSOLUTE "${_s}")
        string(FIND "${_s}" "../" _up)
        if(_up EQUAL -1 AND _own)
          continue()   # relative and not climbing out: inside the target's own directory
        endif()
        set(_s "${_src_dir}/${_s}")
      endif()
      cmake_path(NORMAL_PATH _s)
      if(_own)
        string(FIND "${_s}" "${_own}/" _at)
        if(_at EQUAL 0)
          continue()
        endif()
      endif()
      _engine_test_reach_in_tree(_inside "${_s}")
      if(NOT _inside)
        continue()
      endif()
      _engine_test_reach_in_module(_owned "${_s}")
      if(_owned)
        list(APPEND _module ${_owned})
      else()
        file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_s}")
        list(APPEND _files "${_rel}")
      endif()
    endforeach()
    if(_module)
      list(REMOVE_DUPLICATES _module)
    endif()

    # The edges: what it links, what links through it, what it builds before itself, and an
    # executable whose path it is compiled with ($<TARGET_FILE:...> in a definition: it runs it).
    set(_items "")
    foreach(_prop IN ITEMS LINK_LIBRARIES INTERFACE_LINK_LIBRARIES MANUALLY_ADDED_DEPENDENCIES
                           COMPILE_DEFINITIONS)
      get_property(_v TARGET ${t} PROPERTY ${_prop})
      list(APPEND _items ${_v})
    endforeach()
    foreach(_item IN LISTS _items)
      # Words of a generator expression, keeping a namespace's `::` (engine::gfx) whole and
      # splitting at a lone `:` and `,` ($<LINK_LIBRARY:WHOLE_ARCHIVE,engine_x_impl>).
      string(REPLACE "::" "__ENGINE_NS__" _item "${_item}")
      string(REGEX MATCHALL "[A-Za-z0-9_.+-]+" _tokens "${_item}")
      list(TRANSFORM _tokens REPLACE "__ENGINE_NS__" "::")
      foreach(_token IN LISTS _tokens)
        if(TARGET "${_token}")
          get_property(_aliased TARGET "${_token}" PROPERTY ALIASED_TARGET)
          if(_aliased)
            set(_token "${_aliased}")
          endif()
          list(APPEND _edges "${_token}")
        endif()
      endforeach()
    endforeach()
    if(_edges)
      list(REMOVE_DUPLICATES _edges)
    endif()
  endif()
  set_property(GLOBAL PROPERTY "_ENGINE_TR_MODULE:${t}" "${_module}")
  set_property(GLOBAL PROPERTY "_ENGINE_TR_FILES:${t}" "${_files}")
  set_property(GLOBAL PROPERTY "_ENGINE_TR_DIR:${t}" "${_dir}")
  set_property(GLOBAL PROPERTY "_ENGINE_TR_EDGES:${t}" "${_edges}")
endfunction()

function(_engine_test_reach module out_json)
  set(_pending "")
  foreach(_t IN ITEMS engine_${module}_tests engine_${module}_bench)
    if(TARGET ${_t})
      list(APPEND _pending ${_t})
    endif()
  endforeach()
  get_property(_layer TARGET engine_${module} PROPERTY ENGINE_LAYER)
  if(_layer STREQUAL "apps")
    list(APPEND _pending engine_${module})
  endif()

  set(_seen "")
  set(_reach_modules "")
  set(_reach_files "")
  set(_reach_dirs "")
  while(_pending)
    list(POP_FRONT _pending _t)
    if(_t IN_LIST _seen)
      continue()
    endif()
    list(APPEND _seen ${_t})
    _engine_test_reach_target(${_t})
    get_property(_m GLOBAL PROPERTY "_ENGINE_TR_MODULE:${_t}")
    get_property(_f GLOBAL PROPERTY "_ENGINE_TR_FILES:${_t}")
    get_property(_d GLOBAL PROPERTY "_ENGINE_TR_DIR:${_t}")
    get_property(_e GLOBAL PROPERTY "_ENGINE_TR_EDGES:${_t}")
    list(APPEND _reach_modules ${_m})
    list(APPEND _reach_files ${_f})
    list(APPEND _reach_dirs ${_d})
    list(APPEND _pending ${_e})
  endwhile()

  foreach(_list IN ITEMS _reach_modules _reach_files _reach_dirs)
    if(${_list})
      list(REMOVE_DUPLICATES ${_list})
      list(SORT ${_list})
    endif()
    set(_quoted ${${_list}})
    if(_quoted)
      list(TRANSFORM _quoted PREPEND "\"")
      list(TRANSFORM _quoted APPEND "\"")
    endif()
    list(JOIN _quoted ", " ${_list}_json)
  endforeach()
  set(${out_json}
    "{\"modules\": [${_reach_modules_json}], \"files\": [${_reach_files_json}], \"dirs\": [${_reach_dirs_json}]}"
    PARENT_SCOPE)
endfunction()
