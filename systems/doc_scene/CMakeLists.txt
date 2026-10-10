# doc_scene: a document's placed meshes as a renderer scene (docs/subsystems/doc_scene.md). The
# bridge sits above both sides because neither may know the other: the renderer reads scenes and
# not documents (schemas/scene.schema, the header), and the document model knows nothing of
# drawing. Every host that draws a document — engine-editor's viewport now, engine-host's
# `render.load` of a session next — reads it through here. Not a capability: it is the engine's
# one mapping from `engine.scene.MeshPlacement` records to instances, and has no switch.
engine_module(NAME doc_scene LAYER systems
  DEPS base containers ids json math schema schemas doc renderer
  SOURCES src/doc_scene.cpp)

# CPU only: a document built in memory, read into a scene description, and changed.
engine_module_tests(NAME doc_scene SOURCES tests/doc_scene_tests.cpp)
