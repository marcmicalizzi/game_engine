# world capability (ADR-0027; docs/subsystems/world.md): world tile streaming. The tile grid, the
# observer set and the tile ring, and the consumers a tile's activation and deactivation call — a
# tile's ruins as instances the renderer adds and removes between frames, the tile of a partitioned
# document through the materialization driver, and the tile's projections and snapshot in the
# persistent store that reconciliation reads.
#
# OPTIONAL creates ENGINE_WITH_WORLD (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — the tunables and the schema types —
# from being dropped by the linker because nothing references them.

# The capability graph (docs/plan/08-toolchain.md section 8.5). The store is part of what a tile
# is: reconciliation reads the tile's projections from it and deactivation writes them (05 §5.5),
# so a build without `foundation/store` has no tile streaming either, and says so on the configure's
# output instead of failing it.
engine_capability_requires(world store)

# The capability's own types (ADR-0007): a tile's projection in the store and the world log's line
# (world_tiles.schema), and a save game's manifest (world_save.schema). In `engine.world`, the
# runtime world's namespace, but files of this capability's own — the engine's `schemas/world.schema`
# already owns the name `world` — so the switch takes them out too.
engine_schema_library(NAME world_schemas
  SCHEMAS schemas/world_tiles.schema schemas/world_save.schema
  CAPABILITY world)

# `input` for the player of a headless world, an observer an input log steers (input_observer.h).
# `scene_gen` for the placements consumer (placement_tiles.h): a tile's placements are what the
# scene's placement generators — the ruins, the city — put on it, found in the scene-generator
# registry by the names the scene gave them (ADR-0046), so the world links no generator and a world
# without one (a game with no desert) streams its document and its store as it always did.
set(_world_deps base containers math hash log time ids jobs tunables json io input schema schemas
                sim doc renderer store world_schemas scene_gen)
set(_world_sources src/tile_ring.cpp src/world.cpp src/world_log.cpp src/tile_store.cpp
                   src/state_hash.cpp src/save_game.cpp src/input_observer.cpp
                   src/store_tiles.cpp src/document_tiles.cpp src/placement_tiles.cpp)

# The terrain consumer, when this configuration has the terrain capability (ADR-0043): a tile of the
# dune generator's field for every held tile, and the tile's deformation overlay kept in the store.
# The same pattern as the ruins: a world without a desert streams its document and its store and
# links nothing of the generator.
if(TARGET engine::terrain)
  list(APPEND _world_deps terrain)
  list(APPEND _world_sources src/terrain_tiles.cpp)
endif()

engine_module(NAME world LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS ${_world_deps}
  SOURCES ${_world_sources})
# `TARGET engine_world` as well: a configuration with the terrain but without the store (the graph
# skips this module, above) has no target to define anything on.
if(TARGET engine_world AND TARGET engine::terrain)
  # For whoever links the world (the module is an INTERFACE target wrapping `engine_world_impl`,
  # WHOLE_ARCHIVE): a host includes <systems/world/terrain_tiles.h> only where it exists.
  target_compile_definitions(engine_world INTERFACE ENGINE_WORLD_TERRAIN=1)
endif()

set(_world_tests tests/world_tests.cpp tests/tile_store_tests.cpp tests/consumer_tests.cpp
                 tests/save_tests.cpp
                 tests/size_table.cpp)
if(TARGET engine::ruins)
  # The placements consumer over a streamed ruins entry: the ruins reach it through the registry,
  # so the test links the capability as a host does.
  list(APPEND _world_tests tests/placement_tiles_tests.cpp)
endif()
if(TARGET engine::terrain)
  list(APPEND _world_tests tests/terrain_tiles_tests.cpp)
endif()
if(TARGET engine::ruins AND TARGET engine::terrain)
  list(APPEND _world_tests tests/ruins_on_dunes_tests.cpp)
endif()
engine_module_tests(NAME world SOURCES ${_world_tests})
foreach(_gen ruins)
  if(TARGET engine_world_tests AND TARGET engine::${_gen})
    target_link_libraries(engine_world_tests PRIVATE engine::${_gen})
  endif()
endforeach()

# The ring's update over a moving observer at the default rings, a first fill, and one tile's round
# trip through the store; the ruins consumer's activation is measured where it runs, in E35's
# flythrough (docs/experiments/e35-world-streaming.md).
engine_module_bench(NAME world
  SOURCES bench/world_bench.cpp)
