# Module declaration and layering enforcement.
#
#   engine_module(NAME <name> LAYER <layer>
#                 [DEPS <module>...]           # other engine modules, by NAME
#                 [EXTERNAL_DEPS <target>...]  # third-party targets
#                 [SOURCES <file>...])         # omit for header-only modules
#
#   engine_module_tests(NAME <name> SOURCES <file>...)
#
#   engine_finalize_modules()   # writes ${CMAKE_BINARY_DIR}/modules.json
#
# Layers are ordered. A module may depend only on modules in the same or a
# lower layer, and only on modules declared earlier (so cycles are impossible).
# The alias engine::<name> is the target other modules link against.

set(ENGINE_LAYERS core foundation domain systems apps game)

define_property(GLOBAL PROPERTY ENGINE_MODULES
  BRIEF_DOCS "Declared engine modules" FULL_DOCS "Declared engine modules, in declaration order")
set_property(GLOBAL PROPERTY ENGINE_MODULES "")

function(_engine_layer_index layer out_var)
  list(FIND ENGINE_LAYERS "${layer}" _idx)
  if(_idx EQUAL -1)
    message(FATAL_ERROR "engine_module: unknown layer '${layer}'. Known layers: ${ENGINE_LAYERS}")
  endif()
  set(${out_var} ${_idx} PARENT_SCOPE)
endfunction()

