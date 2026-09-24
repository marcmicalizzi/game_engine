# engine-content: the content build on the command line (docs/plan/07-content-pipeline.md §7.3).
# Imports a glTF file, validates it, clusters it per material on the job system's performance
# pool, and writes the .clusters container the renderer loads; `build-all` does that for a
# manifest of meshes, and `info` and `stats` read one back.
engine_app(NAME engine_content OUTPUT engine-content
  SOURCES main.cpp
  DEPS base containers math hash time log platform jobs io json geometry assets atlas
  E2E_TESTS tests/content_tests.cpp)
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
