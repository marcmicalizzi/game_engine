cmake_minimum_required(VERSION 3.28)

project(GameEngine
  VERSION 0.0.1
  DESCRIPTION "Agent-native high-performance game engine"
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# No C++20 modules yet; skipping the dependency scan saves a pass per translation unit.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

include(EngineOptions)
include(EngineWarnings)
include(EngineModule)
include(EngineTesting)
include(EngineSchema)

# Build tools first: schemac generates sources for schema modules below.
add_subdirectory(tools/schemac)

# Layers, lowest first. engine_module() enforces that a module only depends on
# modules that were declared before it and live in the same or a lower layer.
add_subdirectory(core)
add_subdirectory(schemas)
# add_subdirectory(foundation)
add_subdirectory(domain)
# add_subdirectory(systems)
# add_subdirectory(apps)
# add_subdirectory(game)

engine_finalize_modules()
