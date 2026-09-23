# assets: source-asset import (docs/plan/07-content-pipeline.md section 7.2). glTF 2.0 files
# become a plain merged mesh that domain/geometry's cluster builders consume. No GPU code.
engine_module(NAME assets LAYER domain
  DEPS base containers math hash io geometry
  EXTERNAL_DEPS cgltf
  SOURCES src/gltf.cpp)

engine_module_tests(NAME assets
  SOURCES tests/gltf_tests.cpp tests/skin_tests.cpp tests/uv_repair_tests.cpp tests/weld_tests.cpp
          tests/size_table.cpp)
# ENGINE_SOURCE_DIR is where the weld test looks for the Khronos morph samples
# tools/fetch-samples.ps1 fetches into content/samples (through tests/support/test_paths.h, so a
# bundle made with -WithSamples finds its own copy); without them that case says so and passes.
target_compile_definitions(engine_assets_tests PRIVATE ENGINE_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
