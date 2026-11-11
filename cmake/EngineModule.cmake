# Module declaration and layering enforcement.
#
#   engine_module(NAME <name> LAYER <layer>
#                 [DEPS <module>...]           # other engine modules, by NAME
#                 [EXTERNAL_DEPS <target>...]  # third-party targets
#                 [SOURCES <file>...]          # omit for header-only modules
#                 [WHOLE_ARCHIVE]              # link every object even if unreferenced
#                 [OPTIONAL] [CAPABILITY <c>]  # an optional capability (ADR-0027)
#                 [GENERATOR <kind>:<name>...]) # a scene or mesh generator (EngineGenerators.cmake)
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
#   engine_capability_requires(<capability> <other>...) # the capability graph (ADR-0027)
#
#   engine_finalize_modules()   # writes ${CMAKE_BINARY_DIR}/modules.json
#
# Layers are ordered. A module may depend only on modules in the same or a
# lower layer, and only on modules declared earlier (so cycles are impossible).
# The alias engine::<name> is the target other modules link against.

set(ENGINE_LAYERS core foundation domain systems apps game)

# The GENERATOR tag's list, engine_host_generators() and the hosts' check (roadmap T79).
include("${CMAKE_CURRENT_LIST_DIR}/EngineGenerators.cmake")

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

# The capability graph (ADR-0027's "revisit when": *the grouping rule in decision 1 needs to
# become a declared capability graph*).
#
#   engine_capability_requires(animation ecs)
#
# declared at the top of a capability's CMakeLists.txt, before anything consults its switch.
#
# Why this exists. Decision 1 says a capability is never a dependency of a module that is not part
# of the same capability, because that module could then not be built without it — and until the
# first ticking capability landed, that rule and reality agreed. They stop agreeing the moment a
# capability *ticks*: the registration point for a ticking system is `ecs::register_system` with a
# `sim::SystemDesc` (ADR-0028 seam 2) and its components are flecs components registered from the
# schema IDL (seam 1), so the module that attaches through those points links `domain/ecs` — which
# is itself an optional capability under `ENGINE_WITH_ECS`. With `ENGINE_WITH_ECS=OFF` and
# `ENGINE_WITH_ANIMATION=ON` the dependency check above fired and the configure died, which is the
# wrong answer twice: the combination is legitimate, and the failure was a hard error in a shared
# file rather than one capability quietly not being in this build.
#
# What a declared edge buys: the dependent capability resolves **off** when anything it requires is
# off, it says so once on the status line, and `modules.json` lists it under `disabled_capabilities`
# like any other switched-off capability — so "this configuration has no animation" stays a fact
# the module graph answers, and the minimal build stays the proof it was.
#
# What it deliberately does not do is turn the required capability *on*. A switch the user set is
# never overridden, because `ENGINE_MINIMAL=ON` has to mean what it says.
function(engine_capability_requires capability)
  if(NOT ARGN)
    message(FATAL_ERROR "engine_capability_requires(${capability}): name at least one capability")
  endif()
  get_property(_already GLOBAL PROPERTY ENGINE_CAPABILITY_REQUIRES_${capability})
  list(APPEND _already ${ARGN})
  list(REMOVE_DUPLICATES _already)
  set_property(GLOBAL PROPERTY ENGINE_CAPABILITY_REQUIRES_${capability} "${_already}")
endfunction()

# Every capability `capability` requires, directly or through another, lower-cased: what a
# capability's module may depend on besides its own (engine_module's check below). The graph is
# acyclic (the switch resolution refuses a cycle) and a handful of edges deep.
function(_engine_capability_requires_closure capability out_var)
  set(_closure "")
  set(_todo "${capability}")
  while(_todo)
    list(POP_FRONT _todo _next)
    get_property(_requires GLOBAL PROPERTY ENGINE_CAPABILITY_REQUIRES_${_next})
    foreach(_req IN LISTS _requires)
      string(TOLOWER "${_req}" _req_lower)
      if(NOT _req_lower IN_LIST _closure)
        list(APPEND _closure "${_req_lower}")
        list(APPEND _todo "${_req}")
      endif()
    endforeach()
  endwhile()
  set(${out_var} "${_closure}" PARENT_SCOPE)
