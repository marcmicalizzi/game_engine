# city capability (ADR-0027; docs/subsystems/city.md): Island City's whole-island plan — coastline,
# districts, street graph, blocks, lots, parks and civic reservations — and the hierarchical building
# grammar that turns one lot into a building description with its occupancy summary, the
# validators whose pass rate is E18's yield, and the proxy scene fragment the owner's fly-through
# draws (plan 13 §13.2, plan 07 §7.6).
#
# OPTIONAL creates ENGINE_WITH_CITY (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its schema library, its tests, and its bench from the
# build, and the rest of the tree still builds and passes — engine-content then refuses `city`
# with a sentence. That is the proof that the capability is additive.
#
# No WHOLE_ARCHIVE: the module registers nothing at static-initialization time of its own (its
# schema types register from `city_schemas`, which every caller links through this module), and
# nothing ticks.

# The capability's own types (ADR-0007): the island's parameters and style tables, the plan file
# and the building description. Here and not in the shared schemas/ directory, so adding them
# touches no file this capability does not own; CAPABILITY ties them to the same switch.
engine_schema_library(NAME city_schemas SCHEMAS schemas/city.schema CAPABILITY city)

# `schemas` is the proxy fragment's (an `engine.scene.Scene`, schemas/scene.schema); `jobs` the
# performance pool that generates many buildings at once; `io` the plan and fragment files.
engine_module(NAME city LAYER domain OPTIONAL
  DEPS base containers math hash json schema schemas io log jobs city_schemas
  SOURCES src/params.cpp src/plan.cpp src/plan_io.cpp src/building.cpp src/walls.cpp
          src/validate.cpp src/fragment.cpp)

engine_module_tests(NAME city
  SOURCES tests/plan_tests.cpp tests/building_tests.cpp tests/validate_tests.cpp
          tests/size_table.cpp)

# The plan, one building, and the validators over one building (plan 11 §11.8).
engine_module_bench(NAME city
  SOURCES bench/city_bench.cpp)
