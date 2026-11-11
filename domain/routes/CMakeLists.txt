# routes capability (ADR-0027; docs/subsystems/routes.md, roadmap W96): footpaths over a region's
# ground as one plan — a least-cost search on a lattice under a grade cap, Tobler's hiking function
# for its time, branches merging on trodden ground — and what the plan is written as: each tread a
# benched ground edit, a terrain mask region and band footprints, and the wear any placement asks.
#
# OPTIONAL creates ENGINE_WITH_ROUTES (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its schema types, its tests and its bench, and a scene
# naming the generator "routes" is refused with the registry's sentence. WHOLE_ARCHIVE keeps the
# placement generator's registration from being dropped by the linker.

# The capability graph (docs/plan/08-toolchain.md section 8.5): a tread is a ground edit the tile
# set (`tileset`) writes into its heights, and the tests hold the edit the plan writes to what the
# tile set makes of it, so with the tile set switched off this capability is switched off too.
engine_capability_requires(routes tileset)

engine_schema_library(NAME routes_schemas SCHEMAS schemas/routes.schema CAPABILITY routes)

engine_module(NAME routes LAYER domain OPTIONAL WHOLE_ARCHIVE GENERATOR placement:routes
  DEPS base containers math hash json schema schemas log io jobs geometry scene_gen tileset
       routes_schemas
  SOURCES src/routes.cpp src/generator.cpp)

engine_module_tests(NAME routes
  SOURCES tests/routes_tests.cpp tests/size_table.cpp)

# The planner over a stated length of route, and a wear query.
engine_module_bench(NAME routes
  SOURCES bench/routes_bench.cpp)
