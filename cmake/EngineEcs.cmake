# The ECS capability's dependency (docs/plan/03-data-model.md §3.4, experiment E6): flecs, an
# archetype ECS with first-class relationships, reflection, and a query language. MIT.
#
# Fetched only when the capability is on, so the minimal build (ADR-0027) downloads nothing and
# a configure with ENGINE_WITH_ECS=OFF has no flecs in its cache at all. flecs builds itself
# with its own warning settings (FLECS_STRICT is off by default, so no -Werror of ours applies
# to it); consumers include its headers as SYSTEM, which domain/ecs does.

engine_capability_enabled(ecs _engine_ecs_enabled)
if(NOT _engine_ecs_enabled)
  return()
endif()

include(FetchContent)

set(ENGINE_FLECS_TAG "v4.1.6" CACHE STRING "flecs tag")

FetchContent_Declare(flecs
  GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
  GIT_TAG        ${ENGINE_FLECS_TAG}
  GIT_SHALLOW    TRUE)
set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
set(FLECS_STATIC ON CACHE BOOL "" FORCE)
set(FLECS_TESTS OFF CACHE BOOL "" FORCE)
set(FLECS_PIC ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(flecs)

# flecs' REST addon, and the HTTP server under it, are compiled in by flecs' default addon set and
# `ecs_init()` imports the REST module on every world. Importing it only registers the `EcsRest`
# component and an observer; the socket opens when something *sets* that component, and nothing in
# this tree does. But when it opens, flecs binds `INADDR_ANY` unless the caller names an address
# (src/addons/http/http.c: `if (!srv->ipaddr) addr.sin_addr.s_addr = htonl(INADDR_ANY)`), and a
# listener on a non-loopback address is one Windows Firewall prompt per executable *path* — the
# failure that cost a night on 2026-09-18 and took the machine's GPU with it
# (docs/subsystems/profiling.md). This tree makes paths cheaply: every preset and every agent
# worktree has its own copy of every test, bench and app.
#
# So the addon is compiled out rather than left one `ecs_singleton_set` away, and the switch that
# brings it back is explicit. `FLECS_NO_HTTP` takes the socket code out of the build entirely;
# `FLECS_NO_REST` takes the module that would start it. Both are PUBLIC so the library and every
# consumer's `<flecs.h>` agree on the addon set.
#
# `-DENGINE_WITH_ECS_EXPLORER=ON` restores them, for the flecs explorer over a running world.
# Whoever turns it on binds loopback — `ecs_singleton_set(world, EcsRest, {.ipaddr = "127.0.0.1"})`
# — because the explorer is a development tool for the machine it runs on, and AGENTS.md's
# convention is that anything listening binds loopback unless the user opts in.
option(ENGINE_WITH_ECS_EXPLORER "Compile flecs' REST/HTTP addons in, for the flecs explorer" OFF)
if(NOT ENGINE_WITH_ECS_EXPLORER)
  target_compile_definitions(flecs_static PUBLIC FLECS_NO_HTTP FLECS_NO_REST)
endif()
