# village capability (ADR-0027; docs/subsystems/village.md, roadmap G69): a row-plan village laid
# out along lanes on the scene's ground — plots of whole perches, a house on each plot the ground
# allows, on a pad benched into the ground and facing the lane — as one plan, and what the plan is
# handed over as: ground edits, the shapes the cover keeps off, and a placement a house.
#
# OPTIONAL creates ENGINE_WITH_VILLAGE (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its schema types, its tests and its bench, and a scene
# naming the generator "village" is refused with the registry's sentence. WHOLE_ARCHIVE keeps the
# placement generator's registration from being dropped by the linker.

# The capability graph (docs/plan/08-toolchain.md section 8.5): a pad and a tread are ground edits
# the tile set (`tileset`) writes into its heights, and the tests hold the edits the plan writes to
# what the tile set makes of them, so with the tile set switched off this capability is off too.
engine_capability_requires(village tileset)

engine_schema_library(NAME village_schemas SCHEMAS schemas/village.schema CAPABILITY village)

engine_module(NAME village LAYER domain OPTIONAL WHOLE_ARCHIVE GENERATOR placement:village
  DEPS base containers math hash json schema schemas log io scene_gen tileset village_schemas
  SOURCES src/village.cpp src/generator.cpp src/houses.cpp src/flight.cpp)

engine_module_tests(NAME village
  SOURCES tests/village_tests.cpp tests/size_table.cpp)

# The plan of a street of a stated number of houses.
engine_module_bench(NAME village
  SOURCES bench/village_bench.cpp)
