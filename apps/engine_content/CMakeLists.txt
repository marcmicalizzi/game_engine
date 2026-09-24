# engine-content: the content build on the command line (docs/plan/07-content-pipeline.md §7.3).
# Imports a glTF file, validates it, clusters it per material on the job system's performance
# pool, and writes the .clusters container the renderer loads; `build-all` does that for a
# manifest of meshes, and `info` and `stats` read one back. `limit-dump` is the limit-surface
# conformance exchange with the authoring side, and `tissue` reads, validates and reports tissue
# definitions (docs/subsystems/tissue.md).
#
# `tissue` needs the tissue capability, which needs physics, and both are optional (ADR-0027). An
# app is not a capability and cannot declare `engine_capability_requires`, so it does what
# engine-view does for `--animate`: link the module when this configuration has it, define
# ENGINE_CONTENT_TISSUE so the commands compile, and otherwise build an engine-content that refuses
# `tissue` with a sentence. `msvc-minimal` is the proof that the second half works.
set(_content_deps base containers math hash time log platform jobs io json geometry assets atlas)
set(_content_tests tests/content_tests.cpp tests/determinism_tests.cpp tests/limit_dump_tests.cpp)
if(TARGET engine::tissue)
  list(APPEND _content_deps tissue schemas)
  list(APPEND _content_tests tests/tissue_tests.cpp)
endif()

engine_app(NAME engine_content OUTPUT engine-content
  SOURCES main.cpp tissue_commands.cpp limit_dump.cpp
  DEPS ${_content_deps}
  E2E_TESTS ${_content_tests})

if(TARGET engine::tissue)
  target_compile_definitions(engine_engine_content PRIVATE ENGINE_CONTENT_TISSUE=1)
  if(TARGET engine_engine_content_tests)
    target_compile_definitions(engine_engine_content_tests PRIVATE ENGINE_CONTENT_TISSUE=1)
  endif()
endif()
# The end-to-end test writes its own GLB fixture, with an embedded PNG that foundation/image
# encodes, so nothing binary lives in the tree.
if(TARGET engine_engine_content_tests)
  target_link_libraries(engine_engine_content_tests PRIVATE engine::image)
endif()
# engine-view's mesh end-to-end test drives this app, which sits beside engine-view in bin/, to
# write the container it then renders.
if(TARGET engine_engine_view_tests)
  add_dependencies(engine_engine_view_tests engine_engine_content)
endif()