function(engine_module)
  set(_options "")
  set(_one NAME LAYER)
  set(_multi DEPS EXTERNAL_DEPS SOURCES)
  cmake_parse_arguments(EM "${_options}" "${_one}" "${_multi}" ${ARGN})

  if(NOT EM_NAME)
    message(FATAL_ERROR "engine_module: NAME is required")
  endif()
  if(NOT EM_LAYER)
    message(FATAL_ERROR "engine_module(${EM_NAME}): LAYER is required")
  endif()
  if(EM_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "engine_module(${EM_NAME}): unexpected arguments: ${EM_UNPARSED_ARGUMENTS}")
  endif()

  _engine_layer_index("${EM_LAYER}" _layer_idx)

  get_property(_declared GLOBAL PROPERTY ENGINE_MODULES)
  if("${EM_NAME}" IN_LIST _declared)
    message(FATAL_ERROR "engine_module(${EM_NAME}): module declared twice")
  endif()

  set(_target engine_${EM_NAME})

  # Validate dependencies before creating the target so errors are clear.
  set(_dep_targets "")
  foreach(_dep IN LISTS EM_DEPS)
    if(NOT "${_dep}" IN_LIST _declared)
      message(FATAL_ERROR
        "engine_module(${EM_NAME}) [${EM_LAYER}]: depends on '${_dep}', which is not declared yet. "
        "Modules must be declared lower layers first; a dependency on a higher layer is a layering violation.")
    endif()
    get_property(_dep_layer TARGET engine_${_dep} PROPERTY ENGINE_LAYER)
    _engine_layer_index("${_dep_layer}" _dep_idx)
    if(_dep_idx GREATER _layer_idx)
      message(FATAL_ERROR
        "engine_module(${EM_NAME}) [${EM_LAYER}]: depends on '${_dep}' [${_dep_layer}], which is a higher layer. "
        "See docs/plan/02-architecture.md section 2.3.")
    endif()
    list(APPEND _dep_targets engine::${_dep})
  endforeach()

  if(EM_SOURCES)
    add_library(${_target} STATIC ${EM_SOURCES})
    target_include_directories(${_target}
      PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
      PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${_target} PUBLIC ${_dep_targets} ${EM_EXTERNAL_DEPS})
    target_compile_options(${_target} PRIVATE ${ENGINE_NO_EXCEPTIONS_FLAGS})
    engine_apply_warnings(${_target})
  else()
    add_library(${_target} INTERFACE)
    target_include_directories(${_target} INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_link_libraries(${_target} INTERFACE ${_dep_targets} ${EM_EXTERNAL_DEPS})
  endif()
  add_library(engine::${EM_NAME} ALIAS ${_target})

  set_target_properties(${_target} PROPERTIES
    ENGINE_LAYER "${EM_LAYER}"
    ENGINE_MODULE_NAME "${EM_NAME}"
    ENGINE_MODULE_DEPS "${EM_DEPS}"
    ENGINE_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")

  set_property(GLOBAL APPEND PROPERTY ENGINE_MODULES "${EM_NAME}")
  message(STATUS "engine module: ${EM_NAME} [${EM_LAYER}] deps: ${EM_DEPS}")
endfunction()

#   engine_module_tests(NAME <name> SOURCES <file>... [DEPS <module>...])
# DEPS are test-only dependencies (other engine modules the tests use but the module does not).
function(engine_module_tests)
  set(_one NAME)
  set(_multi SOURCES DEPS)
  cmake_parse_arguments(ET "" "${_one}" "${_multi}" ${ARGN})
  if(NOT ENGINE_BUILD_TESTS)
    return()
  endif()
  if(NOT ET_NAME OR NOT ET_SOURCES)
    message(FATAL_ERROR "engine_module_tests: NAME and SOURCES are required")
  endif()
  if(NOT TARGET engine_${ET_NAME})
    message(FATAL_ERROR "engine_module_tests(${ET_NAME}): declare the module with engine_module() first")
  endif()
  set(_extra "")
  foreach(_dep IN LISTS ET_DEPS)
    if(NOT TARGET engine_${_dep})
      message(FATAL_ERROR "engine_module_tests(${ET_NAME}): test dependency '${_dep}' is not declared")
    endif()
    list(APPEND _extra engine::${_dep})
  endforeach()

  set(_test_target engine_${ET_NAME}_tests)
  add_executable(${_test_target} ${ET_SOURCES})
  target_link_libraries(${_test_target} PRIVATE engine::${ET_NAME} ${_extra} engine_test_main)
  engine_apply_warnings(${_test_target})
  add_test(NAME ${ET_NAME} COMMAND ${_test_target})
  set_tests_properties(${ET_NAME} PROPERTIES LABELS "unit;${ET_NAME}")
endfunction()

function(engine_finalize_modules)
  get_property(_modules GLOBAL PROPERTY ENGINE_MODULES)
  set(_json "{\n  \"layers\": [")
  set(_first TRUE)
  foreach(_layer IN LISTS ENGINE_LAYERS)
    if(NOT _first)
      string(APPEND _json ", ")
    endif()
    string(APPEND _json "\"${_layer}\"")
    set(_first FALSE)
  endforeach()
  string(APPEND _json "],\n  \"modules\": [\n")

  set(_first TRUE)
  foreach(_m IN LISTS _modules)
    get_property(_layer TARGET engine_${_m} PROPERTY ENGINE_LAYER)
    get_property(_deps  TARGET engine_${_m} PROPERTY ENGINE_MODULE_DEPS)
    get_property(_dir   TARGET engine_${_m} PROPERTY ENGINE_MODULE_DIR)
    file(RELATIVE_PATH _rel "${CMAKE_SOURCE_DIR}" "${_dir}")
    set(_deps_json "")
    set(_dfirst TRUE)
    foreach(_d IN LISTS _deps)
      if(NOT _dfirst)
        string(APPEND _deps_json ", ")
      endif()
      string(APPEND _deps_json "\"${_d}\"")
      set(_dfirst FALSE)
    endforeach()
    if(NOT _first)
      string(APPEND _json ",\n")
    endif()
    string(APPEND _json
      "    {\"name\": \"${_m}\", \"layer\": \"${_layer}\", \"path\": \"${_rel}\", \"deps\": [${_deps_json}]}")
    set(_first FALSE)
  endforeach()
  string(APPEND _json "\n  ]\n}\n")
  file(WRITE "${CMAKE_BINARY_DIR}/modules.json" "${_json}")
  message(STATUS "engine modules: wrote ${CMAKE_BINARY_DIR}/modules.json")
endfunction()
