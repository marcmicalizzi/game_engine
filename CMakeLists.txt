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
# Before every include() and add_subdirectory() below: the CPU baseline is a directory property
# and a target inherits it when it is created, so anything fetched or added earlier would be
# compiled for a different machine than the rest of the tree (ADR-0031).
include(EngineCpuBaseline)
# Also before everything: -ffp-contract=off for every target, so GCC and Clang compute what the
# source says, as MSVC does, and the content build's bytes do not depend on the compiler (ADR-0035).
include(EngineFpContraction)
include(EngineWarnings)
include(EngineModule)
# The commit every app was built from (`--version`), refreshed on every build: engine_app links it.
include(EngineBuildStamp)
include(EngineTesting)
include(EngineSchema)
include(EngineProfiling)
include(EngineGraphics)
include(EngineImage)
include(EngineAssets)
include(EngineAtlas)
include(EngineShaders)
include(EngineBench)
# Capability dependencies: each returns immediately when its ENGINE_WITH_<NAME> switch is off,
# so a minimal build (ADR-0027) fetches nothing for a capability it does not contain.
include(EngineEcs)
include(EngineStore)

# Build tools first: schemac generates sources for schema modules below.
add_subdirectory(tools/schemac)

# Layers, lowest first. engine_module() enforces that a module only depends on
# modules that were declared before it and live in the same or a lower layer.
add_subdirectory(core)
add_subdirectory(schemas)
add_subdirectory(foundation)
add_subdirectory(domain)
add_subdirectory(systems)
add_subdirectory(apps)
# add_subdirectory(game)

engine_write_shader_manifest()
engine_finalize_modules()
# Last, when every target that is a host tool or the CPU floor has said so: build.cpu_baseline
# checks the build against that classification (ADR-0031, ADR-0034).
engine_cpu_baseline_finalize()
