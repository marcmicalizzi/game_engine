# Module declaration and layering enforcement.
#
#   engine_module(NAME <name> LAYER <layer>
#                 [DEPS <module>...]           # other engine modules, by NAME
#                 [EXTERNAL_DEPS <target>...]  # third-party targets
#                 [SOURCES <file>...]          # omit for header-only modules
#                 [WHOLE_ARCHIVE]              # link every object even if unreferenced
#                 [OPTIONAL] [CAPABILITY <c>]) # an optional capability (ADR-0027)
#
# WHOLE_ARCHIVE is for modules whose objects register themselves during static
# initialization (generated schema types, a capability's system registration): a static library
# would otherwise drop object files nothing references, and the registrations with them. The
# module becomes an INTERFACE target wrapping engine_<name>_impl with the WHOLE_ARCHIVE feature.
#
# OPTIONAL marks the module as a capability (ADR-0027): it gets an ENGINE_WITH_<UPPER_NAME>
# option defaulting ON, and when that option is off — or when ENGINE_MINIMAL is on, which turns
# every capability off whatever the cache says — the module, its tests, and its bench are
# skipped and it is absent from modules.json. CAPABILITY names the switch when several modules
# share one (a capability plus its schema library); it implies OPTIONAL. Nothing foundational is
# ever optional: a foundation that can be switched off is one nobody can rely on.
#
#   engine_module_tests(NAME <name> SOURCES <file>...)
#   engine_capability_enabled(<capability> <out_var>)   # for a CMakeLists that guards more
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

define_property(GLOBAL PROPERTY ENGINE_DISABLED_MODULES
  BRIEF_DOCS "Capability modules skipped in this configuration"
  FULL_DOCS "Modules declared OPTIONAL whose capability switch is off")
set_property(GLOBAL PROPERTY ENGINE_DISABLED_MODULES "")

define_property(GLOBAL PROPERTY ENGINE_DISABLED_CAPABILITIES
  BRIEF_DOCS "Capabilities switched off in this configuration"
  FULL_DOCS "ENGINE_WITH_<NAME> switches that are off, or all of them under ENGINE_MINIMAL")
set_property(GLOBAL PROPERTY ENGINE_DISABLED_CAPABILITIES "")

# The switch behind one capability (ADR-0027 decision 4). Declared on first use so a capability
# that is not part of this configuration's source tree contributes no stale cache entry.
# ENGINE_MINIMAL wins over the per-capability option: the minimal build is a proof, and a proof
# that a stale cache entry can weaken is not one.
function(_engine_capability_option capability out_var)
  string(TOUPPER "${capability}" _upper)
  set(_opt "ENGINE_WITH_${_upper}")
  if(NOT DEFINED ${_opt})
    option(${_opt} "Build the ${capability} capability (ADR-0027)" ON)
  endif()
  if(ENGINE_MINIMAL)
    set(${out_var} OFF PARENT_SCOPE)
  else()
    set(${out_var} ${${_opt}} PARENT_SCOPE)
  endif()
endfunction()

# For a capability whose CMakeLists.txt has to guard more than its engine_module() call (an
# extra target, an engine_shaders() invocation):
#
#   engine_capability_enabled(cloth _cloth)
#   if(NOT _cloth)
#     return()
#   endif()
function(engine_capability_enabled capability out_var)
  _engine_capability_option("${capability}" _enabled)
  set(${out_var} ${_enabled} PARENT_SCOPE)
endfunction()

function(_engine_layer_index layer out_var)
  list(FIND ENGINE_LAYERS "${layer}" _idx)
  if(_idx EQUAL -1)
    message(FATAL_ERROR "engine_module: unknown layer '${layer}'. Known layers: ${ENGINE_LAYERS}")
  endif()
  set(${out_var} ${_idx} PARENT_SCOPE)
endfunction()

