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
# `scene_gen` for the generator consumers (ADR-0046): the placements consumer (placement_tiles.h)
# holds what the scene's placement generators — the ruins, the city — put on a tile, and the ground
# consumer (ground_tiles.h) a tile of the scene's ground with its record in the store — the dunes'
# tile and overlay — each generator found in the scene-generator registry by the name the scene gave
# it. So the world links no generator, names none, and a world without one (a game with no desert)
# streams its document and its store as it always did.
set(_world_deps base containers math hash log time ids jobs tunables json io input schema schemas
                sim doc renderer store world_schemas scene_gen)
set(_world_sources src/tile_ring.cpp src/world.cpp src/world_log.cpp src/tile_store.cpp
                   src/state_hash.cpp src/save_game.cpp src/input_observer.cpp
                   src/store_tiles.cpp src/document_tiles.cpp src/placement_tiles.cpp
                   src/ground_tiles.cpp src/scene_consumers.cpp)

engine_module(NAME world LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS ${_world_deps}
  SOURCES ${_world_sources})

# scene_order_tests.cpp registers a ground and a placement generator of its own, so it needs no
# capability and runs in every configuration that has the world.
set(_world_tests tests/world_tests.cpp tests/tile_store_tests.cpp tests/consumer_tests.cpp
                 tests/save_tests.cpp tests/scene_order_tests.cpp
                 tests/size_table.cpp)
if(TARGET engine::ruins)
  # The placements consumer over a streamed ruins entry: the ruins reach it through the registry,
  # so the test links the capability as a host does.
  list(APPEND _world_tests tests/placement_tiles_tests.cpp)
endif()
if(TARGET engine::terrain)
  # The ground consumer over the dunes, which reach it through the registry: linked as a host would.
  list(APPEND _world_tests tests/ground_tiles_tests.cpp)
endif()
if(TARGET engine::ruins AND TARGET engine::terrain)
  list(APPEND _world_tests tests/ruins_on_dunes_tests.cpp)
endif()
if(TARGET engine::city)
  # The placements consumer over a streamed city: its per-tile query, reached through the registry.
  list(APPEND _world_tests tests/city_tiles_tests.cpp)
endif()
engine_module_tests(NAME world SOURCES ${_world_tests})
foreach(_gen city ruins terrain)
  if(TARGET engine_world_tests AND TARGET engine::${_gen})
    target_link_libraries(engine_world_tests PRIVATE engine::${_gen})
  endif()
endforeach()

# The ring's update over a moving observer at the default rings, a first fill, and one tile's round
# trip through the store; the ruins consumer's activation is measured where it runs, in E35's
# flythrough (docs/experiments/e35-world-streaming.md).
engine_module_bench(NAME world
  SOURCES bench/world_bench.cpp)
