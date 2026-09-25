# world capability (ADR-0027; docs/subsystems/world.md): world tile streaming. The tile grid, the
# observer set and the tile ring, and the consumers a tile's activation and deactivation call — the
# tile's projections and snapshot in the persistent store that reconciliation reads.
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

# The capability's own types (ADR-0007): a tile's projection in the store and the world log's line.
# In `engine.world`, the runtime world's namespace, but a file of this capability's own — the
# engine's `schemas/world.schema` already owns the name `world` — so the switch takes them out too.
engine_schema_library(NAME world_schemas SCHEMAS schemas/world_tiles.schema CAPABILITY world)

engine_module(NAME world LAYER systems OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash log time ids jobs tunables json io schema schemas sim store
       world_schemas
  SOURCES src/tile_ring.cpp src/world.cpp src/world_log.cpp src/tile_store.cpp)

engine_module_tests(NAME world SOURCES tests/world_tests.cpp tests/tile_store_tests.cpp
                    tests/size_table.cpp)

# The ring's update over a moving observer at the default rings, a first fill, and one tile's round
# trip through the store; the ruins consumer's activation is measured where it runs, in E35's
# flythrough (docs/experiments/e35-world-streaming.md).
engine_module_bench(NAME world
  SOURCES bench/world_bench.cpp)