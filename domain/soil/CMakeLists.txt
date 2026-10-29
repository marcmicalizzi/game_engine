# Static soil morphology through existing mesh and authored-ground seams (ADR-0027).
engine_capability_requires(soil rocks tileset)
engine_schema_library(NAME soil_schemas SCHEMAS schemas/soil.schema CAPABILITY soil)
engine_module(NAME soil LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash json schema assets geometry materials texture scene_gen io rocks tileset soil_schemas
  SOURCES src/soil.cpp src/solver.cpp src/cook.cpp)
engine_module_tests(NAME soil
  SOURCES tests/soil_tests.cpp tests/size_table.cpp tests/determinism_tests.cpp)
