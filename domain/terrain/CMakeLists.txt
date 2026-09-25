# terrain capability (ADR-0027, ADR-0043; docs/subsystems/terrain.md): the desert's procedural
# terrain generator — the dune field as a closed-form function of (world seed, tile, game time), the
# seeded wind record that moves it, the bounded deformation overlay the wind fills back in, and the
# saturating per-tile lag that is the only feedback the player's world has on the dunes.
#
# OPTIONAL creates ENGINE_WITH_TERRAIN (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes — the renderer then refuses a scene whose terrain names the
# generator, engine-content refuses `terrain`, and the world streams no terrain, each with a
# sentence. That is the proof that the capability is additive. WHOLE_ARCHIVE keeps the tunables'
# static registration objects from being dropped by the linker because nothing references them.
#
# Depends on core modules and the tunables registry only: pure CPU arithmetic, no GPU, no ECS, no
# store. `jobs` is `evaluate_tiles`'s performance pool.

# The capability's own types (ADR-0007): the overlay's projection kind in the store and the
# `engine-content terrain` report. CAPABILITY ties them to the same switch.
engine_schema_library(NAME terrain_schemas SCHEMAS schemas/terrain.schema CAPABILITY terrain)

engine_module(NAME terrain LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash jobs tunables schema terrain_schemas
  SOURCES src/fixed.cpp src/wind.cpp src/dunes.cpp src/feedback.cpp src/overlay.cpp src/tile.cpp
          src/stats.cpp src/terrain.cpp)

engine_module_tests(NAME terrain
  SOURCES tests/terrain_tests.cpp tests/overlay_tests.cpp tests/size_table.cpp)

# terrain.tile.eval: one 32 m tile at the overlay's 25 cm grid, heights, normals and crests.
# terrain.overlay.decay: a tile's overlay decayed by a game hour, trampled and not.
engine_module_bench(NAME terrain
  SOURCES bench/terrain_bench.cpp)
