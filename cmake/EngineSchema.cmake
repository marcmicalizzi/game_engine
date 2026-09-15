# Schema code generation (ADR-0007).
#
#   engine_schema_library(NAME <module> SCHEMAS <file.schema>... [DEPS <module>...])
#
# Runs schemac over the listed .schema files and declares an engine module (layer core) built
# from the generated sources. Generated headers are included as <schemas/<stem>.h>. Imports
# between schema files resolve against ${CMAKE_SOURCE_DIR}/schemas and the listed files.

function(engine_schema_library)
  set(_one NAME)
  set(_multi SCHEMAS DEPS)
  cmake_parse_arguments(ES "" "${_one}" "${_multi}" ${ARGN})
  if(NOT ES_NAME OR NOT ES_SCHEMAS)
    message(FATAL_ERROR "engine_schema_library: NAME and SCHEMAS are required")
  endif()

  set(_gen "${CMAKE_CURRENT_BINARY_DIR}/generated/${ES_NAME}")
  set(_outputs "")
  set(_sources "")
  set(_abs_schemas "")
  foreach(_schema IN LISTS ES_SCHEMAS)
    get_filename_component(_abs "${_schema}" ABSOLUTE)
    get_filename_component(_stem "${_schema}" NAME_WE)
    list(APPEND _abs_schemas "${_abs}")
    list(APPEND _outputs
      "${_gen}/include/schemas/${_stem}.h"
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
  engine_module(NAME ${ES_NAME} LAYER core
    DEPS base hash memory containers math ids json schema ${ES_DEPS}
    SOURCES ${_sources}
    WHOLE_ARCHIVE)
  target_include_directories(engine_${ES_NAME}_impl PUBLIC "${_gen}/include")
  set_property(TARGET engine_${ES_NAME} PROPERTY ENGINE_SCHEMA_FILES "${_abs_schemas}")
endfunction()
