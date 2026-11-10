# One list of scene generators (roadmap T79; docs/subsystems/scene_gen.md, "Which generators a host
# carries"; ADR-0060, proposed).
#
#   engine_module(NAME caves LAYER domain OPTIONAL WHOLE_ARCHIVE GENERATOR mesh:cave ...)
#   engine_host_generators(<app> <deps_var> [EXCLUDE <module> "<reason>"]...)
#
# A generator is a capability that registers itself — into the scene-generator registry
# (`scene_gen::Registrar`: a ground, a placement generator or a sky; ADR-0046) or into the mesh
# generators' (`assets::MeshGeneratorRegistrar`) — and that nothing calls by name: a host links it
# for its registrars alone, and a scene reaches it by the name it gives. `GENERATOR` on its
# engine_module() line says so, with every registration its sources make as `<kind>:<name>`
# (`ground`, `placement`, `sky` or `mesh`), and a tagged module that this configuration builds joins
# one build-wide list (read sorted by name).
#
# Why. Until T79 the hosts that read a scene each wrote the list out by hand (engine-view's,
# engine-host's and engine-editor's CMakeLists.txt), the three were edited 17, 17 and 8 times, and
# the editor's had lost `caves` and `soil` under a comment saying it matched the others: a document
# placing a cave was refused by the editor alone, with a sentence nobody saw until a reviewer read
# the three lists side by side. A registrar the linker drops fails just as silently, so the list is
# checked twice: here at configure (every host in ENGINE_GENERATOR_HOSTS links the build's list, less
# what it excludes in writing), and at run time in each host's test executable, which links what the
# host links (`<host>.generators`: every tagged registration is in its registry, and nothing is
# registered that no tag names).
#
# The names are on the tag rather than read from the sources because the run-time check needs
# them and the configure cannot see a registrar; the run-time check's second half is what keeps
# them true (a registrar added to a tagged module without its name fails `<host>.generators`).

set(ENGINE_GENERATOR_KINDS ground placement sky mesh)

# The hosts that read a scene, and so carry every generator the build has: engine-view
# (`--scene`, `--mesh x.<gen>.json`, `--world`), engine-host (`render.load`) and engine-editor (a
# document's placements and ground, drawn through the renderer's scene reader). A host on this list
# that does not call engine_host_generators(), or links a different set, stops the configure. A
# host that reads no scene (engine-cli, engine-content's own commands link what they build) is not
# on it.
set(ENGINE_GENERATOR_HOSTS engine_view engine_host engine_editor)

set_property(GLOBAL PROPERTY ENGINE_GENERATOR_MODULES "")
set_property(GLOBAL PROPERTY ENGINE_GENERATOR_DECLARED "")
set_property(GLOBAL PROPERTY ENGINE_GENERATOR_HOSTS_LINKED "")

# Called by engine_module() for a module whose line carries GENERATOR, before its capability switch
# is consulted, so a malformed tag is refused in the minimal build too.
function(_engine_generator_validate name tags missing_values is_optional whole_archive)
  if(missing_values OR NOT tags)
    message(FATAL_ERROR
      "engine_module(${name}): GENERATOR names every registration the module's sources make, as "
      "<kind>:<name> with <kind> one of ${ENGINE_GENERATOR_KINDS} (GENERATOR placement:ruins). "
      "The run-time check in each host (<host>.generators) compares them with the registries.")
  endif()
  if(NOT is_optional)
    message(FATAL_ERROR
      "engine_module(${name}): GENERATOR on a module that is not a capability. A generator is "
      "linked by the hosts for its registrars and reached by name, so it is OPTIONAL (ADR-0027, "
      "ADR-0046); a registration every executable needs belongs to the module that reads it, as "
      "the renderer's own `waves` ground does.")
  endif()
  if(NOT whole_archive)
    message(FATAL_ERROR
      "engine_module(${name}): GENERATOR without WHOLE_ARCHIVE. Nothing references a registrar's "
      "object, so a static library without the whole-archive link loses it and the registration "
      "with it, silently (docs/subsystems/scene_gen.md, \"The registrar pattern\").")
  endif()
  foreach(_tag IN LISTS tags)
    if(NOT _tag MATCHES "^([a-z]+):([A-Za-z0-9_.-]+)$")
      message(FATAL_ERROR
        "engine_module(${name}): GENERATOR '${_tag}' is not <kind>:<name> (GENERATOR mesh:cave).")
    endif()
    if(NOT CMAKE_MATCH_1 IN_LIST ENGINE_GENERATOR_KINDS)
      message(FATAL_ERROR
        "engine_module(${name}): GENERATOR '${_tag}': '${CMAKE_MATCH_1}' is not a kind of "
        "generator; the kinds are ${ENGINE_GENERATOR_KINDS}.")
    endif()
  endforeach()
  set_property(GLOBAL APPEND PROPERTY ENGINE_GENERATOR_DECLARED "${name}")
