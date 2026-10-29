# Static forest litter meshes and tile accumulation (ADR-0027).
#
# OPTIONAL creates ENGINE_WITH_LITTER (ON by default): switching it off, or configuring with
# ENGINE_MINIMAL=ON, removes this module, its tests, and its bench from the build, and the rest
# of the tree still builds and passes. That is the proof that the capability is additive.
# WHOLE_ARCHIVE keeps the module's static registration objects — the scheduler entry, tunables,
# schema types — from being dropped by the linker because nothing references them.

# The capability graph (docs/plan/08-toolchain.md section 8.5). `deadwood` is itself an optional capability and this one links it,
# so the edge is declared: with that capability switched off, this capability is switched off too —
# with a line on the configure's status output and a row in modules.json's
# disabled_capabilities — instead of failing a configure that is perfectly legitimate.
engine_capability_requires(litter deadwood)

# The capability's own component and event types (ADR-0007). They live here, not in the shared
# schemas/ directory, so that adding them touches no file this capability does not own;
# CAPABILITY ties them to the same switch, so they leave the build with it.
engine_schema_library(NAME litter_schemas SCHEMAS schemas/litter.schema CAPABILITY litter)

engine_module(NAME litter LAYER domain OPTIONAL WHOLE_ARCHIVE
  DEPS base containers math hash json schema assets geometry materials texture scene_gen deadwood litter_schemas
  SOURCES src/litter.cpp src/container.cpp src/placement.cpp)

engine_module_tests(NAME litter
  SOURCES tests/litter_tests.cpp tests/size_table.cpp tests/determinism_tests.cpp)