endfunction()

# The switch behind one capability (ADR-0027 decision 4). Declared on first use so a capability
# that is not part of this configuration's source tree contributes no stale cache entry.
# ENGINE_MINIMAL wins over the per-capability option: the minimal build is a proof, and a proof
# that a stale cache entry can weaken is not one.
# `out_reason` comes back as the switch that actually decided it, so a skip message can say why a
# capability is absent when its own switch is on.
function(_engine_capability_option capability out_var out_reason)
  string(TOUPPER "${capability}" _upper)
  set(_opt "ENGINE_WITH_${_upper}")
  if(NOT DEFINED ${_opt})
    option(${_opt} "Build the ${capability} capability (ADR-0027)" ON)
  endif()
  set(_enabled ${${_opt}})
  set(_why "ENGINE_WITH_${_upper}=OFF")
  if(ENGINE_MINIMAL)
    set(_enabled OFF)
    set(_why "ENGINE_MINIMAL=ON")
  endif()

  # Requirements, if the switch itself said yes. The recursion is over the declared graph, which
  # is a handful of edges deep at most; the stack is carried so a cycle is a named error instead
  # of a CMake that never returns.
  if(_enabled)
    get_property(_stack GLOBAL PROPERTY ENGINE_CAPABILITY_STACK)
    if("${capability}" IN_LIST _stack)
      message(FATAL_ERROR
        "engine capability graph: '${capability}' requires itself through ${_stack}. "
        "A capability that needs another one is a dependency, and dependencies do not form cycles.")
    endif()
    get_property(_requires GLOBAL PROPERTY ENGINE_CAPABILITY_REQUIRES_${capability})
    if(_requires)
      set_property(GLOBAL PROPERTY ENGINE_CAPABILITY_STACK "${_stack};${capability}")
      foreach(_req IN LISTS _requires)
        _engine_capability_option("${_req}" _req_enabled _req_why)
        if(NOT _req_enabled)
          set(_enabled OFF)
          set(_why "it requires ${_req}, and ${_req_why}")
          break()
        endif()
      endforeach()
      set_property(GLOBAL PROPERTY ENGINE_CAPABILITY_STACK "${_stack}")
    endif()
  endif()

  set(${out_var} ${_enabled} PARENT_SCOPE)
  set(${out_reason} ${_why} PARENT_SCOPE)
endfunction()