endfunction()

# Called by engine_module() once the tagged module's target exists in this configuration.
function(_engine_generator_join name tags)
  set_target_properties(engine_${name} PROPERTIES ENGINE_MODULE_GENERATOR "${tags}")
  set_property(GLOBAL APPEND PROPERTY ENGINE_GENERATOR_MODULES "${name}")
endfunction()

#   engine_host_generators(<app> <deps_var> [EXCLUDE <module> "<reason>"]...)
#
# Appends every generator this configuration builds to the list in <deps_var>, for the
# engine_app(<app> ... DEPS ${<deps_var>}) that follows, and records <app> as one that did. The
# modules are tagged WHOLE_ARCHIVE, so engine_app's ordinary link keeps their registrars, in the host
# and in its end-to-end test executable alike. An EXCLUDE leaves a generator out of this host, with
# the reason the configure prints and modules.json would otherwise not know; the check below reads
# it. Every host on ENGINE_GENERATOR_HOSTS calls this before its engine_app().
function(engine_host_generators app deps_var)
  if(NOT app IN_LIST ENGINE_GENERATOR_HOSTS)
    message(FATAL_ERROR
      "engine_host_generators(${app}): not one of ENGINE_GENERATOR_HOSTS (${ENGINE_GENERATOR_HOSTS}). "
      "A new host that reads scenes joins that list in cmake/EngineGenerators.cmake, which is what "
      "makes the configure check it.")
  endif()
  set(_excluded "")
  set(_args ${ARGN})
  while(_args)
    list(POP_FRONT _args _word)
    if(NOT _word STREQUAL "EXCLUDE")
      message(FATAL_ERROR
        "engine_host_generators(${app}): unexpected '${_word}'; the form is EXCLUDE <module> \"<reason>\".")
    endif()
    list(LENGTH _args _left)
    if(_left LESS 2)
      message(FATAL_ERROR
        "engine_host_generators(${app}): EXCLUDE takes a module and the reason it is left out.")
    endif()
    list(POP_FRONT _args _module _reason)
    get_property(_declared GLOBAL PROPERTY ENGINE_GENERATOR_DECLARED)
    if(NOT _module IN_LIST _declared)
      message(FATAL_ERROR
        "engine_host_generators(${app}): EXCLUDE ${_module}, which no engine_module() tags GENERATOR.")
    endif()
    string(STRIP "${_reason}" _reason)
    if(_reason STREQUAL "")
      message(FATAL_ERROR
        "engine_host_generators(${app}): EXCLUDE ${_module} with no reason. A host that leaves a "
        "generator out says why, where the configure check and the next reader find it.")
    endif()
    list(APPEND _excluded "${_module}")
    set_property(GLOBAL PROPERTY ENGINE_GENERATOR_EXCLUDE_REASON_${app}_${_module} "${_reason}")
  endwhile()

  get_property(_generators GLOBAL PROPERTY ENGINE_GENERATOR_MODULES)
  # By name, so a host's DEPS (and its modules.json row) do not depend on add_subdirectory order.
  list(SORT _generators)
  set(_deps ${${deps_var}})
  set(_linked "")
  foreach(_g IN LISTS _generators)
    if(_g IN_LIST _excluded)
      continue()
    endif()
    if(NOT _g IN_LIST _deps)
      list(APPEND _deps "${_g}")
    endif()
    list(APPEND _linked "${_g}")
  endforeach()
  set(${deps_var} ${_deps} PARENT_SCOPE)
  set_property(GLOBAL PROPERTY ENGINE_GENERATOR_EXCLUDED_${app} "${_excluded}")
  set_property(GLOBAL APPEND PROPERTY ENGINE_GENERATOR_HOSTS_LINKED "${app}")
  list(LENGTH _linked _count)
  list(JOIN _linked " " _shown)
  message(STATUS "engine generators: ${app} links ${_count}: ${_shown}")
  foreach(_m IN LISTS _excluded)
    get_property(_why GLOBAL PROPERTY ENGINE_GENERATOR_EXCLUDE_REASON_${app}_${_m})
    message(STATUS "engine generators: ${app} leaves out ${_m}: ${_why}")
  endforeach()
endfunction()

# Every registration the generators `modules` make, as one comma-separated string (the form a
# compile definition carries; the tags hold no comma).
function(_engine_generator_names out)
  set(_names "")
  foreach(_m IN LISTS ARGN)
    get_property(_tags TARGET engine_${_m} PROPERTY ENGINE_MODULE_GENERATOR)
    list(APPEND _names ${_tags})
  endforeach()
  list(JOIN _names "," _joined)
  set(${out} "${_joined}" PARENT_SCOPE)
