# authored capability (ADR-0027; docs/subsystems/authored.md, roadmap W69): the placement generator
# "authored", a scene's own static instances filed by tile, which a streamed world's placements
# consumer streams like any other entry.
#
# OPTIONAL creates ENGINE_WITH_AUTHORED (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module and its tests, and the rest of the tree still builds and
# passes — the scene reader then finds no "authored" generator in the registry and keeps a streamed
# world's instances in the scene's prefix, as it did before the capability. WHOLE_ARCHIVE keeps the
# module's static registration object from being dropped by the linker because nothing references
# it.
#
# Pure CPU: the set is the scene's (`engine.scene.AuthoredSet`, a core schema type the reader
# writes), read with the schema runtime and filed by tile. No GPU, no ECS, no io.

engine_module(NAME authored LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math json schema schemas scene_gen
  SOURCES src/authored.cpp)

engine_module_tests(NAME authored
  SOURCES tests/authored_tests.cpp tests/size_table.cpp)
