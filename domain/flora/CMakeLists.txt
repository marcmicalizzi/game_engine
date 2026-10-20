# flora capability (ADR-0027; docs/subsystems/flora.md): the deterministic tree solver — a species'
# parameters and a seed in, a mesh out — whose first species is the date palm.
#
# OPTIONAL creates ENGINE_WITH_FLORA (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its schema types, and its tests from the build, and the
# rest of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — its schema types — from being
# dropped by the linker because nothing references them.
#
# Pure CPU arithmetic over core modules and the mesh types it emits: `assets` for the mesh
# (`assets::MeshData`), `geometry` for the cluster build its container is made by, `materials` for
# the tiled sets its parts sample and `texture` for the records that say how their images are built
# (flora.md, "Materials"). No GPU, no ECS, no io: the callers read the file and the material pack
# and hand over the text and the pack.

# The capability's own types (ADR-0007): a species' parameters and the flora file. CAPABILITY ties
# them to the same switch, so they leave the build with it.
engine_schema_library(NAME flora_schemas SCHEMAS schemas/flora.schema CAPABILITY flora)

engine_module(NAME flora LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash json schema assets geometry materials texture flora_schemas
  SOURCES src/palm.cpp src/motion.cpp src/kit.cpp src/broadleaf.cpp src/conifer.cpp src/flora.cpp
          src/birch.cpp src/willow.cpp src/orchard.cpp src/ancient.cpp)

engine_module_tests(NAME flora
  SOURCES tests/flora_tests.cpp tests/species_tests.cpp tests/allometry_tests.cpp
          tests/size_table.cpp)
