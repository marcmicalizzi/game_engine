# caves capability (ADR-0027; docs/subsystems/caves.md, roadmap G134): the cave passage solver — a
# recipe and a seed in, a mesh out — a shell from a hole in the ground in and down, wound to face
# inward, and the speleothems in it, built through the mesh-generator registry ("cave") into a
# `.clusters` container like a rock.
#
# OPTIONAL creates ENGINE_WITH_CAVES (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its schema types and its tests from the build, and the
# rest of the tree still builds and passes — a scene's `.cave.json` mesh is then refused with the
# mesh-generator registry's sentence. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — its schema types and the mesh
# generator "cave" — from being dropped by the linker because nothing references them.
#
# Pure CPU arithmetic: `assets` for the mesh and the generator registry, `geometry` for the cluster
# build, `materials` and `texture` for a pack's limestone. No GPU, no ECS.
engine_schema_library(NAME caves_schemas SCHEMAS schemas/caves.schema CAPABILITY caves)

engine_module(NAME caves LAYER domain OPTIONAL WHOLE_ARCHIVE GENERATOR mesh:cave
  DEPS base containers math hash json schema log io assets geometry materials texture caves_schemas
  SOURCES src/caves.cpp src/passage.cpp)

engine_module_tests(NAME caves
  SOURCES tests/caves_tests.cpp tests/size_table.cpp)
