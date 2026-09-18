# Schema code generation (ADR-0007).
#
#   engine_schema_library(NAME <module> SCHEMAS <file.schema>... [DEPS <module>...]
#                         [CAPABILITY <capability>])
#
# Runs schemac over the listed .schema files and declares an engine module (layer core) built
# from the generated sources. Generated headers are included as <schemas/<stem>.h>. Imports
# between schema files resolve against ${CMAKE_SOURCE_DIR}/schemas and the listed files.
#
# CAPABILITY ties the library to an optional capability's switch (ADR-0027): a capability's own
# component and event types live with the capability, in <layer>/<name>/schemas/<name>.schema,
# so adding them touches no shared file, and they leave the build with it. The generated module
# still sits in the core layer, because generated code depends only on core; `path` in
# modules.json says where it actually lives.

function(engine_schema_library)
  set(_one NAME CAPABILITY)
  set(_multi SCHEMAS DEPS)
  cmake_parse_arguments(ES "" "${_one}" "${_multi}" ${ARGN})
  if(NOT ES_NAME OR NOT ES_SCHEMAS)
    message(FATAL_ERROR "engine_schema_library: NAME and SCHEMAS are required")
  endif()

  # Checked before the custom command is created: a capability that is off generates nothing.
  if(ES_CAPABILITY)
    engine_capability_enabled("${ES_CAPABILITY}" _enabled)
    if(NOT _enabled)
      set_property(GLOBAL APPEND PROPERTY ENGINE_DISABLED_MODULES "${ES_NAME}")
      return()
    endif()
  endif()

  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated/${ES_NAME}")
  set(_outputs "")
  set(_sources "")
  set(_abs_schemas "")
  foreach(_schema IN LISTS ES_SCHEMAS)
    get_filename_component(_abs "${_schema}" ABSOLUTE)
    get_filename_component(_stem "${_schema}" NAME_WE)
    list(APPEND _abs_schemas "${_abs}")
    # `<stem>_ecs.h` is the ECS backend (ADR-0028 seam 1). It is generated for every schema and
    # compiled by nobody: it includes <flecs.h>, so only a translation unit that ADR-0028 seam 5
    # allows to reach flecs can include it, which is also what keeps it out of a build with
    # ENGINE_WITH_ECS off.
    list(APPEND _outputs
      "${_gen}/include/schemas/${_stem}.h"
      "${_gen}/include/schemas/${_stem}_ecs.h"
      "${_gen}/src/${_stem}.cpp"
      "${_gen}/json/${_stem}.schema.json"
      "${_gen}/docs/${_stem}.md")
    list(APPEND _sources "${_gen}/src/${_stem}.cpp")
  endforeach()

  add_custom_command(
    OUTPUT ${_outputs}
    COMMAND engine_schemac --out "${_gen}" --schema-root "${CMAKE_SOURCE_DIR}/schemas" ${_abs_schemas}
    DEPENDS ${_abs_schemas} engine_schemac
    COMMENT "schemac: ${ES_NAME}"
    VERBATIM)

  # WHOLE_ARCHIVE: the generated sources register their types during static initialization
  # and nothing else references them, so a plain static library would drop them.
  set(_capability_args "")
  if(ES_CAPABILITY)
    set(_capability_args CAPABILITY ${ES_CAPABILITY})
  endif()
  engine_module(NAME ${ES_NAME} LAYER core
    DEPS base hash memory containers math ids json schema ${ES_DEPS}
    SOURCES ${_sources}
    WHOLE_ARCHIVE ${_capability_args})
  target_include_directories(engine_${ES_NAME}_impl PUBLIC "${_gen}/include")
  set_property(TARGET engine_${ES_NAME} PROPERTY ENGINE_SCHEMA_FILES "${_abs_schemas}")
endfunction()