# For a capability whose CMakeLists.txt has to guard more than its engine_module() call (an
# extra target, an engine_shaders() invocation):
#
#   engine_capability_enabled(cloth _cloth)
#   if(NOT _cloth)
#     return()
#   endif()
function(engine_capability_enabled capability out_var)
  _engine_capability_option("${capability}" _enabled _why)
  # A CMakeLists that guards its whole file with this (because the capability's dependency must
  # not even be fetched when it is off) still owes modules.json the row that says the capability
  # exists and is switched off: engine_module() never runs in that configuration to record it,
  # and an empty "disabled_capabilities" would read as "there is no such capability".
  if(NOT _enabled)
    set_property(GLOBAL APPEND PROPERTY ENGINE_DISABLED_CAPABILITIES "${capability}")
  endif()
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
  set(_multi DEPS EXTERNAL_DEPS SOURCES GENERATOR)
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
  # GENERATOR (cmake/EngineGenerators.cmake): a capability the hosts link for its registrars, with
  # the registrations it makes. Checked before the switch, so the minimal build refuses a bad tag.
  set(_generator_missing FALSE)
  if("GENERATOR" IN_LIST EM_KEYWORDS_MISSING_VALUES)
    set(_generator_missing TRUE)
  endif()
  if(EM_GENERATOR OR _generator_missing)
    set(_generator_optional FALSE)
    if(EM_OPTIONAL OR EM_CAPABILITY)
      set(_generator_optional TRUE)
    endif()
    _engine_generator_validate("${EM_NAME}" "${EM_GENERATOR}" ${_generator_missing}
                               ${_generator_optional} "${EM_WHOLE_ARCHIVE}")
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
    _engine_capability_option("${_capability}" _enabled _why)
    if(NOT _enabled)
      set_property(GLOBAL APPEND PROPERTY ENGINE_DISABLED_MODULES "${EM_NAME}")
      set_property(GLOBAL APPEND PROPERTY ENGINE_DISABLED_CAPABILITIES "${_capability}")
      message(STATUS "engine module: ${EM_NAME} [${EM_LAYER}] skipped (${_why})")
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
        "switched off in this configuration. A module that is not itself a capability cannot depend on "
        "one (ADR-0027 decision 1): it could not be built without it. A module that *is* a capability "
        "declares the edge instead — engine_capability_requires(<this capability> ${_dep}) at the top of "
        "its CMakeLists.txt — and is then switched off with what it requires, rather than failing here.")
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
    # ADR-0027 decision 1 in every configuration, not only the ones that switch the capability off
    # (ADR-0046): a module that is not a capability never depends on one, however its CMakeLists
    # arrives at the edge — which is what closed the conditional link ADR-0037 allowed once and the
    # dunes took a second time. A host (engine_app) links the capabilities it carries; a module
    # reaches one through a registration point (the scene-generator registry, the schema registry,
    # the scheduler's table). A capability that needs another declares the edge, so it is switched
    # off with it (the capability graph above) — the one check the `*-no-ecs` presets otherwise
    # make, and only for `ecs`.
    get_property(_dep_optional TARGET engine_${_dep} PROPERTY ENGINE_MODULE_OPTIONAL)
    if(_dep_optional)
      get_property(_dep_capability TARGET engine_${_dep} PROPERTY ENGINE_MODULE_CAPABILITY)
      if(NOT _optional)
        message(FATAL_ERROR
          "engine_module(${EM_NAME}) [${EM_LAYER}]: depends on '${_dep}', the ${_dep_capability} capability, "
          "and is not a capability itself. A capability never becomes a dependency of a module that is not "
          "part of it (ADR-0027 decision 1), in any configuration: reach it through a registration point "
          "(a scene generator registers itself: docs/subsystems/scene_gen.md, ADR-0046), and link it into "
          "the hosts that carry it.")
      endif()
      # Capability names compare as their switches do, whatever their case (`CAPABILITY PHYSICS`,
      # `engine_capability_requires(tissue physics)`: both are ENGINE_WITH_PHYSICS).
      string(TOLOWER "${_dep_capability}" _dep_cap_lower)
      string(TOLOWER "${_capability}" _cap_lower)
      if(NOT _dep_cap_lower STREQUAL _cap_lower)
        _engine_capability_requires_closure("${_capability}" _closure)
        if(NOT _dep_cap_lower IN_LIST _closure)
          message(FATAL_ERROR
            "engine_module(${EM_NAME}) [${EM_LAYER}]: the ${_capability} capability depends on '${_dep}', the "
            "${_dep_capability} capability, without declaring it. Say "
            "engine_capability_requires(${_capability} ${_dep_capability}) at the top of its CMakeLists.txt, "
            "so a configuration without ${_dep_capability} switches ${_capability} off instead of failing "
            "(docs/plan/08-toolchain.md section 8.5).")
        endif()
      endif()
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
  if(EM_GENERATOR)
    _engine_generator_join("${EM_NAME}" "${EM_GENERATOR}")
  endif()

  set_property(GLOBAL APPEND PROPERTY ENGINE_MODULES "${EM_NAME}")
  if(_optional)
    message(STATUS "engine module: ${EM_NAME} [${EM_LAYER}] (capability ${_capability}) deps: ${EM_DEPS}")
  else()
    message(STATUS "engine module: ${EM_NAME} [${EM_LAYER}] deps: ${EM_DEPS}")
  endif()
endfunction()

