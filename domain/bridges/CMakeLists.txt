# bridges capability (ADR-0027): footbridges where a route fords a stream — stepping stones, a
# clapper bridge, a foot log or log stringers, chosen by the crossing's section and solved in whole
# millimetres (docs/subsystems/bridges.md) — as a placement generator whose tiles hand each bridge
# as its own mesh, and the mesh generator that solves one bridge from its recipe.
#
# OPTIONAL creates ENGINE_WITH_BRIDGES (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the two registrars (the placement generator and the mesh generator) from
# being dropped by the linker because nothing references them.
#
# Layer: domain, beside the other generators (walls, rocks, routes): it reads its crossings from its
# own layer (the command writes them from a route's plan) and needs no other capability, so no
# `engine_capability_requires`: a scene whose routes are off simply has no crossings to read.

# The capability's own types (ADR-0007): the crossing, the layer and the recipe.
engine_schema_library(NAME bridges_schemas SCHEMAS schemas/bridges.schema CAPABILITY bridges)

engine_module(NAME bridges LAYER domain OPTIONAL WHOLE_ARCHIVE GENERATOR placement:bridges mesh:bridge
  DEPS base containers math hash json schema log io assets geometry texture materials scene_gen
       bridges_schemas
  SOURCES src/plan.cpp src/mesh.cpp src/placement.cpp)

# The planned-against-pasted ground case builds two tile sets, so it is compiled where the tile set
# is: a test-only dependency, which a build with `ENGINE_WITH_TILESET=OFF` simply leaves out.
set(_bridges_test_sources tests/bridges_tests.cpp tests/size_table.cpp)
set(_bridges_test_deps "")
if(TARGET engine_tileset)
  list(APPEND _bridges_test_sources tests/ground_plan_tests.cpp)
  list(APPEND _bridges_test_deps tileset)
endif()
engine_module_tests(NAME bridges
  SOURCES ${_bridges_test_sources} DEPS ${_bridges_test_deps})

engine_module_bench(NAME bridges
  SOURCES bench/bridges_bench.cpp)