function(engine_module)
  set(_options WHOLE_ARCHIVE OPTIONAL)
  set(_one NAME LAYER CAPABILITY)
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

  # An optional capability's switch is consulted before anything else is validated or created:
  # a capability that is off contributes no target, no dependency check, and no modules.json row.
  set(_optional FALSE)
  set(_capability "")
  if(EM_OPTIONAL OR EM_CAPABILITY)
    set(_optional TRUE)
    set(_capability "${EM_CAPABILITY}")
    if(NOT _capability)
      set(_capability "${EM_NAME}")
    endif()
    _engine_capability_option("${_capability}" _enabled)
    if(NOT _enabled)
      string(TOUPPER "${_capability}" _upper)
      set_property(GLOBAL APPEND PROPERTY ENGINE_DISABLED_MODULES "${EM_NAME}")
      set_property(GLOBAL APPEND PROPERTY ENGINE_DISABLED_CAPABILITIES "${_capability}")
      message(STATUS "engine module: ${EM_NAME} [${EM_LAYER}] skipped (ENGINE_WITH_${_upper}=OFF)")
      return()
    endif()
  endif()

  set(_target engine_${EM_NAME})

  # Validate dependencies before creating the target so errors are clear.
  get_property(_disabled GLOBAL PROPERTY ENGINE_DISABLED_MODULES)
  set(_dep_targets "")
  foreach(_dep IN LISTS EM_DEPS)
    if("${_dep}" IN_LIST _disabled)
      message(FATAL_ERROR
        "engine_module(${EM_NAME}) [${EM_LAYER}]: depends on '${_dep}', an optional capability that is "
        "switched off in this configuration. A capability may only be depended on by its own capability "
        "group, which shares its switch (ADR-0027 decision 1); anything else cannot be built without it.")
    endif()
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

  if(EM_SOURCES AND EM_WHOLE_ARCHIVE)
    add_library(${_target}_impl STATIC ${EM_SOURCES})
    target_include_directories(${_target}_impl
      PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
      PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(${_target}_impl PUBLIC ${_dep_targets} ${EM_EXTERNAL_DEPS})
    target_compile_options(${_target}_impl PRIVATE ${ENGINE_NO_EXCEPTIONS_FLAGS})
    engine_apply_warnings(${_target}_impl)
    add_library(${_target} INTERFACE)
    target_link_libraries(${_target} INTERFACE "$<LINK_LIBRARY:WHOLE_ARCHIVE,${_target}_impl>")
  elseif(EM_SOURCES)
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
    ENGINE_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    ENGINE_MODULE_OPTIONAL "${_optional}"
    ENGINE_MODULE_CAPABILITY "${_capability}")

  set_property(GLOBAL APPEND PROPERTY ENGINE_MODULES "${EM_NAME}")
  if(_optional)
    message(STATUS "engine module: ${EM_NAME} [${EM_LAYER}] (capability ${_capability}) deps: ${EM_DEPS}")
  else()
    message(STATUS "engine module: ${EM_NAME} [${EM_LAYER}] deps: ${EM_DEPS}")
  endif()
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
  # A capability that is switched off takes its tests with it (ADR-0027).
  get_property(_disabled GLOBAL PROPERTY ENGINE_DISABLED_MODULES)
  if("${ET_NAME}" IN_LIST _disabled)
    return()
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
    get_property(_opt   TARGET engine_${_m} PROPERTY ENGINE_MODULE_OPTIONAL)
    get_property(_cap   TARGET engine_${_m} PROPERTY ENGINE_MODULE_CAPABILITY)
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
    if(_opt)
      set(_opt_json "true, \"capability\": \"${_cap}\"")
    else()
      set(_opt_json "false")
    endif()
    if(NOT _first)
      string(APPEND _json ",\n")
    endif()
    string(APPEND _json
      "    {\"name\": \"${_m}\", \"layer\": \"${_layer}\", \"path\": \"${_rel}\", "
      "\"optional\": ${_opt_json}, \"deps\": [${_deps_json}]}")
    set(_first FALSE)
  endforeach()
  string(APPEND _json "\n  ],\n")

  # What this configuration left out (ADR-0027): an agent reading modules.json can otherwise not
  # tell "this capability does not exist" from "this capability is switched off".
  get_property(_off GLOBAL PROPERTY ENGINE_DISABLED_CAPABILITIES)
  if(_off)
    list(REMOVE_DUPLICATES _off)
    list(SORT _off)
  endif()
  set(_off_json "")
  set(_first TRUE)
  foreach(_c IN LISTS _off)
    if(NOT _first)
      string(APPEND _off_json ", ")
    endif()
    string(APPEND _off_json "\"${_c}\"")
    set(_first FALSE)
  endforeach()
  if(ENGINE_MINIMAL)
    set(_minimal_json "true")
  else()
    set(_minimal_json "false")
  endif()
  string(APPEND _json "  \"minimal\": ${_minimal_json},\n")
  string(APPEND _json "  \"disabled_capabilities\": [${_off_json}]\n}\n")
  file(WRITE "${CMAKE_BINARY_DIR}/modules.json" "${_json}")
  message(STATUS "engine modules: wrote ${CMAKE_BINARY_DIR}/modules.json")
