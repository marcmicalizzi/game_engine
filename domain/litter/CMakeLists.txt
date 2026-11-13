# Static forest litter meshes and tile accumulation (ADR-0027).
#
# OPTIONAL creates ENGINE_WITH_LITTER (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — the scheduler entry, tunables,
# schema types — from being dropped by the linker because nothing references them.

# The capability graph (docs/plan/08-toolchain.md section 8.5). `deadwood` is itself an optional capability and this one links it,
# so the edge is declared: with that capability switched off, this capability is switched off too —
# with a line on the configure's status output and a row in modules.json's
# disabled_capabilities — instead of failing a configure that is perfectly legitimate.
engine_capability_requires(litter deadwood)

# The capability's own component and event types (ADR-0007). They live here, not in the shared
# schemas/ directory, so that adding them touches no file this capability does not own;
# CAPABILITY ties them to the same switch, so they leave the build with it.
engine_schema_library(NAME litter_schemas SCHEMAS schemas/litter.schema CAPABILITY litter)

engine_module(NAME litter LAYER domain OPTIONAL WHOLE_ARCHIVE GENERATOR placement:litter mesh:litter
  DEPS base containers math hash json schema assets geometry materials texture scene_gen deadwood litter_schemas
       jobs profiling tunables
  SOURCES src/litter.cpp src/container.cpp src/placement.cpp)

engine_module_tests(NAME litter
  SOURCES tests/litter_tests.cpp tests/litter_exclusion_tests.cpp tests/size_table.cpp tests/determinism_tests.cpp
          tests/litter_pool_tests.cpp)

# A whole read on a cooked "tiles" ground with its masks (T125): the mask level's key is its
# content, so two cooks of the same masks in two directories are one entry, and the entry's tiles
# on the pool are the one thread's. Only where the tile-set capability is in the build, as the water's
# and the collision's tests link it.
if(TARGET engine_litter_tests AND TARGET engine::tileset)
  target_sources(engine_litter_tests PRIVATE tests/litter_masks_tests.cpp)
  target_link_libraries(engine_litter_tests PRIVATE engine::tileset)
endif()
# What a cold entry costs and what of it scales with the region: the mask cook over a frame one and
# three kilometres wide, and the entry's whole read on one thread and on the pool (litter.md).
if(TARGET engine::tileset)
  engine_module_bench(NAME litter SOURCES bench/litter_bench.cpp DEPS tileset jobs)
endif()
