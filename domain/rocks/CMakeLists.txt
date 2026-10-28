# rocks capability (ADR-0027; docs/subsystems/rocks.md, roadmap G62): the deterministic rock
# solver — a shape's parameters and a seed in, a closed mesh out — for boulders, outcrops and scree.
#
# OPTIONAL creates ENGINE_WITH_ROCKS (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its schema types, and its tests from the build, and the
# rest of the tree still builds and passes — a scene's `.rock.json` mesh is then refused with the
# mesh-generator registry's sentence. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — its schema types and the mesh
# generator "rock" — from being dropped by the linker because nothing references them.
#
# Pure CPU arithmetic over core modules and the mesh types it emits: `assets` for the mesh
# (`assets::MeshData`) and the generator registry, `geometry` for the cluster build its container
# is made by. No GPU, no ECS, no io: the callers read the file and hand over its text.

# The capability's own types (ADR-0007): the shapes' parameters and the rock file. CAPABILITY ties
# them to the same switch, so they leave the build with it.
engine_schema_library(NAME rocks_schemas SCHEMAS schemas/rocks.schema CAPABILITY rocks)

engine_module(NAME rocks LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash json schema log io assets geometry materials texture scene_gen
       rocks_schemas
  SOURCES src/solver.cpp src/cliff.cpp src/rocks.cpp src/cliffs_generator.cpp src/geology.cpp)

engine_module_tests(NAME rocks
  SOURCES tests/rocks_tests.cpp tests/cliff_tests.cpp tests/cliffs_generator_tests.cpp
          tests/size_table.cpp tests/geology_tests.cpp)

# The committed recipes in content/rocks/ are pinned by hash; the test finds them through
# test_paths.h, by this path or in a test bundle.
if(TARGET engine_rocks_tests)
  target_compile_definitions(engine_rocks_tests PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
endif()
