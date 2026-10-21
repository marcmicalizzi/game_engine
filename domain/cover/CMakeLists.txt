# cover capability (ADR-0027; docs/subsystems/cover.md): what grows over another mesh — climbing
# plants (G76) and organic cover, moss, lichen, algae and fungi (G78) — on one host-surface
# foundation, each a companion mesh placed with its host.
#
# OPTIONAL creates ENGINE_WITH_COVER (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its schema types and its tests from the build, and the
# rest of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — its two mesh generators and its
# schema types — from being dropped by the linker because nothing references them.
#
# Pure CPU arithmetic: `assets` for the host seam and the mesh, `geometry` for the container,
# `flora` for the kit's shapes (flora/companion.h) and the motion contract, `materials` and
# `texture` for the sets its parts sample. No GPU, no ECS, no io: the host is read by whoever
# hosts the generator (content_build's GeneratorHosts) and handed over.

# The capability graph (docs/plan/08-toolchain.md section 8.5): `flora` is itself an optional
# capability and this one links it, so the edge is declared.
engine_capability_requires(cover flora)

engine_schema_library(NAME cover_schemas SCHEMAS schemas/cover.schema CAPABILITY cover)

engine_module(NAME cover LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash json schema assets geometry materials texture flora cover_schemas
  SOURCES src/host.cpp src/vine.cpp src/growth.cpp src/cover.cpp)

engine_module_tests(NAME cover
  SOURCES tests/cover_tests.cpp tests/size_table.cpp)
