# scene_gen: the scene-generator registry (ADR-0046; docs/subsystems/scene_gen.md). The two kinds a
# generator is — a ground provider and a placement generator — and the table the scene reader
# (systems/renderer) and the world ring (systems/world) look them up in by the name a scene gives.
#
# Not OPTIONAL: it is the registration point every generator attaches through, below every module
# that reads a scene or streams a tile, and a build with no generator at all still has the renderer's
# own ground (the waves) registered in it. Nothing here names a generator; a capability registers
# its descriptor from its own source with a `scene_gen::Registrar`, and is `WHOLE_ARCHIVE` so the
# linker keeps the registrar.
#
# `schemas` for the scene's entries as the reader parsed them (`engine.scene.Terrain`,
# `engine.scene.WorldRings`), `json` for a placement entry's parameters, `jobs` for the pool a
# generator is offered, and `geometry` for the rings: a ground's ring chunk is a cluster LOD DAG the
# renderer uploads, so the rings' table names `geometry::ClusterLodMesh`.
engine_module(NAME scene_gen LAYER domain
  DEPS base containers math hash json schema schemas jobs log geometry
  SOURCES src/registry.cpp)

engine_module_tests(NAME scene_gen
  SOURCES tests/scene_gen_tests.cpp tests/size_table.cpp)
