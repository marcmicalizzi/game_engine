# assets: source-asset import (docs/plan/07-content-pipeline.md section 7.2). glTF 2.0 files
# become a plain merged mesh that domain/geometry's cluster builders consume. No GPU code.
engine_module(NAME assets LAYER domain
  DEPS base containers math hash io geometry
  EXTERNAL_DEPS cgltf
  SOURCES src/gltf.cpp)

engine_module_tests(NAME assets
  SOURCES tests/gltf_tests.cpp tests/skin_tests.cpp tests/uv_repair_tests.cpp tests/size_table.cpp)