#   _engine_modules_reach(<out> <module> <start>...)
# TRUE in <out> when <module> is one of <start> or a dependency of one of them, through
# ENGINE_MODULE_DEPS, however deep.
function(_engine_modules_reach out module)
  set(_seen "")
  set(_queue ${ARGN})
  while(_queue)
    list(POP_FRONT _queue _m)
    if("${_m}" STREQUAL "${module}")
      set(${out} TRUE PARENT_SCOPE)
      return()
    endif()
    if("${_m}" IN_LIST _seen OR NOT TARGET engine_${_m})
      continue()
    endif()
    list(APPEND _seen "${_m}")
    get_property(_deps TARGET engine_${_m} PROPERTY ENGINE_MODULE_DEPS)
    list(APPEND _queue ${_deps})
  endwhile()
  set(${out} FALSE PARENT_SCOPE)
endfunction()

#   _engine_can_wait_for_gpu(<out> <start>...)
# TRUE when the modules reach gfx, whose Device::create takes the GPU lock, or window, whose
# Vulkan windows take it before they appear (docs/subsystems/gpu_lock.md).
function(_engine_can_wait_for_gpu out)
  _engine_modules_reach(_gfx gfx ${ARGN})
  _engine_modules_reach(_window window ${ARGN})
  if(_gfx OR _window)
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()

#   engine_test_waits_for_gpu(<test>)
# Marks a test whose processes can open a GPU device, and so can wait for the machine-wide GPU
# lock (ADR-0049, docs/subsystems/gpu_lock.md): TIMEOUT is its work plus the queue budget
# (ENGINE_TEST_WORK_TIMEOUT_S + ENGINE_TEST_GPU_LOCK_WAIT_S, cmake/EngineTesting.cmake), exit code
# 75 — a wait that ran out before anything reached the GPU — is a skip and not a failure, and the
# label `gpu` selects them (`ctest -L gpu`). A module's tests get it when the module or a test
# dependency reaches gfx or window (_engine_can_wait_for_gpu), an end-to-end test when its app
# does; a test that starts a process that opens a device without linking either itself
# (engine-cli's and engine-mcp's, which start engine-host) says so beside the line that names
# that process.
function(engine_test_waits_for_gpu test)
  if(NOT ENGINE_BUILD_TESTS OR NOT TEST ${test})
    return()
  endif()
  math(EXPR _timeout "${ENGINE_TEST_WORK_TIMEOUT_S} + ${ENGINE_TEST_GPU_LOCK_WAIT_S}")
  set_tests_properties(${test} PROPERTIES TIMEOUT ${_timeout} SKIP_RETURN_CODE 75)
  set_property(TEST ${test} APPEND PROPERTY LABELS gpu)
endfunction()

