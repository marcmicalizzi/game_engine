# matgen capability (ADR-0027): tiled PBR sets by role, generated from a recipe of whole numbers
# (docs/subsystems/matgen.md): a gravel bed, a sand bar, scree, rock faces and ground floors as
# the four images a material pack's set holds, the same bytes from every toolchain.
#
# OPTIONAL creates ENGINE_WITH_MATGEN (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# It registers nothing (no WHOLE_ARCHIVE): `engine-content matgen` calls it, and a pack directory
# of its sets is read by `materials::pack_at` as any pack is.
#
# `texture` is here for the sRGB transfer function's committed tables (and, in the tests, the mip
# chain a set is held through), so a set's colour bytes are the texture build's own rounding.

# The capability's own types (ADR-0007): the recipe. They live here, not in the shared schemas/
# directory, so adding them touches no file this capability does not own; CAPABILITY ties them to
# the same switch.
engine_schema_library(NAME matgen_schemas SCHEMAS schemas/matgen.schema CAPABILITY matgen)

engine_module(NAME matgen LAYER domain OPTIONAL
  DEPS base containers math hash json schema log texture matgen_schemas
  SOURCES src/matgen.cpp src/granular.cpp src/rock.cpp src/ground.cpp)

engine_module_tests(NAME matgen
  SOURCES tests/matgen_tests.cpp tests/size_table.cpp)

engine_module_bench(NAME matgen
  SOURCES bench/matgen_bench.cpp)
