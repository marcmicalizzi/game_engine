# walls capability (ADR-0027): dry-stone walls along a line over the ground, as a placement
# generator that places a small kit of solved wall modules by station (docs/subsystems/walls.md),
# and the mesh generator that solves one module from its recipe.
#
# OPTIONAL creates ENGINE_WITH_WALLS (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the two registrars (the placement generator and the mesh generator) from
# being dropped by the linker because nothing references them.
#
# Layer: domain, beside the other generators (ruins, rocks, litter; ADR-0060): it reads the
# ground through scene_gen's view and writes meshes through assets' generator registry, and
# needs nothing from systems/.

# The capability's own types (ADR-0007): the module recipe and the layer of lines. They live
# here, not in the shared schemas/ directory, so adding them touches no file this capability does
# not own; CAPABILITY ties them to the same switch.
engine_schema_library(NAME walls_schemas SCHEMAS schemas/walls.schema CAPABILITY walls)

engine_module(NAME walls LAYER domain OPTIONAL WHOLE_ARCHIVE GENERATOR placement:walls mesh:wall
  DEPS base containers math hash json schema log io image assets geometry texture scene_gen
       walls_schemas
  SOURCES src/stations.cpp src/module.cpp src/container.cpp src/placement.cpp)

engine_module_tests(NAME walls
  SOURCES tests/stations_tests.cpp tests/module_tests.cpp tests/placement_tests.cpp
          tests/determinism_tests.cpp tests/size_table.cpp)

engine_module_bench(NAME walls
  SOURCES bench/walls_bench.cpp)