#   _engine_test_parts(<name> <target> <parts> <labels> <opens_devices> <timeout>)
# Registers one test executable as <parts> CTest tests, <name>.1 to <name>.<parts>, each running
# only its own source files' cases (doctest's `--source-file=` filter). Called deferred, at the end
# of the directory that declared the tests (cmake_language(DEFER), scheduled by
# _engine_defer_test_parts), so that the deal reads the target's whole SOURCES — gfx adds most of
# its test files with target_sources after engine_module_tests, and a file the deal never saw
# would be a file whose cases no part runs.
#
# **The deal is by cost** (roadmap T77, T88; docs/ci/what-to-run.md, "Parts dealt by cost";
# engine_deal_test_files in cmake/EngineTestDeal.cmake, which a prediction runs too): the files
# cmake/test-costs.txt gives seconds for go first, longest first, each to the part with the least
# so far (longest-processing-time first, which is within a third of the best possible deal); the
# files it does not name follow in name order, round the parts, which with an empty table is the
# name-order deal every part had before the table. The filter is a wildcard over the whole path
# the compiler saw, anchored at the separator before the basename — `*/<basename>` and
# `*\<basename>`, since MSVC's __FILE__ says `..\..\domain\gfx\tests\x.cpp` — so gfx's
# `deform_tests.cpp` does not also run `flora_deform_tests.cpp` (the unanchored `*<basename>` of
# the first renderer parts refused such pairs instead, and gfx has three). Two files of one
# basename in different directories are one entry, run by one part. Compiled sources only (.cpp,
# .cc, .c): no test case lives in a header, and the count check in what-to-run.md is how a part
# that lost one would be found.
function(_engine_test_parts name target parts labels opens_devices timeout)
  get_target_property(_sources ${target} SOURCES)
  set(_names "")
  foreach(_src IN LISTS _sources)
    if(_src MATCHES "\\.(cpp|cc|c)$")
      get_filename_component(_base "${_src}" NAME)
      list(APPEND _names "${_base}")
    endif()
  endforeach()
  engine_deal_test_files(${name} ${parts} "${_names}" _deal)   # cmake/EngineTestDeal.cmake

  math(EXPR _last "${parts} - 1")
  foreach(_k RANGE 0 ${_last})
    if(NOT _deal_${_k})
      continue()
    endif()
    set(_filters "")
    foreach(_base IN LISTS _deal_${_k})
      list(APPEND _filters "*/${_base}" "*\\${_base}")
    endforeach()
    math(EXPR _part "${_k} + 1")
    set(_test ${name}.${_part})
    string(JOIN "," _filter_arg ${_filters})
    add_test(NAME ${_test} COMMAND ${target} --duration=true "--source-file=${_filter_arg}")
    set_tests_properties(${_test} PROPERTIES LABELS "${labels}")
    if(NOT "${timeout}" STREQUAL "")
      set_tests_properties(${_test} PROPERTIES TIMEOUT ${timeout})
    endif()
    if(opens_devices)
      engine_test_waits_for_gpu(${_test})
    endif()
  endforeach()
endfunction()

#   _engine_defer_test_parts(<name> <target> <parts> <labels> <opens_devices> <timeout>)
# Schedules _engine_test_parts for the end of the calling directory. The values are written into
# the deferred call now: a deferred call evaluates its arguments when it runs, in the directory's
# scope, where the caller's locals no longer exist. A bracket argument keeps the labels' semicolons
# as they are, so the list arrives as one argument and is a list again.
function(_engine_defer_test_parts name target parts labels opens_devices timeout)
  cmake_language(EVAL CODE
    "cmake_language(DEFER CALL _engine_test_parts [[${name}]] [[${target}]] [[${parts}]] [[${labels}]] [[${opens_devices}]] [[${timeout}]])")
endfunction()