endfunction()

#   engine_app(NAME <name> OUTPUT <exe> SOURCES <file>... [DEPS <module>...] [E2E_TESTS <file>...])
#
# An executable in the apps layer: engine_<name> built as <exe> into ${CMAKE_BINARY_DIR}/bin,
# recorded in modules.json, linked against the named modules. E2E_TESTS builds
# engine_<name>_tests (linked against the same modules, core/platform, and the test main, not
# against the app) with ENGINE_APP_PATH defined to the built executable, and registers it.
function(engine_app)
  set(_one NAME OUTPUT)
  set(_multi SOURCES DEPS E2E_TESTS)
  cmake_parse_arguments(EA "" "${_one}" "${_multi}" ${ARGN})
  if(NOT EA_NAME OR NOT EA_SOURCES)
    message(FATAL_ERROR "engine_app: NAME and SOURCES are required")
  endif()
  if(NOT EA_OUTPUT)
    set(EA_OUTPUT "${EA_NAME}")
  endif()
  get_property(_declared GLOBAL PROPERTY ENGINE_MODULES)
  if("${EA_NAME}" IN_LIST _declared)
    message(FATAL_ERROR "engine_app(${EA_NAME}): name already declared")
  endif()
  _engine_layer_index("apps" _app_idx)
  set(_dep_targets "")
  foreach(_dep IN LISTS EA_DEPS)
    if(NOT "${_dep}" IN_LIST _declared)
      message(FATAL_ERROR "engine_app(${EA_NAME}): depends on '${_dep}', which is not declared")
    endif()
    get_property(_dep_layer TARGET engine_${_dep} PROPERTY ENGINE_LAYER)
    _engine_layer_index("${_dep_layer}" _dep_idx)
    if(_dep_idx GREATER _app_idx)
      message(FATAL_ERROR "engine_app(${EA_NAME}): depends on '${_dep}' [${_dep_layer}], which is a higher layer")
    endif()
    list(APPEND _dep_targets engine::${_dep})
  endforeach()

  set(_target engine_${EA_NAME})
  add_executable(${_target} ${EA_SOURCES})
  target_link_libraries(${_target} PRIVATE ${_dep_targets})
  target_compile_options(${_target} PRIVATE ${ENGINE_NO_EXCEPTIONS_FLAGS})
  engine_apply_warnings(${_target})
  set_target_properties(${_target} PROPERTIES
    OUTPUT_NAME "${EA_OUTPUT}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
    ENGINE_LAYER "apps"
    ENGINE_MODULE_NAME "${EA_NAME}"
    ENGINE_MODULE_DEPS "${EA_DEPS}"
    ENGINE_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}"
    ENGINE_MODULE_OPTIONAL FALSE
    ENGINE_MODULE_CAPABILITY "")
  set_property(GLOBAL APPEND PROPERTY ENGINE_MODULES "${EA_NAME}")
  message(STATUS "engine app: ${EA_NAME} -> ${EA_OUTPUT} deps: ${EA_DEPS}")

  if(EA_E2E_TESTS AND ENGINE_BUILD_TESTS)
    set(_test_target ${_target}_tests)
    add_executable(${_test_target} ${EA_E2E_TESTS})
    target_link_libraries(${_test_target} PRIVATE ${_dep_targets} engine::platform engine::json engine_test_main)
    target_compile_definitions(${_test_target} PRIVATE ENGINE_APP_PATH="$<TARGET_FILE:${_target}>")
    add_dependencies(${_test_target} ${_target})
    engine_apply_warnings(${_test_target})
    add_test(NAME ${EA_NAME} COMMAND ${_test_target})
    set_tests_properties(${EA_NAME} PROPERTIES LABELS "unit;e2e;${EA_NAME}")
  endif()
endfunction()
