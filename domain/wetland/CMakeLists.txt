# Optional wetland plant meshes and patch placements (docs/subsystems/wetland.md).
engine_capability_requires(wetland flora)
engine_schema_library(NAME wetland_schemas SCHEMAS schemas/wetland.schema CAPABILITY wetland)
engine_module(NAME wetland LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash json schema assets geometry materials texture flora scene_gen image wetland_schemas
  SOURCES src/wetland.cpp src/container.cpp src/placement.cpp)
engine_module_tests(NAME wetland
  SOURCES tests/wetland_tests.cpp tests/determinism_tests.cpp tests/size_table.cpp)
engine_module_bench(NAME wetland SOURCES bench/wetland_bench.cpp)