#   _engine_check_config_invariant(<name>)
# The proof behind CONFIG_INVARIANT (engine_module_tests, below): the module's library is the same
# code in every configuration, so its GPU cases need to run in one configuration only, and the
# off-configurations may skip them (`tools/dev.ps1 test -SkipInvariantGpu`, docs/ci/what-to-run.md,
# "The off-configurations' GPU cases"). The module is not a capability and links none
# (engine_module refuses that), so the one way its library could still differ is a capability
# conditional in its own CMakeLists.txt: `if(TARGET engine::<x>)`, ENGINE_WITH_<X> or
# ENGINE_MINIMAL around something that is not its tests. Every line inside such a block must name
# the tests — `_tests` (the test target, a list of test files) or a `tests/` path — or be a
# comment, a blank line, flow control or a closing parenthesis; anything else stops the configure.
# The C++ half (a preprocessor conditional on those names in the module's sources) is the
# `config-invariant-conditional` rule of tools/lint.ps1.
function(_engine_check_config_invariant name)
  set(_file "${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt")
  # Read whole and split here rather than with file(STRINGS), which drops empty lines (the line
  # numbers in a message would drift) and splits a line at a non-ASCII byte (an em dash in a
  # comment became a line of its own). A semicolon or a square bracket would mean something to
  # CMake's lists, so each becomes a space first; nothing below looks for one.
  file(READ "${_file}" _content)
  string(REGEX REPLACE "[][;]" " " _content "${_content}")
  string(REPLACE "\r" "" _content "${_content}")
  string(REPLACE "\n" ";" _lines "${_content}")
  set(_stack "")
  set(_n 0)
  foreach(_raw IN LISTS _lines)
    math(EXPR _n "${_n} + 1")
    string(REGEX REPLACE "#.*$" "" _line "${_raw}")
    string(STRIP "${_line}" _line)
    if(_line STREQUAL "")
      continue()
    endif()
    set(_capability FALSE)
    if(_line MATCHES "TARGET[ \t]+engine::|ENGINE_WITH_|ENGINE_MINIMAL")
      set(_capability TRUE)
    endif()
    if(_line MATCHES "^if[ \t]*\\(")
      list(APPEND _stack ${_capability})
      continue()
    elseif(_line MATCHES "^(elseif|else)[ \t]*\\(")
      if(_capability AND _stack)
        list(POP_BACK _stack)
        list(APPEND _stack TRUE)
      endif()
      continue()
    elseif(_line MATCHES "^endif[ \t]*\\(")
      if(_stack)
        list(POP_BACK _stack)
      endif()
      continue()
    elseif(_line MATCHES "^(foreach|endforeach|while|endwhile|\\))[ \t]*(\\(|$)")
      continue()
    endif()
    if(NOT "TRUE" IN_LIST _stack)
      if(_line MATCHES "ENGINE_WITH_|ENGINE_MINIMAL")
        message(FATAL_ERROR "engine_module_tests(${name} CONFIG_INVARIANT): ${_file}:${_n} names a capability switch outside its tests; a module whose GPU cases run in one configuration only is the same code in all of them (docs/ci/what-to-run.md, \"The off-configurations' GPU cases\")\n    ${_raw}")
      endif()
      continue()
    endif()
    if(NOT _line MATCHES "_tests|tests/")
      message(FATAL_ERROR "engine_module_tests(${name} CONFIG_INVARIANT): ${_file}:${_n} is inside a capability conditional and is not about the module's tests; a module whose GPU cases run in one configuration only is the same code in all of them, so drop CONFIG_INVARIANT or move the conditional to the tests (docs/ci/what-to-run.md, \"The off-configurations' GPU cases\")\n    ${_raw}")
    endif()
  endforeach()
endfunction()

#   engine_module_tests(NAME <name> SOURCES <file>... [DEPS <module>...] [PARTS <n>]
#                       [CONFIG_INVARIANT] [WALL_CLOCK])
# DEPS are test-only dependencies (other engine modules the tests use but the module does not).
# PARTS registers the one executable as <n> CTest tests, <name>.1 to <name>.<n>, each running only
# its own files' cases (_engine_test_parts above: dealt by cost, the rest by name), for a suite
# that has outgrown one test's GPU work budget on the slowest card — the renderer on the Titan Xp,
# 2026-11-09: 6,389 s on gate b56, past the 9,000 s ceiling on b57 — so that it runs as parts that
# each fit instead of a third raise of the budget (docs/ci/remote-linux.md). `-Filter <name>` and
# `-Affected` still select every part: the affected filter admits a dotted suffix
# (tools/lib/Affected.psm1), and the gate's reruns name a part.
# CONFIG_INVARIANT labels the tests `config_invariant`: the module's library is the same code in
# every configuration (checked here, _engine_check_config_invariant, and by the lint), so an
# off-configuration may run its tests with no GPU and leave the GPU cases to the all-on suite
# (`tools/dev.ps1 test -SkipInvariantGpu`).
# WALL_CLOCK labels the tests `wall_clock`: a case asserts an upper bound on wall-clock time (a
# lookup under 50 ms), which another test sharing the cores can break, so the finalize in
# cmake/EngineTesting.cmake runs every test with the label alone (RUN_SERIAL) in a run with jobs
# (roadmap T87; docs/ci/what-to-run.md, "Jobs"). A lower bound, a synthetic clock or a ratio does
# not need it.
# Every test runs with doctest's `--duration=true`, one line of seconds a case in the log, which is
# what cmake/test-costs.txt is made from (tools/test-costs.ps1).
function(engine_module_tests)
  set(_one NAME PARTS)
  set(_multi SOURCES DEPS)
  cmake_parse_arguments(ET "CONFIG_INVARIANT;WALL_CLOCK" "${_one}" "${_multi}" ${ARGN})
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
  _engine_can_wait_for_gpu(_opens_devices ${ET_NAME} ${ET_DEPS})
  set(_labels "unit;${ET_NAME}")
  if(ET_CONFIG_INVARIANT)
    _engine_check_config_invariant(${ET_NAME})
    list(APPEND _labels config_invariant)
  endif()
  if(ET_WALL_CLOCK)
    list(APPEND _labels wall_clock)
  endif()
  if(ET_PARTS AND ET_PARTS GREATER 1)
    _engine_defer_test_parts(${ET_NAME} ${_test_target} ${ET_PARTS} "${_labels}" "${_opens_devices}" "")
  else()
    add_test(NAME ${ET_NAME} COMMAND ${_test_target} --duration=true)
    set_tests_properties(${ET_NAME} PROPERTIES LABELS "${_labels}")
    if(_opens_devices)
      engine_test_waits_for_gpu(${ET_NAME})
    endif()
  endif()