endfunction()

# The configure-time check, from engine_finalize_modules(), once every module and app is declared:
# each host on ENGINE_GENERATOR_HOSTS links the build's generators less its written exclusions,
# every difference named; then each host's test executable gets the run-time check.
function(_engine_check_generator_hosts)
  get_property(_generators GLOBAL PROPERTY ENGINE_GENERATOR_MODULES)
  list(SORT _generators)
  get_property(_called GLOBAL PROPERTY ENGINE_GENERATOR_HOSTS_LINKED)
  set(_problems "")
  foreach(_host IN LISTS ENGINE_GENERATOR_HOSTS)
    if(NOT TARGET engine_${_host})
      string(APPEND _problems
        "\n  ${_host}: no such app in this configuration; a host leaving the tree leaves "
        "ENGINE_GENERATOR_HOSTS with it (cmake/EngineGenerators.cmake).")
      continue()
    endif()
    if(NOT _host IN_LIST _called)
      string(APPEND _problems
        "\n  ${_host}: links no generator list. Call engine_host_generators(${_host} <deps var>) "
        "before its engine_app(), as the other hosts do.")
      continue()
    endif()
    get_property(_excluded GLOBAL PROPERTY ENGINE_GENERATOR_EXCLUDED_${_host})
    get_property(_deps TARGET engine_${_host} PROPERTY ENGINE_MODULE_DEPS)
    set(_missing "")
    set(_unwritten "")
    foreach(_g IN LISTS _generators)
      if(_g IN_LIST _excluded)
        if(_g IN_LIST _deps)
          list(APPEND _unwritten "${_g}")
        endif()
      elseif(NOT _g IN_LIST _deps)
        list(APPEND _missing "${_g}")
      endif()
    endforeach()
    list(JOIN _missing ", " _missing)
    list(JOIN _unwritten ", " _unwritten)
    if(_missing)
      string(APPEND _problems
        "\n  ${_host}: does not link ${_missing}, which this configuration builds and the other "
        "hosts link. Either its DEPS lost what engine_host_generators() appended, or it leaves "
        "them out: then say so, engine_host_generators(${_host} <deps var> EXCLUDE <module> \"<reason>\").")
    endif()
    if(_unwritten)
      string(APPEND _problems
        "\n  ${_host}: excludes ${_unwritten} and links it anyway; drop the exclusion or the link.")
    endif()
  endforeach()
  if(_problems)
    message(FATAL_ERROR
      "engine generators: the hosts that read a scene do not carry the build's one list "
      "(docs/subsystems/scene_gen.md, \"Which generators a host carries\"):${_problems}")
  endif()
  list(LENGTH _generators _count)
  list(JOIN _generators " " _shown)
  list(JOIN ENGINE_GENERATOR_HOSTS ", " _hosts)
  message(STATUS "engine generators: ${_count} in this configuration: ${_shown}")
  message(STATUS "engine generators: ${_hosts} link them all")

  # The run-time half: each host's end-to-end test executable is linked by engine_app() from the
  # host's own DEPS, so its registries are the host's. `<host>.generators` runs the generator cases
  # alone, with no device; the host's whole end-to-end run includes them too.
  foreach(_host IN LISTS ENGINE_GENERATOR_HOSTS)
    if(NOT TARGET engine_${_host}_tests)
      continue()
    endif()
    get_property(_excluded GLOBAL PROPERTY ENGINE_GENERATOR_EXCLUDED_${_host})
    set(_expected ${_generators})
    if(_excluded)
      list(REMOVE_ITEM _expected ${_excluded})
    endif()
    _engine_generator_names(_names ${_expected})
    target_sources(engine_${_host}_tests PRIVATE
      "${CMAKE_SOURCE_DIR}/apps/tests/host_generators_tests.cpp")
    target_compile_definitions(engine_${_host}_tests PRIVATE
      ENGINE_HOST_NAME="${_host}" ENGINE_HOST_GENERATORS="${_names}")
    add_test(NAME ${_host}.generators
      COMMAND engine_${_host}_tests "--test-case=generators:*")
    # doctest exits 0 when a filter matches no case, so the pass is doctest's own summary with the
    # two cases of host_generators_tests.cpp (or more) passed and none failed: a renamed case fails
    # here instead of passing with nothing run.
    set_tests_properties(${_host}.generators PROPERTIES LABELS "unit;e2e;${_host};generators"
      PASS_REGULAR_EXPRESSION "test cases: +[0-9]+ \\| +([2-9]|[1-9][0-9]+) passed \\| +0 failed")
  endforeach()
endfunction()
