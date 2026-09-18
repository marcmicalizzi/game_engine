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