endfunction()

function(engine_finalize_modules)
  # The hosts that read a scene carry the build's one generator list (EngineGenerators.cmake).
  _engine_check_generator_hosts()
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
    # A generator's tag (EngineGenerators.cmake): the registrations it makes, `<kind>:<name>`.
    get_property(_gen TARGET engine_${_m} PROPERTY ENGINE_MODULE_GENERATOR)
    set(_gen_json "")
    if(_gen)
      list(TRANSFORM _gen PREPEND "\"")
      list(TRANSFORM _gen APPEND "\"")
      list(JOIN _gen ", " _gen_list)
      set(_gen_json ", \"generator\": [${_gen_list}]")
    endif()
    if(NOT _first)
      string(APPEND _json ",\n")
    endif()
    string(APPEND _json
      "    {\"name\": \"${_m}\", \"layer\": \"${_layer}\", \"path\": \"${_rel}\", "
      "\"optional\": ${_opt_json}${_gen_json}, \"deps\": [${_deps_json}]}")
    set(_first FALSE)
  endforeach()
  string(APPEND _json "\n  ],\n")

  # The build's one generator list, by name, and what each host that links it leaves out.
  get_property(_generators GLOBAL PROPERTY ENGINE_GENERATOR_MODULES)
  set(_gens_json "")
  if(_generators)
    set(_quoted ${_generators})
    list(SORT _quoted)
    list(TRANSFORM _quoted PREPEND "\"")
    list(TRANSFORM _quoted APPEND "\"")
    list(JOIN _quoted ", " _gens_json)
  endif()
  set(_hosts_json "")
  set(_first TRUE)
  foreach(_h IN LISTS ENGINE_GENERATOR_HOSTS)
    get_property(_ex GLOBAL PROPERTY ENGINE_GENERATOR_EXCLUDED_${_h})
    set(_ex_json "")
    set(_efirst TRUE)
    foreach(_e IN LISTS _ex)
      get_property(_why GLOBAL PROPERTY ENGINE_GENERATOR_EXCLUDE_REASON_${_h}_${_e})
      string(REPLACE "\\" "\\\\" _why "${_why}")
      string(REPLACE "\"" "\\\"" _why "${_why}")
      if(NOT _efirst)
        string(APPEND _ex_json ", ")
      endif()
      string(APPEND _ex_json "\"${_e}\": \"${_why}\"")
      set(_efirst FALSE)
    endforeach()
    if(NOT _first)
      string(APPEND _hosts_json ", ")
    endif()
    string(APPEND _hosts_json "\"${_h}\": {\"excludes\": {${_ex_json}}}")
    set(_first FALSE)
  endforeach()
  string(APPEND _json "  \"generators\": [${_gens_json}],\n")
  string(APPEND _json "  \"generator_hosts\": {${_hosts_json}},\n")

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

