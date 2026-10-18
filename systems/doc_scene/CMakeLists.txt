# doc_scene: a document's placed meshes as a renderer scene (docs/subsystems/doc_scene.md). The
# bridge sits above both sides because neither may know the other: the renderer reads scenes and
# not documents (schemas/scene.schema, the header), and the document model knows nothing of
# drawing. Every host that draws a document — engine-editor's viewport and engine-host's
# `render.load` of a session — reads it through here. Not a capability: it is the engine's
# one mapping from `engine.scene.MeshPlacement` records to instances, and from an
# `engine.terrain.Heightfield` record to the scene's terrain, and has no switch. The authored ground
# reaches the tile-set capability only through the scene-generator registry (`scene_gen`): the
# record names the provider "tiles", and a build without it refuses the record with a sentence.
engine_module(NAME doc_scene LAYER systems
  DEPS base containers ids json math schema schemas doc renderer scene_gen io
  SOURCES src/doc_scene.cpp)

# A document built in memory, read into a scene description, and changed (CPU only). With the
# tile-set and terrain capabilities, the authored ground's pixel test too: the renderer handed a
# built tile set draws its heightfield pixel for pixel (offscreen; the device takes the GPU lock).
# The capabilities reach the renderer through the registry, so the test links them as a host does.
set(_doc_scene_tests tests/doc_scene_tests.cpp)
set(_doc_scene_test_deps "")
if(TARGET engine::tileset)
  # The heightfield record read, mapped, changed and refused, its set built through the registry.
  list(APPEND _doc_scene_tests tests/heightfield_record_tests.cpp)
  list(APPEND _doc_scene_test_deps tileset)
endif()
if(TARGET engine::tileset AND TARGET engine::terrain)
  list(APPEND _doc_scene_tests tests/authored_ground_gpu_tests.cpp)
  list(APPEND _doc_scene_test_deps tileset terrain gfx scene_gen jobs)
endif()
engine_module_tests(NAME doc_scene SOURCES ${_doc_scene_tests} DEPS ${_doc_scene_test_deps})