#   engine_app(NAME <name> OUTPUT <exe> SOURCES <file>... [DEPS <module>...] [E2E_TESTS <file>...]
#              [E2E_PARTS <n>])
#
# An executable in the apps layer: engine_<name> built as <exe> into ${CMAKE_BINARY_DIR}/bin,
# recorded in modules.json, linked against the named modules. E2E_TESTS builds
# engine_<name>_tests (linked against the same modules, core/platform, and the test main, not
# against the app) with ENGINE_APP_PATH defined to the built executable, and registers it.
# E2E_PARTS registers it as <n> tests, <name>.1 to <name>.<n>, dealt as engine_module_tests' PARTS
# are (_engine_test_parts): engine_view's run was the longest single test on the machine, 1,536 s
# in msvc-debug and 1,941 s in msvc-release on gate b57 (docs/ci/what-to-run.md).
function(engine_app)
  set(_one NAME OUTPUT E2E_PARTS)
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
  # engine::platform is linked into every app whether it asked for it or not: each app's main()
  # calls platform::require_cpu_baseline() as its first statement (ADR-0031), so the CPU check
  # is a property of being an app here rather than a dependency six CMakeLists have to remember.
  # The E2E test target below has had it unconditionally since it was written, for its own
  # reasons; this is the same idea one target over.
  target_link_libraries(${_target} PRIVATE ${_dep_targets} engine::platform)
  # And the build stamp (cmake/EngineBuildStamp.cmake): the commit this app was built from, for its
  # `--version`. Apps only, so a commit relinks bin/ and not every test; the explicit dependency on
  # the updating target puts the refreshed header in place before main.cpp compiles.
  target_link_libraries(${_target} PRIVATE engine_build_stamp)
  add_dependencies(${_target} engine_build_stamp_update)
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
    _engine_can_wait_for_gpu(_opens_devices ${EA_DEPS})
    if(EA_E2E_PARTS AND EA_E2E_PARTS GREATER 1)
      # Every part carries what the one test did below: the labels, the lock, the timeout. The
      # lock is a property set after the deal (_engine_test_parts sets labels and timeout), so it
      # rides on the e2e label in EngineTesting.cmake's finalize, as the GPU one rides on `gpu`.
      _engine_defer_test_parts(${EA_NAME} ${_test_target} ${EA_E2E_PARTS} "unit;e2e;${EA_NAME}"
                               "${_opens_devices}" ${ENGINE_TEST_WORK_TIMEOUT_S})
      return()
    endif()
    add_test(NAME ${EA_NAME} COMMAND ${_test_target} --duration=true)
    # End-to-end apps open windows, GPU devices, and input devices, and a machine has one of
    # each: RESOURCE_LOCK keeps two of these tests from running at the same time. What it does
    # *not* do is order this ctest invocation against another one — a second build tree, a
    # release build beside a debug one, another agent's worktree, CI on the developer's box —
    # so it is no defence at all against two runs sharing a file. That is what the unique
    # scratch directories of tests/support/test_temp_dir.h are for, and why no end-to-end test
    # may name a fixed path under the system temp directory.
    #
    # TIMEOUT is explicit rather than left at CTest's 1500 s default so that the number carries
    # its reason. Ten minutes is roughly sixteen times the worst end-to-end run measured on a
    # deliberately loaded machine (engine_view, 37 s, with a release build compiling and three
    # 1600x1000 engine-view windows open), which is headroom for a slower machine and a colder
    # cache and still short enough that a hung child is reported inside a coffee break. It is
    # not a tripwire for slowness: a test that is merely slow under load must never fail, and
    # none of these has ever come within an order of magnitude of this bound. An app that opens
    # a GPU device can also wait for the machine-wide GPU lock, and its test gets the queue on
    # top of the ten minutes (engine_test_waits_for_gpu above).
    set_tests_properties(${EA_NAME} PROPERTIES LABELS "unit;e2e;${EA_NAME}"
      RESOURCE_LOCK "e2e_apps" TIMEOUT ${ENGINE_TEST_WORK_TIMEOUT_S})
    if(_opens_devices)
      engine_test_waits_for_gpu(${EA_NAME})
    endif()
  endif()
endfunction()
